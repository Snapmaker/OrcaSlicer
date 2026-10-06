#include <catch2/catch.hpp>

#include <string>

#include "slic3r/Utils/StartupWizardLogic.hpp"
#include "slic3r/Utils/ToolbarScaleLogic.hpp"

using namespace Slic3r;
using StartupWizard::Reason;
using StartupWizard::reason_to_run;

TEST_CASE("Startup wizard: first run and printer-less configs still open it", "[StartupWizard]")
{
    CHECK(reason_to_run(false, false, "", false) == Reason::NoConfig);
    CHECK(reason_to_run(false, true, "true", true) == Reason::NoConfig);
    CHECK(reason_to_run(true, true, "true", true) == Reason::NoPrinter);
}

TEST_CASE("Startup wizard: a finished setup without the old privacy flag does not reopen it", "[StartupWizard]")
{
    // 2.4.0.0+ installs: the wizard no longer writes privacy_policy_isagree, so the flag stays
    // empty after a completed setup. This is what re-ran the wizard at every launch.
    CHECK(reason_to_run(true, false, "", true) == Reason::None);
    // Older configs that have the flag, either way.
    CHECK(reason_to_run(true, false, "true", true) == Reason::None);
    CHECK(reason_to_run(true, false, "false", true) == Reason::None);
    CHECK(reason_to_run(true, false, "false", false) == Reason::None);
}

TEST_CASE("Startup wizard: a config whose setup was never finished still gets it", "[StartupWizard]")
{
    CHECK(reason_to_run(true, false, "", false) == Reason::NeverFinished);
    CHECK(std::string(StartupWizard::reason_name(Reason::NeverFinished)) == "setup was never finished");
    CHECK(std::string(StartupWizard::reason_name(Reason::None)) == "none");
}

TEST_CASE("3D toolbar auto scale compares logical with logical", "[ToolbarScale]")
{
    const float retina = 2.0f;
    // Stored scale at half the fit on a Retina canvas: the old test, |fit - stored * retina|,
    // saw no change and the toolbar stayed half size. It must be seen as a change.
    const float fit = 0.9f, stored = 0.45f;
    CHECK_FALSE(std::fabs(fit - stored * retina) > 0.05f); // the old comparison (the bug)
    CHECK(ToolbarScale::auto_scale_changed(stored, fit));
    // At the fit (or within 5 %) nothing is rewritten.
    CHECK_FALSE(ToolbarScale::auto_scale_changed(0.9f, 0.9f));
    CHECK_FALSE(ToolbarScale::auto_scale_changed(0.9f, 0.93f));
    CHECK(ToolbarScale::auto_scale_changed(0.9f, 0.96f));
}
