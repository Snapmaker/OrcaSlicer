#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/SnapTelemetry.hpp"

#include <boost/filesystem/path.hpp>

namespace fs = boost::filesystem;

TEST_CASE("telemetry file fields expose extensions only", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::file_type(fs::path("C:\\Users\\private\\MODEL.STL")) == "stl");
    REQUIRE(Slic3r::SnapTelemetry::file_type(fs::path("model.step")) == "step");
    REQUIRE(Slic3r::SnapTelemetry::file_type(fs::path("no-extension")) == "unknown");

    const std::vector<fs::path> paths = {
        fs::path("C:\\private\\B.STL"),
        fs::path("/private/a.3mf"),
        fs::path("b.stl"),
        fs::path("c.gcode"),
    };
    REQUIRE(Slic3r::SnapTelemetry::file_types(paths) == "3mf,gcode,stl");
}

TEST_CASE("telemetry file size is zero when unavailable", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::file_size_bytes(fs::path("C:\\definitely\\missing\\snap-telemetry-file")) == "0");
    REQUIRE(Slic3r::SnapTelemetry::bool_value(true) == "true");
    REQUIRE(Slic3r::SnapTelemetry::bool_value(false) == "false");
}

TEST_CASE("telemetry list and preset fields use stable values", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::unique_csv({"PLA", "ABS", "PLA", ""}) == "ABS,PLA");
    REQUIRE(Slic3r::SnapTelemetry::filament_types({"PLA", "pla", "ABS", "PLA-CF", "My private blend", "../secret", ""}) ==
            "ABS,PLA,PLA-CF,custom");
    REQUIRE(Slic3r::SnapTelemetry::preset_type(true) == "system");
    REQUIRE(Slic3r::SnapTelemetry::preset_type(false) == "custom");
    REQUIRE(Slic3r::SnapTelemetry::number(15.0001, 2) == "15");
    REQUIRE(Slic3r::SnapTelemetry::number(0.4000001, 2) == "0.4");
}

TEST_CASE("telemetry operation results use stable outcome values", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::operation_result(true) == "success");
    REQUIRE(Slic3r::SnapTelemetry::operation_result(false) == "failed");
    REQUIRE(Slic3r::SnapTelemetry::operation_result(false, true) == "cancelled");
}

TEST_CASE("telemetry network failures are classified without raw messages", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(0) == "");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-18) == "cancelled");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-2100) == "file_not_found");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-2040) == "file_too_large");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-2020) == "upload_check_failed");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-2060) == "timeout");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-2130) == "upload_failed");
    REQUIRE(Slic3r::SnapTelemetry::network_result_reason(-9999) == "failed");
}

TEST_CASE("telemetry firmware results use stable machine codes", "[snaplog][telemetry]")
{
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_result(0) == "success");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_result(-1) == "failed");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(0) == "");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(-1) == "download_failed");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(-2) == "verify_failed");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(-3) == "flash_failed");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(-4) == "printer_busy");
    REQUIRE(Slic3r::SnapTelemetry::firmware_upgrade_reason(-99) == "failed");
}
