#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/nowide/cstdio.hpp>
#include <boost/filesystem.hpp>

#include "test_data.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Model construction", "[Model]") {
    GIVEN("A Slic3r Model") {
		Slic3r::Model model;
        Slic3r::TriangleMesh sample_mesh = Slic3r::make_cube(20,20,20);
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        Slic3r::Print print;

        WHEN("Model object is added") {
            Slic3r::ModelObject *model_object = model.add_object();
            THEN("Model object list == 1") {
                REQUIRE(model.objects.size() == 1);
            }
            model_object->add_volume(sample_mesh);
            THEN("Model volume list == 1") {
                REQUIRE(model_object->volumes.size() == 1);
            }
            THEN("Model volume is a part") {
                REQUIRE(model_object->volumes.front()->is_model_part());
            }
            THEN("Mesh is equivalent to input mesh.") {
                REQUIRE(! sample_mesh.its.vertices.empty());
				const std::vector<Vec3f>& mesh_vertices = model_object->volumes.front()->mesh().its.vertices;
				Vec3f mesh_offset = model_object->volumes.front()->source.mesh_offset.cast<float>();
				for (size_t i = 0; i < sample_mesh.its.vertices.size(); ++ i) {
					const Vec3f &p1 = sample_mesh.its.vertices[i];
					const Vec3f  p2 = mesh_vertices[i] + mesh_offset;
					REQUIRE((p2 - p1).norm() < EPSILON);
				}
            }
            model_object->add_instance();
            arrange_objects(model, InfiniteBed{scaled(Vec2d(100, 100))}, ArrangeParams{scaled(min_object_distance(config))});
			model_object->ensure_on_bed();
			print.auto_assign_extruders(model_object);
			THEN("Print works?") {
				print.set_status_silent();
				print.apply(model, config);
				print.process();
				// A bare unique_path() made GCode::do_export() call create_directory() on the
				// empty parent path, which throws on Windows. Export into the harness scratch dir.
				boost::filesystem::path temp = Slic3r::Test::scratch_path();
                print.export_gcode(temp.string(), nullptr, nullptr);
                REQUIRE(boost::filesystem::exists(temp));
				REQUIRE(boost::filesystem::is_regular_file(temp));
				REQUIRE(boost::filesystem::file_size(temp) > 0);
				boost::nowide::remove(temp.string().c_str());
			}
        }
    }
}

TEST_CASE("Precise Seam volume types round-trip through type_to/from_string", "[Model][PreciseSeam]")
{
    const ModelVolumeType types[] = {
        ModelVolumeType::PRECISE_SEAM_CENTER, ModelVolumeType::PRECISE_SEAM_LEFT,
        ModelVolumeType::PRECISE_SEAM_RIGHT,  ModelVolumeType::PRECISE_SEAM_ENFORCED,
        ModelVolumeType::PRECISE_SEAM_BLOCKED, ModelVolumeType::PRECISE_SEAM_NEUTRAL
    };
    for (ModelVolumeType t : types) {
        CAPTURE(int(t));
        CHECK(ModelVolume::type_from_string(ModelVolume::type_to_string(t)) == t);
        CHECK(is_precise_seam(t));
    }
    CHECK(is_precise_seam_strong(ModelVolumeType::PRECISE_SEAM_CENTER));
    CHECK(is_precise_seam_strong(ModelVolumeType::PRECISE_SEAM_LEFT));
    CHECK(is_precise_seam_strong(ModelVolumeType::PRECISE_SEAM_RIGHT));
    CHECK_FALSE(is_precise_seam_strong(ModelVolumeType::PRECISE_SEAM_ENFORCED));
    CHECK(is_precise_seam_weak(ModelVolumeType::PRECISE_SEAM_ENFORCED));
    CHECK(is_precise_seam_weak(ModelVolumeType::PRECISE_SEAM_BLOCKED));
    CHECK(is_precise_seam_weak(ModelVolumeType::PRECISE_SEAM_NEUTRAL));
    CHECK_FALSE(is_precise_seam_weak(ModelVolumeType::PRECISE_SEAM_CENTER));
    CHECK_FALSE(is_precise_seam(ModelVolumeType::PARAMETER_MODIFIER));
    CHECK(ModelVolume::type_from_string("unknown_future_seam") == ModelVolumeType::MODEL_PART);
}

TEST_CASE("sort_volumes keeps strong Precise Seam helpers above weak ones", "[Model][PreciseSeam]")
{
    Model        model;
    ModelObject *object = model.add_object();
    auto *part = object->add_volume(make_cube(10, 10, 10));
    part->name = "part";
    auto *weak = object->add_volume(make_cube(2, 2, 2));
    weak->set_type(ModelVolumeType::PRECISE_SEAM_BLOCKED);
    weak->name = "weak";
    auto *strong = object->add_volume(make_cube(2, 2, 2));
    strong->set_type(ModelVolumeType::PRECISE_SEAM_CENTER);
    strong->name = "strong";
    auto *weak2 = object->add_volume(make_cube(2, 2, 2));
    weak2->set_type(ModelVolumeType::PRECISE_SEAM_NEUTRAL);
    weak2->name = "weak2";

    object->sort_volumes(true);
    REQUIRE(object->volumes.size() == 4);
    CHECK(object->volumes[0]->is_model_part());
    CHECK(object->volumes[1]->is_precise_seam_strong());
    CHECK(object->volumes[1]->name == "strong");
    CHECK(object->volumes[2]->is_precise_seam_weak());
    CHECK(object->volumes[2]->name == "weak");
    CHECK(object->volumes[3]->is_precise_seam_weak());
    CHECK(object->volumes[3]->name == "weak2");
}

TEST_CASE("get_extruders excludes Precise Seam helper volumes", "[Model][PreciseSeam]")
{
    Model        model;
    ModelObject *object = model.add_object();
    auto *part = object->add_volume(make_cube(10, 10, 10));
    part->config.set_key_value("extruder", new ConfigOptionInt(2));
    auto *helper = object->add_volume(make_cube(2, 2, 2));
    helper->set_type(ModelVolumeType::PRECISE_SEAM_LEFT);
    helper->config.set_key_value("extruder", new ConfigOptionInt(3));
    helper->config.set_key_value("wall_filament", new ConfigOptionInt(4));

    const std::vector<int> part_ids = part->get_extruders();
    REQUIRE_FALSE(part_ids.empty());
    CHECK(std::find(part_ids.begin(), part_ids.end(), 2) != part_ids.end());
    CHECK(helper->get_extruders().empty());
    CHECK(helper->is_precise_seam());
    CHECK_FALSE(helper->is_modifier());
}
