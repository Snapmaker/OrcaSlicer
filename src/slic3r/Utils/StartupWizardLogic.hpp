#ifndef slic3r_Utils_StartupWizardLogic_hpp_
#define slic3r_Utils_StartupWizardLogic_hpp_

// When the setup wizard opens by itself at startup (GUI_App::config_wizard_startup).
//
// Kept free of wxWidgets so tests/slic3rutils/startup_wizard_tests.cpp can cover it.

#include <string>

namespace Slic3r {
namespace StartupWizard {

enum class Reason {
    None,          // start normally
    NoConfig,      // first run: no EdgeSlicer.conf yet
    NoPrinter,     // only the built-in default printer is installed
    NeverFinished, // a config from an older version whose user never got through the wizard
};

// The "firstguide" / "finish" value as stored in EdgeSlicer.conf. The wizard has written it as
// "true" (GuideFrame::SaveProfile's set(..., "1") bound AppConfig's bool overload) and, in older
// builds and hand-edited configs, as "1"; both mean the wizard was finished. Read from the
// section directly rather than through AppConfig::get_bool, which before Orca #16092 looked for
// the "1" form in the "app" section instead of the one asked for.
inline bool finish_flag_set(const std::string &finish_value)
{
    return finish_value == "true" || finish_value == "1";
}

// conf_existed:          EdgeSlicer.conf was there before this run (GUI_App::m_app_conf_exists).
// only_default_printers: PresetCollection::only_default_printers() for the printers.
// privacy_flag:          app section "privacy_policy_isagree", "" when never written.
// setup_finished:        finish_flag_set(app_config->get("firstguide", "finish")).
//
// The privacy flag used to be written by the wizard's CEIP page, so an empty flag meant "this
// user never finished the wizard". EdgeSlicer dropped that page (5941300f5e, 2.4.0.0) and the
// wizard no longer writes the flag, so on an install that started on 2.4.0.0 or later it stays
// empty for good and the wizard came back at every launch. The flag still counts, but only
// together with a wizard that was never finished.
inline Reason reason_to_run(bool conf_existed, bool only_default_printers, const std::string &privacy_flag, bool setup_finished)
{
    if (!conf_existed)
        return Reason::NoConfig;
    if (only_default_printers)
        return Reason::NoPrinter;
    if (privacy_flag.empty() && !setup_finished)
        return Reason::NeverFinished;
    return Reason::None;
}

inline const char *reason_name(Reason r)
{
    switch (r) {
    case Reason::NoConfig: return "no config file yet";
    case Reason::NoPrinter: return "only the default printer is installed";
    case Reason::NeverFinished: return "setup was never finished";
    default: return "none";
    }
}

} // namespace StartupWizard
} // namespace Slic3r

#endif // slic3r_Utils_StartupWizardLogic_hpp_
