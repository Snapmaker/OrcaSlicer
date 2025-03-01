// WebUrlDialog.cpp
#include "WebUrlDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "SSWCP.hpp"
#include <wx/sizer.h>
#include <slic3r/GUI/Widgets/WebView.hpp>

namespace Slic3r { namespace GUI {

BEGIN_EVENT_TABLE(WebUrlDialog, wxDialog)
    EVT_CLOSE(WebUrlDialog::OnClose)
END_EVENT_TABLE()

WebUrlDialog::WebUrlDialog()
    : wxDialog((wxWindow*)(wxGetApp().mainframe), wxID_ANY, _L("Web View"))
{
    SetBackgroundColour(*wxWHITE);

    // 创建顶部输入区域
    auto *top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_url_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString, 
                                wxDefaultPosition, FromDIP(wxSize(300, -1)));
    m_load_button = new wxButton(this, wxID_ANY, _L("Load"));
    
    top_sizer->Add(m_url_input, 1, wxEXPAND | wxALL, 5);
    top_sizer->Add(m_load_button, 0, wxALL, 5);

    // 创建WebView
    m_browser = WebView::CreateWebView(this, "about:blank");
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();

    // 绑定事件
    Bind(wxEVT_WEBVIEW_NAVIGATING, &WebUrlDialog::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &WebUrlDialog::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &WebUrlDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &WebUrlDialog::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebUrlDialog::OnScriptMessage, this, m_browser->GetId());
    m_load_button->Bind(wxEVT_BUTTON, &WebUrlDialog::OnLoadUrl, this);

    // 设置主布局
    auto *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(top_sizer, 0, wxEXPAND);
    main_sizer->Add(m_browser, 1, wxEXPAND);
    
    SetSizer(main_sizer);
    SetSize(FromDIP(wxSize(800, 600)));
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

WebUrlDialog::~WebUrlDialog()
{
    SSWCP::on_webview_delete(m_browser);
}

void WebUrlDialog::load_url(const wxString &url)
{
    m_url_input->SetValue(url);
    m_browser->LoadURL(url);
    m_browser->Show();
    Layout();
}

void WebUrlDialog::OnLoadUrl(wxCommandEvent& evt)
{
    wxString url = m_url_input->GetValue();
    if (!url.IsEmpty()) {
        load_url(url);
    }
}

bool WebUrlDialog::run()
{
    if (this->ShowModal() == wxID_OK) {
        return true;
    }
    return false;
}

void WebUrlDialog::RunScript(const wxString &javascript)
{
    m_javascript = javascript;
    if (!m_browser) return;
    WebView::RunScript(m_browser, javascript);
}

void WebUrlDialog::OnNavigationRequest(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebUrlDialog::OnNavigationComplete(wxWebViewEvent &evt)
{
    m_browser->Show();
    Layout();
}

void WebUrlDialog::OnDocumentLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
}

void WebUrlDialog::OnError(wxWebViewEvent &evt)
{
    wxLogError("Web View Error: %s", evt.GetString());
}

void WebUrlDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", 
                    evt.GetString(), evt.GetMessageHandler());

    // 处理SSWCP消息
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}

void WebUrlDialog::OnClose(wxCloseEvent& evt)
{
    evt.Skip();
}

}} // namespace Slic3r::GUI