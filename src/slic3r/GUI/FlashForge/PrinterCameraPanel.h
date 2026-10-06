#ifndef slic3r_GUI_PrinterCameraPanel_hpp_
#define slic3r_GUI_PrinterCameraPanel_hpp_

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/webview.h>
#include "MultiComDef.hpp"

namespace Slic3r { namespace GUI {

class PrinterCameraPanel : public wxPanel
{
public:
    PrinterCameraPanel(wxWindow *parent);

    void setSize(wxSize size);

    void setCurComId(com_id_t comId);

    void setStreamUrl(const std::string &streamUrl);

    void setOffline();

    void showPopup();

private:
    void onPaint(wxPaintEvent &event);

    void onScriptMessage(wxWebViewEvent &event);

    void onPageLoaded(wxWebViewEvent &event);

    // Runs a player command now and remembers it, so a page that was still loading (or that
    // reloads) is brought back to the current stream state by onPageLoaded().
    void runPlayerCommand(const wxString &script);

private:
    com_id_t   m_curComId;
    wxWebView *m_webView;
    wxDialog  *m_popupDlg;
    wxString   m_lastCommand;
};

}} // namespace Slic3r::GUI

#endif
