#include <catch2/catch_test_macros.hpp>

// Snapmaker Orca: the GUI-free decisions of the Snapmaker OTA install path. The end-to-end recipes
// need the OTA URL redirected to a local server; these cases cover the same decisions directly.
#include "slic3r/Utils/OtaUpdateRules.hpp"

#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct FakeUpdate
{
    bool can_install;
};

// A scratch OTA root that is removed with the test case.
struct ScratchDir
{
    fs::path path;

    ScratchDir() : path(fs::temp_directory_path() / fs::unique_path("ota_rules_%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~ScratchDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& file, const std::string& content)
{
    boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
    out << content;
}

} // namespace

TEST_CASE("OTA update rules: an update set with nothing installable", "[OtaUpdateRules]")
{
    CHECK_FALSE(OtaUpdateRules::any_installable(std::vector<FakeUpdate>{}));
    CHECK_FALSE(OtaUpdateRules::any_installable(std::vector<FakeUpdate>{{false}, {false}, {false}}));
    CHECK(OtaUpdateRules::any_installable(std::vector<FakeUpdate>{{false}, {true}}));
    CHECK(OtaUpdateRules::any_installable(std::vector<FakeUpdate>{{true}}));
}

TEST_CASE("OTA update rules: a JSON cache entry needs its non-empty vendor directory", "[OtaUpdateRules]")
{
    const ScratchDir root;
    const fs::path   vendor_json = root.path / "Snapmaker.json";
    const fs::path   vendor_dir  = root.path / "Snapmaker";

    SECTION("nothing extracted") {
        CHECK_FALSE(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
    SECTION("truncated package: the bare json") {
        write_file(vendor_json, "{\"version\": \"02.04.00.15\"}");
        CHECK_FALSE(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
    SECTION("truncated package: the json and an empty directory") {
        write_file(vendor_json, "{}");
        fs::create_directories(vendor_dir);
        CHECK_FALSE(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
    SECTION("the directory without the json") {
        fs::create_directories(vendor_dir / "machine");
        CHECK_FALSE(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
    SECTION("a directory where the json should be") {
        fs::create_directories(vendor_json);
        fs::create_directories(vendor_dir / "machine");
        CHECK_FALSE(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
    SECTION("the complete pair") {
        write_file(vendor_json, "{}");
        fs::create_directories(vendor_dir / "machine");
        CHECK(OtaUpdateRules::json_cache_entry_complete(vendor_json, vendor_dir));
    }
}

TEST_CASE("OTA update rules: the minimum application version gate", "[OtaUpdateRules]")
{
    SECTION("build labels") {
        REQUIRE(OtaUpdateRules::application_version("2.5.0").has_value());
        CHECK(*OtaUpdateRules::application_version("2.5.0") == Semver(2, 5, 0));
        // A pre-release label parses and orders below its release.
        REQUIRE(OtaUpdateRules::application_version("2.5.0-beta.1").has_value());
        CHECK(*OtaUpdateRules::application_version("2.5.0-beta.1") < Semver(2, 5, 0));
        // Labels Semver's string constructor throws on.
        CHECK_THROWS(Semver(std::string("v2.5.0")));
        CHECK_FALSE(OtaUpdateRules::application_version("v2.5.0").has_value());
        CHECK_FALSE(OtaUpdateRules::application_version("").has_value());
        CHECK_FALSE(OtaUpdateRules::application_version("nightly").has_value());
    }
    SECTION("a known application version gates the package") {
        const boost::optional<Semver> app = OtaUpdateRules::application_version("2.5.0");
        CHECK(OtaUpdateRules::min_app_version_satisfied(Semver(2, 4, 0), app));
        CHECK(OtaUpdateRules::min_app_version_satisfied(Semver(2, 5, 0), app));
        CHECK_FALSE(OtaUpdateRules::min_app_version_satisfied(Semver(2, 5, 1), app));
        CHECK_FALSE(OtaUpdateRules::min_app_version_satisfied(Semver(3, 0, 0), app));
    }
    SECTION("an unknown application version is no gate") {
        const boost::optional<Semver> app = OtaUpdateRules::application_version("v2.5.0");
        CHECK(OtaUpdateRules::min_app_version_satisfied(Semver(2, 4, 0), app));
        CHECK(OtaUpdateRules::min_app_version_satisfied(Semver(99, 0, 0), app));
    }
}
