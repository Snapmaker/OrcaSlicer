#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <fstream>
#include <functional>

using namespace Slic3r;

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
            // Unlike upstream PrusaSlicer, this fork's convex_hull_2d projects the
            // entire mesh: it does not clip a sinking object at the print bed, so the
            // hull extends to the full rotated extents of the model.
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
                for (const Point &p : hull_2d.points)
                    UNSCOPED_INFO("actual hull point: { " << p.x() << ", " << p.y() << " }");
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("Mirrored instance transforms survive 3mf round trips", "[3mf][Regression]") {
    const bool bbs_format = GENERATE(false, true);

    GIVEN("an instance with a valid mirrored transform") {
        Model model;
        const std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        Transform3d mirrored = Transform3d::Identity();
        mirrored.linear() <<
             4.4408921e-16,  0.819152044,  0.573576436,
             1.0,           -4.4408921e-16, -1.11022302e-16,
            -5.55111512e-17, -0.573576436,  0.819152044;
        mirrored.translation() = Vec3d(700.41477, -169.930939, 63.392571);
        REQUIRE_THAT(mirrored.linear().determinant(), Catch::Matchers::WithinAbs(-1.0, 1e-8));
        model.objects.front()->instances.front()->set_transformation(Geometry::Transformation(mirrored));
        const TriangleMesh expected_mesh = model.mesh();

        WHEN("the model is stored and loaded") {
            // The bbs exporter writes scratch files through Model::get_backup_path(), which is
            // rooted at temporary_dir(). Headless tests must set that, or the path resolves to
            // the root of the current drive.
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());
            const std::string test_file = (tmp_root / (bbs_format ? "mirrored_bbs.3mf" : "mirrored_std.3mf")).string();

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            if (bbs_format) {
                StoreParams store_params;
                store_params.path     = test_file.c_str();
                store_params.model    = &model;
                store_params.config   = &config;
                store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
                REQUIRE(store_bbs_3mf(store_params));
            } else {
                REQUIRE(store_3mf(test_file.c_str(), &model, &config, false));
            }

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs dst_plates;
            std::vector<Preset*> project_presets;
            ScopeGuard cleanup([&dst_plates, &project_presets, &test_file]() {
                release_PlateData_list(dst_plates);
                for (Preset *preset : project_presets)
                    delete preset;
                boost::filesystem::remove(test_file);
            });
            if (bbs_format) {
                bool is_bbl_3mf = false;
                Semver file_version;
                REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                     &project_presets, &is_bbl_3mf, &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                     LoadStrategy::Silence));
            } else {
                REQUIRE(load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false));
            }

            THEN("the mirrored transform and world geometry are preserved") {
                REQUIRE(dst_model.objects.size() == 1);
                REQUIRE(dst_model.objects.front()->instances.size() == 1);
                const Transform3d &loaded = dst_model.objects.front()->instances.front()->get_matrix();
                REQUIRE(loaded.linear().determinant() < 0.0);

                const TriangleMesh loaded_mesh = dst_model.mesh();
                REQUIRE(loaded_mesh.its.vertices.size() == expected_mesh.its.vertices.size());
                for (size_t i = 0; i < loaded_mesh.its.vertices.size(); ++i)
                    REQUIRE(loaded_mesh.its.vertices[i].isApprox(expected_mesh.its.vertices[i], 1e-5f));
            }
        }
    }
}


// True if the loader would drop or rename this key on the way in: PrintConfigDef::handle_legacy()
// clears obsolete keys (extruder_type and silent_mode are in that set although print_config_def
// still defines them), and a round trip cannot expect those back.
static bool retired_on_load(const std::string &key)
{
    t_config_option_key legacy_key = key;
    std::string         value;
    PrintConfigDef::handle_legacy(legacy_key, value);
    return legacy_key != key;
}

// A headless project save. store_bbs_3mf serializes the whole project config with
// ConfigBase::save_to_json (_add_project_config_file_to_archive); a config built from the static
// defaults rather than from a PresetBundle used to crash there on its first coEnums member, whose
// keys map was never set. The application never hit it because its project config comes from a
// PresetBundle, but anything headless that stores a project did.
SCENARIO("A project built from the static default config survives a project 3MF round trip", "[3mf][Config]") {
    GIVEN("a one-part model and DynamicPrintConfig::full_print_config()") {
        Model src_model;
        ModelObject *src_object = src_model.add_object();
        src_object->name = "cube";
        src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
        src_object->add_instance();
        src_object->ensure_on_bed();
        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

        WHEN("the project is stored and loaded again") {
            // The exporter writes its scratch config through Model::get_backup_path(), which is rooted at
            // temporary_dir(). The application sets that at startup; a test that exercises the project
            // writer has to as well, or the path resolves to the root of the current drive.
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());
            const std::string test_file = (tmp_root / "full_print_config_project.3mf").string();

            StoreParams store_params;
            store_params.path     = test_file.c_str();
            store_params.model    = &src_model;
            store_params.config   = &store_config;
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            const bool stored = store_bbs_3mf(store_params);

            Model                     dst_model;
            DynamicPrintConfig        dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs             plate_data;
            std::vector<Preset*>      project_presets;
            bool                      is_bbl_3mf = false;
            Semver                    file_version;
            const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                                       &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                                       nullptr,
                                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                       LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
            release_PlateData_list(plate_data);
            boost::filesystem::remove(test_file);

            THEN("store and load both succeed") {
                REQUIRE(stored);
                REQUIRE(loaded);
            }

            THEN("the coEnums options come back by name, with the values they were stored with") {
                REQUIRE(loaded);
                for (const std::string &key : store_config.keys()) {
                    const ConfigOption *opt = store_config.option(key);
                    if (opt->type() != coEnums || retired_on_load(key))
                        continue;
                    INFO("option " << key);
                    REQUIRE(dst_config.has(key));
                    CHECK(*dst_config.option(key) == *opt);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Ultra (support groups) - Stage 2 / T5: per-part support data through a project 3MF.
//
// The project format is bbs_3mf, NOT the legacy writer the scenarios above use: store_3mf()
// emits a plain PrusaSlicer 3MF and never writes a volume's ModelConfig, so a round trip through
// it could not say anything about group data. store_bbs_3mf()/load_bbs_3mf() is the pair the
// application uses when the user saves a project and opens it again.
//
// Two things an earlier attempt tripped over, fixed here rather than worked around:
//   * StoreParams::config is dereferenced unconditionally (_add_model_config_file_to_archive
//     takes a const DynamicPrintConfig&), so it has to be a real config and not nullptr;
//   * without SaveStrategy::Silence the exporter writes an origin.txt under the model's backup
//     path, which a bare Model has not got.
//
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 2 "Gate" item 1.
// ---------------------------------------------------------------------------------------------
SCENARIO("Support group data survives a project 3MF round trip", "[3mf][SupportGroups]") {
    GIVEN("a two-part object whose parts carry a support group and part-level support values") {
        // A pillar with a plate floating above it: an overhang, so the corpus case built from
        // this same fixture really does generate support.
        Model src_model;
        ModelObject* src_object = src_model.add_object();
        src_object->name = "two_part_groups";
        src_object->add_volume(make_cube(10., 10., 20.))->name = "pillar";
        src_object->add_volume(make_cube(30., 10.,  2.))->name = "plate";
        src_object->volumes[1]->set_offset({ -10., 0., 20. });
        src_object->add_instance();
        src_object->ensure_on_bed();

        // Part A stays in the default group but carries one override, part B is group "B" with
        // the four values a support set writes. Both tiers are represented: A keys
        // (interface geometry and filament) and a B key (support_top_z_distance).
        ModelVolume* a = src_object->volumes[0];
        ModelVolume* b = src_object->volumes[1];
        a->config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(1));
        b->config.set_key_value("support_group",                new ConfigOptionString("B"));
        b->config.set_key_value("support_interface_top_layers",  new ConfigOptionInt(5));
        b->config.set_key_value("support_interface_spacing",     new ConfigOptionFloat(0.15));
        b->config.set_key_value("support_interface_filament",    new ConfigOptionInt(2));
        b->config.set_key_value("support_top_z_distance",        new ConfigOptionFloat(0.));

        WHEN("the project is stored and loaded again") {
            const std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/support_groups.3mf";
            DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

            // The exporter writes its scratch config through Model::get_backup_path(), which is
            // rooted at temporary_dir(). The application sets that at startup; a test that
            // exercises the project writer has to as well, or the path resolves to the root of
            // the current drive.
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());

            // A libslic3r bug this test must not trip over, and worth recording:
            // ConfigOptionEnumsGenericTempl::set() copies only the values, never keys_map, so a
            // coEnums member of a STATIC config class - FullPrintConfig, i.e. what
            // full_print_config() is built from - keeps keys_map == nullptr, and
            // serialize_single_value() dereferences it with no check. The exporter serialises the
            // whole config to JSON (_add_project_config_file_to_archive), so it crashes there. A
            // project config that came from a PresetBundle has the maps, which is why the
            // application never hits this.
            // Rebuild those options straight from print_config_def instead: DynamicConfig's
            // create path clones the def's default value, and THAT clone carries keys_map.
            // Erasing them is not enough - a project config with no nozzle_volume_type crashes
            // the CLI on load.
            for (const std::string& key : store_config.keys())
                if (const ConfigOption* opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
                    store_config.erase(key);
                    store_config.option(key, true);
                }

            StoreParams store_params;
            store_params.path     = test_file.c_str();
            store_params.model    = &src_model;
            store_params.config   = &store_config;
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            const bool stored = store_bbs_3mf(store_params);

            Model                 dst_model;
            DynamicPrintConfig    dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs         plate_data;
            std::vector<Preset*>  project_presets;
            bool                  is_bbl_3mf = false;
            Semver                file_version;
            const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                                       &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                                       nullptr,
                                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                       LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
            release_PlateData_list(plate_data);
            boost::filesystem::remove(test_file);

            THEN("store and load both succeed") {
                REQUIRE(stored);
                REQUIRE(loaded);
            }

            THEN("both volumes come back with byte-identical configs") {
                REQUIRE(dst_model.objects.size() == 1);
                REQUIRE(dst_model.objects.front()->volumes.size() == src_object->volumes.size());
                for (size_t i = 0; i < src_object->volumes.size(); ++ i) {
                    const DynamicPrintConfig& want = src_object->volumes[i]->config.get();
                    const DynamicPrintConfig& have = dst_model.objects.front()->volumes[i]->config.get();
                    INFO("volume " << i);
                    REQUIRE(have.keys() == want.keys());
                    for (const std::string& key : want.keys()) {
                        INFO("key " << key);
                        REQUIRE(*have.option(key) == *want.option(key));
                    }
                }
            }

            THEN("support_group and the tier values are intact, and nothing leaked onto part A") {
                REQUIRE(dst_model.objects.size() == 1);
                const ModelObject* dst_object = dst_model.objects.front();
                REQUIRE(dst_object->volumes.size() == 2);
                const DynamicPrintConfig& da = dst_object->volumes[0]->config.get();
                const DynamicPrintConfig& db = dst_object->volumes[1]->config.get();

                REQUIRE(! da.has("support_group"));
                REQUIRE(da.opt_int("support_interface_bottom_layers") == 1);

                REQUIRE(db.has("support_group"));
                REQUIRE(db.opt_string("support_group") == "B");
                REQUIRE(db.opt_int("support_interface_top_layers") == 5);
                REQUIRE(db.opt_float("support_interface_spacing") == Approx(0.15));
                REQUIRE(db.opt_int("support_interface_filament") == 2);
                REQUIRE(db.opt_float("support_top_z_distance") == Approx(0.));
            }

            THEN("the geometry is unchanged") {
                // The other half of the stock-Orca degradation claim: dropping the key costs the
                // reader nothing but the key.
                REQUIRE(dst_model.mesh().its.vertices.size() == src_model.mesh().its.vertices.size());
                REQUIRE(dst_model.mesh().its.indices.size() == src_model.mesh().its.indices.size());
            }
        }
    }
}

// A build that does not know support_group - stock Orca or Bambu Studio - drops it silently on
// load rather than throwing UnknownOptionException: PrintConfigDef::handle_legacy() clears an
// opt_key it does not recognise and returns, and ConfigBase::set_deserialize() then skips it.
// This is the mechanism behind "a project with groups still opens everywhere", asserted here
// against a key this build genuinely does not have. No stock build was run.
SCENARIO("An unknown support key degrades gracefully the way stock Orca would", "[3mf][SupportGroups]") {
    GIVEN("a key this build does not define") {
        t_config_option_key key   = "support_group_from_a_newer_build";
        std::string         value = "B";
        WHEN("handle_legacy sees it") {
            PrintConfigDef::handle_legacy(key, value);
            THEN("the key is cleared, which is how the loader drops it without failing") {
                REQUIRE(key.empty());
            }
        }
    }
    GIVEN("support_group itself, which THIS build does define") {
        t_config_option_key key   = "support_group";
        std::string         value = "B";
        WHEN("handle_legacy sees it") {
            PrintConfigDef::handle_legacy(key, value);
            THEN("it survives untouched") {
                REQUIRE(key == "support_group");
                REQUIRE(value == "B");
                REQUIRE(print_config_def.has("support_group"));
            }
        }
    }
}

// The sliced-plate half of Metadata/slice_info.config. A Bambu H2C rejects a plate 3MF that
// carries only the pre-2.x schema (HMS 05004046), because on that machine the extruder alone does
// not identify a hotend: extruder 2 holds up to six nozzles. The schema that resolves it is the
// per-<filament> group_id / nozzle_diameter / volume_type and the <nozzle> table, plus
// nozzle_volume_type written as one value PER EXTRUDER rather than a single scalar.
// This asserts the writer and the importer agree on all of that, over a real store/load cycle.
// No printer was involved; this proves the file's shape, not the firmware's acceptance.
SCENARIO("A sliced plate carries the multi-nozzle slice_info schema through a 3MF round trip", "[3mf][H2C]") {
    GIVEN("a plate sliced on a two-extruder machine with each filament on its own nozzle") {
        Model src_model;
        ModelObject* src_object = src_model.add_object();
        src_object->name = "h2c_cube";
        src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
        src_object->add_instance();
        src_object->ensure_on_bed();

        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
        // Same libslic3r quirk the support-group case documents above: a coEnums option that came
        // from a STATIC config class has no keys_map, and the project writer serialises it.
        for (const std::string& key : store_config.keys())
            if (const ConfigOption* opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
                store_config.erase(key);
                store_config.option(key, true);
            }
        store_config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4 }));
        store_config.set_key_value("filament_colour", new ConfigOptionStrings({ "#000000", "#FFFFFF" }));
        store_config.set_key_value("filament_map",    new ConfigOptionInts({ 1, 2 }));
        store_config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values       = { 0, 0 };
        store_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values  = { 0, 0 };

        // Two logical nozzles, one per extruder; filament 0 on nozzle 0, filament 1 on nozzle 1.
        std::vector<MultiNozzleUtils::NozzleInfo> nozzle_list(2);
        nozzle_list[0].diameter = "0.4"; nozzle_list[0].volume_type = nvtStandard; nozzle_list[0].extruder_id = 0; nozzle_list[0].group_id = 0;
        nozzle_list[1].diameter = "0.4"; nozzle_list[1].volume_type = nvtStandard; nozzle_list[1].extruder_id = 1; nozzle_list[1].group_id = 1;
        auto group_result = MultiNozzleUtils::LayeredNozzleGroupResult::create({ 0, 1 }, nozzle_list, { 0u, 1u });
        REQUIRE(group_result);

        PlateData* plate = new PlateData();
        plate->plate_index      = 0;
        plate->is_sliced_valid  = true;
        plate->printer_model_id = "O1C2";
        plate->nozzle_diameters = "0.4,0.4";
        plate->gcode_prediction = "1000";
        plate->gcode_weight     = "12.34";
        plate->first_layer_time = "42";
        plate->filament_maps    = { 1, 2 };
        plate->nozzle_group_result = *group_result;
        plate->objects_and_instances.emplace_back(0, 0);
        for (int fid = 0; fid < 2; ++ fid) {
            FilamentInfo info;
            info.id                 = fid;
            info.type               = "PLA";
            info.color              = fid == 0 ? "#000000" : "#FFFFFF";
            info.filament_id        = fid == 0 ? "GFA00" : "GFA01";
            info.used_m             = 1.5f + fid;
            info.used_g             = 4.5f + fid;
            info.group_id           = { fid };
            info.nozzle_diameter    = 0.4;
            info.nozzle_volume_type = get_nozzle_volume_type_string(nvtStandard);
            info.used_for_object    = fid == 0;
            info.used_for_support   = fid == 1;
            plate->slice_filaments_info.push_back(info);
        }

        WHEN("the plate is stored into a 3MF and read back") {
            const std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/h2c_slice_info.3mf";
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());

            StoreParams store_params;
            store_params.path            = test_file.c_str();
            store_params.model           = &src_model;
            store_params.config          = &store_config;
            store_params.plate_data_list = { plate };
            store_params.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            const bool stored = store_bbs_3mf(store_params);

            Model                     dst_model;
            DynamicPrintConfig        dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs             plate_data;
            std::vector<Preset*>      project_presets;
            bool                      is_bbl_3mf = false;
            Semver                    file_version;
            const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                                       &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                                       nullptr,
                                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                       LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);

            THEN("store and load both succeed and one plate comes back") {
                REQUIRE(stored);
                REQUIRE(loaded);
                REQUIRE(plate_data.size() == 1);
            }

            THEN("nozzle_volume_type is a per-extruder list, not the old single scalar") {
                REQUIRE(plate_data.size() == 1);
                // Two extruders, so two values. The old writer emitted "0".
                REQUIRE(plate_data.front()->nozzle_volume_types == "0 0");
            }

            THEN("the <nozzle> table survives, so a filament's group_id resolves to a hotend") {
                REQUIRE(plate_data.size() == 1);
                const auto& nozzles = plate_data.front()->nozzles_info;
                REQUIRE(nozzles.size() == 2);
                REQUIRE(nozzles[0].group_id == 0);
                REQUIRE(nozzles[0].extruder_id == 0);
                REQUIRE(nozzles[0].diameter == "0.4");
                REQUIRE(nozzles[0].volume_type == nvtStandard);
                REQUIRE(nozzles[1].group_id == 1);
                REQUIRE(nozzles[1].extruder_id == 1);
            }

            THEN("every per-filament multi-nozzle attribute round trips") {
                REQUIRE(plate_data.size() == 1);
                const auto& filaments = plate_data.front()->slice_filaments_info;
                REQUIRE(filaments.size() == 2);
                for (size_t i = 0; i < filaments.size(); ++ i) {
                    INFO("filament " << i);
                    REQUIRE(filaments[i].id == int(i));
                    REQUIRE(filaments[i].group_id == std::vector<int>{ int(i) });
                    REQUIRE(filaments[i].nozzle_diameter == Approx(0.4));
                    REQUIRE(filaments[i].nozzle_volume_type == get_nozzle_volume_type_string(nvtStandard));
                }
                REQUIRE(filaments[0].used_for_object);
                REQUIRE(!filaments[0].used_for_support);
                REQUIRE(!filaments[1].used_for_object);
                REQUIRE(filaments[1].used_for_support);
            }

            THEN("the plate keeps the values the printer keys off") {
                REQUIRE(plate_data.size() == 1);
                REQUIRE(plate_data.front()->printer_model_id == "O1C2");
                REQUIRE(plate_data.front()->nozzle_diameters == "0.4,0.4");
                REQUIRE(plate_data.front()->first_layer_time == "42");
            }

            release_PlateData_list(plate_data);
            boost::filesystem::remove(test_file);
        }
        delete plate;
    }
}

// Deft: Metadata/model_settings.config's <object> block order came from a direct range-for over
// _BBS_3MF_Exporter::ObjectToObjectDataMap, a std::map<ModelObject const*, ObjectData> - so the
// order was the heap-pointer order of the ModelObjects, which two exports of an equivalent model
// need not agree on (ASLR, allocator layout, whatever ran before in the process). Everything else
// about the file - every byte inside one <object> block - was already deterministic; only the
// relative order of the blocks moved. The fix walks a vector sorted by ObjectData::object_id (the
// id= attribute, assigned in model.objects order by a per-export counter, not a global one) instead
// of the map directly. This builds two independently-allocated Model instances with several
// objects each, so their ModelObjects do not share addresses, exports each, and requires the raw
// Metadata/model_settings.config bytes to match byte for byte.
static std::string extract_zip_entry(const std::string& zip_path, const std::string& entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t size = 0;
    void*  data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char*>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

static void build_multi_object_model(Model& model)
{
    // Distinct names/sizes/volume counts per object, several objects, so a pointer-order
    // permutation would be visible: each object's serialized block differs from the others'.
    struct Spec { const char* name; double a, b, c; int extra_volumes; };
    static const Spec specs[] = {
        { "alpha",   10., 10., 10., 0 },
        { "bravo",   12.,  8., 14., 1 },
        { "charlie",  6., 20.,  9., 0 },
        { "delta",   15., 15.,  5., 2 },
    };
    for (const Spec& s : specs) {
        ModelObject* obj = model.add_object();
        obj->name = s.name;
        obj->add_volume(make_cube(s.a, s.b, s.c))->name = std::string(s.name) + "_body";
        for (int i = 0; i < s.extra_volumes; ++i)
            obj->add_volume(make_cube(2., 2., 2.))->name = std::string(s.name) + "_extra";
        obj->add_instance();
        obj->ensure_on_bed();
    }
}

SCENARIO("model_settings.config's object order does not depend on where the Model was allocated", "[3mf][Determinism]") {
    GIVEN("two independently-built models with the same objects in the same order") {
        const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
        boost::filesystem::create_directories(tmp_root);
        Slic3r::set_temporary_dir(tmp_root.string());

        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
        for (const std::string& key : store_config.keys())
            if (const ConfigOption* opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
                store_config.erase(key);
                store_config.option(key, true);
            }

        WHEN("the model is exported to a fresh 3MF several times, each from its own Model instance") {
            const int kRuns = 5;
            std::vector<std::string> configs;
            std::vector<std::string> test_files;
            for (int run = 0; run < kRuns; ++run) {
                // A fresh Model per run: ModelObject::add_object() allocates with `new`, so these
                // objects do not share addresses with the previous run's, or with each other -
                // exactly the condition under which the old pointer-keyed map could reorder them.
                Model model;
                build_multi_object_model(model);

                const std::string test_file = (tmp_root / ("det_model_settings_" + std::to_string(run) + ".3mf")).string();
                test_files.push_back(test_file);

                StoreParams store_params;
                store_params.path     = test_file.c_str();
                store_params.model    = &model;
                store_params.config   = &store_config;
                store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
                REQUIRE(store_bbs_3mf(store_params));

                configs.push_back(extract_zip_entry(test_file, "Metadata/model_settings.config"));
            }

            THEN("Metadata/model_settings.config is not empty") {
                for (const std::string& c : configs)
                    REQUIRE(!c.empty());
            }

            THEN("every run's model_settings.config is byte-identical to the first") {
                for (int run = 1; run < kRuns; ++run) {
                    INFO("run " << run << " vs run 0");
                    CHECK(configs[run] == configs[0]);
                }
            }

            for (const std::string& f : test_files)
                boost::filesystem::remove(f);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Deft (slice-determinism, cause B): writes a real 3-colour painted-cube project 3MF to disk so
// the CLI can slice it several times and the resulting G-code compared. This is a scaled-down,
// self-contained stand-in for the "Bar B" fixture on origin/feat/imagemap-p2-imagefill
// (tests/libslic3r/test_image_fill.cpp, "Image Fill: write the Bar B project") - that branch's
// ImageFill feature is not present here, so the top face is painted directly through
// TriangleSelector rather than through image_fill_apply, but the shape of the fixture is the
// same: a cube, three colours, adjacent painted bands sharing mesh edges. That last property is
// what matters for cause B - two adjacent facets of DIFFERENT colours projecting to the exact
// same contour_idx/line_idx/position/length in MultiMaterialSegmentation.cpp's
// post_process_painted_lines is the tie post_process_painted_lines::comp used to leave to
// thread-arrival order (see PaintedLine::vol_idx/facet_idx and this file's own model_settings.config
// case above for the same family of bug in a different writer).
//
// A box x*y*z whose TOP face is an nx*ny grid, so a paint boundary can land exactly on a shared
// mesh edge instead of only ever inside one triangle. Adapted from test_color_split.cpp's
// make_grid_box (that file lives in this same test binary but its statics are not visible here).
static TriangleMesh det_make_grid_box(double x, double y, double z, int nx, int ny)
{
    indexed_triangle_set its;
    auto V = [&](double px, double py, double pz) { its.vertices.emplace_back(float(px), float(py), float(pz)); return int(its.vertices.size()) - 1; };
    const int b0 = V(0, 0, 0), b1 = V(x, 0, 0), b2 = V(x, y, 0), b3 = V(0, y, 0);
    std::vector<int> top((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            top[j * (nx + 1) + i] = V(x * i / nx, y * j / ny, z);
    auto T = [&](int a, int b, int c) { its.indices.emplace_back(a, b, c); };
    T(b0, b2, b1); T(b0, b3, b2);                                   // bottom (-Z)
    for (int j = 0; j < ny; ++j)                                    // top grid (+Z)
        for (int i = 0; i < nx; ++i) {
            int p = top[j * (nx + 1) + i], q = top[j * (nx + 1) + i + 1], r = top[(j + 1) * (nx + 1) + i + 1], s = top[(j + 1) * (nx + 1) + i];
            T(p, q, r); T(p, r, s);
        }
    auto side = [&](int bA, int bB, const std::vector<int> &edge) {
        T(bA, bB, edge.front());
        for (size_t k = 0; k + 1 < edge.size(); ++k) T(bB, edge[k + 1], edge[k]);
    };
    std::vector<int> e_front, e_right, e_back, e_left;
    for (int i = 0; i <= nx; ++i) e_front.push_back(top[i]);
    for (int j = 0; j <= ny; ++j) e_right.push_back(top[j * (nx + 1) + nx]);
    for (int i = nx; i >= 0; --i) e_back.push_back(top[ny * (nx + 1) + i]);
    for (int j = ny; j >= 0; --j) e_left.push_back(top[j * (nx + 1)]);
    side(b0, b1, e_front); side(b1, b2, e_right); side(b2, b3, e_back); side(b3, b0, e_left);
    return TriangleMesh(std::move(its));
}

TEST_CASE("Deft: write a 3-filament painted-cube project 3MF for the CLI slice-determinism gate", "[3mf][Determinism][barb]")
{
    // 30 mm, 60 columns x 20 rows on top (1200 cells, 2400 top facets): three colour bands of 20
    // columns each, still several millimetres wide (0.5 mm/column - well inside the slicer's
    // resolution because each band is 20 columns wide), but now with far more shared-edge colour
    // boundaries and far more painted facets landing on any one layer at once, which is what it
    // takes for tbb::parallel_for to actually split the facet range across several worker threads
    // instead of running it on one - the condition cause B's fix (PaintedLine::vol_idx/facet_idx)
    // exists for. The coarser 12x4 grid this fixture started with did not reproduce the bug even
    // WITHOUT the fix (negative control, 2026-09-07): too few facets per layer for the range to
    // split, so the old code happened to run single-threaded on that fixture regardless of the cap.
    const double        side = 30.;
    const int           nx = 60, ny = 20;
    TriangleMesh cube = det_make_grid_box(side, side, side, nx, ny);

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "det_barb_cube";
    ModelVolume *volume = object->add_volume(cube);
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(cube);
    // Top grid facets start right after the 2 bottom facets (see det_make_grid_box): cell (i, j)
    // is triangles 2 + 2*(j*nx + i) and +1. Colour by column band, so every one of the 3 interior
    // band boundaries (after column 4 and column 8) sits on a shared vertical mesh edge.
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int band  = i / (nx / 3);   // 0, 1 or 2
            const int base  = 2 + 2 * (j * nx + i);
            selector.set_facet(base,     EnforcerBlockerType(band + 1));
            selector.set_facet(base + 1, EnforcerBlockerType(band + 1));
        }
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string& key : cfg.keys())
        if (const ConfigOption* opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    // Single physical nozzle, three filaments (AMS-style) - the P1S shape report B describes.
    // printer_model/printer_variant/printable_area mirror the single-filament control fixture
    // below (a Bambu Lab P1S 0.4 nozzle's published values) so the two projects this file writes
    // for the CLI slice-determinism gate share the same machine shape - the gate's whole point is
    // comparing "painted, 3 filaments" against "unpainted, 1 filament" on the report's own printer.
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4};
    cfg.option<ConfigOptionStrings>("filament_colour")->values = {"#E01919", "#19B23F", "#1943E0"};
    cfg.set_key_value("printer_model",   new ConfigOptionString("Bambu Lab P1S"));
    cfg.set_key_value("printer_variant", new ConfigOptionString("0.4"));
    cfg.option<ConfigOptionPoints>("printable_area")->values = {
        {0., 0.}, {256., 0.}, {256., 256.}, {0., 256.}
    };

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string out = (tmp_root / "det_mmu_bar_b.3mf").string();

    StoreParams sp;
    sp.path     = out.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    WARN("Determinism Bar-B-style project written to " << out);
    // Left on disk on purpose - the CLI slice-determinism gate reads it from there.
}

// Deft (slice-determinism, control): the single-filament counterpart to the Bar B project above.
// Neither of cause A's or cause B's fixes should be reachable here - one object (so the
// object_id-sorted vector in _add_model_config_file_to_archive has one entry, in the same order a
// plain std::map<ModelObject const*, ...> would already have produced) and no mmu_segmentation_facets
// (so multi_material_segmentation_by_painting's custom_facets is empty for every extruder_idx and
// PaintedLineVisitor is never constructed) - so this project's slice is the negative control: it
// must already have been deterministic before this branch, and this branch must not change that.
// printer_model/printer_variant/bed_shape are set to a Bambu Lab P1S 0.4 nozzle's published values
// (resources/profiles/BBL/machine/Bambu Lab P1S 0.4 nozzle.json) directly in the project config
// rather than through the preset "inherits" chain, so a bare CLI slice sees the same machine shape
// report B was reproduced against without the CLI needing --load-settings to resolve presets by name.
TEST_CASE("Deft: write a single-filament P1S-shaped project 3MF as the slice-determinism control", "[3mf][Determinism]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "det_control_cube";
    object->add_volume(make_cube(30., 30., 30.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string& key : cfg.keys())
        if (const ConfigOption* opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(1);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = {0.4};
    cfg.option<ConfigOptionStrings>("filament_colour")->values = {"#FFFFFF"};
    cfg.set_key_value("printer_model",   new ConfigOptionString("Bambu Lab P1S"));
    cfg.set_key_value("printer_variant", new ConfigOptionString("0.4"));
    cfg.option<ConfigOptionPoints>("printable_area")->values = {
        {0., 0.}, {256., 0.}, {256., 256.}, {0., 256.}
    };

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string out = (tmp_root / "det_control_p1s.3mf").string();

    StoreParams sp;
    sp.path     = out.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    WARN("Determinism control (single-filament P1S-shaped) project written to " << out);
    // Left on disk on purpose - the CLI slice-determinism gate reads it from there.
}

// Rewrites the archive at `path`, letting `edit` change the name or data of each entry; returns
// whether `edit` reported a change for any of them. miniz cannot edit in place and
// open_zip_writer truncates, so the entries are held across the switch.
static bool rewrite_3mf_entries(const std::string &path, const std::function<bool(std::string &name, std::string &data)> &edit)
{
    std::vector<std::pair<std::string, std::string>> entries;
    bool                                             changed = false;
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_reader(&zip, path));
        const mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
            std::string name = st.m_filename;
            std::string data;
            if (st.m_uncomp_size > 0) {
                size_t size = 0;
                void  *mem  = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
                REQUIRE(mem != nullptr);
                data.assign(static_cast<const char *>(mem), size);
                mz_free(mem);
            }
            changed |= edit(name, data);
            entries.emplace_back(std::move(name), std::move(data));
        }
        close_zip_reader(&zip);
    }

    Zipper out(path);
    for (const auto &e : entries)
        out.add_entry(e.first, e.second.data(), e.second.size());
    out.finalize();
    return changed;
}

// Replaces the first occurrence of `from` in any entry whose name ends with `suffix`.
static bool replace_in_3mf_entry(const std::string &path, const std::string &suffix, const std::string &from, const std::string &to)
{
    bool replaced = false;
    rewrite_3mf_entries(path, [&](std::string &name, std::string &data) {
        if (replaced || !boost::algorithm::ends_with(name, suffix))
            return false;
        if (const size_t pos = data.find(from); pos != std::string::npos) {
            data.replace(pos, from.size(), to);
            replaced = true;
        }
        return replaced;
    });
    return replaced;
}

static void prepare_3mf_temp_dir()
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
}

// Stores a one-plate project holding a cube whose first two facets are painted Extruder2 and
// Extruder3, which the exporter writes as paint_color="8" and paint_color="0C". Also paints
// support / seam / fuzzy on those facets so a round-trip can check all four streams.
static void store_painted_cube(const std::string &path)
{
    prepare_3mf_temp_dir();

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "paint_cube";
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::Extruder2);
        selector.set_facet(1, EnforcerBlockerType::Extruder3);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
    }
    {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::ENFORCER);
        REQUIRE(volume->supported_facets.set(selector));
    }
    {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(1, EnforcerBlockerType::ENFORCER);
        REQUIRE(volume->seam_facets.set(selector));
    }
    {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::FUZZY_SKIN);
        REQUIRE(volume->fuzzy_skin_facets.set(selector));
    }

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams        sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));
}

// Loads `path` through the BBS importer into `model`, releasing the plates it returns.
static bool load_project(const std::string &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf,
                                     &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : project_presets)
        delete preset;
    return loaded;
}

static std::string make_temp_3mf_path(const std::string &name)
{
    prepare_3mf_temp_dir();
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    return (tmp_root / name).string();
}

static ScopeGuard remove_file_guard(const std::string &path)
{
    return ScopeGuard([path]() {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    });
}

TEST_CASE("A project with a plate id below 1 fails to load", "[3mf][Regression]")
{
    const int plate_id = GENERATE(0, -1);
    INFO("plater_id " << plate_id);

    const std::string path = make_temp_3mf_path("plate_id_" + std::to_string(plate_id) + ".3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);
    {
        Model model;
        REQUIRE(load_project(path, model));
    }

    REQUIRE(replace_in_3mf_entry(path, "model_settings.config", "key=\"plater_id\" value=\"1\"",
                                 "key=\"plater_id\" value=\"" + std::to_string(plate_id) + "\""));
    Model model;
    bool  loaded = true;
    REQUIRE_NOTHROW(loaded = load_project(path, model));
    REQUIRE_FALSE(loaded);
}

TEST_CASE("A gcode.3mf with a plate id below 1 fails to load from stream", "[3mf][Regression]")
{
    const int plate_id = GENERATE(0, -1);
    INFO("plater_id " << plate_id);

    const std::string path = make_temp_3mf_path("gcode_plate_id_" + std::to_string(plate_id) + ".3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);

    auto load_gcode = [&](bool expect_ok) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in);
        Model              model;
        DynamicPrintConfig config;
        PlateDataPtrs      plates;
        Semver             version;
        ScopeGuard         release_plates([&plates]() { release_PlateData_list(plates); });
        bool               loaded = true;
        REQUIRE_NOTHROW(loaded = load_gcode_3mf_from_stream(in, &config, &model, &plates, &version));
        if (expect_ok)
            REQUIRE(loaded);
        else
            REQUIRE_FALSE(loaded);
    };

    load_gcode(true);
    REQUIRE(replace_in_3mf_entry(path, "model_settings.config", "key=\"plater_id\" value=\"1\"",
                                 "key=\"plater_id\" value=\"" + std::to_string(plate_id) + "\""));
    load_gcode(false);
}

TEST_CASE("A project with malformed paint data loads without the damaged facet", "[3mf][Regression]")
{
    const std::string path = make_temp_3mf_path("malformed_paint.3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);
    // Split codes with no children behind them: the stream runs out mid-tree.
    REQUIRE(replace_in_3mf_entry(path, ".model", "paint_color=\"8\"", "paint_color=\"FFFFFFFFFFFFFFFF3\""));

    Model model;
    REQUIRE(load_project(path, model));
    REQUIRE(model.objects.size() == 1);
    const ModelVolume &volume = *model.objects.front()->volumes.front();
    const auto        &data   = volume.mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder2)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder3)]);

    TriangleSelector selector(volume.mesh());
    REQUIRE_NOTHROW(selector.deserialize(data));
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder2) == 0);
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);
}

TEST_CASE("A non-hex paint string drops only that facet", "[3mf][Regression]")
{
    const std::string path = make_temp_3mf_path("nonhex_paint.3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);
    REQUIRE(replace_in_3mf_entry(path, ".model", "paint_color=\"8\"", "paint_color=\"zz\""));

    Model model;
    REQUIRE(load_project(path, model));
    REQUIRE(model.objects.size() == 1);
    const ModelVolume &volume = *model.objects.front()->volumes.front();
    const auto        &data   = volume.mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder2)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder3)]);

    TriangleSelector selector(volume.mesh());
    REQUIRE_NOTHROW(selector.deserialize(data));
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder2) == 0);
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);
}

TEST_CASE("Lowercase paint hex is accepted", "[3mf][Regression]")
{
    const std::string path = make_temp_3mf_path("lowercase_paint.3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);
    REQUIRE(replace_in_3mf_entry(path, ".model", "paint_color=\"0C\"", "paint_color=\"0c\""));

    Model model;
    REQUIRE(load_project(path, model));
    REQUIRE(model.objects.size() == 1);
    const ModelVolume &volume = *model.objects.front()->volumes.front();
    const auto        &data   = volume.mmu_segmentation_facets.get_data();
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder2)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder3)]);

    TriangleSelector selector(volume.mesh());
    REQUIRE_NOTHROW(selector.deserialize(data));
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder2) == 1);
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);
}

TEST_CASE("A painted cube round-trips mmu, seam, support and fuzzy paint", "[3mf][MMUPaint]")
{
    const std::string path = make_temp_3mf_path("painted_cube_roundtrip.3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    store_painted_cube(path);

    Model dst_model;
    REQUIRE(load_project(path, dst_model));
    REQUIRE(dst_model.objects.size() == 1);
    const ModelVolume &dst_vol = *dst_model.objects.front()->volumes.front();

    TriangleSelector mmu(dst_vol.mesh());
    REQUIRE_NOTHROW(mmu.deserialize(dst_vol.mmu_segmentation_facets.get_data()));
    REQUIRE(mmu.num_facets(EnforcerBlockerType::Extruder2) == 1);
    REQUIRE(mmu.num_facets(EnforcerBlockerType::Extruder3) == 1);

    TriangleSelector support(dst_vol.mesh());
    REQUIRE_NOTHROW(support.deserialize(dst_vol.supported_facets.get_data()));
    REQUIRE(support.num_facets(EnforcerBlockerType::ENFORCER) == 1);

    TriangleSelector seam(dst_vol.mesh());
    REQUIRE_NOTHROW(seam.deserialize(dst_vol.seam_facets.get_data()));
    REQUIRE(seam.num_facets(EnforcerBlockerType::ENFORCER) == 1);

    TriangleSelector fuzzy(dst_vol.mesh());
    REQUIRE_NOTHROW(fuzzy.deserialize(dst_vol.fuzzy_skin_facets.get_data()));
    REQUIRE(fuzzy.num_facets(EnforcerBlockerType::FUZZY_SKIN) == 1);
}

TEST_CASE("Paint states 20, 200 and 255 round-trip through a 3MF byte-identically", "[3mf][MMUPaint][TriangleSelector]")
{
    const std::string path = make_temp_3mf_path("high_state_paint.3mf");
    const ScopeGuard  cleanup = remove_file_guard(path);
    prepare_3mf_temp_dir();

    Model        src_model;
    ModelObject *object = src_model.add_object();
    object->name        = "paint_cube";
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const int states[3] = {20, 200, 255};
    {
        TriangleSelector selector(volume->mesh());
        for (int i = 0; i < 3; ++i)
            selector.set_facet(i, static_cast<EnforcerBlockerType>(states[i]));
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
    }

    std::string hex[3];
    for (int i = 0; i < 3; ++i)
        hex[i] = volume->mmu_segmentation_facets.get_triangle_as_string(i);
    const auto src_data = volume->mmu_segmentation_facets.get_data();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams        sp;
    sp.path            = path.c_str();
    sp.model           = &src_model;
    sp.config          = &cfg;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));

    Model dst_model;
    REQUIRE(load_project(path, dst_model));
    REQUIRE(dst_model.objects.size() == 1);
    const ModelVolume &dst_vol = *dst_model.objects.front()->volumes.front();
    REQUIRE(src_model.objects.front()->volumes.front()->mmu_segmentation_facets.equals(dst_vol.mmu_segmentation_facets));
    REQUIRE(dst_vol.mmu_segmentation_facets.get_data() == src_data);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(dst_vol.mmu_segmentation_facets.get_triangle_as_string(i) == hex[i]);
        REQUIRE_FALSE(hex[i].empty());
    }

    TriangleSelector restored(dst_vol.mesh());
    REQUIRE_NOTHROW(restored.deserialize(dst_vol.mmu_segmentation_facets.get_data()));
    for (int i = 0; i < 3; ++i)
        REQUIRE(restored.num_facets(static_cast<EnforcerBlockerType>(states[i])) == 1);
}
