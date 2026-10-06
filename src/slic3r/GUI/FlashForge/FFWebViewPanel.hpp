#ifndef slic3r_FFWebViewPanel_hpp_
#define slic3r_FFWebViewPanel_hpp_

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <nlohmann/json.hpp>
#include "slic3r/GUI/TitleDialog.hpp"
#include "slic3r/GUI/FlashForge/ComThreadPool.hpp"
#include "slic3r/GUI/FlashForge/FFTextCtrl.hpp"
#include "slic3r/GUI/FlashForge/FFTransientWindow.hpp"
#include "slic3r/GUI/FlashForge/MultiComEvent.hpp"
#include "slic3r/GUI/Widgets/FFButton.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r { namespace GUI {

class NavMoreMenu : public FFTransientWindow
{
public:
    NavMoreMenu(wxWindow *parent);

    void AddItem(const std::string &icon, int iconHeight, const wxString &text);

private:
    static const int IconSpace = 6;
    static const int ItemHeight = 45;

    void OnPaint(wxPaintEvent &evt);
    void OnLeftUp(wxMouseEvent &evt);
    void OnMotion(wxMouseEvent &evt);

private:
    int m_hoverItemIndex;
    std::vector<std::unique_ptr<ScalableBitmap>> m_iconBmps;
    std::vector<wxString> m_texts;
    std::vector<wxSize> m_textSizes;
};

class ReportOptionItem : public wxPanel
{
public:
    ReportOptionItem(wxWindow *parent, const wxString &text, int id);

    int GetId() const { return m_id; }
    bool IsSelected() const { return m_isSelected; }
    void SetSelected(bool isSelected);

private:
    const int Height = 32;
    const int Spacing = 16;
    const int IconSize = 16;

    void OnPaint(wxPaintEvent &evt);
    void OnLeftDown(wxMouseEvent &evt);
    void OnEnterWindow(wxMouseEvent &evt);
    void OnLeaveWindow(wxMouseEvent &evt);

private:
    ScalableBitmap m_selectedBmp;
    wxString       m_text;
    int            m_id;
    wxSize         m_textSize;
    bool           m_isHover;
    bool           m_isSelected;
};

class ReportWindow : public wxDialog
{
public:
    ReportWindow(wxWindow *parent, const nlohmann::json &data);

    bool isOk() const { return m_isOk; }
    wxString GetWindowTitle() const { return m_titleBar->GetTitle(); }

private:
    const int TextCtrlSpacing = 10;

    void Initialize(const nlohmann::json &data);
    void OnPaint(wxPaintEvent &evt);
    void OnSize(wxSizeEvent &evt);
    void OnItemSelected(wxCommandEvent &evt);
    void OnTextChanged(wxCommandEvent &evt);
    void OnReportButton(wxCommandEvent &evt);

private:
    TitleBar                      *m_titleBar;
    wxStaticText                  *m_reportTitleLbl;
    std::vector<ReportOptionItem*> m_optionItems;
    FFTextCtrl                    *m_textCtrl;
    wxPanel                       *m_textCtrlDummyPnl;
    FFButton                      *m_reportBtn;
    bool                           m_isOk;
    const int                      m_radius;
};

class PrintListTipWindow : public FFRoundedWindow
{
public:
    PrintListTipWindow(wxWindow *parent);

    void ShowAutoClose(int msTime);
    void Setup(const wxString &text, bool showButton);
    bool IsAutoCloseTimerRunning();

private:
    void OnButton(wxCommandEvent &evt);

private:
    FFButton *m_button;
    wxStaticText *m_printListTipLbl;
    wxTimer m_timer;
};

struct FindDownloadUrlEvent : public wxCommandEvent {
    FindDownloadUrlEvent(wxEventType type, const wxString &_url, const wxString &_fileName,
        bool _isSupportedFormat)
        : wxCommandEvent(type)
        , url(_url)
        , fileName(_fileName)
        , isSupportedFormat(_isSupportedFormat)
    {
    }
    FindDownloadUrlEvent *Clone() const
    {
        return new FindDownloadUrlEvent(GetEventType(), url, fileName, isSupportedFormat);
    }
    wxString url;
    wxString fileName;
    bool isSupportedFormat;
};

class CheckDownloadUrl : public wxEvtHandler, public std::enable_shared_from_this<CheckDownloadUrl>
{
public:
    void AddUrl(const wxString &url, const std::string &userAgent);

private:
    bool IsDownloadUrl(const wxString &url, const std::string &contentDispositionHeader,
        const std::string &contentTypeHeader, wxString &fileName, bool &isSupportedFormat);

    wxString GetFileName(const std::string &contentDispositionHeader);

private:
    static ComThreadPool *s_threadPool;
};

class OpenBase64Model : public wxEvtHandler, public std::enable_shared_from_this<OpenBase64Model>
{
public:
    void open(std::string &str);

private:
    static bool writeFile(const std::string &destFolder, const std::string &fileName,
        const wxMemoryBuffer &buf, wxString &filePath);

    static wxString getSaveFilePath(const std::string &destFolder, const std::string &fileName, bool isTmp);

private:
    static ComThreadPool *s_threadPool;
};

struct web_veiw_user_config_data_t {
    bool modelPersonalizedRecEnabled;
    wxString modelPersonalizedRecText;
};

class FFWebViewPanel : public wxPanel
{
public:
    FFWebViewPanel(wxWindow *parent);
    
    void RunScript(const wxString &jsStr);
    void SendRecentList(int images);
    void ShowModelDetail(const std::string &data);
    bool ProcComBusGetRequest(const ComBusGetRequestEvent &evt);
    bool ProcComBusPostRequest(const ComBusPostRequestEvent &evt);
    bool GetUserConfigData(web_veiw_user_config_data_t &configData);
    void SetUserConfig(bool modelPersonalizedRecEnabled);
    void GoHome();
    void Rescale();

private:
    bool InitBrowser();
    void InitModelNav();
    void SetMainLayout();
    void CheckGetSystemI18nConfig();
    void PostGetSystemI18nConfig();
    void ProcessGetSystemI18nConfig(const ComBusGetRequestEvent &evt);
    void CheckGetOnlineConfig();
    void PostGetOnlineConfig();
    void ProcessGetOnlineConfig(const ComBusGetRequestEvent &evt);
    void CheckGetDownloadScript();
    void PostGetDownloadScript();
    void ProcessGetDownloadScript(const ComBusGetRequestEvent &evt);
    bool IsUserConfigOk();
    void SetupLikeButton();
    void SetupBackButton();
    void SetupPrintListButton(bool printListAdded);
    void SetupSystemI18n();
    void SetupDownloadScript();
    void MoveViewNowWindow();
    void ReportTrackingData(const std::string &eventType, const std::string &eventName);
    void SyncModelAction(const std::string &action);
    void SyncUserConfig();
    void TryPushBackUrl(const wxString &url);
    void OnHideButton(wxCommandEvent &evt);
    void OnBackButton(wxCommandEvent &evt);
    void OnMoreButton(wxCommandEvent &evt);
    void OnLikeButton(wxCommandEvent& evt);
    void OnPrintListButton(wxCommandEvent &evt);
    void OnMoreMenu(wxCommandEvent &evt);
    void OnReportButton(wxCommandEvent &evt);
    void OnViewNow(wxCommandEvent &evt);
    void OnMainNavigated(wxWebViewEvent &evt);
    void OnMainNewWindow(wxWebViewEvent &evt);
    void OnMainScriptMessageReceived(wxWebViewEvent &evt);
    void OnModelNavigating(wxWebViewEvent &evt);
    void OnModelNavigated(wxWebViewEvent &evt);
    void OnModelLoaded(wxWebViewEvent &evt);
    void OnModelError(wxWebViewEvent &evt);
    void OnModelNewWindow(wxWebViewEvent &evt);
    void OnModelScriptMessageReceived(wxWebViewEvent &evt);
    void OnFindDownloadUrl(FindDownloadUrlEvent &evt);
    void OnOpenBase64Model(wxCommandEvent &evt);
    void OnMainFrameIconize(wxIconizeEvent &evt);
    void OnMainFrameMove(wxMoveEvent &evt);
    void OnMainFrameSize(wxSizeEvent &evt);
    void OnComMaintain(ComWanDevMaintainEvent &evt);
    void OnComGetUserProfile(ComGetUserProfileEvent &evt);
    void OnComAddPrintListModel(ComBusRequestEvent &evt);
    void OnComRemovePrintListModel(ComBusRequestEvent& evt);
    void OnComLikeModel(ComBusRequestEvent& evt);
    void OnComReportModel(ComBusRequestEvent &evt);
    wxString GetModelUrlId(const wxString &url);

private:
    wxPanel            *m_modelPnl;
    wxPanel            *m_modelNavPnl;
    FFPushButton       *m_navHideBtn;
    wxStaticText       *m_navDetailLbl;
    FFPushButton       *m_navBackBtn;
    FFPushButton       *m_navMoreBtn;
    FFButton           *m_navLikeBtn;
    FFButton           *m_navPrintListBtn;
    NavMoreMenu        *m_navMoreMenu;
    PrintListTipWindow *m_viewNowWindow;
    wxPanel            *m_spacerLinePnl;
    wxWebView          *m_mainBrowser;
    wxWebView          *m_modelBrowser;
    wxString            m_homePageUrl;
    std::string         m_uid;
    std::string         m_did;
    std::string         m_sid;
    std::string         m_modelId;
    bool                m_printListAdded;
    std::string         m_modelReqId;
    std::string         m_modelExpIds;
    std::string         m_modelSearchKeyword;
    std::string         m_modelDownloadJsId;
    std::string         m_modelDownloadType;
    wxString            m_modelLoadingUrl;
    wxString            m_navDetailText;
    wxString            m_reportMenuText;
    wxString            m_addPrintListText;
    wxString            m_removePrintListText;
    bool                m_modelLike;
    int                 m_modelLikeCount;
    wxString            m_modelPersonalizedRecText;
    wxString            m_addPrintListTipText;
    wxString            m_removePrintListTipText;
    wxString            m_reportWndTitle;
    bool                m_showWebviewBackButton;
    bool                m_autoOpenDownloadLink;
    bool                m_modelPersonalizedRecEnabled;
    nlohmann::json      m_systemI18nConfig;
    nlohmann::json      m_reportConfig;
    nlohmann::json      m_userConfig;
    nlohmann::json      m_downloadJsConfig;
    int                 m_getSystemI18nConfigTryCnt;
    int64_t             m_getSystemI18nConfigReqId;
    int                 m_getOnlineConfigTryCnt;
    int64_t             m_getOnlineConfigReqId;
    int                 m_getDownloadScriptTryCnt;
    int64_t             m_getDownloadScriptReqId;
    int64_t             m_printListReqId;
    int64_t             m_reportReqId;
    std::set<int64_t>   m_setUserConfigReqIds;
    std::string         m_modelUserAgent;
    std::map<std::string, std::string> m_downloadJsMap;
    std::shared_ptr<CheckDownloadUrl>  m_checkDownloadUrl;
    std::shared_ptr<OpenBase64Model>   m_openBase64Model;
    std::vector<std::pair<wxString, wxString>> m_modelBackUrls;
};

}} // namespace Slic3r::GUI

#endif
