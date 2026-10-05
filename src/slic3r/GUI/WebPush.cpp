// Web Push (see WebPush.hpp). Hub process only; wx-free, like the rest of the hub server.
//
// Everything here is RFC, not vendor: RFC 8030 (the POST and its headers), RFC 8188 (the
// aes128gcm content coding), RFC 8291 (how the content-encryption key is derived from the
// subscription's keys) and RFC 8292 (the VAPID Authorization header). No account anywhere - the
// push service is whichever one the phone's own browser picked, and it can read nothing but the
// envelope.
//
// The crypto is OpenSSL 1.1.1, which the GUI library already links; no new dependency. Every
// expand step of the RFC 8291 key schedule produces at most 32 bytes, i.e. exactly one HMAC
// block, so the whole HKDF is `HMAC(prk, info || 0x01)` truncated - there is no need for the
// HKDF API and none is used.
#include "WebPush.hpp"

#include "PushIds.hpp"
#include "RemoteEvents.hpp"

#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace WebPush {

using json = nlohmann::json;
using Bytes = std::vector<unsigned char>;

static const long        TTL_SECONDS   = 86400;  // a day: a "finished" that arrives tomorrow is noise
static const long long   JWT_LIFETIME  = 12 * 3600;   // RFC 8292 caps it at 24 h; Apple wants it reused
static const long long   JWT_REFRESH   = 3600;        // ... and asks that it not be minted more than hourly
static const size_t      MAX_PLAINTEXT = 3000;   // RFC 8291 §4 allows 3993; a notification is ~200
static const size_t      RECORD_SIZE   = 4096;
static const int         MAX_TRIES     = 3;      // one send plus two retries, like the relay senders
static const size_t      MAX_SUBS      = 20;     // a phone per person, not a phone per page load
static const long        T_CONNECT     = 5;
static const long        T_MAX         = 15;

struct Sub
{
    std::string id, endpoint, p256dh, auth, ua;
    long long   added { 0 };
    // status
    long long   last_sent { 0 };
    int         last_status { 0 };
    int         failures { 0 };
    std::string last_error;
};

static std::mutex        g_mutex;
static std::vector<Sub>  g_subs;
static std::string       g_vapid_private, g_vapid_public; // base64url; the private half is a credential
// The VAPID "sub" claim: the push services want a real contact and Apple answers 403 BadJwtToken to a
// placeholder domain (an .invalid mailto did exactly that on 2026-09-04); the project page is accepted.
static const char* const DEFAULT_SUBJECT = "https://github.com/aceRage/EdgeSlicer";
static const char* const OLD_SUBJECT_REPO = "https://github.com/aceRage/Snapmaker-Ultra"; // the repo before the 2026-09-05 rename (redirects, but say the new name)
static std::string       g_subject { DEFAULT_SUBJECT };
static std::string       g_min_severity { "info" };
// Which event kinds the phones want, as an allow-list; empty is every kind. One list for the
// whole channel rather than one per subscription: the phones are the same person's, and a
// per-device filter is a setting nobody has asked for and a screen nobody wants to fill in.
static std::vector<std::string> g_kinds;
static bool              g_enabled { true };
static std::string       g_phone_link;   // the link a notification opens by default: remote, else LAN
static std::string       g_lan_link;     // the same page on the home network ("" when there is none)
static std::string       g_remote_link;  // the tailnet link ("" while remote access is off)
static std::atomic<bool> g_stopping { false }; // set by stop(), so a backoff does not hold up a hub quit
static std::atomic<bool> g_dirty { false }; // settings.json needs rewriting (a subscription was pruned)
static std::map<std::string, std::pair<std::string, long long>> g_jwt_cache; // audience -> {jwt, exp}

// ------------------------------------------------------------------ small helpers ----

static long long now_ms()
{
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static long long now_s() { return now_ms() / 1000; }

static std::string random_id()
{
    static const char* hx = "0123456789abcdef";
    unsigned char      b[8];
    if (RAND_bytes(b, sizeof(b)) != 1) return std::to_string(now_ms());
    std::string s;
    for (unsigned char c : b) { s += hx[c >> 4]; s += hx[c & 15]; }
    return s;
}

static std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// base64url, no padding - the only encoding the push RFCs use, in both directions.
static const char* const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string b64url(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned a = data[i];
        const unsigned b = i + 1 < len ? data[i + 1] : 0;
        const unsigned c = i + 2 < len ? data[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += B64URL[(v >> 18) & 63];
        out += B64URL[(v >> 12) & 63];
        if (i + 1 < len) out += B64URL[(v >> 6) & 63];
        if (i + 2 < len) out += B64URL[v & 63];
    }
    return out;
}

static std::string b64url(const Bytes& b) { return b64url(b.data(), b.size()); }
static std::string b64url(const std::string& s) { return b64url((const unsigned char*) s.data(), s.size()); }

// Accepts the padded and the standard alphabet too: a browser's PushSubscription is base64url
// without padding, but people paste all sorts of things into a settings file by hand.
static bool b64url_decode(const std::string& in, Bytes& out)
{
    out.clear();
    unsigned buf = 0;
    int      bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\r' || ch == '\n' || ch == ' ') continue;
        int v = -1;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-' || ch == '+') v = 62;
        else if (ch == '_' || ch == '/') v = 63;
        else return false;
        buf = (buf << 6) | (unsigned) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char) ((buf >> bits) & 0xff));
        }
    }
    return true;
}

static void append(Bytes& dst, const unsigned char* p, size_t n) { dst.insert(dst.end(), p, p + n); }
static void append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }
static void append(Bytes& dst, const char* s) { append(dst, (const unsigned char*) s, std::strlen(s)); }

static Bytes hmac_sha256(const Bytes& key, const Bytes& data)
{
    Bytes        out(32);
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.empty() ? (const void*) "" : (const void*) key.data(), (int) key.size(),
         data.empty() ? (const unsigned char*) "" : data.data(), data.size(), out.data(), &len);
    out.resize(len);
    return out;
}

// The one HKDF shape this file needs: extract with `salt`, then a single expand block.
static Bytes hkdf(const Bytes& salt, const Bytes& ikm, const Bytes& info, size_t length)
{
    const Bytes prk = hmac_sha256(salt, ikm);
    Bytes       msg = info;
    msg.push_back(0x01);
    Bytes okm = hmac_sha256(prk, msg);
    okm.resize(length);
    return okm;
}

// The hub only ever posts to https, or to 127.0.0.1 - which is the mock push service the gate
// runs. A real push endpoint is always https; a plain-http one would put the encrypted body and
// the VAPID token on the wire for anyone on the path.
static bool endpoint_allowed(const std::string& url, std::string& why)
{
    const std::string u = lower_ascii(url);
    if (u.compare(0, 8, "https://") == 0) return true;
    if (u.compare(0, 7, "http://") == 0) {
        const std::string rest = u.substr(7);
        if (rest.compare(0, 10, "127.0.0.1:") == 0 || rest.compare(0, 10, "127.0.0.1/") == 0 || rest == "127.0.0.1" ||
            rest.compare(0, 10, "localhost:") == 0 || rest.compare(0, 10, "localhost/") == 0 || rest == "localhost")
            return true;
        why = "a push endpoint must be https (only 127.0.0.1 may be plain http)";
        return false;
    }
    why = "a push endpoint must start with https://";
    return false;
}

// RFC 8292: `aud` is the endpoint's *origin*, scheme and host only. Sending the whole endpoint is
// the single commonest cause of a 403 from FCM.
static std::string origin_of(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    const size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

static std::string host_of(const std::string& url)
{
    const std::string o = origin_of(url);
    const size_t      scheme = o.find("://");
    return scheme == std::string::npos ? o : o.substr(scheme + 3);
}

// A subscription endpoint is long, opaque and unique to one phone. The hub page shows the push
// service it belongs to and just enough of the tail to tell two phones apart.
static std::string mask_endpoint(const std::string& url)
{
    const std::string h = host_of(url);
    const std::string tail = url.size() > 6 ? url.substr(url.size() - 6) : url;
    return h + "/\xE2\x80\xA6" + tail;
}

static std::string mask_key(const std::string& k)
{
    if (k.size() <= 4) return "****";
    return "****" + k.substr(k.size() - 4);
}

static int severity_rank(const std::string& s)
{
    if (s == "error") return 2;
    if (s == "warning") return 1;
    return 0;
}

static std::string ev_str(const json& e, const char* key, const std::string& fallback = "")
{
    if (e.is_object() && e.contains(key) && e[key].is_string()) return e[key].get<std::string>();
    return fallback;
}

// Anything a printer, a file name or a push service puts in front of us: no CR, no LF, ASCII only,
// bounded. Header values in particular - a printer name must never be able to start a header.
static std::string header_safe(const std::string& s, size_t limit = 200)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') { if (!out.empty() && out.back() != ' ') out += ' '; }
        else if (c < 0x20 || c >= 0x7f) out += '?';
        else out += (char) c;
        if (out.size() >= limit) break;
    }
    return trim(out);
}

// The endpoint is a bearer credential of sorts (anyone holding it can push to that phone), so it
// never appears in a log line or in an error the page will show.
static std::string scrub(std::string text, const Sub& s)
{
    for (const std::string& secret : { s.endpoint, s.p256dh, s.auth }) {
        if (secret.size() < 8) continue;
        for (size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + 3))
            text.replace(at, secret.size(), "***");
    }
    if (text.size() > 300) text.resize(300);
    return text;
}

// ------------------------------------------------------------------ EC helpers ----

// A P-256 key held the way the RFCs hold it: the private half is a 32-byte scalar, the public half
// the 65-byte uncompressed point. OpenSSL's EC_KEY is the shortest road between the two on 1.1.1.
struct EcKey
{
    EC_KEY* k { nullptr };
    ~EcKey() { if (k) EC_KEY_free(k); }
    EcKey() = default;
    EcKey(const EcKey&) = delete;
    EcKey& operator=(const EcKey&) = delete;
    explicit operator bool() const { return k != nullptr; }
};

static bool ec_generate(EcKey& out)
{
    out.k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!out.k) return false;
    return EC_KEY_generate_key(out.k) == 1;
}

static bool ec_from_private(const Bytes& scalar, EcKey& out)
{
    if (scalar.size() != 32) return false;
    out.k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!out.k) return false;
    BIGNUM* d = BN_bin2bn(scalar.data(), 32, nullptr);
    if (!d) return false;
    bool ok = EC_KEY_set_private_key(out.k, d) == 1;
    if (ok) {
        // The public half is not stored: derive it, because ECDH and the JWT both need it.
        const EC_GROUP* g   = EC_KEY_get0_group(out.k);
        EC_POINT*       pub = EC_POINT_new(g);
        BN_CTX*         ctx = BN_CTX_new();
        ok = pub && ctx && EC_POINT_mul(g, pub, d, nullptr, nullptr, ctx) == 1 && EC_KEY_set_public_key(out.k, pub) == 1;
        if (pub) EC_POINT_free(pub);
        if (ctx) BN_CTX_free(ctx);
    }
    BN_free(d);
    return ok;
}

static bool ec_from_public(const Bytes& point, EcKey& out)
{
    if (point.size() != 65 || point[0] != 0x04) return false;
    out.k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!out.k) return false;
    const EC_GROUP* g = EC_KEY_get0_group(out.k);
    EC_POINT*       p = EC_POINT_new(g);
    BN_CTX*         ctx = BN_CTX_new();
    bool ok = p && ctx && EC_POINT_oct2point(g, p, point.data(), point.size(), ctx) == 1 && EC_KEY_set_public_key(out.k, p) == 1;
    if (p) EC_POINT_free(p);
    if (ctx) BN_CTX_free(ctx);
    return ok;
}

static bool ec_public_bytes(const EcKey& key, Bytes& out)
{
    const EC_GROUP* g = EC_KEY_get0_group(key.k);
    const EC_POINT* p = EC_KEY_get0_public_key(key.k);
    if (!g || !p) return false;
    BN_CTX* ctx = BN_CTX_new();
    out.assign(65, 0);
    const size_t n = EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, out.data(), out.size(), ctx);
    if (ctx) BN_CTX_free(ctx);
    return n == 65;
}

static bool ec_private_bytes(const EcKey& key, Bytes& out)
{
    const BIGNUM* d = EC_KEY_get0_private_key(key.k);
    if (!d) return false;
    out.assign(32, 0);
    return BN_bn2binpad(d, out.data(), 32) == 32;
}

// The raw ECDH x-coordinate, with no KDF applied - which is exactly what RFC 8291 calls ecdh_secret.
static bool ecdh(const EcKey& ours, const EcKey& theirs, Bytes& out)
{
    out.assign(32, 0);
    const int n = ECDH_compute_key(out.data(), out.size(), EC_KEY_get0_public_key(theirs.k), ours.k, nullptr);
    return n == 32;
}

// ------------------------------------------------------------- RFC 8291 encryption ----

bool encrypt(const std::string& p256dh_b64u,
             const std::string& auth_b64u,
             const std::string& plaintext,
             const std::string& fixed_salt_b64u,
             const std::string& fixed_ephemeral_priv_b64u,
             std::string&       out_body,
             std::string&       out_error)
{
    out_body.clear();
    out_error.clear();
    Bytes ua_public, auth_secret;
    if (!b64url_decode(p256dh_b64u, ua_public) || ua_public.size() != 65 || ua_public[0] != 0x04) {
        out_error = "the subscription's p256dh key is not a 65-byte uncompressed P-256 point";
        return false;
    }
    if (!b64url_decode(auth_b64u, auth_secret) || auth_secret.size() != 16) {
        out_error = "the subscription's auth secret is not 16 bytes";
        return false;
    }
    if (plaintext.size() > MAX_PLAINTEXT) { out_error = "the notification payload is too long"; return false; }

    EcKey peer;
    if (!ec_from_public(ua_public, peer)) { out_error = "the subscription's p256dh key is not on the P-256 curve"; return false; }

    // A fresh ephemeral key pair per message - the whole point of RFC 8291 is that the same
    // plaintext to the same phone twice never produces the same bytes.
    EcKey ours;
    if (!fixed_ephemeral_priv_b64u.empty()) {
        Bytes d;
        if (!b64url_decode(fixed_ephemeral_priv_b64u, d) || !ec_from_private(d, ours)) { out_error = "bad ephemeral private key"; return false; }
    } else if (!ec_generate(ours)) {
        out_error = "could not generate an ephemeral key pair";
        return false;
    }
    Bytes as_public;
    if (!ec_public_bytes(ours, as_public)) { out_error = "could not export the ephemeral public key"; return false; }

    Bytes salt;
    if (!fixed_salt_b64u.empty()) {
        if (!b64url_decode(fixed_salt_b64u, salt) || salt.size() != 16) { out_error = "the salt must be 16 bytes"; return false; }
    } else {
        salt.assign(16, 0);
        if (RAND_bytes(salt.data(), 16) != 1) { out_error = "no randomness available"; return false; }
    }

    Bytes shared;
    if (!ecdh(ours, peer, shared)) { out_error = "the ECDH exchange failed"; return false; }

    // RFC 8291 §3.4, verbatim:
    //   PRK_key = HMAC(auth_secret, ecdh_secret)
    //   IKM     = HKDF-Expand(PRK_key, "WebPush: info" || 0x00 || ua_public || as_public, 32)
    //   PRK     = HMAC(salt, IKM)
    //   CEK     = HKDF-Expand(PRK, "Content-Encoding: aes128gcm" || 0x00, 16)
    //   NONCE   = HKDF-Expand(PRK, "Content-Encoding: nonce" || 0x00, 12)
    Bytes key_info;
    append(key_info, "WebPush: info");
    key_info.push_back(0x00);
    append(key_info, ua_public);
    append(key_info, as_public);
    const Bytes ikm = hkdf(auth_secret, shared, key_info, 32);

    Bytes cek_info, nonce_info;
    append(cek_info, "Content-Encoding: aes128gcm");
    cek_info.push_back(0x00);
    append(nonce_info, "Content-Encoding: nonce");
    nonce_info.push_back(0x00);
    const Bytes cek   = hkdf(salt, ikm, cek_info, 16);
    const Bytes nonce = hkdf(salt, ikm, nonce_info, 12);

    // One record, so the delimiter is 0x02 ("this is the last one") rather than 0x01. No padding
    // beyond it: the payload is a notification, and hiding its length buys nothing here.
    Bytes record;
    append(record, (const unsigned char*) plaintext.data(), plaintext.size());
    record.push_back(0x02);

    Bytes            ciphertext(record.size());
    unsigned char    tag[16];
    EVP_CIPHER_CTX*  ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { out_error = "no cipher context"; return false; }
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1;
    // 12 bytes is OpenSSL's default GCM IV length, so there is deliberately no SET_IVLEN here.
    ok = ok && EVP_EncryptInit_ex(ctx, nullptr, nullptr, cek.data(), nonce.data()) == 1;
    int len = 0, total = 0;
    ok = ok && EVP_EncryptUpdate(ctx, ciphertext.data(), &len, record.data(), (int) record.size()) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, ciphertext.data() + total, &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { out_error = "AES-128-GCM failed"; return false; }
    ciphertext.resize((size_t) total);

    // RFC 8188 §2: salt(16) | record size(4, big-endian) | key id length(1) | key id | ciphertext.
    // For Web Push the key id is the sender's ephemeral public key, so its length is 65.
    Bytes body;
    append(body, salt);
    body.push_back((unsigned char) ((RECORD_SIZE >> 24) & 0xff));
    body.push_back((unsigned char) ((RECORD_SIZE >> 16) & 0xff));
    body.push_back((unsigned char) ((RECORD_SIZE >> 8) & 0xff));
    body.push_back((unsigned char) (RECORD_SIZE & 0xff));
    body.push_back((unsigned char) as_public.size());
    append(body, as_public);
    append(body, ciphertext);
    append(body, tag, sizeof(tag));
    out_body.assign((const char*) body.data(), body.size());
    return true;
}

// ------------------------------------------------------------------- RFC 8292 VAPID ----

// ES256 signatures come out of OpenSSL as DER; JOSE wants the raw pair of 32-byte integers.
static bool der_to_raw(const unsigned char* der, size_t len, Bytes& out)
{
    const unsigned char* p   = der;
    ECDSA_SIG*           sig = d2i_ECDSA_SIG(nullptr, &p, (long) len);
    if (!sig) return false;
    const BIGNUM *r = nullptr, *s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);
    out.assign(64, 0);
    const bool ok = BN_bn2binpad(r, out.data(), 32) == 32 && BN_bn2binpad(s, out.data() + 32, 32) == 32;
    ECDSA_SIG_free(sig);
    return ok;
}

// Signs with the hub's stored VAPID key. Caller holds no lock; the key is read under one.
static bool sign_es256(const std::string& message, Bytes& out, std::string& err)
{
    std::string priv_b64;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        priv_b64 = g_vapid_private;
    }
    Bytes d;
    if (priv_b64.empty() || !b64url_decode(priv_b64, d)) { err = "the hub has no VAPID key"; return false; }
    EcKey key;
    if (!ec_from_private(d, key)) { err = "the stored VAPID private key is not a P-256 scalar"; return false; }

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) { err = "out of memory"; return false; }
    if (EVP_PKEY_set1_EC_KEY(pkey, key.k) != 1) { EVP_PKEY_free(pkey); err = "could not wrap the VAPID key"; return false; }
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    size_t      len = 0;
    bool        ok = md && EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
              EVP_DigestSignUpdate(md, message.data(), message.size()) == 1 && EVP_DigestSignFinal(md, nullptr, &len) == 1;
    Bytes der(len);
    ok = ok && EVP_DigestSignFinal(md, der.data(), &len) == 1;
    if (md) EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!ok) { err = "signing the VAPID token failed"; return false; }
    der.resize(len);
    if (!der_to_raw(der.data(), der.size(), out)) { err = "could not convert the signature"; return false; }
    return true;
}

bool vapid_jwt(const std::string& audience,
               const std::string& subject,
               long long          expires_in_seconds,
               std::string&       out_jwt,
               std::string&       out_error)
{
    out_jwt.clear();
    out_error.clear();
    if (audience.empty()) { out_error = "no audience"; return false; }
    // A day is the RFC's ceiling; anything longer is refused by every push service.
    if (expires_in_seconds <= 0 || expires_in_seconds > 24 * 3600) expires_in_seconds = JWT_LIFETIME;
    const std::string header  = json({ { "typ", "JWT" }, { "alg", "ES256" } }).dump();
    json              payload = json::object();
    payload["aud"] = audience;
    payload["exp"] = now_s() + expires_in_seconds;
    payload["sub"] = subject.empty() ? std::string(DEFAULT_SUBJECT) : subject;
    const std::string signing_input = b64url(header) + "." + b64url(payload.dump());
    Bytes             sig;
    if (!sign_es256(signing_input, sig, out_error)) return false;
    out_jwt = signing_input + "." + b64url(sig);
    return true;
}

// One token per push origin, reused until it is within an hour of expiring: Apple asks that a
// VAPID token not be minted more often than that, and there is no reason to.
static bool jwt_for(const std::string& audience, std::string& out, std::string& err)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_jwt_cache.find(audience);
        if (it != g_jwt_cache.end() && it->second.second - now_s() > JWT_REFRESH) { out = it->second.first; return true; }
    }
    std::string subject;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        subject = g_subject;
    }
    if (!vapid_jwt(audience, subject, JWT_LIFETIME, out, err)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jwt_cache[audience] = { out, now_s() + JWT_LIFETIME };
    return true;
}

// ---------------------------------------------------------------------- the sender ----

struct SendResult
{
    bool        ok { false };
    int         status { 0 };
    std::string error;
};

// The Topic header coalesces: a second "paused" for the same printer replaces the first on the
// phone instead of stacking. The browser's push service reads it in the clear, so it is the same
// keyed HMAC the app push collapse id uses (PushIds.hpp) - never the printer id, and not an
// unkeyed hash of it either (a Bambu serial number could be confirmed against that). 24 base64url
// characters, inside the 32 RFC 8030 allows.
static std::string topic_for(const std::string& printer_id, const std::string& kind)
{
    return PushIds::collapse_id(PushIds::key(), printer_id, kind);
}

static SendResult push_once(const Sub& s, const std::string& payload, const std::string& severity,
                            const std::string& topic)
{
    SendResult res;
    std::string why;
    if (!endpoint_allowed(s.endpoint, why)) { res.error = why; return res; }
    std::string body, err;
    if (!encrypt(s.p256dh, s.auth, payload, "", "", body, err)) { res.error = err; return res; }
    std::string jwt;
    if (!jwt_for(origin_of(s.endpoint), jwt, err)) { res.error = err; return res; }
    std::string pub;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pub = g_vapid_public;
    }

    Http req = Http::post(s.endpoint);
    req.timeout_connect(T_CONNECT).timeout_max(T_MAX)
        .header("TTL", std::to_string(TTL_SECONDS))
        .header("Content-Encoding", "aes128gcm")
        .header("Content-Type", "application/octet-stream")
        // Anything the person needs to see now breaks through; a "started" can wait for the phone
        // to wake up on its own.
        .header("Urgency", severity_rank(severity) >= 1 ? "high" : "normal")
        .header("Authorization", "vapid t=" + jwt + ", k=" + pub);
    if (!topic.empty()) req.header("Topic", topic);
    // Http::post only hands libcurl the POST fields when a body is present; a bodyless POST makes
    // libcurl read stdin and the hub hangs. Every push carries an encrypted body, so this is safe.
    req.set_post_body(body)
        .on_complete([&](std::string, unsigned st) { res.ok = true; res.status = (int) st; })
        .on_error([&](std::string, std::string e, unsigned st) {
            res.status = (int) st;
            res.error  = e.empty() ? ("HTTP " + std::to_string(st)) : e;
        })
        .perform_sync();
    if (!res.ok && res.error.empty()) res.error = "the push service did not answer";
    return res;
}

// 404 and 410 are both the push service saying "this phone is gone". Nothing retries them and the
// subscription is dropped: a dead endpoint retried forever is how a hub ends up spending its life
// talking to a phone that was factory-reset a year ago.
static bool is_gone(const SendResult& r) { return r.status == 404 || r.status == 410; }

static bool worth_retrying(const SendResult& r)
{
    if (r.ok || is_gone(r)) return false;
    if (r.status == 0) return true;              // a transport error: DNS, TLS, no route
    return r.status == 429 || r.status >= 500;
}

static SendResult push_with_retries(const Sub& s, const std::string& payload, const std::string& severity,
                                    const std::string& topic)
{
    SendResult r;
    for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
        r = push_once(s, payload, severity, topic);
        if (r.ok || !worth_retrying(r)) break;
        if (attempt == MAX_TRIES) break;
        // Short enough that a "finished" is still news, long enough to ride out a push service
        // hiccup or a laptop's Wi-Fi coming back - and slept in slices, so quitting the hub does
        // not have to wait out a backoff.
        for (int slept = 0; slept < (attempt == 1 ? 1000 : 3000); slept += 100) {
            if (g_stopping) return r;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return r;
}

// ------------------------------------------------------------------ the payload ----

// What the service worker gets. Everything it needs to render the notification without a single
// request back to this PC - the phone may be on mobile data, miles from the LAN, and a push that
// shows nothing costs the permission on iOS.
static std::string payload_for(const json& e, const std::string& link, const std::string& lan, const std::string& remote)
{
    json p;
    std::string title = ev_str(e, "title", "EdgeSlicer");
    std::string body  = ev_str(e, "text");
    std::string who;
    if (e.is_object() && e.contains("printer") && e["printer"].is_object()) who = ev_str(e["printer"], "name");
    if (body.empty()) body = title;
    if (!who.empty() && body.find(who) == std::string::npos) body = who + ": " + body;
    body = RemoteEvents::notification_body(body, ev_str(e, "code"));
    p["title"]    = title;
    p["body"]     = body;
    p["kind"]     = ev_str(e, "kind");
    p["severity"] = ev_str(e, "severity", "info");
    if (!who.empty()) p["printer"] = who;
    // One notification per printer and kind: a second "paused" replaces the first rather than
    // stacking up five identical lines on the lock screen.
    p["tag"] = (e.is_object() && e.contains("printer") && e["printer"].is_object() ? ev_str(e["printer"], "id") : std::string()) +
               ":" + ev_str(e, "kind");
    if (!link.empty()) p["url"] = link;
    // The error code, when there is one. Small, and it is the key the app needs: a notification
    // that says "the toolhead camera is not working" is a sentence, and the buttons that go with
    // it are fetched from /api/printers or /summary by this code. Carried here rather than the
    // whole actions array, which would not survive the plaintext cap.
    {
        const std::string code = ev_str(e, "code");
        if (!code.empty()) p["code"] = code;
    }
    // Both origins, so the service worker can open the one this phone prefers without a request
    // back to a PC that may be asleep or on a network the phone is not on.
    if (!lan.empty()) p["lan_url"] = lan;
    if (!remote.empty()) p["remote_url"] = remote;
    if (e.is_object() && e.contains("id") && e["id"].is_number_integer()) p["id"] = e["id"];
    if (e.is_object() && e.contains("time") && e["time"].is_number_integer()) p["time"] = e["time"];
    // The stable id (<hub instance>-<id>): the app keys its history on it, so the pushed copy and
    // the pulled copy of one event are one row even across a data-dir reset or a second hub.
    if (e.is_object() && e.contains("uid") && e["uid"].is_string()) p["uid"] = e["uid"];
    std::string out = p.dump();
    if (out.size() > MAX_PLAINTEXT) {
        // Trim the sentence rather than dropping the notification: a title alone still tells the
        // person to go and look.
        p["body"] = body.substr(0, 400);
        out       = p.dump();
        if (out.size() > MAX_PLAINTEXT) out = json({ { "title", title }, { "body", "" } }).dump();
    }
    return out;
}

// ------------------------------------------------------------------- persistence ----

static json sub_json(const Sub& s, bool masked)
{
    json j;
    j["id"]    = s.id;
    j["added"] = s.added;
    if (masked) {
        j["endpoint"] = mask_endpoint(s.endpoint);
        j["host"]     = host_of(s.endpoint);
        j["p256dh"]   = mask_key(s.p256dh);
        j["auth"]     = mask_key(s.auth);
    } else {
        j["endpoint"] = s.endpoint;
        j["p256dh"]   = s.p256dh;
        j["auth"]     = s.auth;
    }
    j["ua"]          = s.ua;
    j["last_sent"]   = s.last_sent;
    j["last_status"] = s.last_status;
    j["last_error"]  = s.last_error;
    j["failures"]    = s.failures;
    if (masked) j["status"] = s.failures >= 3 ? "failing" : (s.last_sent == 0 ? "new" : (s.failures ? "retrying" : "ok"));
    return j;
}

json settings_json()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    j["enabled"]      = g_enabled;
    j["min_severity"] = g_min_severity;
    j["kinds"]        = g_kinds;
    j["subject"]      = g_subject;
    // The private half never leaves this file. It is the hub's identity to the push services and
    // nothing more - it decrypts nothing - but anyone holding it could push to this hub's phones.
    j["vapid"]        = json{ { "private", g_vapid_private }, { "public", g_vapid_public } };
    j["subscriptions"] = json::array();
    for (const Sub& s : g_subs) j["subscriptions"].push_back(sub_json(s, false));
    return j;
}

json masked_json()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    j["enabled"]       = g_enabled;
    j["min_severity"]  = g_min_severity;
    j["kinds"]         = g_kinds;
    j["events"]        = RemoteEvents::events_map(g_kinds); // the same filter as checkboxes
    j["all_kinds"]     = RemoteEvents::all_kinds();
    j["subject"]       = g_subject;
    j["public_key"]    = g_vapid_public;    // public by design: the page passes it as applicationServerKey
    j["has_key"]       = !g_vapid_private.empty();
    j["count"]         = (int) g_subs.size();
    j["phone_link"]    = !g_phone_link.empty();
    j["severities"]    = json::array({ "info", "warning", "error" });
    j["subscriptions"] = json::array();
    for (const Sub& s : g_subs) j["subscriptions"].push_back(sub_json(s, true));
    return j;
}

std::string public_key_b64u()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_vapid_public;
}

bool has_subscriptions()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled && !g_subs.empty();
}

bool consume_dirty() { return g_dirty.exchange(false); }

// ------------------------------------------------------------------- public API ----

void start(const json& saved)
{
    g_stopping = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_subs.clear();
    g_jwt_cache.clear();
    try {
        if (saved.is_object()) {
            g_enabled      = saved.value("enabled", true);
            g_min_severity = saved.value("min_severity", std::string("info"));
            // A kind nobody emits is dropped rather than refused - settings.json is a file a
            // person may have hand-edited, and one stale name must not stop the hub starting.
            g_kinds.clear();
            if (saved.contains("kinds") && saved["kinds"].is_array())
                for (const auto& k : saved["kinds"])
                    if (k.is_string() && RemoteEvents::is_kind(k.get<std::string>())) g_kinds.push_back(k.get<std::string>());
            g_subject      = saved.value("subject", std::string(DEFAULT_SUBJECT));
            if (g_subject == "mailto:hub@snapmaker-orca.invalid" || g_subject == OLD_SUBJECT_REPO)
                g_subject = DEFAULT_SUBJECT; // the first build's placeholder (which Apple refuses), or the pre-rename repo URL
            if (saved.contains("vapid") && saved["vapid"].is_object()) {
                g_vapid_private = saved["vapid"].value("private", "");
                g_vapid_public  = saved["vapid"].value("public", "");
            }
            if (saved.contains("subscriptions") && saved["subscriptions"].is_array())
                for (const auto& e : saved["subscriptions"]) {
                    Sub s;
                    s.id       = e.value("id", "");
                    s.endpoint = e.value("endpoint", "");
                    s.p256dh   = e.value("p256dh", "");
                    s.auth     = e.value("auth", "");
                    s.ua       = e.value("ua", "");
                    s.added    = e.value("added", 0LL);
                    s.last_sent   = e.value("last_sent", 0LL);
                    s.last_status = e.value("last_status", 0);
                    s.last_error  = e.value("last_error", "");
                    if (s.id.empty()) s.id = random_id();
                    if (!s.endpoint.empty() && !s.p256dh.empty() && !s.auth.empty()) g_subs.push_back(s);
                }
        }
    } catch (...) {} // a settings.json somebody hand-edited must not stop the hub starting

    // Generated once, then kept for the life of the data dir: every phone that ever subscribed did
    // so against this public key, and a new pair would silently invalidate all of them.
    Bytes check;
    if (g_vapid_private.empty() || g_vapid_public.empty() || !b64url_decode(g_vapid_private, check) || check.size() != 32) {
        EcKey key;
        Bytes priv, pub;
        if (ec_generate(key) && ec_private_bytes(key, priv) && ec_public_bytes(key, pub)) {
            g_vapid_private = b64url(priv);
            g_vapid_public  = b64url(pub);
            g_dirty         = true;
            BOOST_LOG_TRIVIAL(info) << "WebPush: generated this hub's VAPID key pair";
        } else {
            BOOST_LOG_TRIVIAL(error) << "WebPush: could not generate a VAPID key pair; Web Push is off";
        }
    }
    BOOST_LOG_TRIVIAL(info) << "WebPush: " << g_subs.size() << " phone subscription(s)";
}

void stop()
{
    g_stopping = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jwt_cache.clear();
}

void set_phone_links(const std::string& remote, const std::string& lan)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phone_link  = remote.empty() ? lan : remote;
    g_lan_link    = lan;
    g_remote_link = remote;
}

void deliver(const json& event)
{
    std::vector<Sub>         targets;
    std::vector<std::string> kinds;
    std::string              link, lan, remote, min_sev;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_enabled || g_subs.empty() || g_vapid_private.empty()) return;
        targets = g_subs;
        link    = g_phone_link;
        lan     = g_lan_link;
        remote  = g_remote_link;
        min_sev = g_min_severity;
        kinds   = g_kinds;
    }
    const std::string severity = ev_str(event, "severity", "info");
    // Both filters have to allow it: the minimum severity is the coarse dial that was here
    // first and still means what it meant, the kind list is the fine one on top of it.
    if (severity_rank(severity) < severity_rank(min_sev)) return;
    if (!RemoteEvents::kind_allowed(kinds, ev_str(event, "kind"))) return;
    const std::string payload = payload_for(event, link, lan, remote);
    const std::string printer = event.is_object() && event.contains("printer") && event["printer"].is_object()
                                    ? ev_str(event["printer"], "id") : std::string();
    const std::string topic   = topic_for(printer, ev_str(event, "kind"));

    for (const Sub& s : targets) {
        const SendResult r = push_with_retries(s, payload, severity, topic);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_subs.size(); ++i) {
            if (g_subs[i].id != s.id) continue;
            if (is_gone(r)) {
                BOOST_LOG_TRIVIAL(info) << "WebPush: subscription " << s.id << " on " << host_of(s.endpoint)
                                        << " is gone (HTTP " << r.status << "); forgetting it";
                g_subs.erase(g_subs.begin() + i);
                g_dirty = true;
                break;
            }
            g_subs[i].last_sent   = now_ms();
            g_subs[i].last_status = r.status;
            if (r.ok) {
                g_subs[i].failures = 0;
                g_subs[i].last_error.clear();
            } else {
                ++g_subs[i].failures;
                g_subs[i].last_error = scrub(r.error, s);
                BOOST_LOG_TRIVIAL(warning) << "WebPush: push to " << host_of(s.endpoint) << " failed ("
                                           << g_subs[i].failures << " in a row): " << g_subs[i].last_error;
            }
            g_dirty = true;
            break;
        }
    }
}

std::pair<int, std::string> add_subscription(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    const std::string endpoint = trim(in.value("endpoint", ""));
    std::string       p256dh, auth;
    if (in.contains("keys") && in["keys"].is_object()) {
        p256dh = trim(in["keys"].value("p256dh", ""));
        auth   = trim(in["keys"].value("auth", ""));
    }
    if (p256dh.empty()) p256dh = trim(in.value("p256dh", ""));
    if (auth.empty()) auth = trim(in.value("auth", ""));

    std::string why;
    if (endpoint.empty() || endpoint.size() > 2000) return { 400, json({ { "error", "the subscription has no usable endpoint" } }).dump() };
    if (!endpoint_allowed(endpoint, why)) return { 400, json({ { "error", why } }).dump() };
    Bytes k, a;
    if (!b64url_decode(p256dh, k) || k.size() != 65 || k[0] != 0x04)
        return { 400, json({ { "error", "keys.p256dh must be a 65-byte uncompressed P-256 point" } }).dump() };
    if (!b64url_decode(auth, a) || a.size() != 16)
        return { 400, json({ { "error", "keys.auth must be 16 bytes" } }).dump() };

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Re-subscribing the same phone updates the row it already has: the page re-POSTs its
        // subscription on every launch (the only mechanism that works on every platform), and
        // twenty duplicates of one phone would mean twenty pushes for one event.
        for (Sub& s : g_subs)
            if (s.endpoint == endpoint) {
                s.p256dh = p256dh;
                s.auth   = auth;
                s.ua     = header_safe(in.value("ua", ""), 120);
                s.failures = 0;
                s.last_error.clear();
                g_dirty  = true;
                json out;
                out["ok"] = true;
                out["id"] = s.id;
                out["count"] = (int) g_subs.size();
                return { 200, out.dump() };
            }
        if (g_subs.size() >= MAX_SUBS) return { 429, json({ { "error", "this hub is already pushing to as many phones as it will" } }).dump() };
        Sub s;
        s.id       = random_id();
        s.endpoint = endpoint;
        s.p256dh   = p256dh;
        s.auth     = auth;
        s.ua       = header_safe(in.value("ua", ""), 120);
        s.added    = now_ms();
        g_subs.push_back(s);
        g_dirty = true;
        BOOST_LOG_TRIVIAL(info) << "WebPush: a phone subscribed through " << host_of(endpoint);
        json out;
        out["ok"]    = true;
        out["id"]    = s.id;
        out["count"] = (int) g_subs.size();
        return { 200, out.dump() };
    }
}

std::pair<int, std::string> remove_subscription(const std::string& endpoint)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_subs.size(); ++i)
            if (g_subs[i].endpoint == endpoint) { g_subs.erase(g_subs.begin() + i); found = true; g_dirty = true; break; }
    }
    // Whether it was there or not is not the caller's business - answering "no such subscription"
    // to an unauthenticated caller would turn this into an oracle for guessing endpoints.
    (void) found;
    return { 200, std::string("{\"ok\":true}") };
}

std::pair<int, std::string> remove(const std::string& id)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_subs.size(); ++i)
            if (g_subs[i].id == id) { g_subs.erase(g_subs.begin() + i); found = true; g_dirty = true; break; }
    }
    if (!found) return { 404, json({ { "error", "no such subscription" } }).dump() };
    BOOST_LOG_TRIVIAL(info) << "WebPush: subscription removed from the hub page";
    return { 200, masked_json().dump() };
}

int forget_all_subscriptions()
{
    size_t n;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n = g_subs.size();
        g_subs.clear();
        if (n) g_dirty = true;
    }
    if (n) BOOST_LOG_TRIVIAL(info) << "WebPush: " << n << " subscription(s) forgotten with the old phone link";
    return (int) n;
}

std::pair<int, std::string> set_options(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (in.contains("enabled") && in["enabled"].is_boolean()) g_enabled = in["enabled"].get<bool>();
        if (in.contains("min_severity") && in["min_severity"].is_string()) {
            const std::string s = in["min_severity"].get<std::string>();
            if (s != "info" && s != "warning" && s != "error")
                return { 400, json({ { "error", "min_severity must be info, warning or error" } }).dump() };
            g_min_severity = s;
        }
        if (RemoteEvents::has_kind_filter(in)) {
            std::string why;
            if (!RemoteEvents::read_kinds(in, g_kinds, why)) return { 400, json({ { "error", why } }).dump() };
        }
        if (in.contains("subject") && in["subject"].is_string()) {
            const std::string s = trim(in["subject"].get<std::string>());
            // RFC 8292 says the sub claim is a mailto: or https: URI, and the push services enforce it.
            if (!s.empty() && s.compare(0, 7, "mailto:") != 0 && s.compare(0, 8, "https://") != 0)
                return { 400, json({ { "error", "the contact must be a mailto: address or an https:// URL" } }).dump() };
            if (!s.empty() && s != g_subject) {
                g_subject = header_safe(s, 200);
                g_jwt_cache.clear(); // the cached tokens carry the old sub
            }
        }
        g_dirty = true;
    }
    return { 200, masked_json().dump() };
}

std::pair<int, std::string> test(const std::string& phone_link)
{
    std::vector<Sub>         targets;
    std::vector<std::string> kinds;
    std::string              link = phone_link, lan, remote;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        targets = g_subs;
        kinds   = g_kinds;
        if (link.empty()) link = g_phone_link;
        lan    = g_lan_link;
        remote = g_remote_link;
    }
    if (targets.empty()) return { 200, json({ { "ok", false }, { "error", "no phone has subscribed yet" }, { "results", json::array() } }).dump() };

    json e;
    e["id"]       = 0;
    e["time"]     = now_ms();
    e["printer"]  = json{ { "id", "test" }, { "name", "Test" }, { "kind", "printhost" } };
    // Dressed as a kind the filter allows, so the notification that arrives is shaped like the
    // real one it stands in for. Sent whatever the filters say - the button proves the path.
    e["kind"]     = RemoteEvents::test_kind(kinds);
    e["severity"] = "info";
    e["title"]    = "EdgeSlicer test";
    e["text"]     = "This is a test push from the hub on your PC. If you can read it, Web Push works.";
    const std::string payload = payload_for(e, link, lan, remote);

    // Sent on this thread, once, with no retries: somebody is watching the page for the answer.
    json results = json::array();
    bool any     = false;
    for (const Sub& s : targets) {
        const SendResult r = push_once(s, payload, "info", topic_for("test", e["kind"].get<std::string>()));
        any = any || r.ok;
        json one;
        one["id"]       = s.id;
        one["endpoint"] = mask_endpoint(s.endpoint);
        one["ok"]       = r.ok;
        one["status"]   = r.status;
        one["error"]    = scrub(r.error, s);
        results.push_back(one);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_subs.size(); ++i) {
            if (g_subs[i].id != s.id) continue;
            if (is_gone(r)) { g_subs.erase(g_subs.begin() + i); g_dirty = true; break; }
            g_subs[i].last_sent   = now_ms();
            g_subs[i].last_status = r.status;
            if (r.ok) { g_subs[i].failures = 0; g_subs[i].last_error.clear(); }
            else { ++g_subs[i].failures; g_subs[i].last_error = scrub(r.error, s); }
            g_dirty = true;
            break;
        }
    }
    json out;
    out["ok"]      = any;
    out["results"] = results;
    // What the phones would really be sent, next to the kind this test wore, so the page can
    // say "sent as finished; started is off" rather than leaving somebody guessing.
    out["kind"]    = e["kind"];
    out["kinds"]   = RemoteEvents::enabled_kinds(kinds);
    out["events"]  = RemoteEvents::events_map(kinds);
    return { 200, out.dump() };
}

// ------------------------------------------------------------------ the debug route ----

// Only reachable with SNORCA_DEBUG_ROUTES=1, and only on the loopback admin listener behind the
// hub secret. It exists so the gate can check this file's crypto against the RFC 8291 test vectors
// and against a real Python implementation, instead of against itself.
std::pair<int, std::string> debug_op(const std::string& body)
{
    const char* on = std::getenv("SNORCA_DEBUG_ROUTES");
    if (!on || std::string(on) != "1") return { 404, json({ { "error", "not found" } }).dump() };
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    const std::string op = in.value("op", "");
    json              out;
    if (op == "encrypt") {
        std::string enc, err;
        if (!encrypt(in.value("p256dh", ""), in.value("auth", ""), in.value("plaintext", ""),
                     in.value("salt", ""), in.value("private", ""), enc, err))
            return { 400, json({ { "error", err } }).dump() };
        out["body"] = b64url((const unsigned char*) enc.data(), enc.size());
        return { 200, out.dump() };
    }
    if (op == "jwt") {
        std::string jwt, err;
        if (!vapid_jwt(in.value("audience", ""), in.value("subject", ""), in.value("expires", (long long) JWT_LIFETIME), jwt, err))
            return { 400, json({ { "error", err } }).dump() };
        out["jwt"]        = jwt;
        out["public_key"] = public_key_b64u();
        return { 200, out.dump() };
    }
    if (op == "topic") {
        out["topic"] = topic_for(in.value("printer", ""), in.value("kind", ""));
        return { 200, out.dump() };
    }
    return { 400, json({ { "error", "op must be encrypt, jwt or topic" } }).dump() };
}

} // namespace WebPush
} // namespace GUI
} // namespace Slic3r
