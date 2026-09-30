#include "WindowColourStash.hpp"

#include <map>
#include <set>
#include <vector>

#include <wx/window.h>

namespace Slic3r {
namespace GUI {

namespace {

struct Entry
{
    // Per channel: the colour before UpdateDarkUI changed it, whether the window had one of its own,
    // what UpdateDarkUI made of it, and whether that is in force (the window was changed at all).
    wxColour stock_bg, stock_fg, applied_bg, applied_fg;
    bool     had_bg = false, had_fg = false;
    bool     changed_bg = false, changed_fg = false;
};

std::map<wxWindow*, Entry> g_entries;
// Windows that already carry the destroy handler: entries come and go with every theme switch, the
// handler is bound once.
std::set<wxWindow*> g_bound;

void bind_destroy(wxWindow* window)
{
    if (!g_bound.insert(window).second)
        return;
    window->Bind(wxEVT_DESTROY, [window](wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == window) {
            g_entries.erase(window);
            g_bound.erase(window);
        }
        event.Skip();
    });
}

} // namespace

WindowColourStash::Stock WindowColourStash::snapshot(wxWindow* window)
{
    Stock stock;
    stock.bg     = window->GetBackgroundColour();
    stock.fg     = window->GetForegroundColour();
    stock.has_bg = window->UseBgCol();
    // wx does not say whether the foreground was set by hand (there is no UseFgCol); the colour it
    // reports is put back as it is.
    stock.has_fg = stock.fg.IsOk();
    auto it      = g_entries.find(window);
    if (it != g_entries.end()) {
        const Entry& e = it->second;
        if (e.changed_bg && stock.bg == e.applied_bg) {
            stock.bg     = e.stock_bg;
            stock.has_bg = e.had_bg;
        }
        if (e.changed_fg && stock.fg == e.applied_fg) {
            stock.fg     = e.stock_fg;
            stock.has_fg = e.had_fg;
        }
    }
    return stock;
}

void WindowColourStash::commit(wxWindow* window, const Stock& before)
{
    const wxColour bg = window->GetBackgroundColour();
    const wxColour fg = window->GetForegroundColour();
    const bool     bg_changed = bg != before.bg;
    const bool     fg_changed = fg != before.fg;
    auto           it         = g_entries.find(window);
    if (it == g_entries.end()) {
        if (!bg_changed && !fg_changed)
            return;
        it = g_entries.emplace(window, Entry()).first;
        bind_destroy(window);
    }
    Entry& e = it->second;
    e.changed_bg = bg_changed;
    e.changed_fg = fg_changed;
    if (bg_changed) {
        e.stock_bg   = before.bg;
        e.had_bg     = before.has_bg;
        e.applied_bg = bg;
    }
    if (fg_changed) {
        e.stock_fg   = before.fg;
        e.had_fg     = before.has_fg;
        e.applied_fg = fg;
    }
    if (!e.changed_bg && !e.changed_fg)
        g_entries.erase(it);
}

void WindowColourStash::restore_all()
{
    // Restoring a colour can make wx send events; work on a copy so a window that goes away meanwhile
    // (its destroy handler erases the entry) is simply skipped.
    std::vector<std::pair<wxWindow*, Entry>> entries(g_entries.begin(), g_entries.end());
    g_entries.clear();
    for (auto& [window, e] : entries) {
        if (g_bound.count(window) == 0)
            continue;
        bool touched = false;
        if (e.changed_bg && window->GetBackgroundColour() == e.applied_bg) {
            window->SetBackgroundColour(e.had_bg ? e.stock_bg : wxNullColour);
            touched = true;
        }
        if (e.changed_fg && window->GetForegroundColour() == e.applied_fg) {
            window->SetForegroundColour(e.had_fg ? e.stock_fg : wxNullColour);
            touched = true;
        }
        if (touched)
            window->Refresh();
    }
}

bool WindowColourStash::has(const wxWindow* window) { return g_entries.count(const_cast<wxWindow*>(window)) > 0; }

std::size_t WindowColourStash::size() { return g_entries.size(); }

void WindowColourStash::forget(wxWindow* window) { g_entries.erase(window); }

} // namespace GUI
} // namespace Slic3r
