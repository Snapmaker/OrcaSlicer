#include "PresetFlowVariant.hpp"

namespace Slic3r {

FilamentVolumeType get_nozzle_volume_type(const ConfigBase &printer_config, unsigned int extruder_id)
{
    const ConfigOption *option = printer_config.option("nozzle_volume_type");
    if (option == nullptr || option->type() != coEnums)
        return fvtStandard;

    const auto *types = static_cast<const ConfigOptionEnumsGeneric *>(option);
    if (extruder_id >= types->values.size())
        return fvtStandard;

    const int value = types->values[extruder_id];
    return value == fvtHighFlow ? fvtHighFlow : fvtStandard;
}

void compose_filament_flow_variant_segment(ConfigOptionVectorBase       &dst,
                                           const ConfigOptionVectorBase &src,
                                           size_t                        segment_start,
                                           size_t                        step_size)
{
    const size_t source_size = src.size();
    if (source_size == 0)
        return;
    const bool fall_back_to_first = src.nullable() && !src.is_nil(0);
    for (size_t k = 0; k < step_size; ++k) {
        size_t from = k < source_size ? k : 0;
        if (from != 0 && fall_back_to_first && src.is_nil(from))
            from = 0;
        dst.set_at(&src, segment_start + k, from);
    }
}

int filament_override_effective_slot(const ConfigOption *opt, size_t index)
{
    const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt);
    if (vec == nullptr || vec->size() == 0)
        return -1;
    if (!vec->is_nil(index))
        return int(index);
    if (index > 0 && !vec->is_nil(0))
        return 0;
    return -1;
}

size_t get_preset_flow_variant_idx(const ConfigBase &preset_config, ConfigFlowDomain domain, FilamentVolumeType type)
{
    const ConfigOption *option = preset_config.option(flow_support_key(domain));
    if (option == nullptr || option->type() != coStrings)
        return 0;

    const auto *flow_support = static_cast<const ConfigOptionStrings *>(option);
    if (flow_support->values.empty())
        return 0;

    return flow_variant_index(flow_support->values, to_string(type));
}

} // namespace Slic3r
