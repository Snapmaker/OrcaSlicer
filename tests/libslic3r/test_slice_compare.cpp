#include <catch2/catch.hpp>
#include "libslic3r/SliceCompare/Snapshot.hpp"
#include "libslic3r/SliceCompare/Diff.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace Slic3r;
using namespace Slic3r::SliceCompare;

// Build a synthetic result: two layers (z=0.2, z=0.4), each one 10mm
// external-perimeter square drawn as 4 extrude moves at 60 mm/s,
// preceded by a travel move to the start corner.
static void make_result(GCodeProcessorResult& r, float y_shift = 0.f)
{
    auto add = [&r](EMoveType t, float x, float y, float z, float de, float f,
                    ExtrusionRole role) {
        GCodeProcessorResult::MoveVertex m;
        m.type = t; m.position = Vec3f(x, y, z); m.delta_extruder = de;
        m.feedrate = f; m.extrusion_role = role;
        r.moves.push_back(m);
    };
    for (float z : {0.2f, 0.4f}) {
        add(EMoveType::Travel, 0.f, 0.f + y_shift, z, 0.f, 200.f, erNone);
        add(EMoveType::Extrude, 10.f, 0.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 10.f, 10.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 0.f, 10.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 0.f, 0.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
    }
    r.filament_diameters = {1.75f};
    r.filament_densities = {1.24f};
    r.print_statistics.modes[(size_t)PrintEstimatedStatistics::ETimeMode::Normal].time = 123.f;
}

TEST_CASE("build_snapshot captures layers, segments, cells", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot s = build_snapshot(result, {{"layer_height", "0.2"}}, "A", "session");
    REQUIRE(s.layer_count == 2);
    REQUIRE(s.layers.size() == 2);
    REQUIRE(s.layers.count(20) == 1);   // z=0.20 -> key 20
    REQUIRE(s.layers.count(40) == 1);
    const LayerRec& l = s.layers.at(20);
    CHECK(l.segs.size() == 4);
    CHECK(l.extrusion_mm == Approx(2.0));                 // 4 x 0.5
    // 4 sides x 10mm at 60mm/s = 40/60 s
    CHECK(l.feature_seconds.at((uint8_t)erExternalPerimeter) == Approx(40.0/60.0).margin(1e-3));
    CHECK(l.cells.count({0, 0}) == 1);                    // 10mm grid: square touches cell (0,0)+(1,*)
    CHECK(s.est_seconds == Approx(123.0));
    CHECK(s.filament_mm == Approx(4.0));                  // 8 x 0.5 over both layers
    CHECK(s.config.at("layer_height") == "0.2");
    // grams: mm of 1.75 filament -> volume*density: pi*(0.0875cm)^2 * 0.4cm... just require > 0
    CHECK(s.filament_g > 0.0);
}

TEST_CASE("SnapshotStore ring buffer evicts oldest", "[slice_compare]")
{
    auto& st = SnapshotStore::instance();
    st.clear();
    std::vector<int> ids;
    for (int i = 0; i < 10; ++i) {
        Snapshot s; s.label = "snap" + std::to_string(i);
        ids.push_back(st.add(std::move(s)));
    }
    CHECK(st.list().size() == 8);
    CHECK(st.get(ids[0]) == nullptr);          // evicted
    CHECK(st.get(ids[9]) != nullptr);
    CHECK(st.list().front().second == "snap9"); // newest first
    st.clear();
}

TEST_CASE("diff_configs ignores volatile keys, reports changes", "[slice_compare]")
{
    Snapshot a, b;
    a.config = {{"layer_height","0.2"}, {"wall_loops","2"}, {"print_host","x"},
                {"different_settings_to_system","wall_loops;layer_height"}};
    b.config = {{"layer_height","0.16"},{"wall_loops","2"}, {"extra","1"},
                {"different_settings_to_system","layer_height"}};
    auto rows = diff_configs(a, b);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "extra");            // sorted by key; absent in a
    CHECK(rows[0].a.empty());
    CHECK(rows[1].key == "layer_height");
    CHECK(rows[1].a == "0.2"); CHECK(rows[1].b == "0.16");
    // print_host filtered out entirely (volatile)
}

TEST_CASE("diff_features aggregates per role", "[slice_compare]")
{
    // NOTE: make_result here takes an out-param (GCodeProcessorResult&) rather
    // than returning by value — see the file-static helper above; it was
    // changed in commit 061958a1a8 because GCodeProcessorResult holds a mutex
    // and is not copyable/movable, so `build_snapshot(make_result(), ...)` as
    // written in the task brief does not compile against the current helper.
    GCodeProcessorResult result_a, result_b;
    make_result(result_a);
    make_result(result_b);
    Snapshot a = build_snapshot(result_a, {}, "A", "session");
    Snapshot b = build_snapshot(result_b, {}, "B", "session");
    auto rows = diff_features(a, b);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == (uint8_t)erExternalPerimeter);
    CHECK(rows[0].sec_a == Approx(rows[0].sec_b));
    CHECK(rows[0].mm_a == Approx(4.0));
}

// NOTE: adapted from the task-4 brief, which calls `make_result()` by value.
// The file-static helper above takes an out-param (`make_result(r)` /
// `make_result(r, y_shift)`) because GCodeProcessorResult holds a mutex and
// is not copyable/movable — same intent as the brief, different call shape.

TEST_CASE("diff_layers matches equal heights, self-diff identical", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot a = build_snapshot(result, {}, "A", "s");
    LayerDiff d = diff_layers(a, a);
    CHECK(d.matched == 2); CHECK(d.identical == 2);
    CHECK(d.changed == 0); CHECK(d.a_only == 0); CHECK(d.b_only == 0);
}

TEST_CASE("diff_layers flags relocation via cells", "[slice_compare]")
{
    GCodeProcessorResult result_a, result_b;
    make_result(result_a, 0.f);
    make_result(result_b, 60.f);
    Snapshot a = build_snapshot(result_a, {}, "A", "s");
    Snapshot b = build_snapshot(result_b, {}, "B", "s"); // same amount, moved 60mm
    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 2);
    CHECK(d.changed == 2);
    const auto& row = d.rows.front();
    CHECK(row.overlap < 0.5);
    CHECK(std::find(row.flags.begin(), row.flags.end(), "RELOCATED") != row.flags.end());
}

TEST_CASE("diff_layers honest about unequal layer heights", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot a = build_snapshot(result, {}, "A", "s");       // z 0.2/0.4
    Snapshot b = a;
    b.layers.clear();
    LayerRec l; l.z = 0.3; l.extrusion_mm = 1.0; b.layers[30] = l;  // z=0.30 only
    LayerDiff d = diff_layers(a, b);
    CHECK(d.matched == 0); CHECK(d.a_only == 2); CHECK(d.b_only == 1);
}

// NOTE: adapted from the task-5 brief, which calls `make_result()` by value
// and passes the result straight into build_snapshot(). The file-static
// helper above takes an out-param (`make_result(r)`) because
// GCodeProcessorResult holds a mutex and is not copyable/movable — same
// intent as the brief, different call shape (see the task-4 note above).

TEST_CASE("diff_segments: identical layers are all both", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot s = build_snapshot(result, {}, "A", "s");
    const LayerRec& l = s.layers.at(20);
    SegDiff d = diff_segments(l, l);
    CHECK(d.both.size() == 4);
    CHECK(d.a_only.empty()); CHECK(d.b_only.empty()); CHECK(d.jitter.empty());
}

TEST_CASE("diff_segments: direction-insensitive", "[slice_compare]")
{
    LayerRec a, b;
    a.segs.push_back({0,0, 10,0, 1});
    b.segs.push_back({10,0, 0,0, 1});          // reversed
    SegDiff d = diff_segments(a, b);
    CHECK(d.both.size() == 1); CHECK(d.a_only.empty()); CHECK(d.b_only.empty());
}

TEST_CASE("diff_segments: 0.2mm shift is jitter, 2mm is real", "[slice_compare]")
{
    LayerRec a, b1, b2;
    a.segs.push_back({0,0, 10,0, 1});
    b1.segs.push_back({0,0.2f, 10,0.2f, 1});
    b2.segs.push_back({0,2.f,  10,2.f,  1});
    SegDiff d1 = diff_segments(a, b1);
    CHECK(d1.jitter.size() == 1); CHECK(d1.a_only.empty()); CHECK(d1.b_only.empty());
    SegDiff d2 = diff_segments(a, b2);
    CHECK(d2.a_only.size() == 1); CHECK(d2.b_only.size() == 1); CHECK(d2.jitter.empty());
}

TEST_CASE("load_snapshot_from_file parses gcode + config block", "[slice_compare]")
{
    auto tmp = std::filesystem::temp_directory_path() / "sc_test.gcode";
    std::ofstream f(tmp);
    f << "; CONFIG_BLOCK_START\n; layer_height = 0.2\n; wall_loops = 2\n; CONFIG_BLOCK_END\n"
      << "G21\nG90\nM83\n"
      << "G1 Z0.2 F600\n"
      << "G1 X0 Y0 F6000\n"
      << ";TYPE:Outer wall\n"
      << "G1 X10 Y0 E0.5 F3600\nG1 X10 Y10 E0.5\nG1 X0 Y10 E0.5\nG1 X0 Y0 E0.5\n";
    f.close();
    FileLoadResult r = load_snapshot_from_file(tmp.string());
    REQUIRE(r.snapshot.has_value());
    CHECK(r.snapshot->config.at("layer_height") == "0.2");
    CHECK(r.snapshot->layers.size() == 1);
    CHECK(r.snapshot->layers.begin()->second.segs.size() >= 4);
    CHECK(r.snapshot->source == tmp.string());
    std::filesystem::remove(tmp);
}

TEST_CASE("load_snapshot_from_file reports missing file", "[slice_compare]")
{
    FileLoadResult r = load_snapshot_from_file("Z:/definitely/not/here.gcode");
    CHECK(!r.snapshot.has_value());
    CHECK(!r.error.empty());
}

// --- final-review fix-wave coverage (2026-08-29) -------------------------

TEST_CASE("diff_layers flags SUPPORT-CHANGED only in the affected z band", "[slice_compare]")
{
    // Each layer has a wall (erExternalPerimeter) plus, optionally, support
    // (erSupportMaterial). Snapshot B drops the support seconds/extrusion/
    // segs/cells for exactly one z-band layer (key 40); the other two layers
    // are untouched copies.
    auto make_layer = [](double z, bool with_support) {
        LayerRec l;
        l.z = z;
        l.feature_seconds[(uint8_t)erExternalPerimeter] = 1.0;
        l.feature_extrusion_mm[(uint8_t)erExternalPerimeter] = 1.0;
        l.extrusion_mm += 1.0;
        l.segs.push_back({0, 0, 10, 0, (uint8_t)erExternalPerimeter});
        l.segs.push_back({10, 0, 10, 10, (uint8_t)erExternalPerimeter});
        l.cells.insert({0, 0});
        l.cells.insert({1, 1});
        l.bx0 = 0; l.by0 = 0; l.bx1 = 10; l.by1 = 10; l.has_bbox = true;
        if (with_support) {
            l.feature_seconds[(uint8_t)erSupportMaterial] = 1.0;
            l.feature_extrusion_mm[(uint8_t)erSupportMaterial] = 1.0;
            l.extrusion_mm += 1.0;
            l.segs.push_back({20, 20, 25, 20, (uint8_t)erSupportMaterial});
            l.cells.insert({2, 2});
        }
        return l;
    };

    Snapshot a, b;
    for (int i = 0; i < 3; ++i) {
        const double z = 0.2 * (i + 1);           // 0.2, 0.4, 0.6 -> keys 20, 40, 60
        const int key = (int)std::lround(z * 100.0);
        a.layers[key] = make_layer(z, true);
        b.layers[key] = make_layer(z, /*with_support=*/i != 1);   // support removed only at key 40
    }

    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 3);
    CHECK(d.changed == 1);
    CHECK(d.identical == 2);

    for (const auto& row : d.rows) {
        const bool has_support_flag =
            std::find(row.flags.begin(), row.flags.end(), "SUPPORT-CHANGED") != row.flags.end();
        if (row.zkey_a == 40) {
            CHECK(row.changed);
            CHECK(has_support_flag);
        } else {
            CHECK_FALSE(has_support_flag);
        }
    }

    // diff_segments on the affected layer puts the (removed) support segs in a_only.
    SegDiff sd = diff_segments(a.layers.at(40), b.layers.at(40));
    REQUIRE(sd.a_only.size() == 1);
    CHECK(sd.a_only[0].role == (uint8_t)erSupportMaterial);
    CHECK(sd.b_only.empty());
}

TEST_CASE("diff_layers flags GEOMETRY-CHANGED only past the 5mm bbox threshold", "[slice_compare]")
{
    Snapshot a, b;

    LayerRec la1; la1.z = 0.2; la1.has_bbox = true;
    la1.bx0 = 0; la1.bx1 = 10; la1.by0 = 0; la1.by1 = 10;
    LayerRec lb1 = la1; lb1.bx1 = 16;   // width delta 6mm > 5mm

    LayerRec la2; la2.z = 0.4; la2.has_bbox = true;
    la2.bx0 = 0; la2.bx1 = 10; la2.by0 = 0; la2.by1 = 10;
    LayerRec lb2 = la2; lb2.bx1 = 15;   // width delta 5mm, not > 5mm

    a.layers[20] = la1; b.layers[20] = lb1;
    a.layers[40] = la2; b.layers[40] = lb2;

    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 2);
    auto has_geom_flag = [](const LayerMatch& m) {
        return std::find(m.flags.begin(), m.flags.end(), "GEOMETRY-CHANGED") != m.flags.end();
    };
    REQUIRE(d.rows[0].zkey_a == 20);
    CHECK(has_geom_flag(d.rows[0]));            // >5mm -> flag present
    REQUIRE(d.rows[1].zkey_a == 40);
    CHECK_FALSE(has_geom_flag(d.rows[1]));      // <=5mm -> flag absent
}

TEST_CASE("diff_layers biggest_zkey_a picks the matched layer with max |d_seconds|", "[slice_compare]")
{
    Snapshot a, b;
    // Three matched layers (same keys on both sides), each "changed" via
    // |d_seconds| > MATCH_TOL, with strictly increasing magnitude so the
    // winner is unambiguous.
    struct { int key; double sec_a, sec_b; } specs[] = {
        {10, 1.0, 2.0},   // |d_seconds| = 1.0
        {20, 1.0, 3.5},   // |d_seconds| = 2.5
        {30, 1.0, 5.0},   // |d_seconds| = 4.0 (max)
    };
    for (const auto& s : specs) {
        LayerRec la; la.feature_seconds[(uint8_t)erExternalPerimeter] = s.sec_a;
        LayerRec lb; lb.feature_seconds[(uint8_t)erExternalPerimeter] = s.sec_b;
        a.layers[s.key] = la;
        b.layers[s.key] = lb;
    }

    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 3);
    CHECK(d.changed == 3);
    CHECK(d.biggest_zkey_a == 30);

    // All-identical snapshots (self-diff) -> no changed rows -> -1.
    LayerDiff self = diff_layers(a, a);
    CHECK(self.biggest_zkey_a == -1);
}

TEST_CASE("diff_features reports exact per-role extrusion mm, not length-proportional", "[slice_compare]")
{
    // Two roles share the same 10mm segment length but carry very different
    // extrusion mm. If diff_features ever derived mm from a length-weighted
    // split of the total instead of reading feature_extrusion_mm per role
    // exactly, both roles would come out ~3.5mm here instead of 2.0/5.0.
    LayerRec l;
    l.z = 0.2;
    l.segs.push_back({0, 0, 10, 0, (uint8_t)erExternalPerimeter});   // length 10mm
    l.segs.push_back({0, 0, 10, 0, (uint8_t)erSupportMaterial});     // length 10mm, different mm
    l.feature_extrusion_mm[(uint8_t)erExternalPerimeter] = 2.0;
    l.feature_extrusion_mm[(uint8_t)erSupportMaterial]   = 5.0;
    l.feature_seconds[(uint8_t)erExternalPerimeter] = 1.0;
    l.feature_seconds[(uint8_t)erSupportMaterial]   = 1.0;

    Snapshot a, b;
    a.layers[20] = l;
    b.layers[20] = l;

    auto rows = diff_features(a, b);
    REQUIRE(rows.size() == 2);

    const FeatureRow* wall = nullptr;
    const FeatureRow* supp = nullptr;
    for (const auto& r : rows) {
        if (r.role == (uint8_t)erExternalPerimeter) wall = &r;
        if (r.role == (uint8_t)erSupportMaterial)   supp = &r;
    }
    REQUIRE(wall != nullptr);
    REQUIRE(supp != nullptr);

    CHECK(wall->mm_a == Approx(2.0));
    CHECK(wall->mm_b == Approx(2.0));
    CHECK(supp->mm_a == Approx(5.0));
    CHECK(supp->mm_b == Approx(5.0));
    // Both roles have identical path length, so the differing mm above cannot
    // be an artifact of a length-proportional split.
    CHECK(wall->len_a == Approx(10.0));
    CHECK(supp->len_a == Approx(10.0));
}

TEST_CASE("build_snapshot captures max_speed and layer bbox", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot s = build_snapshot(result, {}, "A", "session");
    // Travel move runs at feedrate 200; extrude moves run at 60 -> max_speed
    // must reflect the travel move, proving it isn't extrude-only.
    CHECK(s.max_speed == Approx(200.0));
    const LayerRec& l = s.layers.at(20);
    CHECK(l.has_bbox);
    CHECK(l.bx0 == Approx(0.0));
    CHECK(l.bx1 == Approx(10.0));
    CHECK(l.by0 == Approx(0.0));
    CHECK(l.by1 == Approx(10.0));
}

// Orca #16007 Stage A: Standard-only / step-size-1 configs must keep get_value_at == get_at,
// which is the byte-identity gate for non-HF output. Packed U1 arrays are a different case
// (filament >= 1 used to read an earlier filament's High-Flow slot); see the PR body.
TEST_CASE("Standard-only filament flow columns resolve to the raw filament id", "[slice_compare]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values = {1, 1};
    config.option<ConfigOptionStrings>("filament_flow_support", true)->values = {FLOW_MODE_STANDARD,
                                                                                FLOW_MODE_STANDARD};
    config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = {int(fvtStandard),
                                                                                     int(fvtStandard)};
    config.option<ConfigOptionInts>("nozzle_temperature")->values               = {200, 215};
    config.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {195, 211};
    config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")->values = {3., 7.};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values  = {11., 13.};
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values      = {true, false};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_volume")->values = {6., 9.};
    config.option<ConfigOptionFloats>("filament_multitool_ramming_flow")->values   = {3., 4.};

    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 0) == 0);
    REQUIRE(get_config_idx(config, ConfigFlowDomain::Filament, 1) == 1);

    const auto *temps  = config.option<ConfigOptionInts>("nozzle_temperature");
    const auto *inits  = config.option<ConfigOptionInts>("nozzle_temperature_initial_layer");
    const auto *purge  = config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower");
    const auto *vol    = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const auto *ram_on = config.option<ConfigOptionBools>("filament_multitool_ramming");
    const auto *ram_v  = config.option<ConfigOptionFloats>("filament_multitool_ramming_volume");
    const auto *ram_f  = config.option<ConfigOptionFloats>("filament_multitool_ramming_flow");
    for (unsigned int id : {0u, 1u}) {
        CHECK(get_value_at(config, *temps, ConfigFlowDomain::Filament, id) == temps->get_at(id));
        CHECK(get_value_at(config, *inits, ConfigFlowDomain::Filament, id) == inits->get_at(id));
        CHECK(get_value_at(config, *purge, ConfigFlowDomain::Filament, id) == purge->get_at(id));
        CHECK(get_value_at(config, *vol, ConfigFlowDomain::Filament, id) == vol->get_at(id));
        CHECK(get_value_at(config, *ram_on, ConfigFlowDomain::Filament, id) == ram_on->get_at(id));
        CHECK(get_value_at(config, *ram_v, ConfigFlowDomain::Filament, id) == ram_v->get_at(id));
        CHECK(get_value_at(config, *ram_f, ConfigFlowDomain::Filament, id) == ram_f->get_at(id));
    }
}
