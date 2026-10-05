// Ultra (over-support surfaces) - the gate for docs/superpowers/specs/2026-09-05-over-support-surfaces.md.
//
// Two halves, and the first one is the important one:
//
//  * OFF mode is the hard gate. With over_support_surfaces off, the classifier must not run at
//    all: the same object must still emit only bridge roles for the faces that land on support,
//    and the string "Bottom surface over support" must not appear anywhere in the G-code.
//  * ON mode: the faces that land on support carry the new role, their feedrate follows
//    over_support_speed (0 = the outer wall speed of the layer), their extrusion per millimetre
//    scales with over_support_flow, and a face that the generator will NOT support - blocked, or
//    under a support type that refuses to support bridges - stays a true bridge.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
// tests/CLAUDE.md: floating point comparisons go through the matchers, never through Approx.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const std::string kOverSupportRole = "Bottom surface over support";
const std::string kBridgeRole      = "Bridge";

// The feature-type marker is ";TYPE:<role>" on a generic printer and "; FEATURE: <role>" on a BBL
// one (GCodeProcessor::Reserved_Tags vs Reserved_Tags_compatible, picked by s_IsBBLPrinter). Return
// the role name for either, and an empty string for anything else.
std::string role_marker(const std::string &line)
{
    static const std::string generic = ";TYPE:";
    static const std::string bbl     = "; FEATURE: ";
    std::string              name;
    if (line.rfind(bbl, 0) == 0)
        name = line.substr(bbl.size());
    else if (line.rfind(generic, 0) == 0)
        name = line.substr(generic.size());
    else
        return std::string();
    while (! name.empty() && (name.back() == '\r' || name.back() == ' '))
        name.pop_back();
    return name;
}

// A 2x2x10 leg on the bed carrying a 20x20x4 slab 10 mm above it, one volume. The slab's whole
// underside hangs over air, so with automatic supports on the generator fills that gap - which
// makes the underside the exact surface this feature is about. A wide flat slab on purpose: the
// area has to be big enough to survive the classifier's opening and produce measurable extrusion.
TriangleMesh leg_and_slab()
{
    TriangleMesh mesh = Slic3r::make_cube(2., 2., 10.);
    TriangleMesh slab = Slic3r::make_cube(20., 20., 4.);
    slab.translate(0.f, 0.f, 10.f);
    mesh.merge(slab);
    return mesh;
}

DynamicPrintConfig base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                  "1" },
        { "support_type",                    "normal(auto)" },
        { "support_style",                   "grid" },
        { "support_top_z_distance",          "0.2" },
        { "support_interface_top_layers",    "2" },
        { "support_interface_bottom_layers", "2" },
        { "support_interface_spacing",       "0.5" },
        { "support_on_build_plate_only",     "0" },
        // The generator must be willing to put support under the slab, or there would be nothing
        // here to reclassify and every ON-mode case below would pass for the wrong reason.
        { "bridge_no_support",               "0" },
        // Keep the numbers comparable across the cases below. The cooling slowdown scales every
        // feedrate on a small, fast layer - the slab is exactly that - so it has to be off before
        // any F in the G-code can be compared against a setting.
        { "outer_wall_speed",                "40" },
        { "bridge_speed",                    "17" },
        { "slow_down_layer_time",            "0" },
        { "slow_down_layers",                "0" },
        { "reduce_fan_stop_start_freq",      "0" },
        // GCode::_extrude caps every speed at filament_max_volumetric_speed / mm3_per_mm. The
        // default is low enough to cap 40 mm/s on this line width, which would make the F values
        // say something about the filament rather than about the setting under test.
        { "filament_max_volumetric_speed",   "200" },
    });
    return config;
}

void add_leg_and_slab(Slic3r::Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "leg_and_slab";
    object->add_volume(leg_and_slab());
    object->add_instance();
    object->ensure_on_bed();
}

std::string slice_leg_and_slab(const DynamicPrintConfig &config)
{
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_slab(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    // Both slicing calls in this file are wrapped so a failure says which half of the pipeline
    // threw. It is worth knowing that this wrapper is not only cosmetic: on the Stage 5 build the
    // UNwrapped call threw ClipperLib "Coordinate outside allowed range" out of print.process()
    // for the supports-off case below, deterministically, while an inline copy of exactly this
    // sequence in the same file did not - a codegen artefact of a Release build with LTCG, not a
    // behaviour change (nothing in the support-group or over-support paths runs on an object with
    // supports off, and the corpus's own no_support case is within tolerance against the
    // baseline). Recorded in 2e of docs/superpowers/plans/2026-09-02-support-sets-and-groups.md.
    try { print.process(); } catch (const std::exception &e) { throw std::runtime_error(std::string("PROCESS: ") + e.what()); }

    // Not Slic3r::Test::gcode(): that helper exports to a bare filename, and this fork's
    // GCode::_do_export creates the output's parent directory when it is missing - with a bare
    // filename the parent is "", and create_directory("") throws. It also passes a null
    // GCodeProcessorResult, which Print::export_gcode dereferences unconditionally on its last
    // line. Both are pre-existing; test_printgcode fails on the first of them on this tree.
    const boost::filesystem::path out =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("over_support_%%%%%%%%.gcode");
    GCodeProcessorResult result;
    try { print.export_gcode(out.string(), &result, nullptr); } catch (const std::exception &e) { throw std::runtime_error(std::string("EXPORT: ") + e.what()); }
    std::ifstream in(out.string());
    std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::filesystem::remove(out);
    return text;
}

// Every feature type name that appears in the G-code, in first-seen order.
std::vector<std::string> feature_types(const std::string &gcode)
{
    std::vector<std::string> out;
    std::size_t              line_begin = 0;
    while (line_begin <= gcode.size()) {
        std::size_t line_end = gcode.find('\n', line_begin);
        if (line_end == std::string::npos)
            line_end = gcode.size();
        const std::string name = role_marker(gcode.substr(line_begin, line_end - line_begin));
        line_begin = line_end + 1;
        if (! name.empty() && std::find(out.begin(), out.end(), name) == out.end())
            out.emplace_back(name);
    }
    return out;
}

struct BlockStats
{
    double              e_total  = 0.;   // extruded filament, mm
    double              xy_total = 0.;   // travelled distance while extruding, mm
    unsigned            segments = 0;
    std::vector<double> feedrates;       // every F seen inside the blocks, mm/min
    // Where on the plate those moves were. The per-part case below is decided by these: a
    // classification that stopped at the part boundary leaves two X ranges that do not overlap.
    double              x_min    =  1e30;
    double              x_max    = -1e30;
};

// Walk the G-code and accumulate the extruding moves that belong to the blocks introduced by
// `role_name`, i.e. from that feature-type marker until the next one. Absolute E only, which is
// what the test config produces.
BlockStats stats_for_type(const std::string &gcode, const std::string &role_name,
                          double x_lo = -1e30, double x_hi = 1e30)
{
    BlockStats stats;
    bool       inside = false;
    double     x = 0., y = 0., e = 0.;
    bool       have_pos = false;
    double     feed = 0.;

    std::size_t line_begin = 0;
    while (line_begin <= gcode.size()) {
        std::size_t line_end = gcode.find('\n', line_begin);
        if (line_end == std::string::npos)
            line_end = gcode.size();
        std::string line = gcode.substr(line_begin, line_end - line_begin);
        line_begin       = line_end + 1;
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        if (const std::string marker = role_marker(line); ! marker.empty()) {
            inside = (marker == role_name);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;

        // Parse the words this test cares about. A missing word means unchanged.
        double nx = x, ny = y, ne = e, nf = feed;
        bool   has_x = false, has_y = false, has_e = false, has_f = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c != 'X' && c != 'Y' && c != 'E' && c != 'F')
                continue;
            double v = 0.;
            try {
                v = std::stod(line.substr(i + 1));
            } catch (...) {
                continue;
            }
            switch (c) {
            case 'X': nx = v; has_x = true; break;
            case 'Y': ny = v; has_y = true; break;
            case 'E': ne = v; has_e = true; break;
            case 'F': nf = v; has_f = true; break;
            default: break;
            }
        }

        const double x_mid = 0.5 * (x + nx);
        if (inside && have_pos && has_e && ne > e && (has_x || has_y) && x_mid >= x_lo && x_mid < x_hi) {
            const double d = std::hypot(nx - x, ny - y);
            if (d > 1e-6) {
                stats.e_total += ne - e;
                stats.xy_total += d;
                ++stats.segments;
                // Only the feedrate of an EXTRUDING move. A block also carries travels (F7200)
                // and the retract/wipe (F1800), which say nothing about the print speed. nf
                // carries the last F forward, so a move that sets no F of its own still counts.
                stats.feedrates.push_back(nf);
                stats.x_min = std::min(stats.x_min, std::min(x, nx));
                stats.x_max = std::max(stats.x_max, std::max(x, nx));
            }
        }
        x = nx; y = ny; feed = nf;
        if (has_e)
            e = ne;
        if (has_x || has_y)
            have_pos = true;
    }
    return stats;
}

// The surface types the object's fill surfaces carry, straight out of the slicing pipeline.
bool object_has_surface_type(const DynamicPrintConfig &config, SurfaceType type)
{
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_slab(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    REQUIRE(! print.objects().empty());
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == type)
                    return true;
    return false;
}

// A leg on the bed carrying TWO 20x20x4 slabs 10 mm above it, 40 mm apart in X, as three
// MODEL_PART volumes of one object. Both slabs' undersides hang over air, so both are bottom
// bridges the generator will support - and they are far enough apart that an X coordinate says
// which slab a move belongs to.
void add_leg_and_two_slabs(Slic3r::Model &model, const std::function<void(ModelObject &)> &tweak)
{
    ModelObject *object = model.add_object();
    object->name = "leg_and_two_slabs";
    object->add_volume(Slic3r::make_cube(2., 2., 10.));       // the leg, on the bed
    TriangleMesh a = Slic3r::make_cube(20., 20., 4.);
    a.translate(0.f, 0.f, 10.f);
    object->add_volume(a);                                    // part A, floating
    TriangleMesh b = Slic3r::make_cube(20., 20., 4.);
    b.translate(40.f, 0.f, 10.f);
    object->add_volume(b);                                    // part B, floating, larger X
    object->add_instance();
    if (tweak)
        tweak(*object);
    object->ensure_on_bed();
}

std::string slice_two_slabs(const DynamicPrintConfig &config,
                            const std::function<void(ModelObject &)> &tweak)
{
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_two_slabs(model, tweak);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const boost::filesystem::path out =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("over_support_%%%%%%%%.gcode");
    GCodeProcessorResult result;
    try { print.export_gcode(out.string(), &result, nullptr); } catch (const std::exception &e) { throw std::runtime_error(std::string("EXPORT: ") + e.what()); }
    std::ifstream in(out.string());
    std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::filesystem::remove(out);
    return text;
}

} // namespace

// ------------------------------------------------------------------ OFF mode: the hard gate

TEST_CASE("over_support: with the switch off the roles are exactly today's", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    // The default, spelled out: this case must behave like a build that never heard of the feature.
    config.set_deserialize_strict({ { "over_support_surfaces", "0" } });

    const std::string              gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    // The slab's underside is over support and is still called a bridge - today's behaviour.
    CHECK(std::find(types.begin(), types.end(), kBridgeRole) != types.end());
    // And the new role never appears, not as a feature type and not anywhere else.
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK(std::find(types.begin(), types.end(), kOverSupportRole) == types.end());

    // Nothing produced the new surface type either.
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: the switch is off by default", "[OverSupport]")
{
    // PrintRegionConfig, not PrintObjectConfig: Stage 5 of the support-sets plan moved the three
    // keys there so a PART can carry them. That move is the whole per-part mechanism, so asserting
    // where they live is asserting that a part-level value can act at all.
    PrintRegionConfig defaults;
    CHECK_FALSE(defaults.over_support_surfaces.value);
    CHECK_THAT(defaults.over_support_flow.value, WithinAbs(1., 1e-9));
    CHECK_THAT(defaults.over_support_speed.value, WithinAbs(0., 1e-9));
}

// -------------------------------------------------------------------------------- ON mode

TEST_CASE("over_support: with the switch on the supported bottoms carry the new role", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" } });

    const std::string              gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    REQUIRE(std::find(types.begin(), types.end(), kOverSupportRole) != types.end());
    CHECK(object_has_surface_type(config, stBottomOverSupport));

    // The role has real extrusion behind it, not a stray marker.
    const BlockStats stats = stats_for_type(gcode, kOverSupportRole);
    CHECK(stats.segments > 20);
    CHECK(stats.xy_total > 50.);
}

TEST_CASE("over_support: speed 0 follows the outer wall speed, a set value is used as is", "[OverSupport]")
{
    // 0 = match the walls around the surface.
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "0" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(! stats.feedrates.empty());
        // outer_wall_speed 40 mm/s -> 2400 mm/min. The slab starts 10 mm up, so neither the first
        // layer nor the slow-down-for-first-layers ramp reaches it.
        for (double f : stats.feedrates)
            CHECK_THAT(f, WithinAbs(40. * 60., 1.));
    }
    // A set value is used as it is - and it is emphatically not bridge_speed (17 mm/s here).
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "23" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(! stats.feedrates.empty());
        for (double f : stats.feedrates)
            CHECK_THAT(f, WithinAbs(23. * 60., 1.));
    }
}

TEST_CASE("over_support: the flow ratio scales the extrusion per millimetre", "[OverSupport]")
{
    auto e_per_mm = [](const char *flow) {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" },
                                        { "over_support_speed", "20" },
                                        { "over_support_flow", flow } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(stats.xy_total > 50.);
        return stats.e_total / stats.xy_total;
    };

    const double full = e_per_mm("1");
    const double half = e_per_mm("0.5");
    REQUIRE(full > 0.);
    // The line geometry is identical either way, so the ratio is the ratio of the two settings.
    CHECK_THAT(half / full, WithinRel(0.5, 0.02));
}

// ------------------------------------------------------------- what stays a true bridge

TEST_CASE("over_support: nothing is reclassified when the generator will not support the bridge", "[OverSupport]")
{
    // bridge_no_support tells the normal generator to leave bridges unsupported, so the slab's
    // underside really is a bridge and has to keep the bridge settings.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "bridge_no_support", "1" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: the tree generators refuse bridges each in their own way", "[OverSupport]")
{
    // Read off the three remove_bridges_from_contacts call sites (PrintObject::over_support_settings):
    // the ORGANIC tree drops every bridge under bridge_no_support, length unmeasured, so the feature
    // stands down; the CLASSIC tree ignores bridge_no_support and drops only bridges shorter than
    // max_bridge_length in both directions - the 20 mm slab is longer than the 10 mm default, so it
    // is supported and reclassified, and a 30 mm limit makes it bridgeable again.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "support_type", "tree(auto)" } });

    SECTION("organic (the default tree style) stands down under bridge_no_support") {
        config.set_deserialize_strict({ { "support_style", "default" }, { "bridge_no_support", "1" } });
        CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
        config.set_deserialize_strict({ { "support_style", "organic" } });
        CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
    }
    SECTION("organic reclassifies with bridge_no_support off") {
        config.set_deserialize_strict({ { "support_style", "organic" }, { "bridge_no_support", "0" }, { "max_bridge_length", "10" } });
        CHECK(object_has_surface_type(config, stBottomOverSupport));
    }
    SECTION("classic measures against max_bridge_length and ignores bridge_no_support") {
        config.set_deserialize_strict({ { "support_style", "tree_slim" }, { "bridge_no_support", "1" }, { "max_bridge_length", "10" } });
        CHECK(object_has_surface_type(config, stBottomOverSupport));
        config.set_deserialize_strict({ { "max_bridge_length", "30" } });
        CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
    }
}

TEST_CASE("over_support: with supports off every bottom stays a bridge", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "enable_support", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: a zero top Z distance is left to the soluble path", "[OverSupport]")
{
    // At a zero gap the bottom is already stBottom (the soluble-interface rule), so there is
    // nothing for this feature to reclassify and it must stay out of the way.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "support_top_z_distance", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

// -------------------------------------------------------------------- keys and plumbing

TEST_CASE("over_support: the three keys are support-set and part eligible", "[OverSupport]")
{
    const std::vector<std::string> &part = Slic3r::part_support_keys();
    for (const char *key : { "over_support_surfaces", "over_support_flow", "over_support_speed" })
        CHECK(std::find(part.begin(), part.end(), std::string(key)) != part.end());
}

TEST_CASE("over_support: the role has a name and survives the round trip", "[OverSupport]")
{
    const std::string name = ExtrusionEntity::role_to_string(erBottomSurfaceOverSupport);
    CHECK(name == kOverSupportRole);
    CHECK(ExtrusionEntity::string_to_role(name) == erBottomSurfaceOverSupport);
    // The plain bottom surface must not be swallowed by the longer name.
    CHECK(ExtrusionEntity::string_to_role("Bottom surface") == erBottomSurface);
    // It is solid infill, and it is emphatically not a bridge.
    CHECK(is_solid_infill(erBottomSurfaceOverSupport));
    CHECK_FALSE(is_bridge(erBottomSurfaceOverSupport));
}

// ---------------------------------------------------------------- PER PART (support-sets Stage 5)

TEST_CASE("over_support: the switch follows the PART, not the object", "[OverSupport][support_groups]")
{
    // The object leaves the feature off. Only part B - the slab at the larger X - asks for it, the
    // way a support group writes its values onto its own parts. B's underside must be reclassified
    // and A's must stay a bridge, and the two must not overlap on the plate: a per-part value that
    // leaked would show up as one X range inside the other.
    DynamicPrintConfig config = base_config();
    const std::string gcode = slice_two_slabs(config, [](ModelObject &object) {
        ModelVolume *part_b = object.volumes.back();
        part_b->config.set_key_value("over_support_surfaces", new ConfigOptionBool(true));
        part_b->config.set_key_value("over_support_speed",    new ConfigOptionFloat(23.));
    });

    const std::vector<std::string> types = feature_types(gcode);
    REQUIRE(std::find(types.begin(), types.end(), kOverSupportRole) != types.end());
    REQUIRE(std::find(types.begin(), types.end(), kBridgeRole) != types.end());

    const BlockStats over   = stats_for_type(gcode, kOverSupportRole);
    const BlockStats bridge = stats_for_type(gcode, kBridgeRole);
    REQUIRE(over.segments > 0);
    REQUIRE(bridge.segments > 0);
    // Part B is the one at the larger X, so its over-support surface starts where the bridges end.
    CHECK(over.x_min > bridge.x_max);

    // ...and the speed came from the PART too: 23 mm/s is 1380 mm/min, and nothing else on this
    // plate is printed at it.
    for (double f : over.feedrates)
        CHECK_THAT(f, WithinAbs(23. * 60., 1e-6));
}

TEST_CASE("over_support: a part-level flow ratio scales that part's extrusion",
          "[OverSupport][support_groups]")
{
    // The object leaves the feature off and part B turns it on, which is how a support group uses
    // these keys: the group writes them onto its own parts. B's flow ratio then has to reach the
    // G-code - the same run with the ratio doubled must extrude about twice as much per millimetre
    // over that part, and part A must still be printing bridges. The OBJECT-wide switch on with
    // both parts producing surfaces is the next case.
    DynamicPrintConfig config = base_config();

    auto per_mm = [&config](double flow) {
        const std::string gcode = slice_two_slabs(config, [flow](ModelObject &object) {
            ModelVolume *part_b = object.volumes.back();
            part_b->config.set_key_value("over_support_surfaces", new ConfigOptionBool(true));
            part_b->config.set_key_value("over_support_speed",    new ConfigOptionFloat(20.));
            part_b->config.set_key_value("over_support_flow",     new ConfigOptionFloat(flow));
        });
        const BlockStats over = stats_for_type(gcode, kOverSupportRole);
        REQUIRE(over.segments > 0);
        REQUIRE(over.xy_total > 0.);
        // The neighbour is untouched: it never asked for the feature, so it is still a bridge.
        const BlockStats bridge = stats_for_type(gcode, kBridgeRole);
        REQUIRE(bridge.segments > 0);
        CHECK(over.x_min > bridge.x_max);
        return over.e_total / over.xy_total;
    };

    const double one = per_mm(1.0);
    const double two = per_mm(2.0);
    CHECK(two > one * 1.7);
    CHECK(two < one * 2.3);
}

TEST_CASE("over_support: with the OBJECT switch on a part still keeps its own flow and speed",
          "[OverSupport][support_groups]")
{
    // The OBJECT turns the feature on, so BOTH slabs produce over-support surfaces, and part B
    // carries its own speed and flow ratio. Before the fix both slabs came out at the object's F
    // and the object's extrusion per millimetre although printing_region(1).config() carried B's
    // values: Layer::make_fills groups fill surfaces across regions by SurfaceFillParams, the two
    // over-support keys were not part of those params, so the two slabs' identical-looking
    // surfaces were merged into ONE fill attributed to the first region, and
    // GCode::extrude_infill applied that region's config to the lot. The keys are in the params
    // now; this asserts the two slabs really are extruded apart, by X window.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" },
                                    { "over_support_speed",    "20" },
                                    { "over_support_flow",     "1" } });
    const std::string gcode = slice_two_slabs(config, [](ModelObject &object) {
        ModelVolume *part_b = object.volumes.back();
        part_b->config.set_key_value("over_support_speed", new ConfigOptionFloat(37.));
        part_b->config.set_key_value("over_support_flow",  new ConfigOptionFloat(2.));
    });

    // Nothing on this plate is a bridge any more: both undersides are over support.
    const std::vector<std::string> types = feature_types(gcode);
    REQUIRE(std::find(types.begin(), types.end(), kOverSupportRole) != types.end());
    CHECK(std::find(types.begin(), types.end(), kBridgeRole) == types.end());

    // The slabs are 20 mm wide and 40 mm apart in X, so the midpoint of the role's X range falls
    // in the gap between them whatever the plate offset is.
    const BlockStats all = stats_for_type(gcode, kOverSupportRole);
    REQUIRE(all.segments > 0);
    const double x_split = 0.5 * (all.x_min + all.x_max);
    const BlockStats a = stats_for_type(gcode, kOverSupportRole, -1e30, x_split);
    const BlockStats b = stats_for_type(gcode, kOverSupportRole, x_split, 1e30);
    REQUIRE(a.segments > 0);
    REQUIRE(b.segments > 0);
    REQUIRE(a.xy_total > 0.);
    REQUIRE(b.xy_total > 0.);

    // Part A prints at the OBJECT's 20 mm/s, part B at its OWN 37 mm/s...
    for (double f : a.feedrates)
        CHECK_THAT(f, WithinAbs(20. * 60., 1e-6));
    for (double f : b.feedrates)
        CHECK_THAT(f, WithinAbs(37. * 60., 1e-6));
    // ...and B's flow ratio of 2 doubles its extrusion per millimetre against A's.
    const double per_mm_a = a.e_total / a.xy_total;
    const double per_mm_b = b.e_total / b.xy_total;
    CHECK(per_mm_b > per_mm_a * 1.7);
    CHECK(per_mm_b < per_mm_a * 2.3);
}

TEST_CASE("over_support: the keys are region members, so a part gets its own region",
          "[OverSupport][support_groups]")
{
    // The mechanism, asserted directly: a volume carrying one of the three keys is enough to give
    // that volume a PrintRegion of its own, which - together with the two keys being part of
    // SurfaceFillParams, so its fills are not merged into a neighbour's - is what makes
    // GCode::extrude_infill apply the part's flow and speed rather than the object's.
    DynamicPrintConfig config = base_config();
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_two_slabs(model, [](ModelObject &object) {
        object.volumes.back()->config.set_key_value("over_support_flow", new ConfigOptionFloat(1.5));
    });
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    REQUIRE(! print.objects().empty());
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.num_printing_regions() >= 2);
    bool found = false;
    for (size_t i = 0; i < object.num_printing_regions(); ++ i)
        if (std::abs(object.printing_region(i).config().over_support_flow.value - 1.5) < 1e-9)
            found = true;
    CHECK(found);
}

// ------------------------------------------------------------------ Orca #15945: fan-aware overhang split
// Adapted from OrcaSlicer tests/fff_print/test_extrusion_processor.cpp. Edge has the older
// non-templated ExtrusionProcessor (no interior sampling); these cases exercise the split and
// fan logic only.

namespace {

constexpr double caged_wall_width = 0.42; // mm, outer wall line width

// A wall along a supported edge of the previous layer, ending past or just short of the edge's end.
// Crossing the edge's end reads half a line width out.
constexpr double edge_run_length = 64.;   // mm, wall start, measured from the end of the previous layer's edge
constexpr double edge_step       = 0.384; // mm, how far this layer's contour extends past the previous layer's end
constexpr double edge_wall_end_past  = edge_step - 0.5 * caged_wall_width;
constexpr double edge_wall_end_short = 0.05; // mm short of the edge, reading 0.21 - 0.05 = 0.16mm out
// Segmentation splits 1.5 line widths plus the end's reading from an end, so an end's slowdown and cooling stay within this.
constexpr double edge_affected_length = 3. * caged_wall_width;

std::vector<ExtendedPoint> sampled_wall_along_edge(double                              wall_end_x,
                                                   const std::function<float(float)>  &distance_to_speed,
                                                   float                               min_distance,
                                                   float                               fan_overlap_threshold)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {edge_run_length + 10., 0.}},
        {{edge_run_length + 10., 0.}, {edge_run_length + 10., -10.}},
        {{edge_run_length + 10., -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const double wall_y = -0.5 * caged_wall_width;
    const Points wall{Point::new_scale(edge_run_length, wall_y), Point::new_scale(wall_end_x, wall_y)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f, min_distance,
                                                              distance_to_speed, fan_overlap_threshold);
}

// How much of a path is printed below the speed a fully supported reading gives. A segment is printed
// at the lower of the speeds its ends read.
double slowed_length(const std::vector<ExtendedPoint> &points, const std::function<float(float)> &distance_to_speed)
{
    double length = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (std::min(distance_to_speed(points[i].distance), distance_to_speed(points[i + 1].distance)) < distance_to_speed(0.f))
            length += (points[i + 1].position - points[i].position).norm();
    return length;
}

// Length printed with the overhang fan on: segments with either end's overlap at or below the threshold.
double cooled_length(const std::vector<ExtendedPoint> &points, float fan_overlap_threshold)
{
    double length = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (1.f - std::max(points[i].distance, points[i + 1].distance) / float(caged_wall_width) <= fan_overlap_threshold)
            length += (points[i + 1].position - points[i].position).norm();
    return length;
}

} // namespace

// Regression: the line up to a step past the previous layer was not split, so the step's slowdown and cooling covered the
// whole wall. The split required an end reading beyond where the slowdown begins, and an edge crossing reads exactly
// there when the wall speed is held below the reference speed (e.g. resonance avoidance).
TEST_CASE("A wall stepping past the previous layer is slowed and cooled only beside the step", "[ExtrusionProcessor][Regression]")
{
    const float crossing_reading = 0.5f * float(caged_wall_width);
    const std::function<float(float)> distance_to_speed = [crossing_reading](float distance) {
        return distance < crossing_reading ? 70.f : 15.f;
    };
    const float fan_overlap_threshold = 0.75f; // The fan switches on at a 25% overhang

    const std::vector<ExtendedPoint> points = sampled_wall_along_edge(-edge_wall_end_past, distance_to_speed, crossing_reading,
                                                                      fan_overlap_threshold);
    const double slowed = slowed_length(points, distance_to_speed);
    const double cooled = cooled_length(points, fan_overlap_threshold);

    REQUIRE(slowed > 0.);
    REQUIRE(cooled > 0.);
    REQUIRE(slowed < edge_affected_length);
    REQUIRE(cooled < edge_affected_length);
}

// Regression: the fan can switch on at a smaller overhang than the first slowdown. Splitting only on speed changes left
// the whole wall cooled when its end read between the two.
TEST_CASE("A wall is split where only the overhang fan changes", "[ExtrusionProcessor][Regression]")
{
    const float crossing_reading = 0.5f * float(caged_wall_width);
    const std::function<float(float)> distance_to_speed = [crossing_reading](float distance) {
        return distance < crossing_reading ? 70.f : 15.f;
    };
    // The end reads 0.16mm out (overlap 0.62): cooled at a 25% threshold, but not slowed.
    const float fan_overlap_threshold = 0.75f;

    const std::vector<ExtendedPoint> points = sampled_wall_along_edge(edge_wall_end_short, distance_to_speed, crossing_reading,
                                                                      fan_overlap_threshold);
    const double cooled = cooled_length(points, fan_overlap_threshold);

    REQUIRE_THAT(slowed_length(points, distance_to_speed), WithinAbs(0., 1e-9));
    REQUIRE(cooled > 0.);
    REQUIRE(cooled < edge_affected_length);
}

// With one speed and a fan threshold no reading reaches, only the wall's ends and the edge crossing remain.
TEST_CASE("A wall is left whole where neither its speed nor its cooling changes", "[ExtrusionProcessor]")
{
    const std::function<float(float)> distance_to_speed = [](float) { return 70.f; };
    // 95% overhang; the step reads 0.384mm out (overlap 0.09).
    const float fan_overlap_threshold = 0.05f;

    const std::vector<ExtendedPoint> points = sampled_wall_along_edge(-edge_wall_end_past, distance_to_speed, -1.f,
                                                                      fan_overlap_threshold);

    REQUIRE(points.size() == 3);
}

// Bridges, overhang perimeters, and enable_overhang_bridge_fan off leave fan_overlap_threshold at -1,
// so an end that would otherwise cool (same geometry as the fan-only split case) must not fan-split
// the wall. Speed is constant, and this fixture has no previous-layer edge crossing, so size stays 2.
TEST_CASE("A wall is not fan-split when the overhang fan does not depend on overlap", "[ExtrusionProcessor]")
{
    const std::function<float(float)> distance_to_speed = [](float) { return 70.f; };
    const float fan_overlap_threshold = -1.f;

    const std::vector<ExtendedPoint> points = sampled_wall_along_edge(edge_wall_end_short, distance_to_speed, -1.f,
                                                                      fan_overlap_threshold);

    REQUIRE(points.size() == 2);
}

namespace {

using Walls = std::vector<std::vector<ProcessedPoint>>;

Walls estimate_walls(ExtrusionQualityEstimator &estimator, const PrintObject *object, const Layer &layer)
{
    const ConfigOptionPercents         overlaps({90, 75, 50, 25, 13, 0});
    const ConfigOptionFloatsOrPercents speeds({FloatOrPercent{100, true}, FloatOrPercent{50, true}, FloatOrPercent{30, true},
                                               FloatOrPercent{20, true}, FloatOrPercent{10, true}, FloatOrPercent{5, true}});
    Walls walls;
    estimator.set_current_object(object);
    for (const LayerRegion *region : layer.regions())
        for_each_extrusion_path(region->perimeters, [&](const ExtrusionPath &path) {
            if (is_perimeter(path.role()))
                walls.push_back(estimator.estimate_extrusion_quality(path, overlaps, speeds, 60.f, 60.f, true, 0.5f));
        });
    return walls;
}

uint32_t float_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void check_identical_walls(const Walls &actual, const Walls &expected)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t wall = 0; wall < actual.size(); ++wall) {
        INFO("wall " << wall);
        REQUIRE(actual[wall].size() == expected[wall].size());
        for (size_t i = 0; i < actual[wall].size(); ++i) {
            const ProcessedPoint &a = actual[wall][i];
            const ProcessedPoint &e = expected[wall][i];
            INFO("point " << i << ": speed " << a.speed << " vs " << e.speed << ", overlap " << a.overlap << " vs " << e.overlap);
            CHECK(a.p == e.p);
            CHECK(float_bits(a.speed) == float_bits(e.speed));
            CHECK(float_bits(a.overlap) == float_bits(e.overlap));
        }
    }
}

const Layer *first_overhang_layer(const PrintObject &object)
{
    for (const Layer *layer : object.layers()) {
        if (layer == nullptr || layer->lower_layer == nullptr || !layer->has_extrusions())
            continue;
        return layer;
    }
    return nullptr;
}

bool has_curled_lines(const PrintObject &object)
{
    const auto layers = object.layers();
    return std::any_of(layers.begin(), layers.end(), [](const Layer *layer) { return !layer->curled_lines.empty(); });
}

// A 40 x 20 x 20 mm box with a 45 degree overhang cut into the y = 0 side. The sloped face is caged
// by full-height walls so the estimator sees unsupported span between supported ends.
TriangleMesh caged_overhang_mesh()
{
    return TriangleMesh(
        {
            {5.0859987f, 10.167065f, 5.711731f},
            {34.914257f, 10.167065f, 5.711731f},
            {34.914257f, 0.f, 15.878796f},
            {5.0859995f, 0.f, 15.878796f},
            {0.f, 0.f, 0.f},
            {0.f, 0.f, 20.f},
            {0.f, 20.f, 20.f},
            {0.f, 20.f, 0.f},
            {40.f, 20.f, 20.f},
            {40.f, 20.f, 0.f},
            {40.f, 0.f, 20.f},
            {40.f, 0.f, 0.f},
            {34.914257f, 0.f, 0.f},
            {5.0859995f, 0.f, 0.f},
            {34.914257f, 10.167065f, 0.f},
            {5.0859995f, 10.167065f, 0.f},
        },
        {
            {0, 1, 2},   {0, 2, 3},    {4, 5, 6},   {4, 6, 7},     {7, 6, 8},    {7, 8, 9},    {9, 8, 10},  {9, 10, 11},
            {12, 11, 10}, {5, 4, 13},  {5, 13, 3},  {2, 12, 10},   {5, 3, 2},    {10, 5, 2},   {9, 11, 12}, {9, 12, 14},
            {13, 4, 7},   {9, 14, 15}, {15, 13, 7}, {7, 9, 15},    {8, 6, 5},    {8, 5, 10},   {14, 1, 0},  {14, 0, 15},
            {2, 1, 14},   {2, 14, 12}, {15, 0, 3},  {15, 3, 13},
        });
}

DynamicPrintConfig overhang_curled_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "1"},
        {"layer_height", "0.2"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        {"printable_area", "0x0,400x0,400x400,0x400"},
    });
    return config;
}

} // namespace

TEST_CASE("Overhang data computed ahead of the generator gives the same wall speeds", "[ExtrusionProcessor]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "1"},
        {"layer_height", "0.2"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
    });
    Print print;
    init_and_process_print({TestMesh::overhang}, print, config);
    const PrintObject *object = print.objects().front();
    const Layer       *layer  = first_overhang_layer(*object);
    REQUIRE(layer != nullptr);
    REQUIRE(layer->lower_layer != nullptr);

    ExtrusionQualityEstimator queried;
    queried.prepare_for_new_layer(object, layer->lower_layer);
    queried.prepare_for_new_layer(object, layer);
    const Walls expected = estimate_walls(queried, object, *layer);
    REQUIRE_FALSE(expected.empty());

    ExtrusionQualityEstimator precomputed;
    precomputed.prepare_for_new_layer(object, layer->lower_layer);
    precomputed.set_precomputed_layers({precompute_overhang_layer(object, *layer)});
    precomputed.prepare_for_new_layer(object, layer);
    check_identical_walls(estimate_walls(precomputed, object, *layer), expected);
}

TEST_CASE("Overhang distances measured against another layer than the previous one are not used", "[ExtrusionProcessor]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_overhang_speed", "1"},
        {"layer_height", "0.2"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
    });
    Print print;
    init_and_process_print({TestMesh::overhang}, print, config);
    const PrintObject *object    = print.objects().front();
    const Layer       *layer     = nullptr;
    for (const Layer *l : object->layers()) {
        if (l != nullptr && l->lower_layer != nullptr && l->lower_layer->lower_layer != nullptr && l->has_extrusions()) {
            layer = l;
            break;
        }
    }
    REQUIRE(layer != nullptr);
    const Layer *two_below = layer->lower_layer->lower_layer;

    ExtrusionQualityEstimator queried;
    queried.prepare_for_new_layer(object, two_below);
    queried.prepare_for_new_layer(object, layer);
    const Walls expected = estimate_walls(queried, object, *layer);

    ExtrusionQualityEstimator precomputed;
    precomputed.prepare_for_new_layer(object, two_below);
    precomputed.set_precomputed_layers({precompute_overhang_layer(object, *layer)});
    precomputed.prepare_for_new_layer(object, layer);
    check_identical_walls(estimate_walls(precomputed, object, *layer), expected);
}

TEST_CASE("Precomputed overhang data has the curled-line tree exactly when a region slows down for curled perimeters", "[ExtrusionProcessor]")
{
    for (const bool slowdown : {false, true}) {
        DYNAMIC_SECTION((slowdown ? "slowdown on" : "slowdown off")) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({
                {"enable_overhang_speed", "1"},
                {"slowdown_for_curled_perimeters", slowdown ? "1" : "0"},
                {"layer_height", "0.2"},
                {"skirt_loops", "0"},
                {"brim_type", "no_brim"},
            });
            Print print;
            init_and_process_print({TestMesh::overhang}, print, config);
            const PrintObject *object = print.objects().front();
            const Layer       *layer  = first_overhang_layer(*object);
            REQUIRE(layer != nullptr);

            GCode::LayerToPrint layer_to_print;
            layer_to_print.object_layer    = layer;
            layer_to_print.original_object = object;
            const std::vector<PrecomputedOverhangLayer> precomputed = precompute_overhang_layers({layer_to_print});
            REQUIRE(precomputed.size() == 1);
            CHECK((precomputed.front().lower_curled_lines != nullptr) == slowdown);
        }
    }
}

TEST_CASE("Curled walls are estimated only when overhang speed and the slowdown for curled perimeters are both on", "[ExtrusionProcessor]")
{
    struct Case
    {
        bool overhang_speed;
        bool slowdown;
        bool estimated;
    };
    const Case cases[] = {
        {true, true, true},
        {true, false, false},
        {false, true, false},
    };
    for (const Case c : cases) {
        DYNAMIC_SECTION("overhang speed " << c.overhang_speed << " slowdown " << c.slowdown) {
            DynamicPrintConfig config = overhang_curled_config();
            config.set_deserialize_strict({{"enable_overhang_speed", c.overhang_speed ? "1" : "0"},
                                           {"slowdown_for_curled_perimeters", c.slowdown ? "1" : "0"}});
            Print print;
            init_and_process_print({caged_overhang_mesh()}, print, config);

            CHECK(has_curled_lines(*print.objects().front()) == c.estimated);
        }
    }
}

TEST_CASE("Curled walls are estimated when overhang speed and the slowdown for curled perimeters are on in different objects",
          "[ExtrusionProcessor]")
{
    DynamicPrintConfig config = overhang_curled_config();
    config.set_deserialize_strict({
        {"enable_overhang_speed", "0"},
        {"slowdown_for_curled_perimeters", "0"},
    });
    Print print;
    Model model;
    init_print({caged_overhang_mesh(), caged_overhang_mesh()}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    model.objects[0]->config.set_key_value("enable_overhang_speed", new ConfigOptionBools{true});
    model.objects[0]->config.set_key_value("slowdown_for_curled_perimeters", new ConfigOptionBools{false});
    model.objects[1]->config.set_key_value("enable_overhang_speed", new ConfigOptionBools{false});
    model.objects[1]->config.set_key_value("slowdown_for_curled_perimeters", new ConfigOptionBools{true});
    print.apply(model, config);
    print.process();

    REQUIRE(print.objects().size() == 2);
    for (const PrintObject *object : print.objects())
        CHECK(has_curled_lines(*object));
}

TEST_CASE("Curled walls from an earlier slice are dropped once overhang speed is off", "[ExtrusionProcessor]")
{
    DynamicPrintConfig config = overhang_curled_config();
    Print              print;
    Model              model;
    init_print({caged_overhang_mesh()}, print, model, config);
    print.process();
    const PrintObject *object     = print.objects().front();
    const Layer       *first_layer = object->layers().front();
    REQUIRE(has_curled_lines(*object));

    config.set_deserialize_strict("enable_overhang_speed", "0");
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().front()->layers().front() == first_layer);
    CHECK_FALSE(has_curled_lines(*print.objects().front()));
}

TEST_CASE("Overhang data is precomputed for the layers the serial code prepares, by the first overhang speed value", "[ExtrusionProcessor]")
{
    // process_layer() prepares the estimator for a layer when a region has extrusions and its FIRST
    // enable_overhang_speed value is on. A second (High Flow) column that is on must not make another
    // layer qualify, or the estimator would compare walls with a different layer than the serial code does.
    struct Case
    {
        std::vector<unsigned char> overhang_speed;
        bool                       precomputed;
    };
    const Case cases[] = {
        {{1}, true},
        {{1, 0}, true},
        {{0}, false},
        {{0, 1}, false},
    };
    for (const Case &c : cases) {
        std::string label;
        for (unsigned char v : c.overhang_speed)
            label += v ? "1" : "0";
        DYNAMIC_SECTION("enable_overhang_speed " << label) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({
                {"layer_height", "0.2"},
                {"skirt_loops", "0"},
                {"brim_type", "no_brim"},
            });
            config.option<ConfigOptionBools>("enable_overhang_speed")->values = c.overhang_speed;
            Print print;
            init_and_process_print({TestMesh::overhang}, print, config);
            const PrintObject *object = print.objects().front();
            const Layer       *layer  = first_overhang_layer(*object);
            REQUIRE(layer != nullptr);
            REQUIRE(layer->lower_layer != nullptr);
            // The option reached the region unchanged.
            bool found_region = false;
            for (const LayerRegion *region : layer->regions())
                if (region->has_extrusions()) {
                    found_region = true;
                    REQUIRE(region->region().config().enable_overhang_speed.values == c.overhang_speed);
                }
            REQUIRE(found_region);

            GCode::LayerToPrint layer_to_print;
            layer_to_print.object_layer    = layer;
            layer_to_print.original_object = object;
            CHECK(precompute_overhang_layers({layer_to_print}).size() == (c.precomputed ? 1u : 0u));
        }
    }
}

TEST_CASE("A per-object slowdown override does not skip the curled-wall estimate while another region reads it", "[ExtrusionProcessor]")
{
    // GCode::_extrude extrudes each region's walls with that region's config applied, so a region whose
    // override turns the slowdown off still shares layers' curled lines with regions that have it on.
    // Skipping the estimate because ONE object's override is off would change the G-code of the others
    // compared with main, which always estimated here.
    struct Case
    {
        bool print_default;
        bool object0;
        bool object1;
        bool estimated;
    };
    const Case cases[] = {
        {true, false, true, true},   // object 0 overrides it off, the print default and object 1 have it on
        {true, false, false, true},  // both objects override it off, but the print default is on
        {false, false, true, true},  // only object 1 turns it on
        {false, false, false, false},
    };
    for (const Case c : cases) {
        DYNAMIC_SECTION("default " << c.print_default << " object 0 " << c.object0 << " object 1 " << c.object1) {
            DynamicPrintConfig config = overhang_curled_config();
            config.set_deserialize_strict({{"slowdown_for_curled_perimeters", c.print_default ? "1" : "0"}});
            Print print;
            Model model;
            init_print({caged_overhang_mesh(), caged_overhang_mesh()}, print, model, config);
            REQUIRE(model.objects.size() == 2);
            model.objects[0]->config.set_key_value("slowdown_for_curled_perimeters", new ConfigOptionBools{c.object0});
            model.objects[1]->config.set_key_value("slowdown_for_curled_perimeters", new ConfigOptionBools{c.object1});
            print.apply(model, config);
            print.process();

            REQUIRE(print.objects().size() == 2);
            for (const PrintObject *object : print.objects())
                CHECK(has_curled_lines(*object) == c.estimated);
        }
    }
}

TEST_CASE("Curled walls follow the first overhang speed value and any slowdown column", "[ExtrusionProcessor]")
{
    // Overhang speed: main's rule, the first (Standard) value. Slowdown: skipped only when no column reads it.
    struct Case
    {
        std::vector<unsigned char> overhang_speed;
        std::vector<unsigned char> slowdown;
        bool                       estimated;
    };
    const Case cases[] = {
        {{1}, {1}, true},
        {{1, 0}, {0, 1}, true},  // High Flow slowdown on: a High Flow filament could read the lines
        {{1, 0}, {0, 0}, false},
        {{0, 1}, {1, 1}, false}, // overhang speed is judged by the first value, as before
    };
    for (const Case &c : cases) {
        std::string label;
        for (unsigned char v : c.overhang_speed)
            label += v ? "1" : "0";
        label += " / ";
        for (unsigned char v : c.slowdown)
            label += v ? "1" : "0";
        DYNAMIC_SECTION("overhang speed / slowdown " << label) {
            DynamicPrintConfig config = overhang_curled_config();
            config.option<ConfigOptionBools>("enable_overhang_speed")->values           = c.overhang_speed;
            config.option<ConfigOptionBools>("slowdown_for_curled_perimeters")->values = c.slowdown;
            Print print;
            init_and_process_print({caged_overhang_mesh()}, print, config);

            CHECK(has_curled_lines(*print.objects().front()) == c.estimated);
        }
    }
}
