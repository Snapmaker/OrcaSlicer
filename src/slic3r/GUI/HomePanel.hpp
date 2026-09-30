#ifndef slic3r_GUI_HomePanel_hpp_
#define slic3r_GUI_HomePanel_hpp_

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/panel.h>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/LibraryIndex.hpp"

class wxStaticText;
class wxBoxSizer;
class wxWebView;
class wxWebViewEvent;
class Button;

namespace Slic3r {
namespace GUI {

class WebViewPanel;
class HomeVendors;

// The Home tab: a local page (resources/web/home) with a side menu of sections - Recent (the
// recent projects), Library (the model files in folders the user picked, Utils/LibraryIndex.hpp),
// Print History (the G-code archive) and Vendors (connectors to vendors' APIs, HomeVendors.hpp). The page only draws; everything on disk is read here and
// every action runs here, for files this panel itself listed (Utils/HomeTabLogic.hpp has the rules).
//
// The old flutter start page (WebViewPanel: Snapmaker's model library) is kept behind it for the
// code that still talks to it: it is built only on demand - File > Start page, EVT_LOAD_URL - and
// then shown in place of Home with a strip to go back.
class HomePanel : public wxPanel
{
public:
    explicit HomePanel(wxWindow* parent);
    ~HomePanel() override;

    WebViewPanel* start_page();                              // builds it on first use
    WebViewPanel* built_start_page() const { return m_start; } // null until then

    void show_home();
    void show_start_page();
    bool start_page_shown() const { return m_start != nullptr && m_start_shown; }

    // The Home tab was selected (true) or left (false).
    void on_tab_changed(bool selected);
    void sys_color_changed();

private:
    void ensure_browser();
    void notify_start_page(bool active);
    void apply_colours();

    void on_script_message(wxWebViewEvent& evt);
    void on_navigating(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);
    void handle(const nlohmann::json& msg);

    void send(const nlohmann::json& msg);
    void send_init();
    void send_recent();
    void send_history();                                     // lists off the GUI thread
    void send_history_thumbnails(const std::vector<std::string>& ids);

    void open_recent(const std::string& path);
    void forget_recent(const std::string& path);
    void open_archived(const std::string& id);
    void open_archived_project(const std::string& id);
    void reveal_archived(const std::string& id);
    void delete_archived(const std::string& id);

    // Library. The folders and the hidden files live in app_config; the index and the covers in
    // <data_dir>/library. The index is loaded once, then kept here and rescanned off the GUI thread.
    std::vector<Library::Folder> library_folders() const;
    void                         save_library_folders(const std::vector<Library::Folder>& folders);
    std::set<std::string>        library_hidden() const;
    void                         save_library_hidden(const std::set<std::string>& hidden);
    void                         library_refresh(bool force_scan);
    void                         library_scan();
    void                         send_library();
    void                         send_library_thumbnails(const std::vector<std::string>& ids);
    void                         send_library_plates(const std::string& id);
    std::string                  library_path(const std::string& id) const; // "" unless listed and present
    void                         library_open(const std::string& id, bool import);
    void                         library_hide(const std::string& id);
    void                         library_add_folder();
    void                         library_update_folder(const nlohmann::json& msg);
    void                         library_remove_folder(const std::string& path);

    wxBoxSizer*   m_sizer { nullptr };
    wxPanel*      m_strip { nullptr };
    wxStaticText* m_strip_label { nullptr };
    Button*       m_btn_back { nullptr };
    wxWebView*    m_browser { nullptr };
    WebViewPanel* m_start { nullptr };
    bool          m_start_shown { false };
    bool          m_selected { false };
    bool          m_page_ready { false };

    // What the page was last given: it may only act on these.
    std::vector<std::string> m_recent_paths;
    std::set<std::string>    m_history_ids;
    unsigned                 m_history_generation { 0 };
    unsigned                 m_recent_generation { 0 };
    std::map<std::string, std::string> m_library_paths; // id -> path

    Library::Index m_library;
    bool           m_library_loading { false };
    bool           m_library_loaded { false };
    bool           m_library_scanning { false };
    bool           m_library_rescan { false }; // asked for again while a scan ran
    size_t         m_library_seen { 0 };       // files seen by the running scan
    std::shared_ptr<std::atomic<bool>> m_library_cancel { std::make_shared<std::atomic<bool>>(false) };

    std::unique_ptr<HomeVendors> m_vendors; // made when the page first asks

    // Worker threads report back through CallAfter only while this is alive.
    std::shared_ptr<bool> m_alive { std::make_shared<bool>(true) };
};

} // namespace GUI
} // namespace Slic3r

#endif
