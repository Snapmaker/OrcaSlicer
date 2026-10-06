#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <utility>

namespace Slic3r {
namespace GUI {

// Relay notifications (P5). The hub forwards printer events to a push relay the person picked -
// ntfy, Pushover or a plain webhook - so the phone is told with the page closed and with no
// service worker, no secure context and no manifest. It works for a LAN-only user, because the
// PC is the one making the outbound call, not the phone.
//
// This module lives in the hub process only. It owns the destination list in
// <datadir>/hub/settings.json (the file that survives a clean quit, next to the phone token),
// a worker thread and a queue: deliver() is called from a request thread and must never block
// it, so it appends to the queue and returns.
//
// The topic of an ntfy destination, a Pushover user key or app token and a webhook's header
// value are credentials - an ntfy topic in particular *is* the password, anybody holding it can
// read and publish. So: never logged, always masked to their last 4 characters in anything this
// module hands back, never reachable from the phone origin (/hub/* is loopback + X-Hub-Secret).
namespace RemoteNotify {

// One event, in the shape P4's watcher produces (see the P5 note in
// docs/superpowers/specs/2026-09-03-phone-mobile-capabilities-research.md):
//   {id, time, instance, printer:{id,name,kind}, kind, severity, title, text, code?, job?}
// Queued and sent by the worker; returns immediately. Safe to call before configure().
void deliver(const nlohmann::json& event);
// Whether an event would go anywhere at all: a configured destination, a subscribed browser or a
// registered app. The hub asks before claiming a printer's notifications (HubPushOwner).
bool has_destinations();

// Load the destinations the hub read out of settings.json (the "notify" object, or a null/absent
// value on a data dir that has none) and start the worker. Called once from HubServer::start().
void start(const nlohmann::json& saved);

// Stop the worker and wait briefly for an in-flight send. Called from HubServer::shutdown().
void stop();

// What write_hub_json() puts back under settings.json's "notify" key: the destinations with
// their secrets, exactly as configured. This is the *only* place secrets leave the module.
nlohmann::json settings_json();

// What GET /hub/notify answers: the same list with every credential masked, plus per-destination
// delivery status (ok/failing, consecutive failures, last error, last send).
nlohmann::json masked_json();

// POST /hub/notify: add or update one destination from the page's JSON body. Returns {status,
// body} - 200 with the masked list, or 400 with a plain reason. A field the caller leaves out on
// an update keeps its stored value, which is how the page can save a destination it only ever
// saw masked. The per-kind filter is read from either `kinds` (an allow-list that replaces it)
// or `events` (a {kind: bool} patch over it); see RemoteEvents.hpp for the canonical kind list.
std::pair<int, std::string> configure(const std::string& body);

// DELETE /hub/notify?id=<id>.
std::pair<int, std::string> remove(const std::string& id);

// POST /hub/notify/test?id=<id>: build a synthetic "test" event, send it to that one destination
// on this thread (people are waiting for the answer) and report what the relay said - {ok,
// status, error} with the relay's own HTTP status, so a wrong topic or a dead server is visible
// on the page instead of vanishing into the queue. The test wears a kind this destination allows
// and the answer also carries {kind, kinds, events}, so the page can show what is switched on.
std::pair<int, std::string> test(const std::string& id, const std::string& phone_link);

// The hub's phone link, used for ntfy's Click: header and Pushover's url field so the
// notification opens the phone page. Empty (and then omitted) while phone access is off.
// Both phone links, whenever the hub's idea of them changes. `remote` (Tailscale) is the one a
// destination's single Click/url gets, because it works from anywhere; `lan` is appended to the
// message body so somebody at home can take the fast path instead. Either may be empty.
void set_phone_links(const std::string& remote, const std::string& lan);

} // namespace RemoteNotify
} // namespace GUI
} // namespace Slic3r
