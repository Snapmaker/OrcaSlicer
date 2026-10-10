#pragma once

#include <optional>
#include <string_view>

namespace Slic3r {
namespace SnapLog {
inline namespace v1 {

#define SNAP_LOG_EVENT_LIST(X)                                                                                                                       \
    X(AppLaunch, "app_launch")                                                                                                                       \
    X(AppSessionEnd, "app_session_end")                                                                                                             \
    X(CalibrationResult, "calibration_result")                                                                                                       \
    X(DeviceConnectAttempt, "device_connect_attempt")                                                                                               \
    X(DeviceDisconnect, "device_disconnect")                                                                                                         \
    X(DeviceDiscoveryStart, "device_discovery_start")                                                                                               \
    X(DeviceEngineSet, "device_engine_set")                                                                                                         \
    X(FileImportResult, "file_import_result")                                                                                                       \
    X(FirmwareUpgradeResult, "firmware_upgrade_result")                                                                                             \
    X(FlutterRunResult, "flutter_run_result")                                                                                                       \
    X(GcodeExportResult, "gcode_export_result")                                                                                                     \
    X(MqttClientCreate, "mqtt_client_create")                                                                                                       \
    X(MqttClientCreateFailed, "mqtt_client_create_failed")                                                                                           \
    X(MqttConnectAttempt, "mqtt_connect_attempt")                                                                                                   \
    X(MqttConnectFailure, "mqtt_connect_failure")                                                                                                   \
    X(MqttConnectResult, "mqtt_connect_result")                                                                                                     \
    X(MqttSubscribeResult, "mqtt_subscribe_result")                                                                                                 \
    X(PrintJobSendResult, "print_job_send_result")                                                                                                   \
    X(PrivacyConsentGranted, "privacy_consent_granted")                                                                                             \
    X(ProjectSaveResult, "project_save_result")                                                                                                     \
    X(SliceCompleted, "slice_completed")                                                                                                             \
    X(SliceFailed, "slice_failed")                                                                                                                   \
    X(SliceStart, "slice_start")

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
    if (name == wire_name) return SnapEvent::id;
    SNAP_LOG_EVENT_LIST(X)
#undef X
    return std::nullopt;
}

#undef SNAP_LOG_EVENT_LIST

} // inline namespace v1
} // namespace SnapLog
} // namespace Slic3r
