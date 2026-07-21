#ifndef slic3r_GUI_FlowTypeHelper_hpp_
#define slic3r_GUI_FlowTypeHelper_hpp_

#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace FlowType {

// Snapmaker requirement 7.1: all functions read/write wxGetApp().preset_bundle state.
//
// The per-nozzle flow combo (nozzle_volume_type), the grouping mode chosen in the
// slice-button hover popup (filament_grouping_mode) and the per-filament mapping
// edited in the grouping dialog (filament_volume_type) are three independent
// pieces of state: changing one never rewrites another. The only shared gate is
// printer_supports_high_flow() -- whether the edited printer preset declares
// "high_flow" in printer_flow_support at all.

// True when the edited printer preset declares "high_flow" in printer_flow_support.
bool printer_supports_high_flow();

// Per-nozzle flow types from project config, resized to the nozzle count and
// normalized (unknown entries -> standard; everything standard when the printer
// preset does not support high flow).
std::vector<std::string> nozzle_volume_types();

// Writes one nozzle's flow type and marks the project dirty.
void set_nozzle_volume_type(size_t nozzle_idx, const std::string &volume_type);

// Grouping mode (FILAMENT_GROUPING_STANDARD / FILAMENT_GROUPING_CUSTOM).
std::string grouping_mode();

// Writes the grouping mode and marks the project dirty.
void set_grouping_mode(const std::string &mode);

// Writes the dialog's per-filament mapping and invalidates the slice result.
void apply_custom_mapping(const std::vector<FilamentVolumeType> &mapping);

}}} // namespace Slic3r::GUI::FlowType

#endif // slic3r_GUI_FlowTypeHelper_hpp_
