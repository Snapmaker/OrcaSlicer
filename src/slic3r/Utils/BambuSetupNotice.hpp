#pragma once

// The first-time Bambu printer setup notice: when it shows, and what its status lines say.
//
// Support keeps answering the same questions about Bambu printers that never appear or never
// connect (sign in, firewall, LAN Only mode, access code, SD card, same network). The notice
// lists them once, the first time a Bambu Lab printer is added or selected. Everything that
// decides something is pure so slic3rutils_tests can pin it; the dialog (GUI/BambuSetupNoticeDialog)
// only gathers the inputs and draws the answers.

#include "WinFirewall.hpp"

#include <cstddef>

namespace Slic3r {
namespace BambuSetup {

// app_config key (section "app"): set once the person dismissed the notice with "Don't show this again".
extern const char* const CONFIG_KEY; // "bambu_setup_notice_shown"

// What made the notice a candidate.
enum class Trigger {
    PresetSelected,  // the printer preset was switched by hand to a Bambu Lab (BBL vendor) preset
    WizardFinished,  // the setup wizard closed with a Bambu Lab printer selected
    DeviceTabOpened, // the Device tab has been open for a while on a Bambu printer
};

struct ShowInputs
{
    Trigger trigger { Trigger::PresetSelected };
    bool    is_bbl_vendor { false };       // the selected printer preset is a Bambu Lab one
    bool    already_dismissed { false };   // app_config bambu_setup_notice_shown
    bool    shown_this_session { false };  // popped up already since this launch (user unticked "don't show")
    bool    main_window_ready { false };   // the main frame exists, is shown and the app has finished starting
    size_t  devices_found { 0 };           // printers the Device tab lists (only read for DeviceTabOpened)
};

// Whether the notice should pop up now. Never for a non-Bambu printer, never once dismissed, never
// twice in one session, never before the main window is up; the Device tab only when it found nothing.
bool should_show(const ShowInputs& in);

// A status line next to an item. Unknown means "could not tell" and shows no verdict.
enum class Status { Unknown, Ok, Attention };

// Windows Firewall as it concerns printer discovery (UDP 2021/1990): Attention when a Block rule,
// a missing allow rule or "block all incoming" drops it on a live network. Unknown when the
// firewall could not be read (or off Windows).
Status firewall_status(const WinFirewall::Diagnosis& d);

// The network profile: Attention on a Public network (Windows blocks discovery there by default),
// Ok on Private/Domain, Unknown when no network is live or the firewall could not be read.
Status network_status(const WinFirewall::Diagnosis& d);

} // namespace BambuSetup
} // namespace Slic3r
