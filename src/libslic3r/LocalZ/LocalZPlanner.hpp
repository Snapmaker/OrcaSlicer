#ifndef slic3r_LocalZPlanner_hpp_
#define slic3r_LocalZPlanner_hpp_

#include "../ExPolygon.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace Slic3r {
class Print;
class PrintObject;

struct LocalZPlanningInput
{
    std::vector<std::vector<ExPolygons>> segmentation;
    bool                                 whole_object_scope_active = false;
};

std::vector<uint8_t>                 local_z_enabled_mixed_rows(const Print& print);
std::vector<std::vector<ExPolygons>> local_z_planner_segmentation_with_whole_object_mixed_wall(
    const PrintObject&                          print_object,
    const std::vector<std::vector<ExPolygons>>& segmentation,
    const std::vector<uint8_t>&                 local_z_enabled_rows,
    bool&                                       whole_object_scope_active);

// Called only after compensation, repair and polyholes have finalized slices.
void finalize_local_z_plan(PrintObject& object, LocalZPlanningInput input, const std::function<void()>& throw_on_cancel);
} // namespace Slic3r
#endif
