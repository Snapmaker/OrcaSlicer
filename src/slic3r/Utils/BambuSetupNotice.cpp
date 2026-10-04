#include "BambuSetupNotice.hpp"

namespace Slic3r {
namespace BambuSetup {

const char* const CONFIG_KEY = "bambu_setup_notice_shown";

bool should_show(const ShowInputs& in)
{
    if (!in.is_bbl_vendor || in.already_dismissed || in.shown_this_session || !in.main_window_ready)
        return false;
    if (in.trigger == Trigger::DeviceTabOpened)
        return in.devices_found == 0;
    return true;
}

Status firewall_status(const WinFirewall::Diagnosis& d)
{
    if (!d.ok)
        return Status::Unknown;
    return (d.discovery_problem_profiles() != 0 || d.enabled_blocks() != 0) ? Status::Attention : Status::Ok;
}

Status network_status(const WinFirewall::Diagnosis& d)
{
    if (!d.ok || d.current_profiles == 0)
        return Status::Unknown;
    return d.on_public_network() ? Status::Attention : Status::Ok;
}

} // namespace BambuSetup
} // namespace Slic3r
