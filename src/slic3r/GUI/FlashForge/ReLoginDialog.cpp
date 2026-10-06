#include "ReLoginDialog.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/FlashForge/MultiComHelper.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/GUI/FlashForge/LoginDialog.hpp"
#include "slic3r/GUI/FlashForge/DeviceData.hpp"
#include "slic3r/GUI/FFUtils.hpp"

namespace Slic3r {
namespace GUI {

static wxImage get_default_user_image()
{
    wxImage image;
    image.LoadFile(Slic3r::GUI::from_u8(Slic3r::var("login_default_usr_pic.png")), wxBITMAP_TYPE_PNG);
    return image;
}

static void update_user_panel_image(RoundImage *panel)
{
    if (!panel) {
        return;
    }
    if (wxGetApp().getUsrPic().IsOk()) {
        panel->SetImage(wxGetApp().getUsrPic());
    } else {
        wxImage image = get_default_user_image();
        if (image.IsOk()) {
            panel->SetImage(image);
        }
    }
}

RoundImage::RoundImage(wxWindow *parent, const wxSize &size /*=wxDefaultSize*/)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
{
    Bind(wxEVT_PAINT, &RoundImage::OnPaint, this);
    // Bind(wxEVT_SIZE, &RoundImagePanel::OnSize, this);
}

void RoundImage::SetImage(const wxImage image)
{
    m_image = image;
    Refresh();
}

void RoundImage::OnSize(wxSizeEvent &event) {}

void RoundImage::OnPaint(wxPaintEvent &event)
{
    if (!m_image.IsOk()) {
        event.Skip();
        return;
    }

    wxSize  size = GetSize();
    wxImage img  = m_image;
    img.Rescale(size.x, size.y);
    wxPaintDC dc(this);
    if (!img.HasAlpha()) {
        img.InitAlpha();
    }
    wxBitmap bmp(size.x, size.y);
    {
        wxMemoryDC memdc;
        memdc.SelectObject(bmp);
#ifdef _WIN32
        memdc.Blit({0, 0}, size, &dc, {0, 0});
#endif
        wxGCDC dc2(memdc);
        dc2.SetFont(GetFont());
        CreateRegion(dc2);
        memdc.SelectObject(wxNullBitmap);
    }
    wxImage ref_img = bmp.ConvertToImage();
    for (int y = 0; y < img.GetHeight(); ++y) {
        for (int x = 0; x < img.GetWidth(); ++x) {
            img.SetAlpha(x, y, ref_img.GetRed(x, y));
        }
    }
    dc.DrawBitmap(wxBitmap(img), 0, 0);
}

void RoundImage::CreateRegion(wxDC &dc)
{
    wxSize sz = GetSize();
    int    x  = sz.x / 2;
    int    y  = sz.y / 2;
    dc.SetBrush(*wxBLACK_BRUSH);
    dc.DrawRectangle(0, 0, sz.x, sz.y);
    //dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetPen(wxColor("#000000"));
    dc.SetBrush(*wxRED);
    dc.DrawCircle(x, y, (x < y) ? x : y);
}

ReLoginDialog::ReLoginDialog() : TitleDialog(static_cast<wxWindow *>(wxGetApp().mainframe), _L("Login"), 6)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    AppConfig *app_config = wxGetApp().app_config;
    std::string usr_name("usrname");
    std::string pic = "login_default_usr_pic.png";
    std::string usr_pic((boost::filesystem::path(Slic3r::var_dir()) / pic).make_preferred().string());
    if(app_config){
        usr_name = app_config->get("usr_name");
        if(!app_config->get("usr_pic").empty()){
            usr_pic = app_config->get("usr_pic");
        }
    }

//水平布局
    m_sizer_main = MainSizer();
    m_sizer_main->SetMinSize(wxSize(FromDIP(380), FromDIP(445)));

//***添加空白间距
    auto m_panel_separotor_0 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1,FromDIP(83)), wxTAB_TRAVERSAL);
    m_panel_separotor_0->SetBackgroundColour(wxColour(255,255,255));

    m_sizer_main->Add(m_panel_separotor_0, 0, wxEXPAND | wxALL, 0);

//**添加用户
    m_user_panel = new RoundImage(this, wxSize(FromDIP(80), FromDIP(80)));
    update_user_panel_image(m_user_panel);
    wxGetApp().Bind(EVT_USER_HEAD_IMAGE_UPDATED, &ReLoginDialog::onUserImageUpdated, this);

    m_sizer_main->Add(m_user_panel, 0, wxALIGN_CENTER, 0);
    m_sizer_main->AddSpacer(FromDIP(6));

    wxString username = wxString::FromUTF8(usr_name);
    wxGCDC   dc(this);
    wxString clipName = FFUtils::trimString(dc, username, FromDIP(180));
    m_usr_name = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    m_usr_name->SetLabelText(clipName);
    m_usr_name->SetToolTip(wxString::FromUTF8(usr_name));

    m_sizer_main->Add(m_usr_name, 0, wxALIGN_CENTER, 0);

//***添加空白间距
    auto m_panel_separotor_1 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1,FromDIP(29)), wxTAB_TRAVERSAL);
    m_panel_separotor_1->SetBackgroundColour(wxColour(255,255,255));

    m_sizer_main->Add(m_panel_separotor_1, 0, wxEXPAND | wxALL, 0);

//**登录按钮
    m_re_login_button = new FFButton(this, wxID_ANY,_L("Login"),8);

    m_re_login_button->SetFontDisableColor(wxColour(255, 255, 255));
    m_re_login_button->SetBorderDisableColor(wxColour(221,221,221));
    m_re_login_button->SetBGColor(wxColour(221,221,221));

    m_re_login_button->SetFontHoverColor(wxColour(255, 255, 255));
    m_re_login_button->SetBGHoverColor(wxColour(149,197,255));
    m_re_login_button->SetBorderHoverColor(wxColour(149,197,255));

    m_re_login_button->SetFontPressColor(wxColour(255, 255, 255));
    m_re_login_button->SetBGPressColor(wxColour(17,111,223));
    m_re_login_button->SetBorderPressColor(wxColour(17,111,223));

    m_re_login_button->SetFontColor(wxColour(255, 255, 255));
    m_re_login_button->SetBorderColor(wxColour(50,141,251));
    m_re_login_button->SetBGColor(wxColour(50,141,251));
    m_re_login_button->Bind(wxEVT_LEFT_UP,&ReLoginDialog::onRelogin2BtnClicked, this);
    m_re_login_button->SetMinSize(wxSize(FromDIP(170),FromDIP(35)));
    //m_re_login_button->SetFont((wxFont(wxFontInfo(16))));

    m_sizer_main->Add(m_re_login_button, 0, wxALIGN_CENTER, 0);

//***添加空白间距
    auto m_panel_separotor_2 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1,FromDIP(18)), wxTAB_TRAVERSAL);
    m_panel_separotor_2->SetBackgroundColour(wxColour(255,255,255));

    m_sizer_main->Add(m_panel_separotor_2, 0, wxEXPAND | wxALL, 0);

//**登出按钮
#if defined(__WIN32__) || defined(__LINUX__)
    m_login_out_button = new wxButton(this, wxID_ANY, _L("Login out"));
    m_login_out_button->SetForegroundColour(wxColour(50,141,251));
    m_login_out_button->SetBackgroundColour(wxColour(255,255,255)); 
    m_login_out_button->SetWindowStyleFlag(wxBORDER_NONE); 
    //m_login_out_button->SetFont((wxFont(wxFontInfo(16))));
    m_login_out_button->Bind(wxEVT_BUTTON,&ReLoginDialog::onLoginoutBtnClicked, this);
#elif defined(__APPLE__)
    m_login_out_button = new FFButton(this, wxID_ANY, _L("Login out"),8);

    m_login_out_button->SetFontDisableColor(wxColour(50, 141, 251));
    m_login_out_button->SetBorderDisableColor(wxColour(255, 255, 255));
    m_login_out_button->SetBGColor(wxColour(255, 255, 255));

    m_login_out_button->SetFontHoverColor(wxColour(50, 141, 251));
    m_login_out_button->SetBGHoverColor(wxColour(255, 255, 255));
    m_login_out_button->SetBorderHoverColor(wxColour(255, 255, 255));

    m_login_out_button->SetFontPressColor(wxColour(50, 141, 251));
    m_login_out_button->SetBGPressColor(wxColour(255, 255, 255));
    m_login_out_button->SetBorderPressColor(wxColour(255, 255, 255));

    m_login_out_button->SetFontColor(wxColour(50, 141, 251));
    m_login_out_button->SetBorderColor(wxColour(255, 255, 255));
    m_login_out_button->SetBGColor(wxColour(255, 255, 255));
    m_login_out_button->Bind(wxEVT_LEFT_UP, &ReLoginDialog::onLoginoutBtnClicked, this);
    m_login_out_button->SetMinSize(wxSize(FromDIP(170), FromDIP(35)));
#endif

    m_sizer_main->Add(m_login_out_button, 0, wxALIGN_CENTER, 0);

    Fit();
    //Thaw();
    Centre(wxBOTH);
    Layout();
}

ReLoginDialog::~ReLoginDialog()
{
    wxGetApp().Unbind(EVT_USER_HEAD_IMAGE_UPDATED, &ReLoginDialog::onUserImageUpdated, this);
}
#if defined(__WIN32__) || defined(__LINUX__)
void ReLoginDialog::onLoginoutBtnClicked(wxCommandEvent& event)
{
#if defined(__WIN32__) || defined(__LINUX__)
    Hide();
#elif defined(__APPLE__)
    Close();
#endif
    wxGetApp().handle_login_out();
    AppConfig *app_config = wxGetApp().app_config;
    if(app_config){
        app_config->set("access_token","");
        app_config->set("refresh_token","");
        app_config->set("token_expire_time", "");
        app_config->set("token_start_time", "");
        app_config->set("usr_email", "");
        app_config->set("show_user_points", "");
        app_config->set("usr_name","");
        app_config->set("usr_pic","");
        app_config->set("usr_uid", "");
        Slic3r::GUI::MultiComMgr::inst()->removeWanDev();
    }
    event.Skip();
}
#endif

#ifdef __APPLE__
void ReLoginDialog::onLoginoutBtnClicked(wxMouseEvent &event) 
{
#if defined(__WIN32__) || defined(__LINUX__)
    Hide();
#elif defined(__APPLE__)
    Close();
#endif
    wxGetApp().handle_login_out();
    AppConfig *app_config = wxGetApp().app_config;
    if (app_config) {
        app_config->set("access_token", "");
        app_config->set("refresh_token", "");
        app_config->set("token_expire_time", "");
        app_config->set("token_start_time", "");
        app_config->set("usr_email", "");
        app_config->set("show_user_points", "");
        app_config->set("usr_name", "");
        app_config->set("usr_pic", "");
        app_config->set("usr_uid", "");
        Slic3r::GUI::MultiComMgr::inst()->removeWanDev();
    }
    event.Skip();
}
#endif

void ReLoginDialog::onRelogin2BtnClicked(wxMouseEvent& event)
{
#if defined(__WIN32__) || defined(__LINUX__)
    Hide();
#elif defined(__APPLE__)
    Close();
#endif
    AppConfig *app_config = wxGetApp().app_config;
    if(app_config){
        std::string usr_name = app_config->get("usr_name");
        std::string usr_eamil = app_config->get("usr_email");
        std::string usr_uid = app_config->get("usr_uid");
        std::string usr_pic = app_config->get("usr_pic");
        std::string usr_email = app_config->get("usr_email");
        std::string show_user_points = app_config->get("show_user_points");
        if (usr_name.empty()) {
            usr_name = LoginDialog::GetUsrName();
        }
        std::string refresh_token = app_config->get("refresh_token");
        std::string access_token = app_config->get("access_token");
        if(refresh_token.empty() && access_token.empty()){
            wxGetApp().ShowUserLogin();
        }
        else{
            com_add_wan_dev_data_t add_wan_dev_data;
            add_wan_dev_data.userProfile.uid = usr_uid;
            add_wan_dev_data.userProfile.nickname = usr_name;
            add_wan_dev_data.userProfile.headImgUrl = usr_pic;
            add_wan_dev_data.userProfile.email = usr_eamil;
            add_wan_dev_data.showUserPoints = show_user_points == "true";
            wxGetApp().handle_login_result(access_token, add_wan_dev_data);
            BOOST_LOG_TRIVIAL(info) << "usr login succeed 333 : ReLoginDialog::onRelogin2BtnClicked";
        }
    }
    event.Skip();
}

void ReLoginDialog::onUserImageUpdated(wxCommandEvent& event)
{
    update_user_panel_image(m_user_panel);
}

void ReLoginDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    ;
}

}
}
