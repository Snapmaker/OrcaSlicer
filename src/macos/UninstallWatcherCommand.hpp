#pragma once

#include <chrono>

#ifndef _WIN32
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <syslog.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
#endif

namespace SnapmakerUninstall
{

/** @brief Separates normal completion from a timeout and a child still awaiting collection. */
enum class CommandWaitResult
{
    Completed,
    TimedOut,
    Unreaped
};

/** @brief Polls child state and bounded, nonblocking output reads until completion or the deadline. */
template<typename Process> bool collect_command_until(Process& process, std::chrono::steady_clock::time_point deadline)
{
    for (;;)
    {
        // Output consumption must itself be bounded, including for a continuously writing child.
        process.read_output();
        if (process.collect())
        {
            // Drain the final available bytes without waiting for inherited pipe writers to close.
            process.read_output();
            return true;
        }
        if (process.now() >= deadline)
        {
            return false;
        }
        process.pause();
    }
}

/**
 * @brief Bounds command execution and both termination phases; zero timeout permits an interactive dialog.
 * @param process Supplies nonblocking child collection, output, signals and a monotonic clock.
 * @param timeout Execution budget; only interactive commands should use zero.
 * @return Unreaped requires retaining exclusive cleanup ownership until the child is collected.
 */
template<typename Process> CommandWaitResult wait_for_command(Process& process, std::chrono::milliseconds timeout)
{
    const std::chrono::steady_clock::time_point deadline = timeout.count() == 0 ?
                                                               std::chrono::steady_clock::time_point::max() :
                                                               process.now() + timeout;
    if (collect_command_until(process, deadline))
    {
        return CommandWaitResult::Completed;
    }
    process.terminate();
    if (collect_command_until(process, process.now() + std::chrono::milliseconds(200)))
    {
        return CommandWaitResult::TimedOut;
    }
    process.kill();
    return collect_command_until(process, process.now() + std::chrono::milliseconds(200)) ?
               CommandWaitResult::TimedOut :
               CommandWaitResult::Unreaped;
}

#ifndef _WIN32
/** @brief Carries child exit status, bounded diagnostic output and timeout acknowledgement. */
struct CommandResult
{
    int         exit_code = -1;
    std::string output;
    bool        timed_out = false;
};

/** @brief Defers watcher termination while publishing or clearing the owned child PID. */
struct CommandSignalBlock
{
    sigset_t previous{};
    bool     active                                          = false;
    CommandSignalBlock(const CommandSignalBlock&)            = delete;
    CommandSignalBlock& operator=(const CommandSignalBlock&) = delete;

    /** @brief Blocks SIGTERM until the owned PID and its collection state agree. */
    CommandSignalBlock()
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        active = ::sigprocmask(SIG_BLOCK, &blocked, &previous) == 0;
    }
    /** @brief Delivers pending termination only after the PID has been updated. */
    ~CommandSignalBlock()
    {
        if (active)
        {
            ::sigprocmask(SIG_SETMASK, &previous, nullptr);
        }
    }
};

/** @brief Releases pipes and initialized spawn resources on every command setup failure. */
struct CommandResources
{
    std::array<int, 2>         pipefd{{-1, -1}};
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t          attributes{};
    bool                       actions_ready             = false;
    bool                       attributes_ready          = false;
    CommandResources()                                   = default;
    CommandResources(const CommandResources&)            = delete;
    CommandResources& operator=(const CommandResources&) = delete;

    /** @brief Initializes checked spawn actions and nonblocking, close-on-exec output capture. */
    bool prepare(bool capture)
    {
        actions_ready    = posix_spawn_file_actions_init(&actions) == 0;
        attributes_ready = posix_spawnattr_init(&attributes) == 0;
        if (!actions_ready || !attributes_ready)
        {
            return false;
        }
        if (!capture)
        {
            return posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                   posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
        }
        return ::pipe(pipefd.data()) == 0 && ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK) == 0 &&
               ::fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == 0 &&
               posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO) == 0 &&
               posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO) == 0 &&
               posix_spawn_file_actions_addclose(&actions, pipefd[0]) == 0 &&
               posix_spawn_file_actions_addclose(&actions, pipefd[1]) == 0;
    }
    /** @brief Closes a pipe endpoint once. */
    void close_pipe(std::size_t index)
    {
        if (pipefd[index] >= 0)
        {
            ::close(pipefd[index]);
            pipefd[index] = -1;
        }
    }
    /** @brief Disposes initialized resources in reverse order. */
    ~CommandResources()
    {
        if (attributes_ready)
        {
            posix_spawnattr_destroy(&attributes);
        }
        if (actions_ready)
        {
            posix_spawn_file_actions_destroy(&actions);
        }
        close_pipe(0);
        close_pipe(1);
    }
};

/** @brief Adapts POSIX child operations to the shared, deterministically tested timeout policy. */
struct CommandProcess
{
    pid_t                  pid;
    int                    output_fd;
    CommandResult&         result;
    volatile sig_atomic_t& running_child;

    /** @brief Collects the owned child without waiting; interruptions are retried by the policy. */
    bool collect()
    {
        const CommandSignalBlock blocked;
        if (!blocked.active)
        {
            return false;
        }
        int         status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid)
        {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            running_child    = 0;
            return true;
        }
        if (waited < 0 && errno == ECHILD)
        {
            running_child = 0;
            return true;
        }
        return false;
    }

    /** @brief Reads at most 64 KiB per poll and retains at most 64 KiB of diagnostic output. */
    void read_output()
    {
        if (output_fd < 0)
        {
            return;
        }
        std::array<char, 4096> buffer{};
        constexpr std::size_t  kMaxOutput = 64 * 1024;
        for (unsigned chunk = 0; chunk < 16; ++chunk)
        {
            const ssize_t count = ::read(output_fd, buffer.data(), buffer.size());
            if (count <= 0)
            {
                break;
            }
            const std::size_t size = std::min(static_cast<std::size_t>(count), kMaxOutput - result.output.size());
            result.output.append(buffer.data(), size);
        }
    }

    /** @brief Returns the monotonic clock used for all deadlines. */
    std::chrono::steady_clock::time_point now() const
    {
        return std::chrono::steady_clock::now();
    }
    /** @brief Avoids a busy loop while polling child state. */
    void pause() const
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    /** @brief Requests graceful termination before escalation. */
    void terminate() const
    {
        ::kill(pid, SIGTERM);
    }
    /** @brief Prevents a timed-out command from continuing user-space work. */
    void kill() const
    {
        ::kill(pid, SIGKILL);
    }
};

/** @brief Runs a command with a three-second budget unless explicitly marked as interactive. */
inline CommandResult run_watcher_command(const std::vector<std::string>& args,
                                         bool                            capture,
                                         volatile sig_atomic_t&          running_child,
                                         bool                            interactive = false)
{
    CommandResult result;
    if (args.empty())
        return result;

    CommandResources resources;
    if (!resources.prepare(capture))
    {
        return result;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    {
        const CommandSignalBlock blocked;
        sigset_t                 defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGTERM);
        // Cleanup ignores SIGTERM in the watcher, but spawned commands still get a graceful stop.
        if (!blocked.active || posix_spawnattr_setsigmask(&resources.attributes, &blocked.previous) != 0 ||
            posix_spawnattr_setsigdefault(&resources.attributes, &defaults) != 0 ||
            posix_spawnattr_setflags(&resources.attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) != 0 ||
            posix_spawn(&pid, argv[0], &resources.actions, &resources.attributes, argv.data(), environ) != 0)
        {
            return result;
        }
        running_child = static_cast<sig_atomic_t>(pid);
    }
    resources.close_pipe(1);

    CommandProcess                              process{pid, resources.pipefd[0], result, running_child};
    const SnapmakerUninstall::CommandWaitResult waited =
        SnapmakerUninstall::wait_for_command(process, interactive ? std::chrono::milliseconds(0) :
                                                                    std::chrono::milliseconds(3000));
    result.timed_out = waited != SnapmakerUninstall::CommandWaitResult::Completed;
    if (result.timed_out)
    {
        syslog(LOG_ERR, "Snapmaker Orca Monitor: command timed out: %s (pid %ld)", args.front().c_str(),
               static_cast<long>(pid));
    }
    if (waited == SnapmakerUninstall::CommandWaitResult::Unreaped)
    {
        // A kernel-stalled child cannot safely overlap a new registration. Retain the lifecycle
        // lock and stop cleanup until SIGKILL takes effect; never advance to another command.
        syslog(LOG_ERR, "Snapmaker Orca Monitor: retaining cleanup ownership for unreaped pid %ld",
               static_cast<long>(pid));
        while (!process.collect())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (result.timed_out)
    {
        result.exit_code = -1;
    }
    return result;
}

#endif

} // namespace SnapmakerUninstall
