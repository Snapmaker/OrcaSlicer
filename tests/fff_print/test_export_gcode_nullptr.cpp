#include <catch2/catch.hpp>
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "test_data.hpp"
#include <boost/filesystem.hpp>

using namespace Slic3r;

TEST_CASE("Print::export_gcode handles null result gracefully", "[Print][ExportGcode]")
{
    GIVEN("A processed print with valid result") {
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        // Initialize a valid print
        Test::init_print({Test::TestMesh::cube_20x20x20}, print, model, config);
        print.process();

        // Verify print is valid and has result
        REQUIRE(print.is_valid());

        WHEN("export_gcode is called with nullptr result parameter") {
            // Create temporary file path
            boost::filesystem::path temp = boost::filesystem::temp_directory_path();
            temp /= "orca_test_valid";
            boost::filesystem::create_directories(temp);

            std::string output_path = (temp / "valid_output.gcode").string();

            // Export should succeed without crashing
            REQUIRE_NOTHROW(print.export_gcode(output_path, nullptr, nullptr));

            // Verify output file was created
            REQUIRE(boost::filesystem::exists(output_path));
            REQUIRE(boost::filesystem::file_size(output_path) > 0);

            // Cleanup
            boost::filesystem::remove_all(temp);
        }
    }

    GIVEN("An unprocessed print with null result") {
        Print print;
        Model model;
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        // Initialize print but don't process it
        Test::init_print({Test::TestMesh::cube_20x20x20}, print, model, config);
        // Note: print.result() may still be nullptr here since we didn't call process()

        WHEN("export_gcode is called on unprocessed print") {
            // Create temporary file path
            boost::filesystem::path temp = boost::filesystem::temp_directory_path();
            temp /= "orca_test_unprocessed";
            boost::filesystem::create_directories(temp);

            std::string output_path = (temp / "unprocessed_output.gcode").string();

            // Export should not crash even if result is null
            REQUIRE_NOTHROW(print.export_gcode(output_path, nullptr, nullptr));

            // Cleanup
            boost::filesystem::remove_all(temp);
        }
    }
}
