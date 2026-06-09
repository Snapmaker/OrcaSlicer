#include "MixedFilamentBatchDialog.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "Widgets/ComboBox.hpp"
#include "Plater.hpp"
#include "BitmapCache.hpp"

#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

MixedFilamentBatchDialog::MixedFilamentBatchDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Batch Match"),
                wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetSize(FromDIP(620), FromDIP(680));
    SetMinSize(wxSize(FromDIP(580), FromDIP(480)));

    if (wxGetApp().preset_bundle) {
        ConfigOptionStrings* co = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (co) m_physical_colors = co->values;
    }
    load_model_colors();
    build_ui();
    update_prompt_text();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(!m_model_colors.empty() && m_physical_colors.size() >= 2);
}

MixedFilamentBatchDialog::~MixedFilamentBatchDialog()
{
    m_cancel_requested->store(true);
    if (m_worker_thread.joinable())
        m_worker_thread.join();
    m_destroyed->store(true);
}

void MixedFilamentBatchDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    SetSize(FromDIP(620), FromDIP(680));
    Layout();
    Refresh();
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::load_model_colors()
{
    m_model_colors.clear();
    auto* pb = wxGetApp().preset_bundle;
    if (!pb) return;

    // Read from filament_colour project config — these are the physical
    // filament colors the user configured. Also try extract_model_colors
    // to pick up any MMU-painted model colors, merging with dedup.
    ConfigOptionStrings* co = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!co || co->values.empty()) return;

    auto add_unique = [this](const wxColour& c, const std::string& hex) {
        for (const auto& e : m_model_colors) {
            if (e.hex_value == hex || color_delta_e00(e.color, c) < 0.5) return;
        }
        m_model_colors.push_back({(unsigned int)(m_model_colors.size() + 1), c, hex});
    };

    for (const auto& hex : co->values) {
        wxColour c;
        if (!try_parse_color_match_hex(hex, c)) continue;
        add_unique(c, hex);
    }

    // Also try to extract from MMU-painted model if available
    if (wxGetApp().plater()) {
        const Print& print = wxGetApp().plater()->fff_print();
        if (!print.objects().empty()) {
            auto mmu_colors = extract_model_colors(print);
            for (const auto& mc : mmu_colors)
                add_unique(mc.color, mc.hex_value);
        }
    }
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    const int M  = FromDIP(8);
    const int M2 = FromDIP(4);

    // Lock the dialog to a fixed minimum size so Fit() in DPIDialog machinery
    // won't collapse it when content is sparse.
    SetMinClientSize(wxSize(FromDIP(560), FromDIP(480)));

    // ---- Error panel (same pattern as MixedFilamentDialog) ----
    {
        m_error_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_error_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FDE8E8")));
        m_error_panel->Hide();
        auto* err_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap error_bmp(m_error_panel, "error_icon_red_exclamation", 14);
        err_sizer->Add(new wxStaticBitmap(m_error_panel, wxID_ANY, error_bmp.bmp()), 0, wxALL, FromDIP(8));
        m_error_text = new Label(m_error_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_error_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D32F2F")));
        err_sizer->Add(m_error_text, 1, wxALL, FromDIP(8));
        m_error_panel->SetSizer(err_sizer);
        top_sizer->Add(m_error_panel, 0, wxEXPAND);
    }
    // ---- Warning panel (same pattern as MixedFilamentDialog) ----
    {
        m_warning_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFF3EB")));
        m_warning_panel->Hide();
        auto* warn_sizer = new wxBoxSizer(wxHORIZONTAL);
        ScalableBitmap warn_bmp(m_warning_panel, "icon_warning_triangle", 14);
        warn_sizer->Add(new wxStaticBitmap(m_warning_panel, wxID_ANY, warn_bmp.bmp()), 0, wxALL, FromDIP(8));
        m_warning_text = new Label(m_warning_panel, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF842D")));
        warn_sizer->Add(m_warning_text, 1, wxALL, FromDIP(8));
        m_warning_panel->SetSizer(warn_sizer);
        top_sizer->Add(m_warning_panel, 0, wxEXPAND);
    }

    // Scroll container
    m_scrolled_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolled_content->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
    m_scrolled_content->SetScrollRate(0, FromDIP(8));
    m_scrolled_content->Bind(wxEVT_CHILD_FOCUS, [](wxChildFocusEvent&) {}); // prevent auto-scroll on click
    auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Card 1: Method + Prompt (white card, same as MixedFilamentDialog) ----
    {
        auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetMinSize(wxSize(FromDIP(340), -1));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));

        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        auto* label = new Label(card, _L("Matching method:"));
        card_sizer->Add(label, 0, wxALL, M);

        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
        std::vector<wxString> names = { _L("Recommended"), _L("Manual") };
        auto seg_bg = StateColor();
        auto seg_fg = StateColor(std::pair(wxColour("#4A4A4A"), (int)StateColor::Normal));
        auto sel_bg = StateColor(std::pair(wxColour("#009688"), (int)StateColor::Normal));
        auto sel_fg = StateColor(std::pair(wxColour("#FEFEFE"), (int)StateColor::Normal));

        for (int i = 0; i < 2; ++i) {
            auto* btn = new Button(card, names[i]);
            btn->SetMinSize(wxSize(FromDIP(120), FromDIP(28)));
            btn->SetPaddingSize(wxSize(FromDIP(6), FromDIP(2)));
            btn->SetCornerRadius(FromDIP(4));
            btn->SetBorderWidth(0);
            btn->SetFont(Label::Body_12);
            if (i == m_method_selected) {
                btn->SetBackgroundColor(sel_bg); btn->SetTextColor(sel_fg);
                btn->SetCanFocus(false);
            } else {
                btn->SetBackgroundColor(seg_bg); btn->SetTextColor(seg_fg);
            }
            btn->Bind(wxEVT_BUTTON, [this, i, seg_bg, seg_fg, sel_bg, sel_fg](wxCommandEvent&) {
                if (i == m_method_selected) return;
                m_method_buttons[m_method_selected]->SetBackgroundColor(seg_bg);
                m_method_buttons[m_method_selected]->SetTextColor(seg_fg);
                m_method_buttons[m_method_selected]->SetCanFocus(true);
                m_method_selected = i;
                m_method_buttons[i]->SetBackgroundColor(sel_bg);
                m_method_buttons[i]->SetTextColor(sel_fg);
                m_method_buttons[i]->SetCanFocus(false);
                on_method_changed(i);
            });
            m_method_buttons.push_back(btn);
            btn_row->Add(btn, 0, wxRIGHT, M2);
        }
        card_sizer->Add(btn_row, 0, wxLEFT | wxBOTTOM, M);

        // Prompt text inside same card (not a separate card)
        m_prompt_label = new Label(card, Label::Body_12, wxEmptyString, LB_AUTO_WRAP);
        card_sizer->Add(m_prompt_label, 0, wxALL, M);

        card->SetSizer(card_sizer);
        scroll_sizer->Add(card, 0, wxEXPAND | wxALL, M);
    }

    // ---- Card 2: Manual filament selection (hidden by default) ----
    {
        auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetMinSize(wxSize(FromDIP(340), -1));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        card->Hide();
        m_manual_card = card;
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        card_sizer->Add(new Label(card, _L("Filament configuration (nozzles 1-4):")), 0, wxALL, M);

        PresetBundle* pb = wxGetApp().preset_bundle;
        const std::vector<std::string>& filament_presets = pb ? pb->filament_presets : std::vector<std::string>();

        for (int i = 0; i < 4; ++i) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            // Checkbox — same label pattern as MFDialog row label
            m_enable_check[i] = new wxCheckBox(card, wxID_ANY,
                wxString::Format(_L("Nozzle %d"), i + 1));
            m_enable_check[i]->SetValue(m_filament_enabled[i]);
            m_enable_check[i]->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            m_enable_check[i]->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            m_enable_check[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { on_manual_selection_changed(); });
            row->Add(m_enable_check[i], 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);

            // Filament label — matches MFDialog::rebuild_filament_rows() label style
            auto* row_lbl = new wxStaticText(card, wxID_ANY,
                wxString::Format(_L("Filament %d"), i + 1),
                wxDefaultPosition, wxSize(FromDIP(50), -1));
            row_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
            row_lbl->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            row->Add(row_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

            // Combo — matches MFDialog::rebuild_filament_rows()
            auto* cb = new ComboBox(card, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(FromDIP(200), FromDIP(30)),
                                    0, nullptr, wxCB_READONLY);
            cb->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

            for (size_t j = 0; j < m_physical_colors.size(); ++j) {
                wxString name;
                if (pb && j < filament_presets.size()) {
                    const Preset* preset = pb->filaments.find_preset(filament_presets[j]);
                    if (preset) name = from_u8(preset->label(false));
                }
                if (name.empty()) name = wxString::Format("F%zu", j + 1);

                wxBitmap* badge_icon = get_extruder_color_icon(
                    m_physical_colors[j], std::to_string(j + 1), FromDIP(20), FromDIP(20));
                cb->Append(name, badge_icon ? badge_icon->ConvertToImage() : wxNullImage);
            }
            if (m_filament_selections[i] >= 0 && m_filament_selections[i] < (int)m_physical_colors.size())
                cb->SetSelection(m_filament_selections[i]);
            else if (!m_physical_colors.empty())
                cb->SetSelection(0);
            cb->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { on_manual_selection_changed(); });
            row->Add(cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
            m_filament_combo[i] = cb;
            card_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M2);
        }
        card->SetSizer(card_sizer);
        scroll_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT, M);
    }

    // ---- Card 3: Mapping legend ----
    {
        auto* card = new StaticBox(m_scrolled_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        card->SetCornerRadius(FromDIP(4));
        card->SetMinSize(wxSize(FromDIP(340), -1));
        card->SetBorderWidth(FromDIP(1));
        card->SetBorderColorNormal(wxColour("#F0F0F0"));
        card->SetBackgroundColor(StateColor(std::pair(wxColour("#FFFFFF"), (int)StateColor::Normal)));
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        card_sizer->Add(new Label(card, _L("Color Mapping:")), 0, wxALL, M);

        m_legend_scroller = new wxScrolledWindow(card, wxID_ANY, wxDefaultPosition,
                                                  wxSize(-1, FromDIP(140)), wxHSCROLL);
        m_legend_scroller->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_legend_sizer = new wxWrapSizer(wxHORIZONTAL);
        m_legend_scroller->SetSizer(m_legend_sizer);
        m_legend_scroller->SetScrollRate(FromDIP(10), 0);
        card_sizer->Add(m_legend_scroller, 0, wxEXPAND | wxALL, M);
        card->SetSizer(card_sizer);
        scroll_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);
    }

    m_scrolled_content->SetSizer(scroll_sizer);
    top_sizer->Add(m_scrolled_content, 1, wxEXPAND);

    // ---- Progress bar ----
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_progress_label = new Label(this, wxEmptyString);
        row->Add(m_progress_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        m_progress_bar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(8)),
                                      wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_progress_bar->SetValue(0);
        row->Add(m_progress_bar, 1, wxALIGN_CENTER_VERTICAL);
        top_sizer->Add(row, 0, wxEXPAND | wxALL, M);
    }

    // ---- Buttons (same Confirm/Cancel pattern as MixedFilamentDialog) ----
    {
        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
        m_btn_start_match = new Button(this, _L("Start Match"));
        m_btn_start_match->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
        m_btn_cancel_match = new Button(this, _L("Cancel"));
        m_btn_cancel_match->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        m_btn_rematch      = new Button(this, _L("Re-match"));
        m_btn_rematch->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
        m_btn_confirm      = new Button(this, _L("OK"));
        m_btn_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);

        btn_row->AddStretchSpacer();
        btn_row->Add(m_btn_start_match, 0, wxRIGHT, M2);
        btn_row->Add(m_btn_cancel_match, 0, wxRIGHT, M2);
        btn_row->Add(m_btn_rematch, 0, wxRIGHT, M2);
        btn_row->Add(m_btn_confirm, 0);

        m_btn_start_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });
        m_btn_cancel_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_batch_match(); });
        m_btn_rematch->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_batch_match(); });
        m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

        top_sizer->Add(btn_row, 0, wxEXPAND | wxALL, M);
    }

    SetSizer(top_sizer);
    Layout();
    SetSize(FromDIP(620), FromDIP(680)); // override Fit() to keep fixed size
}

// ---------------------------------------------------------------------------
// State management (same patterns as MixedFilamentDialog)
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::display_warning(const wxString& msg)
{
    if (!m_warning_panel || !m_warning_text || !m_error_panel) return;
    m_error_panel->Hide();
    m_warning_panel->Show();
    Layout();
    m_warning_text->SetLabel(msg);
    Layout();
}

void MixedFilamentBatchDialog::set_error(const wxString& msg)
{
    if (!m_error_panel || !m_error_text || !m_warning_panel) return;
    m_warning_panel->Hide();
    m_error_panel->Show();
    Layout();
    m_error_text->SetLabel(msg);
    if (m_btn_confirm) m_btn_confirm->Disable();
    Layout();
}

void MixedFilamentBatchDialog::set_match_buttons_state(bool matching)
{
    m_btn_start_match->Enable(!matching && !m_match_completed);
    m_btn_cancel_match->Enable(matching);
    m_btn_rematch->Enable(!matching && m_match_completed);
    m_btn_confirm->Enable(!matching && m_match_completed && m_result.success);
    if (matching) {
        m_progress_bar->SetValue(0);
        m_progress_label->SetLabel(_L("Matching..."));
    }
}

void MixedFilamentBatchDialog::update_prompt_text()
{
    if (m_matching_method == RECOMMENDED)
        m_prompt_label->SetLabel(_L("Filaments 1-4 are primary colors. Filaments 5+ are mixed colors. "
                                    "The system will automatically select filament colors and mix recipes. "
                                    "You can manually adjust the matching plan after clicking OK."));
    else
        m_prompt_label->SetLabel(_L("Filaments 1-4 are primary colors. Filaments 5+ are mixed colors. "
                                    "Please select filaments. The system will perform color matching based on "
                                    "your selection. You can manually adjust the matching plan after clicking OK."));
}

void MixedFilamentBatchDialog::on_method_changed(int method)
{
    m_matching_method = (method == 0) ? RECOMMENDED : MANUAL;
    m_result = BatchMatchResult{};
    m_match_completed = false;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    if (m_manual_card) {
        m_manual_card->Show(m_matching_method == MANUAL);
        if (m_matching_method == MANUAL) {
            m_manual_card->Layout();
            m_manual_card->GetParent()->Layout();
        }
    }
    update_prompt_text();
    update_mapping_legend();
    set_match_buttons_state(false);
    m_btn_start_match->Enable(true);
    m_scrolled_content->FitInside();
    Layout();
}

void MixedFilamentBatchDialog::on_manual_selection_changed()
{
    if (m_match_completed) {
        m_result = BatchMatchResult{};
        m_match_completed = false;
        update_mapping_legend();
    }
    m_btn_start_match->Enable(true);
    m_btn_rematch->Enable(false);
    m_btn_confirm->Enable(false);
}

// ---------------------------------------------------------------------------
// Match
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::start_batch_match()
{
    if (m_match_running) return;
    if (m_model_colors.empty()) {
        set_error(_L("No model colors found. Please load a multi-color model."));
        return;
    }
    m_match_running = true;
    m_error_panel->Hide();
    m_warning_panel->Hide();
    set_match_buttons_state(true);
    launch_background_match();
}

void MixedFilamentBatchDialog::cancel_batch_match()
{
    m_cancel_requested->store(true);
}

void MixedFilamentBatchDialog::launch_background_match()
{
    if (m_worker_thread.joinable()) m_worker_thread.join();
    m_cancel_requested->store(false);

    // Capture manual selections on UI thread before spawning worker
    const auto model_colors = m_model_colors;
    std::vector<std::string> active_colors;
    if (m_matching_method == MANUAL) {
        for (int i = 0; i < 4; ++i) {
            if (!m_filament_enabled[i]) continue;
            int sel = m_filament_combo[i] ? m_filament_combo[i]->GetSelection() : -1;
            if (sel >= 0 && sel < (int)m_physical_colors.size())
                active_colors.push_back(m_physical_colors[sel]);
        }
        if (active_colors.size() < 2)
            active_colors = m_physical_colors;
    }
    const auto manual_colors = std::move(active_colors);
    const auto all_physical  = m_physical_colors;

    // Capture preset library colors on UI thread for recommended mode
    std::vector<std::string> all_preset_colors;
    if (m_matching_method == RECOMMENDED) {
        auto* pb = wxGetApp().preset_bundle;
        if (pb) {
            for (const std::string& alias : pb->filament_presets) {
                const Preset* preset = pb->filaments.find_preset(alias);
                if (!preset) continue;
                auto* opt = preset->config.option<ConfigOptionStrings>("filament_colour");
                if (opt && !opt->values.empty())
                    all_preset_colors.push_back(opt->values[0]);
            }
        }
    }
    const auto preset_colors  = std::move(all_preset_colors);
    const auto matching_method = m_matching_method;

    auto destroyed = m_destroyed;
    auto cancel_token = m_cancel_requested;
    auto progress_bar = m_progress_bar;

    m_worker_thread = std::thread([this, model_colors, manual_colors, all_physical,
                                    preset_colors, matching_method,
                                    destroyed, cancel_token, progress_bar]()
    {
        // Resolve physical colors inside the worker thread
        std::vector<std::string> physical_colors;
        if (matching_method == MANUAL) {
            physical_colors = manual_colors;
        } else {
            // Recommended: find best 4-color combo from preset library
            if (preset_colors.size() >= 4) {
                auto best = recommend_best_filament_combo(model_colors, preset_colors, 15, cancel_token);
                physical_colors = best.empty()
                    ? std::vector<std::string>(preset_colors.begin(), preset_colors.begin() + std::min<size_t>(4, preset_colors.size()))
                    : std::move(best);
            } else {
                physical_colors = all_physical;
            }
        }

        BatchMatchResult result;
        try {
            result = batch_match_model_colors(model_colors, physical_colors, 15, cancel_token,
                [progress_bar, destroyed](int done, int total) {
                    if (progress_bar && !destroyed->load()) {
                        wxGetApp().CallAfter([progress_bar, done, total, destroyed]() {
                            if (destroyed->load()) return;
                            if (total > 0) progress_bar->SetValue(done * 100 / total);
                        });
                    }
                });
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("Error: ") + e.what();
            result.error_code = 3;
        } catch (...) {
            result.success = false;
            result.error_message = "Unknown error during batch match";
            result.error_code = 3;
        }
        wxGetApp().CallAfter([this, destroyed, result = std::move(result)]() mutable {
            if (destroyed->load()) return;
            handle_batch_match_result(result);
        });
    });
}

void MixedFilamentBatchDialog::handle_batch_match_result(const BatchMatchResult& result)
{
    if (m_destroyed->load()) return;
    m_match_running = false;
    m_progress_bar->SetValue(100);
    m_progress_label->SetLabel(wxEmptyString);
    set_match_buttons_state(false);

    if (!result.success) {
        set_error(wxString::FromUTF8(result.error_message));
        m_btn_start_match->Enable(true);
        m_btn_rematch->Enable(true);
        Layout();
        return;
    }

    m_match_completed = false; // reset error state
    m_result = result;
    m_match_completed = true;
    update_mapping_legend();

    BOOST_LOG_TRIVIAL(info) << "Batch match: " << result.mappings.size()
                            << " mappings, avg ΔE=" << result.avg_delta_e;
    Layout();
}

// ---------------------------------------------------------------------------
// Legend — uses MixedFilamentBadge for color blocks
// ---------------------------------------------------------------------------

void MixedFilamentBatchDialog::update_mapping_legend()
{
    while (m_legend_sizer->GetItemCount() > 0) {
        wxSizerItem* item = m_legend_sizer->GetItem(size_t(0));
        if (item && item->GetWindow()) item->GetWindow()->Destroy();
        m_legend_sizer->Remove(0);
    }

    if (m_result.mappings.empty()) {
        auto* label = new Label(m_legend_scroller,
            _L("No color mappings yet. Click \"Start Match\" to begin."));
        m_legend_sizer->Add(label, 0, wxALL, FromDIP(4));
    } else {
        for (const auto& mapping : m_result.mappings) {
            auto* item_panel = new wxPanel(m_legend_scroller, wxID_ANY,
                wxDefaultPosition, wxSize(FromDIP(150), FromDIP(52)), wxBORDER_SIMPLE);
            item_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

            auto* sizer = new wxBoxSizer(wxVERTICAL);

            // Row: source swatch → target badge
            auto* swatch_row = new wxBoxSizer(wxHORIZONTAL);

            // Source color block (small solid bitmap from MixedFilamentBadge cache)
            ColorBlockParams src_params;
            src_params.mode        = ColorBlockParams::Solid;
            src_params.solid_color = mapping.source_color.IsOk() ? mapping.source_color : wxColour(128,128,128);
            src_params.width       = FromDIP(16);
            src_params.height      = FromDIP(16);
            auto* src_bmp = new wxStaticBitmap(item_panel, wxID_ANY,
                *get_color_block_bitmap_cached(src_params));
            swatch_row->Add(src_bmp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));

            swatch_row->Add(new wxStaticText(item_panel, wxID_ANY, "→"), 0,
                            wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));

            // Target color block (solid bitmap with filament ID)
            ColorBlockParams tgt_params;
            tgt_params.mode        = ColorBlockParams::Solid;
            tgt_params.solid_color = mapping.matched_color.IsOk() ? mapping.matched_color : wxColour(128,128,128);
            tgt_params.width       = FromDIP(16);
            tgt_params.height      = FromDIP(16);
            tgt_params.label       = wxString::Format("%u", mapping.target_filament_id);
            auto* tgt_bmp = new wxStaticBitmap(item_panel, wxID_ANY,
                *get_color_block_bitmap_cached(tgt_params));
            swatch_row->Add(tgt_bmp, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));

            sizer->Add(swatch_row, 0, wxALL, FromDIP(4));

            // Delta-E quality line
            wxString de_str = wxString::Format("ΔE %.1f", mapping.delta_e);
            auto* de_label = new wxStaticText(item_panel, wxID_ANY, de_str);
            if (mapping.delta_e <= 1.0)
                de_label->SetForegroundColour(wxColour(0, 150, 0));
            else if (mapping.delta_e <= 3.0)
                de_label->SetForegroundColour(wxColour(200, 150, 0));
            else
                de_label->SetForegroundColour(wxColour(196, 67, 63));
            sizer->Add(de_label, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(2));

            item_panel->SetSizer(sizer);
            m_legend_sizer->Add(item_panel, 0, wxALL, FromDIP(3));
        }
    }
    m_legend_scroller->Layout();
    m_legend_scroller->FitInside();
}

}} // namespace Slic3r::GUI
