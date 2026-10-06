// The hub's view of a Snapmaker U1 on the LAN (SnapmakerLan): when a printer counts as online or
// offline, which address its card carries, and - end to end, against fake_moonraker.py - what the
// probe reports while a printer answers normally, slowly, not at all, and again.
//
// Background (2026-09-25): the owner's U1 cards read "offline · a1pr8yczi3n0se.iot.us-west-1.
// amazonaws.com" or "offline · 10.0.0.106" while the printers answered on the LAN. The Device tab
// records the Snapmaker cloud's MQTT broker as a cloud-bound printer's "ip"; the LAN list took it
// as the printer's address, stored it next to the real one under the same serial number, and the
// two entries shared one status slot - so the broker's failed probe was served as the LAN card's
// state. A single missed probe also turned a card offline for half a minute.
//
// The unit cases touch neither a printer nor the network. The [SnapmakerProbeE2E] cases are hidden
// (they need the fakes): run them with run_snapmaker_probe_e2e.py, which starts two fake U1s on
// 127.0.0.1 and passes their ports in FAKE_MOONRAKER_A / FAKE_MOONRAKER_B.

#include <catch2/catch.hpp>

#include "slic3r/GUI/SnapmakerLan.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::GUI::SnapmakerLan;
using nlohmann::json;

// ------------------------------------------------------------ Presence ----

TEST_CASE("Presence: online on the first answer", "[SnapmakerProbe]")
{
    Presence p;
    CHECK_FALSE(p.online);
    CHECK(p.observe(true, 1000)); // changed
    CHECK(p.online);
    CHECK(p.fails == 0);
    CHECK(p.last_ok_ms == 1000);
    CHECK(p.refresh_after_ms() == Presence::PROBE_TTL_MS);
}

TEST_CASE("Presence: a printer that never answered is offline at the first miss", "[SnapmakerProbe]")
{
    Presence p;
    CHECK_FALSE(p.observe(false, 1000)); // it was not online, so nothing changed
    CHECK_FALSE(p.online);
    CHECK(p.refresh_after_ms() == Presence::OFFLINE_RETRY_MS);
}

TEST_CASE("Presence: goes offline only after FAILS_TO_OFFLINE misses in a row", "[SnapmakerProbe]")
{
    static_assert(Presence::FAILS_TO_OFFLINE == 3, "the cases below count to three");
    Presence p;
    p.observe(true, 0);
    CHECK_FALSE(p.observe(false, 5000));
    CHECK(p.online); // one slow reply from a busy printer
    CHECK_FALSE(p.observe(false, 10000));
    CHECK(p.online);
    CHECK(p.refresh_after_ms() == Presence::PROBE_TTL_MS); // still asked at the normal pace
    CHECK(p.observe(false, 15000));
    CHECK_FALSE(p.online);
    CHECK(p.fails == 3);
    CHECK(p.refresh_after_ms() == Presence::OFFLINE_RETRY_MS);

    SECTION("and comes back on the first answer")
    {
        CHECK(p.observe(true, 25000));
        CHECK(p.online);
        CHECK(p.fails == 0);
    }
    SECTION("and stays offline while it keeps missing")
    {
        CHECK_FALSE(p.observe(false, 25000));
        CHECK_FALSE(p.observe(false, 35000));
        CHECK_FALSE(p.online);
    }
}

TEST_CASE("Presence: a minute without an answer is offline even before three misses", "[SnapmakerProbe]")
{
    Presence p;
    p.observe(true, 0);
    CHECK_FALSE(p.observe(false, 30000));
    CHECK(p.online);
    CHECK(p.observe(false, Presence::SILENCE_MS)); // second miss, but a minute of silence
    CHECK_FALSE(p.online);
}

TEST_CASE("Presence: an answer in between starts the count again", "[SnapmakerProbe]")
{
    Presence p;
    p.observe(true, 0);
    p.observe(false, 5000);
    p.observe(false, 10000);
    p.observe(true, 15000);
    CHECK_FALSE(p.observe(false, 20000));
    CHECK_FALSE(p.observe(false, 25000));
    CHECK(p.online);
    CHECK(p.observe(false, 30000));
    CHECK_FALSE(p.online);
}

TEST_CASE("Presence: a printer that misses every other probe never shows offline", "[SnapmakerProbe]")
{
    Presence  p;
    long long t = 0;
    for (int i = 0; i < 40; ++i, t += 5000) {
        p.observe(i % 2 == 0, t);
        CHECK(p.online);
    }
}

// ------------------------------------------------------------ addresses ----

static const char* BROKER    = "a1pr8yczi3n0se.iot.us-west-1.amazonaws.com";
static const char* BROKER_CN = "a1su7rk2r6cmbq.ats.iot.cn-north-1.amazonaws.com.cn";

TEST_CASE("host_of strips scheme, port, path and brackets", "[SnapmakerProbe]")
{
    CHECK(host_of("10.0.0.106") == "10.0.0.106");
    CHECK(host_of("http://10.0.0.106:7125/server/info") == "10.0.0.106");
    CHECK(host_of(std::string(BROKER) + ":8883") == BROKER);
    CHECK(host_of(std::string("mqtts://") + BROKER + ":8883") == BROKER);
    CHECK(host_of("[fe80::1]:80") == "fe80::1");
    CHECK(host_of("fe80::1") == "fe80::1");
    CHECK(host_of(" u1.local. ") == "u1.local");
    CHECK(host_of("http://user:pw@10.0.0.5") == "10.0.0.5");
}

TEST_CASE("the Snapmaker cloud's endpoints are recognised, LAN addresses are not", "[SnapmakerProbe]")
{
    CHECK(is_cloud_host(BROKER));
    CHECK(is_cloud_host(std::string(BROKER) + ":8883"));
    CHECK(is_cloud_host("a1pr8yczi3n0se-ats.iot.us-west-1.amazonaws.com"));
    CHECK(is_cloud_host(BROKER_CN));
    CHECK(is_cloud_host("A1PR8YCZI3N0SE.IOT.US-WEST-1.AMAZONAWS.COM"));
    CHECK(is_cloud_host("api.snapmaker.com"));
    CHECK(is_cloud_host("something.aliyuncs.com"));

    CHECK_FALSE(is_cloud_host("10.0.0.106"));
    CHECK_FALSE(is_cloud_host("u1.local"));
    CHECK_FALSE(is_cloud_host("U1"));
    CHECK_FALSE(is_cloud_host("notamazonaws.com")); // a label boundary, not a substring
    CHECK_FALSE(is_cloud_host(""));
}

TEST_CASE("which addresses are LAN addresses", "[SnapmakerProbe]")
{
    CHECK(is_lan_host("10.0.0.106"));
    CHECK(is_lan_host("127.0.0.1"));
    CHECK(is_lan_host("u1.local"));
    CHECK(is_lan_host("u1.home.example")); // typed in by hand: accepted, only the cloud is refused
    CHECK(is_lan_host("fe80::1"));
    CHECK_FALSE(is_lan_host(BROKER));
    CHECK_FALSE(is_lan_host(""));
    CHECK_FALSE(is_lan_host("not an address"));

    // The Device tab's record is only trusted with a plainly local address.
    CHECK(is_local_address("10.0.0.106"));
    CHECK(is_local_address("192.168.1.20"));
    CHECK(is_local_address("u1.local"));
    CHECK(is_local_address("U1"));
    CHECK(is_local_address("::1"));
    CHECK_FALSE(is_local_address(BROKER));
    CHECK_FALSE(is_local_address("u1.home.example"));
    CHECK_FALSE(is_local_address("300.1.1.1"));
}

static Device dev(const std::string& id, const std::string& name, const std::string& ip, const std::string& by = "discovery")
{
    Device d;
    d.id       = id;
    d.name     = name;
    d.model    = "Snapmaker U1";
    d.ip       = ip;
    d.port     = 80;
    d.added_by = by;
    return d;
}

TEST_CASE("the owner's stored list: the broker entry goes, one card per printer stays", "[SnapmakerProbe]")
{
    // hub/snapmaker_lan.json as it was on 2026-09-25 (serial numbers replaced).
    const std::vector<Device> raw = { dev("SN-U2", "U2", BROKER, "streams"), dev("SN-U1", "U1", "10.0.0.108", "streams"),
                                      dev("SN-U3", "U3", "10.0.0.162", "streams"), dev("SN-U2", "U2", "10.0.0.106", "streams") };
    const std::vector<Device> list = sanitize(raw);
    REQUIRE(list.size() == 3);
    for (const Device& d : list) CHECK_FALSE(is_cloud_host(d.ip));
    const auto u2 = std::find_if(list.begin(), list.end(), [](const Device& d) { return d.id == "SN-U2"; });
    REQUIRE(u2 != list.end());
    CHECK(u2->ip == "10.0.0.106");
}

TEST_CASE("merge_device never lets the cloud broker replace or become an address", "[SnapmakerProbe]")
{
    std::vector<Device> list = { dev("SN-U2", "U2", "10.0.0.106") };

    SECTION("the Device tab's cloud record of a known printer")
    {
        CHECK_FALSE(merge_device(list, dev("SN-U2", "U2", BROKER, "device_tab")));
        REQUIRE(list.size() == 1);
        CHECK(list[0].ip == "10.0.0.106");
    }
    SECTION("a printer known only through the cloud")
    {
        CHECK_FALSE(merge_device(list, dev("SN-U9", "U9", BROKER, "device_tab")));
        CHECK(list.size() == 1);
    }
    SECTION("a printer that moved to another address moves (DHCP)")
    {
        CHECK(merge_device(list, dev("SN-U2", "", "10.0.0.120", "discovery")));
        REQUIRE(list.size() == 1);
        CHECK(list[0].ip == "10.0.0.120");
        CHECK(list[0].name == "U2");           // an empty name does not erase the known one
        CHECK(list[0].added_by == "discovery"); // how it first arrived is how it stays
    }
    SECTION("a printer first seen without its serial number gets it, and stays one entry")
    {
        std::vector<Device> l2 = { dev("10.0.0.106", "U2", "10.0.0.106"), dev("SN-U2", "U2", "10.0.0.99") };
        CHECK(merge_device(l2, dev("SN-U2", "U2", "10.0.0.106")));
        REQUIRE(l2.size() == 1);
        CHECK(l2[0].id == "SN-U2");
        CHECK(l2[0].ip == "10.0.0.106");
    }
    SECTION("a new LAN printer is added")
    {
        CHECK(merge_device(list, dev("SN-U3", "U3", "10.0.0.162")));
        CHECK(list.size() == 2);
    }
}

TEST_CASE("sanitize drops a placeholder entry once the address has its serial number", "[SnapmakerProbe]")
{
    const std::vector<Device> list = sanitize({ dev("10.0.0.106", "10.0.0.106", "10.0.0.106"), dev("SN-U2", "U2", "10.0.0.106") });
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == "SN-U2");
}

// ------------------------------------------------------ LAN over cloud ----

static json row(const std::string& id, const std::string& kind, bool online)
{
    return json { { "id", id }, { "kind", kind }, { "name", "U2" }, { "online", online } };
}

TEST_CASE("prefer_lan: the LAN card wins while it answers, the cloud connect is the fallback", "[SnapmakerProbe]")
{
    json connect = row("connect", "connect", true);
    connect["lan_id"] = "sm:SN-U2";
    connect["via"]    = "cloud";

    SECTION("LAN card online: the connect card goes")
    {
        json printers = json::array({ row("sm:SN-U2", "snapmaker", true), connect, row("bambu1", "bambu", true) });
        prefer_lan(printers);
        REQUIRE(printers.size() == 2);
        CHECK(printers[0]["id"] == "sm:SN-U2");
        CHECK(printers[1]["id"] == "bambu1");
    }
    SECTION("LAN card offline: both stay, and the LAN card says the cloud still reaches it")
    {
        json printers = json::array({ row("sm:SN-U2", "snapmaker", false), connect });
        prefer_lan(printers);
        REQUIRE(printers.size() == 2);
        CHECK(printers[0]["cloud_online"] == true);
        CHECK(printers[1]["id"] == "connect");
    }
    SECTION("no LAN card for that printer: the connect card stays")
    {
        json printers = json::array({ row("sm:SN-U1", "snapmaker", true), connect });
        prefer_lan(printers);
        CHECK(printers.size() == 2);
    }
    SECTION("a connect card that names no printer stays")
    {
        json plain    = row("connect", "connect", true);
        json printers = json::array({ row("sm:SN-U2", "snapmaker", true), plain });
        prefer_lan(printers);
        CHECK(printers.size() == 2);
    }
}

// ------------------------------------------------- end to end (hidden) ----

namespace {

struct Fake
{
    int port { 0 }, control { 0 };
};

bool fake_from_env(const char* var, Fake& out)
{
    const char* v = std::getenv(var);
    if (!v) return false;
    return std::sscanf(v, "%d:%d", &out.port, &out.control) == 2 && out.port > 0 && out.control > 0;
}

void fake_ctl(const Fake& f, const std::string& query)
{
    bool ok = false;
    Slic3r::Http::get("http://127.0.0.1:" + std::to_string(f.control) + "/fake/" + query)
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .perform_sync();
    REQUIRE(ok);
}

Device fake_device(const Fake& f, const std::string& id)
{
    Device d = dev(id, id, "127.0.0.1", "manual");
    d.port   = f.port;
    return d;
}

long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("the probe against a fake U1: normal, slow, down, back up", "[.][SnapmakerProbeE2E]")
{
    Fake f;
    REQUIRE(fake_from_env("FAKE_MOONRAKER_A", f));
    fake_ctl(f, "mode?m=normal");
    fake_ctl(f, "state?state=printing");
    const Device d = fake_device(f, "E2E-A");

    INFO("normal: one answer, the whole U1 status");
    Status s = status_now(d);
    REQUIRE(s.online);
    CHECK(s.state == "printing");
    CHECK(s.nozzles.size() == 4); // all four toolheads (PR #142)
    CHECK(s.age_ms == 0);
    CHECK(toolheads(d).size() == 4);

    INFO("slow: every reply takes 6 s, each request gives up after 3 s");
    fake_ctl(f, "mode?m=slow&delay=6");
    for (int miss = 1; miss < Presence::FAILS_TO_OFFLINE; ++miss) {
        const auto t0 = std::chrono::steady_clock::now();
        s             = status_now(d);
        CHECK(ms_since(t0) < 4500); // the 3 s request timeout, not the printer's 6 s
        CHECK(s.online);            // still online on its last reading...
        CHECK(s.state == "printing");
        CHECK(s.age_ms > 0);        // ...which is getting older
    }
    s = status_now(d);
    CHECK_FALSE(s.online); // the third miss in a row

    INFO("back up: online on the first answer");
    fake_ctl(f, "mode?m=normal");
    s = status_now(d);
    CHECK(s.online);
    CHECK(s.age_ms == 0);

    INFO("down: nobody listens (connection refused)");
    fake_ctl(f, "mode?m=down");
    for (int miss = 1; miss < Presence::FAILS_TO_OFFLINE; ++miss) {
        const auto t0 = std::chrono::steady_clock::now();
        s             = status_now(d);
        CHECK(ms_since(t0) < 3500);
        CHECK(s.online);
    }
    s = status_now(d);
    CHECK_FALSE(s.online);
    {
        // Offline and not due for a retry: served from the slot, without waiting on the printer.
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_FALSE(status(d).online);
        CHECK(ms_since(t0) < 200);
    }

    INFO("up again");
    fake_ctl(f, "mode?m=up");
    s = status_now(d);
    CHECK(s.online);
    CHECK(s.state == "printing");
}

TEST_CASE("one slow printer does not hold up another's card", "[.][SnapmakerProbeE2E]")
{
    Fake a, b;
    REQUIRE(fake_from_env("FAKE_MOONRAKER_A", a));
    REQUIRE(fake_from_env("FAKE_MOONRAKER_B", b));
    fake_ctl(a, "mode?m=slow&delay=8");
    fake_ctl(b, "mode?m=normal");
    fake_ctl(b, "state?state=standby");
    const std::vector<Device> list = { fake_device(a, "E2E-SLOW"), fake_device(b, "E2E-FAST") };

    const auto                t0  = std::chrono::steady_clock::now();
    const std::vector<Status> got = status_all(list, 3500);
    const long long           took = ms_since(t0);
    REQUIRE(got.size() == 2);
    CHECK(took < 4200);          // bounded by the budget, not by the slow printer
    CHECK(got[1].online);        // the fast one answered
    CHECK(got[1].state == "standby");
    CHECK_FALSE(got[0].online);  // the slow one never answered yet

    // The fast printer, asked again while the slow one is still being probed: at once, from its slot.
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(status(list[1]).online);
    CHECK(ms_since(t1) < 500);
    fake_ctl(a, "mode?m=normal");
    std::this_thread::sleep_for(std::chrono::seconds(4)); // let the slow probe finish before the fakes go
}
