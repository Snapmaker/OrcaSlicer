#pragma once

// Snapmaker Orca: the fork rules around the SnapLog client, which stays a byte-identical upstream copy
// reached through its dependency injection: build switches, consent, a transport guard (https only,
// no signing without a key, paced creates, as the client has no back-off) and path scrubbing.

#include <cstdint>
#include <functional>
#include <string>

#include "slic3r/Utils/SnapLogClient.hpp"

namespace Slic3r { class AppConfig; }

namespace Slic3r { namespace GUI { namespace SnapLogWiring {

using DoRequest = decltype(::Slic3r::SnapLog::v1::SnapLogDeps::do_request);

struct GuardPolicy
{
    bool                     signed_ok         = false; // has_public_key()
    int                      create_burst      = 8;     // token bucket capacity for create requests
    int                      create_refill_sec = 15;    // one token per 15 s
    std::function<int64_t()> steady_ms;                 // injected clock; default steady_clock
};

// SLIC3R_SNAPLOG_ENABLED != 0
bool compiled_in();
// compiled_in() && SNAP_LOG_HMAC_SECRET is not empty
bool has_public_key();
// compiled_in() && privacy_policy_isagree && snaplog_upload, both read with AppConfig::get_bool,
// the reader the two check boxes use.
bool effective_consent(const AppConfig& config);

// Pure. In order: non-https URL -> 403 (a PUT also makes the next create 403); X-Sign without
// signed_ok -> 403; /upload/create takes a bucket token, else 0; /upload/cancel right after a
// bucket-refused create -> 0; else inner. 403 is the client's terminal state, 0 a retried failure.
DoRequest guard_do_request(DoRequest inner, GuardPolicy policy);

// Pure. First line only, every occurrence of home_utf8 replaced by "~", at most 512 bytes, cut on
// a UTF-8 character boundary.
std::string scrub_text(const std::string& text, const std::string& home_utf8);

// Once per process; a second call is a no-op (a second SnapLogClient::init() waits for the old
// workers with the full join deadlines on the calling thread).
void init(AppConfig& config);
// SnapLogClient::set_consent(effective_consent(config))
void apply_consent(const AppConfig& config);
// The selected device id reaches the client hashed, like the printer serial number.
void mirror_device_id(const std::string& dev_id);
// scrub_text with the home directory cached by init().
std::string scrub_reason(const std::string& reason);
// Idempotent.
void shutdown();

}}} // namespace Slic3r::GUI::SnapLogWiring
