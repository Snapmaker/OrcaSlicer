#ifndef slic3r_PrintErrorCommands_hpp_
#define slic3r_PrintErrorCommands_hpp_

// The MQTT payloads behind the buttons on the printer-error dialog, built as pure functions.
//
// Why these are not just written inline in MachineObject::command_*: the shapes are the whole
// point. Firmware that gates an error-triggered resume on seeing "err" and "param":"reserve"
// silently ignores the generic {"command":"resume","param":""} the fork used to send from the
// error dialog, so the buttons looked like they worked and did nothing. The only way to keep the
// fork honest about that is to pin the exact JSON in a test, and the only way to test it without
// a live MachineObject and a broker is to build it somewhere that has neither.
//
// So MachineObject::command_hms_* is a two-liner - call the builder here, hand the dump() to
// publish_json - and the test calls the same builder with a fake err/job_id and compares against
// Bambu Studio's payloads verbatim.
//
// Sequence ids are passed in rather than taken from MachineObject::m_sequence_id, for the same
// reason: a builder that reaches for a global counter is not a pure function and its output is
// not comparable.

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

// Every action id the dialog can render, mirroring Bambu Studio's DeviceErrorDialog::ActionButton.
// The shipped resources/hms/hms_action_<devtype>.json tables name these ids directly in their
// "actions" arrays; an id the dialog has no button for is dropped silently, which is exactly the
// bug that left a stuck printer with no Resume on screen.
namespace PrintErrorAction {
enum Id : int {
    RESUME_PRINTING                = 2,
    RESUME_PRINTING_DEFECTS        = 3,
    RESUME_PRINTING_PROBELM_SOLVED = 4,
    STOP_PRINTING                  = 5,
    CHECK_ASSISTANT                = 6,
    FILAMENT_EXTRUDED              = 7,
    RETRY_FILAMENT_EXTRUDED        = 8,
    CONTINUE                       = 9,
    LOAD_VIRTUAL_TRAY              = 10,
    OK_BUTTON                      = 11,
    FILAMENT_LOAD_RESUME           = 12,
    JUMP_TO_LIVEVIEW               = 13,

    NO_REMINDER_NEXT_TIME          = 23,
    REFRESH_NOZZLE                 = 24,
    IGNORE_NO_REMINDER_NEXT_TIME   = 25,
    IGNORE_RESUME                  = 27,
    PROBLEM_SOLVED_RESUME          = 28,
    TURN_OFF_FIRE_ALARM            = 29,

    RETRY_PROBLEM_SOLVED           = 34,
    STOP_DRYING                    = 35,
    CANCEL_ACTION                  = 37,
    REMOVE_CLOSE_BTN               = 39, // not a button: suppresses the window's close box
    PROCEED                        = 41,
    OK_JUMP_RACK                   = 49,
    ABORT                          = 51,
    DISABLE_PURIFICATION           = 54,
    DONT_REMIND_NEXT_TIME          = 57,
};
} // namespace PrintErrorAction

// True when this id needs a job_id to be meaningful. The resume/stop/ignore family carries
// "job_id" so firmware can check the command is for the job it currently holds; sending one with
// an empty job_id is the documented way to have it rejected, so the dialog disables those buttons
// rather than firing a command that cannot work.
bool print_error_action_needs_job_id(int action_id);

// True when the id is one of the buttons the dialog knows how to render at all.
bool print_error_action_is_known(int action_id);

// The button set for one error code, given that code's "actions" list as the shipped table spells
// it. Unknown ids are dropped (the tables carry ids like 14-22 and 43-48 that not even Bambu
// Studio has buttons for); REMOVE_CLOSE_BTN is kept, since the dialog reads it as a flag.
//
// When nothing survives - an unknown code, or a code whose actions are all ids we cannot render -
// the generic set {STOP_PRINTING, RESUME_PRINTING, OK_BUTTON} is returned instead, so a stuck
// printer always has something to press. `used_fallback` reports which of the two happened.
std::vector<int> resolve_print_error_actions(const std::vector<int>& table_actions, bool& used_fallback);

// The generic set, for callers that want it without a lookup.
std::vector<int> generic_print_error_actions();

// "2, 5, 11" - for the log line that records what a table asked for and what was drawn.
std::string format_action_ids(const std::vector<int>& ids);

// ---- the remote surface: one action id, one verb, one label ----
//
// The hub page and the app cannot press a wx button, so the desktop dialog's button set has to
// cross the wire as data. This is that translation, and it lives here rather than in RemoteControl
// so the desktop dialog and the remote surfaces cannot drift apart about what an id means - and so
// it can be tested with no printer, no socket and no window.
//
// `verb` is what POST /api/printers/{id}/control takes as its action. Several ids share a verb
// (Resume Printing, Resume (defects acceptable) and Problem Solved and Resume are all one
// command_hms_resume) - the id is kept alongside so the label stays the one the table asked for.
//
// `remote_safe` is the gate. An action whose effect a person can only judge with the printer in
// front of them is described but never offered: the AMS controls (done / retry / abort), the
// drying stop, the nozzle recheck, the fire alarm's buzzer and the purification switch all move or
// silence hardware, and a tap from another room cannot see whether the filament is actually out of
// the way. Those ids come across with remote_safe=false so the app can grey them out and say why,
// and the control route refuses their verbs outright.
struct PrintErrorRemoteAction
{
    int         id { 0 };
    std::string verb;          // the control route's action word, "" when the id has no remote verb
    std::string label;         // what the button says
    bool        needs_job_id { false };
    // True when the command behind the verb is built out of the JSON blob the printer hands out
    // with a command error (ack_proceed / dont_remind). Like needs_job_id this is a property of
    // the verb and a reason the action may be described but not offered: with no blob there is
    // nothing to build the payload from, so pressing it could only fail.
    bool        needs_action_json { false };
    bool        remote_safe { false };
};

// The verbs the control route accepts on top of pause / resume / stop:
//
//   resume_error      command_hms_resume        err + job_id + param reserve - the error-aware resume
//   stop_error        command_hms_stop          the error-aware stop
//   ignore_error      command_hms_ignore        the print continues, the fault is NOT fixed
//   idle_ignore_error command_hms_idle_ignore   silences the popup only, the print is untouched
//   ack_proceed       command_ack_proceed       proceed, adding the code to the always-ignore list
//   dont_remind       command_dont_remind_next_time  the next-time-ignore list
//   ack_close         command_clean_print_error_uiop the dismiss-without-acting ack
//
// ack_proceed and dont_remind are remote-safe only while the printer's blob is in hand. Neither
// command can be built without it - it names the command to re-send and the index to suppress -
// so an error that arrived without one (every error that is not a refused-command reply) carries
// those two described and greyed, exactly the way a missing job_id greys a resume.
//
// A verb not in this list is not an error action; the route answers 400 for it.
bool is_print_error_verb(const std::string& verb);

// True when the verb is one the hub and app may send at all. Kept separate from
// is_print_error_verb so the route can tell "not a verb at all" (400) from "a verb, deliberately
// desktop-only" (403).
//
// This is the verb's own ceiling and says nothing about the printer's current state: a verb that
// needs a job_id or an action_json is still remote-safe here, and it is describe_print_error_actions
// and check_print_error_action that refuse it when what it needs is missing.
bool is_remote_safe_verb(const std::string& verb);

// Whether the verb's command needs the printer's job_id. The route refuses without one rather
// than sending a command firmware will drop.
bool print_error_verb_needs_job_id(const std::string& verb);

// Whether the verb's command is built from the error's action_json blob (ack_proceed,
// dont_remind). The route refuses without one rather than sending a payload it cannot build.
bool print_error_verb_needs_action_json(const std::string& verb);

// One action id described for the wire. An id with no remote meaning at all (CHECK_ASSISTANT,
// JUMP_TO_LIVEVIEW, LOAD_VIRTUAL_TRAY, REMOVE_CLOSE_BTN, CANCEL_ACTION) gets an empty verb and
// remote_safe=false; the caller drops those rather than showing a button that does nothing.
PrintErrorRemoteAction describe_print_error_action(int action_id);

// The whole button set of one error, as the status JSON and the event payload carry it: the ids
// resolve_print_error_actions kept, minus the ones with no verb at all, each with its verb, label,
// needs_job_id, needs_action_json and remote_safe.
//
// The two booleans say what the printer is holding right now, and each downgrades the actions that
// depend on it. `has_job_id` - an action that needs a job_id while the printer reports none comes
// across remote_safe=false, because firmware would drop the command. `has_action_json` - the same
// rule for the blob behind ack_proceed / dont_remind, which only a refused-command reply supplies;
// it defaults to false so every caller that has no blob keeps the old, safe answer.
std::vector<PrintErrorRemoteAction> describe_print_error_actions(const std::vector<int>& resolved_actions,
                                                                 bool has_job_id,
                                                                 bool has_action_json = false);

// ---- the control route's guards, as one pure decision ----
//
// Everything that can make a remote error action the wrong thing to send, checked in one place so
// the route, the page and the test all agree about what the answer is and what it says. Split out
// of RemoteControl::prepare because the interesting half needs no printer at all: it is four
// strings and two booleans, and those are exactly the cases that are hard to reach with hardware
// (a code that changed under the person's finger, a job that ended between the page load and the
// tap).
//
// Returns 0 when the action may go out, or the HTTP status the route should answer with:
//   400 - not a verb, or an empty err, or a stop without confirm
//   403 - a real verb this phase keeps at the printer (the AMS controls and their neighbours)
//   409 - the error cleared, the code moved on, no job_id, no action_json for a verb that needs
//         one, or this error does not offer this verb
// `why` is the sentence the person reads; it names both codes when they differ, because "try
// again" is useless advice without knowing what changed.
struct PrintErrorActionRequest
{
    std::string verb;
    std::string asked_err;    // what the client sent, any case, with or without the grouping space
    bool        confirm { false };
    std::string current_err;  // the eight-hex code the printer reports right now, "" when none
    std::string job_id;       // the printer's job right now, "" when none
    std::string printer_name; // for the sentence
    std::vector<int> offered;  // resolve_print_error_actions' output for current_err
    // Whether the printer's latest command error handed over an action_json blob, which is what
    // ack_proceed and dont_remind are built from. False for every error that is not a refused
    // command, which is the ordinary case.
    bool        has_action_json { false };
};
int check_print_error_action(const PrintErrorActionRequest& req, std::string& why);

// "0C00402D" from "0c00 402d" - upper-cased, spaces removed. What the check compares, exposed
// because the caller logs the normalised form.
std::string normalize_error_code(const std::string& code);

// ---- the refused-command reply ----
//
// A printer that will not do what the slicer asked answers on the "print" topic with the command's
// own sequence id and an "err_code". That reply is what opens the command-error dialog, and when
// it also carries an "err_index" the whole object is the action_json blob Proceed and Don't remind
// are built from.
//
// Pulled out of MachineObject::parse_json as a pure function for the usual reason: the interesting
// part is which replies count and what comes out of them, and that is decided by the JSON alone -
// no printer, no broker, no window. `is_studio_seq` is the caller's own is_studio_cmd verdict on
// the reply's sequence id, because the range that makes a sequence id ours is the MachineObject's.
//
// Returns false when this reply is not a refused command. Otherwise `err_code` is the code it
// names, and `action_json` is the whole reply when it carried an err_index and null when it did
// not - null being the case where the dialog shows with Proceed and Don't remind greyed, since
// there is nothing to build them from.
//
// This is the shape check only. MachineObject decides through accept_command_refusal below, which
// also requires the reply to answer a command this slicer actually sent.
bool parse_command_error_reply(const nlohmann::json& print_block, bool is_studio_seq,
                               int& err_code, nlohmann::json& action_json);

// ---- which replies are answers to something this slicer actually sent ----
//
// The sequence-id range alone cannot say that. START_SEQ_ID..END_SEQ_ID is the range Bambu Studio
// and OrcaSlicer use too, so a reply to *their* command (or to one from a previous session of
// ours) lands in it; and the printer's push_status answer to our own "pushall" echoes the
// pushall's sequence id while carrying status-level error fields. Both used to open "the printer
// refused a command" the moment the Device page connected, over a leftover error from an earlier
// task that nobody here had asked for.
//
// So the slicer records what it publishes - sequence id -> command name, with a short expiry - and
// a refusal is only reported when the reply names a command we sent under that id and have not
// yet seen refused. Status and info requests are never recorded: their answers are status, not a
// verdict on a command.

// push_status, pushall, get_version, get_access_code and the other "get" requests. Their replies
// may carry error fields describing the printer, never a refusal of the request itself.
bool is_status_or_info_command(const std::string& command);

// The commands that start, pause, resume, stop or ignore-and-continue a print. Only a refusal of
// one of these is allowed to offer Stop Printing / Resume Printing: refusing anything else says
// nothing about a print that could be stopped or resumed.
bool is_print_action_command(const std::string& command);

class SentCommandTracker
{
public:
    using Clock = std::chrono::steady_clock;
    // Long enough for any printer to answer, short enough that an id is not held for the session.
    static constexpr std::chrono::seconds DEFAULT_TTL { 60 };
    // The network plug-in uploads the file before it publishes project_file, and the upload can
    // take minutes, so a send it makes on our behalf is held for longer.
    static constexpr std::chrono::seconds AGENT_TTL { 15 * 60 };

    SentCommandTracker() = default;
    SentCommandTracker(const SentCommandTracker& other);
    SentCommandTracker& operator=(const SentCommandTracker& other);

    // A command this slicer published under `sequence_id`. Status and info requests, and an empty
    // id or command, are not recorded.
    void note_sent(const std::string& sequence_id, const std::string& command, Clock::time_point now,
                   std::chrono::seconds ttl = DEFAULT_TTL);

    // The same, read out of a payload exactly as it was published ({"print":{...}}, {"system":{...}},
    // ...). Every top-level block carrying both "command" and "sequence_id" is recorded.
    void note_sent_payload(const nlohmann::json& payload, Clock::time_point now);

    // A command the network plug-in publishes for us (project_file, gcode_file) under a sequence id
    // it never hands back. Matched by command name alone, once, within `ttl`.
    void note_sent_by_agent(const std::string& command, Clock::time_point now, std::chrono::seconds ttl = AGENT_TTL);

    // True when a reply naming `command` under `sequence_id` answers something we sent and is still
    // waiting on. Does not consume the entry: most commands are answered with a success first.
    bool is_awaiting(const std::string& sequence_id, const std::string& command, Clock::time_point now);

    // The reply refused it: drop the entry, so one refusal opens one window.
    void forget(const std::string& sequence_id, const std::string& command);

    size_t pending_count(Clock::time_point now);

private:
    struct Entry
    {
        std::string       command;
        Clock::time_point expires;
    };
    void expire_locked(Clock::time_point now);

    mutable std::mutex                       m_mutex;
    std::map<std::string, Entry>             m_by_seq;   // sequence id -> what we sent under it
    std::map<std::string, Clock::time_point> m_by_agent; // command -> until when a plug-in send is awaited
};

// The whole decision for one reply on the "print" topic: parse_command_error_reply's checks, plus
// "the command is one we sent under this sequence id and are still waiting on", minus status and
// info replies of any kind. On true, `command` is the refused command's name and the tracker has
// forgotten it; the caller shows the dialog. On false nothing is shown - `command` is still filled
// in when the reply carried an err_code, so the caller can log what it ignored.
bool accept_command_refusal(const nlohmann::json& print_block, SentCommandTracker& tracker,
                            SentCommandTracker::Clock::time_point now,
                            int& err_code, nlohmann::json& action_json, std::string& command);

// The button set for a refused command. A refused print action gets what the status-push dialog
// gets (resolve_print_error_actions, generic Stop/Resume/OK fallback included). Any other refusal
// is only information about a command that did not happen: Stop Printing and every Resume
// variant are removed from the table's set, and when nothing else is left the answer is OK alone.
std::vector<int> resolve_command_error_actions(const std::vector<int>& table_actions, bool print_action,
                                               bool& used_fallback);

// ---- payload builders ----
//
// `err` is the error code as the printer names it in these commands. Bambu Studio passes
// std::to_string(error_code) - the decimal spelling of print_error, not the 8-hex form the
// hms_action tables are keyed by - and that is reproduced here rather than corrected, because
// firmware matches on what upstream sends.

// {"print":{"command":"resume","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_resume(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"stop","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_stop(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"ignore","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_ignore(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"idle_ignore","err":<err>,"type":<type>,"sequence_id":<seq>}}
nlohmann::json build_hms_idle_ignore(const std::string& err, int type, const std::string& sequence_id);

// {"print":{"command":"refresh_nozzle","sequence_id":<seq>}}
nlohmann::json build_refresh_nozzle(const std::string& sequence_id);

// {"print":{"command":"buzzer_ctrl","mode":0,"sequence_id":<seq>}}
nlohmann::json build_stop_buzzer(const std::string& sequence_id);

// {"print":{"command":"close_air_filt","sequence_id":<seq>}}
nlohmann::json build_purification_disable(const std::string& sequence_id);

// {"print":{"command":"auto_stop_ams_dry","sequence_id":<seq>}}
nlohmann::json build_ams_drying_stop(const std::string& sequence_id);

// {"system":{"command":"uiop","name":"print_error","action":"close","source":1,"type":"dialog",
//            "err":"<8hex>","sequence_id":<seq>}}
// The dismiss-without-acting ack, sent when the window's close box is used.
nlohmann::json build_clean_print_error_uiop(int print_error, const std::string& sequence_id);

// The "proceed" / "don't remind" family. Both take a JSON blob the caller was handed with the
// error (it names the command to re-send and the index of the error to suppress) and grow an
// "err_ignored" list plus a parallel "rm_idx" list of {idx, mode} entries.
//
//   mode 0 - always ignore   (PROCEED)
//   mode 1 - next time ignore (DONT_REMIND_NEXT_TIME)
//
// Returns false with `error` set when the blob is missing the fields either needs, which is the
// case that used to throw out of the dialog.
bool build_ack_proceed(const nlohmann::json& action_json, const std::string& sequence_id,
                       nlohmann::json& out, std::string& error);
bool build_dont_remind_next_time(const nlohmann::json& action_json, const std::string& sequence_id,
                                 nlohmann::json& out, std::string& error);

}} // namespace Slic3r::GUI

#endif // slic3r_PrintErrorCommands_hpp_
