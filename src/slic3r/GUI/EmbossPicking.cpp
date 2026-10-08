#include "EmbossPicking.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::GUI::EmbossPicking {

double prefer_tolerance_from_depth(double emboss_depth_world)
{
    if (!std::isfinite(emboss_depth_world))
        return MIN_PREFER_TOLERANCE;
    return std::clamp(std::abs(emboss_depth_world), MIN_PREFER_TOLERANCE, MAX_PREFER_TOLERANCE);
}

size_t prefer_text_or_svg(const std::vector<VolumeHit> &hits, size_t closest)
{
    if (closest >= hits.size())
        return closest;

    const VolumeHit &winner = hits[closest];
    // Already a text or SVG part, or not part of a model object: nothing to prefer.
    if (winner.is_text_or_svg() || winner.object_id < 0)
        return closest;

    size_t best       = closest;
    double best_depth = std::numeric_limits<double>::max();
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i == closest)
            continue;
        const VolumeHit &hit = hits[i];
        if (!hit.is_text_or_svg())
            continue;
        if (hit.object_id != winner.object_id || hit.instance_id != winner.instance_id)
            continue;
        if (hit.depth > winner.depth + hit.prefer_tolerance)
            continue;
        if (hit.depth < best_depth) {
            best       = i;
            best_depth = hit.depth;
        }
    }
    return best;
}

bool footprint_takes_over(HoverOwner owner)
{
    switch (owner) {
    case HoverOwner::Nothing:
    case HoverOwner::EditedVolume:
    case HoverOwner::SameObjectPart: return true;
    case HoverOwner::OtherTextOrSvg:
    case HoverOwner::OtherObject:
    case HoverOwner::GizmoHandle:
    default: return false;
    }
}

namespace {

double distance_to_segment(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab  = b - a;
    const double len = ab.squaredNorm();
    if (len <= 0.)
        return (p - a).norm();
    const double t = std::clamp((p - a).dot(ab) / len, 0., 1.);
    return (p - (a + t * ab)).norm();
}

} // namespace

bool footprint_contains(const Polygon &hull, const Vec2d &point, double padding)
{
    const Points &pts = hull.points;
    if (pts.empty())
        return false;

    const auto to_vec = [](const Point &pt) { return Vec2d(double(pt.x()), double(pt.y())); };

    if (pts.size() == 1)
        return (point - to_vec(pts.front())).norm() <= padding;

    // Inside test for a convex polygon of either orientation: the point lies on the same side of
    // every edge (zero counts as inside, so points on the outline are in).
    if (pts.size() >= 3) {
        bool has_pos = false;
        bool has_neg = false;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Vec2d  a     = to_vec(pts[i]);
            const Vec2d  b     = to_vec(pts[(i + 1) % pts.size()]);
            const double cross = (b.x() - a.x()) * (point.y() - a.y()) - (b.y() - a.y()) * (point.x() - a.x());
            if (cross > 0.)
                has_pos = true;
            else if (cross < 0.)
                has_neg = true;
            if (has_pos && has_neg)
                break;
        }
        if (!(has_pos && has_neg))
            return true;
    }

    if (padding <= 0.)
        return false;

    for (size_t i = 0; i < pts.size(); ++i) {
        const size_t j = (i + 1) % pts.size();
        if (pts.size() == 2 && i == 1)
            break;
        if (distance_to_segment(point, to_vec(pts[i]), to_vec(pts[j])) <= padding)
            return true;
    }
    return false;
}

} // namespace Slic3r::GUI::EmbossPicking
