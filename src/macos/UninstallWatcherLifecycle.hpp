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
    InstallationChanged,
    LockBusy,
    LockUnavailable
};

/**
 * @brief Retries a confirmed cleanup without asking the user to confirm again.
 * @param attempt Rechecks installation state and attempts cleanup; success exits the helper.
 * @param wait Applies a delay and optional diagnostics before retrying the same confirmation.
 */
template <typename CleanupAttempt, typename RetryWait>
void finish_confirmed_cleanup(CleanupAttempt&& attempt, RetryWait&& wait)
{
    for (;;)
    {
        const CleanupResult result = attempt();
        if (result == CleanupResult::InstallationChanged)
        {
            return;
        }
        wait(result);
    }
}

} // namespace SnapmakerUninstall
