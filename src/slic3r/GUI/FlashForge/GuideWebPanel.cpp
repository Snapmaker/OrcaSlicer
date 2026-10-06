#include "GuideWebPanel.h"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#define LOADING_INTERVAL 200

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_LOADING_TIMEOUT, wxCommandEvent);

GuideWebPanel::GuideWebPanel(wxWindow* parent, wxWindowID id) : 
	wxPanel(parent, id, wxDefaultPosition, wxDefaultSize), m_url("https://api.fdmcloud.flashforge.com/wiki/index.html")
{ 
    auto language = wxGetApp().app_config->get_language_code();
    if (language == "zh-cn") {
        language = "cn";
    }
    m_url += "?lang=" + language;
    SetDoubleBuffered(true);
	auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_web_view        = WebView::CreateWebView(this, m_url);
    m_web_view->Reload(wxWEBVIEW_RELOAD_NO_CACHE);
    m_web_view->SetMinSize(GetClientSize());
    m_loading_page = new LoadingWebPage(this);
    m_loading_page->SetMinSize(GetClientSize());
    m_loading_page->Bind(EVT_LOADING_TIMEOUT, [=](wxCommandEvent& event) {
        m_status = NG;
        m_web_view->Stop();
        m_web_view->Hide();
        m_loading_page->Hide();
        m_loading_page->End();
        m_error_panel->Show();
        Layout();
    });
    sizer->Add(m_web_view, 1, wxEXPAND, 0);
    sizer->Add(m_loading_page, 1, wxEXPAND, 0);
    m_error_panel                 = new wxPanel(this, wxID_ANY);
    m_error_panel->SetMinSize(GetClientSize());
    wxPanel* error_content = new wxPanel(m_error_panel, wxID_ANY);
    wxBoxSizer* error_sizer   = new wxBoxSizer(wxVERTICAL);
    ScalableButton* error_icon    = new ScalableButton(m_error_panel, wxID_ANY, "web_error", "", FromDIP(wxSize(48, 48)), wxDefaultPosition, 2097153L, false, 48);
    auto            error_text    = new wxStaticText(m_error_panel, wxID_ANY, _L("Loading failed. Please try again."));
    auto            again_btn     = new FFButton(m_error_panel, wxID_ANY, _L("Retry"), 8);
    error_icon->SetBackgroundColour(*wxWHITE);
    error_text->SetFont(Label::Body_16);
    again_btn->SetFont(Label::Body_16);
    again_btn->SetMinSize(FromDIP(wxSize(128, 48)));
    again_btn->SetSize(FromDIP(wxSize(128, 48)));
    again_btn->SetBorderColor(wxColor(65, 148, 136));
    again_btn->SetFontColor(wxColor(65, 148, 136));
    again_btn->Bind(wxEVT_BUTTON, [&](wxCommandEvent& evnet) { 
        WebView::LoadUrl(m_web_view, m_url);
    });
    error_sizer->AddStretchSpacer();
    error_sizer->Add(error_icon, 0, wxCENTER, FromDIP(20));
    error_sizer->AddSpacer(FromDIP(20));
    error_sizer->Add(error_text, 0, wxCENTER, FromDIP(10));
    error_sizer->AddSpacer(FromDIP(15));
    error_sizer->Add(again_btn, 0, wxCENTER, FromDIP(10));
    error_sizer->AddStretchSpacer();
    m_error_panel->SetSizer(error_sizer);
    sizer->Add(m_error_panel, 1, wxEXPAND, 0);
    SetSizer(sizer);
    m_web_view->Show();
    m_error_panel->Hide();
    m_loading_page->Hide();
    Layout();
    Bind(wxEVT_WEBVIEW_NAVIGATING, [&](wxWebViewEvent& event) {
        m_url = event.GetURL().utf8_string();
        m_web_view->Hide();
        m_error_panel->Hide();
        m_loading_page->Show();
        m_loading_page->Loading();
        Layout();
        m_status = PREPARE;
    });
    Bind(wxEVT_WEBVIEW_ERROR, [&](wxWebViewEvent& event) { 
        m_status = NG; 
        m_web_view->Hide();
        m_loading_page->Hide();
        m_loading_page->End();
        m_error_panel->Show();
        Layout();
    });
    Bind(wxEVT_WEBVIEW_LOADED, [&](wxWebViewEvent& event) { 
        m_status = NORMAL;
        m_web_view->Show();
        m_loading_page->Hide();
        m_loading_page->End();
        m_error_panel->Hide();
        Layout();
    });
    Bind(wxEVT_WEBVIEW_NEWWINDOW, [&](wxWebViewEvent& event) { 
        wxLaunchDefaultBrowser(event.GetURL(), wxBROWSER_NEW_WINDOW);
    });
}

GuideWebPanel::~GuideWebPanel()
{

}

LoadingWebPage::LoadingWebPage(wxPanel* parent) : 
    wxPanel(parent, wxID_ANY)
{
    for (int i = 0; i < 4; i++) {
        auto           str = boost::format("web_loading_%1%") % (i + 1);
        ScalableBitmap bmp(this, str.str(), FromDIP(60));
        m_loadingIcons.emplace_back(std::move(bmp));
    }

    m_prepareTimer = new wxTimer(this);
    Bind(wxEVT_TIMER, &LoadingWebPage::OnTimer, this);
    Bind(wxEVT_PAINT, &LoadingWebPage::OnPaint, this);
    SetBackgroundColour(*wxWHITE);
    SetDoubleBuffered(true);
}

void LoadingWebPage::OnPaint(wxPaintEvent& event) 
{
    wxAutoBufferedPaintDC dc(this);
    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc)
        return;

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetBrush(wxColor(*wxWHITE));
    gc->DrawRectangle(0, 0, GetClientSize().x, GetClientSize().y);
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    wxPoint   center(GetClientSize().x / 2, GetClientSize().y / 2);
    wxBitmap& bmp = m_loadingIcons[m_loadingIdx].bmp();
    gc->DrawBitmap(bmp, center.x - bmp.GetWidth() / 2, center.y - bmp.GetHeight() / 2 - FromDIP(10), bmp.GetWidth(), bmp.GetHeight());
    auto font = Label::Body_16;
    gc->SetFont(font, wxColour(51, 51, 51));
    
    auto str      = wxString(_CTX("Loading...", "GuideWeb"));
    auto textSize = dc.GetMultiLineTextExtent(str);
    gc->DrawText(str, center.x - textSize.x / 2, center.y - textSize.y + FromDIP(60));
    delete gc;
}

void LoadingWebPage::OnTimer(wxTimerEvent& event)
{
    m_loadTime++;
    m_loadingIdx = (m_loadingIdx + 1) % m_loadingIcons.size();
    if (m_loadTime > 10000 / LOADING_INTERVAL) {
        m_loadingIdx = 0;
        m_prepareTimer->Stop();
        m_loadTime = 0;
        QueueEvent(new wxCommandEvent(EVT_LOADING_TIMEOUT));
    }
    Refresh();
}

void LoadingWebPage::Loading() 
{
    
    if (m_prepareTimer->IsRunning()) {
        m_prepareTimer->Stop();
    }
    m_loadingIdx = 0;
    m_loadTime = 0;
    m_prepareTimer->Start(LOADING_INTERVAL);
    Refresh();
}

void LoadingWebPage::End() 
{
    if (m_prepareTimer->IsRunning()) {
        m_prepareTimer->Stop();
    }
}

LoadingWebPage::~LoadingWebPage() { End(); }

}} // namespace Slic3r
