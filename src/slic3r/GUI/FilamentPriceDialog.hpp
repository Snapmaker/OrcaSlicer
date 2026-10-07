#ifndef slic3r_FilamentPriceDialog_hpp_
#define slic3r_FilamentPriceDialog_hpp_

#include <string>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentPrices.hpp"

class Button;
class wxCheckBox;
class wxDataViewEvent;
class wxDataViewListCtrl;
class wxStaticText;
class wxTextCtrl;

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
    FilamentPrices::Identity id;
    FilamentPrices::Resolved resolved;
};
EditedFilamentPrice edited_filament_price(const PresetCollection &filaments);

// One line for the Filament tab under Price: which price slicing uses and why.
wxString filament_price_note(const EditedFilamentPrice &price);

// Every filament family the installed presets know, one row each, with an editable "Your price".
// Changes are written when the dialog is closed with OK.
class FilamentPriceDialog : public DPIDialog
{
public:
    FilamentPriceDialog(wxWindow *parent);
    ~FilamentPriceDialog() override = default;

    bool changed() const { return m_changed; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void on_sys_color_changed() override {}

private:
    enum Column { COL_VENDOR, COL_FILAMENT, COL_TYPE, COL_PRESET_PRICE, COL_YOUR_PRICE };

    struct Row
    {
        bool        preset_scope{false};   // a "this preset only" price
        std::string key;                   // family key, or the preset name for preset_scope
        std::string vendor, type, family, preset;
        size_t      presets{0};            // installed presets in the family
        bool        visible{false};        // one of them is among the filaments you picked
        bool        has_price{false};      // a preset price known (min/max valid)
        double      min_price{0.}, max_price{0.};
    };

    void build_rows();
    void reload();
    void update_buttons();
    const Row *selected_row() const;
    const FilamentPrices::Entry *entry_of(const Row &row) const;
    void on_value_changed(wxDataViewEvent &evt);
    void on_set_price();
    void on_clear();
    void on_clear_all();
    bool set_price(const Row &row, const wxString &text);

    FilamentPrices::Store    m_store;
    std::vector<Row>         m_rows;
    std::vector<size_t>      m_shown;   // m_rows index of each list row
    bool                     m_changed{false};
    bool                     m_reloading{false};

    wxTextCtrl         *m_search{nullptr};
    wxCheckBox         *m_show_all{nullptr};
    wxCheckBox         *m_only_mine{nullptr};
    wxDataViewListCtrl *m_list{nullptr};
    Button             *m_btn_set{nullptr};
    Button             *m_btn_clear{nullptr};
    Button             *m_btn_clear_all{nullptr};
    Button             *m_btn_ok{nullptr};
    Button             *m_btn_cancel{nullptr};
};

// "Set my price" for one preset: the family (default) or this preset only. True when saved.
bool edit_filament_price(wxWindow *parent, const PresetCollection &filaments);

// Opens the table. True when prices changed (and were saved).
bool show_filament_price_dialog(wxWindow *parent);

// After a save: every plate's G-code is out of date, the Filament tab's note too.
void notify_filament_prices_changed();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FilamentPriceDialog_hpp_
