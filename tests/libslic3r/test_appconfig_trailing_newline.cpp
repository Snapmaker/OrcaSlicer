#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "../../src/slic3r/GUI/PrintSelectKeys.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <string>

using namespace Slic3r;

// Regression tests for a startup crash: AppConfig::load() used to throw an uncaught
// std::out_of_range (from std::string::substr) whenever the on-disk EdgeSlicer.conf did not end
// with the exact "}\n# MD5 checksum <32 hex>\n" tail the writer normally produces. Any of: no
// trailing newline after the final '}', CRLF line endings, an empty file, or an empty/whitespace
// body must be handled by load() without throwing, either loading successfully or returning a
// non-empty error string so the caller's "corrupted config" recovery path can run.

namespace {

// Returns a path to a fresh temp file under the OS temp dir; not created on disk yet.
std::string make_temp_conf_path(const std::string &suffix)
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path();
    boost::filesystem::path file = dir / (std::string("appconfig_test_") + suffix + ".conf");
    return file.string();
}

void write_file_raw(const std::string &path, const std::string &content)
{
    boost::nowide::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
}

// Loads `content` from a temp file via AppConfig::load() and returns {error_message, app_config}.
// The temp file is removed afterwards regardless of outcome.
std::string load_conf_content(AppConfig &config, const std::string &path, const std::string &content)
{
    write_file_raw(path, content);
    config.set_loading_path(path);
    std::string error;
    try {
        error = config.load();
    } catch (...) {
        boost::filesystem::remove(path);
        throw;
    }
    boost::filesystem::remove(path);
    return error;
}

} // namespace

TEST_CASE("AppConfig::load never throws on a malformed trailing checksum/newline", "[AppConfig]")
{
    AppConfig config;

    SECTION("Valid JSON with no trailing newline after the final brace does not throw") {
        std::string path = make_temp_conf_path("no_trailing_newline");
        std::string content = R"({"app":{"some_key":"some_value"}})"; // no trailing "\n" at all
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        // No checksum present -> logged as a mismatch, but the JSON body itself is valid,
        // so the config should still load successfully.
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Valid JSON followed by CRLF and a checksum-shaped comment does not throw") {
        std::string path = make_temp_conf_path("crlf");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\r\n# MD5 checksum deadbeefdeadbeefdeadbeefdeadbeef\r\n";
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Valid JSON followed by LF and a checksum-shaped comment does not throw") {
        std::string path = make_temp_conf_path("lf_checksum");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\n# MD5 checksum deadbeefdeadbeefdeadbeefdeadbeef\n";
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Truncated checksum line (cut off mid-comment) does not throw") {
        std::string path = make_temp_conf_path("truncated_checksum");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\n# MD5 checksum dead"; // cut off, no final newline
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Empty file does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("empty");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, ""));
        // No JSON body at all: must be reported as an error, not silently accepted, and must
        // definitely not crash.
        CHECK_FALSE(error.empty());
    }

    SECTION("Whitespace-only file does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("whitespace_only");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "   \r\n\t\n  "));
        CHECK_FALSE(error.empty());
    }

    SECTION("File with no closing brace at all does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("no_brace");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "not even json"));
        CHECK_FALSE(error.empty());
    }

    SECTION("Single character file ('}' only, no newline) does not throw") {
        std::string path = make_temp_conf_path("just_brace");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "}"));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("remember_print_action defaults to off", "[AppConfig][PrintSelectKeys]")
{
    AppConfig config;
    CHECK(config.get("remember_print_action") == "false");
    CHECK_FALSE(config.get_bool("remember_print_action"));
    CHECK(config.get("last_print_action").empty());
}

TEST_CASE("Print-action preference keys round-trip; unknown keys fall back to the default", "[PrintSelectKeys]")
{
    using namespace Slic3r::GUI::PrintSelectKeys;

    SECTION("every persisted action round-trips through its string key, never the enum integer")
    {
        const int actions[] = {ePrintAll,           ePrintPlate,        eExportSlicedFile, eExportGcode, eSendGcode,
                               eSendToPrinter,      eSendToPrinterAll,  eExportAllSlicedFile, ePrintMultiMachine};
        for (int action : actions) {
            DYNAMIC_SECTION("action " << action)
            {
                const char *k = key(action);
                REQUIRE(k != nullptr);
                REQUIRE_FALSE(std::string(k).empty());
                CHECK(std::string(k) != std::to_string(action));
                int parsed = -1;
                REQUIRE(from_key(std::string(k), parsed));
                CHECK(parsed == action);
            }
        }
    }

    SECTION("eUploadGcode has no dropdown entry and no persisted key")
    {
        CHECK(std::string(key(eUploadGcode)).empty());
        int parsed = 42;
        CHECK_FALSE(from_key("", parsed));
        CHECK(parsed == 42);
    }

    SECTION("unknown, empty, or not-offered keys fall back to the computed default")
    {
        const int third_party[] = {eSendGcode, eExportSlicedFile, eExportAllSlicedFile, eExportGcode};
        const size_t n          = sizeof(third_party) / sizeof(third_party[0]);
        CHECK(resolve_or_default("export_sliced_file", third_party, n, eSendGcode) == eExportSlicedFile);
        CHECK(resolve_or_default("print_multi_machine", third_party, n, eSendGcode) == eSendGcode);
        CHECK(resolve_or_default("not_a_real_action", third_party, n, ePrintPlate) == ePrintPlate);
        CHECK(resolve_or_default("", third_party, n, ePrintPlate) == ePrintPlate);
        CHECK(resolve_or_default("1", third_party, n, ePrintPlate) == ePrintPlate);
        CHECK(resolve_or_default("4", third_party, n, ePrintPlate) == ePrintPlate);
    }
}
