#include "AccountStatus.hpp"

#include "BBLTopbar.hpp"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/app.h>

namespace Slic3r {
namespace GUI {
namespace AccountStatus {

namespace {

BBLTopbar* g_topbar = nullptr;

// Whether the selected printer needs the Bambu cloud account: a printer bound to the account that
// is not in LAN-only mode. While signed out the device list is emptied, so this mostly catches a
// session that ended while the printer was still selected; the "signed in before" flag covers the rest.
bool bambu_selected_printer_cloud_bound()
{
    auto& app = wxGetApp();
    if (DeviceManager* dev = app.getDeviceManager()) {
        if (MachineObject* obj = dev->get_selected_machine()) {
            if (obj->is_lan_mode_printer())
                return false;
            return dev->get_user_machine(obj->dev_id) != nullptr;
        }
    }
    // The plug-in still has a bound printer picked, though no device is selected yet.
    if (NetworkAgent* agent = app.getAgent())
        return !agent->get_user_selected_machine().empty();
    return false;
}

bool was_signed_in(const std::string& provider_id)
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg != nullptr && cfg->get("app", Accounts::signed_in_flag_key(provider_id)) == "true";
}

// A sign-in is remembered the first time it is seen, wherever it came from (a sign-in dialog, the
// Snapmaker silent sign-in at startup, a session restored by the plug-in).
void remember_sign_ins()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    for (const Accounts::Provider& p : Accounts::registry().providers()) {
        if (p.is_signed_in() && !was_signed_in(p.id))
            cfg->set("app", Accounts::signed_in_flag_key(p.id), true);
    }
}

wxString tooltip_for(const Accounts::Provider& p)
{
    return wxString::Format(_L("Signed out of your %s account. Cloud printers and some features need it; LAN-only printers work without it."),
                            wxString::FromUTF8(p.display_name));
}

} // namespace

void register_providers()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    Accounts::Registry& reg = Accounts::registry();

    Accounts::Provider bbl;
    bbl.id           = "bbl";
    bbl.display_name = "Bambu Lab";
    bbl.vendor_ids   = {"BBL"};
    bbl.is_signed_in = [] { return wxGetApp().is_user_login(); };
    bbl.selected_printer_cloud_bound = bambu_selected_printer_cloud_bound;
    reg.add(std::move(bbl));

    // Snapmaker's account is for its web pages and model library; no printer is bound to it, so a
    // Snapmaker user is only warned once they have signed in before and got signed out.
    Accounts::Provider sm;
    sm.id           = "snapmaker";
    sm.display_name = "Snapmaker";
    sm.vendor_ids   = {"Snapmaker"};
    sm.is_signed_in = [] {
        auto* info = wxGetApp().sm_get_userinfo();
        return info != nullptr && info->is_user_login();
    };
    reg.add(std::move(sm));

    // Bambu sign-in / sign-out goes through the network agent wrapper (the sign-in dialogs, the OAuth
    // loopback, the Account menu, an expired session); follow it.
    NetworkAgent::set_login_changed_hook([] { refresh_async(); });
}

void attach_topbar(BBLTopbar* topbar) { g_topbar = topbar; }

void detach_topbar(BBLTopbar* topbar)
{
    if (g_topbar == topbar)
        g_topbar = nullptr;
}

std::string current_printer_vendor_id()
{
    auto& app = wxGetApp();
    if (!app.preset_bundle)
        return {};
    // Same lookup as MainFrame::refresh_account_menu: the edited preset's vendor, else its parent's
    // (a user preset has no vendor of its own).
    if (app.preset_bundle->is_bbl_vendor())
        return "BBL";
    const Preset&        pp = app.preset_bundle->printers.get_edited_preset();
    const VendorProfile* v  = pp.vendor;
    if (v == nullptr) {
        if (const Preset* parent = app.preset_bundle->printers.get_selected_preset_parent())
            v = parent->vendor;
    }
    return v != nullptr ? v->id : std::string();
}

Accounts::Status current_status()
{
    register_providers();
    return Accounts::evaluate(Accounts::registry(), current_printer_vendor_id(), was_signed_in);
}

void refresh()
{
    if (g_topbar == nullptr || wxTheApp == nullptr)
        return;
    register_providers();
    remember_sign_ins();
    const Accounts::Status st = current_status();
    g_topbar->SetAccountWarning(st.warn, st.warn && st.provider != nullptr ? tooltip_for(*st.provider) : wxString());
}

void refresh_async()
{
    if (wxTheApp == nullptr)
        return;
    wxTheApp->CallAfter([] { refresh(); });
}

} // namespace AccountStatus
} // namespace GUI
} // namespace Slic3r
