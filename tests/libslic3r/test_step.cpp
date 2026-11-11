#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstddef>
#include <string>

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {

// A hexagonal prism with a through hole, centred on the origin and exported as a STEP surface model:
// 9 loose faces without shared edges, every other face reversed and shifted by 1e-12 mm, so the seams
// on the x = 0, y = 0 and z = 0 planes differ in float32.
const char *const surface_model = "loose_faces.step";
// Volume of the solid the faces were taken from.
constexpr double surface_model_volume = 1157.666;
// A 10 mm box without its top face: one open shell whose faces share their edges.
const char *const open_surface = "open_box.step";

struct ImportedObject
{
    size_t volumes    = 0;
    int    open_edges = 0;
    double volume     = 0.;
};

ImportedObject import_step(const char *file_name, bool split_compound)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/test_step/" + file_name;
    Model             model;
    bool              is_cancel = false;
    REQUIRE(load_step(path.c_str(), &model, is_cancel, 0.003, 0.5, split_compound));
    REQUIRE(model.objects.size() == 1);
    ImportedObject object;
    for (const ModelVolume *model_volume : model.objects.front()->volumes) {
        ++object.volumes;
        object.open_edges += model_volume->mesh().stats().open_edges;
        object.volume += its_volume(model_volume->mesh().its);
    }
    return object;
}

} // namespace

TEST_CASE("A STEP surface model imports as a closed mesh", "[STEP]")
{
    const ImportedObject object = import_step(surface_model, false);
    CHECK(object.volumes == 1);
    CHECK(object.open_edges == 0);
    CHECK_THAT(object.volume, WithinRel(surface_model_volume, 1e-3));
}

TEST_CASE("A STEP surface model imports as a closed mesh with split compounds", "[STEP]")
{
    const ImportedObject object = import_step(surface_model, true);
    CHECK(object.volumes == 1);
    CHECK(object.open_edges == 0);
    CHECK_THAT(object.volume, WithinRel(surface_model_volume, 1e-3));
}

TEST_CASE("An open STEP surface imports unchanged with split compounds", "[STEP]")
{
    const ImportedObject object = import_step(open_surface, true);
    CHECK(object.volumes == 1);
    CHECK(object.open_edges == 4);
}
