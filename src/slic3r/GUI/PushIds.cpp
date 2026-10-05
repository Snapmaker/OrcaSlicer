// Opaque push identifiers (see PushIds.hpp). wx-free, like the rest of the hub's push code.
#include "PushIds.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cctype>
#include <mutex>
#include <string>

namespace Slic3r {
namespace GUI {
namespace PushIds {
// Named rather than anonymous: the unity build merges this file with its neighbours, and WebPush.cpp
// and AppPush.cpp have file-local helpers of their own (b64url among them).
namespace pushids_impl {

static const char* const HEX    = "0123456789abcdef";
static const char* const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static std::string to_b64url(const unsigned char* data, size_t len)
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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// HMAC-SHA256(key, label || 0x00 || fields joined by 0x00). The label keeps the thread id and the
// collapse id of one printer unrelated to each other; the separators keep ("ab","c") and ("a","bc")
// apart. Returns false (and nothing in `out`) for an invalid key.
static bool mac(const std::string& key_hex, const std::string& label, const std::string& a, const std::string& b,
                bool with_b, unsigned char out[32])
{
    if (!valid_key_hex(key_hex)) return false;
    unsigned char key[32];
    for (size_t i = 0; i < 32; ++i)
        key[i] = (unsigned char) (hex_value(key_hex[2 * i]) * 16 + hex_value(key_hex[2 * i + 1]));
    std::string msg = label;
    msg.push_back('\0');
    msg += a;
    if (with_b) {
        msg.push_back('\0');
        msg += b;
    }
    unsigned int len = 0;
    const unsigned char* r = HMAC(EVP_sha256(), key, (int) sizeof(key), (const unsigned char*) msg.data(), msg.size(), out, &len);
    return r != nullptr && len == 32;
}

static std::mutex  g_mutex;
static std::string g_key; // "" until set_key() or the first key() call

} // namespace pushids_impl

std::string new_key_hex()
{
    unsigned char b[32];
    if (RAND_bytes(b, sizeof(b)) != 1) return std::string();
    std::string s;
    s.reserve(64);
    for (unsigned char c : b) {
        s += pushids_impl::HEX[c >> 4];
        s += pushids_impl::HEX[c & 15];
    }
    return s;
}

bool valid_key_hex(const std::string& key_hex)
{
    if (key_hex.size() != 64) return false;
    for (char c : key_hex)
        if (pushids_impl::hex_value(c) < 0) return false;
    return true;
}

std::string thread_id(const std::string& key_hex, const std::string& printer_id)
{
    if (printer_id.empty()) return std::string();
    unsigned char d[32];
    if (!pushids_impl::mac(key_hex, "edgeslicer-push-thread-v1", printer_id, std::string(), false, d)) return std::string();
    return "t" + pushids_impl::to_b64url(d, 12);
}

std::string collapse_id(const std::string& key_hex, const std::string& printer_id, const std::string& kind)
{
    unsigned char d[32];
    if (!pushids_impl::mac(key_hex, "edgeslicer-push-collapse-v1", printer_id, kind, true, d) &&
        !pushids_impl::mac(key(), "edgeslicer-push-collapse-v1", printer_id, kind, true, d))
        return pushids_impl::to_b64url((const unsigned char*) "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 18);
    return pushids_impl::to_b64url(d, 18);
}

void set_key(const std::string& key_hex)
{
    if (!valid_key_hex(key_hex)) return;
    std::lock_guard<std::mutex> lock(pushids_impl::g_mutex);
    pushids_impl::g_key = key_hex;
}

std::string key()
{
    std::lock_guard<std::mutex> lock(pushids_impl::g_mutex);
    // Never "" and never a fixed value: a hub that pushes before AppPush::start() handed over the
    // saved key still derives opaque ids, only with a key that does not outlive this process.
    if (pushids_impl::g_key.empty()) pushids_impl::g_key = new_key_hex();
    return pushids_impl::g_key;
}

} // namespace PushIds
} // namespace GUI
} // namespace Slic3r
