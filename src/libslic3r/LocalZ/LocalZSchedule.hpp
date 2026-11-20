#ifndef slic3r_LocalZSchedule_hpp_
#define slic3r_LocalZSchedule_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r {

// Tool IDs are zero-based. Source indices in the result refer to the input
// vector, so consumers retain ownership of geometry and extrusion entities.
struct LocalZSchedulePass
{
    size_t                    object_layer     = 0;
    size_t                    pass_index       = 0;
    double                    print_z          = 0.0;
    size_t                    dependency_group = 0;
    size_t                    dependency_order = 0;
    std::vector<unsigned int> extruders;
};

struct LocalZScheduledPhase
{
    unsigned int        extruder_id = 0;
    double              print_z     = 0.0;
    std::vector<size_t> passes;
};

struct LocalZSchedule
{
    std::vector<LocalZScheduledPhase> phases;
    int                               final_extruder           = -1;
    bool                              used_dependency_fallback = false;
};

// Both purge prediction and G-code emission use this deterministic ordering.
// Inputs may be unsorted; empty-tool passes are ignored.
LocalZSchedule build_local_z_schedule(const std::vector<LocalZSchedulePass>& passes, int starting_extruder);

} // namespace Slic3r
#endif
