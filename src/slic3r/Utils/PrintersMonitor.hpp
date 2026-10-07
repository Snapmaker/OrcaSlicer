#ifndef slic3r_PrintersMonitor_hpp_
#define slic3r_PrintersMonitor_hpp_

// Rules for the "Printers" tab (GUI/PrintersPanel.cpp, page resources/web/orca/monitor.html), kept
// free of wx so they can be tested on their own (tests/slic3rutils/printers_monitor_tests.cpp):
//   * which script-channel messages the page may send, and what each one names,
//   * how an answer is handed back to the page (a JSON argument to one of its window.__ hooks),
//   * what the page is given: the rows RemoteAccess::api_printers builds - the very rows the hub
//     serves as /summary and the app's Devices page renders - plus when they were read,
//   * how "Open Device page" reaches a printer, per vendor,
//   * the printer groups (phase 1: stored in AppConfig behind GroupStore; the target is the hub).
//
// The page never talks to the instance API (loopback, no auth) or the hub: everything goes through
// the "wx" script channel of its own web view, and the panel checks the page is ours.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace PrintersMonitor {

// ---- messages from the page --------------------------------------------------------------------
//
//   printers_get                              the printer rows, answered with __printers(payload)
//   printers_groups_get                       the groups, answered with __printerGroups(groups)
//   printers_control:<action>:<0|1>:<id>      pause | resume | stop (stop needs the 1: the page
//                                             asked the person), answered with __printerControl
//   printers_job:<n>                          how control job n is doing, answered with __printerJob
//   printers_open:<id>                        open that printer's Device page (or its web UI)
//   printers_tasks                            the old multi-device task list (cloud send queue)
//   printers_thumb:<archive id>               a G-code archive record's plate picture, answered
//                                             with __printerThumb({ id, data: "data:image/png;..." })
//
// The id goes last because printer ids carry colons ("sm:<serial>", "ph:<id>").
struct Message
{
    enum class Kind { Invalid, Get, Groups, Control, Job, Open, Tasks, Thumb };
    Kind        kind { Kind::Invalid };
    std::string id;              // Control, Open; Thumb: the archive record id
    std::string action;          // Control: pause | resume | stop
    bool        confirm { false };
    int         job { 0 };       // Job
};
Message parse_message(const std::string& msg);

// A printer id as the rows spell them: 1..200 printable ASCII characters, none of them space,
// quote, backslash or angle bracket. Anything else is refused before it reaches RemoteControl.
bool valid_printer_id(const std::string& id);

// The only actions the tab offers (owner decision 2026-10-06: Pause / Resume / Stop only).
bool valid_action(const std::string& action);

// "if (window.<fn>) window.<fn>(<arg>);" - fn must be a plain identifier (else ""), and the JSON
// is written with every non-ASCII character escaped, so nothing in a printer's name can end the
// script early.
std::string script_call(const std::string& fn, const nlohmann::json& arg);

// What the page is given for one poll: { ok, printers: [...rows...], at: <ms since epoch> } when
// api_printers answered 200 with a parsable body, otherwise { ok: false, error, printers: [] }.
nlohmann::json page_payload(int status, const std::string& body, long long now_ms);

// ---- Open Device page --------------------------------------------------------------------------
struct Target
{
    std::string kind; // the row's kind: bambu | snapmaker | connect | printhost
    std::string url;  // its web address, when the row has one
};
// The ids of the last rows the page was given, with what "open" needs. The page names an id only;
// where it goes is taken from here, never from the page.
std::map<std::string, Target> targets_of(const nlohmann::json& printers);

// http:// or https:// with a host, and nothing that could break out of an address.
bool is_web_url(const std::string& url);

enum class OpenWay {
    BambuMonitor,   // MainFrame::jump_to_monitor(id): the Device tab is the Bambu one
    PrinterWebView, // the printer's web UI in the Device tab's web view (a non-Bambu preset)
    Browser,        // the printer's web UI in the system browser (the Device tab is something else)
    NotHere         // nothing to open from this window (a Bambu printer while the preset is not
                    // a Bambu one, a row without a web address); the page says why
};
// bambu_monitor_shown: the Device tab currently holds the Bambu MonitorPanel.
// printer_view_shown:  the Device tab currently holds the PrinterWebView.
OpenWay open_way(const Target& t, bool bambu_monitor_shown, bool printer_view_shown);
// What the page shows when open_way is NotHere.
std::string not_here_reason(const Target& t);

// A G-code archive record id (the sidecar's stem): 1..128 of [A-Za-z0-9._-], not starting with a dot.
bool valid_archive_id(const std::string& id);

// ---- what the card shows besides the row ---------------------------------------------------------
//
// Printer pictures: the vendor profiles' "<model>_cover.png" (resources/profiles/<Vendor>/), which
// the page server serves from the installed resources as /profiles/<Vendor>/<file>. The index maps
// the lower-cased model name to that URL path, each segment percent-encoded.
using CoverIndex = std::map<std::string, std::string>;
// vendor_files: (vendor folder, file name) pairs; only "*_cover.png" names are taken.
CoverIndex index_covers(const std::vector<std::pair<std::string, std::string>>& vendor_files);
// The picture for a model name ("Bambu Lab H2C", "Snapmaker U1", a preset's printer_model), or "".
std::string cover_for(const CoverIndex& index, const std::string& model_name);
// RFC 3986 unreserved characters stay, everything else becomes %XX.
std::string url_encode_segment(const std::string& s);
// What the model line says: a Bambu display name without its "Bambu Lab " prefix ("H2C"); any
// other name as it is; the raw model code when no name is known.
std::string short_model_name(const std::string& kind, const std::string& model_name, const std::string& model_code);

// The current job's plate picture from the G-code archive: per printer, the newest record whose
// sent name or file name is the job the printer reports (the hub's own rule, RemoteHub
// printer_thumbnail_paths, minus its "newest record of any job" fallback: a card shows the
// picture of the job it is running, or none).
struct ArchiveEntry
{
    std::string printer_id, record_id, sent_name, file;
    long long   time { 0 };
};
std::map<std::string, std::string> pick_thumbnails(const std::vector<ArchiveEntry>& entries,
                                                   const std::map<std::string, std::string>& jobs);
// The job name a row reports (task, else subtask_name), the field the hub matches on too.
std::string row_job(const nlohmann::json& row);

// Adds what the card needs to each row of a page payload: "model_short", "picture" (a cover URL
// path) when there is one, and "thumb_id" (an archive record id) when the archive has the running
// job's picture.
void enrich_rows(nlohmann::json& printers, const CoverIndex& covers, const std::map<std::string, std::string>& thumbs);

// ---- groups ------------------------------------------------------------------------------------
struct Group
{
    std::string              id;
    std::string              name;
    std::vector<std::string> printers; // row ids
};

// Where groups live. Phase 1: AppConfig (GUI/PrintersPanel.cpp). The target is a hub-side store
// shared with the app and the phone page (research_multi_device_monitor.md section 3.6); it
// implements this same interface.
class GroupStore
{
public:
    virtual ~GroupStore() = default;
    virtual std::vector<Group> load()                          = 0;
    virtual bool               save(const std::vector<Group>&) = 0;
};

// Parses what save_groups wrote. Bad entries are skipped; duplicate ids keep the first; ids and
// printer ids are checked like valid_printer_id; names are trimmed to 64 characters.
std::vector<Group> parse_groups(const std::string& stored);
std::string        save_groups(const std::vector<Group>& groups);
// The old Multi-device tab's pick list (AppConfig section multi_devices, keys 0..5) as the first
// group, when nothing is stored yet. Empty ids are dropped; no ids, no group.
std::vector<Group> seed_from_multi_devices(const std::vector<std::string>& dev_ids);
// What the page is given: "All printers" first (id "all", every row), then the stored groups.
nlohmann::json groups_json(const std::vector<Group>& groups);

} // namespace PrintersMonitor
} // namespace Slic3r

#endif // slic3r_PrintersMonitor_hpp_
