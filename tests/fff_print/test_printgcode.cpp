#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

std::regex perimeters_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; perimeter");
std::regex infill_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; infill");
std::regex skirt_regex("G1 X[-0-9.]* Y[-0-9.]* E[-0-9.]* ; skirt");

SCENARIO( "PrintGCode basic functionality", "[PrintGCode]") {
    GIVEN("A default configuration and a print test object") {
        WHEN("the output is executed with no support material") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
                { "layer_height",					0.2 },
                { "initial_layer_print_height",				0.2 },
                { "initial_layer_line_width",	0 },
                { "gcode_comments",					true },
                { "machine_start_gcode",					"" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains slic3r version") {
                // The exported header is branded "Snapmaker Orca <Snapmaker_VERSION>"
                // (utils.cpp header_slic3r_generated()), not the SLIC3R_VERSION macro.
                REQUIRE(gcode.find(Snapmaker_VERSION) != std::string::npos);
            }
            //THEN("Exported text contains git commit id") {
            //    REQUIRE(gcode.find("; Git Commit") != std::string::npos);
            //    REQUIRE(gcode.find(SLIC3R_BUILD_ID) != std::string::npos);
            //}
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Exported text does not contain cooling markers (they were consumed)") {
                REQUIRE(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
            }

            THEN("GCode preamble is emitted.") {
                // FORK BEHAVIOUR: the Bambu-derived exporter emits no "G21" units preamble
                // (grep GCode.cpp - the string does not exist); millimetres are implicit.
                SUCCEED("skipped: this exporter emits no G21 preamble");
            }

            THEN("Config options emitted for print config, default region config, default object config") {
                REQUIRE(gcode.find("; first_layer_temperature") != std::string::npos);
                REQUIRE(gcode.find("; layer_height") != std::string::npos);
                // The trailing config block lists the CURRENT option names, and
                // fill_density was renamed sparse_infill_density in this fork.
                REQUIRE(gcode.find("; sparse_infill_density") != std::string::npos);
            }
            THEN("Infill is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
				std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("final Z height is 20mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max<double>(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                // FORK BEHAVIOUR: the highest Z in the file is not the top solid layer any
                // more. The end-of-print retract raises Z by the configured z_hop (0.4 by
                // default) after the last extrusion, so the maximum Z the reader sees is
                // top_layer_z + z_hop. Compare against that instead of the model height.
                REQUIRE(final_z == Approx(20. + print.config().z_hop.get_at(0)));
            }
        }
        WHEN("output is executed with complete objects and two differently-sized meshes") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20,TestMesh::cube_20x20x20}, print, model, {
                { "initial_layer_line_width",    0 },
                { "initial_layer_print_height",             0.3 },
                { "layer_height",                   0.2 },
                { "enable_support",               false },
                { "raft_layers",                    0 },
                { "print_sequence",                 "by object" },
                { "gcode_comments",                 true }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Infill is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, infill_regex));
            }
            THEN("Perimeters are emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, perimeters_regex));
            }
            THEN("Skirt is emitted.") {
                std::smatch has_match;
                REQUIRE(std::regex_search(gcode, has_match, skirt_regex));
            }
            THEN("Between-object-gcode is emitted.") {
                // FORK BEHAVIOUR: PrusaSlicer's `between_objects_gcode` option does not
                // exist in this fork - it was dropped, not renamed, so there is no Orca
                // key to set and no custom G-code to find. PrintConfigDef::handle_legacy()
                // silently CLEARS any key missing from print_config_def, so the option in
                // the config block above never reached the config and this REQUIRE looked
                // for a string nothing had emitted. Skipped rather than deleted: if the
                // option is ever reinstated, restore the key above and drop this SKIP.
                SUCCEED("skipped: between_objects_gcode is not an option in this fork");
            }
            THEN("final Z height is 20.1mm") {
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                // FORK BEHAVIOUR: the highest Z in the file is not the top solid layer any
                // more. The end-of-print retract raises Z by the configured z_hop (0.4 by
                // default) after the last extrusion, so the maximum Z the reader sees is
                // top_layer_z + z_hop. Compare against that instead of the model height.
                REQUIRE(final_z == Approx(20.1 + print.config().z_hop.get_at(0)));
            }
            THEN("Z height resets on object change") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { // saw higher Z before this, now it's lower
                        reset = true;
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
            THEN("Shorter object is printed before taller object.") {
                double final_z = 0.0;
                bool reset = false;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z, &reset] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    if (final_z > 0 && std::abs(self.z() - 0.3) < 0.01 ) { 
                        reset = (final_z > 20.0);
                    } else {
                        final_z = std::max(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                    }
                });
                REQUIRE(reset == true);
            }
        }
        WHEN("the output is executed with support material") {
            std::string gcode = ::Test::slice({TestMesh::cube_20x20x20}, {
                { "initial_layer_line_width",    0 },
                { "enable_support",               true },
                { "raft_layers",                    3 },
                { "gcode_comments",                 true }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") == std::string::npos);
            }
            THEN("Raft is emitted.") {
                REQUIRE(gcode.find("; raft") != std::string::npos);
            }
        }
        WHEN("the output is executed with a separate first layer extrusion width") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "initial_layer_line_width", "0.5" }
                });
            THEN("Some text output is generated.") {
                REQUIRE(gcode.size() > 0);
            }
            THEN("Exported text contains extrusion statistics.") {
                REQUIRE(gcode.find("; external perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; perimeters extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; solid infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; top infill extrusion width") != std::string::npos);
                REQUIRE(gcode.find("; support material extrusion width") == std::string::npos);
                REQUIRE(gcode.find("; first layer extrusion width") != std::string::npos);
            }
        }
        WHEN("Cooling is enabled and the fan is disabled.") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "slow_down_for_layer_cooling",                    true },
                { "close_fan_the_first_x_layers",   5 }
                });
            THEN("GCode to disable fan is emitted."){
                // FORK BEHAVIOUR: GCodeWriter::set_fan() emits "M106 S0" to switch the fan
                // off for every flavor except MakerWare/Sailfish (M127); it never emits M107.
                REQUIRE(gcode.find("M106 S0") != std::string::npos);
            }
        }
        WHEN("end_gcode exists with layer_num and layer_z") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
				{ "machine_end_gcode",              "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
                { "layer_height",           0.1 },
                { "initial_layer_print_height",     0.1 }
                });
            THEN("layer_num and layer_z are processed in the end gcode") {
                REQUIRE(gcode.find("; Layer_num 199") != std::string::npos);
                REQUIRE(gcode.find("; Layer_z 20") != std::string::npos);
            }
        }
        WHEN("current_extruder exists in start_gcode") {
            {
				std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
					{ "machine_start_gcode", "; Extruder [current_extruder]" }
                });
                THEN("current_extruder is processed in the start gcode and set for first extruder") {
                    REQUIRE(gcode.find("; Extruder 0") != std::string::npos);
                }
            }
			{
                DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
                config.set_num_extruders(4);
                // filament_diameter is a FILAMENT option, not an extruder one:
                // Print::object_extruders() bounds extruder indices by its size and
                // clamps anything past it back to 0, so the filament count must be
                // set too or every extruder above the first collapses onto extruder 0.
                config.set_num_filaments(4);
                config.set_deserialize_strict({
                    { "machine_start_gcode",                    "; Extruder [current_extruder]" },
                    { "sparse_infill_filament",                2 },
                    { "solid_infill_filament",          2 },
                    { "wall_filament",             2 },
                    { "support_filament",      2 },
                    { "support_interface_filament", 2 }
                });
                std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                THEN("current_extruder is processed in the start gcode and set for second extruder") {
                    REQUIRE(gcode.find("; Extruder 1") != std::string::npos);
                }
            }
        }

        WHEN("layer_num represents the layer's index from z=0") {
			std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, {
				{ "print_sequence",                 "by object" },
                { "gcode_comments",                 true },
                { "layer_change_gcode",                    ";Layer:[layer_num] ([layer_z] mm)" },
                { "layer_height",                   0.1 },
                { "initial_layer_print_height",             0.1 }
                });
			// End of the 1st object.
            std::string token = ";Layer:199 ";
			size_t pos = gcode.find(token);
			THEN("First and second object last layer is emitted") {
				// First object
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				double z = 0;
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE(z == Approx(20.));
				// Second object
				pos = gcode.find(";Layer:399 ", pos);
				REQUIRE(pos != std::string::npos);
				pos += token.size();
				REQUIRE(pos < gcode.size());
				REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
				REQUIRE(z == Approx(20.));
			}
        }
    }
}

// BBL timelapse: the H2D/H2C/H2S/P2S profiles carry the per-layer photo (M971 / M9711) only in
// time_lapse_gcode, so a non-i3 BBL machine must get that block on every layer, or the printer's
// timelapse flag has nothing to record. The X1/P1 profiles keep the photo in layer_change_gcode
// and leave time_lapse_gcode empty; i3 machines keep their own placement.
TEST_CASE("BBL time_lapse_gcode is emitted once per layer", "[PrintGCode][Timelapse]")
{
    auto slice_bbl = [](const char *structure, const char *time_lapse_gcode) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "printer_structure",          structure },
            { "time_lapse_gcode",           time_lapse_gcode },
            { "layer_change_gcode",         ";TEST_LAYER_CHANGE [layer_num]" },
            { "machine_start_gcode",        "" },
            { "layer_height",               0.2 },
            { "initial_layer_print_height", 0.2 },
        });
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        print.is_BBL_printer() = true;
        return Slic3r::Test::gcode(print);
    };
    auto count = [](const std::string &gcode, const std::string &token) {
        size_t n = 0;
        for (size_t pos = gcode.find(token); pos != std::string::npos; pos = gcode.find(token, pos + token.size()))
            ++n;
        return n;
    };
    // Tokens carry the leading newline so the config dump at the end of the file
    // ("; time_lapse_gcode = ;TEST_TIMELAPSE ...") is not counted.
    const char *marker = ";TEST_TIMELAPSE layer={layer_num} photo={most_used_physical_extruder_id} curr={curr_physical_extruder_id}";

    SECTION("core-xy machine: one photo block per layer, placeholders resolved") {
        std::string gcode  = slice_bbl("corexy", marker);
        size_t      layers = count(gcode, "\n;TEST_LAYER_CHANGE ");
        REQUIRE(layers == 100);
        REQUIRE(count(gcode, "\n;TEST_TIMELAPSE ") == layers);
        REQUIRE(gcode.find(";TEST_TIMELAPSE layer=0 photo=0 curr=0") != std::string::npos);
        // The photo block comes before the layer's own layer_change_gcode, as in BambuStudio.
        REQUIRE(gcode.find(";TEST_TIMELAPSE layer=0 ") < gcode.find(";TEST_LAYER_CHANGE 0"));
    }
    SECTION("profile without time_lapse_gcode (X1/P1): nothing added") {
        std::string gcode = slice_bbl("corexy", "");
        REQUIRE(count(gcode, "\n;TEST_TIMELAPSE ") == 0);
    }
    SECTION("i3 machine keeps its traditional placement: still one block per layer") {
        std::string gcode = slice_bbl("i3", marker);
        REQUIRE(count(gcode, "\n;TEST_TIMELAPSE ") == count(gcode, "\n;TEST_LAYER_CHANGE "));
    }
}

// Orca #15986 / Edge flow variants: pressure_advance is stored per Standard/High-Flow column,
// so filament id is not the array index once a filament declares both variants. Filament 2 is
// High-Flow with Standard=0.02 and High-Flow=0.05; raw get_at(1) reads the Standard slot.
namespace {

constexpr double kPaStdF0 = 0.01;
constexpr double kPaStdF1 = 0.02;
constexpr double kPaHfF1  = 0.05;

DynamicPrintConfig high_flow_pa_config(bool enable_std_f1, bool enable_hf_f1, bool adaptive)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.option<ConfigOptionBool>("gcode_comments")->value       = true;
    config.set_deserialize_strict({{"brim_type", "no_brim"},
                                   {"skirt_loops", "0"},
                                   {"wipe_tower_wall_type", "rectangle"},
                                   {"gcode_flavor", "marlin"},
                                   {"layer_height", "0.2"},
                                   {"initial_layer_print_height", "0.2"}});

    // F0 Standard-only (1 column) + F1 Standard/High-Flow (2 columns). Filament 2 is High-Flow,
    // so get_config_idx(..., 1) == 2 while get_at(1) still reads the Standard 0.02 slot.
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {1, 2};
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values =
        {FLOW_MODE_STANDARD, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW};
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard),
                                                                                     int(fvtHighFlow)};
    config.option<ConfigOptionFloats>("pressure_advance")->values                = {kPaStdF0, kPaStdF1, kPaHfF1};
    config.option<ConfigOptionBools>("enable_pressure_advance")->values          = {true, enable_std_f1, enable_hf_f1};
    config.option<ConfigOptionBools>("adaptive_pressure_advance")->values        = {adaptive, adaptive};
    config.option<ConfigOptionBools>("adaptive_pressure_advance_overhangs")->values = {adaptive, adaptive};
    if (adaptive)
        config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values = {"", ""};
    return config;
}

void require_high_flow_columns(const ConfigBase &config)
{
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 0);
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 2);
    const auto *pa = config.option<ConfigOptionFloats>("pressure_advance");
    REQUIRE(pa != nullptr);
    REQUIRE(pa->values.size() >= 3);
    REQUIRE_THAT(pa->get_at(1), Catch::Matchers::WithinAbs(kPaStdF1, 1e-9));
    REQUIRE_THAT(get_value_at(config, *pa, ConfigFlowDomain::Filament, 1), Catch::Matchers::WithinAbs(kPaHfF1, 1e-9));
}

std::string slice_high_flow_pa(DynamicPrintConfig config, bool bbl)
{
    Print print;
    Model model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);

    print.apply(model, config);
    print.is_BBL_printer() = bbl;
    REQUIRE(print.has_wipe_tower());
    require_high_flow_columns(print.config());
    return Test::gcode(print);
}

struct PaAfterT1 {
    size_t               toolchanges_to_f2 = 0;
    size_t               pa_commands       = 0;
    size_t               pa_high_flow      = 0;
    size_t               pa_standard_slot  = 0;
    std::vector<double>  values;
};

PaAfterT1 collect_pa_after_filament2(const std::string &gcode)
{
    static const std::regex pa_cmd(R"(^(?:M900 K|SET_PRESSURE_ADVANCE ADVANCE=)([0-9.eE+-]+))");
    static const std::regex tool_cmd(R"(^T(\d+)\s*(;.*)?$)");
    const double            tol = 1e-4;

    PaAfterT1   result;
    std::smatch m;
    int         current = -1;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (std::regex_match(line, m, tool_cmd)) {
            current = std::stoi(m[1].str());
            if (current == 1)
                ++result.toolchanges_to_f2;
            continue;
        }
        if (current != 1)
            continue;
        if (!std::regex_search(line, m, pa_cmd))
            continue;
        const double pa = std::stod(m[1].str());
        result.values.push_back(pa);
        ++result.pa_commands;
        if (std::fabs(pa - kPaHfF1) <= tol)
            ++result.pa_high_flow;
        if (std::fabs(pa - kPaStdF1) <= tol)
            ++result.pa_standard_slot;
    }
    return result;
}

void require_filament2_uses_high_flow_pa(const std::string &gcode, size_t min_pa_commands)
{
    const PaAfterT1 pa = collect_pa_after_filament2(gcode);
    INFO("T1 toolchanges " << pa.toolchanges_to_f2 << ", PA commands " << pa.pa_commands << ", HF " << pa.pa_high_flow
                           << ", std-slot " << pa.pa_standard_slot);
    REQUIRE(pa.toolchanges_to_f2 >= 2);
    REQUIRE(pa.pa_commands >= min_pa_commands);
    REQUIRE(pa.pa_standard_slot == 0);
    REQUIRE(pa.pa_high_flow == pa.pa_commands);
}

} // namespace

TEST_CASE("wipe-tower and set_extruder PA follow the High-Flow column", "[PrintGCode][GCode][PAVariant]")
{
    const DynamicPrintConfig config = high_flow_pa_config(true, true, false);
    require_high_flow_columns(config);

    SECTION("non-BBL wipe tower hits set_extruder and append_tcr2") {
        const std::string gcode = slice_high_flow_pa(config, false);
        REQUIRE(gcode.find("Travel to a Wipe Tower") != std::string::npos);
        // Two PA writes per T1 (set_extruder + wipe-tower L1178) across several layers.
        require_filament2_uses_high_flow_pa(gcode, 4);
    }
    SECTION("BBL wipe tower hits append_tcr") {
        const std::string gcode = slice_high_flow_pa(config, true);
        REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);
        require_filament2_uses_high_flow_pa(gcode, 2);
    }
}

TEST_CASE("enable_pressure_advance follows the High-Flow column", "[PrintGCode][GCode][PAVariant]")
{
    // Filament 2 Standard=false, High-Flow=true. get_at(1) is false, so the unfixed readers
    // skip PA entirely on every toolchange to filament 2.
    const DynamicPrintConfig config = high_flow_pa_config(false, true, false);
    require_high_flow_columns(config);

    SECTION("non-BBL wipe tower") {
        const std::string gcode = slice_high_flow_pa(config, false);
        require_filament2_uses_high_flow_pa(gcode, 4);
    }
    SECTION("BBL wipe tower") {
        const std::string gcode = slice_high_flow_pa(config, true);
        require_filament2_uses_high_flow_pa(gcode, 2);
    }
}

TEST_CASE("AdaptivePAProcessor base PA follows the High-Flow column", "[PrintGCode][GCode][PAVariant]")
{
    const DynamicPrintConfig config = high_flow_pa_config(true, true, true);
    require_high_flow_columns(config);
    const std::string gcode = slice_high_flow_pa(config, false);
    REQUIRE(gcode.find("PA_CHANGE") != std::string::npos);
    const PaAfterT1 pa = collect_pa_after_filament2(gcode);
    INFO("T1 toolchanges " << pa.toolchanges_to_f2 << ", PA commands " << pa.pa_commands << ", HF " << pa.pa_high_flow
                           << ", std-slot " << pa.pa_standard_slot);
    REQUIRE(pa.toolchanges_to_f2 >= 2);
    REQUIRE(pa.pa_commands >= 2);
    REQUIRE(pa.pa_standard_slot == 0);
    REQUIRE(pa.pa_high_flow >= 2);
}
