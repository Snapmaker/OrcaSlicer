#ifndef slic3r_FlushVolHighFlow_hpp_
#define slic3r_FlushVolHighFlow_hpp_

#include <cmath>
#include <algorithm>
#include "slic3r/Utils/ColorSpaceConvert.hpp"

// HighFlow flush-volume K compensation (v6 calibration).
// HighFlow-only path; StandardFlow (classify_color + get_special_k) is unchanged.
// calc_flush_vol() rounds (raw*k) for HighFlow; Standard truncates.

namespace Slic3r {

// Standard 9 color types + CoolWhite (split from PearlWhite) + BlueGray + Green.
enum class HFColorType {
    Red,
    PearlWhite,
    CoolWhite,   // cool-shifted near-white
    ColdWhite,
    LightGray,
    MidGray,
    DarkGray,
    Black,
    DarkColor,
    BlueGray,    // low-saturation blue-grey
    Green,       // green / yellow-green
    Normal
};

inline bool hf_is_white(HFColorType t)
{
    return t == HFColorType::PearlWhite || t == HFColorType::ColdWhite || t == HFColorType::CoolWhite;
}

// HighFlow classifier (standard classify_color is unchanged).
inline HFColorType classify_color_highflow(unsigned char r, unsigned char g, unsigned char b)
{
    float rf = r / 255.f;
    float gf = g / 255.f;
    float bf = b / 255.f;

    float h, s, v;
    RGB2HSV(rf, gf, bf, &h, &s, &v);

    float S_pct = s * 100.f;
    float V_pct = v * 100.f;

    if (V_pct < 15.f)
        return HFColorType::Black;

    if (S_pct <= 12.f) {
        if (V_pct >= 85.f) {
            int dBR = (int) b - (int) r;
            int dRG = std::abs((int) r - (int) g);
            int dGB = std::abs((int) g - (int) b);
            if (dBR >= 5)
                return HFColorType::CoolWhite;
            if (dBR >= 12 && (int) b - (int) g >= 10)
                return HFColorType::ColdWhite;
            if (dRG <= 8 && dGB <= 8)
                return HFColorType::PearlWhite;
            return HFColorType::Normal;
        }
        if (V_pct >= 70.f && V_pct <= 88.f && std::abs((int)r-(int)g) <= 10 && std::abs((int)g-(int)b) <= 10)
            return HFColorType::LightGray;
        if (V_pct >= 45.f && V_pct <= 69.f && std::abs((int)r-(int)g) <= 10 && std::abs((int)g-(int)b) <= 10)
            return HFColorType::MidGray;
        if (V_pct >= 15.f && V_pct <= 44.f && std::abs((int)r-(int)g) <= 10 && std::abs((int)g-(int)b) <= 10)
            return HFColorType::DarkGray;
        return HFColorType::Normal;
    }

    if (S_pct <= 40.f && h >= 185.f && h <= 230.f && V_pct >= 65.f && V_pct <= 90.f)
        return HFColorType::BlueGray;
    if (S_pct > 40.f && h >= 60.f && h <= 160.f && V_pct >= 50.f)
        return HFColorType::Green;
    if (V_pct >= 20.f && S_pct >= 40.f && ((h >= 0.f && h <= 15.f) || (h >= 345.f && h <= 360.f))
        && (int) r - (int) g >= 100 && (int) r - (int) b >= 100)
        return HFColorType::Red;
    if (V_pct < 50.f)
        return HFColorType::DarkColor;
    return HFColorType::Normal;
}

// HighFlow K parameters (v6 calibration).
struct HighFlowK {
    float red_white;        // Red src -> white/light
    float red_mg_dc;        // Red src -> MidGray/DarkColor/BlueGray
    float red_else;         // Red src -> other
    float dst_red;          // -> Red
    float pw_dst_dark;      // DarkGray/Black/DarkColor -> white
    float pw_dst_light;     // CoolWhite/ColdWhite/LightGray -> white
    float pw_dst_else;      // other -> white
    float green_to_white;   // Green src -> white
    float pw_src;           // PearlWhite src
    float pw_same;          // same white-type switch
    float same_other;       // other same-type switch
    float dst_mg;           // non-white -> MidGray
    float cw_src;           // CoolWhite src
    float cw_to_mg;         // CoolWhite -> MidGray
    float bg_dst;           // -> BlueGray
    float k_cap;            // clamp ceiling
};

static const HighFlowK s_hf_k = {
    2.33f,   // red_white
    1.37f,   // red_mg_dc
    1.39f,   // red_else
    0.77f,   // dst_red
    1.32f,   // pw_dst_dark
    1.79f,   // pw_dst_light
    1.42f,   // pw_dst_else
    1.70f,   // green_to_white
    0.53f,   // pw_src
    1.15f,   // pw_same
    0.30f,   // same_other
    1.25f,   // dst_mg
    1.17f,   // cw_src
    2.95f,   // cw_to_mg
    1.07f,   // bg_dst
    3.32f    // k_cap
};

// HighFlow K rules (standard get_special_k is unchanged).
inline float get_special_k_highflow(unsigned char src_r, unsigned char src_g, unsigned char src_b,
                                    unsigned char dst_r, unsigned char dst_g, unsigned char dst_b)
{
    HFColorType src_type = classify_color_highflow(src_r, src_g, src_b);
    HFColorType dst_type = classify_color_highflow(dst_r, dst_g, dst_b);
    const HighFlowK &hf = s_hf_k;

    // Same special type => minimal flush
    if (src_type == dst_type && src_type != HFColorType::Normal && src_type != HFColorType::DarkColor
            && src_type != HFColorType::BlueGray && src_type != HFColorType::Green) {
        return std::clamp(hf_is_white(src_type) ? hf.pw_same : hf.same_other, 0.3f, hf.k_cap);
    }

    float k = 1.0f;

    // ---- Red correction (Red -> white uses red_white alone) ----
    if (src_type == HFColorType::Red) {
        if (dst_type == HFColorType::PearlWhite || dst_type == HFColorType::CoolWhite ||
            dst_type == HFColorType::ColdWhite || dst_type == HFColorType::LightGray) {
            k *= hf.red_white;
        } else if (dst_type == HFColorType::MidGray || dst_type == HFColorType::DarkColor ||
                   dst_type == HFColorType::BlueGray) {
            k *= hf.red_mg_dc;
        } else {
            k *= hf.red_else;
        }
    }
    if (dst_type == HFColorType::Red) {
        k *= hf.dst_red;
    }

    // ---- White destination (Green via green_to_white; Red already handled) ----
    if (dst_type == HFColorType::PearlWhite || dst_type == HFColorType::CoolWhite ||
        dst_type == HFColorType::ColdWhite) {
        if (src_type == HFColorType::Green) {
            k *= hf.green_to_white;
        } else if (src_type != HFColorType::Red) {
            if (src_type == HFColorType::DarkGray || src_type == HFColorType::Black ||
                src_type == HFColorType::DarkColor) {
                k *= hf.pw_dst_dark;
            } else if (src_type == HFColorType::CoolWhite || src_type == HFColorType::ColdWhite ||
                       src_type == HFColorType::LightGray) {
                k *= hf.pw_dst_light;
            } else {
                k *= hf.pw_dst_else;
            }
        }
    }
    if (src_type == HFColorType::PearlWhite) {
        k *= hf.pw_src;
    }
    if (src_type == HFColorType::CoolWhite) {
        k *= hf.cw_src;
    }

    // ---- CoolWhite -> MidGray / BlueGray / Gray tiered ----
    if (src_type == HFColorType::CoolWhite && dst_type == HFColorType::MidGray) {
        k *= hf.cw_to_mg;
    }
    if (dst_type == HFColorType::BlueGray) {
        k *= hf.bg_dst;
    }
    if (dst_type == HFColorType::MidGray) {
        if (!hf_is_white(src_type)) {
            k *= hf.dst_mg;
        }
    }
    if (dst_type == HFColorType::DarkGray) {
        k *= 0.9f;
    }

    return std::clamp(k, 0.3f, hf.k_cap);
}

} // namespace Slic3r

#endif // slic3r_FlushVolHighFlow_hpp_
