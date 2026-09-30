#version 140
// Copyright (C) 2016-2021, Intel Corporation
// SPDX-License-Identifier: MIT
// Adapted from XeGTAO_Denoise; see xe-gtao-denoise-LICENSE.txt.
// GLSL 140 prototype: RGBA8 edges, one pixel/invocation, no deliberate edge leakage.
uniform sampler2D depth_texture;
uniform sampler2D ao_texture;
uniform sampler2D edge_texture;
uniform ivec2 full_size;
uniform bool final_apply;
out vec4 out_color;

bool inside(ivec2 p) {
    return all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,full_size));
}
vec4 edges(ivec2 p) {
    // Channels are left/right/down/up (OpenGL bottom-left origin).
    return inside(p)?texelFetch(edge_texture,p,0):vec4(0);
}
void add_sample(ivec2 p,float w,inout float sum,inout float total) {
    if(!inside(p)) return;
    sum+=w*texelFetch(ao_texture,p,0).r;
    total+=w;
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    if(texelFetch(depth_texture,p,0).r>=1.0) { out_color=vec4(1); return; }
    vec4 L=edges(p+ivec2(-1,0)), R=edges(p+ivec2(1,0));
    vec4 D=edges(p+ivec2(0,-1)), U=edges(p+ivec2(0,1));
    // XeGTAO reciprocal multiplication; unlike the older 5x5 prototype's min.
    vec4 C=edges(p)*vec4(L.y,R.x,D.w,U.z);
    const float diagonal=0.85*0.5;
    float ld=diagonal*(C.x*L.z+C.z*D.x);
    float rd=diagonal*(C.y*R.z+C.z*D.y);
    float lu=diagonal*(C.x*L.w+C.w*U.x);
    float ru=diagonal*(C.y*R.w+C.w*U.y);
    // Retain upstream relative centre weight for intermediate/final passes.
    // Input/output visibility is already [0,1]; no XeGTAO term-scale conversion.
    float total=final_apply?1.2:1.2/5.0;
    float sum=texelFetch(ao_texture,p,0).r*total;
    add_sample(p+ivec2(-1,0),C.x,sum,total);
    add_sample(p+ivec2(1,0),C.y,sum,total);
    add_sample(p+ivec2(0,-1),C.z,sum,total);
    add_sample(p+ivec2(0,1),C.w,sum,total);
    add_sample(p+ivec2(-1,-1),ld,sum,total);
    add_sample(p+ivec2(1,-1),rd,sum,total);
    add_sample(p+ivec2(-1,1),lu,sum,total);
    add_sample(p+ivec2(1,1),ru,sum,total);
    out_color=vec4(sum/total);
}
