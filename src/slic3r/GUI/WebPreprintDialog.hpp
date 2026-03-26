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

    bool is_send_page();

    void set_send_page(bool flag);

    bool need_switch_to_device() { return m_switch_to_device; }

    void set_swtich_to_device(bool flag);

    bool is_finish() { return m_finish; }

    void set_finish(bool flag);
    
    // BBS: Safely end modal dialog, preventing duplicate EndModal calls
    void SafeEndModal(int returnCode);

private:
    void OnClose(wxCloseEvent& evt);
    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    void load_orca_preprint_page();
#ifdef __WIN32__
    void try_load_pending_orca_url();
#endif

    wxWebView *m_browser;
    wxString m_javascript;
#ifdef __WIN32__
    wxString m_pending_orca_url;
#endif
    std::string m_gcode_file_name = "";
    std::string m_display_file_name = "";
    bool        m_send_page         = false;
    bool        m_switch_to_device  = false;

    bool  m_finish = false;
    bool  m_modal_ended = false;  // BBS: Flag to prevent duplicate EndModal calls
    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif 