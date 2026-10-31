#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/Thumbnails.hpp"

#include <algorithm>
#include <string>

using namespace Slic3r;

TEST_CASE("Snapmaker thumbnails use one unwrapped PNG data URL", "[Thumbnails][SnapmakerHeader]")
{
    ThumbnailData data;
    data.set(8, 8);
    std::fill(data.pixels.begin(), data.pixels.end(), 255);
    unsigned int calls = 0;
    ThumbnailsGeneratorCallback generator = [&data, &calls](const ThumbnailsParams&) {
        ++calls;
        return ThumbnailsList{data};
    };
    std::string output;
    unsigned int cancellation_checks = 0;
    const GCodeThumbnails::GCodeThumbnailDefinitionsList definitions = {
        {GCodeThumbnailsFormat::JPG, Vec2d(8, 8)}, {GCodeThumbnailsFormat::PNG, Vec2d(16, 16)}};
    GCodeThumbnails::export_thumbnails_to_file(
        generator, 0, definitions, [&output](const char* text) { output += text; }, [&cancellation_checks]() { ++cancellation_checks; }, true);

    const auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
    REQUIRE(compressed->data != nullptr);
    std::string encoded(boost::beast::detail::base64::encoded_size(compressed->size), '\0');
    encoded.resize(boost::beast::detail::base64::encode(encoded.data(), compressed->data, compressed->size));
    CHECK(output == ";thumbnail: data:image/png;base64," + encoded + "\n");
    CHECK(calls == 1);
    CHECK(cancellation_checks == 1);
}

TEST_CASE("Standard thumbnails retain the Orca block format", "[Thumbnails]")
{
    ThumbnailData data;
    data.set(8, 8);
    std::fill(data.pixels.begin(), data.pixels.end(), 255);
    ThumbnailsGeneratorCallback generator = [&data](const ThumbnailsParams&) { return ThumbnailsList{data}; };
    std::string output;
    GCodeThumbnails::export_thumbnails_to_file(
        generator, 0, {{GCodeThumbnailsFormat::PNG, Vec2d(8, 8)}}, [&output](const char* text) { output += text; }, []() {});
    CHECK(output.find("; THUMBNAIL_BLOCK_START\n") == 0);
    CHECK(output.find("; thumbnail begin 8x8 ") != std::string::npos);
    CHECK(output.find("; thumbnail end\n; THUMBNAIL_BLOCK_END\n") != std::string::npos);
    CHECK(output.find(";thumbnail:") == std::string::npos);
}

TEST_CASE("Snapmaker headers allow disabled thumbnail generation", "[Thumbnails][SnapmakerHeader]")
{
    ThumbnailsGeneratorCallback generator;
    std::string output;
    GCodeThumbnails::export_thumbnails_to_file(
        generator, 0, {{GCodeThumbnailsFormat::PNG, Vec2d(8, 8)}}, [&output](const char* text) { output += text; }, []() {}, true);
    CHECK(output.empty());
}
