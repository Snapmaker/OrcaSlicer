#include <catch2/catch.hpp>

#include <memory>
#include <string>

#include "libslic3r/GCode.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Origin manipulation", "[GCode]") {
	Slic3r::GCode gcodegen;
	WHEN("set_origin to (10,0)") {
    	gcodegen.set_origin(Vec2d(10,0));
    	REQUIRE(gcodegen.origin() == Vec2d(10, 0));
    }
	WHEN("set_origin to (10,0) and translate by (5, 5)") {
		gcodegen.set_origin(Vec2d(10,0));
		gcodegen.set_origin(gcodegen.origin() + Vec2d(5, 5));
		THEN("origin returns reference to point") {
    		REQUIRE(gcodegen.origin() == Vec2d(15,5));
    	}
    }
}

// Orca #15755 (selective): U1 end-G-code metadata from the sliced per-filament flow type.
static std::string slice_volume_type_end_gcode(const std::vector<int> &filament_vts,
                                               const std::vector<int> &nozzle_vts = {})
{
    DynamicPrintConfig config    = DynamicPrintConfig::full_print_config();
    const unsigned     n         = unsigned(std::max<size_t>(filament_vts.size(), 2));
    const unsigned     extruders = unsigned(std::max<size_t>(nozzle_vts.size(), 2));
    config.set_num_extruders(extruders);
    config.set_num_filaments(n);
    // GCode.cpp sizes filament_volume_type_list by physical filament_type (not
    // MixedFilamentManager virtual IDs). set_num_filaments() does not resize
    // filament_type, so pin it to the physical count the helper is testing.
    {
        auto *ft = config.option<ConfigOptionStrings>("filament_type", true);
        REQUIRE(ft != nullptr);
        ft->values.assign(n, std::string("PLA"));
    }
    config.set_deserialize_strict({
        { "machine_end_gcode",              "; TEST_FVT = {filament_volume_type_list}" },
        { "machine_start_gcode",            "" },
        { "single_extruder_multi_material", "0" },
        { "layer_height",                   "0.2" },
        { "initial_layer_print_height",     "0.2" },
    });
    if (!filament_vts.empty()) {
        auto *fvt = config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true);
        REQUIRE(fvt != nullptr);
        fvt->values = filament_vts;
    }
    if (!nozzle_vts.empty()) {
        auto *nvt = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
        REQUIRE(nvt != nullptr);
        nvt->values = nozzle_vts;
    }

    TriangleMesh a = mesh(TestMesh::cube_20x20x20);
    TriangleMesh b = mesh(TestMesh::cube_20x20x20);
    b.translate(30.f, 0.f, 0.f);
    return slice({ a, b }, config);
}

TEST_CASE("filament_volume_type_list is emitted in end G-code from filament_volume_type", "[GCode][U1]")
{
    SECTION("mixed Standard / High Flow") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtHighFlow) });
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow\n") != std::string::npos);
    }
    SECTION("defaults are standard,standard") {
        const std::string gcode = slice_volume_type_end_gcode({});
        REQUIRE(gcode.find("; TEST_FVT = standard,standard\n") != std::string::npos);
    }
    SECTION("nozzle high_flow does not override filament standard") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtStandard) },
                                                             { int(nvtStandard), int(nvtHighFlow) });
        REQUIRE(gcode.find("; TEST_FVT = standard,standard\n") != std::string::npos);
    }
    SECTION("extra filaments follow filament_volume_type") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtHighFlow), int(fvtStandard) });
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow,standard\n") != std::string::npos);
    }
}

// Some firmwares only scan the last N lines of the file for "estimated printing time", so it
// must stay close to EOF regardless of the resolved-settings config block's size.
TEST_CASE("Estimated printing time comment follows the config block", "[GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::string  gcode  = slice({TestMesh::cube_20x20x20}, config);
    const size_t       config_block_end = gcode.find("; CONFIG_BLOCK_END");
    const size_t       time_comment     = gcode.rfind("; estimated printing time");
    REQUIRE(config_block_end != std::string::npos);
    REQUIRE(time_comment != std::string::npos);
    REQUIRE(time_comment > config_block_end);
}

// FanMover (active when fan_speedup_time or fan_kickstart != 0) can split a G1 to insert an
// early fan command. GCode::set_extruder must bracket change_filament_gcode so those travels
// stay intact. FanMover keys off a "; custom gcode" prefix and ignores comments shorter than
// 17 chars, so the start marker is "; custom gcode start".
TEST_CASE("Toolchange custom gcode is not split by FanMover", "[GCode][FanMover]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_key_value("change_filament_gcode", new ConfigOptionString("G1 X10 F5000\nG1 X70 F5000"));
    config.set_deserialize_strict({
        {"skirt_loops",           0},
        {"brim_type",             "no_brim"},
        {"print_sequence",        "by object"},
        {"fan_speedup_time",      0.5},
        {"fan_kickstart",         0.5},
        {"fan_speedup_overhangs", 0},
        {"machine_start_gcode",   ""},
    });
    config.option<ConfigOptionInts>("close_fan_the_first_x_layers")->values = {0, 0};
    config.option<ConfigOptionFloats>("fan_min_speed")->values              = {50., 50.};

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    model.objects[1]->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);

    const size_t start = gcode.find("; custom gcode start");
    REQUIRE(start != std::string::npos);
    const size_t end = gcode.find("; custom gcode end", start);
    REQUIRE(end != std::string::npos);
    CHECK(gcode.substr(start, end - start).find("G1 X10 F5000\nG1 X70 F5000") != std::string::npos);
}
