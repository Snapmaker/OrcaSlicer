#include <catch2/catch.hpp>

#include "libslic3r/ModelBrowserRules.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::modelbrowser;

static const std::string PRINTABLES_PAGE = "https://www.printables.com/model/123-benchy";
static const std::string MAKERWORLD_PAGE = "https://makerworld.com/en/models/456";
static const std::string SNAPMAKER_PAGE  = "https://space.snapmaker.com/en/model/789";

TEST_CASE("Model browser: sites are text-only shortcuts on https", "[ModelBrowser]")
{
    REQUIRE(sites().size() == 3);
    for (const Site &s : sites()) {
        INFO(s.id);
        CHECK(s.start_url.rfind("https://", 0) == 0);
        CHECK(site_for_url(s.start_url) == &s);
        CHECK(site_by_id(s.id) == &s);
        CHECK_FALSE(s.name.empty());
    }
    CHECK(site_by_id("makerworld")->download_only);
    CHECK_FALSE(site_by_id("printables")->download_only);
    CHECK_FALSE(site_by_id("snapmaker")->download_only);

    CHECK(site_for_url(PRINTABLES_PAGE) == site_by_id("printables"));
    CHECK(site_for_url("https://makerworld.com.cn/zh/models/1") == site_by_id("makerworld"));
    CHECK(site_for_url(SNAPMAKER_PAGE) == site_by_id("snapmaker"));
    // Look-alikes and other parts of the vendors' domains are not the sites.
    CHECK(site_for_url("https://printables.com.evil.tld/") == nullptr);
    CHECK(site_for_url("https://evilprintables.com/") == nullptr);
    CHECK(site_for_url("https://www.snapmaker.com/") == nullptr);
    CHECK(site_for_url("https://printables.com@evil.tld/") == site_for_url("https://evil.tld/"));
    CHECK(site_for_url("not a url") == nullptr);
}

TEST_CASE("Model browser: this computer and the local network are blocked", "[ModelBrowser]")
{
    const std::vector<std::string> blocked = {
        "localhost", "LOCALHOST", "localhost.", "app.localhost", "127.0.0.1", "127.1", "2130706433", "0x7f000001", "0x7f.1",
        "0.0.0.0", "10.1.2.3", "172.16.0.1", "192.168.1.20", "169.254.1.1", "100.100.100.100", "[::1]", "[::]",
        "[::ffff:127.0.0.1]", "[fe80::1]", "[fd7a:115c:a1e0::1]", "printer", "nas.local", "router.lan", "box.home.arpa",
        "svc.internal", "machine.tail1234.ts.net", "8.8.8.8", "",
    };
    for (const std::string &h : blocked) {
        INFO(h);
        CHECK(is_blocked_host(h));
    }
    for (const std::string &h : {"www.printables.com", "makerworld.com", "space.snapmaker.com", "accounts.google.com",
                                 "public-cdn.bblmw.com", "localhost.example.com"}) {
        INFO(h);
        CHECK_FALSE(is_blocked_host(h));
    }

    // The page server (127.0.0.1:13619+) and the phone hub (13640-13659, LAN or Tailscale) as
    // navigations, in every spelling.
    const std::vector<std::string> urls = {
        "http://127.0.0.1:13619/web/home/index.html", "http://localhost:13640/r/abc/", "http://LocalHost:13640/",
        "http://[::1]:13619/", "http://0x7f.0.0.1:13619/", "http://2130706433:13619/", "http://127.1:13640/",
        "http://192.168.1.5:13640/r/token/", "https://pc.tail1234.ts.net/r/token/", "http://10.0.0.2/",
        "https://printables.com@127.0.0.1/", "http://user:pw@www.printables.com/", "http://0.0.0.0:13619/",
    };
    for (const std::string &u : urls) {
        INFO(u);
        CHECK(decide_navigation(u, PRINTABLES_PAGE, true).action == NavAction::Block);
        CHECK(decide_navigation(u, PRINTABLES_PAGE, false).action == NavAction::Block);
    }
}

TEST_CASE("Model browser: navigation rules", "[ModelBrowser]")
{
    CHECK(decide_navigation("https://www.printables.com/model/1", "").action == NavAction::Allow);
    CHECK(decide_navigation("https://accounts.google.com/o/oauth2/auth?x=1", PRINTABLES_PAGE).action == NavAction::Allow);
    CHECK(decide_navigation("http://example.com/", "").action == NavAction::Allow);
    CHECK(decide_navigation("about:blank", "").action == NavAction::Allow);
    CHECK(decide_navigation("about:srcdoc", "", false).action == NavAction::Allow);
    CHECK(decide_navigation("about:settings", "").action == NavAction::Block);

    // blob: follows its origin.
    CHECK(decide_navigation("blob:https://makerworld.com/2b7c-11", MAKERWORLD_PAGE).action == NavAction::Allow);
    CHECK(decide_navigation("blob:http://127.0.0.1:13619/2b7c", MAKERWORLD_PAGE).action == NavAction::Block);
    CHECK(decide_navigation("blob:null/2b7c", MAKERWORLD_PAGE).action == NavAction::Block);

    // data: never as a page, fine inside a frame.
    CHECK(decide_navigation("data:text/html,<p>x</p>", "", true).action == NavAction::Block);
    CHECK(decide_navigation("data:text/html,<p>x</p>", "", false).action == NavAction::Allow);

    for (const std::string &u : {"file:///C:/Windows/win.ini", "FILE://server/share/x.3mf", "javascript:alert(1)", "view-source:https://x.com/",
                                 "edge://settings", "chrome://version", "ftp://ftp.example.com/x.stl", "ms-settings:privacy",
                                 "search-ms:query=x", "mailto:someone@example.com", "steam://run/1", "ms-msdt:/id", "tel:123",
                                 "", "no-scheme-here", ":nothing"}) {
        INFO(u);
        CHECK(decide_navigation(u, PRINTABLES_PAGE).action == NavAction::Block);
    }
    // Malformed web addresses.
    for (const std::string &u : {"https://", "https:///path", "https://exa mple.com/", "https://example.com:99999/", "https://ex\\ample.com/"}) {
        INFO(u);
        CHECK(decide_navigation(u, "").action == NavAction::Block);
    }
}

TEST_CASE("Model browser: Printables \"Open in\" links go to the app's downloader", "[ModelBrowser]")
{
    const std::string file   = "https://files.printables.com/media/prints/1/stls/2_benchy.stl";
    const std::string file3m = "https://files.printables.com/media/prints/1/3mf/2_benchy.3mf";
    const std::string enc    = percent_encode(file);

    for (const std::string &scheme : {"prusaslicer", "orcaslicer", "bambustudio", "edgeslicer", "PrusaSlicer"}) {
        const std::string link = scheme + "://open?file=" + enc;
        INFO(link);
        const NavDecision d = decide_navigation(link, PRINTABLES_PAGE);
        REQUIRE(d.action == NavAction::OpenLink);
        // The link is re-wrapped as our own scheme with the same file, so the downloader's own
        // checks (and no widening for bambustudio://) apply.
        const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
        REQUIRE(parsed.ok);
        CHECK(parsed.scheme == "edgeslicer");
        CHECK(parsed.file_url == file);
        CHECK(untrusted::check_model_download(parsed.file_url).verdict == untrusted::DownloadVerdict::Allow);
    }
    // "open/?file=" and a display name.
    {
        const NavDecision d = decide_app_link("prusaslicer://open/?file=" + percent_encode(file3m) + "&name=" + percent_encode("My Benchy.3mf"),
                                              PRINTABLES_PAGE);
        REQUIRE(d.action == NavAction::OpenLink);
        const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
        REQUIRE(parsed.ok);
        CHECK(parsed.file_url == file3m);
        CHECK(parsed.name == "My Benchy.3mf");
    }
    // Unknown public hosts still go to the downloader, which asks the user.
    CHECK(decide_app_link("orcaslicer://open?file=" + percent_encode("https://cdn.example.org/a.stl"), PRINTABLES_PAGE).action == NavAction::OpenLink);

    // File URLs the downloader would refuse are blocked here already.
    for (const std::string &f : {"http://files.printables.com/a.stl", "https://127.0.0.1:13619/a.stl", "https://192.168.1.2/a.3mf",
                                 "file:///C:/a.stl", "https://files.printables.com/a.exe", "https://user@files.printables.com/a.stl"}) {
        INFO(f);
        CHECK(decide_app_link("prusaslicer://open?file=" + percent_encode(f), PRINTABLES_PAGE).action == NavAction::Block);
    }
    // Malformed or unsupported link actions.
    CHECK(decide_app_link("prusaslicer://open", PRINTABLES_PAGE).action == NavAction::Block);
    CHECK(decide_app_link("prusaslicer://delete?file=x", PRINTABLES_PAGE).action == NavAction::Block);
    CHECK(decide_app_link("orcaslicer://open?file=", PRINTABLES_PAGE).action == NavAction::Block);
    // bambustudio:// may not reach generic cloud storage from the browser (no scheme widening).
    CHECK(decide_app_link("bambustudio://open?file=" + percent_encode("https://bucket.s3.amazonaws.com/a.3mf"), PRINTABLES_PAGE).action ==
          NavAction::OpenLink); // Ask in the downloader, not Allow
    CHECK(untrusted::check_model_download("https://bucket.s3.amazonaws.com/a.3mf").verdict == untrusted::DownloadVerdict::Ask);
}

TEST_CASE("Model browser: MakerWorld is download-only", "[ModelBrowser]")
{
    const std::string signed_url = "https://public-cdn.bblmw.com/makerworld/model/x.3mf?Signature=abc";
    // Its own "Open in Bambu Studio" button: never followed, wherever it comes from.
    CHECK(decide_navigation("bambustudioopen://" + percent_encode(signed_url), MAKERWORLD_PAGE).action == NavAction::MakerWorldNotice);
    CHECK(decide_navigation("bambustudioopen://" + percent_encode(signed_url), PRINTABLES_PAGE).action == NavAction::MakerWorldNotice);
    CHECK(decide_navigation("BambuStudioOpen://x", "").action == NavAction::MakerWorldNotice);
    // Any other "Open in" link while on MakerWorld: the same notice.
    CHECK(decide_navigation("bambustudio://open?file=" + percent_encode(signed_url), MAKERWORLD_PAGE).action == NavAction::MakerWorldNotice);
    CHECK(decide_navigation("orcaslicer://open?file=" + percent_encode(signed_url), "https://makerworld.com.cn/zh/models/1").action ==
          NavAction::MakerWorldNotice);
    // Plain downloads from MakerWorld are imported.
    const DownloadDecision d = decide_download(signed_url, "Benchy.3mf", MAKERWORLD_PAGE, 1024);
    CHECK(d.action == DownloadAction::Import);
    CHECK(d.file_name == "Benchy.3mf");
}

TEST_CASE("Model browser: new windows", "[ModelBrowser]")
{
    // OAuth popups stay in the isolated browser (window.opener kept).
    CHECK(decide_new_window("https://accounts.google.com/o/oauth2/v2/auth?client_id=1", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    CHECK(decide_new_window("https://accounts.google.com/o/oauth2/v2/auth?client_id=1", PRINTABLES_PAGE, false, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://appleid.apple.com/auth/authorize", MAKERWORLD_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://bambulab.com/en/sign-in", MAKERWORLD_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://id.snapmaker.com/login", SNAPMAKER_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("about:blank", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    CHECK(decide_new_window("", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    // The popup blocker: blank or sized popups nobody clicked for.
    CHECK(decide_new_window("about:blank", PRINTABLES_PAGE, false, true).action == WindowAction::Block);
    CHECK(decide_new_window("https://ads.example.com/", PRINTABLES_PAGE, false, true).action == WindowAction::Block);
    CHECK(decide_new_window("https://ads.example.com/", PRINTABLES_PAGE, false, false).action == WindowAction::Block);
    // A sized window.open() from a click is a popup.
    CHECK(decide_new_window("https://share.example.com/", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    // target=_blank to one of our sites: in place. Elsewhere: the user's own browser.
    CHECK(decide_new_window("https://www.printables.com/model/2", PRINTABLES_PAGE, true, false).action == WindowAction::NavigateInPlace);
    CHECK(decide_new_window("https://makerworld.com/en", PRINTABLES_PAGE, true, false).action == WindowAction::NavigateInPlace);
    CHECK(decide_new_window("https://www.youtube.com/watch?v=1", PRINTABLES_PAGE, true, false).action == WindowAction::SystemBrowser);
    // Never: local targets, files, script URLs.
    for (const std::string &t : {"http://127.0.0.1:13619/", "http://localhost:13640/r/x/", "file:///C:/x", "javascript:alert(1)",
                                 "http://192.168.0.10/"}) {
        INFO(t);
        CHECK(decide_new_window(t, PRINTABLES_PAGE, true, true).action == WindowAction::Block);
    }
    // A new window to an "Open in" link is carried out like the link itself.
    const WindowDecision w = decide_new_window("prusaslicer://open?file=" + percent_encode("https://files.printables.com/a.stl"), PRINTABLES_PAGE,
                                               true, false);
    CHECK(w.action == WindowAction::AppLink);
    CHECK(w.nav.action == NavAction::OpenLink);
    CHECK(decide_new_window("bambustudioopen://x", MAKERWORLD_PAGE, true, false).nav.action == NavAction::MakerWorldNotice);
}

TEST_CASE("Model browser: download routing", "[ModelBrowser]")
{
    SECTION("model files from the sites are imported")
    {
        for (const std::string &n : {"benchy.stl", "Benchy.3MF", "part.step", "part.stp", "thing.obj", "bundle.zip", "plate.gcode.3mf"}) {
            INFO(n);
            const DownloadDecision d = decide_download("https://files.printables.com/media/x/" + n, n, PRINTABLES_PAGE, 2048);
            CHECK(d.action == DownloadAction::Import);
            CHECK(d.file_name == n);
        }
        // Snapmaker Space through its resource host; a blob the page assembled.
        CHECK(decide_download("https://public.resource.snapmaker.com/m/a.3mf", "a.3mf", SNAPMAKER_PAGE).action == DownloadAction::Import);
        CHECK(decide_download("blob:https://space.snapmaker.com/1234-5678", "a.3mf", SNAPMAKER_PAGE).action == DownloadAction::Import);
        // A known file host is enough, whatever page started it.
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", "https://www.example.com/").action == DownloadAction::Import);
    }
    SECTION("model files from other sites need a confirmation")
    {
        const DownloadDecision d = decide_download("https://cdn.example.org/a.stl", "a.stl", "https://www.example.org/thing");
        CHECK(d.action == DownloadAction::ImportAfterConfirm);
        CHECK(decide_download("data:application/octet-stream;base64,AAAA", "a.stl", "https://www.example.org/").action ==
              DownloadAction::ImportAfterConfirm);
    }
    SECTION("names are sanitized: last component only, no device names, no separators")
    {
        CHECK(decide_download("https://files.printables.com/x", "C:\\Users\\me\\Downloads\\benchy.stl", PRINTABLES_PAGE).file_name == "benchy.stl");
        CHECK(decide_download("https://files.printables.com/x", "../../evil.3mf", PRINTABLES_PAGE).file_name == "evil.3mf");
        const std::string con = decide_download("https://files.printables.com/x", "CON.stl", PRINTABLES_PAGE).file_name;
        CHECK(con != "CON.stl");
        CHECK(con.find_first_of("/\\") == std::string::npos);
        // No suggested name: the URL path's last segment.
        CHECK(decide_download("https://files.printables.com/media/a%20b.stl", "", PRINTABLES_PAGE).file_name == "a b.stl");
        CHECK(decide_download("https://files.printables.com/", "", PRINTABLES_PAGE).file_name == "download");
    }
    SECTION("programs and scripts are refused, other files offered as a save")
    {
        for (const std::string &n : {"setup.exe", "run.BAT", "x.ps1", "x.js", "x.lnk", "x.msi", "x.scr", "x.hta", "x.vbs", "x.reg", "a.stl.exe"}) {
            INFO(n);
            CHECK(decide_download("https://files.printables.com/" + n, n, PRINTABLES_PAGE).action == DownloadAction::Refuse);
        }
        CHECK(decide_download("https://files.printables.com/guide.pdf", "guide.pdf", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
        CHECK(decide_download("https://files.printables.com/photo.jpg", "photo.jpg", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
        CHECK(decide_download("https://files.printables.com/plate.gcode", "plate.gcode", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
    }
    SECTION("never from this computer, the LAN or odd schemes; never oversized models")
    {
        CHECK(decide_download("http://127.0.0.1:13619/localfile/a.3mf", "a.3mf", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("http://192.168.1.4/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("file:///C:/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("ftp://example.com/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("blob:http://localhost:13640/1", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        const long long too_big = (long long)untrusted::MODEL_DOWNLOAD_SIZE_LIMIT + 1;
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", PRINTABLES_PAGE, too_big).action == DownloadAction::Refuse);
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", PRINTABLES_PAGE, -1).action == DownloadAction::Import);
    }
}

TEST_CASE("Model browser: the app bridge never serves a page the browser can show", "[ModelBrowser]")
{
    // The app's script bridge (SSWCP / handle_web_request) only answers our own pages: the local
    // page server or files under resources/web (GUI_App::is_own_page_url). Every address the model
    // browser is allowed to load must fail that test, so even a bridge attached by mistake would
    // refuse it. (The browser itself is created with web messages and host objects off, which the
    // panel asserts at runtime.)
    const std::vector<std::string> candidates = {
        "https://www.printables.com/", "https://makerworld.com/en", "https://space.snapmaker.com/",
        "http://127.0.0.1:13619/web/home/index.html", "http://localhost:13619/", "http://localhost:13640/r/x/",
        "file:///C:/Program%20Files/EdgeSlicer/resources/web/home/index.html", "http://[::1]:13619/", "about:blank",
        "blob:http://127.0.0.1:13619/1", "https://accounts.google.com/", "http://example.com/",
    };
    int allowed = 0;
    for (const std::string &u : candidates) {
        const NavDecision d = decide_navigation(u, "", true);
        if (d.action != NavAction::Allow)
            continue;
        ++allowed;
        INFO(u);
        for (int port = 13600; port <= 13700; ++port)
            CHECK_FALSE(untrusted::is_page_server_url(u, port));
        CHECK(untrusted::local_path_from_file_url(u).empty());
    }
    CHECK(allowed == 6); // the three sites, about:blank, Google, example.com
}

TEST_CASE("Model browser: honest User-Agent", "[ModelBrowser]")
{
    const std::string edge = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36 Edg/129.0.0.0";
    CHECK(browser_user_agent(edge, "2.4.2.0") == edge + " EdgeSlicer/2.4.2.0");
    // Anything that would make MakerWorld take us for Bambu Studio goes.
    const std::string bbl = edge + " BBL-Slicer/v1.10.0.0 BBL-Language/en BambuStudio/1.10 OrcaSlicer/2.0 dark";
    const std::string out = browser_user_agent(bbl, "2.4.2.0");
    CHECK(out.find("BBL") == std::string::npos);
    CHECK(out.find("Bambu") == std::string::npos);
    CHECK(out.find("Orca") == std::string::npos);
    CHECK(out.find(" EdgeSlicer/2.4.2.0") != std::string::npos);
    // A version with odd characters cannot inject anything.
    CHECK(browser_user_agent("UA", "1.0\r\nX: y") == "UA EdgeSlicer/1.0Xy");
    // Never two EdgeSlicer tokens.
    CHECK(browser_user_agent("UA EdgeSlicer/1.0", "2.0") == "UA EdgeSlicer/2.0");
}

TEST_CASE("Model browser: percent-encoding round trip", "[ModelBrowser]")
{
    const std::string s = "https://files.printables.com/a b&c=d/%41.stl?x=1#y";
    CHECK(untrusted::percent_decode(percent_encode(s)) == s);
    CHECK(percent_encode("aZ09-._~") == "aZ09-._~");
    CHECK(percent_encode("&") == "%26");
    CHECK(make_open_link("https://x.com/a.stl") == "edgeslicer://open?file=https%3A%2F%2Fx.com%2Fa.stl");
}
