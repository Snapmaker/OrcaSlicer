#ifndef slic3r_WebDeviceDialog_hpp_
#define slic3r_WebDeviceDialog_hpp_

#include <wx/dialog.h>
#include <wx/webview.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI {

class WebDeviceDialog : public wxDialog
{
public:
    WebDeviceDialog();
    virtual ~WebDeviceDialog();

    void load_url(wxString &url);
    bool run();
    void RunScript(const wxString &javascript);

    void reload();

private:
    void OnClose(wxCloseEvent& evt);
    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    wxWebView *m_browser;
    wxString m_javascript;
    wxString   m_device_url;

    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif 