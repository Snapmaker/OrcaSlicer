// The first-time Bambu printer setup notice (slic3r/Utils/BambuSetupNotice): when it pops up, and
// what the status lines next to its items say. Pure logic on hand-built inputs; nothing reads the
// real Windows Firewall, the app config or any window.

#include <catch2/catch.hpp>

#include <string>

#include "slic3r/Utils/BambuSetupNotice.hpp"

using namespace Slic3r::BambuSetup;
namespace FW = Slic3r::WinFirewall;

namespace {

// A Bambu printer, notice not dismissed yet, main window up: the case where it should show.
ShowInputs ready(Trigger t)
{
    ShowInputs in;
    in.trigger            = t;
    in.is_bbl_vendor      = true;
    in.already_dismissed  = false;
    in.shown_this_session = false;
    in.main_window_ready  = true;
    in.devices_found      = 0;
    return in;
}

const std::string EXE    = "C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe";
const std::string GO2RTC = "C:\\Program Files\\EdgeSlicer\\resources\\tools\\go2rtc\\go2rtc.exe";

FW::Rule allow(const std::string& name, const std::string& program, int protocol, const std::string& ports, int profiles)
{
    FW::Rule r;
    r.name        = name;
    r.program     = program;
    r.protocol    = protocol;
    r.local_ports = ports;
    r.profiles    = profiles;
    return r;
}

FW::Snapshot snapshot(int current, int allowed_profiles, bool firewall_on = true)
{
    FW::Snapshot s;
    s.ok               = true;
    s.current_profiles = current;
    for (bool& on : s.firewall_on)
        on = firewall_on;
    s.rules = { allow(FW::RULE_HUB, EXE, FW::ProtoTCP, "13640-13659", allowed_profiles),
                allow(FW::RULE_DISCOVERY, EXE, FW::ProtoUDP, "2021,1990", allowed_profiles),
                allow(FW::RULE_WEBRTC, GO2RTC, FW::ProtoUDP, "8555-8574", allowed_profiles),
                allow(FW::RULE_WEBRTC, GO2RTC, FW::ProtoTCP, "8555-8574", allowed_profiles) };
    return s;
}

} // namespace

TEST_CASE("the notice's app_config key is the documented one", "[BambuSetup]")
{
    CHECK(std::string(CONFIG_KEY) == "bambu_setup_notice_shown");
}

TEST_CASE("selecting or finishing the wizard with a Bambu printer shows the notice once", "[BambuSetup]")
{
    for (Trigger t : { Trigger::PresetSelected, Trigger::WizardFinished }) {
        CHECK(should_show(ready(t)));

        // Whatever the trigger, devices already on the Device tab do not matter for these two.
        ShowInputs with_devices = ready(t);
        with_devices.devices_found = 3;
        CHECK(should_show(with_devices));
    }
}

TEST_CASE("a printer that is not a Bambu Lab one never gets the notice", "[BambuSetup]")
{
    for (Trigger t : { Trigger::PresetSelected, Trigger::WizardFinished, Trigger::DeviceTabOpened }) {
        ShowInputs in   = ready(t);
        in.is_bbl_vendor = false;
        CHECK_FALSE(should_show(in));
    }
}

TEST_CASE("once dismissed the notice never comes back", "[BambuSetup]")
{
    for (Trigger t : { Trigger::PresetSelected, Trigger::WizardFinished, Trigger::DeviceTabOpened }) {
        ShowInputs in       = ready(t);
        in.already_dismissed = true;
        CHECK_FALSE(should_show(in));
    }
}

TEST_CASE("an unticked 'don't show again' still shows at most once per session", "[BambuSetup]")
{
    for (Trigger t : { Trigger::PresetSelected, Trigger::WizardFinished, Trigger::DeviceTabOpened }) {
        ShowInputs in        = ready(t);
        in.shown_this_session = true;
        CHECK_FALSE(should_show(in));
    }
}

TEST_CASE("no popup before the main window is up", "[BambuSetup]")
{
    for (Trigger t : { Trigger::PresetSelected, Trigger::WizardFinished, Trigger::DeviceTabOpened }) {
        ShowInputs in       = ready(t);
        in.main_window_ready = false;
        CHECK_FALSE(should_show(in));
    }
}

TEST_CASE("the Device tab only triggers the notice when it found no printers", "[BambuSetup]")
{
    ShowInputs in = ready(Trigger::DeviceTabOpened);
    CHECK(should_show(in));
    in.devices_found = 1;
    CHECK_FALSE(should_show(in));
    in.devices_found = 5;
    CHECK_FALSE(should_show(in));
}

TEST_CASE("the firewall status follows what printer discovery needs", "[BambuSetup]")
{
    // The installer's rules on a Private network: discovery is fine.
    const auto good = FW::diagnose(snapshot(FW::ProfilePrivate, FW::ProfilePrivate | FW::ProfileDomain), EXE, GO2RTC);
    CHECK(firewall_status(good) == Status::Ok);

    // No allow rule at all: Windows drops the broadcasts.
    const auto none = FW::diagnose(snapshot(FW::ProfilePrivate, 0), EXE, GO2RTC);
    CHECK(firewall_status(none) == Status::Attention);

    // Rules for Private only, PC on Public: the usual reason printers never show up.
    const auto on_public = FW::diagnose(snapshot(FW::ProfilePublic, FW::ProfilePrivate | FW::ProfileDomain), EXE, GO2RTC);
    CHECK(firewall_status(on_public) == Status::Attention);

    // The firewall is off on the live network: it is not what blocks us.
    const auto off = FW::diagnose(snapshot(FW::ProfilePrivate, 0, false), EXE, GO2RTC);
    CHECK(firewall_status(off) == Status::Ok);

    // A firewall that could not be read says nothing.
    FW::Snapshot unreadable;
    unreadable.ok    = false;
    unreadable.error = "access denied";
    CHECK(firewall_status(FW::diagnose(unreadable, EXE, GO2RTC)) == Status::Unknown);
}

TEST_CASE("the network status flags Public networks only", "[BambuSetup]")
{
    const int rules = FW::ProfilePrivate | FW::ProfileDomain;
    CHECK(network_status(FW::diagnose(snapshot(FW::ProfilePrivate, rules), EXE, GO2RTC)) == Status::Ok);
    CHECK(network_status(FW::diagnose(snapshot(FW::ProfileDomain, rules), EXE, GO2RTC)) == Status::Ok);
    CHECK(network_status(FW::diagnose(snapshot(FW::ProfilePublic, rules), EXE, GO2RTC)) == Status::Attention);
    // Public with the firewall off for it is not a problem.
    CHECK(network_status(FW::diagnose(snapshot(FW::ProfilePublic, rules, false), EXE, GO2RTC)) == Status::Ok);
    // No live network, or an unreadable firewall: no verdict.
    CHECK(network_status(FW::diagnose(snapshot(0, rules), EXE, GO2RTC)) == Status::Unknown);
    FW::Snapshot unreadable;
    CHECK(network_status(FW::diagnose(unreadable, EXE, GO2RTC)) == Status::Unknown);
}
