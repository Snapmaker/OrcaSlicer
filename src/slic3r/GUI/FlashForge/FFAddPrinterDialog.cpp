#include "FFAddPrinterDialog.hpp"

#include <boost/asio/ip/address_v4.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/choicdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/GUI/FlashForge/DeviceData.hpp"
#include "slic3r/GUI/FlashForge/FFDiagnostics.hpp"
#include "slic3r/GUI/FlashForge/FlashNetworkIntfc.h"
#include "slic3r/GUI/FlashForge/FreeInDestructor.h"
#include "slic3r/GUI/FlashForge/MultiComDef.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/Utils/Flashforge.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr unsigned short FF_LAN_PORT = 8898; // the Creator 5 / Adventurer 5 HTTP API

std::string trimmed(const wxString& text)
{
    wxString t = text;
    t.Trim(true).Trim(false);
    return t.utf8_string();
}

} // namespace

FFAddPrinterDialog::FFAddPrinterDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Add FlashForge printer"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);

    // Printers already set up as a Physical Printer with Host Type = Flashforge carry exactly the
    // three values this dialog wants; offer them rather than make the user type them twice.
    if (wxGetApp().preset_bundle != nullptr) {
        for (const PhysicalPrinter& printer : wxGetApp().preset_bundle->physical_printers) {
            const DynamicPrintConfig& cfg = printer.config;
            if (!cfg.has("host_type") || cfg.opt_enum<PrintHostType>("host_type") != htFlashforge)
                continue;
            SavedPrinter saved;
            saved.ip         = cfg.has("print_host") ? cfg.opt_string("print_host") : std::string();
            saved.serial     = cfg.has("flashforge_serial_number") ? cfg.opt_string("flashforge_serial_number") : std::string();
            saved.check_code = cfg.has("printhost_apikey") ? cfg.opt_string("printhost_apikey") : std::string();
            if (saved.ip.empty() && saved.serial.empty())
                continue;
            saved.label = wxString::FromUTF8(printer.name.c_str());
            if (!saved.ip.empty())
                saved.label += " (" + wxString::FromUTF8(saved.ip.c_str()) + ")";
            m_saved.push_back(std::move(saved));
        }
    }

    auto* intro = new wxStaticText(
        this, wxID_ANY,
        _L("Add a FlashForge printer by its serial number, IP address and check code. On the printer: Settings > Network > "
           "Network Mode shows all three. The printer must be switched on and on this network."));
    intro->SetFont(Label::Body_13);
    intro->Wrap(FromDIP(480));

    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
    grid->AddGrowableCol(1, 1);

    if (!m_saved.empty()) {
        auto* label = new wxStaticText(this, wxID_ANY, _L("Saved printer"));
        label->SetFont(Label::Body_14);
        grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        m_saved_choice = new wxChoice(this, wxID_ANY);
        m_saved_choice->Append(_L("Choose a printer set up in Printer settings..."));
        for (const SavedPrinter& saved : m_saved)
            m_saved_choice->Append(saved.label);
        m_saved_choice->SetSelection(0);
        grid->Add(m_saved_choice, 1, wxEXPAND);
    }

    auto add_row = [this, grid](const wxString& label, ::TextInput*& field) {
        auto* text = new wxStaticText(this, wxID_ANY, label);
        text->SetFont(Label::Body_14);
        grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        field = new ::TextInput(this, wxEmptyString);
        field->SetMinSize(wxSize(FromDIP(300), FromDIP(28)));
        grid->Add(field, 1, wxEXPAND);
    };
    add_row(_L("Serial number"), m_serial);
    add_row(_L("IP address"), m_ip);
    add_row(_L("Check code"), m_code);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_14);
    m_status->Wrap(FromDIP(480));

    m_search_btn = new wxButton(this, wxID_ANY, _L("Search the network"));
    m_search_btn->SetToolTip(_L("Looks for FlashForge printers on this network and fills in the serial number and IP address of the one "
                                "you pick. Needs UDP port 18007 open in Windows Firewall (Help > Check Windows Firewall)."));
    m_add_btn = new wxButton(this, wxID_ANY, _L("Add"));
    m_add_btn->SetDefault();

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(m_search_btn, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer(1);
    buttons->Add(m_add_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(12));
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    SetSizerAndFit(sizer);
    CentreOnParent();

    m_search_btn->Bind(wxEVT_BUTTON, &FFAddPrinterDialog::on_search, this);
    m_add_btn->Bind(wxEVT_BUTTON, &FFAddPrinterDialog::on_add, this);
    if (m_saved_choice != nullptr)
        m_saved_choice->Bind(wxEVT_CHOICE, &FFAddPrinterDialog::on_pick_saved, this);

    wxGetApp().UpdateDlgDarkUI(this);
}

void FFAddPrinterDialog::set_status(const wxString& text, bool ok)
{
    m_status->SetLabel(text);
    m_status->SetForegroundColour(ok ? wxColour(0x1F, 0x8A, 0x3C) : wxColour(0xEA, 0x35, 0x22));
    m_status->Wrap(FromDIP(480));
    Layout();
    Fit();
}

void FFAddPrinterDialog::on_pick_saved(wxCommandEvent&)
{
    const int sel = m_saved_choice->GetSelection() - 1; // entry 0 is the prompt
    if (sel < 0 || sel >= (int) m_saved.size())
        return;
    const SavedPrinter& saved = m_saved[sel];
    m_serial->GetTextCtrl()->SetValue(wxString::FromUTF8(saved.serial.c_str()));
    m_ip->GetTextCtrl()->SetValue(wxString::FromUTF8(saved.ip.c_str()));
    m_code->GetTextCtrl()->SetValue(wxString::FromUTF8(saved.check_code.c_str()));
}

// The same broadcast the Physical Printer dialog's Browse button sends (UDP 48899, answered on
// 18007). It fills the serial number and address; the check code is only ever on the printer.
void FFAddPrinterDialog::on_search(wxCommandEvent&)
{
    wxBusyCursor                             busy;
    std::vector<FlashforgeDiscoveredPrinter> found;
    wxString                                 error;
    m_search_btn->Enable(false);
    const bool ok = Flashforge::discover_printers(found, error, 4000, 1000, 2);
    m_search_btn->Enable(true);
    BOOST_LOG_TRIVIAL(warning) << "[FlashForge] network search for the Add printer dialog: " << (ok ? found.size() : 0) << " printer(s)"
                               << (ok ? std::string() : ": " + error.utf8_string());
    if (!ok || found.empty()) {
        set_status(error.empty() ? _L("No FlashForge printers answered. Check that the printer is on, on this network, and that "
                                      "Windows Firewall lets EdgeSlicer receive UDP 18007 (Help > Check Windows Firewall). You can "
                                      "still type the serial number and IP address in.") :
                                   error,
                   false);
        return;
    }

    wxArrayString choices;
    for (const FlashforgeDiscoveredPrinter& printer : found)
        choices.Add(wxString::FromUTF8((printer.name + " (" + printer.ip_address + ") [" + printer.serial_number + "]").c_str()));
    wxSingleChoiceDialog pick(this, _L("Select a FlashForge printer"), _L("Discovered printers"), choices);
    if (pick.ShowModal() != wxID_OK)
        return;
    const int idx = pick.GetSelection();
    if (idx < 0 || idx >= (int) found.size())
        return;
    m_serial->GetTextCtrl()->SetValue(wxString::FromUTF8(found[idx].serial_number.c_str()));
    m_ip->GetTextCtrl()->SetValue(wxString::FromUTF8(found[idx].ip_address.c_str()));
    set_status(_L("Found it. Now enter the check code from the printer and press Add."), true);
}

void FFAddPrinterDialog::on_add(wxCommandEvent&)
{
    const std::string serial = trimmed(m_serial->GetTextCtrl()->GetValue());
    const std::string ip     = trimmed(m_ip->GetTextCtrl()->GetValue());
    const std::string code   = trimmed(m_code->GetTextCtrl()->GetValue());

    boost::system::error_code ec;
    boost::asio::ip::make_address_v4(ip, ec);
    if (serial.empty() || ip.empty() || code.empty()) {
        set_status(_L("Enter the serial number, the IP address and the check code."), false);
        return;
    }
    if (ec) {
        set_status(_L("That IP address is not valid. It looks like 192.168.1.50."), false);
        return;
    }

    fnet::FlashNetworkIntfc* intfc = MultiComMgr::inst()->networkIntfc();
    if (intfc == nullptr) {
        set_status(_L("FlashNetwork is not loaded, so printers cannot be added."), false);
        return;
    }

    // Ask the printer to describe itself with these credentials - read-only, no job. This is both the
    // check that they are right and where the printer's own name and product id come from.
    wxBusyCursor busy;
    m_add_btn->Enable(false);
    fnet_dev_detail_t* detail = nullptr;
    const int          code_result = intfc->getLanDevDetail(ip.c_str(), FF_LAN_PORT, serial.c_str(), code.c_str(), &detail, ComTimeoutLanA);
    fnet::FreeInDestructor freeDetail(detail, intfc->freeDevDetail);
    m_add_btn->Enable(true);

    if (!fnet_succeeded(code_result) || detail == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "[FlashForge] Add printer: " << serial << " at " << ip << " refused: " << fnet_error_description(code_result);
        set_status(_L("Could not add the printer:") + " " + wxString::FromUTF8(fnet_error_description(code_result).c_str()) + "\n" +
                       _L("Use Test connection for more detail."),
                   false);
        return;
    }

    std::string name = detail->name != nullptr ? std::string(detail->name) : std::string();
    if (name.empty())
        name = serial;
    const unsigned short pid = (unsigned short) detail->pid;

    fnet_lan_dev_info info = DeviceObjectOpr::make_lan_info(serial, name, ip, FF_LAN_PORT, pid);
    DeviceObjectOpr*  opr  = wxGetApp().getDeviceObjectOpr();
    if (opr == nullptr || !opr->add_manual_lan_machine(info, code)) {
        set_status(_L("The printer answered, but the connection could not be opened."), false);
        return;
    }
    BOOST_LOG_TRIVIAL(warning) << "[FlashForge] Add printer: " << serial << " (" << name << ") at " << ip << " accepted";
    m_added = true;
    EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI
