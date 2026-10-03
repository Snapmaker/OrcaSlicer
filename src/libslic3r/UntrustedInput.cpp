#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "UntrustedInput.hpp"

#include "Config.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#endif
#endif

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r {
namespace untrusted {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string to_lower(std::string s)
{
    for (char &c : s)
        c = lower(c);
    return s;
}

bool ends_with_ci(const std::string &s, const std::string &suffix)
{
    if (suffix.size() > s.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (lower(s[s.size() - suffix.size() + i]) != lower(suffix[i]))
            return false;
    return true;
}

bool starts_with_ci(const std::string &s, const std::string &prefix)
{
    if (prefix.size() > s.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != lower(prefix[i]))
            return false;
    return true;
}

bool is_control_or_space(unsigned char c) { return c <= 0x20 || c == 0x7f; }

bool all_digits(const std::string &s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

// A label of a DNS host name: letters, digits, '-', '_' (seen on some CDNs), not empty.
bool valid_host_label(const std::string &label)
{
    if (label.empty() || label.size() > 63)
        return false;
    return std::all_of(label.begin(), label.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// Numeric IPv4 forms a browser or the OS resolver would accept: 127.0.0.1, 127.1, 2130706433,
// 0x7f.1 ... Any host made only of numeric labels counts.
bool looks_like_ipv4(const std::string &host)
{
    if (host.empty())
        return false;
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        std::string label = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (label.empty())
            return false;
        bool numeric = all_digits(label) ||
            (label.size() > 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X') &&
             std::all_of(label.begin() + 2, label.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; }));
        if (!numeric)
            return false;
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return true;
}

const char *const MODEL_EXTENSIONS[] = {".3mf", ".stl", ".step", ".stp", ".obj", ".zip"};

} // namespace

// ---- URLs ------------------------------------------------------------------------------------

bool parse_url(const std::string &url, Url &out)
{
    out = Url{};
    if (url.empty() || url.size() > 8192)
        return false;
    for (unsigned char c : url)
        if (is_control_or_space(c) || c == '\\')
            return false;

    const size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    std::string scheme = to_lower(url.substr(0, colon));
    if (!std::isalpha((unsigned char)scheme[0]))
        return false;
    for (char c : scheme)
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'))
            return false;
    if (url.compare(colon, 3, "://") != 0)
        return false;

    const size_t auth_begin = colon + 3;
    size_t       auth_end   = url.find_first_of("/?#", auth_begin);
    if (auth_end == std::string::npos)
        auth_end = url.size();
    std::string authority = url.substr(auth_begin, auth_end - auth_begin);

    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        out.has_userinfo = true;
        authority        = authority.substr(at + 1);
    }

    std::string host, port;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return false;
        host = authority.substr(0, close + 1);
        std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':')
                return false;
            port = rest.substr(1);
        }
    } else {
        const size_t pc = authority.find(':');
        host = authority.substr(0, pc);
        if (pc != std::string::npos)
            port = authority.substr(pc + 1);
        for (unsigned char c : host)
            if (c >= 0x80)
                return false; // IDN must be punycoded
    }
    host = to_lower(host);
    while (!host.empty() && host.back() == '.' && host.front() != '[')
        host.pop_back(); // "printables.com." is printables.com
    if (host.empty())
        return false;
    if (host.front() != '[') {
        size_t start = 0;
        while (true) {
            size_t      dot   = host.find('.', start);
            std::string label = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!valid_host_label(label))
                return false;
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
    }
    if (!port.empty()) {
        if (!all_digits(port) || port.size() > 5)
            return false;
        int p = std::stoi(port);
        if (p <= 0 || p > 65535)
            return false;
        out.port = p;
    } else if (authority.find(':') != std::string::npos && authority[0] != '[') {
        return false; // "host:" with an empty port
    }

    std::string rest = url.substr(auth_end);
    const size_t hash = rest.find('#');
    if (hash != std::string::npos)
        rest = rest.substr(0, hash);
    const size_t q = rest.find('?');
    out.path  = rest.substr(0, q);
    out.query = q == std::string::npos ? std::string() : rest.substr(q + 1);
    out.scheme = scheme;
    out.host   = host;
    return true;
}

bool host_is_or_under(const std::string &host, const std::string &domain)
{
    if (host.empty() || domain.empty())
        return false;
    const std::string h = to_lower(host), d = to_lower(domain);
    if (h == d)
        return true;
    return h.size() > d.size() + 1 && h.compare(h.size() - d.size(), d.size(), d) == 0 && h[h.size() - d.size() - 1] == '.';
}

bool is_local_or_ip_host(const std::string &host_in)
{
    const std::string host = to_lower(host_in);
    if (host.empty())
        return true;
    if (host.front() == '[')
        return true; // any IPv6 literal
    if (looks_like_ipv4(host))
        return true;
    for (const char *local : {"localhost", "local", "lan", "home.arpa", "internal", "intranet", "localdomain"})
        if (host_is_or_under(host, local))
            return true;
    return host.find('.') == std::string::npos; // single-label names resolve on the LAN
}

static bool page_link_on(const std::string &url, std::initializer_list<const char *> domains)
{
    Url u;
    if (!parse_url(url, u) || u.has_userinfo || (u.scheme != "http" && u.scheme != "https"))
        return false;
    for (const char *d : domains)
        if (host_is_or_under(u.host, d))
            return true;
    return false;
}

bool is_printables_link(const std::string &url) { return page_link_on(url, {"printables.com"}); }
bool is_makerworld_link(const std::string &url) { return page_link_on(url, {"makerworld.com", "makerworld.com.cn"}); }
bool is_thingiverse_link(const std::string &url) { return page_link_on(url, {"thingiverse.com"}); }
bool is_makerworld_file_url(const std::string &url) { return page_link_on(url, {"makerworld.com", "makerworld.com.cn", "bblmw.com"}); }

bool is_safe_to_open_externally(const std::string &url)
{
    if (starts_with_ci(url, "mailto:")) {
        if (url.size() <= 7 || url.size() > 2048)
            return false;
        for (unsigned char c : url)
            if (is_control_or_space(c) || c == '\\' || c == '"' || c == '<' || c == '>')
                return false;
        return true;
    }
    Url u;
    if (!parse_url(url, u))
        return false;
    return u.scheme == "http" || u.scheme == "https";
}

// ---- pages in our own web views -----------------------------------------------------------------

bool is_page_server_url(const std::string &url, int port)
{
    Url u;
    if (port <= 0 || !parse_url(url, u) || u.has_userinfo || u.scheme != "http")
        return false;
    return (u.host == "127.0.0.1" || u.host == "localhost") && u.port == port;
}

std::string local_path_from_file_url(const std::string &url)
{
    if (!starts_with_ci(url, "file://"))
        return std::string();
    std::string rest = url.substr(7);
    const size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos)
        rest = rest.substr(0, cut);
    // file:///C:/x or file:///home/x; "file://localhost/..." is the same as "file:///...".
    if (starts_with_ci(rest, "localhost/"))
        rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/')
        return std::string(); // file://server/share/... is a UNC path
    std::string path = percent_decode(rest);
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f)
            return std::string();
    if (path.size() >= 3 && path[0] == '/' && std::isalpha((unsigned char)path[1]) && path[2] == ':')
        path = path.substr(1); // "/C:/x" -> "C:/x"
    else if (path.size() >= 2 && (path[1] == '/' || path[1] == '\\'))
        return std::string(); // "//server/share" after decoding
    return path;
}

bool is_safe_attachment_to_launch(const std::string &file_name)
{
    static const char *const ok[] = {".pdf", ".txt", ".md", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
                                     ".stl", ".3mf", ".obj", ".step", ".stp", ".docx", ".xlsx", ".pptx", ".odt", ".ods", ".csv"};
    for (const char *ext : ok)
        if (ends_with_ci(file_name, ext) && file_name.size() > std::string(ext).size())
            return true;
    return false;
}

// ---- "Open in" links -----------------------------------------------------------------------

std::string percent_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(char(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else
            out.push_back(s[i]);
    }
    return out;
}

OpenLink parse_open_link(const std::string &link)
{
    OpenLink r;
    const size_t sep = link.find("://");
    if (sep == std::string::npos || sep == 0) {
        r.error = "not a link";
        return r;
    }
    const std::string scheme = to_lower(link.substr(0, sep));
    static const char *const open_schemes[] = {"edgeslicer", "ultraone", "snapmaker-orca", "snapmaker_orca", "orcaslicer",
                                               "prusaslicer", "bambustudio", "cura"};
    std::string payload;
    if (scheme == "bambustudioopen") {
        payload = link.substr(sep + 3);
    } else if (std::find(std::begin(open_schemes), std::end(open_schemes), scheme) != std::end(open_schemes)) {
        std::string rest = link.substr(sep + 3);
        if (!starts_with_ci(rest, "open")) {
            r.error = "unsupported link action";
            return r;
        }
        rest = rest.substr(4);
        if (!rest.empty() && rest[0] == '/')
            rest = rest.substr(1);
        if (!starts_with_ci(rest, "?file=")) {
            r.error = "the link names no file";
            return r;
        }
        payload = rest.substr(6);
    } else {
        r.error = "unsupported link scheme";
        return r;
    }

    // "&name=" separates the (still encoded) file URL from an optional display name. The file URL
    // itself is encoded, so a literal '&' inside it is always "%26".
    std::string file_part = payload, name_part;
    const size_t name_pos = payload.find("&name=");
    if (name_pos != std::string::npos) {
        file_part = payload.substr(0, name_pos);
        name_part = payload.substr(name_pos + 6);
        const size_t amp = name_part.find('&');
        if (amp != std::string::npos)
            name_part = name_part.substr(0, amp);
    }
    r.scheme   = scheme;
    r.file_url = percent_decode(file_part);
    r.name     = percent_decode(name_part);
    if (r.file_url.empty()) {
        r.error = "the link names no file";
        return r;
    }
    r.ok = true;
    return r;
}

// ---- model downloads -------------------------------------------------------------------------

const std::vector<std::string> &download_allowlist()
{
    // The file hosts behind the "Open in" buttons of the model sites (and their page hosts,
    // which some of them redirect through). Each entry also covers its subdomains.
    static const std::vector<std::string> list = {
        "printables.com",       // files.printables.com, media.printables.com
        "makerworld.com",       // MakerWorld
        "makerworld.com.cn",
        "bblmw.com",            // public-cdn.bblmw.com, makerworld.bblmw.com (MakerWorld CDN)
        "thingiverse.com",      // www.thingiverse.com/download:..., cdn.thingiverse.com
        "snapmaker.com",        // public.resource.snapmaker.com, space.snapmaker.com
        "cults3d.com",          // files.cults3d.com
    };
    return list;
}

bool has_model_extension(const std::string &file_name)
{
    for (const char *ext : MODEL_EXTENSIONS)
        if (ends_with_ci(file_name, ext) && file_name.size() > std::string(ext).size())
            return true;
    return false;
}

DownloadCheck check_model_download(const std::string &file_url, const std::string &link_scheme)
{
    DownloadCheck r;
    Url           u;
    if (!parse_url(file_url, u)) {
        r.reason = "the file address is not a valid web address";
        return r;
    }
    r.host = u.host;
    if (u.scheme != "https") {
        r.reason = "only secure (https) downloads are allowed, this link uses " + u.scheme + ":";
        return r;
    }
    if (u.has_userinfo) {
        r.reason = "the file address hides its real host behind a user name";
        return r;
    }
    if (is_local_or_ip_host(u.host)) {
        r.reason = "the file is on this computer or the local network, not on a model site";
        return r;
    }
    // Model type: the last path segment must carry a model extension, unless the URL has none at
    // all (a download endpoint like thingiverse.com/download:123 names the file in its response
    // header; the name is checked again when it is known).
    const std::string decoded_path = percent_decode(u.path);
    const size_t      slash        = decoded_path.find_last_of('/');
    const std::string last         = slash == std::string::npos ? decoded_path : decoded_path.substr(slash + 1);
    if (last.find('.') != std::string::npos && !has_model_extension(last)) {
        r.reason = "\"" + last + "\" is not a model file (3MF, STL, STEP, OBJ or ZIP)";
        return r;
    }

    for (const std::string &d : download_allowlist())
        if (host_is_or_under(u.host, d)) {
            r.verdict = DownloadVerdict::Allow;
            r.reason  = "known model site";
            return r;
        }
    const std::string s = to_lower(link_scheme);
    if (s == "bambustudio" || s == "bambustudioopen")
        for (const char *d : {"amazonaws.com", "aliyuncs.com"})
            if (host_is_or_under(u.host, d)) {
                r.verdict = DownloadVerdict::Allow;
                r.reason  = "MakerWorld download storage";
                return r;
            }
    r.verdict = DownloadVerdict::Ask;
    r.reason  = u.host + " is not one of the model sites EdgeSlicer knows";
    return r;
}

std::string sanitize_download_filename(const std::string &name_in)
{
    // Last path component, either separator.
    std::string name = name_in;
    const size_t cut = name.find_last_of("/\\");
    if (cut != std::string::npos)
        name = name.substr(cut + 1);

    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*')
            out.push_back('_');
        else
            out.push_back(char(c));
    }
    // Windows drops trailing dots and spaces; leading dots make hidden files / "..".
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    size_t lead = 0;
    while (lead < out.size() && (out[lead] == '.' || out[lead] == ' '))
        ++lead;
    out = out.substr(lead);
    if (out.empty())
        return std::string();

    // Device names are reserved with any extension: "NUL.3mf" opens the null device.
    std::string stem = out.substr(0, out.find('.'));
    std::string upper;
    for (char c : stem)
        upper.push_back(char(std::toupper((unsigned char)c)));
    while (!upper.empty() && upper.back() == ' ')
        upper.pop_back();
    static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
                                           "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                           "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char *r : reserved)
        if (upper == r) {
            out = "_" + out;
            break;
        }

    // Cap the length, keeping the extension.
    const size_t max_len = 150;
    if (out.size() > max_len) {
        const size_t dot = out.find_last_of('.');
        std::string  ext = (dot != std::string::npos && out.size() - dot <= 10) ? out.substr(dot) : std::string();
        out              = out.substr(0, max_len - ext.size()) + ext;
    }
    return out;
}

boost::filesystem::path download_marker_path(const boost::filesystem::path &dest_folder, const std::string &filename)
{
    return dest_folder / (filename + "." + std::to_string(get_current_pid()) + ".download");
}

namespace {

// A stat error is treated as free. Exclusive create and no-replace rename still refuse
// to overwrite if the name is in use, so a missed probe cannot replace an existing file.
bool path_taken(const boost::filesystem::path &p)
{
    boost::system::error_code ec;
    const auto                st = boost::filesystem::symlink_status(p, ec);
    return !ec && boost::filesystem::exists(st);
}

void set_already_exists(boost::system::error_code &ec)
{
    ec = boost::system::errc::make_error_code(boost::system::errc::file_exists);
}

bool exclusive_open_hit_existing() { return errno == EEXIST; }

#ifndef _WIN32
void set_posix_rename_error(boost::system::error_code &ec)
{
    if (errno == EEXIST)
        set_already_exists(ec);
    else
        ec.assign(errno, boost::system::system_category());
}
#endif

} // namespace

bool find_unused_filename(const boost::filesystem::path &dest_folder,
                          const std::string             &filename,
                          const boost::filesystem::path &ignored_marker,
                          std::string                   &result,
                          std::size_t                    max_version)
{
    // Probe the name that will be written, so a name the sanitizing maps onto an existing file is versioned too.
    const std::string sanitized = sanitize_download_filename(filename);
    if (sanitized.empty()) {
        result.clear();
        return false;
    }
    const std::string extension = boost::filesystem::path(sanitized).extension().string();
    const std::string stem      = sanitized.substr(0, sanitized.size() - extension.size());
    auto              is_used   = [&](const std::string &name) {
        const boost::filesystem::path marker = download_marker_path(dest_folder, name);
        return path_taken(dest_folder / name) || (marker != ignored_marker && path_taken(marker));
    };
    result = sanitized;
    for (size_t version = 1; is_used(result); ++version) {
        if (version > max_version)
            return false;
        result = stem + "(" + std::to_string(version) + ")" + extension;
    }
    return true;
}

FILE *open_exclusive_write(const boost::filesystem::path &path)
{
#ifdef _WIN32
    return _wfopen(path.wstring().c_str(), L"wbx");
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return nullptr;
    FILE *f = ::fdopen(fd, "wb");
    if (f == nullptr)
        ::close(fd);
    return f;
#endif
}

bool rename_no_replace(const boost::filesystem::path &from, const boost::filesystem::path &to, boost::system::error_code &ec)
{
    ec.clear();
#ifdef _WIN32
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), 0))
        return true;
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
        set_already_exists(ec);
    else
        ec.assign(static_cast<int>(err), boost::system::system_category());
    return false;
#else
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL) {
        set_posix_rename_error(ec);
        return false;
    }
#endif
#if defined(__APPLE__)
    if (::renameatx_np(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_EXCL) == 0)
        return true;
    if (errno != ENOSYS && errno != ENOTSUP) {
        set_posix_rename_error(ec);
        return false;
    }
#endif
    if (::link(from.c_str(), to.c_str()) == 0) {
        if (::unlink(from.c_str()) == 0)
            return true;
        // dest is a new name for the same inode. A leftover source name is acceptable
        // if dest is in place; do not fail the download after a successful link.
        if (path_taken(to)) {
            ec.clear();
            return true;
        }
        set_posix_rename_error(ec);
        return false;
    }
    // Hard links are missing on some NFS/CIFS/FUSE volumes. After a last existence
    // check, a plain rename is the fallback.
    if (errno == EPERM || errno == ENOTSUP || errno == EXDEV
#ifdef EOPNOTSUPP
        || errno == EOPNOTSUPP
#endif
    ) {
        if (path_taken(to)) {
            set_already_exists(ec);
            return false;
        }
        if (::rename(from.c_str(), to.c_str()) == 0) {
            ec.clear();
            return true;
        }
        set_posix_rename_error(ec);
        return false;
    }
    set_posix_rename_error(ec);
    return false;
#endif
}

FILE *claim_unused_download_name(const boost::filesystem::path &dest_folder,
                                 const std::string             &filename,
                                 const boost::filesystem::path &ignored_marker,
                                 std::string                   &result)
{
    const std::string requested = filename;
    result.clear();
    std::string last;
    for (std::size_t attempt = 0; attempt <= FIND_UNUSED_FILENAME_MAX_VERSION + 1; ++attempt) {
        if (!find_unused_filename(dest_folder, requested, ignored_marker, result))
            return nullptr;
        const boost::filesystem::path marker = download_marker_path(dest_folder, result);
        FILE *f = open_exclusive_write(marker);
        if (f != nullptr)
            return f;
        // The caller already holds this marker (Content-Disposition adopting the same
        // name). A removed marker (early pause) is not this case: exclusive create
        // succeeded above. Only a still-present ignored_marker is reused.
        if (marker == ignored_marker)
            return nullptr;
        if (!exclusive_open_hit_existing() || result == last) {
            result.clear();
            return nullptr;
        }
        last = result;
    }
    result.clear();
    return nullptr;
}

bool place_download_file(const boost::filesystem::path &tmp_path,
                         const boost::filesystem::path &dest_folder,
                         std::string                   &filename,
                         boost::filesystem::path       &dest_path,
                         boost::system::error_code     &ec)
{
    dest_path.clear();
    std::string last;
    for (std::size_t attempt = 0; attempt <= FIND_UNUSED_FILENAME_MAX_VERSION + 1; ++attempt) {
        std::string unused;
        if (!find_unused_filename(dest_folder, filename, tmp_path, unused)) {
            ec = boost::system::errc::make_error_code(boost::system::errc::io_error);
            return false;
        }
        // Same name after an EEXIST: path_taken treated the dest as free (stat error).
        // Do not spin; fail so the caller can drop the marker instead of looping forever.
        if (!last.empty() && unused == last) {
            set_already_exists(ec);
            dest_path = dest_folder / unused;
            filename  = unused;
            return false;
        }
        last      = unused;
        filename  = unused;
        dest_path = dest_folder / unused;
        if (rename_no_replace(tmp_path, dest_path, ec))
            return true;
        if (ec != boost::system::errc::file_exists)
            return false;
    }
    set_already_exists(ec);
    return false;
}

bool content_matches_extension(const std::string &file_name, const std::string &head, unsigned long long file_size, std::string *why)
{
    auto fail = [why](const char *msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (ends_with_ci(file_name, ".3mf") || ends_with_ci(file_name, ".zip")) {
        if (head.size() >= 4 && head.compare(0, 4, std::string("PK\x03\x04", 4)) == 0)
            return true;
        return fail("the file is not a ZIP/3MF archive");
    }
    if (ends_with_ci(file_name, ".stl")) {
        if (head.size() >= 84) {
            const uint32_t n = uint32_t((unsigned char)head[80]) | (uint32_t((unsigned char)head[81]) << 8) |
                               (uint32_t((unsigned char)head[82]) << 16) | (uint32_t((unsigned char)head[83]) << 24);
            if (file_size == 84ull + 50ull * n)
                return true;
        }
        std::string t = trim(head.substr(0, std::min<size_t>(head.size(), 64)));
        if (starts_with_ci(t, "solid"))
            return true;
        return fail("the file is not an STL model");
    }
    if (ends_with_ci(file_name, ".step") || ends_with_ci(file_name, ".stp")) {
        std::string t = trim(head.substr(0, std::min<size_t>(head.size(), 64)));
        // A UTF-8 byte order mark may precede the header.
        if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
            t = trim(t.substr(3));
        if (starts_with_ci(t, "ISO-10303-21"))
            return true;
        return fail("the file is not a STEP model");
    }
    if (ends_with_ci(file_name, ".obj")) {
        if (head.find('\0') == std::string::npos && !(head.size() >= 2 && head[0] == 'M' && head[1] == 'Z') &&
            !(head.size() >= 4 && head.compare(0, 4, std::string("PK\x03\x04", 4)) == 0))
            return true;
        return fail("the file is not an OBJ model");
    }
    return fail("the file type is not a model type");
}

// ---- archive entries ---------------------------------------------------------------------------

namespace {

// Non-ASCII characters that NFKC normalisation or an ANSI code page's best-fit mapping
// (WideCharToMultiByte without WC_NO_BEST_FIT_CHARS) turns into '.', '/', '\\' or ':'. The
// archive extractors write through wide APIs, so these never reach a narrow path call any more;
// refusing them as well keeps a name like "U+FF0E U+FF0E U+FF0F x" (fullwidth "../x") from
// meaning something different to any other consumer of the validated name.
bool is_separator_lookalike(std::uint32_t cp)
{
    switch (cp) {
    case 0x2024: // ONE DOT LEADER
    case 0x2025: // TWO DOT LEADER
    case 0x2044: // FRACTION SLASH
    case 0x2215: // DIVISION SLASH
    case 0x2216: // SET MINUS
    case 0x2236: // RATIO
    case 0x2571: // BOX DRAWINGS LIGHT DIAGONAL UPPER RIGHT TO LOWER LEFT
    case 0x2572: // BOX DRAWINGS LIGHT DIAGONAL UPPER LEFT TO LOWER RIGHT
    case 0x29F5: // REVERSE SOLIDUS OPERATOR
    case 0x29F8: // BIG SOLIDUS
    case 0x29F9: // BIG REVERSE SOLIDUS
    case 0xA789: // MODIFIER LETTER COLON
    case 0xFE30: // PRESENTATION FORM FOR VERTICAL TWO DOT LEADER
    case 0xFE52: // SMALL FULL STOP
    case 0xFE55: // SMALL COLON
    case 0xFE68: // SMALL REVERSE SOLIDUS
    case 0xFF0E: // FULLWIDTH FULL STOP
    case 0xFF0F: // FULLWIDTH SOLIDUS
    case 0xFF1A: // FULLWIDTH COLON
    case 0xFF3C: // FULLWIDTH REVERSE SOLIDUS
        return true;
    default:
        return false;
    }
}

// Scans the UTF-8 text for such a character. A byte that is not part of a well-formed
// sequence is skipped: legacy-encoded names keep working, and no conforming decoder reads a
// malformed (e.g. overlong) sequence as an ASCII character.
bool has_separator_lookalike(const std::string &s)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t              len = 0;
        std::uint32_t       cp  = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp  = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp  = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp  = c & 0x07;
        } else {
            ++i;
            continue;
        }
        bool ok = i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            ok                     = (cc & 0xC0) == 0x80;
            cp                     = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        if (is_separator_lookalike(cp))
            return true;
        i += len;
    }
    return false;
}

} // namespace

bool is_safe_archive_relative_path(const std::string &path)
{
    if (path.empty() || path.size() > 1024 || path.front() == '/')
        return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':')
            return false;
    if (has_separator_lookalike(path))
        return false;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        // Windows strips trailing dots/spaces, so "..." or ".. " would still climb.
        if (seg.find_first_not_of(". ") == std::string::npos)
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

ArchiveEntryName normalize_archive_entry_path(const std::string &raw, std::string &out)
{
    // Far longer than any real name; also bounds the work below.
    if (raw.empty() || raw.size() > 4096)
        return ArchiveEntryName::Reject;
    std::string s = raw;
    for (char &c : s)
        if (c == '\\')
            c = '/';
    // After the conversion an absolute path, a UNC share and "\\?\" all start with '/'.
    if (s.front() == '/')
        return ArchiveEntryName::Reject;
    std::string result;
    size_t      start = 0;
    while (start <= s.size()) {
        const size_t      slash = s.find('/', start);
        const std::string seg   = s.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg == "..")
            return ArchiveEntryName::Reject;
        if (!seg.empty() && seg != ".") {
            if (!result.empty())
                result += '/';
            result += seg;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    if (result.empty())
        return ArchiveEntryName::Skip; // only "." segments and separators
    if (!is_safe_archive_relative_path(result))
        return ArchiveEntryName::Reject;
    out = std::move(result);
    return ArchiveEntryName::Ok;
}

std::string archive_entry_leaf(const std::string &normalized)
{
    const size_t slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

namespace {

bool has_embedded_nul(const std::string &s) { return s.find('\0') != std::string::npos; }

bool has_embedded_nul(const boost::filesystem::path &p)
{
    return has_embedded_nul(p.string()) || has_embedded_nul(p.generic_string());
}

boost::filesystem::path strip_trailing_separators_path(boost::filesystem::path p)
{
    std::string s = p.generic_string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return boost::filesystem::path(s);
}

// weakly_canonical follows a symlink at the last component. Extraction *replaces*
// a destination file symlink, so that last hop of a *candidate* must stay the spelled
// path. Intermediate symlinks are still followed, so root/out/lib.so (out -> outside)
// is rejected. Do not use this on the extraction root: a symlink-to-dir root is a
// valid dest and must be followed (plain weakly_canonical).
boost::filesystem::path weakly_canonical_for_confine(const boost::filesystem::path &p)
{
    const boost::filesystem::path stripped = strip_trailing_separators_path(p);
    boost::system::error_code     ec;
    if (boost::filesystem::is_symlink(boost::filesystem::symlink_status(stripped, ec))) {
        const boost::filesystem::path parent = stripped.parent_path();
        if (parent.empty())
            return stripped;
        return boost::filesystem::weakly_canonical(parent) / stripped.filename();
    }
    return boost::filesystem::weakly_canonical(stripped);
}

bool skip_dot_or_empty(const boost::filesystem::path &comp) { return comp.empty() || comp == "."; }

} // namespace

bool is_path_within_root(const boost::filesystem::path &root, const boost::filesystem::path &candidate)
{
    try {
        if (has_embedded_nul(root) || has_embedded_nul(candidate))
            return false;
        const boost::filesystem::path root_c = boost::filesystem::weakly_canonical(strip_trailing_separators_path(root));
        const boost::filesystem::path cand_c = weakly_canonical_for_confine(candidate);
        if (has_embedded_nul(root_c) || has_embedded_nul(cand_c))
            return false;
        auto r = root_c.begin();
        auto c = cand_c.begin();
        while (r != root_c.end()) {
            if (skip_dot_or_empty(*r)) {
                ++r;
                continue;
            }
            while (c != cand_c.end() && skip_dot_or_empty(*c))
                ++c;
            if (c == cand_c.end() || *r != *c)
                return false;
            ++r;
            ++c;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool is_safe_attachment_path(const boost::filesystem::path &root, const boost::filesystem::path &candidate)
{
    try {
        if (root.empty() || candidate.empty() || candidate.is_relative())
            return false;
        if (!is_path_within_root(root, candidate))
            return false;
        // Follow every symlink, including the last component. Opening a dest-file
        // symlink must not launch the file outside the project's Auxiliaries.
        const boost::filesystem::path root_c = boost::filesystem::weakly_canonical(root);
        const boost::filesystem::path cand_c = boost::filesystem::weakly_canonical(candidate);
        if (cand_c.empty() || cand_c == root_c)
            return false;
        return is_path_within_root(root_c, cand_c);
    } catch (...) {
        return false;
    }
}

// ---- settings --------------------------------------------------------------------------------

std::string joined_post_process(const DynamicPrintConfig &cfg)
{
    const auto *opt = cfg.option<ConfigOptionStrings>("post_process");
    if (opt == nullptr)
        return std::string();
    std::string out;
    for (const std::string &value : opt->values) {
        size_t start = 0;
        while (start <= value.size()) {
            size_t      nl   = value.find_first_of("\r\n", start);
            std::string line = trim(value.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (!line.empty()) {
                if (!out.empty())
                    out.push_back('\n');
                out += line;
            }
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }
    return out;
}

bool filename_format_leaves_folder(const std::string &format)
{
    if (format.find('\\') != std::string::npos || format.find("..") != std::string::npos)
        return true;
    // Walk the literal text; '/' and ':' inside {expressions} may be division and the ternary
    // operator, but a '/' inside a quoted string of an expression is a path again.
    int  depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < format.size(); ++i) {
        const char c = format[i];
        if (depth > 0) {
            if (c == '"')
                in_string = !in_string;
            else if (in_string && c == '/')
                return true;
            else if (!in_string && (c == '}' || c == ']'))
                --depth;
            else if (!in_string && (c == '{' || c == '['))
                ++depth;
            continue;
        }
        if (c == '{' || c == '[')
            ++depth;
        else if (c == '/' || c == ':')
            return true;
    }
    return false;
}

const std::vector<std::string> &network_endpoint_keys()
{
    static const std::vector<std::string> keys = {"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
                                                  "printhost_user", "printhost_password", "printhost_port"};
    return keys;
}

// A bed texture or model path that reaches over the network (UNC share, URL, device path):
// Windows would contact the host - and send the user's credentials to an SMB server - just to
// draw the bed.
static bool is_remote_file_reference(const std::string &v)
{
    const std::string t = trim(v);
    return t.size() >= 2 && ((t[0] == '\\' && t[1] == '\\') || (t[0] == '/' && t[1] == '/') || t.find("://") != std::string::npos);
}

std::vector<UntrustedSetting> find_untrusted_settings(const DynamicPrintConfig &cfg, const std::string &source, const TrustedValues &trusted)
{
    std::vector<UntrustedSetting> found;

    const std::string pp = joined_post_process(cfg);
    if (!pp.empty() && trusted.post_process.count(pp) == 0)
        found.push_back({"post_process", source, pp, SettingRisk::RunsPrograms});

    if (const auto *ff = cfg.option<ConfigOptionString>("filename_format"))
        if (filename_format_leaves_folder(ff->value) && trusted.filename_format.count(ff->value) == 0)
            found.push_back({"filename_format", source, ff->value, SettingRisk::WritesOutsideOutputFolder});

    for (const std::string &key : network_endpoint_keys())
        if (const auto *opt = cfg.option<ConfigOptionString>(key))
            if (!trim(opt->value).empty())
                found.push_back({key, source, opt->value, SettingRisk::NetworkEndpoint});

    for (const char *key : {"bed_custom_texture", "bed_custom_model"})
        if (const auto *opt = cfg.option<ConfigOptionString>(key))
            if (is_remote_file_reference(opt->value))
                found.push_back({key, source, opt->value, SettingRisk::NetworkEndpoint});
    return found;
}

std::size_t neutralize_settings(DynamicPrintConfig &cfg, const std::vector<UntrustedSetting> &found, const DynamicPrintConfig *baseline)
{
    std::size_t changed = 0;
    for (const UntrustedSetting &s : found) {
        if (!cfg.has(s.key))
            continue;
        const ConfigOption *replacement = nullptr;
        if (baseline != nullptr && baseline->has(s.key))
            replacement = baseline->option(s.key);
        // The baseline's value may itself be remote (a network key is never carried over from a
        // project), so network keys always go back to empty.
        if (s.risk == SettingRisk::NetworkEndpoint)
            replacement = nullptr;
        if (replacement == nullptr) {
            const ConfigOptionDef *def = print_config_def.get(s.key);
            if (def == nullptr || def->default_value.get() == nullptr) {
                cfg.erase(s.key);
                ++changed;
                continue;
            }
            replacement = def->default_value.get();
        }
        cfg.option(s.key, true)->set(replacement);
        ++changed;
    }
    return changed;
}

} // namespace untrusted
} // namespace Slic3r
