#include "ConnectionProcessManager.hpp"

#undef pid_t

#include <boost/asio/io_context.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <boost/process/windows.hpp>
#include <boost/algorithm/string.hpp>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#elif defined(__APPLE__)
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#elif defined(__linux__)
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <limits.h>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace Slic3r { namespace Gateway {
namespace {

bool is_valid_locale(const std::string& locale)
{
    if (locale.empty() || locale.size() > 32 || locale.front() == '-')
        return false;

    return std::all_of(locale.begin(), locale.end(), [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-' || ch == '_'; });
}

bool starts_line_at(const std::string& data, std::size_t position) { return position == 0 || data[position - 1] == '\n'; }

std::size_t find_next_line_port(const std::string& data, std::size_t from)
{
    std::size_t position = data.find("PORT:", from);
    while (position != std::string::npos && !starts_line_at(data, position))
        position = data.find("PORT:", position + 1);
    return position;
}

std::string format_arguments(const std::vector<std::string>& arguments)
{
    std::string formatted;
    for (const std::string& argument : arguments) {
        if (!formatted.empty())
            formatted += " ";
        formatted += argument;
    }
    return formatted;
}

constexpr std::uintmax_t maximum_lock_file_size = 64 * 1024;

constexpr std::uint64_t maximum_owner_pid()
{
#if defined(__APPLE__) || defined(__linux__)
    return static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
#else
    return std::numeric_limits<std::uint32_t>::max();
#endif
}

#if defined(__APPLE__) || defined(__linux__)
boost::filesystem::path shared_cli_lock_path()
{ return boost::filesystem::path{Slic3r::data_dir()} / "snapmaker_control" / "data" / "snapmaker_control.lock"; }

std::string process_executable_path(std::uint64_t pid)
{
    constexpr std::string_view deleted_suffix = " (deleted)";

#if defined(__APPLE__)
    char      path[PROC_PIDPATHINFO_MAXSIZE]{};
    const int path_length = proc_pidpath(static_cast<pid_t>(pid), path, sizeof(path));
    if (path_length <= 0)
        return {};
    std::string executable_path{path, static_cast<std::size_t>(path_length)};
#else
    char              path[PATH_MAX]{};
    const std::string proc_path   = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t     path_length = readlink(proc_path.c_str(), path, sizeof(path) - 1);
    if (path_length <= 0)
        return {};
    std::string executable_path{path, static_cast<std::size_t>(path_length)};
#endif
    if (executable_path.size() >= deleted_suffix.size() &&
        executable_path.compare(executable_path.size() - deleted_suffix.size(), deleted_suffix.size(), deleted_suffix) == 0)
        executable_path.resize(executable_path.size() - deleted_suffix.size());
    return executable_path;
}

bool process_executable_name_matches(std::uint64_t pid, const boost::filesystem::path& expected_executable)
{
    const std::string executable_path = process_executable_path(pid);
    if (executable_path.empty())
        return false;

    const boost::filesystem::path actual_executable{executable_path};
    return actual_executable.filename() == expected_executable.filename();
}

bool process_exited(std::uint64_t pid)
{
#if defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    if (proc_pidpath(static_cast<pid_t>(pid), path, sizeof(path)) <= 0)
        return true;
    return false;
#else
    return process_executable_path(pid).empty();
#endif
}

bool terminate_process(std::uint64_t pid, const boost::filesystem::path& expected_executable, std::uint16_t port)
{
    constexpr int signal_timeout_ms = 5000;
#if defined(__linux__)
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
    const int pid_fd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
    if (pid_fd >= 0) {
        // The pidfd pins this PID while the executable identity is checked and signaled.
        if (!process_executable_name_matches(pid, expected_executable)) {
            close(pid_fd);
            return false;
        }
        if (::syscall(SYS_pidfd_send_signal, pid_fd, SIGKILL, nullptr, 0) != 0) {
            const int error = errno;
            BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner cannot be signaled: pid=" << pid << ", port=" << port
                                       << ", error=" << error;
            close(pid_fd);
            return false;
        }
        pollfd descriptor{pid_fd, POLLIN, 0};
        int    poll_result = 0;
        do {
            poll_result = ::poll(&descriptor, 1, signal_timeout_ms);
        } while (poll_result < 0 && errno == EINTR);
        close(pid_fd);
        if (poll_result > 0 && (descriptor.revents & POLLIN) != 0)
            return true;
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner did not exit promptly: pid=" << pid << ", port=" << port;
        return false;
    }
#endif

    // Old Linux kernels and macOS cannot pin a PID here, so this fallback retains a narrow
    // reuse window between identity validation and kill.
    if (!process_executable_name_matches(pid, expected_executable))
        return false;

    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0) {
        const int error = errno;
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner cannot be signaled: pid=" << pid << ", port=" << port
                                   << ", error=" << error;
        return false;
    }

    for (int elapsed_ms = 0; elapsed_ms < signal_timeout_ms; elapsed_ms += 100) {
        if (process_exited(pid))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner did not exit promptly: pid=" << pid << ", port=" << port;
    return false;
}
#endif

#ifdef _WIN32
DWORD loopback_listener_pid(std::uint16_t port)
{
    DWORD table_size = 0;
    DWORD result     = GetExtendedTcpTable(nullptr, &table_size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || table_size == 0)
        return {};

    std::vector<unsigned char> buffer(table_size);
    result = GetExtendedTcpTable(buffer.data(), &table_size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (result != NO_ERROR)
        return {};

    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const MIB_TCPROW_OWNER_PID& row           = table->table[index];
        const auto                  local_port    = ntohs(static_cast<u_short>(row.dwLocalPort & 0xffff));
        const auto                  local_address = ntohl(row.dwLocalAddr);
        if (row.dwState == MIB_TCP_STATE_LISTEN && local_address == INADDR_LOOPBACK && local_port == port)
            return row.dwOwningPid;
    }
    return 0;
}

struct ProcessHandleDeleter
{
    void operator()(HANDLE process) const
    {
        if (process != nullptr)
            CloseHandle(process);
    }
};

using ProcessHandle = std::unique_ptr<void, ProcessHandleDeleter>;

bool process_executable_name_matches(HANDLE process, const boost::filesystem::path& expected_executable)
{
    wchar_t image_path[MAX_PATH]{};
    DWORD   image_path_size = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, image_path, &image_path_size))
        return false;

    const boost::filesystem::path actual_executable{std::wstring{image_path, image_path_size}};
    return boost::iequals(actual_executable.filename().wstring(), expected_executable.filename().wstring());
}
#endif

} // namespace

std::optional<std::uint64_t> ConnectionProcessManager::owner_pid_from_lock(const boost::filesystem::path& lock_path, std::uint16_t port)
{
    boost::system::error_code file_size_error;
    const std::uintmax_t      file_size = boost::filesystem::file_size(lock_path, file_size_error);
    if (file_size_error || file_size > maximum_lock_file_size)
        return std::nullopt;

    std::ifstream lock_file{lock_path.string(), std::ios::binary};
    if (!lock_file)
        return std::nullopt;

    constexpr std::streamsize maximum_read_size = static_cast<std::streamsize>(maximum_lock_file_size + 1);
    std::string               content(static_cast<std::size_t>(maximum_read_size), '\0');
    lock_file.read(content.data(), maximum_read_size);
    content.resize(static_cast<std::size_t>(lock_file.gcount()));
    if (content.size() > maximum_lock_file_size)
        return std::nullopt;

    const nlohmann::json lock = nlohmann::json::parse(content, nullptr, false);
    if (lock.is_discarded() || !lock.is_object())
        return std::nullopt;

    const auto pid       = lock.find("pid");
    const auto lock_port = lock.find("port");
    const auto mode      = lock.find("mode");
    const auto state     = lock.find("state");
    if (pid == lock.end() || !pid->is_number_unsigned())
        return std::nullopt;
    const std::uint64_t owner_pid = pid->get<std::uint64_t>();
    if (owner_pid == 0 || owner_pid > maximum_owner_pid())
        return std::nullopt;
    if (lock_port == lock.end() || !lock_port->is_number_unsigned() || lock_port->get<std::uint64_t>() != port)
        return std::nullopt;
    if (mode == lock.end() || !mode->is_string() || mode->get<std::string>() != "orca")
        return std::nullopt;
    if (state == lock.end() || !state->is_string() || state->get<std::string>() != "serving")
        return std::nullopt;

    return owner_pid;
}

struct ConnectionProcessManager::ChildProcessTracker
{
    // Detached children survive the runner's local child object, so the manager keeps the
    // handles here until the next terminate() call reaps them.
    std::mutex                                          mutex;
    std::vector<std::shared_ptr<boost::process::child>> children;

    void track(std::shared_ptr<boost::process::child> child)
    {
        std::lock_guard<std::mutex> lock(mutex);
        children.push_back(std::move(child));
    }

    void terminate_all()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& child : children) {
            std::error_code terminate_error;
            child->terminate(terminate_error);
            std::error_code wait_error;
            child->wait(wait_error);
        }
        children.clear();
    }
};

ConnectionProcessManager::ConnectionProcessManager(Config config, ProcessRunner runner)
    : config_(std::move(config)), runner_(std::move(runner)), tracker_(std::make_shared<ChildProcessTracker>())
{
    if (!runner_)
        runner_ = default_runner(config_, tracker_);
}

void ConnectionProcessManager::terminate() { tracker_->terminate_all(); }

PortOwnerTakeoverResult ConnectionProcessManager::terminate_port_owner(std::uint16_t port)
{
#ifdef _WIN32
    if (port == 0 || config_.executable.empty())
        return PortOwnerTakeoverResult::NotTakenOver;

    const DWORD pid = loopback_listener_pid(port);
    if (pid == 0) {
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner takeover found no listener on port " << port;
        return PortOwnerTakeoverResult::NotTakenOver;
    }

    ProcessHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)};
    if (!process) {
        const DWORD error = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner cannot be opened: pid=" << pid << ", port=" << port
                                   << ", error=" << error;
        return PortOwnerTakeoverResult::NotTakenOver;
    }
    if (!process_executable_name_matches(process.get(), config_.executable)) {
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner ignored because its executable does not match: pid=" << pid
                                   << ", port=" << port;
        return PortOwnerTakeoverResult::NotTakenOver;
    }
    if (!TerminateProcess(static_cast<HANDLE>(process.get()), 1)) {
        const DWORD error = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner cannot be terminated: pid=" << pid << ", port=" << port
                                   << ", error=" << error;
        return PortOwnerTakeoverResult::NotTakenOver;
    }
    constexpr DWORD terminate_timeout_ms = 5000;
    const DWORD     wait_result          = WaitForSingleObject(process.get(), terminate_timeout_ms);
    if (wait_result == WAIT_FAILED) {
        const DWORD error = GetLastError();
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner wait failed: pid=" << pid << ", port=" << port << ", error=" << error;
        return PortOwnerTakeoverResult::NotTakenOver;
    }
    if (wait_result != WAIT_OBJECT_0) {
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner did not exit promptly: pid=" << pid << ", port=" << port
                                   << ", wait_result=" << wait_result;
        return PortOwnerTakeoverResult::NotTakenOver;
    }
    BOOST_LOG_TRIVIAL(warning) << "connection gateway terminated stale shared CLI owner: pid=" << pid << ", port=" << port;
    return PortOwnerTakeoverResult::TakenOver;
#elif defined(__APPLE__) || defined(__linux__)
    if (port == 0 || config_.executable.empty())
        return PortOwnerTakeoverResult::NotTakenOver;

    const std::optional<std::uint64_t> pid = owner_pid_from_lock(shared_cli_lock_path(), port);
    if (!pid.has_value()) {
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner lock does not identify port " << port;
        return PortOwnerTakeoverResult::NotTakenOver;
    }

    if (!terminate_process(*pid, config_.executable, port)) {
        BOOST_LOG_TRIVIAL(warning) << "connection gateway stale owner ignored or termination failed: pid=" << *pid << ", port=" << port;
        return PortOwnerTakeoverResult::NotTakenOver;
    }

    BOOST_LOG_TRIVIAL(warning) << "connection gateway terminated stale shared CLI owner: pid=" << *pid << ", port=" << port;
    return PortOwnerTakeoverResult::TakenOver;
#else
    BOOST_LOG_TRIVIAL(warning) << "connection gateway stale shared CLI owner takeover is not supported on this platform; port=" << port;
    return PortOwnerTakeoverResult::Unsupported;
#endif
}

std::vector<std::string> ConnectionProcessManager::build_arguments(const std::string& locale) { return {"--locale=" + locale, "--orca"}; }

std::string_view ConnectionProcessManager::cli_executable_name()
{
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "snapmaker_control_windows_x64.exe";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "snapmaker_control_windows_arm64.exe";
#else
#error "unsupported Windows architecture for the snapmaker_control CLI"
#endif
#elif defined(__APPLE__)
#if defined(__x86_64__)
    return "snapmaker_control_macos_x64";
#elif defined(__arm64__) || defined(__aarch64__)
    return "snapmaker_control_macos_arm64";
#else
#error "unsupported macOS architecture for the snapmaker_control CLI"
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    return "snapmaker_control_linux_x64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "snapmaker_control_linux_arm64";
#else
#error "unsupported Linux architecture for the snapmaker_control CLI"
#endif
#else
#error "unsupported platform for the snapmaker_control CLI"
#endif
}

ProcessDiscoveryError ConnectionProcessManager::parse_port_frame(const std::string& output, std::uint16_t& port)
{
    constexpr std::string_view prefix{"PORT:"};
    const std::size_t          frame_start = find_next_line_port(output, 0);
    if (frame_start == std::string::npos)
        return ProcessDiscoveryError::NoPortFrame;

    const std::size_t value_start = frame_start + prefix.size();
    const std::size_t terminator  = output.find("\r\n\r\n", value_start);
    if (terminator == std::string::npos)
        return ProcessDiscoveryError::InvalidPortFrame;

    const std::string value = output.substr(value_start, terminator - value_start);
    if (value.empty() || value.find_first_of("\r\n") != std::string::npos ||
        !std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        return ProcessDiscoveryError::InvalidPortFrame;

    unsigned long parsed = 0;
    for (const char digit : value) {
        parsed = parsed * 10 + static_cast<unsigned long>(digit - '0');
        if (parsed > 65535)
            return ProcessDiscoveryError::InvalidPortFrame;
    }
    if (parsed == 0)
        return ProcessDiscoveryError::InvalidPortFrame;
    port = static_cast<std::uint16_t>(parsed);

    if (find_next_line_port(output, terminator + 4) != std::string::npos)
        return ProcessDiscoveryError::MultiplePortFrames;

    return ProcessDiscoveryError::None;
}

ConnectionProcessManager::DiscoveryResult ConnectionProcessManager::discover_port(const std::string& locale)
{
    DiscoveryResult result;
    result.arguments = build_arguments(locale);

    if (config_.executable.empty()) {
        result.error   = ProcessDiscoveryError::InvalidExecutable;
        result.message = "connection cli path is empty";
        return result;
    }
    if (!is_valid_locale(locale)) {
        result.error   = ProcessDiscoveryError::InvalidLocale;
        result.message = "invalid locale: " + locale;
        return result;
    }

    ProcessRunResult process_result;
    try {
        process_result = runner_(result.arguments);
    } catch (const std::exception& exception) {
        result.error   = ProcessDiscoveryError::LaunchFailed;
        result.message = exception.what();
        return result;
    }
    if (process_result.launch_failed) {
        result.error   = ProcessDiscoveryError::LaunchFailed;
        result.message = process_result.error.empty() ? "failed to launch " + config_.executable.filename().string() : process_result.error;
        return result;
    }
    if (process_result.timed_out) {
        result.error   = ProcessDiscoveryError::Timeout;
        result.message = "timed out waiting for PORT frame";
        return result;
    }
    if (!process_result.error.empty()) {
        result.error   = ProcessDiscoveryError::LaunchFailed;
        result.message = process_result.error;
        return result;
    }

    const ProcessDiscoveryError parse_error = parse_port_frame(process_result.stdout_data, result.port);
    result.error                            = parse_error;
    if (parse_error == ProcessDiscoveryError::None) {
        BOOST_LOG_TRIVIAL(warning) << "[FLUTTER_FLOW] [gateway] [port_ready] port=" << result.port;
    }
    switch (parse_error) {
    case ProcessDiscoveryError::NoPortFrame:
        result.message = "PORT frame was not found in " + config_.executable.filename().string() + " stdout";
        break;
    case ProcessDiscoveryError::InvalidPortFrame: result.message = "PORT frame is malformed"; break;
    case ProcessDiscoveryError::MultiplePortFrames: result.message = "multiple PORT frames were found"; break;
    case ProcessDiscoveryError::None: break;
    default: break;
    }
    return result;
}

ConnectionProcessManager::ProcessRunner ConnectionProcessManager::default_runner(Config                                      config,
                                                                                 const std::shared_ptr<ChildProcessTracker>& tracker)
{
    return [config = std::move(config), tracker](const std::vector<std::string>& arguments) -> ProcessRunResult {
        ProcessRunResult           result;
        boost::asio::io_context    io_context;
        boost::process::async_pipe stdout_pipe(io_context);
        boost::process::child      child;
        std::error_code            launch_error;

        BOOST_LOG_TRIVIAL(info) << "connection cli launch: executable=" << config.executable
                                << ", arguments=" << format_arguments(arguments);

        try {
#ifdef _WIN32
            child = boost::process::child(config.executable, boost::process::args = arguments, boost::process::std_out > stdout_pipe,
                                          boost::process::std_err > boost::process::null, boost::process::windows::create_no_window,
                                          launch_error);
#else
            child = boost::process::child(config.executable, boost::process::args = arguments, boost::process::std_out > stdout_pipe,
                                          boost::process::std_err > boost::process::null, launch_error);
#endif
        } catch (const std::exception& exception) {
            result.launch_failed = true;
            result.error         = exception.what();
            return result;
        }

        if (launch_error) {
            result.launch_failed = true;
            result.error         = launch_error.message();
            boost::system::error_code pipe_error;
            stdout_pipe.close(pipe_error);
            return result;
        }

        boost::asio::streambuf    buffer;
        boost::system::error_code read_error;
        std::size_t               bytes_read      = 0;
        bool                      delimiter_found = false;
        boost::asio::async_read_until(stdout_pipe, buffer, "\r\n\r\n", [&](const boost::system::error_code& error, std::size_t bytes) {
            read_error      = error;
            bytes_read      = bytes;
            delimiter_found = error == boost::system::errc::success;
        });

        const auto processed = io_context.run_for(config.timeout);
        (void) processed;
        if (!delimiter_found) {
            result.timed_out = true;
            std::error_code terminate_error;
            child.terminate(terminate_error);
            std::error_code wait_error;
            child.wait(wait_error);
            boost::system::error_code pipe_error;
            stdout_pipe.close(pipe_error);
            return result;
        }

        if (bytes_read > config.max_stdout_bytes) {
            result.launch_failed = true;
            result.error         = "PORT frame exceeds the stdout size limit";
            std::error_code terminate_error;
            child.terminate(terminate_error);
            std::error_code wait_error;
            child.wait(wait_error);
            return result;
        }

        const auto* data = boost::asio::buffer_cast<const char*>(buffer.data());
        result.stdout_data.assign(data, bytes_read);
        auto tracked_child = std::make_shared<boost::process::child>(std::move(child));
        tracked_child->detach();
        if (tracker)
            tracker->track(std::move(tracked_child));
        boost::system::error_code pipe_error;
        stdout_pipe.close(pipe_error);
        return result;
    };
}

}} // namespace Slic3r::Gateway
