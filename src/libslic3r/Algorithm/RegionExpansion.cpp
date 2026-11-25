#include "RegionExpansion.hpp"

#include <libslic3r/AABBTreeIndirect.hpp>
#include <libslic3r/ClipperZUtils.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Utils.hpp>

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

// Like expolygons_to_zpaths(), but each contour is expanded before converted to zpath.
// The expanded contours are then opened (the first point is repeated at the end) and all get the same Z.
static ClipperZUtils::ZPaths expolygon_to_zpaths_expanded_opened(const ExPolygon &expoly, const float expansion, const coord_t z)
{
    ClipperZUtils::ZPaths out;
    out.reserve(expoly.num_contours());
    for (size_t icontour = 0; icontour < expoly.num_contours(); ++ icontour) {
        // Orient CCW, then grow the contour and shrink the holes. The output contours are CCW.
        Polygon contour = expoly.contour_or_hole(icontour);
        if (! contour.is_counter_clockwise())
            contour.reverse();
        for (const Polygon &expanded : offset(contour, icontour == 0 ? expansion : -expansion, jtSquare))
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

static AABBTreeBBoxes build_aabb_tree_over_expolygons(const ExPolygons &expolygons) 
{
    // Calculate bounding boxes of internal slices.
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
    bboxes.reserve(expolygons.size());
    for (size_t i = 0; i < expolygons.size(); ++ i)
        bboxes.emplace_back(i, get_extents(expolygons[i].contour));
    // Build AABB tree over bounding boxes of boundary expolygons.
    AABBTreeBBoxes out;
    out.build_modify_input(bboxes);
    return out;
}

static int sample_in_expolygons(
    // AABB tree over boundary expolygons
    const AABBTreeBBoxes &aabb_tree, 
    const ExPolygons     &expolygons,
    const Point          &sample)
{
    int out = -1;
    AABBTreeIndirect::traverse(aabb_tree,
        [&sample](const AABBTreeBBoxes::Node &node) {
            return node.bbox.contains(sample);
        },
        [&expolygons, &sample, &out](const AABBTreeBBoxes::Node &node) {
            assert(node.is_leaf());
            assert(node.is_valid());
            if (expolygons[node.idx].contains(sample)) {
                out = int(node.idx);
                // Stop traversal.
                return false;
            }
            // Continue traversal.
            return true;
        });
    return out;
}

// Up to this many boundaries are looked through one by one, with more a tree over their bounding boxes pays off.
static constexpr size_t boundaries_per_tree = 16;

// The boundaries the expanded contours of a source within this bounding box can reach: those among a bounding box
// of theirs that meets it.
static void boundary_candidates(const ExPolygons &boundary, const AABBTreeBBoxes &aabb_tree, const BoundingBox &bbox, std::vector<size_t> &candidates)
{
    candidates.clear();
    if (! aabb_tree.empty()) {
        const AABBTreeBBoxes::BoundingBox query(bbox.min, bbox.max);
        AABBTreeIndirect::traverse(aabb_tree,
            [&query](const AABBTreeBBoxes::Node &node) { return node.bbox.intersects(query); },
            [&candidates](const AABBTreeBBoxes::Node &node) { candidates.emplace_back(node.idx); return true; });
        std::sort(candidates.begin(), candidates.end());
    } else
        for (size_t i = 0; i < boundary.size(); ++ i)
            if (get_extents(boundary[i].contour).overlap(bbox))
                candidates.emplace_back(i);
}

std::vector<WaveSeed> wave_seeds(
    // Source regions that are supposed to touch the boundary.
    const ExPolygons      &src,
    // Boundaries of source regions touching the "boundary" regions will be expanded into the "boundary" region.
    const ExPolygons      &boundary,
    // Initial expansion of src to make the source regions intersect with boundary regions just a little.
    float                  tiny_expansion,
    // Sort output by boundary ID and source ID.
    bool                   sorted)
{
    assert(tiny_expansion > 0);

    if (src.empty() || boundary.empty())
        return {};

    using Intersection  = ClipperZUtils::ClipperZIntersectionVisitor::Intersection;
    using Intersections = ClipperZUtils::ClipperZIntersectionVisitor::Intersections;

    // Every vertex carries an index: a vertex of a boundary contour that of its expolygon plus one, a vertex of an
    // expanded source contour that of its expolygon plus the number of boundaries, an intersection the negated index
    // into intersections minus one.
    Intersections         intersections;

    coord_t               idx_boundary_end = 1;
    // as closed contours
    ClipperZUtils::ZPaths zboundary = ClipperZUtils::expolygons_to_zpaths(boundary, idx_boundary_end);
    const coord_t         idx_src_end = idx_boundary_end + coord_t(src.size());

    // Over the boundaries right away when there are many, otherwise when a closed seed contour calls for it.
    AABBTreeBBoxes aabb_tree = boundary.size() < boundaries_per_tree ? AABBTreeBBoxes() : build_aabb_tree_over_expolygons(boundary);
    // Index of the boundary expolygon around the point, or -1 if there is none.
    auto sample_in_boundaries = [&boundary, &aabb_tree](const Point &sample) {
        if (boundary.size() == 1)
            return boundary.front().contains(sample) ? 0 : -1;
        if (aabb_tree.empty())
            aabb_tree = build_aabb_tree_over_expolygons(boundary);
        return sample_in_expolygons(aabb_tree, boundary, sample);
    };

    // The expanded contours of every source, as open contours, and a bounding box around each.
    std::vector<ClipperZUtils::ZPaths> zsrc(src.size());
    std::vector<BoundingBox>           bbox(src.size());
    for (size_t src_id = 0; src_id < src.size(); ++ src_id) {
        zsrc[src_id] = expolygon_to_zpaths_expanded_opened(src[src_id], tiny_expansion, idx_boundary_end + coord_t(src_id));
        for (const ClipperZUtils::ZPath &path : zsrc[src_id])
            for (const ClipperZUtils::ZPoint &pt : path)
                bbox[src_id].merge(Point(pt.x(), pt.y()));
        // A point of a piece may be rounded, and is looked up in the box of its source below.
        if (! zsrc[src_id].empty())
            bbox[src_id] = bbox[src_id].inflated(SCALED_EPSILON);
    }

    // The sources are clipped in batches, the bounding boxes of the sources of one batch away from each other. So the
    // source of a piece is known to be one of the sources of its batch: a vertex of it names the source, and a piece
    // without a vertex of its source lies in the bounding box of that source. Clipped all together, a piece whose ends
    // are both vertices of the boundary carried no source index, and a point of it could lie on the outline of any
    // source around, or on the outline of one of its holes, expanded by the same tiny_expansion.
    std::vector<std::vector<size_t>> batches;
    {
        // From left to right, so that a source finds the batch of the sources reaching as far right as it does.
        std::vector<size_t> order(src.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&bbox](size_t l, size_t r) { return bbox[l].min.x() < bbox[r].min.x(); });
        for (size_t src_id : order) {
            if (zsrc[src_id].empty())
                continue;
            auto meets = [&bbox, &b = bbox[src_id]](size_t other) { return bbox[other].overlap(b); };
            auto batch = std::find_if(batches.begin(), batches.end(), [&meets](const std::vector<size_t> &batch) {
                return std::none_of(batch.begin(), batch.end(), meets);
            });
            if (batch == batches.end())
                batch = batches.emplace(batch);
            batch->emplace_back(src_id);
        }
    }

    // Sort paths into their respective islands.
    // Each src x boundary will be processed (wave expanded) independently.
    // Multiple pieces of a single src may intersect the same boundary.
    WaveSeeds out;
    ClipperZUtils::ClipperZIntersectionVisitor visitor(intersections);
    std::vector<size_t>                        candidates;
    for (const std::vector<size_t> &batch : batches) {
        BoundingBox bbox_batch;
        for (size_t src_id : batch)
            bbox_batch.merge(bbox[src_id]);
        boundary_candidates(boundary, aabb_tree, bbox_batch, candidates);
        if (candidates.empty())
            continue;
        // as open contours, and where each of them opens.
        ClipperZUtils::ZPaths                             zsrc_batch;
        std::vector<std::pair<ClipperZUtils::ZPoint, int>> zsrc_splits;
        for (size_t src_id : batch) {
            for (const ClipperZUtils::ZPath &path : zsrc[src_id])
                zsrc_splits.emplace_back(path.front(), -1);
            append(zsrc_batch, std::move(zsrc[src_id]));
        }
        // as closed contours
        const ClipperZUtils::ZPaths *pzclip = &zboundary;
        ClipperZUtils::ZPaths         zclip;
        if (candidates.size() != boundary.size()) {
            // Only the boundaries reached, each contour with the index of its expolygon.
            for (size_t idx : candidates) {
                zclip.emplace_back(ClipperZUtils::to_zpath(boundary[idx].contour.points, coord_t(idx) + 1));
                for (const Polygon &hole : boundary[idx].holes)
                    zclip.emplace_back(ClipperZUtils::to_zpath(hole.points, coord_t(idx) + 1));
            }
            pzclip = &zclip;
        }
        std::sort(zsrc_splits.begin(), zsrc_splits.end(), [](const auto &l, const auto &r){ return ClipperZUtils::zpoint_lower(l.first, r.first); });
        ClipperZUtils::ZPaths segments = ClipperZUtils::clip_zpaths(ctIntersection, zsrc_batch, true, *pzclip, visitor.clipper_callback());
        merge_splits(segments, zsrc_splits);

        for (const ClipperZUtils::ZPath &path : segments) {
            assert(path.size() >= 2);
            ClipperZUtils::ZPoint front = path.front();
            ClipperZUtils::ZPoint back  = path.back();
            // Index of the boundary a vertex of the path is a vertex of a contour of, if there is such a vertex.
            coord_t boundary_z = -1;
            // Index of the source, by a vertex of the expanded contours of it, if the path has such a vertex.
            coord_t src_z      = -1;
            for (const ClipperZUtils::ZPoint &point : path) {
                if (point.z() >= idx_boundary_end && point.z() < idx_src_end && src_z < 0)
                    src_z = point.z();
                else if (point.z() >= 1 && point.z() < idx_boundary_end && boundary_z < 0)
                    boundary_z = point.z();
                if (src_z >= 0 && boundary_z >= 0)
                    break;
            }
            int src_id = src_z >= 0 ? int(src_z - idx_boundary_end) : -1;
            if (src_id < 0) {
                // No vertex of the source: the piece runs within the box of the one source of the batch around it.
                for (size_t candidate : batch)
                    if (bbox[candidate].contains(Point(front.x(), front.y()))) {
                        src_id = int(candidate);
                        break;
                    }
                if (src_id < 0)
                    continue;
            }

            // Both ends of a seed segment are supposed to be inside a single boundary expolygon.
            // Thus as long as the seed contour is not closed, it should be open at a boundary point.
            if (front == back && front.z() < idx_boundary_end) {
                // This should be a very rare exception.
                // See https://github.com/prusa3d/PrusaSlicer/issues/12469.
                // Segement is open, yet its first point seems to be part of boundary polygon.
                // Take the first point with src polygon index.
                for (const ClipperZUtils::ZPoint &point : path) {
                    if (point.z() >= idx_boundary_end) {
                        front = point;
                        back = point;
                    }
                }
            }

            const Intersection *intersection = nullptr;
            auto intersection_point_valid = [idx_boundary_end, idx_src_end](const Intersection &is) {
                return is.first >= 1 && is.first < idx_boundary_end &&
                       is.second >= idx_boundary_end && is.second < idx_src_end;
            };
            if (front.z() < 0) {
                const Intersection &is = intersections[- front.z() - 1];
                assert(intersection_point_valid(is));
                if (intersection_point_valid(is))
                    intersection = &is;
            }
            if (! intersection && back.z() < 0) {
                const Intersection &is = intersections[- back.z() - 1];
                assert(intersection_point_valid(is));
                if (intersection_point_valid(is))
                    intersection = &is;
            }
            // The source is known, so only the boundary around the piece is left to find.
            int boundary_id = -1;
            if (intersection) {
                // The path intersects the boundary contour at least at one side.
                boundary_id = int(intersection->first - 1);
            } else if (front != back) {
                // Both ends are vertices of the input. One of a boundary contour tells the boundary, else it is the one
                // around the first point.
                boundary_id = boundary_z >= 0 ? int(boundary_z - 1) : sample_in_boundaries(Point(front.x(), front.y()));
            } else {
                // This should be a closed contour.
                // Find a source boundary expolygon of one sample of this closed path.
                boundary_id = sample_in_boundaries(Point(front.x(), front.y()));
                // Boundary that contains the sample point was found.
                assert(boundary_id >= 0);
            }
            if (boundary_id >= 0)
                out.push_back({ uint32_t(src_id), uint32_t(boundary_id), ClipperZUtils::from_zpath(path) });
        }
    }

    if (sorted)
        // Sort the seeds by their intersection boundary and source contour.
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
    std::sort(expanded.begin(), expanded.end(), [](const auto &l, const auto &r) { return l.src_id < r.src_id; });
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
