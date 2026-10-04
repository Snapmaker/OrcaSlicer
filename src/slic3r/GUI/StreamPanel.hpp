#pragma once

#include <wx/panel.h>

class wxWebView;
class wxWebViewEvent;
class wxShowEvent;
class wxIconizeEvent;

namespace Slic3r {
namespace GUI {

// "Stream" tab: grid of LAN camera streams (see resources/web/orca/stream_center.html).
class StreamPanel : public wxPanel
{
public:
    StreamPanel(wxWindow* parent);

private:
    void OnScriptMessage(wxWebViewEvent& evt);
    // Tell the page whether it is on screen, so its camera tiles stop streaming while the tab is
    // not the selected one or the window is minimised (window.__snorcaTilesActive in
    // stream_center.html). The page also listens for visibilitychange, but a WebView in a hidden
    // wx page is not reliably told that.
    void SetPageActive(bool active);
    void OnShow(wxShowEvent& evt);
    void OnIconize(wxIconizeEvent& evt);
    void OnPageLoaded(wxWebViewEvent& evt);

    wxWebView* m_browser { nullptr };
    bool       m_active { true };   // what the page was last told (it starts out running)
    bool       m_iconized { false };
};

} // namespace GUI
} // namespace Slic3r
