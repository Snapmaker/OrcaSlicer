#ifndef slic3r_SliceBake_hpp_
#define slic3r_SliceBake_hpp_

// Slice baking, phase 1 ("Exact layers" mode).
//
// Turns a sliced PrintObject's PRINTED APPEARANCE - the outer wall as it will actually be
// extruded, fuzzy skin and all - into a watertight mesh that can be re-sliced. The driving
// scenario is texture transplanting: slice at 0.3 mm WITH fuzzy skin, bake, then re-slice the
// bake at 0.12 mm WITHOUT fuzzy skin and get the same texture printed at a finer layer height.
//
// What is baked: LayerRegion::perimeters, outer loops only (erExternalPerimeter, plus the
// overhang/over-support variants, which are the same physical outer wall split by role), from
// every region of every layer, each loop's polyline offset outward by its own width/2 and
// unioned into one filled ExPolygons per layer. The result is SOLID - the bake is a re-sliceable
// object, not a hollow replica - so top and bottom surfaces come for free from the filled layers
// and the loft's own caps.
//
// What is excluded, by construction rather than by filtering: supports and support interface
// (they live in SupportLayer, never touched), brim and skirt (separate collections on Print /
// PrintObject), the wipe tower (a separate structure on Print), and infill and inner perimeters
// (never read - everything radially inward of the outer loop is filled in anyway).
//
// The loft: each layer becomes its own CLOSED PRISM over its [bottom_z, print_z] band - bottom
// cap, vertical walls, top cap, all three generated from the same contour points - and the prisms
// are stacked and welded on exact vertex equality. This is what makes the bake watertight on
// non-prismatic geometry. The existing Slic3r::slices_to_mesh is deliberately NOT used: it
// triangulates the caps from Clipper diffs of consecutive layers, which puts intersection
// vertices on wall edges that have no vertex there (T-junctions), and its own FIXME says the
// result has cracks. See SliceBake.cpp for the construction and the spec for the measurements.
//
// Deterministic: the layer loop is ordered, the per-layer union is Clipper's (itself
// deterministic for a fixed input ordering), and nothing is hashed or threaded across layers in
// a way that could reorder the output.
//
// Spec: docs/superpowers/specs/2026-09-12-slice-bake-research.md (phase 1).

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace Slic3r {

class PrintObject;
class Layer;

// A "close small gaps" radius of 0 - the default - means no closing at all, not "close by
// nothing": the morphological close is skipped entirely so the layer polygons come out of the
// union bit-for-bit as Clipper produced them.
static constexpr double SLICE_BAKE_CLOSE_GAPS_MAX = 2.0;  // mm

// The bake's chord tolerance, in mm: the most a straight segment of the baked boundary is allowed
// to depart from the curve it stands in for. The default is the print's own `resolution` (the
// G-code simplification tolerance), so "bake at the resolution you sliced at" is the no-surprise
// setting; a finer value densifies wherever the curvature asks for it.
static constexpr double SLICE_BAKE_RESOLUTION_MIN     = 0.01;  // mm
static constexpr double SLICE_BAKE_RESOLUTION_MAX     = 1.0;   // mm
static constexpr double SLICE_BAKE_RESOLUTION_DEFAULT = 0.05;  // mm, when the print has no resolution

// Which frame the returned mesh is expressed in.
//
// The three exist because the bake's natural frame is none of the ones a caller wants. The layers
// live in PRINT space: the instance's rotation, scale and Z shift already applied to the vertices,
// and the whole thing translated by -center_offset so Clipper works near the origin. Neither
// "where the ModelVolume's mesh sits" nor "where the part sits on the plate" is that frame, and
// handing a PRINT-frame mesh to a fresh ModelObject - which is what phase 1 did - gives an object
// whose vertices already carry the instance scale AND whose new instance re-applies nothing, so it
// comes out the wrong size and in the wrong place. Hence the enum rather than a bool.
enum class SliceBakeFrame {
    // Exactly as sliced: instance transform applied, XY centred on the object's bbox centre.
    Print = 0,
    // The frame the ModelVolume's own mesh lives in: PRINT put back through trafo_centered().
    // A mesh in this frame replaces a volume whose transform is identity under the object's
    // EXISTING instance transform, which re-applies the rotation and the scale.
    Object,
    // World millimetres (the model space all plates share, NOT plate-local), with the vertices carrying the instance's
    // rotation and scale, and the mesh translated so its instance offset is subtracted out - i.e.
    // the part sits around the origin, and a new instance whose offset is
    // SliceBakeReport::instance_offset (and whose rotation and scale are IDENTITY, because both
    // are already in the vertices) puts it back exactly where the sliced object stood. This is
    // what a new ModelObject needs.
    World,
};

// Where the baked boundary of a layer comes from.
enum class SliceBakeContourSource {
    // The layer's own slice contours (LayerRegion::slices), offset INWARD by half the external
    // perimeter width. These are the un-simplified contours the slicer cut from the mesh, so this
    // is the smooth, exact source: nothing has been through the `resolution` simplifier and no
    // loop has been re-fitted to arcs. Falls back per layer to Extrusion when a layer has no
    // usable slice contour.
    SliceContours = 0,
    // The extrusion centreline of the outer wall loops, offset OUTWARD by half their own width.
    // This is what the nozzle actually follows - fuzzy skin and every other path-level effect is
    // in it - but it has been through the `resolution` simplifier, so it is the more angular of
    // the two.
    Extrusion,
};

struct SliceBakeOptions
{
    // Layer subset, as half-open [first, last) indices into PrintObject::layers(). The default
    // (0, max) means every layer.
    size_t layer_begin = 0;
    size_t layer_end   = std::numeric_limits<size_t>::max();

    // Optional per-layer morphological closing radius in mm (offset out then back in). Bridges
    // the hairline gaps a fuzzed wall can leave between the outward offsets of two neighbouring
    // loops, at the cost of rounding off features finer than the radius. 0 = off.
    double close_gaps_radius = 0.;

    // Chord tolerance in mm for the baked boundary (see SLICE_BAKE_RESOLUTION_*). A segment is
    // split when the arc it stands in for departs from it by more than this; a STRAIGHT run is
    // left alone, however long, because subdividing it adds vertices that carry no shape.
    double resolution = SLICE_BAKE_RESOLUTION_DEFAULT;

    // Where a layer's boundary comes from. SliceContours is the default because the extrusion
    // centreline is the simplified one, and its angularity is what the owner's report is about.
    SliceBakeContourSource contour_source = SliceBakeContourSource::Extrusion; // the GUI only ever uses this one (2026-09-13)

    // Loft consecutive layers into a skirt of sloped quads instead of stacking vertical prisms,
    // wherever two neighbouring layers' contours correspond one to one. Off by default: the
    // stepped stack is the honest picture of what gets printed, and the loft is a smoothing choice
    // the user makes. A layer pair whose contours do not correspond falls back to the two flat
    // caps, so the mesh stays closed either way.
    bool smooth_vertical_steps = false;

    // Which frame the returned mesh is expressed in.
    SliceBakeFrame frame = SliceBakeFrame::Object;
};

struct SliceBakeReport
{
    size_t layers_baked    = 0;
    size_t loops_used      = 0;   // outer-wall loops / slice contours that contributed
    size_t triangles       = 0;
    size_t vertices        = 0;
    size_t empty_layers    = 0;   // layers with no outer wall at all (skipped)
    size_t layers_from_slices    = 0;  // layers whose boundary came from the slice contours
    size_t layers_from_extrusion = 0;  // ... and from the extrusion centreline
    size_t points_added    = 0;   // vertices the resolution densifier inserted
    size_t lofted_bands    = 0;   // layer pairs joined by a sloped skirt rather than two flat caps
    // Points whose XY collided with one already used on the same layer and were moved by ~60 nm to
    // break the tie. A pinch is what made the cap triangulation stitch a blade across two
    // unrelated parts of the boundary; see unpinch_slice in SliceBake.cpp.
    size_t pinch_points_nudged   = 0;
    // Points whose XY collided and for which no free lattice point could be found at all. Must be
    // zero: a survivor sends the cap triangulation down a path whose indices do not match the wall
    // vertices, which is how the extrusion-source bake used to leak.
    size_t pinch_points_unresolved = 0;
    // Cap triangles the tesselation produced and the bake refused: needles (essentially zero area
    // over a long edge - the visible blades) and the rare vertex that matched no contour point.
    size_t cap_triangles_dropped = 0;
    // Islands whose cap the constrained Delaunay triangulation could not produce - CGAL's inexact
    // construction kernel inserts a crossing vertex where a hole runs within a lattice unit of its
    // own contour - and which fell back to the GLU tesselator instead. See append_cap.
    size_t cap_glu_fallbacks     = 0;
    // Islands reduced to their bare contour because their holes could not be triangulated, and
    // islands dropped because even the bare contour could not. Both are sub-square-millimetre fuzz
    // artefacts; the alternative to losing them is an open mesh. See buildable_slice.
    size_t cap_holes_dropped     = 0;
    size_t islands_dropped       = 0;
    double z_min           = 0.;  // in the frame of the returned mesh
    double z_max           = 0.;
    bool   watertight      = false;

    // Where the baked mesh has to be placed for it to stand where the sliced object stood.
    //
    // For SliceBakeFrame::World this is the instance offset a NEW ModelObject must be given:
    // world millimetres (plate origin included, so an object on plate 2 stays on plate 2). Its rotation and scale must stay IDENTITY -
    // both are already baked into the vertices, which is the whole point of the World frame.
    //
    // For the other two frames it is zero: an Object-frame mesh is placed by the object's existing
    // instance, and a Print-frame mesh is not placed at all.
    Vec3d  instance_offset = Vec3d::Zero();

    std::string note;
};

// Thrown when the caller's progress callback asks to stop. Mirrors ColorSplitCancelled.
class SliceBakeCancelled : public std::exception
{
public:
    const char *what() const noexcept override { return "Slice bake cancelled"; }
};

// percent in 0..100; return false to cancel (which raises SliceBakeCancelled out of the bake).
using SliceBakeProgress = std::function<bool(int)>;

// The print's own `resolution` clamped into the bake's range, or SLICE_BAKE_RESOLUTION_DEFAULT
// when the object has no print attached or the print leaves it at 0. This is what the dialog
// seeds its Resolution box with.
double slice_bake_default_resolution(const PrintObject &object);

// The per-layer outer-wall footprints, in PRINT coordinates (the object's own sliced frame,
// unscaled millimetres are recovered by unscaled()). One entry per baked layer, in Z order;
// `out_z` receives each entry's print_z and `out_bottom_z` its bottom_z, so the caller can loft
// with the object's real (possibly variable) layer heights rather than assuming a constant one.
//
// Layers whose outer wall is empty are dropped from all three vectors together.
std::vector<ExPolygons> slice_bake_layer_regions(const PrintObject      &object,
                                                 const SliceBakeOptions &opts,
                                                 std::vector<double>    *out_z,
                                                 std::vector<double>    *out_bottom_z,
                                                 SliceBakeReport        *report   = nullptr,
                                                 const SliceBakeProgress &progress = {});

// The whole phase-1 bake: outer-wall footprints -> loft -> mesh.
//
// The returned mesh is positioned per opts.frame, and `report->instance_offset` says where it has
// to be placed (zero for every frame but World). An empty result (no indices) means the object had
// no bakeable outer wall; `report->note` says why.
indexed_triangle_set slice_bake_to_mesh(const PrintObject       &object,
                                        const SliceBakeOptions  &opts,
                                        SliceBakeReport         *report   = nullptr,
                                        const SliceBakeProgress &progress = {});

// Does this object have anything to bake? Cheap - stops at the first outer-wall loop it finds.
// The GUI uses it to enable/disable the menu item, alongside the plate's sliced state.
bool slice_bake_available(const PrintObject &object);

// A rough triangle count for the dialog's "this will be big" line, without running the bake.
// Each layer becomes a closed prism: two wall triangles per boundary point, plus a bottom and a
// top cap of about n-2 triangles each over the same n points - roughly FOUR per point in total.
// So this counts the boundary points of every layer in the subset and multiplies by four.
//
// The count depends on the options, which is why the dialog can show it LIVE: the contour source
// decides which point set is counted (a slice contour has its own point count, quite different
// from the simplified extrusion), the resolution decides how many points the densifier adds on
// top, and smooth_vertical_steps replaces two flat caps per band with one sloped skirt, which is
// roughly half the triangles. Cheap enough to call on every keystroke: it walks the layers and
// counts points, and never builds a polygon or a triangle.
// Labelled as an estimate wherever shown, since the cap term depends on the tesselation.
size_t slice_bake_estimate_triangles(const PrintObject &object, const SliceBakeOptions &opts);

} // namespace Slic3r

#endif // slic3r_SliceBake_hpp_
