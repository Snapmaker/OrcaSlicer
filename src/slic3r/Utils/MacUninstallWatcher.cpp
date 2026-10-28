#include "MacUninstallWatcher.hpp"

#include "macos/UninstallWatcherNames.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/stdpaths.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Slic3r { namespace GUI {
namespace {

int run_quiet(const std::vector<std::string>& args)
{
    if (args.empty())
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

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

std::string json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool write_text_atomic(const std::filesystem::path& path, const std::string& text)
{
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool files_equal(const boost::filesystem::path& left, const boost::filesystem::path& right)
{
    boost::system::error_code ec;
    if (boost::filesystem::file_size(left, ec) != boost::filesystem::file_size(right, ec) || ec)
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

class FileLock
{
public:
    explicit FileLock(const std::filesystem::path& path)
    {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ >= 0)
            ::flock(fd_, LOCK_EX);
    }
    ~FileLock()
    {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int fd_ = -1;
};

std::filesystem::path current_bundle_path()
{
    const std::string executable = wxStandardPaths::Get().GetExecutablePath().ToUTF8().data();
    if (executable.empty())
        return {};
    // .../Snapmaker Orca.app/Contents/MacOS/Snapmaker_Orca
    return std::filesystem::path(executable).parent_path().parent_path().parent_path().lexically_normal();
}

std::string launchd_domain()
{
    return std::string("gui/") + std::to_string(getuid());
}

bool service_loaded()
{
    const std::string target = launchd_domain() + "/" + SnapmakerUninstall::kLabel;
    return run_quiet({"/bin/launchctl", "print", target}) == 0;
}

} // namespace

void ensure_macos_uninstall_watcher()
{
    if (geteuid() == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Skip uninstaller watcher registration while running as root";
        return;
    }

    const auto bundle = current_bundle_path();
    if (!SnapmakerUninstall::is_managed_install_path(bundle)) {
        BOOST_LOG_TRIVIAL(info) << "Skip uninstaller watcher; bundle is not in /Applications or ~/Applications: "
                                << bundle.string();
        return;
    }

    const auto helper_dir = SnapmakerUninstall::helper_dir();
    const auto helper_path = SnapmakerUninstall::installed_helper_path();
    const auto plist_path = SnapmakerUninstall::launch_agent_plist_path();
    if (helper_dir.empty() || helper_path.empty() || plist_path.empty())
        return;

    std::error_code ec;
    std::filesystem::create_directories(helper_dir, ec);
    if (!ec)
        std::filesystem::create_directories(plist_path.parent_path(), ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create uninstaller watcher directories: " << ec.message();
        return;
    }

    FileLock lock(helper_dir / ".register.lock");

    const std::string executable = wxStandardPaths::Get().GetExecutablePath().ToUTF8().data();
    const boost::filesystem::path seed = boost::filesystem::path(executable).parent_path() / SnapmakerUninstall::kHelperFileName;
    boost::system::error_code boost_ec;
    if (!boost::filesystem::is_regular_file(seed, boost_ec)) {
        BOOST_LOG_TRIVIAL(warning) << "UninstallWatcher seed is missing: " << seed.string();
        return;
    }

    bool replaced = true;
    if (boost::filesystem::is_regular_file(helper_path.string(), boost_ec))
        replaced = !files_equal(seed, helper_path.string());
    if (replaced) {
        const auto tmp = helper_path.string() + ".tmp";
        boost_ec.clear();
        boost::filesystem::copy_file(seed, tmp, boost::filesystem::copy_options::overwrite_existing, boost_ec);
        if (boost_ec) {
            BOOST_LOG_TRIVIAL(error) << "Failed to stage UninstallWatcher: " << boost_ec.message();
            return;
        }
        boost::filesystem::permissions(tmp, boost::filesystem::owner_read | boost::filesystem::owner_write | boost::filesystem::owner_exe |
                                                 boost::filesystem::group_read | boost::filesystem::group_exe |
                                                 boost::filesystem::others_read | boost::filesystem::others_exe,
                                       boost_ec);
        if (boost_ec) {
            BOOST_LOG_TRIVIAL(error) << "Failed to mark UninstallWatcher executable: " << boost_ec.message();
            return;
        }
        std::filesystem::rename(tmp, helper_path, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "Failed to install UninstallWatcher: " << ec.message();
            std::filesystem::remove(tmp, ec);
            return;
        }
        std::filesystem::remove(helper_dir / "UninstallerWatcher", ec);
        ec.clear();
    }

    const std::string state = std::string("{\n  \"bundle_path\": \"") + json_escape(bundle.string()) + "\",\n  \"seen_bundle\": true\n}\n";
    if (!write_text_atomic(SnapmakerUninstall::watch_state_path(), state)) {
        BOOST_LOG_TRIVIAL(error) << "Failed to write uninstaller watcher state";
        return;
    }

    const std::string plist =
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
        std::string(SnapmakerUninstall::kBundleId) +
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
        xml_escape((helper_dir / "watcher.log").string()) +
        "</string>\n"
        "</dict>\n"
        "</plist>\n";

    const bool loaded = service_loaded();
    if (!loaded || replaced) {
        if (!write_text_atomic(plist_path, plist)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to write LaunchAgent plist";
            return;
        }
        std::filesystem::permissions(plist_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                                     std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                                     ec);
    }

    run_quiet({"/bin/launchctl", "enable", launchd_domain() + "/" + SnapmakerUninstall::kLabel});

    if (!loaded) {
        const int rc = run_quiet({"/bin/launchctl", "bootstrap", launchd_domain(), plist_path.string()});
        if (rc != 0 && !service_loaded())
            BOOST_LOG_TRIVIAL(error) << "launchctl bootstrap failed for " << SnapmakerUninstall::kLabel << ", exit " << rc;
        else
            BOOST_LOG_TRIVIAL(info) << "Registered uninstaller watcher " << SnapmakerUninstall::kLabel;
        return;
    }

    if (replaced) {
        // bootout first so launchd drops the previous program path. kickstart would
        // keep running a binary that was still named UninstallerWatcher.
        run_quiet({"/bin/launchctl", "bootout", launchd_domain() + "/" + SnapmakerUninstall::kLabel});
        const int rc = run_quiet({"/bin/launchctl", "bootstrap", launchd_domain(), plist_path.string()});
        if (rc != 0 && !service_loaded())
            BOOST_LOG_TRIVIAL(warning) << "launchctl bootstrap failed for updated SnWathcer, exit " << rc;
        else
            BOOST_LOG_TRIVIAL(info) << "Replaced the existing uninstaller watcher";
    }
}

}} // namespace Slic3r::GUI
