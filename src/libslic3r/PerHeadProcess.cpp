#include "PerHeadProcess.hpp"

#include "libslic3r.h"
#include "Config.hpp"
#include "NozzleFilamentPresets.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>

namespace Slic3r { namespace PerHeadProcess {

const char *const source_column_key = "print_extruder_source_column";
const char *const record_key        = "extruder_process_preset";

const std::set<std::string>& composed_keys()
{
    static const std::set<std::string> keys = {
        // speeds
        "initial_layer_speed", "initial_layer_infill_speed", "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed",
        "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed", "overhang_1_4_speed", "overhang_2_4_speed",
        "overhang_3_4_speed", "overhang_4_4_speed", "bridge_speed", "internal_bridge_speed", "gap_infill_speed", "support_speed",
        "support_interface_speed",
        // accelerations
        "default_acceleration", "bridge_acceleration", "initial_layer_acceleration", "outer_wall_acceleration",
        "inner_wall_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration", "top_surface_acceleration",
        // jerk / junction deviation
        "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "infill_jerk", "top_surface_jerk", "initial_layer_jerk",
        "default_junction_deviation",
    };
    return keys;
}

std::string quality_class(const std::string &preset_name)
{
    const size_t mm = preset_name.find("mm ");
    if (mm == std::string::npos)
        return std::string();
    const size_t begin = mm + 3;
    const size_t at    = preset_name.find(" @", begin);
    if (at == std::string::npos)
        return std::string();
    return preset_name.substr(begin, at - begin);
}

namespace {

bool same_height(double a, double b) { return std::abs(a - b) < EPSILON; }

double layer_height_of(const Preset &preset)
{
    const auto *option = preset.config.option<ConfigOptionFloat>("layer_height");
    return option == nullptr ? 0. : option->value;
}

std::string default_print_profile_of(const Preset &machine)
{
    const auto *option = machine.config.option<ConfigOptionString>("default_print_profile");
    return option == nullptr ? std::string() : option->value;
}

bool compatible(const PresetBundle &bundle, const Preset &process, const Preset &machine)
{
    return is_compatible_with_printer(bundle.prints.get_preset_with_vendor_profile(process), bundle.printers.get_preset_with_vendor_profile(machine));
}

// The system process presets compatible with `machine`, `selected` excluded, whatever their
// visibility; in the order of the collection, which is the order of the names.
std::vector<const Preset*> candidates_for(const PresetBundle &bundle, const Preset &machine, const Preset &selected)
{
    std::vector<const Preset*> out;
    for (const Preset &preset : bundle.prints)
        if (preset.is_system && preset.name != selected.name && compatible(bundle, preset, machine))
            out.emplace_back(&preset);
    return out;
}

// The system preset the selected print preset is or inherits from; nullptr for a detached or
// imported preset, whose get_selected_preset_parent() is itself, the default preset or nullptr.
const Preset* system_parent(const PresetBundle &bundle)
{
    const Preset *parent = bundle.prints.get_selected_preset_parent();
    return parent == nullptr || !parent->is_system ? nullptr : parent;
}

} // namespace

std::set<std::string> edited_keys(const PresetBundle &bundle)
{
    std::set<std::string> out;
    const Preset *parent = system_parent(bundle);
    if (parent == nullptr)
        return out;
    static const std::set<std::string> no_ignored_keys;
    const std::vector<std::string> dirty = PresetCollection::dirty_options_without_option_list(&bundle.prints.get_edited_preset(), parent, no_ignored_keys, false);
    for (const std::string &key : dirty)
        if (composed_keys().count(key) > 0)
            out.insert(key);
    return out;
}

const Preset* source_for_head(const PresetBundle &bundle, const Preset &machine, double preferred_layer_height,
                              const Preset &selected, const std::string &explicit_name, Reason &reason)
{
    const std::vector<const Preset*> candidates = candidates_for(bundle, machine, selected);

    // `reason` is an output: its incoming value says nothing about this call.
    bool not_installed = false;
    if (!explicit_name.empty()) {
        for (const Preset *candidate : candidates)
            if (candidate->name == explicit_name && candidate->is_visible) {
                reason = Reason::Explicit;
                return candidate;
            }
        not_installed = true;
    }
    const Reason by_rule = not_installed ? Reason::NotInstalled : Reason::Derived;

    const double      target_height = preferred_layer_height > 0. ? preferred_layer_height : layer_height_of(selected);
    const std::string quality       = quality_class(selected.name);
    const std::string default_name  = default_print_profile_of(machine);

    // 1. an exact layer height
    std::vector<const Preset*> exact;
    for (const Preset *candidate : candidates)
        if (same_height(layer_height_of(*candidate), target_height))
            exact.emplace_back(candidate);
    if (!exact.empty()) {
        for (const Preset *candidate : exact)
            if (quality_class(candidate->name) == quality) {
                reason = by_rule;
                return candidate;
            }
        for (const Preset *candidate : exact)
            if (candidate->name == default_name) {
                reason = by_rule;
                return candidate;
            }
        reason = by_rule;
        return exact.front();
    }

    // 2. the nearest layer height of the same quality class, a tie going to the thinner preset
    const Preset *nearest = nullptr;
    for (const Preset *candidate : candidates) {
        if (quality_class(candidate->name) != quality)
            continue;
        if (nearest == nullptr) {
            nearest = candidate;
            continue;
        }
        const double distance = std::abs(layer_height_of(*candidate) - target_height);
        const double best     = std::abs(layer_height_of(*nearest) - target_height);
        if (distance < best - EPSILON || (same_height(distance, best) && layer_height_of(*candidate) < layer_height_of(*nearest)))
            nearest = candidate;
    }
    if (nearest != nullptr) {
        reason = by_rule;
        return nearest;
    }

    // 3. the machine preset's default process preset
    if (!default_name.empty())
        for (const Preset &preset : bundle.prints)
            if (preset.is_system && preset.name == default_name && preset.name != selected.name) {
                reason = by_rule;
                return &preset;
            }

    reason = Reason::NoProcessPreset;
    return nullptr;
}

std::vector<Source> head_sources(const PresetBundle &bundle)
{
    std::vector<Source> out;
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    if (!state.rule_on)
        return out;
    const Preset &printer  = bundle.printers.get_edited_preset();
    const Preset &selected = bundle.prints.get_selected_preset();
    const Preset *parent   = system_parent(bundle);
    const double  home     = NozzleFilament::home_nozzle_size(printer.config);
    const auto   *heights  = printer.config.option<ConfigOptionFloats>("extruder_layer_height");
    const std::set<std::string> edited = edited_keys(bundle);

    out.resize(state.head_size.size());
    for (size_t head = 0; head < out.size(); ++head) {
        Source &source = out[head];
        source.head   = head;
        source.preset = &selected;
        if (!bundle.process_follows_nozzle) {
            source.reason = Reason::Off;
            continue;
        }
        const Preset *machine = head < state.head_machine.size() ? state.head_machine[head] : &printer;
        if (machine == nullptr || machine == &printer) {
            source.reason = home <= 0. || same_height(state.head_size[head], home) ? Reason::HomeSize : Reason::NoMachinePreset;
            continue;
        }
        if (parent == nullptr) {
            source.reason = Reason::NoParent;
            continue;
        }
        const double preferred = heights != nullptr && head < heights->values.size() ? heights->values[head] : 0.;
        Reason        reason   = Reason::Derived;
        const Preset *chosen   = source_for_head(bundle, *machine, preferred, selected, std::string(), reason);
        if (chosen == nullptr) {
            source.reason = Reason::NoProcessPreset;
            continue;
        }
        source.preset  = chosen;
        source.derived = true;
        source.reason  = reason;
        for (const std::string &key : composed_keys())
            (edited.count(key) > 0 ? source.kept_keys : source.composed_keys).emplace_back(key);
    }
    return out;
}

std::vector<Source> record_sources(PresetBundle &bundle)
{
    std::vector<Source> sources = head_sources(bundle);
    if (sources.empty())
        return sources;
    // The record stays empty, not one empty entry per head, while no head is derived: a project
    // saved without the key and a plate on which nothing is composed carry the same value, so the
    // apply that follows a load changes nothing (the key is a G-code export step of Print::apply).
    std::vector<std::string> record;
    for (size_t head = 0; head < sources.size(); ++head)
        if (sources[head].derived && sources[head].preset != nullptr) {
            record.resize(sources.size());
            record[head] = sources[head].preset->name;
        }
    // Written only when the value changes; an absent option is created only for a non-empty record.
    auto *option = bundle.project_config.option<ConfigOptionStrings>(record_key, !record.empty());
    if (option != nullptr && option->values != record)
        option->values = std::move(record);
    return sources;
}

std::vector<LoadReportEntry> load_report(const PresetBundle &bundle)
{
    std::vector<LoadReportEntry> out;
    const auto *recorded = bundle.project_config.option<ConfigOptionStrings>(record_key);
    if (recorded == nullptr || recorded->values.empty())
        return out;
    const std::vector<Source> sources = head_sources(bundle);
    if (sources.empty())
        return out;
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    for (size_t head = 0; head < sources.size() && head < recorded->values.size(); ++head) {
        const std::string &record = recorded->values[head];
        if (record.empty())
            continue;
        const std::string current = sources[head].derived && sources[head].preset != nullptr ? sources[head].preset->name : std::string();
        if (current == record)
            continue;
        LoadReportEntry entry;
        entry.head               = head;
        entry.nozzle_size        = head < state.head_size.size() ? state.head_size[head] : 0.;
        entry.recorded           = record;
        entry.current            = current;
        entry.reason             = sources[head].reason;
        entry.recorded_installed = bundle.prints.find_preset(record, false) != nullptr;
        out.emplace_back(std::move(entry));
    }
    return out;
}

namespace {

struct Column
{
    size_t           head;
    NozzleVolumeType volume_type;
    std::string      variant;
};

// The extruder type of a tool head. An unexpanded config holds one extruder_type per printer
// variant column, so the head's first column (via printer_extruder_id) is used.
ExtruderType head_extruder_type(const DynamicPrintConfig &full, size_t head, size_t heads)
{
    const auto *types = full.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (types == nullptr || types->values.empty())
        return etDirectDrive;
    if (types->values.size() != heads)
        if (const auto *ids = full.option<ConfigOptionInts>("printer_extruder_id"); ids != nullptr && ids->values.size() == types->values.size())
            for (size_t column = 0; column < ids->values.size(); ++column)
                if (ids->values[column] == int(head) + 1)
                    return ExtruderType(types->values[column]);
    return ExtruderType(types->get_at(head));
}

// The columns Print::apply narrows the process to: one per tool head, or one per (tool head x
// volume type) when the printer declares several types for a head (the slot walk of
// DynamicPrintConfig::update_values_to_printer_extruders).
std::vector<Column> composed_columns(const DynamicPrintConfig &full)
{
    std::vector<Column> columns;
    int extruder_count = 1;
    full.support_different_extruders(extruder_count);
    if (extruder_count <= 1)
        return columns;
    std::vector<std::vector<NozzleVolumeType>> volume_types;
    const int volume_count = full.get_extruder_nozzle_volume_count(extruder_count, volume_types);
    const auto *flows = full.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (flows == nullptr || flows->values.empty())
        return columns;
    for (int head = 0; head < extruder_count; ++head) {
        const ExtruderType extruder_type = head_extruder_type(full, size_t(head), size_t(extruder_count));
        const auto head_flow             = NozzleVolumeType(flows->get_at(size_t(head)));
        const bool per_type      = volume_count > extruder_count || head_flow == nvtHybrid;
        std::vector<NozzleVolumeType> slots = per_type ? volume_types[size_t(head)] : std::vector<NozzleVolumeType>{head_flow};
        for (NozzleVolumeType flow : slots)
            columns.push_back({size_t(head), flow, get_extruder_variant_string(extruder_type, flow == nvtHybrid ? nvtStandard : flow)});
    }
    return columns;
}

} // namespace

bool compose(DynamicPrintConfig &full, const std::set<std::string> &edited, std::vector<Source> &sources)
{
    bool any_derived = false;
    for (const Source &source : sources)
        any_derived = any_derived || (source.derived && source.preset != nullptr && !source.composed_keys.empty());
    if (!any_derived)
        return false;

    const std::vector<Column> columns = composed_columns(full);
    if (columns.empty())
        return false;
    int heads = 1;
    full.support_different_extruders(heads);

    // The selected preset's column that stands in for each composed column, resolved on the
    // layout of the selected preset before it is rewritten.
    std::vector<int> source_columns;
    source_columns.reserve(columns.size());
    for (const Column &column : columns) {
        const int index = full.get_index_for_extruder(int(column.head) + 1, "print_extruder_id", head_extruder_type(full, column.head, size_t(heads)),
                                                      column.volume_type, "print_extruder_variant");
        source_columns.emplace_back(index >= 0 ? index : 0);
    }

    // The source preset's column for each composed column of a derived head, -1 for a column the
    // source has no values for (the selected preset's column serves it).
    std::vector<int> preset_columns(columns.size(), -1);
    for (size_t c = 0; c < columns.size(); ++c) {
        const Column &column = columns[c];
        if (column.head >= sources.size() || !sources[column.head].derived || sources[column.head].preset == nullptr)
            continue;
        const DynamicPrintConfig &preset = sources[column.head].preset->config;
        int index = preset.get_index_for_extruder(int(column.head) + 1, "print_extruder_id", head_extruder_type(full, column.head, size_t(heads)),
                                                  column.volume_type, "print_extruder_variant");
        if (index < 0) {
            const auto *variants = preset.option<ConfigOptionStrings>("print_extruder_variant");
            if (variants != nullptr && variants->values.size() == 1 && variants->values.front() == column.variant)
                index = 0;
        }
        preset_columns[c] = index;
        if (index < 0) {
            std::vector<int> &fallback = sources[column.head].fallback_variants;
            if (std::find(fallback.begin(), fallback.end(), int(column.volume_type)) == fallback.end())
                fallback.emplace_back(int(column.volume_type));
        }
    }

    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        const auto *original = dynamic_cast<const ConfigOptionVectorBase*>(full.option(key));
        if (original == nullptr || original->empty())
            continue;
        const bool composed = composed_keys().count(key) > 0 && edited.count(key) == 0;
        std::unique_ptr<ConfigOptionVectorBase> selected(static_cast<ConfigOptionVectorBase*>(original->clone()));
        std::unique_ptr<ConfigOptionVectorBase> out(static_cast<ConfigOptionVectorBase*>(original->clone()));
        out->resize(columns.size());
        for (size_t c = 0; c < columns.size(); ++c) {
            const Column &column = columns[c];
            const ConfigOptionVectorBase *from = selected.get();
            int                           index = source_columns[c];
            if (composed && preset_columns[c] >= 0) {
                const auto *preset_option = dynamic_cast<const ConfigOptionVectorBase*>(sources[column.head].preset->config.option(key));
                if (preset_option != nullptr && !preset_option->empty() && preset_option->type() == out->type()) {
                    from  = preset_option;
                    index = preset_columns[c];
                }
            }
            out->set_at(from, c, size_t(index));
        }
        full.set_key_value(key, out.release());
    }

    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (const Column &column : columns) {
        ids.emplace_back(int(column.head) + 1);
        variants.emplace_back(column.variant);
    }
    full.set_key_value("print_extruder_id",      new ConfigOptionInts(std::move(ids)));
    full.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::move(variants)));
    full.set_key_value(source_column_key,        new ConfigOptionInts(std::move(source_columns)));
    return true;
}

} // namespace PerHeadProcess
} // namespace Slic3r
