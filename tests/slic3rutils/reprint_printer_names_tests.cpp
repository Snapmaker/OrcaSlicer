// The printer a Reprint record names, as the phone lists it (2026-09-26).
//
// The owner's Reprint tab showed a filter chip "Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883":
// the U1's pre-print page archives through the Device tab's connection, a U1 bound to a Snapmaker
// account is connected through the cloud's MQTT broker, and the record was named after the broker.
// Covered here:
//  - no printer is ever called by an address (name_is_address, display_printer_name);
//  - a "connect" record whose serial names a LAN card is listed as that card (normalize_printer),
//    and one without a serial - every record written before this - reads "Snapmaker U1";
//  - the archive is organised by the model a file was sliced for, and a reprint may go to any
//    printer of that model (never another model, and a record of no known model only back to its
//    own printer); a file is started in place only on the printer it was sent to.

#include <catch2/catch.hpp>

#include "slic3r/GUI/GcodeArchive.hpp"
#include "slic3r/GUI/SnapmakerLan.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using nlohmann::json;

namespace {

SnapmakerLan::Device card(const std::string& serial, const std::string& name, const std::string& ip)
{
    SnapmakerLan::Device d;
    d.id    = serial;
    d.name  = name;
    d.model = "Snapmaker U1";
    d.ip    = ip;
    return d;
}

// A sidecar as the pre-print page wrote it before this change.
json old_cloud_record()
{
    return json::parse(R"({
        "id": "20260921-101500-cube", "time": 1790000000, "file": "cube.gcode", "mode": "print",
        "printer": { "id": "connect", "kind": "connect",
                     "name": "Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883", "model": "" }
    })");
}

} // namespace

TEST_CASE("an address is never a printer name", "[ReprintNames]")
{
    SECTION("the cloud broker, with and without its port")
    {
        CHECK(GcodeArchive::name_is_address("Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883"));
        CHECK(GcodeArchive::name_is_address("a1pr8yczi3n0se.iot.us-west-1.amazonaws.com"));
    }
    SECTION("LAN addresses")
    {
        CHECK(GcodeArchive::name_is_address("192.168.1.50"));
        CHECK(GcodeArchive::name_is_address("Snapmaker 192.168.1.50:8883"));
        CHECK(GcodeArchive::name_is_address("u1.local"));
        CHECK(GcodeArchive::name_is_address("http://10.0.0.7:7125"));
        CHECK(GcodeArchive::name_is_address("[fe80::1]:7125"));
        CHECK(GcodeArchive::name_is_address("printer (192.168.1.50)"));
    }
    SECTION("a tailnet or other dotted host name")
    {
        CHECK(GcodeArchive::name_is_address("reaper.tail2dff02.ts.net"));
        CHECK(GcodeArchive::name_is_address("mainsail.example.com:80"));
    }
    SECTION("names people give printers")
    {
        for (const char* name : { "ToyPrinterX1C", "Optimuscle", "3DPO", "H2D2", "U1 left bench", "Snapmaker U1",
                                  "Voron 2.4", "Ender-3 V3 SE", "Prusa MK4S", "Printer v1.2.3", "Bench #2: U1" })
            CHECK_FALSE(GcodeArchive::name_is_address(name));
        CHECK_FALSE(GcodeArchive::name_is_address(""));
    }
}

TEST_CASE("the name shown falls back to the model, then to the kind", "[ReprintNames]")
{
    CHECK(GcodeArchive::display_printer_name("H2D2", "Bambu Lab H2D", "bambu") == "H2D2");
    CHECK(GcodeArchive::display_printer_name("  Optimuscle ", "", "snapmaker") == "Optimuscle");

    INFO("an address gives way to the model");
    CHECK(GcodeArchive::display_printer_name("Snapmaker 192.168.1.50:8883", "Snapmaker U1", "connect") == "Snapmaker U1");
    CHECK(GcodeArchive::display_printer_name("192.168.1.9", "Prusa MK4", "printhost") == "Prusa MK4");

    INFO("no model either: a word for the kind, never empty and never the address");
    CHECK(GcodeArchive::display_printer_name("Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883", "", "connect") ==
          "Snapmaker U1");
    CHECK(GcodeArchive::display_printer_name("", "", "snapmaker") == "Snapmaker U1");
    CHECK(GcodeArchive::display_printer_name("Snapmaker", "", "connect") == "Snapmaker U1");
    CHECK(GcodeArchive::display_printer_name("", "", "bambu") == "Bambu Lab printer");
    CHECK(GcodeArchive::display_printer_name("10.0.0.7", "", "printhost") == "Print host");
    CHECK(GcodeArchive::display_printer_name("", "", "") == "Printer");

    INFO("a model that is itself an address is skipped too");
    CHECK(GcodeArchive::display_printer_name("", "10.0.0.7:7125", "printhost") == "Print host");
}

TEST_CASE("a record filed under the Snapmaker cloud is listed under its printer", "[ReprintNames]")
{
    const std::vector<SnapmakerLan::Device> lan = { card("SN0001", "U1 left", "192.168.1.50"),
                                                    card("SN0002", "U2 right", "192.168.1.51") };

    SECTION("an old record with no serial reads Snapmaker U1, and stays a connect record")
    {
        json r = old_cloud_record();
        REQUIRE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r["printer"]["name"] == "Snapmaker U1");
        CHECK(r["printer"]["id"] == "connect");
        CHECK(r["printer"]["kind"] == "connect");
        INFO("what the sidecar said is kept alongside");
        CHECK(r["printer_recorded"]["name"] == "Snapmaker a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883");
        INFO("normalising again changes nothing more");
        const json once = r;
        CHECK_FALSE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r == once);
    }
    SECTION("the model the send recorded is the fallback name")
    {
        json r = old_cloud_record();
        r["printer"]["model"] = "Snapmaker U1 Pro";
        REQUIRE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r["printer"]["name"] == "Snapmaker U1 Pro");
    }
    SECTION("a serial that names a LAN card makes it that card")
    {
        json r = old_cloud_record();
        r["printer"]["serial"] = "sn0002"; // any case
        REQUIRE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r["printer"]["id"] == "sm:SN0002");
        CHECK(r["printer"]["kind"] == "snapmaker");
        CHECK(r["printer"]["name"] == "U2 right");
        CHECK(r["printer"]["model"] == "Snapmaker U1");
        CHECK(r["printer_recorded"]["id"] == "connect");
    }
    SECTION("a serial with no LAN card stays connect, under its own name")
    {
        json r = old_cloud_record();
        r["printer"]["serial"] = "SN9999";
        r["printer"]["name"]   = "Workshop U1";
        CHECK_FALSE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r["printer"]["id"] == "connect");
        CHECK(r["printer"]["name"] == "Workshop U1");
        CHECK_FALSE(r.contains("printer_recorded"));
    }
    SECTION("a LAN card with no name of its own is called by its model")
    {
        json r = old_cloud_record();
        r["printer"]["serial"] = "SN0003";
        std::vector<SnapmakerLan::Device> unnamed = { card("SN0003", "", "192.168.1.52") };
        REQUIRE(GcodeArchive::normalize_printer(r, unnamed));
        CHECK(r["printer"]["id"] == "sm:SN0003");
        CHECK(r["printer"]["name"] == "Snapmaker U1");
    }
    SECTION("every other record is left exactly as it was")
    {
        for (const char* text : {
                 R"({"id":"a","printer":{"id":"01P00A123","kind":"bambu","name":"ToyPrinterX1C","model":"X1C"}})",
                 R"({"id":"b","printer":{"id":"sm:SN0001","kind":"snapmaker","name":"U1 left","model":"Snapmaker U1"}})",
                 R"({"id":"c","printer":{"id":"ph:3dpo","kind":"printhost","name":"3DPO","model":""}})",
                 R"({"id":"d"})" }) {
            json r = json::parse(text);
            const json before = r;
            CHECK_FALSE(GcodeArchive::normalize_printer(r, lan));
            CHECK(r == before);
        }
    }
    SECTION("an address on any other kind gives way to its model")
    {
        json r = json::parse(R"({"id":"e","printer":{"id":"host","kind":"printhost","name":"192.168.1.9:7125","model":"Voron 2.4"}})");
        REQUIRE(GcodeArchive::normalize_printer(r, lan));
        CHECK(r["printer"]["name"] == "Voron 2.4");
        CHECK(r["printer"]["id"] == "host");
    }
}

namespace {

// The code table as resources/printers holds it (the built-in part of load_model_names).
const std::map<std::string, std::string>& names()
{
    static const std::map<std::string, std::string> n = GcodeArchive::load_model_names("");
    return n;
}

json rec(const std::string& id, const std::string& pid, const std::string& kind, const std::string& name, const std::string& model)
{
    return { { "id", id }, { "printer", { { "id", pid }, { "kind", kind }, { "name", name }, { "model", model } } } };
}

} // namespace

TEST_CASE("a record's model is one spelling, whatever named it", "[ReprintModels]")
{
    SECTION("Bambu model codes become the model's name")
    {
        CHECK(GcodeArchive::canonical_model("O1D", names()) == "Bambu Lab H2D");
        CHECK(GcodeArchive::canonical_model("O1C2", names()) == "Bambu Lab H2C");
        CHECK(GcodeArchive::canonical_model("O1C2-V2", names()) == "Bambu Lab H2C"); // a later hardware revision
        CHECK(GcodeArchive::canonical_model("BL-P001", names()) == "Bambu Lab X1 Carbon");
        CHECK(GcodeArchive::canonical_model("C12", names()) == "Bambu Lab P1S");
    }
    SECTION("a model name is kept as it is")
    {
        CHECK(GcodeArchive::canonical_model("Bambu Lab H2D", names()) == "Bambu Lab H2D");
        CHECK(GcodeArchive::canonical_model(" Snapmaker U1 ", names()) == "Snapmaker U1");
        CHECK(GcodeArchive::canonical_model("Elegoo Centauri Carbon", names()) == "Elegoo Centauri Carbon");
    }
    SECTION("nothing, or an address, is no model")
    {
        CHECK(GcodeArchive::canonical_model("", names()).empty());
        CHECK(GcodeArchive::canonical_model("192.168.1.9:7125", names()).empty());
    }
    SECTION("keys and chip labels")
    {
        CHECK(GcodeArchive::model_key("Bambu Lab H2D") == "bambu lab h2d");
        CHECK(GcodeArchive::model_key("Snapmaker  U1") == "snapmaker u1");
        CHECK(GcodeArchive::model_key("") == "other");
        CHECK(GcodeArchive::model_label("Bambu Lab H2D") == "Bambu H2D");
        CHECK(GcodeArchive::model_label("Bambu Lab H2C") == "Bambu H2C");
        CHECK(GcodeArchive::model_label("Bambu Lab X1 Carbon") == "Bambu X1C");
        CHECK(GcodeArchive::model_label("Bambu Lab P1S") == "Bambu P1S");
        CHECK(GcodeArchive::model_label("Elegoo Centauri Carbon") == "Elegoo CC");
        CHECK(GcodeArchive::model_label("Snapmaker U1") == "Snapmaker U1");
        CHECK(GcodeArchive::model_label("") == "Other");
    }
    SECTION("the live printer row and the record of the same H2D share a key")
    {
        CHECK(GcodeArchive::model_key(GcodeArchive::canonical_model("O1D", names())) ==
              GcodeArchive::model_key(GcodeArchive::canonical_model("Bambu Lab H2D", names())));
    }
}

TEST_CASE("a record with no model takes its printer's from the printer's other records", "[ReprintModels]")
{
    // The owner's archive: one H2C record was written with no model and the printer's serial as
    // its name; 24 others of the same printer say "Bambu Lab H2C" / "3DPO".
    std::vector<json> records = {
        rec("new", "31B8AK5C0900288", "bambu", "31B8AK5C0900288", ""),
        rec("mid", "31B8AK5C0900288", "bambu", "3DPO", "Bambu Lab H2C"),
        rec("old", "31B8AK5C0900288", "bambu", "3DPO", "Bambu Lab H2C"),
        rec("x1c", "00M09D542800682", "bambu", "ToyPrinterX1C", "Bambu Lab X1 Carbon"),
        rec("lone", "ph:bench", "printhost", "Bench", ""),
    };
    CHECK(GcodeArchive::fill_from_siblings(records) == 1);
    CHECK(records[0]["printer"]["model"] == "Bambu Lab H2C");
    CHECK(records[0]["printer"]["name"] == "3DPO");
    INFO("a printer with no model anywhere stays without one: it is Other");
    CHECK(records[4]["printer"]["model"] == "");
    CHECK(GcodeArchive::model_key(GcodeArchive::canonical_model(records[4]["printer"]["model"].get<std::string>(), names())) == "other");
}

TEST_CASE("a reprint may go to any printer of the model the file was sliced for", "[ReprintModels]")
{
    using GcodeArchive::reprint_target_refusal;
    SECTION("its own printer always")
    {
        CHECK(reprint_target_refusal("sm:U1A", "snapmaker", "Snapmaker U1", "sm:U1A", "snapmaker", "Snapmaker U1", names()).empty());
        INFO("even with no model on record");
        CHECK(reprint_target_refusal("ph:bench", "printhost", "", "ph:bench", "printhost", "", names()).empty());
    }
    SECTION("another printer of the same model")
    {
        CHECK(reprint_target_refusal("sm:U1A", "snapmaker", "Snapmaker U1", "sm:U1B", "snapmaker", "Snapmaker U1", names()).empty());
        CHECK(reprint_target_refusal("ph:v1", "printhost", "Voron 2.4", "ph:v2", "printhost", "voron  2.4", names()).empty());
        INFO("a U1 send made over the Snapmaker cloud goes to a U1's LAN card");
        CHECK(reprint_target_refusal("connect", "connect", "Snapmaker U1", "sm:U1B", "snapmaker", "Snapmaker U1", names()).empty());
        INFO("a Bambu record's model name matches a live printer's model code");
        CHECK(reprint_target_refusal("0948AD1", "bambu", "Bambu Lab H2D", "0948AD2", "bambu", "O1D", names()).empty());
    }
    SECTION("never another model")
    {
        CHECK_FALSE(reprint_target_refusal("0948AD1", "bambu", "Bambu Lab H2D", "31B8AK1", "bambu", "O1C2", names()).empty());
        CHECK_FALSE(reprint_target_refusal("sm:U1A", "snapmaker", "Snapmaker U1", "sm:J1", "snapmaker", "Snapmaker J1", names()).empty());
        const std::string why = reprint_target_refusal("ph:v1", "printhost", "Voron 2.4", "ph:mk4", "printhost", "Prusa MK4", names());
        CHECK(why.find("Voron 2.4") != std::string::npos);
        CHECK(why.find("Prusa MK4") != std::string::npos);
    }
    SECTION("never another kind")
    {
        CHECK_FALSE(reprint_target_refusal("sm:U1A", "snapmaker", "Snapmaker U1", "host", "printhost", "Snapmaker U1", names()).empty());
        CHECK_FALSE(reprint_target_refusal("0948AD1", "bambu", "Bambu Lab H2D", "sm:U1A", "snapmaker", "Bambu Lab H2D", names()).empty());
    }
    SECTION("a record of no known model only goes back to its own printer")
    {
        CHECK_FALSE(reprint_target_refusal("ph:bench", "printhost", "", "ph:other", "printhost", "Voron 2.4", names()).empty());
        CHECK_FALSE(reprint_target_refusal("ph:v1", "printhost", "Voron 2.4", "ph:v2", "printhost", "", names()).empty());
    }
    SECTION("a file is started in place only on the printer it went to")
    {
        CHECK(GcodeArchive::reprint_in_place_allowed("sm:U1A", "sm:U1A"));
        CHECK_FALSE(GcodeArchive::reprint_in_place_allowed("sm:U1A", "sm:U1B"));
        CHECK_FALSE(GcodeArchive::reprint_in_place_allowed("", ""));
    }
}

TEST_CASE("the archive is grouped by model, each with the printers it may be reprinted on", "[ReprintModels]")
{
    // The owner's archive, newest first: a U1 send filed under the Snapmaker cloud, H2D and H2C
    // jobs (one H2C record without its model), an X1C job and a print host job of no known model.
    std::vector<json> records = {
        rec("u1", "connect", "connect", "Snapmaker U1", "Snapmaker U1"),
        rec("h2d-new", "0948AD561201535", "bambu", "H2D2", "Bambu Lab H2D"),
        rec("h2c-bare", "31B8AK5C0900288", "bambu", "31B8AK5C0900288", ""),
        rec("h2c", "31B8AK5C0900288", "bambu", "3DPO", "Bambu Lab H2C"),
        rec("x1c", "00M09D542800682", "bambu", "ToyPrinterX1C", "Bambu Lab X1 Carbon"),
        rec("bench", "ph:bench", "printhost", "Bench", ""),
        rec("h2d-old", "0948AD561201535", "bambu", "H2D2", "Bambu Lab H2D"),
    };
    GcodeArchive::annotate_models(records, names());
    CHECK(records[0]["model_key"] == "snapmaker u1");
    CHECK(records[0]["model_name"] == "Snapmaker U1");
    CHECK(records[2]["model_key"] == "bambu lab h2c"); // from the printer's other record
    CHECK(records[2]["model_name"] == "Bambu H2C");
    CHECK(records[4]["model_name"] == "Bambu X1C");
    CHECK(records[5]["model_key"] == "other");

    // The hub's printer rows, as /summary lists them: Bambu model codes, LAN U1s, the cloud link.
    const json rows = json::parse(R"J([
        {"id":"sm:811002","name":"U1","model":"Snapmaker U1","kind":"snapmaker","online":true},
        {"id":"sm:811003","name":"U3","model":"Snapmaker U1","kind":"snapmaker","online":true,"stale":true},
        {"id":"connect","name":"Snapmaker (cloud)","model":"Snapmaker U1","kind":"connect","online":true},
        {"id":"0948AD561201535","name":"H2D2","model":"O1D","kind":"bambu","online":true},
        {"id":"31B8AK5C0900288","name":"3DPO","model":"O1C2","kind":"bambu","online":false},
        {"id":"00M09D542800682","name":"ToyPrinterX1C","model":"BL-P001","kind":"bambu","online":true},
        {"id":"ph:v24","name":"192.168.1.40:7125","model":"Voron 2.4","kind":"printhost","online":true}
    ])J");
    const json models = GcodeArchive::archive_models(records, rows, names());
    REQUIRE(models.size() == 5);
    CHECK(models[0]["key"] == "snapmaker u1");
    CHECK(models[1]["key"] == "bambu lab h2d");
    CHECK(models[1]["count"] == 2);
    CHECK(models[2]["key"] == "bambu lab h2c");
    CHECK(models[2]["count"] == 2);
    CHECK(models[3]["name"] == "Bambu X1C");
    CHECK(models[4]["key"] == "other"); // last, whatever order it came in
    CHECK(models[4]["name"] == "Other");

    INFO("each model lists the printers of it: never the cloud link, and a stale row is offline");
    const json& u1 = models[0]["printers"];
    REQUIRE(u1.size() == 2);
    CHECK(u1[0]["id"] == "sm:811002");
    CHECK(u1[0]["online"] == true);
    CHECK(u1[1]["id"] == "sm:811003");
    CHECK(u1[1]["online"] == false);
    REQUIRE(models[1]["printers"].size() == 1);
    CHECK(models[1]["printers"][0]["name"] == "H2D2");
    CHECK(models[2]["printers"][0]["online"] == false);
    INFO("Other offers no other printer; a model nobody holds offers none");
    CHECK(models[4]["printers"].empty());

    INFO("a print host named by its address is listed by its model");
    std::vector<json> voron = { rec("v", "ph:v24", "printhost", "Voron", "Voron 2.4") };
    GcodeArchive::annotate_models(voron, names());
    const json vm = GcodeArchive::archive_models(voron, rows, names());
    REQUIRE(vm.size() == 1);
    CHECK(vm[0]["printers"][0]["name"] == "Voron 2.4");
}
