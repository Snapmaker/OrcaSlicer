#pragma once

#include <string>

namespace Slic3r {
namespace GUI {

// The identifiers that travel NEXT TO an encrypted push, in the clear: the APNs `thread-id`, the
// APNs `apns-collapse-id` / FCM `collapse_key`, and the Web Push `Topic`. The hosted push service,
// Apple and Google all see these, so none of them may carry a printer id - for a Bambu printer the
// id is its serial number (privacy audit 2026-10, EdgeSlicerSite PR 4).
//
// Each one is a keyed HMAC-SHA256 under a per-hub secret, truncated:
//   * opaque: without the hub's key nobody can map a value back to a printer, or confirm a guess
//     (an unkeyed SHA-256 of a serial number can be confirmed by anyone who knows the serial);
//   * stable: the same printer always gets the same value on this hub, so iOS still groups one
//     printer's alerts and a second "paused" still replaces the first;
//   * per hub: two hubs watching the same printer produce unrelated values.
// The real printer id travels only inside the encrypted payload (AppPush plaintext_for), which is
// what the app routes a tap with.
//
// The key is 32 random bytes kept in the hub's settings.json ("apppush"."id_key"), next to the
// device rows and the other credentials there, and never sent anywhere.
namespace PushIds {

// 32 random bytes as 64 lower-case hex characters; "" only if the system RNG fails.
std::string new_key_hex();

// True for exactly 64 hex characters.
bool valid_key_hex(const std::string& key_hex);

// The cleartext thread id for a printer: "t" + 16 base64url characters (12 bytes of the HMAC).
// "" for an empty printer id or an invalid key - a caller then sends no thread id at all.
std::string thread_id(const std::string& key_hex, const std::string& printer_id);

// The collapse id / Web Push Topic for one (printer, kind): 24 base64url characters (18 bytes of
// the HMAC) - inside APNs' 64-byte collapse-id cap and RFC 8030's 32-character Topic cap. Never
// empty: with an invalid key it is derived from a per-process random key instead, so it stays
// opaque (only stability across restarts is lost).
std::string collapse_id(const std::string& key_hex, const std::string& printer_id, const std::string& kind);

// The key in force for this process. AppPush::start() sets it from settings.json (minting one the
// first time); WebPush reads it for its Topic header. Before any set_key() call, key() returns a
// random per-process key, never "", so nothing is ever derived without a key.
void        set_key(const std::string& key_hex);
std::string key();

} // namespace PushIds
} // namespace GUI
} // namespace Slic3r
