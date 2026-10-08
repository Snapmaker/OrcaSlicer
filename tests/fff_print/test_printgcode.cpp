#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Extruder.hpp"
// ToolOrdering.hpp must come after Print.hpp (it relies on Print.hpp's forward declarations,
// e.g. ExtrusionEntity), so it is not included on its own here; test_data.hpp pulls in Print.hpp.
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetFlowVariant.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
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
constexpr int    kStandbyDelta      = -15;
constexpr double kFlowT1StdPacked   = 1.40;
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

void add_two_tool_cubes(Model &model)
{
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
}

std::string g1_feed_fingerprint(const std::string &gcode)
{
    std::ostringstream os;
    std::istringstream in(gcode);
    std::string        line;
    static const std::regex f_re(R"(\bF([0-9]+))");
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "G1") != 0)
            continue;
        std::smatch m;
        if (std::regex_search(line, m, f_re))
            os << 'F' << m[1].str() << '\n';
    }
    return os.str();
}

size_t count_g1_feed(const std::string &gcode, int feed)
{
    size_t             n = 0;
    std::istringstream in(gcode);
    std::string        line;
    static const std::regex f_re(R"(\bF([0-9]+))");
    const std::string       want = std::to_string(feed);
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "G1") != 0)
            continue;
        std::smatch m;
        if (std::regex_search(line, m, f_re) && m[1].str() == want)
            ++n;
    }
    return n;
}

std::map<int, size_t> g1_feed_histogram(const std::string &gcode)
{
    std::map<int, size_t>    counts;
    std::istringstream       in(gcode);
    std::string              line;
    static const std::regex  f_re(R"(\bF([0-9]+))");
    while (std::getline(in, line)) {
        if (line.compare(0, 2, "G1") != 0)
            continue;
        std::smatch m;
        if (std::regex_search(line, m, f_re))
            ++counts[std::stoi(m[1].str())];
    }
    return counts;
}

void raise_role_speeds_for_mvs_cap(DynamicPrintConfig &config)
{
    // Push role speeds above the HF volumetric cap so _extrude F and wipe-tower F
    // bind on filament_max_volumetric_speed / flow_ratio.
    const char *keys[] = {"outer_wall_speed", "inner_wall_speed", "sparse_infill_speed",
                          "internal_solid_infill_speed", "top_surface_speed", "gap_infill_speed",
                          "support_speed", "travel_speed", "initial_layer_speed",
                          "initial_layer_infill_speed"};
    for (const char *key : keys)
        if (auto *opt = config.option<ConfigOptionFloats>(key))
            opt->values.assign(std::max<size_t>(1, opt->values.size()), 200.);
}

void disable_layer_cooling(DynamicPrintConfig &config)
{
    if (auto *opt = config.option<ConfigOptionBools>("slow_down_for_layer_cooling"))
        opt->values.assign(std::max<size_t>(1, opt->values.size()), false);
    if (auto *opt = config.option<ConfigOptionFloats>("fan_cooling_layer_time"))
        opt->values.assign(std::max<size_t>(1, opt->values.size()), 0.);
    if (auto *opt = config.option<ConfigOptionInts>("slow_down_layers"))
        opt->values.assign(std::max<size_t>(1, opt->values.size()), 0);
}

size_t count_substr(const std::string &hay, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = 0; (pos = hay.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++n;
    return n;
}

// Count "Travel to a Wipe Tower" whose next U1_TC is next=<id>. append_tcr2 ~1097
// travels before set_extruder only when the departing tool rams.
size_t count_travel_then_tc_next(const std::string &gcode, int next)
{
    const std::string travel = "Travel to a Wipe Tower";
    const std::string marker = "; U1_TC next=" + std::to_string(next);
    size_t            n      = 0;
    for (size_t pos = 0; (pos = gcode.find(travel, pos)) != std::string::npos; pos += travel.size()) {
        const size_t tc = gcode.find("; U1_TC next=", pos);
        if (tc != std::string::npos && tc < pos + 2500 && gcode.compare(tc, marker.size(), marker) == 0)
            ++n;
    }
    return n;
}

void apply_u1_toolchange_markers(DynamicPrintConfig &config)
{
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionFloat>("preheat_time")->value                 = 30.;
    config.option<ConfigOptionString>("machine_start_gcode")->value =
        "; U1_START init={nozzle_temperature_initial_layer[initial_extruder]} "
        "fl0={first_layer_temperature[0]} fl1={first_layer_temperature[1]} "
        "nt0={nozzle_temperature[0]} nt1={nozzle_temperature[1]} "
        "rl0={retract_length[0]} rl1={retract_length[1]} "
        "ram0={filament_multitool_ramming[0]} ram1={filament_multitool_ramming[1]} "
        "flush0={flush_volumetric_speeds[0]} flush1={flush_volumetric_speeds[1]}\n";
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
}

std::string slice_u1_two_tool(DynamicPrintConfig config)
{
    Print print;
    Model model;
    add_two_tool_cubes(model);
    print.apply(model, config);
    print.is_BBL_printer() = false;
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
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

TEST_CASE("Adaptive PA processor skips the line scan when no flow-variant column has adaptive PA", "[PrintGCode][GCode][PAVariant][AdaptivePA]")
{
    // High-Flow PA is on, adaptive is off for every column. Tool changes still emit
    // in-band PA_RESET; the early-out must strip those without emitting PA_CHANGE.
    const DynamicPrintConfig config = high_flow_pa_config(false, true, false);
    require_high_flow_columns(config);
    const std::string gcode = slice_high_flow_pa(config, false);
    REQUIRE(gcode.find("PA_CHANGE") == std::string::npos);
    REQUIRE(gcode.find("PA_RESET") == std::string::npos);
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
    add_two_tool_cubes(model);

    print.apply(model, config);
    require_applied_tool_retract_and_flow(print);
    REQUIRE(get_config_idx(print.config(), ConfigFlowDomain::Filament, (unsigned int) -1) == 0);
}

TEST_CASE("unpack_filament_flow_override falls back on an empty filament override",
          "[PrintGCode][GCode][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    config.option<ConfigOptionFloatsNullable>("filament_retraction_length", true)->values.clear();
    Print print;
    Model model;
    add_two_tool_cubes(model);
    REQUIRE_NOTHROW(print.apply(model, config));
    REQUIRE(print.config().retraction_length.size() == 2);
}

TEST_CASE("non-SEMM U1 2-tool High-Flow uses per-filament temps retract and placeholders",
          "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    // Ooze standbys T1 to 205-15=190, which hides the first-layer writer's M104 S205 T1
    // and skips idle tools in the layer-2 writer. Keep it off so those two sites emit
    // the per-filament temps the mutation table checks.
    config.option<ConfigOptionBool>("ooze_prevention")->value               = false;
    config.option<ConfigOptionFloat>("preheat_time")->value                 = 30.;
    raise_role_speeds_for_mvs_cap(config);
    disable_layer_cooling(config);
    // No M104/M109 in start G-code so _print_first_layer_extruder_temperatures emits
    // M104 S205 T1 (F1 init), not packed F0 HF S225.
    config.option<ConfigOptionString>("machine_start_gcode")->value =
        "; U1_START init={nozzle_temperature_initial_layer[initial_extruder]} "
        "fl0={first_layer_temperature[0]} fl1={first_layer_temperature[1]} "
        "nt0={nozzle_temperature[0]} nt1={nozzle_temperature[1]} "
        "rl0={retract_length[0]} rl1={retract_length[1]} "
        "ram0={filament_multitool_ramming[0]} ram1={filament_multitool_ramming[1]} "
        "flush0={flush_volumetric_speeds[0]} flush1={flush_volumetric_speeds[1]}\n";
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
    add_two_tool_cubes(model);

    print.apply(model, config);
    print.is_BBL_printer() = false;
    REQUIRE(print.has_wipe_tower());
    require_applied_tool_retract_and_flow(print);

    const std::string gcode = Test::gcode(print);
    {
        const auto         hist = g1_feed_histogram(gcode);
        std::ostringstream hs;
        for (const auto &kv : hist)
            hs << " F" << kv.first << "x" << kv.second;
        INFO("G1 F histogram:" << hs.str());
    }
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

    // GCodeWriter emits "M104 S<temp> T<tool> ; preheat T<tool> ...". Packed get_at(1) would
    // preheat T1 at F0's High-Flow 230/225.
    REQUIRE(gcode.find("preheat T1") != std::string::npos);
    const bool preheat_t1_ok = gcode.find("M104 S" + std::to_string(kTempF1) + " T1 ; preheat") != std::string::npos
                            || gcode.find("M104 S" + std::to_string(kInitF1) + " T1 ; preheat") != std::string::npos;
    REQUIRE(preheat_t1_ok);
    REQUIRE(gcode.find("M104 S" + std::to_string(kTempHfF0) + " T1 ; preheat") == std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(kInitHfF0) + " T1 ; preheat") == std::string::npos);

    // First-layer writer (~4629-4663), wait=false: "M104 S<temp> T<tool> ; set nozzle temperature".
    // Packed get_at(1) writes S225 T1. U1_WAIT M109 S205 T1 is a different comment.
    REQUIRE(gcode.find("M104 S" + std::to_string(kInitF1) + " T1 ; set nozzle temperature") != std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(kInitHfF0) + " T1") == std::string::npos);

    // Layer-2 writer (~6067-6092): other-layer temps, not the U1_WAIT M109 placeholders.
    REQUIRE(gcode.find("M104 S" + std::to_string(kTempHfF0) + " T0 ; set nozzle temperature") != std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(kTempF1) + " T1 ; set nozzle temperature") != std::string::npos);

    // append_tcr2 ramming (~1097): T0 High-Flow ramming forces travel to the tower.
    REQUIRE(count_substr(gcode, "Travel to a Wipe Tower") >= 2);

    // _extrude MVS (~9358) and N2/S5 flow-ratio cap (~9234). F0 HF MVS 30 is above the
    // 200 mm/s role speed, so the cap binds on F1 (MVS 12 / flow 1.01 → G1 F8755.932).
    // get_at(0) flow 0.98 → F9024; packed HF flow 0.95 → F9309; get_at(0) MVS 8 → F5837.
    REQUIRE(count_g1_feed(gcode, 8755) >= 1);
    REQUIRE(count_g1_feed(gcode, 9024) == 0);
    REQUIRE(count_g1_feed(gcode, 9309) == 0);
    REQUIRE(count_g1_feed(gcode, 5837) == 0);

    // WipeTower2 MVS (~1568): F1 wipe at width 0.5 is F7876 (12 mm³/s). get_at(0) MVS 8 → F5251.
    REQUIRE(count_g1_feed(gcode, 7876) >= 1);
    REQUIRE(count_g1_feed(gcode, 5251) == 0);

    // WipeTower2 ramming (~1587-1590): F0 HF flow 10 → G1 F3135; F1 flow 4 → G1 F1254.
    // Packed get_at(0) flow 1 → F313. Turnaround travel is hardcoded F7200 while ramming.
    REQUIRE(count_g1_feed(gcode, 3135) >= 1);
    REQUIRE(count_g1_feed(gcode, 1254) >= 1);
    REQUIRE(count_g1_feed(gcode, 313) == 0);
    REQUIRE(count_g1_feed(gcode, 7200) >= 10);
}

TEST_CASE("non-variant 2-filament flow_ratio keeps get_at(0) _extrude cap",
          "[PrintGCode][GCode][FilamentVariants][slice_compare]")
{
    auto make = [](double ratio_f1) {
        DynamicPrintConfig config = high_flow_pa_config(true, true, false);
        config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {1, 1};
        config.option<ConfigOptionStrings>("filament_flow_support", true)->values =
            {FLOW_MODE_STANDARD, FLOW_MODE_STANDARD};
        config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard),
                                                                                         int(fvtStandard)};
        config.option<ConfigOptionFloats>("filament_flow_ratio")->values = {kFlowStdF0, ratio_f1};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {kVolStdF0, kVolStdF0};
        config.option<ConfigOptionFloats>("pressure_advance")->values              = {kPaStdF0, kPaStdF1};
        config.option<ConfigOptionBools>("enable_pressure_advance")->values        = {true, true};
        disable_layer_cooling(config);
        raise_role_speeds_for_mvs_cap(config);
        REQUIRE_FALSE(filament_flow_variants_active(config));
        return slice_high_flow_pa(config, false, false);
    };

    const std::string g_same = make(kFlowStdF0);
    const std::string g_diff = make(1.40);
    const std::string fp_same = g1_feed_fingerprint(g_same);
    const std::string fp_diff = g1_feed_fingerprint(g_diff);
    INFO(fp_same.size() << " vs " << fp_diff.size());
    REQUIRE_FALSE(fp_same.empty());
    REQUIRE(fp_same == fp_diff);
}

// S7: T0 Standard-only (TPU/PC) + T1 [std,hf] set to Standard. Nothing remaps
// (get_config_idx is 0/1), but T1's ratio is packed slot 1. The old remap-only
// gate used T0's get_at(0) for T1's MVS / M73 cap.
TEST_CASE("T0 Standard-only plus T1 packed-std uses T1 flow_ratio cap",
          "[PrintGCode][GCode][FilamentVariants][slice_compare]")
{
    DynamicPrintConfig config = high_flow_pa_config(true, true, false);
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard),
                                                                                     int(fvtStandard)};
    config.option<ConfigOptionFloats>("filament_flow_ratio")->values = {kFlowStdF0, kFlowT1StdPacked, kFlowHfF0};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {kVolHfF0, kVolF1, kVolF1};
    config.option<ConfigOptionFloats>("pressure_advance")->values              = {kPaStdF0, kPaStdF1, kPaHfF1};
    config.option<ConfigOptionBools>("enable_pressure_advance")->values        = {true, true, true};
    disable_layer_cooling(config);
    raise_role_speeds_for_mvs_cap(config);

    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 0);
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 1);
    REQUIRE(filament_flow_variants_active(config));

    apply_u1_toolchange_markers(config);
    const std::string gcode = slice_u1_two_tool(config);
    // T1 MVS 12 / flow 1.40 on a 0.45×0.2 perimeter → G1 F6316. Old remap-only
    // gate uses T0's 0.98 → F9024; packed HF 0.95 → F9309.
    REQUIRE(count_g1_feed(gcode, 6316) >= 1);
    REQUIRE(count_g1_feed(gcode, 9024) == 0);
    REQUIRE(count_g1_feed(gcode, 9309) == 0);
}

TEST_CASE("ooze-on U1 2-tool High-Flow standbys use the active variant",
          "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    apply_u1_toolchange_markers(config);
    config.option<ConfigOptionBool>("ooze_prevention")->value          = true;
    config.option<ConfigOptionInt>("standby_temperature_delta")->value = kStandbyDelta;
    raise_role_speeds_for_mvs_cap(config);
    disable_layer_cooling(config);

    const std::string gcode = slice_u1_two_tool(config);
    REQUIRE(gcode.find(";cooldown") != std::string::npos);
    // pre_toolchange: _get_temp + standby. T0 HF other-layer 230-15=215; T1 210-15=195.
    // Raw get_at uses T0 Standard 190-15=175 and T1 packed HF 230-15=215.
    const int t0_hf_standby  = kTempHfF0 + kStandbyDelta;
    const int t0_std_standby = kTempStdF0 + kStandbyDelta;
    const int t1_standby     = kTempF1 + kStandbyDelta;
    const int t1_hf_standby  = kTempHfF0 + kStandbyDelta;
    REQUIRE(gcode.find("M104 S" + std::to_string(t0_hf_standby) + " T0") != std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(t1_standby) + " T1") != std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(t0_std_standby) + " T0") == std::string::npos);
    REQUIRE(gcode.find("M104 S" + std::to_string(t1_hf_standby) + " T1") == std::string::npos);
}

// F0 [std,hf] HF rams, F1 does not. get_at(0) is F0 Standard (off); get_at(1) is
// F0 HF (on). ~1097 must use get_value_at so only leaving T0 travels to the tower.
TEST_CASE("append_tcr2 ramming flag follows the departing tool variant",
          "[PrintGCode][GCode][PAVariant][FilamentVariants]")
{
    DynamicPrintConfig config = step_size_2_f0_config();
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values = {false, true, false};
    apply_u1_toolchange_markers(config);
    raise_role_speeds_for_mvs_cap(config);
    disable_layer_cooling(config);

    const std::string gcode = slice_u1_two_tool(config);
    const size_t      leave_t0 = count_travel_then_tc_next(gcode, 1);
    const size_t      leave_t1 = count_travel_then_tc_next(gcode, 0);
    INFO("travel-then-T1 " << leave_t0 << " travel-then-T0 " << leave_t1);
    REQUIRE(leave_t0 >= 10);
    REQUIRE(leave_t0 > leave_t1);
}

// _extrude reads flow ratio, max volumetric speed and the PA enable flag from
// m_filament_flow, resolved once per export. Every id must give what the old
// per-path expressions gave, including an id past the resolved range.
TEST_CASE("resolved per-filament flow values match the per-path lookups",
          "[PrintGCode][GCode][FilamentVariants]")
{
    auto check = [](const DynamicPrintConfig &config) {
        const ResolvedFilamentFlow cache = ResolvedFilamentFlow::resolve(config);
        const auto &ratio = *config.option<ConfigOptionFloats>("filament_flow_ratio");
        const auto &mvs   = *config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        const auto &pa    = *config.option<ConfigOptionBools>("enable_pressure_advance");
        const size_t n    = flow_variant_filament_count(config);
        REQUIRE(cache.flow_ratio.size() == n);
        REQUIRE(cache.variants_active == filament_flow_variants_active(config));
        for (unsigned int id = 0; id <= n; ++id) {
            INFO("filament id " << id);
            // The expressions _extrude used before the cache.
            const double old_ratio = filament_flow_variants_active(config) ?
                                         get_value_at(config, ratio, ConfigFlowDomain::Filament, id) :
                                         ratio.get_at(0);
            const double old_mvs = get_value_at(config, mvs, ConfigFlowDomain::Filament, id);
            const bool   old_pa  = get_value_at(config, pa, ConfigFlowDomain::Filament, id);
            CHECK(cache.flow_ratio_for(config, id) == old_ratio);
            CHECK(cache.max_volumetric_speed_for(config, id) == old_mvs);
            CHECK(cache.enable_pressure_advance_for(config, id) == old_pa);
            CHECK(ResolvedFilamentFlow::uncached_flow_ratio(config, id) == old_ratio);
        }
        // Default-constructed (before apply_print_config): every id falls back.
        const ResolvedFilamentFlow empty;
        for (unsigned int id = 0; id <= n; ++id) {
            CHECK(empty.flow_ratio_for(config, id) == cache.flow_ratio_for(config, id));
            CHECK(empty.max_volumetric_speed_for(config, id) == cache.max_volumetric_speed_for(config, id));
            CHECK(empty.enable_pressure_advance_for(config, id) == cache.enable_pressure_advance_for(config, id));
        }
    };

    SECTION("F0 Standard-only, F1 Standard/High-Flow set to High-Flow")
    {
        DynamicPrintConfig config = high_flow_pa_config(true, false, false);
        config.option<ConfigOptionFloats>("filament_flow_ratio")->values           = {kFlowStdF0, kFlowT1StdPacked, kFlowHfF0};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {kVolStdF0, kVolF1, kVolHfF0};
        REQUIRE(filament_flow_variants_active(config));
        check(config);
    }
    SECTION("F0 Standard/High-Flow set to High-Flow, F1 Standard-only")
    {
        const DynamicPrintConfig config = step_size_2_f0_config();
        REQUIRE(filament_flow_variants_active(config));
        check(config);
    }
    SECTION("no packed variants keeps get_at(0) for the flow ratio")
    {
        DynamicPrintConfig config = high_flow_pa_config(true, true, false);
        config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {1, 1};
        config.option<ConfigOptionStrings>("filament_flow_support", true)->values = {FLOW_MODE_STANDARD, FLOW_MODE_STANDARD};
        config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard), int(fvtStandard)};
        config.option<ConfigOptionFloats>("filament_flow_ratio")->values           = {kFlowStdF0, 1.40};
        config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values = {kVolStdF0, kVolF1};
        config.option<ConfigOptionBools>("enable_pressure_advance")->values        = {true, false};
        REQUIRE_FALSE(filament_flow_variants_active(config));
        check(config);
        REQUIRE(ResolvedFilamentFlow::resolve(config).flow_ratio_for(config, 1) == kFlowStdF0);
    }
    SECTION("a plain full print config")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_filaments(3);
        check(config);
    }
}

// A High-Flow slot of a filament retract override with no value of its own (nil, or a
// preset that never stored one) falls back to the Standard override when composing the
// packed config, and to the printer value only when Standard is nil too. Never NaN.
TEST_CASE("a nil High-Flow retract override slices with the Standard override, then the printer value",
          "[PrintGCode][GCode][FilamentVariants]")
{
    auto applied_tool_retraction = [](const ConfigOptionFloatsNullable &f0_preset) {
        DynamicPrintConfig config = step_size_2_f0_config();   // F0 [std, hf] set to High-Flow, F1 std
        config.option<ConfigOptionFloats>("retraction_length")->values = {0.4, 0.4};
        ConfigOptionFloatsNullable       packed;
        packed.values = {ConfigOptionFloatsNullable::nil_value()};
        const ConfigOptionFloatsNullable f1_preset{1.5};
        compose_filament_flow_variant_segment(packed, f0_preset, 0, 2);
        compose_filament_flow_variant_segment(packed, f1_preset, 2, 1);
        REQUIRE(packed.values.size() == 3);
        config.set_key_value("filament_retraction_length", packed.clone());

        Print print;
        Model model;
        add_two_tool_cubes(model);
        print.apply(model, config);
        REQUIRE(print.config().retraction_length.size() == 2);
        return std::make_pair(print.config().retraction_length.get_at(0), print.config().retraction_length.get_at(1));
    };
    const double nil = ConfigOptionFloatsNullable::nil_value();

    // Standard 0.7, High-Flow nil: T0 (High-Flow) retracts 0.7.
    ConfigOptionFloatsNullable std_only;
    std_only.values = {0.7, nil};
    auto r = applied_tool_retraction(std_only);
    CHECK_FALSE(std::isnan(r.first));
    CHECK(r.first == Approx(0.7));
    CHECK(r.second == Approx(1.5));

    // A preset that stored only one (Standard) value: same.
    ConfigOptionFloatsNullable one_value;
    one_value.values = {0.7};
    r = applied_tool_retraction(one_value);
    CHECK(r.first == Approx(0.7));

    // Both nil: the printer value.
    ConfigOptionFloatsNullable both_nil;
    both_nil.values = {nil, nil};
    r = applied_tool_retraction(both_nil);
    CHECK_FALSE(std::isnan(r.first));
    CHECK(r.first == Approx(0.4));
    CHECK(r.second == Approx(1.5));

    // Its own High-Flow value still wins.
    ConfigOptionFloatsNullable own_hf;
    own_hf.values = {0.7, 0.3};
    r = applied_tool_retraction(own_hf);
    CHECK(r.first == Approx(0.3));
}
// CoolingBuffer reads a G4 dwell (S = seconds, P = milliseconds) into the layer time, so a dwell inside a
// layer counts as time the layer already takes and reduces the min-layer-time slowdown. It used to compare
// find() against 0 instead of npos, so "G4 P5000" parsed as 0 s and the layer was slowed as if the dwell
// were not there. (Orca #16031 also routed this parse through fast_float, which left its result
// uninitialised for unparsable text; that stays covered by the repeat-slice determinism check.)
namespace {
// Slice a 20 mm cube printed with two filaments (walls with the first, infill with the second), so every
// layer carries a tool change in the middle of the layer, after its first extrusion. The tool-change G-code
// is `change_gcode`. The layer-time slowdown is active on every layer. (A dwell before the first extrusion
// of a layer is deliberately not counted by the cooling buffer, which is why the dwell sits in the
// tool change and not in the layer-change G-code.) Returns the G-code.
std::string slice_with_toolchange(const char *change_gcode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_key_value("change_filament_gcode", new ConfigOptionString(change_gcode));
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_filament", 1},
        {"sparse_infill_filament", 2},
        {"solid_infill_filament", 2},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"slow_down_for_layer_cooling", "1,1"},
        // Above the natural layer time of this cube, so the slowdown is active on every layer.
        {"slow_down_layer_time", "20,20"},
        {"slow_down_min_speed", "5,5"},
        {"fan_cooling_layer_time", "20,20"},
        {"machine_start_gcode", ""},
    });
    return Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
}

// The sliced G-code without the lines that legitimately differ between the variants: the generator
// banner, M73 (estimated time, which does include the dwell), the echoed config block (it names the tool-change G-code), and the dwell
// lines themselves (anything starting with one of `drop_prefixes`).
std::vector<std::string> cooling_comparable(const std::string &gcode, const std::vector<std::string> &drop_prefixes)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line)) {
        if (line.find("generated by") != std::string::npos || line.compare(0, 4, "M73 ") == 0 ||
            line.find("change_filament_gcode") != std::string::npos)
            continue;
        bool drop = false;
        for (const std::string &prefix : drop_prefixes)
            drop = drop || (!prefix.empty() && line.compare(0, prefix.size(), prefix) == 0);
        if (!drop)
            out.push_back(line);
    }
    return out;
}

// Sum of every F value on a G1 line: it drops when the slowdown gets stronger.
double feedrate_sum(const std::string &gcode)
{
    double sum = 0.;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line))
        if (line.compare(0, 3, "G1 ") == 0) {
            const size_t f = line.find(" F");
            if (f != std::string::npos)
                sum += std::atof(line.c_str() + f + 2);
        }
    return sum;
}
} // namespace

TEST_CASE("G4 P dwell in custom G-code gives deterministic cooling", "[PrintGCode][CoolingBuffer]")
{
    const std::string with_dwell_1 = slice_with_toolchange("G92 E0\nG4 P5000");
    const std::string with_dwell_2 = slice_with_toolchange("G92 E0\nG4 P5000");
    REQUIRE(with_dwell_1.find("G4 P5000") != std::string::npos);
    CHECK(cooling_comparable(with_dwell_1, {}) == cooling_comparable(with_dwell_2, {}));
}

TEST_CASE("G4 dwell counts towards the layer time", "[PrintGCode][CoolingBuffer]")
{
    const std::string none = slice_with_toolchange("G92 E0\n; dwell");
    const std::vector<std::string> drop = {"G4 ", "; dwell"};

    SECTION("a P dwell (milliseconds) reduces the slowdown") {
        const std::string p = slice_with_toolchange("G92 E0\nG4 P3000");
        REQUIRE(p.find("G4 P3000") != std::string::npos);
        // The layer already takes 3 s longer, so the cooling buffer slows the extrusion down less.
        CHECK(feedrate_sum(p) > feedrate_sum(none) * 1.001);
        CHECK(cooling_comparable(p, drop) != cooling_comparable(none, drop));
    }
    SECTION("S seconds and P milliseconds are the same dwell") {
        const std::string s = slice_with_toolchange("G92 E0\nG4 S2");
        const std::string p = slice_with_toolchange("G92 E0\nG4 P2000");
        REQUIRE(s.find("G4 S2") != std::string::npos);
        CHECK(feedrate_sum(s) > feedrate_sum(none) * 1.001);
        CHECK(cooling_comparable(s, drop) == cooling_comparable(p, drop));
    }
    SECTION("P1500 is 1.5 s") {
        const std::string p = slice_with_toolchange("G92 E0\nG4 P1500");
        const std::string s = slice_with_toolchange("G92 E0\nG4 S1.5");
        CHECK(feedrate_sum(p) > feedrate_sum(none) * 1.001);
        CHECK(cooling_comparable(p, drop) == cooling_comparable(s, drop));
    }
    SECTION("a trailing comment is not parsed as a parameter") {
        // The comment contains an 'S' and a 'P'; the value must still be the 500 ms before it.
        const std::string plain   = slice_with_toolchange("G92 E0\nG4 P500");
        const std::string comment = slice_with_toolchange("G92 E0\nG4 P500 ; Settle Pressure");
        REQUIRE(comment.find("G4 P500 ; Settle Pressure") != std::string::npos);
        CHECK(cooling_comparable(comment, drop) == cooling_comparable(plain, drop));
    }
    SECTION("a G4 with neither S nor P adds no time") {
        // Even when its comment contains an 'S' or a 'P'.
        const std::string bare = slice_with_toolchange("G92 E0\nG4 ; Stop Pause");
        REQUIRE(bare.find("G4 ; Stop Pause") != std::string::npos);
        CHECK(cooling_comparable(bare, drop) == cooling_comparable(none, drop));
    }
}

// Orca #15849 / Edge #113: OozePrevention::pre_toolchange turns a tool fully off
// (M104 S0 ;cooldown) only after that tool's last extrusion anywhere in the print.
namespace {

DynamicPrintConfig two_tool_ooze_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Print::apply()/region_config_from_model_volume() derives num_extruders from
    // filament_diameter.size(), which set_num_filaments() resizes. set_num_extruders()
    // resizes nozzle_diameter. Both are needed or a per-object wall_filament of 2 is
    // clamped back to extruder 1.
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        {"nozzle_diameter",                "0.4,0.4"},
        {"filament_diameter",              "1.75,1.75"},
        {"single_extruder_multi_material", "0"},
        {"ooze_prevention",                "1"},
        {"standby_temperature_delta",      "-5"},
        {"layer_height",                   "0.2"},
        {"initial_layer_print_height",     "0.2"},
        {"gcode_comments",                 "1"},
        {"machine_start_gcode",            ""},
        {"machine_end_gcode",              ""},
        {"before_layer_change_gcode",      ""},
        {"layer_change_gcode",             ""},
        {"top_shell_layers",               "1"},
        {"bottom_shell_layers",            "1"},
        {"sparse_infill_density",          "5%"},
        {"skirt_loops",                    "0"},
        {"enable_prime_tower",             "0"},
        {"wipe_tower_x",                   "0"},
        {"wipe_tower_y",                   "0"},
    });
    return config;
}

void set_object_extruder(ModelObject &object, int extruder_1based)
{
    object.config.set_key_value("wall_filament", new ConfigOptionInt(extruder_1based));
    object.config.set_key_value("sparse_infill_filament", new ConfigOptionInt(extruder_1based));
    object.config.set_key_value("solid_infill_filament", new ConfigOptionInt(extruder_1based));
}

ModelObject *add_scaled_cube(Model &model, Print &print, const std::string &name, Vec3f scale, Vec3d offset, int extruder_1based, bool on_bed)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    TriangleMesh mesh   = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
    mesh.scale(scale);
    object->add_volume(std::move(mesh));
    object->add_instance()->set_offset(offset);
    if (on_bed)
        object->ensure_on_bed();
    print.auto_assign_extruders(object);
    set_object_extruder(*object, extruder_1based);
    return object;
}

struct CooldownHit
{
    int    s   = 0;
    int    t   = 0;
    size_t pos = 0;
};

std::vector<CooldownHit> parse_cooldowns(const std::string &gcode)
{
    std::regex cooldown_re(R"(M104 S(\d+) T(\d+)[^\n]*;cooldown)");
    std::vector<CooldownHit> hits;
    for (auto it = std::sregex_iterator(gcode.begin(), gcode.end(), cooldown_re); it != std::sregex_iterator(); ++it) {
        hits.push_back({std::stoi((*it)[1].str()), std::stoi((*it)[2].str()), size_t(it->position())});
    }
    return hits;
}

size_t first_s0_pos(const std::vector<CooldownHit> &hits, int tool)
{
    for (const CooldownHit &hit : hits)
        if (hit.s == 0 && hit.t == tool)
            return hit.pos;
    return std::string::npos;
}

bool tool_got_s0(const std::vector<CooldownHit> &hits, int tool)
{
    return first_s0_pos(hits, tool) != std::string::npos;
}

} // namespace

TEST_CASE("OozePrevention: tool that finishes early gets S0 exactly once, after its last use", "[OozePrevention][ToolOrdering]")
{
    // Object 0 (extruder 1) is short. Object 1 (extruder 2) is full height.
    // T0 must go to S0 once the short object is done; T1 must never see S0.
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    add_scaled_cube(model, print, "short", Vec3f(1.f, 1.f, 0.2f), Vec3d(0., 0., 0.), 1, true);
    add_scaled_cube(model, print, "tall", Vec3f(1.f, 1.f, 1.f), Vec3d(40., 0., 0.), 2, true);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    const auto hits = parse_cooldowns(gcode);
    REQUIRE_FALSE(hits.empty());
    CHECK(tool_got_s0(hits, 0));
    CHECK_FALSE(tool_got_s0(hits, 1));

    const size_t first_t0_s0 = first_s0_pos(hits, 0);
    REQUIRE(first_t0_s0 != std::string::npos);
    CHECK(gcode.find("T0 ; change extruder", first_t0_s0) == std::string::npos);
}

TEST_CASE("OozePrevention: unused extruder turns off after its final layer on a multi-object print with different layer heights",
          "[OozePrevention][ToolOrdering]")
{
    // Orca #15849 (bcbb8746) "Unused extruder turns off after its final layer on multi-object print",
    // rewritten for Catch2 v2 and Edge's test helpers.
    // Object 1: tall cube (10 mm) printed with extruder 1 (T0) at 0.20 mm layers.
    // Object 2: short cube (4 mm) printed with extruder 2 (T1) at 0.15 mm layers.
    // The merged print-wide layer list interleaves both objects' Z values, so a layer-index
    // lookup and a print_z lookup disagree here; only the print_z one is right.
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    config.set_deserialize_strict({
        {"standby_temperature_delta", "-40"},
        {"nozzle_temperature",        "240,240"},
    });
    ModelObject *tall  = add_scaled_cube(model, print, "tall-t0", Vec3f(1.f, 1.f, 0.5f), Vec3d(0., 0., 0.), 1, true);
    ModelObject *short_ = add_scaled_cube(model, print, "short-t1", Vec3f(1.f, 1.f, 0.2f), Vec3d(40., 0., 0.), 2, true);
    tall->config.set_key_value("layer_height", new ConfigOptionFloat(0.20));
    short_->config.set_key_value("layer_height", new ConfigOptionFloat(0.15));

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    int t0_s0_cooldowns = 0;
    int t1_s0_cooldowns = 0;
    {
        std::istringstream stream(gcode);
        for (std::string line; std::getline(stream, line);) {
            if (line.find(";cooldown") == std::string::npos)
                continue;
            if (line.find("M104 S0 T1") != std::string::npos)
                ++t1_s0_cooldowns;
            if (line.find("M104 S0 T0") != std::string::npos)
                ++t0_s0_cooldowns;
        }
    }
    // T1 finishes at 4 mm and must get the S0 cooldown exactly once when it is parked.
    CHECK(t1_s0_cooldowns == 1);
    // T0 prints all the way to 10 mm, so pre_toolchange must never turn it off.
    CHECK(t0_s0_cooldowns == 0);

    // The S0 belongs to T1's top layer (or the first toolchange after it), not earlier, and T1
    // is never selected again afterwards.
    const size_t t1_s0_pos = first_s0_pos(parse_cooldowns(gcode), 1);
    REQUIRE(t1_s0_pos != std::string::npos);
    CHECK(gcode.find("T1 ; change extruder", t1_s0_pos) == std::string::npos);
    const size_t z_tag = gcode.rfind("\n;Z:", t1_s0_pos);
    REQUIRE(z_tag != std::string::npos);
    const double z_at_s0 = std::stod(gcode.substr(z_tag + 4, 16));
    CHECK(z_at_s0 > 3.7);
    CHECK(z_at_s0 < 4.3);
}

TEST_CASE("OozePrevention: two objects with different effective layer counts on the same extruder never get a premature S0",
          "[OozePrevention][ToolOrdering]")
{
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();

    add_scaled_cube(model, print, "tall", Vec3f(1.f, 1.f, 1.f), Vec3d(0., 0., 0.), 1, true);
    ModelObject *shortobj = add_scaled_cube(model, print, "short", Vec3f(1.f, 1.f, 0.2f), Vec3d(40., 0., 0.), 1, true);
    // Different layer height so object-local Layer::id() diverges from the print-wide index.
    shortobj->config.set_key_value("layer_height", new ConfigOptionFloat(0.1));

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("M104 S0") == std::string::npos);
}

TEST_CASE("OozePrevention: a support-only extruder used on later layers never gets a premature S0", "[OozePrevention][ToolOrdering]")
{
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    config.set_deserialize_strict({
        {"enable_support",             "1"},
        {"support_filament",           "2"},
        {"support_interface_filament", "2"},
        {"support_type",               "normal(auto)"},
        {"support_threshold_angle",    "40"},
    });

    ModelObject *object = model.add_object();
    object->name        = "overhang";
    object->add_volume(Test::mesh(TestMesh::overhang));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    set_object_extruder(*object, 1);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    const size_t first_t1_s0 = first_s0_pos(parse_cooldowns(gcode), 1);
    if (first_t1_s0 != std::string::npos) {
        CHECK(gcode.find("; support material", first_t1_s0) == std::string::npos);
        CHECK(gcode.find("T1 ; change extruder", first_t1_s0) == std::string::npos);
    }
}

TEST_CASE("OozePrevention: by-object (sequential) printing never emits S0", "[OozePrevention][ToolOrdering][ByObject]")
{
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    config.set_deserialize_strict({{"print_sequence", "by object"}});
    add_scaled_cube(model, print, "seq", Vec3f(1.f, 1.f, 1.f), Vec3d(0., 0., 0.), 1, true);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(Test::gcode(print).find("M104 S0") == std::string::npos);
}

TEST_CASE("OozePrevention: SEMM and Bambu (BBL) setups are unaffected", "[OozePrevention][ToolOrdering]")
{
    SECTION("single_extruder_multi_material: no change, ooze prevention path never engages S0") {
        Print              print;
        Model              model;
        DynamicPrintConfig config = two_tool_ooze_config();
        config.set_deserialize_strict({{"single_extruder_multi_material", "1"}});
        add_scaled_cube(model, print, "semm", Vec3f(1.f, 1.f, 1.f), Vec3d(0., 0., 0.), 1, true);

        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        REQUIRE(Test::gcode(print).find("M104 S0") == std::string::npos);
    }

    SECTION("Bambu (is_BBL_printer): no change from this feature, S0 never emitted by OozePrevention") {
        Print              print;
        Model              model;
        DynamicPrintConfig config = two_tool_ooze_config();
        add_scaled_cube(model, print, "tall", Vec3f(1.f, 1.f, 1.f), Vec3d(0., 0., 0.), 1, true);
        add_scaled_cube(model, print, "short", Vec3f(1.f, 1.f, 0.2f), Vec3d(40., 0., 0.), 2, true);

        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        print.is_BBL_printer() = true;
        REQUIRE(Test::gcode(print).find("M104 S0") == std::string::npos);
    }
}

TEST_CASE("OozePrevention: a tool reused after a gap gets standby first and S0 only after its final use", "[OozePrevention][ToolOrdering]")
{
    // One 20 mm cube printed by T1, with a layer range 6..14 mm switched to T0. T1 is idle
    // while T0 prints the middle and is used again on top. (The earlier version floated a
    // second T1 cube at z = 16, which Print rejects as empty layers, so it never ran.)
    // The toolchange away from T1 at ~6 mm must be standby, never S0, and T1 is the last
    // tool so it never gets S0 at all. T0's only S0 comes at ~14 mm, after its final layer.
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    ModelObject       *object = add_scaled_cube(model, print, "gap-t1", Vec3f(1.f, 1.f, 1.f), Vec3d(0., 0., 0.), 2, true);
    DynamicPrintConfig middle;
    // A layer range must carry layer_height: layer_height_profile_from_ranges() reads it unchecked.
    middle.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    middle.set_key_value("wall_filament", new ConfigOptionInt(1));
    middle.set_key_value("sparse_infill_filament", new ConfigOptionInt(1));
    middle.set_key_value("solid_infill_filament", new ConfigOptionInt(1));
    object->layer_config_ranges[{6., 14.}].assign_config(middle);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    const auto hits = parse_cooldowns(gcode);
    REQUIRE_FALSE(hits.empty());

    bool saw_t1_standby = false;
    for (const CooldownHit &hit : hits)
        if (hit.t == 1 && hit.s > 0)
            saw_t1_standby = true;
    CHECK(saw_t1_standby);
    CHECK_FALSE(tool_got_s0(hits, 1));

    int t0_s0 = 0;
    for (const CooldownHit &hit : hits)
        if (hit.t == 0 && hit.s == 0)
            ++t0_s0;
    CHECK(t0_s0 == 1);
    const size_t t0_s0_pos = first_s0_pos(hits, 0);
    REQUIRE(t0_s0_pos != std::string::npos);
    CHECK(gcode.find("T0 ; change extruder", t0_s0_pos) == std::string::npos);
    const size_t z_tag = gcode.rfind("\n;Z:", t0_s0_pos);
    REQUIRE(z_tag != std::string::npos);
    CHECK(std::stod(gcode.substr(z_tag + 4, 16)) > 13.5);
}

TEST_CASE("OozePrevention: mixed filament keeps physical tool 2 heated until its resolved last use", "[OozePrevention][ToolOrdering][MixedFilament]")
{
    // Short object uses physical filament 1. Tall object uses a mixed row whose
    // components are physical 1 and 2, so tool 2 appears later only through
    // MixedFilamentManager resolution. LayerTools::extruders must hold those
    // physical ids — never a virtual mixed id, and never filament_is_mixed.
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};

    MixedFilamentManager mixed_mgr;
    mixed_mgr.add_custom_filament(1, 2, 50, {"#FF0000", "#00FF00"});
    mixed_mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("12");
    const size_t       n_physical = 2;
    const unsigned int virtual_id = mixed_mgr.filament_id_from_mixed_index(0, n_physical);
    REQUIRE(mixed_mgr.total_filaments(n_physical) == n_physical + mixed_mgr.enabled_count());
    REQUIRE(virtual_id > n_physical);
    config.set("mixed_filament_definitions", mixed_mgr.serialize_custom_entries());

    add_scaled_cube(model, print, "short-t0", Vec3f(1.f, 1.f, 0.2f), Vec3d(0., 0., 0.), 1, true);
    add_scaled_cube(model, print, "tall-mixed", Vec3f(1.f, 1.f, 1.f), Vec3d(40., 0., 0.), int(virtual_id), true);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    REQUIRE(print.mixed_filament_manager().total_filaments(n_physical) == mixed_mgr.total_filaments(n_physical));

    bool saw_physical_t1          = false;
    bool saw_unresolved_virtual   = false;
    for (const LayerTools &lt : print.tool_ordering()) {
        for (unsigned int ext : lt.extruders) {
            if (ext == 1)
                saw_physical_t1 = true;
            if (ext >= n_physical)
                saw_unresolved_virtual = true;
        }
    }
    CHECK(saw_physical_t1);
    CHECK_FALSE(saw_unresolved_virtual);

    const auto   hits      = parse_cooldowns(gcode);
    const size_t t1_s0_pos = first_s0_pos(hits, 1);
    if (t1_s0_pos != std::string::npos)
        CHECK(gcode.find("T1 ; change extruder", t1_s0_pos) == std::string::npos);
}

TEST_CASE("OozePrevention: wipe tower on keeps the same S0 placement", "[OozePrevention][ToolOrdering][WipeTower]")
{
    Print              print;
    Model              model;
    DynamicPrintConfig config = two_tool_ooze_config();
    config.set_deserialize_strict({
        {"enable_prime_tower", "1"},
        {"wipe_tower_x",       "70"},
        {"wipe_tower_y",       "70"},
        {"prime_tower_width",  "35"},
        {"brim_type",          "no_brim"},
    });
    add_scaled_cube(model, print, "short", Vec3f(1.f, 1.f, 0.2f), Vec3d(0., 0., 0.), 1, true);
    add_scaled_cube(model, print, "tall", Vec3f(1.f, 1.f, 1.f), Vec3d(40., 0., 0.), 2, true);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.has_wipe_tower());
    const std::string gcode = Test::gcode(print);

    const auto hits = parse_cooldowns(gcode);
    REQUIRE_FALSE(hits.empty());
    CHECK(tool_got_s0(hits, 0));
    CHECK_FALSE(tool_got_s0(hits, 1));
    const size_t first_t0_s0 = first_s0_pos(hits, 0);
    REQUIRE(first_t0_s0 != std::string::npos);
    CHECK(gcode.find("T0 ; change extruder", first_t0_s0) == std::string::npos);
}
