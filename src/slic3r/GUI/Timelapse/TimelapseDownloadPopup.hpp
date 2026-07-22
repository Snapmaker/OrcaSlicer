#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/scrolwin.h>

namespace Slic3r {

class BBLStatusBarSend;

namespace GUI {

class TimelapseDownloadPopup : public DPIDialog
{
public:
    struct TaskInfo
    {
        std::string file_name;
        std::string file_url;
        int         encrypt_type;
        std::string sn;
        size_t      file_size;
        std::string date_index;
    };

    TimelapseDownloadPopup(wxWindow* parent);
    ~TimelapseDownloadPopup();

    void add_tasks(const std::vector<TaskInfo>& tasks);

    void set_task_progress(int index, int percent,
                           size_t downloaded, size_t total);
    void set_task_status(int index, const wxString& text);
    void mark_task_complete(int index, const std::string& file_path);
    void mark_task_error(int index, const std::string& error);
    void mark_task_cancelled(int index);

    void mark_all_complete(int completed, int failed,
                           const std::string& save_path,
                           const std::string& latest_file);

    void set_task_cancel_callback(int index, std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);

    void Close();

    wxWindow* GetPopupParent() { return DPIDialog::GetParent(); }

private:
    void position_bottom_right();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_timer(wxTimerEvent&);
    void toggle_collapse();
    void update_layout_size();

    struct TaskRow
    {
        std::shared_ptr<BBLStatusBarSend> status_bar;
        std::string file_name;
        int        state; // 0=pending, 1=downloading, 2=complete, 3=error, 4=cancelled
    };

    wxPanel*           m_title_bar;
    Label*             m_title_label;
    Button*            m_collapse_btn;
    wxScrolledWindow*  m_task_panel;
    wxBoxSizer*        m_task_sizer;

    std::vector<TaskRow> m_rows;
    bool m_collapsed;
    bool m_all_complete;
    int  m_task_count;

    std::string m_open_target;

    std::function<void()> m_close_callback;

    wxTimer* m_position_timer;

    static constexpr int TITLE_BAR_HEIGHT   = 36;
    static constexpr int TASK_ROW_HEIGHT    = 55;
    static constexpr int DIALOG_WIDTH       = 456;
    static constexpr int MAX_VISIBLE_ROWS   = 5;
    static constexpr int DEFAULT_VISIBLE    = 2;
    static constexpr int SCROLL_RATE        = 10;
};

}} // namespace Slic3r::GUI
