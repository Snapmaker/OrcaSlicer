#include "ConnectionProcessManager.hpp"

#undef pid_t

#include <boost/asio/io_context.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <system_error>

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

} // namespace

ConnectionProcessManager::ConnectionProcessManager(Config config, ProcessRunner runner)
    : config_(std::move(config)), runner_(std::move(runner))
{
    if (!runner_)
        runner_ = default_runner(config_);
}

std::vector<std::string> ConnectionProcessManager::build_arguments(const std::string& locale) { return {"--locale=" + locale, "--orca"}; }

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

    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed == 0 || parsed > 65535)
            return ProcessDiscoveryError::InvalidPortFrame;
        port = static_cast<std::uint16_t>(parsed);
    } catch (...) {
        return ProcessDiscoveryError::InvalidPortFrame;
    }

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
        result.message = "snapmaker_connection.exe path is empty";
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
        result.message = process_result.error.empty() ? "failed to launch snapmaker_connection.exe" : process_result.error;
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
    switch (parse_error) {
    case ProcessDiscoveryError::NoPortFrame: result.message = "PORT frame was not found in snapmaker_connection.exe stdout"; break;
    case ProcessDiscoveryError::InvalidPortFrame: result.message = "PORT frame is malformed"; break;
    case ProcessDiscoveryError::MultiplePortFrames: result.message = "multiple PORT frames were found"; break;
    case ProcessDiscoveryError::None: break;
    default: break;
    }
    return result;
}

ConnectionProcessManager::ProcessRunner ConnectionProcessManager::default_runner(Config config)
{
    return [config = std::move(config)](const std::vector<std::string>& arguments) -> ProcessRunResult {
        ProcessRunResult           result;
        boost::asio::io_context    io_context;
        boost::process::async_pipe stdout_pipe(io_context);
        boost::process::child      child;
        std::error_code            launch_error;

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
        child.detach();
        boost::system::error_code pipe_error;
        stdout_pipe.close(pipe_error);
        return result;
    };
}

}} // namespace Slic3r::Gateway
