#include "OrcaWebViewLoader.hpp"
#include "libslic3r/Utils.hpp"
#include "GUI_App.hpp"
#include <boost/filesystem.hpp>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;

wxString OrcaWebViewLoader::BuildRouteParamsFromApp()
{
    Slic3r::GUI::GUI_App& app = wxGetApp();
    if (!app.app_config)
        return "locale=zh-cn-US&dark_mode=1";
    wxString lang   = wxString::FromUTF8(app.app_config->get_language_code());
    wxString region = wxString::FromUTF8(app.app_config->get_country_code());
    if (region == "Others")
        region = "US";
    std::string dark_mode = app.app_config->get("dark_color_mode");
    return "locale=" + lang + "-" + region + "&dark_mode=" + dark_mode;
}

wxString OrcaWebViewLoader::FlutterPathQueryValue(const wxString& page_id)
{
    // 值必须不含未编码的 '/'。形如 path=/preUploadAndPrint 时，部分 WebKit/wxURI 会把
    // '/preUpload...' 误当作层次路径，查询串被截断，Flutter GoRouter 读到 path=null → Not Found。
    // GoRouter redirect 会在缺省时补上前导 '/'（见 main.dart.js orcaRouter_closure7）。
    wxString segment;
    if (page_id == wxS("2"))
        segment = wxS("deviceControl");
    else if (page_id == wxS("1"))
        segment = wxS("home");
    else if (page_id == wxS("4"))
        segment = wxS("preUpload");
    else if (page_id == wxS("5"))
        segment = wxS("preUploadAndPrint");
    else
        segment = wxS("home");
    return segment;
}

OrcaWebLoadConfig OrcaWebViewLoader::CreateConfigForPage(int path)
{
    wxString user_path;
    // 可从 app_config 读取开发路径覆盖，这里优先 resources
    if (wxGetApp().app_config) {
        std::string dev_path = wxGetApp().app_config->get("web_dev_path");
        if (!dev_path.empty())
            user_path = wxString::FromUTF8(dev_path);
    }
    OrcaWebLoadConfig config = ResolveConfig(user_path);
    config.route_path   = wxString::Format("%d", path);
    config.route_params = BuildRouteParamsFromApp();
    return config;
}

OrcaWebLoadConfig OrcaWebViewLoader::ResolveConfig(const wxString& user_web_path)
{
    OrcaWebLoadConfig config;
    config.route_path   = "1";
    config.route_params = "locale=zh-cn&dark_mode=1";

    const fs::path res_test_web = fs::path(Slic3r::resources_dir()) / "test_web";
    const fs::path res_path     = fs::path(Slic3r::resources_dir()) / "web" / "flutter_web";
    fs::path user_path;
    if (!user_web_path.empty())
        user_path = fs::path(user_web_path.ToUTF8().data());

    // entry_url 必须用「目录 + /」而不是 .../index.html：Flutter Web 的 PathUrlStrategy
    // 用 pathname 去掉 <base> 路径前缀后交给 GoRouter；若 URL 含 index.html，会得到
    // /index.html?query，无法匹配 /，redirect(path 查询) 也不触发 → Not Found Page。
    if (fs::exists(res_test_web / "index.html")) {
        config.root_path = wxString::FromUTF8(res_test_web.string());
        config.entry_url = "orca://app/";
        BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: using resources/test_web";
    } else if (!user_web_path.empty() && fs::exists(user_path / "index.html")) {
        config.root_path = wxString::FromUTF8(user_path.string());
        config.entry_url = "orca://app/";
        BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: using " << user_path.string();
    } else if (fs::exists(res_path / "index.html")) {
        config.root_path = wxString::FromUTF8(Slic3r::resources_dir());
        config.entry_url = "orca://app/web/flutter_web/";
        BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: using resources_dir()/web/flutter_web";
    } else if (!user_web_path.empty()) {
        config.root_path = wxString::FromUTF8(user_path.string());
        config.entry_url = "orca://app/";
        BOOST_LOG_TRIVIAL(warning) << "OrcaWebViewLoader: neither path found, trying " << user_path.string();
    } else {
        config.root_path = wxString::FromUTF8(Slic3r::resources_dir());
        config.entry_url = "orca://app/web/flutter_web/";
        BOOST_LOG_TRIVIAL(warning) << "OrcaWebViewLoader: resources flutter_web not found";
    }

    config.user_assets_dir = EnsureUserAssetsDir();
    return config;
}

wxString OrcaWebViewLoader::EnsureUserAssetsDir()
{
    wxString dir = wxStandardPaths::Get().GetTempDir() + wxFileName::GetPathSeparator() + "orca_user_assets";
    wxFileName userDir(dir);
    if (!userDir.DirExists())
        wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL);
    return dir;
}

wxString OrcaWebViewLoader::BuildFullEntryUrl(const OrcaWebLoadConfig& config)
{
    // entry_url 已带尾部 '/'，直接接 '?'；避免出现 .../web/flutter_web?（无斜杠）导致 pathname 异常
    wxString url = config.entry_url;
    if (!url.empty() && url.Last() != '/')
        url += "/";
    url += "?";
    if (!config.route_params.empty())
        url += config.route_params + "&";
    url += "path=" + FlutterPathQueryValue(config.route_path);
    return url;
}

wxString OrcaWebViewLoader::BuildBaseUrl(const OrcaWebLoadConfig& config)
{
    // Flutter 需从 document URL 读取 path 参数以切换页面，baseUrl 需包含 route 参数
    wxString base = "orca://app/?";
    if (!config.route_params.empty())
        base += config.route_params + "&";
    base += "path=" + FlutterPathQueryValue(config.route_path);
    return base;
}

wxString OrcaWebViewLoader::ReadIndexHtml(const OrcaWebLoadConfig& config)
{
    // entry_url: "orca://app/" 或 "orca://app/web/flutter_web/"（目录 URL，与 PathUrlStrategy 一致）
    wxString rel = config.entry_url;
    if (rel.StartsWith("orca://app"))
        rel = rel.Mid(10); // 去掉 "orca://app"（含其后第一个字符，一般为 '/'）
    while (!rel.empty() && (rel[0] == '/' || rel[0] == wxFileName::GetPathSeparator()))
        rel = rel.Mid(1);
    rel.Replace("/", wxString(wxFileName::GetPathSeparator()));
    if (rel.empty())
        rel = "index.html";
    else if (rel.Last() == wxFileName::GetPathSeparator())
        rel += "index.html";
    else if (!rel.Lower().EndsWith(".html") && !rel.Lower().EndsWith(".htm")) {
        rel.Append(wxFileName::GetPathSeparator());
        rel += "index.html";
    }

    fs::path html_path = fs::path(config.root_path.ToUTF8().data()) / rel.ToUTF8().data();
    wxString path_str = wxString::FromUTF8(html_path.string());
    wxString html;
    wxFile f(path_str, wxFile::read);
    if (f.IsOpened())
        f.ReadAll(&html);
    if (html.empty())
        html = "<html><body><p>index.html not found</p></body></html>";
    return html;
}

wxString OrcaWebViewLoader::LoadLocalHtml(wxWebView* webview, const OrcaWebLoadConfig& config)
{
    if (!webview)
        return wxEmptyString;

#ifdef __WIN32__
    // Windows: orca:// 通过 WebResourceRequested 处理，需在首次 EVT_SIZE 时 LoadURL
    wxString full_url = BuildFullEntryUrl(config);
    BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: Windows - deferred LoadURL on first EVT_SIZE: " << full_url.ToUTF8();
    return full_url;
#else
    // 非 Windows: SetPage 的 base_url 会成为 document URL；Reload 时 WebKit 会重新请求该 URL。
    // 入口须为目录 URL（.../web/flutter_web/?query），避免 pathname 含 index.html 导致 PathUrlStrategy
    // 交给 GoRouter 的路径变成 /index.html?... 而无法匹配 /。与 Windows 一致使用 BuildFullEntryUrl。
    wxString html     = ReadIndexHtml(config);
    wxString base_url = BuildFullEntryUrl(config);
    BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: SetPage with baseUrl=" << base_url.ToUTF8();
    LoadHtml(webview, html, base_url);
    return wxEmptyString;
#endif
}

void OrcaWebViewLoader::LoadHtml(wxWebView* webview, const wxString& html, const wxString& base_url)
{
    if (!webview)
        return;
    webview->SetPage(html.empty() ? "<html><body></body></html>" : html, base_url);
}

} // namespace GUI
} // namespace Slic3r
