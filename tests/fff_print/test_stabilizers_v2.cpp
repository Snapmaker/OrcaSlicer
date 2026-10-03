// Side stabilizers v2 (libslic3r/Support/Stabilizers.hpp): tapered pillars, pillar-to-pillar bracing,
// rounded-rectangle columns, and walls with sparse infill inside the stabilizer bodies - with the
// bake (StabilizerMesh.hpp) building every one of them as the live generator prints it.
//
// Most cases run the planner and the slicer on synthetic layer outlines (a round pin, a pair of
// pins), which is fast and exact; the walls / infill and the bake run on whole slices of the pin.
// The gates: every feature's geometry, printability (no island without the layer below under it,
// nothing reaching more than half a line past it), braces within the span and never across the part,
// the wall / infill settings applied, and bake parity per feature.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SVG.hpp"
#include "libslic3r/Support/StabilizerBake.hpp"
#include "libslic3r/Support/StabilizerMesh.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace stabv2_test {

double area_of(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

double area_of(const Polygons &polys)
{
    double a = 0.;
    for (const Polygon &p : polys)
        a += p.area();
    return unscaled(unscaled(a));
}

ExPolygon disc(const Vec2d &c, double r, int segments = 72)
{
    Polygon p;
    for (int i = 0; i < segments; ++i) {
        const double a = 2. * M_PI * i / segments;
        p.points.emplace_back(scaled(c.x() + r * std::cos(a)), scaled(c.y() + r * std::sin(a)));
    }
    return ExPolygon(p);
}

// 0.2 mm layers of the same outline up to `height`.
struct Column
{
    ExPolygons                              outline;
    std::vector<stabilizers::LayerOutline> layers;

    Column(ExPolygons o, double height) : outline(std::move(o))
    {
        for (double z = 0.1; z < height; z += 0.2)
            layers.push_back({ float(z), &outline });
    }
};

// The pin of the v1 tests, as outlines: 6 mm across, 60 mm tall. Rings at 15, 30 and 45 mm.
stabilizers::StabilizerSettings pin_settings(int points = 3)
{
    stabilizers::StabilizerSettings st;
    st.rings.ring_spacing    = 15.;
    st.rings.points_per_ring = points;
    st.rings.tip_diameter    = 0.8;
    st.pillar_radius         = 1.5;
    st.clearance             = 1.;
    st.max_run               = 10.;
    return st;
}

void set_taper(stabilizers::StabilizerSettings &st, double base_radius)
{
    st.pillar_base_radius = base_radius;
    st.max_run            = std::max(10., base_radius + st.clearance + 4.);
}

struct Printability
{
    size_t islands           = 0;
    size_t floating          = 0;
    double worst_unsupported = 0.;
    double worst_z           = 0.;
};

// FDM lays every layer on the one below: each island must overlap the layer below, and no part of it
// may reach more than half a line (0.2 mm here) past it.
Printability printability(const std::vector<stabilizers::LayerOutline> &layers, const std::vector<ExPolygons> &slices,
                          double half_line = 0.2)
{
    Printability out;
    for (size_t i = 1; i < slices.size(); ++i) {
        const Polygons below = offset(to_polygons(slices[i - 1]), scaled<float>(half_line));
        for (const ExPolygon &island : slices[i]) {
            if (unscaled(unscaled(island.area())) < 0.04)
                continue;
            ++out.islands;
            if (intersection_ex(ExPolygons{ island }, slices[i - 1]).empty())
                ++out.floating;
            const double un = area_of(diff_ex(ExPolygons{ island }, below));
            if (un > out.worst_unsupported) {
                out.worst_unsupported = un;
                out.worst_z           = layers[i].slice_z;
            }
        }
    }
    return out;
}

void check_printable(const std::vector<stabilizers::LayerOutline> &layers, const std::vector<ExPolygons> &slices, const char *label)
{
    REQUIRE_FALSE(slices.empty());
    CHECK(area_of(slices.front()) > 1.);
    const Printability pr = printability(layers, slices);
    INFO(label << ": " << pr.islands << " islands, " << pr.floating << " floating, worst unsupported " << pr.worst_unsupported
               << " mm2 at z=" << pr.worst_z);
    CHECK(pr.islands > 50);
    CHECK(pr.floating == 0);
    CHECK(pr.worst_unsupported < 0.02);
}

// The layer index whose slicing plane is nearest z.
size_t layer_at(const std::vector<stabilizers::LayerOutline> &layers, double z)
{
    size_t best = 0;
    for (size_t i = 0; i < layers.size(); ++i)
        if (std::abs(layers[i].slice_z - z) < std::abs(layers[best].slice_z - z))
            best = i;
    return best;
}

// Parity of the analytic mesh with the live slices, layer by layer.
struct Parity
{
    double live = 0., baked = 0., xor_area = 0., in_part = 0., worst_iou = 1.;
    size_t layers = 0;
};

Parity parity(const std::vector<stabilizers::LayerOutline> &outlines, const std::vector<ExPolygons> &live, const indexed_triangle_set &mesh)
{
    Parity             out;
    std::vector<float> zs;
    for (const stabilizers::LayerOutline &l : outlines)
        zs.push_back(l.slice_z);
    const std::vector<ExPolygons> baked = slice_mesh_ex(mesh, zs);
    for (size_t i = 0; i < outlines.size(); ++i) {
        const double a = area_of(live[i]), b = area_of(baked[i]);
        if (a <= 0. && b <= 0.)
            continue;
        const double x = area_of(diff_ex(live[i], baked[i])) + area_of(diff_ex(baked[i], live[i]));
        out.live += a;
        out.baked += b;
        out.xor_area += x;
        out.in_part += area_of(intersection_ex(baked[i], *outlines[i].islands));
        ++out.layers;
        if (a > 1.) {
            const double inter = area_of(intersection_ex(live[i], baked[i]));
            out.worst_iou      = std::min(out.worst_iou, inter / (a + b - inter));
        }
    }
    return out;
}

// The bake's gate for one feature: a closed mesh that slices like the live plan.
void check_bake(const std::vector<stabilizers::LayerOutline> &layers, const stabilizers::StabilizerSettings &st,
                const stabilizers::Plan &plan, const char *label)
{
    stabilizers::MeshReport rep;
    const indexed_triangle_set mesh = stabilizers::stabilizer_mesh(plan, st, {}, &rep);
    CHECK(rep.closed);
    CHECK(its_num_open_edges(mesh) == 0);
    CHECK(rep.braces == plan.braces.size());
    CHECK(rep.columns == size_t(std::count_if(plan.pillars.begin(), plan.pillars.end(), [](const stabilizers::Pillar &p) { return p.column; })));
    for (const indexed_triangle_set &s : stabilizers::stabilizer_shells(plan, st))
        CHECK(its_num_open_edges(s) == 0);
    const Parity p = parity(layers, stabilizers::slice_plan(layers, st, plan, {}), mesh);
    WARN(label << ": " << plan.struts.size() << " struts, " << rep.pillars << " pillars (" << rep.columns << " columns), " << rep.braces
               << " braces, " << rep.triangles << " triangles, " << (rep.unioned ? "unioned" : "shells") << "; parity over " << p.layers
               << " layers: live " << p.live << " mm2, baked " << p.baked << " mm2, symmetric difference " << p.xor_area << " mm2 ("
               << 100. * p.xor_area / p.live << " %), worst IoU " << p.worst_iou << ", baked inside the part " << p.in_part << " mm2");
    CHECK(p.xor_area < 0.005 * p.live);
    CHECK(p.worst_iou > 0.95);
    CHECK(p.in_part < 0.01 * p.live);
}

// Nothing of a brace comes within the clearance (less the planner's 0.1 mm slack and some
// polygonization) of the part, at any layer.
double closest_brace_approach(const std::vector<stabilizers::LayerOutline> &layers, const stabilizers::Plan &plan)
{
    double closest = std::numeric_limits<double>::max();
    for (const stabilizers::Brace &b : plan.braces)
        for (const stabilizers::LayerOutline &l : layers) {
            if (l.slice_z < b.z_bottom() || l.slice_z > b.z_top())
                continue;
            Polygon e;
            const Vec2d c = b.axis_at(l.slice_z), d = b.dir(), perp(-d.y(), d.x());
            for (int k = 0; k < 48; ++k) {
                const double phi = 2. * M_PI * k / 48;
                const Vec2d  p   = c + d * (M_SQRT2 * b.radius * std::cos(phi)) + perp * (b.radius * std::sin(phi));
                e.points.emplace_back(scaled(p.x()), scaled(p.y()));
            }
            // Cut at the two pillars' axes.
            const Vec2d a0 = b.from - perp * 10., a1 = b.to - perp * 10., a2 = b.to + perp * 10., a3 = b.from + perp * 10.;
            Polygon strip({ Point(scaled(a0.x()), scaled(a0.y())), Point(scaled(a1.x()), scaled(a1.y())),
                            Point(scaled(a2.x()), scaled(a2.y())), Point(scaled(a3.x()), scaled(a3.y())) });
            if (strip.is_clockwise())
                strip.reverse();
            for (const Polygon &sec : intersection(Polygons{ e }, Polygons{ strip }))
                for (const Point &pt : sec.points)
                    for (const ExPolygon &part : *l.islands) {
                        if (part.contains(pt)) {
                            closest = 0.;
                            continue;
                        }
                        for (const Line &ln : part.contour.lines())
                            closest = std::min(closest, unscaled(ln.distance_to(pt)));
                    }
        }
    return closest;
}

// --- whole prints ---------------------------------------------------------------------------------

struct Bench
{
    Print print;
    Model model;
};

DynamicPrintConfig pin_config(std::initializer_list<std::pair<std::string, std::string>> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              "1" },
        { "support_type",                "normal(auto)" },
        { "support_on_build_plate_only", "1" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "stabilizer_supports",         "auto" },
        { "stabilizer_ring_spacing",     "15" },
        { "stabilizer_points_per_ring",  "3" },
        { "stabilizer_tip_gap",          "0" },
    });
    for (const auto &[k, v] : extra)
        config.set_deserialize_strict(k, v);
    return config;
}

void slice_pin(Bench &b, const DynamicPrintConfig &config)
{
    ModelObject *object = b.model.add_object();
    object->name = "pin";
    object->add_volume(TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.)));
    object->add_instance();
    object->ensure_on_bed();
    b.print.auto_assign_extruders(object);
    b.print.apply(b.model, config);
    b.print.set_status_silent();
    b.print.process();
}

std::vector<ExPolygons> printed_stabilizers(const PrintObject &po)
{
    std::vector<ExPolygons> out(po.layers().size());
    for (size_t i = 0; i < po.layers().size(); ++i)
        for (const SupportLayer *sl : po.support_layers())
            if (std::abs(sl->print_z - po.layers()[i]->print_z) < EPSILON)
                expolygons_append(out[i], sl->support_islands);
    return out;
}

// The stabilizer extrusions of the support layer at the object layer nearest `z`: loops and open paths.
struct Extrusions
{
    size_t loops  = 0;
    size_t paths  = 0;
    double length = 0.; // mm
};

void count(const ExtrusionEntity *e, Extrusions &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        for (const ExtrusionEntity *c : coll->entities)
            count(c, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(e)) {
        ++out.loops;
        out.length += unscaled(loop->length());
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(e)) {
        ++out.paths;
        out.length += unscaled(path->length());
    } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e)) {
        out.paths += mp->paths.size();
        out.length += unscaled(mp->length());
    }
}

Extrusions extrusions_at(const PrintObject &po, double z)
{
    Extrusions   out;
    const Layer *layer = po.layers().front();
    for (const Layer *l : po.layers())
        if (std::abs(l->print_z - z) < std::abs(layer->print_z - z))
            layer = l;
    for (const SupportLayer *sl : po.support_layers())
        if (std::abs(sl->print_z - layer->print_z) < EPSILON)
            for (const ExtrusionEntity *e : sl->support_fills.entities)
                count(e, out);
    return out;
}

} // namespace stabv2_test

using namespace stabv2_test;

// --- defaults ---------------------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: the defaults leave the v1 stabilizers as they were", "[Stabilizers][StabilizersV2]")
{
    const PrintObjectConfig               cfg;
    const stabilizers::StabilizerSettings st = stabilizers::StabilizerSettings::from_config(cfg, 0.4);
    CHECK_FALSE(st.tapered());
    CHECK_FALSE(st.bracing);
    CHECK(st.column_shape == scsRound);
    CHECK(st.wall_loops == 0);
    CHECK_THAT(st.max_run, WithinAbs(10., 1e-9));
    // The planner's own defaults are the settings' defaults.
    const stabilizers::StabilizerSettings plain;
    CHECK(st.max_unbraced == plain.max_unbraced);
    CHECK(st.max_brace_span == plain.max_brace_span);
    CHECK(st.column_width == plain.column_width);
    CHECK(st.column_length == plain.column_length);
    CHECK(st.column_min_height == plain.column_min_height);
    CHECK_THAT(st.infill_density, WithinAbs(plain.infill_density, 1e-9));
    CHECK(st.infill_pattern == plain.infill_pattern);

    Column pin({ disc(Vec2d::Zero(), 3.) }, 60.);
    stabilizers::StabilizerSettings s = pin_settings();
    s.max_unbraced                    = cfg.stabilizer_brace_max_unbraced.value;
    s.max_brace_span                  = cfg.stabilizer_brace_max_span.value;
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, s, {}, &rep);
    CHECK(plan.struts.size() == 9);
    CHECK(plan.pillars.size() == 3);
    CHECK(plan.braces.empty());
    CHECK(rep.columns == 0);
    // Straight round pillars: the same circle at every height above the foot.
    for (const stabilizers::Pillar &p : plan.pillars) {
        CHECK_FALSE(p.column);
        const double a5 = area_of(Polygons{ stabilizers::pillar_section(p, s, 5.) });
        const double a9 = area_of(Polygons{ stabilizers::pillar_section(p, s, 9.) });
        CHECK_THAT(a5, WithinRel(a9, 1e-9));
    }
    // No walls: nothing sparse anywhere.
    const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, s, plan, {});
    for (const ExPolygons &e : stabilizers::sparse_infill_areas(slices, s, 0.42, 0.4))
        CHECK(e.empty());
}

// --- tapered pillars --------------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: tapered pillars are wide at the bed, the pillar diameter at their top, and clear of the part",
          "[Stabilizers][StabilizersV2]")
{
    Column                          pin({ disc(Vec2d::Zero(), 3.) }, 60.);
    stabilizers::StabilizerSettings st = pin_settings();
    set_taper(st, 3.);
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
    REQUIRE(plan.struts.size() == 9);
    REQUIRE(plan.pillars.size() == 3);

    for (const stabilizers::Pillar &p : plan.pillars) {
        INFO("pillar at " << p.pos.transpose() << ", top " << p.top_z);
        // Radius: the base at the bed, the pillar radius at the top, linear in between.
        CHECK_THAT(stabilizers::pillar_radius_at(p, st, 0.), WithinAbs(3., 1e-9));
        CHECK_THAT(stabilizers::pillar_radius_at(p, st, p.top_z), WithinAbs(1.5, 1e-9));
        CHECK_THAT(stabilizers::pillar_radius_at(p, st, 0.5 * p.top_z), WithinAbs(2.25, 1e-9));
        // Narrowing upwards, layer by layer: each section inside the one below (prints with no overhang).
        double last = std::numeric_limits<double>::max();
        for (const stabilizers::LayerOutline &l : pin.layers) {
            if (l.slice_z > p.top_z)
                break;
            const Polygon sec = stabilizers::pillar_section(p, st, l.slice_z);
            const double  a   = area_of(Polygons{ sec });
            CHECK(a <= last + 1e-6);
            last = a;
            // Clear of the part above the foot, by the clearance less the planner's slack.
            if (l.slice_z > 1.) {
                CHECK(intersection(Polygons{ sec }, offset(to_polygons(pin.outline), scaled<float>(st.clearance - 0.15))).empty());
            }
        }
    }

    const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
    check_printable(pin.layers, slices, "tapered pin");
    // More material low down than straight pillars, the same at the top.
    stabilizers::StabilizerSettings straight = pin_settings();
    const stabilizers::Plan         sp       = stabilizers::plan_stabilizers(pin.layers, straight, {});
    const std::vector<ExPolygons>   ss       = stabilizers::slice_plan(pin.layers, straight, sp, {});
    const size_t                    low      = layer_at(pin.layers, 3.);
    CHECK(area_of(slices[low]) > 2.5 * area_of(ss[low]));

    // And it bakes as it prints.
    check_bake(pin.layers, st, plan, "tapered pin");
}

// --- bracing ----------------------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: braces tie neighbouring pillars at 45 degrees, within the span, off the part",
          "[Stabilizers][StabilizersV2]")
{
    // Six struts per ring on wide-based pillars: the pillars stand far enough out that the chord between
    // two neighbours passes the part with room to spare.
    Column                          pin({ disc(Vec2d::Zero(), 3.) }, 60.);
    stabilizers::StabilizerSettings st = pin_settings(6);
    set_taper(st, 3.);
    st.bracing        = true;
    st.max_unbraced   = 10.;
    st.max_brace_span = 25.;
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
    REQUIRE(plan.pillars.size() == 6);
    INFO(plan.braces.size() << " braces, " << rep.unbraced_pillars << " pillars left unbraced");
    REQUIRE(plan.braces.size() >= 6);
    CHECK(rep.braces == plan.braces.size());

    for (const stabilizers::Brace &b : plan.braces) {
        const stabilizers::Pillar &lo = plan.pillars[b.lower], &up = plan.pillars[b.upper];
        INFO("brace " << b.lower << " -> " << b.upper << " from z=" << b.z_low << " to " << b.z_high());
        CHECK(b.lower != b.upper);
        // Pillar axis to pillar axis, at 45 degrees: it rises exactly its span.
        CHECK((b.from - lo.pos).norm() < 1e-9);
        CHECK((b.to - up.pos).norm() < 1e-9);
        CHECK_THAT(b.z_high() - b.z_low, WithinAbs(b.span(), 1e-9));
        CHECK(b.span() <= st.max_brace_span + 1e-9);
        // Both ends on their pillars, the lower end off the bed.
        CHECK(b.z_low >= M_SQRT2 * b.radius);
        CHECK(b.z_low <= lo.top_z + 1e-3);
        CHECK(b.z_high() <= up.top_z + 1e-3);
    }
    // No pillar stands unbraced for longer than the limit any more.
    CHECK(rep.unbraced_pillars == 0);
    for (size_t i = 0; i < plan.pillars.size(); ++i)
        CHECK(stabilizers::longest_unbraced(plan, i) <= st.max_unbraced + 1e-6);
    // Never across the part: every brace section stays the clearance (less slack) off it.
    const double closest = closest_brace_approach(pin.layers, plan);
    INFO("closest brace approach to the part: " << closest << " mm");
    CHECK(closest > st.clearance - 0.15);
    // Single diagonals, no X: no two braces of the same pair overlap in height.
    for (size_t a = 0; a < plan.braces.size(); ++a)
        for (size_t c = a + 1; c < plan.braces.size(); ++c) {
            const stabilizers::Brace &x = plan.braces[a], &y = plan.braces[c];
            if (std::minmax(x.lower, x.upper) == std::minmax(y.lower, y.upper))
                CHECK_FALSE((x.z_low < y.z_high() - 1e-6 && y.z_low < x.z_high() - 1e-6));
        }

    const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
    check_printable(pin.layers, slices, "braced pin");
    // Nothing printed inside the part.
    for (size_t i = 0; i < slices.size(); ++i)
        CHECK(area_of(intersection_ex(slices[i], pin.outline)) < 0.01);

    check_bake(pin.layers, st, plan, "braced pin");
}

TEST_CASE("Stabilizers v2: a brace that would cross the part, or span too far, is not placed", "[Stabilizers][StabilizersV2]")
{
    Column pin({ disc(Vec2d::Zero(), 3.) }, 60.);

    SECTION("three pillars around a thin pin: every chord between them passes through the part")
    {
        stabilizers::StabilizerSettings st = pin_settings(3);
        st.bracing                         = true;
        st.max_unbraced                    = 10.;
        stabilizers::PlanReport rep;
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        REQUIRE(plan.pillars.size() == 3);
        CHECK(plan.braces.empty());
        CHECK(rep.unbraced_pillars == 3);
    }
    SECTION("neighbours farther apart than the max span")
    {
        stabilizers::StabilizerSettings st = pin_settings(6);
        set_taper(st, 3.);
        st.bracing        = true;
        st.max_unbraced   = 10.;
        st.max_brace_span = 5.;
        stabilizers::PlanReport rep;
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        REQUIRE(plan.pillars.size() == 6);
        // The nearest neighbours stand 7 mm apart.
        CHECK((plan.pillars[0].pos - plan.pillars[1].pos).norm() > 5.);
        CHECK(plan.braces.empty());
        CHECK(rep.unbraced_pillars == 6);
    }
}

TEST_CASE("Stabilizers v2: two pins brace each other's pillars across the gap", "[Stabilizers][StabilizersV2]")
{
    // Two 6 mm pins 20 mm apart, struts pointing at each other's side: pillars of the two pins face
    // each other across open air, which is where a brace belongs.
    Column pins({ disc(Vec2d(-10., 0.), 3.), disc(Vec2d(10., 0.), 3.) }, 60.);
    stabilizers::StabilizerSettings st = pin_settings(4);
    st.rings.max_island_width          = 0.;
    st.bracing                         = true;
    st.max_unbraced                    = 8.;
    st.max_brace_span                  = 12.;
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(pins.layers, st, {}, &rep);
    REQUIRE(plan.pillars.size() == 8);
    CHECK_FALSE(plan.braces.empty());
    for (const stabilizers::Brace &b : plan.braces)
        CHECK(b.span() <= st.max_brace_span + 1e-9);
    CHECK(closest_brace_approach(pins.layers, plan) > st.clearance - 0.15);
    const std::vector<ExPolygons> slices = stabilizers::slice_plan(pins.layers, st, plan, {});
    check_printable(pins.layers, slices, "two pins");
    check_bake(pins.layers, st, plan, "two pins");
}

// --- columns ----------------------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: rounded-rectangle columns", "[Stabilizers][StabilizersV2]")
{
    Column pin({ disc(Vec2d::Zero(), 3.) }, 60.);

    SECTION("every pillar a column: the set size, the side facing the part where a round pillar's is")
    {
        stabilizers::StabilizerSettings st = pin_settings();
        st.column_shape                    = scsRoundedRect;
        st.column_width                    = 6.;
        st.column_length                   = 10.;
        stabilizers::PlanReport rep;
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        REQUIRE(plan.pillars.size() == 3);
        CHECK(rep.columns == 3);
        for (const stabilizers::Pillar &p : plan.pillars) {
            REQUIRE(p.column);
            const std::vector<Vec2d> outline = stabilizers::column_outline(stabilizers::column_frame(p, st), 0.);
            const Vec2d perp(-p.dir.y(), p.dir.x());
            double umin = 1e9, umax = -1e9, vmin = 1e9, vmax = -1e9;
            for (const Vec2d &v : outline) {
                umin = std::min(umin, (v - p.pos).dot(p.dir));
                umax = std::max(umax, (v - p.pos).dot(p.dir));
                vmin = std::min(vmin, (v - p.pos).dot(perp));
                vmax = std::max(vmax, (v - p.pos).dot(perp));
            }
            CHECK_THAT(umax - umin, WithinAbs(6., 1e-6));
            CHECK_THAT(vmax - vmin, WithinAbs(10., 1e-6));
            // The part side where a round pillar of the pillar diameter would have it.
            CHECK_THAT(umin, WithinAbs(-st.pillar_radius, 1e-6));
            // Area: the rectangle less its four fillets (1 mm), polygonized.
            const double a = area_of(Polygons{ stabilizers::pillar_section(p, st, 10.) });
            CHECK(a > 60. - (4. - M_PI) - 0.1);
            CHECK(a < 60. - (4. - M_PI) + 0.01);
            for (const stabilizers::LayerOutline &l : pin.layers)
                if (l.slice_z > 1. && l.slice_z <= p.top_z)
                    CHECK(intersection(Polygons{ stabilizers::pillar_section(p, st, l.slice_z) },
                                       offset(to_polygons(pin.outline), scaled<float>(st.clearance - 0.15)))
                              .empty());
        }
        const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
        check_printable(pin.layers, slices, "columns");
        check_bake(pin.layers, st, plan, "columns");
    }

    SECTION("tapered columns")
    {
        stabilizers::StabilizerSettings st = pin_settings();
        st.column_shape                    = scsRoundedRect;
        set_taper(st, 2.5);
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {});
        REQUIRE(plan.pillars.size() == 3);
        const stabilizers::Pillar &p = plan.pillars.front();
        CHECK(area_of(Polygons{ stabilizers::pillar_section(p, st, 2.) }) > area_of(Polygons{ stabilizers::pillar_section(p, st, p.top_z) }) + 10.);
        const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
        check_printable(pin.layers, slices, "tapered columns");
        check_bake(pin.layers, st, plan, "tapered columns");
    }

    SECTION("Auto: columns only for tall pillars left long-unbraced or alone")
    {
        stabilizers::StabilizerSettings st = pin_settings();
        st.column_shape                    = scsAuto;
        st.max_unbraced                    = 10.;
        st.column_min_height               = 30.;
        stabilizers::PlanReport rep;
        stabilizers::Plan       plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        REQUIRE(plan.pillars.size() == 3);
        // About 42 mm tall, 15 mm between their struts: all three.
        CHECK(rep.columns == 3);
        check_bake(pin.layers, st, plan, "auto columns");
        check_printable(pin.layers, stabilizers::slice_plan(pin.layers, st, plan, {}), "auto columns");

        // Shorter than the column height: round.
        st.column_min_height = 50.;
        plan                 = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        CHECK(rep.columns == 0);

        // Tied often enough (rings 8 mm apart): round.
        st.column_min_height       = 30.;
        st.rings.ring_spacing      = 8.;
        plan                       = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        CHECK(rep.columns == 0);

        // Braced well enough: round.
        stabilizers::StabilizerSettings br = pin_settings(6);
        set_taper(br, 3.);
        br.bracing      = true;
        br.column_shape = scsAuto;
        plan            = stabilizers::plan_stabilizers(pin.layers, br, {}, &rep);
        CHECK(plan.braces.size() >= 6);
        CHECK(rep.columns == 0);
    }

    SECTION("Auto: a lone pillar is a column even when it is tied often")
    {
        stabilizers::StabilizerSettings st = pin_settings(1);
        st.column_shape                    = scsAuto;
        st.rings.ring_spacing              = 8.;
        stabilizers::PlanReport rep;
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, {}, &rep);
        REQUIRE(plan.pillars.size() == 1);
        CHECK(stabilizers::longest_unbraced(plan, 0) <= st.max_unbraced);
        CHECK(rep.columns == 1);
    }
}

// --- painted points -----------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: painted points get tapered, braced pillars too", "[Stabilizers][StabilizersV2][StabilizerPaint]")
{
    Column                          pin({ disc(Vec2d::Zero(), 3.) }, 60.);
    stabilizers::StabilizerSettings st = pin_settings(6);
    set_taper(st, 3.);
    st.bracing       = true;
    st.column_shape  = scsAuto;
    // Manual: the painted points only - two on the +Y side, one on the -X side.
    st.ring_struts   = false;
    const std::vector<stabilizers::PaintedSpot> spots{ { Vec3d(0., 3., 22.), Vec3d(0., 1., 0.) },
                                                       { Vec3d(0., 3., 40.), Vec3d(0., 1., 0.) },
                                                       { Vec3d(-3., 0., 35.), Vec3d(-1., 0., 0.) } };
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(pin.layers, st, spots, &rep);
    CHECK(rep.painted_placed == 3);
    CHECK(rep.unreachable.empty());
    REQUIRE(plan.struts.size() == 3);
    for (const stabilizers::Strut &s : plan.struts)
        CHECK(s.painted);
    const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
    check_printable(pin.layers, slices, "painted");
    check_bake(pin.layers, st, plan, "painted");
}

// --- walls and infill ---------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: walls and sparse infill where the bodies are wide, solid where they are thin",
          "[Stabilizers][StabilizersV2]")
{
    SECTION("columns with two walls and 20 % infill")
    {
        Bench b;
        slice_pin(b, pin_config({ { "stabilizer_column_shape", "rounded_rect" },
                                  { "stabilizer_column_width", "8" },
                                  { "stabilizer_column_length", "12" },
                                  { "stabilizer_wall_loops", "2" },
                                  { "stabilizer_infill_density", "20%" },
                                  { "stabilizer_infill_pattern", "rectilinear" } }));
        const PrintObject &po = *b.print.objects().front();
        const stabilizers::StabilizerSettings st = stabilizers::settings_of(po);
        REQUIRE(st.wall_loops == 2);
        CHECK_THAT(st.infill_density, WithinAbs(0.2, 1e-9));

        // Between the rings, away from any strut: the three columns, each two walls around sparse lines.
        const Extrusions mid = extrusions_at(po, 20.);
        INFO("z=20: " << mid.loops << " loops, " << mid.paths << " infill paths, " << mid.length << " mm");
        CHECK(mid.loops == 6);
        CHECK(mid.paths >= 3);

        // The same columns solid print a lot more.
        Bench solid;
        slice_pin(solid, pin_config({ { "stabilizer_column_shape", "rounded_rect" },
                                      { "stabilizer_column_width", "8" },
                                      { "stabilizer_column_length", "12" } }));
        const Extrusions full = extrusions_at(*solid.print.objects().front(), 20.);
        INFO("solid: " << full.loops << " loops, " << full.length << " mm");
        CHECK(full.paths == 0);
        CHECK(mid.length < 0.6 * full.length);

        // First layers and the layers under a column's top stay solid: no infill paths there.
        CHECK(extrusions_at(po, 0.2).paths == 0);
        const stabilizers::Plan plan = stabilizers::plan_stabilizers(po);
        double top = 0.;
        for (const stabilizers::Pillar &p : plan.pillars)
            top = std::max(top, p.top_z);
        CHECK(extrusions_at(po, top - 0.1).paths == 0);

        // Still prints layer on layer, and the printed islands are the planned slices.
        const std::vector<ExPolygons> stab = printed_stabilizers(po);
        CHECK(area_of(stab.front()) > 1.);
    }

    SECTION("thin round pillars stay solid whatever the walls")
    {
        Bench b;
        slice_pin(b, pin_config({ { "stabilizer_wall_loops", "2" }, { "stabilizer_infill_density", "15%" } }));
        const PrintObject &po = *b.print.objects().front();
        const stabilizers::StabilizerSettings st     = stabilizers::settings_of(po);
        const Flow                            flow   = support_material_flow(&po);
        const std::vector<ExPolygons>         slices = stabilizers::slice_plan(stabilizers::outlines_of(po), st,
                                                                               stabilizers::plan_stabilizers(po), {});
        for (const ExPolygons &e : stabilizers::sparse_infill_areas(slices, st, flow.width(), flow.spacing()))
            CHECK(e.empty());
        for (double z : { 5., 20., 29.8, 44.8 }) {
            const Extrusions at = extrusions_at(po, z);
            INFO("z=" << z << ": " << at.loops << " loops, " << at.paths << " paths");
            CHECK(at.loops > 0);
            CHECK(at.paths == 0);
        }
    }

    SECTION("the sparse areas keep solid shells and stay inside the walls")
    {
        Column                          pin({ disc(Vec2d::Zero(), 3.) }, 60.);
        stabilizers::StabilizerSettings st = pin_settings();
        st.column_shape                    = scsRoundedRect;
        st.column_width                    = 8.;
        st.column_length                   = 12.;
        st.wall_loops                      = 3;
        st.infill_density                  = 0.15;
        const stabilizers::Plan       plan   = stabilizers::plan_stabilizers(pin.layers, st, {});
        const std::vector<ExPolygons> slices = stabilizers::slice_plan(pin.layers, st, plan, {});
        const std::vector<ExPolygons> sparse = stabilizers::sparse_infill_areas(slices, st, 0.42, 0.4);
        size_t layers_with_infill = 0;
        for (size_t i = 0; i < slices.size(); ++i) {
            if (sparse[i].empty())
                continue;
            ++layers_with_infill;
            // Inside three walls.
            CHECK(area_of(diff_ex(sparse[i], offset_ex(slices[i], -scaled<float>(0.42 + 2 * 0.4 - 0.01)))) < 1e-3);
            // Solid shells: the slices around are there.
            CHECK(i >= stabilizers::STABILIZER_BOTTOM_SOLID_LAYERS);
            CHECK(i + stabilizers::STABILIZER_TOP_SOLID_LAYERS < slices.size());
        }
        CHECK(layers_with_infill > 50);
        CHECK(sparse.front().empty());
    }
}

// --- the bake -------------------------------------------------------------------------------------

TEST_CASE("Stabilizers v2: the bake builds the whole plan and hands the walls and infill to the baked body",
          "[Stabilizers][StabilizersV2][StabilizerBake]")
{
    Bench        b;
    const DynamicPrintConfig config = pin_config({ { "stabilizer_points_per_ring", "6" },
                                                   { "stabilizer_pillar_diameter", "3" },
                                                   { "stabilizer_pillar_base_diameter", "6" },
                                                   { "stabilizer_bracing", "1" },
                                                   { "stabilizer_brace_max_unbraced", "10" },
                                                   { "stabilizer_column_shape", "auto" },
                                                   { "stabilizer_wall_loops", "2" },
                                                   { "stabilizer_infill_density", "25%" },
                                                   { "stabilizer_infill_pattern", "grid" } });
    slice_pin(b, config);
    const PrintObject      &po = *b.print.objects().front();
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(po, &rep);
    REQUIRE_FALSE(plan.braces.empty());

    // The live print: layer on layer.
    {
        const std::vector<ExPolygons> stab = printed_stabilizers(po);
        const std::vector<stabilizers::LayerOutline> outlines = stabilizers::outlines_of(po);
        const Printability pr = printability(outlines, stab, 0.5 * support_material_flow(&po).width());
        INFO("live print: " << pr.islands << " islands, " << pr.floating << " floating, worst unsupported " << pr.worst_unsupported
                            << " mm2 at z=" << pr.worst_z);
        CHECK(pr.floating == 0);
        CHECK(pr.worst_unsupported < 0.02);
    }

    StabilizerBakeOptions      opts;
    const StabilizerBakeResult res = bake_stabilizers(po, opts);
    REQUIRE(res.error.empty());
    CHECK(res.plan.braces.size() == plan.braces.size());
    CHECK(res.mesh_report.braces == plan.braces.size());
    CHECK(res.mesh_report.closed);
    check_bake(stabilizers::outlines_of(po), stabilizers::settings_of(po), res.plan, "baked pin");

    ModelObject *src  = b.model.objects.front();
    ModelObject *stab = apply_stabilizer_bake(b.model, *src, res, opts);
    REQUIRE(stab != nullptr);
    CHECK(stab->config.opt_int("wall_loops") == 2);
    CHECK_THAT(stab->config.option("sparse_infill_density")->getFloat(), WithinAbs(25., 1e-6));
    CHECK(stab->config.option("sparse_infill_pattern")->getInt() == int(ipGrid));

    // Solid stabilizers bake solid, as v1 did.
    Bench solid;
    slice_pin(solid, pin_config());
    const StabilizerBakeResult rs = bake_stabilizers(*solid.print.objects().front(), opts);
    REQUIRE(rs.error.empty());
    ModelObject *so = apply_stabilizer_bake(solid.model, *solid.model.objects.front(), rs, opts);
    REQUIRE(so != nullptr);
    CHECK(so->config.opt_int("wall_loops") == 3);
    CHECK_THAT(so->config.option("sparse_infill_density")->getFloat(), WithinAbs(100., 1e-6));
}

// ---------------------------------------------------------------------------------------------
// Report (hidden): previews of the owner's fixture with the v2 features. Run with
//   fff_print_tests "[.stabilizers_v2_report]"
// and STAB_PREVIEW_DIR=<dir>: each variant writes side.svg (front and side views of every layer) and
// top_z*.svg (single layers from above: part orange, stabilizers green, the layer below grey).
// ---------------------------------------------------------------------------------------------
namespace stabv2_test {

bool load_fixture(Bench &b, DynamicPrintConfig &config)
{
    const std::string         path = std::string(TEST_DATA_DIR) + "/stabilizers/stabilizer_v1.3mf";
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl = false;
    Semver                    file_version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &b.model, &plate_data, &project_presets, &is_bbl, &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plate_data);
    return ok && ! b.model.objects.empty();
}

void write_side_view(const std::string &path, const PrintObject &po, const std::vector<ExPolygons> &slices)
{
    BoundingBox bb;
    for (size_t i = 0; i < slices.size(); ++i) {
        for (const ExPolygon &e : slices[i])
            bb.merge(get_extents(e.contour));
        for (const ExPolygon &e : po.layers()[i]->lslices)
            bb.merge(get_extents(e.contour));
    }
    const double  s = 8., pad = 4.;
    const double  top = po.layers().back()->print_z;
    const double  w = unscaled(std::max(bb.size().x(), bb.size().y())) + 2 * pad, h = top + 2 * pad;
    std::ofstream f(path);
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << (2 * w + 2) * s << "' height='" << (h + 4) * s << "'>\n";
    f << "<rect width='100%' height='100%' fill='white'/>\n";
    for (int view = 0; view < 2; ++view) {
        const double x0 = view * (w + 2);
        auto X = [&](coord_t v) { return (x0 + pad + unscaled(v - (view == 0 ? bb.min.x() : bb.min.y()))) * s; };
        auto Z = [&](double z) { return (h - pad - z) * s; };
        auto bar = [&](const ExPolygons &ex, double z, double height, const char *fill) {
            for (const ExPolygon &e : ex) {
                const BoundingBox b = get_extents(e.contour);
                const coord_t lo = view == 0 ? b.min.x() : b.min.y(), hi = view == 0 ? b.max.x() : b.max.y();
                f << "<rect fill='" << fill << "' x='" << X(lo) << "' y='" << Z(z) << "' width='" << (X(hi) - X(lo)) << "' height='"
                  << height * s << "'/>\n";
            }
        };
        for (size_t i = 0; i < slices.size(); ++i) {
            const Layer *l = po.layers()[i];
            bar(l->lslices, l->print_z, l->height, "#f0a060");
            bar(slices[i], l->print_z, l->height, "#20a040");
        }
        f << "<text x='" << (x0 + pad) * s << "' y='" << (h + 2) * s << "' font-size='14'>" << (view == 0 ? "front (X)" : "side (Y)")
          << "</text>\n";
    }
    f << "</svg>\n";
}

void write_top_views(const std::string &dir, const PrintObject &po, const std::vector<ExPolygons> &slices, const std::vector<double> &zs)
{
    BoundingBox bb;
    for (const ExPolygons &s : slices)
        for (const ExPolygon &e : s)
            bb.merge(get_extents(e.contour));
    for (const Layer *l : po.layers())
        for (const ExPolygon &e : l->lslices)
            bb.merge(get_extents(e.contour));
    for (double z : zs) {
        size_t best = 0;
        for (size_t i = 0; i < po.layers().size(); ++i)
            if (std::abs(po.layers()[i]->print_z - z) < std::abs(po.layers()[best]->print_z - z))
                best = i;
        char name[64];
        snprintf(name, sizeof(name), "/top_z%05.2f.svg", po.layers()[best]->print_z);
        SVG svg(dir + name, bb, scaled(2.));
        svg.draw(po.layers()[best]->lslices, "#f08020", 0.6f);
        if (best > 0)
            svg.draw(slices[best - 1], "#888888", 0.5f);
        svg.draw(slices[best], "#20a040", 0.7f);
        svg.Close();
    }
}

void report_variant(const char *name, std::initializer_list<std::pair<const char *, const char *>> overrides)
{
    Bench              b;
    DynamicPrintConfig config;
    REQUIRE(load_fixture(b, config));
    config.normalize_fdm();
    ModelObject *o = b.model.objects.front();
    ConfigSubstitutionContext strict(ForwardCompatibilitySubstitutionRule::Disable);
    for (const auto &[k, v] : overrides)
        o->config.set_deserialize(k, v, strict);
    b.print.auto_assign_extruders(o);
    b.print.apply(b.model, config);
    b.print.set_status_silent();
    b.print.process();
    const PrintObject      &po = *b.print.objects().front();
    stabilizers::PlanReport rep;
    const stabilizers::Plan plan = stabilizers::plan_stabilizers(po, &rep);
    const std::vector<ExPolygons> stab = printed_stabilizers(po);
    const std::vector<stabilizers::LayerOutline> outlines = stabilizers::outlines_of(po);
    const Printability pr = printability(outlines, stab, 0.5 * support_material_flow(&po).width());
    double vol = 0., tallest_unbraced = 0.;
    for (size_t i = 0; i < stab.size(); ++i)
        vol += area_of(stab[i]) * po.layers()[i]->height;
    for (size_t i = 0; i < plan.pillars.size(); ++i)
        tallest_unbraced = std::max(tallest_unbraced, stabilizers::longest_unbraced(plan, i));
    // What is actually extruded (walls and infill count only their lines).
    double extruded = 0.;
    for (const SupportLayer *sl : po.support_layers())
        extruded += sl->support_fills.total_volume();
    WARN(name << ": " << plan.struts.size() << " struts, " << plan.pillars.size() << " pillars, " << plan.braces.size() << " braces, "
              << rep.columns << " columns, " << rep.unbraced_pillars << " pillars still long-unbraced, longest unbraced stretch "
              << tallest_unbraced << " mm; stabilizer bodies " << vol << " mm3, extruded " << extruded << " mm3; " << pr.islands << " islands, " << pr.floating
              << " floating, worst unsupported " << pr.worst_unsupported << " mm2");
    std::string pillars;
    for (size_t i = 0; i < plan.pillars.size(); ++i) {
        const stabilizers::Pillar &p = plan.pillars[i];
        char line[200];
        snprintf(line, sizeof(line), "  pillar %zu at (%.4f, %.4f) top %.2f %s, ties", i, p.pos.x(), p.pos.y(), p.top_z, p.column ? "column" : "round");
        pillars += line;
        for (double t : stabilizers::pillar_ties(plan, i)) {
            snprintf(line, sizeof(line), " %.2f", t);
            pillars += line;
        }
        pillars += "\n";
    }
    WARN(name << " pillars:\n" << pillars);
    CHECK(pr.floating == 0);
    if (const char *dir = std::getenv("STAB_PREVIEW_DIR")) {
        const std::string out = std::string(dir) + "/" + name;
        boost::filesystem::create_directories(out);
        write_side_view(out + "/side.svg", po, stab);
        write_top_views(out, po, stab, { 0.3, 5., 12., 20., 30., 40., 45. });
        // The baked stabilizers and the part, in the planner's frame, for a 3D look.
        its_write_stl_binary((out + "/stabilizers.stl").c_str(), name, stabilizers::stabilizer_mesh(plan, stabilizers::settings_of(po)));
        indexed_triangle_set part;
        for (const ModelVolume *v : po.model_object()->volumes)
            if (v->is_model_part()) {
                indexed_triangle_set vits = v->mesh().its;
                its_transform(vits, po.trafo_centered() * v->get_matrix());
                its_merge(part, vits);
            }
        its_write_stl_binary((out + "/part.stl").c_str(), "part", part);
    }
}

} // namespace stabv2_test

TEST_CASE("Stabilizer v2 report: the owner's fixture", "[.stabilizers_v2_report]")
{
    report_variant("v1_as_saved", {});
    report_variant("tapered", { { "stabilizer_pillar_base_diameter", "6" } });
    report_variant("columns", { { "stabilizer_column_shape", "rounded_rect" }, { "stabilizer_wall_loops", "2" } });
    report_variant("braced_6pt", { { "stabilizer_points_per_ring", "6" },
                                   { "stabilizer_pillar_base_diameter", "5" },
                                   { "stabilizer_bracing", "1" },
                                   { "stabilizer_brace_max_unbraced", "10" } });
    // What the owner's hand test should try first: the same three touch points per ring (a spire this
    // thin leaves no room for braces between three pillars), the tall pillars as walled columns.
    report_variant("recommended", { { "stabilizer_column_shape", "auto" },
                                    { "stabilizer_wall_loops", "2" },
                                    { "stabilizer_infill_density", "15%" } });
}
