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
uniform int       slice_count;
uniform int       step_count;
uniform int       debug_view;
out vec4          out_color;
#ifdef AO_CONFIDENCE_OUTPUT
out float out_confidence;
#endif

bool  inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size)); }
float depth_at(ivec2 p) { return texelFetch(depth_texture, clamp(p, ivec2(0), full_size - 1), 0).r; }
vec3  position_from_depth(ivec2 p, float depth)
{
    vec2 uv = (vec2(p) + 0.5) / vec2(full_size);
    vec4 v  = inv_projection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}
vec3 position_at(ivec2 p) { return position_from_depth(p, depth_at(p)); }
#ifdef AO_CONFIDENCE_OUTPUT
float view_z(ivec2 p, float depth)
{
    if (inv_projection[0][2] != 0.0 || inv_projection[1][2] != 0.0 ||
        inv_projection[0][3] != 0.0 || inv_projection[1][3] != 0.0)
        return position_from_depth(p, depth).z;
    float clip_z = depth * 2.0 - 1.0;
    return (inv_projection[2][2] * clip_z + inv_projection[3][2]) /
           (inv_projection[2][3] * clip_z + inv_projection[3][3]);
}
float composite_confidence(ivec2 p, float depth)
{
    float z = view_z(p, depth);
    float pixel_size = max(1e-5, 2.0 * (perspective ? -z : 1.0) / (abs(projection[1][1]) * float(full_size.y)));
    float gap_start = max(radius, pixel_size * 4.0);
    float gap_end = max(radius * 2.0, pixel_size * 8.0);
    float confidence = 1.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 o = i == 0 ? ivec2(1, 0) : i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(0, 1) : ivec2(0, -1);
        ivec2 q = p + o;
        if (!inside(q)) continue;
        float d = depth_at(q);
        if (d >= 1.0) confidence = 0.0;
        else confidence = min(confidence, 1.0 - smoothstep(gap_start, gap_end, abs(view_z(q, d) - z)));
        if (confidence == 0.0) break;
    }
    return confidence;
}
#endif
vec2 sign_not_zero(vec2 v) { return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 encode_normal(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-8);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
}
vec3 normal_at(ivec2 p)
{
    vec2 e = texelFetch(normal_texture, p, 0).rg;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    return normalize(n);
}
ivec2 representative(ivec2 q) { return all(equal(ao_size, full_size)) ? q : min(2 * q + ivec2(1), full_size - 1); }
float footprint(vec3 p) { return max(1e-5, 2.0 * (perspective ? -p.z : 1.0) / (abs(projection[1][1]) * float(full_size.y))); }
float guide_weight(ivec2 p, ivec2 q, vec3 P, vec3 N)
{
    if (!inside(q) || depth_at(q) >= 1.0)
        return 0.0;
    vec3  Q     = position_at(q);
    float scale = max(footprint(P) * 2.0, radius * 0.03);
    float plane = max(abs(dot(Q - P, N)), abs(dot(P - Q, normal_at(q))));
    float nw    = pow(max(dot(N, normal_at(q)), 0.0), 8.0);
    return nw * exp(-plane / scale);
}

vec3 tangent(ivec2 p, ivec2 axis, vec3 P, float center_depth)
{
    ivec2 a = p - axis, b = p + axis;
    float depth_a = depth_at(a), depth_b = depth_at(b);
    bool  va = inside(a) && depth_a < 1.0;
    bool  vb = inside(b) && depth_b < 1.0;
    if (!va && !vb)
        return vec3(0.0);
    if (!va)
        return position_from_depth(b, depth_b) - P;
    if (!vb)
        return P - position_from_depth(a, depth_a);
    float da = abs(2.0 * depth_a - depth_at(p - 2 * axis) - center_depth);
    float db = abs(2.0 * depth_b - depth_at(p + 2 * axis) - center_depth);
    return da < db ? P - position_from_depth(a, depth_a) : position_from_depth(b, depth_b) - P;
}
void main()
{
    ivec2 p            = ivec2(gl_FragCoord.xy);
    float center_depth = depth_at(p);
#ifdef AO_CONFIDENCE_OUTPUT
    out_confidence = center_depth < 1.0 ? composite_confidence(p, center_depth) : 1.0;
#endif
    if (center_depth >= 1.0) {
        out_color = vec4(0.0);
        return;
    }
    vec3 P    = position_from_depth(p, center_depth);
    vec3 n    = cross(tangent(p, ivec2(1, 0), P, center_depth), tangent(p, ivec2(0, 1), P, center_depth));
    n         = dot(n, n) > 1e-16 ? normalize(n) : (perspective ? normalize(-P) : vec3(0, 0, 1));
    out_color = vec4(encode_normal(n), 0, 1);
}
