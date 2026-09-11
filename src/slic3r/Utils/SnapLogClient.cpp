#include "SnapLogClient.hpp"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <future>
#include <mutex>
#include <regex>
#include <thread>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#ifdef _WIN32
#include <share.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
namespace Slic3r { namespace SnapLog { inline namespace v1 {

constexpr int kShutdownRealtimeJoinMaxSec = 2;
constexpr int kShutdownBatchJoinMaxSec    = 3;

static bool is_valid_spool_path(const boost::filesystem::path& path)
{
    return !path.empty() && !path.native().empty();
}

static void log_detached_thread_failure(const char* operation) noexcept
{
    BOOST_LOG_TRIVIAL(warning) << "SnapLog " << operation << " thread failed";
}

static void fulfill_purge_completion(const std::shared_ptr<std::promise<bool>>& completion, bool succeeded) noexcept
{
    if (!completion)
        return;

    completion->set_value(succeeded);
}

class PurgeCompletionGuard
{
public:
    explicit PurgeCompletionGuard(std::shared_ptr<std::promise<bool>> completion) : m_completion(std::move(completion)) {}

    ~PurgeCompletionGuard()
    {
        fulfill_purge_completion(m_completion, false);
    }

    void fulfill(bool succeeded)
    {
        fulfill_purge_completion(m_completion, succeeded);
        m_completion.reset();
    }

private:
    std::shared_ptr<std::promise<bool>> m_completion;
};

// ---- SpoolLock: cross-platform exclusive file lock (multi-instance safety) ----
SnapLogClient::SpoolLock::~SpoolLock()
{
    if (fd >= 0) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        fd = -1;
    }
    acquired = false;
}

SnapLogClient::SpoolLock& SnapLogClient::SpoolLock::operator=(SnapLogClient::SpoolLock&& o) noexcept
{
    if (this != &o) {
        if (fd >= 0) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
        }
        fd         = o.fd;
        acquired   = o.acquired;
        o.fd       = -1;
        o.acquired = false;
    }
    return *this;
}

SnapLogClient::SpoolLock SnapLogClient::try_acquire_spool_lock(const boost::filesystem::path& dir)
{
    SpoolLock                 lk;
    if (!is_valid_spool_path(dir))
        return lk;

    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    auto lock_path = dir / ".lock";
#ifdef _WIN32
    int     fd  = -1;
    errno_t err = _wsopen_s(&fd, lock_path.native().c_str(), _O_RDWR | _O_CREAT, _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (err != 0 || fd < 0)
        return lk; // sharing violation = held by another process
#else
    int fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return lk;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return lk;
    }
#endif
    lk.fd       = fd;
    lk.acquired = true;
    return lk;
}

std::string hmac_sha256_hex(std::string_view key, std::string_view msg)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int                               len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(msg.data()),
         static_cast<int>(msg.size()), out.data(), &len);
    static const char* H = "0123456789abcdef";
    std::string        r(2 * len, '0');
    for (unsigned int i = 0; i < len; ++i) {
        r[2 * i]     = H[out[i] >> 4];
        r[2 * i + 1] = H[out[i] & 0xF];
    }
    return r;
}

std::string default_nonce()
{
    // 16 random bytes -> 32 lowercase hex chars (128 bits of entropy).
    // we deliberately avoid boost::uuids.
    unsigned char buf[16];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        // RAND_bytes should not fail in practice; fall back to zeros so the
        // call is still total (the signature still validates, just with a
        // weak nonce — server-side nonce-replay protection catches abuse).
        for (auto& b : buf)
            b = 0;
    }
    static const char* H = "0123456789abcdef";
    std::string        r(32, '0');
    for (int i = 0; i < 16; ++i) {
        r[2 * i]     = H[buf[i] >> 4];
        r[2 * i + 1] = H[buf[i] & 0xF];
    }
    return r;
}

RealtimeRequest build_realtime_request(
    const SnapLogDeps& deps, const SnapLogConfig& cfg, const std::string& clientId, const std::string& token, NonceGenerator nonce_gen)
{
    RealtimeRequest r;

    auto add_common = [&]() {
        r.headers.emplace_back("X-Client-Type", kSnapLogClientType);
        r.headers.emplace_back("X-Client-Id", clientId);
    };

    if (!token.empty()) {
        // Logged-in path: Bearer + the print upload endpoint.
        // The log gateway requires the "Bearer " prefix
        // a bare token is rejected as 110002 "Missing
        // authorization" and, since the realtime worker is fire-and-forget, the
        // event would be silently dropped. Note the app's stored token is bare
        // (parsed from the login redirect's token= param), so we prepend here.
        r.url = cfg.gateway_base + "/api/log/upload/print";
        r.headers.emplace_back("Authorization", std::string("Bearer ") + token);
        add_common();
    } else {
        // Public path: HMAC-signed, public endpoint.
        r.url = cfg.gateway_base + "/api/log/public/upload/print";
        add_common();
        // Timestamp in ms from the injected clock.
        int64_t     ts     = deps.now_ms ? deps.now_ms() : 0;
        std::string ts_str = std::to_string(ts);
        std::string nonce  = nonce_gen ? nonce_gen() : default_nonce();
        // X-Sign = HMAC_SHA256(secret, clientType||clientId||ts||nonce)
        std::string sign = hmac_sha256_hex(cfg.hmac_secret, std::string(kSnapLogClientType) + clientId + ts_str + nonce);
        r.headers.emplace_back("X-Timestamp", ts_str);
        r.headers.emplace_back("X-Nonce", nonce);
        r.headers.emplace_back("X-Sign", sign);
    }
    return r;
}

std::string redact_path(const std::string& home, const std::string& p)
{
    if (home.empty() || p.empty())
        return p;
    auto norm = [](std::string s) {
        for (auto& c : s)
            if (c == '\\')
                c = '/';
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    };
    std::string nhome = norm(home);
    std::string np    = norm(p);
    if (np.rfind(nhome, 0) == 0)
        return "~" + p.substr(home.size());
    // Conservative fallback: if the path matches a user-home pattern anywhere
    // (optional drive letter + / or \ + Users or home + / or \ + name segment),
    // collapse the WHOLE path to "~". Don't leak the structure of a path that
    // is under some user's home (e.g. AppData/Roaming/...). The previous regex
    // orphane a leading drive letter (C:~/AppData) and only stripped the leaf.
    static const std::regex re(R"((?:[A-Za-z]:)?(?:/|\\)(?:Users|home)(?:/|\\)[^/\\]+)", std::regex::icase);
    if (std::regex_search(p, re))
        return "~";
    return p;
}

std::string mask_secret_in_value(std::string v)
{
    // Value-level denylist — operates on individual string values, never on
    // serialized JSON. Each pattern consumes the secret token/value and
    // replaces it wholesale with "***".
    static const std::vector<std::regex> deny = {// PEM private-key headers (body usually multi-line; header is the tell)
                                                 std::regex(R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)"),
                                                 // AWS access-key IDs (20 chars: AKIA + 16 uppercase alnum)
                                                 std::regex(R"(AKIA[0-9A-Z]{16})"),
                                                 // JWTs: three base64url segments separated by dots
                                                 std::regex(R"(eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+)"),
                                                 // GitHub personal access tokens (legacy): ghp_ + 36 alnum
                                                 std::regex(R"(ghp_[A-Za-z0-9]{36})"),
                                                 // key=value / key:value secrets (case-insensitive key). Requires a
                                                 // literal '=' or ':' separator between the keyword and the value so
                                                 // that words like "tokenize" or "tokens" are NOT matched. The keyword
                                                 // set intentionally omits "tok" (it caused false positives on words
                                                 // like "tokenize", "tokens", "tok-model").
                                                 std::regex(R"((password|secret|token|authorization|refresh_token)\s*[=:]\s*\S+)",
                                                            std::regex::icase),
                                                 // Email addresses
                                                 std::regex(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})")};
    for (const auto& re : deny)
        v = std::regex_replace(v, re, "***");
    return v;
}

std::string hash_pii(const std::string& clientId, const std::string& serial)
{
    // Reuse the hmac primitive purely for its sha256 core: clientId is passed
    // as the HMAC key only so we don't duplicate the OpenSSL plumbing. Taking
    // the first 16 hex chars yields a stable 64-bit correlation id that does
    // not reveal the original serial.
    return hmac_sha256_hex(clientId, serial).substr(0, 16);
}

// Local wall-clock time as "YYYY-MM-DD HH:MM:SS" via the C library localtime
// (reflects the user's TZ). Returns "" for non-positive timestamps. Defined
// here (above the renderers) so both render_realtime_body and render_batch_line
// can call it without a forward declaration.
std::string format_local_time_ms(int64_t ms)
{
    if (ms <= 0)
        return std::string();
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm     tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0)
        return std::string();
#else
    if (localtime_r(&t, &tm) == nullptr)
        return std::string();
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return std::string(buf);
}
namespace {
bool        looks_like_path(const std::string& value);
bool        is_pii_key(const std::string& key);
bool        is_secret_key(const std::string& key);
std::string utf8_safe_prefix(const std::string& value, std::size_t budget);
std::string sanitize_message(const std::string& home, const std::string& message);
} // namespace

std::string render_realtime_body(
    SnapLogLevel level, const std::string& msg, const SnapLogExt& ext, std::size_t line_max_bytes, const RealtimeIdentity& id)
{
    nlohmann::json j;
    j["clientType"] = kSnapLogClientType;
    // clientId = the per-install InstanceID (top-level, gateway contract).
    j["clientId"] = utf8_safe_prefix(id.client_id, id.client_id.size());
    j["level"]    = level_str(level);

    // Build ext: hard-reject keys starting with "raw" (case-insensitive, so
    // rawGcode / RawGcode / RAW_DATA / raw3mf are all dropped), mask all
    // string values, and enforce per-string + total-ext size caps.
    // per-string ≤2KB, ext ≤8KB. Protecting eventName (and opId)
    // from whole-ext drop because they are how Grafana filters.
    static constexpr std::size_t kMaxExtValueBytes = 2000;
    static constexpr std::size_t kMaxExtTotalBytes = 8000;
    const std::string            val_trunc_suffix  = u8"…[truncated]"; // "...[truncated]"

    auto is_protected_key = [](const std::string& k) {
        return k == "eventName" || k == "opId" || k == "clientUUID" || k == "connect_clientid" || k == "printerSN" || k == "processId" ||
               k == "deviceId" || k == "userId" || k == "orcaVersion" || k == "os" || k == "batchId" || k == "localTime";
    };
    auto truncate_value = [&](const std::string& key, const std::string& v) -> std::string {
        std::string sanitized;
        if (is_secret_key(key)) {
            sanitized = "***";
        } else if (is_pii_key(key) && !v.empty()) {
            sanitized = hash_pii(id.client_id, v);
        } else if (is_pii_key(key)) {
            sanitized.clear();
        } else if (looks_like_path(v)) {
            sanitized = mask_secret_in_value(redact_path(id.home_for_redact, v));
        } else {
            sanitized = mask_secret_in_value(v);
        }
        sanitized = utf8_safe_prefix(sanitized, sanitized.size());
        if (sanitized.size() <= kMaxExtValueBytes)
            return sanitized;
        std::size_t budget = (kMaxExtValueBytes > val_trunc_suffix.size()) ? (kMaxExtValueBytes - val_trunc_suffix.size()) : 0;
        return utf8_safe_prefix(sanitized, budget) + val_trunc_suffix;
    };

    // Build ext entries preserving insertion order, truncating each value.
    // Use an ordered vector so FIFO dropping is predictable.
    std::vector<std::pair<std::string, std::string>> ext_pairs;
    ext_pairs.reserve(ext.size() + 10);
    // Standard identity/metadata (always present, protected from drop). Values
    // are "" when unavailable (not logged in / no printer connected).
    ext_pairs.emplace_back("clientUUID", truncate_value("clientUUID", id.client_id));
    ext_pairs.emplace_back("processId", truncate_value("processId", id.process_id));
    ext_pairs.emplace_back("connect_clientid", truncate_value("connect_clientid", id.connect_clientid));
    ext_pairs.emplace_back("printerSN", truncate_value("printerSN", id.printer_sn));
    ext_pairs.emplace_back("deviceId", truncate_value("deviceId", id.device_id));
    ext_pairs.emplace_back("userId", truncate_value("userId", id.user_id));
    ext_pairs.emplace_back("orcaVersion", truncate_value("orcaVersion", id.orca_version));
    ext_pairs.emplace_back("os", truncate_value("os", id.os));
    ext_pairs.emplace_back("batchId", truncate_value("batchId", id.batch_id));
    ext_pairs.emplace_back("localTime", truncate_value("localTime", format_local_time_ms(id.ts_ms)));
    for (const auto& kv : ext) {
        const std::string& key = kv.first;
        if (key.size() >= 3) {
            // Case-insensitive compare of first 3 chars to "raw".
            char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[1])));
            char c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[2])));
            if (c0 == 'r' && c1 == 'a' && c2 == 'w')
                continue; // raw* rejected
        }
        std::string safe_key = utf8_safe_prefix(key, key.size());
        ext_pairs.emplace_back(safe_key, truncate_value(key, kv.second));
    }

    // Serialize ext candidates to measure total bytes, dropping non-protected
    // FIFO entries if the ext object exceeds kMaxExtTotalBytes.
    auto build_ext_obj = [&](const std::vector<std::pair<std::string, std::string>>& pairs) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : pairs)
            obj[kv.first] = kv.second;
        return obj;
    };

    // Greedy: start with all pairs. If over cap, drop oldest non-protected entry.
    // Always keep eventName and opId if present.
    std::vector<std::pair<std::string, std::string>> kept = ext_pairs;
    while (!kept.empty()) {
        nlohmann::json test_obj   = build_ext_obj(kept);
        std::string    serialized = test_obj.dump();
        if (serialized.size() <= kMaxExtTotalBytes)
            break;
        // Find the oldest non-protected entry to drop (FIFO).
        auto drop_it = kept.end();
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (!is_protected_key(it->first)) {
                drop_it = it;
                break;
            }
        }
        if (drop_it == kept.end())
            break; // only protected entries left, stop
        kept.erase(drop_it);
    }
    j["ext"] = build_ext_obj(kept);

    // Enforce the line cap. Truncate the message first (it's the dominant
    // contributor), then if still over, drop ext entirely, then message
    // truncation suffix. We never exceed line_max_bytes and always keep valid
    // JSON.
    std::string message = sanitize_message(id.home_for_redact, msg);
    j["message"]        = message;

    auto serialize = [&]() { return j.dump(); };

    std::string out = serialize();
    if (out.size() <= line_max_bytes)
        return out;

    // Step 1: truncate message.
    const std::string trunc_suffix = u8"…[truncated]"; // "...[truncated]"
    // Estimate overhead = everything except message content.
    // message is stored under "message": "...", so overhead ≈ size - msg.size().
    std::size_t overhead = out.size() - message.size();
    // Budget for message content (leave room for the truncation suffix).
    std::size_t budget = (line_max_bytes > overhead + trunc_suffix.size()) ? (line_max_bytes - overhead - trunc_suffix.size()) : 0;
    if (budget < message.size()) {
        message      = utf8_safe_prefix(message, budget) + trunc_suffix;
        j["message"] = message;
        out          = serialize();
    }

    // Step 2: if still over, trim ext to only protected keys (eventName/opId).
    // The per-value/ext caps already bound ext to ~8KB, so this is a rare
    // last-resort — but we still preserve the Grafana-query keys.
    if (out.size() > line_max_bytes) {
        nlohmann::json minimal_ext = nlohmann::json::object();
        for (const auto& kv : kept) {
            if (is_protected_key(kv.first))
                minimal_ext[kv.first] = kv.second;
        }
        if (minimal_ext.empty())
            j["ext"] = nullptr;
        else
            j["ext"] = minimal_ext;
        out = serialize();
    }

    // Step 3: hard cap — if still over (degenerate tiny line_max_bytes), chop
    // the tail and keep the JSON structurally valid by re-dumping a minimal
    // object. This is a last-resort guarantee.
    if (out.size() > line_max_bytes) {
        j["message"] = trunc_suffix;
        j["ext"]     = nullptr;
        out          = serialize();
        if (out.size() > line_max_bytes) {
            // Absolute floor: emit a minimal truncated marker.
            out = nlohmann::json{{"clientType", kSnapLogClientType},
                                 {"clientId", utf8_safe_prefix(id.client_id, id.client_id.size())},
                                 {"level", level_str(level)},
                                 {"message", trunc_suffix},
                                 {"ext", nullptr}}
                      .dump();
        }
    }
    return out;
}

namespace {

// Format a UTC millisecond timestamp as ISO8601: YYYY-MM-DDThh:mm:ss.sssZ
// Uses <chrono> + manual calendar math (no strftime timezone dependency).
// ts_ms is milliseconds since the Unix epoch (UTC).
std::string format_iso8601_utc(int64_t ts_ms)
{
    using namespace std::chrono;
    // Split into seconds + milliseconds.
    int64_t total_sec = ts_ms / 1000;
    int     ms_part   = static_cast<int>(ts_ms % 1000);
    if (ms_part < 0) {
        ms_part += 1000;
        --total_sec;
    } // guard against negatives

    // Convert days since epoch to civil date (Howard Hinnant's algorithm).
    int64_t days        = total_sec / 86400;
    int     secs_of_day = static_cast<int>(total_sec % 86400);
    if (secs_of_day < 0) {
        secs_of_day += 86400;
        --days;
    }

    int hour   = secs_of_day / 3600;
    int minute = (secs_of_day % 3600) / 60;
    int second = secs_of_day % 60;

    // days_from_civil inverse: compute (y, m, d) from days since 1970-01-01.
    days += 719468; // shift to 0000-03-01 epoch
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;                                   // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    int64_t y   = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        // [0, 365]
    int     mp  = (5 * doy + 2) / 153;                            // [0, 11]
    int     d   = static_cast<int>(doy - (153 * mp + 2) / 5 + 1); // [1, 31]
    int     m   = mp < 10 ? mp + 3 : mp - 9;                      // [1, 12]
    if (m <= 2)
        y += 1;

    // YYYY-MM-DDThh:mm:ss.sssZ
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(y), m, d, hour, minute, second, ms_part);
    return std::string(buf);
}

// Heuristic: does a string value look like a filesystem path? Used to decide
// whether to apply redact_path to an ext value. Checks for path separators
// and drive letters.
bool looks_like_path(const std::string& v)
{
    if (v.empty())
        return false;
    // Drive letter (Windows): "C:\..." or "C:/..."
    if (v.size() >= 3 && v[1] == ':' && (v[2] == '\\' || v[2] == '/'))
        return true;
    // Posix absolute: "/..."
    if (v[0] == '/')
        return true;
    // Backslash path: "\..."
    if (v[0] == '\\')
        return true;
    // Home-relative with ~/ prefix
    if (v.size() >= 2 && v[0] == '~' && (v[1] == '/' || v[1] == '\\'))
        return true;
    return false;
}

// PII keys that must be hashed (not masked) before going into ext.
bool is_pii_key(const std::string& key)
{
    // Case-insensitive comparison for known PII field names.
    std::string lower;
    lower.reserve(key.size());
    for (char c : key)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "printerserial" || lower == "printer_serial" || lower == "printersn" || lower == "printer_sn" || lower == "sn" ||
           lower == "serialnumber" || lower == "serial_number" || lower == "mac" || lower == "macaddress" || lower == "mac_address" ||
           lower == "ip" || lower == "ipaddress" || lower == "ip_address";
}

// Secret key names: when the KEY itself names a secret, the value is treated
// as a secret regardless of its content (the value-level masker looks for
// "password=<val>" patterns inside a string, but when the key/value pair is
// split, the bare value won't match). We replace the whole value with "***".
bool is_secret_key(const std::string& key)
{
    std::string lower;
    lower.reserve(key.size());
    for (char c : key)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // Exact-match secret keys.
    if (lower == "password" || lower == "passwd" || lower == "pwd" || lower == "secret" || lower == "token" || lower == "accesstoken" ||
        lower == "access_token" || lower == "refreshtoken" || lower == "refresh_token" || lower == "authorization" || lower == "auth" ||
        lower == "apikey" || lower == "api_key" || lower == "privatekey" || lower == "private_key" || lower == "sessiontoken" ||
        lower == "session_token")
        return true;
    // Contains "password", "secret", "token", "credential" as substrings.
    if (lower.find("password") != std::string::npos || lower.find("secret") != std::string::npos ||
        lower.find("credential") != std::string::npos)
        return true;
    return false;
}

} // anonymous namespace

std::string render_batch_line(
    SnapLogLevel lvl, const std::string& msg, const SnapLogExt& ext, std::size_t line_max_bytes, const BatchLineContext& ctx)
{
    nlohmann::json j;

    j["timestamp"]  = format_iso8601_utc(ctx.ts_ms);
    j["level"]      = level_str(lvl);
    j["service"]    = ctx.service.empty() ? "snapmaker-desktop" : ctx.service;
    j["message"]    = sanitize_message(ctx.home_for_redact, msg);
    j["eventName"]  = utf8_safe_prefix(mask_secret_in_value(ctx.eventName), ctx.eventName.size());
    j["userId"]     = utf8_safe_prefix(mask_secret_in_value(ctx.userId), ctx.userId.size());
    j["clientType"] = ctx.clientType.empty() ? kSnapLogClientType : utf8_safe_prefix(ctx.clientType, ctx.clientType.size());

    // Fix B: batchId moved from top-level to ext (strict mapping on the server
    // side drops unknown top-level fields). It is injected into ext_pairs below.

    // Build ext object: standard identity fields + sanitized caller keys.
    // Policy: drop raw* keys; mask secrets; redact paths; hash PII.
    static constexpr std::size_t kMaxExtValueBytes = 2000;
    static constexpr std::size_t kMaxExtTotalBytes = 8000;
    const std::string            trunc_suffix      = u8"…[truncated]";

    auto sanitize_value = [&](const std::string& key, const std::string& v) -> std::string {
        std::string out_val;
        if (is_secret_key(key)) {
            // The key names a secret: replace the value wholesale.
            out_val = "***";
        } else if (is_pii_key(key) && !v.empty()) {
            // PII: hash with clientId as the HMAC key.
            out_val = hash_pii(ctx.clientId, v);
        } else if (is_pii_key(key)) {
            out_val.clear();
        } else if (looks_like_path(v)) {
            // Path-like: redact home prefix, then mask any remaining secrets.
            out_val = mask_secret_in_value(redact_path(ctx.home_for_redact, v));
        } else {
            out_val = mask_secret_in_value(v);
        }
        // Per-value truncation.
        out_val = utf8_safe_prefix(out_val, out_val.size());
        if (out_val.size() <= kMaxExtValueBytes)
            return out_val;
        std::size_t budget = (kMaxExtValueBytes > trunc_suffix.size()) ? (kMaxExtValueBytes - trunc_suffix.size()) : 0;
        return utf8_safe_prefix(out_val, budget) + trunc_suffix;
    };

    // Build ext entries preserving insertion order.
    std::vector<std::pair<std::string, std::string>> ext_pairs;
    ext_pairs.emplace_back("clientUUID", sanitize_value("clientUUID", ctx.clientId));
    ext_pairs.emplace_back("processId", sanitize_value("processId", ctx.processId));
    ext_pairs.emplace_back("connect_clientid", sanitize_value("connect_clientid", ctx.connect_clientid));
    ext_pairs.emplace_back("printerSN", sanitize_value("printerSN", ctx.print_sn));
    ext_pairs.emplace_back("orcaVersion", sanitize_value("orcaVersion", ctx.appVersion));
    ext_pairs.emplace_back("os", sanitize_value("os", ctx.osVersion));
    ext_pairs.emplace_back("batchId", ctx.batchId);
    ext_pairs.emplace_back("localTime", format_local_time_ms(ctx.ts_ms));
    for (const auto& kv : ext) {
        const std::string& key = kv.first;
        // Drop raw* keys (case-insensitive prefix check).
        if (key.size() >= 3) {
            char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
            char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[1])));
            char c2 = static_cast<char>(std::tolower(static_cast<unsigned char>(key[2])));
            if (c0 == 'r' && c1 == 'a' && c2 == 'w')
                continue;
        }
        std::string safe_key = utf8_safe_prefix(key, key.size());
        ext_pairs.emplace_back(safe_key, sanitize_value(key, kv.second));
    }

    // Enforce total ext cap by FIFO-dropping non-protected entries.
    auto build_ext_obj = [](const std::vector<std::pair<std::string, std::string>>& pairs) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : pairs)
            obj[kv.first] = kv.second;
        return obj;
    };

    std::vector<std::pair<std::string, std::string>> kept = ext_pairs;
    // Never drop clientUUID (it's protected) — it's how the server correlates.
    auto is_batch_protected = [](const std::string& k) {
        return k == "clientUUID" || k == "processId" || k == "connect_clientid" || k == "printerSN" || k == "orcaVersion" || k == "os" ||
               k == "batchId" || k == "localTime" || k == "eventName" || k == "opId";
    };
    while (!kept.empty()) {
        nlohmann::json test_obj   = build_ext_obj(kept);
        std::string    serialized = test_obj.dump();
        if (serialized.size() <= kMaxExtTotalBytes)
            break;
        auto drop_it = kept.end();
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (!is_batch_protected(it->first)) {
                drop_it = it;
                break;
            }
        }
        if (drop_it == kept.end())
            break;
        kept.erase(drop_it);
    }
    j["ext"] = build_ext_obj(kept);

    // Enforce the line cap. Mirror render_realtime_body's truncation skeleton.
    std::string message   = sanitize_message(ctx.home_for_redact, msg);
    auto        serialize = [&]() { return j.dump(); };

    std::string out = serialize();
    if (out.size() <= line_max_bytes) {
        out.push_back('\n'); // NDJSON: line ends with newline.
        return out;
    }

    // Step 1: truncate message.
    {
        std::size_t overhead = out.size() - message.size();
        std::size_t budget   = (line_max_bytes > overhead + trunc_suffix.size()) ? (line_max_bytes - overhead - trunc_suffix.size()) : 0;
        if (budget < message.size()) {
            message      = utf8_safe_prefix(message, budget) + trunc_suffix;
            j["message"] = message;
            out          = serialize();
        }
    }

    // Step 2: if still over, trim ext to only protected keys.
    if (out.size() > line_max_bytes) {
        nlohmann::json minimal_ext = nlohmann::json::object();
        for (const auto& kv : kept) {
            if (is_batch_protected(kv.first))
                minimal_ext[kv.first] = kv.second;
        }
        if (minimal_ext.empty())
            j["ext"] = nullptr;
        else
            j["ext"] = minimal_ext;
        out = serialize();
    }

    // Step 3: hard cap — degenerate tiny line_max_bytes.
    if (out.size() > line_max_bytes) {
        j["message"] = trunc_suffix;
        j["ext"]     = nullptr;
        out          = serialize();
        if (out.size() > line_max_bytes) {
            out = nlohmann::json{{"timestamp", format_iso8601_utc(ctx.ts_ms)},
                                 {"level", level_str(lvl)},
                                 {"service", j["service"]},
                                 {"message", trunc_suffix},
                                 {"ext", nullptr}}
                      .dump();
        }
    }

    out.push_back('\n'); // NDJSON: line ends with newline.
    return out;
}

bool RateLimiter::allow(const std::string&                eventName,
                        int                               default_cap,
                        int                               total_unknown_cap,
                        const std::map<std::string, int>& per_name_caps)
{
    std::lock_guard<std::mutex> lk(mu_);
    int64_t                     now = now_ms_();

    // Resolve this name's cap: registered → configured, else default.
    bool       is_registered = false;
    int        name_cap      = default_cap;
    const auto it            = per_name_caps.find(eventName);
    if (it != per_name_caps.end()) {
        is_registered = true;
        name_cap      = it->second;
    }

    // Refill helper: token-bucket. tokens = min(cap, tokens + elapsed_sec * cap).
    // First touch (!initialized) starts the bucket full so the first burst
    // within a window isn't penalized. Guards against clock regressions.
    auto refill = [now](Bucket& b, int cap) -> double {
        if (!b.initialized) {
            b.tokens      = static_cast<double>(cap);
            b.last_ms     = now;
            b.initialized = true;
            return b.tokens;
        }
        double elapsed_sec = static_cast<double>(now - b.last_ms) / 1000.0;
        if (elapsed_sec < 0.0)
            elapsed_sec = 0.0; // clock went backwards — be safe
        b.tokens  = std::min(static_cast<double>(cap), b.tokens + elapsed_sec * static_cast<double>(cap));
        b.last_ms = now;
        return b.tokens;
    };

    // Per-name bucket (lazily created; first call starts full via refill()).
    Bucket& nb          = name_buckets_[eventName];
    double  name_tokens = refill(nb, name_cap);
    if (name_tokens < 1.0) {
        return false; // throttled by per-name bucket
    }

    // Unregistered names are ALSO gated by the shared unknown-aggregate bucket.
    if (!is_registered) {
        double agg_tokens = refill(unknown_aggregate_, total_unknown_cap);
        if (agg_tokens < 1.0) {
            return false; // throttled by aggregate bucket
        }
        unknown_aggregate_.tokens = agg_tokens - 1.0;
    }

    // Admit: consume one token from the per-name bucket (and the aggregate
    // for unregistered, already consumed above).
    nb.tokens = name_tokens - 1.0;
    return true;
}

// RtEvent and SnapLogClient::Internals are defined in the header so tests can
// construct a fake Internals and drive bt_worker_loop directly. See SnapLogClient.hpp.

SnapLogClient& SnapLogClient::instance()
{
    // Detached consent joiners may still own lifecycle state after app exit.
    // Intentionally leak the singleton so they never race static destruction.
    static SnapLogClient* inst = new SnapLogClient();
    return *inst;
}

// Realtime worker thread function. Runs in its own thread;
// contains NO Catch2 assertions (they're not thread-safe). It polls:
//   - exit conditions (stop/deps_invalid/consent)
//   - the in-flight handle (clear when done)
//   - the queue (send one event when no handle is in-flight)
//
// Invariant: at most one rt_handle is in-flight at a time. The worker is the
// only writer of rt_handle, so the slot is naturally single-writer; we still
// hold m_h_mu during the consent-recheck + store so that cancel_current()

// Fix D: This worker is fire-and-forget for the realtime/print endpoint.
// When the response completes (handle->done()), we simply clear the slot
// — we never inspect the response body or check code==0 vs code==200.
// The realtime endpoint accepts events best-effort; there is no retry or
// auth-dead logic triggered by the response code. This is by design.
void SnapLogClient::rt_worker_loop(std::shared_ptr<Internals> in)
{
    using namespace std::chrono_literals;
    while (true) {
        // 1. Exit conditions (lock-free atomics).
        if (in->stop_receiving.load(std::memory_order_relaxed))
            return;
        if (in->deps_invalid.load(std::memory_order_relaxed))
            return;
        if (!in->consent.load(std::memory_order_relaxed)) {
            // Consent false: sleep and re-check. We do NOT exit the thread on
            // consent-false (consent may flip back on); we just stop sending.
            std::this_thread::sleep_for(std::chrono::milliseconds(in->cfg.poll_interval_ms > 0 ? in->cfg.poll_interval_ms : 1));
            continue;
        }

        // 2. Check the in-flight handle. If done, clear the slot.
        {
            std::lock_guard<std::mutex> lk(in->m_h_mu);
            if (in->rt_handle && in->rt_handle->done()) {
                in->rt_handle.reset();
            }
        }

        // 3. If no handle in-flight, try to pop one event and send it.
        bool have_handle = false;
        {
            std::lock_guard<std::mutex> lk(in->m_h_mu);
            have_handle = (in->rt_handle != nullptr);
        }
        if (!have_handle) {
            // Pop one event under queue_mu.
            std::optional<RtEvent> ev_opt;
            {
                std::lock_guard<std::mutex> qk(in->queue_mu);
                if (!in->rt_queue.empty()) {
                    ev_opt = std::move(in->rt_queue.front());
                    in->rt_queue.pop_front();
                }
            }
            if (ev_opt.has_value()) {
                const RtEvent& ev = *ev_opt;
                // Re-check consent (it may have flipped after enqueue).
                if (!in->consent.load(std::memory_order_relaxed)) {
                    // Consent flipped false — drop the event without sending.
                    in->dropped.fetch_add(1, std::memory_order_relaxed);
                } else if (in->deps_invalid.load(std::memory_order_relaxed) || in->stop_receiving.load(std::memory_order_relaxed)) {
                    // Shutdown/deps-invalid racing in — drop and exit.
                    in->dropped.fetch_add(1, std::memory_order_relaxed);
                    return;
                } else {
                    // Build the request OUTSIDE m_h_mu (now_ms/hmac may be
                    // non-trivial; keep the lock window tight).
                    std::string      token_snap;
                    RealtimeIdentity id;
                    id.client_id       = in->machine_id_snapshot; // set once at init, immutable
                    id.process_id      = in->cfg.process_id;
                    id.home_for_redact = in->cfg.home_for_redact;
                    {
                        std::lock_guard<std::mutex> tk(in->token_mu);
                        token_snap          = in->user_token;
                        id.connect_clientid = in->connect_clientid;
                        id.printer_sn       = in->print_sn;
                        id.device_id        = in->device_id;
                        id.user_id          = in->user_id;
                        id.batch_id         = in->current_batch_id_snapshot;
                    }
                    id.orca_version      = in->cfg.app_version;
                    id.os                = in->cfg.os_version;
                    id.ts_ms             = in->deps.now_ms ? in->deps.now_ms() : 0;
                    NonceGenerator  ng   = in->nonce_gen_for_test ? in->nonce_gen_for_test : NonceGenerator{};
                    RealtimeRequest req  = build_realtime_request(in->deps, in->cfg, in->machine_id_snapshot, token_snap, ng);
                    std::string     body = render_realtime_body(ev.level, ev.message, ev.ext, in->cfg.line_max_bytes, id);

                    // Fire the async request OUTSIDE m_h_mu (do_request creates
                    // a handle and returns immediately; holding the lock here
                    // would serialize cancel_current against the request issue).
                    std::shared_ptr<SnapLogHandle> h;
                    if (in->deps.do_request) {
                        h = in->deps.do_request("POST", req.url, req.headers,
                                                /*body_file*/ nullptr, &body);
                    }
                    if (h) {
                        std::lock_guard<std::mutex> hk(in->m_h_mu);
                        if (in->consent.load(std::memory_order_relaxed) && !in->stop_receiving.load(std::memory_order_relaxed)) {
                            in->rt_handle = std::move(h);
                        } else {
                            // Consent flipped / shutting down between issue and
                            // store — cancel the just-issued handle and drop.
                            h->cancel();
                            in->dropped.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        // do_request not set — drop (counted).
                        in->dropped.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        // 4. Sleep for the poll interval before the next iteration.
        std::this_thread::sleep_for(std::chrono::milliseconds(in->cfg.poll_interval_ms > 0 ? in->cfg.poll_interval_ms : 1));
    }
}

// Forward declaration: generate_batch_id is defined later (near bt_worker_loop)
// but init() needs it to seed current_batch_id on startup.
namespace {
std::string generate_batch_id();
} // anonymous namespace

static void reset_batch_upload_state(SnapLogClient::Internals& in);

static bool purge_spool(const std::shared_ptr<SnapLogClient::Internals>& in)
{
    if (!in || !is_valid_spool_path(in->spool_dir_resolved))
        return false;

    namespace fs = boost::filesystem;
    boost::system::error_code dir_ec;
    if (!fs::is_directory(in->spool_dir_resolved, dir_ec)) {
        in->purge_delete_failed.fetch_add(1, std::memory_order_relaxed);
        BOOST_LOG_TRIVIAL(warning) << "SnapLog batch upload disabled: privacy purge failed at directory check: " << dir_ec.message();
        return false;
    }

    bool all_purged = true;

    auto purge_file = [&in](const fs::path& path) {
        const auto mark_failure = [&in](const char* stage, const boost::system::error_code& failure_ec) {
            in->purge_delete_failed.fetch_add(1, std::memory_order_relaxed);
            BOOST_LOG_TRIVIAL(warning) << "SnapLog batch upload disabled: privacy purge failed at " << stage << ": " << failure_ec.message();
            return false;
        };

        boost::system::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (!ec && !exists)
            return true;
        if (ec == boost::system::errc::no_such_file_or_directory)
            return true;
        if (ec)
            return mark_failure("exists", ec);

        const bool removed = fs::remove(path, ec);
        if (!ec) {
            boost::system::error_code exists_ec;
            if (removed || !fs::exists(path, exists_ec))
                return true;
        }
        if (ec == boost::system::errc::no_such_file_or_directory)
            return true;

        ec.clear();
        boost::system::error_code resize_ec;
        fs::resize_file(path, 0, resize_ec);
        if (resize_ec)
            return mark_failure("resize", resize_ec);

        boost::system::error_code size_ec;
        const auto size = fs::file_size(path, size_ec);
        if (size_ec || size != 0)
            return mark_failure("file size", size_ec);

        boost::system::error_code remove_ec;
        (void) fs::remove(path, remove_ec);
        if (!remove_ec || remove_ec == boost::system::errc::no_such_file_or_directory)
            return true;

        return mark_failure("remove after truncate", remove_ec);
    };

    all_purged = purge_file(in->active_path) && all_purged;
    for (const auto& sealed : list_sealed(in->spool_dir_resolved)) {
        all_purged = purge_file(sealed) && all_purged;
    }

    return all_purged;
}

// Caller must hold m_lifecycle_mu. The consent-OFF retire path invokes this
// only after its asynchronous joiner has drained the retired worker.
void SnapLogClient::start_batch_worker(const std::shared_ptr<Internals>& in)
{
    if (!in || in->spool_dir_resolved.empty() || in->bt_worker.joinable())
        return;

    if (!in->bt_spool_lock.acquired && m_purge_lock_spool_dir == in->spool_dir_resolved && m_purge_spool_lock.acquired) {
        in->bt_spool_lock       = std::move(m_purge_spool_lock);
        m_purge_lock_spool_dir.clear();
    }

    if (!in->bt_spool_lock.acquired) {
        in->bt_spool_lock = try_acquire_spool_lock(in->spool_dir_resolved);
        if (!in->bt_spool_lock.acquired) {
            in->batch_locked_out.store(true);
            return;
        }
    }

    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    if (fs::exists(in->active_path, ec)) {
        boost::system::error_code size_ec;
        const auto active_size = fs::file_size(in->active_path, size_ec);
        if (!size_ec && active_size > 0) {
            std::string err;
            rotate_active(in->spool_dir_resolved, in->active_path, "recovery00000000", err);
        }
    }

    in->batch_deps_invalid.store(false, std::memory_order_release);
    in->stop_uploads.store(false, std::memory_order_release);
    in->drain_and_flush.store(false);
    in->stop_receiving.store(false);
    if (in->current_batch_id.empty()) {
        in->current_batch_id = generate_batch_id();
    }

    {
        std::lock_guard<std::mutex> hk(in->m_h_mu);
        in->bt_handle.reset();
    }
    reset_batch_upload_state(*in);
    in->events_in_active = 0;
    in->last_flush_ms    = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    std::shared_ptr<Internals> in_for_bt  = in;
    const uint64_t             generation = in->batch_worker_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    in->bt_worker = std::thread(bt_worker_loop, in_for_bt, generation);
}

static bool purge_completion_blocks(const std::shared_future<bool>& completion)
{
    if (!completion.valid())
        return false;
    if (completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return true;
    return !completion.get();
}

static bool purge_completion_active(const std::shared_future<bool>& completion)
{
    return completion.valid() && completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

std::shared_future<bool> SnapLogClient::begin_spool_purge(const std::shared_ptr<Internals>& in, std::thread retired_worker)
{
    // Caller must hold m_lifecycle_mu.
    if (!in || in->spool_dir_resolved.empty())
        return {};

    auto retired_worker_handle = std::make_shared<std::thread>(std::move(retired_worker));
    if (!in->bt_spool_lock.acquired && m_purge_lock_spool_dir == in->spool_dir_resolved && m_purge_spool_lock.acquired) {
        in->bt_spool_lock       = std::move(m_purge_spool_lock);
        m_purge_lock_spool_dir.clear();
    } else if (m_purge_lock_spool_dir != in->spool_dir_resolved) {
        m_purge_spool_lock       = SpoolLock{};
        m_purge_lock_spool_dir.clear();
    }

    if (purge_completion_active(m_purge_completion) && m_purge_spool_dir == in->spool_dir_resolved) {
        if (retired_worker_handle->joinable()) {
            std::thread([retired_worker_handle] { retired_worker_handle->join(); }).detach();
            return m_purge_completion;
        }
    }

    auto previous_completion = m_purge_completion;
    auto completion_promise = std::make_shared<std::promise<bool>>();
    auto completion = completion_promise->get_future().share();
    m_purge_spool_dir = in->spool_dir_resolved;
    m_purge_promise   = completion_promise;
    m_purge_completion = completion;

    std::thread(
        [this, in, completion_promise, previous_completion, retired_worker_handle]() mutable {
            PurgeCompletionGuard completion_guard(completion_promise);
            bool purged = false;

            if (retired_worker_handle->joinable())
                retired_worker_handle->join();
            if (previous_completion.valid())
                previous_completion.wait();

            if (!in->bt_spool_lock.acquired)
                in->bt_spool_lock = try_acquire_spool_lock(in->spool_dir_resolved);
            if (in->bt_spool_lock.acquired)
                purged = purge_spool(in);
            else {
                in->purge_delete_failed.fetch_add(1, std::memory_order_relaxed);
                BOOST_LOG_TRIVIAL(warning) << "SnapLog batch upload disabled: privacy purge could not acquire the spool lock";
            }

            std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
            if (in->bt_spool_lock.acquired) {
                m_purge_spool_lock       = std::move(in->bt_spool_lock);
                m_purge_lock_spool_dir = in->spool_dir_resolved;
            }
            if (m_purge_promise == completion_promise && purged) {
                m_purge_promise.reset();
                m_purge_completion = {};
                m_purge_spool_dir.clear();
            }
            if (m_int == in) {
                if (in->stop_receiving.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> state_lock(m_state_mu);
                    m_int.reset();
                } else if (purged && in->consent.load(std::memory_order_relaxed)) {
                    start_batch_worker(in);
                }
            }
            // The deferred starter must not observe completion before spool-lock ownership has moved.
            completion_guard.fulfill(purged);
        })
        .detach();

    return completion;
}

void SnapLogClient::start_batch_worker_after_purge(const std::shared_ptr<Internals>& in)
{
    // Caller must hold m_lifecycle_mu.
    if (!in || in->spool_dir_resolved.empty())
        return;

    if (!spool_purge_blocks_worker(in->spool_dir_resolved)) {
        start_batch_worker(in);
        return;
    }

    if (!purge_completion_active(m_purge_completion)) {
        // A completed false purge means removal could not be verified. Keeping
        // the batch channel disabled is deliberate: uploading a residual file
        // would violate consent-OFF. A later consent OFF starts a fresh purge.
        return;
    }

    auto completion = m_purge_completion;
    std::thread([this, in, completion] {
        completion.wait();
        std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
        if (m_int != in || !in->consent.load(std::memory_order_relaxed) || spool_purge_blocks_worker(in->spool_dir_resolved))
            return;
        start_batch_worker(in);
    }).detach();
}

bool SnapLogClient::spool_purge_blocks_worker(const boost::filesystem::path& spool_dir) const
{
    return m_purge_spool_dir == spool_dir && purge_completion_blocks(m_purge_completion);
}

void SnapLogClient::init(SnapLogDeps deps, SnapLogConfig cfg)
{
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
    std::shared_ptr<Internals>  old;
    {
        std::lock_guard<std::mutex> state_lock(m_state_mu);
        old = m_int;
    }
    // If a previous Internals exists (re-init without explicit shutdown),
    // stop its workers first. Use cancel-then-join (same pattern as shutdown)
    // to bound the join.
    if (old) {
        // Stop rt_worker with the same bounded pattern as shutdown. The
        // joiner pins Internals so a blocked worker can never be destroyed
        // while this helper thread is still joining it.
        if (old->rt_worker.joinable()) {
            old->stop_receiving.store(true);
            cancel_current(old, SnapLogPolicy::Realtime);
            auto                       rt_join_promise = std::make_shared<std::promise<void>>();
            auto                       rt_join_future  = rt_join_promise->get_future();
            std::shared_ptr<Internals> rt_in_pin       = old;
            std::thread                rt_joiner([rt_in_pin, rt_join_promise]() {
                if (rt_in_pin->rt_worker.joinable()) {
                    rt_in_pin->rt_worker.join();
                    rt_join_promise->set_value();
                }
            });
            rt_joiner.detach();

            int rt_deadline_sec = rt_in_pin->cfg.realtime_join_deadline_sec;
            if (rt_deadline_sec < 1)
                rt_deadline_sec = 1;
            if (rt_join_future.wait_for(std::chrono::seconds(rt_deadline_sec)) != std::future_status::ready) {
                rt_in_pin->deps_invalid.store(true);
            }
        }
        // Stop bt_worker if it was started (re-init guard).
        if (old->bt_worker.joinable()) {
            old->stop_receiving.store(true);
            old->stop_uploads.store(true);
            old->batch_worker_generation.fetch_add(1, std::memory_order_acq_rel);
            cancel_current(old, SnapLogPolicy::Buffered);
            // Bounded join on bt_worker (same deadline pattern as shutdown).
            auto                       join_promise = std::make_shared<std::promise<void>>();
            auto                       join_future  = join_promise->get_future();
            std::shared_ptr<Internals> in_pin       = old;
            std::thread                joiner([in_pin, join_promise]() {
                if (in_pin->bt_worker.joinable()) {
                    in_pin->bt_worker.join();
                    join_promise->set_value();
                }
            });
            joiner.detach();
            int deadline_sec = in_pin->cfg.batch_join_deadline_sec;
            if (deadline_sec < 1)
                deadline_sec = 1;
            if (join_future.wait_for(std::chrono::seconds(deadline_sec)) == std::future_status::ready) {
                // Joined within deadline.
            } else {
                in_pin->deps_invalid.store(true);
                in_pin->batch_deps_invalid.store(true);
            }
        }
    }
    auto in = std::make_shared<Internals>(std::move(deps), std::move(cfg));
    if (in->deps.consent_ok)
        in->consent.store(in->deps.consent_ok());
    if (in->deps.machine_id)
        in->machine_id_snapshot = in->deps.machine_id();
    in->stop_receiving.store(false);

    const bool initial_consent = in->consent.load(std::memory_order_relaxed);
    if (!in->cfg.spool_dir.empty()) {
        namespace fs           = boost::filesystem;
        in->spool_dir_resolved = fs::path(in->cfg.spool_dir);
        in->active_path        = in->spool_dir_resolved / "active.log";

        // Create the spool directory (idempotent). The lock, crash recovery,
        // and privacy purge are all handled off this thread so a large spool
        // cannot block GUI startup or a re-init.
        boost::system::error_code ec;
        fs::create_directories(in->spool_dir_resolved, ec);
    }

    // Start the realtime worker thread (moves the shared_ptr by value so the
    // thread keeps Internals alive even if the client resets m_int during
    // shutdown; shutdown joins before m_int.reset() in the shutdown path).
    std::shared_ptr<Internals> in_for_thread = in;
    in->rt_worker                            = std::thread(rt_worker_loop, in_for_thread);

    // Start the batch worker thread if spool_dir is configured. drain_and_flush
    // starts false (only set true during shutdown for the final flush).
    if (initial_consent) {
        start_batch_worker_after_purge(in);
    } else {
        // Consent was already OFF at startup. Crash remnants belong to a
        // session the user did not agree to upload, so purge them instead
        // of rotating them into a sealed recovery batch.
        begin_spool_purge(in);
    }

    {
        std::lock_guard<std::mutex> state_lock(m_state_mu);
        m_int = std::move(in);
    }
}

std::shared_ptr<SnapLogClient::Internals> SnapLogClient::internals() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    return m_int;
}

void SnapLogClient::shutdown()
{
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
    auto                        in = internals();
    // Bounded shutdown:
    // 1. Flip stop_receiving so the worker's next poll-loop top exits.
    // 2. cancel_current(Realtime) — abort any in-flight handle so its done()
    //    trips and the worker clears the slot quickly.
    // 3. Bounded join: run the join on a helper thread, wait up to
    //    realtime_join_deadline_sec. If the worker hasn't exited by then
    //    (e.g. a production do_request stuck in DNS), we set deps_invalid
    //    (tells the worker to stop calling Deps) and detach the worker thread.
    //    The worker closure captures shared_ptr<Internals> (NOT this), so the
    //    detached worker drains safely after m_int.reset() without UAF.
    if (!in)
        return;
    const bool deferred_consent_purge = purge_completion_active(m_purge_completion) && m_purge_spool_dir == in->spool_dir_resolved;
    in->stop_receiving.store(true);
    cancel_current(in, SnapLogPolicy::Realtime); // abort in-flight so worker's done() trips

    if (in->rt_worker.joinable()) {
        // Run join on a helper thread, wait with a deadline. std::thread has
        // no try_join in C++17, so this is the portable pattern: a detached
        // joiner signals completion via a promise/future.
        //
        // The joiner captures a shared_ptr<Internals> copy (in_pin) BY VALUE
        // so Internals cannot be destroyed while the joiner is blocked inside
        // rt->join(). Without this pin, the deadline-exceeded path (detach +
        // m_int.reset()) could let the worker's exit drop the last ref,
        // destroying Internals (and rt_worker) mid-join — UB per
        // [thread.thread.constr]/4. With in_pin: client ref + worker ref +
        // joiner ref → Internals outlives the join.
        auto                       join_promise = std::make_shared<std::promise<void>>();
        auto                       join_future  = join_promise->get_future();
        std::shared_ptr<Internals> in_pin       = in; // increment refcount
        std::thread                joiner([rt = &in_pin->rt_worker, join_promise, in_pin]() {
            if (rt->joinable()) {
                rt->join();
                join_promise->set_value();
            }
            // in_pin released when the lambda ends (after set_value),
            // guaranteeing Internals stays alive across rt->join().
        });
        joiner.detach();

        // Shutdown is on the GUI exit path. Keep its total bounded even when a
        // transport ignores cancellation; a detached worker can finish later.
        int deadline_sec = std::min(in->cfg.realtime_join_deadline_sec, kShutdownRealtimeJoinMaxSec);
        if (deadline_sec < 1)
            deadline_sec = 1;
        if (join_future.wait_for(std::chrono::seconds(deadline_sec)) == std::future_status::ready) {
            // Worker joined within deadline — nothing more to do.
        } else {
            // Worker hasn't exited — detach it so we don't block forever.
            // Set deps_invalid so the worker stops calling Deps callbacks
            // (which may touch destructed GUI objects). The worker captures
            // shared_ptr<Internals> so it drains safely.
            in->deps_invalid.store(true);
            // Note: rt_worker is still running; we intentionally do NOT join
            // it here. m_int.reset() below releases our ref; the worker keeps
            // its own shared_ptr<Internals> alive until it exits.
        }
    }

    bool batch_request_active = in->batch_requests_in_progress.load(std::memory_order_acquire) > 0;
    // Cancelling an in-progress upload must not cause the flusher to issue a
    // fresh create request before the synchronous do_request call unwinds.
    if (batch_request_active)
        in->stop_uploads.store(true, std::memory_order_release);
    cancel_current(in, SnapLogPolicy::Buffered);

    if (in->bt_worker.joinable()) {
        // Set drain_and_flush so bt_worker drains the remaining bt_queue to
        // active.log, rotates to sealed, uploads, then exits.
        in->drain_and_flush.store(true);
        cancel_current(in, SnapLogPolicy::Buffered); // abort any in-flight PUT

        // Bounded join on bt_worker: same promise/future + detach + pin pattern
        // as rt_worker. Reuses batch_join_deadline_sec.
        auto                       bt_join_promise = std::make_shared<std::promise<void>>();
        auto                       bt_join_future  = bt_join_promise->get_future();
        std::shared_ptr<Internals> bt_in_pin       = in;
        std::thread                bt_joiner([bt_in_pin, bt_join_promise]() {
            if (bt_in_pin->bt_worker.joinable()) {
                bt_in_pin->bt_worker.join();
                bt_join_promise->set_value();
            }
        });
        bt_joiner.detach();

        int bt_deadline_sec = std::min(in->cfg.batch_join_deadline_sec, kShutdownBatchJoinMaxSec);
        if (bt_deadline_sec < 1)
            bt_deadline_sec = 1;
        if (bt_join_future.wait_for(std::chrono::seconds(bt_deadline_sec)) == std::future_status::ready) {
            // bt_worker joined within deadline.
        } else {
            // bt_worker hasn't exited — set deps_invalid and detach. The worker
            // closure captures shared_ptr<Internals> so it drains safely after
            // m_int.reset() without UAF.
            in->deps_invalid.store(true);
            in->batch_deps_invalid.store(true);
        }
    }

    {
        std::lock_guard<std::mutex> state_lock(m_state_mu);
        // A consent-OFF purge may still be waiting for its retired worker. Keep
        // Internals published so that joiner can finish the privacy purge.
        if (m_int == in && !deferred_consent_purge)
            m_int.reset();
    }
}

// Extract the "eventName" value from an ext payload, or "" if absent.
static std::string extract_event_name(const SnapLogExt& ext)
{
    for (const auto& kv : ext) {
        if (kv.first == "eventName")
            return kv.second;
    }
    return {};
}

void SnapLogClient::log(SnapLogLevel lvl, std::string msg, SnapLogExt ext, SnapLogPolicy policy, const char* caller_func, int /*caller_line*/)
{
    auto in = internals();
    // 1. Not init'd → safe no-op.
    if (!in)
        return;

    // 2. Config disabled → no-op (applies to both Realtime and Buffered).
    if (!in->cfg.enabled)
        return;

    // 3. Consent false → zero work (atomic read, no lock).
    if (!in->consent.load(std::memory_order_relaxed))
        return;

    if (in->stop_receiving.load(std::memory_order_relaxed))
        return;

    {
        std::lock_guard<std::mutex> tk(in->token_mu);
        if (in->user_token.empty())
            return;
    }

    // 4. Disabled event name → no-op.
    std::string event_name = extract_event_name(ext);
    for (const auto& blocked : in->cfg.event_disable_list) {
        if (blocked == event_name)
            return;
    }

    // 5. Rate limit check (O(1) token bucket).
    if (!in->rate_limiter.allow(event_name, in->cfg.default_rate_cap_per_sec, in->cfg.total_unknown_rate_cap_per_sec,
                                in->cfg.event_rate_cap_per_sec)) {
        return; // throttled
    }

    // 6. Enqueue under queue_mu. Branch by policy:
    //    - Realtime: rt_queue with Error-protect admission policy.
    //    - Buffered: bt_queue with FIFO-drop-incoming (no Error privilege).
    std::lock_guard<std::mutex> lk(in->queue_mu);

    if (policy == SnapLogPolicy::Buffered) {
        // Batch channel: FIFO-drop-incoming when full. Batch is a disk channel
        // — Error events do NOT get eviction privilege here (unlike realtime).
        const std::size_t bcap = in->cfg.batch_queue_cap;
        if (bcap == 0) {
            // cap-0 → always drop.
            in->batch_queue_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (in->bt_queue.size() >= bcap) {
            // Queue full → drop incoming event, bump counter.
            in->batch_queue_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        in->bt_queue.push_back(RtEvent{lvl, std::move(msg), std::move(ext), normalize_logger(caller_func)});
        return;
    }

    // Realtime channel (existing admission policy with Error-protect eviction).
    const std::size_t cap = in->cfg.realtime_queue_cap;
    if (cap == 0) {
        in->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (in->rt_queue.size() < cap) {
        // Room available — just push.
        in->rt_queue.push_back(RtEvent{lvl, std::move(msg), std::move(ext), normalize_logger(caller_func)});
        return;
    }

    // Queue is full — consult admission policy.
    std::deque<SnapLogLevel> levels;
    for (const auto& e : in->rt_queue)
        levels.push_back(e.level);

    auto admit = decide_realtime_admit(levels, cap, lvl);
    switch (admit.decision) {
    case EvictDecision::EvictOldestNonError:
    case EvictDecision::EvictOldestError:
        // Erase the chosen index, push incoming.
        if (admit.evict_index && *admit.evict_index < in->rt_queue.size()) {
            in->rt_queue.erase(in->rt_queue.begin() + static_cast<std::ptrdiff_t>(*admit.evict_index));
        }
        in->rt_queue.push_back(RtEvent{lvl, std::move(msg), std::move(ext), normalize_logger(caller_func)});
        break;
    case EvictDecision::DropIncoming: in->dropped.fetch_add(1, std::memory_order_relaxed); break;
    case EvictDecision::Admit:
        // Shouldn't happen (queue was full), but handle safely.
        in->rt_queue.push_back(RtEvent{lvl, std::move(msg), std::move(ext), normalize_logger(caller_func)});
        break;
    }
}

void SnapLogClient::set_user_token(std::string t)
{
    auto in = internals();
    if (!in)
        return;
    if (!t.empty()) {
        in->auth_known_dead.store(false, std::memory_order_relaxed);
        in->auth_dead_skip_counted.store(false, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(in->token_mu);
    in->user_token = std::move(t);
}

void SnapLogClient::set_user_id(std::string u)
{
    auto in = internals();
    if (!in)
        return;
    std::lock_guard<std::mutex> lk(in->token_mu);
    in->user_id = std::move(u);
}

void SnapLogClient::set_device_id(std::string d)
{
    auto in = internals();
    if (!in)
        return;
    std::lock_guard<std::mutex> lk(in->token_mu);
    in->device_id = std::move(d);
}

void SnapLogClient::set_connect_clientid(std::string v)
{
    auto in = internals();
    if (!in)
        return;
    std::lock_guard<std::mutex> lk(in->token_mu);
    in->connect_clientid = std::move(v);
}

void SnapLogClient::set_print_sn(std::string v)
{
    auto in = internals();
    if (!in)
        return;
    std::lock_guard<std::mutex> lk(in->token_mu);
    in->print_sn = std::move(v);
}

void SnapLogClient::set_consent(bool ok)
{
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
    auto                        in = internals();
    if (!in)
        return;
    in->consent.store(ok);
    // Both transitions clear the queues (symmetric — avoids OFF-period
    // backlog flushing on ON; avoids stale events lingering after OFF).
    {
        std::lock_guard<std::mutex> lk(in->queue_mu);
        in->rt_queue.clear();
        in->bt_queue.clear();
    }

    if (!ok) {
        // Retire the worker synchronously, but perform its join and the spool
        // purge off the UI thread. A blocked transport can otherwise park the
        // preferences callback for the full batch join deadline.
        in->batch_deps_invalid.store(true, std::memory_order_release);
        in->stop_uploads.store(true, std::memory_order_release);
        in->batch_worker_generation.fetch_add(1, std::memory_order_acq_rel);
        cancel_current(in, SnapLogPolicy::Realtime);
        cancel_current(in, SnapLogPolicy::Buffered);

        std::thread retired_worker;
        if (in->bt_worker.joinable())
            retired_worker = std::move(in->bt_worker);
        begin_spool_purge(in, std::move(retired_worker));
    } else {
        // If a retired worker is still unwinding, the deferred starter waits
        // for the purge; events accepted after this ON transition queue until
        // the replacement owns the spool.
        start_batch_worker_after_purge(in);
    }
}

void SnapLogClient::cancel_current(const std::shared_ptr<Internals>& in, SnapLogPolicy p)
{
    if (!in)
        return;
    std::lock_guard<std::mutex> lk(in->m_h_mu);
    auto&                       h = (p == SnapLogPolicy::Realtime) ? in->rt_handle : in->bt_handle;
    if (h)
        h->cancel();
}

// --- Test-only accessors ---

std::size_t SnapLogClient::realtime_queue_size_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return 0;
    std::lock_guard<std::mutex> lk(m_int->queue_mu);
    return m_int->rt_queue.size();
}

uint64_t SnapLogClient::dropped_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return 0;
    return m_int->dropped.load(std::memory_order_relaxed);
}

std::vector<SnapLogLevel> SnapLogClient::realtime_queue_levels_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return {};
    std::lock_guard<std::mutex> lk(m_int->queue_mu);
    std::vector<SnapLogLevel>   out;
    out.reserve(m_int->rt_queue.size());
    for (const auto& e : m_int->rt_queue)
        out.push_back(e.level);
    return out;
}

void SnapLogClient::set_nonce_generator_for_test(NonceGenerator ng)
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return;
    m_int->nonce_gen_for_test = std::move(ng);
}

std::size_t SnapLogClient::batch_queue_size_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return 0;
    std::lock_guard<std::mutex> lk(m_int->queue_mu);
    return m_int->bt_queue.size();
}

uint64_t SnapLogClient::batch_queue_dropped_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return 0;
    return m_int->batch_queue_dropped.load(std::memory_order_relaxed);
}

bool SnapLogClient::batch_worker_joinable_for_test() const
{
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mu);
    auto                        in = internals();
    return in && in->bt_worker.joinable();
}

bool SnapLogClient::auth_known_dead_for_test() const
{
    std::lock_guard<std::mutex> state_lock(m_state_mu);
    if (!m_int)
        return false;
    return m_int->auth_known_dead.load(std::memory_order_relaxed);
}

void SnapLogClient::set_auth_known_dead_for_test(bool v)
{
    auto in = internals();
    if (!in)
        return;
    in->auth_known_dead.store(v, std::memory_order_relaxed);
    if (!v)
        in->auth_dead_skip_counted.store(false, std::memory_order_relaxed);
}

std::string sanitize_file_name(std::string s)
{
    // Pass 1: remove dangerous chars and control chars, collapse whitespace.
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        // Remove control chars (< 0x20) and the set <>:"/\|?*
        if (uc < 0x20)
            continue;
        switch (c) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            // Replace with space so word boundaries are preserved.
            if (!prev_space && !out.empty()) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        default: break;
        }
        if (c == ' ' || c == '\t') {
            if (!prev_space && !out.empty()) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        out.push_back(c);
        prev_space = false;
    }
    // Trim trailing whitespace.
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();

    // Cap length to <=128. Trim to last whitespace boundary if mid-word.
    if (out.size() > 128) {
        out             = out.substr(0, 128);
        auto last_space = out.find_last_of(' ');
        if (last_space != std::string::npos && last_space >= 64)
            out = out.substr(0, last_space);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
    }

    if (out.empty())
        out = "batch";
    return out;
}

// Fix A: Build a unique upload fileName for the S3 presigned-PUT key.
// Format: "<clientId>/<batchId>.<now_ms>.<seq>.ndjson"
// Each segment sanitized independently; now_ms (epoch milliseconds) + seq
// (per-instance monotonic counter) together guarantee uniqueness across
// retries and concurrent batches, preventing silent S3 overwrites.
std::string make_upload_file_name(const std::string& clientId, const std::string& batchId, int64_t now_ms, uint64_t seq)
{
    return sanitize_file_name(clientId) + "/" + sanitize_file_name(batchId) + "." + std::to_string(now_ms) + "." + std::to_string(seq) +
           ".ndjson";
}

bool append_line(const boost::filesystem::path& active, const std::string& line)
{
    namespace fs = boost::filesystem;
    if (!is_valid_spool_path(active))
        return false;

    boost::system::error_code ec;
    auto                      parent = active.parent_path();
    if (!parent.empty() && !fs::exists(parent, ec)) {
        fs::create_directories(parent, ec);
        // Even if create_directories fails (e.g. race), try to open anyway.
    }
    // Open in append + binary mode. This creates the file if missing.
#ifdef _WIN32
    std::ofstream ofs(active.native(), std::ios::app | std::ios::binary);
#else
    std::ofstream ofs(active.string(), std::ios::app | std::ios::binary);
#endif
    if (!ofs.is_open())
        return false;
    ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (!ofs)
        return false;
    ofs.flush();
    return ofs.good();
}

bool rotate_active(const boost::filesystem::path& spool,
                   boost::filesystem::path&       active_path,
                   const std::string&             batchId,
                   std::string&                   err,
                   BatchClock                     clock)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;

    // No-op if active is absent.
    if (!fs::exists(active_path, ec))
        return true;

    // No-op if active is empty (0 bytes).
    boost::system::error_code sec;
    auto                      sz = fs::file_size(active_path, sec);
    if (sec)
        return true; // can't stat — treat as absent
    if (sz == 0)
        return true;

    // Resolve timestamp.
    int64_t ts_ms;
    if (clock) {
        ts_ms = clock();
    } else {
        ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Build sealed target: batch.<ts_ms>.<batchId>.sealed
    std::string sealed_name = "batch." + std::to_string(ts_ms) + "." + batchId + ".sealed";
    fs::path    sealed_path = spool / sealed_name;

    // Windows: rename fails if target exists. Append numeric suffix to resolve.
    if (fs::exists(sealed_path, ec)) {
        for (int i = 1; i < 1000; ++i) {
            sealed_name = "batch." + std::to_string(ts_ms) + "." + batchId + "." + std::to_string(i) + ".sealed";
            sealed_path = spool / sealed_name;
            if (!fs::exists(sealed_path, ec))
                break;
        }
    }

    // Rename (same-volume, atomic). active_path is NOT modified — it still
    // points at active.log (lazy recreate on next append_line).
    fs::rename(active_path, sealed_path, ec);
    if (ec) {
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

std::vector<boost::filesystem::path> list_sealed(const boost::filesystem::path& spool)
{
    namespace fs = boost::filesystem;
    std::vector<fs::path>     result;
    if (!is_valid_spool_path(spool))
        return result;

    boost::system::error_code ec;
    if (!fs::exists(spool, ec) || !fs::is_directory(spool, ec))
        return result;

    for (fs::directory_iterator it(spool, ec), end; it != end && !ec; it.increment(ec)) {
        auto fn = it->path().filename().string();
        // Match batch.*.sealed
        if (fn.size() > 6 && fn.substr(0, 6) == "batch." && fn.substr(fn.size() - 7) == ".sealed") {
            result.push_back(it->path());
        }
    }
    // Sort by filename — lexicographic order sorts by (ts, batchId, partN).
    std::sort(result.begin(), result.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
    return result;
}

uintmax_t sealed_total_bytes(const boost::filesystem::path& spool, const boost::filesystem::path* exclude_in_flight)
{
    namespace fs     = boost::filesystem;
    uintmax_t total  = 0;
    auto      sealed = list_sealed(spool);
    for (const auto& p : sealed) {
        if (exclude_in_flight && p == *exclude_in_flight)
            continue;
        boost::system::error_code ec;
        auto                      sz = fs::file_size(p, ec);
        if (!ec)
            total += sz;
    }
    return total;
}

//
// Three endpoints (create/completed/cancel), two auth variants each. Auth
// selection mirrors build_realtime_request exactly: token non-empty -> Bearer
// logged-in path; empty -> HMAC public path. The path suffix differs per
// endpoint, so we factor a shared helper that takes the suffix and builds
// URL + headers, then each public function supplies its own JSON body.

namespace {

// Shared auth+URL builder for all three batch endpoints. Mirrors the structure
// of build_realtime_request but returns a BatchRequest (with body left empty
// for the caller to fill) and takes the endpoint path suffix.
BatchRequest build_batch_request_common(const SnapLogDeps&   deps,
                                        const SnapLogConfig& cfg,
                                        const std::string&   clientId,
                                        const std::string&   token,
                                        NonceGenerator       nonce_gen,
                                        const std::string&   path_suffix)
{
    BatchRequest r;

    auto add_common = [&]() {
        r.headers.emplace_back("X-Client-Type", kSnapLogClientType);
        r.headers.emplace_back("X-Client-Id", clientId);
    };

    if (!token.empty()) {
        // Logged-in path: Bearer + the upload endpoint. The log gateway requires
        // the "Bearer " prefix per the API spec (bare token -> 110002 "Missing
        // authorization"); the app's stored token is bare, so prepend it here.
        r.url = cfg.gateway_base + "/api/log/upload/" + path_suffix;
        r.headers.emplace_back("Authorization", std::string("Bearer ") + token);
        add_common();
    } else {
        // Public path: HMAC-signed, public endpoint.
        r.url = cfg.gateway_base + "/api/log/public/upload/" + path_suffix;
        add_common();
        int64_t     ts     = deps.now_ms ? deps.now_ms() : 0;
        std::string ts_str = std::to_string(ts);
        std::string nonce  = nonce_gen ? nonce_gen() : default_nonce();
        std::string sign   = hmac_sha256_hex(cfg.hmac_secret, std::string(kSnapLogClientType) + clientId + ts_str + nonce);
        r.headers.emplace_back("X-Timestamp", ts_str);
        r.headers.emplace_back("X-Nonce", nonce);
        r.headers.emplace_back("X-Sign", sign);
    }
    return r;
}

} // anonymous namespace

BatchRequest build_batch_create_request(const SnapLogDeps&   deps,
                                        const SnapLogConfig& cfg,
                                        const std::string&   clientId,
                                        const std::string&   token,
                                        NonceGenerator       nonce_gen,
                                        const std::string&   fileName)
{
    BatchRequest   r = build_batch_request_common(deps, cfg, clientId, token, nonce_gen, "create");
    nlohmann::json body;
    body["fileName"] = fileName;
    r.body           = body.dump();
    return r;
}

BatchRequest build_batch_complete_request(const SnapLogDeps&   deps,
                                          const SnapLogConfig& cfg,
                                          const std::string&   clientId,
                                          const std::string&   token,
                                          NonceGenerator       nonce_gen,
                                          const std::string&   full_key)
{
    BatchRequest   r = build_batch_request_common(deps, cfg, clientId, token, nonce_gen, "completed");
    nlohmann::json body;
    body["fileName"] = full_key;
    r.body           = body.dump();
    return r;
}

BatchRequest build_batch_cancel_request(const SnapLogDeps&   deps,
                                        const SnapLogConfig& cfg,
                                        const std::string&   clientId,
                                        const std::string&   token,
                                        NonceGenerator       nonce_gen,
                                        const std::string&   uploadId)
{
    BatchRequest   r    = build_batch_request_common(deps, cfg, clientId, token, nonce_gen, "cancel");
    nlohmann::json body = nlohmann::json::object();
    if (!uploadId.empty()) {
        body["uploadId"] = uploadId;
    }
    r.body = body.dump();
    return r;
}

//
// The batch flusher is a single thread that owns all spool file operations
// (append/rotate/upload). It polls (non-blocking), mirroring rt_worker_loop.
// Part A implements drain -> active.log, rotate at thresholds, backpressure
// eviction of oldest sealed, and all exit conditions. Upload is a stub
// (attempt_upload returns false) — Part B fills create -> PUT -> completed.

namespace {

// Generate a fresh batchId: 8 random bytes as 16 lowercase hex chars. Good
// enough for filename uniqueness within a spool dir; not security-sensitive.
std::string generate_batch_id()
{
    unsigned char buf[8];
    // RAND_bytes may fail (extremely rare); fall back to a timestamp mix.
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        for (int i = 0; i < 8; ++i)
            buf[i] = static_cast<unsigned char>((ts >> (i * 4)) & 0xFF);
    }
    static const char* H = "0123456789abcdef";
    std::string        r(16, '0');
    for (int i = 0; i < 8; ++i) {
        r[2 * i]     = H[buf[i] >> 4];
        r[2 * i + 1] = H[buf[i] & 0xF];
    }
    return r;
}

} // anonymous namespace

static bool batch_worker_current(const std::shared_ptr<SnapLogClient::Internals>& in, uint64_t generation)
{ return generation == 0 || in->batch_worker_generation.load(std::memory_order_acquire) == generation; }

static bool batch_deps_unavailable(const SnapLogClient::Internals& in)
{
    // deps_invalid is retained for shutdown/re-init compatibility and for the
    // direct test harness; batch_deps_invalid is the consent-OFF fence and is
    // cleared only when a new worker generation is published.
    return in.deps_invalid.load(std::memory_order_acquire) || in.batch_deps_invalid.load(std::memory_order_acquire);
}

static void reset_batch_upload_state(SnapLogClient::Internals& in)
{
    in.bt_upload_phase = SnapLogClient::Internals::BtUploadPhase::Idle;
    in.bt_in_flight_sealed.clear();
    in.bt_put_url.clear();
    in.bt_put_key.clear();
    in.bt_frozen_token.clear();
    in.bt_frozen_client_id.clear();
    in.bt_frozen_batch_id.clear();
    in.bt_upload_attempt = 0;
}

// Consume a completed request without waiting on a promise that was cancelled
// before the transport callback ran. cancel() fulfills the promise in normal
// operation, but this guard also keeps test/adaptor handles without promises
// from dereferencing null.
static SnapLogResult consume_handle_result(const std::shared_ptr<SnapLogHandle>& h)
{
    if (!h || h->cancelled.load(std::memory_order_acquire))
        return {0, "", true};
    if (!h->prom)
        return {0, "", false};
    return h->prom->get_future().get();
}
// Forward declaration: the retry/cancel path is defined after attempt_upload.
static void handle_bt_retry_or_fail(std::shared_ptr<SnapLogClient::Internals> in, uint64_t generation);

// attempt_upload: Part B sub-state-machine for ONE sealed file.
//
// This is a PER-TICK STEP function: it advances the upload by at most one phase
// per call (issue a request OR consume a completed handle), then returns. The
// bt_worker_loop calls it every poll tick, so an upload naturally spans ticks:
//   tick N   : Idle -> freeze token + issue create -> AwaitCreate
//   tick N+1 : AwaitCreate -> create done: issue PUT -> AwaitPut
//   tick N+2 : AwaitPut -> PUT done: issue completed -> AwaitComplete
//   tick N+3 : AwaitComplete -> completed done: delete sealed -> Idle
// On failure (non-2xx or body code!=200), it takes the cancel+retry path: fire-
// and-forget a cancel request, then either re-create (retry, fresh URL, same
// frozen token) or leave the sealed on disk (at-least-once, retried next
// session). We never block the loop on a handle.
//
// In-flight handle lives in in->bt_handle (guarded by m_h_mu) so
// cancel_current(Buffered) can abort it during shutdown/consent. The phase +
// url/key/token/attempt fields (in->bt_upload_*) are written ONLY by this single
// thread, so they need no mutex — same ownership as active_path.
//
// deps_invalid: when set, we cannot touch Deps (production do_request may touch
// destructed GUI objects). We cancel+clear any in-flight handle and bail
// (sealed stays on disk). The loop keeps draining the queue to active.log with
// cached_context in that case (handled by the caller).
static void attempt_upload(std::shared_ptr<SnapLogClient::Internals> in, uint64_t generation)
{
    namespace fs = boost::filesystem;
    using Phase  = SnapLogClient::Internals::BtUploadPhase;

    if (!batch_worker_current(in, generation))
        return;

    // A shutdown/consent transition that cancelled an in-flight request leaves
    // its sealed file on disk and must not start another upload.
    if (in->stop_uploads.load(std::memory_order_acquire)) {
        if (in->bt_upload_phase != Phase::Idle) {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            if (in->bt_handle)
                in->bt_handle->cancel();
            in->bt_handle.reset();
            reset_batch_upload_state(*in);
        }
        return;
    }

    // If deps are dead, never issue Deps calls. Abort any in-flight handle and
    // leave sealed on disk (next session retries).
    if (batch_deps_unavailable(*in)) {
        if (in->bt_upload_phase != Phase::Idle) {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            if (in->bt_handle)
                in->bt_handle->cancel();
            in->bt_handle.reset();
            reset_batch_upload_state(*in);
        }
        return;
    }

    // auth_known_dead: the server rejected auth on a prior create (401/403).
    // Skip all further create attempts (sealed retained, retried next session
    // after re-login). Count the skip once per auth-dead period.
    if (in->auth_known_dead.load(std::memory_order_relaxed) && in->bt_upload_phase == Phase::Idle) {
        bool already_counted = false;
        if (!in->auth_dead_skip_counted.compare_exchange_strong(already_counted, true, std::memory_order_relaxed)) {
            return;
        }
        in->auth_dead_create_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto issue_request = [&](const std::string& method, const std::string& url, std::vector<std::pair<std::string, std::string>> headers,
                             const fs::path* body_file, const std::string* body_str) -> bool {
        if (!batch_worker_current(in, generation) || in->stop_uploads.load(std::memory_order_acquire) || batch_deps_unavailable(*in) ||
            !in->deps.do_request)
            return false;

        std::shared_ptr<SnapLogHandle> h;
        in->batch_requests_in_progress.fetch_add(1, std::memory_order_acq_rel);
        h = in->deps.do_request(method, url, std::move(headers), body_file, body_str);
        in->batch_requests_in_progress.fetch_sub(1, std::memory_order_acq_rel);
        if (!h)
            return false;

        // Re-check consent and generation before committing the slot, so a
        // racing cancel_current/consent transition cannot miss the handle.
        std::lock_guard<std::mutex> hk(in->m_h_mu);
        if (!batch_worker_current(in, generation) || in->stop_uploads.load(std::memory_order_acquire) || batch_deps_unavailable(*in) ||
            !in->consent.load(std::memory_order_relaxed)) {
            h->cancel();
            return false;
        }
        in->bt_handle = std::move(h);
        return true;
    };

    auto abandon_upload = [&]() { reset_batch_upload_state(*in); };

    switch (in->bt_upload_phase) {
    case Phase::Idle: {
        // Pick the oldest sealed and start an upload session.
        auto sealed = list_sealed(in->spool_dir_resolved);
        if (sealed.empty())
            return;
        fs::path target = sealed.front();

        // Freeze token + clientId ONCE for this sealed's entire upload (create/
        // PUT-n/a/completed/cancel + all retries reuse the snapshot). Do NOT
        // switch Bearer<->HMAC mid-batch.
        {
            std::lock_guard<std::mutex> tk(in->token_mu);
            in->bt_frozen_token = in->user_token;
        }
        std::string client_id = in->machine_id_snapshot;
        if (client_id.empty() && in->deps.machine_id)
            client_id = in->deps.machine_id();
        in->bt_frozen_client_id = client_id;
        in->bt_in_flight_sealed = target;
        in->bt_put_url.clear();
        in->bt_put_key.clear();
        in->bt_upload_attempt = 1;

        // fileName for create: "<clientId>/<batchId>.ndjson", each segment
        // sanitized (sanitize strips '/', so apply it per segment and keep the
        // path separator). The server returns the real S3 key.
        // Derive a batchId from the sealed filename so retries name consistently.
        std::string sealed_fn = target.filename().string(); // batch.<ts>.<batchId>.sealed
        std::string seg_batch = in->current_batch_id;
        { // crude parse: the segment between the 2nd and last '.'.
            auto first_dot = sealed_fn.find('.');
            if (first_dot != std::string::npos) {
                auto second_dot = sealed_fn.find('.', first_dot + 1);
                if (second_dot != std::string::npos) {
                    auto last_dot = sealed_fn.rfind('.');
                    if (last_dot != std::string::npos && last_dot > second_dot) {
                        seg_batch = sealed_fn.substr(second_dot + 1, last_dot - second_dot - 1);
                    }
                }
            }
        }
        in->bt_frozen_batch_id = seg_batch;
        // Fix A: fileName includes now_ms + seq for uniqueness, preventing
        // silent S3 overwrites on retries or concurrent batches.
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        uint64_t    seq       = in->upload_seq.fetch_add(1, std::memory_order_relaxed);
        std::string file_name = make_upload_file_name(in->bt_frozen_client_id, seg_batch, now_ms, seq);

        BatchRequest req = build_batch_create_request(in->deps, in->cfg, in->bt_frozen_client_id, in->bt_frozen_token,
                                                      in->nonce_gen_for_test, file_name);
        if (issue_request("POST", req.url, req.headers, nullptr, &req.body)) {
            in->bt_upload_phase = Phase::AwaitCreate;
        } else {
            // A stale generation must not mutate state owned by its replacement.
            if (!batch_worker_current(in, generation))
                return;
            // Could not issue (no deps, or consent/stop raced). Reset session.
            abandon_upload();
        }
        break;
    }
    case Phase::AwaitCreate: {
        std::shared_ptr<SnapLogHandle> h;
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            h = in->bt_handle;
        }
        if (!h || !h->done())
            return; // still in flight — poll next tick
        // Consume the result.
        SnapLogResult res = consume_handle_result(h);
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            in->bt_handle.reset();
        }
        if (!batch_worker_current(in, generation))
            return;
        if (res.cancelled) {
            abandon_upload();
            return;
        }

        // Success = 2xx AND parsed body code==200.
        bool        ok = false;
        std::string url, key;
        if (res.status >= 200 && res.status < 300) {
            auto j = nlohmann::json::parse(res.body.empty() ? "{}" : res.body, nullptr, false);
            if (!j.is_discarded()) {
                int code = j.value("code", 0);
                if (code == 200) {
                    ok = true;
                    if (j.contains("data") && j["data"].is_object()) {
                        url = j["data"].value("url", std::string{});
                        key = j["data"].value("key", std::string{});
                    }
                }
            }
        }

        if (ok && !url.empty()) {
            in->bt_put_url = url;
            in->bt_put_key = key;
            // PUT the sealed FILE bytes to the presigned URL (no extra auth
            // headers — the URL embeds credentials).
            fs::path sealed_path = in->bt_in_flight_sealed;
            if (issue_request("PUT", url, {}, &sealed_path, nullptr)) {
                in->bt_upload_phase = Phase::AwaitPut;
            } else {
                // Could not issue PUT (consent/stop raced). Fall through to retry path.
                handle_bt_retry_or_fail(in, generation);
            }
        } else if (res.status == 401 || res.status == 403) {
            // Auth dead — terminal. Mark, leave sealed on disk, reset session.
            in->auth_known_dead.store(true);
            in->bt_upload_phase = Phase::Idle;
            in->bt_in_flight_sealed.clear();
            in->bt_upload_attempt = 0;
        } else {
            // Fix C: body code==110004 (token expired/revoked at app level)
            // is also an auth-dead signal even on non-4xx HTTP status.
            bool body_auth_dead = false;
            if (!res.body.empty()) {
                auto bj = nlohmann::json::parse(res.body, nullptr, false);
                if (!bj.is_discarded())
                    body_auth_dead = (bj.value("code", 0) == 110004);
            }
            if (body_auth_dead) {
                in->auth_known_dead.store(true);
                in->bt_upload_phase = Phase::Idle;
                in->bt_in_flight_sealed.clear();
                in->bt_upload_attempt = 0;
            } else {
                handle_bt_retry_or_fail(in, generation);
            }
        }
        break;
    }
    case Phase::AwaitPut: {
        std::shared_ptr<SnapLogHandle> h;
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            h = in->bt_handle;
        }
        if (!h || !h->done())
            return;
        SnapLogResult res = consume_handle_result(h);
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            in->bt_handle.reset();
        }
        if (!batch_worker_current(in, generation))
            return;
        if (res.cancelled) {
            abandon_upload();
            return;
        }

        bool ok = (res.status >= 200 && res.status < 300);
        if (ok) {
            // completed: POST {fileName: key}. Success = 2xx + code==200 -> delete sealed.
            BatchRequest req = build_batch_complete_request(in->deps, in->cfg, in->bt_frozen_client_id, in->bt_frozen_token,
                                                            in->nonce_gen_for_test, in->bt_put_key);
            if (issue_request("POST", req.url, req.headers, nullptr, &req.body)) {
                in->bt_upload_phase = Phase::AwaitComplete;
            } else {
                handle_bt_retry_or_fail(in, generation);
            }
        } else {
            handle_bt_retry_or_fail(in, generation);
        }
        break;
    }
    case Phase::AwaitComplete: {
        std::shared_ptr<SnapLogHandle> h;
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            h = in->bt_handle;
        }
        if (!h || !h->done())
            return;
        SnapLogResult res = consume_handle_result(h);
        {
            std::lock_guard<std::mutex> hk(in->m_h_mu);
            in->bt_handle.reset();
        }
        if (!batch_worker_current(in, generation))
            return;
        if (res.cancelled) {
            abandon_upload();
            return;
        }

        bool ok = false;
        if (res.status >= 200 && res.status < 300) {
            auto j = nlohmann::json::parse(res.body.empty() ? "{}" : res.body, nullptr, false);
            ok     = !j.is_discarded() && (j.value("code", 0) == 200);
        }

        if (ok) {
            // Success: at-least-once complete -> delete the sealed file.
            boost::system::error_code rec;
            fs::remove(in->bt_in_flight_sealed, rec);
            if (!rec)
                in->bt_upload_sealed_deleted.fetch_add(1, std::memory_order_relaxed);
            reset_batch_upload_state(*in);
        } else {
            handle_bt_retry_or_fail(in, generation);
        }
        break;
    }
    }
}

// Retry/cancel path: fire-and-forget a best-effort cancel (we don't wait for it
// — the handle self-cleans via its promise/shared_ptr), then either re-create
// with a fresh presigned URL (same frozen token) or, if retries are exhausted,
// leave the sealed on disk (at-least-once; retried next session).
static void handle_bt_retry_or_fail(std::shared_ptr<SnapLogClient::Internals> in, uint64_t generation)
{
    if (!batch_worker_current(in, generation))
        return;
    if (in->stop_uploads.load(std::memory_order_acquire) || batch_deps_unavailable(*in)) {
        reset_batch_upload_state(*in);
        return;
    }

    // Best-effort cancel (single-part: empty uploadId). Fire-and-forget — ignore
    // its result; the handle cleans itself up.
    if (in->deps.do_request) {
        BatchRequest creq = build_batch_cancel_request(in->deps, in->cfg, in->bt_frozen_client_id, in->bt_frozen_token,
                                                       in->nonce_gen_for_test, /*uploadId*/ "");
        in->batch_requests_in_progress.fetch_add(1, std::memory_order_acq_rel);
        (void) in->deps.do_request("POST", creq.url, creq.headers, nullptr, &creq.body);
        in->batch_requests_in_progress.fetch_sub(1, std::memory_order_acq_rel);
    }

    if (!batch_worker_current(in, generation))
        return;
    if (in->stop_uploads.load(std::memory_order_acquire) || batch_deps_unavailable(*in)) {
        reset_batch_upload_state(*in);
        return;
    }

    int max_retry = in->cfg.batch_max_retry > 0 ? in->cfg.batch_max_retry : 3;
    if (in->bt_upload_attempt < max_retry) {
        in->bt_upload_attempt += 1;
        in->bt_upload_retries.fetch_add(1, std::memory_order_relaxed);
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        uint64_t     seq       = in->upload_seq.fetch_add(1, std::memory_order_relaxed);
        std::string  file_name = make_upload_file_name(in->bt_frozen_client_id, in->bt_frozen_batch_id, now_ms, seq);
        BatchRequest req       = build_batch_create_request(in->deps, in->cfg, in->bt_frozen_client_id, in->bt_frozen_token,
                                                            in->nonce_gen_for_test, file_name);

        bool issued = false;
        if (batch_worker_current(in, generation) && !in->stop_uploads.load(std::memory_order_acquire) && !batch_deps_unavailable(*in) &&
            in->consent.load(std::memory_order_relaxed) && in->deps.do_request) {
            std::shared_ptr<SnapLogHandle> h;
            in->batch_requests_in_progress.fetch_add(1, std::memory_order_acq_rel);
            h = in->deps.do_request("POST", req.url, req.headers, nullptr, &req.body);
            in->batch_requests_in_progress.fetch_sub(1, std::memory_order_acq_rel);
            if (h) {
                std::lock_guard<std::mutex> hk(in->m_h_mu);
                if (batch_worker_current(in, generation) && !in->stop_uploads.load(std::memory_order_acquire) &&
                    !batch_deps_unavailable(*in) && in->consent.load(std::memory_order_relaxed)) {
                    in->bt_handle = std::move(h);
                    issued        = true;
                } else {
                    h->cancel();
                }
            }
        }
        if (!batch_worker_current(in, generation))
            return;
        if (issued) {
            in->bt_upload_phase = SnapLogClient::Internals::BtUploadPhase::AwaitCreate;
        } else {
            in->bt_upload_sealed_retained.fetch_add(1, std::memory_order_relaxed);
            reset_batch_upload_state(*in);
        }
    } else {
        in->bt_upload_sealed_retained.fetch_add(1, std::memory_order_relaxed);
        reset_batch_upload_state(*in);
    }
}
void SnapLogClient::bt_worker_loop(std::shared_ptr<Internals> in, uint64_t generation)
{
    if (!in)
        return;
    using namespace std::chrono_literals;
    namespace fs = boost::filesystem;

    while (true) {
        if (!batch_worker_current(in, generation))
            return;

        // 0. Backpressure: if sealed total > sealed_max_disk_mb, delete the
        //    OLDEST NON-in-flight sealed. This runs BEFORE exit checks so a
        //    maintenance tick (empty queue, stop_receiving) still trims over-cap
        //    sealed files. The in-flight sealed (currently being uploaded) is
        //    EXCLUDED so we never evict a file mid-upload.
        {
            // Exclude the in-flight sealed when an upload is in progress.
            const boost::filesystem::path* excl = nullptr;
            boost::filesystem::path        in_flight;
            if (in->bt_upload_phase != Internals::BtUploadPhase::Idle && !in->bt_in_flight_sealed.empty()) {
                in_flight = in->bt_in_flight_sealed;
                excl      = &in_flight;
            }
            uintmax_t total = sealed_total_bytes(in->spool_dir_resolved, excl);
            if (total > 0) {
                size_t cap_bytes = (in->cfg.sealed_max_disk_mb > 0 ? in->cfg.sealed_max_disk_mb : 256) * 1024 * 1024;
                if (total > cap_bytes) {
                    // list_sealed is sorted by filename (ts dominates), so the
                    // first entry that ISN'T the in-flight sealed is the oldest
                    // evictable one.
                    auto sealed = list_sealed(in->spool_dir_resolved);
                    for (const auto& p : sealed) {
                        if (excl && p == *excl)
                            continue; // never evict in-flight
                        boost::system::error_code rec;
                        fs::remove(p, rec);
                        if (!rec) {
                            in->sealed_evicted.fetch_add(1, std::memory_order_relaxed);
                        }
                        break; // evict one per tick
                    }
                }
            }
        }

        // 1. Exit conditions (lock-free atomics).
        //    consent-false -> exit (B6 will purge spool + restart on ON).
        if (!in->consent.load(std::memory_order_relaxed))
            return;
        //    NOTE: deps_invalid does NOT exit the batch flusher (unlike the
        //    realtime worker). Per spec §4.6, the batch flusher instead keeps
        //    draining the queue to active.log using cached_context (no Deps
        //    calls) and skips uploads (attempt_upload is a no-op under
        //    deps_invalid). This lets a detached flusher after a shutdown-timeout
        //    persist queued events to disk without touching destructed GUI
        //    objects. We DO exit, though, once the queue is drained AND nothing
        //    is in flight — there's nothing more to do without Deps.
        if (batch_deps_unavailable(*in)) {
            std::size_t qsize;
            {
                std::lock_guard<std::mutex> qk(in->queue_mu);
                qsize = in->bt_queue.size();
            }
            bool no_inflight = true;
            {
                std::lock_guard<std::mutex> hk(in->m_h_mu);
                no_inflight = (in->bt_handle == nullptr);
            }
            if (qsize == 0 && no_inflight)
                return;
        }
        //    stop_receiving && !drain_and_flush && queue empty -> exit.
        if (in->stop_receiving.load(std::memory_order_relaxed) && !in->drain_and_flush.load(std::memory_order_relaxed)) {
            std::size_t qsize;
            {
                std::lock_guard<std::mutex> qk(in->queue_mu);
                qsize = in->bt_queue.size();
            }
            if (qsize == 0)
                return;
        }
        //    drain_and_flush && queue empty && no in-flight && (no sealed || auth_dead) -> exit.
        if (in->drain_and_flush.load(std::memory_order_relaxed)) {
            std::size_t qsize;
            {
                std::lock_guard<std::mutex> qk(in->queue_mu);
                qsize = in->bt_queue.size();
            }
            bool no_inflight = true;
            {
                std::lock_guard<std::mutex> hk(in->m_h_mu);
                no_inflight = (in->bt_handle == nullptr);
            }
            if (qsize == 0 && no_inflight) {
                auto sealed = list_sealed(in->spool_dir_resolved);
                if (sealed.empty() || in->auth_known_dead.load(std::memory_order_relaxed) ||
                    in->stop_uploads.load(std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        // 2. Bounded drain: pop up to drain_batch_per_tick lines, respecting
        //    the wall-time budget (checked AFTER each line).
        size_t per_tick       = in->cfg.drain_batch_per_tick > 0 ? in->cfg.drain_batch_per_tick : 256;
        int    wall_budget_ms = in->cfg.drain_wall_budget_ms > 0 ? in->cfg.drain_wall_budget_ms : 20;
        auto   drain_start    = std::chrono::steady_clock::now();

        for (size_t processed = 0; processed < per_tick; ++processed) {
            if (!batch_worker_current(in, generation))
                return;
            // Pop one event under queue_mu.
            std::optional<RtEvent> ev_opt;
            {
                std::lock_guard<std::mutex> qk(in->queue_mu);
                if (!in->bt_queue.empty()) {
                    ev_opt = std::move(in->bt_queue.front());
                    in->bt_queue.pop_front();
                }
            }
            if (!ev_opt.has_value())
                break; // queue drained
            const RtEvent& ev = *ev_opt;

            // Build the per-event context. If deps are valid, (re)build the
            // cached context from Deps/Internals and cache it. If deps_invalid
            // is set, reuse the cache; if no cache yet, skip + count.
            BatchLineContext ctx;
            bool             have_ctx = false;
            if (!batch_deps_unavailable(*in)) {
                // (Re)populate the cached context from live Deps/Internals.
                BatchLineContext& c = in->cached_batch_context;
                c.clientId          = in->machine_id_snapshot;
                if (c.clientId.empty() && in->deps.machine_id) {
                    c.clientId = in->deps.machine_id();
                }
                {
                    std::lock_guard<std::mutex> tk(in->token_mu);
                    c.nodeId           = in->user_id;
                    c.userId           = in->user_id;
                    c.deviceId         = in->device_id;
                    c.connect_clientid = in->connect_clientid;
                    c.print_sn         = in->print_sn;
                    // Publish batch id for rt_worker (current_batch_id is
                    // bt_worker-owned; this snapshot is the cross-thread copy).
                    in->current_batch_id_snapshot = in->current_batch_id;
                }
                c.service                = "snapmaker-desktop";
                c.clientType             = kSnapLogClientType;
                c.processId              = in->cfg.process_id;
                c.appVersion             = in->cfg.app_version;
                c.appBuild               = in->cfg.app_build;
                c.platform               = in->cfg.platform;
                c.osVersion              = in->cfg.os_version;
                c.sessionId              = in->cfg.session_id;
                c.region                 = in->cfg.region;
                c.batchId                = in->current_batch_id;
                c.home_for_redact        = in->cfg.home_for_redact;
                c.thread                 = "bt-worker";
                in->cached_context_valid = true;
            }
            if (in->cached_context_valid) {
                ctx = in->cached_batch_context;
                // Per-event fields.
                ctx.ts_ms   = in->deps.now_ms ? in->deps.now_ms() : 0;
                ctx.batchId = in->current_batch_id;
                ctx.logger  = ev.logger;
                // Extract opId + eventName from the ext payload.
                for (const auto& kv : ev.ext) {
                    if (kv.first == "opId")
                        ctx.opId = kv.second;
                    else if (kv.first == "eventName")
                        ctx.eventName = kv.second;
                }
                have_ctx = true;
            } else {
                // deps_invalid && no cache yet — skip the event, bump counter.
                in->deps_invalid_render_skipped.fetch_add(1, std::memory_order_relaxed);
            }

            if (have_ctx) {
                if (!batch_worker_current(in, generation))
                    return;
                std::string line = render_batch_line(ev.level, ev.message, ev.ext, in->cfg.line_max_bytes, ctx);
                if (!append_line(in->active_path, line)) {
                    in->disk_write_failed.fetch_add(1, std::memory_order_relaxed);
                    // Force a rotate on write failure (might recover on a fresh file).
                    std::string err;
                    rotate_active(in->spool_dir_resolved, in->active_path, in->current_batch_id, err);
                    in->current_batch_id = generate_batch_id();
                    in->events_in_active = 0;
                } else {
                    ++in->events_in_active;
                }
            }

            // Check wall budget AFTER each line.
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drain_start).count();
            if (elapsed >= wall_budget_ms)
                break;
        }

        if (!batch_worker_current(in, generation))
            return;

        // 3. Rotate: if active.log exceeds active_max_bytes OR events-since-
        //    rotate >= batch_max_events OR active bytes >= batch_max_bytes.
        namespace fs = boost::filesystem;
        if (!in->active_path.empty()) {
            boost::system::error_code sec;
            auto                      active_sz = fs::file_size(in->active_path, sec);
            if (sec)
                active_sz = 0;
            size_t max_bytes  = in->cfg.batch_max_bytes > 0 ? in->cfg.batch_max_bytes : (size_t(1) << 20);
            size_t active_cap = in->cfg.active_max_bytes > 0 ? in->cfg.active_max_bytes : max_bytes;
            // Use wall time for periodic flushes; the injected clock may advance
            // at a different rate in tests.
            int64_t now_ms_flush =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t flush_interval_ms = (in->cfg.batch_flush_sec > 0) ? static_cast<int64_t>(in->cfg.batch_flush_sec) * 1000 : 0;
            bool    time_to_flush     = (flush_interval_ms > 0) && (in->last_flush_ms > 0) && (in->events_in_active > 0) &&
                                        (now_ms_flush - in->last_flush_ms >= flush_interval_ms);
            bool rotate_needed = (active_sz >= static_cast<uintmax_t>(active_cap)) || (in->events_in_active >= in->cfg.batch_max_events) ||
                                 (active_sz >= static_cast<uintmax_t>(max_bytes)) || time_to_flush;
            if (rotate_needed) {
                std::string err;
                if (rotate_active(in->spool_dir_resolved, in->active_path, in->current_batch_id, err)) {
                    in->current_batch_id = generate_batch_id();
                    in->events_in_active = 0;
                    in->last_flush_ms    = now_ms_flush;
                }
            }
        }

        // 4. Upload: advance the per-sealed sub-state-machine by one phase (or
        //    poll the in-flight handle). No-op under deps_invalid / auth_known_dead.
        attempt_upload(in, generation);

        // 5. Sleep for the poll interval.
        std::this_thread::sleep_for(std::chrono::milliseconds(in->cfg.poll_interval_ms > 0 ? in->cfg.poll_interval_ms : 1));
    }
}

std::shared_ptr<SnapLogClient::Internals> SnapLogClient::make_internals_for_test(SnapLogDeps deps, SnapLogConfig cfg)
{ return std::make_shared<Internals>(std::move(deps), std::move(cfg)); }

// Production do_request adapter factory: returns a std::function suitable for
// assignment to SnapLogDeps::do_request. The returned function builds a real
// Slic3r::Http request and runs it asynchronously. The returned handle owns the
// Http::Ptr (keeping it alive for the duration of the detached curl thread) and
// a promise that is fulfilled exactly once via exchange on the handle's
// `fulfilled` atomic.
//
// `cfg` is captured by value so the closure is self-contained.
std::function<std::shared_ptr<SnapLogHandle>(const std::string&,
                                             const std::string&,
                                             std::vector<std::pair<std::string, std::string>>,
                                             const boost::filesystem::path*,
                                             const std::string*)>
make_production_do_request(SnapLogConfig cfg)
{
    return [cfg](const std::string& method, const std::string& url, std::vector<std::pair<std::string, std::string>> headers,
                 const boost::filesystem::path* body_file, const std::string* body_str) -> std::shared_ptr<SnapLogHandle> {
        Slic3r::Http http = (method == "PUT") ? Slic3r::Http::put(url) : Slic3r::Http::post(url);

        // Apply timeouts (clamp total >= 1s; connect may be 0 for "no limit").
        http.timeout_connect(cfg.http_connect_timeout_sec > 0 ? cfg.http_connect_timeout_sec : 5);
        long total_sec = cfg.http_total_timeout_sec > 0 ? cfg.http_total_timeout_sec : 1;
        if (total_sec < 1)
            total_sec = 1;
        http.timeout_max(total_sec);

        // Apply caller-supplied headers.
        for (const auto& kv : headers) {
            http.header(kv.first, kv.second);
        }
        // Ensure Content-Type for POST with a string body (realtime JSON events).
        if (method != "PUT" && body_str && !body_str->empty()) {
            bool has_ct = false;
            for (const auto& kv : headers) {
                std::string k = kv.first;
                std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return std::tolower(c); });
                if (k == "content-type") {
                    has_ct = true;
                    break;
                }
            }
            if (!has_ct)
                http.header("Content-Type", "application/json");
        }

        // Set body: file for PUT (streaming), string for POST.
        if (body_file) {
            http.set_put_body(*body_file);
        } else if (body_str) {
            http.set_post_body(*body_str);
        }

        // Create the handle and wire callbacks with exchange so the promise is
        // set exactly once (Http may call on_complete then on_error for 4xx, or
        // neither if the request never issues).
        auto h    = std::make_shared<SnapLogHandle>();
        h->prom   = std::make_shared<std::promise<SnapLogResult>>();
        auto prom = h->prom;
        // weak_ptr breaks the Handle -> Http -> callback -> Handle cycle. The
 // io_thread inside Http::perform() keeps the Http object alive, so the
 // callbacks can safely observe an expired handle after the worker resets it.
        auto hweak = std::weak_ptr<SnapLogHandle>(h);
        http.on_complete([prom, hweak](std::string body, unsigned http_status) {
            if (auto hcap = hweak.lock()) {
                if (!hcap->fulfilled.exchange(true)) {
                    prom->set_value({static_cast<int>(http_status), std::move(body), false});
                }
            }
        });
        http.on_error([prom, hweak](std::string body, std::string /*err*/, unsigned http_status) {
            if (auto hcap = hweak.lock()) {
                if (!hcap->fulfilled.exchange(true)) {
                    prom->set_value({static_cast<int>(http_status), std::move(body), false});
                }
            }
        });

        // perform() returns the Http::Ptr and spawns a background thread.
        h->http = http.perform();
        return h;
    };
}

namespace {

std::string utf8_safe_prefix(const std::string& value, std::size_t budget)
{
    if (budget > value.size())
        budget = value.size();
    std::string out;
    out.reserve(budget);
    std::size_t i = 0;
    while (i < value.size()) {
        const auto  first = static_cast<unsigned char>(value[i]);
        std::size_t len   = 0;
        if (first < 0x80) {
            len = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            len = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            len = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            len = 4;
        } else {
            if (out.size() + 1 <= budget)
                out.push_back('?');
            ++i;
            continue;
        }

        bool valid = i + len <= value.size();
        for (std::size_t j = 1; valid && j < len; ++j) {
            const auto cont = static_cast<unsigned char>(value[i + j]);
            if (cont < 0x80 || cont > 0xBF)
                valid = false;
        }
        if (valid && first == 0xE0 && static_cast<unsigned char>(value[i + 1]) < 0xA0)
            valid = false;
        if (valid && first == 0xED && static_cast<unsigned char>(value[i + 1]) > 0x9F)
            valid = false;
        if (valid && first == 0xF0 && static_cast<unsigned char>(value[i + 1]) < 0x90)
            valid = false;
        if (valid && first == 0xF4 && static_cast<unsigned char>(value[i + 1]) > 0x8F)
            valid = false;

        if (!valid) {
            if (out.size() + 1 <= budget)
                out.push_back('?');
            ++i;
            continue;
        }
        if (out.size() + len > budget)
            break;
        out.append(value, i, len);
        i += len;
    }
    return out;
}

std::string redact_user_paths_in_text(const std::string& value)
{
    std::string result  = value;
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<std::string> markers     = {"/users/", "\\users/", "/home/", "\\home/"};
    std::size_t                    search_from = 0;
    while (search_from < lowered.size()) {
        std::size_t marker_at   = std::string::npos;
        std::size_t marker_size = 0;
        for (const auto& marker : markers) {
            auto at = lowered.find(marker, search_from);
            if (at < marker_at) {
                marker_at   = at;
                marker_size = marker.size();
            }
        }
        if (marker_at == std::string::npos)
            break;

        std::size_t start = marker_at;
        if (start >= 2 && lowered[start - 2] == ':')
            start -= 2;
        std::size_t end = marker_at + marker_size;
        while (end < value.size()) {
            unsigned char c = static_cast<unsigned char>(value[end]);
            if (std::isspace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*') {
                break;
            }
            ++end;
        }
        result.replace(start, end - start, "~");
        lowered.replace(start, end - start, "~");
        search_from = start + 1;
    }
    return result;
}

std::string sanitize_message(const std::string& home, const std::string& message)
{
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> secret_markers     = {"akia",     "eyj",    "ghp_",  "begin ",       "@",
                                                                "password", "secret", "token", "authorization"};
    bool                                  may_contain_secret = false;
    for (const auto& marker : secret_markers) {
        if (lowered.find(marker) != std::string::npos) {
            may_contain_secret = true;
            break;
        }
    }

    std::string result = may_contain_secret ? mask_secret_in_value(message) : message;
    if (!home.empty() && (lowered.find("/users/") != std::string::npos || lowered.find("\\users/") != std::string::npos ||
                          lowered.find("/home/") != std::string::npos || lowered.find("\\home/") != std::string::npos)) {
        result = redact_user_paths_in_text(result);
    }
    return utf8_safe_prefix(result, result.size());
}
} // anonymous namespace
}}} // namespace Slic3r::SnapLog::v1
