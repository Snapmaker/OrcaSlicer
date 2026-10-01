// Side stabilizers (libslic3r/Support/Stabilizers.hpp): rings of pinpoint struts on the sides of a
// tall, thin part, built by the SLA support tree builder and printed as FDM support.
//
// Two halves: the ring placer on its own (where the touch points land), then a whole slice of a
// thin pin, which is the real gate - the struts must reach the part's wall at the ring heights,
// touch it without overlapping it, and stand on the bed.

#include <catch2/catch.hpp>

#include <algorithm>
#include <limits>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Distance from p to the nearest edge of the polygon, mm.
double distance_to_contour(const Polygon &poly, const Vec2d &p)
{
    double best = std::numeric_limits<double>::max();
    for (const Line &l : poly.lines())
        best = std::min(best, unscaled(l.distance_to(Point(scaled(p.x()), scaled(p.y())))));
    return best;
}

ExPolygons square(double size)
{
    const coord_t h = scaled(0.5 * size);
    return { ExPolygon(Polygon({ { -h, -h }, { h, -h }, { h, h }, { -h, h } })) };
}

double total_area(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return unscaled(unscaled(a));
}

struct PinPrint
{
    Print print;
    Model model;
};

// A 6 mm wide, 60 mm tall pin standing on the bed, with supports on and nothing to support - the
// stabilizers are the only thing that can put support next to it.
void slice_pin(PinPrint &p, bool stabilizers)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              "1" },
        { "support_type",                "normal(auto)" },
        { "support_on_build_plate_only", "1" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "stabilizer_supports",         stabilizers ? "1" : "0" },
        { "stabilizer_ring_spacing",     "15" },
        { "stabilizer_points_per_ring",  "3" },
    });
    ModelObject *object = p.model.add_object();
    object->name = "pin";
    object->add_volume(TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.)));
    object->add_instance();
    object->ensure_on_bed();
    p.print.auto_assign_extruders(object);
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    p.print.process();
}

// Area of the stabilizer islands on the support layer at `print_z`, and how close they come to
// the part's outline on the object layer at the same height.
struct LayerCheck
{
    double area      = 0.;  // mm^2
    double overlap   = 0.;  // mm^2 of support inside the part
    double min_gap   = std::numeric_limits<double>::max(); // mm, island edge to part outline
    size_t paths     = 0;   // extrusion entities on that support layer
};

LayerCheck check_layer(const PrintObject &po, double print_z)
{
    LayerCheck out;
    const Layer *layer = nullptr;
    for (const Layer *l : po.layers())
        if (std::abs(l->print_z - print_z) < 0.11) { layer = l; break; }
    REQUIRE(layer != nullptr);
    for (const SupportLayer *sl : po.support_layers()) {
        if (std::abs(sl->print_z - layer->print_z) > EPSILON)
            continue;
        for (const ExPolygon &island : sl->support_islands) {
            out.area += unscaled(unscaled(island.area()));
            for (const Point &pt : island.contour.points)
                for (const ExPolygon &part : layer->lslices)
                    out.min_gap = std::min(out.min_gap, distance_to_contour(part.contour, unscaled(pt)));
        }
        out.paths += sl->support_fills.entities.size();
        out.overlap += total_area(intersection_ex(sl->support_islands, layer->lslices));
    }
    return out;
}

} // namespace

TEST_CASE("Stabilizer rings land on the outline, ring by ring", "[Stabilizers]")
{
    // 0.2 mm layers up to 40 mm, the same 6 mm square on every one.
    const ExPolygons pin = square(6.);
    std::vector<stabilizers::LayerOutline> layers;
    for (int i = 1; i <= 200; ++i)
        layers.push_back({ float(0.2 * i - 0.1), &pin });

    stabilizers::RingParams rp;
    rp.ring_spacing    = 10.;
    rp.points_per_ring = 4;
    const sla::SupportPoints pts = stabilizers::ring_points(layers, rp);

    // Rings at 10, 20 and 30 mm; 40 is inside the top margin.
    REQUIRE(pts.size() == 12);
    for (size_t i = 0; i < pts.size(); ++i) {
        const sla::SupportPoint &sp = pts[i];
        CHECK_THAT(double(sp.pos.z()), WithinAbs(10. * double(i / 4 + 1), 0.21));
        CHECK(distance_to_contour(pin.front().contour, sp.pos.head<2>().cast<double>()) < 0.01);
        CHECK_THAT(double(sp.head_front_radius), WithinAbs(0.5 * rp.tip_diameter, 1e-6));
    }
    // The second ring is turned by half a step: its first point is on a corner diagonal, not on a
    // face centre like the first ring's.
    CHECK_THAT(double(pts[0].pos.y()), WithinAbs(0., 1e-3));
    CHECK(std::abs(pts[4].pos.y()) > 1.);
}

TEST_CASE("Stabilizers skip islands wider than the limit", "[Stabilizers]")
{
    const ExPolygons wide = square(30.);
    std::vector<stabilizers::LayerOutline> layers;
    for (int i = 1; i <= 200; ++i)
        layers.push_back({ float(0.2 * i - 0.1), &wide });

    stabilizers::RingParams rp;
    rp.ring_spacing     = 10.;
    rp.max_island_width = 20.;
    CHECK(stabilizers::ring_points(layers, rp).empty());
    rp.max_island_width = 0.;
    CHECK_FALSE(stabilizers::ring_points(layers, rp).empty());
}

TEST_CASE("Stabilizers touch a thin pin at the ring heights and stand on the bed", "[Stabilizers]")
{
    SECTION("off: a pin with nothing to support gets no support at all")
    {
        PinPrint p;
        slice_pin(p, false);
        const PrintObject &po = *p.print.objects().front();
        double total = 0.;
        for (const SupportLayer *sl : po.support_layers())
            total += total_area(sl->support_islands);
        CHECK(total < 0.01);
    }

    SECTION("on: struts reach the wall at each ring and do not cut into it")
    {
        PinPrint p;
        slice_pin(p, true);
        const PrintObject &po = *p.print.objects().front();
        REQUIRE_FALSE(po.support_layers().empty());

        // Support layers stay sorted, and every one of them has extrusions or is a pre-existing
        // empty layer of the regular generator.
        for (size_t i = 1; i < po.support_layers().size(); ++i)
            CHECK(po.support_layers()[i - 1]->print_z < po.support_layers()[i]->print_z);

        // First layer: pillar feet on the bed.
        CHECK(check_layer(po, 0.2).area > 1.);


        for (double ring_z : { 15., 30., 45. }) {
            const LayerCheck at = check_layer(po, ring_z);
            INFO("ring at " << ring_z << " mm: area " << at.area << ", gap " << at.min_gap << ", overlap " << at.overlap);
            CHECK(at.area > 0.1);
            // ...and it is actually printed, not just outlined.
            CHECK(at.paths > 0);
            // The tip ends at the wall: it touches (no visible gap) without printing into it.
            CHECK(at.min_gap < 0.05);
            CHECK(at.overlap < 0.01);
        }
    }
}
