// Synthetic plates exercise ordering and rod clearance without external assets.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
constexpr double HEIGHT_TO_ROD = 27.5; // Snapmaker U1 profile.

DynamicPrintConfig sequential_config()
{
    auto config = default_print_config();
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    config.set_key_value("extruder_clearance_height_to_rod", new ConfigOptionFloat(HEIGHT_TO_ROD));
    config.set_key_value("extruder_clearance_height_to_lid", new ConfigOptionFloat(140));
    config.set_key_value("extruder_clearance_radius", new ConfigOptionFloat(72.5));
    config.set_key_value("skirt_loops", new ConfigOptionInt(0));
    return config;
}

TriangleMesh box(double x, double y, double z)
{
    auto mesh = Test::mesh(TestMesh::cube_20x20x20);
    mesh.scale(Vec3f(x / 20., y / 20., z / 20.));
    return mesh;
}

TriangleMesh slab_with_fin(bool back, double width = 2.)
{
    auto slab = box(40, 30, 2);
    auto fin  = box(8, width, 32);
    fin.translate(16.f, back ? float(30 - width) : 0.f, 0.f);
    slab.merge(fin);
    return slab;
}

void place(Print& print, Model& model, const DynamicPrintConfig& config, const std::vector<Vec2d>& centres)
{
    REQUIRE(model.objects.size() == centres.size());
    for (size_t i = 0; i < centres.size(); ++i) {
        auto* inst = model.objects[i]->instances.front();
        inst->set_offset(Vec3d(centres[i].x(), centres[i].y(), inst->get_offset().z()));
    }
    print.apply(model, config);
}

bool too_tall(const Print& print) { return Print::sequential_print_clearance_valid(print).string.find("too tall") != std::string::npos; }

void require_order(const Print& print, size_t first, size_t second)
{
    auto order = sort_object_instances_by_model_order(print);
    REQUIRE(order.size() == 2);
    REQUIRE(order[0]->print_object->model_object() == print.model().objects[first]);
    REQUIRE(order[1]->print_object->model_object() == print.model().objects[second]);
    REQUIRE(order[0]->model_instance->arrange_order == 1);
    REQUIRE(order[1]->model_instance->arrange_order == 2);
}
} // namespace

TEST_CASE("Sequential clearance searches for a safe order", "[SequentialClearance]")
{
    Print print;
    Model model;
    auto  config = sequential_config();
    config.set_key_value("gcode_label_objects", new ConfigOptionBool(true));
    init_print({box(40, 40, 35), box(40, 40, 10)}, print, model, config);
    model.objects[0]->name = "clearance_tall";
    model.objects[1]->name = "clearance_short";
    place(print, model, config, {Vec2d(60, 130), Vec2d(190, 130)});
    REQUIRE(Print::sequential_print_clearance_valid(print).string.empty());
    require_order(print, 1, 0);
    const auto code       = Test::gcode(print);
    const auto tall_start = code.find("; printing object clearance_tall");
    const auto short_end  = code.rfind("; stop printing object clearance_short");
    REQUIRE(tall_start != std::string::npos);
    REQUIRE(short_end != std::string::npos);
    REQUIRE(short_end < tall_start);
}

TEST_CASE("Sequential clearance rejects unavoidable rod and lid collisions", "[SequentialClearance]")
{
    for (double height : {35., 150.}) {
        CAPTURE(height);
        Print print;
        Model model;
        auto  config = sequential_config();
        init_print({box(40, 40, height), box(40, 40, height)}, print, model, config);
        place(print, model, config, {Vec2d(60, 130), Vec2d(190, 140)});
        REQUIRE(too_tall(print));
        require_order(print, 0, 1); // Unsolvable constraints retain the list order.
    }
}

TEST_CASE("Sequential clearance preserves narrow material above the rod", "[SequentialClearance]")
{
    for (double width : {0.15, 0.2, 0.21, 2.}) {
        for (bool facing : {true, false}) {
            CAPTURE(width, facing);
            Print print;
            Model model;
            auto  config = sequential_config();
            init_print({slab_with_fin(!facing, width), slab_with_fin(facing, width)}, print, model, config);
            place(print, model, config, {Vec2d(60, 100), Vec2d(190, 60)});
            CHECK(too_tall(print) == facing);
        }
    }
}

TEST_CASE("Sequential clearance keeps the order of short objects", "[SequentialClearance]")
{
    Print print;
    Model model;
    auto  config = sequential_config();
    init_print({box(40, 40, 10), box(40, 40, 12)}, print, model, config);
    place(print, model, config, {Vec2d(60, 125), Vec2d(180, 135)});
    REQUIRE(Print::sequential_print_clearance_valid(print).string.empty());
    require_order(print, 0, 1);
}

TEST_CASE("Sequential clearance keeps conservative support constraints", "[SequentialClearance]")
{
    for (auto support : {stNormalAuto, stTreeAuto}) {
        for (bool neighbour_needs_last : {false, true}) {
            CAPTURE(support, neighbour_needs_last);
            Print print;
            Model model;
            auto  config = sequential_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(support));
            config.set_key_value("bridge_no_support", new ConfigOptionBool(false));
            config.set_key_value("max_bridge_length", new ConfigOptionFloat(0));
            config.set_key_value("support_expansion", new ConfigOptionFloat(5));
            auto shape = box(40, 30, 2);
            auto stem  = box(8, 2, 60);
            stem.translate(16.f, 28.f, 0.f);
            shape.merge(stem);
            auto cap = box(20, 8, 2);
            cap.translate(10.f, 22.f, 58.f);
            shape.merge(cap);
            init_print({shape, box(40, 40, neighbour_needs_last ? 150 : 10)}, print, model, config);
            place(print, model, config, {Vec2d(60, 100), Vec2d(190, 44.85)});
            CHECK(too_tall(print) == neighbour_needs_last);
            require_order(print, neighbour_needs_last ? 0 : 1, neighbour_needs_last ? 1 : 0);
            if (support == stNormalAuto && !neighbour_needs_last) {
                print.process();
                double      min_y = 1e9;
                const auto* obj   = print.objects().front();
                for (auto* layer : obj->support_layers()) {
                    if (layer->print_z <= HEIGHT_TO_ROD)
                        continue;
                    for (const auto& line : layer->support_fills.as_polylines())
                        for (const auto& point : line.points)
                            min_y = std::min(min_y, unscaled<double>(point.y() + obj->instances().front().shift.y()));
                }
                REQUIRE(min_y < 121.); // Model starts at Y=122; generated support enters the neighbour's sweep.
                REQUIRE_FALSE(too_tall(print));
                require_order(print, 1, 0);
            }
        }
    }
}

TEST_CASE("Sequential clearance retains the known sunk-object false positive", "[SequentialClearance]")
{
    // Remaining limitation: local height claims the last slot even when sunk below the rod.
    Print print;
    Model model;
    auto  config = sequential_config();
    init_print({box(40, 40, 150), box(40, 40, 150)}, print, model, config);
    model.objects[0]->instances.front()->set_offset(Vec3d(190, 130, 0));
    model.objects[1]->instances.front()->set_offset(Vec3d(60, 130, -130));
    print.apply(model, config);
    REQUIRE(too_tall(print));
}

TEST_CASE("Sequential clearance rejects thin fins that produce toolpaths", "[SequentialClearance]")
{
    Print print;
    Model model;
    auto  config = sequential_config();
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Arachne));
    config.set_key_value("min_feature_size", new ConfigOptionPercent(0));
    config.set_key_value("min_bead_width", new ConfigOptionPercent(25));
    init_print({slab_with_fin(false, 0.15), slab_with_fin(true, 0.15)}, print, model, config);
    place(print, model, config, {Vec2d(60, 100), Vec2d(190, 60)});
    REQUIRE(too_tall(print));
    print.process(); // Inspect material despite validation intentionally rejecting this plate.
    size_t above_rod = 0;
    for (auto* object : print.objects())
        for (auto* layer : object->layers())
            if (layer->print_z > HEIGHT_TO_ROD)
                for (auto* region : layer->regions())
                    above_rod += !region->perimeters.empty();
    REQUIRE(above_rod > 0);
}

TEST_CASE("Sequential clearance documents the expanded support fallback", "[SequentialClearance][SupportClearanceP2]")
{
    for (double gap : {45., 75.}) {
        for (SupportType type : {stNormalAuto, stNormal, stTreeAuto, stTree}) {
            for (bool enabled : {false, true}) {
                CAPTURE(gap, type, enabled);
                Print print;
                Model model;
                auto  config = sequential_config();
                config.set_key_value("enable_support", new ConfigOptionBool(enabled));
                config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
                config.set_key_value("raft_layers", new ConfigOptionInt(0));
                config.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
                init_print({box(40, 20, 35), box(40, 20, 35)}, print, model, config);
                place(print, model, config, {Vec2d(60, 100), Vec2d(190, 120 + gap)});
                // A known P2 restriction: both expanded Y footprints are used for possible support.
                const bool rejected = enabled && gap == 45.;
                const auto before   = Print::sequential_print_clearance_valid(print);
                CHECK(before.string.empty() == !rejected);
                CHECK((before.string.find("too tall") != std::string::npos) == rejected);
                print.process(); // Inspect generated support without exporting this potentially rejected plate.
                size_t support_layers = 0;
                for (const PrintObject* object : print.objects())
                    support_layers += object->support_layers().size();
                CHECK(support_layers == 0);
                // Empty generated support must not become a general validation bypass.
                const auto after = Print::sequential_print_clearance_valid(print);
                CHECK(after.string.empty() == !rejected);
                CHECK((after.string.find("too tall") != std::string::npos) == rejected);
                require_order(print, 0, 1);
            }
        }
    }
}
