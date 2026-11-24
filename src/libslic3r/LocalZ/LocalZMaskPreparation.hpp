#ifndef slic3r_LocalZMaskPreparation_hpp_
#define slic3r_LocalZMaskPreparation_hpp_

#include "LocalZPlan.hpp"

namespace Slic3r {

constexpr double LOCAL_Z_PERIMETER_MASK_EXPAND_MM = 0.10;
// Base exclusion is smaller than pass inclusion to retain a slight overlap.
constexpr double LOCAL_Z_BASE_MASK_EXPAND_MM = 0.04;

ExPolygons local_z_compensate_masks(const ExPolygons& masks, float delta_scaled, bool fallback_to_source = true);

struct PreparedLocalZPassMasks
{
    std::vector<ExPolygons> by_extruder;
    ExPolygons              raw_union;
    size_t                  raw_polygon_count         = 0;
    size_t                  compensated_polygon_count = 0;
};

// Shared by purge planning and extrusion emission. Fixed paint takes precedence
// over expanded mixed masks; fixed paths retain their original mask boundary.
PreparedLocalZPassMasks prepare_local_z_pass_masks(const SubLayerPlan& plan);

} // namespace Slic3r
#endif
