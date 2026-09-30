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

bool  inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, full_size)); }
float depth_at(ivec2 p) { return texelFetch(depth_texture, clamp(p, ivec2(0), full_size - 1), 0).r; }
vec3  position_at(ivec2 p)
{
    vec2 uv = (vec2(p) + 0.5) / vec2(full_size);
    vec4 v  = inv_projection * vec4(uv * 2.0 - 1.0, depth_at(p) * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}
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

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - viewport_origin;
    if (depth_at(p) >= 1.0) {
        out_color = vec4(1);
        return;
    }
    if (debug_view == 1) {
        out_color = vec4(vec3(1.0 / (1.0 + abs(position_at(p).z) * 0.01)), 1);
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
    float ao         = pow(visibility, intensity);
    vec3  P          = position_at(p);
    float confidence = 1.0;
    float gap_start = max(radius, footprint(P) * 4.0);
    float gap_end = max(radius * 2.0, footprint(P) * 8.0);
    // Reject depth discontinuities, not normal discontinuities (which include true creases).
    for (int i = 0; i < 4; ++i) {
        ivec2 o = i == 0 ? ivec2(1, 0) : i == 1 ? ivec2(-1, 0) : i == 2 ? ivec2(0, 1) : ivec2(0, -1);
        ivec2 q = p + o;
        if (!inside(q))
            continue;
        if (depth_at(q) >= 1.0)
            confidence = 0.0;
        else {
            float gap  = abs(position_at(q).z - P.z);
            confidence = min(confidence, 1.0 - smoothstep(gap_start, gap_end, gap));
        }
        if (confidence == 0.0)
            break;
    }
    if (debug_view == 4) {
        out_color = vec4(vec3(confidence), 1);
        return;
    }
    out_color = vec4(vec3(mix(1.0, ao, confidence * clamp(ao_strength, 0.0, 1.0))), 1);
}
