#pragma once

// Side stabilizers: pinpoint struts for tall, thin FDM parts.
//
// A tall, slender part (a spire, a sword, a figure's staff) wobbles under the nozzle long before it
// has any overhang to support. Stabilizers put rings of contact points on the part's SIDES every
// few millimetres of height. Each contact gets a strut that climbs to it at 45 degrees from a
// vertical pillar standing on the bed, and tapers to a small tip that touches the wall. Contacts
// at the same angle in every ring share one pillar, so each pillar is tied to the part at every
// ring. Struts and pillars are built directly as layer cross-sections, each layer resting on the
// one below, and printed as support material, so they use the support filament, speed and flow,
// and break off at the pinpoint touch.
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

// A contact point on the part's wall.
struct Contact
{
    Vec2d  pos;   // on the outline, mm
    Vec2d  dir;   // unit, pointing away from the part
    size_t layer; // index of the layer the ring was placed on
    float  z;     // that layer's slice_z
};

// Contact points on the outlines, ring by ring. Layers must be sorted by slice_z. Every ring uses
// the same angles. Exposed for tests.
std::vector<Contact> ring_contacts(const std::vector<LayerOutline> &layers, const RingParams &params);
// The same, as SLA support points (pos at the ring layer's slice_z, radius = tip radius).
sla::SupportPoints   ring_points(const std::vector<LayerOutline> &layers, const RingParams &params);

// One strut: from its tip on the wall it runs `run` mm outwards along `dir` while dropping the
// same height (45 degrees) to the top of its pillar, which stands on the bed.
struct Strut
{
    Vec2d  tip;
    Vec2d  dir;
    size_t tip_layer = 0;
    double tip_z     = 0.;
    double run       = 0.;

    Vec2d  pillar() const { return tip + dir * run; }
    double junction_z() const { return tip_z - run; }
    // Where the strut's axis crosses height z.
    Vec2d  axis_at(double z) const { return tip + dir * (tip_z - z); }
};

// Radius of the stabilizer pillars of `object`: the configured diameter, but never less than two
// support lines on each side.
double pillar_radius(const PrintObject &object);

// The struts for `object` (sliced, layers built), from its stabilizer settings. A contact whose
// pillar can't reach the bed clear of the part, within a 10 mm strut, is dropped.
std::vector<Strut> plan_struts(const PrintObject &object);

// The struts' cross-sections at the object's layers, clipped by the part's outline (grown by the
// tip gap): one entry per object layer, the areas that print as stabilizer.
std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled);

} // namespace stabilizers

// Generates the stabilizers for `object` and adds them to its support layers, inserting support
// layers at object layer heights where none exist. No-op unless the object's stabilizer_supports
// option is on. Must run after the regular support generator, inside the support step.
void generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
