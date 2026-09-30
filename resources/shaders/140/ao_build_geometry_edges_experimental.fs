#version 140
// Experimental full-resolution RGBA8 connections: left, right, down, up.
uniform sampler2D depth_texture;
uniform sampler2D normal_texture;
uniform mat4 inv_projection;
uniform mat4 projection;
uniform ivec2 full_size;
uniform bool perspective;
uniform float radius;
out vec4 out_color;

bool inside(ivec2 p) {
    return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size));
}
vec3 position(ivec2 p, float depth) {
    vec2 uv = (vec2(p)+0.5)/vec2(full_size);
    vec4 v = inv_projection * vec4(uv*2.0-1.0, depth*2.0-1.0, 1.0);
    return v.xyz/v.w;
}
vec3 normal(ivec2 p) {
    vec2 e = texelFetch(normal_texture,p,0).rg;
    vec3 n = vec3(e,1.0-abs(e.x)-abs(e.y));
    if(n.z<0.0) n.xy=(1.0-abs(n.yx))*vec2(n.x>=0.0?1.0:-1.0,n.y>=0.0?1.0:-1.0);
    return normalize(n);
}
float scale(vec3 p) {
    float footprint=max(1e-5,2.0*(perspective?-p.z:1.0)/(abs(projection[1][1])*float(full_size.y)));
    return max(footprint*2.0,radius*0.03);
}
float connection(ivec2 q,vec3 P,vec3 N,float center_scale) {
    if(!inside(q)) return 0.0;
    float d=texelFetch(depth_texture,q,0).r;
    if(d>=1.0) return 0.0;
    vec3 Q=position(q,d), Nq=normal(q);
    float plane=max(abs(dot(Q-P,N)),abs(dot(P-Q,Nq)));
    // Symmetric, conservative combination of the existing guide in both directions.
    float s=min(center_scale,scale(Q));
    return pow(max(dot(N,Nq),0.0),4.0)*exp(-plane/s);
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    float d=texelFetch(depth_texture,p,0).r;
    if(d>=1.0) { out_color=vec4(0); return; }
    vec3 P=position(p,d), N=normal(p);
    float s=scale(P);
    out_color=vec4(connection(p+ivec2(-1,0),P,N,s),connection(p+ivec2(1,0),P,N,s),
                   connection(p+ivec2(0,-1),P,N,s),connection(p+ivec2(0,1),P,N,s));
}
