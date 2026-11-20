#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Config block writes one flow-variant value per filament", "[GCode]") {
	DynamicPrintConfig config = Test::default_print_config();
	config.set_num_filaments(4);
	config.option<ConfigOptionInts>("nozzle_temperature", true)->values = { 220, 230, 220, 220 };
	config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = { 1 };

	const auto config_block = [](DynamicPrintConfig full_config) {
		Model model;
		Print print;
		print.apply(model, std::move(full_config));
		std::string block;
		GCode::append_full_config(print, block);
		return block;
	};

	SECTION("filament_volume_type holds a single entry") {
		config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = { fvtStandard };
		REQUIRE_THAT(config_block(config), ContainsSubstring("; nozzle_temperature = 220,230,220,220\n"));
	}
	SECTION("filament_volume_type holds more entries than there are filaments") {
		config.option<ConfigOptionFloats>("filament_diameter", true)->values = { 1.75, 1.75 };
		config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = { fvtStandard, fvtStandard, fvtStandard, fvtStandard };
		REQUIRE_THAT(config_block(config), ContainsSubstring("; nozzle_temperature = 220,230\n"));
	}
}

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
