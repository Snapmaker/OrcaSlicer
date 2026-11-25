#include "ClipperZUtils.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "AABBTreeIndirect.hpp"
#include <clipper2/clipper2_z.hpp>

namespace Slic3r {
namespace ClipperZUtils {

namespace C2 = Clipper2Lib_Z;

static C2::Paths64 to_paths64(const ZPaths &paths)
{
    C2::Paths64 out;
    out.reserve(paths.size());
    for (const ZPath &path : paths) {
        C2::Path64 &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const ZPoint &pt : path)
            dst.emplace_back(pt.x(), pt.y(), pt.z());
    }
    return out;
}

static void append_zpaths(const C2::Paths64 &paths, ZPaths &out)
{
    out.reserve(out.size() + paths.size());
    for (const C2::Path64 &path : paths) {
        ZPath &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const C2::Point64 &pt : path)
            dst.emplace_back(pt.x, pt.y, pt.z);
    }
}

double area(const ZPath &path)
{
    const size_t size = path.size();
    if (size < 3)
        return 0.;
    double a = 0.;
    for (size_t i = 0, j = size - 1; i < size; j = i ++)
        a += (double(path[j].x()) + path[i].x()) * (double(path[j].y()) - path[i].y());
    return -a * 0.5;
}

static C2::ClipType to_c2(ClipType clip_type)
{
    return clip_type == ctIntersection ? C2::ClipType::Intersection :
           clip_type == ctUnion        ? C2::ClipType::Union :
           clip_type == ctDifference   ? C2::ClipType::Difference : C2::ClipType::Xor;
}

static C2::FillRule to_c2(PolyFillType fill)
{
    return fill == pftEvenOdd  ? C2::FillRule::EvenOdd :
           fill == pftPositive ? C2::FillRule::Positive :
           fill == pftNegative ? C2::FillRule::Negative : C2::FillRule::NonZero;
}

// Shared setup of the Clipper2 engine for the boolean helpers below.
static void setup_clipper(C2::Clipper64 &clipper, bool preserve_collinear, const ZFillCallback &zfill,
    const ZPaths &subject, bool subject_open, const ZPaths &clip)
{
    clipper.PreserveCollinear(preserve_collinear);
    if (zfill)
        clipper.SetZCallback([&zfill](const C2::Point64 &e1bot, const C2::Point64 &e1top, const C2::Point64 &e2bot, const C2::Point64 &e2top, C2::Point64 &pt) {
            // Clipper2 already copied Z from a coincident input vertex.
            auto coincident = [&pt](const C2::Point64 &p) { return p.x == pt.x && p.y == pt.y; };
            if (coincident(e1bot) || coincident(e1top) || coincident(e2bot) || coincident(e2top))
                return;
            ZPoint zpt(pt.x, pt.y, pt.z);
            zfill(ZPoint(e1bot.x, e1bot.y, e1bot.z), ZPoint(e1top.x, e1top.y, e1top.z),
                  ZPoint(e2bot.x, e2bot.y, e2bot.z), ZPoint(e2top.x, e2top.y, e2top.z), zpt);
            pt.z = zpt.z();
        });
    if (subject_open)
        clipper.AddOpenSubject(to_paths64(subject));
    else
        clipper.AddSubject(to_paths64(subject));
    clipper.AddClip(to_paths64(clip));
}

ZPaths clip_zpaths(ClipType clip_type, const ZPaths &subject, bool subject_open, const ZPaths &clip, const ZFillCallback &zfill, bool preserve_collinear, PolyFillType fill)
{
    C2::Clipper64 clipper;
    setup_clipper(clipper, preserve_collinear, zfill, subject, subject_open, clip);
    C2::Paths64 closed, open;
    clipper.Execute(to_c2(clip_type), to_c2(fill), closed, open);
    ZPaths out;
    append_zpaths(closed, out);
    append_zpaths(open, out);
    return out;
}

namespace {

struct C2ZBBoxNode
{
    size_t                                         m_idx;
    AABBTreeIndirect::Tree<2, coord_t>::BoundingBox m_bbox;
    Vec2crd                                        m_centroid;

    C2ZBBoxNode(size_t i, const C2::Rect64 &r, int64_t pad = 0)
        : m_idx(i)
        , m_bbox(Vec2crd(r.left - pad, r.top - pad), Vec2crd(r.right + pad, r.bottom + pad))
        , m_centroid(r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2)
    {}

    size_t                                                idx() const { return m_idx; }
    const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox &bbox() const { return m_bbox; }
    const Vec2crd                                         &centroid() const { return m_centroid; }
};

int c2z_point_in_path(const C2::Point64 &pt, const C2::Path64 &path)
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

bool c2z_point_near_boundary(const C2::Point64 &pt, const C2::Path64 &outer, int64_t tol, double tol2)
{
    const size_t n = outer.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const C2::Point64 &a = outer[j];
        const C2::Point64 &b = outer[i];
        if (pt.x < std::min(a.x, b.x) - tol || pt.x > std::max(a.x, b.x) + tol ||
            pt.y < std::min(a.y, b.y) - tol || pt.y > std::max(a.y, b.y) + tol)
            continue;
        const double dx   = double(b.x - a.x);
        const double dy   = double(b.y - a.y);
        const double px   = double(pt.x - a.x);
        const double py   = double(pt.y - a.y);
        const double len2 = dx * dx + dy * dy;
        double       d2;
        if (len2 == 0.) {
            d2 = px * px + py * py;
        } else {
            const double t  = std::clamp((px * dx + py * dy) / len2, 0., 1.);
            const double rx = px - t * dx;
            const double ry = py - t * dy;
            d2 = rx * rx + ry * ry;
        }
        if (d2 <= tol2)
            return true;
    }
    return false;
}

bool c2z_point_in_path_double(double px, double py, const C2::Path64 &path)
{
    bool         inside = false;
    const size_t n      = path.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double yi = double(path[i].y);
        const double yj = double(path[j].y);
        if ((yi > py) != (yj > py)) {
            const double xi = double(path[i].x);
            const double xj = double(path[j].x);
            if ((px - xi) < (py - yi) * (xj - xi) / (yj - yi))
                inside = ! inside;
        }
    }
    return inside;
}

bool c2z_find_interior_point(const C2::Path64 &path, double &mx, double &my)
{
    const size_t n = path.size();
    if (n < 3)
        return false;
    std::vector<int64_t> ys;
    ys.reserve(n);
    for (const C2::Point64 &p : path)
        ys.push_back(p.y);
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
            const double yi = double(path[i].y);
            const double yj = double(path[j].y);
            if ((yi < y_mid && yj > y_mid) || (yj < y_mid && yi > y_mid)) {
                const double xi = double(path[i].x);
                const double xj = double(path[j].x);
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

bool c2z_path_encloses(const C2::Path64 &outer, const C2::Path64 &inner)
{
    constexpr int64_t kTol  = 3;
    constexpr double  kTol2 = double(kTol * kTol);
    for (const C2::Point64 &pt : inner) {
        const int pip = c2z_point_in_path(pt, outer);
        if (pip == -1)
            continue;
        if (! c2z_point_near_boundary(pt, outer, kTol, kTol2))
            return pip == 1;
    }
    double mx = 0., my = 0.;
    if (c2z_find_interior_point(inner, mx, my))
        return c2z_point_in_path_double(mx, my, outer);
    size_t inside_cnt = 0;
    for (const C2::Point64 &pt : inner)
        if (c2z_point_in_path(pt, outer) == 1)
            ++inside_cnt;
    return inside_cnt * 2 > inner.size();
}

bool c2z_polytree_is_well_nested(const C2::PolyPath64 &node, unsigned depth)
{
    const double a = C2::Area(node.Polygon());
    if ((depth % 2 == 0) ? (a <= 0.) : (a >= 0.))
        return false;
    for (const auto &child : node)
        if (! c2z_polytree_is_well_nested(*child, depth + 1))
            return false;
    return true;
}

bool c2z_polytree_is_well_nested(const C2::PolyTree64 &tree)
{
    for (const auto &child : tree)
        if (! c2z_polytree_is_well_nested(*child, 0))
            return false;
    return true;
}

struct C2ZRebuiltNode
{
    const C2::Path64   *path   = nullptr;
    double              area   = 0.;
    C2::Rect64          bounds;
    std::vector<size_t> children;
};

void c2z_collect_polytree_paths(const C2::PolyPath64 &node, std::vector<C2ZRebuiltNode> &items)
{
    const C2::Path64 &poly = node.Polygon();
    if (poly.size() >= 3) {
        const double a = C2::Area(poly);
        if (a != 0.) {
            C2ZRebuiltNode item;
            item.path   = &poly;
            item.area   = a;
            item.bounds = C2::GetBounds(poly);
            items.emplace_back(std::move(item));
        }
    }
    for (const auto &child : node)
        c2z_collect_polytree_paths(*child, items);
}

void walk_rebuilt_zpolytree(const std::vector<C2ZRebuiltNode> &nodes, size_t idx, unsigned depth,
    const std::function<void(const ZPath &, unsigned)> &callback)
{
    ZPath contour;
    const C2::Path64 &polygon = *nodes[idx].path;
    contour.reserve(polygon.size());
    for (const C2::Point64 &pt : polygon)
        contour.emplace_back(pt.x, pt.y, pt.z);
    callback(contour, depth);
    for (size_t child_idx : nodes[idx].children)
        walk_rebuilt_zpolytree(nodes, child_idx, depth + 1, callback);
}

} // namespace

static void walk_polytree(const C2::PolyPath64 &node, unsigned depth, const std::function<void(const ZPath &, unsigned)> &callback)
{
    ZPath contour;
    const C2::Path64 &polygon = node.Polygon();
    contour.reserve(polygon.size());
    for (const C2::Point64 &pt : polygon)
        contour.emplace_back(pt.x, pt.y, pt.z);
    callback(contour, depth);
    for (const auto &child : node)
        walk_polytree(*child, depth + 1, callback);
}

void clip_zpaths_polytree(ClipType clip_type, const ZPaths &subject, bool subject_open, const ZPaths &clip, const ZFillCallback &zfill,
    const std::function<void(const ZPath &contour, unsigned depth)> &callback, bool preserve_collinear, PolyFillType fill)
{
    C2::Clipper64 clipper;
    setup_clipper(clipper, preserve_collinear, zfill, subject, subject_open, clip);
    C2::PolyTree64 polytree;
    C2::Paths64    open;
    clipper.Execute(to_c2(clip_type), to_c2(fill), polytree, open);
    if (c2z_polytree_is_well_nested(polytree)) {
        // The root node carries no contour; its children are the top level contours.
        for (const auto &child : polytree)
            walk_polytree(*child, 0, callback);
        return;
    }

    // Reconstruct exact nesting hierarchy when Clipper2 PolyTree64 has misnested nodes.
    std::vector<C2ZRebuiltNode> nodes;
    for (const auto &child : polytree)
        c2z_collect_polytree_paths(*child, nodes);
    if (nodes.empty())
        return;
    std::sort(nodes.begin(), nodes.end(), [](const C2ZRebuiltNode &a, const C2ZRebuiltNode &b) {
        return std::abs(a.area) > std::abs(b.area);
    });

    constexpr int64_t kPad = 3;
    std::vector<C2ZBBoxNode> bbox_nodes;
    bbox_nodes.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        bbox_nodes.emplace_back(i, nodes[i].bounds, kPad);

    AABBTreeIndirect::Tree<2, coord_t> aabb;
    aabb.build(std::move(bbox_nodes));

    std::vector<size_t> roots;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const C2ZRebuiltNode &cur     = nodes[i];
        const double          cur_abs = std::abs(cur.area);
        const bool            cur_pos = cur.area > 0.;
        const AABBTreeIndirect::Tree<2, coord_t>::BoundingBox query_box(
            Vec2crd(cur.bounds.left, cur.bounds.top), Vec2crd(cur.bounds.right, cur.bounds.bottom));
        candidates.clear();
        AABBTreeIndirect::traverse(
            aabb,
            [&query_box](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) { return n.bbox.contains(query_box); },
            [&](const AABBTreeIndirect::Tree<2, coord_t>::Node &n) {
                if (n.idx < i) {
                    const C2ZRebuiltNode &cand = nodes[n.idx];
                    if ((cand.area > 0.) != cur_pos && std::abs(cand.area) > cur_abs)
                        candidates.push_back(n.idx);
                }
                return true;
            });
        std::sort(candidates.begin(), candidates.end(), [&nodes](size_t a, size_t b) {
            return std::abs(nodes[a].area) < std::abs(nodes[b].area);
        });
        size_t parent = size_t(-1);
        for (size_t cand_idx : candidates) {
            if (c2z_path_encloses(*nodes[cand_idx].path, *cur.path)) {
                parent = cand_idx;
                break;
            }
        }
        if (parent == size_t(-1))
            roots.push_back(i);
        else
            nodes[parent].children.push_back(i);
    }
    for (size_t root_idx : roots)
        walk_rebuilt_zpolytree(nodes, root_idx, 0, callback);
}

} // namespace ClipperZUtils
} // namespace Slic3r
