#ifndef slic3r_TimelapseProgressPopup_hpp_
#define slic3r_TimelapseProgressPopup_hpp_

#include <memory>
#include <string>
#include <functional>

#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/dialog.h>
#include <wx/timer.h>

class Button;

namespace Slic3r {

class BBLStatusBarSend;

namespace GUI {

class TimelapseProgressPopup : public DPIDialog
{
public:
    TimelapseProgressPopup(wxWindow* parent, const std::string& file_name);
    ~TimelapseProgressPopup();

    void set_progress(int percent);
    void set_status(const wxString& text);
    void set_cancel_callback(std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);
    void mark_complete();
    void mark_error(const std::string& error);

    void reset_for_next_file(int current_index, int total_count,
                             const std::string& file_name);
    void mark_queue_complete(int completed_count, int failed_count,
                             const std::string& save_path = "");
    void show_error_with_retry(const std::string& error,
                               std::function<void()> retry_cb,
                               std::function<void()> skip_cb);

    void Close();

private:
    void position_bottom_right();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_timer(wxTimerEvent&);

    std::shared_ptr<BBLStatusBarSend> m_status_bar;
    Button* m_close_btn;
    wxTimer* m_position_timer;
    bool m_completed{false};
    std::function<void()> m_close_callback;
};

}} // namespace Slic3r::GUI

#endif // slic3r_TimelapseProgressPopup_hpp_
