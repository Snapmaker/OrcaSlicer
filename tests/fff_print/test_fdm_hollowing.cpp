// FDM hollowing (libslic3r/FDMHollowing.hpp): the SLA hollower's 3D inward offset, sliced and cut
// out of the part like a negative volume, so a hollowed part prints as an even shell around an
// empty cavity. hollow_interior / hollow_shell_thickness are region settings: every model part is
// hollowed on its own and may override the object's values. A part that can't be hollowed is
// reported as a slicing warning.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/FDMHollowing.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

struct ObjPrint
{
    Print print;
    Model model;
};

DynamicPrintConfig base_config(bool hollow, const char *shell = "3")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "sparse_infill_density",      "15%" },
        { "hollow_interior",            hollow ? "1" : "0" },
        { "hollow_shell_thickness",     shell },
    });
    return config;
}

void process(ObjPrint &p, ModelObject *object, const DynamicPrintConfig &config)
{
    object->add_instance();
    object->ensure_on_bed();
    p.print.auto_assign_extruders(object);
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    p.print.process();
}

// A box of the given size, as one part, hollowed or not.
void slice_box(ObjPrint &p, bool hollow, const char *shell = "3", Vec3d size = Vec3d(20., 20., 20.))
{
    ModelObject *object = p.model.add_object();
    object->name = "cube";
    object->add_volume(make_cube(size.x(), size.y(), size.z()));
    process(p, object, base_config(hollow, shell));
}

// Two 20 mm cubes side by side in ONE object, 10 mm apart: "left" and "right". add_volume()
// centres a volume's mesh, so an offset is where the volume's centre goes.
void slice_pair(ObjPrint &p, const DynamicPrintConfig &config, const std::function<void(ModelVolume &left, ModelVolume &right)> &setup)
{
    ModelObject *object = p.model.add_object();
    object->name = "pair";
    ModelVolume *left = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    left->name = "left";
    left->set_offset(Vec3d(-15., 0., 0.));
    ModelVolume *right = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    right->name = "right";
    right->set_offset(Vec3d(15., 0., 0.));
    setup(*left, *right);
    process(p, object, config);
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

double hole_area(const ExPolygon &e)
{
    double a = 0.;
    for (const Polygon &h : e.holes)
        a += std::abs(h.area());
    return unscaled(unscaled(a));
}

double hole_area(const Layer &layer)
{
    double a = 0.;
    for (const ExPolygon &e : layer.lslices)
        a += hole_area(e);
    return a;
}

// Cavity area at mid-height of the island left (x < 0) or right (x > 0) of the object's centre.
double hole_area_side(const PrintObject &po, bool right)
{
    double a = 0.;
    for (const ExPolygon &e : layer_at(po, 10.).lslices)
        if ((get_extents(e.contour).center().x() > 0) == right)
            a += hole_area(e);
    return a;
}

PrintStateBase::Warning hollowing_warning(const PrintObject &po)
{
    for (const PrintStateBase::Warning &w : po.step_state_with_warnings(posSlice).warnings)
        if (w.message_id == int(PrintStateBase::SlicingHollowingSkipped))
            return w;
    return PrintStateBase::Warning{};
}

} // namespace

TEST_CASE("Hollowing leaves an even shell around an empty cavity", "[Hollowing]")
{
    SECTION("off: a solid cube")
    {
        ObjPrint p;
        slice_box(p, false);
        const PrintObject &po = *p.print.objects().front();
        CHECK(hole_area(layer_at(po, 10.)) < 0.01);
        CHECK(hollowing_warning(po).message.empty());
    }

    SECTION("on: a cavity in the middle, solid floor and roof")
    {
        ObjPrint p;
        slice_box(p, true);
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
        CHECK(hollowing_warning(po).message.empty());
    }
}

// What prints inside the shell is the shell's own infill; nothing at all may be printed in the
// cavity.
TEST_CASE("Hollowing: no infill inside the cavity", "[Hollowing]")
{
    ObjPrint p;
    slice_box(p, true);
    const PrintObject &po = *p.print.objects().front();
    for (double z : { 6., 10., 14. }) {
        const Layer &layer = layer_at(po, z);
        Polygons cavity;
        for (const ExPolygon &e : layer.lslices)
            append(cavity, e.holes);
        REQUIRE(!cavity.empty());
        // A little inside the cavity's outline, so a perimeter running along it does not count.
        ExPolygons inner = offset_ex(union_ex(cavity), -scaled<float>(0.3));
        REQUIRE(!inner.empty());

        size_t points = 0, inside = 0;
        for (const LayerRegion *layerm : layer.regions()) {
            Polylines lines;
            layerm->fills.collect_polylines(lines);
            layerm->perimeters.collect_polylines(lines);
            for (const Polyline &pl : lines)
                for (const Point &pt : pl.points) {
                    ++points;
                    for (const ExPolygon &in : inner)
                        if (in.contains(pt))
                            ++inside;
                }
        }
        INFO("z=" << z << ": " << points << " extrusion points, " << inside << " inside the cavity");
        CHECK(points > 100);
        CHECK(inside == 0);
    }
}

// A part that is too thin for its shell is not hollowed - and the user is told, with a thickness
// that would have worked.
TEST_CASE("Hollowing warns about a part it cannot hollow", "[Hollowing]")
{
    SECTION("a 20 mm cube with a 10 mm shell: suggests a shell that fits")
    {
        ObjPrint p;
        slice_box(p, true, "10");
        const PrintObject &po = *p.print.objects().front();
        CHECK(hole_area(layer_at(po, 10.)) < 0.01);
        const PrintStateBase::Warning w = hollowing_warning(po);
        INFO(w.message);
        REQUIRE(!w.message.empty());
        CHECK(w.message.find("\"cube\"") != std::string::npos);
        CHECK(w.message.find("10 mm shell") != std::string::npos);
        const size_t at = w.message.find("up to about ");
        REQUIRE(at != std::string::npos);
        const double suggested = std::atof(w.message.c_str() + at + 12);
        // The cube is 10 mm deep at its centre and the hollower keeps a 2 mm fillet: about 7.9.
        CHECK(suggested > 6.5);
        CHECK(suggested < 8.);

        // ...and the suggestion does work.
        ObjPrint again;
        slice_box(again, true, std::to_string(suggested).c_str());
        const PrintObject &po2 = *again.print.objects().front();
        CHECK(hole_area(layer_at(po2, 10.)) > 1.);
        CHECK(hollowing_warning(po2).message.empty());
    }

    SECTION("a 4 mm plate: too thin for any shell")
    {
        ObjPrint p;
        slice_box(p, true, "1", Vec3d(30., 30., 4.));
        const PrintObject &po = *p.print.objects().front();
        const PrintStateBase::Warning w = hollowing_warning(po);
        INFO(w.message);
        REQUIRE(!w.message.empty());
        CHECK(w.message.find("too thin") != std::string::npos);
        CHECK(w.level == PrintStateBase::WarningLevel::NON_CRITICAL);
    }

    SECTION("the depth estimate")
    {
        ObjPrint p;
        slice_box(p, false, "3", Vec3d(30., 12., 20.));
        const PrintObject &po = *p.print.objects().front();
        std::vector<ExPolygons> slices;
        std::vector<float>      zs;
        for (const Layer *l : po.layers()) {
            slices.push_back(l->lslices);
            zs.push_back(float(l->slice_z));
        }
        // Half the narrowest side, 6 mm, a little less for the slice planes at half a layer.
        CHECK_THAT(hollowing_depth(slices, zs), Catch::Matchers::WithinAbs(5.95, 0.1));
    }
}

// hollow_interior / hollow_shell_thickness per part.
TEST_CASE("Hollowing per part", "[Hollowing]")
{
    SECTION("the object's setting applies to every part when none overrides it")
    {
        ObjPrint p;
        slice_pair(p, base_config(true), [](ModelVolume &, ModelVolume &) {});
        const PrintObject &po = *p.print.objects().front();
        INFO("left " << hole_area_side(po, false) << ", right " << hole_area_side(po, true));
        CHECK(hole_area_side(po, false) > 150.);
        CHECK(hole_area_side(po, true) > 150.);
    }

    SECTION("two parts in one object, only one hollow")
    {
        ObjPrint p;
        slice_pair(p, base_config(false), [](ModelVolume &, ModelVolume &right) {
            right.config.set_key_value("hollow_interior", new ConfigOptionBool(true));
        });
        const PrintObject &po = *p.print.objects().front();
        INFO("left " << hole_area_side(po, false) << ", right " << hole_area_side(po, true));
        CHECK(hole_area_side(po, false) < 0.01);
        CHECK(hole_area_side(po, true) > 150.);
        CHECK(hole_area_side(po, true) < 200.);
    }

    SECTION("a part turns hollowing off for itself")
    {
        ObjPrint p;
        slice_pair(p, base_config(true), [](ModelVolume &left, ModelVolume &) {
            left.config.set_key_value("hollow_interior", new ConfigOptionBool(false));
        });
        const PrintObject &po = *p.print.objects().front();
        CHECK(hole_area_side(po, false) < 0.01);
        CHECK(hole_area_side(po, true) > 150.);
    }

    SECTION("a part overrides the shell thickness")
    {
        ObjPrint p;
        slice_pair(p, base_config(true), [](ModelVolume &, ModelVolume &right) {
            right.config.set_key_value("hollow_shell_thickness", new ConfigOptionFloat(5.));
        });
        const PrintObject &po = *p.print.objects().front();
        const double left = hole_area_side(po, false), right = hole_area_side(po, true);
        INFO("left " << left << ", right " << right);
        // 3 mm: about 14 x 14; 5 mm: about 10 x 10, a little less for the rounded corners.
        CHECK(left > 150.);
        CHECK(right > 70.);
        CHECK(right < 100.);
    }

    SECTION("a part's warning names the part")
    {
        ObjPrint p;
        slice_pair(p, base_config(true), [](ModelVolume &left, ModelVolume &) {
            left.config.set_key_value("hollow_shell_thickness", new ConfigOptionFloat(12.));
        });
        const PrintObject &po = *p.print.objects().front();
        CHECK(hole_area_side(po, false) < 0.01);
        CHECK(hole_area_side(po, true) > 150.);
        const std::string msg = hollowing_warning(po).message;
        INFO(msg);
        CHECK(msg.find("Part \"left\" of \"pair\"") != std::string::npos);
        CHECK(msg.find("right") == std::string::npos);
    }

    SECTION("modifiers and negative volumes are never hollowed")
    {
        ObjPrint p;
        slice_pair(p, base_config(false), [](ModelVolume &left, ModelVolume &right) {
            // A modifier asking for hollowing covers the whole left cube...
            ModelObject *object = left.get_object();
            ModelVolume *mod = object->add_volume(TriangleMesh(its_make_cube(22., 22., 22.)));
            mod->set_type(ModelVolumeType::PARAMETER_MODIFIER);
            mod->set_offset(Vec3d(-15., 0., 0.));
            mod->config.set_key_value("hollow_interior", new ConfigOptionBool(true));
            // ...and a hollowed negative volume sits in the right one.
            ModelVolume *neg = object->add_volume(TriangleMesh(its_make_cube(16., 16., 16.)));
            neg->set_type(ModelVolumeType::NEGATIVE_VOLUME);
            neg->set_offset(Vec3d(15., 0., 0.));
            neg->config.set_key_value("hollow_interior", new ConfigOptionBool(true));
            (void)right;
        });
        const PrintObject &po = *p.print.objects().front();
        // The modifier's region gets hollow_interior, but the part is what is hollowed or not.
        CHECK(hole_area_side(po, false) < 0.01);
        // The negative volume cuts its plain 16 x 16 box, nothing more.
        CHECK_THAT(hole_area_side(po, true), Catch::Matchers::WithinAbs(256., 3.));
        CHECK(hollowing_warning(po).message.empty());
    }
}
