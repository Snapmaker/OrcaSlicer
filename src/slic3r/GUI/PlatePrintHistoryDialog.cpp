#include "PlatePrintHistoryDialog.hpp"

#include "PartPlate.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/panel.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Slic3r { namespace GUI {

namespace {

// "1 h 23 min" / "45 min" / "30 s"
wxString duration_text(int seconds)
{
    if (seconds <= 0)
        return {};
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    if (h > 0)
        return wxString::Format(_L("%d h %d min"), h, m);
    if (m > 0)
        return wxString::Format(_L("%d min"), m);
    return wxString::Format(_L("%d s"), seconds);
}

wxColour text_colour() { return StateColor::darkModeColorFor(wxColour("#262E30")); }
wxColour dim_colour() { return StateColor::darkModeColorFor(wxColour("#6B6B6B")); }
wxColour back_colour() { return StateColor::darkModeColorFor(wxColour("#FFFFFF")); }
wxColour line_colour() { return StateColor::darkModeColorFor(wxColour("#E5E5E5")); }
wxColour sent_colour() { return wxColour(0x2E, 0xA0, 0x5A); }

} // namespace

wxString PlatePrintHistoryDialog::connection_label(const std::string &key)
{
    if (key == "bambu_lan")        return _L("Bambu LAN");
    if (key == "bambu_cloud")      return _L("Bambu cloud");
    if (key == "snapmaker_lan")    return _L("Snapmaker LAN");
    if (key == "snapmaker_cloud")  return _L("Snapmaker cloud");
    if (key == "snapmaker")        return _L("Snapmaker");
    if (key == "phone_hub")        return _L("Phone hub");
    if (key == "file")             return _L("File");
    if (key == "octoprint")        return wxString("OctoPrint");
    if (key == "prusalink")        return wxString("PrusaLink");
    if (key == "prusaconnect")     return wxString("Prusa Connect");
    if (key == "moonraker")        return wxString("Moonraker");
    if (key == "flashforge")       return wxString("Flashforge");
    if (key == "elegoolink")       return wxString("Elegoo");
    if (key == "duet")             return wxString("Duet");
    if (key.empty())               return {};
    // A host type this list does not know: show its key as a readable word.
    std::string s = key;
    std::replace(s.begin(), s.end(), '_', ' ');
    if (!s.empty())
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return wxString::FromUTF8(s.c_str());
}

wxString PlatePrintHistoryDialog::action_label(PlateHistory::Action action)
{
    switch (action) {
    case PlateHistory::Action::Sent:           return _L("Sent to printer");
    case PlateHistory::Action::SentAndStarted: return _L("Sent and started");
    case PlateHistory::Action::UploadedOnly:   return _L("Uploaded only");
    case PlateHistory::Action::Exported:       return _L("Exported to file");
    }
    return {};
}

PlatePrintHistoryDialog::PlatePrintHistoryDialog(wxWindow *parent, PartPlate *plate, const wxString &plate_label)
    : DPIDialog(parent, wxID_ANY, wxString::Format(_L("Print history - %s"), plate_label), wxDefaultPosition, wxDefaultSize,
                wxCLOSE_BOX | wxCAPTION)
    , m_plate(plate)
{
    SetBackgroundColour(back_colour());

    auto *main = new wxBoxSizer(wxVERTICAL);
    auto *top  = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(460), 1));
    top->SetBackgroundColour(wxColour(166, 169, 170));
    main->Add(top, 0, wxEXPAND, 0);

    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(460), FromDIP(300)), wxVSCROLL);
    m_list->SetScrollRate(0, FromDIP(12));
    m_list->SetBackgroundColour(back_colour());
    main->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(8));

    auto *buttons = new DialogButtons(this, {"Clear", "OK"});
    m_clear_button = buttons->GetButtonFromID(wxID_CLEAR);
    if (m_clear_button != nullptr) {
        m_clear_button->SetLabel(_L("Clear history"));
        m_clear_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_clear(); });
    }
    if (auto *ok = buttons->GetOK())
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    main->Add(buttons, 0, wxEXPAND, 0);

    SetSizer(main);
    rebuild();
    main->Fit(this);
    CenterOnParent();

    wxGetApp().UpdateDlgDarkUI(this);
}

PlatePrintHistoryDialog::~PlatePrintHistoryDialog() = default;

void PlatePrintHistoryDialog::on_dpi_changed(const wxRect &)
{
    if (m_list != nullptr) {
        m_list->SetScrollRate(0, FromDIP(12));
        rebuild();
    }
    Layout();
}

void PlatePrintHistoryDialog::rebuild()
{
    if (m_list == nullptr || m_plate == nullptr)
        return;
    m_list->Freeze();
    m_list->DestroyChildren();
    auto *sizer = new wxBoxSizer(wxVERTICAL);

    auto add_text = [&](const wxString &text, const wxFont &font, const wxColour &colour, int top_gap = 0) {
        auto *t = new wxStaticText(m_list, wxID_ANY, text);
        t->SetFont(font);
        t->SetForegroundColour(colour);
        t->SetBackgroundColour(back_colour());
        sizer->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8) + top_gap);
        return t;
    };

    const PlateHistory::History &history = m_plate->print_history();
    if (history.empty()) {
        add_text(_L("Not printed yet"), Label::Body_14, dim_colour(), FromDIP(40));
    } else {
        if (m_plate->modified_since_last_send()) {
            auto *note = add_text(_L("This plate was changed after it was last sent to a printer."), Label::Body_13,
                                  wxColour(0xEF, 0x75, 0x4A));
            note->Wrap(FromDIP(430));
        }
        bool first = true;
        for (const PlateHistory::Entry &e : history.newest_first()) {
            if (!first) {
                auto *line = new wxPanel(m_list, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
                line->SetBackgroundColour(line_colour());
                sizer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
            }
            first = false;

            const wxString when = wxString::FromUTF8(PlateHistory::format_local(e).c_str());
            auto *head = new wxBoxSizer(wxHORIZONTAL);
            auto *time = new wxStaticText(m_list, wxID_ANY, when.empty() ? _L("Unknown time") : when);
            time->SetFont(Label::Head_14);
            time->SetForegroundColour(text_colour());
            time->SetBackgroundColour(back_colour());
            head->Add(time, 0, wxALIGN_CENTER_VERTICAL);
            auto *action = new wxStaticText(m_list, wxID_ANY, action_label(e.action));
            action->SetFont(Label::Body_13);
            action->SetForegroundColour(PlateHistory::is_printer_action(e.action) ? sent_colour() : dim_colour());
            action->SetBackgroundColour(back_colour());
            head->Add(action, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
            sizer->Add(head, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8) + FromDIP(8));

            auto detail = [&](const wxString &label, const wxString &value) {
                if (value.empty())
                    return;
                auto *t = add_text(label + wxString(": ") + value, Label::Body_13, dim_colour(), 0);
                t->Wrap(FromDIP(430));
            };
            if (PlateHistory::is_printer_action(e.action)) {
                wxString printer = wxString::FromUTF8(e.printer_name.c_str());
                if (!e.printer_model.empty() && e.printer_model != e.printer_name)
                    printer += wxString::Format(" (%s)", wxString::FromUTF8(e.printer_model.c_str()));
                detail(_L("Printer"), printer);
                detail(_L("Connection"), connection_label(e.connection));
            }
            detail(_L("File"), wxString::FromUTF8(e.file_name.c_str()));
            wxString estimate = duration_text(e.time_estimate_s);
            if (e.filament_g > 0.0) {
                wxString g = wxString::Format(_L("%.1f g"), e.filament_g);
                estimate   = estimate.empty() ? g : estimate + ", " + g;
            }
            detail(_L("Estimate"), estimate);
            sizer->AddSpacer(FromDIP(8));
        }
    }

    m_list->SetSizer(sizer);
    m_list->Layout();
    m_list->FitInside();
    m_list->Thaw();

    if (m_clear_button != nullptr)
        m_clear_button->Enable(!history.empty());
}

void PlatePrintHistoryDialog::on_clear()
{
    if (m_plate == nullptr || m_plate->print_history().empty())
        return;
    MessageDialog dlg(this, _L("Clear the print history of this plate? The list of sends recorded for it is removed, and this is not part of undo."),
                      _L("Clear history"), wxYES_NO | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    m_plate->clear_print_history();
    rebuild();
    if (auto *canvas = wxGetApp().plater()->get_current_canvas3D())
        canvas->set_as_dirty();
}

}} // namespace Slic3r::GUI
