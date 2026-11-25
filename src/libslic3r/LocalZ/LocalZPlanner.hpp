#ifndef slic3r_LocalZPlanner_hpp_
#define slic3r_LocalZPlanner_hpp_

#include "../ExPolygon.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace Slic3r {
class Print;
class PrintObject;

// A painted island that would print at the nominal height, in its physical filament on this layer.
struct LocalZNominalZone
{
    unsigned int filament;
    ExPolygons   islands;
};
using LocalZNominalZones = std::vector<LocalZNominalZone>;

struct LocalZPlanningInput
{
    std::vector<std::vector<ExPolygons>> segmentation;
    std::vector<LocalZNominalZones>      nominal_zones;
    bool                                 whole_object_scope_active = false;
};

std::vector<uint8_t>                 local_z_enabled_mixed_rows(const Print& print);
std::vector<std::vector<ExPolygons>> local_z_planner_segmentation_with_whole_object_mixed_wall(
    const PrintObject&                          print_object,
    const std::vector<std::vector<ExPolygons>>& segmentation,
    const std::vector<uint8_t>&                 local_z_enabled_rows,
    bool&                                       whole_object_scope_active);

// Called only after compensation, repair and polyholes have finalized slices.
// Returns true when part of a mixed region could not be divided within the nozzle layer
// height limits and was printed with a single component instead.
bool finalize_local_z_plan(PrintObject& object, LocalZPlanningInput input, const std::function<void()>& throw_on_cancel);
} // namespace Slic3r
#endif
