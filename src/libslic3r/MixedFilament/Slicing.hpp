#ifndef slic3r_MixedFilamentSlicing_hpp_
#define slic3r_MixedFilamentSlicing_hpp_
#include "../MixedFilament.hpp"
#include "../ExPolygon.hpp"
namespace Slic3r {
class DynamicPrintConfig;
class Layer;
// Internal policy shared by segmentation and Local-Z planning.
namespace MixedFilamentSlicing {
bool                      bool_from_full_config(const DynamicPrintConfig& full_cfg, const char* key, bool fallback);
coordf_t                  float_from_full_config(const DynamicPrintConfig& full_cfg, const char* key, coordf_t fallback);
unsigned int              segmentation_channel_filament_id(size_t channel_idx);
std::vector<unsigned int> decode_gradient_component_ids(const MixedFilament& mf, size_t num_physical);
std::vector<int>          decode_gradient_component_weights(const MixedFilament& mf, size_t expected_components);
std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int>& ids,
                                                           const std::vector<int>&          weights,
                                                           size_t                           max_cycle_limit = 0);
bool                      local_z_eligible_mixed_definition(const MixedFilament& definition);
ExPolygons                collect_layer_region_slices(const Layer& layer);
} // namespace MixedFilamentSlicing
} // namespace Slic3r
#endif
