#version 430 core
// Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT
// XeGTAO a5b1686: scalar AO port, FP32, GL lower-left coordinates, orthographic support.
// Full MIT notice and adaptation details: ../third_party_licenses.txt.
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D linear_depth;
layout(binding = 1) uniform sampler2D normals;
layout(r8ui, binding = 0) writeonly uniform uimage2D raw_ao;
layout(r8ui, binding = 1) writeonly uniform uimage2D edge_output;
uniform mat4  inverse_projection;
uniform ivec2 full_size;
uniform bool  perspective;
uniform float radius;
uniform int   slices, steps, max_lod;
const float   PI = 3.141592653589793;
float         depth_at(ivec2 p) { return texelFetch(linear_depth, clamp(p, ivec2(0), full_size - 1), 0).r; }
vec3          position(vec2 uv, float z)
{
    // Camera::apply_projection uses standard (including off-axis) projections.
    // Positive linear depth is -view Z; orthographic XY does not depend on Z.
    vec2 xy = (uv * 2.0 - 1.0) * vec2(inverse_projection[0][0], inverse_projection[1][1]) + inverse_projection[3].xy;
    return vec3(perspective ? xy * z : xy, -z);
}
vec3 normal_at(ivec2 p)
{
    vec2 e = texelFetch(normals, p, 0).rg;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0)
        n.xy = (1.0 - abs(n.yx)) * mix(vec2(-1), vec2(1), greaterThanEqual(n.xy, vec2(0)));
    return normalize(n);
}
uint hilbert(uvec2 p)
{
    p &= uvec2(63);
    uint index = 0u;
    for (uint level = 32u; level > 0u; level >>= 1u) {
        uint rx = (p.x & level) > 0u ? 1u : 0u, ry = (p.y & level) > 0u ? 1u : 0u;
        index += level * level * ((3u * rx) ^ ry);
        if (ry == 0u) {
            if (rx == 1u)
                p = uvec2(63) - p;
            p = p.yx;
        }
    }
    return index;
}
float fast_acos(float v)
{
    float x = clamp(abs(v), 0.0, 1.0);
    float a = (-0.156583 * x + PI * 0.5) * sqrt(1.0 - x);
    return v >= 0.0 ? a : PI - a;
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, full_size)))
        return;
    float z = depth_at(p);
    if (z >= 1e19) {
        imageStore(raw_ao, p, uvec4(170));
        imageStore(edge_output, p, uvec4(0));
        return;
    }
    // L/R/T/B here use GL +Y for top; all denoiser paths use this same convention.
    vec4 delta = vec4(depth_at(p + ivec2(-1, 0)), depth_at(p + ivec2(1, 0)), depth_at(p + ivec2(0, 1)), depth_at(p + ivec2(0, -1))) - z;
    vec2 slope = vec2(delta.y - delta.x, delta.w - delta.z) * 0.5;
    vec4 edge  = clamp(1.25 - min(abs(delta), abs(delta + vec4(slope.x, -slope.x, slope.y, -slope.y))) / max(z * 0.011, 1e-6), 0.0, 1.0);
    if (p.x == 0)
        edge.x = 0;
    if (p.x == full_size.x - 1)
        edge.y = 0;
    if (p.y == full_size.y - 1)
        edge.z = 0;
    if (p.y == 0)
        edge.w = 0;
    uvec4 edge_bits = uvec4(round(edge * 2.9));
    imageStore(edge_output, p, uvec4((edge_bits.x << 6u) | (edge_bits.y << 4u) | (edge_bits.z << 2u) | edge_bits.w));
    vec2  uv = (vec2(p) + 0.5) / vec2(full_size);
    vec3  P = position(uv, z * 0.99999), N = normal_at(p);
    vec3  V         = perspective ? normalize(-P) : vec3(0, 0, 1);
    float footprint = max(2.0 * abs(inverse_projection[0][0]) * (perspective ? z : 1.0) / float(full_size.x), 1e-6);
    float r = max(radius * 1.457, 1e-6), screen_radius = r / footprint;
    vec2  noise      = fract(0.5 + float(hilbert(uvec2(p))) * vec2(0.75487766624669276005, 0.56984029099805326591));
    float visibility = clamp((10.0 - screen_radius) / 100.0, 0.0, 1.0) * 0.5;
    // One angular slice has direction-dependent unoccluded energy. Normalize
    // against the SAME slice before the response curve so an unoccluded sloped
    // surface stays white. Multi-slice presets keep their existing estimator.
#ifdef AO_LEGACY_LOW
    bool normalize_low = false;
#else
    bool normalize_low = slices == 1;
#endif
    float unoccluded_visibility = visibility;
    for (int slice = 0; slice < slices; ++slice) {
        float phi       = (float(slice) + noise.x) / float(slices) * PI;
        vec2  direction = vec2(cos(phi), sin(phi));
        vec3  D = vec3(direction, 0), ortho = D - dot(D, V) * V;
        vec3  axis      = normalize(cross(ortho, V));
        vec3  projected = N - axis * dot(N, axis);
        float len       = length(projected);
        float cosN      = clamp(dot(projected, V) / max(len, 1e-6), 0.0, 1.0);
        float n         = sign(dot(ortho, projected)) * fast_acos(cosN);
        vec2  low = cos(vec2(n + PI * 0.5, n - PI * 0.5)), horizon = low;
        for (int step = 0; step < steps; ++step) {
            float jitter = fract(noise.y + float(slice + step * steps) * 0.6180339887498948482);
            float t      = (float(step) + jitter) / float(steps);
            float s      = t * t + 1.3 / max(screen_radius, 1e-6);
            vec2  offset = s * direction * screen_radius;
            float lod    = clamp(log2(max(length(offset), 1e-6)) - 3.30, 0.0, float(max_lod));
            offset       = round(offset) / vec2(full_size);
            for (int side = 0; side < 2; ++side) {
                vec2 sample_uv = uv + offset * (side == 0 ? 1.0 : -1.0);
                if (any(lessThan(sample_uv, vec2(0))) || any(greaterThanEqual(sample_uv, vec2(1))))
                    continue;
                // Low has only four taps. A filtered MIP depth on a sloped plane
                // is not necessarily the depth at this exact UV, creating false occluders.
                float sample_z = textureLod(linear_depth, sample_uv * vec2(full_size) / vec2(textureSize(linear_depth, 0)), normalize_low ? 0.0 : lod).r;
                if (sample_z >= 1e19)
                    continue;
                vec3  diff    = position(sample_uv, sample_z) - P;
                // Pixel snapping may place a tap outside its integration slice.
                // Samples below the receiver tangent are not hemisphere occluders.
                if (dot(N, diff) <= max(footprint * 0.05, 1e-5))
                    continue;
                float dist    = length(diff);
                float weight  = clamp(1.0 / 0.615 - dist / (r * 0.615), 0.0, 1.0);
                horizon[side] = max(horizon[side], mix(low[side], dot(diff / max(dist, 1e-6), V), weight));
            }
        }
        len      = mix(len, 1.0, 0.05);
        float h0 = -fast_acos(horizon.y), h1 = fast_acos(horizon.x);
        visibility += len * ((cosN + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) + (cosN + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n))) * 0.25;
        if (normalize_low) {
            float b0 = -fast_acos(low.y), b1 = fast_acos(low.x);
            unoccluded_visibility += len * ((cosN + 2.0 * b0 * sin(n) - cos(2.0 * b0 - n)) + (cosN + 2.0 * b1 * sin(n) - cos(2.0 * b1 - n))) * 0.25;
        }
    }
    visibility = max(0.03, pow(max(visibility / (normalize_low ? max(unoccluded_visibility, 1e-6) : float(slices)), 0.0), 2.2));
    imageStore(raw_ao, p, uvec4(uint(clamp(visibility / 1.5, 0.0, 1.0) * 255.0 + 0.5)));
}
