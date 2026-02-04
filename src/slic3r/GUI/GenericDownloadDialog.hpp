#ifndef slic3r_GenericDownloadDialog_hpp_
#define slic3r_GenericDownloadDialog_hpp_

#include <string>
#include <functional>
#include <memory>
#include <atomic>

#include "GUI_Utils.hpp"
#include <wx/dialog.h>
#include <wx/simplebook.h>
#include "BBLStatusBar.hpp"
#include "BBLStatusBarSend.hpp"
#include "Jobs/Worker.hpp"
#include "slic3r/GUI/DownloadManager.hpp"
#include "Widgets/Button.hpp"

class wxBoxSizer;
class wxPanel;
class wxStaticText;
class wxHyperlinkCtrl;

namespace Slic3r {
namespace GUI {

// Generic download dialog for custom download tasks with progress display
class GenericDownloadDialog : public DPIDialog
{
public:
    // Callback types
    using DownloadCallback = std::function<void(size_t task_id, int percent, size_t downloaded, size_t total)>;
    using CompleteCallback = std::function<void(size_t task_id, const std::string& file_path)>;
    using ErrorCallback = std::function<void(size_t task_id, const std::string& error)>;
    using RetryCallback = std::function<void()>;

    GenericDownloadDialog(wxString title, 
                         const std::string& file_url,
                         const std::string& file_name,
                         const std::string& dest_path = "",
                         wxWindow* parent = nullptr);
    ~GenericDownloadDialog();

    // Start download
    void start_download();
    
    // Set callbacks (optional)
    void set_on_progress(DownloadCallback callback) { m_on_progress = callback; }
    void set_on_complete(CompleteCallback callback) { m_on_complete = callback; }
    void set_on_error(ErrorCallback callback) { m_on_error = callback; }
    void set_on_retry(RetryCallback callback) { m_on_retry = callback; }

    // Get download result
    bool is_success() const { return m_download_success; }
    std::string get_file_path() const { return m_file_path; }
    std::string get_error_message() const { return m_error_message; }

    // Show modal and return result
    int ShowModal() override;

protected:
    void on_close(wxCloseEvent& event);
    void on_dpi_changed(const wxRect &suggested_rect) override;
    wxString format_text(wxStaticText* st, wxString str, int warp);

    // Event handlers
    void on_download_progress(size_t task_id, int percent, size_t downloaded, size_t total);
    void on_download_complete(size_t task_id, const std::string& file_path);
    void on_download_error(size_t task_id, const std::string& error);
    void on_retry_clicked(wxCommandEvent& event);
    void on_close_clicked(wxCommandEvent& event);

private:
    void setup_ui();
    void show_progress_page();
    void show_complete_page();
    void show_error_page(const std::string& error_msg);
    void update_progress(int percent, const wxString& status_text = "");

    wxString m_title;
    std::string m_file_url;
    std::string m_file_name;
    std::string m_dest_path;
    size_t m_task_id{0};
    bool m_download_success{false};
    std::string m_file_path;
    std::string m_error_message;

    // Callbacks
    DownloadCallback m_on_progress;
    CompleteCallback m_on_complete;
    ErrorCallback m_on_error;
    RetryCallback m_on_retry;

    // UI components
    wxSimplebook* m_simplebook_status{nullptr};
    std::shared_ptr<BBLStatusBarSend> m_status_bar;
    wxPanel* m_panel_download{nullptr};
    wxPanel* m_panel_complete{nullptr};
    wxPanel* m_panel_error{nullptr};
    
    wxStaticText* m_complete_text{nullptr};
    wxStaticText* m_error_text{nullptr};  
    Button* m_retry_button{nullptr};
    Button* m_close_button{nullptr};
    Button* m_error_close_button{nullptr};  
    
    std::atomic<bool> m_is_destroying{false};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GenericDownloadDialog_hpp_

