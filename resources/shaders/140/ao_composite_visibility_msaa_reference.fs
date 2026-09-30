#version 400

uniform sampler2D   depth_texture;
uniform sampler2DMS sample_depth_texture;
uniform sampler2D   normal_texture;
uniform sampler2D   ao_texture;
uniform mat4        inv_projection;
uniform mat4        projection;
uniform ivec2       full_size;
uniform ivec2       ao_size;
uniform ivec2       viewport_origin;
uniform bool        perspective;
uniform float       radius;
uniform float       thickness;
uniform float       intensity;
uniform float       ao_strength;
uniform int         slice_count;
uniform int         step_count;
uniform int         debug_view;
uniform bool        capture_sample;
uniform int         capture_sample_index;
uniform vec2        capture_sample_position;
out vec4            out_color;

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

ivec2 match_surface(ivec2 p, vec3 sample_p, out int status)
{
    status           = 0;
    float pixel_size = footprint(sample_p);
    float error      = depth_at(p) < 1.0 ? abs(dot(sample_p - position_at(p), normal_at(p))) : 1e30;
    if (error > pixel_size * 0.5) {
        ivec2 best       = p;
        float best_score = 1e30, best_error = 1e30;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
                ivec2 q = p + ivec2(x, y);
                if (!inside(q) || depth_at(q) >= 1.0)
                    continue;
                float plane_error = abs(dot(sample_p - position_at(q), normal_at(q)));
                float score       = plane_error + pixel_size * 0.05 * float(x * x + y * y);
                if (score < best_score) {
                    best       = q;
                    best_score = score;
                    best_error = plane_error;
                }
            }
        if (best_error > pixel_size) {
            status = 2;
            return p;
        }
        status = 1;
        p      = best;
    }
    return p;
}

void main()
{
    ivec2 p               = ivec2(gl_FragCoord.xy) - viewport_origin;
    int   sample_index    = capture_sample ? capture_sample_index : gl_SampleID;
    vec2  sample_position = capture_sample ? capture_sample_position : gl_SamplePosition;
    float sample_depth    = texelFetch(sample_depth_texture, p, sample_index).r;
    if (sample_depth >= 1.0) {
        out_color = capture_sample && debug_view == 5 ? vec4(sample_depth, vec2(p), 3) :
                    capture_sample && debug_view == 6 ? vec4(1, 0, 1, 1) :
                                                        vec4(1);
        return;
    }
    vec2 sample_uv = (vec2(p) + sample_position) / vec2(full_size);
    vec4 sample_h  = inv_projection * vec4(sample_uv * 2.0 - 1.0, sample_depth * 2.0 - 1.0, 1);
    vec3 sample_p  = sample_h.xyz / sample_h.w;
    int  status;
    p = match_surface(p, sample_p, status);
    if (capture_sample && debug_view == 5) {
        // RGBA32F export: actual depth, chosen pixel X/Y, status. Coordinates use
        // OpenGL's bottom-left origin; status 0=original, 1=search, 2=reject, 3=sky.
        out_color = vec4(sample_depth, vec2(p), float(status));
        return;
    }
    if (status == 2 || depth_at(p) >= 1.0) {
        out_color = capture_sample && debug_view == 6 ? vec4(1, 0, 1, 1) : vec4(1);
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
    if (debug_view == 0 && !capture_sample && (visibility >= 1.0 || ao_strength <= 0.0)) {
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
    float factor = mix(1.0, ao, confidence * clamp(ao_strength, 0.0, 1.0));
    // One-shot export, using the same sampled surface and arithmetic as presentation.
    if (capture_sample && debug_view == 6) {
        out_color = vec4(visibility, confidence, factor, 1);
        return;
    }
    out_color = vec4(vec3(factor), 1);
}
