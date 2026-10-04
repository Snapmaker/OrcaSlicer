#include "StabilizerMesh.hpp"

#include "../MeshBoolean.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace Slic3r { namespace stabilizers {

// Names kept out of the stabilizers namespace proper: Stabilizers.cpp shares it, and a unity build
// may put both files in one translation unit.
namespace stab_mesh_detail {

// Orient the shell outwards (positive volume). Every builder below winds its triangles
// consistently, so one sign check is enough.
static void orient_outward(indexed_triangle_set &its)
{
    double vol = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>(), b = its.vertices[f(1)].cast<double>(),
                    c = its.vertices[f(2)].cast<double>();
        vol += a.dot(b.cross(c));
    }
    if (vol < 0.)
        its_flip_triangles(its);
}

static int add_vertex(indexed_triangle_set &its, const Vec3d &p)
{
    its.vertices.emplace_back(p.cast<float>());
    return int(its.vertices.size()) - 1;
}

// A triangle, unless two of its corners are the same vertex (a collapsed side of a cut tube).
static void add_triangle(indexed_triangle_set &its, int a, int b, int c)
{
    if (a != b && b != c && c != a)
        its.indices.emplace_back(a, b, c);
}

// Fan a closed loop of vertices around a new vertex at their centroid: (centre, loop[k], loop[k+1]).
static void fan(indexed_triangle_set &its, const std::vector<int> &loop)
{
    if (loop.size() < 3)
        return;
    Vec3d c = Vec3d::Zero();
    for (int v : loop)
        c += its.vertices[v].cast<double>();
    const int centre = add_vertex(its, c / double(loop.size()));
    for (size_t k = 0; k < loop.size(); ++k)
        add_triangle(its, centre, loop[k], loop[(k + 1) % loop.size()]);
}

// Closed loops of the same number of vertices, lofted loop to loop (vertex k to vertex k) and capped
// at both ends. The loops must be ordered along the solid and each be convex.
static indexed_triangle_set loft_loops(const std::vector<std::vector<Vec3d>> &loops)
{
    indexed_triangle_set its;
    if (loops.size() < 2 || loops.front().size() < 3)
        return its;
    const size_t segments = loops.front().size();
    std::vector<std::vector<int>> idx(loops.size(), std::vector<int>(segments));
    for (size_t r = 0; r < loops.size(); ++r)
        for (size_t i = 0; i < segments; ++i)
            idx[r][i] = add_vertex(its, loops[r][i]);
    for (size_t r = 0; r + 1 < loops.size(); ++r)
        for (size_t i = 0; i < segments; ++i) {
            const size_t j = (i + 1) % segments;
            add_triangle(its, idx[r][i], idx[r][j], idx[r + 1][j]);
            add_triangle(its, idx[r][i], idx[r + 1][j], idx[r + 1][i]);
        }
    // Lateral edges run i -> j on the first loop and j -> i on the last, so the caps run the other way.
    std::vector<int> first(idx.front().rbegin(), idx.front().rend());
    fan(its, first);
    fan(its, idx.back());
    orient_outward(its);
    return its;
}

// Rings of `segments` points around an axis (the frame e1, e2 spans each ring), lofted ring to ring
// and capped at both ends. The rings must be ordered along the axis.
static indexed_triangle_set loft(const std::vector<std::pair<Vec3d, double>> &rings, const Vec3d &e1, const Vec3d &e2,
                                 int segments)
{
    if (rings.size() < 2 || segments < 3)
        return {};
    std::vector<std::vector<Vec3d>> loops(rings.size(), std::vector<Vec3d>(size_t(segments)));
    for (size_t r = 0; r < rings.size(); ++r)
        for (int i = 0; i < segments; ++i) {
            const double phi = 2. * M_PI * i / segments;
            loops[r][size_t(i)] = rings[r].first + rings[r].second * (std::cos(phi) * e1 + std::sin(phi) * e2);
        }
    return loft_loops(loops);
}

// A pillar: its section at every height is pillar_section's - a circle or a column outline grown by the
// taper and the foot. That growth is piecewise linear in z with kinks at the foot's top and (tapered)
// at the pillar's top, so loops at those heights and at both ends make it exact.
static indexed_triangle_set pillar_shell(const Pillar &p, const StabilizerSettings &st, double cap, int segments)
{
    const double top = p.top_z + cap;
    if (top <= EPSILON)
        return {}; // a strut that reaches the bed by itself
    const double foot = pillar_foot_at(st, 0.);
    std::vector<double> zs{ 0. };
    if (foot > EPSILON && foot < top - EPSILON)
        zs.push_back(foot);
    if (st.tapered() && p.top_z > zs.back() + EPSILON && p.top_z < top - EPSILON)
        zs.push_back(p.top_z);
    zs.push_back(top);
    std::vector<std::vector<Vec3d>> loops;
    for (double z : zs) {
        const double grow = pillar_growth_at(p, st, z);
        std::vector<Vec3d> loop;
        if (p.column) {
            for (const Vec2d &v : column_outline(column_frame(p, st), grow))
                loop.emplace_back(v.x(), v.y(), z);
        } else {
            // The X axis first, as the live generator's circles start, so the two polygonize alike.
            const double r = st.pillar_radius + grow;
            for (int i = 0; i < segments; ++i) {
                const double phi = 2. * M_PI * i / segments;
                loop.emplace_back(p.pos.x() + r * std::cos(phi), p.pos.y() + r * std::sin(phi), z);
            }
        }
        loops.emplace_back(std::move(loop));
    }
    return loft_loops(loops);
}

// A brace: the 45 degree rod cut at the two pillars' axes. Every generator of the rod is parallel to its
// axis, and the cuts are vertical planes, so it is a prism - the cut at the lower pillar's axis (an
// ellipse in that vertical plane, starting sqrt(2) radii below z_low) moved along the axis to the
// upper pillar's. Its section at any height is the live generator's cut ellipse.
static indexed_triangle_set brace_shell(const Brace &b, int segments)
{
    const Vec2d  dir = b.dir();
    const Vec2d  perp(-dir.y(), dir.x());
    const double d   = b.span();
    std::vector<std::vector<Vec3d>> loops(2);
    for (int i = 0; i < segments; ++i) {
        const double phi = 2. * M_PI * i / segments;
        // Generator phi meets the lower cut (along-axis offset 0) at s = -sqrt(2) r cos(phi).
        const Vec2d  xy  = b.from + perp * (b.radius * std::sin(phi));
        const double z   = b.z_low - M_SQRT2 * b.radius * std::cos(phi);
        loops[0].emplace_back(xy.x(), xy.y(), z);
        loops[1].emplace_back(xy.x() + dir.x() * d, xy.y() + dir.y() * d, z + d);
    }
    return loft_loops(loops);
}

// A frame perpendicular to the unit axis u.
static std::pair<Vec3d, Vec3d> perpendicular_frame(const Vec3d &u)
{
    const Vec3d helper = std::abs(u.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX();
    const Vec3d e1     = u.cross(helper).normalized();
    return { e1, u.cross(e1).normalized() };
}

// One strut as a cut oblique tube; see stabilizer_shells() for the geometry.
class StrutTube
{
public:
    StrutTube(const Strut &s, const StabilizerSettings &st, double cap)
        // Built set back along its axis by the tip gap (gapped()), as slice_struts builds it: it tapers
        // to the tip diameter at the trimmed end. The wall cut still measures from the wall contact.
        : m_s(gapped(s, st.tip_gap)), m_contact(s.tip), m_tip_r(0.5 * st.rings.tip_diameter), m_pillar_r(st.pillar_radius),
          m_gap(st.tip_gap), m_z_top(m_s.tip_z + cap), m_perp(-s.dir.y(), s.dir.x())
    {
        // The tip is cut along the wall: its outward normal there, or `dir` when unknown or when the
        // wall is so oblique to the strut that the cut would be a sliver.
        m_wall = s.normal.squaredNorm() > 0.5 && s.normal.dot(s.dir) > 0.5 ? Vec2d(s.normal.normalized()) : s.dir;
    }

    // The tube's radius (perpendicular to its axis) at axis parameter s: the tip radius above the tip,
    // the pillar radius below the junction, linear in between - what slice_struts uses at each height.
    double rho(double s) const
    {
        if (s <= 0.)
            return m_tip_r;
        if (s >= m_s.run)
            return m_pillar_r;
        return m_tip_r + (m_pillar_r - m_tip_r) * s / m_s.run;
    }

    // The surface point on generator phi at axis parameter s. s is the axis's horizontal run from the
    // tip, which is also how far below the tip it is (45 degrees). The tube is built from its
    // horizontal sections, exactly as slice_struts builds it: at every height an ellipse around the
    // axis, sqrt(2) rho long along the strut and rho across. A generator joins the sections' points of
    // the same angle, and is straight wherever rho is linear.
    Vec3d point(double s, double phi) const
    {
        const double r  = rho(s);
        const Vec2d  xy = m_s.tip + m_s.dir * (s + M_SQRT2 * r * std::cos(phi)) + m_perp * (r * std::sin(phi));
        return { xy.x(), xy.y(), m_s.tip_z - s };
    }

    // The four cuts, each >= 0 inside. 0: the wall plane through the tip, moved out by the gap;
    // 1: the top, just above the tip layer; 2: the vertical plane through the pillar's axis; 3: the bed.
    // 0 and 1 bound s from below, 2 and 3 from above.
    double value(int cut, double s, double phi) const
    {
        switch (cut) {
        case 0: {
            const Vec3d p = point(s, phi);
            return (Vec2d(p.x(), p.y()) - m_contact).dot(m_wall) - m_gap;
        }
        case 1: return m_z_top - (m_s.tip_z - s);
        case 2: return m_s.run - (s + M_SQRT2 * rho(s) * std::cos(phi));
        default: return m_s.tip_z - s;
        }
    }

    // Where generator phi crosses cut `cut`. The two horizontal cuts are closed form. The two vertical
    // ones are monotonic along a generator unless the strut tapers very steeply (a pillar several
    // times the tip), so they are found by a scan from the inside of the strut outwards and then
    // bisected: the last crossing for a lower bound, the first for an upper one.
    double root(int cut, double phi) const
    {
        if (cut == 1)
            return m_s.tip_z - m_z_top;
        if (cut == 3)
            return m_s.tip_z;
        const double reach = M_SQRT2 * std::max(m_tip_r, m_pillar_r) + m_gap + 1.;
        const double lo0   = cut == 0 ? -reach : m_s.run - reach;
        const double hi0   = cut == 0 ? m_s.run + reach : m_s.run + reach;
        const int    steps = 400;
        const double step  = (hi0 - lo0) / steps;
        double lo = lo0, hi = lo0;
        bool   found = false;
        if (cut == 0) {
            // Lower bound: walk down from the far end while inside; the root is where it stops being.
            for (int i = steps; i > 0 && ! found; --i) {
                const double a = lo0 + step * (i - 1), b = lo0 + step * i;
                if (value(0, a, phi) < 0. && value(0, b, phi) >= 0.) {
                    lo = a; hi = b; found = true;
                }
            }
            if (! found)
                return lo0;
        } else {
            // Upper bound: walk up from the near end while inside.
            for (int i = 0; i < steps && ! found; ++i) {
                const double a = lo0 + step * i, b = lo0 + step * (i + 1);
                if (value(2, a, phi) >= 0. && value(2, b, phi) < 0.) {
                    lo = a; hi = b; found = true;
                }
            }
            if (! found)
                return hi0;
        }
        for (int it = 0; it < 50; ++it) {
            const double mid    = 0.5 * (lo + hi);
            const bool   inside = value(cut, mid, phi) >= 0.;
            if (inside == (cut == 0))
                hi = mid;
            else
                lo = mid;
        }
        return 0.5 * (lo + hi);
    }

    // The interval of generator phi left by the cuts, and which cut ends it on either side.
    void interval(double phi, double &s_lo, int &cut_lo, double &s_hi, int &cut_hi) const
    {
        const double r0 = root(0, phi), r1 = root(1, phi), r2 = root(2, phi), r3 = root(3, phi);
        if (r0 >= r1) { s_lo = r0; cut_lo = 0; } else { s_lo = r1; cut_lo = 1; }
        if (r2 <= r3) { s_hi = r2; cut_hi = 2; } else { s_hi = r3; cut_hi = 3; }
    }

    // Between generators phi_a (ended by cut a) and phi_b (ended by cut b), the generator where both
    // cuts meet: the crease between the two flat end faces.
    double crease(double phi_a, int a, double phi_b, int b) const
    {
        const double ha = root(a, phi_a) - root(b, phi_a);
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (phi_a + phi_b);
            const double h   = root(a, mid) - root(b, mid);
            if ((h > 0.) == (ha > 0.))
                phi_a = mid;
            else
                phi_b = mid;
        }
        return 0.5 * (phi_a + phi_b);
    }

    indexed_triangle_set build(int segments) const
    {
        indexed_triangle_set its;
        const int n = std::max(3, segments);
        std::vector<std::array<int, 4>> v(static_cast<size_t>(n));
        std::vector<int>    cut_lo(static_cast<size_t>(n)), cut_hi(static_cast<size_t>(n));
        std::vector<double> phis(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double phi = 2. * M_PI * i / n;
            phis[size_t(i)]  = phi;
            double s_lo, s_hi;
            interval(phi, s_lo, cut_lo[size_t(i)], s_hi, cut_hi[size_t(i)]);
            if (s_hi - s_lo < 1e-6)
                return {}; // the cuts leave nothing of this side: no strut at all
            // Stations: both ends plus the radius profile's two kinks (tip and junction) where they fall
            // inside, so every side between stations is straight. Coinciding stations share a vertex.
            const std::array<double, 4> st = { s_lo, std::clamp(0., s_lo, s_hi), std::clamp(m_s.run, s_lo, s_hi), s_hi };
            for (size_t k = 0; k < 4; ++k)
                v[size_t(i)][k] = k > 0 && std::abs(st[k] - st[k - 1]) < 1e-5 ? v[size_t(i)][k - 1] : add_vertex(its, point(st[k], phi));
        }
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            for (size_t k = 0; k < 3; ++k) {
                add_triangle(its, v[i][k], v[j][k], v[j][k + 1]);
                add_triangle(its, v[i][k], v[j][k + 1], v[i][k + 1]);
            }
        }
        // The two ends. The lateral sides run i -> j along the tip end and j -> i along the pillar end,
        // so the tip end's face runs j -> i (descending) and the pillar end's i -> j (ascending).
        end_face(its, v, phis, cut_lo, 0, false);
        end_face(its, v, phis, cut_hi, 3, true);
        orient_outward(its);
        return its;
    }

private:
    // One end of the tube: flat where a single cut ends every generator; where two cuts take turns, a
    // crease vertex on the tube between the generators where the cut changes, a triangle closing the
    // side there, and one flat face per cut.
    void end_face(indexed_triangle_set &its, const std::vector<std::array<int, 4>> &v, const std::vector<double> &phis,
                  const std::vector<int> &cuts, size_t station, bool ascending) const
    {
        const int n = int(v.size());
        std::vector<int>  loop;      // vertex indices in face order
        std::vector<bool> is_crease;
        for (int step = 0; step < n; ++step) {
            const int i = ascending ? step : (n - step) % n;
            const int j = ascending ? (i + 1) % n : (i - 1 + n) % n;
            loop.push_back(v[i][station]);
            is_crease.push_back(false);
            if (cuts[i] != cuts[j]) {
                // The generators i and j in ascending order, so the bisection runs over a positive span.
                const int    lo  = ascending ? i : j, hi = ascending ? j : i;
                const double plo = phis[lo], phi_hi = hi == 0 ? 2. * M_PI : phis[hi];
                const double phi = crease(plo, cuts[lo], phi_hi, cuts[hi]);
                const int    c   = add_vertex(its, point(root(cuts[lo], phi), phi));
                // Close the side: it carries the edge between the two generators' end vertices, which
                // the side triangles run j -> i at either end once the face is walked i -> j.
                add_triangle(its, v[i][station], v[j][station], c);
                loop.push_back(c);
                is_crease.push_back(true);
            }
        }
        const size_t creases = size_t(std::count(is_crease.begin(), is_crease.end(), true));
        if (creases != 2) {
            fan(its, loop);
            return;
        }
        // Two flat faces, split along the crease line.
        size_t first = 0;
        while (! is_crease[first])
            ++first;
        std::vector<int> a, b;
        size_t k = first;
        a.push_back(loop[k]);
        for (k = (k + 1) % loop.size(); ! is_crease[k]; k = (k + 1) % loop.size())
            a.push_back(loop[k]);
        a.push_back(loop[k]);
        const size_t second = k;
        b.push_back(loop[k]);
        for (k = (k + 1) % loop.size(); k != first; k = (k + 1) % loop.size())
            b.push_back(loop[k]);
        b.push_back(loop[first]);
        (void)second;
        fan(its, a);
        fan(its, b);
    }

    Strut        m_s;
    Vec2d        m_contact;
    double       m_tip_r, m_pillar_r, m_gap, m_z_top;
    Vec2d        m_perp;
    Vec2d        m_wall;
};

} // namespace stab_mesh_detail

indexed_triangle_set frustum_between(const Vec3d &a, double ra, const Vec3d &b, double rb, int segments)
{
    const Vec3d axis = b - a;
    if (axis.norm() < EPSILON)
        return {};
    const auto [e1, e2] = stab_mesh_detail::perpendicular_frame(axis.normalized());
    return stab_mesh_detail::loft({ { a, ra }, { b, rb } }, e1, e2, segments);
}

std::vector<indexed_triangle_set> stabilizer_shells(const Plan &plan, const StabilizerSettings &st, const MeshOptions &opts,
                                                    MeshReport *report)
{
    std::vector<indexed_triangle_set> shells;
    MeshReport rep;
    const int n = std::max(3, opts.segments);

    // Pillars: struts of different rings come down onto the same pillar; one shell up to the highest
    // junction, never two coincident ones.
    for (const Pillar &p : plan.pillars) {
        indexed_triangle_set shell = stab_mesh_detail::pillar_shell(p, st, opts.cap, n);
        if (shell.indices.empty())
            continue;
        shells.emplace_back(std::move(shell));
        ++rep.pillars;
        if (p.column)
            ++rep.columns;
    }

    // Braces between the pillars.
    for (const Brace &b : plan.braces) {
        indexed_triangle_set shell = stab_mesh_detail::brace_shell(b, n);
        if (shell.indices.empty())
            continue;
        shells.emplace_back(std::move(shell));
        ++rep.braces;
    }

    // Struts: the oblique tube from the tip down past the pillar's axis, its radius tapering from the
    // tip radius to the pillar radius over the run. Its ends are cut where slice_struts cuts it - at the
    // tip by the vertical plane across the strut through the tip (moved out by the tip gap, so the tip
    // touches, or stops short of, the wall), and just above the tip layer; at the pillar by the
    // vertical plane through the pillar's axis, and by the bed.
    for (const Strut &s : plan.struts) {
        indexed_triangle_set tube = stab_mesh_detail::StrutTube(s, st, opts.cap).build(n);
        if (tube.indices.empty()) {
            ++rep.skipped;
            continue;
        }
        shells.emplace_back(std::move(tube));
        ++rep.struts;
    }
    rep.shells = shells.size();
    if (report != nullptr)
        *report = rep;
    return shells;
}

std::vector<indexed_triangle_set> stabilizer_shells(const std::vector<Strut> &struts, const StabilizerSettings &st,
                                                    const MeshOptions &opts, MeshReport *report)
{
    return stabilizer_shells(plan_from_struts(st, struts), st, opts, report);
}

indexed_triangle_set stabilizer_mesh(const std::vector<Strut> &struts, const StabilizerSettings &st,
                                     const MeshOptions &opts, MeshReport *report)
{
    return stabilizer_mesh(plan_from_struts(st, struts), st, opts, report);
}

indexed_triangle_set stabilizer_mesh(const Plan &plan, const StabilizerSettings &st, const MeshOptions &opts, MeshReport *report)
{
    MeshReport rep;
    std::vector<indexed_triangle_set> shells = stabilizer_shells(plan, st, opts, &rep);
    indexed_triangle_set out;
    if (shells.empty()) {
        if (report != nullptr)
            *report = rep;
        return out;
    }
    if (opts.union_shells && MeshBoolean::mfd::union_all(shells, out) && ! out.indices.empty()) {
        rep.unioned = true;
    } else {
        // Overlapping closed shells: each is closed and outward, and slicers union them per layer.
        out.clear();
        for (const indexed_triangle_set &s : shells)
            its_merge(out, s);
        if (opts.union_shells)
            BOOST_LOG_TRIVIAL(warning) << "Stabilizer mesh: the union failed, keeping " << shells.size() << " overlapping shells";
    }
    rep.triangles = out.indices.size();
    rep.closed    = ! out.indices.empty() && its_num_open_edges(out) == 0;
    rep.volume    = its_volume(out);
    if (report != nullptr)
        *report = rep;
    return out;
}

}} // namespace Slic3r::stabilizers
