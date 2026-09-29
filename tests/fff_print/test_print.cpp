#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("PrintObject: Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            // Pin the Slic3r-era geometry this scenario was written against:
            // 0.5 + 65*0.3 = 20mm -> 66 layers, and 3 classic perimeter loops.
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
                { "sparse_infill_density",               0 },
                { "nozzle_diameter",            0.6 },
                { "layer_height",               0.3 },
                { "initial_layer_print_height", 0.5 },
                { "wall_loops",                 3 }
            });
			const PrintObject &object = *print.objects().front();
			THEN("67 layers exist in the model") {
                REQUIRE(object.layers().size() == 66);
            }
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
            THEN("Every layer in region 0 has 3 paths in its perimeters list.") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

SCENARIO("Print: Skirt generation", "[Print]") {
    GIVEN("20mm cube and default config") {
        WHEN("Skirts is set to 2 loops")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
            	{ "skirt_height", 	1 },
        		{ "skirt_distance", 1 },
        		{ "skirt_loops", 		2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid surfaces does not cause all surfaces to become internal.", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_solid_surfaces = 2 and bottom_solid_surfaces = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
			{ "top_shell_layers",		2 },
			{ "bottom_shell_layers",	1 },
			{ "layer_height",			0.25 }, // get a known number of layers
			{ "initial_layer_print_height",		0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (39, 38)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_solid_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 3mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					3 },
	        	// brim_type defaults to auto_brim in this fork, which ignores
	        	// brim_width for a shape that needs no brim (a plain cube).
	        	{ "brim_type", 					"outer_only" }
	        });
            THEN("Brim Extrusion collection has 2 loops in it") {
            // FORK BEHAVIOUR: Brim.cpp:385 quantises the requested brim_width DOWN to an
            // EVEN number of flow widths - floor(brim_width / flowWidth / 2) * flowWidth * 2 -
            // so the loop count is 2*floor(width / (2*flow)), not the upstream width/flow.
            // flow 1mm: 3mm -> 2 loops (not 3), 6mm -> 6; flow 0.5mm: 6mm -> 12 (not 14).
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 2);
            }
        }
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "brim_type", 					"outer_only" }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
	        	{ "initial_layer_line_width", 	1 },
	        	{ "brim_width", 					6 },
	        	{ "brim_type", 					"outer_only" },
	        	{ "initial_layer_line_width", 	0.5 }
	        });
			print.process();
            THEN("Brim Extrusion collection has 12 loops in it") {
            // FORK BEHAVIOUR: Brim.cpp:385 quantises the requested brim_width DOWN to an
            // EVEN number of flow widths - floor(brim_width / flowWidth / 2) * flowWidth * 2 -
            // so the loop count is 2*floor(width / (2*flow)), not the upstream width/flow.
            // flow 1mm: 3mm -> 2 loops (not 3), 6mm -> 6; flow 0.5mm: 6mm -> 12 (not 14).
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 12);
            }
        }
    }
}

// Orca #15924: inner-outer-inner (IOI) wall order. After the first layer, a 3-wall island is
// supposed to print the second internal wall, then the outer wall, then the first internal wall.
// Arachne's old centreline-distance test ignored variable width, so a widened odd centre line on a
// narrow wall was not grouped with its neighbours and the outer wall printed first.
namespace {

std::vector<int> island_wall_insets(const ExtrusionEntity *island)
{
    std::vector<int> insets;
    auto take = [&](const ExtrusionEntity *entity) {
        if (entity->inset_idx >= 0)
            insets.push_back(entity->inset_idx);
    };
    if (island->is_collection()) {
        for (const ExtrusionEntity *entity : static_cast<const ExtrusionEntityCollection *>(island)->entities)
            take(entity);
    } else {
        take(island);
    }
    return insets;
}

std::string insets_to_string(const std::vector<int> &insets)
{
    std::ostringstream os;
    for (size_t i = 0; i < insets.size(); ++i) {
        if (i)
            os << ',';
        os << insets[i];
    }
    return os.str();
}

TriangleMesh thin_ring(double wall_mm, double height_mm = 1.2)
{
    // Square-section ring: a hole plus an outer contour, matching the upstream thin-ring case.
    const double inner = 8.0;
    const double outer = inner + wall_mm;
    std::vector<Vec2d> profile{{inner, 0.}, {outer, 0.}, {outer, height_mm}, {inner, height_mm}};
    return TriangleMesh(its_make_revolved(profile, 64));
}

std::vector<int> sandwich_core(const std::vector<int> &insets)
{
    std::vector<int> core;
    for (int inset : insets) {
        if (inset == 0 || inset == 1 || inset == 2)
            core.push_back(inset);
    }
    return core;
}

bool has_insets_0_1_2(const std::vector<int> &insets)
{
    bool has0 = false, has1 = false, has2 = false;
    for (int inset : insets) {
        has0 = has0 || inset == 0;
        has1 = has1 || inset == 1;
        has2 = has2 || inset == 2;
    }
    return has0 && has1 && has2;
}

int count_ioi_sandwiches(const Print &print)
{
    int sandwiches = 0;
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() > 1);
    for (const Layer *layer : object.layers()) {
        if (layer->id() == 0)
            continue; // IOI is disabled on the first layer
        for (const LayerRegion *region : layer->regions()) {
            for (const ExtrusionEntity *island : region->perimeters.entities) {
                const std::vector<int> insets = island_wall_insets(island);
                if (!has_insets_0_1_2(insets))
                    continue;
                const std::vector<int> core = sandwich_core(insets);
                CAPTURE(layer->id(), insets_to_string(insets), insets_to_string(core));
                // Inner-outer-inner prints the second internal wall (inset 2) before the outer wall.
                REQUIRE_FALSE(core.empty());
                REQUIRE(core.front() == 2);
                ++sandwiches;
            }
        }
    }
    return sandwiches;
}

} // namespace

TEST_CASE("Inner-outer-inner wall order starts with the second internal wall on a cube", "[PrintObject][IOI]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({Slic3r::make_cube(20., 20., 1.2)}, print, {
        { "wall_generator",             wall_generator },
        { "wall_sequence",              "inner-outer-inner wall" },
        { "wall_loops",                 3 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter",            0.4 },
        { "line_width",                 0.4 },
        { "outer_wall_line_width",      0.4 },
        { "inner_wall_line_width",      0.4 },
        { "only_one_wall_top",          0 },
        { "sparse_infill_density",      0 },
        { "enable_support",             0 },
        { "brim_width",                 0 },
        { "detect_overhang_wall",       0 },
        { "offset_layers",              0 },
        { "precise_outer_wall",         0 },
        { "spiral_mode",                0 }
    });

    REQUIRE(count_ioi_sandwiches(print) > 0);
}

TEST_CASE("Arachne inner-outer-inner wall order holds on a narrow wall", "[PrintObject][IOI][Arachne]")
{
    // Upstream #15924 failed on a thin RING (outer contour + hole), not a solid strip. A solid
    // strip is one island; sandwich reordering still fires even when the width-aware touching
    // test misses the widened centre line. A ring has two outers, so grouping has to attach the
    // odd centre line or one side prints outer-first.
    //
    // Discriminator (old centreline test vs this PR, 0.4 mm line, 5 walls):
    //   1.6 mm  — fewer than three insets, sandwich never runs
    //   1.8 mm  — sandwich still fires without the width-aware test
    //   2.0 mm  — FAILS without the fix (first wall is inset 0), PASSES with it
    const double wall_mm = 2.0;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({thin_ring(wall_mm)}, print, {
        { "wall_generator",             "arachne" },
        { "wall_sequence",              "inner-outer-inner wall" },
        { "wall_loops",                 5 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter",            0.4 },
        { "line_width",                 0.4 },
        { "outer_wall_line_width",      0.4 },
        { "inner_wall_line_width",      0.4 },
        { "only_one_wall_top",          0 },
        { "sparse_infill_density",      0 },
        { "enable_support",             0 },
        { "brim_width",                 0 },
        { "detect_overhang_wall",       0 },
        { "offset_layers",              0 },
        { "precise_outer_wall",         0 },
        { "spiral_mode",                0 }
    });

    REQUIRE(count_ioi_sandwiches(print) > 0);
}

namespace {

std::string mixed_ab_definition()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00"});
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("12");
    return mgr.serialize_custom_entries();
}

unsigned int mixed_ab_virtual_id()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00"});
    return mgr.filament_id_from_mixed_index(0, 2);
}

DynamicPrintConfig two_filament_config(bool by_object, bool mixed_walls)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4"},
        {"filament_diameter",          "1.75,1.75"},
        {"enable_prime_tower",         "0"},
        {"enable_support",             "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "0"},
        {"brim_type",                  "no_brim"},
        {"print_sequence",             by_object ? "by object" : "by layer"},
        {"wall_loops",                 "2"},
        {"gcode_comments",             "1"},
        {"single_extruder_multi_material", "1"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};
    if (mixed_walls) {
        const unsigned int virtual_id = mixed_ab_virtual_id();
        config.set_deserialize_strict({
            {"wall_filament",          std::to_string(virtual_id)},
            {"sparse_infill_filament", std::to_string(virtual_id)},
            {"solid_infill_filament",  std::to_string(virtual_id)},
        });
        config.set("mixed_filament_definitions", mixed_ab_definition());
    } else {
        config.set_deserialize_strict({
            {"wall_filament",          "1"},
            {"sparse_infill_filament", "1"},
            {"solid_infill_filament",  "1"},
        });
    }
    return config;
}

std::string mixed_ac_pattern13_definition()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00", "#0000FF"});
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("13");
    return mgr.serialize_custom_entries();
}

unsigned int mixed_ac_virtual_id()
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00", "#0000FF"});
    return mgr.filament_id_from_mixed_index(0, 3);
}

DynamicPrintConfig three_filament_config(bool by_object)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4,0.4"},
        {"filament_diameter",          "1.75,1.75,1.75"},
        {"enable_prime_tower",         "0"},
        {"enable_support",             "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "0"},
        {"brim_type",                  "no_brim"},
        {"print_sequence",             by_object ? "by object" : "by layer"},
        {"wall_loops",                 "2"},
        {"gcode_comments",             "1"},
        {"single_extruder_multi_material", "1"},
        {"wall_filament",              "1"},
        {"sparse_infill_filament",     "1"},
        {"solid_infill_filament",      "1"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF"};
    config.set("mixed_filament_definitions", mixed_ac_pattern13_definition());
    return config;
}

std::string export_print_gcode(Print &print)
{
    print.set_status_silent();
    print.process();
    const boost::filesystem::path out = scratch_path(".gcode");
    print.export_gcode(out.string(), nullptr, nullptr);
    boost::nowide::ifstream in(out.string());
    std::string             gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    boost::nowide::remove(out.string().c_str());
    return gcode;
}

// Strip the lines that legitimately differ across runs (generation timestamp and M73
// estimates) so two exports of the same job can be compared byte-for-byte.
std::string strip_gcode_timestamps(const std::string &gcode)
{
    std::string out;
    out.reserve(gcode.size());
    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol + 1;
        const std::string line = gcode.substr(pos, end - pos);
        const bool volatile_line =
            line.find("; generated by") != std::string::npos ||
            line.find("M73") != std::string::npos ||
            line.find("estimated") != std::string::npos ||
            line.find("total estimated time") != std::string::npos;
        if (!volatile_line)
            out += line;
        pos = end;
    }
    return out;
}

size_t count_toolchange(const std::string &gcode, unsigned int extruder_id)
{
    const std::string needle = "T" + std::to_string(extruder_id);
    size_t            count  = 0;
    size_t            pos    = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol;
        std::string  line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // "T<n>" or "T<n> ; change extruder" — comments are optional when gcode_comments is off.
        if (line.compare(0, needle.size(), needle) == 0 &&
            (line.size() == needle.size() || line[needle.size()] == ' ' || line[needle.size()] == ';'))
            ++count;
        pos = end == gcode.size() ? gcode.size() : end + 1;
    }
    return count;
}

struct AutoGenerateGuard
{
    AutoGenerateGuard() : previous(MixedFilamentManager::auto_generate_enabled())
    {
        MixedFilamentManager::set_auto_generate_enabled(true);
    }
    ~AutoGenerateGuard() { MixedFilamentManager::set_auto_generate_enabled(previous); }
    bool previous;
};

struct SupportExtrusionHit
{
    double   z    = 0.;
    unsigned tool = 0;
};

std::vector<SupportExtrusionHit> parse_support_extrusions(const std::string &gcode)
{
    std::vector<SupportExtrusionHit> hits;
    double                           z    = 0.;
    unsigned                         tool = 0;
    size_t                           pos  = 0;
    while (pos < gcode.size()) {
        const size_t      eol  = gcode.find('\n', pos);
        const size_t      end  = eol == std::string::npos ? gcode.size() : eol;
        std::string       line = gcode.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if ((line.compare(0, 2, "G0") == 0 || line.compare(0, 2, "G1") == 0) &&
            (line.size() == 2 || line[2] == ' ' || line[2] == 'X' || line[2] == 'Y' || line[2] == 'Z' || line[2] == 'F' ||
             line[2] == 'E')) {
            const size_t zpos = line.find('Z');
            if (zpos != std::string::npos && zpos + 1 < line.size()) {
                char *endptr = nullptr;
                const double parsed = std::strtod(line.c_str() + zpos + 1, &endptr);
                if (endptr != line.c_str() + zpos + 1)
                    z = parsed;
            }
        }
        if (!line.empty() && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = unsigned(std::strtoul(line.c_str() + 1, nullptr, 10));
        }
        if (line.find("support material") != std::string::npos)
            hits.push_back({z, tool});
        pos = end == gcode.size() ? gcode.size() : end + 1;
    }
    return hits;
}

bool support_hit_at_z(const std::vector<SupportExtrusionHit> &hits, double print_z, unsigned expected_tool)
{
    for (const SupportExtrusionHit &hit : hits) {
        if (std::abs(hit.z - print_z) < 0.05 && hit.tool == expected_tool)
            return true;
    }
    return false;
}

} // namespace

// S1: ByObject + mixed virtual wall_filament used to SIGSEGV in GCode::needs_retraction
// (writer().extruder() dangling because Print::extruders() clamped the virtual id to 0, so
// GCodeWriter never registered the mixed component-B physical extruder).
TEST_CASE("ByObject mixed virtual wall filament exports with physical toolchanges", "[Print][MixedFilament][GCode]")
{
    REQUIRE(mixed_ab_virtual_id() == 3);

    const bool by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, two_filament_config(by_object, true));
        REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

        // Export first: on main this SIGSEGVs in GCode::needs_retraction before any
        // post-export check can run. Print::extruders() is asserted afterwards.
        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());

        const std::vector<unsigned int> used = print.extruders();
        REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
        REQUIRE(std::find(used.begin(), used.end(), 1u) != used.end());
        if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
            boost::nowide::ofstream dump(std::string(dir) + (by_object ? "/mixed_byobject.gcode" : "/mixed_bylayer.gcode"));
            dump << strip_gcode_timestamps(gcode);
        }
        const size_t t0 = count_toolchange(gcode, 0);
        const size_t t1 = count_toolchange(gcode, 1);
        INFO("T0=" << t0 << " T1=" << t1);
        REQUIRE(t0 + t1 >= 2);
        REQUIRE(t0 >= 1);
        REQUIRE(t1 >= 1);
    }
}

TEST_CASE("Non-mixed two-filament G-code is unchanged by mixed-id expansion", "[Print][GCode]")
{
    const bool by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, two_filament_config(by_object, false));
        const std::vector<unsigned int> used = print.extruders();
        REQUIRE(used == std::vector<unsigned int>{0});

        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());
        REQUIRE(count_toolchange(gcode, 0) >= 1);
        REQUIRE(count_toolchange(gcode, 1) == 0);
        REQUIRE(count_toolchange(gcode, 2) == 0);
        REQUIRE_FALSE(strip_gcode_timestamps(gcode).empty());
        if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
            boost::nowide::ofstream dump(std::string(dir) + (by_object ? "/nonmixed_byobject.gcode" : "/nonmixed_bylayer.gcode"));
            dump << strip_gcode_timestamps(gcode);
        }
    }
}

// Pattern "13" names physical 1 and 3. Expanding those tokens in Print::extruders(), or
// unioning the later object's ToolOrdering, is each enough to register T0 and T2. Component B
// (filament 2) is unused and must stay out of Print::extruders() — over-including it would
// trip temp-compat / CLI hard-block / bed-max / tower validation.
TEST_CASE("ByObject later object mixed pattern 13 exports T0 and T2", "[Print][MixedFilament][GCode]")
{
    REQUIRE(mixed_ac_virtual_id() == 4);

    Print print;
    Model model;
    DynamicPrintConfig config = three_filament_config(true);
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    REQUIRE(print.mixed_filament_manager().is_mixed(4, 3));

    model.objects[1]->volumes[0]->config.set_key_value("wall_filament", new ConfigOptionInt(4));
    model.objects[1]->volumes[0]->config.set_key_value("sparse_infill_filament", new ConfigOptionInt(4));
    model.objects[1]->volumes[0]->config.set_key_value("solid_infill_filament", new ConfigOptionInt(4));
    print.apply(model, config);
    print.validate();

    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 2u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) == used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    INFO("T0=" << count_toolchange(gcode, 0) << " T2=" << count_toolchange(gcode, 2));
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 2) >= 1);
    REQUIRE(count_toolchange(gcode, 1) == 0);
}

TEST_CASE("ByObject mixed support filament exports with physical toolchanges", "[Print][MixedFilament][GCode]")
{
    DynamicPrintConfig config = two_filament_config(true, true);
    config.set_deserialize_strict({
        {"wall_filament",                 "1"},
        {"sparse_infill_filament",        "1"},
        {"solid_infill_filament",         "1"},
        {"enable_support",                "1"},
        {"support_type",                  "normal(auto)"},
        {"support_filament",               "3"},
        {"support_interface_filament",     "3"},
        {"support_on_build_plate_only",    "0"},
    });

    Print print;
    Model model;
    init_print({TestMesh::overhang}, print, model, config);
    REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) != used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    INFO("T0=" << count_toolchange(gcode, 0) << " T1=" << count_toolchange(gcode, 1));
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 1) >= 1);
}

TEST_CASE("Auto mixed support with A/B layer heights matches ToolOrdering", "[Print][MixedFilament][GCode]")
{
    AutoGenerateGuard auto_mixed;
    const bool        by_object = GENERATE(true, false);
    DYNAMIC_SECTION((by_object ? "by object" : "by layer"))
    {
        DynamicPrintConfig config = two_filament_config(by_object, false);
        config.set_deserialize_strict({
            {"enable_support",             "1"},
            {"support_type",               "normal(auto)"},
            {"support_filament",           "3"},
            {"support_interface_filament", "3"},
            {"support_on_build_plate_only","0"},
            {"mixed_color_layer_height_a", "0.3"},
            {"mixed_color_layer_height_b", "0.6"},
        });

        Print print;
        Model model;
        init_print({TestMesh::overhang}, print, model, config);
        REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));
        REQUIRE_FALSE(print.mixed_filament_manager().mixed_filaments().front().custom);

        std::string gcode;
        REQUIRE_NOTHROW(gcode = export_print_gcode(print));
        REQUIRE_FALSE(gcode.empty());
        const std::vector<SupportExtrusionHit> hits = parse_support_extrusions(gcode);

        const PrintObject &object = *print.objects().front();
        REQUIRE_FALSE(object.support_layers().empty());
        ToolOrdering ordering = by_object ? ToolOrdering(object, (unsigned int) -1) : print.tool_ordering();

        bool saw_height_cycle_disagreement = false;
        size_t filled_support_layers       = 0;
        for (const SupportLayer *sl : object.support_layers()) {
            if (sl == nullptr || sl->support_fills.entities.empty())
                continue;
            ++filled_support_layers;
            const LayerTools  &lt     = ordering.tools_for_layer(sl->print_z);
            const unsigned int via_to = lt.resolve_mixed_1based_at(3, float(sl->print_z), float(sl->height), &object);
            REQUIRE(via_to >= 1);
            REQUIRE(lt.has_extruder(via_to - 1));

            unsigned int via_direct = 3;
            if (lt.mixed_mgr != nullptr)
                via_direct = lt.mixed_mgr->resolve(3, lt.num_physical, lt.layer_index, float(sl->print_z), float(sl->height), false, &object);
            if (via_direct != via_to)
                saw_height_cycle_disagreement = true;

            INFO("print_z=" << sl->print_z << " scheduled_tool=" << (via_to - 1) << " direct=" << (via_direct >= 1 ? via_direct - 1 : -1)
                            << " hits=" << hits.size());
            REQUIRE(support_hit_at_z(hits, sl->print_z, via_to - 1));
        }
        REQUIRE(filled_support_layers >= 1);
        REQUIRE(saw_height_cycle_disagreement);
    }
}

TEST_CASE("Non-mixed multi-object ByObject G-code dump", "[Print][GCode]")
{
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, two_filament_config(true, false));
    REQUIRE(print.extruders() == std::vector<unsigned int>{0});

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 1) == 0);
    if (const char *dir = std::getenv("DUMP_GCODE_DIR")) {
        boost::nowide::ofstream dump(std::string(dir) + "/nonmixed_byobject_multi.gcode");
        dump << strip_gcode_timestamps(gcode);
    }
}

TEST_CASE("ByObject mixed pattern [10] exports T0 and T9", "[Print][MixedFilament][GCode]")
{
    MixedFilamentManager mgr;
    std::vector<std::string> colors(10, "#FF0000");
    colors[1] = "#00FF00";
    colors[9] = "#0000FF";
    mgr.add_custom_filament(1, 2, 50, colors);
    mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("1[10]");
    REQUIRE(mgr.filament_id_from_mixed_index(0, 10) == 11);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(10);
    config.set_num_filaments(10);
    config.set_deserialize_strict({
        {"nozzle_diameter",            "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4"},
        {"filament_diameter",          "1.75,1.75,1.75,1.75,1.75,1.75,1.75,1.75,1.75,1.75"},
        {"enable_prime_tower",         "0"},
        {"enable_support",             "0"},
        {"sparse_infill_density",      "0"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "0"},
        {"brim_type",                  "no_brim"},
        {"print_sequence",             "by object"},
        {"wall_loops",                 "2"},
        {"gcode_comments",             "1"},
        {"single_extruder_multi_material", "1"},
        {"wall_filament",              "11"},
        {"sparse_infill_filament",     "11"},
        {"solid_infill_filament",      "11"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = colors;
    config.set("mixed_filament_definitions", mgr.serialize_custom_entries());

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.mixed_filament_manager().is_mixed(11, 10));
    const std::vector<unsigned int> used = print.extruders();
    REQUIRE(std::find(used.begin(), used.end(), 0u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 9u) != used.end());
    REQUIRE(std::find(used.begin(), used.end(), 1u) == used.end());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = export_print_gcode(print));
    REQUIRE_FALSE(gcode.empty());
    REQUIRE(count_toolchange(gcode, 0) >= 1);
    REQUIRE(count_toolchange(gcode, 9) >= 1);
}
