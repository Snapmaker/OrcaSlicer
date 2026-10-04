#include "ModelBrowserRules.hpp"

#include "UntrustedInput.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace Slic3r {
namespace modelbrowser {

using untrusted::host_is_or_under;
using untrusted::parse_url;
using untrusted::Url;

namespace {

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool starts_with_ci(const std::string &s, const std::string &prefix)
{
    if (s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)prefix[i]))
            return false;
    return true;
}

bool ends_with_ci(const std::string &s, const std::string &suffix)
{
    if (s.size() < suffix.size())
        return false;
    return starts_with_ci(s.substr(s.size() - suffix.size()), suffix);
}

// "scheme" of "scheme:rest", lower case, "" when there is none (or it is not a valid scheme).
std::string scheme_of(const std::string &url)
{
    const size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0)
        return std::string();
    const std::string s = to_lower(url.substr(0, colon));
    if (!std::isalpha((unsigned char)s[0]))
        return std::string();
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'))
            return std::string();
    return s;
}

// The host of an http(s) URL, "" when it does not parse.
std::string web_host(const std::string &url)
{
    Url u;
    if (!parse_url(url, u) || (u.scheme != "http" && u.scheme != "https"))
        return std::string();
    return u.host;
}

NavDecision allow(std::string reason = std::string()) { return NavDecision{NavAction::Allow, std::move(reason), std::string()}; }
NavDecision block(std::string reason) { return NavDecision{NavAction::Block, std::move(reason), std::string()}; }

// http(s) URL checks shared by navigations, blob: origins and new windows.
NavDecision decide_web_url(const std::string &url)
{
    Url u;
    if (!parse_url(url, u))
        return block("the address is malformed");
    if (u.scheme != "http" && u.scheme != "https")
        return block("not a web address");
    if (u.has_userinfo)
        return block("the address carries a user name or password");
    if (is_blocked_host(u.host))
        return block("addresses on this computer or the local network are not opened in the model browser");
    return allow();
}

} // namespace

// ---- sites ---------------------------------------------------------------------------------------

const std::vector<Site> &sites()
{
    static const std::vector<Site> list = {
        {"printables", "Printables", "https://www.printables.com/", {"printables.com"}},
        {"makerworld", "MakerWorld", "https://makerworld.com/", {"makerworld.com", "makerworld.com.cn"}},
        {"snapmaker", "Snapmaker Space", "https://space.snapmaker.com/", {"space.snapmaker.com"}},
    };
    return list;
}

const Site *site_by_id(const std::string &id)
{
    for (const Site &s : sites())
        if (s.id == id)
            return &s;
    return nullptr;
}

const Site *site_for_url(const std::string &url)
{
    const std::string host = web_host(url);
    if (host.empty())
        return nullptr;
    for (const Site &s : sites())
        for (const std::string &d : s.domains)
            if (host_is_or_under(host, d))
                return &s;
    return nullptr;
}

// ---- hosts ---------------------------------------------------------------------------------------

bool is_blocked_host(const std::string &host_in)
{
    std::string host = to_lower(host_in);
    while (!host.empty() && host.back() == '.' && host.front() != '[')
        host.pop_back();
    if (host.empty())
        return true;
    // Localhost names, every IP literal (IPv4 in any of the forms a resolver accepts, IPv6),
    // single-label names and the local suffixes.
    if (untrusted::is_local_or_ip_host(host))
        return true;
    // Tailscale MagicDNS / Serve names resolve to tailnet addresses (100.64/10, fd7a:115c:a1e0::/48).
    if (host_is_or_under(host, "ts.net"))
        return true;
    // "localhost" spelled as a label of a longer name that some resolvers still map to loopback.
    if (host_is_or_under(host, "localhost.localdomain"))
        return true;
    return false;
}

// ---- navigation ----------------------------------------------------------------------------------

NavDecision decide_navigation(const std::string &url, const std::string &page_url, bool main_frame)
{
    if (url.empty())
        return block("empty address");
    const std::string scheme = scheme_of(url);
    if (scheme.empty())
        return block("the address is malformed");

    if (scheme == "http" || scheme == "https")
        return decide_web_url(url);

    if (scheme == "about") {
        const std::string what = to_lower(url);
        if (what == "about:blank" || what == "about:srcdoc")
            return allow();
        return block("internal page");
    }
    if (scheme == "blob") {
        // blob:https://site.example/uuid - the blob belongs to that origin.
        const std::string inner = url.substr(5);
        if (scheme_of(inner) != "https" && scheme_of(inner) != "http")
            return block("blob address without a web origin");
        // The origin part is "https://host[:port]/uuid": parse_url takes the uuid as the path.
        return decide_web_url(inner);
    }
    if (scheme == "data")
        return main_frame ? block("data: pages are not opened as a page") : allow();

    // Never: local files, script URLs, browser-internal and source views, FTP and friends.
    static const char *const never[] = {"file",   "javascript", "vbscript", "view-source", "edge",  "chrome",         "chrome-extension",
                                        "devtools", "ftp",      "filesystem", "ws",        "wss",   "microsoft-edge", "res",
                                        "ms-settings", "search-ms", "intent"};
    for (const char *n : never)
        if (scheme == n)
            return block("\"" + scheme + ":\" addresses are not opened in the model browser");

    return decide_app_link(url, page_url);
}

NavDecision decide_app_link(const std::string &url, const std::string &page_url)
{
    const std::string scheme = scheme_of(url);
    const untrusted::OpenLink link = untrusted::parse_open_link(url);
    if (!link.ok) {
        if (link.error == "unsupported link scheme")
            return block("links to other programs (\"" + scheme + ":\") are not opened from the model browser");
        return block("the \"Open in\" link was not understood: " + link.error);
    }
    (void) page_url;

    // MakerWorld's "Open in" button: the link goes to the downloader as it is, the same as when
    // the system browser hands it over (the downloader widens the host list for this scheme to
    // MakerWorld's signed storage and opens MakerWorld files through its import path).
    if (link.scheme == "bambustudio" || link.scheme == "bambustudioopen") {
        const untrusted::DownloadCheck check = untrusted::check_model_download(link.file_url, link.scheme);
        if (check.verdict == untrusted::DownloadVerdict::Refuse)
            return block(check.reason);
        return NavDecision{NavAction::OpenLink, check.reason, url};
    }

    // Other schemes: re-wrapped as our own link, without any scheme-specific widening.
    const untrusted::DownloadCheck check = untrusted::check_model_download(link.file_url);
    if (check.verdict == untrusted::DownloadVerdict::Refuse)
        return block(check.reason);
    return NavDecision{NavAction::OpenLink, check.reason, make_open_link(link.file_url, link.name)};
}

std::string percent_encode(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string       out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string make_open_link(const std::string &file_url, const std::string &name)
{
    std::string link = "edgeslicer://open?file=" + percent_encode(file_url);
    if (!name.empty())
        link += "&name=" + percent_encode(name);
    return link;
}

// ---- new windows ---------------------------------------------------------------------------------

bool is_identity_provider_host(const std::string &host)
{
    static const char *const idps[] = {
        "accounts.google.com", "appleid.apple.com",  "login.microsoftonline.com", "login.live.com", "www.facebook.com",
        "m.facebook.com",      "github.com",         "discord.com",               "api.twitter.com", "x.com",
        "twitter.com",         "bambulab.com",       "bambulab.cn",               "prusa3d.com",     "id.snapmaker.com",
        "account.snapmaker.com",
    };
    for (const char *d : idps)
        if (host_is_or_under(host, d))
            return true;
    return false;
}

WindowDecision decide_new_window(const std::string &target, const std::string &opener_url, bool user_initiated, bool popup_features)
{
    WindowDecision r;
    const std::string lower = to_lower(target);
    if (target.empty() || lower == "about:blank") {
        r.nav = allow();
        if (user_initiated) {
            r.action = WindowAction::Popup;
            r.reason = "blank popup the page fills in";
        } else {
            r.reason = "popup the user did not ask for";
        }
        return r;
    }

    r.nav = decide_navigation(target, opener_url, true);
    if (r.nav.action == NavAction::OpenLink) {
        r.action = WindowAction::AppLink;
        r.reason = r.nav.reason;
        return r;
    }
    if (r.nav.action != NavAction::Allow) {
        r.reason = r.nav.reason;
        return r;
    }
    const std::string scheme = scheme_of(target);
    if (scheme != "http" && scheme != "https") {
        r.reason = "only web pages open in a new window";
        return r;
    }

    const std::string host = web_host(target);
    if (is_identity_provider_host(host)) {
        r.action = WindowAction::Popup;
        r.reason = "sign-in page";
        return r;
    }
    if (popup_features) {
        if (user_initiated) {
            r.action = WindowAction::Popup;
            r.reason = "popup window";
        } else {
            r.reason = "popup the user did not ask for";
        }
        return r;
    }
    // Another page of one of our sites (the opener's own or another): stay in the browser.
    if (site_for_url(target) != nullptr) {
        r.action = WindowAction::NavigateInPlace;
        r.reason = "page of a model site";
        return r;
    }
    if (!user_initiated) {
        r.reason = "new window the user did not ask for";
        return r;
    }
    r.action = WindowAction::SystemBrowser;
    r.reason = "page outside the model sites";
    return r;
}

// ---- downloads -----------------------------------------------------------------------------------

bool is_executable_file_name(const std::string &file_name)
{
    static const char *const exts[] = {".exe", ".msi",  ".msix", ".msp",  ".appx", ".appxbundle", ".bat", ".cmd",  ".com", ".scr",
                                       ".pif", ".cpl",  ".dll",  ".sys",  ".ps1",  ".psm1",       ".vbs", ".vbe",  ".js",  ".jse",
                                       ".wsf", ".wsh",  ".hta",  ".lnk",  ".url",  ".reg",        ".jar", ".application", ".gadget",
                                       ".msc", ".scf",  ".inf",  ".iso",  ".img",  ".vhd",        ".vhdx", ".sh",  ".app", ".dmg",
                                       ".pkg", ".deb",  ".rpm",  ".appimage", ".run", ".py",      ".pyw", ".command", ".settingcontent-ms",
                                       ".library-ms", ".search-ms", ".xll", ".xlam", ".docm", ".xlsm", ".pptm"};
    for (const char *e : exts)
        if (ends_with_ci(file_name, e))
            return true;
    return false;
}

DownloadDecision decide_download(const std::string &uri, const std::string &suggested_name, const std::string &page_url, long long total_bytes)
{
    DownloadDecision r;

    // Where it comes from: the web (not this computer or the LAN), or a blob/data URL the page made.
    const std::string scheme = scheme_of(uri);
    std::string       file_host;
    if (scheme == "http" || scheme == "https") {
        const NavDecision nav = decide_web_url(uri);
        if (nav.action != NavAction::Allow) {
            r.reason = nav.reason;
            return r;
        }
        file_host = web_host(uri);
    } else if (scheme == "blob") {
        const NavDecision nav = decide_navigation(uri, page_url, false);
        if (nav.action != NavAction::Allow) {
            r.reason = nav.reason;
            return r;
        }
    } else if (scheme != "data") {
        r.reason = "downloads are taken only from web addresses";
        return r;
    }

    // The name: the last component of what the browser suggests, else of the URL path.
    std::string name = suggested_name;
    const size_t sep = name.find_last_of("/\\");
    if (sep != std::string::npos)
        name = name.substr(sep + 1);
    name = untrusted::sanitize_download_filename(name);
    if (name.empty() && !file_host.empty()) {
        Url u;
        if (parse_url(uri, u)) {
            std::string path = untrusted::percent_decode(u.path);
            const size_t slash = path.find_last_of('/');
            name = untrusted::sanitize_download_filename(slash == std::string::npos ? path : path.substr(slash + 1));
        }
    }
    if (name.empty())
        name = "download";
    r.file_name = name;

    if (is_executable_file_name(name)) {
        r.reason = "programs and scripts are never downloaded from the model browser";
        return r;
    }
    if (untrusted::has_model_extension(name)) {
        if (total_bytes >= 0 && (unsigned long long)total_bytes > (unsigned long long)untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) {
            r.reason = "the file is larger than the 500 MB limit for models";
            return r;
        }
        bool known = site_for_url(page_url) != nullptr;
        if (!known && !file_host.empty())
            for (const std::string &d : untrusted::download_allowlist())
                if (host_is_or_under(file_host, d)) {
                    known = true;
                    break;
                }
        r.action = known ? DownloadAction::Import : DownloadAction::ImportAfterConfirm;
        r.reason = known ? "model file from a known model site" : "model file from a site EdgeSlicer does not know";
        return r;
    }
    r.action = DownloadAction::OfferSave;
    r.reason = "not a model file";
    return r;
}

// ---- browser identity ----------------------------------------------------------------------------

std::string browser_user_agent(const std::string &default_user_agent, const std::string &version)
{
    std::string out;
    size_t      i = 0;
    const std::string &ua = default_user_agent;
    while (i < ua.size()) {
        // Tokens are separated by spaces; a parenthesised comment stays one token.
        size_t j = i;
        if (ua[i] == '(') {
            const size_t close = ua.find(')', i);
            j = close == std::string::npos ? ua.size() : close + 1;
        } else {
            while (j < ua.size() && ua[j] != ' ')
                ++j;
        }
        const std::string token = ua.substr(i, j - i);
        const std::string lower = to_lower(token);
        const bool drop = lower.find("bbl") != std::string::npos || lower.find("bambustudio") != std::string::npos ||
                          lower.find("bambu-studio") != std::string::npos || lower.find("orcaslicer") != std::string::npos ||
                          lower.find("prusaslicer") != std::string::npos || lower.find("edgeslicer") != std::string::npos;
        if (!token.empty() && !drop) {
            if (!out.empty())
                out += ' ';
            out += token;
        }
        i = j;
        while (i < ua.size() && ua[i] == ' ')
            ++i;
    }
    std::string v;
    for (char c : version)
        if (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == '+')
            v += c;
    if (v.empty())
        v = "0";
    if (!out.empty())
        out += ' ';
    out += "EdgeSlicer/" + v;
    return out;
}

} // namespace modelbrowser
} // namespace Slic3r
