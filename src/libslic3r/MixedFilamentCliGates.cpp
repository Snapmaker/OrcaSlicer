#include "MixedFilamentCliGates.hpp"

#include "Model.hpp"

#include <algorithm>
#include <boost/format.hpp>
#include <cstdlib>
#include <initializer_list>
#include <sstream>

namespace Slic3r {

// Defined in PrintObject.cpp; slicing and this helper must share the same region builder so
// object/part/range `extruder` assignments match Print::extruders().
PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config,
                                                  const DynamicPrintConfig *layer_range_config,
                                                  const ModelVolume        &volume,
                                                  size_t                    num_extruders);

std::string cli_mixed_filament_definitions(const DynamicPrintConfig &print_config, const DynamicPrintConfig *extra_config)
{
    if (extra_config) {
        if (const auto *opt = extra_config->option<ConfigOptionString>("mixed_filament_definitions");
            opt && !opt->value.empty())
            return opt->value;
    }
    if (const auto *opt = print_config.option<ConfigOptionString>("mixed_filament_definitions"))
        return opt->value;
    return {};
}

void populate_cli_mixed_filament_manager(MixedFilamentManager           &mgr,
                                          const DynamicPrintConfig       &print_config,
                                          const DynamicPrintConfig       *extra_config,
                                          const std::vector<std::string> &filament_colours,
                                          size_t                          num_physical)
{
    std::vector<std::string> colors = filament_colours;
    if (num_physical > 0)
        colors.resize(num_physical, colors.empty() ? std::string("#26A69A") : colors.back());

    mgr = MixedFilamentManager();
    mgr.auto_generate(colors);
    mgr.load_custom_entries(cli_mixed_filament_definitions(print_config, extra_config), colors);
}

void zero_mixed_flush_rows_and_cols(std::vector<double>        &flush_vol_matrix,
                                     size_t                      matrix_filament_count,
                                     const MixedFilamentManager &mgr,
                                     size_t                      num_physical)
{
    if (matrix_filament_count == 0)
        return;
    if (flush_vol_matrix.size() < matrix_filament_count * matrix_filament_count)
        return;
    auto is_mixed_slot = [&](int idx) {
        return mgr.is_mixed(static_cast<unsigned int>(idx + 1), num_physical);
    };
    for (size_t from_idx = 0; from_idx < matrix_filament_count; ++from_idx) {
        for (size_t to_idx = 0; to_idx < matrix_filament_count; ++to_idx) {
            if (from_idx == to_idx || is_mixed_slot(int(from_idx)) || is_mixed_slot(int(to_idx)))
                flush_vol_matrix[matrix_filament_count * from_idx + to_idx] = 0.f;
        }
    }
}

bool mixed_definitions_have_slot_without_filament(const std::string &serialized, size_t num_physical)
{
    if (serialized.empty())
        return false;

    std::stringstream all(serialized);
    std::string       row;
    while (std::getline(all, row, ';')) {
        if (row.empty())
            continue;
        std::vector<std::string> tokens;
        std::stringstream        ss(row);
        std::string              token;
        while (std::getline(ss, token, ','))
            tokens.emplace_back(token);
        if (tokens.size() < 2)
            continue;

        bool deleted = false;
        for (const std::string &tok : tokens) {
            if (tok == "d1") {
                deleted = true;
                break;
            }
        }
        if (deleted)
            continue;

        const int  a       = std::atoi(tokens[0].c_str());
        const int  b       = std::atoi(tokens[1].c_str());
        const bool enabled = tokens.size() < 3 || std::atoi(tokens[2].c_str()) != 0;
        if (!enabled || a <= 0 || b <= 0)
            continue;
        if (num_physical < 2 || static_cast<size_t>(a) > num_physical || static_cast<size_t>(b) > num_physical)
            return true;
    }
    return false;
}

namespace {

void append_positive_int_keys(const ConfigBase &cfg, std::initializer_list<const char *> keys, std::vector<int> &ids)
{
    for (const char *key : keys) {
        if (const ConfigOptionInt *opt = cfg.option<ConfigOptionInt>(key)) {
            if (opt->value > 0)
                ids.push_back(opt->value);
        }
    }
}

bool config_int_if_present(const ConfigBase *cfg, const char *key, int &out)
{
    if (cfg == nullptr)
        return false;
    if (const ConfigOption *opt = cfg->option(key)) {
        out = opt->getInt();
        return true;
    }
    return false;
}

int config_int_or(const ConfigBase &cfg, const char *key, int fallback)
{
    if (const ConfigOption *opt = cfg.option(key))
        return opt->getInt();
    return fallback;
}

double config_float_or(const ConfigBase &cfg, const char *key, double fallback)
{
    if (const ConfigOption *opt = cfg.option(key))
        return opt->getFloat();
    return fallback;
}

bool config_bool_or(const ConfigBase &cfg, const char *key, bool fallback)
{
    if (const ConfigOption *opt = cfg.option(key))
        return opt->getBool();
    return fallback;
}

// Same four feature-enable gates as PrintRegion::collect_object_printing_extruders
// (PrintRegion.cpp:128-137), but 1-based project ids are pushed without clamping to
// filament_diameter.size(). That clamp would map mixed virtual ids onto filament 1.
void append_gated_feature_filament_ids(const PrintRegionConfig &region, bool has_brim, std::vector<int> &ids)
{
    if (region.wall_loops.value > 0 || has_brim) {
        if (region.wall_filament.value > 0)
            ids.push_back(region.wall_filament.value);
        if (region.wall_loops.value > 0 && region.outer_wall_filament.value > 0)
            ids.push_back(region.outer_wall_filament.value);
    }
    if (region.sparse_infill_density.value > 0 && region.sparse_infill_filament.value > 0)
        ids.push_back(region.sparse_infill_filament.value);
    if ((region.top_shell_layers.value > 0 || region.bottom_shell_layers.value > 0) && region.solid_infill_filament.value > 0)
        ids.push_back(region.solid_infill_filament.value);
}

PrintRegionConfig effective_region(const ConfigBase &global, const ConfigBase *object_cfg, const ConfigBase *overlay)
{
    PrintRegionConfig region;
    region.apply(global, true);
    if (object_cfg != nullptr)
        region.apply(*object_cfg, true);
    if (overlay != nullptr)
        region.apply(*overlay, true);
    return region;
}

bool object_has_brim(const ModelObject &object, const DynamicPrintConfig &global)
{
    const int raft = object.config.has("raft_layers") ? object.config.option("raft_layers")->getInt() :
                                                       config_int_or(global, "raft_layers", 0);
    if (raft > 0)
        return false;
    const int type = object.config.has("brim_type") ? object.config.option("brim_type")->getInt() :
                                                      config_int_or(global, "brim_type", int(btNoBrim));
    const double width = object.config.has("brim_width") ? object.config.option("brim_width")->getFloat() :
                                                           config_float_or(global, "brim_width", 0.);
    return (type != int(btNoBrim) && width > 0.) || type == int(btAutoBrim) || type == int(btPainted);
}

bool object_prints_support(const ModelObject &object, const DynamicPrintConfig &global)
{
    const ConfigOption *obj_support = object.config.option("enable_support");
    const ConfigOption *obj_raft    = object.config.option("raft_layers");
    if (obj_support != nullptr || obj_raft != nullptr) {
        bool support = obj_support != nullptr && obj_support->getBool();
        if (obj_raft != nullptr)
            support |= obj_raft->getInt() > 0;
        return support;
    }
    return config_bool_or(global, "enable_support", false) || config_int_or(global, "raft_layers", 0) > 0;
}

void append_support_filament_ids(const ModelObject &object, const DynamicPrintConfig &global, std::vector<int> &ids)
{
    const int glb_support_intf = config_int_or(global, "support_interface_filament", 0);
    const int glb_support      = config_int_or(global, "support_filament", 0);
    int       obj_support_intf = object.config.has("support_interface_filament") ?
                               object.config.option("support_interface_filament")->getInt() : 0;
    int       obj_support      = object.config.has("support_filament") ?
                          object.config.option("support_filament")->getInt() : 0;
    if (obj_support_intf != 0)
        ids.push_back(obj_support_intf);
    else if (glb_support_intf != 0)
        ids.push_back(glb_support_intf);
    if (obj_support != 0)
        ids.push_back(obj_support);
    else if (glb_support != 0)
        ids.push_back(glb_support);
}

} // namespace

bool volume_contributes_feature_filaments(const ModelVolume &volume)
{
    return volume.is_modifier() || volume.is_model_part();
}

void append_feature_filament_overrides(const ConfigBase &cfg, std::vector<int> &ids)
{
    // Defaults fill in the enable flags (wall_loops, density, shells) when the overlay
    // only names a filament key. Only keys actually present on `cfg` are pushed, so a
    // modifier that sets wall_filament=2 does not also inherit the global sparse id.
    const PrintRegionConfig region = effective_region(cfg, nullptr, nullptr);
    if (cfg.option("wall_filament") && (region.wall_loops.value > 0) && region.wall_filament.value > 0)
        ids.push_back(region.wall_filament.value);
    if (cfg.option("outer_wall_filament") && region.wall_loops.value > 0 && region.outer_wall_filament.value > 0)
        ids.push_back(region.outer_wall_filament.value);
    if (cfg.option("sparse_infill_filament") && region.sparse_infill_density.value > 0 && region.sparse_infill_filament.value > 0)
        ids.push_back(region.sparse_infill_filament.value);
    if (cfg.option("solid_infill_filament") &&
        (region.top_shell_layers.value > 0 || region.bottom_shell_layers.value > 0) && region.solid_infill_filament.value > 0)
        ids.push_back(region.solid_infill_filament.value);
}

void append_config_filament_ids(const DynamicPrintConfig &cfg, std::vector<int> &ids)
{
    append_feature_filament_overrides(cfg, ids);
    if (config_bool_or(cfg, "enable_support", false) || config_int_or(cfg, "raft_layers", 0) > 0)
        append_positive_int_keys(cfg, {"support_filament", "support_interface_filament"}, ids);
}

void append_object_plate_filament_ids(const ModelObject &object, const DynamicPrintConfig &global_config, std::vector<int> &ids)
{
    const bool has_brim = object_has_brim(object, global_config);

    PrintRegionConfig default_region;
    default_region.apply(global_config, true);

    size_t num_physical = 0;
    if (const auto *opt = global_config.option<ConfigOptionFloats>("filament_diameter"))
        num_physical = opt->values.size();
    else if (const auto *opt = global_config.option<ConfigOptionStrings>("filament_colour"))
        num_physical = opt->values.size();
    std::vector<std::string> colors;
    if (const auto *opt = global_config.option<ConfigOptionStrings>("filament_colour"))
        colors = opt->values;
    MixedFilamentManager mgr;
    populate_cli_mixed_filament_manager(mgr, global_config, nullptr, colors, num_physical);
    const size_t num_total = std::max(std::max(mgr.total_filaments(num_physical), num_physical), size_t(1));

    std::vector<const ModelVolume *> parts;
    std::vector<const ModelVolume *> modifiers;
    for (const ModelVolume *mv : object.volumes) {
        if (mv == nullptr)
            continue;
        if (mv->is_model_part())
            parts.push_back(mv);
        else if (mv->is_modifier())
            modifiers.push_back(mv);
    }

    if (parts.empty()) {
        // Slot-gate objects often have no volumes; keep object-level feature keys and extruder.
        append_feature_filament_overrides(object.config.get(), ids);
        if (object.config.has("extruder")) {
            if (const int id = object.config.option("extruder")->getInt(); id > 0)
                ids.push_back(id);
        }
        if (object_prints_support(object, global_config))
            append_support_filament_ids(object, global_config, ids);
        return;
    }

    // Per MODEL_PART per existing layer_config_ranges entry (do not synthesize LayerRanges
    // gaps: a nullptr-config gap would push the default filament 1). `extruder` is applied
    // inside region_config_from_model_volume the same way slicing does.
    std::vector<PrintRegionConfig> parent_regions;
    parent_regions.reserve(parts.size() * std::max(object.layer_config_ranges.size(), size_t(1)));
    for (const ModelVolume *part : parts) {
        if (object.layer_config_ranges.empty()) {
            parent_regions.push_back(region_config_from_model_volume(default_region, nullptr, *part, num_total));
        } else {
            for (const auto &layer_range : object.layer_config_ranges)
                parent_regions.push_back(
                    region_config_from_model_volume(default_region, &layer_range.second.get(), *part, num_total));
        }
    }
    for (const PrintRegionConfig &region : parent_regions)
        append_gated_feature_filament_ids(region, has_brim, ids);

    for (const ModelVolume *modifier : modifiers) {
        for (const PrintRegionConfig &parent : parent_regions) {
            append_gated_feature_filament_ids(region_config_from_model_volume(parent, nullptr, *modifier, num_total),
                                              has_brim, ids);
        }
    }

    if (object_prints_support(object, global_config))
        append_support_filament_ids(object, global_config, ids);
}

int resolve_outer_wall_filament(const ConfigBase *object_config, const ConfigBase &global_config)
{
    int value = 0;
    if (!config_int_if_present(object_config, "outer_wall_filament", value))
        config_int_if_present(&global_config, "outer_wall_filament", value);

    int loops = 0;
    if (!config_int_if_present(object_config, "wall_loops", loops))
        config_int_if_present(&global_config, "wall_loops", loops);

    return (loops > 0 && value > 0) ? value : 0;
}

void collect_cli_filament_ids(const std::vector<Model> &models, const DynamicPrintConfig &print_config, std::vector<int> &ids)
{
    append_config_filament_ids(print_config, ids);
    for (const Model &model : models) {
        for (const ModelObject *obj : model.objects) {
            if (obj == nullptr)
                continue;
            append_object_plate_filament_ids(*obj, print_config, ids);
        }
    }
}

std::vector<unsigned int> mixed_physical_component_ids(const MixedFilament &mf, size_t num_physical)
{
    std::vector<unsigned int> ids;
    const std::string         norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (!norm.empty()) {
        for (const auto &group : MixedFilamentManager::split_pattern_groups(norm)) {
            for (const auto &token : MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical)) {
                const unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
                if (eid >= 1 && eid <= num_physical)
                    ids.push_back(eid);
            }
        }
    } else {
        if (mf.component_a >= 1)
            ids.push_back(mf.component_a);
        if (mf.component_b >= 1)
            ids.push_back(mf.component_b);
        for (unsigned int fid : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, num_physical)) {
            if (fid >= 1 && fid <= num_physical)
                ids.push_back(fid);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool mixed_components_differ_in_filament_type(const MixedFilament &mf, DynamicPrintConfig &cfg, size_t num_physical)
{
    const std::vector<unsigned int> ids = mixed_physical_component_ids(mf, num_physical);
    std::string                     first_type;
    for (unsigned int fid_1based : ids) {
        if (fid_1based < 1 || static_cast<size_t>(fid_1based) > num_physical)
            continue;
        std::string displayed;
        std::string type = cfg.get_filament_type(displayed, int(fid_1based - 1));
        if (type.empty())
            type = "PLA";
        if (first_type.empty())
            first_type = type;
        else if (type != first_type)
            return true;
    }
    return false;
}

CliMixedFilamentVerdict cli_check_mixed_filament_slots_have_filament(const MixedFilamentManager &mgr,
                                                                      const std::string           &mixed_defs,
                                                                      size_t                       num_physical,
                                                                      const std::vector<Model>   &models,
                                                                      const DynamicPrintConfig    &print_config,
                                                                      int                          filament_count)
{
    // Feature off (no serialized definitions, no enabled mixed rows): skip the gate entirely.
    if (mixed_defs.empty() && mgr.enabled_count() == 0)
        return CliMixedFilamentVerdict::pass();

    if (mixed_definitions_have_slot_without_filament(mixed_defs, num_physical)) {
        return CliMixedFilamentVerdict::fail(
            str(boost::format("mixed filament slot has no filament of its own, only %1% filaments are loaded; "
                               "load one filament per physical component, including each mixed one")
                % filament_count));
    }

    std::vector<int> used_ids;
    collect_cli_filament_ids(models, print_config, used_ids);
    for (int id : used_ids) {
        if (id <= int(num_physical))
            continue;
        if (mgr.is_mixed(static_cast<unsigned int>(id), num_physical))
            continue;
        return CliMixedFilamentVerdict::fail(
            str(boost::format("mixed filament slot %1% has no filament of its own, only %2% filaments are loaded; "
                               "load one filament per slot, including each mixed one")
                % id % filament_count));
    }

    return CliMixedFilamentVerdict::pass();
}

CliMixedFilamentVerdict cli_check_mixed_filament_type_compatibility(const MixedFilamentManager &mgr,
                                                                     const std::vector<int>      &plate_slots,
                                                                     size_t                        num_physical,
                                                                     DynamicPrintConfig           &plate_config,
                                                                     int                            plate_index_1based)
{
    if (mgr.enabled_count() == 0)
        return CliMixedFilamentVerdict::pass();

    for (int slot_1based : plate_slots) {
        if (slot_1based <= 0)
            continue;
        if (!mgr.is_mixed(static_cast<unsigned int>(slot_1based), num_physical))
            continue;
        const MixedFilament *mf = mgr.mixed_filament_from_id(static_cast<unsigned int>(slot_1based), num_physical);
        if (mf == nullptr)
            continue;
        if (!mixed_components_differ_in_filament_type(*mf, plate_config, num_physical))
            continue;
        return CliMixedFilamentVerdict::fail(
            str(boost::format("plate %1%: mixed filament %2% mixes components of different filament types")
                % plate_index_1based % slot_1based));
    }

    return CliMixedFilamentVerdict::pass();
}

} // namespace Slic3r
