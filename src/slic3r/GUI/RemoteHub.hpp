#pragma once

#include <string>
#include <utility>
#include <vector>

#include "slic3r/Utils/HubHomeLogic.hpp"

namespace Slic3r {
namespace GUI {

// The "hub": one long-lived helper process per PC (this same executable started with
// --hub) that owns everything a phone or an agent talks to, so it keeps working after the
// slicer window is closed and serves every open slicer instance at once:
//
//   * the token-gated LAN listener on :13640 (phone page, camera list, JSON API),
//   * the bundled go2rtc relay and the Bambu MJPEG relay (camera streams),
//   * the list of running slicer instances (each instance runs a loopback-only JSON API
//     and drops a <pid>.json under <datadir>/hub/instances; the hub proxies
//     /r/<token>/i/<pid>/api/... to it),
//   * uploads from the phone (<datadir>/hub/uploads), opened in an existing instance or
//     in a freshly spawned one.
//
// The first slicer instance that needs it spawns the hub detached; the hub exits on its
// own once phone access is off and no instance has been alive for a minute. State the
// hub needs across restarts (phone on/off, the camera list) lives in
// <datadir>/hub/hub.json and streams.json. The phone link's token lives in settings.json
// next to them, because hub.json is deleted on a clean quit and the link must survive that:
// it is in QR codes people scanned and in home-screen icons they installed.
namespace RemoteHub {

struct Info
{
    bool                     alive { false };
    long                     pid { 0 };
    int                      port { 0 };      // the listener port (LAN when phone is on, loopback otherwise)
    int                      admin_port { 0 }; // the loopback-only control plane: where /hub/* and the hub page answer
    bool                     phone { false }; // LAN listener + /r/<token>/ routes enabled
    std::string              token;
    std::string              secret;          // hub.json's per-run secret for /hub/* (client side only)
    int                      go2rtc_port { 0 };
    int                      relay_port { 0 };
    std::string              version;
    std::vector<std::string> ips; // LAN IPv4 addresses, default-route one first (phone on only)
    std::string              remote_url; // https://<machine>.<tailnet>.ts.net/r/<token>/ while Tailscale remote access is on
    std::string              relay_url;  // the hosted-relay link; always "" in phase 0 (nothing hosts it yet)
    std::string              hubid;      // this data dir's durable relay identity (16 hex chars); see RemoteHub::Testing::hubid_from_public_key
    std::string url() const;      // http://<ip>:<port>/r/<token>/ or ""
    std::string json() const;     // what the Stream tab's phone modal shows: {on, port, token, ips, url}
};

// Process mode: serve until asked to quit or idle. Called from CLI::run for `--hub`.
int run_server(const std::string& token_hint, bool phone_on);

// ---- client side (a slicer instance) ----
Info query(long timeout_s = 3);                                     // is a hub running? (answer within timeout_s)
// What <datadir>/hub says when query() got no answer: no network, one file read and a process
// check. Tells "quit cleanly" (and why) and "crashed" apart from "running but slow to answer".
struct Record
{
    HubHome::Presence   presence { HubHome::Presence::Unknown };
    HubHome::ExitReason exit_reason { HubHome::ExitReason::Unknown }; // NoRecord only
    bool                pid_alive { false };
};
Record record();
std::pair<int, std::string> onvif_discover();                       // ONVIF WS-Discovery via the hub's go2rtc: {http status, body}
Info ensure_running(const std::string& token_hint, bool phone_on); // spawn one if needed; waits for it
Info set_phone(bool on, const std::string& token = ""); // off and on keeps the same link; a valid
                                                        // token seeds a hub that has none yet
Info new_link();                                        // replace the phone link (explicit only):
                                                        // saved links, QR codes and home-screen
                                                        // icons made from the old one stop working
bool post_state(const std::string& json); // full Stream-tab state; remembered for a hub started later
// The camera list the Stream tab last saved: from the running hub, else what this process last
// posted, else <datadir>/hub/streams.json. Empty when there is none. Every window's Stream tab
// opens with this rather than its own localStorage, which is per port and so per window.
std::string saved_state();
// One printer event from this instance's watcher (RemoteEvents.hpp), handed to the hub's
// POST /hub/event on the loopback control plane. Fire and forget: with no hub running it is a
// failed connect and false, never an error anybody sees. The hub assigns the id and the time.
bool post_event(const std::string& event_json);
void quit();

std::string hub_dir();
std::string instances_dir();
std::string uploads_dir();
std::string saves_dir();

// ---- pure parsing helpers, exposed for tests (slic3rutils remote_hub_tests.cpp) ----
// Each of these takes captured text rather than running the command itself, so the parsing rule
// can be exercised with no process, no socket and no real netstat/tailscale on the test machine.
namespace Testing {

// One row of `netstat -ano` for a TCP port in LISTENING state: the PID that holds it, or 0 if the
// port does not appear as LISTENING in `text`. Local address column may be "0.0.0.0:<port>",
// "127.0.0.1:<port>" or "[::]:<port>" depending on what bound it.
long netstat_holder_pid(const std::string& netstat_text, int port);

// The image name (no path, no ".exe" stripped) `tasklist /fi "PID eq <n>" /fo csv /nh` printed
// for that pid, or "" if the row is not there (process exited between the two calls, or the
// filter matched nothing).
std::string tasklist_image_name(const std::string& tasklist_csv_text);

// `tailscale serve status --json`'s "Web" -> "<domain>:443" -> Handlers -> "/" -> Proxy field is
// "http://127.0.0.1:<port>"; this pulls that port back out, or 0 if nothing is being served.
int serve_status_target_port(const std::string& serve_status_json_text);

// ---- remote access (Tailscale) as one state ----
// Everything the hub page needs to draw the remote-access card, and the only place the wording
// lives. The classifier below turns what `tailscale status --json` / `tailscale serve status
// --json` said into exactly one of these, so the page never has to re-derive "installed but not
// signed in" out of three separate booleans.
enum class RemoteAccessState {
    NotInstalled,  // no tailscale CLI on this PC -> offer the download
    NotSignedIn,   // installed, BackendState=NeedsLogin -> `tailscale login`
    NotRunning,    // installed and signed in, backend is Stopped/NoState/... -> start it
    HttpsOff,      // running, but the tailnet issues no certificates -> admin console > DNS
    Serving,       // running, certificates on, Serve is pointed at this hub: the URL works
    Ready,         // running, certificates on, remote access simply switched off
    Error          // anything else the CLI said
};

// What the classifier produced: the state, the one sentence the card shows, and the link the card's
// button opens (empty when there is nothing to open).
struct RemoteAccessInfo
{
    RemoteAccessState state { RemoteAccessState::NotInstalled };
    std::string       message;
    std::string       action_url;
    std::string       action;   // the button's label ("Install Tailscale", "Try again", ...) or ""
};

const char* remote_access_state_name(RemoteAccessState s); // "not_installed", "not_signed_in", ...

// The classifier itself: pure, no CLI, no socket. `installed` is "the tailscale CLI ran at all",
// `backend` is BackendState, `https` is "CertDomains is non-empty", `serving` is "Serve forwards
// the root to our loopback port", `on` is the hub's own remote_on switch, `error` is whatever the
// CLI printed (only used for RemoteAccessState::Error).
RemoteAccessInfo classify_remote_access(bool installed, const std::string& backend, bool https,
                                        bool serving, bool on, const std::string& error);

// ---- hub identity (design_hosted_relay.md section 2) ----
// `hubid` = the first 16 hex characters of SHA-256 over the 32 raw bytes of the Ed25519 public key.
// Durable: minted once per data dir, kept in settings.json, and what a relay will later know this
// hub by. Returns "" if the key is not 32 bytes.
std::string hubid_from_public_key(const std::vector<unsigned char>& public_key);
std::string hubid_from_public_key_hex(const std::string& public_key_hex);

// The identity as it is stored and read back. The private half never leaves settings.json: it is
// not in hub.json, not in /pair, not in the hub info JSON and never logged.
struct HubIdentity
{
    std::string public_hex;   // 32 bytes, hex
    std::string private_hex;  // 32 bytes (the Ed25519 seed), hex - secret
    std::string hubid;        // derived, 16 hex chars
    bool        valid() const { return public_hex.size() == 64 && private_hex.size() == 64 && hubid.size() == 16; }
};

// settings.json round trip for the identity: what gets written under "identity", and what a later
// start reads back out of it. parse returns an invalid HubIdentity when the object is missing or
// malformed, which is the signal to mint a fresh pair.
std::string  identity_settings_dump(const HubIdentity& id);              // the JSON object, as text
HubIdentity  identity_from_settings(const std::string& settings_json_text);

// The whole identity rebuilt from the 32-byte Ed25519 seed (64 hex): the public half and the hubid
// are derived, never trusted. Invalid if the seed is not 64 hex characters. Used by the tests to
// pin RFC 8032's vectors; a running hub mints its pair instead.
HubIdentity  identity_from_seed_hex(const std::string& private_hex);

// ---- the loopback trust of Tailscale Serve's headers (design section 6.6) ----
// Tailscale Serve terminates on loopback and sets Tailscale-User-Login / X-Forwarded-Proto,
// stripping whatever the client sent. Those two headers are therefore trusted from a loopback peer
// and from nowhere else - and phase 1's relayed streams will also arrive on loopback, so this is
// the single place that decides it and the single place a relay stream will have to say "no" in.
// true = the request may keep ts_login / fwd_proto; false = both must be cleared before anything
// looks at them.
bool trusted_proxy_headers(bool peer_is_loopback, bool via_relay);

// ---- the pairing document's identity and origins ----
// The part of pair_json() that is pure: the three named origins and this hub's identity. Pulled
// out so the shape the apps read generically can be exercised without a running hub. `relay` is
// always present and always "" in phase 0 - a client that handles an empty origin correctly today
// needs no change when phase 1 fills it in. The private key is not an argument here and never
// appears in the result.
std::string pair_identity_json(const std::string& lan_url, const std::string& remote_url,
                               const std::string& relay_url, const std::string& hubid,
                               const std::string& public_key_hex);

// ---- joining a camera to the printer it watches (summary_json's "camera" field) ----
// One entry of the Stream tab's camera wall, reduced to what the join needs: its own id, the
// alias a person gave it, the address a "LAN camera on this printer's address" match goes by
// (streams.json's own `ip`), and the host pulled out of its stream URL (`rurl` for a browser
// camera, `rsrc` for a go2rtc one) for a printer whose row never got an `ip` of its own recorded
// under that exact string - a camera someone typed in by hostname, or a printer whose LAN address
// changed since the camera was added.
struct CameraCandidate
{
    std::string id;
    std::string alias;
    std::string ip;
    std::string url_host; // host_of(rurl), or host_of(rsrc) when rurl is empty
};

// Which camera (its id, or "" for none) watches `printer_id`: the camera's own id first, then one
// on the same address as `printer_ip` - by its recorded `ip`, or failing that by the host its own
// stream URL names. Never guesses from the printer's *name*; that join is the app's own alias
// fallback and happens on the phone, not here. Empty strings on either side are never a match, so
// a printer or a camera missing an address does not accidentally pair with every other one that is
// also missing it.
std::string camera_for_printer(const std::string& printer_id, const std::string& printer_ip,
                               const std::vector<CameraCandidate>& cams);

} // namespace Testing

// ---- signing with the hub identity (the push forwarder's X-Hub-Sig) ----
// Ed25519 (RFC 8032, pure - no pre-hash) over exactly the bytes of `message`, as base64url with no
// padding. The caller signs the very string it is about to post and never re-serialises it: a
// signature over "the same JSON" re-dumped is a signature over different bytes. False, with the
// output empty, if the identity's private half is not a 32-byte hex seed.
bool identity_sign(const Testing::HubIdentity& id, const std::string& message, std::string& out_sig_b64url);

// The other half, for the tests and a loopback mock: whether `sig_b64url` (padding tolerated) is a
// valid Ed25519 signature by `public_hex` over exactly `message`.
bool identity_verify(const std::string& public_hex, const std::string& message, const std::string& sig_b64url);

} // namespace RemoteHub
} // namespace GUI
} // namespace Slic3r
