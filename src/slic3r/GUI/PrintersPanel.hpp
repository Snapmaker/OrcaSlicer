#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include <wx/panel.h>

#include "slic3r/Utils/PrintersMonitor.hpp"

class wxWebView;
class wxWebViewEvent;
class wxIconizeEvent;

namespace Slic3r {
namespace GUI {

// "Printers" tab: every printer this slicer knows, at a glance (resources/web/orca/monitor.html).
// The page is fed in process over the "wx" script channel with the rows RemoteAccess::api_printers
// builds - the same rows as the hub's /summary and the app's Devices page - and never talks to the
// loopback instance API or the hub. Always present, whatever printer preset is selected and
// whatever the old "Multi-device Management" preference says. Message format: PrintersMonitor.hpp.
class PrintersPanel : public wxPanel
{
public:
    explicit PrintersPanel(wxWindow* parent);
    ~PrintersPanel() override;

    // The notebook (through LazyPanelHolder) shows and hides the panel: the page polls only
    // while it is on screen.
    bool Show(bool show = true) override;
    // The slicer's theme changed: the page switches in place (it is excluded from the reload).
    void sys_color_changed();

private:
    void OnScriptMessage(wxWebViewEvent& evt);
    void OnPageLoaded(wxWebViewEvent& evt);
    void OnIconize(wxIconizeEvent& evt);
    void SetPageActive(bool active, bool force = false);
    void RunPageScript(const std::string& fn, const nlohmann::json& arg);

    void get_printers();
    void get_groups();
    void control(const PrintersMonitor::Message& m);
    void job(int id);
    void open_device(const std::string& id);
    void thumbnail(const std::string& archive_id);

    wxWebView* m_browser { nullptr };
    bool       m_shown { false };
    bool       m_iconized { false };
    bool       m_active { false }; // what the page was last told
    // One printer read at a time: a slow one (printers that are off take their probe timeout)
    // must not pile up behind the page's next tick. Shared with the worker, which may outlive us.
    std::shared_ptr<std::atomic<bool>> m_reading;
    // What "Open Device page" may open, from the rows the page was last given (never from the page).
    std::map<std::string, PrintersMonitor::Target> m_targets;
    std::shared_ptr<bool> m_alive;
};

} // namespace GUI
} // namespace Slic3r
