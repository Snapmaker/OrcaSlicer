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
//
// Painted stabilizer points: the user can paint spots on a part (paint-on supports, state
// EnforcerBlockerType::STABILIZER) where a strut must always touch, whatever the ring settings say.
// They are added on top of the rings and planned by the same rules.
//
// The planner and the strut slicer work on plain layer outlines and a StabilizerSettings, not on a
// PrintObject, so the bake (Support/StabilizerBake.hpp) and the tests can run them too.

#include "../ExPolygon.hpp"
#include "../SLA/SupportPoint.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;
class PrintObject;
class PrintObjectConfig;

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

// Everything the planner and the strut slicer read. from_config() is what a PrintObject's
// stabilizer settings resolve to.
struct StabilizerSettings
{
    RingParams rings;
    // Space left between each tip and the part, mm.
    double     tip_gap       = 0.;
    // Pillar radius, mm: the configured diameter, never less than two support lines on each side.
    double     pillar_radius = 1.;
    // The pillar stands this far off anything of the part below it, mm.
    double     clearance     = 1.;
    // The longest strut: how far out (and down) from its tip its pillar may stand, mm.
    double     max_run       = 10.;
    // What gets struts (stabilizer_supports): Auto = rings and painted points, Manual = painted points
    // only, Off = nothing.
    bool       ring_struts    = true;
    bool       painted_points = true;

    // `support_line_width` is the object's support material flow width, mm.
    static StabilizerSettings from_config(const PrintObjectConfig &cfg, double support_line_width);
};

// The settings of a PrintObject (its config and its support flow).
StabilizerSettings settings_of(const PrintObject &object);

// One object layer as the ring placer sees it: its slicing height (object frame) and outline.
struct LayerOutline
{
    float             slice_z;
    const ExPolygons *islands;
};

// The outlines of a sliced PrintObject's layers (Layer::lslices). Valid while the layers are.
std::vector<LayerOutline> outlines_of(const PrintObject &object);

// A contact point on the part's wall.
struct Contact
{
    Vec2d  pos;   // on the outline, mm
    Vec2d  dir;   // unit, pointing away from the part
    size_t layer; // index of the layer the ring was placed on
    float  z;     // that layer's slice_z
    Vec2d  normal = Vec2d::Zero(); // the outline's outward normal there (unit; zero when unknown)
};

// Contact points on the outlines, ring by ring. Layers must be sorted by slice_z. Every ring uses
// the same angles. Exposed for tests.
std::vector<Contact> ring_contacts(const std::vector<LayerOutline> &layers, const RingParams &params);
// The same, as SLA support points (pos at the ring layer's slice_z, radius = tip radius).
sla::SupportPoints   ring_points(const std::vector<LayerOutline> &layers, const RingParams &params);

// A spot the user painted as a stabilizer point, in the planner's frame: XY in the object's sliced
// (print) coordinates, Z its height above the object's bottom, mm. `normal` is the painted
// surface's outward normal there (unit).
struct PaintedSpot
{
    Vec3d pos;
    Vec3d normal;
};

// The painted stabilizer points of the object's model parts (supported_facets in the STABILIZER
// state), with `trafo` taking the object's coordinates to the planner's frame. One spot per
// connected painted patch; a patch taller than `ring_spacing` gives one spot per ring spacing of
// its height, so painting a strip up the part asks for a strut every ring.
std::vector<PaintedSpot> painted_spots(const ModelObject &object, const Transform3d &trafo, double ring_spacing);
// The same for a PrintObject (its model object, through trafo_centered()).
std::vector<PaintedSpot> painted_spots(const PrintObject &object);

// One strut: from its tip on the wall it runs `run` mm outwards along `dir` while dropping the
// same height (45 degrees) to the top of its pillar, which stands on the bed.
struct Strut
{
    Vec2d  tip;
    Vec2d  dir;
    size_t tip_layer = 0;
    double tip_z     = 0.;
    double run       = 0.;
    // Placed for a painted spot rather than by the rings.
    bool   painted   = false;
    // The wall's outward normal at the tip (unit; zero when unknown). Where the wall is not square to
    // `dir` (a part that is not round), the baked tip is cut along the wall rather than across `dir`.
    Vec2d  normal    = Vec2d::Zero();

    Vec2d  pillar() const { return tip + dir * run; }
    double junction_z() const { return tip_z - run; }
    // Where the strut's axis crosses height z.
    Vec2d  axis_at(double z) const { return tip + dir * (tip_z - z); }
};

// The strut as it is built with a tip gap: its tip moved back along its own axis by the gap - out
// from the wall and down by as much - so it tapers to the tip diameter at the gap-trimmed end and
// is the same cone to a point at every gap, only set back. Pillar and junction stay where they are.
// slice_struts and the baked mesh both build from this.
Strut gapped(const Strut &s, double gap);

// What the planner did with the painted spots.
struct PlanReport
{
    size_t painted             = 0;  // spots asked for
    size_t painted_placed      = 0;  // got a strut of their own
    size_t painted_on_ring     = 0;  // a ring strut already touches there
    // Manual stabilizers without a single painted point: nothing to place.
    bool   manual_without_paint = false;
    // Spots no strut can reach under the printability rules (45 degree climb, a pillar clear of the
    // part, nothing floating), and where they are.
    std::vector<Vec3d> unreachable;
};

// Radius of the stabilizer pillars of `object`: the configured diameter, but never less than two
// support lines on each side.
double pillar_radius(const PrintObject &object);

// The struts for the layers, from the settings: the rings' contacts, then one strut for every
// painted spot that no ring strut already touches. A ring contact whose pillar can't reach the bed
// clear of the part, within a max_run strut, is dropped; a painted spot that can't is reported.
std::vector<Strut> plan_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings,
                               const std::vector<PaintedSpot> &painted = {}, PlanReport *report = nullptr);
// The struts for `object` (sliced, layers built), from its stabilizer settings and painted points.
std::vector<Strut> plan_struts(const PrintObject &object, PlanReport *report = nullptr);

// The struts' cross-sections at the layers, clipped by the part's outline (grown by the tip gap):
// one entry per layer, the areas that print as stabilizer.
std::vector<ExPolygons> slice_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings,
                                     const std::vector<Strut> &struts, const std::function<void()> &throw_if_canceled);
std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled);

} // namespace stabilizers

// Generates the stabilizers for `object` and adds them to its support layers, inserting support
// layers at object layer heights where none exist. No-op unless the object's stabilizer_supports
// option is on. Must run after the regular support generator, inside the support step. Returns what
// the planner did with the painted points, so the caller can warn about the unreachable ones.
stabilizers::PlanReport generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
