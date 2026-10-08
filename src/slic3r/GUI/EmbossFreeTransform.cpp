#include "EmbossFreeTransform.hpp"

#include <cmath>

namespace Slic3r::GUI::EmbossFreeTransform {

Kind classify_move(const Vec3d &displacement, const Vec3d &normal)
{
    if (displacement.norm() < MOVE_EPSILON)
        return Kind::None;
    const double n_len = normal.norm();
    if (n_len <= 0.)
        return Kind::Normal; // no idea where the surface is: treat as leaving it
    const double along_normal = std::abs(displacement.dot(normal) / n_len);
    return along_normal < MOVE_EPSILON ? Kind::Tangent : Kind::Normal;
}

Kind classify_rotation(const Vec3d &axis, const Vec3d &normal, double angle)
{
    if (std::abs(angle) < 1e-9)
        return Kind::None;
    const double len = axis.norm() * normal.norm();
    if (len <= 0.)
        return Kind::Tilt;
    return std::abs(axis.dot(normal)) / len >= IN_PLANE_COS ? Kind::InPlane : Kind::Tilt;
}

Outcome outcome(const Projection &projection, Kind kind)
{
    Outcome res;
    switch (kind) {
    case Kind::None:
        break;
    case Kind::Tangent:
    case Kind::InPlane:
        // Still parallel to the surface and over it: a projection simply follows.
        res.reprocess        = projection.follows_surface();
        res.measure_distance = !projection.follows_surface();
        break;
    case Kind::Normal:
    case Kind::Tilt:
        // The projection would put the part straight back onto the surface: let it go free.
        res.detach           = projection.follows_surface();
        res.reprocess        = res.detach;
        res.measure_distance = true;
        break;
    }
    return res;
}

Projection detached(const Projection &projection)
{
    Projection res  = projection;
    res.use_surface = false;
    res.per_glyph   = false;
    return res;
}

std::optional<Projection> reattach(const Projection &current, const std::optional<Projection> &remembered)
{
    if (!remembered.has_value() || !remembered->follows_surface())
        return {};
    // The user changed the projection meanwhile (or undo brought the old one back): keep theirs.
    if (current.follows_surface())
        return {};
    return remembered;
}

Placement classify_placement(bool is_object, const Projection &projection, const std::optional<SurfaceProbe> &probe, double max_distance)
{
    if (is_object)
        return Placement::Object;
    if (projection.follows_surface())
        return Placement::Projected;
    if (!probe.has_value() || std::abs(probe->distance) > max_distance)
        return Placement::Free;
    if (std::abs(probe->cos_angle) < PARALLEL_COS)
        return Placement::Tilted;
    if (std::abs(probe->distance) <= ON_SURFACE_DISTANCE)
        return Placement::OnSurface;
    return Placement::Floating;
}

Transform3d moved_volume_matrix(const Transform3d &instance, const Transform3d &volume, const Transform3d &world_delta)
{
    // world = instance * volume  ->  world' = delta * instance * volume = instance * volume'
    return instance.inverse() * world_delta * instance * volume;
}

Transform3d rotation_about(const Vec3d &pivot, const Vec3d &axis, double angle)
{
    Transform3d res = Transform3d::Identity();
    const double len = axis.norm();
    if (len <= 0.)
        return res;
    res.translate(pivot);
    res.rotate(Eigen::AngleAxisd(angle, axis / len));
    res.translate(-pivot);
    return res;
}

double axis_drag_distance(const Vec3d &start, const Vec3d &axis, const Vec3d &ray_a, const Vec3d &ray_dir, double snap)
{
    // Same as GLGizmoMove3D::calc_projection: the closest point of the ray to the start lies in the
    // plane through the start perpendicular to the ray; its offset is measured along the axis.
    const Vec3d inters = ray_a + (start - ray_a).dot(ray_dir) * ray_dir;
    double      res    = (inters - start).dot(axis);
    if (snap > 0.)
        res = snap * std::round(res / snap);
    return res;
}

double angle_delta(double from, double to)
{
    double d = std::fmod(to - from, 2. * PI);
    if (d <= -PI)
        d += 2. * PI;
    else if (d > PI)
        d -= 2. * PI;
    return d;
}

bool arrow_visible(const Vec3d &axis, const Vec3d &view_dir)
{
    const double len = axis.norm() * view_dir.norm();
    if (len <= 0.)
        return false;
    return std::abs(axis.dot(view_dir)) / len <= ARROW_HIDE_COS;
}

} // namespace Slic3r::GUI::EmbossFreeTransform
