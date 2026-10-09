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

ivec2 match_surface(ivec2 p, vec3 sample_p, out int status)
{
    status           = 0;
    float center_depth = depth_at(p);
    float pixel_size = footprint(sample_p);
    float error      = center_depth < 1.0 ? abs(dot(sample_p - position_from_depth(p, center_depth), normal_at(p))) : 1e30;
    if (error > pixel_size * 0.5) {
        ivec2 best       = p;
        float best_score = 1e30, best_error = 1e30;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
                ivec2 q = p + ivec2(x, y);
                if (!inside(q))
                    continue;
                float candidate_depth = depth_at(q);
                if (candidate_depth >= 1.0)
                    continue;
                vec3 candidate_position = position_from_depth(q, candidate_depth);
                float plane_error = abs(dot(sample_p - candidate_position, normal_at(q)));
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

// A negative factor marks a pixel requiring the original per-sample path.
#if defined(AO_CLASSIFY_SURFACE) || defined(AO_COMPOSITE_FAST) || defined(AO_COMPOSITE_EDGES)
uniform sampler2D composite_factor_texture;
#endif
#ifdef AO_CLASSIFY_SURFACE
uniform int sample_count;
uniform vec2 sample_positions[8];
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float center_depth = depth_at(p);
    vec3 P = center_depth < 1.0 ? position_from_depth(p, center_depth) : vec3(0);
    vec3 N = center_depth < 1.0 ? normal_at(p) : vec3(0);
    bool all_background = true;
    bool all_direct = center_depth < 1.0;
    for (int i = 0; i < 8; ++i) {
        if (i >= sample_count) break;
        float d = texelFetch(sample_depth_texture, p, i).r;
        if (d >= 1.0) { all_direct = false; continue; }
        all_background = false;
        if (!all_direct) continue;
        vec2 uv = (vec2(p) + sample_positions[i]) / vec2(full_size);
        vec4 h = inv_projection * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1);
        vec3 sample_p = h.xyz / h.w;
        float error = abs(dot(sample_p - P, N));
        // Require margin below the direct-match threshold; ambiguous pixels use the old path.
        if (error > footprint(sample_p) * 0.49) all_direct = false;
    }
    if (all_background) { out_color = vec4(1); return; }
    if (!all_direct) { out_color = vec4(-1); return; }
    float visibility = clamp(texelFetch(ao_texture, p, 0).r, 0.0, 1.0);
    if (visibility >= 1.0 || ao_strength <= 0.0) { out_color = vec4(1); return; }
    float ao = intensity == 1.0 ? visibility : pow(visibility, intensity);
    float z = view_z(p, center_depth);
    float pixel_size = footprint(vec3(0, 0, z));
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
    out_color = vec4(mix(1.0, ao, confidence * clamp(ao_strength, 0.0, 1.0)));
}
#elif defined(AO_COMPOSITE_FAST)
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) - viewport_origin;
    float factor = texelFetch(composite_factor_texture, p, 0).r;
    if (factor < 0.0) discard;
    out_color = vec4(vec3(factor), 1);
}
#else
void main()
{
    ivec2 p               = ivec2(gl_FragCoord.xy) - viewport_origin;
#ifdef AO_COMPOSITE_EDGES
    // Diagnostic exports always execute the original per-sample equations.
    if (!capture_sample && debug_view == 0 && texelFetch(composite_factor_texture, p, 0).r >= 0.0) discard;
#endif
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
    int status;
    p = match_surface(p, sample_p, status);
    if (capture_sample && debug_view == 5) {
        // RGBA32F export: actual depth, chosen pixel X/Y, status. Coordinates use
        // OpenGL's bottom-left origin; status 0=original, 1=search, 2=reject, 3=sky.
        out_color = vec4(sample_depth, vec2(p), float(status));
        return;
    }
    float center_depth = depth_at(p);
    if (status == 2 || center_depth >= 1.0) {
        out_color = capture_sample && debug_view == 6 ? vec4(1, 0, 1, 1) : vec4(1);
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
    if (debug_view == 0 && !capture_sample && (visibility >= 1.0 || ao_strength <= 0.0)) {
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
    float factor = mix(1.0, ao, confidence * clamp(ao_strength, 0.0, 1.0));
    // One-shot export, using the same sampled surface and arithmetic as presentation.
    if (capture_sample && debug_view == 6) {
        out_color = vec4(visibility, confidence, factor, 1);
        return;
    }
    out_color = vec4(vec3(factor), 1);
}

#endif
