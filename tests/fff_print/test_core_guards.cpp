// Slicing-level regression tests for the crash / UB / hang guards ported from OrcaSlicer in batch 1A.
#include <catch2/catch.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Orca #14665: max_layer_height can be shorter than the extruder count (normalization sizes it to
// the filament count under single_extruder_multi_material). calc_max_layer_height() in ToolOrdering
// indexed it per nozzle and read past the end. Shortened directly here to isolate that read.
TEST_CASE("Multi-extruder slice stays in bounds with a short max_layer_height", "[CoreGuards][ToolOrdering]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#00FF00" };
    config.set_deserialize_strict({ { "max_layer_height", "0.3" } }); // deliberately one entry short
    Print print;
    init_and_process_print({ TestMesh::cube_20x20x20 }, print, config);
    REQUIRE_FALSE(print.objects().front()->layers().empty());
}
