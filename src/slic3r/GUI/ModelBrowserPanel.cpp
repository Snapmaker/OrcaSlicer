#include "ModelBrowserPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModelBrowserRules.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "common_func/common_func.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#endif

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using namespace Slic3r::modelbrowser;

bool model_browser_enabled()
{
#ifdef _WIN32
    return wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("model_browser_beta");
#else
    return false;
#endif
}

#ifdef _WIN32

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::string take_string(LPWSTR s)
{
    if (s == nullptr)
        return std::string();
    std::string r = boost::nowide::narrow(s);
    CoTaskMemFree(s);
    return r;
}

std::string source_of(ICoreWebView2* view)
{
    LPWSTR s = nullptr;
    if (view == nullptr || FAILED(view->get_Source(&s)))
        return std::string();
    return take_string(s);
}

std::string leaf_of(const std::string& path)
{
    const size_t sep = path.find_last_of("/\\");
    return sep == std::string::npos ? path : path.substr(sep + 1);
}

// The isolated user data folder: next to (not inside) the app's own WebView2 data, which wx keeps
// in GetUserLocalDataDir()\EBWebView.
std::wstring user_data_folder()
{
    fs::path dir = into_path(wxStandardPaths::Get().GetUserLocalDataDir()) / "ModelBrowser";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir.wstring();
}

fs::path downloads_folder()
{
    const fs::path dir = into_path(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads));
    boost::system::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec))
        return dir;
    return fs::path();
}

// Where model files go: the same folder as the "Open in" link downloads, else Downloads.
fs::path model_folder()
{
    const fs::path dir(wxGetApp().app_config->get("download_path"));
    boost::system::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec))
        return dir;
    return downloads_folder();
}

COREWEBVIEW2_COLOR to_webview_colour(const wxColour& c) { return COREWEBVIEW2_COLOR{255, c.Red(), c.Green(), c.Blue()}; }

// Sub-resource requests that might reach this computer or the LAN. The patterns are coarse (a
// wildcard also matches the query string), the handler parses the URL and decides.
const wchar_t* const BLOCK_FILTERS[] = {
    L"*://localhost*", L"*://*.localhost*", L"*://127.*",       L"*://0.*",         L"*://10.*",      L"*://172.*",
    L"*://192.168.*",  L"*://169.254.*",    L"*://100.*",       L"*://[*",          L"*.local/*",     L"*.local:*",
    L"*.lan/*",        L"*.lan:*",          L"*.internal/*",    L"*.home.arpa/*",   L"*.ts.net/*",    L"*.ts.net:*",
    L"file:*",
};

typedef HRESULT(STDAPICALLTYPE* CreateEnvironment_t)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

} // namespace

struct ModelBrowserPanel::Impl
{
    ModelBrowserPanel*    owner;
    std::function<void()> on_leave;
    std::shared_ptr<bool> alive { std::make_shared<bool>(true) };

    // ---- widgets
    wxPanel*              toolbar { nullptr };
    Button*               btn_leave { nullptr };
    Button*               btn_back { nullptr };
    Button*               btn_forward { nullptr };
    Button*               btn_reload { nullptr };
    Button*               btn_home { nullptr };
    std::vector<Button*>  site_buttons;
    wxStaticText*         address { nullptr };
    wxStaticText*         status { nullptr };
    Button*               btn_external { nullptr };
    wxPanel*              notice { nullptr };
    wxStaticText*         notice_text { nullptr };
    Button*               notice_action { nullptr };
    Button*               notice_close { nullptr };
    std::function<void()> notice_callback;
    wxTimer               notice_timer;
    wxPanel*              placeholder { nullptr };
    wxStaticText*         placeholder_text { nullptr };
    Button*               btn_retry { nullptr };
    wxWindow*             host { nullptr };

    // ---- browser
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  controller;
    ComPtr<ICoreWebView2>            view;
    bool                             creating { false };
    bool                             active { false };
    std::string                      site_id { "printables" };
    std::string                      last_app_link;
    std::chrono::steady_clock::time_point last_app_link_time;

    struct Popup
    {
        wxFrame*                        frame { nullptr };
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2>           view;
    };
    std::vector<Popup> popups;

    struct Download
    {
        ComPtr<ICoreWebView2DownloadOperation> op;
        EventRegistrationToken                 state_token {};
        EventRegistrationToken                 bytes_token {};
        fs::path                               folder;
        fs::path                               marker;
        fs::path                               part;
        std::string                            name;
        bool                                   to_import { false };
    };
    std::map<int, Download> downloads;
    int                     next_download { 1 };

    std::deque<fs::path> import_queue;
    bool                 importing { false };

    Impl(ModelBrowserPanel* owner, std::function<void()> on_leave) : owner(owner), on_leave(std::move(on_leave)) {}
    ~Impl();

    void build_ui();
    void apply_colours();
    void update_nav_buttons();
    void update_address(const std::string& url);
    void show_placeholder(const wxString& text, bool retry);
    // Info: a result (saved to Downloads). Warning: something was not followed (a local address,
    // an "Open in" link whose file host was refused, a link to another program). Error: a download was refused or failed.
    enum class NoticeKind { Info, Warning, Error };
    NoticeKind notice_kind { NoticeKind::Warning };
    void show_notice(const wxString& text, NoticeKind kind, const wxString& action_label = wxString(), std::function<void()> action = nullptr);
    void apply_notice_colours();
    void hide_notice();

    void create_browser();
    void fail(const wxString& why);
    bool configure(ICoreWebView2* wv);
    void attach_handlers(ICoreWebView2* wv, wxFrame* popup);
    void apply_theme_to_browser();
    void resize();
    void navigate(const std::string& url);
    void go_site(const std::string& id);
    std::string current_url() const { return source_of(view.Get()); }

    void carry_out(const NavDecision& d, const std::string& url, bool user_visible);
    void handle_new_window(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args);
    void open_popup(ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args, ComPtr<ICoreWebView2Deferral> deferral, const wxSize& size);
    bool close_popup(wxFrame* frame); // false when the frame is not (or no longer) a popup of ours

    void handle_download(ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args);
    bool begin_download(ICoreWebView2DownloadStartingEventArgs* args, ICoreWebView2DownloadOperation* op, const std::string& name,
                        const fs::path& folder, bool to_import);
    void on_download_state(int id);
    void on_download_bytes(int id);
    void finish_download(int id, Download d, const std::string& result_path);

    void enqueue_import(const fs::path& path);
    void process_imports();
};

// ------------------------------------------------------------------------------------- UI ----

void ModelBrowserPanel::Impl::build_ui()
{
    wxWindow* panel = owner;
    auto*     main  = new wxBoxSizer(wxVERTICAL);

    toolbar = new wxPanel(panel, wxID_ANY);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto  compact = [this](const wxString& label, const wxString& tip) {
        auto* b = new Button(toolbar, label);
        b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        if (!tip.empty())
            b->SetToolTip(tip);
        return b;
    };
    btn_leave   = compact(_L("Back to Home"), _L("Back to Recent, Library and the other Home sections"));
    btn_back    = compact(_L("Back"), _L("Go back"));
    btn_forward = compact(_L("Forward"), _L("Go forward"));
    btn_reload  = compact(_L("Reload"), _L("Reload the page"));
    btn_home    = compact(_L("Home"), _L("The site's home page"));
    const int gap = owner->FromDIP(4);
    row->Add(btn_leave, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    row->AddSpacer(owner->FromDIP(12));
    for (Button* b : {btn_back, btn_forward, btn_reload, btn_home})
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    row->AddSpacer(owner->FromDIP(12));
    for (const Site& s : sites()) {
        Button* b = compact(from_u8(s.name), wxString());
        const std::string id = s.id;
        b->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { go_site(id); });
        site_buttons.push_back(b);
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    }
    address = new wxStaticText(toolbar, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    address->SetFont(Label::Body_12);
    row->Add(address, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(10));
    status = new wxStaticText(toolbar, wxID_ANY, wxEmptyString);
    status->SetFont(Label::Body_12);
    row->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    btn_external = compact(_L("Open in browser"), _L("Open this page in your web browser"));
    row->Add(btn_external, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    auto* tb_sizer = new wxBoxSizer(wxVERTICAL);
    tb_sizer->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    toolbar->SetSizer(tb_sizer);

    notice      = new wxPanel(panel, wxID_ANY);
    notice_text = new wxStaticText(notice, wxID_ANY, wxEmptyString);
    notice_text->SetFont(Label::Body_13);
    notice_action = new Button(notice, wxEmptyString);
    notice_action->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    notice_close = new Button(notice, _L("Close"));
    notice_close->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto* nrow = new wxBoxSizer(wxHORIZONTAL);
    nrow->Add(notice_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(12));
    nrow->Add(notice_action, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    nrow->Add(notice_close, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(8));
    auto* nsizer = new wxBoxSizer(wxVERTICAL);
    nsizer->Add(nrow, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    notice->SetSizer(nsizer);
    notice->Hide();

    placeholder      = new wxPanel(panel, wxID_ANY);
    placeholder_text = new wxStaticText(placeholder, wxID_ANY, _L("Starting the model browser..."), wxDefaultPosition, wxDefaultSize,
                                        wxALIGN_CENTRE_HORIZONTAL);
    placeholder_text->SetFont(Label::Body_14);
    btn_retry = new Button(placeholder, _L("Retry"));
    btn_retry->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    btn_retry->Hide();
    auto* psizer = new wxBoxSizer(wxVERTICAL);
    psizer->AddStretchSpacer(1);
    psizer->Add(placeholder_text, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, owner->FromDIP(24));
    psizer->Add(btn_retry, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, owner->FromDIP(12));
    psizer->AddStretchSpacer(2);
    placeholder->SetSizer(psizer);

    // The WebView2 controller is a child of this window; nothing else is drawn in it.
    host = new wxWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxCLIP_CHILDREN);
    host->Hide();

    main->Add(toolbar, 0, wxEXPAND);
    main->Add(notice, 0, wxEXPAND);
    main->Add(placeholder, 1, wxEXPAND);
    main->Add(host, 1, wxEXPAND);
    panel->SetSizer(main);

    btn_leave->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_leave)
            on_leave();
    });
    btn_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view)
            view->GoBack();
    });
    btn_forward->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view)
            view->GoForward();
    });
    btn_reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view)
            view->Reload();
        else if (!creating)
            create_browser();
    });
    btn_home->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { go_site(site_id); });
    btn_external->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string url = current_url();
        if (untrusted::is_safe_to_open_externally(url) && url.rfind("mailto:", 0) != 0)
            wxLaunchDefaultBrowser(from_u8(url));
    });
    btn_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { create_browser(); });
    notice_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { hide_notice(); });
    notice_action->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto cb = notice_callback;
        hide_notice();
        if (cb)
            cb();
    });
    notice_timer.SetOwner(owner);
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { hide_notice(); }, notice_timer.GetId());

    host->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        resize();
    });
    host->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& evt) {
        evt.Skip();
        if (controller)
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    });

    apply_colours();
    update_nav_buttons();
}

void ModelBrowserPanel::Impl::apply_colours()
{
    const wxColour bg     = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour bar    = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour text   = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour muted  = StateColor::darkModeColorFor(wxColour("#6B6B6B"));
    owner->SetBackgroundColour(bg);
    toolbar->SetBackgroundColour(bar);
    address->SetForegroundColour(muted);
    status->SetForegroundColour(muted);
    apply_notice_colours();
    placeholder->SetBackgroundColour(bg);
    placeholder_text->SetForegroundColour(text);
    host->SetBackgroundColour(bg);
    for (Button* b : {btn_leave, btn_back, btn_forward, btn_reload, btn_home, btn_external, notice_action, notice_close, btn_retry})
        b->Rescale();
    for (size_t i = 0; i < site_buttons.size(); ++i) {
        const bool current = sites()[i].id == site_id;
        site_buttons[i]->SetStyle(current ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Compact);
        site_buttons[i]->Rescale();
    }
    toolbar->Layout();
    owner->Layout();
    owner->Refresh();
    if (controller) {
        ComPtr<ICoreWebView2Controller2> c2;
        if (SUCCEEDED(controller.As(&c2)))
            c2->put_DefaultBackgroundColor(to_webview_colour(bg));
    }
}

void ModelBrowserPanel::Impl::update_nav_buttons()
{
    BOOL back = FALSE, forward = FALSE;
    if (view) {
        view->get_CanGoBack(&back);
        view->get_CanGoForward(&forward);
    }
    btn_back->Enable(back == TRUE);
    btn_forward->Enable(forward == TRUE);
    btn_reload->Enable(view != nullptr);
    btn_home->Enable(view != nullptr);
    btn_external->Enable(view != nullptr);
}

void ModelBrowserPanel::Impl::update_address(const std::string& url)
{
    wxString shown;
    if (url.rfind("https://", 0) == 0)
        shown = from_u8(url.substr(8));
    else if (url.rfind("http://", 0) == 0)
        shown = _L("Not secure") + "  " + from_u8(url.substr(7));
    else
        shown = from_u8(url);
    address->SetLabel(shown);
    address->SetToolTip(from_u8(url));
    // The site button follows the page.
    if (const Site* s = site_for_url(url)) {
        if (s->id != site_id) {
            site_id = s->id;
            apply_colours();
        }
    }
}

void ModelBrowserPanel::Impl::show_placeholder(const wxString& text, bool retry)
{
    placeholder_text->SetLabel(text);
    placeholder_text->Wrap(owner->FromDIP(520));
    btn_retry->Show(retry);
    placeholder->Show();
    host->Hide();
    owner->Layout();
}

// The app's banner colours (StateColor's light -> dark table, and the active UI theme): the
// warning and error banner backgrounds with the normal text colour, readable in both themes.
// Called again on every theme change (apply_colours), so a notice on screen follows it.
void ModelBrowserPanel::Impl::apply_notice_colours()
{
    const char* bg = notice_kind == NoticeKind::Error ? "#FDE8E8" :   // error banner background
                     notice_kind == NoticeKind::Warning ? "#FFF3EB" : // warning banner background
                                                          "#F3F4F6";  // card / divider grey
    notice->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(bg)));
    notice_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    notice->Refresh();
    notice_text->Refresh();
}

void ModelBrowserPanel::Impl::show_notice(const wxString& text, NoticeKind kind, const wxString& action_label, std::function<void()> action)
{
    notice_kind = kind;
    apply_notice_colours();
    notice_text->SetLabel(text);
    notice_text->Wrap((std::max)(owner->GetClientSize().x - owner->FromDIP(260), owner->FromDIP(300)));
    notice_callback = std::move(action);
    notice_action->SetLabel(action_label);
    notice_action->Show(!action_label.empty() && notice_callback != nullptr);
    notice->Show();
    owner->Layout();
    notice_timer.StartOnce(20000);
}

void ModelBrowserPanel::Impl::hide_notice()
{
    notice_timer.Stop();
    notice_callback = nullptr;
    if (notice->IsShown()) {
        notice->Hide();
        owner->Layout();
    }
}

// --------------------------------------------------------------------------------- browser ----

void ModelBrowserPanel::Impl::fail(const wxString& why)
{
    creating = false;
    if (controller)
        controller->Close();
    controller.Reset();
    view.Reset();
    BOOST_LOG_TRIVIAL(error) << "ModelBrowser: " << into_u8(why);
    show_placeholder(why, true);
    update_nav_buttons();
}

void ModelBrowserPanel::Impl::create_browser()
{
    if (creating || controller)
        return;
    creating = true;
    show_placeholder(_L("Starting the model browser..."), false);

    static HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    auto create = loader ? reinterpret_cast<CreateEnvironment_t>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
    if (create == nullptr) {
        fail(_L("The model browser needs the Microsoft Edge WebView2 Runtime, which could not be loaded."));
        return;
    }

    auto alive_ = alive;
    auto on_env = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [this, alive_](HRESULT result, ICoreWebView2Environment* created) -> HRESULT {
            if (!*alive_)
                return S_OK;
            if (FAILED(result) || created == nullptr) {
                fail(format_wxstr(_L("The model browser could not start (error 0x%1$08X). Is the Microsoft Edge WebView2 Runtime installed?"),
                                  unsigned(result)));
                return S_OK;
            }
            env = created;
            HRESULT hr = env->CreateCoreWebView2Controller(
                (HWND) host->GetHWND(),
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, alive_](HRESULT result, ICoreWebView2Controller* created) -> HRESULT {
                        if (!*alive_) {
                            if (created)
                                created->Close();
                            return S_OK;
                        }
                        if (FAILED(result) || created == nullptr) {
                            fail(format_wxstr(_L("The model browser could not start (error 0x%1$08X)."), unsigned(result)));
                            return S_OK;
                        }
                        controller = created;
                        controller->get_CoreWebView2(&view);
                        if (!view || !configure(view.Get())) {
                            // Never show a view that could talk to the app.
                            fail(_L("The model browser was not started because its isolation could not be confirmed."));
                            return S_OK;
                        }
                        attach_handlers(view.Get(), nullptr);
                        creating = false;
                        placeholder->Hide();
                        host->Show();
                        owner->Layout();
                        apply_colours();
                        apply_theme_to_browser();
                        resize();
                        controller->put_IsVisible(active ? TRUE : FALSE);
                        update_nav_buttons();
                        go_site(site_id);
                        return S_OK;
                    })
                    .Get());
            if (FAILED(hr))
                fail(format_wxstr(_L("The model browser could not start (error 0x%1$08X)."), unsigned(hr)));
            return S_OK;
        });
    const std::wstring folder = user_data_folder();
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: starting in " << boost::nowide::narrow(folder);
    const HRESULT hr = create(nullptr, folder.c_str(), nullptr, on_env.Get());
    if (FAILED(hr))
        fail(format_wxstr(_L("The model browser could not start (error 0x%1$08X). Is the Microsoft Edge WebView2 Runtime installed?"),
                          unsigned(hr)));
}

// Settings for every view of the browser (the main one and popups). Returns false unless the
// view provably has no bridge to the app: web messages and host objects off.
bool ModelBrowserPanel::Impl::configure(ICoreWebView2* wv)
{
    ComPtr<ICoreWebView2Settings> s;
    if (FAILED(wv->get_Settings(&s)) || !s)
        return false;
    s->put_IsWebMessageEnabled(FALSE);
    s->put_AreHostObjectsAllowed(FALSE);
    s->put_AreDevToolsEnabled(wxGetApp().app_config->get_bool("developer_mode") ? TRUE : FALSE);
    s->put_AreDefaultScriptDialogsEnabled(TRUE);
    s->put_IsStatusBarEnabled(TRUE);
    s->put_IsZoomControlEnabled(TRUE);
    s->put_IsBuiltInErrorPageEnabled(TRUE);

    ComPtr<ICoreWebView2Settings2> s2;
    if (SUCCEEDED(s.As(&s2))) {
        LPWSTR ua = nullptr;
        if (SUCCEEDED(s2->get_UserAgent(&ua))) {
            const std::string agent = browser_user_agent(take_string(ua), Snapmaker_VERSION);
            s2->put_UserAgent(boost::nowide::widen(agent).c_str());
        }
    }
    ComPtr<ICoreWebView2Settings4> s4;
    if (SUCCEEDED(s.As(&s4))) {
        s4->put_IsPasswordAutosaveEnabled(FALSE);
        s4->put_IsGeneralAutofillEnabled(FALSE);
    }
    ComPtr<ICoreWebView2_13> v13;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&v13)))) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(v13->get_Profile(&profile)) && profile) {
            ComPtr<ICoreWebView2Profile6> p6;
            if (SUCCEEDED(profile.As(&p6))) {
                p6->put_IsPasswordAutosaveEnabled(FALSE);
                p6->put_IsGeneralAutofillEnabled(FALSE);
            }
        }
    }

    // The assertion: read back what the page could use to reach us.
    BOOL messages = TRUE, host_objects = TRUE;
    if (FAILED(s->get_IsWebMessageEnabled(&messages)) || FAILED(s->get_AreHostObjectsAllowed(&host_objects)))
        return false;
    assert(messages == FALSE && host_objects == FALSE);
    if (messages != FALSE || host_objects != FALSE) {
        BOOST_LOG_TRIVIAL(error) << "ModelBrowser: a view still had web messages or host objects enabled";
        return false;
    }
    return true;
}

void ModelBrowserPanel::Impl::attach_handlers(ICoreWebView2* wv, wxFrame* popup)
{
    auto                   alive_ = alive;
    EventRegistrationToken token;

    // Top-level navigations, redirects included.
    wv->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                   [this, alive_](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                       if (!*alive_)
                                           return S_OK;
                                       LPWSTR uri = nullptr;
                                       args->get_Uri(&uri);
                                       const std::string url = take_string(uri);
                                       const NavDecision d   = decide_navigation(url, source_of(sender), true);
                                       if (d.action != NavAction::Allow) {
                                           args->put_Cancel(TRUE);
                                           carry_out(d, url, true);
                                       }
                                       return S_OK;
                                   })
                                   .Get(),
                               &token);
    // Frames inside the page.
    wv->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [this, alive_](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                            if (!*alive_)
                                                return S_OK;
                                            LPWSTR uri = nullptr;
                                            args->get_Uri(&uri);
                                            const std::string url = take_string(uri);
                                            const NavDecision d   = decide_navigation(url, source_of(sender), false);
                                            if (d.action != NavAction::Allow) {
                                                args->put_Cancel(TRUE);
                                                // An "Open in" button may navigate a hidden frame; that is the user's click,
                                                // so a refused app link is reported. Blocked web frames (ads, probes) are not.
                                                const bool app_link = url.rfind("http", 0) != 0 && url.rfind("about:", 0) != 0 &&
                                                                      url.rfind("blob:", 0) != 0 && url.rfind("data:", 0) != 0;
                                                carry_out(d, url, d.action != NavAction::Block || app_link);
                                            }
                                            return S_OK;
                                        })
                                        .Get(),
                                    &token);
    // The backstop for links to other programs.
    ComPtr<ICoreWebView2_18> v18;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&v18))))
        v18->add_LaunchingExternalUriScheme(Callback<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
                                                [this, alive_](ICoreWebView2* sender, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
                                                    if (!*alive_)
                                                        return S_OK;
                                                    args->put_Cancel(TRUE);
                                                    LPWSTR uri = nullptr;
                                                    args->get_Uri(&uri);
                                                    const std::string url = take_string(uri);
                                                    carry_out(decide_app_link(url, source_of(sender)), url, true);
                                                    return S_OK;
                                                })
                                                .Get(),
                                            &token);

    wv->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                   [this, alive_](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                       if (*alive_)
                                           handle_new_window(sender, args);
                                       else
                                           args->put_Handled(TRUE);
                                       return S_OK;
                                   })
                                   .Get(),
                               &token);

    ComPtr<ICoreWebView2_4> v4;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&v4))))
        v4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                                     [this, alive_](ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                                         if (*alive_)
                                             handle_download(sender, args);
                                         else
                                             args->put_Cancel(TRUE);
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);

    // Camera, microphone, location, notifications, clipboard, MIDI, sensors, automatic downloads...
    wv->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                                        args->get_PermissionKind(&kind);
                                        args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                                        BOOST_LOG_TRIVIAL(info) << "ModelBrowser: denied permission " << int(kind);
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

    // Sub-resources on this computer or the LAN (probes of the page server or the hub).
    ComPtr<ICoreWebView2_22> v22;
    const bool               with_kinds = SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&v22)));
    for (const wchar_t* pattern : BLOCK_FILTERS) {
        if (with_kinds)
            v22->AddWebResourceRequestedFilterWithRequestSourceKinds(pattern, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
                                                                     COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        else
            wv->AddWebResourceRequestedFilter(pattern, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    }
    wv->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                     [this, alive_](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                         if (!*alive_ || !env)
                                             return S_OK;
                                         ComPtr<ICoreWebView2WebResourceRequest> request;
                                         if (FAILED(args->get_Request(&request)) || !request)
                                             return S_OK;
                                         LPWSTR uri = nullptr;
                                         request->get_Uri(&uri);
                                         const std::string url = take_string(uri);
                                         untrusted::Url    u;
                                         const bool        web     = untrusted::parse_url(url, u);
                                         const bool        blocked = !web ? url.rfind("file:", 0) == 0 || url.rfind("FILE:", 0) == 0
                                                                          : (is_blocked_host(u.host) || u.scheme == "file");
                                         if (blocked) {
                                             ComPtr<ICoreWebView2WebResourceResponse> response;
                                             env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"Content-Type: text/plain", &response);
                                             args->put_Response(response.Get());
                                             BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: blocked a request to " << (web ? u.host : std::string("a local file"));
                                         }
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);

    if (popup == nullptr) {
        wv->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>([this, alive_](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                  if (*alive_)
                                      update_address(source_of(sender));
                                  return S_OK;
                              }).Get(),
                              &token);
        wv->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>([this, alive_](ICoreWebView2*, IUnknown*) -> HRESULT {
                                   if (*alive_)
                                       update_nav_buttons();
                                   return S_OK;
                               }).Get(),
                               &token);
        wv->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                  [this, alive_](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                      if (!*alive_)
                                          return S_OK;
                                      COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                      args->get_ProcessFailedKind(&kind);
                                      if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                          owner->CallAfter([this]() {
                                              for (Popup& p : popups)
                                                  if (p.frame)
                                                      p.frame->Destroy();
                                              popups.clear();
                                              env.Reset();
                                              fail(_L("The model browser stopped."));
                                          });
                                      } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && view) {
                                          view->Reload();
                                      }
                                      return S_OK;
                                  })
                                  .Get(),
                              &token);
    } else {
        wxFrame* frame = popup;
        wv->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>([alive_, frame](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                         if (!*alive_)
                                             return S_OK;
                                         LPWSTR title = nullptr;
                                         if (SUCCEEDED(sender->get_DocumentTitle(&title))) {
                                             const std::string t = take_string(title);
                                             if (!t.empty())
                                                 frame->SetTitle(from_u8(t));
                                         }
                                         return S_OK;
                                     }).Get(),
                                     &token);
        // window.close() from the sign-in page.
        wv->add_WindowCloseRequested(Callback<ICoreWebView2WindowCloseRequestedEventHandler>([this, alive_, frame](ICoreWebView2*, IUnknown*) -> HRESULT {
                                         if (*alive_)
                                             owner->CallAfter([this, frame]() { close_popup(frame); });
                                         return S_OK;
                                     }).Get(),
                                     &token);
    }
}

void ModelBrowserPanel::Impl::apply_theme_to_browser()
{
    if (!view)
        return;
    ComPtr<ICoreWebView2_13> v13;
    if (SUCCEEDED(view.As(&v13))) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(v13->get_Profile(&profile)) && profile)
            profile->put_PreferredColorScheme(wxGetApp().dark_mode() ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                                                                     : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
    }
}

void ModelBrowserPanel::Impl::resize()
{
    if (!controller || host == nullptr)
        return;
    RECT r;
    ::GetClientRect((HWND) host->GetHWND(), &r);
    controller->put_Bounds(r);
}

void ModelBrowserPanel::Impl::navigate(const std::string& url)
{
    if (!view)
        return;
    if (decide_navigation(url, current_url(), true).action != NavAction::Allow)
        return;
    view->Navigate(boost::nowide::widen(url).c_str());
}

void ModelBrowserPanel::Impl::go_site(const std::string& id)
{
    const Site* s = site_by_id(id);
    if (s == nullptr)
        return;
    site_id = s->id;
    wxGetApp().app_config->set("model_browser_site", site_id);
    apply_colours();
    hide_notice();
    if (view)
        navigate(s->start_url);
    else if (!creating)
        create_browser();
}

// What a refused navigation turns into.
void ModelBrowserPanel::Impl::carry_out(const NavDecision& d, const std::string& url, bool user_visible)
{
    switch (d.action) {
    case NavAction::Allow: return;
    case NavAction::OpenLink: {
        // Pages may fire a link twice (location + iframe, or NavigationStarting and then the
        // external-scheme event): one download per click.
        const auto now = std::chrono::steady_clock::now();
        if (d.open_link == last_app_link && now - last_app_link_time < std::chrono::seconds(3))
            return;
        last_app_link      = d.open_link;
        last_app_link_time = now;
        BOOST_LOG_TRIVIAL(info) << "ModelBrowser: \"Open in\" link handed to the downloader (" << d.reason << ")";
        const std::string link = d.open_link;
        owner->CallAfter([link]() { wxGetApp().start_download(link); });
        return;
    }
    case NavAction::Block: {
        untrusted::Url u;
        const bool     web = untrusted::parse_url(url, u);
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: blocked a navigation (" << d.reason << ")" << (web ? " to " + u.host : std::string());
        if (!user_visible)
            return;
        const wxString text = (web && is_blocked_host(u.host)) ?
                                  _L("The model browser does not open addresses on this computer or your local network.") :
                                  format_wxstr(_L("The model browser did not open this link: %1%"), from_u8(d.reason));
        owner->CallAfter([this, text]() { show_notice(text, NoticeKind::Warning); });
        return;
    }
    }
}

// ------------------------------------------------------------------------------ new windows ----

void ModelBrowserPanel::Impl::handle_new_window(ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args)
{
    LPWSTR uri = nullptr;
    args->get_Uri(&uri);
    const std::string target = take_string(uri);
    BOOL user = FALSE;
    args->get_IsUserInitiated(&user);
    bool                                    sized = false;
    wxSize                                  size  = owner->FromDIP(wxSize(520, 720));
    ComPtr<ICoreWebView2WindowFeatures>     features;
    if (SUCCEEDED(args->get_WindowFeatures(&features)) && features) {
        BOOL has_size = FALSE, has_position = FALSE;
        features->get_HasSize(&has_size);
        features->get_HasPosition(&has_position);
        sized = has_size == TRUE || has_position == TRUE;
        if (has_size == TRUE) {
            UINT32 w = 0, h = 0;
            features->get_Width(&w);
            features->get_Height(&h);
            if (w >= 200 && h >= 200 && w <= 2000 && h <= 2000)
                size = owner->FromDIP(wxSize(int(w), int(h)));
        }
    }
    const WindowDecision w = decide_new_window(target, source_of(sender), user == TRUE, sized);
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: new window, " << w.reason;
    switch (w.action) {
    case WindowAction::Popup: {
        if (!env) {
            args->put_Handled(TRUE);
            return;
        }
        ComPtr<ICoreWebView2Deferral> deferral;
        if (FAILED(args->GetDeferral(&deferral))) {
            args->put_Handled(TRUE);
            return;
        }
        open_popup(ComPtr<ICoreWebView2NewWindowRequestedEventArgs>(args), deferral, size);
        return;
    }
    case WindowAction::NavigateInPlace:
        args->put_Handled(TRUE);
        if (view)
            view->Navigate(boost::nowide::widen(target).c_str());
        return;
    case WindowAction::SystemBrowser:
        args->put_Handled(TRUE);
        if (untrusted::is_safe_to_open_externally(target)) {
            const wxString url = from_u8(target);
            owner->CallAfter([url]() { wxLaunchDefaultBrowser(url); });
        }
        return;
    case WindowAction::AppLink:
        args->put_Handled(TRUE);
        carry_out(w.nav, target, true);
        return;
    case WindowAction::Block:
    default: args->put_Handled(TRUE); return;
    }
}

void ModelBrowserPanel::Impl::open_popup(ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args, ComPtr<ICoreWebView2Deferral> deferral, const wxSize& size)
{
    wxWindow* top   = wxGetTopLevelParent(owner);
    auto*     frame = new wxFrame(top, wxID_ANY, _L("Sign in"), wxDefaultPosition, size, wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
    frame->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    frame->CentreOnParent();
    frame->Show();

    auto alive_ = alive;
    frame->Bind(wxEVT_CLOSE_WINDOW, [this, alive_, frame](wxCloseEvent&) {
        // A popup still being created is not in the list yet: just close its window.
        if (!*alive_ || !close_popup(frame))
            frame->Destroy();
    });
    frame->Bind(wxEVT_SIZE, [this, frame](wxSizeEvent& evt) {
        evt.Skip();
        for (Popup& p : popups)
            if (p.frame == frame && p.controller) {
                RECT r;
                ::GetClientRect((HWND) frame->GetHWND(), &r);
                p.controller->put_Bounds(r);
            }
    });

    wxWeakRef<wxFrame> weak(frame);
    const HRESULT      hr = env->CreateCoreWebView2Controller(
        (HWND) frame->GetHWND(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, alive_, weak, args, deferral](HRESULT result, ICoreWebView2Controller* created) -> HRESULT {
                wxFrame* frame = weak.get();
                if (!*alive_ || frame == nullptr) { // closed while it was being made
                    if (created)
                        created->Close();
                    args->put_Handled(TRUE);
                    deferral->Complete();
                    return S_OK;
                }
                Popup p;
                p.frame = frame;
                if (SUCCEEDED(result) && created != nullptr) {
                    p.controller = created;
                    p.controller->get_CoreWebView2(&p.view);
                }
                if (!p.view || !configure(p.view.Get())) {
                    if (p.controller)
                        p.controller->Close();
                    args->put_Handled(TRUE);
                    deferral->Complete();
                    frame->Destroy();
                    BOOST_LOG_TRIVIAL(error) << "ModelBrowser: a popup could not be created safely";
                    return S_OK;
                }
                attach_handlers(p.view.Get(), frame);
                RECT r;
                ::GetClientRect((HWND) frame->GetHWND(), &r);
                p.controller->put_Bounds(r);
                p.controller->put_IsVisible(TRUE);
                popups.push_back(p);
                args->put_NewWindow(p.view.Get());
                args->put_Handled(TRUE);
                deferral->Complete();
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        args->put_Handled(TRUE);
        deferral->Complete();
        frame->Destroy();
    }
}

bool ModelBrowserPanel::Impl::close_popup(wxFrame* frame)
{
    for (auto it = popups.begin(); it != popups.end(); ++it)
        if (it->frame == frame) {
            if (it->controller)
                it->controller->Close();
            popups.erase(it);
            frame->Destroy();
            return true;
        }
    return false;
}

// -------------------------------------------------------------------------------- downloads ----

void ModelBrowserPanel::Impl::handle_download(ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args)
{
    ComPtr<ICoreWebView2DownloadOperation> op;
    if (FAILED(args->get_DownloadOperation(&op)) || !op) {
        args->put_Cancel(TRUE);
        return;
    }
    LPWSTR uri = nullptr, suggested = nullptr;
    op->get_Uri(&uri);
    args->get_ResultFilePath(&suggested);
    INT64 total = -1;
    op->get_TotalBytesToReceive(&total);
    const std::string      url  = take_string(uri);
    const std::string      page = source_of(sender);
    const DownloadDecision d    = decide_download(url, leaf_of(take_string(suggested)), page, total);
    // Our own progress line and notices instead of the browser's download bubble.
    args->put_Handled(TRUE);
    untrusted::Url u;
    const std::string host = untrusted::parse_url(url, u) ? u.host : untrusted::parse_url(page, u) ? u.host : std::string();
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: download of \"" << d.file_name << "\" from " << host << ": " << d.reason;

    switch (d.action) {
    case DownloadAction::Import: {
        const fs::path folder = model_folder();
        if (folder.empty() || !begin_download(args, op.Get(), d.file_name, folder, true)) {
            args->put_Cancel(TRUE);
            show_notice(format_wxstr(_L("\"%1%\" could not be saved: no usable download folder."), from_u8(d.file_name)), NoticeKind::Error);
        }
        return;
    }
    case DownloadAction::Refuse:
        args->put_Cancel(TRUE);
        show_notice(format_wxstr(_L("The download of \"%1%\" was refused: %2%"), from_u8(d.file_name), from_u8(d.reason)), NoticeKind::Error);
        return;
    case DownloadAction::ImportAfterConfirm:
    case DownloadAction::OfferSave: {
        // Ask outside the browser's callback; the download waits for the answer.
        ComPtr<ICoreWebView2Deferral> deferral;
        if (FAILED(args->GetDeferral(&deferral))) {
            args->put_Cancel(TRUE);
            return;
        }
        ComPtr<ICoreWebView2DownloadStartingEventArgs> keep(args);
        const bool                                     to_import = d.action == DownloadAction::ImportAfterConfirm;
        const std::string                              name   = d.file_name;
        auto                                           alive_ = alive;
        owner->CallAfter([this, alive_, keep, deferral, op, to_import, name, host]() {
            if (!*alive_) {
                keep->put_Cancel(TRUE);
                deferral->Complete();
                return;
            }
            const fs::path folder = to_import ? model_folder() : downloads_folder();
            const wxString msg = to_import ?
                format_wxstr(_L("\"%1%\" comes from %2%, which is not one of the model sites EdgeSlicer knows.\n\n"
                                "Only open it if you trust this site. Download and open it?"), from_u8(name), from_u8(host)) :
                format_wxstr(_L("\"%1%\" is not a model file.\n\nSave it to your Downloads folder?"), from_u8(name));
            MessageDialog dlg(wxGetTopLevelParent(owner), msg, to_import ? _L("Download from an unknown site") : _L("Save file"),
                              wxYES_NO | wxICON_QUESTION);
            dlg.SetButtonLabel(wxID_YES, to_import ? _L("Download and open") : _L("Save"));
            dlg.SetButtonLabel(wxID_NO, _L("Cancel"), true);
            const bool yes = dlg.ShowModal() == wxID_YES;
            if (!yes || folder.empty() || !begin_download(keep.Get(), op.Get(), name, folder, to_import))
                keep->put_Cancel(TRUE);
            deferral->Complete();
        });
        return;
    }
    }
}

// Claims a name in the folder (exclusive marker, the same scheme as the "Open in" downloads) and
// points the browser at a temporary file next to it; the file is checked and moved into place
// without replacing anything when it is complete.
bool ModelBrowserPanel::Impl::begin_download(ICoreWebView2DownloadStartingEventArgs* args, ICoreWebView2DownloadOperation* op,
                                             const std::string& name, const fs::path& folder, bool to_import)
{
    std::string claimed;
    FILE*       marker_file = untrusted::claim_unused_download_name(folder, name, {}, claimed);
    if (marker_file == nullptr)
        return false;
    fclose(marker_file);

    Download dl;
    dl.op     = op;
    dl.folder = folder;
    dl.name   = claimed;
    dl.to_import = to_import;
    dl.marker = untrusted::download_marker_path(folder, claimed);
    boost::system::error_code ec;
    for (int i = 0; i < 100; ++i) {
        fs::path part = dl.marker;
        part += i == 0 ? std::string(".part") : ".part" + std::to_string(i);
        if (!fs::exists(fs::symlink_status(part, ec))) {
            dl.part = part;
            break;
        }
    }
    if (dl.part.empty() || FAILED(args->put_ResultFilePath(dl.part.wstring().c_str()))) {
        fs::remove(dl.marker, ec);
        return false;
    }

    const int id = next_download++;
    auto      alive_ = alive;
    op->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>([this, alive_, id](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
                             if (*alive_)
                                 on_download_state(id);
                             return S_OK;
                         }).Get(),
                         &dl.state_token);
    op->add_BytesReceivedChanged(Callback<ICoreWebView2BytesReceivedChangedEventHandler>([this, alive_, id](ICoreWebView2DownloadOperation*, IUnknown*) -> HRESULT {
                                     if (*alive_)
                                         on_download_bytes(id);
                                     return S_OK;
                                 }).Get(),
                                 &dl.bytes_token);
    downloads[id] = std::move(dl);
    status->SetLabel(format_wxstr(_L("Downloading %1%..."), from_u8(claimed)));
    toolbar->Layout();
    return true;
}

void ModelBrowserPanel::Impl::on_download_bytes(int id)
{
    auto it = downloads.find(id);
    if (it == downloads.end())
        return;
    INT64 got = 0, total = -1;
    it->second.op->get_BytesReceived(&got);
    it->second.op->get_TotalBytesToReceive(&total);
    if (total > 0 && got > total)
        got = total;
    status->SetLabel(total > 0 ? format_wxstr(_L("Downloading %1%... %2%%%"), from_u8(it->second.name), int(got * 100 / total)) :
                                 format_wxstr(_L("Downloading %1%... %2% MB"), from_u8(it->second.name), int(got / (1024 * 1024))));
    toolbar->Layout();
}

void ModelBrowserPanel::Impl::on_download_state(int id)
{
    auto it = downloads.find(id);
    if (it == downloads.end())
        return;
    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    it->second.op->get_State(&state);
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS)
        return;
    Download dl = std::move(it->second);
    downloads.erase(it);
    dl.op->remove_StateChanged(dl.state_token);
    dl.op->remove_BytesReceivedChanged(dl.bytes_token);
    if (downloads.empty())
        status->SetLabel(wxEmptyString);
    boost::system::error_code ec;

    if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
        dl.op->get_InterruptReason(&reason);
        fs::remove(dl.part, ec);
        fs::remove(dl.marker, ec);
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: download of \"" << dl.name << "\" interrupted, reason " << int(reason);
        if (reason != COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED && reason != COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN)
            show_notice(format_wxstr(_L("The download of \"%1%\" failed."), from_u8(dl.name)), NoticeKind::Error);
        return;
    }
    LPWSTR result = nullptr;
    dl.op->get_ResultFilePath(&result);
    finish_download(id, std::move(dl), take_string(result));
}

void ModelBrowserPanel::Impl::finish_download(int /*id*/, Download dl, const std::string& result_path)
{
    boost::system::error_code ec;
    fs::path                  file = result_path.empty() ? dl.part : fs::path(boost::nowide::widen(result_path));
    auto                      drop = [&](const wxString& why) {
        fs::remove(file, ec);
        fs::remove(dl.marker, ec);
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: dropped \"" << dl.name << "\": " << into_u8(why);
        show_notice(format_wxstr(_L("\"%1%\" was not opened: %2%"), from_u8(dl.name), why), NoticeKind::Error);
    };
    // The browser must have written where we told it to, inside the folder.
    if (!untrusted::is_path_within_root(dl.folder, file)) {
        drop(_L("it was saved outside the download folder"));
        return;
    }
    if (dl.to_import) {
        const uintmax_t size = fs::file_size(file, ec);
        if (ec || size == 0) {
            drop(_L("the file is empty"));
            return;
        }
        if (size > untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) {
            drop(_L("the file is larger than the 500 MB limit for models"));
            return;
        }
        std::string head(512, '\0');
        {
            boost::nowide::ifstream in(file.string(), std::ios::binary);
            in.read(&head[0], std::streamsize(head.size()));
            head.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
        }
        std::string why;
        if (!untrusted::content_matches_extension(dl.name, head, size, &why)) {
            drop(format_wxstr(_L("its content does not match its type (%1%)"), from_u8(why)));
            return;
        }
    }
    std::string name = dl.name;
    fs::path    dest;
    if (!untrusted::place_download_file(file, dl.folder, name, dest, ec)) {
        drop(_L("it could not be moved into the download folder"));
        return;
    }
    fs::remove(dl.marker, ec);
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: saved " << dest.string();
    if (dl.to_import) {
        enqueue_import(dest);
    } else {
        show_notice(format_wxstr(_L("Saved \"%1%\" to your Downloads folder."), from_u8(name)), NoticeKind::Info, _L("Show in folder"),
                    [dest]() { desktop_open_any_folderEx(into_u8(from_path(dest))); });
    }
}

// ----------------------------------------------------------------------------------- import ----

// Imports one file at a time: a project load can show dialogs, and further downloads that finish
// meanwhile wait their turn instead of starting a second load inside the first.
void ModelBrowserPanel::Impl::enqueue_import(const fs::path& path)
{
    import_queue.push_back(path);
    owner->CallAfter([this]() { process_imports(); });
}

void ModelBrowserPanel::Impl::process_imports()
{
    if (importing || import_queue.empty())
        return;
    Plater*    plater = wxGetApp().plater();
    MainFrame* frame  = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr)
        return;
    importing           = true;
    const fs::path path = import_queue.front();
    import_queue.pop_front();
    frame->select_tab(size_t(MainFrame::tp3DEditor));
    wxGetApp().mark_web_download(path);
    wxArrayString files;
    files.Add(from_path(path));
    // from_url: the user downloaded this specific file. Projects follow the Load Behaviour setting;
    // their settings go through the untrusted-settings check (UntrustedSettingsGuard) as always.
    plater->load_files(files, /* from_url */ true);
    importing = false;
    if (!import_queue.empty())
        owner->CallAfter([this]() { process_imports(); });
}

ModelBrowserPanel::Impl::~Impl()
{
    *alive = false;
    notice_timer.Stop();
    for (auto& [id, dl] : downloads) {
        if (dl.op) {
            dl.op->remove_StateChanged(dl.state_token);
            dl.op->remove_BytesReceivedChanged(dl.bytes_token);
            dl.op->Cancel();
        }
        boost::system::error_code ec;
        fs::remove(dl.marker, ec);
    }
    downloads.clear();
    for (Popup& p : popups) {
        if (p.controller)
            p.controller->Close();
        if (p.frame)
            p.frame->Destroy();
    }
    popups.clear();
    if (controller)
        controller->Close();
    controller.Reset();
    view.Reset();
    env.Reset();
}

#else // !_WIN32

struct ModelBrowserPanel::Impl
{
    explicit Impl(ModelBrowserPanel*, std::function<void()>) {}
};

#endif // _WIN32

// ---------------------------------------------------------------------------------- panel ----

ModelBrowserPanel::ModelBrowserPanel(wxWindow* parent, std::function<void()> on_leave)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , p(std::make_unique<Impl>(this, std::move(on_leave)))
{
#ifdef _WIN32
    const std::string saved = wxGetApp().app_config->get("model_browser_site");
    if (site_by_id(saved) != nullptr)
        p->site_id = saved;
    p->build_ui();
    // Moved to a monitor with another scale: the toolbar re-measures; the browser follows the
    // monitor's scale itself (its bounds are in the host window's physical pixels).
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
        evt.Skip();
        CallAfter([this]() {
            p->apply_colours();
            p->resize();
        });
    });
#endif
}

ModelBrowserPanel::~ModelBrowserPanel() = default;

void ModelBrowserPanel::set_active(bool active)
{
#ifdef _WIN32
    p->active = active;
    if (active && !p->controller && !p->creating)
        p->create_browser();
    if (p->controller)
        p->controller->put_IsVisible(active ? TRUE : FALSE);
    if (active)
        p->resize();
#else
    (void) active;
#endif
}

void ModelBrowserPanel::sys_color_changed()
{
#ifdef _WIN32
    p->apply_colours();
    p->apply_theme_to_browser();
#endif
}

} // namespace GUI
} // namespace Slic3r
