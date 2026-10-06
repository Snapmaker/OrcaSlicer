#ifndef slic3r_GUI_Theme_hpp_
#define slic3r_GUI_Theme_hpp_

#include <map>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include "slic3r/Utils/ThemePack.hpp"

// UI themes (docs/themes.md). A theme is a folder with a theme.json, found in the app's
// resources/themes (the ones EdgeSlicer ships) or in <data dir>/themes (ones the user installed;
// these win over a shipped one with the same folder name). The chosen one is the "ui_theme" app
// setting and is loaded at startup, before the fonts and colours are made. Choosing another one
// later applies its colours, corner radii, icons, title bar, 3D view and Home page at once
// (GUI_App::apply_theme_live); its fonts need a restart.
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

// Switches the running look to theme `id` ("" for Default, the stock look): everything a theme
// hands over is reset first (the pack, the colour table, the corner radii, the banner), then the
// theme is loaded, so going from one theme to another, or back to Default, leaves nothing of the
// old one behind. Returns false when `id` could not be loaded (the stock look is then in force).
// The fonts are left as they are unless `fonts` is set: the faces Label made its fonts from at
// start stay until the next start, and fonts_pending() says whether the theme now in force wants
// others. This only changes state; GUI_App::apply_theme_live() repaints the UI afterwards.
bool apply(const std::string& id, bool fonts = false);

// The theme in force wants fonts (or none) other than the ones the running fonts were made from,
// so they will only show after a restart.
bool fonts_pending();

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

// Where theme `id` lives (an installed one first, then a shipped one) and its theme.json.
bool locate(const std::string& id, boost::filesystem::path& dir, bool& builtin);
bool read(const std::string& id, ThemePack::Spec& spec, boost::filesystem::path& dir, bool& builtin, std::string& error);
// Whether an installed (not shipped) theme with folder name `id` exists.
bool installed(const std::string& id);
bool shipped(const std::string& id);

// Writes `spec` as the installed theme `id`, replacing it if it is there. The files of the theme
// being edited are carried over from `source_dir` (empty for none), then `imports` (a path inside
// the pack -> a file on disk the user picked, a font or an image) are copied in. Nothing is changed
// when it fails. The Themes window saves with this.
bool save(const std::string& id, const ThemePack::Spec& spec, const boost::filesystem::path& source_dir,
          const std::map<std::string, boost::filesystem::path>& imports, std::string& error);

// Deletes the installed theme `id`. Shipped themes cannot be deleted.
bool remove(const std::string& id, std::string& error);

// Copies a theme from a folder or a .zip into user_dir() and returns its folder name, or "" with
// `error` set. `exists` is set (and nothing written) when a theme with that folder name is already
// installed and `overwrite` is false.
std::string install(const boost::filesystem::path& source, bool overwrite, bool& exists, std::string& error);

} // namespace Theme
} // namespace GUI
} // namespace Slic3r

#endif
