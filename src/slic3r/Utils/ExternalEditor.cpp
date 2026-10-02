#include "ExternalEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <iterator>
#include <wx/utils.h>
#include <wx/string.h>
#else
#include <boost/process/args.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/spawn.hpp>
#endif

namespace Slic3r {
namespace ExternalEditor {

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool starts_with_nocase(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && to_lower(s.substr(0, prefix.size())) == to_lower(prefix);
}

bool ends_with_nocase(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

std::string file_name(const std::string &path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_dir(const std::string &path)
{
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string strip_trailing_separators(std::string path)
{
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

bool parse_dir_version(const std::string &dir_name, const std::string &prefix, std::vector<int> &version, size_t min_parts, size_t max_parts)
{
    if (!starts_with_nocase(dir_name, prefix))
        return false;
    const std::string rest = dir_name.substr(prefix.size());
    std::vector<int>  parts;
    size_t            start = 0;
    while (true) {
        size_t      dot  = rest.find('.', start);
        std::string part = rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return false;
        parts.push_back(std::atoi(part.c_str()));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    if (parts.size() < min_parts || parts.size() > max_parts)
        return false;
    version = std::move(parts);
    return true;
}

std::string newest_versioned_dir(const std::vector<std::string> &dir_names, const std::string &prefix, size_t min_parts, size_t max_parts)
{
    std::string      best;
    std::vector<int> best_version;
    for (const std::string &name : dir_names) {
        std::vector<int> version;
        if (!parse_dir_version(name, prefix, version, min_parts, max_parts))
            continue;
        if (best.empty() || std::lexicographical_compare(best_version.begin(), best_version.end(), version.begin(), version.end())) {
            best         = name;
            best_version = std::move(version);
        }
    }
    return best;
}

int compare_versions(const std::string &a, const std::string &b)
{
    auto split = [](const std::string &v) {
        std::vector<int> parts;
        size_t           start = 0;
        while (start <= v.size()) {
            size_t dot = v.find('.', start);
            parts.push_back(std::atoi(v.substr(start, dot == std::string::npos ? std::string::npos : dot - start).c_str()));
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
        return parts;
    };
    std::vector<int> va = split(a), vb = split(b);
    const size_t     n  = std::max(va.size(), vb.size());
    va.resize(n, 0);
    vb.resize(n, 0);
    return va < vb ? -1 : (vb < va ? 1 : 0);
}

// -------------------------------------------------------------------------------------------
// Platform adapters
// -------------------------------------------------------------------------------------------

namespace {

// Paths are UTF-8 throughout; only Windows needs them widened for the filesystem calls.
boost::filesystem::path to_path(const std::string &utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(utf8));
#else
    return boost::filesystem::path(utf8);
#endif
}

std::string from_path(const boost::filesystem::path &path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

} // namespace

Platform this_platform()
{
#ifdef _WIN32
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

bool real_exists(const std::string &path)
{
    boost::system::error_code ec;
    return boost::filesystem::exists(to_path(path), ec);
}

std::vector<std::string> real_list_dirs(const std::string &dir)
{
    std::vector<std::string> names;
    boost::system::error_code ec;
    boost::filesystem::path   root = to_path(dir);
    if (!boost::filesystem::is_directory(root, ec))
        return names;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (boost::filesystem::is_directory(it->path(), ec))
            names.push_back(from_path(it->path().filename()));
    return names;
}

std::string env_var(const char *name)
{
#ifdef _WIN32
    const wchar_t *value = _wgetenv(boost::nowide::widen(name).c_str());
    return value ? boost::nowide::narrow(value) : std::string();
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::string search_path(const std::string &name)
{
#ifdef _WIN32
    (void)name;
    return {};
#else
    return boost::process::search_path(name).string();
#endif
}

std::vector<std::string> program_files_roots()
{
    std::vector<std::string> roots;
#ifdef _WIN32
    for (const char *var : { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" }) {
        std::string root = env_var(var);
        if (!root.empty() && std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    }
#endif
    return roots;
}

std::string association_executable(const std::string &extension)
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD   size = static_cast<DWORD>(std::size(buf));
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, boost::nowide::widen(extension).c_str(), nullptr, buf, &size)))
        return {};
    return boost::nowide::narrow(buf);
#else
    (void)extension;
    return {};
#endif
}

bool launch(const std::vector<std::string> &argv, const std::string &log_tag)
{
    if (argv.empty())
        return false;
#ifdef _WIN32
    // wxExecute takes the arguments as an array, so paths with spaces or quotes need no escaping.
    std::vector<wxString> args;
    for (const std::string &arg : argv)
        args.emplace_back(wxString::FromUTF8(arg.c_str()));
    std::vector<const wchar_t *> raw;
    for (const wxString &arg : args)
        raw.push_back(arg.wc_str());
    raw.push_back(nullptr);
    long pid = ::wxExecute(const_cast<wchar_t **>(raw.data()), wxEXEC_ASYNC);
    if (pid <= 0) {
        BOOST_LOG_TRIVIAL(error) << log_tag << ": failed to start \"" << argv.front() << "\"";
        return false;
    }
#else
    // Same as BambuStudioLauncher: wxExecute is unreliable on macOS, so boost::process is used on
    // both Unix platforms.
    try {
        boost::filesystem::path exe = argv.front().find('/') != std::string::npos ? boost::filesystem::path(argv.front()) :
                                                                                  boost::process::search_path(argv.front());
        if (exe.empty())
            return false;
        boost::process::spawn(exe, boost::process::args(std::vector<std::string>(argv.begin() + 1, argv.end())));
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << log_tag << ": failed to start \"" << argv.front() << "\": " << ex.what();
        return false;
    }
#endif
    BOOST_LOG_TRIVIAL(info) << log_tag << ": started \"" << argv.front() << "\"";
    return true;
}

} // namespace ExternalEditor
} // namespace Slic3r
