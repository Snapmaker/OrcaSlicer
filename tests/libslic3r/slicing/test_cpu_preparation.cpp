#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/MixedFilament/LayerTracker.hpp"
#include "libslic3r/MixedFilament/ChannelMap.hpp"
#include "libslic3r/Slicing/LayerGeometryBatch.hpp"
#include "libslic3r/GCodeReader.hpp"
#include <string>

using namespace Slic3r;

namespace {
ExPolygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    return ExPolygon(Polygon(Points{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}));
}
} // namespace

TEST_CASE("Mixed channel snapshots preserve hidden rows and borrowed legacy edits", "[CPUSlicing][PaintPreparation]")
{
    MixedFilamentManager       manager;
    std::vector<MixedFilament> rows(4);
    for (auto& row : rows) {
        row.component_a = 1;
        row.component_b = 2;
    }
    rows[0].deleted = true;
    rows[1].enabled = false;
    manager.set_mixed_filament_legacy_rows(rows, 2, {"#FFFFFF", "#000000"});
    const auto initial = mixed_filament_channel_rows(manager, 2, 7);
    REQUIRE(initial == std::vector<int>{-1, -1, -1, 2, 3, -1, -1});
    auto& borrowed      = manager.mixed_filaments();
    borrowed[2].enabled = false;
    const auto updated  = mixed_filament_channel_rows(manager, 2, 7);
    REQUIRE(updated == std::vector<int>{-1, -1, -1, 3, -1, -1, -1});
    CHECK(initial[3] == 2);
    for (size_t channel = 0; channel < updated.size(); ++channel)
        CHECK(updated[channel] == manager.mixed_index_from_filament_id(unsigned(channel), 2));
    CHECK(mixed_filament_channel_rows(manager, 2, 0).empty());
}

TEST_CASE("Layer geometry batches preserve progress and neighbor halos at boundaries", "[CPUSlicing][PaintPreparation]")
{
    struct Fixture
    {
        std::vector<size_t> points, ends;
    };
    std::vector<Fixture> fixtures{{std::vector<size_t>(64, 1), {64}},
                                  {std::vector<size_t>(65, 1), {64, 65}},
                                  {std::vector<size_t>(66, 1), {64, 66}},
                                  {{200, 1, 1}, {1, 3}},
                                  {{1, 200, 1}, {1, 2, 3}},
                                  {{1, 1, 200}, {2, 3}},
                                  {{0}, {1}}};
    fixtures[2].points[64] = 0;
    for (const auto& fixture : fixtures) {
        size_t begin = 0;
        for (size_t expected_end : fixture.ends) {
            const auto batch = next_layer_geometry_batch(begin, fixture.points.size(), 64, 128,
                                                         [&](size_t i) { return fixture.points.at(i); });
            REQUIRE(batch.end == expected_end);
            CHECK(batch.begin == begin);
            CHECK(batch.neighbor_begin == (begin == 0 ? 0 : begin - 1));
            CHECK(batch.neighbor_end == std::min(expected_end + 1, fixture.points.size()));
            if (batch.input_points > 128)
                CHECK(batch.end == begin + 1);
            for (size_t layer = batch.begin; layer < batch.end; ++layer) {
                CHECK(layer >= batch.neighbor_begin);
                CHECK(layer < batch.neighbor_end);
                if (layer > 0)
                    CHECK(layer - 1 >= batch.neighbor_begin);
                if (layer + 1 < fixture.points.size())
                    CHECK(layer + 1 < batch.neighbor_end);
            }
            begin = batch.end;
        }
        CHECK(begin == fixture.points.size());
    }
}

TEST_CASE("Cheap mixed-component overlap preserves holes, contacts and earliest activation", "[CPUSlicing][PaintPreparation]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const std::vector<ExPolygons> layers{{rectangle(200, 0, 250, 50)},
                                         {ring, rectangle(200, 0, 250, 50)},
                                         {rectangle(25, 25, 75, 75), rectangle(5, 5, 15, 15), rectangle(100, 0, 110, 10),
                                          rectangle(200, 0, 250, 50)},
                                         {rectangle(0, 0, 300, 100)},
                                         {},
                                         {ring},
                                         {ring}};
    MixedFilamentLayerTracker     reference, optimized(true);
    std::vector<int>              expected, actual;
    for (size_t layer = 0; layer < layers.size(); ++layer) {
        reference.track_layer(layers[layer], int(layer), expected);
        optimized.track_layer(layers[layer], int(layer), actual);
        REQUIRE(actual == expected);
        if (layer == 3)
            CHECK(actual == std::vector<int>{3});
    }
}

TEST_CASE("Mixed painted components retain cadence through splits and merges", "[CPUSlicing][MixedFilament]")
{
    // All layers remain alive, as in the segmentation pass that owns the masks.
    const std::vector<ExPolygons>       layers{{rectangle(0, 0, 100, 100)},
                                               {rectangle(0, 0, 40, 100), rectangle(60, 0, 100, 100), rectangle(200, 0, 300, 100)},
                                               {rectangle(0, 0, 300, 100)},
                                               {},
                                               {rectangle(0, 0, 100, 100)}};
    const std::vector<std::vector<int>> expected{{0}, {1, 1, 0}, {2}, {}, {0}};
    MixedFilamentLayerTracker           tracker;
    std::vector<int>                    indices;
    for (size_t i = 0; i < layers.size(); ++i) {
        tracker.track_layer(layers[i], int(i), indices);
        REQUIRE(indices == expected[i]);
    }
}

TEST_CASE("Mixed painted cadence uses polygon overlap including holes", "[CPUSlicing][MixedFilament]")
{
    ExPolygon ring = rectangle(0, 0, 100, 100);
    Polygon   hole = rectangle(20, 20, 80, 80).contour;
    hole.reverse();
    ring.holes.push_back(hole);
    const ExPolygons          first{ring};
    const ExPolygons          second{rectangle(30, 30, 70, 70), rectangle(5, 5, 15, 15), ExPolygon{}};
    MixedFilamentLayerTracker tracker;
    std::vector<int>          indices;
    tracker.track_layer(first, 5, indices);
    tracker.track_layer(second, 6, indices);
    REQUIRE(indices == std::vector<int>{0, 1, -1});
}

TEST_CASE("Mixed painted channels have independent activation runs", "[CPUSlicing][MixedFilament]")
{
    const ExPolygons          masks{rectangle(0, 0, 100, 100)};
    MixedFilamentLayerTracker first, second;
    std::vector<int>          first_indices, second_indices;
    first.track_layer(masks, 0, first_indices);
    second.track_layer(masks, 2, second_indices);
    first.track_layer(masks, 3, first_indices);
    second.track_layer(masks, 3, second_indices);
    REQUIRE(first_indices == std::vector<int>{3});
    REQUIRE(second_indices == std::vector<int>{1});
}

TEST_CASE("Read-only extrusion flattening borrows leaves without changing order", "[CPUSlicing][GCode]")
{
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(Points{{0, 0}, {100, 0}, {100, 100}});
    ExtrusionEntityCollection inner;
    inner.no_sort = true;
    inner.append(ExtrusionLoop(path));
    inner.append(ExtrusionMultiPath(path));
    ExtrusionEntityCollection source;
    source.append(path);
    source.append(inner);
    source.append(ExtrusionEntityCollection{});
    source.append(path);

    const auto  leaves = source.flatten_refs();
    const auto* nested = static_cast<const ExtrusionEntityCollection*>(source.entities[1]);
    REQUIRE(leaves == std::vector<const ExtrusionEntity*>{source.entities[0], nested->entities[0], nested->entities[1], source.entities[3]});
    const auto copied = source.flatten(false);
    REQUIRE(leaves.size() == copied.entities.size());
    for (size_t i = 0; i < leaves.size(); ++i) {
        CHECK(leaves[i] != copied.entities[i]);
        CHECK(leaves[i]->role() == copied.entities[i]->role());
        CHECK(leaves[i]->is_loop() == copied.entities[i]->is_loop());
        Points original_points, copied_points;
        leaves[i]->collect_points(original_points);
        copied.entities[i]->collect_points(copied_points);
        CHECK(original_points == copied_points);
    }
    REQUIRE(ExtrusionEntityCollection{}.flatten_refs().empty());
}

TEST_CASE("Borrowed streaming buffers preserve commands and modal coordinates", "[CPUSlicing][GCode]")
{
    const std::vector<std::string> buffers{"G90\nG1 X10 Y5 Z0.2 E1\n", "G91\r\nG1 X2 Y-1\r\n", "G1 X1", ""};
    GCodeReader                    owned, borrowed;
    std::vector<std::string>       expected, actual;
    for (const auto& buffer : buffers) {
        owned.parse_buffer(buffer, [&](GCodeReader&, const GCodeReader::GCodeLine& line) { expected.push_back(line.raw()); });
        borrowed.parse_buffer(buffer.c_str(), buffer.size(),
                              [&](GCodeReader&, const GCodeReader::GCodeLine& line) { actual.push_back(line.raw()); });
    }
    CHECK(actual == expected);
    REQUIRE(actual.size() == 5);
    CHECK(actual.back() == "G1 X1");
    CHECK(borrowed.x() == 13.f);
    CHECK(borrowed.y() == 4.f);
    CHECK(borrowed.x() == owned.x());
    CHECK(borrowed.z() == owned.z());
    CHECK(borrowed.e() == owned.e());
    CHECK(borrowed.relative_xyz());
}
