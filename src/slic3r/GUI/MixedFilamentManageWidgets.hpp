#pragma once

// Manage presentation widgets ported from Hanno Witzleb (Xipit)'s FullSpectrum UI
// at main cc152a84dde. See doc/Mixed-Filament-Manage.md for source commits.
// Selection is owned by the dialog's staged recommendation catalog.
#include <wx/panel.h>
#include <wx/stattext.h>
#include <functional>
#include <vector>

namespace Slic3r::GUI {

struct ManageSwatch
{
    wxColour              color;
    std::vector<wxColour> gradient;
    wxString              label;
    wxString              tooltip;
    bool                  editable = true;
};

// Retains FullSpectrum's swatch drawing and spacing without a native window for
// every recipe. Off-screen rows are never painted and mark changes do not layout.
class BatchSwatchGrid : public wxPanel
{
public:
    BatchSwatchGrid(wxWindow* parent, std::vector<ManageSwatch> swatches, std::function<void(size_t)> on_toggled = {});
    void set_mark(size_t index, int mark);
    void rescale();

private:
    std::vector<ManageSwatch>   m_swatches;
    std::vector<int>            m_marks;
    std::function<void(size_t)> m_on_toggled;
    int                         m_columns = 1;
    int                         m_hovered = -1;
    int                         m_focused = -1;
    int                         m_pitch   = 32;
    int                         m_margin  = 2;
    bool                        update_columns(int width);
    wxRect                      visible_rect() const;
    wxRect                      tile_rect(size_t index) const;
    int                         hit_test(const wxPoint& point) const;
    void                        refresh_tile(int index);
    void                        update_hover(int index);
    void                        focus_tile(int index);
    void                        on_paint(wxPaintEvent& event);
    void                        on_key_down(wxKeyEvent& event);
    wxSize                      DoGetBestClientSize() const override;
};

class BatchCheckBox : public wxPanel
{
public:
    BatchCheckBox(wxWindow* parent, std::function<void()> on_clicked);
    void set_state(int state);
    int  get_state() const { return m_state; }
    void rescale();

private:
    std::function<void()> m_on_clicked;
    int                   m_state   = 0;
    bool                  m_hovered = false;
    void                  on_paint(wxPaintEvent& event);
    void                  on_enter(wxMouseEvent& event);
    void                  on_leave(wxMouseEvent& event);
    void                  on_left_up(wxMouseEvent& event);
};

class CollapsibleSubSection : public wxPanel
{
public:
    CollapsibleSubSection(wxWindow* parent, const wxString& title);
    wxPanel*                            get_body() const { return m_body; }
    void                                set_check_state(int state);
    int                                 get_check_state() const;
    void                                set_check_enabled(bool enabled);
    void                                set_info(int add_count, int del_count);
    void                                toggle();
    bool                                is_collapsed() const { return m_collapsed; }
    void                                rescale();
    std::function<void(bool collapsed)> on_toggle;
    std::function<void()>               on_check_clicked;

private:
    wxPanel*       m_header    = nullptr;
    wxPanel*       m_body      = nullptr;
    wxStaticText*  m_lbl_title = nullptr;
    wxStaticText*  m_lbl_info  = nullptr;
    BatchCheckBox* m_check_box = nullptr;
    bool           m_collapsed = false;
    void           build_ui(const wxString& title);
    void           on_header_click(wxMouseEvent& event);
    void           on_paint(wxPaintEvent& event);
};
} // namespace Slic3r::GUI
