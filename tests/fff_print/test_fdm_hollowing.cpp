// FDM hollowing (libslic3r/FDMHollowing.hpp): the SLA hollower's 3D inward offset, sliced and cut
// out of the part like a negative volume, so a hollowed part prints as an even shell around an
// empty cavity.

#include <catch2/catch.hpp>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

struct CubePrint
{
    Print print;
    Model model;
};

// A 20 mm cube, hollowed or not, with a 3 mm shell.
void slice_cube(CubePrint &p, bool hollow)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "hollow_interior",            hollow ? "1" : "0" },
        { "hollow_shell_thickness",     "3" },
    });
    ModelObject *object = p.model.add_object();
    object->name = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    p.print.auto_assign_extruders(object);
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    p.print.process();
}

// The object layer nearest to print_z.
const Layer &layer_at(const PrintObject &po, double print_z)
{
    const Layer *best = po.layers().front();
    for (const Layer *l : po.layers())
        if (std::abs(l->print_z - print_z) < std::abs(best->print_z - print_z))
            best = l;
    return *best;
}

double hole_area(const Layer &layer)
{
    double a = 0.;
    for (const ExPolygon &e : layer.lslices)
        for (const Polygon &h : e.holes)
            a += std::abs(h.area());
    return unscaled(unscaled(a));
}

} // namespace

TEST_CASE("Hollowing leaves an even shell around an empty cavity", "[Hollowing]")
{
    SECTION("off: a solid cube")
    {
        CubePrint p;
        slice_cube(p, false);
        const PrintObject &po = *p.print.objects().front();
        CHECK(hole_area(layer_at(po, 10.)) < 0.01);
    }

    SECTION("on: a cavity in the middle, solid floor and roof")
    {
        CubePrint p;
        slice_cube(p, true);
        const PrintObject &po = *p.print.objects().front();

        // Mid-height: a 3 mm shell leaves roughly a 14 x 14 mm hole (196 mm^2); the distance-field
        // offset rounds its corners, so a little less.
        const double mid = hole_area(layer_at(po, 10.));
        INFO("cavity area at 10 mm: " << mid);
        CHECK(mid > 150.);
        CHECK(mid < 200.);

        // Inside the 3 mm floor and roof there is no cavity.
        CHECK(hole_area(layer_at(po, 1.)) < 0.01);
        CHECK(hole_area(layer_at(po, 19.)) < 0.01);
        // ...but just past them there is.
        CHECK(hole_area(layer_at(po, 4.)) > 50.);
        CHECK(hole_area(layer_at(po, 16.)) > 50.);
    }
}
