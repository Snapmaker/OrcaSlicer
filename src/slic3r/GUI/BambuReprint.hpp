#pragma once

// Reprinting an archived Bambu job from the phone (Reprint tab): the job read back from the
// .gcode.3mf the archive kept, matched against the target printer's AMS exactly the way the
// desktop's send dialog matches a sliced plate (BambuSendMapping), with the checks that dialog
// makes before it lets a job go - and the phone's own overrides, per filament, on top.
//
// RemoteSend drives it: prepare_from_record() for a Bambu record loads the Job (any thread), steps
// onto the GUI thread for evaluate() against the live MachineObject, and hands the result to the
// same run_bambu() the phone's plate sends use - the network plug-in's LAN upload and project_file
// start, never a second implementation of the protocol.
//
// The pure parts (load_job, parse_choices, apply_choices, check, preview naming) take no app state
// and are unit tested (tests/slic3rutils/bambu_reprint_tests.cpp).

#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/BambuNozzleMappingRequest.hpp"
#include "libslic3r/ProjectTask.hpp" // FilamentInfo

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
namespace GUI {
namespace BambuReprint {

// One filament the archived plate prints with.
struct Filament
{
    int                 id { -1 };      // 0-based project filament index (the G-code's T number)
    std::string         type;           // "PLA", as the send dialog names it
    std::string         color;          // "#RRGGBBAA", upper case (the dialog's m_filaments form)
    std::string         filament_id;    // the filament preset id ("GFA00"), may be empty
    double              used_g { 0.0 };
    int                 extruder { -1 }; // logical extruder it was sliced for: 0 left, 1 right; -1 one-extruder job
    std::vector<int>    group_ids;       // logical nozzle groups (slice_info group_id)
};

// A sliced plate as the archive holds it: everything a send needs, read from the file itself -
// Metadata/slice_info.config (plate, model, filaments, nozzle groups), project_settings.config
// (filament ids, physical_extruder_map, bed type, nozzle flow types) and filament_sequence.json.
struct Job
{
    std::string                     error;      // non-empty: the file cannot be sent (says why)
    int                             plate { 1 }; // 1-based plate number inside the file (Metadata/plate_<n>.gcode)
    std::string                     printer_model_id; // "O1D", "BL-P001" - the model it was sliced for
    std::string                     printer_model;    // "Bambu Lab H2D" (project_settings printer_model)
    std::vector<double>             nozzle_diameters; // per logical extruder
    std::vector<int>                nozzle_volume_types; // NozzleVolumeType per logical extruder
    std::vector<Filament>           filaments;  // the ones this plate prints with, by id
    size_t                          project_filament_count { 0 };
    std::vector<std::string>        filament_ids; // per project filament
    std::vector<int>                filament_map; // 1-based extruder per project filament; empty for a one-extruder job
    std::vector<int>                physical_extruder_map;
    std::vector<BambuNozzleMapping::FilamentNozzle> filament_nozzles; // (filament, nozzle) pairs of the slice
    std::vector<int>                filament_change_sequence; // 0-based filament ids in print order
    std::string                     bed_type;   // G-code form ("textured_plate"), what PrintJob sends
    bool                            timelapse_warning { false }; // the slice could not make a timelapse
    int                             prediction_s { 0 };
    double                          weight_g { 0.0 };

    bool dual() const { return nozzle_diameters.size() == 2; }
    // The send dialog's m_filaments: what BambuSendMapping::auto_map matches against the AMS.
    std::vector<FilamentInfo> filament_infos() const;
    const Filament* find(int id) const;
};

// Any thread. Reads a .gcode.3mf with the desktop's own reader (load_gcode_3mf_from_stream - what
// the Device tab's storage browser uses to print a file from the printer).
Job load_job(const std::string& path);

// One tray of the target printer, as the phone's mapping sheet shows it.
struct Tray
{
    int         ams_id { -1 };
    int         slot_id { -1 };
    int         tray_id { -1 };        // ams_id * 4 + slot_id, MachineObject::ams_filament_mapping's numbering
    int         physical_extruder { -1 }; // Ams::nozzle
    int         extruder { -1 };        // logical (0 left, 1 right) on a two-extruder printer, else -1
    std::string name;                   // "A1", "HT-A"
    std::string type;                   // what the mapping compares ("PLA")
    std::string display_type;           // what the printer's screen shows ("Sup.PLA")
    std::string color;                  // "#RRGGBBAA", as the printer reports it
    std::vector<std::string> colors;    // multi-colour spools
    int         ctype { 0 };
    std::string filament_id;            // tray_info_idx
    bool        exists { false };       // a spool is in the slot
    bool        ready { false };        // its type and colour are known: it can be picked
};

// GUI thread. Every slot of every AMS unit of the printer (external spool holders are not offered,
// exactly as the desktop's tray picker does not offer them). `physical_extruder_map` is the job's.
std::vector<Tray> trays_of(MachineObject* obj, const std::vector<int>& physical_extruder_map);

// The phone's override of one filament: "<filament>:<ams_id>-<slot_id>", comma separated,
// filament 0-based ("0:0-2,3:128-0").
struct Choice
{
    int filament { -1 };
    int ams_id { -1 };
    int slot_id { -1 };
};
bool parse_choices(const std::string& text, std::vector<Choice>& out, std::string& error);
// The wire form of a mapping result (only the filaments that have a tray).
std::string choices_text(const std::vector<FilamentInfo>& result);

// Puts each choice into `result` (the auto mapping, one entry per job filament): the tray's id,
// colour, type and ams/slot, as the desktop's tray picker does. False with `error` when a choice
// names a filament the job does not print with, or a tray that does not exist or holds nothing.
bool apply_choices(const Job& job, const std::vector<Tray>& trays, const std::vector<Choice>& choices,
                   std::vector<FilamentInfo>& result, std::string& error);

// Why a mapping cannot print as sliced, per filament.
struct Problem
{
    int         filament { -1 };
    std::string code;   // unmapped | type | side
    std::string text;
};
// Every used filament must have a tray, of its type, feeding the extruder it was sliced for (the
// dialog's PrintStatusAmsMappingInvalid and PrintStatusAmsMappingWrongExtruder).
std::vector<Problem> check(const Job& job, const std::vector<Tray>& trays, const std::vector<FilamentInfo>& result);

// "left" / "right" / "" for a logical extruder.
std::string side_word(int extruder);
// "Filament 2 (PETG, black)" - how the refusals name a filament.
std::string filament_label(const Filament& f);

// What the phone asked for, besides the printer.
struct Request
{
    std::string mode { "print" };   // print | upload
    std::string mapping;            // Choice list, "" = the automatic mapping
    int         bed_leveling { -1 }, flow_cali { -1 }, timelapse { -1 }, use_ams { -1 }, nozzle_offset_cali { -1 };
    bool        force { false };    // send although the nozzle diameter or hardness does not match
};

// Everything evaluate() worked out.
struct Evaluation
{
    int                       status { 200 }; // 200, or the refusal's HTTP status
    std::string               error;          // the refusal, in words the phone shows
    nlohmann::json            preview;        // the mapping sheet's data (see RemoteAccess's manifest)
    bool                      can_send { false };
    std::vector<FilamentInfo> result;         // the final mapping, one entry per job filament
    // Options, resolved the way the send dialog resolves its checkboxes.
    bool        bed_leveling { true }, flow_cali { true }, timelapse { false }, use_ams { true };
    int         auto_offset_cali { 0 };       // 0 off, 2 auto (dual-nozzle only)
    bool        has_sdcard { false };
    // The strings PrintJob hands the plug-in.
    std::string ams_mapping, ams_mapping2, ams_mapping_info, nozzles_info, nozzle_mapping_request;
};

// The target printer as the checks see it: a snapshot of the live MachineObject (snapshot()), plain
// data so the checks can be tested without a printer.
struct Printer
{
    std::string id, name, model;             // dev_id, dev_name, printer_type ("O1D")
    bool        online { false };
    bool        lan_mode { false };           // LAN-only printer
    bool        access_code_known { false };
    bool        lan_route { false };          // IP address and access code known: the plug-in can reach it
    bool        upgrading { false }, system_printing { false }, printing { false }, filament_changing { false };
    std::string printing_what;                // the running job's name
    int         extruder_count { 0 };         // 0 = not reported yet
    bool        has_ams { false }, ams_mapping_supported { true }, nozzle_rack { false };
    bool        i3 { false };                 // an A1-style printer (timelapse starts off)
    int         sdcard { 0 };                 // MachineObject::SdcardState
    std::vector<std::string>         compatible_models; // DeviceManager::get_compatible_machine(model)
    std::vector<std::vector<double>> diameters;   // per logical extruder: the nozzles it has (rack included)
    std::vector<int>                 nozzle_hrc;  // per logical extruder: the current nozzle's hardness, 0 unknown
    std::vector<Tray>                trays;
    std::vector<DualNozzleSync::PrinterNozzle> nozzles; // for the nozzle-mapping request
};

// What the checks read from the app: the send dialog's remembered checkboxes and the hardness a
// filament type needs. app_env() is the live one.
struct Env
{
    std::function<bool(const char*)>         remembered;   // AppConfig "print" <key>: unset or "1" = on
    std::function<int(const std::string&)>   required_hrc; // PresetBundle::get_required_hrc_by_filament_type
};

// The send dialog's checks and mapping for `job` on `pr`. `automatic` is the dialog's automatic
// mapping on that printer (BambuSendMapping::auto_map), one entry per job filament. A refusal sets
// status and error (the printer is offline, busy, of another model, has another nozzle, has no
// storage...); mapping problems leave status 200 with can_send false and the problems in the
// preview, so the phone can show them and let the person pick other trays.
Evaluation evaluate(const Printer& pr, const Job& job, const Request& req, const Env& env, const std::vector<FilamentInfo>& automatic);

// GUI thread: the live pieces.
Printer    snapshot(MachineObject* obj, const Job& job);
Env        app_env();
// snapshot() + the automatic mapping on `obj` + app_env() + evaluate().
Evaluation evaluate(MachineObject* obj, const Job& job, const Request& req);

} // namespace BambuReprint
} // namespace GUI
} // namespace Slic3r
