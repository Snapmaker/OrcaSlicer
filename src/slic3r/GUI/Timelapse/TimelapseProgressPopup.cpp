#include "TimelapseProgressPopup.hpp"

#include <wx/sizer.h>
#include <wx/display.h>

#include "slic3r/GUI/BBLStatusBarSend.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

TimelapseProgressPopup::TimelapseProgressPopup(wxWindow* parent, const std::string& file_name)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(456, 75),
                wxSTAY_ON_TOP | wxBORDER_SIMPLE)
{
    SetBackgroundColour(*wxWHITE);

    m_status_bar = std::make_shared<BBLStatusBarSend>(this);
    wxPanel* panel = m_status_bar->get_panel();
    panel->SetSize(wxSize(FromDIP(400), FromDIP(55)));
    panel->SetMinSize(wxSize(FromDIP(400), FromDIP(55)));

    StateColor btn_bg_red(
        std::pair<wxColour, int>(wxColour(180, 50, 50), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(200, 40, 40), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(240, 60, 60), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(220, 55, 55), StateColor::Normal));

    StateColor btn_bd_red(
        std::pair<wxColour, int>(wxColour(180, 50, 50), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(220, 55, 55), StateColor::Normal));

    StateColor btn_txt_white(
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(255, 255, 255), StateColor::Normal));

    m_close_btn = new Button(this, "X");
    m_close_btn->SetSize(wxSize(FromDIP(20), FromDIP(20)));
    m_close_btn->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    m_close_btn->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_close_btn->SetBackgroundColor(btn_bg_red);
    m_close_btn->SetBorderColor(btn_bd_red);
    m_close_btn->SetTextColor(btn_txt_white);
    m_close_btn->SetCornerRadius(FromDIP(10));
    m_close_btn->SetFont(::Label::Body_13);
    m_close_btn->SetCursor(wxCURSOR_HAND);
    m_close_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_close_callback) m_close_callback();
    });

    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(panel, 0, wxALL | wxEXPAND, FromDIP(8));
    sizer->Add(m_close_btn, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(8));
    SetSizer(sizer);
    Layout();
    Fit();

    position_bottom_right();

    m_status_bar->set_range(100);
    m_status_bar->set_progress(0);
    m_status_bar->set_status_text(_L("Preparing download..."));
    m_status_bar->change_button_label(_L("Cancel"));

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        if (m_close_callback) m_close_callback();
    });

    m_position_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &TimelapseProgressPopup::on_timer, this);
    m_position_timer->Start(200);

    wxGetApp().UpdateDlgDarkUI(this);
}

TimelapseProgressPopup::~TimelapseProgressPopup()
{
}

void TimelapseProgressPopup::position_bottom_right()
{
    wxWindow* p = GetParent();
    if (!p) return;

    wxRect parentRect = p->GetScreenRect();
    wxSize mySize = GetSize();

    int x = parentRect.GetRight() - mySize.GetWidth() - FromDIP(24);
    int y = parentRect.GetBottom() - mySize.GetHeight() - FromDIP(24);
    SetPosition(wxPoint(x, y));
}

void TimelapseProgressPopup::set_progress(int percent)
{
    if (m_completed) return;
    m_status_bar->set_progress(percent);
    m_status_bar->show_progress(true);
}

void TimelapseProgressPopup::set_status(const wxString& text)
{
    if (m_completed) return;
    m_status_bar->set_status_text(text);
}

void TimelapseProgressPopup::set_cancel_callback(std::function<void()> cb)
{
    m_status_bar->set_cancel_callback_fina([this, cb = std::move(cb)]() {
        cb();
    });
}

void TimelapseProgressPopup::set_close_callback(std::function<void()> cb)
{
    m_close_callback = std::move(cb);
}

void TimelapseProgressPopup::mark_complete()
{
    m_completed = true;
    m_status_bar->set_progress(100);
    m_status_bar->set_status_text(_L("Download completed"));
    m_status_bar->hide_cancel_button();
    m_status_bar->change_button_label(_L("Close"));
    m_status_bar->set_cancel_callback_fina([this]() { Close(); });
    m_status_bar->show_cancel_button();
}

void TimelapseProgressPopup::mark_error(const std::string& error)
{
    m_completed = true;
    m_status_bar->set_status_text(wxString::FromUTF8(error.c_str()));
    m_status_bar->show_error_info(wxString::FromUTF8(error.c_str()), -1, wxEmptyString, wxEmptyString);
    m_status_bar->hide_cancel_button();
    m_status_bar->change_button_label(_L("Close"));
    m_status_bar->set_cancel_callback_fina([this]() { Close(); });
    m_status_bar->show_cancel_button();
}

void TimelapseProgressPopup::show_error_with_retry(const std::string& error,
                                                     std::function<void()> retry_cb,
                                                     std::function<void()> skip_cb)
{
    m_completed = true;
    m_status_bar->set_status_text(wxString::FromUTF8(error.c_str()));
    m_status_bar->show_error_info(wxString::FromUTF8(error.c_str()), -1, wxEmptyString, wxEmptyString);
    m_status_bar->hide_cancel_button();
    m_status_bar->change_button_label(_L("Retry"));
    m_status_bar->set_cancel_callback_fina([this, retry_cb = std::move(retry_cb)]() {
        m_completed = false;
        m_status_bar->change_button_label(_L("Cancel"));
        retry_cb();
    });
    m_status_bar->show_cancel_button();
    m_close_callback = std::move(skip_cb);
}

void TimelapseProgressPopup::Close()
{
    if (m_position_timer) {
        m_position_timer->Stop();
    }
    this->Hide();
    this->Destroy();
}

void TimelapseProgressPopup::reset_for_next_file(int current_index, int total_count,
                                                  const std::string& file_name)
{
    m_status_bar->set_progress(0);
    m_status_bar->show_progress(true);
    wxString status_text = wxString::Format(_L("File %d/%d: %s -- Downloading..."),
                                            current_index, total_count,
                                            wxString::FromUTF8(file_name.c_str()));
    m_status_bar->set_status_text(status_text);
    m_status_bar->change_button_label(_L("Cancel"));
}

void TimelapseProgressPopup::mark_queue_complete(int completed_count, int failed_count,
                                                   const std::string& save_path)
{
    m_completed = true;
    m_status_bar->set_progress(100);
    m_status_bar->hide_cancel_button();

    if (failed_count > 0) {
        wxString msg = wxString::Format(_L("Download finished: %d completed, %d failed"),
                                        completed_count, failed_count);
        m_status_bar->set_status_text(msg);
    } else {
        m_status_bar->set_status_text(_L("All downloads completed"));
    }

    if (!save_path.empty()) {
        m_status_bar->change_button_label(_L("Open Folder"));
        std::string sp = save_path;
        m_status_bar->set_cancel_callback_fina([this, sp]() {
            desktop_open_any_folderEx(sp);
            Close();
        });
    } else {
        m_status_bar->change_button_label(_L("Close"));
        m_status_bar->set_cancel_callback_fina([this]() { Close(); });
    }
    m_status_bar->show_cancel_button();
}

void TimelapseProgressPopup::on_dpi_changed(const wxRect& suggested_rect)
{
    position_bottom_right();
}

void TimelapseProgressPopup::on_timer(wxTimerEvent&)
{
    position_bottom_right();
}

}} // namespace Slic3r::GUI
