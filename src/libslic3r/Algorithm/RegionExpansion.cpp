#include "RegionExpansion.hpp"

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/ClipperZUtils.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Int128.hpp>
#include <libslic3r/Utils.hpp>

#include <algorithm>
#include <numeric>

namespace Slic3r {
namespace Algorithm {

// Calculating radius discretization according to the Clipper offsetter code, see ClipperOffset::DoGroupOffset()
inline double clipper_round_offset_error(double offset, double arc_tolerance)
{
    static constexpr const double def_arc_tolerance = 0.25;
    const double y =
        arc_tolerance <= 0 ?
            def_arc_tolerance :
            arc_tolerance > offset * def_arc_tolerance ?
                offset * def_arc_tolerance :
                arc_tolerance;
    double steps = std::min(M_PI / std::acos(1. - y / offset), offset * M_PI);
    return offset * (1. - cos(M_PI / steps));
}

RegionExpansionParameters RegionExpansionParameters::build(
    // Scaled expansion value
    float                full_expansion,
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float                expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t               max_nr_expansion_steps)
{
    assert(full_expansion > 0);
    assert(expansion_step > 0);
    assert(max_nr_expansion_steps > 0);

    RegionExpansionParameters out;
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    // The expansion should not be too tiny, but also small enough, so the following expansion will
    // compensate for tiny_expansion and bring the wave back to the boundary without producing
    // ugly cusps where it touches the boundary.
    out.tiny_expansion = std::min(0.25f * full_expansion, scaled<float>(0.05f));
    size_t nsteps = size_t(ceil((full_expansion - out.tiny_expansion) / expansion_step));
    if (max_nr_expansion_steps > 0)
        nsteps = std::min(nsteps, max_nr_expansion_steps);
    assert(nsteps > 0);
    out.initial_step = (full_expansion - out.tiny_expansion) / nsteps;
    if (nsteps > 1 && 0.25 * out.initial_step < out.tiny_expansion) {
        // Decrease the step size by lowering number of steps.
        nsteps       = std::max<size_t>(1, (floor((full_expansion - out.tiny_expansion) / (4. * out.tiny_expansion))));
        out.initial_step = (full_expansion - out.tiny_expansion) / nsteps;
    }
    if (0.25 * out.initial_step < out.tiny_expansion || nsteps == 1) {
        out.tiny_expansion = 0.2f * full_expansion;
        out.initial_step   = 0.8f * full_expansion;
    }
    out.other_step           = out.initial_step;
    out.num_other_steps      = nsteps - 1;

    // Accuracy of the offsetter for wave propagation.
    out.arc_tolerance        = scaled<double>(0.1);

    // Maximum inflation of seed contours over the boundary. Used to trim boundary to speed up
    // clipping during wave propagation. Needs to be in sync with the offsetter accuracy.
    // Clipper positive round offset should rather offset less than more.
    // Still a little bit of additional offset was added.
    out.max_inflation = (out.tiny_expansion + nsteps * out.initial_step) * 1.1;
//                (clipper_round_offset_error(out.tiny_expansion, co.ArcTolerance) + nsteps * clipper_round_offset_error(out.initial_step, co.ArcTolerance) * 1.5; // Account for uncertainty 

    return out;
}

// Similar to expolygons_to_zpaths(), but each contour of a single ExPolygon is expanded before
// converting to open zpaths (the first point is repeated at the end) carrying the same Z value.
static void expolygon_to_zpaths_expanded_opened(
    const ExPolygon &expoly, const float expansion, const coord_t z, Polygons &expanded_polys_out, ClipperZUtils::ZPaths &zpaths_out)
{
    expanded_polys_out.clear();
    zpaths_out.clear();
    expanded_polys_out.reserve(expoly.num_contours());
    zpaths_out.reserve(expoly.num_contours());
    for (size_t icontour = 0; icontour < expoly.num_contours(); ++ icontour) {
        // Orient CCW, then grow the contour and shrink the holes. The output contours are CCW.
        Polygon contour = expoly.contour_or_hole(icontour);
        if (! contour.is_counter_clockwise())
            contour.reverse();
        for (Polygon &expanded : offset(contour, icontour == 0 ? expansion : -expansion, jtSquare))
            if (expanded.points.size() >= 2) {
                zpaths_out.emplace_back(ClipperZUtils::to_zpath<true>(expanded.points, z));
                expanded_polys_out.emplace_back(std::move(expanded));
            }
    }
}

// Paths were created by splitting closed polygons into open paths and then by clipping them.
// Thus some pieces of the clipped polygons may now become split at the ends of the source polygons.
// Those ends are sorted lexicographically in "splits".
// Reconnect those split pieces.
static inline void merge_splits(ClipperZUtils::ZPaths &paths, std::vector<std::pair<ClipperZUtils::ZPoint, int>> &splits)
{
    for (auto it_path = paths.begin(); it_path != paths.end(); ) {
        ClipperZUtils::ZPath &path = *it_path;
        assert(path.size() >= 2);
        bool merged = false;
        if (path.size() >= 2) {
            const ClipperZUtils::ZPoint &front = path.front();
            const ClipperZUtils::ZPoint &back  = path.back();
            // The path before clipping was supposed to cross the clipping boundary or be fully out of it.
            // Thus the clipped contour is supposed to become open, with one exception: The anchor expands into a closed hole.
            if (front.x() != back.x() || front.y() != back.y()) {
                // Look up the ends in "splits", possibly join the contours.
                // "splits" maps into the other piece connected to the same end point.
                auto find_end = [&splits](const ClipperZUtils::ZPoint &pt) -> std::pair<ClipperZUtils::ZPoint, int>* {
                    auto it = std::lower_bound(splits.begin(), splits.end(), pt,
                        [](const auto &l, const auto &r){ return ClipperZUtils::zpoint_lower(l.first, r); });
                    return it != splits.end() && it->first == pt ? &(*it) : nullptr;
                };
                auto *end = find_end(front);
                bool  end_front = true;
                if (! end) {
                    end_front = false;
                    end = find_end(back);
                }
                if (end) {
                    // This segment ends at a split point of the source closed contour before clipping.
                    if (end->second == -1) {
                        // Open end was found, not matched yet.
                        end->second = int(it_path - paths.begin());
                    } else {
                        // Open end was found and matched with end->second
                        ClipperZUtils::ZPath &other_path = paths[end->second];
                        polylines_merge(other_path, other_path.front() == end->first, std::move(path), end_front);
                        if (std::next(it_path) == paths.end()) {
                            paths.pop_back();
                            break;
                        }
                        path = std::move(paths.back());
                        paths.pop_back();
                        merged = true;
                    }
                }
            }
        }
        if (! merged)
            ++ it_path;
    }
}

using AABBTreeBBoxes = AABBTreeIndirect::Tree<2, coord_t>;

struct BBoxNode
{
    size_t                      m_idx;
    AABBTreeBBoxes::BoundingBox m_bbox;

    BBoxNode(size_t idx, const BoundingBox &bbox) : m_idx(idx), m_bbox(bbox.min, bbox.max) {}
    size_t                             idx() const { return m_idx; }
    const AABBTreeBBoxes::BoundingBox& bbox() const { return m_bbox; }
    Point centroid() const
    {
        return Point(coord_t((int64_t(m_bbox.min().x()) + int64_t(m_bbox.max().x())) / 2),
                     coord_t((int64_t(m_bbox.min().y()) + int64_t(m_bbox.max().y())) / 2));
    }
};

static AABBTreeBBoxes build_aabb_tree_over_expolygons(const ExPolygons &expolygons)
{
    std::vector<BBoxNode> bboxes;
    bboxes.reserve(expolygons.size());
    for (size_t i = 0; i < expolygons.size(); ++ i) {
        BoundingBox bb = get_extents(expolygons[i].contour);
        if (bb.defined) {
            bb.offset(SCALED_EPSILON);
            bboxes.emplace_back(i, bb);
        }
    }
    AABBTreeBBoxes out;
    out.build_modify_input(bboxes);
    return out;
}

static int sample_in_expolygons(
    // AABB tree over expolygons
    const AABBTreeBBoxes &aabb_tree,
    const ExPolygons     &expolygons,
    const Point          &sample,
    bool                  border_result = true)
{
    int out = -1;
    AABBTreeIndirect::traverse(aabb_tree,
        [&sample](const AABBTreeBBoxes::Node &node) {
            return node.bbox.contains(sample);
        },
        [&expolygons, &sample, border_result, &out](const AABBTreeBBoxes::Node &node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            if (expolygons[node.idx].contains(sample, border_result)) {
                out = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return out;
}

// Up to this many boundaries are scanned linearly via cached bounding boxes; above this threshold an AABB tree is built.
static constexpr size_t boundaries_per_tree = 16;

// A gap spanning both sets of boxes separates independent clipping problems.
// Sources and boundaries touching at a projection endpoint remain in the same group.
static std::vector<std::vector<size_t>> split_at_layer_gaps(const std::vector<size_t>&      sources,
                                                            const std::vector<BoundingBox>& source_boxes,
                                                            const std::vector<BoundingBox>& boundary_boxes)
{
    struct Projection
    {
        coord_t lo, hi;
        size_t  source;
    };
    std::vector<Projection> intervals;
    intervals.reserve(sources.size() + boundary_boxes.size());
    for (int axis : {0, 1}) {
        intervals.clear();
        for (size_t s : sources)
            intervals.push_back({source_boxes[s].min[axis], source_boxes[s].max[axis], s});
        for (const BoundingBox& box : boundary_boxes)
            if (box.defined)
                intervals.push_back({box.min[axis], box.max[axis], source_boxes.size()});
        std::sort(intervals.begin(), intervals.end(), [](const Projection& a, const Projection& b) { return a.lo < b.lo; });
        std::vector<std::vector<size_t>> groups;
        coord_t                          end         = intervals.front().hi;
        bool                             needs_group = true;
        for (const Projection& p : intervals) {
            if (p.lo > end)
                needs_group = true;
            end = std::max(end, p.hi);
            if (p.source < source_boxes.size()) {
                if (needs_group) {
                    groups.emplace_back();
                    needs_group = false;
                }
                groups.back().push_back(p.source);
            }
        }
        if (groups.size() > 1) {
            for (auto& group : groups)
                std::sort(group.begin(), group.end());
            return groups;
        }
    }
    return {};
}

struct PolyOBB
{
    double ux = 1., uy = 0.;
    double u_min = 0., u_max = 0.;
    double v_min = 0., v_max = 0.;
};

template<class Range>
static PolyOBB compute_poly_obb_from_polys(const Range &polys, double pad)
{
    PolyOBB obb;
    double  best_len2 = -1.;
    double  ux = 1., uy = 0.;
    for (const Polygon &poly : polys) {
        const Points &pts = poly.points;
        const size_t  n   = pts.size();
        for (size_t i = 0; i < n; ++ i) {
            const Point &a  = pts[i];
            const Point &b  = pts[(i + 1 == n) ? 0 : i + 1];
            const double dx = double(b.x() - a.x());
            const double dy = double(b.y() - a.y());
            const double l2 = dx * dx + dy * dy;
            if (l2 > best_len2) {
                best_len2 = l2;
                if (l2 > 0.) {
                    const double inv = 1. / std::sqrt(l2);
                    ux = dx * inv;
                    uy = dy * inv;
                }
            }
        }
    }
    obb.ux = ux;
    obb.uy = uy;
    double u_min = std::numeric_limits<double>::max();
    double u_max = std::numeric_limits<double>::lowest();
    double v_min = std::numeric_limits<double>::max();
    double v_max = std::numeric_limits<double>::lowest();
    bool   any   = false;
    for (const Polygon &poly : polys) {
        for (const Point &p : poly.points) {
            any            = true;
            const double x = double(p.x()), y = double(p.y());
            const double u = ux * x + uy * y;
            const double v = -uy * x + ux * y;
            u_min = std::min(u_min, u);
            u_max = std::max(u_max, u);
            v_min = std::min(v_min, v);
            v_max = std::max(v_max, v);
        }
    }
    if (any) {
        obb.u_min = u_min - pad;
        obb.u_max = u_max + pad;
        obb.v_min = v_min - pad;
        obb.v_max = v_max + pad;
    }
    return obb;
}

static bool obb_separated(const PolyOBB &a, const PolyOBB &b)
{
    auto project_sep = [](const PolyOBB &ref, const PolyOBB &other) -> bool {
        const double vx = -ref.uy, vy = ref.ux;
        const double ox = other.ux, oy = other.uy;
        const double ovx = -oy, ovy = ox;
        const double us[2] = { other.u_min, other.u_max };
        const double vs[2] = { other.v_min, other.v_max };
        double       ru_min = std::numeric_limits<double>::max();
        double       ru_max = std::numeric_limits<double>::lowest();
        double       rv_min = std::numeric_limits<double>::max();
        double       rv_max = std::numeric_limits<double>::lowest();
        for (double u : us) {
            for (double v : vs) {
                const double px = u * ox + v * ovx;
                const double py = u * oy + v * ovy;
                const double ru = ref.ux * px + ref.uy * py;
                const double rv = vx * px + vy * py;
                ru_min = std::min(ru_min, ru);
                ru_max = std::max(ru_max, ru);
                rv_min = std::min(rv_min, rv);
                rv_max = std::max(rv_max, rv);
            }
        }
        return ru_max < ref.u_min || ru_min > ref.u_max || rv_max < ref.v_min || rv_min > ref.v_max;
    };
    return project_sep(a, b) || project_sep(b, a);
}

static double point_to_segment_dist2(const Point &p, const Point &a, const Point &b)
{
    const double dx   = double(b.x() - a.x());
    const double dy   = double(b.y() - a.y());
    const double px   = double(p.x() - a.x());
    const double py   = double(p.y() - a.y());
    const double len2 = dx * dx + dy * dy;
    if (len2 == 0.)
        return px * px + py * py;
    const double t  = std::clamp((px * dx + py * dy) / len2, 0., 1.);
    const double rx = px - t * dx;
    const double ry = py - t * dy;
    return rx * rx + ry * ry;
}

static bool segments_within_tol(const Point &a1, const Point &a2, const Point &b1, const Point &b2, coord_t tol, double tol2)
{
    if (std::max(a1.x(), a2.x()) + tol < std::min(b1.x(), b2.x()) ||
        std::max(b1.x(), b2.x()) + tol < std::min(a1.x(), a2.x()) ||
        std::max(a1.y(), a2.y()) + tol < std::min(b1.y(), b2.y()) ||
        std::max(b1.y(), b2.y()) + tol < std::min(a1.y(), a2.y()))
        return false;
    const int o1 = int128::orient(a1, a2, b1);
    const int o2 = int128::orient(a1, a2, b2);
    const int o3 = int128::orient(b1, b2, a1);
    const int o4 = int128::orient(b1, b2, a2);
    if (o1 != o2 && o3 != o4)
        return true;
    auto on_seg = [](const Point &p, const Point &q, const Point &r) {
        return q.x() >= std::min(p.x(), r.x()) && q.x() <= std::max(p.x(), r.x()) &&
               q.y() >= std::min(p.y(), r.y()) && q.y() <= std::max(p.y(), r.y());
    };
    if ((o1 == 0 && on_seg(a1, b1, a2)) || (o2 == 0 && on_seg(a1, b2, a2)) ||
        (o3 == 0 && on_seg(b1, a1, b2)) || (o4 == 0 && on_seg(b1, a2, b2)))
        return true;
    if (tol <= 0)
        return false;
    return point_to_segment_dist2(a1, b1, b2) <= tol2 || point_to_segment_dist2(a2, b1, b2) <= tol2 ||
           point_to_segment_dist2(b1, a1, a2) <= tol2 || point_to_segment_dist2(b2, a1, a2) <= tol2;
}

static bool ring_edges_touch(const Polygon& pa, const Polygon& pb, coord_t tol)
{
    const Points &a = pa.points;
    const Points &b = pb.points;
    if (a.size() < 2 || b.size() < 2)
        return false;
    const double tol2 = double(tol) * double(tol);
    const size_t na   = a.size();
    const size_t nb   = b.size();

    if (na * nb <= 256) {
        for (size_t ia = 0; ia < na; ++ ia) {
            const Point &a1 = a[ia];
            const Point &a2 = a[(ia + 1 == na) ? 0 : ia + 1];
            for (size_t ib = 0; ib < nb; ++ ib) {
                const Point &b1 = b[ib];
                const Point &b2 = b[(ib + 1 == nb) ? 0 : ib + 1];
                if (segments_within_tol(a1, a2, b1, b2, tol, tol2))
                    return true;
            }
        }
    } else {
        const bool    index_b = nb <= na;
        const Points &idx_pts = index_b ? b : a;
        const Points &qry_pts = index_b ? a : b;
        const size_t  n_idx   = idx_pts.size();
        const size_t  n_qry   = qry_pts.size();
        std::vector<BBoxNode> nodes;
        nodes.reserve(n_idx);
        for (size_t i = 0; i < n_idx; ++ i) {
            const Point &u = idx_pts[i];
            const Point &v = idx_pts[(i + 1 == n_idx) ? 0 : i + 1];
            BoundingBox  eb(Point(std::min(u.x(), v.x()) - tol, std::min(u.y(), v.y()) - tol),
                            Point(std::max(u.x(), v.x()) + tol, std::max(u.y(), v.y()) + tol));
            nodes.emplace_back(i, eb);
        }
        AABBTreeBBoxes edge_tree;
        edge_tree.build_modify_input(nodes);
        bool hit = false;
        for (size_t iq = 0; iq < n_qry; ++ iq) {
            const Point &q1 = qry_pts[iq];
            const Point &q2 = qry_pts[(iq + 1 == n_qry) ? 0 : iq + 1];
            const AABBTreeBBoxes::BoundingBox qbox(Point(std::min(q1.x(), q2.x()), std::min(q1.y(), q2.y())),
                                                   Point(std::max(q1.x(), q2.x()), std::max(q1.y(), q2.y())));
            AABBTreeIndirect::traverse(
                edge_tree,
                AABBTreeIndirect::intersecting(qbox),
                [&](const AABBTreeBBoxes::Node &node) {
                    const size_t ei = node.idx;
                    const Point &p1 = idx_pts[ei];
                    const Point &p2 = idx_pts[(ei + 1 == n_idx) ? 0 : ei + 1];
                    if (segments_within_tol(q1, q2, p1, p2, tol, tol2)) {
                        hit = true;
                        return false;
                    }
                    return true;
                });
            if (hit)
                return true;
        }
    }
    return false;
}

static bool expolygons_touch_or_overlap(const ExPolygon &a, const ExPolygon &b)
{
    for (size_t ia = 0; ia < a.num_contours(); ++ ia)
        for (size_t ib = 0; ib < b.num_contours(); ++ ib)
            if (ring_edges_touch(a.contour_or_hole(ia), b.contour_or_hole(ib), 1))
                return true;
    // Disjoint rings can be nested. Test the filled region, including its holes.
    return (!a.contour.empty() && b.contains(a.contour.front())) || (!b.contour.empty() && a.contains(b.contour.front()));
}

// Exact point-in-polygon for doubled 64-bit coordinates (pt2 = 2*P tested against 2*path): 1 inside, 0 outside, -1 on boundary.
static int point_in_polygon_doubled(int64_t pt2x, int64_t pt2y, const Points &path)
{
    const size_t cnt = path.size();
    if (cnt < 3)
        return 0;
    int     result = 0;
    int64_t ipx    = int64_t(path[0].x()) * 2;
    int64_t ipy    = int64_t(path[0].y()) * 2;
    for (size_t i = 1; i <= cnt; ++ i) {
        const Point  &raw_next  = (i == cnt) ? path[0] : path[i];
        const int64_t ip_next_x = int64_t(raw_next.x()) * 2;
        const int64_t ip_next_y = int64_t(raw_next.y()) * 2;
        if (ip_next_y == pt2y &&
            (ip_next_x == pt2x || (ipy == pt2y && ((ip_next_x > pt2x) == (ipx < pt2x)))))
            return -1;
        if ((ipy < pt2y) != (ip_next_y < pt2y)) {
            if (ipx >= pt2x) {
                if (ip_next_x > pt2x)
                    result = 1 - result;
                else {
                    const int sign = Int128::sign_determinant_2x2(ipx - pt2x, ipy - pt2y, ip_next_x - pt2x, ip_next_y - pt2y);
                    if (! sign)
                        return -1;
                    if ((sign > 0) == (ip_next_y > ipy))
                        result = 1 - result;
                }
            } else if (ip_next_x > pt2x) {
                const int sign = Int128::sign_determinant_2x2(ipx - pt2x, ipy - pt2y, ip_next_x - pt2x, ip_next_y - pt2y);
                if (! sign)
                    return -1;
                if ((sign > 0) == (ip_next_y > ipy))
                    result = 1 - result;
            }
        }
        ipx = ip_next_x;
        ipy = ip_next_y;
    }
    return result;
}

static bool point_strictly_in_expolygon_doubled(int64_t pt2x, int64_t pt2y, const ExPolygon &ex)
{
    if (point_in_polygon_doubled(pt2x, pt2y, ex.contour.points) != 1)
        return false;
    for (const Polygon &hole : ex.holes)
        if (point_in_polygon_doubled(pt2x, pt2y, hole.points) != 0)
            return false;
    return true;
}

// Find which boundary expolygon contains the clipped seed path.
// Prefers strict interior containment (border_result = false) on interior vertices and segment midpoints so endpoints
// lying on shared boundary vertices or edges do not misattribute the seed to a touching neighbor boundary.
// Falls back to boundary-inclusive containment (border_result = true) when the entire path lies on a boundary edge.
static int find_boundary_for_path(
    const ClipperZUtils::ZPath     &path,
    const ExPolygons               &boundary,
    const std::vector<BoundingBox> &boundary_bboxes,
    const AABBTreeBBoxes           &boundary_aabb_tree,
    const std::vector<size_t>      &candidates,
    const std::vector<size_t>      &subbatch_stamp,
    size_t                          cur_subbatch_stamp,
    int                             hint_boundary_id)
{
    assert(path.size() >= 2);
    BoundingBox path_bbox;
    for (const ClipperZUtils::ZPoint &pt : path)
        path_bbox.merge(Point(pt.x(), pt.y()));
    const AABBTreeBBoxes::BoundingBox path_aabb(path_bbox.min, path_bbox.max);

    const bool use_hint = hint_boundary_id >= 0 &&
                          subbatch_stamp[size_t(hint_boundary_id)] == cur_subbatch_stamp &&
                          boundary_bboxes[size_t(hint_boundary_id)].defined;

    auto for_each_boundary_containing_pt = [&](const Point &pt, auto &&fn) {
        if (! boundary_aabb_tree.empty()) {
            AABBTreeIndirect::traverse(boundary_aabb_tree,
                [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                [&](const AABBTreeBBoxes::Node &node) {
                    assert(node.is_leaf());
                    assert(node.is_valid());
                    if (subbatch_stamp[node.idx] != cur_subbatch_stamp)
                        return true;
                    return fn(node.idx);
                });
        } else {
            for (size_t idx : candidates)
                if (boundary_bboxes[idx].defined && boundary_bboxes[idx].contains(pt))
                    if (! fn(idx))
                        break;
        }
    };

    auto find_strict = [&](const Point &pt) -> int {
        if (use_hint && boundary_bboxes[size_t(hint_boundary_id)].contains(pt) &&
            boundary[size_t(hint_boundary_id)].contains(pt, false))
            return hint_boundary_id;
        int found = -1;
        for_each_boundary_containing_pt(pt, [&](size_t idx) {
            if (int(idx) != hint_boundary_id && boundary[idx].contains(pt, false)) {
                found = int(idx);
                return false;
            }
            return true;
        });
        return found;
    };

    // Pass 1a: Interior vertices strictly inside a boundary.
    for (size_t i = 1; i + 1 < path.size(); ++ i)
        if (int id = find_strict(Point(path[i].x(), path[i].y())); id >= 0)
            return id;

    // Pass 1b: Segment midpoints strictly inside a boundary (using exact 64-bit doubled coordinates).
    for (size_t i = 0; i + 1 < path.size(); ++ i) {
        const Point p0(path[i].x(), path[i].y());
        const Point p1(path[i + 1].x(), path[i + 1].y());
        if (p0 == p1)
            continue;
        const int64_t mid2x = int64_t(p0.x()) + int64_t(p1.x());
        const int64_t mid2y = int64_t(p0.y()) + int64_t(p1.y());
        const Point   mid_probe(coord_t(mid2x / 2), coord_t(mid2y / 2));
        if (use_hint && boundary_bboxes[size_t(hint_boundary_id)].contains(mid_probe) &&
            point_strictly_in_expolygon_doubled(mid2x, mid2y, boundary[size_t(hint_boundary_id)]))
            return hint_boundary_id;
        int found = -1;
        for_each_boundary_containing_pt(mid_probe, [&](size_t idx) {
            if (int(idx) != hint_boundary_id && point_strictly_in_expolygon_doubled(mid2x, mid2y, boundary[idx])) {
                found = int(idx);
                return false;
            }
            return true;
        });
        if (found >= 0)
            return found;
    }

    // Pass 1c: Endpoint strictly inside a boundary (e.g. a closed path whose vertices lie strictly inside).
    if (int id = find_strict(Point(path.front().x(), path.front().y())); id >= 0)
        return id;

    // Pass 2: Entire path lies along a boundary border. Prefer a boundary that contains all vertices of the path
    // (checking hint_boundary_id first when multiple boundaries share the entire edge).
    auto contains_all_vertices = [&boundary, &path](size_t idx) {
        for (const ClipperZUtils::ZPoint &pt : path)
            if (! boundary[idx].contains(Point(pt.x(), pt.y()), true))
                return false;
        return true;
    };
    if (use_hint &&
        boundary_bboxes[size_t(hint_boundary_id)].contains(path_bbox) &&
        contains_all_vertices(size_t(hint_boundary_id)))
        return hint_boundary_id;

    {
        int found = -1;
        if (! boundary_aabb_tree.empty()) {
            AABBTreeIndirect::traverse(boundary_aabb_tree,
                [&path_aabb](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(path_aabb); },
                [&](const AABBTreeBBoxes::Node &node) {
                    assert(node.is_leaf());
                    assert(node.is_valid());
                    if (subbatch_stamp[node.idx] == cur_subbatch_stamp &&
                        int(node.idx) != hint_boundary_id &&
                        contains_all_vertices(node.idx)) {
                        found = int(node.idx);
                        return false;
                    }
                    return true;
                });
        } else {
            for (size_t idx : candidates)
                if (int(idx) != hint_boundary_id &&
                    boundary_bboxes[idx].defined &&
                    boundary_bboxes[idx].contains(path_bbox) &&
                    contains_all_vertices(idx)) {
                    found = int(idx);
                    break;
                }
        }
        if (found >= 0)
            return found;
    }

    // Pass 3: Fallback if rounding nudged a vertex off a slanted boundary edge.
    if (hint_boundary_id >= 0 && boundary_bboxes[size_t(hint_boundary_id)].defined) {
        for (const ClipperZUtils::ZPoint &pt : path)
            if (boundary_bboxes[size_t(hint_boundary_id)].contains(Point(pt.x(), pt.y())))
                return hint_boundary_id;
    }

    for (const ClipperZUtils::ZPoint &zpt : path) {
        const Point pt(zpt.x(), zpt.y());
        int         found = -1;
        for_each_boundary_containing_pt(pt, [&](size_t idx) {
            if (boundary[idx].contains(pt, true)) {
                found = int(idx);
                return false;
            }
            return true;
        });
        if (found >= 0)
            return found;
    }

    return -1;
}

std::vector<WaveSeed> wave_seeds(
    // Source regions that are supposed to touch the boundary.
    const ExPolygons      &src,
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons      &boundary,
    // Initial expansion of src to make the source regions intersect with boundary regions just a bit.
    float                  tiny_expansion,
    // Sort output by boundary ID and source ID.
    bool                   sorted)
{
    assert(tiny_expansion > 0);

    if (src.empty() || boundary.empty())
        return {};

    using Intersection  = ClipperZUtils::ClipperZIntersectionVisitor::Intersection;
    using Intersections = ClipperZUtils::ClipperZIntersectionVisitor::Intersections;

    // Z coordinate layout:
    //   [1, idx_boundary_end)        : boundary expolygon index + 1
    //   [idx_boundary_end, idx_src_end) : source expolygon index + idx_boundary_end
    //   < 0                          : -(index into intersections + 1)
    const coord_t idx_boundary_begin = 1;
    const coord_t idx_boundary_end   = idx_boundary_begin + coord_t(boundary.size());
    const coord_t idx_src_end        = idx_boundary_end + coord_t(src.size());

    // Expand each source independently and compute its inflated bounding box & OBB so disjoint angled sources can share a batch.
    std::vector<Polygons>              src_expanded_polys(src.size());
    std::vector<ClipperZUtils::ZPaths> zsrc(src.size());
    std::vector<BoundingBox>           src_bboxes(src.size());
    std::vector<PolyOBB>               src_obbs(src.size());
    std::vector<size_t>                non_empty_srcs;
    non_empty_srcs.reserve(src.size());
    for (size_t src_id = 0; src_id < src.size(); ++ src_id) {
        expolygon_to_zpaths_expanded_opened(
            src[src_id], tiny_expansion, idx_boundary_end + coord_t(src_id),
            src_expanded_polys[src_id], zsrc[src_id]);
        if (zsrc[src_id].empty())
            continue;
        BoundingBox bb;
        for (const ClipperZUtils::ZPath &path : zsrc[src_id])
            for (const ClipperZUtils::ZPoint &pt : path)
                bb.merge(Point(pt.x(), pt.y()));
        // Inflate so clipped vertices stay inside their source's box and adjacent sources within epsilon separate into batches.
        bb.offset(SCALED_EPSILON);
        src_bboxes[src_id] = bb;
        src_obbs[src_id]   = compute_poly_obb_from_polys(src_expanded_polys[src_id], double(SCALED_EPSILON) + 4.0);
        non_empty_srcs.push_back(src_id);
    }

    if (non_empty_srcs.empty())
        return {};

    auto sources_conflict = [&](size_t a, size_t b) -> bool {
        if (obb_separated(src_obbs[a], src_obbs[b]))
            return false;
        for (const Polygon &pa : src_expanded_polys[a])
            for (const Polygon &pb : src_expanded_polys[b])
                if (ring_edges_touch(pa, pb, SCALED_EPSILON) || (pa.size() >= 3 && contains(pa, pb.front(), true)) ||
                    (pb.size() >= 3 && contains(pb, pa.front(), true)))
                    return true;
        return false;
    };

    // Partition sources into batches whose expanded regions are geometrically disjoint (using AABB tree + OBB + exact edge check).
    std::vector<std::vector<size_t>> batches;
    std::vector<size_t>              batch_of(src.size(), size_t(-1));
    AABBTreeBBoxes                   src_aabb_tree;
    if (non_empty_srcs.size() == 1) {
        batch_of[non_empty_srcs.front()] = 0;
        batches.push_back({ non_empty_srcs.front() });
    } else {
        std::vector<BBoxNode> src_nodes;
        src_nodes.reserve(non_empty_srcs.size());
        for (size_t src_id : non_empty_srcs)
            src_nodes.emplace_back(src_id, src_bboxes[src_id]);
        src_aabb_tree.build_modify_input(src_nodes);

        std::vector<size_t> forbidden_stamp;
        size_t              stamp = 0;
        for (size_t src_id : non_empty_srcs) {
            ++ stamp;
            const AABBTreeBBoxes::BoundingBox query(src_bboxes[src_id].min, src_bboxes[src_id].max);
            AABBTreeIndirect::traverse(src_aabb_tree,
                [&query](const AABBTreeBBoxes::Node &node) { return node.bbox.intersects(query); },
                [&](const AABBTreeBBoxes::Node &node) {
                    const size_t b = batch_of[node.idx];
                    if (b != size_t(-1) && forbidden_stamp[b] != stamp && sources_conflict(src_id, node.idx))
                        forbidden_stamp[b] = stamp;
                    return true;
                });
            size_t chosen = 0;
            while (chosen < forbidden_stamp.size() && forbidden_stamp[chosen] == stamp)
                ++ chosen;
            if (chosen == batches.size()) {
                batches.emplace_back();
                forbidden_stamp.push_back(0);
            }
            batch_of[src_id] = chosen;
            batches[chosen].push_back(src_id);
        }
    }

    // Cache inflated bounding boxes and OBBs of boundaries (and build an AABB tree when there are many boundaries).
    std::vector<BoundingBox> boundary_bboxes;
    std::vector<PolyOBB>     boundary_obbs;
    boundary_bboxes.reserve(boundary.size());
    boundary_obbs.reserve(boundary.size());
    for (const ExPolygon &ex : boundary) {
        BoundingBox bb = get_extents(ex.contour);
        if (bb.defined)
            bb.offset(SCALED_EPSILON);
        boundary_bboxes.emplace_back(bb);
        boundary_obbs.emplace_back(compute_poly_obb_from_polys(std::array<Polygon, 1>{ ex.contour }, 2.0));
    }

    AABBTreeBBoxes boundary_aabb_tree;
    if (boundary.size() >= boundaries_per_tree) {
        std::vector<BBoxNode> nodes;
        nodes.reserve(boundary.size());
        for (size_t i = 0; i < boundary.size(); ++ i)
            if (boundary_bboxes[i].defined)
                nodes.emplace_back(i, boundary_bboxes[i]);
        boundary_aabb_tree.build_modify_input(nodes);
    }

    // Keep the scanline local when a large batch covers several disjoint rows or columns.
    if (batches.size() == 1 && non_empty_srcs.size() >= boundaries_per_tree && !boundary_aabb_tree.empty()) {
        auto groups = split_at_layer_gaps(non_empty_srcs, src_bboxes, boundary_bboxes);
        if (!groups.empty()) {
            batches = std::move(groups);
            for (size_t i = 0; i < batches.size(); ++i)
                for (size_t s : batches[i])
                    batch_of[s] = i;
        }
    }

    auto collect_boundary_candidates = [&](const BoundingBox& bbox, size_t batch_idx, std::vector<size_t>& candidates) {
        const auto& batch = batches[batch_idx];
        candidates.clear();
        auto overlaps_any_src_obb = [&](size_t bnd_idx) -> bool {
            const BoundingBox& bounds = boundary_bboxes[bnd_idx];
            if (batch.size() == 1) {
                const size_t src_id = batch.front();
                return bounds.overlap(src_bboxes[src_id]) && !obb_separated(boundary_obbs[bnd_idx], src_obbs[src_id]);
            }
            const AABBTreeBBoxes::BoundingBox query(bounds.min, bounds.max);
            bool                              hit = false;
            AABBTreeIndirect::traverse(src_aabb_tree, AABBTreeIndirect::intersecting(query), [&](const AABBTreeBBoxes::Node& node) {
                hit = batch_of[node.idx] == batch_idx && !obb_separated(boundary_obbs[bnd_idx], src_obbs[node.idx]);
                return !hit;
            });
            return hit;
        };
        if (! boundary_aabb_tree.empty()) {
            const AABBTreeBBoxes::BoundingBox query(bbox.min, bbox.max);
            AABBTreeIndirect::traverse(boundary_aabb_tree,
                [&query](const AABBTreeBBoxes::Node &node) { return node.bbox.intersects(query); },
                [&](const AABBTreeBBoxes::Node &node) {
                    if (overlaps_any_src_obb(node.idx))
                        candidates.push_back(node.idx);
                    return true;
                });
            std::sort(candidates.begin(), candidates.end());
        } else {
            for (size_t i = 0; i < boundary.size(); ++ i)
                if (boundary_bboxes[i].defined && boundary_bboxes[i].overlap(bbox) && overlaps_any_src_obb(i))
                    candidates.push_back(i);
        }
    };

    auto point_near_or_inside_src = [&](size_t src_id, const Point &pt) -> bool {
        const double tol2 = double(SCALED_EPSILON) * double(SCALED_EPSILON);
        int          parity = 0;
        for (const Polygon &poly : src_expanded_polys[src_id]) {
            const Points &pts = poly.points;
            const size_t  n   = pts.size();
            for (size_t i = 0; i < n; ++ i) {
                if (point_to_segment_dist2(pt, pts[i], pts[(i + 1 == n) ? 0 : i + 1]) <= tol2)
                    return true;
            }
            if (poly.contains(pt))
                parity ^= 1;
        }
        return parity != 0;
    };

    auto find_src_for_path = [&](const ClipperZUtils::ZPath &path, size_t batch_idx) -> int {
        const std::vector<size_t> &batch = batches[batch_idx];
        if (batch.size() == 1)
            return int(batch.front());
        auto lookup_pt = [&](const Point &pt) -> int {
            int found = -1;
            AABBTreeIndirect::traverse(src_aabb_tree,
                [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                [&](const AABBTreeBBoxes::Node &node) {
                    assert(node.is_leaf());
                    assert(node.is_valid());
                    if (batch_of[node.idx] == batch_idx && point_near_or_inside_src(node.idx, pt)) {
                        found = int(node.idx);
                        return false;
                    }
                    return true;
                });
            return found;
        };
        if (int id = lookup_pt(Point(path.front().x(), path.front().y())); id >= 0)
            return id;
        if (int id = lookup_pt(Point(path.back().x(), path.back().y())); id >= 0)
            return id;
        for (size_t i = 1; i + 1 < path.size(); ++ i)
            if (int id = lookup_pt(Point(path[i].x(), path[i].y())); id >= 0)
                return id;
        return -1;
    };

    // Lazily populated only if a batch actually needs all boundaries; avoids building an unused full zboundary.
    ClipperZUtils::ZPaths                      zboundary_all;
    Intersections                              intersections;
    ClipperZUtils::ClipperZIntersectionVisitor visitor(intersections);
    std::vector<size_t>                        candidates;
    std::vector<size_t>                        cand_subbatch(boundary.size(), size_t(-1));
    std::vector<size_t>                        subbatch_stamp(boundary.size(), 0);
    size_t                                     cur_subbatch_stamp = 0;

    // Sort paths into their respective islands.
    // Each src x boundary will be processed (wave expanded) independently.
    // Multiple pieces of a single src may intersect the same boundary.
    WaveSeeds out;
    for (size_t batch_idx = 0; batch_idx < batches.size(); ++ batch_idx) {
        const std::vector<size_t> &batch = batches[batch_idx];
        BoundingBox                bbox_batch;
        size_t                     num_src_paths = 0;
        for (size_t src_id : batch) {
            bbox_batch.merge(src_bboxes[src_id]);
            num_src_paths += zsrc[src_id].size();
        }
        collect_boundary_candidates(bbox_batch, batch_idx, candidates);
        if (candidates.empty())
            continue;

        // Open source contours and their split seam points for this batch.
        ClipperZUtils::ZPaths                              zsrc_batch;
        std::vector<std::pair<ClipperZUtils::ZPoint, int>> zsrc_splits_base;
        zsrc_batch.reserve(num_src_paths);
        zsrc_splits_base.reserve(num_src_paths);
        for (size_t src_id : batch) {
            for (const ClipperZUtils::ZPath &path : zsrc[src_id]) {
                assert(path.size() >= 2);
                assert(path.front() == path.back());
                zsrc_splits_base.emplace_back(path.front(), -1);
            }
            append(zsrc_batch, std::move(zsrc[src_id]));
        }
        std::sort(zsrc_splits_base.begin(), zsrc_splits_base.end(),
            [](const auto &l, const auto &r) { return ClipperZUtils::zpoint_lower(l.first, r.first); });

        // Partition candidates that touch or overlap at vertices/edges into separate sub-batches
        // so Clipper2 horizontal scans never truncate an open seed at a shared boundary vertex.
        std::vector<std::vector<size_t>> bnd_subbatches;
        if (candidates.size() <= 1) {
            bnd_subbatches.push_back(candidates);
        } else {
            std::vector<size_t> forbidden_bnd;
            size_t              bnd_stamp = 0;
            for (size_t ci = 0; ci < candidates.size(); ++ ci) {
                const size_t c = candidates[ci];
                ++ bnd_stamp;
                auto check_neighbor = [&](size_t j) {
                    const size_t sb = cand_subbatch[j];
                    if (sb == size_t(-1) || forbidden_bnd[sb] == bnd_stamp)
                        return;
                    if (! obb_separated(boundary_obbs[c], boundary_obbs[j]) &&
                        expolygons_touch_or_overlap(boundary[c], boundary[j]))
                        forbidden_bnd[sb] = bnd_stamp;
                };
                if (! boundary_aabb_tree.empty()) {
                    const AABBTreeBBoxes::BoundingBox qbox(boundary_bboxes[c].min, boundary_bboxes[c].max);
                    AABBTreeIndirect::traverse(boundary_aabb_tree,
                        [&qbox](const AABBTreeBBoxes::Node &node) { return node.bbox.intersects(qbox); },
                        [&](const AABBTreeBBoxes::Node &node) {
                            check_neighbor(node.idx);
                            return true;
                        });
                } else {
                    for (size_t cj = 0; cj < ci; ++ cj)
                        if (boundary_bboxes[c].overlap(boundary_bboxes[candidates[cj]]))
                            check_neighbor(candidates[cj]);
                }
                size_t chosen = 0;
                while (chosen < forbidden_bnd.size() && forbidden_bnd[chosen] == bnd_stamp)
                    ++ chosen;
                if (chosen == bnd_subbatches.size()) {
                    bnd_subbatches.emplace_back();
                    forbidden_bnd.push_back(0);
                }
                cand_subbatch[c] = chosen;
                bnd_subbatches[chosen].push_back(c);
            }
            for (size_t c : candidates)
                cand_subbatch[c] = size_t(-1);
        }

        auto valid_intersection = [&intersections, idx_boundary_begin, idx_boundary_end, idx_src_end](coord_t z) -> const Intersection* {
            if (z >= 0)
                return nullptr;
            const size_t idx = size_t(-int64_t(z) - 1);
            if (idx >= intersections.size())
                return nullptr;
            const Intersection &is = intersections[idx];
            if (is.first >= idx_boundary_begin && is.first < idx_boundary_end &&
                is.second >= idx_boundary_end && is.second < idx_src_end)
                return &is;
            return nullptr;
        };

        auto count_boundary_bboxes_containing = [&](const Point &pt, const std::vector<size_t> &subbatch,
                                                    const std::vector<size_t> &subbatch_stamp, size_t cur_stamp) -> size_t {
            size_t count = 0;
            if (subbatch.size() >= boundaries_per_tree && ! boundary_aabb_tree.empty()) {
                AABBTreeIndirect::traverse(boundary_aabb_tree,
                    [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                    [&](const AABBTreeBBoxes::Node &node) {
                        if (subbatch_stamp[node.idx] == cur_stamp)
                            ++ count;
                        return count < 2;
                    });
            } else {
                for (size_t idx : subbatch)
                    if (boundary_bboxes[idx].defined && boundary_bboxes[idx].contains(pt))
                        if (++ count >= 2)
                            break;
            }
            return count;
        };

        std::vector<std::pair<ClipperZUtils::ZPoint, int>> zsrc_splits;
        for (const std::vector<size_t> &subbatch : bnd_subbatches) {
            ++ cur_subbatch_stamp;
            for (size_t idx : subbatch)
                subbatch_stamp[idx] = cur_subbatch_stamp;

            const ClipperZUtils::ZPaths *pzclip = nullptr;
            ClipperZUtils::ZPaths        zclip;
            if (subbatch.size() == boundary.size()) {
                if (zboundary_all.empty()) {
                    coord_t tmp_idx = idx_boundary_begin;
                    zboundary_all   = ClipperZUtils::expolygons_to_zpaths(boundary, tmp_idx);
                }
                pzclip = &zboundary_all;
            } else {
                size_t num_clip_paths = 0;
                for (size_t idx : subbatch)
                    num_clip_paths += boundary[idx].num_contours();
                zclip.reserve(num_clip_paths);
                for (size_t idx : subbatch) {
                    const coord_t z = coord_t(idx) + idx_boundary_begin;
                    zclip.emplace_back(ClipperZUtils::to_zpath(boundary[idx].contour.points, z));
                    for (const Polygon &hole : boundary[idx].holes)
                        zclip.emplace_back(ClipperZUtils::to_zpath(hole.points, z));
                }
                pzclip = &zclip;
            }

            visitor.reset();
            ClipperZUtils::ZPaths segments = ClipperZUtils::clip_zpaths(ctIntersection, zsrc_batch, true, *pzclip, visitor.clipper_callback());
            if (bnd_subbatches.size() == 1) {
                merge_splits(segments, zsrc_splits_base);
            } else {
                zsrc_splits = zsrc_splits_base;
                merge_splits(segments, zsrc_splits);
            }

            reserve_more_power_of_2(out, segments.size());
            for (const ClipperZUtils::ZPath &path : segments) {
                assert(path.size() >= 2);
                const ClipperZUtils::ZPoint &front = path.front();
                const ClipperZUtils::ZPoint &back  = path.back();

                const Intersection *front_is = valid_intersection(front.z());
                const Intersection *back_is  = valid_intersection(back.z());

                // 1. Determine src_id (from valid intersection, any source vertex Z, or the unique source in this batch).
                int src_id = -1;
                if (front_is)
                    src_id = int(front_is->second - idx_boundary_end);
                else if (back_is)
                    src_id = int(back_is->second - idx_boundary_end);
                else {
                    for (const ClipperZUtils::ZPoint &point : path)
                        if (point.z() >= idx_boundary_end && point.z() < idx_src_end) {
                            src_id = int(point.z() - idx_boundary_end);
                            break;
                        }
                    if (src_id < 0)
                        src_id = find_src_for_path(path, batch_idx);
                }
                assert(src_id >= 0 && src_id < int(src.size()));
                if (src_id < 0)
                    continue;

                // 2. Determine boundary_id (directly when unambiguous, else via strict-interior-first geometric lookup).
                int boundary_id = -1;
                if (subbatch.size() == 1) {
                    boundary_id = int(subbatch.front());
                } else {
                    int hint_boundary_id = -1;
                    if (front_is && back_is && front_is->first == back_is->first) {
                        const int candidate_b = int(front_is->first - idx_boundary_begin);
                        if (count_boundary_bboxes_containing(Point(front.x(), front.y()), subbatch, subbatch_stamp, cur_subbatch_stamp) <= 1 ||
                            count_boundary_bboxes_containing(Point(back.x(), back.y()), subbatch, subbatch_stamp, cur_subbatch_stamp) <= 1)
                            boundary_id = candidate_b;
                        else
                            hint_boundary_id = candidate_b;
                    } else if (front_is && ! back_is) {
                        const int candidate_b = int(front_is->first - idx_boundary_begin);
                        if (count_boundary_bboxes_containing(Point(front.x(), front.y()), subbatch, subbatch_stamp, cur_subbatch_stamp) <= 1)
                            boundary_id = candidate_b;
                        else
                            hint_boundary_id = candidate_b;
                    } else if (back_is && ! front_is) {
                        const int candidate_b = int(back_is->first - idx_boundary_begin);
                        if (count_boundary_bboxes_containing(Point(back.x(), back.y()), subbatch, subbatch_stamp, cur_subbatch_stamp) <= 1)
                            boundary_id = candidate_b;
                        else
                            hint_boundary_id = candidate_b;
                    } else {
                        if (front_is)
                            hint_boundary_id = int(front_is->first - idx_boundary_begin);
                        else if (back_is)
                            hint_boundary_id = int(back_is->first - idx_boundary_begin);
                        else {
                            for (const ClipperZUtils::ZPoint &point : path)
                                if (point.z() >= idx_boundary_begin && point.z() < idx_boundary_end) {
                                    hint_boundary_id = int(point.z() - idx_boundary_begin);
                                    break;
                                }
                        }
                    }

                    if (boundary_id < 0)
                        boundary_id = find_boundary_for_path(
                            path, boundary, boundary_bboxes, boundary_aabb_tree,
                            subbatch, subbatch_stamp, cur_subbatch_stamp, hint_boundary_id);
                }

                assert(boundary_id >= 0 && boundary_id < int(boundary.size()));
                if (boundary_id >= 0)
                    out.push_back({ uint32_t(src_id), uint32_t(boundary_id), ClipperZUtils::from_zpath(path) });
            }
        }
    }

    if (sorted)
        // Sort the seeds deterministically by their intersection boundary and source contour.
        std::stable_sort(out.begin(), out.end(), lower_by_boundary_and_src);
    return out;
}

static Polygons wavefront_initial(const VecOfPoints &polylines, float offset, double arc_tolerance)
{
    Polygons out;
    out.reserve(polylines.size());
    for (const Points &path : polylines) {
        assert(path.size() >= 2);
        append(out, Slic3r::offset(Polyline(path), offset, jtRound, arc_tolerance, path.front() == path.back() ? etClosedLine : etOpenRound));
    }
    return out;
}

// Input polygons may consist of multiple expolygons, even nested expolygons.
// After inflation some polygons may thus overlap, however the overlap is being resolved during the successive
// clipping operation, thus it is not being done here.
static Polygons wavefront_step(const Polygons &polygons, float offset, double arc_tolerance)
{
    Polygons out;
    out.reserve(polygons.size());
    for (const Polygon &polygon : polygons)
        // CCW contours grow, CW holes shrink.
        append(out, Slic3r::offset(polygon, offset, jtRound, arc_tolerance));
    return out;
}

static Polygons wavefront_clip(const Polygons &wavefront, const Polygons &clipping)
{
    return intersection(wavefront, clipping, pftPositive);
}

static Polygons propagate_wave_from_boundary(
    // Seed of the wave: Open polylines very close to the boundary.
    const VecOfPoints           &seed,
    // Boundary inside which the waveform will propagate.
    const ExPolygon             &boundary,
    // How much to inflate the seed lines to produce the first wave area.
    const float                  initial_step,
    // How much to inflate the first wave area and the successive wave areas in each step.
    const float                  other_step,
    // Number of inflate steps after the initial step.
    const size_t                 num_other_steps,
    // Maximum inflation of seed contours over the boundary. Used to trim boundary to speed up
    // clipping during wave propagation.
    const float                  max_inflation,
    // Accuracy of the round offsets.
    const double                 arc_tolerance)
{
    assert(! seed.empty() && seed.front().size() >= 2);
    Polygons clipping = ClipperUtils::clip_clipper_polygons_with_subject_bbox(boundary, get_extents<true>(seed).inflated(max_inflation));
    Polygons polygons = wavefront_clip(wavefront_initial(seed, initial_step, arc_tolerance), clipping);
    // Now offset the remaining 
    for (size_t ioffset = 0; ioffset < num_other_steps; ++ ioffset)
        polygons = wavefront_clip(wavefront_step(polygons, other_step, arc_tolerance), clipping);
    return polygons;
}

// Resulting regions are sorted by boundary id and source id.
std::vector<RegionExpansion> propagate_waves(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    std::vector<RegionExpansion> out;
    VecOfPoints                  paths;
    for (auto it_seed = seeds.begin(); it_seed != seeds.end();) {
        auto it = it_seed;
        paths.clear();
        for (; it != seeds.end() && it->boundary == it_seed->boundary && it->src == it_seed->src; ++ it)
            paths.emplace_back(it->path);
        // Propagate the wavefront while clipping it with the trimmed boundary.
        // Collect the expanded polygons, merge them with the source polygons.
        RegionExpansion re;
        for (Polygon &polygon : propagate_wave_from_boundary(paths, boundary[it_seed->boundary], params.initial_step, params.other_step, params.num_other_steps, params.max_inflation, params.arc_tolerance))
            out.push_back({ std::move(polygon), it_seed->src, it_seed->boundary });
        it_seed = it;
    }

    return out;
}

std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    return propagate_waves(wave_seeds(src, boundary, params.tiny_expansion, true), boundary, params);
}

std::vector<RegionExpansion> propagate_waves(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps)
{
    return propagate_waves(src, boundary, RegionExpansionParameters::build(expansion, expansion_step, max_nr_steps));
}

// Returns regions per source ExPolygon expanded into boundary.
std::vector<RegionExpansionEx> propagate_waves_ex(const WaveSeeds &seeds, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    std::vector<RegionExpansion> expanded = propagate_waves(seeds, boundary, params);
    assert(std::is_sorted(seeds.begin(), seeds.end(), [](const auto &l, const auto &r){ return l.boundary < r.boundary || (l.boundary == r.boundary && l.src < r.src); }));
    Polygons acc;
    std::vector<RegionExpansionEx> out;
    for (auto it = expanded.begin(); it != expanded.end(); ) {
        auto it2 = it;
        acc.clear();
        for (; it2 != expanded.end() && it2->boundary_id == it->boundary_id && it2->src_id == it->src_id; ++ it2)
            acc.emplace_back(std::move(it2->polygon));
        size_t size = it2 - it;
        if (size == 1)
            out.push_back({ ExPolygon{std::move(acc.front())}, it->src_id, it->boundary_id });
        else {
            ExPolygons expolys = union_ex(acc);
            reserve_more_power_of_2(out, expolys.size());
            for (ExPolygon &ex : expolys)
                out.push_back({ std::move(ex), it->src_id, it->boundary_id });
        }
        it = it2;
    }
    return out;
}

// Returns regions per source ExPolygon expanded into boundary.
std::vector<RegionExpansionEx> propagate_waves_ex(
    // Source regions that are supposed to touch the boundary.
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons    &src,
    const ExPolygons    &boundary,
    // Scaled expansion value
    float                full_expansion,
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float                expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t               max_nr_expansion_steps)
{
    auto params = RegionExpansionParameters::build(full_expansion, expansion_step, max_nr_expansion_steps);
    return propagate_waves_ex(wave_seeds(src, boundary, params.tiny_expansion, true), boundary, params);
}

std::vector<Polygons> expand_expolygons(const ExPolygons &src, const ExPolygons &boundary,
    // Scaled expansion value
    float expansion, 
    // Expand by waves of expansion_step size (expansion_step is scaled).
    float expansion_step,
    // Don't take more than max_nr_steps for small expansion_step.
    size_t max_nr_steps)
{
    std::vector<Polygons> out(src.size(), Polygons{});
    for (RegionExpansion &r : propagate_waves(src, boundary, expansion, expansion_step, max_nr_steps))
        out[r.src_id].emplace_back(std::move(r.polygon));
    return out;
}

std::vector<ExPolygon> merge_expansions_into_expolygons(ExPolygons &&src, std::vector<RegionExpansion> &&expanded)
{
    // expanded regions will be merged into source regions, thus they will be re-sorted by source id.
    std::stable_sort(expanded.begin(), expanded.end(), [](const auto &l, const auto &r) { return l.src_id < r.src_id; });
    uint32_t   last = 0;
    Polygons   acc;
    ExPolygons out;
    out.reserve(src.size());
    for (auto it = expanded.begin(); it != expanded.end();) {
        for (; last < it->src_id; ++ last)
            out.emplace_back(std::move(src[last]));
        acc.clear();
        assert(it->src_id == last);
        for (; it != expanded.end() && it->src_id == last; ++ it)
            acc.emplace_back(std::move(it->polygon));
        //FIXME offset & merging could be more efficient, for example one does not need to copy the source expolygon
        ExPolygon &src_ex = src[last ++];
        assert(! src_ex.contour.empty());
#if 0
        {
            static int iRun = 0;
            BoundingBox bbox = get_extents(acc);
            bbox.merge(get_extents(src_ex));
            SVG svg(debug_out_path("expand_merge_expolygons-failed-union=%d.svg", iRun ++).c_str(), bbox);
            svg.draw(acc);
            svg.draw_outline(acc, "black", scale_(0.05));
            svg.draw(src_ex, "red");
            svg.Close();
        }
#endif
        Point sample = src_ex.contour.front();
        append(acc, to_polygons(std::move(src_ex)));
        ExPolygons merged = union_safety_offset_ex(acc);
        // Expanding one expolygon by waves should not change connectivity of the source expolygon:
        // Single expolygon should be produced possibly with increased number of holes.
        if (merged.size() > 1) {
            // assert(merged.size() == 1);
            // There is something wrong with the initial waves. Most likely the bridge was not valid at all
            // or the boundary region was very close to some bridge edge, but not really touching.
            // Pick only a single merged expolygon, which contains one sample point of the source expolygon.
            auto aabb_tree = build_aabb_tree_over_expolygons(merged);
            int id = sample_in_expolygons(aabb_tree, merged, sample);
            assert(id != -1);
            if (id != -1)
                out.emplace_back(std::move(merged[id]));
        } else if (merged.size() == 1)
            out.emplace_back(std::move(merged.front()));
    }
    for (; last < uint32_t(src.size()); ++ last)
        out.emplace_back(std::move(src[last]));
    return out;
}

std::vector<ExPolygon> expand_merge_expolygons(ExPolygons &&src, const ExPolygons &boundary, const RegionExpansionParameters &params)
{
    // expanded regions are sorted by boundary id and source id
    std::vector<RegionExpansion> expanded = propagate_waves(src, boundary, params);
    return merge_expansions_into_expolygons(std::move(src), std::move(expanded));
}

} // Algorithm
} // Slic3r
