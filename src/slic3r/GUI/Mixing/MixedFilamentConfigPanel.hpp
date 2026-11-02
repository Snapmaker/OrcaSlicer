#pragma once

#include <functional>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/colour.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/spinctrl.h>
#include <wx/button.h>

#include "../libslic3r/MixedFilament.hpp"
#include "../MixedGradientSelector.hpp"
#include "../GUI_App.hpp"

namespace Slic3r {
namespace GUI {
class MixedMixPreview;

class MixedFilamentConfigPanel : public wxPanel
{
public:
    using OnChangeFn = std::function<void(const MixedFilament &)>;

    MixedFilamentConfigPanel(wxWindow *parent,
                             size_t mixed_id,
                             const MixedFilament &mf,
                             size_t num_physical,
                             const std::vector<std::string> &physical_colors,
                             const std::vector<double> &nozzle_diameters,
                             const std::vector<wxColour> &palette,
                             const MixedFilamentPreviewSettings &preview_settings,
                             bool bias_mode_enabled,
                             OnChangeFn on_change = {});

    // Get the updated mixed filament data
    MixedFilament get_mixed_filament() const { return m_mf; }
    bool has_changes() const { return m_has_changes; }
    static int effective_local_z_preview_mix_b_percent(const MixedFilament &mf,
                                                       const MixedFilamentPreviewSettings &preview_settings);

private:
    void build_ui();
    void update_preview();
    void update_local_z_breakdown();
    void update_component_picker_visuals();

    size_t                          m_mixed_id;
    MixedFilament                   m_mf;
    size_t                          m_num_physical;
    std::vector<std::string>        m_physical_colors;
    std::vector<double>             m_nozzle_diameters;
    std::vector<wxColour>           m_palette;
    MixedFilamentPreviewSettings    m_preview_settings;
    bool                            m_bias_mode_enabled = false;
    bool                            m_has_changes = false;

    wxChoice                       *m_choice_a = nullptr;
    wxChoice                       *m_choice_b = nullptr;
    wxChoice                       *m_choice_c = nullptr;
    wxChoice                       *m_choice_d = nullptr;
    wxPanel                        *m_picker_a_container = nullptr;
    wxPanel                        *m_picker_b_container = nullptr;
    wxPanel                        *m_picker_c_container = nullptr;
    wxPanel                        *m_picker_d_container = nullptr;
    wxPanel                        *m_picker_a_swatch = nullptr;
    wxPanel                        *m_picker_b_swatch = nullptr;
    wxPanel                        *m_picker_c_swatch = nullptr;
    wxPanel                        *m_picker_d_swatch = nullptr;
    wxStaticText                   *m_picker_a_label = nullptr;
    wxStaticText                   *m_picker_b_label = nullptr;
    wxStaticText                   *m_picker_c_label = nullptr;
    wxStaticText                   *m_picker_d_label = nullptr;
    wxPanel                        *m_surface_offset_target_container = nullptr;
    wxPanel                        *m_surface_offset_target_swatch = nullptr;
    wxStaticText                   *m_surface_offset_target_label = nullptr;
    MixedGradientSelector          *m_blend_selector = nullptr;
    wxStaticText                   *m_blend_label = nullptr;
    wxTextCtrl                     *m_pattern_ctrl = nullptr;
    wxCheckBox                     *m_local_z_limit_checkbox = nullptr;
    wxSpinCtrl                     *m_local_z_limit_spin = nullptr;
    wxSpinCtrlDouble               *m_surface_offset_spin = nullptr;
    std::vector<wxButton*>          m_pattern_quick_buttons;
    MixedMixPreview                *m_mix_preview = nullptr;
    wxStaticText                   *m_breakdown_label = nullptr;
    wxPanel                        *m_swatch = nullptr;
    std::shared_ptr<std::vector<int>> m_selected_weight_state;
    OnChangeFn                       m_on_change;

    // Helper functions (copied from update_mixed_filament_panel)
    static std::vector<unsigned int> decode_gradient_ids(const std::string &s);
    static std::string encode_gradient_ids(const std::vector<unsigned int> &ids);
    static std::vector<unsigned int> decode_manual_pattern_ids(const std::string &pattern,
                                                               unsigned int       a,
                                                               unsigned int       b,
                                                               size_t             num_physical,
                                                               size_t             wall_loops = 0);
    static std::vector<int> decode_gradient_weights(const std::string &s, size_t n);
    static std::vector<int> normalize_gradient_weights(const std::vector<int> &w, size_t n);
    static std::string encode_gradient_weights(const std::vector<int> &w);
    static std::vector<unsigned int> build_weighted_pair_sequence(unsigned int a, unsigned int b, int percent_b, bool limit_cycle = false);
    static std::vector<unsigned int> build_weighted_multi_sequence(const std::vector<unsigned int> &ids,
                                                                   const std::vector<int> &weights,
                                                                   size_t max_cycle_limit = 0);
    static std::string summarize_sequence(const std::vector<unsigned int> &seq);
    static std::string summarize_local_z_breakdown(const MixedFilament &mf,
                                                   const std::vector<int> &weights,
                                                   const MixedFilamentPreviewSettings &preview_settings);
    static std::string blend_from_sequence(const std::vector<std::string> &colors, const std::vector<unsigned int> &seq, const std::string &fallback);
    static std::vector<double> build_local_z_preview_pass_heights(double nominal_layer_height,
                                                                  double lower_bound,
                                                                  double upper_bound,
                                                                  double preferred_a_height,
                                                                  double preferred_b_height,
                                                                  int mix_b_percent,
                                                                  int max_sublayers_limit);
};

class MixedMixPreview : public wxPanel
{
public:
    explicit MixedMixPreview(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(120), FromDIP(20)));
        Bind(wxEVT_PAINT, &MixedMixPreview::on_paint, this);
    }

    void set_data(const std::vector<wxColour> &palette,
                  const std::vector<unsigned int> &sequence,
                  bool same_layer_mode,
                  const std::vector<double> &surface_offsets_mm,
                  const wxColour &fallback,
                  const wxString &left_overlay,
                  const wxString &right_overlay)
    {
        m_palette    = palette;
        m_sequence   = sequence;
        m_same_layer = same_layer_mode;
        m_surface_offsets_mm = surface_offsets_mm;
        m_fallback   = fallback;
        m_left_overlay = left_overlay;
        m_right_overlay = right_overlay;
        Refresh();
    }

private:
    wxRect preview_rect() const
    {
        const int margin_x = FromDIP(1);
        const int margin_y = FromDIP(1);
        const wxSize sz = GetClientSize();
        return wxRect(margin_x, margin_y, std::max(1, sz.GetWidth() - margin_x * 2), std::max(1, sz.GetHeight() - margin_y * 2));
    }

    wxColour color_for_extruder(unsigned int extruder_id) const
    {
        if (extruder_id >= 1 && extruder_id <= m_palette.size())
            return m_palette[extruder_id - 1];
        return m_fallback;
    }

    double max_active_surface_offset_mm() const
    {
        double max_offset = 0.0;
        for (double offset_mm : m_surface_offsets_mm)
            max_offset = std::max(max_offset, std::abs(offset_mm));
        return std::max(0.001, max_offset);
    }

    int slot_inset_for_extruder(unsigned int extruder_id, int slot_extent) const
    {
        if (extruder_id == 0 || extruder_id >= m_surface_offsets_mm.size() || slot_extent <= 2)
            return 0;

        const double offset_mm = m_surface_offsets_mm[extruder_id];
        if (std::abs(offset_mm) <= EPSILON)
            return 0;

        const double normalized = std::clamp(std::abs(offset_mm) / max_active_surface_offset_mm(), 0.0, 1.0);
        const int inset = int(std::round(normalized * slot_extent * 0.45)) * (offset_mm < 0.0 ? -1 : 1);
        return std::clamp(inset, -std::max(0, slot_extent / 2), std::max(0, slot_extent / 2));
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();

        const wxRect rect = preview_rect();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_fallback));
        dc.DrawRectangle(rect);

        if (!m_sequence.empty()) {
            if (m_same_layer) {
                // Same-layer preview: full-height stripe lines.
                const int stripes = 24;
                const int stripe_w = std::max(1, rect.GetWidth() / stripes);
                const size_t seq_len = m_sequence.size();
                for (int s = 0; s < stripes; ++s) {
                    const size_t idx = size_t(s % int(seq_len));
                    const unsigned int extruder_id = m_sequence[idx];
                    dc.SetBrush(wxBrush(color_for_extruder(m_sequence[idx])));
                    const int x = rect.GetLeft() + s * stripe_w;
                    const int w = (s == stripes - 1) ? (rect.GetRight() - x + 1) : stripe_w;
                    const int inset = slot_inset_for_extruder(extruder_id, w);
                    wxRect draw_rect(x + inset / 2, rect.GetTop(), std::max(1, w - inset), rect.GetHeight());
                    draw_rect.Intersect(rect);
                    if (draw_rect.GetWidth() > 0)
                        dc.DrawRectangle(draw_rect);
                }
            } else {
                const int bars = 24;
                const int bar_w = std::max(1, rect.GetWidth() / bars);
                for (int i = 0; i < bars; ++i) {
                    size_t idx = 0;
                    if (m_sequence.size() > size_t(bars))
                        idx = (size_t(i) * m_sequence.size()) / size_t(bars);
                    else
                        idx = size_t(i) % m_sequence.size();
                    const unsigned int extruder_id = m_sequence[idx];
                    dc.SetBrush(wxBrush(color_for_extruder(extruder_id)));
                    const int x = rect.GetLeft() + i * bar_w;
                    const int w = (i == bars - 1) ? (rect.GetRight() - x + 1) : bar_w;
                    const int inset = slot_inset_for_extruder(extruder_id, w);
                    wxRect draw_rect(x + inset / 2, rect.GetTop(), std::max(1, w - inset), rect.GetHeight());
                    draw_rect.Intersect(rect);
                    if (draw_rect.GetWidth() > 0)
                        dc.DrawRectangle(draw_rect);
                }
            }
        }

        auto draw_outlined_text = [this, &dc](const wxString &text, int x, int y) {
            if (text.empty())
                return;
            dc.SetTextForeground(wxColour(255, 255, 255));
            const int outline_radius = std::max(2, FromDIP(2));
            for (int ox = -outline_radius; ox <= outline_radius; ++ox) {
                for (int oy = -outline_radius; oy <= outline_radius; ++oy) {
                    if (ox == 0 && oy == 0)
                        continue;
                    dc.DrawText(text, x + ox, y + oy);
                }
            }
            dc.SetTextForeground(wxColour(22, 22, 22));
            dc.DrawText(text, x, y);
        };

        wxCoord left_w = 0, left_h = 0;
        wxCoord right_w = 0, right_h = 0;
        dc.GetTextExtent(m_left_overlay, &left_w, &left_h);
        dc.GetTextExtent(m_right_overlay, &right_w, &right_h);
        const int text_y = rect.GetTop() + std::max(0, (rect.GetHeight() - int(std::max(left_h, right_h))) / 2);
        const int pad = FromDIP(6);
        if (!m_left_overlay.empty())
            draw_outlined_text(m_left_overlay, rect.GetLeft() + pad, text_y);
        if (!m_right_overlay.empty())
            draw_outlined_text(m_right_overlay, rect.GetRight() - pad - int(right_w), text_y);

        const bool is_dark = wxGetApp().dark_mode();
        dc.SetPen(wxPen(is_dark ? wxColour(110, 110, 110) : wxColour(170, 170, 170), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

private:
    std::vector<wxColour>       m_palette;
    std::vector<unsigned int>   m_sequence;
    std::vector<double>         m_surface_offsets_mm;
    bool                        m_same_layer { false };
    wxColour                    m_fallback { wxColour(38, 166, 154) };
    wxString                    m_left_overlay;
    wxString                    m_right_overlay;
};

std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(const std::string &pattern,
    unsigned int       component_a,
    unsigned int       component_b,
    size_t             num_physical,
    size_t             wall_loops);

void reduce_weight_counts_to_cycle_limit(std::vector<int> &counts, size_t cycle_limit);
float mixed_filament_single_surface_offset_value(const MixedFilament       &mf,
                                                 const std::vector<double> &nozzle_diameters);
std::pair<float, float> mixed_filament_single_surface_offset_pair(const MixedFilament &mf,
                                                                  float                value,
                                                                  const std::vector<double> &nozzle_diameters);


} // namespace GUI
} // namespace Slic3r
