#pragma once

#include <atomic>

namespace SnapmakerUninstall {

/** @brief Operations needed to install or refresh one user LaunchAgent. */
enum class AgentCommand
{
    Enable,
    Bootout,
    Bootstrap
};

/** @brief Distinguishes cancellation before an update from completion and command failure. */
enum class AgentUpdateResult
{
    Cancelled,
    Registered,
    Failed
};

/**
 * @brief Updates an agent without allowing cancellation to split bootout from bootstrap.
 * @param stopping Cancellation checked before the first command only.
 * @param run Executes one command with its own timeout; must not cancel an update already in progress.
 * @return Whether the update was cancelled, registered an available agent, or failed.
 */
template <typename CommandRunner>
AgentUpdateResult update_launch_agent(bool loaded, bool needs_reload, const std::atomic<bool>& stopping,
                                     CommandRunner&& run)
{
    if (stopping.load())
    {
        return AgentUpdateResult::Cancelled;
    }
    if (run(AgentCommand::Enable) != 0)
    {
        return AgentUpdateResult::Failed;
    }
    if (loaded && !needs_reload)
    {
        return AgentUpdateResult::Registered;
    }
    if (loaded)
    {
        run(AgentCommand::Bootout);
    }
    // Always attempt bootstrap after bootout, including a cancellation request or bootout error.
    // A loaded service after a bootstrap error may still be the old program, so it is not an acknowledgement.
    if (run(AgentCommand::Bootstrap) == 0)
    {
        return AgentUpdateResult::Registered;
    }
    return AgentUpdateResult::Failed;
}

/** @brief Reasons an attempted cleanup returned instead of completing and exiting. */
enum class CleanupResult
{
    LockBusy,
    LockUnavailable,
    CommandFailed
};

/** @brief Monotonic per-kind retry limits; reaching one ends a cleanup with self-removal only. */
struct CleanupRetryPolicy
{
    unsigned max_command_failures = 3;
    unsigned max_lock_busy        = 15;
    unsigned max_lock_unavailable = 3;
};

/**
 * @brief Retries a confirmed cleanup without asking the user to confirm again.
 * @param attempt Attempts cleanup once; success exits the helper, a returned failure is retried.
 * @param wait Applies a delay and optional diagnostics before retrying the same confirmation.
 * @param terminal Runs once when a failure kind reaches its retry limit; it removes the watcher and exits.
 * @param policy Retry limits per failure kind; counters are monotonic so mixed failures still terminate.
 */
template <typename CleanupAttempt, typename RetryWait, typename TerminalCleanup>
void finish_confirmed_cleanup(CleanupAttempt&& attempt, RetryWait&& wait, TerminalCleanup&& terminal,
                              const CleanupRetryPolicy& policy = {})
{
    unsigned command_failures = 0;
    unsigned lock_busy        = 0;
    unsigned lock_unavailable = 0;
    for (;;)
    {
        const CleanupResult result = attempt();
        switch (result)
        {
        case CleanupResult::CommandFailed:
            if (++command_failures >= policy.max_command_failures)
            {
                terminal(result);
                return;
            }
            break;
        case CleanupResult::LockBusy:
            if (++lock_busy >= policy.max_lock_busy)
            {
                terminal(result);
                return;
            }
            break;
        case CleanupResult::LockUnavailable:
            if (++lock_unavailable >= policy.max_lock_unavailable)
            {
                terminal(result);
                return;
            }
            break;
        }
        wait(result);
    }
}

} // namespace SnapmakerUninstall
