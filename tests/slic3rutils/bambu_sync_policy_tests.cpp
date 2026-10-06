#include <catch2/catch.hpp>

#include "slic3r/Utils/BambuSyncPolicy.hpp"

using namespace Slic3r::BambuSync;

// GUI_App::maybe_start_bambu_sync gathers these inputs from the presets, the device lists and the
// network plug-in, and runs PresetUpdater::sync_bambu when the plan says so (privacy audit 2026-10).

TEST_CASE("Bambu sync: nobody with no Bambu printer and no login is sent to Bambu", "[BambuSync]")
{
    Inputs in; // a fresh install: no Bambu preset, device or login
    in.network_plugin  = true;
    in.ultranet_plugin = true;
    CHECK_FALSE(bambu_set_up(in));
    const Plan p = plan(in);
    CHECK_FALSE(p.run);
    CHECK_FALSE(p.printer_config);
    CHECK_FALSE(p.plugin_check);
    CHECK(p.reason == "no Bambu printer or Bambu login");
}

TEST_CASE("Bambu sync: any one of the three signals is enough", "[BambuSync]")
{
    Inputs preset; preset.bbl_printer_preset = true;
    Inputs device; device.bambu_device = true;
    Inputs login;  login.bambu_login = true;
    for (const Inputs& in : { preset, device, login }) {
        CHECK(bambu_set_up(in));
        const Plan p = plan(in);
        CHECK(p.run);
        CHECK(p.printer_config);
    }
    CHECK(plan(login).reason.rfind("Bambu login", 0) == 0);
    CHECK(plan(device).reason.rfind("Bambu printer in the device list", 0) == 0);
    CHECK(plan(preset).reason.rfind("Bambu printer preset", 0) == 0);
}

TEST_CASE("Bambu sync: Stealth mode wins over everything", "[BambuSync]")
{
    Inputs in;
    in.bbl_printer_preset = in.bambu_device = in.bambu_login = true;
    in.network_plugin = true;
    in.stealth_mode   = true;
    const Plan p = plan(in);
    CHECK_FALSE(p.run);
    CHECK_FALSE(p.plugin_check);
    CHECK(p.reason == "stealth mode");
}

TEST_CASE("Bambu sync: the plug-in check never runs for UltraNet", "[BambuSync]")
{
    Inputs in;
    in.bbl_printer_preset = true;
    in.network_plugin     = true;
    in.ultranet_plugin    = true;
    Plan p = plan(in);
    CHECK(p.run);
    CHECK(p.printer_config);
    CHECK_FALSE(p.plugin_check);
    CHECK(p.reason.find("UltraNet") != std::string::npos);

    // No plug-in loaded at all: nothing to check either.
    in.network_plugin  = false;
    in.ultranet_plugin = false;
    p = plan(in);
    CHECK_FALSE(p.plugin_check);

    // A genuine Bambu plug-in kept on purpose (ultranet_keep_foreign_plugin) still gets its check.
    in.network_plugin = true;
    p = plan(in);
    CHECK(p.plugin_check);
}
