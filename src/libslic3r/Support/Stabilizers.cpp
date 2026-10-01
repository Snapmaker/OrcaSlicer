#include "Stabilizers.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../TriangleSelector.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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

// The shortest strut from contact `c` out along `dir` whose pillar has a clear way down to the bed,
// and whose own path from the pillar up to the tip stays off the part. False when none fits within
// the settings' max_run.
static bool fit_strut(const std::vector<LayerOutline> &layers, const StabilizerSettings &st, const Contact &c,
                      const Vec2d &dir, Strut &out)
{
    const double pillar_r  = st.pillar_radius;
    const double clearance = st.clearance;
    for (double run = pillar_r + clearance; run <= st.max_run + EPSILON; run += 0.5) {
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
                ok = !disc_hits(islands_at(layers, i), pillar, pillar_r + clearance - 0.1);
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

double pillar_radius(const PrintObject &object)
{
    return settings_of(object).pillar_radius;
}

std::vector<ExPolygons> slice_struts(const std::vector<LayerOutline> &layers, const StabilizerSettings &st,
                                     const std::vector<Strut> &struts, const std::function<void()> &throw_if_canceled)
{
    std::vector<ExPolygons> out(layers.size());
    if (struts.empty())
        return out;

    const double tip_r    = 0.5 * st.rings.tip_diameter;
    const double pillar_r = st.pillar_radius;
    // A small foot on the bed: the pillar widens by this much over the same height, at 45 degrees.
    const double foot     = std::min(1.0, pillar_r);
    const float  tip_gap  = scaled<float>(st.tip_gap);

    for (size_t i = 0; i < layers.size(); ++i) {
        if (throw_if_canceled)
            throw_if_canceled();
        const double z = layers[i].slice_z;
        Polygons     polys;
        for (const Strut &s : struts) {
            if (z > s.tip_z + EPSILON)
                continue;
            const Vec2d  pillar = s.pillar();
            const double top_z  = s.junction_z();
            if (z <= top_z + EPSILON)
                polys.push_back(circle(pillar, pillar_r + std::max(0., foot - z)));
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
        if (polys.empty())
            continue;
        // Touch, don't fuse: clip at the part's outline, so the tip's footprint ends exactly where
        // the outer wall begins - or stop short of it by the tip gap.
        const ExPolygons &part = islands_at(layers, i);
        out[i] = tip_gap > 0.f ? diff_ex(union_(polys), offset_ex(part, tip_gap)) : diff_ex(union_(polys), part);
    }
    return out;
}

std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled)
{
    return slice_struts(outlines_of(object), settings_of(object), struts, throw_if_canceled);
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

    const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(object, &report);
    BOOST_LOG_TRIVIAL(debug) << "Stabilizers: " << struts.size() << " struts, " << report.painted << " painted point(s), "
                             << report.unreachable.size() << " unreachable";
    if (struts.empty())
        return report;
    throw_if_canceled();

    std::vector<ExPolygons> slices = stabilizers::slice_struts(object, struts, throw_if_canceled);

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
            while (!loop.empty()) {
                Polygons next = offset(loop, -spacing);
                extrusion_entities_append_loops(coll->entities, std::move(loop), erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                loop = std::move(next);
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
