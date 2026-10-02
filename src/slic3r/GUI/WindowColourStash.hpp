#ifndef slic3r_GUI_WindowColourStash_hpp_
#define slic3r_GUI_WindowColourStash_hpp_

#include <cstddef>

#include <wx/colour.h>

class wxWindow;

namespace Slic3r {
namespace GUI {

// The colours a window had before GUI_App::UpdateDarkUI painted it for the dark mode or a UI theme.
//
// UpdateDarkUI maps a window's colour through a table (stock light colour -> dark twin -> themed
// colour). That is a one-way trip: a theme paints several stock colours with one colour, so once a
// window wears theme A's colour there is no telling which stock colour it came from, and switching
// to theme B (or back to the stock look) would leave A's colours on it. So instead of inverting the
// table, each window keeps the colours it had when UpdateDarkUI first changed them, and a live theme
// switch puts those back (restore_all) before the windows are themed again from the new tables.
//
// A window whose colour was changed by somebody else in the meantime (it no longer wears what
// UpdateDarkUI applied) is left alone, and what it wears now counts as its stock colour. Entries go
// with their window (wxEVT_DESTROY). Main thread only.
class WindowColourStash
{
public:
    // A window's stock colours as UpdateDarkUI should map them: the stashed ones while the window
    // still wears what was applied, its own otherwise. `has_bg`: the window had a background of its
    // own (wxWindow::UseBgCol), as opposed to following its parent or the system; wx cannot tell
    // that for the foreground, so `has_fg` just says the colour is valid.
    struct Stock
    {
        wxColour bg, fg;
        bool     has_bg = false, has_fg = false;
    };

    // Call before changing the window's colours ...
    static Stock snapshot(wxWindow* window);
    // ... and after: records what changed (nothing is stored for a window that stayed as it was).
    static void commit(wxWindow* window, const Stock& before);

    // Puts every stashed window back to its stock colours (those still wearing what was applied),
    // redraws it and forgets it, so UpdateDarkUI starts from the stock colours again.
    static void restore_all();

    // Whether `window` has an entry, and how many there are (for tests).
    static bool        has(const wxWindow* window);
    static std::size_t size();
    // Drops the entry of a window (it is also dropped when the window is destroyed).
    static void forget(wxWindow* window);
};

} // namespace GUI
} // namespace Slic3r

#endif
