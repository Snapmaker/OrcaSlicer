#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/MemoryGuardPolicy.hpp"

using namespace Slic3r;

// The slicing "Memory Usage Warning" can be switched off (dialog "Don't ask again" box or
// Preferences > General). These pin the decision table: the setting only ever silences the dialog
// for a person at the screen, never the safe stop for a headless / hub run.

TEST_CASE("Memory guard action: setting x interactive mode", "[MemoryGuard]")
{
    // Interactive: warning on -> dialog, off -> keep slicing.
    CHECK(memory_guard_action(true, true) == MemoryGuardAction::ShowDialog);
    CHECK(memory_guard_action(false, true) == MemoryGuardAction::ContinueSilently);
    // Non-interactive (RemoteAccess Request/Background): always stop, setting or not.
    CHECK(memory_guard_action(true, false) == MemoryGuardAction::Stop);
    CHECK(memory_guard_action(false, false) == MemoryGuardAction::Stop);
}

TEST_CASE("Memory guard: only Yes plus the box disables the warning", "[MemoryGuard]")
{
    CHECK(memory_guard_should_disable_warning(true, true));
    CHECK_FALSE(memory_guard_should_disable_warning(true, false));
    // "No, Stop" with the box ticked must not switch the warning off.
    CHECK_FALSE(memory_guard_should_disable_warning(false, true));
    CHECK_FALSE(memory_guard_should_disable_warning(false, false));
}

TEST_CASE("Memory guard threshold: env override", "[MemoryGuard]")
{
    const size_t def = 512ULL * 1024 * 1024;
    CHECK(memory_guard_threshold_bytes(nullptr, def) == def);
    CHECK(memory_guard_threshold_bytes("", def) == def);
    CHECK(memory_guard_threshold_bytes("1000000", def) == 1000000ULL * 1024 * 1024);
    CHECK(memory_guard_threshold_bytes("2048", def) == 2048ULL * 1024 * 1024);
    // Garbage, zero, negative or absurd values fall back to the built-in threshold.
    CHECK(memory_guard_threshold_bytes("0", def) == def);
    CHECK(memory_guard_threshold_bytes("-5", def) == def);
    CHECK(memory_guard_threshold_bytes("abc", def) == def);
    CHECK(memory_guard_threshold_bytes("12abc", def) == def);
    CHECK(memory_guard_threshold_bytes("99999999999999999999", def) == def);
}

TEST_CASE("Memory guard warning defaults to on", "[MemoryGuard]")
{
    AppConfig cfg;
    cfg.set_defaults();
    CHECK(cfg.get_bool(MEMORY_GUARD_WARN_CONFIG_KEY));
    // An explicit "off" survives set_defaults (it only fills missing keys).
    cfg.set_bool(MEMORY_GUARD_WARN_CONFIG_KEY, false);
    cfg.set_defaults();
    CHECK_FALSE(cfg.get_bool(MEMORY_GUARD_WARN_CONFIG_KEY));
}

// libvgcode stage 3 Mac test: the guard fired on every slice of an 8 GB Mac with half its memory
// free, because macOS "available" was read as free pages only. These pin the arithmetic.
TEST_CASE("Memory guard: macOS available memory counts free, inactive and purgeable pages", "[MemoryGuard]")
{
    const uint64_t page = 16384; // Apple silicon page size
    const uint64_t mb   = 1024 * 1024;
    // The reported case: ~190 MB free pages, but several GB inactive (memory_pressure: 51% free).
    const uint64_t free_pages      = 190 * mb / page;
    const uint64_t inactive_pages  = 3600 * mb / page;
    const uint64_t purgeable_pages = 120 * mb / page;
    const uint64_t avail = macos_available_bytes(page, free_pages, inactive_pages, purgeable_pages, 8192 * mb);
    CHECK(avail == (190 + 3600 + 120) * mb);
    CHECK(avail > 512 * mb); // the guard's threshold: no longer "low"
    // A genuinely full machine: little free, little inactive.
    CHECK(macos_available_bytes(page, 40 * mb / page, 100 * mb / page, 0, 8192 * mb) == 140 * mb);
    // Overlapping counts never report more than the machine has.
    CHECK(macos_available_bytes(page, 6000 * mb / page, 6000 * mb / page, 0, 8192 * mb) == 8192 * mb);
    // Physical size unknown: no cap.
    CHECK(macos_available_bytes(4096, 10, 20, 30, 0) == 60 * 4096);
}

TEST_CASE("Memory guard: Linux available memory from /proc/meminfo", "[MemoryGuard]")
{
    const char* modern = "MemTotal:       16303316 kB\nMemFree:          512000 kB\nMemAvailable:    8000000 kB\n"
                         "Buffers:          100000 kB\nCached:          4000000 kB\nSwapCached:            0 kB\n";
    CHECK(meminfo_available_bytes(modern) == 8000000ULL * 1024);
    // Before kernel 3.14 there is no MemAvailable: free + buffers + cache + reclaimable slab.
    // SwapCached must not be read as Cached.
    const char* old = "MemTotal:       16303316 kB\nMemFree:          512000 kB\nBuffers:          100000 kB\n"
                      "SwapCached:        77777 kB\nCached:          4000000 kB\nSReclaimable:     200000 kB\n";
    CHECK(meminfo_available_bytes(old) == (512000ULL + 100000 + 4000000 + 200000) * 1024);
    CHECK(meminfo_available_bytes("") == 0);
    CHECK(meminfo_available_bytes("garbage") == 0);
}

TEST_CASE("Memory guard: the libvgcode preview loads toolpaths unless they would not fit", "[MemoryGuard]")
{
    const uint64_t mb = 1024 * 1024;
    // The Mac case after the fix: 740k moves need ~181 MB, GBs are available.
    CHECK(preview_toolpaths_fit(740581, 3900 * mb));
    // Unknown available memory never falls back to the summary.
    CHECK(preview_toolpaths_fit(50000000, 0));
    // Truly low: 145 MB available cannot take 740k moves plus the headroom.
    CHECK_FALSE(preview_toolpaths_fit(740581, 145 * mb));
    // The boundary: estimate + headroom.
    const size_t moves = 1000000;
    CHECK(preview_toolpaths_fit(moves, preview_bytes_estimate(moves) + PREVIEW_HEADROOM_BYTES));
    CHECK_FALSE(preview_toolpaths_fit(moves, preview_bytes_estimate(moves) + PREVIEW_HEADROOM_BYTES - 1));
}
