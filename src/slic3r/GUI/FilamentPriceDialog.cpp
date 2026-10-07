#include "FilamentPriceDialog.hpp"

#include <wx/checkbox.h>
#include <wx/dataview.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cmath>
#include <locale>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r {
namespace GUI {

using FilamentPrices::Entry;
using FilamentPrices::Identity;
using FilamentPrices::Resolved;
using FilamentPrices::Source;
using FilamentPrices::Store;

// ------------------------------------------------------------------ money ----

static const char *CURRENCY_KEY = "cost_currency_symbol";

// The user's locale currency symbol, UTF-8, or "" when the system will not say.
static std::string locale_currency_symbol()
{
#ifdef _WIN32
    wchar_t buf[16] = {0};
    if (::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SCURRENCY, buf, int(sizeof(buf) / sizeof(buf[0]))) > 0)
        return into_u8(wxString(buf));
    return {};
#else
    try {
        std::locale loc("");
        std::string symbol = std::use_facet<std::moneypunct<char>>(loc).curr_symbol();
        boost::trim(symbol);
        // Only trust it when it is valid UTF-8 (the usual case on macOS / Linux).
        return wxString::FromUTF8(symbol.c_str()).empty() && !symbol.empty() ? std::string() : symbol;
    } catch (...) {
        return {};
    }
#endif
}

std::string currency_symbol()
{
    AppConfig *config = wxGetApp().app_config;
    if (config == nullptr)
        return "$";
    std::string symbol;
    if (config->get("app", CURRENCY_KEY, symbol))
        return symbol;   // set (possibly to nothing on purpose)
    symbol = locale_currency_symbol();
    if (symbol.empty())
        symbol = "$";
    config->set(CURRENCY_KEY, symbol);
    return symbol;
}

wxString format_money(double amount)
{
    return from_u8(currency_symbol()) + wxString::Format("%.2f", amount);
}

static wxString per_kg(double amount) { return format_money(amount) + "/kg"; }

// "23.50", "23,50", "$23.50", " 23.5 /kg" -> 23.5. Empty -> false with empty = true.
static bool parse_price(wxString text, double &out, bool &empty)
{
    text.Trim(true).Trim(false);
    const wxString symbol = from_u8(currency_symbol());
    if (!symbol.empty())
        text.Replace(symbol, "");
    text.Replace("/kg", "");
    text.Replace(" ", "");
    text.Replace(",", ".");
    empty = text.empty();
    if (empty)
        return false;
    double value = 0.;
    if (!text.ToCDouble(&value) || !std::isfinite(value) || value < 0.)
        return false;
    out = value;
    return true;
}

// ----------------------------------------------------------- the Filament tab ----

EditedFilamentPrice edited_filament_price(const PresetCollection &filaments)
{
    EditedFilamentPrice out;
    const Preset &edited = filaments.get_edited_preset();
    const auto   *cost   = edited.config.option<ConfigOptionFloats>("filament_cost");
    const double  price  = cost != nullptr && !cost->values.empty() ? cost->get_at(0) : 0.;
    // Named after the selected preset; the edited copy carries its name and flags.
    out.id       = FilamentPrices::identify(edited, price, &filaments);
    out.resolved = FilamentPrices::global()->resolve(out.id, price);
    return out;
}

static wxString family_label(const Identity &id)
{
    wxString label = from_u8(id.family);
    std::vector<std::string> parts;
    if (!id.vendor.empty())
        parts.push_back(id.vendor);
    if (!id.type.empty())
        parts.push_back(id.type);
    if (!parts.empty()) {
        std::string joined;
        for (const std::string &p : parts)
            joined += (joined.empty() ? "" : ", ") + p;
        label += " (" + from_u8(joined) + ")";
    }
    return label;
}

wxString filament_price_note(const EditedFilamentPrice &price)
{
    const Resolved &r = price.resolved;
    switch (r.source) {
    case Source::PresetOnly:
        return format_wxstr(_L("Your price for this preset applies: %1% (preset value %2%)."), per_kg(r.price), per_kg(r.preset_price));
    case Source::Family:
        return format_wxstr(_L("Your price applies: %1% for every %2% (preset value %3%)."), per_kg(r.price), family_label(price.id),
                            per_kg(r.preset_price));
    case Source::OwnPrice:
        if (r.family_shadowed)
            return format_wxstr(_L("This preset's own price applies (%1%): you set it different from its parent's %2%, so your price "
                                   "for %3% (%4%) is not used here."),
                                per_kg(r.preset_price), per_kg(price.id.parent_price), family_label(price.id), per_kg(r.family_price));
        return _L("This preset's own price applies.");
    case Source::Preset:
    default:
        return format_wxstr(_L("No price of your own for %1%: slicing uses this preset's price."), family_label(price.id));
    }
}

// --------------------------------------------------------------- the popup ----

class FilamentPriceEditDialog : public DPIDialog
{
public:
    FilamentPriceEditDialog(wxWindow *parent, const EditedFilamentPrice &price, const Store &store)
        : DPIDialog(parent, wxID_ANY, _L("Your filament price"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_price(price)
    {
        const Entry *preset_entry = store.find_preset(price.id.preset);
        const Entry *family_entry = store.find_family(price.id.key());

        auto *top = new wxBoxSizer(wxVERTICAL);
        auto *intro = new wxStaticText(this, wxID_ANY,
                                       _L("Your price is kept on this computer, outside the presets, and applies to every project you "
                                          "slice here. It is never written into project files."));
        intro->Wrap(FromDIP(420));
        top->Add(intro, 0, wxEXPAND | wxALL, FromDIP(12));

        m_family = new wxRadioButton(this, wxID_ANY, format_wxstr(_L("Every %1%, all printers and nozzles"), family_label(price.id)),
                                     wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_preset = new wxRadioButton(this, wxID_ANY, format_wxstr(_L("This preset only: %1%"), from_u8(price.id.preset)));
        top->Add(m_family, 0, wxLEFT | wxRIGHT, FromDIP(12));
        top->Add(m_preset, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        if (price.id.family.empty())
            m_family->Disable();
        (preset_entry != nullptr || price.id.family.empty() ? m_preset : m_family)->SetValue(true);

        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, _L("Price") + " (" + from_u8(currency_symbol()) + "/kg)"), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_value = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), wxTE_PROCESS_ENTER);
        row->Add(m_value, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(new wxStaticText(this, wxID_ANY, format_wxstr(_L("preset value %1%"), per_kg(price.resolved.preset_price))), 0,
                 wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        top->Add(row, 0, wxALL, FromDIP(12));

        auto fill = [this, preset_entry, family_entry]() {
            const Entry *e = m_preset->GetValue() ? preset_entry : family_entry;
            m_value->SetValue(e != nullptr ? wxString::Format("%.2f", e->price_per_kg) : wxString());
            m_btn_remove->Enable(e != nullptr);
        };

        auto *btns = new wxBoxSizer(wxHORIZONTAL);
        m_btn_remove = new ::Button(this, _L("Remove my price"));
        m_btn_remove->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
        m_btn_remove->SetToolTip(_L("Slicing goes back to the preset's price for this choice"));
        auto *ok_btn = new ::Button(this, _L("OK"));
        ok_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        auto *cancel_btn = new ::Button(this, _L("Cancel"));
        cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        cancel_btn->SetId(wxID_CANCEL);
        const int gap = FromDIP(ButtonProps::ChoiceButtonGap());
        btns->Add(m_btn_remove, 0, wxALIGN_CENTER_VERTICAL);
        btns->AddStretchSpacer();
        btns->Add(ok_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
        btns->Add(cancel_btn, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizer(top);
        top->SetSizeHints(this);
        CenterOnParent();
        SetEscapeId(wxID_CANCEL);

        m_family->Bind(wxEVT_RADIOBUTTON, [fill](wxCommandEvent &) { fill(); });
        m_preset->Bind(wxEVT_RADIOBUTTON, [fill](wxCommandEvent &) { fill(); });
        auto accept = [this]() {
            double value = 0.;
            bool   empty = false;
            if (!parse_price(m_value->GetValue(), value, empty)) {
                if (empty) {
                    m_remove = true;
                    EndModal(wxID_OK);
                    return;
                }
                show_error(this, _L("Enter a price per kilogram of 0 or more, for example 23.50."));
                return;
            }
            m_new_price = value;
            EndModal(wxID_OK);
        };
        ok_btn->Bind(wxEVT_BUTTON, [accept](wxCommandEvent &) { accept(); });
        m_value->Bind(wxEVT_TEXT_ENTER, [accept](wxCommandEvent &) { accept(); });
        m_btn_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_remove = true;
            EndModal(wxID_OK);
        });
        cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        fill();
        m_value->SetFocus();
        m_value->SelectAll();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    // Applies the choice to `store`; false when nothing changed.
    bool apply(Store &store) const
    {
        if (m_preset->GetValue()) {
            if (m_remove)
                return store.clear_preset(m_price.id.preset);
            return store.set_preset(m_price.id.preset, m_price.id, m_new_price);
        }
        if (m_remove)
            return store.clear_family(m_price.id.key());
        return store.set_family(m_price.id.vendor, m_price.id.type, m_price.id.family, m_new_price);
    }

protected:
    void on_dpi_changed(const wxRect &) override { Refresh(); }
    void on_sys_color_changed() override {}

private:
    EditedFilamentPrice m_price;
    wxRadioButton      *m_family{nullptr};
    wxRadioButton      *m_preset{nullptr};
    wxTextCtrl         *m_value{nullptr};
    ::Button           *m_btn_remove{nullptr};
    bool                m_remove{false};
    double              m_new_price{0.};
};

bool edit_filament_price(wxWindow *parent, const PresetCollection &filaments)
{
    const EditedFilamentPrice price = edited_filament_price(filaments);
    Store                     store = *FilamentPrices::global();
    FilamentPriceEditDialog   dlg(parent, price, store);
    if (dlg.ShowModal() != wxID_OK || !dlg.apply(store))
        return false;
    if (!FilamentPrices::save_global(store))
        show_error(parent, format_wxstr(_L("Could not save your filament prices to %1%. They apply until EdgeSlicer is closed."),
                                        from_u8(FilamentPrices::store_path())));
    notify_filament_prices_changed();
    return true;
}

// ---------------------------------------------------------------- the table ----

FilamentPriceDialog::FilamentPriceDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Filament prices"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(*FilamentPrices::global())
{
    const int em = GetTextExtent("m").x;
    auto     *top = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
                                   _L("Your price per kilogram applies to every printer and nozzle variant of a filament, over the "
                                      "presets' own prices. A user preset you gave a price of its own keeps it. Prices are saved on "
                                      "this computer and apply to every project you slice here; they are never written into project "
                                      "files. Leave \"Your price\" empty to use the preset's price."));
    intro->Wrap(90 * em);
    top->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

    auto *filters = new wxBoxSizer(wxHORIZONTAL);
    m_search      = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(28 * em, -1));
    m_search->SetHint(_L("Search vendor, filament or type"));
    m_show_all  = new wxCheckBox(this, wxID_ANY, _L("Show all installed filaments"));
    m_show_all->SetToolTip(_L("Off: only the filaments you picked for your printers, and the ones with a price of yours."));
    m_only_mine = new wxCheckBox(this, wxID_ANY, _L("Only my prices"));
    filters->Add(m_search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    filters->Add(m_show_all, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    filters->Add(m_only_mine, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(filters, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES | wxDV_SINGLE);
    m_list->AppendTextColumn(_L("Vendor"), wxDATAVIEW_CELL_INERT, 13 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    m_list->AppendTextColumn(_L("Filament"), wxDATAVIEW_CELL_INERT, 26 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    m_list->AppendTextColumn(_L("Type"), wxDATAVIEW_CELL_INERT, 8 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    m_list->AppendTextColumn(_L("Preset price"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Your price"), wxDATAVIEW_CELL_EDITABLE, 10 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    top->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto  make = [this](const wxString &label, ButtonStyle style) {
        auto *b = new ::Button(this, label);
        b->SetStyle(style, ButtonType::Choice);
        return b;
    };
    m_btn_set       = make(_L("Set price") + dots, ButtonStyle::Regular);
    m_btn_clear     = make(_L("Clear"), ButtonStyle::Regular);
    m_btn_clear->SetToolTip(_L("Remove your price from the selected row: the preset's price applies again"));
    m_btn_clear_all = make(_L("Clear all"), ButtonStyle::Alert);
    m_btn_ok        = make(_L("OK"), ButtonStyle::Confirm);
    m_btn_cancel    = make(_L("Cancel"), ButtonStyle::Regular);
    m_btn_cancel->SetId(wxID_CANCEL);
    const int gap = FromDIP(ButtonProps::ChoiceButtonGap());
    btns->Add(m_btn_set, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(m_btn_clear, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(m_btn_clear_all, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->AddStretchSpacer();
    btns->Add(m_btn_ok, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(m_btn_cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxALL, FromDIP(10));

    SetSizer(top);
    SetSize(wxSize(80 * em, 40 * em));
    CenterOnParent();

    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { reload(); });
    m_show_all->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { reload(); });
    m_only_mine->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { reload(); });
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent &) { update_buttons(); });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent &evt) { on_value_changed(evt); });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent &) { on_set_price(); });
    m_btn_set->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_set_price(); });
    m_btn_clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_clear(); });
    m_btn_clear_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_clear_all(); });
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_changed && !FilamentPrices::save_global(m_store))
            show_error(this, format_wxstr(_L("Could not save your filament prices to %1%. They apply until EdgeSlicer is closed."),
                                          from_u8(FilamentPrices::store_path())));
        EndModal(wxID_OK);
    });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_changed = false;
        EndModal(wxID_CANCEL);
    });
    SetEscapeId(wxID_CANCEL);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(m_list);

    build_rows();
    reload();
    m_search->SetFocus();
}

void FilamentPriceDialog::build_rows()
{
    m_rows.clear();
    std::map<std::string, size_t> by_key;
    if (PresetBundle *bundle = wxGetApp().preset_bundle) {
        const PresetCollection &filaments = bundle->filaments;
        for (const Preset &preset : filaments) {
            if (preset.is_default)
                continue;
            const auto  *cost  = preset.config.option<ConfigOptionFloats>("filament_cost");
            const double price = cost != nullptr && !cost->values.empty() ? cost->get_at(0) : 0.;
            const Identity id  = FilamentPrices::identify(preset, price, &filaments);
            if (id.family.empty())
                continue;
            const std::string key = id.key();
            auto              it  = by_key.find(key);
            if (it == by_key.end()) {
                Row row;
                row.key    = key;
                row.vendor = id.vendor;
                row.type   = id.type;
                row.family = id.family;
                it         = by_key.emplace(key, m_rows.size()).first;
                m_rows.push_back(row);
            }
            Row &row = m_rows[it->second];
            ++row.presets;
            row.visible |= preset.is_visible && !preset.is_project_embedded;
            if (!row.has_price) {
                row.min_price = row.max_price = price;
                row.has_price                 = true;
            } else {
                row.min_price = std::min(row.min_price, price);
                row.max_price = std::max(row.max_price, price);
            }
        }
    }
    // Prices of yours for families that are not installed (vendor profile removed or renamed), and
    // every "this preset only" price: one row each, so nothing you set is ever hidden.
    for (const Entry &e : m_store.entries()) {
        if (e.scope == Entry::Scope::Preset) {
            Row row;
            row.preset_scope = true;
            row.key          = e.preset;
            row.preset       = e.preset;
            row.vendor       = e.vendor;
            row.type         = e.type;
            row.family       = e.family;
            if (PresetBundle *bundle = wxGetApp().preset_bundle)
                if (const Preset *p = bundle->filaments.find_preset(e.preset, false); p != nullptr && p->name == e.preset) {
                    const auto *cost = p->config.option<ConfigOptionFloats>("filament_cost");
                    row.presets      = 1;
                    row.has_price    = cost != nullptr && !cost->values.empty();
                    row.min_price = row.max_price = row.has_price ? cost->get_at(0) : 0.;
                    row.visible                   = true;
                }
            m_rows.push_back(row);
        } else if (by_key.find(e.key()) == by_key.end()) {
            Row row;
            row.key    = e.key();
            row.vendor = e.vendor;
            row.type   = e.type;
            row.family = e.family;
            by_key.emplace(row.key, m_rows.size());
            m_rows.push_back(row);
        }
    }
    std::sort(m_rows.begin(), m_rows.end(), [](const Row &a, const Row &b) {
        const std::string va = boost::algorithm::to_lower_copy(a.vendor), vb = boost::algorithm::to_lower_copy(b.vendor);
        if (va != vb)
            return va < vb;
        const std::string fa = boost::algorithm::to_lower_copy(a.family), fb = boost::algorithm::to_lower_copy(b.family);
        if (fa != fb)
            return fa < fb;
        if (a.type != b.type)
            return a.type < b.type;
        return a.preset_scope < b.preset_scope;
    });
}

const Entry *FilamentPriceDialog::entry_of(const Row &row) const
{
    return row.preset_scope ? m_store.find_preset(row.preset) : m_store.find_family(row.key);
}

void FilamentPriceDialog::reload()
{
    m_reloading = true;
    const Row  *keep_row = selected_row();
    std::string keep_key = keep_row ? keep_row->key : std::string();
    const bool  keep_scope = keep_row ? keep_row->preset_scope : false;

    const std::string needle   = boost::algorithm::to_lower_copy(into_u8(m_search->GetValue()));
    const bool        show_all = m_show_all->GetValue();
    const bool        mine     = m_only_mine->GetValue();

    m_list->DeleteAllItems();
    m_shown.clear();
    int select = wxNOT_FOUND;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const Row   &row   = m_rows[i];
        const Entry *entry = entry_of(row);
        if (mine && entry == nullptr)
            continue;
        if (!show_all && !row.visible && entry == nullptr)
            continue;
        if (!needle.empty()) {
            const std::string hay = boost::algorithm::to_lower_copy(row.vendor + " " + row.family + " " + row.type + " " + row.preset);
            if (hay.find(needle) == std::string::npos)
                continue;
        }
        wxString filament = from_u8(row.family);
        if (row.preset_scope)
            filament = from_u8(row.preset) + " " + _L("(this preset only)");
        else if (row.presets == 0)
            filament += " " + _L("(not installed)");
        else if (row.presets > 1)
            filament += " " + format_wxstr(_L("(%1% presets)"), row.presets);
        wxString preset_price;
        if (row.has_price)
            preset_price = std::abs(row.max_price - row.min_price) < 1e-6 ? per_kg(row.min_price) :
                                                                            format_money(row.min_price) + " - " + per_kg(row.max_price);
        wxVector<wxVariant> values;
        values.push_back(wxVariant(from_u8(row.vendor)));
        values.push_back(wxVariant(filament));
        values.push_back(wxVariant(from_u8(row.type)));
        values.push_back(wxVariant(preset_price));
        values.push_back(wxVariant(entry != nullptr ? wxString::Format("%.2f", entry->price_per_kg) : wxString()));
        m_list->AppendItem(values);
        if (!keep_key.empty() && row.key == keep_key && row.preset_scope == keep_scope)
            select = int(m_shown.size());
        m_shown.push_back(i);
    }
    if (select != wxNOT_FOUND)
        m_list->SelectRow(select);
    m_reloading = false;
    update_buttons();
}

const FilamentPriceDialog::Row *FilamentPriceDialog::selected_row() const
{
    if (m_list == nullptr)
        return nullptr;
    const int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND || size_t(row) >= m_shown.size())
        return nullptr;
    return &m_rows[m_shown[size_t(row)]];
}

void FilamentPriceDialog::update_buttons()
{
    const Row *row = selected_row();
    m_btn_set->Enable(row != nullptr);
    m_btn_clear->Enable(row != nullptr && entry_of(*row) != nullptr);
    m_btn_clear_all->Enable(!m_store.empty());
}

bool FilamentPriceDialog::set_price(const Row &row, const wxString &text)
{
    double value = 0.;
    bool   empty = false;
    if (!parse_price(text, value, empty)) {
        if (!empty) {
            show_error(this, _L("Enter a price per kilogram of 0 or more, for example 23.50, or leave it empty to use the preset's price."));
            return false;
        }
        const bool cleared = row.preset_scope ? m_store.clear_preset(row.preset) : m_store.clear_family(row.key);
        m_changed |= cleared;
        return cleared;
    }
    bool ok = false;
    if (row.preset_scope) {
        Identity id;
        id.vendor = row.vendor;
        id.type   = row.type;
        id.family = row.family;
        ok        = m_store.set_preset(row.preset, id, value);
    } else
        ok = m_store.set_family(row.vendor, row.type, row.family, value);
    m_changed |= ok;
    return ok;
}

void FilamentPriceDialog::on_value_changed(wxDataViewEvent &evt)
{
    if (m_reloading || evt.GetColumn() != COL_YOUR_PRICE)
        return;
    const int r = m_list->ItemToRow(evt.GetItem());
    if (r == wxNOT_FOUND || size_t(r) >= m_shown.size())
        return;
    const Row row = m_rows[m_shown[size_t(r)]];
    wxVariant value;
    m_list->GetValue(value, unsigned(r), COL_YOUR_PRICE);
    set_price(row, value.GetString());
    // Re-read the table on the next turn: changing rows inside the editor's own event is not safe.
    CallAfter([this]() { reload(); });
}

void FilamentPriceDialog::on_set_price()
{
    const Row *row = selected_row();
    if (row == nullptr)
        return;
    const Entry   *entry = entry_of(*row);
    const wxString what  = row->preset_scope ? from_u8(row->preset) : from_u8(row->family) + " (" + from_u8(row->vendor) + ", " + from_u8(row->type) + ")";
    wxTextEntryDialog dlg(this, format_wxstr(_L("Your price per kilogram for %1%, in %2%. Leave it empty to use the preset's price."), what,
                                             from_u8(currency_symbol())),
                          _L("Set price"), entry != nullptr ? wxString::Format("%.2f", entry->price_per_kg) : wxString());
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const Row copy = *row;
    set_price(copy, dlg.GetValue());
    reload();
}

void FilamentPriceDialog::on_clear()
{
    const Row *row = selected_row();
    if (row == nullptr)
        return;
    const bool cleared = row->preset_scope ? m_store.clear_preset(row->preset) : m_store.clear_family(row->key);
    m_changed |= cleared;
    reload();
}

void FilamentPriceDialog::on_clear_all()
{
    MessageDialog ask(this, _L("Remove every price of yours? Slicing goes back to the presets' prices."), _L("Filament prices"),
                      wxICON_QUESTION | wxYES_NO);
    if (ask.ShowModal() != wxID_YES)
        return;
    m_changed |= !m_store.empty();
    m_store.clear_all();
    reload();
}

void FilamentPriceDialog::on_dpi_changed(const wxRect &)
{
    const int em = GetTextExtent("m").x;
    SetSize(wxSize(80 * em, 40 * em));
    Refresh();
}

bool show_filament_price_dialog(wxWindow *parent)
{
    if (wxGetApp().preset_bundle == nullptr)
        return false;
    FilamentPriceDialog dlg(parent);
    const bool          saved = dlg.ShowModal() == wxID_OK && dlg.changed();
    if (saved)
        notify_filament_prices_changed();
    return saved;
}

void notify_filament_prices_changed()
{
    // filament_cost only feeds the G-code export: the next slice of each plate re-runs just that.
    if (Plater *plater = wxGetApp().plater())
        plater->post_slice_state_change_update();
    if (auto *tab = dynamic_cast<TabFilament *>(wxGetApp().get_tab(Preset::TYPE_FILAMENT)))
        tab->update_price_note();
}

} // namespace GUI
} // namespace Slic3r
