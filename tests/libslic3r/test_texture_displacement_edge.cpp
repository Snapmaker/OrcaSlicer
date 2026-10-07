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

