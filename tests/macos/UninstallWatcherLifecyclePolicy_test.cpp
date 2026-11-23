#include "macos/UninstallWatcherLifecycle.hpp"

#include <atomic>
#include <iostream>
#include <vector>

namespace {

using SnapmakerUninstall::AgentCommand;
using SnapmakerUninstall::AgentUpdateResult;
using SnapmakerUninstall::CleanupResult;

/** @brief Reports a failed policy check. */
bool check(bool result, const char* description)
{
    if (!result)
    {
        std::cerr << "FAILED: " << description << '\n';
    }
    return result;
}

/** @brief Checks cancellation before any command preserves the existing agent. */
bool test_cancel_before_update()
{
    const std::atomic<bool> stopping{true};
    unsigned calls = 0;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, true, stopping, [&calls](AgentCommand) { ++calls; return 0; });
    return check(result == AgentUpdateResult::Cancelled && calls == 0,
                 "cancellation before update runs no commands");
}

/** @brief Checks shutdown during initial registration or reload cannot suppress bootstrap. */
bool test_stop_during_update(bool loaded)
{
    std::atomic<bool> stopping{false};
    std::vector<AgentCommand> calls;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        loaded, true, stopping,
        [&calls, &stopping, loaded](AgentCommand command)
        {
            calls.push_back(command);
            if (command == (loaded ? AgentCommand::Bootout : AgentCommand::Enable))
            {
                stopping.store(true);
            }
            return 0;
        });
    const std::vector<AgentCommand> expected = loaded
        ? std::vector<AgentCommand>{AgentCommand::Enable, AgentCommand::Bootout, AgentCommand::Bootstrap}
        : std::vector<AgentCommand>{AgentCommand::Enable, AgentCommand::Bootstrap};
    return check(result == AgentUpdateResult::Registered && stopping.load() && calls == expected,
                 "bootstrap completes after a shutdown request during update");
}

/** @brief Checks enable failures do not remove a running agent. */
bool test_enable_failure()
{
    const std::atomic<bool> stopping{false};
    std::vector<AgentCommand> calls;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, true, stopping,
        [&calls](AgentCommand command) { calls.push_back(command); return -1; });
    return check(result == AgentUpdateResult::Failed && calls == std::vector<AgentCommand>{AgentCommand::Enable},
                 "enable failure preserves the running agent");
}

/** @brief Checks a bootout error still attempts a fresh bootstrap. */
bool test_update_failure_recovery()
{
    const std::atomic<bool> stopping{false};
    std::vector<AgentCommand> calls;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, true, stopping,
        [&calls](AgentCommand command)
        {
            calls.push_back(command);
            return command == AgentCommand::Bootout ? -1 : 0;
        });
    const std::vector<AgentCommand> expected{
        AgentCommand::Enable, AgentCommand::Bootout, AgentCommand::Bootstrap};
    return check(result == AgentUpdateResult::Registered && calls == expected,
                 "bootout failure does not skip bootstrap");
}

/** @brief Checks bootstrap failure is not treated as success even if an old service might remain loaded. */
bool test_update_unrecoverable_failure()
{
    const std::atomic<bool> stopping{false};
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, true, stopping,
        [](AgentCommand command)
        {
            return command == AgentCommand::Enable || command == AgentCommand::Bootout ? 0 : -1;
        });
    return check(result == AgentUpdateResult::Failed, "unavailable service is not reported as registered");
}

/** @brief Checks an unchanged agent is enabled without unnecessary teardown. */
bool test_unchanged_agent()
{
    const std::atomic<bool> stopping{false};
    std::vector<AgentCommand> calls;
    const AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        true, false, stopping,
        [&calls](AgentCommand command) { calls.push_back(command); return 0; });
    return check(result == AgentUpdateResult::Registered && calls == std::vector<AgentCommand>{AgentCommand::Enable},
                 "unchanged agent is not unloaded");
}

/** @brief Checks repeated lock errors reuse the same cleanup attempt until installation state changes. */
bool test_confirmed_cleanup_retries()
{
    const std::vector<CleanupResult> results{CleanupResult::LockBusy, CleanupResult::LockUnavailable,
                                             CleanupResult::CommandFailed, CleanupResult::LockBusy,
                                             CleanupResult::InstallationChanged};
    std::size_t attempts = 0;
    std::vector<CleanupResult> waits;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&results, &attempts]()
        {
            const std::size_t index = attempts++;
            return index < results.size() ? results[index] : CleanupResult::InstallationChanged;
        },
        [&waits](CleanupResult result) { waits.push_back(result); });
    return check(attempts == results.size() && waits == std::vector<CleanupResult>(results.begin(), results.end() - 1),
                 "one confirmed cleanup retries lock and command failures, then stops on installation change");
}

/** @brief Checks a restored installation cancels the pending cleanup without another retry. */
bool test_cleanup_cancelled_by_installation_change()
{
    unsigned attempts = 0;
    unsigned waits = 0;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]() { ++attempts; return CleanupResult::InstallationChanged; },
        [&waits](CleanupResult) { ++waits; });
    return check(attempts == 1 && waits == 0, "restored installation cancels pending cleanup immediately");
}

} // namespace

/** @brief Runs production lifecycle policy against deterministic command and retry failures. */
int main()
{
    bool ok = test_cancel_before_update();
    ok = test_stop_during_update(false) && ok;
    ok = test_stop_during_update(true) && ok;
    ok = test_enable_failure() && ok;
    ok = test_update_failure_recovery() && ok;
    ok = test_update_unrecoverable_failure() && ok;
    ok = test_unchanged_agent() && ok;
    ok = test_confirmed_cleanup_retries() && ok;
    ok = test_cleanup_cancelled_by_installation_change() && ok;
    if (ok)
    {
        std::cout << "All nine uninstall watcher lifecycle policy cases passed.\n";
    }
    return ok ? 0 : 1;
}
