# SnapLog Entry API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the legacy entry macros `SNAP_LOG`, `SNAP_LOG_BATCH`, and `SNAP_LOG_BATCH_FORCE` with `SNAP_LOG_INFO`, `SNAP_LOG_WARNING`, and `SNAP_LOG_ERROR`, backed by a centralized event registry and common sink routing. The old macro names are deleted, not shimmed. Gateway reporting stays login-only; the anonymous HMAC signing key and public upload paths that realtime and forced reporting depended on are removed with them.

**Architecture:** A header-only X-macro registry defines the event enum and name mapping. One variadic macro per level forwards all caller arguments to overloaded `SnapLogClient::log_event` methods, letting C++ overload resolution select event-only, event-plus-fields, event-plus-message, and event-plus-fields-plus-message forms. Renderers receive the configured profile version and then append caller business fields.

**Tech Stack:** C++17, nlohmann/json, Catch2, existing SnapLog Gateway and PostHog queues.

---

### Task 1: Event Registry

**Files:**
- Create: `src/slic3r/Utils/SnapLogEvents.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

- [x] **Step 1: Write failing registry tests**

Append near the top of `tests/slic3rutils/SnapLogClient_test.cpp`:

```cpp
TEST_CASE("SnapEvent registry maps every enum to a unique stable name", "[snaplog][events]")
{
    REQUIRE(snap_event_name(SnapEvent::AppLaunch) == "app_launch");
    REQUIRE(snap_event_name(SnapEvent::SliceFailed) == "slice_failed");
    REQUIRE(snap_event_from_name("calibration_result") == SnapEvent::CalibrationResult);
    REQUIRE_FALSE(snap_event_from_name("not_a_snap_event").has_value());

    const std::array<SnapEvent, 23> events = {
        SnapEvent::AppLaunch, SnapEvent::AppSessionEnd, SnapEvent::CalibrationResult,
        SnapEvent::DeviceConnectAttempt, SnapEvent::DeviceDisconnect, SnapEvent::DeviceDiscoveryStart,
        SnapEvent::DeviceEngineSet, SnapEvent::FileImportResult, SnapEvent::FirmwareUpgradeResult,
        SnapEvent::FlutterRunResult, SnapEvent::GcodeExportResult, SnapEvent::MqttClientCreate,
        SnapEvent::MqttClientCreateFailed, SnapEvent::MqttConnectAttempt, SnapEvent::MqttConnectFailure,
        SnapEvent::MqttConnectResult, SnapEvent::MqttSubscribeResult, SnapEvent::PrintJobSendResult,
        SnapEvent::PrivacyConsentGranted, SnapEvent::ProjectSaveResult, SnapEvent::SliceCompleted,
        SnapEvent::SliceFailed, SnapEvent::SliceStart
    };

    std::set<std::string_view> names;
    for (const auto event : events)
        REQUIRE(names.insert(snap_event_name(event)).second);
}
```

Add `<array>` and `<set>` to the test includes.

- [x] **Step 2: Run the failing test**

Run:

```powershell
.\build2\tests\slic3rutils\Release\slic3rutils_tests.exe "[snaplog]"
```

Expected: compilation fails because `SnapEvent`, `snap_event_name`, and `snap_event_from_name` do not exist.

- [x] **Step 3: Create the registry**

Create `src/slic3r/Utils/SnapLogEvents.hpp`:

```cpp
#pragma once

#include <optional>
#include <string_view>

namespace Slic3r {
namespace SnapLog {
inline namespace v1 {

#define SNAP_LOG_EVENT_LIST(X) \
    X(AppLaunch, "app_launch") \
    X(AppSessionEnd, "app_session_end") \
    X(CalibrationResult, "calibration_result") \
    X(DeviceConnectAttempt, "device_connect_attempt") \
    X(DeviceDisconnect, "device_disconnect") \
    X(DeviceDiscoveryStart, "device_discovery_start") \
    X(DeviceEngineSet, "device_engine_set") \
    X(FileImportResult, "file_import_result") \
    X(FirmwareUpgradeResult, "firmware_upgrade_result") \
    X(FlutterRunResult, "flutter_run_result") \
    X(GcodeExportResult, "gcode_export_result") \
    X(MqttClientCreate, "mqtt_client_create") \
    X(MqttClientCreateFailed, "mqtt_client_create_failed") \
    X(MqttConnectAttempt, "mqtt_connect_attempt") \
    X(MqttConnectFailure, "mqtt_connect_failure") \
    X(MqttConnectResult, "mqtt_connect_result") \
    X(MqttSubscribeResult, "mqtt_subscribe_result") \
    X(PrintJobSendResult, "print_job_send_result") \
    X(PrivacyConsentGranted, "privacy_consent_granted") \
    X(ProjectSaveResult, "project_save_result") \
    X(SliceCompleted, "slice_completed") \
    X(SliceFailed, "slice_failed") \
    X(SliceStart, "slice_start")

enum class SnapEvent {
#define X(id, name) id,
    SNAP_LOG_EVENT_LIST(X)
#undef X
};

inline constexpr std::string_view snap_event_name(SnapEvent event)
{
    switch (event) {
#define X(id, name) case SnapEvent::id: return name;
        SNAP_LOG_EVENT_LIST(X)
#undef X
    }
    return {};
}

inline std::optional<SnapEvent> snap_event_from_name(std::string_view name)
{
#define X(id, wire_name) if (name == wire_name) return SnapEvent::id;
    SNAP_LOG_EVENT_LIST(X)
#undef X
    return std::nullopt;
}

#undef SNAP_LOG_EVENT_LIST

} // inline namespace v1
} // namespace SnapLog
} // namespace Slic3r
```

Include `SnapLogEvents.hpp` from `SnapLogClient.hpp`.

- [x] **Step 4: Run the registry test**

Rebuild the test target and rerun `[snaplog]`. Expected: the new test passes.

### Task 2: Entry Overloads And Macros

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

- [x] **Step 1: Write failing API tests**

Add tests that invoke all four forms:

```cpp
TEST_CASE("level macros route event-only and field-only forms", "[snaplog][api]")
{
    FakeClient fc;
    fc.cfg.default_rate_cap_per_sec = 1000;
    fc.cfg.posthog_api_key = "phc_test";
    fc.init();

    SNAP_LOG_INFO(SinkPostHog, SnapEvent::AppLaunch);
    SNAP_LOG_WARNING(SinkGateway | SinkPostHog, SnapEvent::FileImportResult,
        {{"source", "dialog"}, {"success", "false"}});

    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    REQUIRE(SnapLogClient::instance().posthog_queue_size_for_test() == 2);
    fc.shutdown();
}

TEST_CASE("level macros route message-only and field-plus-message forms", "[snaplog][api]")
{
    FakeClient fc;
    fc.cfg.default_rate_cap_per_sec = 1000;
    fc.cfg.posthog_api_key = "phc_test";
    fc.init();

    SNAP_LOG_ERROR(SinkGateway, SnapEvent::MqttClientCreateFailed, "mqtt failed");
    SNAP_LOG_INFO(SinkPostHog, SnapEvent::FileImportResult,
        {{"source", "dialog"}, {"fileType", "3mf"}}, "import completed");

    REQUIRE(SnapLogClient::instance().batch_queue_size_for_test() == 1);
    REQUIRE(SnapLogClient::instance().posthog_queue_size_for_test() == 1);
    fc.shutdown();
}
```

- [x] **Step 2: Verify failure**

Run the test target. Expected: compilation fails because the macros and overloads do not exist.

- [x] **Step 3: Implement the API**

Declare these public methods on `SnapLogClient`:

```cpp
void log_event(SnapLogLevel level, const char* caller_func, int caller_line, unsigned sinks, SnapEvent event);
void log_event(SnapLogLevel level, const char* caller_func, int caller_line, unsigned sinks, SnapEvent event, SnapLogExt ext);
void log_event(SnapLogLevel level, const char* caller_func, int caller_line, unsigned sinks, SnapEvent event, const std::string& msg);
void log_event(SnapLogLevel level, const char* caller_func, int caller_line, unsigned sinks, SnapEvent event, SnapLogExt ext,
               const std::string& msg);
```

Implement a private helper that appends the registry event name to a copy of caller fields and calls the existing sink-routing `log` overload. Then replace the bottom macro block with:

```cpp
#define SNAP_LOG_INFO(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event( \
        ::Slic3r::SnapLog::v1::SnapLogLevel::Info, __FUNCTION__, __LINE__, __VA_ARGS__)
#define SNAP_LOG_WARNING(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event( \
        ::Slic3r::SnapLog::v1::SnapLogLevel::Warning, __FUNCTION__, __LINE__, __VA_ARGS__)
#define SNAP_LOG_ERROR(...) \
    ::Slic3r::SnapLog::v1::SnapLogClient::instance().log_event( \
        ::Slic3r::SnapLog::v1::SnapLogLevel::Error, __FUNCTION__, __LINE__, __VA_ARGS__)
```

Do not define `SNAP_LOG`, `SNAP_LOG_BATCH`, or `SNAP_LOG_BATCH_FORCE`.

- [x] **Step 4: Run API tests**

Expected: both new API tests pass together with the existing suite.

### Task 3: Profile Version And Field Override

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Modify: `src/slic3r/GUI/GUI_App.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

- [x] **Step 1: Write failing renderer tests**

```cpp
TEST_CASE("renderers inject and allow caller override of profileVersion", "[snaplog][fields]")
{
    BatchLineContext ctx;
    ctx.eventName = "app_launch";
    ctx.profileVersion = "02.03.03.03";

    auto gateway = render_batch_line(SnapLogLevel::Info, "", {}, 64 * 1024, ctx);
    auto parsed_gateway = nlohmann::json::parse(gateway.substr(0, gateway.size() - 1));
    REQUIRE(parsed_gateway["ext"]["profileVersion"] == "02.03.03.03");

    PostHogEvent event;
    event.context = ctx;
    event.context.eventName = "app_launch";
    event.ext = {{"profileVersion", "99.99.99.99"}, {"source", "flutter"}};
    auto parsed_posthog = nlohmann::json::parse(render_posthog_event(event));
    REQUIRE(parsed_posthog["properties"]["profileVersion"] == "99.99.99.99");
    REQUIRE(parsed_posthog["properties"]["source"] == "flutter");

    ctx.profileVersion.clear();
    auto empty_gateway = render_batch_line(SnapLogLevel::Info, "", {}, 64 * 1024, ctx);
    auto parsed_empty = nlohmann::json::parse(empty_gateway.substr(0, empty_gateway.size() - 1));
    REQUIRE(parsed_empty["ext"]["profileVersion"] == "");
}
```

- [x] **Step 2: Verify failure**

Expected: compilation fails because `BatchLineContext::profileVersion` does not exist.

- [x] **Step 3: Implement field semantics**

Add `std::string profile_version;` to `SnapLogConfig` and `std::string profileVersion;` to `BatchLineContext`. Copy the config value while constructing both render contexts. Emit the default before caller ext:

```cpp
ext_pairs.emplace_back("profileVersion", sanitize_value("profileVersion", ctx.profileVersion));
properties["profileVersion"] = sanitize_value("profileVersion", ctx.profileVersion);
```

Remove `source` and `profileVersion` from the PostHog reserved list. Keep protocol and identity fields reserved. In GUI initialization set:

```cpp
snap_cfg.profile_version = common::get_profile_version();
```

- [x] **Step 4: Run field tests**

Expected: new field tests pass and existing renderer tests remain green.

### Task 4: Remove Legacy Entry, Then Adapt Call Sites

Implementation order is delete-first: strip the old interface and every reference so the codebase compiles with no legacy logging entry, then re-add logging at each site through the new macros. Do not keep the old macro alive during migration.

**Files:**
- Modify: `src/slic3r/GUI/DeviceManager.cpp`
- Modify: `src/slic3r/GUI/GUI_App.cpp`
- Modify: `src/slic3r/GUI/Jobs/PrintJob.cpp`
- Modify: `src/slic3r/GUI/Jobs/SendJob.cpp`
- Modify: `src/slic3r/GUI/Plater.cpp`
- Modify: `src/slic3r/GUI/Preferences.cpp`
- Modify: `src/slic3r/GUI/SSWCP.cpp`
- Modify: `src/slic3r/GUI/UpgradePanel.cpp`
- Modify: `src/slic3r/GUI/WebGuideDialog.cpp`
- Modify: `src/slic3r/GUI/WebSMUserLoginDialog.cpp`
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`

- [x] **Step 1: Delete the legacy macro and every call site**

Delete the `SNAP_LOG_BATCH` macro definition from `SnapLogClient.hpp`. Then remove every `SNAP_LOG_BATCH(...)` call statement across the GUI files listed above, together with the local variables that only served those calls. Do not replace them yet in this step.

Keep `SNAP_LOG` and `SNAP_LOG_BATCH_FORCE` absent. Also confirm no header or source references the deleted macro shape (`SnapLogPolicy`-based `log` overload, `bypass_login_gate`) from production code.

- [x] **Step 2: Verify the stripped build**

Build `libslic3r_gui` (or the app target) and run `rg -n "SNAP_LOG_BATCH|SNAP_LOG_BATCH_FORCE|#define SNAP_LOG\\b" src`. Expected: compilation succeeds and the search returns nothing. The codebase now has zero legacy logging entry; site-by-site restoration starts only after this checkpoint.

- [x] **Step 3: Adapt each site to the new API**

Re-add logging at the stripped sites using the matching level macro, the registry enum, and the original business fields and messages, minus the old `{"eventName", ...}` pair. For example:

```cpp
SNAP_LOG_INFO(SinkPostHog, SnapEvent::AppLaunch,
    {
        {"firstLaunch", SnapTelemetry::bool_value(!m_app_conf_exists)},
        {"openMethod", launch_with_file ? "file" : "normal"}
    },
    "app launched");
```

Convert direct `SnapLogClient::log` result reporters to `SNAP_LOG_INFO` or `SNAP_LOG_WARNING` branches, or use `log_event` with an already-resolved strongly typed event.

- [x] **Step 4: Convert dynamic event names to registry values**

For `calibration_result`, use:

```cpp
const auto event = m_is_calibration_task ? SnapEvent::CalibrationResult : SnapEvent::PrintJobSendResult;
```

For slice completion, pass `SnapEvent::SliceCompleted` or `SnapEvent::SliceFailed` from the caller.

For the Flutter bridge, validate the incoming string:

```cpp
const auto event = snap_event_from_name(ext_event_name);
if (!event)
    return;
```

Then route through the new sink path with `SinkGateway`.

- [x] **Step 5: Search verification**

Run:

```powershell
rg -n "SNAP_LOG_BATCH(_FORCE)?|#define SNAP_LOG\\b" src
rg -n "\\{\")eventName\\\"" src\\slic3r\\GUI src\\slic3r\\Utils
```

Expected: no matches. `eventName` may remain only in SnapLog protocol rendering and tests.

### Task 5: Login-Only Gateway And Secret Cleanup

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/slic3r/GUI/GUI_App.cpp`
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

Gateway log reporting is login-only: without a user token, gateway events are dropped at admission. PostHog does not require the Gateway login token. The historical anonymous upload paths and their signing key are dead code once the realtime and forced entries are gone, so remove them outright.

- [x] **Step 1: Remove the anonymous signing key**

Delete from `CMakeLists.txt`:

```cmake
set(SNAP_LOG_HMAC_SECRET "..." CACHE STRING ...)
add_definitions("-DSNAP_LOG_HMAC_SECRET=\"${SNAP_LOG_HMAC_SECRET}\"")
```

Delete from `GUI_App.cpp`:

```cpp
snap_cfg.hmac_secret = SNAP_LOG_HMAC_SECRET;
```

Delete `SnapLogConfig::hmac_secret`. Keep the `hmac_sha256_hex` primitive itself: `hash_pii` reuses its OpenSSL SHA-256 core for local PII hashing with `clientId` as the key, and no shared secret is involved there.

- [x] **Step 2: Remove the public upload paths**

In `build_realtime_request` and `build_batch_create_request` / `build_batch_complete_request` / `build_batch_cancel_request`, delete the `token.empty()` branches that target `/api/log/public/upload/...` and emit `X-Timestamp` / `X-Nonce` / `X-Sign`. Only the logged-in `Authorization: Bearer` paths remain. A missing token must produce no request.

Add a test asserting an empty token yields no URL/headers (or that the worker issues no request), replacing the historical public-path HMAC tests including the X-Sign doc-vector cases.

- [x] **Step 3: Search verification**

```powershell
rg -n "SNAP_LOG_HMAC_SECRET|hmac_secret" CMakeLists.txt src tests
rg -n "public/upload|X-Sign|X-Nonce" src tests
```

Expected: no matches outside this plan document. `hash_pii` and `hmac_sha256_hex` may still appear; the shared signing secret must not.

### Task 6: Full Verification

**Files:**
- No new files.

- [ ] **Step 1: Build and run focused tests**

```powershell
cmake --build build2 --target slic3rutils_tests --config Release
.\build2\tests\slic3rutils\Release\slic3rutils_tests.exe "[snaplog]"
```

Expected: all tests pass with no new warnings.

- [ ] **Step 2: Build the application**

```powershell
cmake --build build2 --target Snapmaker_Orca --config Release
```

If the existing unrelated `PresetBundle::full_print_config` build error remains, record it as pre-existing and do not revert unrelated worktree changes.

- [ ] **Step 3: Review the diff**

Confirm:

- Event strings are centralized in `SnapLogEvents.hpp`.
- No production `SnapLogExt` appears merely to name `eventName`.
- `profileVersion` is always emitted.
- Caller business fields survive sanitization.
- No legacy macro is defined or called.
- No anonymous signing key, `X-Sign` header, or `/public/upload/` path remains in production code.
