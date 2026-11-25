#include "macos/UninstallWatcherNames.hpp"
#ifdef __APPLE__
#include "macos/UninstallWatcherLock.hpp"
#endif

#include "macos/UninstallWatcherState.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

/** @brief Reports a failed behavioral check without requiring the application's test dependencies. */
bool check(bool result, const char* description)
{
    if (!result)
    {
        std::cerr << "FAILED: " << description << '\n';
    }
    return result;
}

/** @brief Sets HOME only in this test process. */
bool set_test_home(const char* value)
{
#ifdef _WIN32
    return ::_putenv_s("HOME", value == nullptr ? "" : value) == 0;
#else
    return value == nullptr ? ::unsetenv("HOME") == 0 : ::setenv("HOME", value, 1) == 0;
#endif
}

/** @brief Owns a fresh temporary HOME so checks cannot access real application data. */
class TestHome
{
public:
    /** @brief Creates an isolated home directory, preserving the original environment. */
    TestHome()
    {
        const char* previous = std::getenv("HOME");
        if (previous != nullptr)
        {
            previous_ = previous;
        }
        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        if (ec.value() != 0)
        {
            return;
        }
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = temp / ("orca-watcher-lifecycle-test-" + std::to_string(stamp));
        owns_root_ = std::filesystem::create_directory(root_, ec);
        if (!owns_root_ || ec.value() != 0)
        {
            return;
        }
        ready_ = set_test_home(root_.string().c_str());
    }

    /** @brief Restores HOME and removes only the directory successfully created by this instance. */
    ~TestHome()
    {
        if (ready_)
        {
            set_test_home(previous_.has_value() ? previous_->c_str() : nullptr);
        }
        if (owns_root_)
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }
    }

    TestHome(const TestHome&) = delete;
    TestHome& operator=(const TestHome&) = delete;
    TestHome(TestHome&&) = delete;
    TestHome& operator=(TestHome&&) = delete;

    /** @brief Reports whether the environment was successfully isolated. */
    bool ready() const { return ready_; }

private:
    std::optional<std::string> previous_;
    std::filesystem::path root_;
    bool owns_root_ = false;
    bool ready_ = false;
};

/** @brief Checks that personal-data cleanup preserves coordination files and excludes their directory. */
bool test_cleanup_boundary()
{
    namespace fs = std::filesystem;
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    std::error_code ec;
    fs::create_directories(SnapmakerUninstall::helper_dir(), ec);
    if (!check(ec.value() == 0, "create simulated helper data"))
    {
        return false;
    }
    fs::create_directories(SnapmakerUninstall::lifecycle_dir(), ec);
    if (!check(ec.value() == 0, "create coordination directory"))
    {
        return false;
    }
    const fs::path lock_path = SnapmakerUninstall::lifecycle_dir() / "lifecycle.lock";
    std::ofstream(lock_path).close();
    bool ok = check(SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::support_dir()),
                    "remove personal-data tree");
    ok = check(!fs::exists(SnapmakerUninstall::helper_dir(), ec), "helper data removed") && ok;
    ok = check(fs::is_regular_file(lock_path, ec), "coordination file survives cleanup") && ok;
    ok = check(!SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::lifecycle_dir()),
               "coordination directory excluded from cleanup whitelist") && ok;
    return ok;
}

/** @brief Checks failed reload intent survives destruction and forces refresh despite matching disk files. */
bool test_persistent_reload_recovery()
{
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(SnapmakerUninstall::helper_dir(), ec);
    if (!check(ec.value() == 0, "create helper directory"))
    {
        return false;
    }
    using SnapmakerUninstall::AgentCommand;
    using SnapmakerUninstall::AgentUpdateResult;
    const std::atomic<bool> stopping{false};
    bool ok = true;
    {
        SnapmakerUninstall::PendingReload first;
        if (!check(first.valid() && !first.required() && first.require(), "persist intent before changing files"))
        {
            return false;
        }
        const AgentUpdateResult failed = SnapmakerUninstall::update_launch_agent(
            true, true, stopping, [](AgentCommand) { return -1; });
        ok = check(!first.complete(failed), "failed registration cannot clear reload intent") && ok;
    }
    SnapmakerUninstall::PendingReload restarted;
    ok = check(restarted.valid() && restarted.required() && restarted.require(),
               "another attempt retains reload intent even when disk comparisons match") && ok;
    std::vector<AgentCommand> commands;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, restarted.required(), stopping,
        [&commands](AgentCommand command) { commands.push_back(command); return 0; });
    const std::vector<AgentCommand> expected{AgentCommand::Enable, AgentCommand::Bootout, AgentCommand::Bootstrap};
    ok = check(commands == expected && restarted.complete(result), "pending marker forces refresh and clears on success") && ok;
    const SnapmakerUninstall::PendingReload completed;
    ok = check(completed.valid() && !completed.required(), "successful refresh is acknowledged on disk") && ok;
    return ok;
}

/** @brief Checks an invalid marker is rejected rather than mistaken for an acknowledged refresh. */
bool test_invalid_reload_marker()
{
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(SnapmakerUninstall::helper_dir() / ".reload.pending", ec);
    if (!check(ec.value() == 0, "create invalid marker fixture"))
    {
        return false;
    }
    SnapmakerUninstall::PendingReload marker;
    return check(!marker.valid() && !marker.require() &&
                 !marker.complete(SnapmakerUninstall::AgentUpdateResult::Registered),
                 "invalid reload marker fails closed");
}

/** @brief Checks UUID state round-trips and superseded attempts cannot republish older identities. */
bool test_registration_identity()
{
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    const std::filesystem::path bundle = SnapmakerUninstall::home_path() / "Applications" /
                                         SnapmakerUninstall::kAppBundleName;
    const std::string first = "11111111-1111-4111-8111-111111111111";
    const std::string second = "22222222-2222-4222-8222-222222222222";
    const std::string first_text = SnapmakerUninstall::watch_state_text(bundle, first);
    const std::string second_text = SnapmakerUninstall::watch_state_text(bundle, second);
    bool ok = check(SnapmakerUninstall::registration_id_from_state(first_text) == first &&
                    SnapmakerUninstall::registration_id_from_state(second_text) == second,
                    "registration UUID round-trips through production state serialization");
    ok = check(SnapmakerUninstall::registration_attempt_is_current("", first) &&
               SnapmakerUninstall::registration_attempt_is_current(first, first) &&
               !SnapmakerUninstall::registration_attempt_is_current(first, second) &&
               !SnapmakerUninstall::registration_attempt_is_current(first, ""),
               "an older retry cannot restore its UUID after another registration or cleanup") && ok;
    ok = check(SnapmakerUninstall::registration_id_from_state("{\"seen_bundle\":true}").empty() &&
               SnapmakerUninstall::registration_id_from_state("{\"registration_id\":\"invalid\"}").empty() &&
               SnapmakerUninstall::watch_state_text(bundle, "invalid").empty(),
               "missing or malformed registration identity is rejected") && ok;
    return ok;
}

/**
 * @brief Checks the confirmed-cleanup retry policy consumes no watch.json state: rewriting the registration
 * UUID mid-retry changes neither the retry count nor the terminal result.
 */
bool test_retry_policy_ignores_watch_json_rewrites()
{
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    const std::filesystem::path bundle = SnapmakerUninstall::home_path() / "Applications" /
                                         SnapmakerUninstall::kAppBundleName;
    const std::string first = "11111111-1111-4111-8111-111111111111";
    const std::string second = "22222222-2222-4222-8222-222222222222";
    std::error_code ec;
    std::filesystem::create_directories(SnapmakerUninstall::helper_dir(), ec);
    const std::filesystem::path state = SnapmakerUninstall::watch_state_path();
    if (!check(ec.value() == 0 && SnapmakerUninstall::write_text_atomic(
                   state, SnapmakerUninstall::watch_state_text(bundle, first)), "publish initial registration"))
    {
        return false;
    }
    unsigned attempts = 0;
    unsigned waits = 0;
    bool replaced = false;
    unsigned terminal_calls = 0;
    SnapmakerUninstall::CleanupResult terminal_result = SnapmakerUninstall::CleanupResult::CommandFailed;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]()
        {
            ++attempts;
            return attempts <= 4 ? SnapmakerUninstall::CleanupResult::LockBusy
                                 : SnapmakerUninstall::CleanupResult::LockUnavailable;
        },
        [&](SnapmakerUninstall::CleanupResult)
        {
            ++waits;
            // A reinstall publishes a new UUID while the confirmed cleanup keeps retrying.
            if (waits == 2)
            {
                replaced = SnapmakerUninstall::write_text_atomic(
                    state, SnapmakerUninstall::watch_state_text(bundle, second));
            }
        },
        [&terminal_calls, &terminal_result](SnapmakerUninstall::CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(replaced && attempts == 7 && waits == 6 && terminal_calls == 1 &&
                 terminal_result == SnapmakerUninstall::CleanupResult::LockUnavailable,
                 "retry policy is insensitive to watch.json rewrites: mixed failures still reach the lock-unavailable limit");
}

#ifdef __APPLE__
/** @brief Exercises actual kernel locking, contention classification, cleanup isolation, and release. */
bool test_lock_lifecycle()
{
    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    bool ok = true;
    {
        const SnapmakerUninstall::LifecycleLock first(Clock::now() + std::chrono::milliseconds(50));
        if (!check(first.locked(), "first lock creates missing parent directories and acquires ownership"))
        {
            return false;
        }
        std::error_code ec;
        fs::create_directories(SnapmakerUninstall::helper_dir(), ec);
        ok = check(ec.value() == 0, "create simulated helper data") && ok;
        ok = check(SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::support_dir()),
                   "clean user data while holding lifecycle lock") && ok;
        const SnapmakerUninstall::LifecycleLock second(Clock::now() + std::chrono::milliseconds(50));
        ok = check(!second.locked() && second.timed_out(), "cleanup preserves exclusive lock identity") && ok;
    }
    const SnapmakerUninstall::LifecycleLock third(Clock::now() + std::chrono::milliseconds(50));
    ok = check(third.locked(), "lock is available after owner destruction") && ok;
    return ok;
}

/** @brief Confirms a permanent path error will not be mistaken for retryable contention. */
bool test_invalid_lock_directory()
{
    namespace fs = std::filesystem;
    const TestHome home;
    if (!check(home.ready(), "temporary HOME setup"))
    {
        return false;
    }
    std::error_code ec;
    fs::create_directories(SnapmakerUninstall::lifecycle_dir().parent_path(), ec);
    if (!check(ec.value() == 0, "create application support"))
    {
        return false;
    }
    std::ofstream(SnapmakerUninstall::lifecycle_dir()).close();
    const SnapmakerUninstall::LifecycleLock lock(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    return check(!lock.locked() && !lock.timed_out(), "invalid parent is a permanent error");
}
#endif

} // namespace

/** @brief Runs isolated filesystem checks and, on macOS, native lock behavior checks. */
int main()
{
    bool ok = test_cleanup_boundary();
    ok = test_persistent_reload_recovery() && ok;
    ok = test_invalid_reload_marker() && ok;
    ok = test_registration_identity() && ok;
    ok = test_retry_policy_ignores_watch_json_rewrites() && ok;
#ifdef __APPLE__
    ok = test_lock_lifecycle() && ok;
    ok = test_invalid_lock_directory() && ok;
#else
    std::cout << "Native macOS lock checks skipped on this platform.\n";
#endif
    if (ok)
    {
        std::cout << "Uninstall watcher lifecycle checks passed.\n";
    }
    return ok ? 0 : 1;
}
