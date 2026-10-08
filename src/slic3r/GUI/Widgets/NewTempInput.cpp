#include "NewTempInput.hpp"
#include "Label.hpp"
#include "PopupWindow.hpp"
#include "../I18N.hpp"
#include <wx/dcgraph.h>
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "slic3r/GUI/FFUtils.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_CANCEL_PRINT_CLICKED, wxCommandEvent);
wxDEFINE_EVENT(EVT_CONTINUE_PRINT_CLICKED, wxCommandEvent);
wxDEFINE_EVENT(EVT_HIDE_PANEL, wxCommandEvent);

const std::string CLOSE = "close";
const std::string OPEN  = "open";

CancelPrint::CancelPrint(const wxString &info, const wxString &leftBtnTxt, const wxString &rightBtnTxt, const wxString &title)
    : TitleDialog(static_cast<wxWindow *>(Slic3r::GUI::wxGetApp().GetMainTopWindow()), title, 6)
{
    m_sizer_main = MainSizer();
    m_sizer_main->SetMinSize(wxSize(FromDIP(370), FromDIP(154)));

    m_sizer_main->AddSpacer(FromDIP(31));
    m_info = new wxStaticText(this, wxID_ANY, info);
    m_sizer_main->Add(m_info, 0, wxALIGN_CENTER, 0);

    m_sizer_main->AddSpacer(FromDIP(18));

    // 确认、取消按钮
    wxBoxSizer *bSizer_operate_hor = new wxBoxSizer(wxHORIZONTAL);
    wxPanel    *operate_panel      = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_cancel_btn                   = new FFButton(operate_panel, wxID_ANY, leftBtnTxt);
    m_cancel_btn->SetMinSize(wxSize(FromDIP(76), FromDIP(34)));
    m_cancel_btn->SetFontHoverColor(wxColour(255, 255, 255));
    m_cancel_btn->SetBGHoverColor(wxColour("#65A79E"));
    m_cancel_btn->SetBorderHoverColor(wxColour("#65A79E"));

    m_cancel_btn->SetFontPressColor(wxColour(255, 255, 255));
    m_cancel_btn->SetBGPressColor(wxColour("#1A8676"));
    m_cancel_btn->SetBorderPressColor(wxColour("#1A8676"));

    m_cancel_btn->SetFontColor(wxColour(255, 255, 255));
    m_cancel_btn->SetBorderColor(wxColour("#419488"));
    m_cancel_btn->SetBGColor(wxColour("#419488"));
    m_cancel_btn->Bind(wxEVT_LEFT_DOWN, [this, operate_panel](wxMouseEvent &event) {
        event.Skip();
        wxCommandEvent ev(EVT_CANCEL_PRINT_CLICKED, GetId());
         ev.SetEventObject(this);
         wxPostEvent(this, ev);
    });

    bSizer_operate_hor->AddStretchSpacer();
    bSizer_operate_hor->Add(m_cancel_btn, 0, wxALIGN_CENTER, 0);
    bSizer_operate_hor->AddSpacer(FromDIP(43));

    m_confirm_btn = new FFButton(operate_panel, wxID_ANY, rightBtnTxt);
    m_confirm_btn->SetMinSize(wxSize(FromDIP(76), FromDIP(34)));
    m_confirm_btn->SetFontHoverColor(wxColour("#65A79E"));
    m_confirm_btn->SetBGHoverColor(wxColour(255, 255, 255));
    m_confirm_btn->SetBorderHoverColor(wxColour("#65A79E"));

    m_confirm_btn->SetFontPressColor(wxColour("#1A8676"));
    m_confirm_btn->SetBGPressColor(wxColour(255, 255, 255));
    m_confirm_btn->SetBorderPressColor(wxColour("#1A8676"));

    m_confirm_btn->SetFontColor(wxColour("#333333"));
    m_confirm_btn->SetBorderColor(wxColour("#333333"));
    m_confirm_btn->SetBGColor(wxColour(255, 255, 255));
    m_confirm_btn->Bind(wxEVT_LEFT_DOWN, [this, operate_panel](wxMouseEvent &event) {
        event.Skip();
        wxCommandEvent ev(EVT_CONTINUE_PRINT_CLICKED, GetId());
        ev.SetEventObject(this);
        wxPostEvent(this, ev);
    });

    bSizer_operate_hor->Add(m_confirm_btn, 0, wxALIGN_CENTER, 0);
    bSizer_operate_hor->AddStretchSpacer();

    operate_panel->SetSizer(bSizer_operate_hor);
    operate_panel->Layout();
    bSizer_operate_hor->Fit(operate_panel);

    m_sizer_main->Add(operate_panel, 0, wxALL | wxALIGN_CENTER, 0);

    Fit();
    //Thaw();
    Centre(wxBOTH);
    Layout();
}

ShowTip::ShowTip(const wxString &info)
    : TitleDialog(static_cast<wxWindow *>(Slic3r::GUI::wxGetApp().GetMainTopWindow()), _L("Tip"), 6)
{
    m_sizer_main = MainSizer();
    m_sizer_main->SetMinSize(wxSize(FromDIP(360), FromDIP(160)));

    m_sizer_main->AddSpacer(FromDIP(50));
    m_info = new wxStaticText(this, wxID_ANY, info, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_info->SetForegroundColour(wxColor("#419488"));
    m_sizer_main->Add(m_info, 0, wxALIGN_CENTER);
    m_sizer_main->AddStretchSpacer();

    Fit();
    //Thaw();
    Centre(wxBOTH);
    Layout();
}

void ShowTip::SetLabel(const wxString &info) 
{ 
    m_info->SetLabel(info);
}

BEGIN_EVENT_TABLE(NewTempInput, wxPanel)
EVT_ENTER_WINDOW(NewTempInput::mouseEnterWindow)
EVT_LEAVE_WINDOW(NewTempInput::mouseLeaveWindow)
EVT_PAINT(NewTempInput::paintEvent)
END_EVENT_TABLE()

NewTempInput::NewTempInput()
    : label_color(std::make_pair(wxColour(0xAC, 0xAC, 0xAC), (int)StateColor::Disabled), std::make_pair(0x323A3C, (int)StateColor::Normal))
    , text_color(std::make_pair(wxColour(0xAC, 0xAC, 0xAC), (int)StateColor::Disabled), std::make_pair(0x6B6B6B, (int)StateColor::Normal))
{
    hover = false;
    radius = 0;
    border_color = StateColor(std::make_pair(*wxWHITE, (int)StateColor::Disabled), std::make_pair(0x009688, (int)StateColor::Focused), std::make_pair(0x009688, (int)StateColor::Hovered),
        std::make_pair(*wxWHITE, (int)StateColor::Normal));
    background_color = StateColor(std::make_pair(*wxWHITE, (int)StateColor::Disabled), std::make_pair(*wxWHITE, (int)StateColor::Normal));
    SetFont(Label::Body_12);
}

NewTempInput::NewTempInput(wxWindow* parent, wxString normal_icon, const wxPoint& pos, const wxSize& size, long style)
    : NewTempInput()
{
    Create(parent, "--", "--", normal_icon, pos, size, style);
}

void NewTempInput::Create(wxWindow* parent, wxString text, wxString label, wxString normal_icon, const wxPoint& pos, const wxSize& size, long style)
{
    StaticBox::Create(parent, wxID_ANY, pos, size, style);
    wxWindow::SetLabel(label);
    style &= ~wxALIGN_CENTER_HORIZONTAL;
    wxClientDC dc(this);
    dc.SetFont(Label::sysFont(19, false));
    auto text_size = dc.GetTextExtent("000");
    text_ctrl = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxSize(text_size),
        wxTE_PROCESS_ENTER | wxBORDER_NONE | wxALIGN_RIGHT,
        wxTextValidator(wxFILTER_NUMERIC), wxTextCtrlNameStr);
    text_ctrl->SetBackgroundColour(wxColor("#F8F8F8"));
    text_ctrl->SetMaxLength(3);
    text_ctrl->SetFont(Label::sysFont(19, false));
    text_ctrl->SetMaxSize(text_size);
    text_ctrl->SetMinSize(text_size);
    text_ctrl->SetMargins(0, 0);
    text_ctrl->SetForegroundColour(wxColor("#969696"));
    state_handler.attach_child(text_ctrl);
    text_ctrl->Bind(wxEVT_SET_FOCUS, [this](auto& e) {
        ProcessEventLocally(e);
        e.Skip();
        if (m_read_only) return;
        // enter input mode
        auto temp = text_ctrl->GetValue();
        if (temp.length() > 0 && temp[0] == (0x5f)) {
            text_ctrl->SetValue(wxEmptyString);
        }
        if (wdialog != nullptr) { wdialog->Dismiss(); }
        });
    text_ctrl->Bind(wxEVT_ENTER_WINDOW, [this](auto& e) {
        if (m_read_only) { 
            SetCursor(wxCURSOR_ARROW); 
        }
    });
    text_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto& e) {
        ProcessEventLocally(e);
        e.Skip();
        OnEdit();
        auto temp = text_ctrl->GetValue();
        lostFocusmodifyTemp();
        SetFinish();
        });
    text_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e) {
        e.Skip();
        if (m_read_only) {
            return;
        }
        OnEdit();
        auto temp = text_ctrl->GetValue();
        if (temp.ToStdString().empty()) return;
        lostFocusmodifyTemp();
        SetFinish();
        Slic3r::GUI::wxGetApp().GetMainTopWindow()->SetFocus();
    });
    text_ctrl->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (m_read_only) {
            return;
        }
        else {
            e.Skip();
        }
    });
    text_ctrl->Bind(wxEVT_RIGHT_DOWN, [this](auto& e) {}); // disable context menu
    if (!normal_icon.IsEmpty()) { this->normal_icon = ScalableBitmap(this, normal_icon.ToStdString(), 24); }
}


bool NewTempInput::AllisNum(std::string str)
{
    for (int i = 0; i < str.size(); i++) {
        int tmp = (int)str[i];
        if (tmp >= 48 && tmp <= 57) {
            continue;
        }
        else {
            return false;
        }
    }
    return true;
}

void NewTempInput::SetFinish()
{/*
    wxCommandEvent event(wxCUSTOMEVT_SET_TEMP_FINISH);
    event.SetInt(temp_type);
    wxPostEvent(this->GetParent(), event);*/
}

void NewTempInput::SetTagTemp(int temp, bool notifyModify)
{
    if (target_temp == temp || m_read_only) {
        return;
    }

    target_temp = temp;
    if (target_temp == INT_MAX) {
        curr_temp = INT_MAX;
        text_ctrl->SetValue("--");
        SetLabel("--");
        return;
    }

    target_temp = std::min(target_temp, max_temp);
    target_temp = std::max(target_temp, min_temp);

    if (notifyModify) {
        Freeze();
        text_ctrl->Freeze();
        text_ctrl->SetValue(wxString::Format("%d", target_temp));
        text_ctrl->Thaw();
        Thaw();
        Refresh();
    }
    else {
        text_ctrl->SetValue(wxString::Format("%d", target_temp));
        Refresh();
    }

}

void NewTempInput::SetCurrTemp(int temp, bool notifyModify)
{
    if (curr_temp == temp) {
        return;
    }
    curr_temp = temp;
    if (curr_temp == INT_MAX) {
        target_temp = INT_MAX;
        text_ctrl->SetValue("--");
        SetLabel("--");
        return;
    }
    
    if (notifyModify) {
        Freeze();
        SetLabel(wxString::Format("%d", curr_temp));
        Thaw();
    }
    else {
        SetLabel(wxString::Format("%d", curr_temp));
    }
}

void NewTempInput::Warning(bool warn, WarningType type)
{
    warning_mode = warn;
    //Refresh();

    if (warning_mode) {
        if (wdialog == nullptr) {
            wdialog = new PopupWindow(this);
            wdialog->SetBackgroundColour(wxColour(0xFFFFFF));

            wdialog->SetSizeHints(wxDefaultSize, wxDefaultSize);

            wxBoxSizer* sizer_body = new wxBoxSizer(wxVERTICAL);

            auto body = new wxPanel(wdialog, wxID_ANY, wxDefaultPosition, { FromDIP(260), -1 }, wxTAB_TRAVERSAL);
            body->SetBackgroundColour(wxColour(0xFFFFFF));


            wxBoxSizer* sizer_text;
            sizer_text = new wxBoxSizer(wxHORIZONTAL);



            warning_text = new wxStaticText(body, wxID_ANY,
                wxEmptyString,
                wxDefaultPosition, wxDefaultSize,
                wxALIGN_CENTER_HORIZONTAL);
            warning_text->SetFont(::Label::Body_12);
            warning_text->SetForegroundColour(wxColour(255, 111, 0));
            warning_text->Wrap(-1);
            sizer_text->Add(warning_text, 1, wxEXPAND | wxTOP | wxBOTTOM, 2);

            body->SetSizer(sizer_text);
            body->Layout();
            sizer_body->Add(body, 0, wxEXPAND, 0);

            wdialog->SetSizer(sizer_body);
            wdialog->Layout();
            sizer_body->Fit(wdialog);
        }

        wxPoint pos = this->ClientToScreen(wxPoint(2, 0));
        pos.y += this->GetRect().height - (this->GetSize().y - this->text_ctrl->GetSize().y) / 2 - 2;
        wdialog->SetPosition(pos);

        wxString warning_string;
        if (type == WarningType::WARNING_TOO_HIGH)
            warning_string = _L("The maximum temperature cannot exceed" + wxString::Format("%d", max_temp));
        else if (type == WarningType::WARNING_TOO_LOW)
            warning_string = _L("The minmum temperature should not be less than " + wxString::Format("%d", max_temp));

        warning_text->SetLabel(warning_string);
        wdialog->Popup();
    }
    else {
        if (wdialog)
            wdialog->Dismiss();
    }
}

void NewTempInput::SetNozzleIndex(int index)
{
    m_nozzle_index = index;
    Refresh();
}

void NewTempInput::SetMaxTemp(int temp) { max_temp = temp; }

void NewTempInput::SetMinTemp(int temp) { min_temp = temp; }

void NewTempInput::SetNormalIcon(wxString normalIcon)
{
    this->normal_icon = ScalableBitmap(this, normalIcon.ToStdString(), 16);
}

void NewTempInput::EnableTargetTemp(bool visible)
{
    if (visible) {
        m_target_temp_enable = true;
        text_ctrl->Show();
    }
    else {
        m_target_temp_enable = false;
        text_ctrl->Hide();
    }
    Refresh();
}

int NewTempInput::GetTagTemp() 
{ 
    int curr_target_temp;
    text_ctrl->GetValue().ToLong((long*)&curr_target_temp);
    curr_target_temp = std::min(curr_target_temp, max_temp);
    curr_target_temp = std::max(curr_target_temp, min_temp);
    return curr_target_temp;
}

void NewTempInput::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    Refresh();
}

void NewTempInput::SetTextColor(StateColor const& color)
{
    text_color = color;
    state_handler.update_binds();
}

void NewTempInput::SetLabelColor(StateColor const& color)
{
    label_color = color;
    state_handler.update_binds();
}

void NewTempInput::Rescale()
{
    if (this->normal_icon.bmp().IsOk()) this->normal_icon.msw_rescale();
}

bool NewTempInput::Enable(bool enable)
{
    bool result = wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
    }
    return result;
}

void NewTempInput::SetMinSize(const wxSize& size)
{
    wxSize size2 = size;
    if (size2.y < 0) {
#ifdef __WXMAC__
        if (GetPeer()) // peer is not ready in Create on mac
#endif
            size2.y = GetSize().y;
    }
    wxWindow::SetMinSize(size2);
}

void NewTempInput::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags & wxSIZE_USE_EXISTING) return;

    auto       left = padding_left;
    wxClientDC dc(this);
    if (normal_icon.bmp().IsOk()) {
        wxSize szIcon = normal_icon.GetBmpSize();
        left += szIcon.x;
    }

    // interval
    left += 9;

    // label
    dc.SetFont(::Label::Head_14);
    labelSize = dc.GetMultiLineTextExtent(wxWindow::GetLabel());
    left += labelSize.x;

    // interval
    left += 10;

    // separator
    dc.SetFont(::Label::Body_12);
    auto sepSize = dc.GetMultiLineTextExtent(wxString("/"));
    left += sepSize.x;

    // text text
    auto textSize = text_ctrl->GetTextExtent(wxString("0000"));
    text_ctrl->SetSize(textSize);
    text_ctrl->SetPosition({ left, (GetSize().y - text_ctrl->GetSize().y) / 2 });
}

void NewTempInput::DoSetToolTipText(wxString const& tip)
{
    wxWindow::DoSetToolTipText(tip);
    text_ctrl->SetToolTip(tip);
}

void NewTempInput::lostFocusmodifyTemp()
{
    double temp;
    bool   b = text_ctrl->GetValue().ToDouble(&temp);
    if (!b) {
        return;
    }
    temp = std::fmax(temp, min_temp);
    temp = std::fmin(temp, max_temp);
    auto str = wxString::Format("%d", (int)temp);
    text_ctrl->SetValue(str);
}

void NewTempInput::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void NewTempInput::render(wxDC& dc)
{
    StaticBox::render(dc);
    wxSize size = GetClientSize();

    if (warning_mode) {
        border_color = wxColour(255, 111, 0);
    }
    else {
        border_color = StateColor(std::make_pair(*wxWHITE, (int)StateColor::Disabled), std::make_pair(0x009688, (int)StateColor::Focused),
            std::make_pair(0x009688, (int)StateColor::Hovered), std::make_pair(*wxWHITE, (int)StateColor::Normal));
    }

    dc.SetBrush(wxColor("#F8F8F8"));
    dc.DrawRectangle(this->GetClientRect());
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // start draw
    wxSize content_size = { 0, 0 };
    const int      icon_sper = FromDIP(9);
    const wxString temp_char = wxString::FromUTF8(u8"℃");
    ScalableBitmap sbmp;
    wxString       numStr = wxString::Format("T%d", m_nozzle_index);
    dc.SetFont(Label::sysFont(18, true));
    auto     num_size = dc.GetTextExtent(numStr);
    if (m_nozzle_index != -1) {
        content_size.x += num_size.x + icon_sper;
    }
    else {
        if (normal_icon.bmp().IsOk()) {
            sbmp = normal_icon;
        }
        if (sbmp.bmp().IsOk()) {
            content_size.x += sbmp.GetBmpWidth() + icon_sper;
        }
    }


    auto text = wxWindow::GetLabel();
    dc.SetFont(Label::sysFont(26, false));
    labelSize = dc.GetTextExtent("000" + temp_char);
    content_size.x += labelSize.x;
    wxSize sepSize;
    if (m_target_temp_enable)
    {
        dc.SetFont(Label::sysFont(19, false));
        sepSize = dc.GetTextExtent(wxString("/") + temp_char);
        content_size.x += sepSize.x + FromDIP(4);
        auto text_size0 = dc.GetTextExtent("000");
#ifdef __APPLE__
        text_size0.x += FromDIP(6);
#endif // 

        this->text_ctrl->SetClientSize(text_size0);
        content_size.x += this->text_ctrl->GetClientSize().x;
    }

    int pt = (size.x - content_size.x) / 2;
    if (m_nozzle_index != -1) {
        dc.SetTextForeground(wxColour("#999999"));
        dc.SetFont(Label::sysFont(18, true));
        dc.DrawText(numStr, pt, (size.y - num_size.y) / 2);
        pt += icon_sper + num_size.x;
    }
    else {
        if (sbmp.bmp().IsOk()) {
            dc.DrawBitmap(sbmp.bmp(), pt, (size.y - sbmp.GetBmpHeight()) / 2);
            pt += sbmp.GetBmpWidth() + icon_sper;
        }
    }
    dc.SetFont(Label::sysFont(26, false));
    dc.SetTextForeground(StateColor::darkModeColorFor("#328DFB"));
    if (text.compare("--") == 0) {
        dc.SetTextForeground(StateColor::darkModeColorFor("#969696"));
    }
    auto textSize = dc.GetTextExtent(text + temp_char);
    dc.DrawText(text + temp_char, pt + labelSize.x - textSize.x, (size.y - labelSize.y) / 2);
    if (m_target_temp_enable) {
        pt += labelSize.x + FromDIP(2);
        dc.SetTextForeground(wxColor("#969696"));
        dc.SetFont(Label::sysFont(19, false));
        auto sepSize0 = dc.GetTextExtent(wxString("/"));
        dc.DrawText(wxString("/"), pt, (size.y - labelSize.y) / 2 + labelSize.y - sepSize.y);
        pt += sepSize0.x + FromDIP(2);
        this->text_ctrl->SetPosition(wxPoint(pt, (size.y - labelSize.y) / 2 + labelSize.y - sepSize.y));
        pt += text_ctrl->GetClientSize().x;
        dc.DrawText(temp_char, pt, (size.y - labelSize.y) / 2 + labelSize.y - sepSize.y);
    }
}

void NewTempInput::mouseEnterWindow(wxMouseEvent& event)
{
    if (!hover) {
        hover = true;
        Refresh();
    }
}

void NewTempInput::mouseLeaveWindow(wxMouseEvent& event)
{
    if (hover) {
        hover = false;
        Refresh();
    }
}

IconText::IconText() {}

IconText::IconText(wxWindow* parent,wxString icon,int iconSize,wxString text,int textSize,const wxPoint &pos,const wxSize & size,long style)
                //: wxPanel(parent, wxID_ANY,pos, size, style)
{
    Create(parent, wxID_ANY, pos, size);
    SetBackgroundColour(*wxWHITE);
    create_panel(this, icon, iconSize, text, textSize);
}

void IconText::create_panel(wxWindow* parent,wxString icon,int iconSize,wxString text,int textSize)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    auto m_panel_page = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_icon = create_scaled_bitmap(icon.ToStdString(), parent, iconSize);
    auto icon_static = new wxStaticBitmap(m_panel_page, wxID_ANY, m_icon);
    icon_static->SetBackgroundColour(*wxWHITE);

    wxSize size = icon_static->GetSize();
    icon_static->SetSize(size.GetWidth(), size.GetHeight());

    m_text_ctrl = new Label(m_panel_page, text);
    //m_text_ctrl->Wrap(-1);
    //m_text_ctrl->SetFont(wxFont(wxFontInfo(textSize)));
    m_text_ctrl->SetForegroundColour(wxColour(50, 141, 251));
    m_text_ctrl->SetBackgroundColour(*wxWHITE);
    //m_text_ctrl->SetMinSize(wxSize(FromDIP(70), -1));

    sizer->AddStretchSpacer();
    sizer->Add(icon_static, 0, wxALIGN_CENTER_HORIZONTAL | wxALL | wxEXPAND, 0);
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(m_text_ctrl, 0, wxALIGN_CENTER_HORIZONTAL | wxALL | wxEXPAND, 0);
    sizer->AddStretchSpacer();

    m_panel_page->SetSizer(sizer);
    m_panel_page->Layout();
    sizer->Fit(m_panel_page); 
}

void IconText::setText(wxString text)
{
    m_text_ctrl->SetLabel(text);
    Refresh();
}

void IconText::setTextForegroundColour(wxColour colour) 
{
    m_text_ctrl->SetForegroundColour(colour);
    Refresh();
}

void IconText::setTextBackgroundColor(wxColour colour)
{
    m_text_ctrl->SetBackgroundColour(colour);
    Refresh();
}

IconBottonText::IconBottonText(wxWindow* parent,wxString icon,int iconSize,wxString text,int textSize,wxString secondIcon,wxString thirdIcon,bool positiveOrder,const wxPoint &pos,const wxSize & size,long style)
                : wxPanel(parent, wxID_ANY,pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    create_panel(this, icon, iconSize, text, textSize, secondIcon, thirdIcon, positiveOrder);
}

void IconBottonText::create_panel(
    wxWindow *parent, wxString icon, int iconSize, wxString text, int textSize, wxString secondIcon, wxString thirdIcon, bool positiveOrder)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    auto m_panel_page = new wxPanel(this, wxID_ANY, wxDefaultPosition,wxDefaultSize,wxBORDER_NONE);
    m_panel_page->SetSize(wxSize(-1,-1));
    m_icon = create_scaled_bitmap(icon.ToStdString(), parent, iconSize);

    m_text_ctrl = new wxTextCtrl(m_panel_page, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    //m_text_ctrl->SetFont(wxFont(wxFontInfo(textSize)));
    m_text_ctrl->SetForegroundColour(wxColour(51, 51, 51));
    m_text_ctrl->SetBackgroundColour(wxColour(255, 255, 255));
    m_text_ctrl->SetMinSize(wxSize(FromDIP(50), -1));
    //m_text_ctrl->Bind(wxEVT_TEXT, &IconBottonText::onTextChange, this);
    m_text_ctrl->Bind(wxEVT_KILL_FOCUS, &IconBottonText::onTextFocusOut, this);

    m_unitLabel = new wxStaticText(m_panel_page, wxID_ANY, "%");

    auto icon_static = new wxStaticBitmap(m_panel_page, wxID_ANY, m_icon);

    if (secondIcon.IsEmpty()) {
        m_dec_btn = new FFPushButton(m_panel_page, wxID_ANY, "push_button_dec_normal", "push_button_dec_hover", "push_button_dec_press","push_button_dec_disable",21);
    } else {
        m_dec_btn = new FFPushButton(m_panel_page, wxID_ANY, "push_button_arrow_inc_normal", "push_button_arrow_inc_hover",
                                     "push_button_arrow_inc_press", "push_button_arrow_inc_disable",21);
    }
    m_dec_btn->Bind(wxEVT_LEFT_UP, &IconBottonText::onDecBtnClicked, this);
    m_dec_btn->SetBackgroundColour(*wxWHITE);

    if (secondIcon.IsEmpty()) {
        m_inc_btn = new FFPushButton(m_panel_page, wxID_ANY, "push_button_inc_normal", "push_button_inc_hover", "push_button_inc_press","push_button_inc_disable",21);
    } else {
        m_inc_btn = new FFPushButton(m_panel_page, wxID_ANY, "push_button_arrow_dec_normal", "push_button_arrow_dec_hover",
                                     "push_button_arrow_dec_press", "push_button_arrow_dec_disable",21);
            
    }
    m_inc_btn->Bind(wxEVT_LEFT_UP, &IconBottonText::onIncBtnClicked, this);
    m_inc_btn->SetBackgroundColour(*wxWHITE);

    if (positiveOrder) {
        sizer->Add(icon_static, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);
        sizer->AddSpacer(FromDIP(12));
        sizer->Add(m_dec_btn, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);
        sizer->AddSpacer(FromDIP(5));
        sizer->Add(m_text_ctrl, 0, wxALIGN_CENTER_HORIZONTAL, 0);
        sizer->Add(m_unitLabel, 0, wxALIGN_CENTER_HORIZONTAL, 0);
        sizer->AddSpacer(FromDIP(5));
        sizer->Add(m_inc_btn, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);
    } else {
        sizer->Add(icon_static, 0, wxALIGN_CENTER | wxALL | wxEXPAND, 0);
        sizer->AddSpacer(FromDIP(12));
        sizer->Add(m_inc_btn, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);
        sizer->AddSpacer(FromDIP(5));
        sizer->Add(m_text_ctrl, 0, wxALIGN_CENTER, 0);
        sizer->Add(m_unitLabel, 0, wxALIGN_CENTER, 0);
        sizer->AddSpacer(FromDIP(5));
        sizer->Add(m_dec_btn, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);
    }


    m_panel_page->SetSizer(sizer);
    m_panel_page->Layout();
    sizer->Fit(m_panel_page); 
    if (!secondIcon.IsEmpty()) {
        m_unitLabel->Hide();
    }
}

void IconBottonText::setLimit(double min, double max)
{ 
    m_min = min;
    m_max = max;
}

void IconBottonText::setAdjustValue(double value) 
{
    m_adjust_value = value; 
}

wxString IconBottonText::getTextValue() 
{ 
    return m_text_ctrl->GetValue(); 
}

void IconBottonText::setText(wxString text) 
{
    m_text_ctrl->SetValue(text);
    Refresh();
}

void IconBottonText::setCurValue(double value) 
{
    m_cur_value = value;
}

void IconBottonText::setPoint(int value) 
{
    m_point = value;
}

void IconBottonText::onTextChange(wxCommandEvent &event) 
{
    long value;
    if (m_text_ctrl->GetValue().ToLong(&value)) {
        if (value < m_min) {
            wxString str_min = wxString::Format("%.2f", m_min);
            m_text_ctrl->ChangeValue(str_min);
        } else if (value > m_max) {
            wxString str_max = wxString::Format("%.2f", m_max);
            m_text_ctrl->ChangeValue(str_max);
        }
    }
}

void IconBottonText::checkValue() 
{
    wxString text = m_text_ctrl->GetValue();
    long     value;
    double   doubleValue;
    if (text.ToLong(&value)) {
        if (value < m_min) {
            wxString str_min = wxString::Format("%.0f", m_min);
            m_text_ctrl->ChangeValue(str_min);
        } else if (value > m_max) {
            wxString str_max = wxString::Format("%.0f", m_max);
            m_text_ctrl->ChangeValue(str_max);
        }
    } else if (text.ToDouble(&doubleValue)) {
        if (doubleValue < m_min) {
            wxString str_min = wxString::Format("%.3f", m_min);
            m_text_ctrl->ChangeValue(str_min);
        } else if (doubleValue > m_max) {
            wxString str_max = wxString::Format("%.3f", m_max);
            m_text_ctrl->ChangeValue(str_max);
        }
    } else {
        // 输入不合法
        if (m_point == 3) {
            wxString str_last = wxString::Format("%.3f", m_cur_value);
            m_text_ctrl->ChangeValue(str_last);
        } else {
            wxString str_last = wxString::Format("%.0f", m_cur_value);
            m_text_ctrl->ChangeValue(str_last);
        }
    }
}

void IconBottonText::onTextFocusOut(wxFocusEvent &event) 
{
    checkValue();
    event.Skip();
}

void IconBottonText::onDecBtnClicked(wxMouseEvent &event) 
{
    wxString text = m_text_ctrl->GetValue();
    long     value;
    double   doubleValue;
    if (text.ToLong(&value)) {
        double cur_value = (value - m_adjust_value) < m_min ? m_min : value - m_adjust_value;
        wxString str_cur   = wxString::Format("%.0f", cur_value);
        m_text_ctrl->ChangeValue(str_cur);
    } else if (text.ToDouble(&doubleValue)) {
        double cur_value = (doubleValue - m_adjust_value) < m_min ? m_min : doubleValue - m_adjust_value;
        wxString str_cur   = wxString::Format("%.3f", cur_value);
        m_text_ctrl->ChangeValue(str_cur);
    }
    event.Skip(); 
}

void IconBottonText::onIncBtnClicked(wxMouseEvent &event)
{
    wxString text = m_text_ctrl->GetValue();
    long     value;
    double   doubleValue;
    if (text.ToLong(&value)) {
        double cur_value = (value + m_adjust_value) > m_max ? m_max : value + m_adjust_value;
        wxString str_cur   = wxString::Format("%.0f", cur_value);
        m_text_ctrl->ChangeValue(str_cur);
    } else if (text.ToDouble(&doubleValue)) {
        double   cur_value = (doubleValue + m_adjust_value) > m_max ? m_max : doubleValue + m_adjust_value;
        wxString str_cur   = wxString::Format("%.3f", cur_value);
        m_text_ctrl->ChangeValue(str_cur);
    }
    event.Skip();
}

StartFiltering::StartFiltering(wxWindow* parent)
    : wxPanel(parent, wxID_ANY,wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    SetBackgroundColour(*wxWHITE);
    create_panel(this);
}

void StartFiltering::setCurId(int curId)
{
    if (curId < 0) {
        return;
    }
    m_cur_id = curId;
}

void StartFiltering::create_panel(wxWindow* parent)
{
    parent->SetMinSize(wxSize(-1, FromDIP(277)));
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *bSizer_filtering_title = new wxBoxSizer(wxHORIZONTAL);

    auto m_panel_filtering_title = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(36)), wxTAB_TRAVERSAL);
    m_panel_filtering_title->SetBackgroundColour(wxColour(248,248,248));

    //过滤标题
    auto m_staticText_filtering = new wxStaticText(m_panel_filtering_title, wxID_ANY ,_L("Start Filtering"));
    m_staticText_filtering->Wrap(-1);
    //m_staticText_filtering->SetFont(wxFont(wxFontInfo(16)));
    m_staticText_filtering->SetForegroundColour(wxColour(51,51,51));

    bSizer_filtering_title->Add(m_staticText_filtering, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(17));
    bSizer_filtering_title->Add(0, 1, wxEXPAND, 0);
    m_panel_filtering_title->SetSizer(bSizer_filtering_title);
    m_panel_filtering_title->Layout();
    bSizer_filtering_title->Fit(m_panel_filtering_title);

    //内循环过滤
    wxBoxSizer *bSizer_internal_circulate_hor = new wxBoxSizer(wxHORIZONTAL);
    wxPanel*    internal_circulate_panel      = new wxPanel(parent, wxID_ANY, wxDefaultPosition,wxDefaultSize, wxTAB_TRAVERSAL);
    auto m_staticText_internal_circulate = new wxStaticText(internal_circulate_panel, wxID_ANY, _L("Internal Circulate"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_staticText_internal_circulate->Wrap(-1);
    //m_staticText_internal_circulate->SetFont(wxFont(wxFontInfo(16)));
    m_internal_circulate_switch = new SwitchButton(internal_circulate_panel);
    m_internal_circulate_switch->SetBackgroundColour(*wxWHITE);

    bSizer_internal_circulate_hor->AddSpacer(FromDIP(17));
    bSizer_internal_circulate_hor->Add(m_staticText_internal_circulate, 0, wxALL | wxEXPAND, 0);
    bSizer_internal_circulate_hor->AddSpacer(FromDIP(7));
    bSizer_internal_circulate_hor->Add(m_internal_circulate_switch, 0, wxALL | wxEXPAND, 0);
    m_internal_circulate_switch->Bind(wxEVT_TOGGLEBUTTON, &StartFiltering::onAirFilterToggled, this);

    internal_circulate_panel->SetSizer(bSizer_internal_circulate_hor);
    internal_circulate_panel->Layout();
    bSizer_internal_circulate_hor->Fit(internal_circulate_panel);

    //外循环过滤
    wxBoxSizer *bSizer_external_circulate_hor = new wxBoxSizer(wxHORIZONTAL);
    wxPanel*    external_circulate_panel      = new wxPanel(parent, wxID_ANY, wxDefaultPosition,wxDefaultSize, wxTAB_TRAVERSAL);
    auto m_staticText_external_circulate = new wxStaticText(external_circulate_panel, wxID_ANY, _L("External Circulate"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_staticText_external_circulate->Wrap(-1);
    //m_staticText_external_circulate->SetFont(wxFont(wxFontInfo(16)));
    m_external_circulate_switch = new SwitchButton(external_circulate_panel);
    m_external_circulate_switch->SetBackgroundColour(*wxWHITE);
    m_external_circulate_switch->Bind(wxEVT_TOGGLEBUTTON, &StartFiltering::onAirFilterToggled, this);

    bSizer_external_circulate_hor->AddSpacer(FromDIP(17));
    bSizer_external_circulate_hor->Add(m_staticText_external_circulate, 0, wxALL | wxEXPAND, 0);
    bSizer_external_circulate_hor->AddSpacer(FromDIP(7));
    bSizer_external_circulate_hor->Add(m_external_circulate_switch, 0, wxALL | wxEXPAND, 0);

    external_circulate_panel->SetSizer(bSizer_external_circulate_hor);
    external_circulate_panel->Layout();
    bSizer_external_circulate_hor->Fit(external_circulate_panel);

    sizer->Add(m_panel_filtering_title, 0, wxEXPAND | wxALL, 0);
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(internal_circulate_panel, 0, wxEXPAND | wxALIGN_CENTER, 0);
    sizer->AddSpacer(FromDIP(14));
    sizer->Add(external_circulate_panel, 0, wxEXPAND | wxALIGN_CENTER, 0);
    sizer->AddStretchSpacer();

    parent->SetSizer(sizer);
}

void StartFiltering::setBtnState(bool internalOpen, bool externalOpen) 
{ 
    m_internal_circulate_switch->SetValue(internalOpen);
    m_external_circulate_switch->SetValue(externalOpen);
}

void StartFiltering::onAirFilterToggled(wxCommandEvent &event) 
{
    event.Skip();
    SwitchButton *click_btn   = dynamic_cast<SwitchButton *>(event.GetEventObject());
    std::string   inter_state = CLOSE;
    std::string   exter_state = CLOSE;

    if (m_internal_circulate_switch) {
        inter_state = m_internal_circulate_switch->GetValue() ? OPEN : CLOSE;
    }

    if (m_external_circulate_switch) {
        exter_state = m_external_circulate_switch->GetValue() ? OPEN : CLOSE;
    }

    if (m_internal_circulate_switch->GetValue() && m_external_circulate_switch->GetValue()) {
        if (click_btn == m_internal_circulate_switch) {
            m_external_circulate_switch->SetValue(false);
            exter_state = CLOSE;
        } else if (click_btn == m_external_circulate_switch) {
            m_internal_circulate_switch->SetValue(false);
            inter_state = CLOSE;
        }
    }
    Slic3r::GUI::ComAirFilterCtrl *filterCtrl = new Slic3r::GUI::ComAirFilterCtrl(inter_state, exter_state);
    // 测试，临时将id写死
    if (m_cur_id >= 0) {
        Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, filterCtrl);
    }
}

TempMixDevice::TempMixDevice(wxWindow* parent,bool idle, wxString nozzleTemp, wxString platformTemp, wxString cavityTemp,const wxPoint &pos,const wxSize & size,long style)
             : wxPanel(parent, wxID_ANY,pos, size, style)
{
    //SetBackgroundColour(*wxWHITE);
    create_panel(this,idle,nozzleTemp,platformTemp,cavityTemp);
    connectEvent();
}

void TempMixDevice::setState(int state, bool lampState)
{ 
    if (0 == state) {   //offline
        //图标、解绑
        m_idle_device_info_button->SetIcon("device_file_offline");
        //m_idle_lamp_control_button->SetIcon("device_lamp_offline");
        m_idle_filter_button->SetIcon("device_filter_offline");
        m_idle_device_info_button->Unbind(wxEVT_LEFT_DOWN, &TempMixDevice::onDevInfoBtnClicked, this);
        //m_idle_lamp_control_button->Unbind(wxEVT_LEFT_DOWN, &TempMixDevice::onLampBtnClicked, this);
        m_idle_filter_button->Unbind(wxEVT_LEFT_DOWN, &TempMixDevice::onFilterBtnClicked, this);
        m_panel_idle_device_info->Hide();
        m_panel_circula_filter->Hide();
        m_idle_device_info_button->Enable(false);
        //m_idle_lamp_control_button->Enable(false);
        m_idle_filter_button->Enable(false);
        m_idle_device_info_button->SetBackgroundColor(wxColour(255, 255, 255));
        m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
        Layout();
    } else if(1 == state){  //idle
        m_idle_device_info_button->SetIcon("device_idle_file_info");
        /*if (lampState) {
            m_idle_lamp_control_button->SetIcon("device_lamp_control_press");
        } else {
            m_idle_lamp_control_button->SetIcon("device_lamp_control");
        }*/
        if (m_g3uMachine && !m_clearFanPressed) {
            m_idle_filter_button->SetIcon("device_filter_offline");
        } else {
            m_idle_filter_button->SetIcon("device_filter");
        }
        m_idle_device_info_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onDevInfoBtnClicked, this);
        //m_idle_lamp_control_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onLampBtnClicked, this);
        m_idle_filter_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onFilterBtnClicked, this);
        m_idle_device_info_button->Enable(true);
        //m_idle_lamp_control_button->Enable(true);
        m_idle_filter_button->Enable(true);
    } else if (2 == state) {   // normal
        m_idle_device_info_button->SetIcon("device_file_info");
        //m_idle_lamp_control_button->SetIcon("device_lamp_control");
        if (m_g3uMachine && !m_clearFanPressed) {
            m_idle_filter_button->SetIcon("device_filter_offline");
        } else {
            m_idle_filter_button->SetIcon("device_filter");
        }
        m_idle_device_info_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onDevInfoBtnClicked, this);
        //m_idle_lamp_control_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onLampBtnClicked, this);
        m_idle_filter_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onFilterBtnClicked, this);
        m_idle_device_info_button->Enable(true);
        //m_idle_lamp_control_button->Enable(true);
        m_idle_filter_button->Enable(true);
    }
}

void TempMixDevice::setCurId(int curId)
{
    if (curId < 0) {
        return;
    }
    m_cur_id = curId;
    m_panel_circula_filter->setCurId(curId);
    //m_pos_btn->SetCurId(curId);
    m_clearFanPressed = true;
    reInitPage();
}

void TempMixDevice::reInitProductState() 
{ 
    //m_idle_lamp_control_button->SetIcon("device_lamp_control");
    m_idle_filter_button->SetIcon("device_filter");
    //m_idle_lamp_control_button->Enable(true);
    m_idle_filter_button->Enable(true);
}

void TempMixDevice::reInitPage() 
{
    if (m_panel_idle_device_info) {
        m_panel_idle_device_info->Show();
    }
    if (m_panel_circula_filter) {
        m_panel_circula_filter->Hide();
    }
    if (m_idle_device_info_button) {
        m_idle_device_info_button->SetBackgroundColor(wxColour(255, 255, 255));
    }
    if (m_idle_filter_button) {
        m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
    }
}

void TempMixDevice::setDevProductAuthority(const fnet_dev_product_t &data) 
{
    bool fanCtrl   = data.internalFanCtrlState == 0 ? false : true;
    if (!m_g3uMachine  && !fanCtrl) {
        m_idle_filter_button->SetIcon("device_filter_offline");
        m_idle_filter_button->Enable(false);
        m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
        m_idle_filter_button->SetBorderColor(wxColour(255, 255, 255));
        if (m_panel_circula_filter) {
            m_panel_circula_filter->Hide();
        }
    }
}

void TempMixDevice::lostFocusmodifyTemp()
{
    //double top_temp;
    //double bottom_temp;
    //double mid_temp;
    //bool   bTop = m_top_btn->GetTagTemp().ToDouble(&top_temp);
    //bool   bBottom = m_bottom_btn->GetTagTemp().ToDouble(&bottom_temp);
    //bool   bMid    = m_mid_btn->GetTagTemp().ToDouble(&mid_temp);
    //if (!m_g3uMachine) {
    //    if (!bTop || top_temp < 0) {
    //        m_top_btn->SetTagTemp(m_right_target_temp, true);
    //        top_temp = m_right_target_temp;
    //    }
    //    if (top_temp > 280) {
    //        top_temp = 280;
    //        m_top_btn->SetTagTemp(top_temp, true);
    //        m_right_target_temp = top_temp;
    //    } else if (top_temp < 0) {
    //        top_temp = 0;
    //        m_top_btn->SetTagTemp(top_temp, true);
    //        m_right_target_temp = top_temp;
    //    }
    //    if (!bBottom || bottom_temp < 0) {
    //        m_bottom_btn->SetTagTemp(m_plat_target_temp, true);
    //        bottom_temp = m_plat_target_temp;
    //    }
    //    if (bottom_temp > 110) {
    //        bottom_temp = 110;
    //        m_bottom_btn->SetTagTemp(bottom_temp, true);
    //        m_plat_target_temp = bottom_temp;
    //    } else if (bottom_temp < 0) {
    //        bottom_temp = 0;
    //        m_bottom_btn->SetTagTemp(bottom_temp, true);
    //        m_plat_target_temp = bottom_temp;
    //    }
    //    //Slic3r::GUI::ComTempCtrl* tempCtrl = new Slic3r::GUI::ComTempCtrl(bottom_temp, top_temp, 0, mid_temp);
    //    //Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
    //} else {
    //    //right
    //    if (!bTop || top_temp < 0) {
    //        m_top_btn->SetTagTemp(m_right_target_temp, true);
    //        top_temp = m_right_target_temp;
    //    }
    //    if (top_temp > 350) {
    //        top_temp = 350;
    //        m_top_btn->SetTagTemp(top_temp, true);
    //        m_right_target_temp = top_temp;
    //    } else if (top_temp < 0) {
    //        top_temp = 0;
    //        m_top_btn->SetTagTemp(top_temp, true);
    //        m_right_target_temp = top_temp;
    //    }
    //    //left
    //    if (!bBottom || bottom_temp < 0) {
    //        m_bottom_btn->SetTagTemp(m_plat_target_temp, true);
    //        bottom_temp = m_plat_target_temp;
    //    }
    //    if (bottom_temp > 350) {
    //        bottom_temp = 350;
    //        m_bottom_btn->SetTagTemp(bottom_temp, true);
    //        m_plat_target_temp = bottom_temp;
    //    } else if (bottom_temp < 0) {
    //        bottom_temp = 0;
    //        m_bottom_btn->SetTagTemp(bottom_temp, true);
    //        m_plat_target_temp = bottom_temp;
    //    }
    //    //bottom
    //    if (!bMid || mid_temp < 0) {
    //        m_mid_btn->SetTagTemp(m_cavity_target_temp, true);
    //        mid_temp = m_cavity_target_temp;
    //    }
    //    if (mid_temp > 110) {
    //        mid_temp = 110;
    //        m_mid_btn->SetTagTemp(mid_temp, true);
    //        m_cavity_target_temp = mid_temp;
    //    } else if (mid_temp < 0) {
    //        mid_temp = 0;
    //        m_mid_btn->SetTagTemp(mid_temp, true);
    //        m_cavity_target_temp = mid_temp;
    //    }

    //    //Slic3r::GUI::ComTempCtrl* tempCtrl = new Slic3r::GUI::ComTempCtrl(mid_temp, top_temp, bottom_temp, 0);
    //    //Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
    //}
}

void TempMixDevice::changeMachineType(unsigned short pid)
{
    if (!FFUtils::isPrinterSupportDeviceFilter(pid)) {
        m_panel_idle_device_info->Hide();
        m_panel_idle_device_state->Hide();
        m_panel_idle_device_title->Show();
        m_panel_u_device->Show();
    } else {
        m_panel_idle_device_info->Hide();
        m_panel_idle_device_state->Show();
        m_panel_idle_device_title->Hide();
        m_panel_u_device->Hide();
    }
    switch (pid) {
    case ADVENTURER_5M: //"adventurer_5m"
    case ADVENTURER_5M_PRO: //"adventurer_5m_pro"
    case ADVENTURER_A5: // "adventurer_a5"
        m_g3uMachine = false;
        /*m_top_btn->SetNormalIcon("device_top_temperature");
        m_top_btn->SetIconNormal();
        m_bottom_btn->SetNormalIcon("device_bottom_temperature");
        m_bottom_btn->SetIconNormal();
        m_mid_btn->SetNormalIcon("device_mid_temperature");
        m_mid_btn->SetIconNormal();
        m_mid_btn->SetReadOnly(true);*/
        break;
    case GUIDER_3_ULTRA: //"guider_3_ultra"
        m_g3uMachine = true;
       /* m_top_btn->SetNormalIcon("device_right_temperature");
        m_top_btn->SetIconNormal();
        m_bottom_btn->SetNormalIcon("device_left_temperature");
        m_bottom_btn->SetIconNormal();
        m_mid_btn->SetNormalIcon("device_bottom_temperature");
        m_mid_btn->SetIconNormal();
        m_mid_btn->SetReadOnly(false);*/
        break;
    }
}

void TempMixDevice::setDisabledMoveCtrl(bool b) 
{ 
    /*auto color = b ? wxColor(221, 221, 221) : *wxBLACK;
    m_pos_title_text->SetForegroundColour(color); 
    m_pos_btn->Enable(!b);
    m_x_text->SetForegroundColour(color);
    m_y_text->SetForegroundColour(color);
    m_z_text->SetForegroundColour(color);
    m_zero_btn->Enable(!b);
    m_zero_btn->SetIcon(b ? "zero_btn_disabled" : "zero_btn_pressed");
    m_plate_up_btn->Enable(!b);
    m_plate_down_btn->Enable(!b);
    m_plate_up_btn->SetIcon(b ? "arrow_up_disabled" : "arrow_up_normal");
    m_plate_down_btn->SetIcon(b ? "arrow_down_disabled" : "arrow_down_normal");
    m_btn_step1->Enable(!b);
    m_btn_step50->Enable(!b);
    m_btn_step100->Enable(!b);*/
}

void TempMixDevice::setDisabledExtruderCtrl(bool b) 
{
    /*auto color = b ? wxColor(221, 221, 221) : *wxBLACK;
    m_extruder_title->SetForegroundColour(color);
    m_extruder_up_btn->Enable(!b);
    m_extruder_down_btn->Enable(!b);
    m_extruder_up_btn->SetIcon(b ? "arrow_up_disabled" : "arrow_up_normal");
    m_extruder_down_btn->SetIcon(b ? "arrow_down_disabled" : "arrow_down_normal");
    m_vSizer3->Show(!b);
    m_extruderLine->Show(!b);
    m_extruderSperator->Show(!b);
    m_extruderSperator1->Show(!b);
    m_plateSperator->Show(b);*/
}

void TempMixDevice::create_panel(wxWindow* parent, bool idle, wxString nozzleTemp, wxString platformTemp, wxString cavityTemp)
{
    // 新建垂直布局
    wxBoxSizer* idleSizer = new wxBoxSizer(wxVERTICAL);

    ////***温度控件 2
    //wxBoxSizer* bSizer_temperature  = new wxBoxSizer(wxVERTICAL);
    //auto        m_panel_temperature = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(491), FromDIP(286)));
    //m_panel_temperature->SetBackgroundColour(*wxWHITE);
    //wxWindowID top_id = wxWindow::NewControlId();
    //m_top_btn         = new TempInput(m_panel_temperature, top_id, wxString("--"), wxString("--"), wxString("device_top_temperature"),
    //                                  wxString("device_top_temperature"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    //m_top_btn->SetMinTemp(20);
    //m_top_btn->SetMaxTemp(120);
    //m_top_btn->SetMinSize((wxSize(-1, FromDIP(58))));
    //m_top_btn->SetBorderWidth(0);
    //StateColor tempinput_text_colour(std::make_pair(wxColour(51, 51, 51), (int) StateColor::Disabled),
    //                                 std::make_pair(wxColour(48, 58, 60), (int) StateColor::Normal));
    //m_top_btn->SetTextColor(tempinput_text_colour);
    //StateColor tempinput_border_colour(std::make_pair(*wxWHITE, (int) StateColor::Disabled),
    //                                   std::make_pair(wxColour(0, 150, 136), (int) StateColor::Focused),
    //                                   std::make_pair(wxColour(0, 150, 136), (int) StateColor::Hovered),
    //                                   std::make_pair(*wxWHITE, (int) StateColor::Normal));
    //m_top_btn->SetBorderColor(tempinput_border_colour);
    //m_top_btn->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});
    //m_top_btn->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});

    //bSizer_temperature->AddSpacer(FromDIP(16));
    //bSizer_temperature->Add(m_top_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(28));

    //wxWindowID bottom_id = wxWindow::NewControlId();
    //m_bottom_btn = new TempInput(m_panel_temperature, bottom_id, wxString("--"), wxString("--"), wxString("device_bottom_temperature"),
    //                             wxString("device_bottom_temperature"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    //m_bottom_btn->SetMinTemp(20);
    //m_bottom_btn->SetMaxTemp(120);
    //m_bottom_btn->SetMinSize((wxSize(-1, FromDIP(58))));
    //m_bottom_btn->SetBorderWidth(0);
    //m_bottom_btn->SetBorderColor(tempinput_border_colour);
    //m_bottom_btn->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});
    //m_bottom_btn->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});
    //bSizer_temperature->AddSpacer(FromDIP(16));
    //bSizer_temperature->Add(m_bottom_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(28));

    //wxWindowID bottom_mid = wxWindow::NewControlId();
    //m_mid_btn = new TempInput(m_panel_temperature, bottom_mid, wxString("--"), wxString("--"), wxString("device_mid_temperature"),
    //                          wxString("device_mid_temperature"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    //m_mid_btn->SetMinTemp(20);
    //m_mid_btn->SetMaxTemp(120);
    //m_mid_btn->SetMinSize((wxSize(-1, FromDIP(58))));
    //m_mid_btn->SetBorderWidth(0);
    //m_mid_btn->SetReadOnly(true);
    //// m_mid_btn->SetTextBindInput();
    //m_mid_btn->SetBorderColor(tempinput_border_colour);
    //m_mid_btn->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});
    //m_mid_btn->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
    //    event.Skip();
    //    lostFocusmodifyTemp();
    //});
    //bSizer_temperature->AddSpacer(FromDIP(16));
    //bSizer_temperature->Add(m_mid_btn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(28));
    //bSizer_temperature->AddStretchSpacer();
    //bSizer_temperature->Fit(m_panel_temperature);
    //m_panel_temperature->SetSizer(bSizer_temperature);
    //m_panel_temperature->Layout();
    //idleSizer->Add(m_panel_temperature, 0, wxALL, 0);

    //// 添加空白间距
    //auto m_panel_separotor5 = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    //m_panel_separotor5->SetBackgroundColour(wxColour(240, 240, 240));
    //m_panel_separotor5->SetMinSize(wxSize(-1, FromDIP(8)));
    //m_panel_separotor5->SetMaxSize(wxSize(-1, FromDIP(8)));
    //idleSizer->Add(m_panel_separotor5, 0, wxEXPAND, 0);

    // 创建灯控件布局

    m_panel_idle_device_state = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_idle_device_state->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* deviceStateSizer = new wxBoxSizer(wxVERTICAL);
    setupLayoutIdleDeviceState(deviceStateSizer, m_panel_idle_device_state, idle);
    m_panel_idle_device_state->SetSizer(deviceStateSizer);
    m_panel_idle_device_state->Layout();
    deviceStateSizer->Fit(m_panel_idle_device_state);

    idleSizer->Add(m_panel_idle_device_state, 0, wxALL | wxEXPAND, 0);

    //***添加设备信息布局
    m_panel_idle_device_info = new DeviceInfoPanel(parent, wxSize(-1, FromDIP(277)));
    //***添加循环过滤信息布局
    m_panel_circula_filter = new StartFiltering(parent);
    idleSizer->Add(m_panel_idle_device_info, 0, wxALL | wxEXPAND, 0);
    idleSizer->Add(m_panel_circula_filter, 0, wxALL | wxEXPAND, 0);
    m_panel_idle_device_info->Hide();
    m_panel_circula_filter->Hide();

    m_panel_idle_device_title = new wxPanel(parent);
    m_panel_idle_device_title->SetBackgroundColour(wxColor(248, 248, 248));
    m_panel_idle_device_title->SetMinSize(wxSize(-1, FromDIP(49)));
    auto device_title = new Label(m_panel_idle_device_title, _L("Device Info"));
    auto title_sizer  = new wxBoxSizer(wxHORIZONTAL);
    title_sizer->Add(device_title, 0, wxLEFT | wxALIGN_CENTER, FromDIP(20));
    title_sizer->AddStretchSpacer();
    m_panel_idle_device_title->SetSizer(title_sizer);
    title_sizer->Fit(m_panel_idle_device_title);
    m_panel_idle_device_title->Layout();
    //***添加设备信息布局
    m_panel_u_device = new DeviceInfoPanel(parent, wxSize(-1, FromDIP(298)));
    idleSizer->Add(m_panel_idle_device_title, 0, wxALL | wxEXPAND, 0);
    idleSizer->Add(m_panel_u_device, 0, wxALL | wxEXPAND, 0);
    m_panel_idle_device_title->Hide();
    m_panel_u_device->Hide();

    parent->SetSizer(idleSizer);
    setDisabledMoveCtrl(true);
    setDisabledExtruderCtrl(true);
}



void TempMixDevice::setupLayoutIdleDeviceState(wxBoxSizer *deviceStateSizer, wxPanel *parent,bool idle) 
{
//***灯控制布局
    wxBoxSizer *bSizer_control_lamp  = new wxBoxSizer(wxHORIZONTAL);
    auto        m_panel_control_lamp = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTAB_TRAVERSAL);
    m_panel_control_lamp->SetBackgroundColour(wxColour(255, 255, 255));

    // 显示文件信息按钮
    wxString file_pic = idle ? "device_idle_file_info" :"device_file_info";
    m_idle_device_info_button = new Button(m_panel_control_lamp, wxString(""), file_pic, 0, 24);
    //m_idle_device_info_button->SetFont(wxFont(wxFontInfo(16)));
    m_idle_device_info_button->SetBorderWidth(0);
    m_idle_device_info_button->SetBackgroundColor(wxColour(255, 255, 255));
//    m_idle_device_info_button->SetBackgroundColor(wxColour(217, 234, 255));
    m_idle_device_info_button->SetBorderColor(wxColour(255, 255, 255));
    // m_idle_device_info_button->SetTextColor(wxColour(51,51,51));
    m_idle_device_info_button->SetMinSize((wxSize(-1, FromDIP(69))));
    m_idle_device_info_button->SetCornerRadius(0);
    //bSizer_control_lamp->Add(m_idle_device_info_button, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM , FromDIP(4));
    bSizer_control_lamp->Add(m_idle_device_info_button, wxSizerFlags(1).Expand());
    /*bSizer_control_lamp->AddSpacer(FromDIP(8));*/

    //// 显示灯控制按钮
    //m_idle_lamp_control_button = new Button(m_panel_control_lamp, wxString(""), "device_lamp_control", 0, 24);
    ////m_idle_lamp_control_button->SetFont(wxFont(wxFontInfo(16)));
    //m_idle_lamp_control_button->SetBorderWidth(0);
    //m_idle_lamp_control_button->SetBackgroundColor(wxColour(255, 255, 255));
    //m_idle_lamp_control_button->SetBorderColor(wxColour(255, 255, 255));
    //// m_idle_lamp_control_button->SetTextColor(wxColour(51,51,51));
    //m_idle_lamp_control_button->SetMinSize((wxSize(-1, FromDIP(69))));
    //m_idle_lamp_control_button->SetCornerRadius(0);

    ////bSizer_control_lamp->Add(m_idle_lamp_control_button, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(4));
    //bSizer_control_lamp->Add(m_idle_lamp_control_button, wxSizerFlags(1).Expand());
    //bSizer_control_lamp->AddSpacer(FromDIP(10));

    // 显示过滤按钮
    m_idle_filter_button = new Button(m_panel_control_lamp, wxString(""), "device_filter", 0, 24);
    //m_idle_filter_button->SetFont(wxFont(wxFontInfo(16)));
    m_idle_filter_button->SetBorderWidth(0);
    m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
    m_idle_filter_button->SetBorderColor(wxColour(255, 255, 255));
    // m_idle_filter_button->SetTextColor(wxColour(51,51,51));
    m_idle_filter_button->SetMinSize((wxSize(-1, FromDIP(69))));
    m_idle_filter_button->SetCornerRadius(0);
    //bSizer_control_lamp->Add(m_idle_filter_button, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(4));
    bSizer_control_lamp->Add(m_idle_filter_button, wxSizerFlags(1).Expand());

    //***灯控制布局添加至垂直布局
    m_panel_control_lamp->SetSizer(bSizer_control_lamp);
    m_panel_control_lamp->Layout();
    bSizer_control_lamp->Fit(m_panel_control_lamp);

    deviceStateSizer->Add(m_panel_control_lamp, 0, wxALL | wxEXPAND, 0);
}

void TempMixDevice::connectEvent()
{
    //idle button slot
    m_idle_device_info_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onDevInfoBtnClicked, this);
    //m_idle_lamp_control_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onLampBtnClicked, this);
    m_idle_filter_button->Bind(wxEVT_LEFT_DOWN, &TempMixDevice::onFilterBtnClicked, this);
}

void TempMixDevice::onDevInfoBtnClicked(wxMouseEvent &event)
{
    //event.Skip();
    if (m_panel_idle_device_info) {
        bool bShow = !m_panel_idle_device_info->IsShown();
        hideMonitorPanel(bShow);
        this->GetParent()->Freeze();
        m_panel_idle_device_info->Show(bShow);
        this->GetParent()->Layout();
        this->GetParent()->Thaw();
        //m_blank_page->Show(bShow);
        
        if (bShow) {
            m_idle_device_info_button->SetBackgroundColor(wxColour(217, 234, 255));
        } else {
            m_idle_device_info_button->SetBackgroundColor(wxColour(255, 255, 255));
        }
    }
    if (m_panel_circula_filter) {
        m_panel_circula_filter->Hide();
    }
    if (m_idle_filter_button) {
        m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
    }
}

void TempMixDevice::onLampBtnClicked(wxMouseEvent &event) 
{
    //event.Skip();
    //if (m_idle_lamp_control_button->GetFlashForgeSelected()) {
    //    // 关灯
    //    Slic3r::GUI::ComLightCtrl *lightctrl = new Slic3r::GUI::ComLightCtrl(CLOSE);
    //    // 测试，临时将id写死
    //    if (m_cur_id >= 0) {
    //        Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, lightctrl);
    //    }
    //    m_idle_lamp_control_button->SetIcon("device_lamp_control");
    //    m_idle_lamp_control_button->Refresh();
    //    m_idle_lamp_control_button->SetFlashForgeSelected(false);
    //} else {
    //    // 开灯
    //    Slic3r::GUI::ComLightCtrl *lightctrl = new Slic3r::GUI::ComLightCtrl(OPEN);
    //    // 测试，临时将id写死
    //    if (m_cur_id >= 0) {
    //        Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, lightctrl);
    //    }
    //    m_idle_lamp_control_button->SetIcon("device_lamp_control_press");
    //    m_idle_lamp_control_button->Refresh();
    //    m_idle_lamp_control_button->SetFlashForgeSelected(true);
    //}
}

void TempMixDevice::onFilterBtnClicked(wxMouseEvent &event) 
{
    //event.Skip();
    if (m_g3uMachine) {
        m_clearFanPressed = !m_clearFanPressed;
        if (m_clearFanPressed) {
            m_idle_filter_button->SetIcon("device_filter");
            Slic3r::GUI::ComClearFanCtrl* clearFan = new Slic3r::GUI::ComClearFanCtrl(OPEN);
            Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, clearFan);
        } else {
            m_idle_filter_button->SetIcon("device_filter_offline");
            Slic3r::GUI::ComClearFanCtrl* clearFan = new Slic3r::GUI::ComClearFanCtrl(CLOSE);
            Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, clearFan);
        }
    } else {
        if (m_panel_circula_filter) {
            bool bShow = !m_panel_circula_filter->IsShown();
            m_panel_circula_filter->Show(bShow);
            m_panel_circula_filter->Layout();
            hideMonitorPanel(bShow);
            if (bShow) {
                m_idle_filter_button->SetBackgroundColor(wxColour(217, 234, 255));
            } else {
                m_idle_filter_button->SetBackgroundColor(wxColour(255, 255, 255));
            }
        }
    }

    if (m_panel_idle_device_info) {
        m_panel_idle_device_info->Hide();
    }
    if (m_idle_device_info_button) {
        m_idle_device_info_button->SetBackgroundColor(wxColour(255, 255, 255));
    }
    Layout();
}

void TempMixDevice::setDeviceInfoBtnIcon(const wxString &icon) 
{ 
    m_idle_device_info_button->SetIcon(icon);
}

void TempMixDevice::modifyTemp(
    wxString nozzleTemp, wxString platformTemp, wxString cavityTemp, int topTemp, int bottomTemp, int chamberTemp)
{
    /*if (nozzleTemp.compare("/") == 0 && platformTemp.compare("/") == 0 && cavityTemp.compare("/") == 0) {
        m_top_btn->SetTagTemp("--");
        m_bottom_btn->SetTagTemp("--");
        m_mid_btn->SetTagTemp("--");
        m_top_btn->SetCurrTemp("--");
        m_bottom_btn->SetCurrTemp("--");
        m_mid_btn->SetCurrTemp("--");
    } else {
        if (!m_g3uMachine) {
            m_top_btn->SetLabel(nozzleTemp);
            m_bottom_btn->SetLabel(platformTemp);
            m_mid_btn->SetLabel(cavityTemp);
            if (m_right_target_temp != topTemp && !m_top_btn->HasFocus()) {
                m_right_target_temp = topTemp;
                m_top_btn->SetTagTemp(topTemp, true);
            }
            if (m_plat_target_temp != bottomTemp && !m_bottom_btn->HasFocus()) {
                m_plat_target_temp = bottomTemp;
                m_bottom_btn->SetTagTemp(bottomTemp, true);
            }
            if (m_cavity_target_temp != chamberTemp && !m_mid_btn->HasFocus()) {
                m_cavity_target_temp = chamberTemp;
                m_mid_btn->SetTagTemp(chamberTemp, true);
            }
        } else {
            m_top_btn->SetLabel(nozzleTemp);
            m_bottom_btn->SetLabel(platformTemp);
            m_mid_btn->SetLabel(cavityTemp);
            if (m_right_target_temp != topTemp && !m_top_btn->HasFocus()) {
                m_right_target_temp = topTemp;
                m_top_btn->SetTagTemp(topTemp, true);
            }
            if (m_plat_target_temp != bottomTemp && !m_bottom_btn->HasFocus()) {
                m_plat_target_temp = bottomTemp;
                m_bottom_btn->SetTagTemp(bottomTemp, true);
            }
            if (m_cavity_target_temp != chamberTemp && !m_mid_btn->HasFocus()) {
                m_cavity_target_temp = chamberTemp;
                m_mid_btn->SetTagTemp(chamberTemp, true);
            }
        }
    }*/
}

void TempMixDevice::modifyDeviceInfo(wxString machineType,
                                     wxString sprayNozzle,
                                     wxString printSize,
                                     wxString version,
                                     wxString number,
                                     wxString time,
                                     wxString material,
                                     wxString ip)
{
    m_panel_idle_device_info->SetDeviceInfo(machineType, sprayNozzle, printSize, version, number, time, material, ip);
    m_panel_u_device->SetDeviceInfo(machineType, sprayNozzle, printSize, version, number, time, material, ip);
}

void TempMixDevice::modifyDeviceLampState(bool bOpen) 
{
    /*if (bOpen) {
        m_idle_lamp_control_button->SetIcon("device_lamp_control_press");
        m_idle_lamp_control_button->Refresh();
        m_idle_lamp_control_button->SetFlashForgeSelected(true);
    } else {
        m_idle_lamp_control_button->SetIcon("device_lamp_control");
        m_idle_lamp_control_button->Refresh();
        m_idle_lamp_control_button->SetFlashForgeSelected(false);
    }*/
}

void TempMixDevice::modifyDeviceFilterState(bool internalOpen, bool externalOpen) 
{
    m_panel_circula_filter->setBtnState(internalOpen, externalOpen);
}

void TempMixDevice::modifyG3UClearFanState(bool bOpen) 
{
    if (bOpen) {
        m_idle_filter_button->SetIcon("device_filter");
        m_clearFanPressed = true;
    } else {
        m_idle_filter_button->SetIcon("device_filter_offline");
        m_clearFanPressed = false;
    }
}

void TempMixDevice::modifyDevicePositonState(double x, double y, double z) 
{ 
    /*m_x_text->SetLabelText(wxString((boost::format("X % 0.1f(mm)") % x).str()));
    m_y_text->SetLabelText(wxString((boost::format("Y % 0.1f(mm)") % y).str()));
    m_z_text->SetLabelText(wxString((boost::format("Z % 0.1f(mm)") % z).str()));*/
}

void TempMixDevice::hideMonitorPanel(bool b) 
{ 
    auto event = new wxCommandEvent(EVT_HIDE_PANEL);
    event->SetInt(b);
    wxQueueEvent(this, event);
}

PosCtrlButton::PosCtrlButton(wxWindow* parent, int* step) : 
    Button(parent, "", "", wxNO_BORDER), m_step(step)
{ 
    this->SetMinSize(FromDIP(wxSize(133, 133)));
    this->SetMaxSize(FromDIP(wxSize(133, 133)));
    this->SetBackgroundColor(*wxWHITE);
    this->SetBorderWidth(0);
    this->Bind(wxEVT_LEFT_UP, [&](wxMouseEvent& event) {
        if (!IsEnabled()) {
            return;
        }
        m_dirState[m_mouse_down] = StateColor::Normal;
        m_mouse_down = -1;
        Refresh();
    });
    this->Bind(wxEVT_LEFT_DOWN, [&](wxMouseEvent& event) {
        if (!IsEnabled()) {
            return;
        }
        if (!m_step || m_cur_id == -1) {
            return;
        }
        for (auto i = 0; i < 4; i++) {
            if (m_dirRect[i].Contains(event.GetPosition())) {
                m_dirState[i] = StateColor::Pressed;
                m_mouse_down = i;
                std::string str  = m_axisDir[i].x ? "y" : "x";
                auto        comm = new ComMoveCtrl(str.c_str(), m_axisDir[i].y * (*m_step));
                Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comm);
                break;
            }
        }
        Refresh();
    });
    this->Bind(wxEVT_MOTION, [&](wxMouseEvent& event) {
        if (!IsEnabled()) {
            return;
        }
        if (m_mouse_down != -1) {
            return;
        }
        for (auto i = 0; i < 4; i++) {
            if (m_dirRect[i].Contains(event.GetPosition())) {
                m_dirState[i] = StateColor::Hovered;
            } else {
                m_dirState[i] = StateColor::Normal; 
            }
        }
        Refresh();
    });

    this->Bind(wxEVT_PAINT, [&](wxPaintEvent& event) {
        wxPaintDC dc(this);
        render(dc);
    });
    for (auto i = 0; i < 4; i++) {
        m_dirState[i] = StateColor::Normal;
    }
    m_arrow_size            = FromDIP(24);
    auto          size      = GetMinSize();
    const int     border    = FromDIP(8);
    const wxPoint arrPos[4] = {
        wxPoint((size.x - m_arrow_size) / 2, border), 
        wxPoint(border, (size.y - m_arrow_size) / 2), 
        wxPoint((size.x - m_arrow_size) / 2, size.y - border - m_arrow_size), 
        wxPoint(size.x - border - m_arrow_size, (size.y - m_arrow_size) / 2)
    };
    for (auto i = 0; i < 4; i++) {
        m_dirRect[i] = wxRect(arrPos[i], wxSize(m_arrow_size, m_arrow_size));
    }
}

void PosCtrlButton::render(wxDC& dc) 
{ 
    dc.SetBrush(*wxWHITE);
    dc.SetPen(wxPen(*wxWHITE, 0));
    dc.DrawRectangle(wxPoint(0, 0), GetSize());
    if (!this->IsEnabled()) {
        ScalableBitmap bg(this, "pos_ctrl_bg_disabled", ToDIP(GetMinSize().x));
        dc.DrawBitmap(bg.bmp(), wxPoint(0, 0));
        for (auto i = 0; i < 4; i++) {
            ScalableBitmap arr(this, "arrow_" + m_arrows[i] + "_disabled", ToDIP(m_arrow_size));
            dc.DrawBitmap(arr.bmp(), m_dirRect[i].GetLeftTop());
        }
        return;
    }
    ScalableBitmap bg(this, "pos_ctrl_bg_normal", ToDIP(GetMinSize().x));
    dc.DrawBitmap(bg.bmp(), wxPoint(0, 0));
    map<int, string> stateTypes;
    stateTypes[StateColor::Normal] = "_normal";
    stateTypes[StateColor::Hovered] = "_hover";
    stateTypes[StateColor::Pressed] = "_pressed";
    for (auto i = 0; i < 4; i++) {
        ScalableBitmap arr(this, "arrow_" + m_arrows[i] + stateTypes[this->m_dirState[i]], ToDIP(m_arrow_size));
        dc.DrawBitmap(arr.bmp(), m_dirRect[i].GetLeftTop());
    }
    return;
}

void PosCtrlButton::SetCurId(int curId) 
{ 
    m_cur_id = curId; 
}

NewTempInputPanel::NewTempInputPanel(wxWindow* parent) : 
    wxPanel(parent, wxID_ANY)  
{ 
    SetSize(wxSize(FromDIP(491), FromDIP(286)));
    SetMinSize(wxSize(FromDIP(491), FromDIP(286)));
    SetBackgroundColour(*wxWHITE);
    m_tempSizer = new wxBoxSizer(wxHORIZONTAL);
    auto main_panel = new wxPanel(this);
    main_panel->SetMinSize(wxSize(FromDIP(491), FromDIP(286)));
    auto       main_panel_sizer = new wxBoxSizer(wxVERTICAL);
    auto sizer          = new wxBoxSizer(wxHORIZONTAL);
    auto top_temp = new NewTempInput(main_panel, wxString("device_top_temperature"));
    m_tempInputs["top"] = top_temp;
    auto bottom_temp = new NewTempInput(main_panel, wxString("device_bottom_temperature"));
    m_tempInputs["bottom"] = bottom_temp;
    auto mid_temp = new NewTempInput(main_panel, wxString("device_mid_temperature"));
    m_tempInputs["mid"] = mid_temp;
    main_panel_sizer->AddSpacer(FromDIP(16));
    main_panel_sizer->Add(top_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
    main_panel_sizer->AddSpacer(FromDIP(16));
    main_panel_sizer->Add(bottom_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
    main_panel_sizer->AddSpacer(FromDIP(16));
    main_panel_sizer->Add(mid_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
    main_panel_sizer->AddSpacer(FromDIP(58));
    for (auto temp : m_tempInputs) {
        temp.second->SetCurrTemp(INT_MAX);
        temp.second->EnableTargetTemp(true);
        temp.second->SetWindowStyle(wxALIGN_CENTER);
        temp.second->SetMinSize((wxSize(-1, FromDIP(58))));
        temp.second->SetBorderWidth(0);
    }
    main_panel->SetSizer(main_panel_sizer);
    main_panel_sizer->Fit(main_panel);
    main_panel->Layout();
    m_tempSizer->Add(main_panel, 0, wxALL | wxEXPAND, 0);
    sizer->Add(m_tempSizer, 0, wxALL | wxEXPAND, 0);
    SetSizer(sizer);
    sizer->Fit(this);
    Layout();
}

NewTempInput* NewTempInputPanel::tempInput(const char* key) const
{
    auto it = m_tempInputs.find(key);
    return it == m_tempInputs.end() ? nullptr : it->second;
}

void NewTempInputPanel::setTemps(const char* key, double curr, double target)
{
    // A row the current layout does not have is skipped: reading a missing row through
    // operator[] inserted a null and crashed the device page (EDGESLICER-6).
    if (NewTempInput* input = tempInput(key)) {
        input->SetCurrTemp(curr, true);
        input->SetTagTemp(target, true);
    }
}

void NewTempInputPanel::UpdateTempatrue(const com_dev_data_t& data)
{
    if (m_cur_id == -1 || data.devDetail == nullptr) {
        for (auto temp : m_tempInputs) {
            temp.second->SetCurrTemp(INT_MAX);
        }
        return;
    }

    // The panel was built before the printer said what it is (or it said something else since):
    // build it again for what it is now, so the rows below are the ones this layout has.
    const unsigned short pid = FFUtils::getPid(data);
    if (pid != m_layout_pid || FFUtils::tempLayout(pid, data.devDetail->nozzleCnt) != m_layout) {
        buildTempLayout(m_cur_id, pid, data.devDetail->nozzleCnt);
    }

    const fnet_dev_detail_t& detail = *data.devDetail;
    switch (m_layout) {
    case FFTempLayout::Nozzles4:
    case FFTempLayout::Nozzles4Chamber: {
        if (detail.nozzleCnt < 4 || detail.nozzleTemps == nullptr || detail.nozzleTargetTemps == nullptr) {
            return;
        }
        setTemps("t1", detail.nozzleTemps[0], detail.nozzleTargetTemps[0]);
        setTemps("t2", detail.nozzleTemps[1], detail.nozzleTargetTemps[1]);
        setTemps("t3", detail.nozzleTemps[2], detail.nozzleTargetTemps[2]);
        setTemps("t4", detail.nozzleTemps[3], detail.nozzleTargetTemps[3]);
        if (m_layout == FFTempLayout::Nozzles4) {
            setTemps("mid", detail.platTemp, detail.platTargetTemp);
        } else {
            setTemps("bottom", detail.platTemp, detail.platTargetTemp);
            setTemps("mid", detail.chamberTemp, detail.chamberTargetTemp);
        }
        break;
    }
    case FFTempLayout::Guider3Ultra:
        setTemps("top", detail.rightTemp, detail.rightTargetTemp);
        setTemps("bottom", detail.leftTemp, detail.leftTargetTemp);
        setTemps("mid", detail.platTemp, detail.platTargetTemp);
        break;
    case FFTempLayout::Generic:
    default:
        setTemps("top", detail.rightTemp, detail.rightTargetTemp);
        setTemps("bottom", detail.platTemp, detail.platTargetTemp);
        setTemps("mid", detail.chamberTemp, detail.chamberTargetTemp);
        break;
    }
}

void NewTempInputPanel::ReInitTempature(int curId)
{
    unsigned short pid       = 0;
    int            nozzleCnt = 0;
    if (curId != -1) {
        bool                  valid = false;
        const com_dev_data_t& data  = MultiComMgr::inst()->devData(curId, &valid);
        if (valid) {
            pid       = FFUtils::getPid(data);
            nozzleCnt = data.devDetail != nullptr ? data.devDetail->nozzleCnt : 0;
        }
    }
    buildTempLayout(curId, pid, nozzleCnt);
}

void NewTempInputPanel::buildTempLayout(int curId, unsigned short pid, int nozzleCnt)
{
    m_tempInputs.clear();
    m_tempSizer->Clear(true);
    m_cur_id = curId;
    m_layout = FFTempLayout::Generic;
    m_layout_pid = 0;
    m_layout_known = false;

    StateColor tempinput_text_colour(std::make_pair(wxColour(51, 51, 51), (int)StateColor::Disabled),
        std::make_pair(wxColour(48, 58, 60), (int)StateColor::Normal));
    StateColor tempinput_border_colour(std::make_pair(*wxWHITE, (int)StateColor::Disabled),
        std::make_pair(wxColour(0, 150, 136), (int)StateColor::Focused),
        std::make_pair(wxColour(0, 150, 136), (int)StateColor::Hovered),
        std::make_pair(*wxWHITE, (int)StateColor::Normal));
    auto main_panel = new wxPanel(this);
    main_panel->SetMinSize(wxSize(FromDIP(491), FromDIP(286)));
    auto       main_panel_sizer = new wxBoxSizer(wxVERTICAL);

    if (curId == -1) {
        auto top_temp = new NewTempInput(main_panel, wxString("device_top_temperature"));
        m_tempInputs["top"] = top_temp;
        auto bottom_temp = new NewTempInput(main_panel, wxString("device_bottom_temperature"));
        m_tempInputs["bottom"] = bottom_temp;
        auto mid_temp = new NewTempInput(main_panel, wxString("device_mid_temperature"));
        m_tempInputs["mid"] = mid_temp;

        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(top_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(bottom_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(mid_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(58));

		main_panel->SetSizer(main_panel_sizer);
		main_panel_sizer->Fit(main_panel);
		main_panel->Layout();
		m_tempSizer->Add(main_panel, 0, wxALL | wxEXPAND, 0);

        for (auto temp : m_tempInputs) {
            temp.second->SetCurrTemp(INT_MAX);
            temp.second->EnableTargetTemp(true);
            temp.second->SetWindowStyle(wxALIGN_CENTER);
            temp.second->SetMinSize((wxSize(-1, FromDIP(58))));
            temp.second->SetBorderWidth(0);
            temp.second->SetTextColor(tempinput_text_colour);
            temp.second->SetBorderColor(tempinput_border_colour);
        }
        Layout();
		return;
    }

    m_layout_pid   = pid;
    m_layout       = FFUtils::tempLayout(pid, nozzleCnt);
    // A model this build does not know gets its temperatures shown, but no target is sent to it:
    // its limits are not known here.
    m_layout_known = FFUtils::isKnownPid(pid);
    if (!m_layout_known) {
        BOOST_LOG_TRIVIAL(warning) << "[FlashForge] device page: unknown product id 0x" << std::hex << pid << std::dec
                                   << " (" << nozzleCnt << " nozzles); temperatures shown read-only";
    }

    auto make_nozzle = [&](int index) {
        auto t = new NewTempInput(main_panel);
        t->SetNozzleIndex(index);
        t->SetMinTemp(0);
        t->SetMaxTemp(320);
        m_tempInputs["t" + std::to_string(index)] = t;
        return t;
    };

    switch (m_layout) {
    case FFTempLayout::Nozzles4: {
        auto       u1_panel_up_sizer = new wxGridSizer(2, 2, FromDIP(19), FromDIP(26));
        auto t1_temp = make_nozzle(1);
        auto t2_temp = make_nozzle(2);
        auto t3_temp = make_nozzle(3);
        auto t4_temp = make_nozzle(4);
        auto mid_temp = new NewTempInput(main_panel, wxString("device_bottom_temperature"));
        mid_temp->SetMinTemp(0);
        mid_temp->SetMaxTemp(65);
        m_tempInputs["mid"] = mid_temp;

        u1_panel_up_sizer->Add(t1_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t2_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t3_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t4_temp, 0, wxEXPAND, 0);
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(u1_panel_up_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(19));
        main_panel_sizer->Add(mid_temp, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(58));
        break;
    }
    case FFTempLayout::Nozzles4Chamber: {
        auto u1_panel_up_sizer = new wxGridSizer(3, 2, FromDIP(19), FromDIP(26));
        auto t1_temp = make_nozzle(1);
        auto t2_temp = make_nozzle(2);
        auto t3_temp = make_nozzle(3);
        auto t4_temp = make_nozzle(4);
        auto bottom_temp      = new NewTempInput(main_panel, wxString("device_bottom_temperature"));
        bottom_temp->SetMinTemp(0);
        bottom_temp->SetMaxTemp(120);
        m_tempInputs["bottom"] = bottom_temp;
        auto mid_temp       = new NewTempInput(main_panel, wxString("device_mid_temperature"));
        mid_temp->SetMinTemp(0);
        mid_temp->SetMaxTemp(65);
        m_tempInputs["mid"] = mid_temp;

        u1_panel_up_sizer->Add(t1_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t2_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t3_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(t4_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(bottom_temp, 0, wxEXPAND, 0);
        u1_panel_up_sizer->Add(mid_temp, 0, wxEXPAND, 0);
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(u1_panel_up_sizer, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(58));
        break;
    }
    case FFTempLayout::Guider3Ultra:
    case FFTempLayout::Generic:
    default: {
        auto top_temp = new NewTempInput(main_panel, wxString("device_top_temperature"));
        m_tempInputs["top"] = top_temp;
        auto bottom_temp = new NewTempInput(main_panel, wxString("device_bottom_temperature"));
        m_tempInputs["bottom"] = bottom_temp;
        auto mid_temp = new NewTempInput(main_panel, wxString("device_mid_temperature"));
        m_tempInputs["mid"] = mid_temp;
        if (pid == ADVENTURER_5M || pid == ADVENTURER_5M_PRO || pid == ADVENTURER_A5) {
            top_temp->SetMinTemp(0);
            top_temp->SetMaxTemp(280);
            bottom_temp->SetMinTemp(0);
            bottom_temp->SetMaxTemp(110);
            mid_temp->SetReadOnly(true);
            mid_temp->SetTagTemp(INT_MAX);
        }
        else if (pid == GUIDER_4 || pid == GUIDER_4_PRO) {
            top_temp->SetMinTemp(0);
            top_temp->SetMaxTemp(320);
            bottom_temp->SetMinTemp(0);
            bottom_temp->SetMaxTemp(120);
            mid_temp->SetReadOnly(false);
            mid_temp->SetMinTemp(0);
            mid_temp->SetMaxTemp(60);
        }
        else if (pid == AD5X) {
            top_temp->SetMinTemp(0);
            top_temp->SetMaxTemp(300);
            bottom_temp->SetMinTemp(0);
            bottom_temp->SetMaxTemp(110);
            mid_temp->SetReadOnly(true);
            mid_temp->SetTagTemp(INT_MAX);
        }
        else if (pid == GUIDER_3_ULTRA) {
            top_temp->SetMinTemp(0);
            top_temp->SetMaxTemp(350);
            bottom_temp->SetMinTemp(0);
            bottom_temp->SetMaxTemp(350);
            mid_temp->SetReadOnly(false);
            mid_temp->SetMinTemp(0);
            mid_temp->SetMaxTemp(120);
            top_temp->SetNormalIcon("device_right_temperature");
            bottom_temp->SetNormalIcon("device_left_temperature");
            mid_temp->SetNormalIcon("device_bottom_temperature");
        }

        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(top_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(bottom_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(16));
        main_panel_sizer->Add(mid_temp, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(26));
        main_panel_sizer->AddSpacer(FromDIP(58));
        break;
    }
    }

    if (!m_layout_known) {
        for (auto temp : m_tempInputs) {
            temp.second->SetMinTemp(0);
            temp.second->SetMaxTemp(500);
            temp.second->SetReadOnly(true);
        }
    }

    main_panel->SetSizer(main_panel_sizer);
    main_panel_sizer->Fit(main_panel);
    main_panel->Layout();
    m_tempSizer->Add(main_panel, 0, wxALL | wxEXPAND, 0);

    for (auto temp : m_tempInputs) {
        temp.second->EnableTargetTemp(true);
        temp.second->SetWindowStyle(wxALIGN_CENTER);
        temp.second->SetMinSize((wxSize(-1, FromDIP(58))));
        temp.second->SetBorderWidth(0);
        temp.second->SetTextColor(tempinput_text_colour);
        temp.second->SetBorderColor(tempinput_border_colour);
        temp.second->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            event.Skip();
            lostTempModify();
        });
        temp.second->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& event) {
            event.Skip();
            lostTempModify();
        });
    }
    Layout();
}

void NewTempInputPanel::SwitchTargetTemp(bool flag)
{
    /*for (auto temp : m_tempInputs) {
        temp.second->EnableTargetTemp(flag);
    }*/
}

void NewTempInputPanel::lostTempModify()
{
    if (m_cur_id == -1 || !m_layout_known) {
        return;
    }
    // The command follows the layout the panel was built with, so every row it reads exists.
    auto tag = [this](const char* key, double& out) {
        NewTempInput* input = tempInput(key);
        if (input == nullptr)
            return false;
        out = input->GetTagTemp();
        return true;
    };
    switch (m_layout) {
    case FFTempLayout::Generic:
    case FFTempLayout::Guider3Ultra: {
        double top_tag_temp, bottom_tag_temp, mid_tag_temp;
        if (!tag("top", top_tag_temp) || !tag("bottom", bottom_tag_temp) || !tag("mid", mid_tag_temp))
            return;
        if (m_layout == FFTempLayout::Guider3Ultra) {
            ComTempCtrl* tempCtrl = new ComTempCtrl(mid_tag_temp, top_tag_temp, bottom_tag_temp, 0);
            MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
        }
        else {
            ComTempCtrl* tempCtrl = new ComTempCtrl(bottom_tag_temp, top_tag_temp, 0, mid_tag_temp);
            MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
        }
        break;
    }
    case FFTempLayout::Nozzles4: {
        double t1_tag_temp, t2_tag_temp, t3_tag_temp, t4_tag_temp, mid_tag_temp;
        if (!tag("t1", t1_tag_temp) || !tag("t2", t2_tag_temp) || !tag("t3", t3_tag_temp) || !tag("t4", t4_tag_temp) ||
            !tag("mid", mid_tag_temp))
            return;
        std::vector<double> nozzlesTemp  = {t1_tag_temp, t2_tag_temp, t3_tag_temp, t4_tag_temp};
        ComTempCtrl*        tempCtrl     = new ComTempCtrl(mid_tag_temp, 0, 0, 0);
        tempCtrl->addNozzlesTemp(nozzlesTemp);
        MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
        break;
    }
    case FFTempLayout::Nozzles4Chamber: {
        double t1_tag_temp, t2_tag_temp, t3_tag_temp, t4_tag_temp, mid_tag_temp, bottom_tag_temp;
        if (!tag("t1", t1_tag_temp) || !tag("t2", t2_tag_temp) || !tag("t3", t3_tag_temp) || !tag("t4", t4_tag_temp) ||
            !tag("mid", mid_tag_temp) || !tag("bottom", bottom_tag_temp))
            return;
        std::vector<double> nozzlesTemp = { t1_tag_temp, t2_tag_temp, t3_tag_temp, t4_tag_temp };
        ComTempCtrl* tempCtrl = new ComTempCtrl(bottom_tag_temp, 0, 0, mid_tag_temp);
        tempCtrl->addNozzlesTemp(nozzlesTemp);
        MultiComMgr::inst()->putCommand(m_cur_id, tempCtrl);
        break;
    }
    }
}

DeviceInfoPanel::DeviceInfoPanel(wxWindow* parent, wxSize size):
    wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxTAB_TRAVERSAL)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(size);
    wxBoxSizer* deviceInfoSizer = new wxBoxSizer(wxVERTICAL);
    setupLayoutDeviceInfo(deviceInfoSizer, this);
    SetSizer(deviceInfoSizer);
    Layout();
    deviceInfoSizer->Fit(this);
}

void DeviceInfoPanel::SetDeviceInfo(wxString machineType, wxString sprayNozzle, wxString printSize, wxString version, wxString number, wxString time, wxString material, wxString ip)
{
    m_machine_type_data->SetLabel(machineType);
    m_spray_nozzle_data->SetLabel(sprayNozzle);
    m_print_size_data->SetLabel(printSize);
    m_firmware_version_data->SetLabel(version);
    m_serial_number_data->SetLabel(number);
    m_cumulative_print_time->SetLabel(time);
    m_private_material_data->SetLabel(material);
    m_ipAddr->SetLabel(ip);
}

void DeviceInfoPanel::setupLayoutDeviceInfo(wxBoxSizer* deviceInfoSizer, wxPanel* parent)
{
    //水平布局中添加垂直布局
    wxBoxSizer* bSizer_device_info = new wxBoxSizer(wxVERTICAL);
    //
    wxBoxSizer* deviceStateSizer = new wxBoxSizer(wxHORIZONTAL);
    auto m_panel_device_info = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(180), -1), wxTAB_TRAVERSAL);
    m_panel_device_info->SetBackgroundColour(wxColour(255, 255, 255));

    //  添加空白间距
    auto m_panel_separotor0 = new wxPanel(m_panel_device_info, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_separotor0->SetBackgroundColour(wxColour(255, 255, 255));
    m_panel_separotor0->SetMinSize(wxSize(FromDIP(20), -1));

    deviceStateSizer->Add(m_panel_separotor0);

    wxBoxSizer* bSizer_device_name = new wxBoxSizer(wxVERTICAL);
    auto machine_type = new Label(m_panel_device_info, _L("Machine Type"));
    machine_type->Wrap(-1);
    //machine_type->SetFont(wxFont(wxFontInfo(16)));
    machine_type->SetForegroundColour(wxColour(153, 153, 153));
    machine_type->SetBackgroundColour(wxColour(255, 255, 255));
    machine_type->SetMinSize(wxSize(FromDIP(120), -1));

    auto spray_nozzle = new Label(m_panel_device_info, _L("Spray Nozzle"));
    //spray_nozzle->SetFont(wxFont(wxFontInfo(16)));
    spray_nozzle->SetForegroundColour(wxColour(153, 153, 153));
    spray_nozzle->SetBackgroundColour(wxColour(255, 255, 255));

    auto print_size = new Label(m_panel_device_info, _L("Print Size"));
    //print_size->SetFont(wxFont(wxFontInfo(16)));
    print_size->SetForegroundColour(wxColour(153, 153, 153));
    print_size->SetBackgroundColour(wxColour(255, 255, 255));

    auto firmware_version = new Label(m_panel_device_info, _L("Firmware Version"));
    //firmware_version->SetFont(wxFont(wxFontInfo(16)));
    firmware_version->SetForegroundColour(wxColour(153, 153, 153));
    firmware_version->SetBackgroundColour(wxColour(255, 255, 255));

    auto serial_number = new Label(m_panel_device_info, _L("Serial Number"));
    //serial_number->SetFont(wxFont(wxFontInfo(16)));
    serial_number->SetForegroundColour(wxColour(153, 153, 153));
    serial_number->SetBackgroundColour(wxColour(255, 255, 255));

    auto cumulative_print_time = new Label(m_panel_device_info, _L("Printing Time"));
    // cumulative_print_time->SetFont(wxFont(wxFontInfo(16)));
    cumulative_print_time->SetForegroundColour(wxColour(153, 153, 153));
    cumulative_print_time->SetBackgroundColour(wxColour(255, 255, 255));

    auto private_material = new Label(m_panel_device_info, _L("Private Material Statistics"));
    //private_material->SetFont(wxFont(wxFontInfo(16)));
    private_material->SetForegroundColour(wxColour(153, 153, 153));
    private_material->SetBackgroundColour(wxColour(255, 255, 255));

    auto ip_addr = new Label(m_panel_device_info, _L("IP-Address"));
    // ip_addr->SetFont(wxFont(wxFontInfo(16)));
    ip_addr->SetForegroundColour(wxColour(153, 153, 153));
    ip_addr->SetBackgroundColour(wxColour(255, 255, 255));

    bSizer_device_name->AddSpacer(FromDIP(20));
    bSizer_device_name->Add(machine_type, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(spray_nozzle, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(print_size, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(firmware_version, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(serial_number, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(cumulative_print_time, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(private_material, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(13));
    bSizer_device_name->Add(ip_addr, 0, wxALL | wxEXPAND, 0);
    bSizer_device_name->AddSpacer(FromDIP(12));
    //bSizer_device_name->AddStretchSpacer();

    m_panel_device_info->SetSizer(bSizer_device_name);
    m_panel_device_info->Layout();
    bSizer_device_name->Fit(m_panel_device_info);
    deviceStateSizer->Add(m_panel_device_info);

    //  添加空白间距
    auto m_panel_separotor1 = new wxPanel(m_panel_device_info, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_separotor1->SetBackgroundColour(wxColour(255, 255, 255));
    m_panel_separotor1->SetMinSize(wxSize(FromDIP(20), -1));

    deviceStateSizer->Add(m_panel_separotor1);

    auto m_panel_device_data = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_panel_device_data->SetBackgroundColour(wxColour(255, 255, 255));

    wxBoxSizer* bSizer_device_data = new wxBoxSizer(wxVERTICAL);
    m_machine_type_data = new Label(m_panel_device_data, ("Adventurer 5M"));
    //m_machine_type_data->Wrap(-1);
    //m_machine_type_data->SetFont(wxFont(wxFontInfo(16)));
    m_machine_type_data->SetForegroundColour(wxColour(51, 51, 51));
    m_machine_type_data->SetBackgroundColour(wxColour(255, 255, 255));
    //m_machine_type_data->SetMinSize(wxSize(FromDIP(155), -1));

    m_spray_nozzle_data = new Label(m_panel_device_data, ("0.4mm"));
    //m_spray_nozzle_data->SetFont(wxFont(wxFontInfo(16)));
    m_spray_nozzle_data->SetForegroundColour(wxColour(51, 51, 51));
    m_spray_nozzle_data->SetBackgroundColour(wxColour(255, 255, 255));

    m_print_size_data = new Label(m_panel_device_data, ("220*220*220mm"));
    //m_print_size_data->SetFont(wxFont(wxFontInfo(16)));
    m_print_size_data->SetForegroundColour(wxColour(51, 51, 51));
    m_print_size_data->SetBackgroundColour(wxColour(255, 255, 255));

    m_firmware_version_data = new Label(m_panel_device_data, ("2.1.4-2.1.6"));
    //m_firmware_version_data->SetFont(wxFont(wxFontInfo(16)));
    m_firmware_version_data->SetForegroundColour(wxColour(51, 51, 51));
    m_firmware_version_data->SetBackgroundColour(wxColour(255, 255, 255));

    m_serial_number_data = new Label(m_panel_device_data, ("ABCDEFG"));
    //m_serial_number_data->SetFont(wxFont(wxFontInfo(16)));
    m_serial_number_data->SetForegroundColour(wxColour(51, 51, 51));
    m_serial_number_data->SetBackgroundColour(wxColour(255, 255, 255));

    m_cumulative_print_time = new Label(m_panel_device_data, ("0 hours"));
    // m_cumulative_print_time->SetFont(wxFont(wxFontInfo(16)));
    m_cumulative_print_time->SetForegroundColour(wxColour(51, 51, 51));
    m_cumulative_print_time->SetBackgroundColour(wxColour(255, 255, 255));

    m_private_material_data = new Label(m_panel_device_data, ("1600.9 m"));
    //m_private_material_data->SetFont(wxFont(wxFontInfo(16)));
    m_private_material_data->SetForegroundColour(wxColour(51, 51, 51));
    m_private_material_data->SetBackgroundColour(wxColour(255, 255, 255));

    m_ipAddr = new Label(m_panel_device_data, ("127.0.0.0"));
    // m_ipAddr->SetFont(wxFont(wxFontInfo(16)));
    m_ipAddr->SetForegroundColour(wxColour(51, 51, 51));
    m_ipAddr->SetBackgroundColour(wxColour(255, 255, 255));

    bSizer_device_data->AddSpacer(FromDIP(20));
    bSizer_device_data->Add(m_machine_type_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_spray_nozzle_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_print_size_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_firmware_version_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_serial_number_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_cumulative_print_time, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_private_material_data, 0, wxALL | wxEXPAND, 0);
    bSizer_device_data->AddSpacer(FromDIP(13));
    bSizer_device_data->Add(m_ipAddr, 0, wxALL | wxEXPAND, 0);
#ifdef __WIN32__
    bSizer_device_data->AddSpacer(FromDIP(12));
#elif defined(__APPLE__)
    bSizer_device_data->AddSpacer(FromDIP(24));
#endif

    m_panel_device_data->SetSizer(bSizer_device_data);
    m_panel_device_data->Layout();
    bSizer_device_data->Fit(m_panel_device_data);

    deviceStateSizer->Add(m_panel_device_data);

    deviceInfoSizer->Add(deviceStateSizer);
}

MachineComboBox::MachineComboBox(
    wxWindow* parent, wxWindowID id, const wxString& value, const wxPoint& pos, const wxSize& size, int n, const wxString choices[])
    : wxOwnerDrawnComboBox(parent, id, value, pos, size, n, choices, wxCB_READONLY)
{
    SetFont(Label::Body_12);
    SetMinSize(wxSize(FromDIP(180), FromDIP(34)));
    m_borderColor = StateColor(std::make_pair(0xF7F8FA, (int) StateColor::Disabled), std::make_pair(0xC9CDD4, (int) StateColor::Hovered),
                               std::make_pair(0xF7F8FA, (int) StateColor::Normal));
    m_bgColor     = StateColor(std::make_pair(0xF0F0F1, (int) StateColor::Disabled), std::make_pair(0xFFFFFF, (int) StateColor::Hovered),
                               std::make_pair(0xF7F8FA, (int) StateColor::Normal));
    m_textColor   = StateColor(std::make_pair(0x6B6B6B, (int) StateColor::Disabled), std::make_pair(0x272828, (int) StateColor::Hovered),
                               std::make_pair(0x272828, (int) StateColor::Normal));
    m_dropdownBgColor  = wxColour(255, 255, 255);
    m_itemSelectedBg   = wxColour(232, 243, 255);
    m_itemSelectedText = wxColour(0x272828);
    m_cornerRadius     = FromDIP(4);
    m_itemHeight       = FromDIP(34);
    m_isHover          = false;
    m_isDropped        = false;

    Bind(wxEVT_ENTER_WINDOW, &MachineComboBox::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &MachineComboBox::OnMouseLeave, this);
    Bind(wxEVT_COMBOBOX_DROPDOWN, &MachineComboBox::OnDropDown, this);
    Bind(wxEVT_COMBOBOX_CLOSEUP, &MachineComboBox::OnCloseUp, this);
    Bind(wxEVT_PAINT, &MachineComboBox::OnPaint, this);

    try {
        m_arrowUp   = ScalableBitmap(this, "combobox_arrow_up", 10);
        m_arrowDown = ScalableBitmap(this, "combobox_arrow_down", 10);
    } catch (...) {}
}

void MachineComboBox::OnPaint(wxPaintEvent& evt)
{
    wxPaintDC dc(this);
    wxGCDC    gdc(dc);

    wxRect rect      = GetClientRect();
    int    arrowSize = 6;
    int    arrowX    = rect.width - arrowSize - 12;
    int    arrowY    = (rect.height - arrowSize) / 2;

    wxColour bg_color     = m_bgColor.colorForStates(!m_isEnabled ? StateColor::Disabled :
                                                     m_isHover    ? StateColor::Hovered | StateColor::Enabled :
                                                                    StateColor::Enabled);
    wxColour text_color   = m_textColor.colorForStates(!m_isEnabled ? StateColor::Disabled :
                                                       m_isHover    ? StateColor::Hovered | StateColor::Enabled :
                                                                      StateColor::Enabled);
    wxColour border_color = m_borderColor.colorForStates(!m_isEnabled ? StateColor::Disabled :
                                                         m_isHover    ? StateColor::Hovered | StateColor::Enabled :
                                                                        StateColor::Enabled);
    gdc.SetBrush(wxBrush(bg_color));
    gdc.SetPen(wxPen(border_color, 1));
    gdc.DrawRoundedRectangle(rect.x, rect.y, rect.width - 1, rect.height - 1, m_cornerRadius);
    gdc.SetTextForeground(text_color);
    wxFont font = GetFont();
    dc.SetFont(font);
    wxString text = GetString(GetSelection());
    if (!text.IsEmpty()) {
        wxSize textSize = dc.GetTextExtent(text);
        int    textX    = 10;
        int    textY    = (rect.height - textSize.GetHeight()) / 2;
        dc.DrawText(text, textX, textY);
    }

    auto& arrowBmp = m_isDropped ? m_arrowUp : m_arrowDown;
    if (arrowBmp.bmp().IsOk()) {
        gdc.DrawBitmap(arrowBmp.bmp(), arrowBmp.GetBmpWidth(), arrowBmp.GetBmpHeight(), true);
    } else {
        gdc.SetPen(wxPen(text_color, 2));
        wxPoint arrowPoints[3];
        if (m_isDropped) {
            arrowPoints[0] = wxPoint(arrowX, arrowY + 3);
            arrowPoints[1] = wxPoint(arrowX + 3, arrowY);
            arrowPoints[2] = wxPoint(arrowX + 6, arrowY + 3);
        } else {
            arrowPoints[0] = wxPoint(arrowX, arrowY);
            arrowPoints[1] = wxPoint(arrowX + 3, arrowY + 3);
            arrowPoints[2] = wxPoint(arrowX + 6, arrowY);
        }
        gdc.DrawLines(3, arrowPoints);
    }
}

wxCoord MachineComboBox::OnMeasureItem(size_t item) const
{
    return m_itemHeight; 
}

void MachineComboBox::OnMouseEnter(wxMouseEvent& evt)
{
    m_isHover = true;
    Refresh();
    evt.Skip();
}

void MachineComboBox::OnMouseLeave(wxMouseEvent& evt)
{
    m_isHover = false;
    Refresh();
    evt.Skip();
}

void MachineComboBox::OnDropDown(wxCommandEvent& evt)
{
    m_isDropped = true;
    Refresh();
    evt.Skip();
}

void MachineComboBox::OnCloseUp(wxCommandEvent& evt)
{
    m_isDropped = false;
    Refresh();
    evt.Skip();
}

void MachineComboBox::OnDrawItem(wxDC& dc, const wxRect& rect, int item, int flags) const
{
    std::unique_ptr<wxGraphicsContext> gdc(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (gdc == nullptr) {
        return;
    }
    wxRect itemRect = rect;

    bool isSelected = (flags & 0x0001) != 0;
    gdc->SetBrush(wxBrush(isSelected ? m_itemSelectedBg : m_dropdownBgColor));
    gdc->SetPen(*wxTRANSPARENT_PEN);
    gdc->DrawRectangle(itemRect.x, itemRect.y, itemRect.width + 2, itemRect.height);

    wxString itemText = GetString(item);
    wxColour text_color = m_textColor.colorForStates(!m_isEnabled ? StateColor::Disabled :
                                                     m_isHover    ? StateColor::Hovered | StateColor::Enabled :
                                                                    StateColor::Enabled);
    dc.SetTextForeground(isSelected ? m_itemSelectedText : text_color);
    wxFont font = GetFont();
    dc.SetFont(font);
    wxSize textSize = dc.GetTextExtent(itemText);
    int    textX    = 10;
    int    textY    = m_itemHeight * item + (itemRect.height - textSize.GetHeight()) / 2;
    dc.DrawText(itemText, textX, textY);
    if (item == GetSelection()) {
        int checkSize = 10;
        int checkX    = itemRect.width - checkSize - 10;
        int checkY    = m_itemHeight * item + (itemRect.height - checkSize) / 2;

        // 绘制勾选标记
        gdc->SetPen(wxPen(isSelected ? *wxWHITE : wxColour(0, 120, 215), 2));
        wxPoint checkPoints[2];
        checkPoints[0] = wxPoint(checkX, checkY + 7);
        checkPoints[1] = wxPoint(checkX + 5, checkY + 12);
        gdc->StrokeLine(checkPoints[0].x, checkPoints[0].y, checkPoints[1].x, checkPoints[1].y);
        checkPoints[0] = wxPoint(checkX + 5, checkY + 12);
        checkPoints[1] = wxPoint(checkX + 12, checkY);
        gdc->StrokeLine(checkPoints[0].x, checkPoints[0].y, checkPoints[1].x, checkPoints[1].y);
    }
}

MachineIconCombo::MachineIconCombo(
    wxWindow* parent, wxWindowID id, const wxString& icon, int iconSize, const wxPoint& pos, const wxSize& size, int n, const wxString choices[]): 
    wxPanel(parent, id, pos, size)
{
    SetBackgroundColour(*wxWHITE);
    wxBoxSizer* sizer        = new wxBoxSizer(wxHORIZONTAL);
    auto        m_panel_page = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_panel_page->SetSize(wxSize(-1, -1));
    m_icon = ScalableBitmap(m_panel_page, icon.ToStdString(), iconSize);
    auto icon_static = new wxStaticBitmap(m_panel_page, wxID_ANY, m_icon.bmp());
    m_combobox = new MachineComboBox(m_panel_page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, n, choices);

    sizer->Add(icon_static, 0, wxALIGN_CENTER | wxALL | wxEXPAND, 0);
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(m_combobox, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxEXPAND, 0);

    m_panel_page->SetSizer(sizer);
    m_panel_page->Layout();
    sizer->Fit(m_panel_page);
}

MachineComboBox* MachineIconCombo::combobox() { return m_combobox; }

} // namespace GUI
} // namespace Slic3r
