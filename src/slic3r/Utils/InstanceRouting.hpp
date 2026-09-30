#ifndef slic3r_Utils_InstanceRouting_hpp_
#define slic3r_Utils_InstanceRouting_hpp_

// Who receives the files another EdgeSlicer process hands over (`EdgeSlicer.exe --single-instance
// a.stl`, used by "Send to EdgeSlicer" in Blender, by Explorer and by the phone hub).
//
// GUI/InstanceCheck.cpp does the real work with windows, lock files and D-Bus; the decisions are
// kept here, free of wxWidgets and the platform headers, so tests/slic3rutils/instance_routing_tests.cpp
// can cover them.
//
// The rules:
//  * An instance is identified by a hash of its own executable path, so a hand-off only ever goes to
//    another process of the same executable. The path is normalised first (case, separators, the
//    \?\ prefix, junctions resolved by the caller) so that the same install reached by two
//    spellings is one instance.
//  * Only an instance somebody can see receives hand-offs. A hidden instance (started with
//    --hidden / SNORCA_HIDDEN, "Start hidden", or by the phone hub) never claims the lock and is never
//    chosen as the target; it may still hand its own launch arguments to a visible instance.
//  * When no visible instance is found, the launch starts a normal instance instead of exiting with
//    the files dropped.
//  * The headless `--hub` process never reaches the single-instance check at all.

#include <cstdint>
#include <optional>
#include <string>

namespace Slic3r {
namespace InstanceRouting {

// Whether this process starts hidden. Precedence: SNORCA_HIDDEN (non-empty; "0" means visible, anything
// else hidden) > the --hidden command line option > the "start_hidden" preference.
inline bool resolve_hidden_start(const std::optional<std::string> &env_value, bool cli_hidden, bool config_hidden)
{
    if (env_value && !env_value->empty())
        return *env_value != "0";
    if (cli_hidden)
        return true;
    return config_hidden;
}

// The key an executable path is hashed from. `path` should already be absolute with junctions and
// symlinks resolved when the platform can do that; this removes the remaining spelling differences of a
// Windows path: the extended-length prefix, forward slashes, letter case and a trailing separator.
inline std::string normalize_exe_path_key(const std::string &path)
{
    std::string p = path;
    if (p.rfind("\\?\UNC\\", 0) == 0)
        p = "\\\\" + p.substr(8);
    else if (p.rfind("\\?\\", 0) == 0)
        p = p.substr(4);
    for (char &c : p) {
        if (c == '/')
            c = '\';
        else if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    while (p.size() > 1 && p.back() == '\')
        p.pop_back();
    return p;
}

// A window (Windows) may receive a hand-off when it belongs to the same executable and can be seen.
// A hidden hub-managed slicer has a main window too; it must be skipped, or the files load into a
// window nobody can look at.
inline bool is_hand_off_target(std::uint64_t my_hash, std::uint64_t window_hash, bool window_visible)
{
    return window_visible && my_hash == window_hash;
}

// This launch hands its files over and exits only when it was asked to, another instance holds the
// lock AND a visible one was found to receive them. Otherwise it starts normally, so the files are
// never silently dropped.
inline bool should_hand_off(bool hand_off_requested, bool other_instance_holds_lock, bool visible_target_found)
{
    return hand_off_requested && other_instance_holds_lock && visible_target_found;
}

// Hidden instances do not claim the lock, so they can never be picked as the target.
inline bool claims_instance_lock(bool hidden_start)
{
    return !hidden_start;
}

} // namespace InstanceRouting
} // namespace Slic3r

#endif // slic3r_Utils_InstanceRouting_hpp_
