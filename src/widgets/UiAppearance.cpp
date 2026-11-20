#include "UiAppearance.hpp"

#include <wx/log.h>

namespace Slic3r {
namespace GUI {

namespace {

UiAppearance *s_impl = nullptr;

// fix-widget-resource-crash: controlled default appearance used when the host
// has not installed an implementation. Conservative values (em=10, light mode,
// system GUI font); a one-shot wxLogWarning keeps the degradation non-silent,
// and ui_appearance_installed() allows querying it programmatically.
class DefaultUiAppearance : public UiAppearance
{
public:
    int em_unit() const override { return 10; }
    int em_unit_of(wxWindow *win) const override
    {
        return win ? int(win->FromDIP(10) + 0.5f) : 10;
    }
    bool dark_mode() const override { return false; }
    bool menu_dark_mode() const override { return false; }
    void update_dark_ui(wxWindow *, bool) const override {}
    const wxFont &bold_font() const override
    {
        static wxFont f = [] {
            wxFont f = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
            f.MakeBold();
            return f;
        }();
        return f;
    }
    const wxFont &normal_font() const override
    {
        static wxFont f = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        return f;
    }
    wxWindow *mainframe() const override { return nullptr; }
};

UiAppearance &default_ui_appearance()
{
    static DefaultUiAppearance inst;
    return inst;
}

} // namespace

UiAppearance &ui()
{
    if (s_impl == nullptr) {
        // Controlled degradation: no assert, no null dereference (a crash source in
// practice, see G4). Warns on first use.
        static bool warned = false;
        if (!warned) {
            warned = true;
            wxLogWarning("UiAppearance not installed (set_ui_appearance); "
                         "default appearance in use");
        }
        return default_ui_appearance();
    }
    return *s_impl;
}

void set_ui_appearance(UiAppearance *impl)
{
    s_impl = impl;
}

bool ui_appearance_installed()
{
    return s_impl != nullptr;
}

}} // namespace Slic3r::GUI
