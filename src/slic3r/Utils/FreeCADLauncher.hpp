#ifndef slic3r_Utils_FreeCADLauncher_hpp_
#define slic3r_Utils_FreeCADLauncher_hpp_

// "Edit in FreeCAD": find an installed FreeCAD (0.21 or 1.x), start it on a part exported by the
// FreeCAD bridge (GUI/FreeCADBridge.cpp), and install the EdgeSlicer add-on
// (resources/freecad/EdgeSlicerBridge) into FreeCAD's user Mod folder.
//
// Discovery, the session macro and the add-on folders are pure so they can be unit tested without
// a real filesystem, registry or process launch (tests/slic3rutils/freecad_launcher_tests.cpp); the
// platform adapters at the bottom gather the real inputs. install_addon() works on real folders and
// is tested against a temporary directory.

#include <string>
#include <vector>

#include "ExternalEditor.hpp"

namespace Slic3r {
namespace FreeCADLauncher {

using FileExists = ExternalEditor::FileExists;
using ListDirs   = ExternalEditor::ListDirs;
using Platform   = ExternalEditor::Platform;

// One "FreeCAD" entry of Windows' installed-programs list (HKLM/HKCU ...\Uninstall\<key>).
struct RegistryInstall
{
    std::string display_name;     // "FreeCAD 1.0.2"
    std::string display_version;  // "1.0.2"
    std::string display_icon;     // "C:\Program Files\FreeCAD 1.0\bin\FreeCAD.exe" (may carry ",0")
    std::string install_location; // often empty
    std::string uninstall_string; // "\"C:\Program Files\FreeCAD 1.0\Uninstall-FreeCAD.exe\""
};

// What discovery looks at, gathered by the caller so the precedence logic stays testable.
struct Environment
{
    Platform                     platform = Platform::Linux;
    // Windows: Program Files roots to look in, 64-bit first.
    std::vector<std::string>     program_files;
    // Windows: %LOCALAPPDATA%, for per-user installs in %LOCALAPPDATA%\Programs.
    std::string                  local_app_data;
    // Windows: the program .FCStd files open with, or empty.
    std::string                  fcstd_association;
    // Windows: installed-programs entries whose name starts with "FreeCAD".
    std::vector<RegistryInstall> registry;
    // macOS / Linux: the user's home directory.
    std::string                  home;
    // macOS / Linux: `freecad` (or `FreeCAD`) resolved through PATH, or empty.
    std::string                  freecad_on_path;
    FileExists                   exists;
    ListDirs                     list_dirs;
};

// "FreeCAD 1.0" -> {1, 0}; "FreeCAD 0.21.2" -> {0, 21, 2}. False for anything else.
bool parse_install_dir_version(const std::string &dir_name, std::vector<int> &version);

// The newest "FreeCAD X.Y[.Z]" folder among `dir_names`, or empty.
std::string newest_install_dir(const std::vector<std::string> &dir_names);

// A user-typed location: FreeCAD.exe itself, its bin folder or the install folder (Windows), the
// FreeCAD.app bundle (macOS) or the executable / AppImage (Linux). The executable when it exists,
// else empty.
std::string resolve_custom_path(const std::string &path, Platform platform, const FileExists &exists);

// .FCStd files open with FreeCAD.exe. Anything else (FreeCADCmd, a zip tool, a viewer) gives an
// empty string, so an unrelated handler is never launched.
std::string freecad_exe_from_association(const std::string &exe);

// The FreeCAD.exe an installed-programs entry stands for (DisplayIcon, then InstallLocation, then
// the uninstaller's folder), or empty when none of them exists.
std::string freecad_exe_from_registry(const RegistryInstall &entry, const FileExists &exists);

// A macOS .app bundle path becomes the binary inside it; any other path is returned unchanged.
std::string macos_bundle_binary(const std::string &path);

// The command (executable plus any fixed leading arguments) that starts FreeCAD, or empty when
// none was found. A `custom_path` that resolves wins; then, per platform:
//   Windows: the .FCStd association, the newest registered install, the newest
//            "FreeCAD X.Y" (or plain "FreeCAD") folder in each Program Files root, then per-user
//            installs in %LOCALAPPDATA%\Programs.
//   macOS:   /Applications/FreeCAD.app, ~/Applications/FreeCAD.app, the newest
//            /Applications/FreeCAD X.Y.app, then `freecad` on PATH.
//   Linux:   `freecad` on PATH, the Snap, then the Flathub Flatpak (system or user install).
std::vector<std::string> discover(const std::string &custom_path, const Environment &env);

// Full argv for an edit session: FreeCAD runs the session macro, which opens the part and arms
// the save hook. --single-instance hands the macro to a FreeCAD that is already open.
std::vector<std::string> edit_session_args(const std::vector<std::string> &freecad_command, const std::string &session_macro);

// A Python string literal for `utf8` (double quotes, backslashes and control characters escaped).
std::string python_string_literal(const std::string &utf8);

// The per-session macro (edgeslicer_edit.FCMacro) EdgeSlicer writes next to the exported part.
// It loads the bridge module from `bridge_dir` (resources/freecad/EdgeSlicerBridge) unless an
// installed add-on of the same or a newer version is already loaded, then starts the session
// described by `session_json`.
std::string session_macro(const std::string &bridge_dir, const std::string &session_json);

// FreeCAD's user Mod folders the add-on goes into:
//   Windows: %APPDATA%\FreeCAD\Mod (0.21, 1.0) plus <...>\FreeCAD\vX-Y\Mod for every versioned
//            user folder that exists (1.1 and later keep one per version).
//   macOS:   ~/Library/Application Support/FreeCAD/Mod plus the versioned folders.
//   Linux:   $XDG_DATA_HOME (~/.local/share)/FreeCAD/Mod plus the versioned folders, ~/.FreeCAD/Mod
//            when that legacy folder exists, and the Flatpak's data folder when it is installed.
// `base` is %APPDATA% on Windows, $HOME elsewhere; `xdg_data_home` is only used on Linux.
std::vector<std::string> addon_mod_dirs(Platform platform, const std::string &base, const std::string &xdg_data_home,
                                        const FileExists &exists, const ListDirs &list_dirs);

// "v1-1" -> true. FreeCAD 1.1+ names its per-version user folders like this.
bool is_versioned_user_dir(const std::string &name);

struct InstallReport
{
    std::vector<std::string> installed; // <mod dir>/EdgeSlicerBridge for every successful copy
    std::vector<std::string> errors;
};

// Copies the add-on folder `source_dir` into every folder of `mod_dirs` (as
// <mod dir>/EdgeSlicerBridge, replacing an older copy) and records `edgeslicer_exe` in it, so
// "Send to EdgeSlicer" in FreeCAD reaches this install. Real filesystem; true when at least one
// copy succeeded and none failed.
bool install_addon(const std::string &source_dir, const std::vector<std::string> &mod_dirs, const std::string &edgeslicer_exe, InstallReport &report);

// Name of the add-on folder inside Mod, and of the file holding the EdgeSlicer location.
extern const char *ADDON_FOLDER;
extern const char *LOCATION_FILE;

// -------------------------------------------------------------------------------------------
// Platform adapters (real filesystem / registry / process launch, not unit tested).
// -------------------------------------------------------------------------------------------

// discover() with the real environment of this machine.
std::vector<std::string> find_freecad(const std::string &custom_path);

// addon_mod_dirs() for this machine's user.
std::vector<std::string> find_addon_mod_dirs();

// Starts `argv` detached. False when the process could not be started.
bool launch(const std::vector<std::string> &argv);

} // namespace FreeCADLauncher
} // namespace Slic3r

#endif // slic3r_Utils_FreeCADLauncher_hpp_
