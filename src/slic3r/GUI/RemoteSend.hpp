#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "GcodeArchive.hpp"
#include "SnapmakerLan.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

namespace Slic3r {
class MachineObject;
namespace GUI {

// Sending a sliced plate to a printer for the phone / agent API (RemoteAccess), without any of the
// desktop's dialogs. It composes exactly what SelectMachineDialog + PrintJob (upload and print),
// SendToPrinterDialog + SendJob (upload only) and the print-host / Snapmaker branch of
// Plater::send_gcode_legacy would send, so a phone send looks like a desktop send to the printer.
namespace RemoteSend {

struct Request
{
    int         plate { -1 };
    std::string printer;           // a Bambu dev_id, "host" (the printer preset's print host) or
                                   // "connect" (the Snapmaker connected on the PC's Device tab)
    std::string mode;              // "upload" | "print"
    bool        confirm { false }; // mode=print needs it: the phone asks the person first
    bool        force { false };   // send although the printer's model differs from the sliced profile
    bool        dry_run { false }; // everything but the upload / print command (SNORCA_SEND_DRYRUN=1 forces it)
    // Bambu options; -1 = the desktop's remembered choice (AppConfig section "print").
    int         bed_leveling { -1 }, flow_cali { -1 }, vibration_cali { -1 }, timelapse { -1 }, use_ams { -1 };
    std::string name;              // print host: the file name on the printer (default: the export name)
    // Snapmaker over the LAN: which toolhead prints which of the file's filaments,
    // "<filament>:<toolhead>,..." (0-based). Empty = the auto-match the printer's own app makes.
    std::string mapping;
    // Snapmaker over the LAN: "unload the filaments this print used when it ends". Empty = the
    // printer preset's own unload_filaments_at_end (or, for a reprint, what the archived send did);
    // "1"/"0" overrides it for this send only, the way the desktop send dialog's checkbox does.
    std::string unload_at_end;
    // Stage 2 (a reprint): the G-code archive record whose bytes are the payload. Set by
    // /api/archive/{id}/send only; a plate send leaves it empty.
    std::string record;
    // A Bambu reprint: which AMS slot feeds each of the job's filaments,
    // "<filament>:<ams_id>-<slot_id>,..." (0-based filament). Empty = the send dialog's automatic
    // mapping (BambuSendMapping); the preview (/api/archive/{id}/preview) hands out that mapping in
    // this form, and the phone sends it back, edited or not.
    std::string ams_mapping;
    // Bambu two-nozzle printers: nozzle offset calibration (-1 = the desktop's remembered choice).
    int         nozzle_offset_cali { -1 };
};

// Everything prepare() worked out on the GUI thread; run() only performs the transfer.
struct Prepared
{
    std::string kind;              // bambu | printhost | connect
    std::string mode;              // upload | print
    int         plate { -1 };
    bool        dry_run { false };
    std::string printer_id, printer_name;
    // Bambu (the network plugin)
    BBL::PrintParams params {};
    std::string      call;                          // the NetworkAgent function the desktop would call
    bool             verify_access_code { false };  // PrintJob's tiny upload that proves IP + access code first
    bool             lan_fallback_to_cloud { false };
    int              print_error_before { 0 };      // the printer's error code before the send
    // Print host (Moonraker / OctoPrint / … and the connected Snapmaker)
    std::shared_ptr<PrintHost> host;
    PrintHostUpload  upload {};
    bool             two_step { false };            // upload with print=false, then printer.print.start over MQTT
    // One of this printer model's devices (<datadir>/hub/print_host_devices.json), when the target
    // was "ph:<id>" rather than the preset's own address. Empty for the plain "host" target.
    std::string      device_id, model_key;
    // Snapmaker over the LAN (Moonraker HTTP): no host object, just the printer and the mapping.
    SnapmakerLan::Device                    lan {};
    std::string                             lan_filename;
    std::vector<SnapmakerLan::FileFilament> file_filaments;
    std::vector<int>                        mapping;      // toolhead per file filament, -1 = unused
    std::vector<SnapmakerLan::Toolhead>     toolheads;
    // The printer preset's unload_filaments_at_end, for a Snapmaker tool changer. The flag cannot
    // ride in the G-code (the firmware refuses print_task_config commands while it is printing),
    // so it goes with the print start as END_UNLOAD_FILAMENT.
    bool                                    unload_at_end { false };
    // Whether this printer can do it at all (is_snapmaker_toolchanger). The send sheet shows its
    // toggle only when it can, and the dry run reports both so the sheet needs no second request.
    bool                                    unload_supported { false };
    // Ultra: what the G-code archive records about this send, gathered on the GUI thread by
    // prepare() so run() only has to copy the file it uploaded.
    GcodeArchive::Meta archive_meta;
    // Stage 2: this send replays an archived record. Its bytes are already in the archive, so
    // run() neither archives them again nor deducts filament for a plate that is not loaded.
    bool        from_record { false };
    std::string record_id;
    // A reprint to a printer that still holds the file (the same name, the same size): start it
    // where it is instead of uploading it again. Snapmaker over the LAN only.
    bool        reuse_remote { false };
    // Stage 1d: after a successful send, ask Spoolman to deduct this plate's filament the way the
    // desktop's own send path does. Decided on the GUI thread in prepare(), where the preference
    // and the Spoolman URL can be read; run() only fires it.
    bool        spoolman_deduct { false };
};

struct Sink
{
    std::function<void(int percent, const std::string& text)>                              progress;
    std::function<void(bool ok, const std::string& error, const nlohmann::json& result)>   done;
};

// GUI thread, first. A Bambu printer other than the selected one is selected (and so connected)
// like picking it in the dialog's combo box; `wait` then says the caller should give its status
// (SD card, connection) a moment to arrive before prepare(): the GUI thread must not block for it.
// Anything else returns {200, ""} with wait = false. Errors as prepare().
std::pair<int, std::string> preselect(const Request& req, bool& wait);
// GUI thread. True once the printer's connection and status have arrived (or it is not a Bambu one).
bool printer_ready(const std::string& printer);

// GUI thread. Validates the request against the plater and the printer, exports the plate file the
// way the desktop does and composes the parameters. {200, ""} with `out` filled, or an HTTP status
// and an error text for the phone.
std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out);

// Stage 2, a reprint: the same Prepared, filled from a G-code archive record instead of from the
// plater. There is no plate and no project - the archived bytes are the payload and the sidecar is
// the memory - so nothing is re-sliced and no project has to be open. Any thread but the GUI one:
// it talks to the printer, and only steps onto the GUI thread for the printer preset. Errors as
// prepare(), plus 409 when the record's file is gone or the target printer is of another kind.
std::pair<int, std::string> prepare_from_record(const Request& req, std::shared_ptr<Prepared>& out);

// A Bambu reprint's mapping sheet, before anything is sent: the record's job read back from its
// .gcode.3mf, the target printer selected (and so connected, as picking it in the send dialog
// does) and matched the way prepare_from_record() would send it - the proposed AMS slot per
// filament, the printer's slots, the print options with their defaults, and what stands in the way.
// `out` is that JSON. Any thread but the GUI one. Errors as prepare_from_record(): a printer that
// cannot take the job at all (offline, busy, another model or nozzle, no storage) is a 409 with
// the reason; mapping problems are not errors - they are in the preview, with can_send false.
std::pair<int, std::string> preview_record(const Request& req, nlohmann::json& out);

// Which send path a printer id names, without asking anything: "sm:<id>" snapmaker, "host" and
// "ph:<device>" printhost, "connect" connect, anything else a Bambu serial.
std::string printer_kind_of(const std::string& id);

// Worker thread. The upload / print start (or the dry run); reports through the sink and always
// ends with sink.done.
void run(std::shared_ptr<Prepared> p, Sink sink);

// GUI thread. The file name the desktop's own export gives a plate ("<project>_plate_2.gcode"),
// for any plate, without making it the current one on the PC. `plate` out of range = the current
// plate; an empty extension gives the bare name.
std::string export_name_for(int plate, const std::string& extension);

// GUI thread. What /api/printers adds for the send UI: a Bambu machine's send capabilities and
// option defaults, and the print-host / connected-Snapmaker entries. `plate` says which plate the
// `upload_name` defaults are for (-1 = the current one).
void describe_bambu(MachineObject* m, nlohmann::json& p);
void list_hosts(nlohmann::json& printers, int plate = -1);
// The upload name a Snapmaker over the LAN gets for `plate` (always the plate's G-code, whatever
// the preset's vendor). GUI thread; the LAN listing itself happens off it.
std::string lan_upload_name(int plate = -1);

} // namespace RemoteSend
} // namespace GUI
} // namespace Slic3r
