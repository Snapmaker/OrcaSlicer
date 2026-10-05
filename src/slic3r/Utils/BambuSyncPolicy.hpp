#ifndef slic3r_BambuSyncPolicy_hpp_
#define slic3r_BambuSyncPolicy_hpp_

#include <string>

// When EdgeSlicer may ask Bambu Lab's web service (api.bambulab.com, or .cn) for its startup
// resources: the printers/ OTA data the Device tab uses for Bambu printers, and the Bambu network
// plug-in update check. Owner decision on the privacy audit (2026-10, EdgeSlicerSite PR 4): nobody
// who has no Bambu printer and no Bambu login is sent to Bambu at all. Kept free of wx so it can be
// tested on its own (tests/slic3rutils/bambu_sync_policy_tests.cpp); GUI_App::maybe_start_bambu_sync
// gathers the inputs and runs the sync once the plan says so - at startup, or later in the session
// as soon as a Bambu printer or login appears.
namespace Slic3r {
namespace BambuSync {

struct Inputs
{
    // A Bambu Lab (BBL vendor) printer preset among the visible printers: an installed system
    // preset, a user preset based on one, or the selected printer.
    bool bbl_printer_preset = false;
    // A Bambu printer in the Device tab's lists (bound or found on the LAN) or among the saved LAN
    // printers in the app config.
    bool bambu_device = false;
    // Signed in to Bambu Lab's cloud through the network plug-in.
    bool bambu_login = false;
    // Preferences > Stealth mode: no Bambu cloud traffic at all.
    bool stealth_mode = false;
    // A network plug-in is loaded (its version is not 00.00.00.00).
    bool network_plugin = false;
    // That plug-in is UltraNet, EdgeSlicer's own replacement for the Bambu network plug-in.
    bool ultranet_plugin = false;
};

struct Plan
{
    bool        run            = false; // contact Bambu at all
    bool        printer_config = false; // slicer/printer/bbl
    bool        plugin_check   = false; // slicer/plugins/cloud
    std::string reason;                 // for the log line
};

// True when any of the three "set up" signals is present.
bool bambu_set_up(const Inputs& in);

// What the sync may do now. The plug-in check only runs for a plug-in that is not UltraNet (a
// genuine Bambu plug-in kept on purpose): asking Bambu about updates to a plug-in UltraNet
// replaces is useless, and their "force" answer would stage their package in ota/.
Plan plan(const Inputs& in);

} // namespace BambuSync
} // namespace Slic3r

#endif // slic3r_BambuSyncPolicy_hpp_
