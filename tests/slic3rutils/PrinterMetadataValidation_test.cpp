#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/PrinterMetadataValidation.hpp"

#include <fstream>

namespace fs = boost::filesystem;

namespace {
struct StagingDirectory
{
    fs::path path = fs::temp_directory_path() / fs::unique_path("orca-metadata-%%%%-%%%%");
    StagingDirectory() { fs::create_directories(path); }
    ~StagingDirectory() { fs::remove_all(path); }
    void write(const char* name, const char* content) { std::ofstream(path.string() + "/" + name) << content; }
};
} // namespace

TEST_CASE("Shipped flat printer metadata passes staging validation", "[updater][printer-metadata]")
{ REQUIRE(Slic3r::validate_printer_metadata_directory(fs::path(TEST_RESOURCES_DIR) / "printers")); }

TEST_CASE("Incomplete printer metadata cannot replace the installed directory", "[updater][printer-metadata]")
{
    StagingDirectory staging;
    REQUIRE_FALSE(Slic3r::validate_printer_metadata_directory(staging.path));
    staging.write("C11.json", "{}");
    REQUIRE_FALSE(Slic3r::validate_printer_metadata_directory(staging.path));
    staging.write("version.txt", "01.10.00.01");
    REQUIRE(Slic3r::validate_printer_metadata_directory(staging.path));
    fs::remove(staging.path / "C11.json");
    REQUIRE_FALSE(Slic3r::validate_printer_metadata_directory(staging.path));
}

TEST_CASE("Preset trees and directories named like JSON files are not printer metadata", "[updater][printer-metadata]")
{
    StagingDirectory staging;
    staging.write("version.txt", "01.10.00.01");
    fs::create_directories(staging.path / "machine");
    fs::create_directories(staging.path / "C11.json");
    REQUIRE_FALSE(Slic3r::validate_printer_metadata_directory(staging.path));
    fs::remove(staging.path / "version.txt");
    fs::create_directories(staging.path / "version.txt");
    staging.write("C12.json", "{}");
    REQUIRE_FALSE(Slic3r::validate_printer_metadata_directory(staging.path));
}
