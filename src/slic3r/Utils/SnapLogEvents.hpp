#pragma once

#include <optional>
#include <string_view>

namespace Slic3r { namespace SnapLog { inline namespace v1 {

struct SnapLogEventRoute
{
    bool snaplog{false};
};

enum class SnapLogLevel { Info, Warning, Error };

// snaplog is the route used for WARNING/ERROR. A level-sensitive result event
// drops its INFO (success) variant and keeps SnapLog for failures.
#define SNAP_LOG_EVENT_LIST(X) \
    X(DEVICE_DISCOVERY_START, "device_discovery_start", true, false) \
    X(APP_LAUNCH, "app_launch", true, false) \
    X(FILE_IMPORT_RESULT, "file_import_result", true, true) \
    X(GCODE_EXPORT_RESULT, "gcode_export_result", true, true) \
    X(PROJECT_SAVE_RESULT, "project_save_result", true, true) \
    X(SLICE_FAILED, "slice_failed", true, true) \
    X(USER_LOGIN_RESULT, "user_login_result", true, false) \
    X(SLICE_START, "slice_start", true, false) \
    X(MIX_COLOR_USED, "mix_color_used", true, false)

enum class SnapEvent {
#define X(id, name, snaplog_enabled, level_sensitive) id,
    SNAP_LOG_EVENT_LIST(X)
#undef X
};

inline constexpr std::string_view snap_event_name(SnapEvent event)
{
    switch (event) {
#define X(id, name, snaplog_enabled, level_sensitive) \
    case SnapEvent::id: return name;
        SNAP_LOG_EVENT_LIST(X)
#undef X
    }
    return {};
}

inline std::optional<SnapEvent> snap_event_from_name(std::string_view name)
{
#define X(id, wire_name, snaplog_enabled, level_sensitive) \
    if (name == wire_name) \
        return SnapEvent::id;
    SNAP_LOG_EVENT_LIST(X)
#undef X
    return std::nullopt;
}

inline constexpr SnapLogEventRoute snap_event_route(SnapEvent event, SnapLogLevel level);

inline constexpr SnapLogEventRoute snap_event_route(SnapEvent event, SnapLogLevel level)
{
    switch (event) {
#define X(id, name, snaplog_enabled, level_sensitive) \
    case SnapEvent::id: { \
        SnapLogEventRoute route{snaplog_enabled != 0}; \
        if (level_sensitive && level == SnapLogLevel::Info) \
            route.snaplog = false; \
        return route; \
    }
        SNAP_LOG_EVENT_LIST(X)
#undef X
    }
    return {};
}

#undef SNAP_LOG_EVENT_LIST

}}} // namespace Slic3r::SnapLog::v1
