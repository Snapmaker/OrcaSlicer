#include "MacUninstallWatcher.hpp"

#include "macos/UninstallWatcherLock.hpp"
#include "macos/UninstallWatcherLifecycle.hpp"
#include "macos/UninstallWatcherState.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/stdpaths.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uuid/uuid.h>

extern char** environ;

namespace Slic3r { namespace GUI {
namespace {

// launchctl is not critical for startup: bound every wait and degrade on timeout.
int run_quiet(const std::vector<std::string>& args, std::chrono::steady_clock::time_point deadline,
              const std::atomic<bool>& stopping, pid_t& unreaped_child)
{
    if (args.empty() || stopping.load() || unreaped_child != 0 || std::chrono::steady_clock::now() >= deadline)
        return -1;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0)
        return -1;

    for (;;)
    {
        int         status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (waited < 0 && errno != EINTR)
        {
            return -1;
        }

        const auto now = std::chrono::steady_clock::now();
        if (stopping.load() || now >= deadline)
            break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(10)));
    }

    BOOST_LOG_TRIVIAL(warning) << "Timed out waiting for " << args.front();
    ::kill(pid, SIGTERM);

    const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < grace_deadline)
    {
        int         status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD))
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ::kill(pid, SIGKILL);
    // Even a killed child must not turn worker shutdown into an unbounded wait.
    const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < kill_deadline)
    {
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD))
        {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_LOG_TRIVIAL(warning) << "Uninstall watcher command has not exited after SIGKILL, pid " << pid;
    unreaped_child = pid;
    return -1;
}

std::string xml_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

using SnapmakerUninstall::read_text_file;
using SnapmakerUninstall::write_text_atomic;

bool files_equal(const boost::filesystem::path& left, const boost::filesystem::path& right)
{
    // Two error codes: the evaluation order of the two size queries is unspecified, so a
    // shared error_code would only reflect whichever call happens to run last.
    boost::system::error_code left_ec;
    boost::system::error_code right_ec;
    const auto left_size  = boost::filesystem::file_size(left, left_ec);
    const auto right_size = boost::filesystem::file_size(right, right_ec);
    if (left_ec || right_ec || left_size != right_size)
        return false;

    std::ifstream a(left.string(), std::ios::binary);
    std::ifstream b(right.string(), std::ios::binary);
    if (!a || !b)
        return false;

    char left_buf[8192];
    char right_buf[8192];
    for (;;) {
        a.read(left_buf, sizeof(left_buf));
        b.read(right_buf, sizeof(right_buf));
        const auto na = a.gcount();
        const auto nb = b.gcount();
        if (na != nb || std::memcmp(left_buf, right_buf, static_cast<size_t>(na)) != 0)
            return false;
        if (na == 0)
            return true;
    }
}

std::filesystem::path current_bundle_path(const std::string& executable)
{
    if (executable.empty())
        return {};
    // .../Snapmaker Orca.app/Contents/MacOS/Snapmaker_Orca
    return std::filesystem::path(executable).parent_path().parent_path().parent_path().lexically_normal();
}

std::string launchd_domain()
{
    return std::string("gui/") + std::to_string(getuid());
}

bool service_loaded(std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& stopping,
                    pid_t& unreaped_child)
{
    const std::string target = launchd_domain() + "/" + SnapmakerUninstall::kLabel;
    return run_quiet({"/bin/launchctl", "print", target}, deadline, stopping, unreaped_child) == 0;
}

/** @brief Serializes the shared LaunchAgent configuration. */
std::string launch_agent_plist(const std::filesystem::path& helper_path, const std::filesystem::path& helper_dir)
{
    return
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n"
                    "<dict>\n"
                    "    <key>Label</key>\n"
                    "    <string>") +
        SnapmakerUninstall::kLabel +
        "</string>\n"
        "    <key>AssociatedBundleIdentifiers</key>\n"
        "    <array>\n"
        "        <string>" +
        SnapmakerUninstall::kBundleId +
        "</string>\n"
        "    </array>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>" +
        xml_escape(helper_path.string()) +
        "</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "    <key>KeepAlive</key>\n"
        "    <true/>\n"
        "    <key>LimitLoadToSessionType</key>\n"
        "    <string>Aqua</string>\n"
        "    <key>ProcessType</key>\n"
        "    <string>Interactive</string>\n"
        "    <key>ThrottleInterval</key>\n"
        "    <integer>10</integer>\n"
        "    <key>Nice</key>\n"
        "    <integer>10</integer>\n"
        "    <key>StandardErrorPath</key>\n"
        "    <string>" +
        xml_escape((helper_dir / SnapmakerUninstall::kLogFileName).string()) +
        "</string>\n"
        "</dict>\n"
        "</plist>\n";

}

/** @brief Generates a unique identity for one registration, retained by retries of that registration. */
std::string new_registration_id()
{
    std::array<unsigned char, 16> id{};
    std::array<char, 37> text{};
    ::uuid_generate_random(id.data());
    ::uuid_unparse_lower(id.data(), text.data());
    return std::string(text.data());
}

/** @brief Requests a background retry with a diagnostic that the worker deduplicates. */
bool retry_registration(std::string& error, const std::string& message)
{
    error = message;
    return false;
}

/** @brief Stages and atomically replaces the helper after reload intent has been persisted. */
bool install_helper(const boost::filesystem::path& seed, const std::filesystem::path& target, std::string& error)
{
    const std::filesystem::path tmp = target.string() + ".tmp";
    boost::system::error_code boost_ec;
    boost::filesystem::copy_file(seed, tmp.string(), boost::filesystem::copy_options::overwrite_existing, boost_ec);
    if (!boost_ec)
    {
        boost::filesystem::permissions(tmp.string(),
            boost::filesystem::owner_read | boost::filesystem::owner_write | boost::filesystem::owner_exe |
            boost::filesystem::group_read | boost::filesystem::group_exe |
            boost::filesystem::others_read | boost::filesystem::others_exe, boost_ec);
    }
    if (boost_ec)
    {
        return retry_registration(error, "Failed to stage uninstall watcher: " + boost_ec.message());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec.value() != 0)
    {
        const std::string message = ec.message();
        std::filesystem::remove(tmp, ec);
        return retry_registration(error, "Failed to install uninstall watcher: " + message);
    }
    return true;
}

/** @brief Runs one committed agent command with a fresh timeout, independent of shutdown cancellation. */
int run_agent_command(SnapmakerUninstall::AgentCommand command, const std::filesystem::path& plist,
                      pid_t& unreaped_child)
{
    const std::string domain = launchd_domain();
    const std::string service = domain + "/" + SnapmakerUninstall::kLabel;
    std::vector<std::string> args{"/bin/launchctl"};
    switch (command)
    {
    case SnapmakerUninstall::AgentCommand::Enable: args.insert(args.end(), {"enable", service}); break;
    case SnapmakerUninstall::AgentCommand::Bootout: args.insert(args.end(), {"bootout", service}); break;
    case SnapmakerUninstall::AgentCommand::Bootstrap:
        args.insert(args.end(), {"bootstrap", domain, plist.string()});
        break;
    }
    const std::atomic<bool> committed_update{false};
    return run_quiet(args, std::chrono::steady_clock::now() + std::chrono::seconds(3),
                     committed_update, unreaped_child);
}

/** @brief Performs one registration attempt; false requests a retry without losing reload intent. */
bool register_watcher(const std::string& executable, std::string& published_id, const std::atomic<bool>& stopping,
                      pid_t& unreaped_child, std::string& error)
{
    const std::filesystem::path bundle = current_bundle_path(executable);
    const std::filesystem::path helper_dir = SnapmakerUninstall::helper_dir();
    const std::filesystem::path helper_path = SnapmakerUninstall::installed_helper_path();
    const std::filesystem::path plist_path = SnapmakerUninstall::launch_agent_plist_path();
    if (helper_dir.empty() || helper_path.empty() || plist_path.empty())
    {
        error = "Missing uninstall watcher paths";
        return true;
    }
    const SnapmakerUninstall::LifecycleLock lock(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (!lock.locked())
    {
        error = lock.timed_out() ? "Uninstaller lifecycle lock is busy" : "Could not open uninstaller lifecycle lock";
        return !lock.timed_out();
    }
    if (stopping.load())
    {
        return true;
    }
    const std::string current_id = SnapmakerUninstall::registration_id_from_state(
        read_text_file(SnapmakerUninstall::watch_state_path()));
    if (!SnapmakerUninstall::registration_attempt_is_current(published_id, current_id))
    {
        // Another registration (or completed cleanup) superseded this attempt. Never republish an old identity.
        return true;
    }
    const boost::filesystem::path seed = boost::filesystem::path(executable).parent_path() /
                                         SnapmakerUninstall::kHelperFileName;
    boost::system::error_code boost_ec;
    if (!boost::filesystem::is_regular_file(seed, boost_ec))
    {
        error = "Uninstall watcher seed is missing: " + seed.string();
        return true;
    }

    // From this point, finish the attempt even when shutdown requests cancellation.
    const std::atomic<bool> committed_update{false};
    std::error_code ec;
    std::filesystem::create_directories(helper_dir, ec);
    if (ec.value() == 0)
    {
        std::filesystem::create_directories(plist_path.parent_path(), ec);
    }
    if (ec.value() != 0)
    {
        return retry_registration(error, "Failed to create watcher directories: " + ec.message());
    }
    const bool replaced = !boost::filesystem::is_regular_file(helper_path.string(), boost_ec) ||
                           !files_equal(seed, helper_path.string());
    const std::string plist = launch_agent_plist(helper_path, helper_dir);
    const bool plist_stale = read_text_file(plist_path) != plist;
    const bool loaded = service_loaded(std::chrono::steady_clock::now() + std::chrono::seconds(3),
                                       committed_update, unreaped_child);
    if (unreaped_child != 0)
    {
        return retry_registration(error, "Waiting for timed-out launchctl command to exit");
    }
    SnapmakerUninstall::PendingReload reload;
    if (!reload.valid())
    {
        return retry_registration(error, "Could not read watcher reload marker safely");
    }
    const bool needs_reload = reload.required() || replaced || plist_stale || !loaded;
    if (needs_reload && !reload.require())
    {
        return retry_registration(error, "Could not persist watcher reload marker");
    }
    const std::string id = published_id.empty() ? new_registration_id() : published_id;
    const std::string state = SnapmakerUninstall::watch_state_text(bundle, id);
    if (state.empty() || !write_text_atomic(SnapmakerUninstall::watch_state_path(), state))
    {
        return retry_registration(error, "Failed to publish watcher registration identity");
    }
    published_id = id;
    if (replaced && !install_helper(seed, helper_path, error))
    {
        return false;
    }
    if (!loaded || replaced || plist_stale)
    {
        if (!write_text_atomic(plist_path, plist))
        {
            return retry_registration(error, "Failed to write watcher LaunchAgent plist");
        }
    }
    // Retry permission repair even if a previous attempt already wrote the final plist contents.
    std::filesystem::permissions(plist_path, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write | std::filesystem::perms::group_read |
        std::filesystem::perms::others_read, ec);
    if (ec.value() != 0)
    {
        return retry_registration(error, "Failed to set watcher plist permissions: " + ec.message());
    }
    const SnapmakerUninstall::AgentUpdateResult result = SnapmakerUninstall::update_launch_agent(
        loaded, needs_reload, committed_update,
        [&plist_path, &unreaped_child](SnapmakerUninstall::AgentCommand command)
        {
            return run_agent_command(command, plist_path, unreaped_child);
        });
    if (!reload.complete(result))
    {
        // The failure text goes through the retry diagnostics, which the worker deduplicates,
        // so a persistent bootstrap failure does not repeat its log line on every attempt.
        return retry_registration(error,
                                  result == SnapmakerUninstall::AgentUpdateResult::Failed
                                      ? "launchctl update failed for " + std::string(SnapmakerUninstall::kLabel) + "; reload remains pending"
                                      : "Uninstall watcher registration is incomplete; reload remains pending");
    }
    BOOST_LOG_TRIVIAL(info) << "Registered uninstaller watcher " << SnapmakerUninstall::kLabel;
    return true;
}

} // namespace

MacUninstallWatcherRegistration::~MacUninstallWatcherRegistration()
{
    stop();
}

void MacUninstallWatcherRegistration::start()
{
    if (running_)
    {
        return;
    }
    // wxStandardPaths belongs to the GUI thread. The worker uses only this owned snapshot.
    executable_ = wxStandardPaths::Get().GetExecutablePath().ToUTF8().data();
    if (geteuid() == 0 || !SnapmakerUninstall::is_managed_install_path(current_bundle_path(executable_)))
    {
        return;
    }
    stopping_.store(false);
    const int result = ::pthread_create(&worker_, nullptr, &MacUninstallWatcherRegistration::run, this);
    if (result != 0)
    {
        BOOST_LOG_TRIVIAL(warning) << "Could not start uninstaller registration worker: " << std::strerror(result);
        return;
    }
    running_ = true;
}

void MacUninstallWatcherRegistration::stop()
{
    if (!running_)
    {
        return;
    }
    stopping_.store(true);
    ::pthread_join(worker_, nullptr);
    running_ = false;
}

void* MacUninstallWatcherRegistration::run(void* context)
{
    MacUninstallWatcherRegistration* self = static_cast<MacUninstallWatcherRegistration*>(context);
    if (self == nullptr)
    {
        return nullptr;
    }
    std::string published_id;
    std::string reported_error;
    pid_t unreaped_child = 0;
    while (!self->stopping_.load())
    {
        // A timed-out command must exit before another attempt can mutate the same launchd service.
        if (unreaped_child != 0)
        {
            int status = 0;
            const pid_t waited = ::waitpid(unreaped_child, &status, WNOHANG);
            if (waited == unreaped_child || (waited < 0 && errno == ECHILD))
            {
                unreaped_child = 0;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
        }
        std::string error;
        const bool finished = register_watcher(self->executable_, published_id, self->stopping_, unreaped_child, error);
        if (!error.empty() && error != reported_error)
        {
            BOOST_LOG_TRIVIAL(warning) << error;
            reported_error = error;
        }
        if (finished)
        {
            return nullptr;
        }
        for (unsigned tick = 0; tick < 50 && !self->stopping_.load(); ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return nullptr;
}

}} // namespace Slic3r::GUI
