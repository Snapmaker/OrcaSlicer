#pragma once

#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/textctrl.h>
#include <wx/slider.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/clrpicker.h>
#include <wx/colour.h>

#include "../GUI_Utils.hpp"
#include "../MixedColorMatchHelpers.hpp"
#include "../MixedGradientSelector.hpp"
#include "../MixedFilamentColorMapPanel.hpp"
#include "../MainFrame.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"

namespace Slic3r {
namespace GUI {

class MixedFilamentColorMatchDialog : public DPIDialog
{
public:
    MixedFilamentColorMatchDialog(wxWindow* parent, const std::vector<std::string>& physical_colors, const wxColour& initial_color)
        : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                    wxID_ANY,
                    _L("Add Color"),
                    wxDefaultPosition,
                    wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_physical_colors(physical_colors)
    {
        m_recipe_timer.SetOwner(this);
        m_loading_timer.SetOwner(this);
        m_display_context = build_mixed_filament_display_context(m_physical_colors);

        m_palette.reserve(m_physical_colors.size());
        for (const std::string& hex : m_physical_colors)
            m_palette.emplace_back(parse_mixed_color(hex));

        const wxColour   safe_initial = initial_color.IsOk() ?
                                            initial_color :
                                            (m_palette.size() >= 2 ? blend_pair_filament_mixer(m_palette[0], m_palette[1], 0.5f) :
                                                                     wxColour("#26A69A"));
        std::vector<int> initial_weights(m_palette.size(), 0);
        if (!initial_weights.empty())
            initial_weights[0] = 100;
        if (initial_weights.size() >= 2) {
            initial_weights[0] = 50;
            initial_weights[1] = 50;
        }

        std::vector<unsigned int> filament_ids;
        filament_ids.reserve(m_palette.size());
        for (size_t idx = 0; idx < m_palette.size(); ++idx)
            filament_ids.emplace_back(unsigned(idx + 1));

        SetMinSize(wxSize(FromDIP(430), FromDIP(520)));

        auto* root        = new wxBoxSizer(wxVERTICAL);
        auto* description = new wxStaticText(this, wxID_ANY,
                                             _L("Pick from the current filament gamut. The dialog previews the closest 2-color, 3-color, "
                                                "or 4-color FilamentMixer recipe before it is added."));
        description->Wrap(FromDIP(390));
        root->Add(description, 0, wxEXPAND | wxALL, FromDIP(12));

        m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, m_palette, initial_weights, wxSize(FromDIP(260), FromDIP(260)));
        root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        auto* hex_row = new wxBoxSizer(wxHORIZONTAL);
        hex_row->Add(new wxStaticText(this, wxID_ANY, _L("Hex")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_hex_input = new wxTextCtrl(this, wxID_ANY, normalize_color_match_hex(safe_initial.GetAsString(wxC2S_HTML_SYNTAX)),
                                     wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        m_hex_input->SetToolTip(_L("Enter a hex color like #00FF88. The picker will snap to the closest supported FilamentMixer color."));
        hex_row->Add(m_hex_input, 1, wxALIGN_CENTER_VERTICAL);
        hex_row->AddSpacer(FromDIP(8));
        m_classic_picker = new wxColourPickerCtrl(this, wxID_ANY, safe_initial);
        m_classic_picker->SetToolTip(_L("Classic color picker. The result will snap to the closest supported FilamentMixer color."));
        hex_row->Add(m_classic_picker, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(hex_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* range_row = new wxBoxSizer(wxHORIZONTAL);
        range_row->Add(new wxStaticText(this, wxID_ANY, _L("Range")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_range_slider = new wxSlider(this, wxID_ANY, m_min_component_percent, 0, 50);
        m_range_slider->SetToolTip(_L("Minimum percent for each participating color. Higher values block highly skewed mixes."));
        range_row->Add(m_range_slider, 1, wxALIGN_CENTER_VERTICAL);
        range_row->AddSpacer(FromDIP(8));
        m_range_value = new wxStaticText(this, wxID_ANY, wxEmptyString);
        range_row->Add(m_range_value, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(range_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        auto* summary_grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        summary_grid->AddGrowableCol(1, 1);

        summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Requested")), 0, wxALIGN_CENTER_VERTICAL);
        auto* selected_row = new wxBoxSizer(wxHORIZONTAL);
        m_selected_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
        selected_row->Add(m_selected_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_selected_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        selected_row->Add(m_selected_label, 1, wxALIGN_CENTER_VERTICAL);
        summary_grid->Add(selected_row, 1, wxEXPAND);

        summary_grid->Add(new wxStaticText(this, wxID_ANY, _L("Creates")), 0, wxALIGN_CENTER_VERTICAL);
        auto* recipe_row = new wxBoxSizer(wxHORIZONTAL);
        m_recipe_preview = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(24)), wxBORDER_SIMPLE);
        recipe_row->Add(m_recipe_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_recipe_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_recipe_label->Wrap(FromDIP(280));
        recipe_row->Add(m_recipe_label, 1, wxALIGN_CENTER_VERTICAL);
        summary_grid->Add(recipe_row, 1, wxEXPAND);

        root->Add(summary_grid, 0, wxEXPAND | wxALL, FromDIP(12));

        m_delta_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_delta_label, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        m_presets_label = new wxStaticText(this, wxID_ANY, _L("Exact preset mixes"));
        root->Add(m_presets_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        m_presets_host = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(96)), wxVSCROLL | wxBORDER_SIMPLE);
        m_presets_host->SetScrollRate(FromDIP(6), FromDIP(6));
        m_presets_sizer = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
        m_presets_host->SetSizer(m_presets_sizer);
        root->Add(m_presets_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        m_error_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_error_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
        root->Add(m_error_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        if (wxSizer* button_sizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL))
            root->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

        m_loading_panel = new wxPanel(this, wxID_ANY);
        m_loading_panel->SetMinSize(wxSize(-1, FromDIP(24)));
        auto* loading_row = new wxBoxSizer(wxHORIZONTAL);
        m_loading_label   = new wxStaticText(m_loading_panel, wxID_ANY, " ");
        loading_row->Add(m_loading_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_loading_gauge = new wxGauge(m_loading_panel, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(150), FromDIP(8)),
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_loading_gauge->SetValue(0);
        m_loading_gauge->Enable(false);
        loading_row->Add(m_loading_gauge, 0, wxALIGN_CENTER_VERTICAL);
        m_loading_panel->SetSizer(loading_row);
        root->Add(m_loading_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        SetSizerAndFit(root);

        m_selected_target  = safe_initial;
        m_requested_target = safe_initial;
        if (m_color_map)
            m_color_map->set_min_component_percent(m_min_component_percent);
        update_range_label();
        rebuild_presets_ui();
        sync_inputs_to_requested();
        update_dialog_state();

        if (m_color_map) {
            m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
                if (!m_color_map)
                    return;
                request_recipe_match(m_color_map->selected_color(), true, _L("Matching closest supported mix..."));
            });
        }

        if (m_hex_input) {
            m_hex_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { apply_hex_input(true); });
            m_hex_input->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
                apply_hex_input(false);
                evt.Skip();
            });
        }
        if (m_classic_picker) {
            m_classic_picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent& evt) {
                if (m_syncing_inputs)
                    return;
                apply_requested_target(evt.GetColour());
            });
        }
        if (m_range_slider) {
            m_range_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
                m_min_component_percent = m_range_slider ? std::clamp(m_range_slider->GetValue(), 0, 50) : m_min_component_percent;
                update_range_label();
                if (m_color_map)
                    m_color_map->set_min_component_percent(m_min_component_percent);
                rebuild_presets_ui();
                request_recipe_match(m_requested_target, true, _L("Matching closest supported mix..."));
            });
        }

        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh_selected_recipe(); }, m_recipe_timer.GetId());
        Bind(
            wxEVT_TIMER,
            [this](wxTimerEvent&) {
                if (m_loading_gauge && m_recipe_loading)
                    m_loading_gauge->Pulse();
            },
            m_loading_timer.GetId());
        if (wxWindow* ok_button = FindWindow(wxID_OK)) {
            ok_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
                if (m_recipe_refresh_pending)
                    refresh_selected_recipe();
                if (m_recipe_loading || !m_selected_recipe.valid)
                    return;
                evt.Skip();
            });
        }

        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

        CentreOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    ~MixedFilamentColorMatchDialog() override
    {
        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        if (m_loading_timer.IsRunning())
            m_loading_timer.Stop();
    }

    void begin_initial_recipe_load() { request_recipe_match(m_requested_target, false, _L("Calculating closest supported mix...")); }

    MixedColorMatchRecipeResult selected_recipe() const { return m_selected_recipe; }

    void on_dpi_changed(const wxRect& suggested_rect) override
    {
        wxUnusedVar(suggested_rect);
        Layout();
        Fit();
        Refresh();
    }

private:
    void sync_recipe_preview(MixedColorMatchRecipeResult& recipe, const wxColour* requested_target = nullptr)
    {
        if (!recipe.valid)
            return;

        recipe.preview_color = compute_color_match_recipe_display_color(recipe, m_display_context);
        if (requested_target != nullptr && requested_target->IsOk())
            recipe.delta_e = color_delta_e00(*requested_target, recipe.preview_color);
    }

    void update_range_label()
    {
        if (m_range_value)
            m_range_value->SetLabel(wxString::Format(_L("%d%% min"), m_min_component_percent));
    }

    void rebuild_presets_ui()
    {
        if (!m_presets_host || !m_presets_sizer || !m_presets_label)
            return;

        m_presets = build_color_match_presets(m_physical_colors, m_min_component_percent);
        for (MixedColorMatchRecipeResult& preset : m_presets)
            sync_recipe_preview(preset);

        m_presets_host->Freeze();
        while (m_presets_sizer->GetItemCount() > 0) {
            wxSizerItem* item   = m_presets_sizer->GetItem(size_t(0));
            wxWindow*    window = item ? item->GetWindow() : nullptr;
            m_presets_sizer->Remove(0);
            if (window)
                window->Destroy();
        }

        for (const MixedColorMatchRecipeResult& preset : m_presets) {
            auto*          button  = new wxBitmapButton(m_presets_host, wxID_ANY,
                                                        make_color_match_swatch_bitmap(preset.preview_color, wxSize(FromDIP(30), FromDIP(20))),
                                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            const wxString tooltip = from_u8(summarize_color_match_recipe(preset)) + "\n" +
                                     normalize_color_match_hex(preset.preview_color.GetAsString(wxC2S_HTML_SYNTAX));
            button->SetToolTip(tooltip);
            button->Bind(wxEVT_BUTTON, [this, preset](wxCommandEvent&) { apply_preset(preset); });
            m_presets_sizer->Add(button, 0, wxALL, FromDIP(2));
        }

        m_presets_host->FitInside();
        const bool show_presets = !m_presets.empty();
        m_presets_label->Show(show_presets);
        m_presets_host->Show(show_presets);
        m_presets_host->Thaw();
    }

    void set_recipe_loading(bool loading, const wxString& message)
    {
        m_recipe_loading = loading;
        if (!message.empty())
            m_loading_message = message;

        if (m_loading_label)
            m_loading_label->SetLabel(loading ? m_loading_message : wxString(" "));
        if (m_loading_gauge) {
            if (loading) {
                m_loading_gauge->Enable(true);
                m_loading_gauge->Pulse();
                if (!m_loading_timer.IsRunning())
                    m_loading_timer.Start(100);
            } else {
                if (m_loading_timer.IsRunning())
                    m_loading_timer.Stop();
                m_loading_gauge->SetValue(0);
                m_loading_gauge->Enable(false);
            }
        }
    }

    void sync_inputs_to_requested()
    {
        m_syncing_inputs = true;
        if (m_hex_input)
            m_hex_input->ChangeValue(normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)));
        if (m_classic_picker)
            m_classic_picker->SetColour(m_requested_target);
        m_syncing_inputs = false;
    }

    bool apply_requested_target(const wxColour& requested_target)
    {
        request_recipe_match(requested_target, false, _L("Matching closest supported mix..."));
        return true;
    }

    bool apply_hex_input(bool show_invalid_error)
    {
        if (!m_hex_input || m_syncing_inputs)
            return false;

        wxColour parsed;
        if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed)) {
            if (show_invalid_error && m_error_label)
                m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
            return false;
        }

        return apply_requested_target(parsed);
    }

    void request_recipe_match(const wxColour& requested_target, bool debounce, const wxString& loading_message)
    {
        m_requested_target = requested_target;
        m_selected_target  = requested_target;
        sync_inputs_to_requested();

        ++m_recipe_request_token;
        set_recipe_loading(true, loading_message);

        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        m_recipe_refresh_pending = debounce;
        update_dialog_state();

        if (debounce) {
            m_recipe_timer.StartOnce(120);
            return;
        }

        launch_recipe_match(m_recipe_request_token, requested_target);
    }

    void refresh_selected_recipe()
    {
        m_recipe_refresh_pending = false;
        launch_recipe_match(m_recipe_request_token, m_requested_target);
    }

    void launch_recipe_match(size_t request_token, const wxColour& requested_target)
    {
        const std::vector<std::string> physical_colors       = m_physical_colors;
        const int                      min_component_percent = m_min_component_percent;
        wxWeakRef<wxWindow>            weak_self(this);
        std::thread([weak_self, physical_colors, requested_target, request_token, min_component_percent]() {
            MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(physical_colors, requested_target, min_component_percent);
            wxGetApp().CallAfter([weak_self, requested_target, recipe = std::move(recipe), request_token]() mutable {
                if (!weak_self)
                    return;
                auto* self = static_cast<MixedFilamentColorMatchDialog*>(weak_self.get());
                self->handle_recipe_result(request_token, requested_target, std::move(recipe));
            });
        }).detach();
    }

    void handle_recipe_result(size_t request_token, const wxColour& requested_target, MixedColorMatchRecipeResult recipe)
    {
        if (request_token != m_recipe_request_token)
            return;

        m_has_recipe_result = true;
        m_selected_recipe   = std::move(recipe);
        sync_recipe_preview(m_selected_recipe, &requested_target);
        set_recipe_loading(false, wxEmptyString);

        if (m_selected_recipe.valid) {
            m_selected_target = m_selected_recipe.preview_color;
            if (m_color_map)
                m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
            sync_inputs_to_requested();
        } else {
            m_selected_target = requested_target;
        }

        update_dialog_state();
    }

    void apply_preset(MixedColorMatchRecipeResult preset)
    {
        preset.delta_e = 0.0;
        sync_recipe_preview(preset);
        ++m_recipe_request_token;
        m_requested_target       = preset.preview_color;
        m_selected_target        = preset.preview_color;
        m_selected_recipe        = std::move(preset);
        m_has_recipe_result      = true;
        m_recipe_refresh_pending = false;
        if (m_recipe_timer.IsRunning())
            m_recipe_timer.Stop();
        set_recipe_loading(false, wxEmptyString);
        if (m_color_map)
            m_color_map->set_normalized_weights(expand_color_match_recipe_weights(m_selected_recipe, m_palette.size()), false);
        sync_inputs_to_requested();
        update_dialog_state();
    }

    void update_dialog_state()
    {
        const wxColour fallback = wxColour("#26A69A");
        if (m_selected_preview) {
            m_selected_preview->SetBackgroundColour(m_requested_target.IsOk() ? m_requested_target : fallback);
            m_selected_preview->Refresh();
        }
        if (m_selected_label)
            m_selected_label->SetLabel(m_requested_target.IsOk() ?
                                           normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)) :
                                           normalize_color_match_hex(fallback.GetAsString(wxC2S_HTML_SYNTAX)));

        const bool     valid        = m_selected_recipe.valid;
        const wxColour recipe_color = (valid && m_selected_recipe.preview_color.IsOk()) ?
                                          m_selected_recipe.preview_color :
                                          (m_requested_target.IsOk() ? m_requested_target : fallback);
        if (m_recipe_preview) {
            m_recipe_preview->SetBackgroundColour(recipe_color);
            m_recipe_preview->Refresh();
        }
        if (m_recipe_label) {
            if (m_recipe_loading) {
                m_recipe_label->SetLabel(m_loading_message);
            } else if (valid) {
                const wxString recipe_summary = from_u8(summarize_color_match_recipe(m_selected_recipe));
                const wxString recipe_hex     = normalize_color_match_hex(recipe_color.GetAsString(wxC2S_HTML_SYNTAX));
                m_recipe_label->SetLabel(recipe_summary + "  " + recipe_hex);
            } else if (m_has_recipe_result) {
                m_recipe_label->SetLabel(_L("No supported 2-color, 3-color, or 4-color recipe found."));
            } else {
                m_recipe_label->SetLabel(wxEmptyString);
            }
        }
        if (m_delta_label) {
            if (m_recipe_loading && m_requested_target.IsOk()) {
                m_delta_label->SetLabel(
                    wxString::Format(_L("Matching %s..."), normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX))));
            } else if (valid && m_requested_target.IsOk()) {
                m_delta_label->SetLabel(wxString::Format(_L("Requested %s, closest recipe delta: %.2f"),
                                                         normalize_color_match_hex(m_requested_target.GetAsString(wxC2S_HTML_SYNTAX)),
                                                         m_selected_recipe.delta_e));
            } else {
                m_delta_label->SetLabel(wxEmptyString);
            }
        }
        if (m_error_label) {
            if (m_recipe_loading)
                m_error_label->SetLabel(wxEmptyString);
            else if (!valid && m_has_recipe_result)
                m_error_label->SetLabel(
                    _L("Unable to create a color mix from the current physical filament colors within the selected range."));
            else if (m_hex_input && !m_syncing_inputs) {
                wxColour parsed;
                if (!try_parse_color_match_hex(m_hex_input->GetValue(), parsed))
                    m_error_label->SetLabel(_L("Use a valid hex color like #00FF88."));
                else
                    m_error_label->SetLabel(wxEmptyString);
            } else {
                m_error_label->SetLabel(wxEmptyString);
            }
        }
        if (wxWindow* ok_button = FindWindow(wxID_OK))
            ok_button->Enable(valid && !m_recipe_loading && !m_recipe_refresh_pending);

        Layout();
    }

private:
    std::vector<std::string>                 m_physical_colors;
    MixedFilamentDisplayContext              m_display_context;
    std::vector<wxColour>                    m_palette;
    std::vector<MixedColorMatchRecipeResult> m_presets;
    MixedFilamentColorMapPanel*              m_color_map        = nullptr;
    wxTextCtrl*                              m_hex_input        = nullptr;
    wxColourPickerCtrl*                      m_classic_picker   = nullptr;
    wxSlider*                                m_range_slider     = nullptr;
    wxStaticText*                            m_range_value      = nullptr;
    wxStaticText*                            m_presets_label    = nullptr;
    wxScrolledWindow*                        m_presets_host     = nullptr;
    wxWrapSizer*                             m_presets_sizer    = nullptr;
    wxPanel*                                 m_loading_panel    = nullptr;
    wxStaticText*                            m_loading_label    = nullptr;
    wxGauge*                                 m_loading_gauge    = nullptr;
    wxPanel*                                 m_selected_preview = nullptr;
    wxStaticText*                            m_selected_label   = nullptr;
    wxPanel*                                 m_recipe_preview   = nullptr;
    wxStaticText*                            m_recipe_label     = nullptr;
    wxStaticText*                            m_delta_label      = nullptr;
    wxStaticText*                            m_error_label      = nullptr;
    wxColour                                 m_requested_target{wxColour("#26A69A")};
    wxColour                                 m_selected_target{wxColour("#26A69A")};
    MixedColorMatchRecipeResult              m_selected_recipe;
    wxTimer                                  m_recipe_timer;
    wxTimer                                  m_loading_timer;
    wxString                                 m_loading_message;
    size_t                                   m_recipe_request_token{0};
    int                                      m_min_component_percent{15};
    bool                                     m_has_recipe_result{false};
    bool                                     m_recipe_loading{false};
    bool                                     m_recipe_refresh_pending{false};
    bool                                     m_syncing_inputs{false};
};

MixedColorMatchRecipeResult prompt_best_color_match_recipe(
    wxWindow* parent,
    const std::vector<std::string>& physical_colors,
    const wxColour& initial_color);


} // namespace GUI
} // namespace Slic3r
