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
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (depth_at(p) >= 1.0) {
        out_color = vec4(1);
        return;
    }
    vec3 P = position_at(p), N = normal_at(p);
    // Representatives are at full-resolution coordinates 2*q+1 (texel indices).
    vec2  low  = (vec2(p) - 1.0) * 0.5;
    ivec2 base = ivec2(floor(low));
    // Odd viewports clamp the final representative to full_size-1, shortening
    // the last interval to one pixel. Interpolate between the actual centres.
    ivec2 lo = clamp(base, ivec2(0), ao_size - 1);
    ivec2 hi = clamp(base + 1, ivec2(0), ao_size - 1);
    vec2  a = vec2(representative(lo)), b = vec2(representative(hi));
    vec2  f   = clamp((vec2(p) - a) / max(b - a, vec2(1)), 0.0, 1.0);
    float sum = 0.0, weight = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            ivec2 q   = clamp(base + ivec2(x, y), ivec2(0), ao_size - 1);
            vec2  wxy = mix(1.0 - f, f, vec2(x, y));
            float w   = wxy.x * wxy.y * guide_weight(p, representative(q), P, N);
            sum += texelFetch(ao_texture, q, 0).r * w;
            weight += w;
        }
    out_color = vec4(weight > 1e-5 ? sum / weight : 1.0);
}
