# SnapLog Entry API Design

## Goal

Replace the caller-facing SnapLog entry points with level macros and a strongly typed event registry. The new API keeps sink routing explicit, makes the event name mandatory, allows caller fields to override business defaults, and injects the installed Snapmaker profile version into both sinks.

## Public API

```cpp
SNAP_LOG_INFO(SinkPostHog, SnapEvent::AppLaunch);

SNAP_LOG_INFO(SinkGateway | SinkPostHog, SnapEvent::FileImportResult,
    {
        {"source", "dialog"},
        {"fileType", "3mf"},
        {"success", "true"}
    });

SNAP_LOG_WARNING(SinkGateway | SinkPostHog, SnapEvent::FileImportResult,
    {
        {"source", "dialog"},
        {"success", "false"}
    },
    "file import failed");

SNAP_LOG_ERROR(SinkGateway, SnapEvent::MqttClientCreateFailed, "mqtt client create failed");
```

There is one public macro per level. Optional arguments are selected by C++ overloads, not by preprocessor argument counting. `SnapLogExt` remains the internal field-vector representation but callers write an initializer list directly and do not name the type.

## Event Registry

All production events are declared once in `src/slic3r/Utils/SnapLogEvents.hpp` with an X-macro list. The list generates `enum class SnapEvent` and `snap_event_name(SnapEvent)`. It also generates `snap_event_from_name(std::string_view)` for the existing Flutter bridge, so dynamic Flutter event names are validated against the same registry rather than bypassing it.

Adding an event requires one registry entry followed by use of the generated enum value. Direct event-name string literals are not valid at production call sites.

## Field Semantics

The caller ext contains business fields only. `eventName` is not caller-supplied. `msg` is optional and defaults empty.

`SnapLogConfig` gains `profile_version`, populated once during GUI initialization from `common::get_profile_version()`. Both renderers emit it with a stable field name:

- Gateway: `ext.profileVersion`
- PostHog: `properties.profileVersion`

The field is emitted even when empty. A caller may override `profileVersion` and `source` because they are business defaults. Protocol and identity fields remain protected: event name, level, timestamp, service, message, PostHog `distinct_id`, PostHog API key, user/client/process/batch identity, and transport metadata. All caller fields still pass the existing privacy pipeline.

## Routing

`SinkGateway` continues to use the existing local batch spool and Gateway upload lifecycle. `SinkPostHog` continues to use the in-memory PostHog batch queue and `/batch/` endpoint. They can be combined with bitwise OR. Gateway admission remains login-gated; PostHog does not require a Gateway login token.

## Legacy Removal

The old `SNAP_LOG_BATCH` macro is removed after migrating all production call sites. The historical `SNAP_LOG` and `SNAP_LOG_BATCH_FORCE` entry names are not retained; if no definition exists in the current worktree, the final verification still asserts that neither reappears. Production code must not call the old policy-based `SnapLogClient::log` overload directly. The Flutter bridge goes through registry validation and the new sink path.

## Verification

Unit tests cover event registry uniqueness and lookup, all macro overload forms, profile-version injection, business-field override, protected-field behavior, routing to both queues, and absence of legacy production calls. The existing `[snaplog]` suite remains the regression gate.
