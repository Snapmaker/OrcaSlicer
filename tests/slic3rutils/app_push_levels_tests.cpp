// Notification levels on the hub side (src/slic3r/GUI/AppPush.hpp, AppPush::policy): what a phone
// may ask for when it registers - priority_kinds, all_events, level_hint - and what the hub does
// with it. The rules themselves (which kind is quiet, normal or urgent, and later the time windows)
// stay on the phone; the hub only ever learns these three derived fields.
//
// No socket and no push service: the policy functions are pure, and the registration and test
// routes are driven through their handlers with bodies that never reach a provider.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AppPush.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::AppPush;
using json = nlohmann::json;

namespace {

// A real P-256 point and auth secret (the app repository's tools/vector.json), because
// register_device refuses anything WebPush::encrypt cannot encrypt to.
const char* const P256DH = "BGD-1LolWp0xyWHrdMY1bWjASbiSO2H6bOZpYi5g8p-2eQP-EAi4vJmkGunpVii8ZPLxsgwtfp9Rd6PClNRGIpk";
const char* const AUTH   = "vYwvHnpJNtBcix4vOk1caw";

json registration(const std::string& token)
{
    return json{ { "platform", "apns" }, { "env", "sandbox" }, { "token", token }, { "bundle", "dev.acerage.ultra1" },
                 { "p256dh", P256DH }, { "auth", AUTH }, { "label", "Test iPhone" }, { "app", "0.2.0" }, { "os", "iOS 26.1" } };
}

json stored_row(const std::string& token)
{
    const json s = settings_json();
    for (const auto& d : s["devices"])
        if (d.value("token", "") == token) return d;
    return json();
}

} // namespace

TEST_CASE("read_prefs: absent fields are the defaults", "[AppPushLevels]")
{
    const policy::DevicePrefs p = policy::read_prefs(json::object());
    CHECK(p.priority_kinds.empty());
    CHECK_FALSE(p.all_events);
    CHECK_FALSE(p.level_hint);
    // Not an object at all (a hand-edited settings row): still the defaults, never a throw.
    CHECK(policy::read_prefs(json("nonsense")).priority_kinds.empty());
}

TEST_CASE("read_prefs: kinds in canonical order, deduplicated, unknown ones dropped", "[AppPushLevels]")
{
    const json in = { { "priority_kinds", { "runout", "finished", "hovering", "finished", 7, "failed" } },
                      { "all_events", true },
                      { "level_hint", true } };
    const policy::DevicePrefs p = policy::read_prefs(in);
    REQUIRE(p.priority_kinds == std::vector<std::string>({ "finished", "failed", "runout" }));
    CHECK(p.all_events);
    CHECK(p.level_hint);

    // The wrong types are ignored, not refused: a newer app must still register.
    const policy::DevicePrefs q = policy::read_prefs(json{ { "priority_kinds", "failed" }, { "all_events", "yes" } });
    CHECK(q.priority_kinds.empty());
    CHECK_FALSE(q.all_events);
}

TEST_CASE("priority: severity first, then the device's own urgent kinds", "[AppPushLevels]")
{
    policy::DevicePrefs plain;
    CHECK(policy::priority(plain, "info", "finished") == 5);
    CHECK(policy::priority(plain, "info", "started") == 5);
    CHECK(policy::priority(plain, "warning", "paused") == 10);
    CHECK(policy::priority(plain, "error", "failed") == 10);

    policy::DevicePrefs urgent_finish;
    urgent_finish.priority_kinds = { "finished" };
    CHECK(policy::priority(urgent_finish, "info", "finished") == 10);
    CHECK(policy::priority(urgent_finish, "info", "started") == 5);
    // A raise, never a lower: a warning stays at 10 whatever the list says.
    CHECK(policy::priority(urgent_finish, "warning", "cancelled") == 10);
}

TEST_CASE("ttl: four hours for a failure, five minutes for routine, else thirty", "[AppPushLevels]")
{
    CHECK(policy::ttl("failed") == 14400);
    CHECK(policy::ttl("error") == 14400);
    CHECK(policy::ttl("runout") == 14400);
    CHECK(policy::ttl("started") == 300);
    CHECK(policy::ttl("resumed") == 300);
    CHECK(policy::ttl("finished") == 1800);
    CHECK(policy::ttl("paused") == 1800);
    CHECK(policy::ttl("cancelled") == 1800);
}

TEST_CASE("wants: the hub's filter, unless the device asked for every event", "[AppPushLevels]")
{
    policy::DevicePrefs plain;
    const std::vector<std::string> only_failures { "failed", "error", "runout" };
    // The global filter as before: kind AND severity.
    CHECK(policy::wants(plain, "error", "failed", "info", only_failures));
    CHECK_FALSE(policy::wants(plain, "info", "finished", "info", only_failures));
    CHECK_FALSE(policy::wants(plain, "warning", "paused", "error", {}));
    CHECK(policy::wants(plain, "info", "started", "info", {}));

    policy::DevicePrefs everything;
    everything.all_events = true;
    CHECK(policy::wants(everything, "info", "finished", "info", only_failures));
    CHECK(policy::wants(everything, "warning", "paused", "error", {}));
}

TEST_CASE("interruption_level: only with the hint, and only on the listed kinds", "[AppPushLevels]")
{
    policy::DevicePrefs p;
    p.priority_kinds = { "failed", "runout", "error" };
    CHECK(policy::interruption_level(p, "failed").empty());
    p.level_hint = true;
    CHECK(policy::interruption_level(p, "failed") == "time-sensitive");
    CHECK(policy::interruption_level(p, "runout") == "time-sensitive");
    CHECK(policy::interruption_level(p, "paused").empty());
    CHECK(policy::interruption_level(p, "finished").empty());
}

TEST_CASE("register_device stores the levels, answers the features, and resets them when omitted", "[AppPushLevels]")
{
    const std::string token = "aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44ee55ff66aa11bb22cc33dd44";

    json body = registration(token);
    body["priority_kinds"] = { "failed", "finished" };
    body["all_events"]     = true;
    body["level_hint"]     = true;
    const auto first = register_device(body.dump());
    REQUIRE(first.first == 200);
    const json answer = json::parse(first.second);
    CHECK(answer["ok"] == true);
    REQUIRE(answer.contains("features"));
    const auto& f = answer["features"];
    CHECK(std::find(f.begin(), f.end(), json("all_events")) != f.end());
    CHECK(std::find(f.begin(), f.end(), json("push_test")) != f.end());

    json row = stored_row(token);
    REQUIRE(row.is_object());
    REQUIRE(row.contains("levels"));
    CHECK(row["levels"]["priority_kinds"] == json({ "finished", "failed" }));
    CHECK(row["levels"]["all_events"] == true);
    CHECK(row["levels"]["level_hint"] == true);
    // The stored row reads back into the same prefs (AppPush::start uses read_prefs on it).
    const policy::DevicePrefs back = policy::read_prefs(row["levels"]);
    CHECK(back.priority_kinds == std::vector<std::string>({ "finished", "failed" }));
    CHECK(back.all_events);

    // The hub page sees them too, and they are not secrets.
    bool       seen   = false;
    const json masked = masked_json(); // held: a range-for over a temporary's member dangles
    for (const auto& d : masked["devices"])
        if (d.value("label", "") == "Test iPhone") {
            seen = true;
            CHECK(d["levels"]["all_events"] == true);
        }
    CHECK(seen);

    // An app version without notification levels re-registers: the defaults, exactly as before.
    REQUIRE(register_device(registration(token).dump()).first == 200);
    row = stored_row(token);
    CHECK(row["levels"]["priority_kinds"].empty());
    CHECK(row["levels"]["all_events"] == false);
    CHECK(row["levels"]["level_hint"] == false);

    forget_device(json{ { "platform", "apns" }, { "token", token } }.dump());
    CHECK(stored_row(token).is_null());
}

TEST_CASE("test_device refuses before it sends anything", "[AppPushLevels]")
{
    CHECK(test_device("not json").first == 400);
    CHECK(test_device(json{ { "token", "abc" }, { "kind", "exploded" } }.dump()).first == 400);
    CHECK(test_device(json{ { "kind", "failed" } }.dump()).first == 400);
    // A token no device registered: 404, and nothing was sent.
    const auto r = test_device(json{ { "platform", "fcm" }, { "token", "never-registered" }, { "kind", "failed" } }.dump());
    CHECK(r.first == 404);
    CHECK(json::parse(r.second).contains("error"));
}
