#pragma once

#include "GUI_Utils.hpp"

#include <string>
#include <vector>

namespace Slic3r {
class Preset;
}

class wxStaticBitmap;
class CheckBox;
class Button;

namespace Slic3r { namespace GUI {

class SupportParameterDiffDialog : public DPIDialog
{
public:
    enum class Mode { ApplyRecommendation, RestoreDefaults };

    struct Item
    {
        wxString    name;
        wxString    current_value;
        wxString    target_value;
    };

    SupportParameterDiffDialog(Mode mode, std::vector<Item> items);

private:
    void on_dpi_changed(const wxRect& suggested_rect) override;
};

class SupportFilamentGuideDialog : public DPIDialog
{
public:
    SupportFilamentGuideDialog(const std::string& guide_url);

    bool dont_show_again() const;

private:
    void on_dpi_changed(const wxRect& suggested_rect) override;

    ::CheckBox*     m_dont_show_checkbox{nullptr};
    Button*         m_got_it_button{nullptr};
    Button*         m_guide_button{nullptr};
    wxStaticBitmap* m_guide_bitmap{nullptr};
};

void show_support_filament_guide_if_needed(const Preset& preset);

}} // namespace Slic3r::GUI
