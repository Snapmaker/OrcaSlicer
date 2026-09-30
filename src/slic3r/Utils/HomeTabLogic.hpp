#ifndef slic3r_HomeTabLogic_hpp_
#define slic3r_HomeTabLogic_hpp_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The Home tab (resources/web/home, hosted by GUI/HomePanel.cpp) lists recent projects and the
// print history (the G-code archive). These are the rules that page's host follows, kept free of wx
// so they can be tested on their own (tests/slic3rutils/home_tab_tests.cpp):
//   * which sections exist and which one opens by default,
//   * what a Print History card carries (display fields only - never a path on this PC),
//   * which paths the page may act on (only ones the host itself listed),
//   * how an archived file is opened again (a G-code preview or a .gcode.3mf project),
//   * how data reaches the page (one JSON argument, ASCII-escaped).
namespace Slic3r {
namespace HomeTab {

// The sections the page has: "recent", "library", "history" and "vendors".
bool valid_section(const std::string& section);
// `section` when it is one of them, else "recent".
std::string section_or_default(const std::string& section);

// How a file from the G-code archive is opened again on this PC: plain G-code goes to the preview,
// a sliced 3MF (".gcode.3mf") is loaded as a project (it opens preview-only), anything else is not
// offered. Decided by the file name alone, case-insensitively.
enum class ArchiveOpen { None, Gcode, Project };
ArchiveOpen archive_open_kind(const std::string& file_name);

// "#RGB", "#RRGGBB" or "#RRGGBBAA" as given (upper-cased), "" for anything else: the only colour
// form the page is ever handed for a filament swatch.
std::string clean_colour(const std::string& colour);

// One Print History card from a record's sidecar (as GcodeArchive::list() reads it, after
// annotate_models()). `printer` is the printer's name as the user should see it. The card holds
// what the page shows and nothing else: no path on this PC, no hash, no printer address.
nlohmann::json history_card(const nlohmann::json& sidecar, const std::string& printer, bool file_present,
                            bool project_present);

// True when `path` is exactly one of `listed`: the page may only ask to open, reveal or forget a
// file the host itself put on it.
bool is_listed(const std::string& path, const std::vector<std::string>& listed);

// Standard base64 (RFC 4648, with padding) and a PNG data URI built from it ("" for no bytes).
std::string base64(const std::string& bytes);
std::string png_data_uri(const std::string& png_bytes);

// The script that hands `message` to the page: window.HomeApp.receive(<json>), guarded so a page
// that is not (yet) ours ignores it. The JSON is ASCII-only, so no character in it can end the
// script early or break a JS string.
std::string receive_script(const nlohmann::json& message);

} // namespace HomeTab
} // namespace Slic3r

#endif
