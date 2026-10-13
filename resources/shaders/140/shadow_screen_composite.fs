#version 140

// Full resolution scene depth (background = 1.0)
uniform sampler2D scene_depth;
// Possibly lower resolution result: R = shadow amount, G = linear view depth (0 = no object), BA = octahedral normal
uniform sampler2D object_shadow;
uniform mat4 inv_projection;
uniform mat4 inv_view_projection;
uniform vec3 to_camera;
// Full resolution pixels per shadow pixel (1 or 2)
uniform int resolution_scale;
// Neighbours whose depth differs more than this fraction are ignored, so shadows do not bleed over silhouettes
uniform float depth_tolerance;

in vec2 tex_coord;

out vec4 frag_color;

// Gaussian falloff in shadow pixels; 3x3 taps remove isolated acne pixels and soften noise
const float BLUR_SIGMA = 1.0;
// Normal agreement range: samples from another face (e.g. across a cube edge) are dropped
const float NORMAL_REJECT_LOW = 0.8;
const float NORMAL_REJECT_HIGH = 0.95;
// Relative slope change between both sides of a pixel that marks a crease. Fix: 0.1 let 24-bit depth steps
// on planes pass as creases and made spike shadows wavy; real creases change the slope several times over
const float CREASE_TOLERANCE = 0.5;
// Margin above the quantization noise of 24-bit depth in a second difference
const float CREASE_EPSILON = 16.0 / 16777216.0;
// Marks an axis without a crease near the pixel
const float NO_CROSSING = 1.0e6;
// Crease crossings farther than this (in pixels) are ignored
const float MAX_CROSSING = 2.0;
// Shadow amounts below this leave the pixel untouched
const float MIN_SHADOW = 0.004;

ivec2 g_depth_size;

// Depths at -2, -1, +1, +2 pixels along x (l, r) and y (b, t); the outer ring is read on edge pixels only
struct DepthCross
{
    float l2;
    float l1;
    float r1;
    float r2;
    float b2;
    float b1;
    float t1;
    float t2;
};

float FetchDepth(ivec2 c)
{
    return texelFetch(scene_depth, clamp(c, ivec2(0), g_depth_size - ivec2(1)), 0).r;
}

DepthCross FetchInner(ivec2 c)
{
    DepthCross k;
    k.l1 = FetchDepth(c - ivec2(1, 0));
    k.r1 = FetchDepth(c + ivec2(1, 0));
    k.b1 = FetchDepth(c - ivec2(0, 1));
    k.t1 = FetchDepth(c + ivec2(0, 1));
    k.l2 = k.l1;
    k.r2 = k.r1;
    k.b2 = k.b1;
    k.t2 = k.t1;
    return k;
}

void FetchOuter(ivec2 c, inout DepthCross k)
{
    k.l2 = FetchDepth(c - ivec2(2, 0));
    k.r2 = FetchDepth(c + ivec2(2, 0));
    k.b2 = FetchDepth(c - ivec2(0, 2));
    k.t2 = FetchDepth(c + ivec2(0, 2));
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

// Picks one side per axis: the smaller depth step, or at creases the side whose two-tap extrapolation
// best predicts d, so the faces of a crease are not mixed into one normal
vec3 ReconstructNormal(ivec2 c, float d, DepthCross k, bool two_tap)
{
    vec3 pos = WorldFromDepth(c, d);
    ivec2 cx = ivec2(1, 0);
    ivec2 cy = ivec2(0, 1);
    bool right = two_tap ? abs(2.0 * k.r1 - k.r2 - d) < abs(2.0 * k.l1 - k.l2 - d) : abs(k.r1 - d) < abs(d - k.l1);
    bool top = two_tap ? abs(2.0 * k.t1 - k.t2 - d) < abs(2.0 * k.b1 - k.b2 - d) : abs(k.t1 - d) < abs(d - k.b1);
    vec3 ddx = right ? WorldFromDepth(c + cx, k.r1) - pos : pos - WorldFromDepth(c - cx, k.l1);
    vec3 ddy = top ? WorldFromDepth(c + cy, k.t1) - pos : pos - WorldFromDepth(c - cy, k.b1);
    vec3 n = normalize(cross(ddx, ddy));
    return (dot(n, to_camera) < 0.0) ? -n : n;
}

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (vec2(1.0) - abs(e.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// Window depth is affine across a plane, so a slope change between both neighbours marks a crease
bool IsCrease(float a, float d, float b)
{
    if (a >= 1.0 || b >= 1.0)
        return false;
    float s1 = d - a;
    float s2 = b - d;
    return abs(s2 - s1) > CREASE_TOLERANCE * max(abs(s1), abs(s2)) + CREASE_EPSILON;
}

// Signed position (pixels from the centre) where the planes of both sides of one axis meet, or NO_CROSSING.
// Each side plane is extrapolated from its two taps.
float CreaseCrossing(float a2, float a1, float d, float b1, float b2)
{
    if (a1 >= 1.0 || a2 >= 1.0 || b1 >= 1.0 || b2 >= 1.0)
        return NO_CROSSING;

    float sa = a1 - a2;
    float sb = b2 - b1;
    float slope_change = sb - sa;
    if (abs(slope_change) <= CREASE_TOLERANCE * max(abs(sa), abs(sb)) + CREASE_EPSILON)
        return NO_CROSSING;

    float a0 = a1 + sa;
    float b0 = b1 - sb;
    float x = (a0 - b0) / slope_change;
    // The centre lies on the face it predicts best, so the crease must lie towards the other face
    bool on_a = abs(a0 - d) <= abs(b0 - d);
    if ((on_a && x <= 0.0) || (!on_a && x >= 0.0) || abs(x) > MAX_CROSSING)
        return NO_CROSSING;
    return x;
}

// Shadow pixel nearest to full resolution pixel c (shadow pixel i was evaluated at pixel i * resolution_scale)
ivec2 ShadowCenter(ivec2 c)
{
    return ivec2(floor(vec2(c) / float(resolution_scale) + 0.5));
}

// Reads the 3x3 shadow pixels around full resolution pixel c and returns their largest shadow
float FetchShadowBlock(ivec2 c, out vec4 block[9])
{
    ivec2 size = textureSize(object_shadow, 0);
    ivec2 center = ShadowCenter(c);
    float top = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec4 s = texelFetch(object_shadow, clamp(center + ivec2(i, j), ivec2(0), size - ivec2(1)), 0);
            block[(j + 1) * 3 + i + 1] = s;
            top = max(top, s.r);
        }
    }
    return top;
}

// Depth and normal aware 3x3 Gaussian of the shadow pixels; falls back to the best matching neighbour
float FilterShadow(ivec2 c, vec4 block[9], float view_depth, vec3 n)
{
    ivec2 size = textureSize(object_shadow, 0);
    vec2 p = vec2(c) / float(resolution_scale);
    ivec2 center = ShadowCenter(c);
    float inv_two_sigma2 = 1.0 / (2.0 * BLUR_SIGMA * BLUR_SIGMA);

    float sum = 0.0;
    float weight_sum = 0.0;
    float best_score = -1.0e30;
    float best_shadow = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            ivec2 s_coord = clamp(center + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec4 s = block[(j + 1) * 3 + i + 1];
            if (s.g <= 0.0)
                continue;

            float rel_diff = abs(s.g - view_depth) / view_depth;
            float normal_dot = dot(n, OctDecode(s.ba));
            float score = normal_dot - rel_diff / depth_tolerance;
            if (score > best_score) {
                best_score = score;
                best_shadow = s.r;
            }
            if (rel_diff <= depth_tolerance) {
                vec2 delta = vec2(s_coord) - p;
                float normal_weight = smoothstep(NORMAL_REJECT_LOW, NORMAL_REJECT_HIGH, normal_dot);
                float w = exp(-dot(delta, delta) * inv_two_sigma2) * normal_weight;
                sum += s.r * w;
                weight_sum += w;
            }
        }
    }
    return (weight_sum > 1.0e-4) ? sum / weight_sum : best_shadow;
}

void main()
{
    g_depth_size = textureSize(scene_depth, 0);
    ivec2 c = clamp(ivec2(tex_coord * vec2(g_depth_size)), ivec2(0), g_depth_size - ivec2(1));
    float d = FetchDepth(c);
    bool silhouette = d >= 1.0;
    if (silhouette) {
        // Fix for white dots on object/background silhouettes: such a pixel may still hold object samples, so it
        // takes the shadow of the nearest object neighbour; the plates drawn afterwards cover the background again
        ivec2 neighbours[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
        ivec2 nearest = c;
        for (int i = 0; i < 4; ++i) {
            float nd = FetchDepth(c + neighbours[i]);
            if (nd < d) {
                d = nd;
                nearest = clamp(c + neighbours[i], ivec2(0), g_depth_size - ivec2(1));
            }
        }
        if (d >= 1.0)
            discard;
        c = nearest;
    }

    // Performance: most pixels lie outside any shadow, so skip the normal reconstruction and filtering there
    vec4 block[9];
    if (FetchShadowBlock(c, block) < MIN_SHADOW)
        discard;

    // Cheap test on the four neighbours; only crease pixels pay for the outer ring and the blend
    DepthCross k = FetchInner(c);
    bool crease = !silhouette && (IsCrease(k.l1, d, k.r1) || IsCrease(k.b1, d, k.t1));
    if (crease)
        FetchOuter(c, k);
    float shadow = FilterShadow(c, block, LinearDepth(d), ReconstructNormal(c, d, k, crease));

    // Fix for dotted lines along creases: MSAA leaves two faces in such pixels but only one factor is written,
    // so blend the shadows of both faces by the face coverage estimated from the plane intersection
    if (crease) {
        float x = CreaseCrossing(k.l2, k.l1, d, k.r1, k.r2);
        float y = CreaseCrossing(k.b2, k.b1, d, k.t1, k.t2);
        float ax = abs(x);
        float ay = abs(y);
        // Distance from the centre to the crease line through both axis crossings
        float dist = (ax < NO_CROSSING && ay < NO_CROSSING) ? ax * ay / sqrt(ax * ax + ay * ay) : min(ax, ay);
        if (dist < 0.5) {
            ivec2 o = c + ivec2(ax <= 1.5 ? int(sign(x)) : 0, ay <= 1.5 ? int(sign(y)) : 0);
            float od = FetchDepth(o);
            if (od < 1.0 && o != c) {
                DepthCross ok = FetchInner(o);
                FetchOuter(o, ok);
                vec4 other_block[9];
                FetchShadowBlock(o, other_block);
                float other = FilterShadow(o, other_block, LinearDepth(od), ReconstructNormal(o, od, ok, true));
                shadow = mix(other, shadow, 0.5 + dist);
            }
        }
    }

    if (shadow < MIN_SHADOW)
        discard;

    // Multiplied into the frame buffer (blend DST_COLOR, ZERO)
    frag_color = vec4(vec3(1.0 - shadow), 1.0);
}
