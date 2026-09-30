#ifndef GUI_PROCESS_HPP
#define GUI_PROCESS_HPP

#include <string>
#include <vector>


class wxWindow;
class wxString;

namespace Slic3r {
namespace GUI {

// Start a new slicer instance, optionally with a file to open.
void start_new_slicer(const wxString *path_to_open = nullptr, bool single_instance = false);
void start_new_slicer(const std::vector<wxString>& files, bool single_instance = false);

// Start the running executable again with the command line this process was started with
// (--datadir and the like), without the files and links in skip_args. For a restart: call it only
// once this process has let go of its single-instance lock (GUI_App::OnExit does), or the new one
// would hand over to it and quit.
void relaunch_slicer(int argc, char** argv, const std::vector<std::string>& skip_args);

// Start a new G-code viewer instance, optionally with a file to open.
void start_new_gcodeviewer(const wxString *path_to_open = nullptr);
// Open a file dialog, ask the user to select a new G-code to open, start a new G-code viewer.
void start_new_gcodeviewer_open_file(wxWindow *parent = nullptr);

} // namespace GUI
} // namespace Slic3r

#endif // GUI_PROCESS_HPP
