#version 430 core
// Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT
// XeGTAO a5b1686 denoiser. Two horizontal pixels/invocation, GL lower-left coordinates.
// Full MIT notice and adaptation details: ../third_party_licenses.txt.
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform usampler2D source_ao;
layout(binding = 1) uniform usampler2D source_edges;
layout(binding = 2) uniform sampler2D source_depth;
layout(r8ui, binding = 0) writeonly uniform uimage2D intermediate;
layout(r8, binding = 1) writeonly uniform image2D final_ao;
uniform ivec2 full_size;
uniform bool  final_apply;
vec4          decode_edges(uint e) { return vec4((e >> 6u) & 3u, (e >> 4u) & 3u, (e >> 2u) & 3u, e & 3u) / 3.0; }
void          pixel(ivec2 p, float values[9], vec4 C, vec4 L, vec4 R, vec4 T, vec4 B)
{
    if (any(greaterThanEqual(p, full_size)))
        return;
    if (texelFetch(source_depth, p, 0).r >= 1e19) {
        if (final_apply)
            imageStore(final_ao, p, vec4(1));
        else
            imageStore(intermediate, p, uvec4(170));
        return;
    }
    C *= vec4(L.y, R.x, T.w, B.z);
    float leak           = clamp((1.5 - dot(C, vec4(1))) / 1.5, 0.0, 1.0) * 0.5;
    C                    = clamp(C + leak, 0.0, 1.0);
    vec4        diagonal = 0.425 * vec4(C.x * L.z + C.z * T.x, C.z * T.y + C.y * R.z, C.w * B.x + C.x * L.w, C.y * R.w + C.w * B.y);
    float       center   = final_apply ? 1.2 : 0.24;
    float       sum = values[0] * center, weights = center;
    const ivec2 offsets[8] = ivec2[8](ivec2(-1, 0), ivec2(1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(-1, 1), ivec2(1, 1), ivec2(-1, -1),
                                      ivec2(1, -1));
    for (int i = 0; i < 8; ++i) {
        ivec2 q = p + offsets[i];
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, full_size)))
            continue;
        float w = i < 4 ? C[i] : diagonal[i - 4];
        sum += values[i + 1] * w;
        weights += w;
    }
    float result = sum / weights;
    if (final_apply)
        imageStore(final_ao, p, vec4(clamp(result * 1.5, 0.0, 1.0)));
    else
        imageStore(intermediate, p, uvec4(uint(clamp(result, 0.0, 1.0) * 255.0 + 0.5)));
}
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy) * ivec2(2, 1);
    if (any(greaterThanEqual(p, full_size)))
        return;
    // GL Gather: x=upper-left, y=upper-right, z=lower-right, w=lower-left.
    vec2  inv_size  = 1.0 / vec2(full_size);
    vec4  a         = vec4(textureGather(source_ao, vec2(p) * inv_size)) / 255.0;
    vec4  b         = vec4(textureGather(source_ao, vec2(p + ivec2(2, 0)) * inv_size)) / 255.0;
    vec4  c         = vec4(textureGather(source_ao, vec2(p + ivec2(0, 1)) * inv_size)) / 255.0;
    vec4  d         = vec4(textureGather(source_ao, vec2(p + ivec2(2, 1)) * inv_size)) / 255.0;
    float first[9]  = float[9](a.y, a.x, b.x, c.y, a.z, c.x, d.x, a.w, b.w);
    float second[9] = float[9](b.x, a.y, b.y, d.x, b.w, c.y, d.y, a.z, b.z);
    // Share the two centre edges across both stencils. GL Gather's upper row
    // is +Y; clamp-to-edge reproduces the previous clamped texelFetch reads.
    uvec4 e0      = textureGather(source_edges, vec2(p) * inv_size);
    uvec4 e1      = textureGather(source_edges, vec2(p + ivec2(2, 0)) * inv_size);
    uvec4 e2      = textureGather(source_edges, vec2(p + ivec2(1, 1)) * inv_size);
    vec4  center0 = decode_edges(e0.y), center1 = decode_edges(e1.x);
    pixel(p, first, center0, decode_edges(e0.x), center1, decode_edges(e2.x), decode_edges(e0.z));
    pixel(p + ivec2(1, 0), second, center1, center0, decode_edges(e1.y), decode_edges(e2.y), decode_edges(e1.w));
}
