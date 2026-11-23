#version 140
// Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT
// XeGTAO a5b1686 edge-guided stencil adapted to normalized FS AO (no 1.5 encoding or exponent).
// RGBA8 edge order: left, right, +Y, -Y. See ../third_party_licenses.txt.
uniform sampler2D depth_texture;
uniform sampler2D ao_texture;
uniform sampler2D edge_texture;
uniform ivec2 full_size;
uniform bool filter_refine;
out vec4 out_color;
vec4 edges(ivec2 p) { return texelFetch(edge_texture,clamp(p,ivec2(0),full_size-1),0); }
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    if(texelFetch(depth_texture,p,0).r>=1.0) { out_color=vec4(1); return; }
    vec4 C=edges(p),L=edges(p+ivec2(-1,0)),R=edges(p+ivec2(1,0)),T=edges(p+ivec2(0,1)),B=edges(p+ivec2(0,-1));
    C*=vec4(L.y,R.x,T.w,B.z);
    float leak=clamp((1.5-dot(C,vec4(1)))/1.5,0.0,1.0)*0.5;
    C=clamp(C+leak,0.0,1.0);
    vec4 diagonal=0.425*vec4(C.x*L.z+C.z*T.x,C.z*T.y+C.y*R.z,C.w*B.x+C.x*L.w,C.y*R.w+C.w*B.y);
    float weight=filter_refine ? 1.2 : 0.24;
    float sum=texelFetch(ao_texture,p,0).r*weight;
    const ivec2 offsets[8]=ivec2[8](ivec2(-1,0),ivec2(1,0),ivec2(0,1),ivec2(0,-1),
                                  ivec2(-1,1),ivec2(1,1),ivec2(-1,-1),ivec2(1,-1));
    for(int i=0;i<8;++i) {
        ivec2 q=p+offsets[i];
        if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,full_size))) continue;
        float w=i<4 ? C[i] : diagonal[i-4];
        sum+=texelFetch(ao_texture,q,0).r*w;
        weight+=w;
    }
    out_color=vec4(clamp(sum/weight,0.0,1.0));
}
