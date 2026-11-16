#pragma once

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Gateway {

enum class ProcessDiscoveryError {
    None,
    InvalidExecutable,
    InvalidLocale,
    LaunchFailed,
    Timeout,
    NoPortFrame,
    InvalidPortFrame,
    MultiplePortFrames,
};

enum class PortOwnerTakeoverResult {
    Unsupported,
    NotTakenOver,
    TakenOver,
};

class ConnectionProcessManager
{
public:
    struct Config
    {
        boost::filesystem::path   executable;
        std::chrono::milliseconds timeout{std::chrono::milliseconds{5000}};
        std::size_t               max_stdout_bytes{64 * 1024};
    };

    struct ProcessRunResult
    {
        bool        launch_failed{false};
        bool        timed_out{false};
        std::string stdout_data;
        std::string error;
    };

    struct DiscoveryResult
    {
        ProcessDiscoveryError    error{ProcessDiscoveryError::None};
        std::uint16_t            port{0};
        std::string              message;
        std::vector<std::string> arguments;
    };

    using ProcessRunner = std::function<ProcessRunResult(const std::vector<std::string>& arguments)>;

    ConnectionProcessManager(Config config, ProcessRunner runner = {});
    virtual ~ConnectionProcessManager() = default;

    DiscoveryResult discover_port(const std::string& locale);

    // Force-terminates the CLI process spawned by the most recent discover_port() call
    // (TerminateProcess on Windows, SIGKILL elsewhere). No-op when the injected runner is
    // used or no process has been spawned yet.
    virtual void terminate();

    // Takes over a stale shared CLI instance that owns the discovered loopback port. Windows
    // validates the TCP listener PID; POSIX validates the PID from the shared CLI lock. Both
    // paths also validate the executable name before terminating the process.
    virtual PortOwnerTakeoverResult terminate_port_owner(std::uint16_t port);

    const boost::filesystem::path& executable() const { return config_.executable; }
    const Config&                  config() const { return config_; }

    static ProcessDiscoveryError        parse_port_frame(const std::string& output, std::uint16_t& port);
    static std::vector<std::string>     build_arguments(const std::string& locale);
    static std::optional<std::uint64_t> owner_pid_from_lock(const boost::filesystem::path& lock_path, std::uint16_t port);
    // macOS truncates the kernel process name (p_comm) to MAXCOMLEN bytes, so only the common prefix is comparable.
    static bool truncated_process_name_matches(std::string_view process_name, std::string_view expected_name);
    // File name of the snapmaker_connection CLI binary expected next to the resources directory
    // for the current platform and architecture. The binary itself is placed locally and not tracked in git.
    static std::string_view cli_executable_name();

private:
    struct ChildProcessTracker;

    static ProcessRunner default_runner(Config config, const std::shared_ptr<ChildProcessTracker>& tracker);

    Config                               config_;
    ProcessRunner                        runner_;
    std::shared_ptr<ChildProcessTracker> tracker_;
};

}} // namespace Slic3r::Gateway
