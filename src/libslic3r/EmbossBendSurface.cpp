#include "EmbossBendSurface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "AABBMesh.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r::Emboss {

namespace {

Vec3d bs_tangent_part(const Vec3d &v, const Vec3d &n) { return v - v.dot(n) * n; }

bool bs_normalize(Vec3d &v)
{
    const double len = v.norm();
    if (!(len > 1e-12))
        return false;
    v /= len;
    return true;
}

} // namespace

BendSurface::BendSurface(indexed_triangle_set its) : m_its(std::make_unique<indexed_triangle_set>(std::move(its)))
{
    if (m_its->indices.empty())
        return;
    m_tree = std::make_unique<AABBMesh>(*m_its);

    // Corner normals: area weighted face normals of the faces around the vertex that are within
    // 45 degrees of the face itself, so creases stay sharp
    const size_t faces = m_its->indices.size();
    std::vector<Vec3d> face_normals(faces);
    std::vector<double> face_areas(faces);
    std::vector<std::vector<size_t>> vertex_faces(m_its->vertices.size());
    for (size_t f = 0; f < faces; ++f) {
        const stl_triangle_vertex_indices &t = m_its->indices[f];
        const Vec3d a = m_its->vertices[t[0]].cast<double>();
        const Vec3d b = m_its->vertices[t[1]].cast<double>();
        const Vec3d c = m_its->vertices[t[2]].cast<double>();
        Vec3d n = (b - a).cross(c - a);
        face_areas[f] = n.norm() / 2.;
        face_normals[f] = bs_normalize(n) ? n : Vec3d::UnitZ();
        for (int k = 0; k < 3; ++k)
            vertex_faces[t[k]].push_back(f);
    }
    const double cos_crease = std::cos(M_PI / 4.);
    m_corner_normals.resize(faces);
    for (size_t f = 0; f < faces; ++f) {
        const stl_triangle_vertex_indices &t = m_its->indices[f];
        for (int k = 0; k < 3; ++k) {
            Vec3d sum = Vec3d::Zero();
            for (size_t g : vertex_faces[t[k]])
                if (face_normals[g].dot(face_normals[f]) >= cos_crease)
                    sum += face_areas[g] * face_normals[g];
            m_corner_normals[f][k] = bs_normalize(sum) ? sum : face_normals[f];
        }
    }
}

BendSurface::~BendSurface() = default;

bool BendSurface::empty() const { return m_tree == nullptr; }

std::optional<BendSurface::Point> BendSurface::closest(const Vec3d &p) const
{
    if (empty())
        return {};
    int   face = -1;
    Vec3d c;
    m_tree->squared_distance(p, face, c);
    if (face < 0 || face >= static_cast<int>(m_its->indices.size()))
        return {};
    const stl_triangle_vertex_indices &t = m_its->indices[face];
    const Vec3d a = m_its->vertices[t[0]].cast<double>();
    const Vec3d b = m_its->vertices[t[1]].cast<double>();
    const Vec3d d = m_its->vertices[t[2]].cast<double>();
    // barycentric coordinates of c
    const Vec3d  v0 = b - a, v1 = d - a, v2 = c - a;
    const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
    const double den = d00 * d11 - d01 * d01;
    Vec3d n;
    if (std::abs(den) > 1e-18) {
        const double w1 = (d11 * d20 - d01 * d21) / den;
        const double w2 = (d00 * d21 - d01 * d20) / den;
        const double w0 = 1. - w1 - w2;
        n = w0 * m_corner_normals[face][0] + w1 * m_corner_normals[face][1] + w2 * m_corner_normals[face][2];
    } else {
        n = m_corner_normals[face][0];
    }
    if (!bs_normalize(n))
        n = m_corner_normals[face][0];
    if (m_flip)
        n = -n;
    return Point{c, n};
}

BendSurface::Walk BendSurface::walk(const Point &start, const Vec3d &direction, double length, double step) const
{
    Walk result;
    result.end = start;
    Vec3d d    = bs_tangent_part(direction, start.normal);
    if (!bs_normalize(d))
        return result;
    result.direction = d;
    if (!(length > 0.)) {
        result.complete = true;
        return result;
    }
    if (step <= 0.)
        step = std::clamp(length / 48., 0.05, 0.5);

    Point  p         = start;
    double remaining = length;
    // safety: a walk never needs more than a few times its nominal step count
    const int max_steps = static_cast<int>(std::ceil(length / step)) * 4 + 8;
    for (int i = 0; i < max_steps && remaining > 1e-9; ++i) {
        const double h = std::min(step, remaining);
        std::optional<Point> next = closest(p.position + h * d);
        if (!next.has_value())
            break;
        Vec3d chord = next->position - p.position;
        const double moved = chord.norm();
        if (moved < 1e-3 * h)
            break; // stuck (e.g. at a boundary of an open mesh)
        Vec3d nd = bs_tangent_part(chord, next->normal);
        if (!bs_normalize(nd)) {
            nd = bs_tangent_part(d, next->normal);
            if (!bs_normalize(nd))
                break;
        }
        remaining -= moved;
        p = *next;
        d = nd;
    }
    result.end       = p;
    result.direction = d;
    result.length    = length - std::max(remaining, 0.);
    result.complete  = remaining <= 1e-6 + 1e-3 * length;
    return result;
}

namespace {

struct BsCircle
{
    const BendSurface *surface;
    BendSurface::Point center;
    Vec3d              u, v; // tangent basis at the centre, u towards the text origin
    double             rho;
    double             sign; // reading direction: phi = sign * arc length / r
    double             step; // walk step [mm]

    std::optional<BendSurface::Point> at(double phi) const
    {
        BendSurface::Walk w = surface->walk(center, std::cos(phi) * u + std::sin(phi) * v, rho, step);
        if (!w.complete)
            return {};
        return w.end;
    }
};

// Centre and tangent basis for a geodesic radius rho
std::optional<BsCircle> bs_make_circle(const BendSurface &surface, const BendSurface::Point &origin, const Vec3d &to_center,
                                        double rho, bool inside, double step)
{
    BendSurface::Walk w = surface.walk(origin, to_center, rho, step);
    if (!w.complete)
        return {};
    Vec3d u = -w.direction;
    u = bs_tangent_part(u, w.end.normal);
    if (!bs_normalize(u))
        return {};
    Vec3d v = w.end.normal.cross(u);
    // arch reads with decreasing phi (clockwise seen from outside), smile with increasing phi
    return BsCircle{&surface, w.end, u, v, rho, inside ? 1. : -1., step};
}

// Arc length table along the circle, phi from 0 in reading direction until `need_pos` mm of arc
// are covered forward and `need_neg` mm backward (or half a turn)
struct BsArcTable
{
    std::vector<double> phi;    // ascending in reading order
    std::vector<double> length; // signed arc length from phi = 0
};

BsArcTable bs_make_arc_table(const BsCircle &circle, double need_neg, double need_pos, double dphi)
{
    BsArcTable table;
    std::optional<BendSurface::Point> p0 = circle.at(0.);
    if (!p0.has_value())
        return table;
    auto march = [&](double dir, double need, std::vector<double> &phis, std::vector<double> &lens) {
        BendSurface::Point prev   = *p0;
        double             length = 0.;
        for (int k = 1; length < need && k * dphi <= M_PI + 1e-9; ++k) {
            const double phi = circle.sign * dir * k * dphi;
            std::optional<BendSurface::Point> p = circle.at(phi);
            if (!p.has_value())
                break;
            length += (p->position - prev.position).norm();
            prev = *p;
            phis.push_back(phi);
            lens.push_back(dir * length);
        }
    };
    std::vector<double> phi_neg, len_neg, phi_pos, len_pos;
    march(-1., need_neg, phi_neg, len_neg);
    march(1., need_pos, phi_pos, len_pos);
    for (size_t i = phi_neg.size(); i-- > 0;) {
        table.phi.push_back(phi_neg[i]);
        table.length.push_back(len_neg[i]);
    }
    table.phi.push_back(0.);
    table.length.push_back(0.);
    table.phi.insert(table.phi.end(), phi_pos.begin(), phi_pos.end());
    table.length.insert(table.length.end(), len_pos.begin(), len_pos.end());
    return table;
}

// phi of a signed arc length, empty outside of the table
std::optional<double> bs_phi_of(const BsArcTable &table, double s)
{
    if (table.length.size() < 2 || s < table.length.front() - 1e-9 || s > table.length.back() + 1e-9)
        return {};
    auto it = std::lower_bound(table.length.begin(), table.length.end(), s);
    size_t i = std::clamp<size_t>(static_cast<size_t>(it - table.length.begin()), 1, table.length.size() - 1);
    const double s0 = table.length[i - 1], s1 = table.length[i];
    const double t  = (s1 > s0) ? std::clamp((s - s0) / (s1 - s0), 0., 1.) : 0.;
    return table.phi[i - 1] + t * (table.phi[i] - table.phi[i - 1]);
}

// Arc length per radian of the circle over the text, |phi| measured around the centre
std::optional<double> bs_effective_radius(const BsCircle &circle, double x_min, double x_max, double guess_r)
{
    // expected angles of the text ends for the guessed radius, sampled coarsely
    const double phi_a = x_min / guess_r, phi_b = x_max / guess_r;
    const int    count = std::clamp(static_cast<int>(std::ceil(std::abs(phi_b - phi_a) / (M_PI / 18.))), 4, 24);
    double length = 0.;
    std::optional<BendSurface::Point> prev;
    for (int i = 0; i <= count; ++i) {
        const double phi = circle.sign * (phi_a + (phi_b - phi_a) * i / count);
        std::optional<BendSurface::Point> p = circle.at(phi);
        if (!p.has_value())
            return {};
        if (prev.has_value())
            length += (p->position - prev->position).norm();
        prev = p;
    }
    const double dphi = std::abs(phi_b - phi_a);
    if (!(dphi > 0.))
        return {};
    return length / dphi;
}

} // namespace

SurfaceArc place_on_surface_arc(const BendSurface &surface, const std::vector<double> &pivots_mm, double x_min, double x_max,
                                const SurfaceArcParams &params)
{
    SurfaceArc result;
    result.frames.assign(pivots_mm.size(), std::nullopt);
    result.curvature_radius.assign(pivots_mm.size(), 0.);
    if (surface.empty() || !(params.radius > 0.) || !(x_max > x_min))
        return result;

    std::optional<BendSurface::Point> origin = surface.closest(Vec3d::Zero());
    if (!origin.has_value())
        return result;
    Vec3d y_axis = bs_tangent_part(Vec3d::UnitY(), origin->normal);
    if (!bs_normalize(y_axis))
        return result;
    const Vec3d to_center = params.inside ? y_axis : Vec3d(-y_axis);
    const double width    = params.width > 0. ? params.width : (x_max - x_min);

    // fine walks for the placement, coarse ones while solving the radius
    auto fine_step   = [](double rho) { return std::clamp(rho / 48., 0.05, 0.5); };
    auto coarse_step = [](double rho) { return std::clamp(rho / 24., 0.1, 1.); };
    auto circle_for  = [&](double rho, double step) {
        return bs_make_circle(surface, *origin, to_center, rho, params.inside, step);
    };

    // ---- geodesic radius ----
    double rho     = std::max(params.radius, params.min_radius);
    bool   limited = rho > params.radius + 1e-9;
    // Geodesic radius whose circle gives the widest line the target span: r_eff(rho) = width / span
    auto solve_span = [&](double target_span) -> std::optional<double> {
        const double target = width / target_span;
        auto f = [&](double r) -> std::optional<double> {
            std::optional<BsCircle> c = circle_for(r, coarse_step(r));
            if (!c.has_value())
                return {};
            std::optional<double> re = bs_effective_radius(*c, x_min, x_max, target);
            if (!re.has_value())
                return {};
            return *re - target;
        };
        const double tol = 1e-3 * target;
        double lo = std::max(params.min_radius, 1e-3);
        double hi = std::max(target, lo * 1.01);
        std::optional<double> f_hi = f(hi);
        if (f_hi.has_value() && std::abs(*f_hi) < tol)
            return hi; // flat: the flat radius
        std::optional<double> f_lo = f(lo);
        if (!f_lo.has_value() || !f_hi.has_value())
            return {};
        if (*f_lo > 0.) {
            limited = true; // even the tightest allowed circle is too long
            return lo;
        }
        // grow hi until the circle is long enough, or until it stops growing (past the "equator")
        for (int i = 0; i < 24 && *f_hi < 0.; ++i) {
            std::optional<double> f_next = f(hi * 1.25);
            if (!f_next.has_value() || *f_next <= *f_hi) {
                limited = true; // no circle on this surface is that long: the longest one
                return hi;
            }
            lo   = hi;
            f_lo = f_hi;
            hi  *= 1.25;
            f_hi = f_next;
        }
        if (*f_hi < 0.) {
            limited = true;
            return hi;
        }
        // Illinois regula falsi
        double a = lo, fa = *f_lo, b = hi, fb = *f_hi;
        int side = 0;
        for (int i = 0; i < 16; ++i) {
            const double c = (a * fb - b * fa) / (fb - fa);
            std::optional<double> fc = f(c);
            if (!fc.has_value())
                break;
            if (std::abs(*fc) < tol)
                return c;
            if (*fc < 0.) {
                a = c; fa = *fc;
                if (side == -1) fb /= 2.;
                side = -1;
            } else {
                b = c; fb = *fc;
                if (side == 1) fa /= 2.;
                side = 1;
            }
            if (b - a < 1e-4 * b)
                break;
        }
        return 0.5 * (a + b);
    };

    if (params.span.has_value()) {
        if (std::optional<double> r = solve_span(std::min(*params.span, params.max_span)); r.has_value())
            rho = std::max(*r, params.min_radius);
    }

    std::optional<BsCircle> circle = circle_for(rho, fine_step(rho));
    if (!circle.has_value())
        return result;
    double step_phi = std::clamp(0.5 / rho, M_PI / 720., M_PI / 60.);
    BsArcTable table  = bs_make_arc_table(*circle, std::max(0., -x_min), std::max(0., x_max), step_phi);

    // radius mode: the text must fit into the largest span
    auto span_of = [&](const BsArcTable &t) -> std::optional<double> {
        std::optional<double> a = bs_phi_of(t, x_min), b = bs_phi_of(t, x_max);
        if (!a.has_value() || !b.has_value())
            return {};
        return std::abs(*b - *a);
    };
    std::optional<double> span = span_of(table);
    if (!params.span.has_value() && (!span.has_value() || *span > params.max_span)) {
        if (std::optional<double> r = solve_span(params.max_span); r.has_value()) {
            rho     = std::max(*r, rho);
            limited = true;
            circle  = circle_for(rho, fine_step(rho));
            if (!circle.has_value())
                return result;
            step_phi = std::clamp(0.5 / rho, M_PI / 720., M_PI / 60.);
            table    = bs_make_arc_table(*circle, std::max(0., -x_min), std::max(0., x_max), step_phi);
            span     = span_of(table);
        }
    }

    // ---- glyph frames ----
    const double delta = std::clamp(1. / rho, 1e-4, 0.05); // [rad] for the tangent and curvature
    for (size_t i = 0; i < pivots_mm.size(); ++i) {
        const double x = pivots_mm[i];
        if (!std::isfinite(x))
            continue;
        std::optional<double> phi = bs_phi_of(table, x);
        if (!phi.has_value())
            continue;
        std::optional<BendSurface::Point> p  = circle->at(*phi);
        std::optional<BendSurface::Point> pa = circle->at(*phi - circle->sign * delta);
        std::optional<BendSurface::Point> pb = circle->at(*phi + circle->sign * delta);
        if (!p.has_value() || !pa.has_value() || !pb.has_value())
            continue;
        const Vec3d z = p->normal;
        Vec3d x_dir = bs_tangent_part(pb->position - pa->position, z);
        if (!bs_normalize(x_dir))
            continue;
        const Vec3d y_dir = z.cross(x_dir);
        Transform3d frame = Transform3d::Identity();
        frame.linear().col(0) = x_dir;
        frame.linear().col(1) = y_dir;
        frame.linear().col(2) = z;
        frame.translation()   = p->position;
        result.frames[i]      = frame;

        // curvature of the curve in the tangent plane: circumcircle of the three points
        const Vec2d a2((pa->position - p->position).dot(x_dir), (pa->position - p->position).dot(y_dir));
        const Vec2d b2((pb->position - p->position).dot(x_dir), (pb->position - p->position).dot(y_dir));
        const double cross = a2.x() * b2.y() - a2.y() * b2.x();
        if (std::abs(cross) > 1e-12) {
            const double ab = (a2 - b2).norm();
            result.curvature_radius[i] = a2.norm() * b2.norm() * ab / (2. * std::abs(cross));
        }
    }

    // ---- preview ----
    SurfaceArcPreview &preview = result.preview;
    preview.valid    = true;
    preview.center   = circle->center.position;
    preview.normal   = circle->center.normal;
    preview.radius   = rho;
    preview.limited  = limited;
    preview.span_deg = span.has_value() ? *span * 180. / M_PI : 0.;
    const int count  = 90;
    preview.circle.reserve(count);
    preview.circle_normal.reserve(count);
    preview.circle_ok.reserve(count);
    for (int k = 0; k < count; ++k) {
        std::optional<BendSurface::Point> p = circle->at(2. * M_PI * k / count);
        preview.circle.push_back(p.has_value() ? p->position : Vec3d::Zero());
        preview.circle_normal.push_back(p.has_value() ? p->normal : Vec3d::UnitZ());
        preview.circle_ok.push_back(p.has_value());
    }
    return result;
}

std::optional<SurfaceGlyphLayout> surface_glyph_layout(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances,
                                                       const EmbossBend &bend, double shape_scale)
{
    if (!bend.is_active() || !(shape_scale > 0.))
        return {};
    if (advances != nullptr && advances->size() != shapes.size())
        advances = nullptr;
    const BendInput  input = measure_bend_input(shapes, advances);
    const BendResult flat  = resolve_bend(bend, input, shape_scale, BEND_SURFACE_TOLERANCE_MM);
    if (!flat.is_active())
        return {};

    SurfaceGlyphLayout layout;
    layout.pivots = glyph_pivots(shapes, advances);
    layout.pivots_mm.assign(shapes.size(), std::numeric_limits<double>::quiet_NaN());
    double x_min = std::numeric_limits<double>::max(), x_max = -std::numeric_limits<double>::max();
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (shapes[i].expoly.empty()) {
            layout.pivots[i] = std::numeric_limits<double>::quiet_NaN();
            continue;
        }
        const BoundingBox bb = get_extents(shapes[i].expoly);
        double lo = static_cast<double>(bb.min.x()), hi = static_cast<double>(bb.max.x());
        if (advances != nullptr && (*advances)[i].valid) {
            lo = std::min(lo, (*advances)[i].x_min);
            hi = std::max(hi, (*advances)[i].x_max);
        }
        x_min = std::min(x_min, lo * shape_scale);
        x_max = std::max(x_max, hi * shape_scale);
        layout.pivots_mm[i] = layout.pivots[i] * shape_scale;
    }
    if (!(x_max > x_min))
        return {};
    layout.x_min = x_min;
    layout.x_max = x_max;

    SurfaceArcParams &params = layout.params;
    params.inside     = bend.inside;
    params.width      = input.width * shape_scale;
    params.min_radius = BEND_MIN_RADIUS_RATIO * std::max(0., (bend.inside ? input.y_max : -input.y_min) * shape_scale);
    params.radius     = flat.radius_mm;
    if (bend.mode == EmbossBend::Mode::angle)
        params.span = std::min(static_cast<double>(bend.angle), BEND_MAX_ANGLE_DEG) * M_PI / 180.;
    else
        params.radius = std::max(static_cast<double>(bend.radius), params.min_radius);
    return layout;
}

ExPolygons surface_glyph_shape(const ExPolygons &glyph, double pivot, double curvature_radius_mm, const EmbossBend &bend,
                               double shape_scale)
{
    ExPolygons result = union_ex(glyph);
    const Point offset(-static_cast<coord_t>(std::llround(pivot)), 0);
    for (ExPolygon &e : result)
        e.translate(offset);
    if (!bend.rigid && curvature_radius_mm > 0. && shape_scale > 0.) {
        BendSpec local;
        local.radius    = curvature_radius_mm / shape_scale;
        local.inside    = bend.inside;
        local.tolerance = BEND_SURFACE_TOLERANCE_MM / shape_scale;
        result          = bend_expolygons(result, local);
    }
    return result;
}

} // namespace Slic3r::Emboss
