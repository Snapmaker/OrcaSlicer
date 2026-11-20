#include "LocalZClipper.hpp"

#include "../ClipperUtils.hpp"
#include "LocalZClipDecision.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <boost/iterator/indirect_iterator.hpp>

namespace Slic3r {
namespace {

bool bounded(const Point& point)
{
    return point.x() >= -(1 << 29) && point.x() <= (1 << 29) && point.y() >= -(1 << 29) && point.y() <= (1 << 29);
}

struct BorrowedPaths
{
    const std::vector<const Points*>& paths;
    auto                              begin() const { return boost::make_indirect_iterator(paths.begin()); }
    auto                              end() const { return boost::make_indirect_iterator(paths.end()); }
    size_t                            size() const { return paths.size(); }
};

template<class SubjectProvider>
ClipperLib::PolyTree clip(SubjectProvider subject, const std::vector<const Points*>& masks, ClipperLib::ClipType operation)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(subject, ClipperLib::ptSubject, false);
    clipper.AddPaths(BorrowedPaths{masks}, ClipperLib::ptClip, true);
    ClipperLib::PolyTree result;
    clipper.Execute(operation, result, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
    return result;
}

Polylines to_polylines(ClipperLib::PolyTree tree)
{
    Polylines result;
    result.reserve(tree.Total());
    // Same depth-first order as ClipperUtils' PolyTreeToPolylines.
    for (auto* node = tree.GetFirst(); node != nullptr; node = node->GetNext())
        if (!node->Contour.empty())
            result.emplace_back(std::move(node->Contour));
    return result;
}

bool has_scanline_ties(const Polylines& lines)
{
    if (lines.size() < 2)
        return false;
    std::vector<coord_t> maxima;
    maxima.reserve(lines.size());
    for (const Polyline& line : lines)
        maxima.push_back(get_extents<true>(line.points).max.y());
    std::sort(maxima.begin(), maxima.end());
    return std::adjacent_find(maxima.begin(), maxima.end()) != maxima.end();
}

} // namespace

struct LocalZClipCache::Impl
{
    struct Mask
    {
        const ExPolygons* polygons;
        size_t            hash, users;
    };
    struct Entry
    {
        size_t              mask;
        bool                difference;
        std::vector<Points> subject;
        Polylines           result;
    };
    mutable std::mutex                     mutex;
    std::vector<Mask>                      masks;
    std::unordered_multimap<size_t, Entry> entries;
    const size_t                           byte_budget;
    Statistics                             stats;
    explicit Impl(size_t budget) : byte_budget(budget) {}

    static void combine(size_t& hash, size_t value) { hash ^= value + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2); }
    template<class Provider> static size_t hash_paths(Provider paths, size_t seed)
    {
        combine(seed, paths.size());
        for (const Points& points : paths) {
            combine(seed, points.size());
            for (const Point& point : points) {
                combine(seed, std::hash<coord_t>{}(point.x()));
                combine(seed, std::hash<coord_t>{}(point.y()));
            }
        }
        return seed;
    }
    size_t bind(const ExPolygons& polygons)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t                hash = hash_paths(ClipperUtils::ExPolygonsProvider(polygons), 0);
        for (size_t i = 0; i < masks.size(); ++i) {
            // Preserve polygon, hole and vertex order as well as coordinates.
            // Geometric equivalence alone would not preserve Clipper's ties.
            if (masks[i].hash == hash && *masks[i].polygons == polygons) {
                ++masks[i].users;
                return i;
            }
        }
        masks.push_back({&polygons, hash, 1});
        stats.masks = masks.size();
        return masks.size() - 1;
    }
    template<class Provider> static bool matches(const Entry& entry, size_t mask, bool difference, Provider paths)
    {
        return entry.mask == mask && entry.difference == difference && entry.subject.size() == paths.size() &&
               std::equal(entry.subject.begin(), entry.subject.end(), paths.begin());
    }
    template<class Provider> bool find(size_t mask, bool difference, Provider paths, Polylines& result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (masks[mask].users < 2 || byte_budget == 0)
            return false;
        const auto range = entries.equal_range(hash_paths(paths, mask * 2 + size_t(difference)));
        for (auto it = range.first; it != range.second; ++it) {
            if (matches(it->second, mask, difference, paths)) {
                result = it->second.result;
                ++stats.hits;
                return true;
            }
        }
        ++stats.misses;
        return false;
    }
    template<class Provider> void insert(size_t mask, bool difference, Provider paths, const Polylines& result)
    {
        std::lock_guard<std::mutex> lock(mutex);
        // Only retain masks shared by multiple passes. Bound both retained data
        // (including a per-entry bookkeeping allowance) and hash-table growth.
        if (masks[mask].users < 2 || entries.size() >= 65536 || stats.bytes >= byte_budget)
            return;
        const size_t hash  = hash_paths(paths, mask * 2 + size_t(difference));
        const auto   range = entries.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            if (matches(it->second, mask, difference, paths))
                return;
        size_t bytes = sizeof(Entry) + 64 + paths.size() * sizeof(Points) + result.size() * sizeof(Polyline);
        for (const Points& points : paths)
            bytes += points.size() * sizeof(Point);
        for (const Polyline& line : result)
            bytes += line.points.size() * sizeof(Point) + line.fitting_result.size() * sizeof(PathFittingData);
        if (bytes > byte_budget - stats.bytes)
            return;
        Entry entry{mask, difference, {}, result};
        entry.subject.reserve(paths.size());
        for (const Points& points : paths)
            entry.subject.emplace_back(points);
        entries.emplace(hash, std::move(entry));
        stats.bytes += bytes;
        stats.entries = entries.size();
    }
};

LocalZClipCache::LocalZClipCache(size_t byte_budget) : m_impl(std::make_unique<Impl>(byte_budget)) {}
LocalZClipCache::~LocalZClipCache() = default;
LocalZClipCache::Statistics LocalZClipCache::statistics() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->stats;
}

LocalZClipper::LocalZClipper(const ExPolygons& masks, LocalZClipCache* cache) : m_masks(&masks)
{
    if (cache && !masks.empty()) {
        m_cache      = cache;
        m_cache_mask = cache->m_impl->bind(masks);
    }
    m_polygon_bounds.reserve(masks.size());
    for (const ExPolygon& mask : masks) {
        m_path_count += 1 + mask.holes.size();
        BoundingBox bounds = get_extents<true>(mask.contour.points);
        for (const Polygon& hole : mask.holes)
            bounds.merge(get_extents<true>(hole.points));
        m_polygon_bounds.push_back(bounds);
        m_bounds.merge(bounds);
    }
    // Reuse the conservative integer classifier without packing an entire
    // layer's queries. The exact clipping API retains
    // Clipper for every nonempty result, including wholly contained paths.
    size_t edge_count = 0;
    for (const ExPolygon& mask : masks) {
        edge_count += mask.contour.points.size();
        for (const Polygon& hole : mask.holes)
            edge_count += hole.points.size();
    }
    if (edge_count < 3 || edge_count > (1u << 20))
        return;
    std::vector<ClipMaskEdge> edges;
    edges.reserve(edge_count);
    auto append_ring = [&edges](const Points& points) {
        for (size_t i = 0; i < points.size(); ++i) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % points.size()];
            if (!bounded(a) || !bounded(b))
                return false;
            edges.push_back({int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())});
        }
        return true;
    };
    for (const ExPolygon& mask : masks) {
        if (!append_ring(mask.contour.points))
            return;
        for (const Polygon& hole : mask.holes)
            if (!append_ring(hole.points))
                return;
    }
    m_edge_count  = uint32_t(edges.size());
    m_empty_index = std::make_shared<LocalZClipIndex>(edges, 0, m_edge_count);
}

bool LocalZClipper::proven_empty(const Points& subject, bool difference) const
{
    if (!m_empty_index || subject.size() < 2)
        return false;
    for (size_t i = 1; i < subject.size(); ++i) {
        const Point& a = subject[i - 1];
        const Point& b = subject[i];
        if (!bounded(a) || !bounded(b) ||
            m_empty_index->classify(
                {{int32_t(a.x()), int32_t(a.y()), int32_t(b.x()), int32_t(b.y())}, 0, m_edge_count, uint32_t(difference), 0}) !=
                uint32_t(ClipDecision::Empty))
            return false;
    }
    return true;
}

bool LocalZClipper::may_intersect(const BoundingBox& subject_bounds) const
{
    return subject_bounds.defined && m_bounds.defined && subject_bounds.overlap(m_bounds) &&
           std::any_of(m_polygon_bounds.begin(), m_polygon_bounds.end(),
                       [&subject_bounds](const BoundingBox& bounds) { return bounds.defined && subject_bounds.overlap(bounds); });
}

std::vector<const Points*> LocalZClipper::candidates(const BoundingBox& subject_bounds) const
{
    std::vector<const Points*> paths;
    const auto                 overlaps_y = [&subject_bounds](const BoundingBox& bounds) {
        return bounds.min.y() <= subject_bounds.max.y() && bounds.max.y() >= subject_bounds.min.y();
    };
    if (!subject_bounds.defined || !m_bounds.defined || !overlaps_y(m_bounds))
        return paths;
    for (size_t i = 0; i < m_polygon_bounds.size(); ++i) {
        // Retain every polygon overlapping the subject's Y range, even when it
        // is disjoint in X: its vertices contribute scanlines, which can affect
        // integer rounding at subject intersections. Include boundary touches
        // and keep complete polygons/holes in the original mask order.
        if (m_polygon_bounds[i].defined && overlaps_y(m_polygon_bounds[i])) {
            const ExPolygon& mask = (*m_masks)[i];
            paths.push_back(&mask.contour.points);
            for (const Polygon& hole : mask.holes)
                paths.push_back(&hole.points);
        }
    }
    return paths;
}

Polylines LocalZClipper::clip_path(const Polyline& subject, const LocalZClipper* include_masks, const LocalZClipper* exclude_masks)
{
    Polylines result;
    if (include_masks != nullptr && !include_masks->empty())
        result = include_masks->intersection(subject);
    else
        result.emplace_back(subject);
    if (exclude_masks != nullptr && !exclude_masks->empty())
        result = exclude_masks->difference(result);
    return result;
}

Polylines LocalZClipper::intersection(const Polyline& subject) const
{
    const auto provider = ClipperUtils::SinglePathProvider(subject.points);
    Polylines  result;
    const bool cached = m_cache && m_cache->m_impl->find(m_cache_mask, false, provider, result);
    if (!cached) {
        const auto bounds = get_extents<true>(subject.points);
        const auto masks  = may_intersect(bounds) ? candidates(bounds) : std::vector<const Points*>{};
        if (!masks.empty() && !proven_empty(subject.points, false))
            result = to_polylines(clip(provider, masks, ClipperLib::ctIntersection));
        // Removing unrelated minima can change Clipper's tie ordering. Preserve
        // fragment order for equal starting scanlines with a full-mask query.
        if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
            result = intersection_pl(Polylines{subject}, *m_masks);
        if (m_cache)
            m_cache->m_impl->insert(m_cache_mask, false, provider, result);
    }
    return result;
}

Polylines LocalZClipper::difference(const Polylines& subject) const
{
    const auto provider = ClipperUtils::PolylinesProvider(subject);
    Polylines  result;
    const bool cached = m_cache && m_cache->m_impl->find(m_cache_mask, true, provider, result);
    if (!cached) {
        BoundingBox bounds;
        for (const Polyline& line : subject)
            bounds.merge(get_extents<true>(line.points));
        // Even with no candidate masks, let Clipper normalize path direction
        // and duplicate points exactly as before; returning input would differ.
        const auto masks = candidates(bounds);
        if (!std::all_of(subject.begin(), subject.end(), [this](const Polyline& line) { return proven_empty(line.points, true); }))
            result = to_polylines(clip(provider, masks, ClipperLib::ctDifference));
        if (m_masks != nullptr && masks.size() != m_path_count && has_scanline_ties(result))
            result = diff_pl(subject, *m_masks);
        if (m_cache)
            m_cache->m_impl->insert(m_cache_mask, true, provider, result);
    }
    return result;
}

bool LocalZClipper::intersects(const Polyline& subject) const
{
    const auto bounds = get_extents<true>(subject.points);
    const auto masks  = may_intersect(bounds) ? candidates(bounds) : std::vector<const Points*>{};
    bool       result = false;
    if (!masks.empty()) {
        const auto tree = clip(ClipperUtils::SinglePathProvider(subject.points), masks, ClipperLib::ctIntersection);
        for (const auto* node = tree.GetFirst(); node != nullptr; node = node->GetNext()) {
            if (node->Contour.size() >= 2) {
                result = true;
                break;
            }
        }
    }
    return result;
}

} // namespace Slic3r
