#include "PresetMirrorCore.hpp"

#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace Slic3r { namespace GUI { namespace mirror {

// ---- manifest ---------------------------------------------------------------------------------

std::map<std::string, Entry> parse_manifest(const std::string& json_text)
{
    std::map<std::string, Entry> out;
    json j;
    try {
        j = json::parse(json_text);
    } catch (...) {
        return out;   // malformed -> behave as if empty; callers then treat every file as new
    }
    if (!j.is_object())
        return out;

    // Current shape { "files": { "<rel>": {"t":N,"deleted":b} } }; legacy flat shape { "<rel>": {"t":N} }.
    const json* files = nullptr;
    if (j.contains("files") && j["files"].is_object())
        files = &j["files"];
    else
        files = &j;

    for (auto it = files->begin(); it != files->end(); ++it) {
        if (!it.value().is_object())
            continue;
        const json& v = it.value();
        if (!v.contains("t"))
            continue;
        Entry e;
        try {
            if (v["t"].is_number())
                e.t = v["t"].get<long long>();
        } catch (...) {
            e.t = 0;
        }
        try {
            if (v.contains("deleted") && v["deleted"].is_boolean())
                e.deleted = v["deleted"].get<bool>();
        } catch (...) {
            e.deleted = false;
        }
        out[it.key()] = e;
    }
    return out;
}

std::string dump_manifest(const std::map<std::string, Entry>& files)
{
    json j      = json::object();
    json fj     = json::object();
    for (const auto& kv : files)
        fj[kv.first] = json{{"t", kv.second.t}, {"deleted", kv.second.deleted}};
    j["files"] = fj;
    return j.dump(1);
}

// ---- the parse guard --------------------------------------------------------------------------

bool preset_is_parseable(const std::string&              json_text,
                         const std::vector<std::string>& nullable_keys,
                         std::string*                    reason)
{
    json j;
    try {
        j = json::parse(json_text);
    } catch (const std::exception& e) {
        if (reason) *reason = std::string("invalid json: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (reason) *reason = "top level is not an object";
        return false;
    }

    const std::set<std::string> nullable(nullable_keys.begin(), nullable_keys.end());

    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        if (nullable.count(key))
            continue;   // "nil" is a legal value for this option in this fork

        // A literal "nil", either as the whole value or as one element of a per-extruder array,
        // is what makes ConfigOption*::deserialize() throw for a non-nullable option - and that
        // throw is what makes PresetCollection::load_presets() delete the file.
        auto is_nil = [](const json& v) {
            if (!v.is_string())
                return false;
            std::string s = v.get<std::string>();
            // trim
            const char* ws = " \t\r\n";
            auto b = s.find_first_not_of(ws);
            if (b == std::string::npos)
                return false;
            auto e = s.find_last_not_of(ws);
            return s.substr(b, e - b + 1) == "nil";
        };

        if (is_nil(it.value())) {
            if (reason) *reason = "nil value for non-nullable option '" + key + "'";
            return false;
        }
        if (it.value().is_array()) {
            for (const auto& el : it.value()) {
                if (is_nil(el)) {
                    if (reason) *reason = "nil element for non-nullable option '" + key + "'";
                    return false;
                }
            }
        }
    }
    return true;
}

bool sanitize_nil_arrays(const std::string&              json_text,
                         const std::vector<std::string>& nullable_keys,
                         std::string*                    out,
                         int*                            collapsed)
{
    if (collapsed) *collapsed = 0;

    json j;
    try {
        j = json::parse(json_text);
    } catch (...) {
        return false;
    }
    if (!j.is_object())
        return false;

    const std::set<std::string> nullable(nullable_keys.begin(), nullable_keys.end());

    auto is_nil = [](const json& v) {
        if (!v.is_string())
            return false;
        std::string s = v.get<std::string>();
        const char* ws = " \t\r\n";
        auto b2 = s.find_first_not_of(ws);
        if (b2 == std::string::npos)
            return false;
        auto e2 = s.find_last_not_of(ws);
        return s.substr(b2, e2 - b2 + 1) == "nil";
    };

    int changed = 0;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (nullable.count(it.key()))
            continue;               // nil is legal here; leave it exactly as Bambu wrote it
        if (!it.value().is_array())
            continue;

        bool              any_nil = false;
        std::vector<json> keep;
        for (const auto& el : it.value()) {
            if (is_nil(el)) { any_nil = true; continue; }
            keep.push_back(el);
        }
        if (!any_nil || keep.empty())
            continue;               // nothing to do, or nothing real to collapse to

        // Only collapse when every remaining slot agrees - otherwise the array really is
        // per-extruder and we must not invent a single value for it.
        bool all_same = true;
        for (size_t i = 1; i < keep.size(); ++i)
            if (keep[i] != keep[0]) { all_same = false; break; }
        if (!all_same)
            continue;

        it.value() = json::array({keep[0]});
        ++changed;
    }

    if (changed == 0)
        return false;
    if (collapsed) *collapsed = changed;
    if (out) *out = j.dump(1);
    return true;
}

// ---- dest matching (content-hash adoption) ----------------------------------------------------

std::string info_inert(const std::string& src_info)
{
    std::istringstream       in(src_info);
    std::vector<std::string> lines;
    std::string              line;
    bool                     had_sync = false;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.rfind("sync_info", 0) == 0) {
            lines.push_back("sync_info = ");
            had_sync = true;
        } else {
            lines.push_back(line);
        }
    }
    if (!had_sync)
        lines.push_back("sync_info = ");
    std::string out;
    for (const auto& l : lines)
        out += l + "\n";
    return out;
}

bool dest_matches_mirror(const SourceFile& f, const DestState& dst)
{
    auto bit = dst.bytes.find(f.rel);
    if (bit == dst.bytes.end() || bit->second.empty() || f.body.empty())
        return false;

    const bool raw = bit->second == f.body;
    bool       sanitized = false;
    if (!raw) {
        std::string fixed;
        sanitized = sanitize_nil_arrays(f.body, dst.nullable_keys, &fixed) && bit->second == fixed;
    }
    if (!raw && !sanitized)
        return false;

    // base\ entries have no .info; a source with no sidecar is not rewritten either.
    if (f.rel.find("/base/") != std::string::npos || f.info.empty())
        return true;

    auto iit = dst.info_bytes.find(f.rel);
    if (iit == dst.info_bytes.end())
        return false;
    return iit->second == info_inert(f.info);
}

// ---- the plan ---------------------------------------------------------------------------------

std::vector<PlanItem> build_plan(const SourceListing&                src,
                                 const std::map<std::string, Entry>& manifest,
                                 const DestState&                    dst)
{
    std::vector<PlanItem> plan;

    // A source listing we could not trust is a strict no-op. Returning an empty plan here is the
    // whole safety property: no Retire, no RespectDelete, nothing for a caller to act on.
    if (!src.ok)
        return plan;

    auto dst_present = [&dst](const std::string& rel) {
        auto it = dst.present.find(rel);
        return it != dst.present.end() && it->second;
    };

    std::set<std::string> seen;

    for (const SourceFile& f : src.files) {
        seen.insert(f.rel);

        auto mit      = manifest.find(f.rel);
        bool tracked  = mit != manifest.end();
        bool exists   = dst_present(f.rel);

        PlanItem item;
        item.rel = f.rel;
        item.t   = f.t;

        if (exists && !tracked) {
            // Untracked dest whose bytes already equal what this run would write
            // (raw, or the sanitized rewrite, plus the inert .info). Adopt it so
            // a crash after the copy and before persist — or main's old
            // non-atomic write — does not strand the file as ProtectNative.
            // A byte-different dest stays someone else's file.
            if (dest_matches_mirror(f, dst)) {
                item.action = Action::Adopt;
                plan.push_back(item);
            } else {
                item.action = Action::ProtectNative;
                item.t      = 0;
                plan.push_back(item);
            }
            continue;
        }

        if (exists && tracked) {
            if (f.t <= mit->second.t) {
                item.action = Action::UpToDate;
                item.t      = mit->second.t;
            } else if (!f.parseable) {
                // Source moved on but we still cannot read it - leave our older copy in place.
                item.action = Action::SkipUnparseable;
                item.t      = mit->second.t;
            } else {
                item.action = Action::Copy;
            }
            plan.push_back(item);
            continue;
        }

        // Not on disk.
        if (tracked && f.t <= mit->second.t) {
            // We copied it, the user removed it, the source has not changed since: honour that.
            item.action = Action::RespectDelete;
            item.t      = mit->second.t;
            plan.push_back(item);
            continue;
        }

        // New, or the source was edited after the user deleted our copy -> (re-)pull it, but only
        // if the fork can actually read it. Copying an unparseable preset is what triggered the
        // loader's hard delete, so we skip instead.
        item.action = f.parseable ? Action::Copy : Action::SkipUnparseable;
        if (!f.parseable)
            item.t = tracked ? mit->second.t : 0;
        plan.push_back(item);
    }

    // Tracked entries the source no longer lists. Only reachable with src.ok == true.
    for (const auto& kv : manifest) {
        if (seen.count(kv.first))
            continue;
        if (dst_present(kv.first))
            continue;   // still on disk though unlisted: leave it and its entry alone
        PlanItem item;
        item.rel    = kv.first;
        item.action = Action::Retire;
        item.t      = kv.second.t;
        plan.push_back(item);
    }

    return plan;
}

std::map<std::string, Entry> apply_plan(const std::map<std::string, Entry>& manifest,
                                        const std::vector<PlanItem>&        plan,
                                        const std::set<std::string>&        copied)
{
    std::map<std::string, Entry> out = manifest;
    for (const PlanItem& p : plan) {
        switch (p.action) {
        case Action::Copy:
            // Only copies that actually landed. Recording a refused Copy marks
            // a missing file as mirrored, and the next run RespectDeletes it.
            if (copied.count(p.rel))
                out[p.rel] = Entry{p.t, false};
            break;
        case Action::Adopt:
            // Dest already matches; no file write. Always record so the next
            // run treats it as ours (UpToDate / Copy-on-edit) instead of
            // ProtectNative forever.
            out[p.rel] = Entry{p.t, false};
            break;
        case Action::RespectDelete:
            out[p.rel] = Entry{p.t, true};
            break;
        case Action::Retire:
            out.erase(p.rel);
            break;
        case Action::UpToDate:
        case Action::ProtectNative:
        case Action::SkipUnparseable:
        default:
            break;   // manifest untouched
        }
    }
    return out;
}

bool write_manifest_bytes(const std::string& path, const std::string& dump, std::string* err)
{
    return write_file_atomically(path, dump, err, true);
}

bool commit_pending_copies(std::set<std::string>&              copied,
                           std::vector<std::string>&           pending,
                           const std::map<std::string, Entry>& manifest,
                           const std::vector<PlanItem>&        plan,
                           const std::string&                  path,
                           std::string*                        err,
                           ManifestWriteFn                     write)
{
    std::set<std::string> next = copied;
    for (const auto& rel : pending)
        next.insert(rel);
    const std::string dump = dump_manifest(apply_plan(manifest, plan, next));
    const bool        ok   = write ? write(path, dump, err) : write_manifest_bytes(path, dump, err);
    if (!ok)
        return false;
    copied = std::move(next);
    pending.clear();
    return true;
}

bool copy_run_ends_at(const std::vector<PlanItem>& plan, size_t i)
{
    if (i >= plan.size() || plan[i].action != Action::Copy)
        return false;
    return i + 1 == plan.size() || plan[i + 1].action != Action::Copy;
}

bool CopyBatch::should_flush(const std::vector<PlanItem>& plan, size_t i) const
{
    return !skip_remaining && copy_run_ends_at(plan, i);
}

// ---- source directory resolution ---------------------------------------------------------------

int choose_uid(const std::vector<UidCandidate>& cands, const std::string& logged_in_uid)
{
    auto stable = [](const UidCandidate& c) { return c.root == "BambuStudio"; };

    if (!logged_in_uid.empty()) {
        int best = -1;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (cands[i].uid != logged_in_uid)
                continue;
            if (best < 0 || (stable(cands[i]) && !stable(cands[best])))
                best = (int) i;
        }
        if (best >= 0)
            return best;
    }

    int best = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (best < 0) {
            best = (int) i;
            continue;
        }
        if (cands[i].mtime > cands[best].mtime)
            best = (int) i;
        else if (cands[i].mtime == cands[best].mtime && stable(cands[i]) && !stable(cands[best]))
            best = (int) i;
    }
    return best;
}

}}} // namespace Slic3r::GUI::mirror
