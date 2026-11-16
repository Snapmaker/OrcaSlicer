#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/PressureEqualizer.hpp"

using namespace Slic3r;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Configure the GCodeConfig fields the PressureEqualizer reads.
static void init_config(GCodeConfig &config, float slope, float segment_length = 2.f)
{
    config.max_volumetric_extrusion_rate_slope.values                 = { slope };
    config.max_volumetric_extrusion_rate_slope_segment_length.values  = { segment_length };
    config.filament_diameter.values                                   = { 1.75 };
    config.use_relative_e_distances.value                             = true;
    config.extrusion_rate_smoothing_external_perimeter_only.values    = { false };
}

// Feed a single layer and flush it with a NOP result; return the processed gcode.
static std::string process_single_layer(const GCodeConfig &config, const std::string &gcode)
{
    PressureEqualizer pe(config);
    pe.process_layer(LayerResult{ gcode, 0, false, false, false });
    LayerResult out = pe.process_layer(LayerResult::make_nop_layer_result());
    return out.gcode;
}

// Cross-section of the 1.75mm filament configured above [mm^2].
static const double kPi           = 3.14159265358979323846;
static const double kFilamentArea = 0.25 * kPi * 1.75 * 1.75;

// One feedrate quantum (speeds are quantized to 60 mm/min) at the extrusion
// densities used below (E/dist = 0.05) maps to ~7.2 mm^3/min of rate change.
static const double kRateSlackMm3Min      = 8.0;
// Quantized feedrates can push the instantaneous slope between two adjacent
// output pieces slightly above the nominal cap; allow two quanta of slack.
static const double kSlopeSlackMm3PerS    = 0.25;

// One extrusion piece as emitted in the output gcode (relative or absolute E value as written).
struct OutPiece {
    double x_start;
    double x_end;
    double e;
    double feedrate; // effective F [mm/min] (from the line itself or the last "G1 F..." line)
};

// Parse "G1" lines; strips comments. Tracks the effective feedrate and X position
// so that each line carrying both X and E becomes one OutPiece.
static std::vector<OutPiece> parse_extrusion_pieces(const std::string &out)
{
    std::vector<OutPiece> pieces;
    double                 feed = 0.0, x_prev = 0.0;
    size_t                 start = 0;
    while (start <= out.size()) {
        size_t nl = out.find('\n', start);
        std::string line = out.substr(start, (nl == std::string::npos ? out.size() : nl) - start);
        if (line.compare(0, 3, "G1 ") == 0) {
            double x = 0, e = 0, f = 0;
            bool   has_x = false, has_e = false, has_f = false;
            size_t pos = 2;
            while (pos < line.size() && line[pos] != ';') {
                if (line[pos] == ' ') { ++pos; continue; }
                const char axis = line[pos];
                size_t val_end = line.find_first_of(" ;", pos + 1);
                if (val_end == std::string::npos) val_end = line.size();
                const double v = std::stod(line.substr(pos + 1, val_end - pos - 1));
                switch (axis) {
                case 'X': has_x = true; x = v; break;
                case 'E': has_e = true; e = v; break;
                case 'F': has_f = true; f = v; break;
                default: break;
                }
                pos = val_end;
            }
            if (has_f) feed = f;
            if (has_x && has_e) pieces.push_back({ x_prev, x, e, feed });
            if (has_x) x_prev = x;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return pieces;
}

static double piece_dist(const OutPiece &p) { return std::abs(p.x_end - p.x_start); }
// Volumetric rate of a piece = A_filament * F * (dE / dX) [mm^3/min].
static double piece_rate(const OutPiece &p) { return kFilamentArea * p.feedrate * p.e / piece_dist(p); }
static double piece_time_s(const OutPiece &p) { return piece_dist(p) / (p.feedrate / 60.0); }

static double sum_e(const std::vector<OutPiece> &pieces)
{
    double s = 0.0;
    for (const OutPiece &p : pieces) s += p.e;
    return s;
}

// Pieces must tile the original path with no gaps or overlaps.
static void require_contiguous(const std::vector<OutPiece> &pieces)
{
    for (size_t i = 1; i < pieces.size(); ++i) {
        INFO("piece #" << i << " starts at " << pieces[i].x_start << ", previous ended at " << pieces[i - 1].x_end);
        REQUIRE_THAT(pieces[i].x_start, WithinAbs(pieces[i - 1].x_end, 1e-6));
    }
}

// Core contract: output rates stay within the input rate band, and the rate
// change between adjacent pieces respects the configured slope cap.
static void require_rate_profile(const std::vector<OutPiece> &pieces, double slope_mm3_per_s2,
                                 double rate_min, double rate_max)
{
    for (size_t i = 0; i < pieces.size(); ++i) {
        const OutPiece &p = pieces[i];
        INFO("piece #" << i << ": X" << p.x_start << "->" << p.x_end << " E" << p.e << " F" << p.feedrate);
        REQUIRE(piece_dist(p) > 0.0);
        const double rate = piece_rate(p);
        REQUIRE(rate <= rate_max + kRateSlackMm3Min);
        REQUIRE(rate >= rate_min - kRateSlackMm3Min);
        if (i > 0) {
            const double prev_rate = piece_rate(pieces[i - 1]);
            const double dr        = std::abs(rate - prev_rate) / 60.0; // mm^3/s
            const double dt        = 0.5 * (piece_time_s(p) + piece_time_s(pieces[i - 1]));
            INFO("adjacent rates " << prev_rate << " -> " << rate << " mm^3/min over " << dt << "s");
            REQUIRE(dr <= slope_mm3_per_s2 * dt + kSlopeSlackMm3PerS);
        }
    }
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

SCENARIO("PressureEqualizer limits the volumetric rate slope on deceleration", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    // Fast segment (144 mm^3/min) followed by slow ones (36 mm^3/min):
    // rate = A_filament * F * E/dist, A = 2.405 mm^2 for 1.75mm filament.
    const double rate_fast = kFilamentArea * 1200 * 0.5 / 10;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        "G1 X40 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";

    WHEN("a fast segment is followed by slow segments") {
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the fast segment is split into a deceleration ramp of ~2mm pieces") {
            // 10mm line with a large start/end rate delta: ceil(10 / 2) = 5 pieces.
            const size_t ramp_pieces = std::count_if(pieces.begin(), pieces.end(),
                                                     [](const OutPiece &p) { return p.x_end <= 10.0 + 1e-9; });
            REQUIRE(ramp_pieces == 5);
            for (const OutPiece &p : pieces)
                if (p.x_end <= 10.0 + 1e-9) REQUIRE(piece_dist(p) <= 2.0 + 1e-3);
        }
        THEN("total extrusion is conserved") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(2.0, 1e-3));
        }
        THEN("geometry is conserved: contiguous pieces reaching the final X") {
            require_contiguous(pieces);
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(40.0, 1e-6));
        }
        THEN("output rates stay within the input band and respect the slope cap") {
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
}

SCENARIO("PressureEqualizer smooths acceleration and deceleration within one segment", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    // slow -> fast -> fast -> slow -> slow: one continuous extrusion in which the
    // middle fast pair must ramp up from and back down to the slow rate.
    const double rate_fast = kFilamentArea * 1200 * 0.5 / 10;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F300 ;_EXTRUDE_SET_SPEED\n"
        "G1 X20 Y0 E0.5 F1200\n"
        "G1 X30 Y0 E0.5 F1200\n"
        "G1 X40 Y0 E0.5 F300\n"
        "G1 X50 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";

    WHEN("a slow-fast-slow profile is processed") {
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("both fast segments are split into ramps") {
            const size_t ramp1 = std::count_if(pieces.begin(), pieces.end(),
                                               [](const OutPiece &p) { return p.x_end > 10.0 + 1e-9 && p.x_end <= 20.0 + 1e-9; });
            const size_t ramp2 = std::count_if(pieces.begin(), pieces.end(),
                                               [](const OutPiece &p) { return p.x_end > 20.0 + 1e-9 && p.x_end <= 30.0 + 1e-9; });
            REQUIRE(ramp1 == 5);
            REQUIRE(ramp2 == 5);
        }
        THEN("total extrusion is conserved and geometry is contiguous") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(2.5, 1e-3));
            require_contiguous(pieces);
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(50.0, 1e-6));
        }
        THEN("rates respect the slope cap in both directions") {
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
}

SCENARIO("PressureEqualizer splits long segments", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f, /*segment_length=*/2.f);

    const double rate_fast = kFilamentArea * 1200 * 1.5 / 30;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    // A 30mm fast segment (E1.5) followed by three 10mm slow segments.
    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X30 Y0 E1.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G1 X40 Y0 E0.5 F300\n"
        "G1 X50 Y0 E0.5 F300\n"
        "G1 X60 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";

    WHEN("a layer is processed") {
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the 30mm segment is split into ~2mm pieces") {
            size_t long_line_pieces = 0;
            for (const OutPiece &p : pieces)
                if (p.x_end <= 30.0 + 1e-9) {
                    ++long_line_pieces;
                    // Ramp pieces are <= m_max_segment_length by ceil(l/2), but the
                    // optional steady head (l_steady = l - t_acc * feed_avg) is only
                    // bounded from below (>= 1mm) and lands slightly above 2mm here.
                    REQUIRE(piece_dist(p) <= 2.1);
                }
            REQUIRE(long_line_pieces >= 15); // 30mm / 2mm
        }
        THEN("extrusion and geometry are conserved") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(3.0, 1e-3));
            require_contiguous(pieces);
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(60.0, 1e-6));
        }
        THEN("rates respect the input band and the slope cap") {
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
}

SCENARIO("PressureEqualizer leaves a single segment unchanged", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 2.f);

    const std::string gcode = "G1 X10 Y0 E0.5 F600\n";

    WHEN("a single extrusion is processed") {
        std::string out = process_single_layer(config, gcode);
        THEN("the output is byte-identical to the input") {
            REQUIRE(out == gcode);
        }
    }
}

SCENARIO("PressureEqualizer handles empty input", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 2.f);

    WHEN("an empty layer is processed") {
        std::string out = process_single_layer(config, "");
        THEN("the output is empty") {
            REQUIRE(out.empty());
        }
    }
}

SCENARIO("PressureEqualizer does not modify flow without EXTRUDE_SET_SPEED", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    // Different rates but no ;_EXTRUDE_SET_SPEED marker -> adjustable_flow is false.
    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F1200\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        "G1 X40 Y0 E0.5 F300\n";
    const std::string expected =
        "G1 X10 Y0 E0.5 F1200\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        "G1 X40 Y0 E0.5 F300\n";

    WHEN("a layer is processed") {
        std::string out = process_single_layer(config, gcode);
        THEN("the output is byte-identical (no flow adjustment)") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("PressureEqualizer stops adjusting after EXTRUDE_END closes the block", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    // The block opened by ;_EXTRUDE_SET_SPEED is closed by ;_EXTRUDE_END;
    // lines after it are never flow-adjustable even with a large rate change.
    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F600 ;_EXTRUDE_SET_SPEED\n"
        ";_EXTRUDE_END\n"
        "G1 X20 Y0 E0.5 F1200\n"
        "G1 X30 Y0 E0.5 F1200\n";
    const std::string expected =
        "G1 X10 Y0 E0.5 F600 ;_EXTRUDE_SET_SPEED\n"
        ";_EXTRUDE_END\n"
        "G1 X20 Y0 E0.5 F1200\n"
        "G1 X30 Y0 E0.5 F1200\n";

    WHEN("extrusions follow a closed speed block") {
        std::string out = process_single_layer(config, gcode);
        THEN("the output is byte-identical (block outside lines are untouched)") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("PressureEqualizer exempts ironing and bridge infill", "[PressureEqualizer]") {
    const std::string body =
        "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        "G1 X40 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";
    const std::string expected = body;

    WHEN("the extrusion role is ironing") {
        GCodeConfig config;
        init_config(config, 0.5f);
        std::string out = process_single_layer(config, ";_EXTRUSION_ROLE:" + std::to_string(int(ExtrusionRole::erIroning)) + "\n" + body);
        THEN("no flow adjustment is applied") {
            REQUIRE(out == expected);
        }
    }
    WHEN("the extrusion role is bridge infill") {
        GCodeConfig config;
        init_config(config, 0.5f);
        std::string out = process_single_layer(config, ";_EXTRUSION_ROLE:" + std::to_string(int(ExtrusionRole::erBridgeInfill)) + "\n" + body);
        THEN("no flow adjustment is applied") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("PressureEqualizer limits smoothing to external perimeters when configured", "[PressureEqualizer]") {
    const std::string body =
        "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        "G1 X40 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";
    const double rate_fast = kFilamentArea * 1200 * 0.5 / 10;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    WHEN("the role is a plain perimeter while only external perimeters are adjusted") {
        GCodeConfig config;
        init_config(config, 0.5f);
        config.extrusion_rate_smoothing_external_perimeter_only.values = { true };
        std::string out = process_single_layer(config, ";_EXTRUSION_ROLE:" + std::to_string(int(ExtrusionRole::erPerimeter)) + "\n" + body);
        THEN("no flow adjustment is applied") {
            REQUIRE(out == body);
        }
    }
    WHEN("the role is an external perimeter while only external perimeters are adjusted") {
        GCodeConfig config;
        init_config(config, 0.5f);
        config.extrusion_rate_smoothing_external_perimeter_only.values = { true };
        std::string out = process_single_layer(config, ";_EXTRUSION_ROLE:" + std::to_string(int(ExtrusionRole::erExternalPerimeter)) + "\n" + body);
        auto pieces = parse_extrusion_pieces(out);
        THEN("the output is adjusted and tagged as external perimeter") {
            REQUIRE(out != body);
            REQUIRE_THAT(out, ContainsSubstring(";_EXTERNAL_PERIMETER"));
        }
        THEN("rates respect the slope cap") {
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
    WHEN("the role is an overhang perimeter while only external perimeters are adjusted") {
        GCodeConfig config;
        init_config(config, 0.5f);
        config.extrusion_rate_smoothing_external_perimeter_only.values = { true };
        std::string out = process_single_layer(config, ";_EXTRUSION_ROLE:" + std::to_string(int(ExtrusionRole::erOverhangPerimeter)) + "\n" + body);
        auto pieces = parse_extrusion_pieces(out);
        THEN("the output is adjusted but not tagged as external perimeter") {
            REQUIRE(out != body);
            REQUIRE(out.find(";_EXTERNAL_PERIMETER") == std::string::npos);
        }
        THEN("rates respect the slope cap") {
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
}

SCENARIO("PressureEqualizer passes firmware retraction (G10/G11) through unchanged", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    const double rate_fast = kFilamentArea * 1200 * 0.5 / 10;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    // G10/G11 are classified as RETRACT/UNRETRACT (no XY movement, not EXTRUDE):
    // they are emitted verbatim and contribute no XY distance, so the extrusion
    // segment around them stays continuous and the fast move is still ramped.
    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G10\n"
        "G11\n"
        "G1 X20 Y0 E0.5 F300\n"
        "G1 X30 Y0 E0.5 F300\n"
        ";_EXTRUDE_END\n";

    WHEN("a layer with firmware retract/unretract is processed") {
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the retract/unretract lines are preserved verbatim") {
            REQUIRE_THAT(out, ContainsSubstring("G10"));
            REQUIRE_THAT(out, ContainsSubstring("G11"));
        }
        THEN("the fast segment is still split into a ramp (retract does not break the segment)") {
            const size_t ramp_pieces = std::count_if(pieces.begin(), pieces.end(),
                                                     [](const OutPiece &p) { return p.x_end <= 10.0 + 1e-9; });
            REQUIRE(ramp_pieces == 5);
        }
        THEN("extrusion is conserved and the slope cap holds") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(1.5, 1e-3));
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(30.0, 1e-6));
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
}

SCENARIO("PressureEqualizer splits absolute-E extrusions keeping absolute E output", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);
    config.use_relative_e_distances.value = false; // absolute E

    // Absolute E: 0.5, 1.0, ... cumulative. The fast first segment is split and
    // its pieces must carry interpolated absolute E values (0.1 ... 0.5).
    const std::string gcode =
        ";_EXTRUSION_ROLE:1\n"
        "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
        "G1 X20 Y0 E1 F300\n"
        "G1 X30 Y0 E1.5 F300\n"
        "G1 X40 Y0 E2 F300\n"
        ";_EXTRUDE_END\n";

    WHEN("a layer with absolute E is processed") {
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the fast segment is split with increasing absolute E values") {
            const size_t ramp_pieces = std::count_if(pieces.begin(), pieces.end(),
                                                     [](const OutPiece &p) { return p.x_end <= 10.0 + 1e-9; });
            REQUIRE(ramp_pieces == 5);
            double e_prev = 0.0;
            for (const OutPiece &p : pieces) {
                REQUIRE(p.e > e_prev);
                e_prev = p.e;
            }
        }
        THEN("the final absolute E position is conserved") {
            REQUIRE_THAT(pieces.back().e, WithinAbs(2.0, 1e-3));
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(40.0, 1e-6));
        }
    }
}

SCENARIO("PressureEqualizer buffers one layer and returns the previous layer", "[PressureEqualizer]") {
    GCodeConfig config;
    init_config(config, 0.5f);

    const std::string layer1 = "G1 X10 Y0 E0.5 F600\n";
    const std::string layer2 = "G1 X20 Y0 E0.5 F600\n";

    PressureEqualizer pe(config);

    WHEN("the first layer is submitted") {
        LayerResult res = pe.process_layer(LayerResult{ layer1, 0, false, false, false });
        THEN("a NOP result is returned while the layer is buffered") {
            REQUIRE(res.nop_layer_result);
        }
    }
    WHEN("the second layer is submitted") {
        pe.process_layer(LayerResult{ layer1, 0, false, false, false });
        LayerResult res = pe.process_layer(LayerResult{ layer2, 1, false, false, false });
        THEN("the processed first layer is returned unchanged") {
            REQUIRE(!res.nop_layer_result);
            REQUIRE(res.gcode == layer1);
        }
    }
    WHEN("the stream is flushed with a NOP result") {
        pe.process_layer(LayerResult{ layer1, 0, false, false, false });
        pe.process_layer(LayerResult{ layer2, 1, false, false, false });
        LayerResult res = pe.process_layer(LayerResult::make_nop_layer_result());
        THEN("the buffered second layer is returned") {
            REQUIRE(res.gcode == layer2);
        }
    }
}

SCENARIO("PressureEqualizer bridges small travel gaps but not large ones", "[PressureEqualizer]") {
    const double rate_fast = kFilamentArea * 1200 * 0.5 / 10;
    const double rate_slow = kFilamentArea * 300 * 0.5 / 10;

    WHEN("a 1mm travel separates two different-rate extrusions") {
        GCodeConfig config;
        init_config(config, 0.5f);
        // The gap is below max_ignored_gap_between_extruding_segments (3mm), so both
        // extrusions form one continuous segment and the fast one is decelerated early.
        const std::string gcode =
            ";_EXTRUSION_ROLE:1\n"
            "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
            "G1 X11 Y0 F1200\n"
            "G1 X21 Y0 E0.5 F300\n"
            "G1 X31 Y0 E0.5 F300\n"
            ";_EXTRUDE_END\n";
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the fast segment is still split into a ramp") {
            const size_t ramp_pieces = std::count_if(pieces.begin(), pieces.end(),
                                                     [](const OutPiece &p) { return p.x_end <= 10.0 + 1e-9; });
            REQUIRE(ramp_pieces == 5);
        }
        THEN("extrusion/geometry are conserved and the slope cap holds") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(1.5, 1e-3));
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(31.0, 1e-6));
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
    WHEN("an exactly 3mm travel separates two different-rate extrusions") {
        GCodeConfig config;
        init_config(config, 0.5f);
        // 3mm is the boundary: advance_segment_beyond_small_gap splits only when the
        // accumulated gap is STRICTLY greater than max_ignored_gap_between_extruding_segments
        // (3mm), so an exactly-3mm travel is still bridged as one continuous segment.
        const std::string gcode =
            ";_EXTRUSION_ROLE:1\n"
            "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
            "G1 X13 Y0 F1200\n"
            "G1 X23 Y0 E0.5 F300\n"
            "G1 X33 Y0 E0.5 F300\n"
            ";_EXTRUDE_END\n";
        std::string out = process_single_layer(config, gcode);
        auto pieces = parse_extrusion_pieces(out);

        THEN("the fast segment is still split into a ramp (gap is bridged)") {
            const size_t ramp_pieces = std::count_if(pieces.begin(), pieces.end(),
                                                     [](const OutPiece &p) { return p.x_end <= 10.0 + 1e-9; });
            REQUIRE(ramp_pieces == 5);
        }
        THEN("extrusion/geometry are conserved and the slope cap holds") {
            REQUIRE_THAT(sum_e(pieces), WithinAbs(1.5, 1e-3));
            REQUIRE_THAT(pieces.back().x_end, WithinAbs(33.0, 1e-6));
            require_rate_profile(pieces, 0.5, rate_slow, rate_fast);
        }
    }
    WHEN("a 5mm travel separates two different-rate extrusions") {
        GCodeConfig config;
        init_config(config, 0.5f);
        // Above the 3mm threshold the extrusions are separate segments: the fast
        // one is the last line of its own segment and is never adjusted.
        const std::string gcode =
            ";_EXTRUSION_ROLE:1\n"
            "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
            "G1 X15 Y0 F1200\n"
            "G1 X25 Y0 E0.5 F300\n"
            "G1 X35 Y0 E0.5 F300\n"
            ";_EXTRUDE_END\n";
        const std::string expected =
            "G1 X10 Y0 E0.5 F1200 ;_EXTRUDE_SET_SPEED\n"
            "G1 X15 Y0 F1200\n"
            "G1 X25 Y0 E0.5 F300\n"
            "G1 X35 Y0 E0.5 F300\n"
            ";_EXTRUDE_END\n";
        std::string out = process_single_layer(config, gcode);
        THEN("the output is byte-identical (segments are independent)") {
            REQUIRE(out == expected);
        }
    }
}
