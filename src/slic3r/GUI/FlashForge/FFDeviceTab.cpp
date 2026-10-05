#include "FFDeviceTab.hpp"

#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/FlashForge/DeviceListPanel.hpp"
#include "slic3r/GUI/FlashForge/SingleDeviceState.hpp"
#include "slic3r/GUI/FlashForge/DeviceData.hpp"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/FlashForge/FFDiagnostics.hpp"
#include "slic3r/GUI/FlashForge/FFDiagnosticsDialog.hpp"

namespace Slic3r {
namespace GUI {

FFDeviceTab::FFDeviceTab(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_book = new wxSimplebook(this, wxID_ANY);
    m_device_list   = new DeviceListPanel(m_book);
    m_device_status = new SingleDeviceState(m_book);
    m_unavailable   = build_unavailable_page();
    m_book->AddPage(m_device_list, wxEmptyString, true);
    m_book->AddPage(m_device_status, wxEmptyString, false);
    m_book->AddPage(m_unavailable, wxEmptyString, false);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_book, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Same routing as Orca-Flashforge's MonitorPanel.
    Bind(EVT_SWITCH_TO_DEVICE_STATUS, [this](wxCommandEvent& event) {
        m_device_status->setCurId(event.GetInt());
        m_book->SetSelection(1);
        m_device_status->checkPrinterStatus();
    });
    Bind(EVT_SWITCH_TO_DEVICE_LIST, [this](wxCommandEvent&) {
        m_book->SetSelection(0);
    });

    refresh_availability();
}

// The "nothing here works and here is why" page.
//
// The failure this replaces was silent: no FlashNetwork.dll meant the tab initialised nothing
// and drew an empty list, with the only trace in a log file. Naming the paths that were
// searched is what lets a user say where they put the library, or find they never had it.
wxWindow* FFDeviceTab::build_unavailable_page()
{
    auto* page = new wxPanel(m_book, wxID_ANY);
    page->SetBackgroundColour(*wxWHITE);

    auto* title = new wxStaticText(page, wxID_ANY, _L("FlashForge printers are unavailable"));
    title->SetFont(Label::Head_16);

    m_unavailable_text = new wxStaticText(page, wxID_ANY, wxEmptyString);
    m_unavailable_text->SetFont(Label::Body_13);

    auto* download_btn = new wxButton(page, wxID_ANY, _L("Download"));
    auto* locate_btn   = new wxButton(page, wxID_ANY, _L("Locate..."));
    auto* diag_btn     = new wxButton(page, wxID_ANY, _L("Diagnostics"));
    download_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_download(); });
    locate_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_locate(); });
    diag_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        FFDiagnosticsDialog dlg(this);
        dlg.ShowModal();
    });

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(download_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(locate_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(diag_btn, 0);

    auto* notice = new wxStaticText(
        page, wxID_ANY,
        _L("FlashNetwork is FlashForge's own library, redistributed unmodified from the "
           "Orca-Flashforge / Flash Studio release it was taken from."));
    notice->SetFont(Label::Body_12);
    notice->SetForegroundColour(wxColour(107, 107, 107));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddStretchSpacer(2);
    sizer->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(12));
    sizer->Add(m_unavailable_text, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(30));
    sizer->AddSpacer(FromDIP(18));
    sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL);
    sizer->AddSpacer(FromDIP(18));
    sizer->Add(notice, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(30));
    sizer->AddStretchSpacer(3);
    page->SetSizer(sizer);
    return page;
}

void FFDeviceTab::update_unavailable_text()
{
    if (m_unavailable_text == nullptr)
        return;
    std::string message = wxGetApp().flashnetwork_error();
    if (message.empty())
        message = ff_flashnetwork_missing_text(wxGetApp().flashnetwork_searched());
    m_unavailable_text->SetLabel(wxString::FromUTF8(message.c_str()));
    m_unavailable_text->Wrap(FromDIP(560));
    m_unavailable->Layout();
}

bool FFDeviceTab::refresh_availability()
{
    const bool ok = wxGetApp().flashnetwork_loaded();
    if (!ok) {
        update_unavailable_text();
        m_book->SetSelection(2);
    } else if (m_book->GetSelection() == 2) {
        m_book->SetSelection(0);
    }
    return ok;
}

// "Locate" rather than "Download" is the path that actually works today: FlashForge publish the
// library nowhere, so there is no URL to fetch. A user who has Flash Studio installed already
// has a copy, and this points the app at it without a reinstall.
void FFDeviceTab::on_locate()
{
    wxFileDialog dlg(this, _L("Select FlashNetwork.dll"), wxEmptyString, "FlashNetwork.dll",
                     "FlashNetwork.dll|FlashNetwork.dll|All files|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    if (wxGetApp().init_flashnetwork(dlg.GetPath().utf8_string())) {
        refresh_availability();
        MessageDialog ok(this, _L("FlashForge printers are now available."), _L("FlashNetwork"),
                         wxOK | wxICON_INFORMATION);
        ok.ShowModal();
    } else {
        update_unavailable_text();
        MessageDialog err(this, wxString::FromUTF8(wxGetApp().flashnetwork_error().c_str()),
                          _L("FlashNetwork"), wxOK | wxICON_ERROR);
        err.ShowModal();
    }
}

void FFDeviceTab::on_download()
{
    // There is deliberately no download here. FlashForge ship the library only inside their own
    // installers - there is no CDN or updater URL for it - so an automatic fetch would mean us
    // redistributing it from somewhere of our own. Say where it comes from instead, and let
    // "Locate" do the rest.
    MessageDialog dlg(
        this,
        _L("FlashForge does not publish this library for download on its own.\n\n"
           "It is included with EdgeSlicer installers built with FlashForge support. If this "
           "install does not have it, you can also take FlashNetwork.dll from a FlashForge "
           "Flash Studio installation on this machine and point at it with Locate."),
        _L("FlashNetwork"), wxOK | wxICON_INFORMATION);
    dlg.ShowModal();
}

void FFDeviceTab::OnActivate()
{
    // Re-checked on every activation: the user may have used Locate since the tab was built.
    if (!refresh_availability())
        return;
    // Saved printers reconnect from their saved address. They used to wait for a LAN scan that
    // nothing ever ran, so a printer added once sat Offline for good.
    if (DeviceObjectOpr* opr = wxGetApp().getDeviceObjectOpr())
        opr->connect_saved_machines();
    if (m_device_list != nullptr)
        m_device_list->OnActivate();
}

} // namespace GUI
} // namespace Slic3r
