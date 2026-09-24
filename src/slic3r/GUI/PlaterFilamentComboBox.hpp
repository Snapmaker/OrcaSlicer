#pragma once

#include "FilamentDropDown.hpp"
#include "FilamentSort.hpp"
#include "PresetComboBoxes.hpp"

#include <memory>
#include <vector>

namespace Slic3r
{
namespace GUI
{

/** @brief Defines an overridable ordering for system filament vendors. */
/** @brief Provides the filament-specific grouped presentation over the preset combo pipeline. */
class PlaterFilamentComboBox : public PlaterPresetComboBox
{
public:
    /** @brief Creates the filament presentation for a filament preset combobox. */
    PlaterFilamentComboBox(wxWindow *parent, Preset::Type preset_type);
    ~PlaterFilamentComboBox() override;

    /** @brief Rebuilds popup rows after the base preset collection refreshes. */
    void update() override;
    /** @brief Invalidates popup geometry after a Windows DPI change. */
    void msw_rescale() override;

    // Null sorters preserve base order (project/user) or restore the default (system).
    /** @brief Replaces the project-section row sorter. */
    void set_project_sorter(std::unique_ptr<FilamentSorter> sorter);
    /** @brief Replaces the user-section row sorter. */
    void set_user_sorter(std::unique_ptr<FilamentSorter> sorter);
    /** @brief Replaces the system vendor sorter. */
    void set_system_vendor_sorter(std::unique_ptr<FilamentVendorSorter> sorter);
    /** @brief Replaces the system vendor-group row sorter. */
    void set_system_filament_sorter(std::unique_ptr<FilamentSorter> sorter);

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
        FilamentSortItem      sort_item;
        bool                  header{false};
    };

    void rebuild_popup_rows();
    void sort_section_rows(Section section, const FilamentSorter *sorter);
    void sort_system_rows();
    void show_popup();
    void close_popup(bool notify);
    void on_popup_selection(wxCommandEvent &event);
    void on_popup_dismiss(wxCommandEvent &event);
    void on_mouse_down(wxMouseEvent &event);
    void on_key_down(wxKeyEvent &event);
    void on_scroll_parent_move(wxMoveEvent &event);
    void on_top_level_move(wxMoveEvent &event);
    void on_top_level_size(wxSizeEvent &event);

    Section section_from_header(const wxString &text) const;
    PopupRow make_header(const wxString &text, Section section) const;
    std::string preset_vendor(const Preset *preset) const;
    std::string preset_filament_product(const Preset *preset) const;
    wxString popup_group(Section section, const std::string &vendor) const;
    bool is_system_row(const PopupRow &row) const;

    wxWindow *m_top_level{nullptr}; // non-owning wx parent
    wxWindow *m_scroll_parent{nullptr}; // non-owning scroll viewport
    FilamentDropDown *m_popup{nullptr}; // owned by wx parent
    std::unique_ptr<FilamentSorter> m_project_sorter;
    std::unique_ptr<FilamentSorter> m_user_sorter;
    std::unique_ptr<FilamentVendorSorter> m_system_vendor_sorter;
    std::unique_ptr<FilamentSorter> m_system_filament_sorter;
    std::vector<PopupRow> m_rows;
    std::vector<FilamentDropDown::Item> m_popup_items;
    std::vector<int> m_popup_to_combo;
    bool m_popup_visible{false};
    bool m_rebuilding{false};

};

} // namespace GUI
} // namespace Slic3r
