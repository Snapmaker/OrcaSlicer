#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "libslic3r/GCode.hpp"
#include "test_data.hpp"

#include <regex>
#include <sstream>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace Slic3r;

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

// With lifts enforced on top surfaces only, nothing lifts before a top surface is printed. The
// role of the last fill decides it, and the first travel of the print comes before any fill, so
// that role must start empty rather than hold whatever was in memory. On glibc the test fills new
// memory with the byte of a top solid infill role, so a missing initial value always shows.
// Ported from OrcaSlicer/OrcaSlicer#17949.
TEST_CASE("A lift enforced on top surfaces only never lifts before the first top surface", "[GCode]")
{
#if defined(__GLIBC__)
    mallopt(M_PERTURB, int(erTopSolidInfill) ^ 0xff);
#endif
    Print print;
    Model model;
    // Two objects, so the first layer travels between them with a retraction.
    Test::init_print({Test::TestMesh::cube_20x20x20, Test::TestMesh::cube_20x20x20}, print, model, {
        {"retract_lift_enforce", "Top Only"},
        {"z_hop", "0.6"},
        {"retraction_length", "1"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    const std::string gcode = Test::gcode(print);
#if defined(__GLIBC__)
    mallopt(M_PERTURB, 0);
#endif

    double             layer_z = 0.;
    size_t             first_layer_lifts = 0;
    std::istringstream lines(gcode);
    std::string        line;
    const std::regex   z_word(R"(\bZ([-0-9.]+))");
    while (std::getline(lines, line)) {
        if (line.rfind(";Z:", 0) == 0) {
            layer_z = std::stod(line.substr(3));
            continue;
        }
        if (layer_z <= 0. || layer_z > 0.2 + EPSILON || (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0))
            continue;
        const std::string move = line.substr(0, line.find(';'));
        std::smatch       match;
        first_layer_lifts += std::regex_search(move, match, z_word) && std::stod(match[1]) > layer_z + 0.3;
    }
    CHECK(layer_z > 0.2); // the print has more than the first layer
    CHECK(first_layer_lifts == 0);
}
