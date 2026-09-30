#pragma once
// Ultra assembly "Exact highlight": transfer the TARGET face's own shape onto the MOVING part at the
// cursor (the "footprint"), and build the rigid mate that lands that footprint back on the target face.
// Used when the moving part's face is larger than the target's, where a centroid-to-centroid mate puts
// the contact wherever the big face's centre happens to be. Header-only; Eigen + indexed_triangle_set.
#include <vector>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <limits>
#include <Eigen/Geometry>
#include "libslic3r/Point.hpp"
#include <admesh/stl.h>

namespace Slic3r { namespace GUI { namespace UltraFit {

struct ExactFootprintInput
{
    // Target (fixed) face: facet list on t_its, mesh -> common-frame transform, outward normal and centre
    // in the common frame, and an optional in-plane reference axis (zero = none).
    const indexed_triangle_set* t_its{nullptr};
    const std::vector<int>*     t_facets{nullptr};
    Transform3d                 t_w{Transform3d::Identity()};
    Vec3d                       t_normal{Vec3d::UnitZ()};
    Vec3d                       t_centre{Vec3d::Zero()};
    Vec3d                       t_axis{Vec3d::Zero()};
    // Moving face under the cursor: facet list on a_its, its transform, the cursor hit point and the
    // outward normal there (common frame), and an optional in-plane reference axis (zero = none).
    const indexed_triangle_set* a_its{nullptr};
    const std::vector<int>*     a_facets{nullptr};
    Transform3d                 a_w{Transform3d::Identity()};
    Vec3d                       a_hit{Vec3d::Zero()};
    Vec3d                       a_normal{Vec3d::UnitZ()};
    Vec3d                       a_axis{Vec3d::Zero()};
    // Symmetry of the two reference axes: M_PI for a long axis (a rectangle's), M_PI/2 for a square's
    // edges. The roll is folded into (-fold/2, fold/2] so it never spins further than the symmetry needs.
    double                      axis_fold{M_PI};
    // true: drape the footprint onto the moving surface (curved faces). false: the moving face is flat,
    // so the footprint is the target shape laid on its plane.
    bool                        a_curved{false};
};

struct ExactFootprint
{
    bool               ok{false};
    // Footprint triangles on the moving part, 3 points each, in the common frame, wound to face out of it.
    std::vector<Vec3d> tris;
    Vec3d              anchor{Vec3d::Zero()};  // cursor point on the moving part; the target centre lands here
    Vec3d              normal{Vec3d::UnitZ()}; // moving part's outward normal under the footprint
    // Moves the MOVING part (common frame) so the footprint lands exactly on the target face.
    Transform3d        mate{Transform3d::Identity()};
    double             roll{0.0};              // radians of edge-alignment spin about the target normal
    double             contact_shift{0.0};     // mm slid along the target normal so the surfaces just touch
    int                draped{0};              // footprint vertices that found the moving surface
    int                missed{0};              // ... that hang past its edge (kept on the rigid placement)
};

// Spin about n that turns direction a onto direction b (both projected into the plane of n), folded by
// the axes' symmetry so a long axis is never flipped end for end.
inline double exact_axis_roll(const Vec3d& n_in, Vec3d a, Vec3d b, double fold)
{
    const Vec3d n = n_in.normalized();
    a -= n * a.dot(n); b -= n * b.dot(n);
    if (a.norm() < 1e-9 || b.norm() < 1e-9 || fold <= 0.0) return 0.0;
    a.normalize(); b.normalize();
    double t = std::atan2(n.dot(a.cross(b)), a.dot(b));
    t = std::fmod(t, fold);
    if (t >  fold / 2) t -= fold;
    if (t <= -fold / 2) t += fold;
    return t;
}

namespace detail {

// Line (not ray) through o along d vs triangle (a,b,c): returns the line parameter, or NaN on a miss.
inline double exact_line_tri(const Vec3d& o, const Vec3d& d, const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Vec3d e1 = b - a, e2 = c - a, p = d.cross(e2);
    const double det = e1.dot(p);
    if (std::abs(det) < 1e-14) return nan;
    const double inv = 1.0 / det;
    const Vec3d s = o - a;
    const double u = s.dot(p) * inv;
    if (u < -1e-9 || u > 1.0 + 1e-9) return nan;
    const Vec3d q = s.cross(e1);
    const double v = d.dot(q) * inv;
    if (v < -1e-9 || u + v > 1.0 + 1e-9) return nan;
    return e2.dot(q) * inv;
}

// The moving patch bucketed on a 2D grid in the plane of n, so each footprint vertex tests only the
// triangles above / below it instead of the whole (possibly 20000-facet) patch.
struct PatchGrid
{
    std::vector<std::array<Vec3d, 3>> tris;
    Vec3d n, e1, e2, o;
    double cell{1.0};
    int nx{0}, ny{0};
    double minx{0}, miny{0};
    std::vector<std::vector<int>> cells;

    void build(std::vector<std::array<Vec3d, 3>> t, const Vec3d& n_in, const Vec3d& origin)
    {
        tris = std::move(t); n = n_in.normalized(); e1 = n.unitOrthogonal(); e2 = n.cross(e1); o = origin;
        cells.clear(); nx = ny = 0;
        if (tris.empty()) return;
        double maxx = -1e300, maxy = -1e300; minx = miny = 1e300;
        for (const auto& T : tris) for (const auto& P : T) {
            const double x = (P - o).dot(e1), y = (P - o).dot(e2);
            minx = std::min(minx, x); miny = std::min(miny, y); maxx = std::max(maxx, x); maxy = std::max(maxy, y);
        }
        const double w = std::max(maxx - minx, 1e-6), h = std::max(maxy - miny, 1e-6);
        cell = std::max(std::sqrt(w * h / double(tris.size())) * 2.0, 1e-4);
        nx = std::min(512, int(w / cell) + 1); ny = std::min(512, int(h / cell) + 1);
        cell = std::max(w / nx, h / ny) * (1.0 + 1e-9);
        cells.assign(size_t(nx) * size_t(ny), {});
        for (int i = 0; i < (int) tris.size(); ++i) {
            double tx0 = 1e300, ty0 = 1e300, tx1 = -1e300, ty1 = -1e300;
            for (const auto& P : tris[i]) {
                const double x = (P - o).dot(e1), y = (P - o).dot(e2);
                tx0 = std::min(tx0, x); ty0 = std::min(ty0, y); tx1 = std::max(tx1, x); ty1 = std::max(ty1, y);
            }
            const int ix0 = std::clamp(int((tx0 - minx) / cell), 0, nx - 1), ix1 = std::clamp(int((tx1 - minx) / cell), 0, nx - 1);
            const int iy0 = std::clamp(int((ty0 - miny) / cell), 0, ny - 1), iy1 = std::clamp(int((ty1 - miny) / cell), 0, ny - 1);
            for (int ix = ix0; ix <= ix1; ++ix)
                for (int iy = iy0; iy <= iy1; ++iy) cells[size_t(ix) * ny + iy].push_back(i);
        }
    }

    // Point on the patch along the line q + s*dir closest to q (smallest |s|); false when the line misses.
    bool drape(const Vec3d& q, const Vec3d& dir, Vec3d& out) const
    {
        if (cells.empty()) return false;
        const double x = (q - o).dot(e1), y = (q - o).dot(e2);
        const int ix = int(std::floor((x - minx) / cell)), iy = int(std::floor((y - miny) / cell));
        if (ix < 0 || iy < 0 || ix >= nx || iy >= ny) return false;
        double best = std::numeric_limits<double>::infinity();
        for (int i : cells[size_t(ix) * ny + iy]) {
            const double s = exact_line_tri(q, dir, tris[i][0], tris[i][1], tris[i][2]);
            if (!std::isnan(s) && std::abs(s) < std::abs(best)) best = s;
        }
        if (!std::isfinite(best)) return false;
        out = q + dir * best;
        return true;
    }
};

} // namespace detail

// Place the target face's shape on the moving face at the cursor and build the mate that lands it back.
//
// Frames: the target centre maps to the cursor point, the target normal to the reverse of the moving
// normal (faces meet anti-parallel), and the spin about the normal is the smallest one, plus -- when
// both reference axes are given -- the roll that lines the two faces' edges / cylinder axes up. For a
// curved moving face the footprint is draped onto the surface along its normal and the normal is
// re-estimated from the draped footprint (a few passes, so it settles on the region under the shape
// rather than the one facet under the cursor). The mate is the inverse of that placement, then slid
// along the target normal until the draped footprint just touches the target (no gap, no overlap).
inline ExactFootprint exact_footprint(const ExactFootprintInput& in)
{
    ExactFootprint out;
    if (!in.t_its || !in.t_facets || !in.a_its || in.t_facets->empty()) return out;
    if (in.t_normal.norm() < 1e-12 || in.a_normal.norm() < 1e-12) return out;
    const Vec3d nA = in.t_normal.normalized();
    const Vec3d cA = in.t_centre;
    const Vec3d h  = in.a_hit;

    // Target patch in the common frame, vertices shared.
    std::unordered_map<int, int> remap;
    std::vector<Vec3d> P;
    std::vector<std::array<int, 3>> T;
    for (int t : *in.t_facets) {
        if (t < 0 || t >= (int) in.t_its->indices.size()) continue;
        const auto& f = in.t_its->indices[t];
        std::array<int, 3> tri;
        bool good = true;
        for (int k = 0; k < 3; ++k) {
            const int vi = f[k];
            if (vi < 0 || vi >= (int) in.t_its->vertices.size()) { good = false; break; }
            auto it = remap.find(vi);
            if (it == remap.end()) {
                it = remap.emplace(vi, int(P.size())).first;
                P.push_back(in.t_w * in.t_its->vertices[vi].cast<double>());
            }
            tri[k] = it->second;
        }
        if (good) T.push_back(tri);
    }
    if (T.empty()) return out;

    // Moving patch in the common frame (only needed to drape onto a curved face).
    std::vector<std::array<Vec3d, 3>> A;
    if (in.a_curved && in.a_facets) {
        A.reserve(in.a_facets->size());
        for (int t : *in.a_facets) {
            if (t < 0 || t >= (int) in.a_its->indices.size()) continue;
            const auto& f = in.a_its->indices[t];
            A.push_back({ in.a_w * in.a_its->vertices[f[0]].cast<double>(),
                          in.a_w * in.a_its->vertices[f[1]].cast<double>(),
                          in.a_w * in.a_its->vertices[f[2]].cast<double>() });
        }
    }
    const bool drape = in.a_curved && !A.empty();
    const bool have_axes = in.t_axis.norm() > 1e-9 && in.a_axis.norm() > 1e-9;

    Vec3d nB = in.a_normal.normalized();
    Matrix3d R = Matrix3d::Identity();
    double roll = 0.0;
    std::vector<Vec3d> Q(P.size());
    std::vector<char>  hit(P.size(), 0);
    detail::PatchGrid grid;
    const int passes = drape ? 3 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        // R: moving-part frame -> target frame. nB -> -nA by the smallest turn, then the edge roll.
        R = Eigen::Quaterniond::FromTwoVectors(nB, -nA).toRotationMatrix();
        roll = have_axes ? exact_axis_roll(nA, R * in.a_axis, in.t_axis, in.axis_fold) : 0.0;
        if (std::abs(roll) > 1e-12) R = Eigen::AngleAxisd(roll, nA).toRotationMatrix() * R;
        const Matrix3d Rt = R.transpose();
        if (drape) grid.build(A, nB, h);
        for (size_t i = 0; i < P.size(); ++i) {
            const Vec3d q0 = h + Rt * (P[i] - cA);
            if (drape) {
                Vec3d q;
                hit[i] = grid.drape(q0, nB, q) ? 1 : 0;
                Q[i]   = hit[i] ? q : q0;
            } else {
                Q[i]   = q0 - nB * (q0 - h).dot(nB); // lay it on the moving plane
                hit[i] = 1;
            }
        }
        if (!drape || pass + 1 == passes) break;
        // Re-estimate the moving normal from the draped footprint. Target triangles face +nA; mapped by
        // R^T they face -nB, hence the minus.
        Vec3d nsum = Vec3d::Zero();
        for (const auto& t : T)
            if (hit[t[0]] && hit[t[1]] && hit[t[2]]) nsum -= (Q[t[1]] - Q[t[0]]).cross(Q[t[2]] - Q[t[0]]);
        if (nsum.norm() < 1e-12 || nsum.normalized().dot(nB) < 0.2) break; // keep the last good normal
        const Vec3d nNew = nsum.normalized();
        if ((nNew - nB).norm() < 1e-6) break;
        nB = nNew;
    }

    // Mate: target centre <- cursor point, rotated by R; then slide along nA to contact.
    Transform3d M = Transform3d::Identity();
    M.linear() = R;
    M.translation() = cA - R * h;
    double min_gap = std::numeric_limits<double>::infinity();
    int draped = 0, missed = 0;
    for (size_t i = 0; i < P.size(); ++i) {
        if (!hit[i]) { ++missed; continue; }
        ++draped;
        min_gap = std::min(min_gap, (M * Q[i] - P[i]).dot(nA)); // > 0: moving surface clear of the target
    }
    const double shift = std::isfinite(min_gap) ? -min_gap : 0.0;
    out.mate = Transform3d(Eigen::Translation3d(nA * shift)) * M;

    out.tris.reserve(T.size() * 3);
    for (const auto& t : T) { out.tris.push_back(Q[t[0]]); out.tris.push_back(Q[t[2]]); out.tris.push_back(Q[t[1]]); }
    out.anchor = h; out.normal = nB; out.roll = roll; out.contact_shift = shift;
    out.draped = draped; out.missed = missed;
    out.ok = true;
    return out;
}

}}} // namespace Slic3r::GUI::UltraFit
