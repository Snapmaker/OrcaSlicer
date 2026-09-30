#ifndef slic3r_ThemePack_hpp_
#define slic3r_ThemePack_hpp_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// A UI theme pack: a folder with a theme.json and, optionally, fonts and images it names
// (docs/themes.md). These are the rules for reading one, kept free of wx so they can be tested on
// their own (tests/slic3rutils/theme_pack_tests.cpp). GUI/Theme.cpp finds packs on disk, loads the
// active one at startup and hands the result to the widgets.
//
// A theme never runs anything: it is colours, sizes, a font face and file names, and every file it
// names must sit inside its own folder.
namespace Slic3r {
namespace ThemePack {

struct Font
{
    std::vector<std::string> files; // relative to the pack folder
    std::string              face;  // the family name inside those files, e.g. "Cinzel"
    bool empty() const { return face.empty(); }
};

struct Spec
{
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string base;                              // "light", "dark" or "" (follow the dark mode setting)
    std::map<std::string, std::string> palette;    // role -> "#RRGGBB"
    std::map<std::string, std::string> overrides;  // stock light colour "#RRGGBB" -> "#RRGGBB"
    Font        body;                             // every Label::Body_* font
    Font        heading;                          // every Label::Head_* font
    Font        button;                           // the label of every Button (tabs included)
    int         button_radius = -1;               // DIP; -1 keeps each button's own
    int         box_radius    = -1;               // DIP; inputs, combo boxes, cards
    std::string banner;                           // title bar image, relative to the pack folder
    std::string banner_align = "left";            // left | center | right | stretch | tile
    std::map<std::string, std::string> home;       // Home tab CSS variable -> "#RRGGBB"
    std::vector<std::string> warnings;            // what was ignored, for the log
};

// The palette roles a theme may set, each with the stock light colours it recolours (the keys of
// the light -> dark table in GUI/Widgets/StateColor.cpp). Roles with no stock colours are read
// directly by the code that draws them (title bar, 3D view, icons).
const std::vector<std::pair<std::string, std::vector<std::string>>>& roles();
bool known_role(const std::string& role);

// "#abc", "#AABBCC" or "#AABBCCFF" (alpha dropped) -> "#AABBCC"; anything else -> "".
std::string normalize_colour(const std::string& colour);

// A path a pack may name: relative, forward or back slashes, no "..", no drive or root, no empty
// or "." segments, at most 255 characters.
bool safe_relative_path(const std::string& path);

// A folder name for an installed theme: 1-64 of [A-Za-z0-9._ -], not starting with '.' or ' '.
bool valid_id(const std::string& id);
// A usable folder name made from `name` (e.g. a zip's file name): invalid characters become '-'.
std::string id_from_name(const std::string& name);

// Reads theme.json. False only when there is nothing usable (not JSON, not an object, no name);
// a bad value inside is skipped with a note in `out.warnings`.
bool parse(const std::string& text, Spec& out, std::string& error);

// The stock light colour -> themed colour map the widgets use: every role's colours, then the raw
// overrides on top. A themed colour that is itself one of the `reserved` colours (the stock light
// colours and their dark twins) is moved by one step of blue, as the stock table does with
// #FFFFFE, so a colour that was already themed can never be mistaken for one still to be themed.
std::map<std::string, std::string> colour_map(const Spec& spec, const std::set<std::string>& reserved);

// The Home tab's CSS variables for this theme (resources/web/home/home.css), from the palette and
// then the pack's own "home" entries. Only variables the page defines are passed.
nlohmann::json home_css(const Spec& spec);

} // namespace ThemePack
} // namespace Slic3r

#endif
