#ifndef slic3r_ModelBrowserRules_hpp_
#define slic3r_ModelBrowserRules_hpp_

// Decisions for the in-app model browser (Home > Models, src/slic3r/GUI/ModelBrowserPanel.cpp):
// which navigations it may make, what happens to the "Open in" buttons of the model sites, which
// popups it opens, and where a download goes. Plain C++ (no wx, no WebView2) so it is unit tested
// on its own (tests/libslic3r/test_model_browser_rules.cpp). The panel only carries the verdicts out.
//
// The browser is a separate, isolated WebView2 environment with no script bridge to the app. These
// rules are the second line: it never loads a page on this computer or the local network (our page
// server and the phone hub live there), never runs another program for a link, and a model file it
// downloads goes through the same safe download path and import as an "Open in" link.

#include <string>
#include <vector>

namespace Slic3r {
namespace modelbrowser {

// ---- sites ---------------------------------------------------------------------------------------

struct Site
{
    std::string              id;            // "printables", "makerworld", "snapmaker"
    std::string              name;          // shown as text only: no logos, no endorsement wording
    std::string              start_url;     // https
    std::vector<std::string> domains;       // each matches itself and its subdomains
    bool                     download_only; // its "Open in <other app>" button is not followed
};

// The sites offered as shortcuts, in toolbar order.
const std::vector<Site> &sites();
const Site *site_by_id(const std::string &id);
// The site whose domains the URL's host is (or is under); nullptr for any other URL.
const Site *site_for_url(const std::string &url);

// ---- hosts ---------------------------------------------------------------------------------------

// Hosts the browser never loads anything from, top-level or as a sub-resource: this computer
// (localhost, *.localhost, 127/8, [::1], 0.0.0.0), any IP address literal (private ranges, link
// local, Tailscale's 100.64/10 and public ones alike: the model sites are reached by name), single
// label names and local suffixes (*.local, *.lan, *.home.arpa, *.internal, ...), and *.ts.net
// (Tailscale names that resolve into the tailnet, where the phone hub may be served). A trailing
// dot ("localhost.") is ignored. An empty host is blocked.
bool is_blocked_host(const std::string &host);

// ---- navigation ----------------------------------------------------------------------------------

enum class NavAction {
    Allow,            // let the browser load it
    Block,            // cancel (reason says why)
    OpenLink,         // cancel and hand `open_link` (an edgeslicer://open?file= link) to the app's downloader
    MakerWorldNotice, // cancel and tell the user to use the site's download button instead
};

struct NavDecision
{
    NavAction   action = NavAction::Block;
    std::string reason;    // for the log and the user
    std::string open_link; // OpenLink only
};

// A navigation of the main document (main_frame) or of a frame inside it, to `url`, while the
// browser shows `page_url` (the page that asked; "" when unknown).
//   http/https: Allow, unless the URL does not parse strictly, carries credentials ("user@host"),
//               or its host is_blocked_host().
//   about:blank / about:srcdoc: Allow. blob:<origin>: Allow when <origin> would be. data: only in
//               frames.
//   file:, javascript:, view-source:, edge:, chrome:, ftp:, ...: Block.
//   any other scheme: decide_app_link().
NavDecision decide_navigation(const std::string &url, const std::string &page_url, bool main_frame = true);

// A link to another program ("<scheme>:..." that is not a web scheme), from page_url.
//   bambustudioopen:            MakerWorldNotice (the owner's decision: MakerWorld is download-only).
//   any "Open in" link while the page is on a download_only site: MakerWorldNotice.
//   prusaslicer:// orcaslicer:// bambustudio:// edgeslicer:// cura:// ... "open?file=<url>" (the
//   schemes untrusted::parse_open_link() accepts): OpenLink with the file URL re-wrapped as an
//   edgeslicer://open?file= link, unless untrusted::check_model_download() refuses the file URL
//   (not https, LAN, credentials, not a model type) - then Block. The app's downloader checks the
//   link again and asks before fetching from a host that is not a known model site.
//   anything else (mailto:, ms-*, steam:, ...): Block - a site never starts an OS handler from here.
NavDecision decide_app_link(const std::string &url, const std::string &page_url);

// "edgeslicer://open?file=<percent-encoded file_url>[&name=<percent-encoded name>]".
std::string make_open_link(const std::string &file_url, const std::string &name = std::string());

// RFC 3986 percent-encoding of everything but unreserved characters (A-Z a-z 0-9 - . _ ~).
std::string percent_encode(const std::string &s);

// ---- new windows ---------------------------------------------------------------------------------

enum class WindowAction {
    Popup,           // a popup window in the same isolated browser (keeps window.opener: OAuth sign-in)
    NavigateInPlace, // load the target in the main view
    SystemBrowser,   // open the target in the user's own browser
    AppLink,         // the target is a link to another program: carry out `nav` (OpenLink / notice / block)
    Block,
};

struct WindowDecision
{
    WindowAction action = WindowAction::Block;
    NavDecision  nav;      // decide_navigation() of the target
    std::string  reason;
};

// Sign-in pages (Google, Apple, Microsoft, Facebook, GitHub, Discord, X, Bambu Lab, Prusa,
// Snapmaker ID, ...) that are opened as popups and talk back to their opener.
bool is_identity_provider_host(const std::string &host);

// window.open() / target=_blank from opener_url to target.
//   target blocked by decide_navigation: Block (or AppLink for a link to another program).
//   "" / about:blank (a script fills it in later), a sign-in page, or a window.open() that asked
//   for a size or position (a popup, not a tab): Popup - only when the user started it, except
//   for sign-in pages; otherwise Block (the popup blocker).
//   a page of one of our sites: NavigateInPlace.
//   any other web page: SystemBrowser.
WindowDecision decide_new_window(const std::string &target, const std::string &opener_url, bool user_initiated, bool popup_features);

// ---- downloads -----------------------------------------------------------------------------------

enum class DownloadAction {
    Import,             // model file from one of our sites or a known model file host: save, check, open
    ImportAfterConfirm, // model file from any other site: ask first, then as Import
    OfferSave,          // not a model: ask whether to save it to the Downloads folder
    Refuse,             // never: programs and scripts, files from this computer / the LAN, oversized
};

struct DownloadDecision
{
    DownloadAction action = DownloadAction::Refuse;
    std::string    file_name; // sanitized plain file name (untrusted::sanitize_download_filename)
    std::string    reason;
};

// Programs, scripts, shortcuts and installers (".exe", ".msi", ".bat", ".ps1", ".js", ".lnk", ...).
bool is_executable_file_name(const std::string &file_name);

// A download the page started.
//   uri:             the download URL (http(s), blob: or data:)
//   suggested_name:  the file name the browser picked (from Content-Disposition or the URL); may be
//                    a full path, only the last component counts
//   page_url:        the page that started it
//   total_bytes:     the announced size, < 0 when unknown
DownloadDecision decide_download(const std::string &uri, const std::string &suggested_name, const std::string &page_url,
                                 long long total_bytes = -1);

// ---- browser identity ----------------------------------------------------------------------------

// The engine's default User-Agent with every token that would make a site take us for another app
// removed ("BBL-Slicer/...", "BBL-Language/...", anything with "BambuStudio", "OrcaSlicer",
// "PrusaSlicer") and " EdgeSlicer/<version>" appended. Never impersonates.
std::string browser_user_agent(const std::string &default_user_agent, const std::string &version);

} // namespace modelbrowser
} // namespace Slic3r

#endif // slic3r_ModelBrowserRules_hpp_
