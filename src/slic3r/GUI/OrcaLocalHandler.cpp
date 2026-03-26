#include "OrcaLocalHandler.hpp"

#include "slic3r/GUI/GUI.hpp"

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/uri.h>
#include <wx/wfstream.h>

namespace Slic3r {
namespace GUI {

OrcaLocalHandler::OrcaLocalHandler(const wxString& scheme, const wxString& rootPath,
                                   const wxString& userAssetsRoot)
    : wxWebViewHandler(scheme)
{
    m_root = rootPath;
    if (!m_root.empty() && m_root.Last() != wxFileName::GetPathSeparator())
        m_root += wxFileName::GetPathSeparator();
    m_userAssetsRoot = userAssetsRoot;
    if (!m_userAssetsRoot.empty() && m_userAssetsRoot.Last() != wxFileName::GetPathSeparator())
        m_userAssetsRoot += wxFileName::GetPathSeparator();
}

wxFSFile* OrcaLocalHandler::GetFile(const wxString& uri)
{
    wxURI u(uri);
    wxString path = u.GetPath();

    if (path.empty())
        return nullptr;

    // Remove leading slash
    while (path.Length() > 0 && (path[0] == '/' || path[0] == '\\'))
        path = path.Mid(1);

    // Basic path traversal guard
    if (path.Contains(".."))
        return nullptr;

    wxString fullPath;
    wxString pathForMime = path;

    // orca://app/user_assets/xxx -> m_userAssetsRoot/xxx
    if (!m_userAssetsRoot.empty() && path.Lower().StartsWith("user_assets")) {
        wxString sub = path.Mid(11);  // skip "user_assets"
        while (sub.Length() > 0 && (sub[0] == '/' || sub[0] == '\\'))
            sub = sub.Mid(1);
        if (sub.Contains("..")) return nullptr;
        sub.Replace("/", wxString(wxFileName::GetPathSeparator()));
        fullPath = m_userAssetsRoot + sub;
    } else {
        path.Replace("/", wxString(wxFileName::GetPathSeparator()));
        fullPath = m_root + path;
    }

    BOOST_LOG_TRIVIAL(trace) << "OrcaLocalHandler::GetFile uri=" << uri.ToUTF8() << " fullPath=" << fullPath.ToUTF8();

    // 入口 URL 为 .../web/flutter_web/?query 时，路径指向目录，需映射到 index.html
    // 使用 wxFileName::DirExists() 实例方法：部分工具链下静态 wxFileName::DirExists(path) 不可用或声明不一致
    if (!wxFileExists(fullPath)) {
        wxFileName asDir(fullPath);
        if (asDir.DirExists()) {
            // GetPathSeparator() 为 wxUniChar，不可与 "index.html" 直接 +，否则重载歧义
            fullPath.Append(wxFileName::GetPathSeparator());
            fullPath += "index.html";
            pathForMime = "index.html";
        }
    }

    if (!wxFileExists(fullPath)) {
        // SPA fallback：如果路径没有文件扩展名（说明是客户端路由如 /bridge），
        // 回退到同级目录或上级目录的 index.html，让 Flutter 路由器处理
        if (!pathForMime.Contains(".")) {
            wxFileName fn(fullPath);
            wxString dir = fn.GetPath();
            wxString fallbackPath = dir;
            fallbackPath.Append(wxFileName::GetPathSeparator());
            fallbackPath += "index.html";
            if (!wxFileExists(fallbackPath)) {
                // 再尝试 rootPath/index.html
                fallbackPath = m_root + "index.html";
            }
            if (wxFileExists(fallbackPath)) {
                BOOST_LOG_TRIVIAL(info) << "OrcaLocalHandler: SPA fallback -> " << fallbackPath.ToUTF8();
                fullPath = fallbackPath;
                pathForMime = "index.html";
            } else {
                return nullptr;
            }
        } else {
            return nullptr;
        }
    }

    wxFileInputStream* stream = new wxFileInputStream(fullPath);
    if (!stream->IsOk()) {
        delete stream;
        return nullptr;
    }

    wxString mime = "application/octet-stream";
    if (pathForMime.Lower().EndsWith(".html") || pathForMime.Lower().EndsWith(".htm"))
        mime = "text/html; charset=utf-8";
    else if (pathForMime.Lower().EndsWith(".css"))
        mime = "text/css";
    else if (pathForMime.Lower().EndsWith(".js"))
        mime = "application/javascript";
    else if (pathForMime.Lower().EndsWith(".json"))
        mime = "application/json";
    else if (pathForMime.Lower().EndsWith(".wasm"))
        mime = "application/wasm";
    else if (pathForMime.Lower().EndsWith(".png"))
        mime = "image/png";
    else if (pathForMime.Lower().EndsWith(".jpg") || pathForMime.Lower().EndsWith(".jpeg"))
        mime = "image/jpeg";
    else if (pathForMime.Lower().EndsWith(".svg"))
        mime = "image/svg+xml";
    else if (pathForMime.Lower().EndsWith(".ttf"))
        mime = "font/ttf";
    else if (pathForMime.Lower().EndsWith(".woff"))
        mime = "font/woff";
    else if (pathForMime.Lower().EndsWith(".woff2"))
        mime = "font/woff2";
    else if (pathForMime.Lower().EndsWith(".ico"))
        mime = "image/x-icon";

    wxFSFile * ff = new wxFSFile(stream, uri, mime, wxEmptyString, wxDateTime::Now());
    
    return ff;
}

} // namespace GUI
} // namespace Slic3r
