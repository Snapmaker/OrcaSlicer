#include <catch2/catch_all.hpp>

#include <fstream>
#include <sstream>
#include <regex>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

// Snapmaker Orca: scans the GUI sources to check that every Print::apply is fed by
// PresetBundle::full_config_for_print(); a plain full_config() would apply another per-extruder
// process table and invalidate the slice.

namespace {

namespace fs = boost::filesystem;

struct Hit
{
    std::string file;
    size_t      line;
    std::string text;
};

// Every line of the GUI sources that matches `pattern`.
std::vector<Hit> gui_lines_matching(const std::regex &pattern)
{
    std::vector<Hit> hits;
    const fs::path   gui = fs::path(SLIC3R_SOURCE_DIR) / "slic3r" / "GUI";
    REQUIRE(fs::is_directory(gui));
    for (fs::recursive_directory_iterator it(gui), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path()))
            continue;
        const std::string extension = it->path().extension().string();
        if (extension != ".cpp" && extension != ".hpp" && extension != ".mm")
            continue;
        std::ifstream in(it->path().string());
        std::string   line;
        size_t        number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (std::regex_search(line, pattern))
                hits.push_back({it->path().filename().string(), number, line});
        }
    }
    return hits;
}

std::string describe(const std::vector<Hit> &hits)
{
    std::string out;
    for (const Hit &hit : hits)
        out += hit.file + ":" + std::to_string(hit.line) + ": " + hit.text + "\n";
    return out;
}

} // namespace

TEST_CASE("Every Print::apply of the GUI is fed by full_config_for_print", "[PerHeadProcess][phs_apply_sites]")
{
    // A Print applied with the plain full config on the same line.
    const std::vector<Hit> plain = gui_lines_matching(std::regex(R"(apply\(.*full_config(_secure)?\()"));
    INFO(describe(plain));
    CHECK(plain.empty());

    // Expected sites: Plater.cpp (update_background_process x2, load_gcode, apply_background_progress,
    // select_plate, select_plate_by_hover_id); PartPlate.cpp (loaded slice result, wipe tower footprint).
    const std::vector<Hit> sites = gui_lines_matching(std::regex(R"(full_config_for_print\()"));
    INFO(describe(sites));
    CHECK(sites.size() >= 8);
    size_t plater = 0, part_plate = 0;
    for (const Hit &hit : sites) {
        if (hit.file == "Plater.cpp")
            ++plater;
        else if (hit.file == "PartPlate.cpp")
            ++part_plate;
    }
    CHECK(plater >= 6);
    CHECK(part_plate >= 2);
}

namespace {

// The text of a GUI source file.
std::string gui_source(const std::string &name)
{
    const fs::path path = fs::path(SLIC3R_SOURCE_DIR) / "slic3r" / "GUI" / name;
    REQUIRE(fs::is_regular_file(path));
    std::ifstream      in(path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The body of the first function definition whose line contains `signature` and does not end in
// a semicolon (a declaration or a call): from that line to the first closing brace in column 0.
// Empty when there is none.
std::string function_body(const std::string &source, const std::string &signature)
{
    for (size_t at = source.find(signature); at != std::string::npos; at = source.find(signature, at + 1)) {
        const size_t line_begin = source.rfind('\n', at) == std::string::npos ? 0 : source.rfind('\n', at) + 1;
        const size_t line_end   = source.find('\n', at);
        const std::string line  = source.substr(line_begin, line_end == std::string::npos ? std::string::npos : line_end - line_begin);
        if (line.find(';') != std::string::npos || line.empty() || line.front() == ' ' || line.front() == '\t')
            continue;
        const size_t end = source.find("\n}\n", line_begin);
        return source.substr(line_begin, end == std::string::npos ? std::string::npos : end - line_begin);
    }
    return std::string();
}

} // namespace

// Plater::on_config_change refreshes the Process tab's speed selector on a nozzle size or preferred
// layer height change, so its labels and the selected head's source preset match the new sizes. The
// refresh is Tab::update_extruder_variants, called directly or via refresh_process_head_selector.
TEST_CASE("A nozzle size or preferred layer height change refreshes the speed selector of the Process tab", "[PerHeadProcess][hs_selector_refresh]")
{
    const std::string plater = gui_source("Plater.cpp");
    const std::string body   = function_body(plater, "void Plater::on_config_change(");
    REQUIRE_FALSE(body.empty());
    // The branch of the nozzle keys is where the refresh belongs.
    REQUIRE(body.find("\"nozzle_diameter\"") != std::string::npos);
    REQUIRE(body.find("\"extruder_layer_height\"") != std::string::npos);

    bool refreshes = body.find("update_extruder_variants(") != std::string::npos;
    if (!refreshes && body.find("refresh_process_head_selector(") != std::string::npos) {
        // The helper: its definition, in any GUI source, calls the refresh.
        for (const char *file : {"Plater.cpp", "Tab.cpp", "GUI_App.cpp"}) {
            const std::string helper = function_body(gui_source(file), "::refresh_process_head_selector(");
            if (helper.find("update_extruder_variants(") != std::string::npos)
                refreshes = true;
        }
    }
    CHECK(refreshes);
}
