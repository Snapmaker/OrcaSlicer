#pragma once

// Snapmaker Orca: small decisions of the Snapmaker OTA install path (PresetUpdater.cpp), kept
// GUI-free and header-only so the unit tests reach them without a running application.

#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>

#include "libslic3r/Semver.hpp"

namespace Slic3r {
namespace OtaUpdateRules {

// A cached JSON vendor update is usable only as <vendor>.json plus a non-empty <vendor>/ directory;
// a lone <vendor>.json would raise the installed version over presets never replaced.
// Never throws; a path that cannot be examined counts as incomplete.
inline bool json_cache_entry_complete(const boost::filesystem::path& vendor_json, const boost::filesystem::path& vendor_dir)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(vendor_json, ec) || ec)
        return false;
    if (!boost::filesystem::is_directory(vendor_dir, ec) || ec)
        return false;
    const bool empty = boost::filesystem::is_empty(vendor_dir, ec);
    return !ec && !empty;
}

// True when at least one collected update may be installed. An update whose minimum application
// version is above the running one is collected with can_install == false: it is listed, with the
// reason in its changelog, and skipped by perform_updates().
template<class UpdateRange> bool any_installable(const UpdateRange& updates)
{
    for (const auto& update : updates)
        if (update.can_install)
            return true;
    return false;
}

// The running application's version for the minimum-application-version gate of a package.
// A label Semver cannot parse (for instance "v2.5.0") yields no version instead of throwing.
inline boost::optional<Semver> application_version(const std::string& version_label)
{
    return Semver::parse(version_label);
}

// The gate itself. An application version that is not known cannot be compared, so it does not
// block a package: no gate rather than no updates.
inline bool min_app_version_satisfied(const Semver& min_version, const boost::optional<Semver>& app_version)
{
    return !app_version || min_version <= *app_version;
}

} // namespace OtaUpdateRules
} // namespace Slic3r
