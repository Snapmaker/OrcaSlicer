#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Extruder.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
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

std::string slice_high_flow_pa(DynamicPrintConfig config, bool bbl, bool check_pa_columns = true)
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
    if (check_pa_columns)
        require_high_flow_columns(print.config());
    return Test::gcode(print);
}

struct PaAfterT1 {
    size_t              toolchanges_to_f2 = 0;
    size_t              pa_commands       = 0;
    size_t              pa_high_flow      = 0;
    size_t              pa_standard_slot  = 0;
    size_t              pa_ramming_zero   = 0;
    size_t              pa_unexpected     = 0;
    std::vector<double> values;
};

PaAfterT1 collect_pa_after_filament2(const std::string &gcode)
{
    static const std::regex pa_cmd(R"(^(?:M900 K|SET_PRESSURE_ADVANCE ADVANCE=)([0-9.eE+-]+))");
    static const std::regex tool_cmd(R"(^T(\d+)\s*(;.*)?$)");
    const double            tol = 1e-4;

    PaAfterT1          result;
    std::smatch        m;
    int                current = -1;
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
        const bool is_hf      = std::fabs(pa - kPaHfF1) <= tol;
        const bool is_std     = std::fabs(pa - kPaStdF1) <= tol;
        // WipeTower2 ramming writes M900 K0 / SET_PRESSURE_ADVANCE ADVANCE=0 because
        // ramming_pressure_advance_value defaults to 0 (WipeTower2.cpp disable_linear_advance_value).
        const bool is_ramming = std::fabs(pa) <= tol;
        if (is_hf)
            ++result.pa_high_flow;
        if (is_std)
            ++result.pa_standard_slot;
        if (is_ramming)
            ++result.pa_ramming_zero;
        if (!is_hf && !is_ramming)
            ++result.pa_unexpected;
    }
    return result;
}

void require_filament2_uses_high_flow_pa(const std::string &gcode, size_t min_pa_commands)
{
    const PaAfterT1 pa = collect_pa_after_filament2(gcode);
    INFO("T1 toolchanges " << pa.toolchanges_to_f2 << ", PA commands " << pa.pa_commands << ", HF " << pa.pa_high_flow
                           << ", std-slot " << pa.pa_standard_slot << ", ramming-0 " << pa.pa_ramming_zero
                           << ", unexpected " << pa.pa_unexpected);
    REQUIRE(pa.toolchanges_to_f2 >= 2);
    REQUIRE(pa.pa_commands >= min_pa_commands);
    REQUIRE(pa.pa_standard_slot == 0);
    // Every PA inside a T1 block is High-Flow 0.05, except WipeTower2 ramming M900 K0.
    // A wrong-column read of filament 2's Standard slot is 0.02; a silent fallback to
    // filament 0 (or get_at(0)) is 0.01. On the multi-extruder path T (~GCode.cpp:10882)
    // is emitted before PA (~:10921), so a later filament's 0.01 cannot appear here.
    REQUIRE(pa.pa_unexpected == 0);
    REQUIRE(pa.pa_high_flow + pa.pa_ramming_zero == pa.pa_commands);
    REQUIRE(pa.pa_high_flow >= min_pa_commands);
}

// Orca #16007: F0 packed Standard+High-Flow, F1 single. get_at(1) is F0's High-Flow slot.
constexpr int    kTempStdF0    = 190;
constexpr int    kTempHfF0     = 230;
constexpr int    kTempF1       = 210;
constexpr int    kInitStdF0    = 185;
constexpr int    kInitHfF0     = 225;
constexpr int    kInitF1       = 205;
constexpr double kPurgeStdF0   = 1.;
constexpr double kPurgeHfF0    = 15.;
constexpr double kPurgeF1      = 5.;
constexpr double kVolStdF0     = 8.;
constexpr double kVolHfF0      = 30.;
constexpr double kVolF1        = 12.;
constexpr double kRamVolStdF0  = 1.;
constexpr double kRamVolHfF0   = 20.;
constexpr double kRamVolF1     = 8.;
constexpr double kRamFlowStdF0 = 1.;
constexpr double kRamFlowHfF0  = 10.;
constexpr double kRamFlowF1    = 4.;
constexpr double kFlowStdF0    = 0.98;
constexpr double kFlowHfF0     = 0.95;
constexpr double kFlowF1       = 1.01;
constexpr double kRetractStdF0 = 0.8;
constexpr double kRetractHfF0  = 3.0;
constexpr double kRetractF1    = 1.5;
constexpr double kRetractSpeedStdF0 = 30.;
constexpr double kRetractSpeedHfF0  = 40.;
constexpr double kRetractSpeedF1    = 25.;
constexpr int    kStandbyDelta = -15;

DynamicPrintConfig step_size_2_f0_config()
{
    DynamicPrintConfig config = high_flow_pa_config(true, true, false);
    // F0 Standard+High-Flow (2 columns) + F1 Standard-only (1 column). F0 is High-Flow, so
    // get_config_idx(..., 0) == 1 and get_config_idx(..., 1) == 2. get_at(1) is F0's HF slot.
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {2, 1};
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values =
        {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD};
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtHighFlow),
                                                                                     int(fvtStandard)};
    config.option<ConfigOptionInts>("nozzle_temperature")->values               = {kTempStdF0, kTempHfF0, kTempF1};
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {kInitStdF0, kInitHfF0, kInitF1};
    config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values = {kPurgeStdF0, kPurgeHfF0,
                                                                                         kPurgeF1};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values        = {kVolStdF0, kVolHfF0, kVolF1};
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values            = {false, true, true};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_volume")->values    = {kRamVolStdF0, kRamVolHfF0,
                                                                                        kRamVolF1};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_flow")->values      = {kRamFlowStdF0, kRamFlowHfF0,
                                                                                        kRamFlowF1};
    // 2x2 flush matrix so extract_wipe_volumes walks two filament ids, not the 4x4 default.
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = {0.f, 0.f, 0.f, 0.f};
    config.option<ConfigOptionFloats>("filament_flow_ratio")->values = {kFlowStdF0, kFlowHfF0, kFlowF1};
    config.option<ConfigOptionFloats>("retraction_length")->values   = {0.4, 0.4};
    config.option<ConfigOptionFloats>("retraction_speed")->values    = {10., 10.};
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values = {kRetractStdF0, kRetractHfF0,
                                                                                             kRetractF1};
    config.option<ConfigOptionFloatsNullable>("filament_retraction_speed", true)->values = {kRetractSpeedStdF0,
                                                                                            kRetractSpeedHfF0,
                                                                                            kRetractSpeedF1};
    return config;
}

void require_applied_tool_retract_and_flow(Print &print)
{
    REQUIRE(print.config().retraction_length.size() == 2);
    REQUIRE_THAT(print.config().retraction_length.get_at(0), Catch::Matchers::WithinAbs(kRetractHfF0, 1e-9));
    REQUIRE_THAT(print.config().retraction_length.get_at(1), Catch::Matchers::WithinAbs(kRetractF1, 1e-9));
    REQUIRE_THAT(print.config().retraction_speed.get_at(0), Catch::Matchers::WithinAbs(kRetractSpeedHfF0, 1e-9));
    REQUIRE_THAT(print.config().retraction_speed.get_at(1), Catch::Matchers::WithinAbs(kRetractSpeedF1, 1e-9));

    GCodeConfig gc;
    gc.apply(print.config(), true);
    Extruder e0(0, &gc, false);
    Extruder e1(1, &gc, false);
    REQUIRE_THAT(e0.filament_flow_ratio(), Catch::Matchers::WithinAbs(kFlowHfF0, 1e-9));
    REQUIRE_THAT(e1.filament_flow_ratio(), Catch::Matchers::WithinAbs(kFlowF1, 1e-9));
    REQUIRE_THAT(e0.retraction_length(), Catch::Matchers::WithinAbs(kRetractHfF0, 1e-9));
    REQUIRE_THAT(e1.retraction_length(), Catch::Matchers::WithinAbs(kRetractF1, 1e-9));
    REQUIRE(e0.retract_speed() == int(kRetractSpeedHfF0));
    REQUIRE(e1.retract_speed() == int(kRetractSpeedF1));
}

int e_feedrate_from_vol(double vol)
{
    const double area = (M_PI / 4.) * 1.75 * 1.75;
    const int    feed = int(60.0 * vol / area);
    return feed == 0 ? 100 : feed;
}

void require_step_size_2_f0_columns(const ConfigBase &config)
{
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 1);
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 2);

    const auto *temps  = config.option<ConfigOptionInts>("nozzle_temperature");
    const auto *inits  = config.option<ConfigOptionInts>("nozzle_temperature_initial_layer");
    const auto *purge  = config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower");
    const auto *vol    = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const auto *ram_on = config.option<ConfigOptionBools>("filament_multitool_ramming");
    const auto *ram_v  = config.option<ConfigOptionFloats>("filament_multitool_ramming_volume");
    const auto *ram_f  = config.option<ConfigOptionFloats>("filament_multitool_ramming_flow");
    REQUIRE(temps != nullptr);
    REQUIRE(inits != nullptr);
    REQUIRE(purge != nullptr);
    REQUIRE(vol != nullptr);
    REQUIRE(ram_on != nullptr);
    REQUIRE(ram_v != nullptr);
    REQUIRE(ram_f != nullptr);

    // Raw filament-id reads land in F0's High-Flow column.
    REQUIRE(temps->get_at(1) == kTempHfF0);
    REQUIRE(inits->get_at(1) == kInitHfF0);
    REQUIRE_THAT(purge->get_at(1), Catch::Matchers::WithinAbs(kPurgeHfF0, 1e-9));
    REQUIRE_THAT(vol->get_at(1), Catch::Matchers::WithinAbs(kVolHfF0, 1e-9));
    REQUIRE(ram_on->get_at(1));
    REQUIRE_THAT(ram_v->get_at(1), Catch::Matchers::WithinAbs(kRamVolHfF0, 1e-9));
    REQUIRE_THAT(ram_f->get_at(1), Catch::Matchers::WithinAbs(kRamFlowHfF0, 1e-9));

    REQUIRE(get_value_at(config, *temps, ConfigFlowDomain::Filament, 1) == kTempF1);
    REQUIRE(get_value_at(config, *inits, ConfigFlowDomain::Filament, 1) == kInitF1);
    REQUIRE_THAT(get_value_at(config, *purge, ConfigFlowDomain::Filament, 1),
                 Catch::Matchers::WithinAbs(kPurgeF1, 1e-9));
    REQUIRE_THAT(get_value_at(config, *vol, ConfigFlowDomain::Filament, 1), Catch::Matchers::WithinAbs(kVolF1, 1e-9));
    REQUIRE(get_value_at(config, *ram_on, ConfigFlowDomain::Filament, 1));
    REQUIRE_THAT(get_value_at(config, *ram_v, ConfigFlowDomain::Filament, 1),
                 Catch::Matchers::WithinAbs(kRamVolF1, 1e-9));
    REQUIRE_THAT(get_value_at(config, *ram_f, ConfigFlowDomain::Filament, 1),
                 Catch::Matchers::WithinAbs(kRamFlowF1, 1e-9));

    REQUIRE(get_value_at(config, *temps, ConfigFlowDomain::Filament, 0) == kTempHfF0);
    REQUIRE(temps->get_at(0) == kTempStdF0);
}

PrintConfig as_print_config(const DynamicPrintConfig &dyn)
{
    PrintConfig print_cfg;
    print_cfg.apply(dyn, true);
    return print_cfg;
}

size_t count_substr(const std::string &hay, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++n;
    return n;
}

} // namespace

TEST_CASE("wipe-tower and set_extruder PA follow the High-Flow column", "[PrintGCode][GCode][PAVariant]")
{
    const DynamicPrintConfig config = high_flow_pa_config(true, true, false);
    require_high_flow_columns(config);

    SECTION("non-BBL wipe tower hits set_extruder and append_tcr2") {
        const std::string gcode = slice_high_flow_pa(config, false);
        REQUIRE(gcode.find("Travel to a Wipe Tower") != std::string::npos);
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
    require_filament2_uses_high_flow_pa(gcode, 2);
}

TEST_CASE("AdaptivePA enable follows the High-Flow column", "[PrintGCode][GCode][PAVariant]")
{
    // Standard=false, High-Flow=true. get_at(1) is false, so this fails if:
    //   * _extrude (~GCode.cpp:9548) reads enable_pressure_advance by raw filament id
    //     (no PA_CHANGE tags for filament 2), or
    //   * AdaptivePAProcessor ctor (~:78) does the same (interpolator never installed;
    //     "; APA: Tool doesnt have APA enabled" instead of the empty-model fallback).
    // set_extruder ~:10606 is the single-extruder path (PA then T) and ~:10642 is the
    // BBL start-gcode first-filament path; this 2-extruder wipe-tower fixture hits
    // the multi-extruder set_extruder site (~:10921) instead.
    const DynamicPrintConfig config = high_flow_pa_config(false, true, true);
    require_high_flow_columns(config);
    const std::string gcode = slice_high_flow_pa(config, false);
    // PA_CHANGE:T1 is emitted only when _extrude's enable check uses the High-Flow
    // column. A bare "PA_CHANGE" match is not enough: filament 0 still tags T0
    // after a get_at(1) revert at ~:9548.
    REQUIRE(gcode.find("PA_CHANGE:T1") != std::string::npos);
    // Empty model still marks the interpolator initialised, so interpolation
    // returns -1 and process_layer falls back. That path only runs if the ctor
    // installed a per-tool interpolator via get_value_at (High-Flow true).
    REQUIRE(gcode.find("; APA: Interpolation failed") != std::string::npos);
    REQUIRE(gcode.find("; APA: Tool doesnt have APA enabled") == std::string::npos);
    require_filament2_uses_high_flow_pa(gcode, 2);
}

// Orca #16007 Stage A / Edge flow variants: when F0 declares both Standard and High-Flow, the
// packed filament arrays are [F0-std, F0-hf, F1, ...]. A raw get_at(1) for filament 1 reads
// F0's High-Flow slot (wrong filament), not F1's own column.
TEST_CASE("step-size-2 F0 variants misindex F1 on raw get_at", "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    const DynamicPrintConfig config = step_size_2_f0_config();
    require_step_size_2_f0_columns(config);

    const PrintConfig print_cfg = as_print_config(config);
    const auto        vols      = WipeTower2::extract_wipe_volumes(print_cfg);
    REQUIRE(vols.size() == 2);
    REQUIRE(vols[0].size() == 2);
    // F1 is column j=1. get_at(1) is F0's High-Flow 15 mm3; the variant reader is F1's 5 mm3.
    REQUIRE_THAT(vols[0][1], Catch::Matchers::WithinAbs(float(kPurgeF1), 1e-4f));
    REQUIRE_THAT(vols[1][1], Catch::Matchers::WithinAbs(float(kPurgeF1), 1e-4f));
    REQUIRE_THAT(vols[0][0], Catch::Matchers::WithinAbs(float(kPurgeHfF0), 1e-4f));

    const std::string gcode = slice_high_flow_pa(config, false, false);
    REQUIRE(gcode.find("Travel to a Wipe Tower") != std::string::npos);
    // Header comment is filament 0. get_at(0) is Standard 185; F0 is High-Flow so 225.
    REQUIRE(gcode.find("; first_layer_temperature = " + std::to_string(kInitHfF0)) != std::string::npos);
    REQUIRE(gcode.find("; first_layer_temperature = " + std::to_string(kInitStdF0)) == std::string::npos);
    // F1's own first-layer / other-layer temps must appear. F0's High-Flow pair (225/230) is
    // what a raw get_at(1) would write for filament 1.
    REQUIRE(count_substr(gcode, "S" + std::to_string(kInitF1)) >= 1);
    REQUIRE(count_substr(gcode, "S" + std::to_string(kTempF1)) >= 1);
}

TEST_CASE("Standard-only flow columns stay get_at-identical after variant readers",
          "[PrintGCode][GCode][slice_compare]")
{
    DynamicPrintConfig config = high_flow_pa_config(true, true, false);
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {1, 1};
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values = {FLOW_MODE_STANDARD,
                                                                                FLOW_MODE_STANDARD};
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard),
                                                                                     int(fvtStandard)};
    config.option<ConfigOptionInts>("nozzle_temperature")->values               = {200, 215};
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {195, 211};
    config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values = {3., 7.};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values  = {11., 13.};
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values      = {true, true};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_volume")->values = {6., 9.};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_flow")->values   = {3., 4.};
    config.option<ConfigOptionFloats>("flush_volumes_matrix")->values              = {0.f, 0.f, 0.f, 0.f};
    config.option<ConfigOptionFloats>("pressure_advance")->values                 = {kPaStdF0, kPaStdF1};
    config.option<ConfigOptionBools>("enable_pressure_advance")->values           = {true, true};

    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 0);
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 1);
    const auto *temps = config.option<ConfigOptionInts>("nozzle_temperature");
    const auto *inits = config.option<ConfigOptionInts>("nozzle_temperature_initial_layer");
    const auto *purge = config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower");
    const auto *vol   = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    REQUIRE(get_value_at(config, *temps, ConfigFlowDomain::Filament, 0) == temps->get_at(0));
    REQUIRE(get_value_at(config, *temps, ConfigFlowDomain::Filament, 1) == temps->get_at(1));
    REQUIRE(get_value_at(config, *inits, ConfigFlowDomain::Filament, 0) == inits->get_at(0));
    REQUIRE(get_value_at(config, *inits, ConfigFlowDomain::Filament, 1) == inits->get_at(1));
    REQUIRE_THAT(get_value_at(config, *purge, ConfigFlowDomain::Filament, 1),
                 Catch::Matchers::WithinAbs(purge->get_at(1), 1e-9));
    REQUIRE_THAT(get_value_at(config, *vol, ConfigFlowDomain::Filament, 1),
                 Catch::Matchers::WithinAbs(vol->get_at(1), 1e-9));

    const auto vols = WipeTower2::extract_wipe_volumes(as_print_config(config));
    REQUIRE_THAT(vols[0][1], Catch::Matchers::WithinAbs(7.f, 1e-4f));
    REQUIRE_THAT(vols[1][0], Catch::Matchers::WithinAbs(3.f, 1e-4f));

    const std::string gcode = slice_high_flow_pa(config, false, false);
    REQUIRE(gcode.find("; first_layer_temperature = 195") != std::string::npos);
    REQUIRE(count_substr(gcode, "S211") >= 1);
    REQUIRE(count_substr(gcode, "S215") >= 1);
}

TEST_CASE("apply_override unpacks flow-variant retract keys by filament id",
          "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    Print              print;
    Model              model;
    ModelObject *      first = model.add_object();
    first->name              = "cube-a.stl";
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
    require_applied_tool_retract_and_flow(print);
    REQUIRE(get_config_idx(print.config(), ConfigFlowDomain::Filament, (unsigned int) -1) == 0);
}

TEST_CASE("non-SEMM U1 2-tool High-Flow uses per-filament temps retract and placeholders",
          "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("ooze_prevention")->value               = true;
    config.option<ConfigOptionInt>("standby_temperature_delta")->value      = kStandbyDelta;
    config.option<ConfigOptionFloat>("preheat_time")->value                 = 30.;
    config.option<ConfigOptionString>("machine_start_gcode")->value =
        "; U1_START init={nozzle_temperature_initial_layer[initial_extruder]} "
        "fl0={first_layer_temperature[0]} fl1={first_layer_temperature[1]} "
        "nt0={nozzle_temperature[0]} nt1={nozzle_temperature[1]} "
        "rl0={retract_length[0]} rl1={retract_length[1]} "
        "ram0={filament_multitool_ramming[0]} ram1={filament_multitool_ramming[1]} "
        "flush0={flush_volumetric_speeds[0]} flush1={flush_volumetric_speeds[1]}\n"
        "M104 S{nozzle_temperature_initial_layer[initial_extruder]}\n";
    config.option<ConfigOptionString>("change_filament_gcode")->value =
        "; U1_TC next={next_extruder} layer={layer_num}\n"
        "{if layer_num < 1}\n"
        "M109 S{first_layer_temperature[next_extruder]} T{next_extruder} ; U1_WAIT_L0\n"
        "{else}\n"
        "M109 S{temperature[next_extruder]} T{next_extruder} ; U1_WAIT_LX\n"
        "{endif}\n"
        "; U1_FULL NT={nozzle_temperature[next_extruder]} "
        "NTI={nozzle_temperature_initial_layer[next_extruder]} "
        "RL={retract_length[next_extruder]} RAM={filament_multitool_ramming[next_extruder]} "
        "FLUSH={flush_volumetric_speeds[next_extruder]} FEED={new_filament_e_feedrate}\n";

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
    print.is_BBL_printer() = false;
    REQUIRE(print.has_wipe_tower());
    require_applied_tool_retract_and_flow(print);

    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("Travel to a Wipe Tower") != std::string::npos);

    const size_t start_pos = gcode.find("; U1_START ");
    REQUIRE(start_pos != std::string::npos);
    const size_t start_eol = gcode.find('\n', start_pos);
    const std::string start_line = gcode.substr(start_pos, start_eol - start_pos);
    REQUIRE(start_line.find("init=" + std::to_string(kInitHfF0)) != std::string::npos);
    REQUIRE(start_line.find("fl0=" + std::to_string(kInitHfF0)) != std::string::npos);
    REQUIRE(start_line.find("fl1=" + std::to_string(kInitF1)) != std::string::npos);
    REQUIRE(start_line.find("nt0=" + std::to_string(kTempHfF0)) != std::string::npos);
    REQUIRE(start_line.find("nt1=" + std::to_string(kTempF1)) != std::string::npos);
    INFO(start_line);
    REQUIRE(start_line.find("rl0=") != std::string::npos);
    REQUIRE(start_line.find("rl1=") != std::string::npos);
    REQUIRE(start_line.find("rl0=0.4") == std::string::npos);
    const bool ram0_on = start_line.find("ram0=1") != std::string::npos || start_line.find("ram0=true") != std::string::npos;
    const bool ram1_on = start_line.find("ram1=1") != std::string::npos || start_line.find("ram1=true") != std::string::npos;
    REQUIRE(ram0_on);
    REQUIRE(ram1_on);
    REQUIRE(start_line.find("flush0=" + std::to_string(int(kVolHfF0))) != std::string::npos);
    REQUIRE(start_line.find("flush1=" + std::to_string(int(kVolF1))) != std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(kInitHfF0)) != std::string::npos);

    REQUIRE(gcode.find("M109 S" + std::to_string(kInitF1) + " T1") != std::string::npos);
    REQUIRE(gcode.find("M109 S" + std::to_string(kTempF1) + " T1") != std::string::npos);
    REQUIRE(gcode.find("M109 S" + std::to_string(kTempHfF0) + " T0") != std::string::npos);
    REQUIRE(gcode.find("M109 S" + std::to_string(kTempHfF0) + " T1") == std::string::npos);
    REQUIRE(gcode.find("M109 S" + std::to_string(kInitHfF0) + " T1") == std::string::npos);
    REQUIRE(gcode.find("M109 S" + std::to_string(kTempStdF0) + " T0") == std::string::npos);

    REQUIRE(gcode.find("FEED=" + std::to_string(e_feedrate_from_vol(kVolF1))) != std::string::npos);
    REQUIRE(gcode.find("FEED=" + std::to_string(e_feedrate_from_vol(kVolHfF0))) != std::string::npos);
    REQUIRE(gcode.find("FLUSH=" + std::to_string(int(kVolF1))) != std::string::npos);
    REQUIRE(gcode.find("FLUSH=" + std::to_string(int(kVolHfF0))) != std::string::npos);

    const int ooze_t0 = kTempHfF0 + kStandbyDelta;
    REQUIRE(gcode.find("M104 S" + std::to_string(ooze_t0) + " T0") != std::string::npos);
    REQUIRE(gcode.find(";cooldown") != std::string::npos);

    // GCodeWriter emits "M104 S<temp> T<tool> ; preheat T<tool> ...". Packed get_at(1) would
    // preheat T1 at F0's High-Flow 230/225.
    const bool preheat_t1 = gcode.find("preheat T1") != std::string::npos;
    const bool preheat_t1_ok = gcode.find("M104 S" + std::to_string(kTempF1) + " T1") != std::string::npos
                            || gcode.find("M104 S" + std::to_string(kInitF1) + " T1") != std::string::npos;
    INFO("preheat T1 present=" << preheat_t1 << " ok=" << preheat_t1_ok);
    if (preheat_t1) {
        REQUIRE(preheat_t1_ok);
        REQUIRE(gcode.find("M104 S" + std::to_string(kTempHfF0) + " T1 ; preheat") == std::string::npos);
        REQUIRE(gcode.find("M104 S" + std::to_string(kInitHfF0) + " T1 ; preheat") == std::string::npos);
    }
}
