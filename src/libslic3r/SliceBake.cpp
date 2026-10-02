#include "SliceBake.hpp"

#include "ClipperUtils.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "SlicesToTriangleMesh.hpp"
#include "Surface.hpp"
#include "SurfaceCollection.hpp"
#include "Tesselate.hpp"
#include "TriangleMesh.hpp"
#include "Triangulation.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace Slic3r {

namespace {

// The roles that make up the OUTER wall.
//
// erExternalPerimeter is the outer wall proper. The other two are the same physical loop split by
// role where it leaves the layer below: erOverhangPerimeter where it hangs over air, and this
// fork's erOverSupportPerimeter where it lands on support material. Both are outer wall and both
// must be baked - dropping them punches holes in exactly the overhanging bands where the surface
// matters most. (erPerimeter, the inner walls, is deliberately never read: the bake is solid, so
// everything inward of the outer loop is filled by construction.)
bool is_outer_wall(ExtrusionRole role)
{
    return role == erExternalPerimeter || role == erOverhangPerimeter || role == erOverSupportPerimeter;
}

// One outer-wall loop's printed footprint, appended to `out`.
//
// The stored polyline is the extrusion's CENTRELINE; what the nozzle lays down is that centreline
// swept by the path's own `width`. The printed outer boundary is therefore the centreline offset
// outward by width/2.
//
// The loop is offset as a CLOSED POLYGON, not as a set of open polylines, and this is the trick
// that makes the bake solid. Offsetting the open polylines (what
// ExtrusionPath::polygons_covered_by_width does) sweeps a capsule along each path, and the union
// of those capsules around a closed ring is an ANNULUS - a ring with the object's own cross
// section punched out as a hole. Offsetting the closed polygon instead gives the filled region
// bounded by the outer edge of the wall, which is exactly the printed cross section.
//
// Orientation carries the meaning here, and the union below reads it. A CONTOUR loop runs
// counter-clockwise and its printed region is everything inside it out to width/2 BEYOND the
// centreline: offset the CCW polygon by +width/2. A HOLE loop runs clockwise and the material is
// everything OUTSIDE it, so the void it encloses reaches width/2 INSIDE the centreline: offsetting
// the CW polygon by the same +width/2 grows it along its own (reversed) normal, which is exactly
// the shrink the hole needs, and it stays clockwise. Handing both to a pftNonZero union then makes
// the contours solid and the holes hollow, with no separate bookkeeping.
//
// The width is the loop's own, taken as the MAXIMUM over its outer-wall paths. A loop crossing an
// overhang is split into paths of differing width (overhang flow is not wall flow), and a closed
// offset takes one delta; the larger is the safe choice, since it can only push the boundary out
// by at most the difference - well under a tenth of a millimetre - whereas the smaller would pull
// the printed edge inside where the wide half actually lands.
void append_loop_footprint(const ExtrusionLoop &loop, Polygons &out, size_t &loops_used)
{
    float width = 0.f;
    bool  any   = false;
    for (const ExtrusionPath &path : loop.paths) {
        // A loop's own role() is erMixed once it has been split by overhang, so the per-path role
        // is the only reliable one.
        if (is_outer_wall(path.role())) {
            any   = true;
            width = std::max(width, path.width);
        }
    }
    if (! any || width <= 0.f)
        return;

    // The WHOLE ring, not just its outer-wall paths: a loop is one closed curve, and dropping an
    // overhanging section would leave a polygon that does not close. The role test above decides
    // whether the loop is an outer wall, not which parts of it exist.
    Polygon poly = loop.polygon();
    if (poly.points.size() < 3)
        return;

    // The tiny extra ClipperSafetyOffset makes the footprints of neighbouring loops (two islands
    // that touch) overlap rather than meet exactly, so the union closes the joint instead of
    // leaving a zero-width crack there.
    const float delta = float(scale_(width / 2.)) + ClipperSafetyOffset;
    Polygons    grown = offset(poly, delta);
    // offset() normalises what it returns to CCW outer / CW hole, so a hole loop's grown ring
    // comes back as a positive contour. Put its orientation back, so the union reads it as the
    // void it is.
    if (poly.is_clockwise())
        for (Polygon &p : grown)
            p.make_clockwise();
    polygons_append(out, std::move(grown));
    ++loops_used;
}

void collect_outer_walls(const ExtrusionEntity *entity, Polygons &out, size_t &loops_used)
{
    if (entity == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_outer_walls(child, out, loops_used);
        return;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        append_loop_footprint(*loop, out, loops_used);
        return;
    }
    // ExtrusionMultiPath before ExtrusionPath: the two are siblings, not parent and child, but
    // testing the leaf first would still be wrong the day one of them gains a common base.
    //
    // Neither is a closed ring, so neither can be offset as a polygon. An outer wall that reaches
    // the bake as an open path is a rarity (a wall too short to close, chiefly), and the capsule
    // around it is the right footprint for it: an open stroke encloses no interior to fill.
    if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multi->paths)
            if (is_outer_wall(path.role()) && path.polyline.points.size() >= 2 && path.width > 0.f) {
                path.polygons_covered_by_width(out, float(ClipperSafetyOffset));
                ++loops_used;
            }
        return;
    }
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        if (is_outer_wall(path->role()) && path->polyline.points.size() >= 2 && path->width > 0.f) {
            path->polygons_covered_by_width(out, float(ClipperSafetyOffset));
            ++loops_used;
        }
    }
}

bool layer_has_outer_wall(const Layer &layer)
{
    Polygons scratch;
    size_t   n = 0;
    for (const LayerRegion *region : layer.regions()) {
        for (const ExtrusionEntity *entity : region->perimeters.entities) {
            collect_outer_walls(entity, scratch, n);
            if (n > 0)
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Source B: the layer's own slice contours
// ---------------------------------------------------------------------------------------------
//
// Why this source exists at all. The extrusion centreline above is what the nozzle follows, and
// that is exactly the problem: by the time a loop reaches LayerRegion::perimeters it has been
// through the `resolution` simplifier (Douglas-Peucker at the print's own G-code tolerance, 0.01
// mm by default but commonly 0.05-0.1), possibly re-fitted to arcs, and had xy compensation
// applied. A 8 mm cylinder whose slice contour has ~200 points comes out of the perimeter
// generator with ~40, and the bake built on those 40 is visibly faceted next to the preview -
// which draws the arcs, not the chords. That is the owner's "more angular than the slice preview".
//
// LayerRegion::slices is the un-simplified contour the slicer cut from the mesh. The outer wall's
// printed boundary is that contour itself (the wall centreline sits half an external width inside
// it, and the bake offsets the centreline back out by the same half width) - so the boundary is
// recoverable EXACTLY, with none of the simplification in between, by just taking the slice
// contour. The half-width round trip is a no-op and is not performed: offsetting in by w/2 and
// back out by w/2 through Clipper would round every convex corner by w/2 and leave the concave
// ones alone, which is a real distortion introduced for nothing.
//
// What this source CANNOT reproduce is anything the perimeter generator adds to the path after
// the slice: fuzzy skin above all, which is displacement applied to the centreline and has no
// counterpart in the slice contour. A fuzzy bake must use the Extrusion source; the GUI says so
// and the caller chooses.
bool append_slice_contours(const Layer &layer, Polygons &out, size_t &loops_used)
{
    bool any = false;
    for (const LayerRegion *region : layer.regions()) {
        if (region == nullptr)
            continue;
        for (const Surface &surface : region->slices.surfaces) {
            if (surface.expolygon.contour.points.size() < 3)
                continue;
            // The ExPolygon's own orientation is already CCW contour / CW holes, which is the
            // convention the pftNonZero union below reads - the same one append_loop_footprint
            // hand-maintains for the offset extrusion rings.
            //
            // The tiny safety offset is the one thing borrowed from the extrusion path: two
            // regions of the same layer meet along a shared edge, and an exact meeting leaves a
            // zero-width crack the union will not close. Growing every contour by a Clipper epsilon
            // makes them overlap instead. It is ~2 nm scaled, far below any geometry.
            Polygons grown = offset(surface.expolygon.contour, float(ClipperSafetyOffset));
            polygons_append(out, std::move(grown));
            for (const Polygon &hole : surface.expolygon.holes) {
                if (hole.points.size() < 3)
                    continue;
                // A hole is CW, and offset() normalises what it returns to CCW; put the winding
                // back so the union reads it as the void it is. Note the offset DELTA is positive
                // for a hole too: growing a CW ring along its own reversed normal shrinks the void
                // by the epsilon, which is the direction that closes cracks rather than opening
                // them.
                Polygons gh = offset(hole, float(ClipperSafetyOffset));
                for (Polygon &g : gh)
                    g.make_clockwise();
                polygons_append(out, std::move(gh));
            }
            ++loops_used;
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// The resolution densifier
// ---------------------------------------------------------------------------------------------
//
// What it is for: even an un-simplified slice contour is a polyline, and the bake's caps and walls
// are built on its points. Where the real surface curves, a long chord between two contour points
// is a visible flat. The densifier splits such a chord - but ONLY where the curvature says a chord
// is standing in for an arc.
//
// The curvature test, and why straight runs are deliberately left alone. Subdividing a straight
// segment adds vertices that carry no shape at all: the new points lie on the line the segment
// already described, so the mesh is identical and only the triangle count grows. On a 100 mm cube
// wall that is tens of thousands of wasted triangles. So each segment is judged by the TURN at its
// two endpoints - the angle between it and its neighbours - which is the polyline's discrete
// curvature. A segment between two collinear neighbours is on a straight run and is kept whole,
// however long. A segment whose neighbours turn away from it is standing in for an arc, and the
// sagitta of that arc is what has to be held under the tolerance.
//
// The sagitta of a circular arc of radius R subtending a chord of length L is
// R - sqrt(R^2 - (L/2)^2) ~= L^2 / (8R) for small L, and the local radius implied by a turn of
// angle `theta` over a segment of length L is R ~= L / theta. So sagitta ~= L * theta / 8, and the
// number of pieces needed to bring it under `tol` is ceil(sqrt(L * theta / (8 * tol))) - the square
// root because halving a chord quarters its sagitta.
//
// Everything is computed in SCALED integer coordinates and the split points are rounded to them,
// which keeps the output on the same lattice as the input: a densified contour's original points
// are bit-identical to the un-densified ones, so nothing downstream (the exact-match cap table,
// the prism weld) has to care whether the densifier ran.
Polygon densify_polygon(const Polygon &poly, double tol_mm, size_t &points_added)
{
    const size_t n = poly.points.size();
    if (n < 3 || tol_mm <= 0.)
        return poly;

    const double tol = std::max(tol_mm, SLICE_BAKE_RESOLUTION_MIN);

    Polygon out;
    out.points.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const Point &a = poly.points[i];
        const Point &b = poly.points[(i + 1) % n];
        const Point &prev = poly.points[(i + n - 1) % n];
        const Point &next = poly.points[(i + 2) % n];

        out.points.push_back(a);

        const Vec2d va = unscaled(a), vb = unscaled(b);
        const double len = (vb - va).norm();
        if (len <= tol)
            continue;                       // already finer than the tolerance

        // The turn at each end of the segment, in radians. A zero turn at both ends means the
        // segment sits inside a straight run: nothing to approximate, nothing to add.
        auto turn = [](const Vec2d &p, const Vec2d &q, const Vec2d &r) -> double {
            const Vec2d u = q - p, v = r - q;
            const double nu = u.norm(), nv = v.norm();
            if (nu < EPSILON || nv < EPSILON)
                return 0.;
            const double c = std::clamp(u.dot(v) / (nu * nv), -1., 1.);
            return std::acos(c);
        };
        const double theta = std::max(turn(unscaled(prev), va, vb), turn(va, vb, unscaled(next)));
        if (theta < 1e-4)
            continue;                       // straight run

        // sagitta ~= len * theta / 8; pieces = ceil(sqrt(sagitta / tol)).
        const double sagitta = len * theta / 8.;
        if (sagitta <= tol)
            continue;
        const int pieces = int(std::ceil(std::sqrt(sagitta / tol)));
        // A hard ceiling so a pathological turn on a long segment cannot explode the mesh. 64 new
        // points on one segment is already far past anything a real contour asks for.
        const int k = std::min(pieces, 64);
        for (int j = 1; j < k; ++j) {
            const double t = double(j) / double(k);
            out.points.emplace_back(coord_t(std::llround(double(a.x()) + t * double(b.x() - a.x()))),
                                    coord_t(std::llround(double(a.y()) + t * double(b.y() - a.y()))));
            ++points_added;
        }
    }
    // Consecutive duplicates would be degenerate wall quads; rounding to the lattice can produce
    // one when a segment is barely longer than a scaled unit.
    out.points.erase(std::unique(out.points.begin(), out.points.end()), out.points.end());
    while (out.points.size() > 1 && out.points.front() == out.points.back())
        out.points.pop_back();
    return out.points.size() >= 3 ? out : poly;
}

// ---------------------------------------------------------------------------------------------
// The pinch fix
// ---------------------------------------------------------------------------------------------
//
// The defect, in one sentence: a layer whose boundary visits the same XY twice makes the cap
// triangulation stitch two unrelated parts of the wall together, and the triangle that does it is a
// millimetres-long zero-area blade lying across the surface.
//
// How a layer comes to visit an XY twice, and why it is not a bug upstream. Two cases, both legal
// Clipper output:
//
//   * a self-touching contour - one closed path that returns to a vertex it already used. Two sharp
//     CONCAVE features of the boundary angling towards each other produce it as soon as their
//     offsets meet, which on a Benchy hull happens all over the deck cutout and the bow. Clipper
//     emits it as a single path because that is what the non-zero fill rule describes; splitting it
//     into two simple paths is what the StrictlySimple flag is for, and simplify_polygons_ex above
//     now asks for it.
//
//   * two SEPARATE rings sharing a vertex - two islands that touch at a point, or a hole whose
//     boundary touches its contour. StrictlySimple does not merge those (they really are two
//     rings), so the shared vertex survives the union, and it is the one that aliases.
//
// The fix for what is left after StrictlySimple: walk every point of the layer and, whenever an XY
// has already been seen, move it by 64 SCALED UNITS - about 60 nanometres - along the bisector of
// its own two edges, away from the point it collided with. That is far below any geometry the
// slicer produces (Clipper's own safety offset is ten scaled units) and far above the spacing a
// float can resolve at the magnitudes a print bed spans (~30 nm at 300 mm), so the two points stay
// distinct all the way into the mesh.
//
// Who still needs this, now that the caps are triangulated by index rather than by coordinate:
// Triangulation::triangulate itself. Its own header says it is "Not working properly for ExPolygons
// with multiple point on same coordinate", and the duplicate-handling path it takes otherwise
// (collect_duplicates / create_changes) merges the coincident points into one - which would weld
// the two sides of a pinch in the CAP while the walls keep them apart. Feeding it a slice with no
// repeated coordinate keeps it on its fast, exact path.
//
// Why nudge rather than drop, merge or weld. Dropping a cap triangle leaves a hole and breaks
// watertightness, which is the one property the whole loft exists to guarantee (measured: a plain
// 20 mm cube went from 0 open edges to 40 when zero-area cap triangles were rejected). Merging the
// two rings changes the topology of the layer. Nudging changes the geometry by an amount no
// measurement in this file can see.
void unpinch_slice(ExPolygons &slice, size_t &nudged, size_t &unresolved)
{
    // 64 scaled units: see above for why it is not 1. In millimetres this is 6.4e-8.
    const coord_t NUDGE = 64;

    std::set<std::pair<coord_t, coord_t>> seen;

    // Move `p` to the nearest free lattice point along `dir`, and if that whole ray is taken, spiral
    // outwards until something is free.
    //
    // TOTALITY MATTERS HERE, and the first version of this function did not have it: it tried eight
    // steps along one direction and then GAVE UP, leaving the duplicate in place. That is not a
    // cosmetic shortfall, because everything downstream assumes the invariant holds:
    // Triangulation::triangulate takes a completely different code path when to_points() contains a
    // duplicate (collect_duplicates / create_changes), one that COLLAPSES the coincident points and
    // returns indices into the collapsed set - with uint32_t::max() for the entries its reverse map
    // never reaches. The cap code maps those indices onto vertex runs built over the UNCOLLAPSED
    // set, so a single unresolved duplicate on a layer corrupts every cap triangle that references
    // it: the out-of-range ones are dropped (open edges) and the in-range ones stitch to the wrong
    // vertex (slivers).
    //
    // It went unnoticed because the slice-contour source never triggers it - its contours are far
    // apart and its points are not densified - while the extrusion source does: a fuzzed wall's
    // loops run within a fraction of a millimetre of each other, and the resolution densifier then
    // inserts interpolated points on both, which round to the same lattice XY often enough that
    // eight steps of 64 nm in one direction is not always enough to separate them.
    auto place = [&](Point &p, Vec2d dir) {
        if (seen.emplace(p.x(), p.y()).second)
            return;                          // already free

        if (dir.norm() < EPSILON)
            dir = Vec2d(1., 0.);
        dir.normalize();
        const Vec2d perp(-dir.y(), dir.x());
        const Point origin = p;

        // Ring `r` of the spiral is at radius r*NUDGE; within it, try the bisector first (it is the
        // direction that opens a pinch rather than closing it) and then eight offsets to either
        // side. A few hundred candidates is already far more than any real layer needs, and the
        // loop is bounded so a pathological slice cannot hang the bake - but the bound is large
        // enough that reaching it means something else is very wrong, which is what `unresolved`
        // records.
        for (int r = 1; r <= 64; ++r) {
            for (int s = 0; s <= 8; ++s) {
                for (int sign = 0; sign < (s == 0 ? 1 : 2); ++sign) {
                    const double lateral = double(s) * (sign == 0 ? 1. : -1.);
                    const Vec2d  off = (dir * double(r) + perp * lateral) * double(NUDGE);
                    const Point  cand(origin.x() + coord_t(std::llround(off.x())),
                                      origin.y() + coord_t(std::llround(off.y())));
                    if (seen.emplace(cand.x(), cand.y()).second) {
                        p = cand;
                        ++nudged;
                        return;
                    }
                }
            }
        }
        // Nothing free within 64 rings. Record it rather than pretending: the caller surfaces the
        // count, and the cap code's range check still keeps the read in bounds.
        ++unresolved;
    };

    auto fix_ring = [&](Polygon &poly) {
        const size_t n = poly.points.size();
        for (size_t i = 0; i < n; ++i) {
            // The OUTWARD bisector of this point's two edges: it points out of the material at a
            // concave corner, and a pinch is made of concave corners, so this is the direction that
            // separates the two sides rather than driving them together.
            const Vec2d prev = unscaled(poly.points[(i + n - 1) % n]);
            const Vec2d next = unscaled(poly.points[(i + 1) % n]);
            const Vec2d here = unscaled(poly.points[i]);
            Vec2d       dir  = Vec2d::Zero();
            const Vec2d in   = here - prev, out = here - next;
            if (in.norm() > EPSILON && out.norm() > EPSILON)
                dir = in.normalized() + out.normalized();
            place(poly.points[i], dir);
        }
    };

    for (ExPolygon &ex : slice) {
        fix_ring(ex.contour);
        for (Polygon &hole : ex.holes)
            fix_ring(hole);
    }
}

void densify_expolygons(ExPolygons &slice, double tol_mm, size_t &points_added)
{
    if (tol_mm <= 0.)
        return;
    for (ExPolygon &ex : slice) {
        ex.contour = densify_polygon(ex.contour, tol_mm, points_added);
        for (Polygon &hole : ex.holes)
            hole = densify_polygon(hole, tol_mm, points_added);
    }
}

// ---------------------------------------------------------------------------------------------
// The loft: one closed prism per layer
// ---------------------------------------------------------------------------------------------
//
// Why not Slic3r::slices_to_mesh, which this bake used first: that loft triangulates the caps
// between consecutive layers from CLIPPER DIFFS (diff_ex(lower, upper) and diff_ex(upper, lower))
// while the vertical walls come from the layer polygons themselves. A diff introduces
// intersection vertices on edges the wall strip has no vertex on, so the two meshes meet along
// edges split on one side and whole on the other - T-junctions, which its_merge_vertices cannot
// weld because the vertices genuinely are not coincident. Its own FIXME says as much
// ("there will be cracks in the output"). A prismatic cube never differs layer to layer and so
// came out clean; a fuzzed cube produced 20,264 open edges and a cylinder 15,245.
//
// What is built instead: each layer is its own CLOSED prism - bottom cap, vertical walls, top cap
// - and every one of the three is generated from the SAME contour point set, so the prism is
// watertight on its own, by construction, with no diff and no tolerance anywhere. Stacking the
// prisms and welding exactly-coincident vertices then gives a mesh whose only remaining internal
// structure is a pair of coplanar, oppositely-wound cap faces between neighbouring layers. Those
// pairs are matched (each is somebody's top and somebody's bottom), so every edge still has an
// even number of incident faces and the result is closed: its_num_open_edges() == 0.
//
// The cost is the coplanar internal faces where two layers overlap. They are the price of never
// needing a diff, and nothing downstream minds: a closed mesh is a closed mesh to TriangleMesh's
// statistics, to the repair path, and to the slicer, which cuts the same contours either way.
//
// The cap triangulation is the existing GLU tesselator. It emits its vertices verbatim from the
// coordinates handed in - the same unscale<double>() the wall vertices use - so a cap vertex can
// be matched back to its contour point by exact double equality. The one case that would not
// match is a tessCombine vertex, which GLU only produces for a self-intersecting contour; the
// union_ex output here is not self-intersecting, and if one ever appeared it is counted into
// report.note rather than silently welded to the wrong place.

// The cap triangulation, and why it is INDEX based rather than coordinate based.
//
// The first version of this loft triangulated the caps with the GLU tesselator
// (triangulate_expolygons_3d) and matched the bare XY coordinates it emits back to the mesh
// vertices the walls had already laid down, through a std::map keyed on the unscaled XY. Two things
// were wrong with that, and together they are the owner's "stray straight slivers" report:
//
//   * The map is only injective if the layer visits every XY at most once. A layer whose boundary
//     touches itself - two sharp concave features angling at each other, which a Benchy's deck
//     cutout and bow are full of - visits one twice, and the map's emplace kept the FIRST entry, so
//     the second visit silently aliased onto the first visit's vertex. The cap triangle that used
//     it was then stitched to a point on the far side of the pinch.
//
//   * Measured on a plate with five 0.55 mm slots, GLU emitted FOUR triangles per cap that span a
//     slot - covering a void with surface that should not be there - on input whose contour is CCW,
//     whose five holes are all CW, which has no repeated coordinate at all and no vertex off the
//     contour. That is GLU getting a valid polygon-with-holes wrong, not a defect in the input, and
//     no amount of cleaning the input fixes it.
//
// Slic3r::Triangulation::triangulate(const ExPolygons &) has neither problem. It is a constrained
// Delaunay triangulation whose constraints are the polygon's own edges, so a triangle can never
// cross one - a void cannot be covered. And it returns INDICES into the ExPolygons' own point list
// rather than coordinates, so there is nothing to match back: the point list is
// `contour, then holes, for each ExPolygon in order` (to_points()), which is exactly the order
// append_layer_prism lays its vertex runs down in, so an index maps to a vertex by adding the run's
// base. No table, no keying on a double, nothing to alias.
//
// What is lost: nothing the bake used. GLU's tessCombine vertices (which the old code had to count
// and drop) cannot arise, because the triangulation adds no points.

// The mesh vertex indices for one layer's points, in to_points() order: `lo[i]` and `hi[i]` are the
// bottom and top vertex of the i-th point of the layer.
struct PrismVertices
{
    std::vector<int> lo;
    std::vector<int> hi;
    size_t unmatched  = 0;  // triangulation indices out of range, or a point list of the wrong size
    // Points of this cap's slice whose XY was already used by another point of the same slice.
    // unpinch_slice is supposed to leave none; if any survive, the cap is refused rather than
    // triangulated, because the CDT's duplicate path returns indices into a COLLAPSED point list
    // that the wall vertex runs do not mirror.
    size_t duplicates = 0;
    // Islands the constrained Delaunay triangulation could not do, which fell back to the GLU
    // tesselator. See append_cap for why the fallback exists and why it cannot be screened for.
    size_t fallbacks = 0;
};

// Append the two vertices (lo, hi) for every point of `poly` and record them in `pv`.
// Returns the index of the first LO vertex; the walls are emitted over that contiguous run.
int append_ring_vertices(indexed_triangle_set &mesh, PrismVertices &pv, const Polygon &poly,
                         double z_lo, double z_hi)
{
    const int first_lo = int(mesh.vertices.size());
    const int n        = int(poly.points.size());
    // Both runs are contiguous: lo vertices [first_lo, first_lo+n), hi [first_lo+n, +2n).
    for (const Point &p : poly.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_lo)));
    for (const Point &p : poly.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_hi)));

    // Appended in the same order the caller walks the slice, which is to_points()' order.
    for (int i = 0; i < n; ++i) {
        pv.lo.push_back(first_lo + i);
        pv.hi.push_back(first_lo + n + i);
    }
    return first_lo;
}

// The vertical wall of one ring, over the two contiguous vertex runs append_ring_vertices laid
// down. One winding rule serves both a contour and a hole: the ring's own traversal direction
// already carries the orientation. Walking a CCW contour's bottom edge in +x gives
// (lo_i, lo_j, hi_j) the normal x cross z = -y, which points out of the part; a hole runs CW, so
// the same expression evaluates to the opposite side, which is again out of the material. This
// is the rule Slic3r::wall_strip uses, and it is right for the same reason.
void append_ring_walls(indexed_triangle_set &mesh, int first_lo, int n)
{
    for (int i = 0; i < n; ++i) {
        const int j    = (i + 1) % n;
        const int lo_i = first_lo + i,     lo_j = first_lo + j;
        const int hi_i = first_lo + n + i, hi_j = first_lo + n + j;
        mesh.indices.emplace_back(lo_i, lo_j, hi_j);
        mesh.indices.emplace_back(lo_i, hi_j, hi_i);
    }
}

// Triangulate ONE island into indices over its own to_points() order, or report that it cannot be
// done. Read this before changing anything about the caps: the two triangulators are not
// interchangeable, the order matters, and the return value is what keeps the mesh closed.
//
// PREFERRED: Slic3r::Triangulation - a constrained Delaunay triangulation (CGAL) returning INDICES
// into the island's own point list. Nothing is matched back by coordinate and no triangle can cross
// a constraint edge, so a void cannot be covered. On the slice-contour source it is flawless: 1002
// Benchy layers, zero dropped triangles, zero open edges.
//
// WHY IT NEEDS A FALLBACK, which took several rounds of measurement to pin down. CGAL's
// Exact_predicates_INEXACT_constructions kernel resolves a hole running within a lattice unit of its
// own contour as a genuine intersection and inserts a vertex at the crossing - a vertex whose
// info() is never assigned, so it returns as an out-of-range index. The extrusion source hits this
// and the slice-contour source does not: a fuzzed wall's outward offset pinches slivers of void
// against their own contours all over a Benchy hull, and those near-contacts are what the inexact
// kernel cannot resolve. Clipper, exact on the integer lattice, sees nothing wrong with them -
// measured, every such island returns from its own union_ex unchanged, ring for ring and to the
// last square micrometre.
//
// AND IT CANNOT BE SCREENED FOR IN ADVANCE. CGAL::spatial_sort shuffles with a RANDOM seed, so
// triangulate() is not a function of its input: probing an island, finding it sound, and then
// triangulating it for real can still fail. An earlier "sanitiser" built on such a probe certified
// nothing and the open-edge count wandered between runs on identical geometry.
//
// FALLBACK: the GLU tesselator for that island, mapped back by coordinate - safe because
// unpinch_slice has already guaranteed the island visits no XY twice. GLU is deterministic and
// always covers the polygon; its own flaw (spanning a void when a polygon has several narrow,
// closely-spaced holes) is rarer and strictly less bad than a hole in the mesh.
//
// LAST RESORT: if GLU also produces a vertex that maps to nothing - a tessCombine vertex, which it
// only emits on self-intersecting input - the island is reported UNTRIANGULABLE. The caller then
// drops its holes and asks again, and builds the walls from that same reduced shape. That is the
// part that matters: an earlier version filled the holes in the cap alone while the walls were
// already built from the unreduced island, which left every filled hole ringed by wall with nothing
// to close against - 1,894 open edges out of 16 dropped triangles. A cap and its walls must come
// from the same geometry or the arithmetic cannot work out.
bool triangulate_island(const ExPolygon &ex, Triangulation::Indices &out, size_t &fallbacks)
{
    out.clear();
    const ExPolygons one { ex };
    const Points     pts = to_points(one);
    if (pts.size() < 3)
        return true;                        // nothing to cap, and nothing broken

    // The duplicate-free precondition of the two-argument overload, enforced rather than trusted:
    // the one-argument form silently switches to a path that COLLAPSES duplicates and returns
    // indices into the collapsed list, which the caller's vertex run does not mirror.
    std::map<std::pair<coord_t, coord_t>, size_t> by_xy;
    for (size_t k = 0; k < pts.size(); ++k)
        by_xy.emplace(std::make_pair(pts[k].x(), pts[k].y()), k);
    if (by_xy.size() != pts.size())
        return false;

    auto in_range = [&](const Triangulation::Indices &v) {
        for (const Vec3i32 &t : v)
            for (int k = 0; k < 3; ++k)
                if (t(k) < 0 || size_t(t(k)) >= pts.size())
                    return false;
        return true;
    };

    out = Triangulation::triangulate(one, pts);
    if (in_range(out))
        return true;

    // GLU, for this island only.
    out.clear();
    const std::vector<Vec3d> soup = triangulate_expolygons_3d(one, 0., NORMALS_UP);
    for (size_t s = 0; s + 2 < soup.size(); s += 3) {
        int idx[3];
        for (int k = 0; k < 3; ++k) {
            // The tesselator emits unscale<double> of the same Points, so the scaled coordinate is
            // recoverable exactly.
            const coord_t qx = coord_t(std::llround(scale_(soup[s + size_t(k)].x())));
            const coord_t qy = coord_t(std::llround(scale_(soup[s + size_t(k)].y())));
            auto it = by_xy.find(std::make_pair(qx, qy));
            if (it == by_xy.end()) {
                out.clear();
                return false;               // a tessCombine vertex: nothing to map it to
            }
            idx[k] = int(it->second);
        }
        out.emplace_back(idx[0], idx[1], idx[2]);
    }
    ++fallbacks;
    return true;
}

// The shape a layer can actually be built from: every island either triangulable as it stands, or
// reduced to its bare contour, or - if even that fails - dropped. Returns the reduced slice and the
// triangulation of each island, so the caller builds walls and caps from ONE decision.
ExPolygons buildable_slice(const ExPolygons &slice, std::vector<Triangulation::Indices> &out_tris,
                           size_t &fallbacks, size_t &holes_dropped, size_t &islands_dropped)
{
    ExPolygons ok;
    out_tris.clear();
    for (const ExPolygon &raw : slice) {
        // Strip degenerate rings FIRST, so that to_points() (which the triangulation indexes), the
        // vertex runs (which skip rings under three points, there being no strip to emit for one)
        // and the cap's base arithmetic all walk the same rings. They did not before: the vertex
        // loops skipped a two-point ring and the cap's base advanced past it anyway, so every
        // island after one was capped onto the wrong vertices - every index in range, nothing
        // dropped, and a hole in the mesh regardless. Such rings come out of a fuzzed wall's
        // outward offset and essentially never out of a slice contour, which is exactly why only
        // the extrusion source ever leaked.
        if (raw.contour.points.size() < 3)
            continue;
        ExPolygon ex(raw.contour);
        for (const Polygon &h : raw.holes)
            if (h.points.size() >= 3)
                ex.holes.push_back(h);

        Triangulation::Indices tris;
        if (triangulate_island(ex, tris, fallbacks)) {
            ok.emplace_back(ex);
            out_tris.emplace_back(std::move(tris));
            continue;
        }
        if (! ex.holes.empty()) {
            // The holes are the usual offender - a sliver of void pinched against its own contour
            // by a fuzzed wall's outward offset. Filling them in costs a few hundredths of a square
            // millimetre and keeps the layer closed.
            ExPolygon solid(ex.contour);
            if (triangulate_island(solid, tris, fallbacks)) {
                holes_dropped += ex.holes.size();
                ok.emplace_back(std::move(solid));
                out_tris.emplace_back(std::move(tris));
                continue;
            }
        }
        // Even the bare contour cannot be triangulated: its own boundary crosses itself, so neither
        // its cap nor its walls could close. Losing it is strictly better than leaving a hole, and
        // what is lost is by construction a sub-square-millimetre artefact.
        ++islands_dropped;
    }
    return ok;
}

// One cap over `slice`, as indexed faces over the vertices already appended for it. `top` picks the
// hi vertex run and the up-facing winding; otherwise the lo run and the down-facing one.
void append_cap(indexed_triangle_set &mesh, PrismVertices &pv, const ExPolygons &slice,
                const std::vector<Triangulation::Indices> &per_island, bool top)
{
    const std::vector<int> &run = top ? pv.hi : pv.lo;
    if (run.empty() || per_island.size() != slice.size())
        return;

    // to_points() lays a slice out as `contour, holes...` per ExPolygon in order, which is exactly
    // the order append_ring_vertices filled `run` in - so an island's indices land on the right
    // stretch of the run by adding a base that is the running sum of the islands' point counts.
    // count_points is the right stride because buildable_slice has already removed every ring the
    // vertex loops would skip, so the two walk exactly the same rings.
    size_t base = 0;
    for (size_t e = 0; e < slice.size(); ++e) {
        const size_t n = count_points(slice[e]);
        for (const Vec3i32 &t : per_island[e]) {
            if (t(0) < 0 || t(1) < 0 || t(2) < 0 ||
                size_t(t(0)) >= n || size_t(t(1)) >= n || size_t(t(2)) >= n) {
                ++pv.unmatched;
                continue;
            }
            const size_t ia = base + size_t(t(0)), ib = base + size_t(t(1)), ic = base + size_t(t(2));
            if (ia >= run.size() || ib >= run.size() || ic >= run.size()) {
                ++pv.unmatched;
                continue;
            }
            int a = run[ia], b = run[ib], c = run[ic];
            if (a == b || b == c || a == c)
                continue;                   // degenerate
            // Triangulation emits CCW-in-XY triangles. A cap seen from OUTSIDE the solid is CCW at
            // the top and CW at the bottom, so the bottom cap is the same triangle reversed.
            if (! top)
                std::swap(b, c);
            mesh.indices.emplace_back(a, b, c);
        }
        base += n;
    }
}

// One layer -> one closed prism, appended to `mesh`.
void append_layer_prism(indexed_triangle_set &mesh, const ExPolygons &slice,
                        double z_lo, double z_hi, size_t &unmatched, size_t &fallbacks,
                        size_t &holes_dropped, size_t &islands_dropped)
{
    if (slice.empty() || z_hi <= z_lo)
        return;

    // CAP FIRST. buildable_slice decides, per island, what can actually be triangulated - and may
    // reduce an island to its bare contour or drop it. The walls are then built from THAT shape, so
    // the two always agree. Building the walls first and reducing the cap afterwards is the bug
    // this ordering exists to prevent: it leaves a ring of wall with no cap to close against.
    std::vector<Triangulation::Indices> tris;
    const ExPolygons shape = buildable_slice(slice, tris, fallbacks, holes_dropped, islands_dropped);
    if (shape.empty())
        return;

    PrismVertices pv;
    for (const ExPolygon &ex : shape) {
        if (ex.contour.points.size() >= 3) {
            const int first = append_ring_vertices(mesh, pv, ex.contour, z_lo, z_hi);
            append_ring_walls(mesh, first, int(ex.contour.points.size()));
        }
        for (const Polygon &hole : ex.holes) {
            if (hole.points.size() < 3)
                continue;
            const int first = append_ring_vertices(mesh, pv, hole, z_lo, z_hi);
            append_ring_walls(mesh, first, int(hole.points.size()));
        }
    }
    append_cap(mesh, pv, shape, tris, false);
    append_cap(mesh, pv, shape, tris, true);
    unmatched += pv.unmatched + pv.duplicates;
}

// The whole stack. Serial and ordered, so the vertex and face order is a pure function of the
// input - the determinism the bake asserts.
indexed_triangle_set prisms_to_mesh(const std::vector<ExPolygons> &slices,
                                    const std::vector<double>     &bottom_z,
                                    const std::vector<double>     &top_z,
                                    size_t                        &unmatched,
                                    size_t                        &fallbacks,
                                    size_t                        &holes_dropped,
                                    size_t                        &islands_dropped)
{
    indexed_triangle_set mesh;
    for (size_t i = 0; i < slices.size(); ++i)
        append_layer_prism(mesh, slices[i], bottom_z[i], top_z[i], unmatched, fallbacks, holes_dropped, islands_dropped);

    // Weld only EXACTLY coincident vertices: neighbouring prisms share a Z plane and, wherever
    // their contours share a point, that point's float coordinates are bit-identical (both sides
    // went through the same unscaled().cast<float>()). Nothing here is a tolerance merge, so a
    // weld can never pull two distinct contour points together and puncture the mesh.
    its_merge_vertices(mesh);
    its_remove_degenerate_faces(mesh);
    its_compactify_vertices(mesh);
    return mesh;
}

// ---------------------------------------------------------------------------------------------
// The smooth loft: sloped skirts instead of vertical steps
// ---------------------------------------------------------------------------------------------
//
// What the stepped stack looks like and why one might not want it. Every layer is a prism with
// VERTICAL walls over its own [bottom_z, print_z] band, so a sloped surface comes out as a
// staircase. That is a truthful picture of what the printer lays down, and it is the default. But
// when the bake is a re-sliceable replica of a curved part, the staircase is the one artefact the
// source mesh did not have, and re-slicing it at a finer layer height reproduces the COARSE steps
// rather than the curve. Hence the toggle.
//
// The construction is the same SHAPE of argument as the prism stack, which is what keeps it
// watertight: the Z range is cut into intervals, each interval is built as a solid that is CLOSED
// ON ITS OWN, and the closed solids are stacked and welded. Every edge of a closed solid has two
// incident faces; stacking cannot change that, so the union is closed too, exactly as for the
// prisms. What changes is only what one interval looks like inside.
//
// The intervals are the gaps between the layers' SAMPLE heights:
//
//   z[0]   = bottom_z of the first layer      (so the stack still reaches the bed)
//   z[i]   = the middle of layer i's band     (interior layers)
//   z[n-1] = print_z of the last layer        (so it still reaches the top)
//
// Sampling each interior layer at its MID height is what makes an interval an interpolation of the
// real surface: a slice contour is the cross-section of the part at (roughly) the middle of its
// band, so a straight line between two mid-heights is a chord of the real surface, not a chord
// that has been pushed out to a band edge first.
//
// Interval [z[i], z[i+1]] is built one of two ways:
//
//   RIBBON, when the two layers' rings CORRESPOND - the same number of rings, matched one to one
//     by winding and centroid proximity, and resampled to a common point count (the larger of the
//     two, by arc length) so the strip is a clean run of quads with no T-junctions. The interval
//     is that ribbon, plus a bottom cap over layer i and a top cap over layer i+1. Closed.
//
//   PRISM, otherwise - a hole opening, two islands merging, a ring appearing. Layer i's contour
//     extruded vertically over the whole interval, bottom and top caps both over layer i. Closed.
//     This is precisely the stepped construction applied to one interval, so the fallback is the
//     old behaviour locally and nothing else has to change.
//
// The cost against the prism stack: a ribbon replaces a vertical wall of the same point count, so
// the triangle count is about the same per interval, and there are n-1 intervals instead of n. The
// gain is that the wall is sloped.

// A ring resampled to exactly `n` points by arc length, starting from the point nearest `anchor`.
// Anchoring matters: two rings resampled from arbitrary starts would twist the ribbon between them
// into a spiral, which self-intersects.
Polygon resample_ring(const Polygon &poly, size_t n, const Point &anchor)
{
    const size_t m = poly.points.size();
    if (m < 3 || n < 3)
        return poly;

    size_t start = 0;
    double best  = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m; ++i) {
        const double dd = (unscaled(poly.points[i]) - unscaled(anchor)).squaredNorm();
        if (dd < best) { best = dd; start = i; }
    }

    std::vector<Vec2d> pts;
    pts.reserve(m);
    for (size_t i = 0; i < m; ++i)
        pts.push_back(unscaled(poly.points[(start + i) % m]));

    std::vector<double> cum(m + 1, 0.);
    for (size_t i = 0; i < m; ++i)
        cum[i + 1] = cum[i] + (pts[(i + 1) % m] - pts[i]).norm();
    const double total = cum[m];
    if (total < EPSILON)
        return poly;

    Polygon out;
    out.points.reserve(n);
    size_t seg = 0;
    for (size_t k = 0; k < n; ++k) {
        const double t = total * double(k) / double(n);
        while (seg + 1 < m && cum[seg + 1] < t)
            ++seg;
        const double seg_len = cum[seg + 1] - cum[seg];
        const double u = seg_len > EPSILON ? (t - cum[seg]) / seg_len : 0.;
        const Vec2d  q = pts[seg] + u * (pts[(seg + 1) % m] - pts[seg]);
        out.points.emplace_back(coord_t(std::llround(scale_(q.x()))), coord_t(std::llround(scale_(q.y()))));
    }
    out.points.erase(std::unique(out.points.begin(), out.points.end()), out.points.end());
    while (out.points.size() > 1 && out.points.front() == out.points.back())
        out.points.pop_back();
    return out.points.size() >= 3 ? out : poly;
}

// Every ring of a slice, flattened: each ExPolygon's contour, then its holes. The ORDER is what the
// pairing below matches on, and it is a pure function of the slice.
std::vector<const Polygon *> slice_rings(const ExPolygons &slice)
{
    std::vector<const Polygon *> out;
    for (const ExPolygon &ex : slice) {
        if (ex.contour.points.size() >= 3)
            out.push_back(&ex.contour);
        for (const Polygon &h : ex.holes)
            if (h.points.size() >= 3)
                out.push_back(&h);
    }
    return out;
}

// Do two slices correspond ring for ring? Requires the same ring count, the same winding on each
// pair (a contour must never be matched to a hole), and every pair's bounding-box centres within a
// tolerance that scales with the ring itself - so a wall leaning layer to layer still pairs up,
// but two unrelated islands do not.
//
// `order` receives, for each ring of the LOWER slice, the index of its partner in the upper.
bool rings_correspond(const std::vector<const Polygon *> &lo,
                      const std::vector<const Polygon *> &hi,
                      std::vector<size_t>                &order)
{
    if (lo.empty() || lo.size() != hi.size())
        return false;

    order.assign(lo.size(), 0);
    std::vector<bool> taken(hi.size(), false);
    for (size_t i = 0; i < lo.size(); ++i) {
        const BoundingBox bb_lo = lo[i]->bounding_box();
        const Vec2d  c_lo = unscaled(bb_lo.center());
        // A quarter of the ring's own diagonal, floored at 1 mm. Layer to layer a contour moves by
        // at most the wall angle times the layer height, which is far inside this.
        const double tol  = std::max(1., 0.25 * unscaled(bb_lo.size()).norm());
        size_t best  = hi.size();
        double bestd = std::numeric_limits<double>::max();
        for (size_t j = 0; j < hi.size(); ++j) {
            if (taken[j] || hi[j]->is_clockwise() != lo[i]->is_clockwise())
                continue;
            const double dd = (unscaled(hi[j]->bounding_box().center()) - c_lo).squaredNorm();
            if (dd < bestd) { bestd = dd; best = j; }
        }
        if (best == hi.size() || std::sqrt(bestd) > tol)
            return false;
        taken[best] = true;
        order[i]    = best;
    }
    return true;
}

// The sloped ribbon between one pair of corresponding rings, plus the two vertex runs it needs.
// Both rings are resampled to the same point count so the strip is a clean run of quads. The
// winding follows append_ring_walls': walking the lower ring in its own traversal direction and
// closing to the upper gives an outward normal for a CCW contour and, by the same expression, for
// a CW hole.
//
// The resampled rings are handed back through `out_lo` / `out_hi` because the interval's CAPS have
// to be tesselated over the resampled point set, not the original - a cap vertex that is not also
// a ribbon vertex is a T-junction, which is the crack this whole loft exists to avoid.
void append_skirt(indexed_triangle_set &mesh, const Polygon &lo, const Polygon &hi,
                  double z_lo, double z_hi, Polygon &out_lo, Polygon &out_hi)
{
    const size_t n = std::max(lo.points.size(), hi.points.size());
    out_lo = lo;
    out_hi = hi;
    if (n < 3)
        return;
    const Polygon rlo = resample_ring(lo, n, lo.points.front());
    const Polygon rhi = resample_ring(hi, n, lo.points.front());
    if (rlo.points.size() != rhi.points.size() || rlo.points.size() < 3)
        return;
    out_lo = rlo;
    out_hi = rhi;

    const int m     = int(rlo.points.size());
    const int first = int(mesh.vertices.size());
    for (const Point &p : rlo.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_lo)));
    for (const Point &p : rhi.points)
        mesh.vertices.emplace_back(to_3d(unscaled(p).cast<float>().eval(), float(z_hi)));
    for (int i = 0; i < m; ++i) {
        const int j    = (i + 1) % m;
        const int lo_i = first + i,     lo_j = first + j;
        const int hi_i = first + m + i, hi_j = first + m + j;
        mesh.indices.emplace_back(lo_i, lo_j, hi_j);
        mesh.indices.emplace_back(lo_i, hi_j, hi_i);
    }
}

// One flat cap over a ring set at `z`, with vertices of its own. The same GLU tesselation the prism
// caps use, over the same points the ribbon was built on, so it welds to the ribbon exactly.
//
// `rings` is a flat ring list in slice_rings' order; it is rebuilt into ExPolygons for the
// tesselator by re-reading the windings, which is what the tesselator's even-odd rule wants anyway.
void append_flat_cap_rings(indexed_triangle_set &mesh, const ExPolygons &shape,
                           const std::vector<Polygon> &rings, double z, bool top, size_t &unmatched,
                           size_t &fallbacks, size_t &holes_dropped, size_t &islands_dropped)
{
    if (rings.empty())
        return;
    // Same cap-first rule as append_layer_prism: whatever buildable_slice reduces the shape to is
    // what the vertices are laid down for.
    std::vector<Triangulation::Indices> tris;
    const ExPolygons built = buildable_slice(shape, tris, fallbacks, holes_dropped, islands_dropped);
    if (built.empty())
        return;

    PrismVertices pv;
    for (const ExPolygon &ex : built) {
        if (ex.contour.points.size() >= 3)
            append_ring_vertices(mesh, pv, ex.contour, z, z);
        for (const Polygon &hole : ex.holes)
            if (hole.points.size() >= 3)
                append_ring_vertices(mesh, pv, hole, z, z);
    }
    append_cap(mesh, pv, built, tris, top);
    unmatched += pv.unmatched + pv.duplicates;
}

// An ExPolygons rebuilt from a flat ring list in slice_rings' order, so the tesselator sees the
// same shape the ribbons were built from. `shape` supplies the structure (how many holes belong to
// which contour); `rings` supplies the (possibly resampled) points.
ExPolygons rings_to_expolygons(const ExPolygons &shape, const std::vector<Polygon> &rings)
{
    ExPolygons out;
    out.reserve(shape.size());
    size_t k = 0;
    for (const ExPolygon &ex : shape) {
        if (ex.contour.points.size() < 3)
            continue;
        ExPolygon n;
        n.contour = rings[k++];
        for (const Polygon &h : ex.holes)
            if (h.points.size() >= 3)
                n.holes.push_back(rings[k++]);
        out.emplace_back(std::move(n));
    }
    return out;
}

// The smoothed stack: one closed solid per Z interval, welded.
indexed_triangle_set smooth_loft_to_mesh(const std::vector<ExPolygons> &slices,
                                         const std::vector<double>     &bottom_z,
                                         const std::vector<double>     &top_z,
                                         size_t                        &unmatched,
                                         size_t                        &lofted_bands,
                                         size_t                        &fallbacks,
                                    size_t                        &holes_dropped,
                                    size_t                        &islands_dropped)
{
    indexed_triangle_set mesh;
    const size_t n = slices.size();
    if (n == 0)
        return mesh;
    if (n == 1) {
        // Nothing to interpolate between: one layer is one prism, smoothing or not.
        append_layer_prism(mesh, slices[0], bottom_z[0], top_z[0], unmatched, fallbacks, holes_dropped, islands_dropped);
        its_merge_vertices(mesh);
        its_remove_degenerate_faces(mesh);
        its_compactify_vertices(mesh);
        return mesh;
    }

    // The sample height of every layer. Interior layers sit at the middle of their own band; the
    // two ends are pushed out so the stack still spans the object's full height.
    std::vector<double> zs(n);
    for (size_t i = 0; i < n; ++i)
        zs[i] = 0.5 * (bottom_z[i] + top_z[i]);
    zs.front() = bottom_z.front();
    zs.back()  = top_z.back();

    std::vector<std::vector<const Polygon *>> rings(n);
    for (size_t i = 0; i < n; ++i)
        rings[i] = slice_rings(slices[i]);

    // Each interval is built as a solid CLOSED ON ITS OWN, and the closed solids are stacked and
    // welded. That is the same argument the prism stack rests on, and it is the reason this loft is
    // watertight on any input: every edge of a closed solid has two incident faces, stacking cannot
    // change that, so the union is closed.
    //
    // The alternative was tried and rejected. Consecutive ribbon intervals LOOK like they should be
    // able to share their boundary and skip the cap pair there - it is the same layer's contour on
    // both sides - which would remove the internal horizontal faces entirely. They cannot, because
    // append_skirt resamples each ribbon to max(lower, upper) points, so interval i's upper ring
    // and interval i+1's lower ring are resampled to DIFFERENT counts whenever the layer above
    // differs in point count from the layer below. Carrying the rings across to force a match makes
    // the ribbon above no longer match the layer it is supposed to reach. Measured, that variant
    // opened a sphere's bake by 26,175 edges and a bored block's by 4,740, which is exactly the
    // class of defect this whole loft exists to avoid.
    //
    // So the cap pairs stay. What smoothing buys is what the user actually sees: the WALLS are
    // sloped instead of stepped, so a curved part reads as curved and re-slices as curved. The caps
    // between two ribbons are coplanar, opposite, and interior to the solid - the same internal
    // structure the prism stack has always carried, and the same thing nothing downstream minds.
    for (size_t i = 0; i + 1 < n; ++i) {
        if (zs[i + 1] <= zs[i] + EPSILON)
            continue;                      // a zero-height interval contributes nothing

        std::vector<size_t> order;
        if (rings_correspond(rings[i], rings[i + 1], order)) {
            // A RIBBON interval: sloped walls, a bottom cap over layer i and a top cap over layer
            // i+1, all three over the RESAMPLED point sets the ribbons produced. Tesselating a cap
            // over the ORIGINAL points instead would put cap vertices where the ribbon has none -
            // T-junctions, and the cracks they bring.
            std::vector<Polygon> lo_rings(rings[i].size()), hi_rings(rings[i].size());
            for (size_t k = 0; k < rings[i].size(); ++k)
                append_skirt(mesh, *rings[i][k], *rings[i + 1][order[k]], zs[i], zs[i + 1],
                             lo_rings[k], hi_rings[k]);
            // The upper rings come back in the LOWER slice's ring order (that is what `order`
            // maps), so slices[i] is the right structural template for both ends: rings_correspond
            // pairs contour to contour and hole to hole, so the two ends have the same shape.
            const ExPolygons lo_shape = rings_to_expolygons(slices[i], lo_rings);
            const ExPolygons hi_shape = rings_to_expolygons(slices[i], hi_rings);
            append_flat_cap_rings(mesh, lo_shape, lo_rings, zs[i],     false, unmatched, fallbacks, holes_dropped, islands_dropped);
            append_flat_cap_rings(mesh, hi_shape, hi_rings, zs[i + 1], true,  unmatched, fallbacks, holes_dropped, islands_dropped);
            ++lofted_bands;
        } else {
            // A PRISM interval: exactly the stepped construction, over this one interval. Closed by
            // its own two caps, so it seals against whatever is above and below it.
            append_layer_prism(mesh, slices[i], zs[i], zs[i + 1], unmatched, fallbacks, holes_dropped, islands_dropped);
        }
    }

    its_merge_vertices(mesh);
    its_remove_degenerate_faces(mesh);
    its_compactify_vertices(mesh);
    return mesh;
}

void clamp_range(const PrintObject &object, const SliceBakeOptions &opts, size_t &begin, size_t &end)
{
    const size_t n = object.layer_count();
    begin = std::min(opts.layer_begin, n);
    end   = std::min(opts.layer_end, n);
    if (end < begin)
        end = begin;
}

} // namespace

bool slice_bake_available(const PrintObject &object)
{
    for (const Layer *layer : object.layers())
        if (layer != nullptr && layer_has_outer_wall(*layer))
            return true;
    return false;
}

double slice_bake_default_resolution(const PrintObject &object)
{
    // The print's `resolution` is the tolerance the G-code path was simplified at, so baking at
    // the same number reproduces what the preview drew: finer is pointless (the detail is already
    // gone from the extrusion source) and coarser throws away detail the slice contours still
    // have. A print that leaves it at 0 means "do not simplify", for which the bake has no
    // meaningful tolerance of its own, so it falls back to its default.
    const Print *print = object.print();
    if (print == nullptr)
        return SLICE_BAKE_RESOLUTION_DEFAULT;
    const double r = print->config().resolution.value;
    if (r <= 0.)
        return SLICE_BAKE_RESOLUTION_DEFAULT;
    return std::clamp(r, SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);
}

size_t slice_bake_estimate_triangles(const PrintObject &object, const SliceBakeOptions &opts)
{
    size_t begin = 0, end = 0;
    clamp_range(object, opts, begin, end);

    const bool   from_slices = opts.contour_source == SliceBakeContourSource::SliceContours;
    const double tol         = std::clamp(opts.resolution, SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);

    size_t points = 0;
    size_t layers = 0;
    for (size_t i = begin; i < end; ++i) {
        const Layer *layer = object.get_layer(int(i));
        if (layer == nullptr)
            continue;
        size_t layer_points = 0;
        if (from_slices) {
            // The slice contours: the densifier runs on these, so the estimate runs the same
            // curvature test over the same points. It is the only way the figure can move when the
            // user drags the resolution, which is the point of showing it live.
            for (const LayerRegion *region : layer->regions()) {
                if (region == nullptr)
                    continue;
                for (const Surface &surface : region->slices.surfaces) {
                    size_t added = 0;
                    if (surface.expolygon.contour.points.size() >= 3) {
                        layer_points += densify_polygon(surface.expolygon.contour, tol, added).points.size();
                    }
                    for (const Polygon &hole : surface.expolygon.holes)
                        if (hole.points.size() >= 3)
                            layer_points += densify_polygon(hole, tol, added).points.size();
                }
            }
        } else {
            // The extrusion centreline. Densifying it too, for the same reason.
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->perimeters.entities) {
                    Polylines pls;
                    entity->collect_polylines(pls);
                    for (const Polyline &pl : pls) {
                        if (pl.points.size() < 3) {
                            layer_points += pl.points.size();
                            continue;
                        }
                        size_t  added = 0;
                        Polygon as_poly;
                        as_poly.points = pl.points;
                        layer_points += densify_polygon(as_poly, tol, added).points.size();
                    }
                }
        }
        points += layer_points;
        if (layer_points > 0)
            ++layers;
    }

    if (opts.smooth_vertical_steps) {
        // The smoothed stack is one sloped ribbon per INTERVAL - n-1 of them over n layers - plus
        // a cap pair at each interval's two ends. Two ribbon triangles per point, and caps of
        // about n-2 triangles over the same n points: four per point again, over one fewer band.
        const double bands = layers > 1 ? double(layers - 1) / double(layers) : 1.;
        return size_t(double(points) * 4. * bands);
    }
    // Two wall triangles per boundary point, plus a bottom and a top cap of about n-2 each over the
    // same n points: four per point. Deliberately a plain multiplication - a figure shown as
    // "about N" must not pretend to know the tesselation.
    return points * 4;
}

std::vector<ExPolygons> slice_bake_layer_regions(const PrintObject       &object,
                                                 const SliceBakeOptions  &opts,
                                                 std::vector<double>     *out_z,
                                                 std::vector<double>     *out_bottom_z,
                                                 SliceBakeReport         *report,
                                                 const SliceBakeProgress &progress)
{
    size_t begin = 0, end = 0;
    clamp_range(object, opts, begin, end);

    std::vector<ExPolygons> slices;
    slices.reserve(end - begin);
    if (out_z != nullptr)        out_z->clear();
    if (out_bottom_z != nullptr) out_bottom_z->clear();

    const double close_radius = std::clamp(opts.close_gaps_radius, 0., SLICE_BAKE_CLOSE_GAPS_MAX);
    const double tol          = std::clamp(opts.resolution, SLICE_BAKE_RESOLUTION_MIN, SLICE_BAKE_RESOLUTION_MAX);
    size_t loops_used = 0, empty_layers = 0;
    size_t from_slices = 0, from_extrusion = 0, points_added = 0, nudged = 0, unresolved = 0;


    // Sequential on purpose. The per-layer work is one Clipper union of a few hundred polygons -
    // small next to the loft that follows - and a serial loop is the cheapest way to guarantee
    // the output ordering is a pure function of the input, which is what "deterministic" has to
    // mean here.
    for (size_t i = begin; i < end; ++i) {
        if (progress) {
            // The footprints are the first half of the bake; the loft is the second.
            const int pct = end > begin ? int(50. * double(i - begin) / double(end - begin)) : 0;
            if (! progress(pct))
                throw SliceBakeCancelled();
        }

        const Layer *layer = object.get_layer(int(i));
        if (layer == nullptr) {
            ++empty_layers;
            continue;
        }

        // The two sources, and the fallback between them. SliceContours is preferred because the
        // extrusion centreline reaching this point has already been simplified at the print's
        // `resolution` - that simplification is the owner's "more angular than the preview" - but a
        // layer whose regions carry no slice surface (it can happen on a layer built entirely from
        // a bridge or from an empty region) still has extrusions, so the fallback is per LAYER and
        // is counted, rather than the whole bake refusing one source or the other.
        Polygons footprint;
        bool     used_slices = false;
        if (opts.contour_source == SliceBakeContourSource::SliceContours)
            used_slices = append_slice_contours(*layer, footprint, loops_used);
        if (! used_slices) {
            footprint.clear();
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->perimeters.entities)
                    collect_outer_walls(entity, footprint, loops_used);
        }
        if (used_slices)
            ++from_slices;
        else
            ++from_extrusion;

        if (footprint.empty()) {
            // A layer with no outer wall contributes nothing and is dropped rather than lofted as
            // an empty slice - an empty ExPolygons in the middle of the stack would pinch the
            // mesh shut and reopen it, which is not what the object looks like.
            ++empty_layers;
            continue;
        }

        // Union with pftNonZero fills every loop's interior: the offset ring of a closed loop
        // encloses its own inside, and the non-zero rule keeps that inside solid instead of
        // treating it as a hole. This is what makes the bake a SOLID - and what makes top and
        // bottom surfaces implicit, since a filled top layer already is the top skin.
        //
        // STRICTLY SIMPLE, which a plain union_ex is not. Clipper's default output may contain
        // SELF-TOUCHING paths: one closed ring that returns to a vertex it already visited, which
        // is what two sharp concave features of the boundary produce as soon as their offsets meet
        // - all over a Benchy's deck cutout and bow. That is a legal polygon and a correct answer,
        // but it makes the cap table ambiguous and the cap triangulation stitch a blade across the
        // pinch. simplify_polygons_ex runs the same non-zero union with Clipper's StrictlySimple
        // flag, which splits such a path into separate simple ones. Clipper's own comment calls the
        // flag expensive; it runs once per layer over a few hundred polygons, which is nothing next
        // to the loft that follows.
        ExPolygons filled = simplify_polygons_ex(footprint);

        if (close_radius > 0.) {
            const float delta = float(scale_(close_radius));
            filled = closing_ex(to_polygons(filled), delta, delta);
            // The close is two more Clipper offsets, and the dilation half of it brings separated
            // boundaries into contact ON PURPOSE - that is what closing a gap IS - so it
            // reintroduces pinches freely. Re-simplify.
            filled = simplify_polygons_ex(to_polygons(filled));
        }

        if (filled.empty()) {
            ++empty_layers;
            continue;
        }

        // Densify LAST, after the union and the closing: both are Clipper operations that would
        // otherwise throw the added points away again (a union re-emits its own intersection
        // vertices) or, worse, be handed several times the input they need.
        densify_expolygons(filled, tol, points_added);

        // ... and unpinch LAST OF ALL, because the densifier inserts points and DOES land them on
        // XYs that are already used - the extrusion source's loops run within a fraction of a
        // millimetre of each other on a fuzzed wall, and interpolated points on both round to the
        // same lattice square. After this the layer visits every XY at most once, which is the
        // invariant both the cap triangulation and PrismVertices depend on.
        // The layer's two invariants, established TOGETHER because each can break the other:
        //
        //   (1) no XY is visited twice     - or Triangulation::triangulate switches to a path that
        //                                    collapses the duplicates and returns indices into a
        //                                    point list the wall vertex runs do not mirror;
        //   (2) the islands are DISJOINT   - or two caps cover the same ground and the edges they
        //                                    share carry three faces, which reads as an open mesh.
        //
        // unpinch_slice gives (1) by moving a colliding point ~60 nm along its own outward
        // bisector, which can push one island across another and break (2). union_ex gives (2)
        // exactly on the integer lattice, but it can split an edge at an intersection and land the
        // new vertex on a coordinate already in use, breaking (1). So they are iterated to a fixed
        // point rather than run once each - three passes is already more than any measured layer
        // needs (the owner's Benchy converges on the first or second), and the loop exits as soon
        // as a pass changes nothing.
        //
        // Why this matters at all, and why only on the extrusion source: a fuzzed wall's outward
        // offset leaves islands running within nanometres of each other all over a Benchy hull.
        // Measured before this loop existed, one layer in a thousand had two islands overlapping by
        // 9.3e-6 mm^2 - a few square microns - and that was enough for several hundred unbalanced
        // edges. The slice-contour source produced none, which is exactly why it never leaked.
        // The order within a pass is union THEN unpinch, and the loop runs until unpinch has
        // nothing left to do. That order matters: whichever of the two runs last is the one whose
        // invariant survives, and (1) is the one that cannot be re-established afterwards by
        // anything cheap - a duplicate XY sends the triangulation down a path that returns indices
        // into a collapsed point list, with no way to tell from the outside. (2), by contrast, is
        // only broken by a nudge, and a nudge that breaks it moves a point by 60 nm across a
        // boundary the union had just made disjoint - so the loop simply runs again.
        //
        // Convergence: each pass either changes nothing (and exits) or removes at least one
        // collision. Three passes is far more than any measured layer needs - the owner's Benchy
        // settles on the first - and the bound keeps a pathological slice from spinning.
        for (int pass = 0; pass < 3; ++pass) {
            if (filled.size() > 1)
                filled = union_ex(to_polygons(filled));
            if (filled.empty())
                break;
            const size_t before = nudged;
            unpinch_slice(filled, nudged, unresolved);
            if (nudged == before)
                break;                      // no duplicate to move: both invariants hold
        }

        slices.emplace_back(std::move(filled));
        if (out_z != nullptr)        out_z->push_back(layer->print_z);
        if (out_bottom_z != nullptr) out_bottom_z->push_back(layer->bottom_z());
    }

    if (report != nullptr) {
        report->layers_baked         = slices.size();
        report->loops_used           = loops_used;
        report->empty_layers         = empty_layers;
        report->layers_from_slices   = from_slices;
        report->layers_from_extrusion = from_extrusion;
        report->points_added         = points_added;
        report->pinch_points_nudged  = nudged;
        report->pinch_points_unresolved = unresolved;

    }
    return slices;
}

indexed_triangle_set slice_bake_to_mesh(const PrintObject       &object,
                                        const SliceBakeOptions  &opts,
                                        SliceBakeReport         *report,
                                        const SliceBakeProgress &progress)
{
    SliceBakeReport local;
    SliceBakeReport &rep = report != nullptr ? *report : local;

    std::vector<double> z, bottom_z;
    std::vector<ExPolygons> slices = slice_bake_layer_regions(object, opts, &z, &bottom_z, &rep, progress);

    if (slices.empty()) {
        rep.note = "the object has no outer wall to bake - slice the plate first";
        return {};
    }

    if (progress && ! progress(55))
        throw SliceBakeCancelled();

    // Each printed layer occupies [bottom_z, print_z] and becomes one closed prism spanning
    // exactly that band, so a variable or adaptive layer height and a subset that does not start
    // at the bed both come out right without a uniform grid having to be derived. With smoothing
    // on, the same Z range is cut into intervals between the layers' mid-heights instead and each
    // interval is lofted; see smooth_loft_to_mesh for why that is still closed.
    size_t unmatched = 0, lofted = 0, fallbacks = 0, holes_dropped = 0, islands_dropped = 0;
    indexed_triangle_set mesh = opts.smooth_vertical_steps
                                    ? smooth_loft_to_mesh(slices, bottom_z, z, unmatched, lofted, fallbacks, holes_dropped, islands_dropped)
                                    : prisms_to_mesh(slices, bottom_z, z, unmatched, fallbacks, holes_dropped, islands_dropped);
    rep.lofted_bands          = lofted;
    rep.cap_triangles_dropped = unmatched;
    rep.cap_glu_fallbacks     = fallbacks;
    rep.cap_holes_dropped     = holes_dropped;
    rep.islands_dropped       = islands_dropped;
    if (unmatched > 0)
        rep.note = std::to_string(unmatched) + " cap triangle(s) dropped as slivers or unmatched";

    if (progress && ! progress(85))
        throw SliceBakeCancelled();

    // ---- the frame ------------------------------------------------------------------------------
    //
    // What the mesh is in right now: PRINT space. The sliced geometry went through
    // trafo_centered(), which is the instance's rotation, scale and Z placement, followed by a
    // translation of -center_offset that puts the object near the origin so Clipper works in small
    // coordinates. The instance's own XY position is NOT in it - that lives in
    // PrintInstance::shift, which is the instance offset PLUS the same center_offset, plus the
    // plate origin.
    //
    // So, writing W for a point in world millimetres (the model space every plate is laid out in,
    // NOT relative to any one plate), P for the same point in print space and S for the instance's
    // full shift (PrintInstance::shift, which includes the plate origin because ModelInstance
    // offsets are world values):
    //
    //     W = P + S                        (S is XY only; Z is already right in P)
    //
    // and the instance's own XY offset - what ModelInstance::get_offset() holds, and what a new
    // instance would have to be given - is
    //
    //     offset_xy = unscaled(S - center_offset)
    //
    // because PrintObject::set_instances adds center_offset into every shift. S must NOT have the
    // plate origin taken off (shift_without_plate_offset()): that is plate-LOCAL, and a new
    // ModelInstance given a plate-local offset lands on plate 1 whatever plate the source is on.
    // The two frames below fall straight out of that.
    switch (opts.frame) {
    case SliceBakeFrame::Print:
        // Nothing to do: the caller asked for exactly what was sliced.
        break;

    case SliceBakeFrame::Object: {
        // Undo trafo_centered() so the mesh lands where the ModelVolume's own mesh sits. A volume
        // whose transform is reset to identity then carries this mesh, and the object's EXISTING
        // instance re-applies the rotation and the scale - which is why they must come OUT of the
        // vertices here.
        const Transform3d inv = object.trafo_centered().inverse();
        for (Vec3f &v : mesh.vertices)
            v = (inv * v.cast<double>()).cast<float>();
        // A mirroring or negative-determinant instance transform flips the winding; put it back
        // so the replacement mesh is not inside-out.
        if (inv.linear().determinant() < 0.)
            for (Vec3i32 &f : mesh.indices)
                std::swap(f(1), f(2));
        break;
    }

    case SliceBakeFrame::World: {
        // The frame a NEW ModelObject needs. The instance rotation and scale STAY in the vertices -
        // undoing them would need the new instance to re-apply them, and a fresh ModelObject gets
        // an identity instance, which is exactly the bug this is fixing: the phase-1 code handed an
        // Object-frame mesh (scale and rotation stripped out) to load_mesh_object, which then never
        // put them back, so a 2x-scaled part baked to a 1x object.
        //
        // What DOES have to come out is the position, so the new object's own instance offset can
        // carry it: translate print space by S to reach world, then subtract the instance offset
        // so the mesh sits around the origin the way a loaded mesh does. Net translation is
        // therefore (S - offset_xy) = center_offset, in XY, and nothing in Z.
        //
        // Z: the instance's Z offset is already inside the vertices (trafo_centered carries it),
        // so the new instance's Z offset is 0 and the mesh keeps the real heights. That also makes
        // "the bake sits on the bed exactly where the slice did" true without an ensure_on_bed().
        Vec2d offset_xy(0., 0.);
        Vec2d translate(0., 0.);
        if (! object.instances().empty()) {
            // The FULL shift, plate origin included: the new instance's offset is a world value.
            const Point s_full = object.instances().front().shift;
            // Point arithmetic yields an Eigen EXPRESSION, which unscaled() has no overload for;
            // name the concrete Point first.
            const Point own(s_full.x() - object.center_offset().x(), s_full.y() - object.center_offset().y());
            offset_xy = unscaled(own);
            translate = unscaled(Point(object.center_offset().x(), object.center_offset().y()));
        }
        for (Vec3f &v : mesh.vertices)
            v = Vec3f(v.x() + float(translate.x()), v.y() + float(translate.y()), v.z());
        rep.instance_offset = Vec3d(offset_xy.x(), offset_xy.y(), 0.);
        break;
    }
    }

    rep.triangles  = mesh.indices.size();
    rep.vertices   = mesh.vertices.size();
    rep.watertight = ! mesh.indices.empty() && its_num_open_edges(mesh) == 0;
    if (! mesh.vertices.empty()) {
        rep.z_min = rep.z_max = mesh.vertices.front().z();
        for (const Vec3f &v : mesh.vertices) {
            rep.z_min = std::min(rep.z_min, double(v.z()));
            rep.z_max = std::max(rep.z_max, double(v.z()));
        }
    }

    BOOST_LOG_TRIVIAL(info) << "slice_bake: " << rep.layers_baked << " layers, " << rep.loops_used
                            << " outer-wall paths, " << rep.triangles << " triangles, "
                            << (rep.watertight ? "watertight" : "NOT watertight")
                            << ", " << rep.empty_layers << " empty layer(s) skipped"
                            << ", source " << rep.layers_from_slices << " slice / "
                            << rep.layers_from_extrusion << " extrusion"
                            << ", +" << rep.points_added << " densified points"
                            << ", " << rep.lofted_bands << " lofted band(s)"
                            << ", " << rep.pinch_points_nudged << " pinch point(s) nudged"
                            << ", " << rep.cap_triangles_dropped << " cap triangle(s) dropped";

    if (progress && ! progress(100))
        throw SliceBakeCancelled();

    return mesh;
}

} // namespace Slic3r
