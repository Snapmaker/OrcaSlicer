#pragma once

#include "GUI_Utils.hpp"
#include "MixedGradientSelector.hpp"
#include "MixedColorMatchPanel.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"

#include <wx/wx.h>
#include <wx/statline.h>
#include <wx/dcbuffer.h>

#include <vector>
#include <string>

#include "MixedColorMatchHelpers.hpp"

namespace Slic3r { namespace GUI {

class AddColorMixDialog : public DPIDialog
{
public:
    AddColorMixDialog(wxWindow* parent, const std::vector<std::string>& filament_colours);
    AddColorMixDialog(wxWindow* parent, const std::vector<std::string>& filament_colours,
                      const Slic3r::MixedFilament& existing);

    const Slic3r::MixedFilament& GetResult() const { return m_result; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void rebuild_filament_rows();
    void on_mode_changed(int mode_index);
    void update_preview();
    void update_gradient_selector_colors();
    void build_swatch_grid();
    void sync_rows_to_result();
    void resize_gradient_ids(int target_count);
    std::string compute_preview_color();
    wxBitmap make_color_bitmap(const wxColour& c, int size);
    int max_filaments_for_mode(int mode_index) const;
    void collect_result();
    void draw_strip(wxDC& dc, wxPanel* panel);

    static constexpr int MODE_RATIO    = 0;
    static constexpr int MODE_CYCLE    = 1;
    static constexpr int MODE_MATCH    = 2;
    static constexpr int MODE_GRADIENT = 3;

    RadioGroup*             m_mode_radio          = nullptr;
    wxStaticText*           m_filament_title      = nullptr;
    wxPanel*                m_filament_rows_panel = nullptr;
    wxBoxSizer*             m_filament_rows_sizer = nullptr;
    wxPanel*                m_preview_panel       = nullptr;
    wxStaticText*           m_preview_label       = nullptr;
    MixedGradientSelector*  m_gradient_selector   = nullptr;
    wxStaticText*           m_ratio_label_a       = nullptr;
    wxStaticText*           m_ratio_label_b       = nullptr;
    wxBoxSizer*             m_ratio_label_sizer   = nullptr;
    wxBoxSizer*             m_gradient_bar_sizer  = nullptr;
    wxPanel*                m_tri_picker          = nullptr;
    wxBoxSizer*             m_tri_picker_sizer    = nullptr;
    wxPanel*                m_strip_panel         = nullptr;
    wxPanel*                m_tri_strip_panel     = nullptr;
    wxPanel*                m_cycle_strip_panel   = nullptr;
    wxPanel*                m_swatch_grid_panel   = nullptr;
    wxStaticLine*           m_line_below_mid      = nullptr;
    wxStaticLine*           m_line_above_swatch   = nullptr;
    wxStaticLine*           m_line_below_swatch   = nullptr;
    wxStaticText*           m_recommended_label   = nullptr;
    Button*                 m_btn_cancel          = nullptr;
    Button*                 m_btn_confirm         = nullptr;
    wxTextCtrl*             m_pattern_ctrl        = nullptr;
    wxBoxSizer*             m_pattern_panel_sizer = nullptr;
    std::vector<wxButton*>  m_pattern_quick_buttons;

    // Match mode
    wxBoxSizer*             m_match_panel_sizer   = nullptr;
    MixedColorMatchPanel*   m_match_panel         = nullptr;

    double m_tri_wx{1.0/3.0}, m_tri_wy{1.0/3.0}, m_tri_wz{1.0/3.0};
    bool   m_tri_dragging{false};

    void build_tri_picker();
    void update_ratio_or_tri_visibility();

    struct FilamentRow {
        wxPanel*  swatch = nullptr;
        ComboBox* combo  = nullptr;
    };
    std::vector<FilamentRow> m_filament_rows;

    const std::vector<std::string>& m_filament_colours;
    Slic3r::MixedFilament            m_result;
    int                              m_current_mode = MODE_RATIO;
};

}} // namespace Slic3r::GUI
