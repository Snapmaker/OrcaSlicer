#include "PrinterCameraPanel.h"
#include <memory>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

namespace Slic3r { namespace GUI {

PrinterCameraPanel::PrinterCameraPanel(wxWindow *parent)
    : wxPanel(parent)
    , m_curComId(ComInvalidId)
    , m_popupDlg(nullptr)
{
    wxString url = file_url_from_path(boost::filesystem::path(resources_dir()) / "web/orca/missing_connection.html");
    m_webView = WebView::CreateWebView(this, url);
    std::string homePageEnableDebug = wxGetApp().app_config->get("home_page_enable_debug");
    m_webView->EnableAccessToDevTools(homePageEnableDebug == "true" || homePageEnableDebug == "1");
    if (m_webView != nullptr) {
        //m_webView->EnableAccessToDevTools(true);
        m_webView->EnableContextMenu(true);
    }
    Bind(wxEVT_PAINT, &PrinterCameraPanel::onPaint, this);
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterCameraPanel::onScriptMessage, this);
}

void PrinterCameraPanel::setSize(wxSize size)
{
    SetSize(size);
    SetMinSize(size);
    SetMaxSize(size);
    if (m_webView != nullptr) {
        m_webView->SetSize(GetClientSize());
        m_webView->SetMinSize(GetClientSize());
        m_webView->SetMaxSize(GetClientSize());
    }
}

void PrinterCameraPanel::setCurComId(com_id_t comId)
{
    m_curComId = comId;
}

void PrinterCameraPanel::setStreamUrl(const std::string &streamUrl)
{
    nlohmann::json json;
    json["command"] = "modify_rtsp_player_address";
    json["address"] = streamUrl;
    json["language"]    = wxGetApp().app_config->get("language");
    json["sequence_id"] = "10001";
    wxString jsStr = wxString::Format("window.postMessage(%s)", wxString::FromUTF8(json.dump()));
    if (m_webView != nullptr) {
        WebView::RunScript(m_webView, jsStr);
    }
}

void PrinterCameraPanel::setOffline()
{
    nlohmann::json json;
    json["command"] = "close_rtsp";
    json["language"] = wxGetApp().app_config->get("language");
    json["sequence_id"] = "10001";
    wxString jsStr = wxString::Format("window.postMessage(%s)", wxString::FromUTF8(json.dump()));
    if (m_webView != nullptr) {
        WebView::RunScript(m_webView, jsStr);
    }
}

void PrinterCameraPanel::onPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    wxRect rt = GetClientRect();
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->SetBrush(*wxBLACK_BRUSH);
    gc->DrawRectangle(rt.x, rt.y, rt.width, rt.height);
}

void PrinterCameraPanel::onScriptMessage(wxWebViewEvent &event)
{
    if (m_curComId == ComInvalidId) {
        return;
    }
    try {
        nlohmann::json json = nlohmann::json::parse(event.GetString().ToUTF8().data());
        std::string command = json["command"];
        if (command == "rtsp_player_continue") {
            Slic3r::GUI::MultiComMgr::inst()->putCommand(m_curComId, new ComCameraStreamCtrl("open"));
        } else if (command == "full_screen") {
            CallAfter([this]() { showPopup(); });
        }
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "PrinterCameraPanel error, " << event.GetString().c_str();
    }
}

void PrinterCameraPanel::showPopup()
{
    if (m_popupDlg != nullptr) {
        return;
    }
    wxSize videoSize(FromDIP(640), FromDIP(480));
    m_webView->SetClientSize(videoSize);
    m_webView->SetMinClientSize(videoSize);
    m_webView->SetMaxClientSize(videoSize);
    m_popupDlg = new wxDialog(wxGetApp().mainframe, wxID_ANY, "");
    m_popupDlg->SetClientSize(wxSize(m_webView->GetSize().x + 4, m_webView->GetSize().y));
    m_popupDlg->SetMinClientSize(wxSize(m_webView->GetSize().x + 4, m_webView->GetSize().y));
    m_popupDlg->SetMaxClientSize(wxSize(m_webView->GetSize().x + 4, m_webView->GetSize().y));
    m_popupDlg->CenterOnParent();
    m_popupDlg->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterCameraPanel::onScriptMessage, this);
    m_webView->Reparent(m_popupDlg);
    m_popupDlg->Bind(wxEVT_CLOSE_WINDOW, [=](wxCloseEvent& event) {
        m_webView->SetSize(GetClientSize());
        m_webView->SetMinSize(GetClientSize());
        m_webView->SetMaxSize(GetClientSize());
        m_webView->Reparent(this);
        m_popupDlg->Destroy();
        m_popupDlg = nullptr;
    });
    m_popupDlg->Show();
}

}} // namespace Slic3r::GUI
