#ifndef slic3r_CostsDialog_hpp_
#define slic3r_CostsDialog_hpp_

#include <string>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/CostOverrides.hpp"

class Button;
class wxCheckBox;
class wxChoice;
class wxDataViewEvent;
class wxDataViewListCtrl;
class wxSimplebook;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r {

class Preset;
class PresetCollection;

namespace GUI {

// The currency symbol of the Cost preferences ("cost_currency_symbol"): a label only, prices are
// never converted. Until it is set, the symbol of the system's locale, else "$".
std::string currency_symbol();
// "$23.50" (symbol, then the amount with 2 decimals).
wxString    format_money(double amount);

// The price of the filament preset being edited in the Filament tab, as slicing will use it.
struct EditedFilamentPrice
{
    CostOverrides::Identity id;
    CostOverrides::Resolved resolved;
};
EditedFilamentPrice edited_filament_price(const PresetCollection &filaments);
// One line for the Filament tab under Price: which price slicing uses and why.
wxString filament_price_note(const EditedFilamentPrice &price);

// The machine rate of the printer preset being edited in the Printer tab, as slicing will use it.
struct EditedMachineRate
{
    CostOverrides::MachineIdentity id;
    CostOverrides::MachineResolved resolved;
};
EditedMachineRate edited_machine_rate(const PresetCollection &printers);
// One line for the Printer tab under Time cost.
wxString machine_rate_note(const EditedMachineRate &rate);

// "Bambulab" (the BBL vendor profile's name, part of the machine-rate keys) -> "Bambu Lab". Display only.
std::string display_vendor(const std::string &vendor);

// The Costs window: tab Filaments (your price per kg for each filament family, editable), tab
// Machines (your rate per hour for each printer model, editable, and a default rate) and tab
// Project (fees and markup: this project's values over your defaults, display only). Changes are
// written when the window is closed with OK.
class CostsDialog : public DPIDialog
{
public:
    enum class Page { Filaments, Machines, Project };
    CostsDialog(wxWindow *parent, Page page = Page::Filaments);
    ~CostsDialog() override = default;

    // Filament prices / machine rates changed: the slices' G-code is out of date.
    bool changed() const { return m_changed; }
    // Fees or markup changed (your defaults or the project's): only the cost display is.
    bool pricing_changed() const { return m_pricing_changed || m_project_changed; }
    // This project's fees / markup changed (the project is modified).
    bool project_changed() const { return m_project_changed; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override {}

private:
    // Columns: Vendor, Filament / Model, [Type], Preset value, Your value (last, editable).
    struct Row
    {
        bool        preset_scope{false};   // a "this preset only" value
        std::string key;                   // family / model key, or the preset name for preset_scope
        std::string vendor, type, name, preset;
        size_t      presets{0};            // installed presets with this key
        bool        visible{false};        // one of them is among the ones you picked
        bool        has_value{false};      // a preset value known (min/max valid)
        double      min_value{0.}, max_value{0.};
    };

    struct Table
    {
        bool                machines{false};
        std::vector<Row>    rows;
        std::vector<size_t> shown;   // rows index of each list row
        wxTextCtrl         *search{nullptr};
        wxCheckBox         *show_all{nullptr};
        wxCheckBox         *only_mine{nullptr};
        wxDataViewListCtrl *list{nullptr};
        Button             *btn_set{nullptr};
        Button             *btn_clear{nullptr};
        Button             *btn_clear_all{nullptr};
    };

    wxWindow  *build_page(wxWindow *parent, Table &table);
    void       build_filament_rows();
    void       build_machine_rows();
    void       reload(Table &table);
    void       update_buttons(Table &table);
    const Row *selected_row(const Table &table) const;
    // The value of yours for the row, or false.
    bool       value_of(const Table &table, const Row &row, double &out) const;
    bool       set_value(Table &table, const Row &row, const wxString &text);
    bool       clear_value(Table &table, const Row &row);
    void       on_value_changed(Table &table, wxDataViewEvent &evt);
    void       on_set(Table &table);
    void       on_clear_all(Table &table);
    void       show_page(Page page);
    void       commit_default_rate();

    // ---- Project page (fees and markup)
    struct PricingRow
    {
        PricingField  field{PricingField::Markup};
        wxCheckBox   *use_default{nullptr};
        wxTextCtrl   *value{nullptr};
        wxChoice     *type{nullptr};    // Markup only
        wxChoice     *basis{nullptr};   // Markup only
        wxStaticText *default_text{nullptr};
    };
    wxWindow *build_project_page(wxWindow *parent);
    // Shows `from`'s value of the row's field in its editors.
    void      show_pricing_value(const PricingRow &row, const PricingSettings &from);
    // The row's editors into `to`; false (and `error` says which) when a value is not a number >= 0.
    bool      read_pricing_value(const PricingRow &row, PricingSettings &to, wxString &error) const;
    void      update_pricing_row(const PricingRow &row);
    void      update_default_texts();
    void      on_use_default(PricingRow &row);
    void      on_save_as_defaults();
    // The project's pricing as the page shows it; false on a bad value (an error was shown).
    bool      read_project(ProjectPricing &out, bool show_errors);
    bool      commit_project();

    CostOverrides::Store m_store;
    Table                m_filaments;
    Table                m_machines;
    bool                 m_changed{false};
    bool                 m_pricing_changed{false};   // your default fees / markup
    bool                 m_project_changed{false};   // this project's
    bool                 m_reloading{false};

    std::vector<PricingRow> m_pricing_rows;
    // The project's own values while a field shows your default, so unticking brings them back.
    PricingSettings         m_project_values;

    Button       *m_tab_filaments{nullptr};
    Button       *m_tab_machines{nullptr};
    Button       *m_tab_project{nullptr};
    wxSimplebook *m_book{nullptr};
    wxTextCtrl   *m_default_rate{nullptr};
    Button       *m_btn_ok{nullptr};
    Button       *m_btn_cancel{nullptr};
};

// "Set my price" for the filament preset being edited: the family (default) or this preset only.
bool edit_filament_price(wxWindow *parent, const PresetCollection &filaments);
// "Set my rate" for the printer preset being edited: the model (default) or this preset only.
bool edit_machine_rate(wxWindow *parent, const PresetCollection &printers);

// Opens the Costs window. True when something changed (and was saved).
bool show_costs_dialog(wxWindow *parent, CostsDialog::Page page = CostsDialog::Page::Filaments);

// After a save: every plate's G-code is out of date, the tabs' notes too.
void notify_costs_changed();
// Fees or markup changed: the cost breakdown is drawn again (no reslice).
void notify_pricing_changed();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_CostsDialog_hpp_
