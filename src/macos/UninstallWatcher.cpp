// User-level helper. Launchd keeps a single job alive outside the .app so a
// drag-to-Trash still has something that can ask whether to delete leftovers.
// It does not relaunch Snapmaker Orca, and it does not need an administrator password.

#include "macos/UninstallWatcherCommand.hpp"
#include "macos/UninstallWatcherLifecycle.hpp"
#include "macos/UninstallWatcherLock.hpp"
#include "macos/UninstallWatcherState.hpp"

#include <algorithm>
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
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/event.h>
#include <syslog.h>
#include <unistd.h>

namespace {

// Track the helper child so a SIGTERM from launchctl bootout can close a pending
// dialog before the watcher exits; otherwise osascript may outlive its parent
// and leave a modal window whose answer nobody reads.
volatile sig_atomic_t g_running_child = 0;

void terminate_running_child(int)
{
    const pid_t pid = static_cast<pid_t>(g_running_child);
    if (pid > 0)
        ::kill(pid, SIGKILL);
    _exit(0);
}

using SnapmakerUninstall::CommandResult;

/** @brief Runs watcher commands while keeping SIGTERM aware of the currently owned child. */
CommandResult run_command(const std::vector<std::string>& args, bool capture, bool interactive = false)
{
    return SnapmakerUninstall::run_watcher_command(args, capture, g_running_child, interactive);
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

/** @brief Reads a `key = value` line from the app's INI-style configuration file. */
std::optional<std::string> ini_string_field(const std::string& text, const char* key)
{
    const std::string key_text(key);
    std::optional<std::string> value;
    std::size_t line_start = 0;
    while (line_start < text.size()) {
        const std::size_t newline = text.find('\n', line_start);
        const std::size_t line_stop = newline == std::string::npos ? text.size() : newline;
        const std::size_t first = text.find_first_not_of(" \t\r", line_start);
        if (first < line_stop && text.compare(first, key_text.size(), key_text) == 0) {
            const std::size_t equals = text.find_first_not_of(" \t", first + key_text.size());
            if (equals < line_stop && text[equals] == '=') {
                const std::size_t value_start = text.find_first_not_of(" \t", equals + 1);
                if (value_start < line_stop) {
                    std::size_t value_end = line_stop;
                    while (value_end > value_start &&
                           (text[value_end - 1] == ' ' || text[value_end - 1] == '\t' || text[value_end - 1] == '\r'))
                        --value_end;
                    value = text.substr(value_start, value_end - value_start);
                }
            }
        }
        line_start = newline == std::string::npos ? text.size() : newline + 1;
    }
    return value;
}

/** @brief Installation snapshot whose identity scopes one uninstall confirmation. */
struct WatchState
{
    std::filesystem::path bundle;
    std::string           registration_id;
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
    state.bundle          = std::filesystem::path(*bundle).lexically_normal();
    state.registration_id = SnapmakerUninstall::registration_id_from_state(text);
    state.seen            = json_bool_true(text, "seen_bundle");
    state.valid           = SnapmakerUninstall::is_managed_install_path(state.bundle) &&
                            SnapmakerUninstall::valid_registration_id(state.registration_id);
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
    const int n = kevent(queue, &change, 1, &event, 1, &timeout);
    close(queue);
    close(dirfd);
    // An EV_ADD failure is reported as an EV_ERROR event (positive return),
    // not a negative one. Without this fallback a volume that rejects vnode
    // monitoring would spin the poll loop at full speed. A plain timeout
    // (n == 0) must keep its normal 200ms cadence, so do not sleep on it.
    if (n < 0 || (n > 0 && (event.flags & EV_ERROR)))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

// Old-style plist output is `( en, "zh-Hans", ... )`: plain language codes are
// unquoted, so use the first array element rather than the first quoted string.
std::string first_apple_language(const std::string& output)
{
    const auto open = output.find('(');
    std::size_t pos = open == std::string::npos ? 0 : open + 1;
    while (pos < output.size() && std::isspace(static_cast<unsigned char>(output[pos])))
        ++pos;
    if (pos >= output.size())
        return {};
    if (output[pos] == '"') {
        const auto close = output.find('"', pos + 1);
        if (close == std::string::npos)
            return {};
        return output.substr(pos + 1, close - pos - 1);
    }
    const auto end = output.find_first_of(",)\r\n", pos);
    return output.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// Software language wins when Snapmaker Orca.conf has one. Otherwise use the macOS preferred language.
bool prompt_in_chinese()
{
    const auto home = SnapmakerUninstall::home_path();
    if (!home.empty()) {
        const auto conf = home / "Library" / "Application Support" / SnapmakerUninstall::kDataDirName / "Snapmaker_Orca.conf";
        const std::string conf_text = read_text(conf);
        if (const auto language = json_string_field(conf_text, "language"))
            return language_is_chinese(*language);
        // The app writes an INI-style `language = <code>` line (boost::property_tree::read_ini).
        if (const auto language = ini_string_field(conf_text, "language"))
            return language_is_chinese(*language);
    }

    const CommandResult languages = run_command({"/usr/bin/defaults", "read", "-g", "AppleLanguages"}, true);
    return language_is_chinese(first_apple_language(languages.output));
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

    const auto result = run_command({"/usr/bin/osascript", "-e", script}, true, true);
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
    const CommandResult disabled = run_command({"/bin/launchctl", "disable", service}, false);
    if (disabled.exit_code != 0)
    {
        syslog(LOG_ERR, "Snapmaker Orca Monitor: launchctl disable failed: %d", disabled.exit_code);
        log_line("launchctl disable failed: " + std::to_string(disabled.exit_code));
    }
    const CommandResult booted = run_command({"/bin/launchctl", "bootout", service}, false);
    if (booted.exit_code != 0)
    {
        syslog(LOG_ERR, "Snapmaker Orca Monitor: launchctl bootout failed: %d", booted.exit_code);
        log_line("launchctl bootout failed: " + std::to_string(booted.exit_code));
    }
    const auto plist = SnapmakerUninstall::launch_agent_plist_path();
    if (!plist.empty())
    {
        const CommandResult booted_plist = run_command({"/bin/launchctl", "bootout", gui_domain(), plist.string()}, false);
        if (booted_plist.exit_code != 0)
        {
            syslog(LOG_ERR, "Snapmaker Orca Monitor: launchctl bootout plist failed: %d", booted_plist.exit_code);
            log_line("launchctl bootout plist failed: " + std::to_string(booted_plist.exit_code));
        }
    }
}

void remove_plist()
{
    const auto plist = SnapmakerUninstall::launch_agent_plist_path();
    if (!SnapmakerUninstall::is_unredirected_user_path(plist))
        return;
    std::error_code ec;
    std::filesystem::remove(plist, ec);
    if (ec.value() != 0) {
        syslog(LOG_ERR, "Snapmaker Orca Monitor: could not remove the watcher plist: %s", ec.message().c_str());
        log_line("could not remove the watcher plist: " + ec.message());
    }
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

/** @brief Removes preferences after flushing the domain; a timeout stops this cleanup attempt. */
bool remove_preferences_plist()
{
    namespace fs = std::filesystem;
    const auto path = SnapmakerUninstall::preferences_plist_path();
    if (path.empty())
        return true;

    // Flush the domain through cfprefsd first: on older systems an unlink
    // alone lets cfprefsd write its cached values back into the file. A
    // missing domain exits non-zero, which is expected; the file itself is
    // still removed below.
    const CommandResult flushed = run_command({"/usr/bin/defaults", "delete", SnapmakerUninstall::kBundleId}, false);
    if (flushed.timed_out || flushed.exit_code < 0)
    {
        return false;
    }

    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory))
        return true;
    if (ec || !fs::is_regular_file(status) || !SnapmakerUninstall::is_unredirected_user_path(path)) {
        log_line("could not safely remove preferences file " + path.string());
        return true;
    }

    if (::unlink(path.c_str()) != 0)
        log_line("failed to remove preferences file " + path.string() + ": " + std::strerror(errno));
    return true;
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

    const std::string log_name = SnapmakerUninstall::kLogFileName;
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
    if (!SnapmakerUninstall::safe_remove_exact(path)) {
        syslog(LOG_ERR, "Snapmaker Orca Monitor: could not safely remove %s", path.c_str());
        log_line("could not safely remove " + path.string());
    }
}

/** @brief Restores normal watcher termination if a cleanup attempt returns for a retry. */
struct CleanupSignalGuard
{
    using Handler    = void (*)(int);
    Handler previous = std::signal(SIGTERM, SIG_IGN);

    /** @brief Restores the previous handler before releasing the lifecycle lock. */
    ~CleanupSignalGuard()
    {
        if (previous != SIG_ERR)
        {
            std::signal(SIGTERM, previous);
        }
    }
};

/**
 * @brief Attempts cleanup under the lifecycle lock; returns a reason to retry.
 * @param delete_user_data Whether the confirmed cleanup should also remove personal data.
 */
SnapmakerUninstall::CleanupResult try_finish(bool delete_user_data)
{
    const SnapmakerUninstall::LifecycleLock lock(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (!lock.locked())
    {
        return lock.timed_out() ? SnapmakerUninstall::CleanupResult::LockBusy
                                : SnapmakerUninstall::CleanupResult::LockUnavailable;
    }

    const CleanupSignalGuard signals; // bootout delivers SIGTERM; ignore it so cleanup can finish
    if (run_command({"/bin/launchctl", "disable", gui_domain() + "/" + SnapmakerUninstall::kLabel}, false).exit_code !=
        0)
    {
        return SnapmakerUninstall::CleanupResult::CommandFailed;
    }

    if (delete_user_data) {
        log_line("removing Snapmaker Orca user data: Application Support, WebKit, caches, HTTP storage, preferences, saved state");
        if (!remove_preferences_plist())
        {
            return SnapmakerUninstall::CleanupResult::CommandFailed;
        }
        remove_user_data_dir(SnapmakerUninstall::webkit_data_dir());
        remove_user_data_dir(SnapmakerUninstall::caches_dir());
        remove_user_data_dir(SnapmakerUninstall::http_storages_dir());
        remove_http_cookie_files();
        remove_user_data_dir(SnapmakerUninstall::saved_state_dir());
    }

    remove_helper_dir();
    bootout_self();
    remove_plist();
    if (delete_user_data)
        remove_user_data_dir(SnapmakerUninstall::support_dir());
    // Keep the lifecycle lock until process exit releases it, after the final launchctl operation.
    _exit(0);
}

/** @brief Best-effort self-removal without the lifecycle lock; used only on terminal failures. */
void remove_watcher_without_lock()
{
    const CleanupSignalGuard signals; // bootout delivers SIGTERM; ignore it so removal can finish
    remove_helper_dir();
    bootout_self();
    remove_plist();
    _exit(0);
}

/** @brief Removes watcher files without user data once a retry limit has been reached. */
void terminal_self_removal(SnapmakerUninstall::CleanupResult result)
{
    // A retry limit is terminal: remove only the watcher, never a user-data directory.
    syslog(LOG_ERR,
           "Snapmaker Orca Monitor: cleanup retry limit reached (%d); removing the watcher without deleting user data",
           static_cast<int>(result));
    log_line("cleanup retry limit reached; removing the watcher without deleting user data");
    const CleanupSignalGuard signals; // bootout delivers SIGTERM; ignore it so self-removal can finish
    const SnapmakerUninstall::LifecycleLock lock(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (lock.locked())
    {
        remove_helper_dir();
        bootout_self();
        remove_plist();
        // Keep the lifecycle lock until process exit releases it, after the final launchctl operation.
        _exit(0);
    }
    remove_watcher_without_lock();
}

/** @brief Retains the confirmation while retrying; a retry limit removes the watcher without user data. */
void finish(bool delete_user_data)
{
    std::optional<SnapmakerUninstall::CleanupResult> reported_failure;
    SnapmakerUninstall::finish_confirmed_cleanup(
        [delete_user_data]() { return try_finish(delete_user_data); },
        [&reported_failure](SnapmakerUninstall::CleanupResult result)
        {
            if (!reported_failure.has_value() || *reported_failure != result)
            {
                log_line(result == SnapmakerUninstall::CleanupResult::CommandFailed ?
                             "cleanup command failed; retaining confirmation and retrying cleanup" :
                         result == SnapmakerUninstall::CleanupResult::LockBusy ?
                             "lifecycle lock is busy; retaining confirmation and retrying cleanup" :
                             "lifecycle lock is unavailable; retaining confirmation and retrying cleanup");
                reported_failure = result;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        },
        terminal_self_removal);
}

} // namespace

int main()
{
    std::signal(SIGTERM, terminate_running_child);

    if (geteuid() == 0) {
        log_line("refusing to run as root");
        return 0;
    }

    log_line("watching for a Trash uninstall");
    unsigned retry_delay   = 30;
    unsigned dialog_failed = 0;
    // A dialog that never succeeds (for example a broken osascript) must not keep
    // the watcher alive forever; give up and self-remove without touching user data.
    constexpr unsigned kMaxDialogFailures = 10;
    constexpr unsigned kMaxInvalidStatePolls = 30; // ~1 minute at the 2 s poll interval
    unsigned invalid_state_polls = 0;
    for (;;) {
        const WatchState state = load_state();
        if (!state.valid || !state.seen) {
            // A watcher whose state never becomes readable can never prompt; self-remove so
            // nothing stays resident (the app registers a fresh watcher on its next launch).
            if (++invalid_state_polls >= kMaxInvalidStatePolls) {
                log_line("watcher state has been invalid for too long; removing the watcher");
                syslog(LOG_ERR, "Snapmaker Orca Monitor: watcher state has been invalid for too long; removing the watcher");
                remove_watcher_without_lock();
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        invalid_state_polls = 0;
        if (!bundle_missing(state.bundle)) {
            wait_for_install_dir_change(state.bundle);
            continue;
        }

        const Choice choice = ask_user();
        if (choice == Choice::DeleteData || choice == Choice::KeepData)
        {
            finish(choice == Choice::DeleteData);
            dialog_failed = 0;
            continue;
        }
        if (++dialog_failed >= kMaxDialogFailures) {
            log_line("dialog failed " + std::to_string(dialog_failed) + " times; removing the watcher");
            syslog(LOG_ERR, "Snapmaker Orca Monitor: dialog failed %u times; removing the watcher", dialog_failed);
            finish(false);
            dialog_failed = 0;
            continue;
        }
        // Bound the retry log growth when the dialog keeps failing.
        std::this_thread::sleep_for(std::chrono::seconds(retry_delay));
        retry_delay = std::min(retry_delay * 2, 300u);
    }
}
