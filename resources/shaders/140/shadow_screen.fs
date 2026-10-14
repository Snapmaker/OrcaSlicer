#version 140

// Copy of the scene depth after the opaque objects were drawn (background = 1.0)
uniform sampler2D scene_depth;
// Same light depth texture bound twice: raw depth for blocker search, compare mode for hardware PCF
uniform sampler2D shadow_depth;
uniform sampler2DShadow shadow_depth_cmp;
uniform mat4 inv_view_projection;
uniform mat4 inv_projection;
uniform vec3 to_camera;
// Full resolution pixels per output pixel (1 = full resolution, 2 = half resolution)
uniform int resolution_scale;
uniform mat4 light_view_projection;
// Inverse transpose of the light view projection 3x3, maps world normals to light (u, v, depth) space
uniform mat3 light_normal_matrix;
// Clamp of the receiver depth gradient (depth per uv), avoids huge offsets on grazing surfaces
uniform float max_receiver_slope;
uniform vec3 to_light;
uniform float depth_span;
uniform vec2 light_extent;
uniform float light_spread;
uniform float min_penumbra;
uniform float max_search;
uniform float depth_bias;
uniform float normal_offset;
// Extra normal offset per unit of view depth, covers the error of reconstructing positions from depth
uniform float reconstruct_offset;
uniform float shadow_strength;
uniform int blocker_samples;
uniform int pcf_samples;
// Coarse map of the local caster top; the search radius follows nearby casters, not the tallest object of the scene
uniform sampler2D local_top;
uniform bool use_local_top;

// R = shadow amount, G = linear view depth, BA = octahedral normal, used by the composite filter
out vec4 frag_color;

const float GOLDEN_ANGLE = 2.39996323;
const float TWO_PI = 6.28318531;
// Squared radius of the inner blocker ring (half of the search radius)
const float INNER_RING_R2 = 0.25;
ivec2 g_depth_size;

float FetchDepth(ivec2 c)
{
    return texelFetch(scene_depth, clamp(c, ivec2(0), g_depth_size - ivec2(1)), 0).r;
}

vec3 WorldFromDepth(ivec2 c, float d)
{
    vec2 uv = (vec2(c) + 0.5) / vec2(g_depth_size);
    vec4 p = inv_view_projection * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

float LinearDepth(float d)
{
    vec4 v = inv_projection * vec4(0.0, 0.0, d * 2.0 - 1.0, 1.0);
    return -v.z / v.w;
}

// Octahedral normal encoding into two components
vec2 OctEncode(vec3 n)
{
    vec3 m = n / (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = m.xy;
    if (m.z < 0.0)
        e = (vec2(1.0) - abs(m.yx)) * vec2(m.x >= 0.0 ? 1.0 : -1.0, m.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

// Closest-to-light depth of the casters around uv, from the coarse local top map (3x3 tiles)
float LocalTop(vec2 uv)
{
    if (!use_local_top)
        return 0.0;

    ivec2 size = textureSize(local_top, 0);
    ivec2 c = ivec2(floor(uv * vec2(size)));
    float top = 1.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i)
            top = min(top, texelFetch(local_top, clamp(c + ivec2(i, j), ivec2(0), size - ivec2(1)), 0).r);
    }
    return top;
}

vec2 VogelDisk(int i, int n, float phi)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * GOLDEN_ANGLE + phi;
    return r * vec2(cos(theta), sin(theta));
}

// Picks the neighbour with the smaller depth step on each axis, so silhouettes do not bend the normal
vec3 ReconstructNormal(ivec2 c, float d, vec3 pos)
{
    ivec2 cx = ivec2(1, 0);
    ivec2 cy = ivec2(0, 1);
    float dl = FetchDepth(c - cx);
    float dr = FetchDepth(c + cx);
    float db = FetchDepth(c - cy);
    float dt = FetchDepth(c + cy);
    vec3 ddx = (abs(dr - d) < abs(d - dl)) ? WorldFromDepth(c + cx, dr) - pos : pos - WorldFromDepth(c - cx, dl);
    vec3 ddy = (abs(dt - d) < abs(d - db)) ? WorldFromDepth(c + cy, dt) - pos : pos - WorldFromDepth(c - cy, db);
    vec3 n = normalize(cross(ddx, ddy));
    return (dot(n, to_camera) < 0.0) ? -n : n;
}

float ShadowVisibility(vec3 pos, vec3 n, float view_depth)
{
    float offset = normal_offset + reconstruct_offset * view_depth;
    vec4 lp = light_view_projection * vec4(pos + n * offset, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0))))
        return 1.0;

    // Receiver plane depth bias: each tap is compared against the receiver depth at that tap,
    // so curved or slanted receivers do not shadow themselves inside large filter kernels
    vec3 nl = light_normal_matrix * n;
    vec2 slope = (abs(nl.z) > 1.0e-5) ? -nl.xy / nl.z : vec2(0.0);
    slope = clamp(slope, vec2(-max_receiver_slope), vec2(max_receiver_slope));

    float receiver = p.z - depth_bias;
    float phi = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) * TWO_PI;

    // Step 1: weighted blocker search. Fix: at least min_penumbra wide, so the search covers the PCF region
    // and the early outs below agree with it (a tiny search left hard, stair-stepped edges on top faces)
    vec2 search_uv = vec2(clamp((p.z - LocalTop(p.xy)) * depth_span * light_spread, min_penumbra, max_search)) / light_extent;
    // Inner ring blockers take priority, keeping contact shadows hard (same as the ground mask)
    float blocker_sum = 0.0;
    float blocker_weight = 0.0;
    float inner_sum = 0.0;
    float inner_weight = 0.0;
    int blocker_count = 0;
    for (int i = 0; i < blocker_samples; ++i) {
        vec2 o = VogelDisk(i, blocker_samples, phi) * search_uv;
        float bd = texture(shadow_depth, p.xy + o).r;
        if (bd < receiver + dot(slope, o)) {
            float r2 = (float(i) + 0.5) / float(blocker_samples);
            float w = 1.0 - r2;
            blocker_sum += bd * w;
            blocker_weight += w;
            if (r2 < INNER_RING_R2) {
                inner_sum += bd * w;
                inner_weight += w;
            }
            ++blocker_count;
        }
    }
    if (blocker_count == 0)
        return 1.0;
    if (blocker_count == blocker_samples)
        return 0.0;

    // Step 2: penumbra estimation; Step 3: PCF
    float avg_blocker = (inner_weight > 0.0) ? inner_sum / inner_weight : blocker_sum / blocker_weight;
    float penumbra = clamp((p.z - avg_blocker) * depth_span * light_spread, min_penumbra, max_search);
    vec2 filter_uv = vec2(penumbra) / light_extent;
    float lit = 0.0;
    for (int i = 0; i < pcf_samples; ++i) {
        vec2 o = VogelDisk(i, pcf_samples, phi) * filter_uv;
        lit += texture(shadow_depth_cmp, vec3(p.xy + o, receiver + dot(slope, o)));
    }
    return lit / float(pcf_samples);
}

void main()
{
    g_depth_size = textureSize(scene_depth, 0);
    // Exact full resolution pixel of this output pixel; sampling at block corners made rows flip
    ivec2 c = min(ivec2(gl_FragCoord.xy) * resolution_scale, g_depth_size - ivec2(1));
    float d = FetchDepth(c);
    // Background keeps the cleared value (G = 0 marks "no object")
    if (d >= 1.0)
        discard;

    float view_depth = LinearDepth(d);
    vec3 pos = WorldFromDepth(c, d);
    vec3 n = ReconstructNormal(c, d, pos);
    // Only surfaces facing the shadow light receive, keeping back sides lit by the camera light
    float facing = smoothstep(0.05, 0.3, dot(n, to_light));
    float shadow = 0.0;
    if (facing > 0.0)
        shadow = (1.0 - ShadowVisibility(pos, n, view_depth)) * facing * shadow_strength;

    frag_color = vec4(shadow, view_depth, OctEncode(n));
}
