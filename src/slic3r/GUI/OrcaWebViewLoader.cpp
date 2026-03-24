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

    const fs::path res_path = fs::path(Slic3r::resources_dir()) / "web" / "flutter_web";
    fs::path user_path;
    if (!user_web_path.empty())
        user_path = fs::path(user_web_path.ToUTF8().data());

    if (!user_web_path.empty() && fs::exists(user_path / "index.html")) {
        config.root_path = wxString::FromUTF8(user_path.string());
        config.entry_url = "orca://app/index.html";
        BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: using " << user_path.string();
    } else if (fs::exists(res_path / "index.html")) {
        config.root_path = wxString::FromUTF8(Slic3r::resources_dir());
        config.entry_url = "orca://app/web/flutter_web/index.html";
        BOOST_LOG_TRIVIAL(info) << "OrcaWebViewLoader: using resources_dir()/web/flutter_web";
    } else if (!user_web_path.empty()) {
        config.root_path = wxString::FromUTF8(user_path.string());
        config.entry_url = "orca://app/index.html";
        BOOST_LOG_TRIVIAL(warning) << "OrcaWebViewLoader: neither path found, trying " << user_path.string();
    } else {
        config.root_path = wxString::FromUTF8(Slic3r::resources_dir());
        config.entry_url = "orca://app/web/flutter_web/index.html";
        BOOST_LOG_TRIVIAL(warning) << "OrcaWebViewLoader: resources flutter_web not found";
    }

    config.user_assets_dir = EnsureUserAssetsDir();
    return config;
}

wxString OrcaWebViewLoader::EnsureUserAssetsDir()
{
    wxString dir = wxStandardPaths::Get().GetTempDir() + wxFileName::GetPathSeparator() + "orca_user_assets";
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, 0755, wxPATH_MKDIR_FULL);
    return dir;
}

wxString OrcaWebViewLoader::BuildFullEntryUrl(const OrcaWebLoadConfig& config)
{
    wxString url = config.entry_url + "?";
    if (!config.route_params.empty())
        url += config.route_params + "&";
    url += "path=" + config.route_path;
    return url;
}

wxString OrcaWebViewLoader::BuildBaseUrl(const OrcaWebLoadConfig& config)
{
    // Flutter 要求 base href 必须以 "/" 结尾，否则报错：
    // "The base href has to end with a '/' to work correctly"
    // base 仅用于解析相对路径（如 main.dart.js），path/locale 等参数通过 LoadURL 的完整 URL 传递
    return "orca://app/";
}

wxString OrcaWebViewLoader::ReadIndexHtml(const OrcaWebLoadConfig& config)
{
    // entry_url: "orca://app/index.html" 或 "orca://app/web/flutter_web/index.html"
    wxString rel = config.entry_url;
    if (rel.StartsWith("orca://app/"))
        rel = rel.Mid(10); // 去掉 "orca://app/"
    rel.Replace("/", wxString(wxFileName::GetPathSeparator()));

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
    // 非 Windows: 直接 SetPage，orca:// handler 可用
    wxString html     = ReadIndexHtml(config);
    wxString base_url = BuildBaseUrl(config);
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
