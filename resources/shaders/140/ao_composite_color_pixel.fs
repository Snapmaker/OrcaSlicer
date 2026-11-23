#version 140

uniform sampler2D depth_texture;
uniform sampler2D normal_texture;
uniform sampler2D ao_texture;
uniform mat4      inv_projection;
uniform mat4      projection;
uniform ivec2     full_size;
uniform ivec2     ao_size;
uniform ivec2     viewport_origin;
uniform bool      perspective;
uniform float     radius;
uniform float     thickness;
uniform float     intensity;
uniform float     ao_strength;
uniform int       slice_count;
uniform int       step_count;
uniform int       debug_view;
out vec4          out_color;
#ifdef AO_PRECOMPUTED_CONFIDENCE
uniform sampler2D confidence_texture;
#endif

bool  inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size)); }
float depth_at(ivec2 p) { return texelFetch(depth_texture, clamp(p, ivec2(0), full_size - 1), 0).r; }
vec3 position_from_depth(ivec2 p, float depth)
{
    vec2 uv = (vec2(p) + 0.5) / vec2(full_size);
    vec4 v  = inv_projection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}
// Standard perspective/orthographic projections need only clip Z and W here.
// Retain full reconstruction for projections whose Z/W depend on screen XY.
float view_z(ivec2 p, float depth)
{
    if (inv_projection[0][2] != 0.0 || inv_projection[1][2] != 0.0 ||
        inv_projection[0][3] != 0.0 || inv_projection[1][3] != 0.0)
        return position_from_depth(p, depth).z;
    float clip_z = depth * 2.0 - 1.0;
    return (inv_projection[2][2] * clip_z + inv_projection[3][2]) /
           (inv_projection[2][3] * clip_z + inv_projection[3][3]);
}
vec2 sign_not_zero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec3 normal_at(ivec2 p)
{
    vec2 e = texelFetch(normal_texture, p, 0).rg;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    return normalize(n);
}
float footprint(vec3 p) { return max(1e-5, 2.0 * (perspective ? -p.z : 1.0) / (abs(projection[1][1]) * float(full_size.y))); }

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - viewport_origin;
    float center_depth = depth_at(p);
    if (center_depth >= 1.0) {
        out_color = vec4(1);
        return;
    }
    if (debug_view == 1) {
        out_color = vec4(vec3(1.0 / (1.0 + abs(view_z(p, center_depth)) * 0.01)), 1);
        return;
    }
    if (debug_view == 2) {
        out_color = vec4(normal_at(p) * 0.5 + 0.5, 1);
        return;
    }
    float visibility = clamp(texelFetch(ao_texture, p, 0).r, 0.0, 1.0);
    // Show the AO buffer independently of presentation strength and contrast.
    if (debug_view == 3) {
        out_color = vec4(vec3(visibility), 1);
        return;
    }
    // These pixels blend to identity regardless of confidence. Keep diagnostic
    // exports on the full path so their confidence channels remain meaningful.
    if (debug_view == 0 && (visibility >= 1.0 || ao_strength <= 0.0)) {
        out_color = vec4(1);
        return;
    }
    float ao         = intensity == 1.0 ? visibility : pow(visibility, intensity);
#ifdef AO_PRECOMPUTED_CONFIDENCE
    // MSAA may have selected a different pixel; always use that surface's guide.
    float confidence = texelFetch(confidence_texture, p, 0).r;
#else
    vec3 P = vec3(0, 0, view_z(p, center_depth));
    float confidence = 1.0;
    float gap_start = max(radius, footprint(P) * 4.0);
    float gap_end = max(radius * 2.0, footprint(P) * 8.0);
    // Reject depth discontinuities, not normal discontinuities (which include true creases).
    for (int i = 0; i < 4; ++i) {
        ivec2 o = i == 0 ? ivec2(1, 0) : i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(0, 1) : ivec2(0, -1);
        ivec2 q = p + o;
        if (!inside(q))
            continue;
        float neighbor_depth = depth_at(q);
        if (neighbor_depth >= 1.0)
            confidence = 0.0;
        else {
            float gap  = abs(view_z(q, neighbor_depth) - P.z);
            confidence = min(confidence, 1.0 - smoothstep(gap_start, gap_end, gap));
        }
        if (confidence == 0.0)
            break;
    }
#endif
    if (debug_view == 4) {
        out_color = vec4(vec3(confidence), 1);
        return;
    }
    out_color = vec4(vec3(mix(1.0, ao, confidence * clamp(ao_strength, 0.0, 1.0))), 1);
}
