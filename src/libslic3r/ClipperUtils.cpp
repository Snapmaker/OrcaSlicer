#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "AABBTreeIndirect.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Int128.hpp"
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

inline coord_t clamp_to_coord(int64_t v)
{
    return coord_t(std::clamp<int64_t>(v, std::numeric_limits<coord_t>::min(), std::numeric_limits<coord_t>::max()));
}

struct C2BBoxNode
{
    size_t                                         m_idx;
    AABBTreeIndirect::Tree<2, coord_t>::BoundingBox m_bbox;
    Vec2crd                                        m_centroid;

    C2BBoxNode(size_t i, const C2::Rect64 &r, int64_t pad = 0)
        : m_idx(i)
        , m_bbox(Vec2crd(clamp_to_coord(r.left - pad), clamp_to_coord(r.top - pad)),
                 Vec2crd(clamp_to_coord(r.right + pad), clamp_to_coord(r.bottom + pad)))
        , m_centroid(clamp_to_coord(r.left + (r.right - r.left) / 2), clamp_to_coord(r.top + (r.bottom - r.top) / 2))
    {}

    size_t                                                idx() const { return m_idx; }
    const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox &bbox() const { return m_bbox; }
    const Vec2crd                                         &centroid() const { return m_centroid; }
};

// Exact point-in-path test using 128-bit orientation: 1 inside, 0 outside, -1 on boundary.
int c2_point_in_path(const C2::Point64 &pt, const C2::Path64 &path)
{
    const size_t cnt = path.size();
    if (cnt < 3)
        return 0;
    int result = 0;
    const Vec2crd p(pt.x, pt.y);
    C2::Point64   ip = path[0];
    for (size_t i = 1; i <= cnt; ++i) {
        const C2::Point64 ip_next = (i == cnt) ? path[0] : path[i];
        if (ip_next.y == pt.y &&
            (ip_next.x == pt.x || (ip.y == pt.y && ((ip_next.x > pt.x) == (ip.x < pt.x)))))
            return -1;
        if ((ip.y < pt.y) != (ip_next.y < pt.y)) {
            if (ip.x >= pt.x) {
                if (ip_next.x > pt.x)
                    result = 1 - result;
                else {
                    const int sign = int128::orient(p, Vec2crd(ip.x, ip.y), Vec2crd(ip_next.x, ip_next.y));
                    if (! sign)
                        return -1;
                    if ((sign > 0) == (ip_next.y > ip.y))
                        result = 1 - result;
                }
            } else if (ip_next.x > pt.x) {
                const int sign = int128::orient(p, Vec2crd(ip.x, ip.y), Vec2crd(ip_next.x, ip_next.y));
                if (! sign)
                    return -1;
                if ((sign > 0) == (ip_next.y > ip.y))
                    result = 1 - result;
            }
        }
        ip = ip_next;
    }
    return result;
}

template<class PtT, class PathT>
bool point_near_boundary_templ(const PtT &pt, const PathT &outer, int64_t tol, double tol2)
{
    const size_t n = outer.size();
    const int64_t ptx = px(pt), pty = py(pt);
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto   &a  = outer[j];
        const auto   &b  = outer[i];
        const int64_t ax = px(a), ay = py(a);
        const int64_t bx = px(b), by = py(b);
        if (ptx < std::min(ax, bx) - tol || ptx > std::max(ax, bx) + tol ||
            pty < std::min(ay, by) - tol || pty > std::max(ay, by) + tol)
            continue;
        const double dx   = double(bx - ax);
        const double dy   = double(by - ay);
        const double qx   = double(ptx - ax);
        const double qy   = double(pty - ay);
        const double len2 = dx * dx + dy * dy;
        double       d2;
        if (len2 == 0.) {
            d2 = qx * qx + qy * qy;
        } else {
            const double t  = std::clamp((qx * dx + qy * dy) / len2, 0., 1.);
            const double rx = qx - t * dx;
            const double ry = qy - t * dy;
            d2 = rx * rx + ry * ry;
        }
        if (d2 <= tol2)
            return true;
    }
    return false;
}

template<class PathT>
bool point_in_path_double_templ(double px_val, double py_val, const PathT &path)
{
    bool         inside = false;
    const size_t n      = path.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = double(py(path[i]));
        const double yj = double(py(path[j]));
        if ((yi > py_val) != (yj > py_val)) {
            const double xi = double(px(path[i]));
            const double xj = double(px(path[j]));
            if ((px_val - xi) < (py_val - yi) * (xj - xi) / (yj - yi))
                inside = ! inside;
        }
    }
    return inside;
}

template<class PathT>
bool find_interior_point_templ(const PathT &path, double &mx, double &my)
{
    const size_t n = path.size();
    if (n < 3)
        return false;
    std::vector<int64_t> ys;
    ys.reserve(n);
    for (const auto &p : path)
        ys.push_back(py(p));
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    if (ys.size() < 2)
        return false;

    std::vector<size_t> slabs(ys.size() - 1);
    std::iota(slabs.begin(), slabs.end(), size_t(0));
    std::sort(slabs.begin(), slabs.end(), [&ys](size_t a, size_t b) {
        return (ys[a + 1] - ys[a]) > (ys[b + 1] - ys[b]);
    });

    double              best_span = 0.;
    bool                found     = false;
    std::vector<double> xs;
    for (size_t s : slabs) {
        const double y_mid = double(ys[s]) + 0.5 * double(ys[s + 1] - ys[s]);
        xs.clear();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double yi = double(py(path[i]));
            const double yj = double(py(path[j]));
            if ((yi < y_mid && yj > y_mid) || (yj < y_mid && yi > y_mid)) {
                const double xi = double(px(path[i]));
                const double xj = double(px(path[j]));
                xs.push_back(xi + (y_mid - yi) * (xj - xi) / (yj - yi));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const double span = xs[k + 1] - xs[k];
            if (span > best_span) {
                best_span = span;
                mx        = xs[k] + 0.5 * span;
                my        = y_mid;
                found     = true;
            }
        }
        if (found && best_span > 6.)
            return true;
    }
    return found;
}

bool c2_path_encloses(const C2::Path64 &outer, const C2::Path64 &inner)
{
    constexpr int64_t kTol  = 3;
    constexpr double  kTol2 = double(kTol * kTol);
    for (const C2::Point64 &pt : inner) {
        const int pip = c2_point_in_path(pt, outer);
        if (pip == -1)
            continue;
        if (! point_near_boundary_templ(pt, outer, kTol, kTol2))
            return pip == 1;
    }
    double mx = 0., my = 0.;
    if (find_interior_point_templ(inner, mx, my))
        return point_in_path_double_templ(mx, my, outer);
    size_t inside_cnt = 0;
    for (const C2::Point64 &pt : inner)
        if (c2_point_in_path(pt, outer) == 1)
            ++inside_cnt;
    return inside_cnt * 2 > inner.size();
}

Int128 c2_twice_signed_area_128(const C2::Path64 &path)
{
    Int128       acc(0);
    const size_t n = path.size();
    if (n < 3)
        return acc;
    const C2::Point64 &p0 = path[0];
    for (size_t i = 1; i + 1 < n; ++i) {
        const int64_t dx1 = path[i].x - p0.x;
        const int64_t dy1 = path[i].y - p0.y;
        const int64_t dx2 = path[i + 1].x - p0.x;
        const int64_t dy2 = path[i + 1].y - p0.y;
        acc += Int128::multiply(dx1, dy2) - Int128::multiply(dy1, dx2);
    }
    return acc;
}

double c2_exact_area(const C2::Path64 &path)
{
    return 0.5 * double(c2_twice_signed_area_128(path));
}

int c2_exact_area_sign(const C2::Path64 &path)
{
    return c2_twice_signed_area_128(path).sign();
}

bool c2_is_collinear_path(const C2::Path64 &path)
{
    if (path.size() < 3)
        return true;
    const Vec2crd p0(path[0].x, path[0].y);
    size_t        i = 1;
    while (i < path.size() && path[i].x == path[0].x && path[i].y == path[0].y)
        ++i;
    if (i >= path.size())
        return true;
    const Vec2crd p1(path[i].x, path[i].y);
    for (++i; i < path.size(); ++i)
        if (int128::orient(p0, p1, Vec2crd(path[i].x, path[i].y)) != 0)
            return false;
    return true;
}

bool c2_polytree_is_well_nested(const C2::PolyPath64 &node, unsigned depth)
{
    const int s = c2_exact_area_sign(node.Polygon());
    if ((depth % 2 == 0) ? (s <= 0) : (s >= 0))
        return false;
    for (const auto &child : node)
        if (! c2_polytree_is_well_nested(*child, depth + 1))
            return false;
    return true;
}

bool c2_polytree_is_well_nested(const C2::PolyTree64 &tree)
{
    for (const auto &child : tree)
        if (! c2_polytree_is_well_nested(*child, 0))
            return false;
    return true;
}

struct C2RebuiltNode
{
    const C2::Path64   *path   = nullptr;
    double              area   = 0.;
    C2::Rect64          bounds;
    size_t              parent = size_t(-1);
    std::vector<size_t> children;
};

struct C2RebuiltTree
{
    std::vector<C2RebuiltNode> nodes;
    std::vector<size_t>        roots;
};

void c2_collect_polytree_paths(const C2::PolyPath64 &node, std::vector<C2RebuiltNode> &items)
{
    const C2::Path64 &poly = node.Polygon();
    if (poly.size() >= 3) {
        const double a = c2_exact_area(poly);
        if (a != 0.) {
            C2RebuiltNode item;
            item.path   = &poly;
            item.area   = a;
            item.bounds = C2::GetBounds(poly);
            items.emplace_back(std::move(item));
        }
    }
    for (const auto &child : node)
        c2_collect_polytree_paths(*child, items);
}

// Reconstruct exact containment hierarchy when Clipper2 PolyTree64 has misnested nodes.
C2RebuiltTree c2_rebuild_polytree(const C2::PolyTree64 &tree)
{
    C2RebuiltTree rebuilt;
    for (const auto &child : tree)
        c2_collect_polytree_paths(*child, rebuilt.nodes);
    if (rebuilt.nodes.empty())
        return rebuilt;

    std::sort(rebuilt.nodes.begin(), rebuilt.nodes.end(), [](const C2RebuiltNode &a, const C2RebuiltNode &b) {
        return std::abs(a.area) > std::abs(b.area);
    });

    constexpr int64_t kPad = 3;
    std::vector<C2BBoxNode> bbox_nodes;
    bbox_nodes.reserve(rebuilt.nodes.size());
    for (size_t i = 0; i < rebuilt.nodes.size(); ++i)
        bbox_nodes.emplace_back(i, rebuilt.nodes[i].bounds, kPad);

    AABBTreeIndirect::Tree<2, coord_t> aabb;
    aabb.build(std::move(bbox_nodes));

    std::vector<size_t> candidates;
    for (size_t i = 0; i < rebuilt.nodes.size(); ++i) {
        const C2RebuiltNode &cur      = rebuilt.nodes[i];
        const double         cur_abs  = std::abs(cur.area);
        const bool           cur_pos  = cur.area > 0.;
        const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query_box(
            Vec2crd(cur.bounds.left, cur.bounds.top), Vec2crd(cur.bounds.right, cur.bounds.bottom));
        candidates.clear();
        AABBTreeIndirect::traverse(
            aabb,
            [&query_box](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) { return n.bbox.contains(query_box); },
            [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) {
                if (n.idx < i) {
                    const C2RebuiltNode &cand = rebuilt.nodes[n.idx];
                    if ((cand.area > 0.) != cur_pos && std::abs(cand.area) > cur_abs)
                        candidates.push_back(n.idx);
                }
                return true;
            });
        // Check from smallest enclosing candidate to largest so we pick the immediate parent.
        std::sort(candidates.begin(), candidates.end(), [&rebuilt](size_t a, size_t b) {
            return std::abs(rebuilt.nodes[a].area) < std::abs(rebuilt.nodes[b].area);
        });
        size_t parent = size_t(-1);
        for (size_t cand_idx : candidates) {
            if (c2_path_encloses(*rebuilt.nodes[cand_idx].path, *cur.path)) {
                parent = cand_idx;
                break;
            }
        }
        rebuilt.nodes[i].parent = parent;
        if (parent == size_t(-1))
            rebuilt.roots.push_back(i);
        else
            rebuilt.nodes[parent].children.push_back(i);
    }
    return rebuilt;
}

void c2_append_rebuilt_expolygons(const C2RebuiltTree &rebuilt, size_t idx, ExPolygons &out)
{
    const C2RebuiltNode &node   = rebuilt.nodes[idx];
    ExPolygon           &expoly = out.emplace_back();
    expoly.contour.points       = c2_to_points(*node.path);
    expoly.holes.reserve(node.children.size());
    for (size_t hole_idx : node.children)
        expoly.holes.emplace_back().points = c2_to_points(*rebuilt.nodes[hole_idx].path);
    for (size_t hole_idx : node.children)
        for (size_t island_idx : rebuilt.nodes[hole_idx].children)
            c2_append_rebuilt_expolygons(rebuilt, island_idx, out);
}

size_t c2_count_expolygons(const C2::PolyPath64 &outer)
{
    size_t cnt = 1;
    for (const auto &hole : outer)
        for (const auto &island : *hole)
            cnt += c2_count_expolygons(*island);
    return cnt;
}

void c2_append_expolygons(const C2::PolyPath64 &outer, ExPolygons &out)
{
    ExPolygon &expoly = out.emplace_back();
    expoly.contour.points = c2_to_points(outer.Polygon());
    expoly.holes.reserve(outer.Count());
    for (const auto &hole : outer)
        expoly.holes.emplace_back().points = c2_to_points(hole->Polygon());
    // Islands inside the holes.
    for (const auto &hole : outer)
        for (const auto &island : *hole)
            c2_append_expolygons(*island, out);
}

ExPolygons c2_to_expolygons(const C2::PolyTree64 &tree)
{
    if (c2_polytree_is_well_nested(tree)) {
        size_t cnt = 0;
        for (const auto &outer : tree)
            cnt += c2_count_expolygons(*outer);
        ExPolygons out;
        out.reserve(cnt);
        for (const auto &outer : tree)
            c2_append_expolygons(*outer, out);
        return out;
    }
    const C2RebuiltTree rebuilt = c2_rebuild_polytree(tree);
    ExPolygons          out;
    out.reserve(rebuilt.nodes.size());
    for (size_t root_idx : rebuilt.roots)
        c2_append_rebuilt_expolygons(rebuilt, root_idx, out);
    return out;
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

struct UInt128Raw {
    uint64_t lo = 0;
    uint64_t hi = 0;
};

inline UInt128Raw to_uint128_raw(const Int128 &v)
{
    assert(v.sign() >= 0);
    return { v.lo(), static_cast<uint64_t>(v.hi()) };
}

inline void mul_u64_pair(uint64_t a, uint64_t b, uint64_t &hi, uint64_t &lo)
{
#if defined(_MSC_VER) && defined(_WIN64)
    lo = _umul128(a, b, &hi);
#elif (defined(__clang__) || defined(__GNUC__)) && UINTPTR_MAX >= UINT64_MAX
    const unsigned __int128 p = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
    lo = static_cast<uint64_t>(p);
    hi = static_cast<uint64_t>(p >> 64);
#else
    const uint64_t a_lo = a & 0xFFFFFFFFULL, a_hi = a >> 32;
    const uint64_t b_lo = b & 0xFFFFFFFFULL, b_hi = b >> 32;
    const uint64_t p00 = a_lo * b_lo;
    const uint64_t p01 = a_lo * b_hi;
    const uint64_t p10 = a_hi * b_lo;
    const uint64_t p11 = a_hi * b_hi;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
    lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

// Exact comparison of non-negative 128-bit rationals (n1/d1) vs (n2/d2) with d1, d2 > 0.
int cmp_rational_128(const UInt128Raw &n1, const UInt128Raw &d1, const UInt128Raw &n2, const UInt128Raw &d2)
{
    if (n1.hi == 0 && d1.hi == 0 && n2.hi == 0 && d2.hi == 0) {
        uint64_t h1 = 0, l1 = 0, h2 = 0, l2 = 0;
        mul_u64_pair(n1.lo, d2.lo, h1, l1);
        mul_u64_pair(n2.lo, d1.lo, h2, l2);
        if (h1 != h2)
            return h1 < h2 ? -1 : 1;
        if (l1 != l2)
            return l1 < l2 ? -1 : 1;
        return 0;
    }
    auto mul_128x128 = [](const UInt128Raw &a, const UInt128Raw &b, std::array<uint64_t, 4> &out) {
        out = { 0, 0, 0, 0 };
        auto add_at = [&out](size_t pos, uint64_t val) {
            uint64_t carry = val;
            for (size_t i = pos; i < 4 && carry != 0; ++i) {
                const uint64_t s = out[i] + carry;
                carry = (s < out[i]) ? 1 : 0;
                out[i] = s;
            }
        };
        const uint64_t as[2] = { a.lo, a.hi };
        const uint64_t bs[2] = { b.lo, b.hi };
        for (size_t i = 0; i < 2; ++i)
            for (size_t j = 0; j < 2; ++j) {
                uint64_t hi = 0, lo = 0;
                mul_u64_pair(as[i], bs[j], hi, lo);
                add_at(i + j, lo);
                add_at(i + j + 1, hi);
            }
    };
    std::array<uint64_t, 4> p1, p2;
    mul_128x128(n1, d2, p1);
    mul_128x128(n2, d1, p2);
    for (int i = 3; i >= 0; --i)
        if (p1[size_t(i)] != p2[size_t(i)])
            return p1[size_t(i)] < p2[size_t(i)] ? -1 : 1;
    return 0;
}

struct OpenClipEdge {
    Point   a, b;
    coord_t min_x, min_y, max_x, max_y;
};

struct OpenClipEdgeData {
    std::vector<OpenClipEdge>          edges;
    AABBTreeIndirect::Tree<2, coord_t> tree;
    BoundingBox                        total_bbox;

    bool empty() const { return edges.empty(); }

    template<class PathsRange>
    void build_from_paths(const PathsRange &paths)
    {
        edges.clear();
        tree.clear();
        total_bbox = BoundingBox();
        size_t total_pts = 0;
        for (const auto &poly : paths)
            total_pts += poly.size();
        edges.reserve(total_pts);
        for (const auto &poly : paths) {
            const size_t n = poly.size();
            if (n < 3)
                continue;
            for (size_t i = 0; i < n; ++i) {
                const Point &a = poly[i];
                const Point &b = poly[(i + 1 == n) ? 0 : i + 1];
                if (a == b)
                    continue;
                const coord_t min_x = std::min(a.x(), b.x());
                const coord_t min_y = std::min(a.y(), b.y());
                const coord_t max_x = std::max(a.x(), b.x());
                const coord_t max_y = std::max(a.y(), b.y());
                edges.push_back({ a, b, min_x, min_y, max_x, max_y });
                total_bbox.merge(a);
                total_bbox.merge(b);
            }
        }
        if (edges.size() >= 32) {
            std::vector<C2BBoxNode> nodes;
            nodes.reserve(edges.size());
            for (size_t i = 0; i < edges.size(); ++i) {
                const OpenClipEdge &e = edges[i];
                nodes.emplace_back(i, C2::Rect64(e.min_x, e.min_y, e.max_x, e.max_y));
            }
            tree.build(std::move(nodes));
        }
    }

    template<class Fn>
    void query_box(const BoundingBox &box, Fn &&fn) const
    {
        if (edges.empty() || ! total_bbox.overlap(box))
            return;
        if (! tree.empty()) {
            const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox q(box.min, box.max);
            AABBTreeIndirect::traverse(
                tree,
                AABBTreeIndirect::intersecting(q),
                [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                    return fn(node.idx);
                });
        } else {
            for (size_t i = 0; i < edges.size(); ++i) {
                const OpenClipEdge &e = edges[i];
                if (e.max_x >= box.min.x() && e.min_x <= box.max.x() &&
                    e.max_y >= box.min.y() && e.min_y <= box.max.y())
                    if (! fn(i))
                        break;
            }
        }
    }

    enum class IntervalKind {
        StrictlyInside,
        StrictlyOutside,
        HorizTop,
        HorizBottom,
        NonHorizLeft,
        NonHorizRight
    };

    struct RawPathInterval {
        size_t       seg_idx = 0;
        double       t0      = 0.;
        double       t1      = 1.;
        Point        p0;
        Point        p1;
        Point        src_a;
        Point        src_b;
        IntervalKind kind   = IntervalKind::StrictlyOutside;
        bool         inside = false;
    };

    int point_winding(double px_val, double py_val) const
    {
        if (edges.empty() || px_val > double(total_bbox.max.x()) + 1. ||
            py_val < double(total_bbox.min.y()) - 1. || py_val > double(total_bbox.max.y()) + 1.)
            return 0;
        int               w    = 0;
        const int64_t     x_lo = std::clamp<int64_t>(int64_t(std::floor(px_val)) - 1, total_bbox.min.x(), total_bbox.max.x());
        const int64_t     y_lo = std::clamp<int64_t>(int64_t(std::floor(py_val)) - 1, total_bbox.min.y(), total_bbox.max.y());
        const int64_t     y_hi = std::clamp<int64_t>(int64_t(std::ceil(py_val)) + 1, total_bbox.min.y(), total_bbox.max.y());
        const BoundingBox ray_box(Point(coord_t(x_lo), coord_t(y_lo)), Point(total_bbox.max.x(), coord_t(y_hi)));
        query_box(ray_box, [&](size_t idx) {
            const OpenClipEdge &ce = edges[idx];
            if (ce.a.y() == ce.b.y())
                return true;
            const double ay = double(ce.a.y()), by = double(ce.b.y());
            if ((ay <= py_val && by > py_val) || (by <= py_val && ay > py_val)) {
                const double x_cross = (ce.a.x() == ce.b.x()) ? double(ce.a.x())
                    : (double(ce.a.x()) + (py_val - ay) * double(ce.b.x() - ce.a.x()) / (by - ay));
                if (x_cross > px_val)
                    w += (ce.b.y() > ce.a.y() ? 1 : -1);
            }
            return true;
        });
        return w;
    }

    void clip_segment_raw(const Point &a, const Point &b, size_t seg_idx, std::vector<RawPathInterval> &out) const
    {
        if (a == b)
            return;
        if (edges.empty()) {
            out.push_back({ seg_idx, 0., 1., a, b, a, b, IntervalKind::StrictlyOutside, false });
            return;
        }

        const int64_t dsx       = int64_t(b.x()) - int64_t(a.x());
        const int64_t dsy       = int64_t(b.y()) - int64_t(a.y());
        const bool    horiz     = (a.y() == b.y());
        const bool    use_x     = std::abs(dsx) >= std::abs(dsy);
        const int64_t axis_len  = use_x ? std::abs(dsx) : std::abs(dsy);
        const int64_t axis_sign = (use_x ? dsx : dsy) > 0 ? 1 : -1;

        struct Breakpoint {
            UInt128Raw num;
            UInt128Raw den;
            double     t_dbl    = 0.;
            Point      pt;
            bool       exact_pt = false;
        };
        struct CollinearSpan {
            int64_t n0   = 0;
            int64_t n1   = 0;
            int     wind = 0;
        };

        std::vector<Breakpoint>    bps;
        std::vector<CollinearSpan> collinear_spans;
        std::vector<size_t>        cand_edges;

        const UInt128Raw u_axis_len{ static_cast<uint64_t>(axis_len), 0 };
        bps.push_back({ { 0, 0 }, { 1, 0 }, 0., a, true });
        bps.push_back({ { 1, 0 }, { 1, 0 }, 1., b, true });

        auto proj_along = [a, use_x, axis_sign](const Point &p) -> int64_t {
            return (use_x ? (int64_t(p.x()) - int64_t(a.x())) : (int64_t(p.y()) - int64_t(a.y()))) * axis_sign;
        };
        auto add_axis_bp = [&](int64_t proj, const Point &p, bool is_exact) {
            if (proj <= 0 || proj >= axis_len)
                return;
            const UInt128Raw num{ static_cast<uint64_t>(proj), 0 };
            bps.push_back({ num, u_axis_len, double(proj) / double(axis_len), p, is_exact });
        };

        const BoundingBox seg_box(Point(std::min(a.x(), b.x()), std::min(a.y(), b.y())),
                                  Point(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
        query_box(seg_box, [&](size_t idx) {
            cand_edges.push_back(idx);
            return true;
        });

        for (size_t idx : cand_edges) {
            const OpenClipEdge &ce = edges[idx];
            const int           o1 = int128::orient(a, b, ce.a);
            const int           o2 = int128::orient(a, b, ce.b);
            if (o1 == 0 && o2 == 0) {
                const int64_t pa = proj_along(ce.a);
                const int64_t pb = proj_along(ce.b);
                const int64_t lo = std::max<int64_t>(0, std::min(pa, pb));
                const int64_t hi = std::min<int64_t>(axis_len, std::max(pa, pb));
                if (lo < hi) {
                    const int wind = horiz ? (ce.a.x() > ce.b.x() ? 1 : -1) : (ce.b.y() > ce.a.y() ? 1 : -1);
                    collinear_spans.push_back({ lo, hi, wind });
                    add_axis_bp(lo, (lo == pa) ? ce.a : ((lo == pb) ? ce.b : a), lo == pa || lo == pb);
                    add_axis_bp(hi, (hi == pa) ? ce.a : ((hi == pb) ? ce.b : b), hi == pa || hi == pb);
                }
                continue;
            }
            const int o3 = int128::orient(ce.a, ce.b, a);
            const int o4 = int128::orient(ce.a, ce.b, b);
            if (o1 != o2 && o3 != o4) {
                if (o1 == 0)
                    add_axis_bp(proj_along(ce.a), ce.a, true);
                else if (o2 == 0)
                    add_axis_bp(proj_along(ce.b), ce.b, true);
                else if (o3 != 0 && o4 != 0) {
                    const int64_t dcx = int64_t(ce.b.x()) - int64_t(ce.a.x());
                    const int64_t dcy = int64_t(ce.b.y()) - int64_t(ce.a.y());
                    const int64_t wx  = int64_t(ce.a.x()) - int64_t(a.x());
                    const int64_t wy  = int64_t(ce.a.y()) - int64_t(a.y());
                    Int128        den = Int128::multiply(dsx, dcy) - Int128::multiply(dsy, dcx);
                    Int128        num = Int128::multiply(wx, dcy) - Int128::multiply(wy, dcx);
                    if (den.sign() < 0) {
                        den = -den;
                        num = -num;
                    }
                    if (den.sign() > 0 && num.sign() > 0 && num < den) {
                        const double t_dbl = std::clamp(double(num) / double(den), 0., 1.);
                        const Point  ip(a.x() + coord_t(std::llround(t_dbl * double(dsx))),
                                        a.y() + coord_t(std::llround(t_dbl * double(dsy))));
                        bps.push_back({ to_uint128_raw(num), to_uint128_raw(den), t_dbl, ip, false });
                    }
                }
            } else if (o1 == 0 && ce.a.x() >= seg_box.min.x() && ce.a.x() <= seg_box.max.x() &&
                       ce.a.y() >= seg_box.min.y() && ce.a.y() <= seg_box.max.y()) {
                add_axis_bp(proj_along(ce.a), ce.a, true);
            } else if (o2 == 0 && ce.b.x() >= seg_box.min.x() && ce.b.x() <= seg_box.max.x() &&
                       ce.b.y() >= seg_box.min.y() && ce.b.y() <= seg_box.max.y()) {
                add_axis_bp(proj_along(ce.b), ce.b, true);
            }
        }

        std::sort(bps.begin(), bps.end(), [](const Breakpoint &l, const Breakpoint &r) {
            if (std::abs(l.t_dbl - r.t_dbl) > 1e-12)
                return l.t_dbl < r.t_dbl;
            const int c = cmp_rational_128(l.num, l.den, r.num, r.den);
            if (c != 0)
                return c < 0;
            return l.exact_pt && ! r.exact_pt;
        });

        // Deduplicate identical rational parameters while keeping the exact-vertex point when present.
        size_t write = 0;
        for (size_t i = 0; i < bps.size(); ++i) {
            if (write > 0 &&
                std::abs(bps[write - 1].t_dbl - bps[i].t_dbl) <= 1e-12 &&
                cmp_rational_128(bps[write - 1].num, bps[write - 1].den, bps[i].num, bps[i].den) == 0) {
                if (! bps[write - 1].exact_pt && bps[i].exact_pt)
                    bps[write - 1] = bps[i];
                continue;
            }
            if (write != i)
                bps[write] = bps[i];
            ++write;
        }
        bps.resize(write);

        auto classify_mid = [&](double t_mid) -> IntervalKind {
            const double mx = double(a.x()) + t_mid * double(dsx);
            const double my = double(a.y()) + t_mid * double(dsy);

            if (horiz) {
                int w_above = 0;
                if (a.y() <= total_bbox.max.y() &&
                    mx >= double(total_bbox.min.x()) - 1. && mx <= double(total_bbox.max.x()) + 1.) {
                    const int64_t     x_lo = std::clamp<int64_t>(int64_t(std::floor(mx)) - 1, total_bbox.min.x(), total_bbox.max.x());
                    const int64_t     x_hi = std::clamp<int64_t>(int64_t(std::ceil(mx)) + 1, total_bbox.min.x(), total_bbox.max.x());
                    const int64_t     y_lo = std::max<int64_t>(a.y(), total_bbox.min.y());
                    const BoundingBox ray_box(Point(coord_t(x_lo), coord_t(y_lo)), Point(coord_t(x_hi), total_bbox.max.y()));
                    query_box(ray_box, [&](size_t idx) {
                        const OpenClipEdge &ce = edges[idx];
                        if (ce.a.x() == ce.b.x())
                            return true;
                        const double ax = double(ce.a.x()), bx = double(ce.b.x());
                        if ((ax <= mx && bx > mx) || (bx <= mx && ax > mx)) {
                            if (ce.a.y() == a.y() && ce.b.y() == a.y())
                                return true;
                            const double y_cross = (ce.a.y() == ce.b.y()) ? double(ce.a.y())
                                : (double(ce.a.y()) + (mx - ax) * double(ce.b.y() - ce.a.y()) / (bx - ax));
                            if (y_cross > my)
                                w_above += (ce.a.x() > ce.b.x() ? 1 : -1);
                        }
                        return true;
                    });
                }
                int w_below = w_above;
                for (const CollinearSpan &sp : collinear_spans) {
                    const double t0 = double(sp.n0) / double(axis_len);
                    const double t1 = double(sp.n1) / double(axis_len);
                    if (t_mid > t0 && t_mid < t1)
                        w_below += sp.wind;
                }
                if (w_above != 0 && w_below != 0)
                    return IntervalKind::StrictlyInside;
                if (w_above == 0 && w_below == 0)
                    return IntervalKind::StrictlyOutside;
                return (w_below != 0) ? IntervalKind::HorizTop : IntervalKind::HorizBottom;
            }

            int w_right = 0;
            if (mx <= double(total_bbox.max.x()) + 1. &&
                my >= double(total_bbox.min.y()) - 1. && my <= double(total_bbox.max.y()) + 1.) {
                const int64_t     x_lo = std::clamp<int64_t>(int64_t(std::floor(mx)) - 1, total_bbox.min.x(), total_bbox.max.x());
                const int64_t     y_lo = std::clamp<int64_t>(int64_t(std::floor(my)) - 1, total_bbox.min.y(), total_bbox.max.y());
                const int64_t     y_hi = std::clamp<int64_t>(int64_t(std::ceil(my)) + 1, total_bbox.min.y(), total_bbox.max.y());
                const BoundingBox ray_box(Point(coord_t(x_lo), coord_t(y_lo)), Point(total_bbox.max.x(), coord_t(y_hi)));
                query_box(ray_box, [&](size_t idx) {
                    const OpenClipEdge &ce = edges[idx];
                    if (ce.a.y() == ce.b.y())
                        return true;
                    const double ay = double(ce.a.y()), by = double(ce.b.y());
                    if ((ay <= my && by > my) || (by <= my && ay > my)) {
                        if (int128::orient(a, b, ce.a) == 0 && int128::orient(a, b, ce.b) == 0)
                            return true;
                        const double x_cross = (ce.a.x() == ce.b.x()) ? double(ce.a.x())
                            : (double(ce.a.x()) + (my - ay) * double(ce.b.x() - ce.a.x()) / (by - ay));
                        if (x_cross > mx)
                            w_right += (ce.b.y() > ce.a.y() ? 1 : -1);
                    }
                    return true;
                });
            }
            int w_left = w_right;
            for (const CollinearSpan &sp : collinear_spans) {
                const double t0 = double(sp.n0) / double(axis_len);
                const double t1 = double(sp.n1) / double(axis_len);
                if (t_mid > t0 && t_mid < t1)
                    w_left += sp.wind;
            }
            if (w_right != 0 && w_left != 0)
                return IntervalKind::StrictlyInside;
            if (w_right == 0 && w_left == 0)
                return IntervalKind::StrictlyOutside;
            return (w_right != 0) ? IntervalKind::NonHorizLeft : IntervalKind::NonHorizRight;
        };

        const size_t base_out = out.size();
        out.reserve(base_out + bps.size() - 1);
        for (size_t k = 0; k + 1 < bps.size(); ++k) {
            if (bps[k].pt == bps[k + 1].pt) {
                if (k + 2 == bps.size() && out.size() > base_out) {
                    out.back().t1 = bps[k + 1].t_dbl;
                    out.back().p1 = bps[k + 1].pt;
                }
                continue;
            }
            const double       t_mid = 0.5 * (bps[k].t_dbl + bps[k + 1].t_dbl);
            const IntervalKind kind  = classify_mid(t_mid);
            out.push_back({ seg_idx, bps[k].t_dbl, bps[k + 1].t_dbl, bps[k].pt, bps[k + 1].pt, a, b, kind, false });
        }
        if (out.size() == base_out) {
            const IntervalKind kind = classify_mid(0.5);
            out.push_back({ seg_idx, 0., 1., a, b, a, b, kind, false });
        } else {
            out[base_out].t0 = 0.;
            out[base_out].p0 = a;
            out.back().t1    = 1.;
            out.back().p1    = b;
        }
    }

    void clip_segment(const Point &a, const Point &b, ClipperUtils::OpenClipBoundaryRule rule,
                      std::vector<ClipperUtils::OpenClipInterval> &out) const
    {
        out.clear();
        if (a == b)
            return;
        std::vector<RawPathInterval> raw;
        clip_segment_raw(a, b, 0, raw);
        out.reserve(raw.size());
        for (const RawPathInterval &r : raw) {
            const bool inside = (rule == ClipperUtils::OpenClipBoundaryRule::Inclusive)
                ? (r.kind != IntervalKind::StrictlyOutside)
                : (r.kind == IntervalKind::StrictlyInside || r.kind == IntervalKind::HorizBottom || r.kind == IntervalKind::NonHorizLeft);
            if (! out.empty() && out.back().inside == inside) {
                out.back().t1 = r.t1;
                out.back().p1 = r.p1;
            } else {
                out.push_back({ r.t0, r.t1, r.p0, r.p1, inside });
            }
        }
    }

    void clip_path(const Points &path, bool is_closed, ClipperUtils::OpenClipBoundaryRule rule,
                   std::vector<ClipperUtils::OpenClipPathInterval> &out) const
    {
        out.clear();
        if (path.size() < 2)
            return;
        size_t num_segs = path.size() - 1;
        if (is_closed && path.size() >= 3 && path.front() != path.back())
            num_segs = path.size();

        std::vector<RawPathInterval> raw;
        raw.reserve(num_segs);
        for (size_t i = 0; i < num_segs; ++i) {
            const Point &a = path[i];
            const Point &b = path[(i + 1 == path.size()) ? 0 : i + 1];
            if (a != b)
                clip_segment_raw(a, b, i, raw);
        }
        if (raw.empty())
            return;

        if (rule == ClipperUtils::OpenClipBoundaryRule::Inclusive || edges.empty()) {
            for (RawPathInterval &r : raw)
                r.inside = (r.kind != IntervalKind::StrictlyOutside);
        } else {
            const size_t M        = raw.size();
            auto         next_mod = [M](size_t i) -> size_t { return (i + 1 == M) ? 0 : i + 1; };
            auto         prev_mod = [M](size_t i) -> size_t { return (i == 0) ? M - 1 : i - 1; };

            // Pass 1: base classification + contiguous NonHorizRight runs via Clipper1 top-corner maxima rule.
            for (RawPathInterval &r : raw) {
                switch (r.kind) {
                case IntervalKind::StrictlyInside:
                case IntervalKind::NonHorizLeft:
                    r.inside = true;
                    break;
                case IntervalKind::StrictlyOutside:
                case IntervalKind::HorizTop:
                case IntervalKind::HorizBottom:
                case IntervalKind::NonHorizRight:
                    r.inside = false;
                    break;
                }
            }

            size_t start_v = 0;
            bool   all_v   = true;
            if (is_closed) {
                for (size_t i = 0; i < M; ++i)
                    if (raw[i].kind != IntervalKind::NonHorizRight) {
                        start_v = next_mod(i);
                        all_v   = false;
                        break;
                    }
            } else {
                all_v = std::all_of(raw.begin(), raw.end(), [](const RawPathInterval &r) {
                    return r.kind == IntervalKind::NonHorizRight;
                });
            }

            if (! all_v) {
                for (size_t step = 0; step < M;) {
                    const size_t s = (start_v + step) % M;
                    if (raw[s].kind != IntervalKind::NonHorizRight) {
                        ++step;
                        continue;
                    }
                    size_t run_len = 1;
                    while (run_len < M) {
                        const size_t nxt = (s + run_len) % M;
                        if (! is_closed && nxt == 0)
                            break;
                        if (raw[nxt].kind != IntervalKind::NonHorizRight)
                            break;
                        ++run_len;
                    }
                    const size_t e        = (s + run_len - 1) % M;
                    const bool   has_prev = is_closed || s > 0;
                    const bool   has_next = is_closed || (e + 1 < M);

                    bool v_inside = false;
                    auto test_upper_endpoint = [&](const Point &p_hi, const Point &p_along,
                                                   const RawPathInterval &adj, const Point &adj_other) {
                        if (adj.kind == IntervalKind::StrictlyInside &&
                            (adj_other.y() > p_hi.y() || (adj_other.y() == p_hi.y() && adj_other.x() < p_hi.x()))) {
                            v_inside = true;
                            return;
                        }
                        if (point_winding(double(p_hi.x()) - 1e-3, double(p_hi.y()) + 1e-3) == 0 &&
                            int128::orient(p_hi, p_along, adj_other) < 0)
                            v_inside = true;
                    };

                    if (has_prev && raw[s].src_a.y() > raw[s].src_b.y()) {
                        const size_t prev_idx = prev_mod(s);
                        if (raw[prev_idx].p1 == raw[s].p0)
                            test_upper_endpoint(raw[s].p0, raw[s].p1, raw[prev_idx], raw[prev_idx].p0);
                    }
                    if (! v_inside && has_next && raw[e].src_b.y() > raw[e].src_a.y()) {
                        const size_t next_idx = next_mod(e);
                        if (raw[next_idx].p0 == raw[e].p1)
                            test_upper_endpoint(raw[e].p1, raw[e].p0, raw[next_idx], raw[next_idx].p1);
                    }

                    for (size_t k = 0; k < run_len; ++k)
                        raw[(s + k) % M].inside = v_inside;
                    step += run_len;
                }
            }

            // Pass 2: contiguous HorizBottom runs — inside iff connected at either end to an upper inside edge.
            size_t start_h = 0;
            bool   all_h   = true;
            if (is_closed) {
                for (size_t i = 0; i < M; ++i) {
                    const size_t nxt = next_mod(i);
                    if (raw[i].kind != IntervalKind::HorizBottom ||
                        raw[nxt].kind != IntervalKind::HorizBottom ||
                        raw[i].src_a.y() != raw[nxt].src_a.y()) {
                        start_h = nxt;
                        all_h   = false;
                        break;
                    }
                }
            } else {
                const coord_t y_first = raw.front().src_a.y();
                all_h                 = std::all_of(raw.begin(), raw.end(), [y_first](const RawPathInterval &r) {
                    return r.kind == IntervalKind::HorizBottom && r.src_a.y() == y_first;
                });
            }

            if (! all_h) {
                for (size_t step = 0; step < M;) {
                    const size_t s = (start_h + step) % M;
                    if (raw[s].kind != IntervalKind::HorizBottom) {
                        ++step;
                        continue;
                    }
                    const coord_t y0      = raw[s].src_a.y();
                    size_t        run_len = 1;
                    while (run_len < M) {
                        const size_t nxt = (s + run_len) % M;
                        if (! is_closed && nxt == 0)
                            break;
                        if (raw[nxt].kind != IntervalKind::HorizBottom || raw[nxt].src_a.y() != y0)
                            break;
                        ++run_len;
                    }
                    const size_t e        = (s + run_len - 1) % M;
                    const bool   has_prev = is_closed || s > 0;
                    const bool   has_next = is_closed || (e + 1 < M);

                    bool h_inside = false;
                    if (has_prev) {
                        const size_t prev_idx = prev_mod(s);
                        if (raw[prev_idx].inside && raw[prev_idx].p1 == raw[s].p0 && raw[prev_idx].src_a.y() > y0)
                            h_inside = true;
                    }
                    if (! h_inside && has_next) {
                        const size_t next_idx = next_mod(e);
                        if (raw[next_idx].inside && raw[next_idx].p0 == raw[e].p1 && raw[next_idx].src_b.y() > y0)
                            h_inside = true;
                    }
                    for (size_t k = 0; k < run_len; ++k)
                        raw[(s + k) % M].inside = h_inside;
                    step += run_len;
                }
            }
        }

        out.reserve(raw.size());
        for (const RawPathInterval &r : raw) {
            if (! out.empty() && out.back().seg_idx == r.seg_idx && out.back().inside == r.inside && out.back().p1 == r.p0) {
                out.back().t1 = r.t1;
                out.back().p1 = r.p1;
            } else {
                out.push_back({ r.seg_idx, r.t0, r.t1, r.p0, r.p1, r.inside });
            }
        }
    }
};

Polylines clip_open_paths_with_index(const OpenClipEdgeData &index, const C2::Paths64 &subject,
                                     C2::ClipType type, ClipperUtils::OpenClipBoundaryRule rule)
{
    Polylines out;
    if (subject.empty())
        return out;
    const bool keep_inside = (type == C2::ClipType::Intersection);
    std::vector<ClipperUtils::OpenClipPathInterval> intervals;
    Points                                          pts;
    for (const C2::Path64 &path : subject) {
        if (path.size() < 2)
            continue;
        pts.clear();
        pts.reserve(path.size());
        for (const C2::Point64 &p : path)
            pts.emplace_back(coord_t(p.x), coord_t(p.y));
        const bool is_closed = (pts.size() >= 3 && pts.front() == pts.back());
        index.clip_path(pts, is_closed, rule, intervals);
        Polyline cur;
        auto flush = [&]() {
            if (cur.points.size() >= 2)
                out.emplace_back(std::move(cur));
            cur.points.clear();
        };
        for (const ClipperUtils::OpenClipPathInterval &iv : intervals) {
            if (iv.inside != keep_inside) {
                flush();
                continue;
            }
            if (iv.p0 == iv.p1)
                continue;
            if (cur.points.empty()) {
                cur.points.push_back(iv.p0);
                cur.points.push_back(iv.p1);
            } else {
                if (cur.points.back() != iv.p0) {
                    flush();
                    cur.points.push_back(iv.p0);
                }
                if (cur.points.back() != iv.p1)
                    cur.points.push_back(iv.p1);
            }
        }
        flush();
    }
    return out;
}

Polylines c2_clip_open(C2::ClipType type, const C2::Paths64 &subject, const C2::Paths64 &clip)
{
    if (type != C2::ClipType::Intersection && type != C2::ClipType::Difference) {
        C2::Clipper64 clipper;
        clipper.PreserveCollinear(false);
        clipper.AddOpenSubject(subject);
        if (! clip.empty())
            clipper.AddClip(clip);
        C2::Paths64 closed, open;
        clipper.Execute(type, C2::FillRule::NonZero, closed, open);
        return c2_to_polylines(open);
    }
    if (subject.empty())
        return {};
    if (clip.empty())
        return (type == C2::ClipType::Difference) ? c2_to_polylines(subject) : Polylines{};
    OpenClipEdgeData index;
    Polygons         clip_polys;
    clip_polys.reserve(clip.size());
    for (const C2::Path64 &p : clip)
        clip_polys.emplace_back().points = c2_to_points(p);
    index.build_from_paths(clip_polys);
    if (index.empty())
        return (type == C2::ClipType::Difference) ? c2_to_polylines(subject) : Polylines{};
    return clip_open_paths_with_index(index, subject, type, ClipperUtils::OpenClipBoundaryRule::Clipper1ExcludeTop);
}

// Clipper1 semantics: miter limit at least 2, arc tolerance (also for round caps) at most a quarter of the offset.
void c2_offset_setup(C2::ClipperOffset &co, float delta, JoinType joinType, double miterLimit)
{
    const double max_arc_tolerance = std::abs(delta) * 0.25;
    if (joinType == jtRound)
        co.ArcTolerance(std::min(miterLimit > 0. ? miterLimit : 0.25, max_arc_tolerance));
    else {
        co.MiterLimit(std::max(miterLimit, 2.));
        co.ArcTolerance(std::min(0.25, max_arc_tolerance));
    }
}

bool c2_segments_intersect(const C2::Point64 &a1, const C2::Point64 &a2, const C2::Point64 &b1, const C2::Point64 &b2)
{
    if (std::max(a1.x, a2.x) < std::min(b1.x, b2.x) || std::max(b1.x, b2.x) < std::min(a1.x, a2.x) ||
        std::max(a1.y, a2.y) < std::min(b1.y, b2.y) || std::max(b1.y, b2.y) < std::min(a1.y, a2.y))
        return false;
    const Vec2crd pa1(a1.x, a1.y), pa2(a2.x, a2.y), pb1(b1.x, b1.y), pb2(b2.x, b2.y);
    const int o1 = int128::orient(pa1, pa2, pb1);
    const int o2 = int128::orient(pa1, pa2, pb2);
    const int o3 = int128::orient(pb1, pb2, pa1);
    const int o4 = int128::orient(pb1, pb2, pa2);
    if (o1 != o2 && o3 != o4)
        return true;
    auto on_seg = [](const C2::Point64 &p, const C2::Point64 &q, const C2::Point64 &r) {
        return q.x >= std::min(p.x, r.x) && q.x <= std::max(p.x, r.x) &&
               q.y >= std::min(p.y, r.y) && q.y <= std::max(p.y, r.y);
    };
    return (o1 == 0 && on_seg(a1, b1, a2)) || (o2 == 0 && on_seg(a1, b2, a2)) ||
           (o3 == 0 && on_seg(b1, a1, b2)) || (o4 == 0 && on_seg(b1, a2, b2));
}

bool c2_paths_touch(const C2::Path64 &a, const C2::Path64 &b, const C2::Rect64 &box_a, const C2::Rect64 &box_b)
{
    auto collect_edges = [](const C2::Path64 &p, const C2::Rect64 &box, std::vector<size_t> &edges) {
        edges.clear();
        const size_t n = p.size();
        for (size_t i = 0; i < n; ++i) {
            const C2::Point64 &u = p[i];
            const C2::Point64 &v = p[(i + 1 == n) ? 0 : i + 1];
            if (std::max(u.x, v.x) < box.left || std::min(u.x, v.x) > box.right ||
                std::max(u.y, v.y) < box.top  || std::min(u.y, v.y) > box.bottom)
                continue;
            edges.push_back(i);
        }
    };
    std::vector<size_t> edges_b;
    collect_edges(b, box_a, edges_b);
    if (edges_b.empty())
        return false;
    std::vector<size_t> edges_a;
    collect_edges(a, box_b, edges_a);
    if (edges_a.empty())
        return false;
    const size_t na = a.size();
    const size_t nb = b.size();
    if (edges_a.size() * edges_b.size() <= 256) {
        for (size_t ia : edges_a) {
            const C2::Point64 &a1 = a[ia];
            const C2::Point64 &a2 = a[(ia + 1 == na) ? 0 : ia + 1];
            for (size_t ib : edges_b) {
                const C2::Point64 &b1 = b[ib];
                const C2::Point64 &b2 = b[(ib + 1 == nb) ? 0 : ib + 1];
                if (c2_segments_intersect(a1, a2, b1, b2))
                    return true;
            }
        }
        return false;
    }

    const bool                 index_b     = edges_b.size() <= edges_a.size();
    const C2::Path64          &indexed     = index_b ? b : a;
    const C2::Path64          &queried     = index_b ? a : b;
    const std::vector<size_t> &idx_edges   = index_b ? edges_b : edges_a;
    const std::vector<size_t> &qry_edges   = index_b ? edges_a : edges_b;
    const size_t               n_idx       = indexed.size();
    const size_t               n_qry       = queried.size();

    std::vector<C2BBoxNode> edge_nodes;
    edge_nodes.reserve(idx_edges.size());
    for (size_t i = 0; i < idx_edges.size(); ++i) {
        const size_t       ei = idx_edges[i];
        const C2::Point64 &u  = indexed[ei];
        const C2::Point64 &v  = indexed[(ei + 1 == n_idx) ? 0 : ei + 1];
        edge_nodes.emplace_back(ei, C2::Rect64(std::min(u.x, v.x), std::min(u.y, v.y), std::max(u.x, v.x), std::max(u.y, v.y)));
    }
    AABBTreeIndirect::Tree<2, coord_t> edge_tree;
    edge_tree.build(std::move(edge_nodes));

    bool hit = false;
    for (size_t iq : qry_edges) {
        const C2::Point64 &q1 = queried[iq];
        const C2::Point64 &q2 = queried[(iq + 1 == n_qry) ? 0 : iq + 1];
        const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query_box(
            Vec2crd(std::min(q1.x, q2.x), std::min(q1.y, q2.y)),
            Vec2crd(std::max(q1.x, q2.x), std::max(q1.y, q2.y)));
        AABBTreeIndirect::traverse(
            edge_tree,
            AABBTreeIndirect::intersecting(query_box),
            [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                const size_t       ei = node.idx;
                const C2::Point64 &p1 = indexed[ei];
                const C2::Point64 &p2 = indexed[(ei + 1 == n_idx) ? 0 : ei + 1];
                if (c2_segments_intersect(q1, q2, p1, p2)) {
                    hit = true;
                    return false;
                }
                return true;
            });
        if (hit)
            return true;
    }
    return false;
}

inline int64_t c2_offset_influence_pad(float delta, JoinType joinType, double miterLimit)
{
    const double factor = (joinType == jtMiter) ? std::max(2.0, miterLimit)
                        : (joinType == jtSquare) ? 1.42 : 1.05;
    return int64_t(std::ceil(factor * std::abs(double(delta)))) + 4;
}

inline C2::Rect64 c2_inflate_rect64(const C2::Rect64 &r, int64_t pad)
{
    constexpr int64_t kLo = std::numeric_limits<int64_t>::min() / 4;
    constexpr int64_t kHi = std::numeric_limits<int64_t>::max() / 4;
    auto clamp_sub = [kLo](int64_t v, int64_t p) -> int64_t {
        return (v - kLo < p) ? kLo : v - p;
    };
    auto clamp_add = [kHi](int64_t v, int64_t p) -> int64_t {
        return (kHi - v < p) ? kHi : v + p;
    };
    return C2::Rect64(clamp_sub(r.left, pad), clamp_sub(r.top, pad), clamp_add(r.right, pad), clamp_add(r.bottom, pad));
}

inline bool c2_rects_overlap(const C2::Rect64 &a, const C2::Rect64 &b)
{
    return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}

// Offset non-CW paths safely: mutually influence-disjoint positive-area paths run in one fast batch,
// while any paths with overlapping offset-influence bboxes (or zero-area bowties) run individually so
// concave self-intersections cannot cancel adjacent paths.
template<class TOut>
bool c2_offset_ccw_paths_isolated(C2::Paths64 &&paths, const std::vector<int> &area_signs,
                                  float delta, JoinType joinType, double miterLimit, TOut &out)
{
    const size_t n = paths.size();
    if (n == 0)
        return false;
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    if (n == 1) {
        co.AddPath(paths[0], to_c2(joinType), C2::EndType::Polygon);
        co.Execute(delta, out);
        return true;
    }

    const int64_t          pad = c2_offset_influence_pad(delta, joinType, miterLimit);
    std::vector<C2::Rect64> inf_bounds;
    inf_bounds.reserve(n);
    for (const C2::Path64 &p : paths)
        inf_bounds.emplace_back(c2_inflate_rect64(C2::GetBounds(p), pad));

    std::vector<uint8_t> isolate(n, 0);
    bool                 any_isolated = false;
    for (size_t i = 0; i < n; ++i)
        if (area_signs[i] <= 0) {
            isolate[i]   = 1;
            any_isolated = true;
        }

    if (n < 16) {
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                if (c2_rects_overlap(inf_bounds[i], inf_bounds[j])) {
                    isolate[i]   = 1;
                    isolate[j]   = 1;
                    any_isolated = true;
                }
            }
        }
    } else {
        std::vector<C2BBoxNode> nodes;
        nodes.reserve(n);
        for (size_t i = 0; i < n; ++i)
            nodes.emplace_back(i, inf_bounds[i]);
        AABBTreeIndirect::Tree<2, coord_t> aabb;
        aabb.build(std::move(nodes));
        for (size_t i = 0; i < n; ++i) {
            const C2::Rect64 &bi = inf_bounds[i];
            const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query(
                Vec2crd(clamp_to_coord(bi.left), clamp_to_coord(bi.top)),
                Vec2crd(clamp_to_coord(bi.right), clamp_to_coord(bi.bottom)));
            AABBTreeIndirect::traverse(
                aabb,
                AABBTreeIndirect::intersecting(query),
                [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                    if (node.idx != i && c2_rects_overlap(bi, inf_bounds[node.idx])) {
                        isolate[i]        = 1;
                        isolate[node.idx] = 1;
                        any_isolated      = true;
                    }
                    return true;
                });
        }
    }

    if (! any_isolated) {
        co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
        co.Execute(delta, out);
        return true;
    }

    C2::Paths64 batch;
    C2::Paths64 collected;
    C2::Paths64 single;
    size_t      batches_used = 0;
    for (size_t i = 0; i < n; ++i) {
        if (! isolate[i]) {
            batch.emplace_back(std::move(paths[i]));
        } else {
            co.Clear();
            co.AddPath(paths[i], to_c2(joinType), C2::EndType::Polygon);
            co.Execute(delta, single);
            if (! single.empty()) {
                append(collected, std::move(single));
                ++batches_used;
            }
        }
    }
    if (! batch.empty()) {
        co.Clear();
        co.AddPaths(batch, to_c2(joinType), C2::EndType::Polygon);
        C2::Paths64 batch_out;
        co.Execute(delta, batch_out);
        if (! batch_out.empty()) {
            append(collected, std::move(batch_out));
            ++batches_used;
        }
    }
    if (collected.empty())
        return false;
    if constexpr (std::is_same_v<TOut, C2::PolyTree64>) {
        c2_clip(C2::ClipType::Union, collected, {}, delta > 0. ? C2::FillRule::NonZero : C2::FillRule::Positive, out);
    } else {
        if (batches_used <= 1)
            out = std::move(collected);
        else
            c2_clip(C2::ClipType::Union, collected, {}, delta > 0. ? C2::FillRule::NonZero : C2::FillRule::Positive, out);
    }
    return true;
}

// A single group offsets like Clipper1's path by path offset only if every CW path is strictly
// enclosed inside a CCW path (positive winding, no touching boundaries), no same-sign paths
// have overlapping offset influence boxes, and no zero-area path can confuse orientation.
bool c2_offset_as_group(const C2::Paths64 &paths, const std::vector<int> &area_signs,
                        float delta, JoinType joinType, double miterLimit)
{
    bool has_cw   = false;
    bool has_zero = false;
    for (int s : area_signs) {
        if (s < 0)
            has_cw = true;
        else if (s == 0)
            has_zero = true;
    }
    if (has_zero)
        return false;
    if (! has_cw)
        return false;

    const size_t            n   = paths.size();
    const int64_t           pad = c2_offset_influence_pad(delta, joinType, miterLimit);
    std::vector<C2::Rect64> bounds;
    std::vector<C2::Rect64> inf_bounds;
    bounds.reserve(n);
    inf_bounds.reserve(n);
    std::vector<C2BBoxNode> bbox_nodes;
    std::vector<C2BBoxNode> inf_nodes;
    bbox_nodes.reserve(n);
    inf_nodes.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        bounds.emplace_back(C2::GetBounds(paths[i]));
        inf_bounds.emplace_back(c2_inflate_rect64(bounds.back(), pad));
        bbox_nodes.emplace_back(i, bounds.back());
        inf_nodes.emplace_back(i, inf_bounds.back());
    }
    AABBTreeIndirect::Tree<2, coord_t> aabb;
    AABBTreeIndirect::Tree<2, coord_t> inf_aabb;
    aabb.build(std::move(bbox_nodes));
    inf_aabb.build(std::move(inf_nodes));

    std::vector<size_t> candidates;
    for (size_t i = 0; i < n; ++i) {
        bool same_sign_conflict = false;
        const C2::Rect64 &ibi   = inf_bounds[i];
        const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox inf_query(
            Vec2crd(clamp_to_coord(ibi.left), clamp_to_coord(ibi.top)),
            Vec2crd(clamp_to_coord(ibi.right), clamp_to_coord(ibi.bottom)));
        AABBTreeIndirect::traverse(
            inf_aabb,
            AABBTreeIndirect::intersecting(inf_query),
            [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                if (node.idx != i && ((area_signs[i] < 0) == (area_signs[node.idx] < 0)) &&
                    c2_rects_overlap(ibi, inf_bounds[node.idx])) {
                    same_sign_conflict = true;
                    return false;
                }
                return true;
            });
        if (same_sign_conflict)
            return false;

        if (area_signs[i] >= 0)
            continue;
        const C2::Rect64 &hole_box = bounds[i];
        const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query_box(
            Vec2crd(hole_box.left, hole_box.top), Vec2crd(hole_box.right, hole_box.bottom));
        candidates.clear();
        AABBTreeIndirect::traverse(
            aabb,
            AABBTreeIndirect::intersecting(query_box),
            [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                if (node.idx != i)
                    candidates.push_back(node.idx);
                return true;
            });

        const C2::Point64 &probe   = paths[i].front();
        int                winding = 0;
        for (size_t j : candidates) {
            if (c2_paths_touch(paths[i], paths[j], hole_box, bounds[j]))
                return false;
            const C2::Rect64 &rb = bounds[j];
            if (probe.x >= rb.left && probe.x <= rb.right && probe.y >= rb.top && probe.y <= rb.bottom) {
                const int pip = c2_point_in_path(probe, paths[j]);
                if (pip == -1)
                    return false;
                if (pip == 1)
                    winding += (area_signs[j] > 0 ? 1 : -1);
            }
        }
        if (winding <= 0)
            return false;
    }
    return true;
}

template<class TOut>
void c2_offset_closed_paths(C2::Paths64 &&paths, float delta, JoinType joinType, double miterLimit, TOut &out)
{
    std::vector<int> area_signs;
    area_signs.reserve(paths.size());
    bool has_cw   = false;
    bool has_zero = false;
    if (delta < 0.) {
        // Drop only truly collinear/degenerate zero-area paths; self-intersecting zero-area paths (e.g. bowties)
        // still keep their positive-winding lobes under negative offset, matching Clipper1.
        size_t write = 0;
        for (size_t i = 0; i < paths.size(); ++i) {
            const int s = c2_exact_area_sign(paths[i]);
            if (s == 0 && c2_is_collinear_path(paths[i]))
                continue;
            if (write != i)
                paths[write] = std::move(paths[i]);
            if (s < 0)
                has_cw = true;
            else if (s == 0)
                has_zero = true;
            area_signs.emplace_back(s);
            ++write;
        }
        paths.resize(write);
        if (paths.empty())
            return;
    } else {
        for (const C2::Path64 &path : paths) {
            const int s = c2_exact_area_sign(path);
            if (s < 0)
                has_cw = true;
            else if (s == 0)
                has_zero = true;
            area_signs.emplace_back(s);
        }
    }

    if (! has_cw && ! has_zero) {
        c2_offset_ccw_paths_isolated(std::move(paths), area_signs, delta, joinType, miterLimit, out);
        return;
    }

    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    if (c2_offset_as_group(paths, area_signs, delta, joinType, miterLimit)) {
        co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
        co.Execute(delta, out);
        return;
    }
    C2::Paths64 offsetted, single;
    for (size_t i = 0; i < paths.size(); ++i) {
        co.Clear();
        co.AddPath(paths[i], to_c2(joinType), C2::EndType::Polygon);
        // Clipper2 reverses a lone CW path, so the flipped sign shrinks it as a hole.
        co.Execute(area_signs[i] < 0 ? - delta : delta, single);
        append(offsetted, std::move(single));
    }
    c2_clip(C2::ClipType::Union, offsetted, {}, delta > 0. ? C2::FillRule::NonZero : C2::FillRule::Positive, out);
}

template<class PathsT, class TOut>
void c2_offset_closed(PathsT &&src, float delta, JoinType joinType, double miterLimit, TOut &out,
                      double shortest_edge_factor = ClipperOffsetShortestEdgeFactor)
{
    const double shortest = std::abs(delta * shortest_edge_factor);
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
// Isolate holes to their own entity on both positive and negative offsets, and isolate influence-overlapping
// holeless contours so concave self-intersections cannot cancel neighboring islands or fill neighboring holes.
template<class ExPolygonRange, class TOut>
void c2_offset_expolygons(const ExPolygonRange &src, float delta, JoinType joinType, double miterLimit, TOut &out)
{
    const double      shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64       combined;
    C2::Paths64       holeless_contours;
    std::vector<int>  holeless_signs;
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    size_t            entity_batches = 0;

    for (const auto &item : src) {
        const ExPolygon &expoly = expolygon_of(item);
        C2::Paths64      contour_paths;
        if (! append_offset_path(contour_paths, expoly.contour.points, shortest, C2::EndType::Polygon))
            continue;
        C2::Path64 &contour = contour_paths.front();
        const int   c_sign  = c2_exact_area_sign(contour);
        if (delta < 0.) {
            if (c_sign == 0 && c2_is_collinear_path(contour))
                continue;
        } else {
            if (c_sign == 0)
                continue;
        }
        if (c_sign < 0)
            std::reverse(contour.begin(), contour.end());
        const int norm_c_sign = (c_sign < 0) ? -c_sign : c_sign;

        if (expoly.holes.empty()) {
            holeless_contours.emplace_back(std::move(contour));
            holeless_signs.push_back(norm_c_sign);
            continue;
        }

        co.Clear();
        co.AddPath(contour, to_c2(joinType), C2::EndType::Polygon);
        C2::Paths64 offset_contour;
        co.Execute(delta, offset_contour);
        if (offset_contour.empty())
            continue;

        C2::Paths64      hole_inputs;
        std::vector<int> hole_signs;
        hole_inputs.reserve(expoly.holes.size());
        hole_signs.reserve(expoly.holes.size());
        for (const Polygon &hole : expoly.holes) {
            if (! append_offset_path(hole_inputs, hole.points, shortest, C2::EndType::Polygon))
                continue;
            const int h_sign = c2_exact_area_sign(hole_inputs.back());
            if (h_sign == 0 && (delta >= 0. || c2_is_collinear_path(hole_inputs.back()))) {
                hole_inputs.pop_back();
                continue;
            }
            if (h_sign < 0)
                std::reverse(hole_inputs.back().begin(), hole_inputs.back().end());
            hole_signs.push_back(h_sign < 0 ? -h_sign : h_sign);
        }

        if (hole_inputs.empty()) {
            append(combined, std::move(offset_contour));
            ++entity_batches;
            continue;
        }

        C2::Paths64 offset_holes;
        c2_offset_ccw_paths_isolated(std::move(hole_inputs), hole_signs, -delta, joinType, miterLimit, offset_holes);
        if (offset_holes.empty()) {
            append(combined, std::move(offset_contour));
            ++entity_batches;
            continue;
        }

        C2::Paths64 diff_out;
        c2_clip(C2::ClipType::Difference, offset_contour, offset_holes, C2::FillRule::NonZero, diff_out);
        if (! diff_out.empty()) {
            append(combined, std::move(diff_out));
            ++entity_batches;
        }
    }

    if (! holeless_contours.empty()) {
        C2::Paths64 holeless_out;
        c2_offset_ccw_paths_isolated(std::move(holeless_contours), holeless_signs, delta, joinType, miterLimit, holeless_out);
        if (! holeless_out.empty()) {
            append(combined, std::move(holeless_out));
            ++entity_batches;
        }
    }

    if (combined.empty())
        return;
    if constexpr (std::is_same_v<TOut, C2::PolyTree64>) {
        c2_clip(C2::ClipType::Union, combined, {}, C2::FillRule::NonZero, out);
    } else {
        if (entity_batches <= 1)
            out = std::move(combined);
        else
            c2_clip(C2::ClipType::Union, combined, {}, C2::FillRule::NonZero, out);
    }
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
Polygons c2_offset_lines(PathsProvider &&src, float delta, JoinType joinType, double miterLimit, EndType endType,
                         double shortest_edge_factor = ClipperOffsetShortestEdgeFactor)
{
    C2::Paths64 out;
    if (endType == etClosedPolygon) {
        c2_offset_closed(std::forward<PathsProvider>(src), delta, joinType, miterLimit, out, shortest_edge_factor);
        return c2_to_polygons(out);
    }
    const C2::EndType end      = to_c2(endType);
    const double      shortest = std::abs(delta * shortest_edge_factor);
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
C2::Paths64 c2_clip_paths(const PathsProvider &clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(clip);
    C2::Paths64 out;
    c2_offset_closed(clip, ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

C2::Paths64 c2_clip_paths(const ClipperUtils::ExPolygonProvider &clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(clip);
    C2::Paths64 out;
    c2_offset_expolygons(std::array<const ExPolygon*, 1>{ &clip.data() }, ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

C2::Paths64 c2_clip_paths(const ClipperUtils::ExPolygonsProvider &clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(clip);
    C2::Paths64 out;
    c2_offset_expolygons(clip.data(), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

C2::Paths64 c2_clip_paths(const ClipperUtils::SurfacesProvider &clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(clip);
    C2::Paths64 out;
    c2_offset_expolygons(clip.data(), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

C2::Paths64 c2_clip_paths(const ClipperUtils::SurfacesPtrProvider &clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(clip);
    C2::Paths64 out;
    c2_offset_expolygons(clip.data(), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

} // namespace

Slic3r::Polygons offset(const Slic3r::Polygon &polygon, const float delta, JoinType joinType, double miterLimit)
{
    C2::Paths64 paths;
    if (! append_offset_path(paths, polygon.points, std::abs(delta * ClipperOffsetShortestEdgeFactor), C2::EndType::Polygon))
        return {};
    const int area_sign = c2_exact_area_sign(paths.front());
    if (area_sign == 0 && delta < 0. && c2_is_collinear_path(paths.front()))
        return {};
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
    C2::Paths64 out;
    // A CW polygon shrinks as a hole and stays CW.
    co.Execute(area_sign < 0 ? - delta : delta, out);
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

Slic3r::Polygons offset(const Slic3r::Polyline &polyline, const float delta, JoinType joinType, double miterLimit, EndType end_type, double shortest_edge_factor)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::SinglePathProvider(polyline.points), delta, joinType, miterLimit, end_type, shortest_edge_factor); }

Slic3r::Polygons offset(const Slic3r::Polylines &polylines, const float delta, JoinType joinType, double miterLimit, EndType end_type, double shortest_edge_factor)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::PolylinesProvider(polylines), delta, joinType, miterLimit, end_type, shortest_edge_factor); }

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
    C2::PolyTree64 first_tree;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first_tree);
    const ExPolygons first_ex = c2_to_expolygons(first_tree);
    if (first_ex.empty())
        return {};
    C2::Paths64 out;
    c2_offset_expolygons(first_ex, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
ExPolygons offset2_ex(const ExPolygons &expolygons, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    C2::PolyTree64 first_tree;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first_tree);
    const ExPolygons first_ex = c2_to_expolygons(first_tree);
    if (first_ex.empty())
        return {};
    C2::PolyTree64 out;
    c2_offset_expolygons(first_ex, delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}
ExPolygons offset2_ex(const Surfaces &surfaces, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    C2::PolyTree64 first_tree;
    c2_offset_expolygons(surfaces, delta1, joinType, miterLimit, first_tree);
    const ExPolygons first_ex = c2_to_expolygons(first_tree);
    if (first_ex.empty())
        return {};
    C2::PolyTree64 out;
    c2_offset_expolygons(first_ex, delta2, joinType, miterLimit, out);
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
    C2::Paths64 first, out;
    c2_offset_expolygons(expolygons, - delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::Polygons opening(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 first, out;
    c2_offset_expolygons(surfaces, - delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
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

static void _clipper_pl_recombine(Polylines &polylines);

namespace ClipperUtils {

struct ClipEdgesIndex::Impl : OpenClipEdgeData {};

ClipEdgesIndex::ClipEdgesIndex() : m_impl(std::make_unique<Impl>()) {}
ClipEdgesIndex::~ClipEdgesIndex() = default;
ClipEdgesIndex::ClipEdgesIndex(ClipEdgesIndex &&) noexcept = default;
ClipEdgesIndex& ClipEdgesIndex::operator=(ClipEdgesIndex &&) noexcept = default;
ClipEdgesIndex::ClipEdgesIndex(const Polygons &clip) : ClipEdgesIndex() { build(clip); }
ClipEdgesIndex::ClipEdgesIndex(const ExPolygons &clip) : ClipEdgesIndex() { build(clip); }

void ClipEdgesIndex::build(const Polygons &clip)
{
    std::vector<Points> paths;
    paths.reserve(clip.size());
    for (const Polygon &p : clip)
        paths.push_back(p.points);
    m_impl->build_from_paths(paths);
}

void ClipEdgesIndex::build(const ExPolygons &clip)
{
    std::vector<Points> paths;
    paths.reserve(number_polygons(clip));
    for (const ExPolygon &ex : clip) {
        paths.push_back(ex.contour.points);
        for (const Polygon &h : ex.holes)
            paths.push_back(h.points);
    }
    m_impl->build_from_paths(paths);
}

bool ClipEdgesIndex::empty() const { return m_impl->empty(); }

void ClipEdgesIndex::clip_segment(const Point &a, const Point &b, OpenClipBoundaryRule rule, std::vector<OpenClipInterval> &out) const
{
    m_impl->clip_segment(a, b, rule, out);
}

void ClipEdgesIndex::clip_path(const Points &path, bool is_closed, OpenClipBoundaryRule rule, std::vector<OpenClipPathInterval> &out) const
{
    m_impl->clip_path(path, is_closed, rule, out);
}

void clip_closed_polygon_split(const Polygon &subject, const Polygons &clip, Polylines &inside_out, Polylines &outside_out)
{
    inside_out.clear();
    outside_out.clear();
    const size_t n = subject.points.size();
    if (n < 2)
        return;
    if (clip.empty()) {
        outside_out.emplace_back(subject.split_at_first_point());
        return;
    }

    ClipEdgesIndex index(clip);
    if (index.empty()) {
        outside_out.emplace_back(subject.split_at_first_point());
        return;
    }

    Polyline cur_in, cur_out;
    auto append_iv = [](Polyline &cur, Polylines &dst, const Point &p0, const Point &p1) {
        if (p0 == p1)
            return;
        if (cur.points.empty()) {
            cur.points.push_back(p0);
            cur.points.push_back(p1);
        } else {
            if (cur.points.back() != p0) {
                if (cur.points.size() >= 2)
                    dst.emplace_back(std::move(cur));
                cur.points.clear();
                cur.points.push_back(p0);
            }
            if (cur.points.back() != p1)
                cur.points.push_back(p1);
        }
    };
    auto flush_cur = [](Polyline &cur, Polylines &dst) {
        if (cur.points.size() >= 2)
            dst.emplace_back(std::move(cur));
        cur.points.clear();
    };

    std::vector<OpenClipPathInterval> intervals;
    index.clip_path(subject.points, true, OpenClipBoundaryRule::Clipper1ExcludeTop, intervals);
    for (const OpenClipPathInterval &iv : intervals) {
        if (iv.p0 == iv.p1)
            continue;
        if (iv.inside) {
            flush_cur(cur_out, outside_out);
            append_iv(cur_in, inside_out, iv.p0, iv.p1);
        } else {
            flush_cur(cur_in, inside_out);
            append_iv(cur_out, outside_out, iv.p0, iv.p1);
        }
    }
    flush_cur(cur_in, inside_out);
    flush_cur(cur_out, outside_out);
    _clipper_pl_recombine(inside_out);
    _clipper_pl_recombine(outside_out);
}

} // namespace ClipperUtils

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

static void traverse_pt_outside_in(const C2::PolyPath64 &parent, Polygons *retval)
{
    // collect ordering points
    Points ordering_points;
    ordering_points.reserve(parent.Count());
    for (const auto &node : parent)
        ordering_points.emplace_back(node->Polygon().front().x, node->Polygon().front().y);

    // Perform the ordering, push results recursively.
    //FIXME pass the last point to chain_points?
    for (size_t idx : chain_points(ordering_points)) {
        const C2::PolyPath64 &node = *parent.Child(idx);
        retval->emplace_back().points = c2_to_points(node.Polygon());
        if (node.IsHole())
            // Orient a hole, which is clockwise oriented, to CCW.
            retval->back().reverse();
        // traverse the next depth
        traverse_pt_outside_in(node, retval);
    }
}

static void traverse_rebuilt_outside_in(const C2RebuiltTree &rebuilt, const std::vector<size_t> &children, Polygons *retval)
{
    if (children.empty())
        return;
    Points ordering_points;
    ordering_points.reserve(children.size());
    for (size_t child_idx : children) {
        const C2::Point64 &pt = rebuilt.nodes[child_idx].path->front();
        ordering_points.emplace_back(pt.x, pt.y);
    }
    for (size_t idx : chain_points(ordering_points)) {
        const C2RebuiltNode &node = rebuilt.nodes[children[idx]];
        retval->emplace_back().points = c2_to_points(*node.path);
        if (node.area < 0.)
            // Orient a hole, which is clockwise oriented, to CCW.
            retval->back().reverse();
        traverse_rebuilt_outside_in(rebuilt, node.children, retval);
    }
}

Polygons union_pt_chained_outside_in(const Polygons &subject)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::EvenOdd, tree);
    Polygons retval;
    if (c2_polytree_is_well_nested(tree))
        traverse_pt_outside_in(tree, &retval);
    else {
        const C2RebuiltTree rebuilt = c2_rebuild_polytree(tree);
        traverse_rebuilt_outside_in(rebuilt, rebuilt.roots, &retval);
    }
    return retval;
}

static bool has_repeated_vertex(const Points &pts)
{
    if (pts.size() < 4)
        return false;
    std::unordered_set<Point, PointHash> seen;
    seen.reserve(pts.size());
    for (const Point &pt : pts)
        if (! seen.insert(pt).second)
            return true;
    return false;
}

static void remove_exact_collinear(Points &pts)
{
    bool changed = true;
    while (changed && pts.size() >= 3) {
        changed = false;
        Points next;
        next.reserve(pts.size());
        const size_t n = pts.size();
        for (size_t i = 0; i < n; ++i) {
            const Point &prev = pts[(i + n - 1) % n];
            const Point &curr = pts[i];
            const Point &succ = pts[(i + 1) % n];
            if (curr == prev || int128::orient(prev, curr, succ) == 0) {
                changed = true;
                continue;
            }
            next.push_back(curr);
        }
        pts = std::move(next);
    }
}

// Insert vertices that lie strictly inside a non-adjacent edge of the same contour so
// stack-based loop splitting can separate vertex-on-edge self-touching contours.
static bool insert_touching_vertices(Points &pts)
{
    const size_t n = pts.size();
    if (n < 4)
        return false;

    std::vector<std::vector<Point>> insertions(n);
    bool                            any_inserted = false;

    auto test_vertex_against_edge = [&](size_t edge_idx, const Point &v) {
        const Point &a = pts[edge_idx];
        const Point &b = pts[(edge_idx + 1 == n) ? 0 : edge_idx + 1];
        if (a == b || v == a || v == b)
            return;
        if (v.x() < std::min(a.x(), b.x()) || v.x() > std::max(a.x(), b.x()) ||
            v.y() < std::min(a.y(), b.y()) || v.y() > std::max(a.y(), b.y()))
            return;
        if (int128::orient(a, b, v) == 0) {
            insertions[edge_idx].push_back(v);
            any_inserted = true;
        }
    };

    if (n <= 32) {
        for (size_t k = 0; k < n; ++k)
            for (size_t i = 0; i < n; ++i)
                test_vertex_against_edge(i, pts[k]);
    } else {
        std::vector<C2BBoxNode> edge_nodes;
        edge_nodes.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const Point &a = pts[i];
            const Point &b = pts[(i + 1 == n) ? 0 : i + 1];
            edge_nodes.emplace_back(i, C2::Rect64(std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::max(a.x(), b.x()), std::max(a.y(), b.y())));
        }
        AABBTreeIndirect::Tree<2, coord_t> edge_tree;
        edge_tree.build(std::move(edge_nodes));
        for (size_t k = 0; k < n; ++k) {
            const Point &v = pts[k];
            AABBTreeIndirect::traverse(
                edge_tree,
                [&v](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) { return node.bbox.contains(v); },
                [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &node) {
                    test_vertex_against_edge(node.idx, v);
                    return true;
                });
        }
    }

    if (! any_inserted)
        return false;

    Points out;
    out.reserve(n + 8);
    for (size_t i = 0; i < n; ++i) {
        const Point &a = pts[i];
        out.push_back(a);
        if (! insertions[i].empty()) {
            auto &ins = insertions[i];
            auto  dist_along = [&a](const Point &p) -> uint64_t {
                const uint64_t dx = uint64_t(p.x() >= a.x() ? p.x() - a.x() : a.x() - p.x());
                const uint64_t dy = uint64_t(p.y() >= a.y() ? p.y() - a.y() : a.y() - p.y());
                return dx + dy;
            };
            std::sort(ins.begin(), ins.end(), [&dist_along](const Point &u, const Point &v) {
                return dist_along(u) < dist_along(v);
            });
            ins.erase(std::unique(ins.begin(), ins.end()), ins.end());
            out.insert(out.end(), ins.begin(), ins.end());
        }
    }
    pts = std::move(out);
    return true;
}

// Split a contour at every repeated vertex or vertex-on-edge touch into strictly simple loops (Clipper1 StrictlySimple semantics).
static void split_self_touching_polygon(Points pts, Polygons &out)
{
    if (pts.size() < 3)
        return;
    insert_touching_vertices(pts);
    if (! has_repeated_vertex(pts)) {
        if (Polygon::area(pts) != 0.)
            out.emplace_back(std::move(pts));
        return;
    }

    Points                                    stack;
    std::unordered_map<Point, size_t, PointHash> pos;
    stack.reserve(pts.size());
    pos.reserve(pts.size());

    auto emit_loop = [&out](Points loop) {
        remove_exact_collinear(loop);
        if (loop.size() >= 3 && Polygon::area(loop) != 0.)
            out.emplace_back(std::move(loop));
    };

    for (const Point &pt : pts) {
        if (auto it = pos.find(pt); it != pos.end()) {
            const size_t k = it->second;
            Points loop(stack.begin() + k, stack.end());
            for (size_t j = k + 1; j < stack.size(); ++j)
                pos.erase(stack[j]);
            stack.resize(k + 1);
            emit_loop(std::move(loop));
        } else {
            pos.emplace(pt, stack.size());
            stack.push_back(pt);
        }
    }
    emit_loop(std::move(stack));
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
    for (size_t i = 1; i <= cnt; ++i) {
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

static bool simple_ring_encloses(const Polygon &outer, const Polygon &inner)
{
    constexpr int64_t kTol  = 3;
    constexpr double  kTol2 = double(kTol * kTol);
    // 1. Try any vertex of `inner` cleanly away from `outer`'s boundary.
    for (const Point &pt : inner.points) {
        const int pip = point_in_polygon_doubled(int64_t(pt.x()) * 2, int64_t(pt.y()) * 2, outer.points);
        if (pip != -1 && ! point_near_boundary_templ(pt, outer.points, kTol, kTol2))
            return pip == 1;
    }
    // 2. Try a scanline strict interior point of `inner` (avoids near-boundary vertex rounding ambiguity).
    double mx = 0., my = 0.;
    if (find_interior_point_templ(inner.points, mx, my))
        return point_in_path_double_templ(mx, my, outer.points);
    // 3. Fallback: exact doubled-coordinate edge midpoints.
    const size_t n = inner.points.size();
    for (size_t i = 0; i < n; ++i) {
        const Point &a   = inner.points[i];
        const Point &b   = inner.points[(i + 1 == n) ? 0 : i + 1];
        const int    pip = point_in_polygon_doubled(int64_t(a.x()) + int64_t(b.x()), int64_t(a.y()) + int64_t(b.y()), outer.points);
        if (pip != -1)
            return pip == 1;
    }
    // 4. Last resort: majority of inner vertices inside outer.
    size_t inside_cnt = 0;
    for (const Point &pt : inner.points)
        if (point_in_polygon_doubled(int64_t(pt.x()) * 2, int64_t(pt.y()) * 2, outer.points) == 1)
            ++inside_cnt;
    return inside_cnt * 2 > inner.points.size();
}

Polygons simplify_polygons(const Polygons &subject)
{
    C2::Paths64 united;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, united);
    Polygons out;
    out.reserve(united.size());
    for (const C2::Path64 &path : united)
        split_self_touching_polygon(c2_to_points(path), out);
    return out;
}

ExPolygons simplify_polygons_ex(const Polygons &subject)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, tree);
    ExPolygons raw = c2_to_expolygons(tree);
    ExPolygons out;
    out.reserve(raw.size());

    for (ExPolygon &ex : raw) {
        bool needs_split = has_repeated_vertex(ex.contour.points) || insert_touching_vertices(ex.contour.points);
        if (! needs_split) {
            for (Polygon &h : ex.holes)
                if (has_repeated_vertex(h.points) || insert_touching_vertices(h.points)) {
                    needs_split = true;
                    break;
                }
        }
        if (! needs_split) {
            out.emplace_back(std::move(ex));
            continue;
        }

        // Split only this ExPolygon's contours and re-nest its holes locally.
        Polygons split_rings;
        split_self_touching_polygon(std::move(ex.contour.points), split_rings);
        for (Polygon &h : ex.holes)
            split_self_touching_polygon(std::move(h.points), split_rings);

        struct SimpleRing
        {
            Polygon     poly;
            double      area   = 0.;
            BoundingBox bbox;
            bool        is_ccw = false;
        };
        std::vector<SimpleRing> rings;
        rings.reserve(split_rings.size());
        std::vector<size_t> ccw_indices;
        std::vector<size_t> cw_indices;
        for (Polygon &p : split_rings) {
            SimpleRing r;
            r.area   = p.area();
            r.is_ccw = r.area > 0.;
            r.bbox   = get_extents(p);
            r.poly   = std::move(p);
            const size_t idx = rings.size();
            rings.emplace_back(std::move(r));
            if (rings.back().is_ccw)
                ccw_indices.push_back(idx);
            else
                cw_indices.push_back(idx);
        }

        const size_t base_out = out.size();
        out.resize(base_out + ccw_indices.size());
        // Cache contour copies/references before moving; keep rings[idx].poly alive until hole assignment finishes.
        if (! cw_indices.empty() && ! ccw_indices.empty()) {
            constexpr coord_t kPad = 3;
            std::vector<size_t> unmatched_holes;
            if (ccw_indices.size() == 1) {
                const size_t      c_ring     = ccw_indices.front();
                const BoundingBox padded_box = rings[c_ring].bbox.inflated(kPad);
                for (size_t h_ring : cw_indices) {
                    if (padded_box.contains(rings[h_ring].bbox) &&
                        simple_ring_encloses(rings[c_ring].poly, rings[h_ring].poly))
                        out[base_out].holes.emplace_back(std::move(rings[h_ring].poly));
                    else
                        unmatched_holes.push_back(h_ring);
                }
            } else {
                std::vector<C2BBoxNode> bbox_nodes;
                bbox_nodes.reserve(ccw_indices.size());
                for (size_t k = 0; k < ccw_indices.size(); ++k) {
                    const BoundingBox &bb = rings[ccw_indices[k]].bbox;
                    bbox_nodes.emplace_back(k, C2::Rect64(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()), kPad);
                }
                AABBTreeIndirect::Tree<2, coord_t> aabb;
                aabb.build(std::move(bbox_nodes));

                std::vector<size_t> candidates;
                for (size_t h_ring : cw_indices) {
                    const BoundingBox &hbb     = rings[h_ring].bbox;
                    const double       h_abs   = -rings[h_ring].area;
                    const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query_box(hbb.min, hbb.max);
                    candidates.clear();
                    AABBTreeIndirect::traverse(
                        aabb,
                        [&query_box](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) { return n.bbox.contains(query_box); },
                        [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) {
                            if (rings[ccw_indices[n.idx]].area > h_abs)
                                candidates.push_back(n.idx);
                            return true;
                        });
                    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
                        return rings[ccw_indices[a]].area < rings[ccw_indices[b]].area;
                    });
                    bool assigned = false;
                    for (size_t k : candidates) {
                        if (simple_ring_encloses(rings[ccw_indices[k]].poly, rings[h_ring].poly)) {
                            out[base_out + k].holes.emplace_back(std::move(rings[h_ring].poly));
                            assigned = true;
                            break;
                        }
                    }
                    if (! assigned)
                        unmatched_holes.push_back(h_ring);
                }
            }

            if (! unmatched_holes.empty()) {
                std::vector<size_t> ccw_by_area(ccw_indices.size());
                std::iota(ccw_by_area.begin(), ccw_by_area.end(), size_t(0));
                std::sort(ccw_by_area.begin(), ccw_by_area.end(), [&](size_t a, size_t b) {
                    return rings[ccw_indices[a]].area < rings[ccw_indices[b]].area;
                });
                for (size_t h_ring : unmatched_holes) {
                    const BoundingBox padded_h = rings[h_ring].bbox.inflated(kPad);
                    const double      h_abs    = -rings[h_ring].area;
                    for (size_t k : ccw_by_area) {
                        const SimpleRing &cr = rings[ccw_indices[k]];
                        if (cr.area <= h_abs || ! cr.bbox.inflated(kPad).overlap(padded_h))
                            continue;
                        const ExPolygons diff_res = diff_ex(Polygons{ cr.poly }, Polygons{ rings[h_ring].poly });
                        double           diff_area = 0.;
                        for (const ExPolygon &d : diff_res)
                            diff_area += d.area();
                        if (diff_area < cr.area - 0.25 * h_abs) {
                            out[base_out + k].holes.emplace_back(std::move(rings[h_ring].poly));
                            break;
                        }
                    }
                }
            }
        }
        for (size_t k = 0; k < ccw_indices.size(); ++k)
            out[base_out + k].contour = std::move(rings[ccw_indices[k]].poly);
    }
    return out;
}

Polygons top_level_islands(const Slic3r::Polygons &polygons)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(polygons)), {}, C2::FillRule::EvenOdd, tree);
    Polygons out;
    if (c2_polytree_is_well_nested(tree)) {
        out.reserve(tree.Count());
        for (const auto &island : tree)
            out.emplace_back().points = c2_to_points(island->Polygon());
        return out;
    }
    const C2RebuiltTree rebuilt = c2_rebuild_polytree(tree);
    out.reserve(rebuilt.roots.size());
    for (size_t root_idx : rebuilt.roots)
        if (rebuilt.nodes[root_idx].area > 0.)
            out.emplace_back().points = c2_to_points(*rebuilt.nodes[root_idx].path);
    return out;
}

ExPolygons top_level_expolygons(const ExPolygons &expolygons, ExPolygons *nested)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::ExPolygonsProvider(expolygons)), {}, C2::FillRule::EvenOdd, tree);
    ExPolygons out;
    if (c2_polytree_is_well_nested(tree)) {
        out.reserve(tree.Count());
        for (const auto &outer : tree) {
            ExPolygon &expoly = out.emplace_back();
            expoly.contour.points = c2_to_points(outer->Polygon());
            for (const auto &hole : *outer) {
                expoly.holes.emplace_back().points = c2_to_points(hole->Polygon());
                if (nested)
                    for (const auto &island : *hole)
                        c2_append_expolygons(*island, *nested);
            }
        }
        return out;
    }
    const C2RebuiltTree rebuilt = c2_rebuild_polytree(tree);
    out.reserve(rebuilt.roots.size());
    for (size_t root_idx : rebuilt.roots) {
        const C2RebuiltNode &outer = rebuilt.nodes[root_idx];
        if (outer.area <= 0.)
            continue;
        ExPolygon &expoly = out.emplace_back();
        expoly.contour.points = c2_to_points(*outer.path);
        expoly.holes.reserve(outer.children.size());
        for (size_t hole_idx : outer.children) {
            const C2RebuiltNode &hole = rebuilt.nodes[hole_idx];
            expoly.holes.emplace_back().points = c2_to_points(*hole.path);
            if (nested)
                for (size_t island_idx : hole.children)
                    c2_append_rebuilt_expolygons(rebuilt, island_idx, *nested);
        }
    }
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
		assert(c2_exact_area_sign(c) > 0);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	C2::Paths64 holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, unite(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), C2::FillRule::Negative));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(c2_exact_area_sign(c) > 0);
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
