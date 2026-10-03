#ifndef slic3r_GUI_PresetMirrorCore_hpp_
#define slic3r_GUI_PresetMirrorCore_hpp_

#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <set>

// GUI-free core of the Bambu Studio user-preset mirror. Everything here is pure logic over plain
// values (no wxWidgets, no AppConfig, no PresetBundle), so tests/slic3rutils can exercise it.
//
// The mirror used to be a thin "copy if newer" loop. Two independent faults made it destructive:
//
//  1. It handed Bambu multi-extruder presets straight to PresetCollection::load_presets(), which
//     HARD-DELETES (fs::remove on the .json AND the .info) any file whose JSON the fork's config
//     deserializer rejects - see Preset.cpp, the `if (!reason.empty())` branch. Bambu writes
//     per-extruder arrays with a literal "nil" hole (e.g. top_surface_acceleration =
//     ["100","nil","100","nil","nil"]) for H2C/H2D/H2S. Those keys are non-nullable scalars here
//     (coFloat), so deserialize() throws "Deserializing nil into a non-nullable object" and the
//     loader deletes the preset the mirror had just copied in.
//  2. Nothing distinguished "the source listed zero presets because enumeration failed" from "the
//     user really deleted everything upstream", so a bad source read could sweep the manifest.
//
// The rules below fix both by construction: a source listing must be explicitly known-good before
// any entry is retired, and a preset that the fork cannot parse is never copied in the first place.

namespace Slic3r { namespace GUI { namespace mirror {

// ---- manifest ---------------------------------------------------------------------------------

struct Entry {
    long long   t       = 0;      // source updated_time / mtime at the time we copied it
    bool        deleted = false;  // user removed our copy; don't re-pull unless the source is newer

    bool operator==(const Entry& o) const { return t == o.t && deleted == o.deleted; }
    bool operator!=(const Entry& o) const { return !(*this == o); }
};

// Parse / serialize the manifest at user\default\.bs_mirror_manifest.json. Round-trips, tolerates
// the legacy flat spike shape { "<rel>": {"t": N} } and any malformed input (-> empty).
std::map<std::string, Entry> parse_manifest(const std::string& json_text);
std::string                  dump_manifest(const std::map<std::string, Entry>& files);

// ---- source enumeration -----------------------------------------------------------------------

// One preset as found in the Bambu Studio tree.
struct SourceFile {
    std::string rel;        // manifest key, always '/'-separated, e.g. "filament/Foo.json"
    long long   t = 0;      // updated_time from the .info, else mtime
    bool        parseable = true;  // the fork's config loader can read it (see preset_is_parseable)
    std::string body;       // raw source JSON; needed to adopt an untracked dest by content
    std::string info;       // raw source .info (empty when the source has none, e.g. base/)
};

// Outcome of listing the source tree. `ok` is the safety gate: false means we could not read the
// source (missing dir, iteration error, exception). A not-ok listing must never retire anything.
struct SourceListing {
    bool                    ok = false;
    std::vector<SourceFile> files;
};

// ---- the parse guard --------------------------------------------------------------------------

// True when the fork's config deserializer would accept this preset JSON. Today the only known
// rejection that reaches a hard delete is a literal "nil" element inside a value for an option the
// fork defines as non-nullable; `nullable_keys` carries the keys for which nil IS legal here.
// Copying a preset that fails this check is what used to get it deleted moments later.
bool preset_is_parseable(const std::string& json_text,
                         const std::vector<std::string>& nullable_keys,
                         std::string* reason = nullptr);

// Rewrite a Bambu preset so this fork can load it, WITHOUT changing what it means.
//
// Bambu stores per-extruder arrays on multi-extruder machines (H2C/H2D/H2S) and writes "nil" for
// the slots an extruder does not use, e.g. "nozzle_temperature": ["230", "nil"]. The fork models
// these options as non-nullable, so the array cannot be loaded at all. Where every non-nil slot
// carries the SAME value the array is just that one value plus unused slots, so it collapses to
// the scalar with no loss - which is exactly how the preset already behaves.
//
// Only that unambiguous case is rewritten. A genuinely per-extruder array with differing values
// (or one that is entirely nil) has no single correct answer, so it is left alone and the preset
// stays unparseable rather than being silently given a value the user never chose.
//
// Returns true when `out` holds a rewritten document; false when nothing needed changing or the
// input could not be parsed. `collapsed` counts the keys rewritten.
bool sanitize_nil_arrays(const std::string&              json_text,
                         const std::vector<std::string>& nullable_keys,
                         std::string*                    out,
                         int*                            collapsed = nullptr);

// ---- the plan ---------------------------------------------------------------------------------

enum class Action {
    Copy,          // new, or the source is newer than what we copied
    UpToDate,      // our copy matches the source
    Adopt,         // untracked dest whose bytes already equal what we would write -> record, don't overwrite
    ProtectNative, // a fork-native file sits at this path and we don't own it -> never touch
    RespectDelete, // the user deleted our copy and the source has not changed -> record, don't re-pull
    SkipUnparseable, // the fork's loader would reject (and then delete) it -> never copy
    Retire         // tracked, gone from BOTH sides -> drop from the manifest (only when listing ok)
};

struct PlanItem {
    std::string rel;
    Action      action = Action::UpToDate;
    long long   t      = 0;   // manifest time to record when the action is applied
};

// Inputs describing the destination side.
struct DestState {
    // rel -> exists on disk in the fork's user\default
    std::map<std::string, bool> present;
    // dest JSON bytes, keyed by rel. Only untracked dests with a populated
    // entry can be adopted; missing/empty bytes stay ProtectNative.
    std::map<std::string, std::string> bytes;
    // dest .info bytes, keyed by the preset rel (not the .info path).
    std::map<std::string, std::string> info_bytes;
    // fork-nullable option keys, used when matching a sanitized dest.
    std::vector<std::string>           nullable_keys;
};

// Blank sync_info the way the mirror writes a copied .info. Used both to write
// the sidecar and to decide whether an untracked dest already matches it.
std::string info_inert(const std::string& src_info);

// True when dest JSON equals the raw source body or the sanitized rewrite the
// mirror would write, and (when the mirror writes an .info) dest .info equals
// info_inert(source .info). Empty/missing dest or source bytes never match.
bool dest_matches_mirror(const SourceFile& f, const DestState& dst);

// Build the full plan. This is the whole decision procedure, and it is total: every rel in the
// listing and every rel in the manifest gets exactly one PlanItem.
//
// SAFETY INVARIANT (the bug this fixes): when `src.ok` is false the plan contains NO Retire and NO
// RespectDelete - a failed or empty-because-unreadable enumeration is a strict no-op. Callers must
// not apply anything destructive without `src.ok`.
std::vector<PlanItem> build_plan(const SourceListing&               src,
                                 const std::map<std::string, Entry>& manifest,
                                 const DestState&                   dst);

// Apply a plan to a manifest, returning the new manifest. RespectDelete updates
// entries, Retire drops them, Adopt is always recorded (the dest already
// matches; no file write). Copy is recorded only for rels in `copied` — a
// refused or failed copy (or a copy whose manifest write failed) stays pending
// so the next run retries it instead of treating it as done or as a user
// deletion. There is no two-argument overload: omitting `copied` used to
// record every planned Copy.
std::map<std::string, Entry> apply_plan(const std::map<std::string, Entry>& manifest,
                                        const std::vector<PlanItem>&        plan,
                                        const std::set<std::string>&        copied);

// True when plan[i] is Copy and the next item is not (or i is last). The GUI
// flushes the pending batch at that boundary so a later lock steal cannot
// drop already-landed copies.
bool copy_run_ends_at(const std::vector<PlanItem>& plan, size_t i);

// End-of-run flush + "flush failed, skip remaining copies". Factored out of
// the GUI so the skip decision is testable without wxWidgets.
struct CopyBatch {
    bool skip_remaining = false;

    bool should_flush(const std::vector<PlanItem>& plan, size_t i) const;
    void on_flush_failed() { skip_remaining = true; }
    void abort() { skip_remaining = true; }
};

// Atomic replace of the manifest file (write_file_atomically, binary). A
// crash or full disk leaves the previous complete file, which parse_manifest
// can read. A truncate-write can leave a torn file that parses as empty.
bool write_manifest_bytes(const std::string& path, const std::string& dump, std::string* err = nullptr);

// Persist apply_plan(manifest, plan, copied ∪ pending). On success, pending
// rels are moved into `copied` and cleared. On failure `copied` and `pending`
// are unchanged — those copies are not recorded. `write` defaults to
// write_manifest_bytes; tests inject a failing or inspecting writer.
using ManifestWriteFn = std::function<bool(const std::string& path, const std::string& dump, std::string* err)>;
bool commit_pending_copies(std::set<std::string>&              copied,
                           std::vector<std::string>&           pending,
                           const std::map<std::string, Entry>& manifest,
                           const std::vector<PlanItem>&        plan,
                           const std::string&                  path,
                           std::string*                        err   = nullptr,
                           ManifestWriteFn                     write = {});

// ---- source directory resolution ---------------------------------------------------------------

// A candidate Bambu Studio user dir discovered under %APPDATA%.
struct UidCandidate {
    std::string root;      // "BambuStudio" or "BambuStudioBeta"
    std::string uid;       // numeric directory name
    long long   mtime = 0; // last write time, used only to break ties
};

// Pick the right user dir. Rules, in order:
//   1. an exact match on the logged-in uid, preferring the stable root over Beta;
//   2. otherwise the newest numeric uid, again preferring stable on a tie.
// Returns the index into `cands`, or -1 when there is nothing usable.
int choose_uid(const std::vector<UidCandidate>& cands, const std::string& logged_in_uid);

}}} // namespace Slic3r::GUI::mirror

#endif // slic3r_GUI_PresetMirrorCore_hpp_
