#include "PresetFlowVariant.hpp"
#include "Exception.hpp"

#include <algorithm>
#include <memory>
#include <numeric>

namespace Slic3r {

namespace {

std::string common_variant(const std::string& mode)
{
    if (mode == FLOW_MODE_STANDARD)
        return "Direct Drive Standard";
    if (mode == FLOW_MODE_HIGH_FLOW)
        return "Direct Drive High Flow";
    return mode;
}

std::string legacy_mode(const std::string& variant)
{
    if (variant == "Direct Drive Standard")
        return FLOW_MODE_STANDARD;
    if (variant == "Direct Drive High Flow")
        return FLOW_MODE_HIGH_FLOW;
    // Preserve other drive types and future variants, including their positions.
    return variant;
}

} // namespace

std::vector<std::string> filament_extruder_variants(const ConfigBase& config)
{
    if (const auto* variants = config.option<ConfigOptionStrings>("filament_extruder_variant"); variants && !variants->values.empty())
        return variants->values;
    std::vector<std::string> result;
    if (const auto* legacy = config.option<ConfigOptionStrings>("filament_flow_support"))
        for (const auto& mode : legacy->values)
            result.push_back(common_variant(mode));
    return result;
}

void set_filament_flow_modes(ConfigBase& config, const std::vector<std::string>& modes)
{
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values = modes;
    auto& variants = config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values;
    variants.clear();
    for (const auto& mode : modes)
        variants.push_back(common_variant(mode));
    if (auto* self = config.option<ConfigOptionInts>("filament_self_index");
        self && !self->values.empty() && std::all_of(self->values.begin(), self->values.end(), [](int id) { return id == 1; }))
        self->values.assign(modes.size(), 1);
}

void normalize_filament_variant_metadata(ConfigBase& config, bool legacy_source)
{
    const auto* legacy = config.option<ConfigOptionStrings>("filament_flow_support");
    const auto* common = config.option<ConfigOptionStrings>("filament_extruder_variant");
    if (legacy_source && legacy && !legacy->values.empty()) {
        const auto modes = legacy->values;
        set_filament_flow_modes(config, modes);
    } else if (!common || common->values.empty()) {
        if (legacy && !legacy->values.empty()) {
            const auto modes = legacy->values;
            set_filament_flow_modes(config, modes);
        }
    } else {
        auto& modes = config.option<ConfigOptionStrings>("filament_flow_support", true)->values;
        modes.clear();
        for (const auto& variant : common->values)
            modes.push_back(legacy_mode(variant));
    }

    auto* variants = config.option<ConfigOptionStrings>("filament_extruder_variant");
    if (!variants || variants->values.empty())
        return;
    const size_t count          = variants->values.size();
    const auto*  self           = config.option<ConfigOptionInts>("filament_self_index");
    const auto*  diameters      = config.option<ConfigOptionFloats>("filament_diameter");
    const size_t filament_count = diameters ? diameters->values.size() : 0;
    const auto*  stored_steps   = config.option<ConfigOptionInts>("filament_flow_step_size");
    const bool   shared_default = count == 1 && self && self->values == std::vector<int>{1};
    if (filament_count > 1 && (!self || self->values.empty() || legacy_source || shared_default) &&
        (!stored_steps || stored_steps->values.size() != filament_count) && (count == 1 || count == filament_count)) {
        // Old projects stored one value per filament without an owner table.
        // A default Standard declaration is shared, not a one-filament layout.
        variants->values.resize(filament_count, variants->values.front());
        auto& modes = config.option<ConfigOptionStrings>("filament_flow_support", true)->values;
        modes.clear();
        for (const auto& variant : variants->values)
            modes.push_back(legacy_mode(variant));
        config.option<ConfigOptionInts>("filament_flow_step_size", true)->values.assign(filament_count, 1);
        auto& owners = config.option<ConfigOptionInts>("filament_self_index", true)->values;
        owners.resize(filament_count);
        std::iota(owners.begin(), owners.end(), 1);
        return;
    }
    if (!legacy_source && self && !self->values.empty()) {
        auto owners = self->values;
        if (owners.size() == 1 && owners.front() == 1)
            owners.assign(count, 1);
        if (owners.size() != count || *std::min_element(owners.begin(), owners.end()) < 1)
            throw FilamentVariantLayoutError("Invalid filament_self_index: one positive filament index is required per extruder variant.");
        const size_t num_filaments = size_t(*std::max_element(owners.begin(), owners.end()));
        if (num_filaments > count)
            throw FilamentVariantLayoutError("Invalid filament_self_index: filament indices must be contiguous.");
        std::vector<int> steps(num_filaments, 0);
        for (int owner : owners)
            ++steps[size_t(owner - 1)];
        if (std::find(steps.begin(), steps.end(), 0) != steps.end())
            throw FilamentVariantLayoutError("Invalid filament_self_index: filament indices must be contiguous.");
        std::vector<size_t> order(count);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return owners[a] < owners[b]; });
        // The legacy slicer uses contiguous filament segments. Canonical input
        // may interleave owners; reorder complete rows without changing values.
        for (const auto& key : filament_flow_variant_options()) {
            auto* option = dynamic_cast<ConfigOptionVectorBase*>(config.option(key));
            if (!option || option->empty())
                continue;
            std::unique_ptr<ConfigOption> original(option->clone());
            const size_t                  source_size = option->size();
            option->resize(count);
            for (size_t i = 0; i < count; ++i) {
                const size_t source = source_size == num_filaments && source_size != count ? size_t(owners[order[i]] - 1) :
                                      order[i] < source_size                               ? order[i] :
                                                                                             0;
                option->set_at(original.get(), i, source);
            }
        }
        const auto names   = variants->values;
        auto&      modes   = config.option<ConfigOptionStrings>("filament_flow_support", true)->values;
        auto&      indices = config.option<ConfigOptionInts>("filament_self_index", true)->values;
        indices.resize(count);
        for (size_t i = 0; i < count; ++i) {
            variants->values[i] = names[order[i]];
            modes[i]            = legacy_mode(variants->values[i]);
            indices[i]          = owners[order[i]];
        }
        config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = std::move(steps);
    } else if (const auto* steps = config.option<ConfigOptionInts>("filament_flow_step_size"); steps && !steps->values.empty()) {
        auto& indices = config.option<ConfigOptionInts>("filament_self_index", true)->values;
        indices.clear();
        // A single preset's declaration takes precedence over a default step=1.
        if (steps->values.size() == 1) {
            indices.assign(count, 1);
            config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {int(count)};
        } else {
            for (size_t i = 0; i < steps->values.size(); ++i)
                indices.insert(indices.end(), size_t(std::max(1, steps->values[i])), int(i + 1));
            if (indices.size() != count)
                throw FilamentVariantLayoutError("Invalid filament variant segments: counts do not match the declared variants.");
        }
    }
}

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

size_t get_preset_flow_variant_idx(const ConfigBase &preset_config, ConfigFlowDomain domain, FilamentVolumeType type)
{
    if (domain == ConfigFlowDomain::Filament) {
        const auto variants  = filament_extruder_variants(preset_config);
        const auto requested = common_variant(to_string(type));
        const auto found     = std::find(variants.begin(), variants.end(), requested);
        if (found != variants.end())
            return size_t(found - variants.begin());
        const auto standard = std::find(variants.begin(), variants.end(), "Direct Drive Standard");
        return standard == variants.end() ? 0 : size_t(standard - variants.begin());
    }
    const ConfigOption *option = preset_config.option(flow_support_key(domain));
    if (option == nullptr || option->type() != coStrings)
        return 0;

    const auto *flow_support = static_cast<const ConfigOptionStrings *>(option);
    if (flow_support->values.empty())
        return 0;

    return flow_variant_index(flow_support->values, to_string(type));
}

} // namespace Slic3r
