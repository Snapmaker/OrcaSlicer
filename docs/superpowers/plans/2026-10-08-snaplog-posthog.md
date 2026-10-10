# SnapLog PostHog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reuse the SnapLog producer pipeline to route batch events to the existing gateway and a new self-hosted PostHog sink.

**Architecture:** Keep `SNAP_LOG_BATCH` as the public batch entry point and add a required sink bitmask after `msg`. Gateway events continue through the existing login-gated `bt_queue`/spool path; PostHog events are sanitized and snapshot at producer time, stored only in memory, and flushed as a PostHog `/batch/` request. Existing realtime and forced paths remain unchanged.

**Tech Stack:** C++17, nlohmann JSON, Catch2, existing `SnapLogDeps::do_request`.

## Global Constraints

- `SNAP_LOG_BATCH` remains the batch macro name.
- `SinkGateway = 1 << 0`, `SinkPostHog = 1 << 1`, combinable with bitwise OR.
- `privacy_policy_isagree` remains the sole consent gate for both output channels.
- Gateway output remains login-gated; PostHog output is login-independent.
- `distinct_id` is always the producer-snapshot `machine_id`; `userId` is only a property.
- PostHog properties are flattened; no nested `properties.ext`.
- PostHog uses a memory queue only and no local file.
- No high-frequency aggregation and no event registry in this change.
- Do not redesign or remove `SNAP_LOG`, realtime logging, or force logging in this change.

---

### Task 1: Sink Routing And PostHog Event Contract

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

**Interfaces:**
- Produces: `enum SnapLogSinks : unsigned`, `PostHogEvent`, `render_posthog_event`, `build_posthog_batch_request`.
- Produces: `SnapLogClient::log(..., const char*, int, unsigned sinks)` overload while retaining the legacy policy overload.

- [ ] Add failing tests for default config, flattened/sanitized PostHog JSON, `/batch/` request shape, sink routing, anonymous PostHog admission, and consent-off queue cleanup.
- [ ] Run only the new tests and confirm compilation fails because the sink API is absent.
- [ ] Add the minimal sink enum, event snapshot, producer rendering, queue, and request builder.
- [ ] Re-run the new tests and then the full `slic3rutils` suite.

### Task 2: PostHog Worker Lifecycle

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/Utils/SnapLogClient.cpp`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

**Interfaces:**
- Produces: `ph_worker_loop`, queue capacity/size/count test accessors, and consent/shutdown cancellation behavior.

- [ ] Add failing worker tests for size-triggered flush, timer-triggered flush, transport failure dropping, and shutdown drain.
- [ ] Implement one-worker, one-in-flight batching with 1024 queue cap, 50-event threshold, 60-second timer, and no retry.
- [ ] Clear/cancel PostHog state on consent-off and shutdown.
- [ ] Re-run focused and full test suites.

### Task 3: Public Macro And Call-Site Migration

**Files:**
- Modify: `src/slic3r/Utils/SnapLogClient.hpp`
- Modify: `src/slic3r/GUI/*.cpp` call sites that use `SNAP_LOG_BATCH`
- Test: `tests/slic3rutils/SnapLogClient_test.cpp`

**Interfaces:**
- Produces: `SNAP_LOG_BATCH(lvl, msg, sinks, ...)`; existing events are migrated to `SinkGateway`.

- [ ] Add a failing compile test that calls the new macro with `SinkGateway | SinkPostHog`.
- [ ] Change the macro and mechanically insert `SinkGateway` into all existing call sites.
- [ ] Configure the production PostHog endpoint/key from CMake without printing secrets.
- [ ] Build the focused test target and run it.

### Task 4: Verification

**Files:**
- No new production files.

- [ ] Run `cmake --build build --target slic3rutils_tests --config Release` (or the existing configured equivalent).
- [ ] Run the `slic3rutils` Catch2 executable with `[snaplog]`.
- [ ] Inspect the diff for unrelated files and summarize residual risks.
