#pragma once

// Side stabilizers: resin-style support struts for FDM prints.
//
// A tall, slender part (a spire, a sword, a figure's staff) wobbles under the nozzle long before it
// has any overhang to support. Stabilizers place rings of contact points on the part's SIDES every
// few millimetres of height and hand them to the SLA support tree builder (libslic3r/SLA), which
// grows a pinhead for each point - a thin tip that meets the wall from below at the bridge slope -
// and routes a pillar from it to the bed, cross-bracing neighbouring pillars. The resulting mesh is
// sliced at the object's layers and printed as support material, so it uses the support filament,
// speed and flow, and breaks off at the pinpoint touch.
//
// They are an addition to the normal or tree supports: they run in the support step, only for
// objects with supports enabled, and they never replace what those generators made.

#include "../ExPolygon.hpp"
#include "../SLA/SupportPoint.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

class PrintObject;

namespace stabilizers {

struct RingParams
{
    // Vertical distance between two rings of contact points, mm. The first ring sits one spacing
    // above the bed.
    double ring_spacing      = 15.;
    // Contact points per island in each ring, spread evenly around it.
    int    points_per_ring   = 3;
    // Diameter of the pinpoint tip where it touches the part, mm.
    double tip_diameter      = 0.8;
    // Islands wider than this (the smaller side of their bounding box) are not stabilized, mm.
    // 0 = no limit. Keeps a wide base under a thin spire free of touch marks.
    double max_island_width  = 20.;
    // No ring closer than this below the part's top, mm, so the topmost tip still has a wall to
    // land on.
    double top_margin        = 1.;
};

// One object layer as the ring placer sees it: its slicing height (object frame) and outline.
struct LayerOutline
{
    float             slice_z;
    const ExPolygons *islands;
};

// Contact points on the outlines, ring by ring. Layers must be sorted by slice_z. Each ring is
// rotated by half a step against the one below, so tips don't stack into a single seam.
// Exposed for tests.
sla::SupportPoints ring_points(const std::vector<LayerOutline> &layers, const RingParams &params);

} // namespace stabilizers

// Generates the stabilizers for `object` and adds them to its support layers, inserting support
// layers at object layer heights where none exist. No-op unless the object's stabilizer_supports
// option is on. Must run after the regular support generator, inside the support step.
void generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
