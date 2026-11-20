#ifndef slic3r_GUI_Widgets_DpiAware_hpp_
#define slic3r_GUI_Widgets_DpiAware_hpp_

#include <cmath>
#include <deque>
#include <type_traits>

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/frame.h>

#include "UiAppearance.hpp"

// DPI-aware window mixin, extracted from GUI_Utils.hpp when the widget
// inspector moved to the standalone widget lab. The template routes its
// app-facing queries (dark restyle, dark-config refresh) through
// UiAppearance, so it links against the widgets library alone.

// Same token-for-token definition as slic3r/GUI/GUI_Utils.hpp:33; the guard
// makes the double definition order-independent when both headers end up in
// one translation unit.
#ifndef wxVERSION_EQUAL_OR_GREATER_THAN
#define wxVERSION_EQUAL_OR_GREATER_THAN(major, minor, release) ((wxMAJOR_VERSION > major) || ((wxMAJOR_VERSION == major) && (wxMINOR_VERSION > minor)) || ((wxMAJOR_VERSION == major) && (wxMINOR_VERSION == minor) && (wxRELEASE_NUMBER >= release)))
#endif // wxVERSION_EQUAL_OR_GREATER_THAN

namespace Slic3r {
namespace GUI {

enum { DPI_DEFAULT = 96 };

#if !wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
struct DpiChangedEvent : public wxEvent {
    int dpi;
    wxRect rect;

    DpiChangedEvent(wxEventType eventType, int dpi, wxRect rect)
        : wxEvent(0, eventType), dpi(dpi), rect(rect)
    {}

    virtual wxEvent *Clone() const
    {
        return new DpiChangedEvent(*this);
    }
};

wxDECLARE_EVENT(EVT_DPI_CHANGED_SLICER, DpiChangedEvent);
#endif // !wxVERSION_EQUAL_OR_GREATER_THAN

inline std::deque<wxDialog*> dialogStack;

// End every modal dialog on the stack with wxID_ABORT (Windows session end).
// Entries are raw pointers: validate against the live top-level window list
// first, wx may already have deleted a dialog under a running modal loop.
void abort_all_modal_dialogs();

// True while any modal wxDialog loop is running, in dialogStack or not.
// GUI thread only: walks wxTopLevelWindows, which is not thread-safe
// from other threads.
bool any_modal_dialog_showing();

template<class P> class DPIAware : public P
{
public:
    DPIAware(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos=wxDefaultPosition,
        const wxSize &size=wxDefaultSize, long style=wxDEFAULT_FRAME_STYLE, const wxString &name=wxFrameNameStr)
        : P(parent, id, title, pos, size, style, name)
    {
        int dpi = ui().dpi_for_window(this);
        m_scale_factor = (float)dpi / (float)DPI_DEFAULT;
        m_prev_scale_factor = m_scale_factor;
		m_normal_font = ui().default_font_for_dpi(this, dpi);

        /* Because of default window font is a primary display font,
         * We should set correct font for window before getting em_unit value.
         */
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
        this->SetFont(m_normal_font);
#endif
        this->CenterOnParent();
#ifdef _WIN32
        ui().update_dark_ui(this);
#endif

        // Linux specific issue : get_dpi_for_window(this) still doesn't responce to the Display's scale in new wxWidgets(3.1.3).
        // So, calculate the m_em_unit value from the font size, as before
#if !defined(__WXGTK__)
        m_em_unit = std::max<size_t>(10, 10.0f * m_scale_factor);
#else
        // initialize default width_unit according to the width of the one symbol ("m") of the currently active font of this window.
        m_em_unit = std::max<size_t>(10, this->GetTextExtent("m").x - 1);
#endif // __WXGTK__

//        recalc_font();

#ifndef __WXOSX__
#if wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
        this->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
	            m_scale_factor = (float)evt.GetNewDPI().x / (float)DPI_DEFAULT;
	            m_new_font_point_size = ui().default_font_for_dpi(this, evt.GetNewDPI().x).GetPointSize();
	            if (m_can_rescale && (m_force_rescale || is_new_scale_factor()))
	                rescale(wxRect());
            });
#else
        this->Bind(EVT_DPI_CHANGED_SLICER, [this](const DpiChangedEvent& evt) {
            m_scale_factor = (float)evt.dpi / (float)DPI_DEFAULT;

            m_new_font_point_size = ui().default_font_for_dpi(this, evt.dpi).GetPointSize();

            if (!m_can_rescale)
                return;

            if (m_force_rescale || is_new_scale_factor())
                rescale(evt.rect);
            });
#endif // wxVERSION_EQUAL_OR_GREATER_THAN
#endif // no __WXOSX__

        this->Bind(wxEVT_MOVE_START, [this](wxMoveEvent& event)
        {
            event.Skip();

            // Suppress application rescaling, when a MainFrame moving is not ended
            m_can_rescale = false;
        });

        this->Bind(wxEVT_MOVE_END, [this](wxMoveEvent& event)
        {
            event.Skip();

            m_can_rescale = is_new_scale_factor();

            // If scale factor is different after moving of MainFrame ...
            if (m_can_rescale)
                // ... rescale application
                rescale(event.GetRect());
            else
            // set value to _true_ in purpose of possibility of a display dpi changing from System Settings
                m_can_rescale = true;
        });

        this->Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& event)
        {
#ifndef __WINDOWS__
                ui().update_dark_config();
                on_sys_color_changed();
                event.Skip();
#endif // __WINDOWS__

        });

        if (std::is_same<wxDialog, P>::value) {
            this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
                if (e.GetKeyCode() == WXK_ESCAPE) {
                    //if (this->IsModal())
                    //    this->EndModal(wxID_CANCEL);
                    //else
                        this->Close();
                }
                else
                    e.Skip();
                });
        }
    }

    virtual ~DPIAware() {}

    float   scale_factor() const        { return m_scale_factor; }
    float   prev_scale_factor() const   { return m_prev_scale_factor; }

    int     em_unit() const             { return m_em_unit; }
//    int     font_size() const           { return m_font_size; }
    const wxFont& normal_font() const   { return m_normal_font; }
    void enable_force_rescale()         { m_force_rescale = true; }

#ifdef _WIN32
    void force_color_changed()
    {
        ui().update_dark_ui(this);
        on_sys_color_changed();
    }
#endif

    int ShowModal()
    {
        dialogStack.push_front(this);
        int r = wxDialog::ShowModal();
        dialogStack.pop_front();
        return r;
    }

protected:
    virtual void on_dpi_changed(const wxRect &suggested_rect) = 0;
    virtual void on_sys_color_changed() {};

private:
    float m_scale_factor;
    int m_em_unit;
//    int m_font_size;

    wxFont m_normal_font;
    float m_prev_scale_factor;
    bool  m_can_rescale{ true };
    bool m_force_rescale{ false };

    int   m_new_font_point_size;

//    void recalc_font()
//    {
//        wxClientDC dc(this);
//        const auto metrics = dc.GetFontMetrics();
//        m_font_size = metrics.height;
//         m_em_unit = metrics.averageWidth;
//    }

    // check if new scale is differ from previous
    bool    is_new_scale_factor() const { return fabs(m_scale_factor - m_prev_scale_factor) > 0.001; }

    // function for a font scaling of the window
    void    scale_win_font(wxWindow *window, const int font_point_size)
    {
        wxFont new_font(window->GetFont());
        new_font.SetPointSize(font_point_size);
        window->SetFont(new_font);
    }

    // recursive function for scaling fonts for all controls in Window
    void    scale_controls_fonts(wxWindow *window, const int font_point_size)
    {
        auto children = window->GetChildren();

        for (auto child : children) {
            scale_controls_fonts(child, font_point_size);
            scale_win_font(child, font_point_size);
        }

        window->Layout();
    }

    void    rescale(const wxRect &suggested_rect)
    {
        this->Freeze();

        m_force_rescale = false;
#if !wxVERSION_EQUAL_OR_GREATER_THAN(3,1,3)
        // rescale fonts of all controls
        scale_controls_fonts(this, m_new_font_point_size);
        // rescale current window font
        scale_win_font(this, m_new_font_point_size);
#endif // wxVERSION_EQUAL_OR_GREATER_THAN

        // set normal application font as a current window font
        m_normal_font = this->GetFont();

        // update em_unit value for new window font
        m_em_unit = std::max<int>(10, 10.0f * m_scale_factor);

        // rescale missed controls sizes and images
        on_dpi_changed(suggested_rect);

        this->Layout();
        this->Thaw();

        // reset previous scale factor from current scale factor value
        m_prev_scale_factor = m_scale_factor;
    }

#if 0 //#ifdef _WIN32  // #ysDarkMSW - Allow it when we deside to support the sustem colors for application
    bool HandleSettingChange(WXWPARAM wParam, WXLPARAM lParam) override
    {
        ui().update_dark_ui(this);
        on_sys_color_changed();

        // let the system handle it
        return false;
    }
#endif

};

typedef DPIAware<wxFrame> DPIFrame;
typedef DPIAware<wxDialog> DPIDialog;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_Widgets_DpiAware_hpp_
