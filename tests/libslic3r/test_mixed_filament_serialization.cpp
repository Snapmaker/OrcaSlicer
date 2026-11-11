#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilament/Serialization.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r;
using Json = nlohmann::json;

namespace {
const std::string recipes = "1,2,1,1,25,0,m2,u101;"
                            "1,2,1,1,50,0,g1/2/12,w20/30/50,m0,z4,xa0.02,xb-0.01,u102,cm3,r1/1/0,p0/0.2/0.5/0.8/1,v0/0.03/0;"
                            "1,2,1,1,50,0,m0,u103,cm1,1[12]2,21[10];"
                            "1,2,0,1,25,0,m2,u104;"
                            "2,3,0,0,75,0,m2,d1,o1,u105;"
                            "1,3,1,0,50,0,m2,o1,u106;"
                            "2,3,1,1,50,0,m2,o1,u18446744073709551615";

DynamicPrintConfig recipe_config()
{
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>(12, "#808080")));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(recipes));
    return config;
}

struct TemporaryDirectory
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mixed-json-%%%%-%%%%");
    TemporaryDirectory() { boost::filesystem::create_directories(path); }
    ~TemporaryDirectory()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

struct LoadedProject
{
    Model                model;
    DynamicPrintConfig   config;
    PlateDataPtrs        plates;
    std::vector<Preset*> presets;
    ~LoadedProject()
    {
        release_PlateData_list(plates);
        for (auto* preset : presets)
            delete preset;
    }
    bool load(const std::string& path, const std::string& backup)
    {
        model.set_backup_path(backup);
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        bool                      project = false;
        Semver                    version;
        return load_bbs_3mf(path.c_str(), &config, &substitutions, &model, &plates, &presets, &project, &version, nullptr,
                            LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence);
    }
};

// Read and rebuild an actual archive so precedence and failures exercise the
// production importer, including reversed entry order and duplicate parts.
using Entries = std::vector<std::pair<std::string, std::string>>;
Entries read_zip(const std::string& path)
{
    mz_zip_archive archive{};
    REQUIRE(open_zip_reader(&archive, path));
    struct Close
    {
        mz_zip_archive* archive;
        ~Close() { close_zip_reader(archive); }
    } close{&archive};
    Entries entries;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&archive, i, &stat));
        std::string data(size_t(stat.m_uncomp_size), '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&archive, i, data.data(), data.size(), 0));
        entries.emplace_back(stat.m_filename, std::move(data));
    }
    return entries;
}

void write_zip(const std::string& path, const Entries& entries)
{
    mz_zip_archive archive{};
    REQUIRE(open_zip_writer(&archive, path));
    struct Close
    {
        mz_zip_archive* archive;
        ~Close() { close_zip_writer(archive); }
    } close{&archive};
    for (const auto& entry : entries)
        REQUIRE(mz_zip_writer_add_mem(&archive, entry.first.c_str(), entry.second.data(), entry.second.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&archive));
}

std::string& entry(Entries& entries, const std::string& name)
{
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.first == name; });
    REQUIRE(found != entries.end());
    return found->second;
}

bool save_project(const std::string& path, Model& model, DynamicPrintConfig& config)
{
    StoreParams params;
    params.path     = path.c_str();
    params.model    = &model;
    params.config   = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::Silence |
                      SaveStrategy::SkipAuxiliary;
    return store_bbs_3mf(params);
}

DynamicPrintConfig full_config()
{
    auto config = DynamicPrintConfig::full_print_config();
    // The exporter requires enum dictionaries on full-default vector options.
    for (const auto& key : config.keys()) {
        if (config.option(key)->type() == coEnums) {
            auto* option = config.def()->get(key)->create_default_option();
            option->set(config.option(key));
            config.set_key_value(key, option);
        }
    }
    config.apply(recipe_config());
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(12, 1.75);
    return config;
}
} // namespace

TEST_CASE("Mixed JSON retains typed recipe meaning and legacy compatibility", "[MixedFilamentJSON][Persistence]")
{
    const auto config      = recipe_config();
    const auto definitions = mixed_filaments_from_project_config(config, 12);
    REQUIRE(definitions.size() == 7);
    const auto legacy   = mixed_filaments_to_legacy(definitions);
    const auto text     = serialize_mixed_filaments_json(definitions, 12);
    const auto document = Json::parse(text);
    CHECK(document.at("version") == 1);
    CHECK(document.at("recipes")[1].at("pair_percent_b") == 50);
    CHECK(document.at("recipes")[2].at("pattern") == Json::parse("[[1,12,2],[2,1,10]]"));
    CHECK(document.at("recipes")[3].at("state") == "disabled");
    CHECK(document.at("recipes")[4].at("state") == "deleted");
    CHECK(document.at("recipes")[5].at("origin") == "generated");
    CHECK(document.at("recipes")[6].at("origin") == "customized_generated");
    CHECK(document.at("recipes")[6].at("id") == "18446744073709551615");

    const auto restored = deserialize_mixed_filaments_json(text, 12);
    REQUIRE(restored.size() == definitions.size());
    CHECK(mixed_filaments_to_legacy(restored) == legacy);
    CHECK(serialize_mixed_filaments_json(restored, 12) == text);
    MixedFilamentManager old_reader;
    old_reader.load_custom_entries(legacy, std::vector<std::string>(12, "#808080"), MixedFilamentManager::LoadMode::Exact);
    CHECK(old_reader.filament_id_from_stable_id(101, 12) == 13);
    CHECK(old_reader.filament_id_from_stable_id(103, 12) == 15);
    CHECK_FALSE(old_reader.filament_id_from_stable_id(104, 12));
    CHECK_FALSE(old_reader.filament_id_from_stable_id(105, 12));
    CHECK(old_reader.filament_id_from_stable_id(106, 12) == 16);
}

TEST_CASE("Mixed JSON validates numbers identities and references before publication", "[MixedFilamentJSON][Persistence]")
{
    auto  document = Json::parse(serialize_mixed_filaments_json(mixed_filaments_from_project_config(recipe_config(), 12), 12));
    auto& recipe   = document["recipes"][0];
    SECTION("Future version") { document["version"] = 2; }
    SECTION("Fractional version") { document["version"] = 1.5; }
    SECTION("Missing recipes") { document.erase("recipes"); }
    SECTION("Duplicate identity") { document["recipes"][1]["id"] = recipe["id"]; }
    SECTION("Zero identity") { recipe["id"] = "0"; }
    SECTION("Overflow identity") { recipe["id"] = "18446744073709551616"; }
    SECTION("Numeric identity") { recipe["id"] = 101; }
    SECTION("Negative identity") { recipe["id"] = "-1"; }
    SECTION("Missing physical filament") { recipe["components"][0]["filament"] = 13; }
    SECTION("Narrowing overflow") { recipe["components"][0]["filament"] = uint64_t(4294967297); }
    SECTION("Negative reference") { recipe["components"][0]["filament"] = -1; }
    SECTION("Fractional reference") { recipe["components"][0]["filament"] = 1.5; }
    SECTION("Duplicate component") { recipe["components"][1]["filament"] = 1; }
    SECTION("Bad total") { recipe["components"][0]["percent"] = 74; }
    SECTION("Empty pattern group") { recipe["pattern"] = Json::parse("[[]]"); }
    SECTION("Pattern references missing filament") { recipe["pattern"] = Json::parse("[[13]]"); }
    SECTION("Bad gradient order") { document["recipes"][1]["gradient"]["order"] = {1, 2, 2}; }
    SECTION("Bad gradient stops") { document["recipes"][1]["gradient"]["stops"] = {0, .8, .5, .2, 1}; }
    SECTION("Bad gradient widths") { document["recipes"][1]["gradient"]["solid_widths"] = {0, .03}; }
    SECTION("Invalid surface bias") { recipe["surface_bias"] = {{"a_mm", 3}, {"b_mm", 0}}; }
    SECTION("Unsupported distribution") { recipe["distribution"] = "unknown"; }
    SECTION("Unsupported state") { recipe["state"] = "unknown"; }
    REQUIRE_THROWS_AS(deserialize_mixed_filaments_json(document.dump(), 12), std::runtime_error);
}

TEST_CASE("Mixed JSON owns recipes across production 3MF loads and saves", "[MixedFilamentJSON][Persistence][3mf]")
{
    TemporaryDirectory temporary;
    Model              model;
    model.set_backup_path((temporary.path / "export").string());
    auto* object = model.add_object();
    object->config.set("extruder", 13);
    auto* volume = object->add_volume(make_cube(5., 5., 5.));
    volume->config.set("extruder", 15);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(14));
    volume->mmu_segmentation_facets.set(selector);
    const auto paint = volume->mmu_segmentation_facets.get_triangle_as_string(0);
    // Single-part objects intentionally inherit the object assignment on load.
    object->add_volume(make_cube(2., 2., 2.))->set_offset(Vec3d(10., 0., 0.));
    object->add_instance();
    // Exercise the real ShareMesh export path as well as object/part identities.
    model.add_object(*object)->config.set("extruder", 16);
    auto       config   = full_config();
    const auto original = (temporary.path / "original.3mf").string();
    REQUIRE(save_project(original, model, config));
    auto       entries    = read_zip(original);
    const auto saved_json = entry(entries, MIXED_FILAMENTS_3MF_PATH);
    const auto saved_legacy =
        Json::parse(entry(entries, "Metadata/project_settings.config")).at("mixed_filament_definitions").get<std::string>();
    CHECK(saved_legacy == mixed_filaments_to_legacy(deserialize_mixed_filaments_json(saved_json, 12)));
    CHECK(config.opt_string("mixed_filament_definitions") == recipes); // Save does not mutate the caller.
    bool success = true;
    bool empty   = false;
    SECTION("Ordinary round trip") {}
    SECTION("JSON without a legacy recipe field")
    {
        auto settings = Json::parse(entry(entries, "Metadata/project_settings.config"));
        settings.erase("mixed_filament_definitions");
        entry(entries, "Metadata/project_settings.config") = settings.dump();
    }
    SECTION("JSON precedes stale legacy settings in ZIP order")
    {
        auto settings                                      = Json::parse(entry(entries, "Metadata/project_settings.config"));
        settings["mixed_filament_definitions"]             = "1,2,1,1,99,0,m2,u999";
        entry(entries, "Metadata/project_settings.config") = settings.dump();
        std::reverse(entries.begin(), entries.end());
    }
    SECTION("Legacy-only project")
    {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [](const auto& item) { return item.first == MIXED_FILAMENTS_3MF_PATH; }),
                      entries.end());
    }
    SECTION("Empty canonical recipes clear stale legacy recipes")
    {
        entry(entries, MIXED_FILAMENTS_3MF_PATH) = R"({"version":1,"recipes":[]})";
        empty                                    = true;
    }
    SECTION("Malformed canonical JSON never falls back")
    {
        entry(entries, MIXED_FILAMENTS_3MF_PATH) = "{";
        success                                  = false;
    }
    SECTION("Unsupported canonical version never falls back")
    {
        entry(entries, MIXED_FILAMENTS_3MF_PATH) = R"({"version":2,"recipes":[]})";
        success                                  = false;
    }
    SECTION("Missing required recipes never falls back")
    {
        entry(entries, MIXED_FILAMENTS_3MF_PATH) = R"({"version":1})";
        success                                  = false;
    }
    SECTION("Empty JSON file never falls back")
    {
        entry(entries, MIXED_FILAMENTS_3MF_PATH).clear();
        success = false;
    }
    SECTION("Duplicate canonical ZIP parts fail")
    {
        entries.emplace_back(MIXED_FILAMENTS_3MF_PATH, saved_json);
        success = false;
    }
    const auto modified = (temporary.path / "modified.3mf").string();
    write_zip(modified, entries);
    LoadedProject loaded;
    if (!success) {
        REQUIRE_THROWS_WITH(loaded.load(modified, (temporary.path / "import").string()),
                            Catch::Matchers::ContainsSubstring(MIXED_FILAMENTS_3MF_PATH));
        return;
    }
    REQUIRE(loaded.load(modified, (temporary.path / "import").string()));
    REQUIRE(loaded.config.opt_string("mixed_filament_definitions") == (empty ? "" : saved_legacy));
    REQUIRE(loaded.model.objects.size() == 2);
    if (!empty) {
        CHECK(loaded.model.objects[0]->config.opt_int("extruder") == 13);
        CHECK(loaded.model.objects[1]->config.opt_int("extruder") == 16);
        REQUIRE(loaded.model.objects[0]->volumes.size() == 2);
        REQUIRE(loaded.model.objects[0]->volumes[0]->config.option("extruder") != nullptr);
        CHECK(loaded.model.objects[0]->volumes[0]->config.opt_int("extruder") == 15);
        CHECK(loaded.model.objects[0]->volumes[0]->mmu_segmentation_facets.get_triangle_as_string(0) == paint);
    }
    const auto resaved = (temporary.path / "resaved.3mf").string();
    REQUIRE(save_project(resaved, loaded.model, loaded.config));
    auto rebuilt = read_zip(resaved);
    CHECK(entry(rebuilt, MIXED_FILAMENTS_3MF_PATH) == (empty ? serialize_mixed_filaments_json({}, 12) : saved_json));
}

TEST_CASE("Mixed persistence exact loading does not generate or drop auto recipes", "[MixedFilamentJSON][Persistence]")
{
    MixedFilamentManager           manager;
    const std::vector<std::string> colors(12, "#808080");
    manager.load_custom_entries(recipes, colors, MixedFilamentManager::LoadMode::Exact);
    REQUIRE(manager.mixed_filament_definitions().size() == 7);
    manager.load_custom_entries("", colors, MixedFilamentManager::LoadMode::Exact);
    CHECK(manager.mixed_filament_definitions().empty());
    auto config = recipe_config();
    config.set("mixed_filament_definitions", "1,13,1,1,50,0,m2,u101");
    REQUIRE_THROWS(mixed_filaments_from_project_config(config, 12));
}

TEST_CASE("3MF saving fails instead of discarding invalid mixed recipes", "[MixedFilamentJSON][Persistence][3mf]")
{
    TemporaryDirectory temporary;
    Model              model;
    model.set_backup_path((temporary.path / "export").string());
    auto* object = model.add_object();
    object->add_volume(make_cube(5., 5., 5.));
    object->add_instance();
    auto config = full_config();
    config.set("mixed_filament_definitions", "1,13,1,1,50,0,m2,u101");
    REQUIRE_FALSE(save_project((temporary.path / "invalid.3mf").string(), model, config));
}

TEST_CASE("Existing CubeLines project migrates to JSON without changing recipes or paint", "[MixedFilamentJSON][Persistence][3mf]")
{
    TemporaryDirectory temporary;
    LoadedProject      original;
    REQUIRE(original.load(std::string(TEST_DATA_DIR) + "/local_z/CubeLines.3mf", (temporary.path / "original").string()));
    const auto* colors = original.config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(colors != nullptr);
    const auto definitions = mixed_filaments_from_project_config(original.config, colors->values.size());
    REQUIRE_FALSE(definitions.empty());
    const auto path = (temporary.path / "migrated.3mf").string();
    REQUIRE(save_project(path, original.model, original.config));
    auto entries = read_zip(path);
    CHECK(entry(entries, MIXED_FILAMENTS_3MF_PATH) == serialize_mixed_filaments_json(definitions, colors->values.size()));

    LoadedProject restored;
    REQUIRE(restored.load(path, (temporary.path / "restored").string()));
    CHECK(restored.config.opt_string("mixed_filament_definitions") == mixed_filaments_to_legacy(definitions));
    REQUIRE(restored.model.objects.size() == original.model.objects.size());
    for (size_t i = 0; i < original.model.objects.size(); ++i) {
        const auto* before = original.model.objects[i];
        const auto* after  = restored.model.objects[i];
        CHECK(after->config.opt_int("extruder") == before->config.opt_int("extruder"));
        REQUIRE(after->volumes.size() == before->volumes.size());
        for (size_t j = 0; j < before->volumes.size(); ++j) {
            const auto* first  = before->volumes[j];
            const auto* second = after->volumes[j];
            REQUIRE(second->mesh().its.indices.size() == first->mesh().its.indices.size());
            for (size_t triangle = 0; triangle < first->mesh().its.indices.size(); ++triangle)
                CHECK(second->mmu_segmentation_facets.get_triangle_as_string(triangle) ==
                      first->mmu_segmentation_facets.get_triangle_as_string(triangle));
        }
    }
}

TEST_CASE("256 mixed recipes retain high object and painted IDs through 3MF", "[MixedManage][MixedFilamentJSON][3mf]")
{
    TemporaryDirectory temporary;
    Model              model;
    model.set_backup_path((temporary.path / "export").string());
    auto* object = model.add_object();
    auto* volume = object->add_volume(make_cube(5., 5., 5.));
    object->add_volume(make_cube(2., 2., 2.))->set_offset(Vec3d(10., 0., 0.));
    object->add_instance();
    object->config.set("extruder", 272);
    volume->config.set("extruder", 271);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(272));
    selector.set_facet(1, EnforcerBlockerType(256));
    volume->mmu_segmentation_facets.set(selector);
    auto                           config = full_config();
    const std::vector<std::string> colors(16, "#808080");
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(16, 1.75);
    std::vector<MixedFilamentDefinition> definitions(256);
    for (size_t i = 0; i < definitions.size(); ++i) {
        definitions[i].identity.stable_id      = i + 1;
        definitions[i].source.kind             = MixedFilamentSourceKind::Custom;
        definitions[i].recipe.blend.components = {{{1}, 50}, {{2}, 50}};
    }
    config.set("mixed_filament_definitions", mixed_filaments_to_legacy(definitions));
    const auto path = (temporary.path / "256-mixed.3mf").string();
    REQUIRE(save_project(path, model, config));
    LoadedProject loaded;
    REQUIRE(loaded.load(path, (temporary.path / "import").string()));
    REQUIRE(mixed_filaments_from_project_config(loaded.config, 16).size() == 256);
    REQUIRE(loaded.model.objects.size() == 1);
    CHECK(loaded.model.objects[0]->config.opt_int("extruder") == 272);
    const auto* reopened = loaded.model.objects[0]->volumes[0];
    CHECK(reopened->config.opt_int("extruder") == 271);
    REQUIRE(reopened->mmu_segmentation_facets.get_data().used_states.size() > 272);
    CHECK(reopened->mmu_segmentation_facets.get_data().used_states[272]);
    CHECK(reopened->mmu_segmentation_facets.get_data().used_states[256]);
    CHECK(reopened->mmu_segmentation_facets.get_triangle_as_string(0) == volume->mmu_segmentation_facets.get_triangle_as_string(0));

    // The OBJ matching path can also select an existing virtual recipe.
    Model                           obj_model;
    auto*                           obj_volume = obj_model.add_object()->add_volume(make_cube(5., 5., 5.));
    const std::vector<unsigned int> face_ids(obj_volume->mesh().its.indices.size(), 272);
    REQUIRE(Model::obj_import_face_color_deal(face_ids, 272u, &obj_model));
    CHECK(obj_volume->mmu_segmentation_facets.get_data().used_states[272]);
    const std::vector<unsigned int> vertex_ids(obj_volume->mesh().its.vertices.size(), 256);
    REQUIRE(Model::obj_import_vertex_color_deal(vertex_ids, 256u, &obj_model));
    CHECK(obj_volume->mmu_segmentation_facets.get_data().used_states[256]);
}
