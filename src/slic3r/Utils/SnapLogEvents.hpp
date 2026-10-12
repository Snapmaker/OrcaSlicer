#pragma once

#include <optional>
#include <string_view>

namespace Slic3r { namespace SnapLog { inline namespace v1 {

#define SNAP_LOG_EVENT_LIST(X) \
    X(APP_LAUNCH, "app_launch") \
    X(APP_SESSION_END, "app_session_end") \
    X(CALIBRATION_RESULT, "calibration_result") \
    X(DEVICE_CONNECT_ATTEMPT, "device_connect_attempt") \
    X(DEVICE_DISCONNECT, "device_disconnect") \
    X(DEVICE_DISCOVERY_START, "device_discovery_start") \
    X(DEVICE_ENGINE_SET, "device_engine_set") \
    X(FILE_IMPORT_RESULT, "file_import_result") \
    X(FIRMWARE_UPGRADE_RESULT, "firmware_upgrade_result") \
    X(FLUTTER_RUN_RESULT, "flutter_run_result") \
    X(GCODE_EXPORT_RESULT, "gcode_export_result") \
    X(MQTT_CLIENT_CREATE, "mqtt_client_create") \
    X(MQTT_CLIENT_CREATE_FAILED, "mqtt_client_create_failed") \
    X(MQTT_CONNECT_ATTEMPT, "mqtt_connect_attempt") \
    X(MQTT_CONNECT_FAILURE, "mqtt_connect_failure") \
    X(MQTT_CONNECT_RESULT, "mqtt_connect_result") \
    X(MQTT_SUBSCRIBE_RESULT, "mqtt_subscribe_result") \
    X(PRINT_JOB_SEND_RESULT, "print_job_send_result") \
    X(PRIVACY_CONSENT_GRANTED, "privacy_consent_granted") \
    X(PROJECT_SAVE_RESULT, "project_save_result") \
    X(SLICE_CANCELLED, "slice_cancelled") \
    X(SLICE_COMPLETED, "slice_completed") \
    X(SLICE_FAILED, "slice_failed") \
    X(SLICE_START, "slice_start") \
    X(USER_LOGIN_RESULT, "user_login_result") \
    X(USER_LOGOUT, "user_logout")

enum class SnapEvent {
#define X(id, name) id,
    SNAP_LOG_EVENT_LIST(X)
#undef X
};

inline constexpr std::string_view snap_event_name(SnapEvent event)
{
    switch (event) {
#define X(id, name) \
    case SnapEvent::id: return name;
        SNAP_LOG_EVENT_LIST(X)
#undef X
    }
    return {};
}

inline std::optional<SnapEvent> snap_event_from_name(std::string_view name)
{
#define X(id, wire_name) \
    if (name == wire_name) \
        return SnapEvent::id;
    SNAP_LOG_EVENT_LIST(X)
#undef X
    return std::nullopt;
}

#undef SNAP_LOG_EVENT_LIST

}}} // namespace Slic3r::SnapLog::v1
