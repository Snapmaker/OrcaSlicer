#include "BambuSetupNoticeDialog.hpp"

#include "FirewallCheckDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Monitor.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/BambuSetupNotice.hpp"

#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/window.h>

namespace Slic3r {
namespace GUI {

namespace {

const int TEXT_WIDTH = 560; // DIP

// ---- when the notice pops up -------------------------------------------------------------------

bool g_shown_this_session = false;

// Another modal dialog on top (the wizard, a preset-save prompt...): the notice waits its turn.
bool other_modal_open()
{
    wxWindow* active = wxGetActiveWindow();
    while (active != nullptr) {
        if (auto* dlg = dynamic_cast<wxDialog*>(active); dlg != nullptr && dlg->IsModal())
            return true;
        active = active->GetParent();
    }
    return false;
}

BambuSetup::ShowInputs gather_inputs(BambuSetup::Trigger trigger)
{
    BambuSetup::ShowInputs in;
    GUI_App& app = wxGetApp();
    in.trigger            = trigger;
    in.is_bbl_vendor      = app.preset_bundle != nullptr && app.preset_bundle->is_bbl_vendor();
    in.already_dismissed  = app.app_config != nullptr && app.app_config->get_bool(BambuSetup::CONFIG_KEY);
    in.shown_this_session = g_shown_this_session;
    // No modal popup while the app is still starting: the main frame must exist, be on screen and
    // the startup must be over (the startup wizard's own follow-up work runs first).
    in.main_window_ready = app.initialized() && app.mainframe != nullptr && app.mainframe->IsShown() && !app.mainframe->IsIconized() &&
                           !other_modal_open();
    if (trigger == BambuSetup::Trigger::DeviceTabOpened) {
        if (DeviceManager* dev = app.getDeviceManager())
            in.devices_found = dev->get_my_machine_list().size() + dev->get_local_machine_list().size();
    }
    return in;
}

const char* trigger_name(BambuSetup::Trigger t)
{
    switch (t) {
    case BambuSetup::Trigger::PresetSelected: return "printer preset selected";
    case BambuSetup::Trigger::WizardFinished: return "setup wizard finished";
    case BambuSetup::Trigger::DeviceTabOpened: return "Device tab opened with no printers";
    }
    return "";
}

// One pending trigger at a time. The timer gives the UI a moment to settle (the preset change and
// the wizard's own follow-up work finish first), re-tries every second while the main window is not
// ready yet (startup) or another modal is open, and gives up after a minute.
class NoticeScheduler : public wxTimer
{
public:
    static NoticeScheduler& get()
    {
        // Never destroyed: a wxTimer must not outlive wx, and the process is ending anyway.
        static NoticeScheduler* s = new NoticeScheduler();
        return *s;
    }

    void request(BambuSetup::Trigger trigger, int delay_ms)
    {
        // Cheap early outs, so most selections never start a timer.
        if (wxGetApp().app_config == nullptr || wxGetApp().app_config->get_bool(BambuSetup::CONFIG_KEY) || g_shown_this_session)
            return;
        if (IsRunning() && m_trigger != BambuSetup::Trigger::DeviceTabOpened)
            return; // a selection or wizard trigger is already waiting; the Device tab one is the weaker
        m_trigger  = trigger;
        m_attempts = 0;
        StartOnce(delay_ms);
    }

    void Notify() override
    {
        // Only the trigger for the Device tab depends on the tab still being open.
        if (m_trigger == BambuSetup::Trigger::DeviceTabOpened) {
            MainFrame* mf = wxGetApp().mainframe;
            if (mf == nullptr || mf->m_monitor == nullptr || !mf->m_monitor->IsShownOnScreen())
                return;
        }
        const BambuSetup::ShowInputs in = gather_inputs(m_trigger);
        if (!in.main_window_ready && in.is_bbl_vendor && !in.already_dismissed && !in.shown_this_session && ++m_attempts < 60) {
            StartOnce(1000);
            return;
        }
        if (!BambuSetup::should_show(in))
            return;
        BOOST_LOG_TRIVIAL(info) << "[BambuSetup] showing the first-time notice (" << trigger_name(m_trigger) << ")";
        g_shown_this_session = true;
        BambuSetupNoticeDialog::show_modal(wxGetApp().mainframe);
    }

private:
    BambuSetup::Trigger m_trigger { BambuSetup::Trigger::PresetSelected };
    int                 m_attempts { 0 };
};

wxColour status_colour(BambuSetup::Status s)
{
    return s == BambuSetup::Status::Ok        ? wxColour(0, 150, 136) :
           s == BambuSetup::Status::Attention ? wxColour(230, 126, 0) :
                                                wxColour(107, 107, 107);
}

} // namespace

// ---- triggers ----------------------------------------------------------------------------------

void BambuSetupNoticeDialog::on_printer_preset_selected()
{
    if (wxGetApp().preset_bundle == nullptr || !wxGetApp().preset_bundle->is_bbl_vendor())
        return;
    NoticeScheduler::get().request(BambuSetup::Trigger::PresetSelected, 800);
}

void BambuSetupNoticeDialog::on_wizard_finished()
{
    if (wxGetApp().preset_bundle == nullptr || !wxGetApp().preset_bundle->is_bbl_vendor())
        return;
    NoticeScheduler::get().request(BambuSetup::Trigger::WizardFinished, 800);
}

void BambuSetupNoticeDialog::on_device_tab_shown()
{
    if (wxGetApp().preset_bundle == nullptr || !wxGetApp().preset_bundle->is_bbl_vendor())
        return;
    // Discovery takes a while (10 to 60 s on a healthy network); a short wait still catches a
    // cold start with nothing bound without nagging someone whose printers are about to appear.
    NoticeScheduler::get().request(BambuSetup::Trigger::DeviceTabOpened, 12000);
}

// ---- the dialog --------------------------------------------------------------------------------

void BambuSetupNoticeDialog::show_modal(wxWindow* parent)
{
    BambuSetupNoticeDialog dlg(parent != nullptr ? parent : static_cast<wxWindow*>(wxGetApp().mainframe));
    dlg.ShowModal();
}

BambuSetupNoticeDialog::BambuSetupNoticeDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Bambu printer setup"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const int wrap = FromDIP(TEXT_WIDTH);

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* intro = new ::Label(this, Label::Body_13,
                              _L("A Bambu Lab printer needs a few things in place before it shows up and connects. "
                                 "If yours is missing, go through this list."));
    intro->Wrap(wrap);
    top->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    int number = 0;
    // One item: a numbered heading and one or two short sentences.
    auto add_item = [&](const wxString& title, const wxString& body) {
        ++number;
        top->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        auto* head = new ::Label(this, Label::Head_14, wxString::Format("%d. %s", number, title));
        top->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        auto* text = new ::Label(this, Label::Body_13, body);
        text->Wrap(wrap);
        top->Add(text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    };
    auto add_status = [&](::Label*& slot) {
        slot = new ::Label(this, Label::Body_12, wxEmptyString);
        slot->Wrap(wrap);
        top->Add(slot, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    };

    // 1. Bambu account
    add_item(_L("Bambu account"),
             _L("Sign in with the account button at the top of the window. Cloud printers need it; LAN-only printers can skip it."));
    add_status(m_status_account);

    // 2. Firewall (Windows only: there is no Windows Firewall to check elsewhere)
    if (FirewallCheckDialog::supported()) {
        add_item(_L("Firewall"),
                 _L("Printer discovery needs Windows Firewall to allow EdgeSlicer, and Public networks block it by default."));
        add_status(m_status_firewall);
        add_status(m_status_network);
        m_btn_firewall = new ::Button(this, _L("Check Windows Firewall..."));
        m_btn_firewall->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        top->Add(m_btn_firewall, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        m_btn_firewall->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            FirewallCheckDialog::show_modal(this);
            refresh_status(); // the person may have just fixed the rules
        });
    }

    // 3. LAN mode (menu paths from Bambu's wiki: "How to enable LAN Mode" and "How to enable Developer Mode")
    add_item(_L("Printer in LAN mode"),
             _L("For LAN-only use, turn on LAN Only mode on the printer; newer firmware may also need Developer mode, found in the same menu. "
                "X1, H2 and P2S: Settings > LAN Only. P1: Settings > WLAN > LAN Only mode. A1: Settings > LAN Only Mode (page 3)."));

    // 4. Access code
    add_item(_L("Access code"),
             _L("LAN connections need the printer's access code, shown on the printer's screen on the LAN Only page (under WLAN on the P1 series). "
                "It changes when LAN mode is switched off and on."));

    // 5. SD card (the X1 note is on Bambu's LAN mode wiki page; P1 and A1 have no internal storage)
    add_item(_L("SD card"),
             _L("Most Bambu printers (such as the P1, A1 and X1 series) need a microSD card inserted to receive prints from EdgeSlicer, "
                "and to store timelapses. Some newer models with built-in storage, like the P2S, may not."));

    // 6. Same network
    add_item(_L("Same network"),
             _L("The PC and the printer must be on the same subnet; discovery does not cross VLANs, guest Wi-Fi or mesh client isolation. "
                "If discovery fails, add the printer by its IP address from the printer list on the Device tab."));

    // "Don't show this again" + Got it
    top->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* row      = new wxBoxSizer(wxHORIZONTAL);
    m_cb_dont_show = new ::CheckBox(this);
    m_cb_dont_show->SetValue(true);
    row->Add(m_cb_dont_show, 0, wxALIGN_CENTER_VERTICAL);
    auto* cb_label = new ::Label(this, Label::Body_13, _L("Don't show this again"));
    row->Add(cb_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    top->Add(row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    cb_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { m_cb_dont_show->SetValue(!m_cb_dont_show->GetValue()); });

    auto* dlg_btns = new DialogButtons(this, {"OK"});
    m_btn_ok       = dlg_btns->GetOK();
    m_btn_ok->SetLabel(_L("Got it"));
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        save_flag();
        EndModal(wxID_OK);
    });
    top->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(6));

    // The window's close button counts as "Got it": the checkbox decides whether it comes back.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        save_flag();
        EndModal(wxID_CLOSE);
    });

    SetSizer(top);
    wxGetApp().UpdateDlgDarkUI(this);
    // UpdateDlgDarkUI recolours text for dark mode; the status colours are set after it. This also
    // fits the dialog to its (now filled-in) content.
    refresh_status();
    CentreOnParent();
}

void BambuSetupNoticeDialog::save_flag()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || m_cb_dont_show == nullptr)
        return;
    cfg->set_bool(BambuSetup::CONFIG_KEY, m_cb_dont_show->GetValue());
    cfg->save();
}

void BambuSetupNoticeDialog::refresh_status()
{
    auto set = [](::Label* label, BambuSetup::Status s, const wxString& text) {
        if (label == nullptr)
            return;
        label->SetLabel(text);
        label->SetForegroundColour(status_colour(s));
    };

    const bool signed_in = wxGetApp().is_user_login();
    set(m_status_account, signed_in ? BambuSetup::Status::Ok : BambuSetup::Status::Unknown,
        signed_in ? _L("Status: signed in.") : _L("Status: not signed in (fine for LAN-only printers)."));

    if (m_status_firewall != nullptr) {
        wxBusyCursor busy;
        m_diag = WinFirewall::diagnose_this_copy(); // reads the rules only; no admin rights, nothing changes
        const BambuSetup::Status fw = BambuSetup::firewall_status(m_diag);
        set(m_status_firewall, fw,
            fw == BambuSetup::Status::Ok        ? _L("Status: Windows Firewall lets printer discovery through for this copy.") :
            fw == BambuSetup::Status::Attention ? _L("Status: Windows Firewall is likely hiding your printers. Use the button below.") :
                                                  _L("Status: Windows Firewall could not be checked."));
        const BambuSetup::Status net = BambuSetup::network_status(m_diag);
        wxString profile;
        if (m_diag.ok) {
            wxArrayString parts;
            if (m_diag.current_profiles & WinFirewall::ProfileDomain) parts.Add(_L("Domain"));
            if (m_diag.current_profiles & WinFirewall::ProfilePrivate) parts.Add(_L("Private"));
            if (m_diag.current_profiles & WinFirewall::ProfilePublic) parts.Add(_L("Public"));
            for (size_t i = 0; i < parts.size(); ++i)
                profile += (i == 0 ? wxString() : wxString(", ")) + parts[i];
        }
        set(m_status_network, net,
            net == BambuSetup::Status::Unknown ? _L("Network: not detected.") :
            net == BambuSetup::Status::Attention ?
                wxString::Format(_L("Network: this PC's network is %s. Set your home network to Private (Windows Settings > Network & internet)."), profile) :
                wxString::Format(_L("Network: this PC's network is %s."), profile));
    }
    relayout();
}

void BambuSetupNoticeDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    if (m_btn_firewall != nullptr)
        m_btn_firewall->Rescale();
    if (m_btn_ok != nullptr)
        m_btn_ok->Rescale();
    if (m_cb_dont_show != nullptr)
        m_cb_dont_show->Rescale();
    relayout();
}

void BambuSetupNoticeDialog::relayout()
{
    if (GetSizer() == nullptr)
        return;
    for (::Label* l : { m_status_account, m_status_firewall, m_status_network })
        if (l != nullptr)
            l->Wrap(FromDIP(TEXT_WIDTH));
    GetSizer()->SetSizeHints(this);
    Layout();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
