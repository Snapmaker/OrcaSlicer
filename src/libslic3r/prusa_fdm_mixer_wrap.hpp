// First-party convenience wrappers around the vendored prusa-fdm-mixer model.
// Compiled ONLY when ENABLE_PRUSA_FDM_MIXER_DEMO is on (Technologies.hpp).
// When the macro is off this header is empty — safe to include unconditionally.
//
// These are NOT a model dispatch: they always call prusa. The legacy code path
// stays in place verbatim under #else at each call site (see MixedFilament.cpp,
// MixedGradientSelector.cpp, MixedFilamentColorMapPanel.cpp). Removal = delete
// this header + the #if blocks + the macro. See plan: encapsulated-frolicking-zephyr.md.
#pragma once

#if ENABLE_PRUSA_FDM_MIXER_DEMO

#include "libslic3r/prusa_fdm_mixer.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// Predict the apparent mixed colour via prusa-fdm-mixer, returning sRGB
// channels in [0,255]. parts: (hex "#rrggbb", ratio in [0,1]); ratios should
// sum to ~1. Returns false (outputs untouched) for an empty/all-zero part list,
// so callers can fall back to a sentinel colour.
inline bool prusa_mix_rgb(const std::vector<std::pair<std::string, double>>& parts,
                          unsigned char& out_r, unsigned char& out_g, unsigned char& out_b)
{
    std::vector<prusa_fdm_mixer::Part> prusa_parts;
    prusa_parts.reserve(parts.size());
    for (const auto& p : parts) {
        if (p.second > 0.0)
            prusa_parts.push_back({ p.first, p.second });
    }
    if (prusa_parts.empty())
        return false;
    const prusa_fdm_mixer::RGB rgb = prusa_fdm_mixer::mix_rgb(prusa_parts);
    out_r = rgb.r;
    out_g = rgb.g;
    out_b = rgb.b;
    return true;
}

} // namespace Slic3r

#endif // ENABLE_PRUSA_FDM_MIXER_DEMO
