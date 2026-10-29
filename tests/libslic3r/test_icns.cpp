#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

std::vector<uint8_t> read_resource_file(const std::string &filename)
{
    std::ifstream ifs(resources_dir() + "/" + filename, std::ios::binary);
    REQUIRE(ifs.good());
    return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
}

uint32_t read_be32(const std::vector<uint8_t> &data, size_t offset)
{
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) | (uint32_t(data[offset + 2]) << 8) |
           uint32_t(data[offset + 3]);
}

} // namespace

// Icon.icns is regenerated manually (scripts/regen_app_icons.sh) from a master
// file that is not committed. A previous regeneration silently dropped the
// @2x retina variants (ic11-ic14), which made the icon change size between
// Dock, Cmd-Tab and Finder contexts. Guard the container structure.
TEST_CASE("Icon.icns is a well-formed container with @2x variants", "[resources][icns]")
{
    auto data = read_resource_file("Icon.icns");

    // 'icns' magic followed by a big-endian container length.
    REQUIRE(data.size() >= 8);
    REQUIRE(data[0] == 'i');
    REQUIRE(data[1] == 'c');
    REQUIRE(data[2] == 'n');
    REQUIRE(data[3] == 's');
    REQUIRE(read_be32(data, 4) == data.size());

    // Walk the chunk table: every chunk must be a well-formed (type, length) pair.
    bool has_1024px = false;
    std::vector<std::string> chunk_types;
    size_t offset = 8;
    while (offset + 8 <= data.size()) {
        std::string type(reinterpret_cast<const char *>(data.data()) + offset, 4);
        uint32_t len = read_be32(data, offset + 4);
        REQUIRE(len >= 8);
        REQUIRE(offset + len <= data.size());
        chunk_types.push_back(type);
        has_1024px |= type == "ic10";
        offset += len;
    }
    REQUIRE(offset == data.size());

    // @2x retina variants, dropped in a previous regeneration.
    for (const char *type : {"ic11", "ic12", "ic13", "ic14"})
        REQUIRE(std::find(chunk_types.begin(), chunk_types.end(), type) != chunk_types.end());
    // The 1024px master frame anchors all downscaled variants.
    REQUIRE(has_1024px);
}

// The layered Tahoe icon ships as a prebuilt actool artifact; guard that the
// file exists and still looks like a compiled asset catalog.
TEST_CASE("Assets.car is present and looks like a compiled asset catalog", "[resources][icns]")
{
    auto data = read_resource_file("Assets.car");

    // Compiled asset catalogs start with the BOM ("Bill of Materials") signature.
    REQUIRE(data.size() >= 8);
    REQUIRE(data[0] == 'B');
    REQUIRE(data[1] == 'O');
    REQUIRE(data[2] == 'M');
    REQUIRE(data[3] == 'S');
    REQUIRE(data[4] == 't');
    REQUIRE(data[5] == 'o');
    REQUIRE(data[6] == 'r');
    REQUIRE(data[7] == 'e');
}
