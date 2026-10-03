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
//
// v2 (all off by default, so v1 output is unchanged unless asked for):
//  * tapered pillars: wider at the bed, narrowing linearly to the pillar diameter at their own top;
//  * pillar-to-pillar bracing: 45 degree diagonals between neighbouring pillars wherever a pillar
//    stands unbraced (between two ties: the bed, a strut junction, a brace end) for longer than a
//    limit, never across the part and never longer than the max span;
//  * rounded-rectangle columns: a prime-tower-like filleted rectangle instead of a round pillar, for
//    every pillar or (Auto) for the tall ones that stand alone or are still long-unbraced;
//  * walls and sparse infill inside the stabilizer bodies (the support step only; the bake hands
//    the same settings to the baked object).
// A Plan carries the struts with their pillars and braces; the live slicer and the bake both build
// from it.

#include "../ExPolygon.hpp"
#include "../PrintConfig.hpp"
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

    // --- v2 ---
    // Pillar radius at the bed, mm. Pillars taper linearly from it to pillar_radius at their own top;
    // not more than pillar_radius = straight pillars (v1).
    double     pillar_base_radius = 0.;
    // Brace neighbouring pillars (45 degree diagonals) where one stands unbraced for longer than
    // max_unbraced, mm, never to a pillar farther than max_brace_span (axis to axis), mm.
    bool       bracing            = false;
    double     max_unbraced       = 10.;
    double     max_brace_span     = 25.;
    // Pillar cross-section. Columns are filleted rectangles column_width along their struts by
    // column_length across, mm (never less than the pillar diameter); Auto makes columns of pillars at
    // least column_min_height tall that stand alone or are still unbraced for longer than max_unbraced.
    StabilizerColumnShape column_shape      = scsRound;
    double                column_width      = 6.;
    double                column_length     = 10.;
    double                column_min_height = 30.;
    // Walls around the stabilizer bodies with sparse infill inside; 0 = solid (v1). Density 0..1.
    int           wall_loops     = 0;
    double        infill_density = 0.15;
    InfillPattern infill_pattern = ipRectilinear;

    bool tapered() const { return pillar_base_radius > pillar_radius + EPSILON; }

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

// A pillar: a column standing on the bed under the junctions of the struts that come down onto it.
struct Pillar
{
    Vec2d  pos    = Vec2d::Zero();  // axis, mm
    Vec2d  dir    = Vec2d(1., 0.);  // the way its (first) strut runs out from the part, unit
    double top_z  = 0.;             // its highest strut junction, mm
    // A rounded-rectangle column rather than round (column_shape Rounded rectangle, or Auto).
    bool   column = false;
};

// A brace: a 45 degree diagonal rod of `radius` from the axis of pillar `lower` at height z_low up to
// the axis of pillar `upper`, which it reaches at z_low + span. Each horizontal section is the rod's
// ellipse (sqrt(2) radius along it, radius across), cut at the two pillars' axes.
struct Brace
{
    size_t lower  = 0;
    size_t upper  = 0;
    Vec2d  from   = Vec2d::Zero();  // lower pillar's axis
    Vec2d  to     = Vec2d::Zero();  // upper pillar's axis
    double z_low  = 0.;
    double radius = 0.;

    double span() const { return (to - from).norm(); }
    double z_high() const { return z_low + span(); }
    Vec2d  dir() const { return (to - from).normalized(); }
    Vec2d  axis_at(double z) const { return from + dir() * (z - z_low); }
    // The rod's whole height range, its slanted ends included.
    double z_bottom() const { return z_low - M_SQRT2 * radius; }
    double z_top() const { return z_high() + M_SQRT2 * radius; }
};

// What the live generator prints and the bake builds: the struts, the pillars they stand on, and the
// braces between the pillars.
struct Plan
{
    std::vector<Strut>  struts;
    std::vector<Pillar> pillars;
    std::vector<Brace>  braces;
    // The pillar each strut comes down onto (an index into pillars, one per strut).
    std::vector<size_t> strut_pillar;
};

// Struts whose pillars stand closer than this (axis to axis, mm) share one pillar.
static constexpr double PILLAR_MERGE_DISTANCE = 0.05;

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

    // v2.
    size_t braces            = 0;  // braces placed
    // Pillars still standing unbraced for longer than max_unbraced somewhere (bracing on): no
    // neighbour within the span, or every brace would cross the part.
    size_t unbraced_pillars  = 0;
    size_t columns           = 0;  // pillars built as rounded-rectangle columns
    // Auto columns that would come too close to the part, left round.
    size_t column_fallbacks  = 0;
};

// --- v2 pillar geometry (shared by the live slicer and the bake) ---

// The extra width of the small 45 degree foot every pillar stands on, at height z (0 above it).
double pillar_foot_at(const StabilizerSettings &settings, double z);
// How far a pillar's section at height z is grown beyond its top section: the taper (zero when not
// tapered) plus the foot, mm.
double pillar_growth_at(const Pillar &pillar, const StabilizerSettings &settings, double z);
// A round pillar's radius at height z, taper included, foot not.
double pillar_radius_at(const Pillar &pillar, const StabilizerSettings &settings, double z);

// A rounded-rectangle column's frame: centre, along (unit, the struts' direction), the half sizes of
// its core rectangle and its corner radius at the top. Its section at height z is the core grown by
// fillet + pillar_growth_at(z) on every side. The side facing the part stays where a round pillar's
// would be, so a wider column grows away from the part.
struct ColumnFrame
{
    Vec2d  centre;
    Vec2d  along;
    double core_u;  // half size of the core along `along`
    double core_v;  // and across
    double fillet;
};
ColumnFrame column_frame(const Pillar &pillar, const StabilizerSettings &settings);
// Corner segments of a column's section (each fillet is drawn with this many straight segments).
static constexpr int COLUMN_CORNER_SEGMENTS = 6;
// The section's vertices (counter-clockwise, 4 * (COLUMN_CORNER_SEGMENTS + 1) of them), grown by `grow`.
std::vector<Vec2d> column_outline(const ColumnFrame &frame, double grow);

// A pillar's printed section at height z: a circle or a column, taper and foot included.
Polygon pillar_section(const Pillar &pillar, const StabilizerSettings &settings, double z);

// The heights at which a pillar is tied - the bed, its strut junctions and the brace ends on it -
// sorted, and the longest stretch between two of them.
std::vector<double> pillar_ties(const Plan &plan, size_t pillar);
double              longest_unbraced(const Plan &plan, size_t pillar);

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

// The whole plan: the struts as plan_struts places them, then their pillars, the braces and the
// columns (complete_plan).
Plan plan_stabilizers(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings,
                      const std::vector<PaintedSpot> &painted = {}, PlanReport *report = nullptr);
Plan plan_stabilizers(const PrintObject &object, PlanReport *report = nullptr);
// The pillars, braces and columns for the struts, by the settings. Deterministic: the live slicer and
// the bake complete the same struts into the same plan. `report` gets the v2 counts only.
Plan complete_plan(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings, const std::vector<Strut> &struts,
                   PlanReport *report = nullptr);
// The pillars of the struts alone, without the part: no braces, columns only when every pillar is
// one (Rounded rectangle). What the mesh builder uses when it is given bare struts.
Plan plan_from_struts(const StabilizerSettings &settings, const std::vector<Strut> &struts);

// The plan's cross-sections at the layers: pillars and braces clipped by the part's outline, struts by
// the outline grown by the tip gap. One entry per layer, the areas that print as stabilizer.
std::vector<ExPolygons> slice_plan(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings,
                                   const Plan &plan, const std::function<void()> &throw_if_canceled);

// The struts' cross-sections at the layers, completed into a plan first (complete_plan), clipped by
// the part's outline (grown by the tip gap): one entry per layer, the areas that print as stabilizer.
std::vector<ExPolygons> slice_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &settings,
                                     const std::vector<Strut> &struts, const std::function<void()> &throw_if_canceled);
std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled);

// Walls and infill (stabilizer_wall_loops > 0): per layer, the part of the stabilizer slices that prints
// as sparse infill - inside the walls, wide enough for the infill's line spacing (thin pillars, struts
// and tips stay solid), and sparse on the layers below and above too, so every body keeps solid bottom
// and top layers. Empty everywhere with no walls set. `line_width` and `spacing` are the support flow's, mm.
static constexpr size_t STABILIZER_BOTTOM_SOLID_LAYERS = 2;
static constexpr size_t STABILIZER_TOP_SOLID_LAYERS    = 3;
std::vector<ExPolygons> sparse_infill_areas(const std::vector<ExPolygons> &slices, const StabilizerSettings &settings,
                                            double line_width, double spacing);

} // namespace stabilizers

// Generates the stabilizers for `object` and adds them to its support layers, inserting support
// layers at object layer heights where none exist. No-op unless the object's stabilizer_supports
// option is on. Must run after the regular support generator, inside the support step. Returns what
// the planner did with the painted points, so the caller can warn about the unreachable ones.
stabilizers::PlanReport generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled);

} // namespace Slic3r
