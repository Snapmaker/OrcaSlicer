#include <algorithm>
#include <cmath>
#include <sstream>
#include <wx/display.h>
#include <wx/scrolwin.h>
#include "libslic3r/FlushVolCalc.hpp"
#include "WipeTowerDialog.hpp"
#include "BitmapCache.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "libslic3r/Color.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/DialogButtons.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "MainFrame.hpp"
#include "libslic3r/Config.hpp"
#include "Widgets/Label.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

int scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit() / 10; }
int ITEM_WIDTH() { return scale(30); }
static const wxColour g_text_color = wxColour(107, 107, 107, 255);

#undef  ICON_SIZE
#define ICON_SIZE               wxSize(FromDIP(16), FromDIP(16))
#define TABLE_BORDER            FromDIP(28)
#define HEADER_VERT_PADDING     FromDIP(12)
#define HEADER_BEG_PADDING      FromDIP(30)
#define HEADER_END_PADDING      FromDIP(24)
#define ROW_VERT_PADDING        FromDIP(6)
#define ROW_BEG_PADDING         FromDIP(20)
#define EDIT_BOXES_GAP          FromDIP(30)
#define ROW_END_PADDING         FromDIP(21)
#define BTN_SIZE                wxSize(FromDIP(58), FromDIP(24))
#define BTN_GAP                 FromDIP(20)
#define TEXT_BEG_PADDING        FromDIP(30)
#define MAX_FLUSH_VALUE         2147483520
// The dialog width used to be derived from the filament count; keep that value as the
// floor so the historical minimum width of the dialog is preserved.
#define MIN_WIPING_DIALOG_WIDTH_DIP 300
#define MIN_WIPING_DIALOG_WIDTH     FromDIP(MIN_WIPING_DIALOG_WIDTH_DIP)
// Minimum size of the scrolling viewport (DIP). The dialog may be shrunk down to this;
// below the fitted size the matrix is simply scrolled instead of being clipped.
#define MIN_WIPING_VIEW_WIDTH_DIP  240
#define MIN_WIPING_VIEW_HEIGHT_DIP 120
// Scroll step of the matrix viewport, in DIP. Re-applied on a DPI change (see
// WipingPanel::refit_in_dialog()), because the pixel value follows the scale factor.
#define SCROLL_RATE_DIP            10
#define TIP_MESSAGES_PADDING    FromDIP(8)



static void update_ui(wxWindow* window)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(window);
}

RammingDialog::RammingDialog(wxWindow* parent,const std::string& parameters)
: wxDialog(parent, wxID_ANY, _(L("Ramming customization")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE/* | wxRESIZE_BORDER*/)
{
    SetBackgroundColour(*wxWHITE);
    m_panel_ramming  = new RammingPanel(this,parameters);
    m_panel_ramming->Show(true);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_panel_ramming, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    main_sizer->Add(dlg_btns, 0, wxEXPAND);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { EndModal(wxCANCEL); });

    this->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        m_output_data = m_panel_ramming->get_parameters();
        EndModal(wxID_OK);
        },wxID_OK);

    wxGetApp().UpdateDlgDarkUI(this);
    this->Show();

    Slic3r::GUI::MessageDialog dlg(this, _(L("Ramming denotes the rapid extrusion just before a tool change in a single-extruder MM printer. Its purpose is to "
        "properly shape the end of the unloaded filament so it does not prevent insertion of the new filament and can itself "
        "be reinserted later. This phase is important and different materials can require different extrusion speeds to get "
        "the good shape. For this reason, the extrusion rates during ramming are adjustable.\n\nThis is an expert-level "
        "setting, incorrect adjustment will likely lead to jams, extruder wheel grinding into filament etc.")), _(L("Warning")), wxOK | wxICON_EXCLAMATION);// .ShowModal();
    dlg.ShowModal();
}


#ifdef _WIN32
#define style wxSP_ARROW_KEYS | wxBORDER_SIMPLE
#else 
#define style wxSP_ARROW_KEYS
#endif



RammingPanel::RammingPanel(wxWindow* parent, const std::string& parameters)
: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize/*,wxPoint(50,50), wxSize(800,350),wxBORDER_RAISED*/)
{
    SetBackgroundColour(*wxWHITE);
    update_ui(this);
	auto sizer_chart = new wxBoxSizer(wxVERTICAL);
	auto sizer_param = new wxBoxSizer(wxVERTICAL);

	std::stringstream stream{ parameters };
	stream >> m_ramming_line_width_multiplicator >> m_ramming_step_multiplicator;
	int ramming_speed_size = 0;
	float dummy = 0.f;
	while (stream >> dummy)
		++ramming_speed_size;
	stream.clear();
	stream.get();

	std::vector<std::pair<float, float>> buttons;
	float x = 0.f;
	float y = 0.f;
	while (stream >> x >> y)
		buttons.push_back(std::make_pair(x, y));

	m_chart = new Chart(this, wxRect(scale(10),scale(10),scale(480),scale(360)), buttons, ramming_speed_size, 0.25f, scale(10));
    update_ui(m_chart);
 	sizer_chart->Add(m_chart, 0, wxALL, 5);

    // Create help text for constant flow rate dragging
    std::string ctrl_str = GUI::shortkey_ctrl_prefix();
    if (!ctrl_str.empty() && ctrl_str.back() == '+') 
        ctrl_str.pop_back(); // Remove trailing '+'
    wxString message = format_wxstr(_L("For constant flow rate, hold %1% while dragging."), ctrl_str);
    Label* label = new Label(this, wxEmptyString);
    wxClientDC dc(label);
    wxString multiline_message;
    label->SetFont(Label::Body_14);
    label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    label->split_lines(dc, scale(470), message, multiline_message);
    label->SetLabel(multiline_message);
    sizer_chart->Add(label, 0, wxEXPAND | wxALL, 5);

    m_widget_time                             = new SpinInput(this, wxEmptyString, "ms" , wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 0 , 5000 , 3000, 250);
    m_widget_volume                           = new SpinInput(this, wxEmptyString, wxString::FromUTF8("mm³"), wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 0 , 10000, 0   );
    m_widget_ramming_line_width_multiplicator = new SpinInput(this, wxEmptyString, "%"  , wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 10, 300  , 100 );
    m_widget_ramming_step_multiplicator       = new SpinInput(this, wxEmptyString, "%"  , wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 10, 300  , 100 );

    auto add_title = [this, sizer_param](wxString label){
        auto title = new StaticLine(this, 0, label);
        title->SetFont(Label::Head_14);
        title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        sizer_param->Add(title, 0, wxEXPAND | wxBOTTOM, scale(8));
    };

    SetFont(Label::Body_14);
    wxSize col_size;
    for(auto label : {"Time", "Volume", "Width", "Spacing"})
        col_size.IncTo(GetTextExtent(_L(label)));
    col_size = wxSize(col_size.x + scale(30) ,-1);

    auto add_spin = [this, sizer_param, col_size](wxString label, SpinInput* spin){
        spin->Bind(wxEVT_KILL_FOCUS, [this](auto &e) {
            e.SetId(GetId());
            ProcessEventLocally(e);
            e.Skip();
        });
        auto h_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto text = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, col_size);
        text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        h_sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        h_sizer->Add(spin);
        sizer_param->Add(h_sizer, 0, wxEXPAND | wxBOTTOM, scale(2));
    };

    add_title(_L("Total ramming"));
    add_spin( _L("Time")  , m_widget_time  );
    add_spin( _L("Volume"), m_widget_volume);

    sizer_param->AddSpacer(10);

    add_title(_L("Ramming line"));
    add_spin( _L("Width")  , m_widget_ramming_line_width_multiplicator);
    add_spin( _L("Spacing"), m_widget_ramming_step_multiplicator      );

    m_widget_time->SetValue(int(m_chart->get_time() * 1000));
    m_widget_volume->SetValue(m_chart->get_volume());
    m_widget_volume->Disable();
    m_widget_ramming_line_width_multiplicator->SetValue(m_ramming_line_width_multiplicator);
    m_widget_ramming_step_multiplicator->SetValue(m_ramming_step_multiplicator);

    m_widget_ramming_step_multiplicator->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) { line_parameters_changed(); });
    m_widget_ramming_line_width_multiplicator->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) { line_parameters_changed(); });

	auto sizer = new wxBoxSizer(wxHORIZONTAL);
	sizer->Add(sizer_chart, 0, wxALL, 5);
	sizer->Add(sizer_param, 0, wxALL, 10);

	sizer->SetSizeHints(this);
	SetSizer(sizer);

    m_widget_time->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) {
        m_chart->set_xy_range(m_widget_time->GetValue() * 0.001,-1);
    });
    m_widget_time->Bind(wxEVT_CHAR,[](wxKeyEvent&){});      // do nothing - prevents the user to change the value
    m_widget_volume->Bind(wxEVT_CHAR,[](wxKeyEvent&){});    // do nothing - prevents the user to change the value   
    Bind(EVT_WIPE_TOWER_CHART_CHANGED,[this](wxCommandEvent&) {
        m_widget_volume->SetValue(m_chart->get_volume());
        m_widget_time->SetValue(m_chart->get_time() * 1000);
    });
    Refresh(true); // erase background
}

void RammingPanel::line_parameters_changed() {
    m_ramming_line_width_multiplicator = m_widget_ramming_line_width_multiplicator->GetValue();
    m_ramming_step_multiplicator = m_widget_ramming_step_multiplicator->GetValue();
}

std::string RammingPanel::get_parameters()
{
    std::vector<float> speeds = m_chart->get_ramming_speed(0.25f);
    std::vector<std::pair<float,float>> buttons = m_chart->get_buttons();
    std::stringstream stream;
    stream << m_ramming_line_width_multiplicator << " " << m_ramming_step_multiplicator;
    for (const float& speed_value : speeds)
        stream << " " << speed_value;
    stream << "|";    
    for (const auto& button : buttons)
        stream << " " << button.first << " " << button.second;
    return stream.str();
}


#ifdef _WIN32
#define style wxSP_ARROW_KEYS | wxBORDER_SIMPLE
#else 
#define style wxSP_ARROW_KEYS
#endif

static const float g_min_flush_multiplier = 0.f;
static const float g_max_flush_multiplier = 3.f;

wxBoxSizer* WipingDialog::create_btn_sizer(long flags)
{
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();

    if (flags & wxRESET) {
        Button *calc_btn = new Button(this, _L("Auto-Calc"));
        calc_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        calc_btn->SetFocus();
        calc_btn->SetId(wxID_RESET);
        btn_sizer->Add(calc_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
        m_button_list[wxRESET] = calc_btn;
    }
    if (flags & wxOK) {
        Button* ok_btn = new Button(this, _L("OK"));
        ok_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        ok_btn->SetFocus();
        ok_btn->SetId(wxID_OK);
        btn_sizer->Add(ok_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
        m_button_list[wxOK] = ok_btn;
    }
    if (flags & wxCANCEL) {
        Button* cancel_btn = new Button(this, _L("Cancel"));
        cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        cancel_btn->SetId(wxID_CANCEL);
        btn_sizer->Add(cancel_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::ChoiceButtonGap()));
        m_button_list[wxCANCEL] = cancel_btn;
    }

    return btn_sizer;

}

wxBoxSizer* WipingPanel::create_calc_btn_sizer(wxWindow* parent) {
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    Button* calc_btn = new Button(parent, _L("Re-calculate"));
    calc_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    calc_btn->SetFocus();
    btn_sizer->Add(calc_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(ButtonProps::WindowButtonGap()));
    calc_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { calc_flushing_volumes(); });

    return btn_sizer;
}

namespace {

/// \brief Work area (task bar excluded) of the display the given dialog is on.
///
/// \param dlg  The dialog whose display is queried. The dialog itself is used as the reference
///             window so that a dialog dragged to a secondary display is measured on that
///             display, not on the one its parent is on; the parent and then the main frame are
///             tried next, which is what the constructor call relies on (the dialog has no
///             native handle of its own yet at that point).
/// \return     The client area of that display, or an empty rectangle when it cannot be
///             resolved (headless / no display).
///
/// GUI thread only: it queries window hierarchy and display geometry.
wxRect dialog_work_area(const wxWindow* dlg)
{
    // Prefer the dialog itself: a dialog dragged to a secondary display must be measured on
    // that display, not on the one its parent is on. The parent (and then the main frame) are
    // the fallbacks for the constructor call, where the dialog has no native handle yet and
    // wxDisplay cannot resolve it.
    const wxWindow* candidates[] = {
        dlg,
        dlg != nullptr ? dlg->GetParent() : nullptr,
        wxGetApp().mainframe,
    };
    for (const wxWindow* ref : candidates) {
        if (ref == nullptr)
            continue;
        wxDisplay display(ref);
        if (display.IsOk())
            return display.GetClientArea();
    }
    return wxRect();
}

/// \brief Size the dialog so that the whole flushing matrix stays reachable.
///
/// The matrix holds one row and one column per filament, so its natural size grows with the
/// filament count. WipingDialog used to size itself to the content via SetSizeHints(), and
/// because wx clamps that size to the display, every row/column past the screen edge was
/// simply clipped: there was no scrollbar and the dialog was not resizable, so the rest of
/// the matrix could not be reached at all.
///
/// This keeps the dialog inside 4/5 of the display work area (the same ratio as
/// GUI::fit_in_display()) and leaves the overflow to the scrolling viewport that hosts the
/// panel: the panel's minimum size and the viewport's virtual size are both set to the
/// measured content size, and what does not fit becomes scrollable.
///
/// \param dlg       The dialog to size. It is resized as a side effect and its minimum client
///                  size is set to the fitted size, so it can be shrunk but not below chrome +
///                  the DIP floor.
/// \param viewport  The scrolling window that hosts \a content. Must be the parent of
///                  \a content: its sizer lays \a content out, and it is the window whose
///                  minimum size and virtual size are pinned.
/// \param content   The panel holding the matrix; its best size is the content size.
///
/// Call it again on a DPI change (WipingPanel::refit_in_dialog()). The content itself does not
/// change while the dialog is open, so no other caller is needed.
///
/// GUI thread only: it reads/sets window geometry and queries the display the dialog is on.
/// GUI::fit_in_display() is not reused here: it only resizes a top level window from a desired
/// size and knows nothing about the scrolling viewport that has to be resized instead, nor
/// about the minimum size the window keeps while it can be shrunk.
///
/// Note: do NOT call SetSizeHints() here. It pins the window's minimum client size to the
/// fitted size, and that size is computed from the content size, so the viewport would be
/// forced back to the full matrix size and the scrollbars would never appear.
void fit_wiping_dialog_to_display(wxDialog* dlg, wxScrolledWindow* viewport, WipingPanel* content)
{
    if (dlg == nullptr || viewport == nullptr || content == nullptr)
        return;

    wxSizer* sizer = dlg->GetSizer();
    if (sizer == nullptr)
        return;

    // Measure the matrix at its full content size. GetBestSize() is clamped to the window's
    // current minimum size (wxWindowBase::DoGetBestSize ends with IncTo(GetMinSize())), so any
    // minimum the panel already carries would leak into the measurement and make the content
    // size grow-only (it would not shrink back after a DPI decrease).
    //
    // Drop it first, and keep doing so unconditionally: where that minimum comes from is the
    // panel's business and may change with the wx version and with the order of the calls in
    // its constructor (in wx 3.1.5 the `m_sizer->SetSizeHints(this)` that runs before
    // `SetSizer(m_sizer)` takes `GetSizer()` internally and therefore sets nothing; a version
    // or a reordering that lets it see the sizer would pin the minimum to whatever page was
    // visible at construction). Resetting here is cheap and keeps this function correct either
    // way - do not remove it as "dead code".
    content->SetMinSize(wxDefaultSize);
    const wxSize content_size = content->GetBestSize(); // == min size of WipingPanel's sizer

    // Pin the panel's minimum to that content size: the viewport sizer lays the panel out at
    // this size, so the whole matrix is laid out and the viewport can scroll over it. Without
    // it the panel would be shrunk to the visible area and its rows would be clipped a second
    // time, where scrolling cannot reach them.
    content->SetMinSize(content_size);

    // Measure the dialog as if the whole matrix was visible at once: the viewport at full
    // content size plus the fixed chrome (separator + button row).
    viewport->SetMinSize(content_size);
    const wxSize full_size = sizer->GetMinSize();

    const wxRect work_area = dialog_work_area(dlg);
    wxSize     view_size   = content_size;
    if (work_area.GetWidth() > 0 && work_area.GetHeight() > 0) {
        const wxSize max_size(work_area.GetWidth() * 4 / 5, work_area.GetHeight() * 4 / 5);
        view_size.x = std::max(view_size.x - std::max(0, full_size.x - max_size.x), dlg->FromDIP(MIN_WIPING_VIEW_WIDTH_DIP));
        view_size.y = std::max(view_size.y - std::max(0, full_size.y - max_size.y), dlg->FromDIP(MIN_WIPING_VIEW_HEIGHT_DIP));
    }

    viewport->SetMinSize(view_size);
    viewport->SetVirtualSize(content_size); // same as FitInside(): the scrollable area

    // Initial size: fitted to the matrix, so a small matrix does not need any scrolling.
    wxSize fitted = sizer->GetMinSize();
    fitted.x = std::max(fitted.x, dlg->FromDIP(MIN_WIPING_DIALOG_WIDTH_DIP));

    // Minimum size: a small but usable viewport instead of the fitted size, so the window
    // can be shrunk as well; the scrollbars keep the whole matrix reachable either way.
    // Setting the minimum before the size mirrors wxSizer::SetSizeHints().
    viewport->SetMinSize(wxSize(std::min(view_size.x, dlg->FromDIP(MIN_WIPING_VIEW_WIDTH_DIP)),
                                std::min(view_size.y, dlg->FromDIP(MIN_WIPING_VIEW_HEIGHT_DIP))));
    wxSize min_client = sizer->GetMinSize();
    min_client.x = std::max(min_client.x, dlg->FromDIP(MIN_WIPING_DIALOG_WIDTH_DIP));

    dlg->SetMinClientSize(min_client);
    dlg->SetClientSize(fitted);
    dlg->Layout();
}

} // anonymous namespace

void WipingDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (auto button_item : m_button_list) 
    {
        if (button_item.first == wxRESET) 
        {
            button_item.second->Rescale();
        }
    }
    m_panel_wiping->msw_rescale();

    // Re-clamp the dialog for the display it is on now. Two things are frozen and two are not:
    //  - frozen: the panel's DIP sizes, evaluated once when it was constructed (ITEM_WIDTH() and
    //    the FromDIP constants are baked into the controls' minimum sizes), and msw_rescale()
    //    only swaps the icon bitmaps - so the re-measured content size keeps its width and can
    //    only differ in height (fonts);
    //  - recomputed: the display work area and the DIP floors (240x120 / 300 DIP).
    // The re-fit therefore re-applies the display area to the same content size.
    //
    // Trade-off: the fit sets the client size explicitly, so a size the user dragged the dialog
    // to is replaced by the fitted one. That is intentional - after a DPI change the fitted size
    // is what keeps the whole matrix reachable, and the user can resize again afterwards.
    m_panel_wiping->refit_in_dialog();
    this->Refresh();
};

// Parent dialog for purging volume adjustments - it fathers WipingPanel widget (that contains all controls) and a button to toggle simple/advanced mode:
WipingDialog::WipingDialog(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders, const std::vector<std::string>& extruder_colours,
    const std::vector<int>&extra_flush_volume, float flush_multiplier)
    : GUI::DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _(L("Flushing volumes for filament change")),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    this->SetBackgroundColour(*wxWHITE);
    // No SetMinSize() here: fit_wiping_dialog_to_display() sets the minimum client size, and it
    // already enforces MIN_WIPING_DIALOG_WIDTH_DIP; a second minimum would only drift from it.

    // The flushing matrix grows with the filament count (one row and one column per
    // filament), so host it in a scrolling viewport instead of letting the dialog grow
    // with the content: the dialog is kept inside the display work area below, and
    // whatever does not fit (the last rows and columns) stays reachable by scrolling.
    wxScrolledWindow* viewport = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    viewport->SetScrollRate(FromDIP(SCROLL_RATE_DIP), FromDIP(SCROLL_RATE_DIP));
    viewport->SetBackgroundColour(*wxWHITE);

    m_panel_wiping = new WipingPanel(viewport, matrix, extruders, extruder_colours, nullptr, extra_flush_volume, flush_multiplier);

    auto viewport_sizer = new wxBoxSizer(wxVERTICAL);
    // proportion 1: when the viewport is larger than the matrix, the panel still fills it
    // (the alternating row background covers the whole width, as before); when it is
    // smaller, the sizer keeps the panel at its minimum, i.e. at full content size.
    viewport_sizer->Add(m_panel_wiping, 1, wxEXPAND);
    viewport->SetSizer(viewport_sizer);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->Add(viewport, 1, wxEXPAND, 0);

    // The button row stays outside of the scrolling area, so it is always visible.
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    main_sizer->Add(dlg_btns, 0, wxEXPAND);
    SetSizer(main_sizer);
    fit_wiping_dialog_to_display(this, viewport, m_panel_wiping);
    this->CenterOnParent();

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {                 // if OK button is clicked..
        m_output_matrix = m_panel_wiping->read_matrix_values();    // ..query wiping panel and save returned values
        m_output_extruders = m_panel_wiping->read_extruders_values(); // so they can be recovered later by calling get_...()
        EndModal(wxID_OK);
    }, wxID_OK);

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxCANCEL); });

    /*
    if (this->FindWindowById(wxID_RESET, this)) {
        this->FindWindowById(wxID_RESET, this)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_panel_wiping->calc_flushing_volumes(); });
    }
    */

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { EndModal(wxCANCEL); });
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) {
            if (this->IsModal())
                this->EndModal(wxID_CANCEL);
            else 
                this->Close();
        }
        else
            e.Skip();
        });

    wxGetApp().UpdateDlgDarkUI(this);
}

void WipingPanel::create_panels(wxWindow* parent, const int num) {
    for (size_t i = 0; i < num; i++)
    {
        wxPanel* panel = new wxPanel(parent);
        panel->SetBackgroundColour(i % 2 == 0 ? *wxWHITE : wxColour(238, 238, 238));
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        panel->SetSizer(sizer);

        wxButton* icon = new wxButton(panel, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
        icon->SetBitmap(*get_extruder_color_icon(m_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16)));
        icon->SetCanFocus(false);
        icon_list2.push_back(icon);

        sizer->AddSpacer(ROW_BEG_PADDING);
        sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, ROW_VERT_PADDING);

        for (int j = 0; j < num; ++j) {
            edit_boxes[j][i]->Reparent(panel);
            edit_boxes[j][i]->SetBackgroundColour(panel->GetBackgroundColour());
            edit_boxes[j][i]->SetFont(::Label::Body_13);
            sizer->AddSpacer(EDIT_BOXES_GAP);
            sizer->Add(edit_boxes[j][i], 0, wxALIGN_CENTER_VERTICAL, 0);
        }
        sizer->AddSpacer(ROW_END_PADDING);

        m_sizer_advanced->Add(panel, 0, wxRIGHT | wxLEFT | wxEXPAND, TABLE_BORDER);
        panel->Layout();
    }
}

// This panel contains all control widgets for both simple and advanced mode (these reside in separate sizers)
WipingPanel::WipingPanel(wxWindow* parent, const std::vector<float>& matrix, const std::vector<float>& extruders, const std::vector<std::string>& extruder_colours, Button* calc_button,
    const std::vector<int>& extra_flush_volume, float flush_multiplier)
: wxPanel(parent,wxID_ANY, wxDefaultPosition, wxDefaultSize/*,wxBORDER_RAISED*/)
,m_matrix(matrix), m_min_flush_volume(extra_flush_volume), m_max_flush_volume(Slic3r::g_max_flush_volume)
{
    m_number_of_extruders = (int)(sqrt(matrix.size())+0.001);

    // Contract: one minimum flush volume per filament. `extra_flush_volume` comes from
    // Plater's get_min_flush_volumes(full_config), which sizes it by filament_diameter, and
    // PresetBundle::update_multi_material_filament_presets() keeps the matrix at n x n, so the
    // dialog never derives a dimension from data the caller does not have. No assert on
    // purpose: a project whose two vectors disagree would turn a debug build into an abort on
    // load, instead of opening the dialog.

    for (const std::string& color : extruder_colours) {
        Slic3r::ColorRGB rgb;
        Slic3r::decode_color(color, rgb);
        m_colours.push_back(wxColor(rgb.r_uchar(), rgb.g_uchar(), rgb.b_uchar()));
    }
    auto sizer_width = (int)((sqrt(matrix.size())) * ITEM_WIDTH() + (sqrt(matrix.size()) + 1) * HEADER_BEG_PADDING);
    sizer_width = sizer_width > MIN_WIPING_DIALOG_WIDTH ? sizer_width : MIN_WIPING_DIALOG_WIDTH;
    // Create two switched panels with their own sizers
    m_sizer_simple          = new wxBoxSizer(wxVERTICAL);
    m_sizer_advanced        = new wxBoxSizer(wxVERTICAL);
    m_page_simple			= new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_advanced			= new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_simple->SetSizer(m_sizer_simple);
    m_page_advanced->SetSizer(m_sizer_advanced);
    m_page_advanced->SetBackgroundColour(*wxWHITE);

    update_ui(m_page_simple);
    update_ui(m_page_advanced);

    auto gridsizer_simple   = new wxGridSizer(3, 5, 10);
    m_gridsizer_advanced = new wxGridSizer(m_number_of_extruders + 1, 5, 1);

    // First create controls for advanced mode and assign them to m_page_advanced:
    for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
        edit_boxes.push_back(std::vector<wxTextCtrl*>(0));

        for (unsigned int j = 0; j < m_number_of_extruders; ++j) {
#ifdef _WIN32
            wxTextCtrl* text = new wxTextCtrl(m_page_advanced, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1), wxTE_CENTER | wxBORDER_NONE | wxTE_PROCESS_ENTER);
            update_ui(text);
            edit_boxes.back().push_back(text);
#else
            edit_boxes.back().push_back(new wxTextCtrl(m_page_advanced, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1)));
#endif
            if (i == j) {
                edit_boxes[i][j]->SetValue(wxString("0"));
                edit_boxes[i][j]->SetEditable(false);
                edit_boxes[i][j]->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&) {});
                edit_boxes[i][j]->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent&) {});
            }
            else {
                edit_boxes[i][j]->SetValue(wxString("") << int(m_matrix[m_number_of_extruders * j + i] * flush_multiplier));

                edit_boxes[i][j]->Bind(wxEVT_TEXT, [this, i, j](wxCommandEvent& e) {
                    wxString str = edit_boxes[i][j]->GetValue();
                    int value = wxAtoi(str);
                    if (value > MAX_FLUSH_VALUE) {
                        str = wxString::Format(("%d"), MAX_FLUSH_VALUE);
                        edit_boxes[i][j]->SetValue(str);
                    }
                    else if (value < 0) {
                        edit_boxes[i][j]->SetValue(wxString("0"));
                    }
                    });

                auto on_apply_text_modify = [this, i, j](wxEvent &e) {
                    wxString str   = edit_boxes[i][j]->GetValue();
                    int      value = wxAtoi(str);
                    m_matrix[m_number_of_extruders * j + i] = value / get_flush_multiplier();
                    this->update_warning_texts();
                    e.Skip();
                };

                edit_boxes[i][j]->Bind(wxEVT_TEXT_ENTER, on_apply_text_modify);
                edit_boxes[i][j]->Bind(wxEVT_KILL_FOCUS, on_apply_text_modify);
            }
        }
    }

    // BBS
    m_sizer_advanced->AddSpacer(FromDIP(10));
    auto tip_message_panel = new wxPanel(m_page_advanced, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    tip_message_panel->SetBackgroundColour(wxColour(238, 238, 238));
    auto message_sizer = new wxBoxSizer(wxVERTICAL);
    tip_message_panel->SetSizer(message_sizer);
    {
        wxString message = _L("Orca would re-calculate your flushing volumes every time the filaments color changed. You could disable the auto-calculate in Snapmaker Orca > Preferences");
        m_tip_message_label = new Label(tip_message_panel, wxEmptyString);
        wxClientDC dc(tip_message_panel);
        wxString multiline_message;
        m_tip_message_label->split_lines(dc, sizer_width, message, multiline_message);
        m_tip_message_label->SetLabel(multiline_message);
        m_tip_message_label->SetFont(Label::Body_13);
        message_sizer->Add(m_tip_message_label, 0, wxEXPAND | wxALL, TIP_MESSAGES_PADDING);
    }
    m_sizer_advanced->Add(tip_message_panel, 0, wxEXPAND | wxRIGHT | wxLEFT, TABLE_BORDER);
    bool is_show = wxGetApp().app_config->get("auto_calculate") == "true" || wxGetApp().app_config->get("auto_calculate_when_filament_change") == "true";
    tip_message_panel->Show(is_show);
    m_sizer_advanced->AddSpacer(FromDIP(10));
    auto calc_btn_sizer = create_calc_btn_sizer(m_page_advanced);
    m_sizer_advanced->Add(calc_btn_sizer, 0, wxEXPAND | wxLEFT, FromDIP(30));
    
    //m_sizer_advanced->AddSpacer(FromDIP(10));
    m_sizer_advanced->AddSpacer(FromDIP(5));
    header_line_panel = new wxPanel(m_page_advanced, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    header_line_panel->SetBackgroundColour(wxColour(238, 238, 238));
    auto header_line_sizer = new wxBoxSizer(wxHORIZONTAL);
    header_line_panel->SetSizer(header_line_sizer);

    // The header row must be built from exactly the same per-column geometry as the matrix
    // rows (spacer EDIT_BOXES_GAP + a cell ITEM_WIDTH wide), and the same leading offset as
    // a row's first cell (ROW_BEG_PADDING + icon width). The two layouts used to compute the
    // column pitch differently - a 44 DIP gap plus the 16 DIP icon here vs EDIT_BOXES_GAP
    // plus ITEM_WIDTH there - which is the same 60 DIP nominally but rounds differently at
    // fractional DPI scaling: the header drifted 1 px per column, so by the time you scrolled
    // to the 40th column the icons were ~40 px away from the values underneath them.
    header_line_sizer->AddSpacer(ROW_BEG_PADDING + ICON_SIZE.x);
    for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
        auto* cell = new wxPanel(header_line_panel, wxID_ANY, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1));
        cell->SetBackgroundColour(header_line_panel->GetBackgroundColour());

        wxButton* icon = new wxButton(cell, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
        icon->SetBitmap(*get_extruder_color_icon(m_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16)));
        icon->SetCanFocus(false);
        icon_list1.push_back(icon);

        auto* cell_sizer = new wxBoxSizer(wxHORIZONTAL);
        cell_sizer->AddStretchSpacer();
        cell_sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL);
        cell_sizer->AddStretchSpacer();
        cell->SetSizer(cell_sizer);
        // The sizer's minimum is just the icon width (the stretch spacers can shrink to 0), and
        // this cell is added with proportion 0, so without a minimum the header cell would be
        // ICON_SIZE.x wide while the data cells below it are ITEM_WIDTH() wide - the very
        // column-pitch mismatch this change is meant to remove.
        cell->SetMinSize(wxSize(ITEM_WIDTH(), -1));

        header_line_sizer->AddSpacer(EDIT_BOXES_GAP);
        header_line_sizer->Add(cell, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, HEADER_VERT_PADDING);
    }
    header_line_sizer->AddSpacer(HEADER_END_PADDING);
    
    m_sizer_advanced->Add(header_line_panel, 0, wxEXPAND | wxRIGHT | wxLEFT, TABLE_BORDER);
    
    create_panels(m_page_advanced, m_number_of_extruders);

    //m_sizer_advanced->AddSpacer(BTN_SIZE.y);

    // BBS: for tunning flush volumes
    {
        auto multi_desc_label = new wxStaticText(m_page_advanced, wxID_ANY, _(L("Flushing volume (mm³) for each filament pair.")), wxDefaultPosition, wxDefaultSize, 0);
        multi_desc_label->SetForegroundColour(g_text_color);
        m_sizer_advanced->Add(multi_desc_label, 0, wxEXPAND | wxLEFT, TEXT_BEG_PADDING);

        wxString min_flush_str = wxString::Format(_L("Suggestion: Flushing Volume in range [%d, %d]"),*std::min_element(m_min_flush_volume.begin(), m_min_flush_volume.end()), m_max_flush_volume);
        m_min_flush_label = new wxStaticText(m_page_advanced, wxID_ANY, min_flush_str, wxDefaultPosition, wxDefaultSize, 0);
        m_min_flush_label->SetForegroundColour(g_text_color);
        m_sizer_advanced->Add(m_min_flush_label, 0, wxEXPAND | wxLEFT, TEXT_BEG_PADDING);

        auto on_apply_text_modify = [this](wxEvent& e) {
            wxString str = m_flush_multiplier_ebox->GetValue();
            float      multiplier = wxAtof(str);
            if (multiplier < g_min_flush_multiplier || multiplier > g_max_flush_multiplier) {
                str = wxString::Format(("%.2f"), multiplier < g_min_flush_multiplier ? g_min_flush_multiplier : g_max_flush_multiplier);
                m_flush_multiplier_ebox->SetValue(str);
                MessageDialog dlg(nullptr,
                    wxString::Format(_L("The multiplier should be in range [%.2f, %.2f]."), g_min_flush_multiplier, g_max_flush_multiplier),
                    _L("Warning"), wxICON_WARNING | wxOK);
                dlg.ShowModal();
            }
            for (unsigned int i = 0; i < m_number_of_extruders; ++i) {
                for (unsigned int j = 0; j < m_number_of_extruders; ++j) {
                    edit_boxes[i][j]->SetValue(to_string(int(m_matrix[m_number_of_extruders * j + i] * multiplier)));
                }
            }

            this->update_warning_texts();
            e.Skip();
        };

        m_sizer_advanced->AddSpacer(10);

        wxBoxSizer* param_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText* flush_multiplier_title = new wxStaticText(m_page_advanced, wxID_ANY, _L("Multiplier"));
        param_sizer->Add(flush_multiplier_title, 0, wxALIGN_CENTER | wxALL, 0);
        param_sizer->AddSpacer(FromDIP(5));
        m_flush_multiplier_ebox = new wxTextCtrl(m_page_advanced, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(50), -1), wxTE_PROCESS_ENTER);
        m_flush_multiplier_ebox->SetValue(wxString::Format(("%.2f"), flush_multiplier));
        m_flush_multiplier_ebox->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        param_sizer->Add(m_flush_multiplier_ebox, 0, wxALIGN_CENTER | wxALL, 0);
        param_sizer->AddStretchSpacer(1);
        m_sizer_advanced->Add(param_sizer, 0, wxEXPAND | wxLEFT, TEXT_BEG_PADDING);

        m_flush_multiplier_ebox->Bind(wxEVT_TEXT_ENTER, on_apply_text_modify);
        m_flush_multiplier_ebox->Bind(wxEVT_KILL_FOCUS, on_apply_text_modify);
        m_flush_multiplier_ebox->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this](wxCommandEvent&) {
            wxString str = m_flush_multiplier_ebox->GetValue();
            float multiplier = wxAtof(str);
            if (multiplier < g_min_flush_multiplier || multiplier > g_max_flush_multiplier) {
                str = wxString::Format(("%.2f"), multiplier < g_min_flush_multiplier ? g_min_flush_multiplier : g_max_flush_multiplier);
                m_flush_multiplier_ebox->SetValue(str);
            }
            m_flush_multiplier_ebox->SetInsertionPointEnd();
        });
    }
    this->update_warning_texts();

    m_page_advanced->Hide(); 

    // Now the same for simple mode:
    gridsizer_simple->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString("")), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
    gridsizer_simple->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString(_(L("unloaded")))), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);
    gridsizer_simple->Add(new wxStaticText(m_page_simple,wxID_ANY,wxString(_(L("loaded")))), 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL);

    auto add_spin_ctrl = [this](std::vector<wxSpinCtrl*>& vec, float initial)
    {
        wxSpinCtrl* spin_ctrl = new wxSpinCtrl(m_page_simple, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(ITEM_WIDTH(), -1), style | wxALIGN_RIGHT, 0, 300, (int)initial);
        update_ui(spin_ctrl);
        vec.push_back(spin_ctrl);

#ifdef __WXOSX__
        // On OSX / Cocoa, wxSpinCtrl::GetValue() doesn't return the new value
        // when it was changed from the text control, so the on_change callback
        // gets the old one, and on_kill_focus resets the control to the old value.
        // As a workaround, we get the new value from $event->GetString and store
        // here temporarily so that we can return it from get_value()
        spin_ctrl->Bind(wxEVT_TEXT, ([spin_ctrl](wxCommandEvent e)
        {
            long value;
            const bool parsed = e.GetString().ToLong(&value);
            int tmp_value = parsed && value >= INT_MIN && value <= INT_MAX ? (int)value : INT_MIN;

            // Forcibly set the input value for SpinControl, since the value 
            // inserted from the keyboard or clipboard is not updated under OSX
            if (tmp_value != INT_MIN) {
                spin_ctrl->SetValue(tmp_value);

                // But in SetValue() is executed m_text_ctrl->SelectAll(), so
                // discard this selection and set insertion point to the end of string
                spin_ctrl->GetText()->SetInsertionPointEnd();
            }
        }), spin_ctrl->GetId());
#endif
    };

    for (unsigned int i=0;i<m_number_of_extruders;++i) {
        add_spin_ctrl(m_old, extruders[2 * i]);
        add_spin_ctrl(m_new, extruders[2 * i+1]);

        auto hsizer = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* w = new wxWindow(m_page_simple, wxID_ANY, wxDefaultPosition, ICON_SIZE, wxBORDER_SIMPLE);
        w->SetCanFocus(false);
        w->SetBackgroundColour(m_colours[i]);
        hsizer->Add(w, wxALIGN_CENTER_VERTICAL);
        hsizer->AddSpacer(10);
        hsizer->Add(new wxStaticText(m_page_simple, wxID_ANY, wxString(_(L("Filament #"))) << i + 1 << ": "), 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);

        gridsizer_simple->Add(hsizer, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        gridsizer_simple->Add(m_old.back(),0);
        gridsizer_simple->Add(m_new.back(),0);
    }

    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(m_page_simple, 0, wxEXPAND, 0);
    m_sizer->Add(m_page_advanced, 0, wxEXPAND, 0);

    m_sizer->SetSizeHints(this);
    SetSizer(m_sizer);
    this->Layout();

    toggle_advanced(); // to show/hide what is appropriate
    
    header_line_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(header_line_panel);
        wxString from_text = _L("From");
        wxString to_text = _L("To");
        wxSize from_text_size = dc.GetTextExtent(from_text);
        wxSize to_text_size = dc.GetTextExtent(to_text);

        int base_y = (header_line_panel->GetSize().y - from_text_size.y - to_text_size.y) / 2;
        int vol_width = ROW_BEG_PADDING + EDIT_BOXES_GAP / 2 + ICON_SIZE.x;
        int base_x = (vol_width - from_text_size.x - to_text_size.x) / 2;

        // draw from text
        int x = base_x;
        int y = base_y + to_text_size.y;
        dc.DrawText(from_text, x, y);

        // draw to text
        x = base_x + from_text_size.x;
        y = base_y;
        dc.DrawText(to_text, x, y);

        // draw a line
        int p1_x = base_x + from_text_size.x - to_text_size.y;
        int p1_y = base_y;
        int p2_x = base_x + from_text_size.x + from_text_size.y;
        int p2_y = base_y + from_text_size.y + to_text_size.y;
        dc.SetPen(wxPen(wxColour(172, 172, 172, 1)));
        dc.DrawLine(p1_x, p1_y, p2_x, p2_y);
    });
}

int WipingPanel::calc_flushing_volume(const wxColour& from_, const wxColour& to_ ,int min_flush_volume)
{
    Slic3r::FlushVolCalculator calculator(min_flush_volume, m_max_flush_volume);

    return calculator.calc_flush_vol(from_.Alpha(), from_.Red(), from_.Green(), from_.Blue(), to_.Alpha(), to_.Red(), to_.Green(), to_.Blue());
}

void WipingPanel::update_warning_texts()
{
    static const wxColour g_warning_color = *wxRED;
    static const wxColour g_normal_color = *wxBLACK;

    wxString multi_str = m_flush_multiplier_ebox->GetValue();
    float multiplier = wxAtof(multi_str);

    bool has_exception_flush = false;
    for (int i = 0; i < edit_boxes.size(); i++) {
        auto& box_vec = edit_boxes[i];
        for (int j = 0; j < box_vec.size(); j++) {
            if (i == j)
                continue;

            auto text_box = box_vec[j];
            wxString str = text_box->GetValue();
            int actual_volume = wxAtoi(str);
            if (actual_volume < m_min_flush_volume[i] || actual_volume > m_max_flush_volume) {
                if (text_box->GetForegroundColour() != g_warning_color) {
                    text_box->SetForegroundColour(g_warning_color);
                    text_box->Refresh();
                }
                has_exception_flush = true;
            }
            else {
                if (text_box->GetForegroundColour() != g_normal_color) {
                    text_box->SetForegroundColour(StateColor::darkModeColorFor(g_normal_color));
                    text_box->Refresh();
                }
            }
        }
    }

    if (has_exception_flush && m_min_flush_label->GetForegroundColour() != g_warning_color) {
        m_min_flush_label->SetForegroundColour(g_warning_color);
        m_min_flush_label->Refresh();
    }
    else if (!has_exception_flush && m_min_flush_label->GetForegroundColour() != g_text_color) {
        m_min_flush_label->SetForegroundColour(g_text_color);
        m_min_flush_label->Refresh();
    }
}

void WipingPanel::calc_flushing_volumes()
{
    auto& ams_multi_color_filament = wxGetApp().preset_bundle->ams_multi_color_filment;
    std::vector<std::vector<wxColour>> multi_colors;

    // Support for multi-color filament
    for (int i = 0; i < m_colours.size(); ++i) {
        std::vector<wxColour> single_filament;
        if (i < ams_multi_color_filament.size()) {
            if (!ams_multi_color_filament[i].empty()) {
                std::vector<std::string> colors = ams_multi_color_filament[i];
                for (int j = 0; j < colors.size(); ++j) {
                    single_filament.push_back(wxColour(colors[j]));
                }
                multi_colors.push_back(single_filament);
                continue;
            }
        }
        single_filament.push_back(wxColour(m_colours[i]));
        multi_colors.push_back(single_filament);
    }

    for (int from_idx = 0; from_idx < multi_colors.size(); ++from_idx) {
        bool is_from_support = is_support_filament(from_idx);
        for (int to_idx = 0; to_idx < multi_colors.size(); ++to_idx) {
            bool is_to_support = is_support_filament(to_idx);
            if (from_idx == to_idx) {
                edit_boxes[to_idx][from_idx]->SetValue(std::to_string(0));
            }
            else {
                int flushing_volume = 0;
                if (is_to_support) {
                    flushing_volume = Slic3r::g_flush_volume_to_support;
                }
                else {
                    for (int i = 0; i < multi_colors[from_idx].size(); ++i) {
                        const wxColour& from = multi_colors[from_idx][i];
                        for (int j = 0; j < multi_colors[to_idx].size(); ++j) {
                            const wxColour& to = multi_colors[to_idx][j];
                            int volume = calc_flushing_volume(from, to, m_min_flush_volume[from_idx]);
                            flushing_volume = std::max(flushing_volume, volume);
                        }
                    }

                    if (is_from_support) {
                        flushing_volume = std::max(Slic3r::g_min_flush_volume_from_support, flushing_volume);
                    }
                }

                m_matrix[m_number_of_extruders * from_idx + to_idx] = flushing_volume;
                flushing_volume = int(flushing_volume * get_flush_multiplier());
                edit_boxes[to_idx][from_idx]->SetValue(std::to_string(flushing_volume));
            }
        }
    }

    this->update_warning_texts();
}

void WipingPanel::msw_rescale()
{
    for (unsigned int i = 0; i < icon_list1.size(); ++i) {
        auto bitmap = *get_extruder_color_icon(m_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16));
        icon_list1[i]->SetBitmap(bitmap);
        icon_list2[i]->SetBitmap(bitmap);
    }
}

// Reads values from the (advanced) wiping matrix:
std::vector<float> WipingPanel::read_matrix_values() {
    // No fill_in_matrix() call here: it is only reachable from toggle_advanced(true), which
    // nothing calls - the constructor's toggle_advanced() takes no user_action, so m_advanced is
    // always true and the removed "if (!m_advanced)" branch never ran.
    float flush_multipler = get_flush_multiplier();

    std::vector<float> output;
    for (unsigned int i=0;i<m_number_of_extruders;++i) {
        for (unsigned int j=0;j<m_number_of_extruders;++j) {
            double val = 0.;
            edit_boxes[j][i]->GetValue().ToDouble(&val);

            if (flush_multipler == 0) {
                output.push_back(0.);
            }
            else {
                output.push_back((float) val / flush_multipler);
            }
        }
    }
    return output;
}

// Reads values from simple mode to save them for next time:
std::vector<float> WipingPanel::read_extruders_values() {
    std::vector<float> output;
    for (unsigned int i=0;i<m_number_of_extruders;++i) {
        output.push_back(m_old[i]->GetValue());
        output.push_back(m_new[i]->GetValue());
    }
    return output;
}

// This updates the "advanced" matrix based on values from "simple" mode
void WipingPanel::fill_in_matrix() {
    for (unsigned i=0;i<m_number_of_extruders;++i) {
        for (unsigned j=0;j<m_number_of_extruders;++j) {
            if (i==j) continue;
                edit_boxes[j][i]->SetValue(wxString("")<< (m_old[i]->GetValue() + m_new[j]->GetValue()));
        }
    }
}



// Function to check if simple and advanced settings are matching
bool WipingPanel::advanced_matches_simple() {
    for (unsigned i=0;i<m_number_of_extruders;++i) {
        for (unsigned j=0;j<m_number_of_extruders;++j) {
            if (i==j) continue;
            if (edit_boxes[j][i]->GetValue() != (wxString("")<< (m_old[i]->GetValue() + m_new[j]->GetValue())))
                return false;
        }
    }
    return true;
}


// Switches the dialog from simple to advanced mode and vice versa
void WipingPanel::toggle_advanced(bool user_action) {
    if (user_action)
        m_advanced = !m_advanced;                // user demands a change -> toggle
    else {
        // BBS: show advanced mode by default
        //m_advanced = !advanced_matches_simple(); // if called from constructor, show what is appropriate
        m_advanced = true;
    }

    (m_advanced ? m_page_advanced : m_page_simple)->Show();
    (!m_advanced ? m_page_advanced : m_page_simple)->Hide();

    if (m_advanced)
        if (user_action) fill_in_matrix();  // otherwise keep values loaded from config

   m_sizer->Layout();
   Refresh();
}

/// \brief Re-measure this panel and resize the dialog that hosts it.
///
/// The dialog pins this panel's minimum size and the scrolling viewport's virtual size to the
/// panel's content size, so both have to be recomputed when the fitted size would change. That
/// happens on a DPI change: the panel's DIP sizes are frozen at construction, while the display
/// work area and the DIP floors follow the scale factor.
///
/// No page-switch caller is needed: toggle_advanced() is called once from the constructor
/// without user_action and nothing else calls it, so the visible page never changes and the
/// panel's content size stays the same while the dialog is open.
/// WipingDialog::on_dpi_changed() is therefore the only caller.
///
/// Both windows are looked up from the hierarchy: the scrolling viewport is our parent and the
/// dialog is our top level window.
///
/// Does nothing outside the expected hierarchy (for example when no viewport/dialog was found).
///
/// GUI thread only: it reads/sets window geometry.
void WipingPanel::refit_in_dialog()
{
    auto* viewport = dynamic_cast<wxScrolledWindow*>(GetParent());
    auto* dlg      = dynamic_cast<wxDialog*>(wxGetTopLevelParent(this));
    if (viewport == nullptr || dlg == nullptr)
        return;

    // Re-apply the scroll step too: it was converted from DIP with the scale factor that was
    // active when the dialog was built, and this is the DPI-change path.
    viewport->SetScrollRate(viewport->FromDIP(SCROLL_RATE_DIP), viewport->FromDIP(SCROLL_RATE_DIP));
    fit_wiping_dialog_to_display(dlg, viewport, this);
}
