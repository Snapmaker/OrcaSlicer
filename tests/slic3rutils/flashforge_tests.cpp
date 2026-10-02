// The Flashforge port-8898 HTTP local API: the parsing and the one decision that routes a printer
// to it.
//
// A Creator 5 / Adventurer 5M-series printer does not speak the old TCP-8899 M-code console at all,
// so a user who fills the Physical Printer dialog in and presses Test gets one of two very
// different conversations depending on a single predicate. That predicate, and the shapes of the
// JSON the printer answers with, are the contract worth pinning:
//
//  1. uses_local_api(). Serial number AND check code, both present, is what selects the HTTP path.
//     Miss either and the printer stays on the legacy serial console - which is exactly how an
//     Adventurer 3/4 owner keeps working after this change. Getting this backwards is the reported
//     bug ("target machine actively refused it"), so it gets its own tests.
//
//  2. validate_response(). The printer answers HTTP 200 with a JSON body that carries its own
//     result code; a non-zero `code`/`err` is a refusal (a wrong check code lands here) and the
//     message it carries is the only thing that tells the user which field is wrong.
//
//  3. parse_detail_material_slots(). The IFS (multi-filament station) mapping dialog is built from
//     /detail. The firmware has shipped both camelCase and PascalCase spellings of these keys and
//     reports "no station" by several different routes, so the reader tolerates all of them.
//
//  4. sanitize_filename() / url_for(). The printer rejects '=' and friends in an upload name, and
//     the URL is assembled from whatever the user typed in Hostname (bare IP, or a full URL).
//
//  5. upload_header_flag(). The send dialog's checkboxes travel to /uploadGcode as an extended-info
//     map of "1"/"0" and leave as "true"/"false" headers. flowCalibration and firstLayerInspection
//     were hardcoded "false" before the dialog exposed them, so the conversion - and what an
//     absent key means - is worth pinning: a missing option is off, never on.
//
// Nothing here touches a printer, a socket, wx or the filesystem.

#include <catch2/catch.hpp>

#include <map>
#include <string>
#include <vector>

#include "slic3r/Utils/FlashforgeLocalApi.hpp"

using Slic3r::FlashforgeMaterialSlot;
namespace ff = Slic3r::FlashforgeLocalApi;

// ---------------------------------------------------------------- the legacy/HTTP decision ----

TEST_CASE("Flashforge local API needs both the serial number and the check code", "[Flashforge]")
{
    // The AD5-series case: both fields filled in, so the HTTP local API is used.
    CHECK(ff::uses_local_api("SNMOCK123456", "abcd1234"));

    // Every way of being incomplete keeps the printer on the legacy TCP-8899 console. This is what
    // preserves the Adventurer 3/4 workflow, which has no serial/check-code fields to fill.
    CHECK_FALSE(ff::uses_local_api("", ""));
    CHECK_FALSE(ff::uses_local_api("SNMOCK123456", ""));
    CHECK_FALSE(ff::uses_local_api("", "abcd1234"));
}

// ------------------------------------------------------------------------ the URL it talks to ----

TEST_CASE("Flashforge local API URLs are built on port 8898", "[Flashforge]")
{
    SECTION("a bare address is used as-is")
    {
        CHECK(ff::url_for("192.168.1.50", "detail") == "http://192.168.1.50:8898/detail");
        CHECK(ff::url_for("192.168.1.50", "product") == "http://192.168.1.50:8898/product");
        CHECK(ff::url_for("192.168.1.50", "uploadGcode") == "http://192.168.1.50:8898/uploadGcode");
    }

    SECTION("a host with a path keeps only the host")
    {
        CHECK(ff::host_name_of("192.168.1.50/whatever") == "192.168.1.50");
    }

    SECTION("a full URL is reduced to its host, so the port is ours and not the user's")
    {
        CHECK(ff::host_name_of("http://192.168.1.50:80/") == "192.168.1.50");
        CHECK(ff::url_for("http://printer.local:8080/", "detail") == "http://printer.local:8898/detail");
    }
}

// ------------------------------------------------------------------------- the upload filename ----

TEST_CASE("Flashforge upload names drop only characters the printer rejects", "[Flashforge]")
{
    // '=' is the one the printer is known to refuse, along with the path separators and shell
    // metacharacters the send dialog already warns about.
    CHECK(ff::sanitize_filename("plate=1.gcode.3mf") == "plate_1.gcode.3mf");
    // No drive-letter shape here: fs::path would read a leading "x:" as a Windows drive and drop
    // it before the replacing below ever runs.
    CHECK(ff::sanitize_filename("plate*1?v2.3mf") == "plate_1_v2.3mf");

    // Dots, underscores and hyphens survive - they are how the extension and most names are spelled.
    CHECK(ff::sanitize_filename("Benchy_v1-2.gcode") == "Benchy_v1-2.gcode");

    // Only the basename is uploaded, never the directories above it.
    CHECK(ff::sanitize_filename("C:/tmp/sub dir/part.3mf") == "part.3mf");

    // An empty name still has to be something, and the fallback extension rides along.
    CHECK(ff::sanitize_filename("", ".3mf") == "print.3mf");
    CHECK(ff::sanitize_filename("") == "print");

    // A name that was nothing but illegal characters cannot become a bare run of underscores.
    CHECK(ff::sanitize_filename("///", ".3mf") == "print.3mf");
}

TEST_CASE("Flashforge upload names keep the characters Flash Studio keeps", "[Flashforge]")
{
    // The reported bug: a plate the user called "Kyogre x10" arrived on the Creator 5's touchscreen
    // as an unreadable row of underscores, because the sanitiser whitelisted [A-Za-z0-9._-] and
    // folded everything else - the space included - into '_'. Flash Studio's own uploads keep
    // spaces and parentheses ("stylized dragon toy 3d model(1)_PLA_3h37m.gcode.3mf" is a real one,
    // sampled from the printer), so the name the user reads has to survive.
    SECTION("a space is not a character the printer refuses")
    {
        CHECK(ff::sanitize_filename("Kyogre x10_PLA_3h37m.gcode.3mf") == "Kyogre x10_PLA_3h37m.gcode.3mf");
        CHECK(ff::sanitize_filename("my model v2.3mf") == "my model v2.3mf");
    }

    SECTION("parentheses survive, exactly as in Flash Studio's own upload")
    {
        CHECK(ff::sanitize_filename("stylized dragon toy 3d model(1)_PLA_3h37m.gcode.3mf") ==
              "stylized dragon toy 3d model(1)_PLA_3h37m.gcode.3mf");
    }

    SECTION("a non-ASCII name travels as whole UTF-8, never mangled per byte")
    {
        // Every byte of these is >= 0x80 apart from the ASCII run; rewriting them one at a time is
        // what turned a title into per-byte garbage. The name must come back byte-for-byte.
        const std::string jp = "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3 x2_PLA_1h02m.gcode.3mf";
        CHECK(ff::sanitize_filename(jp) == jp);

        // "Pokemon" with an e-acute, spelled as its UTF-8 bytes so the test does not depend on
        // how the compiler reads this source file's own encoding.
        const std::string accented = "Pok\xC3\xA9mon Kyogre x10.gcode.3mf";
        CHECK(ff::sanitize_filename(accented) == accented);
    }
}

// ------------------------------------------------- the name a sliced plate must be sent under ----

TEST_CASE("Flashforge sliced 3mf uploads carry the .gcode.3mf double extension", "[Flashforge]")
{
    // The bug this exists for: the send path named the upload "<model>.3mf", and the Creator 5
    // listed it with no thumbnail and froze its touchscreen when the file was selected. Flash
    // Studio, upstream OrcaSlicer and this program's own File > Export plate sliced file all
    // write "<model>.gcode.3mf", and a file copied to the printer under that name opens.
    CHECK(ff::sliced_3mf_name("dragon.3mf") == "dragon.gcode.3mf");

    // The default the send dialog now offers is already right and must survive untouched -
    // it must not become "dragon.gcode.gcode.3mf".
    CHECK(ff::sliced_3mf_name("dragon.gcode.3mf") == "dragon.gcode.3mf");
    CHECK(ff::sliced_3mf_name("dragon.GCODE.3MF") == "dragon.GCODE.3MF");

    // A name the user typed over in the dialog: no extension, or the raw G-code one.
    CHECK(ff::sliced_3mf_name("dragon") == "dragon.gcode.3mf");
    CHECK(ff::sliced_3mf_name("dragon.gcode") == "dragon.gcode.3mf");

    // Only the last extension is replaced; dots inside the name are part of it.
    CHECK(ff::sliced_3mf_name("my.model.v2.3mf") == "my.model.v2.gcode.3mf");

    CHECK(ff::sliced_3mf_name("") == "print.gcode.3mf");
}

TEST_CASE("Flashforge upload names keep the double extension through sanitising", "[Flashforge]")
{
    // The two run back to back in Flashforge::upload_local_api(): the name is given the double
    // extension and then stripped of characters the printer refuses. The stripping must not eat
    // either dot, or the fix above is undone on the way out.
    const auto sent = [](const std::string& name) {
        return ff::sanitize_filename(ff::sliced_3mf_name(name), ".3mf");
    };

    CHECK(sent("dragon.3mf") == "dragon.gcode.3mf");
    CHECK(sent("my model v2.3mf") == "my model v2.gcode.3mf");
    CHECK(sent("plate=1.3mf") == "plate_1.gcode.3mf");

    // The whole point of the fix, end to end: what the send dialog offers for a project called
    // "Kyogre x10" is what the touchscreen shows.
    CHECK(sent("Kyogre x10_PLA_3h37m.gcode.3mf") == "Kyogre x10_PLA_3h37m.gcode.3mf");
    CHECK(sent("Kyogre x10") == "Kyogre x10.gcode.3mf");

    // The upload_path can arrive with directories on it; only the basename is posted, and it
    // still ends in the double extension.
    CHECK(sent("C:/tmp/sub dir/part.3mf") == "part.gcode.3mf");

    // Every name the printer is offered ends in ".gcode.3mf" - the property the firmware keys on.
    for (const char* name : {"dragon.3mf", "dragon", "dragon.gcode", "a=b c.3mf", ""}) {
        const std::string out = sent(name);
        CHECK(out.size() > 10);
        CHECK(out.substr(out.size() - 10) == ".gcode.3mf");
    }
}

// ------------------------------------------------------------- the result code in every reply ----

TEST_CASE("Flashforge replies are accepted or refused by their own result code", "[Flashforge]")
{
    std::string error;

    SECTION("code 0 is success")
    {
        REQUIRE(ff::validate_response(R"({"code":0,"message":"Success"})", error));
        CHECK(error.empty());
    }

    SECTION("a reply with no code at all is success")
    {
        // /product answers with the payload and no envelope code on some firmware.
        REQUIRE(ff::validate_response(R"({"product":{"nozzleTempCtrlState":1}})", error));
        CHECK(error.empty());
    }

    SECTION("a non-zero code is a refusal and carries the printer's own words")
    {
        // This is what a wrong check code looks like coming back over HTTP 200.
        REQUIRE_FALSE(ff::validate_response(R"({"code":5,"message":"token is invalid"})", error));
        CHECK(error.find("5") != std::string::npos);
        CHECK(error.find("token is invalid") != std::string::npos);
    }

    SECTION("the older err/msg spelling is understood too")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"err":3,"msg":"check code error"})", error));
        CHECK(error.find("check code error") != std::string::npos);
    }

    SECTION("a code carried as a string still counts")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"code":"7","message":"busy"})", error));
        CHECK(error.find("busy") != std::string::npos);
    }

    SECTION("a refusal with no message still says something")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"code":9})", error));
        CHECK_FALSE(error.empty());
    }

    SECTION("a body that is not JSON, or not an object, is a failure and not a crash")
    {
        // What a printer that is not a Flashforge answers with - or an HTML error page.
        REQUIRE_FALSE(ff::validate_response("<html>404</html>", error));
        CHECK_FALSE(error.empty());

        REQUIRE_FALSE(ff::validate_response("[1,2,3]", error));
        CHECK_FALSE(error.empty());

        REQUIRE_FALSE(ff::validate_response("", error));
        CHECK_FALSE(error.empty());
    }
}

// ------------------------------------------------------------------------ the IFS slot mapping ----

TEST_CASE("Flashforge /detail yields the material station slots", "[Flashforge]")
{
    std::vector<FlashforgeMaterialSlot> slots;
    bool                                has_station = false;

    SECTION("a four-slot AD5X station is read whole")
    {
        const std::string body = R"({
            "code": 0,
            "detail": {
                "hasMatlStation": 1,
                "matlStationInfo": {
                    "slotCnt": 4,
                    "slotInfos": [
                        {"slotId": 1, "hasFilament": true,  "materialName": "PLA",     "materialColor": "#FF0000"},
                        {"slotId": 2, "hasFilament": true,  "materialName": "PETG",    "materialColor": "#00FF00"},
                        {"slotId": 3, "hasFilament": false, "materialName": "",        "materialColor": ""},
                        {"slotId": 4, "hasFilament": true,  "materialName": "PLA-CF",  "materialColor": "#0000FF"}
                    ]
                }
            }
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 4);

        CHECK(slots[0].slot_id == 1);
        CHECK(slots[0].has_filament);
        CHECK(slots[0].material_name == "PLA");
        CHECK(slots[0].material_color == "#FF0000");

        // An empty slot is reported, not skipped: the mapping dialog has to show it as empty.
        CHECK(slots[2].slot_id == 3);
        CHECK_FALSE(slots[2].has_filament);
        CHECK(slots[2].material_name.empty());

        CHECK(slots[3].material_name == "PLA-CF");
    }

    SECTION("the PascalCase spelling of the same reply reads identically")
    {
        const std::string body = R"({
            "detail": {
                "HasMatlStation": 1,
                "MatlStationInfo": {
                    "SlotCnt": 2,
                    "SlotInfos": [
                        {"slotId": 1, "hasFilament": true, "materialName": "ABS", "materialColor": "#101010"},
                        {"slotId": 2, "hasFilament": false}
                    ]
                }
            }
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].material_name == "ABS");
        CHECK_FALSE(slots[1].has_filament);
    }

    SECTION("a reply with the detail inlined rather than nested still reads")
    {
        const std::string body = R"({
            "hasMatlStation": 1,
            "matlStationInfo": {"slotCnt": 1, "slotInfos": [{"slotId": 1, "hasFilament": true, "materialName": "PLA"}]}
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 1);
    }

    SECTION("a printer with no station reports none and no slots")
    {
        // A plain Creator 5 / AD5M: single extruder, no IFS. The send dialog must not offer mapping.
        const std::string body = R"({"code":0,"detail":{"hasMatlStation":0,"firmwareVersion":"3.1.3"}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK_FALSE(has_station);
        CHECK(slots.empty());
    }

    SECTION("slots present without the flag still mean a station")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[{"slotId":1,"hasFilament":true}]}}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 1);
    }

    SECTION("a missing slotId falls back to the slot's position, 1-based")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[
            {"hasFilament":true,"materialName":"PLA"},
            {"hasFilament":true,"materialName":"PETG"}
        ]}}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].slot_id == 1);
        CHECK(slots[1].slot_id == 2);
    }

    SECTION("a body that will not parse is a failure, not a crash")
    {
        CHECK_FALSE(ff::parse_detail_material_slots("not json at all", slots, has_station));
        CHECK(slots.empty());
    }

    SECTION("slotInfos that is a number yields no slots and does not throw")
    {
        // Ranging a non-array JSON value iterates the primitive itself; slot.value() then throws.
        REQUIRE_NOTHROW(ff::parse_detail_material_slots(
            R"({"detail":{"matlStationInfo":{"slotInfos":4}}})", slots, has_station));
        REQUIRE(ff::parse_detail_material_slots(
            R"({"detail":{"matlStationInfo":{"slotInfos":4}}})", slots, has_station));
        CHECK(slots.empty());
        CHECK_FALSE(has_station);
    }

    SECTION("an array of strings yields no slots")
    {
        REQUIRE(ff::parse_detail_material_slots(
            R"({"detail":{"matlStationInfo":{"slotInfos":["a","b"]}}})", slots, has_station));
        CHECK(slots.empty());
        CHECK(has_station); // a non-empty array still means the printer reported a station
    }

    SECTION("mixed objects and non-objects keep only the objects")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[
            {"slotId":1,"hasFilament":true,"materialName":"PLA","materialColor":"#FF0000"},
            "not an object",
            12,
            {"slotId":3,"hasFilament":true,"materialName":"PETG"}
        ]}}})";
        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].material_name == "PLA");
        CHECK(slots[0].material_color == "#FF0000");
        CHECK(slots[1].slot_id == 3);
        CHECK(slots[1].material_name == "PETG");
    }

    SECTION("wrong-typed fields are dropped rather than thrown")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[
            {"slotId":"x","hasFilament":"yes","materialName":12,"materialColor":true}
        ]}}})";
        REQUIRE_NOTHROW(ff::parse_detail_material_slots(body, slots, has_station));
        REQUIRE(slots.size() == 1);
        CHECK(slots[0].slot_id == 1); // 1-based position fallback
        CHECK_FALSE(slots[0].has_filament);
        CHECK(slots[0].material_name.empty());
        CHECK(slots[0].material_color.empty());
    }
}

// ---------------------------------------------------- the job's boolean options as headers ----

TEST_CASE("Flashforge upload options become true/false headers", "[Flashforge]")
{
    SECTION("a checked option is \"true\", an unchecked one \"false\"")
    {
        // What FlashforgePrintHostSendDialog::extendedInfo() hands to Flashforge::upload_local_api()
        // with every box ticked.
        const std::map<std::string, std::string> all_on{
            {"levelingBeforePrint", "1"},
            {"flowCalibration", "1"},
            {"firstLayerInspection", "1"},
            {"timeLapseVideo", "1"},
            {"useMatlStation", "1"},
        };

        CHECK(std::string(ff::upload_header_flag(all_on, "levelingBeforePrint")) == "true");
        CHECK(std::string(ff::upload_header_flag(all_on, "flowCalibration")) == "true");
        CHECK(std::string(ff::upload_header_flag(all_on, "firstLayerInspection")) == "true");
        CHECK(std::string(ff::upload_header_flag(all_on, "timeLapseVideo")) == "true");
        CHECK(std::string(ff::upload_header_flag(all_on, "useMatlStation")) == "true");

        const std::map<std::string, std::string> all_off{
            {"levelingBeforePrint", "0"},
            {"flowCalibration", "0"},
            {"firstLayerInspection", "0"},
            {"timeLapseVideo", "0"},
            {"useMatlStation", "0"},
        };

        CHECK(std::string(ff::upload_header_flag(all_off, "levelingBeforePrint")) == "false");
        CHECK(std::string(ff::upload_header_flag(all_off, "flowCalibration")) == "false");
        CHECK(std::string(ff::upload_header_flag(all_off, "firstLayerInspection")) == "false");
        CHECK(std::string(ff::upload_header_flag(all_off, "timeLapseVideo")) == "false");
        CHECK(std::string(ff::upload_header_flag(all_off, "useMatlStation")) == "false");
    }

    SECTION("an option nobody wrote is off, not on")
    {
        // A config written before the two new checkboxes existed, or any other print host path that
        // fills the map itself: the printer must not be told to burn filament on a calibration the
        // user never asked for.
        const std::map<std::string, std::string> legacy{
            {"levelingBeforePrint", "1"},
            {"timeLapseVideo", "0"},
        };

        CHECK(std::string(ff::upload_header_flag(legacy, "flowCalibration")) == "false");
        CHECK(std::string(ff::upload_header_flag(legacy, "firstLayerInspection")) == "false");
        CHECK(std::string(ff::upload_header_flag({}, "flowCalibration")) == "false");
    }

    SECTION("only \"1\" is on - nothing else is coaxed into a true")
    {
        // The dialog only ever writes "1"/"0", so anything else is a config the user edited by hand
        // or a value from somewhere unexpected; none of it turns an option on behind their back.
        const std::map<std::string, std::string> odd{
            {"flowCalibration", "true"},
            {"firstLayerInspection", "yes"},
            {"timeLapseVideo", ""},
            {"useMatlStation", "01"},
        };

        CHECK(std::string(ff::upload_header_flag(odd, "flowCalibration")) == "false");
        CHECK(std::string(ff::upload_header_flag(odd, "firstLayerInspection")) == "false");
        CHECK(std::string(ff::upload_header_flag(odd, "timeLapseVideo")) == "false");
        CHECK(std::string(ff::upload_header_flag(odd, "useMatlStation")) == "false");
    }
}
