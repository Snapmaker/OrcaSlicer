#include <catch_main.hpp>

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <vector>
// The nanoSVG C implementation used to be defined inline here. It is now
// provided solely by the shared tests/nanosvg_impl.cpp OBJECT library that
// every test executable links, so headless builds (SLIC3R_GUI=OFF) resolve
// libslic3r's nanoSVG references without duplicate definitions.
namespace {

TEST_CASE("sort_remove_duplicates", "[utils]") {
	std::vector<int> data_src = { 3, 0, 2, 1, 15, 3, 5, 6, 3, 1, 0 };
	std::vector<int> data_dst = { 0, 1, 2, 3, 5, 6, 15 };
	Slic3r::sort_remove_duplicates(data_src);
    REQUIRE(data_src == data_dst);
}

TEST_CASE("string_printf", "[utils]") {
    SECTION("Empty format with empty data should return empty string") {
        std::string outs = Slic3r::string_printf("");
        REQUIRE(outs.empty());
    }
    
    SECTION("String output length should be the same as input") {
        std::string outs = Slic3r::string_printf("1234");
        REQUIRE(outs.size() == 4);
    }
    
    SECTION("String format should be interpreted as with sprintf") {
        std::string outs = Slic3r::string_printf("%d %f %s", 10, 11.4, " This is a string");
        char buffer[1024];
        
        sprintf(buffer, "%d %f %s", 10, 11.4, " This is a string");
        
        REQUIRE(outs.compare(buffer) == 0);
    }
    
    SECTION("String format should survive large input data") {
        std::string input(2048, 'A');
        std::string outs = Slic3r::string_printf("%s", input.c_str());
        REQUIRE(outs.compare(input) == 0);
    }
}

TEST_CASE("collect_log_export_files includes application and control logs", "[utils][logs]")
{
    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
            : m_path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_log_export_test_%%%%-%%%%"))
        {}
        ~TemporaryDirectory()
        {
            boost::system::error_code error_code;
            boost::filesystem::remove_all(m_path, error_code);
        }
        const boost::filesystem::path& path() const { return m_path; }

    private:
        boost::filesystem::path m_path;
    };

    TemporaryDirectory temporary_directory;
    const auto         application_log = temporary_directory.path() / "log" / "Orca.log";
    const auto         control_log     = temporary_directory.path() / "snapmaker_control" / "data" / "log" / "control.log";
    boost::filesystem::create_directories(application_log.parent_path());
    boost::filesystem::create_directories(control_log.parent_path());
    {
        std::ofstream(application_log.string(), std::ios::binary) << "application log";
        std::ofstream(control_log.string(), std::ios::binary) << "control log";
    }

    const auto exported_files = Slic3r::collect_log_export_files(temporary_directory.path());

    REQUIRE(exported_files.size() == 2);
    REQUIRE(exported_files[0].source_path == application_log);
    REQUIRE(exported_files[0].archive_name == "Orca.log");
    REQUIRE(exported_files[1].source_path == control_log);
    REQUIRE(exported_files[1].archive_name == "snapmaker_control/data/log/control.log");
}
}
