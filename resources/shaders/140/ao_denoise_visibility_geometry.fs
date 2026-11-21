#version 140
// Validated production guide reuse. Kernel, geometric weights and refinement remain unchanged.

uniform bool filter_refine;
uniform sampler2D depth_texture;
uniform sampler2D normal_texture;
uniform sampler2D ao_texture;
uniform mat4 inv_projection;
uniform mat4 projection;
uniform ivec2 full_size;
uniform ivec2 ao_size;
uniform ivec2 viewport_origin;
uniform bool perspective;
uniform float radius;
uniform float thickness;
uniform float intensity;
uniform int slice_count;
uniform int step_count;
uniform int debug_view;
out vec4 out_color;

bool inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size)); }
float depth_at(ivec2 p) { return texelFetch(depth_texture, clamp(p, ivec2(0), full_size-1), 0).r; }
vec3 position_from_depth(ivec2 p, float depth) {
    vec2 uv = (vec2(p)+0.5)/vec2(full_size);
    vec4 v = inv_projection * vec4(uv*2.0-1.0, depth*2.0-1.0, 1.0);
    return v.xyz/v.w;
}
vec2 sign_not_zero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 encode_normal(vec3 n) {
    n /= max(abs(n.x)+abs(n.y)+abs(n.z), 1e-8);
    return n.z >= 0.0 ? n.xy : (1.0-abs(n.yx))*sign_not_zero(n.xy);
}
vec3 normal_at(ivec2 p) {
    vec2 e = texelFetch(normal_texture, p, 0).rg;
    vec3 n = vec3(e, 1.0-abs(e.x)-abs(e.y));
    if (n.z < 0.0) n.xy = (1.0-abs(n.yx))*sign_not_zero(n.xy);
    return normalize(n);
}
ivec2 representative(ivec2 q) {
    return all(equal(ao_size, full_size)) ? q : min(2*q+ivec2(1), full_size-1);
}
float footprint(vec3 p) {
    return max(1e-5, 2.0*(perspective ? -p.z : 1.0)/(abs(projection[1][1])*float(full_size.y)));
}
float guide_weight(ivec2 q, vec3 P, vec3 N, float scale) {
    if (!inside(q)) return 0.0;
    float depth = depth_at(q);
    if (depth >= 1.0) return 0.0;
    vec3 Q = position_from_depth(q, depth);
    vec3 Nq = normal_at(q);
    float plane = max(abs(dot(Q-P,N)), abs(dot(P-Q,Nq)));
    // Smooth curved surfaces without borrowing AO across a silhouette or crease.
    float nw = pow(max(dot(N,Nq),0.0), 4.0);
    return nw * exp(-plane/scale);
}

void main() {
    ivec2 q=ivec2(gl_FragCoord.xy), p=representative(q);
    float center_depth=depth_at(p);
    if(center_depth>=1.0) { out_color=vec4(1); return; }
    vec3 P=position_from_depth(p,center_depth), N=normal_at(p);
    float scale=max(footprint(P)*2.0,radius*0.03);
    // A sparse second pass removes residual variation from the already denoised
    // field. AO-range weights retain contact gradients; geometry weights prevent
    // borrowing from another surface. This runs only at full resolution.
    if(filter_refine) {
        float center=texelFetch(ao_texture,q,0).r;
        float sum=0.0, weight=0.0;
        for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
            ivec2 sq=q+ivec2(x,y)*2;
            if(!inside(sq)) continue;
            float value=texelFetch(ao_texture,sq,0).r;
            float w=exp(-0.5*float(x*x+y*y)-abs(value-center)/0.15)*guide_weight(sq,P,N,scale);
            sum+=value*w;weight+=w;
        }
        out_color=vec4(weight>1e-6 ? sum/weight : center);
        return;
    }
    // A wider kernel is useful on continuous full-resolution surfaces. Keep
    // the original footprint at silhouettes, creases and half-resolution AO.
    // Blend smoothly so crossing the continuity threshold cannot create a seam.
    float smoothing=0.0;
    if(all(equal(ao_size,full_size))) {
        float continuity=min(min(guide_weight(p+ivec2(3,0),P,N,scale),guide_weight(p-ivec2(3,0),P,N,scale)),
                             min(guide_weight(p+ivec2(0,3),P,N,scale),guide_weight(p-ivec2(0,3),P,N,scale)));
        smoothing=smoothstep(0.65,0.9,continuity);
    }
    float sum=0.0, weight=0.0;
    for(int y=-3;y<=3;++y) for(int x=-3;x<=3;++x) {
        bool outer=abs(x)>2||abs(y)>2;
        if(outer && smoothing<=0.0) continue;
        ivec2 sq=q+ivec2(x,y);
        if(any(lessThan(sq,ivec2(0)))||any(greaterThanEqual(sq,ao_size))) continue;
        float w=exp(-mix(0.32,0.18,smoothing)*float(x*x+y*y))*guide_weight(representative(sq),P,N,scale);
        if(outer) w*=smoothing;
        sum+=texelFetch(ao_texture,sq,0).r*w;
        weight+=w;
    }
    out_color=vec4(weight>1e-6 ? sum/weight : 1.0);
}
