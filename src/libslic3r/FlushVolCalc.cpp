#include <cmath>
#include <algorithm>
#include <assert.h>
#include "slic3r/Utils/ColorSpaceConvert.hpp"

#include "FlushVolCalc.hpp"


namespace Slic3r {

const int g_min_flush_volume_from_support = 420.f;
const int g_flush_volume_to_support = 230;

const int g_max_flush_volume = 350;

static float to_radians(float degree)
{
    return degree / 180.f * M_PI;
}


static float get_luminance(float r, float g, float b)
{
    return r * 0.3 + g * 0.59 + b * 0.11;
}

static float calc_triangle_3rd_edge(float edge_a, float edge_b, float degree_ab)
{
    return std::sqrt(edge_a * edge_a + edge_b * edge_b - 2 * edge_a * edge_b * std::cos(to_radians(degree_ab)));
}

static float smoothstep(float edge0, float edge1, float x)
{
    if (edge0 == edge1)
        return x < edge0 ? 0.f : 1.f;

    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

static float normalize_hue(float hue)
{
    hue = std::fmod(hue, 360.f);
    return hue < 0.f ? hue + 360.f : hue;
}

static float DeltaHS_BBS(float h1, float s1, float v1, float h2, float s2, float v2)
{
    float h1_rad = to_radians(h1);
    float h2_rad = to_radians(h2);

    float dx = std::cos(h1_rad) * s1 * v1 - cos(h2_rad) * s2 * v2;
    float dy = std::sin(h1_rad) * s1 * v1 - sin(h2_rad) * s2 * v2;
    float dxy = std::sqrt(dx * dx + dy * dy);
    return std::min(0.8f, dxy);
}

FlushVolCalculator::FlushVolCalculator(int min, int max, float multiplier)
    :m_min_flush_vol(min), m_max_flush_vol(max), m_multiplier(multiplier)
{
}

int FlushVolCalculator::calc_flush_vol(unsigned char src_a, unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_a, unsigned char dst_r, unsigned char dst_g, unsigned char dst_b)
{
    // BBS: Transparent materials are treated as white materials
    if (src_a == 0) {
        src_r = src_g = src_b = 255;
    }
    if (dst_a == 0) {
        dst_r = dst_g = dst_b = 255;
    }

    float src_r_f, src_g_f, src_b_f, dst_r_f, dst_g_f, dst_b_f;
    float from_hsv_h, from_hsv_s, from_hsv_v;
    float to_hsv_h, to_hsv_s, to_hsv_v;

    src_r_f = (float)src_r / 255.f;
    src_g_f = (float)src_g / 255.f;
    src_b_f = (float)src_b / 255.f;
    dst_r_f = (float)dst_r / 255.f;
    dst_g_f = (float)dst_g / 255.f;
    dst_b_f = (float)dst_b / 255.f;

    // Calculate color distance in HSV color space
    RGB2HSV(src_r_f, src_g_f,src_b_f, &from_hsv_h, &from_hsv_s, &from_hsv_v);
    RGB2HSV(dst_r_f, dst_g_f, dst_b_f, &to_hsv_h, &to_hsv_s, &to_hsv_v);
    from_hsv_h = normalize_hue(from_hsv_h);
    to_hsv_h = normalize_hue(to_hsv_h);
    float hs_dist = DeltaHS_BBS(from_hsv_h, from_hsv_s, from_hsv_v, to_hsv_h, to_hsv_s, to_hsv_v);

    // 1. Color difference is more obvious if the dest color has high luminance
    // 2. Color difference is more obvious if the source color has low luminance
    float from_lumi = get_luminance(src_r_f, src_g_f, src_b_f);
    float to_lumi = get_luminance(dst_r_f, dst_g_f, dst_b_f);
    float lumi_flush = 0.f;
    if (to_lumi >= from_lumi) {
        lumi_flush = std::pow(to_lumi - from_lumi, 1.12f) * 150.f;
    }
    else {
        lumi_flush = (from_lumi - to_lumi) * 8.f;

        float inter_hsv_v = 0.92f * to_hsv_v + 0.08f * from_hsv_v;
        hs_dist = std::min(inter_hsv_v, hs_dist);
    }
    float hs_flush = 55.f * hs_dist;

    float flush_volume = calc_triangle_3rd_edge(hs_flush, lumi_flush, 115.f);

    // Targeted residue compensation for measured high-risk transitions: saturated colors into light neutral targets and red into neutral midtones.
    const float src_chroma = from_hsv_s * from_hsv_v;
    const float dst_chroma = to_hsv_s * to_hsv_v;
    const float dst_rgb_spread = std::max(dst_r_f, std::max(dst_g_f, dst_b_f)) - std::min(dst_r_f, std::min(dst_g_f, dst_b_f));
    const float neutral_target = 1.f - (std::max(dst_r_f, std::max(dst_g_f, dst_b_f)) - std::min(dst_r_f, std::min(dst_g_f, dst_b_f)));
    const float cool_target = std::max(0.f, dst_b_f - dst_r_f);
    const float lumi_gap = to_lumi - from_lumi;
    const float white_risk = smoothstep(0.70f, 0.86f, to_lumi) *
                             smoothstep(0.86f, 0.96f, neutral_target) *
                             (1.f + 0.45f * smoothstep(0.00f, 0.06f, cool_target)) *
                             (1.f - 0.75f * smoothstep(0.46f, 0.62f, from_lumi)) *
                             smoothstep(0.45f, 0.75f, from_hsv_s) *
                             smoothstep(0.32f, 0.80f, src_chroma) *
                             smoothstep(0.34f, 0.58f, lumi_gap) *
                             (1.f - 0.45f * smoothstep(0.25f, 0.55f, to_hsv_s));

    const float red_hue = std::max(std::max(smoothstep(340.f, 360.f, from_hsv_h), smoothstep(0.f, 20.f, 20.f - from_hsv_h)),
                                   0.55f * smoothstep(300.f, 340.f, from_hsv_h));
    const float red_residue = red_hue *
                              smoothstep(0.75f, 0.95f, from_hsv_s) *
                              smoothstep(0.65f, 0.90f, from_hsv_v) *
                              (1.f - 0.70f * smoothstep(0.48f, 0.62f, from_lumi)) *
                              smoothstep(0.08f, 0.35f, lumi_gap) *
                              smoothstep(0.50f, 0.82f, to_lumi) *
                              (1.f - 0.25f * smoothstep(0.35f, 0.65f, to_hsv_s));
    const float gray_residue = red_hue *
                               smoothstep(0.75f, 0.95f, from_hsv_s) *
                               smoothstep(0.65f, 0.90f, from_hsv_v) *
                               smoothstep(0.48f, 0.60f, to_lumi) *
                               (1.f - smoothstep(0.72f, 0.84f, to_lumi)) *
                               smoothstep(0.70f, 0.92f, neutral_target) *
                               (1.f - 0.85f * smoothstep(0.30f, 0.58f, to_hsv_s));

    flush_volume += 210.f * white_risk * std::pow(std::max(0.f, lumi_gap + 0.18f), 0.65f);
    flush_volume += 145.f * red_residue;
    flush_volume += 180.f * gray_residue;

    // Generic HSV/RGB stain risk for high-chroma sources into bright neutral targets, damped once existing compensation is already high.
    const float warm_or_pink_target = std::max(smoothstep(330.f, 360.f, to_hsv_h), smoothstep(0.f, 70.f, 70.f - to_hsv_h));
    const float warm_pastel = smoothstep(0.70f, 0.92f, to_lumi) *
                              smoothstep(0.08f, 0.20f, to_hsv_s) *
                              warm_or_pink_target;
    const float stain_risk = smoothstep(0.30f, 0.55f, src_chroma) *
                             smoothstep(0.32f, 0.48f, src_chroma - dst_chroma) *
                             smoothstep(0.74f, 0.88f, to_lumi) *
                             (1.f - smoothstep(0.04f, 0.12f, dst_rgb_spread)) *
                             std::max(0.f, 1.f - 0.45f * warm_pastel);
    const float existing_flush_damp = 1.f - smoothstep(180.f, 260.f, flush_volume);
    const float high_flush_activation = std::max(smoothstep(40.f, 125.f, flush_volume), 0.75f * stain_risk) * existing_flush_damp;
    flush_volume *= 1.f + 0.70f * high_flush_activation * stain_risk;
    flush_volume = std::max(flush_volume, 32.f);

    //float flush_multiplier = std::atof(m_flush_multiplier_ebox->GetValue().c_str());
    // flush_volume += m_min_flush_vol;
    return std::min((int)flush_volume, m_max_flush_vol);
}

}
