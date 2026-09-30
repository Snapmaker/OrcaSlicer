#version 140
// Validated production evaluation. Sampling, integration and falloffs are unchanged.

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
// Keep High radial noise when testing fewer angular slices.
uniform bool high_sampling_noise;
uniform int step_count;
uniform int debug_view;
out vec4 out_color;
#ifdef AO_EDGE_OUTPUT
// RGBA8 L/R/+Y/-Y, normalized connectivity. See xegtao-LICENSE.txt.
out vec4 out_edges;
#endif

bool inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size)); }
float depth_at(ivec2 p) { return texelFetch(depth_texture, clamp(p, ivec2(0), full_size-1), 0).r; }
vec3 position_from_depth(ivec2 p, float depth) {
    vec2 uv = (vec2(p)+0.5)/vec2(full_size);
    vec4 v = inv_projection * vec4(uv*2.0-1.0, depth*2.0-1.0, 1.0);
    return v.xyz/v.w;
}
#ifdef AO_EDGE_OUTPUT
float edge_depth(ivec2 p) { return -position_from_depth(p,depth_at(p)).z; }
vec4 connectivity(ivec2 p,float z) {
    vec4 delta=vec4(edge_depth(p+ivec2(-1,0)),edge_depth(p+ivec2(1,0)),
                    edge_depth(p+ivec2(0,1)),edge_depth(p+ivec2(0,-1)))-z;
    vec2 slope=vec2(delta.y-delta.x,delta.w-delta.z)*0.5;
    vec4 edges=clamp(1.25-min(abs(delta),abs(delta+vec4(slope.x,-slope.x,slope.y,-slope.y)))/max(z*0.011,1e-6),0.0,1.0);
    if(p.x==0) edges.x=0.0;
    if(p.x==full_size.x-1) edges.y=0.0;
    if(p.y==full_size.y-1) edges.z=0.0;
    if(p.y==0) edges.w=0.0;
    return edges;
}
#endif
vec3 position_at(ivec2 p) {
    vec2 uv = (vec2(p)+0.5)/vec2(full_size);
    vec4 v = inv_projection * vec4(uv*2.0-1.0, depth_at(p)*2.0-1.0, 1.0);
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
float guide_weight(ivec2 p, ivec2 q, vec3 P, vec3 N) {
    if (!inside(q) || depth_at(q) >= 1.0) return 0.0;
    vec3 Q = position_at(q);
    float scale = max(footprint(P)*2.0, radius*0.03);
    float plane = max(abs(dot(Q-P,N)), abs(dot(P-Q,normal_at(q))));
    float nw = pow(max(dot(N,normal_at(q)),0.0), 8.0);
    return nw * exp(-plane/scale);
}
/*
 * Horizon integration adapted from three.js r180, examples/jsm/shaders/GTAOShader.js.
 * See gtao-LICENSE.txt for the pinned upstream revision and MIT license.
 */

const float PI=3.141592653589793;
float ign(vec2 p) { return fract(52.9829189*fract(dot(p,vec2(0.06711056,0.00583715)))); }
float sample_noise(ivec2 p) {
    // A separate integer hash avoids deriving radial jitter from a translated
    // copy of the direction noise. It is fixed in time for on-demand rendering.
    uint bits=uint(p.x)*1973u+uint(p.y)*9277u+0x68bc21ebu;
    bits^=bits>>16u; bits*=0x7feb352du;
    bits^=bits>>15u; bits*=0x846ca68bu;
    bits^=bits>>16u;
    return float(bits&0x00ffffffu)/16777216.0;
}
void main() {
    ivec2 q=ivec2(gl_FragCoord.xy), p=representative(q);
    float center_depth=depth_at(p);
#ifdef AO_EDGE_OUTPUT
    out_edges=vec4(0.0);
    if(center_depth<1.0) out_edges=connectivity(p,-position_from_depth(p,center_depth).z);
#endif
    if (center_depth>=1.0) { out_color=vec4(1); return; }
    vec3 P=position_from_depth(p,center_depth), N=normal_at(p);
    vec3 V=perspective ? normalize(-P) : vec3(0,0,1);
    float pixel_footprint=footprint(P);
    float tangent_bias=max(1e-5,pixel_footprint*0.05);
    float thickness_radius=max(radius,1e-5);
    float rp=radius/pixel_footprint;
    float fade=smoothstep(0.5,2.0,rp);
    if (fade<=0.0) { out_color=vec4(1); return; }
    float search_radius=min(rp,64.0);
    // When zoom caps the search, taper its outer taps too. World-space falloff
    // alone can leave a strong horizon that disappears abruptly at 64 pixels.
    float cap_fade=smoothstep(64.0,96.0,rp);
    float phase=ign(vec2(p))*PI;
    float jitter=0.5+0.5*ign(vec2(p)+vec2(19,47));
    // Keep the lower-cost presets unchanged. High and reference captures use
    // independent radial noise, rotated between slices and distance strata.
    float radial_noise=(slice_count>=4 || high_sampling_noise) ? sample_noise(p) : 0.0;
    float sum=0.0, unoccluded_sum=0.0;
    // Higher bounds support diagnostic integration references; normal presets
    // still exit at their configured sample count.
    for(int s=0;s<16;++s) {
        if(s>=slice_count) break;
        float a=phase+float(s)*PI/float(slice_count);
        vec2 direction=vec2(cos(a),sin(a));
        vec3 B=cross(vec3(direction,0),V);
        if(dot(B,B)<1e-12) continue;
        B=normalize(B);
        vec3 T=cross(B,V);
        vec3 ns=N-B*dot(N,B);
        float projected_length=length(ns);
        if(projected_length<1e-6) continue;
        ns/=projected_length;
        vec3 tn=cross(ns,B);
        // Attenuate every candidate against the unoccluded tangent, not the
        // previously accumulated horizon. Repeated taps must not deepen AO.
        vec2 baseline=vec2(dot(V,tn),dot(V,-tn));
        vec2 horizons=baseline;
        for(int j=0;j<32;++j) {
            if(j>=step_count) break;
            float step_jitter=(slice_count>=4 || high_sampling_noise) ?
                0.5+0.5*fract(radial_noise+float(s)*0.754877666+float(j)*0.569840296) : jitter;
            float t=(float(j)+step_jitter)/float(step_count);
            // Concentrate samples near contacts while retaining the outer radius.
            float distance_px=mix(min(1.0,search_radius),search_radius,t*t);
            float screen_falloff=mix(1.0,1.0-smoothstep(search_radius*0.65,search_radius,distance_px),cap_fade);
            for(int side=0;side<2;++side) {
                float sign_side=side==0 ? 1.0 : -1.0;
                ivec2 sp=ivec2(floor(vec2(p)+0.5+sign_side*direction*distance_px));
                if(!inside(sp)||all(equal(sp,p))) continue;
                float sample_depth=depth_at(sp);
                if(sample_depth>=1.0) continue;
                vec3 delta=position_from_depth(sp,sample_depth)-P;
                // Reject non-occluders before the square root and normalization.
                float distance_squared=dot(delta,delta);
                if(distance_squared<1e-12||distance_squared>=radius*radius) continue;
                // Pixel snapping can move a tap off its slice. Samples on or
                // below the local tangent plane are not hemisphere occluders.
                float normal_distance=dot(N,delta);
                if(normal_distance<=tangent_bias) continue;
                float distance_world=sqrt(distance_squared);
                // Start the distance taper slightly earlier while retaining the
                // search radius, so tall nearby occluders are still sampled.
                float falloff=(1.0-smoothstep(radius*0.45,radius,distance_world))*screen_falloff;
                // Mild extra taper along the receiver's tangent plane reduces
                // broad skirts. Occluders directly above a contact keep their
                // original weight; this is a spatial tuning, not a depth cutoff.
                float lateral_distance=sqrt(max(0.0,distance_world*distance_world-normal_distance*normal_distance));
                falloff*=1.0-0.25*smoothstep(radius*0.2,radius*0.8,lateral_distance);
                // A hard |delta.z| cutoff erases contact AO under tall objects in top view.
                // Treat thickness as a soft attenuation scale, retaining nearby front occluders.
                float thin=1.0/(1.0+max(0.0,abs(delta.z)-thickness)/thickness_radius);
                float h=dot(V,delta/distance_world);
                if(side==0) horizons.x=max(horizons.x,mix(baseline.x,h,falloff*thin));
                else horizons.y=max(horizons.y,mix(baseline.y,h,falloff*thin));
            }
        }
        horizons=clamp(horizons,vec2(-1),vec2(1));
        vec2 sins=sqrt(max(vec2(0),1.0-horizons*horizons));
        float nx=dot(ns,T), ny=dot(ns,V);
        float nxb=0.5*(acos(horizons.y)-acos(horizons.x)+sins.x*horizons.x-sins.y*horizons.y);
        float nyb=0.5*(2.0-horizons.x*horizons.x-horizons.y*horizons.y);
        // Retain the projected normal's measure. Normalize the finite set of
        // slices against the same unoccluded integral, keeping sloped/curved
        // surfaces white without clamping away weak contact occlusion.
        float baseline_visibility=max(1e-6,ny+nx*asin(clamp(nx,-1.0,1.0)));
        sum+=projected_length*clamp(nx*nxb+ny*nyb,0.0,baseline_visibility);
        unoccluded_sum+=projected_length*baseline_visibility;
    }
    float visibility=unoccluded_sum>1e-6 ? sum/unoccluded_sum : 1.0;
    out_color=vec4(mix(1.0,clamp(visibility,0.0,1.0),fade));
}
