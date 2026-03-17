#include "OrcaLocalHandler.hpp"

#include "slic3r/GUI/GUI.hpp"

#include <wx/filename.h>
#include <wx/file.h>
#include <wx/uri.h>
#include <wx/wfstream.h>

namespace Slic3r {
namespace GUI {

OrcaLocalHandler::OrcaLocalHandler(const wxString& scheme, const wxString& rootPath)
    : wxWebViewHandler(scheme)
{
    m_root = rootPath;
    if (!m_root.empty() && m_root.Last() != wxFileName::GetPathSeparator())
        m_root += wxFileName::GetPathSeparator();
}

wxFSFile* OrcaLocalHandler::GetFile(const wxString& uri)
{
    wxURI u(uri);
    wxString path = u.GetPath();

    // Debug: log the request
    BOOST_LOG_TRIVIAL(info) << "OrcaLocalHandler::GetFile uri=" << uri.ToUTF8() << " path=" << path.ToUTF8() << " root=" << m_root.ToUTF8();

    if (path.empty())
        return nullptr;

    // Remove leading slash
    while (path.Length() > 0 && (path[0] == '/' || path[0] == '\\'))
        path = path.Mid(1);

    // Basic path traversal guard
    if (path.Contains(".."))
        return nullptr;

    path.Replace("/", wxString(wxFileName::GetPathSeparator()));
    wxString fullPath = m_root + path;

    if (!wxFileExists(fullPath))
        return nullptr;

    wxFileInputStream* stream = new wxFileInputStream(fullPath);
    if (!stream->IsOk()) {
        delete stream;
        return nullptr;
    }

    wxString mime = "application/octet-stream";
    if (path.Lower().EndsWith(".html") || path.Lower().EndsWith(".htm"))
        mime = "text/html; charset=utf-8";
    else if (path.Lower().EndsWith(".css"))
        mime = "text/css";
    else if (path.Lower().EndsWith(".js"))
        mime = "application/javascript";
    else if (path.Lower().EndsWith(".json"))
        mime = "application/json";
    else if (path.Lower().EndsWith(".wasm"))
        mime = "application/wasm";
    else if (path.Lower().EndsWith(".png"))
        mime = "image/png";
    else if (path.Lower().EndsWith(".jpg") || path.Lower().EndsWith(".jpeg"))
        mime = "image/jpeg";
    else if (path.Lower().EndsWith(".svg"))
        mime = "image/svg+xml";
    else if (path.Lower().EndsWith(".ttf"))
        mime = "font/ttf";
    else if (path.Lower().EndsWith(".woff"))
        mime = "font/woff";
    else if (path.Lower().EndsWith(".woff2"))
        mime = "font/woff2";
    else if (path.Lower().EndsWith(".ico"))
        mime = "image/x-icon";

    return new wxFSFile(stream, uri, mime, wxEmptyString, wxDateTime::Now());
}

} // namespace GUI
} // namespace Slic3r
