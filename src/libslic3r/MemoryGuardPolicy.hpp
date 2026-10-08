#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <string_view>

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

// ---- Available memory, per platform (the arithmetic only; utils.cpp reads the numbers) ----------

// macOS: what host_statistics64(HOST_VM_INFO64) says the system can hand out without compressing or
// swapping. free_count already includes the speculative pages (vm_stat prints "free" as free minus
// speculative), inactive pages are reclaimed first, purgeable (volatile) pages are simply dropped.
// free_count alone, what the guard used up to 2.4.x (#642), is a few hundred MB on any busy Mac
// (macOS keeps memory in use on purpose), which made the guard fire on nearly every slice.
// Capped at the physical memory size when that is known, as the pages can overlap.
inline uint64_t macos_available_bytes(uint64_t page_size, uint64_t free_count, uint64_t inactive_count,
                                      uint64_t purgeable_count, uint64_t physical_bytes)
{
    const uint64_t bytes = (free_count + inactive_count + purgeable_count) * page_size;
    return (physical_bytes > 0 && bytes > physical_bytes) ? physical_bytes : bytes;
}

// Linux: /proc/meminfo text -> bytes. MemAvailable (kernel 3.14+) is the kernel's own estimate;
// older kernels: MemFree + Buffers + Cached + SReclaimable. 0 when nothing could be read.
inline uint64_t meminfo_available_bytes(std::string_view text)
{
    auto field_kb = [text](std::string_view key, bool& found) -> uint64_t {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string_view::npos) {
            if (pos == 0 || text[pos - 1] == '\n') {
                size_t i = pos + key.size();
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                    ++i;
                uint64_t kb = 0;
                bool     digits = false;
                for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                    kb     = kb * 10 + uint64_t(text[i] - '0');
                    digits = true;
                }
                found = digits;
                return kb;
            }
            pos += key.size();
        }
        found = false;
        return 0;
    };
    bool found = false;
    const uint64_t available = field_kb("MemAvailable:", found);
    if (found)
        return available * 1024;
    uint64_t sum = 0;
    bool     any = false;
    for (std::string_view key : { std::string_view("MemFree:"), std::string_view("Buffers:"), std::string_view("Cached:"), std::string_view("SReclaimable:") }) {
        const uint64_t kb = field_kb(key, found);
        sum += kb;
        any |= found;
    }
    return any ? sum * 1024 : 0;
}

// ---- G-code preview after the guard fired --------------------------------------------------------

// What libvgcode needs while it loads a G-code of N moves: about 1.25 vertices per move (path-start
// and actual-speed points), each 80 B on the CPU (reserved at two per move while converting) plus
// ~40 B of GPU buffers and the upload copies. A generous round figure, so a "fits" is safe.
constexpr uint64_t PREVIEW_BYTES_PER_MOVE  = 256;
// Left free for the rest of the app (and the OS) after the preview is in.
constexpr uint64_t PREVIEW_HEADROOM_BYTES = 256ULL * 1024 * 1024;

inline uint64_t preview_bytes_estimate(size_t moves) { return uint64_t(moves) * PREVIEW_BYTES_PER_MOVE; }

// Whether the full toolpath preview fits in the memory available now. available_bytes 0 means
// "could not be read": load the toolpaths (the summary is a last resort, never a default).
inline bool preview_toolpaths_fit(size_t moves, uint64_t available_bytes)
{
    if (available_bytes == 0)
        return true;
    return preview_bytes_estimate(moves) + PREVIEW_HEADROOM_BYTES <= available_bytes;
}

} // namespace Slic3r
