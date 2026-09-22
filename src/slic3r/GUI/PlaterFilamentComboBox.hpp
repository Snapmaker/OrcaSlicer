#ifndef slic3r_GUI_PlaterFilamentComboBox_hpp_
#define slic3r_GUI_PlaterFilamentComboBox_hpp_

#include "FilamentDropDown.hpp"
#include "PresetComboBoxes.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

struct FilamentSortItem {
    wxString    display_name;
    std::string vendor;
    std::string filament_type;
    std::string filament_product;
    size_t      original_index{0};
};

// Default sorter for popup rows. Specialized sorters may extend the ordering
// without depending on the popup or preset collection.
class FilamentSorter
{
public:
    FilamentSorter() = default;
    virtual ~FilamentSorter() = default;

    FilamentSorter(const FilamentSorter &)            = delete;
    FilamentSorter &operator=(const FilamentSorter &) = delete;
    FilamentSorter(FilamentSorter &&)                 = delete;
    FilamentSorter &operator=(FilamentSorter &&)      = delete;

    // Implementations must provide a strict weak ordering.
    virtual bool less(const FilamentSortItem &left, const FilamentSortItem &right) const;

protected:
    bool less_by_name(const FilamentSortItem &left, const FilamentSortItem &right) const;
};

// Vendor sorters operate only on vendor names. Keeping this contract separate
// from FilamentSorter prevents row fields from being mixed in one comparator.
class FilamentVendorSorter
{
public:
    FilamentVendorSorter() = default;
    virtual ~FilamentVendorSorter() = default;

    FilamentVendorSorter(const FilamentVendorSorter &)            = delete;
    FilamentVendorSorter &operator=(const FilamentVendorSorter &) = delete;
    FilamentVendorSorter(FilamentVendorSorter &&)                 = delete;
    FilamentVendorSorter &operator=(FilamentVendorSorter &&)      = delete;

    // Implementations must provide a strict weak ordering.
    virtual bool less(const std::string &left, const std::string &right) const;
};

class SystemFilamentVendorSorter final : public FilamentVendorSorter
{
public:
    bool less(const std::string &left, const std::string &right) const override;
};

class SystemFilamentSorter final : public FilamentSorter
{
public:
    bool less(const FilamentSortItem &left, const FilamentSortItem &right) const override;
};

// Filament-only presentation layer. PresetBundle, PresetCollection and the
// existing Plater selection pipeline remain owned by PlaterPresetComboBox.
class PlaterFilamentComboBox : public PlaterPresetComboBox
{
public:
    PlaterFilamentComboBox(wxWindow *parent, Preset::Type preset_type);
    ~PlaterFilamentComboBox() override;

    void update() override;
    void msw_rescale() override;

    // A null project/user sorter preserves the row order produced by the base
    // combo box. A null system sorter restores the corresponding default.
    void set_project_sorter(std::unique_ptr<FilamentSorter> sorter);
    void set_user_sorter(std::unique_ptr<FilamentSorter> sorter);
    void set_system_vendor_sorter(std::unique_ptr<FilamentVendorSorter> sorter);
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
    std::unique_ptr<FilamentSorter> m_project_sorter;
    std::unique_ptr<FilamentSorter> m_user_sorter;
    std::unique_ptr<FilamentVendorSorter> m_system_vendor_sorter;
    std::unique_ptr<FilamentSorter> m_system_filament_sorter;
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
