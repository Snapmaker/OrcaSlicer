// Tests for the GCodeProcessor rework ported from OrcaSlicer #10735 (libvgcode stage 1): per-move times,
// the per-move-type / per-role / per-layer tables recomputed from them, PrusaSlicer's G2/G3
// discretisation and the actual-speed profile.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

using Move = GCodeProcessorResult::MoveVertex;
constexpr size_t NORMAL = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

// Runs the processor over a G-code text the way the G-code viewer does (process_file()).
void process(GCodeProcessorResult &out, const std::string &gcode, bool actual_speed_moves = false, const PrintConfig *config = nullptr)
{
    // Bambu style tags (" FEATURE: ", " CHANGE_LAYER", ...)
    const bool was_bbl = GCodeProcessor::s_IsBBLPrinter;
    GCodeProcessor::s_IsBBLPrinter = true;

    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("edge_gcp_times_%%%%%%%%.gcode");
    {
        boost::nowide::ofstream out(path.string(), std::ios::binary);
        out << gcode;
    }
    GCodeProcessor processor;
    if (config != nullptr)
        processor.apply_config(*config);
    processor.enable_actual_speed_moves(actual_speed_moves);
    processor.process_file(path.string());
    boost::filesystem::remove(path);
    GCodeProcessor::s_IsBBLPrinter = was_bbl;
    out = std::move(processor.extract_result());
}

double sum_move_times(const GCodeProcessorResult &result)
{
    double sum = 0.;
    for (const Move &m : result.moves)
        sum += m.time[NORMAL];
    return sum;
}

float table_time(const std::vector<std::pair<ExtrusionRole, float>> &table, ExtrusionRole role)
{
    auto it = std::find_if(table.begin(), table.end(), [role](const auto &item) { return item.first == role; });
    return it == table.end() ? 0.f : it->second;
}

float table_time(const std::vector<std::pair<EMoveType, float>> &table, EMoveType type)
{
    auto it = std::find_if(table.begin(), table.end(), [type](const auto &item) { return item.first == type; });
    return it == table.end() ? 0.f : it->second;
}

// A G-code from an unknown producer gets no machine config, so each test G-code sets an acceleration
// (M204 S1500); without one the time machine runs with 0 mm/s^2.

// Two layers: start G-code (erCustom, with a travel), outer / inner walls, infill, travels and a 0.5 s dwell.
const char *const TWO_LAYERS = R"(G90
M83
M204 S1500
; FEATURE: Custom
G1 Z5 F600
G1 X5 Y5 F6000
G1 X5 Y15 E2 F600
G1 Z0.2 F600
; CHANGE_LAYER
; FEATURE: Outer wall
G1 X10 Y0 F6000
G1 X20 Y0 E0.5 F1800
G1 X20 Y10 E0.5
G4 P500
G1 X30 Y10 F6000
; FEATURE: Sparse infill
G1 X40 Y10 E0.5 F3000
G1 X40 Y30 E1.0
; CHANGE_LAYER
G1 Z0.4 F600
; FEATURE: Inner wall
G1 X40 Y20 E0.5 F1800
G1 X30 Y20 E0.5
G1 X10 Y20 F6000
G1 X10 Y30 E0.5 F1800
)";

} // namespace

TEST_CASE("Per-move times add up to the time estimate", "[GCodeProcessor][Times]")
{
    GCodeProcessorResult r;
    process(r, TWO_LAYERS);
    const PrintEstimatedStatistics::Mode &mode = r.print_statistics.modes[NORMAL];
    REQUIRE(mode.time > 1.f);
    CHECK(sum_move_times(r) == Approx(mode.time).epsilon(1e-5));
    // the dwell is in there
    CHECK(mode.time > 0.5f);

    // markers and the dummy first move carry no time
    for (const Move &m : r.moves)
        if (m.type == EMoveType::Noop || m.type == EMoveType::Seam || m.type == EMoveType::Custom_GCode)
            CHECK(m.time[NORMAL] == 0.f);
    // every real motion has some
    for (size_t i = 1; i < r.moves.size(); ++i)
        if (r.moves[i].type == EMoveType::Extrude || r.moves[i].type == EMoveType::Travel)
            CHECK(r.moves[i].time[NORMAL] > 0.f);
}

TEST_CASE("Layer, role and move-type tables are recomputed from the per-move times", "[GCodeProcessor][Times]")
{
    GCodeProcessorResult r;
    process(r, TWO_LAYERS);
    const PrintEstimatedStatistics::Mode &mode = r.print_statistics.modes[NORMAL];

    SECTION("per layer")
    {
        // start G-code and the first layer share layer 0 (the time machine's max(1, layer) - 1)
        REQUIRE(mode.layers_times.size() == 2);
        std::vector<double> expected(2, 0.);
        for (const Move &m : r.moves)
            expected[m.layer_id] += m.time[NORMAL];
        CHECK(mode.layers_times[0] == Approx(expected[0]).epsilon(1e-6));
        CHECK(mode.layers_times[1] == Approx(expected[1]).epsilon(1e-6));
        CHECK(double(mode.layers_times[0]) + mode.layers_times[1] == Approx(mode.time).epsilon(1e-5));
    }

    SECTION("per role: travel counts as erNone unless it is custom G-code")
    {
        double custom = 0., none = 0., outer = 0.;
        for (const Move &m : r.moves) {
            if (m.extrusion_role == erCustom)
                custom += m.time[NORMAL];
            else if (m.type == EMoveType::Travel)
                none += m.time[NORMAL];
            else if (m.extrusion_role == erExternalPerimeter)
                outer += m.time[NORMAL];
        }
        CHECK(table_time(mode.roles_times, erCustom) == Approx(custom).epsilon(1e-6));
        CHECK(table_time(mode.roles_times, erNone) == Approx(none).epsilon(1e-6));
        CHECK(table_time(mode.roles_times, erExternalPerimeter) == Approx(outer).epsilon(1e-6));
        double sum = 0.;
        for (const auto &[role, t] : mode.roles_times)
            sum += t;
        CHECK(sum == Approx(mode.time).epsilon(1e-5));
    }

    SECTION("per move type: travel inside the start G-code is left out")
    {
        double travel = 0., start_travel = 0., extrude = 0.;
        for (const Move &m : r.moves) {
            if (m.type == EMoveType::Travel)
                (m.prepare_stage ? start_travel : travel) += m.time[NORMAL];
            else if (m.type == EMoveType::Extrude)
                extrude += m.time[NORMAL];
        }
        REQUIRE(start_travel > 0.);
        CHECK(table_time(mode.moves_times, EMoveType::Travel) == Approx(travel).epsilon(1e-6));
        CHECK(table_time(mode.moves_times, EMoveType::Extrude) == Approx(extrude).epsilon(1e-6));
    }

    SECTION("first layer time keeps the Bambu slice_info meaning (layer 0 minus all custom G-code time)")
    {
        const float custom = table_time(mode.roles_times, erCustom);
        REQUIRE(custom > 0.f);
        CHECK(r.initial_layer_time == Approx(std::max(0.f, mode.layers_times[0] - custom)));
    }

    SECTION("the last move belongs to the second layer (libvgcode's per-layer times key on layer_id)")
    {
        const Move &last = r.moves.back();
        CHECK(last.layer_id == 1);
    }
}

TEST_CASE("fill_time_tables applies the time machine's old table rules", "[GCodeProcessor][Times]")
{
    std::vector<Move> moves(5);
    auto set = [&moves](size_t i, EMoveType type, ExtrusionRole role, unsigned int layer, float t, bool prepare) {
        moves[i].type = type;
        moves[i].extrusion_role = role;
        moves[i].layer_id = layer;
        moves[i].time[NORMAL] = t;
        moves[i].prepare_stage = prepare;
    };
    set(0, EMoveType::Travel, erCustom, 0, 1.f, true);             // start G-code travel
    set(1, EMoveType::Extrude, erCustom, 0, 2.f, true);            // purge line
    set(2, EMoveType::Travel, erExternalPerimeter, 0, 4.f, false); // travel: role erNone
    set(3, EMoveType::Extrude, erExternalPerimeter, 2, 8.f, false);
    set(4, EMoveType::Travel, erCustom, 2, 16.f, false);           // end G-code travel keeps erCustom
    PrintEstimatedStatistics::Mode mode;
    GCodeProcessor::fill_time_tables(moves, PrintEstimatedStatistics::ETimeMode::Normal, mode);

    CHECK(table_time(mode.moves_times, EMoveType::Travel) == 20.f);
    CHECK(table_time(mode.moves_times, EMoveType::Extrude) == 10.f);
    CHECK(table_time(mode.roles_times, erCustom) == 19.f);
    CHECK(table_time(mode.roles_times, erNone) == 4.f);
    CHECK(table_time(mode.roles_times, erExternalPerimeter) == 8.f);
    REQUIRE(mode.layers_times.size() == 3);
    CHECK(mode.layers_times[0] == 7.f);
    CHECK(mode.layers_times[1] == 0.f);
    CHECK(mode.layers_times[2] == 24.f);
}

TEST_CASE("G2/G3 arcs are discretised into internal G1 moves", "[GCodeProcessor][Arc]")
{
    // half circle around (0,0) with radius 10, from (10,0) to (-10,0)
    auto arc_gcode = [](const char *arc_line) {
        return std::string("G90\nM83\nM204 S1500\nG1 X10 Y0 Z0.2 F600\n; CHANGE_LAYER\n; FEATURE: Outer wall\n") + arc_line +
               "\nG1 X-10 Y-5 E0.2\n";
    };
    const size_t expected_segments = Geometry::ArcWelder::arc_discretization_steps(10., M_PI, 0.0125);
    REQUIRE(expected_segments > 10);

    auto arc_moves = [](const GCodeProcessorResult &r) {
        std::vector<const Move *> out;
        for (const Move &m : r.moves)
            if (m.internal_only)
                out.push_back(&m);
        return out;
    };

    SECTION("G3 (counter-clockwise) with I/J")
    {
        GCodeProcessorResult r;
        process(r, arc_gcode("G3 X-10 Y0 I-10 J0 E3 F1200"));
        const std::vector<const Move *> arc = arc_moves(r);
        REQUIRE(arc.size() == expected_segments);
        double e = 0., t = 0.;
        for (const Move *m : arc) {
            CHECK(m->type == EMoveType::Extrude);
            CHECK(m->extrusion_role == erExternalPerimeter);
            CHECK(m->gcode_id == arc.front()->gcode_id);           // all on the G3 line
            CHECK(Vec2f(m->position.x(), m->position.y()).norm() == Approx(10.f).margin(1e-3));
            CHECK(m->position.y() >= -1e-4f);                      // through +Y
            e += m->delta_extruder;
            t += m->time[NORMAL];
        }
        CHECK(arc.back()->position.x() == Approx(-10.f).margin(1e-4));
        CHECK(arc.back()->position.y() == Approx(0.f).margin(1e-4));
        CHECK(e == Approx(3.));
        // 31.4 mm at 20 mm/s, plus acceleration
        CHECK(t > M_PI * 10. / 20.);
        CHECK(t < M_PI * 10. / 20. + 1.);
        // the following G1 is a move of its own line
        CHECK_FALSE(r.moves.back().internal_only);
        CHECK(r.moves.back().gcode_id > arc.front()->gcode_id);
        CHECK(sum_move_times(r) == Approx(r.print_statistics.modes[NORMAL].time).epsilon(1e-5));
    }

    SECTION("G2 (clockwise) with I/J")
    {
        GCodeProcessorResult r;
        process(r, arc_gcode("G2 X-10 Y0 I-10 J0 E3 F1200"));
        const std::vector<const Move *> arc = arc_moves(r);
        REQUIRE(arc.size() == expected_segments);
        for (const Move *m : arc)
            CHECK(m->position.y() <= 1e-4f);                       // through -Y
    }

    SECTION("G3 with R")
    {
        // quarter circle (10,0) -> (0,10), center (0,0)
        GCodeProcessorResult r;
        process(r, arc_gcode("G3 X0 Y10 R10 E1.5 F1200"));
        const std::vector<const Move *> arc = arc_moves(r);
        REQUIRE(arc.size() == Geometry::ArcWelder::arc_discretization_steps(10., M_PI / 2., 0.0125));
        for (const Move *m : arc) {
            CHECK(Vec2f(m->position.x(), m->position.y()).norm() == Approx(10.f).margin(1e-3));
            CHECK(m->position.x() >= -1e-4f);
            CHECK(m->position.y() >= -1e-4f);
        }
    }

    SECTION("Marlin 2 firmware: plan_arc() segment length")
    {
        PrintConfig config;
        config.gcode_flavor.value = gcfMarlinFirmware;
        GCodeProcessorResult r;
        process(r, arc_gcode("G3 X-10 Y0 I-10 J0 E3 F1200"), false, &config);
        // segment = clamp(min(sqrt(8 * 10 * 0.02), 20 / 50), 0.1, 2) = 0.4 mm; 31.42 / 0.4 + 0.8 -> 79
        CHECK(arc_moves(r).size() == 79);
    }

    SECTION("an arc without a center or radius is skipped")
    {
        GCodeProcessorResult r;
        process(r, arc_gcode("G3 X-10 Y0 E3 F1200"));
        CHECK(arc_moves(r).empty());
    }
}

TEST_CASE("A seam vertex does not take its move's time", "[GCodeProcessor][Times]")
{
    GCodeProcessorResult r;
    process(r, R"(G90
M83
M204 S1500
G1 Z0.2 F600
; CHANGE_LAYER
; FEATURE: Outer wall
G1 X0 Y0 F6000
G1 X10 Y0 E0.5 F1800
G1 X10 Y10 E0.5
G1 X0 Y10 E0.5
G1 X0 Y0.1 E0.5
; FEATURE: Inner wall
G1 X1 Y1 E0.1
G1 X5 Y1 E0.2
)");
    auto seam = std::find_if(r.moves.begin(), r.moves.end(), [](const Move &m) { return m.type == EMoveType::Seam; });
    REQUIRE(seam != r.moves.end());
    CHECK(seam->time[NORMAL] == 0.f);
    // the inner wall move stored right after the seam vertex carries the time of its block
    REQUIRE(seam + 1 != r.moves.end());
    CHECK((seam + 1)->extrusion_role == erPerimeter);
    CHECK((seam + 1)->time[NORMAL] > 0.f);
    CHECK(sum_move_times(r) == Approx(r.print_statistics.modes[NORMAL].time).epsilon(1e-5));
}

TEST_CASE("Moves queued behind a relocated custom G-code marker keep their time", "[GCodeProcessor][Times]")
{
    GCodeProcessorResult r;
    process(r, R"(G90
M83
M204 S1500
G1 Z0.2 F600
; CHANGE_LAYER
; FEATURE: Outer wall
G1 X0 Y0 F6000
G1 X10 Y0 E0.5 F1800
; CUSTOM_GCODE
G1 X20 Y0 F6000
G1 X30 Y0 E0.5 F1800
G1 X40 Y0 E0.5
)");
    auto marker = std::find_if(r.moves.begin(), r.moves.end(), [](const Move &m) { return m.type == EMoveType::Custom_GCode; });
    REQUIRE(marker != r.moves.end());
    CHECK(marker->time[NORMAL] == 0.f);
    for (size_t i = 1; i < r.moves.size(); ++i)
        if (r.moves[i].type == EMoveType::Extrude || r.moves[i].type == EMoveType::Travel)
            CHECK(r.moves[i].time[NORMAL] > 0.f);
    CHECK(sum_move_times(r) == Approx(r.print_statistics.modes[NORMAL].time).epsilon(1e-5));
}

TEST_CASE("Actual speed profile", "[GCodeProcessor][ActualSpeed]")
{
    const char *gcode = R"(G90
M83
M204 S1500
G1 Z0.2 F600
; CHANGE_LAYER
; FEATURE: Outer wall
G1 X10 Y0 F12000
G1 X100 Y0 E5 F9000
G1 X100 Y100 E5
G1 X0 Y100 F12000
)";
    GCodeProcessorResult plain, with;
    process(plain, gcode, false);
    process(with, gcode, true);

    SECTION("end speeds are filled in without inserting moves")
    {
        size_t motions = 0;
        for (const Move &m : plain.moves)
            if (m.type == EMoveType::Extrude || m.type == EMoveType::Travel) {
                ++motions;
                CHECK(m.actual_feedrate > 0.f);
                CHECK(m.actual_feedrate <= m.feedrate + 1e-3f);
                CHECK_FALSE(m.internal_only);
            }
        CHECK(motions == 5); // Z lift, two travels, two extrusions
    }

    SECTION("enabled: acceleration and deceleration points become internal moves")
    {
        REQUIRE(with.moves.size() > plain.moves.size());
        size_t inserted = 0;
        for (size_t i = 1; i + 1 < with.moves.size(); ++i) {
            const Move &m = with.moves[i];
            if (!m.internal_only)
                continue;
            ++inserted;
            CHECK(m.time[NORMAL] == 0.f);
            CHECK(m.actual_feedrate > 0.f);
            // on the segment between its neighbours
            const Vec3f a = with.moves[i - 1].position, b = with.moves[i + 1].position;
            CHECK((m.position - a).norm() + (b - m.position).norm() == Approx((b - a).norm()).epsilon(1e-4));
        }
        CHECK(inserted == with.moves.size() - plain.moves.size());
    }

    SECTION("enabling them changes no time")
    {
        const PrintEstimatedStatistics::Mode &a = plain.print_statistics.modes[NORMAL];
        const PrintEstimatedStatistics::Mode &b = with.print_statistics.modes[NORMAL];
        CHECK(a.time == b.time);
        CHECK(a.layers_times == b.layers_times);
        CHECK(a.roles_times == b.roles_times);
        CHECK(a.moves_times == b.moves_times);
        CHECK(sum_move_times(with) == Approx(b.time).epsilon(1e-5));
    }
}
