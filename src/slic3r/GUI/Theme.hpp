#ifndef slic3r_GUI_Theme_hpp_
#define slic3r_GUI_Theme_hpp_

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include "slic3r/Utils/ThemePack.hpp"

// UI themes (docs/themes.md). A theme is a folder with a theme.json, found in the app's
// resources/themes (the ones EdgeSlicer ships) or in <data dir>/themes (ones the user installed;
// these win over a shipped one with the same folder name). The chosen one is the "ui_theme" app
// setting and is loaded once at startup, before the fonts and colours are made, so changing it
// takes a restart.
namespace Slic3r {
namespace GUI {
namespace Theme {

struct Entry
{
    std::string id;          // folder name, what "ui_theme" stores
    std::string name;
    std::string author;
    std::string description;
    bool        builtin = false;
};

// Every theme that reads, sorted by name.
std::vector<Entry> available();

// <data dir>/themes, where installed themes go.
boost::filesystem::path user_dir();

// Loads theme `id` ("" for none) and hands its colours, fonts and shapes to the widgets. A theme
// that cannot be read is logged and left off. Call once, after the app config is loaded and before
// Label::initSysFont().
void load(const std::string& id);

bool                       active();
const std::string&         active_id();
const ThemePack::Spec&     spec();

// The look the theme is built on: 1 dark, 0 light, -1 none (follow the dark mode setting).
int base_dark();

// A palette role's colour, or `fallback` when no theme is active or it leaves the role alone.
wxColour colour(const std::string& role, const wxColour& fallback);
bool     has_colour(const std::string& role);

// The title bar banner, loaded on first use; !IsOk() when there is none or it could not be read.
const wxBitmap& banner();

// Copies a theme from a folder or a .zip into user_dir() and returns its folder name, or "" with
// `error` set. `exists` is set (and nothing written) when a theme with that folder name is already
// installed and `overwrite` is false.
std::string install(const boost::filesystem::path& source, bool overwrite, bool& exists, std::string& error);

} // namespace Theme
} // namespace GUI
} // namespace Slic3r

#endif
