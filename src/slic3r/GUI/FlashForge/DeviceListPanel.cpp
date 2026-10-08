#include "DeviceListPanel.hpp"
#include <boost/log/trivial.hpp>
#include <wx/graphics.h>
#include <wx/event.h>
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/FFUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/FFToggleButton.hpp"
#include "slic3r/GUI/Widgets/FFCheckBox.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
//#include "DeviceData.hpp"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/FFUtils.hpp"
#include <slic3r/GUI/BindDialog.hpp>
#include "slic3r/GUI/FlashForge/FFDiagnosticsDialog.hpp"
#include "slic3r/GUI/FlashForge/FFAddPrinterDialog.hpp"
#include "slic3r/GUI/FlashForge/DeviceData.hpp"
#include "slic3r/GUI/FlashForge/FFPrinterSources.hpp"
#include "slic3r/GUI/PrintHostDevicesDialog.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

namespace {

constexpr auto FF_DEVICE_LIST_PRIMARY_TEXT   = "#262E30";
constexpr auto FF_DEVICE_LIST_SECONDARY_TEXT = "#6B6B6B";
constexpr auto FF_DEVICE_LIST_MUTED_TEXT     = "#909090";

void apply_light_mode_text(wxWindow* window, const wxColour& color)
{
#ifdef __APPLE__
    if (!window) {
        return;
    }
    window->SetForegroundColour(color);
#else
    (void) window;
    (void) color;
#endif
}

}

DropDownButton::DropDownButton(wxWindow* parent/*=nullptr*/, const wxString& text/*=wxEmptyString*/, const wxBitmap& bitmap/*=wxNullBitmap*/)
    : wxPanel(parent)
{
    //SetMinSize(wxSize(FromDIP(80), FromDIP(24)));
    if (parent) {
        SetBackgroundColour(parent->GetBackgroundColour());
    }
    m_text = new wxStaticText(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    apply_light_mode_text(m_text, wxColour(FF_DEVICE_LIST_PRIMARY_TEXT));
    //m_text->FitInside();
    m_text->Fit();
    m_bitmap = new wxStaticBitmap(this, wxID_ANY, bitmap, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    if (!bitmap.IsNull()) {
        auto sz = bitmap.GetSize();
        m_bitmap->SetSize(bitmap.GetSize());
        m_bitmap->SetMinSize(bitmap.GetSize());
    }
    m_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_sizer->Add(m_text, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(3));
    m_sizer->AddSpacer(FromDIP(12));
    m_sizer->Add(m_bitmap, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(3));
    m_sizer->AddStretchSpacer(1);

    updateMinSize();
    SetSizer(m_sizer);
    Layout();
    Fit();

    m_text->Bind(wxEVT_LEFT_DOWN, &DropDownButton::onLeftDown, this);
    m_text->Bind(wxEVT_LEFT_UP, &DropDownButton::onLeftUp, this);
    m_bitmap->Bind(wxEVT_LEFT_DOWN, &DropDownButton::onLeftDown, this);
    m_bitmap->Bind(wxEVT_LEFT_UP, &DropDownButton::onLeftUp, this);
}

DropDownButton::~DropDownButton()
{
}

void DropDownButton::setText(const wxString& text)
{
    m_text->SetLabel(text);
    updateMinSize();
}

wxPoint DropDownButton::convertEventPoint(const wxMouseEvent& event)
{
    wxPoint pnt = event.GetPosition();
    if (event.GetId() == m_text->GetId()) {
        pnt += m_text->GetPosition();
    } else if (event.GetId() == m_bitmap->GetId()) {
        pnt += m_bitmap->GetPosition();
    }
    return pnt;
}

void DropDownButton::updateMinSize()
{
    wxScreenDC dc;
    dc.SetFont(GetFont());
    auto text_size = dc.GetTextExtent(m_text->GetLabel());
    int width = text_size.x + 2 * FromDIP(12) + m_bitmap->GetSize().x;
    SetMinSize(wxSize(width, FromDIP(24)));
    Layout();
    Fit();
}

void DropDownButton::onLeftDown(wxMouseEvent& event)
{
    wxMouseEvent e(event);
    e.SetPosition(convertEventPoint(event));
    e.SetEventObject(this);
    wxPostEvent(this, e);
}

void DropDownButton::onLeftUp(wxMouseEvent& event)
{
    wxMouseEvent e(event);
    e.SetPosition(convertEventPoint(event));
    e.SetEventObject(this);
    wxPostEvent(this, e);
}


std::map<unsigned short, wxBitmap> DeviceItemPanel::m_machineBitmapMap;
DeviceItemPanel::DeviceItemPanel(wxWindow *parent) 
    : wxPanel(parent)
{
    SetSize(FromDIP(220), FromDIP(205));
    SetMinSize(wxSize(FromDIP(220), FromDIP(205)));
    SetMaxSize(wxSize(FromDIP(220), FromDIP(205)));

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_main_sizer);
    Layout();

    bindEvent(true);
    Bind(wxEVT_PAINT, &DeviceItemPanel::onPaint, this);
}

DeviceItemPanel::~DeviceItemPanel()
{
    leaveWindow();
}

void DeviceItemPanel::bindEvent(bool bind)
{
    if (bind) {
        Bind(wxEVT_LEFT_DOWN, &DeviceItemPanel::mouseDown, this);
        Bind(wxEVT_LEFT_UP, &DeviceItemPanel::mouseReleased, this);
        Bind(wxEVT_ENTER_WINDOW, &DeviceItemPanel::onEnter, this);
        Bind(wxEVT_LEAVE_WINDOW, &DeviceItemPanel::onLeave, this);
        Bind(wxEVT_MOTION, &DeviceItemPanel::onMotion, this);
    } else {
        Unbind(wxEVT_LEFT_DOWN, &DeviceItemPanel::mouseDown, this);
        Unbind(wxEVT_LEFT_UP, &DeviceItemPanel::mouseReleased, this);
        Unbind(wxEVT_ENTER_WINDOW, &DeviceItemPanel::onEnter, this);
        Unbind(wxEVT_LEAVE_WINDOW, &DeviceItemPanel::onLeave, this);
        Unbind(wxEVT_MOTION, &DeviceItemPanel::onMotion, this);
    }
}

void DeviceItemPanel::blockMouseEvent(bool block)
{
    bindEvent(!block);
}

wxPoint DeviceItemPanel::convertEventPoint(wxMouseEvent& event)
{
    return event.GetPosition();
}

bool DeviceItemPanel::isPointIn(const wxPoint& pt)
{
    return !(wxHT_WINDOW_OUTSIDE == HitTest(pt));
}

void DeviceItemPanel::leaveWindow()
{
    m_hovered = false;
    m_pressed = false;
    Refresh();
}

void DeviceItemPanel::mouseDown(wxMouseEvent& event)
{
    if (isPointIn(convertEventPoint(event))) {
        m_pressed = true;
        Refresh();
    }
    event.Skip();
}

void DeviceItemPanel::mouseReleased(wxMouseEvent& event)
{
    if (m_pressed == false) {
        return;
    }
    if (isPointIn(convertEventPoint(event))) {
        sendEvent();
    }
    m_pressed = false;
    Refresh();
    event.Skip();
}

void DeviceItemPanel::onEnter(wxMouseEvent& event)
{
    if (isPointIn(convertEventPoint(event)) && !m_hovered) {
        m_hovered = true;
        Refresh();
    }
    event.Skip();
}

void DeviceItemPanel::onLeave(wxMouseEvent& event)
{
    //BOOST_LOG_TRIVIAL(info) << "DeviceItemPanel::onLeave";
    //flush_logs();
    if (!isPointIn(convertEventPoint(event))) {
        leaveWindow();
    }
    event.Skip();
}

void DeviceItemPanel::onMotion(wxMouseEvent& event)
{
    if (isPointIn(convertEventPoint(event))) {
        if (!m_hovered) {
            m_hovered = true;
            Refresh();
        }
    } else {
        leaveWindow();
    }
    event.Skip();
}

void DeviceItemPanel::onPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    wxSize sz = GetSize();
#ifdef __WXMSW__
    wxMemoryDC memdc;
    wxBitmap   bmp(sz.x, sz.y);
    memdc.SelectObject(bmp);
    memdc.Blit({0, 0}, sz, &dc, {0, 0});
    {
        wxGCDC dc2(memdc);
        dc2.SetFont(GetFont());
        do_render(dc2);
    }
    memdc.SelectObject(wxNullBitmap);
    dc.DrawBitmap(bmp, 0, 0);
#else
    do_render(dc);
#endif
    event.Skip();
}

void DeviceItemPanel::do_render(wxDC& dc)
{
    wxSize sz = GetSize();
    wxPen  pen;
    pen.SetWidth(2);
    dc.SetBrush(m_bg_color);
    if (m_hovered && m_pressed) {
        pen.SetColour(m_border_press_color);
    } else if (m_hovered) {
        pen.SetColour(m_border_hover_color);
    } else {
        pen.SetColour(m_border_color);
    }
    dc.SetPen(pen);
    dc.DrawRoundedRectangle(1, 1, sz.x-1, sz.y-1, 7);
}


DeviceInfoItemPanel::DeviceInfoItemPanel(wxWindow *parent, const DeviceInfo& info, wxWindow* event_handle/*=nullptr*/)
    : DeviceItemPanel(parent)
    , m_event_handle(event_handle)
{
    Freeze();
    m_name_text = new wxStaticText(this, wxID_ANY, wxT("AD5M"));
    m_name_text->SetBackgroundColour(m_bg_color);
    apply_light_mode_text(m_name_text, wxColour(FF_DEVICE_LIST_PRIMARY_TEXT));
    m_icon = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxSize(FromDIP(112), FromDIP(112)), wxALIGN_LEFT);
    m_icon->SetMinSize(wxSize(FromDIP(112), FromDIP(112)));
    m_icon->SetMaxSize(wxSize(FromDIP(112), FromDIP(112)));
    m_icon->SetBackgroundColour(m_bg_color);

    m_exit_btn = new ScalableButton(this, wxID_ANY, "unbind_select", wxEmptyString, FromDIP(wxSize(20, 20)), wxDefaultPosition,
                                    wxBU_EXACTFIT | wxNO_BORDER, false, 20);
    m_exit_btn->SetBackgroundColour(m_bg_color);
    m_exit_btn->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    m_exit_btn->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_exit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        if (m_unbinding) {
            return;
        }

        m_unbinding = true;
        m_exit_btn->Enable(false);
        auto restore_unbind_button = [this]() {
            m_unbinding = false;
            if (m_exit_btn) {
                m_exit_btn->Enable(true);
            }
        };

        DeviceObjectOpr* devOpr = wxGetApp().getDeviceObjectOpr();
        if (!devOpr) {
            restore_unbind_button();
            return;
        }
        std::map<std::string, DeviceObject*> list;
        list.clear();
        devOpr->get_my_machine_list(list);
        auto it = list.find(m_dev_id);
        if (it == list.end() || it->second == nullptr) {
            restore_unbind_button();
            return;
        }
        if (m_info.lanFlag) {
            MessageDialog msg_wingow(nullptr, _L("Are you sure to unbind this device?"), _L("Question"), wxYES_NO);
            if (wxID_YES == msg_wingow.ShowModal()) {
                devOpr->unbind_lan_machine(it->second);

                MessageDialog msg_wingow1(nullptr, _L("Log out successful."), "", wxAPPLY | wxOK);
                if (msg_wingow1.ShowModal() == wxOK) {
                    return;
                }
            } else {
                restore_unbind_button();
            }
        } else {
            BindInfo*           info = it->second->get_bind_info();
            UnBindMachineDialog dlg;
            dlg.update_device_info2(info);
            if (dlg.ShowModal() != wxID_OK) {
                restore_unbind_button();
            }
            /*if (dlg.ShowModal() == wxID_OK) {
                devOpr->set_selected_machine("");
            }*/
        }
    });

    m_placement_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_placement_text->SetBackgroundColour(m_bg_color);
    apply_light_mode_text(m_placement_text, wxColour(FF_DEVICE_LIST_SECONDARY_TEXT));
    m_status_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_status_text->SetBackgroundColour(m_bg_color);
    m_progress_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxHL_ALIGN_RIGHT);
    m_progress_text->SetBackgroundColour(m_bg_color);
    m_warning_icon = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_warning_icon->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    m_warning_icon->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_warning_icon->SetBackgroundColour(m_bg_color);
    m_warning_icon->SetBitmap(create_scaled_bitmap("ff_warning", this, 20));

    wxBoxSizer* top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->Add(m_name_text, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    top_sizer->AddStretchSpacer(1);
    top_sizer->Add(m_exit_btn, 0, wxEXPAND | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);

    wxBoxSizer* status_sizer = new wxBoxSizer(wxHORIZONTAL);
    status_sizer->Add(m_status_text, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    status_sizer->AddStretchSpacer(1);
    status_sizer->Add(m_progress_text, 0, wxEXPAND | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    status_sizer->Add(m_warning_icon, 0, wxEXPAND | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);

    // Shown only on a tile whose print-host settings entry is missing something.
    m_setup_btn = new wxButton(this, wxID_ANY, _L("Set up"));
    m_setup_btn->SetToolTip(_L("Open the print-host settings to fill in what is missing."));
    m_setup_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_on_setup)
            m_on_setup();
    });
    m_setup_btn->Hide();

    wxBoxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);
    left_sizer->Add(m_placement_text, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    left_sizer->AddSpacer(FromDIP(3));
    left_sizer->Add(status_sizer, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    left_sizer->Add(m_setup_btn, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));

    m_main_sizer->AddSpacer(2);
    m_main_sizer->AddStretchSpacer(1);    
    m_main_sizer->Add(top_sizer, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->AddSpacer(FromDIP(3));
    m_main_sizer->Add(m_icon, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->AddSpacer(FromDIP(3));
    m_main_sizer->Add(left_sizer, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->AddStretchSpacer(1);
    m_main_sizer->AddSpacer(2);
    
    updateInfo(info);
    Layout();
    Thaw();
    bindEvent(true);
}

void DeviceInfoItemPanel::updateInfo(const DeviceInfo& info)
{
    int width = GetSize().x - FromDIP(40);
    wxScreenDC dc;
    dc.SetFont(GetFont());
    wxString name = FFUtils::trimString(dc, wxString::FromUTF8(info.name), width);
    m_name_text->SetLabelText(name);
    //if (info.placement.empty()) {
    //    m_placement_text->SetLabel(_L("Default"));
    //} else {
    wxString placement = FFUtils::trimString(dc, wxString::FromUTF8(info.placement), width);
    if (info.needs_setup)
        placement = FFUtils::trimString(dc, _L("Missing:") + " " + wxString::FromUTF8(info.setup_text.c_str()), width);
    m_placement_text->SetLabel(placement);
    //m_placement_text->SetToolTip(FFUtils::trimString(wxPaintDC(m_placement_text), wxString::FromUTF8(info.placement), m_placement_text->GetSizer()->~wxClientDataContainer));
    //}
    if (info.pid != m_info.pid) {
        m_icon->SetBitmap(machineBitmap(info.pid));
    }
    m_info = info;
    // A settings-owned printer is removed or edited in the settings, not unbound here.
    m_exit_btn->Show(!info.from_settings);
    m_setup_btn->Show(info.needs_setup);
    updateStatus();
    Layout();
}

const DeviceInfoItemPanel::DeviceInfo& DeviceInfoItemPanel::deviceInfo() const
{
    return m_info;
}

void DeviceInfoItemPanel::bindEvent(bool bind)
{
    if (bind) {
        m_name_text->Bind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_name_text->Bind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_name_text->Bind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_name_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_icon->Bind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_icon->Bind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_icon->Bind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_icon->Bind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);    
        m_placement_text->Bind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_placement_text->Bind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_placement_text->Bind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_placement_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_status_text->Bind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_status_text->Bind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_status_text->Bind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_status_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_name_text->Bind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
        m_icon->Bind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);    
        m_placement_text->Bind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
        m_status_text->Bind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
    } else {
        m_name_text->Unbind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_name_text->Unbind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_name_text->Unbind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_name_text->Unbind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_icon->Unbind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_icon->Unbind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_icon->Unbind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_icon->Unbind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);    
        m_placement_text->Unbind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_placement_text->Unbind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_placement_text->Unbind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_placement_text->Unbind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_status_text->Unbind(wxEVT_LEFT_DOWN, &DeviceInfoItemPanel::mouseDown, this);
        m_status_text->Unbind(wxEVT_LEFT_UP, &DeviceInfoItemPanel::mouseReleased, this);
        m_status_text->Unbind(wxEVT_ENTER_WINDOW, &DeviceInfoItemPanel::onEnter, this);
        m_status_text->Unbind(wxEVT_LEAVE_WINDOW, &DeviceInfoItemPanel::onLeave, this);
        m_name_text->Unbind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
        m_icon->Unbind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);    
        m_placement_text->Unbind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
        m_status_text->Unbind(wxEVT_MOTION, &DeviceInfoItemPanel::onMotion, this);
    }
}

void DeviceInfoItemPanel::blockMouseEvent(bool block)
{
    bindEvent(!block);
    DeviceItemPanel::blockMouseEvent(block);
}

void DeviceInfoItemPanel::setDevId(const std::string& id) 
{ 
    m_dev_id = id; 
}

wxPoint DeviceInfoItemPanel::convertEventPoint(wxMouseEvent& event)
{
    wxPoint pnt = event.GetPosition();
    if (event.GetEventObject() == m_name_text) {
        pnt += m_name_text->GetPosition();
    } else if (event.GetEventObject() == m_icon) {
        pnt += m_icon->GetPosition();
    } else if (event.GetEventObject() == m_placement_text) {
        pnt += m_placement_text->GetPosition();
    } else if (event.GetEventObject() == m_status_text) {
        pnt += m_status_text->GetPosition();
    }
    return pnt;
}

void DeviceInfoItemPanel::sendEvent()
{
    if (m_info.needs_setup) {
        if (m_on_setup)
            m_on_setup();
        return;
    }
    if (m_info.conn_id >= 0 && !m_info.status.empty() && m_info.status != "offline" && m_event_handle) {
        wxGetApp().mainframe->jump_to_monitor(EVT_SWITCH_TO_DEVICE_STATUS, m_info.conn_id);
    }
}

void DeviceInfoItemPanel::updateStatus()
{
    if (m_info.conn_id < 0) {
        m_info.status = "offline";
    }
    // build plate detect, first-layer defect
    m_warning_icon->Show("error" == m_info.status && ("E0088" == m_info.errorCode || "E0089" == m_info.errorCode));
    wxColour color("#00CD6D");
    wxString status = FFUtils::convertStatus(m_info.status, color);
    if (m_info.needs_setup) {
        status = _L("Needs setup");
        color  = wxColour("#E67E00");
    }
    m_status_text->SetLabel(status);
    m_status_text->SetForegroundColour(color);
    if ("printing" == m_info.status) {
        m_progress_text->Show(true);
        m_progress_text->SetLabel(wxString::Format("%d%s", m_info.progress, "%"));
        m_progress_text->SetForegroundColour(color);
    } else {
        m_progress_text->Show(false);
    }
}

wxBitmap DeviceInfoItemPanel::machineBitmap(unsigned short pid)
{
    wxBitmap bmp;
    auto iter = m_machineBitmapMap.find(pid);
    if (iter == m_machineBitmapMap.end()) {
        wxString bmp_str = FFUtils::getBitmapFileName(pid);
        if (!bmp_str.empty()) {
            bmp = create_scaled_bitmap(bmp_str.ToStdString(), 0, 120);
            m_machineBitmapMap[pid] = bmp;
        }
    } else {
        bmp = iter->second;
    }
    return bmp;
}


DeviceStaticItemPanel::DeviceStaticItemPanel(wxWindow* parent, std::string status, int count)
    : DeviceItemPanel(parent)
    , m_count(count)
    , m_status(status)
{
    Freeze();
    wxColour color;
    wxString wxstatus = FFUtils::convertStatus(m_status, color);
    m_status_text = new wxStaticText(this, wxID_ANY, wxstatus, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_status_text->SetForegroundColour(color);
    m_status_text->SetBackgroundColour(wxColour("#ffffff"));
    m_count_text = new wxStaticText(this, wxID_ANY, wxString::Format("%d", m_count), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_count_text->SetFont(::Label::Head_18);
    m_count_text->SetBackgroundColour(wxColour("#ffffff"));
    apply_light_mode_text(m_count_text, wxColour(FF_DEVICE_LIST_PRIMARY_TEXT));

    m_main_sizer->AddStretchSpacer(1);
    m_main_sizer->Add(m_status_text, 0, wxEXPAND | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->AddSpacer(FromDIP(40));
    m_main_sizer->Add(m_count_text, 0, wxEXPAND | wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->AddSpacer(FromDIP(50));
    m_main_sizer->AddStretchSpacer(1);
    Layout();
    Thaw();
    m_status_text->Bind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
    m_status_text->Bind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
    m_status_text->Bind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
    m_status_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
    m_count_text->Bind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
    m_count_text->Bind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
    m_count_text->Bind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
    m_count_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
}

void DeviceStaticItemPanel::setStatus(const std::string& status)
{
    wxColour color;
    wxString wxstatus = FFUtils::convertStatus(m_status, color);
    m_status_text->SetLabel(wxstatus);
    m_status_text->SetBackgroundColour(color);
    Layout();
}
void DeviceStaticItemPanel::setCount(int count)
{
    m_count = count;
    m_count_text->SetLabel(wxString::Format("%d", m_count));
    Layout();
}

void DeviceStaticItemPanel::bindEvent(bool bind)
{
    if (bind) {
        m_status_text->Bind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
        m_status_text->Bind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
        m_status_text->Bind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
        m_status_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
        m_status_text->Bind(wxEVT_MOTION, &DeviceStaticItemPanel::onMotion, this);
        m_count_text->Bind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
        m_count_text->Bind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
        m_count_text->Bind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
        m_count_text->Bind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
        m_count_text->Bind(wxEVT_MOTION, &DeviceStaticItemPanel::onMotion, this);

    } else {
        m_status_text->Unbind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
        m_status_text->Unbind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
        m_status_text->Unbind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
        m_status_text->Unbind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
        m_status_text->Unbind(wxEVT_MOTION, &DeviceStaticItemPanel::onMotion, this);
        m_count_text->Unbind(wxEVT_LEFT_DOWN, &DeviceStaticItemPanel::mouseDown, this);
        m_count_text->Unbind(wxEVT_LEFT_UP, &DeviceStaticItemPanel::mouseReleased, this);
        m_count_text->Unbind(wxEVT_ENTER_WINDOW, &DeviceStaticItemPanel::onEnter, this);
        m_count_text->Unbind(wxEVT_LEAVE_WINDOW, &DeviceStaticItemPanel::onLeave, this);
        m_count_text->Unbind(wxEVT_MOTION, &DeviceStaticItemPanel::onMotion, this);
    }
}

void DeviceStaticItemPanel::blockMouseEvent(bool block)
{
    bindEvent(!block);
    DeviceItemPanel::blockMouseEvent(block);
}

wxPoint DeviceStaticItemPanel::convertEventPoint(wxMouseEvent& event)
{
    wxPoint pnt = event.GetPosition();
    if (event.GetEventObject() == m_status_text) {
        pnt += m_status_text->GetPosition();
    } else if (event.GetEventObject() == m_count_text) {
        pnt += m_count_text->GetPosition();
    }
    return pnt;
}


int DeviceListPanel::m_last_priority_id = 0;
DeviceListPanel::DeviceListPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, 
        const wxSize& size)
    : wxPanel(parent, id, pos, size, wxTAB_TRAVERSAL)
    , m_filter_popup(new DeviceFilterPopupWindow(this))
{
    this->SetBackgroundColour(0xEEEEEE);

    build();
    connectEvent();
    initDeviceList();
}

DeviceListPanel::~DeviceListPanel()
{
    
}

void DeviceListPanel::msw_rescale()
{
	
}

void DeviceListPanel::build()
{
    //SetScrollRate(5, 5);
    //m_filter_popup = new FilterPopupWindow(this);
    m_default_filter_item = new DeviceFilterItem(m_filter_popup, _L("All"), true);
    m_default_filter_item->Bind(EVT_DEVICE_FILTER_ITEM_CLICKED, &DeviceListPanel::onFilterItemClicked, this);
    m_default_filter_item->Show(false);
    SetBackgroundColour(wxColour("#F0F0F0"));
    m_placement_btn = new DropDownButton(this, _L("All"), create_scaled_bitmap("device_dropdown", this, 8));
    m_status_btn  = new DropDownButton(this, _L("Status"), create_scaled_bitmap("device_dropdown", this, 8));
    m_type_btn = new DropDownButton(this, _L("Machine Type"), create_scaled_bitmap("device_dropdown", this, 8));
    m_wlan_btn = new FFToggleButton(this, _L("Network"));
    m_wlan_btn->SetValue(true);
    m_lan_btn = new FFToggleButton(this, _L("LAN"));
    m_lan_btn->SetValue(true);
    m_static_btn = new FFBitmapToggleButton(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
    m_static_btn->setNormalBitmap(create_scaled_bitmap("device_more", this, 16));
    m_static_btn->setNormalHoverBitmap(create_scaled_bitmap("device_more_hover", this, 16));
    m_static_btn->setNormalPressBitmap(create_scaled_bitmap("device_more_press", this, 16));
    m_static_btn->setSelectBitmap(create_scaled_bitmap("device_back", this, 16));
    m_static_btn->setSelectHoverBitmap(create_scaled_bitmap("device_back_hover", this, 16));
    m_static_btn->setSelectPressBitmap(create_scaled_bitmap("device_back_press", this, 16));
    m_static_btn->SetValue(false);

    wxPanel *lan_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
    lan_line->SetBackgroundColour("#666666");

    wxBoxSizer* hTopSizer = new wxBoxSizer(wxHORIZONTAL);
    hTopSizer->Add(m_placement_btn, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
    hTopSizer->AddSpacer(FromDIP(50));
    hTopSizer->Add(m_status_btn, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
    hTopSizer->AddSpacer(FromDIP(50));
    hTopSizer->Add(m_type_btn, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
    hTopSizer->AddStretchSpacer(1);
    hTopSizer->Add(m_wlan_btn, 0, wxALIGN_CENTER_VERTICAL);
    hTopSizer->AddSpacer(FromDIP(10));
    hTopSizer->Add(lan_line, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 3);
    hTopSizer->AddSpacer(FromDIP(10));
    hTopSizer->Add(m_lan_btn, 0, wxALIGN_CENTER_VERTICAL);
    hTopSizer->AddSpacer(FromDIP(30));
    hTopSizer->Add(m_static_btn, 0, wxALIGN_CENTER_VERTICAL);
    hTopSizer->AddSpacer(FromDIP(20));
    m_add_btn = new wxButton(this, wxID_ANY, _L("Add printer"));
    m_add_btn->SetToolTip(_L("Add a FlashForge printer by its serial number, IP address and check code, or search the network for it."));
    hTopSizer->Add(m_add_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_test_btn = new wxButton(this, wxID_ANY, _L("Test connection"));
    m_test_btn->SetToolTip(_L("Check whether a FlashForge printer answers on the network, and "
                              "collect diagnostics if it does not. No print job is started."));
    hTopSizer->Add(m_test_btn, 0, wxALIGN_CENTER_VERTICAL);

    wxString homePageUrl = wxGetApp().get_homepage_url();
    if (homePageUrl.EndsWith("/") || homePageUrl.EndsWith("/home")) {
        homePageUrl = homePageUrl.Mid(0, homePageUrl.find_last_of('/'));
    }   
    m_webBanner                     = WebView::CreateWebView(this, homePageUrl + "/sliceBanner");
    std::string homePageEnableDebug = wxGetApp().app_config->get("home_page_enable_debug");
    m_webBanner->EnableAccessToDevTools(homePageEnableDebug == "true" || homePageEnableDebug == "1");
    m_webBanner->SetMinSize(wxSize(-1, FromDIP(128)));
    m_webBanner->Hide();
    m_webBanner->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& evt) { 
        wxLaunchDefaultBrowser(evt.GetURL(), wxBROWSER_NEW_WINDOW);
    });
    m_webBanner->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [=](wxWebViewEvent& evt) { 
        std::string response = wxGetApp().handle_web_request(evt.GetString().ToUTF8().data(), {"judge_banner_exist", "banner_get_token"});
        wxString    resp     = response;
        if (resp.StartsWith("banner")) {
            int exist = resp[resp.size() - 1] - '0';
            if (exist == 1) {
                m_webBanner->Show();
            } else {
                m_webBanner->Hide();
            }
        }
    });
    wxGetApp().Bind(EVT_BANNER_UPDATE, [=](wxCommandEvent& event) { 
        bool b = event.GetInt();
        nlohmann::json json;
        json["command"]          = "studio_bannerToken";
        std::string access_token = wxGetApp().app_config->get("access_token");
        json["data"]["token"]    = b ? access_token : "";
        json["sequence_id"]      = "10001";
        std::string jsonStr      = json.dump();
        wxString    strJS        = wxString::Format("window.postMessage(%s)", wxString::FromUTF8(jsonStr));
        CallAfter([this, strJS]() { 
            m_webBanner->RunScript(strJS); 
        });
    });

    m_simple_book = new wxSimplebook(this);
    // no device panel
    m_no_device_panel = new wxPanel(m_simple_book);
    m_no_device_bitmap = new wxStaticBitmap(m_no_device_panel, wxID_ANY, wxNullBitmap, wxDefaultPosition, wxDefaultSize, 0);
    m_no_device_bitmap->SetBitmap(create_scaled_bitmap("monitor_device_empty", nullptr, 250));
    m_no_device_staticText = new wxStaticText(m_no_device_panel, wxID_ANY,
        _L("No FlashForge printers yet. Use Add printer (serial number, IP address and check code) to add one.") );
    m_no_device_staticText->Wrap(-1);
    m_no_device_staticText->SetForegroundColour("#909090");
    apply_light_mode_text(m_no_device_staticText, wxColour(FF_DEVICE_LIST_MUTED_TEXT));
    m_no_device_sizer = new wxBoxSizer(wxVERTICAL);
    m_no_device_sizer->AddStretchSpacer();
    m_no_device_sizer->Add(m_no_device_bitmap, 0, wxALIGN_CENTER_HORIZONTAL);
    m_no_device_sizer->AddSpacer(20);
    m_no_device_sizer->Add(m_no_device_staticText, 1, wxALL | wxALIGN_CENTER_HORIZONTAL);
    m_no_device_sizer->AddStretchSpacer();
    m_no_device_panel->SetSizer(m_no_device_sizer);
    m_no_device_panel->Layout();
    m_simple_book->AddPage(m_no_device_panel, wxEmptyString, true);

    m_device_scrolled_window = new wxScrolledWindow(m_simple_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_device_scrolled_window->SetScrollRate(50, 50);
    m_device_panel = new wxPanel(m_device_scrolled_window);
    wxBoxSizer* scroll_sizer = new wxBoxSizer(wxVERTICAL);
    scroll_sizer->AddSpacer(FromDIP(30));
    scroll_sizer->Add(m_device_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));
    scroll_sizer->AddSpacer(FromDIP(30));
    m_device_scrolled_window->SetSizer(scroll_sizer);
    //m_device_window->EnableScrolling(true, true);
    //m_device_window->SetScrollRate(0, 10);
    wxBoxSizer* device_sizer = new wxBoxSizer(wxVERTICAL);
    //m_device_panel->SetBackgroundColour(wxColour("#F0F0F0"));
    m_device_sizer = new wxGridSizer(5);
    m_device_sizer->SetHGap(FromDIP(30));
    m_device_sizer->SetVGap(FromDIP(30));
    device_sizer->Add(m_device_sizer, 0, wxALIGN_LEFT | wxALIGN_TOP);
    m_device_panel->SetSizer(device_sizer);
    m_simple_book->AddPage(m_device_scrolled_window, wxEmptyString, false);

    wxPanel *hor_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    hor_line->SetBackgroundColour("#ffffff");
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(hTopSizer, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(30));
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(hor_line, 0, wxALIGN_CENTER_HORIZONTAL | wxEXPAND | wxLEFT | wxRIGHT, FromDIP(30));
    //sizer->AddSpacer(FromDIP(30));
    sizer->Add(m_webBanner, 0, wxEXPAND);
    sizer->Add(m_simple_book, 1, wxEXPAND | wxALIGN_CENTER_HORIZONTAL);
    //sizer->AddSpacer(FromDIP(30));
    //sizer->Add(m_machinePanel, 1, wxEXPAND);

    this->SetSizer(sizer);
    this->Layout();
    this->Fit();
}

void DeviceListPanel::OnActivate()
{
    updateDeviceList();
}

void DeviceListPanel::connectEvent()
{
    m_placement_btn->Bind(wxEVT_LEFT_DOWN, &DeviceListPanel::onFilterButtonClicked, this);
    m_status_btn->Bind(wxEVT_LEFT_DOWN, &DeviceListPanel::onFilterButtonClicked, this);
    m_type_btn->Bind(wxEVT_LEFT_DOWN, &DeviceListPanel::onFilterButtonClicked, this);
    m_wlan_btn->Bind(wxEVT_TOGGLEBUTTON, &DeviceListPanel::onNetworkTypeToggled, this);
    m_lan_btn->Bind(wxEVT_TOGGLEBUTTON, &DeviceListPanel::onNetworkTypeToggled, this);
    m_static_btn->Bind(wxEVT_TOGGLEBUTTON, &DeviceListPanel::onStaticModeToggled, this);
    m_test_btn->Bind(wxEVT_BUTTON, &DeviceListPanel::onTestConnection, this);
    m_add_btn->Bind(wxEVT_BUTTON, &DeviceListPanel::onAddPrinter, this);
    MultiComMgr::inst()->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &DeviceListPanel::onComDevDetailUpdate, this);
    MultiComMgr::inst()->Bind(COM_WAN_DEV_INFO_UPDATE_EVENT, &DeviceListPanel::onComWanDeviceInfoUpdate, this);
    wxGetApp().getDeviceObjectOpr()->Bind(EVT_DEVICE_LIST_UPDATED, &DeviceListPanel::onDeviceListUpdated, this);
    wxGetApp().getDeviceObjectOpr()->Bind(EVT_LOCAL_DEVICE_NAME_CHANGED, &DeviceListPanel::onLocalDeviceNameChanged, this);
    m_filter_popup->Bind(wxEVT_SHOW, &DeviceListPanel::onPopupShow, this);
    m_refresh_timer.Bind(wxEVT_TIMER, &DeviceListPanel::onRefreshTimeout, this);
}

void DeviceListPanel::initLocalDevice(std::map<std::string, DeviceInfoItemPanel::DeviceInfo>& deviceInfoMap)
{
    deviceInfoMap.clear();
    AppConfig *config = wxGetApp().app_config;
    if (config) {
        std::vector<MacInfoMap> macInfo;
        config->get_local_mahcines(macInfo);
        // get_local_mahcines() already leaves out the Bambu LAN printers that share the table.
        // A fresh DeviceInfo per row: the old shared one carried the previous row's name, placement
        // and product id into any row that lacked its own.
        for (auto& mac : macInfo) {
            auto it = mac.find("dev_id");
            if (it != mac.end()) {
                DeviceInfoItemPanel::DeviceInfo dev_info;
                dev_info.lanFlag = true;
                dev_info.status = "offline";
                std::string dev_id = it->second;
                it = mac.find("dev_name");
                if (it != mac.end()) {
                    dev_info.name = it->second;
                }
                it = mac.find("dev_placement");
                if (it != mac.end()) {
                    dev_info.placement = it->second;
                }
                it = mac.find("dev_pid");
                if (it != mac.end()) {
                    try {
                        dev_info.pid = (unsigned short)std::stoi(it->second);
                    } catch (const std::exception&) {
                        dev_info.pid = 0;
                    }
                }
                if (deviceInfoMap.find(dev_id) == deviceInfoMap.end()) {
                    deviceInfoMap.emplace(dev_id, dev_info);
                }
            }
        }
        BOOST_LOG_TRIVIAL(warning) << "[FlashForge] Device tab: " << deviceInfoMap.size()
                                   << " saved FlashForge printer(s) in the list ("
                                   << config->get_local_machines().size() << " saved LAN printers in all, the rest are Bambu)";
    }
}

void DeviceListPanel::initWlanDevice(std::map<std::string, DeviceInfoItemPanel::DeviceInfo>& deviceInfoMap)
{
    com_id_list_t idList = MultiComMgr::inst()->getReadyDevList();
    for (const auto& id : idList) {
        DeviceInfoItemPanel::DeviceInfo dev_info;
        if (getDeviceInfo(dev_info, id)) {
            bool valid = false;
            const auto& data = MultiComMgr::inst()->devData(id, &valid);
            std::string dev_id = (COM_CONNECT_LAN == data.connectMode) ? data.lanDevInfo.serialNumber : data.wanDevInfo.serialNumber;
            auto iter = deviceInfoMap.find(dev_id);
            if (iter == deviceInfoMap.end()) {
                deviceInfoMap.emplace(std::make_pair(dev_id, dev_info));
            } else {// if (dev_info.status != "offline" || dev_info.lanFlag) {
                iter->second = dev_info;
            }
        }
    }
}

void DeviceListPanel::initDeviceList()
{
    m_last_priority_id = 0;
    std::map<std::string, DeviceInfoItemPanel::DeviceInfo> devList;
    initLocalDevice(devList);
    initWlanDevice(devList);  

    if (devList.empty()) {
        m_simple_book->ChangeSelection(0);
        return;
    }
    m_simple_book->ChangeSelection(1);

    std::vector<std::string> devKeyList;
    for (const auto& it: devList) {
        devKeyList.emplace_back(it.first);
    }
    std::sort(devKeyList.begin(), devKeyList.end(), [&devList](auto& a, auto&b) {
        return devList[a].name.compare(devList[b].name) < 0;
    });
    //m_device_map
    for (const auto& iter : devKeyList) {
        DeviceKey key(generateNewPriorityId(), iter, devList[iter].name);
        DeviceInfoItemPanel* item = new DeviceInfoItemPanel(m_device_panel, devList[iter], this);
        item->setDevId(key.dev_id);
        item->Show(false);
        m_device_map.emplace(std::make_pair(key, item));
        //m_device_sizer->Add(item);
    }
    //std::sort(m_device_map.begin(), m_device_map.end(), deviceKeySortFunc);
    // m_device_stat_map
    std::map<std::string, int> statusMap;
    for (const auto& dev : devList) {
        auto iter = statusMap.find(dev.second.status);
        if (iter != statusMap.end()) {
            iter->second += 1;   
        } else {
            statusMap.emplace(std::make_pair(dev.second.status, 1));
        }
    }
    for (const auto& iter : statusMap) {
        auto item = new DeviceStaticItemPanel(m_device_panel, iter.first, iter.second);
        item->Show(false);
        m_device_stat_map.emplace(std::make_pair(iter.first, item));
    }

    //m_device_window->Layout();
    //Layout();
    if (!m_device_map.empty()) {
        updateFilterMap();
    }
    filterDeviceList();
    updateDeviceWindowSize();
}

void DeviceListPanel::filterDeviceList()
{
    if (m_static_btn->GetValue()) {
        return;
    }
    Freeze();
    for (const auto& iter : m_device_stat_map) {
        iter.second->Show(false);
    }
    DeviceKeySet device_key_set;
    for (const auto& iter : m_device_map) {
        device_key_set.emplace(iter.first);
    }

    m_device_sizer->Clear();
    for (const auto& key : device_key_set) {
        const auto& iter = m_device_map.find(key);
        if (iter == m_device_map.end()) {
            continue;
        }
        const auto& dev_info = iter->second->deviceInfo();
        //std::string type_str = FFUtils::getPrinterName(dev_info.pid);
        if ((m_filter_placement_default || dev_info.placement == m_filter_placement)
            && (m_filter_status_default || FFUtils::convertStatus(dev_info.status) == m_filter_status)
            && (m_filter_types.find(dev_info.pid) != m_filter_types.end())
            && ((m_wlan_btn->GetValue() && !dev_info.lanFlag) || (m_lan_btn->GetValue() && dev_info.lanFlag))) {
            m_device_sizer->Add(iter->second);
            iter->second->Show(true);
        } else {
            iter->second->Show(false);
        }
    }    
    updateDeviceWindowSize();
    m_device_panel->Layout();
    Layout();
    //Fit();
    Thaw();
}

bool DeviceListPanel::updateFilterMap()
{
    bool refresh = false;
    refresh = updatePlacementMap();
    if (updateStatusMap()) {
        refresh = true;
    }
    updateTypeMap();
    return refresh;
}

bool DeviceListPanel::updatePlacementMap()
{
    PlacementItemMap backMap;
    for (const auto& iter : m_placement_item_map) {
        backMap.emplace(std::make_pair(iter.first, iter.second));
    }
    bool default_exist = false;
    m_placement_item_map.clear();
    for (auto& iter : m_device_map) {
        std::string placement = iter.second->deviceInfo().placement;
        auto it = backMap.find(placement);
        if (it != backMap.end()) {
            m_placement_item_map.emplace(std::make_pair(placement, it->second));
            it->second = nullptr;
        } else if (m_placement_item_map.find(placement) == m_placement_item_map.end()) {
            auto item = new DeviceFilterItem(m_filter_popup, placement);
            item->Show(false);
            item->Bind(EVT_DEVICE_FILTER_ITEM_CLICKED, &DeviceListPanel::onFilterItemClicked, this);
            m_placement_item_map.emplace(std::make_pair(placement, item));
        }
        if (m_filter_placement == placement) {
            default_exist = true;
        }
    }
    for (auto& iter : backMap) {
        if (iter.second) {
            iter.second->Destroy();
            iter.second = nullptr;
        }
    }
//    if (!default_exist) {
//        if (!m_filter_placement_default) {
 //           m_filter_placement_default = true;
//            m_filter_placement = "";
//            m_filter_placement_trimmed = "";
//            updateFilterTitle();
//        }
//        updateFilterTitle();
//        filterDeviceList();
//    }
    backMap.clear();
    return !default_exist;
}

bool DeviceListPanel::updateStatusMap()
{
    for (auto& iter : m_status_item_map) {
        iter.second->SetValid(false);
    }
    bool default_exist = false;
    for (auto& iter : m_device_map) {
        wxString status = FFUtils::convertStatus(iter.second->deviceInfo().status);
        auto status_iter = m_status_item_map.find(status);
        if (status_iter == m_status_item_map.end()) {
            auto item = new DeviceStatusFilterItem(m_filter_popup, status);
            item->Bind(EVT_DEVICE_FILTER_ITEM_CLICKED, &DeviceListPanel::onFilterItemClicked, this);
            item->Show(false);
            item->SetValid(true);
            m_status_item_map.emplace(std::make_pair(status, item));
        } else {
            status_iter->second->SetValid(true);
        }
        if (m_filter_status == status) {
            default_exist = true;
        }
    }
    return !default_exist;
    if (!default_exist) {
        m_filter_status_default = true;
        m_filter_status = "";
        filterDeviceList();
    }
}

void DeviceListPanel::updateTypeMap()
{
    if (m_filter_popup->IsShown()) {
        m_filter_popup->Dismiss();
    }
    for (auto& iter : m_type_item_map) {
        iter.second->SetValid(false);
    }
    m_filter_types.clear();
    for (auto& iter : m_device_map) {
        auto pid = iter.second->deviceInfo().pid;
        auto type_iter = m_type_item_map.find(pid);
        if (type_iter == m_type_item_map.end()) {
            auto item = new DeviceTypeFilterItem(m_filter_popup, pid, false, false, false);
            item->Bind(EVT_DEVICE_FILTER_ITEM_CLICKED, &DeviceListPanel::onFilterItemClicked, this);
            item->Show(false);
            m_type_item_map.emplace(std::make_pair(pid, item));
        } else {
            type_iter->second->SetValid(true);
        }
        if (m_filter_types.find(pid) == m_filter_types.end()) {
            m_filter_types.emplace(pid);
        }
    }
}

void DeviceListPanel::updateFilterTitle()   
{
    if (m_filter_placement_default) {
        m_placement_btn->setText(_L("All"));
    } else {
        m_placement_btn->setText(m_filter_placement_trimmed);
    }
    Layout();
}

void DeviceListPanel::updateStaticMap()
{
    std::map<std::string, int> statusMap;
    DeviceItemMapSort  deviceMapSort;
    for (auto it : m_device_map) {
        deviceMapSort.emplace(it);
    }
    for (const auto& dev : deviceMapSort) {
        const auto& status = dev.second->deviceInfo().status;
        auto iter = statusMap.find(status);
        if (iter != statusMap.end()) {
            iter->second += 1;   
        } else {
            statusMap.emplace(std::make_pair(status, 1));
        }
    }
    for (auto& iter : m_device_stat_map) {
        iter.second->setCount(0);
    }
    for (const auto& iter : statusMap) {
        auto it = m_device_stat_map.find(iter.first);
        if (it != m_device_stat_map.end()) {
            it->second->setCount(iter.second);
            m_device_stat_map.emplace(std::make_pair(iter.first, it->second));
        } else {
            auto item = new DeviceStaticItemPanel(m_device_panel, iter.first, iter.second);
            item->Show(false);
            m_device_stat_map.emplace(std::make_pair(iter.first, item));
        }
    }
}

void DeviceListPanel::updateDeviceSizer()
{
    if (m_simple_book->GetSelection() == 0) {
        return;
    }
    if (m_static_btn->GetValue()) {
        Freeze();
        m_device_sizer->Clear();
        for (const auto& iter : m_device_map) {
            iter.second->Show(false);
        }
        for (const auto& iter : m_device_stat_map) {
            if (iter.second->getCount() > 0) {
                m_device_sizer->Add(iter.second);
                iter.second->Show(true);
            }
        }
        updateDeviceWindowSize();
        m_device_panel->Layout();
        Layout();
        //Fit();
        Thaw();
    } else {
        filterDeviceList();
    }
    if (IsShownOnScreen()) {
        Refresh();
    }
}

void DeviceListPanel::updateDeviceWindowSize()
{
    int count = 0;
    count = m_device_sizer->GetItemCount();
    int width = FromDIP(5 * 220 + 4 * 30);
    int hcnt = count / 5;
    if (hcnt * 5 < count) {
        hcnt += 1;
    }
    if (hcnt == 0) {
        hcnt = 1;
    }
    int height = FromDIP(hcnt * 205 + (hcnt - 1) * 30);
    
    auto last_min = m_device_panel->GetMinSize();
    if (last_min.x != width || last_min.y != height) {
        int space = FromDIP(30);
        m_device_panel->SetMinSize(wxSize(width, height));
        auto sz = m_device_scrolled_window->GetSize();
        sz.x -= 2 * space;
        sz.y -= 2 * space;        
        if (width <= sz.x && height <= sz.x) {
            m_device_panel->SetSize(sz);
        } else {
            m_device_panel->SetSize(width, height);
        }
        m_device_scrolled_window->SetVirtualSize(wxSize(width+2*space, height+2*space));
        m_device_panel->Layout();
    }    
    //Layout();
    //Fit();
}

void DeviceListPanel::onPopupShow(wxShowEvent& event)
{
    bool show = event.IsShown();
    //BOOST_LOG_TRIVIAL(info) << "onPopupShow: " << show ? "true" : "false";
    //flush_logs();
    for (auto& iter : m_device_map)  {
        if (iter.second) {
            iter.second->blockMouseEvent(show);
        }
    }
}

void DeviceListPanel::onFilterItemClicked(DeviceFilterEvent& event)
{
    if (Filter_Popup_Type_Placement == m_filter_popup_type) {
        if (event.eventObject == m_default_filter_item) {
            m_filter_placement_default = true;
            m_filter_placement = "";
            m_filter_placement_trimmed = "";
        } else {
            m_filter_placement_default = false;
            m_filter_placement = event.fullStringValue.ToStdString();
            m_filter_placement_trimmed = event.elidedStringValue.ToStdString();
        }
        m_filter_popup->Dismiss();
    } else if (Filter_Popup_Type_Status == m_filter_popup_type) {
        if (event.eventObject == m_default_filter_item) {
            m_filter_status_default = true;
            m_filter_status = "";
        } else {
            m_filter_status_default = false;
            m_filter_status = event.fullStringValue;
        }
        m_filter_popup->Dismiss();
    } else if (Filter_Popup_Type_Device_Type == m_filter_popup_type) {
        unsigned short pid = (unsigned short)event.intValue;
        DeviceTypeFilterItem* item = (DeviceTypeFilterItem*)event.eventObject;
        if (item) {
            bool check = item->IsChecked();
            auto iter = m_filter_types.find(pid);
            if (check) {
                if (iter == m_filter_types.end()) {
                    m_filter_types.emplace(pid);
                    //BOOST_LOG_TRIVIAL(info) << "add filter type: " << pid;
                    //flush_logs();
                }
            } else {
                if (iter != m_filter_types.end()) {
                    m_filter_types.erase(iter);
                    //BOOST_LOG_TRIVIAL(info) << "erase filter type: " << pid;
                    //flush_logs();
                }
            }
        }
    }
    filterDeviceList();
    updateFilterTitle();
}

void DeviceListPanel::onFilterButtonClicked(wxMouseEvent &event)
{
    wxPoint pos;
    m_filter_popup->ClearItems();
    if (event.GetEventObject() == m_placement_btn) {
        //pos = m_placement_btn->ClientToScreen(wxPoint(0, 0));
        //pos.y += m_placement_btn->GetSize().y + 2;
        m_filter_popup_type = Filter_Popup_Type_Placement;
        //m_default_filter_item->SetBottomCornerRound(m_placement_item_map.empty());
        m_default_filter_item->SetSelect(m_filter_placement_default);
        m_filter_popup->AddItem(m_default_filter_item);
        for (auto& iter : m_placement_item_map) {
            //iter.second->SetBottomCornerRound(false);
            iter.second->SetSelect(!m_filter_placement_default && (iter.first == m_filter_placement));
            m_filter_popup->AddItem(iter.second);
        }
        //if (!m_placement_item_map.empty()) {
        //    m_placement_item_map.rbegin()->second->SetBottomCornerRound(true);
        //}
        //m_filter_popup->Move(pos);
        m_filter_popup->Popup(m_placement_btn);
    } else if (event.GetEventObject() == m_status_btn) {
        m_filter_popup_type = Filter_Popup_Type_Status;
        //m_default_filter_item->SetBottomCornerRound(m_status_item_map.empty());
        m_default_filter_item->SetSelect(m_filter_status_default);
        m_filter_popup->AddItem(m_default_filter_item);
        DeviceStatusFilterItem* last_item = nullptr;
        for (auto& iter : m_status_item_map) {
            if (iter.second->IsValid()) {
                //iter.second->SetBottomCornerRound(false);
                iter.second->SetSelect(!m_filter_status_default && (iter.first == m_filter_status));
                m_filter_popup->AddItem(iter.second);
                last_item = iter.second;
            }
        }
        //if (last_item) {
        //    last_item->SetBottomCornerRound(true);
        //}
        //pos = m_status_btn->ClientToScreen(wxPoint(0, 0));
        //pos.y += m_status_btn->GetSize().y + 2;
        m_filter_popup->Move(pos);
        m_filter_popup->Popup(m_status_btn);
    } else if (event.GetEventObject() == m_type_btn) {
        m_filter_popup_type = Filter_Popup_Type_Device_Type;
        DeviceTypeFilterItem* last_item = nullptr;
        for (auto& iter : m_type_item_map) {
            if (iter.second->IsValid()) {
                //iter.second->SetBottomCornerRound(false);
                iter.second->SetChecked(m_filter_types.find(iter.first) != m_filter_types.end());
                m_filter_popup->AddItem(iter.second);
                last_item = iter.second;
            }
        }
        //if (last_item) {
            //m_type_item_map.begin()->second->SetTopCornerRound(true);
            //m_type_item_map.rbegin()->second->SetBottomCornerRound(true);
        //}
        //pos = m_type_btn->ClientToScreen(wxPoint(0, 0));
        //pos.y += m_type_btn->GetSize().y + 2;
        if (!m_type_item_map.empty()) {
            m_filter_popup->Move(pos);
            m_filter_popup->Popup(m_type_btn);
        }
    }
}

void DeviceListPanel::onNetworkTypeToggled(wxCommandEvent& event)
{
    filterDeviceList();
    event.Skip();
}

void DeviceListPanel::onStaticModeToggled(wxCommandEvent &event)
{
    updateDeviceSizer();
    event.Skip();
}

void DeviceListPanel::copyDeviceInfo(DeviceInfoItemPanel::DeviceInfo& dest, const DeviceInfoItemPanel::DeviceInfo& source)
{
    //if (source.status != "offline" || source.lanFlag) {
    dest.conn_id = source.conn_id;
    dest.lanFlag = source.lanFlag;
    //}
    if (!source.name.empty()) dest.name = source.name;
    if (source.pid > 0) dest.pid = source.pid;
    if (!source.placement.empty()) dest.placement = source.placement;
    if (!source.status.empty()) {
        dest.status = source.status;
    }
    if (dest.conn_id < 0) {
        dest.status = "offline";
    }
    if (dest.status != "offline") {
        dest.errorCode = source.errorCode;
    }
    dest.progress = source.progress;
}

void DeviceListPanel::onDeviceListUpdated(DeviceListUpdateEvent& event)
{
//    bool is_show_on_screen = IsShownOnScreen();
    DeviceCacheData device_data;
    device_data.dev_id = event.GetDeviceId();
    device_data.op = event.GetOperator();

    int conn_id = event.GetConnectionId();
    bool valid = getDeviceInfo(device_data.device_info, conn_id);
    //BOOST_LOG_TRIVIAL(error) << "onDeviceListUpdated: dev_id(" << device_data.dev_id << "), op(" << (int)device_data.op
    //    <<"), name(" << device_data.device_info.name << "), status: (" << device_data.device_info.status <<")";
    auto iter = m_device_data_cached.find(device_data.dev_id);
    if (iter == m_device_data_cached.end()) {
        DeviceKey key(generateNewPriorityId(), device_data.dev_id, device_data.device_info.name);
        m_device_data_cached.emplace(std::make_pair(key, device_data));
    } else {
        auto old_data = iter->second;
        if (DeviceListUpdateEvent::UpdateType::UpdateType_Add == device_data.op) {
            if (DeviceListUpdateEvent::UpdateType::UpdateType_Add == old_data.op
                || DeviceListUpdateEvent::UpdateType::UpdateType_Update == old_data.op) {
                iter->second.op = DeviceListUpdateEvent::UpdateType::UpdateType_Update ;
                copyDeviceInfo(iter->second.device_info, device_data.device_info);
            } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Remove == old_data.op) {
                m_device_data_cached.erase(iter);
            }
        } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Remove == device_data.op) {
            if (DeviceListUpdateEvent::UpdateType::UpdateType_Add == old_data.op) {
                m_device_data_cached.erase(iter);
            } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Update == old_data.op
                || DeviceListUpdateEvent::UpdateType::UpdateType_Remove == old_data.op) {
                copyDeviceInfo(iter->second.device_info, device_data.device_info);
                iter->second.op = DeviceListUpdateEvent::UpdateType::UpdateType_Remove;
            }
        } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Update == device_data.op) {
            if (DeviceListUpdateEvent::UpdateType::UpdateType_Add == old_data.op) {
                copyDeviceInfo(iter->second.device_info, device_data.device_info);
            } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Remove == old_data.op) {
                iter->second.op = DeviceListUpdateEvent::UpdateType::UpdateType_Update;
            } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Update == old_data.op) {
                if (valid) {                    
                    copyDeviceInfo(iter->second.device_info, device_data.device_info);
                } else {
                    iter->second.device_info.conn_id = -1;
                    iter->second.device_info.status = "offline";
                }
                 iter->second.op = DeviceListUpdateEvent::UpdateType::UpdateType_Update; 
            }
        }
    }

    if (/*IsShownOnScreen() && */!m_refresh_timer.IsRunning()) {
        m_refresh_timer.StartOnce(1000);
    }
    event.Skip();

    //if ()
    //m_device_income_cached
}

void DeviceListPanel::onLocalDeviceNameChanged(LocalDeviceNameChangeEvent& event)
{
    const std::string& dev_id = event.dev_id;
    const std::string& dev_name = event.dev_name;
    auto iter = m_device_map.find(dev_id);
    if (iter != m_device_map.end() && iter->second) {
        auto info = iter->second->deviceInfo();
        if (info.lanFlag && info.conn_id == ComInvalidId && info.name != dev_name) {
            info.name = dev_name;
            iter->second->updateInfo(info);
        }
    }
    event.Skip();
}

void DeviceListPanel::onRefreshTimeout(wxTimerEvent& event)
{
    updateDeviceList();
    event.Skip();
}

void DeviceListPanel::updateDeviceList()
{
    if (m_device_data_cached.empty()) {
        return;
    }

    DeviceKeySet device_key_set;
    for (const auto& iter : m_device_data_cached) {
        device_key_set.emplace(iter.first);
    }
    bool refresh_flag = false;
    for (const auto& it : device_key_set) {
        const std::string& dev_id = it.dev_id;
        auto cache_data = m_device_data_cached[it];
        DeviceListUpdateEvent::UpdateType op = cache_data.op;
        auto dev_info = cache_data.device_info;
        if (DeviceListUpdateEvent::UpdateType::UpdateType_Add == op) {
            auto info_iter = m_device_map.find(dev_id);
            if (info_iter == m_device_map.end()) {
                DeviceInfoItemPanel* info_item = new DeviceInfoItemPanel(m_device_panel, dev_info, this);
                info_item->setDevId(dev_id);
                m_device_map.emplace(std::make_pair(it, info_item));
                refresh_flag = true;
            } else {
                auto _dev_info = info_iter->second->deviceInfo();
                if (dev_info.status != "offline" || dev_info.lanFlag) {
                    copyDeviceInfo(_dev_info, dev_info);
                }
                info_iter->second->updateInfo(_dev_info);
            }
        } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Remove == op) {
            auto iter = m_device_map.find(dev_id);
            if (iter != m_device_map.end()) {
                iter->second->blockMouseEvent(true);
                iter->second->Disable();
                iter->second->Destroy();
                m_device_map.erase(iter);
                refresh_flag = true;
            }
        } else if (DeviceListUpdateEvent::UpdateType::UpdateType_Update == op) {
            auto info_iter = m_device_map.find(dev_id);
            if (info_iter == m_device_map.end()) {
                DeviceInfoItemPanel* info_item = new DeviceInfoItemPanel(m_device_panel, dev_info, this);
                info_item->setDevId(dev_id);
                m_device_map.emplace(std::make_pair(it, info_item));
            } else {
                auto _dev_info = info_iter->second->deviceInfo();
                copyDeviceInfo(_dev_info, dev_info);
                info_iter->second->updateInfo(_dev_info);
            }
            refresh_flag = true;
        }
    }
    //std::sort(m_device_map.begin(), m_device_map.end(), deviceKeySortFunc);
    if (updateFilterMap()) {
        refresh_flag = true;
    }
    updateStaticMap();
    m_device_data_cached.clear();
    if (refresh_flag) {
        //if (!m_filter_placement_default) {
        //    m_filter_placement_default = true;
        //    updateFilterTitle();
        //}
        //m_filter_status_default = true;
        m_simple_book->ChangeSelection(m_device_map.empty() ? 0 : 1);
        //updateDeviceWindowSize();
        updateDeviceSizer();
    } 
}

bool DeviceListPanel::getDeviceInfo(DeviceInfoItemPanel::DeviceInfo& info, int conn_id)
{
    bool valid = false;
    const auto& data = MultiComMgr::inst()->devData(conn_id, &valid);
    if (valid) {
        if (COM_CONNECT_LAN == data.connectMode) {
            std::string dev_id = data.lanDevInfo.serialNumber;
            info.lanFlag = true;
            info.conn_id = conn_id;
            if (data.devDetail){
                info.placement = data.devDetail->location;
                info.status    = data.devDetail->status;
                info.errorCode = data.devDetail->errorCode;
                info.name      = data.devDetail->name;
                info.progress  = data.devDetail->printProgress * 100;
            } else {
                info.name = data.lanDevInfo.name;
                info.progress = 0;
            }
            // A printer typed in or taken from the settings has no product id until it answers.
            info.pid = FFUtils::getPid(data);
        } else if (COM_CONNECT_WAN == data.connectMode && valid && data.devDetail) {
            std::string dev_id = data.wanDevInfo.serialNumber;
            info.lanFlag = false;
            info.conn_id = conn_id;
            info.name = data.wanDevInfo.name;        
            info.pid = data.devDetail->pid;
            info.placement = data.wanDevInfo.location;
            info.status = data.wanDevInfo.status;
            if (data.devDetail) {
                info.errorCode = data.devDetail->errorCode;
                info.progress = data.devDetail->printProgress * 100;
            } else {
                info.progress = 0;
            }
        }
        if (info.status.empty()) info.status = "offline";
    }
    return valid;
}

void DeviceListPanel::updateDeviceInfo(const std::string& dev_id, const DeviceInfoItemPanel::DeviceInfo& info)
{
    bool status_changed = false;
    bool placement_changed = false;
    bool type_changed = false;
    bool refresh_list = false;
    auto iter = m_device_map.find(dev_id);
    if (iter != m_device_map.end()) {
        auto dev_info = iter->second->deviceInfo();        
        if (info.lanFlag || info.status != "offline" || !dev_info.lanFlag) {
            if (dev_info.status != info.status) {
                status_changed = true;
                refresh_list = (m_filter_status == FFUtils::convertStatus(dev_info.status)) || (m_filter_status == FFUtils::convertStatus(info.status));
            }
            if (dev_info.placement != info.placement) {
                placement_changed = true;
                refresh_list = (m_filter_placement == dev_info.placement) || (m_filter_placement == info.placement);
            }
            if (dev_info.pid != info.pid) {
                type_changed = true;
            }
            if (status_changed || placement_changed || type_changed
                || dev_info.conn_id != info.conn_id || dev_info.lanFlag != info.lanFlag
                || dev_info.name != info.name || dev_info.progress != info.progress
                || dev_info.errorCode != info.errorCode) {
                // Events know nothing of where the tile came from; keep that.
                DeviceInfoItemPanel::DeviceInfo merged = info;
                merged.from_settings = dev_info.from_settings;
                merged.needs_setup   = dev_info.needs_setup;
                merged.setup_text    = dev_info.setup_text;
                iter->second->updateInfo(merged);
            }
            DeviceObjectOpr* settings_opr = wxGetApp().getDeviceObjectOpr();
            if (info.lanFlag && (dev_info.name != info.name || dev_info.placement != info.placement)
                && !(settings_opr && settings_opr->is_settings_serial(dev_id))) {
                AppConfig* config = GUI::wxGetApp().app_config;
                if (config) {
                    config->save_bind_machine_to_config(dev_id, info.name, info.placement, info.pid);
                }
            }
            if (placement_changed) {
                if (updatePlacementMap()) {
                    refresh_list = true;
                }
            }
            if (type_changed) {
                updateTypeMap();
            }
            if (status_changed) {
                if (updateStatusMap()) {
                    refresh_list = true;
                }
                updateStaticMap();
                if (m_static_btn->GetValue()) {
                    updateDeviceSizer();
                }
            }
            if (!m_static_btn->GetValue() && refresh_list) {
                filterDeviceList();
            }
        }
    }
}

void DeviceListPanel::onComDevDetailUpdate(ComDevDetailUpdateEvent& event)
{
    auto conn_id = event.id;
    bool valid = false;
    const auto& data = MultiComMgr::inst()->devData(conn_id, &valid);
    //BOOST_LOG_TRIVIAL(info) << "onComDevDetailUpdate: " << data.connectMode << ", " << valid ? "valid" : "invalid";
    if (valid) {
        DeviceInfoItemPanel::DeviceInfo info;
        info.lanFlag = COM_CONNECT_LAN == data.connectMode;
        info.conn_id = conn_id;
        if (data.devDetail) {
            info.name = data.devDetail->name;
            info.pid = data.devDetail->pid;
            info.placement = data.devDetail->location;
            info.status = data.devDetail->status;
            info.errorCode = data.devDetail->errorCode;
            info.progress = data.devDetail->printProgress * 100;
        }
        std::string dev_id = info.lanFlag ? data.lanDevInfo.serialNumber : data.wanDevInfo.serialNumber;
        updateDeviceInfo(dev_id, info);
        BOOST_LOG_TRIVIAL(info) << "onComDevDetailUpdate: " << info.name << ", " << info.placement << ", " << info.status;
    }
    flush_logs();
    event.Skip();
}

void DeviceListPanel::onComWanDeviceInfoUpdate(ComWanDevInfoUpdateEvent& event)
{
    auto conn_id = event.id;
    bool valid = false;
    const auto& data = MultiComMgr::inst()->devData(conn_id, &valid);
    BOOST_LOG_TRIVIAL(info) << "onComWanDeviceInfoUpdate: " << data.connectMode << ", " << valid ? "valid" : "invalid";
    if (COM_CONNECT_WAN == data.connectMode && valid && data.devDetail) {
        DeviceInfoItemPanel::DeviceInfo info;
        info.lanFlag = false;
        info.conn_id = conn_id;
        info.name = data.wanDevInfo.name;
        info.placement = data.wanDevInfo.location;
        info.status = data.wanDevInfo.status;
        if (data.devDetail) {
            info.pid = data.devDetail->pid;
            info.errorCode = data.devDetail->errorCode;
            info.progress = data.devDetail->printProgress * 100;
        }
        updateDeviceInfo(data.wanDevInfo.serialNumber, info);
        BOOST_LOG_TRIVIAL(info) << "onComWanDeviceInfoUpdate: " << info.name << ", " << info.placement << ", " << info.status;
    }
    flush_logs();
    event.Skip();
}

int DeviceListPanel::generateNewPriorityId()
{
    return ++m_last_priority_id;
}

void DeviceListPanel::updatePriorityId()
{
    int max = 0;
    for (const auto& iter : m_device_map) {
        if (iter.first.priority > max) {
            max = iter.first.priority;
        }
    }
    m_last_priority_id = max;
}


// Open the LAN connection test, prefilled from the selected device when there is one.
//
// The check code is deliberately NOT prefilled even when we hold one: the point of the test is
// usually to find out whether the stored code is the problem, and showing it back to the user
// invites them to accept it unread.
void DeviceListPanel::onTestConnection(wxCommandEvent &event)
{
    std::string serial;
    std::string ip;
    if (DeviceObjectOpr *opr = wxGetApp().getDeviceObjectOpr()) {
        if (DeviceObject *obj = opr->get_selected_machine()) {
            serial = obj->get_dev_id();
            ip     = obj->get_dev_ip();
        }
    }
    FFDiagnosticsDialog dlg(this, serial, ip);
    dlg.ShowModal();
    event.Skip();
}

// What the print-host settings hold for FlashForge: every device whose host type is Flashforge, in
// every model's list. (The old single-address presets are imported into those lists first, once.)
static std::vector<FFPrinterSource> collect_settings_printers()
{
    std::vector<FFPrinterSource> out;
    try {
        if (PresetBundle* bundle = wxGetApp().preset_bundle)
            PrintHostDevices::migrate_from_presets(*bundle);
        for (const auto& [model, devices] : PrintHostDevices::all_devices()) {
            for (const PrintHostDevices::Device& d : devices) {
                if (PrintHostDevices::host_type_enum(d.host_type) != htFlashforge || d.host_type != PrintHostDevices::host_type_key(htFlashforge))
                    continue;
                FFPrinterSource s;
                s.name       = d.display_name();
                s.address    = d.address;
                s.serial     = d.serial;
                s.check_code = d.apikey;
                s.origin     = model + "/" + d.id;
                out.push_back(std::move(s));
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[FlashForge] could not read the print-host settings: " << e.what();
    }
    return out;
}

void DeviceListPanel::syncSettingsPrinters()
{
    DeviceObjectOpr* opr = wxGetApp().getDeviceObjectOpr();
    if (opr == nullptr)
        return;
    const std::vector<FFPrinterEntry> entries = ff_merge_printer_sources(collect_settings_printers());
    opr->sync_settings_printers(entries);

    std::set<std::string> saved_serials;
    if (AppConfig* config = wxGetApp().app_config)
        for (const auto& row : config->get_local_machines())
            if (row.second.is_flashforge())
                saved_serials.insert(row.first);

    bool changed = false;
    // Tiles the settings no longer ask for. (A printer saved by Add printer keeps its tile.)
    for (const std::string& key : ff_stale_setting_tiles(m_settings_tiles, entries, saved_serials)) {
        auto it = m_device_map.find(key);
        if (it != m_device_map.end()) {
            it->second->blockMouseEvent(true);
            it->second->Disable();
            it->second->Destroy();
            m_device_map.erase(it);
            changed = true;
        }
    }
    m_settings_tiles.clear();

    for (const FFPrinterEntry& e : entries) {
        m_settings_tiles.insert(e.key);
        // A printer the user also saved with Add printer is theirs and works with what it saved:
        // the settings entry being incomplete does not make it "needs setup", and it keeps Unbind.
        const bool owned       = saved_serials.count(e.key) == 0;
        const bool needs_setup = owned && e.state != FFPrinterState::Ready;
        auto       it          = m_device_map.find(e.key);
        if (it == m_device_map.end()) {
            DeviceInfoItemPanel::DeviceInfo info;
            info.lanFlag       = true;
            info.status        = "offline";
            info.name          = e.name;
            info.from_settings = owned;
            info.needs_setup   = needs_setup;
            info.setup_text    = e.missing_text();
            DeviceInfoItemPanel* item = new DeviceInfoItemPanel(m_device_panel, info, this);
            item->setDevId(e.key);
            item->setSetupHandler([this]() { openPrinterSettings(); });
            item->Show(false);
            m_device_map.emplace(DeviceKey(generateNewPriorityId(), e.key, e.name), item);
            changed = true;
        } else {
            DeviceInfoItemPanel::DeviceInfo info = it->second->deviceInfo();
            if (info.from_settings != owned || info.needs_setup != needs_setup || info.setup_text != e.missing_text()) {
                info.from_settings = owned;
                info.needs_setup   = needs_setup;
                info.setup_text    = e.missing_text();
                if (needs_setup) {
                    info.conn_id = ComInvalidId;
                    info.status  = "offline";
                }
                it->second->setSetupHandler([this]() { openPrinterSettings(); });
                it->second->updateInfo(info);
                changed = true;
            }
        }
    }
    if (!changed)
        return;
    m_simple_book->ChangeSelection(m_device_map.empty() ? 0 : 1);
    if (!m_device_map.empty()) {
        updateFilterMap();
        updateStaticMap();
        updateDeviceSizer();
    }
}

void DeviceListPanel::openPrinterSettings()
{
    // The device list of the selected printer's model - where a FlashForge printer's address, serial
    // number and check code are entered. Closing it re-reads them, so the tile follows the edit.
    show_print_host_devices_dialog(this);
    CallAfter([this]() { syncSettingsPrinters(); });
}

void DeviceListPanel::onAddPrinter(wxCommandEvent &event)
{
    FFAddPrinterDialog dlg(this);
    dlg.ShowModal();
    // A new printer's tile arrives with the connection (COM_CONNECTION_READY -> device list update),
    // not from here.
    event.Skip();
}

} // GUI
} // Slic3r
