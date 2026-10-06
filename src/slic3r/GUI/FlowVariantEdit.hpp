#ifndef slic3r_GUI_FlowVariantEdit_hpp_
#define slic3r_GUI_FlowVariantEdit_hpp_

// Editor-only helpers for copying / dual-writing flow-variant preset slots.
// Do not call these from slice routing: Both is not a FilamentVolumeType / *_flow_support value.

#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

const std::vector<std::string> &flow_variant_option_keys(ConfigFlowDomain domain);

// Copy every domain flow-variant key from from_mode onto to_mode.
// Operates on preset-sized vectors (support indices), not composed get_config_idx layout.
// Returns true if any option object changed.
bool copy_flow_variant_slot(DynamicPrintConfig       &config,
                            ConfigFlowDomain          domain,
                            const std::string        &from_mode,
                            const std::string        &to_mode);

bool flow_variant_slots_differ(const DynamicPrintConfig &config,
                               ConfigFlowDomain          domain,
                               const std::string        &mode_a,
                               const std::string        &mode_b);

// Ensure the domain's *_flow_support key contains `mode`, appending it when
// missing. An absent or empty key is treated as ["standard"] first, so a
// filament-tab switch to High flow persists as ["standard","high_flow"].
// Existing modes are preserved, nothing is duplicated. Returns true when the
// option object changed (callers then mark the preset dirty).
bool ensure_flow_support_mode(DynamicPrintConfig       &config,
                              ConfigFlowDomain          domain,
                              const std::string        &mode);

// Copy source_index onto every other index in modes for a single key.
// Replaces the whole option object so undo/dirty see one vector write.
bool replicate_flow_variant_value(DynamicPrintConfig          &config,
                                  const std::string           &opt_key,
                                  size_t                       source_index,
                                  const std::vector<std::string> &modes);

// Slot the filament tab / ConfigManipulation must read for a flow-variant key.
// Matches Tab::flow_variant_view_index() (0 Standard, 1 High-Flow). When the
// filament has no variants the view index is 0, so non-U1 behaviour is unchanged.
// Non-variant keys stay on index 0 even if the High-Flow view is selected.
int filament_tab_option_index(const std::string &key, size_t view_index);

// Range warnings used by ConfigManipulation. nozzle_temperature_range_* stay
// per-filament (index 0); only the temperature value follows the view index.
bool filament_nozzle_temperature_out_of_range(const DynamicPrintConfig &config, int variant_index);
bool filament_nozzle_temperature_initial_layer_out_of_range(const DynamicPrintConfig &config, int variant_index);

} // namespace GUI
} // namespace Slic3r

#endif
