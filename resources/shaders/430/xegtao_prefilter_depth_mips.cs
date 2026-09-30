#version 430 core
// Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT
// XeGTAO a5b1686: FP32 GLSL port; OpenGL inverse projection and padded boundaries.
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D source_depth;
layout(r32f, binding = 0) writeonly uniform image2D mip0;
layout(r32f, binding = 1) writeonly uniform image2D mip1;
layout(r32f, binding = 2) writeonly uniform image2D mip2;
layout(r32f, binding = 3) writeonly uniform image2D mip3;
layout(r32f, binding = 4) writeonly uniform image2D mip4;
uniform mat4  inverse_projection;
uniform ivec2 full_size;
uniform float radius;
shared float  scratch[64];
float         load_depth(ivec2 p)
{
    p       = clamp(p, ivec2(0), full_size - 1);
    float d = texelFetch(source_depth, p, 0).r;
    if (d >= 1.0)
        return 1e20;
    // Standard orthographic/perspective projection: view Z is independent of XY.
    float ndc_z  = d * 2.0 - 1.0;
    float view_z = inverse_projection[2][2] * ndc_z + inverse_projection[3][2];
    float view_w = inverse_projection[2][3] * ndc_z + inverse_projection[3][3];
    return clamp(-view_z / view_w, 0.0, 1e20);
}
float reduce_depth(vec4 d)
{
    float farthest = max(max(d.x, d.y), max(d.z, d.w));
    float r        = max(0.75 * radius * 1.457, 1e-6);
    vec4  weights  = clamp(vec4(1.0 / 0.615) - (farthest - d) / (r * 0.615), 0.0, 1.0);
    return dot(weights, d) / max(dot(weights, vec4(1)), 1e-6);
}
void main()
{
    ivec2 b = ivec2(gl_GlobalInvocationID.xy), p = b * 2;
    ivec2 l = ivec2(gl_LocalInvocationID.xy);
    int   i = l.y * 8 + l.x;
    vec4  d = vec4(load_depth(p), load_depth(p + ivec2(1, 0)), load_depth(p + ivec2(0, 1)), load_depth(p + 1));
    imageStore(mip0, p, vec4(d.x));
    imageStore(mip0, p + ivec2(1, 0), vec4(d.y));
    imageStore(mip0, p + ivec2(0, 1), vec4(d.z));
    imageStore(mip0, p + 1, vec4(d.w));
    scratch[i] = reduce_depth(d);
    imageStore(mip1, b, vec4(scratch[i]));
    barrier();
    if (all(equal(l % 2, ivec2(0)))) {
        scratch[i] = reduce_depth(vec4(scratch[i], scratch[i + 1], scratch[i + 8], scratch[i + 9]));
        imageStore(mip2, b / 2, vec4(scratch[i]));
    }
    barrier();
    if (all(equal(l % 4, ivec2(0)))) {
        scratch[i] = reduce_depth(vec4(scratch[i], scratch[i + 2], scratch[i + 16], scratch[i + 18]));
        imageStore(mip3, b / 4, vec4(scratch[i]));
    }
    barrier();
    if (all(equal(l, ivec2(0))))
        imageStore(mip4, b / 8, vec4(reduce_depth(vec4(scratch[0], scratch[4], scratch[32], scratch[36]))));
}
