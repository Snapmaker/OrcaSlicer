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

    /** Reload current page in WebView (e.g. recover from white screen after debugger breakpoint). */
    void RecoverWebView();

    void set_gcode_file_name(const std::string& filename);

    void set_display_file_name(const std::string& filename);

    bool is_send_page();

    void set_send_page(bool flag);

    bool need_switch_to_device() { return m_switch_to_device; }

    void set_swtich_to_device(bool flag);

    bool is_finish() { return m_finish; }

    void set_finish(bool flag) { m_finish = flag; }

    /** End modal once; safe to call from SSWCP or from OnClose. Prevents double EndModal. */
    void EndModalWithResult(int code);

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
    wxString    m_preSend_url;
    std::string m_gcode_file_name = "";
    std::string m_display_file_name = "";
    bool        m_send_page         = false;
    bool        m_switch_to_device  = false;

    bool  m_finish = false;
    bool  m_modal_ended = false;  // guard against double EndModal (SSWCP vs OnClose)
    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif 