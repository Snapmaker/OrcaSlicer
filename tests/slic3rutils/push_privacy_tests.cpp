// Push privacy (privacy audit 2026-10, EdgeSlicerSite PR 4):
//   * the identifiers that travel in the clear next to an encrypted push - the APNs thread-id, the
//     APNs / FCM collapse id, the Web Push Topic - are a keyed HMAC under a per-hub secret
//     (src/slic3r/GUI/PushIds.hpp), never the printer id (a Bambu serial) or an unkeyed hash of it;
//   * a new phone link forgets every push registration made under the old one
//     (RemoteHub::Testing::revoke_push_for_old_link, called by HubServer::new_link).
//
// No socket and no push service: AppPush and WebPush are driven through their handlers with
// bodies that never reach a provider.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AppPush.hpp"
#include "slic3r/GUI/PushIds.hpp"
#include "slic3r/GUI/RemoteHub.hpp"
#include "slic3r/GUI/WebPush.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <set>
#include <string>

using namespace Slic3r::GUI;
using json = nlohmann::json;

namespace {

const std::string KEY_A = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const std::string KEY_B = "f0e1d2c3b4a5968778695a4b3c2d1e0f00112233445566778899aabbccddeeff";
// The shape of a real Bambu serial number; not anybody's printer.
const std::string SERIAL = "01P00A123456789";

// A real P-256 point and auth secret (the app repository's tools/vector.json), because both
// registration routes refuse anything WebPush::encrypt cannot encrypt to.
const char* const P256DH = "BGD-1LolWp0xyWHrdMY1bWjASbiSO2H6bOZpYi5g8p-2eQP-EAi4vJmkGunpVii8ZPLxsgwtfp9Rd6PClNRGIpk";
const char* const AUTH   = "vYwvHnpJNtBcix4vOk1caw";

bool is_b64url(const std::string& s)
{
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

// What the hub sent before this change: base64url of the first 18 bytes of SHA-256("<id>|<kind>").
std::string unkeyed_collapse(const std::string& printer_id, const std::string& kind)
{
    static const char* A  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const std::string  in = printer_id + "|" + kind;
    unsigned char      d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*) in.data(), in.size(), d);
    std::string out;
    for (size_t i = 0; i < 18; i += 3) {
        const unsigned v = (unsigned(d[i]) << 16) | (unsigned(d[i + 1]) << 8) | unsigned(d[i + 2]);
        out += A[(v >> 18) & 63];
        out += A[(v >> 12) & 63];
        out += A[(v >> 6) & 63];
        out += A[v & 63];
    }
    return out;
}

json app_registration(const std::string& token)
{
    return json{ { "platform", "apns" }, { "env", "sandbox" }, { "token", token }, { "bundle", "dev.acerage.ultra1" },
                 { "p256dh", P256DH }, { "auth", AUTH }, { "label", "Test iPhone" }, { "app", "0.2.0" }, { "os", "iOS 26.1" } };
}

} // namespace

// ================================================================== opaque ids ====

TEST_CASE("push ids: a minted key is 32 random bytes in hex", "[PushPrivacy]")
{
    const std::string a = PushIds::new_key_hex();
    const std::string b = PushIds::new_key_hex();
    CHECK(PushIds::valid_key_hex(a));
    CHECK(PushIds::valid_key_hex(b));
    CHECK(a != b);
    CHECK_FALSE(PushIds::valid_key_hex(""));
    CHECK_FALSE(PushIds::valid_key_hex(KEY_A.substr(1)));
    CHECK_FALSE(PushIds::valid_key_hex(KEY_A.substr(0, 63) + "g"));
}

TEST_CASE("push ids: the thread id is stable per printer on one hub", "[PushPrivacy]")
{
    const std::string t = PushIds::thread_id(KEY_A, SERIAL);
    CHECK(t.size() == 17);
    CHECK(t[0] == 't');
    CHECK(is_b64url(t.substr(1)));
    // Stable: iOS keeps grouping one printer's notifications.
    CHECK(PushIds::thread_id(KEY_A, SERIAL) == t);
    // Different printers, different threads.
    CHECK(PushIds::thread_id(KEY_A, "01P00A123456780") != t);
    // No printer, no thread id at all (the APNs payload then has no thread-id).
    CHECK(PushIds::thread_id(KEY_A, "").empty());
    // An invalid key never falls back to something derivable without one.
    CHECK(PushIds::thread_id("", SERIAL).empty());
    CHECK(PushIds::thread_id("not-a-key", SERIAL).empty());
}

TEST_CASE("push ids: keyed - another hub's key gives unrelated ids", "[PushPrivacy]")
{
    CHECK(PushIds::thread_id(KEY_A, SERIAL) != PushIds::thread_id(KEY_B, SERIAL));
    CHECK(PushIds::collapse_id(KEY_A, SERIAL, "paused") != PushIds::collapse_id(KEY_B, SERIAL, "paused"));
}

TEST_CASE("push ids: not reversible - neither the serial nor an unkeyed hash of it is on the wire", "[PushPrivacy]")
{
    const std::string t = PushIds::thread_id(KEY_A, SERIAL);
    const std::string c = PushIds::collapse_id(KEY_A, SERIAL, "paused");
    CHECK(t.find(SERIAL) == std::string::npos);
    CHECK(c.find(SERIAL) == std::string::npos);
    CHECK(t.find("123456789") == std::string::npos);
    // The old derivation could be confirmed by anybody who knew the serial; the new one cannot.
    CHECK(c != unkeyed_collapse(SERIAL, "paused"));
    CHECK(t.substr(1) != unkeyed_collapse(SERIAL, "").substr(0, 16));
    // Guessing the serial without the key does not reproduce the value: try a handful of keys.
    std::set<std::string> seen { t };
    for (int i = 0; i < 16; ++i) seen.insert(PushIds::thread_id(PushIds::new_key_hex(), SERIAL));
    CHECK(seen.size() == 17);
}

TEST_CASE("push ids: the collapse id is 24 base64url characters, per printer and kind", "[PushPrivacy]")
{
    const std::string c = PushIds::collapse_id(KEY_A, SERIAL, "paused");
    CHECK(c.size() == 24);
    CHECK(is_b64url(c));
    CHECK(PushIds::collapse_id(KEY_A, SERIAL, "paused") == c);
    CHECK(PushIds::collapse_id(KEY_A, SERIAL, "finished") != c);
    CHECK(PushIds::collapse_id(KEY_A, "01P00A123456780", "paused") != c);
    // The separator keeps ("ab","c") and ("a","bc") apart.
    CHECK(PushIds::collapse_id(KEY_A, "ab", "c") != PushIds::collapse_id(KEY_A, "a", "bc"));
    // Never empty, even with no key handed over yet (a per-process key is used instead).
    CHECK(PushIds::collapse_id("", SERIAL, "paused").size() == 24);
    CHECK(PushIds::collapse_id("", SERIAL, "paused") != unkeyed_collapse(SERIAL, "paused"));
}

TEST_CASE("push ids: the process key is never empty and set_key ignores junk", "[PushPrivacy]")
{
    CHECK(PushIds::valid_key_hex(PushIds::key()));
    PushIds::set_key(KEY_A);
    CHECK(PushIds::key() == KEY_A);
    PushIds::set_key("junk");
    CHECK(PushIds::key() == KEY_A);
}

// ================================================================ AppPush's key ====

TEST_CASE("app push: the id key is minted once, kept in settings.json, and never shown", "[PushPrivacy]")
{
    AppPush::start(json::object());
    const json s1 = AppPush::settings_json();
    REQUIRE(s1.contains("id_key"));
    const std::string key = s1["id_key"].get<std::string>();
    CHECK(PushIds::valid_key_hex(key));
    CHECK(PushIds::key() == key);
    // The hub page and the phone plane never see it.
    CHECK(AppPush::masked_json().dump().find(key) == std::string::npos);
    CHECK(AppPush::providers_json().dump().find(key) == std::string::npos);

    // A restart from the saved settings keeps it, so a printer keeps its thread across restarts.
    const std::string thread = PushIds::thread_id(PushIds::key(), SERIAL);
    AppPush::start(s1);
    CHECK(AppPush::settings_json()["id_key"] == key);
    CHECK(PushIds::thread_id(PushIds::key(), SERIAL) == thread);

    // A hand-edited, broken key is replaced rather than used.
    json broken = s1;
    broken["id_key"] = "1234";
    AppPush::start(broken);
    const std::string fresh = AppPush::settings_json()["id_key"].get<std::string>();
    CHECK(PushIds::valid_key_hex(fresh));
    CHECK(fresh != "1234");
}

// =============================================================== new phone link ====

TEST_CASE("new link: every app push device and browser subscription of the old link is forgotten", "[PushPrivacy]")
{
    AppPush::start(json::object());
    WebPush::start(json::object());
    // Start from nothing, whatever earlier tests left behind.
    RemoteHub::Testing::revoke_push_for_old_link();

    REQUIRE(AppPush::register_device(app_registration("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").dump()).first == 200);
    REQUIRE(AppPush::register_device(app_registration("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb").dump()).first == 200);
    const json sub = { { "endpoint", "https://push.example.invalid/send/abc123" },
                       { "keys", { { "p256dh", P256DH }, { "auth", AUTH } } } };
    REQUIRE(WebPush::add_subscription(sub.dump()).first == 200);
    REQUIRE(AppPush::settings_json()["devices"].size() == 2);
    REQUIRE(AppPush::has_devices());
    REQUIRE(WebPush::has_subscriptions());
    AppPush::consume_dirty();
    WebPush::consume_dirty();

    const RemoteHub::Testing::LinkRevocation gone = RemoteHub::Testing::revoke_push_for_old_link();
    CHECK(gone.app_devices == 2);
    CHECK(gone.web_subscriptions == 1);
    CHECK(AppPush::settings_json()["devices"].empty());
    CHECK_FALSE(AppPush::has_devices());
    CHECK(WebPush::settings_json()["subscriptions"].empty());
    CHECK_FALSE(WebPush::has_subscriptions());
    // settings.json must be rewritten so a restart does not bring the rows back.
    CHECK(AppPush::consume_dirty());
    CHECK(WebPush::consume_dirty());

    // Nothing left: a second rotation forgets nothing and is harmless.
    const RemoteHub::Testing::LinkRevocation again = RemoteHub::Testing::revoke_push_for_old_link();
    CHECK(again.app_devices == 0);
    CHECK(again.web_subscriptions == 0);

    // A phone re-paired with the new link registers again as a new row.
    REQUIRE(AppPush::register_device(app_registration("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").dump()).first == 200);
    CHECK(AppPush::settings_json()["devices"].size() == 1);
    RemoteHub::Testing::revoke_push_for_old_link();
}

TEST_CASE("unpair: a single phone can still be removed on its own", "[PushPrivacy]")
{
    AppPush::start(json::object());
    RemoteHub::Testing::revoke_push_for_old_link();
    REQUIRE(AppPush::register_device(app_registration("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc").dump()).first == 200);
    REQUIRE(AppPush::register_device(app_registration("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd").dump()).first == 200);
    // The app unpairing names its own token (DELETE /r/<token>/push/device) ...
    CHECK(AppPush::forget_device(json{ { "platform", "apns" }, { "token", std::string(64, 'c') } }.dump()).first == 200);
    json devs = AppPush::settings_json()["devices"];
    REQUIRE(devs.size() == 1);
    CHECK(devs[0]["token"] == std::string(64, 'd'));
    // ... and the hub page removes one by its short id (DELETE /hub/apppush?id=).
    CHECK(AppPush::remove(devs[0]["id"].get<std::string>()).first == 200);
    CHECK(AppPush::settings_json()["devices"].empty());
}
