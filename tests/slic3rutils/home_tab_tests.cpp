#include <catch2/catch.hpp>

#include "slic3r/Utils/HomeTabLogic.hpp"

using namespace Slic3r;
using namespace Slic3r::HomeTab;
using json = nlohmann::json;

// The Home tab's host (src/slic3r/GUI/HomePanel.cpp) follows these rules; the page itself
// (resources/web/home) is click-tested in a browser with a stand-in for the slicer.

TEST_CASE("home tab: sections", "[HomeTab]")
{
    CHECK(valid_section("recent"));
    CHECK(valid_section("history"));
    CHECK_FALSE(valid_section(""));
    CHECK(valid_section("library"));
    CHECK(valid_section("vendors"));
    CHECK_FALSE(valid_section("hub"));
    CHECK_FALSE(valid_section("Recent"));
    CHECK(section_or_default("history") == "history");
    CHECK(section_or_default("") == "recent");
    CHECK(section_or_default("hub") == "recent");
}

TEST_CASE("home tab: how an archived file opens again", "[HomeTab]")
{
    CHECK(archive_open_kind("plate_1.gcode") == ArchiveOpen::Gcode);
    CHECK(archive_open_kind("PLATE_1.GCODE") == ArchiveOpen::Gcode);
    CHECK(archive_open_kind("part.gco") == ArchiveOpen::Gcode);
    CHECK(archive_open_kind("part.g") == ArchiveOpen::Gcode);
    CHECK(archive_open_kind("job.gcode.3mf") == ArchiveOpen::Project);
    CHECK(archive_open_kind("Job.GCODE.3MF") == ArchiveOpen::Project);
    CHECK(archive_open_kind("model.3mf") == ArchiveOpen::None); // a plain project is not a sliced file
    CHECK(archive_open_kind("job.bgcode") == ArchiveOpen::None);
    CHECK(archive_open_kind("") == ArchiveOpen::None);
    CHECK(archive_open_kind("gcode") == ArchiveOpen::None);
}

TEST_CASE("home tab: filament colours are only ever hex", "[HomeTab]")
{
    CHECK(clean_colour("#1f3a5f") == "#1F3A5F");
    CHECK(clean_colour("#fff") == "#FFF");
    CHECK(clean_colour("#1F3A5F80") == "#1F3A5F80");
    CHECK(clean_colour("").empty());
    CHECK(clean_colour("1F3A5F").empty());
    CHECK(clean_colour("#12345").empty());
    CHECK(clean_colour("#GGGGGG").empty());
    CHECK(clean_colour("red").empty());
    CHECK(clean_colour("#fff;x").empty());
    CHECK(clean_colour("url(x)").empty());
}

TEST_CASE("home tab: a print history card carries display fields only", "[HomeTab]")
{
    const json sidecar = json::parse(R"({
        "id": "20260929-120000-u1-plate_1", "time": 1790000000, "file": "Snorlax_plate_1.gcode", "size": 1234,
        "sha256": "abc", "path": "/home/me/archive/Snorlax_plate_1.gcode",
        "project_title": "Snorlax", "project_path": "/home/me/prints/Snorlax.3mf",
        "plate": 1, "plate_name": "Ears", "mode": "print", "source": "phone",
        "printer": {"id": "sm:abc", "kind": "snapmaker", "name": "192.168.1.20:7125", "model": "Snapmaker U1", "serial": "S1"},
        "model_name": "Snapmaker U1", "estimated_time_s": 2700, "estimated_weight_g": 6.2,
        "filaments": [{"index": 0, "type": "PLA", "colour": "#1f3a5f", "grams": 6.2},
                      {"index": 1, "type": "PLA", "colour": "javascript:x", "grams": -3}],
        "reprints": [{"time": 1}, {"time": 2}], "has_thumbnail": true, "remote_path": "/sd/x.gcode", "mapping": "0:2"
    })");
    const json card = history_card(sidecar, "Snapmaker U1", true, true);

    CHECK(card["id"] == "20260929-120000-u1-plate_1");
    CHECK(card["title"] == "Snorlax");
    CHECK(card["file"] == "Snorlax_plate_1.gcode");
    CHECK(card["plate"] == 2); // 1-based
    CHECK(card["plate_name"] == "Ears");
    CHECK(card["printer"] == "Snapmaker U1");
    CHECK(card["mode"] == "print");
    CHECK(card["source"] == "phone");
    CHECK(card["print_time_s"] == 2700);
    CHECK(card["reprints"] == 2);
    CHECK(card["has_thumbnail"] == true);
    CHECK(card["can_open"] == true);
    CHECK(card["project_present"] == true);
    REQUIRE(card["filaments"].size() == 2);
    CHECK(card["filaments"][0]["colour"] == "#1F3A5F");
    CHECK(card["filaments"][1]["colour"] == "");
    CHECK(card["filaments"][1]["grams"] == 0.0);

    // Nothing that points somewhere on this PC or at a printer.
    const std::string dumped = card.dump();
    CHECK(dumped.find("/home/me") == std::string::npos);
    CHECK(dumped.find("192.168") == std::string::npos);
    CHECK(dumped.find("sha256") == std::string::npos);
    CHECK(dumped.find("remote_path") == std::string::npos);
    CHECK_FALSE(card.contains("path"));
    CHECK_FALSE(card.contains("project_path"));
}

TEST_CASE("home tab: a card from a thin or odd sidecar", "[HomeTab]")
{
    SECTION("no project title: the sent name, then the file, without extension")
    {
        CHECK(history_card(json::parse(R"({"id":"a","sent_name":"Cube.gcode","file":"a.gcode"})"), "", true, false)["title"] == "Cube");
        CHECK(history_card(json::parse(R"({"id":"a","file":"Cube_plate_3.gcode.3mf"})"), "", true, false)["title"] == "Cube_plate_3");
    }
    SECTION("an unknown plate is 0, an unknown mode an upload, an unknown source the desktop")
    {
        const json c = history_card(json::parse(R"({"id":"a","file":"a.gcode","plate":-1,"mode":"weird","source":"x"})"), "", true, false);
        CHECK(c["plate"] == 0);
        CHECK(c["mode"] == "upload");
        CHECK(c["source"] == "desktop");
    }
    SECTION("wrong types do not throw")
    {
        const json c = history_card(json::parse(R"({"id":5,"file":["x"],"time":"soon","plate":"2","filaments":"red","reprints":{}})"), "P", false, false);
        CHECK(c["id"] == "");
        CHECK(c["time"] == 0);
        CHECK(c["plate"] == 0);
        CHECK(c["filaments"].empty());
        CHECK(c["reprints"] == 0);
    }
    SECTION("a missing file cannot be opened, a file of another kind neither")
    {
        CHECK(history_card(json::parse(R"({"id":"a","file":"a.gcode"})"), "", false, false)["can_open"] == false);
        CHECK(history_card(json::parse(R"({"id":"a","file":"a.bgcode"})"), "", true, false)["can_open"] == false);
    }
    SECTION("not an object")
    {
        const json c = history_card(json::array(), "", false, false);
        CHECK(c["id"] == "");
        CHECK(c["filaments"].empty());
    }
}

TEST_CASE("home tab: the page may only act on what it was given", "[HomeTab]")
{
    const std::vector<std::string> listed{"/home/me/a.3mf", "C:\\Users\\me\\b.3mf"};
    CHECK(is_listed("/home/me/a.3mf", listed));
    CHECK(is_listed("C:\\Users\\me\\b.3mf", listed));
    CHECK_FALSE(is_listed("", listed));
    CHECK_FALSE(is_listed("/home/me/../me/a.3mf", listed));
    CHECK_FALSE(is_listed("/etc/passwd", listed));
    CHECK_FALSE(is_listed("/home/me/a.3mf", {}));
}

TEST_CASE("home tab: base64 and data URIs", "[HomeTab]")
{
    CHECK(base64("") == "");
    CHECK(base64("f") == "Zg==");
    CHECK(base64("fo") == "Zm8=");
    CHECK(base64("foo") == "Zm9v");
    CHECK(base64("foob") == "Zm9vYg==");
    CHECK(base64("fooba") == "Zm9vYmE=");
    CHECK(base64("foobar") == "Zm9vYmFy");
    CHECK(base64(std::string("\x89PNG\r\n\x1a\n", 8)) == "iVBORw0KGgo=");
    CHECK(base64(std::string("\xff\xfe\x00", 3)) == "//4A");
    CHECK(png_data_uri("").empty());
    CHECK(png_data_uri("foo") == "data:image/png;base64,Zm9v");
}

TEST_CASE("home tab: the script handing data to the page", "[HomeTab]")
{
    const std::string s = receive_script({{"type", "recent"}, {"name", "a\u2028b</script>\"'"}});
    CHECK(s.rfind("window.HomeApp && window.HomeApp.receive(", 0) == 0);
    CHECK(s.substr(s.size() - 2) == ");");
    CHECK(s.find("\\u2028") != std::string::npos);
    for (unsigned char c : s)
        CHECK(c < 0x80); // ASCII only
    // Invalid UTF-8 in a file name must not throw.
    CHECK_NOTHROW(receive_script({{"name", std::string("bad\xff", 4)}}));
}
