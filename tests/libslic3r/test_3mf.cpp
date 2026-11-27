#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"

#include <boost/filesystem/operations.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
        	std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        	DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

// The loader centers a mesh on its first use and shares it with every later component that uses it, so each of those
// volumes has to apply the same centering shift before its component transform.
TEST_CASE("Components sharing an off-center mesh keep their placement", "[3mf]") {
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext substitutions{ ForwardCompatibilitySubstitutionRule::Disable };
    PlateDataPtrs             plates;
    std::vector<Preset*>      presets;
    bool                      is_bbl = false;
    Semver                    version;
    // One box mesh (x 10..12, y 0..4, z 0..1) used by four components: rotated 90 degrees about X (its first use),
    // rotated the same way and moved 20 mm along X, scaled 2x along X and moved 40 mm, moved 80 mm.
    const std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/shared_mesh_components.3mf";
    REQUIRE(load_bbs_3mf(path.c_str(), &config, &substitutions, &model, &plates, &presets, &is_bbl, &version, nullptr, LoadStrategy::LoadModel));
    release_PlateData_list(plates);
    REQUIRE(model.objects.size() == 1);
    const ModelVolumePtrs &volumes = model.objects.front()->volumes;
    REQUIRE(volumes.size() == 4);

    // Where each component puts the box, in object coordinates.
    const std::vector<std::pair<Vec3d, Vec3d>> expected = {
        { { 10., -1., 0. }, { 12., 0., 4. } },
        { { 30., -1., 0. }, { 32., 0., 4. } },
        { { 60., 0., 0. }, { 64., 4., 1. } },
        { { 90., 0., 0. }, { 92., 4., 1. } },
    };
    for (size_t i = 0; i < volumes.size(); ++i) {
        INFO("volume " << i);
        REQUIRE(volumes[i]->mesh_ptr() == volumes.front()->mesh_ptr());
        const BoundingBoxf3 box = volumes[i]->mesh().transformed_bounding_box(volumes[i]->get_matrix());
        for (int axis = 0; axis < 3; ++axis) {
            INFO("axis " << axis);
            CHECK_THAT(box.min(axis), WithinAbs(expected[i].first(axis), 1e-6));
            CHECK_THAT(box.max(axis), WithinAbs(expected[i].second(axis), 1e-6));
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &model);
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects.front();
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled and sinking
            ModelInstance* instance = object->instances.front();
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
            Polygon hull_2d = object->convex_hull_2d(instance->get_transformation().get_matrix());

            // verify result
            Points result = {
                { -91501495, -15914144 },
                { 91501495, -15914144 },
                { 91501495, 13792823 },
                { 34846496, 14717717 },
                { -85501495, 13917981 },
                { -91501495, 13792823 }
            };

            // Allow 1um error due to floating point rounding.
            bool res = hull_2d.points.size() == result.size();
            if (res)
                for (size_t i = 0; i < result.size(); ++ i) {
                    const Point &p1 = result[i];
                    const Point &p2 = hull_2d.points[i];
                    if (std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1) {
                        res = false;
                        break;
                    }
                }
            THEN("2D convex hull should match with reference") {
                REQUIRE(res);
            }
        }
    }
}

