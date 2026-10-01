// The print-host device store: <datadir>/hub/print_host_devices.json.
//
// The store is wx-free on purpose, so it can be exercised here rather than through the dialog.
// Every case points it at its own temporary file (set_store_path) - nothing here reads or writes
// the user's data dir.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"
#include "slic3r/Utils/PrusaLinkStatus.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <ctime>
#include <string>

using namespace Slic3r;
using namespace Slic3r::PrintHostDevices;
namespace fs = boost::filesystem;

// One temporary store file, removed again at the end of the case.
struct ScopedStore
{
    fs::path path;
    explicit ScopedStore(const std::string& tag)
    {
        path = fs::temp_directory_path() / ("print_host_devices_test_" + tag + "_" + std::to_string((long long) ::time(nullptr)) + ".json");
        boost::system::error_code ec;
        fs::remove(path, ec);
        set_store_path(path.string());
    }
    ~ScopedStore()
    {
        set_store_path("");
        boost::system::error_code ec;
        fs::remove(path, ec);
    }
    void write(const std::string& body) const
    {
        boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        out << body;
    }
};

static Device make_device(const std::string& alias, const std::string& address, const std::string& host_type = "octoprint")
{
    Device d;
    d.alias         = alias;
    d.address       = address;
    d.host_type     = host_type;
    d.auth_type     = "key";
    d.apikey        = "k-" + alias;
    d.printer_model = "Elegoo Centauri Carbon";
    return d;
}

TEST_CASE("PrintHostDevices: a device survives save and load", "[PrintHostDevices]")
{
    ScopedStore store("roundtrip");
    const std::string key = "Elegoo Centauri Carbon";

    Device      d = make_device("Left bay", "192.168.1.41", "elegoolink");
    d.serial      = "SNADVA1234567";
    std::string error;
    REQUIRE(add(key, d, error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(d.id.empty());
    REQUIRE(d.created > 0);

    const std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == d.id);
    CHECK(back[0].alias == "Left bay");
    CHECK(back[0].address == "192.168.1.41");
    CHECK(back[0].host_type == "elegoolink");
    CHECK(back[0].auth_type == "key");
    CHECK(back[0].apikey == "k-Left bay");
    CHECK(back[0].serial == "SNADVA1234567");
    CHECK(back[0].printer_model == "Elegoo Centauri Carbon");
    CHECK(back[0].created == d.created);

    // The file really is the store: it is on disk, and it is JSON a person can read.
    REQUIRE(fs::exists(store.path));
    CHECK(fs::file_size(store.path) > 0);

    // An address already in this model's list is refused rather than silently doubled.
    Device dup = make_device("Left bay again", "192.168.1.41/");
    error.clear();
    CHECK_FALSE(add(key, dup, error));
    CHECK_FALSE(error.empty());
    CHECK(devices(key).size() == 1);
}

TEST_CASE("PrintHostDevices: an id is stable across edits and reloads", "[PrintHostDevices]")
{
    ScopedStore store("idstable");
    const std::string key = "Elegoo Centauri Carbon";

    Device      d = make_device("Left bay", "192.168.1.41");
    std::string error;
    REQUIRE(add(key, d, error));
    const std::string id = d.id;

    // Renaming and readdressing keeps the id, so anything holding it (an archived send, the phone's
    // device card) still points at the same printer.
    Device edited  = d;
    edited.alias   = "Garage";
    edited.address = "192.168.1.55";
    REQUIRE(update(key, edited, error));

    Device found;
    REQUIRE(find(key, id, found));
    CHECK(found.id == id);
    CHECK(found.alias == "Garage");
    CHECK(found.address == "192.168.1.55");
    CHECK(found.created == d.created); // created is not rewritten by an edit

    // And it is still that id after the store has been read from the file again.
    const std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == id);

    // last_used is a memory of where the last plate went, not a setting: it names the device and
    // stamps that device's own timestamp in one go.
    CHECK(last_used_id(key).empty()); // nothing was ever sent
    set_last_used(key, id);
    CHECK(last_used_id(key) == id);
    REQUIRE(find(key, id, found));
    CHECK(found.last_used > 0);

    REQUIRE(remove(key, id));
    CHECK(devices(key).empty());
    CHECK(last_used_id(key).empty()); // the memory went with the device
    CHECK_FALSE(remove(key, id));
}

TEST_CASE("PrintHostDevices: devices are grouped by printer model", "[PrintHostDevices]")
{
    ScopedStore store("grouping");
    const std::string elegoo = "Elegoo Centauri Carbon";
    const std::string voron  = "Voron 2.4";

    std::string error;
    Device      a = make_device("Left bay", "192.168.1.41");
    Device      b = make_device("Right bay", "192.168.1.42");
    Device      c = make_device("Voron", "192.168.1.41"); // the same address, another model: fine
    REQUIRE(add(elegoo, a, error));
    REQUIRE(add(elegoo, b, error));
    REQUIRE(add(voron, c, error));

    CHECK(devices(elegoo).size() == 2);
    CHECK(devices(voron).size() == 1);
    CHECK(devices("Elegoo Centauri").empty()); // a model nobody added to has no devices

    const auto all = all_devices();
    REQUIRE(all.size() == 2);
    CHECK(all.at(elegoo).size() == 2);
    CHECK(all.at(voron).size() == 1);

    // A device of one model is not visible under another.
    Device found;
    CHECK(find(elegoo, a.id, found));
    CHECK_FALSE(find(voron, a.id, found));

    // Every variant of one machine shares a list: the nozzle is in the preset name, not the model.
    CHECK(model_key("Elegoo Centauri Carbon", "Elegoo Centauri Carbon 0.4 nozzle") == elegoo);
    CHECK(model_key("Elegoo Centauri Carbon", "Elegoo Centauri Carbon 0.2 nozzle") == elegoo);
    // A preset with no printer_model at all falls back to its own name.
    CHECK(model_key("", "My homebrew printer") == "preset:My homebrew printer");
}

TEST_CASE("PrintHostDevices: a broken file reads as an empty list", "[PrintHostDevices]")
{
    const std::string key = "Elegoo Centauri Carbon";

    SECTION("not JSON at all") {
        ScopedStore store("malformed");
        store.write("{ this is not json ]]");
        CHECK(devices(key).empty());
        CHECK(all_devices().empty());
        CHECK(last_used_id(key).empty());
        // ... and writing to it repairs it rather than failing.
        Device      d = make_device("Left bay", "192.168.1.41");
        std::string error;
        REQUIRE(add(key, d, error));
        CHECK(devices(key).size() == 1);
    }

    SECTION("JSON, but the wrong shape") {
        ScopedStore store("wrongshape");
        store.write("[1, 2, 3]");
        CHECK(devices(key).empty());
    }

    SECTION("the right shape with rubbish in it") {
        ScopedStore store("rubbish");
        store.write(R"({"version":1,"models":{"Elegoo Centauri Carbon":{"devices":[
            {"id":"d1","alias":"Good","address":"192.168.1.41"},
            {"id":"d2","alias":"No address"},
            "not an object",
            {"id":"d3","address":"192.168.1.42","port":"nonsense","created":"yesterday"}
        ]}}})");
        const std::vector<Device> back = devices(key);
        REQUIRE(back.size() == 2); // the entry with no address and the string are dropped
        CHECK(back[0].alias == "Good");
        CHECK(back[0].host_type == "octoprint"); // the default for a field that is not there
        CHECK(back[1].address == "192.168.1.42");
        CHECK(back[1].created == 0); // "yesterday" is not a number, so no date rather than a wrong one
    }

    SECTION("no file at all") {
        ScopedStore store("missing");
        CHECK(devices(key).empty());
    }
}

TEST_CASE("PrintHostDevices: a preset's own address becomes device 1, once", "[PrintHostDevices]")
{
    ScopedStore store("migration");
    const std::string key = "Elegoo Centauri Carbon";

    PresetHost p;
    p.model_key     = key;
    p.preset_name   = "Elegoo Centauri Carbon 0.4 nozzle";
    p.address       = "192.168.1.41";
    p.host_type     = "elegoolink";
    p.auth_type     = "key";
    p.apikey        = "secret";
    p.printer_model = "Elegoo Centauri Carbon";

    PresetHost none;             // a preset with no print_host contributes nothing
    none.model_key   = "Voron 2.4";
    none.preset_name = "Voron";
    none.address     = "";

    CHECK(migrate_from_presets({ p, none }) == 1);
    std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].alias == "Elegoo Centauri Carbon 0.4 nozzle");
    CHECK(back[0].address == "192.168.1.41");
    CHECK(back[0].host_type == "elegoolink");
    CHECK(back[0].apikey == "secret");
    // An import is not a send: it does not make the imported device the model's last used one,
    // because this feature has no "main printer" for it to become.
    CHECK(last_used_id(key).empty());
    CHECK(devices("Voron 2.4").empty());

    // Idempotent: running it again changes nothing.
    const std::string id = back[0].id;
    CHECK(migrate_from_presets({ p, none }) == 0);
    back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == id);

    // Another variant of the same machine, pointing at the same printer, adds nothing either.
    PresetHost variant = p;
    variant.preset_name = "Elegoo Centauri Carbon 0.2 nozzle";
    variant.address     = "192.168.1.41/";
    CHECK(migrate_from_presets({ variant }) == 0);
    CHECK(devices(key).size() == 1);

    // A device somebody deleted afterwards stays deleted - the pair was already imported.
    REQUIRE(remove(key, id));
    CHECK(migrate_from_presets({ p }) == 0);
    CHECK(devices(key).empty());

    // A second, genuinely different address of the same model does arrive.
    PresetHost second = p;
    second.preset_name = "Elegoo Centauri Carbon - garage";
    second.address     = "192.168.1.55";
    CHECK(migrate_from_presets({ second }) == 1);
    REQUIRE(devices(key).size() == 1);
    CHECK(devices(key)[0].address == "192.168.1.55");
}

TEST_CASE("PrintHostDevices: a phase-1 store's \"current\" is read as the last used one", "[PrintHostDevices]")
{
    // Phase 1 wrote a model-level "current": the device whose address its "Use this device" button
    // had copied into the preset. That button is gone, but the field is the best guess at "the one
    // you last sent to", so it is still read - and replaced the next time a send happens.
    ScopedStore store("phase1current");
    const std::string key = "Elegoo Centauri Carbon";
    store.write("{\"version\":1,\"models\":{\"" + key +
                "\":{\"current\":\"dcafe\",\"devices\":[{\"id\":\"dcafe\",\"address\":\"192.168.1.41\"},"
                "{\"id\":\"dbeef\",\"address\":\"192.168.1.42\"}]}}}");
    CHECK(last_used_id(key) == "dcafe");

    set_last_used(key, "dbeef");
    CHECK(last_used_id(key) == "dbeef");
    Device found;
    REQUIRE(find(key, "dbeef", found));
    CHECK(found.last_used > 0);

    // config_for is what the send builds its job from: a copy, never the preset itself.
    DynamicPrintConfig preset;
    preset.opt_string("print_host", true) = "192.168.1.99";
    REQUIRE(find(key, "dbeef", found));
    const DynamicPrintConfig job = config_for(found, preset);
    CHECK(job.opt_string("print_host") == "192.168.1.42");
    CHECK(preset.opt_string("print_host") == "192.168.1.99"); // untouched
}

TEST_CASE("PrintHostDevices: the send button follows the devices, not the preset", "[PrintHostDevices]")
{
    ScopedStore store("cansend");
    const std::string key = "Elegoo Centauri Carbon";

    // The preset the send path reads: a printer model, and no address of its own. This is exactly
    // the case the devices feature exists for, and the Print button has to light up for it.
    DynamicPrintConfig config;
    config.opt_string("printer_model", true) = key;
    config.opt_string("print_host", true)    = "";

    CHECK_FALSE(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
    CHECK_FALSE(has_devices(key));

    // One device with an address is enough - nothing is written to the preset.
    Device      d = make_device("Left bay", "192.168.1.41", "elegoolink");
    std::string error;
    REQUIRE(add(key, d, error));
    CHECK(has_devices(key));
    CHECK(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
    CHECK(config.opt_string("print_host").empty()); // still empty: no bridge writes it any more

    // Every nozzle variant of the machine shares the list, so they all send.
    CHECK(can_send_for(config, "Elegoo Centauri Carbon 0.2 nozzle"));

    // A different model does not borrow it.
    DynamicPrintConfig other;
    other.opt_string("printer_model", true) = "Voron 2.4";
    other.opt_string("print_host", true)    = "";
    CHECK_FALSE(can_send_for(other, "Voron"));
    // ... unless the preset carries its own address, the way it always worked.
    other.opt_string("print_host") = "192.168.1.90";
    CHECK(can_send_for(other, "Voron"));

    // A device with no address is not a place to send to. (add() refuses one, so this goes in by
    // hand, the way a hand-edited file would.)
    REQUIRE(remove(key, d.id));
    CHECK_FALSE(has_devices(key));
    store.write("{\"version\":1,\"models\":{\"" + key + "\":{\"devices\":[{\"id\":\"x\",\"address\":\"\"}]}}}");
    CHECK_FALSE(has_devices(key));
    CHECK_FALSE(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
}

TEST_CASE("PrintHostDevices: what a device says it has loaded", "[PrintHostDevices]")
{
    // No network here: parse_moonraker_status is the half of probe() that turns a Moonraker
    // `result.status` object into slots, so the parsing is exercised without a printer.

    SECTION("a Snapmaker-flavoured Moonraker names its filaments") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"print_task_config\":{"
            "   \"filament_type\":[\"PLA\",\"PETG\",\"\"],"
            "   \"filament_sub_type\":[\"Basic\",\"HF\",\"\"],"
            "   \"filament_vendor\":[\"Polymaker\",\"Generic\",\"\"],"
            "   \"filament_color_rgba\":[\"FF0000FF\",\"0000FFFF\",\"\"],"
            "   \"filament_exist\":[true,true,false]}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.probed);
        CHECK(st.online);
        CHECK(st.state == "standby");
        REQUIRE(st.slots.size() == 3);
        CHECK(st.slots_have_filaments);
        CHECK(st.slots[0].index == 0);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[0].vendor == "Polymaker");
        CHECK(st.slots[0].color == "#FF0000"); // the alpha byte is dropped
        CHECK(st.slots[0].loaded);
        CHECK(st.slots[1].color == "#0000FF");
        CHECK_FALSE(st.slots[2].loaded);
        CHECK(st.note.empty()); // there is something to map, so nothing to apologise for
        CHECK(st.slots[0].label() == "T1 PLA Basic");
    }

    SECTION("a stock Klipper names its tools and nothing in them") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"printing\"},"
            " \"extruder\":{\"nozzle_diameter\":0.4,\"temperature\":210},"
            " \"extruder1\":{\"nozzle_diameter\":0.4,\"temperature\":25}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.online);
        CHECK(st.state == "printing");
        REQUIRE(st.slots.size() == 2);
        CHECK_FALSE(st.slots_have_filaments); // tools, not filaments: no mapping table
        CHECK(st.slots[1].index == 1);
        CHECK(st.slots[0].nozzle == Approx(0.4));
        CHECK_FALSE(st.note.empty());
    }

    SECTION("something that is not a Moonraker printer") {
        const Status st = parse_moonraker_status("{}", "octoprint");
        CHECK_FALSE(st.online);
        CHECK(st.slots.empty());
        CHECK_FALSE(st.note.empty());
    }

    SECTION("Elegoo Link is never asked, and says so") {
        // Measured against this fork's own ElegooLink: it speaks SDCP over its own websocket and
        // sends only Cmd 0 (status, one field read) and Cmd 128 (start print). Cmd 1 (Attributes),
        // where an SDCP device would describe its materials, is declared and never sent.
        CHECK_FALSE(can_probe("elegoolink"));
        CHECK(can_probe("octoprint"));
        const std::string note = no_filament_note("elegoolink");
        CHECK(note.find("Elegoo Link") != std::string::npos);
        CHECK(note.find("sent exactly as it was sliced") != std::string::npos);

        Device d   = make_device("Centauri", "192.0.2.1", "elegoolink");
        Status  st = probe(d, 1); // no network is touched: the host type is not probeable
        CHECK_FALSE(st.probed);
        CHECK_FALSE(st.online);
        CHECK(st.slots.empty());
        CHECK(st.note == note);
    }

    SECTION("a device with no address is not probed either") {
        Device d;
        d.host_type = "octoprint";
        const Status st = probe(d, 1);
        CHECK_FALSE(st.probed);
        CHECK(st.note == "this device has no address");
    }
}

TEST_CASE("PrintHostDevices: which send a preset takes", "[PrintHostDevices]")
{
    // The regression this case exists for. `use_new_connect` is a GLOBAL app-config flag: SSWCP and
    // SMPhysicalPrinterDialog set it to "true" the moment any Snapmaker machine connects, and
    // nothing clears it when the user then selects a different printer preset. The send branch used
    // to read it on its own (`use_new_connect == "true" || is_snapmaker_u1`), so on a PC with a
    // Snapmaker U1 on the LAN, selecting an Elegoo Centauri Carbon and pressing Print handed the
    // plate to WebPreprintDialog - the Snapmaker pre-treat page - instead of uploading it to the
    // Elegoo host.

    SECTION("a print-host printer never takes the Snapmaker flow, flag or no flag")
    {
        // The owner's machine: an Elegoo Centauri Carbon with a U1 connected beside it.
        CHECK(send_flow_for("Elegoo Centauri Carbon", true) == SendFlow::PrintHost);
        CHECK(send_flow_for("Elegoo Centauri Carbon", false) == SendFlow::PrintHost);

        // And every other host type this store is for.
        for (const char* model : {"Prusa MK4", "Voron 2.4", "Duet Delta", "Creality K1", "Custom Printer"}) {
            CAPTURE(model);
            CHECK(send_flow_for(model, true) == SendFlow::PrintHost);
            CHECK(send_flow_for(model, false) == SendFlow::PrintHost);
        }
    }

    SECTION("a U1 always takes the connect flow, as it did before the fix")
    {
        // Unchanged behaviour: the U1's toolhead mapping page is the only way to pick its tools.
        CHECK(send_flow_for("Snapmaker U1", true) == SendFlow::SnapmakerConnect);
        CHECK(send_flow_for("Snapmaker U1", false) == SendFlow::SnapmakerConnect);
        // The model name is matched case-insensitively, the way the rest of the GUI matches it.
        CHECK(send_flow_for("snapmaker u1 (0.4 nozzle)", false) == SendFlow::SnapmakerConnect);
    }

    SECTION("another Snapmaker follows its own connection")
    {
        CHECK(send_flow_for("Snapmaker Artisan", true) == SendFlow::SnapmakerConnect);
        // Not connected: it is reached at whatever print_host it holds, like any other host.
        CHECK(send_flow_for("Snapmaker Artisan", false) == SendFlow::PrintHost);
    }

    SECTION("an empty model name is a print host, not a Snapmaker")
    {
        // A hand-made preset with no printer_model at all must not be swept into the connect flow.
        CHECK(send_flow_for("", true) == SendFlow::PrintHost);
        CHECK(send_flow_for("", false) == SendFlow::PrintHost);
    }

    SECTION("the model predicates themselves")
    {
        CHECK(is_snapmaker_model("Snapmaker U1"));
        CHECK(is_snapmaker_model("SNAPMAKER Artisan"));
        CHECK_FALSE(is_snapmaker_model("Elegoo Centauri Carbon"));
        CHECK_FALSE(is_snapmaker_model(""));

        CHECK(is_snapmaker_u1_model("Snapmaker U1"));
        CHECK_FALSE(is_snapmaker_u1_model("Snapmaker Artisan"));
        // "U1" without the vendor is not a U1: a model name is vendor-qualified in practice.
        CHECK_FALSE(is_snapmaker_u1_model("Elegoo U1"));
    }
}

TEST_CASE("PrintHostDevices: the preset bridge writes the fields the send path reads", "[PrintHostDevices]")
{
    // A bare config: apply_to_config creates the options it needs, so this works on a preset's
    // config and on an empty one alike.
    DynamicPrintConfig config;

    Device d      = make_device("Right bay", "192.168.1.42", "octoprint");
    d.auth_type   = "user";
    d.user        = "someone";
    d.password    = "pw";
    apply_to_config(d, config);

    CHECK(config.opt_string("print_host") == "192.168.1.42");
    CHECK(config.opt_string("printhost_user") == "someone");
    CHECK(config.opt_string("printhost_password") == "pw");
    CHECK(config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htOctoPrint);
    CHECK(config.option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type")->value == atUserPassword);

    // And back out again, unchanged.
    const Device read = from_config(config);
    CHECK(read.address == d.address);
    CHECK(read.host_type == "octoprint");
    CHECK(read.auth_type == "user");
    CHECK(read.user == "someone");
    CHECK(read.password == "pw");

    CHECK(host_type_enum("elegoolink") == htElegooLink);
    CHECK(host_type_key(htElegooLink) == "elegoolink");
    CHECK(host_type_enum("nonsense") == htOctoPrint); // an unknown key is never a crash
    CHECK(speaks_moonraker("octoprint"));
    CHECK_FALSE(speaks_moonraker("elegoolink"));

    CHECK(normalize_address(" 192.168.1.41/ ") == "192.168.1.41");
    CHECK(normalize_address("HTTP://Printer.local/") == "http://printer.local");
}

TEST_CASE("PrintHostDevices: a device's serial reaches the Flashforge send, an empty one keeps the preset's", "[PrintHostDevices]")
{
    // The Flashforge HTTP local API is spoken with flashforge_serial_number + printhost_apikey (the
    // check code), both read from the per-device config the send builds. Two Creator 5s under one
    // preset must each send their own serial.
    DynamicPrintConfig preset;
    preset.opt_string("flashforge_serial_number", true) = "SNPRESET000001";

    Device left    = make_device("Left", "192.168.1.51", "flashforge");
    left.apikey    = "12345678";
    left.serial    = "SNLEFT00000001";
    Device right   = make_device("Right", "192.168.1.52", "flashforge");
    right.apikey   = "87654321";
    right.serial   = "SNRIGHT0000001";

    const DynamicPrintConfig l = config_for(left, preset);
    const DynamicPrintConfig r = config_for(right, preset);
    CHECK(l.opt_string("flashforge_serial_number") == "SNLEFT00000001");
    CHECK(l.opt_string("printhost_apikey") == "12345678");
    CHECK(l.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htFlashforge);
    CHECK(r.opt_string("flashforge_serial_number") == "SNRIGHT0000001");
    CHECK(r.opt_string("printhost_apikey") == "87654321");
    CHECK(from_config(l).serial == "SNLEFT00000001");

    // A device that was never given a serial (every non-Flashforge printer, and a Flashforge added
    // before this field existed) falls back to whatever the preset holds.
    Device bare = make_device("Bare", "192.168.1.53", "flashforge");
    CHECK(config_for(bare, preset).opt_string("flashforge_serial_number") == "SNPRESET000001");

    // And the migration carries the preset's serial into device 1.
    ScopedStore store("serial_migration");
    PresetHost  p;
    p.model_key     = "Flashforge Creator 5";
    p.preset_name   = "Creator 5";
    p.address       = "192.168.1.60";
    p.host_type     = "flashforge";
    p.apikey        = "11112222";
    p.serial        = "SNMIGRATED0001";
    p.printer_model = "Flashforge Creator 5";
    REQUIRE(migrate_from_presets(std::vector<PresetHost>{ p }) == 1);
    const std::vector<Device> back = devices(p.model_key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].serial == "SNMIGRATED0001");
}

TEST_CASE("PrusaLinkStatus: what a PrusaLink printer says it is doing", "[PrintHostDevices]")
{
    // No network here: parse_status is the half of probe() that turns the two REST answers into a
    // Status, so the mapping is exercised without a Buddy board on somebody's desk. The bodies
    // below are the shapes PrusaLink 2.1.2 answers on an MK4S.
    using namespace Slic3r::PrusaLinkStatus;

    SECTION("an idle printer: a state and two heaters, no job") {
        const std::string body =
            "{\"printer\":{\"state\":\"IDLE\",\"temp_nozzle\":24.7,\"target_nozzle\":0.0,"
            " \"temp_bed\":23.9,\"target_bed\":0.0,\"axis_z\":10.0,\"flow\":100,\"speed\":100},"
            " \"storage\":{\"path\":\"/usb/\",\"name\":\"usb\",\"read_only\":false}}";
        const PrusaLinkStatus::Status st = parse_status(body);
        CHECK(st.answered);
        CHECK(st.authorized);
        CHECK(st.raw_state == "IDLE");
        CHECK(st.state == "standby");
        CHECK(st.has_bed);
        CHECK(st.bed_temp == Approx(23.9));
        CHECK(st.bed_target == Approx(0.0));
        CHECK(st.has_nozzle);
        CHECK(st.nozzle_temp == Approx(24.7));
        CHECK_FALSE(st.has_progress);       // nothing is printing, so no percentage is invented
        CHECK_FALSE(st.has_time_remaining);
        CHECK(st.filename.empty());
    }

    SECTION("a printing one, status plus job: progress, time left and the file name") {
        const std::string status =
            "{\"printer\":{\"state\":\"PRINTING\",\"temp_nozzle\":215.0,\"target_nozzle\":215.0,"
            " \"temp_bed\":60.1,\"target_bed\":60.0},"
            " \"job\":{\"id\":34,\"progress\":62.0,\"time_remaining\":1440,\"time_printing\":2400}}";
        // /api/v1/job is the only answer that names the file; the status one never does.
        const std::string job =
            "{\"id\":34,\"state\":\"PRINTING\",\"progress\":62.0,\"time_remaining\":1440,"
            " \"time_printing\":2400,\"file\":{\"name\":\"CUBE~1.BGC\",\"display_name\":\"calibration cube.bgcode\","
            " \"path\":\"/usb\",\"size\":91234}}";
        const PrusaLinkStatus::Status st = parse_status(status, job);
        CHECK(st.answered);
        CHECK(st.state == "printing");
        CHECK(st.has_progress);
        CHECK(st.progress == Approx(62.0));
        CHECK(st.has_time_remaining);
        CHECK(st.time_remaining == 1440);
        CHECK(st.has_time_printing);
        CHECK(st.time_printing == 2400);
        CHECK(st.job_id == 34);
        CHECK(st.filename == "calibration cube.bgcode"); // display_name wins over the 8.3 name
        CHECK(st.has_nozzle);
        CHECK(st.nozzle_target == Approx(215.0));
    }

    SECTION("a job with no display_name falls back to the on-disk one") {
        const std::string status = "{\"printer\":{\"state\":\"PAUSED\"}}";
        const std::string job    = "{\"state\":\"PAUSED\",\"file\":{\"name\":\"PART~1.GCO\"}}";
        const PrusaLinkStatus::Status st     = parse_status(status, job);
        CHECK(st.state == "paused");
        CHECK(st.filename == "PART~1.GCO");
    }

    SECTION("the state vocabulary is PrusaLink's, mapped onto this API's") {
        CHECK(to_klipper_state("IDLE") == "standby");
        CHECK(to_klipper_state("READY") == "standby");
        CHECK(to_klipper_state("PRINTING") == "printing");
        CHECK(to_klipper_state("PAUSED") == "paused");
        CHECK(to_klipper_state("FINISHED") == "complete");
        CHECK(to_klipper_state("STOPPED") == "cancelled");
        CHECK(to_klipper_state("ERROR") == "error");
        CHECK(to_klipper_state("ATTENTION") == "error");
        CHECK(to_klipper_state("BUSY") == "busy");
        CHECK(to_klipper_state("printing") == "printing");      // case does not matter
        CHECK(to_klipper_state("SOMETHING_NEW") == "something_new"); // never dropped, just lowered
        CHECK(to_klipper_state("").empty());
    }

    SECTION("something that is not a PrusaLink printer") {
        CHECK_FALSE(parse_status("{}").answered);
        CHECK_FALSE(parse_status("<html>404</html>").answered);
        CHECK_FALSE(parse_status("{\"state\":{\"text\":\"Operational\"}}").answered); // an OctoPrint box
        CHECK_FALSE(parse_status("").answered);
    }

    SECTION("progress is clamped, negative times are dropped") {
        const PrusaLinkStatus::Status st = parse_status(
            "{\"printer\":{\"state\":\"PRINTING\"},\"job\":{\"progress\":100.0001,\"time_remaining\":-1}}");
        CHECK(st.has_progress);
        CHECK(st.progress == Approx(100.0));
        CHECK_FALSE(st.has_time_remaining); // PrusaLink sends -1 for "no estimate yet"
    }

    SECTION("which host types this client speaks, and how it addresses them") {
        CHECK(speaks_prusalink("prusalink"));
        CHECK(speaks_prusalink("prusaconnect"));
        CHECK_FALSE(speaks_prusalink("octoprint"));
        CHECK_FALSE(speaks_prusalink("elegoolink"));
        CHECK_FALSE(speaks_prusalink(""));
        CHECK(base_url("192.168.1.50") == "http://192.168.1.50");
        CHECK(base_url("http://prusa.local/") == "http://prusa.local");
        CHECK(base_url("https://connect.prusa3d.com") == "https://connect.prusa3d.com");
        CHECK(base_url("").empty());
    }

    SECTION("can_probe now covers PrusaLink, and it still says it maps no filaments") {
        CHECK(PrintHostDevices::can_probe("prusalink"));
        CHECK(PrintHostDevices::can_probe("prusaconnect"));
        CHECK(PrintHostDevices::can_probe("octoprint"));
        CHECK_FALSE(PrintHostDevices::can_probe("elegoolink"));
        // It reports a state but never a filament, and the dialog has to say so.
        CHECK(PrintHostDevices::no_filament_note("prusalink").find("PrusaLink") != std::string::npos);
    }
}

TEST_CASE("PrintHost: get_err_code_from_body does not throw on malformed JSON", "[PrintHost]")
{
    // Duet/MKS (and the deleted ESP3D/Flashforge copies) used throwing pt::read_json.
    // Html from a non-Duet host, garbage, a JSON array, or a non-integer err must return -1
    // rather than throw out of an Http on_complete callback (Orca #15947).
    CHECK(PrintHost::get_err_code_from_body("") == -1);
    CHECK(PrintHost::get_err_code_from_body("<html>") == -1);
    CHECK(PrintHost::get_err_code_from_body("[]") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"x"})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":0})") == 0);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":1})") == 1);
    // A successful Duet connect often has no err field; that is success (0), not unknown (-1).
    CHECK(PrintHost::get_err_code_from_body(R"({})") == 0);
    REQUIRE_NOTHROW(PrintHost::get_err_code_from_body("<html>404</html>"));
}
