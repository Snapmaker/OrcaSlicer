// Which FlashForge printers the Device tab takes from the user's print-host settings, and how the
// same printer appearing more than once becomes one tile (slic3r/GUI/FlashForge/FFPrinterSources).
//
// Pure data in, pure data out: no config, no wx, no printer, no network.

#include <catch2/catch.hpp>

#include "slic3r/GUI/FlashForge/FFPrinterSources.hpp"

using namespace Slic3r::GUI;

namespace {

FFPrinterSource src(const std::string& name, const std::string& address, const std::string& serial, const std::string& code,
                    const std::string& origin = "m/1")
{
    FFPrinterSource s;
    s.name       = name;
    s.address    = address;
    s.serial     = serial;
    s.check_code = code;
    s.origin     = origin;
    return s;
}

} // namespace

TEST_CASE("a complete FlashForge entry is a Ready tile keyed by its serial", "[FFPrinterSources]")
{
    const auto out = ff_merge_printer_sources({ src("Bench C5", "192.168.1.50", "SNFF123", "abc123") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].key == "SNFF123");
    CHECK(out[0].state == FFPrinterState::Ready);
    CHECK(out[0].ip == "192.168.1.50");
    CHECK(out[0].port == 8898);
    CHECK(out[0].check_code == "abc123");
    CHECK(out[0].missing.empty());
}

TEST_CASE("addresses: scheme, path and port are understood, a host name is not an IP", "[FFPrinterSources]")
{
    std::string    ip;
    unsigned short port = 0;
    CHECK(ff_parse_address(" http://10.0.0.7:8898/detail ", ip, port));
    CHECK(ip == "10.0.0.7");
    CHECK(port == 8898);
    CHECK(ff_parse_address("10.0.0.7:9000", ip, port));
    CHECK(port == 9000);
    CHECK(ff_parse_address("10.0.0.7:8899", ip, port)); // the old console port is not the API's
    CHECK(port == 8898);
    CHECK_FALSE(ff_parse_address("creator5.local", ip, port));
    CHECK_FALSE(ff_parse_address("10.0.0", ip, port));
    CHECK_FALSE(ff_parse_address("300.1.1.1", ip, port));
    CHECK_FALSE(ff_parse_address("", ip, port));
}

TEST_CASE("missing fields make a NeedsSetup tile that says what is missing", "[FFPrinterSources]")
{
    SECTION("no check code")
    {
        const auto out = ff_merge_printer_sources({ src("C5", "192.168.1.50", "SNFF123", "") });
        REQUIRE(out.size() == 1);
        CHECK(out[0].state == FFPrinterState::NeedsSetup);
        CHECK(out[0].missing_text() == "check code");
        CHECK(out[0].key == "SNFF123");
    }
    SECTION("no serial and no check code")
    {
        const auto out = ff_merge_printer_sources({ src("C5", "192.168.1.50", "", "") });
        REQUIRE(out.size() == 1);
        CHECK(out[0].missing_text() == "serial number and check code");
        CHECK(out[0].key == "addr:192.168.1.50");
    }
    SECTION("a host name instead of an IP")
    {
        const auto out = ff_merge_printer_sources({ src("C5", "c5.local", "SNFF1", "x") });
        REQUIRE(out.size() == 1);
        CHECK(out[0].state == FFPrinterState::NeedsSetup);
        CHECK(out[0].ip.empty());
    }
    SECTION("nothing at all is not a printer")
    {
        CHECK(ff_merge_printer_sources({ src("", "", "", "") }).empty());
    }
}

TEST_CASE("the same serial from two settings entries is one tile", "[FFPrinterSources]")
{
    const std::vector<FFPrinterSource> a = { src("Old entry", "192.168.1.50", "SNFF1", "", "model1/a"),
                                             src("Good entry", "192.168.1.51", "SNFF1", "code", "model2/b") };
    const std::vector<FFPrinterSource> b = { a[1], a[0] }; // the other order

    for (const auto* list : { &a, &b }) {
        const auto out = ff_merge_printer_sources(*list);
        REQUIRE(out.size() == 1);
        CHECK(out[0].state == FFPrinterState::Ready); // the complete entry wins
        CHECK(out[0].ip == "192.168.1.51");
    }
}

TEST_CASE("equally complete entries for one serial: the answer does not depend on the order", "[FFPrinterSources]")
{
    const FFPrinterSource x = src("Alpha", "10.0.0.1", "S", "c", "o1");
    const FFPrinterSource y = src("Beta", "10.0.0.2", "S", "c", "o2");
    CHECK(ff_merge_printer_sources({ x, y })[0].ip == "10.0.0.1");
    CHECK(ff_merge_printer_sources({ y, x })[0].ip == "10.0.0.1");
}

TEST_CASE("an entry without a serial at a known printer's address is that printer", "[FFPrinterSources]")
{
    const auto out = ff_merge_printer_sources({ src("Typed by IP", "192.168.1.50", "", "code"),
                                                src("Bench C5", "192.168.1.50", "SNFF123", "code") });
    REQUIRE(out.size() == 1);
    CHECK(out[0].key == "SNFF123");

    // At another address it is a different printer, still waiting for its serial number.
    const auto two = ff_merge_printer_sources({ src("Other", "192.168.1.60", "", "code"),
                                                src("Bench C5", "192.168.1.50", "SNFF123", "code") });
    CHECK(two.size() == 2);
}

TEST_CASE("two printers stay two tiles, sorted by name", "[FFPrinterSources]")
{
    const auto out = ff_merge_printer_sources({ src("zeta", "10.0.0.2", "S2", "c"), src("Alpha", "10.0.0.1", "S1", "c") });
    REQUIRE(out.size() == 2);
    CHECK(out[0].name == "Alpha");
    CHECK(out[1].name == "zeta");
}

TEST_CASE("a tile the settings stop asking for goes, unless Add printer saved it", "[FFPrinterSources]")
{
    const auto now = ff_merge_printer_sources({ src("Bench C5", "10.0.0.1", "S1", "c") });

    // S2 was a settings tile and is gone from the settings; S3 too, but the user saved it themselves.
    const auto stale = ff_stale_setting_tiles({ "S1", "S2", "S3" }, now, { "S3" });
    REQUIRE(stale.size() == 1);
    CHECK(stale[0] == "S2");

    // An edit that changes the serial retires the old key and adds the new one.
    const auto edited = ff_merge_printer_sources({ src("Bench C5", "10.0.0.1", "S1-NEW", "c") });
    const auto gone   = ff_stale_setting_tiles({ "S1" }, edited, {});
    REQUIRE(gone.size() == 1);
    CHECK(gone[0] == "S1");
}
