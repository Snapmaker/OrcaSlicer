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
#include <fnmatch.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <syslog.h>
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
    std::fprintf(stderr, "Snapmaker Orca Monitor: %s\n", message.c_str());
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
    state.valid    = SnapmakerUninstall::is_managed_install_path(state.bundle);
    return state;
}

bool bundle_missing(const std::filesystem::path& bundle)
{
    std::error_code ec;
    const auto      status = std::filesystem::symlink_status(bundle, ec);
    return status.type() == std::filesystem::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory);
}

// Sleep until the install directory changes, or 200ms passes. A longer sleep
// can miss a put-back that is trashed again before the next poll.
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
    const char* script_zh =
        "use framework \"AppKit\"\n"
        "use scripting additions\n"
        "set nsApp to current application's NSApplication's sharedApplication()\n"
        "nsApp's setActivationPolicy:(current application's NSApplicationActivationPolicyRegular)\n"
        "nsApp's activateIgnoringOtherApps:true\n"
        "set alert to current application's NSAlert's alloc()'s init()\n"
        "alert's setMessageText:\"Snapmaker Orca已从「应用程序」中移除。\"\n"
        "alert's setInformativeText:\"勾选「删除个人数据」将清除预设、登录状态及缓存等本机配置，此操作无法恢复。\"\n"
        "alert's setAlertStyle:(current application's NSAlertStyleWarning)\n"
        "alert's addButtonWithTitle:\"完成\"\n"
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
        "if (alert's runModal() as integer) is not (current application's NSAlertFirstButtonReturn as integer) then return \"retry\"\n"
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
        "alert's setMessageText:\"Snapmaker Orca has been removed from \\\"Applications\\\".\"\n"
        "alert's setInformativeText:\"Selecting \\\"Delete personal data\\\" will clear slicing presets, login status, cached local data, and other local settings. This action cannot be undone.\"\n"
        "alert's setAlertStyle:(current application's NSAlertStyleWarning)\n"
        "alert's addButtonWithTitle:\"Done\"\n"
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
        "if (alert's runModal() as integer) is not (current application's NSAlertFirstButtonReturn as integer) then return \"retry\"\n"
        "if (checkbox's state() as integer) is 1 then\n"
        "return \"delete\"\n"
        "end if\n"
        "return \"keep\"\n";
    const char* script = prompt_in_chinese() ? script_zh : script_en;

    const auto result = run_command({"/usr/bin/osascript", "-e", script}, true);
    const auto end    = result.output.find_last_not_of("\r\n");
    if (result.exit_code == 0 && end != std::string::npos) {
        const auto separator = result.output.find_last_of("\r\n", end);
        const auto start     = separator == std::string::npos ? 0 : separator + 1;
        const auto choice    = result.output.substr(start, end - start + 1);
        if (choice == "delete")
            return Choice::DeleteData;
        if (choice == "keep")
            return Choice::KeepData;
    }
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
    if (!SnapmakerUninstall::is_unredirected_user_path(plist))
        return;
    std::error_code ec;
    std::filesystem::remove(plist, ec);
}

void remove_http_cookie_files()
{
    namespace fs          = std::filesystem;
    const auto legacy_dir = SnapmakerUninstall::http_storages_dir();
    if (legacy_dir.empty())
        return;

    const auto      dir = legacy_dir.parent_path();
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec)) || !SnapmakerUninstall::is_unredirected_user_path(dir))
        return;

    const std::string cookie_name  = std::string(SnapmakerUninstall::kBundleId) + ".binarycookies";
    const std::string temp_pattern = cookie_name + "_tmp_*.dat";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name != cookie_name && fnmatch(temp_pattern.c_str(), name.c_str(), 0) != 0)
            continue;

        std::error_code file_ec;
        if (fs::is_regular_file(it->symlink_status(file_ec)) && ::unlink(it->path().c_str()) != 0)
            file_ec = std::error_code(errno, std::generic_category());
        if (file_ec)
            log_line("failed to clean up cookie file " + it->path().string() + ": " + file_ec.message());
    }
    if (ec)
        log_line("failed to enumerate HTTP storage " + dir.string() + ": " + ec.message());
}

// Removes the watcher's own files: the copy of the binary, watch.json and the log.
// User data lives beside Helpers and is only touched when delete_user_data is set.
// Entries are removed one by one so watcher.log stays in place until the end:
// failures of earlier removals are then logged while the log can still record them.
void remove_helper_dir()
{
    const auto dir = SnapmakerUninstall::helper_dir();
    if (!SnapmakerUninstall::is_unredirected_user_path(dir))
        return;

    const std::string log_name = "watcher.log";
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    if (ec) {
        log_line("failed to list helper dir " + dir.string() + ": " + ec.message());
        return;
    }

    for (; it != end; it.increment(ec)) {
        const std::filesystem::path entry = it->path();
        if (entry.filename().string() == log_name)
            continue;
        std::error_code remove_ec;
        std::filesystem::remove_all(entry, remove_ec);
        if (remove_ec)
            log_line("failed to remove helper file " + entry.string() + ": " + remove_ec.message());
    }
    if (ec)
        log_line("failed to enumerate helper dir " + dir.string() + ": " + ec.message());

    const auto log_path = dir / log_name;
    std::error_code log_ec;
    std::filesystem::remove(log_path, log_ec);
    if (log_ec)
        log_line("failed to remove helper log " + log_path.string() + ": " + log_ec.message());

    std::error_code dir_ec;
    std::filesystem::remove(dir, dir_ec);
    if (dir_ec)
        log_line("failed to remove helper dir " + dir.string() + ": " + dir_ec.message());
}

void remove_user_data_dir(const std::filesystem::path& path)
{
    if (!SnapmakerUninstall::safe_remove_exact(path))
        syslog(LOG_ERR, "Snapmaker Orca Monitor: could not safely remove %s", path.c_str());
}

// The watcher always removes itself once the prompt is answered: the app is
// gone, so the launch agent has no reason to stay. The checkbox only decides
// whether the user data goes with it. Reinstalling and launching Snapmaker
// Orca registers a fresh watcher.
[[noreturn]] void finish(bool delete_user_data)
{
    std::signal(SIGTERM, SIG_IGN); // bootout delivers SIGTERM; ignore it so cleanup can finish
    run_command({"/bin/launchctl", "disable", gui_domain() + "/" + SnapmakerUninstall::kLabel}, false);

    if (delete_user_data) {
        log_line("removing Snapmaker Orca user data: Application Support, WebKit, caches, HTTP storage");
        remove_user_data_dir(SnapmakerUninstall::webkit_data_dir());
        remove_user_data_dir(SnapmakerUninstall::caches_dir());
        remove_user_data_dir(SnapmakerUninstall::http_storages_dir());
        remove_http_cookie_files();
    }

    remove_helper_dir();
    remove_plist();
    if (delete_user_data)
        remove_user_data_dir(SnapmakerUninstall::support_dir());
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
        if (!bundle_missing(state.bundle)) {
            wait_for_install_dir_change(state.bundle);
            continue;
        }

        const Choice     choice  = ask_user();
        const WatchState current = load_state();
        if (!current.valid || !current.seen || current.bundle != state.bundle || !bundle_missing(state.bundle)) {
            log_line("installation state changed while the dialog was open; skipping cleanup");
            continue;
        }
        if (choice == Choice::DeleteData)
            finish(true);
        if (choice == Choice::KeepData)
            finish(false);
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}
