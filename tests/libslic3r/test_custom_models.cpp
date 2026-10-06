#include <catch2/catch.hpp>

#include "libslic3r/CustomModels.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <set>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace cm = Slic3r::custom_models;

namespace {

// A scratch folder per test case, removed at the end.
struct ScratchDir
{
    fs::path path;
    ScratchDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("snorca_custom_models_%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~ScratchDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path &p)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary);
    f << "x";
}

std::vector<std::string> labels(const std::vector<cm::Entry> &entries)
{
    std::vector<std::string> out;
    for (const cm::Entry &e : entries)
        out.push_back(e.label);
    return out;
}

std::vector<std::string> labels(const std::vector<cm::Folder> &folders)
{
    std::vector<std::string> out;
    for (const cm::Folder &f : folders)
        out.push_back(f.label);
    return out;
}

// The exporter writes its scratch files under Model::get_backup_path(), which is rooted at
// temporary_dir(); the application sets it at startup, a test that stores a 3MF has to as well.
void use_scratch_as_temporary_dir(const ScratchDir &scratch)
{
    const fs::path tmp = scratch.path / "tmp";
    fs::create_directories(tmp);
    Slic3r::set_temporary_dir(tmp.string());
}

struct Loaded
{
    bool               ok = false;
    Model              model;
    DynamicPrintConfig config;
};

// What the application's "Add Custom Models" asks the loader for: the model with its object /
// part settings, without LoadConfig, so nothing of the file's project settings is applied.
std::unique_ptr<Loaded> load_as_custom_model(const fs::path &file)
{
    auto                      out = std::make_unique<Loaded>();
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    out->ok = load_bbs_3mf(file.string().c_str(), &out->config, &ctxt, &out->model, &plate_data, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                           LoadStrategy::LoadModel | LoadStrategy::KeepObjectSettings | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    return out;
}

ModelVolume *volume_named(ModelObject &object, const std::string &name)
{
    for (ModelVolume *v : object.volumes)
        if (v->name == name)
            return v;
    return nullptr;
}

// A bracket: a body part and a modifier, a per-object override, a per-part override, a filament
// assignment and a painted region.
ModelObject *make_bracket(Model &model, const std::string &name)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    ModelVolume *body   = object->add_volume(make_cube(20., 10., 5.));
    body->name          = "body";
    ModelVolume *zone   = object->add_volume(make_cube(6., 6., 6.));
    zone->name          = "infill zone";
    zone->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    zone->set_offset({4., 0., 0.});
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

void paint_first_facets(ModelVolume *volume, int filament_state, int facets)
{
    const indexed_triangle_set &its = volume->mesh().its;
    TriangleSelector            selector(volume->mesh());
    for (int f = 0; f < std::min<int>(facets, int(its.indices.size())); ++f)
        selector.set_facet(f, EnforcerBlockerType(filament_state));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
}

} // namespace

// ---- library listing ---------------------------------------------------------------------------------

SCENARIO("The Custom Models library is listed by name, with sub-folders as sub-menus", "[CustomModels]") {
    GIVEN("a library folder with files, hidden files, junk and sub-folders") {
        ScratchDir scratch;
        const fs::path root = scratch.path / "custom_models";
        touch(root / "Zeta.stl");
        touch(root / "alpha.3mf");
        touch(root / "Part 10.stl");
        touch(root / "Part 2.stl");
        touch(root / "notes.txt");
        touch(root / "readme.md");
        touch(root / ".hidden.stl");
        touch(root / "~$lock.3mf");
        touch(root / "cube.stl");
        touch(root / "cube.3mf");
        touch(root / "Brackets" / "b.STL");
        touch(root / "Brackets" / "a.obj");
        touch(root / "Brackets" / "sub" / "deep.step");
        touch(root / "OnlyJunk" / "junk.txt");
        fs::create_directories(root / "Empty");
        fs::create_directories(root / ".git");
        touch(root / ".git" / "model.stl");

        const cm::Folder library = cm::scan_library(root);

        THEN("only files with a model extension are listed, sorted case-insensitively with numbers as numbers") {
            CHECK(labels(library.files) == std::vector<std::string>{"alpha", "cube (.3mf)", "cube (.stl)", "Part 2", "Part 10", "Zeta"});
        }
        THEN("two files of one name are told apart by their extension") {
            REQUIRE(library.files.size() == 6);
            CHECK(library.files[1].type == cm::FileType::Project3mf);
            CHECK(library.files[2].type == cm::FileType::Mesh);
            CHECK(library.files[1].path.filename() == "cube.3mf");
        }
        THEN("sub-folders become folders, and a folder with no models in it is left out") {
            CHECK(labels(library.folders) == std::vector<std::string>{"Brackets"});
            REQUIRE(library.folders.size() == 1);
            const cm::Folder &brackets = library.folders.front();
            CHECK(labels(brackets.files) == std::vector<std::string>{"a", "b"});
            CHECK(brackets.files[0].type == cm::FileType::Mesh);
            CHECK(brackets.files[1].type == cm::FileType::Mesh); // ".STL": the extension is not case-sensitive
            REQUIRE(brackets.folders.size() == 1);
            CHECK(brackets.folders.front().label == "sub");
            CHECK(labels(brackets.folders.front().files) == std::vector<std::string>{"deep"});
        }
        THEN("the count covers every depth, and the library is not empty") {
            CHECK(library.file_count() == 9);
            CHECK_FALSE(library.empty());
            CHECK_FALSE(library.truncated);
        }

        WHEN("the file limit is reached") {
            cm::ScanLimits limits;
            limits.max_files          = 3;
            const cm::Folder limited = cm::scan_library(root, limits);
            THEN("the scan stops there and says so") {
                CHECK(limited.file_count() == 3);
                CHECK(limited.truncated);
            }
        }
        WHEN("sub-folders are not allowed") {
            cm::ScanLimits limits;
            limits.max_depth          = 0;
            const cm::Folder flat = cm::scan_library(root, limits);
            THEN("only the top level is listed") {
                CHECK(flat.folders.empty());
                CHECK(flat.files.size() == 6);
            }
        }
    }
    GIVEN("a folder that is missing, or has nothing in it") {
        ScratchDir scratch;
        THEN("the library is empty") {
            CHECK(cm::scan_library(scratch.path / "nope").empty());
            fs::create_directories(scratch.path / "custom_models");
            CHECK(cm::scan_library(scratch.path / "custom_models").empty());
            touch(scratch.path / "custom_models" / "notes.txt");
            CHECK(cm::scan_library(scratch.path / "custom_models").empty());
        }
    }
    GIVEN("the data directory") {
        THEN("the library is the custom_models folder inside it") {
            CHECK(cm::library_dir("/data") == fs::path("/data") / "custom_models");
        }
    }
}

SCENARIO("Names sort the way a person expects", "[CustomModels]") {
    CHECK(cm::natural_less("a2", "a10"));
    CHECK_FALSE(cm::natural_less("a10", "a2"));
    CHECK(cm::natural_less("alpha", "Zeta"));
    CHECK(cm::natural_less("Alpha", "beta"));
    CHECK(cm::natural_less("file 007", "file 8"));
    // Equal ignoring case: the raw bytes decide, so the order is total.
    CHECK(cm::natural_less("A", "a") != cm::natural_less("a", "A"));
    CHECK_FALSE(cm::natural_less("same", "same"));
}

// ---- the name typed for "Save selected object to Custom Models" -------------------------------------------

SCENARIO("A typed name becomes a safe file name in the library", "[CustomModels]") {
    CHECK(cm::library_file_name("Benchy") == "Benchy.3mf");
    CHECK(cm::library_file_name("  Benchy  ") == "Benchy.3mf");
    CHECK(cm::library_file_name("Benchy.3mf") == "Benchy.3mf");
    CHECK(cm::library_file_name("benchy.3MF") == "benchy.3mf");
    CHECK(cm::library_file_name("v1.2 hinge") == "v1.2 hinge.3mf");
    CHECK(cm::library_file_name(u8"W\u00fcrfel") == u8"W\u00fcrfel.3mf");

    // Only the last path component survives: a typed name cannot leave the library folder.
    CHECK(cm::library_file_name("a/b") == "b.3mf");
    CHECK(cm::library_file_name("..\\..\\evil") == "evil.3mf");
    CHECK(cm::library_file_name("C:\\Users\\x\\y") == "y.3mf");
    CHECK(cm::library_file_name("../../evil.3mf") == "evil.3mf");
    // Reserved characters are replaced.
    CHECK(cm::library_file_name("my:model?") == "my_model_.3mf");
    CHECK(cm::library_file_name("a*b|c") == "a_b_c.3mf");

    // Nothing usable left.
    CHECK(cm::library_file_name("").empty());
    CHECK(cm::library_file_name("   ").empty());
    CHECK(cm::library_file_name("..").empty());
    CHECK(cm::library_file_name("...").empty());
    CHECK(cm::library_file_name(".3mf").empty());
    CHECK(cm::library_file_name("dir/").empty());

    // Windows device names are defused.
    CHECK(cm::library_file_name("NUL") == "_NUL.3mf");
    CHECK(cm::library_file_name("con.3mf") == "_con.3mf");

    // A very long name is capped and keeps its extension.
    const std::string longest = cm::library_file_name(std::string(400, 'a'));
    CHECK(longest.size() <= 150);
    CHECK(longest.size() > 100);
    CHECK(longest.substr(longest.size() - 4) == ".3mf");

    // Whatever comes out is a plain file name.
    for (const char *typed : {"x/../y", "a\\b\\c", "<>", "tab\there", "line\nbreak"}) {
        const std::string name = cm::library_file_name(typed);
        INFO("typed: " << typed);
        CHECK(name.find('/') == std::string::npos);
        CHECK(name.find('\\') == std::string::npos);
        CHECK(name.find('\n') == std::string::npos);
        CHECK(name.find('\t') == std::string::npos);
    }
}

// ---- round trip --------------------------------------------------------------------------------------------

SCENARIO("A saved custom model keeps object settings, modifiers and paint, and no printer settings", "[CustomModels][3mf]") {
    GIVEN("a bracket with an override, a modifier, a part override, painting and a filament") {
        ScratchDir scratch;
        use_scratch_as_temporary_dir(scratch);

        Model        source;
        ModelObject *bracket = make_bracket(source, "bracket");
        bracket->config.set("wall_loops", 5);
        bracket->config.set("extruder", 2);
        ModelVolume *body = volume_named(*bracket, "body");
        ModelVolume *zone = volume_named(*bracket, "infill zone");
        REQUIRE(body != nullptr);
        REQUIRE(zone != nullptr);
        zone->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(35));
        body->config.set("top_shell_layers", 7);
        paint_first_facets(body, 4, 3);
        // Where it sat on the plate must not matter.
        bracket->instances.front()->set_offset(Vec3d(120., 80., bracket->instances.front()->get_offset().z()));

        const fs::path file = scratch.path / "custom_models" / "bracket.3mf";

        WHEN("it is saved to the library and loaded back the way Add Custom Models loads it") {
            const cm::SaveResult saved = cm::save_objects({{bracket, 0}}, file);
            THEN("the save succeeds and leaves only the 3MF in the folder") {
                REQUIRE(saved.ok);
                REQUIRE(fs::is_regular_file(file));
                std::set<std::string> names;
                for (fs::directory_iterator it(file.parent_path()); it != fs::directory_iterator(); ++it)
                    names.insert(it->path().filename().string());
                CHECK(names == std::set<std::string>{"bracket.3mf"});
            }
            auto loaded = load_as_custom_model(file);
            REQUIRE(loaded->ok);
            REQUIRE(loaded->model.objects.size() == 1);
            ModelObject &back = *loaded->model.objects.front();

            THEN("the object comes back with its name, parts and modifier") {
                CHECK(back.name == "bracket");
                REQUIRE(back.volumes.size() == 2);
                ModelVolume *back_body = volume_named(back, "body");
                ModelVolume *back_zone = volume_named(back, "infill zone");
                REQUIRE(back_body != nullptr);
                REQUIRE(back_zone != nullptr);
                CHECK(back_body->type() == ModelVolumeType::MODEL_PART);
                CHECK(back_zone->type() == ModelVolumeType::PARAMETER_MODIFIER);
                CHECK(back_zone->mesh().its.indices.size() == zone->mesh().its.indices.size());
            }
            THEN("the per-object and per-part overrides and the filament survive") {
                REQUIRE(back.config.has("wall_loops"));
                CHECK(back.config.opt_int("wall_loops") == 5);
                CHECK(back.config.opt_int("extruder") == 2);
                ModelVolume *back_zone = volume_named(back, "infill zone");
                ModelVolume *back_body = volume_named(back, "body");
                REQUIRE(back_zone != nullptr);
                REQUIRE(back_body != nullptr);
                const auto *density = back_zone->config.get().option<ConfigOptionPercent>("sparse_infill_density");
                REQUIRE(density != nullptr);
                CHECK(density->value == Approx(35.));
                CHECK(back_body->config.opt_int("top_shell_layers") == 7);
                // Settings were not invented on the way: only what was set comes back.
                CHECK_FALSE(back.config.has("sparse_infill_density"));
                CHECK_FALSE(back_zone->config.has("top_shell_layers"));
            }
            THEN("the painted region survives") {
                ModelVolume *back_body = volume_named(back, "body");
                REQUIRE(back_body != nullptr);
                const std::vector<size_t> painted = back_body->get_extruders_from_multi_material_painting();
                CHECK(painted == std::vector<size_t>{3}); // filament 4, zero-based
            }
            THEN("no printer, filament or process settings are in the file") {
                for (const char *key : {"printer_settings_id", "filament_settings_id", "print_settings_id", "nozzle_diameter", "layer_height",
                                        "filament_colour", "printable_area", "machine_start_gcode", "sparse_infill_density"})
                    CHECK_FALSE(loaded->config.has(key));
                CHECK(loaded->config.empty());
            }
            THEN("the group is centred on the origin, not where the object sat on the plate") {
                REQUIRE_FALSE(back.instances.empty());
                const BoundingBoxf3 bb = back.instance_bounding_box(0, false);
                CHECK(bb.center().x() == Approx(0.).margin(1e-3));
                CHECK(bb.center().y() == Approx(0.).margin(1e-3));
                CHECK(back.instances.size() == 1);
            }
        }
    }
}

SCENARIO("Saving replaces a file of the same name and leaves no scratch file behind", "[CustomModels][3mf]") {
    GIVEN("a library file that is saved twice with different content") {
        ScratchDir scratch;
        use_scratch_as_temporary_dir(scratch);
        const fs::path file = scratch.path / "lib" / "thing.3mf";

        Model        first;
        ModelObject *a = make_bracket(first, "first");
        a->config.set("wall_loops", 2);
        Model        second;
        ModelObject *b = make_bracket(second, "second");
        b->config.set("wall_loops", 9);

        REQUIRE(cm::save_objects({{a, 0}}, file).ok);
        REQUIRE(cm::save_objects({{b, 0}}, file).ok);

        THEN("the second save is what is in the file, and nothing else is in the folder") {
            auto loaded = load_as_custom_model(file);
            REQUIRE(loaded->ok);
            REQUIRE(loaded->model.objects.size() == 1);
            CHECK(loaded->model.objects.front()->name == "second");
            CHECK(loaded->model.objects.front()->config.opt_int("wall_loops") == 9);
            std::set<std::string> names;
            for (fs::directory_iterator it(file.parent_path()); it != fs::directory_iterator(); ++it)
                names.insert(it->path().filename().string());
            CHECK(names == std::set<std::string>{"thing.3mf"});
        }
    }
    GIVEN("nothing to save") {
        THEN("the save fails with a message and writes nothing") {
            ScratchDir scratch;
            use_scratch_as_temporary_dir(scratch);
            const cm::SaveResult r = cm::save_objects({}, scratch.path / "x.3mf");
            CHECK_FALSE(r.ok);
            CHECK_FALSE(r.error.empty());
            CHECK_FALSE(fs::exists(scratch.path / "x.3mf"));
        }
    }
}

SCENARIO("Several selected objects are saved together, one instance each", "[CustomModels][3mf]") {
    GIVEN("two objects, the second with two copies of which the second is selected") {
        ScratchDir scratch;
        use_scratch_as_temporary_dir(scratch);

        Model        source;
        ModelObject *one = make_bracket(source, "one");
        ModelObject *two = make_bracket(source, "two");
        one->instances.front()->set_offset(Vec3d(10., 10., one->instances.front()->get_offset().z()));
        two->instances.front()->set_offset(Vec3d(60., 10., two->instances.front()->get_offset().z()));
        ModelInstance *copy = two->add_instance(*two->instances.front());
        copy->set_offset(Vec3d(60., 90., two->instances.front()->get_offset().z()));
        copy->set_rotation(Vec3d(0., 0., 1.5));

        const fs::path file = scratch.path / "pair.3mf";
        REQUIRE(cm::save_objects({{one, 0}, {two, 1}}, file).ok);

        auto loaded = load_as_custom_model(file);
        THEN("both objects come back once, keeping the rotation of the selected copy") {
            REQUIRE(loaded->ok);
            REQUIRE(loaded->model.objects.size() == 2);
            for (const ModelObject *o : loaded->model.objects)
                CHECK(o->instances.size() == 1);
            const ModelObject *back_two = loaded->model.objects[0]->name == "two" ? loaded->model.objects[0] : loaded->model.objects[1];
            CHECK(back_two->instances.front()->get_rotation().z() == Approx(1.5).margin(1e-6));
        }
        THEN("their positions relative to each other are kept") {
            REQUIRE(loaded->ok);
            REQUIRE(loaded->model.objects.size() == 2);
            const ModelObject *a = loaded->model.objects[0];
            const ModelObject *b = loaded->model.objects[1];
            const double       dy = std::abs(a->instances.front()->get_offset().y() - b->instances.front()->get_offset().y());
            const double       dx = std::abs(a->instances.front()->get_offset().x() - b->instances.front()->get_offset().x());
            CHECK(dx == Approx(50.).margin(1e-3));
            CHECK(dy == Approx(80.).margin(1e-3));
        }
    }
}

// ---- before the objects join the project ----------------------------------------------------------------------------

SCENARIO("A library 3MF is prepared for the current project's filaments, and checked for untrusted settings", "[CustomModels]") {
    GIVEN("an object that asks for filaments 5, 3 and 4 and paints with 4 and 2") {
        Model        model;
        ModelObject *bracket = make_bracket(model, "bracket");
        bracket->config.set("extruder", 5);
        bracket->config.set("wall_loops", 6);
        bracket->config.set("support_filament", 4);
        ModelVolume *body = volume_named(*bracket, "body");
        ModelVolume *zone = volume_named(*bracket, "infill zone");
        body->config.set("extruder", 3);
        zone->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(20));
        {
            const indexed_triangle_set &its = body->mesh().its;
            TriangleSelector            selector(body->mesh());
            for (int f = 0; f < std::min<int>(6, int(its.indices.size())); ++f)
                selector.set_facet(f, EnforcerBlockerType(f % 2 == 0 ? 4 : 2));
            REQUIRE(body->mmu_segmentation_facets.set(selector));
        }

        WHEN("the project has 2 filaments") {
            const cm::ImportReport report = cm::prepare_imported_objects({bracket}, 2);
            THEN("every number above 2 becomes filament 1") {
                CHECK(bracket->config.opt_int("extruder") == 1);
                CHECK(bracket->config.opt_int("support_filament") == 1);
                CHECK(body->config.opt_int("extruder") == 1);
                CHECK(report.filament_settings_clamped == 3);
                CHECK(report.painted_volumes_clamped == 1);
                CHECK(report.highest_filament_requested == 5);
                CHECK(report.filaments_clamped());
            }
            THEN("painted regions that used filament 4 now use filament 1, and filament 2 stays") {
                const std::vector<size_t> painted = body->get_extruders_from_multi_material_painting();
                CHECK(painted == std::vector<size_t>{0, 1});
            }
            THEN("everything else is as the file had it") {
                CHECK(bracket->config.opt_int("wall_loops") == 6);
                const auto *density = zone->config.get().option<ConfigOptionPercent>("sparse_infill_density");
                REQUIRE(density != nullptr);
                CHECK(density->value == Approx(20.));
                CHECK(zone->type() == ModelVolumeType::PARAMETER_MODIFIER);
            }
        }
        WHEN("the project has enough filaments") {
            const cm::ImportReport report = cm::prepare_imported_objects({bracket}, 8);
            THEN("nothing is changed and nothing is reported") {
                CHECK(bracket->config.opt_int("extruder") == 5);
                CHECK(bracket->config.opt_int("support_filament") == 4);
                CHECK(body->config.opt_int("extruder") == 3);
                CHECK(body->get_extruders_from_multi_material_painting() == std::vector<size_t>{1, 3});
                CHECK_FALSE(report.filaments_clamped());
                CHECK(report.highest_filament_requested == 5);
            }
        }
        WHEN("the project has a single filament") {
            cm::prepare_imported_objects({bracket}, 1);
            THEN("all of it is filament 1") {
                CHECK(bracket->config.opt_int("extruder") == 1);
                CHECK(body->get_extruders_from_multi_material_painting() == std::vector<size_t>{0});
            }
        }
    }
    GIVEN("the default filament (0) and unset filament settings") {
        Model        model;
        ModelObject *bracket = make_bracket(model, "bracket");
        bracket->config.set("extruder", 0);
        WHEN("it is prepared for a one-filament project") {
            const cm::ImportReport report = cm::prepare_imported_objects({bracket}, 1);
            THEN("0 stays 0 - it means the default - and nothing is reported") {
                CHECK(bracket->config.opt_int("extruder") == 0);
                CHECK_FALSE(report.filaments_clamped());
                CHECK(report.highest_filament_requested == 0);
            }
        }
    }
    GIVEN("object, part and layer-range settings that run programs or point at the network") {
        Model        model;
        ModelObject *bracket = make_bracket(model, "bracket");
        ModelVolume *body    = volume_named(*bracket, "body");
        bracket->config.set_key_value("post_process", new ConfigOptionStrings({"C:\\evil.exe"}));
        bracket->config.set("wall_loops", 3);
        body->config.set_key_value("post_process", new ConfigOptionStrings({"python steal.py"}));
        body->config.set_key_value("filename_format", new ConfigOptionString("..\\..\\out\\{input_filename_base}.gcode"));
        body->config.set_key_value("print_host", new ConfigOptionString("http://evil.example"));
        bracket->layer_config_ranges[{0.2, 1.0}].set_key_value("post_process", new ConfigOptionStrings({"rm -rf"}));
        bracket->layer_config_ranges[{0.2, 1.0}].set("wall_loops", 4);

        WHEN("they are prepared") {
            const cm::ImportReport report = cm::prepare_imported_objects({bracket}, 4);
            THEN("the dangerous keys are gone and the harmless ones stay") {
                CHECK(report.untrusted_settings_removed == 5);
                CHECK_FALSE(bracket->config.has("post_process"));
                CHECK_FALSE(body->config.has("post_process"));
                CHECK_FALSE(body->config.has("filename_format"));
                CHECK_FALSE(body->config.has("print_host"));
                CHECK_FALSE(bracket->layer_config_ranges[{0.2, 1.0}].has("post_process"));
                CHECK(bracket->config.opt_int("wall_loops") == 3);
                CHECK(bracket->layer_config_ranges[{0.2, 1.0}].opt_int("wall_loops") == 4);
            }
        }
    }
    GIVEN("a filename format that stays in the output folder") {
        Model        model;
        ModelObject *bracket = make_bracket(model, "bracket");
        bracket->config.set_key_value("filename_format", new ConfigOptionString("{input_filename_base}_{layer_height}.gcode"));
        THEN("it is not touched") {
            const cm::ImportReport report = cm::prepare_imported_objects({bracket}, 1);
            CHECK(report.untrusted_settings_removed == 0);
            CHECK(bracket->config.has("filename_format"));
        }
    }
}

SCENARIO("The filament keys are the ones a per-object or per-part setting can carry", "[CustomModels]") {
    const std::vector<std::string> &keys = cm::filament_index_keys();
    CHECK(std::find(keys.begin(), keys.end(), "extruder") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "wall_filament") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "support_interface_filament") != keys.end());
    for (const std::string &key : keys) {
        INFO("key " << key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->type == coInt);
    }
}

// A library 3MF written by the full project save (what a person who drops a finished project into
// the folder has): the loader hands back the project settings, and loading never applies them -
// the application only applies them with LoadConfig, which Add Custom Models does not pass.
SCENARIO("A project 3MF dropped into the library carries settings that the loader reports but Add Custom Models never applies", "[CustomModels][3mf]") {
    GIVEN("a project saved with a full print configuration") {
        ScratchDir scratch;
        use_scratch_as_temporary_dir(scratch);
        Model        source;
        ModelObject *bracket = make_bracket(source, "bracket");
        bracket->config.set("wall_loops", 5);

        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
        for (const std::string &key : store_config.keys())
            if (const ConfigOption *opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
                store_config.erase(key);
                store_config.option(key, true);
            }
        const fs::path file = scratch.path / "project.3mf";
        StoreParams    params;
        const std::string path = file.string();
        params.path     = path.c_str();
        params.model    = &source;
        params.config   = &store_config;
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(params));

        WHEN("it is loaded as a custom model") {
            auto loaded = load_as_custom_model(file);
            THEN("the objects come with their settings, and what the caller does with the project settings is up to it") {
                REQUIRE(loaded->ok);
                REQUIRE(loaded->model.objects.size() == 1);
                CHECK(loaded->model.objects.front()->config.opt_int("wall_loops") == 5);
                // The loader reports the file's settings; the application's Add Custom Models path
                // (Plater::load_files without LoadConfig) never hands them to the preset bundle.
                CHECK(loaded->config.has("layer_height"));
            }
        }
    }
}
