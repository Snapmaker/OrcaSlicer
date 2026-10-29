#include "catch_main.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

using namespace Slic3r;

// s_IsBBLPrinter is a mutable static selecting the tag dialect; restore it even if
// process_buffer/finalize throws (its default is true, so bbl=false cases depend on it).
struct ScopedBBLPrinter
{
    explicit ScopedBBLPrinter(bool bbl) : previous(GCodeProcessor::s_IsBBLPrinter) { GCodeProcessor::s_IsBBLPrinter = bbl; }
    ~ScopedBBLPrinter() { GCodeProcessor::s_IsBBLPrinter = previous; }
    bool previous;
};

// GCodeProcessorResult holds a std::mutex, so it cannot be copied/moved out of the
// processor. Process a gcode buffer and hand the (non-owning) result to a callback.
template <typename Fn>
static void with_processor(const std::string &gcode, bool bbl, Fn &&fn)
{
    GCodeProcessor p;
    p.apply_config(static_cast<const PrintConfig &>(FullPrintConfig::defaults()));
    ScopedBBLPrinter bbl_guard(bbl);
    p.initialize("test.gcode");
    p.process_buffer(gcode);
    p.finalize(false);
    fn(p.get_result());
}

SCENARIO("GCodeProcessor classifies roles from TYPE tags", "[GCodeProcessor]") {
    GIVEN("a gcode with ;TYPE: role tags") {
        with_processor(
            ";TYPE:Inner wall\n"
            "G1 X10 Y0 E1 F600\n"
            ";TYPE:Outer wall\n"
            "G1 X20 Y0 E1 F600\n"
            ";TYPE:Sparse infill\n"
            "G1 X30 Y0 E1 F600\n",
            /*bbl=*/false,
            [](const GCodeProcessorResult &result) {
                std::vector<ExtrusionRole> roles;
                for (const auto &m : result.moves)
                    if (m.type != EMoveType::Noop)
                        roles.push_back(m.extrusion_role);
                REQUIRE(roles.size() == 3);
                REQUIRE(roles[0] == erPerimeter);
                REQUIRE(roles[1] == erExternalPerimeter);
                REQUIRE(roles[2] == erInternalInfill);
            });
    }
}

SCENARIO("GCodeProcessor captures M106 and M104", "[GCodeProcessor]") {
    GIVEN("a gcode with fan and temperature commands") {
        with_processor(
            "M104 S200\n"
            "M106 S255\n"
            "G1 X10 Y0 E1 F600\n",
            /*bbl=*/false,
            [](const GCodeProcessorResult &result) {
                REQUIRE(!result.moves.empty());
                const auto &m = result.moves.back();
                // (100.0f/255.0f)*255.0f is a double-rounding hazard; use a tolerance.
                REQUIRE_THAT(m.fan_speed, Catch::Matchers::WithinRel(100.0f, 1e-6f));
                REQUIRE(m.temperature == 200.0f);
            });
    }
}

SCENARIO("GCodeProcessor time estimation produces a positive total", "[GCodeProcessor]") {
    GIVEN("a sequence of extrusion moves") {
        with_processor(
            ";TYPE:Inner wall\n"
            "G1 X10 Y0 E1 F600\n"
            "G1 X20 Y0 E1 F600\n"
            "G1 X30 Y0 E1 F600\n",
            /*bbl=*/false,
            [](const GCodeProcessorResult &result) {
                const auto &mode = result.print_statistics.modes[0]; // Normal
                REQUIRE(mode.time > 0.0f);
                REQUIRE(!mode.moves_times.empty());
            });
    }
}

SCENARIO("GCodeProcessor estimates a positive time per layer", "[GCodeProcessor]") {
    GIVEN("multiple physically-separated layers (Z moves + LAYER_CHANGE)") {
        // Each layer has a Z move so the speed chain is broken; per-layer time is
        // only asserted to be positive (not monotonic) — real layers vary in duration.
        with_processor(
            ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 Z0.2 F600\nG1 X10 Y0 E1 F600\n"
            ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 Z0.4 F600\nG1 X20 Y0 E1 F600\n"
            ";LAYER_CHANGE\n;TYPE:Inner wall\nG1 Z0.6 F600\nG1 X30 Y0 E1 F600\n",
            /*bbl=*/false,
            [](const GCodeProcessorResult &result) {
                const auto &layers = result.print_statistics.modes[0].layers_times;
                REQUIRE(layers.size() >= 3);
                for (float t : layers)
                    REQUIRE(t > 0.0f);
            });
    }
}

SCENARIO("GCodeProcessor supports both ;TYPE: and ; FEATURE: dialects", "[GCodeProcessor]") {
    GIVEN("the PrusaSlicer dialect (s_IsBBLPrinter=false)") {
        with_processor(";TYPE:Inner wall\nG1 X10 Y0 E1 F600\n", false,
            [](const GCodeProcessorResult &result) {
                REQUIRE(result.moves.back().extrusion_role == erPerimeter);
            });
    }
    GIVEN("the Bambu dialect (s_IsBBLPrinter=true)") {
        with_processor("; FEATURE: Inner wall\nG1 X10 Y0 E1 F600\n", true,
            [](const GCodeProcessorResult &result) {
                REQUIRE(result.moves.back().extrusion_role == erPerimeter);
            });
    }
}
