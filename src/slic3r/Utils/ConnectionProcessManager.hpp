#ifndef slic3r_Utils_ConnectionProcessManager_hpp_
#define slic3r_Utils_ConnectionProcessManager_hpp_

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

    const boost::filesystem::path& executable() const { return config_.executable; }
    const Config&                  config() const { return config_; }

    static ProcessDiscoveryError    parse_port_frame(const std::string& output, std::uint16_t& port);
    static std::vector<std::string> build_arguments(const std::string& locale);
    // File name of the snapmaker_connection CLI binary expected next to the resources directory
    // for the current platform and architecture. The binary itself is placed locally and not tracked in git.
    static std::string_view cli_executable_name();

private:
    struct ChildProcessTracker;

    static ProcessRunner default_runner(Config config, const std::shared_ptr<ChildProcessTracker>& tracker);

    Config                         config_;
    ProcessRunner                  runner_;
    std::shared_ptr<ChildProcessTracker> tracker_;
};

}} // namespace Slic3r::Gateway

#endif // slic3r_Utils_ConnectionProcessManager_hpp_
