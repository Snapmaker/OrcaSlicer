#include "FillBedPack.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include "Polygon.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace fill_bed {

int estimate_count(double bbox_w, double bbox_h, double gap, double free_area)
{
    if (bbox_w <= 0. || bbox_h <= 0. || free_area <= 0.)
        return 0;

    const double g    = std::max(0., gap);
    const double cell = (bbox_w + g) * (bbox_h + g);
    if (cell <= 0.)
        return 0;

    const double n = std::floor(free_area / cell);
    if (n <= 0.)
        return 0;
    // Clamp before the cast: a tiny part on a huge bed can overflow int. This is a guard against
    // undefined behaviour, not the fill's cap - that is COUNT_CAP, applied by
    // estimate_count_with_overshoot(), so the raw tiling number stays honest for the dialog label.
    return int(std::min(n, 1.e9));
}

int estimate_count_with_overshoot(double bbox_w, double bbox_h, double gap, double free_area)
{
    const int base = estimate_count(bbox_w, bbox_h, gap, free_area);
    if (base <= 0)
        return 0;
    const double over = std::ceil(double(base) * COUNT_OVERSHOOT);
    return int(std::min(over, double(COUNT_CAP)));
}

Points shrink_bed_per_side(const Points &bedpts,
                           double        edge_margin,
                           double        front_margin,
                           bool          front_margin_enabled,
                           double        already_shrunk_x,
                           double        already_shrunk_y)
{
    Points out = bedpts;
    if (out.empty())
        return out;

    const double edge  = std::max(0., edge_margin);
    // Never closer to the front than the general margin: the override only ever widens it.
    const double front = front_margin_enabled ? std::max(edge, std::max(0., front_margin)) : edge;

    // What arrange's own shrink already took off each side; only the remainder is ours to move.
    const double dx       = std::max(0., edge - already_shrunk_x);
    const double dy_back  = std::max(0., edge - already_shrunk_y);
    const double dy_front = std::max(0., front - already_shrunk_y);

    if (dx <= 0. && dy_back <= 0. && dy_front <= 0.)
        return out;

    const Point center = Polygon(out).bounding_box().center();

    for (Point &pt : out) {
        // X: move every point towards the centre, exactly as get_shrink_bedpts does.
        if (dx > 0.) {
            const coord_t sgn = (center.x() - pt.x()) >= 0 ? 1 : -1;
            pt.x() += coord_t(scaled(dx)) * sgn;
        }
        // Y: the same, except that points on the front half (y below the centre) use the
        // - possibly larger - front margin.
        const bool   is_front = pt.y() < center.y();
        const double dy       = is_front ? dy_front : dy_back;
        if (dy > 0.) {
            const coord_t sgn = (center.y() - pt.y()) >= 0 ? 1 : -1;
            pt.y() += coord_t(scaled(dy)) * sgn;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------------------------
// The Grid layout
// ---------------------------------------------------------------------------------------------

namespace {

// The template rotated by `rot` radians about the origin, as the grid will place it. Only 0 and
// pi/2 are ever asked for, so this stays exact rather than a general rotation.
Polygon rotated_hull(const ExPolygon &tmpl, double rot)
{
    Polygon p = tmpl.contour;
    if (rot != 0.)
        p.rotate(rot);
    return p;
}

// One orientation's worth of grid, as a list of translations. Kept apart from grid_pack() so
// both orientations can be built and compared before either is returned.
std::vector<Point> grid_for_rotation(const Polygon                   &hull,
                                     const Polygon                   &bed_poly,
                                     const BoundingBox               &bed_bb,
                                     bool                             bed_is_box,
                                     coord_t                          gap,
                                     const std::vector<GridObstacle> &obstacles,
                                     size_t                           max_cells)
{
    std::vector<Point> out;

    const BoundingBox hull_bb = hull.bounding_box();
    const coord_t     w       = hull_bb.size().x();
    const coord_t     h       = hull_bb.size().y();
    if (w <= 0 || h <= 0)
        return out;

    // The gap is shared between neighbours, so one copy steps by its own size plus one gap.
    const coord_t step_x = w + gap;
    const coord_t step_y = h + gap;

    const coord_t span_x = bed_bb.size().x();
    const coord_t span_y = bed_bb.size().y();
    if (span_x < w || span_y < h)
        return out;

    // n copies span n*w + (n-1)*gap = n*step - gap, so the largest n that fits a span s is
    // floor((s + gap) / step).
    const int cols = int((span_x + gap) / step_x);
    const int rows = int((span_y + gap) / step_y);
    if (cols <= 0 || rows <= 0)
        return out;

    // Centre the block on the free bed area, so the leftover margin is split evenly and the
    // layout reads as deliberate rather than jammed into one corner.
    const coord_t used_x  = coord_t(cols) * step_x - gap;
    const coord_t used_y  = coord_t(rows) * step_y - gap;
    const coord_t start_x = bed_bb.min.x() + (span_x - used_x) / 2;
    const coord_t start_y = bed_bb.min.y() + (span_y - used_y) / 2;

    // Half the gap each, the same rule ArrangePolygon::inflation uses on the NFP path: two
    // neighbouring inflated boxes touch exactly when their outlines are one gap apart.
    const coord_t half_gap = gap / 2;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            // Where this cell lands, and hence how far the template has to move to get there.
            // The result is relative to the template's own position, which is the convention
            // ArrangePolygon::translation uses.
            const Point cell_min{start_x + coord_t(c) * step_x, start_y + coord_t(r) * step_y};
            const Point translation = cell_min - hull_bb.min;

            const BoundingBox cell_bb{cell_min, Point{cell_min.x() + w, cell_min.y() + h}};

            // Inside the bed? The gap is deliberately NOT added here - the bed outline already
            // carries arrange's shrink and the user's own edge margins, and a copy may sit that
            // close to the edge however large the copy-to-copy gap is.
            if (!bed_bb.contains(cell_bb))
                continue;
            if (!bed_is_box) {
                // Circular or custom bed: the whole moved hull has to be inside it.
                Polygon moved = hull;
                moved.translate(translation);
                if (!diff(moved, bed_poly).empty())
                    continue;
            }

            // Clear of everything already on the plate? The cell inflated by half the gap
            // against each obstacle inflated by its own clearance.
            BoundingBox infl = cell_bb;
            infl.offset(half_gap);

            bool blocked = false;
            for (const GridObstacle &ob : obstacles) {
                if (ob.outline.points.size() < 3)
                    continue;
                BoundingBox ob_bb = ob.outline.bounding_box();
                if (ob.inflation != 0)
                    ob_bb.offset(ob.inflation);
                // Boxes apart means no overlap, whatever the outlines look like.
                if (!infl.overlap(ob_bb))
                    continue;
                // Boxes touch: settle it on the real outline, so a copy can still sit in the
                // concavity of an L-shaped neighbour's bounding box.
                const Polygon  infl_poly = infl.polygon();
                const Polygons ob_polys  = ob.inflation != 0 ? offset(ob.outline, double(ob.inflation))
                                                             : Polygons{ob.outline};
                if (!intersection(Polygons{infl_poly}, ob_polys).empty()) {
                    blocked = true;
                    break;
                }
            }
            if (blocked)
                continue;

            out.emplace_back(translation);
            if (max_cells != 0 && out.size() >= max_cells)
                return out;
        }
    }

    return out;
}

} // namespace

std::vector<GridCell> grid_pack(const ExPolygon                 &tmpl,
                                const Points                    &bed,
                                coord_t                          gap,
                                bool                             allow_rotation,
                                const std::vector<GridObstacle> &obstacles,
                                size_t                           max_cells)
{
    std::vector<GridCell> out;
    if (tmpl.contour.points.size() < 3 || bed.size() < 3)
        return out;

    const coord_t     g   = std::max<coord_t>(0, gap);
    const Polygon     bed_poly(bed);
    const BoundingBox bed_bb = bed_poly.bounding_box();
    // A four-point outline whose area equals its bounding box is an axis-aligned rectangle, and
    // then the cheap box test is conclusive. Every printer the fill is used on has one.
    const bool bed_is_box = bed_poly.points.size() == 4 &&
                            std::abs(bed_poly.area()) >= 0.999 * double(bed_bb.size().x()) * double(bed_bb.size().y());

    const Polygon      hull0    = rotated_hull(tmpl, 0.);
    std::vector<Point> best     = grid_for_rotation(hull0, bed_poly, bed_bb, bed_is_box, g, obstacles, max_cells);
    double             best_rot = 0.;

    if (allow_rotation) {
        // 90 degrees only. The spec's optional "widen the rotation set" item is deliberately
        // left out, and for a bounding-box tiling nothing between 0 and 90 could help anyway -
        // the step is the box either way.
        const double       rot90  = PI / 2.;
        const Polygon      hull90 = rotated_hull(tmpl, rot90);
        std::vector<Point> alt    = grid_for_rotation(hull90, bed_poly, bed_bb, bed_is_box, g, obstacles, max_cells);
        // Strictly more, so a tie leaves the copies in the template's own orientation.
        if (alt.size() > best.size()) {
            best     = std::move(alt);
            best_rot = rot90;
        }
    }

    out.reserve(best.size());
    for (const Point &t : best)
        out.push_back(GridCell{t, best_rot});
    return out;
}

size_t grid_count(const ExPolygon                 &tmpl,
                  const Points                    &bed,
                  coord_t                          gap,
                  bool                             allow_rotation,
                  const std::vector<GridObstacle> &obstacles)
{
    return grid_pack(tmpl, bed, gap, allow_rotation, obstacles, 0).size();
}

} // namespace fill_bed
} // namespace Slic3r
