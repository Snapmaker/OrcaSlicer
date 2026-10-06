// Per-plate print history (libslic3r/PlatePrintHistory.hpp): the list's rules (cap, ordering, what
// turns a plate "printed", when it counts as modified since its last send), its JSON, and its
// round trip through a project 3MF (one <metadata key="edgeslicer_print_history"> per plate).
#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PlatePrintHistory.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <string>

using namespace Slic3r;
using namespace Slic3r::PlateHistory;

namespace {

// "2026-10-05T14:03:11Z" style stamps that sort as text, one minute apart.
std::string stamp(int minute_of_day_offset)
{
    const int total = 8 * 60 + minute_of_day_offset;
    char      buf[32];
    std::snprintf(buf, sizeof buf, "2020-10-%02dT%02d:%02d:00Z", 1 + total / (24 * 60), (total / 60) % 24, total % 60);
    return buf;
}

Entry entry_at(int minutes, Action action = Action::Sent, const std::string &hash = "")
{
    Entry e;
    e.uid            = "id" + std::to_string(minutes);
    e.time_utc       = stamp(minutes);
    e.utc_offset_min = 120;
    e.printer_name   = "Office H2D";
    e.printer_model  = "Bambu Lab H2D";
    e.connection     = "bambu_lan";
    e.action         = action;
    e.file_name      = "plate_" + std::to_string(minutes) + ".gcode.3mf";
    e.input_hash     = hash;
    return e;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

Model cube_model(int objects = 1)
{
    Model model;
    for (int i = 0; i < objects; ++i) {
        ModelObject *obj = model.add_object();
        obj->name        = "cube" + std::to_string(i);
        obj->add_volume(make_cube(10. + i, 10., 10.))->name = "cube";
        obj->add_instance();
        obj->ensure_on_bed();
        obj->instances.front()->set_offset(Vec3d(20. * i, 0., obj->instances.front()->get_offset().z()));
    }
    return model;
}

// Stores `history_json` on plate 0 of a one-cube project, reads it back, and says what the file held.
struct Stored
{
    bool        ok { false };
    std::string history_json;       // what the loaded PlateData carried
    std::string dual_nozzle_confirm; // carried by the same plate-data copy (it used to be dropped)
    std::string model_settings;     // Metadata/model_settings.config as written
    std::string fingerprint_before; // plate_input_fingerprint of the source model
    std::string fingerprint_after;  // ... of the loaded model
};

Stored store_and_load(const std::string &history_json)
{
    Stored      out;
    Model       src_model = cube_model();
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    // A coEnums option from a static config class has no keys_map and the project writer serialises
    // it (the same quirk the other project round trips in test_3mf.cpp work around).
    for (const std::string &key : store_config.keys())
        if (const ConfigOption *opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
            store_config.erase(key);
            store_config.option(key, true);
        }
    store_config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000"}));

    PlateData *plate  = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    plate->print_history = history_json;
    plate->dual_nozzle_confirm = "{\"v\":1,\"ok\":true}";

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "plate_history_project.3mf").string();

    StoreParams params;
    params.path            = test_file.c_str();
    params.model           = &src_model;
    params.config          = &store_config;
    params.plate_data_list = {plate};
    params.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored      = store_bbs_3mf(params);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool                loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets,
                                                              &is_bbl_3mf, &file_version, nullptr,
                                                              LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                                  LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    out.ok = loaded && plate_data.size() == 1;
    if (out.ok) {
        out.history_json       = plate_data.front()->print_history;
        out.dual_nozzle_confirm = plate_data.front()->dual_nozzle_confirm;
        out.fingerprint_before = plate_input_fingerprint(src_model, {{0, 0}}, Vec3d::Zero());
        out.fingerprint_after  = plate_input_fingerprint(dst_model, {{0, 0}}, Vec3d::Zero());
    }
    out.model_settings = zip_entry(test_file, "Metadata/model_settings.config");
    release_PlateData_list(plate_data);
    delete plate;
    boost::filesystem::remove(test_file);
    return out;
}

} // namespace

TEST_CASE("A history survives serialise and deserialise with every field", "[PlateHistory]")
{
    History h;
    Entry   e = entry_at(5, Action::SentAndStarted, "0123456789abcdef");
    e.time_estimate_s = 5025;
    e.filament_g      = 12.5;
    h.add(e);
    h.add(entry_at(9, Action::Exported));
    h.add(entry_at(7, Action::UploadedOnly));

    const std::string text = h.serialize();
    REQUIRE_FALSE(text.empty());

    const History back = History::deserialize(text);
    REQUIRE(back == h);
    REQUIRE(back.size() == 3);
    // Oldest first inside, newest first for display.
    REQUIRE(back.entries().front().uid == "id5");
    REQUIRE(back.entries().back().uid == "id9");
    REQUIRE(back.newest_first().front().uid == "id9");
    REQUIRE(back.newest_first().back().uid == "id5");
    REQUIRE(back.entries().front().time_estimate_s == 5025);
    REQUIRE(back.entries().front().filament_g == Approx(12.5));
    REQUIRE(back.entries().front().action == Action::SentAndStarted);
}

TEST_CASE("Plate number, plate name and title round trip, and entries without them read blank", "[PlateHistory]")
{
    Entry e = entry_at(3, Action::UploadedOnly);
    e.plate_number = 2;
    e.plate_name   = "Lid \"A\"";
    e.title        = "Benchy remix";
    e.printer_name = "Garage U1";
    e.printer_model = "Snapmaker U1";
    History h;
    h.add(e);
    const History back = History::deserialize(h.serialize());
    REQUIRE(back == h);
    REQUIRE(back.entries().front().plate_number == 2);
    REQUIRE(back.entries().front().plate_name == "Lid \"A\"");
    REQUIRE(back.entries().front().title == "Benchy remix");
    REQUIRE(back.entries().front().printer_model == "Snapmaker U1");

    // Exactly what the first build of this feature wrote: no plate, plate name or title keys.
    const History old = History::deserialize(
        "{\"v\":1,\"entries\":[{\"id\":\"abc\",\"t\":\"2020-10-01T08:00:00Z\",\"off\":60,\"action\":\"uploaded\","
        "\"printer\":\"Garage U1\",\"model\":\"Snapmaker U1\",\"conn\":\"snapmaker_lan\",\"file\":\"p.gcode\"}]}");
    REQUIRE(old.size() == 1);
    REQUIRE(old.entries().front().plate_number == 0);
    REQUIRE(old.entries().front().plate_name.empty());
    REQUIRE(old.entries().front().title.empty());
    REQUIRE(old.entries().front().printer_model == "Snapmaker U1");
    REQUIRE(old.was_sent());
    // And a blank field is not written back.
    REQUIRE(old.serialize().find("\"plate\"") == std::string::npos);
    REQUIRE(old.serialize().find("\"title\"") == std::string::npos);
}

TEST_CASE("An exported G-code is an entry but never a send", "[PlateHistory]")
{
    // What PlateHistoryRecorder::record_export() adds for Export G-code / Export plate sliced file.
    Entry e;
    e.time_utc     = "2020-10-02T10:00:00Z";
    e.action       = Action::Exported;
    e.connection   = "file";
    e.file_name    = "plate_1.gcode";
    e.plate_number = 1;
    History h;
    h.add(e);
    REQUIRE(h.size() == 1);
    REQUIRE_FALSE(h.was_sent());
    REQUIRE(History::deserialize(h.serialize()) == h);
    REQUIRE(h.entries().front().action == Action::Exported);
}

TEST_CASE("An empty history writes nothing, and unreadable text reads as empty", "[PlateHistory]")
{
    REQUIRE(History().serialize().empty());
    REQUIRE(History::deserialize("").empty());
    REQUIRE(History::deserialize("not json at all").empty());
    REQUIRE(History::deserialize("[1,2,3]").empty());
    REQUIRE(History::deserialize("{\"entries\":42}").empty());
    // One bad entry does not take the good ones with it.
    const History partial = History::deserialize(
        "{\"v\":1,\"entries\":[7,{\"t\":\"garbage\"},{\"t\":\"2026-10-01T08:00:00Z\",\"action\":\"sent\",\"printer\":\"A\"},"
        "{\"t\":\"2026-10-01T09:00:00Z\",\"action\":\"from the future\",\"extra\":true}]}");
    REQUIRE(partial.size() == 2);
    REQUIRE(partial.entries().front().printer_name == "A");
    // An action this build does not know reads as a plain send.
    REQUIRE(partial.entries().back().action == Action::Sent);
}

TEST_CASE("The history keeps the newest 50 entries", "[PlateHistory]")
{
    History h;
    for (int i = 0; i < 60; ++i)
        h.add(entry_at(i));
    REQUIRE(h.size() == History::MAX_ENTRIES);
    REQUIRE(h.size() == 50);
    // The ten oldest were dropped.
    REQUIRE(h.entries().front().uid == "id10");
    REQUIRE(h.entries().back().uid == "id59");
    REQUIRE(h.newest_first().front().uid == "id59");

    // The cap holds through a round trip.
    const History back = History::deserialize(h.serialize());
    REQUIRE(back.size() == 50);
    REQUIRE(back == h);
}

TEST_CASE("Entries stay in time order whatever order they arrive in", "[PlateHistory]")
{
    History h;
    h.add(entry_at(30));
    h.add(entry_at(10));
    h.add(entry_at(20));
    REQUIRE(h.entries()[0].uid == "id10");
    REQUIRE(h.entries()[1].uid == "id20");
    REQUIRE(h.entries()[2].uid == "id30");

    // Same time: the one added later is the newer.
    Entry a = entry_at(40), b = entry_at(40);
    a.uid = "first";
    b.uid = "second";
    h.add(a);
    h.add(b);
    REQUIRE(h.newest_first().front().uid == "second");

    // An entry added without a time is stamped now, which sorts after the fixed 2020-10 stamps.
    Entry now;
    now.uid = "now";
    h.add(now);
    REQUIRE(h.entries().back().uid == "now");
    REQUIRE_FALSE(h.entries().back().time_utc.empty());
}

TEST_CASE("Only a send to a printer turns the plate printed; an export does not", "[PlateHistory]")
{
    History h;
    REQUIRE_FALSE(h.was_sent());

    h.add(entry_at(1, Action::Exported));
    REQUIRE_FALSE(h.empty());
    REQUIRE_FALSE(h.was_sent());
    REQUIRE(h.last_sent() == nullptr);

    for (Action a : {Action::Sent, Action::SentAndStarted, Action::UploadedOnly}) {
        History one;
        one.add(entry_at(2, a));
        INFO(action_key(a));
        REQUIRE(one.was_sent());
        REQUIRE(is_printer_action(a));
    }
    REQUIRE_FALSE(is_printer_action(Action::Exported));

    // Printed once, printed for good: a later export neither removes it nor becomes the last send.
    h.add(entry_at(3, Action::Sent));
    h.add(entry_at(4, Action::Exported));
    REQUIRE(h.was_sent());
    REQUIRE(h.last_sent()->uid == "id3");

    // Clearing starts over.
    h.clear();
    REQUIRE(h.empty());
    REQUIRE_FALSE(h.was_sent());
}

TEST_CASE("An upload that is then started is upgraded in place", "[PlateHistory]")
{
    History h;
    Entry   e = entry_at(1, Action::UploadedOnly);
    h.add(e);
    REQUIRE(h.set_action("id1", Action::SentAndStarted));
    REQUIRE(h.size() == 1);
    REQUIRE(h.entries().front().action == Action::SentAndStarted);
    REQUIRE_FALSE(h.set_action("missing", Action::Sent));
    REQUIRE_FALSE(h.set_action("", Action::Sent));
}

TEST_CASE("Modified since the last send compares the plate against its last send only", "[PlateHistory]")
{
    History h;
    // Never sent: nothing to be modified since.
    REQUIRE_FALSE(h.modified_since_last_send("aaaa"));
    h.add(entry_at(1, Action::Exported, "export-hash"));
    REQUIRE_FALSE(h.modified_since_last_send("different"));

    h.add(entry_at(2, Action::Sent, "aaaa"));
    REQUIRE_FALSE(h.modified_since_last_send("aaaa"));
    REQUIRE(h.modified_since_last_send("bbbb"));
    // Unknown on either side never claims a change.
    REQUIRE_FALSE(h.modified_since_last_send(""));
    History no_hash;
    no_hash.add(entry_at(3, Action::Sent, ""));
    REQUIRE_FALSE(no_hash.modified_since_last_send("bbbb"));

    // An export made after a change is not a send: the plate is still modified since the send.
    h.add(entry_at(3, Action::Exported, "bbbb"));
    REQUIRE(h.modified_since_last_send("bbbb"));
    // A new send of the changed plate resets it.
    h.add(entry_at(4, Action::UploadedOnly, "bbbb"));
    REQUIRE_FALSE(h.modified_since_last_send("bbbb"));
}

TEST_CASE("The plate fingerprint follows the plate's content, not its place", "[PlateHistory]")
{
    Model                            model = cube_model();
    const std::vector<std::pair<int, int>> oi{{0, 0}};
    const std::string                base = plate_input_fingerprint(model, oi, Vec3d::Zero());
    REQUIRE_FALSE(base.empty());
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) == base);

    // Moving the plate (a reordered plate sits at another origin) with its objects is no change.
    ModelInstance *inst = model.objects.front()->instances.front();
    const Vec3d    home = inst->get_offset();
    inst->set_offset(home + Vec3d(300., -50., 0.));
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d(300., -50., 0.)) == base);
    inst->set_offset(home);

    // Moving, scaling or rotating an object on the plate is.
    inst->set_offset(home + Vec3d(5., 0., 0.));
    const std::string moved = plate_input_fingerprint(model, oi, Vec3d::Zero());
    REQUIRE(moved != base);
    inst->set_offset(home);
    inst->set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) != base);
    inst->set_scaling_factor(Vec3d(1., 1., 1.));
    inst->set_rotation(Vec3d(0., 0., 0.5));
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) != base);
    inst->set_rotation(Vec3d(0., 0., 0.));
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) == base);

    // A per-object setting is.
    model.objects.front()->config.set("layer_height", 0.3);
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) != base);
    model.objects.front()->config.erase("layer_height");
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) == base);

    // A different or an extra object is; a rename is not.
    model.objects.front()->name = "renamed";
    REQUIRE(plate_input_fingerprint(model, oi, Vec3d::Zero()) == base);
    Model two = cube_model(2);
    REQUIRE(plate_input_fingerprint(two, {{0, 0}, {1, 0}}, Vec3d::Zero()) != plate_input_fingerprint(two, {{0, 0}}, Vec3d::Zero()));
    // The order of the objects does not matter.
    REQUIRE(plate_input_fingerprint(two, {{0, 0}, {1, 0}}, Vec3d::Zero()) == plate_input_fingerprint(two, {{1, 0}, {0, 0}}, Vec3d::Zero()));
    // An empty plate has no fingerprint, so it is never reported as modified.
    REQUIRE(plate_input_fingerprint(model, {}, Vec3d::Zero()).empty());
}

TEST_CASE("Entry times show in the local time they were recorded in", "[PlateHistory]")
{
    Entry e;
    e.time_utc       = "2026-10-05T14:03:11Z";
    e.utc_offset_min = 120;
    REQUIRE(format_local(e) == "2026-10-05 16:03");
    e.utc_offset_min = -300;
    REQUIRE(format_local(e) == "2026-10-05 09:03");
    // Across midnight and across a year boundary.
    e.time_utc       = "2026-01-01T00:30:00Z";
    e.utc_offset_min = -300;
    REQUIRE(format_local(e) == "2025-12-31 19:30");
    e.time_utc       = "2026-12-31T23:30:00Z";
    e.utc_offset_min = 90;
    REQUIRE(format_local(e) == "2027-01-01 01:00");
    e.time_utc = "nonsense";
    REQUIRE(format_local(e).empty());

    // The clock functions agree with each other.
    REQUIRE(now_utc_iso().size() == 20);
    REQUIRE(local_utc_offset_minutes() >= -14 * 60);
    REQUIRE(local_utc_offset_minutes() <= 14 * 60);
}

TEST_CASE("A plate's history round trips through a project 3MF", "[PlateHistory][3mf]")
{
    History h;
    Entry   e = entry_at(5, Action::SentAndStarted, "feedfacecafebeef");
    e.printer_name    = "Shop \"U1\" & <friends>";  // characters an XML attribute must escape
    e.time_estimate_s = 3600;
    e.filament_g      = 7.25;
    h.add(e);
    h.add(entry_at(8, Action::Exported));

    const Stored stored = store_and_load(h.serialize());
    REQUIRE(stored.ok);
    REQUIRE_FALSE(stored.history_json.empty());
    REQUIRE(stored.model_settings.find("edgeslicer_print_history") != std::string::npos);

    const History back = History::deserialize(stored.history_json);
    REQUIRE(back == h);
    REQUIRE(back.entries().front().printer_name == "Shop \"U1\" & <friends>");
    REQUIRE(back.was_sent());
    // The plate-data copy used to drop the dual-nozzle confirmation on load.
    REQUIRE(stored.dual_nozzle_confirm == "{\"v\":1,\"ok\":true}");

    // What the plate would slice from is the same after a save and reopen, so "modified since the
    // last send" does not fire just because the project was reopened.
    REQUIRE_FALSE(stored.fingerprint_before.empty());
    REQUIRE(stored.fingerprint_after == stored.fingerprint_before);
}

TEST_CASE("A project without a history, and a file from an older build, load with none", "[PlateHistory][3mf]")
{
    // Nothing is written for an empty history.
    const Stored none = store_and_load("");
    REQUIRE(none.ok);
    REQUIRE(none.history_json.empty());
    REQUIRE(none.model_settings.find("edgeslicer_print_history") == std::string::npos);
    REQUIRE(History::deserialize(none.history_json).empty());
    REQUIRE_FALSE(History::deserialize(none.history_json).was_sent());

    // An older build's file is exactly such a file: the plate element has no history key at all.
    // A newer file read by an older build is the other direction: an unknown <metadata> key is
    // skipped by the loader (the same path edgeslicer_dual_nozzle_confirm takes).
    REQUIRE(none.model_settings.find("<plate>") != std::string::npos);
}
