#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/GCode/FirstLayerReference.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include <png.h>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <sstream>

using namespace Slic3r;
using Catch::Approx;

namespace {
DynamicPrintConfig reference_config()
{
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_key_value("printable_area", new ConfigOptionPoints({{0, 0}, {100, 0}, {100, 50}, {0, 50}}));
    return config;
}

void segment(
    GCodeProcessorResult& result, Vec3f a, Vec3f b, unsigned int layer = 1, ExtrusionRole role = erExternalPerimeter, float width = 2.f)
{
    GCodeProcessorResult::MoveVertex start, end;
    start.position     = a;
    start.type         = EMoveType::Travel;
    end.position       = b;
    end.type           = EMoveType::Extrude;
    end.extrusion_role = role;
    end.width          = width;
    end.delta_extruder = 1;
    end.layer_id       = layer;
    result.moves.push_back(start);
    result.moves.push_back(end);
}

unsigned char sample(const FirstLayerReference& ref, double x, double y)
{
    const auto& m   = ref.metadata.at("pixel_to_bed_mm");
    const int   col = int(std::lround((x - m[0][2].get<double>()) / m[0][0].get<double>()));
    const int   row = int(std::lround((y - m[1][2].get<double>()) / m[1][1].get<double>()));
    REQUIRE(col >= 0);
    REQUIRE(row >= 0);
    REQUIRE(col < int(ref.image.width));
    REQUIRE(row < int(ref.image.height));
    return ref.image.pixels[4 * ((ref.image.height - 1 - row) * ref.image.width + col)];
}

struct TempDirectory
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-first-layer-%%%%-%%%%");
    TempDirectory() { boost::filesystem::create_directories(path); }
    ~TempDirectory()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

std::string archive_text(mz_zip_archive& archive, const std::string& path)
{
    size_t size = 0;
    void*  data = mz_zip_reader_extract_file_to_heap(&archive, path.c_str(), &size, 0);
    REQUIRE(data != nullptr);
    std::string text(static_cast<char*>(data), size);
    mz_free(data);
    return text;
}

std::string gcode(float x)
{
    // Use the same reserved tags as the generator, for both supported tag styles.
    return "G90\nM83\nG1 X" + std::to_string(x) + " Y10 Z0.2 F1200\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) +
           "\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) + "Outer wall\n;" +
           GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + "0.4\n;" +
           GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + "0.2\nG1 Y30 E1\n;" +
           GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\nG1 X80 Y40 Z0.4\nG1 X90 E1\n";
}
} // namespace

TEST_CASE("First layer reference selects slicer layer tags and extrusion roles", "[first_layer_reference]")
{
    GCodeProcessorResult result;
    segment(result, {5, 5, 0.2f}, {20, 5, 0.2f}, 0, erCustom);
    segment(result, {10, 10, 0.3f}, {30, 10, 0.3f});
    segment(result, {10, 20, 0.3f}, {30, 20, 0.3f}, 1, erSupportMaterial);
    segment(result, {10, 30, 0.3f}, {30, 30, 0.3f}, 1, erBrim);
    segment(result, {10, 40, 0.5f}, {30, 40, 0.5f}, 2);
    auto ref = render_first_layer_reference(result, reference_config(), 3, "digest", 400);
    REQUIRE(ref.metadata.at("status") == "ready");
    REQUIRE(ref.metadata.at("plate_id") == 3);
    REQUIRE(ref.metadata.at("gcode_md5") == "digest");
    REQUIRE(ref.image.width == 400);
    REQUIRE(ref.image.height == 200);
    REQUIRE(sample(ref, 20, 5) == 0);
    REQUIRE(sample(ref, 20, 10) == 255);
    REQUIRE(sample(ref, 20, 20) == 255);
    REQUIRE(sample(ref, 20, 30) == 255);
    REQUIRE(sample(ref, 20, 40) == 0);
    REQUIRE(ref.metadata.at("z_range_mm")[0].get<double>() == Approx(0.3));
}

TEST_CASE("First layer reference preserves holes and follows arc interpolation", "[first_layer_reference]")
{
    GCodeProcessorResult result;
    segment(result, {10, 10, .2f}, {30, 10, .2f});
    segment(result, {30, 10, .2f}, {30, 30, .2f});
    segment(result, {30, 30, .2f}, {10, 30, .2f});
    segment(result, {10, 30, .2f}, {10, 10, .2f});
    segment(result, {50, 10, .2f}, {70, 10, .2f});
    auto& arc                = result.moves.back();
    arc.move_path_type       = EMovePathType::Arc_move_cw;
    arc.interpolation_points = {{60, 20, .2f}, {70, 10, .2f}};
    auto ref                 = render_first_layer_reference(result, reference_config(), 1, "", 400);
    REQUIRE(sample(ref, 20, 20) == 0);
    REQUIRE(sample(ref, 10, 20) == 255);
    REQUIRE(sample(ref, 60, 20) > 0);
    REQUIRE(sample(ref, 60, 10) == 0); // no straight chord across the arc
}

TEST_CASE("First layer reference uses printer coordinates and clips a non-rectangular bed", "[first_layer_reference]")
{
    auto config = reference_config();
    config.set_key_value("printable_area", new ConfigOptionPoints({{-50, -25}, {50, -25}, {-50, 25}}));
    GCodeProcessorResult result;
    segment(result, {-40, -10, .2f}, {40, -10, .2f});
    auto ref = render_first_layer_reference(result, config, 1, "", 400);
    REQUIRE(sample(ref, -30, -10) == 255);
    REQUIRE(sample(ref, 30, -10) == 0);
    const auto& m = ref.metadata.at("pixel_to_bed_mm");
    REQUIRE(m[0][0].get<double>() == Approx(.25));
    REQUIRE(m[1][1].get<double>() == Approx(-.25));
    REQUIRE(m[0][2].get<double>() == Approx(-49.875));
    REQUIRE(m[1][2].get<double>() == Approx(24.875));
}

TEST_CASE("First layer reference reports unsupported or empty inputs without a misleading image", "[first_layer_reference]")
{
    auto                 config = reference_config();
    GCodeProcessorResult result;
    REQUIRE_FALSE(render_first_layer_reference(result, config, 1, "").image.is_valid());
    segment(result, {10, 10, .2f}, {30, 10, .2f});
    REQUIRE_FALSE(render_first_layer_reference(result, config, 1, "", 0).image.is_valid());
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    auto ref = render_first_layer_reference(result, config, 1, "");
    REQUIRE_FALSE(ref.image.is_valid());
    REQUIRE(ref.metadata.at("status") == "unsupported_sequential_print");
}

TEST_CASE("Slicer layer ids survive G-code processor finalization", "[first_layer_reference]")
{
    for (const bool bbl_tags : {false, true}) {
        const bool original            = GCodeProcessor::s_IsBBLPrinter;
        GCodeProcessor::s_IsBBLPrinter = bbl_tags;
        const std::string text         = gcode(10);
        GCodeProcessor::s_IsBBLPrinter = !bbl_tags;
        GCodeProcessor processor;
        PrintConfig    processor_config;
        processor_config.apply(reference_config(), true);
        processor.apply_config(processor_config);
        processor.initialize("first_layer_test.gcode");
        processor.process_buffer(text);
        processor.finalize(false);
        GCodeProcessor::s_IsBBLPrinter = original;
        auto ref                       = render_first_layer_reference(processor.get_result(), reference_config(), 1, "", 400);
        REQUIRE(ref.metadata.at("status") == "ready");
        REQUIRE(ref.metadata.at("layer_id") == 1);
        REQUIRE(sample(ref, 10, 20) > 0);
        REQUIRE(sample(ref, 85, 40) == 0);
    }
}

TEST_CASE("Sliced 3MF exports references per actual plate and rebuilds them from final G-code", "[first_layer_reference][3mf]")
{
    TempDirectory temp;
    auto          config = reference_config();
    Model         model;
    model.set_backup_path((temp.path / "backup").string());
    PlateData plate1, plate3, invalid;
    plate1.plate_index  = 0;
    plate3.plate_index  = 2;
    invalid.plate_index = 1;
    ThumbnailData thumbnail;
    thumbnail.set(512, 512);
    const std::string source1 = (temp.path / "one.gcode").string();
    const std::string source3 = (temp.path / "three.gcode").string();
    {
        boost::nowide::ofstream f(source1);
        f << gcode(10);
    }
    {
        boost::nowide::ofstream f(source3);
        f << gcode(30);
    }
    auto export_file = [&](const std::string& name, bool with_gcode = true) {
        plate1.gcode_file      = source1;
        plate3.gcode_file      = source3;
        plate1.is_sliced_valid = plate3.is_sliced_valid = true;
        const std::string path                          = (temp.path / name).string();
        StoreParams       params;
        params.path            = path.c_str();
        params.model           = &model;
        params.config          = &config;
        params.thumbnail_data  = {&thumbnail, &thumbnail, &thumbnail};
        params.plate_data_list = {&plate1, &invalid, &plate3};
        params.strategy        = SaveStrategy::Silence | SaveStrategy::SkipModel;
        if (with_gcode)
            params.strategy = params.strategy | SaveStrategy::WithGcode;
        REQUIRE(store_bbs_3mf(params));
        return path;
    };
    const std::string path = export_file("first.gcode.3mf");
    mz_zip_archive    archive;
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, path.c_str(), 0));
    const auto meta1 = nlohmann::json::parse(archive_text(archive, "Metadata/first_layer_1.json"));
    const auto meta3 = nlohmann::json::parse(archive_text(archive, "Metadata/first_layer_3.json"));
    REQUIRE(meta1.at("status") == "ready");
    REQUIRE(meta3.at("plate_id") == 3);
    REQUIRE(meta1.at("gcode_file") == "Metadata/plate_1.gcode");
    REQUIRE(meta1.at("gcode_md5") == archive_text(archive, "Metadata/plate_1.gcode.md5"));
    REQUIRE(mz_zip_reader_locate_file(&archive, "Metadata/first_layer_2.png", nullptr, 0) < 0);
    const std::string png_data = archive_text(archive, "Metadata/first_layer_1.png");
    png_image         image{};
    image.version = PNG_IMAGE_VERSION;
    REQUIRE(png_image_begin_read_from_memory(&image, png_data.data(), png_data.size()));
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> decoded(PNG_IMAGE_SIZE(image));
    REQUIRE(png_image_finish_read(&image, nullptr, decoded.data(), 0, nullptr));
    REQUIRE(image.width == 2048);
    REQUIRE(image.height == 1024);
    // libpng's simplified API returns PNG rows top-down, independently of
    // Orca's bottom-up thumbnail decoder. Y=10 must be near the bottom.
    REQUIRE(decoded[(819 * image.width + 205) * 4] > 0);
    REQUIRE(decoded[(205 * image.width + 205) * 4] == 0);
    png_image_free(&image);
    const std::string model_config = archive_text(archive, "Metadata/model_settings.config");
    REQUIRE(model_config.find("first_layer_file") != std::string::npos);
    REQUIRE(archive_text(archive, "[Content_Types].xml").find("application/json") != std::string::npos);
    mz_zip_reader_end(&archive);

    // Exercise the sliced-file importer, including the resource links in PlateData.
    boost::nowide::ifstream sliced_file(path, std::ios::binary);
    DynamicPrintConfig      imported_config;
    Model                   imported_model;
    imported_model.set_backup_path((temp.path / "import-backup").string());
    PlateDataPtrs imported_plates;
    Semver        version;
    REQUIRE(load_gcode_3mf_from_stream(sliced_file, &imported_config, &imported_model, &imported_plates, &version));
    REQUIRE(imported_plates.size() == 3);
    CHECK(imported_plates[0]->first_layer_file == "Metadata/first_layer_1.png");
    CHECK(imported_plates[2]->first_layer_metadata_file == "Metadata/first_layer_3.json");
    for (PlateData* plate : imported_plates)
        delete plate;
    imported_plates.clear();

    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
    std::vector<Preset*>      presets;
    bool                      is_orca = false;
    REQUIRE(load_bbs_3mf(path.c_str(), &imported_config, &substitutions, &imported_model, &imported_plates, &presets, &is_orca, &version,
                         nullptr, LoadStrategy::LoadConfig | LoadStrategy::Silence));
    REQUIRE(imported_plates.size() == 3);
    REQUIRE(boost::filesystem::exists(imported_plates[0]->first_layer_file));
    REQUIRE(boost::filesystem::exists(imported_plates[2]->first_layer_metadata_file));
    const std::string roundtrip_path = (temp.path / "roundtrip.gcode.3mf").string();
    // As in the GUI load/validate/store flow, only existing sliced toolpaths
    // are marked valid for export; importing metadata alone does not do that.
    for (PlateData* plate : imported_plates)
        plate->is_sliced_valid = !plate->gcode_file.empty() && boost::filesystem::exists(plate->gcode_file);
    StoreParams roundtrip;
    roundtrip.path            = roundtrip_path.c_str();
    roundtrip.model           = &imported_model;
    roundtrip.config          = &imported_config;
    roundtrip.plate_data_list = imported_plates;
    roundtrip.strategy        = SaveStrategy::Silence | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(roundtrip));
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, roundtrip_path.c_str(), 0));
    REQUIRE(archive_text(archive, "Metadata/first_layer_1.png") == png_data);
    mz_zip_reader_end(&archive);
    release_PlateData_list(imported_plates);
    for (Preset* preset : presets)
        delete preset;

    // External post-processing changes the final G-code before another export.
    {
        boost::nowide::ofstream f(source1);
        f << gcode(40);
    }
    const std::string changed = export_file("changed.gcode.3mf");
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, changed.c_str(), 0));
    const auto changed_meta = nlohmann::json::parse(archive_text(archive, "Metadata/first_layer_1.json"));
    REQUIRE(changed_meta.at("gcode_md5") != meta1.at("gcode_md5"));
    REQUIRE(archive_text(archive, "Metadata/first_layer_1.png") != png_data);
    mz_zip_reader_end(&archive);

    const std::string project = export_file("project.3mf", false);
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, project.c_str(), 0));
    REQUIRE(mz_zip_reader_locate_file(&archive, "Metadata/first_layer_1.json", nullptr, 0) < 0);
    mz_zip_reader_end(&archive);

    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
    const std::string sequential = export_file("sequential.gcode.3mf");
    mz_zip_zero_struct(&archive);
    REQUIRE(mz_zip_reader_init_file(&archive, sequential.c_str(), 0));
    REQUIRE(nlohmann::json::parse(archive_text(archive, "Metadata/first_layer_1.json")).at("status") == "unsupported_sequential_print");
    REQUIRE(mz_zip_reader_locate_file(&archive, "Metadata/first_layer_1.png", nullptr, 0) < 0);
    mz_zip_reader_end(&archive);
}
