#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

namespace SnapmakerLan { struct Device; }

// Ultra: the G-code archive.
//
// Every file this PC uploads to a printer - from the desktop's own Send / Print dialogs, from the
// Snapmaker preprint page, or from the phone through RemoteSend - is copied into a folder the user
// picks, next to a JSON sidecar saying which printer it went to and what the plate was. The phone's
// "Reprints" tab (stage 2) then has a list it can send again without the project being open.
//
// Rules for callers:
//   * archiving must never fail a send. Everything here swallows its own errors and logs them.
//   * archive() copies the file, so it may be called after the sender has finished with it, but
//     before anything deletes it (the print-host queue removes its temporary source afterwards).
//   * meta_for_plate() reads the plater and the presets and so has to run on the GUI thread; it
//     marshals there itself when called from a worker, so any hook may call it.
namespace GcodeArchive {

struct Filament
{
    int         index { 0 };
    std::string type;
    std::string colour;
    double      grams { 0.0 };
};

// What a send knows about itself. The printer identity and the mode are the caller's; everything
// else meta_for_plate() fills in from the plate.
struct Meta
{
    std::string printer_id;    // the id /api/printers uses: a Bambu dev_id, "host", "connect", "sm:<id>"
    std::string printer_kind;  // bambu | snapmaker | printhost | connect
    std::string printer_name;
    std::string printer_model;
    // A "connect" send's printer serial, when the Device tab knew it: what lets a record made while
    // the printer had no LAN card be listed under that card once it has one (normalize_printer).
    std::string printer_serial;
    std::string source { "desktop" }; // desktop | phone
    std::string mode { "upload" };    // upload | print
    std::string file_name;     // the name the printer was given ("plate_1.gcode"); "" = the source's

    int         plate { -1 };  // 0-based
    std::string plate_name;
    std::string project_title, project_path;
    std::vector<Filament> filaments;
    int         estimated_time_s { 0 };
    double      estimated_weight_g { 0.0 };
    std::string thumbnail_png;  // raw PNG bytes of the plate's small thumbnail, "" = none
    // Snapmaker over the LAN: which toolhead printed which of the file's filaments, in the wire
    // form /api/plates/{i}/send takes ("0:2,1:1"). A reprint replays it rather than deriving it
    // again, which is where "what we printed" and "what we replay" would quietly diverge.
    std::string mapping;
    // Snapmaker tool changers: this send asked the printer to unload the filaments it used when it
    // is done (the printer preset's unload_filaments_at_end). Kept for the same reason `mapping`
    // is - a reprint should behave the way the print it replays did.
    bool        unload_at_end { false };
    // Stage 1d: a Spoolman deduction was asked for after this send (the preference was on and a
    // server was configured). Whether the server accepted it is Spoolman's business, not ours.
    bool        spoolman_deduct { false };
    // Where the file sits on the printer when the send said so (the U1 pre-print page's
    // sw_StartLocalPrint "path"): what a reprint starts in place instead of uploading again.
    std::string remote_path;
};

// One archived send, as the sidecar holds it.
struct Record
{
    std::string    id;         // the sidecar's stem, unique in the folder
    long long      time { 0 }; // unix seconds
    std::string    file;       // the archived file's name (not its path)
    std::string    path;       // its full path on this PC - never leaves the instance API
    long long      size { 0 };
    std::string    sha256;
    bool           file_present { false }; // the archived file is still on disk (stage 2 can send it)
    bool           has_thumbnail { false };
    std::string    thumbnail_path;
    nlohmann::json json;       // the whole sidecar, as read
};

// Is the archive switched on (app_config ultra_gcode_archive)?
bool enabled();
// The folder, from app_config ultra_gcode_archive_dir, defaulting to <datadir>/gcode_archive.
std::string dir();
// ultra_gcode_archive_max, clamped to 1..10000.
int max_records();

// GUI thread or any thread (it marshals). Everything about the plate a sidecar wants.
Meta meta_for_plate(int plate, const std::string& mode);

// Any thread (it marshals). The name the Device tab shows for a Bambu dev_id, or the id itself.
std::string bambu_printer_name(const std::string& dev_id);

// Any thread. Copies `sent_file_path` into the archive and writes its sidecar; then trims the
// folder to max_records(). Returns the record, or a record with an empty id when nothing was
// archived (switched off, no such file, no room on disk...). Never throws.
Record archive(const std::string& sent_file_path, const Meta& meta);

// Any thread. The sidecars, newest first. `printer_id_filter` empty = all of them.
std::vector<Record> list(const std::string& printer_id_filter = "");

// Any thread. One record by id, or a record with an empty id.
Record find(const std::string& id);

// Any thread. Removes the file, its sidecar and its thumbnail. True when the sidecar was there.
bool remove(const std::string& id);

// A record's mode after the fact: an upload the person then started (on the PC's pre-print page,
// or from the phone's Reprint tab) becomes a print, and learns where the printer keeps the file
// when the start said. False when there is no such record or its sidecar could not be rewritten.
bool set_mode(const std::string& id, const std::string& mode, const std::string& remote_path = "");
// The same on an explicit folder, with no app config involved (tests).
bool set_mode_in(const std::string& root_dir, const std::string& id, const std::string& mode, const std::string& remote_path = "");
// A send of a record again (a reprint), kept in the record's own sidecar as "reprints": [...] -
// the job's history: when, to which printer, how (upload / print, uploaded again or started in
// place) and from where. The newest REPRINT_HISTORY_MAX entries are kept. False when there is no
// such record or its sidecar could not be rewritten; a reprint never fails because of it.
constexpr size_t REPRINT_HISTORY_MAX = 50;
bool note_reprint(const std::string& id, const nlohmann::json& entry);
// The same on an explicit folder (tests).
bool note_reprint_in(const std::string& root_dir, const std::string& id, const nlohmann::json& entry);

// ---- which printer a record names, as the phone's Reprint list shows it ----
//
// The U1 pre-print page archives through the Device tab's connection, and a U1 bound to a Snapmaker
// account is connected through the Snapmaker cloud's MQTT broker. Its records were therefore filed
// under printer "connect" and named "Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883" -
// the broker, not the printer - which the phone showed as a Reprint filter chip (2026-09-26).
// Everything below is pure: no app config, no files, no network.

// A printer "name" that is really an address: a URL, host:port, an IP literal, a cloud endpoint or
// a dotted host name (two dots or more, ending in a letters-only label). Any one such word counts.
bool name_is_address(const std::string& name);
// What a record's printer is called on the phone: its own name unless that is an address, then its
// model unless that is one, then a word for its kind ("Snapmaker U1" for the Snapmaker kinds).
// Never an address, never empty.
std::string display_printer_name(const std::string& name, const std::string& model, const std::string& kind);
// A record as the phone sees it, applied on read (the sidecar itself is not rewritten):
//  - a "connect" record whose printer.serial names a card in `lan` becomes that LAN card
//    (id sm:<serial>, kind snapmaker, the card's name and model), so it is listed, filtered and
//    reprinted with that printer - the same "the LAN wins" rule /api/printers applies;
//  - a name that is an address is replaced by display_printer_name().
// The printer as the sidecar has it is kept under "printer_recorded" when anything changed.
// Returns true when the record changed.
bool normalize_printer(nlohmann::json& record, const std::vector<SnapmakerLan::Device>& lan);

// ---- the model a record was sliced for (the phone's Reprint list is organised by it) ----
//
// A sliced file is specific to a printer model, not to one printer: a job sliced for a Snapmaker U1
// prints on any U1. So the phone groups the archive by model, and a reprint may go to any printer
// of the model it was sliced for. A record whose model is not known is grouped under "Other" and
// may only go back to the printer it was sent to.

// Bambu model codes ("O1D", "BL-P001", a sub-series "O1C2-V2") -> the model's name ("Bambu Lab H2D"),
// from resources/printers/*.json ("00.00.00.00".display_name) plus a built-in table for when the
// folder cannot be read. model_names() is that table for this app's resources, read once.
std::map<std::string, std::string> load_model_names(const std::string& printers_dir);
const std::map<std::string, std::string>& model_names();
// The model a printer or a record names, as one spelling: a Bambu code becomes its model name,
// anything else is kept (trimmed). "" when there is none, or when it is an address, not a model.
std::string canonical_model(const std::string& model, const std::map<std::string, std::string>& names);
// The key the phone groups and filters by: the canonical model in lower case, "other" for "".
std::string model_key(const std::string& canonical);
// The chip's words: "Bambu Lab H2D" -> "Bambu H2D", "Bambu Lab X1 Carbon" -> "Bambu X1C",
// "Elegoo Centauri Carbon" -> "Elegoo CC", "" -> "Other"; anything else as it is.
std::string model_label(const std::string& canonical);
// A record of one printer shares that printer's model and name: a record with no model, or whose
// name is only the printer's id, takes them from the newest record of the same printer that has
// them. `records` is newest first. Returns how many records it filled in.
int fill_from_siblings(std::vector<nlohmann::json>& records);
// Why a record may NOT be sent to `target_id`, or "" when it may:
//  - its own printer always may;
//  - another printer must be of the same kind (a U1 send made over the Snapmaker cloud counts as a
//    Snapmaker LAN one: the file is the same .gcode) and of the same model;
//  - a record with no known model may only go back to its own printer.
// Models are compared as canonical_model(..., names).
std::string reprint_target_refusal(const std::string& recorded_id, const std::string& recorded_kind,
                                   const std::string& record_model, const std::string& target_id,
                                   const std::string& target_kind, const std::string& target_model,
                                   const std::map<std::string, std::string>& names);
// A file is started in place (not uploaded again) only on the very printer it was sent to: another
// printer of the model may hold a different file under the same name.
bool reprint_in_place_allowed(const std::string& recorded_id, const std::string& target_id);

// The archive as the phone lists it: fill_from_siblings(), then every record gets "model_key" and
// "model_name" (the model its file was sliced for; "other" / "Other" when not known). `records` is
// newest first.
void annotate_models(std::vector<nlohmann::json>& records, const std::map<std::string, std::string>& names);
// The Reprint chips: one entry per model the (annotated) records were sliced for, in list order with
// "other" last - {key, name, count, printers [{id, name, kind, online}]} - where `printers` are the
// rows of `printer_rows` (the hub's /summary printers) of that model: every printer a record of it
// may be reprinted on. "Other" never lists any, and a "connect" row (the Device tab's link) is no
// printer a reprint can be addressed to.
nlohmann::json archive_models(const std::vector<nlohmann::json>& records, const nlohmann::json& printer_rows,
                              const std::map<std::string, std::string>& names);

} // namespace GcodeArchive
} // namespace GUI
} // namespace Slic3r
