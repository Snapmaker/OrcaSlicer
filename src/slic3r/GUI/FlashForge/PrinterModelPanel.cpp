#include "PrinterModelPanel.hpp"
#include <memory>
#include <wx/dcgraph.h>
#include <wx/filefn.h>
#include <wx/image.h>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI {

PrinterModelPanel::PrinterModelPanel(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , m_printerCmb(nullptr)
    , m_editBtn(nullptr)
    , m_connectionBtn(nullptr)
    , m_hover(false)
{
}

void PrinterModelPanel::setup(PlaterPresetComboBox *printerCmb, ScalableButton *editBtn, ScalableButton *connectionBtn)
{
    m_printerCmb = printerCmb;
    m_editBtn = editBtn;
    m_connectionBtn = connectionBtn;

    SetMinSize(wxSize(-1, FromDIP(56)));
    SetMaxSize(wxSize(-1, FromDIP(56)));

    m_printerCmb->SetBorderColor(StateColor(
        std::make_pair(0xffffff, (int)StateColor::Disabled),
        std::make_pair(0xffffff, (int)StateColor::Hovered),
        std::make_pair(0xffffff, (int)StateColor::Normal))
    );

    wxBoxSizer *horzSizer = new wxBoxSizer(wxHORIZONTAL);
    horzSizer->Add(m_printerCmb, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(56));
    horzSizer->Add(m_editBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ElementSpacing()));
    horzSizer->Add(m_connectionBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    horzSizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    Bind(wxEVT_PAINT, &PrinterModelPanel::onPaint, this);
    Bind(wxEVT_LEAVE_WINDOW, &PrinterModelPanel::onLeave, this);
    Bind(wxEVT_MOTION, &PrinterModelPanel::onMotion, this);
    Bind(wxEVT_LEFT_DOWN, &PrinterModelPanel::onLeftDown, this);
    m_printerCmb->Bind(wxEVT_LEAVE_WINDOW, &PrinterModelPanel::onLeave, this);
    m_printerCmb->Bind(wxEVT_MOTION, &PrinterModelPanel::onMotion, this);
    m_printerCmb->Bind(wxEVT_SET_FOCUS, &PrinterModelPanel::onPrintCmbSetFocus, this);
    m_printerCmb->GetDropDown().Bind(wxEVT_SET_FOCUS, &PrinterModelPanel::onPrintCmbSetFocus, this);
    m_printerCmb->Bind(wxEVT_KILL_FOCUS, &PrinterModelPanel::onPrintCmbKillFocus, this);
    m_printerCmb->GetDropDown().Bind(wxEVT_KILL_FOCUS, &PrinterModelPanel::onPrintCmbKillFocus, this);
    m_editBtn->Bind(wxEVT_LEAVE_WINDOW, &PrinterModelPanel::onLeave, this);
    m_editBtn->Bind(wxEVT_MOTION, &PrinterModelPanel::onMotion, this);
    m_editBtn->Bind(wxEVT_BUTTON, &PrinterModelPanel::onButtonClicked, this);
    m_connectionBtn->Bind(wxEVT_LEAVE_WINDOW, &PrinterModelPanel::onLeave, this);
    m_connectionBtn->Bind(wxEVT_MOTION, &PrinterModelPanel::onMotion, this);
    m_connectionBtn->Bind(wxEVT_BUTTON, &PrinterModelPanel::onButtonClicked, this);
    wxGetApp().Bind(wxEVT_ACTIVATE_APP, &PrinterModelPanel::onActivateApp, this);

    SetSizer(horzSizer);
    Layout();
    Fit();
}

void PrinterModelPanel::updatePrinterIcon()
{
    PresetBundle* presetBundle = wxGetApp().preset_bundle;
    if (presetBundle == nullptr) {
        m_iconBmp = wxBitmap();
        m_iconPath.clear();
        return;
    }
    Preset &preset = presetBundle->printers.get_edited_preset();
    std::string printerType = preset.config.opt_string("printer_model");
    std::string iconPath;
    if (preset.vendor != nullptr) {
        iconPath = resources_dir() + "/profiles/" + preset.vendor->id + "/" + printerType + "_cover.png";
    } else {
        for (auto &item : presetBundle->vendors) {
            iconPath = resources_dir() + "/profiles/" + item.second.id + "/" + printerType + "_cover.png";
            if (wxFileExists(wxString::FromUTF8(iconPath))) {
                break;
            }
        }
    }
    if (iconPath == m_iconPath) {
        return;
    }
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(iconPath))) {
        m_iconBmp = wxBitmap();
        m_iconPath.clear();
        return;
    }
    m_iconBmp = image.Rescale(FromDIP(48), FromDIP(48));
    m_iconPath = iconPath;
}

void PrinterModelPanel::onPaint(wxPaintEvent &event)
{
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    gc->SetPen(m_hover ? wxColour(0x00, 0x96, 0x88) : wxColour(0xDB, 0xDB, 0xDB));
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->DrawRectangle(0, 0, GetSize().x - 1, GetSize().y - 1);
    if (m_iconBmp.IsOk()) {
        gc->DrawBitmap(m_iconBmp, FromDIP(4), FromDIP(4), m_iconBmp.GetWidth(), m_iconBmp.GetHeight());
    }
}

void PrinterModelPanel::onLeave(wxMouseEvent &event)
{
    event.Skip();
    if (!m_focusObjSet.empty()) {
        return;
    }
    if (m_hover) {
        m_hover = false;
        Refresh();
        Update();
    }
}

void PrinterModelPanel::onMotion(wxMouseEvent &event)
{
    event.Skip();
    if (!m_hover) {
        m_hover = true;
        Refresh();
        Update();
    }
}

void PrinterModelPanel::onLeftDown(wxMouseEvent &event)
{
    event.Skip();
    wxKeyEvent tmpEvent(wxEVT_KEY_DOWN);
    tmpEvent.m_keyCode = WXK_SPACE;
    wxPostEvent(m_printerCmb, tmpEvent);
}

void PrinterModelPanel::onPrintCmbSetFocus(wxFocusEvent &event)
{
    event.Skip();
    m_focusObjSet.insert(event.GetEventObject());
    if (!m_focusObjSet.empty() && !m_hover) {
        m_hover = true;
        Refresh();
        Update();
    }
}

void PrinterModelPanel::onPrintCmbKillFocus(wxFocusEvent &event)
{
    event.Skip();
    m_focusObjSet.erase(event.GetEventObject());
    if (m_focusObjSet.empty() && m_hover) {
        m_hover = false;
        Refresh();
        Update();
    }
}

void PrinterModelPanel::onButtonClicked(wxCommandEvent &event)
{
    event.Skip();
    if (m_hover) {
        m_hover = false;
        Refresh();
        Update();
    }
}

void PrinterModelPanel::onActivateApp(wxActivateEvent &event)
{
    event.Skip();
    if (event.GetActive()) {
        return;
    }
    if (m_hover) {
        m_hover = false;
        Refresh();
        Update();
    }
}

}} // namespace Slic3r::GUI
