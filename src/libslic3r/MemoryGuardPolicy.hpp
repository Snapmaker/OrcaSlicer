#pragma once

#include <cstddef>
#include <cstdlib>
#include <cerrno>

// Pure helpers for the slicing "Memory Usage Warning" (PrintBase::check_memory_guard and the
// callback in Plater.cpp). Kept free of wx / app state so the decisions can be unit-tested.

namespace Slic3r {

// app_config key behind the dialog's "Don't ask again" box and the Preferences checkbox.
// Absent or true: warn; false: keep slicing and only log + show a non-modal notice.
constexpr const char* MEMORY_GUARD_WARN_CONFIG_KEY = "warn_low_memory_slicing";

// Debug override: when set to a positive number of MB, available memory below that many MB counts
// as low. Setting it very high (e.g. 1000000) forces the guard to fire a second or so into any
// slice, so the dialog / silenced path can be tested on a machine with plenty of memory.
// Unset (the normal case) leaves the built-in threshold alone.
constexpr const char* MEMORY_GUARD_FORCE_MB_ENV = "EDGESLICER_MEM_GUARD_FORCE_MB";

enum class MemoryGuardAction {
    ShowDialog,        // interactive and warnings on: ask "Yes, Continue / No, Stop"
    ContinueSilently,  // interactive and warnings off: log, notify, keep slicing
    Stop,              // nobody can answer (hub / RemoteAccess): stop the slice safely
};

// What the guard does when memory is low.
//   warn_enabled: the app_config key above
//   interactive : a person can answer a dialog (RemoteAccess::Mode::Interactive)
// The silenced path applies only to a person at the screen: a headless/hub run cannot answer and
// is never silenced into "continue", it still stops.
inline MemoryGuardAction memory_guard_action(bool warn_enabled, bool interactive)
{
    if (!interactive)
        return MemoryGuardAction::Stop;
    return warn_enabled ? MemoryGuardAction::ShowDialog : MemoryGuardAction::ContinueSilently;
}

// Whether answering the dialog should switch the warning off for good: only "Yes, Continue" with
// the "Don't ask again" box ticked. "No, Stop" never disables the warning, whatever the box says.
inline bool memory_guard_should_disable_warning(bool answered_yes, bool dont_ask_checked)
{
    return answered_yes && dont_ask_checked;
}

// Threshold in bytes: default_bytes unless env_value (the raw EDGESLICER_MEM_GUARD_FORCE_MB text,
// may be null) holds a positive integer number of MB.
inline size_t memory_guard_threshold_bytes(const char* env_value, size_t default_bytes)
{
    if (env_value == nullptr || *env_value == '\0')
        return default_bytes;
    char*              end = nullptr;
    errno                  = 0;
    const unsigned long long mb = std::strtoull(env_value, &end, 10);
    if (errno != 0 || end == env_value || *end != '\0' || mb == 0 || mb > (1ULL << 40))
        return default_bytes;
    return static_cast<size_t>(mb) * 1024ULL * 1024ULL;
}

} // namespace Slic3r
