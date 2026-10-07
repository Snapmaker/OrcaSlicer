// EdgeSlicer-specific checks around the texture displacement port (OrcaSlicer #14662 + #16148):
// object ids of the eight per-part paint masks (undo/redo and "; model label id"), the undo/redo
// payload, the mesh-change guards, the colour gate, and what the 3MF writer does for our own
// projects versus "Export Bambu 3MF".
#include <catch2/catch.hpp>

#include <cstdlib>
#include <set>
#include <sstream>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cereal/archives/binary.hpp>
#include <cereal/types/array.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>

#include "libslic3r/BRep/CadBody.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TextureDisplacementGuards.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

using namespace Slic3r;

namespace {

constexpr size_t SECONDARY_ID_BASE = size_t(1) << 62;

// Paints every facet of `volume` into texture displacement slot `slot`.
void paint_all(ModelVolume &volume, int slot)
{
    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < int(volume.mesh().its.indices.size()); ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    REQUIRE(volume.texture_displacement_facet(slot).set(selector));
}

TextureDisplacementLayer make_layer(int slot)
{
    TextureDisplacementLayer layer;
    layer.slot          = slot;
    layer.name          = "layer " + std::to_string(slot);
    layer.path          = "C:/textures/knurl.png";
    layer.depth_mm      = 0.6f;
    layer.tiling_scale  = 3.f;
    layer.rotation_deg  = 15.f;
    layer.color_enabled = false;
    // Image bytes are stored and restored verbatim; they need not decode for these checks.
    std::vector<unsigned char> bytes(128);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(i * 7);
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    return layer;
}

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
                                         ("snorca_tests_texdisp_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(root);
    Slic3r::set_temporary_dir(root.string());
    return (root / name).string();
}

std::vector<std::string> zip_entries(const std::string &zip_path)
{
    std::vector<std::string> out;
    mz_zip_archive           archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return out;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&archive, i, &stat))
            out.emplace_back(stat.m_filename);
    }
    close_zip_reader(&archive);
    return out;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

bool any_entry_under(const std::vector<std::string> &entries, const std::string &prefix)
{
    for (const std::string &e : entries)
        if (e.rfind(prefix, 0) == 0)
            return true;
    return false;
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // As test_bambu_3mf_export.cpp: coEnums of a static config have no keys_map; rebuild them.
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

// One object, one painted part with two unbaked texture layers, one instance.
void build_textured_model(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "textured_cube";
    ModelVolume *part   = object->add_volume(make_cube(20., 20., 20.));
    part->name          = "cube";
    object->add_instance();
    object->ensure_on_bed();
    paint_all(*part, 0);
    paint_all(*part, 3);
    part->texture_displacement_layers  = { make_layer(0), make_layer(3) };
    part->texture_displacement_options.color_despeckle = 4;
}

bool store(const std::string &path, Model &model, DynamicPrintConfig &cfg, BambuExport::Report *bambu_report)
{
    StoreParams sp;
    sp.path         = path.c_str();
    sp.model        = &model;
    sp.config       = &cfg;
    sp.strategy     = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat = bambu_report != nullptr;
    sp.bambu_report = bambu_report;
    return store_bbs_3mf(sp);
}

bool load(const std::string &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool ok = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *p : presets)
        delete p;
    return ok;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Object ids: the masks must not shift "; model label id" (ModelInstance::id()), and must stay
// distinct so the undo/redo stack (which keys FacetsAnnotation snapshots by ObjectID) works.
// ------------------------------------------------------------------------------------------------

TEST_CASE("Texture displacement masks take their ids from the secondary range", "[TextureDisplacementEdge]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *v1     = object->add_volume(make_cube(10., 10., 10.));
    ModelVolume *v2     = object->add_volume(make_cube(10., 10., 10.));

    std::set<size_t> td_ids;
    for (const ModelVolume *v : { v1, v2 })
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
            const ObjectID id = v->texture_displacement_facet(slot).id();
            CHECK(id.valid());
            CHECK(id.id > SECONDARY_ID_BASE);
            td_ids.insert(id.id);
        }
    CHECK(td_ids.size() == 2 * TEXTURE_DISPLACEMENT_MAX_LAYERS);

    // The main counter advances only by the ids a part held before the port: the part, its config
    // and the five upstream paint channels (support, seam, colour, fuzzy skin, exterior). A ninth
    // ObjectBase member taken from the main counter would make this gap - and the label ids of every
    // later instance - grow.
    const std::vector<ObjectID> main_ids = { v1->id(), v1->config.id(), v1->supported_facets.id(), v1->seam_facets.id(),
                                             v1->mmu_segmentation_facets.id(), v1->fuzzy_skin_facets.id(),
                                             v1->exterior_facets.id() };
    for (const ObjectID &id : main_ids)
        CHECK(id.id < SECONDARY_ID_BASE);
    CHECK(v2->id().id - v1->id().id == main_ids.size());

    // New ids (copy / paste, undo-stack clones) stay in the secondary range and change.
    const ObjectID before = v1->texture_displacement_facet(5).id();
    v1->set_new_unique_id();
    CHECK(v1->texture_displacement_facet(5).id() != before);
    CHECK(v1->texture_displacement_facet(5).id().id > SECONDARY_ID_BASE);
    CHECK(v1->id().id < SECONDARY_ID_BASE);
}

TEST_CASE("An instance's label id is the same with and without the port's masks", "[TextureDisplacementEdge]")
{
    // The instance id grows by the main-range ids of everything created before it. Two parts more
    // shift it by exactly two parts' worth of main ids - not by 2 x (7 + 8).
    auto instance_id_after = [](int parts) {
        Model        model;
        ModelObject *object = model.add_object();
        for (int i = 0; i < parts; ++i)
            object->add_volume(make_cube(10., 10., 10.));
        const size_t first = object->id().id;
        return object->add_instance()->id().id - first;
    };
    CHECK(instance_id_after(3) - instance_id_after(1) == 2 * 7);
}

TEST_CASE("Copies and clones keep texture layers and masks", "[TextureDisplacementEdge]")
{
    Model model;
    build_textured_model(model);
    const ModelVolume &src = *model.objects.front()->volumes.front();

    SECTION("a Model copy (what an undo snapshot restores) keeps ids and content") {
        Model              copy(model);
        const ModelVolume &dst = *copy.objects.front()->volumes.front();
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
            CHECK(dst.texture_displacement_facet(slot).id() == src.texture_displacement_facet(slot).id());
            CHECK(dst.texture_displacement_facet(slot).equals(src.texture_displacement_facet(slot)));
        }
        REQUIRE(dst.texture_displacement_layers.size() == 2);
        CHECK(dst.texture_displacement_layers[1].slot == 3);
        CHECK(*dst.texture_displacement_layers[1].image_data == *src.texture_displacement_layers[1].image_data);
        CHECK(dst.texture_displacement_options.color_despeckle == 4);
    }

    SECTION("a clone (copy / paste) gets new secondary ids and the same content") {
        ModelObject       *clone = model.add_object(*model.objects.front());
        const ModelVolume &dst   = *clone->volumes.front();
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot) {
            CHECK(dst.texture_displacement_facet(slot).id() != src.texture_displacement_facet(slot).id());
            CHECK(dst.texture_displacement_facet(slot).id().id > SECONDARY_ID_BASE);
            CHECK(dst.texture_displacement_facet(slot).equals(src.texture_displacement_facet(slot)));
        }
        CHECK(dst.texture_displacement_layers.size() == 2);
        CHECK(dst.is_texture_displacement_painted());
    }
}

TEST_CASE("The undo/redo payload of texture layers and options round-trips", "[TextureDisplacementEdge]")
{
    // ModelVolume::save()/load() hand exactly these two members to the Undo / Redo stack's cereal
    // archive (the masks go through save_by_value like the other paint channels).
    std::vector<TextureDisplacementLayer> layers = { make_layer(1), make_layer(6) };
    layers[1].color_enabled     = true;
    layers[1].projection_method = TextureProjectionMethod::Cylindrical;
    layers[1].tile_enabled      = false;
    layers[1].lscm_seam_edges   = { { 1, 2 }, { 3, 4 } };
    TextureDisplacementOptions options;
    options.smooth_enabled  = true;
    options.color_mix_mode  = ColorMixMode::XYDither;
    options.color_despeckle = 5;

    std::stringstream stream;
    {
        cereal::BinaryOutputArchive ar(stream);
        ar(layers, options);
    }
    std::vector<TextureDisplacementLayer> read_layers;
    TextureDisplacementOptions            read_options;
    {
        cereal::BinaryInputArchive ar(stream);
        ar(read_layers, read_options);
    }
    REQUIRE(read_layers.size() == 2);
    CHECK(read_layers[0].slot == 1);
    CHECK(read_layers[1].slot == 6);
    CHECK(read_layers[1].color_enabled);
    CHECK(read_layers[1].projection_method == TextureProjectionMethod::Cylindrical);
    CHECK(!read_layers[1].tile_enabled);
    CHECK(read_layers[1].lscm_seam_edges == layers[1].lscm_seam_edges);
    CHECK(read_layers[1].depth_mm == Approx(0.6f));
    REQUIRE(read_layers[1].image_data);
    CHECK(*read_layers[1].image_data == *layers[1].image_data);
    CHECK(read_options.smooth_enabled);
    CHECK(read_options.color_mix_mode == ColorMixMode::XYDither);
    CHECK(read_options.color_despeckle == 5);
}

// ------------------------------------------------------------------------------------------------
// Guards
// ------------------------------------------------------------------------------------------------

TEST_CASE("A mesh change detaches text, SVG and CAD recipes but keeps texture data", "[TextureDisplacementEdge]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *part   = object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    paint_all(*part, 2);
    part->texture_displacement_layers = { make_layer(2) };

    SECTION("plain part: nothing to detach") {
        const TextureDisplacementMeshRecipes r = texture_displacement_mesh_recipes(*part);
        CHECK(!r.any());
        CHECK(!detach_mesh_recipes_for_texture_displacement(*part).any());
    }

    SECTION("editable text becomes a plain part") {
        part->text_configuration = TextConfiguration();
        part->emboss_shape       = EmbossShape();
        REQUIRE(part->is_text());
        const TextureDisplacementMeshRecipes r = texture_displacement_mesh_recipes(*part);
        CHECK(r.text);
        CHECK(!r.svg); // a text part's emboss shape is the text's, not an SVG
        CHECK(r.detaches());
        const TextureDisplacementMeshRecipes found = detach_mesh_recipes_for_texture_displacement(*part);
        CHECK(found.text);
        CHECK(!part->is_text());
        CHECK(!part->is_svg());
        CHECK(!part->emboss_shape.has_value());
    }

    SECTION("editable SVG becomes a plain part") {
        part->emboss_shape = EmbossShape();
        REQUIRE(part->is_svg());
        CHECK(texture_displacement_mesh_recipes(*part).svg);
        detach_mesh_recipes_for_texture_displacement(*part);
        CHECK(!part->is_svg());
    }

    SECTION("an exact CAD body that matches the mesh is reported and dropped") {
        auto body  = std::make_shared<BRep::CadBody>();
        body->brep = "not a real shape";
        body->mesh = BRep::mesh_fingerprint(part->mesh().its);
        part->cad_body = body;
        CHECK(texture_displacement_mesh_recipes(*part).cad_body);
        detach_mesh_recipes_for_texture_displacement(*part);
        CHECK(part->cad_body == nullptr);
    }

    SECTION("a stale CAD body is not reported but still dropped") {
        auto body  = std::make_shared<BRep::CadBody>();
        body->brep = "not a real shape";
        body->mesh = BRep::mesh_fingerprint(make_cube(5., 5., 5.).its);
        part->cad_body = body;
        CHECK(!texture_displacement_mesh_recipes(*part).cad_body);
        detach_mesh_recipes_for_texture_displacement(*part);
        CHECK(part->cad_body == nullptr);
    }

    // Whatever was detached, the texture layers and masks are untouched.
    CHECK(part->texture_displacement_layers.size() == 1);
    CHECK(part->is_texture_displacement_painted());
}

TEST_CASE("Texture colour is offered only without mixed filaments", "[TextureDisplacementEdge]")
{
    CHECK(texture_displacement_colors_allowed(0));
    CHECK(!texture_displacement_colors_allowed(1));
    CHECK(!texture_displacement_colors_allowed(12));
    // And it is off by default on a new layer.
    CHECK(!TextureDisplacementLayer().color_enabled);
}

// ------------------------------------------------------------------------------------------------
// 3MF
// ------------------------------------------------------------------------------------------------

TEST_CASE("An EdgeSlicer project keeps unbaked texture layers; Export Bambu 3MF leaves them out", "[TextureDisplacementEdge][3mf]")
{
    Model model;
    build_textured_model(model);
    DynamicPrintConfig cfg = project_config();

    SECTION("our own project") {
        const std::string path = temp_file("texdisp_project.3mf");
        REQUIRE(store(path, model, cfg, nullptr));
        const std::vector<std::string> entries = zip_entries(path);
        CHECK(any_entry_under(entries, "Metadata/texture_displacement/"));

        Model loaded;
        REQUIRE(load(path, loaded));
        REQUIRE(loaded.objects.size() == 1);
        const ModelVolume &v = *loaded.objects.front()->volumes.front();
        REQUIRE(v.texture_displacement_layers.size() == 2);
        CHECK(v.texture_displacement_layers[1].slot == 3);
        REQUIRE(v.texture_displacement_layers[1].image_data);
        CHECK(*v.texture_displacement_layers[1].image_data == *model.objects.front()->volumes.front()->texture_displacement_layers[1].image_data);
        CHECK(v.texture_displacement_options.color_despeckle == 4);
        CHECK(!v.texture_displacement_facet(0).empty());
        CHECK(!v.texture_displacement_facet(3).empty());
        CHECK(v.texture_displacement_facet(1).empty());
        // For the hand check that an older EdgeSlicer opens such a project (see the PR): keep a copy.
        if (const char *keep = std::getenv("EDGE_TEXDISP_KEEP_3MF"); keep != nullptr && *keep != 0)
            boost::filesystem::copy_file(path, keep, boost::filesystem::copy_option::overwrite_if_exists);
        boost::filesystem::remove(path);
    }

    SECTION("Export Bambu 3MF") {
        const std::string   path = temp_file("texdisp_bambu.3mf");
        BambuExport::Report report;
        REQUIRE(store(path, model, cfg, &report));
        const std::vector<std::string> entries = zip_entries(path);
        CHECK(!any_entry_under(entries, "Metadata/texture_displacement/"));
        for (const std::string &e : entries) {
            if (e.size() < 6 || e.compare(e.size() - 6, 6, ".model") != 0)
                continue;
            INFO(e);
            CHECK(zip_entry(path, e).find("paint_texture_") == std::string::npos);
        }
        CHECK(zip_entry(path, "Metadata/model_settings.config").find("texture_displacement") == std::string::npos);

        Model loaded;
        REQUIRE(load(path, loaded));
        const ModelVolume &v = *loaded.objects.front()->volumes.front();
        CHECK(v.texture_displacement_layers.empty());
        CHECK(!v.is_texture_displacement_painted());
        boost::filesystem::remove(path);
    }
}

TEST_CASE("A build without the feature drops the texture metadata key quietly", "[TextureDisplacementEdge][3mf]")
{
    // An older EdgeSlicer reader does not know the "texture_displacement" volume metadata key and
    // hands it to the part's config, like any per-part setting. That must neither throw nor leave a
    // key behind (it is no print setting); the paint_texture_N triangle attributes and the files
    // under Metadata/texture_displacement/ are simply never read.
    REQUIRE(!print_config_def.has("texture_displacement"));
    ModelConfig               config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    CHECK_NOTHROW(config.set_deserialize("texture_displacement", "Metadata/texture_displacement/12.json", ctxt));
    CHECK(!config.has("texture_displacement"));
}
