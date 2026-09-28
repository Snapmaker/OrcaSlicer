#include <catch2/catch.hpp>

#include <memory>
#include <string>

#include "libslic3r/GCode.hpp"
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
