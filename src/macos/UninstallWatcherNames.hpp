#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

// Shared by Snapmaker Orca and the user-level UninstallWatcher.
// The running helper lives outside the .app; the copy inside Contents/MacOS is only the seed.

namespace SnapmakerUninstall {

inline constexpr char kLabel[]        = "com.snapmaker.orca-monitor";
inline constexpr char kBundleId[]     = "com.snapmaker.snapmaker-orca";
inline constexpr char kAppBundleName[] = "Snapmaker Orca.app";
inline constexpr char kDataDirName[]  = "Snapmaker_Orca";
inline constexpr char kHelperFileName[] = "Snapmaker Orca Monitor";
inline constexpr char kWatchFileName[]  = "watch.json";

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

inline bool is_unredirected_user_path(const std::filesystem::path& path)
{
    namespace fs    = std::filesystem;
    const auto home = home_path().lexically_normal();
    if (!home.is_absolute() || !path.is_absolute())
        return false;
    const auto relative = path.lexically_normal().lexically_relative(home);
    if (relative.empty() || relative == "." || *relative.begin() == "..")
        return false;

    std::error_code ec;
    const auto      canonical_home = fs::canonical(home, ec);
    if (ec)
        return false;
    const auto actual = fs::weakly_canonical(path, ec);
    return !ec && actual == canonical_home / relative;
}

// Removes `expected` only when it is exactly one of the Snapmaker Orca library
// directories. Refuses the home folder, ~/Library, or a symlink that resolves elsewhere.
inline bool safe_remove_exact(const std::filesystem::path& expected)
{
    namespace fs = std::filesystem;
    if (expected.empty() ||
        (expected != support_dir() && expected != webkit_data_dir() && expected != caches_dir() && expected != http_storages_dir()))
        return false;

    std::error_code ec;
    const auto      status = fs::symlink_status(expected, ec);
    if (status.type() == fs::file_type::not_found)
        return !ec || ec == std::errc::no_such_file_or_directory;
    if (ec || !fs::is_directory(status) || !is_unredirected_user_path(expected))
        return false;

    fs::remove_all(expected, ec);
    return !ec;
}

} // namespace SnapmakerUninstall
