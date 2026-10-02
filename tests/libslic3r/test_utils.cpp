#include <catch2/catch.hpp>

#include "libslic3r/Utils.hpp"
#include <test_utils.hpp>

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>
#include <system_error>

using namespace Slic3r;

TEST_CASE("A resolved input path still names the same file after the working directory changes", "[utils]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
                                        boost::filesystem::unique_path("cli_input_%%%%%%%%");
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path model = dir / "model.3mf";
    {
        std::ofstream out(model.string());
        out << "3mf";
    }

    struct Cleanup
    {
        boost::filesystem::path path;
        ~Cleanup()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};

    // Resolve the bare name from the directory holding the file, then move away from it. The guard
    // restores the directory the test started in, wherever this leaves it.
    ScopedWorkingDirectory cwd(dir);
    const std::string      resolved = resolve_cli_input_path(model.filename().string());
    boost::filesystem::current_path(boost::filesystem::path(TEST_DATA_DIR));

    REQUIRE(boost::filesystem::exists(resolved));
    REQUIRE(boost::filesystem::equivalent(resolved, model));
    // Control: the bare name finds nothing from here, so resolving it this late would have failed.
    REQUIRE_FALSE(boost::filesystem::exists(model.filename().string()));
}

TEST_CASE("resolve_cli_input_path completes a relative path against the working directory", "[utils]")
{
    ScopedWorkingDirectory        cwd(boost::filesystem::temp_directory_path());
    // Read back rather than reusing temp_directory_path(): changing to it resolves any symlink.
    const boost::filesystem::path here = boost::filesystem::current_path();

    SECTION("a bare name") {
        REQUIRE(resolve_cli_input_path("model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ./ prefix is dropped") {
        REQUIRE(resolve_cli_input_path("./model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ../ traversal is collapsed") {
        REQUIRE(resolve_cli_input_path("../model.3mf") == (here.parent_path() / "model.3mf").make_preferred().string());
    }
}

TEST_CASE("resolve_cli_input_path leaves inputs that must not be completed unchanged", "[utils]")
{
    SECTION("an absolute path") {
        const boost::filesystem::path absolute = (boost::filesystem::temp_directory_path() / "model.3mf").make_preferred();
        REQUIRE(resolve_cli_input_path(absolute.string()) == absolute.string());
    }
#ifdef _WIN32
    // Every absolute form Windows accepts opens today, so each must come back byte for byte:
    // normalizing them would rewrite the forward slashes and rebuild the \\?\ and UNC prefixes.
    SECTION("an absolute Windows path of any form") {
        for (const std::string absolute : {R"(C:\models\model.3mf)",
                                           R"(C:/models/model.3mf)",
                                           R"(\\server\share\model.3mf)",
                                           R"(\\?\C:\models\model.3mf)"})
            REQUIRE(resolve_cli_input_path(absolute) == absolute);
    }
#endif
    // These are downloaded rather than opened, and completing one would produce a path, not a URL.
    // Edge registers edgeslicer:// and still accepts every older scheme this fork has shipped.
    SECTION("a custom open protocol URL") {
        for (const std::string url : {"edgeslicer://open/?file=https://example.com/model.3mf",
                                      "ultraone://open/?file=https://example.com/model.3mf",
                                      "Snapmaker_Orca://open/?file=https://example.com/model.3mf",
                                      "snapmaker-orca://open/?file=https://example.com/model.3mf",
                                      "orcaslicer://open/?file=https://example.com/model.3mf",
                                      "prusaslicer://open/?file=https://example.com/model.3mf",
                                      "bambustudio://open/?file=https://example.com/model.3mf",
                                      "cura://open/?file=https://example.com/model.3mf"})
            REQUIRE(resolve_cli_input_path(url) == url);
    }
    SECTION("an empty argument") { REQUIRE(resolve_cli_input_path("").empty()); }
}

namespace {

struct ScopedTempDir
{
    boost::filesystem::path path;
    ScopedTempDir()
    {
        path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("atomic_%%%%%%%%");
        boost::filesystem::create_directories(path);
    }
    ~ScopedTempDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

std::string slurp(const boost::filesystem::path &file)
{
    std::string content;
    load_string_file(file, content);
    return content;
}

} // namespace

TEST_CASE("write_file_atomically writes the full content and leaves no temporary", "[utils][atomic]")
{
    ScopedTempDir                     dir;
    const boost::filesystem::path     target = dir.path / "preset.json";
    const std::string                 body   = "{\n  \"name\": \"atomic\"\n}\n";

    std::string err;
    REQUIRE(write_file_atomically(target.string(), body, &err));
    REQUIRE(err.empty());
    REQUIRE(slurp(target) == body);

    size_t entries = 0;
    for (auto &entry : boost::filesystem::directory_iterator(dir.path)) {
        (void) entry;
        ++entries;
    }
    REQUIRE(entries == 1);
}

TEST_CASE("write_file_atomically leaves the original file intact when the write fails", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path target   = dir.path / "preset.json";
    const std::string             original = "keep-me";
    REQUIRE(write_file_atomically(target.string(), original));

    // Plant a directory on the next temporary so fopen of that sibling fails.
    const boost::filesystem::path blocker = atomic_write_temp_path(target.string(), /*consume=*/false);
    boost::filesystem::create_directory(blocker);

    std::string err;
    REQUIRE_FALSE(write_file_atomically(target.string(), "replacement", &err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(slurp(target) == original);
    REQUIRE(boost::filesystem::is_directory(blocker));
}

TEST_CASE("atomic write temp names are unique per call and include the process id", "[utils][atomic]")
{
    const std::string a = atomic_write_temp_path("preset.json");
    const std::string b = atomic_write_temp_path("preset.json");
    REQUIRE(a != b);
    REQUIRE(a.find(std::to_string(get_current_pid())) != std::string::npos);
    REQUIRE(b.find(std::to_string(get_current_pid())) != std::string::npos);
    REQUIRE(a.find(".tmp") != std::string::npos);
    REQUIRE(b.find(".tmp") != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("rename_file replaces an existing POSIX file and reports success", "[utils][atomic]")
{
    ScopedTempDir                 dir;
    const boost::filesystem::path from = dir.path / "from.json";
    const boost::filesystem::path to   = dir.path / "to.json";
    {
        std::ofstream out_from(from.string());
        out_from << "new-bytes";
        std::ofstream out_to(to.string());
        out_to << "old-bytes";
    }

    const std::error_code ec = rename_file(from.string(), to.string());
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(boost::filesystem::exists(from));
    REQUIRE(slurp(to) == "new-bytes");
}
#endif
