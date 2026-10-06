#pragma once

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace BambuFlowSupport {

// Bambu presets carry their Standard / High Flow values as extruder-variant slots: one slot
// per variant name in filament_extruder_variant (filaments), print_extruder_variant (process)
// and, for the printer, extruder_variant_list (which variants each extruder can take). In
// every Bambu dual-variant layout slot 0 is "<type> Standard" and slot 1 is "<type> High Flow"
// (docs/bambu-config-compat.md, D2), which is exactly Edge's ["standard", "high_flow"] flow
// layout. Slots 2+ (E3D High Flow, TPU High Flow, a second extruder, Bowden) stay inert.

// True when names[0] is "<type> Standard" and names[1] is "<type> High Flow" with the same
// <type> ("Direct Drive Standard", "Direct Drive High Flow"). "TPU High Flow" and
// "E3D High Flow" at slot 1 do not count.
bool variant_names_standard_high_flow(const std::vector<std::string> &names);

// True when some extruder of a printer can take "<type> High Flow" (extruder_variant_list
// entries are comma-separated variant lists, one per extruder) and no extruder is a Bowden
// one: the X2D's Bowden second extruder is out of phase 1 (owner decision D9).
bool printer_variants_offer_high_flow(const std::vector<std::string> &extruder_variant_list);

// Which keys derive() switched on.
struct DeriveResult
{
    bool filament = false;
    bool process  = false;
    bool printer  = false;
    bool any() const { return filament || process || printer; }
};

// Owner decision D1: switch on Edge's flow variants for a Bambu preset from its own variant
// names, without any profile JSON edit. Runs from Preset::normalize on a single preset
// (filament, process or printer config); a composed full config (it has keys of all three)
// is left alone. A *_flow_support key that is authored (anything but the default
// ["standard"]) always wins. Filament: filament_extruder_variant starts Standard / High Flow
// and filament_max_volumetric_speed has a value for the High Flow slot. Process:
// print_extruder_variant starts Standard / High Flow. Printer: extruder_variant_list offers
// High Flow (printer_variants_offer_high_flow). Idempotent.
DeriveResult derive(DynamicPrintConfig &config);

// Owner decision D6, Bambu MQTT nozzle flow code (2nd character of the nozzle type string):
// 'H' High Flow and 'E' E3D High Flow slice the High Flow column (E3D uses Bambu's own
// High Flow data); 'U' TPU High Flow, 'S' Standard and anything unknown slice Standard.
NozzleVolumeType nozzle_flow_from_device_code(char code);

// The column a nozzle_volume_type value (project / 3MF / device) slices with, same D6 rule:
// High Flow and E3D High Flow -> High Flow; Standard, TPU High Flow, Hybrid, unknown -> Standard.
FilamentVolumeType slicing_flow_of_nozzle(int nozzle_volume_type);

// Owner decision D2, dual-nozzle Bambu (H2D, H2D Pro, H2C): a filament slices the column of the
// nozzle its extruder carries. filament_map holds the 1-based logical extruder of each filament
// (1 = left, 2 = right), nozzle_volume_types one nozzle_volume_type value per logical extruder.
// Returns one entry per filament (filament_count); a filament without a map entry, or mapped
// to an extruder the list does not reach, slices Standard.
std::vector<FilamentVolumeType> filament_volume_types_from_map(const std::vector<int> &filament_map,
                                                               const std::vector<int> &nozzle_volume_types,
                                                               size_t                  filament_count);

// True when this printer config (a printer preset or a full config) declares "high_flow" in
// printer_flow_support.
bool printer_supports_high_flow(const ConfigBase &config);

// The slice-time half of D2. When the printer supports High Flow and has two or more
// nozzles, rewrites filament_volume_type in `config` from its filament_map and
// nozzle_volume_type. Returns true when a value changed. Called after the grouping wrote the
// final filament_map (ToolOrdering) so an auto-grouped filament on the High Flow nozzle slices
// the High Flow column.
bool apply_filament_volume_types_from_map(ConfigBase &config);

// Owner decision D4: the filaments (out of `filament_ids`) that a slice maps to High Flow but
// whose preset declares no High Flow column, so they slice their Standard values. `config` is a
// composed config (filament_volume_type, packed filament_flow_support, filament_flow_step_size).
std::vector<unsigned int> filaments_without_high_flow_column(const ConfigBase                &config,
                                                             const std::vector<unsigned int> &filament_ids);

}} // namespace Slic3r::BambuFlowSupport
