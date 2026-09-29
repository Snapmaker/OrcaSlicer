#include <catch2/catch.hpp>

// nanosvg's parser implementation is compiled once for this test executable, in libslic3r_tests.cpp.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "nanosvg/nanosvg.h"


#include <libslic3r/Barcode.hpp>
#include <libslic3r/CodeEmboss.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/NSVGUtils.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
std::string row_string(const Barcode::Matrix &m, int y)
{
    std::string r;
    for (int x = 0; x < m.width; ++x)
        r += m.at(x, y) ? '1' : '0';
    return r;
}

double total_area(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

const char *LOGO_SVG = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80" viewBox="0 0 100 80">
<path d="M50 5 L95 75 L5 75 Z M50 30 L70 65 L30 65 Z" fill-rule="evenodd" fill="red"/>
<circle cx="80" cy="20" r="12" fill="blue"/>
</svg>)";
} // namespace

TEST_CASE("QR code matches the reference encoder", "[CodeEmboss]")
{
    // python-qrcode: version 1, quartile, mask 6
    const std::vector<std::string> expected = {
        "111111100001001111111", "100000101100101000001", "101110100101101011101", "101110101111101011101", "101110101101001011101",
        "100000100100101000001", "111111101010101111111", "000000001101100000000", "010111101100111011010", "101111010000111101110",
        "001010110001001100000", "101101000101100011000", "110111111110111011111", "000000001000100101000", "111111100110011001111",
        "100000101010010010111", "101110101101001000111", "101110101011100010100", "101110100100001000011", "100000101110011100110",
        "111111100101000000010",
    };
    Barcode::QrOptions options;
    options.ecc       = Barcode::QrEcc::Quartile;
    options.mask      = 6;
    options.boost_ecc = false;
    Barcode::Result r = Barcode::encode_qr("HELLO WORLD", options);
    REQUIRE(r.is_valid());
    CHECK(r.qr_version == 1);
    REQUIRE(r.matrix.width == 21);
    REQUIRE(r.matrix.height == 21);
    for (int y = 0; y < 21; ++y)
        CHECK(row_string(r.matrix, y) == expected[size_t(y)]);
}

TEST_CASE("QR code versions and capacity", "[CodeEmboss]")
{
    Barcode::QrOptions options;
    options.ecc = Barcode::QrEcc::Low;
    SECTION("byte mode UTF-8 text grows the version")
    {
        Barcode::Result r = Barcode::encode_qr(std::string(1000, 'x'), options);
        REQUIRE(r.is_valid());
        CHECK(r.qr_version > 20);
        CHECK(r.matrix.width == r.qr_version * 4 + 17);
    }
    SECTION("too long text fails")
    {
        Barcode::Result r = Barcode::encode_qr(std::string(3000, 'x'), options);
        CHECK(!r.is_valid());
        CHECK(!r.error.empty());
    }
    SECTION("empty text fails") { CHECK(!Barcode::encode_qr("").is_valid()); }
    SECTION("error correction is boosted when it fits")
    {
        options.boost_ecc = true;
        Barcode::Result r = Barcode::encode_qr("1", options);
        REQUIRE(r.is_valid());
        CHECK(r.qr_ecc == Barcode::QrEcc::High);
    }
}

TEST_CASE("Linear barcodes match the reference encoder", "[CodeEmboss]")
{
    SECTION("Code 128 switches to code set C for digits")
    {
        // python-barcode
        Barcode::Result r = Barcode::encode_code128("Hi12345678");
        REQUIRE(r.is_valid());
        CHECK(row_string(r.matrix, 0) == "11010010000110001010001000011010010111011110101100111001000101100011100010110110000101001101000100011000111010"
                                         "11");
    }
    SECTION("EAN-13 adds the check digit")
    {
        Barcode::Result r = Barcode::encode_ean13("590123412345");
        REQUIRE(r.is_valid());
        CHECK(r.encoded_text == "5901234123457");
        CHECK(row_string(r.matrix, 0) ==
              "10100010110100111011001100100110111101001110101010110011011011001000010101110010011101000100101");
        CHECK(!Barcode::encode_ean13("5901234123458").is_valid()); // wrong check digit
    }
    SECTION("UPC-A is EAN-13 with leading zero")
    {
        Barcode::Result upc = Barcode::encode_upca("03600029145");
        Barcode::Result ean = Barcode::encode_ean13("003600029145");
        REQUIRE(upc.is_valid());
        CHECK(upc.encoded_text == "036000291452");
        CHECK(upc.matrix.dark == ean.matrix.dark);
    }
    SECTION("Code 39 upper case")
    {
        Barcode::Result r = Barcode::encode_code39("abc-1");
        REQUIRE(r.is_valid());
        CHECK(r.encoded_text == "ABC-1");
        CHECK(!Barcode::encode_code39("a*b").is_valid());
    }
}

TEST_CASE("Code parts share the center and cover the code", "[CodeEmboss]")
{
    CodeEmbossParams params;
    params.text        = "https://github.com/aceRage/EdgeSlicer";
    params.ecc         = Barcode::QrEcc::High;
    params.module_size = 0.8;
    params.group_id    = "0123456789abcdef";

    ExPolygons logo = load_code_logo(LOGO_SVG);
    REQUIRE(!logo.empty());
    params.has_logo = true;

    for (CodeLogoClear clear : {CodeLogoClear::Outline, CodeLogoClear::Square, CodeLogoClear::Circle}) {
        params.logo_clear       = clear;
        CodeEmbossResult result = create_code_emboss(params, &logo);
        REQUIRE(result.is_valid());
        REQUIRE(result.parts.size() == 3);
        CHECK(result.cleared_modules > 0);

        double sum = 0.;
        for (const CodeEmbossPart &part : result.parts) {
            // all parts have the same center, so volumes with the same transformation are aligned
            BoundingBox bb = get_extents(part.shape);
            Vec2d       c  = unscaled(bb.center());
            CHECK_THAT(c.x(), WithinAbs(result.width / 2., 1e-3));
            CHECK_THAT(c.y(), WithinAbs(result.height / 2., 1e-3));
            sum += total_area(part.shape);

            // SVG contains the same shape
            NSVGimage_ptr image = nsvgParse(part.svg);
            REQUIRE(image != nullptr);
            ExPolygonsWithIds loaded = create_shape_with_ids(*image, NSVGLineParams{1e6});
            double            loaded_area = 0.;
            for (const ExPolygonsWithId &s : loaded)
                loaded_area += total_area(s.expoly);
            // modules are exact, logo vertices (logo and holes in light part) are rounded to micrometers in SVG
            double tolerance = part.role == CodePartRole::Dark ? 1e-6 : 1e-3;
            CHECK_THAT(loaded_area, WithinRel(total_area(part.shape), tolerance));

            std::optional<CodeEmbossMeta> meta = read_code_emboss_meta(part.svg);
            REQUIRE(meta.has_value());
            CHECK(meta->role == part.role);
            CHECK(meta->params.group_id == params.group_id);
            CHECK(meta->params.text == params.text);
            CHECK(meta->params.logo_clear == clear);
        }
        // parts do not overlap and fill the whole code
        CHECK_THAT(sum, WithinRel(result.width * result.height, 1e-6));
    }
}

TEST_CASE("Barcode parts without light part", "[CodeEmboss]")
{
    CodeEmbossParams params;
    params.symbology  = Barcode::Symbology::Code128;
    params.text       = "EDGE-12345678";
    params.light_part = false;
    params.quiet_zone = 10;
    CodeEmbossResult result = create_code_emboss(params);
    REQUIRE(result.is_valid());
    REQUIRE(result.parts.size() == 1);
    CHECK(result.parts.front().role == CodePartRole::Dark);
    CHECK_THAT(result.height, WithinAbs(params.bar_height + 2 * 3 * params.module_size, 1e-6));
}

TEST_CASE("Code metadata survives special characters", "[CodeEmboss]")
{
    CodeEmbossMeta meta;
    meta.params.text     = "a<b>&c \"quoted\" -- é日本";
    meta.params.group_id = "g";
    meta.role            = CodePartRole::Logo;
    CodeEmbossParams params = meta.params;
    params.light_part       = true;
    CodeEmbossResult result = create_code_emboss(params);
    REQUIRE(result.is_valid());
    std::optional<CodeEmbossMeta> loaded = read_code_emboss_meta(result.parts.front().svg);
    REQUIRE(loaded.has_value());
    CHECK(loaded->params.text == meta.params.text);
    CHECK(!read_code_emboss_meta("<svg><metadata id=\"edgeslicer-code\"></metadata></svg>").has_value());
    CHECK(!read_code_emboss_meta("<svg/>").has_value());
}
