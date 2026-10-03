#pragma once

#include "PrintConfig.hpp"

namespace Slic3r {

// Read the per-extruder nozzle flow type from a printer preset; missing or invalid values are standard.
FilamentVolumeType get_nozzle_volume_type(const ConfigBase &printer_config, unsigned int extruder_id = 0);

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

inline double filament_preset_flow_ratio(const ConfigBase &preset_config, FilamentVolumeType type)
{
    const auto *opt = preset_config.option<ConfigOptionFloats>("filament_flow_ratio");
    if (opt == nullptr || opt->values.empty())
        return 1.0;
    return get_preset_value_at(preset_config, *opt, ConfigFlowDomain::Filament, type);
}

} // namespace Slic3r
