#include "FlowVariantEdit.hpp"

#include <algorithm>
#include <memory>

#include "libslic3r/Config.hpp"

namespace Slic3r {
namespace GUI {

const std::vector<std::string> &flow_variant_option_keys(ConfigFlowDomain domain)
{
    switch (domain) {
    case ConfigFlowDomain::Filament: return filament_flow_variant_options();
    case ConfigFlowDomain::Process:  return process_flow_variant_options();
    default:                         return machine_flow_variant_options();
    }
}

static std::vector<std::string> support_modes(const DynamicPrintConfig &config, ConfigFlowDomain domain)
{
    const auto *support = config.option<ConfigOptionStrings>(flow_support_key(domain));
    if (support == nullptr || support->values.empty())
        return {FLOW_MODE_STANDARD};
    return support->values;
}

static bool find_mode_index(const std::vector<std::string> &modes, const std::string &mode, size_t &index)
{
    const auto it = std::find(modes.begin(), modes.end(), mode);
    if (it == modes.end())
        return false;
    index = size_t(it - modes.begin());
    return true;
}

static std::string slot_serialized(const ConfigOptionVectorBase &vec, size_t index)
{
    const std::vector<std::string> values = vec.vserialize();
    if (values.empty())
        return {};
    return index < values.size() ? values[index] : values.front();
}

static bool option_slots_differ(const ConfigOption *opt, size_t index_a, size_t index_b)
{
    if (opt == nullptr || index_a == index_b)
        return false;
    const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt);
    if (vec == nullptr)
        return false;
    return slot_serialized(*vec, index_a) != slot_serialized(*vec, index_b);
}

static bool copy_option_slot(DynamicPrintConfig &config, const std::string &key, size_t from_index, size_t to_index)
{
    ConfigOption *opt = config.option(key);
    if (opt == nullptr || from_index == to_index)
        return false;
    if (dynamic_cast<ConfigOptionVectorBase *>(opt) == nullptr)
        return false;

    std::unique_ptr<ConfigOption> clone(opt->clone());
    auto *clone_vec = dynamic_cast<ConfigOptionVectorBase *>(clone.get());
    if (clone_vec == nullptr)
        return false;

    clone_vec->set_at(opt, to_index, from_index);
    if (*clone == *opt)
        return false;

    config.set_key_value(key, clone.release());
    return true;
}

bool copy_flow_variant_slot(DynamicPrintConfig &config,
                            ConfigFlowDomain    domain,
                            const std::string  &from_mode,
                            const std::string  &to_mode)
{
    const std::vector<std::string> modes = support_modes(config, domain);
    size_t from_index = 0;
    size_t to_index   = 0;
    if (!find_mode_index(modes, from_mode, from_index) || !find_mode_index(modes, to_mode, to_index))
        return false;
    if (from_index == to_index)
        return false;

    bool changed = false;
    for (const std::string &key : flow_variant_option_keys(domain))
        changed = copy_option_slot(config, key, from_index, to_index) || changed;
    return changed;
}

bool flow_variant_slots_differ(const DynamicPrintConfig &config,
                               ConfigFlowDomain          domain,
                               const std::string        &mode_a,
                               const std::string        &mode_b)
{
    const std::vector<std::string> modes = support_modes(config, domain);
    size_t index_a = 0;
    size_t index_b = 0;
    if (!find_mode_index(modes, mode_a, index_a) || !find_mode_index(modes, mode_b, index_b))
        return false;
    if (index_a == index_b)
        return false;

    for (const std::string &key : flow_variant_option_keys(domain)) {
        if (option_slots_differ(config.option(key), index_a, index_b))
            return true;
    }
    return false;
}

bool ensure_flow_support_mode(DynamicPrintConfig &config,
                              ConfigFlowDomain    domain,
                              const std::string  &mode)
{
    std::vector<std::string> modes;
    if (const auto *support = config.option<ConfigOptionStrings>(flow_support_key(domain)))
        modes = support->values;
    if (modes.empty())
        modes.emplace_back(FLOW_MODE_STANDARD);
    if (std::find(modes.begin(), modes.end(), mode) != modes.end())
        return false;
    modes.emplace_back(mode);
    config.set_key_value(flow_support_key(domain), new ConfigOptionStrings(modes));
    return true;
}

bool replicate_flow_variant_value(DynamicPrintConfig             &config,
                                  const std::string              &opt_key,
                                  size_t                          source_index,
                                  const std::vector<std::string> &modes)
{
    if (modes.size() <= 1)
        return false;

    ConfigOption *opt = config.option(opt_key);
    if (opt == nullptr || dynamic_cast<ConfigOptionVectorBase *>(opt) == nullptr)
        return false;

    std::unique_ptr<ConfigOption> clone(opt->clone());
    auto *clone_vec = dynamic_cast<ConfigOptionVectorBase *>(clone.get());
    if (clone_vec == nullptr)
        return false;

    for (size_t index = 0; index < modes.size(); ++index) {
        if (index == source_index)
            continue;
        clone_vec->set_at(opt, index, source_index);
    }

    if (*clone == *opt)
        return false;

    config.set_key_value(opt_key, clone.release());
    return true;
}

int filament_tab_option_index(const std::string &key, size_t view_index)
{
    return is_filament_flow_variant_option(key) ? int(view_index) : 0;
}

static bool temperature_out_of_recommended_range(const DynamicPrintConfig &config, const char *temp_key, int variant_index)
{
    if (!config.has(temp_key) || !config.has("nozzle_temperature_range_low") || !config.has("nozzle_temperature_range_high"))
        return false;
    const int temperature = config.opt_int(temp_key, variant_index);
    const int range_low   = config.opt_int("nozzle_temperature_range_low", 0);
    const int range_high  = config.opt_int("nozzle_temperature_range_high", 0);
    return temperature < range_low || temperature > range_high;
}

bool filament_nozzle_temperature_out_of_range(const DynamicPrintConfig &config, int variant_index)
{
    return temperature_out_of_recommended_range(config, "nozzle_temperature", variant_index);
}

bool filament_nozzle_temperature_initial_layer_out_of_range(const DynamicPrintConfig &config, int variant_index)
{
    return temperature_out_of_recommended_range(config, "nozzle_temperature_initial_layer", variant_index);
}

} // namespace GUI
} // namespace Slic3r
