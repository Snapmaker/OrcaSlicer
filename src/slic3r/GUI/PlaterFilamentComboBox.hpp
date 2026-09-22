#ifndef slic3r_GUI_PlaterFilamentComboBox_hpp_
#define slic3r_GUI_PlaterFilamentComboBox_hpp_

#include "FilamentDropDown.hpp"
#include "PresetComboBoxes.hpp"

#include <vector>

namespace Slic3r {
namespace GUI {

// Filament-only presentation layer. PresetBundle, PresetCollection and the
// existing Plater selection pipeline remain owned by PlaterPresetComboBox.
class PlaterFilamentComboBox : public PlaterPresetComboBox
{
public:
    PlaterFilamentComboBox(wxWindow *parent, Preset::Type preset_type);
    ~PlaterFilamentComboBox();

    void update() override;
    void msw_rescale() override;

private:
    enum class Section {
        Other,
        Project,
        User,
        System,
    };

    struct PopupRow {
        FilamentDropDown::Item item;
        int                   combo_index{-1};
        Section               section{Section::Other};
        std::string           vendor;
        std::string           filament_type;
        std::string           filament_product;
        bool                  header{false};
    };

    void rebuild_popup_rows();
    void show_popup();
    void close_popup(bool notify);
    void on_popup_selection(wxCommandEvent &event);
    void on_popup_dismiss(wxCommandEvent &event);
    void on_mouse_down(wxMouseEvent &event);
    void on_key_down(wxKeyEvent &event);
    void on_top_level_move(wxMoveEvent &event);
    void on_top_level_size(wxSizeEvent &event);

    Section section_from_header(const wxString &text) const;
    PopupRow make_header(const wxString &text, Section section) const;
    std::string preset_vendor(const Preset *preset) const;
    std::string preset_filament_type(const Preset *preset) const;
    std::string preset_filament_product(const Preset *preset) const;
    wxString popup_group(Section section, const std::string &vendor) const;
    bool is_system_row(const PopupRow &row) const;

    wxWindow *m_top_level{nullptr}; // non-owning wx parent
    FilamentDropDown *m_popup{nullptr}; // owned by wx parent
    std::vector<PopupRow> m_rows;
    std::vector<FilamentDropDown::Item> m_popup_items;
    std::vector<int> m_popup_to_combo;
    bool m_popup_visible{false};
    bool m_rebuilding{false};

    wxDECLARE_EVENT_TABLE();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PlaterFilamentComboBox_hpp_
