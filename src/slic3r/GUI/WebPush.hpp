#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

// Web Push (P7). The long-term notification path: the phone page, installed to the home screen
// or open on a secure origin, subscribes to its browser's own push service, and the hub posts
// the event straight to that endpoint. No relay account anywhere - the only third party is the
// push service the browser already picked (Apple, Google, Mozilla), and it never sees the
// payload, because the body is encrypted to a key pair the phone made and only the phone can
// read (RFC 8291, aes128gcm).
//
// Two keys matter here and they are easy to confuse:
//   * the VAPID key pair is the *hub's* identity. It is generated once, stored in
//     <datadir>/hub/settings.json, and its public half goes to the page as
//     applicationServerKey. The private half is a credential: masked everywhere, never served
//     under /r/, never logged.
//   * a subscription's p256dh/auth are the *phone's*. They come from the browser and are only
//     useful together with that phone's endpoint, so they are stored as given and masked on the
//     hub page for tidiness rather than for secrecy.
//
// This module lives in the hub process only. The sender runs on RemoteNotify's worker thread -
// deliver() is called from there, never from a request thread.
namespace WebPush {

// Load the "webpush" object out of settings.json (or a null value on a data dir that has none),
// generating the VAPID key pair the first time. Called once from HubServer::start().
void start(const nlohmann::json& saved);

// Nothing to join - the sender borrows RemoteNotify's worker - but clears the in-memory state.
void stop();

// What write_hub_json() puts back under settings.json's "webpush" key: the VAPID key pair and
// every subscription, in the clear. The only place the private key leaves the module.
nlohmann::json settings_json();

// What GET /hub/push answers: the VAPID public key, the subscription count, one masked row per
// subscription (endpoint host + last 6 characters of the endpoint, last result, last send) and
// the minimum severity. Never the private key.
nlohmann::json masked_json();

// The VAPID public key, base64url of the uncompressed P-256 point - what the page passes as
// applicationServerKey. Served by GET /r/<token>/push/key.
std::string public_key_b64u();

// POST /r/<token>/push/subscription with the browser's PushSubscription JSON
// ({endpoint, keys:{p256dh, auth}}). Idempotent on the endpoint: re-subscribing the same phone
// updates the row instead of adding a second one. Returns {status, body}.
std::pair<int, std::string> add_subscription(const std::string& body);

// DELETE /r/<token>/push/subscription with the same body (or ?endpoint=): the page telling the
// hub it unsubscribed. Not authenticated beyond the token, so it can only remove a subscription
// whose full endpoint the caller already knows.
std::pair<int, std::string> remove_subscription(const std::string& endpoint);

// DELETE /hub/push?id=<id>: the hub page removing a row by its short id.
std::pair<int, std::string> remove(const std::string& id);

// A new phone link (HubServer::new_link): every subscription was made from the phone page under
// the old token, so all of them are forgotten - a phone that kept the old page open would
// otherwise go on receiving this hub's notifications. Answers how many were forgotten.
int forget_all_subscriptions();

// POST /hub/push/options {"min_severity":"info|warning|error", "enabled":bool, and the per-kind
// filter as either "kinds":[...] or "events":{kind:bool}} - see RemoteEvents.hpp for the list.
std::pair<int, std::string> set_options(const std::string& body);

// POST /hub/push/test: build a synthetic event and send it to every subscription on this thread
// (people are waiting for the answer). Reports per-subscription {ok, status, error}.
std::pair<int, std::string> test(const std::string& phone_link);

// Called from RemoteNotify's worker for every event that passes the hub's own filter. Sends to
// all stored subscriptions, prunes any that answers 404 or 410, and retries transport errors and
// 5xx the way the relay senders do.
void deliver(const nlohmann::json& event);

// The hub's phone link, used as the notification's click target. Empty while phone access is off
// (the notification still shows, it just has nowhere to go).
// Both phone links. The payload carries the pair; the service worker opens whichever one this
// phone's owner chose (see resources/web/orca/sw.js), falling back to the remote one.
void set_phone_links(const std::string& remote, const std::string& lan);

// True when at least one phone is subscribed - lets the hub page and the event path skip the
// work entirely.
bool has_subscriptions();

// True once, if anything changed that <datadir>/hub/settings.json does not yet know about - a
// phone subscribed, or a dead subscription was pruned on the sender thread. The hub's own two
// second loop asks, so the sender never has to reach back into HubServer to save.
bool consume_dirty();

// ---------------------------------------------------------------------------------------------
// Primitives, exposed so the gate can cross-check them against the RFC test vectors through the
// debug-only route (SNORCA_DEBUG_ROUTES=1). Nothing on the normal path calls these directly.

// RFC 8291 aes128gcm. Pass empty strings for salt/ephemeral private key to get fresh random ones
// (the normal path); pass the vector's values to reproduce appendix A byte for byte.
bool encrypt(const std::string& p256dh_b64u,
             const std::string& auth_b64u,
             const std::string& plaintext,
             const std::string& fixed_salt_b64u,
             const std::string& fixed_ephemeral_priv_b64u,
             std::string&       out_body,
             std::string&       out_error);

// RFC 8292 VAPID: the ES256 JWT for one endpoint origin, signed with the hub's private key.
bool vapid_jwt(const std::string& audience,
               const std::string& subject,
               long long          expires_in_seconds,
               std::string&       out_jwt,
               std::string&       out_error);

// GET/POST /hub/push/debug (SNORCA_DEBUG_ROUTES=1 only): {"op":"encrypt"|"jwt", ...}.
std::pair<int, std::string> debug_op(const std::string& body);

} // namespace WebPush
} // namespace GUI
} // namespace Slic3r
