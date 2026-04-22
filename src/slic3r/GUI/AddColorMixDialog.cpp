#include "AddColorMixDialog.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/dcbuffer.h>
#include <wx/statline.h>
#include <wx/sizer.h>
#include <wx/bmpbuttn.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

static constexpr int SWATCH_SIZE  = 16;
static constexpr int PREVIEW_SIZE = 80;
static constexpr int STRIP_HEIGHT = 24;

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

AddColorMixDialog::AddColorMixDialog(wxWindow* parent,
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

AddColorMixDialog::AddColorMixDialog(wxWindow* parent,
                                     const std::vector<std::string>& filament_colours,
                                     const Slic3r::MixedFilament& existing)
    : DPIDialog(parent, wxID_ANY, _L("Edit Color Mix"),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_filament_colours(filament_colours)
    , m_result(existing)
{
    if (!MixedFilamentManager::normalize_manual_pattern(existing.manual_pattern).empty())
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
            }
        }
    }

    build_ui();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

wxBitmap AddColorMixDialog::make_color_bitmap(const wxColour& c, int size)
{
    wxBitmap bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    return bmp;
}

int AddColorMixDialog::max_filaments_for_mode(int mode) const
{
    return (mode == MODE_RATIO) ? 3 : 4;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void AddColorMixDialog::build_ui()
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

    // "Mixed Filament" section title (hidden in match mode)
    m_filament_title = new wxStaticText(this, wxID_ANY, _L("Mixed Filament"));
    top_sizer->Add(m_filament_title, 0, wxLEFT | wxTOP, M);

    // Filament rows + preview
    auto* mid_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_filament_rows_panel = new wxPanel(this);
    m_filament_rows_sizer = new wxBoxSizer(wxVERTICAL);
    m_filament_rows_panel->SetSizer(m_filament_rows_sizer);
    mid_sizer->Add(m_filament_rows_panel, 1, wxEXPAND | wxRIGHT, M);

    auto* preview_col = new wxBoxSizer(wxVERTICAL);
    m_preview_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                  wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_preview_panel);
        if (m_current_mode == MODE_GRADIENT && m_filament_rows.size() >= 2) {
            wxSize sz = m_preview_panel->GetClientSize();
            int ia = std::max(0, std::min(m_filament_rows[0].combo->GetSelection(), (int)m_filament_colours.size()-1));
            int ib = std::max(0, std::min(m_filament_rows[1].combo->GetSelection(), (int)m_filament_colours.size()-1));
            wxColour ca = parse_mixed_color(m_filament_colours[ia]);
            wxColour cb = parse_mixed_color(m_filament_colours[ib]);
            wxImage img(sz.GetWidth(), sz.GetHeight());
            unsigned char* data = img.GetData();
            for (int y = 0; y < sz.GetHeight(); ++y) {
                for (int x = 0; x < sz.GetWidth(); ++x) {
                    float t = (sz.GetWidth() + sz.GetHeight() > 2)
                        ? float(x + y) / float(sz.GetWidth() + sz.GetHeight() - 2) : 0.5f;
                    wxColour c = blend_pair_filament_mixer(ca, cb, t);
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
    m_preview_label = new wxStaticText(this, wxID_ANY, _L("Mix Color Effect"));
    preview_col->Add(m_preview_label, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
    mid_sizer->Add(preview_col, 0, wxALIGN_TOP);

    top_sizer->Add(mid_sizer, 0, wxEXPAND | wxALL, M);
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
        m_pattern_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (m_cycle_strip_panel) m_cycle_strip_panel->Refresh();
        });
        m_pattern_panel_sizer->Add(m_cycle_strip_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, M);

        top_sizer->Add(m_pattern_panel_sizer, 0, wxEXPAND);
    }

    m_line_above_swatch = new wxStaticLine(this);
    top_sizer->Add(m_line_above_swatch, 0, wxEXPAND | wxLEFT | wxRIGHT, M);

    // Recommended swatches
    m_recommended_label = new wxStaticText(this, wxID_ANY, _L("Recommended"));
    top_sizer->Add(m_recommended_label, 0, wxLEFT | wxTOP, M);
    m_swatch_grid_panel = new wxPanel(this);
    build_swatch_grid();
    top_sizer->Add(m_swatch_grid_panel, 0, wxEXPAND | wxALL, M);
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
    double total = std::abs(tri_area2(v0, v1, v2));
    if (total < 1e-9) { w0 = w1 = w2 = 1.0 / 3.0; return; }
    w0 = std::abs(tri_area2(p, v1, v2)) / total;
    w1 = std::abs(tri_area2(v0, p, v2)) / total;
    w2 = 1.0 - w0 - w1;
    w0 = std::clamp(w0, 0.0, 1.0);
    w1 = std::clamp(w1, 0.0, 1.0);
    w2 = std::clamp(w2, 0.0, 1.0);
    double s = w0 + w1 + w2;
    if (s > 0) { w0 /= s; w1 /= s; w2 /= s; }
}

static TriPt tri_clamp_pt(TriPt p, TriPt v0, TriPt v1, TriPt v2)
{
    double w0, w1, w2;
    tri_bary(p, v0, v1, v2, w0, w1, w2);
    return { w0 * v0.x + w1 * v1.x + w2 * v2.x,
             w0 * v0.y + w1 * v1.y + w2 * v2.y };
}

// ---------------------------------------------------------------------------

void AddColorMixDialog::rebuild_filament_rows()
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
    count = std::max(2, std::min(count, max_filaments_for_mode(m_current_mode)));

    for (int i = 0; i < count; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* row_lbl = new wxStaticText(m_filament_rows_panel, wxID_ANY,
                                         wxString::Format(_L("Filament %d"), i + 1),
                                         wxDefaultPosition, wxSize(FromDIP(60), -1));
        row->Add(row_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));

        auto* sw = new wxPanel(m_filament_rows_panel, wxID_ANY, wxDefaultPosition,
                               wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetMinSize(wxSize(FromDIP(SWATCH_SIZE), FromDIP(SWATCH_SIZE)));
        sw->SetBackgroundStyle(wxBG_STYLE_PAINT);

        auto* cb = new ComboBox(m_filament_rows_panel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(FromDIP(120), -1),
                                0, nullptr, wxCB_READONLY);
        for (int j = 0; j < (int)m_filament_colours.size(); ++j)
            cb->Append(wxString::Format("F%d", j + 1),
                       make_color_bitmap(parse_mixed_color(m_filament_colours[j]), FromDIP(SWATCH_SIZE)));

        int sel = (i < (int)sels.size()) ? sels[i] : i;
        sel = std::max(0, std::min(sel, (int)m_filament_colours.size() - 1));
        cb->SetSelection(sel);

        wxColour init_col = parse_mixed_color(m_filament_colours[sel]);
        sw->Bind(wxEVT_PAINT, [sw, init_col](wxPaintEvent&) mutable {
            wxAutoBufferedPaintDC dc(sw);
            dc.SetBackground(wxBrush(init_col));
            dc.Clear();
        });

        cb->Bind(wxEVT_COMBOBOX, [this, sw, cb](wxCommandEvent&) {
            int s = cb->GetSelection();
            if (s >= 0 && s < (int)m_filament_colours.size()) {
                wxColour nc = parse_mixed_color(m_filament_colours[s]);
                sw->Bind(wxEVT_PAINT, [sw, nc](wxPaintEvent&) mutable {
                    wxAutoBufferedPaintDC dc(sw);
                    dc.SetBackground(wxBrush(nc));
                    dc.Clear();
                });
                sw->Refresh();
            }
            if (m_tri_picker) m_tri_picker->Refresh();
            update_preview();
        });

        row->Add(sw, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        row->Add(cb, 1, wxALIGN_CENTER_VERTICAL);

        if (m_current_mode == MODE_GRADIENT && i > 0) {
            auto* btn_up = new wxButton(m_filament_rows_panel, wxID_ANY, wxString::FromUTF8("\xe2\x86\x91"),
                                        wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
            btn_up->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
                if (i < 1 || i >= (int)m_filament_rows.size()) return;
                int sel_a = m_filament_rows[i - 1].combo->GetSelection();
                int sel_b = m_filament_rows[i].combo->GetSelection();
                m_filament_rows[i - 1].combo->SetSelection(sel_b);
                m_filament_rows[i].combo->SetSelection(sel_a);
                wxColour ca = sel_b >= 0 && sel_b < (int)m_filament_colours.size()
                    ? parse_mixed_color(m_filament_colours[sel_b]) : wxColour(128,128,128);
                wxColour cb2 = sel_a >= 0 && sel_a < (int)m_filament_colours.size()
                    ? parse_mixed_color(m_filament_colours[sel_a]) : wxColour(128,128,128);
                m_filament_rows[i - 1].swatch->Bind(wxEVT_PAINT, [sw0 = m_filament_rows[i-1].swatch, ca](wxPaintEvent&) mutable {
                    wxAutoBufferedPaintDC dc(sw0); dc.SetBackground(wxBrush(ca)); dc.Clear();
                });
                m_filament_rows[i].swatch->Bind(wxEVT_PAINT, [sw1 = m_filament_rows[i].swatch, cb2](wxPaintEvent&) mutable {
                    wxAutoBufferedPaintDC dc(sw1); dc.SetBackground(wxBrush(cb2)); dc.Clear();
                });
                m_filament_rows[i - 1].swatch->Refresh();
                m_filament_rows[i].swatch->Refresh();
                update_preview();
            });
            row->Add(btn_up, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        }

        if (i == count - 1) {
            auto* btn_rm = new wxButton(m_filament_rows_panel, wxID_ANY, "-",
                                        wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
            auto* btn_ad = new wxButton(m_filament_rows_panel, wxID_ANY, "+",
                                        wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
            btn_rm->Enable(count > 2);
            btn_ad->Enable(count < max_filaments_for_mode(m_current_mode));

            btn_rm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
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
            btn_ad->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                sync_rows_to_result();
                resize_gradient_ids((int)m_filament_rows.size() + 1);
                wxTheApp->CallAfter([this]() {
                    rebuild_filament_rows();
                    Layout(); Fit();
                });
            });

            row->Add(btn_rm, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
            row->Add(btn_ad, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
        }

        m_filament_rows_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        m_filament_rows.push_back({ sw, cb });
    }

    m_filament_rows_panel->Layout();
    update_ratio_or_tri_visibility();
    update_preview();
}

void AddColorMixDialog::build_tri_picker()
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

    m_tri_picker->Bind(wxEVT_PAINT, [this, get_verts](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_tri_picker);
        wxSize sz = m_tri_picker->GetClientSize();
        auto [v0, v1, v2] = get_verts();

        dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
        dc.Clear();

        // get the 3 filament colours (rows 0,1,2)
        auto safe_col = [&](int row) -> wxColour {
            if (row >= (int)m_filament_rows.size()) return wxColour(128,128,128);
            int s = m_filament_rows[row].combo->GetSelection();
            s = std::max(0, std::min(s, (int)m_filament_colours.size()-1));
            return parse_mixed_color(m_filament_colours[s]);
        };
        wxColour c0 = safe_col(0), c1 = safe_col(1), c2 = safe_col(2);

        int min_y = (int)std::min({v0.y, v1.y, v2.y});
        int max_y = (int)std::max({v0.y, v1.y, v2.y});
        int min_x = (int)std::min({v0.x, v1.x, v2.x});
        int max_x = (int)std::max({v0.x, v1.x, v2.x});

        const int img_w = sz.GetWidth(), img_h = sz.GetHeight();
        wxImage img(img_w, img_h);
        const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        img.SetRGB(wxRect(0, 0, img_w, img_h), bg.Red(), bg.Green(), bg.Blue());
        unsigned char *data = img.GetData();

        wxPoint clip_pts[3] = {{(int)v0.x,(int)v0.y},{(int)v1.x,(int)v1.y},{(int)v2.x,(int)v2.y}};

        for (int py = min_y; py <= max_y; ++py) {
            for (int px = min_x; px <= max_x; ++px) {
                TriPt p = {(double)px, (double)py};
                double w0, w1, w2;
                tri_bary(p, v0, v1, v2, w0, w1, w2);
                if (w0 < -0.001 || w1 < -0.001 || w2 < -0.001) continue;
                if (px < 0 || px >= img_w || py < 0 || py >= img_h) continue;
                const int idx = (py * img_w + px) * 3;
                data[idx]     = (unsigned char)std::clamp((int)(c0.Red()  *w0 + c1.Red()  *w1 + c2.Red()  *w2), 0, 255);
                data[idx + 1] = (unsigned char)std::clamp((int)(c0.Green()*w0 + c1.Green()*w1 + c2.Green()*w2), 0, 255);
                data[idx + 2] = (unsigned char)std::clamp((int)(c0.Blue() *w0 + c1.Blue() *w1 + c2.Blue() *w2), 0, 255);
            }
        }

        wxBitmap bmp(img);
        wxMemoryDC mdc(bmp);

        // outline
        mdc.SetPen(wxPen(wxColour(180,180,180), 1));
        mdc.SetBrush(*wxTRANSPARENT_BRUSH);
        mdc.DrawPolygon(3, clip_pts);

        // drag handle
        double hx = m_tri_wx*v0.x + m_tri_wy*v1.x + m_tri_wz*v2.x;
        double hy = m_tri_wx*v0.y + m_tri_wy*v1.y + m_tri_wz*v2.y;
        mdc.SetBrush(*wxWHITE_BRUSH);
        mdc.SetPen(wxPen(wxColour(40,40,40), FromDIP(2)));
        mdc.DrawCircle((int)hx, (int)hy, FromDIP(5));

        mdc.SelectObject(wxNullBitmap);
        dc.DrawBitmap(bmp, 0, 0);

        // percentage labels at vertices
        dc.SetFont(wxFont(wxFontInfo(8)));
        dc.SetTextForeground(wxColour(60,60,60));
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

void AddColorMixDialog::update_ratio_or_tri_visibility()
{
    bool is_ratio_mode = (m_current_mode == MODE_RATIO);
    bool is_cycle_mode = (m_current_mode == MODE_CYCLE);
    bool is_match_mode = (m_current_mode == MODE_MATCH);
    int  n             = (int)m_filament_rows.size();
    bool show_slider   = is_ratio_mode && (n == 2);
    bool show_tri      = is_ratio_mode && (n == 3);

    if (m_filament_title)      m_filament_title->Show(!is_match_mode);
    if (m_filament_rows_panel) m_filament_rows_panel->Show(!is_match_mode);
    if (m_preview_panel)       m_preview_panel->Show(!is_match_mode);
    if (m_preview_label)       m_preview_label->Show(!is_match_mode);
    if (m_swatch_grid_panel)   m_swatch_grid_panel->Show(!is_match_mode);
    if (m_recommended_label)   m_recommended_label->Show(!is_match_mode);
    if (m_line_below_mid)      m_line_below_mid->Show(!is_match_mode);
    if (m_line_above_swatch)   m_line_above_swatch->Show(!is_match_mode);
    if (m_line_below_swatch)   m_line_below_swatch->Show(!is_match_mode);

    if (m_gradient_bar_sizer)  m_gradient_bar_sizer->ShowItems(show_slider && !is_match_mode);
    if (m_tri_picker_sizer)    m_tri_picker_sizer->ShowItems(show_tri && !is_match_mode);
    if (m_pattern_panel_sizer) m_pattern_panel_sizer->ShowItems(is_cycle_mode);
    if (m_match_panel_sizer)   m_match_panel_sizer->ShowItems(is_match_mode);
}

void AddColorMixDialog::resize_gradient_ids(int target_count)
{
    int extra = target_count - 2;
    if (extra <= 0) { m_result.gradient_component_ids.clear(); return; }
    std::string ids = m_result.gradient_component_ids;
    while ((int)ids.size() < extra)
        ids += char('1' + (int)m_result.component_b - 1);
    ids.resize((size_t)extra);
    m_result.gradient_component_ids = ids;
}

void AddColorMixDialog::sync_rows_to_result()
{
    if (m_filament_rows.empty()) return;
    int a = m_filament_rows[0].combo->GetSelection();
    int b = (m_filament_rows.size() > 1) ? m_filament_rows[1].combo->GetSelection() : a;
    m_result.component_a = (unsigned int)(std::max(0, a) + 1);
    m_result.component_b = (unsigned int)(std::max(0, b) + 1);
    std::string ids;
    for (int i = 2; i < (int)m_filament_rows.size(); ++i) {
        int s = m_filament_rows[i].combo->GetSelection();
        ids += char('1' + std::max(0, s));
    }
    m_result.gradient_component_ids = ids;
}

std::string AddColorMixDialog::compute_preview_color()
{
    if (m_filament_colours.empty() || m_filament_rows.empty()) return "#808080";

    int n   = (int)m_filament_rows.size();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;

    auto safe_idx = [&](int raw) {
        return std::max(0, std::min(raw, (int)m_filament_colours.size() - 1));
    };

    if (n == 2) {
        int ia = safe_idx(m_filament_rows[0].combo->GetSelection());
        int ib = safe_idx(m_filament_rows[1].combo->GetSelection());
        int w = (m_current_mode == MODE_RATIO) ? val : 50;
        return MixedFilamentManager::blend_color(
            m_filament_colours[ia], m_filament_colours[ib], 100 - w, w);
    }

    // 3-filament ratio mode: linear weighted average matching tri picker rendering
    if (n == 3 && m_current_mode == MODE_RATIO) {
        auto get_col = [&](int row) {
            return parse_mixed_color(m_filament_colours[safe_idx(m_filament_rows[row].combo->GetSelection())]);
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
        int idx = safe_idx(m_filament_rows[i].combo->GetSelection());
        cp.push_back(std::make_pair(m_filament_colours[idx], per + (i == 0 ? rem : 0)));
    }
    return MixedFilamentManager::blend_color_multi(cp);
}

void AddColorMixDialog::draw_strip(wxDC& dc, wxPanel* panel)
{
    wxSize sz = panel->GetClientSize();
    if (sz.x <= 0 || sz.y <= 0 || m_filament_rows.empty()) {
        dc.SetBackground(*wxGREY_BRUSH); dc.Clear(); return;
    }
    int n = (int)m_filament_rows.size();
    int seg = FromDIP(12), x = 0;

    // In 2-color ratio mode, build a pattern reflecting the A:B ratio
    std::vector<int> pattern;
    if (m_current_mode == MODE_RATIO && n == 2 && m_gradient_selector) {
        int val = m_gradient_selector->value();
        int steps_a = std::max(1, (100 - val) / 10);
        int steps_b = std::max(1, val / 10);
        for (int i = 0; i < steps_a; ++i) pattern.push_back(0);
        for (int i = 0; i < steps_b; ++i) pattern.push_back(1);
    } else if (m_current_mode == MODE_RATIO && n == 3) {
        int s0 = std::max(1, (int)(m_tri_wx * 10 + 0.5));
        int s1 = std::max(1, (int)(m_tri_wy * 10 + 0.5));
        int s2 = std::max(1, (int)(m_tri_wz * 10 + 0.5));
        for (int i = 0; i < s0; ++i) pattern.push_back(0);
        for (int i = 0; i < s1; ++i) pattern.push_back(1);
        for (int i = 0; i < s2; ++i) pattern.push_back(2);
    } else if (m_current_mode == MODE_CYCLE && m_pattern_ctrl) {
        // Mirror physical_filament_from_pattern_step():
        //   '1' → component_a (row 0), '2' → component_b (row 1),
        //   '3'..'9' → direct physical filament ID (1-based, not a row index).
        // Store as negative -(id) to distinguish direct IDs from row indices.
        std::string raw = into_u8(m_pattern_ctrl->GetValue());
        for (char c : raw) {
            if (c == ',' ) continue;
            if (c == '1' || c == 'A' || c == 'a') pattern.push_back(0);
            else if (c == '2' || c == 'B' || c == 'b') pattern.push_back(1);
            else if (c >= '3' && c <= '9') pattern.push_back(-(c - '0'));
        }
        if (pattern.empty())
            for (int i = 0; i < n; ++i) pattern.push_back(i);
    } else {
        for (int i = 0; i < n; ++i) pattern.push_back(i);
    }

    bool vertical = (m_current_mode == MODE_RATIO && n == 3);
    int fi = 0, pos = 0;
    int total = vertical ? sz.y : sz.x;
    while (pos < total) {
        int entry = pattern[fi % (int)pattern.size()];
        int idx;
        if (entry < 0) {
            idx = std::max(0, std::min(-entry - 1, (int)m_filament_colours.size() - 1));
        } else {
            int row = std::min(entry, n - 1);
            idx = m_filament_rows[row].combo->GetSelection();
            idx = std::max(0, std::min(idx, (int)m_filament_colours.size() - 1));
        }
        dc.SetBrush(wxBrush(parse_mixed_color(m_filament_colours[idx])));
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (vertical)
            dc.DrawRectangle(0, pos, sz.x, std::min(seg, sz.y - pos));
        else
            dc.DrawRectangle(pos, 0, std::min(seg, sz.x - pos), sz.y);
        pos += seg; ++fi;
    }
}

void AddColorMixDialog::build_swatch_grid()
{
    int n = (int)m_filament_colours.size();
    if (n < 2) return;

    std::vector<std::pair<int,int>> pairs;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            pairs.push_back(std::make_pair(i, j));

    int cols = std::min((int)pairs.size(), 8);
    if (cols == 0) return;
    auto* grid = new wxGridSizer(0, cols, FromDIP(4), FromDIP(4));

    for (int pi = 0; pi < (int)pairs.size(); ++pi) {
        int ia = pairs[pi].first;
        int ib = pairs[pi].second;
        std::string blended = MixedFilamentManager::blend_color(
            m_filament_colours[ia], m_filament_colours[ib], 50, 50);
        auto* btn = new wxBitmapButton(m_swatch_grid_panel, wxID_ANY,
                                       make_color_bitmap(parse_mixed_color(blended), FromDIP(20)),
                                       wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
        btn->SetToolTip(wxString::Format("F%d + F%d", ia + 1, ib + 1));
        btn->Bind(wxEVT_BUTTON, [this, ia, ib](wxCommandEvent&) {
            if (!m_filament_rows.empty()) {
                m_filament_rows[0].combo->SetSelection(ia);
                wxColour ca = parse_mixed_color(m_filament_colours[ia]);
                m_filament_rows[0].swatch->Bind(wxEVT_PAINT, [sw0 = m_filament_rows[0].swatch, ca](wxPaintEvent&) {
                    wxAutoBufferedPaintDC dc(sw0);
                    dc.SetBackground(wxBrush(ca)); dc.Clear();
                });
                m_filament_rows[0].swatch->Refresh();
            }
            if (m_filament_rows.size() > 1) {
                m_filament_rows[1].combo->SetSelection(ib);
                wxColour cb = parse_mixed_color(m_filament_colours[ib]);
                m_filament_rows[1].swatch->Bind(wxEVT_PAINT, [sw1 = m_filament_rows[1].swatch, cb](wxPaintEvent&) {
                    wxAutoBufferedPaintDC dc(sw1);
                    dc.SetBackground(wxBrush(cb)); dc.Clear();
                });
                m_filament_rows[1].swatch->Refresh();
            }
            update_preview();
        });
        grid->Add(btn);
    }

    m_swatch_grid_panel->SetSizer(grid);
    m_swatch_grid_panel->Layout();
}

void AddColorMixDialog::on_mode_changed(int mode_index)
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

void AddColorMixDialog::update_gradient_selector_colors()
{
    if (!m_gradient_selector || m_filament_rows.size() < 2) return;
    int ia = m_filament_rows[0].combo->GetSelection();
    int ib = m_filament_rows[1].combo->GetSelection();
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

void AddColorMixDialog::update_preview()
{
    if ((int)m_filament_rows.size() == 2)
        update_gradient_selector_colors();
    if (m_preview_panel)    m_preview_panel->Refresh();
    if (m_strip_panel)      m_strip_panel->Refresh();
    if (m_tri_strip_panel)  m_tri_strip_panel->Refresh();
    if (m_tri_picker)       m_tri_picker->Refresh();
}

void AddColorMixDialog::collect_result()
{
    sync_rows_to_result();
    int val = m_gradient_selector ? m_gradient_selector->value() : 50;
    m_result.mix_b_percent = val;
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
                    int s = m_filament_rows[i].combo->GetSelection();
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
        // gradient_component_ids already set by sync_rows_to_result(); keep it
        m_result.gradient_component_weights.clear();
        m_result.manual_pattern.clear();
        break;
    }
    m_result.custom = true;
}

void AddColorMixDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    m_preview_panel->SetMinSize(wxSize(FromDIP(PREVIEW_SIZE), FromDIP(PREVIEW_SIZE)));
    m_strip_panel->SetMinSize(wxSize(-1, FromDIP(STRIP_HEIGHT)));
    Layout(); Fit();
}

}} // namespace Slic3r::GUI
