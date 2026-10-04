// APNs token authentication, the iOS half of the push plane (see AppPush.hpp).
//
// Three things about Apple's contract are easy to get wrong by copying the VAPID code next door,
// and all three are deliberate here:
//
//  1. **There is no `exp` claim.** RFC 8292 VAPID has one; Apple's provider token does not. The
//     lifetime is implied by `iat`, which "must be no more than one hour" old, and a token older
//     than that earns a 403 ExpiredProviderToken. The header is `{"alg":"ES256","kid":...}` and
//     the claims are `{"iss":<team id>,"iat":<now>}` - nothing else in either.
//  2. **The key is an unencrypted PKCS#8 PEM**, not the raw 32-byte scalar the VAPID key is
//     stored as, so it needs OpenSSL's PEM reader. Everything after that - the ES256 signing, the
//     DER-to-raw conversion, the base64url - is the same code path WebPush.cpp uses.
//  3. **The alert dictionary carries literal placeholder text.** Apple requires `mutable-content`
//     *and* an alert with title, subtitle or body information before it will run a Notification
//     Service Extension at all, and a `title-loc-key` naming a string that exists in no .strings
//     file is not title information - it fails silently. The placeholder is also exactly what the
//     person sees if the extension crashes or runs out of its 30 seconds, so it has to read as
//     intentional and leak nothing: "Printer update" / "Tap to open".
//
// And one about transport: APNs speaks HTTP/2 and nothing else. Offered both, it negotiates ALPN
// h2; offered only http/1.1 it negotiates nothing at all. A libcurl without nghttp2 cannot reach
// it, which is why available() checks and says so rather than failing every push with a transport
// error. See deps/NGHTTP2/NGHTTP2.cmake.
#include "AppPushProvider.hpp"

#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>

namespace Slic3r {
namespace GUI {
namespace AppPush {

using json = nlohmann::json;

static const char* const APNS_PRODUCTION = "https://api.push.apple.com";
static const char* const APNS_SANDBOX    = "https://api.sandbox.push.apple.com";
// Apple asks for no more than one new token per 20 minutes and no fewer than one per hour.
// Forty-five minutes is comfortably inside the hour and comfortably outside the twenty.
static const long long JWT_REFRESH_AFTER = 45 * 60;
static const long      T_CONNECT = 5;
static const long      T_MAX     = 20;

static long long apns_now_s()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

class ApnsProvider : public Provider
{
public:
    ~ApnsProvider() override { drop_key(); }

    const char* name() const override { return "apns"; }

    bool available(std::string& why) const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_enabled) { why = "APNs is switched off"; return false; }
        if (!Http::has_http2()) {
            // The honest message, and the reason the check exists: this is a property of the
            // build, not of the person's credentials, and no amount of re-entering a key fixes it.
            why = "APNs needs an HTTP/2-capable build of libcurl, and this one has none "
                  "(rebuild the dependencies to pick up nghttp2)";
            return false;
        }
        if (!m_key) { why = m_key_error.empty() ? "no APNs signing key is configured" : m_key_error; return false; }
        if (m_key_id.empty()) { why = "the APNs key id (10 characters) is not set"; return false; }
        if (m_team_id.empty()) { why = "the APNs team id (10 characters) is not set"; return false; }
        if (m_bundle.empty()) { why = "the app's bundle id is not set"; return false; }
        return true;
    }

    void configure(const std::string& config_json) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        drop_key();
        m_jwt.clear();
        m_jwt_minted = 0;
        m_jwt_kid.clear();
        m_enabled = false;
        m_key_id.clear();
        m_team_id.clear();
        m_bundle.clear();
        m_default_env = "production";
        m_host_override.clear();
        m_key_error.clear();

        json c;
        try { c = json::parse(config_json); } catch (...) { return; }
        if (!c.is_object()) return;
        m_enabled     = c.value("enabled", true);
        m_key_id      = c.value("key_id", "");
        m_team_id     = c.value("team_id", "");
        m_bundle      = c.value("bundle", "");
        m_default_env = c.value("env", std::string("production")) == "sandbox" ? "sandbox" : "production";
        if (detail::debug_routes_on()) {
            // The gate's mock APNs on loopback, and nothing else.
            m_host_override = c.value("host_override", "");
        }

        // Read once and hold the parsed key, so a push at 03:00 does not fail because the file
        // moved. The path remains the source of truth; this is a cache, refreshed on any change.
        std::string pem = c.value("key_pem", "");
        if (pem.empty()) {
            const std::string path = c.value("key_path", "");
            if (path.empty()) { m_key_error = "no APNs signing key is configured"; return; }
            std::string err;
            if (!detail::read_text_file(path, pem, err)) { m_key_error = err; return; }
        }
        std::string err;
        m_key = detail::load_pkcs8_pem(pem, err);
        if (!m_key) m_key_error = "the .p8 could not be read: " + err;
    }

    PushResult send(const PushRequest& req) override
    {
        PushResult res;
        std::string host, bundle;
        bool        loopback_mock = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            host          = host_for(req.env);
            loopback_mock = !m_host_override.empty();
            bundle        = req.bundle.empty() ? m_bundle : req.bundle;
        }
        res.host = host;
        if (bundle.empty()) { res.error = "no apns-topic: the app's bundle id is not set"; return res; }

        std::string jwt, err;
        if (!provider_token(host, jwt, err)) { res.error = err; return res; }

        // The literal placeholder is the whole of what a push vendor - or a person whose service
        // extension failed - can read. Everything real is in `e`, encrypted to the device.
        json payload;
        payload["aps"]["alert"]["title"]  = "Printer update";
        payload["aps"]["alert"]["body"]   = "Tap to open";
        payload["aps"]["mutable-content"] = 1;
        payload["aps"]["sound"]           = "default";
        if (!req.thread_id.empty()) payload["aps"]["thread-id"] = req.thread_id;
        // Only when the device asked for the level hint (see PushRequest::interruption_level).
        if (!req.interruption_level.empty()) payload["aps"]["interruption-level"] = req.interruption_level;
        payload["v"] = 1;
        payload["e"] = req.ciphertext_b64u;
        const std::string body = payload.dump();

        const std::string url = host + "/3/device/" + req.device_token;
        std::string       answer;
        Http request = Http::post(url);
        request.timeout_connect(T_CONNECT).timeout_max(T_MAX)
            // Not a preference: on a libcurl without nghttp2 this option is refused outright, and
            // failing here is far better than sending an HTTP/1.1 request Apple will not answer.
            // Against the gate's loopback mock it is cleartext h2, which needs prior knowledge
            // because there is no TLS and so no ALPN to negotiate with.
            .http_version(loopback_mock ? CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE : CURL_HTTP_VERSION_2TLS)
            .header("authorization", "bearer " + jwt)
            .header("apns-topic", bundle)
            // Our notifications are always user-visible alerts. Apple requires this on watchOS 6+
            // and asks that it "accurately reflect notification payload contents" everywhere.
            .header("apns-push-type", "alert")
            .header("apns-priority", req.priority >= 10 ? "10" : "5")
            // A nonzero expiration means "store and retry for this long"; a print alert that
            // arrives after the print is over is noise, so this is short by design.
            .header("apns-expiration", std::to_string(apns_now_s() + req.ttl_seconds))
            .header("apns-collapse-id", req.collapse_id)
            .header("Content-Type", "application/json")
            .set_post_body(body)
            .on_complete([&](std::string b, unsigned st) { res.ok = true; res.status = (int) st; answer = b; })
            .on_error([&](std::string b, std::string e, unsigned st) {
                res.status = (int) st;
                res.error  = e.empty() ? ("HTTP " + std::to_string(st)) : e;
                answer     = b;
            })
            .perform_sync();

        if (!res.ok) classify(res, answer);
        if (!res.ok && res.error.empty()) res.error = "APNs did not answer";
        return res;
    }

private:
    void drop_key()
    {
        if (m_key) { detail::free_pkey(m_key); m_key = nullptr; }
    }

    std::string host_for(const std::string& env) const
    {
        if (!m_host_override.empty()) return m_host_override;
        const std::string e = env.empty() ? m_default_env : env;
        // Per device, not per hub: a token minted by a development build is invalid on the
        // production host and the symptom - BadDeviceToken - looks exactly like a code bug.
        return e == "sandbox" ? APNS_SANDBOX : APNS_PRODUCTION;
    }

    // Apple's error body is {"reason":"Unregistered"}, with a `timestamp` on a 410.
    void classify(PushResult& res, const std::string& answer)
    {
        std::string reason;
        try {
            const json j = json::parse(answer);
            if (j.is_object()) reason = j.value("reason", "");
        } catch (...) {}
        if (!reason.empty()) res.error = reason;
        // The app was uninstalled, or the token belongs to another environment or another app.
        // None of these ever come right on a retry, and the row goes.
        if (res.status == 410 && (reason == "Unregistered" || reason.empty())) res.gone = true;
        if (res.status == 400 && (reason == "BadDeviceToken" || reason == "BadTopic" ||
                                  reason == "DeviceTokenNotForTopic"))
            res.gone = true;
        // The provider token aged past its hour mid-flight. This must never prune a live device:
        // AppPush mints a fresh one and tries exactly once more.
        if (res.status == 403 && (reason == "ExpiredProviderToken" || reason == "ExpiredToken"))
            res.credential_expired = true;
    }

    // One provider token, reused for up to 45 minutes. Keyed on (host, kid) rather than on an
    // origin: the same key signs for sandbox and production, and a key rotation must invalidate
    // the cache even when the host has not changed.
    bool provider_token(const std::string& host, std::string& out, std::string& err)
    {
        std::string kid, iss, key_error;
        void*       key = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_enabled) { err = "APNs is switched off"; return false; }
            const std::string cache_key = host + "|" + m_key_id;
            if (!m_jwt.empty() && m_jwt_kid == cache_key && apns_now_s() - m_jwt_minted < JWT_REFRESH_AFTER) {
                out = m_jwt;
                return true;
            }
            kid       = m_key_id;
            iss       = m_team_id;
            key       = m_key;
            key_error = m_key_error;
        }
        if (!key) { err = key_error.empty() ? "no APNs signing key is configured" : key_error; return false; }
        if (kid.empty() || iss.empty()) { err = "the APNs key id or team id is not set"; return false; }

        // Exactly these two header fields and exactly these two claims. No `typ`, and above all
        // no `exp`: Apple does not use one and adding it is the classic copy-from-VAPID mistake.
        const json  header = json({ { "alg", "ES256" }, { "kid", kid } });
        json        claims;
        claims["iss"] = iss;
        claims["iat"] = apns_now_s();
        std::string jwt;
        if (!detail::sign_jwt(key, /*es256=*/true, header.dump(), claims.dump(), jwt, err)) return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_jwt        = jwt;
        m_jwt_kid    = host + "|" + kid;
        m_jwt_minted = apns_now_s();
        out          = jwt;
        return true;
    }

    mutable std::mutex m_mutex;
    bool               m_enabled { false };
    void*              m_key { nullptr };
    std::string        m_key_error;
    std::string        m_key_id, m_team_id, m_bundle;
    std::string        m_default_env { "production" };
    std::string        m_host_override;
    std::string        m_jwt, m_jwt_kid;
    long long          m_jwt_minted { 0 };
};

std::unique_ptr<Provider> make_apns_provider() { return std::unique_ptr<Provider>(new ApnsProvider()); }

} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
