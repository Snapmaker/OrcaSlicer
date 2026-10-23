# SnapLog Public Upload Authentication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove SnapLog's dependency on the encrypted account token while preserving all existing reporting fields and behavior.

**Architecture:** The three batch lifecycle requests move from Bearer endpoints to public HMAC endpoints. Public request construction is deterministic and test-injectable through `SnapLogConfig`, `SnapLogDeps::now_ms`, and a nonce generator. SnapLog admission works in anonymous and signed-in states; `userId` is captured at event time as a payload identity field, and GUI wiring no longer copies the encrypted token into SnapLog.

**Tech Stack:** C++17, OpenSSL HMAC/RAND_bytes, nlohmann/json, Catch2, CMake.

---

## Global Constraints

- Do not modify `GUI_App::request_version_from_config`; its account-token use is intentionally out of scope.
- Preserve event registry routes, NDJSON fields, PostHog fields, privacy handling, queue limits, retry behavior, spool behavior, and regional endpoint selection.
- Do not touch unrelated Flutter web build artifacts in the dirty worktree.
- Production never embeds the public signing secret in source; it is CMake-configured and injected as a compile definition.

### Task 1: Public HMAC Request Builders

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

- [x] **Step 1: Write failing builder tests**

Replace the current Bearer/empty-token builder tests with tests requiring:

```cpp
SnapLogConfig cfg;
cfg.snaplog_base       = "https://api.snapmaker.com";
cfg.public_hmac_secret = "secret";
cfg.now_ms             = []() { return int64_t(1778639794110); };
cfg.next_nonce         = [n = 0]() mutable { return std::string(n++ ? "nonce2" : "nonce1"); };

auto req = build_batch_create_request(cfg, "app-123", "batch.log");
REQUIRE(req.url == "https://api.snapmaker.com/api/log/public/upload/create");
REQUIRE(hdr(req.headers, "Authorization").empty());
REQUIRE(hdr(req.headers, "X-Client-Type") == "Orca");
REQUIRE(hdr(req.headers, "X-Client-Id") == "app-123");
REQUIRE(hdr(req.headers, "X-Timestamp") == "1778639794110");
REQUIRE(hdr(req.headers, "X-Nonce") == "nonce1");
REQUIRE(hdr(req.headers, "X-Sign") ==
        hmac_sha256_hex("secret", std::string("Orca") + "app-123" + "1778639794110" + "nonce1"));
```

Also assert completed and cancel receive fresh nonces, and an empty secret produces no URL, headers, or body.

- [x] **Step 2: Verify RED**

Run:

```powershell
cmd /c build2\codex-rsp\compile_task3.cmd
```

Expected: compilation fails because `public_hmac_secret`, `now_ms`, and `next_nonce` do not exist on `SnapLogConfig`.

- [x] **Step 3: Implement public request support**

Add to `SnapLogConfig`:

```cpp
std::string public_hmac_secret;
std::function<int64_t()> now_ms;
std::function<std::string()> next_nonce;
```

Add an OpenSSL `RAND_bytes` fallback for `next_nonce`, and build all lifecycle requests with:

```cpp
r.url = cfg.snaplog_base + "/api/log/public/upload/" + path_suffix;
r.headers.emplace_back("X-Client-Type", kSnapLogClientType);
r.headers.emplace_back("X-Client-Id", clientId);
r.headers.emplace_back("X-Timestamp", timestamp);
r.headers.emplace_back("X-Nonce", nonce);
r.headers.emplace_back("X-Sign", sign);
```

- [x] **Step 4: Verify builder tests pass**

Run:

```powershell
cmd /c build2\codex-rsp\compile_task3.cmd
cmd /c build2\codex-rsp\link_tests.cmd
build2\tests\slic3rutils\Release\slic3rutils_tests.exe "[snaplog][batch]" --order decl
```

Expected: all batch cases pass.

### Task 2: Anonymous Admission With Event-Time User ID

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

- [x] **Step 1: Write failing admission tests**

Change `FakeClient::init()` to call `set_user_id("user-1")` for payload assertions, then add:

```cpp
TEST_CASE("SnapLog admission does not require a user id", "[snaplog][pipeline][auth]")
{
    FakeClient fc;
    fc.init();
    SnapLogClient::instance().set_user_id("");
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 0);

    SnapLogClient::instance().log(SnapLogLevel::Warning, "anonymous accepted", {{"eventName", "anonymous_event"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);

    SnapLogClient::instance().set_user_id("user-1");
    SnapLogClient::instance().log(SnapLogLevel::Warning, "signed-in accepted", {{"eventName", "signed_in_event"}}, SinkSnapLog);
    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 2);
    fc.shutdown();
}
```

Also enqueue while `userId` is empty, change the live identity, drain through `bt_worker_loop`, and assert the rendered line keeps `userId == ""`.

- [x] **Step 2: Verify RED**

Run the focused test command above. Expected: anonymous admission fails while the implementation still checks account identity.

- [x] **Step 3: Implement minimal admission change**

Replace both admission checks:

```cpp
snaplog_allowed = !in->user_id.empty() && !in->batch_locked_out.load(std::memory_order_acquire);
```

with:

```cpp
snaplog_allowed = snaplog && !in->batch_locked_out.load(std::memory_order_acquire);
```

Capture `in->user_id` once at event time into `LogEvent`, and let the batch renderer use that event snapshot rather than the worker-time live identity. Remove the upload token freeze and token-empty return. Preserve auth-dead handling for public signature rejection.

- [x] **Step 4: Verify GREEN**

Run the full SnapLog suite. Expected: all tests pass except tests explicitly pinned to removed token behavior; update those tests to the new public-auth contract.

### Task 3: GUI Wiring And Hygiene

**Files:**
- Modify: `src/slic3r/GUI/GUI_App.cpp`
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Modify: `tests/slic3rutils/SnapLogClient_test.cpp`
- Modify: `docs/superpowers/specs/2026-10-27-snaplog-public-upload-auth-design.md` if implementation wording needs correction.

- [x] **Step 1: Write failing GUI/source tests**

Extend the existing source-hygiene test to reject:

```cpp
static constexpr std::string_view stale[] = {
    "set_user_token(snapshot.token)",
    "Authorization\", std::string(\"Bearer \") + token",
    "\"/api/log/upload/\"",
    "Auth is Bearer-only",
};
```

Only scan SnapLog production files and `GUI_App::apply_gateway_account`; do not scan `request_version_from_config`.

- [x] **Step 2: Verify RED**

Run the focused source-hygiene test. Expected: it finds the current Bearer transport and token wiring.

- [x] **Step 3: Implement GUI and cleanup changes**

In `GUI_App::apply_gateway_account`, delete:

```cpp
SnapLogClient::instance().set_user_token(snapshot.token);
```

Keep:

```cpp
SnapLogClient::instance().set_user_id(snapshot.userid);
```

In `initialize_snaplog`, configure the CMake-injected secret and default clock/nonce callbacks. Remove `set_user_token` from SnapLog's public API and internals, then update comments to describe public HMAC auth.

- [x] **Step 4: Full verification**

Run:

```powershell
cmd /c build2\codex-rsp\compile_task3.cmd
cmd /c build2\codex-rsp\link_lib.cmd
cmd /c build2\codex-rsp\link_tests.cmd
build2\tests\slic3rutils\Release\slic3rutils_tests.exe "[snaplog]" --order decl
git diff --check
```

Expected: all SnapLog tests pass and `git diff --check` is clean for files touched by this task.
