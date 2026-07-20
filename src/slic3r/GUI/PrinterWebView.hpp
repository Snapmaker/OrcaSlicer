#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>


namespace Slic3r {
namespace GUI {


class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    bool isSnapmakerPage();
    void sendMessage(const std::string& msg);
    wxWebView* get_browser() const { return m_browser; }
    // Re-issue the last page load if the page never loaded successfully. Called
    // when the user enters the Device tab so a page whose initial load failed
    // (e.g. during startup) heals itself instead of staying blank all session.
    void reload_if_failed();

private:
    void SendAPIKey();
    void OnLoadRetryTimer(wxTimerEvent& evt);

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    bool m_apikey_sent;
    // Retry state for transient localhost load failures. The device page is
    // served by the app's embedded HTTP server; if that server (or the
    // webview's network stack) isn't ready when the first load fires during
    // startup, WKWebView hangs and reports a connection timeout after 60s,
    // leaving the page blank forever. A timer re-issues the load every few
    // seconds until the page loads (or we give up), so the tab heals itself
    // without the user having to revisit it.
    wxString m_last_url;
    int      m_retry_count     = 0;
    bool     m_load_succeeded  = false;
    bool     m_load_in_flight  = false;
    int      m_in_flight_ticks = 0; // timer ticks the current load has been in flight
    wxTimer  m_load_retry_timer;
    static constexpr int RETRY_INTERVAL_MS   = 5000;
    static constexpr int MAX_LOAD_RETRIES    = 24; // ~2 minutes of retries, then give up
    static constexpr int MAX_IN_FLIGHT_TICKS = 3;  // a load in flight >15s counts as hung

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
