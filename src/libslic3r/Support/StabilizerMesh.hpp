#pragma once

// Side stabilizers as real geometry: the struts and pillars Support/Stabilizers.hpp plans, built
// as a closed triangle mesh that any slicer can print (the stabilizer bake, StabilizerBake.hpp).
//
// The geometry is exact rather than stacked from layer slices: a pillar is a cylinder (a frustum when
// tapered, or a filleted-rectangle column) standing on a 45 degree foot, a brace a 45 degree prism cut
// at its two pillars' axes, a strut is an oblique tapered tube along its 45 degree axis whose every horizontal
// section is the ellipse the live generator prints at that height (slice_struts), cut flat by the
// wall plane through its tip (set back by the tip gap), just above its tip layer, and by the vertical
// plane through its pillar's axis. A few hundred triangles per strut, and the 45
// degrees stay 45 degrees at any layer height the mesh is sliced at later.
//
// Every primitive is a closed shell on its own; the shells are unioned with Manifold
// (MeshBoolean::mfd). When the union fails the overlapping closed shells are returned as they are,
// which every slicer unions per layer anyway.
//
// Frame: the planner's - XY in the object's sliced (print) coordinates, Z the height above the
// object's bottom, mm.

#include "Stabilizers.hpp"

#include "../TriangleMesh.hpp"

#include <vector>

namespace Slic3r { namespace stabilizers {

// A closed frustum between two points: a circle of radius `ra` around `a` and one of `rb` around
// `b`, each perpendicular to the axis a-b, joined by `segments` flat sides and capped at both ends.
// Unlike its_make_frustum (vertical, top radius half the bottom one) any axis and any two radii;
// ra == rb is a cylinder. Empty when a == b.
indexed_triangle_set frustum_between(const Vec3d &a, double ra, const Vec3d &b, double rb, int segments = 24);

struct MeshOptions
{
    // Sides of every pillar and strut. 24 is what the live generator's cross-sections use.
    int    segments     = 24;
    // How far above its tip layer's slicing plane a strut ends, and above its last strut's junction a
    // pillar ends, mm. Keeps the mesh's top faces off the slicing planes they were planned on.
    double cap          = 0.01;
    // Union the shells into one with Manifold. Off: the overlapping shells as they are.
    bool   union_shells = true;
};

struct MeshReport
{
    size_t pillars   = 0;
    size_t struts    = 0;
    size_t braces    = 0;
    size_t columns   = 0;   // pillars built as rounded-rectangle columns
    size_t shells    = 0;   // closed primitives built (pillars + braces + struts)
    size_t skipped   = 0;   // struts whose cut left nothing (a tip at the bed)
    size_t triangles = 0;
    bool   unioned   = false; // the Manifold union succeeded
    bool   closed    = false; // no open edges
    double volume    = 0.;   // mm^3
};

// The closed shells of a plan: one per pillar (foot and taper included, a round frustum stack or a
// rounded-rectangle column; struts sharing a pillar share the shell), one per brace (a 45 degree prism
// cut at the two pillars' axes) and one per strut.
std::vector<indexed_triangle_set> stabilizer_shells(const Plan &plan, const StabilizerSettings &settings,
                                                    const MeshOptions &options = {}, MeshReport *report = nullptr);
// The same for bare struts: their pillars without the part (plan_from_struts), so no braces and no
// Auto columns. Use the Plan overload for what the live generator prints.
std::vector<indexed_triangle_set> stabilizer_shells(const std::vector<Strut> &struts, const StabilizerSettings &settings,
                                                    const MeshOptions &options = {}, MeshReport *report = nullptr);

// The shells as one mesh: unioned when options.union_shells and the union succeeds, merged as
// separate overlapping shells otherwise.
indexed_triangle_set stabilizer_mesh(const Plan &plan, const StabilizerSettings &settings,
                                     const MeshOptions &options = {}, MeshReport *report = nullptr);
indexed_triangle_set stabilizer_mesh(const std::vector<Strut> &struts, const StabilizerSettings &settings,
                                     const MeshOptions &options = {}, MeshReport *report = nullptr);

}} // namespace Slic3r::stabilizers
