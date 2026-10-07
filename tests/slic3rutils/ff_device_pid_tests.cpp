// Which FlashForge model the Device tab thinks a connection is (EDGESLICER-6).
//
// A printer added by address + serial + check code, or taken from the Printers list, connects
// with lanDevInfo.pid == 0: nothing told the slicer its model before it answered. The device page
// read only that field for LAN printers, built no temperature rows for "model 0", then filled the
// rows of the default layout - which did not exist - and crashed opening a Creator 5. These pin
// the two rules that keep the page consistent:
//
//  1. resolvePid(): the printer's own detail wins; the id the connection started with stands in
//     until the first detail; nothing known is 0.
//  2. tempLayout(): every model maps to a layout whose rows the page builds, the Creator 5 / 5 Pro
//     to their four-nozzle layouts, and a model this build does not know still gets one.
//
// Nothing here touches a printer, a socket or a window.

#include <catch2/catch.hpp>

#include "slic3r/GUI/FFUtils.hpp"

using namespace Slic3r::GUI;

TEST_CASE("a LAN printer added by address takes its model from the printer's detail", "[FFDevicePid]")
{
    // The crash: started with pid 0, the printer has since said it is a Creator 5.
    CHECK(FFUtils::resolvePid(true, 0, true, C5) == C5);
    CHECK(FFUtils::resolvePid(true, 0, true, C5P) == C5P);
    // Before the first detail there is nothing to go on.
    CHECK(FFUtils::resolvePid(true, 0, false, 0) == 0);
}

TEST_CASE("a scanned or saved LAN printer keeps its id until the detail arrives", "[FFDevicePid]")
{
    CHECK(FFUtils::resolvePid(true, AD5X, false, 0) == AD5X);
    CHECK(FFUtils::resolvePid(true, AD5X, true, 0) == AD5X);
    CHECK(FFUtils::resolvePid(true, AD5X, true, AD5X) == AD5X);
    // A stale saved id loses to what the printer says about itself.
    CHECK(FFUtils::resolvePid(true, ADVENTURER_5M, true, C5) == C5);
    // The "invalid" marker is not an id.
    CHECK(FFUtils::resolvePid(true, OTHER, false, 0) == 0);
}

TEST_CASE("a cloud printer's model comes from its detail only", "[FFDevicePid]")
{
    CHECK(FFUtils::resolvePid(false, 0, true, GUIDER_4) == GUIDER_4);
    CHECK(FFUtils::resolvePid(false, 0, false, 0) == 0);
    // lanDevInfo is zeroed for a cloud connection; whatever is there is not used.
    CHECK(FFUtils::resolvePid(false, AD5X, false, 0) == 0);
    CHECK(FFUtils::resolvePid(false, 0, true, -1) == 0);
}

TEST_CASE("the Creator 5 and 5 Pro get their four-nozzle temperature layouts", "[FFDevicePid]")
{
    CHECK(FFUtils::tempLayout(C5, 4) == FFTempLayout::Nozzles4);
    CHECK(FFUtils::tempLayout(C5P, 4) == FFTempLayout::Nozzles4Chamber);
    // Known models do not depend on the nozzle count (it can be 0 before the first detail).
    CHECK(FFUtils::tempLayout(C5, 0) == FFTempLayout::Nozzles4);
    CHECK(FFUtils::tempLayout(GUIDER_3_ULTRA, 2) == FFTempLayout::Guider3Ultra);
    for (unsigned short pid : {ADVENTURER_5M, ADVENTURER_5M_PRO, GUIDER_4, GUIDER_4_PRO, AD5X, ADVENTURER_A5})
        CHECK(FFUtils::tempLayout(pid, 1) == FFTempLayout::Generic);
}

TEST_CASE("a model this build does not know still gets a temperature layout", "[FFDevicePid]")
{
    CHECK_FALSE(FFUtils::isKnownPid(0));
    CHECK_FALSE(FFUtils::isKnownPid(0x0030));
    CHECK(FFUtils::isKnownPid(C5));
    CHECK(FFUtils::tempLayout(0, 0) == FFTempLayout::Generic);
    CHECK(FFUtils::tempLayout(0x0030, 1) == FFTempLayout::Generic);
    // Four nozzles is the Creator 5 shape, whatever the id.
    CHECK(FFUtils::tempLayout(0x0030, 4) == FFTempLayout::Nozzles4);
}
