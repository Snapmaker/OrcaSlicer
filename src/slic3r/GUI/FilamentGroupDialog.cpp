#include "FilamentGroupDialog.hpp"

#include "FlowTypeHelper.hpp"
#include "GUI_App.hpp"
#include "HighFlowCompat.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>
#include <set>

namespace Slic3r { namespace GUI {

FilamentGroupDialog::FilamentGroupDialog(wxWindow* parent, bool default_all_high_flow)
    : DPIDialog(parent ? wxGetTopLevelParent(parent) : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Filament flow assignment"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const wxColour background = StateColor::darkModeColorFor(*wxWHITE);
    const wxColour foreground = StateColor::darkModeColorFor(wxColour("#242424"));
    SetBackgroundColour(background);
    SetForegroundColour(foreground);
    SetFont(Label::Body_13);

    load_filaments();
    m_mapping = wxGetApp().preset_bundle->get_filament_volume_types();
    m_mapping.resize(wxGetApp().preset_bundle->filament_presets.size(), fvtStandard);
    if (default_all_high_flow)
        for (const FilamentInfo& info : m_filaments)
            m_mapping[info.filament_idx] = fvtHighFlow;

    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* intro  = new wxStaticText(this, wxID_ANY, _L("Physical nozzle configuration (printer settings)"));
    intro->SetFont(Label::Head_14);
    layout->Add(intro, 0, wxALL, FromDIP(16));

    auto*                          nozzles      = new wxBoxSizer(wxHORIZONTAL);
    const std::vector<std::string> nozzle_types = FlowType::nozzle_volume_types();
    for (size_t i = 0; i < nozzle_types.size(); ++i) {
        auto* card = new wxPanel(this, wxID_ANY);
        card->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F4F4F4")));
        auto* contents = new wxBoxSizer(wxVERTICAL);
        auto* name     = new wxStaticText(card, wxID_ANY, _L("Nozzle") + wxString::Format(" %zu", i + 1));
        auto* type     = new wxStaticText(card, wxID_ANY, nozzle_types[i] == FLOW_MODE_HIGH_FLOW ? _L("Phaetus HF") : _L("Standard"));
        name->SetBackgroundColour(card->GetBackgroundColour());
        type->SetBackgroundColour(card->GetBackgroundColour());
        type->SetFont(Label::Body_14);
        contents->Add(name, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        contents->Add(type, 0, wxALL, FromDIP(12));
        card->SetSizer(contents);
        nozzles->Add(card, 1, wxEXPAND | (i + 1 < nozzle_types.size() ? wxRIGHT : 0), FromDIP(8));
    }
    layout->Add(nozzles, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    auto* print_intro = new wxStaticText(this, wxID_ANY, _L("Print settings: filament flow"));
    print_intro->SetFont(Label::Head_14);
    layout->Add(print_intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_summary     = new wxStaticText(this, wxID_ANY, wxEmptyString);
    actions->Add(m_summary, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    auto add_bulk_button = [this, actions, &nozzle_types](const wxString& label, bool high_flow) {
        auto* button = new Button(this, label);
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->Enable(std::find(nozzle_types.begin(), nozzle_types.end(), high_flow ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD) !=
                       nozzle_types.end());
        button->Bind(wxEVT_BUTTON, [this, high_flow](wxCommandEvent&) { assign_all(high_flow); });
        actions->Add(button, 0, wxLEFT, FromDIP(8));
    };
    add_bulk_button(_L("All Standard"), false);
    add_bulk_button(_L("All High Flow"), true);
    layout->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));

    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    m_list->SetBackgroundColour(background);
    m_list->SetScrollRate(0, FromDIP(24));
    const int visible_rows = std::clamp(int(m_filaments.size()), 1, 4);
    m_list->SetMinSize(wxSize(-1, FromDIP(40 + visible_rows * 68)));
    auto* rows = new wxFlexGridSizer(0, 4, FromDIP(12), FromDIP(12));
    rows->AddGrowableCol(1);
    rows->AddSpacer(FromDIP(28));
    rows->Add(new wxStaticText(m_list, wxID_ANY, _L("Filament")), 0, wxALIGN_CENTER_VERTICAL);
    rows->Add(new wxStaticText(m_list, wxID_ANY, _L("Print flow mode")), 0, wxALIGN_CENTER_VERTICAL);
    auto* flow_heading = new wxStaticText(m_list, wxID_ANY, _L("Applied filament settings"), wxDefaultPosition, wxDefaultSize,
                                          wxALIGN_RIGHT);
    flow_heading->Wrap(FromDIP(180));
    rows->Add(flow_heading, 0, wxALIGN_CENTER_VERTICAL);
    std::vector<wxStaticText*> numbers;
    for (const FilamentInfo& info : m_filaments) {
        auto* badge  = new wxPanel(m_list, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(28, 28)));
        auto* number = new wxStaticText(badge, wxID_ANY, wxString::Format("%zu", info.filament_idx + 1), wxDefaultPosition, wxDefaultSize,
                                        wxALIGN_CENTER);
        auto* badge_layout = new wxBoxSizer(wxVERTICAL);
        badge_layout->AddStretchSpacer();
        badge_layout->Add(number, 0, wxEXPAND);
        badge_layout->AddStretchSpacer();
        badge->SetSizer(badge_layout);
        numbers.push_back(number);
        rows->Add(badge, 0, wxALIGN_CENTER_VERTICAL);

        auto* name = new wxStaticText(m_list, wxID_ANY, from_u8(info.preset_name), wxDefaultPosition, wxSize(FromDIP(288), -1),
                                      wxST_ELLIPSIZE_END);
        name->SetToolTip(from_u8(info.preset_name));
        auto* name_layout = new wxBoxSizer(wxHORIZONTAL);
        name_layout->Add(name, 1, wxALIGN_CENTER_VERTICAL);
        rows->Add(name_layout, 1, wxEXPAND);

        auto* choice = new ComboBox(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        choice->Append(_L("Standard flow"));
        choice->Append(_L("High flow (HF)"));
        choice->SetMinSize(wxSize(FromDIP(144), FromDIP(32)));
        choice->SetName(format_wxstr(_L("Filament %1% flow settings"), info.filament_idx + 1));
        if (!info.has_high_flow_settings)
            choice->SetToolTip(_L("This filament has no High Flow preset. Its Standard settings will be used."));
        choice->Bind(wxEVT_COMBOBOX,
                     [this, index = info.filament_idx, choice](wxCommandEvent&) { move_filament(index, choice->GetSelection() == 1); });
        m_choices.push_back(choice);
        rows->Add(choice, 0, wxALIGN_CENTER_VERTICAL);

        auto* detail = new wxStaticText(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), wxALIGN_RIGHT);
        m_details.push_back(detail);
        rows->Add(detail, 0, wxALIGN_CENTER_VERTICAL);
    }
    m_list->SetSizer(rows);
    layout->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    m_warning_sizer = new wxBoxSizer(wxVERTICAL);
    layout->Add(m_warning_sizer, 0, wxEXPAND | wxALL, FromDIP(16));
    auto* buttons    = new DialogButtons(this, {"Cancel", "Confirm"});
    m_confirm_button = buttons->GetCONFIRM();
    m_confirm_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_confirm_button->IsEnabled())
            return;
        // A deliberate assignment must survive the mixed-nozzle slice path.
        // Automatic uniform mapping continues to use the existing standard mode.
        if (FlowType::distinct_nozzle_flow_type_count() > 1 && FlowType::grouping_mode() != FILAMENT_GROUPING_CUSTOM)
            FlowType::set_grouping_mode(FILAMENT_GROUPING_CUSTOM);
        FlowType::apply_custom_mapping(m_mapping);
        FlowType::remember_assignment();
        EndModal(wxID_OK);
    });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    layout->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    SetSizer(layout);
    SetMinSize(FromDIP(wxSize(820, 540)));
    update_assignments();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    intro->SetBackgroundColour(background);
    print_intro->SetBackgroundColour(background);
    m_summary->SetBackgroundColour(background);
    for (wxWindow* child : m_list->GetChildren())
        if (auto* text = dynamic_cast<wxStaticText*>(child))
            text->SetBackgroundColour(background);
    // Filament colors must not be changed by the generic dark-mode color walk.
    for (size_t row = 0; row < m_filaments.size(); ++row) {
        const wxColour& color = m_filaments[row].color;
        numbers[row]->SetBackgroundColour(color);
        numbers[row]->GetParent()->SetBackgroundColour(color);
        const double luminance = 0.299 * color.Red() + 0.587 * color.Green() + 0.114 * color.Blue();
        numbers[row]->SetForegroundColour(luminance < 160.0 ? *wxWHITE : *wxBLACK);
    }
}

void FilamentGroupDialog::load_filaments()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto*         colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    std::set<int>       used;
    if (Plater* plater = wxGetApp().plater(); plater != nullptr)
        used = plater->get_partplate_list().get_extruders(true);
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        if (!used.empty() && used.find(int(i) + 1) == used.end())
            continue;
        const std::string& name    = bundle.filament_presets[i];
        const Preset*      preset  = bundle.filaments.get_edited_preset().name == name ? &bundle.filaments.get_edited_preset() :
                                                                                         bundle.filaments.find_preset(name, false);
        const auto*        support = preset ? preset->config.option<ConfigOptionStrings>("filament_flow_support") : nullptr;
        m_filaments.push_back(
            {i, colors && i < colors->values.size() ? wxColour(from_u8(colors->values[i])) : *wxWHITE,
             preset ? preset->config.opt_string("filament_type", 0u) : std::string(), name,
             support && std::find(support->values.begin(), support->values.end(), FLOW_MODE_HIGH_FLOW) != support->values.end()});
    }
}

void FilamentGroupDialog::move_filament(size_t filament_idx, bool high_flow)
{
    if (filament_idx >= m_mapping.size())
        return;
    m_mapping[filament_idx] = high_flow ? fvtHighFlow : fvtStandard;
    update_assignments();
}

void FilamentGroupDialog::assign_all(bool high_flow)
{
    for (const FilamentInfo& info : m_filaments)
        m_mapping[info.filament_idx] = high_flow ? fvtHighFlow : fvtStandard;
    update_assignments();
}

void FilamentGroupDialog::update_assignments()
{
    DynamicPrintConfig preview = wxGetApp().preset_bundle->full_config();
    auto*              types   = preview.option<ConfigOptionEnumsGeneric>("filament_volume_type", true);
    types->values.assign(m_mapping.begin(), m_mapping.end());
    const auto* max_flow   = preview.option<ConfigOptionFloats>("filament_max_volumetric_speed");
    const auto* ratio      = preview.option<ConfigOptionFloats>("filament_flow_ratio");
    size_t      high_count = 0;
    for (size_t row = 0; row < m_filaments.size(); ++row) {
        const unsigned int index = static_cast<unsigned int>(m_filaments[row].filament_idx);
        const bool         high  = m_mapping[index] == fvtHighFlow;
        high_count += high;
        m_choices[row]->SetSelection(high ? 1 : 0);
        const bool fallback = high && !m_filaments[row].has_high_flow_settings;
        wxString   detail = fallback ? _L("Standard (no HF preset)") : high ? _L("High Flow preset values") : _L("Standard preset values");
        if (max_flow)
            detail += "\n" + wxString::Format(L"%.1f mm\u00b3/s", get_value_at(preview, *max_flow, ConfigFlowDomain::Filament, index));
        if (ratio)
            detail += "\n" + _L("Flow ratio") +
                      wxString::Format(": %.3f", get_value_at(preview, *ratio, ConfigFlowDomain::Filament, index));
        m_details[row]->SetLabel(detail);
        m_details[row]->Wrap(FromDIP(180));
        m_details[row]->SetForegroundColour(StateColor::darkModeColorFor(wxColour(fallback ? "#FF842D" : "#242424")));
        m_details[row]->Refresh();
    }
    m_summary->SetLabel(format_wxstr(_L("Filaments: %1% Standard, %2% High Flow"), m_filaments.size() - high_count, high_count));
    m_list->FitInside();
    update_warnings();
}

void FilamentGroupDialog::update_warnings()
{
    m_warning_sizer->Clear(true);
    const std::vector<std::string> nozzles            = FlowType::nozzle_volume_types();
    const bool                     standard_available = std::find(nozzles.begin(), nozzles.end(), FLOW_MODE_STANDARD) != nozzles.end();
    const bool                     high_available     = std::find(nozzles.begin(), nozzles.end(), FLOW_MODE_HIGH_FLOW) != nozzles.end();
    bool                           missing_standard   = false;
    bool                           missing_high       = false;
    bool                           unsupported        = false;
    wxString                       standard_fallback;
    std::set<std::pair<bool, std::string>> messages;
    for (const FilamentInfo& info : m_filaments) {
        const bool high = m_mapping[info.filament_idx] == fvtHighFlow;
        missing_high |= high && !high_available;
        missing_standard |= !high && !standard_available;
        if (!high)
            continue;
        if (!info.has_high_flow_settings) {
            if (!standard_fallback.empty())
                standard_fallback += ", ";
            standard_fallback += wxString::Format("%zu", info.filament_idx + 1);
        }
        const auto result = HighFlowCompat::check(info.type_raw, info.preset_name);
        if (result.level != HighFlowCompat::CompatibilityLevel::Compatible) {
            const bool error = result.level == HighFlowCompat::CompatibilityLevel::Unsupported;
            unsupported |= error;
            messages.emplace(error, result.material);
        }
    }
    auto add_warning = [this](const wxString& message, bool error) {
        auto* text = new wxStaticText(this, wxID_ANY, message);
        text->SetBackgroundColour(GetBackgroundColour());
        text->SetForegroundColour(StateColor::darkModeColorFor(wxColour(error ? "#D32F2F" : "#FF842D")));
        text->Wrap(FromDIP(704));
        m_warning_sizer->Add(text, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    };
    if (missing_standard)
        add_warning(_L("No Standard nozzle is configured. Assign these filaments to High Flow or update the nozzle settings."), true);
    if (missing_high)
        add_warning(_L("No High Flow nozzle is configured. Assign these filaments to Standard or update the nozzle settings."), true);
    if (!standard_fallback.empty())
        add_warning(format_wxstr(_L("Using Standard preset values for filaments without High Flow settings: %1%."), standard_fallback),
                    false);
    const std::string diameter = wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("printer_variant");
    for (bool error : {false, true}) {
        wxString materials;
        for (const auto& [is_error, material] : messages) {
            if (is_error != error)
                continue;
            if (!materials.empty())
                materials += ", ";
            materials += material == "CF/GF filaments" ? _L("CF/GF filaments") : from_u8(material);
        }
        if (!materials.empty())
            add_warning(error ?
                            format_wxstr(_L("These filaments cannot be printed with the %1%mm high flow nozzle: %2%"), from_u8(diameter),
                                         materials) :
                            format_wxstr(_L("Warning:%1%mm High Flow nozzles are not recommended for %2%"), from_u8(diameter), materials),
                        error);
    }
    const bool ready = !unsupported && !missing_standard && !missing_high;
    m_confirm_button->SetStyle(ready ? ButtonStyle::Confirm : ButtonStyle::Disabled, ButtonType::Choice);
    m_confirm_button->Enable(ready);
    wxSize size = GetSize();
    size.IncTo(GetSizer()->ComputeFittingWindowSize(this));
    SetSize(size);
    Layout();
}

void FilamentGroupDialog::on_dpi_changed(const wxRect&)
{
    Layout();
    Fit();
}

}} // namespace Slic3r::GUI
