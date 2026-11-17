#include "macos/UninstallWatcherCommand.hpp"

#include <iostream>
#include <string>

namespace
{

using Clock        = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using SnapmakerUninstall::CommandWaitResult;

/** @brief Models child completion and signal handling without wall-clock sleeps. */
struct FakeProcess
{
    Clock::time_point current{};
    Clock::time_point exit_at       = Clock::time_point::max();
    bool              exits_on_term = false;
    bool              exits_on_kill = true;
    unsigned          terms         = 0;
    unsigned          kills         = 0;
    unsigned          reads         = 0;

    /** @brief Models an output provider that never reports EOF. */
    void read_output()
    {
        ++reads;
    }
    /** @brief Models nonblocking child collection. */
    bool collect() const
    {
        return current >= exit_at;
    }
    /** @brief Supplies a deterministic monotonic clock. */
    Clock::time_point now() const
    {
        return current;
    }
    /** @brief Advances one polling interval. */
    void pause()
    {
        current += Milliseconds(10);
    }
    /** @brief Models either graceful termination or an ignored SIGTERM. */
    void terminate()
    {
        ++terms;
        if (exits_on_term)
        {
            exit_at = current + Milliseconds(20);
        }
    }
    /** @brief Models successful SIGKILL or a child stalled in the kernel. */
    void kill()
    {
        ++kills;
        if (exits_on_kill)
        {
            exit_at = current + Milliseconds(20);
        }
    }
};

/** @brief Reports a failed regression assertion in Release builds as well as Debug builds. */
bool check(bool result, const char* description)
{
    if (!result)
    {
        std::cerr << "FAILED: " << description << '\n';
    }
    return result;
}

/** @brief Exercises deadlines, both termination phases, inherited output writers and interactive exemption. */
bool test_wait_policy()
{
    FakeProcess normal;
    normal.exit_at = normal.now() + Milliseconds(20);
    bool ok = check(SnapmakerUninstall::wait_for_command(normal, Milliseconds(100)) == CommandWaitResult::Completed &&
                        normal.terms == 0 && normal.kills == 0 && normal.reads > 0,
                    "child exit completes even when the output provider never reports EOF");
    FakeProcess graceful;
    graceful.exits_on_term = true;
    ok = check(SnapmakerUninstall::wait_for_command(graceful, Milliseconds(100)) == CommandWaitResult::TimedOut &&
                   graceful.terms == 1 && graceful.kills == 0 &&
                   graceful.now() == Clock::time_point{} + Milliseconds(120),
               "execution deadline sends SIGTERM and collects graceful exit") &&
         ok;
    FakeProcess killed;
    ok = check(SnapmakerUninstall::wait_for_command(killed, Milliseconds(100)) == CommandWaitResult::TimedOut &&
                   killed.terms == 1 && killed.kills == 1 && killed.now() == Clock::time_point{} + Milliseconds(320),
               "ignored SIGTERM escalates after the bounded grace period") &&
         ok;
    FakeProcess stalled;
    stalled.exits_on_kill = false;
    ok = check(SnapmakerUninstall::wait_for_command(stalled, Milliseconds(100)) == CommandWaitResult::Unreaped &&
                   stalled.now() == Clock::time_point{} + Milliseconds(500),
               "failed collection after SIGKILL is explicit after two bounded grace periods") &&
         ok;
    FakeProcess interactive;
    interactive.exit_at = interactive.now() + Milliseconds(10000);
    ok = check(SnapmakerUninstall::wait_for_command(interactive, Milliseconds(0)) == CommandWaitResult::Completed &&
                   interactive.terms == 0 && interactive.kills == 0,
               "interactive commands can outlive the normal execution budget") &&
         ok;
    return ok;
}

#ifndef _WIN32
/** @brief Provides real child behaviors without invoking launchctl or accessing user data. */
int run_child(const std::string& mode)
{
    if (mode == "dialog")
    {
        std::this_thread::sleep_for(Milliseconds(3100));
        return 7;
    }
    if (mode == "pipe")
    {
        const pid_t writer = ::fork();
        if (writer < 0)
        {
            return 1;
        }
        if (writer == 0)
        {
            std::this_thread::sleep_for(Milliseconds(1500));
            _exit(0);
        }
        return 7;
    }
    if (mode == "ignore")
    {
        std::signal(SIGTERM, SIG_IGN);
    }
    if (mode == "disposition")
    {
        return std::signal(SIGTERM, SIG_IGN) == SIG_DFL ? 7 : 1;
    }
    if (mode == "exit")
    {
        std::cout << "captured output\n";
        return 7;
    }
    for (;;)
    {
        if (mode == "flood")
        {
            std::cout << std::string(4096, 'x') << std::flush;
        }
        else
        {
            std::this_thread::sleep_for(Milliseconds(10));
        }
    }
}

/** @brief Exercises the production POSIX spawn, pipe and wait implementation on Linux and macOS. */
bool test_native_commands(const char* executable)
{
    volatile sig_atomic_t child  = 0;
    const auto            exited = SnapmakerUninstall::run_watcher_command({executable, "exit"}, true, child);
    bool                  ok     = check(exited.exit_code == 7 && exited.output == "captured output\n" && child == 0,
                                         "real child exit status and output are retained");
    const auto            quiet  = SnapmakerUninstall::run_watcher_command({executable, "exit"}, false, child);
    ok = check(quiet.exit_code == 7 && quiet.output.empty() && child == 0, "quiet child exit is collected") && ok;
    const auto missing = SnapmakerUninstall::run_watcher_command({"/nonexistent/watcher-test-command"}, true, child);
    ok = check(missing.exit_code == -1 && child == 0, "spawn failure returns without an owned child") && ok;
    const auto inherited   = std::signal(SIGTERM, SIG_IGN);
    const auto disposition = SnapmakerUninstall::run_watcher_command({executable, "disposition"}, false, child);
    std::signal(SIGTERM, inherited);
    ok = check(disposition.exit_code == 7 && child == 0, "child resets inherited SIGTERM ignore") && ok;
    for (const std::string mode : {"hang", "quiet", "ignore", "flood", "pipe", "dialog"})
    {
        // Match the watcher's cleanup phase; commands must reset this inherited disposition.
        const auto              previous = std::signal(SIGTERM, SIG_IGN);
        const Clock::time_point started  = Clock::now();
        const auto         result  = SnapmakerUninstall::run_watcher_command({executable, mode}, mode != "quiet", child,
                                                                             mode == "dialog");
        const Milliseconds elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - started);
        std::signal(SIGTERM, previous);
        const bool timed_out = mode != "pipe" && mode != "dialog";
        ok                   = check(result.timed_out == timed_out && child == 0 && result.output.size() <= 64 * 1024 &&
                                         (timed_out ? result.exit_code == -1 : result.exit_code == 7) &&
                                         elapsed < (mode == "pipe" ? Milliseconds(1200) : Milliseconds(6000)),
                                     mode.c_str()) &&
             ok;
    }
    int status = 0;
    ok         = check(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
                       "all directly spawned children are collected") &&
         ok;
    return ok;
}
#endif

} // namespace

/** @brief Runs deterministic policy checks everywhere and real subprocess regressions on Unix. */
int main(int argc, char** argv)
{
#ifndef _WIN32
    if (argc == 2)
    {
        return run_child(argv[1]);
    }
#else
    (void) argc;
    (void) argv;
#endif
    bool ok = test_wait_policy();
#ifndef _WIN32
    ok = test_native_commands(argv[0]) && ok;
#else
    std::cout << "Native POSIX command checks skipped on Windows.\n";
#endif
    return ok ? 0 : 1;
}
