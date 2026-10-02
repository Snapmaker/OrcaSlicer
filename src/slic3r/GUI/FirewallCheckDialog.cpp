#include "FirewallCheckDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace Slic3r {
namespace GUI {

namespace {

const int TEXT_WIDTH = 600; // DIP

wxString profile_words(int profiles)
{
    wxArrayString parts;
    if (profiles & WinFirewall::ProfileDomain) parts.Add(_L("Domain"));
    if (profiles & WinFirewall::ProfilePrivate) parts.Add(_L("Private"));
    if (profiles & WinFirewall::ProfilePublic) parts.Add(_L("Public"));
    wxString out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += (i + 1 == parts.size()) ? wxString(" ") + _L("and") + " " : wxString(", ");
        out += parts[i];
    }
    return out;
}

wxString purpose_words(const std::string& purpose)
{
    if (purpose == "discovery") return _L("Printer discovery (UDP 2021, 1990)");
    if (purpose == "hub") return _L("Phone hub (TCP 13640-13659)");
    return _L("Phone camera video, go2rtc.exe (UDP and TCP 8555-8574)");
}

// The live profiles where Windows Firewall is on.
int guarded_profiles(const WinFirewall::Diagnosis& d)
{
    int out = 0;
    for (int bit : { WinFirewall::ProfileDomain, WinFirewall::ProfilePrivate, WinFirewall::ProfilePublic })
        if ((d.current_profiles & bit) && d.firewall_on[WinFirewall::profile_index(bit)]) out |= bit;
    return out;
}

} // namespace

bool FirewallCheckDialog::supported()
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void FirewallCheckDialog::show_modal(wxWindow* parent)
{
    if (!supported()) {
        MessageDialog dlg(parent, _L("The firewall check is for Windows Firewall only. On this system, if printers do not appear, "
                                     "check that your firewall lets EdgeSlicer receive UDP ports 2021 and 1990 on the local network."),
                          _L("Check firewall"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    FirewallCheckDialog dlg(parent);
    dlg.ShowModal();
}

FirewallCheckDialog::FirewallCheckDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Check Windows Firewall"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_alive(std::make_shared<bool>(true))
{
    SetBackgroundColour(*wxWHITE);
    const int wrap = FromDIP(TEXT_WIDTH);

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
        _L("EdgeSlicer finds Bambu Lab printers on your network by listening for their announcements on UDP ports 2021 and 1990. "
           "If Windows Firewall drops them, the printers do not appear in the Device list. This check only reads the firewall; "
           "nothing changes until you press \"Fix firewall rules\"."));
    intro->SetFont(Label::Body_13);
    intro->Wrap(wrap);
    top->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_summary->SetFont(Label::Head_14);
    top->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_lines_panel = new wxPanel(this, wxID_ANY);
    m_lines_panel->SetBackgroundColour(*wxWHITE);
    m_lines_sizer = new wxBoxSizer(wxVERTICAL);
    m_lines_panel->SetSizer(m_lines_sizer);
    top->Add(m_lines_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    top->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));

    // "Also allow on Public networks", off by default, with the trade-off spelled out.
    auto* public_row = new wxBoxSizer(wxHORIZONTAL);
    m_cb_public      = new ::CheckBox(this);
    m_cb_public->SetValue(false);
    public_row->Add(m_cb_public, 0, wxALIGN_CENTER_VERTICAL);
    m_public_label = new wxStaticText(this, wxID_ANY, _L("Also allow on Public networks"));
    m_public_label->SetFont(Label::Body_13);
    public_row->Add(m_public_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    top->Add(public_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    m_public_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (m_cb_public->IsEnabled()) {
            m_cb_public->SetValue(!m_cb_public->GetValue());
            update_public_hint();
        }
    });
    m_cb_public->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        update_public_hint();
        e.Skip();
    });

    m_public_explain = new wxStaticText(this, wxID_ANY,
        _L("Public networks are the ones Windows treats as untrusted, such as cafe or hotel Wi-Fi. Allowing EdgeSlicer there "
           "lets other devices on such a network reach it too (printer discovery, the phone hub and its video). Leave this off "
           "and set your home network to Private instead, unless you cannot change it."));
    m_public_explain->SetFont(Label::Body_12);
    m_public_explain->SetForegroundColour(wxColour(107, 107, 107));
    m_public_explain->Wrap(wrap);
    top->Add(m_public_explain, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    // Shown when the PC is on a Public network and the box above is off: how to make it Private.
    m_public_hint = new wxPanel(this, wxID_ANY);
    m_public_hint->SetBackgroundColour(wxColour(255, 247, 230));
    auto* hint_sizer   = new wxBoxSizer(wxVERTICAL);
    m_public_hint_text = new wxStaticText(m_public_hint, wxID_ANY,
        _L("This PC's network is set to Public, and the fix only allows EdgeSlicer on Private and Domain networks. If this is your "
           "home or workshop network, set it to Private: Settings > Network & internet > Wi-Fi or Ethernet > your network > "
           "Network profile type > Private. EdgeSlicer never changes this setting itself."));
    m_public_hint_text->SetFont(Label::Body_13);
    m_public_hint_text->Wrap(wrap - FromDIP(20));
    hint_sizer->Add(m_public_hint_text, 0, wxEXPAND | wxALL, FromDIP(10));
    m_btn_settings = new ::Button(m_public_hint, _L("Open network settings"));
    m_btn_settings->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_btn_settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_network_settings(); });
    hint_sizer->Add(m_btn_settings, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_public_hint->SetSizer(hint_sizer);
    top->Add(m_public_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_result = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_result->SetFont(Label::Body_13);
    top->Add(m_result, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    m_btn_recheck = new ::Button(this, _L("Check again"));
    m_btn_recheck->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    buttons->Add(m_btn_recheck, 0, wxLEFT, FromDIP(8));
    m_btn_fix = new ::Button(this, _L("Fix firewall rules"));
    m_btn_fix->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_fix->SetToolTip(_L("Windows asks for administrator permission once. The fix removes Block rules for this copy of EdgeSlicer "
                             "and its go2rtc.exe, and adds the allow rules for this copy's location. It never touches other programs' "
                             "rules and never turns the firewall off."));
    buttons->Add(m_btn_fix, 0, wxLEFT, FromDIP(8));
    m_btn_close = new ::Button(this, _L("Close"));
    m_btn_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    buttons->Add(m_btn_close, 0, wxLEFT, FromDIP(8));
    top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(16));

    m_btn_recheck->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_result->SetLabel(wxEmptyString);
        run_check();
    });
    m_btn_fix->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_fix(); });
    m_btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    // The helper's thread posts back to this dialog: it stays open until the helper is done.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (m_fixing && e.CanVeto()) {
            e.Veto();
            return;
        }
        EndModal(wxID_CLOSE);
    });

    SetSizer(top);
    wxGetApp().UpdateDlgDarkUI(this);
    run_check();
    CentreOnParent();
}

FirewallCheckDialog::~FirewallCheckDialog() { *m_alive = false; }

void FirewallCheckDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    m_btn_fix->Rescale();
    m_btn_recheck->Rescale();
    m_btn_close->Rescale();
    m_btn_settings->Rescale();
    m_cb_public->Rescale();
    relayout();
}

void FirewallCheckDialog::relayout()
{
    GetSizer()->SetSizeHints(this);
    Layout();
    Refresh();
}

void FirewallCheckDialog::run_check()
{
    wxBusyCursor busy;
    m_diag = WinFirewall::diagnose_this_copy();
    rebuild_lines();
    update_public_hint();
    m_btn_fix->Enable(m_diag.ok && !m_fixing);
    m_btn_fix->SetStyle(m_diag.has_problems() ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Choice);
    relayout();
}

void FirewallCheckDialog::add_line(Severity sev, const wxString& text)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    const wxString tag = sev == Severity::Ok ? _L("OK") : sev == Severity::Note ? _L("Note") : _L("Problem");
    auto* badge = new wxStaticText(m_lines_panel, wxID_ANY, tag, wxDefaultPosition, wxSize(FromDIP(64), -1));
    badge->SetFont(Label::Head_13);
    badge->SetForegroundColour(sev == Severity::Ok ? wxColour(0, 150, 136) : sev == Severity::Note ? wxColour(230, 126, 0) : wxColour(225, 71, 71));
    auto* body = new wxStaticText(m_lines_panel, wxID_ANY, text);
    body->SetFont(Label::Body_13);
    body->Wrap(FromDIP(TEXT_WIDTH - 72));
    row->Add(badge, 0, wxALIGN_TOP);
    row->Add(body, 1, wxEXPAND | wxLEFT, FromDIP(8));
    m_lines_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    wxGetApp().UpdateDarkUI(badge);
    wxGetApp().UpdateDarkUI(body);
}

void FirewallCheckDialog::rebuild_lines()
{
    m_lines_panel->Freeze();
    m_lines_sizer->Clear(true);
    const WinFirewall::Diagnosis& d = m_diag;

    if (!d.ok) {
        m_summary->SetLabel(_L("Windows Firewall could not be checked."));
        add_line(Severity::Problem, wxString::Format(_L("Windows did not let EdgeSlicer read the firewall rules (%s)."), from_u8(d.error)));
        m_lines_panel->Thaw();
        return;
    }

    const int live    = d.current_profiles;
    const int guarded = guarded_profiles(d);
    if (d.has_problems())
        m_summary->SetLabel(d.discovery_problem_profiles() || d.enabled_blocks() ?
                                _L("Windows Firewall is likely hiding your printers. Press \"Fix firewall rules\".") :
                                _L("Windows Firewall needs attention for some EdgeSlicer features."));
    else
        m_summary->SetLabel(_L("No firewall problems found for this copy of EdgeSlicer."));

    add_line(Severity::Note, wxString::Format(_L("This copy: %s"), from_u8(d.exe)));

    if (live == 0)
        add_line(Severity::Note, _L("Windows reports no active network right now."));
    else
        add_line(Severity::Ok, wxString::Format(_L("This PC's network is %s."), profile_words(live)));

    for (int bit : { WinFirewall::ProfileDomain, WinFirewall::ProfilePrivate, WinFirewall::ProfilePublic }) {
        if (!(live & bit)) continue;
        const int idx = WinFirewall::profile_index(bit);
        if (!d.firewall_on[idx])
            add_line(Severity::Note, wxString::Format(_L("Windows Firewall is off for %s networks, so it is not what blocks EdgeSlicer. If another "
                                                         "security program runs its own firewall, allow EdgeSlicer there."),
                                                      profile_words(bit)));
        else if (d.block_all_inbound[idx])
            add_line(Severity::Problem, wxString::Format(_L("Windows Firewall is set to block all incoming connections on %s networks, which overrides "
                                                            "every allow rule. Turn it off in Windows Security > Firewall & network protection."),
                                                         profile_words(bit)));
    }

    // One line per feature (the two video rules, UDP and TCP, read as one).
    std::vector<std::string> purposes;
    for (const auto& e : d.expected)
        if (std::find(purposes.begin(), purposes.end(), e.rule.purpose) == purposes.end()) purposes.push_back(e.rule.purpose);
    for (const std::string& purpose : purposes) {
        int allowed = WinFirewall::ProfileAll, blocked = 0;
        for (const auto& e : d.expected)
            if (e.rule.purpose == purpose) {
                allowed &= e.allowed_profiles;
                blocked |= e.blocked_profiles;
            }
        const wxString what = purpose_words(purpose);
        if (blocked & guarded)
            add_line(Severity::Problem, wxString::Format(_L("%s: blocked by a Block rule on this network."), what));
        else if (allowed == 0)
            add_line(guarded ? Severity::Problem : Severity::Note, wxString::Format(_L("%s: no allow rule for this copy."), what));
        else if (guarded & ~allowed)
            add_line(Severity::Problem, wxString::Format(_L("%s: allowed on %s networks only, but this PC is on a %s network."), what,
                                                         profile_words(allowed), profile_words(guarded & ~allowed)));
        else
            add_line(Severity::Ok, wxString::Format(_L("%s: allowed on %s networks."), what, profile_words(allowed)));
    }
    if (d.go2rtc.empty())
        add_line(Severity::Note, _L("go2rtc.exe is not part of this copy, so the phone video rules are skipped."));

    int enabled = 0, disabled = 0;
    wxString names;
    for (const auto& r : d.blocks) {
        (r.enabled ? enabled : disabled) += 1;
        if (r.enabled && names.Find(from_u8(r.name)) == wxNOT_FOUND) names += (names.empty() ? "" : ", ") + wxString("\"") + from_u8(r.name) + "\"";
    }
    if (enabled > 0)
        add_line(Severity::Problem,
                 wxString::Format(_L("Windows Firewall has %d Block rule(s) for this copy (%s). Windows creates these when its \"Allow access\" "
                                     "prompt is cancelled or closed, and a Block rule wins over every allow rule."),
                                  enabled, names));
    if (disabled > 0)
        add_line(Severity::Note, wxString::Format(_L("There are also %d switched-off Block rule(s) for this copy; the fix removes them too."), disabled));

    m_lines_panel->Thaw();
}

void FirewallCheckDialog::update_public_hint()
{
    const bool show = m_diag.ok && m_diag.on_public_network() && !m_cb_public->GetValue();
    if (m_public_hint->IsShown() != show) {
        m_public_hint->Show(show);
        relayout();
    }
}

void FirewallCheckDialog::open_network_settings()
{
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] opening Settings > Network (ms-settings:network-status)";
#ifdef _WIN32
    ::ShellExecuteW(nullptr, L"open", L"ms-settings:network-status", nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void FirewallCheckDialog::start_fix()
{
    if (m_fixing) return;
    const bool allow_public = m_cb_public->GetValue();
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] user pressed Fix firewall rules (Public networks too: " << (allow_public ? "yes" : "no") << ")";
    m_fixing = true;
    m_btn_fix->Enable(false);
    m_btn_recheck->Enable(false);
    m_btn_close->Enable(false);
    m_cb_public->Enable(false);
    SetEscapeId(wxID_NONE); // Esc must not end the dialog while the helper runs
    m_result->SetLabel(_L("Waiting for Windows to ask for administrator permission..."));
    relayout();

#ifdef _WIN32
    void* hwnd = GetHWND();
#else
    void* hwnd = nullptr;
#endif
    std::weak_ptr<bool> alive = m_alive;
    // The UAC prompt and the helper run off the UI thread; the dialog cannot close meanwhile
    // (the close handler vetoes while m_fixing), so CallAfter always finds it.
    std::thread([this, hwnd, allow_public, alive]() {
        const WinFirewall::ElevatedOutcome outcome = WinFirewall::run_elevated_fix(hwnd, allow_public);
        if (auto a = alive.lock(); a && *a)
            CallAfter([this, outcome, allow_public, alive]() {
                if (auto b = alive.lock(); b && *b) on_fix_done(outcome, allow_public);
            });
    }).detach();
}

void FirewallCheckDialog::on_fix_done(const WinFirewall::ElevatedOutcome& outcome, bool allow_public)
{
    m_fixing = false;
    m_btn_recheck->Enable(true);
    m_btn_close->Enable(true);
    m_cb_public->Enable(true);
    SetEscapeId(wxID_ANY);

    wxString msg;
    switch (outcome.result) {
    case WinFirewall::ElevatedResult::Cancelled:
        msg = _L("You cancelled the Windows permission prompt, so nothing was changed.");
        break;
    case WinFirewall::ElevatedResult::Failed:
        msg = wxString::Format(_L("The fix could not run: %s."), from_u8(outcome.error));
        break;
    case WinFirewall::ElevatedResult::Done:
        if (outcome.exit_code == WinFirewall::FIX_EXIT_OK)
            msg = allow_public ? _L("The firewall rules were updated (Private, Domain and Public networks). The check above was run again.") :
                                 _L("The firewall rules were updated (Private and Domain networks). The check above was run again.");
        else if (outcome.exit_code == WinFirewall::FIX_EXIT_PARTIAL)
            msg = _L("Some changes could not be made; the details are in the log (Help > Show Configuration Folder > log). The check above was run again.");
        else if (outcome.exit_code == WinFirewall::FIX_EXIT_READ_FAILED)
            msg = _L("The helper could not read Windows Firewall, so nothing was changed.");
        else
            msg = wxString::Format(_L("The helper stopped with code %d; nothing may have changed. The check above was run again."), outcome.exit_code);
        break;
    }
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix result shown to the user: " << into_u8(msg);
    m_result->SetLabel(msg);
    m_result->Wrap(FromDIP(TEXT_WIDTH));
    run_check();
}

} // namespace GUI
} // namespace Slic3r
