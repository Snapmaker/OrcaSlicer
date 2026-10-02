#include "UntrustedSettingsGuard.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "RemoteAccess.hpp"
#include "format.hpp"
#include "GUI.hpp"
#include "PageServerSecurity.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <algorithm>
#include <map>

namespace Slic3r {
namespace GUI {

using namespace Slic3r::untrusted;

namespace {

// Presets the user has themselves: system, user and default ones, never the ones a project or an
// imported file brought in (external / project-embedded).
bool is_own(const Preset &p) { return !p.is_external && !p.is_project_embedded; }

TrustedValues collect_trusted(const PresetBundle &bundle)
{
    TrustedValues t;
    auto add = [&t](const Preset &p) {
        const std::string pp = joined_post_process(p.config);
        if (!pp.empty())
            t.post_process.insert(pp);
        if (const auto *ff = p.config.option<ConfigOptionString>("filename_format"))
            t.filename_format.insert(ff->value);
    };
    for (auto it = bundle.prints.begin(); it != bundle.prints.end(); ++it)
        if (is_own(*it))
            add(*it);
    // The print settings being edited right now count as the user's own even when unsaved.
    const Preset &edited = bundle.prints.get_edited_preset();
    if (is_own(bundle.prints.get_selected_preset()))
        add(edited);
    return t;
}

const PresetCollection *collection_for(const PresetBundle &bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINT: return &bundle.prints;
    case Preset::TYPE_FILAMENT: return &bundle.filaments;
    case Preset::TYPE_PRINTER: return &bundle.printers;
    default: return nullptr;
    }
}

// What a flagged value goes back to: the user's own preset of the same name, else the preset it
// inherits from, else the option's default (nullptr).
const DynamicPrintConfig *baseline_for(const PresetBundle &bundle, Preset::Type type, const std::string &name, const std::string &inherits)
{
    const PresetCollection *coll = collection_for(bundle, type);
    if (coll == nullptr)
        return nullptr;
    for (const std::string &n : {name, inherits}) {
        if (n.empty())
            continue;
        const Preset *p = coll->find_preset(n, false);
        if (p != nullptr && p->name == n && is_own(*p))
            return &p->config;
    }
    return nullptr;
}

std::string config_string(const DynamicPrintConfig &cfg, const char *key)
{
    if (const auto *s = cfg.option<ConfigOptionString>(key))
        return s->value;
    if (const auto *v = cfg.option<ConfigOptionStrings>(key))
        return v->values.empty() ? std::string() : v->values.front();
    return std::string();
}

wxString shorten(const std::string &text)
{
    // At most 6 lines of 160 characters: enough to recognise a script, not a wall of text.
    std::string out;
    size_t      lines = 0, start = 0;
    while (start < text.size() && lines < 6) {
        size_t      nl   = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (line.size() > 160)
            line = line.substr(0, 157) + "...";
        out += (out.empty() ? "" : "\n") + std::string("    ") + line;
        ++lines;
        if (nl == std::string::npos) {
            start = text.size();
            break;
        }
        start = nl + 1;
    }
    if (start < text.size())
        out += "\n    ...";
    return from_u8(out);
}

bool ask_keep(const std::string &file_label, const std::vector<UntrustedSetting> &ask)
{
    wxString what;
    std::map<std::string, bool> seen;
    for (const UntrustedSetting &s : ask) {
        if (seen[std::to_string(int(s.risk)) + s.value])
            continue;
        seen[std::to_string(int(s.risk)) + s.value] = true;
        if (s.risk == SettingRisk::RunsPrograms)
            what += "\n\n" + _L("Post-processing scripts (programs run on this computer):") + "\n" + shorten(s.value);
        else
            what += "\n\n" + _L("Output file name that leaves the output folder:") + "\n" + shorten(s.value);
    }

    const wxString msg =
        format_wxstr(_L("\"%1%\" contains settings that can run programs on this computer or write files outside the "
                        "output folder. EdgeSlicer runs post-processing scripts every time G-code is exported, and for "
                        "Bambu Lab printers every time a plate is sliced."),
                     file_label) +
        what + "\n\n" +
        _L("Keep them only if you trust where this file came from and know what the scripts do. If you remove them, "
           "your own settings are used instead.");

    MessageDialog dlg(nullptr, msg, _L("Untrusted settings in file"), wxYES | wxNO | wxICON_WARNING);
    dlg.SetButtonLabel(wxID_YES, _L("Remove (recommended)"), true);
    dlg.SetButtonLabel(wxID_NO, _L("Keep"));
    // Anything but an explicit "Keep" removes: closing the dialog, and the auto-answer a hidden or
    // phone-driven instance gives (that one never gets here, see below, but stay safe anyway).
    return dlg.ShowModal() == wxID_NO;
}

void notify(const wxString &text)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || plater->get_notification_manager() == nullptr)
        return;
    plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                           NotificationManager::NotificationLevel::WarningNotificationLevel,
                                                           into_u8(text));
}

} // namespace

bool guard_untrusted_settings(const std::string &file_label, DynamicPrintConfig *project_config, std::vector<Preset *> *embedded_presets,
                              bool strip_network)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;
    const TrustedValues trusted = collect_trusted(*bundle);

    struct Target
    {
        DynamicPrintConfig             *config;
        const DynamicPrintConfig       *baseline;
        std::vector<UntrustedSetting>   found;
    };
    std::vector<Target> targets;

    if (project_config != nullptr) {
        Target t{project_config, nullptr, find_untrusted_settings(*project_config, "project settings", trusted)};
        if (!t.found.empty()) {
            // Print settings go back to the print preset the project names, network keys to empty.
            t.baseline = baseline_for(*bundle, Preset::TYPE_PRINT, config_string(*project_config, "print_settings_id"), std::string());
            targets.push_back(std::move(t));
        }
    }
    if (embedded_presets != nullptr)
        for (Preset *p : *embedded_presets) {
            if (p == nullptr)
                continue;
            Target t{&p->config, nullptr, find_untrusted_settings(p->config, p->name, trusted)};
            if (t.found.empty())
                continue;
            t.baseline = baseline_for(*bundle, p->type, p->name, config_string(p->config, "inherits"));
            targets.push_back(std::move(t));
        }
    if (!strip_network)
        for (Target &t : targets)
            t.found.erase(std::remove_if(t.found.begin(), t.found.end(),
                                         [](const UntrustedSetting &s) { return s.risk == SettingRisk::NetworkEndpoint; }),
                          t.found.end());
    targets.erase(std::remove_if(targets.begin(), targets.end(), [](const Target &t) { return t.found.empty(); }), targets.end());
    if (targets.empty())
        return false;

    std::vector<UntrustedSetting> ask;
    for (const Target &t : targets)
        for (const UntrustedSetting &s : t.found)
            if (s.risk != SettingRisk::NetworkEndpoint)
                ask.push_back(s);

    bool keep_asked = false, asked = false;
    if (!ask.empty()) {
        if (RemoteAccess::dialog_mode() == RemoteAccess::Mode::Interactive) {
            keep_asked = ask_keep(file_label, ask);
            asked      = true;
        } else {
            // Nobody is at the PC to ask: never keep.
            RemoteAccess::get().note_attention("Untrusted settings in \"" + file_label + "\"", "removed");
        }
    }

    size_t removed = 0;
    for (Target &t : targets) {
        std::vector<UntrustedSetting> strip;
        for (const UntrustedSetting &s : t.found)
            if (s.risk == SettingRisk::NetworkEndpoint || !keep_asked)
                strip.push_back(s);
        for (const UntrustedSetting &s : strip)
            BOOST_LOG_TRIVIAL(warning) << "Untrusted settings: removed " << s.key << " from " << s.source << " of \"" << file_label << "\"";
        removed += neutralize_settings(*t.config, strip, t.baseline);
    }
    if (keep_asked)
        BOOST_LOG_TRIVIAL(warning) << "Untrusted settings: the user kept " << ask.size() << " flagged setting(s) of \"" << file_label << "\"";
    else if (!ask.empty() && !asked)
        notify(format_wxstr(_L("Post-processing scripts and similar settings from \"%1%\" were removed. Your own settings are used instead."),
                            file_label));
    return removed > 0;
}

void open_project_attachment(const boost::filesystem::path &path)
{
    const std::string utf8 = boost::nowide::narrow(path.wstring());
    Plater           *plater = wxGetApp().plater();
    const std::string dir    = plater != nullptr ? plater->model().get_auxiliary_file_temp_path() : std::string();
    // Testable confinement (follows a last-component symlink out of Auxiliaries). The
    // page-server check below still runs: it resolves Windows junctions / 8.3 names.
    if (dir.empty() || !untrusted::is_safe_attachment_path(boost::filesystem::path(dir), path)) {
        BOOST_LOG_TRIVIAL(warning) << "Refused to open a file that is not one of the project's attachments";
        return;
    }
    std::string key, dir_key;
    if (!page_server::resolve_final_path(utf8, &key, nullptr) || !page_server::resolve_final_path(dir, &dir_key, nullptr) ||
        !page_server::key_is_within(key, dir_key) || key == dir_key) {
        BOOST_LOG_TRIVIAL(warning) << "Refused to open a file that is not one of the project's attachments";
        return;
    }
    if (untrusted::is_safe_attachment_to_launch(boost::nowide::narrow(path.filename().wstring()))) {
        wxLaunchDefaultApplication(path.wstring(), 0);
    } else {
        BOOST_LOG_TRIVIAL(info) << "Project attachment of a type that can run code; showing it in its folder instead";
        desktop_open_any_folderEx(utf8);
    }
}

void install_untrusted_config_filter()
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    bundle->untrusted_config_filter = [](const std::string &source, DynamicPrintConfig &config) {
        std::string label = source;
        const size_t cut  = label.find_last_of("/\\");
        if (cut != std::string::npos)
            label = label.substr(cut + 1);
        guard_untrusted_settings(label, &config, nullptr, /* strip_network */ false);
    };
}

} // namespace GUI
} // namespace Slic3r
