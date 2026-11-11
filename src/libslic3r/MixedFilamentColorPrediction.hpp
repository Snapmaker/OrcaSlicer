#pragma once

#include "MixedFilament.hpp"
#include "FullSpectrumKSPairResidual.hpp"

namespace Slic3r {

// Automatic application previews use measured KM/K-S only when every active
// material is identified. Keep generic picker colors on FilamentMixer: an
// estimated spectrum need not reproduce their RGB endpoints.
inline std::string blend_mixed_color_inputs_auto(const std::vector<MixedFilamentColorInput>& inputs, MixedFilamentColorEngine engine)
{
    if (engine == MixedFilamentColorEngine::FullSpectrumKSPairResidual) {
        std::vector<FullSpectrumKSPairResidualColorInput> calibrated_inputs;
        calibrated_inputs.reserve(inputs.size());
        for (const auto& input : inputs)
            calibrated_inputs.push_back({input.color_hex, input.percent,
                                         MixedFilamentManager::use_td_for_color_prediction() ? input.td_mm : std::nullopt,
                                         input.material_id});
        if (!full_spectrum_ks_inputs_are_calibrated(calibrated_inputs))
            engine = MixedFilamentColorEngine::FilamentMixer;
    }
    return MixedFilamentManager::blend_color_multi(inputs, engine);
}

} // namespace Slic3r
