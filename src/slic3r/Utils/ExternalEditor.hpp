#ifndef slic3r_Utils_ExternalEditor_hpp_
#define slic3r_Utils_ExternalEditor_hpp_

// What the "Edit in <program>" bridges share below the GUI: finding an installed program and
// starting it. BlenderLauncher and FreeCADLauncher build on this; GUI/ExternalEditorBridge is the
// GUI half (session folders, watching the file the program writes back, swapping the part's mesh).
//
// The pure helpers take their inputs as arguments so the launchers' discovery rules can be unit
// tested without a real filesystem, registry or process launch; the adapters at the bottom gather
// the real inputs.

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace ExternalEditor {

using FileExists = std::function<bool(const std::string &path)>;
// Names (not full paths) of the directories directly inside `dir`; empty when it does not exist.
using ListDirs = std::function<std::vector<std::string>(const std::string &dir)>;

enum class Platform { Windows, MacOS, Linux };

std::string to_lower(std::string s);
bool        starts_with_nocase(const std::string &s, const std::string &prefix);
bool        ends_with_nocase(const std::string &s, const std::string &suffix);
// The last component of a path written with either separator.
std::string file_name(const std::string &path);
// Everything before the last separator, or empty.
std::string parent_dir(const std::string &path);
std::string strip_trailing_separators(std::string path);

// "<prefix>X.Y" (case-insensitive prefix, e.g. "Blender " or "FreeCAD ") -> {X, Y}. A version has
// between `min_parts` and `max_parts` dot-separated numbers; anything else gives false.
bool parse_dir_version(const std::string &dir_name, const std::string &prefix, std::vector<int> &version, size_t min_parts, size_t max_parts);

// The folder among `dir_names` with the highest version (1.10 is newer than 1.9), or empty.
std::string newest_versioned_dir(const std::vector<std::string> &dir_names, const std::string &prefix, size_t min_parts, size_t max_parts);

// "1.0.2" < "1.1"; missing parts count as 0. Non-numeric parts compare as 0.
int compare_versions(const std::string &a, const std::string &b);

// -------------------------------------------------------------------------------------------
// Platform adapters (real filesystem / registry / process launch, not unit tested).
// -------------------------------------------------------------------------------------------

Platform                 this_platform();
bool                     real_exists(const std::string &path);
std::vector<std::string> real_list_dirs(const std::string &dir);
// UTF-8 value of an environment variable, or empty.
std::string              env_var(const char *name);
// `name` resolved through PATH, or empty.
std::string              search_path(const std::string &name);
// Windows: "C:\\Program Files", "... (x86)" without duplicates. Empty elsewhere.
std::vector<std::string> program_files_roots();
// Windows: the program files with this extension (".blend") open with, or empty.
std::string              association_executable(const std::string &extension);

// Starts `argv` detached. False when the process could not be started. `log_tag` prefixes the
// log lines ("BlenderLauncher").
bool launch(const std::vector<std::string> &argv, const std::string &log_tag);

} // namespace ExternalEditor
} // namespace Slic3r

#endif // slic3r_Utils_ExternalEditor_hpp_
