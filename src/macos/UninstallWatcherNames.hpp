#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

// Shared by Snapmaker Orca and the user-level UninstallWatcher.
// The running helper lives outside the .app; the copy inside Contents/MacOS is only the seed.

namespace SnapmakerUninstall {

inline constexpr char kLabel[]        = "com.snapmaker.snapmaker-orca.uninstaller-watcher";
inline constexpr char kBundleId[]     = "com.snapmaker.snapmaker-orca";
inline constexpr char kAppBundleName[] = "Snapmaker Orca.app";
inline constexpr char kDataDirName[]  = "Snapmaker_Orca";
inline constexpr char kHelperFileName[] = "SnWathcer";
inline constexpr char kWatchFileName[]  = "watch.json";

// Used only while an installer disk image that contains Snapmaker Orca.app is
// mounted. A replace copy can leave /Applications empty for the whole copy.
// With no such image mounted, disappearance is treated as a Trash uninstall immediately.
inline constexpr int kSteadyMissingSeconds = 45;
// Give up this disappearance episode if the bundle has not stayed missing long enough.
inline constexpr int kMaxWaitSeconds = 180;

inline std::filesystem::path home_path()
{
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return {};
    return std::filesystem::path(home);
}

inline std::filesystem::path support_dir()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "Application Support" / kDataDirName;
}

inline std::filesystem::path helper_dir()
{
    const auto support = support_dir();
    if (support.empty())
        return {};
    return support / "Helpers";
}

inline std::filesystem::path installed_helper_path()
{
    const auto dir = helper_dir();
    if (dir.empty())
        return {};
    return dir / kHelperFileName;
}

inline std::filesystem::path watch_state_path()
{
    const auto dir = helper_dir();
    if (dir.empty())
        return {};
    return dir / kWatchFileName;
}

inline std::filesystem::path launch_agent_plist_path()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "LaunchAgents" / (std::string(kLabel) + ".plist");
}

inline std::filesystem::path webkit_data_dir()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "WebKit" / kBundleId;
}

inline std::filesystem::path caches_dir()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "Caches" / kBundleId;
}

inline std::filesystem::path http_storages_dir()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "HTTPStorages" / kBundleId;
}

inline bool is_managed_install_path(const std::filesystem::path& bundle)
{
    if (bundle.empty() || !bundle.is_absolute())
        return false;
    const auto normal = bundle.lexically_normal();
    if (normal.filename() != kAppBundleName)
        return false;
    const auto parent = normal.parent_path();
    if (parent == "/Applications")
        return true;
    const auto home = home_path();
    return !home.empty() && parent == (home / "Applications").lexically_normal();
}

inline bool is_removable_snapmaker_dir(const std::filesystem::path& path)
{
    const auto name   = path.filename().string();
    const auto parent = path.parent_path().filename().string();
    const bool data_dir = name == kDataDirName && parent == "Application Support";
    const bool web_dir  = name == kBundleId && (parent == "WebKit" || parent == "Caches" || parent == "HTTPStorages");
    const bool saved_state = name == std::string(kBundleId) + ".savedState" && parent == "Saved Application State";
    return data_dir || web_dir || saved_state;
}

inline std::filesystem::path saved_state_dir()
{
    const auto home = home_path();
    if (home.empty())
        return {};
    return home / "Library" / "Saved Application State" / (std::string(kBundleId) + ".savedState");
}

inline bool is_snapmaker_preference_file(const std::filesystem::path& path)
{
    const auto name   = path.filename().string();
    const auto parent = path.parent_path().filename().string();
    const std::string bundle_plist = std::string(kBundleId) + ".plist";
    const std::string bundle_dot   = std::string(kBundleId) + ".";
    if (parent == "Preferences" && name == bundle_plist)
        return true;
    if (parent != "ByHost" || path.parent_path().parent_path().filename() != "Preferences")
        return false;
    return name.rfind(bundle_dot, 0) == 0 && name.size() > bundle_dot.size() + 5 &&
           name.compare(name.size() - 6, 6, ".plist") == 0;
}

inline void safe_remove_snapmaker_preferences()
{
    namespace fs = std::filesystem;
    const auto home = home_path();
    if (home.empty())
        return;
    const fs::path prefs = (home / "Library" / "Preferences").lexically_normal();
    std::error_code ec;
    const fs::path direct = prefs / (std::string(kBundleId) + ".plist");
    if (is_snapmaker_preference_file(direct))
        fs::remove(direct, ec);

    const fs::path byhost = prefs / "ByHost";
    if (!fs::is_directory(byhost, ec) || ec)
        return;
    for (fs::directory_iterator it(byhost, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_snapmaker_preference_file(it->path()))
            continue;
        std::error_code remove_ec;
        fs::remove(it->path(), remove_ec);
    }
}

// Removes `expected` only when it is exactly one of the Snapmaker Orca library
// directories. Refuses the home folder, ~/Library, or a symlink that resolves elsewhere.
inline bool safe_remove_exact(const std::filesystem::path& expected)
{
    namespace fs = std::filesystem;
    const auto home = home_path();
    if (expected.empty() || home.empty() || !is_removable_snapmaker_dir(expected.lexically_normal()))
        return false;

    std::error_code ec;
    if (!fs::exists(expected, ec) || ec)
        return !ec;

    const fs::path actual = fs::weakly_canonical(expected, ec);
    if (ec || !is_removable_snapmaker_dir(actual))
        return false;

    const fs::path library = fs::weakly_canonical(home / "Library", ec);
    if (ec)
        return false;
    const auto relative = actual.lexically_relative(library).generic_string();
    if (relative.empty() || relative == "." || relative.compare(0, 2, "..") == 0)
        return false;

    fs::remove_all(actual, ec);
    return !ec;
}

} // namespace SnapmakerUninstall
