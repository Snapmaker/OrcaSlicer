// Tests for the Bambu Studio user-preset mirror core (src/slic3r/GUI/PresetMirrorCore.*).
//
// Background: "Sync Now" removed ~208 previously mirrored presets from the fork's user\default.
// Two faults combined:
//   * the mirror copied Bambu multi-extruder presets whose per-extruder arrays carry a literal
//     "nil" hole; the fork's config deserializer rejects nil for non-nullable options, and
//     PresetCollection::load_presets() HARD-DELETES (.json + .info) any preset it fails to parse;
//   * nothing separated "source enumeration failed" from "the user deleted everything upstream".
//
// The rule these tests pin down: a sync must never retire or delete anything unless the source was
// enumerated successfully, and a preset the fork cannot parse must never be copied in.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PresetMirrorCore.hpp"
#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <set>

using namespace Slic3r::GUI::mirror;

namespace {

Action action_for(const std::vector<PlanItem>& plan, const std::string& rel)
{
    auto it = std::find_if(plan.begin(), plan.end(), [&](const PlanItem& p) { return p.rel == rel; });
    REQUIRE(it != plan.end());
    return it->action;
}

bool has(const std::vector<PlanItem>& plan, const std::string& rel)
{
    return std::any_of(plan.begin(), plan.end(), [&](const PlanItem& p) { return p.rel == rel; });
}

SourceFile sf(const std::string& rel, long long t, bool parseable = true)
{
    SourceFile f;
    f.rel       = rel;
    f.t         = t;
    f.parseable = parseable;
    return f;
}

std::set<std::string> copies_of(const std::vector<PlanItem>& plan)
{
    std::set<std::string> s;
    for (const auto& p : plan)
        if (p.action == Action::Copy)
            s.insert(p.rel);
    return s;
}

} // namespace

// ---- the regression: a bad source listing must never delete ------------------------------------

TEST_CASE("an unreadable source directory is a strict no-op", "[PresetMirror]")
{
    std::map<std::string, Entry> man{
        {"filament/Mirrored A.json", {100, false}},
        {"process/Mirrored B.json", {100, false}},
    };
    DestState dst;
    dst.present["filament/Mirrored A.json"] = true;
    dst.present["process/Mirrored B.json"]  = true;

    SourceListing src;           // ok == false: we could not read the Bambu tree at all
    src.ok = false;

    auto plan = build_plan(src, man, dst);

    CHECK(plan.empty());
    // and nothing is retired or flagged deleted
    auto after = apply_plan(man, plan, {});
    CHECK(after == man);
}

TEST_CASE("an empty-but-failed listing never retires tracked presets", "[PresetMirror]")
{
    std::map<std::string, Entry> man{{"filament/A.json", {5, false}}};
    DestState dst;
    dst.present["filament/A.json"] = true;

    SourceListing bad;
    bad.ok = false;   // zero files because enumeration failed
    CHECK(build_plan(bad, man, dst).empty());

    // Contrast: a *successful* listing that is genuinely empty, with the file also gone from disk,
    // is the one case where retiring is correct.
    SourceListing good;
    good.ok = true;
    DestState gone;
    gone.present["filament/A.json"] = false;
    auto plan = build_plan(good, man, gone);
    REQUIRE(has(plan, "filament/A.json"));
    CHECK(action_for(plan, "filament/A.json") == Action::Retire);
    CHECK(apply_plan(man, plan, {}).empty());
}

// ---- the parse guard ---------------------------------------------------------------------------

TEST_CASE("a preset with a nil hole in a non-nullable option is never copied", "[PresetMirror]")
{
    // This is the exact shape Bambu Studio writes for H2C/H2D/H2S multi-extruder machines.
    const std::string bambu = R"({
        "name": "0.08mm @H2D - Minis",
        "type": "process",
        "top_surface_acceleration": ["100", "nil", "100", "nil", "nil"]
    })";

    std::string reason;
    CHECK_FALSE(preset_is_parseable(bambu, {}, &reason));
    CHECK(reason.find("top_surface_acceleration") != std::string::npos);

    // It must be planned as SkipUnparseable, not Copy - copying it is what got it deleted.
    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("process/0.08mm @H2D - Minis.json", 10, /*parseable*/ false));

    auto plan = build_plan(src, {}, DestState{});
    CHECK(action_for(plan, "process/0.08mm @H2D - Minis.json") == Action::SkipUnparseable);
    CHECK(apply_plan({}, plan, {}).empty());   // and it is not recorded as mirrored
}

TEST_CASE("nil is accepted for options this fork defines as nullable", "[PresetMirror]")
{
    const std::string filament = R"({
        "name": "Some Filament",
        "filament_retraction_length": ["nil", "0.8"]
    })";
    CHECK(preset_is_parseable(filament, {"filament_retraction_length"}));
    CHECK_FALSE(preset_is_parseable(filament, {}));
}

TEST_CASE("ordinary presets and malformed json are classified correctly", "[PresetMirror]")
{
    CHECK(preset_is_parseable(R"({"name":"Plain","wall_loops":"3"})", {}));
    CHECK_FALSE(preset_is_parseable("{ not json", {}));
    CHECK_FALSE(preset_is_parseable("[1,2,3]", {}));
    // "nil" as a substring is fine; only a standalone value counts.
    CHECK(preset_is_parseable(R"({"name":"nil-ish","notes":"nil pointer"})", {}));
}

TEST_CASE("unambiguous per-extruder nil padding is collapsed", "[PresetMirror]")
{
    // Bambu's shape for a 2-extruder machine where only the first is used.
    const std::string bambu = R"({
        "name": "Amolen PLA Silk",
        "nozzle_temperature": ["230", "nil"],
        "hot_plate_temp": ["60", "nil", "60"]
    })";

    std::string out;
    int         collapsed = 0;
    REQUIRE(sanitize_nil_arrays(bambu, {}, &out, &collapsed));
    CHECK(collapsed == 2);

    // and the result is now loadable
    CHECK(preset_is_parseable(out, {}));

    auto j = nlohmann::json::parse(out);
    CHECK(j["nozzle_temperature"] == nlohmann::json::array({"230"}));
    CHECK(j["hot_plate_temp"] == nlohmann::json::array({"60"}));
    CHECK(j["name"] == "Amolen PLA Silk");   // untouched
}

TEST_CASE("a genuinely per-extruder array is never collapsed", "[PresetMirror]")
{
    // Differing real values: there is no single correct answer, so we must not invent one.
    const std::string mixed = R"({"filament_flow_ratio": ["0.96", "0.99", "nil"]})";
    std::string out;
    CHECK_FALSE(sanitize_nil_arrays(mixed, {}, &out));
    CHECK_FALSE(preset_is_parseable(mixed, {}));   // still refused, so still never copied

    // An all-nil array has nothing to collapse to either.
    const std::string empty = R"({"filament_flow_ratio": ["nil", "nil"]})";
    CHECK_FALSE(sanitize_nil_arrays(empty, {}, &out));
}

TEST_CASE("sanitizing leaves nullable options alone", "[PresetMirror]")
{
    // nil is meaningful for these - collapsing would change behaviour.
    const std::string s = R"({"filament_retraction_length": ["nil", "0.8"]})";
    std::string out;
    CHECK_FALSE(sanitize_nil_arrays(s, {"filament_retraction_length"}, &out));
}

TEST_CASE("sanitizing is a no-op for clean presets and malformed input", "[PresetMirror]")
{
    std::string out;
    CHECK_FALSE(sanitize_nil_arrays(R"({"name":"Clean","wall_loops":"3"})", {}, &out));
    CHECK_FALSE(sanitize_nil_arrays("{ not json", {}, &out));
}

// ---- deletion semantics --------------------------------------------------------------------------

TEST_CASE("a genuine upstream deletion is respected and not re-pulled", "[PresetMirror]")
{
    // Tracked, still listed upstream unchanged, but the user removed our copy.
    std::map<std::string, Entry> man{{"filament/Gone.json", {50, false}}};
    DestState dst;
    dst.present["filament/Gone.json"] = false;

    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/Gone.json", 50));

    auto plan = build_plan(src, man, dst);
    CHECK(action_for(plan, "filament/Gone.json") == Action::RespectDelete);

    auto after = apply_plan(man, plan, {});
    REQUIRE(after.count("filament/Gone.json") == 1);
    CHECK(after["filament/Gone.json"].deleted);

    // A later sync still does not re-pull it while the source is unchanged.
    DestState still_gone;
    still_gone.present["filament/Gone.json"] = false;
    auto plan2 = build_plan(src, after, still_gone);
    CHECK(action_for(plan2, "filament/Gone.json") == Action::RespectDelete);
}

TEST_CASE("a deletion is re-pulled once Bambu Studio edits the preset again", "[PresetMirror]")
{
    std::map<std::string, Entry> man{{"filament/Gone.json", {50, true}}};
    DestState dst;
    dst.present["filament/Gone.json"] = false;

    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/Gone.json", 99));   // edited upstream since the deletion

    auto plan = build_plan(src, man, dst);
    CHECK(action_for(plan, "filament/Gone.json") == Action::Copy);
    auto after = apply_plan(man, plan, copies_of(plan));
    CHECK_FALSE(after["filament/Gone.json"].deleted);
    CHECK(after["filament/Gone.json"].t == 99);
}

TEST_CASE("a fork-native preset is never touched", "[PresetMirror]")
{
    // Present on disk, absent from the manifest, and not byte-equal to the source:
    // we do not own it, even on a name collision. (Matching bytes are Adopt, below.)
    DestState dst;
    dst.present["filament/My Own.json"] = true;

    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/My Own.json", 1234));

    auto plan = build_plan(src, {}, dst);
    CHECK(action_for(plan, "filament/My Own.json") == Action::ProtectNative);
    CHECK(apply_plan({}, plan, {}).empty());   // never adopted into the manifest
}

TEST_CASE("unchanged and newer sources are classified correctly", "[PresetMirror]")
{
    std::map<std::string, Entry> man{
        {"filament/Same.json", {10, false}},
        {"filament/Newer.json", {10, false}},
    };
    DestState dst;
    dst.present["filament/Same.json"]  = true;
    dst.present["filament/Newer.json"] = true;

    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/Same.json", 10));
    src.files.push_back(sf("filament/Newer.json", 20));

    auto plan = build_plan(src, man, dst);
    CHECK(action_for(plan, "filament/Same.json") == Action::UpToDate);
    CHECK(action_for(plan, "filament/Newer.json") == Action::Copy);
}

TEST_CASE("a tracked preset still on disk but unlisted upstream is left alone", "[PresetMirror]")
{
    std::map<std::string, Entry> man{{"filament/Kept.json", {10, false}}};
    DestState dst;
    dst.present["filament/Kept.json"] = true;

    SourceListing src;
    src.ok = true;   // successful listing that simply no longer mentions it

    auto plan = build_plan(src, man, dst);
    CHECK_FALSE(has(plan, "filament/Kept.json"));   // no Retire while the file is on disk
    CHECK(apply_plan(man, plan, {}) == man);
}

// ---- manifest round-trip -------------------------------------------------------------------------

TEST_CASE("the manifest round-trips", "[PresetMirror]")
{
    std::map<std::string, Entry> man{
        {"filament/A.json", {1754686735LL, false}},
        {"process/B - Custom.json", {1770072192LL, true}},
        {"machine/C.json", {0, false}},
    };
    auto back = parse_manifest(dump_manifest(man));
    CHECK(back == man);
}

TEST_CASE("the manifest tolerates malformed and legacy input", "[PresetMirror]")
{
    CHECK(parse_manifest("").empty());
    CHECK(parse_manifest("{ broken").empty());
    CHECK(parse_manifest("[]").empty());

    // Legacy flat spike shape is adopted, defaulting deleted to false.
    auto legacy = parse_manifest(R"({"filament/Old.json":{"t":42}})");
    REQUIRE(legacy.count("filament/Old.json") == 1);
    CHECK(legacy["filament/Old.json"].t == 42);
    CHECK_FALSE(legacy["filament/Old.json"].deleted);

    // Real shape, with the deleted flag preserved.
    auto cur = parse_manifest(R"({"files":{"process/X.json":{"t":7,"deleted":true}}})");
    REQUIRE(cur.count("process/X.json") == 1);
    CHECK(cur["process/X.json"].deleted);
}

TEST_CASE("a corrupt manifest does not cause deletions", "[PresetMirror]")
{
    // An unreadable manifest parses to empty; files on disk then look fork-native and are protected
    // rather than overwritten or removed.
    auto man = parse_manifest("{{{");
    CHECK(man.empty());

    DestState dst;
    dst.present["filament/A.json"] = true;
    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/A.json", 10));

    auto plan = build_plan(src, man, dst);
    CHECK(action_for(plan, "filament/A.json") == Action::ProtectNative);
}

// ---- uid / source-dir resolution ------------------------------------------------------------------

TEST_CASE("the logged-in uid wins and prefers the stable root over Beta", "[PresetMirror]")
{
    // Both %APPDATA%\BambuStudio and %APPDATA%\BambuStudioBeta hold a 1237263648 and a newer other uid.
    std::vector<UidCandidate> cands{
        {"BambuStudioBeta", "1237263648", 500},
        {"BambuStudio", "1237263648", 100},
        {"BambuStudio", "9999999999", 900},
    };

    int pick = choose_uid(cands, "1237263648");
    REQUIRE(pick >= 0);
    CHECK(cands[pick].root == "BambuStudio");
    CHECK(cands[pick].uid == "1237263648");
}

TEST_CASE("with no logged-in uid the newest dir wins, stable breaking ties", "[PresetMirror]")
{
    std::vector<UidCandidate> newest{
        {"BambuStudio", "111", 100},
        {"BambuStudioBeta", "222", 900},
    };
    int pick = choose_uid(newest, "");
    REQUIRE(pick >= 0);
    CHECK(newest[pick].uid == "222");

    std::vector<UidCandidate> tie{
        {"BambuStudioBeta", "111", 300},
        {"BambuStudio", "222", 300},
    };
    int pick2 = choose_uid(tie, "");
    REQUIRE(pick2 >= 0);
    CHECK(tie[pick2].root == "BambuStudio");

    CHECK(choose_uid({}, "1237263648") == -1);
    CHECK(choose_uid({}, "") == -1);
}

TEST_CASE("an unmatched logged-in uid falls back to the newest dir", "[PresetMirror]")
{
    std::vector<UidCandidate> cands{
        {"BambuStudio", "111", 100},
        {"BambuStudio", "222", 900},
    };
    int pick = choose_uid(cands, "333");
    REQUIRE(pick >= 0);
    CHECK(cands[pick].uid == "222");
}

// ---- the owner's scenario, end to end --------------------------------------------------------------

TEST_CASE("the reported mass-delete cannot happen", "[PresetMirror]")
{
    // 208 tracked, mirrored presets on disk.
    std::map<std::string, Entry> man;
    DestState dst;
    for (int i = 0; i < 208; ++i) {
        std::string rel = "filament/P" + std::to_string(i) + ".json";
        man[rel]         = Entry{1000, false};
        dst.present[rel] = true;
    }

    // Whatever goes wrong with the source read - missing dir, permission error, wrong uid - the
    // listing comes back not-ok and the sync changes nothing at all.
    SourceListing failed;
    failed.ok = false;
    auto plan = build_plan(failed, man, dst);
    CHECK(plan.empty());
    CHECK(apply_plan(man, plan, {}) == man);

    // Even a successful listing where every preset is unparseable leaves the copies in place.
    SourceListing unparseable;
    unparseable.ok = true;
    for (int i = 0; i < 208; ++i)
        unparseable.files.push_back(sf("filament/P" + std::to_string(i) + ".json", 2000, false));

    auto plan2  = build_plan(unparseable, man, dst);
    auto after2 = apply_plan(man, plan2, {});
    CHECK(after2.size() == man.size());
    for (const auto& kv : after2)
        CHECK_FALSE(kv.second.deleted);
    CHECK(std::none_of(plan2.begin(), plan2.end(), [](const PlanItem& p) {
        return p.action == Action::Retire || p.action == Action::RespectDelete;
    }));
}

TEST_CASE("a mid-mirror refusal leaves refused copies pending and the next run converges", "[PresetMirror]")
{
    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/A.json", 10));
    src.files.push_back(sf("filament/B.json", 10));
    src.files.push_back(sf("filament/C.json", 10));

    std::map<std::string, Entry> man;
    DestState                    empty;
    auto                         plan = build_plan(src, man, empty);
    CHECK(action_for(plan, "filament/A.json") == Action::Copy);
    CHECK(action_for(plan, "filament/B.json") == Action::Copy);
    CHECK(action_for(plan, "filament/C.json") == Action::Copy);

    // A landed. B, C and the batch manifest write were refused (visible
    // instance took the lock). Only A may be recorded.
    const std::set<std::string> copied_a{"filament/A.json"};
    auto                        after = apply_plan(man, plan, copied_a);
    REQUIRE(after.count("filament/A.json") == 1);
    CHECK_FALSE(after.count("filament/B.json"));
    CHECK_FALSE(after.count("filament/C.json"));

    DestState dest_after;
    dest_after.present["filament/A.json"] = true;
    auto plan2                            = build_plan(src, after, dest_after);
    CHECK(action_for(plan2, "filament/A.json") == Action::UpToDate);
    CHECK(action_for(plan2, "filament/B.json") == Action::Copy);
    CHECK(action_for(plan2, "filament/C.json") == Action::Copy);

    const std::set<std::string> copied_rest{"filament/B.json", "filament/C.json"};
    auto                        done = apply_plan(after, plan2, copied_rest);
    CHECK(done.size() == 3);
    CHECK(done.count("filament/A.json"));
    CHECK(done.count("filament/B.json"));
    CHECK(done.count("filament/C.json"));
}

TEST_CASE("a failed manifest write does not record the pending copies", "[PresetMirror]")
{
    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/A.json", 10));
    src.files.push_back(sf("filament/B.json", 10));
    auto plan = build_plan(src, {}, DestState{});

    std::set<std::string>    copied;
    std::vector<std::string> pending{"filament/A.json"};
    int                      writes = 0;
    auto fail_write = [&](const std::string&, const std::string& dump, std::string* err) {
        ++writes;
        REQUIRE(dump.find("filament/A.json") != std::string::npos);
        if (err)
            *err = "forced";
        return false;
    };
    std::string err;
    REQUIRE_FALSE(commit_pending_copies(copied, pending, {}, plan, "unused", &err, fail_write));
    REQUIRE(writes == 1);
    CHECK(copied.empty());
    CHECK(pending == std::vector<std::string>{"filament/A.json"});
    CHECK(apply_plan({}, plan, copied).empty());

    auto ok_write = [&](const std::string&, const std::string&, std::string*) {
        ++writes;
        return true;
    };
    REQUIRE(commit_pending_copies(copied, pending, {}, plan, "unused", &err, ok_write));
    CHECK(copied.count("filament/A.json"));
    CHECK(pending.empty());
}

TEST_CASE("write_manifest_bytes is atomic and a torn file is not produced", "[PresetMirror]")
{
    const auto dir  = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("mirror_%%%%%%%%");
    boost::filesystem::create_directories(dir);
    const auto path = (dir / ".bs_mirror_manifest.json").string();
    struct Cleanup
    {
        boost::filesystem::path p;
        ~Cleanup()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(p, ec);
        }
    } cleanup{dir};

    std::map<std::string, Entry> man{{"filament/A.json", {10, false}}};
    const std::string            good = dump_manifest(man);
    REQUIRE(write_manifest_bytes(path, good, nullptr));
    REQUIRE(parse_manifest([&] {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }()) == man);

    Slic3r::set_atomic_write_force_fail_hook([](const char*) { return true; });
    std::string err;
    std::map<std::string, Entry> newer{{"filament/A.json", {10, false}}, {"filament/B.json", {11, false}}};
    REQUIRE_FALSE(write_manifest_bytes(path, dump_manifest(newer), &err));
    Slic3r::set_atomic_write_force_fail_hook(nullptr);
    REQUIRE_FALSE(err.empty());

    std::ifstream in(path, std::ios::binary);
    const std::string on_disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(parse_manifest(on_disk) == man);
    CHECK(on_disk.find("filament/B.json") == std::string::npos);

    // A torn prefix parses as empty, so every file looks fork-native (ProtectNative).
    // Atomic persist makes that file unproducible; this pins the parse behaviour.
    CHECK(parse_manifest("{\"files\":{\"filament/A.json\":{\"t\":10").empty());
    DestState dst;
    dst.present["filament/A.json"] = true;
    SourceListing src;
    src.ok = true;
    src.files.push_back(sf("filament/A.json", 10));
    auto plan = build_plan(src, parse_manifest("{\"files\":{\"filament/A.json\":{\"t\":10"), dst);
    CHECK(action_for(plan, "filament/A.json") == Action::ProtectNative);
}

// ---- content-hash adoption (S1) + batch flush (S2) --------------------------------------------

TEST_CASE("info_inert blanks sync_info the way the mirror writes the sidecar", "[PresetMirror]")
{
    CHECK(info_inert("user_id = 1\nsync_info = upload\nsetting_id = x\n")
          == "user_id = 1\nsync_info = \nsetting_id = x\n");
    CHECK(info_inert("user_id = 1\n") == "user_id = 1\nsync_info = \n");
}

TEST_CASE("dest_matches_mirror accepts exactly what the mirror would write", "[PresetMirror]")
{
    const std::string raw = R"({"name":"A","nozzle_temperature":["230","nil"]})";
    std::string       sanitized;
    REQUIRE(sanitize_nil_arrays(raw, {}, &sanitized));
    const std::string src_info  = "user_id = 1\nsync_info = upload\n";
    const std::string dest_info = info_inert(src_info);

    SourceFile f = sf("filament/A.json", 10);
    f.body       = raw;
    f.info       = src_info;

    DestState dst;
    dst.present["filament/A.json"]    = true;
    dst.bytes["filament/A.json"]      = sanitized;
    dst.info_bytes["filament/A.json"] = dest_info;
    CHECK(dest_matches_mirror(f, dst));

    // Raw unparseable is not what this run would write (S5).
    dst.bytes["filament/A.json"] = raw;
    CHECK_FALSE(dest_matches_mirror(f, dst));

    dst.bytes["filament/A.json"] = R"({"name":"edited"})";
    CHECK_FALSE(dest_matches_mirror(f, dst));

    dst.bytes["filament/A.json"]      = sanitized;
    dst.info_bytes["filament/A.json"] = src_info;   // parseable sidecar, not inert
    CHECK_FALSE(dest_matches_mirror(f, dst));

    // S4: matching JSON + missing sidecar still matches; rewrite is separate.
    DestState no_info = dst;
    no_info.bytes["filament/A.json"] = sanitized;
    no_info.info_bytes.clear();
    CHECK(dest_matches_mirror(f, no_info));
    CHECK(dest_info_needs_rewrite(f, no_info));

    SourceFile base = sf("filament/base/A.json", 10);
    base.body       = R"({"name":"A"})";
    DestState base_dst;
    base_dst.bytes["filament/base/A.json"] = base.body;
    CHECK(dest_matches_mirror(base, base_dst));   // base/ writes no .info
    CHECK_FALSE(dest_info_needs_rewrite(base, base_dst));

    SourceFile empty;
    empty.rel  = "filament/A.json";
    empty.body = sanitized;
    DestState missing;
    missing.present["filament/A.json"] = true;
    CHECK_FALSE(dest_matches_mirror(empty, missing));   // no dest bytes
}

TEST_CASE("a crash after copies land but before persist is healed by adoption", "[PresetMirror]")
{
    // Simulated kill: A, B, C landed on disk; the batch persist never ran.
    // D and E were not copied. User.json is a byte-different untracked file.
    const std::string info  = "user_id = 1\nsync_info = upload\n";
    const std::string inert = info_inert(info);
    auto mk = [&](const std::string& rel, long long t, const std::string& body) {
        SourceFile f = sf(rel, t);
        f.body       = body;
        f.info       = info;
        return f;
    };

    SourceListing src;
    src.ok = true;
    src.files.push_back(mk("filament/A.json", 10, R"({"name":"A"})"));
    src.files.push_back(mk("filament/B.json", 10, R"({"name":"B"})"));
    src.files.push_back(mk("filament/C.json", 10, R"({"name":"C"})"));
    src.files.push_back(mk("filament/D.json", 10, R"({"name":"D"})"));
    src.files.push_back(mk("filament/E.json", 10, R"({"name":"E"})"));
    src.files.push_back(mk("filament/User.json", 10, R"({"name":"User"})"));

    // Run 1 planned Copy of all five plus User. Only A,B,C landed; persist skipped.
    auto plan1 = build_plan(src, {}, DestState{});
    CHECK(action_for(plan1, "filament/A.json") == Action::Copy);
    CHECK(action_for(plan1, "filament/E.json") == Action::Copy);
    CHECK(apply_plan({}, plan1, {}).empty());   // kill: nothing recorded

    DestState after_kill;
    auto land = [&](const std::string& rel, const std::string& body) {
        after_kill.present[rel]    = true;
        after_kill.bytes[rel]      = body;
        after_kill.info_bytes[rel] = inert;
    };
    land("filament/A.json", R"({"name":"A"})");
    land("filament/B.json", R"({"name":"B"})");
    land("filament/C.json", R"({"name":"C"})");
    // User.json exists but is the user's own bytes — never adopt, never overwrite.
    after_kill.present["filament/User.json"]    = true;
    after_kill.bytes["filament/User.json"]      = R"({"name":"User-edited"})";
    after_kill.info_bytes["filament/User.json"] = inert;

    auto plan2 = build_plan(src, {}, after_kill);
    CHECK(action_for(plan2, "filament/A.json") == Action::Adopt);
    CHECK(action_for(plan2, "filament/B.json") == Action::Adopt);
    CHECK(action_for(plan2, "filament/C.json") == Action::Adopt);
    CHECK(action_for(plan2, "filament/D.json") == Action::Copy);
    CHECK(action_for(plan2, "filament/E.json") == Action::Copy);
    CHECK(action_for(plan2, "filament/User.json") == Action::ProtectNative);

    // Adopt is recorded even though those rels are not in `copied`.
    auto man = apply_plan({}, plan2, copies_of(plan2));
    CHECK(man.count("filament/A.json"));
    CHECK(man.count("filament/B.json"));
    CHECK(man.count("filament/C.json"));
    CHECK(man.count("filament/D.json"));
    CHECK(man.count("filament/E.json"));
    CHECK_FALSE(man.count("filament/User.json"));
    CHECK(dump_manifest(man).find("\"deleted\"") != std::string::npos);   // format unchanged

    // Later source edit of A, B, C: they are ours, so they update. User stays protected.
    SourceListing edited = src;
    edited.files[0].t    = 20;
    edited.files[0].body = R"({"name":"A2"})";
    edited.files[1].t    = 20;
    edited.files[1].body = R"({"name":"B2"})";
    edited.files[2].t    = 20;
    edited.files[2].body = R"({"name":"C2"})";

    DestState dest2 = after_kill;
    dest2.present["filament/D.json"]    = true;
    dest2.bytes["filament/D.json"]      = R"({"name":"D"})";
    dest2.info_bytes["filament/D.json"] = inert;
    dest2.present["filament/E.json"]    = true;
    dest2.bytes["filament/E.json"]      = R"({"name":"E"})";
    dest2.info_bytes["filament/E.json"] = inert;

    auto plan3 = build_plan(edited, man, dest2);
    CHECK(action_for(plan3, "filament/A.json") == Action::Copy);
    CHECK(action_for(plan3, "filament/B.json") == Action::Copy);
    CHECK(action_for(plan3, "filament/C.json") == Action::Copy);
    CHECK(action_for(plan3, "filament/D.json") == Action::UpToDate);
    CHECK(action_for(plan3, "filament/E.json") == Action::UpToDate);
    CHECK(action_for(plan3, "filament/User.json") == Action::ProtectNative);
    CHECK_FALSE(apply_plan(man, plan3, copies_of(plan3)).count("filament/User.json"));
}

TEST_CASE("an empty manifest plus matching dest bytes is adopted, not ProtectNative", "[PresetMirror]")
{
    // Heals files stranded by main's old non-atomic persist (torn -> empty manifest).
    SourceFile f = sf("filament/A.json", 10);
    f.body       = R"({"name":"A"})";
    f.info       = "user_id = 1\n";
    SourceListing src;
    src.ok = true;
    src.files.push_back(f);

    DestState dst;
    dst.present["filament/A.json"]    = true;
    dst.bytes["filament/A.json"]      = f.body;
    dst.info_bytes["filament/A.json"] = info_inert(f.info);

    auto plan = build_plan(src, parse_manifest("{{{"), dst);
    CHECK(action_for(plan, "filament/A.json") == Action::Adopt);
    auto after = apply_plan({}, plan, {});
    REQUIRE(after.count("filament/A.json") == 1);
    CHECK(after["filament/A.json"].t == 10);
    CHECK_FALSE(after["filament/A.json"].deleted);
}

TEST_CASE("a kill between JSON and .info, or a torn .info, is adopted and rewritten", "[PresetMirror]")
{
    const std::string body  = R"({"name":"A"})";
    const std::string info  = "user_id = 1\nsync_info = upload\nsetting_id = x\n";
    const std::string inert = info_inert(info);

    SourceFile f = sf("filament/A.json", 10);
    f.body       = body;
    f.info       = info;
    SourceListing src;
    src.ok = true;
    src.files.push_back(f);

    DestState missing_info;
    missing_info.present["filament/A.json"] = true;
    missing_info.bytes["filament/A.json"]   = body;
    auto plan_missing                       = build_plan(src, {}, missing_info);
    CHECK(action_for(plan_missing, "filament/A.json") == Action::Adopt);
    CHECK(dest_info_needs_rewrite(f, missing_info));
    CHECK(apply_plan({}, plan_missing, {}).count("filament/A.json"));

    DestState torn;
    torn.present["filament/A.json"]    = true;
    torn.bytes["filament/A.json"]      = body;
    torn.info_bytes["filament/A.json"] = "user_id = 1\nsync_inf";
    CHECK_FALSE(info_is_parseable(torn.info_bytes["filament/A.json"]));
    auto plan_torn = build_plan(src, {}, torn);
    CHECK(action_for(plan_torn, "filament/A.json") == Action::Adopt);
    CHECK(dest_info_needs_rewrite(f, torn));

    // A complete user sidecar is someone else's file, even if the JSON matches.
    DestState user_info;
    user_info.present["filament/A.json"]    = true;
    user_info.bytes["filament/A.json"]      = body;
    user_info.info_bytes["filament/A.json"] = "user_id = 9\nsync_info = upload\nsetting_id = mine\n";
    CHECK(info_is_parseable(user_info.info_bytes["filament/A.json"]));
    CHECK(action_for(build_plan(src, {}, user_info), "filament/A.json") == Action::ProtectNative);

    // After adopt, a later source edit updates the file (cross-run).
    auto man = apply_plan({}, plan_missing, {});
    SourceListing edited = src;
    edited.files[0].t    = 20;
    edited.files[0].body = R"({"name":"A2"})";
    DestState dest2      = missing_info;
    CHECK(action_for(build_plan(edited, man, dest2), "filament/A.json") == Action::Copy);
}

TEST_CASE("a raw unparseable dest is not adopted; only the sanitized bytes are", "[PresetMirror]")
{
    const std::string raw = R"({"name":"A","nozzle_temperature":["230","nil"]})";
    std::string       sanitized;
    REQUIRE(sanitize_nil_arrays(raw, {}, &sanitized));
    REQUIRE_FALSE(preset_is_parseable(raw, {}));
    REQUIRE(preset_is_parseable(sanitized, {}));

    SourceFile f = sf("filament/A.json", 10, /*parseable*/ true);
    f.body       = raw;
    f.info       = "user_id = 1\n";
    SourceListing src;
    src.ok = true;
    src.files.push_back(f);

    DestState raw_dst;
    raw_dst.present["filament/A.json"]    = true;
    raw_dst.bytes["filament/A.json"]      = raw;
    raw_dst.info_bytes["filament/A.json"] = info_inert(f.info);
    CHECK_FALSE(dest_matches_mirror(f, raw_dst));
    CHECK(action_for(build_plan(src, {}, raw_dst), "filament/A.json") == Action::ProtectNative);

    DestState san_dst = raw_dst;
    san_dst.bytes["filament/A.json"] = sanitized;
    CHECK(dest_matches_mirror(f, san_dst));
    CHECK(action_for(build_plan(src, {}, san_dst), "filament/A.json") == Action::Adopt);

    SourceFile skipped = f;
    skipped.parseable  = false;
    src.files[0]       = skipped;
    CHECK_FALSE(dest_matches_mirror(skipped, san_dst));
    CHECK(action_for(build_plan(src, {}, san_dst), "filament/A.json") == Action::ProtectNative);
}

TEST_CASE("same-length dest JSON that is not byte-identical is not adopted", "[PresetMirror]")
{
    const std::string older = R"({"name":"AA"})";
    const std::string newer = R"({"name":"BB"})";
    REQUIRE(older.size() == newer.size());
    REQUIRE(older != newer);

    SourceFile f = sf("filament/A.json", 20);
    f.body       = newer;
    f.info       = "user_id = 1\n";
    SourceListing src;
    src.ok = true;
    src.files.push_back(f);

    DestState dst;
    dst.present["filament/A.json"]    = true;
    dst.bytes["filament/A.json"]      = older;
    dst.info_bytes["filament/A.json"] = info_inert(f.info);
    CHECK_FALSE(dest_matches_mirror(f, dst));
    CHECK(action_for(build_plan(src, {}, dst), "filament/A.json") == Action::ProtectNative);
    CHECK(apply_plan({}, build_plan(src, {}, dst), {}).empty());
}

TEST_CASE("a failed .info write is not recorded as a successful copy", "[PresetMirror]")
{
    CHECK(copy_ready_to_record(true, true, true));
    CHECK_FALSE(copy_ready_to_record(true, true, false));
    CHECK(copy_ready_to_record(true, false, false));
    CHECK_FALSE(copy_ready_to_record(false, true, true));

    const auto dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("info_%%%%%%%%");
    boost::filesystem::create_directories(dir);
    const auto path = (dir / "A.info").string();
    struct Cleanup
    {
        boost::filesystem::path p;
        ~Cleanup()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(p, ec);
        }
    } cleanup{dir};

    REQUIRE(write_info_inert(path, "user_id = 1\nsync_info = upload\n", nullptr));
    std::ifstream in0(path, std::ios::binary);
    const std::string first((std::istreambuf_iterator<char>(in0)), std::istreambuf_iterator<char>());
    CHECK(first == info_inert("user_id = 1\nsync_info = upload\n"));

    Slic3r::set_atomic_write_force_fail_hook([](const char*) { return true; });
    std::string err;
    REQUIRE_FALSE(write_info_inert(path, "user_id = 2\nsync_info = upload\n", &err));
    Slic3r::set_atomic_write_force_fail_hook(nullptr);
    REQUIRE_FALSE(err.empty());
    std::ifstream in1(path, std::ios::binary);
    const std::string still((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());
    CHECK(still == first);
    CHECK(still.find("user_id = 2") == std::string::npos);
}

TEST_CASE("copy_run_ends_at flushes the last Copy and a failed flush skips the rest", "[PresetMirror]")
{
    auto item = [](const std::string& rel, Action a) {
        PlanItem p;
        p.rel    = rel;
        p.action = a;
        p.t      = 10;
        return p;
    };
    const std::vector<PlanItem> plan{
        item("filament/A.json", Action::Copy),
        item("filament/B.json", Action::Copy),
        item("filament/C.json", Action::UpToDate),
        item("filament/D.json", Action::Copy),
        item("filament/E.json", Action::Copy),
    };

    CHECK_FALSE(copy_run_ends_at(plan, 0));
    CHECK(copy_run_ends_at(plan, 1));
    CHECK_FALSE(copy_run_ends_at(plan, 2));
    CHECK_FALSE(copy_run_ends_at(plan, 3));
    CHECK(copy_run_ends_at(plan, 4));
    CHECK_FALSE(copy_run_ends_at(plan, 99));

    CopyBatch batch;
    CHECK_FALSE(batch.should_flush(plan, 0));
    CHECK(batch.should_flush(plan, 1));
    // Flush of A+B failed: D and E must not be copied.
    batch.on_flush_failed();
    CHECK(batch.skip_remaining);
    CHECK_FALSE(batch.should_flush(plan, 4));
}
