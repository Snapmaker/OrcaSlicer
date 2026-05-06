#include "MixedFilamentDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include "MixedColorMatchPanel.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "PresetBundle.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"

#include <wx/dcbuffer.h>
#include <wx/statline.h>
#include <wx/sizer.h>
#include <wx/bmpbuttn.h>

#include <algorithm>
#include <utility>
#include <set>

namespace Slic3r { namespace GUI {

static constexpr int SWATCH_SIZE  = 16;
static constexpr int PREVIEW_SIZE = 80;
static constexpr int STRIP_HEIGHT = 24;

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours)
    : DPIDialog(parent, wxID_ANY, _L("Add Color Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_filament_colours(filament_colours)
{
    m_result.component_a   = 1;
    m_result.component_b   = 2;
    m_result.mix_b_percent = 50;
    build_ui();
}

MixedFilamentDialog::MixedFilamentDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours,
                                     const Slic3r::MixedFilament& existing)
    : DPIDialog(parent, wxID_ANY, _L("Edit Color Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_filament_colours(filament_colours)
    , m_result(existing)
{
    if (existing.gradient_enabled)
        m_current_mode = MODE_GRADIENT;
    else if (!MixedFilamentManager::normalize_manual_pattern(existing.manual_pattern).empty())
        m_current_mode = MODE_CYCLE;
    else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::Simple))
        m_current_mode = MODE_MATCH;
    else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::LayerCycle) &&
             !existing.gradient_component_weights.empty())
        m_current_mode = MODE_RATIO;
    else if (!existing.gradient_component_ids.empty() &&
             existing.distribution_mode == int(MixedFilament::LayerCycle))
        m_current_mode = MODE_GRADIENT;
    else
        m_current_mode = MODE_RATIO;

    // Determine gradient direction from existing configuration
    // Direction 0: A→B (component_a starts dominant, transitions to component_b)
    //              Implemented as: gradient_start > gradient_end (e.g., 0.8 → 0.2)
    // Direction 1: B→A (component_b starts dominant, transitions to component_a)
    //              Implemented as: gradient_start < gradient_end (e.g., 0.2 → 0.8)
    m_gradient_direction = (existing.gradient_start >= existing.gradient_end) ? 0 : 1;

    // Restore tri-picker weights from slash-separated weight string
    if (m_current_mode == MODE_RATIO && !existing.gradient_component_weights.empty()) {
        std::vector<int> vals;
        const char *p = existing.gradient_component_weights.c_str();
        while (*p) {
            char *end;
            int v = (int)std::strtol(p, &end, 10);
            if (end == p) break;
            vals.push_back(v);
            p = end;
            if (*p == '/') ++p;
        }
        if (vals.size() >= 3) {
            int total = 0;
            for (int v : vals) total += v;
            if (total > 0) {
                m_tri_wx = vals[0] / (double)total;
                m_tri_wy = vals[1] / (double)total;
                m_tri_wz = vals[2] / (double)total;
                // Clamp each weight to be at least 10% and at most 90%
                constexpr double MIN_WEIGHT = 0.10;
                constexpr double MAX_WEIGHT = 0.90;
                m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
                m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
                // Renormalize after clamping
                double sum = m_tri_wx + m_tri_wy + m_tri_wz;
                if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
            }
        }
    }

    build_ui();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

wxBitmap MixedFilamentDialog::make_color_bitmap(const wxColour& c, int size)
{
    wxBitmap bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    return bmp;
}

int MixedFilamentDialog::max_filaments_for_mode(int mode) const
{
    if (mode == MODE_RATIO)    return 3;
    if (mode == MODE_GRADIENT) return 2;
    return 4;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void MixedFilamentDialog::build_ui()
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));

    auto* top_sizer = new wxBoxSizer(wxVERTICAL);
    const int M = FromDIP(8);

    // Mode radio
    m_mode_radio = new RadioGroup(this,
        { _L("Ratio"), _L("Cycle"), _L("Match"), _L("Gradient") },
        wxHORIZONTAL);
    m_mode_radio->SetSelection(m_current_mode);
    top_sizer->Add(m_mode_radio, 0, wxEXPAND | wxALL, M);
    top_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    // Bind button events (buttons created later)
    m_btn_remove_filament = new ScalableButton(this, wxID_ANY, "delete_filament");
    m_btn_add_filament = new ScalableButton(this, wxID_ANY, "add_filament");
    m_btn_remove_filament->SetToolTip(_L("Remove last filament"));
    m_btn_add_filament->SetToolTip(_L("Add one filament"));
    m_btn_remove_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        sync_rows_to_result();
        int new_count = std::max(2, (int)m_filament_rows.size() - 1);
        if (new_count == 2) {
            m_tri_wx = 1.0/3.0; m_tri_wy = 1.0/3.0; m_tri_wz = 1.0/3.0;
        }
        resize_gradient_ids(new_count);
        wxTheApp->CallAfter([this]() {
            rebuild_filament_rows();
            Layout(); Fit();
        });
    });
    m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        sync_rows_to_result();
        resize_gradient_ids((int)m_filament_rows.size() + 1);
        wxTheApp->CallAfter([this]() {
            rebuild_filament_rows();
            Layout(); Fit();
        });
    });

    // Main horizontal layout: left side (title + filament rows), right side (preview)
    auto* main_h_sizer = new wxBoxSizer(wxHORIZONTAL);

    // Left column: title row + filament rows panel
    auto* left_col = new wxBoxSizer(wxVERTICAL);
    
    // Create swap button for gradient direction (shown only in gradient mode)
    m_btn_swap_gradient_dir = new wxButton(this, wxID_ANY, from_u8("\xe2\x86\x95"),
                                           wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
    m_btn_swap_gradient_dir->SetToolTip(_L("Swap gradient direction"));
    m_btn_swap_gradient_dir->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_gradient_direction = 1 - m_gradient_direction;
        if (m_preview_panel)
            m_preview_panel->Refresh();
    });
    
    // Title row for left column: title + swap button + add/remove buttons
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    m_filament_title = new wxStaticText(this, wxID_ANY, _L("Mixed Filament"));
    title_row->Add(m_filament_title, 0, wxALIGN_CENTER_VERTICAL);
    title_row->Add(m_btn_swap_gradient_dir, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    title_row->AddStretchSpacer();
    title_row->Add(m_btn_remove_filament, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    title_row->Add(m_btn_add_filament, 0, wxALIGN_CENTER_VERTICAL);
    left_col->Add(title_row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    // Filament rows panel

    m_filament_rows_panel = new wxPanel(this);
    m_filament_rows_sizer = new wxBoxSizer(wxVERTICAL);
    m_filament_rows_panel->SetSizer(m_filament_rows_sizer);
    left_col->Add(m_filament_rows_panel, 1, wxEXPAND);
    
    main_h_sizer->Add(left_col, 1, wxEXPAND | wxRIGHT, M);

    // Right column: preview panel
    auto* preview_col = new wxBoxSizer(wxVERTICAL);
    m_preview_label = new wxStaticText(this, wxID_ANY, _L("Mix Color Effect"));
    preview_col->Add(m_preview_label, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(4));
    m_preview_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                  wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_preview_panel);
        if (m_current_mode == MODE_GRADIENT && m_filament_rows.size() >= 2) {
            wxSize sz = m_preview_panel->GetClientSize();
            int ia = std::max(0, std::min(get_filament_index(0), (int)m_filament_colours.size()-1));
            int ib = std::max(0, std::min(get_filament_index(1), (int)m_filament_colours.size()-1));
            wxColour ca = parse_mixed_color(m_filament_colours[ia]);
            wxColour cb = parse_mixed_color(m_filament_colours[ib]);
            if (m_gradient_direction != 0)
                std::swap(ca, cb);
            wxImage img(sz.GetWidth(), sz.GetHeight());
            unsigned char* data = img.GetData();
            // Use vertical gradient to represent Z-axis direction
            // Top of preview = bottom layers, bottom of preview = top layers
            for (int y = 0; y < sz.GetHeight(); ++y) {
                // Calculate gradient parameter t from 0 (top) to 1 (bottom)
                float t = (sz.GetHeight() > 1) ? float(y) / float(sz.GetHeight() - 1) : 0.5f;
                wxColour c = blend_pair_filament_mixer(ca, cb, t);
                for (int x = 0; x < sz.GetWidth(); ++x) {
                    int idx = (y * sz.GetWidth() + x) * 3;
                    data[idx]   = c.Red();
                    data[idx+1] = c.Green();
                    data[idx+2] = c.Blue();
                }
            }
            dc.DrawBitmap(wxBitmap(img), 0, 0, false);
        } else {
            dc.SetBackground(wxBrush(parse_mixed_color(compute_preview_color())));
            dc.Clear();
        }
    });
    preview_col->Add(m_preview_panel, 0, wxALIGN_CENTER);
    
    main_h_sizer->Add(preview_col, 0, wxALIGN_TOP);

    top_sizer->Add(main_h_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, M);
    m_line_below_mid = new wxStaticLine(this);
    top_sizer->Add(m_line_below_mid, 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    // Gradient selector (2-filament ratio mode)
    int initial_val = m_result.mix_b_percent;
    if (m_current_mode == MODE_RATIO && m_result.gradient_component_ids.empty()) {
        int total = m_result.ratio_a + m_result.ratio_b;
        if (total > 0) initial_val = m_result.ratio_b * 100 / total;
    }
    wxColour col_a = (!m_filament_colours.empty())
        ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_a - 1)])
        : wxColour(128, 128, 128);
    wxColour col_b = (m_filament_colours.size() > 1)
        ? parse_mixed_color(m_filament_colours[std::max(0, (int)m_result.component_b - 1)])
        : col_a;
    m_gradient_selector = new MixedGradientSelector(this, col_a, col_b, initial_val);
    m_ratio_label_a = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", 100 - initial_val));
    m_ratio_label_b = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", initial_val));
    m_ratio_label_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_ratio_label_sizer->Add(m_ratio_label_a, 0);
    m_ratio_label_sizer->AddStretchSpacer();
    m_ratio_label_sizer->Add(m_ratio_label_b, 0);

    // Strip preview panel — shown in 2-color ratio mode
    m_strip_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                wxSize(-1, FromDIP(STRIP_HEIGHT)));
    m_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
    m_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_strip_panel);
        draw_strip(dc, m_strip_panel);
    });

    const int LABEL_W = FromDIP(60);
    m_gradient_bar_sizer = new wxBoxSizer(wxVERTICAL);
    {
        auto* ratio_row = new wxBoxSizer(wxHORIZONTAL);
        auto* ratio_lbl = new wxStaticText(this, wxID_ANY, _L("Mix Ratio"),
                                           wxDefaultPosition, wxSize(LABEL_W, -1));
        auto* ratio_right = new wxBoxSizer(wxVERTICAL);
        ratio_right->Add(m_ratio_label_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
        ratio_right->Add(m_gradient_selector, 0, wxEXPAND);
        ratio_row->Add(ratio_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        ratio_row->Add(ratio_right, 1, wxEXPAND);
        m_gradient_bar_sizer->Add(ratio_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, M);
    }
    {
        // Preview strip row — 2-color only; strip panel lives here
        auto* preview_row = new wxBoxSizer(wxHORIZONTAL);
        auto* preview_lbl = new wxStaticText(this, wxID_ANY, _L("Preview"),
                                             wxDefaultPosition, wxSize(LABEL_W, -1));
        preview_row->Add(preview_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        preview_row->Add(m_strip_panel, 1, wxEXPAND);
        m_gradient_bar_sizer->Add(preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, M);
    }
    top_sizer->Add(m_gradient_bar_sizer, 0, wxEXPAND);

    // Triangle picker (3-filament ratio mode) — triangle left, preview strip right
    build_tri_picker();
    {
        m_tri_strip_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_tri_strip_panel->SetMinSize(wxSize(FromDIP(160), -1));
        m_tri_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_tri_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_tri_strip_panel);
            draw_strip(dc, m_tri_strip_panel);
        });

        m_tri_picker_sizer = new wxBoxSizer(wxVERTICAL);
        auto* tri_cols = new wxBoxSizer(wxHORIZONTAL);

        auto* tri_left = new wxBoxSizer(wxVERTICAL);
        tri_left->Add(new wxStaticText(this, wxID_ANY, _L("Mix Ratio")),
                      0, wxALIGN_CENTER | wxBOTTOM, FromDIP(2));
        tri_left->Add(m_tri_picker, 0, wxALIGN_CENTER);

        auto* tri_right = new wxBoxSizer(wxVERTICAL);
        tri_right->Add(new wxStaticText(this, wxID_ANY, _L("Preview")),
                       0, wxALIGN_CENTER | wxBOTTOM, FromDIP(2));
        tri_right->Add(m_tri_strip_panel, 1, wxEXPAND);

        tri_cols->Add(tri_left, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, M * 2);
        tri_cols->Add(tri_right, 1, wxEXPAND);
        m_tri_picker_sizer->Add(tri_cols, 0, wxEXPAND | wxALL, M);
    }
    top_sizer->Add(m_tri_picker_sizer, 0, wxEXPAND);

    // Match mode panel
    {
        m_match_panel_sizer = new wxBoxSizer(wxVERTICAL);
        wxColour initial = (m_current_mode == MODE_MATCH && !m_result.display_color.empty())
            ? parse_mixed_color(m_result.display_color)
            : wxColour("#26A69A");
        m_match_panel = new MixedColorMatchPanel(this, m_filament_colours, initial);
        m_match_panel_sizer->Add(m_match_panel, 0, wxEXPAND | wxALL, M);
        top_sizer->Add(m_match_panel_sizer, 0, wxEXPAND);
    }

    // Pattern editor (cycle mode) — strip panel is added here for cycle/gradient modes
    {
        const std::string init_pattern = MixedFilamentManager::normalize_manual_pattern(
            m_current_mode == MODE_CYCLE ? m_result.manual_pattern : std::string());
        m_pattern_panel_sizer = new wxBoxSizer(wxVERTICAL);

        auto* pattern_row = new wxBoxSizer(wxHORIZONTAL);
        pattern_row->Add(new wxStaticText(this, wxID_ANY, _L("Pattern")), 0,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT, M);
        m_pattern_ctrl = new wxTextCtrl(this, wxID_ANY,
                                        from_u8(init_pattern.empty() ? "12" : init_pattern),
                                        wxDefaultPosition, wxSize(FromDIP(200), -1),
                                        wxTE_PROCESS_ENTER);
        m_pattern_ctrl->SetToolTip(_L("Repeating layer pattern. Use 1/2 or A/B for the two filaments, "
                                      "3..9 for direct physical filament IDs. "
                                      "Comma-separated groups set per-perimeter patterns, e.g. 12,21. "
                                      "Examples: 1/1/2/2, 12,21, 1/2/3."));
        pattern_row->Add(m_pattern_ctrl, 1, wxALIGN_CENTER_VERTICAL);
        m_pattern_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (m_cycle_strip_panel) m_cycle_strip_panel->Refresh();
            if (m_preview_panel) {
                m_preview_panel->Refresh();
                m_preview_panel->Update();
            }
        });
        m_pattern_panel_sizer->Add(pattern_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, M);

        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);
        btn_row->Add(new wxStaticText(this, wxID_ANY, _L("Filaments")), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        for (int fid = 0; fid < (int)m_filament_colours.size(); ++fid) {
            auto* btn = new wxButton(this, wxID_ANY, wxString::Format("%d", fid + 1),
                                     wxDefaultPosition, wxSize(FromDIP(24), FromDIP(22)),
                                     wxBU_EXACTFIT);
            btn->SetBackgroundColour(parse_mixed_color(m_filament_colours[fid]));
            btn->SetToolTip(wxString::Format(_L("Append filament %d to pattern"), fid + 1));
            btn->Bind(wxEVT_BUTTON, [this, fid](wxCommandEvent&) {
                if (m_pattern_ctrl)
                    m_pattern_ctrl->AppendText(wxString::Format("%d", fid + 1));
            });
            btn_row->Add(btn, 0, wxRIGHT, FromDIP(4));
            m_pattern_quick_buttons.push_back(btn);
        }
        m_pattern_panel_sizer->Add(btn_row, 0, wxLEFT | wxRIGHT | wxTOP, M);

        m_cycle_strip_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                          wxSize(-1, FromDIP(STRIP_HEIGHT)));
        m_cycle_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
        m_cycle_strip_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_cycle_strip_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_cycle_strip_panel);
            draw_strip(dc, m_cycle_strip_panel);
        });
        m_pattern_panel_sizer->Add(m_cycle_strip_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);

        top_sizer->Add(m_pattern_panel_sizer, 0, wxEXPAND);
    }

    m_line_above_swatch = new wxStaticLine(this);
    top_sizer->Add(m_line_above_swatch, 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    // Recommended swatches - use wxScrolledWindow with fixed height
    m_recommended_label = new wxStaticText(this, wxID_ANY, _L("Recommended"));
    top_sizer->Add(m_recommended_label, 0, wxLEFT | wxTOP, M);
    m_swatch_grid_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
                                               wxSize(-1, FromDIP(100)), wxVSCROLL);
    m_swatch_grid_panel->SetMinSize(wxSize(-1, FromDIP(100)));
    m_swatch_grid_panel->SetScrollRate(0, FromDIP(8));
    build_swatch_grid();
    top_sizer->Add(m_swatch_grid_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);
    m_line_below_swatch = new wxStaticLine(this);
    top_sizer->Add(m_line_below_swatch, 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    // Buttons
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_cancel  = new Button(this, _L("Cancel"));
    m_btn_confirm = new Button(this, _L("Confirm"));
    m_btn_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_cancel,  0, wxRIGHT, FromDIP(8));
    btn_sizer->Add(m_btn_confirm, 0);
    top_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, M);

    SetSizer(top_sizer);

    rebuild_filament_rows();

    // RadioGroup fires wxEVT_BUTTON on its internal label buttons; bind at panel level
    m_mode_radio->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this](wxCommandEvent&) {
        on_mode_changed(m_mode_radio->GetSelection());
    });
    m_gradient_selector->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        int val = m_gradient_selector->value();
        if (m_ratio_label_a) m_ratio_label_a->SetLabel(wxString::Format("%d%%", 100 - val));
        if (m_ratio_label_b) m_ratio_label_b->SetLabel(wxString::Format("%d%%", val));
        if (m_ratio_label_sizer) m_ratio_label_sizer->Layout();
        if (m_preview_panel) m_preview_panel->Refresh();
        if (m_strip_panel)   m_strip_panel->Refresh();
    });
    m_btn_cancel->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_btn_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        collect_result();
        EndModal(wxID_OK);
    });

    Fit();
    CentreOnParent();

    // Trigger after Fit() so the panel has a real size when the async result arrives.
    if (m_current_mode == MODE_MATCH && m_match_panel)
        m_match_panel->begin_initial_recipe_load();
}

// ---------------------------------------------------------------------------
// Barycentric triangle utilities
// ---------------------------------------------------------------------------

struct TriPt { double x, y; };

static double tri_area2(TriPt a, TriPt b, TriPt c)
{
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

static void tri_bary(TriPt p, TriPt v0, TriPt v1, TriPt v2,
                     double& w0, double& w1, double& w2)
{
    double total = tri_area2(v0, v1, v2);
    if (std::abs(total) < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = tri_area2(p, v1, v2) / total;
    w1 = tri_area2(v0, p, v2) / total;
    w2 = 1.0 - w0 - w1;
}

static TriPt tri_clamp_pt(TriPt p, TriPt v0, TriPt v1, TriPt v2)
{
    double w0, w1, w2;
    tri_bary(p, v0, v1, v2, w0, w1, w2);
    return { w0 * v0.x + w1 * v1.x + w2 * v2.x,
             w0 * v0.y + w1 * v1.y + w2 * v2.y };
}

// ---------------------------------------------------------------------------

int MixedFilamentDialog::get_filament_index(int row_idx) const
{
    if (row_idx < 0 || row_idx >= (int)m_filament_rows.size()) return 0;
    const auto& row = m_filament_rows[row_idx];
    int cb_idx = row.combo->GetSelection();
    if (cb_idx < 0 || cb_idx >= (int)row.filament_indices.size()) return 0;
    return row.filament_indices[cb_idx];
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_all_combos()
{
    // Get current selections from m_filament_rows
    std::vector<int> current_selections;
    for (const auto& row : m_filament_rows) {
        current_selections.push_back(get_filament_index(&row - &m_filament_rows[0]));
    }
    
    rebuild_all_combos_with_selections(current_selections);
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_all_combos_with_selections(const std::vector<int>& selections)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();
    
    for (size_t i = 0; i < m_filament_rows.size(); ++i) {
        auto& row = m_filament_rows[i];
        ComboBox* cb = row.combo;
        
        // Build set of filaments already used by other rows
        std::set<int> used_by_others;
        for (size_t k = 0; k < m_filament_rows.size(); ++k) {
            if (k != i && k < selections.size()) {
                used_by_others.insert(selections[k]);
            }
        }
        
        // Clear and rebuild combo box
        cb->Clear();
        row.filament_indices.clear();
        
        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            // Skip filaments already used by other rows
            if (used_by_others.find(j) != used_by_others.end()) continue;
            
            wxString display_name;
            if (preset_bundle && j < (int)filament_presets.size()) {
                const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
                if (preset) {
                    display_name = from_u8(preset->label(false));
                }
            }
            if (display_name.empty()) {
                display_name = wxString::Format("F%d", j + 1);
            }
            wxBitmap* icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(16), FromDIP(16));
            cb->Append(display_name, icon ? icon->ConvertToImage() : wxNullImage);
            row.filament_indices.push_back(j);
        }
        
        // Restore selection from the provided selections vector
        int sel = (i < selections.size()) ? selections[i] : 0;
        for (int k = 0; k < (int)row.filament_indices.size(); ++k) {
            if (row.filament_indices[k] == sel) {
                cb->SetSelection(k);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------

void MixedFilamentDialog::rebuild_filament_rows()
{
    m_filament_rows_sizer->Clear(true);
    m_filament_rows.clear();

    const int max_idx = std::max(0, (int)m_filament_colours.size() - 1);
    std::vector<int> sels;

    // Detect "all-ids" format (match recipe): gradient_component_ids contains
    // component_a as its first entry, meaning all filaments are listed there.
    const std::string &ids = m_result.gradient_component_ids;
    const bool all_ids_format = !ids.empty() &&
        (ids[0] - '0') == (int)m_result.component_a;

    if (all_ids_format) {
        for (char c : ids)
            sels.push_back(std::clamp(int(c - '1'), 0, max_idx));
    } else {
        sels.push_back(std::clamp((int)m_result.component_a - 1, 0, max_idx));
        sels.push_back(std::clamp((int)m_result.component_b - 1, 0, max_idx));
        for (char c : ids) {
            int idx = c - '1';
            sels.push_back(std::clamp(idx, 0, max_idx));
        }
    }

    int count = (int)sels.size();
    if (m_current_mode == MODE_GRADIENT)
        count = 2;
    else
        count = std::max(2, std::min(count, max_filaments_for_mode(m_current_mode)));

    for (int i = 0; i < count; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* row_lbl = new wxStaticText(m_filament_rows_panel, wxID_ANY,
                                         wxString::Format(_L("Filament %d"), i + 1),
                                         wxDefaultPosition, wxSize(FromDIP(60), -1));
        row->Add(row_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        // Note: sw (color swatch panel) is kept for internal state tracking but hidden
        // The color is now shown inside the ComboBox via get_extruder_color_icon
        auto* sw = new wxPanel(m_filament_rows_panel, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetMinSize(wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetBackgroundStyle(wxBG_STYLE_PAINT);
        sw->Hide();  // Hide the external swatch, color is shown in ComboBox

        auto* cb = new ComboBox(m_filament_rows_panel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(FromDIP(180), -1),
                                0, nullptr, wxCB_READONLY);

        // Get filament preset names from preset bundle (same as Plater)
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        const std::vector<std::string>& filament_presets = preset_bundle ? preset_bundle->filament_presets : std::vector<std::string>();

        // Build set of filaments already used by other rows (based on sels array)
        std::set<int> used_by_others;
        for (int k = 0; k < count; ++k) {
            if (k != i && k < (int)sels.size()) {
                used_by_others.insert(sels[k]);
            }
        }

        // Create FilamentRow and build mapping
        FilamentRow fr;
        fr.swatch = sw;
        fr.combo = cb;

        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            // Skip filaments already used by other rows
            if (used_by_others.find(j) != used_by_others.end()) continue;

            wxString display_name;
            if (preset_bundle && j < (int)filament_presets.size()) {
                const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[j]);
                if (preset) {
                    display_name = from_u8(preset->label(false));
                }
            }
            if (display_name.empty()) {
                display_name = wxString::Format("F%d", j + 1);
            }
            // Use the same icon style as Plater (with filament number label)
            wxBitmap* icon = get_extruder_color_icon(m_filament_colours[j], std::to_string(j + 1), FromDIP(16), FromDIP(16));
            cb->Append(display_name, icon ? icon->ConvertToImage() : wxNullImage);
            fr.filament_indices.push_back(j);  // Map combo box index to actual filament index
        }

        // Find the combo box index for the selected filament
        int sel = (i < (int)sels.size()) ? sels[i] : i;
        sel = std::max(0, std::min(sel, (int)m_filament_colours.size() - 1));
        for (int k = 0; k < (int)fr.filament_indices.size(); ++k) {
            if (fr.filament_indices[k] == sel) {
                cb->SetSelection(k);
                break;
            }
        }

        wxColour init_col = parse_mixed_color(m_filament_colours[sel]);
        sw->Bind(wxEVT_PAINT, [sw, init_col](wxPaintEvent&) mutable {
            wxAutoBufferedPaintDC dc(sw);
            dc.SetBackground(wxBrush(init_col));
            dc.Clear();
        });

        // Add to m_filament_rows before binding so we can use row index
        m_filament_rows.push_back(fr);
        int row_idx = (int)m_filament_rows.size() - 1;

        cb->Bind(wxEVT_COMBOBOX, [this, row_idx](wxCommandEvent&) {
            // Sync current selection to result first
            sync_rows_to_result();
            // Rebuild all combo boxes to update available options
            rebuild_all_combos();
            // Update preview
            if (m_tri_picker) m_tri_picker->Refresh();
            update_preview();
        });

        row->Add(cb, 1, wxALIGN_CENTER_VERTICAL);

        m_filament_rows_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    }

    m_filament_rows_panel->Layout();
    update_ratio_or_tri_visibility();

    if (m_swatch_grid_panel) {
        m_swatch_grid_panel->DestroyChildren();
        build_swatch_grid();
    }

    update_preview();
}

void MixedFilamentDialog::build_tri_picker()
{
    static constexpr int TRI_SIZE = 160;
    m_tri_picker = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetMinSize(wxSize(FromDIP(TRI_SIZE), FromDIP(TRI_SIZE)));
    m_tri_picker->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto get_verts = [this]() -> std::tuple<TriPt, TriPt, TriPt> {
        wxSize sz = m_tri_picker->GetClientSize();
        double pw = sz.GetWidth(), ph = sz.GetHeight();
        double margin = FromDIP(20);
        double side = std::min(pw, ph) - 2 * margin;
        double tri_h = side * std::sqrt(3.0) / 2.0;
        double cx = pw / 2.0;
        double top_y = (ph - tri_h) / 2.0;
        double bot_y = top_y + tri_h;
        return { {cx, top_y}, {cx - side / 2.0, bot_y}, {cx + side / 2.0, bot_y} };
    };

    auto safe_col = [&](int row) -> wxColour {
        if (row >= (int)m_filament_rows.size()) return wxColour(128,128,128);
        int s = get_filament_index(row);
        s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
        return parse_mixed_color(m_filament_colours[s]);
    };

    m_tri_picker->Bind(wxEVT_PAINT, [this, get_verts, safe_col](wxPaintEvent&) {
        wxBufferedPaintDC dc(m_tri_picker);
        wxSize sz = m_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();

        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        // Create image for gradient rendering (more efficient than DrawPoint)
        const int img_w = sz.GetWidth();
        const int img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        
        // Fill with background color
        const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char* data = img.GetData();

        // Draw gradient only inside triangle
        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                // Skip points outside triangle (negative barycentric coordinates)
                constexpr double EPSILON = 0.001;
                if (w0 < -EPSILON || w1 < -EPSILON || w2 < -EPSILON) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
                
                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()   * w0 + c1.Red()   * w1 + c2.Red()   * w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green() * w0 + c1.Green() * w1 + c2.Green() * w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue()  * w0 + c1.Blue()  * w1 + c2.Blue()  * w2), 0, 255);
            }
        }

        // Draw the image
        wxBitmap bmp(img);
        dc.DrawBitmap(bmp, 0, 0);

        // Draw triangle outline
        wxPoint pts[3] = {{(int)v0.x, (int)v0.y}, {(int)v1.x, (int)v1.y}, {(int)v2.x, (int)v2.y}};
        dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#CECECE")), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawPolygon(3, pts);

        // Draw selection circle
        double hx = m_tri_wx*v0.x + m_tri_wy*v1.x + m_tri_wz*v2.x;
        double hy = m_tri_wx*v0.y + m_tri_wy*v1.y + m_tri_wz*v2.y;
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.SetPen(wxPen(wxColour("#262E30"), FromDIP(2)));
        dc.DrawCircle((int)hx, (int)hy, FromDIP(5));

        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30")));
        int r0 = (int)(m_tri_wx * 100 + 0.5);
        int r1 = (int)(m_tri_wy * 100 + 0.5);
        int r2 = 100 - r0 - r1;
        auto draw_label = [&](TriPt v, int pct, int dx, int dy) {
            wxString s = wxString::Format("%d%%", pct);
            wxSize ts = dc.GetTextExtent(s);
            dc.DrawText(s, (int)v.x - ts.GetWidth()/2 + dx, (int)v.y + dy);
        };
        draw_label(v0, r0, 0, -FromDIP(14));
        draw_label(v1, r1, -FromDIP(4), FromDIP(3));
        draw_label(v2, r2,  FromDIP(4), FromDIP(3));
    });

    auto handle_mouse = [this, get_verts](wxMouseEvent& e, bool is_down) {
        auto [v0, v1, v2] = get_verts();
        TriPt p = {(double)e.GetX(), (double)e.GetY()};
        if (is_down) {
            m_tri_dragging = true;
            m_tri_picker->CaptureMouse();
        }
        if (!m_tri_dragging) return;
        TriPt clamped = tri_clamp_pt(p, v0, v1, v2);
        tri_bary(clamped, v0, v1, v2, m_tri_wx, m_tri_wy, m_tri_wz);
        // Clamp each weight to be at least 10% and at most 90%
        constexpr double MIN_WEIGHT = 0.10;
        constexpr double MAX_WEIGHT = 0.90;
        m_tri_wx = std::clamp(m_tri_wx, MIN_WEIGHT, MAX_WEIGHT);
        m_tri_wy = std::clamp(m_tri_wy, MIN_WEIGHT, MAX_WEIGHT);
        m_tri_wz = std::clamp(m_tri_wz, MIN_WEIGHT, MAX_WEIGHT);
        // Renormalize after clamping
        double sum = m_tri_wx + m_tri_wy + m_tri_wz;
        if (sum > 0) { m_tri_wx /= sum; m_tri_wy /= sum; m_tri_wz /= sum; }
        update_preview();
        m_tri_picker->Refresh();
    };

    m_tri_picker->Bind(wxEVT_LEFT_DOWN, [handle_mouse](wxMouseEvent& e) { handle_mouse(e, true); });
    m_tri_picker->Bind(wxEVT_MOTION,    [handle_mouse](wxMouseEvent& e) { handle_mouse(e, false); });
    m_tri_picker->Bind(wxEVT_LEFT_UP,   [this](wxMouseEvent&) {
        if (m_tri_dragging) {
            m_tri_dragging = false;
            if (m_tri_picker->HasCapture()) m_tri_picker->ReleaseMouse();
        }
    });
    m_tri_picker->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_tri_dragging = false;
    });
}

void MixedFilamentDialog::update_ratio_or_tri_visibility()
{
    bool is_ratio_mode = (m_current_mode == MODE_RATIO);
    bool is_cycle_mode = (m_current_mode == MODE_CYCLE);
    bool is_match_mode = (m_current_mode == MODE_MATCH);
    bool is_gradient_mode = (m_current_mode == MODE_GRADIENT);
    int  n             = (int)m_filament_rows.size();
    bool show_slider   = is_ratio_mode && (n == 2);
    bool show_tri      = is_ratio_mode && (n == 3);

    if (m_filament_title)      m_filament_title->Show(!is_match_mode);
    if (m_filament_rows_panel) m_filament_rows_panel->Show(!is_match_mode);
    if (m_preview_panel)       m_preview_panel->Show(!is_match_mode);
    if (m_preview_label)       m_preview_label->Show(!is_match_mode);
    bool show_swatches = !is_match_mode && !is_cycle_mode;
    if (m_swatch_grid_panel)   m_swatch_grid_panel->Show(show_swatches);
    if (m_recommended_label)   m_recommended_label->Show(show_swatches);
    if (m_line_below_mid)      m_line_below_mid->Show(show_swatches);
    if (m_line_above_swatch)   m_line_above_swatch->Show(show_swatches);
    if (m_line_below_swatch)   m_line_below_swatch->Show(show_swatches);

    // Update add/remove button visibility
    bool can_remove = !is_match_mode && !is_gradient_mode && (n > 2);
    bool can_add = !is_match_mode && !is_gradient_mode && (n < max_filaments_for_mode(m_current_mode));
    if (m_btn_remove_filament) m_btn_remove_filament->Show(can_remove);
    if (m_btn_add_filament)    m_btn_add_filament->Show(can_add);

    if (m_gradient_bar_sizer)  m_gradient_bar_sizer->ShowItems(show_slider && !is_match_mode);
    if (m_tri_picker_sizer)    m_tri_picker_sizer->ShowItems(show_tri && !is_match_mode);
    if (m_pattern_panel_sizer) m_pattern_panel_sizer->ShowItems(is_cycle_mode);
    if (m_match_panel_sizer)   m_match_panel_sizer->ShowItems(is_match_mode);

    const bool show_gradient_swap = (m_current_mode == MODE_GRADIENT) && (n == 2);
    if (m_btn_swap_gradient_dir)
        m_btn_swap_gradient_dir->Show(show_gradient_swap);
}

void MixedFilamentDialog::resize_gradient_ids(int target_count)
{
    int extra = target_count - 2;
    if (extra <= 0) { m_result.gradient_component_ids.clear(); return; }
    std::string ids = m_result.gradient_component_ids;
    
    // Build set of already used filament indices
    std::set<int> used;
    used.insert((int)m_result.component_a - 1);
    used.insert((int)m_result.component_b - 1);
    for (char c : ids) {
        used.insert(c - '1');
    }
    
    // Find unused filaments for new slots
    while ((int)ids.size() < extra) {
        int new_filament = 0;
        for (int j = 0; j < (int)m_filament_colours.size(); ++j) {
            if (used.find(j) == used.end()) {
                new_filament = j;
                used.insert(j);
                break;
            }
        }
        ids += char('1' + new_filament);
    }
    ids.resize((size_t)extra);
    m_result.gradient_component_ids = ids;
}

void MixedFilamentDialog::sync_rows_to_result()
{
    if (m_filament_rows.empty()) return;
    int a = get_filament_index(0);
    int b = (m_filament_rows.size() > 1) ? get_filament_index(1) : a;
    m_result.component_a = (unsigned int)(std::max(0, a) + 1);
    m_result.component_b = (unsigned int)(std::max(0, b) + 1);
    std::string ids;
    for (int i = 2; i < (int)m_filament_rows.size(); ++i) {
        int s = get_filament_index(i);
        ids += char('1' + std::max(0, s));
    }
    m_result.gradient_component_ids = ids;
}

std::string MixedFilamentDialog::compute_preview_color()
{
    if (m_filament_colours.empty() || m_filament_rows.empty()) return "#808080";

    int n   = (int)m_filament_rows.size();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;

    auto safe_idx = [&](int raw) {
        return std::max(0, std::min(raw, (int)m_filament_colours.size() - 1));
    };

    // Cycle mode: blend by pattern frequency using the same logic as Plater.cpp
    if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        if (!normalized.empty()) {
            // Get component_a and component_b (1-based extruder IDs)
            const unsigned int component_a = (unsigned int)(get_filament_index(0) + 1);
            const unsigned int component_b = (unsigned int)((m_filament_rows.size() > 1 ? get_filament_index(1) : 0) + 1);
            const size_t num_physical = m_filament_colours.size();

            // Decode pattern tokens to extruder IDs (same as Plater.cpp)
            std::vector<unsigned int> sequence;
            for (char token : normalized) {
                unsigned int extruder_id = 0;
                if (token == '1')
                    extruder_id = component_a;
                else if (token == '2')
                    extruder_id = component_b;
                else if (token >= '3' && token <= '9')
                    extruder_id = (unsigned int)(token - '0');
                
                if (extruder_id >= 1 && extruder_id <= num_physical)
                    sequence.push_back(extruder_id);
            }

            if (!sequence.empty()) {
                std::vector<wxColour> palette;
                palette.reserve(m_filament_colours.size());
                for (const auto& s : m_filament_colours)
                    palette.push_back(parse_mixed_color(s));
                wxColour result = blend_sequence_filament_mixer(palette, sequence);
                return wxString::Format("#%02X%02X%02X", result.Red(), result.Green(), result.Blue()).ToStdString();
            }
        }
    }

    if (n == 2) {
        int ia = safe_idx(get_filament_index(0));
        int ib = safe_idx(get_filament_index(1));
        int w = (m_current_mode == MODE_RATIO) ? val : 50;
        return MixedFilamentManager::blend_color(
            m_filament_colours[ia], m_filament_colours[ib], 100 - w, w);
    }

    // 3-filament ratio mode: linear weighted average matching tri picker rendering
    if (n == 3 && m_current_mode == MODE_RATIO) {
        auto get_col = [&](int row) {
            return parse_mixed_color(m_filament_colours[safe_idx(get_filament_index(row))]);
        };
        wxColour c0 = get_col(0), c1 = get_col(1), c2 = get_col(2);
        int r = (int)(c0.Red()   * m_tri_wx + c1.Red()   * m_tri_wy + c2.Red()   * m_tri_wz + 0.5);
        int g = (int)(c0.Green() * m_tri_wx + c1.Green() * m_tri_wy + c2.Green() * m_tri_wz + 0.5);
        int b = (int)(c0.Blue()  * m_tri_wx + c1.Blue()  * m_tri_wy + c2.Blue()  * m_tri_wz + 0.5);
        return wxString::Format("#%02X%02X%02X",
            std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)).ToStdString();
    }

    int per = 100 / n;
    int rem = 100 - per * n;
    std::vector<std::pair<std::string, int>> cp;
    for (int i = 0; i < n; ++i) {
        int idx = safe_idx(get_filament_index(i));
        cp.push_back(std::make_pair(m_filament_colours[idx], per + (i == 0 ? rem : 0)));
    }
    return MixedFilamentManager::blend_color_multi(cp);
}

void MixedFilamentDialog::draw_strip(wxDC& dc, wxPanel* panel)
{
    wxSize sz = panel->GetClientSize();
    if (sz.x <= 0 || sz.y <= 0 || m_filament_rows.empty()) {
        dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return;
    }
    int n = (int)m_filament_rows.size();
    static constexpr int STRIP_SEGMENTS = 24;
    bool vertical = (m_current_mode == MODE_RATIO && n == 3);
    int total_px = vertical ? sz.y : sz.x;

    // In 2-color ratio mode, build a pattern reflecting the A:B ratio
    std::vector<int> pattern;
    if (m_current_mode == MODE_RATIO && n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        const int pct_b = std::clamp(val, 0, 100);
        const int pct_a = 100 - pct_b;
        int ratio_a = 1, ratio_b = 0;
        if (pct_b >= 100) {
            ratio_a = 0; ratio_b = 1;
        } else if (pct_b > 0) {
            const bool b_major = pct_b >= pct_a;
            const int major = b_major ? pct_b : pct_a;
            const int minor = b_major ? pct_a : pct_b;
            const int layers = std::max(1, (int)std::lround((double)major / (double)std::max(1, minor)));
            ratio_a = b_major ? 1 : layers;
            ratio_b = b_major ? layers : 1;
            const int g = std::gcd(ratio_a, ratio_b);
            if (g > 1) { ratio_a /= g; ratio_b /= g; }
        }
        const int cycle = std::max(1, ratio_a + ratio_b);
        for (int pos = 0; pos < cycle; ++pos) {
            const int b_before = (pos * ratio_b) / cycle;
            const int b_after  = ((pos + 1) * ratio_b) / cycle;
            pattern.push_back((b_after > b_before) ? 1 : 0);
        }
    } else if (m_current_mode == MODE_RATIO && n == 3) {
        int w0 = std::max(1, (int)std::round(m_tri_wx * 100));
        int w1 = std::max(1, (int)std::round(m_tri_wy * 100));
        int w2 = std::max(1, 100 - w0 - w1);
        int g = std::gcd(std::gcd(w0, w1), w2);
        if (g > 1) { w0 /= g; w1 /= g; w2 /= g; }
        const int counts[3] = {w0, w1, w2};
        const int total = w0 + w1 + w2;
        int emitted[3] = {0, 0, 0};
        for (int pos = 0; pos < total; ++pos) {
            int best = 0;
            double best_score = -1e9;
            for (int i = 0; i < 3; ++i) {
                double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                if (score > best_score) { best_score = score; best = i; }
            }
            ++emitted[best];
            pattern.push_back(best);
        }
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        // Decode pattern using the same logic as Plater.cpp:
        //   '1' → component_a, '2' → component_b,
        //   '3'-'9' → direct physical filament ID (1-based).
        // Store as negative -(id) to distinguish direct IDs from component IDs.
        const std::string raw = into_u8(m_pattern_ctrl->GetValue());
        const std::string normalized = MixedFilamentManager::normalize_manual_pattern(raw);
        const unsigned int component_a = (unsigned int)(get_filament_index(0) + 1);
        const unsigned int component_b = (unsigned int)((m_filament_rows.size() > 1 ? get_filament_index(1) : 0) + 1);
        const size_t num_physical = m_filament_colours.size();
        
        for (char token : normalized) {
            unsigned int extruder_id = 0;
            if (token == '1')
                extruder_id = component_a;
            else if (token == '2')
                extruder_id = component_b;
            else if (token >= '3' && token <= '9')
                extruder_id = (unsigned int)(token - '0');
            
            if (extruder_id >= 1 && extruder_id <= num_physical)
                pattern.push_back(-(int)extruder_id);  // Store as negative to indicate direct extruder ID
        }
        if (pattern.empty())
            for (int i = 0; i < n; ++i) pattern.push_back(i);
    } else if (m_current_mode == MODE_MATCH && m_match_panel) {
        const MixedColorMatchRecipeResult recipe = m_match_panel->selected_recipe();
        if (recipe.valid) {
            std::vector<int> weights = expand_color_match_recipe_weights(recipe, m_filament_colours.size());
            int g = 0;
            for (int w : weights) g = std::gcd(g, w);
            if (g > 1) for (int &w : weights) w /= g;
            std::vector<int> ids, counts;
            for (int i = 0; i < (int)weights.size(); ++i)
                if (weights[i] > 0) { ids.push_back(i); counts.push_back(weights[i]); }
            const int total = std::accumulate(counts.begin(), counts.end(), 0);
            std::vector<int> emitted(counts.size(), 0);
            for (int pos = 0; pos < total; ++pos) {
                int best = 0; double best_score = -1e9;
                for (int i = 0; i < (int)counts.size(); ++i) {
                    double score = double(pos + 1) * double(counts[i]) / double(total) - double(emitted[i]);
                    if (score > best_score) { best_score = score; best = i; }
                }
                ++emitted[best];
                pattern.push_back(-(ids[best] + 1));  // negative = direct colour index
            }
        }
        if (pattern.empty()) {
            const int num_colours = (int)m_filament_colours.size();
            for (int i = 0; i < num_colours; ++i) pattern.push_back(-(i + 1));
        }
    } else {
        for (int i = 0; i < n; ++i) pattern.push_back(i);
    }

    if (pattern.empty())
        return;

    int seg = std::max(1, total_px / STRIP_SEGMENTS);
    for (int s = 0; s < STRIP_SEGMENTS; ++s) {
        int entry = pattern[s % (int)pattern.size()];
        int idx;
        if (entry < 0) {
            idx = std::max(0, std::min(-entry - 1, (int)m_filament_colours.size() - 1));
        } else {
            int row = std::min(entry, n - 1);
            idx = get_filament_index(row);
            idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));
        }
        dc.SetBrush(wxBrush(parse_mixed_color(m_filament_colours[idx])));
        dc.SetPen(*wxTRANSPARENT_PEN);
        const int pos = s * seg;
        const int len = (s == STRIP_SEGMENTS - 1) ? (total_px - pos) : seg;
        if (vertical)
            dc.DrawRectangle(0, pos, sz.x, len);
        else
            dc.DrawRectangle(pos, 0, len, sz.y);
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(wxColour(180, 180, 180), 1));
    dc.DrawRectangle(0, 0, sz.x, sz.y);
}

void MixedFilamentDialog::build_swatch_grid()
{
    int n = (int)m_filament_colours.size();
    if (n < 2) return;

    bool is_ratio_3 = (m_current_mode == MODE_RATIO) && ((int)m_filament_rows.size() == 3);

    // Build candidates using the same preset logic as match mode.
    // For ratio mode with 2 rows: pair candidates at 25/50/75.
    // For ratio mode with 3 rows: pair + triple candidates.
    // For other modes: pairs at 50 only.
    struct Candidate {
        wxColour color;
        wxString tooltip;
        // For pair: row indices and b_pct. For triple: row indices and weights.
        int rows[3] = {0, 0, 0};
        int n_rows   = 2;
        int b_pct    = 50;       // used when n_rows == 2
        double wx    = 1.0/3.0;  // used when n_rows == 3
        double wy    = 1.0/3.0;
        double wz    = 1.0/3.0;
    };

    std::vector<Candidate> candidates;

    if (m_current_mode == MODE_RATIO) {
        std::vector<wxColour> palette;
        palette.reserve(n);
        for (const auto& s : m_filament_colours)
            palette.push_back(parse_mixed_color(s));

        if (is_ratio_3) {
            // Three-color mode: only show triple candidates
            auto make_triple_candidate = [&](int i, int j, int k,
                                             const std::vector<int>& input_weights) -> Candidate {
                std::vector<unsigned int> ids = {(unsigned)(i+1), (unsigned)(j+1), (unsigned)(k+1)};
                auto recipe = build_multi_color_match_candidate(palette, ids, input_weights);
                if (!recipe.valid) return {};

                Candidate c;
                c.color  = recipe.preview_color;
                c.n_rows = 3;
                // Use original input order for rows (i, j, k correspond to rows 0, 1, 2)
                c.rows[0] = i;
                c.rows[1] = j;
                c.rows[2] = k;
                // Weights correspond to the original input order
                c.wx = input_weights[0] / 100.0;
                c.wy = input_weights[1] / 100.0;
                c.wz = input_weights[2] / 100.0;
                c.tooltip = wxString::Format("F%d(%d%%)+F%d(%d%%)+F%d(%d%%)",
                    i+1, input_weights[0],
                    j+1, input_weights[1],
                    k+1, input_weights[2]);
                return c;
            };

            const std::vector<int> eq = normalize_color_match_weights({1, 1, 1}, 3);
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    for (int k = j + 1; k < n; ++k) {
                        auto c_eq = make_triple_candidate(i, j, k, eq);
                        if (c_eq.n_rows == 3) candidates.push_back(c_eq);

                        for (int dom = 0; dom < 3; ++dom) {
                            std::vector<int> dw = {25, 25, 25};
                            dw[dom] = 50;
                            auto c_dom = make_triple_candidate(i, j, k, dw);
                            if (c_dom.n_rows == 3) candidates.push_back(c_dom);
                        }
                    }
                }
            }
        } else {
            // Two-color mode: only show pair candidates at 50:50
            for (int i = 0; i < n; ++i) {
                for (int j = i + 1; j < n; ++j) {
                    auto recipe = build_pair_color_match_candidate(palette, i + 1, j + 1, 50);
                    if (!recipe.valid) continue;
                    Candidate c;
                    c.color   = recipe.preview_color;
                    c.tooltip = wxString::Format("F%d(50%%) + F%d(50%%)", i+1, j+1);
                    c.rows[0] = i; c.rows[1] = j;
                    c.n_rows  = 2;
                    c.b_pct   = 50;
                    candidates.push_back(c);
                }
            }
        }
    } else {
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                std::string blended = MixedFilamentManager::blend_color(
                    m_filament_colours[i], m_filament_colours[j], 50, 50);
                Candidate c;
                c.color   = parse_mixed_color(blended);
                c.tooltip = wxString::Format("F%d + F%d", i+1, j+1);
                c.rows[0] = i; c.rows[1] = j;
                c.n_rows  = 2;
                c.b_pct   = 50;
                candidates.push_back(c);
            }
        }
    }

    if (candidates.empty()) return;
    int cols = std::min((int)candidates.size(), 8);
    auto* grid = new wxGridSizer(0, cols, FromDIP(4), FromDIP(4));

    for (const auto& cand : candidates) {
        auto* btn = new wxBitmapButton(m_swatch_grid_panel, wxID_ANY,
                                       make_color_bitmap(cand.color, FromDIP(20)),
                                       wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
        btn->SetToolTip(cand.tooltip);

        btn->Bind(wxEVT_BUTTON, [this, cand](wxCommandEvent&) {
            // Collect target filament indices
            std::vector<int> selections;
            for (int r = 0; r < cand.n_rows; ++r)
                selections.push_back(cand.rows[r]);

            // Update m_result
            if (selections.size() >= 2) {
                m_result.component_a = (unsigned int)(selections[0] + 1);
                m_result.component_b = (unsigned int)(selections[1] + 1);
            }

            // Rebuild all combos with the new selections
            rebuild_all_combos_with_selections(selections);

            if (cand.n_rows == 2 && m_current_mode == MODE_RATIO && m_gradient_selector) {
                m_gradient_selector->set_value(cand.b_pct);
                if (m_ratio_label_a) m_ratio_label_a->SetLabel(wxString::Format("%d%%", 100 - cand.b_pct));
                if (m_ratio_label_b) m_ratio_label_b->SetLabel(wxString::Format("%d%%", cand.b_pct));
                if (m_ratio_label_sizer) m_ratio_label_sizer->Layout();
            } else if (cand.n_rows == 3) {
                m_tri_wx = cand.wx;
                m_tri_wy = cand.wy;
                m_tri_wz = cand.wz;
            }
            update_preview();
        });
        grid->Add(btn);
    }

    m_swatch_grid_panel->SetSizer(grid);
    m_swatch_grid_panel->FitInside();
}

void MixedFilamentDialog::on_mode_changed(int mode_index)
{
    sync_rows_to_result();
    m_current_mode = mode_index;
    int max_f = max_filaments_for_mode(mode_index);
    if ((int)m_filament_rows.size() > max_f)
        resize_gradient_ids(max_f);
    bool locked = (mode_index == MODE_MATCH);
    m_gradient_selector->Enable(!locked);
    if (locked) m_gradient_selector->set_colors(
        parse_mixed_color(m_filament_colours[std::max(0,(int)m_result.component_a-1)]),
        parse_mixed_color(m_filament_colours[std::max(0,(int)m_result.component_b-1)]));
    if (mode_index == MODE_CYCLE && m_pattern_ctrl) {
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(m_result.manual_pattern);
        m_pattern_ctrl->SetValue(from_u8(norm.empty() ? "12" : norm));
    }
    if (mode_index == MODE_MATCH && m_match_panel)
        m_match_panel->begin_initial_recipe_load();
    rebuild_filament_rows();
    update_ratio_or_tri_visibility();
    update_preview();
    Layout(); Fit();
}

void MixedFilamentDialog::update_gradient_selector_colors()
{
    if (!m_gradient_selector || m_filament_rows.size() < 2) return;
    int ia = get_filament_index(0);
    int ib = get_filament_index(1);
    ia = std::max(0, std::min(ia, (int)m_filament_colours.size()-1));
    ib = std::max(0, std::min(ib, (int)m_filament_colours.size()-1));
    m_gradient_selector->set_colors(parse_mixed_color(m_filament_colours[ia]),
                                    parse_mixed_color(m_filament_colours[ib]));
    int val = m_gradient_selector->value();
    if (m_ratio_label_a)
        m_ratio_label_a->SetLabel(wxString::Format("%d%%", 100 - val));
    if (m_ratio_label_b)
        m_ratio_label_b->SetLabel(wxString::Format("%d%%", val));
    if (m_ratio_label_sizer)
        m_ratio_label_sizer->Layout();
}

void MixedFilamentDialog::update_preview()
{
    if ((int)m_filament_rows.size() == 2)
        update_gradient_selector_colors();
    if (m_preview_panel)    m_preview_panel->Refresh();
    if (m_strip_panel)      m_strip_panel->Refresh();
    if (m_tri_strip_panel)  m_tri_strip_panel->Refresh();
    if (m_tri_picker)       m_tri_picker->Refresh();
    if (m_cycle_strip_panel) m_cycle_strip_panel->Refresh();
}

void MixedFilamentDialog::collect_result()
{
    sync_rows_to_result();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;
    m_result.mix_b_percent = val;
    // Default: drop Z-gradient state. Only MODE_GRADIENT re-enables it below.
    m_result.gradient_enabled = false;
    switch (m_current_mode) {
    case MODE_RATIO:
        m_result.gradient_component_weights.clear();
        if ((int)m_filament_rows.size() == 3) {
            m_result.distribution_mode = int(MixedFilament::LayerCycle);
            m_result.manual_pattern.clear();
            // Store all 3 filament ids so gradient_component_ids.size() >= 3,
            // which lets resolve() enter the weighted gradient branch.
            {
                std::string all_ids;
                for (int i = 0; i < 3; ++i) {
                    int s = get_filament_index(i);
                    all_ids += char('1' + std::max(0, s));
                }
                m_result.gradient_component_ids = all_ids;
            }
            int r0 = (int)(m_tri_wx * 100 + 0.5);
            int r1 = (int)(m_tri_wy * 100 + 0.5);
            int r2 = 100 - r0 - r1;
            m_result.gradient_component_weights =
                std::to_string(r0) + "/" + std::to_string(r1) + "/" + std::to_string(r2);
        } else {
            m_result.distribution_mode = int(MixedFilament::Simple);
            m_result.gradient_component_ids.clear();
            m_result.manual_pattern.clear();
            int steps_a = std::max(1, (100 - val) / 10);
            int steps_b = std::max(1, val / 10);
            m_result.ratio_a = steps_a;
            m_result.ratio_b = steps_b;
        }
        break;
    case MODE_CYCLE: {
        const std::string raw = m_pattern_ctrl ? into_u8(m_pattern_ctrl->GetValue()) : std::string();
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(raw);
        m_result.distribution_mode = int(MixedFilament::Simple);
        m_result.manual_pattern = norm.empty() ? "12" : norm;
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        break;
    }
    case MODE_MATCH: {
        const MixedColorMatchRecipeResult recipe = m_match_panel ? m_match_panel->selected_recipe() : MixedColorMatchRecipeResult{};
        if (recipe.valid) {
            m_result.distribution_mode          = recipe.gradient_component_ids.empty()
                                                    ? int(MixedFilament::Simple)
                                                    : int(MixedFilament::LayerCycle);
            m_result.component_a                = recipe.component_a;
            m_result.component_b                = recipe.component_b;
            m_result.mix_b_percent              = recipe.mix_b_percent;
            m_result.manual_pattern             = recipe.manual_pattern;
            m_result.gradient_component_ids     = recipe.gradient_component_ids;
            m_result.gradient_component_weights = recipe.gradient_component_weights;
        } else {
            m_result.distribution_mode = int(MixedFilament::Simple);
            m_result.mix_b_percent = 50;
            m_result.manual_pattern.clear();
            m_result.gradient_component_ids.clear();
            m_result.gradient_component_weights.clear();
            m_result.ratio_a = 1;
            m_result.ratio_b = 1;
        }
        break;
    }
    case MODE_GRADIENT:
        m_result.distribution_mode = int(MixedFilament::LayerCycle);
        // 2-filament Z gradient does not need extra component IDs/weights.
        m_result.gradient_component_ids.clear();
        m_result.gradient_component_weights.clear();
        m_result.manual_pattern.clear();
        m_result.gradient_enabled = true;
        
        // Gradient direction mapping:
        // Direction 0: A→B (component_a starts dominant at 80%, ends at 20%)
        //              gradient_start=0.8, gradient_end=0.2 (start > end)
        // Direction 1: B→A (component_a starts at 20%, ends dominant at 80%)
        //              gradient_start=0.2, gradient_end=0.8 (start < end)
        // 
        // The gradient_start/end values represent the ratio of component_a.
        // Component_b ratio is always (1 - component_a_ratio).
        
        if (m_gradient_direction == 0) {
            // A→B: component_a goes from dominant to minority
            m_result.gradient_start = MixedFilament::k_default_gradient_dominant;
            m_result.gradient_end   = MixedFilament::k_default_gradient_minority;
        } else {
            // B→A: component_a goes from minority to dominant
            m_result.gradient_start = MixedFilament::k_default_gradient_minority;
            m_result.gradient_end   = MixedFilament::k_default_gradient_dominant;
        }
        
        // Mid-blend layer ratio (50%) keeps non-gradient consumers consistent.
        m_result.mix_b_percent = 50;
        m_result.ratio_a = 1;
        m_result.ratio_b = 1;
        // Force at least 2 sublayers per layer so the LocalZ planner emits the
        // two sub-layers needed to realize the per-layer gradient ratio.
        if (m_result.local_z_max_sublayers < 2)
            m_result.local_z_max_sublayers = 2;
        break;
    }
    m_result.custom = true;
}

void MixedFilamentDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
    Layout(); Fit();
}

}} // namespace Slic3r::GUI
