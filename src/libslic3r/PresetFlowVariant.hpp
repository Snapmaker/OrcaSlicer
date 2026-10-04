#pragma once

#include "PrintConfig.hpp"

namespace Slic3r {

// Read the per-extruder nozzle flow type from a printer preset; missing or invalid values are standard.
FilamentVolumeType get_nozzle_volume_type(const ConfigBase &printer_config, unsigned int extruder_id = 0);

// Flow type of one extruder's nozzle from per-extruder flow names (FLOW_MODE_*,
// e.g. GUI::FlowType::nozzle_volume_types()). `fallback` when the list does not
// reach `extruder_id`. Calibration on a chosen extruder reads this.
inline FilamentVolumeType nozzle_flow_type_at(const std::vector<std::string> &nozzle_types, unsigned int extruder_id,
                                              FilamentVolumeType fallback)
{
    if (extruder_id >= nozzle_types.size())
        return fallback;
    return nozzle_types[extruder_id] == FLOW_MODE_HIGH_FLOW ? fvtHighFlow : fvtStandard;
}

// Same type get_config_idx() uses for Filament / Process / Printer: project
// filament_volume_type[filament_id]. Missing or out-of-range values are Standard.
// Do not substitute nozzle_volume_type: a Standard filament on an HF nozzle
// still slices the Standard column.
inline FilamentVolumeType filament_volume_type_at(const ConfigBase &config, unsigned int filament_id = 0)
{
    const auto *types = config.option<ConfigOptionEnumsGeneric>("filament_volume_type");
    if (types == nullptr || types->values.empty())
        return fvtStandard;
    if (filament_id >= types->values.size())
        return fvtStandard;
    return types->values[filament_id] == int(fvtHighFlow) ? fvtHighFlow : fvtStandard;
}

// Resolve an index inside a single preset's variant array. Unlike get_config_idx(), this helper
// does not expect filament_flow_step_size or a composed multi-filament config.
size_t get_preset_flow_variant_idx(const ConfigBase &preset_config, ConfigFlowDomain domain, FilamentVolumeType type);

template<typename VectorOption>
inline auto get_preset_value_at(const ConfigBase &preset_config, const VectorOption &opt, ConfigFlowDomain domain, FilamentVolumeType type)
    -> decltype(opt.get_at(0))
{
    return opt.get_at(get_preset_flow_variant_idx(preset_config, domain, type));
}

// Copy one filament preset's flow-variant values into its segment of a composed
// (packed) vector: slot k of the segment takes source slot k, or slot 0 when the
// source is shorter (a preset that never stored a High-Flow value uses its
// Standard one). For a nullable option (the filament_* retract overrides) a nil
// slot k > 0 also falls back to a set slot 0: "no High-Flow override of its own"
// means the Standard override applies, and only when that is nil too does the
// printer value (via apply_override) apply. Never yields NaN for a set Standard.
void compose_filament_flow_variant_segment(ConfigOptionVectorBase       &dst,
                                           const ConfigOptionVectorBase &src,
                                           size_t                        segment_start,
                                           size_t                        step_size);

// Which slot of a filament override a flow-variant view `index` effectively uses:
// `index` when it holds a value, else slot 0 when that does (index > 0), else -1
// (nothing set: the printer value applies). Bounds-safe like get_at.
int filament_override_effective_slot(const ConfigOption *opt, size_t index);

inline double filament_preset_flow_ratio(const ConfigBase &preset_config, FilamentVolumeType type)
{
    const auto *opt = preset_config.option<ConfigOptionFloats>("filament_flow_ratio");
    if (opt == nullptr || opt->values.empty())
        return 1.0;
    return get_preset_value_at(preset_config, *opt, ConfigFlowDomain::Filament, type);
}

// The flow ratio and max volumetric speed a flow / max-flowrate calibration sizes its
// test from, for the flow type the calibration print will slice with. A filament preset
// that declares no column for `type` (most U1 filaments only have Standard) gives its
// Standard values, exactly as slicing it would.
struct CalibFlowValues
{
    double flow_ratio           = 1.0;
    double max_volumetric_speed = 0.0;
};
inline CalibFlowValues calib_filament_flow_values(const ConfigBase &filament_preset, FilamentVolumeType type)
{
    CalibFlowValues out;
    out.flow_ratio = filament_preset_flow_ratio(filament_preset, type);
    const auto *mvs = filament_preset.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    if (mvs != nullptr && !mvs->values.empty())
        out.max_volumetric_speed = get_preset_value_at(filament_preset, *mvs, ConfigFlowDomain::Filament, type);
    return out;
}

} // namespace Slic3r
