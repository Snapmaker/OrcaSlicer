#pragma once

// "Bake stabilizers": turns an object's live side stabilizers (Support/Stabilizers.hpp, printed as
// support by EdgeSlicer only) into real geometry that any slicer prints - Bambu Studio, OrcaSlicer,
// PrusaSlicer - because a project file loses the EdgeSlicer-only settings in all of them.
//
// Study: tests/research_stabilizer_bake.md (phase 1).
//
// The bake plans the struts exactly as the live generator does (plan_struts, painted stabilizer
// points included), builds them as one closed mesh (StabilizerMesh.hpp) and puts it in the model in
// one of two places:
//
//  * a separate object "<name> stabilizers" (the default): the tips stay separate bodies, so they
//    touch the wall and snap off, and the object carries its own settings (no supports, no brim,
//    solid or the stabilizer walls and infill, the support filament). It does not move with the part - arrange and drag treat it as an
//    object of its own.
//  * an extra part of the same object: it moves with the part, but parts of one object are unioned
//    layer by layer, so a tip that touched the wall would fuse into it; the tip gap is forced up to
//    STABILIZER_BAKE_PART_MIN_GAP. Only region settings can differ per part (walls, infill, filament).
//
// Either way the source object's live stabilizer_supports is switched off, so the stabilizers are
// not printed twice. The geometry is planned from the sliced object, so the plate must be sliced
// first; nothing links the bake to its source afterwards (no re-bake, no stale check: phase 2).

#include "StabilizerMesh.hpp"
#include "Stabilizers.hpp"

#include "../TriangleMesh.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Model;
class ModelObject;
class PrintObject;
class PrintObjectConfig;

enum class StabilizerBakePlacement {
    SeparateObject = 0,
    PartOfObject,
};

// Bambu Studio's default thin-wall handling can drop a tip thinner than this; the dialog says so.
static constexpr double STABILIZER_BAKE_MIN_TIP_DIAMETER = 1.0;  // mm
// A part of the same object fuses with the part it touches; its tips stop at least this far short.
static constexpr double STABILIZER_BAKE_PART_MIN_GAP     = 0.15; // mm

// The bake always uses the stabilizer settings the object was sliced with (tip diameter, tip gap,
// pillar, rings, painted points): what is baked is what the preview showed. Only the placement is a
// choice, and the source's live stabilizers are always switched off, so the two never print together.
struct StabilizerBakeOptions
{
    StabilizerBakePlacement placement = StabilizerBakePlacement::SeparateObject;
    int                     segments  = 24;
};

// The tip gap a placement bakes with, from the gap the object was sliced with: a part of the same
// object never touches the wall, so it gets at least STABILIZER_BAKE_PART_MIN_GAP.
double stabilizer_bake_gap(StabilizerBakePlacement placement, double sliced_gap);

struct StabilizerBakeResult
{
    // SeparateObject: around the origin, each instance offset puts it where its source instance's
    // stabilizers stand (world frame with the instance offset taken out; rotation and scale are in
    // the vertices). PartOfObject: in the source object's own coordinates, for a new part.
    indexed_triangle_set     mesh;
    std::vector<Vec3d>       instance_offsets;
    // What was baked: the plan (struts, pillars, braces) and its struts.
    stabilizers::Plan        plan;
    std::vector<stabilizers::Strut> struts;
    stabilizers::MeshReport  mesh_report;
    stabilizers::PlanReport  plan_report;
    // The settings baked with: the slice's tip diameter, and its tip gap or, for a part, at least
    // STABILIZER_BAKE_PART_MIN_GAP.
    double                   tip_diameter     = 0.;
    double                   tip_gap          = 0.;
    double                   sliced_tip_gap   = 0.;
    double                   layer_height     = 0.;
    // The source's support filament, 1-based; 0 = the object's own.
    int                      support_filament = 0;
    // The stabilizer walls and infill (stabilizer_wall_loops, _infill_density 0..1, _infill_pattern) the
    // baked body is given as its own wall_loops / sparse_infill_*; 0 walls = solid.
    int                      wall_loops       = 0;
    double                   infill_density   = 1.;
    InfillPattern            infill_pattern   = ipRectilinear;
    // Why there is no mesh, when there is none.
    std::string              error;
};

// Plans and builds the stabilizers of a sliced PrintObject (its layers built; posSlice is enough).
// Safe off the UI thread while nothing reslices the object.
StabilizerBakeResult bake_stabilizers(const PrintObject &object, const StabilizerBakeOptions &options);

// Puts a bake into `model`: a new object after `source`'s (SeparateObject) or a new part of `source`
// (PartOfObject), and switches `source`'s live stabilizers off. Returns
// the new object, or `source` for a part; null when the result has no mesh.
ModelObject *apply_stabilizer_bake(Model &model, ModelObject &source, const StabilizerBakeResult &result,
                                   const StabilizerBakeOptions &options);

// The name the separate object gets.
std::string stabilizer_bake_object_name(const ModelObject &source);

// The objects whose live side stabilizers - EdgeSlicer-only settings, which a Bambu Studio export
// leaves out - are on: stabilizer_supports and enable_support, each from the object's own config or
// else `print_config`. Their names, in model order.
std::vector<std::string> objects_with_live_stabilizers(const Model &model, const DynamicPrintConfig &print_config);

} // namespace Slic3r
