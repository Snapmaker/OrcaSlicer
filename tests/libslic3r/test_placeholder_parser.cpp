#include <catch2/catch.hpp>

#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

SCENARIO("Placeholder parser scripting", "[PlaceholderParser]") {
	PlaceholderParser 	parser;
	auto 				config = DynamicPrintConfig::full_print_config();

	config.set_deserialize_strict( {
		{ "printer_notes", "  PRINTER_VENDOR_PRUSA3D  PRINTER_MODEL_MK2  " },
	    { "nozzle_diameter", "0.6,0.6,0.6,0.6" },
	    { "nozzle_temperature", "357,359,363,378" },
	    // FloatOrPercent options exercising the "ratio_over" resolution paths below:
	    // Since the High-Flow retype, outer_wall_speed / bridge_speed / travel_speed are per-extruder
	    // coFloats and small_perimeter_speed / internal_bridge_speed / overhang_N_4_speed are
	    // coFloatsOrPercents; a bare reference resolves to the current extruder's element.
	    { "outer_wall_speed", "60" },            // parent of small_perimeter_speed
	    { "small_perimeter_speed", "50%" },      // percent over outer_wall_speed (coFloats)
	    { "bridge_speed", "50" },                // parent of internal_bridge_speed
	    { "internal_bridge_speed", "150%" },     // percent over bridge_speed (coFloats)
	    { "overhang_1_4_speed", "40" },          // absolute coFloatsOrPercents, returned as-is
	    { "travel_speed", "200" },               // parent of wipe_speed
	    { "wipe_speed", "80%" },                 // scalar percent over a coFloats parent
	    { "sparse_infill_line_width", "0.45" },  // absolute parent of infill_anchor
	    { "infill_anchor", "300%" },             // percent over sparse_infill_line_width (coFloatOrPercent)
	    { "initial_layer_line_width", "0.9" },   // absolute FloatOrPercent, returned as-is
	    // line_width is a percent whose ratio_over parent (nozzle_diameter) is a per-extruder vector,
	    // read at the current extruder.
	    { "line_width", "112.5%" },
	});

    parser.apply_config(config);
	parser.set("foo", 0);
	parser.set("bar", 2);
	parser.set("num_extruders", 4);

    SECTION("nested config options (legacy syntax)") { REQUIRE(parser.process("[nozzle_temperature_[foo]]") == "357"); }
    SECTION("array reference") { REQUIRE(parser.process("{nozzle_temperature[foo]}") == "357"); }
    SECTION("whitespaces and newlines are maintained") { REQUIRE(parser.process("test [ nozzle_temperature_ [foo] ] \n hu") == "test 357 \n hu"); }

    // Test the math expressions.
    SECTION("math: 2*3") { REQUIRE(parser.process("{2*3}") == "6"); }
    SECTION("math: 2*3/6") { REQUIRE(parser.process("{2*3/6}") == "1"); }
    SECTION("math: 2*3/12") { REQUIRE(parser.process("{2*3/12}") == "0"); }
    SECTION("math: 2.*3/12") { REQUIRE(std::stod(parser.process("{2.*3/12}")) == Approx(0.5)); }
    SECTION("math: 10 % 2.5") { REQUIRE(std::stod(parser.process("{10%2.5}")) == Approx(0.)); }
    SECTION("math: 11 % 2.5") { REQUIRE(std::stod(parser.process("{11%2.5}")) == Approx(1.)); }
    SECTION("math: 2*(3-12)") { REQUIRE(parser.process("{2*(3-12)}") == "-18"); }
    SECTION("math: 2*foo*(3-12)") { REQUIRE(parser.process("{2*foo*(3-12)}") == "0"); }
    SECTION("math: 2*bar*(3-12)") { REQUIRE(parser.process("{2*bar*(3-12)}") == "-36"); }
    SECTION("math: 2.5*bar*(3-12)") { REQUIRE(std::stod(parser.process("{2.5*bar*(3-12)}")) == Approx(-45)); }
    SECTION("math: min(12, 14)") { REQUIRE(parser.process("{min(12, 14)}") == "12"); }
    SECTION("math: max(12, 14)") { REQUIRE(parser.process("{max(12, 14)}") == "14"); }
    SECTION("math: min(13.4, -1238.1)") { REQUIRE(std::stod(parser.process("{min(13.4, -1238.1)}")) == Approx(-1238.1)); }
    SECTION("math: max(13.4, -1238.1)") { REQUIRE(std::stod(parser.process("{max(13.4, -1238.1)}")) == Approx(13.4)); }
    SECTION("math: int(13.4)") { REQUIRE(parser.process("{int(13.4)}") == "13"); }
    SECTION("math: int(-13.4)") { REQUIRE(parser.process("{int(-13.4)}") == "-13"); }
    SECTION("math: round(13.4)") { REQUIRE(parser.process("{round(13.4)}") == "13"); }
    SECTION("math: round(-13.4)") { REQUIRE(parser.process("{round(-13.4)}") == "-13"); }
    SECTION("math: round(13.6)") { REQUIRE(parser.process("{round(13.6)}") == "14"); }
    SECTION("math: round(-13.6)") { REQUIRE(parser.process("{round(-13.6)}") == "-14"); }
    SECTION("math: digits(5, 15)") { REQUIRE(parser.process("{digits(5, 15)}") == "              5"); }
    SECTION("math: digits(5., 15)") { REQUIRE(parser.process("{digits(5., 15)}") == "              5"); }
    SECTION("math: zdigits(5, 15)") { REQUIRE(parser.process("{zdigits(5, 15)}") == "000000000000005"); }
    SECTION("math: zdigits(5., 15)") { REQUIRE(parser.process("{zdigits(5., 15)}") == "000000000000005"); }
    SECTION("math: digits(5, 15, 8)") { REQUIRE(parser.process("{digits(5, 15, 8)}") == "     5.00000000"); }
    SECTION("math: digits(5., 15, 8)") { REQUIRE(parser.process("{digits(5, 15, 8)}") == "     5.00000000"); }
    SECTION("math: zdigits(5, 15, 8)") { REQUIRE(parser.process("{zdigits(5, 15, 8)}") == "000005.00000000"); }
    SECTION("math: zdigits(5., 15, 8)") { REQUIRE(parser.process("{zdigits(5, 15, 8)}") == "000005.00000000"); }
    SECTION("math: digits(13.84375892476, 15, 8)") { REQUIRE(parser.process("{digits(13.84375892476, 15, 8)}") == "    13.84375892"); }
    SECTION("math: zdigits(13.84375892476, 15, 8)") { REQUIRE(parser.process("{zdigits(13.84375892476, 15, 8)}") == "000013.84375892"); }
    SECTION("math: interpolate_table(13.84375892476, (0, 0), (20, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(13.84375892476, (0, 0), (20, 20))}")) == Approx(13.84375892476)); }
    SECTION("math: interpolate_table(13, (0, 0), (20, 20), (30, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(13, (0, 0), (20, 20), (30, 20))}")) == Approx(13.)); }
    SECTION("math: interpolate_table(25, (0, 0), (20, 20), (30, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(25, (0, 0), (20, 20), (30, 20))}")) == Approx(20.)); }

    // Test the "coFloatOrPercent" substitutions via the "ratio_over" chains.
    SECTION("initial_layer_line_width, absolute") { REQUIRE(std::stod(parser.process("{initial_layer_line_width}")) == Approx(0.9)); }
    // small_perimeter_speed 50% over outer_wall_speed (coFloats) 60
    SECTION("small_perimeter_speed") { REQUIRE(std::stod(parser.process("{small_perimeter_speed}")) == Approx(30.)); }
    SECTION("small_perimeter_speed, indexed") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[0]}")) == Approx(30.)); }
    SECTION("small_perimeter_speed in a condition") { REQUIRE(parser.process("{if small_perimeter_speed > 20}fast{else}slow{endif}") == "fast"); }
    // internal_bridge_speed 150% over bridge_speed (coFloats) 50
    SECTION("internal_bridge_speed") { REQUIRE(std::stod(parser.process("{internal_bridge_speed}")) == Approx(75.)); }
    SECTION("overhang_1_4_speed, absolute") { REQUIRE(std::stod(parser.process("{overhang_1_4_speed}")) == Approx(40.)); }
    // wipe_speed (scalar coFloatOrPercent) 80% over travel_speed (coFloats) 200
    SECTION("wipe_speed") { REQUIRE(std::stod(parser.process("{wipe_speed}")) == Approx(160.)); }
    // infill_anchor 300% over sparse_infill_line_width (absolute coFloatOrPercent) 0.45
    SECTION("infill_anchor") { REQUIRE(std::stod(parser.process("{infill_anchor}")) == Approx(1.35)); }
    // line_width 112.5% over nozzle_diameter (coFloats) 0.6 at the current extruder
    SECTION("line_width") { REQUIRE(std::stod(parser.process("{line_width}")) == Approx(0.675)); }

    // Test the boolean expression parser.
    auto boolean_expression = [&parser](const std::string& templ) { return parser.evaluate_boolean_expression(templ, parser.config()); };

    SECTION("boolean expression parser: 12 == 12") { REQUIRE(boolean_expression("12 == 12")); }
    SECTION("boolean expression parser: 12 != 12") { REQUIRE(! boolean_expression("12 != 12")); }
    SECTION("boolean expression parser: regex matches") { REQUIRE(boolean_expression("\"has some PATTERN embedded\" =~ /.*PATTERN.*/")); }
    SECTION("boolean expression parser: regex does not match") { REQUIRE(! boolean_expression("\"has some PATTERN embedded\" =~ /.*PTRN.*/")); }
    SECTION("boolean expression parser: accessing variables, equal") { REQUIRE(boolean_expression("foo + 2 == bar")); }
    SECTION("boolean expression parser: accessing variables, not equal") { REQUIRE(! boolean_expression("foo + 3 == bar")); }
    SECTION("boolean expression parser: (12 == 12) and (13 != 14)") { REQUIRE(boolean_expression("(12 == 12) and (13 != 14)")); }
    SECTION("boolean expression parser: (12 == 12) && (13 != 14)") { REQUIRE(boolean_expression("(12 == 12) && (13 != 14)")); }
    SECTION("boolean expression parser: (12 == 12) or (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) or (13 == 14)")); }
    SECTION("boolean expression parser: (12 == 12) || (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) || (13 == 14)")); }
    SECTION("boolean expression parser: (12 == 12) and not (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) and not (13 == 14)")); }
    SECTION("boolean expression parser: ternary true") { REQUIRE(boolean_expression("(12 == 12) ? (1 - 1 == 0) : (2 * 2 == 3)")); }
    SECTION("boolean expression parser: ternary false") { REQUIRE(! boolean_expression("(12 == 21/2) ? (1 - 1 == 0) : (2 * 2 == 3)")); }
    SECTION("boolean expression parser: ternary false 2") { REQUIRE(boolean_expression("(12 == 13) ? (1 - 1 == 3) : (2 * 2 == 4)")); }
    SECTION("boolean expression parser: ternary true 2") { REQUIRE(! boolean_expression("(12 == 2 * 6) ? (1 - 1 == 3) : (2 * 2 == 4)")); }
    SECTION("boolean expression parser: lower than - false") { REQUIRE(! boolean_expression("12 < 3")); }
    SECTION("boolean expression parser: lower than - true") { REQUIRE(boolean_expression("12 < 22")); }
    SECTION("boolean expression parser: greater than - true") { REQUIRE(boolean_expression("12 > 3")); }
    SECTION("boolean expression parser: greater than - false") { REQUIRE(! boolean_expression("12 > 22")); }
    SECTION("boolean expression parser: lower than or equal- false") { REQUIRE(! boolean_expression("12 <= 3")); }
    SECTION("boolean expression parser: lower than or equal - true") { REQUIRE(boolean_expression("12 <= 22")); }
    SECTION("boolean expression parser: greater than or equal - true") { REQUIRE(boolean_expression("12 >= 3")); }
    SECTION("boolean expression parser: greater than or equal - false") { REQUIRE(! boolean_expression("12 >= 22")); }
    SECTION("boolean expression parser: lower than or equal (same values) - true") { REQUIRE(boolean_expression("12 <= 12")); }
    SECTION("boolean expression parser: greater than or equal (same values) - true") { REQUIRE(boolean_expression("12 >= 12")); }
    SECTION("complex expression") { REQUIRE(boolean_expression("printer_notes=~/.*PRINTER_VENDOR_PRUSA3D.*/ and printer_notes=~/.*PRINTER_MODEL_MK2.*/ and nozzle_diameter[0]==0.6 and num_extruders>1")); }
    SECTION("complex expression2") { REQUIRE(boolean_expression("printer_notes=~/.*PRINTER_VEwerfNDOR_PRUSA3D.*/ or printer_notes=~/.*PRINTertER_MODEL_MK2.*/ or (nozzle_diameter[0]==0.6 and num_extruders>1)")); }
    SECTION("complex expression3") { REQUIRE(! boolean_expression("printer_notes=~/.*PRINTER_VEwerfNDOR_PRUSA3D.*/ or printer_notes=~/.*PRINTertER_MODEL_MK2.*/ or (nozzle_diameter[0]==0.3 and num_extruders>1)")); }
}

TEST_CASE("curr_bed_type placeholder resolves to the enum key", "[placeholder][curr_bed_type]") {
    OtherSlicingStatesConfigDef other_slicing_states;
    REQUIRE(other_slicing_states.has("curr_bed_type"));

    PlaceholderParser parser;
    auto              config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"curr_bed_type", "Textured PEI Plate"}});
    parser.apply_config(config);

    REQUIRE(parser.process("[curr_bed_type]") == "Textured PEI Plate");
    REQUIRE(parser.process("{if curr_bed_type==\"High Temp Plate\"}A{else}B{endif}") == "B");
}
