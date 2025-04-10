#ifndef slic3r_WebPreprintDialog_hpp_
#define slic3r_WebPreprintDialog_hpp_

#include <wx/dialog.h>
#include <wx/webview.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI {

class WebPreprintDialog : public wxDialog
{
public:
    WebPreprintDialog();
    virtual ~WebPreprintDialog();

    void load_url(wxString &url);
    bool run();
    void RunScript(const wxString &javascript);

    void reload();

    void set_gcode_file_name(const std::string& filename);

    void set_display_file_name(const std::string& filename);

private:
    void OnClose(wxCloseEvent& evt);
    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    wxWebView *m_browser;
    wxString m_javascript;
    wxString    m_prePrint_url;
    std::string m_gcode_file_name = "";
    std::string m_display_file_name = "";

    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif 