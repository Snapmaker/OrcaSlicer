#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "DeviceControls.hpp"      // Caps: what the phone may set on the printer
#include "SnapmakerTaskConfig.hpp" // TOOLHEAD_COUNT, end_unload_parameter, with_end_unload

namespace Slic3r {
namespace GUI {

// Snapmaker printers over the LAN, through the Moonraker HTTP API they serve on port 80.
//
// Measured on the U1 (firmware 1.5.2): GET /access/info answers
// {"login_required": false, "trusted": true}, and /server/info, /printer/info,
// /machine/system_info, /printer/objects/query, /server/files/* all answer without any
// credential. Uploading is a multipart POST to /server/files/upload and a print is started with
// POST /printer/print/start?filename=. Nothing here needs the MQTT connection the PC's Device tab
// makes, so the phone can list, watch and feed a printer the PC has never connected to.
//
// The device list lives in <datadir>/hub/snapmaker_lan.json, next to the hub's own state, so every
// slicer instance on this PC sees the same printers.
namespace SnapmakerLan {

struct Device
{
    std::string id;       // the printer's serial number when known, else its address
    std::string name;     // its own device name (or the mDNS instance name)
    std::string model;    // "Snapmaker U1"
    std::string ip;
    int         port { 80 };
    std::string added_by; // discovery | manual | device_tab
};

// What one probe of a printer found. Empty/false when it did not answer.
struct Status
{
    bool        online { false };
    bool        login_required { false };
    std::string state;        // print_stats: standby | printing | paused | complete | cancelled | error
    std::string filename;     // the job the printer is on
    std::string message;      // the printer's own error / display message
    double      progress { 0 };   // 0..1
    double      bed_temp { 0 }, bed_target { 0 }, nozzle_temp { 0 }, nozzle_target { 0 };
    // (temperature, target) of every toolhead heater that answered, toolhead order; nozzle_temp /
    // nozzle_target above are the first one's, kept for the readers that show a single nozzle.
    std::vector<std::pair<double, double>> nozzles;
    int         layer { 0 }, total_layers { 0 };
    double      print_duration { 0 }, total_duration { 0 };
    std::string klippy;       // klippy_state: ready | startup | shutdown | error
    // How old this reading is: 0 for a probe that just answered, more while the printer is still
    // counted online through a probe or two that did not (see Presence), -1 when it never answered.
    long long   age_ms { -1 };
    // What the phone's printer screen may set, from the same query: the bed and each toolhead with
    // their limits, the speed factor, the cavity light and the fans (DeviceControls::moonraker_caps).
    DeviceControls::Caps caps;
    // The printer's G-code help lists every command the load / unload scripts use
    // (FilamentCommands::u1_filament_macros_available). Asked with the login check, not every probe.
    bool        filament_macros { false };
    bool        printing() const { return state == "printing" || state == "paused"; }
};

// ---- addresses ----
//
// A U1 bound to a Snapmaker account is connected by the Device tab through the cloud's MQTT broker,
// and the Device tab then records that broker (a1pr8yczi3n0se.iot.us-west-1.amazonaws.com) as the
// printer's "ip". That is not an address the printer answers HTTP on - it is not the printer at
// all - so it must never become a LAN card's address or be probed.
//
// The bare host of an address: no scheme, no path, no port, no IPv6 brackets.
std::string host_of(const std::string& address);
// A cloud broker / cloud service endpoint (AWS IoT, Aliyun, Snapmaker's own domains).
bool        is_cloud_host(const std::string& host);
// An address worth probing over the LAN: anything that is not empty, not a cloud endpoint and not
// obviously malformed. A typed-in "u1.home.example" is accepted; only the cloud is refused.
bool        is_lan_host(const std::string& host);
// Stricter, for sources that are known to mix in the cloud broker (the Device tab's record): an IP
// literal, a .local name or a single-label name only.
bool        is_local_address(const std::string& host);

// The list rules, without the file: add or update `d` in `list` (by serial number first, then by
// address, so a printer that moved to another address moves rather than doubling up). A cloud
// endpoint never enters the list and never replaces a LAN address. Returns false when `d` was
// refused. Any other entry carrying the same id afterwards is dropped.
bool                merge_device(std::vector<Device>& list, const Device& d);
// What a stored list looks like once read: cloud endpoints dropped and one entry per id (the first
// one with a usable address wins), so no two cards - and no two status slots - share an id.
std::vector<Device> sanitize(const std::vector<Device>& raw);
// The LAN card a connection address belongs to: the PC's Device tab connects a U1 over MQTT at
// "<ip>:<port>", and this finds the printer in `list` that answers HTTP on the same host. False for
// a cloud broker (a cloud-bound U1 has no LAN identity from its connection alone) or no match.
bool                match_host(const std::vector<Device>& list, const std::string& address, Device& out);
// The same against the stored list (devices()).
bool                device_for_host(const std::string& address, Device& out);

// ---- online / offline ----
//
// A printer is online as soon as one probe answers, and offline only after FAILS_TO_OFFLINE probes
// in a row did not, or after SILENCE_MS without an answer - one slow reply from a busy printer (or
// one dropped Wi-Fi packet) no longer turns its card grey for half a minute.
struct Presence
{
    static constexpr int       FAILS_TO_OFFLINE = 3;
    static constexpr long long SILENCE_MS       = 60000;
    // How often a printer is asked: every PROBE_TTL_MS while it is online (or still being given the
    // benefit of the doubt), every OFFLINE_RETRY_MS once it is offline, so a printer that is off
    // does not cost a timeout on every poll but one that comes back is seen within seconds.
    static constexpr long long PROBE_TTL_MS     = 4000;
    static constexpr long long OFFLINE_RETRY_MS = 10000;

    int       fails { 0 };      // probes in a row that did not answer
    long long last_ok_ms { -1 }; // when the printer last answered (-1 = never)
    bool      online { false };

    // Feed one probe result. Returns true when `online` changed.
    bool      observe(bool answered, long long now_ms);
    // How long a reading taken now stays good before the printer is asked again.
    long long refresh_after_ms() const { return online ? PROBE_TTL_MS : OFFLINE_RETRY_MS; }
};

// One of the printer's toolheads and what is loaded in it (print_task_config, per-toolhead arrays).
struct Toolhead
{
    int         index { 0 };
    std::string type;      // PLA, PETG, ...
    std::string sub_type;  // Matte, Silk, ...
    std::string vendor;
    std::string color;     // #RRGGBB, from filament_color_rgba (RRGGBBAA)
    bool        loaded { false };  // filament_exist
    bool        official { false };
    double      nozzle { 0 };      // extruder<N>.nozzle_diameter
};

// ---- the shared list ----
std::vector<Device> devices();                                   // any thread
bool                find(const std::string& id, Device& out);
// Ask an address who it is, without remembering it (blocking, a few seconds).
bool                identify_at(const std::string& ip, int port, Device& out, std::string& error);
// Probe an address and remember it (blocking, a few seconds). error is for the phone.
bool                add(const std::string& ip, int port, Device& out, std::string& error);
bool                remove(const std::string& id);               // only what a person added
// The printers this PC already knows from its Device tab (AppConfig): GUI thread.
void                merge_app_devices();
// One mDNS pass in the background, merging what answers into the list (the fork's Bonjour, the
// "snapmaker" service the Device page's own search uses). Cheap to call: at most one pass a minute.
void                start_discovery();
// The Stream tab's camera list (<datadir>/hub/streams.json) is a fourth source: a camera whose
// address answers as a Snapmaker is that printer, with the camera's alias as its name. Bambu
// cameras are left alone - those printers arrive through the device manager.
void                merge_stream_devices();

// ---- live state ----
// (temperature, target) of extruder, extruder1, ... in a /printer/objects/query status object, in
// toolhead order, up to TOOLHEAD_COUNT; stops at the first toolhead the answer lacks so the index
// stays the toolhead number.
std::vector<std::pair<double, double>> nozzle_temps_of(const nlohmann::json& status_obj);
// Probes the printer, at most once every few seconds per device. Online/offline follows Presence:
// a probe that does not answer leaves a printer that was online online (with its last reading)
// until it has missed FAILS_TO_OFFLINE probes or been silent for SILENCE_MS.
Status status(const Device& d);
// The same, ignoring that cache: for watching a printer right after telling it to do something.
Status status_now(const Device& d);
// The last probe's answer without asking the printer: for the GUI thread, which must not wait on
// the network. False when this printer has never been probed.
bool cached_status(const Device& d, Status& out);
// What each toolhead holds, from the same cached probe.
std::vector<Toolhead> toolheads(const Device& d);
// Probe several printers side by side and wait at most `budget_ms` for all of them together; a
// printer still being asked when the budget runs out is reported from its last reading (or as
// offline if it never answered), and its probe lands in the cache for the next call. One slow or
// switched-off printer therefore never holds up the others' cards. Any thread but the GUI one.
std::vector<Status> status_all(const std::vector<Device>& list, long long budget_ms);
// The whole list with each printer's state, probed in parallel. Any thread but the GUI one.
void   list_json(nlohmann::json& out);
// What /api/printers needs for the send picker (one entry per device).
void   list_printers(nlohmann::json& printers);
// The LAN wins: a "connect" row (the Device tab's MQTT link, possibly through the Snapmaker cloud)
// whose `lan_id` names a LAN row that is online is dropped from `printers`, so the printer has one
// card and a send goes over the LAN. While the LAN row is offline the connect row stays - it is
// then the only way to the printer - and the LAN row is marked `cloud_online` with its state.
void   prefer_lan(nlohmann::json& printers);

// ---- sending ----
// Multipart upload to /server/files/upload with print=false, so a failed print start still leaves
// a usable file on the printer. `progress` is called with 0..100.
bool upload(const Device& d, const std::string& source_path, const std::string& filename,
            std::function<void(int)> progress, std::string& error);
// GET /server/files/metadata?filename= - proof that the file really landed.
bool metadata(const Device& d, const std::string& filename, long long& size, std::string& error);
// The file is still on the printer, byte for byte as far as a size can tell: what lets a reprint
// start it in place instead of uploading it again. `expected_size` <= 0 accepts any size.
bool file_on_printer(const Device& d, const std::string& filename, long long expected_size);
// POST /printer/print/start?filename= - the whole file as it was sliced, no mapping.
bool start_print(const Device& d, const std::string& filename, std::string& error);

// One filament of the sliced file, as the plate holds it.
struct FileFilament
{
    int         index { 0 };  // the file's filament / extruder slot, 0-based
    std::string color;        // #RRGGBB
    std::string type;         // PLA, PETG, ...
    double      used_g { 0 };
    bool        used { true };
};

// Which toolhead prints which of the file's filaments, chosen the way the printer's own app and
// u1hub choose it: the nearest loaded colour (redmean distance), one toolhead per colour, a
// filament of the same colour shares that toolhead, and anything left over falls back to the first
// loaded toolhead. Type never decides - it only earns a warning on the phone.
// Returns one toolhead per filament (-1 for a filament the file does not use).
std::vector<int> auto_match(const std::vector<FileFilament>& filaments, const std::vector<Toolhead>& heads);

// POST /printer/gcode/script?script= - one or more macros, run at once. The transport every
// task-config command below travels on.
bool run_script(const Device& d, const std::string& script, std::string& error);

// The macros for one mapping, without the start (what a dry run reports).
// `unload_at_end` adds END_UNLOAD_FILAMENT=[..] to SET_PRINT_PREFERENCES: a 1 for every toolhead
// this print uses, so the firmware's PRINT_END unloads them once the job is done.
std::string mapping_script(const std::vector<int>& mapping, bool unload_at_end = false);

// Start `filename` with that mapping: the SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS /
// SET_PRINT_PREFERENCES macros, then SDCARD_PRINT_FILE, each a POST /printer/gcode/script.
// `sent` receives the scripts. mapping[i] = the toolhead for the file's filament i.
bool start_print_mapped(const Device& d, const std::string& filename, const std::vector<int>& mapping,
                        bool unload_at_end, nlohmann::json& sent, std::string& error);

std::string base_url(const Device& d);

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
