#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>

#include "ClipperUtils.hpp"
#include "AABBTreeIndirect.hpp"
#include "Geometry.hpp"
#include "ShortestPath.hpp"

#include <clipper2/clipper.h>

// #define CLIPPER_UTILS_DEBUG

#ifdef CLIPPER_UTILS_DEBUG
#include "SVG.hpp"
#endif /* CLIPPER_UTILS_DEBUG */

// Profiling support using the Shiny intrusive profiler
//#define CLIPPER_UTILS_PROFILE
#if defined(SLIC3R_PROFILE) && defined(CLIPPER_UTILS_PROFILE)
	#include <Shiny/Shiny.h>
	#define CLIPPERUTILS_PROFILE_FUNC() PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name) PROFILE_BLOCK(name)
#else
	#define CLIPPERUTILS_PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name)
#endif

namespace Slic3r {

namespace ClipperUtils {
Points EmptyPathsProvider::s_empty_points;
Points SinglePathProvider::s_end;

// Clip source polygon to be used as a clipping polygon with a bouding box around the source (to be clipped) polygon.
// Useful as an optimization for expensive Clipper operations, for example when clipping source polygons one by one
// with a set of polygons covering the whole layer below.
template<typename PointsType> inline void clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox, PointsType &out, const bool get_entire_polygons=false)
{
    using PointType = typename PointsType::value_type;

    out.clear();
    const size_t cnt = src.size();
    if (cnt < 3) return;

    enum class Side { Left = 1, Right = 2, Top = 4, Bottom = 8 };

    auto sides = [bbox](const PointType &p) {
        return int(p.x() < bbox.min.x()) * int(Side::Left) + int(p.x() > bbox.max.x()) * int(Side::Right) + int(p.y() < bbox.min.y()) * int(Side::Bottom) +
               int(p.y() > bbox.max.y()) * int(Side::Top);
    };

    int          sides_prev = sides(src.back());
    int          sides_this = sides(src.front());
    const size_t last       = cnt - 1;
    for (size_t i = 0; i < last; ++i) {
        int sides_next = sides(src[i + 1]);
        if ( // This point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0) {
            out.emplace_back(src[i]);
            sides_prev = sides_this;
        } else {
            // All the three points (this, prev, next) are outside at the same side.
            // Ignore this point.
        }
        sides_this = sides_next;
    }

    // Never produce just a single point output polygon.
    if (!out.empty()) {
        if (get_entire_polygons) {
            out=src;
        } else {
            if (int sides_next = sides(out.front());
            // The last point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0)
            out.emplace_back(src.back());
        }
    }
}

void clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox, Points &out, const bool get_entire_polygons) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out, get_entire_polygons); }
void clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox, ZPoints &out) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out); }

template<typename PointsType> [[nodiscard]] PointsType clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox)
{
    PointsType out;
    clip_clipper_polygon_with_subject_bbox(src, bbox, out);
    return out;
}

[[nodiscard]] Points  clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }
[[nodiscard]] ZPoints clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }

void clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, Polygon &out) { 
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points);
}

[[nodiscard]] Polygon clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygon out;
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points, get_entire_polygons);
    return out;
}

[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const Polygons &src, const BoundingBox &bbox)
{
    Polygons out;
    out.reserve(src.size());
    for (const Polygon &p : src) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(src.num_contours());
    out.emplace_back(clip_clipper_polygon_with_subject_bbox(src.contour, bbox, get_entire_polygons));
    for (const Polygon &p : src.holes) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox, get_entire_polygons));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygons &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(number_polygons(src));
    for (const ExPolygon &p : src) {
        Polygons temp = clip_clipper_polygons_with_subject_bbox(p, bbox, get_entire_polygons);
        out.insert(out.end(), temp.begin(), temp.end());
    }

    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) {return polygon.empty(); }), out.end());
    return out;
}
}

namespace C2 = Clipper2Lib;

namespace {

C2::ClipType to_c2(ClipType type)
{
    switch (type) {
    case ctIntersection: return C2::ClipType::Intersection;
    case ctUnion:        return C2::ClipType::Union;
    case ctDifference:   return C2::ClipType::Difference;
    default:                         return C2::ClipType::Xor;
    }
}

C2::FillRule to_c2(PolyFillType type)
{
    switch (type) {
    case pftEvenOdd:  return C2::FillRule::EvenOdd;
    case pftNonZero:  return C2::FillRule::NonZero;
    case pftPositive: return C2::FillRule::Positive;
    default:                      return C2::FillRule::Negative;
    }
}

C2::JoinType to_c2(JoinType type)
{
    switch (type) {
    case jtSquare: return C2::JoinType::Square;
    case jtRound:  return C2::JoinType::Round;
    default:                   return C2::JoinType::Miter;
    }
}

C2::EndType to_c2(EndType type)
{
    switch (type) {
    case etClosedPolygon: return C2::EndType::Polygon;
    case etClosedLine:    return C2::EndType::Joined;
    case etOpenSquare:    return C2::EndType::Square;
    case etOpenRound:     return C2::EndType::Round;
    default:                          return C2::EndType::Butt;
    }
}

inline int64_t px(const Point &pt) { return pt.x(); }
inline int64_t py(const Point &pt) { return pt.y(); }
inline int64_t px(const C2::Point64 &pt) { return pt.x; }
inline int64_t py(const C2::Point64 &pt) { return pt.y; }

template<typename PathsProvider>
C2::Paths64 to_paths64(PathsProvider &&paths)
{
    C2::Paths64 out;
    out.reserve(paths.size());
    for (const Points &path : paths) {
        C2::Path64 &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const Point &pt : path)
            dst.emplace_back(pt.x(), pt.y());
    }
    return out;
}

// Drops vertices closer than `shortest` to the last kept one, like Clipper1's ShortestEdgeLength.
template<class PathT>
bool append_offset_path(C2::Paths64 &out, const PathT &path, double shortest, C2::EndType end)
{
    int high = int(path.size()) - 1;
    if (high < 0)
        return false;
    const double shortest2 = shortest * shortest;
    auto same = [shortest2](const auto &a, const auto &b) {
        const double dx = double(px(a) - px(b));
        const double dy = double(py(a) - py(b));
        return shortest2 > 0. ? dx * dx + dy * dy < shortest2 : dx == 0. && dy == 0.;
    };
    if (end == C2::EndType::Polygon || end == C2::EndType::Joined)
        while (high > 0 && same(path[high], path[0]))
            -- high;
    C2::Path64 dst;
    dst.reserve(high + 1);
    dst.emplace_back(px(path[0]), py(path[0]));
    for (int i = 1, last = 0; i <= high; ++ i)
        if (! same(path[i], path[last])) {
            dst.emplace_back(px(path[i]), py(path[i]));
            last = i;
        }
    if (end == C2::EndType::Polygon && dst.size() < 3)
        return false;
    out.emplace_back(std::move(dst));
    return true;
}

inline Points c2_to_points(const C2::Path64 &path)
{
    Points out;
    out.reserve(path.size());
    for (const C2::Point64 &pt : path)
        out.emplace_back(pt.x, pt.y);
    return out;
}

Polygons c2_to_polygons(const C2::Paths64 &paths)
{
    Polygons out;
    out.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        out.emplace_back().points = c2_to_points(path);
    return out;
}

Polylines c2_to_polylines(const C2::Paths64 &paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        out.emplace_back(c2_to_points(path));
    return out;
}

// Only sizes the reserve of c2_to_expolygons(): for a tree Clipper2 misnested (see below) the count is approximate.
size_t c2_count_expolygons(const C2::PolyPath64 &outer)
{
    size_t cnt = 1;
    for (const auto &hole : outer)
        for (const auto &island : *hole)
            cnt += c2_count_expolygons(*island);
    return cnt;
}

// Clipper2 sometimes puts a path at the wrong level of its PolyTree: a hole beside the outer path around it instead of
// under it, or an island at the level of holes. Taken as they are, a misplaced hole becomes a clockwise contour and its
// outer an expolygon that covers it; whole-layer booleans cancel the two, but anything handling expolygons one at a time
// sees the hole filled. So the orientation of a path decides its role: a clockwise path where a contour belongs is
// collected in stray_holes, for c2_attach_stray_holes() to give to the contour around it.
void c2_append_expolygons(const C2::PolyPath64 &outer, ExPolygons &out, std::vector<const C2::Path64*> &stray_holes)
{
    if (C2::Area(outer.Polygon()) < 0.) {
        stray_holes.emplace_back(&outer.Polygon());
        for (const auto &island : outer)
            c2_append_expolygons(*island, out, stray_holes);
        return;
    }
    // Indexed, as the recursion below may reallocate out.
    const size_t idx = out.size();
    out.emplace_back().contour.points = c2_to_points(outer.Polygon());
    out[idx].holes.reserve(outer.Count());
    for (const auto &child : outer)
        if (C2::Area(child->Polygon()) > 0.)
            c2_append_expolygons(*child, out, stray_holes);
        else {
            out[idx].holes.emplace_back().points = c2_to_points(child->Polygon());
            // Islands inside the hole.
            for (const auto &island : *child)
                c2_append_expolygons(*island, out, stray_holes);
        }
}

// Whether outer encloses inner, two paths of one Clipper2 result. Such paths don't cross, they only touch, and rounding
// both to the grid leaves a touching vertex up to about 1.5 units across the other path. So a vertex of inner that close
// to outer doesn't count, the first vertex clear of it decides, and a path touching at every vertex goes by a point
// strictly inside it: either lies on the same side of outer as the whole path. A sliver too thin to hold such a point is
// taken as not enclosed. A hole closely hugging an island inside it would pass, so callers skip any outer no larger than
// inner: a path inside another is always the smaller.
bool path_encloses(const Polygon &outer, const Polygon &inner)
{
    // The rounding of the two paths, and of the projection Polygon::on_boundary() measures to.
    static constexpr double touching = 3.;
    for (const Point &pt : inner.points)
        if (! outer.on_boundary(pt, touching))
            return contains(outer, pt, true);
    const std::optional<Point> pt = ClipperUtils::interior_point(inner);
    return pt && contains(outer, *pt, true);
}

// Gives each stray hole to the innermost of the contours out[first..] around it. Contours of a Clipper2 result don't
// cross, so those around a hole are nested and the innermost one is the smallest: candidates are tried smallest first,
// from the first one larger than the hole.
void c2_attach_stray_holes(ExPolygons &out, size_t first, const std::vector<const C2::Path64*> &stray_holes)
{
    if (stray_holes.empty())
        return;
    using Tree = AABBTreeIndirect::Tree<2, coord_t>;
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
    bboxes.reserve(out.size() - first);
    for (size_t i = first; i < out.size(); ++ i)
        bboxes.emplace_back(i, get_extents(out[i].contour));
    Tree tree;
    tree.build_modify_input(bboxes);
    // Contour areas, computed when a contour first becomes a candidate.
    std::vector<double> areas(out.size(), 0.);
    std::vector<size_t> candidates;
    for (const C2::Path64 *path : stray_holes) {
        Polygon hole;
        hole.points = c2_to_points(*path);
        const double            hole_area = - C2::Area(*path);
        const BoundingBox       bbox      = get_extents(hole);
        const Tree::BoundingBox hole_bbox(bbox.min, bbox.max);
        candidates.clear();
        AABBTreeIndirect::traverse(tree,
            [&hole_bbox](const Tree::Node &node) { return node.bbox.contains(hole_bbox); },
            [&candidates](const Tree::Node &node) { candidates.emplace_back(node.idx); return true; });
        for (size_t idx : candidates)
            if (areas[idx] == 0.)
                areas[idx] = out[idx].contour.area();
        std::sort(candidates.begin(), candidates.end(), [&areas](size_t a, size_t b) { return areas[a] < areas[b]; });
        auto larger = std::upper_bound(candidates.begin(), candidates.end(), hole_area, [&areas](double area, size_t idx) { return area < areas[idx]; });
        if (auto it = std::find_if(larger, candidates.end(), [&out, &hole](size_t idx) { return path_encloses(out[idx].contour, hole); });
            it == candidates.end())
            // Nothing around it: keep the path as Clipper2 returned it.
            out.emplace_back().contour = std::move(hole);
        else
            out[*it].holes.emplace_back(std::move(hole));
    }
}

ExPolygons c2_to_expolygons(const C2::PolyTree64 &tree)
{
    size_t cnt = 0;
    for (const auto &outer : tree)
        cnt += c2_count_expolygons(*outer);
    ExPolygons out;
    out.reserve(cnt);
    std::vector<const C2::Path64*> stray_holes;
    for (const auto &outer : tree)
        c2_append_expolygons(*outer, out, stray_holes);
    c2_attach_stray_holes(out, 0, stray_holes);
    return out;
}

// A vertex the path visits twice: Clipper1 split a contour touching itself at a vertex into two contours, Clipper2 keeps
// it as one contour that passes through the vertex twice. Writes both positions, the earlier one first.
static bool c2_repeated_vertex(const C2::Path64 &path, size_t &first, size_t &second)
{
    struct PointHash {
        std::size_t operator()(const C2::Point64 &p) const
        {
            return std::hash<int64_t>{}(p.x) ^ (std::hash<int64_t>{}(p.y) << 1);
        }
    };
    std::unordered_map<C2::Point64, size_t, PointHash> seen;
    seen.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++ i)
        if (! seen.emplace(path[i], i).second) {
            first  = seen[path[i]];
            second = i;
            return true;
        }
    return false;
}

// Appends the simple loops of a contour that visits a vertex twice, the loop around the other ones first.
static void c2_append_split_contour(C2::Paths64 &out, C2::Path64 &&contour)
{
    std::vector<C2::Path64> pending;
    pending.emplace_back(std::move(contour));
    while (! pending.empty()) {
        C2::Path64 ring = std::move(pending.back());
        pending.pop_back();
        size_t first, second;
        if (! c2_repeated_vertex(ring, first, second)) {
            out.emplace_back(std::move(ring));
            continue;
        }
        // The loop closing at the repeated vertex, and the rest of the contour, which still opens there.
        C2::Path64 inner(ring.begin() + first, ring.begin() + second);
        C2::Path64 outer(ring.begin() + second, ring.end());
        outer.insert(outer.end(), ring.begin(), ring.begin() + first);
        // A loop of less than three points is a spike out and back, it encloses nothing.
        if (inner.size() >= 3)
            pending.emplace_back(std::move(inner));
        if (outer.size() >= 3)
            pending.emplace_back(std::move(outer));
    }
}

static void c2_split_repeated_vertices(C2::Paths64 &paths)
{
    C2::Paths64 out;
    size_t      first, second;
    for (C2::Path64 &path : paths)
        if (c2_repeated_vertex(path, first, second))
            c2_append_split_contour(out, std::move(path));
        else
            out.emplace_back(std::move(path));
    paths = std::move(out);
}

// The polygons of a tree, outside in, and whether one of them visits a vertex twice.
static void c2_tree_to_paths(const C2::PolyPath64 &node, C2::Paths64 &out, bool &repeated)
{
    size_t first, second;
    repeated = repeated || c2_repeated_vertex(node.Polygon(), first, second);
    out.push_back(node.Polygon());
    for (const auto &child : node)
        c2_tree_to_paths(*child, out, repeated);
}

// Whether the loop lies inside the contour. Loops of one result only touch, so the middle of an edge of the loop that is
// clear of the contour decides for the whole loop.
static bool c2_loop_inside(const Polygon &contour, const Polygon &loop)
{
    for (size_t i = 0; i < loop.size(); ++ i) {
        const Point &a = loop[i];
        const Point &b = loop[(i + 1) % loop.size()];
        const Point  middle((a.x() + b.x()) / 2, (a.y() + b.y()) / 2);
        if (contains(contour, middle, false))
            return true;
        if (! contains(contour, middle, true))
            return false;
    }
    // The loop runs along the contour: go by the bounding boxes.
    return contour.bounding_box().contains(loop.bounding_box());
}

// ExPolygons of simple loops that only touch each other: a clockwise loop is a hole of the smallest loop around it.
static ExPolygons c2_loops_to_expolygons(Polygons &&loops)
{
    struct Loop
    {
        size_t      idx;
        double      area;
        BoundingBox bbox;
    };
    std::vector<Loop> sorted;
    sorted.reserve(loops.size());
    for (size_t i = 0; i < loops.size(); ++ i) {
        const double area = loops[i].area();
        sorted.push_back({ i, area < 0. ? - area : area, loops[i].bounding_box() });
    }
    std::sort(sorted.begin(), sorted.end(), [](const Loop &l, const Loop &r) { return l.area > r.area; });

    // The loops around another one are the larger ones: they are nested, so the smallest of them is the one around it.
    std::vector<size_t> parent(loops.size(), size_t(-1));
    for (size_t i = 0; i < sorted.size(); ++ i)
        for (size_t j = i; j > 0; -- j)
            if (sorted[j - 1].bbox.contains(sorted[i].bbox) && c2_loop_inside(loops[sorted[j - 1].idx], loops[sorted[i].idx])) {
                parent[sorted[i].idx] = sorted[j - 1].idx;
                break;
            }

    ExPolygons out;
    // The expolygon whose contour is the loop, for the counter-clockwise loops.
    std::vector<size_t> owner(loops.size(), size_t(-1));
    for (const Loop &loop : sorted) {
        const size_t p = parent[loop.idx];
        // A clockwise loop inside a counter-clockwise one is its hole, anything else is an expolygon of its own.
        if (p != size_t(-1) && ! loops[loop.idx].is_counter_clockwise() && loops[p].is_counter_clockwise())
            out[owner[p]].holes.push_back(std::move(loops[loop.idx]));
        else {
            out.emplace_back().contour = std::move(loops[loop.idx]);
            owner[loop.idx] = out.size() - 1;
        }
    }
    return out;
}

// c2_to_expolygons(), with the contours that visit a vertex twice split the way simplify_polygons() does.
static ExPolygons c2_to_expolygons_split(const C2::PolyTree64 &tree)
{
    C2::Paths64 loops;
    bool        repeated = false;
    for (const auto &outer : tree)
        c2_tree_to_paths(*outer, loops, repeated);
    if (! repeated)
        return c2_to_expolygons(tree);
    c2_split_repeated_vertices(loops);
    return c2_loops_to_expolygons(c2_to_polygons(loops));
}

template<class TOut>
void c2_clip(C2::ClipType type, const C2::Paths64 &subject, const C2::Paths64 &clip, C2::FillRule fill, TOut &out)
{
    C2::Clipper64 clipper;
    // Clipper2 keeps collinear vertices by default, Clipper1 removed them.
    clipper.PreserveCollinear(false);
    clipper.AddSubject(subject);
    if (! clip.empty())
        clipper.AddClip(clip);
    clipper.Execute(type, fill, out);
}

Polylines c2_clip_open(C2::ClipType type, const C2::Paths64 &subject, const C2::Paths64 &clip)
{
    C2::Clipper64 clipper;
    clipper.PreserveCollinear(false);
    clipper.AddOpenSubject(subject);
    if (! clip.empty())
        clipper.AddClip(clip);
    C2::Paths64 closed, open;
    clipper.Execute(type, C2::FillRule::NonZero, closed, open);
    return c2_to_polylines(open);
}

// Clipper1 semantics: miter limit at least 2, arc tolerance (also for round caps) at most a quarter of the offset.
void c2_offset_setup(C2::ClipperOffset &co, float delta, JoinType joinType, double miterLimit)
{
    const double max_arc_tolerance = std::abs(delta) * 0.25;
    if (joinType == jtRound)
        co.ArcTolerance(miterLimit > 0. ? std::min(miterLimit, max_arc_tolerance) : 0.25);
    else {
        co.MiterLimit(std::max(miterLimit, 2.));
        co.ArcTolerance(std::min(0.25, max_arc_tolerance));
    }
}

// Whether the edges share a point: they cross, or an end of one lies on the other, or they are along one line and overlap.
static bool c2_edges_touch(const C2::Point64 &a0, const C2::Point64 &a1, const C2::Point64 &b0, const C2::Point64 &b1)
{
    const Vec2crd a[2] = { Vec2crd(a0.x, a0.y), Vec2crd(a1.x, a1.y) }, b[2] = { Vec2crd(b0.x, b0.y), Vec2crd(b1.x, b1.y) };
    const int     ends_of_b[2] = { int128::orient(a[0], a[1], b[0]), int128::orient(a[0], a[1], b[1]) };
    const int     ends_of_a[2] = { int128::orient(b[0], b[1], a[0]), int128::orient(b[0], b[1], a[1]) };
    if (ends_of_b[0] * ends_of_b[1] > 0 || ends_of_a[0] * ends_of_a[1] > 0)
        // The two ends of one edge lie strictly on the same side of the other one.
        return false;
    if (ends_of_b[0] != 0 || ends_of_b[1] != 0)
        return true;
    // Along one line: they share a point if they overlap.
    return std::max(a0.x, a1.x) >= std::min(b0.x, b1.x) && std::max(b0.x, b1.x) >= std::min(a0.x, a1.x) &&
           std::max(a0.y, a1.y) >= std::min(b0.y, b1.y) && std::max(b0.y, b1.y) >= std::min(a0.y, a1.y);
}

// Whether the boundaries of the two paths share a point: they cross, or they touch or coincide along a stretch.
static bool c2_paths_touch(const C2::Path64 &a, const C2::Path64 &b)
{
    for (size_t i = 0; i < a.size(); ++ i) {
        const C2::Point64 &a0 = a[i], &a1 = a[(i + 1) % a.size()];
        const int64_t      a_left = std::min(a0.x, a1.x), a_right = std::max(a0.x, a1.x), a_top = std::min(a0.y, a1.y), a_bottom = std::max(a0.y, a1.y);
        for (size_t j = 0; j < b.size(); ++ j) {
            const C2::Point64 &b0 = b[j], &b1 = b[(j + 1) % b.size()];
            // Only edges with bounding boxes around one another can share a point.
            if (std::max(b0.x, b1.x) < a_left || std::min(b0.x, b1.x) > a_right || std::max(b0.y, b1.y) < a_top || std::min(b0.y, b1.y) > a_bottom)
                continue;
            if (c2_edges_touch(a0, a1, b0, b1))
                return true;
        }
    }
    return false;
}

// A single group offsets like Clipper1's path by path offset only if every CW path is a hole of a CCW path. Such a path
// touches no other path, and lies in the solid of the paths around it. A clockwise contour in a notch of another one
// touches it, so it goes to the path by path offset with the whole batch, where it is offset as a contour of its own.
bool c2_offset_as_group(const C2::Paths64 &paths, const std::vector<double> &areas)
{
    std::vector<C2::Rect64> bounds;
    bounds.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        bounds.emplace_back(C2::GetBounds(path));
    // One tree over the bounding boxes of all the paths: a clockwise path looks at those around it only, not at every
    // path of the batch. The boxes of the tree are inflated for the traversal, they only have to be coarse.
    using Tree = AABBTreeIndirect::Tree<2, coord_t>;
    std::vector<AABBTreeIndirect::BoundingBoxWrapper> bboxes;
    bboxes.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++ i) {
        const C2::Rect64 &box = bounds[i];
        bboxes.emplace_back(i, BoundingBox(Point(coord_t(box.left), coord_t(box.top)), Point(coord_t(box.right), coord_t(box.bottom))));
    }
    Tree tree;
    tree.build_modify_input(bboxes);
    std::vector<size_t> candidates;
    for (size_t i = 0; i < paths.size(); ++ i) {
        if (areas[i] >= 0.)
            continue;
        // The paths this one touches or lies in: those whose bounding box meets its own, its own excluded.
        const C2::Rect64 &box = bounds[i];
        candidates.clear();
        const Tree::BoundingBox query(Point(coord_t(box.left), coord_t(box.top)), Point(coord_t(box.right), coord_t(box.bottom)));
        AABBTreeIndirect::traverse(tree,
            [&query](const Tree::Node &node) { return node.bbox.intersects(query); },
            [&candidates](const Tree::Node &node) { candidates.emplace_back(node.idx); return true; });
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
            [&bounds, &box, i](size_t j) { return j == i || ! bounds[j].Intersects(box); }), candidates.end());
        for (size_t j : candidates)
            if (c2_paths_touch(paths[i], paths[j]))
                return false;
        // No boundary of another path crosses this path, so all of it lies the same way to them: one vertex tells.
        const C2::Point64 &pt     = paths[i].front();
        int                winding = 0;
        for (size_t j : candidates)
            if (C2::PointInPolygon(pt, paths[j]) == C2::PointInPolygonResult::IsInside)
                winding += areas[j] > 0. ? 1 : -1;
        // Not in the solid of the other paths: offset it on its own.
        if (winding == 0)
            return false;
    }
    return true;
}

template<class TOut>
void c2_offset_closed_paths(C2::Paths64 &&paths, float delta, JoinType joinType, double miterLimit, TOut &out)
{
    std::vector<double> areas;
    areas.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        areas.emplace_back(C2::Area(path));
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    if (c2_offset_as_group(paths, areas)) {
        // Clipper2 would grow degenerate paths even for a negative offset.
        if (delta < 0. && std::all_of(areas.begin(), areas.end(), [](double a) { return a == 0.; }))
            return;
        co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
        co.Execute(delta, out);
        return;
    }
    C2::Paths64 offsetted, single;
    for (size_t i = 0; i < paths.size(); ++ i) {
        if (areas[i] == 0. && delta < 0.)
            continue;
        co.Clear();
        co.AddPath(paths[i], to_c2(joinType), C2::EndType::Polygon);
        // Clipper2 reverses a lone CW path, so the flipped sign shrinks it as a hole.
        co.Execute(areas[i] < 0. ? - delta : delta, single);
        append(offsetted, std::move(single));
    }
    c2_clip(C2::ClipType::Union, offsetted, {}, delta > 0. ? C2::FillRule::NonZero : C2::FillRule::Positive, out);
}

template<class PathsT, class TOut>
void c2_offset_closed(PathsT &&src, float delta, JoinType joinType, double miterLimit, TOut &out)
{
    const double shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64  paths;
    paths.reserve(src.size());
    for (const auto &path : src)
        append_offset_path(paths, path, shortest, C2::EndType::Polygon);
    c2_offset_closed_paths(std::move(paths), delta, joinType, miterLimit, out);
}

template<class PathsProvider, class TOut>
void c2_offset2_closed(PathsProvider &&src, float delta1, float delta2, JoinType joinType, double miterLimit, TOut &out)
{
    C2::Paths64 first;
    c2_offset_closed(std::forward<PathsProvider>(src), delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
}

inline const ExPolygon& expolygon_of(const ExPolygon &expoly) { return expoly; }
inline const ExPolygon& expolygon_of(const ExPolygon *expoly) { return *expoly; }
inline const ExPolygon& expolygon_of(const Surface &surface) { return surface.expolygon; }
inline const ExPolygon& expolygon_of(const Surface *surface) { return surface->expolygon; }

// Clipper1 ignored the orientation of expolygons, a single group needs CCW contours and CW holes.
template<class ExPolygonRange, class TOut>
void c2_offset_expolygons(const ExPolygonRange &src, float delta, JoinType joinType, double miterLimit, TOut &out)
{
    const double shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64  paths;
    for (const auto &item : src) {
        const ExPolygon &expoly = expolygon_of(item);
        if (! append_offset_path(paths, expoly.contour.points, shortest, C2::EndType::Polygon))
            continue;
        if (const double area = C2::Area(paths.back()); area == 0.) {
            // A degenerate contour vanishes when shrunk and encloses no hole when grown.
            if (delta < 0.)
                paths.pop_back();
            continue;
        } else if (area < 0.)
            std::reverse(paths.back().begin(), paths.back().end());
        for (const Polygon &hole : expoly.holes)
            if (append_offset_path(paths, hole.points, shortest, C2::EndType::Polygon)) {
                if (const double area = C2::Area(paths.back()); area == 0.)
                    paths.pop_back();
                else if (area > 0.)
                    std::reverse(paths.back().begin(), paths.back().end());
            }
    }
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
    co.Execute(delta, out);
}

template<class ExPolygonRange>
Polygons c2_expolygons_offset(const ExPolygonRange &src, float delta, JoinType joinType, double miterLimit)
{
    C2::Paths64 out;
    c2_offset_expolygons(src, delta, joinType, miterLimit, out);
    return c2_to_polygons(out);
}

template<class ExPolygonRange>
ExPolygons c2_expolygons_offset_ex(const ExPolygonRange &src, float delta, JoinType joinType, double miterLimit)
{
    C2::PolyTree64 out;
    c2_offset_expolygons(src, delta, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

template<class PathsProvider>
Polygons c2_offset_lines(PathsProvider &&src, float delta, JoinType joinType, double miterLimit, EndType endType)
{
    C2::Paths64 out;
    if (endType == etClosedPolygon) {
        c2_offset_closed(std::forward<PathsProvider>(src), delta, joinType, miterLimit, out);
        return c2_to_polygons(out);
    }
    const C2::EndType end      = to_c2(endType);
    const double      shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64       paths;
    paths.reserve(src.size());
    for (const Points &path : src)
        append_offset_path(paths, path, shortest, end);
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), end);
    co.Execute(delta, out);
    return c2_to_polygons(out);
}

template<class PathsProvider>
C2::Paths64 c2_clip_paths(PathsProvider &&clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(std::forward<PathsProvider>(clip));
    C2::Paths64 out;
    c2_offset_closed(std::forward<PathsProvider>(clip), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

} // namespace

Slic3r::Polygons offset(const Slic3r::Polygon &polygon, const float delta, JoinType joinType, double miterLimit)
{
    C2::Paths64 paths;
    if (! append_offset_path(paths, polygon.points, std::abs(delta * ClipperOffsetShortestEdgeFactor), C2::EndType::Polygon))
        return {};
    const double area = C2::Area(paths.front());
    if (area == 0. && delta < 0.)
        return {};
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
    C2::Paths64 out;
    // A CW polygon shrinks as a hole and stays CW.
    co.Execute(area < 0. ? - delta : delta, out);
    return c2_to_polygons(out);
}

Slic3r::Polygons offset(const Slic3r::Polygons &polygons, const float delta, JoinType joinType, double miterLimit)
{
    C2::Paths64 out;
    c2_offset_closed(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::ExPolygons offset_ex(const Slic3r::Polygons &polygons, const float delta, JoinType joinType, double miterLimit)
{
    C2::PolyTree64 out;
    c2_offset_closed(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

Slic3r::Polygons offset(const Slic3r::Polyline &polyline, const float delta, JoinType joinType, double miterLimit, EndType end_type)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::SinglePathProvider(polyline.points), delta, joinType, miterLimit, end_type); }

Slic3r::Polygons offset(const Slic3r::Polylines &polylines, const float delta, JoinType joinType, double miterLimit, EndType end_type)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::PolylinesProvider(polylines), delta, joinType, miterLimit, end_type); }

Polygons contour_to_polygons(const Polygon &polygon, const float line_width, JoinType join_type, double miter_limit)
    { assert(line_width > 1.f); return c2_offset_lines(ClipperUtils::SinglePathProvider(polygon.points), line_width / 2, join_type, miter_limit, etClosedLine); }
Polygons contour_to_polygons(const Polygons &polygons, const float line_width, JoinType join_type, double miter_limit)
    { assert(line_width > 1.f); return c2_offset_lines(ClipperUtils::PolygonsProvider(polygons), line_width / 2, join_type, miter_limit, etClosedLine); }

Slic3r::Polygons offset(const Slic3r::ExPolygon &expolygon, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(std::array<const ExPolygon*, 1>{ &expolygon }, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::ExPolygons &expolygons, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(expolygons, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::Surfaces &surfaces, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(surfaces, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::SurfacesPtr &surfaces, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(surfaces, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygon &expolygon, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(std::array<const ExPolygon*, 1>{ &expolygon }, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygons &expolygons, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(expolygons, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::Surfaces &surfaces, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::SurfacesPtr &surfaces, const float delta, JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }

Polygons offset2(const ExPolygons &expolygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    C2::Paths64 first, out;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
ExPolygons offset2_ex(const ExPolygons &expolygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    C2::Paths64    first;
    C2::PolyTree64 out;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}
ExPolygons offset2_ex(const Surfaces &surfaces, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    C2::Paths64    first;
    C2::PolyTree64 out;
    c2_offset_expolygons(surfaces, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

// Offset outside, then inside produces morphological closing. All deltas should be positive.
Slic3r::Polygons closing(const Slic3r::Polygons &polygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::ExPolygons closing_ex(const Slic3r::Polygons &polygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::PolyTree64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}
Slic3r::ExPolygons closing_ex(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::PolyTree64 out;
    c2_offset2_closed(ClipperUtils::SurfacesProvider(surfaces), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

// Offset inside, then outside produces morphological opening. All deltas should be positive.
Slic3r::Polygons opening(const Slic3r::Polygons &polygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::Polygons opening(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::ExPolygonsProvider(expolygons), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::Polygons opening(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::SurfacesProvider(surfaces), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}

template<class TSubj, class TClip>
static inline Polygons _clipper(ClipType clipType, TSubj &&subject, TClip &&clip, ApplySafetyOffset do_safety_offset)
{
    // Safety offset only allowed on intersection and difference.
    assert(do_safety_offset == ApplySafetyOffset::No || clipType != ctUnion);
    C2::Paths64 out;
    c2_clip(to_c2(clipType), to_paths64(std::forward<TSubj>(subject)), c2_clip_paths(std::forward<TClip>(clip), do_safety_offset), C2::FillRule::NonZero, out);
    return c2_to_polygons(out);
}

Slic3r::Polygons diff(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return diff(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons & subject, const Slic3r::ExPolygons & clip, ApplySafetyOffset do_safety_offset)
{
    return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset);
}
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons intersection_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return intersection(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, PolyFillType fill_type)
{
    C2::Paths64 out;
    c2_clip(C2::ClipType::Intersection, to_paths64(ClipperUtils::PolygonsProvider(subject)), to_paths64(ClipperUtils::PolygonsProvider(clip)), to_c2(fill_type), out);
    return c2_to_polygons(out);
}
Slic3r::Polygons intersection(const Slic3r::ExPolygon &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
Slic3r::Polygons intersection(const Slic3r::Polygons& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons clip_temp;
    clip_temp.push_back(clip);
    return intersection(subject, clip_temp, do_safety_offset);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject)
    { return _clipper(ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::ExPolygons &subject)
    { return _clipper(ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const PolyFillType fillType)
{
    C2::Paths64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, to_c2(fillType), out);
    return c2_to_polygons(out);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject, const Slic3r::Polygons &subject2)
    {
        // BBS
        Polygons polys = subject;
        for (const Polygon& poly : subject2)
            polys.push_back(poly);
        return union_(polys);
    }

// Clipper2 builds the PolyTree in one pass without Clipper1's slowdown on shared edges (#117).
template <typename TSubject, typename TClip>
static ExPolygons _clipper_ex(ClipType clipType, TSubject &&subject,  TClip &&clip, ApplySafetyOffset do_safety_offset, PolyFillType fill_type = pftNonZero)
{
    assert(do_safety_offset == ApplySafetyOffset::No || clipType != ctUnion);
    C2::PolyTree64 out;
    c2_clip(to_c2(clipType), to_paths64(std::forward<TSubject>(subject)), c2_clip_paths(std::forward<TClip>(clip), do_safety_offset), to_c2(fill_type), out);
    return c2_to_expolygons(out);
}

Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygon &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygons& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    subject_temp.push_back(subject);

    return diff_ex(subject_temp, clip, do_safety_offset);
}

inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    Slic3r::Polygons clip_temp;

    subject_temp.push_back(subject);
    clip_temp.push_back(clip);
    return diff_ex(subject_temp, clip_temp, do_safety_offset);
}

Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygons& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ctIntersection, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
namespace ClipperUtils {
    // Between two consecutive heights of its vertices the polygon is a set of trapezoids, and the line through the middle
    // of that band meets no vertex and no horizontal edge: its crossings with the edges alternate into and out of the
    // polygon, and there each trapezoid is at least half as wide as at its widest. So the bands are tried widest first,
    // until the middle of the widest span on that line rounds to a point strictly inside. A band under two units has no
    // grid line inside it.
    std::optional<Point> interior_point(const Polygon &polygon)
    {
        std::vector<coord_t> heights;
        heights.reserve(polygon.size());
        for (const Point &pt : polygon.points)
            heights.emplace_back(pt.y());
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
        std::vector<std::pair<coord_t, coord_t>> bands;
        for (size_t i = 1; i < heights.size(); ++ i)
            if (heights[i] - heights[i - 1] >= 2)
                bands.emplace_back(heights[i - 1], heights[i]);
        std::sort(bands.begin(), bands.end(), [](const auto &a, const auto &b) { return a.second - a.first > b.second - b.first; });
        std::vector<double> crossings;
        for (const auto &[low, high] : bands) {
            const coord_t y = low + (high - low) / 2;
            crossings.clear();
            for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i ++) {
                const Point &a = polygon.points[j];
                const Point &b = polygon.points[i];
                if ((a.y() > y) != (b.y() > y))
                    crossings.emplace_back(double(a.x()) + double(y - a.y()) * double(b.x() - a.x()) / double(b.y() - a.y()));
            }
            std::sort(crossings.begin(), crossings.end());
            // Inside from the first crossing to the second, from the third to the fourth...
            double left = 0., right = 0.;
            for (size_t i = 1; i < crossings.size(); i += 2)
                if (crossings[i] - crossings[i - 1] > right - left) {
                    left  = crossings[i - 1];
                    right = crossings[i];
                }
            if (const Point pt(coord_t(std::round(0.5 * (left + right))), y); right > left && contains(polygon, pt, false))
                return pt;
        }
        return std::nullopt;
    }
}
// May be used to "heal" unusual models (3DLabPrints etc.) by providing fill_type (pftEvenOdd, pftNonZero, pftPositive, pftNegative).
Slic3r::ExPolygons union_ex(const Slic3r::Polygons &subject, PolyFillType fill_type)
    { return _clipper_ex(ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No, fill_type); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject)
    { return _clipper_ex(ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &subject2)
    { return _clipper_ex(ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(subject2), ApplySafetyOffset::No); }
Slic3r::ExPolygons union_ex(const Slic3r::Surfaces &subject)
    { return _clipper_ex(ctUnion, ClipperUtils::SurfacesProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }

// BBS
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons& poly1, const Slic3r::ExPolygons& poly2, bool safety_offset_)
    {
    ExPolygons expolys = poly1;
    for (const ExPolygon& expoly : poly2)
        expolys.push_back(expoly);
    return union_ex(expolys);
}

Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);
}
Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);
}

template<typename PathsProvider1, typename PathsProvider2>
Polylines _clipper_pl_open(ClipType clipType, PathsProvider1 &&subject, PathsProvider2 &&clip)
    { return c2_clip_open(to_c2(clipType), to_paths64(std::forward<PathsProvider1>(subject)), to_paths64(std::forward<PathsProvider2>(clip))); }

// If the split_at_first_point() call above happens to split the polygon inside the clipping area
// we would get two consecutive polylines instead of a single one, so we go through them in order
// to recombine continuous polylines.
static void _clipper_pl_recombine(Polylines &polylines)
{
    for (size_t i = 0; i < polylines.size(); ++i) {
        for (size_t j = i+1; j < polylines.size(); ++j) {
            if (polylines[i].points.back() == polylines[j].points.front()) {
                /* If last point of i coincides with first point of j,
                   append points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.back()) {
                /* If first point of i coincides with last point of j,
                   prepend points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.front()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when first point of i coincides with first point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.back() == polylines[j].points.back()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when last point of i coincides with last point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            }
        }
    }
}

template<typename PathProvider1, typename PathProvider2>
Polylines _clipper_pl_closed(ClipType clipType, PathProvider1 &&subject, PathProvider2 &&clip)
{
    // Transform input polygons into open paths.
    C2::Paths64 paths;
    paths.reserve(subject.size());
    for (const Points &poly : subject) {
        // Emplace polygon, duplicate the 1st point.
        C2::Path64 &path = paths.emplace_back();
        path.reserve(poly.size() + 1);
        for (const Point &pt : poly)
            path.emplace_back(pt.x(), pt.y());
        if (! poly.empty())
            path.emplace_back(poly.front().x(), poly.front().y());
    }
    // perform clipping
    Polylines retval = c2_clip_open(to_c2(clipType), paths, to_paths64(std::forward<PathProvider2>(clip)));
    _clipper_pl_recombine(retval);
    return retval;
}

Slic3r::Polylines diff_pl(const Slic3r::Polyline& subject, const Slic3r::Polygons& clip)
    { return _clipper_pl_open(ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygon &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::SinglePathProvider(clip.points)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }

// Orca: Sort and orient open polyline fragments produced by clipping `source` with
// intersection_pl(), so that they run in the same order and direction as the source
// polyline. Clipping creates new endpoints at the clip boundary, but it keeps the
// interior source vertices intact, so a fragment's position on the source path is
// recovered exactly by looking its vertices up in the source. Fragments without any
// surviving source vertex lie on a single source segment, found by a nearest-segment
// search.
void restore_source_path_order(const Slic3r::Polyline &source, Slic3r::Polylines &fragments)
{
    const Points &src = source.points;
    if (src.size() < 2 || fragments.empty())
        return;

    std::unordered_map<Point, size_t, PointHash> source_index;
    source_index.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++ i)
        source_index.emplace(src[i], i);

    // Sort key: index of the source vertex where the fragment starts, then the signed
    // offset of the fragment's start from that vertex, to order multiple fragments cut
    // from one long source segment.
    std::vector<std::pair<size_t, double>> keys(fragments.size());
    for (size_t n = 0; n < fragments.size(); ++ n) {
        Polyline    &pl    = fragments[n];
        const size_t npos  = size_t(-1);
        size_t       front = npos;
        size_t       back  = npos;
        for (const Point &pt : pl.points)
            if (auto it = source_index.find(pt); it != source_index.end()) {
                front = it->second;
                break;
            }
        for (auto i = pl.points.rbegin(); i != pl.points.rend(); ++ i)
            if (auto it = source_index.find(*i); it != source_index.end()) {
                back = it->second;
                break;
            }
        Vec2crd source_dir;
        if (front == npos) {
            // All vertices were created by clipping, thus the whole fragment lies on a
            // single source segment. Find that segment.
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i + 1 < src.size(); ++ i)
                if (double d = Line::distance_to_squared(pl.first_point(), src[i], src[i + 1]); d < best) {
                    best  = d;
                    front = i;
                }
            back       = front;
            source_dir = src[front + 1] - src[front];
        } else
            source_dir = src[std::min(back + 1, src.size() - 1)] - src[front > 0 ? front - 1 : 0];
        if (front > back) {
            pl.reverse();
            std::swap(front, back);
        } else if (front == back &&
                   (pl.last_point() - pl.first_point()).cast<double>().dot(source_dir.cast<double>()) < 0.)
            pl.reverse();
        const Vec2crd seg = src[std::min(front + 1, src.size() - 1)] - src[front];
        keys[n] = { front, (pl.first_point() - src[front]).cast<double>().dot(seg.cast<double>()) };
    }

    std::vector<size_t> order(fragments.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&keys](size_t a, size_t b) { return keys[a] < keys[b]; });
    Polylines sorted;
    sorted.reserve(fragments.size());
    for (size_t n : order)
        sorted.emplace_back(std::move(fragments[n]));
    fragments = std::move(sorted);
}

Lines _clipper_ln(ClipType clipType, const Lines &subject, const Polygons &clip)
{
    // convert Lines to Polylines
    Polylines polylines;
    polylines.reserve(subject.size());
    for (const Line &line : subject)
        polylines.emplace_back(Polyline(line.a, line.b));
    
    // perform operation
    polylines = _clipper_pl_open(clipType, ClipperUtils::PolylinesProvider(polylines), ClipperUtils::PolygonsProvider(clip));
    
    // convert Polylines to Lines
    Lines retval;
    for (Polylines::const_iterator polyline = polylines.begin(); polyline != polylines.end(); ++polyline)
        if (polyline->size() >= 2)
            //FIXME It may happen, that Clipper produced a polyline with more than 2 collinear points by clipping a single line with polygons. It is a very rare issue, but it happens, see GH #6933.
            retval.push_back({ polyline->front(), polyline->back() });
    return retval;
}

// Chains the loops of one level of the tree, each followed by the loops under it, depth first. Clipper2 may put a loop
// beside the loop around it instead of under it (see c2_append_expolygons()), out of the nesting order the callers rely
// on: the spiral of FillSpiralInset continues only into a loop inside the last one. So a loop whose orientation doesn't
// fit its level (CCW under the top or a CW loop, CW under a CCW loop) goes under the smallest loop of the level around
// it, the one Clipper2 failed to put it under, and stays where it is if none is.
static void traverse_pt_outside_in(const std::vector<const C2::PolyPath64*> &nodes, bool holes, Polygons *retval)
{
    struct Loop
    {
        const C2::PolyPath64              *node;
        double                             area;
        // The loops to traverse under this one: its children, then the misplaced loops it takes in.
        std::vector<const C2::PolyPath64*> next;
        bool                               moved = false;
    };
    std::vector<Loop>   loops;
    std::vector<size_t> fitting, misfits;
    loops.reserve(nodes.size());
    for (const C2::PolyPath64 *node : nodes) {
        Loop &loop = loops.emplace_back(Loop{ node, C2::Area(node->Polygon()), {} });
        loop.next.reserve(node->Count());
        for (const auto &child : *node)
            loop.next.emplace_back(child.get());
        ((loop.area < 0.) == holes ? fitting : misfits).emplace_back(loops.size() - 1);
    }
    if (! misfits.empty()) {
        // Candidates are tried smallest first, from the first one larger than the misplaced loop (see path_encloses()).
        std::sort(fitting.begin(), fitting.end(), [&loops](size_t a, size_t b) { return std::abs(loops[a].area) < std::abs(loops[b].area); });
        std::vector<C2::Rect64> bounds(loops.size());
        for (size_t i = 0; i < loops.size(); ++ i)
            bounds[i] = C2::GetBounds(loops[i].node->Polygon());
        for (size_t misfit : misfits) {
            Loop             &loop = loops[misfit];
            const C2::Rect64 &box  = bounds[misfit];
            Polygon           inner;
            inner.points = c2_to_points(loop.node->Polygon());
            for (size_t idx : fitting) {
                // The slack of a vertex rounded across the candidate, as in c2_attach_stray_holes().
                const C2::Rect64 &around = bounds[idx];
                if (std::abs(loops[idx].area) <= std::abs(loop.area) || around.left - SCALED_EPSILON > box.left ||
                    around.right + SCALED_EPSILON < box.right || around.top - SCALED_EPSILON > box.top || around.bottom + SCALED_EPSILON < box.bottom)
                    continue;
                Polygon outer;
                outer.points = c2_to_points(loops[idx].node->Polygon());
                if (path_encloses(outer, inner)) {
                    loops[idx].next.emplace_back(loop.node);
                    loop.moved = true;
                    break;
                }
            }
        }
    }

    // Chain the loops staying on this level, push results recursively.
    std::vector<size_t> staying;
    Points              ordering_points;
    for (size_t i = 0; i < loops.size(); ++ i)
        if (! loops[i].moved) {
            staying.emplace_back(i);
            ordering_points.emplace_back(loops[i].node->Polygon().front().x, loops[i].node->Polygon().front().y);
        }
    //FIXME pass the last point to chain_points?
    for (size_t idx : chain_points(ordering_points)) {
        const Loop &loop = loops[staying[idx]];
        retval->emplace_back().points = c2_to_points(loop.node->Polygon());
        if (loop.area < 0.)
            // Orient a hole, which is clockwise oriented, to CCW.
            retval->back().reverse();
        traverse_pt_outside_in(loop.next, loop.area > 0., retval);
    }
}

Polygons union_pt_chained_outside_in(const Polygons &subject)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::EvenOdd, tree);
    std::vector<const C2::PolyPath64*> top_level;
    top_level.reserve(tree.Count());
    for (const auto &node : tree)
        top_level.emplace_back(node.get());
    Polygons retval;
    traverse_pt_outside_in(top_level, false, &retval);
    return retval;
}

// Clipper2 unions are strictly simple, like Clipper1's with StrictlySimple(true), but keep a contour that touches itself
// at a vertex in one piece where Clipper1 split it into two.
Polygons simplify_polygons(const Polygons &subject)
{
    C2::Paths64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, out);
    c2_split_repeated_vertices(out);
    return c2_to_polygons(out);
}

ExPolygons simplify_polygons_ex(const Polygons &subject)
{
    C2::PolyTree64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, out);
    return c2_to_expolygons_split(out);
}

Polygons top_level_islands(const Slic3r::Polygons &polygons)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(polygons)), {}, C2::FillRule::EvenOdd, tree);
    // Convert only the top level islands to the output.
    Polygons out;
    out.reserve(tree.Count());
    for (const auto &island : tree)
        out.emplace_back().points = c2_to_points(island->Polygon());
    return out;
}

ExPolygons top_level_expolygons(const ExPolygons &expolygons, ExPolygons *nested)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::ExPolygonsProvider(expolygons)), {}, C2::FillRule::EvenOdd, tree);
    ExPolygons out;
    out.reserve(tree.Count());
    const size_t                   first_nested = nested ? nested->size() : 0;
    std::vector<const C2::Path64*> stray_holes;
    for (const auto &outer : tree) {
        ExPolygon &expoly = out.emplace_back();
        expoly.contour.points = c2_to_points(outer->Polygon());
        for (const auto &hole : *outer) {
            expoly.holes.emplace_back().points = c2_to_points(hole->Polygon());
            if (nested)
                for (const auto &island : *hole)
                    c2_append_expolygons(*island, *nested, stray_holes);
        }
    }
    if (nested)
        c2_attach_stray_holes(*nested, first_nested, stray_holes);
    return out;
}

Points mittered_offset_path_scaled(const Points &contour, const std::vector<float> &deltas, double miter_limit)
{
	assert(contour.size() == deltas.size());

#ifndef NDEBUG
	// Verify that the deltas are either all positive, or all negative.
	bool positive = false;
	bool negative = false;
	for (float delta : deltas)
		if (delta < 0.f)
			negative = true;
		else if (delta > 0.f)
			positive = true;
	assert(! (negative && positive));
#endif /* NDEBUG */

	Points out;

	if (deltas.size() > 2)
	{
		out.reserve(contour.size() * 2);

		// Clamp miter limit to 2.
		miter_limit = (miter_limit > 2.) ? 2. / (miter_limit * miter_limit) : 0.5;
		
		// perpenduclar vector
		auto   perp = [](const Vec2d &v) -> Vec2d { return Vec2d(v.y(), - v.x()); };

		// Add a new point to the output, rounded to coord_t.
		auto   add_offset_point = [&out](Vec2d pt) {
            pt += Vec2d(0.5 - (pt.x() < 0), 0.5 - (pt.y() < 0));
			out.emplace_back(coord_t(pt.x()), coord_t(pt.y()));
		};

		// Minimum edge length, squared.
		double lmin  = *std::max_element(deltas.begin(), deltas.end()) * ClipperOffsetShortestEdgeFactor;
		double l2min = lmin * lmin;
		// Minimum angle to consider two edges to be parallel.
		// Vojtech's estimate.
//		const double sin_min_parallel = EPSILON + 1. / double(CLIPPER_OFFSET_SCALE);
		// Implementation equal to Clipper.
		const double sin_min_parallel = 1.;

		// Find the last point further from pt by l2min.
		Vec2d  pt     = contour.front().cast<double>();
		size_t iprev  = contour.size() - 1;
		Vec2d  ptprev;
		for (; iprev > 0; -- iprev) {
			ptprev = contour[iprev].cast<double>();
			if ((ptprev - pt).squaredNorm() > l2min)
				break;
		}

		if (iprev != 0) {
			size_t ilast = iprev;
			// Normal to the (pt - ptprev) segment.
			Vec2d nprev = perp(pt - ptprev).normalized();
			for (size_t i = 0; ; ) {
				// Find the next point further from pt by l2min.
				size_t j = i + 1;
				Vec2d ptnext;
				for (; j <= ilast; ++ j) {
					ptnext = contour[j].cast<double>();
					double l2 = (ptnext - pt).squaredNorm();
					if (l2 > l2min)
						break;
				}
				if (j > ilast) {
					assert(i <= ilast);
					// If the last edge is too short, merge it with the previous edge.
					i = ilast;
					ptnext = contour.front().cast<double>();
				}

				// Normal to the (ptnext - pt) segment.
				Vec2d nnext  = perp(ptnext - pt).normalized();

				double delta  = deltas[i];
				double sin_a  = std::clamp(cross2(nprev, nnext), -1., 1.);
				double convex = sin_a * delta;
				if (convex <= - sin_min_parallel) {
					// Concave corner.
					add_offset_point(pt + nprev * delta);
					add_offset_point(pt);
					add_offset_point(pt + nnext * delta);
				} else {
					double dot = nprev.dot(nnext);
					if (convex < sin_min_parallel && dot > 0.) {
						// Nearly parallel.
						add_offset_point((nprev.dot(nnext) > 0.) ? (pt + nprev * delta) : pt);
					} else {
						// Convex corner, possibly extremely sharp if convex < sin_min_parallel.
						double r = 1. + dot;
					  	if (r >= miter_limit)
							add_offset_point(pt + (nprev + nnext) * (delta / r));
					  	else {
							double dx = std::tan(std::atan2(sin_a, dot) / 4.);
							Vec2d  newpt1 = pt + (nprev - perp(nprev) * dx) * delta;
							Vec2d  newpt2 = pt + (nnext + perp(nnext) * dx) * delta;
#ifndef NDEBUG
							Vec2d vedge = 0.5 * (newpt1 + newpt2) - pt;
							double dist_norm = vedge.norm();
							assert(std::abs(dist_norm - std::abs(delta)) < SCALED_EPSILON);
#endif /* NDEBUG */
							add_offset_point(newpt1);
							add_offset_point(newpt2);
					  	}
					}
				}

				if (i == ilast)
					break;

				ptprev = pt;
				nprev  = nnext;
				pt     = ptnext;
				i = j;
			}
		}
	}

	return out;
}

// Clipper1 united the raw contour offsets with the positive rule and the raw hole offsets with the negative rule.
template<class TOut>
static void variable_offset(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit, TOut &out)
{
	auto unite = [](const Points &raw, C2::FillRule fill) {
		C2::Paths64 united;
		if (! raw.empty())
			c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::SinglePathProvider(raw)), {}, fill, united);
		return united;
	};

	// 1) Offset the outer contour.
	C2::Paths64 contours = unite(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), C2::FillRule::Positive);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(C2::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	C2::Paths64 holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, unite(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), C2::FillRule::Negative));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(C2::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	c2_clip(C2::ClipType::Difference, contours, holes, C2::FillRule::NonZero, out);
}

Polygons variable_offset_inner(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float> &ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	C2::Paths64 out;
	variable_offset(expoly, deltas, miter_limit, out);
	return c2_to_polygons(out);
}

Polygons variable_offset_outer(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non negative.
	for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	C2::Paths64 out;
	variable_offset(expoly, deltas, miter_limit, out);
	return c2_to_polygons(out);
}

ExPolygons variable_offset_outer_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non negative.
	for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	C2::PolyTree64 out;
	variable_offset(expoly, deltas, miter_limit, out);
	return c2_to_expolygons(out);
}

ExPolygons variable_offset_inner_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	C2::PolyTree64 out;
	variable_offset(expoly, deltas, miter_limit, out);
	return c2_to_expolygons(out);
}

Pointfs make_counter_clockwise(const Pointfs& pointfs)
{
    Pointfs ps = pointfs;
    if (Polygon::new_scale(pointfs).is_clockwise()) {
        std::reverse(ps.begin(), ps.end());
    }

    return ps;
}

}
