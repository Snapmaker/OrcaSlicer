// The FlashForge LAN connection test and diagnostics zip.
//
// Two things here are worth pinning, and both are things a user sees rather than things the code
// merely computes:
//
//  1. The FNET_* error map. FlashForge's FlashNetwork.dll reports every failure as a bare integer.
//     A C5 owner who mistypes one character of the check code gets 1001 and nothing else, and the
//     support thread stalls there. The map turns those integers into sentences, so the mapping -
//     including the fact that an unknown code still produces usable text - is the contract.
//
//  2. The diagnostics zip manifest. Whatever the manifest lists is what leaves the user's machine,
//     and the printer's check code must not be in it. The structure enforces that (the input type
//     has no field for a check code), and these tests hold the structure to it.
//
// Nothing here touches wx, the filesystem, the network, FlashNetwork.dll or a printer.

#include <catch2/catch.hpp>

#include <algorithm>

#include "slic3r/GUI/FlashForge/FFDiagnostics.hpp"

using Slic3r::GUI::FFConnectionTestResult;
using Slic3r::GUI::FFNetInitStage;
using Slic3r::GUI::FFDiagnosticsEntry;
using Slic3r::GUI::FFDiagnosticsInput;
using Slic3r::GUI::ff_diagnostics_file_name;
using Slic3r::GUI::ff_diagnostics_manifest;
using Slic3r::GUI::ff_diagnostics_manifest_is_free_of;
using Slic3r::GUI::ff_diagnostics_summary;
using Slic3r::GUI::ff_flashnetwork_dat_search_paths;
using Slic3r::GUI::ff_flashnetwork_init_error;
using Slic3r::GUI::ff_flashnetwork_init_failed_text;
using Slic3r::GUI::ff_flashnetwork_load_failed_text;
using Slic3r::GUI::ff_loadlibrary_error_hint;
using Slic3r::GUI::ff_flashnetwork_missing_text;
using Slic3r::GUI::ff_flashnetwork_search_paths;
using Slic3r::GUI::ff_test_fully_ok;
using Slic3r::GUI::ff_test_reachable;
using Slic3r::GUI::ff_test_report;
using Slic3r::GUI::ff_test_summary;
using Slic3r::GUI::fnet_error_description;
using Slic3r::GUI::fnet_error_name;
using Slic3r::GUI::fnet_error_text;
using Slic3r::GUI::fnet_succeeded;

namespace {

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A test that got as far as both calls succeeding.
FFConnectionTestResult good_result()
{
    FFConnectionTestResult r;
    r.serial_number     = "SNC5ABC123";
    r.ip                = "192.168.1.42";
    r.port              = 8898;
    r.product_attempted = true;
    r.product_code      = 0; // FNET_OK
    r.detail_attempted  = true;
    r.detail_code       = 0;
    r.product_name      = "Creator 5";
    r.machine_type      = "Creator 5";
    r.firmware_version  = "3.1.2";
    r.mac_address       = "aa:bb:cc:dd:ee:ff";
    r.status            = "ready";
    return r;
}

} // namespace

// -------------------------------------------------------------------------------------------
// The FNET error map
// -------------------------------------------------------------------------------------------

TEST_CASE("fnet error names match the constants in FlashNetwork.h", "[FFDiagnostics]")
{
    CHECK(fnet_error_name(0) == "FNET_OK");
    CHECK(fnet_error_name(-1) == "FNET_ERROR");
    CHECK(fnet_error_name(1) == "FNET_ABORTED_BY_CALLBACK");
    CHECK(fnet_error_name(2) == "FNET_DIVICE_IS_BUSY");
    CHECK(fnet_error_name(3) == "FNET_GCODE_NOT_FOUND");
    CHECK(fnet_error_name(1001) == "FNET_VERIFY_LAN_DEV_FAILED");
    CHECK(fnet_error_name(2001) == "FNET_UNAUTHORIZED");
    CHECK(fnet_error_name(2002) == "FNET_INVALID_VALIDATION");
    CHECK(fnet_error_name(2003) == "FNET_DEVICE_HAS_BEEN_BOUND");
    CHECK(fnet_error_name(3001) == "FNET_CONN_SEND_ERROR");
}

TEST_CASE("the credential rejection is the one that must read plainly", "[FFDiagnostics]")
{
    // 1001 is what a mistyped check code produces, and the whole reason the map exists.
    const std::string text = fnet_error_text(1001);
    CHECK(contains(text, "serial number"));
    CHECK(contains(text, "check code"));
    // It must not read as a network problem - the printer answered.
    CHECK_FALSE(contains(text, "powered on"));
}

TEST_CASE("FNET_ERROR reads as a network problem, not a credential one", "[FFDiagnostics]")
{
    // -1 is the library's catch-all and on a LAN call means nothing answered.
    const std::string text = fnet_error_text(-1);
    CHECK(contains(text, "IP address"));
    CHECK_FALSE(contains(text, "check code"));
}

TEST_CASE("an unknown code still produces usable text", "[FFDiagnostics]")
{
    // A newer FlashNetwork.dll may return codes this build predates. The user must still get a
    // sentence and the number, never an empty string.
    CHECK(fnet_error_name(4242) == "FNET_UNKNOWN");
    CHECK_FALSE(fnet_error_text(4242).empty());
    CHECK(contains(fnet_error_description(4242), "4242"));
    CHECK(contains(fnet_error_description(4242), "FNET_UNKNOWN"));
}

TEST_CASE("every mapped code has non-empty text", "[FFDiagnostics]")
{
    const int codes[] = {0, -1, 1, 2, 3, 1001, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 3001};
    for (int code : codes) {
        CHECK_FALSE(fnet_error_text(code).empty());
        CHECK_FALSE(fnet_error_name(code).empty());
        // The description always quotes the number, so a user can paste it back to us.
        CHECK(contains(fnet_error_description(code), std::to_string(code)));
    }
}

TEST_CASE("only FNET_OK counts as success", "[FFDiagnostics]")
{
    CHECK(fnet_succeeded(0));
    CHECK_FALSE(fnet_succeeded(-1));
    // The regression this guards: FNET_ERROR is -1, so a "ret < 0" test would wave the positive
    // failure codes through as success.
    CHECK_FALSE(fnet_succeeded(1));
    CHECK_FALSE(fnet_succeeded(2));
    CHECK_FALSE(fnet_succeeded(3));
    CHECK_FALSE(fnet_succeeded(1001));
}

// -------------------------------------------------------------------------------------------
// The connection test result
// -------------------------------------------------------------------------------------------

TEST_CASE("a fully successful test reports the product and firmware", "[FFDiagnostics]")
{
    const FFConnectionTestResult r = good_result();
    CHECK(ff_test_reachable(r));
    CHECK(ff_test_fully_ok(r));
    const std::string summary = ff_test_summary(r);
    CHECK(contains(summary, "Reachable: yes"));
    CHECK(contains(summary, "Creator 5"));
    CHECK(contains(summary, "3.1.2"));
}

TEST_CASE("a rejected check code is not reachable and says why", "[FFDiagnostics]")
{
    FFConnectionTestResult r = good_result();
    r.product_code = 1001;
    r.detail_code  = 1001;
    r.product_name.clear();
    r.firmware_version.clear();

    CHECK_FALSE(ff_test_fully_ok(r));
    const std::string summary = ff_test_summary(r);
    CHECK(contains(summary, "FNET_VERIFY_LAN_DEV_FAILED"));
    CHECK(contains(summary, "1001"));
}

TEST_CASE("a printer that answers but will not describe itself is still reachable", "[FFDiagnostics]")
{
    FFConnectionTestResult r = good_result();
    r.detail_code = 2; // busy
    r.firmware_version.clear();

    CHECK(ff_test_reachable(r));
    CHECK_FALSE(ff_test_fully_ok(r));
    const std::string summary = ff_test_summary(r);
    CHECK(contains(summary, "Reachable: yes"));
    CHECK(contains(summary, "FNET_DIVICE_IS_BUSY"));
}

TEST_CASE("a test that could not run says so instead of claiming unreachable", "[FFDiagnostics]")
{
    FFConnectionTestResult r;
    r.unavailable_reason = "FlashNetwork.dll is not installed";
    CHECK_FALSE(ff_test_reachable(r));
    CHECK_FALSE(ff_test_fully_ok(r));
    const std::string summary = ff_test_summary(r);
    CHECK(contains(summary, "could not run"));
    CHECK(contains(summary, "FlashNetwork.dll is not installed"));
    // "Reachable: no" would be a lie - nothing was ever asked.
    CHECK_FALSE(contains(summary, "Reachable: no"));
}

TEST_CASE("the report carries the serial and address but never a check code", "[FFDiagnostics]")
{
    const std::string report = ff_test_report(good_result());
    CHECK(contains(report, "SNC5ABC123"));
    CHECK(contains(report, "192.168.1.42"));
    CHECK(contains(report, "8898"));
    CHECK(contains(report, "not recorded"));
}

// -------------------------------------------------------------------------------------------
// The diagnostics zip manifest
// -------------------------------------------------------------------------------------------

TEST_CASE("the zip is named for the day it was made", "[FFDiagnostics]")
{
    CHECK(ff_diagnostics_file_name("2026-09-11") == "EdgeSlicer_FlashForge_diagnostics_2026-09-11.zip");
}

TEST_CASE("the manifest always carries the summary and the connection test", "[FFDiagnostics]")
{
    FFDiagnosticsInput input;
    input.app_version = "2.3.7.0";
    input.test        = good_result();

    const auto entries = ff_diagnostics_manifest(input);
    REQUIRE(entries.size() >= 2);
    CHECK(entries[0].zip_path == "summary.txt");
    CHECK(entries[0].is_inline);
    CHECK_FALSE(entries[0].inline_text.empty());
    CHECK(entries[1].zip_path == "connection_test.txt");
    CHECK(entries[1].is_inline);
}

TEST_CASE("logs land under their own folders, keeping only their base names", "[FFDiagnostics]")
{
    FFDiagnosticsInput input;
    input.test = good_result();
    input.flashnetwork_logs = {"C:/Users/someone/AppData/Roaming/EdgeSlicer/FlashNetwork/fnet_2026-09-11.log"};
    input.app_logs          = {"C:/Users/someone/AppData/Roaming/EdgeSlicer/log/debug_2026-09-11.log"};

    const auto entries = ff_diagnostics_manifest(input);
    auto has = [&entries](const std::string &zip_path) {
        return std::any_of(entries.begin(), entries.end(),
                           [&zip_path](const FFDiagnosticsEntry &e) { return e.zip_path == zip_path; });
    };
    CHECK(has("flashnetwork/fnet_2026-09-11.log"));
    CHECK(has("log/debug_2026-09-11.log"));

    // The home directory must not become part of the archive layout.
    for (const FFDiagnosticsEntry &entry : entries)
        CHECK_FALSE(contains(entry.zip_path, "someone"));
}

TEST_CASE("two logs with the same base name both survive", "[FFDiagnostics]")
{
    FFDiagnosticsInput input;
    input.test = good_result();
    // The regression this guards: flattening to base names can collide, and silently dropping one
    // of two logs is worse than an ugly name.
    input.app_logs = {"C:/a/debug.log", "C:/b/debug.log"};

    const auto entries = ff_diagnostics_manifest(input);
    const auto log_entries = std::count_if(entries.begin(), entries.end(),
                                           [](const FFDiagnosticsEntry &e) {
                                               return e.zip_path.rfind("log/", 0) == 0;
                                           });
    CHECK(log_entries == 2);

    // ... and the two names are genuinely distinct.
    std::vector<std::string> paths;
    for (const FFDiagnosticsEntry &e : entries)
        paths.push_back(e.zip_path);
    std::sort(paths.begin(), paths.end());
    CHECK(std::adjacent_find(paths.begin(), paths.end()) == paths.end());
}

TEST_CASE("the check code never reaches the manifest", "[FFDiagnostics]")
{
    // The mechanism is structural - FFDiagnosticsInput has nowhere to put a check code - and this
    // is the assertion that keeps it that way if someone adds a field later.
    const std::string check_code = "9KJ3MQ";

    FFDiagnosticsInput input;
    input.app_version           = "2.3.7.0";
    input.flashnetwork_dll_path = "C:/Program Files/EdgeSlicer/FlashNetwork.dll";
    input.flashnetwork_loaded   = true;
    input.test                  = good_result();
    input.flashnetwork_logs     = {"C:/data/FlashNetwork/fnet.log"};
    input.app_logs              = {"C:/data/log/debug.log"};

    const auto entries = ff_diagnostics_manifest(input);
    CHECK(ff_diagnostics_manifest_is_free_of(entries, check_code));

    // The helper must actually be able to find a secret, or the check above proves nothing.
    std::vector<FFDiagnosticsEntry> poisoned = entries;
    FFDiagnosticsEntry leak;
    leak.zip_path    = "leak.txt";
    leak.inline_text = "check code: " + check_code;
    leak.is_inline   = true;
    poisoned.push_back(leak);
    CHECK_FALSE(ff_diagnostics_manifest_is_free_of(poisoned, check_code));
}

TEST_CASE("the summary states the versions and the DLL path", "[FFDiagnostics]")
{
    FFDiagnosticsInput input;
    input.app_version           = "2.3.7.0";
    input.os_description        = "Windows 11";
    input.flashnetwork_version  = "3.0.0";
    input.flashnetwork_dll_path = "C:/Program Files/EdgeSlicer/FlashNetwork.dll";
    input.flashnetwork_loaded   = true;
    input.test                  = good_result();

    const std::string summary = ff_diagnostics_summary(input);
    CHECK(contains(summary, "2.3.7.0"));
    CHECK(contains(summary, "Windows 11"));
    CHECK(contains(summary, "3.0.0"));
    CHECK(contains(summary, "FlashNetwork.dll"));
    CHECK(contains(summary, "Creator 5"));
}

TEST_CASE("a summary made with no DLL loaded says so rather than going blank", "[FFDiagnostics]")
{
    FFDiagnosticsInput input;
    input.app_version         = "2.3.7.0";
    input.flashnetwork_loaded = false;
    input.test.unavailable_reason = "FlashNetwork.dll is not installed";

    const std::string summary = ff_diagnostics_summary(input);
    CHECK(contains(summary, "FlashNetwork loaded : no"));
    CHECK(contains(summary, "not found"));
    CHECK(contains(summary, "could not run"));
}

// -------------------------------------------------------------------------------------------
// Where the library is looked for
// -------------------------------------------------------------------------------------------

TEST_CASE("the executable directory is searched before the data directory", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_search_paths("C:/Program Files/EdgeSlicer", "C:/Users/x/AppData/Roaming/EdgeSlicer");
    REQUIRE(paths.size() == 2);
    // The installed copy wins: it is the one the installer controls and the one that matches the
    // build. A stale hand-placed copy in the data dir must not shadow it.
    CHECK(paths[0] == "C:/Program Files/EdgeSlicer/FlashNetwork.dll");
    CHECK(paths[1] == "C:/Users/x/AppData/Roaming/EdgeSlicer/plugins/FlashNetwork.dll");
}

TEST_CASE("a directory that already ends in a separator does not gain a second one", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_search_paths("C:/app/", "C:/data\\");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "C:/app/FlashNetwork.dll");
    CHECK(paths[1] == "C:/data\\plugins/FlashNetwork.dll");
}

TEST_CASE("an empty directory contributes no candidate", "[FFDiagnostics]")
{
    CHECK(ff_flashnetwork_search_paths("", "").empty());
    CHECK(ff_flashnetwork_search_paths("C:/app", "").size() == 1);
    CHECK(ff_flashnetwork_search_paths("", "C:/data").size() == 1);
}

TEST_CASE("the missing-library message lists every path that was tried", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_search_paths("C:/app", "C:/data");
    const std::string text = ff_flashnetwork_missing_text(paths);
    // An empty Device tab tells a user nothing; the path is what lets them say where they put it.
    CHECK(contains(text, "C:/app/FlashNetwork.dll"));
    CHECK(contains(text, "C:/data/plugins/FlashNetwork.dll"));
    CHECK(contains(text, "Download"));
    CHECK(contains(text, "Locate"));
}

TEST_CASE("the missing-library message survives having no paths at all", "[FFDiagnostics]")
{
    const std::string text = ff_flashnetwork_missing_text({});
    CHECK_FALSE(text.empty());
    CHECK(contains(text, "FlashNetwork"));
}

TEST_CASE("a load failure points at the wrong-architecture case", "[FFDiagnostics]")
{
    const std::string text = ff_flashnetwork_load_failed_text("C:/app/FlashNetwork.dll");
    CHECK(contains(text, "C:/app/FlashNetwork.dll"));
    // The overwhelmingly common cause, and the one a user can check themselves.
    CHECK(contains(text, "32-bit"));
}

// ---------------------------------------------------------------------------------------------
// FLASHNETWORK9.DAT and the init-failure reasons
//
// 2.4.2.0 shipped FlashNetwork.dll without its server-settings file. fnet_initlize returns -1 on a
// missing, empty or wrong-generation file (checked against the shipped 3.4.2 DLL), and the log
// said only "initalize FlashNetwork failed". These pin where the file is looked for, and that each
// failure step names itself.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the server-settings file is looked for in resources/data first, as upstream does", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_dat_search_paths("C:/EdgeSlicer/resources",
                                                        "C:/Users/x/AppData/Roaming/EdgeSlicer/plugins/FlashNetwork.dll",
                                                        "C:/Users/x/AppData/Roaming/EdgeSlicer",
                                                        "FLASHNETWORK9.DAT");
    REQUIRE(paths.size() == 3);
    // Where the installer puts it and where FlashForge's own client reads it.
    CHECK(paths[0] == "C:/EdgeSlicer/resources/data/FLASHNETWORK9.DAT");
    // Beside a hand-placed library.
    CHECK(paths[1] == "C:/Users/x/AppData/Roaming/EdgeSlicer/plugins/FLASHNETWORK9.DAT");
    // An Orca-Flashforge install picked with Locate keeps it in its own resources/data.
    CHECK(paths[2] == "C:/Users/x/AppData/Roaming/EdgeSlicer/plugins/resources/data/FLASHNETWORK9.DAT");
    // <data dir>/plugins/<dat> was already listed as "beside the library" and is not repeated.
}

TEST_CASE("an installed library and a hand-placed settings file are both found", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_dat_search_paths("C:/app/resources", "C:/app/FlashNetwork.dll",
                                                        "C:/data", "FLASHNETWORK9.DAT");
    // <dll dir>/resources/data is <resources>/data here, so it appears once.
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == "C:/app/resources/data/FLASHNETWORK9.DAT");
    CHECK(paths[1] == "C:/app/FLASHNETWORK9.DAT");
    CHECK(paths[2] == "C:/data/plugins/FLASHNETWORK9.DAT");
}

TEST_CASE("a backslash library path and empty inputs are handled", "[FFDiagnostics]")
{
    const auto paths = ff_flashnetwork_dat_search_paths("", "C:\\app\\FlashNetwork.dll", "", "FLASHNETWORK9.DAT");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "C:\\app\\FLASHNETWORK9.DAT");
    CHECK(paths[1] == "C:\\app\\resources/data/FLASHNETWORK9.DAT");

    CHECK(ff_flashnetwork_dat_search_paths("", "", "", "FLASHNETWORK9.DAT").empty());
    CHECK(ff_flashnetwork_dat_search_paths("C:/app/resources", "C:/app/FlashNetwork.dll", "C:/data", "").empty());
}

TEST_CASE("a missing settings file is named as the reason fnet_initlize failed", "[FFDiagnostics]")
{
    const std::string line = ff_flashnetwork_init_error(FFNetInitStage::InitFailed, "C:/app/FlashNetwork.dll",
                                                        "C:/app/resources/data/FLASHNETWORK9.DAT", -1,
                                                        /*dat_found=*/false);
    CHECK(contains(line, "fnet_initlize returned -1"));
    CHECK(contains(line, "FNET_ERROR"));
    CHECK(contains(line, "C:/app/resources/data/FLASHNETWORK9.DAT"));
    CHECK(contains(line, "does not exist"));

    // With the file present the line still carries the code and path, but does not blame the file
    // for being absent.
    const std::string present = ff_flashnetwork_init_error(FFNetInitStage::InitFailed, "C:/app/FlashNetwork.dll",
                                                           "C:/app/resources/data/FLASHNETWORK9.DAT", -1, true);
    CHECK(contains(present, "returned -1"));
    CHECK_FALSE(contains(present, "does not exist"));
}

TEST_CASE("each init failure step names itself", "[FFDiagnostics]")
{
    const std::string load = ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, "C:/app/FlashNetwork.dll",
                                                        "The specified module could not be found", 126);
    CHECK(contains(load, "could not load C:/app/FlashNetwork.dll"));
    CHECK(contains(load, "system error 126"));
    CHECK(contains(load, "Visual C++ runtime"));
    CHECK(contains(load, "The specified module could not be found"));

    const std::string arch = ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, "C:/app/FlashNetwork.dll", "", 193);
    CHECK(contains(arch, "system error 193"));
    CHECK(contains(arch, "64-bit"));

    const std::string sym = ff_flashnetwork_init_error(FFNetInitStage::MissingSymbol, "C:/app/FlashNetwork.dll",
                                                       "fnet_getMqttConfig", 0);
    CHECK(contains(sym, "fnet_getMqttConfig"));
    CHECK(contains(sym, "C:/app/FlashNetwork.dll"));

    const std::string ver = ff_flashnetwork_init_error(FFNetInitStage::BadVersion, "C:/app/FlashNetwork.dll", "2.9.0", 0);
    CHECK(contains(ver, "2.9.0"));
    CHECK(contains(ver, "3.x"));
}

TEST_CASE("unknown LoadLibrary codes get no invented explanation", "[FFDiagnostics]")
{
    CHECK(ff_loadlibrary_error_hint(0).empty());
    CHECK(ff_loadlibrary_error_hint(424242).empty());
    CHECK_FALSE(ff_loadlibrary_error_hint(126).empty());
    const std::string line = ff_flashnetwork_init_error(FFNetInitStage::LoadFailed, "C:/x.dll", "", 424242);
    CHECK(contains(line, "system error 424242"));
    CHECK_FALSE(contains(line, "()"));
}

TEST_CASE("the Device tab shows the init failure reason, not the generic 32-bit guess", "[FFDiagnostics]")
{
    const std::string reason = "fnet_initlize returned -1 (FNET_ERROR) with server settings "
                               "C:/app/resources/data/FLASHNETWORK9.DAT - that file does not exist";
    const std::string text = ff_flashnetwork_init_failed_text("C:/app/FlashNetwork.dll", reason);
    CHECK(contains(text, "C:/app/FlashNetwork.dll"));
    CHECK(contains(text, reason));
    CHECK(contains(text, "FLASHNETWORK9.DAT"));
    CHECK_FALSE(contains(text, "32-bit"));

    // No reason recorded: the older text, which still names the path.
    CHECK(ff_flashnetwork_init_failed_text("C:/app/FlashNetwork.dll", "") ==
          ff_flashnetwork_load_failed_text("C:/app/FlashNetwork.dll"));
}
