// Reprint for every printer the hub can send to, not only Bambu (2026-09-25).
//
// The owner's Reprint list held Bambu jobs and nothing else. Three causes, each covered here:
//  - the U1 pre-print page moved to sw_GetPrintZip / sw_StartLocalPrint and stopped reaching the two
//    hooks that archived its sends, and what it did archive was named "connect" (the cloud broker)
//    rather than the printer's LAN card, so the phone could not send it again (match_host);
//  - a record sent to a print-host device ("ph:<id>") was classed as a Bambu printer and refused
//    (printer_kind_of);
//  - an upload made to start later had no way to become a print, and a reprint always uploaded the
//    file again even when the printer still had it (set_mode_in, file_on_printer, against a fake
//    Moonraker in the hidden end-to-end case).

#include <catch2/catch.hpp>

#include "slic3r/GUI/GcodeArchive.hpp"
#include "slic3r/GUI/RemoteSend.hpp"
#include "slic3r/GUI/SnapmakerLan.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

SnapmakerLan::Device lan(const std::string& id, const std::string& ip, int port = 80)
{
    SnapmakerLan::Device d;
    d.id    = id;
    d.name  = "U1-" + id;
    d.model = "Snapmaker U1";
    d.ip    = ip;
    d.port  = port;
    return d;
}

json read_json(const fs::path& p)
{
    boost::nowide::ifstream f(p.string().c_str(), std::ios::binary);
    std::stringstream       ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}

void write_text(const fs::path& p, const std::string& text)
{
    boost::nowide::ofstream f(p.string().c_str(), std::ios::binary);
    f << text;
}

} // namespace

TEST_CASE("a print-host device is a print host, not a Bambu printer", "[ReprintHosts]")
{
    CHECK(RemoteSend::printer_kind_of("ph:3f2a9c") == "printhost");
    CHECK(RemoteSend::printer_kind_of("host") == "printhost");
    CHECK(RemoteSend::printer_kind_of("sm:8110025111600047BIY2") == "snapmaker");
    CHECK(RemoteSend::printer_kind_of("connect") == "connect");
    CHECK(RemoteSend::printer_kind_of("0938AC571101137") == "bambu");
}

TEST_CASE("the Device tab's connection address finds the printer's LAN card", "[ReprintHosts]")
{
    const std::vector<SnapmakerLan::Device> list = { lan("SN-U1", "10.0.0.108"), lan("SN-U2", "10.0.0.106"),
                                                     lan("SN-U3", "U3.local") };
    SnapmakerLan::Device out;
    SECTION("an IP with the MQTT port")
    {
        REQUIRE(SnapmakerLan::match_host(list, "10.0.0.106:1884", out));
        CHECK(out.id == "SN-U2");
    }
    SECTION("a URL")
    {
        REQUIRE(SnapmakerLan::match_host(list, "mqtt://10.0.0.108:8883", out));
        CHECK(out.id == "SN-U1");
    }
    SECTION("a host name, in any case")
    {
        REQUIRE(SnapmakerLan::match_host(list, "u3.LOCAL:1884", out));
        CHECK(out.id == "SN-U3");
    }
    SECTION("the cloud broker is nobody's LAN card")
    {
        CHECK_FALSE(SnapmakerLan::match_host(list, "a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883", out));
    }
    SECTION("an address no card has")
    {
        CHECK_FALSE(SnapmakerLan::match_host(list, "10.0.0.99:1884", out));
        CHECK_FALSE(SnapmakerLan::match_host(list, "", out));
    }
}

TEST_CASE("an upload record becomes a print once it is started", "[ReprintHosts]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("edgeslicer-archive-test-%%%%%%%%");
    fs::create_directories(dir);
    write_text(dir / "20260925-2210_U1_cube.json",
               json({ { "id", "20260925-2210_U1_cube" }, { "time", 1790392000 }, { "file", "20260925-2210_U1_cube.gcode" },
                      { "mode", "upload" }, { "printer", { { "id", "sm:SN-U1" }, { "kind", "snapmaker" } } } })
                   .dump(2));

    REQUIRE(GcodeArchive::set_mode_in(dir.string(), "20260925-2210_U1_cube", "print", "gcodes/cube.gcode"));
    const json j = read_json(dir / "20260925-2210_U1_cube.json");
    CHECK(j["mode"] == "print");
    CHECK(j["remote_path"] == "gcodes/cube.gcode");
    CHECK(j["printer"]["id"] == "sm:SN-U1"); // nothing else moved

    CHECK_FALSE(GcodeArchive::set_mode_in(dir.string(), "20260925-2210_U1_cube", "delete"));
    CHECK_FALSE(GcodeArchive::set_mode_in(dir.string(), "no_such_record", "print"));
    CHECK_FALSE(GcodeArchive::set_mode_in(dir.string(), "../20260925-2210_U1_cube", "print"));

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// ------------------------------------------------- end to end (hidden) ----
//
// Against tests/slic3rutils/fake_moonraker.py on loopback:
//   python fake_moonraker.py --port 18201 --control 18202
//   FAKE_MOONRAKER_A=18201:18202 slic3rutils_tests "[SnapmakerSendE2E]"

namespace {

bool fake_ports(int& port, int& control)
{
    const char* v = std::getenv("FAKE_MOONRAKER_A");
    return v && std::sscanf(v, "%d:%d", &port, &control) == 2 && port > 0 && control > 0;
}

json fake_get(int control, const std::string& what)
{
    std::string body;
    Slic3r::Http::get("http://127.0.0.1:" + std::to_string(control) + "/fake/" + what)
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([&body](std::string b, unsigned) { body = b; })
        .perform_sync();
    REQUIRE_FALSE(body.empty());
    return json::parse(body);
}

} // namespace

TEST_CASE("a file the printer still has is started in place, not uploaded again", "[.][SnapmakerSendE2E]")
{
    int port = 0, control = 0;
    REQUIRE(fake_ports(port, control));
    fake_get(control, "reset");
    SnapmakerLan::Device d = lan("E2E-SEND", "127.0.0.1", port);

    const fs::path src = fs::temp_directory_path() / fs::unique_path("edgeslicer-send-%%%%%%%%.gcode");
    write_text(src, std::string(4096, ';'));
    std::string error;

    INFO("nothing there yet: a reprint must upload");
    CHECK_FALSE(SnapmakerLan::file_on_printer(d, "cube.gcode", 4096));

    INFO("upload only, as a send 'to start later' does");
    REQUIRE(SnapmakerLan::upload(d, src.string(), "cube.gcode", nullptr, error));
    CHECK(fake_get(control, "files")["uploads"] == 1);

    INFO("the same name and size is the same file; another size is not");
    CHECK(SnapmakerLan::file_on_printer(d, "cube.gcode", 4096));
    CHECK(SnapmakerLan::file_on_printer(d, "cube.gcode", 0));
    CHECK_FALSE(SnapmakerLan::file_on_printer(d, "cube.gcode", 4095));
    CHECK_FALSE(SnapmakerLan::file_on_printer(d, "other.gcode", 4096));

    INFO("started in place: the printer runs it and nothing was uploaded a second time");
    REQUIRE(SnapmakerLan::start_print(d, "cube.gcode", error));
    const json files = fake_get(control, "files");
    CHECK(files["uploads"] == 1);
    REQUIRE(files["started"].size() == 1);
    CHECK(files["started"][0] == "cube.gcode");
    const SnapmakerLan::Status st = SnapmakerLan::status_now(d);
    CHECK(st.state == "printing");
    CHECK(st.filename == "cube.gcode");

    INFO("a file that is not there cannot be started");
    CHECK_FALSE(SnapmakerLan::start_print(d, "missing.gcode", error));

    boost::system::error_code ec;
    fs::remove(src, ec);
}
