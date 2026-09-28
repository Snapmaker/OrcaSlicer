#pragma once
#ifndef slic3r_ProjectConfigFill_hpp_
#define slic3r_ProjectConfigFill_hpp_

// Fill printer/process keys that are absent from a CLI project config from the matching
// system preset (OrcaSlicer #15953, adapted onto Edge's NamedPresets path).
// Header-only so Catch2 can cover it from tests/libslic3r/test_config.cpp without a
// CMake source-list change.

#include "PrintConfig.hpp"

#include <set>
#include <string>
#include <vector>

namespace Slic3r {

// Keys never copied from a system printer/process preset into a project. Identity and
// print-host keys match upstream #15953; the rest are Edge-only (flow-variant layout,
// filament mapping, MixedFilamentManager). Filament vector keys are out of scope.
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
        "thumbnails",
        "thumbnails_format",
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
        // filament mapping (project-owned)
        "filament_map",
        "filament_map_2",
        "filament_map_mode",
        "enable_filament_dynamic_map",
        "has_filament_switcher",
        // Edge #117 mapping config (not on main yet; skip so a later merge does not fill them)
        "filament_mapping_protocol",
        "filament_physical_map",
        "physical_filament_maps",
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
    };
    return skip;
}

// True when handle_legacy drops the key on load (obsolete, e.g. silent_mode). Those keys
// can never be in a loaded project, so they do not count as missing.
inline bool project_config_key_dropped_on_load(std::string key)
{
    std::string value;
    PrintConfigDef::handle_legacy(key, value);
    return key.empty();
}

// Copies keys listed in `options` that are absent from `project` out of `system`.
// Never overwrites present keys, skip-list keys, or keys handle_legacy drops on load.
// Returns the number of keys copied. Optionally records the copied keys.
inline size_t fill_missing_project_keys(DynamicPrintConfig              &project,
                                        const DynamicPrintConfig        &system,
                                        const std::vector<std::string>  &options,
                                        std::vector<std::string>        *filled_keys = nullptr)
{
    const auto &skip = project_config_fill_skip_keys();
    size_t      n    = 0;
    for (const std::string &key : options) {
        if (skip.count(key) || project.option(key) != nullptr || project_config_key_dropped_on_load(key))
            continue;
        const ConfigOption *opt = system.option(key);
        if (opt == nullptr)
            continue;
        project.set_key_value(key, opt->clone());
        if (filled_keys)
            filled_keys->push_back(key);
        ++n;
    }
    return n;
}

} // namespace Slic3r

#endif // slic3r_ProjectConfigFill_hpp_
