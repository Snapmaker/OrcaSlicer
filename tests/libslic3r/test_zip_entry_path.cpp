#include <catch2/catch.hpp>

#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>

#include <cstdint>
#include <string>

using namespace Slic3r;

namespace {

// Writes a one-entry zip whose entry name is `legacy_name` (no UTF-8 flag) and whose central
// directory carries `central_extra`. Returns the archive path.
boost::filesystem::path write_zip(const std::string &legacy_name, const std::string &central_extra)
{
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("zip_entry_path_%%%%%%%%.zip");
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path.string()));
    const std::string data = "x";
    REQUIRE(mz_zip_writer_add_mem_ex_v2(&zip, legacy_name.c_str(), data.data(), data.size(), nullptr, 0,
                                        MZ_ZIP_FLAG_ASCII_FILENAME, // no UTF-8 flag: the entry name is a legacy-encoded one
                                        0, 0, nullptr, nullptr, 0,
                                        central_extra.empty() ? nullptr : central_extra.data(), static_cast<mz_uint>(central_extra.size())));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
    return path;
}

std::string unicode_path_field(const std::string &legacy_name, const std::string &utf8_name, bool good_crc)
{
    std::uint32_t crc = static_cast<std::uint32_t>(mz_crc32(0, reinterpret_cast<const unsigned char *>(legacy_name.data()), legacy_name.size()));
    if (!good_crc)
        crc ^= 0xFFFFFFFFu;
    const std::uint16_t len = static_cast<std::uint16_t>(5 + utf8_name.size());
    std::string field;
    field.push_back(0x75);
    field.push_back(0x70);
    field.push_back(static_cast<char>(len & 0xFF));
    field.push_back(static_cast<char>(len >> 8));
    field.push_back(0x01);
    for (int i = 0; i < 4; ++i)
        field.push_back(static_cast<char>((crc >> (8 * i)) & 0xFF));
    field += utf8_name;
    return field;
}

std::string read_entry_path(const boost::filesystem::path &path)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path.string()));
    mz_zip_archive_file_stat stat;
    REQUIRE(mz_zip_reader_file_stat(&zip, 0, &stat));
    const std::string out = decode_archive_entry_path(&zip, stat);
    close_zip_reader(&zip);
    return out;
}

} // namespace

TEST_CASE("decode_archive_entry_path honours the Info-ZIP Unicode Path extra field", "[zip]")
{
    const std::string legacy = "model_cp437.stl";
    const std::string utf8   = "\xE6\xA8\xA1\xE5\x9E\x8B.stl"; // CJK name, UTF-8

    SECTION("matching CRC returns the UTF-8 name")
    {
        const auto path = write_zip(legacy, unicode_path_field(legacy, utf8, true));
        CHECK(read_entry_path(path) == utf8);
        boost::filesystem::remove(path);
    }
    SECTION("mismatching CRC falls back to the legacy name")
    {
        const auto path = write_zip(legacy, unicode_path_field(legacy, utf8, false));
        CHECK(read_entry_path(path) == legacy);
        boost::filesystem::remove(path);
    }
    SECTION("no extra field falls back to the legacy name")
    {
        const auto path = write_zip(legacy, std::string());
        CHECK(read_entry_path(path) == legacy);
        boost::filesystem::remove(path);
    }
}
