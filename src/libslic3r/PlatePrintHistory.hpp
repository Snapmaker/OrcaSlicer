#pragma once

// Per-plate print history.
//
// A plate remembers when, where and on which machine it was sent. The list is GUI-free so the
// rules (cap, ordering, which actions turn the plate "printed", when the plate counts as modified
// since its last send) can be unit tested; the GUI side (PartPlate, the history pop-up, the send
// hooks) only feeds it.
//
// Persistence: the list rides in the project 3MF as one <metadata key="edgeslicer_print_history">
// element per plate (JSON value), written only when the list is not empty. A build that does not
// know the key ignores it. See Format/bbs_3mf.cpp.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Point.hpp"

namespace Slic3r {

class Model;

namespace PlateHistory {

// What happened to the plate. "Exported" is a file written to disk: it is on the list, but it does
// not mean the plate went to a printer, so it never turns the plate icon green.
enum class Action { Sent, SentAndStarted, UploadedOnly, Exported };

// Stable keys used in the 3MF and in the UI mapping. Unknown keys read back as Sent.
const char *action_key(Action a);
Action      action_from_key(const std::string &key);

// True for the actions that put the plate on a printer.
inline bool is_printer_action(Action a) { return a != Action::Exported; }

struct Entry
{
    // Short random id; lets a later step of the same send (an upload that the printer then
    // starts) upgrade the entry instead of adding a second one. Empty on entries from files that
    // predate it.
    std::string uid;
    // UTC, "YYYY-MM-DDTHH:MM:SSZ". Sorts as text.
    std::string time_utc;
    // The local offset from UTC at that moment, in minutes (the time shown in the pop-up is
    // time_utc + this, so a file opened in another time zone still shows when it happened here).
    int         utc_offset_min { 0 };
    std::string printer_name;
    std::string printer_model;
    // How it got there: bambu_lan, bambu_cloud, snapmaker_lan, snapmaker_cloud, moonraker, octoprint,
    // prusalink, prusaconnect, flashforge, duet, ..., phone_hub, file. Free text for hosts this
    // list does not know.
    std::string connection;
    Action      action { Action::Sent };
    // The file name the printer was given, or the path's file name for an export.
    std::string file_name;
    // Optional: 0 = unknown.
    int         time_estimate_s { 0 };
    double      filament_g { 0.0 };
    // Fingerprint of the plate's slice input when this was done (plate_input_fingerprint()).
    // Empty = unknown, which never reports the plate as modified.
    std::string input_hash;
};

inline bool operator==(const Entry &a, const Entry &b)
{
    return a.uid == b.uid && a.time_utc == b.time_utc && a.utc_offset_min == b.utc_offset_min &&
           a.printer_name == b.printer_name && a.printer_model == b.printer_model && a.connection == b.connection &&
           a.action == b.action && a.file_name == b.file_name && a.time_estimate_s == b.time_estimate_s &&
           a.filament_g == b.filament_g && a.input_hash == b.input_hash;
}

class History
{
public:
    // A plate keeps its last 50 sends: more is noise in a pop-up and in every saved project.
    static constexpr size_t MAX_ENTRIES = 50;

    // Adds an entry and keeps the list ordered by time (oldest first); beyond MAX_ENTRIES the
    // oldest are dropped. An entry with an empty time is stamped "now".
    void add(Entry e);

    // Oldest first.
    const std::vector<Entry> &entries() const { return m_entries; }
    // Newest first, for display.
    std::vector<Entry> newest_first() const;

    bool   empty() const { return m_entries.empty(); }
    size_t size() const { return m_entries.size(); }
    void   clear() { m_entries.clear(); }

    // The plate has been sent to a printer at least once (an export does not count).
    bool was_sent() const;
    // The most recent entry that put the plate on a printer; nullptr if none.
    const Entry *last_sent() const;

    // The plate's slice input differs from what it was at the last send. False when never sent or
    // when either fingerprint is unknown. Exports are not compared: what a printer received is
    // the question.
    bool modified_since_last_send(const std::string &current_fingerprint) const;

    // Changes the action of the entry with this uid (an upload that was then started). False when
    // there is no such entry.
    bool set_action(const std::string &uid, Action action);

    // JSON text. An empty history serialises to an empty string (nothing is written to the 3MF).
    std::string serialize() const;
    // Tolerant: garbage, wrong types and unknown keys give an empty or partial list, never throw.
    static History deserialize(const std::string &text);

    bool operator==(const History &o) const { return m_entries == o.m_entries; }

private:
    std::vector<Entry> m_entries;
};

// Current time as "YYYY-MM-DDTHH:MM:SSZ" and the local UTC offset in minutes.
std::string now_utc_iso();
int         local_utc_offset_minutes();
// "2026-10-05 16:03" - the entry's UTC time shifted by its own recorded offset. Empty on bad input.
std::string format_local(const Entry &e);

// A short random hex id.
std::string make_uid();

// A fingerprint of what the plate would slice from: for each (object index, instance index) pair,
// the instance's placement relative to the plate origin, its volumes' geometry statistics,
// transforms and settings, and the object's settings. Independent of the plate's position on the
// bed, of object order, and of names, so reordering plates or renaming an object does not count as
// a change, and stable across a save and reopen. Geometry is summarised (counts and bounding box)
// rather than hashed vertex by vertex, so float round trips through the 3MF cannot flip it.
std::string plate_input_fingerprint(const Model &model, const std::vector<std::pair<int, int>> &objects_and_instances,
                                    const Vec3d &plate_origin);

} // namespace PlateHistory
} // namespace Slic3r
