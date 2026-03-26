#ifndef slic3r_WebTextPanel_hpp_
#define slic3r_WebTextPanel_hpp_

#include <wx/panel.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

/**
 * Independent WebView panel for webText tab.
 * No SSWCP, no fltviews, no shared state with other WebViews.
 * Supports bidirectional PostMessage: JS <-> C++
 */
class WebTextPanel : public wxPanel {
public:
    WebTextPanel(wxWindow *parent);
    virtual ~WebTextPanel();

    void load_url(const wxString& url);
    void reload();
    wxWebView* get_browser() const { return m_browser; }

private:
    void OnScriptMessage(wxWebViewEvent& evt);
    void RunScript(const wxString& javascript);

    wxWebView* m_browser{ nullptr };
    bool m_orca_url_loaded{ false };  // Windows: 首次 EVT_SIZE 后加载 orca://app/
};

} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_WebTextPanel_hpp_ */
