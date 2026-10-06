#include <catch2/catch.hpp>

#include "slic3r/Utils/HubHomeLogic.hpp"
#include "slic3r/Utils/LoginUserAgent.hpp"

using namespace Slic3r;
using namespace Slic3r::HubHome;

// Rules for embedding the phone hub page (the Home tab showed it until it became Recent / Print
// History); RemoteHub still classifies the hub's record with them.

static const std::string TOKEN = "abc123def456ghi7";

TEST_CASE("hub home: the page address is built from the hub's port, token and theme", "[HubHome]")
{
    CHECK(hub_page_url(13640, TOKEN, true) == "http://127.0.0.1:13640/r/abc123def456ghi7/?embed=1&theme=dark");
    CHECK(hub_page_url(13641, TOKEN, false) == "http://127.0.0.1:13641/r/abc123def456ghi7/?embed=1&theme=light");
    // A fallback port is used as given: nothing assumes 13640.
    CHECK(hub_page_url(13659, TOKEN, true) == "http://127.0.0.1:13659/r/abc123def456ghi7/?embed=1&theme=dark");
}

TEST_CASE("hub home: no address without a usable port and token", "[HubHome]")
{
    CHECK(hub_page_url(0, TOKEN, true).empty());
    CHECK(hub_page_url(-1, TOKEN, true).empty());
    CHECK(hub_page_url(70000, TOKEN, true).empty());
    CHECK(hub_page_url(13640, "", true).empty());
    CHECK(hub_page_url(13640, "short", true).empty());                       // under 10
    CHECK(hub_page_url(13640, std::string(33, 'a'), true).empty());          // over 32
    CHECK(hub_page_url(13640, "ABCDEFGHIJKL", true).empty());                // upper case
    CHECK(hub_page_url(13640, "abc/../def456", true).empty());               // path characters
    CHECK(hub_page_url(13640, "abcdefghij?x=1", true).empty());              // query injection
}

TEST_CASE("hub home: navigation stays on the loaded hub page", "[HubHome]")
{
    const int port = 13641;
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/?embed=1&theme=light", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/#devices", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7?embed=1", port, TOKEN));

    // Another port (the stale one after a move), another token (after "New link"), another
    // path or origin: all refused.
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13640/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/zzz123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7x/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/hub/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://localhost:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://192.168.1.5:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("https://127.0.0.1:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:136410/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641.evil.com/r/abc123def456ghi7/", port, TOKEN));
    // Nothing is allowed before a page was loaded.
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/", 0, ""));

    CHECK(is_allowed_navigation("about:blank", port, TOKEN));
    CHECK(is_allowed_navigation("data:text/html,<p>x</p>", port, TOKEN));
    CHECK(is_allowed_navigation("about:blank", 0, ""));
    CHECK_FALSE(is_allowed_navigation("https://github.com/aceRage/EdgeSlicer", port, TOKEN));
    CHECK_FALSE(is_allowed_navigation("file:///C:/Windows/win.ini", port, TOKEN));
    CHECK_FALSE(is_allowed_navigation("http://127.0.0.1:13619/web/flutter_web/index.html", port, TOKEN));
}

TEST_CASE("hub home: only web links go to the system browser", "[HubHome]")
{
    CHECK(is_external_browser_url("https://www.gnu.org/licenses/agpl-3.0.html"));
    CHECK(is_external_browser_url("http://example.com/"));
    CHECK(is_external_browser_url("HTTPS://GITHUB.COM/aceRage/EdgeSlicer"));
    CHECK_FALSE(is_external_browser_url("https://"));
    CHECK_FALSE(is_external_browser_url("file:///C:/Windows/System32/calc.exe"));
    CHECK_FALSE(is_external_browser_url("javascript:alert(1)"));
    CHECK_FALSE(is_external_browser_url("ms-settings:privacy"));
    CHECK_FALSE(is_external_browser_url("bambustudioopen://x"));
    CHECK_FALSE(is_external_browser_url("https://example.com/a b"));
    CHECK_FALSE(is_external_browser_url("https://example.com/\n--flag"));
    CHECK_FALSE(is_external_browser_url(""));
}

TEST_CASE("hub home: a re-check follows port moves, new links and a stopped hub", "[HubHome]")
{
    // Nothing changed: leave the page (and its camera streams) alone.
    CHECK(recheck(true, 13640, TOKEN, 13640, TOKEN) == Recheck::Keep);
    // Phone access toggled and the listener rebound on another port.
    CHECK(recheck(true, 13642, TOKEN, 13640, TOKEN) == Recheck::Reload);
    // "New link" or a restarted hub with another token.
    CHECK(recheck(true, 13640, "newtoken12345", 13640, TOKEN) == Recheck::Reload);
    // The first load (nothing loaded yet) is a reload too.
    CHECK(recheck(true, 13640, TOKEN, 0, "") == Recheck::Reload);
    // No hub, or an answer that cannot be used.
    CHECK(recheck(false, 13640, TOKEN, 13640, TOKEN) == Recheck::HubDown);
    CHECK(recheck(true, 0, TOKEN, 13640, TOKEN) == Recheck::HubDown);
    CHECK(recheck(true, 13640, "", 13640, TOKEN) == Recheck::HubDown);
}

static Probe answer(int port, const std::string& token)
{
    Probe p;
    p.answered = true;
    p.port     = port;
    p.token    = token;
    return p;
}

static Probe miss(Presence presence, ExitReason reason = ExitReason::Unknown)
{
    Probe p;
    p.presence    = presence;
    p.exit_reason = reason;
    return p;
}

TEST_CASE("hub home: one unanswered look never takes the page down", "[HubHome]")
{
    // The owner's report (2026-09-23): a running hub, one /hub/info that did not answer in time,
    // and the page was replaced by "not running". The old rule did exactly that on one miss.
    CHECK(recheck(false, 0, "", 13640, TOKEN) == Recheck::HubDown);

    Watch w;
    CHECK(w.judge(answer(13640, TOKEN), 13640, TOKEN) == Verdict::Keep);
    // A miss while the record says the hub is running (busy), or while the record is unreadable
    // (hub.json caught mid-rewrite): keep the page, look again soon.
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.next_check_s() == 2);
    CHECK(w.judge(miss(Presence::Unknown), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.next_check_s() == 4);
    // It answers again: all forgotten, back to the normal period.
    CHECK(w.judge(answer(13640, TOKEN), 13640, TOKEN) == Verdict::Keep);
    CHECK(w.failures() == 0);
    CHECK(w.next_check_s() == Watch::RECHECK_S);
    // ...so two more misses are again only retries.
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
}

TEST_CASE("hub home: misses in a row from a running hub make it 'not responding', not 'not running'", "[HubHome]")
{
    Watch w;
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Unreachable);
    CHECK(Watch::UNREACHABLE_AFTER == 3);
    CHECK(w.judge(miss(Presence::Unknown), 13640, TOKEN) == Verdict::Unreachable);
    // Backoff: 2, 4, 8, then every 10 s.
    CHECK(w.next_check_s() == 10);
    // It answers: the page comes back.
    CHECK(w.judge(answer(13640, TOKEN), 13640, TOKEN) == Verdict::Keep);
}

TEST_CASE("hub home: 'gone' needs the hub's own record, seen twice", "[HubHome]")
{
    {
        // Quit cleanly (no hub.json): the first sighting could be a hub being replaced by a newer
        // build, so look once more before saying so.
        Watch w;
        CHECK(w.judge(miss(Presence::NoRecord, ExitReason::Tray), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.next_check_s() == 2);
        CHECK(w.judge(miss(Presence::NoRecord, ExitReason::Tray), 13640, TOKEN) == Verdict::Gone);
        CHECK(w.judge(miss(Presence::NoRecord, ExitReason::Tray), 13640, TOKEN) == Verdict::Gone);
    }
    {
        // Replaced: gone once, then the new hub answers (same link, maybe another port).
        Watch w;
        CHECK(w.judge(miss(Presence::NoRecord, ExitReason::Requested), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(answer(13641, TOKEN), 13640, TOKEN) == Verdict::Reload);
        CHECK(w.failures() == 0);
    }
    {
        // Crashed: hub.json names a dead process.
        Watch w;
        CHECK(w.judge(miss(Presence::Crashed), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(miss(Presence::Crashed), 13640, TOKEN) == Verdict::Gone);
    }
    {
        // The two gone sightings must be in a row; a "running" look in between starts over.
        Watch w;
        CHECK(w.judge(miss(Presence::NoRecord), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(miss(Presence::Running), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(miss(Presence::NoRecord), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(miss(Presence::Crashed), 13640, TOKEN) == Verdict::Gone);
    }
    {
        // Not responding first, then the process dies: that becomes "gone" once confirmed.
        Watch w;
        for (int i = 0; i < 3; ++i)
            w.judge(miss(Presence::Running), 13640, TOKEN);
        CHECK(w.judge(miss(Presence::Crashed), 13640, TOKEN) == Verdict::Retry);
        CHECK(w.judge(miss(Presence::Crashed), 13640, TOKEN) == Verdict::Gone);
    }
}

TEST_CASE("hub home: an answer that cannot be used counts as a miss", "[HubHome]")
{
    Watch w;
    CHECK(w.judge(answer(0, TOKEN), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.judge(answer(13640, "bad"), 13640, TOKEN) == Verdict::Retry);
    CHECK(w.judge(answer(70000, TOKEN), 13640, TOKEN) == Verdict::Unreachable);
    // A usable one after that is a normal Keep / Reload.
    CHECK(w.judge(answer(13640, "newtoken12345"), 13640, TOKEN) == Verdict::Reload);
    CHECK(w.judge(answer(13640, TOKEN), 13640, TOKEN) == Verdict::Keep);
}

TEST_CASE("hub home: the hub's record, classified", "[HubHome]")
{
    CHECK(classify_record(false, false, false) == Presence::NoRecord);
    CHECK(classify_record(true, false, false) == Presence::Unknown); // there, but caught mid-rewrite
    CHECK(classify_record(true, true, true) == Presence::Running);
    CHECK(classify_record(true, true, false) == Presence::Crashed);

    CHECK(exit_reason_from("tray") == ExitReason::Tray);
    CHECK(exit_reason_from("request") == ExitReason::Requested);
    CHECK(exit_reason_from("idle") == ExitReason::Idle);
    CHECK(exit_reason_from("exit") == ExitReason::Unknown);
    CHECK(exit_reason_from("") == ExitReason::Unknown);
    CHECK(std::string(exit_reason_name(ExitReason::Tray)) == "tray");
    CHECK(std::string(verdict_name(Verdict::Unreachable)) == "unreachable");
}

TEST_CASE("hub home: which page load errors count", "[HubHome]")
{
    const std::string page = "http://127.0.0.1:13640/r/abc123def456ghi7/?embed=1&theme=dark";
    // Cancelled/aborted navigations (our own vetoes, a reload interrupting a load) never count.
    CHECK(classify_load_error(true, false, page, 13640, TOKEN) == LoadError::Ignore);
    CHECK(classify_load_error(true, true, page, 13640, TOKEN) == LoadError::Ignore);
    // The loaded page itself failing does.
    CHECK(classify_load_error(false, true, page, 13640, TOKEN) == LoadError::Connection);
    CHECK(classify_load_error(false, false, page, 13640, TOKEN) == LoadError::Page);
    // A stale navigation to the page's old port or old link, or anything that is not the hub page.
    CHECK(classify_load_error(false, true, "http://127.0.0.1:13641/r/abc123def456ghi7/", 13640, TOKEN) == LoadError::Ignore);
    CHECK(classify_load_error(false, true, "http://127.0.0.1:13640/r/oldtoken12345/", 13640, TOKEN) == LoadError::Ignore);
    CHECK(classify_load_error(false, true, "https://example.com/", 13640, TOKEN) == LoadError::Ignore);
    CHECK(classify_load_error(false, false, "data:text/html,x", 13640, TOKEN) == LoadError::Ignore);
    // A first load that failed can still report the blank page it started from: that counts.
    CHECK(classify_load_error(false, true, "about:blank", 13640, TOKEN) == LoadError::Connection);
    CHECK(classify_load_error(false, true, "", 13640, TOKEN) == LoadError::Connection);
}

TEST_CASE("hub home: theme parameter and live switch script", "[HubHome]")
{
    CHECK(std::string(theme_name(true)) == "dark");
    CHECK(std::string(theme_name(false)) == "light");
    CHECK(theme_script(true) == "if (window.__edgeTheme) window.__edgeTheme('dark');");
    CHECK(theme_script(false) == "if (window.__edgeTheme) window.__edgeTheme('light');");
}

TEST_CASE("login UA: views report the platform they were built for", "[LoginUA][HubHome]")
{
    // WebView::RecreateAll() refreshes every view's User-Agent after a theme switch; it used to
    // pass MacOS on every platform.
#if defined(_WIN32)
    CHECK(current_login_ua_platform() == LoginUAPlatform::Windows);
    CHECK(bbl_login_user_agent(current_login_ua_platform(), true, "en", "SM-Slicer", "2.4.0.0").find("Windows NT") != std::string::npos);
#elif defined(__APPLE__)
    CHECK(current_login_ua_platform() == LoginUAPlatform::MacOS);
#else
    CHECK(current_login_ua_platform() == LoginUAPlatform::Linux);
#endif
}
