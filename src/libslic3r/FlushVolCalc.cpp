#include <cmath>
#include <algorithm>
#include <assert.h>
#include <string>
#include "slic3r/Utils/ColorSpaceConvert.hpp"

#include "FlushVolCalc.hpp"


namespace Slic3r {

// Per-flow flushing thresholds — user-specified (2026-07-20).
const FlushThresholds g_standard_flush_thresholds = {40, 350, 50, 100};
const FlushThresholds g_highflow_flush_thresholds   = {80, 700, 120, 250};

// ---- Special color type classification for flush correction ----

enum class SpecialColorType {
    Red,
    PearlWhite,
    ColdWhite,
    LightGray,
    MidGray,
    DarkGray,
    Black,
    DarkColor,
    Normal
};

static SpecialColorType classify_color(unsigned char r, unsigned char g, unsigned char b)
{
    float rf = r / 255.f;
    float gf = g / 255.f;
    float bf = b / 255.f;

    float h, s, v;
    RGB2HSV(rf, gf, bf, &h, &s, &v);

    float S_pct = s * 100.f;
    float V_pct = v * 100.f;

    // Black: extremely dark, regardless of saturation (V < 15)
    if (V_pct < 15.f)
        return SpecialColorType::Black;

    // Achromatic path (S <= 12, widened to capture low-saturation greys)
    if (S_pct <= 12.f) {
        if (V_pct >= 85.f) {
            int dRG = std::abs((int)r - (int)g);
            int dGB = std::abs((int)g - (int)b);
            int dBR = (int)b - (int)r;

            // Cold white first — blue shift is more specific
            if (dBR >= 12 && (int)b - (int)g >= 10)
                return SpecialColorType::ColdWhite;

            // Pearl white — balanced channels
            if (dRG <= 8 && dGB <= 8)
                return SpecialColorType::PearlWhite;

            return SpecialColorType::Normal;
        }

        if (V_pct >= 70.f && V_pct <= 88.f) {
            int dRG = std::abs((int)r - (int)g);
            int dGB = std::abs((int)g - (int)b);
            if (dRG <= 10 && dGB <= 10)
                return SpecialColorType::LightGray;
        }

        if (V_pct >= 45.f && V_pct <= 69.f) {
            int dRG = std::abs((int)r - (int)g);
            int dGB = std::abs((int)g - (int)b);
            if (dRG <= 10 && dGB <= 10)
                return SpecialColorType::MidGray;
        }

        if (V_pct >= 15.f && V_pct <= 44.f) {
            int dRG = std::abs((int)r - (int)g);
            int dGB = std::abs((int)g - (int)b);
            if (dRG <= 10 && dGB <= 10)
                return SpecialColorType::DarkGray;
        }

        return SpecialColorType::Normal;
    }

    // Chromatic path (S > 8):
    // Red — tightened hue range to exclude orange/pink/magenta
    if (V_pct >= 20.f && S_pct >= 40.f) {
        bool hue_in_red = (h >= 0.f && h <= 15.f) || (h >= 345.f && h <= 360.f);
        int r_minus_g = (int)r - (int)g;
        int r_minus_b = (int)r - (int)b;
        if (hue_in_red && r_minus_g >= 100 && r_minus_b >= 100)
            return SpecialColorType::Red;
    }

    // Dark non-special chromatic color (V < 50)
    if (V_pct < 50.f)
        return SpecialColorType::DarkColor;

    return SpecialColorType::Normal;
}

static bool is_white_class(SpecialColorType t)
{
    return t == SpecialColorType::PearlWhite || t == SpecialColorType::ColdWhite;
}

static float get_special_k(unsigned char src_r, unsigned char src_g, unsigned char src_b,
                            unsigned char dst_r, unsigned char dst_g, unsigned char dst_b)
{
    SpecialColorType src_type = classify_color(src_r, src_g, src_b);
    SpecialColorType dst_type = classify_color(dst_r, dst_g, dst_b);

    // Boundary: same special type => minimal flush
    if (src_type == dst_type && src_type != SpecialColorType::Normal && src_type != SpecialColorType::DarkColor)
        return 0.4f;

    float k = 1.0f;

    // ---- Red correction ----
    if (src_type == SpecialColorType::Red) {
        if (dst_type == SpecialColorType::PearlWhite ||
            dst_type == SpecialColorType::ColdWhite ||
            dst_type == SpecialColorType::LightGray) {
            k *= 1.9f;
        } else if (dst_type == SpecialColorType::MidGray ||
                   dst_type == SpecialColorType::DarkColor) {
            k *= 1.15f;
        } else {
            k *= 1.3f;
        }
    }
    if (dst_type == SpecialColorType::Red) {
        k *= 0.75f;
    }

    // ---- Pearl white correction ----
    if (dst_type == SpecialColorType::PearlWhite) {
        if (src_type == SpecialColorType::Red ||
            src_type == SpecialColorType::DarkGray ||
            src_type == SpecialColorType::Black ||
            src_type == SpecialColorType::DarkColor) {
            k *= 1.8f;
        } else if (src_type == SpecialColorType::ColdWhite ||
                   src_type == SpecialColorType::LightGray) {
            k *= 1.4f;
        }
        else
        {
            k *= 1.3f;
        }
    }
    if (src_type == SpecialColorType::PearlWhite) {
        k *= 0.85f;
    }

    // ---- Cold white correction ----
    if (dst_type == SpecialColorType::ColdWhite) {
        if (src_type == SpecialColorType::Red ||
            src_type == SpecialColorType::DarkGray ||
            src_type == SpecialColorType::Black ||
            src_type == SpecialColorType::DarkColor) {
            k *= 1.3f;
        }
    }

    // ---- Gray tiered correction ----
    if (dst_type == SpecialColorType::LightGray) {
        if (src_type == SpecialColorType::Red ||
            src_type == SpecialColorType::DarkGray ||
            src_type == SpecialColorType::Black) {
            k *= 1.3f;
        } else if (is_white_class(src_type)) {
            k *= 1.05f;
        }
    }
    if (dst_type == SpecialColorType::MidGray) {
        if (!is_white_class(src_type)) {
            k *= 1.15f;
        }
    }
    if (dst_type == SpecialColorType::DarkGray) {
        k *= 0.9f;
    }

    return std::clamp(k, 0.3f, 2.5f);
}

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

// ---- Formula coefficient sets (V5 abs opt calibration) ----

static const FlushFormulaParams s_normal_params = {
    0.80f,   // lumi_pow_exp
    120.f,   // lumi_pow_scale
    30.f,    // lumi_linear_scale
    0.80f,   // inter_hsv_from_w (weight of from_hsv_v)
    220.f,   // hs_scale
    125.f,   // triangle_angle (degrees)
    0.30f    // hs_cap
};

// HighFlow coefficients — calibrated against HighFlow measured matrix (2026-07-18).
// Optimization target: 实测-30 < 预测 < 实测+100 (50k random + 20k hill-climb).
static const FlushFormulaParams s_highflow_params = {
    0.617f,   // lumi_pow_exp
    339.4f,   // lumi_pow_scale
    128.0f,   // lumi_linear_scale
    0.605f,   // inter_hsv_from_w (weight of from_hsv_v)
    1163.1f,  // hs_scale
    72.8f,    // triangle_angle (degrees)
    0.147f    // hs_cap
};

static float DeltaHS_BBS(float h1, float s1, float v1, float h2, float s2, float v2, float hs_cap)
{
    float h1_rad = to_radians(h1);
    float h2_rad = to_radians(h2);

    float dx = std::cos(h1_rad) * s1 * v1 - cos(h2_rad) * s2 * v2;
    float dy = std::sin(h1_rad) * s1 * v1 - sin(h2_rad) * s2 * v2;
    float dxy = std::sqrt(dx * dx + dy * dy);
    return std::min(hs_cap, dxy);
}

FlushVolCalculator::FlushVolCalculator(int min, int max, float multiplier, int flush_dataset)
    :m_min_flush_vol(min), m_max_flush_vol(max), m_multiplier(multiplier), m_flush_dataset(flush_dataset), m_predictor(flush_dataset)
{
}

bool FlushVolCalculator::get_flush_vol_from_data(unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_r, unsigned char dst_g, unsigned char dst_b, float& flush)
{
    FlushPredict::RGBColor src(src_r, src_g, src_b);
    FlushPredict::RGBColor dst(dst_r, dst_g, dst_b);

    return m_predictor.predict(src, dst, flush);
}

// Path B: HSV color-distance formula with stain-risk compensation.
// Accepts a FlushFormulaParams to support independent calibration per flow type.
int FlushVolCalculator::calc_flush_vol_rgb(unsigned char src_r, unsigned char src_g, unsigned char src_b,
    unsigned char dst_r, unsigned char dst_g, unsigned char dst_b,
    const FlushFormulaParams& params)
{
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
    RGB2HSV(src_r_f, src_g_f, src_b_f, &from_hsv_h, &from_hsv_s, &from_hsv_v);
    RGB2HSV(dst_r_f, dst_g_f, dst_b_f, &to_hsv_h, &to_hsv_s, &to_hsv_v);
    float hs_dist = DeltaHS_BBS(from_hsv_h, from_hsv_s, from_hsv_v, to_hsv_h, to_hsv_s, to_hsv_v, params.hs_cap);

    // 1. Color difference is more obvious if the dest color has high luminance
    // 2. Color difference is more obvious if the source color has low luminance
    float from_lumi = get_luminance(src_r_f, src_g_f, src_b_f);
    float to_lumi = get_luminance(dst_r_f, dst_g_f, dst_b_f);
    float lumi_flush = 0.f;
    if (to_lumi >= from_lumi) {
        lumi_flush = std::pow(to_lumi - from_lumi, params.lumi_pow_exp) * params.lumi_pow_scale;
    }
    else {
        lumi_flush = (from_lumi - to_lumi) * params.lumi_linear_scale;

        float inter_hsv_v = (1.f - params.inter_hsv_from_w) * to_hsv_v + params.inter_hsv_from_w * from_hsv_v;
        hs_dist = std::min(inter_hsv_v, hs_dist);
    }
    float hs_flush = params.hs_scale * hs_dist;

    float flush_volume = calc_triangle_3rd_edge(hs_flush, lumi_flush, params.triangle_angle);

    return std::min((int)flush_volume, m_max_flush_vol);
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

    // Path A: always try lookup table first — lookup data is pre-calibrated, no extra correction
    float lookup_volume;
    //if (get_flush_vol_from_data(src_r, src_g, src_b, dst_r, dst_g, dst_b, lookup_volume)) {
    //    return std::min((int)lookup_volume, m_max_flush_vol);
    //}

    // Select formula coefficients: StandardFlow vs HighFlow
    const auto& params = (m_flush_dataset == static_cast<int>(FlushDataset::HighFlow))
        ? s_highflow_params : s_normal_params;

    // Lookup miss — fall through to Path B (HSV formula with stain-risk compensation)
    float flush_volume = (float)calc_flush_vol_rgb(src_r, src_g, src_b, dst_r, dst_g, dst_b, params);

    // Apply special color correction coefficient K only for Path B (red / pearl white / cold white / gray)
    float k = get_special_k(src_r, src_g, src_b, dst_r, dst_g, dst_b);
    int   final_volume = (int) ((float) flush_volume * k);

    // Per-flow clamping with flow-specific thresholds — user-specified (2026-07-20).
    const auto& thresholds = get_flush_thresholds(m_flush_dataset);
    return std::clamp(final_volume, thresholds.min_flush_volume, thresholds.max_flush_volume);
}

}
