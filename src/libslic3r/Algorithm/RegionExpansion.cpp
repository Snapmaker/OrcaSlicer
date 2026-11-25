#include "RegionExpansion.hpp"

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/ClipperZUtils.hpp>
#include <libslic3r/ClipperUtils.hpp>
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
static ClipperZUtils::ZPaths expolygon_to_zpaths_expanded_opened(
    const ExPolygon &expoly, const float expansion, const coord_t z)
{
    ClipperZUtils::ZPaths out;
    out.reserve(expoly.num_contours());
    for (size_t icontour = 0; icontour < expoly.num_contours(); ++ icontour) {
        // Orient CCW, then grow the contour and shrink the holes. The output contours are CCW.
        Polygon contour = expoly.contour_or_hole(icontour);
        if (! contour.is_counter_clockwise())
            contour.reverse();
        for (const Polygon &expanded : offset(contour, icontour == 0 ? expansion : -expansion, jtSquare))
            if (expanded.points.size() >= 2)
                out.emplace_back(ClipperZUtils::to_zpath<true>(expanded.points, z));
    }
    return out;
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
    int                             hint_boundary_id)
{
    assert(path.size() >= 2);
    BoundingBox path_bbox;
    for (const ClipperZUtils::ZPoint &pt : path)
        path_bbox.merge(Point(pt.x(), pt.y()));
    const AABBTreeBBoxes::BoundingBox path_aabb(path_bbox.min, path_bbox.max);

    auto for_each_boundary_containing_pt = [&](const Point &pt, auto &&fn) {
        if (! boundary_aabb_tree.empty()) {
            AABBTreeIndirect::traverse(boundary_aabb_tree,
                [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                [&fn](const AABBTreeBBoxes::Node &node) {
                    assert(node.is_leaf());
                    assert(node.is_valid());
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
        int found = -1;
        for_each_boundary_containing_pt(pt, [&](size_t idx) {
            if (boundary[idx].contains(pt, false)) {
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

    // Pass 1b: Segment midpoints strictly inside a boundary.
    for (size_t i = 0; i + 1 < path.size(); ++ i) {
        const Point p0(path[i].x(), path[i].y());
        const Point p1(path[i + 1].x(), path[i + 1].y());
        if (p0 == p1)
            continue;
        const Point mid(coord_t((int64_t(p0.x()) + int64_t(p1.x())) / 2),
                        coord_t((int64_t(p0.y()) + int64_t(p1.y())) / 2));
        if (int id = find_strict(mid); id >= 0)
            return id;
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
    if (hint_boundary_id >= 0 &&
        boundary_bboxes[size_t(hint_boundary_id)].defined &&
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
                    if (int(node.idx) != hint_boundary_id && contains_all_vertices(node.idx)) {
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

    // Expand each source independently and compute its inflated bounding box so overlapping sources can be batched apart.
    std::vector<ClipperZUtils::ZPaths> zsrc(src.size());
    std::vector<BoundingBox>           src_bboxes(src.size());
    std::vector<size_t>                non_empty_srcs;
    non_empty_srcs.reserve(src.size());
    for (size_t src_id = 0; src_id < src.size(); ++ src_id) {
        zsrc[src_id] = expolygon_to_zpaths_expanded_opened(src[src_id], tiny_expansion, idx_boundary_end + coord_t(src_id));
        if (zsrc[src_id].empty())
            continue;
        BoundingBox bb;
        for (const ClipperZUtils::ZPath &path : zsrc[src_id])
            for (const ClipperZUtils::ZPoint &pt : path)
                bb.merge(Point(pt.x(), pt.y()));
        // Inflate so clipped vertices stay inside their source's box and adjacent sources within epsilon separate into batches.
        bb.offset(SCALED_EPSILON);
        src_bboxes[src_id] = bb;
        non_empty_srcs.push_back(src_id);
    }

    if (non_empty_srcs.empty())
        return {};

    // Partition sources into batches whose inflated bounding boxes are mutually disjoint using an AABB tree (O(N log N + E)).
    // Within each batch, at most one source's box can contain any clipped segment, preserving exact source attribution even
    // when all vertices of a clipped segment coincide with boundary vertices and carry no source Z coordinate.
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
                [&batch_of, &forbidden_stamp, stamp](const AABBTreeBBoxes::Node &node) {
                    const size_t b = batch_of[node.idx];
                    if (b != size_t(-1))
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

    // Cache inflated bounding boxes of boundaries (and build an AABB tree when there are many boundaries).
    std::vector<BoundingBox> boundary_bboxes;
    boundary_bboxes.reserve(boundary.size());
    for (const ExPolygon &ex : boundary) {
        BoundingBox bb = get_extents(ex.contour);
        if (bb.defined)
            bb.offset(SCALED_EPSILON);
        boundary_bboxes.emplace_back(bb);
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

    auto collect_boundary_candidates = [&](const BoundingBox &bbox, std::vector<size_t> &candidates) {
        candidates.clear();
        if (! boundary_aabb_tree.empty()) {
            const AABBTreeBBoxes::BoundingBox query(bbox.min, bbox.max);
            AABBTreeIndirect::traverse(boundary_aabb_tree,
                [&query](const AABBTreeBBoxes::Node &node) { return node.bbox.intersects(query); },
                [&candidates](const AABBTreeBBoxes::Node &node) {
                    candidates.push_back(node.idx);
                    return true;
                });
            std::sort(candidates.begin(), candidates.end());
        } else {
            for (size_t i = 0; i < boundary.size(); ++ i)
                if (boundary_bboxes[i].defined && boundary_bboxes[i].overlap(bbox))
                    candidates.push_back(i);
        }
    };

    auto find_src_for_path = [&](const ClipperZUtils::ZPath &path, size_t batch_idx) -> int {
        const std::vector<size_t> &batch = batches[batch_idx];
        if (batch.size() == 1)
            return int(batch.front());
        auto lookup_pt = [&](const Point &pt) -> int {
            int found = -1;
            AABBTreeIndirect::traverse(src_aabb_tree,
                [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                [&batch_of, batch_idx, &found](const AABBTreeBBoxes::Node &node) {
                    assert(node.is_leaf());
                    assert(node.is_valid());
                    if (batch_of[node.idx] == batch_idx) {
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
        collect_boundary_candidates(bbox_batch, candidates);
        if (candidates.empty())
            continue;

        // Open source contours and their split seam points for this batch.
        ClipperZUtils::ZPaths                              zsrc_batch;
        std::vector<std::pair<ClipperZUtils::ZPoint, int>> zsrc_splits;
        zsrc_batch.reserve(num_src_paths);
        zsrc_splits.reserve(num_src_paths);
        for (size_t src_id : batch) {
            for (const ClipperZUtils::ZPath &path : zsrc[src_id]) {
                assert(path.size() >= 2);
                assert(path.front() == path.back());
                zsrc_splits.emplace_back(path.front(), -1);
            }
            append(zsrc_batch, std::move(zsrc[src_id]));
        }
        std::sort(zsrc_splits.begin(), zsrc_splits.end(),
            [](const auto &l, const auto &r) { return ClipperZUtils::zpoint_lower(l.first, r.first); });

        // Closed boundary contours for the candidate boundaries reached by this batch.
        const ClipperZUtils::ZPaths *pzclip = nullptr;
        ClipperZUtils::ZPaths        zclip;
        if (candidates.size() == boundary.size()) {
            if (zboundary_all.empty()) {
                coord_t tmp_idx = idx_boundary_begin;
                zboundary_all   = ClipperZUtils::expolygons_to_zpaths(boundary, tmp_idx);
            }
            pzclip = &zboundary_all;
        } else {
            size_t num_clip_paths = 0;
            for (size_t idx : candidates)
                num_clip_paths += boundary[idx].num_contours();
            zclip.reserve(num_clip_paths);
            for (size_t idx : candidates) {
                const coord_t z = coord_t(idx) + idx_boundary_begin;
                zclip.emplace_back(ClipperZUtils::to_zpath(boundary[idx].contour.points, z));
                for (const Polygon &hole : boundary[idx].holes)
                    zclip.emplace_back(ClipperZUtils::to_zpath(hole.points, z));
            }
            pzclip = &zclip;
        }

        visitor.reset();
        ClipperZUtils::ZPaths segments = ClipperZUtils::clip_zpaths(ctIntersection, zsrc_batch, true, *pzclip, visitor.clipper_callback());
        merge_splits(segments, zsrc_splits);

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

        auto count_boundary_bboxes_containing = [&](const Point &pt) -> size_t {
            size_t count = 0;
            if (! boundary_aabb_tree.empty()) {
                AABBTreeIndirect::traverse(boundary_aabb_tree,
                    [&pt](const AABBTreeBBoxes::Node &node) { return node.bbox.contains(pt); },
                    [&count](const AABBTreeBBoxes::Node &) { return ++ count < 2; });
            } else {
                for (size_t idx : candidates)
                    if (boundary_bboxes[idx].defined && boundary_bboxes[idx].contains(pt))
                        if (++ count >= 2)
                            break;
            }
            return count;
        };

        out.reserve(out.size() + segments.size());
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
            if (candidates.size() == 1) {
                boundary_id = int(candidates.front());
            } else {
                int hint_boundary_id = -1;
                if (front_is && back_is && front_is->first == back_is->first) {
                    const int candidate_b = int(front_is->first - idx_boundary_begin);
                    if (count_boundary_bboxes_containing(Point(front.x(), front.y())) <= 1 ||
                        count_boundary_bboxes_containing(Point(back.x(), back.y())) <= 1)
                        boundary_id = candidate_b;
                    else
                        hint_boundary_id = candidate_b;
                } else if (front_is && ! back_is) {
                    const int candidate_b = int(front_is->first - idx_boundary_begin);
                    if (count_boundary_bboxes_containing(Point(front.x(), front.y())) <= 1)
                        boundary_id = candidate_b;
                    else
                        hint_boundary_id = candidate_b;
                } else if (back_is && ! front_is) {
                    const int candidate_b = int(back_is->first - idx_boundary_begin);
                    if (count_boundary_bboxes_containing(Point(back.x(), back.y())) <= 1)
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
                        path, boundary, boundary_bboxes, boundary_aabb_tree, candidates, hint_boundary_id);
            }

            assert(boundary_id >= 0 && boundary_id < int(boundary.size()));
            if (boundary_id >= 0)
                out.push_back({ uint32_t(src_id), uint32_t(boundary_id), ClipperZUtils::from_zpath(path) });
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
