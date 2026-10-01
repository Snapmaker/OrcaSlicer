#include "Stabilizers.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace stabilizers {

// Farthest crossing of the ray `from + t * dir` (t > 0) with the island's outer contour, as t.
// Negative when the ray never crosses it.
static double outermost_crossing(const Polygon &contour, const Vec2d &from, const Vec2d &dir)
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
        if (t > 0. && s >= 0. && s <= 1.)
            best = std::max(best, t);
    }
    return best;
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
        auto it = std::lower_bound(layers.begin(), layers.end(), float(z),
                                   [](const LayerOutline &l, float zz) { return l.slice_z < zz; });
        if (it == layers.end())
            it = std::prev(it);
        else if (it != layers.begin() && std::abs(std::prev(it)->slice_z - z) < std::abs(it->slice_z - z))
            it = std::prev(it);
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
                const double t = outermost_crossing(island.contour, c, dir);
                if (t <= 0.)
                    continue;
                const Vec2d p = c + dir * t;
                out.push_back({ Vec2d(unscaled(p.x()), unscaled(p.y())), dir, size_t(it - layers.begin()), it->slice_z });
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

std::vector<Strut> plan_struts(const PrintObject &object)
{
    std::vector<Strut> out;
    const PrintObjectConfig &cfg = object.config();
    const auto              &layers = object.layers();
    if (layers.size() < 2)
        return out;

    std::vector<LayerOutline> outlines;
    outlines.reserve(layers.size());
    for (const Layer *layer : layers)
        outlines.push_back({ float(layer->slice_z), &layer->lslices });

    RingParams rp;
    rp.ring_spacing     = cfg.stabilizer_ring_spacing.value;
    rp.points_per_ring  = cfg.stabilizer_points_per_ring.value;
    rp.tip_diameter     = cfg.stabilizer_tip_diameter.value;
    rp.max_island_width = cfg.stabilizer_max_island_width.value;

    const double pillar_r = pillar_radius(object);
    // The pillar stands this far off anything of the part below it.
    const double clearance = std::max(1.0, cfg.support_object_xy_distance.value);
    const double max_run   = 10.;

    for (const Contact &c : ring_contacts(outlines, rp)) {
        // Shortest strut whose pillar has a clear way down to the bed, and whose own path from the
        // pillar up to the tip stays off the part.
        for (double run = pillar_r + clearance; run <= max_run + EPSILON; run += 0.5) {
            Strut s;
            s.tip       = c.pos;
            s.dir       = c.dir;
            s.tip_layer = c.layer;
            s.tip_z     = c.z;
            s.run       = run;
            const Vec2d  pillar = s.pillar();
            const double top_z  = s.junction_z();
            bool ok = true;
            for (size_t i = 0; ok && i <= c.layer; ++i) {
                const double z = layers[i]->slice_z;
                if (z <= top_z + EPSILON) {
                    // (A little slack: on a round part the clearance is exactly met.)
                    ok = !disc_hits(layers[i]->lslices, pillar, pillar_r + clearance - 0.1);
                } else {
                    // Along the strut: its axis must stay outside the part and move away from the
                    // wall at least half as fast as it drops - true of any wall that does not lean
                    // out over the strut.
                    const double dz = c.z - z;
                    if (dz > rp.tip_diameter)
                        ok = distance_to(layers[i]->lslices, s.axis_at(z)) >= 0.5 * dz;
                }
            }
            if (ok) {
                out.push_back(s);
                break;
            }
        }
    }
    return out;
}

double pillar_radius(const PrintObject &object)
{
    // Two perimeters on each side at least, or a pillar is a single wobbly loop.
    const double min_d = 4. * support_material_flow(&object).width();
    return 0.5 * std::max(object.config().stabilizer_pillar_diameter.value, min_d);
}

std::vector<ExPolygons> slice_struts(const PrintObject &object, const std::vector<Strut> &struts,
                                     const std::function<void()> &throw_if_canceled)
{
    const auto &layers = object.layers();
    std::vector<ExPolygons> out(layers.size());
    if (struts.empty())
        return out;

    const PrintObjectConfig &cfg = object.config();
    const double tip_r    = 0.5 * cfg.stabilizer_tip_diameter.value;
    const double pillar_r = pillar_radius(object);
    // A small foot on the bed: the pillar widens by this much over the same height, at 45 degrees.
    const double foot     = std::min(1.0, pillar_r);
    const float  tip_gap  = scaled<float>(cfg.stabilizer_tip_gap.value);

    for (size_t i = 0; i < layers.size(); ++i) {
        throw_if_canceled();
        const double z = layers[i]->slice_z;
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
        const ExPolygons &part = layers[i]->lslices;
        out[i] = tip_gap > 0.f ? diff_ex(union_(polys), offset_ex(part, tip_gap)) : diff_ex(union_(polys), part);
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

void generate_stabilizer_supports(PrintObject &object, const std::function<void()> &throw_if_canceled)
{
    const PrintObjectConfig &cfg = object.config();
    if (!cfg.stabilizer_supports.value || object.layers().size() < 2)
        return;

    const std::vector<stabilizers::Strut> struts = stabilizers::plan_struts(object);
    BOOST_LOG_TRIVIAL(debug) << "Stabilizers: " << struts.size() << " struts";
    if (struts.empty())
        return;
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
}

} // namespace Slic3r
