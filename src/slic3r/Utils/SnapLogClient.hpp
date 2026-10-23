#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "Http.hpp"
#include "SnapLogEvents.hpp"
namespace Slic3r { namespace SnapLog { inline namespace v1 {

struct SnapLogConfig;

using SnapLogExt                           = std::vector<std::pair<std::string, std::string>>;
inline constexpr char kSnapLogClientType[] = "Orca";

inline constexpr std::string_view kSnapLogUploadCnUrl     = "https://api.snapmaker.cn";
inline constexpr std::string_view kSnapLogUploadIntlUrl   = "https://api.snapmaker.com";
inline constexpr std::string_view kSnapLogPostHogCnUrl     = "https://ph.snapmaker.cn";
inline constexpr std::string_view kSnapLogPostHogIntlUrl   = "https://us.i.posthog.com";
inline constexpr std::string_view kSnapLogPostHogBatchPath = "/batch/";

inline const char* level_str(SnapLogLevel l)
{
    switch (l) {
    case SnapLogLevel::Info: return "INFO";
    case SnapLogLevel::Warning: return "WARN";
    default: return "ERROR";
    }
}

// Strip a user home prefix from `p`. If `home` matches `p` (case- and
// slash-insensitive), the prefix is replaced with "~". As a fallback, a regex
// also collapses the common (/Users|/home)/<name> and C:\Users\<name> patterns
// to "~/". If nothing matches, `p` is returned unchanged. Pure; no FS access.
std::string redact_path(const std::string& home, const std::string& p);

// Value-level secret denylist. Scans a single string value (NOT serialized JSON)
// and replaces PEM headers, AWS AKIA keys, JWTs, GitHub tokens,
// password=/secret=/token=/authorization:/refresh_token=<value>, and email
// addresses with "***". Returns a new sanitized string. Pure.
std::string mask_secret_in_value(std::string v);

// PII one-way hash: first 16 hex chars of sha256 over (clientId, serial).
// Uses the hmac_sha256_hex primitive purely for its sha256 core — clientId is
// passed as the key only to reuse the existing implementation. The output is a
// stable, deterministic 16-char hex digest suitable for correlation without
// leaking the original serial. Pure.
std::string hash_pii(const std::string& clientId, const std::string& serial);

// Context struct for batch line rendering. Carries the full per-event identity
// and metadata that the batch (file-upload) channel needs on each NDJSON line.
// The renderer is the privacy enforcement point: it drops
// raw* keys, masks secrets, redacts paths, and hashes PII.
struct BatchLineContext
{
    std::string clientId;
    std::string userId;
    // Flutter-supplied MQTT identity of the connected printer (always emitted
    // into ext, empty when nothing is connected).
    std::string connect_clientid;
    std::string print_sn;
    std::string service    = "snapmaker-desktop";
    std::string clientType = kSnapLogClientType;
    std::string processId;
    std::string appVersion;
    std::string osVersion;
    std::string profileVersion;
    std::string batchId;
    std::string eventName;
    int64_t     ts_ms = 0;
    std::string home_for_redact;
};

// Output destinations for batch logging. The values are a stable bitmask so
// callers can express one sink or combine sinks with bitwise OR.
enum SnapLogSinks : unsigned int {
    SinkNone    = 0,
    SinkSnapLog = 1u << 0,
    SinkPostHog = 1u << 1,
};

// ext contains clientId (per [DOC-3]) + caller-supplied event keys.
// Privacy policy: drop ext keys starting with "raw"
// (case-insensitive); pass all string values through mask_secret_in_value;
// path-like values through redact_path(ctx.home_for_redact, ...); PII keys
// like printerSerial through hash_pii(ctx.clientId, ...). Enforce the
// line_max_bytes cap by truncating message/ext and appending "…[truncated]".
// Always valid JSON + trailing '\n'.
std::string render_batch_line(
    SnapLogLevel lvl, const std::string& msg, const SnapLogExt& ext, std::size_t line_max_bytes, const BatchLineContext& ctx);

// A PostHog capture event snapshot. Rendering happens on the producer thread:
// identity, timestamp, and all property values are fixed before queueing.
struct PostHogEvent
{
    SnapLogLevel     level = SnapLogLevel::Info;
    std::string      message;
    SnapLogExt       ext;
    BatchLineContext context;
};

std::string render_posthog_event(const PostHogEvent& event);

struct PostHogBatchRequest
{
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body;
};

struct PostHogProjectConfig
{
    std::string base = std::string{kSnapLogPostHogCnUrl};
    std::string api_key;
    bool        enabled = true;
};

struct PostHogProjectQueue
{
    std::deque<std::string> events;
    std::size_t             bytes = 0;
};

PostHogBatchRequest build_posthog_batch_request(const PostHogProjectConfig& project, const std::vector<std::string>& event_lines);

void apply_production_snaplog_site(SnapLogConfig& config, std::string_view country_code);

// Clock callback for rotate_active timestamp injection (testability). Returns
// milliseconds-since-epoch. Default (nullptr) uses the wall clock.
using BatchClock = std::function<int64_t()>;

// Sanitize a string for safe use as a filename component. Strips the chars
// <>:"/\|?* and all control chars (< 0x20), collapses runs of whitespace to a
// single space, trims leading/trailing whitespace, and caps the length to
// <=128 chars (trimmed to the last whitespace boundary if the cap falls mid-
// word). Returns "batch" if the result would be empty.
std::string sanitize_file_name(std::string s);

// Append a line to the active spool file (created lazily). Opens in append+
// binary mode, writes `line`, flushes. Creates the parent directory if it
// doesn't exist. Returns false on open/write failure (caller increments
// disk_write_failed). Never throws.
bool append_line(const boost::filesystem::path& active, const std::string& line);

// Rotate (seal) the current active file. If `active_path` exists and is
// non-empty, it is renamed (same-volume, atomic) to
// "batch.<ts_ms>.<batchId>.sealed" inside `spool`. `active_path` is NOT
// modified — it stays pointing at the active.log path (lazy recreate on next
// append). If `active_path` is absent or empty, this is a no-op (returns
// true). On Windows, if the target sealed name already exists, a numeric
// suffix is appended to avoid the rename-fails-on-existing-target behavior.
// Returns false (with err set) only if the rename fails. Never throws.
bool rotate_active(const boost::filesystem::path& spool,
                   boost::filesystem::path&       active_path,
                   const std::string&             batchId,
                   std::string&                   err,
                   BatchClock                     clock = nullptr);

// List all "batch.*.sealed" files in `spool`, sorted by filename (which sorts
// by (ts, batchId, partN) given the structured naming). Never throws.
std::vector<boost::filesystem::path> list_sealed(const boost::filesystem::path& spool);

// Sum the byte sizes of all sealed files in `spool`, excluding the one pointed
// to by `exclude_in_flight` (may be nullptr to count all). Never throws.
uintmax_t sealed_total_bytes(const boost::filesystem::path& spool, const boost::filesystem::path* exclude_in_flight);

// Per-eventName token bucket that caps outbound event volume so a chatty
// module can't starve critical events. allow() is safe to call from producer
// threads; the mutex keeps bucket refill/consumption a single atomic operation.
//
// Buckets:
//   - Each registered eventName gets its configured cap from `per_name_caps`.
//   - Each unregistered eventName gets a bucket sized to `default_cap`.
//   - A second shared "unknown-aggregate" bucket (sized to `total_unknown_cap`)
//     caps the *aggregate* of all unregistered-name traffic, so a caller
//     emitting N distinct unregistered names can't flood at N*default.
//
// Refill: token-bucket; on each call, tokens = min(cap, tokens + elapsed_sec
// * cap) where elapsed_sec is measured against the injected clock.
//
struct RateLimiter
{
    using Clock = std::function<int64_t()>;
    explicit RateLimiter(Clock now_ms) : now_ms_(std::move(now_ms)) {}
    bool allow(const std::string& eventName, int default_cap, int total_unknown_cap, const std::map<std::string, int>& per_name_caps);

private:
    // One bucket per eventName (registered OR unregistered). tokens_ is the
    // current available count; cap_ is the max (== the configured per-name cap
    // or default_cap). last_ms_ is the injected-clock time of the last refill.
    struct Bucket
    {
        double  tokens      = 0.0;
        int64_t last_ms     = 0;
        bool    initialized = false;
    };
    Clock                         now_ms_;
    std::mutex                    mu_;
    std::map<std::string, Bucket> name_buckets_;
    Bucket                        unknown_aggregate_; // shared across all unregistered
};

// Result of a completed HTTP request. status=0 means a transport-level failure
// (no HTTP status available); cancelled=true means the client cancelled it.
struct SnapLogResult
{
    int         status = 0;
    std::string body;
    bool        cancelled = false;
};

// Async request handle. In test fakes `http` is null; in production the adapter
// creates a real Http. The promise is shared between the handle and
// the Http callback lambda (on the detached io_thread), so a detached curl
// thread self-cleans without UAF on SnapLogClient destruction.
struct SnapLogHandle
{
    Slic3r::Http::Ptr                            http; // null in test fakes
    std::shared_ptr<std::promise<SnapLogResult>> prom;
    std::atomic<bool>                            fulfilled{false};
    std::atomic<bool>                            cancelled{false};
    std::atomic<int>                             completion_status{-1};

    // Worker/flusher polls this (success/failure/cancel -> fulfilled).
    bool done() const { return fulfilled.load(std::memory_order_acquire) || cancelled.load(std::memory_order_acquire); }

    // Complete the promise before aborting curl so consumers never observe a
    // completed handle backed by an unfulfilled future. The Http callbacks use
    // the same fulfilled.exchange() guard, making completion exactly-once.
    void cancel()
    {
        cancelled.store(true, std::memory_order_release);
        if (!fulfilled.exchange(true, std::memory_order_acq_rel) && prom) {
            prom->set_value({0, "", true});
        }
        if (http)
            http->cancel();
    }
};

// Dependency injection: Http creation + identity/consent callbacks.
// All callbacks MUST be self-contained (capture by value or read atomics);
// never capture pointers to destructible GUI objects (see spec §4.6).
struct SnapLogDeps
{
    std::function<std::shared_ptr<SnapLogHandle>(const std::string&                               method,
                                                 const std::string&                               url,
                                                 std::vector<std::pair<std::string, std::string>> headers,
                                                 const boost::filesystem::path* body_file, // batch PUT path; nullptr for JSON POST
                                                 const std::string*             body_str)>
                                 do_request; // asynchronous HTTP adapter
    std::function<bool()>        consent_ok;
    std::function<std::string()> machine_id;
    std::function<int64_t()>     now_ms;
};

struct SnapLogConfig
{
    std::string                                    snaplog_base    = std::string{kSnapLogUploadIntlUrl};
    bool                                           posthog_enabled = true;
    std::map<PostHogProject, PostHogProjectConfig> posthog_projects{{PostHogProject::CnOrca, {}}};
    std::string                                    spool_dir; // batch spool directory; empty disables the disk-upload worker
    bool                                           enabled = true;

    // Populated by the GUI wiring layer (B7) at init. Defaults are empty; the
    // renderer treats empty strings as "field omitted" gracefully.
    std::string app_version;     // e.g. "2.5.0"
    std::string profile_version; // installed Snapmaker system profile bundle version
    std::string os_version;      // e.g. "10.0.19045"
    std::string process_id;      // OS process id used to distinguish local multi-instance runs
    std::string home_for_redact; // user home path for redact_path() on ext values

    std::map<std::string, int> event_rate_cap_per_sec;
    int                        default_rate_cap_per_sec       = 5;
    int                        total_unknown_rate_cap_per_sec = 20;
    std::vector<std::string>   event_disable_list;

    size_t batch_queue_cap = 4096;

    // Batch spool, rotation, retry, and upload fields.
    size_t batch_max_events   = 500;
    size_t batch_max_bytes    = 1 * 1024 * 1024;
    size_t active_max_bytes   = 2 * 1024 * 1024;
    size_t line_max_bytes     = 64 * 1024;
    int    batch_flush_sec    = 60;
    int    batch_max_retry    = 3;
    size_t sealed_max_disk_mb = 256;

    int    http_connect_timeout_sec = 5;
    int    http_total_timeout_sec   = 30;
    int    poll_interval_ms         = 50;
    size_t drain_batch_per_tick     = 256;
    int    drain_wall_budget_ms     = 20;
    int    batch_join_deadline_sec  = 30;

    size_t posthog_queue_cap              = 1024;
    size_t posthog_event_max_bytes        = 64 * 1024;
    size_t posthog_queue_max_bytes        = 1 * 1024 * 1024;
    size_t posthog_global_queue_max_bytes = 2 * 1024 * 1024;
    size_t posthog_batch_size             = 50;
    size_t posthog_batch_max_bytes        = 1 * 1024 * 1024;
    int    posthog_flush_sec              = 60;
};

// Meyers singleton. init() must be called before log(); un-init log() is a
// safe no-op. shutdown() stops workers; between shutdown() and a
// subsequent init(), log() is a no-op.
//
class SnapLogClient
{
public:
    static SnapLogClient& instance();
    void                  init(SnapLogDeps deps, SnapLogConfig cfg);
    void                  shutdown();
    void                  log(SnapLogLevel lvl, std::string msg, SnapLogExt ext, unsigned sinks);
    void                  log_event(SnapLogLevel level, SnapEvent event);
    void                  log_event(SnapLogLevel level, SnapEvent event, SnapLogExt ext);
    void                  log_event(SnapLogLevel level, SnapEvent event, const std::string& msg);
    void                  log_event(SnapLogLevel level, SnapEvent event, SnapLogExt ext, const std::string& msg);
    void                  set_user_token(std::string t);
    void                  set_user_id(std::string u);
    // Flutter MQTT identity of the connected printer (pushed from
    // sw_mqtt_set_engine; cleared on disconnect/logout).
    void set_connect_clientid(std::string v);
    void set_print_sn(std::string v);
    void set_consent(bool ok);

    // Test-only accessors (no-op when un-init'd). Suffix _for_test marks them
    // as test scaffolding; production code must not rely on them.
    std::size_t batch_queue_size_for_test() const;    // bt_queue size under queue_mu
    uint64_t    batch_queue_dropped_for_test() const; // batch_queue_dropped atomic
    std::size_t posthog_queue_size_for_test() const;
    std::size_t posthog_queue_size_for_test(PostHogProject project) const;
    std::size_t posthog_queue_bytes_for_test(PostHogProject project) const;
    uint64_t    posthog_queue_dropped_for_test() const;
    void        set_posthog_timer_expired_for_test();
    uint64_t    posthog_upload_successes_for_test() const;
    uint64_t    posthog_upload_failures_for_test() const;

    // Used to verify that a timed-out consent-OFF leaves bt_worker non-joinable
    // (so OFF->ON restart works). Also used to verify file-lock defers bt_worker.
    // (so OFF->ON can restart it). False when un-init'd.
    bool batch_worker_joinable_for_test() const;

    // setter forces it (drives the re-login-clears path deterministically without
    // running the 401 flusher). No-op / false when un-init'd.
    bool auth_known_dead_for_test() const;
    void set_auth_known_dead_for_test(bool v);

    //
    // bt_worker_loop drains bt_queue -> active.log, rotates to sealed at
    // thresholds, evicts oldest sealed under backpressure, and calls
    // attempt_upload (create/PUT/completed). It is a single-threaded owner of
    // all spool FS operations.
    //
    // Internals is exposed publicly ONLY so tests can construct a fake one
    // and drive bt_worker_loop without going through the singleton. Production
    // code must never construct Internals directly.
    //
    // Stored batch event.
    struct LogEvent
    {
        SnapLogLevel level;
        std::string  message;
        SnapLogExt   ext;
    };

    // Cross-platform exclusive file lock for spool_dir (prevents two OrcaSlicer
    // processes from writing the same spool simultaneously).
    // Win: _sopen_s(_SH_DENYRW). POSIX: flock(LOCK_EX|LOCK_NB). Auto-released
    // on process exit/crash (OS closes the fd).
    struct SpoolLock
    {
        int  fd       = -1;
        bool acquired = false;
        SpoolLock()   = default;
        ~SpoolLock(); // calls _close/close if fd >= 0
        SpoolLock(const SpoolLock&)            = delete;
        SpoolLock& operator=(const SpoolLock&) = delete;
        SpoolLock(SpoolLock&& o) noexcept : fd(o.fd), acquired(o.acquired)
        {
            o.fd       = -1;
            o.acquired = false;
        }
        SpoolLock& operator=(SpoolLock&& o) noexcept;
    };
    static SpoolLock try_acquire_spool_lock(const boost::filesystem::path& dir);

    struct Internals
    {
        // --- Configuration / deps (set at init, immutable after) ---
        SnapLogDeps   deps;
        SnapLogConfig cfg;
        std::string   machine_id_snapshot; // read once at init for renderer

        // --- Event queues (guarded by queue_mu) ---
        std::mutex queue_mu;

        // --- Atomics (lock-free fast paths) ---
        std::atomic<bool> consent{true};
        std::atomic<bool> stop_receiving{true}; // true until init() flips it
        std::atomic<bool> deps_invalid{false};

        // --- Identity (guarded by token_mu) ---
        std::mutex  token_mu;
        std::string user_token;
        std::string user_id;
        // Flutter MQTT identity of the connected printer.
        std::string connect_clientid;
        std::string print_sn;
        // Mirror of current_batch_id (owned by bt_worker_loop) for lock-safe
        // reads from log()/PostHog rendering; refreshed by bt_worker each tick under
        // token_mu. Empty when the batch channel is inactive.
        std::string current_batch_id_snapshot;

        // --- Rate limiter (constructed at init with deps.now_ms) ---
        RateLimiter rate_limiter;

        // --- In-flight handle slots (guarded by m_h_mu; used by worker) ---
        std::mutex                     m_h_mu;
        std::shared_ptr<SnapLogHandle> bt_handle;
        std::shared_ptr<SnapLogHandle> ph_handle;

        std::deque<LogEvent>                          bt_queue;
        std::map<PostHogProject, PostHogProjectQueue> ph_queues;
        std::atomic<bool>                             auth_known_dead{false};        // set on 401/403 (B5)
        std::atomic<bool>                             drain_and_flush{false};        // shutdown final-flush (B5/B6)
        std::atomic<bool>                             stop_uploads{false};           // shutdown/consent aborts uploads
        std::atomic<bool>                             batch_deps_invalid{false};     // batch-only dependency fence
        std::atomic<uint32_t>                         batch_requests_in_progress{0}; // synchronous do_request calls
        std::atomic<uint64_t>                         batch_queue_dropped{0};
        std::atomic<uint64_t>                         posthog_queue_dropped{0};
        std::atomic<uint64_t>                         posthog_upload_successes{0};
        std::atomic<uint64_t>                         posthog_upload_failures{0};
        std::atomic<bool>                             posthog_drain_and_flush{false};
        std::atomic<bool>                             posthog_stop_uploads{false};
        std::atomic<int64_t>                          posthog_last_flush_ms{0};
        std::atomic<uint64_t>                         posthog_consent_generation{0};
        std::atomic<uint64_t>                         auth_dead_create_skipped{0}; // bumped in B5
        std::atomic<bool>                             auth_dead_skip_counted{false};
        std::atomic<uint64_t>                         disk_write_failed{0};              // bumped in B5
        std::atomic<uint64_t>                         shutdown_detach_queue_residual{0}; // bumped in B6
        std::atomic<uint64_t>                         purge_delete_failed{0};            // bumped in B6 (Windows delete-fail)
        std::thread                                   bt_worker;                         // started in B6, not here
        std::thread                                   ph_worker;
        // Each production worker captures a generation. Consent OFF/re-init
        // increments it before retiring the old worker, so a late return from
        // a blocked request cannot mutate the restarted worker's state.
        std::atomic<uint64_t>   batch_worker_generation{0};
        boost::filesystem::path spool_dir_resolved;      // resolved in B6; empty until then
        SpoolLock               bt_spool_lock;           // exclusive lock on spool_dir/.lock
        std::atomic<bool>       batch_locked_out{false}; // true if another process holds the lock

        boost::filesystem::path active_path;
        std::string             current_batch_id;
        size_t                  events_in_active{0};
        int64_t                 last_flush_ms{0}; // periodic-flush timer (cfg.batch_flush_sec); reset on rotate
        BatchLineContext        cached_batch_context;
        bool                    cached_context_valid{false};
        std::atomic<uint64_t>   deps_invalid_render_skipped{0};
        std::atomic<uint64_t>   sealed_evicted{0};

        // The upload spans multiple poll ticks: issue on tick N, poll done() on
        // ticks N+1.. attempt_upload is a per-tick STEP function that advances
        // one phase per tick (or returns "still in-flight"). All fields below
        // are written ONLY by bt_worker_loop (single-threaded owner), so they
        // need no mutex — same ownership model as active_path/current_batch_id.
        // bt_handle (in m_h_mu above) holds the in-flight SnapLogHandle; the
        // phase tracks which request is in flight.
        enum class BtUploadPhase {
            Idle          = 0, // no upload in progress; pick a sealed next tick
            AwaitCreate   = 1, // create request issued, polling done()
            AwaitPut      = 2, // PUT to presigned URL issued, polling done()
            AwaitComplete = 3  // completed request issued, polling done()
        };
        BtUploadPhase           bt_upload_phase{BtUploadPhase::Idle};
        boost::filesystem::path bt_in_flight_sealed; // sealed being uploaded (backpressure exclude + PUT body)
        std::string             bt_put_url;          // presigned PUT URL from create response
        std::string             bt_put_key;          // full S3 key from create response (completed body)
        std::string bt_frozen_token;      // token snapshot frozen at upload start (reused across create/PUT/completed/cancel + retries)
        std::string bt_frozen_client_id;  // machine_id snapshot frozen at upload start
        std::string bt_frozen_batch_id;   // sealed-file batchId frozen across retries
        int         bt_upload_attempt{0}; // 1-based attempt counter (re-create on retry)
        std::atomic<uint64_t> bt_upload_sealed_deleted{0};  // sealed files deleted after successful completed
        std::atomic<uint64_t> bt_upload_sealed_retained{0}; // sealed left on disk after terminal failure (at-least-once)
        std::atomic<uint64_t> bt_upload_retries{0};         // total retry re-creates performed

        // Fix A: per-instance upload sequence counter for unique S3 keys. Each
        // create call appends ".<now_ms>.<seq>" so two uploads from the same
        // batchId never collide on S3 (which would silently overwrite).
        std::atomic<uint64_t> upload_seq{0};

        explicit Internals(SnapLogDeps d, SnapLogConfig c)
            : deps(std::move(d)), cfg(std::move(c)), rate_limiter(deps.now_ms ? deps.now_ms : []() { return int64_t(0); })
        {}
    };

    static void bt_worker_loop(std::shared_ptr<Internals> in, uint64_t generation = 0);
    static void ph_worker_loop(std::shared_ptr<Internals> in);

    // Test-only factory: build a fresh Internals from deps + cfg. Used by the
    // batch tests to construct a fake flusher context (spool dir, queues, etc)
    // without touching the singleton.
    static std::shared_ptr<Internals> make_internals_for_test(SnapLogDeps deps, SnapLogConfig cfg);

private:
    // m_state_mu only guards the m_int pointer. Slow lifecycle transitions use
    // m_lifecycle_mu and copy the pointer, so producers never wait on joins.
    mutable std::mutex         m_state_mu;
    mutable std::mutex         m_lifecycle_mu;
    std::shared_ptr<Internals> m_int;
    // The spool path is stable after init, so one pending purge is sufficient.
    boost::filesystem::path             m_purge_spool_dir;
    std::shared_ptr<std::promise<bool>> m_purge_promise;
    std::shared_future<bool>            m_purge_completion;
    boost::filesystem::path             m_purge_lock_spool_dir;
    SpoolLock                           m_purge_spool_lock;
    std::shared_ptr<Internals>          internals() const;
    void                     dispatch_event(SnapLogLevel lvl, std::string msg, SnapLogExt ext, bool snaplog, PostHogProjectMask projects);
    void                     cancel_current(const std::shared_ptr<Internals>& in);
    void                     cancel_posthog_current(const std::shared_ptr<Internals>& in);
    void                     start_posthog_worker(const std::shared_ptr<Internals>& in);
    void                     start_batch_worker(const std::shared_ptr<Internals>& in);
    std::shared_future<bool> begin_spool_purge(const std::shared_ptr<Internals>& in, std::thread retired_worker = {});
    void                     start_batch_worker_after_purge(const std::shared_ptr<Internals>& in);
    bool                     spool_purge_blocks_worker(const boost::filesystem::path& spool_dir) const;
};

// Fix A: Build a unique upload fileName for the S3 presigned-PUT key.
// Format: "<clientId>/<batchId>.<now_ms>.<seq>.ndjson"
// now_ms + atomic seq guarantees uniqueness across retries and concurrent
// batches, preventing S3 key collisions that would silently overwrite logs.
std::string make_upload_file_name(const std::string& clientId, const std::string& batchId, int64_t now_ms, uint64_t seq);

// Result of batch request building: URL + ordered headers + JSON body string.
// Batch endpoints carry a JSON body (create/completed/
// cancel lifecycle). Auth is Bearer-only
// (Bearer-only); the body is endpoint-specific per [DOC-2].
struct BatchRequest
{
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body; // JSON string
};

// Build the batch create request (pre-signed PUT URL acquisition).
//   URL: {snaplog}/api/log/upload/create
//   body: {"fileName":"<sanitized name>"} (checkSum omitted for v1 — server
//         doesn't require it; computing it needs the file bytes which belongs
//         in the flusher, not the request builder).
BatchRequest build_batch_create_request(const SnapLogConfig& cfg,
                                        const std::string&   clientId,
                                        const std::string&   token,
                                        const std::string&   fileName);

// Build the batch complete request (notify server the PUT finished).
//   URL: {snaplog}/api/log/upload/completed
//   body: {"fileName":"<full S3 key returned by create response>"}
//         (single-part: no uploadId/uploadParts).
BatchRequest build_batch_complete_request(const SnapLogConfig& cfg,
                                          const std::string&   clientId,
                                          const std::string&   token,
                                          const std::string&   full_key);

// Build the batch cancel request (abort an in-progress upload).
//   URL: {snaplog}/api/log/upload/cancel
//   body: {"uploadId":"<id>"} when uploadId is non-empty; "{}" when empty
//         (single-part has no uploadId — cancel is best-effort).
BatchRequest build_batch_cancel_request(const SnapLogConfig& cfg,
                                        const std::string&   clientId,
                                        const std::string&   token,
                                        const std::string&   uploadId);

// These are defined in SnapLogClient.cpp and used by the GUI wiring code
// (GUI_App.cpp on_init_inner / login / logout hooks). They live in this
// namespace so callers qualify them as
// ::Slic3r::SnapLog::v1::make_production_do_request(cfg) etc.

// Factory that returns a do_request std::function wrapping real Slic3r::Http.
// Captures cfg by value so the closure is self-contained.
std::function<std::shared_ptr<SnapLogHandle>(const std::string&,
                                             const std::string&,
                                             std::vector<std::pair<std::string, std::string>>,
                                             const boost::filesystem::path*,
                                             const std::string*)>
make_production_do_request(SnapLogConfig cfg);

}}} // namespace Slic3r::SnapLog::v1

// Global-scope macros using the fully-qualified namespace. Call sites write:
//   SNAP_LOG_INFO(SnapEvent::APP_LAUNCH);

#define SNAP_LOG_INFO(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event(::Slic3r::SnapLog::v1::SnapLogLevel::Info, __VA_ARGS__)
#define SNAP_LOG_WARNING(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event(::Slic3r::SnapLog::v1::SnapLogLevel::Warning, __VA_ARGS__)
#define SNAP_LOG_ERROR(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event(::Slic3r::SnapLog::v1::SnapLogLevel::Error, __VA_ARGS__)
