// Orca #14103: a printer switch to one with fewer filaments left per-part filament assignments that
// no longer exist, and later readers indexed per-filament vectors with them.
// ModelVolume::update_extruder_count (called by Plater::on_filaments_change with the new total,
// mixed filaments included) now drops such an assignment so the part follows its object again.

#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

ModelVolume *add_part(ModelObject &object, int extruder)
{
    ModelVolume *volume = object.add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    if (extruder >= 0)
        volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    return volume;
}

} // namespace

TEST_CASE("a part assigned to a filament the new printer does not have follows its object", "[StaleFilamentIndex][Model]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->config.set_key_value("extruder", new ConfigOptionInt(1));

    ModelVolume *stale   = add_part(*object, 5);  // filament 5 on a 4-filament printer
    ModelVolume *last    = add_part(*object, 4);  // still valid
    ModelVolume *inherit = add_part(*object, 0);  // "default": follows the object
    ModelVolume *unset   = add_part(*object, -1); // no own assignment

    for (ModelVolume *v : object->volumes)
        v->update_extruder_count(4);

    CHECK_FALSE(stale->config.has("extruder"));
    CHECK(stale->extruder_id() == 1); // falls back to the object's filament
    REQUIRE(last->config.has("extruder"));
    CHECK(last->config.extruder() == 4);
    REQUIRE(inherit->config.has("extruder"));
    CHECK(inherit->config.extruder() == 0);
    CHECK_FALSE(unset->config.has("extruder"));
}

TEST_CASE("a part on a mixed filament keeps it while the total still covers it", "[StaleFilamentIndex][Model]")
{
    // The caller passes physical + mixed (virtual) filaments: 4 physical + 2 mixed = 6.
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *mixed  = add_part(*object, 6);

    mixed->update_extruder_count(6);
    REQUIRE(mixed->config.has("extruder"));
    CHECK(mixed->config.extruder() == 6);

    // The mixed filament is gone (total back to 4): the stale id is dropped.
    mixed->update_extruder_count(4);
    CHECK_FALSE(mixed->config.has("extruder"));
}

TEST_CASE("growing the filament count leaves every assignment alone", "[StaleFilamentIndex][Model]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->config.set_key_value("extruder", new ConfigOptionInt(3));
    ModelVolume *part = add_part(*object, 2);

    part->update_extruder_count(8);
    REQUIRE(part->config.has("extruder"));
    CHECK(part->config.extruder() == 2);
    CHECK(object->config.extruder() == 3);
}
