#include "SnapmakerFlowCompat.hpp"

#include "PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <set>
#include <string>

namespace Slic3r {

size_t normalize_promoted_filament_keys(DynamicPrintConfig& config, size_t num_filaments, const std::vector<int>& filament_self_index)
{
    const size_t num_columns = filament_self_index.size();
    // With one column per filament both layouts are the same vector.
    if (num_filaments == 0 || num_columns <= num_filaments)
        return 0;
    for (int owner : filament_self_index)
        if (owner < 1 || size_t(owner) > num_filaments)
            return 0;

    size_t rebuilt = 0;
    for (const std::string& key : promoted_filament_variant_keys()) {
        auto* values = dynamic_cast<ConfigOptionVectorBase*>(config.option(key));
        if (values == nullptr || values->size() != num_filaments)
            continue;
        std::unique_ptr<ConfigOption> per_filament(values->clone());
        values->resize(num_columns);
        for (size_t column = 0; column < num_columns; ++column)
            values->set_at(per_filament.get(), column, size_t(filament_self_index[column] - 1));
        ++rebuilt;
    }
    if (rebuilt > 0)
        BOOST_LOG_TRIVIAL(info) << "normalize_promoted_filament_keys: rebuilt " << rebuilt << " per filament keys over "
                                << num_columns << " filament variant columns";
    return rebuilt;
}

namespace {

const char* const FLOW_STANDARD  = "standard";
const char* const FLOW_HIGH_FLOW = "high_flow";

// project_schema_version is not among them: it is no sign of a 2.4 file, this application writes it
// too, and it has a reader of its own.
const std::array<const char*, 6> SNAPMAKER_FLOW_KEYS = {
    "filament_flow_support", "process_flow_support", "printer_flow_support", "filament_flow_step_size",
    "filament_volume_type", "filament_grouping_mode"
};

const ConfigOptionStrings* strings_option(const DynamicPrintConfig& config, const char* key)
{
    return dynamic_cast<const ConfigOptionStrings*>(config.option(key));
}

bool is_high_flow(const std::string& flow) { return flow == FLOW_HIGH_FLOW; }

// Drive of the variant names. 2.4 has no drive in its flow types; the printer's first extruder
// names it, a lone filament or process preset belongs to a direct drive printer (the only kind
// 2.4 has High Flow values for).
ExtruderType drive_of(const DynamicPrintConfig& config, size_t extruder)
{
    const auto* types = dynamic_cast<const ConfigOptionEnumsGeneric*>(config.option("extruder_type"));
    if (types == nullptr || types->values.empty())
        return etDirectDrive;
    return ExtruderType(types->get_at(extruder));
}

std::string variant_name(ExtruderType drive, const std::string& flow)
{
    return get_extruder_variant_string(drive, is_high_flow(flow) ? nvtHighFlow : nvtStandard);
}

// Repeats every value of a vector that holds one value per owner over the columns of that owner.
void broadcast_over_columns(ConfigOptionVectorBase& values, const std::vector<int>& owner_of_column)
{
    std::unique_ptr<ConfigOption> per_owner(values.clone());
    values.resize(owner_of_column.size());
    for (size_t column = 0; column < owner_of_column.size(); ++column)
        values.set_at(per_owner.get(), column, size_t(owner_of_column[column] - 1));
}

size_t count_filaments(const DynamicPrintConfig& config)
{
    if (const auto* steps = dynamic_cast<const ConfigOptionInts*>(config.option("filament_flow_step_size")); steps != nullptr && !steps->values.empty())
        return steps->values.size();
    for (const char* key : {"filament_colour", "filament_settings_id"})
        if (const ConfigOptionStrings* values = strings_option(config, key); values != nullptr && !values->values.empty())
            return values->values.size();
    return 1;
}

void normalize_filament_columns(DynamicPrintConfig& config, size_t num_filaments, ExtruderType drive)
{
    const ConfigOptionStrings* support = strings_option(config, "filament_flow_support");
    if (support == nullptr || support->values.empty())
        return;

    // Values per filament: the project says it, a lone preset has as many as it declares.
    std::vector<int> steps(num_filaments, 1);
    const auto* stored_steps = dynamic_cast<const ConfigOptionInts*>(config.option("filament_flow_step_size"));
    if (stored_steps != nullptr && stored_steps->values.size() == num_filaments) {
        for (size_t i = 0; i < num_filaments; ++i)
            steps[i] = std::max(1, stored_steps->values[i]);
    } else if (num_filaments == 1)
        steps[0] = int(support->values.size());
    const size_t num_columns = size_t(std::accumulate(steps.begin(), steps.end(), 0));
    if (support->values.size() != num_columns) {
        BOOST_LOG_TRIVIAL(warning) << "normalize_snapmaker_flow_config: filament_flow_support names " << support->values.size()
                                   << " flow types, the filaments hold " << num_columns << " columns; the filament columns are left as they are";
        return;
    }

    std::vector<int>         owner_of_column;
    std::vector<std::string> variants;
    for (size_t filament = 0; filament < num_filaments; ++filament)
        for (int k = 0; k < steps[filament]; ++k) {
            owner_of_column.emplace_back(int(filament) + 1);
            variants.emplace_back(variant_name(drive, support->values[variants.size()]));
        }

    if (num_columns != num_filaments)
        for (const std::string& key : filament_options_with_variant) {
            if (key == "filament_extruder_variant")
                continue;
            // The keys 2.4 stores per flow type already hold one value per column.
            auto* values = dynamic_cast<ConfigOptionVectorBase*>(config.option(key));
            if (values != nullptr && values->size() == num_filaments)
                broadcast_over_columns(*values, owner_of_column);
        }
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings(variants));
    config.set_key_value("filament_self_index", new ConfigOptionInts(owner_of_column));
}

void normalize_process_columns(DynamicPrintConfig& config, ExtruderType drive)
{
    const ConfigOptionStrings* support = strings_option(config, "process_flow_support");
    if (support == nullptr || support->values.empty())
        return;
    const size_t num_columns = support->values.size();
    std::vector<std::string> variants;
    for (const std::string& flow : support->values)
        variants.emplace_back(variant_name(drive, flow));
    // One id for every column: the columns differ by flow type only and serve every tool head.
    config.set_key_value("print_extruder_id", new ConfigOptionInts(std::vector<int>(num_columns, 1)));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    for (const std::string& key : print_options_with_variant) {
        auto* values = dynamic_cast<ConfigOptionVectorBase*>(config.option(key));
        if (values != nullptr && values->size() > 0 && values->size() != num_columns)
            // Front value, i.e. the Standard one, as the loader of the system presets pads.
            values->resize(num_columns, values);
    }
}

size_t count_heads(const DynamicPrintConfig& config)
{
    if (const auto* diameters = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter")))
        return diameters->values.size();
    size_t heads = 0;
    for (const std::string& key : printer_options_with_variant_1)
        if (const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key)))
            heads = std::max(heads, values->size());
    return heads;
}

void normalize_printer_columns(DynamicPrintConfig& config, bool whole_project)
{
    const size_t num_heads = count_heads(config);
    if (num_heads == 0)
        return;
    // Preset::normalize() runs before a project is loaded and gives a printer without variants one
    // Standard column per tool head. That layout, or none at all, is what a 2.4 file can have;
    // anything else was written by this application and is left alone.
    if (const ConfigOptionStrings* declared = strings_option(config, "extruder_variant_list"))
        for (const std::string& variants_of_head : declared->values)
            if (variants_of_head.find(',') != std::string::npos)
                return;
    if (const ConfigOptionStrings* columns = strings_option(config, "printer_extruder_variant"); columns != nullptr && columns->values.size() != num_heads)
        return;

    const ConfigOptionStrings* support = strings_option(config, "printer_flow_support");
    std::vector<std::string> flows = support != nullptr && !support->values.empty() ? support->values : std::vector<std::string>{FLOW_STANDARD};
    // A user preset holds what differs from its parent, one value per tool head: those are
    // Standard columns, the parent's other columns are filled by the rule for printer presets in
    // update_diff_values_to_child_config().
    if (!whole_project)
        flows = {FLOW_STANDARD};

    std::vector<int>         ids;
    std::vector<std::string> variants, variant_list;
    for (size_t head = 0; head < num_heads; ++head) {
        std::string declared;
        for (const std::string& flow : flows) {
            ids.emplace_back(int(head) + 1);
            variants.emplace_back(variant_name(drive_of(config, head), flow));
            declared += (declared.empty() ? "" : ",") + variants.back();
        }
        variant_list.emplace_back(declared);
    }
    const size_t num_flows = flows.size();
    auto widen = [&config, num_heads, num_flows](const std::set<std::string>& keys, size_t stride) {
        for (const std::string& key : keys) {
            auto* values = dynamic_cast<ConfigOptionVectorBase*>(config.option(key));
            if (values == nullptr || values->size() != num_heads * stride)
                continue;
            std::unique_ptr<ConfigOption> per_head(values->clone());
            values->resize(num_heads * num_flows * stride);
            for (size_t head = 0; head < num_heads; ++head)
                for (size_t flow = 0; flow < num_flows; ++flow)
                    for (size_t k = 0; k < stride; ++k)
                        values->set_at(per_head.get(), (head * num_flows + flow) * stride + k, head * stride + k);
        }
    };
    if (num_flows > 1) {
        widen(printer_options_with_variant_1, 1);
        widen(printer_options_with_variant_2, 2);
    }
    config.set_key_value("printer_extruder_id", new ConfigOptionInts(ids));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings(variants));
    if (whole_project)
        config.set_key_value("extruder_variant_list", new ConfigOptionStrings(variant_list));
}

// The tool head (0 based) that prints every filament, as the tool ordering decides it for a
// printer with one nozzle per tool head: a manual map is binding, every other mode prints
// filament i with tool head i and the filaments beyond the tool heads with the master extruder.
std::vector<size_t> heads_of_filaments(const DynamicPrintConfig& config, size_t num_filaments, size_t num_heads)
{
    std::vector<size_t> heads(num_filaments, 0);
    const ConfigOption* mode      = config.option("filament_map_mode");
    const auto*         map       = dynamic_cast<const ConfigOptionInts*>(config.option("filament_map"));
    const ConfigOption* master    = config.option("master_extruder_id");
    const bool          binding   = mode != nullptr && mode->getInt() >= int(fmmManual) && map != nullptr && map->values.size() == num_filaments;
    size_t              fallback  = master != nullptr && master->getInt() >= 1 && size_t(master->getInt()) <= num_heads ? size_t(master->getInt()) - 1 : 0;
    for (size_t filament = 0; filament < num_filaments; ++filament) {
        heads[filament] = filament < num_heads ? filament : fallback;
        if (binding && map->values[filament] >= 1 && size_t(map->values[filament]) <= num_heads)
            heads[filament] = size_t(map->values[filament]) - 1;
    }
    return heads;
}

void reconcile_flow_types(DynamicPrintConfig& config, size_t num_filaments, FlowImportReport& report)
{
    const ConfigOptionStrings* filament_types = strings_option(config, "filament_volume_type");
    auto* head_types = dynamic_cast<ConfigOptionEnumsGeneric*>(config.option("nozzle_volume_type"));
    if (filament_types == nullptr || head_types == nullptr || head_types->values.empty())
        return;
    const size_t num_heads = head_types->values.size();
    const std::vector<size_t> heads = heads_of_filaments(config, num_filaments, num_heads);

    for (size_t head = 0; head < num_heads; ++head) {
        std::vector<size_t> printed;
        for (size_t filament = 0; filament < num_filaments && filament < filament_types->values.size(); ++filament)
            if (heads[filament] == head)
                printed.emplace_back(filament);
        if (printed.empty())
            continue;
        const bool head_high_flow = head_types->values[head] == int(nvtHighFlow);
        const bool first_high_flow = is_high_flow(filament_types->values[printed.front()]);
        const bool agree = std::all_of(printed.begin(), printed.end(), [&](size_t filament) {
            return is_high_flow(filament_types->values[filament]) == first_high_flow; });
        if (agree) {
            if (first_high_flow != head_high_flow) {
                head_types->values[head] = int(first_high_flow ? nvtHighFlow : nvtStandard);
                report.changed_heads.push_back({int(head) + 1, first_high_flow});
            }
        } else
            for (size_t filament : printed)
                if (is_high_flow(filament_types->values[filament]) != head_high_flow)
                    report.dropped_filaments.push_back({int(filament) + 1, int(head) + 1, !head_high_flow});
    }
}

} // namespace

bool has_snapmaker_flow_keys(const DynamicPrintConfig& config)
{
    for (const char* key : SNAPMAKER_FLOW_KEYS)
        if (config.option(key) != nullptr)
            return true;
    return false;
}

void drop_snapmaker_flow_keys(DynamicPrintConfig& config)
{
    for (const char* key : SNAPMAKER_FLOW_KEYS)
        config.erase(key);
}

bool normalize_snapmaker_flow_config(DynamicPrintConfig& config, FlowImportReport& report)
{
    if (!has_snapmaker_flow_keys(config))
        return false;

    // A project holds the printer; a user preset is a filament, a process or a printer preset.
    const bool whole_project = config.option("filament_flow_step_size") != nullptr || config.option("filament_volume_type") != nullptr;
    const size_t       num_filaments = count_filaments(config);
    const ExtruderType drive         = drive_of(config, 0);

    // A config that names its columns already (a preset of this application that kept the 2.4 keys
    // as markers) is not rewritten.
    if (config.option("filament_extruder_variant") == nullptr || whole_project)
        normalize_filament_columns(config, num_filaments, drive);
    if (config.option("print_extruder_variant") == nullptr || whole_project)
        normalize_process_columns(config, drive);
    if (whole_project || config.option("printer_flow_support") != nullptr)
        normalize_printer_columns(config, whole_project);
    if (whole_project)
        reconcile_flow_types(config, num_filaments, report);
    if (const auto* grouping = dynamic_cast<const ConfigOptionString*>(config.option("filament_grouping_mode")))
        report.custom_grouping = grouping->value == "custom";

    drop_snapmaker_flow_keys(config);
    BOOST_LOG_TRIVIAL(info) << "normalize_snapmaker_flow_config: rewrote the flow columns of a Snapmaker Orca 2.4 "
                            << (whole_project ? "project" : "preset") << ", " << report.changed_heads.size() << " extruders changed, "
                            << report.dropped_filaments.size() << " filament flow types dropped";
    return true;
}

bool is_snapmaker_flow_scalar_key(const std::string& key)
{
    static const std::set<std::string> keys = {
        "ironing_speed", "slow_down_layers", "accel_to_decel_enable", "accel_to_decel_factor",
        "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length",
        "extrusion_rate_smoothing_external_perimeter_only"
    };
    return keys.count(key) > 0;
}

int project_schema_version_for(const DynamicPrintConfig& config)
{
    size_t num_filaments = 0;
    for (const char* key : {"filament_colour", "filament_settings_id"})
        if (const ConfigOptionStrings* values = strings_option(config, key))
            num_filaments = std::max(num_filaments, values->values.size());
    const ConfigOptionStrings* filament_columns = strings_option(config, "filament_extruder_variant");
    if (filament_columns != nullptr && filament_columns->values.size() > std::max<size_t>(num_filaments, 1))
        return 2;
    for (const std::string& key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        // The id and variant lists of a single column preset are widened to the printer's columns;
        // the values tell whether the process has more than one.
        const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
        if (values != nullptr && values->size() > 1)
            return 2;
    }
    return 1;
}

} // namespace Slic3r
