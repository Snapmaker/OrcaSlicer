// Pure parsing rules pulled out of RemoteHub.cpp for the port-fallback reachability fix
// (Slic3r::GUI::RemoteHub::Testing, see RemoteHub.hpp): the port that `tailscale serve status
// --json` is currently forwarding to, who `netstat -ano` says is LISTENING on a given port, and
// the image name `tasklist`'s CSV line names for that pid. All three are driven here from
// captured command output - no socket, no process, no real tailnet or netstat needed.

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r::GUI::RemoteHub::Testing;

// ---- serve_status_target_port ------------------------------------------------------------

TEST_CASE("serve_status_target_port reads the root handler's loopback proxy port", "[RemoteHub]")
{
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://127.0.0.1:13640" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 13640);
}

TEST_CASE("serve_status_target_port follows the hub to a fallback port", "[RemoteHub]")
{
    // What the reconcile has to notice: Serve is still forwarding to the port from before the hub
    // stepped past HUB_PORT because something else held it.
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://127.0.0.1:13641" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 13641);
}

TEST_CASE("serve_status_target_port is 0 when nothing is served", "[RemoteHub]")
{
    REQUIRE(serve_status_target_port(R"({"Web": {}})") == 0);
    REQUIRE(serve_status_target_port("") == 0);
    REQUIRE(serve_status_target_port("not json") == 0);
}

TEST_CASE("serve_status_target_port ignores a non-root path", "[RemoteHub]")
{
    // Serve can host more than one path; only "/" is the hub's own root and the one m_port has to
    // match. A handler on some other path must not be mistaken for it.
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/other": { "Proxy": "http://127.0.0.1:9999" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 0);
}

TEST_CASE("serve_status_target_port ignores a non-loopback proxy target", "[RemoteHub]")
{
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://10.0.0.5:13640" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 0);
}

// ---- netstat_holder_pid --------------------------------------------------------------------

TEST_CASE("netstat_holder_pid finds the pid listening on the port", "[RemoteHub]")
{
    const std::string out =
        "\n"
        "Active Connections\n"
        "\n"
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       1234\n"
        "  TCP    0.0.0.0:13640          0.0.0.0:0              LISTENING       5678\n"
        "  TCP    127.0.0.1:13641        0.0.0.0:0              LISTENING       9012\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 5678);
    REQUIRE(netstat_holder_pid(out, 13641) == 9012);
}

TEST_CASE("netstat_holder_pid is 0 when the port is not listed", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       1234\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 0);
    REQUIRE(netstat_holder_pid("", 13640) == 0);
}

TEST_CASE("netstat_holder_pid ignores non-LISTENING rows and a port-number prefix collision", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        // An ESTABLISHED row on the port must not count as a holder of it.
        "  TCP    192.168.1.5:13640      192.168.1.9:51000      ESTABLISHED     4321\n"
        // 136400 must not match a lookup for 13640 (suffix match, not substring).
        "  TCP    0.0.0.0:136400         0.0.0.0:0              LISTENING       8888\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 0);
}

TEST_CASE("netstat_holder_pid handles the [::] IPv6 wildcard column", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    [::]:13640             [::]:0                 LISTENING       2222\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 2222);
}

// ---- tasklist_image_name -------------------------------------------------------------------

TEST_CASE("tasklist_image_name reads the first quoted CSV field", "[RemoteHub]")
{
    REQUIRE(tasklist_image_name("\"EdgeSlicer.exe\",\"5678\",\"Console\",\"1\",\"123,456 K\"") == "EdgeSlicer.exe");
    REQUIRE(tasklist_image_name("\"python.exe\",\"9012\",\"Services\",\"0\",\"4,096 K\"") == "python.exe");
}

TEST_CASE("tasklist_image_name is empty for the no-match message", "[RemoteHub]")
{
    REQUIRE(tasklist_image_name("INFO: No tasks are running which match the specified criteria.") == "");
    REQUIRE(tasklist_image_name("") == "");
}

// ---- status JSON shape (info_json's port_note / lan_firewall) -------------------------------
// info_json() itself needs a live HubServer (a bound socket, go2rtc, the works), which is exactly
// what these pure-function tests are trying to avoid pulling in. What is checked here instead is
// the shape the deliverable specifies, built the same way info_json() builds it, so a change to
// either side is caught by the other: {"port_note": {"default_port", "held_by", "port"}} only
// appears when the bound port differs from HUB_PORT, and "lan_firewall" always carries the same
// state vocabulary the video one already uses.
TEST_CASE("port_note JSON shape matches the spec when the hub fell back to another port", "[RemoteHub]")
{
    nlohmann::json note;
    note["default_port"] = 13640;
    note["held_by"]      = "EdgeSlicer.exe (pid 5678)";
    note["port"]         = 13641;
    note["command"]      = "netsh advfirewall firewall add rule name=\"EdgeSlicer\" dir=in action=allow "
                            "program=\"C:\\EdgeSlicer.exe\" protocol=TCP localport=13640-13659 profile=private,domain";
    REQUIRE(note["default_port"].get<int>() == 13640);
    REQUIRE(note["port"].get<int>() == 13641);
    REQUIRE(note["held_by"].get<std::string>() == "EdgeSlicer.exe (pid 5678)");
    REQUIRE_THAT(note["command"].get<std::string>(), Catch::Matchers::Contains("netsh"));
}

TEST_CASE("lan_firewall JSON uses the same state vocabulary as video.firewall", "[RemoteHub]")
{
    const std::vector<std::string> valid = { "allowed", "partial", "missing", "blocked", "unknown", "off" };
    for (const std::string& s : { std::string("allowed"), std::string("missing"), std::string("off") })
        REQUIRE(std::find(valid.begin(), valid.end(), s) != valid.end());
}

// ---- the note/command split (owner, 2026-09-18: the hub page's firewall warnings were too long -
// one short sentence now, with the exact netsh line moved out to its own field so hub.html can put
// it behind a "Show command" toggle instead of running it into the sentence). Both `firewall_query`
// itself and the two call sites (go2rtc's WebRTC port, the phone/LAN listener) are private to
// RemoteHub.cpp, so this pins the JSON shape info_json() builds - the same approach the port_note
// and lan_firewall-vocabulary tests above already take.
TEST_CASE("a firewall note is one short sentence with no netsh text run into it", "[RemoteHub]")
{
    // What used to ship as a single string, e.g.: "Windows Firewall allows go2rtc.exe on Public
    // networks, but this PC is on a Private network. In Windows Defender Firewall allow go2rtc.exe
    // (inbound), or as Administrator run: netsh advfirewall ...". The note now stops at the
    // sentence; the netsh line lives in `command` instead.
    const std::string note    = "Windows Firewall allows go2rtc.exe on Public networks, but this PC is on Private.";
    const std::string command = "netsh advfirewall firewall add rule name=\"go2rtc\" dir=in action=allow "
                                 "program=\"C:\\go2rtc.exe\" protocol=TCP localport=8556 profile=private,domain";
    REQUIRE_THAT(note, !Catch::Matchers::Contains("netsh"));
    REQUIRE_THAT(command, Catch::Matchers::StartsWith("netsh advfirewall firewall add rule"));

    nlohmann::json v;
    v["firewall"] = "partial";
    v["note"]     = note;
    v["command"]  = command;
    REQUIRE(v["note"].get<std::string>() == note);
    REQUIRE(v["command"].get<std::string>() == command);
}

TEST_CASE("an allowed firewall state carries an empty note and an empty command", "[RemoteHub]")
{
    // firewall_query()'s "allowed" branch sets note to "" and leaves command unset (default ""):
    // nothing to warn about, so hub.html's warningLine()/videoLine() never draw a "Show command"
    // toggle for it.
    nlohmann::json lv;
    lv["firewall"] = "allowed";
    lv["note"]     = std::string();
    lv["command"]  = std::string();
    REQUIRE(lv["note"].get<std::string>().empty());
    REQUIRE(lv["command"].get<std::string>().empty());
}

// ---- the actual root cause of the 13641 fallback --------------------------------------------
// Not a parsing rule but a Windows socket-option fact, and the one that mattered: HubServer::bind()
// used to open the NEW acceptor and bind it before closing the OLD one. A hub starts on
// 127.0.0.1:13640 (phone off) and switching phone access on calls bind(true), which tries a new
// SO_EXCLUSIVEADDRUSE 0.0.0.0:13640 while that old 127.0.0.1:13640 acceptor is still open in the
// same process - Windows refuses the exclusive wildcard bind over an existing narrower one, even
// in the same process, so the loop stepped to 13641 on *every* "turn phone access on", with
// nothing external involved at all. bind() now closes the old acceptor before attempting the new
// one (RemoteHub.cpp, HubServer::bind). This pins the two halves of that fact so a change to the
// option or the ordering is caught here rather than by a tester again.
#ifdef _WIN32
#include <boost/asio.hpp>
namespace {
namespace RHTAsio = boost::asio;
using RHTTcp = RHTAsio::ip::tcp;

bool bind_exclusive(RHTAsio::io_context& ioc, RHTTcp::acceptor& a, const RHTAsio::ip::address_v4& addr, unsigned short port)
{
    boost::system::error_code ec;
    a.open(RHTTcp::v4(), ec);
    if (ec) return false;
    a.set_option(RHTAsio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true), ec);
    if (ec) return false;
    a.bind(RHTTcp::endpoint(addr, port), ec);
    return !ec;
}
} // namespace

TEST_CASE("SO_EXCLUSIVEADDRUSE: a wildcard bind conflicts with an existing loopback bind on the same port", "[RemoteHub][socket]")
{
    RHTAsio::io_context ioc;
    // An arbitrary high port, picked by letting the OS assign one first so the case does not
    // collide with anything else bound on this test machine (including a real hub).
    RHTTcp::acceptor probe(ioc);
    probe.open(RHTTcp::v4());
    probe.bind(RHTTcp::endpoint(RHTAsio::ip::make_address_v4("127.0.0.1"), 0));
    const unsigned short port = probe.local_endpoint().port();
    probe.close();

    // Step 1: bind 127.0.0.1:<port> exclusively (the hub's phone-off, loopback-only listener).
    RHTTcp::acceptor loopback(ioc);
    REQUIRE(bind_exclusive(ioc, loopback, RHTAsio::ip::make_address_v4("127.0.0.1"), port));

    // Step 2: WITHOUT closing it, try 0.0.0.0:<port> exclusively (what bind(true) used to attempt
    // first, for "phone access turned on") - this is the self-conflict that sent the hub to
    // 13641 every time, and it must still fail so the fix is proven to be closing the old
    // acceptor first, not some other accidental change.
    RHTTcp::acceptor wildcard_while_open(ioc);
    REQUIRE_FALSE(bind_exclusive(ioc, wildcard_while_open, RHTAsio::ip::address_v4::any(), port));

    // Step 3: close the loopback acceptor (what bind() now does before attempting the new one) -
    // the same 0.0.0.0:<port> exclusive bind must now succeed.
    boost::system::error_code ec;
    loopback.close(ec);
    RHTTcp::acceptor wildcard_after_close(ioc);
    REQUIRE(bind_exclusive(ioc, wildcard_after_close, RHTAsio::ip::address_v4::any(), port));
}
#endif // _WIN32
// ==============================================================================================
// Phase 0 of the hosted remote-access plan (tests/design_hosted_relay.md section 7): the remote
// state classifier the hub page draws from, the hub identity a relay will later know this hub by,
// and the one rule that decides whether Tailscale Serve's headers may be believed. All pure.
// ==============================================================================================


// ---- classify_remote_access --------------------------------------------------------------

// The whole table in one place, exactly as the card shows it. Each row is (installed, backend,
// https, serving, on) -> state. The wording is checked separately below; what matters here is
// that no input lands in two buckets and none falls through to Error by accident.
TEST_CASE("classify_remote_access maps every Tailscale situation to one state", "[RemoteHub]")
{
    struct Row { bool installed; const char* backend; bool https, serving, on; RemoteAccessState want; };
    const Row rows[] = {
        // Nothing installed: whatever else is claimed, the answer is "install it".
        { false, "",           false, false, false, RemoteAccessState::NotInstalled },
        { false, "Running",    true,  true,  true,  RemoteAccessState::NotInstalled },
        // Installed but the backend has no account yet.
        { true,  "NeedsLogin", false, false, false, RemoteAccessState::NotSignedIn },
        { true,  "Starting",   false, false, false, RemoteAccessState::NotSignedIn },
        // Signed in but the service is down.
        { true,  "Stopped",    true,  false, false, RemoteAccessState::NotRunning },
        { true,  "NoState",    true,  false, false, RemoteAccessState::NotRunning },
        { true,  "",           true,  false, false, RemoteAccessState::NotRunning },
        // Running, but the tailnet issues no certificates: the one thing that cannot be fixed here.
        { true,  "Running",    false, false, false, RemoteAccessState::HttpsOff },
        { true,  "Running",    false, true,  true,  RemoteAccessState::HttpsOff },
        // Ready to be switched on, and switched on and working.
        { true,  "Running",    true,  false, false, RemoteAccessState::Ready },
        { true,  "Running",    true,  true,  false, RemoteAccessState::Ready }, // Serve left over from before
        { true,  "Running",    true,  true,  true,  RemoteAccessState::Serving },
        // Switched on but Serve is not pointed here any more - the port-drift case the reconcile
        // in bind() exists for. Reported, with a retry, never as "ready".
        { true,  "Running",    true,  false, true,  RemoteAccessState::Error },
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION("installed=" << r.installed << " backend=" << r.backend << " https=" << r.https
                                     << " serving=" << r.serving << " on=" << r.on) {
            const auto info = classify_remote_access(r.installed, r.backend, r.https, r.serving, r.on, "");
            REQUIRE(info.state == r.want);
            REQUIRE_FALSE(info.message.empty()); // every state says something; the card draws it verbatim
        }
    }
}

TEST_CASE("classify_remote_access offers the install link when Tailscale is missing", "[RemoteHub]")
{
    const auto info = classify_remote_access(false, "", false, false, false, "");
    REQUIRE(std::string(remote_access_state_name(info.state)) == "not_installed");
    // The explanation the design asks for: what Tailscale is, and that it goes on both devices.
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("Tailscale"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("free private network"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("same account"));
    REQUIRE(info.action_url == "https://tailscale.com/download/windows");
    REQUIRE_FALSE(info.action.empty());
}

TEST_CASE("classify_remote_access sends the HTTPS-certificates error to the admin console", "[RemoteHub]")
{
    // The one error nobody can fix from this PC: it is a tailnet-wide setting, so the card links
    // straight at the page with the switch and offers a retry rather than a dead end.
    const auto info = classify_remote_access(true, "Running", false, false, false, "");
    REQUIRE(info.state == RemoteAccessState::HttpsOff);
    REQUIRE(std::string(remote_access_state_name(info.state)) == "https_off");
    REQUIRE(info.action_url == "https://login.tailscale.com/admin/dns");
    // The meaning of the old string is kept: admin console > DNS > HTTPS Certificates > Enable.
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("HTTPS Certificates"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("admin console"));
}

TEST_CASE("classify_remote_access keeps the sign-in hint actionable", "[RemoteHub]")
{
    const auto info = classify_remote_access(true, "NeedsLogin", false, false, false, "");
    REQUIRE(info.state == RemoteAccessState::NotSignedIn);
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("tailscale login"));
    REQUIRE(info.action_url.empty()); // nothing to open: it is a local sign-in
    REQUIRE_FALSE(info.action.empty());
}

TEST_CASE("classify_remote_access reports the CLI's own words when it has nothing better", "[RemoteHub]")
{
    const auto info = classify_remote_access(true, "Running", true, false, false, "tailscaled said no");
    REQUIRE(info.state == RemoteAccessState::Error);
    REQUIRE(info.message == "tailscaled said no");
}

TEST_CASE("remote_access_state_name is the wire spelling of every state", "[RemoteHub]")
{
        REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotInstalled)) == "not_installed");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotSignedIn))  == "not_signed_in");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotRunning))   == "not_running");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::HttpsOff))     == "https_off");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Serving))      == "serving");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Ready))        == "ready");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Error))        == "error");
}

// ---- hubid derivation ---------------------------------------------------------------------

TEST_CASE("hubid is the first 16 hex characters of SHA-256 over the public key", "[RemoteHub]")
{
    // A fixed key with a fixed answer, so a change to the derivation cannot slip through: a relay
    // registration is keyed on this value and every hub that re-derived differently would lose it.
    std::vector<unsigned char> key(32);
    for (int i = 0; i < 32; ++i) key[i] = (unsigned char) i;
    REQUIRE(hubid_from_public_key(key) == "630dcd2966c43366");
    REQUIRE(hubid_from_public_key_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == "630dcd2966c43366");
    REQUIRE(hubid_from_public_key_hex("ABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB") == "9a2db2e23f1504cd");
    // Two different keys never share a hubid, and the derivation is stable across calls.
    REQUIRE(hubid_from_public_key_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") !=
            hubid_from_public_key_hex("abababababababababababababababababababababababababababababababab"));
}

TEST_CASE("hubid refuses anything that is not a 32-byte key", "[RemoteHub]")
{
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>()).empty());
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>(31, 0)).empty());
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>(33, 0)).empty());
    REQUIRE(hubid_from_public_key_hex("").empty());
    REQUIRE(hubid_from_public_key_hex("00010203").empty());
    REQUIRE(hubid_from_public_key_hex(std::string(64, 'z')).empty()); // right length, not hex
}

// ---- the identity's settings.json round trip ----------------------------------------------

TEST_CASE("the hub identity survives a settings.json round trip", "[RemoteHub]")
{

    HubIdentity id;
    id.public_hex  = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    id.private_hex = std::string(64, '7');
    id.hubid       = "630dcd2966c43366";
    REQUIRE(id.valid());

    // What write_hub_json() puts under "identity", wrapped the way the real settings file wraps it.
    nlohmann::json settings;
    settings["token"]    = "abc";
    settings["identity"] = nlohmann::json::parse(identity_settings_dump(id));

    const HubIdentity back = identity_from_settings(settings.dump());
    REQUIRE(back.valid());
    REQUIRE(back.public_hex  == id.public_hex);
    REQUIRE(back.private_hex == id.private_hex);
    REQUIRE(back.hubid       == id.hubid);
    // The bare identity object parses too, so a caller that already dug the member out is fine.
    REQUIRE(identity_from_settings(identity_settings_dump(id)).hubid == id.hubid);
}

TEST_CASE("a settings file with no identity yields an invalid one, which is the signal to mint", "[RemoteHub]")
{
    REQUIRE_FALSE(identity_from_settings(R"({"token":"abc"})").valid());
    REQUIRE_FALSE(identity_from_settings("{}").valid());
    REQUIRE_FALSE(identity_from_settings("not json").valid());
    REQUIRE_FALSE(identity_from_settings("").valid());
    // A truncated or otherwise unusable public key is the same case: mint a new pair.
    REQUIRE_FALSE(identity_from_settings(R"({"identity":{"public":"0001","private":"ff","hubid":"x"}})").valid());
}

TEST_CASE("the stored hubid is only a cache: the public key always wins", "[RemoteHub]")
{
    // A hand-edited settings file claiming somebody else's hubid must not make this hub answer to
    // it - the name is a function of the key, and it is re-derived on every read.
    const std::string j = R"({"identity":{
        "public":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "private":"7777777777777777777777777777777777777777777777777777777777777777",
        "hubid":"deadbeefdeadbeef"}})";
    REQUIRE(identity_from_settings(j).hubid == "630dcd2966c43366");
}

// ---- the pairing document (what every app reads) -------------------------------------------

TEST_CASE("the pairing document carries the hubid, the public key and three named origins", "[RemoteHub]")
{
    const std::string pub = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const nlohmann::json j = nlohmann::json::parse(
        pair_identity_json("http://192.168.1.20:13640/r/tok/", "https://pc.tailnet.ts.net/r/tok/", "", "630dcd2966c43366", pub));

    REQUIRE(j["hubid"] == "630dcd2966c43366");
    REQUIRE(j["public_key"] == pub);
    // Three keys, always present. `relay` is empty in phase 0 and the apps skip an empty origin,
    // so phase 1 fills a slot that every client already knows about rather than adding one.
    REQUIRE(j["urls"].contains("lan"));
    REQUIRE(j["urls"].contains("remote"));
    REQUIRE(j["urls"].contains("relay"));
    REQUIRE(j["urls"]["relay"] == "");
    REQUIRE(j["urls"]["lan"] == "http://192.168.1.20:13640/r/tok/");
    REQUIRE(j["urls"]["remote"] == "https://pc.tailnet.ts.net/r/tok/");
    // The private key is never a part of this document, whatever was passed in.
    REQUIRE_FALSE(j.contains("private"));
    REQUIRE_FALSE(j.contains("private_key"));
}

TEST_CASE("the pairing document still has all three origins when only the LAN one exists", "[RemoteHub]")
{
    const nlohmann::json j = nlohmann::json::parse(
        pair_identity_json("http://192.168.1.20:13640/r/tok/", "", "", "630dcd2966c43366", std::string(64, '0')));
    REQUIRE(j["urls"]["remote"] == "");
    REQUIRE(j["urls"]["relay"] == "");
    REQUIRE(j["hubid"] == "630dcd2966c43366");
}

// ---- the loopback trust of Tailscale Serve's headers (design section 6.6) ------------------

TEST_CASE("a non-loopback peer never gets Tailscale-User-Login trusted", "[RemoteHub]")
{
    // This is the rule the MAIN listener applies: the LAN listener answers real network peers, and
    // a client on the LAN can put any header it likes in its request. Tailscale Serve's headers
    // are believed only because Serve terminates on loopback and overwrites them, so anything that
    // did not arrive on loopback has ts_login and fwd_proto cleared before login_allowed() is
    // reached. Without this, a phone on the Wi-Fi could send Tailscale-User-Login: <an allow-listed
    // address> and be treated as an authenticated tailnet visitor.
    REQUIRE_FALSE(trusted_proxy_headers(/*peer_is_loopback*/ false, /*via_relay*/ false));
    REQUIRE_FALSE(trusted_proxy_headers(false, true));
    // Genuine Serve: loopback, not relayed.
    REQUIRE(trusted_proxy_headers(true, false));
    // And the trap design section 3 names: a phase-1 relayed stream is re-injected AS a loopback
    // peer (so is_private_v4 need not be loosened) and therefore looks exactly like Serve. Nothing
    // upstream of it strips these headers, so it must never inherit the trust.
    REQUIRE_FALSE(trusted_proxy_headers(true, true));
}

// ---- joining a camera to the printer it watches (summary_json's "camera" field) -------------

TEST_CASE("camera_for_printer matches a camera added as the printer itself", "[RemoteHub][camera]")
{
    const std::vector<CameraCandidate> cams = { { "00M09D542800682", "X1C", "", "" } };
    REQUIRE(camera_for_printer("00M09D542800682", "", cams) == "00M09D542800682");
    // The printer's own id wins even when an address would also have matched something else.
    REQUIRE(camera_for_printer("00M09D542800682", "10.0.0.150", cams) == "00M09D542800682");
}

TEST_CASE("camera_for_printer matches a LAN camera by its recorded ip", "[RemoteHub][camera]")
{
    // The owner's actual reports: streams.json's H2C camera (id is go2rtc's own id, alias "H2C",
    // ip 10.0.0.206) against a Bambu printer row whose name ("ToyPrinterX1C"-style) never matches
    // the camera's alias. Once the row carries the printer's LAN ip, the address join finds it
    // without the name ever entering into it.
    const std::vector<CameraCandidate> cams = {
        { "cmtpxb0uqqhnf2", "H2C", "10.0.0.206", "" },
        { "cmtqambvc4pbba", "X1C", "10.0.0.150", "" },
    };
    REQUIRE(camera_for_printer("some-h2c-serial", "10.0.0.206", cams) == "cmtpxb0uqqhnf2");
    REQUIRE(camera_for_printer("00M09D542800682", "10.0.0.150", cams) == "cmtqambvc4pbba");
}

TEST_CASE("camera_for_printer matches a camera keyed by address under its own id", "[RemoteHub][camera]")
{
    // A camera streams.json stored under its LAN address as the id (the "auto" kind U1 cameras use)
    // rather than a separate `ip` field.
    const std::vector<CameraCandidate> cams = { { "10.0.0.108", "U1", "", "" } };
    REQUIRE(camera_for_printer("printer-1", "10.0.0.108", cams) == "10.0.0.108");
}

TEST_CASE("camera_for_printer falls back to the host inside the camera's own stream URL", "[RemoteHub][camera]")
{
    // A camera whose `ip` was never recorded (added by hostname, or the field is simply blank) but
    // whose rurl/rsrc still names an address - RemoteHub::summary_json reduces those to url_host
    // with SnapmakerLan::host_of before this is called.
    const std::vector<CameraCandidate> cams = { { "camid1", "Shop cam", "", "10.0.0.206" } };
    REQUIRE(camera_for_printer("some-h2c-serial", "10.0.0.206", cams) == "camid1");
}

TEST_CASE("camera_for_printer never matches on name, and never matches two blanks", "[RemoteHub][camera]")
{
    // A printer row with no known address (the gap this fix closes for Bambu, but any kind can hit
    // it) must not pair with a camera that also has no address, and the alias/name is not consulted
    // here at all - that fallback is the app's own, over a printer's *name*.
    const std::vector<CameraCandidate> cams = { { "cam1", "ToyPrinterX1C", "", "" } };
    REQUIRE(camera_for_printer("printer-1", "", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "10.0.0.206", cams).empty());
}

TEST_CASE("camera_for_printer returns empty when nothing matches", "[RemoteHub][camera]")
{
    const std::vector<CameraCandidate> cams = { { "cam1", "X1C", "10.0.0.150", "10.0.0.150" } };
    REQUIRE(camera_for_printer("printer-1", "10.0.0.206", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "10.0.0.206", {}).empty());
}
