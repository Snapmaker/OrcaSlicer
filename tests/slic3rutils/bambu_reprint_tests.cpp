// Reprinting an archived Bambu job from the phone (2026-09-26).
//
// The hub used to refuse every Bambu reprint ("not supported yet"). It now reads the job back from
// the archived .gcode.3mf and sends it the way the desktop's send dialog sends a plate. Covered here,
// without a printer:
//  - the job as the file holds it (plate number, model, nozzles, filaments, the filament -> nozzle
//    grouping, physical_extruder_map, bed type, the timelapse warning) - BambuReprint::load_job;
//  - the phone's per-filament override and its refusals - parse_choices / apply_choices;
//  - the dual-nozzle rule: a filament's tray must feed the nozzle it was sliced for, be of its type,
//    and exist - BambuReprint::check;
//  - the three mapping strings and nozzles_info exactly as SelectMachineDialog composes them, now
//    shared code (BambuSendMapping), and its per-side automatic mapping against a made-up H2D AMS;
//  - the job history a reprint leaves in the record's sidecar (GcodeArchive::note_reprint_in).
// The fake-printer gate (tests/phone/test_bambu_reprint_gate.py) covers the send itself.

#include <catch2/catch.hpp>

// The wx / Windows headers first, as every GUI source has them (GUI_App.hpp): a libslic3r header
// read before them leaves std::byte and the SDK's byte ambiguous.
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/BambuReprint.hpp"
#include "slic3r/GUI/BambuSendMapping.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GcodeArchive.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

std::string fixture(const char* name) { return (fs::path(TEST_DATA_DIR) / "bambu_reprint" / name).string(); }

// A tray as trays_of() reports it, made by hand: logical extruder 0 = left, 1 = right.
BambuReprint::Tray tray(int ams, int slot, int physical, int logical, const std::string& type, const std::string& colour, bool ready = true)
{
    BambuReprint::Tray t;
    t.ams_id            = ams;
    t.slot_id           = slot;
    t.tray_id           = ams * 4 + slot;
    t.physical_extruder = physical;
    t.extruder          = logical;
    t.name              = ams >= 128 ? "HT-" + std::string(1, char('A' + ams - 128)) : std::string(1, char('A' + ams)) + std::to_string(slot + 1);
    t.type              = type;
    t.display_type      = type;
    t.color             = colour;
    t.exists            = ready;
    t.ready             = ready;
    t.filament_id       = type == "PETG" ? "GFG00" : "GFA00";
    return t;
}

// The H2D of these tests: an AMS on the right (main, physical 0) extruder with white PLA, black PETG,
// red PLA and an empty slot, and an AMS HT on the left (physical 1) with white PLA.
std::vector<BambuReprint::Tray> h2d_trays()
{
    return { tray(0, 0, 0, 1, "PLA", "FFFFFFFF"), tray(0, 1, 0, 1, "PETG", "000000FF"), tray(0, 2, 0, 1, "PLA", "C12E1FFF"),
             tray(0, 3, 0, 1, "", "", false), tray(128, 0, 1, 0, "PLA", "FFFFFFFF") };
}

// The same printer as a MachineObject, for the shared automatic mapping.
struct FakeH2D
{
    MachineObject obj { nullptr, "fake-h2d", "FAKEH2D0001", "127.0.0.1" };
    FakeH2D()
    {
        obj.printer_type = "O1D";
        auto add = [this](const std::string& ams_id, int nozzle, std::vector<std::pair<std::string, std::string>> trays) {
            Ams* a = new Ams(ams_id, nozzle, 1);
            a->is_exists = true;
            for (size_t i = 0; i < trays.size(); ++i) {
                AmsTray* t = new AmsTray(std::to_string(i));
                t->type    = trays[i].first;
                t->color   = trays[i].second;
                t->is_exists = !trays[i].first.empty();
                t->setting_id = trays[i].first == "PETG" ? "GFG00" : "GFA00";
                a->trayList[t->id] = t;
            }
            obj.amsList[ams_id] = a;
        };
        add("0", 0, { { "PLA", "FFFFFFFF" }, { "PETG", "000000FF" }, { "PLA", "C12E1FFF" }, { "", "" } });
        add("128", 1, { { "PLA", "FFFFFFFF" } });
    }
};

const json* find_entry(const json& arr, const char* key, int value)
{
    for (const json& e : arr)
        if (e.value(key, -999) == value) return &e;
    return nullptr;
}

} // namespace

TEST_CASE("a one-nozzle job is read back from its .gcode.3mf", "[BambuReprint]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("x1c_two_colour.gcode.3mf"));
    REQUIRE(job.error.empty());
    CHECK(job.plate == 1);
    CHECK(job.printer_model_id == "BL-P001");
    CHECK(job.printer_model == "Bambu Lab X1 Carbon");
    CHECK_FALSE(job.dual());
    REQUIRE(job.nozzle_diameters.size() == 1);
    CHECK(job.nozzle_diameters[0] == Approx(0.4));
    CHECK(job.filament_map.empty()); // no grouping on a one-extruder job, so no nozzleId either
    CHECK(job.project_filament_count == 2);
    REQUIRE(job.filaments.size() == 2);
    CHECK(job.filaments[0].id == 0);
    CHECK(job.filaments[0].type == "PLA");
    CHECK(job.filaments[0].color == "#FFFFFFFF"); // the send dialog's #RRGGBBAA
    CHECK(job.filaments[0].extruder == -1);
    CHECK(job.filaments[1].color == "#000000FF");
    CHECK(job.bed_type == "textured_plate");
    CHECK(job.filament_change_sequence == std::vector<int>{ 0, 1, 0, 1 });
    CHECK_FALSE(job.timelapse_warning);

    const std::vector<FilamentInfo> infos = job.filament_infos();
    REQUIRE(infos.size() == 2);
    CHECK(infos[1].id == 1);
    CHECK(infos[1].tray_id == -1);
    CHECK(infos[1].filament_id == "GFA00");
}

TEST_CASE("a two-nozzle job keeps the nozzle every filament was sliced for", "[BambuReprint]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    REQUIRE(job.error.empty());
    CHECK(job.plate == 2); // plate 2 of 3: the payload's param is Metadata/plate_2.gcode
    CHECK(job.printer_model_id == "O1D");
    CHECK(job.dual());
    CHECK(job.physical_extruder_map == std::vector<int>{ 1, 0 });
    CHECK(job.filament_map == std::vector<int>{ 1, 2, 1, 2 });
    CHECK(job.nozzle_volume_types == std::vector<int>{ (int) nvtStandard, (int) nvtHighFlow });
    CHECK(job.project_filament_count == 4);
    CHECK(job.filament_ids == std::vector<std::string>{ "GFA00", "GFG00", "GFA00", "GFA00" });
    REQUIRE(job.filaments.size() == 3); // filament 3 is not printed on this plate
    CHECK(job.filaments[0].id == 0);
    CHECK(job.filaments[0].extruder == 0); // left
    CHECK(job.filaments[1].id == 1);
    CHECK(job.filaments[1].type == "PETG");
    CHECK(job.filaments[1].extruder == 1); // right
    CHECK(job.filaments[2].id == 3);
    CHECK(job.filaments[2].extruder == 1);
    CHECK(job.find(2) == nullptr);
    // (filament, nozzle) pairs for the nozzle-mapping request: group 0 is on extruder 0, group 1 on 1.
    REQUIRE(job.filament_nozzles.size() == 3);
    CHECK(job.filament_nozzles[0].filament == 0);
    CHECK(job.filament_nozzles[0].logical_extruder == 0);
    CHECK(job.filament_nozzles[1].filament == 1);
    CHECK(job.filament_nozzles[1].logical_extruder == 1);
    CHECK(job.filament_nozzles[1].volume == nvtHighFlow);
    CHECK(job.filament_change_sequence == std::vector<int>{ 0, 1, 3, 0 });
}

TEST_CASE("a slice that cannot record a timelapse says so", "[BambuReprint]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("h2c_rack.gcode.3mf"));
    REQUIRE(job.error.empty());
    CHECK(job.printer_model_id == "O1C2");
    CHECK(job.timelapse_warning);
    REQUIRE(job.filaments.size() == 3);
    CHECK(job.filaments[2].extruder == 1);
}

TEST_CASE("a file that is not a sliced job is refused in words", "[BambuReprint]")
{
    CHECK_FALSE(BambuReprint::load_job(fixture("no_such_file.gcode.3mf")).error.empty());
    const fs::path junk = fs::temp_directory_path() / fs::unique_path("reprint_junk_%%%%%%.gcode.3mf");
    {
        boost::nowide::ofstream f(junk.string().c_str(), std::ios::binary);
        f << "not a zip";
    }
    CHECK_FALSE(BambuReprint::load_job(junk.string()).error.empty());
    fs::remove(junk);
}

TEST_CASE("the phone's slot choices are parsed strictly", "[BambuReprint]")
{
    std::vector<BambuReprint::Choice> c;
    std::string                       error;
    REQUIRE(BambuReprint::parse_choices("0:128-0, 1:0-1,3:0-2", c, error));
    REQUIRE(c.size() == 3);
    CHECK(c[0].filament == 0);
    CHECK(c[0].ams_id == 128);
    CHECK(c[0].slot_id == 0);
    CHECK(c[2].filament == 3);
    CHECK(BambuReprint::parse_choices("", c, error));
    CHECK(c.empty());
    CHECK_FALSE(BambuReprint::parse_choices("0:1", c, error));       // no slot
    CHECK_FALSE(BambuReprint::parse_choices("0:a-1", c, error));     // not a number
    CHECK_FALSE(BambuReprint::parse_choices("0:0-1,0:0-2", c, error)); // one filament twice
    CHECK_FALSE(BambuReprint::parse_choices("0:0-99", c, error));    // no such slot
    CHECK_FALSE(error.empty());
}

TEST_CASE("choices land on the job's filaments and name only slots that hold something", "[BambuReprint]")
{
    const BambuReprint::Job job   = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    const auto              trays = h2d_trays();
    std::vector<FilamentInfo> result = job.filament_infos();
    std::string               error;
    std::vector<BambuReprint::Choice> c;
    REQUIRE(BambuReprint::parse_choices("0:128-0,1:0-1,3:0-2", c, error));
    REQUIRE(BambuReprint::apply_choices(job, trays, c, result, error));
    REQUIRE(result.size() == 3);
    CHECK(result[0].tray_id == 512); // the AMS HT: 128 * 4 + 0, ams_filament_mapping's numbering
    CHECK(result[0].ams_id == "128");
    CHECK(result[0].slot_id == "0");
    CHECK(result[0].color == "#FFFFFFFF");
    CHECK(result[1].tray_id == 1);
    CHECK(result[1].type == "PETG");
    CHECK(result[2].tray_id == 2);
    CHECK(BambuReprint::choices_text(result) == "0:128-0,1:0-1,3:0-2");
    CHECK(BambuReprint::check(job, trays, result).empty());

    SECTION("an empty slot")
    {
        REQUIRE(BambuReprint::parse_choices("1:0-3", c, error));
        CHECK_FALSE(BambuReprint::apply_choices(job, trays, c, result, error));
        CHECK(error.find("empty") != std::string::npos);
    }
    SECTION("a filament the plate does not print with")
    {
        REQUIRE(BambuReprint::parse_choices("2:0-0", c, error));
        CHECK_FALSE(BambuReprint::apply_choices(job, trays, c, result, error));
    }
    SECTION("a slot the printer does not have")
    {
        REQUIRE(BambuReprint::parse_choices("1:3-0", c, error));
        CHECK_FALSE(BambuReprint::apply_choices(job, trays, c, result, error));
    }
}

TEST_CASE("a two-nozzle job only prints from slots on the nozzle it was sliced for", "[BambuReprint]")
{
    const BambuReprint::Job job   = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    const auto              trays = h2d_trays();
    std::string             error;
    std::vector<BambuReprint::Choice> c;

    SECTION("the left filament on a right-side slot")
    {
        std::vector<FilamentInfo> result = job.filament_infos();
        REQUIRE(BambuReprint::parse_choices("0:0-0,1:0-1,3:0-2", c, error));
        REQUIRE(BambuReprint::apply_choices(job, trays, c, result, error));
        const auto problems = BambuReprint::check(job, trays, result);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].filament == 0);
        CHECK(problems[0].code == "side");
        CHECK(problems[0].text.find("left") != std::string::npos);
    }
    SECTION("a slot of another type")
    {
        std::vector<FilamentInfo> result = job.filament_infos();
        REQUIRE(BambuReprint::parse_choices("0:128-0,1:0-0,3:0-2", c, error));
        REQUIRE(BambuReprint::apply_choices(job, trays, c, result, error));
        const auto problems = BambuReprint::check(job, trays, result);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].filament == 1);
        CHECK(problems[0].code == "type");
    }
    SECTION("a filament with no slot at all")
    {
        std::vector<FilamentInfo> result = job.filament_infos(); // nothing mapped
        const auto problems = BambuReprint::check(job, trays, result);
        REQUIRE(problems.size() == 3);
        for (const auto& p : problems) CHECK(p.code == "unmapped");
    }
    SECTION("no slot of that type on that side")
    {
        // Only the left AMS HT is loaded with PETG missing on the right: the refusal names the side.
        std::vector<BambuReprint::Tray> left_only = { tray(128, 0, 1, 0, "PLA", "FFFFFFFF") };
        std::vector<FilamentInfo>       result    = job.filament_infos();
        const auto problems = BambuReprint::check(job, left_only, result);
        bool named = false;
        for (const auto& p : problems)
            if (p.filament == 1) named = p.text.find("no PETG is loaded in an AMS on the right side") != std::string::npos;
        CHECK(named);
    }
}

TEST_CASE("the automatic mapping matches each side against its own AMS", "[BambuReprint][BambuSendMapping]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    FakeH2D                 h2d;
    std::vector<FilamentInfo> result;
    const int rc = BambuSendMapping::auto_map(&h2d.obj, job.filament_infos(), job.filament_map, job.physical_extruder_map, result);
    CHECK(rc == 0);
    REQUIRE(result.size() == 3);
    // White PLA is on both sides; filament 1 is sliced for the left, so only the AMS HT may feed it.
    CHECK(result[0].id == 0);
    CHECK(result[0].ams_id == "128");
    CHECK(result[1].id == 1);
    CHECK(result[1].ams_id == "0");
    CHECK(result[1].slot_id == "1");
    CHECK(result[2].id == 3);
    CHECK(result[2].ams_id == "0");
    CHECK(result[2].slot_id == "2");
    CHECK(BambuSendMapping::wrong_extruder(&h2d.obj, result, job.filament_map, job.physical_extruder_map).empty());

    // The same filaments matched against every AMS (a one-extruder job's way) may pick the right
    // side's white PLA for filament 1: that is exactly what the per-side match prevents.
    std::vector<FilamentInfo> swapped = result;
    swapped[0].ams_id  = "0";
    swapped[0].slot_id = "0";
    swapped[0].tray_id = 0;
    CHECK(BambuSendMapping::wrong_extruder(&h2d.obj, swapped, job.filament_map, job.physical_extruder_map) == std::vector<int>{ 0 });
}

TEST_CASE("the mapping strings are the send dialog's", "[BambuSendMapping]")
{
    const BambuReprint::Job job   = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    std::vector<FilamentInfo> result = job.filament_infos();
    std::string               error;
    std::vector<BambuReprint::Choice> c;
    REQUIRE(BambuReprint::parse_choices("0:128-0,1:0-1,3:0-2", c, error));
    REQUIRE(BambuReprint::apply_choices(job, h2d_trays(), c, result, error));

    BambuSendMapping::ComposeInput in;
    in.project_filament_count = job.project_filament_count;
    in.filament_ids           = job.filament_ids;
    in.nozzle_filament_map    = job.filament_map;
    std::string v0, v1, info;
    REQUIRE(BambuSendMapping::compose(result, job.filament_infos(), in, v0, v1, info));
    // One entry per project filament, the unused one included.
    CHECK(json::parse(v0) == json::parse("[512, 1, -1, 2]"));
    const json j1 = json::parse(v1);
    REQUIRE(j1.size() == 4);
    CHECK(j1[0] == json({ { "ams_id", 128 }, { "slot_id", 0 } }));
    CHECK(j1[2] == json({ { "ams_id", 255 }, { "slot_id", 255 } }));
    const json ji = json::parse(info);
    REQUIRE(ji.size() == 4);
    CHECK(ji[0]["nozzleId"] == 1);      // left, in the task's numbering (1 = left, 0 = right)
    CHECK(ji[1]["nozzleId"] == 0);      // right
    CHECK(ji[1]["filamentType"] == "PETG");
    CHECK(ji[1]["filamentId"] == "GFG00");
    CHECK(ji[1]["sourceColor"] == "#000000FF");
    CHECK(ji[1]["targetColor"] == "#000000FF");
    CHECK(ji[2]["ams"] == -1);          // not printed: no tray, no nozzle
    CHECK_FALSE(ji[2].contains("nozzleId"));

    SECTION("a one-nozzle job carries no nozzleId")
    {
        in.nozzle_filament_map.clear();
        REQUIRE(BambuSendMapping::compose(result, job.filament_infos(), in, v0, v1, info));
        for (const json& e : json::parse(info)) CHECK_FALSE(e.contains("nozzleId"));
    }
    SECTION("nothing mapped leaves the strings alone")
    {
        std::string a = "keep", b = "keep", d = "keep";
        CHECK_FALSE(BambuSendMapping::compose(job.filament_infos(), job.filament_infos(), in, a, b, d));
        CHECK(a == "keep");
    }
}

TEST_CASE("nozzles_info names both nozzles of a two-nozzle printer only", "[BambuSendMapping]")
{
    const json two = json::parse(BambuSendMapping::nozzles_info({ 0.4, 0.4 }, { (int) nvtStandard, (int) nvtHighFlow }));
    REQUIRE(two.size() == 2);
    const json* left  = find_entry(two, "id", 1);
    const json* right = find_entry(two, "id", 0);
    REQUIRE(left);
    REQUIRE(right);
    CHECK((*left)["flowSize"] == "standard_flow");
    CHECK((*right)["flowSize"] == "high_flow");
    CHECK((*left)["diameter"] == Approx(0.4));
    CHECK((*left)["type"].is_null());
    CHECK(BambuSendMapping::nozzles_info({ 0.4 }, {}) == "[]");
}

TEST_CASE("a reprint is added to the record's history, newest last and bounded", "[BambuReprint]")
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("reprint_history_%%%%%%");
    fs::create_directories(root);
    {
        boost::nowide::ofstream f((root / "rec1.json").string().c_str(), std::ios::binary);
        f << json({ { "id", "rec1" }, { "file", "rec1.gcode.3mf" }, { "mode", "upload" } }).dump(2);
    }
    for (int i = 0; i < (int) GcodeArchive::REPRINT_HISTORY_MAX + 5; ++i)
        REQUIRE(GcodeArchive::note_reprint_in(root.string(), "rec1", { { "time", i }, { "mode", "print" } }));
    CHECK_FALSE(GcodeArchive::note_reprint_in(root.string(), "nope", { { "time", 1 } }));
    CHECK_FALSE(GcodeArchive::note_reprint_in(root.string(), "../rec1", { { "time", 1 } }));
    std::stringstream ss;
    ss << boost::nowide::ifstream((root / "rec1.json").string().c_str()).rdbuf();
    const json j = json::parse(ss.str());
    REQUIRE(j["reprints"].size() == GcodeArchive::REPRINT_HISTORY_MAX);
    CHECK(j["reprints"].back()["time"] == (int) GcodeArchive::REPRINT_HISTORY_MAX + 4);
    CHECK(j["file"] == "rec1.gcode.3mf"); // the rest of the sidecar is kept
    fs::remove_all(root);
}

// ------------------------------------------------------------- evaluate(): checks and defaults ----

namespace {

// An idle H2D the PC reaches over the LAN, with the trays of h2d_trays() and 0.4 nozzles.
BambuReprint::Printer idle_h2d()
{
    BambuReprint::Printer pr;
    pr.id                = "FAKEH2D0001";
    pr.name              = "Workshop H2D";
    pr.model             = "O1D";
    pr.online            = true;
    pr.lan_mode          = true;
    pr.access_code_known = true;
    pr.lan_route         = true;
    pr.extruder_count    = 2;
    pr.has_ams           = true;
    pr.sdcard            = 1; // HAS_SDCARD_NORMAL
    pr.diameters         = { { 0.4 }, { 0.4 } };
    pr.nozzle_hrc        = { 0, 0 };
    pr.trays             = h2d_trays();
    return pr;
}

// The send dialog's defaults with nothing remembered: every checkbox on.
BambuReprint::Env all_on()
{
    BambuReprint::Env env;
    env.remembered   = [](const char*) { return true; };
    env.required_hrc = [](const std::string&) { return 0; };
    return env;
}

// The automatic mapping the dialog would make on that printer: each side from its own AMS.
std::vector<FilamentInfo> auto_h2d(const BambuReprint::Job& job)
{
    std::vector<FilamentInfo>         result = job.filament_infos();
    std::string                       error;
    std::vector<BambuReprint::Choice> c;
    BambuReprint::parse_choices("0:128-0,1:0-1,3:0-2", c, error);
    BambuReprint::apply_choices(job, h2d_trays(), c, result, error);
    return result;
}

} // namespace

TEST_CASE("a Bambu reprint is refused in words when the printer cannot take it", "[BambuReprint]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    BambuReprint::Request   req;
    BambuReprint::Printer   pr = idle_h2d();
    auto refusal = [&](int status) {
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job));
        CHECK(ev.status == status);
        return ev.error;
    };
    REQUIRE(BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job)).status == 200);

    SECTION("offline") { pr.online = false; CHECK(refusal(409).find("offline") != std::string::npos); }
    SECTION("busy printing")
    {
        pr.printing      = true;
        pr.printing_what = "benchy";
        const std::string why = refusal(409);
        CHECK(why.find("busy printing") != std::string::npos);
        CHECK(why.find("benchy") != std::string::npos);
    }
    SECTION("changing filament") { pr.filament_changing = true; CHECK(refusal(409).find("busy") != std::string::npos); }
    SECTION("updating firmware") { pr.upgrading = true; CHECK(refusal(409).find("firmware") != std::string::npos); }
    SECTION("a LAN-only printer without its access code")
    {
        pr.access_code_known = false;
        CHECK(refusal(409).find("access code") != std::string::npos);
    }
    SECTION("a cloud printer the plug-in cannot reach over the LAN")
    {
        pr.lan_mode  = false;
        pr.lan_route = false;
        CHECK(refusal(409).find("cloud") != std::string::npos);
    }
    SECTION("another model")
    {
        pr.model          = "BL-P001";
        pr.extruder_count = 1;
        CHECK(refusal(409).find("sliced for") != std::string::npos);
    }
    SECTION("another nozzle diameter")
    {
        pr.diameters = { { 0.6 }, { 0.4 } };
        CHECK(refusal(409).find("0.4 mm nozzle on the left extruder") != std::string::npos);
        req.force = true;
        CHECK(BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job)).status == 200);
    }
    SECTION("an H2C rack holds the diameter: the right extruder has it")
    {
        pr.diameters = { { 0.4 }, { 0.6, 0.4 } };
        CHECK(BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job)).status == 200);
    }
    SECTION("a soft nozzle for a filament that needs a hard one")
    {
        pr.nozzle_hrc          = { 20, 20 };
        BambuReprint::Env env  = all_on();
        env.required_hrc       = [](const std::string& type) { return type == "PETG" ? 40 : 0; };
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, env, auto_h2d(job));
        CHECK(ev.status == 409);
        CHECK(ev.error.find("right nozzle") != std::string::npos);
    }
    SECTION("no SD card") { pr.sdcard = 0; CHECK(refusal(409).find("SD card") != std::string::npos); }
    SECTION("a read-only SD card") { pr.sdcard = 3; CHECK(refusal(409).find("read-only") != std::string::npos); }
    SECTION("a slot choice that makes no sense")
    {
        req.mapping = "1:0-3"; // the empty slot
        CHECK(refusal(400).find("empty") != std::string::npos);
    }
}

TEST_CASE("a Bambu reprint's options default to the send dialog's", "[BambuReprint]")
{
    const BambuReprint::Job job = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    BambuReprint::Request   req;
    BambuReprint::Printer   pr = idle_h2d();

    SECTION("nothing remembered: all on, nozzle offset calibration on auto")
    {
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job));
        CHECK(ev.bed_leveling);
        CHECK(ev.flow_cali);
        CHECK(ev.timelapse);
        CHECK(ev.use_ams);
        CHECK(ev.auto_offset_cali == 2);
        CHECK(ev.preview["options"]["nozzle_offset_cali"]["shown"] == true);
        CHECK(ev.preview["options"]["use_ams"]["shown"] == true);
    }
    SECTION("what the dialog remembers as off stays off")
    {
        BambuReprint::Env env = all_on();
        env.remembered = [](const char* key) { return std::string(key) != "timelapse" && std::string(key) != "flow_cali"; };
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, env, auto_h2d(job));
        CHECK(ev.bed_leveling);
        CHECK_FALSE(ev.flow_cali);
        CHECK_FALSE(ev.timelapse);
    }
    SECTION("the phone's choice wins over the remembered one")
    {
        req.bed_leveling       = 0;
        req.timelapse          = 0;
        req.nozzle_offset_cali = 0;
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job));
        CHECK_FALSE(ev.bed_leveling);
        CHECK_FALSE(ev.timelapse);
        CHECK(ev.auto_offset_cali == 0);
    }
    SECTION("an A1 starts with timelapse off")
    {
        pr.i3 = true;
        CHECK_FALSE(BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job)).timelapse);
    }
    SECTION("a printer without an AMS gets no mapping, and the first filament's info")
    {
        pr.has_ams = false;
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), {});
        CHECK_FALSE(ev.use_ams);
        CHECK(ev.ams_mapping.empty());
        CHECK(json::parse(ev.ams_mapping_info) == json::parse(R"([{"sourceColor":"FFFFFFFF","filamentType":"PLA"}])"));
        CHECK(ev.preview["options"]["use_ams"]["shown"] == false);
        CHECK(ev.can_send);
    }
    SECTION("a slice without a timelapse cannot turn it on")
    {
        const BambuReprint::Job h2c = BambuReprint::load_job(fixture("h2c_rack.gcode.3mf"));
        pr.model      = "O1C2";
        req.timelapse = 1;
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, h2c, req, all_on(), {});
        REQUIRE(ev.status == 200);
        CHECK_FALSE(ev.timelapse);
        CHECK(ev.preview["options"]["timelapse"]["enabled"] == false);
    }
    SECTION("a one-nozzle job has no nozzle offset calibration and no nozzles_info")
    {
        const BambuReprint::Job x1c = BambuReprint::load_job(fixture("x1c_two_colour.gcode.3mf"));
        BambuReprint::Printer   one = idle_h2d();
        one.model          = "BL-P001";
        one.extruder_count = 1;
        one.diameters      = { { 0.4 } };
        one.nozzle_hrc     = { 0 };
        one.trays          = { tray(0, 0, 0, -1, "PLA", "FFFFFFFF"), tray(0, 1, 0, -1, "PLA", "000000FF") };
        std::vector<FilamentInfo>         automatic = x1c.filament_infos();
        std::string                       error;
        std::vector<BambuReprint::Choice> c;
        REQUIRE(BambuReprint::parse_choices("0:0-0,1:0-1", c, error));
        REQUIRE(BambuReprint::apply_choices(x1c, one.trays, c, automatic, error));
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(one, x1c, req, all_on(), automatic);
        REQUIRE(ev.status == 200);
        CHECK(ev.auto_offset_cali == 0);
        CHECK(ev.nozzles_info == "[]");
        CHECK(ev.nozzle_mapping_request.empty());
        CHECK(ev.preview["options"]["nozzle_offset_cali"]["shown"] == false);
        CHECK(json::parse(ev.ams_mapping) == json::parse("[0, 1]"));
        for (const json& e : json::parse(ev.ams_mapping_info)) CHECK_FALSE(e.contains("nozzleId"));
    }
}

TEST_CASE("the mapping preview proposes, flags and accepts overrides", "[BambuReprint]")
{
    const BambuReprint::Job     job = BambuReprint::load_job(fixture("h2d_two_sided.gcode.3mf"));
    BambuReprint::Request       req;
    const BambuReprint::Printer pr = idle_h2d();

    SECTION("the automatic proposal")
    {
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job));
        REQUIRE(ev.can_send);
        const json& p = ev.preview;
        CHECK(p["mapping"] == "0:128-0,1:0-1,3:0-2");
        REQUIRE(p["filaments"].size() == 3);
        CHECK(p["filaments"][0]["side"] == "L");
        CHECK(p["filaments"][0]["tray"] == "128-0");
        CHECK(p["filaments"][0]["auto"] == true);
        CHECK(p["filaments"][0]["colour"] == "#FFFFFF");
        CHECK(p["filaments"][1]["side"] == "R");
        REQUIRE(p["trays"].size() == 5);
        CHECK(p["trays"][4]["name"] == "HT-A");
        CHECK(p["trays"][4]["side"] == "L");
        CHECK(p["trays"][3]["ready"] == false);
        CHECK(p["job"]["plate"] == 2);
        CHECK(p["printer"]["dual"] == true);
        CHECK(json::parse(ev.ams_mapping) == json::parse("[512, 1, -1, 2]"));
        CHECK(json::parse(ev.nozzles_info).size() == 2);
        // An H2D has no nozzle rack: no get_auto_nozzle_mapping handshake, as the desktop.
        CHECK(ev.nozzle_mapping_request.empty());
    }
    SECTION("nothing loaded for a filament: shown, not sent")
    {
        std::vector<FilamentInfo> automatic = auto_h2d(job);
        automatic[1].tray_id = -1; // the PETG found no slot
        automatic[1].ams_id.clear();
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), automatic);
        CHECK(ev.status == 200);
        CHECK_FALSE(ev.can_send);
        CHECK(ev.preview["can_send"] == false);
        REQUIRE(ev.preview["problems"].size() == 1);
        CHECK(ev.preview["problems"][0]["code"] == "unmapped");
        CHECK(ev.preview["filaments"][1]["tray"].is_null());
        CHECK(ev.ams_mapping.empty()); // nothing composed for a job that cannot print as sliced
    }
    SECTION("the phone moves a filament to the wrong side")
    {
        req.mapping = "0:0-0"; // white PLA, but on the right
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), auto_h2d(job));
        CHECK_FALSE(ev.can_send);
        REQUIRE(ev.preview["problems"].size() == 1);
        CHECK(ev.preview["problems"][0]["code"] == "side");
        CHECK(ev.preview["filaments"][0]["auto"] == false);
    }
    SECTION("the phone's override on the right side is taken")
    {
        std::vector<FilamentInfo> automatic = auto_h2d(job);
        automatic[2].tray_id = -1; // no red found automatically
        automatic[2].ams_id.clear();
        req.mapping = "3:0-0"; // the person accepts white PLA for the red part
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(pr, job, req, all_on(), automatic);
        CHECK(ev.can_send);
        CHECK(json::parse(ev.ams_mapping) == json::parse("[512, 1, -1, 0]"));
        CHECK(ev.preview["mapping"] == "0:128-0,1:0-1,3:0-0");
    }
    SECTION("an H2C with a rack asks the printer for its nozzle mapping")
    {
        const BambuReprint::Job h2c  = BambuReprint::load_job(fixture("h2c_rack.gcode.3mf"));
        BambuReprint::Printer   rack = pr;
        rack.model       = "O1C2";
        rack.nozzle_rack = true;
        rack.trays       = { tray(0, 0, 0, 1, "PLA", "042F56FF"), tray(0, 1, 0, 1, "PLA", "5D989EFF"), tray(128, 0, 1, 0, "PLA", "F7D959FF") };
        std::vector<FilamentInfo>         automatic = h2c.filament_infos();
        std::string                       error;
        std::vector<BambuReprint::Choice> c;
        REQUIRE(BambuReprint::parse_choices("0:128-0,1:0-0,2:0-1", c, error));
        REQUIRE(BambuReprint::apply_choices(h2c, rack.trays, c, automatic, error));
        const BambuReprint::Evaluation ev = BambuReprint::evaluate(rack, h2c, req, all_on(), automatic);
        REQUIRE(ev.can_send);
        REQUIRE_FALSE(ev.nozzle_mapping_request.empty());
        const json r = json::parse(ev.nozzle_mapping_request);
        CHECK(r["print"]["command"] == "get_auto_nozzle_mapping");
        CHECK(ev.preview["nozzle_mapping"]["applies"] == true);
        // An upload is not a sliced send: no handshake.
        req.mode = "upload";
        CHECK(BambuReprint::evaluate(rack, h2c, req, all_on(), automatic).nozzle_mapping_request.empty());
    }
}
