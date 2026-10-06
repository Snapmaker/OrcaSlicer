#ifndef slic3r_Utils_BlenderLauncher_hpp_
#define slic3r_Utils_BlenderLauncher_hpp_

// "Edit in Blender": find an installed Blender and start it on a part exported by the Blender
// bridge (GUI/BlenderBridge.cpp), with resources/blender/edgeslicer_bridge.py loading the part.
//
// Discovery and argument construction are pure so they can be unit tested without a real
// filesystem, registry or process launch (tests/slic3rutils/blender_launcher_tests.cpp); the
// platform adapters at the bottom gather the real inputs.

#include <functional>
#include <string>
#include <vector>

#include "ExternalEditor.hpp"

namespace Slic3r {
namespace BlenderLauncher {

// Shared with the other external-editor launchers (Utils/ExternalEditor.hpp).
using FileExists = ExternalEditor::FileExists;
using ListDirs   = ExternalEditor::ListDirs;
using Platform   = ExternalEditor::Platform;

// What discovery looks at, gathered by the caller so the precedence logic stays testable.
struct Environment
{
    Platform                 platform = Platform::Linux;
    // Windows: Program Files roots to look in, 64-bit first ("C:\\Program Files", "... (x86)").
    std::vector<std::string> program_files;
    // Windows: the program .blend files open with (usually blender-launcher.exe), or empty.
    std::string              blend_association;
    // macOS: the user's home directory, for ~/Applications.
    std::string              home;
    // macOS / Linux: `blender` resolved through PATH, or empty.
    std::string              blender_on_path;
    FileExists               exists;
    ListDirs                 list_dirs;
};

// "Blender 4.2" -> 4, 2. False for anything else ("Blender", "Blender Launcher").
bool parse_install_dir_version(const std::string &dir_name, int &major, int &minor);

// The newest "Blender X.Y" folder among `dir_names` (4.10 is newer than 4.9), or empty.
std::string newest_install_dir(const std::vector<std::string> &dir_names);

// .blend files open with blender-launcher.exe, which starts blender.exe without a console window
// but does not wait for it. blender.exe beside it is preferred when present; otherwise `exe` is
// returned unchanged. Anything whose file name is not a Blender executable gives an empty string,
// so an unrelated .blend handler is never launched.
std::string blender_exe_from_association(const std::string &exe, const FileExists &exists);

// A macOS .app bundle path becomes the binary inside it; any other path is returned unchanged.
std::string macos_bundle_binary(const std::string &path);

// The command (executable plus any fixed leading arguments) that starts Blender, or empty when
// none was found. A non-empty `custom_path` that exists wins; then, per platform:
//   Windows: the .blend association, the newest "Blender Foundation\\Blender X.Y" in each Program
//            Files root, then Steam's default library.
//   macOS:   /Applications/Blender.app, ~/Applications/Blender.app, then `blender` on PATH.
//   Linux:   `blender` on PATH, the Snap, then the Flathub Flatpak (system or user install).
std::vector<std::string> discover(const std::string &custom_path, const Environment &env);

struct EditSession
{
    std::string script;         // edgeslicer_bridge.py
    std::string input;          // the part as EdgeSlicer exported it
    std::string output;         // where Blender writes the edited part back
    std::string name;           // the part's name, used for the Blender object
    std::string edgeslicer_exe; // so Blender's "Send to EdgeSlicer" can find this install
};

// Full argv for an edit session: `blender_command` followed by the bridge script and its options.
std::vector<std::string> edit_session_args(const std::vector<std::string> &blender_command, const EditSession &session);

// -------------------------------------------------------------------------------------------
// Platform adapters (real filesystem / registry / process launch, not unit tested).
// -------------------------------------------------------------------------------------------

// discover() with the real environment of this machine.
std::vector<std::string> find_blender(const std::string &custom_path);

// Starts `argv` detached. False when the process could not be started.
bool launch(const std::vector<std::string> &argv);

} // namespace BlenderLauncher
} // namespace Slic3r

#endif // slic3r_Utils_BlenderLauncher_hpp_
