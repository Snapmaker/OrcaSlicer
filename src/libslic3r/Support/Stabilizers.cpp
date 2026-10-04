#include "Stabilizers.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Fill/FillBase.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../TriangleSelector.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace Slic3r {

namespace stabilizers {

// Farthest crossing of the ray `from + t * dir` (t > 0) with the island's outer contour, as t.
// Negative when the ray never crosses it.
static double outermost_crossing(const Polygon &contour, const Vec2d &from, const Vec2d &dir, Vec2d *normal = nullptr)
{
    double best = -1.;
    const size_t n = contour.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec2d a = contour[i].cast<double>();
        const Vec2d b = contour[(i + 1) % n].cast<double>();
        const Vec2d e = b - a;
        const double denom = dir.x() * e.y() - dir.y() * e.x();
        if (std::abs(denom) < EPSILON)
            continue;
        const Vec2d  w = a - from;
        const double t = (w.x() * e.y() - w.y() * e.x()) / denom; // along the ray
        const double s = (w.x() * dir.y() - w.y() * dir.x()) / denom; // along the edge
        if (t > 0. && s >= 0. && s <= 1. && t > best) {
            best = t;
            // Outward of a counter-clockwise contour.
            if (normal != nullptr)
                *normal = Vec2d(e.y(), -e.x()).normalized();
        }
    }
    return best;
}

// The layer whose slicing plane is nearest to height z. Layers must be sorted and not empty.
static std::vector<LayerOutline>::const_iterator nearest_layer(const std::vector<LayerOutline> &layers, double z)
{
    auto it = std::lower_bound(layers.begin(), layers.end(), float(z),
                               [](const LayerOutline &l, float zz) { return l.slice_z < zz; });
    if (it == layers.end())
        it = std::prev(it);
    else if (it != layers.begin() && std::abs(std::prev(it)->slice_z - z) < std::abs(it->slice_z - z))
        it = std::prev(it);
    return it;
}

std::vector<Contact> ring_contacts(const std::vector<LayerOutline> &layers, const RingParams &params)
{
    std::vector<Contact> out;
    if (layers.empty() || params.ring_spacing <= EPSILON || params.points_per_ring < 1)
        return out;

    const float  top_z      = layers.back().slice_z;
    const double min_area   = sqr(scaled<double>(params.tip_diameter)) * M_PI;
    const double max_width  = scaled<double>(params.max_island_width);
    const int    n          = params.points_per_ring;
    const double step_angle = 2. * M_PI / n;

    for (double z = params.ring_spacing; z <= double(top_z) - params.top_margin; z += params.ring_spacing) {
        // The layer whose slicing plane is nearest to the ring height.
        auto it = nearest_layer(layers, z);
        if (it->islands == nullptr)
            continue;

        // Every ring uses the same directions, so the struts of all rings come down onto the same
        // few pillars: each pillar is tied to the part at every ring instead of standing alone for
        // two ring spacings, and there are N pillars in all rather than N per ring.
        for (const ExPolygon &island : *it->islands) {
            if (island.contour.size() < 3 || std::abs(island.contour.area()) < min_area)
                continue;
            if (max_width > 0.) {
                const BoundingBox bb   = get_extents(island.contour);
                const Point       size = bb.size();
                if (double(std::min(size.x(), size.y())) > max_width)
                    continue;
            }
            const Vec2d c = island.contour.centroid().cast<double>();
            for (int i = 0; i < n; ++i) {
                const double a = i * step_angle;
                const Vec2d  dir(std::cos(a), std::sin(a));
                Vec2d        normal = dir;
                const double t = outermost_crossing(island.contour, c, dir, &normal);
                if (t <= 0.)
                    continue;
                const Vec2d p = c + dir * t;
                out.push_back({ Vec2d(unscaled(p.x()), unscaled(p.y())), dir, size_t(it - layers.begin()), it->slice_z, normal });
            }
        }
    }
    return out;
}

sla::SupportPoints ring_points(const std::vector<LayerOutline> &layers, const RingParams &params)
{
    sla::SupportPoints out;
    const float tip_r = float(0.5 * params.tip_diameter);
    for (const Contact &c : ring_contacts(layers, params))
        out.emplace_back(Vec3f(float(c.pos.x()), float(c.pos.y()), c.z), tip_r);
    return out;
}

// --- settings -----------------------------------------------------------------------------------

StabilizerSettings StabilizerSettings::from_config(const PrintObjectConfig &cfg, double support_line_width)
{
    StabilizerSettings s;
    s.rings.ring_spacing     = cfg.stabilizer_ring_spacing.value;
    s.rings.points_per_ring  = cfg.stabilizer_points_per_ring.value;
    s.rings.tip_diameter     = cfg.stabilizer_tip_diameter.value;
    s.rings.max_island_width = cfg.stabilizer_max_island_width.value;
    s.tip_gap                = cfg.stabilizer_tip_gap.value;
    // Two perimeters on each side at least, or a pillar is a single wobbly loop.
    s.pillar_radius          = 0.5 * std::max(cfg.stabilizer_pillar_diameter.value, 4. * support_line_width);
    s.clearance              = std::max(1.0, cfg.support_object_xy_distance.value);
    s.max_run                = 10.;
    const StabilizerMode mode = cfg.stabilizer_supports.value;
    s.ring_struts            = mode == smAuto;
    s.painted_points         = mode != smOff;

    // v2. Each default leaves v1 as it was.
    s.pillar_base_radius     = 0.5 * cfg.stabilizer_pillar_base_diameter.value;
    s.bracing                = cfg.stabilizer_bracing.value;
    s.max_unbraced           = std::max(1., cfg.stabilizer_brace_max_unbraced.value);
    s.max_brace_span         = std::max(1., cfg.stabilizer_brace_max_span.value);
    s.column_shape           = cfg.stabilizer_column_shape.value;
    s.column_width           = cfg.stabilizer_column_width.value;
    s.column_length          = cfg.stabilizer_column_length.value;
    s.column_min_height      = cfg.stabilizer_column_min_height.value;
    s.wall_loops             = std::max(0, cfg.stabilizer_wall_loops.value);
    s.infill_density         = std::clamp(cfg.stabilizer_infill_density.value / 100., 0., 1.);
    s.infill_pattern         = cfg.stabilizer_infill_pattern.value;
    // A wide base stands further out; give its strut the room to reach it.
    if (s.tapered())
        s.max_run = std::max(s.max_run, s.pillar_base_radius + s.clearance + 4.);
    return s;
}

StabilizerSettings settings_of(const PrintObject &object)
{
    return StabilizerSettings::from_config(object.config(), support_material_flow(&object).width());
}

std::vector<LayerOutline> outlines_of(const PrintObject &object)
{
    std::vector<LayerOutline> outlines;
    outlines.reserve(object.layers().size());
    for (const Layer *layer : object.layers())
        outlines.push_back({ float(layer->slice_z), &layer->lslices });
    return outlines;
}

// --- painted points -----------------------------------------------------------------------------

std::vector<PaintedSpot> painted_spots(const ModelObject &object, const Transform3d &trafo, double ring_spacing)
{
    std::vector<PaintedSpot> out;
    for (const ModelVolume *mv : object.volumes) {
        if (! mv->is_model_part() || mv->supported_facets.empty() ||
            ! mv->supported_facets.has_facets(*mv, EnforcerBlockerType::STABILIZER))
            continue;
        indexed_triangle_set its = mv->supported_facets.get_facets_strict(*mv, EnforcerBlockerType::STABILIZER);
        if (its.indices.empty())
            continue;
        its_transform(its, trafo * mv->get_matrix(), true);

        // Connected patches: triangles sharing a vertex position (split triangles may carry their
        // own copies of a vertex, so positions, not indices).
        std::vector<int> parent(its.vertices.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = int(i);
        auto find = [&parent](int i) {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        struct KeyHash
        {
            size_t operator()(const std::tuple<int64_t, int64_t, int64_t> &k) const
            {
                return std::hash<int64_t>()(std::get<0>(k)) * 73856093 ^ std::hash<int64_t>()(std::get<1>(k)) * 19349663 ^
                       std::hash<int64_t>()(std::get<2>(k)) * 83492791;
            }
        };
        std::unordered_map<std::tuple<int64_t, int64_t, int64_t>, int, KeyHash> by_pos;
        auto key = [](const Vec3f &v) {
            return std::make_tuple(int64_t(std::llround(v.x() * 1000.)), int64_t(std::llround(v.y() * 1000.)),
                                   int64_t(std::llround(v.z() * 1000.)));
        };
        for (size_t i = 0; i < its.vertices.size(); ++i) {
            auto [it, inserted] = by_pos.emplace(key(its.vertices[i]), int(i));
            if (! inserted)
                parent[find(int(i))] = find(it->second);
        }
        for (const stl_triangle_vertex_indices &f : its.indices) {
            parent[find(f(1))] = find(f(0));
            parent[find(f(2))] = find(f(0));
        }

        struct Tri { Vec3d c; Vec3d n; double area; };
        std::unordered_map<int, std::vector<Tri>> patches;
        for (const stl_triangle_vertex_indices &f : its.indices) {
            const Vec3d a = its.vertices[f(0)].cast<double>(), b = its.vertices[f(1)].cast<double>(),
                        c = its.vertices[f(2)].cast<double>();
            const Vec3d  cr   = (b - a).cross(c - a);
            const double area = 0.5 * cr.norm();
            if (area <= 0.)
                continue;
            patches[find(f(0))].push_back({ (a + b + c) / 3., cr.normalized(), area });
        }

        for (auto &[id, tris] : patches) {
            double zmin = std::numeric_limits<double>::max(), zmax = std::numeric_limits<double>::lowest();
            for (const Tri &t : tris) {
                zmin = std::min(zmin, t.c.z());
                zmax = std::max(zmax, t.c.z());
            }
            // A patch taller than a ring spacing asks for one strut per spacing of its height.
            const size_t bands = ring_spacing > EPSILON && zmax - zmin > ring_spacing ?
                                     size_t(std::ceil((zmax - zmin) / ring_spacing)) : 1;
            std::vector<Vec3d>  sum_c(bands, Vec3d::Zero()), sum_n(bands, Vec3d::Zero());
            std::vector<double> sum_a(bands, 0.);
            for (const Tri &t : tris) {
                const size_t b = bands == 1 ? 0 :
                    std::min(bands - 1, size_t((t.c.z() - zmin) / ((zmax - zmin) / double(bands))));
                sum_c[b] += t.c * t.area;
                sum_n[b] += t.n * t.area;
                sum_a[b] += t.area;
            }
            for (size_t b = 0; b < bands; ++b)
                if (sum_a[b] > 0.) {
                    const double nn = sum_n[b].norm();
                    out.push_back({ sum_c[b] / sum_a[b], nn > EPSILON ? Vec3d(sum_n[b] / nn) : Vec3d::Zero() });
                }
        }
    }
    // Deterministic order (the patch map is unordered): bottom up, then by position.
    std::sort(out.begin(), out.end(), [](const PaintedSpot &a, const PaintedSpot &b) {
        return std::make_tuple(a.pos.z(), a.pos.x(), a.pos.y()) < std::make_tuple(b.pos.z(), b.pos.x(), b.pos.y());
    });
    return out;
}

std::vector<PaintedSpot> painted_spots(const PrintObject &object)
{
    if (object.model_object() == nullptr)
        return {};
    return painted_spots(*object.model_object(), object.trafo_centered(), object.config().stabilizer_ring_spacing.value);
}

// The contact a painted spot asks for: the point of the nearest island's outline, at the layer
// nearest the spot's height, closest to the spot. Its direction is the painted surface's own,
// flattened; on a flat top or bottom (no sideways normal) the outline's own outward normal there.
static bool painted_contact(const std::vector<LayerOutline> &layers, const PaintedSpot &spot, Contact &out)
{
    if (layers.empty())
        return false;
    auto it = nearest_layer(layers, spot.pos.z());
    if (it->islands == nullptr || it->islands->empty())
        return false;
    const Point p(scaled(spot.pos.x()), scaled(spot.pos.y()));
    const ExPolygon *best      = nullptr;
    Point            best_pt   = p;
    size_t           best_edge = 0;
    double           best_d2   = std::numeric_limits<double>::max();
    for (const ExPolygon &island : *it->islands) {
        if (island.contour.size() < 3)
            continue;
        size_t      edge = 0;
        const Point q    = island.contour.point_projection(p, &edge);
        const double d2  = (q - p).cast<double>().squaredNorm();
        if (d2 < best_d2) {
            best_d2   = d2;
            best      = &island;
            best_pt   = q;
            best_edge = edge;
        }
    }
    // The spot must lie on this layer's outline, not across the plate from it.
    if (best == nullptr || best_d2 > sqr(scaled<double>(3.)))
        return false;

    const Polygon &contour = best->contour;
    const Vec2d    e       = (contour[(best_edge + 1) % contour.size()] - contour[best_edge]).cast<double>();
    if (e.norm() < EPSILON)
        return false;
    const Vec2d wall = Vec2d(e.y(), -e.x()).normalized(); // outward of a counter-clockwise contour
    Vec2d dir(spot.normal.x(), spot.normal.y());
    if (dir.norm() > 0.2)
        dir.normalize();
    else
        dir = wall;
    const Vec2d pos = unscaled(best_pt);
    // Out of the part, whatever the paint's winding said.
    const Vec2d probe = pos + dir * 0.05;
    if (best->contains(Point(scaled(probe.x()), scaled(probe.y()))))
        dir = -dir;
    out = { pos, dir, size_t(it - layers.begin()), it->slice_z, wall };
    return true;
}

// --- geometry ---------------------------------------------------------------------------------
//
// A stabilizer is a vertical pillar standing on the bed plus one 45 degree strut per ring that it
// serves. A strut starts at the pillar's top and climbs towards the part, tapering from the pillar
// radius to the tip radius, and ends with its tip on the wall. It is built directly as the cross
// sections at the object's layers, so every layer is the one below it moved in by one layer height
// at most - a 45 degree overhang, which FDM prints - and nothing ever hangs in the air.

static constexpr double SLOPE_STEP = 1.0;  // horizontal run per mm of height: 45 degrees
static constexpr int    CIRCLE_SEGMENTS = 24;

static Polygon ellipse(const Vec2d &c, const Vec2d &dir, double a, double b)
{
    Polygon poly;
    poly.points.reserve(CIRCLE_SEGMENTS);
    const Vec2d perp(-dir.y(), dir.x());
    for (int i = 0; i < CIRCLE_SEGMENTS; ++i) {
        const double phi = 2. * M_PI * i / CIRCLE_SEGMENTS;
        const Vec2d  p   = c + dir * (a * std::cos(phi)) + perp * (b * std::sin(phi));
        poly.points.emplace_back(scaled(p.x()), scaled(p.y()));
    }
    return poly;
}

static Polygon circle(const Vec2d &c, double r) { return ellipse(c, Vec2d(1., 0.), r, r); }

// The half plane on the part's side of the line through `pillar` across `dir`, as a big square.
static Polygon inner_half_plane(const Vec2d &pillar, const Vec2d &dir, double size)
{
    const Vec2d perp(-dir.y(), dir.x());
    const Vec2d a = pillar + perp * size, b = pillar - perp * size;
    const Vec2d c = b - dir * (2. * size), d = a - dir * (2. * size);
    Polygon poly;
    for (const Vec2d &p : { a, d, c, b })
        poly.points.emplace_back(scaled(p.x()), scaled(p.y()));
    if (poly.is_clockwise())
        poly.reverse();
    return poly;
}

// True when a disc of `radius` around `c` (mm) overlaps any of the islands.
static bool disc_hits(const ExPolygons &islands, const Vec2d &c, double radius)
{
    const Point  pc(scaled(c.x()), scaled(c.y()));
    const double r = scaled<double>(radius);
    for (const ExPolygon &island : islands) {
        BoundingBox bb = get_extents(island.contour);
        bb.offset(coord_t(r) + 1);
        if (!bb.contains(pc))
            continue;
        if (island.contains(pc))
            return true;
        auto close_to = [&pc, r](const Polygon &poly) {
            for (const Line &l : poly.lines())
                if (l.distance_to(pc) < r)
                    return true;
            return false;
        };
        if (close_to(island.contour))
            return true;
        for (const Polygon &h : island.holes)
            if (close_to(h))
                return true;
    }
    return false;
}

static double distance_to(const ExPolygons &islands, const Vec2d &c)
{
    const Point pc(scaled(c.x()), scaled(c.y()));
    double best = std::numeric_limits<double>::max();
    for (const ExPolygon &island : islands) {
        if (island.contains(pc))
            return 0.;
        for (const Line &l : island.contour.lines())
            best = std::min(best, l.distance_to(pc));
        for (const Polygon &h : island.holes)
            for (const Line &l : h.lines())
                best = std::min(best, l.distance_to(pc));
    }
    return unscaled(best);
}

static const ExPolygons &islands_at(const std::vector<LayerOutline> &layers, size_t i)
{
    static const ExPolygons none;
    return layers[i].islands != nullptr ? *layers[i].islands : none;
}

// --- v2 pillar geometry ---------------------------------------------------------------------------

// How much wider than at its top a tapered pillar of height `top` is at height z (0 when not tapered).
static double taper_growth(const StabilizerSettings &st, double z, double top)
{
    if (! st.tapered() || top <= EPSILON)
        return 0.;
    return (st.pillar_base_radius - st.pillar_radius) * std::clamp(1. - z / top, 0., 1.);
}

double pillar_foot_at(const StabilizerSettings &st, double z)
{
    // A small foot on the bed: the pillar widens by this much over the same height, at 45 degrees.
    const double foot = std::min(1.0, st.pillar_radius);
    return std::max(0., foot - z);
}

double pillar_growth_at(const Pillar &pillar, const StabilizerSettings &st, double z)
{
    return taper_growth(st, z, pillar.top_z) + pillar_foot_at(st, z);
}

double pillar_radius_at(const Pillar &pillar, const StabilizerSettings &st, double z)
{
    return st.pillar_radius + taper_growth(st, z, pillar.top_z);
}

static ColumnFrame column_frame_at(const Vec2d &pos, const Vec2d &dir, const StabilizerSettings &st)
{
    const double r  = st.pillar_radius;
    const double hu = 0.5 * std::max(st.column_width, 2. * r);
    const double hv = 0.5 * std::max(st.column_length, 2. * r);
    const double f  = std::min(1.0, 0.5 * std::min(hu, hv));
    ColumnFrame out;
    out.along  = dir.norm() > EPSILON ? Vec2d(dir.normalized()) : Vec2d(1., 0.);
    // The side facing the part where a round pillar's is: the column grows outwards.
    out.centre = pos + out.along * (hu - r);
    out.core_u = hu - f;
    out.core_v = hv - f;
    out.fillet = f;
    return out;
}

ColumnFrame column_frame(const Pillar &pillar, const StabilizerSettings &st) { return column_frame_at(pillar.pos, pillar.dir, st); }

std::vector<Vec2d> column_outline(const ColumnFrame &f, double grow)
{
    std::vector<Vec2d> out;
    out.reserve(4 * (COLUMN_CORNER_SEGMENTS + 1));
    const Vec2d  perp(-f.along.y(), f.along.x());
    const double rho = f.fillet + grow;
    static const double su[4] = { 1., -1., -1., 1. }, sv[4] = { 1., 1., -1., -1. };
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k <= COLUMN_CORNER_SEGMENTS; ++k) {
            const double th = 0.5 * M_PI * (c + double(k) / COLUMN_CORNER_SEGMENTS);
            out.emplace_back(f.centre + f.along * (su[c] * f.core_u + rho * std::cos(th)) + perp * (sv[c] * f.core_v + rho * std::sin(th)));
        }
    return out;
}

Polygon pillar_section(const Pillar &pillar, const StabilizerSettings &st, double z)
{
    const double grow = pillar_growth_at(pillar, st, z);
    if (! pillar.column)
        return circle(pillar.pos, st.pillar_radius + grow);
    Polygon poly;
    for (const Vec2d &v : column_outline(column_frame(pillar, st), grow))
        poly.points.emplace_back(scaled(v.x()), scaled(v.y()));
    return poly;
}

// Distances in mm, on unscaled points.
static double point_segment_distance(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + ab * t - p).norm();
}

static double perp_dot(const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); }

static double segment_segment_distance(const Vec2d &p1, const Vec2d &q1, const Vec2d &p2, const Vec2d &q2)
{
    const Vec2d  r = q1 - p1, s = q2 - p2;
    const double d1 = perp_dot(r, p2 - p1), d2 = perp_dot(r, q2 - p1), d3 = perp_dot(s, p1 - p2), d4 = perp_dot(s, q1 - p2);
    if (((d1 > 0. && d2 < 0.) || (d1 < 0. && d2 > 0.)) && ((d3 > 0. && d4 < 0.) || (d3 < 0. && d4 > 0.)))
        return 0.;
    return std::min({ point_segment_distance(p1, p2, q2), point_segment_distance(q1, p2, q2), point_segment_distance(p2, p1, q1),
                      point_segment_distance(q2, p1, q1) });
}

// Distance from the segment pq to the islands, mm: 0 where it touches or lies inside one. Only
// islands within `limit` are looked at; farther than that it returns `limit`.
static double segment_distance(const ExPolygons &islands, const Vec2d &p, const Vec2d &q, double limit)
{
    double      best = limit;
    BoundingBox sbb;
    sbb.merge(Point(scaled(p.x()), scaled(p.y())));
    sbb.merge(Point(scaled(q.x()), scaled(q.y())));
    sbb.offset(scaled<double>(limit) + 1);
    for (const ExPolygon &island : islands) {
        if (! sbb.overlap(get_extents(island.contour)))
            continue;
        if (island.contains(Point(scaled(p.x()), scaled(p.y()))))
            return 0.;
        auto edges = [&best, &p, &q](const Polygon &poly) {
            const size_t n = poly.size();
            for (size_t i = 0; i < n && best > 0.; ++i)
                best = std::min(best, segment_segment_distance(p, q, unscaled(poly[i]), unscaled(poly[(i + 1) % n])));
        };
        edges(island.contour);
        for (const Polygon &h : island.holes)
            edges(h);
        if (best <= 0.)
            return 0.;
    }
    return best;
}

// Distance from a column's core rectangle to the islands, mm (0 inside), up to `limit`.
static double core_distance(const ExPolygons &islands, const ColumnFrame &f, double limit)
{
    const Vec2d perp(-f.along.y(), f.along.x());
    const Vec2d c[4] = { f.centre + f.along * f.core_u + perp * f.core_v, f.centre - f.along * f.core_u + perp * f.core_v,
                         f.centre - f.along * f.core_u - perp * f.core_v, f.centre + f.along * f.core_u - perp * f.core_v };
    double best = limit;
    for (int i = 0; i < 4 && best > 0.; ++i)
        best = std::min(best, segment_distance(islands, c[i], c[(i + 1) % 4], best));
    if (best > 0.)
        // An island wholly inside the core (it never is: the column stands off the part).
        for (const ExPolygon &island : islands) {
            const Vec2d v = unscaled(island.contour.points.front()) - f.centre;
            if (std::abs(v.dot(f.along)) < f.core_u && std::abs(v.dot(perp)) < f.core_v)
                return 0.;
        }
    return best;
}

// True when a pillar at `pos`, its struts running out along `dir`, comes closer than the clearance (less a
// little slack: on a round part it is met exactly) to the islands at height z. A tapered pillar is
// checked as if it reached `taper_top`: every pillar is at most as tall as the part, so checking the
// part's height keeps any later, taller pillar on that spot clear too.
static bool pillar_hits(const ExPolygons &islands, const StabilizerSettings &st, const Vec2d &pos, const Vec2d &dir, bool column,
                        double z, double taper_top)
{
    const double grow = taper_growth(st, z, taper_top) + st.clearance - 0.1;
    if (! column)
        return disc_hits(islands, pos, st.pillar_radius + grow);
    const ColumnFrame f = column_frame_at(pos, dir, st);
    return core_distance(islands, f, f.fillet + grow + 1.) < f.fillet + grow;
}

// The shortest strut from contact `c` out along `dir` whose pillar has a clear way down to the bed,
// and whose own path from the pillar up to the tip stays off the part. False when none fits within
// the settings' max_run.
static bool fit_strut(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const Contact &c,
                      const Vec2d &dir, Strut &out)
{
    const double pillar_r  = st.pillar_radius;
    const double clearance = st.clearance;
    // Every pillar a column (a forced shape: Auto decides after planning, and checks then).
    const bool   column    = st.column_shape == scsRoundedRect;
    const double taper_top = layers.back().slice_z;
    // The strut has to reach out of its pillar towards the wall and still end the tip gap short of it,
    // so with a large gap the pillar stands further out (a gap up to half a millimetre under the
    // clearance changes nothing).
    const double min_run = pillar_r + std::max(clearance, st.tip_gap + 0.5);
    for (double run = min_run; run <= st.max_run + EPSILON; run += 0.5) {
        Strut s;
        s.tip       = c.pos;
        s.dir       = dir;
        s.tip_layer = c.layer;
        s.tip_z     = c.z;
        s.run       = run;
        s.normal    = c.normal;
        const Vec2d  pillar = s.pillar();
        const double top_z  = s.junction_z();
        bool ok = true;
        for (size_t i = 0; ok && i <= c.layer; ++i) {
            const double z = layers[i].slice_z;
            if (z <= top_z + EPSILON) {
                // (A little slack: on a round part the clearance is exactly met.)
                ok = ! pillar_hits(islands_at(layers, i), st, pillar, dir, column, z, taper_top);
            } else {
                // Along the strut: its axis must stay outside the part and move away from the
                // wall at least half as fast as it drops - true of any wall that does not lean
                // out over the strut.
                const double dz = c.z - z;
                if (dz > st.rings.tip_diameter)
                    ok = distance_to(islands_at(layers, i), s.axis_at(z)) >= 0.5 * dz;
            }
        }
        if (ok) {
            out = s;
            return true;
        }
    }
    return false;
}

std::vector<Strut> plan_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &st,
                               const std::vector<PaintedSpot> &painted, PlanReport *report)
{
    std::vector<Strut> out;
    if (report != nullptr)
        *report = PlanReport();
    if (layers.size() < 2)
        return out;

    if (st.ring_struts)
        for (const Contact &c : ring_contacts(layers, st.rings)) {
            Strut s;
            if (fit_strut(layers, st, c, c.dir, s))
                out.push_back(s);
        }
    const size_t n_ring = out.size();

    // Painted points: on top of the rings, ignoring their spacing, count and island width limit,
    // but held to the same printability rules - and, as they are the user's explicit ask, never
    // silently dropped.
    PlanReport rep;
    static const std::vector<PaintedSpot> no_spots;
    const std::vector<PaintedSpot> &spots = st.painted_points ? painted : no_spots;
    rep.painted              = spots.size();
    rep.manual_without_paint = ! st.ring_struts && st.painted_points && spots.empty();
    const double same_spot = std::max(1.0, st.rings.tip_diameter);
    for (const PaintedSpot &spot : spots) {
        Contact c;
        if (! painted_contact(layers, spot, c)) {
            rep.unreachable.push_back(spot.pos);
            continue;
        }
        // A strut already touching there (a ring's, or an earlier painted spot's) serves it.
        auto touches = [&c, same_spot](const Strut &s) {
            return std::abs(s.tip_z - double(c.z)) < same_spot && (s.tip - c.pos).norm() < same_spot;
        };
        if (std::any_of(out.begin(), out.begin() + n_ring, touches)) {
            ++rep.painted_on_ring;
            continue;
        }
        if (std::any_of(out.begin() + n_ring, out.end(), touches)) {
            ++rep.painted_placed;
            continue;
        }
        // Straight out from the painted surface first, then swung further and further to either side
        // until a strut fits past whatever is in the way.
        bool placed = false;
        for (double deg : { 0., 15., -15., 30., -30., 45., -45., 60., -60. }) {
            const double a = deg * M_PI / 180.;
            const Vec2d  dir(c.dir.x() * std::cos(a) - c.dir.y() * std::sin(a), c.dir.x() * std::sin(a) + c.dir.y() * std::cos(a));
            Strut s;
            // A painted point near the bed: the pillar must still stand on the bed, under the strut.
            if (fit_strut(layers, st, c, dir, s) && s.junction_z() >= 0.) {
                s.painted = true;
                out.push_back(s);
                placed = true;
                break;
            }
        }
        if (placed)
            ++rep.painted_placed;
        else
            rep.unreachable.push_back(Vec3d(c.pos.x(), c.pos.y(), double(c.z)));
    }
    if (report != nullptr)
        *report = std::move(rep);
    return out;
}

std::vector<Strut> plan_struts(const PrintObject &object, PlanReport *report)
{
    if (object.layers().size() < 2) {
        if (report != nullptr)
            *report = PlanReport();
        return {};
    }
    return plan_struts(outlines_of(object), settings_of(object), painted_spots(object), report);
}

Strut gapped(const Strut &s, double gap)
{
    // Never past the pillar's axis: some strut has to remain (the planner keeps run >= gap + 0.5 plus
    // the pillar radius, so this only guards odd inputs).
    const double g = std::clamp(gap, 0., std::max(0., s.run - 0.1));
    Strut out = s;
    out.tip   = s.tip + s.dir * g;
    out.tip_z = s.tip_z - g;
    out.run   = s.run - g;
    return out;
}

double pillar_radius(const PrintObject &object)
{
    return settings_of(object).pillar_radius;
}

// --- v2 planning: pillars, braces, columns -----------------------------------------------------

Plan plan_from_struts(const StabilizerSettings &st, const std::vector<Strut> &struts)
{
    Plan plan;
    plan.struts = struts;
    plan.strut_pillar.reserve(struts.size());
    // Struts of different rings come down onto the same pillar: one pillar up to the highest junction.
    // The rings' contacts are found on each ring's own layer outline, so the same pillar comes out a few
    // microns apart from ring to ring; anything within PILLAR_MERGE_DISTANCE is that one pillar.
    for (const Strut &s : struts) {
        const Vec2d p     = s.pillar();
        size_t      found = plan.pillars.size();
        for (size_t k = 0; k < plan.pillars.size() && found == plan.pillars.size(); ++k)
            if ((plan.pillars[k].pos - p).squaredNorm() < sqr(PILLAR_MERGE_DISTANCE))
                found = k;
        if (found == plan.pillars.size()) {
            Pillar pillar;
            pillar.pos    = p;
            pillar.dir    = s.dir;
            pillar.top_z  = s.junction_z();
            pillar.column = st.column_shape == scsRoundedRect;
            plan.pillars.push_back(pillar);
        } else
            plan.pillars[found].top_z = std::max(plan.pillars[found].top_z, s.junction_z());
        plan.strut_pillar.push_back(found);
    }
    return plan;
}

std::vector<double> pillar_ties(const Plan &plan, size_t i)
{
    const Pillar       &pillar = plan.pillars[i];
    std::vector<double> ties{ 0. };
    for (size_t k = 0; k < plan.struts.size() && k < plan.strut_pillar.size(); ++k)
        if (plan.strut_pillar[k] == i)
            ties.push_back(plan.struts[k].junction_z());
    for (const Brace &b : plan.braces) {
        if (b.lower == i)
            ties.push_back(b.z_low);
        if (b.upper == i)
            ties.push_back(b.z_high());
    }
    for (double &t : ties)
        t = std::clamp(t, 0., std::max(0., pillar.top_z));
    std::sort(ties.begin(), ties.end());
    ties.erase(std::unique(ties.begin(), ties.end(), [](double a, double b) { return std::abs(a - b) < EPSILON; }), ties.end());
    return ties;
}

static double longest_gap(const std::vector<double> &ties)
{
    double out = 0.;
    for (size_t k = 1; k < ties.size(); ++k)
        out = std::max(out, ties[k] - ties[k - 1]);
    return out;
}

double longest_unbraced(const Plan &plan, size_t pillar) { return longest_gap(pillar_ties(plan, pillar)); }

// The layers whose slicing planes lie in [z0, z1].
static std::pair<size_t, size_t> layer_range(const std::vector<LayerOutline> &layers, double z0, double z1)
{
    auto lo = std::lower_bound(layers.begin(), layers.end(), float(z0), [](const LayerOutline &l, float z) { return l.slice_z < z; });
    auto hi = std::upper_bound(layers.begin(), layers.end(), float(z1), [](float z, const LayerOutline &l) { return z < l.slice_z; });
    return { size_t(lo - layers.begin()), size_t(hi - layers.begin()) };
}

// A brace stays the clearance (less the same slack as the pillars) off the part at every layer it
// prints on. Its section at a layer (an ellipse cut at the two pillar axes) lies within the brace's
// radius of a short piece of its axis, so the check is that piece's distance to the part.
static bool brace_clear(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const Brace &b)
{
    const double need = b.radius + st.clearance - 0.1;
    const double half = (M_SQRT2 - 1.) * b.radius;
    const double span = b.span();
    const Vec2d  dir  = b.dir();
    const auto [i0, i1] = layer_range(layers, b.z_bottom(), b.z_top());
    for (size_t i = i0; i < i1; ++i) {
        const ExPolygons &islands = islands_at(layers, i);
        if (islands.empty())
            continue;
        const double u = layers[i].slice_z - b.z_low;
        const Vec2d  p = b.from + dir * std::clamp(u - half, 0., span);
        const Vec2d  q = b.from + dir * std::clamp(u + half, 0., span);
        if (segment_distance(islands, p, q, need) < need)
            return false;
    }
    return true;
}

// Bracing: while some pillar stands unbraced for longer than max_unbraced, tie it to a neighbour with a
// 45 degree diagonal - up from it to the neighbour, or up from the neighbour to it - at the top of the
// first max_unbraced of that stretch, or lower if that one would cross the part. One diagonal per bay,
// alternating as the stretches above get braced in turn: a zigzag. An X (two diagonals crossing in one
// bay) prints too, but doubles the material and the blob where they cross, and a zigzag already makes
// every bay a rigid triangle.
static void place_braces(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, Plan &plan)
{
    const size_t n      = plan.pillars.size();
    const double rb     = st.pillar_radius;
    const double L      = st.max_unbraced;
    // The brace's lower end, which reaches sqrt(2) radii below its axis, stays off the bed and the foot.
    const double z_min  = M_SQRT2 * rb + 0.5;
    const double step   = 0.5;

    // Neighbours within the span, nearest first. Pillars closer than two radii and a millimetre are
    // practically one and need no brace.
    std::vector<std::vector<std::pair<double, size_t>>> nb(n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const double dist = (plan.pillars[i].pos - plan.pillars[j].pos).norm();
            if (dist > st.max_brace_span + EPSILON || dist < 2. * rb + 1.)
                continue;
            if (plan.pillars[i].top_z <= z_min || plan.pillars[j].top_z <= z_min)
                continue;
            nb[i].emplace_back(dist, j);
            nb[j].emplace_back(dist, i);
        }
    for (auto &v : nb)
        std::sort(v.begin(), v.end());

    std::vector<std::vector<double>> ties(n);
    for (size_t i = 0; i < n; ++i)
        ties[i] = pillar_ties(plan, i);

    auto overlaps_pair = [&plan](const Brace &c) {
        for (const Brace &b : plan.braces)
            if (std::min(b.lower, b.upper) == std::min(c.lower, c.upper) && std::max(b.lower, b.upper) == std::max(c.lower, c.upper) &&
                c.z_low < b.z_high() - EPSILON && b.z_low < c.z_high() - EPSILON)
                return true;
        return false;
    };
    auto make = [&plan, rb](size_t lower, size_t upper, double z_low) {
        Brace b;
        b.lower  = lower;
        b.upper  = upper;
        b.from   = plan.pillars[lower].pos;
        b.to     = plan.pillars[upper].pos;
        b.z_low  = z_low;
        b.radius = rb;
        return b;
    };
    auto add_tie = [](std::vector<double> &t, double z) {
        t.insert(std::upper_bound(t.begin(), t.end(), z), z);
    };

    std::set<std::tuple<size_t, long long, long long>> failed;
    const size_t max_braces = 2000;
    while (plan.braces.size() < max_braces) {
        // The lowest stretch longer than the limit, on any pillar that has a neighbour.
        size_t best_i = n;
        double best_a = 0., best_b = 0.;
        for (size_t i = 0; i < n; ++i) {
            if (nb[i].empty())
                continue;
            for (size_t k = 1; k < ties[i].size(); ++k) {
                const double a = ties[i][k - 1], b = ties[i][k];
                if (b - a <= L + EPSILON || failed.count({ i, std::llround(a * 1000.), std::llround(b * 1000.) }))
                    continue;
                if (best_i == n || a < best_a - EPSILON) {
                    best_i = i;
                    best_a = a;
                    best_b = b;
                }
                break;
            }
        }
        if (best_i == n)
            break;

        const size_t i      = best_i;
        bool         placed = false;
        // Up from this pillar to a neighbour first, anywhere in the stretch: its high end lands higher up
        // the neighbour, where that one usually needs a tie too, so a ring of pillars gets a ring of
        // braces. Only then up from a neighbour to this pillar, which ties the neighbour lower down.
        for (int option = 0; option < 2 && ! placed; ++option)
            for (double z = std::min(best_a + L, best_b - step); ! placed && z >= best_a + step - EPSILON; z -= step)
                for (const auto &[dist, j] : nb[i]) {
                    const double top_j = plan.pillars[j].top_z;
                    const bool   fits  = option == 0 ? z >= z_min && z + dist <= top_j + EPSILON :
                                                       z - dist >= z_min && z - dist <= top_j + EPSILON;
                    if (! fits)
                        continue;
                    const Brace cand = option == 0 ? make(i, j, z) : make(j, i, z - dist);
                    if (! overlaps_pair(cand) && brace_clear(layers, st, cand)) {
                        plan.braces.push_back(cand);
                        add_tie(ties[cand.lower], cand.z_low);
                        add_tie(ties[cand.upper], cand.z_high());
                        placed = true;
                        break;
                    }
                }
        if (! placed)
            failed.insert({ i, std::llround(best_a * 1000.), std::llround(best_b * 1000.) });
    }
}

// Auto columns: a pillar at least column_min_height tall that stands alone (no other pillar within the
// bracing span) or is still unbraced for longer than max_unbraced becomes a rounded-rectangle column,
// if the column stays the clearance off the part all the way down; otherwise it stays round.
static void auto_columns(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, Plan &plan, PlanReport &rep)
{
    for (size_t i = 0; i < plan.pillars.size(); ++i) {
        Pillar &p = plan.pillars[i];
        if (p.column || p.top_z < st.column_min_height - EPSILON || p.top_z <= 0.)
            continue;
        bool single = true;
        for (size_t j = 0; j < plan.pillars.size() && single; ++j)
            if (j != i && (plan.pillars[j].pos - p.pos).norm() <= st.max_brace_span + EPSILON)
                single = false;
        if (! single && longest_unbraced(plan, i) <= st.max_unbraced + EPSILON)
            continue;
        const ColumnFrame f   = column_frame(p, st);
        const auto [i0, i1]   = layer_range(layers, -1., p.top_z + EPSILON);
        bool              clear = true;
        for (size_t k = i0; k < i1 && clear; ++k) {
            const double grow = taper_growth(st, layers[k].slice_z, p.top_z) + st.clearance - 0.1;
            clear = core_distance(islands_at(layers, k), f, f.fillet + grow + 1.) >= f.fillet + grow;
        }
        if (clear)
            p.column = true;
        else
            ++rep.column_fallbacks;
    }
}

Plan complete_plan(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const std::vector<Strut> &struts,
                   PlanReport *report)
{
    Plan       plan = plan_from_struts(st, struts);
    PlanReport rep;
    if (! layers.empty()) {
        if (st.bracing)
            place_braces(layers, st, plan);
        if (st.column_shape == scsAuto)
            auto_columns(layers, st, plan, rep);
    }
    rep.braces = plan.braces.size();
    for (size_t i = 0; i < plan.pillars.size(); ++i) {
        if (plan.pillars[i].column)
            ++rep.columns;
        if (st.bracing && plan.pillars[i].top_z > 0. && longest_unbraced(plan, i) > st.max_unbraced + EPSILON)
            ++rep.unbraced_pillars;
    }
    if (report != nullptr) {
        report->braces           = rep.braces;
        report->unbraced_pillars = rep.unbraced_pillars;
        report->columns          = rep.columns;
        report->column_fallbacks = rep.column_fallbacks;
    }
    return plan;
}

Plan plan_stabilizers(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const std::vector<PaintedSpot> &painted,
                      PlanReport *report)
{
    const std::vector<Strut> struts = plan_struts(layers, st, painted, report);
    return complete_plan(layers, st, struts, report);
}

Plan plan_stabilizers(const PrintObject &object, PlanReport *report)
{
    if (object.layers().size() < 2) {
        if (report != nullptr)
            *report = PlanReport();
        return {};
    }
    return plan_stabilizers(outlines_of(object), settings_of(object), painted_spots(object), report);
}

// The band between the vertical planes through `from` and `from + dir * span`, as a wide rectangle.
static Polygon strip(const Vec2d &from, const Vec2d &dir, double span, double half_width)
{
    const Vec2d perp(-dir.y(), dir.x());
    Polygon     poly;
    for (const Vec2d &p : { Vec2d(from - perp * half_width), Vec2d(from + dir * span - perp * half_width),
                            Vec2d(from + dir * span + perp * half_width), Vec2d(from + perp * half_width) })
        poly.points.emplace_back(scaled(p.x()), scaled(p.y()));
    if (poly.is_clockwise())
        poly.reverse();
    return poly;
}

std::vector<ExPolygons> slice_plan(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const Plan &plan,
                                   const std::function<void()> &throw_if_canceled)
{
    std::vector<ExPolygons> out(layers.size());
    if (plan.struts.empty() && plan.pillars.empty())
        return out;

    const double tip_r    = 0.5 * st.rings.tip_diameter;
    const double pillar_r = st.pillar_radius;
    const float  tip_gap  = scaled<float>(st.tip_gap);

    for (size_t i = 0; i < layers.size(); ++i) {
        if (throw_if_canceled)
            throw_if_canceled();
        const double z = layers[i].slice_z;
        // Pillars (with their feet) and braces apart from the struts: the tip gap is the struts' business only.
        Polygons     pillars, polys;
        for (const Pillar &p : plan.pillars)
            if (z <= p.top_z + EPSILON)
                pillars.push_back(pillar_section(p, st, z));
        // Braces: the rod's ellipse at this height, cut at the two pillars' axes.
        for (const Brace &b : plan.braces)
            if (z >= b.z_bottom() - EPSILON && z <= b.z_top() + EPSILON)
                append(pillars, intersection(Polygons{ ellipse(b.axis_at(z), b.dir(), b.radius * M_SQRT2, b.radius) },
                                             Polygons{ strip(b.from, b.dir(), b.span(), 4. * b.radius) }));
        for (const Strut &strut : plan.struts) {
            // Built set back by the tip gap, so it tapers to its tip at the trimmed end.
            const Strut s = gapped(strut, st.tip_gap);
            if (z > s.tip_z + EPSILON)
                continue;
            const Vec2d  pillar = s.pillar();
            const double top_z  = s.junction_z();
            // The strut. Its slice at 45 degrees is an ellipse, sqrt(2) longer along the strut than
            // across it. It continues below the pillar's top until its lower side comes out of the
            // pillar's side, and the part of it beyond the pillar's axis is cut off, so where it
            // leaves the pillar it grows by one layer height per layer and no more.
            if (z >= top_z - pillar_r) {
                const double dz = s.tip_z - z;
                const double t  = std::clamp(dz / s.run, 0., 1.);
                const double r  = tip_r + (pillar_r - tip_r) * t;
                append(polys, intersection(Polygons{ ellipse(s.axis_at(z), s.dir, r * M_SQRT2, r) },
                                           Polygons{ inner_half_plane(pillar, s.dir, 4. * (s.run + pillar_r)) }));
            }
        }
        if (polys.empty() && pillars.empty())
            continue;
        // Touch, don't fuse: clip the struts at the part's outline, so the tip's footprint ends exactly
        // where the outer wall begins - or stop short of it by the tip gap. The pillars, their feet and the
        // braces keep the planner's own clearance whatever the gap, and are only ever kept out of the
        // part: the gap used to clip them too, which at a gap above the clearance ate the feet and the
        // pillars.
        const ExPolygons &part = islands_at(layers, i);
        ExPolygons layer = diff_ex(union_(pillars), part);
        if (! polys.empty())
            append(layer, tip_gap > 0.f ? diff_ex(union_(polys), offset_ex(part, tip_gap)) : diff_ex(union_(polys), part));
        out[i] = union_ex(layer);
    }
    return out;
}

std::vector<ExPolygons> slice_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &st,
                                     const std::vector<Strut> &struts, const std::function<void()> &throw_if_canceled)
{
    if (struts.empty())
        return std::vector<ExPolygons>(layers.size());
    return slice_plan(layers, st, complete_plan(layers, st, struts), throw_if_canceled);
}

std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled)
{
    return slice_struts(outlines_of(object), settings_of(object), struts, throw_if_canceled);
}

std::vector<ExPolygons> sparse_infill_areas(const std::vector<ExPolygons> &slices, const StabilizerSettings &st, double line_width,
                                            double spacing)
{
    std::vector<ExPolygons> out(slices.size());
    if (st.wall_loops <= 0 || st.infill_density >= 0.999 || slices.size() < 2)
        return out;
    // Inside the walls: the last wall's inner edge.
    const float inset = scaled<float>(line_width + (st.wall_loops - 1) * spacing);
    // Too narrow for infill: a region that sparse lines this far apart (or three lines) would hardly
    // cross stays solid - thin pillars, struts and tips.
    const float narrow = scaled<float>(0.5 * std::max(3. * spacing, spacing / std::max(0.01, st.infill_density)));
    std::vector<ExPolygons> wide(slices.size());
    for (size_t i = 0; i < slices.size(); ++i) {
        if (slices[i].empty())
            continue;
        for (ExPolygon &e : offset_ex(slices[i], -inset))
            if (! offset(e, -narrow).empty())
                wide[i].emplace_back(std::move(e));
    }
    // Solid shells: sparse infill only where the layers below and above are sparse too, so it never
    // ends in the air nor carries a strut or a pillar's end on bare infill lines.
    for (size_t i = STABILIZER_BOTTOM_SOLID_LAYERS; i + STABILIZER_TOP_SOLID_LAYERS < slices.size(); ++i) {
        if (wide[i].empty())
            continue;
        ExPolygons a = wide[i];
        for (size_t k = i - STABILIZER_BOTTOM_SOLID_LAYERS; k <= i + STABILIZER_TOP_SOLID_LAYERS && ! a.empty(); ++k)
            if (k != i)
                a = intersection_ex(a, wide[k]);
        out[i] = std::move(a);
    }
    return out;
}

// The support layer at `layer`'s print_z, or null when the support generators made none.
static SupportLayer *support_layer_find(PrintObject &object, const Layer &layer)
{
    SupportLayerPtrs &sls = object.support_layers();
    auto it = std::lower_bound(sls.begin(), sls.end(), layer.print_z - EPSILON,
                               [](const SupportLayer *l, double z) { return l->print_z < z; });
    return it != sls.end() && std::abs((*it)->print_z - layer.print_z) < EPSILON ? *it : nullptr;
}

// The support layer at `layer`'s print_z, inserting one when the support generators made none.
static SupportLayer *support_layer_at(PrintObject &object, const Layer &layer, bool &inserted)
{
    if (SupportLayer *sl = support_layer_find(object, layer))
        return sl;
    SupportLayerPtrs &sls = object.support_layers();
    auto it = std::lower_bound(sls.begin(), sls.end(), layer.print_z - EPSILON,
                               [](const SupportLayer *l, double z) { return l->print_z < z; });
    it = object.insert_support_layer(it, 0, 0, layer.height, layer.print_z, layer.slice_z);
    (*it)->support_type = stInnerTree;
    inserted = true;
    return *it;
}

} // namespace stabilizers

stabilizers::PlanReport generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled)
{
    stabilizers::PlanReport report;
    const PrintObjectConfig &cfg = object.config();
    if (cfg.stabilizer_supports.value == smOff || object.layers().size() < 2)
        return report;

    const stabilizers::Plan plan = stabilizers::plan_stabilizers(object, &report);
    BOOST_LOG_TRIVIAL(debug) << "Stabilizers: " << plan.struts.size() << " struts, " << plan.pillars.size() << " pillars, "
                             << plan.braces.size() << " braces, " << report.columns << " columns, " << report.unbraced_pillars
                             << " pillar(s) left long-unbraced, " << report.painted << " painted point(s), "
                             << report.unreachable.size() << " unreachable";
    if (plan.struts.empty())
        return report;
    throw_if_canceled();

    const std::vector<stabilizers::LayerOutline> outlines = stabilizers::outlines_of(object);
    const stabilizers::StabilizerSettings        st       = stabilizers::settings_of(object);
    std::vector<ExPolygons> slices = stabilizers::slice_plan(outlines, st, plan, throw_if_canceled);

    // Walls and sparse infill (stabilizer_wall_loops > 0): where the bodies are wide enough, and not on
    // their first and last few layers.
    const Flow              base_flow    = support_material_flow(&object);
    const std::vector<ExPolygons> sparse = stabilizers::sparse_infill_areas(slices, st, base_flow.width(), base_flow.spacing());
    std::unique_ptr<Fill>   filler;
    if (std::any_of(sparse.begin(), sparse.end(), [](const ExPolygons &e) { return ! e.empty(); })) {
        filler.reset(Fill::new_from_type(st.infill_pattern));
        BoundingBox bbox;
        for (const ExPolygons &s : slices)
            for (const ExPolygon &e : s)
                bbox.merge(get_extents(e.contour));
        filler->set_bounding_box(bbox);
    }

    // Specks left by clipping at the wall, too small to print a line into.
    const double min_island_area = sqr(scaled<double>(0.2));

    bool inserted = false;
    for (size_t i = 0; i < object.layers().size() && i < slices.size(); ++i) {
        const Layer &layer = *object.layers()[i];
        ExPolygons &islands = slices[i];
        if (islands.empty())
            continue;

        const Flow flow = i == 0 ? support_material_1st_layer_flow(&object, float(layer.height)) :
                                   support_material_flow(&object, float(layer.height));
        const float half_w  = 0.5f * float(flow.scaled_width());
        const float spacing = float(flow.scaled_spacing());

        // Where the regular supports already print, don't print a second time.
        if (const SupportLayer *existing = stabilizers::support_layer_find(object, layer); existing && !existing->support_islands.empty())
            islands = diff_ex(islands, existing->support_islands);

        auto *coll = new ExtrusionEntityCollection();
        ExPolygons printed;
        for (ExPolygon &island : islands) {
            if (island.area() < min_island_area)
                continue;
            // Solid concentric loops, outside in: a pillar a few lines wide is all wall. An island
            // thinner than one line (the very tip) is traced along its own outline instead, which
            // is what makes the contact.
            Polygons loop = offset(island, -half_w);
            if (loop.empty())
                loop = to_polygons(island);
            // With walls: the region inside them that is sparse on this layer.
            ExPolygons sparse_here, inner;
            if (filler && i < sparse.size() && ! sparse[i].empty()) {
                inner       = offset_ex(island, -(2.f * half_w + float(st.wall_loops - 1) * spacing));
                sparse_here = intersection_ex(inner, sparse[i]);
                sparse_here.erase(std::remove_if(sparse_here.begin(), sparse_here.end(),
                                                 [spacing](const ExPolygon &e) { return e.area() < 4. * double(spacing) * double(spacing); }),
                                  sparse_here.end());
            }
            if (sparse_here.empty()) {
                while (!loop.empty()) {
                    Polygons next = offset(loop, -spacing);
                    extrusion_entities_append_loops(coll->entities, std::move(loop), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                    loop = std::move(next);
                }
            } else {
                // The walls...
                for (int w = 0; w < st.wall_loops && ! loop.empty(); ++w) {
                    Polygons next = offset(loop, -spacing);
                    extrusion_entities_append_loops(coll->entities, std::move(loop), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                    loop = std::move(next);
                }
                // ...what is inside them but not sparse here (too narrow, or a solid shell layer) solid...
                Polygons rest = offset(diff_ex(inner, sparse_here), -half_w);
                while (! rest.empty()) {
                    Polygons next = offset(rest, -spacing);
                    extrusion_entities_append_loops(coll->entities, std::move(rest), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                    rest = std::move(next);
                }
                // ...and sparse infill in the rest.
                filler->layer_id = i;
                filler->z        = layer.print_z;
                filler->spacing  = flow.spacing();
                filler->angle    = float(M_PI / 4. + (st.infill_pattern == ipRectilinear && (i % 2) == 1 ? M_PI / 2. : 0.));
                FillParams params;
                params.density     = float(st.infill_density);
                params.dont_adjust = true;
                for (ExPolygon &e : sparse_here) {
                    Surface   surface(stInternal, std::move(e));
                    Polylines lines;
                    try {
                        lines = filler->fill_surface(&surface, params);
                    } catch (InfillFailedException &) {
                    }
                    if (! lines.empty())
                        extrusion_entities_append_paths(coll->entities, std::move(lines), erSupportMaterial, flow.mm3_per_mm(), flow.width(),
                                                        flow.height());
                }
            }
            printed.emplace_back(std::move(island));
        }
        if (coll->entities.empty()) {
            delete coll;
            continue;
        }

        SupportLayer *sl = stabilizers::support_layer_at(object, layer, inserted);
        sl->support_fills.entities.push_back(coll);
        expolygons_append(sl->support_islands, printed);
        if (sl->support_type == stInnerTree)
            expolygons_append(sl->lslices, printed);
    }

    if (inserted) {
        // Keep ids equal to positions and the neighbour links intact after the insertions.
        SupportLayerPtrs &sls = object.support_layers();
        for (size_t i = 0; i < sls.size(); ++i) {
            sls[i]->set_id(i);
            sls[i]->lower_layer = i > 0 ? sls[i - 1] : nullptr;
            sls[i]->upper_layer = i + 1 < sls.size() ? sls[i + 1] : nullptr;
        }
    }
    return report;
}

} // namespace Slic3r
