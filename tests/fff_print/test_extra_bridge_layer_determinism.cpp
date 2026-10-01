// The second bridge layers (enable_extra_bridge_layer) and the lightning anchor expansion must not
// depend on how TBB splits the layers between threads.
//
// All three rewrote a NEIGHBOUR layer's surfaces from inside a tbb::parallel_for over the layers:
//  - detect_surfaces_type(), second external bridge layer: iteration i rewrote layer i+1's slices
//    while iteration i+1 read them looking for stBottomBridge;
//  - bridge_over_infill(), second internal bridge layer: iteration i rewrote layer i+1's
//    fill_surfaces while iteration i+1 read them looking for stInternalBridge;
//  - bridge_over_infill(), lightning anchors: iteration i asked whether layer i-1 had lightning
//    sparse infill while iteration i-1 moved layer i-1's fill_surfaces out and rebuilt them.
// Two threads on adjacent layers read half-moved vectors, so the result changed from slice to slice
// (or crashed). These cases slice one plate with each setting on one thread and then repeatedly on
// all of them, in one process, and require the same G-code every time.
//
// Built on DynamicPrintConfig::full_print_config() rather than Slic3r::Test::init_print(), like
// test_tree_support_determinism.cpp, for the same reason (PrusaSlicer key names).

#include <catch2/catch.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <tbb/global_control.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

void add_box(ModelObject *object, double x0, double y0, double z0, double dx, double dy, double dz)
{
    TriangleMesh box = make_cube(dx, dy, dz);
    box.translate(float(x0), float(y0), float(z0));
    object->add_volume(box);
}

void place(ModelObject *object, double x, double y)
{
    object->add_instance()->set_offset(Vec3d(x, y, 0.));
    object->ensure_on_bed();
}

// Three objects, each putting bridges on many different layers so that a lot of adjacent layer
// pairs both have work to do - the races needed two workers on neighbouring layers at once.
void add_plate(Model &model)
{
    // A table: two legs and a 4 mm deck spanning 30 mm of air. The deck's first layer is an external
    // bridge, the layer above it the second external bridge layer.
    ModelObject *table = model.add_object();
    table->name = "table.stl";
    add_box(table, 0., 0., 0., 8., 20., 6.);
    add_box(table, 38., 0., 0., 8., 20., 6.);
    add_box(table, 0., 0., 6., 46., 20., 4.);
    place(table, 20., 20.);

    // An upside-down ziggurat: every 0.6 mm step reaches 2 mm further out, so every third layer has a
    // ring of external bridge with internal infill on top of it.
    ModelObject *funnel = model.add_object();
    funnel->name = "funnel.stl";
    for (int step = 0; step < 12; ++step) {
        const double half = 4. + 2. * step;
        add_box(funnel, -half, -half, 0.6 * step, 2. * half, 2. * half, 0.6 + (step == 11 ? 1.2 : 0.));
    }
    place(funnel, 110., 50.);

    // A ziggurat with sparse infill: every 1.6 mm step is 4 mm narrower, so the top shells of every
    // step's ledge sit on sparse infill and start with an internal bridge.
    ModelObject *ziggurat = model.add_object();
    ziggurat->name = "ziggurat.stl";
    for (int step = 0; step < 8; ++step) {
        const double half = 34. - 4. * step;
        add_box(ziggurat, -half, -half, 1.6 * step, 2. * half, 2. * half, 1.6);
    }
    place(ziggurat, 100., 150.);
}

DynamicPrintConfig plate_config(const char *extra_bridge_layer, const char *sparse_pattern)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_extra_bridge_layer", extra_bridge_layer},
        {"sparse_infill_pattern", sparse_pattern},
        {"sparse_infill_density", "15%"},
        {"wall_loops", "1"},
        {"top_shell_layers", "4"},
        {"bottom_shell_layers", "3"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"enable_support", "0"},
        // Classic walls: a difference here can only come from the surface classification.
        {"wall_generator", "classic"},
        // full_print_config() has relative E, which validate() refuses without a per-layer reset.
        {"layer_change_gcode", "G92 E0"},
    });
    return config;
}

std::string slice_plate(const DynamicPrintConfig &config, size_t max_threads = 0)
{
    std::unique_ptr<tbb::global_control> cap;
    if (max_threads > 0)
        cap = std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, max_threads);

    Print print;
    Model model;
    add_plate(model);
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    return Test::gcode(print);
}

// The exported G-code minus what legitimately differs between two slices of the same plate: the
// header's generation timestamp and the object label ids, which come from a process-wide counter.
std::vector<std::string> comparable_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line))
        if (line.find("generated by") == std::string::npos && line.find("label id") == std::string::npos)
            out.push_back(line);
    return out;
}

// First line where two slices differ, or -1. Comparing line by line keeps a failure readable.
long long first_difference(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return (long long) i;
    return a.size() == b.size() ? -1 : (long long) n;
}

void require_same(const std::vector<std::string> &reference, const std::vector<std::string> &again, const std::string &what)
{
    const long long diff = first_difference(reference, again);
    INFO(what << ", first difference at line " << diff << ": \""
              << (diff >= 0 && diff < (long long) reference.size() ? reference[size_t(diff)] : std::string("<end>")) << "\" vs \""
              << (diff >= 0 && diff < (long long) again.size() ? again[size_t(diff)] : std::string("<end>")) << "\"");
    CHECK(diff == -1);
}

// One thread first (the serial result), then the same plate again and again on every core.
void check_repeatable(const char *extra_bridge_layer, const char *sparse_pattern)
{
    INFO("enable_extra_bridge_layer = " << extra_bridge_layer << ", sparse_infill_pattern = " << sparse_pattern);
    const DynamicPrintConfig       config    = plate_config(extra_bridge_layer, sparse_pattern);
    const std::vector<std::string> reference = comparable_lines(slice_plate(config, 1));
    REQUIRE(reference.size() > 1000);

    // The case only means something if the setting reaches the G-code at all.
    if (std::string(extra_bridge_layer) != "disabled") {
        const std::vector<std::string> without = comparable_lines(slice_plate(plate_config("disabled", sparse_pattern), 1));
        REQUIRE(first_difference(reference, without) != -1);
    }

    for (int repeat = 1; repeat <= 4; ++repeat)
        require_same(reference, comparable_lines(slice_plate(config)), "parallel slice " + std::to_string(repeat));
}

} // namespace

TEST_CASE("second external bridge layer is identical on one thread and on many", "[ExtraBridgeLayerDeterminism]")
{
    check_repeatable("external_bridge_only", "grid");
}

TEST_CASE("second internal bridge layer is identical on one thread and on many", "[ExtraBridgeLayerDeterminism]")
{
    check_repeatable("internal_bridge_only", "grid");
}

TEST_CASE("both second bridge layers are identical on one thread and on many", "[ExtraBridgeLayerDeterminism]")
{
    check_repeatable("apply_to_all", "grid");
}

TEST_CASE("lightning anchor expansion is identical on one thread and on many", "[ExtraBridgeLayerDeterminism]")
{
    check_repeatable("disabled", "lightning");
    check_repeatable("apply_to_all", "lightning");
}
