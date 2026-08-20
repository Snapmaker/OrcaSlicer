
// WebUrlDialog.hpp
#ifndef slic3r_WebUrlDialog_hpp_
#define slic3r_WebUrlDialog_hpp_

#include <wx/dialog.h>
#include <wx/webview.h>
#include <wx/timer.h>
#include <wx/textctrl.h>
#include <wx/button.h>

namespace Slic3r { namespace GUI {

class WebUrlDialog : public wxDialog
{
public:
    WebUrlDialog();
    virtual ~WebUrlDialog();

    void load_url(const wxString &url);
    bool run();
    void RunScript(const wxString &javascript);

private:
    void OnClose(wxCloseEvent& evt);
    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);
    void OnLoadUrl(wxCommandEvent& evt);

    wxWebView* m_browser{nullptr};
    wxTextCtrl* m_url_input{nullptr};
    wxButton* m_load_button{nullptr};
    wxString m_javascript;

    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif