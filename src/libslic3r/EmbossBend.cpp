#include "EmbossBend.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/QR>

#include "BoundingBox.hpp"

namespace Slic3r::Emboss {

namespace {

bool bend_use_advances(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances)
{
    return advances != nullptr && advances->size() == shapes.size();
}

Point bend_round_point(const Vec2d &p)
{
    return Point(static_cast<coord_t>(std::llround(p.x())), static_cast<coord_t>(std::llround(p.y())));
}

// Largest angular step of one densified segment, so its sagitta at radius rho stays below tolerance
double bend_max_angle_step(double rho, double tolerance)
{
    const double max_step = M_PI / 8.;
    if (rho <= tolerance || tolerance <= 0.)
        return max_step;
    double step = 2. * std::acos(1. - tolerance / rho);
    if (!std::isfinite(step) || step <= 0.)
        return max_step;
    return std::min(step, max_step);
}

} // namespace

BendInput measure_bend_input(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances)
{
    BendInput result;
    const bool has_advances = bend_use_advances(shapes, advances);

    const double inf = std::numeric_limits<double>::infinity();
    double line_min = inf, line_max = -inf;
    double width = 0.;
    double y_min = inf, y_max = -inf;
    auto close_line = [&]() {
        if (line_max > line_min)
            width = std::max(width, line_max - line_min);
        line_min = inf;
        line_max = -inf;
    };

    for (size_t i = 0; i < shapes.size(); ++i) {
        const ExPolygonsWithId &shape = shapes[i];
        if (shape.id == ENTER_UNICODE) {
            close_line();
            continue;
        }
        if (!shape.expoly.empty()) {
            BoundingBox bb = get_extents(shape.expoly);
            if (bb.defined) {
                line_min = std::min(line_min, static_cast<double>(bb.min.x()));
                line_max = std::max(line_max, static_cast<double>(bb.max.x()));
                y_min    = std::min(y_min, static_cast<double>(bb.min.y()));
                y_max    = std::max(y_max, static_cast<double>(bb.max.y()));
            }
        }
        if (has_advances && (*advances)[i].valid) {
            line_min = std::min(line_min, (*advances)[i].x_min);
            line_max = std::max(line_max, (*advances)[i].x_max);
        }
    }
    close_line();

    if (y_max < y_min)
        return {}; // no outline at all
    result.width = width;
    result.y_min = y_min;
    result.y_max = y_max;
    return result;
}

BendResult resolve_bend(const EmbossBend &bend, const BendInput &input, double shape_scale, double tolerance_mm)
{
    BendResult result;
    if (!bend.is_active() || !input.is_valid() || !(shape_scale > 0.))
        return result;

    const double max_angle = BEND_MAX_ANGLE_DEG * M_PI / 180.;
    double radius = 0.;
    if (bend.mode == EmbossBend::Mode::angle) {
        double angle = std::min(static_cast<double>(bend.angle), BEND_MAX_ANGLE_DEG) * M_PI / 180.;
        radius = input.width / angle;
    } else {
        radius = static_cast<double>(bend.radius) / shape_scale;
        double min_radius_length = input.width / max_angle;
        if (radius < min_radius_length) {
            radius = min_radius_length;
            result.limited_by_length = true;
        }
    }

    // The part of the glyphs facing the arc centre must stay on the correct side of the centre
    double extent = bend.inside ? input.y_max : -input.y_min;
    if (extent > 0.) {
        double min_radius_height = BEND_MIN_RADIUS_RATIO * extent;
        if (radius < min_radius_height) {
            radius = min_radius_height;
            result.limited_by_height = true;
        }
    }

    if (!std::isfinite(radius) || radius <= 0.)
        return {};

    result.spec.radius    = radius;
    result.spec.inside    = bend.inside;
    result.spec.tolerance = (tolerance_mm > 0. ? tolerance_mm : BEND_TOLERANCE_MM) / shape_scale;
    result.angle_deg      = input.width / radius * 180. / M_PI;
    result.radius_mm      = radius * shape_scale;
    return result;
}

Vec2d bend_point(const Vec2d &p, const BendSpec &spec)
{
    if (!spec.is_active())
        return p;
    const double s     = spec.side();
    const double theta = p.x() / spec.radius;
    const double rho   = spec.radius + s * p.y();
    const double half  = std::sin(theta / 2.);
    // Y = s * (rho * cos(theta) - R), written without cancellation for huge radii
    return Vec2d(rho * std::sin(theta), p.y() * std::cos(theta) - s * spec.radius * 2. * half * half);
}

Polygon bend_polygon(const Polygon &polygon, const BendSpec &spec)
{
    if (!spec.is_active() || polygon.points.empty())
        return polygon;

    const Points &pts   = polygon.points;
    const size_t  count = pts.size();
    const double  s     = spec.side();
    Points        out;
    out.reserve(count * 2);
    auto push = [&out](const Point &p) {
        if (out.empty() || out.back() != p)
            out.push_back(p);
    };

    for (size_t i = 0; i < count; ++i) {
        const Vec2d a = pts[i].cast<double>();
        const Vec2d b = pts[(i + 1) % count].cast<double>();
        push(bend_round_point(bend_point(a, spec)));

        // A horizontal edge becomes an arc and a diagonal a spiral: subdivide by angle.
        // Vertical edges map onto radial lines and need no subdivision.
        const double dtheta = std::abs(b.x() - a.x()) / spec.radius;
        if (dtheta <= 0.)
            continue;
        const double rho_max = std::max(spec.radius + s * a.y(), spec.radius + s * b.y());
        const double step    = bend_max_angle_step(rho_max, spec.tolerance);
        const int    parts   = std::min(4096, static_cast<int>(std::ceil(dtheta / step)));
        for (int j = 1; j < parts; ++j) {
            const double t = static_cast<double>(j) / parts;
            push(bend_round_point(bend_point(a + t * (b - a), spec)));
        }
    }
    while (out.size() > 1 && out.front() == out.back())
        out.pop_back();
    Polygon result;
    result.points = std::move(out);
    return result;
}

ExPolygons bend_expolygons(const ExPolygons &shape, const BendSpec &spec)
{
    if (!spec.is_active())
        return shape;
    ExPolygons result;
    result.reserve(shape.size());
    for (const ExPolygon &expoly : shape) {
        Polygon contour = bend_polygon(expoly.contour, spec);
        if (contour.size() < 3)
            continue;
        ExPolygon bent(std::move(contour));
        bent.holes.reserve(expoly.holes.size());
        for (const Polygon &hole : expoly.holes) {
            Polygon h = bend_polygon(hole, spec);
            if (h.size() >= 3)
                bent.holes.push_back(std::move(h));
        }
        result.push_back(std::move(bent));
    }
    return result;
}

void bend_shapes(ExPolygonsWithIds &shapes, const BendSpec &spec)
{
    if (!spec.is_active())
        return;
    for (ExPolygonsWithId &shape : shapes)
        if (!shape.expoly.empty())
            shape.expoly = bend_expolygons(shape.expoly, spec);
}

std::vector<double> glyph_pivots(const ExPolygonsWithIds &shapes, const GlyphAdvances *advances)
{
    const bool has_advances = bend_use_advances(shapes, advances);
    std::vector<double> result(shapes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < shapes.size(); ++i) {
        if (has_advances && (*advances)[i].valid) {
            result[i] = (*advances)[i].center();
        } else if (!shapes[i].expoly.empty()) {
            BoundingBox bb = get_extents(shapes[i].expoly);
            if (bb.defined)
                result[i] = (static_cast<double>(bb.min.x()) + static_cast<double>(bb.max.x())) / 2.;
        }
    }
    return result;
}

void place_glyphs_on_arc(ExPolygonsWithIds &shapes, const std::vector<double> &pivots_x, const BendSpec &spec)
{
    if (!spec.is_active())
        return;
    const double s = spec.side();
    for (size_t i = 0; i < shapes.size(); ++i) {
        ExPolygons &expolys = shapes[i].expoly;
        if (expolys.empty())
            continue;
        double pivot_x = (i < pivots_x.size()) ? pivots_x[i] : std::numeric_limits<double>::quiet_NaN();
        if (!std::isfinite(pivot_x)) {
            BoundingBox bb = get_extents(expolys);
            if (!bb.defined)
                continue;
            pivot_x = (static_cast<double>(bb.min.x()) + static_cast<double>(bb.max.x())) / 2.;
        }
        const Vec2d  pivot(pivot_x, 0.);
        const Vec2d  target = bend_point(pivot, spec);
        const double phi    = -s * pivot_x / spec.radius;
        const double c      = std::cos(phi);
        const double sn     = std::sin(phi);
        auto move = [&](Polygon &polygon) {
            for (Point &p : polygon.points) {
                const Vec2d d = p.cast<double>() - pivot;
                p = bend_round_point(Vec2d(target.x() + c * d.x() - sn * d.y(), target.y() + sn * d.x() + c * d.y()));
            }
        };
        for (ExPolygon &expoly : expolys) {
            move(expoly.contour);
            for (Polygon &hole : expoly.holes)
                move(hole);
        }
    }
}

BendResult apply_bend(ExPolygonsWithIds &shapes, const EmbossBend &bend, double shape_scale, const GlyphAdvances *advances,
                      double tolerance_mm)
{
    if (!bend.is_active())
        return {};
    BendResult result = resolve_bend(bend, measure_bend_input(shapes, advances), shape_scale, tolerance_mm);
    if (!result.is_active())
        return result;
    if (bend.rigid)
        place_glyphs_on_arc(shapes, glyph_pivots(shapes, advances), result.spec);
    else
        bend_shapes(shapes, result.spec);
    return result;
}

std::optional<RoundOutline> fit_round_outline(const std::vector<Vec2d> &rim)
{
    const size_t min_count = 12;
    if (rim.size() < min_count)
        return {};
    // centre the points first for a well conditioned system
    Vec2d c0 = Vec2d::Zero();
    for (const Vec2d &p : rim)
        c0 += p;
    c0 /= static_cast<double>(rim.size());

    // x^2 + y^2 + D x + E y + F = 0
    Eigen::Matrix3d a = Eigen::Matrix3d::Zero();
    Eigen::Vector3d b = Eigen::Vector3d::Zero();
    for (const Vec2d &p : rim) {
        const Vec2d     d = p - c0;
        Eigen::Vector3d row(d.x(), d.y(), 1.);
        a += row * row.transpose();
        b -= row * d.squaredNorm();
    }
    Eigen::Vector3d solution = a.colPivHouseholderQr().solve(b);
    const Vec2d  center(-solution(0) / 2., -solution(1) / 2.);
    const double radius_sq = center.squaredNorm() - solution(2);
    if (!std::isfinite(radius_sq) || radius_sq <= 0.)
        return {};
    const double radius = std::sqrt(radius_sq);

    double              sum_sq = 0.;
    std::vector<double> angles;
    angles.reserve(rim.size());
    for (const Vec2d &p : rim) {
        const Vec2d  d    = p - c0 - center;
        const double dist = d.norm() - radius;
        sum_sq += dist * dist;
        angles.push_back(std::atan2(d.y(), d.x()));
    }
    if (std::sqrt(sum_sq / rim.size()) > 0.01 * radius)
        return {}; // not round
    std::sort(angles.begin(), angles.end());
    double max_gap = angles.front() + 2. * M_PI - angles.back();
    for (size_t i = 1; i < angles.size(); ++i)
        max_gap = std::max(max_gap, angles[i] - angles[i - 1]);
    if (max_gap > M_PI / 4.)
        return {}; // only a part of a circle
    return RoundOutline{c0 + center, radius};
}

namespace {

// Convex hull (Andrew's monotone chain), counter-clockwise without collinear points
std::vector<Vec2d> bend_convex_hull(std::vector<Vec2d> points)
{
    std::sort(points.begin(), points.end(), [](const Vec2d &a, const Vec2d &b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3)
        return points;
    auto cross = [](const Vec2d &o, const Vec2d &a, const Vec2d &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<Vec2d> hull(2 * points.size());
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0.)
            --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.)
            --k;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

} // namespace

std::optional<SurfaceRound> surface_round_area(const std::vector<Vec3d> &points, double max_slope_deg)
{
    if (points.size() < 12)
        return {};
    std::vector<Vec2d> flat;
    flat.reserve(points.size());
    for (const Vec3d &p : points)
        flat.emplace_back(p.x(), p.y());
    std::optional<RoundOutline> outline = fit_round_outline(bend_convex_hull(std::move(flat)));
    if (!outline.has_value())
        return {};

    // Height of the surface facing the text (largest z) over the distance from the centre
    const int    bins = 64;
    const double r    = outline->radius;
    std::vector<double> top_z(bins, -std::numeric_limits<double>::infinity());
    std::vector<double> top_r(bins, 0.);
    for (const Vec3d &p : points) {
        const double d = (Vec2d(p.x(), p.y()) - outline->center).norm();
        const int    i = std::clamp(static_cast<int>(d / r * bins), 0, bins - 1);
        if (p.z() > top_z[i]) {
            top_z[i] = p.z();
            top_r[i] = d;
        }
    }

    // Walk inward from the outline to the first place where the surface is not steeper than the
    // limit. Inward, so a knob or handle in the middle of a lid does not shrink the usable ring.
    const double max_slope = std::tan(std::clamp(max_slope_deg, 1., 89.) * M_PI / 180.);
    const double min_step  = r * 1e-3;
    double usable = 0.;
    int    outer  = -1;
    for (int i = bins - 1; i >= 0; --i) {
        if (!std::isfinite(top_z[i]))
            continue;
        if (outer < 0) {
            outer = i;
            continue;
        }
        const double dr = top_r[outer] - top_r[i];
        if (dr < min_step)
            continue;
        if (std::abs(top_z[outer] - top_z[i]) / dr <= max_slope) {
            usable = top_r[outer];
            break;
        }
        outer = i;
    }
    if (outer >= 0 && usable <= 0.)
        usable = top_r[outer]; // steep everywhere (a cone tip): the innermost sample

    SurfaceRound result;
    result.center        = outline->center;
    result.radius        = r;
    result.usable_radius = std::clamp(usable, 0., r);
    return result;
}

} // namespace Slic3r::Emboss
