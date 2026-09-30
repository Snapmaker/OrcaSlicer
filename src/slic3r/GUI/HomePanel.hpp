#ifndef slic3r_GUI_HomePanel_hpp_
#define slic3r_GUI_HomePanel_hpp_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/panel.h>

#include <nlohmann/json.hpp>

class wxStaticText;
class wxBoxSizer;
class wxWebView;
class wxWebViewEvent;
class Button;

namespace Slic3r {
namespace GUI {

class WebViewPanel;

// The Home tab: a local page (resources/web/home) with a side menu of sections - Recent (the
// recent projects) and Print History (the G-code archive). The page only draws; everything on disk
// is read here and every action runs here, for files this panel itself listed
// (Utils/HomeTabLogic.hpp has the rules).
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

    // Worker threads report back through CallAfter only while this is alive.
    std::shared_ptr<bool> m_alive { std::make_shared<bool>(true) };
};

} // namespace GUI
} // namespace Slic3r

#endif
