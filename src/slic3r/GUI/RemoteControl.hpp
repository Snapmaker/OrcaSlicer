#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/PrintHost.hpp"

namespace Slic3r {
class MachineObject;
namespace GUI {

// Pausing, resuming and stopping a running print for the phone / agent API (RemoteAccess), without
// any of the desktop's dialogs. It sends what StatusPanel's own Pause / Resume / Stop buttons send:
// MachineObject::command_task_pause / _resume / _abort for a Bambu printer, gated by the same
// can_pause() / can_resume() / can_abort() predicates, and Moonraker's print controls for a
// Snapmaker. Nothing here can start a print - that stays in RemoteSend, behind its own confirm.
//
// On top of those three it carries the printer-error actions: the buttons the desktop's error
// dialog draws for one specific error code, offered remotely for the subset a person can judge
// without standing at the printer (PrintErrorCommands.hpp decides which). They are separate verbs
// rather than a flag on "resume" because the commands are different - an error-aware resume
// carries "err", "job_id" and param "reserve", and firmware that wants those silently drops the
// generic one - and because "ignore" must never be reachable by a person who pressed "resume".
namespace RemoteControl {

struct Request
{
    std::string printer;           // a Bambu dev_id, "host" (the printer preset's print host),
                                   // "connect" (the Snapmaker connected on the PC's Device tab) or
                                   // "sm:<id>" (a Snapmaker on the LAN list, SnapmakerLan.hpp)
    std::string action;            // "pause" | "resume" | "stop", or one of the error verbs below
    bool        confirm { false }; // stop and stop_error need it: the phone asks the person first
    bool        dry_run { false }; // work everything out but send nothing (SNORCA_SEND_DRYRUN=1 forces it)
    // The printer-error actions (PrintErrorCommands.hpp): resume_error | stop_error |
    // ignore_error | idle_ignore_error | ack_proceed | dont_remind | ack_close. Unlike the three
    // generic controls these are about one specific error, so the request has to name it: `err` is
    // the eight-hex code the status JSON carries, and the route refuses (409) unless it is still
    // the code the printer is reporting right now. A status page a person left open for ten
    // minutes is exactly how a resume meant for a filament runout would otherwise land on the
    // nozzle-crash that replaced it.
    std::string err;               // "0C00402D" - required by every error verb, ignored by the rest
    // The settings verbs (DeviceControls.hpp): set_temp heater=<bed|chamber|nozzle<N>>&target=<C>,
    // set_speed value=<level or %>, set_light on=<1|0>, set_fan fan=<id>&percent=<0..100>. Carried as
    // the form spelled them and checked against the printer's own `controls` before anything goes out.
    std::string heater, target, value, fan, percent, on;
    // load_filament / unload_filament: ams=<AMS id, 128+ for an AMS HT, 254 / 255 for an external
    // spool>&slot=<0..3> on a Bambu printer; slot=<toolhead 0..3> (no ams) on a Snapmaker U1.
    std::string ams, slot;
};

// Everything prepare() worked out on the GUI thread; run() only sends the command.
struct Prepared
{
    std::string kind;       // bambu | printhost | connect | snapmaker (the LAN list)
    std::string action;     // pause | resume | stop, or one of the error verbs
    bool        dry_run { false };
    std::string printer_id, printer_name;
    // Bambu: the MachineObject command the desktop's own buttons call.
    std::string command;            // pause | resume | stop (what goes into the MQTT payload)
    std::string call;               // command_task_pause | command_task_resume | command_task_abort
    std::string status_before;      // print_status when the command was composed
    int         print_error_before { 0 };
    // An error action: what prepare() read off the printer while it held the GUI thread, so run()
    // sends the same command the check passed on rather than re-reading a state that has moved.
    bool        is_error_action { false };
    std::string err_code;           // the eight-hex code this action targets, as verified
    std::string err_arg;            // what the command carries: std::to_string(print_error)
    std::string job_id;             // the printer's job_id, for the commands that need one
    // The blob ack_proceed / dont_remind build their payloads out of, copied off the printer while
    // prepare() held the GUI thread, for the same reason err_code is: by the time run() publishes,
    // the object may have been handed a blob for a different error. Null for every other verb.
    nlohmann::json action_json;
    // Moonraker (a Snapmaker over the LAN, or any Moonraker print host): one POST.
    std::string url;                // http://<printer>/printer/print/{pause,resume,cancel}
    std::string moonraker_method;   // printer.print.pause | .resume | .cancel (the MQTT name)
    std::shared_ptr<PrintHost> host; // only for "connect": the live MQTT host, used if the POST fails
    // A settings verb (set_temp | set_speed | set_light | set_fan): what prepare() checked, and what
    // run() sends. For a Moonraker printer `script` is the G-code and `url` its /printer/gcode/script
    // POST; for a Bambu printer `call` names the MachineObject method and these are its arguments.
    bool        is_setting { false };
    std::string setting_text;       // "Bed to 60 C", for the job text and the log
    std::string script;             // the Klipper G-code, Moonraker printers only
    std::string heater;             // set_temp: bed | chamber | nozzle<N>
    int         int_value { 0 };    // the target, the speed level / %, the fan value sent (0..255)
    int         extruder_id { -1 }; // set_temp on a nozzle: the Bambu extruder id
    bool        dual_nozzle { false }; // command_set_nozzle_new (per extruder) rather than command_set_nozzle
    int         fan_type { 0 };     // MachineObject::FanType
    bool        light_on { false };
    // load_filament / unload_filament on a Bambu printer: the arguments of
    // command_ams_change_filament, as StatusPanel's Load / Unload work them out.
    std::string fil_ams, fil_slot;
    int         fil_old_temp { -1 }, fil_new_temp { -1 };
    bool        multi_extruders { false };
    // How long a Moonraker command may take: a filament load or unload runs for minutes and the
    // printer answers the script only when it is done.
    int         timeout_s { 15 };
};

struct Sink
{
    std::function<void(int percent, const std::string& text)>                            progress;
    std::function<void(bool ok, const std::string& error, const nlohmann::json& result)> done;
};

// GUI thread. Validates the request against the printer's own state and composes the command.
// {200, ""} with `out` filled, or an HTTP status and an error text for the phone.
std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out);

// Worker thread. Sends the command (or, on a dry run, does not) and watches what the printer does;
// reports through the sink and always ends with sink.done.
void run(std::shared_ptr<Prepared> p, Sink sink);

// GUI thread. What GET /api/printers adds for the control buttons of one Bambu printer:
// can_pause / can_resume / can_stop, print_status, the current stage and the print error, and the
// native controls: `controls` (DeviceControls::to_json - heaters with limits, speed level, chamber
// light, fans) and `ams` (every AMS unit with its side, humidity and trays).
//
// print_error is {code, message, job_id, has_details, actions[]} while one is reported and null
// otherwise. `actions` is the dialog's own button set for that code, resolved through the same
// hms_action table and the same resolver the desktop uses, each entry
// {id, verb, label, needs_job_id, needs_details, remote_safe} - so the hub page and the app draw
// what the desktop would draw, and know which of them they are allowed to press.
//
// `has_details` is true only while the printer's latest refused command left an action_json blob
// for this very code; the two actions with needs_details=true are remote_safe only then, which is
// the same rule needs_job_id follows.
void describe_bambu(MachineObject* m, nlohmann::json& p);

// Whether an action is one of the settings verbs (set_temp, set_speed, set_light, set_fan).
bool is_setting_verb(const std::string& action);
// load_filament / unload_filament.
bool is_filament_verb(const std::string& action);

// GUI thread. The action ids the desktop's error dialog would draw for this printer and this
// code: the shipped hms_action_<devtype>.json entry, StatusPanel's 0300-800x liveview special
// case, and resolve_print_error_actions' generic fallback - the one place that knows the whole
// rule, so the status JSON, the event payload and the control route's own check cannot disagree
// about what an error offers. Empty when there is no HMS query yet.
std::vector<int> resolved_print_error_actions(const std::string& dev_id, int print_error);

// A print-host entry of /api/printers that speaks Moonraker, and where to reach it. Collected on
// the GUI thread (the preset and the connected host live there), probed off it.
struct HostTarget
{
    std::string id;   // "host" | "connect" | "ph:<device id>"
    std::string base; // http://<address>, ready for /printer/...
    // A PrusaLink / PrusaConnect device is asked over its own REST API instead of Moonraker's,
    // with the credentials its preset holds (a Buddy board answers 401 to everything without
    // them). Empty host_type = ask as a Moonraker printer, which is what every other target is.
    std::string host_type;
    std::string auth_type, apikey, user, password;
};
void list_host_targets(std::vector<HostTarget>& out);

// Request thread, never the GUI one: ask each printer what it is doing and fill the same
// control fields into its entry of the printers array, plus the temperatures a Bambu entry carries
// (bed_temp, bed_target, nozzles) when the printer reports them. A printer that does not answer
// keeps its buttons off and gets no temperatures. Safe to call with an empty list.
void describe_hosts(const std::vector<HostTarget>& targets, nlohmann::json& printers);

} // namespace RemoteControl
} // namespace GUI
} // namespace Slic3r
