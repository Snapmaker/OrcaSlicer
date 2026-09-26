#include <catch2/catch.hpp>

#include <memory>

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

// Orca #15755 (selective): U1 end-G-code metadata from Edge per-extruder flow variants.
static std::string slice_volume_type_end_gcode(const std::vector<int> &nozzle_vts, unsigned filaments = 0)
{
    DynamicPrintConfig config    = DynamicPrintConfig::full_print_config();
    const unsigned     extruders = unsigned(std::max<size_t>(nozzle_vts.size(), 2));
    const unsigned     n         = filaments ? filaments : extruders;
    config.set_num_extruders(extruders);
    config.set_num_filaments(n);
    config.set_deserialize_strict({
        { "machine_end_gcode",              "; TEST_FVT = {filament_volume_type_list}" },
        { "machine_start_gcode",            "" },
        { "single_extruder_multi_material", "0" },
        { "layer_height",                   "0.2" },
        { "initial_layer_print_height",     "0.2" },
    });
    auto *nvt = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    REQUIRE(nvt != nullptr);
    if (!nozzle_vts.empty())
        nvt->values = nozzle_vts;
    if (nvt->values.size() < extruders)
        nvt->values.resize(extruders, int(nvtStandard));

    TriangleMesh a = mesh(TestMesh::cube_20x20x20);
    TriangleMesh b = mesh(TestMesh::cube_20x20x20);
    b.translate(30.f, 0.f, 0.f);
    return slice({ a, b }, config);
}

TEST_CASE("filament_volume_type_list is emitted in end G-code from nozzle volume types", "[GCode][U1]")
{
    SECTION("mixed Standard / High Flow") {
        const std::string gcode = slice_volume_type_end_gcode({ int(nvtStandard), int(nvtHighFlow) });
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow\n") != std::string::npos);
    }
    SECTION("defaults are standard,standard") {
        const std::string gcode = slice_volume_type_end_gcode({});
        REQUIRE(gcode.find("; TEST_FVT = standard,standard\n") != std::string::npos);
    }
    SECTION("identity map wraps extra filaments onto toolheads") {
        const std::string gcode = slice_volume_type_end_gcode({ int(nvtStandard), int(nvtHighFlow) }, 3);
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow,standard\n") != std::string::npos);
    }
}
