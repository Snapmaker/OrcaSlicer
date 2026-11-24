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

/** @brief Checks a confirmed cleanup stops after the command-failure retry limit without deleting. */
bool test_command_failure_cap()
{
    unsigned attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::LockBusy;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]() { ++attempts; return CleanupResult::CommandFailed; },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(attempts == 3 && waits == 2 && terminal_calls == 1 && terminal_result == CleanupResult::CommandFailed,
                 "command failures reach the limit and end in one terminal self-removal");
}

/** @brief Checks a confirmed cleanup stops after the lock-busy retry limit without deleting. */
bool test_lock_busy_cap()
{
    unsigned attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::CommandFailed;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]() { ++attempts; return CleanupResult::LockBusy; },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(attempts == 15 && waits == 14 && terminal_calls == 1 && terminal_result == CleanupResult::LockBusy,
                 "busy locks reach the limit and end in one terminal self-removal");
}

/** @brief Checks a confirmed cleanup stops after the lock-unavailable retry limit without deleting. */
bool test_lock_unavailable_cap()
{
    unsigned attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::LockBusy;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]() { ++attempts; return CleanupResult::LockUnavailable; },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(attempts == 3 && waits == 2 && terminal_calls == 1 && terminal_result == CleanupResult::LockUnavailable,
                 "unavailable locks reach the limit and end in one terminal self-removal");
}

/** @brief Checks each retry counter is monotonic: interleaved failures still terminate. */
bool test_mixed_failures_are_monotonic()
{
    const std::vector<CleanupResult> results{CleanupResult::CommandFailed, CleanupResult::LockBusy,
                                             CleanupResult::CommandFailed, CleanupResult::LockBusy,
                                             CleanupResult::CommandFailed};
    std::size_t attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::LockBusy;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&results, &attempts]()
        {
            const std::size_t index = attempts++;
            return index < results.size() ? results[index] : CleanupResult::CommandFailed;
        },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(attempts == results.size() && waits == results.size() - 1 && terminal_calls == 1 &&
                 terminal_result == CleanupResult::CommandFailed,
                 "mixed failures accumulate per kind and stop at the command-failure limit on the fifth attempt");
}

/** @brief Checks interleaved unavailable locks and command failures keep separate retry counters. */
bool test_mixed_lu_cf_are_monotonic()
{
    const std::vector<CleanupResult> results{CleanupResult::LockUnavailable, CleanupResult::CommandFailed,
                                             CleanupResult::LockUnavailable, CleanupResult::CommandFailed,
                                             CleanupResult::LockUnavailable};
    std::size_t attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::CommandFailed;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&results, &attempts]()
        {
            const std::size_t index = attempts++;
            return index < results.size() ? results[index] : CleanupResult::CommandFailed;
        },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        });
    return check(attempts == 5 && waits == 4 && terminal_calls == 1 &&
                 terminal_result == CleanupResult::LockUnavailable,
                 "interleaved command failures do not reset the unavailable-lock counter");
}

/** @brief Checks a custom policy binds each failure kind to its own retry limit. */
bool test_custom_policy_command_cap()
{
    unsigned attempts = 0;
    unsigned waits = 0;
    unsigned terminal_calls = 0;
    CleanupResult terminal_result = CleanupResult::LockBusy;
    SnapmakerUninstall::CleanupRetryPolicy policy;
    policy.max_command_failures = 2;
    policy.max_lock_busy        = 15;
    policy.max_lock_unavailable = 3;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [&attempts]() { ++attempts; return CleanupResult::CommandFailed; },
        [&waits](CleanupResult) { ++waits; },
        [&terminal_calls, &terminal_result](CleanupResult result)
        {
            ++terminal_calls;
            terminal_result = result;
        },
        policy);
    return check(attempts == 2 && waits == 1 && terminal_calls == 1 &&
                 terminal_result == CleanupResult::CommandFailed,
                 "a custom command-failure limit stops the second attempt instead of the lock-unavailable limit");
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
    ok = test_command_failure_cap() && ok;
    ok = test_lock_busy_cap() && ok;
    ok = test_lock_unavailable_cap() && ok;
    ok = test_mixed_failures_are_monotonic() && ok;
    ok = test_mixed_lu_cf_are_monotonic() && ok;
    ok = test_custom_policy_command_cap() && ok;
    if (ok)
    {
        std::cout << "All thirteen uninstall watcher lifecycle policy cases passed.\n";
    }
    return ok ? 0 : 1;
}
