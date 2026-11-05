#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using namespace Slic3r;

namespace {

// Counts occurrences of a substring in a file's whole content.
size_t count_in_file(const std::string &path, const std::string &needle)
{
    std::ifstream in(path, std::ios::binary);
    std::string   content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t        count = 0;
    for (size_t pos = 0; (pos = content.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

// Post-process rewrites per-extruder filament statistics, which requires a minimally
// populated result config (the export path always applies one before finalizing).
void configure_processor(GCodeProcessor &processor, Print &print)
{
    PrintConfig config;
    config.filament_diameter.values = {1.75};
    config.filament_density.values  = {1.24};
    config.filament_cost.values     = {20.0};
    processor.reset();
    processor.set_print(&print);
    processor.apply_config(config);
}

} // namespace

// The layer total written by machine gcode templates (e.g. Klipper's
// "SET_PRINT_STATS_INFO TOTAL_LAYER=[total_layer_count]") comes from the pre-export
// layer estimate, which counts empty layers the exporter skips. On a model whose tip
// lands exactly on a layer boundary the estimate overshoots by one, and the printer
// progress (which divides by TOTAL_LAYER) never reaches 100%. The post-process pass
// must rewrite the value with the real layer count counted from the layer-change tags.
TEST_CASE("GCodeProcessor post-process rewrites TOTAL_LAYER with the real layer count", "[LayerCount][TotalLayer]")
{
    // 3 real layer changes; the machine template rendered the estimate TOTAL_LAYER=4
    // (one skipped empty top layer). s_IsBBLPrinter defaults to true, so the BBL tag
    // flavour ("; CHANGE_LAYER" + "; Z_HEIGHT:") is used, matching the export path.
    const std::string gcode =
        "G90\n"
        "M82\n"
        "SET_PRINT_STATS_INFO TOTAL_LAYER=4 CURRENT_LAYER=0\n"
        "; CHANGE_LAYER\n"
        "; Z_HEIGHT: 0.2\n"
        "G1 Z0.200 F600\n"
        "G1 X10.0 Y10.0 E1.0 F1800\n"
        "SET_PRINT_STATS_INFO TOTAL_LAYER=4 CURRENT_LAYER=1\n"
        "; CHANGE_LAYER\n"
        "; Z_HEIGHT: 0.4\n"
        "G1 Z0.400 F600\n"
        "G1 X20.0 Y10.0 E2.0 F1800\n"
        "SET_PRINT_STATS_INFO TOTAL_LAYER=4 CURRENT_LAYER=2\n"
        "; CHANGE_LAYER\n"
        "; Z_HEIGHT: 0.6\n"
        "G1 Z0.600 F600\n"
        "G1 X30.0 Y10.0 E3.0 F1800\n"
        "SET_PRINT_STATS_INFO TOTAL_LAYER=4 CURRENT_LAYER=3\n"
        "M104 S0\n"
        ";LAYER_COUNT:4\n";

    const std::string path = "total_layer_rewrite_test.gcode";
    {
        std::ofstream out(path, std::ios::binary);
        out << gcode;
    }

    Print          print;
    print.set_status_silent();
    GCodeProcessor processor;
    configure_processor(processor, print);
    processor.initialize(path);
    processor.process_buffer(gcode);
    processor.finalize(true);

    REQUIRE(processor.result().layer_endpoints.size() == 3);

    // Every machine statistics line is rewritten with the real count...
    CHECK(count_in_file(path, "TOTAL_LAYER=4") == 0);
    CHECK(count_in_file(path, "TOTAL_LAYER=3") == 4);
    // ...while the surrounding parameters and human-readable comments stay untouched.
    CHECK(count_in_file(path, "CURRENT_LAYER=0") == 1);
    CHECK(count_in_file(path, "CURRENT_LAYER=3") == 1);
    CHECK(count_in_file(path, ";LAYER_COUNT:4") == 1);

    std::remove(path.c_str());
    std::remove((path + ".postprocess").c_str());
}

// gcode without machine statistics lines must pass through post-process unchanged in
// this respect (no accidental rewrite of unrelated lines).
TEST_CASE("GCodeProcessor post-process leaves gcode without TOTAL_LAYER untouched", "[LayerCount][TotalLayer]")
{
    const std::string gcode =
        "G90\n"
        "; CHANGE_LAYER\n"
        "; Z_HEIGHT: 0.2\n"
        "G1 X10.0 Y10.0 E1.0 F1800\n"
        "M104 S0\n";

    const std::string path = "total_layer_absent_test.gcode";
    {
        std::ofstream out(path, std::ios::binary);
        out << gcode;
    }

    Print          print;
    print.set_status_silent();
    GCodeProcessor processor;
    configure_processor(processor, print);
    processor.initialize(path);
    processor.process_buffer(gcode);
    processor.finalize(true);

    CHECK(count_in_file(path, "G1 X10.0 Y10.0 E1.0 F1800") == 1);
    CHECK(count_in_file(path, "M104 S0") == 1);

    std::remove(path.c_str());
    std::remove((path + ".postprocess").c_str());
}
