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

private:
#ifdef __WXOSX__
    /** @brief Filters copied macOS outside-click events for filament combo boxes. */
    class RepostedClickDetector;
#endif

    /** @brief Identifies which preset source a popup row was read from. */
    enum class Section {
        Other,
        Project,
        User,
        System,
    };

    /** @brief Pairs one popup row with the combo index and sort keys it was built from. */
    struct PopupRow {
        FilamentDropDown::Item item;
        int                   combo_index{-1};
        Section               section{Section::Other};
        FilamentSortItem      sort_item;
        bool                  header{false};
    };

    /** @brief Rebuilds the popup items and rows from the current preset collection. */
    void rebuild_popup_rows();
    /** @brief Sorts one section's rows with @p sorter, leaving its headers in place. */
    void sort_section_rows(Section section, const FilamentSorter *sorter);
    /** @brief Sorts the system rows by vendor priority and then by the configured filament order. */
    void sort_system_rows();
    /** @brief Opens the two-level popup, dismissing any flat popup the base class left open. */
    void show_popup();
    /** @brief Hides the popup, releasing the active-popup bookkeeping; @p notify reports the selection. */
    void close_popup(bool notify);
    /** @brief Applies the selected popup row to the preset pipeline. */
    void on_popup_selection(wxCommandEvent &event);
    /** @brief Clears the visible-popup state after the popup dismissed itself. */
    void on_popup_dismiss(wxCommandEvent &event);
    /** @brief Opens the popup for a click on this control. */
    void on_mouse_down(wxMouseEvent &event);
    /** @brief Forwards text-control key events to the popup. */
    void on_key_down(wxKeyEvent &event);
    /** @brief Repositions or dismisses the popup when the sidebar's scrolled panel moves. */
    void on_scroll_parent_move(wxMoveEvent &event);
    /** @brief Repositions or dismisses the popup when the top-level window moves. */
    void on_top_level_move(wxMoveEvent &event);
    /** @brief Repositions or dismisses the popup when the top-level window is resized. */
    void on_top_level_size(wxSizeEvent &event);

    /** @brief Maps a popup section header label back to its section. */
    Section section_from_header(const wxString &text) const;
    /** @brief Builds a header row for @p section with the given label. */
    PopupRow make_header(const wxString &text, Section section) const;
    /** @brief Returns the vendor recorded by @p preset, or an empty string. */
    std::string preset_vendor(const Preset *preset) const;
    /** @brief Returns the product key that the configured order matches for @p preset. */
    std::string preset_filament_product(const Preset *preset) const;
    /** @brief Returns the group label that @p vendor belongs to within @p section. */
    wxString popup_group(Section section, const std::string &vendor) const;
    /** @brief Reports whether @p row is a selectable system row. */
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
