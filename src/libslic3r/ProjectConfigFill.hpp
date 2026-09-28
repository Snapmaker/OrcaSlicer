#pragma once
#ifndef slic3r_ProjectConfigFill_hpp_
#define slic3r_ProjectConfigFill_hpp_

// Fill printer/process keys that are absent from a CLI project config from the matching
// system preset (OrcaSlicer #15953, adapted onto Edge's NamedPresets path).
// Header-only so Catch2 can cover it from tests/libslic3r/test_config.cpp without a
// CMake source-list change.

#include "PrintConfig.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

// Keys never copied from a system printer/process preset into a project. Identity and
// print-host keys match upstream #15953; the rest are Edge-only (flow-variant layout,
// filament mapping, MixedFilamentManager, s_project_options). Filament vector keys are
// out of scope. filament_mapping_protocol is a printer capability and is filled (S4).
inline const std::set<std::string> &project_config_fill_skip_keys()
{
    static const std::set<std::string> skip = {
        // identity / meta (GUI bookkeeping; never take these from a system preset)
        "inherits",
        "name",
        "from",
        "type",
        "version",
        "setting_id",
        "instantiation",
        "model_id",
        "compatible_printers",
        "compatible_prints",
        "compatible_printers_condition",
        "compatible_prints_condition",
        "print_settings_id",
        "filament_settings_id",
        "printer_settings_id",
        // print-host: the GUI never takes these from a project
        "print_host",
        "print_host_webui",
        "printhost_apikey",
        "printhost_cafile",
        "printhost_user",
        "printhost_password",
        "printhost_port",
        "printhost_authorization_type",
        "printhost_ssl_ignore_revoke",
        "host_type",
        "flashforge_serial_number",
        "bbl_use_printhost",
        // extruder variant layout: an older project must keep a consistent layout
        "printer_extruder_id",
        "printer_extruder_variant",
        "print_extruder_id",
        "print_extruder_variant",
        "extruder_variant_list",
        "filament_extruder_variant",
        "physical_extruder_map",
        // flow-variant / nozzle volume (project-owned or derived; Edge #169 uses filament_volume_type)
        "nozzle_volume_type",
        "filament_volume_type",
        "default_nozzle_volume_type",
        "extruder_nozzle_volume_type",
        "filament_volume_map",
        "filament_nozzle_map",
        "filament_flow_support",
        "process_flow_support",
        "printer_flow_support",
        // filament mapping (project-owned). filament_mapping_protocol is a printer capability: fill it.
        "filament_map",
        "filament_map_2",
        "filament_map_mode",
        "enable_filament_dynamic_map",
        "enable_filament_mapping",
        "has_filament_switcher",
        "device_tool_count",
        "device_changer",
        // Edge #117 mapping config (not on main yet; skip so a later merge does not fill them)
        "filament_physical_map",
        "flush_volumes_synced",
        // MixedFilamentManager owns these; never fill from a preset (never filament_is_mixed)
        "mixed_filament_definitions",
        "mixed_filament_gradient_mode",
        "mixed_filament_height_lower_bound",
        "mixed_filament_height_upper_bound",
        "mixed_filament_advanced_dithering",
        "mixed_filament_component_bias_enabled",
        "mixed_filament_surface_indentation",
        "mixed_filament_region_collapse",
        "mixed_filament_auto_gradient_choice",
        "mixed_filament_auto_gradient_physical_count",
        "mixed_color_layer_height_a",
        "mixed_color_layer_height_b",
        "mixed_filament_pointillism_pixel_size",
        "mixed_filament_pointillism_line_gap",
        // Edge project-owned keys from s_project_options (PresetBundle.cpp)
        "dithering_local_z_mode",
        "dithering_local_z_whole_objects",
        "dithering_local_z_infill",
        "wipe_tower_rotation_angle",
        "extruder_ams_count",
        "extruder_nozzle_stats",
    };
    return skip;
}

// True when handle_legacy drops a *known* key on load (obsolete, e.g. silent_mode).
// Unknown/future keys (filament_mapping_protocol before Edge #117 lands in PrintConfigDef)
// are not treated as dropped so they can still be filled from a system option (S4).
inline bool project_config_key_dropped_on_load(std::string key)
{
    if (!print_config_def.has(key))
        return false;
    std::string value;
    PrintConfigDef::handle_legacy(key, value);
    return key.empty();
}

// --printer-preset / --process-preset replace the project printer/process; do not fill those.
inline bool cli_fill_from_system_preset(const std::string &new_preset_cli_name) { return new_preset_cli_name.empty(); }

// inherits_group layout: [process, filament..., printer], size == filament_count + 2.
// Missing, empty-slot, or mis-sized group falls back to current_name. No renamed_from / alias lookup.
inline std::string resolve_project_system_preset_name(const std::string               &current_name,
                                                      const std::vector<std::string> *inherits_group,
                                                      size_t                           filament_count,
                                                      bool                             printer)
{
    if (inherits_group == nullptr || inherits_group->size() != filament_count + 2)
        return current_name;
    const std::string &slot = printer ? inherits_group->back() : inherits_group->front();
    return slot.empty() ? current_name : slot;
}

inline std::string project_config_fill_log_value(const ConfigOption *opt, size_t max_len = 96)
{
    if (opt == nullptr)
        return {};
    std::string s = opt->serialize();
    if (s.size() > max_len)
        s.replace(s.begin() + static_cast<std::ptrdiff_t>(max_len), s.end(), "...");
    return s;
}

// Keys listed in `options` that are absent from the project (skip / dropped-on-load excluded).
// When `present_keys` is set it is the snapshot of config.keys() right after the 3MF load,
// so create=true defaults inserted later are still treated as missing (S2).
inline std::vector<std::string> missing_project_keys(const DynamicPrintConfig         &project,
                                                     const std::vector<std::string>   &options,
                                                     const std::set<std::string>      *present_keys = nullptr)
{
    const auto             &skip = project_config_fill_skip_keys();
    std::vector<std::string> missing;
    missing.reserve(options.size());
    for (const std::string &key : options) {
        if (skip.count(key) || project_config_key_dropped_on_load(key))
            continue;
        const bool present = present_keys ? present_keys->count(key) > 0 : project.option(key) != nullptr;
        if (!present)
            missing.push_back(key);
    }
    return missing;
}

inline bool project_config_had_key(const DynamicPrintConfig    &project,
                                   const std::string           &key,
                                   const std::set<std::string> *present_keys)
{
    return present_keys ? present_keys->count(key) > 0 : project.option(key) != nullptr;
}

inline size_t project_config_nozzle_count(const DynamicPrintConfig &cfg)
{
    if (const auto *nd = cfg.option<ConfigOptionFloats>("nozzle_diameter"))
        return nd->values.size();
    return 0;
}

inline bool project_config_flow_support_differs(const DynamicPrintConfig    &project,
                                                const DynamicPrintConfig    &system,
                                                const char                  *key,
                                                const std::set<std::string> *present_keys)
{
    if (!project_config_had_key(project, key, present_keys))
        return false;
    const ConfigOption *p = project.option(key);
    const ConfigOption *s = system.option(key);
    if (p == nullptr || s == nullptr)
        return false;
    return p->serialize() != s->serialize();
}

// Copies keys listed in `options` that are absent from `project` out of `system`.
// Never overwrites keys present in the 3MF (or, without a snapshot, keys already on `project`).
// Skip-list keys, keys handle_legacy drops on load, and flow-variant vectors whose
// process_flow_support / printer_flow_support differs from the project are not copied.
// Per-extruder vectors are resized to the project's nozzle_diameter size (S5).
// Returns the number of keys copied. Optionally records the copied keys.
inline size_t fill_missing_project_keys(DynamicPrintConfig             &project,
                                        const DynamicPrintConfig       &system,
                                        const std::vector<std::string> &options,
                                        std::vector<std::string>       *filled_keys  = nullptr,
                                        const std::set<std::string>    *present_keys = nullptr)
{
    const std::vector<std::string> missing = missing_project_keys(project, options, present_keys);
    if (missing.empty())
        return 0;

    const bool skip_process_flow = project_config_flow_support_differs(project, system, "process_flow_support", present_keys);
    const bool skip_printer_flow = project_config_flow_support_differs(project, system, "printer_flow_support", present_keys);
    const auto &extruder_keys    = print_config_def.extruder_option_keys();
    size_t      n                = 0;
    for (const std::string &key : missing) {
        if ((skip_process_flow && is_process_flow_variant_option(key)) ||
            (skip_printer_flow && is_machine_flow_variant_option(key)))
            continue;
        const ConfigOption *opt = system.option(key);
        if (opt == nullptr)
            continue;
        ConfigOption *cloned = opt->clone();
        if (cloned->is_vector()) {
            auto *vec = static_cast<ConfigOptionVectorBase *>(cloned);
            if (is_process_flow_variant_option(key) && project_config_had_key(project, "process_flow_support", present_keys)) {
                if (const auto *fs = project.option<ConfigOptionStrings>("process_flow_support"))
                    if (!fs->values.empty() && vec->size() != fs->values.size())
                        vec->resize(fs->values.size());
            } else if (is_machine_flow_variant_option(key) &&
                       project_config_had_key(project, "printer_flow_support", present_keys)) {
                if (const auto *fs = project.option<ConfigOptionStrings>("printer_flow_support"))
                    if (!fs->values.empty() && vec->size() != fs->values.size())
                        vec->resize(fs->values.size());
            } else if (std::find(extruder_keys.begin(), extruder_keys.end(), key) != extruder_keys.end()) {
                size_t nozzles = project_config_nozzle_count(project);
                if (nozzles == 0)
                    nozzles = project_config_nozzle_count(system);
                if (nozzles > 0 && vec->size() != nozzles)
                    vec->resize(nozzles);
            }
        }
        project.set_key_value(key, cloned);
        if (filled_keys)
            filled_keys->push_back(key);
        ++n;
    }
    return n;
}

} // namespace Slic3r

#endif // slic3r_ProjectConfigFill_hpp_
