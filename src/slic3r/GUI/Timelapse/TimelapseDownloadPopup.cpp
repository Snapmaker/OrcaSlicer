#include "TimelapseDownloadPopup.hpp"

#include <wx/sizer.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/toplevel.h>

#include "slic3r/GUI/BBLStatusBarSend.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

TimelapseDownloadPopup::TimelapseDownloadPopup(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                wxDefaultSize,
                wxSTAY_ON_TOP | wxBORDER_SIMPLE)
    , m_title_bar(nullptr)
    , m_title_label(nullptr)
    , m_collapse_btn(nullptr)
    , m_task_panel(nullptr)
    , m_task_sizer(nullptr)
    , m_collapsed(false)
    , m_all_complete(false)
    , m_task_count(0)
{
    SetBackgroundColour(*wxWHITE);

    // Title bar
    m_title_bar = new wxPanel(this, wxID_ANY);
    m_title_bar->SetBackgroundColour(wxColour(250, 250, 250));
    m_title_bar->SetMinSize(wxSize(FromDIP(DIALOG_WIDTH), FromDIP(TITLE_BAR_HEIGHT)));
    m_title_bar->SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH), FromDIP(TITLE_BAR_HEIGHT)));

    wxBoxSizer* title_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_title_label = new Label(m_title_bar, _L("Download Task List"));
    m_title_label->SetFont(::Label::Head_13);
    title_sizer->Add(m_title_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

    StateColor btn_bg(std::pair<wxColour, int>(wxColour(0x90, 0x90, 0x90), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
        std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));

    StateColor btn_bd(std::pair<wxColour, int>(*wxWHITE, StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    StateColor btn_txt(std::pair<wxColour, int>(wxColour(0x90, 0x90, 0x90), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Normal));

    m_collapse_btn = new Button(m_title_bar, "");
    m_collapse_btn->SetSize(wxSize(FromDIP(22), FromDIP(22)));
    m_collapse_btn->SetMinSize(wxSize(FromDIP(22), FromDIP(22)));
    m_collapse_btn->SetMaxSize(wxSize(FromDIP(22), FromDIP(22)));
    m_collapse_btn->SetBackgroundColor(btn_bg);
    m_collapse_btn->SetBorderColor(btn_bd);
    m_collapse_btn->SetTextColor(btn_txt);
    m_collapse_btn->SetCornerRadius(FromDIP(11));
    m_collapse_btn->SetFont(::Label::Body_13);
    m_collapse_btn->SetCursor(wxCURSOR_HAND);
    m_collapse_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        toggle_collapse();
    });
    title_sizer->Add(m_collapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_title_bar->SetSizer(title_sizer);

    // Task panel (scrolled window)
    m_task_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_task_panel->SetBackgroundColour(*wxWHITE);
    m_task_panel->SetScrollRate(FromDIP(SCROLL_RATE), FromDIP(SCROLL_RATE));
    m_task_sizer = new wxBoxSizer(wxVERTICAL);
    m_task_panel->SetSizer(m_task_sizer);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_title_bar, 0, wxEXPAND);
    main_sizer->Add(m_task_panel, 0, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(main_sizer);
    Layout();
    Fit();

    position_bottom_right();

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        if (m_close_callback) m_close_callback();
    });

    m_position_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, &TimelapseDownloadPopup::on_timer, this);
    m_position_timer->Start(200);

    wxGetApp().UpdateDlgDarkUI(this);
}

TimelapseDownloadPopup::~TimelapseDownloadPopup()
{
}

void TimelapseDownloadPopup::add_tasks(const std::vector<TaskInfo>& tasks)
{
    m_task_count = static_cast<int>(tasks.size());

    // Update title with task count
    m_title_label->SetLabel(
        wxString::Format(_L("Download Task List (%d files)"), m_task_count));

    for (size_t i = 0; i < tasks.size(); ++i)
    {
        const TaskInfo& info = tasks[i];

        auto status_bar = std::make_shared<BBLStatusBarSend>(m_task_panel);
        wxPanel* bar_panel = status_bar->get_panel();
        bar_panel->SetMinSize(wxSize(FromDIP(420), FromDIP(55)));
        bar_panel->SetMaxSize(wxSize(FromDIP(420), FromDIP(55)));

        status_bar->set_range(100);
        status_bar->set_progress(0);
        status_bar->show_progress(true);
        wxString init_status = wxString::FromUTF8(info.file_name.c_str())
            + "  —  " + _L("Waiting...");
        status_bar->set_status_text(init_status);

        m_task_sizer->Add(bar_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(2));

        TaskRow row;
        row.status_bar = status_bar;
        row.file_name  = info.file_name;
        row.state      = 0;
        m_rows.push_back(std::move(row));
    }

    update_layout_size();
}

void TimelapseDownloadPopup::set_task_progress(int index, int percent,
                                                size_t downloaded, size_t total)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    TaskRow& row = m_rows[index];
    if (row.state == 2 || row.state == 3 || row.state == 4) return;
    row.state = 1;
    row.status_bar->set_progress(percent);

    auto fmt_size = [](size_t bytes) -> wxString {
        if (bytes >= 1024 * 1024)
            return wxString::Format("%.1f MB", bytes / (1024.0 * 1024.0));
        return wxString::Format("%.0f KB", bytes / 1024.0);
    };
    if (total > 0) {
        row.status_bar->set_status_text(
            wxString::Format("%s  —  %s / %s (%d%%)",
                wxString::FromUTF8(row.file_name.c_str()),
                fmt_size(downloaded), fmt_size(total), percent));
    } else {
        row.status_bar->set_status_text(
            wxString::Format("%s  —  %s...",
                wxString::FromUTF8(row.file_name.c_str()),
                fmt_size(downloaded)));
    }
}

void TimelapseDownloadPopup::set_task_status(int index, const wxString& text)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    wxString full = wxString::FromUTF8(m_rows[index].file_name.c_str()) + "  —  " + text;
    m_rows[index].status_bar->set_status_text(full);
}

void TimelapseDownloadPopup::mark_task_complete(int index, const std::string&)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    TaskRow& row = m_rows[index];
    row.state = 2;
    row.status_bar->set_progress(100);
    wxString msg = wxString::FromUTF8(row.file_name.c_str()) + "  —  " + _L("Download completed");
    row.status_bar->set_status_text(msg);
    row.status_bar->hide_cancel_button();
}

void TimelapseDownloadPopup::mark_task_error(int index, const std::string& error)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    TaskRow& row = m_rows[index];
    row.state = 3;
    wxString msg = error.empty()
        ? _L("Network error or server unreachable")
        : wxString::FromUTF8(error.c_str());
    wxString full = wxString::FromUTF8(row.file_name.c_str()) + "  —  " + msg;
    row.status_bar->set_status_text(full);
    row.status_bar->show_error_info(full, -1, wxEmptyString, wxEmptyString);
    row.status_bar->hide_cancel_button();
}

void TimelapseDownloadPopup::mark_task_cancelled(int index)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    TaskRow& row = m_rows[index];
    row.state = 4;
    wxString msg = wxString::FromUTF8(row.file_name.c_str()) + "  —  " + _L("Cancelled");
    row.status_bar->set_status_text(msg);
    row.status_bar->hide_cancel_button();
}

void TimelapseDownloadPopup::mark_all_complete(int completed, int failed,
                                                  const std::string& save_path,
                                                  const std::string& latest_file)
{
    m_all_complete = true;
    m_open_target = latest_file.empty() ? save_path : latest_file;
    if (!m_open_target.empty()) {
        m_close_callback = [this]() {
            desktop_open_any_folderEx(m_open_target);
            Close();
        };
    }
}

void TimelapseDownloadPopup::set_task_cancel_callback(int index, std::function<void()> cb)
{
    if (index < 0 || index >= static_cast<int>(m_rows.size())) return;
    TaskRow& row = m_rows[index];
    row.status_bar->set_cancel_callback_fina([this, cb = std::move(cb)]() mutable {
        wxGetApp().CallAfter([cb = std::move(cb)]() {
            cb();
        });
    });
}

void TimelapseDownloadPopup::set_close_callback(std::function<void()> cb)
{
    m_close_callback = std::move(cb);
}

void TimelapseDownloadPopup::position_bottom_right()
{
    wxWindow* p = GetPopupParent();
    if (p == nullptr) return;
    wxRect parentRect = p->GetScreenRect();
    wxSize mySize = GetSize();
    int x = parentRect.GetRight() - mySize.GetWidth() - FromDIP(24);
    int y = parentRect.GetBottom() - mySize.GetHeight() - FromDIP(24);
    SetPosition(wxPoint(x, y));
}

void TimelapseDownloadPopup::on_dpi_changed(const wxRect&) { position_bottom_right(); }
void TimelapseDownloadPopup::on_timer(wxTimerEvent&)
{
    wxWindow* p = GetPopupParent();
    if (p != nullptr) {
        wxTopLevelWindow* tlw = dynamic_cast<wxTopLevelWindow*>(p);
        if (tlw != nullptr && tlw->IsIconized()) {
            if (IsShown()) { Hide(); }
            return;
        }
    }

    // Hide when Orca app loses foreground focus to another application
    if (wxWindow::FindFocus() == nullptr) {
        if (IsShown()) { Hide(); }
        return;
    }

    if (!IsShown()) { Show(); }
    position_bottom_right();
}

void TimelapseDownloadPopup::toggle_collapse()
{
    m_collapsed = !m_collapsed;
    if (m_collapsed) {
        m_task_panel->Hide();
        m_collapse_btn->SetLabel(wxString::FromUTF8("\xE2\x96\xB6")); // ▶
    } else {
        m_task_panel->Show();
        m_collapse_btn->SetLabel(wxString::FromUTF8("\xE2\x96\xBC")); // ▼
    }
    update_layout_size();
}

void TimelapseDownloadPopup::update_layout_size()
{
    int total_rows = static_cast<int>(m_rows.size());
    int visible_rows = m_collapsed ? 0 : total_rows;
    if (visible_rows > MAX_VISIBLE_ROWS) { visible_rows = MAX_VISIBLE_ROWS; }
    if (visible_rows == 0 && !m_collapsed && total_rows == 0) { visible_rows = DEFAULT_VISIBLE; }

    int panel_h = visible_rows > 0 ? visible_rows * FromDIP(TASK_ROW_HEIGHT) + FromDIP(8) : 0;

    if (visible_rows > 0) {
        m_task_panel->SetMinSize(wxSize(FromDIP(DIALOG_WIDTH) - FromDIP(12), panel_h));
        m_task_panel->SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH) - FromDIP(12), panel_h));
    }

    if (total_rows > visible_rows && visible_rows > 0) {
        int content_h = total_rows * FromDIP(TASK_ROW_HEIGHT) + FromDIP(8);
        m_task_panel->SetVirtualSize(FromDIP(DIALOG_WIDTH) - FromDIP(12), content_h);
    }

    Layout();
    Fit();
    SetMinSize(wxSize(FromDIP(DIALOG_WIDTH), -1));
    SetMaxSize(wxSize(FromDIP(DIALOG_WIDTH), -1));
    position_bottom_right();
}

void TimelapseDownloadPopup::Close()
{
    if (m_position_timer != nullptr) m_position_timer->Stop();
    this->Hide();
    this->Destroy();
}

}} // namespace Slic3r::GUI
