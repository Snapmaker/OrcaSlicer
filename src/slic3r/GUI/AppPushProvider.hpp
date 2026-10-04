#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace Slic3r {
namespace GUI {
namespace AppPush {

// One already-encrypted notification, on its way to one device.
//
// Everything a provider is allowed to know is in here, and it is deliberately little: an opaque
// device token, a blob, a size, a collapse id and a couple of routing hints. The plaintext was
// encrypted to the device's own key before this struct was built, so a provider - and the push
// service behind it - cannot read the notification even in principle. That is what makes a hosted
// relay a possible third implementation of this interface rather than a redesign: it would see
// exactly what APNs and FCM see.
struct PushRequest
{
    std::string platform;          // "apns" or "fcm": the device row's platform (the hosted service needs it)
    std::string device_token;      // APNs hex device token, or an FCM registration token
    std::string env;               // APNs only: "production" or "sandbox" (see risk R3)
    std::string bundle;            // APNs apns-topic
    std::string ciphertext_b64u;   // the aes128gcm body, base64url, ready for the JSON envelope
    std::string collapse_id;       // truncated SHA-256 of "<printer id>|<kind>" - leaks nothing
    std::string thread_id;         // the printer id, so iOS groups a printer's alerts together
    int         priority { 10 };   // APNs 10 or 5; FCM maps this to "high" or "normal"
    int         ttl_seconds { 1800 };
    // APNs only, and empty unless the device asked for it (`level_hint` on /push/device): the
    // cleartext `aps.interruption-level` ("time-sensitive"). The app's Notification Service
    // Extension sets the level itself after decrypting; this is the fallback for when that does
    // not take effect, and it tells Apple no more than apns-priority 10 already does.
    std::string interruption_level;
};

struct PushResult
{
    bool        ok { false };
    int         status { 0 };   // 0 means the request never got an answer: DNS, TLS, no route
    bool        gone { false }; // the platform says this device is dead; prune the row
    bool        credential_expired { false }; // 403 ExpiredProviderToken / 401: re-mint, retry once
    std::string error;
    std::string host;           // which host answered, so a BadDeviceToken is diagnosable
};

// A destination for already-encrypted notifications. ApnsProvider and FcmProvider use this hub's own
// keys; HostedProvider (HostedPush.hpp) hands the notification to the EdgeSlicer push service.
struct Provider
{
    virtual ~Provider() = default;

    // "apns" or "fcm" - the same string a device row carries as its platform.
    virtual const char* name() const = 0;

    // Whether this provider could send right now, and if not, why in a sentence a person can act
    // on. This is where the HTTP/2 check lives for APNs, so a hub built against a libcurl without
    // nghttp2 says so on its own page instead of failing every push with a transport error.
    virtual bool available(std::string& why) const = 0;

    // Re-read whatever the settings point at (a .p8 file, a service-account JSON) and drop any
    // cached credential. Called from AppPush::start() and whenever the options change.
    virtual void configure(const std::string& config_json) = 0;

    // Sends one notification, synchronously, on the caller's thread. Never called from a request
    // thread: AppPush rides RemoteNotify's worker, exactly as WebPush does.
    virtual PushResult send(const PushRequest& req) = 0;
};

std::unique_ptr<Provider> make_apns_provider();
std::unique_ptr<Provider> make_fcm_provider();

// ------------------------------------------------------------------ shared helpers ----
//
// Small pieces both providers need. They live in AppPush.cpp so the two provider files stay about
// their own platform's contract and nothing else.
namespace detail {

std::string b64url(const unsigned char* data, size_t len);
std::string b64url(const std::string& s);

// Load an unencrypted PKCS#8 PEM private key - which is what both Apple's .p8 and the FCM service
// account's `private_key` field are. WebPush.cpp's ec_from_private() cannot be used for either:
// it takes the raw 32-byte scalar the VAPID key is stored as, and a PEM is not that.
// Returns an EVP_PKEY* the caller must EVP_PKEY_free, or nullptr with `err` set.
void* load_pkcs8_pem(const std::string& pem, std::string& err);
void  free_pkey(void* pkey);

// A JOSE-compact JWT: base64url(header) "." base64url(claims) "." base64url(signature).
// `es256` picks the algorithm: ES256 signatures come out of OpenSSL as DER and are converted to
// the raw 64-byte pair JOSE wants (the same der_to_raw WebPush.cpp uses); RS256 signatures are
// already raw and are used as they are.
bool sign_jwt(void* pkey, bool es256, const std::string& header_json, const std::string& claims_json,
              std::string& out_jwt, std::string& err);

// Read a whole file, or return false with `err` set. Used for the .p8 and the service account.
bool read_text_file(const std::string& path, std::string& out, std::string& err);

// True when SNORCA_DEBUG_ROUTES=1. The gate's mock APNs and mock FCM are only reachable when it
// is: a shipped hub must not be talkable into posting a device's notifications somewhere else.
bool debug_routes_on();

} // namespace detail
} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
