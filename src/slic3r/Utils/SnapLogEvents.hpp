#pragma once

#include <optional>
#include <string_view>

namespace Slic3r { namespace SnapLog { inline namespace v1 {

using PostHogProjectMask                                   = unsigned int;
inline constexpr PostHogProjectMask PostHogProjectNone = 0;
inline constexpr PostHogProjectMask PostHogProjectCnOrca = 1u << 0;
inline constexpr PostHogProjectMask PostHogProjectIntlOrca = 1u << 1;
inline constexpr PostHogProjectMask PostHogProjectIntlModelLab = 1u << 2;
inline constexpr PostHogProjectMask PostHogProjectIntlClient = 1u << 3;
inline constexpr PostHogProjectMask PostHogProjectIntlModelCommunity = 1u << 4;
inline constexpr PostHogProjectMask kAllPostHogProjects = PostHogProjectCnOrca | PostHogProjectIntlOrca | PostHogProjectIntlModelLab |
                                                            PostHogProjectIntlClient | PostHogProjectIntlModelCommunity;

enum class PostHogProject {
    CnOrca = PostHogProjectCnOrca,
    IntlOrca = PostHogProjectIntlOrca,
    IntlModelLab = PostHogProjectIntlModelLab,
    IntlClient = PostHogProjectIntlClient,
    IntlModelCommunity = PostHogProjectIntlModelCommunity,
};

inline constexpr PostHogProjectMask posthog_project_mask(PostHogProject project)
{
    switch (project) {
    case PostHogProject::CnOrca: return PostHogProjectCnOrca;
    case PostHogProject::IntlOrca: return PostHogProjectIntlOrca;
    case PostHogProject::IntlModelLab: return PostHogProjectIntlModelLab;
    case PostHogProject::IntlClient: return PostHogProjectIntlClient;
    case PostHogProject::IntlModelCommunity: return PostHogProjectIntlModelCommunity;
    }
    return PostHogProjectNone;
}


struct SnapLogEventRoute
{
    bool               snaplog{false};
    PostHogProjectMask posthog_projects{PostHogProjectNone};
};

enum class SnapLogLevel { Info, Warning, Error };

// snaplog is the route used for WARNING/ERROR. A level-sensitive result event
// uses PostHog only for INFO (success) and adds SnapLog for failures.
#define SNAP_LOG_EVENT_LIST(X) \
    X(APP_LAUNCH, "app_launch", false, PostHogProjectCnOrca | PostHogProjectIntlOrca, false) \
    X(APP_SESSION_END, "app_session_end", false, PostHogProjectCnOrca | PostHogProjectIntlOrca, false) \
    X(DEVICE_DISCOVERY_START, "device_discovery_start", true, PostHogProjectNone, false) \
    X(FILE_IMPORT_RESULT, "file_import_result", true, PostHogProjectCnOrca | PostHogProjectIntlOrca, true) \
    X(GCODE_EXPORT_RESULT, "gcode_export_result", true, PostHogProjectCnOrca | PostHogProjectIntlOrca, true) \
    X(PRIVACY_CONSENT_GRANTED, "privacy_consent_granted", false, PostHogProjectCnOrca | PostHogProjectIntlOrca, false) \
    X(PROJECT_SAVE_RESULT, "project_save_result", true, PostHogProjectCnOrca | PostHogProjectIntlOrca, true) \
    X(SLICE_CANCELLED, "slice_cancelled", true, PostHogProjectCnOrca | PostHogProjectIntlOrca, true) \
    X(SLICE_COMPLETED, "slice_completed", true, PostHogProjectCnOrca | PostHogProjectIntlOrca | PostHogProjectIntlModelLab, true) \
    X(SLICE_FAILED, "slice_failed", true, PostHogProjectCnOrca | PostHogProjectIntlOrca, true) \
    X(SLICE_START, "slice_start", false, PostHogProjectCnOrca | PostHogProjectIntlOrca, false) \
    X(USER_LOGIN_RESULT, "user_login_result", true, PostHogProjectNone, false) \
    X(USER_LOGOUT, "user_logout", false, PostHogProjectCnOrca | PostHogProjectIntlOrca, false) \

enum class SnapEvent {
#define X(id, name, snaplog_enabled, projects, level_sensitive) id,
    SNAP_LOG_EVENT_LIST(X)
#undef X
};

inline constexpr std::string_view snap_event_name(SnapEvent event)
{
    switch (event) {
#define X(id, name, snaplog_enabled, projects, level_sensitive) \
    case SnapEvent::id: return name;
        SNAP_LOG_EVENT_LIST(X)
#undef X
    }
    return {};
}

inline std::optional<SnapEvent> snap_event_from_name(std::string_view name)
{
#define X(id, wire_name, snaplog_enabled, projects, level_sensitive) \
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
#define X(id, name, snaplog_enabled, projects, level_sensitive) \
    case SnapEvent::id: { \
        SnapLogEventRoute route{snaplog_enabled != 0, projects}; \
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