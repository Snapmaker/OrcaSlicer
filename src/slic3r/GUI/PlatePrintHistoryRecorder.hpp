#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/PlatePrintHistory.hpp"

namespace Slic3r { namespace GUI {

// The one place every send path reports a finished send (or an export) to, so each plate's history
// (PlatePrintHistory.hpp) is filled the same way whichever printer, host or phone it went through.
//
// Rules for callers:
//   * call it only after the send succeeded (or the file was written); it records what it is told.
//   * it never throws and never blocks a send: from a worker thread it queues the work on the GUI
//     thread and returns.
//   * a send that failed, was cancelled, or is a replay of an archived file (a reprint: it has no
//     plate in this project) records nothing.
namespace PlateHistoryRecorder {

struct Send
{
    // 0-based plate indices. -1 (PLATE_CURRENT_IDX) = the current plate, -2 (PLATE_ALL_IDX) = every
    // plate. Empty = the current plate.
    std::vector<int>      plates;
    std::string           printer_name;
    std::string           printer_model;
    // bambu_lan, bambu_cloud, snapmaker_lan, snapmaker_cloud, phone_hub, a print host's lower-case
    // name (octoprint, prusalink, moonraker, ...), or file.
    std::string           connection;
    std::string           file_name;
    PlateHistory::Action  action { PlateHistory::Action::Sent };
    // Optional entry id, so a later step of the same send can find the entry (upgrade()).
    std::string           uid;
};

// Lower-case key for a PrintHost::get_name(): "Creality Print" -> "creality_print".
std::string connection_key_for_host(const std::string &host_name);

// GUI thread only. Records on the plates the send names and returns (plate index, entry uid) for
// each plate recorded.
std::vector<std::pair<int, std::string>> record_now(const Send &send);

// Any thread: queues record_now() on the GUI thread.
void record(const Send &send);

// An exported G-code / sliced plate file. Any thread. `plate` as in Send::plates (one value).
void record_export(int plate, const std::string &file_path);

// Any thread: an entry's action changes once the printer is told to start the upload.
void upgrade(int plate, const std::string &uid, PlateHistory::Action action);

// Any thread: the name and model a Bambu dev_id is shown with, from the device manager.
void bambu_identity(const std::string &dev_id, std::string &name, std::string &model);

} // namespace PlateHistoryRecorder
}} // namespace Slic3r::GUI
