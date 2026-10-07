#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintersMonitor.hpp"

using namespace Slic3r;
using namespace Slic3r::PrintersMonitor;
using json = nlohmann::json;

// The Printers tab (GUI/PrintersPanel.cpp + resources/web/orca/monitor.html): what the page may ask
// for, what it is given, and where "Open Device page" goes.

TEST_CASE("printers monitor: the page's messages", "[PrintersMonitor]")
{
    CHECK(parse_message("printers_get").kind == Message::Kind::Get);
    CHECK(parse_message("printers_groups_get").kind == Message::Kind::Groups);
    CHECK(parse_message("printers_tasks").kind == Message::Kind::Tasks);

    Message m = parse_message("printers_control:pause:0:01P00A123456789");
    CHECK(m.kind == Message::Kind::Control);
    CHECK(m.action == "pause");
    CHECK_FALSE(m.confirm);
    CHECK(m.id == "01P00A123456789");

    // Ids carry colons: everything after the second separator is the id.
    m = parse_message("printers_control:stop:1:sm:U1-0042");
    CHECK(m.kind == Message::Kind::Control);
    CHECK(m.action == "stop");
    CHECK(m.confirm);
    CHECK(m.id == "sm:U1-0042");

    m = parse_message("printers_open:ph:abc-1");
    CHECK(m.kind == Message::Kind::Open);
    CHECK(m.id == "ph:abc-1");

    m = parse_message("printers_job:17");
    CHECK(m.kind == Message::Kind::Job);
    CHECK(m.job == 17);
}

TEST_CASE("printers monitor: anything else is refused", "[PrintersMonitor]")
{
    for (const char* bad : { "", "printers_get ", "printers", "hub_start", "printers_control:pause:0:",
                             "printers_control:set_temp:0:abc", "printers_control:stop:yes:abc", "printers_control:pause",
                             "printers_control:pause:0:a b", "printers_control:pause:0:a\"b", "printers_control:pause:0:<x>",
                             "printers_open:", "printers_open:a'b", "printers_job:", "printers_job:0", "printers_job:-1",
                             "printers_job:1x", "printers_job:12345678901" })
        CHECK(parse_message(bad).kind == Message::Kind::Invalid);
    // The settings verbs exist on the phone, not on this tab (owner decision: Pause / Resume / Stop).
    CHECK_FALSE(valid_action("set_fan"));
    CHECK_FALSE(valid_action("resume_error"));
    CHECK(valid_action("resume"));
    CHECK_FALSE(valid_printer_id(std::string(201, 'a')));
    CHECK(valid_printer_id(std::string(200, 'a')));
    CHECK_FALSE(valid_printer_id("caf\xc3\xa9"));
}

TEST_CASE("printers monitor: answers are a script call with escaped JSON", "[PrintersMonitor]")
{
    CHECK(script_call("__printers", json { { "ok", true } }) == "if (window.__printers) window.__printers({\"ok\":true});");
    // A name that tries to end the script, and non-ASCII (U+2028 among it), stay inside the string.
    const std::string js = script_call("__printers", json { { "name", "a\"); alert(1); (\"\xe2\x80\xa8\xc3\xa9" } });
    CHECK(js.find("\\\"); alert(1)") != std::string::npos);
    CHECK(js.find("\\u2028") != std::string::npos);
    CHECK(js.find("\\u00e9") != std::string::npos);
    for (unsigned char c : js) CHECK(c < 0x80);
    // Broken UTF-8 does not throw.
    CHECK_FALSE(script_call("__printers", json { { "name", std::string("\xff\xfe") } }).empty());
    // Only identifiers are called.
    CHECK(script_call("alert(1);x", json()).empty());
    CHECK(script_call("", json()).empty());
    CHECK(script_call("1abc", json()).empty());
}

TEST_CASE("printers monitor: api_printers rows become the page's payload", "[PrintersMonitor]")
{
    const std::string body = R"({"printers":[
        {"id":"01P00A1","kind":"bambu","name":"X1C","percent":42,"ams":[{"id":"0","trays":[{"color":"#FF0000","type":"PLA"}]}],
         "controls":{"heaters":[{"id":"chamber","label":"Chamber","temp":31,"target":0}],"fans":[{"id":"part","label":"Part","percent":60}]}},
        {"id":"sm:U1-1","kind":"snapmaker","name":"U1","url":"http://192.168.1.20","toolheads":[{"index":0,"color":"#00FF00","loaded":true}]},
        {"id":"bad id","kind":"bambu"},
        {"name":"no id"},
        7
    ]})";
    const json p = page_payload(200, body, 1234);
    CHECK(p["ok"] == true);
    CHECK(p["at"] == 1234);
    REQUIRE(p["printers"].size() == 2);
    // The rows go through unchanged: the same schema as the hub's /summary.
    CHECK(p["printers"][0]["ams"][0]["trays"][0]["color"] == "#FF0000");
    CHECK(p["printers"][0]["controls"]["fans"][0]["percent"] == 60);
    CHECK(p["printers"][1]["toolheads"][0]["loaded"] == true);

    const json busy = page_payload(503, R"({"error":"the slicer is busy"})", 5);
    CHECK(busy["ok"] == false);
    CHECK(busy["error"] == "the slicer is busy");
    CHECK(busy["printers"].empty());
    CHECK(page_payload(500, "garbage", 5)["error"] == "the printer list is not available right now");
    CHECK(page_payload(200, "garbage", 5)["ok"] == false);
    CHECK(page_payload(200, R"({"printers":{}})", 5)["ok"] == false);
    CHECK(page_payload(200, R"({"printers":[]})", 5)["ok"] == true);
}

TEST_CASE("printers monitor: Open Device page, per vendor", "[PrintersMonitor]")
{
    const json rows = json::parse(R"J([
        {"id":"01P00A1","kind":"bambu"},
        {"id":"sm:U1-1","kind":"snapmaker","url":"http://192.168.1.20"},
        {"id":"ph:k1","kind":"printhost","url":"https://klipper.local:7125"},
        {"id":"host","kind":"printhost","url":"javascript:alert(1)"},
        {"id":"01P00A1","kind":"snapmaker","url":"http://dup"}
    ])J");
    const auto t = targets_of(rows);
    REQUIRE(t.size() == 4);
    CHECK(t.at("01P00A1").kind == "bambu"); // the first row with an id wins

    // Bambu: the Device tab while it is the Bambu one, else the page says why.
    CHECK(open_way(t.at("01P00A1"), true, false) == OpenWay::BambuMonitor);
    CHECK(open_way(t.at("01P00A1"), false, true) == OpenWay::NotHere);
    CHECK(not_here_reason(t.at("01P00A1")).find("Bambu Lab printer preset") != std::string::npos);
    // Snapmaker / Moonraker: the Device tab's web view when it is showing, else the browser.
    CHECK(open_way(t.at("sm:U1-1"), false, true) == OpenWay::PrinterWebView);
    CHECK(open_way(t.at("sm:U1-1"), true, false) == OpenWay::Browser);
    CHECK(open_way(t.at("ph:k1"), false, false) == OpenWay::Browser);
    // Never a non-web address.
    CHECK(open_way(t.at("host"), false, true) == OpenWay::NotHere);

    CHECK(is_web_url("http://192.168.1.20"));
    CHECK(is_web_url("https://printer.local:7125/"));
    CHECK_FALSE(is_web_url("http://"));
    CHECK_FALSE(is_web_url("file:///c:/x"));
    CHECK_FALSE(is_web_url("http://a b"));
    CHECK_FALSE(is_web_url("http://a\"onload=x"));
}

TEST_CASE("printers monitor: groups", "[PrintersMonitor]")
{
    // The old Multi-device pick list seeds the first group.
    auto seeded = seed_from_multi_devices({ "01P00A1", "", " 01P00A2 ", "01P00A1", "", "" });
    REQUIRE(seeded.size() == 1);
    CHECK(seeded[0].name == "Multi-device");
    CHECK(seeded[0].printers == std::vector<std::string> { "01P00A1", "01P00A2" });
    CHECK(seed_from_multi_devices({ "", "", "" }).empty());

    // Round trip.
    const std::string saved = save_groups(seeded);
    auto back = parse_groups(saved);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == seeded[0].id);
    CHECK(back[0].printers == seeded[0].printers);

    // Bad entries are skipped; "all" is reserved; duplicates keep the first; a missing name is the id.
    back = parse_groups(R"([{"id":"all","name":"x"},{"id":"farm","printers":["a","a","b c","b"]},{"id":"farm","name":"dup"},{"name":"no id"},3])");
    REQUIRE(back.size() == 1);
    CHECK(back[0].name == "farm");
    CHECK(back[0].printers == std::vector<std::string> { "a", "b" });
    CHECK(parse_groups("").empty());
    CHECK(parse_groups("not json").empty());

    const json g = groups_json(back);
    REQUIRE(g.size() == 2);
    CHECK(g[0]["id"] == "all");
    CHECK(g[0]["all"] == true);
    CHECK(g[1]["id"] == "farm");
}
