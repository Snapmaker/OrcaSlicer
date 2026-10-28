// User-level helper. Launchd keeps a single job alive outside the .app so a
// drag-to-Trash still has something that can ask whether to delete leftovers.
// It does not relaunch Snapmaker Orca, and it does not need an administrator password.

#include "macos/UninstallWatcherNames.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

struct CommandResult {
    int         exit_code = -1;
    std::string output;
};

CommandResult run_command(const std::vector<std::string>& args, bool capture)
{
    CommandResult result;
    if (args.empty())
        return result;

    int pipefd[2] = {-1, -1};
    if (capture && pipe(pipefd) != 0)
        return result;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (capture) {
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture)
        close(pipefd[1]);
    if (rc != 0) {
        if (capture)
            close(pipefd[0]);
        return result;
    }

    if (capture) {
        char buf[512];
        ssize_t n = 0;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
            result.output.append(buf, buf + n);
        close(pipefd[0]);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return result;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

void log_line(const std::string& message)
{
    std::fprintf(stderr, "SnWathcer: %s\n", message.c_str());
    std::fflush(stderr);
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::optional<std::string> json_string_field(const std::string& text, const char* key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const auto key_pos = text.find(pattern);
    if (key_pos == std::string::npos)
        return std::nullopt;
    const auto colon = text.find(':', key_pos + pattern.size());
    if (colon == std::string::npos)
        return std::nullopt;
    const auto open = text.find('"', colon + 1);
    if (open == std::string::npos)
        return std::nullopt;

    std::string value;
    for (size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            value.push_back(text[i + 1]);
            ++i;
            continue;
        }
        if (text[i] == '"')
            return value;
        value.push_back(text[i]);
    }
    return std::nullopt;
}

bool json_bool_true(const std::string& text, const char* key)
{
    const std::string pattern = std::string("\"") + key + "\"";
    const auto key_pos = text.find(pattern);
    if (key_pos == std::string::npos)
        return false;
    const auto colon = text.find(':', key_pos + pattern.size());
    if (colon == std::string::npos)
        return false;
    const auto rest = text.find_first_not_of(" \t\r\n", colon + 1);
    return rest != std::string::npos && text.compare(rest, 4, "true") == 0;
}

struct WatchState {
    std::filesystem::path bundle;
    bool                  seen = false;
    bool                  valid = false;
    bool                  declined = false;
};

WatchState load_state()
{
    WatchState state;
    const auto path = SnapmakerUninstall::watch_state_path();
    if (path.empty())
        return state;
    const auto text = read_text(path);
    const auto bundle = json_string_field(text, "bundle_path");
    if (!bundle)
        return state;
    state.bundle   = std::filesystem::path(*bundle).lexically_normal();
    state.seen     = json_bool_true(text, "seen_bundle");
    state.declined = json_bool_true(text, "declined");
    state.valid    = SnapmakerUninstall::is_managed_install_path(state.bundle);
    return state;
}

std::string json_escape_path(const std::string& text)
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

void write_watch_state(const std::filesystem::path& bundle, bool declined)
{
    const auto path = SnapmakerUninstall::watch_state_path();
    if (path.empty())
        return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return;
    out << "{\n  \"bundle_path\": \"" << json_escape_path(bundle.string()) << "\",\n  \"seen_bundle\": true,\n  \"declined\": "
        << (declined ? "true" : "false") << "\n}\n";
}

bool bundle_exists(const std::filesystem::path& bundle)
{
    std::error_code ec;
    return std::filesystem::is_directory(bundle, ec) && !ec;
}

// Sleep until the install directory changes, or 200ms passes. A 2s sleep can
// miss a put-back that is trashed again before the next poll, leaving declined set.
void wait_for_install_dir_change(const std::filesystem::path& bundle)
{
    const int dirfd = open(bundle.parent_path().c_str(), O_EVTONLY);
    if (dirfd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }
    const int queue = kqueue();
    if (queue < 0) {
        close(dirfd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }

    struct kevent change{};
    EV_SET(&change, dirfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_LINK | NOTE_ATTRIB, 0, nullptr);
    struct kevent event{};
    struct timespec timeout{};
    timeout.tv_nsec = 200 * 1000 * 1000;
    kevent(queue, &change, 1, &event, 1, &timeout);
    close(queue);
    close(dirfd);
}

bool slicer_still_running()
{
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0 || bytes > 4 * 1024 * 1024)
        bytes = 256 * 1024;
    std::vector<pid_t> pids(static_cast<size_t>(bytes / static_cast<int>(sizeof(pid_t))) + 64);
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    const int buffer_bytes = static_cast<int>(pids.size() * sizeof(pid_t));
    if (bytes <= 0)
        return false;
    if (bytes > buffer_bytes)
        bytes = buffer_bytes;

    const std::string marker = std::string("/") + SnapmakerUninstall::kAppBundleName + "/Contents/MacOS/Snapmaker_Orca";
    const int count = bytes / static_cast<int>(sizeof(pid_t));
    for (int i = 0; i < count; ++i) {
        if (pids[static_cast<size_t>(i)] <= 0)
            continue;
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pids[static_cast<size_t>(i)], path, sizeof(path)) <= 0)
            continue;
        const std::string executable(path);
        if (executable.size() >= marker.size() &&
            executable.compare(executable.size() - marker.size(), marker.size(), marker) == 0)
            return true;
    }
    return false;
}

// Finder mounts the installer image before a drag-replace. The volume root
// holds Snapmaker Orca.app. An unrelated disk image does not count.
bool installer_dmg_mounted()
{
    const CommandResult info = run_command({"/usr/bin/hdiutil", "info"}, true);
    if (info.exit_code != 0 || info.output.empty())
        return false;

    const std::string marker = "/Volumes/";
    for (size_t pos = 0; (pos = info.output.find(marker, pos)) != std::string::npos;) {
        const size_t end = info.output.find_first_of("\r\n", pos);
        std::string mount = info.output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!mount.empty() && (mount.back() == ' ' || mount.back() == '\t'))
            mount.pop_back();
        std::error_code ec;
        const bool has_app = std::filesystem::is_directory(std::filesystem::path(mount) / SnapmakerUninstall::kAppBundleName, ec) && !ec;
        if (has_app)
            return true;
        pos += marker.size();
    }
    return false;
}

// No installer image: the app left /Applications, so ask immediately.
// An installer image is mounted: wait out a replace copy. If the .app returns, this was an update.
// Restoring the trashed app afterwards is ignored; wiped data stays wiped.
bool confirm_trashed_uninstall(const std::filesystem::path& bundle)
{
    if (!installer_dmg_mounted()) {
        log_line("no Snapmaker Orca installer image mounted; asking whether to remove leftovers");
        return true;
    }

    log_line("installer image is mounted; waiting before treating a missing app as an uninstall");
    int steady_missing = 0;
    for (int waited = 0; waited < SnapmakerUninstall::kMaxWaitSeconds; ++waited) {
        if (bundle_exists(bundle)) {
            log_line("bundle is back; treating this as an update, not an uninstall");
            return false;
        }
        if (slicer_still_running())
            steady_missing = 0;
        else
            ++steady_missing;
        if (steady_missing >= SnapmakerUninstall::kSteadyMissingSeconds) {
            log_line("installed app stayed missing and is not running; asking whether to remove leftovers");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    log_line("installed app is missing but Snapmaker Orca is still running; keeping watch");
    return false;
}

enum class Choice { DeleteData, KeepData, Retry };

std::string ascii_lower(std::string text)
{
    for (char& ch : text)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

bool language_is_chinese(const std::string& tag)
{
    return ascii_lower(tag).rfind("zh", 0) == 0;
}

// Software language wins when Snapmaker Orca.conf has one. Otherwise use the macOS preferred language.
bool prompt_in_chinese()
{
    const auto home = SnapmakerUninstall::home_path();
    if (!home.empty()) {
        const auto conf = home / "Library" / "Application Support" / SnapmakerUninstall::kDataDirName / "Snapmaker_Orca.conf";
        if (const auto language = json_string_field(read_text(conf), "language"))
            return language_is_chinese(*language);
    }

    const CommandResult languages = run_command({"/usr/bin/defaults", "read", "-g", "AppleLanguages"}, true);
    const auto open = languages.output.find('"');
    const auto close = open == std::string::npos ? std::string::npos : languages.output.find('"', open + 1);
    if (open == std::string::npos || close == std::string::npos)
        return false;
    return language_is_chinese(languages.output.substr(open + 1, close - open - 1));
}

Choice ask_user()
{
    const auto script_path = SnapmakerUninstall::helper_dir() / "uninstall-prompt.applescript";
    if (SnapmakerUninstall::helper_dir().empty())
        return Choice::Retry;

    const char* script_zh =
        "use framework \"AppKit\"\n"
        "use scripting additions\n"
        "set nsApp to current application's NSApplication's sharedApplication()\n"
        "nsApp's setActivationPolicy:(current application's NSApplicationActivationPolicyRegular)\n"
        "nsApp's activateIgnoringOtherApps:true\n"
        "set alert to current application's NSAlert's alloc()'s init()\n"
        "alert's setMessageText:\"Snapmaker Orca 已从「应用程序」移除。\"\n"
        "alert's setInformativeText:\"勾选「删除个人数据」会清除预设、登录状态、缓存和其他本机配置，且无法恢复。不勾选则不删除任何数据。\"\n"
        "alert's setAlertStyle:(current application's NSAlertStyleWarning)\n"
        "alert's addButtonWithTitle:\"确定\"\n"
        "set row to current application's NSView's alloc()'s initWithFrame:(current application's NSMakeRect(0, 0, 360, 24))\n"
        "set checkbox to current application's NSButton's alloc()'s initWithFrame:(current application's NSMakeRect(4, 0, 340, 24))\n"
        "checkbox's setButtonType:(current application's NSSwitchButton)\n"
        "checkbox's setTitle:\"删除个人数据\"\n"
        "checkbox's setState:0\n"
        "row's addSubview:checkbox\n"
        "alert's setAccessoryView:row\n"
        "alert's |layout|()\n"
        "set alertWindow to alert's |window|()\n"
        "alertWindow's setLevel:(current application's NSFloatingWindowLevel)\n"
        "alertWindow's setCollectionBehavior:2\n"
        "alertWindow's setAnimationBehavior:(current application's NSWindowAnimationBehaviorNone)\n"
        "set hostScreen to current application's NSScreen's mainScreen()\n"
        "if hostScreen is missing value then set hostScreen to first item of (current application's NSScreen's screens())\n"
        "set vis to hostScreen's visibleFrame()\n"
        "set wf to alertWindow's frame()\n"
        "set scrOrigin to item 1 of vis\n"
        "set scrSize to item 2 of vis\n"
        "set winSize to item 2 of wf\n"
        "set freeY to (item 2 of scrSize) - (item 2 of winSize)\n"
        "set originX to (item 1 of scrOrigin) + ((item 1 of scrSize) - (item 1 of winSize)) / 2\n"
        "set originY to (item 2 of scrOrigin) + freeY / 2 + freeY * 0.08\n"
        "alertWindow's setFrameOrigin:(current application's NSMakePoint(originX, originY))\n"
        "alertWindow's orderFrontRegardless()\n"
        "alert's runModal()\n"
        "if (checkbox's state() as integer) is 1 then\n"
        "return \"delete\"\n"
        "end if\n"
        "return \"keep\"\n";
    const char* script_en =
        "use framework \"AppKit\"\n"
        "use scripting additions\n"
        "set nsApp to current application's NSApplication's sharedApplication()\n"
        "nsApp's setActivationPolicy:(current application's NSApplicationActivationPolicyRegular)\n"
        "nsApp's activateIgnoringOtherApps:true\n"
        "set alert to current application's NSAlert's alloc()'s init()\n"
        "alert's setMessageText:\"Snapmaker Orca has been removed from Applications.\"\n"
        "alert's setInformativeText:\"Check \\\"Delete personal data\\\" to remove presets, login state, caches, and other local settings. This cannot be undone. Leave it unchecked to delete nothing.\"\n"
        "alert's setAlertStyle:(current application's NSAlertStyleWarning)\n"
        "alert's addButtonWithTitle:\"OK\"\n"
        "set row to current application's NSView's alloc()'s initWithFrame:(current application's NSMakeRect(0, 0, 360, 24))\n"
        "set checkbox to current application's NSButton's alloc()'s initWithFrame:(current application's NSMakeRect(4, 0, 340, 24))\n"
        "checkbox's setButtonType:(current application's NSSwitchButton)\n"
        "checkbox's setTitle:\"Delete personal data\"\n"
        "checkbox's setState:0\n"
        "row's addSubview:checkbox\n"
        "alert's setAccessoryView:row\n"
        "alert's |layout|()\n"
        "set alertWindow to alert's |window|()\n"
        "alertWindow's setLevel:(current application's NSFloatingWindowLevel)\n"
        "alertWindow's setCollectionBehavior:2\n"
        "alertWindow's setAnimationBehavior:(current application's NSWindowAnimationBehaviorNone)\n"
        "set hostScreen to current application's NSScreen's mainScreen()\n"
        "if hostScreen is missing value then set hostScreen to first item of (current application's NSScreen's screens())\n"
        "set vis to hostScreen's visibleFrame()\n"
        "set wf to alertWindow's frame()\n"
        "set scrOrigin to item 1 of vis\n"
        "set scrSize to item 2 of vis\n"
        "set winSize to item 2 of wf\n"
        "set freeY to (item 2 of scrSize) - (item 2 of winSize)\n"
        "set originX to (item 1 of scrOrigin) + ((item 1 of scrSize) - (item 1 of winSize)) / 2\n"
        "set originY to (item 2 of scrOrigin) + freeY / 2 + freeY * 0.08\n"
        "alertWindow's setFrameOrigin:(current application's NSMakePoint(originX, originY))\n"
        "alertWindow's orderFrontRegardless()\n"
        "alert's runModal()\n"
        "if (checkbox's state() as integer) is 1 then\n"
        "return \"delete\"\n"
        "end if\n"
        "return \"keep\"\n";
    const char* script = prompt_in_chinese() ? script_zh : script_en;

    {
        std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return Choice::Retry;
        out << script;
    }

    const auto result = run_command({"/usr/bin/osascript", script_path.string()}, true);
    std::error_code ec;
    std::filesystem::remove(script_path, ec);

    if (result.exit_code == 0 && result.output.find("delete") != std::string::npos)
        return Choice::DeleteData;
    if (result.exit_code == 0 && result.output.find("keep") != std::string::npos)
        return Choice::KeepData;
    log_line("dialog failed: " + result.output);
    return Choice::Retry;
}

std::string gui_domain()
{
    return std::string("gui/") + std::to_string(getuid());
}

void bootout_self()
{
    const auto service = gui_domain() + "/" + SnapmakerUninstall::kLabel;
    // disable first so KeepAlive cannot start a second copy after this process exits
    run_command({"/bin/launchctl", "disable", service}, false);
    const auto plist = SnapmakerUninstall::launch_agent_plist_path();
    run_command({"/bin/launchctl", "bootout", service}, false);
    if (!plist.empty())
        run_command({"/bin/launchctl", "bootout", gui_domain(), plist.string()}, false);
}

void remove_plist()
{
    const auto plist = SnapmakerUninstall::launch_agent_plist_path();
    if (plist.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(plist, ec);
}

[[noreturn]] void finish(bool delete_user_data)
{
    std::signal(SIGTERM, SIG_IGN);
    if (!delete_user_data) {
        log_line("checkbox off; not deleting anything");
        _exit(0);
    }

    log_line("removing Snapmaker Orca user data: preferences, Application Support, WebKit, caches, HTTP storage, saved state");
    run_command({"/bin/launchctl", "disable", gui_domain() + "/" + SnapmakerUninstall::kLabel}, false);
    SnapmakerUninstall::safe_remove_snapmaker_preferences();
    SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::webkit_data_dir());
    SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::caches_dir());
    SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::http_storages_dir());
    SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::saved_state_dir());
    remove_plist();
    SnapmakerUninstall::safe_remove_exact(SnapmakerUninstall::support_dir());
    bootout_self();
    _exit(0);
}

} // namespace

int main()
{
    if (geteuid() == 0) {
        log_line("refusing to run as root");
        return 0;
    }

    log_line("watching for a Trash uninstall");
    for (;;) {
        const WatchState state = load_state();
        if (!state.valid || !state.seen) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (bundle_exists(state.bundle)) {
            if (state.declined) {
                log_line("bundle is back; clearing declined so the next removal asks again");
                write_watch_state(state.bundle, false);
            }
            wait_for_install_dir_change(state.bundle);
            continue;
        }
        if (state.declined) {
            wait_for_install_dir_change(state.bundle);
            continue;
        }
        if (!confirm_trashed_uninstall(state.bundle)) {
            if (bundle_exists(state.bundle) && state.declined)
                write_watch_state(state.bundle, false);
            wait_for_install_dir_change(state.bundle);
            continue;
        }

        for (;;) {
            if (bundle_exists(state.bundle)) {
                write_watch_state(state.bundle, false);
                break;
            }
            const Choice choice = ask_user();
            if (bundle_exists(state.bundle)) {
                log_line("bundle returned while the dialog was open; not deleting anything");
                write_watch_state(state.bundle, false);
                break;
            }
            if (choice == Choice::DeleteData)
                finish(true);
            if (choice == Choice::KeepData) {
                log_line("checkbox off; not deleting anything");
                write_watch_state(state.bundle, true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }
}
