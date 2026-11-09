#include "FlowTypeHelper.hpp"

#include "GUI_App.hpp"
#include "FilamentGroupDialog.hpp"
#include "HighFlowCompat.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "format.hpp"

#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetFlowVariant.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <set>

namespace Slic3r { namespace GUI { namespace FlowType {
namespace {

constexpr const char* HF_STANDARD_NOTE = "HF parameter copy for Standard hotends.\nSource preset: ";

const Preset* filament_preset(const std::string& name)
{
    const auto& presets = wxGetApp().preset_bundle->filaments;
    return presets.get_edited_preset().name == name ? &presets.get_edited_preset() : presets.find_preset(name, false);
}

const Preset* filament_preset_for_copy(const std::string& name)
{
    // Maximum-flow calibration temporarily removes the flow cap and cooling
    // limits. New material presets must start from the saved material values.
    if (auto* plater = wxGetApp().plater();
        plater && plater->get_partplate_list().get_current_fff_print().calib_mode() == CalibMode::Calib_Vol_speed_Tower)
        if (const Preset* saved = wxGetApp().preset_bundle->filaments.find_preset(name, false, true))
            return saved;
    return filament_preset(name);
}

std::vector<std::string> declared_modes(const Preset& preset)
{
    const auto* support = preset.config.option<ConfigOptionStrings>("filament_flow_support");
    return support && !support->values.empty() ? support->values : std::vector<std::string>{FLOW_MODE_STANDARD};
}

std::string unique_preset_name(const std::string& base)
{
    const auto& presets = wxGetApp().preset_bundle->filaments;
    std::string name    = base;
    for (unsigned int n = 2;; ++n) {
        bool exists = false;
        for (const auto& preset : presets)
            exists |= boost::iequals(preset.name, name);
        if (!exists)
            return name;
        name = base + " (" + std::to_string(n) + ")";
    }
}

void copy_variant_values(Preset& target, size_t count, size_t destination, size_t source)
{
    // Only these declared options are flow variants. Other material arrays keep
    // their original shape, brand and compatibility metadata.
    for (const auto& key : filament_flow_variant_options()) {
        auto* vector = dynamic_cast<ConfigOptionVectorBase*>(target.config.option(key));
        if (!vector || vector->size() == 0)
            continue;
        std::unique_ptr<ConfigOption> original(vector->clone());
        const size_t                  index = std::min(source, vector->size() - 1);
        vector->resize(count);
        vector->set_at(original.get(), destination, index);
    }
}

void save_user_copy(Preset candidate, const std::string& name)
{
    normalize_filament_variant_metadata(candidate.config, true);
    auto&             bundle   = *wxGetApp().preset_bundle;
    auto&             presets  = bundle.filaments;
    const std::string selected = presets.get_selected_preset_name();
    const Preset      edited   = presets.get_edited_preset();
    try {
        presets.save_current_preset(name, false, false, &candidate, &bundle.printers.get_edited_preset());
    } catch (...) {
        if (presets.find_preset(name, false))
            presets.delete_preset(name);
        presets.select_preset_by_name(selected, true);
        presets.get_edited_preset() = edited;
        throw;
    }
    // Creating a profile does not commit the flow dialog's slot selection, nor
    // discard unsaved edits in another filament's settings page.
    presets.select_preset_by_name(selected, true);
    presets.get_edited_preset() = edited;
}

class FlowDialog : public DPIDialog
{
public:
    using DPIDialog::DPIDialog;

protected:
    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Fit();
    }
};

wxColour difference_background() { return wxGetApp().dark_mode() ? wxColour("#493D25") : wxColour("#FFF2D9"); }

class FlowComparisonDialog : public FlowDialog
{
public:
    FlowComparisonDialog(wxWindow* parent, const DynamicPrintConfig& config, const std::string& preset_name)
        : FlowDialog(parent ? wxGetTopLevelParent(parent) : nullptr,
                     wxID_ANY,
                     _L("Compare Standard and High Flow settings"),
                     wxDefaultPosition,
                     wxDefaultSize,
                     wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        SetFont(Label::Body_13);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* name   = new wxStaticText(this, wxID_ANY, from_u8(preset_name));
        name->Wrap(FromDIP(700));
        layout->Add(name, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* hint = new wxStaticText(this, wxID_ANY,
                                      _L("Highlighted rows differ between Standard and High Flow. Shared settings are unchanged."));
        hint->Wrap(FromDIP(700));
        layout->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        auto* list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
        list->SetScrollRate(0, FromDIP(24));
        auto* rows = new wxFlexGridSizer(0, 4, 0, 0);
        rows->AddGrowableCol(0);
        std::vector<wxStaticText*> cells;
        std::vector<bool>          highlights;
        const auto                 add = [&](const wxString& text, bool changed, int width, bool numeric = false) {
            auto* panel = new wxPanel(list, wxID_ANY);
            auto* cell  = new wxStaticText(panel, wxID_ANY, text, wxDefaultPosition, wxSize(FromDIP(width), -1),
                                          numeric ? wxALIGN_RIGHT : wxALIGN_LEFT);
            cell->Wrap(FromDIP(width));
            auto* padding = new wxBoxSizer(wxVERTICAL);
            padding->Add(cell, 1, wxEXPAND | wxALL, FromDIP(8));
            panel->SetSizer(padding);
            rows->Add(panel, 1, wxEXPAND);
            cells.push_back(cell);
            highlights.push_back(changed);
        };
        add(_L("Parameter"), false, 280);
        add(_L("Standard"), false, 120, true);
        add(_L("High flow (HF)"), false, 120, true);
        add(_L("Comparison"), false, 100);
        const auto*  support      = config.option<ConfigOptionStrings>("filament_flow_support");
        const auto   modes        = support && !support->values.empty() ? support->values : std::vector<std::string>{FLOW_MODE_STANDARD};
        const size_t standard     = flow_variant_index(modes, FLOW_MODE_STANDARD);
        const size_t high         = flow_variant_index(modes, FLOW_MODE_HIGH_FLOW);
        const bool   has_standard = std::find(modes.begin(), modes.end(), FLOW_MODE_STANDARD) != modes.end();
        const bool   has_high     = std::find(modes.begin(), modes.end(), FLOW_MODE_HIGH_FLOW) != modes.end();
        for (const auto& key : filament_flow_variant_options()) {
            const auto* option     = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key));
            const auto* definition = print_config_def.get(key);
            if (!option || option->empty() || !definition)
                continue;
            const auto values = option->vserialize();
            if (values.empty())
                continue;
            const size_t standard_index = std::min(standard, values.size() - 1);
            const size_t high_index     = std::min(high, values.size() - 1);
            const bool   changed        = has_standard && has_high && values[standard_index] != values[high_index];
            const auto   display        = [&](size_t index) {
                if (option->is_nil(index))
                    return _L("Use printer setting");
                wxString text = from_u8(values[index]);
                if (option->type() == coBools)
                    return values[index] == "0" ? _L("Disabled") : _L("Enabled");
                for (size_t i = 0; i < definition->enum_values.size() && i < definition->enum_labels.size(); ++i)
                    if (values[index] == definition->enum_values[i])
                        return _L(definition->enum_labels[i]);
                if (!definition->sidetext.empty() && text.find('%') == wxString::npos)
                    text += " " + _L(definition->sidetext);
                return text;
            };
            const wxString label = key == "fan_min_speed" ? _L("Part cooling fan minimum speed") :
                                   key == "fan_max_speed" ? _L("Part cooling fan maximum speed") :
                                   key == "additional_cooling_fan_speed" ?
                                                            _L("Auxiliary cooling fan speed") :
                                                            _L(definition->full_label.empty() ? definition->label : definition->full_label);
            add(label, changed, 280);
            add(has_standard ? display(standard_index) : _L("Not configured"), changed, 120, true);
            add(has_high ? display(high_index) : _L("Not configured"), changed, 120, true);
            add(changed ? _L("Different") : (has_standard && has_high ? _L("Same") : _L("Not configured")), changed, 100);
        }
        list->SetSizer(rows);
        layout->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        auto* close = new Button(this, _L("Close"));
        close->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        layout->Add(close, 0, wxALIGN_RIGHT | wxALL, FromDIP(16));
        SetSizer(layout);
        SetMinSize(FromDIP(wxSize(760, 480)));
        SetSize(FromDIP(wxSize(800, 640)));
        wxGetApp().UpdateDlgDarkUI(this);
        // Apply difference colors after the generic theme walk. Text also marks
        // differences, so the comparison does not rely on color alone.
        const auto foreground = StateColor::darkModeColorFor(wxColour("#242424"));
        list->SetBackgroundColour(GetBackgroundColour());
        for (size_t i = 0; i < cells.size(); ++i) {
            cells[i]->SetBackgroundColour(highlights[i] ? difference_background() : list->GetBackgroundColour());
            cells[i]->GetParent()->SetBackgroundColour(cells[i]->GetBackgroundColour());
            cells[i]->SetForegroundColour(foreground);
            if (highlights[i])
                cells[i]->SetFont(cells[i]->GetFont().Bold());
        }
        Layout();
        list->FitInside();
        CenterOnParent();
    }

protected:
    void on_dpi_changed(const wxRect&) override { Layout(); }
};

class MaxFlowResultDialog : public FlowDialog
{
public:
    explicit MaxFlowResultDialog(wxWindow* parent) : FlowDialog(parent, wxID_ANY, _L("Calculate flow from test height"))
    {
        SetFont(Label::Body_13);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* hint   = new wxStaticText(this, wxID_ANY,
                                        _L("Enter the height from the base where print quality started to fail. Match the start flow and "
                                             "increase per mm to the test you printed."));
        hint->Wrap(FromDIP(460));
        layout->Add(hint, 0, wxEXPAND | wxALL, FromDIP(16));
        const auto*       app_config = wxGetApp().app_config;
        const std::string previous   = app_config->get("max_flow_test", "filament");
        if (!previous.empty()) {
            auto* source = new wxStaticText(this, wxID_ANY, format_wxstr(_L("Last test settings: %1%"), from_u8(previous)));
            source->Wrap(FromDIP(460));
            layout->Add(source, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }
        auto* grid = new wxFlexGridSizer(0, 2, FromDIP(12), FromDIP(16));
        grid->AddGrowableCol(1);
        const auto add = [this, grid](const wxString& label, const wxString& value, const wxString& unit) {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            auto* input = new TextInput(this, value, unit);
            input->SetMinSize(FromDIP(wxSize(180, 32)));
            grid->Add(input, 1, wxEXPAND);
            input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_result(); });
            return input;
        };
        const auto last_value = [app_config](const char* key, double fallback) {
            double         value  = fallback;
            const wxString saved  = from_u8(app_config->get("max_flow_test", key));
            double         parsed = 0;
            if (saved.ToDouble(&parsed) && std::isfinite(parsed) && parsed > 0)
                value = parsed;
            return wxString::FromDouble(value);
        };
        start  = add(_L("Start volumetric speed"), last_value("start", 5), wxString(L"mm\u00b3/s"));
        step   = add(_L("Flow increase per mm"), last_value("step", 0.5), wxString(L"mm\u00b3/s/mm"));
        height = add(_L("Height where quality starts to fail"), wxEmptyString, "mm");
        margin = add(_L("Flow to subtract"), "5", wxString(L"mm\u00b3/s"));
        layout->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        result = new wxStaticText(this, wxID_ANY, wxEmptyString);
        result->Wrap(FromDIP(460));
        layout->Add(result, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* buttons = new DialogButtons(this, {"Cancel", "Confirm"});
        apply         = buttons->GetCONFIRM();
        apply->SetLabel(_L("Use calculated flow"));
        apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(8));
        SetSizerAndFit(layout);
        wxGetApp().UpdateDlgDarkUI(this);
        update_result();
        Fit();
        CenterOnParent();
        height->GetTextCtrl()->SetFocus();
    }

    double flow = 0;

private:
    TextInput *   start = nullptr, *step = nullptr, *height = nullptr, *margin = nullptr;
    wxStaticText* result = nullptr;
    Button*       apply  = nullptr;

    void update_result()
    {
        if (!result || !apply)
            return;
        double     initial = 0, increase = 0, measured = 0, deduction = 0;
        const auto read = [](TextInput* input, double& value) {
            return input->GetTextCtrl()->GetValue().ToDouble(&value) && std::isfinite(value);
        };
        const bool valid = read(start, initial) && read(step, increase) && read(height, measured) && read(margin, deduction) &&
                           initial > 0 && increase > 0 && measured >= 0 && deduction >= 0;
        flow                   = valid ? initial + measured * increase - deduction : 0;
        const auto* definition = print_config_def.get("filament_max_volumetric_speed");
        const bool  usable     = valid && std::isfinite(flow) && flow > 0 && (!definition || flow <= definition->max);
        apply->Enable(usable);
        result->SetLabel(usable ? format_wxstr(_L("%1% + %2% x %3% - %4% = %5% mm\u00b3/s"), wxString::FromDouble(initial, 2),
                                               wxString::FromDouble(measured, 2), wxString::FromDouble(increase, 2),
                                               wxString::FromDouble(deduction, 2), wxString::FromDouble(flow, 2)) :
                                  _L("Enter valid test values. The flow after subtraction must be greater than zero."));
        result->Wrap(FromDIP(460));
        Layout();
    }
};

class AddHighFlowDialog : public FlowDialog
{
public:
    AddHighFlowDialog(wxWindow* parent, const Preset& source)
        : FlowDialog(parent ? wxGetTopLevelParent(parent) : nullptr, wxID_ANY, _L("Add High Flow filament settings")), candidate(source)
    {
        SetFont(Label::Body_13);
        auto       modes    = declared_modes(source);
        const auto high     = std::find(modes.begin(), modes.end(), FLOW_MODE_HIGH_FLOW);
        const bool existing = high != modes.end();
        if (existing) {
            target_index = size_t(std::distance(modes.begin(), high));
            SetTitle(_L("Save calibrated HF settings"));
        } else {
            const size_t standard = flow_variant_index(modes, FLOW_MODE_STANDARD);
            target_index          = modes.size();
            modes.emplace_back(FLOW_MODE_HIGH_FLOW);
            copy_variant_values(candidate, modes.size(), target_index, standard);
            candidate.config.option<ConfigOptionStrings>("filament_flow_support", true)->values = modes;
        }

        auto* layout = new wxBoxSizer(wxVERTICAL);
        auto* hint =
            new wxStaticText(this, wxID_ANY,
                             from_u8(source.name) + "\n\n" +
                                 (existing ? _L("Enter calibrated HF values. Standard settings and the original preset are preserved.") :
                                             _L("Start with the Standard values and enter calibrated HF values. Standard settings and the "
                                                "original preset are preserved.")));
        hint->Wrap(FromDIP(500));
        layout->Add(hint, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* name_row = new wxBoxSizer(wxHORIZONTAL);
        name_row->Add(new wxStaticText(this, wxID_ANY, _L("Preset name")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        name_input = new TextInput(this, from_u8(unique_preset_name(source.name + (existing ? " - calibrated" : " - HF"))));
        name_input->SetMinSize(FromDIP(wxSize(280, 32)));
        name_row->Add(name_input, 1, wxEXPAND);
        layout->Add(name_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        auto* grid = new wxFlexGridSizer(0, 3, FromDIP(12), FromDIP(16));
        grid->AddGrowableCol(2);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Parameter")));
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Standard")));
        grid->Add(new wxStaticText(this, wxID_ANY, _L("High flow (HF)")));
        const size_t                                        standard_index = flow_variant_index(modes, FLOW_MODE_STANDARD);
        const std::vector<std::pair<std::string, wxString>> fields{{"filament_max_volumetric_speed", _L("Max volumetric speed")},
                                                                   {"filament_flow_ratio", _L("Flow ratio")},
                                                                   {"nozzle_temperature", _L("Nozzle temperature")},
                                                                   {"nozzle_temperature_initial_layer",
                                                                    _L("Initial layer nozzle temperature")}};
        for (const auto& [key, label] : fields) {
            const ConfigOption* option = candidate.config.option(key);
            double              value = 0, baseline = 0;
            if (const auto* floats = dynamic_cast<const ConfigOptionFloats*>(option)) {
                value    = floats->get_at(target_index);
                baseline = floats->get_at(standard_index);
            } else if (const auto* integers = dynamic_cast<const ConfigOptionInts*>(option)) {
                value    = integers->get_at(target_index);
                baseline = integers->get_at(standard_index);
            }
            auto* field_label = new wxStaticText(this, wxID_ANY, label);
            field_label->SetMinSize(field_label->GetTextExtent(label + " (" + _L("Different") + ")"));
            grid->Add(field_label, 0, wxALIGN_CENTER_VERTICAL);
            const wxString unit   = key == "filament_max_volumetric_speed"       ? wxString(L"mm\u00b3/s") :
                                    key.find("temperature") != std::string::npos ? wxString(L"\u00b0C") :
                                                                                   wxString();
            const auto     format = key == "filament_flow_ratio" ? "%.3f" : "%.1f";
            auto* standard_value  = new wxStaticText(this, wxID_ANY, wxString::Format(format, baseline) + (unit.empty() ? "" : " " + unit),
                                                     wxDefaultPosition, wxSize(FromDIP(130), -1), wxALIGN_RIGHT);
            grid->Add(standard_value, 0, wxALIGN_CENTER_VERTICAL);
            auto* input = new TextInput(this, wxString::Format(format, value), unit);
            input->SetMinSize(FromDIP(wxSize(180, 32)));
            grid->Add(input, 1, wxEXPAND);
            inputs.emplace_back(key, input);
            input_borders.push_back(input->borderColor());
            baseline_values.push_back(baseline);
            baseline_labels.push_back(standard_value);
            field_labels.emplace_back(field_label, label);
            input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_differences(); });
        }
        layout->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        auto* calculate = new Button(this, _L("Calculate from test height"));
        calculate->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        calculate->SetMinSize(FromDIP(wxSize(-1, 32)));
        calculate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            MaxFlowResultDialog dialog(this);
            if (dialog.ShowModal() == wxID_OK)
                inputs.front().second->GetTextCtrl()->SetValue(wxString::FromDouble(dialog.flow, 2));
        });
        layout->Add(calculate, 0, wxALIGN_RIGHT | wxTOP | wxRIGHT, FromDIP(16));
        error = new wxStaticText(this, wxID_ANY, wxEmptyString);
        error->SetForegroundColour(wxColour("#D32F2F"));
        layout->Add(error, 0, wxEXPAND | wxALL, FromDIP(16));
        auto* buttons = new DialogButtons(this, {"Cancel", "Confirm"});
        buttons->GetCONFIRM()->SetLabel(_L("Save user preset"));
        buttons->GetCONFIRM()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(8));
        SetSizerAndFit(layout);
        CenterOnParent();
        SetFont(Label::Body_13);
        wxGetApp().UpdateDlgDarkUI(this);
        update_differences();
    }

    Preset      candidate;
    std::string name;

private:
    size_t                                          target_index = 0;
    TextInput*                                      name_input   = nullptr;
    wxStaticText*                                   error        = nullptr;
    std::vector<std::pair<std::string, TextInput*>> inputs;
    std::vector<StateColor>                         input_borders;
    std::vector<double>                             baseline_values;
    std::vector<wxStaticText*>                      baseline_labels;
    std::vector<std::pair<wxStaticText*, wxString>> field_labels;

    void update_differences()
    {
        for (size_t i = 0; i < inputs.size(); ++i) {
            double     value   = 0;
            const bool changed = inputs[i].second->GetTextCtrl()->GetValue().ToDouble(&value) && std::isfinite(value) &&
                                 std::abs(value - baseline_values[i]) > 1e-9;
            const auto background = changed ? difference_background() : GetBackgroundColour();
            field_labels[i].first->SetLabel(field_labels[i].second + (changed ? " (" + _L("Different") + ")" : wxString()));
            field_labels[i].first->SetBackgroundColour(background);
            baseline_labels[i]->SetBackgroundColour(background);
            inputs[i].second->SetBorderColor(changed ? StateColor(wxColour("#FF842D")) : input_borders[i]);
            field_labels[i].first->Refresh();
            baseline_labels[i]->Refresh();
        }
        Layout();
    }

    void accept()
    {
        wxString label = name_input->GetTextCtrl()->GetValue();
        label.Trim(true).Trim(false);
        name           = into_u8(label);
        bool duplicate = false;
        for (const auto& preset : wxGetApp().preset_bundle->filaments)
            duplicate |= boost::iequals(name, preset.name);
        if (name.empty() || duplicate || name.find_first_of("<>[]:/\\|?*\"\r\n") != std::string::npos || name.back() == '.' ||
            name.find(PresetCollection::get_suffix_modified()) != std::string::npos) {
            error->SetLabel(_L("Choose a new preset name without filename symbols or an existing preset name."));
            Layout();
            Fit();
            return;
        }
        std::vector<double> values;
        for (const auto& [key, input] : inputs) {
            double      value      = 0;
            const auto* definition = print_config_def.get(key);
            if (!input->GetTextCtrl()->GetValue().ToDouble(&value) || !std::isfinite(value) ||
                (definition && (value < definition->min || value > definition->max)) ||
                ((key == "filament_flow_ratio" || key == "filament_max_volumetric_speed") && value <= 0) ||
                (key.find("temperature") != std::string::npos && value != std::floor(value))) {
                error->SetLabel(_L("Enter valid positive flow values and whole-number temperatures within the allowed range."));
                Layout();
                Fit();
                return;
            }
            values.push_back(value);
        }
        for (size_t i = 0; i < inputs.size(); ++i) {
            ConfigOption* option = candidate.config.option(inputs[i].first);
            if (auto* floats = dynamic_cast<ConfigOptionFloats*>(option))
                floats->values[target_index] = values[i];
            else if (auto* integers = dynamic_cast<ConfigOptionInts*>(option))
                integers->values[target_index] = int(values[i]);
        }
        EndModal(wxID_OK);
    }
};

struct ReviewedAssignment
{
    DynamicPrintConfig       config;
    std::vector<std::string> filament_names;
    std::set<int>            used_filaments;
    std::vector<int>         compatibility;
};

std::optional<ReviewedAssignment> reviewed_assignment;

ReviewedAssignment current_assignment()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    ReviewedAssignment  state;
    state.filament_names = bundle.filament_presets;
    state.config.apply_only(bundle.full_config(),
                            {"printer_model", "nozzle_diameter", "nozzle_volume_type", "filament_settings_id", "filament_type",
                             "filament_flow_support", "filament_volume_type", "filament_grouping_mode", "filament_flow_ratio",
                             "filament_max_volumetric_speed"},
                            true);
    if (Plater* plater = wxGetApp().plater(); plater != nullptr)
        state.used_filaments = plater->get_partplate_list().get_extruders(true);
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        if (!state.used_filaments.empty() && state.used_filaments.count(int(i) + 1) == 0)
            continue;
        const std::string& name   = bundle.filament_presets[i];
        const Preset*      preset = bundle.filaments.get_edited_preset().name == name ? &bundle.filaments.get_edited_preset() :
                                                                                        bundle.filaments.find_preset(name, false);
        const auto         result = HighFlowCompat::check(preset ? preset->config.opt_string("filament_type", 0u) : std::string(), name);
        state.compatibility.push_back(static_cast<int>(result.level));
    }
    return state;
}

} // namespace

static size_t nozzle_count()
{
    const auto* diameters = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    return diameters != nullptr && !diameters->values.empty() ? diameters->values.size() : 1;
}

static void notify_plater()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    plater->update_project_dirty_from_presets();
    plater->on_config_change(wxGetApp().preset_bundle->full_config());
}

bool printer_supports_high_flow()
{
    const auto* support = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionStrings>("printer_flow_support");
    return support != nullptr && std::find(support->values.begin(), support->values.end(), FLOW_MODE_HIGH_FLOW) != support->values.end();
}

bool any_filament_supports_high_flow()
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    for (const std::string& name : bundle.filament_presets) {
        const Preset* preset = bundle.filaments.find_preset(name, false);
        if (preset == nullptr)
            continue;
        const auto* support = preset->config.option<ConfigOptionStrings>("filament_flow_support");
        if (support != nullptr && std::find(support->values.begin(), support->values.end(), FLOW_MODE_HIGH_FLOW) != support->values.end())
            return true;
    }
    return false;
}

std::vector<std::string> nozzle_volume_types()
{
    std::vector<std::string> types;
    if (const auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")) {
        types.reserve(opt->values.size());
        for (int v : opt->values)
            types.push_back(to_string(FilamentVolumeType(v)));
    }
    types.resize(nozzle_count(), FLOW_MODE_STANDARD);
    const bool supported = printer_supports_high_flow();
    for (std::string& t : types)
        if (!supported || t != FLOW_MODE_HIGH_FLOW)
            t = FLOW_MODE_STANDARD;
    return types;
}

size_t distinct_nozzle_flow_type_count()
{
    std::vector<std::string> types = nozzle_volume_types();
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types.empty() ? 1 : types.size();
}

bool any_nozzle_high_flow()
{
    const std::vector<std::string> types = nozzle_volume_types();
    return std::find(types.begin(), types.end(), FLOW_MODE_HIGH_FLOW) != types.end();
}

// App-level memory of the per-nozzle flow selection, stored a
// "nozzle_volume_types" section keyed by printer preset name, CSV of
// "Standard"/"High Flow" per nozzle. The project config itself is volatile
// (reset on restart), hence this mirror.
static const char* APP_CONFIG_NOZZLE_FLOW_SECTION = "nozzle_volume_types";
static const char* FLOW_DISPLAY_HIGH_FLOW         = "High Flow";
static const char* FLOW_DISPLAY_STANDARD          = "Standard";

static void save_nozzle_volume_types_to_app_config()
{
    if (wxGetApp().app_config == nullptr)
        return;
    std::vector<std::string> display;
    for (const std::string& t : nozzle_volume_types())
        display.push_back(t == FLOW_MODE_HIGH_FLOW ? FLOW_DISPLAY_HIGH_FLOW : FLOW_DISPLAY_STANDARD);
    wxGetApp().app_config->set(APP_CONFIG_NOZZLE_FLOW_SECTION, wxGetApp().preset_bundle->printers.get_selected_preset_name(),
                               boost::algorithm::join(display, ","));
}

void set_nozzle_volume_type(size_t nozzle_idx, const std::string& volume_type)
{
    std::vector<std::string> types = nozzle_volume_types();
    if (nozzle_idx >= types.size())
        return;
    types[nozzle_idx] = volume_type == FLOW_MODE_HIGH_FLOW ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD;
    auto* opt         = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.clear();
    for (const std::string& t : types)
        opt->values.push_back(filament_volume_type_from_string(t));
    save_nozzle_volume_types_to_app_config();
    if (Plater* plater = wxGetApp().plater())
        plater->get_partplate_list().set_filament_group_dirty(true);
    notify_plater();
}

void set_nozzle_volume_types(const std::vector<std::string>& volume_types)
{
    const size_t count = nozzle_count();
    if (volume_types.size() != count)
        return;

    auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.clear();
    opt->values.reserve(volume_types.size());
    for (const std::string& type : volume_types)
        opt->values.push_back(type == FLOW_MODE_HIGH_FLOW ? fvtHighFlow : fvtStandard);
    save_nozzle_volume_types_to_app_config();
    notify_plater();
}

void restore_nozzle_volume_types_from_app_config()
{
    if (wxGetApp().app_config == nullptr)
        return;
    const std::string stored = wxGetApp().app_config->get(APP_CONFIG_NOZZLE_FLOW_SECTION,
                                                          wxGetApp().preset_bundle->printers.get_selected_preset_name());
    if (stored.empty())
        return; // no memory for this printer yet, keep whatever the project config holds

    std::vector<std::string> tokens;
    boost::split(tokens, stored, boost::is_any_of(","));
    // Normalize like nozzle_volume_types(): unknown values / unsupported printers -> standard.
    const bool               supported = printer_supports_high_flow();
    std::vector<std::string> types;
    for (std::string& t : tokens) {
        boost::algorithm::trim(t);
        types.push_back(supported && boost::iequals(t, FLOW_DISPLAY_HIGH_FLOW) ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD);
    }
    types.resize(nozzle_count(), FLOW_MODE_STANDARD);

    // Write directly without notify_plater(); callers rebuild the nozzle UI right after.
    auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.clear();
    opt->values.reserve(types.size());
    for (const std::string& t : types)
        opt->values.push_back(t == FLOW_MODE_HIGH_FLOW ? fvtHighFlow : fvtStandard);
}

void reset_nozzle_volume_types_to_standard()
{
    auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
    opt->values.assign(nozzle_count(), fvtStandard);
    notify_plater();
}

std::string grouping_mode()
{
    const auto* opt = wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("filament_grouping_mode");
    return opt != nullptr && opt->value == FILAMENT_GROUPING_CUSTOM ? FILAMENT_GROUPING_CUSTOM : FILAMENT_GROUPING_STANDARD;
}

void set_grouping_mode(const std::string& mode)
{
    wxGetApp().preset_bundle->project_config.option<ConfigOptionString>("filament_grouping_mode", true)->value =
        mode == FILAMENT_GROUPING_CUSTOM ? FILAMENT_GROUPING_CUSTOM : FILAMENT_GROUPING_STANDARD;
    notify_plater();
}

void apply_custom_mapping(const std::vector<FilamentVolumeType>& mapping)
{
    wxGetApp().preset_bundle->set_filament_volume_types(mapping);
    notify_plater();
}

void sync_filament_volume_types_for_slice()
{
    // Flow type every filament should use when the custom per-filament mapping does
    // not apply: follow the single nozzle type when the nozzles are not mixing types
    // (all standard -> standard, all high flow -> high flow); in standard mode with
    // mixed nozzles, fall back to standard.
    FilamentVolumeType type = fvtStandard;
    if (distinct_nozzle_flow_type_count() < 2) {
        const std::vector<std::string> nozzles = nozzle_volume_types();
        if (!nozzles.empty() && nozzles.front() == FLOW_MODE_HIGH_FLOW)
            type = fvtHighFlow;
    }
    const std::vector<FilamentVolumeType> current = wxGetApp().preset_bundle->get_filament_volume_types();
    if (std::all_of(current.begin(), current.end(), [type](FilamentVolumeType t) { return t == type; }))
        return; // already uniform at the target type
    const size_t n = wxGetApp().preset_bundle->filament_presets.size();
    apply_custom_mapping(std::vector<FilamentVolumeType>(std::max<size_t>(n, size_t(1)), type));
}

void remember_assignment() { reviewed_assignment = current_assignment(); }

std::string high_flow_copy_source(const std::string& preset_name)
{
    const Preset* preset = filament_preset(preset_name);
    const auto*   notes  = preset ? preset->config.option<ConfigOptionStrings>("filament_notes") : nullptr;
    if (!notes || notes->values.empty() || notes->values.front().rfind(HF_STANDARD_NOTE, 0) != 0)
        return {};
    const std::string source = notes->values.front().substr(std::strlen(HF_STANDARD_NOTE));
    return source.substr(0, source.find('\n'));
}

void show_filament_flow_comparison(wxWindow* parent, const DynamicPrintConfig& config, const std::string& preset_name)
{
    FlowComparisonDialog dialog(parent, config, preset_name);
    dialog.ShowModal();
}

std::string create_high_flow_preset(wxWindow* parent, const std::string& source_name)
{
    const Preset* source = filament_preset_for_copy(source_name);
    if (!source)
        return {};
    AddHighFlowDialog dialog(parent, *source);
    if (dialog.ShowModal() != wxID_OK)
        return {};
    try {
        save_user_copy(dialog.candidate, dialog.name);
        return dialog.name;
    } catch (const std::exception& error) {
        wxMessageBox(from_u8(error.what()), _L("Could not save filament preset"), wxOK | wxICON_ERROR, parent);
        return {};
    }
}

std::string copy_high_flow_for_standard(wxWindow* parent, const std::string& source_name)
{
    const Preset* source = filament_preset_for_copy(source_name);
    if (!source)
        return {};
    const auto modes = declared_modes(*source);
    const auto high  = std::find(modes.begin(), modes.end(), FLOW_MODE_HIGH_FLOW);
    if (high == modes.end())
        return {};
    Preset candidate = *source;
    copy_variant_values(candidate, 1, 0, size_t(std::distance(modes.begin(), high)));
    candidate.config.option<ConfigOptionStrings>("filament_flow_support", true)->values = {FLOW_MODE_STANDARD};
    auto* notes = candidate.config.option<ConfigOptionStrings>("filament_notes", true);
    if (notes->values.empty())
        notes->values.emplace_back();
    notes->values.front()  = std::string(HF_STANDARD_NOTE) + source_name + "\n\n" + notes->values.front();
    const std::string name = unique_preset_name(source_name + into_u8(_L(" - HF on Standard")));
    try {
        save_user_copy(candidate, name);
        return name;
    } catch (const std::exception& error) {
        wxMessageBox(from_u8(error.what()), _L("Could not save filament preset"), wxOK | wxICON_ERROR, parent);
        return {};
    }
}

MissingHighFlowChoice resolve_missing_high_flow(wxWindow* parent, const std::string& preset_name)
{
    FlowDialog dialog(parent ? wxGetTopLevelParent(parent) : nullptr, wxID_ANY, _L("HF settings on Standard hotends"));
    dialog.SetFont(Label::Body_13);
    auto* layout  = new wxBoxSizer(wxVERTICAL);
    auto* message = new wxStaticText(&dialog, wxID_ANY,
                                     from_u8(preset_name) + "\n\n" +
                                         _L("All physical hotends are set to Standard. Choose how to use this filament's HF settings.") +
                                         "\n\n" +
                                         _L("Using HF values anyway saves a separate Standard preset with the HF parameters.\nExcessive "
                                            "flow may cause under-extrusion."));
    message->Wrap(dialog.FromDIP(500));
    layout->Add(message, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
    auto add = [&](const wxString& label, int result, ButtonStyle style) {
        auto* button = new Button(&dialog, label);
        button->SetStyle(style, ButtonType::Expanded);
        button->SetMinSize(dialog.FromDIP(wxSize(500, 36)));
        button->Bind(wxEVT_BUTTON, [&dialog, result](wxCommandEvent&) { dialog.EndModal(result); });
        layout->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        return button;
    };
    add(_L("Change hotend / review physical setup"), wxID_MORE, ButtonStyle::Regular);
    Button* standard = add(_L("Use Standard settings"), wxID_NO, ButtonStyle::Confirm);
    add(_L("Use HF values anyway"), wxID_YES, ButtonStyle::Alert);
    add(_L("Cancel"), wxID_CANCEL, ButtonStyle::Regular);
    dialog.SetSizerAndFit(layout);
    dialog.CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    standard->SetFocus();
    switch (dialog.ShowModal()) {
    case wxID_MORE: return MissingHighFlowChoice::ReviewHardware;
    case wxID_NO: return MissingHighFlowChoice::UseStandard;
    case wxID_YES: return MissingHighFlowChoice::CopyHighFlow;
    default: return MissingHighFlowChoice::Cancel;
    }
}

void check_selected_filament(size_t filament_idx, const std::string& expected_name, const std::string& previous_name)
{
    auto& bundle = *wxGetApp().preset_bundle;
    if (filament_idx >= bundle.filament_presets.size() || bundle.filament_presets[filament_idx] != expected_name ||
        expected_name == previous_name || !printer_supports_high_flow() || any_nozzle_high_flow())
        return;
    const Preset* preset = filament_preset(expected_name);
    if (!preset)
        return;
    const auto modes = declared_modes(*preset);
    if (std::find(modes.begin(), modes.end(), FLOW_MODE_HIGH_FLOW) == modes.end())
        return;
    Plater*     plater = wxGetApp().plater();
    const auto  result = resolve_missing_high_flow(plater, expected_name);
    std::string name   = expected_name;
    if (result == MissingHighFlowChoice::Cancel) {
        if (filament_preset(previous_name))
            name = previous_name;
    } else if (result == MissingHighFlowChoice::CopyHighFlow) {
        const auto copy = copy_high_flow_for_standard(plater, expected_name);
        if (!copy.empty())
            name = copy;
    }
    bundle.set_filament_preset(filament_idx, name);
    auto mapping = bundle.get_filament_volume_types();
    mapping.resize(bundle.filament_presets.size(), fvtStandard);
    mapping[filament_idx] = fvtStandard;
    bundle.set_filament_volume_types(mapping);
    bundle.export_selections(*wxGetApp().app_config);
    if (plater) {
        plater->sidebar().update_all_preset_comboboxes(false);
        if (result == MissingHighFlowChoice::ReviewHardware)
            plater->sidebar().show_physical_nozzle_settings();
    }
    notify_plater();
}

bool prepare_for_slice(wxWindow* parent)
{
    if (!any_nozzle_high_flow()) {
        sync_filament_volume_types_for_slice();
        return true;
    }
    const ReviewedAssignment state = current_assignment();
    if (reviewed_assignment && reviewed_assignment->config == state.config && reviewed_assignment->filament_names == state.filament_names &&
        reviewed_assignment->used_filaments == state.used_filaments && reviewed_assignment->compatibility == state.compatibility)
        return true;
    FilamentGroupDialog dialog(parent, distinct_nozzle_flow_type_count() < 2);
    return dialog.ShowModal() == wxID_OK;
}

}}} // namespace Slic3r::GUI::FlowType
