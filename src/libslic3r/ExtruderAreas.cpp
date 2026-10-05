#include "ExtruderAreas.hpp"

#include "ClipperUtils.hpp"
#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

namespace Slic3r {

bool ExtruderAreas::has_exclusive_regions() const
{
    for (const Polygons &o : only)
        if (!o.empty())
            return true;
    return false;
}

bool ExtruderAreas::has_height_limits() const
{
    if (heights.size() < 2)
        return false;
    for (size_t e = 1; e < heights.size(); ++e)
        if (std::abs(height_limit(e) - height_limit(0)) > 1e-6)
            return true;
    return false;
}

// Smallest area (scaled units squared) that counts as "something is left": clipping a polygon against an
// identical or touching one leaves hairlines of a few scaled units that must not read as a real overhang.
// 1e-4 mm^2 (SCALING_FACTOR changes with the bed size, so this is not a constant).
static double residual_area() { return 1e-4 * double(scale_(1.)) * double(scale_(1.)); }

static double total_area(const Polygons &polygons)
{
    double a = 0.;
    for (const Polygon &p : polygons)
        a += std::abs(p.area());
    return a;
}

ExtruderAreas compute_extruder_areas(const Pointfs &bed, const std::vector<Pointfs> &extruder_areas, const std::vector<double> &extruder_heights)
{
    ExtruderAreas out;
    if (extruder_areas.size() < 2 || bed.size() < 3)
        return out;

    const Polygon bed_poly = Polygon::new_scale(bed);
    for (const Pointfs &area : extruder_areas) {
        // A missing / degenerate polygon means this nozzle is not constrained.
        Polygons reach = area.size() >= 3 ? Polygons{ Polygon::new_scale(area) } : Polygons{ bed_poly };
        for (Polygon &p : reach)
            p.make_counter_clockwise();
        out.printable.emplace_back(std::move(reach));
    }

    const size_t n = out.printable.size();
    out.shared     = out.printable.front();
    for (size_t e = 1; e < n; ++e)
        out.shared = intersection(out.shared, out.printable[e]);

    for (size_t e = 0; e < n; ++e) {
        out.unprintable.emplace_back(diff(Polygons{ bed_poly }, out.printable[e]));
        Polygons others;
        for (size_t o = 0; o < n; ++o)
            if (o != e)
                append(others, out.printable[o]);
        Polygons strip = diff(out.printable[e], union_(others));
        // Drop slivers left by sub-micron rounding of the profile's coordinates.
        if (total_area(strip) < residual_area())
            strip.clear();
        out.only.emplace_back(std::move(strip));
    }

    out.heights.assign(n, 0.);
    for (size_t e = 0; e < n && e < extruder_heights.size(); ++e) {
        const double h = extruder_heights[e];
        out.heights[e] = (std::isfinite(h) && h > 0.) ? h : 0.;
    }
    return out;
}

// Four points that are the four corners of an axis-aligned rectangle (any order).
static bool is_axis_aligned_rectangle(Pointfs::const_iterator first)
{
    double min_x = first->x(), max_x = first->x(), min_y = first->y(), max_y = first->y();
    for (int i = 1; i < 4; ++i) {
        min_x = std::min(min_x, (first + i)->x());
        max_x = std::max(max_x, (first + i)->x());
        min_y = std::min(min_y, (first + i)->y());
        max_y = std::max(max_y, (first + i)->y());
    }
    if (max_x - min_x < EPSILON || max_y - min_y < EPSILON)
        return false;
    int seen = 0; // one bit per corner (min/max x, min/max y)
    for (int i = 0; i < 4; ++i) {
        const double x = (first + i)->x(), y = (first + i)->y();
        const bool   x_min = std::abs(x - min_x) < EPSILON, x_max = std::abs(x - max_x) < EPSILON;
        const bool   y_min = std::abs(y - min_y) < EPSILON, y_max = std::abs(y - max_y) < EPSILON;
        if ((!x_min && !x_max) || (!y_min && !y_max))
            return false;
        seen |= 1 << ((x_max ? 2 : 0) + (y_max ? 1 : 0));
    }
    return seen == 0xF;
}

std::vector<Pointfs> split_merged_extruder_areas(std::vector<Pointfs> areas)
{
    if (areas.size() == 1 && areas.front().size() == 8 && is_axis_aligned_rectangle(areas.front().cbegin()) &&
        is_axis_aligned_rectangle(areas.front().cbegin() + 4)) {
        const Pointfs merged = areas.front();
        areas.assign({ Pointfs(merged.begin(), merged.begin() + 4), Pointfs(merged.begin() + 4, merged.end()) });
    }
    return areas;
}

ExtruderAreas extruder_areas_from_config(const ConfigBase &config)
{
    const auto *bed    = dynamic_cast<const ConfigOptionPoints *>(config.option("printable_area"));
    const auto *groups = dynamic_cast<const ConfigOptionPointsGroups *>(config.option("extruder_printable_area"));
    if (bed == nullptr || groups == nullptr)
        return {};
    // extruder_printable_height is declared nullable, so a dynamic config holds the Nullable flavour of the
    // vector; both derive from ConfigOptionVector<double>.
    std::vector<double> heights;
    if (const auto *h = dynamic_cast<const ConfigOptionVector<double> *>(config.option("extruder_printable_height")))
        heights = h->values;
    std::vector<Pointfs> areas;
    areas.reserve(groups->values.size());
    for (const auto &group : groups->values)
        areas.emplace_back(group.begin(), group.end());
    return compute_extruder_areas(bed->values, split_merged_extruder_areas(std::move(areas)), heights);
}

ExtruderAreas translate_extruder_areas(const ExtruderAreas &areas, const Vec2d &offset)
{
    ExtruderAreas out = areas;
    const Point   d   = Point::new_scale(offset.x(), offset.y());
    auto          move = [&d](Polygons &polygons) {
        for (Polygon &p : polygons)
            p.translate(d);
    };
    for (Polygons &p : out.printable)
        move(p);
    for (Polygons &p : out.unprintable)
        move(p);
    for (Polygons &p : out.only)
        move(p);
    move(out.shared);
    return out;
}

ExPolygons hatch_region(const Polygons &region, double period_mm, double stripe_mm)
{
    if (region.empty() || period_mm <= 0. || stripe_mm <= 0.)
        return {};
    const BoundingBox box    = get_extents(region);
    const coord_t     period = coord_t(scale_(period_mm));
    const coord_t     stripe = coord_t(scale_(stripe_mm));
    if (period <= 0 || stripe <= 0)
        return {};
    const coord_t y0 = box.min.y() - 1, y1 = box.max.y() + 1;
    // A stripe is the band between the lines x + y = k and x + y = k + stripe.
    Polygons stripes;
    for (coord_t k = box.min.x() + y0 - period; k <= box.max.x() + y1; k += period)
        stripes.emplace_back(Polygon{ Point(k - y0, y0), Point(k + stripe - y0, y0), Point(k + stripe - y1, y1), Point(k - y1, y1) });
    return intersection_ex(stripes, region);
}

bool point_in_area(const Point &point, const Polygons &area)
{
    return contains(area, point, true);
}

bool footprint_within(const Polygons &footprint, const Polygons &area, double tolerance_mm)
{
    if (footprint.empty())
        return true;
    if (area.empty())
        return false;
    const Polygons roomy = tolerance_mm > 0. ? offset(area, float(scale_(tolerance_mm)), ClipperLib::jtMiter, 3.) : area;
    return total_area(diff(footprint, roomy)) < residual_area();
}

std::vector<bool> extruders_reaching(const ExtruderAreas &areas, const Polygons &footprint, double max_z, double tolerance_mm)
{
    std::vector<bool> reach(areas.count(), true);
    for (size_t e = 0; e < areas.count(); ++e) {
        const double limit = areas.height_limit(e);
        if (limit > 0. && max_z > limit + 0.01)
            reach[e] = false;
        else if (!footprint_within(footprint, areas.printable[e], tolerance_mm))
            reach[e] = false;
    }
    return reach;
}

std::vector<ReachViolation> find_reach_violations(const std::vector<ObjectReach> &objects, size_t extruder_count,
                                                  const std::vector<int> &filament_map, bool manual)
{
    std::vector<ReachViolation> out;
    if (extruder_count < 2)
        return out;

    auto reachable = [](const ObjectReach &o, size_t e) { return e >= o.reach.size() || o.reach[e]; };
    auto add_object = [](std::vector<int> &ids, int id) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    };

    if (manual) {
        // (filament, extruder) -> violation, kept in first-seen order.
        std::vector<ReachViolation> found;
        for (const ObjectReach &o : objects)
            for (int f : o.filaments) {
                if (f < 0 || f >= int(filament_map.size()))
                    continue; // the map does not say where this filament goes: nothing to verify
                const int e = filament_map[size_t(f)] - 1;
                if (e < 0 || e >= int(extruder_count) || reachable(o, size_t(e)))
                    continue;
                auto it = std::find_if(found.begin(), found.end(), [f, e](const ReachViolation &v) { return v.filament == f && v.extruder == e; });
                if (it == found.end()) {
                    found.push_back({ f, e, {} });
                    it = std::prev(found.end());
                }
                add_object(it->object_ids, o.id);
            }
        std::sort(found.begin(), found.end(), [](const ReachViolation &a, const ReachViolation &b) {
            return a.filament != b.filament ? a.filament < b.filament : a.extruder < b.extruder;
        });
        return found;
    }

    // Automatic grouping: which nozzles each filament is shut out of, over every object that uses it.
    std::set<int> filaments;
    for (const ObjectReach &o : objects)
        filaments.insert(o.filaments.begin(), o.filaments.end());
    for (int f : filaments) {
        std::vector<bool> blocked(extruder_count, false);
        std::vector<int>  blockers;
        for (const ObjectReach &o : objects) {
            if (std::find(o.filaments.begin(), o.filaments.end(), f) == o.filaments.end())
                continue;
            bool any = false;
            for (size_t e = 0; e < extruder_count; ++e)
                if (!reachable(o, e)) {
                    blocked[e] = true;
                    any        = true;
                }
            if (any)
                add_object(blockers, o.id);
        }
        if (std::all_of(blocked.begin(), blocked.end(), [](bool b) { return b; }))
            out.push_back({ f, -1, std::move(blockers) });
    }
    return out;
}

} // namespace Slic3r
