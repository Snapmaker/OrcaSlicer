#include "CostsDialog.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/simplebook.h>
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
#include "GLCanvas3D.hpp"
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
#include <functional>
#include <locale>
#include <map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r {
namespace GUI {

using CostOverrides::Entry;
using CostOverrides::Identity;
using CostOverrides::MachineEntry;
using CostOverrides::MachineIdentity;
using CostOverrides::MachineResolved;
using CostOverrides::MachineSource;
using CostOverrides::Resolved;
using CostOverrides::Source;
using CostOverrides::Store;

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

std::string display_vendor(const std::string &vendor)
{
    // The BBL vendor profile is called "Bambulab"; its printers' model names say "Bambu Lab".
    if (boost::algorithm::to_lower_copy(vendor) == "bambulab")
        return "Bambu Lab";
    return vendor;
}

static wxString per_kg(double amount) { return format_money(amount) + "/kg"; }
static wxString per_h(double amount) { return format_money(amount) + "/h"; }

// "23.50", "23,50", "$23.50", " 23.5 /kg" -> 23.5. Empty -> false with empty = true.
static bool parse_money(wxString text, double &out, bool &empty)
{
    text.Trim(true).Trim(false);
    const wxString symbol = from_u8(currency_symbol());
    if (!symbol.empty())
        text.Replace(symbol, "");
    text.Replace("/kg", "");
    text.Replace("/h", "");
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

static double first_float(const DynamicPrintConfig &config, const char *key)
{
    if (const auto *opt = config.option<ConfigOptionFloats>(key); opt != nullptr && !opt->values.empty())
        return opt->get_at(0);
    if (const auto *opt = config.option<ConfigOptionFloat>(key); opt != nullptr)
        return opt->value;
    return 0.;
}

static void save_or_warn(wxWindow *parent, const Store &store)
{
    if (!CostOverrides::save_global(store))
        show_error(parent, format_wxstr(_L("Could not save your costs to %1%. They apply until EdgeSlicer is closed."),
                                        from_u8(CostOverrides::store_path())));
}

// ------------------------------------------------------------- the tab notes ----

EditedFilamentPrice edited_filament_price(const PresetCollection &filaments)
{
    EditedFilamentPrice out;
    const Preset &edited = filaments.get_edited_preset();
    const double  price  = first_float(edited.config, "filament_cost");
    out.id               = CostOverrides::identify(edited, price, &filaments);
    out.resolved         = CostOverrides::global()->resolve(out.id, price);
    return out;
}

static wxString family_label(const Identity &id)
{
    wxString                 label = from_u8(id.family);
    std::string              joined;
    for (const std::string &p : {id.vendor, id.type})
        if (!p.empty())
            joined += (joined.empty() ? "" : ", ") + p;
    if (!joined.empty())
        label += " (" + from_u8(joined) + ")";
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

EditedMachineRate edited_machine_rate(const PresetCollection &printers)
{
    EditedMachineRate out;
    const Preset &edited = printers.get_edited_preset();
    const double  rate   = first_float(edited.config, "time_cost");
    out.id               = CostOverrides::identify_machine(edited, rate, &printers);
    out.resolved         = CostOverrides::global()->resolve_machine(out.id, rate);
    return out;
}

wxString machine_rate_note(const EditedMachineRate &rate)
{
    const MachineResolved &r     = rate.resolved;
    const wxString         model = from_u8(rate.id.model);
    switch (r.source) {
    case MachineSource::PresetOnly:
        return format_wxstr(_L("Your rate for this printer preset applies: %1% (preset value %2%)."), per_h(r.rate), per_h(r.preset_rate));
    case MachineSource::Model:
        return format_wxstr(_L("Your rate applies: %1% for every %2% (preset value %3%)."), per_h(r.rate), model, per_h(r.preset_rate));
    case MachineSource::Default:
        return format_wxstr(_L("Your default rate applies: %1% (preset value %2%)."), per_h(r.rate), per_h(r.preset_rate));
    case MachineSource::OwnRate:
        if (r.shadowed)
            return format_wxstr(_L("This preset's own time cost applies (%1%): you set it different from its parent's %2%, so your "
                                   "rate (%3%) is not used here."),
                                per_h(r.preset_rate), per_h(rate.id.parent_rate), per_h(r.shadowed_rate));
        return _L("This preset's own time cost applies.");
    case MachineSource::Preset:
    default:
        return format_wxstr(_L("No rate of your own for %1%: slicing uses this preset's time cost."), model);
    }
}

// --------------------------------------------------------------- the popup ----

// One value of yours for either everything of a kind (a filament family, a printer model) or one
// preset, or its removal.
class ScopedValueDialog : public DPIDialog
{
public:
    struct Setup
    {
        wxString title, intro, all_label, preset_label, unit;
        double   preset_value{0.};
        bool     has_all{false}, has_preset{false};
        double   all_value{0.}, preset_only_value{0.};
        bool     all_possible{true};
    };

    ScopedValueDialog(wxWindow *parent, const Setup &setup)
        : DPIDialog(parent, wxID_ANY, setup.title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE), m_setup(setup)
    {
        auto *top   = new wxBoxSizer(wxVERTICAL);
        auto *intro = new wxStaticText(this, wxID_ANY, setup.intro);
        intro->Wrap(FromDIP(420));
        top->Add(intro, 0, wxEXPAND | wxALL, FromDIP(12));

        m_all    = new wxRadioButton(this, wxID_ANY, setup.all_label, wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_preset = new wxRadioButton(this, wxID_ANY, setup.preset_label);
        top->Add(m_all, 0, wxLEFT | wxRIGHT, FromDIP(12));
        top->Add(m_preset, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        if (!setup.all_possible)
            m_all->Disable();
        (setup.has_preset || !setup.all_possible ? m_preset : m_all)->SetValue(true);

        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, setup.unit), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_value = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), wxTE_PROCESS_ENTER);
        row->Add(m_value, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(new wxStaticText(this, wxID_ANY, format_wxstr(_L("preset value %1%"), format_money(setup.preset_value))), 0,
                 wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        top->Add(row, 0, wxALL, FromDIP(12));

        auto *btns   = new wxBoxSizer(wxHORIZONTAL);
        m_btn_remove = new ::Button(this, _L("Remove"));
        m_btn_remove->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
        m_btn_remove->SetToolTip(_L("Slicing goes back to the preset's value for this choice"));
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

        m_all->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &) { fill(); });
        m_preset->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent &) { fill(); });
        auto accept = [this]() {
            bool empty = false;
            if (!parse_money(m_value->GetValue(), m_new_value, empty)) {
                if (empty) {
                    m_remove = true;
                    EndModal(wxID_OK);
                    return;
                }
                show_error(this, _L("Enter an amount of 0 or more, for example 23.50."));
                return;
            }
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

    bool   preset_only() const { return m_preset->GetValue(); }
    bool   remove() const { return m_remove; }
    double value() const { return m_new_value; }

protected:
    void on_dpi_changed(const wxRect &) override { Refresh(); }
    void on_sys_color_changed() override {}

private:
    void fill()
    {
        const bool   preset = m_preset->GetValue();
        const bool   has    = preset ? m_setup.has_preset : m_setup.has_all;
        const double value  = preset ? m_setup.preset_only_value : m_setup.all_value;
        m_value->SetValue(has ? wxString::Format("%.2f", value) : wxString());
        m_btn_remove->Enable(has);
    }

    Setup          m_setup;
    wxRadioButton *m_all{nullptr};
    wxRadioButton *m_preset{nullptr};
    wxTextCtrl    *m_value{nullptr};
    ::Button      *m_btn_remove{nullptr};
    bool           m_remove{false};
    double         m_new_value{0.};
};

static const wxString &kept_note()
{
    static const wxString note = _L("Kept on this computer, outside the presets, and used for every project you slice here. "
                                    "Never written into project files.");
    return note;
}

bool edit_filament_price(wxWindow *parent, const PresetCollection &filaments)
{
    const EditedFilamentPrice price = edited_filament_price(filaments);
    Store                     store = *CostOverrides::global();
    const Entry              *fam   = store.find_family(price.id.key());
    const Entry              *one   = store.find_preset(price.id.preset);

    ScopedValueDialog::Setup setup;
    setup.title        = _L("Your filament price");
    setup.intro        = kept_note();
    setup.all_label    = format_wxstr(_L("Every %1%, all printers and nozzles"), family_label(price.id));
    setup.preset_label = format_wxstr(_L("This preset only: %1%"), from_u8(price.id.preset));
    setup.unit         = _L("Price") + " (" + from_u8(currency_symbol()) + "/kg)";
    setup.preset_value = price.resolved.preset_price;
    setup.has_all      = fam != nullptr;
    setup.all_value    = fam ? fam->price_per_kg : 0.;
    setup.has_preset   = one != nullptr;
    setup.preset_only_value = one ? one->price_per_kg : 0.;
    setup.all_possible = !price.id.family.empty();

    ScopedValueDialog dlg(parent, setup);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    bool changed = false;
    if (dlg.preset_only())
        changed = dlg.remove() ? store.clear_preset(price.id.preset) : store.set_preset(price.id.preset, price.id, dlg.value());
    else
        changed = dlg.remove() ? store.clear_family(price.id.key()) :
                                 store.set_family(price.id.vendor, price.id.type, price.id.family, dlg.value());
    if (!changed)
        return false;
    save_or_warn(parent, store);
    notify_costs_changed();
    return true;
}

bool edit_machine_rate(wxWindow *parent, const PresetCollection &printers)
{
    const EditedMachineRate rate  = edited_machine_rate(printers);
    Store                   store = *CostOverrides::global();
    const MachineEntry     *model = store.find_machine_model(rate.id.key());
    const MachineEntry     *one   = store.find_machine_preset(rate.id.preset);

    ScopedValueDialog::Setup setup;
    setup.title        = _L("Your machine rate");
    setup.intro        = _L("What an hour of printing costs you on this printer (power, wear, your time).") + " " + kept_note();
    setup.all_label    = format_wxstr(_L("Every %1%, all nozzle variants"), from_u8(rate.id.model));
    setup.preset_label = format_wxstr(_L("This printer preset only: %1%"), from_u8(rate.id.preset));
    setup.unit         = _L("Rate") + " (" + from_u8(currency_symbol()) + "/h)";
    setup.preset_value = rate.resolved.preset_rate;
    setup.has_all      = model != nullptr;
    setup.all_value    = model ? model->rate_per_h : 0.;
    setup.has_preset   = one != nullptr;
    setup.preset_only_value = one ? one->rate_per_h : 0.;
    setup.all_possible = !rate.id.model.empty();

    ScopedValueDialog dlg(parent, setup);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    bool changed = false;
    if (dlg.preset_only())
        changed = dlg.remove() ? store.clear_machine_preset(rate.id.preset) : store.set_machine_preset(rate.id.preset, rate.id, dlg.value());
    else
        changed = dlg.remove() ? store.clear_machine_model(rate.id.key()) : store.set_machine_model(rate.id.vendor, rate.id.model, dlg.value());
    if (!changed)
        return false;
    save_or_warn(parent, store);
    notify_costs_changed();
    return true;
}

// ---------------------------------------------------------------- the table ----

CostsDialog::CostsDialog(wxWindow *parent, Page page)
    : DPIDialog(parent, wxID_ANY, _L("Costs"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(*CostOverrides::global())
{
    m_machines.machines = true;
    const int em        = GetTextExtent("m").x;
    auto     *top       = new wxBoxSizer(wxVERTICAL);


    // Two tabs: the fork's own buttons over a simplebook (follows the dark palette, unlike wxNotebook).
    auto *tabs      = new wxBoxSizer(wxHORIZONTAL);
    m_tab_filaments = new ::Button(this, _L("Filaments"));
    m_tab_machines  = new ::Button(this, _L("Machines"));
    m_tab_project   = new ::Button(this, _L("Project"));
    m_tab_project->SetToolTip(_L("Fees and markup: this project's selling price"));
    tabs->Add(m_tab_filaments, 0, wxRIGHT, FromDIP(6));
    tabs->Add(m_tab_machines, 0, wxRIGHT, FromDIP(6));
    tabs->Add(m_tab_project, 0);
    top->Add(tabs, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_book = new wxSimplebook(this, wxID_ANY);
    m_book->AddPage(build_page(m_book, m_filaments), _L("Filaments"));
    m_book->AddPage(build_page(m_book, m_machines), _L("Machines"));
    m_book->AddPage(build_project_page(m_book), _L("Project"));
    top->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto *btns   = new wxBoxSizer(wxHORIZONTAL);
    m_btn_ok     = new ::Button(this, _L("OK"));
    m_btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_cancel = new ::Button(this, _L("Cancel"));
    m_btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_cancel->SetId(wxID_CANCEL);
    btns->AddStretchSpacer();
    btns->Add(m_btn_ok, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
    btns->Add(m_btn_cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxALL, FromDIP(10));

    SetSizer(top);
    SetSize(wxSize(84 * em, 44 * em));
    CenterOnParent();

    m_tab_filaments->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_page(Page::Filaments); });
    m_tab_machines->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_page(Page::Machines); });
    m_tab_project->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { show_page(Page::Project); });
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        commit_default_rate();
        // A value that is not a number keeps the window open on the Project page.
        if (!commit_project())
            return;
        if (m_changed || m_pricing_changed)
            save_or_warn(this, m_store);
        EndModal(wxID_OK);
    });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_changed = m_pricing_changed = m_project_changed = false;
        EndModal(wxID_CANCEL);
    });
    SetEscapeId(wxID_CANCEL);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(m_filaments.list);
    wxGetApp().UpdateDVCDarkUI(m_machines.list);

    build_filament_rows();
    build_machine_rows();
    reload(m_filaments);
    reload(m_machines);
    show_page(page);
}

wxWindow *CostsDialog::build_page(wxWindow *parent, Table &table)
{
    const int em    = GetTextExtent("m").x;
    auto     *panel = new wxPanel(parent);
    auto     *top   = new wxBoxSizer(wxVERTICAL);

    if (table.machines) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(panel, wxID_ANY, _L("Default rate for every printer without a rate of yours") + " (" +
                                                       from_u8(currency_symbol()) + "/h):"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_default_rate = new wxTextCtrl(panel, wxID_ANY, m_store.has_default_rate() ? wxString::Format("%.2f", m_store.default_rate()) : wxString(),
                                        wxDefaultPosition, wxSize(8 * em, -1), wxTE_PROCESS_ENTER);
        m_default_rate->SetHint(_L("none"));
        m_default_rate->SetToolTip(_L("Used for every printer that has no rate of yours (model or preset), over the preset's time cost. "
                                      "Leave it empty to use the presets' time cost."));
        m_default_rate->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { commit_default_rate(); });
        m_default_rate->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) {
            commit_default_rate();
            e.Skip();
        });
        row->Add(m_default_rate, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(row, 0, wxBOTTOM, FromDIP(8));
    }

    auto *filters  = new wxBoxSizer(wxHORIZONTAL);
    table.search   = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(28 * em, -1));
    table.search->SetHint(table.machines ? _L("Search vendor or model") : _L("Search vendor, filament or type"));
    table.show_all = new wxCheckBox(panel, wxID_ANY, table.machines ? _L("Show all installed printers") : _L("Show all installed filaments"));
    table.show_all->SetToolTip(table.machines ? _L("Off: only the printers you added, and the ones with a rate of yours.") :
                                                _L("Off: only the filaments you picked for your printers, and the ones with a price of yours."));
    table.only_mine = new wxCheckBox(panel, wxID_ANY, table.machines ? _L("Only my rates") : _L("Only my prices"));
    filters->Add(table.search, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    filters->Add(table.show_all, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    filters->Add(table.only_mine, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(filters, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    table.list = new wxDataViewListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES | wxDV_SINGLE);
    const int sortable = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE;
    table.list->AppendTextColumn(_L("Vendor"), wxDATAVIEW_CELL_INERT, 13 * em, wxALIGN_LEFT, sortable);
    if (table.machines) {
        table.list->AppendTextColumn(_L("Model"), wxDATAVIEW_CELL_INERT, 34 * em, wxALIGN_LEFT, sortable);
        table.list->AppendTextColumn(_L("Preset time cost"), wxDATAVIEW_CELL_INERT, 13 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
        table.list->AppendTextColumn(_L("Your rate"), wxDATAVIEW_CELL_EDITABLE, 10 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
        table.list->AppendTextColumn(_L("Applied rate"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    } else {
        table.list->AppendTextColumn(_L("Filament"), wxDATAVIEW_CELL_INERT, 26 * em, wxALIGN_LEFT, sortable);
        table.list->AppendTextColumn(_L("Type"), wxDATAVIEW_CELL_INERT, 8 * em, wxALIGN_LEFT, sortable);
        table.list->AppendTextColumn(_L("Preset price"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
        table.list->AppendTextColumn(_L("Your price"), wxDATAVIEW_CELL_EDITABLE, 10 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
        table.list->AppendTextColumn(_L("Applied price"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_RIGHT, wxDATAVIEW_COL_RESIZABLE);
    }
    top->Add(table.list, 1, wxEXPAND);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto  make = [panel](const wxString &label, ButtonStyle style) {
        auto *b = new ::Button(panel, label);
        b->SetStyle(style, ButtonType::Choice);
        return b;
    };
    table.btn_set       = make((table.machines ? _L("Set rate") : _L("Set price")) + dots, ButtonStyle::Regular);
    table.btn_clear     = make(_L("Clear"), ButtonStyle::Regular);
    table.btn_clear->SetToolTip(_L("Remove your value from the selected row: the preset's value applies again"));
    table.btn_clear_all = make(_L("Clear all"), ButtonStyle::Alert);
    const int gap       = FromDIP(ButtonProps::ChoiceButtonGap());
    btns->Add(table.btn_set, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(table.btn_clear, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(table.btn_clear_all, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxTOP, FromDIP(8));
    panel->SetSizer(top);

    Table *t = &table;
    table.search->Bind(wxEVT_TEXT, [this, t](wxCommandEvent &) { reload(*t); });
    table.show_all->Bind(wxEVT_CHECKBOX, [this, t](wxCommandEvent &) { reload(*t); });
    table.only_mine->Bind(wxEVT_CHECKBOX, [this, t](wxCommandEvent &) { reload(*t); });
    table.list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this, t](wxDataViewEvent &) { update_buttons(*t); });
    table.list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this, t](wxDataViewEvent &evt) { on_value_changed(*t, evt); });
    table.list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this, t](wxDataViewEvent &) { on_set(*t); });
    table.btn_set->Bind(wxEVT_BUTTON, [this, t](wxCommandEvent &) { on_set(*t); });
    table.btn_clear->Bind(wxEVT_BUTTON, [this, t](wxCommandEvent &) {
        if (const Row *row = selected_row(*t)) {
            const Row copy = *row;
            clear_value(*t, copy);
            reload(*t);
        }
    });
    table.btn_clear_all->Bind(wxEVT_BUTTON, [this, t](wxCommandEvent &) { on_clear_all(*t); });
    return panel;
}

void CostsDialog::show_page(Page page)
{
    m_book->SetSelection(page == Page::Project ? 2 : page == Page::Machines ? 1 : 0);
    m_tab_filaments->SetStyle(page == Page::Filaments ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    m_tab_machines->SetStyle(page == Page::Machines ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    m_tab_project->SetStyle(page == Page::Project ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    if (page != Page::Project)
        (page == Page::Machines ? m_machines : m_filaments).search->SetFocus();
    Layout();
}

void CostsDialog::commit_default_rate()
{
    if (m_default_rate == nullptr)
        return;
    double value = 0.;
    bool   empty = false;
    bool   changed = false;
    if (parse_money(m_default_rate->GetValue(), value, empty)) {
        if (!m_store.has_default_rate() || std::abs(m_store.default_rate() - value) > 1e-9)
            changed = m_store.set_default_rate(value);
    } else if (empty) {
        changed = m_store.clear_default_rate();
    } else {
        // Not a number: show what applies.
        m_default_rate->ChangeValue(m_store.has_default_rate() ? wxString::Format("%.2f", m_store.default_rate()) : wxString());
    }
    if (changed) {
        m_changed = true;
        // The Applied rate column follows the default rate. Not inside the list's own events.
        CallAfter([this]() { reload(m_machines); });
    }
}

static void add_preset_value(double value, bool &has, double &lo, double &hi)
{
    if (!has) {
        lo = hi = value;
        has     = true;
    } else {
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
}

void CostsDialog::build_filament_rows()
{
    std::vector<Row> &rows = m_filaments.rows;
    rows.clear();
    std::map<std::string, size_t> by_key;
    PresetBundle                 *bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const PresetCollection &filaments = bundle->filaments;
        for (const Preset &preset : filaments) {
            if (preset.is_default)
                continue;
            const double   price = first_float(preset.config, "filament_cost");
            const Identity id    = CostOverrides::identify(preset, price, &filaments);
            if (id.family.empty())
                continue;
            const std::string key = id.key();
            auto              it  = by_key.find(key);
            if (it == by_key.end()) {
                Row row;
                row.key    = key;
                row.vendor = id.vendor;
                row.type   = id.type;
                row.name   = id.family;
                it         = by_key.emplace(key, rows.size()).first;
                rows.push_back(row);
            }
            Row &row = rows[it->second];
            ++row.presets;
            row.visible |= preset.is_visible && !preset.is_project_embedded;
            add_preset_value(price, row.has_value, row.min_value, row.max_value);
        }
    }
    // Values of yours whose presets are not installed, and every "this preset only" value: one row
    // each, so nothing you set is ever hidden.
    for (const Entry &e : m_store.entries()) {
        if (e.scope == Entry::Scope::Preset) {
            Row row;
            row.preset_scope = true;
            row.key          = e.preset;
            row.preset       = e.preset;
            row.vendor       = e.vendor;
            row.type         = e.type;
            row.name         = e.family;
            if (bundle != nullptr)
                if (const Preset *p = bundle->filaments.find_preset(e.preset, false); p != nullptr && p->name == e.preset) {
                    row.presets = 1;
                    row.visible = true;
                    add_preset_value(first_float(p->config, "filament_cost"), row.has_value, row.min_value, row.max_value);
                }
            rows.push_back(row);
        } else if (by_key.find(e.key()) == by_key.end()) {
            Row row;
            row.key    = e.key();
            row.vendor = e.vendor;
            row.type   = e.type;
            row.name   = e.family;
            by_key.emplace(row.key, rows.size());
            rows.push_back(row);
        }
    }
}

void CostsDialog::build_machine_rows()
{
    std::vector<Row> &rows = m_machines.rows;
    rows.clear();
    std::map<std::string, size_t> by_key;
    PresetBundle                 *bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const PresetCollection &printers = bundle->printers;
        for (const Preset &preset : printers) {
            if (preset.is_default || preset.printer_technology() != ptFFF)
                continue;
            const double          rate = first_float(preset.config, "time_cost");
            const MachineIdentity id   = CostOverrides::identify_machine(preset, rate, &printers);
            if (id.model.empty())
                continue;
            const std::string key = id.key();
            auto              it  = by_key.find(key);
            if (it == by_key.end()) {
                Row row;
                row.key    = key;
                row.vendor = id.vendor;
                row.name   = id.model;
                it         = by_key.emplace(key, rows.size()).first;
                rows.push_back(row);
            }
            Row &row = rows[it->second];
            ++row.presets;
            row.visible |= preset.is_visible && !preset.is_project_embedded;
            add_preset_value(rate, row.has_value, row.min_value, row.max_value);
        }
    }
    for (const MachineEntry &e : m_store.machines()) {
        if (e.scope == MachineEntry::Scope::Preset) {
            Row row;
            row.preset_scope = true;
            row.key          = e.preset;
            row.preset       = e.preset;
            row.vendor       = e.vendor;
            row.name         = e.model;
            if (bundle != nullptr)
                if (const Preset *p = bundle->printers.find_preset(e.preset, false); p != nullptr && p->name == e.preset) {
                    row.presets = 1;
                    row.visible = true;
                    add_preset_value(first_float(p->config, "time_cost"), row.has_value, row.min_value, row.max_value);
                }
            rows.push_back(row);
        } else if (by_key.find(e.key()) == by_key.end()) {
            Row row;
            row.key    = e.key();
            row.vendor = e.vendor;
            row.name   = e.model;
            by_key.emplace(row.key, rows.size());
            rows.push_back(row);
        }
    }
}

bool CostsDialog::value_of(const Table &table, const Row &row, double &out) const
{
    if (table.machines) {
        const MachineEntry *e = row.preset_scope ? m_store.find_machine_preset(row.preset) : m_store.find_machine_model(row.key);
        if (e != nullptr)
            out = e->rate_per_h;
        return e != nullptr;
    }
    const Entry *e = row.preset_scope ? m_store.find_preset(row.preset) : m_store.find_family(row.key);
    if (e != nullptr)
        out = e->price_per_kg;
    return e != nullptr;
}

void CostsDialog::reload(Table &table)
{
    m_reloading            = true;
    const Row  *keep_row   = selected_row(table);
    std::string keep_key   = keep_row ? keep_row->key : std::string();
    const bool  keep_scope = keep_row ? keep_row->preset_scope : false;

    const std::string needle   = boost::algorithm::to_lower_copy(into_u8(table.search->GetValue()));
    const bool        show_all = table.show_all->GetValue();
    const bool        mine     = table.only_mine->GetValue();
    auto              unit     = [&table](double v) { return table.machines ? per_h(v) : per_kg(v); };

    std::vector<size_t> order(table.rows.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&table](size_t ia, size_t ib) {
        const Row        &a  = table.rows[ia];
        const Row        &b  = table.rows[ib];
        const std::string va = boost::algorithm::to_lower_copy(display_vendor(a.vendor)),
                          vb = boost::algorithm::to_lower_copy(display_vendor(b.vendor));
        if (va != vb)
            return va < vb;
        const std::string na = boost::algorithm::to_lower_copy(a.name), nb = boost::algorithm::to_lower_copy(b.name);
        if (na != nb)
            return na < nb;
        if (a.type != b.type)
            return a.type < b.type;
        return a.preset_scope < b.preset_scope;
    });

    table.list->DeleteAllItems();
    table.shown.clear();
    int select = wxNOT_FOUND;
    for (size_t i : order) {
        const Row &row = table.rows[i];
        double     yours = 0.;
        const bool has_yours = value_of(table, row, yours);
        if (mine && !has_yours)
            continue;
        if (!show_all && !row.visible && !has_yours)
            continue;
        if (!needle.empty()) {
            const std::string hay = boost::algorithm::to_lower_copy(display_vendor(row.vendor) + " " + row.vendor + " " + row.name + " " +
                                                                    row.type + " " + row.preset);
            if (hay.find(needle) == std::string::npos)
                continue;
        }
        wxString name = from_u8(row.name);
        if (row.preset_scope)
            name = from_u8(row.preset) + " " + _L("(this preset only)");
        else if (row.presets == 0)
            name += " " + _L("(not installed)");
        else if (row.presets > 1)
            name += " " + format_wxstr(_L("(%1% presets)"), row.presets);
        wxString preset_value;
        if (row.has_value)
            preset_value = std::abs(row.max_value - row.min_value) < 1e-6 ? unit(row.min_value) :
                                                                            format_money(row.min_value) + " - " + unit(row.max_value);
        wxVector<wxVariant> values;
        values.push_back(wxVariant(from_u8(display_vendor(row.vendor))));
        values.push_back(wxVariant(name));
        if (!table.machines)
            values.push_back(wxVariant(from_u8(row.type)));
        values.push_back(wxVariant(preset_value));
        values.push_back(wxVariant(has_yours ? wxString::Format("%.2f", yours) : wxString()));
        // What slicing uses: yours when set, else (machines) your default rate, else the preset's. A user
        // preset with a value of its own keeps it.
        wxString applied = has_yours ? unit(yours) : preset_value;
        if (!has_yours && table.machines && m_store.has_default_rate())
            applied = unit(m_store.default_rate());
        values.push_back(wxVariant(applied));
        table.list->AppendItem(values);
        if (!keep_key.empty() && row.key == keep_key && row.preset_scope == keep_scope)
            select = int(table.shown.size());
        table.shown.push_back(i);
    }
    if (select != wxNOT_FOUND)
        table.list->SelectRow(select);
    m_reloading = false;
    update_buttons(table);
}

const CostsDialog::Row *CostsDialog::selected_row(const Table &table) const
{
    if (table.list == nullptr)
        return nullptr;
    const int row = table.list->GetSelectedRow();
    if (row == wxNOT_FOUND || size_t(row) >= table.shown.size())
        return nullptr;
    return &table.rows[table.shown[size_t(row)]];
}

void CostsDialog::update_buttons(Table &table)
{
    const Row *row   = selected_row(table);
    double     dummy = 0.;
    table.btn_set->Enable(row != nullptr);
    table.btn_clear->Enable(row != nullptr && value_of(table, *row, dummy));
    table.btn_clear_all->Enable(table.machines ? !m_store.machines().empty() : !m_store.entries().empty());
}

bool CostsDialog::clear_value(Table &table, const Row &row)
{
    bool cleared = false;
    if (table.machines)
        cleared = row.preset_scope ? m_store.clear_machine_preset(row.preset) : m_store.clear_machine_model(row.key);
    else
        cleared = row.preset_scope ? m_store.clear_preset(row.preset) : m_store.clear_family(row.key);
    m_changed |= cleared;
    return cleared;
}

bool CostsDialog::set_value(Table &table, const Row &row, const wxString &text)
{
    double value = 0.;
    bool   empty = false;
    if (!parse_money(text, value, empty)) {
        if (!empty) {
            show_error(this, _L("Enter an amount of 0 or more, for example 23.50, or leave it empty to use the preset's value."));
            return false;
        }
        return clear_value(table, row);
    }
    bool ok = false;
    if (table.machines) {
        if (row.preset_scope) {
            MachineIdentity id;
            id.vendor = row.vendor;
            id.model  = row.name;
            ok        = m_store.set_machine_preset(row.preset, id, value);
        } else
            ok = m_store.set_machine_model(row.vendor, row.name, value);
    } else {
        if (row.preset_scope) {
            Identity id;
            id.vendor = row.vendor;
            id.type   = row.type;
            id.family = row.name;
            ok        = m_store.set_preset(row.preset, id, value);
        } else
            ok = m_store.set_family(row.vendor, row.type, row.name, value);
    }
    m_changed |= ok;
    return ok;
}

void CostsDialog::on_value_changed(Table &table, wxDataViewEvent &evt)
{
    const unsigned value_col = table.machines ? 3 : 4;
    if (m_reloading || evt.GetColumn() != int(value_col))
        return;
    const int r = table.list->ItemToRow(evt.GetItem());
    if (r == wxNOT_FOUND || size_t(r) >= table.shown.size())
        return;
    const Row row = table.rows[table.shown[size_t(r)]];
    wxVariant value;
    table.list->GetValue(value, unsigned(r), value_col);
    set_value(table, row, value.GetString());
    // Re-read the table on the next turn: changing rows inside the editor's own event is not safe.
    Table *t = &table;
    CallAfter([this, t]() { reload(*t); });
}

void CostsDialog::on_set(Table &table)
{
    const Row *row = selected_row(table);
    if (row == nullptr)
        return;
    double         current = 0.;
    const bool     has     = value_of(table, *row, current);
    const wxString what    = row->preset_scope ? from_u8(row->preset) :
                             table.machines    ? from_u8(row->name) :
                                                 from_u8(row->name) + " (" + from_u8(display_vendor(row->vendor)) + ", " + from_u8(row->type) + ")";
    const wxString prompt  = table.machines ?
                                 format_wxstr(_L("Your rate per hour for %1%, in %2%. Leave it empty to use the preset's time cost."), what,
                                              from_u8(currency_symbol())) :
                                 format_wxstr(_L("Your price per kilogram for %1%, in %2%. Leave it empty to use the preset's price."), what,
                                              from_u8(currency_symbol()));
    wxTextEntryDialog dlg(this, prompt, table.machines ? _L("Set rate") : _L("Set price"),
                          has ? wxString::Format("%.2f", current) : wxString());
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const Row copy = *row;
    set_value(table, copy, dlg.GetValue());
    reload(table);
}

void CostsDialog::on_clear_all(Table &table)
{
    MessageDialog ask(this,
                      table.machines ? _L("Remove every machine rate of yours, the default rate included? Slicing goes back to the "
                                          "presets' time cost.") :
                                       _L("Remove every filament price of yours? Slicing goes back to the presets' prices."),
                      _L("Costs"), wxICON_QUESTION | wxYES_NO);
    if (ask.ShowModal() != wxID_YES)
        return;
    if (table.machines) {
        m_changed |= !m_store.machines().empty() || m_store.has_default_rate();
        m_store.clear_all_machines();
        if (m_default_rate != nullptr)
            m_default_rate->ChangeValue(wxString());
    } else {
        m_changed |= !m_store.entries().empty();
        m_store.clear_all_filaments();
    }
    reload(table);
}

void CostsDialog::on_dpi_changed(const wxRect &)
{
    const int em = GetTextExtent("m").x;
    SetSize(wxSize(84 * em, 44 * em));
    Refresh();
}

bool show_costs_dialog(wxWindow *parent, CostsDialog::Page page)
{
    if (wxGetApp().preset_bundle == nullptr)
        return false;
    CostsDialog dlg(parent, page);
    if (dlg.ShowModal() != wxID_OK)
        return false;
    // The project's own fees / markup are saved with it: a change is a modified project.
    if (dlg.project_changed())
        if (Plater *plater = wxGetApp().plater())
            plater->set_plater_dirty(true);
    if (dlg.changed())
        notify_costs_changed();
    else if (dlg.pricing_changed())
        notify_pricing_changed();
    return dlg.changed() || dlg.pricing_changed();
}

void notify_pricing_changed()
{
    // Display only: nothing is resliced, the cost breakdown is drawn again.
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    if (GLCanvas3D *canvas = plater->get_current_canvas3D()) {
        canvas->set_as_dirty();
        canvas->request_extra_frame();
    }
}


// ---------------------------------------------------------------- the project page ----

// "2.50" -> "2.5", "3.00" -> "3": hours and percentages.
static wxString short_number(double value)
{
    wxString s = wxString::Format("%.2f", value);
    while (s.EndsWith("0"))
        s.RemoveLast();
    if (s.EndsWith("."))
        s.RemoveLast();
    return s;
}

// "35", "35%", "2,5", "$2.50" -> a number >= 0. Empty is 0.
static bool parse_amount(wxString text, double &out)
{
    text.Replace("%", "");
    text.Replace("/h", "");
    text.Replace("h", "");
    bool empty = false;
    if (parse_money(text, out, empty))
        return true;
    if (empty) {
        out = 0.;
        return true;
    }
    return false;
}

static wxString pricing_field_name(PricingField field)
{
    switch (field) {
    case PricingField::Markup: return _L("Markup");
    case PricingField::AssemblyHours: return _L("Assembly time per plate");
    case PricingField::AssemblyRate: return _L("Assembly rate");
    case PricingField::ObjectFee: return _L("Fee per object");
    case PricingField::PartFee: return _L("Fee per part");
    case PricingField::PlateFee: return _L("Setup fee per plate");
    case PricingField::Packaging: return _L("Packaging per plate");
    default: return {};
    }
}

static wxString pricing_field_unit(PricingField field)
{
    const wxString symbol = from_u8(currency_symbol());
    switch (field) {
    case PricingField::AssemblyHours: return _L("h");
    case PricingField::AssemblyRate: return symbol + "/h";
    case PricingField::Markup: return {};
    default: return symbol;
    }
}

static wxString pricing_field_tip(PricingField field)
{
    switch (field) {
    case PricingField::Markup:
        return _L("Percent: a markup on cost, 100% doubles it (a margin on the price is markup / (100 + markup)). Of material: only the "
                  "filament is marked up. Of total cost: material, machine time and every fee below.\n"
                  "Flat: a fixed amount added once per plate.");
    case PricingField::AssemblyHours: return _L("Hours of work (removing supports, assembly, finishing) charged for each plate.");
    case PricingField::AssemblyRate: return _L("What an hour of that work is charged.");
    case PricingField::ObjectFee:
        return _L("Charged for every object on the plate: every printable copy counts, so 4 copies of a part pay it 4 times.");
    case PricingField::PartFee:
        return _L("Charged for every part of every object on the plate. Only model parts count: modifiers, negative parts, support "
                  "blockers and enforcers do not.");
    case PricingField::PlateFee: return _L("Charged once for each plate (preparing the printer, the bed, the job).");
    case PricingField::Packaging: return _L("Charged once for each plate. A project of 3 plates pays it 3 times.");
    default: return {};
    }
}

// The value of one field as text, for the "Default Cost" and "Applied Cost" columns.
static wxString pricing_field_text(const PricingSettings &s, PricingField field)
{
    switch (field) {
    case PricingField::Markup:
        if (!s.has_markup())
            return _L("none");
        if (s.markup_type == MarkupType::Flat)
            return format_wxstr(_L("%1% per plate"), format_money(s.markup_value));
        return s.markup_basis == MarkupBasis::Material ? format_wxstr(_L("%1%%% of material"), short_number(s.markup_value)) :
                                                         format_wxstr(_L("%1%%% of total cost"), short_number(s.markup_value));
    case PricingField::AssemblyHours: return short_number(s.assembly_hours) + " " + _L("h");
    case PricingField::AssemblyRate: return format_money(s.assembly_rate_per_h) + "/h";
    case PricingField::ObjectFee: return format_money(s.fee_per_object);
    case PricingField::PartFee: return format_money(s.fee_per_part);
    case PricingField::PlateFee: return format_money(s.fee_per_plate);
    case PricingField::Packaging: return format_money(s.packaging_per_plate);
    default: return {};
    }
}

static double pricing_number(const PricingSettings &s, PricingField field)
{
    switch (field) {
    case PricingField::Markup: return s.markup_value;
    case PricingField::AssemblyHours: return s.assembly_hours;
    case PricingField::AssemblyRate: return s.assembly_rate_per_h;
    case PricingField::ObjectFee: return s.fee_per_object;
    case PricingField::PartFee: return s.fee_per_part;
    case PricingField::PlateFee: return s.fee_per_plate;
    case PricingField::Packaging: return s.packaging_per_plate;
    default: return 0.;
    }
}

static void set_pricing_number(PricingSettings &s, PricingField field, double value)
{
    switch (field) {
    case PricingField::Markup: s.markup_value = value; break;
    case PricingField::AssemblyHours: s.assembly_hours = value; break;
    case PricingField::AssemblyRate: s.assembly_rate_per_h = value; break;
    case PricingField::ObjectFee: s.fee_per_object = value; break;
    case PricingField::PartFee: s.fee_per_part = value; break;
    case PricingField::PlateFee: s.fee_per_plate = value; break;
    case PricingField::Packaging: s.packaging_per_plate = value; break;
    default: break;
    }
}

wxWindow *CostsDialog::build_project_page(wxWindow *parent)
{
    const int em    = GetTextExtent("m").x;
    auto     *panel = new wxPanel(parent);
    auto     *top   = new wxBoxSizer(wxVERTICAL);

    Plater *plater = wxGetApp().plater();
    const ProjectPricing project  = plater != nullptr ? plater->model().pricing : ProjectPricing();
    const PricingSettings defaults = m_store.pricing();
    m_project_values               = project.resolve(defaults);

    // Description | Use Default | Default Cost | This project | Applied Cost
    auto *grid = new wxFlexGridSizer(5, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(3);
    auto bold = [panel](const wxString &text) {
        auto *t = new wxStaticText(panel, wxID_ANY, text);
        t->SetFont(t->GetFont().Bold());
        return t;
    };
    grid->Add(bold(_L("Description")));
    grid->Add(bold(_L("Use Default")));
    grid->Add(bold(_L("Default Cost")));
    grid->Add(bold(_L("This project")));
    grid->Add(bold(_L("Applied Cost")));

    m_pricing_rows.clear();
    m_pricing_rows.reserve(PRICING_FIELDS);
    for (size_t i = 0; i < PRICING_FIELDS; ++i) {
        PricingRow row;
        row.field = PricingField(i);
        auto *label = new wxStaticText(panel, wxID_ANY, pricing_field_name(row.field) + ":");
        label->SetToolTip(pricing_field_tip(row.field));
        grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);

        // A toggle in the app's button style: highlighted while the default applies.
        row.use_default    = new ::Button(panel, _L("Use Default"));
        row.use_default_on = !project.is_set(row.field);
        row.use_default->SetToolTip(_L("On: this field follows your default. Off: this project's own value below applies."));
        grid->Add(row.use_default, 0, wxALIGN_CENTER_VERTICAL);

        row.default_text = new wxStaticText(panel, wxID_ANY, wxEmptyString);
        row.default_text->SetToolTip(_L("Your default, used by every project that does not set its own value."));
        grid->Add(row.default_text, 0, wxALIGN_CENTER_VERTICAL);

        auto *editors = new wxBoxSizer(wxHORIZONTAL);
        if (row.field == PricingField::Markup) {
            row.type = new wxChoice(panel, wxID_ANY);
            row.type->Append(_L("Percent"));
            row.type->Append(_L("Flat per plate"));
            row.basis = new wxChoice(panel, wxID_ANY);
            row.basis->Append(_L("of material"));
            row.basis->Append(_L("of total cost"));
            row.basis->SetToolTip(_L("What a percent markup is taken of. Total cost: material, machine time and the fees."));
            editors->Add(row.type, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        }
        row.value = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(8 * em, -1));
        row.value->SetToolTip(pricing_field_tip(row.field));
        editors->Add(row.value, 0, wxALIGN_CENTER_VERTICAL);
        if (row.field == PricingField::Markup) {
            editors->Add(row.basis, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        } else {
            editors->Add(new wxStaticText(panel, wxID_ANY, pricing_field_unit(row.field)), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        }
        grid->Add(editors, 0, wxALIGN_CENTER_VERTICAL);

        row.applied_text = new wxStaticText(panel, wxID_ANY, wxEmptyString);
        row.applied_text->SetToolTip(_L("What the cost breakdown uses for this project."));
        grid->Add(row.applied_text, 0, wxALIGN_CENTER_VERTICAL);
        m_pricing_rows.push_back(row);
    }
    top->Add(grid, 0, wxEXPAND);

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *save = new ::Button(panel, _L("Save as my defaults"));
    save->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    save->SetToolTip(_L("Make the Applied Cost values your defaults, for every project that does not set its own."));
    btns->Add(save, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxTOP, FromDIP(12));
    if (plater == nullptr)
        panel->Disable();
    panel->SetSizer(top);

    for (PricingRow &row : m_pricing_rows) {
        update_pricing_row(row);
        const size_t idx = size_t(row.field);
        row.use_default->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) { on_use_default(m_pricing_rows[idx]); });
        // Applied Cost follows every keystroke and choice.
        row.value->Bind(wxEVT_TEXT, [this, idx](wxCommandEvent &) { update_applied_text(m_pricing_rows[idx]); });
        if (row.type != nullptr) {
            row.type->Bind(wxEVT_CHOICE, [this, idx](wxCommandEvent &) { update_pricing_row(m_pricing_rows[idx]); });
            row.basis->Bind(wxEVT_CHOICE, [this, idx](wxCommandEvent &) { update_applied_text(m_pricing_rows[idx]); });
        }
    }
    update_default_texts();
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_save_as_defaults(); });
    return panel;
}

void CostsDialog::show_pricing_value(const PricingRow &row, const PricingSettings &from)
{
    if (row.field == PricingField::Markup) {
        row.type->SetSelection(from.markup_type == MarkupType::Flat ? 1 : 0);
        row.basis->SetSelection(from.markup_basis == MarkupBasis::Material ? 0 : 1);
        row.value->ChangeValue(from.markup_type == MarkupType::Flat ? wxString::Format("%.2f", from.markup_value) :
                                                                       short_number(from.markup_value));
        return;
    }
    const double value = pricing_number(from, row.field);
    row.value->ChangeValue(row.field == PricingField::AssemblyHours ? short_number(value) : wxString::Format("%.2f", value));
}

bool CostsDialog::read_pricing_value(const PricingRow &row, PricingSettings &to, wxString &error) const
{
    double value = 0.;
    if (!parse_amount(row.value->GetValue(), value)) {
        error = format_wxstr(_L("%1%: \"%2%\" is not a number of 0 or more."), pricing_field_name(row.field), row.value->GetValue());
        return false;
    }
    if (row.field == PricingField::Markup) {
        to.markup_type  = row.type->GetSelection() == 1 ? MarkupType::Flat : MarkupType::Percent;
        to.markup_basis = row.basis->GetSelection() == 0 ? MarkupBasis::Material : MarkupBasis::Overall;
    }
    set_pricing_number(to, row.field, value);
    return true;
}

void CostsDialog::update_pricing_row(const PricingRow &row)
{
    const bool own = !row.use_default_on;
    row.use_default->SetValue(row.use_default_on);
    row.use_default->SetStyle(row.use_default_on ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    row.use_default->Refresh();
    if (!own)
        show_pricing_value(row, m_store.pricing());
    row.value->Enable(own);
    if (row.type != nullptr) {
        row.type->Enable(own);
        // The basis is for a percent markup; a flat amount is added as it is.
        row.basis->Enable(own && row.type->GetSelection() == 0);
    }
    if (own && row.field == PricingField::Markup)
        row.value->SetHint(row.type->GetSelection() == 1 ? from_u8(currency_symbol()) : wxString("%"));
    update_applied_text(row);
}

void CostsDialog::update_applied_text(const PricingRow &row)
{
    if (row.applied_text == nullptr)
        return;
    wxString text;
    if (row.use_default_on) {
        text = pricing_field_text(m_store.pricing(), row.field);
    } else {
        PricingSettings value = m_project_values;
        wxString        error;
        text = read_pricing_value(row, value, error) ? pricing_field_text(value, row.field) : _L("not a number");
    }
    if (row.applied_text->GetLabel() != text) {
        row.applied_text->SetLabel(text);
        row.applied_text->GetParent()->Layout();
    }
}

void CostsDialog::update_default_texts()
{
    for (const PricingRow &row : m_pricing_rows)
        row.default_text->SetLabel(pricing_field_text(m_store.pricing(), row.field));
    for (const PricingRow &row : m_pricing_rows)
        update_applied_text(row);
    if (!m_pricing_rows.empty())
        m_pricing_rows.front().default_text->GetParent()->Layout();
}

void CostsDialog::on_use_default(PricingRow &row)
{
    row.use_default_on = !row.use_default_on;
    if (row.use_default_on) {
        // Remember what was typed, in case the toggle is switched off again.
        wxString error;
        read_pricing_value(row, m_project_values, error);
    } else {
        show_pricing_value(row, m_project_values);
    }
    update_pricing_row(row);
}

bool CostsDialog::read_project(ProjectPricing &out, bool show_errors)
{
    Plater *plater = wxGetApp().plater();
    out            = plater != nullptr ? plater->model().pricing : ProjectPricing();
    for (const PricingRow &row : m_pricing_rows) {
        if (row.use_default_on) {
            out.clear_field(row.field);
            continue;
        }
        PricingSettings value = m_project_values;
        wxString        error;
        if (!read_pricing_value(row, value, error)) {
            if (show_errors) {
                show_page(Page::Project);
                show_error(this, error);
                row.value->SetFocus();
            }
            return false;
        }
        out.set_field(row.field, value);
    }
    return true;
}

void CostsDialog::on_save_as_defaults()
{
    ProjectPricing shown;
    if (!read_project(shown, true))
        return;
    const PricingSettings values = shown.resolve(m_store.pricing());
    if (values == m_store.pricing())
        return;
    if (m_store.set_pricing(values)) {
        m_pricing_changed = true;
        update_default_texts();
        for (const PricingRow &row : m_pricing_rows)
            update_pricing_row(row);
    }
}

bool CostsDialog::commit_project()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || m_pricing_rows.empty())
        return true;
    ProjectPricing pricing;
    if (!read_project(pricing, true))
        return false;
    if (pricing != plater->model().pricing) {
        plater->model().pricing = pricing;
        m_project_changed       = true;
    }
    return true;
}

void notify_costs_changed()
{
    // filament_cost and time_cost only feed the G-code export: the next slice of each plate re-runs just that.
    if (Plater *plater = wxGetApp().plater())
        plater->post_slice_state_change_update();
    if (auto *tab = dynamic_cast<TabFilament *>(wxGetApp().get_tab(Preset::TYPE_FILAMENT)))
        tab->update_price_note();
    if (auto *tab = dynamic_cast<TabPrinter *>(wxGetApp().get_tab(Preset::TYPE_PRINTER)))
        tab->update_rate_note();
}

} // namespace GUI
} // namespace Slic3r
