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
