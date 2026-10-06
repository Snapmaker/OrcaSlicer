#include "MaterialStation.hpp"
#include <memory>
#include <slic3r/GUI/wxExtensions.hpp>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/graphics.h>
#include "slic3r/GUI/FFUtils.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"

#define UNKNOWN_COLOR wxColour(248, 248, 248)   //材料站背景颜色

namespace Slic3r {
namespace GUI {

FFPrinterPid        MaterialStation::s_PrinterType = OTHER;
MaterialSlotAreaU1* MaterialSlotAreaU1::s_self = nullptr;

MaterialSlot::MaterialSlot(wxWindow*       parent,
                           wxWindowID      id,
                           const wxPoint&  pos,
                           const wxSize&   size,
                           long            style,
                           const wxString& name) 
    : wxWindow(parent, id, pos, size, style, name) 
    , m_material_info{wxEmptyString, wxColour()}
    , m_type(MaterialSlot::Empty)
    , m_edit_state(EditState::Normal)
    , m_edit_white_bmp(create_scaled_bitmap("edit_white_btn", nullptr, 11))
    , m_edit_black_bmp(create_scaled_bitmap("edit_black_btn", nullptr, 11))
    , m_edit_hover_bmp(create_scaled_bitmap("edit_hover_btn", nullptr, 11))
    , m_edit_press_bmp(create_scaled_bitmap("edit_press_btn", nullptr, 11))
    , m_seleced_bmp(create_scaled_bitmap("selected_slot", nullptr, 60))
    , m_unknow_bmp(create_scaled_bitmap("unknow_slot", nullptr, 60))
    , m_empty_bmp(create_scaled_bitmap("empty_slot", nullptr, 60))
    , m_unknow_name_bmp(this, "unknown_name", 12)
    , m_is_editable(true)
{
    SetMinSize(wxSize(FromDIP(60), FromDIP(60)));
    m_edit_pos = wxPoint(FromDIP(34), FromDIP(30));
    m_edit_size = wxSize(FromDIP(11), FromDIP(11));
    SetBackgroundColour(wxColour(255, 255, 255));
    connectEvent();
}   

MaterialSlot::~MaterialSlot() {}

MaterialInfo MaterialSlot::get_material_info() { return m_material_info; }

MaterialSlot::SlotType MaterialSlot::get_slot_type() { return m_type; }

void MaterialSlot::set_slot_type(SlotType type)
{
    m_type = type;
    Refresh();
}

void MaterialSlot::set_edit_state(EditState state)
{
    m_edit_state = state;
    Refresh();
}

void MaterialSlot::set_edit_enable(bool enable) { m_is_editable = enable; }

bool MaterialSlot::is_edit_enable() { return m_is_editable; }

void MaterialSlot::set_material_info(MaterialInfo& info)
{
    m_material_info = info;
    Refresh();
}

void MaterialSlot::connectEvent() 
{ 
    Bind(wxEVT_PAINT, &MaterialSlot::paintEvent, this); 
}

void MaterialSlot::paintEvent(wxPaintEvent& event)
{ 
    wxPaintDC dc(this);
    auto      w = GetSize().GetWidth();
    auto      h = GetSize().GetHeight();
    switch (m_type) {
    case MaterialSlot::Complete: {
        dc.SetBrush(wxBrush(m_material_info.m_color));
        dc.SetPen(wxPen(m_material_info.m_color, 0));
        int iconX = (w - m_seleced_bmp.GetWidth()) / 2;
        int iconY = (h - m_seleced_bmp.GetHeight()) / 2;
        dc.DrawRectangle(iconX, iconY, m_seleced_bmp.GetWidth(), m_seleced_bmp.GetHeight());               // 画背景
        dc.DrawBitmap(m_seleced_bmp, iconX, iconY);//画料槽

        wxFont font(FromDIP(6), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
        dc.SetFont(font);
        auto pair = compute_fore_color(m_material_info.m_color);
        dc.SetTextForeground(pair.first);
        render_name(m_material_info.m_name, dc);
        draw_edit_bmp(dc, *pair.second, m_edit_pos);
        break;
    }
    case MaterialSlot::Unknown: {
        int iconX = (w - m_unknow_bmp.GetWidth()) / 2;
        int iconY = (h - m_unknow_bmp.GetHeight()) / 2;
        dc.DrawBitmap(m_unknow_bmp, iconX, iconY);
        int    name_x = FromDIP(32);
        int    name_y = FromDIP(16);
        dc.DrawBitmap(m_unknow_name_bmp.bmp(), name_x, name_y); // 画名字
        draw_edit_bmp(dc, m_edit_black_bmp, m_edit_pos);
        break;
    }
    case MaterialSlot::Empty: {
        int iconX = (w - m_empty_bmp.GetWidth()) / 2;
        int iconY = (h - m_empty_bmp.GetHeight()) / 2;
        dc.DrawBitmap(m_empty_bmp, iconX, iconY);
        break;
    }
    default: break;
    }
    
}

void MaterialSlot::draw_edit_bmp(wxPaintDC& dc, wxBitmap& bitmap, wxPoint& point)
{
    if (!m_is_editable) {
        return;
    }
    switch (m_edit_state) {
    case MaterialSlot::Normal: {
        dc.DrawBitmap(bitmap, point);
        break;
    }
    case MaterialSlot::Hover: {
        dc.DrawBitmap(m_edit_hover_bmp, point);
        break;
    }
    case MaterialSlot::Press: {
        dc.DrawBitmap(m_edit_press_bmp, point);
        break;
    }
    default: break;
    }
}

void MaterialSlot::render_name(const wxString& name, wxPaintDC& dc)
{
    dc.SetFont(::Label::Body_9);
    int name_symmetry_x = m_edit_pos.x + m_edit_size.GetWidth() / 2;
    if (name.size() <= 4) {
        int name_x = name_symmetry_x - name.size() * FromDIP(6) / 2;
        int name_y = FromDIP(18);
        dc.DrawText(name, name_x, name_y); // 画名字
    } else {
        size_t   p        = name.find('-');
        size_t   pos      = (p < 4) ? p : 4;
        wxString name1 = name.substr(0, pos);
        wxString name2 = name.substr(pos, name.size() - 1);
        int      name1_x = name_symmetry_x - name1.size() * FromDIP(6) / 2;
        int      name2_x = name_symmetry_x - name2.size() * FromDIP(6) / 2;
        int      name1_y  = FromDIP(12);
        int      name2_y  = FromDIP(22);
        dc.DrawText(name1, name1_x, name1_y); 
        dc.DrawText(name2, name2_x, name2_y); 
    }
}

std::pair<wxColour, wxBitmap*> MaterialSlot::compute_fore_color(const wxColour& color)
{
    std::pair<wxColour, wxBitmap*> pair;
    unsigned char ave_rgb = (color.Red() + color.Green() + color.Blue()) / 3;
    if (ave_rgb < 128) {
        pair.first = wxColour(255, 255, 255);
        pair.second = &m_edit_white_bmp;
        
    } else {
        pair.first  = wxColour(51, 51, 51);
        pair.second = &m_edit_black_bmp;
    }
    return pair;
}

bool MaterialSlot::in_edit_scope(const wxPoint& pos)
{
    if (!m_is_editable) {
        return false;
    }
    return (pos.x >= m_edit_pos.x && pos.x <= m_edit_pos.x + m_edit_size.GetWidth() && pos.y >= m_edit_pos.y &&
            pos.y <= m_edit_pos.y + m_edit_size.GetHeight());
}

bool MaterialSlot::get_user_choices()
{
    bool update_info = false;
    // 确定对话框弹出位置
    wxPoint        pos(GetScreenPosition().x + FromDIP(91), GetScreenPosition().y - FromDIP(47)); // 预计弹出位置
    wxSize         dialog_size(FromDIP(422), FromDIP(224));
    wxPoint        finally_pos = MaterialDialog::calculate_pop_position(pos, dialog_size);
    int            state       = 0;
    if (m_type == SlotType::Complete) {
        state = MaterialDialog::InfoState::NameKnown | MaterialDialog::InfoState::ColorKnown;
    }
    MaterialDialog material_dialog(this, wxID_ANY, wxEmptyString, state, finally_pos, dialog_size);
    material_dialog.init_comboBox();
    if (m_material_info.m_color.IsOk()) {
        material_dialog.set_material_color(m_material_info.m_color);
    }
    if (!m_material_info.m_name.empty()) {
        material_dialog.set_material_name(m_material_info.m_name);
    }
    if (material_dialog.ShowModal() == wxID_OK) {
        m_material_info.m_name = material_dialog.get_material_name();
        m_material_info.m_color = material_dialog.get_material_color();
        update_info             = true;
    }
    if (!m_material_info.m_name.empty() && m_material_info.m_color.IsOk()) {
        set_slot_type(SlotType::Complete);
    }
    Refresh();
    return update_info;
}

SlotNumber::SlotNumber(wxWindow*       parent,
                       wxWindowID      id,
                       const wxString& number,
                       const wxPoint&  pos,
                       const wxSize&   size,
                       long            style,
                       const wxString& name)
    : wxWindow(parent, id, pos, size, style, name) 
    , m_number(number), m_mode(PaintMode::Normal), m_selected(false)
{
    SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    SetBackgroundColour(wxColour(255, 255, 255));
    Bind(wxEVT_PAINT, &SlotNumber::paintEvent, this);
}


SlotNumber::~SlotNumber() {}

void SlotNumber::set_paint_mode(PaintMode mode)
{
    m_mode = mode;
    Refresh();
}

void SlotNumber::set_selected(bool selected)
{
    m_selected = selected;
    Refresh();
}

void SlotNumber::set_number(int number)
{
    m_number = wxString::Format(wxT("%i"), number);
    Refresh();
}

void SlotNumber::paintEvent(wxPaintEvent& event)
{
    // 绘制序号椭圆
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    auto      w = GetSize().GetWidth()- 1;
    auto      h = GetSize().GetHeight()- 1;
    wxColour  curr_color;
    switch (m_mode) {
    case SlotNumber::Normal: {
        if (m_selected) {
            curr_color = wxColour(50, 141, 251);
        } else {
            curr_color = wxColour(221, 221, 221);
        }
        break;
    }
    case SlotNumber::Hover: {
        curr_color = wxColour(149, 197, 255);
        break;
    }
    case SlotNumber::Press: {
        curr_color = wxColour(17, 111, 223);
        break;
    }
    default: break;
    }
    
    gc->SetBrush(wxBrush(curr_color));
    gc->SetPen(wxPen(curr_color, 0));
    gc->DrawEllipse(0, 0, w , h);
    // 绘制序号文本
    int    textX = (GetSize().GetWidth() - FromDIP(7)) / 2;
    int    textY = (GetSize().GetHeight() - FromDIP(16)) / 2;
    dc.SetTextForeground(wxColour(255, 255, 255));
    dc.DrawText(m_number, textX, textY);
}

ProgressNumber::ProgressNumber(
    wxWindow* parent, wxWindowID id, const int number, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_process_num(this, std::string("progress_num_") + std::to_string(number),15)
    , m_not_process_num(this, std::string("unprogress_num_") + std::to_string(number), 15)
    , m_succeed(this, "success_btn", 15)
    , m_mode(PaintMode::NotProcess)
{
    SetMinSize(wxSize(FromDIP(15), FromDIP(15)));
    SetBackgroundColour(wxColour(255, 255, 255));
    Bind(wxEVT_PAINT, &ProgressNumber::paintEvent, this);
}

ProgressNumber::~ProgressNumber() {}

void ProgressNumber::set_state(PaintMode mode){
    m_mode = mode;
    Refresh();
}

void ProgressNumber::paintEvent(wxPaintEvent& event)
{
    // 绘制序号椭圆
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    auto      w = GetSize().GetWidth();
    auto      h = GetSize().GetHeight();
    switch (m_mode) {
    case ProgressNumber::Processing: {
        int iconW = m_process_num.GetBmpWidth();
        int iconH = m_process_num.GetBmpHeight();
        int iconX = (w - iconW) / 2;
        int iconY = (h - iconH) / 2;
        gc->DrawBitmap(m_process_num.bmp(), iconX, iconY, iconW, iconH);
        break;
    }
    case ProgressNumber::NotProcess: {
        int iconW = m_not_process_num.GetBmpWidth();
        int iconH = m_not_process_num.GetBmpHeight();
        int iconX = (w - iconW) / 2;
        int iconY = (h - iconH) / 2;
        gc->DrawBitmap(m_not_process_num.bmp(), iconX, iconY, iconW, iconH);
        break;
    }
    case ProgressNumber::Succeed: {
        int iconW = m_succeed.GetBmpWidth();
        int iconH = m_succeed.GetBmpHeight();
        int iconX = (w - iconW) / 2;
        int iconY = (h - iconH) / 2;
        gc->DrawBitmap(m_succeed.bmp(), iconX, iconY, iconW, iconH);
        return;
    }
    default: break;
    }
   
}

MaterialSlotWgt::MaterialSlotWgt(wxWindow*       parent,
                                 wxWindowID      id,
                                 const int number,
                                 const wxPoint&  pos,
                                 const wxSize&   size,
                                 long            style,
                                 const wxString& name) 
    : wxWindow(parent, id, pos, size, style, name), m_slot_ID(number), m_cur_id(ComInvalidId)
{ 
    SetMinSize(wxSize(FromDIP(62), FromDIP(80)));
    SetBackgroundColour(wxColour(255, 255, 255));
    setup_layout(this, number);
    connectEvent();
}

MaterialSlotWgt::~MaterialSlotWgt() {}

void MaterialSlotWgt::set_selected(bool selected) { m_number->set_selected(selected); }

void MaterialSlotWgt::set_slot_ID(int number)
{
    m_slot_ID = number;
    m_number->set_number(m_slot_ID);
}

MaterialInfo MaterialSlotWgt::get_material_info() { return m_material_slot->get_material_info(); }

int MaterialSlotWgt::get_slot_ID() { return m_slot_ID; }

void MaterialSlotWgt::set_material_info(MaterialInfo& info) { m_material_slot->set_material_info(info); }

void MaterialSlotWgt::set_slot_type(MaterialSlot::SlotType slot_type) { m_material_slot->set_slot_type(slot_type); }

void MaterialSlotWgt::set_conn_point(const wxPoint& point) { m_conn_point = point; }

wxPoint MaterialSlotWgt::get_conn_point() { return m_conn_point; }

void MaterialSlotWgt::set_slot_wgt_type(SlotWgtType type) { m_slot_wgt_type = type; }

void MaterialSlotWgt::set_edit_enable(bool enable) { m_material_slot->set_edit_enable(enable); }

void MaterialSlotWgt::setCurId(int curId) { m_cur_id = curId; }

bool MaterialSlotWgt::send_config_command() 
{ 
    std::string name(m_material_slot->get_material_info().m_name.c_str());
    std::string color_str( m_material_slot->get_material_info().m_color.GetAsString(wxC2S_HTML_SYNTAX).c_str());
    ComCommand* comCommand = nullptr;
    switch (m_slot_wgt_type) {
    case MaterialSlotWgt::MaterialStation: {
        comCommand = new ComMatlStationConfig(m_slot_ID, name, color_str);
        break;
    }
    case MaterialSlotWgt::IndependentMatl: {
        comCommand = new ComIndepMatlConfig(name, color_str);
        break;
    }
    default: break;
    }
    return Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comCommand);
}

bool MaterialSlotWgt::start_supply_wire()
{
    MaterialSlot::SlotType type = m_material_slot->get_slot_type();
    switch (type) {
    case MaterialSlot::SlotType::Complete: {
        // 发出进丝命令
        ComCommand* comCommand = nullptr;
        if (m_slot_wgt_type == SlotWgtType::MaterialStation) {
            comCommand  = new ComMatlStationCtrl(m_slot_ID, ComAction::SupplyWire);
        } else {
            comCommand = new ComIndepMatlCtrl(ComAction::SupplyWire);
        }
        return Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comCommand);
    }
    case MaterialSlot::SlotType::Unknown: {
        if (m_material_slot->get_user_choices()) {
            send_config_command();
        }
        return false;
    }
    case MaterialSlot::SlotType::Empty: 
    default: {
        return false;
    }
    }

}

bool MaterialSlotWgt::start_withdrawn_wire()
{
    MaterialSlot::SlotType type = m_material_slot->get_slot_type();
    switch (type) {
    case MaterialSlot::SlotType::Complete: {
        // 发出退丝命令
        ComCommand* comCommand = nullptr;
        if (m_slot_wgt_type == SlotWgtType::MaterialStation) {
            comCommand = new ComMatlStationCtrl(m_slot_ID, ComAction::WithdrawnWire);
        } else {
            comCommand = new ComIndepMatlCtrl(ComAction::WithdrawnWire);
        }
        return Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comCommand);
    }
    case MaterialSlot::SlotType::Unknown: {
        if (m_material_slot->get_user_choices()) {
            send_config_command();
        }
        return false;
    }
    case MaterialSlot::SlotType::Empty:
    default: {
        return false;
    }
    }

}

bool MaterialSlotWgt::cancel_operation()
{
    ComCommand* comCommand = nullptr;
    if (m_slot_wgt_type == SlotWgtType::MaterialStation) {
        comCommand = new ComMatlStationCtrl(m_slot_ID, ComAction::CancelAction);
    } else {
        comCommand = new ComIndepMatlCtrl(ComAction::CancelAction);
    }
    return Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comCommand);
}

void MaterialSlotWgt::setup_layout(wxWindow* parent, const int& number)
{ 
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL); 
    m_number = new SlotNumber(parent, wxID_ANY, wxString::Format(wxT("%i"), number), wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18))); 
    m_material_slot        = new MaterialSlot(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(62), FromDIP(62)));
    sizer->Add(m_number, 0, wxLEFT | wxRIGHT, (GetSize().GetWidth() - m_number->GetSize().GetWidth()) / 2);
    sizer->AddSpacer(FromDIP(2));
    sizer->Add(m_material_slot, 0, wxLEFT | wxRIGHT, 0);
    SetSizer(sizer);
    Layout();
    sizer->Fit(this);
}

void MaterialSlotWgt::connectEvent()
{
    m_material_slot->Bind(wxEVT_ENTER_WINDOW, &MaterialSlotWgt::OnMouseEnter, this);
    m_material_slot->Bind(wxEVT_LEAVE_WINDOW, &MaterialSlotWgt::OnMouseLeave, this);
    m_material_slot->Bind(wxEVT_LEFT_DOWN, &MaterialSlotWgt::OnMouseDown, this);
    m_material_slot->Bind(wxEVT_LEFT_UP, &MaterialSlotWgt::OnMouseUp, this);
    m_material_slot->Bind(wxEVT_LEFT_DCLICK, &MaterialSlotWgt::OnMouseDclick, this);
    m_material_slot->Bind(wxEVT_MOTION, &MaterialSlotWgt::OnMouseMove, this);
}

void MaterialSlotWgt::OnMouseDown(wxMouseEvent& event) 
{ 
    if (m_material_slot->get_slot_type() == MaterialSlot::SlotType::Empty)
        return;
    m_number->set_paint_mode(SlotNumber::Press); 
    if (m_material_slot->in_edit_scope(event.GetPosition())) {
        m_material_slot->set_edit_state(MaterialSlot::Press);
    }
}

void MaterialSlotWgt::OnMouseUp(wxMouseEvent& event)
{
    if (m_material_slot->get_slot_type() == MaterialSlot::SlotType::Empty)
        return;
    m_number->set_paint_mode(SlotNumber::Normal);
    wxCommandEvent click_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    ProcessWindowEvent(click_event);
    if (m_material_slot->in_edit_scope(event.GetPosition())) {
        m_material_slot->set_edit_state(MaterialSlot::Normal);
        if (m_material_slot->get_user_choices()) {
            send_config_command();
        }
    } 
}

void MaterialSlotWgt::OnMouseEnter(wxMouseEvent& event)
{
    if (m_material_slot->get_slot_type() == MaterialSlot::SlotType::Empty)
        return;
    m_number->set_paint_mode(SlotNumber::Hover);
}

void MaterialSlotWgt::OnMouseLeave(wxMouseEvent& event)
{
    if (m_material_slot->get_slot_type() == MaterialSlot::SlotType::Empty)
        return;
    m_number->set_paint_mode(SlotNumber::Normal);
    
}

void MaterialSlotWgt::OnMouseDclick(wxMouseEvent& event)
{
    bool slot_empty = m_material_slot->get_slot_type() == MaterialSlot::SlotType::Empty;
    if (slot_empty || !m_material_slot->is_edit_enable())
        return;
    m_number->set_paint_mode(SlotNumber::Normal);
    wxCommandEvent click_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    ProcessWindowEvent(click_event);
    if (m_material_slot->get_user_choices()) {
        send_config_command();
    }
}

void MaterialSlotWgt::OnMouseMove(wxMouseEvent& event)
{
    if (m_material_slot->in_edit_scope(event.GetPosition())) {
        m_material_slot->set_edit_state(MaterialSlot::Hover);
    } else {
        m_material_slot->set_edit_state(MaterialSlot::Normal);
    }
}

Nozzle::Nozzle(wxWindow*       parent,
               wxWindowID      id,
               const wxPoint&  pos,
               const wxSize&   size,
               long            style,
               const wxString& name)
    : wxWindow(parent, id, pos, size, style, name), m_bitmap(this, "nozzle_with_wire", 22), m_wire_color(wxColour(255, 0 ,0))
{
    SetBackgroundColour(wxColour(255, 255, 255));
    SetMinSize(wxSize(FromDIP(30), FromDIP(22)));
    Bind(wxEVT_PAINT, &Nozzle::paintEvent, this);
}

Nozzle::~Nozzle() {}

void Nozzle::set_wite_color(const wxColour& color)
{
    m_wire_color = color;
    Refresh();
}

void Nozzle::set_conn_point(const wxPoint& point) { m_conn_point = point; }

wxPoint Nozzle::get_conn_point() { return m_conn_point; }

void Nozzle::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    // 绘制图标
    int bmp_w = m_bitmap.GetBmpWidth();
    int bmp_h = m_bitmap.GetBmpHeight();
    int iconX = (GetSize().GetWidth() - bmp_w) / 2;
    int iconY = (GetSize().GetHeight() - bmp_h) / 2;
    dc.SetBrush(wxBrush(m_wire_color));
    dc.SetPen(wxPen(m_wire_color, 0));
    dc.DrawRectangle(iconX, iconY, bmp_w, bmp_h);
    dc.DrawBitmap(m_bitmap.bmp(), iconX, iconY);
}

TipsArea::TipsArea(wxWindow*       parent,
                   wxWindowID      id,
                   const wxPoint&  pos,
                   const wxSize&   size,
                   long            style,
                   const wxString& name) 
    : wxWindow(parent, id, pos, size, style, name), m_state(TipsAreaState::Free), m_hasMatlStation(1)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    prepare_layout(this);
    switch_layout_state(TipsAreaState::Free);
    m_progress->set_state_action(ProgressArea::StateAction::Free);
    m_progress->set_state_step(ProgressArea::StateStep::NoProcessed);
    connectEvent();
}

TipsArea::~TipsArea() {}

void TipsArea::switch_layout_state(TipsAreaState state) 
{
    m_state = state;
    switch (m_state) {
    case TipsArea::TipsAreaState::Printing:
    case TipsArea::TipsAreaState::PrintingPaused:
    case TipsArea::TipsAreaState::Busy:
    case TipsArea::TipsAreaState::Free: {
        m_tips_area_title->SetLabel(_L("Tips"));
        const wxString tips_text("Select a slot, and click the \"Load\" or \n\"Unload\" button to load or unload \nfilament.");
        m_tips_text->SetLabel(_L(tips_text));
        layout_tips_info();
        break;
    }
    case TipsArea::TipsAreaState::SupplyWire: {
        m_tips_area_title->SetLabel(_CTX("Load", "filament"));
        layout_progress_status();
        break;
    }
    case TipsArea::TipsAreaState::WithdrawnWire: {
        m_tips_area_title->SetLabel(_CTX("Unload", "filament"));
        layout_progress_status();
        break;
    }
    case TipsArea::TipsAreaState::Canceling:
    default: break;
    }
}

void TipsArea::Synchronize_printer_status(const com_dev_data_t& data) 
{
    m_hasMatlStation = data.devDetail->hasMatlStation;
    if (m_hasMatlStation) {
        //表示打印机接有四色材料站
        m_stateAction = data.devDetail->matlStationInfo.stateAction;
        m_stateStep   = data.devDetail->matlStationInfo.stateStep;
    } else {
        // 表示打印机未接有四色材料站
        m_stateAction = data.devDetail->indepMatlInfo.stateAction;
        m_stateStep   = data.devDetail->indepMatlInfo.stateStep;
    }
    
    m_state = static_cast<TipsAreaState>(m_stateAction);
    auto pro_action = static_cast<ProgressArea::StateAction>(m_stateAction);
    auto pro_step   = static_cast<ProgressArea::StateStep>(m_stateStep);
    switch_layout_state(m_state);
    m_progress->set_state_action(pro_action);
    m_progress->set_state_step(pro_step);

}

void TipsArea::reset_printer_status()
{
    switch_layout_state(TipsAreaState::Free);
    m_progress->set_state_action(ProgressArea::StateAction::Free);
    m_progress->set_state_step(ProgressArea::StateStep::NoProcessed);
}

TipsArea::TipsAreaState TipsArea::get_tips_area_state() { return m_state; }


void TipsArea::set_cancel_enable(bool enable) { m_progress->set_cancel_enable(enable); }

bool TipsArea::is_heating() { return m_progress->is_heating(); }

void TipsArea::connectEvent() { Bind(wxEVT_COMMAND_BUTTON_CLICKED, &TipsArea::on_cancel_clicked,this, m_progress->GetId()); }

void TipsArea::on_cancel_clicked(wxCommandEvent& event)
{
    wxCommandEvent cancel_clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId()); // 通知父窗口取消被按下
    ProcessWindowEvent(cancel_clicked_event);
}

void TipsArea::prepare_layout(wxWindow* parent)
{
    //控件的创建
    m_tips_area_title = new wxStaticText(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(254), FromDIP(18)), wxALIGN_LEFT);
    m_tips_area_title->SetForegroundColour(wxColour(50, 141, 251));
    m_tips_text = new wxStaticText(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(254), FromDIP(120)), wxALIGN_LEFT);
    m_tips_text->SetFont(::Label::Body_14);
    m_progress  = new ProgressArea(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(254), FromDIP(120)));

}

void TipsArea::layout_tips_info()
{
    // 布局提示及文本控件
    wxBoxSizer* tips_sizer = new wxBoxSizer(wxVERTICAL);
    tips_sizer->AddSpacer(FromDIP(42));
    tips_sizer->Add(m_tips_area_title, 0, wxLEFT | wxRIGHT, FromDIP(23));
    tips_sizer->Add(m_tips_text, 0, wxLEFT | wxRIGHT, FromDIP(23));
    tips_sizer->AddStretchSpacer();
    m_progress->Hide();
    m_tips_text->Show();
    SetSizer(tips_sizer);
    Layout();
}

void TipsArea::layout_progress_status()
{
    // 布局进度信息控件
    wxBoxSizer* progress_sizer = new wxBoxSizer(wxVERTICAL);
    progress_sizer->AddSpacer(FromDIP(42));
    progress_sizer->Add(m_tips_area_title, 0, wxLEFT | wxRIGHT, FromDIP(23));
    progress_sizer->AddSpacer(FromDIP(6));
    progress_sizer->Add(m_progress, 0, wxLEFT | wxRIGHT, FromDIP(23));
    progress_sizer->AddStretchSpacer();
    m_tips_text->Hide();
    m_progress->Show();
    SetSizer(progress_sizer);
    Layout();
}


LineArea::LineArea(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name) 
    : wxWindow(parent, id, pos, size, style, name)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    Bind(wxEVT_PAINT, &LineArea::paintEvent, this);
}

LineArea::~LineArea() {}

void LineArea::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    // 设置画笔颜色和样式
    wxPen pen(wxColour(102, 102, 102), 2);
    dc.SetPen(pen);
    int     width  = GetSize().GetWidth();
    int     height = GetSize().GetHeight();
    // 绘制直线
    dc.DrawLine(width / 2, 0, width / 2, height);
}


ProgressArea::ProgressArea(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_state_action(StateAction::Free)
    , m_state_step(StateStep::NoProcessed)
{
    setup_layout(this);
    connectEvent();
}

ProgressArea::~ProgressArea() {}


void ProgressArea::set_state_action(StateAction action) 
{ //该函数只改变不同任务文本内容
    m_state_action = action; 
    switch (m_state_action) {
    case ProgressArea::StateAction::Printing:
    case ProgressArea::StateAction::PrintingPaused:
    case ProgressArea::StateAction::Busy:
    case ProgressArea::StateAction::Free: {
        for (int i = 0; i < 4; ++i) {
            m_txt_group[i]->SetLabelText(wxEmptyString);
        }
        break;
    }
    case ProgressArea::StateAction::SupplyWire: {
        for (int i = 0; i < 4; ++i) {
            m_txt_group[i]->SetLabelText(m_supply_step[i]);
        }
        break;
    }
    case ProgressArea::StateAction::WithdrawnWire: {
        for (int i = 0; i < 4; ++i) {;
            m_txt_group[i]->SetLabelText(m_withdrawn_step[i]);
        }
        break;
    }
    case ProgressArea::StateAction::Canceling:
    default: break;
    }
}

void ProgressArea::set_state_step(StateStep step) 
{
    m_state_step = step;
    wxColour dark_txt(51, 51, 51);
    wxColour light_txt(221, 221, 221);
    wxColour blue_txt(50, 141, 251);
    switch (m_state_step) {
    case ProgressArea::StateStep::Heating: {
        m_btn_group[0]->set_state(ProgressNumber::Processing);
        m_txt_group[0]->SetForegroundColour(dark_txt);
        for (int i = 1 ; i < 4; ++i){
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    case ProgressArea::StateStep::PushMaterials: 
    case ProgressArea::StateStep::CutOffMaterials: {
        m_btn_group[0]->set_state(ProgressNumber::Succeed);
        m_txt_group[0]->SetForegroundColour(blue_txt);
        m_btn_group[1]->set_state(ProgressNumber::Processing);
        m_txt_group[1]->SetForegroundColour(dark_txt);
        for (int i = 2; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    case ProgressArea::StateStep::WashOldMaterials: 
    case ProgressArea::StateStep::PullBackMaterials: {
        for (int i = 0; i < 2; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(blue_txt);
        }
        m_btn_group[2]->set_state(ProgressNumber::Processing);
        m_txt_group[2]->SetForegroundColour(dark_txt);
        m_btn_group[3]->set_state(ProgressNumber::NotProcess);
        m_txt_group[3]->SetForegroundColour(light_txt);
        break;
    }
    case ProgressArea::StateStep::Finish: {
        for (int i = 0; i < 3; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(dark_txt);
        }
        m_btn_group[3]->set_state(ProgressNumber::Processing);
        m_txt_group[3]->SetForegroundColour(blue_txt);
        for (int i = 0; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(blue_txt);
        }
        break;
    }
    case ProgressArea::StateStep::NoProcessed: {
        for (int i = 0; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    default: break;
    }
}

void ProgressArea::set_cancel_enable(bool enable) { m_cancel_btn->Enable(enable); }

bool ProgressArea::is_heating() 
{
    return m_state_step == StateStep::Heating && 
           (m_state_action == StateAction::SupplyWire || m_state_action == StateAction::WithdrawnWire); 
}

void ProgressArea::setup_layout(wxWindow* parent)
{
    int         width          = GetSize().GetWidth();
    int         height         = GetSize().GetHeight();
    wxBoxSizer* progress_sizer = new wxBoxSizer(wxHORIZONTAL);
    //序号按钮区布局
    wxBoxSizer* num_btn_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   num_btn_area  = new LineArea(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(18), height));
    m_btn_group.reserve(4);
    for (int i = 0; i < 4; ++i) {
        ProgressNumber* col_btn = new ProgressNumber(num_btn_area, wxID_ANY, i + 1, wxDefaultPosition,
                                               wxSize(FromDIP(18), FromDIP(18)));
        m_btn_group.push_back(col_btn);
        num_btn_sizer->Add(col_btn, 0, wxLEFT | wxRIGHT, 0);
        if (i < 3) { 
            num_btn_sizer->AddStretchSpacer();
        }
    }
    num_btn_area->SetSizer(num_btn_sizer);
    num_btn_area->Layout();
    //文本区布局

    wxBoxSizer* txt_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   txt_area  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(196), height));
    txt_area->SetBackgroundColour(wxColour(255, 255, 255));
    m_txt_group.reserve(4);
    for (int i = 0; i < 4; ++i){
        wxStaticText* txt = new wxStaticText(txt_area, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(196), FromDIP(16)),
                                               wxALIGN_LEFT);
        m_txt_group.push_back(txt);
        txt_sizer->Add(txt, 0, wxLEFT, FromDIP(6));
        if (i < 3) {
            txt_sizer->AddStretchSpacer();
        }
    }
    txt_area->SetSizer(txt_sizer);
    txt_area->Layout();

    //取消按钮区
    wxBoxSizer* cancel_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   cancel_area  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(40), height));
    cancel_area->SetBackgroundColour(wxColour(255, 255, 255));
    m_cancel_btn = new RoundedButton(cancel_area, wxID_ANY, false, _L("Cancel"), wxDefaultPosition,
                                                            wxSize(FromDIP(60), FromDIP(24)));
    int    txt_len = m_cancel_btn->GetLabel().Length();
    wxFont font;
    if (txt_len <= 2) {
        font = ::Label::Body_14;
    } else if (txt_len > 2 && txt_len <= 6) {
        font = ::Label::Body_12;
    } else if (txt_len > 6 && txt_len <= 10) {
        font = ::Label::Body_10;
    }
    m_cancel_btn->SetFont(font);
    m_cancel_btn->set_state_color(wxColour(50, 141, 251), RoundedButton::Normal);
    m_cancel_btn->set_state_color(wxColour(149, 197, 255), RoundedButton::Hovered);
    m_cancel_btn->set_state_color(wxColour(17, 111, 223), RoundedButton::Pressed);
    m_cancel_btn->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_cancel_btn->set_radius(4);
    m_cancel_btn->SetForegroundColour(wxColour(50, 141, 251));

    cancel_sizer->AddStretchSpacer();
    cancel_sizer->Add(m_cancel_btn, 0, wxLEFT | wxRIGHT, 0);
    cancel_area->SetSizer(cancel_sizer);
    cancel_area->Layout();

    //整体布局
    progress_sizer->Add(num_btn_area, 0, wxLEFT | wxRIGHT, 0);
    progress_sizer->Add(txt_area, 0, wxLEFT | wxRIGHT, 0);
    progress_sizer->Add(cancel_area, 0, wxLEFT | wxRIGHT, 0);
    progress_sizer->AddStretchSpacer();
    SetSizer(progress_sizer);
    Layout();
}

void ProgressArea::connectEvent()
{
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &ProgressArea::on_cancel_clicked, this, m_cancel_btn->GetId());
}

void ProgressArea::on_cancel_clicked(wxCommandEvent& event)
{
    wxCommandEvent cancel_clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId()); //通知父窗口取消被按下
    ProcessWindowEvent(cancel_clicked_event);
}

MaterialSlotArea::MaterialSlotArea(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_radio_slot(nullptr)
    , m_printer_type(OTHER)
    , m_hasMatlStation(1)
    , m_nozzle_has_wire(1)
    , m_currentLoadSlot(-1)
    , m_currentSlot(0)
    , m_radio_changeable(true)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    prepare_layout(this);
    setup_layout_four(this);
    connectEvent();
    s_self = this;
}

MaterialSlotArea::~MaterialSlotArea() { }

MaterialSlotArea* MaterialSlotArea::get_inst() { return s_self; }

void MaterialSlotArea::change_layout_mode(LayoutMode layout_model)
{
    if (m_layout_mode == layout_model)
        return;
    m_layout_mode = layout_model;
    switch (m_layout_mode) {
    case LayoutMode::One: {
        setup_layout_one(this);
        break;
    }
    case LayoutMode::Four: {
        setup_layout_four(this);
        break;
    }
    default: break;
    }
}

MaterialSlotWgt* MaterialSlotArea::get_radio_slot() { return m_radio_slot; }

FFPrinterPid MaterialSlotArea::get_printer_type() { return m_printer_type; }

void MaterialSlotArea::abandon_selected()
{
    if (!m_radio_changeable)
        return;
    if (m_radio_slot) {
        m_radio_slot->set_selected(false);
        m_radio_slot = nullptr;
    }
}

std::vector<wxColour> MaterialSlotArea::get_all_material_color() 
{ 
    std::vector<wxColour> color_all;
    color_all.reserve((*m_curr_slot_contaier).size());
    for (auto& slot : (*m_curr_slot_contaier)) {
        if (slot->get_material_info().m_color.IsOk()) {
            color_all.push_back(slot->get_material_info().m_color);
        }
    }
    return color_all;
}


void MaterialSlotArea::setCurId(int curId) 
{
    for (auto& slot : m_material_slots_four) {
        slot->set_slot_type(MaterialSlot::Empty);
        slot->setCurId(curId);
    }
    for (auto& slot : m_material_slot_one) {
        slot->set_slot_type(MaterialSlot::Empty);
        slot->setCurId(curId);
    }
    m_currentLoadSlot = -1;
    if (m_radio_slot) {
        m_radio_slot->set_selected(false);
        m_radio_slot = nullptr;
    }
}

void MaterialSlotArea::set_radio_changeable(bool enable) 
{ 
    m_radio_changeable = enable;
    if (!m_radio_changeable) {
        //如果打印机正忙，料槽不可改选，radio料槽应与currslot同步
        if (m_radio_slot) {
            m_radio_slot->set_selected(false);
        }
        if (m_hasMatlStation) {
            m_radio_slot = m_material_slots_four[m_currentSlot];
        } else {
            m_radio_slot = m_material_slot_one[0];
        }
        m_radio_slot->set_selected(true); 

    }
}

bool MaterialSlotArea::is_executive_slot(MaterialSlotWgt* slot) 
{ 
    if (!slot)
        return false;
    switch (m_printer_type) {
    case AD5X: {
        if (m_hasMatlStation) {
            return slot != m_material_slot_one.front();
        } else {
            return slot == m_material_slot_one.front();
        }
        break;
    }
    case GUIDER_4:
    case GUIDER_4_PRO: {
        if (m_hasMatlStation) {
            return true;
        }
        else {
            return slot == m_material_slot_one.front();
        }
        break;
    }
    default: {
        return false;
        break;
    }
    }
    
}

bool MaterialSlotArea::is_supply_wire_slot(MaterialSlotWgt* slot) 
{ 
    if (!slot || !m_nozzle_has_wire)
        return false;
    if (m_hasMatlStation) {
        return slot == m_material_slots_four[m_currentSlot];
    } else {
        return slot == m_material_slot_one[0];
    } 
}

bool MaterialSlotArea::is_current_slot(MaterialSlotWgt* slot)
{
    if (!slot)
        return false;
    if (m_hasMatlStation) {
        return slot == m_material_slots_four[m_currentSlot];
    } else {
        return slot == m_material_slot_one.front();
    }
}

void MaterialSlotArea::set_slot_edit_enable(bool enable) 
{
    for (auto& slot : m_material_slots_four) {
        slot->set_edit_enable(enable);
    }
    for (auto& slot : m_material_slot_one) {
        slot->set_edit_enable(enable);
    }
}

void MaterialSlotArea::synchronize_printer_status(const com_dev_data_t& data) 
{   
    m_hasMatlStation = data.devDetail->hasMatlStation;
    synchronize_matl_station(data);
    synchronize_indep_matl(data);
    unsigned short curr_pid = 0;
    if (data.connectMode == 0) {
        curr_pid            = data.lanDevInfo.pid;
    } else if (data.connectMode == 1) {
        curr_pid            = data.devDetail->pid;
    }
    if (FFUtils::printer_preset_map.find(curr_pid) != FFUtils::printer_preset_map.end()) {
        MaterialStation::set_printer_type((FFPrinterPid)curr_pid);
        m_printer_type = (FFPrinterPid)curr_pid;
    }

    // 同步喷嘴传感器的状态
    m_nozzle_has_wire = data.devDetail->hasRightFilament;
    // 同步有色料线的连接的状态
    m_currentLoadSlot = data.devDetail->matlStationInfo.currentLoadSlot - 1;
    if (m_currentLoadSlot < -1 || m_currentLoadSlot > 3) {
        m_currentLoadSlot = -1;
    }
    if (m_hasMatlStation) {
        // 表示打印机接有四色材料站
        m_state_action = (StateAction) data.devDetail->matlStationInfo.stateAction;
        m_state_step   = (StateStep) data.devDetail->matlStationInfo.stateStep;
    } else {
        // 表示打印机未接有四色材料站
        m_state_action = (StateAction) data.devDetail->indepMatlInfo.stateAction;
        m_state_step   = (StateStep) data.devDetail->indepMatlInfo.stateStep;
    }
    switch (m_state_action) {
    case MaterialSlotArea::StateAction::Free: {
        set_radio_changeable(true);
        set_slot_edit_enable(true);
        break;
    }
    case MaterialSlotArea::StateAction::SupplyWire:
    case MaterialSlotArea::StateAction::WithdrawnWire: 
    case MaterialSlotArea::StateAction::Canceling: 
    case MaterialSlotArea::StateAction::Printing: 
    case MaterialSlotArea::StateAction::Busy: {
        set_radio_changeable(false);
        set_slot_edit_enable(false);
        break;
    }
    case MaterialSlotArea::StateAction::PrintingPaused: {
        set_radio_changeable(true);
        set_slot_edit_enable(false);
        break;
    }
    default: break;
    }
    if (m_hasMatlStation) {
        change_layout_mode(LayoutMode::Four);
    }else {
        change_layout_mode(LayoutMode::One);
    }
    Refresh();
}

void MaterialSlotArea::synchronize_matl_station(const com_dev_data_t& data) 
{
    //同步四色料盘的状态
    int                    slot_cnt  = data.devDetail->matlStationInfo.slotCnt;
    fnet_matl_slot_info_t* slotInfos = data.devDetail->matlStationInfo.slotInfos;
    if (!slotInfos) {
        return;
    }
    for (int i = 0; i < slot_cnt; ++i) {
        int   slotId        = (slotInfos + i)->slotId;
        int   hasFilament   = (slotInfos + i)->hasFilament; // 1 true, 0 false，四色状态下hasFilament表示料盘是否为空
        wxString materialName  = (slotInfos + i)->materialName;
        wxColour materialColor = (slotInfos + i)->materialColor;
        MaterialSlot::SlotType slot_type;
        MaterialInfo           material_info{wxEmptyString, wxColour()};
        if (hasFilament) {
            if (!materialName.empty() && materialColor.IsOk()) {
                slot_type             = MaterialSlot::SlotType::Complete;
                material_info.m_name  = materialName;
                material_info.m_color = materialColor;
            } else {
                slot_type = MaterialSlot::SlotType::Unknown;
            }
        } else {
            slot_type = MaterialSlot::SlotType::Empty;
            if (m_material_slots_four[i] == m_radio_slot) {
                m_radio_slot = nullptr;
                m_material_slots_four[i]->set_selected(false);
            }
        }
        m_material_slots_four[i]->set_slot_ID(slotId);
        m_material_slots_four[i]->set_slot_type(slot_type);
        m_material_slots_four[i]->set_material_info(material_info);
    }
    int currentSlot  = data.devDetail->matlStationInfo.currentSlot - 1;   //currentSlot是从1开始，m_currentSlot要求从0开始
    m_currentSlot   = (currentSlot < 0 || currentSlot > 3) ? 0 : currentSlot;
}

void MaterialSlotArea::synchronize_indep_matl(const com_dev_data_t& data) 
{
    // 同步外挂料盘的状态
    fnet_indep_matl_info_t& indepMatlInfo = data.devDetail->indepMatlInfo;
    wxString materialName(indepMatlInfo.materialName);
    wxColour materialColor(indepMatlInfo.materialColor);
    MaterialSlot::SlotType  slot_type;
    MaterialInfo            material_info{wxEmptyString, wxColour()};
    if (!materialName.empty() && materialColor.IsOk()) {
        slot_type             = MaterialSlot::SlotType::Complete;
        material_info.m_name  = materialName;
        material_info.m_color = materialColor;
    } else {
        slot_type = MaterialSlot::SlotType::Unknown;
    }
    m_material_slot_one[0]->set_slot_type(slot_type); // 外挂料盘永远不会空
    m_material_slot_one[0]->set_material_info(material_info);
}

bool MaterialSlotArea::start_supply_wire() { return m_radio_slot->start_supply_wire(); }

bool MaterialSlotArea::start_withdrawn_wire() { return m_radio_slot->start_withdrawn_wire(); }

bool MaterialSlotArea::cancel_operation() { return m_radio_slot->cancel_operation(); }

void MaterialSlotArea::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    wxColour  wire_color(221, 221, 221);
    wxColour  nozzle_color(255, 255, 255);
    switch (m_layout_mode) {
    case MaterialSlotArea::One: {
        dc.SetPen(wxPen(wire_color, FromDIP(3)));
        // 先画中间贯通的直线
        wxPoint slot_point   = m_material_slot_one[0]->get_conn_point();
        wxPoint nozzle_point = m_nozzle->get_conn_point();
        int     x1           = slot_point.x;
        int     x2           = nozzle_point.x;
        int     Y            = (slot_point.y + nozzle_point.y) / 2;
        dc.DrawLine(x1, Y, x2, Y);
        // 将料槽与直线相连
        dc.DrawLine(slot_point.x, slot_point.y, slot_point.x, Y);
        // 将喷嘴与直线相连
        dc.DrawLine(nozzle_point.x, nozzle_point.y, nozzle_point.x, Y);
        m_nozzle->set_wite_color(nozzle_color);

        if (!m_hasMatlStation && m_nozzle_has_wire) {
            wxColour curr_color = m_material_slot_one[0]->get_material_info().m_color;
            if (curr_color.IsOk()) {
                wire_color   = curr_color;
                nozzle_color = curr_color;
            }
            dc.SetPen(wxPen(wire_color, FromDIP(1)));
            // 先画中间贯通的直线
            wxPoint slot_point   = m_material_slot_one[0]->get_conn_point();
            wxPoint nozzle_point = m_nozzle->get_conn_point();
            int     x1           = slot_point.x;
            int     x2           = nozzle_point.x;
            int     Y            = (slot_point.y + nozzle_point.y) / 2;
            dc.DrawLine(x1, Y, x2, Y);
            // 将料槽与直线相连
            dc.DrawLine(slot_point.x, slot_point.y, slot_point.x, Y);
            // 将喷嘴与直线相连
            dc.DrawLine(nozzle_point.x, nozzle_point.y, nozzle_point.x, Y);
            m_nozzle->set_wite_color(nozzle_color);
            
        }
        
        break;
    }
    case MaterialSlotArea::Four: {
        // 先画中间贯通的直线
        wxPoint begin_point   = m_material_slots_four.front()->get_conn_point();
        wxPoint end_point    = m_material_slots_four.back()->get_conn_point();
        wxPoint nozzle_point = m_nozzle->get_conn_point();
        int     x1            = begin_point.x;
        int     x2            = end_point.x;
        int     Y             = (begin_point.y + nozzle_point.y) / 2;
        dc.SetPen(wxPen(wire_color, FromDIP(3)));
        dc.DrawLine(x1, Y, x2, Y);
        // 将料槽与直线相连
        for (auto& slot : m_material_slots_four){
            wxPoint slot_point = slot->get_conn_point();
            dc.DrawLine(slot_point.x, slot_point.y, slot_point.x, Y);
        }
        // 将喷嘴与直线相连
        dc.DrawLine(nozzle_point.x, nozzle_point.y, nozzle_point.x, Y);
        m_nozzle->set_wite_color(nozzle_color);

        //如果喷嘴检测到有材料，料线和喷嘴要有颜色
        if (m_hasMatlStation && m_currentLoadSlot != -1) {
            wxColour curr_color = m_material_slots_four[m_currentLoadSlot]->get_material_info().m_color;
            if (curr_color.IsOk()) {
                wire_color   = curr_color;
                nozzle_color = curr_color;
            }
            dc.SetPen(wxPen(wire_color, FromDIP(1)));
            // 先画中间贯通的直线
            wxPoint slot_point   = m_material_slots_four[m_currentLoadSlot]->get_conn_point();
            wxPoint nozzle_point = m_nozzle->get_conn_point();
            int     x1           = slot_point.x;
            int     x2           = nozzle_point.x;
            int     Y            = (slot_point.y + nozzle_point.y) / 2;
            dc.DrawLine(x1, Y, x2, Y);
            // 将料槽与直线相连
            dc.DrawLine(slot_point.x, slot_point.y, slot_point.x, Y);
            // 将喷嘴与直线相连
            dc.DrawLine(nozzle_point.x, nozzle_point.y, nozzle_point.x, Y);
            if (m_nozzle_has_wire) {
                m_nozzle->set_wite_color(nozzle_color);
            }
            
        }

        break;
    }
    default: break;
    }
    
}

void MaterialSlotArea::on_asides_mouse_down(wxMouseEvent& event)
{
    abandon_selected();
    wxCommandEvent clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId()); // 为了改变进丝按钮状态
    ProcessWindowEvent(clicked_event);
}

void MaterialSlotArea::connectEvent() 
{ 
    Bind(wxEVT_PAINT, &MaterialSlotArea::paintEvent, this);
    for (auto& slot : m_material_slots_four) {
        Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialSlotArea::slot_selected_event, this, slot->GetId());
    }
    for (auto& slot : m_material_slot_one) {
        Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialSlotArea::slot_selected_event, this, slot->GetId());
    }
    
}

void MaterialSlotArea::calculate_connection_points(const wxPoint& slot_offset, const wxPoint& nozzle_offset)
{
    //分别计算槽和喷嘴的链接点
    for (auto& slot : (*m_curr_slot_contaier)) {
        wxPoint pos  = slot->GetPosition();
        wxSize  size = slot->GetSize();
        int     x    = pos.x + size.GetWidth() / 2;
        int     y    = pos.y + size.GetHeight();
        slot->set_conn_point(wxPoint(x, y) + slot_offset);
    }
    wxPoint pos  = m_nozzle->GetPosition();
    wxSize  size = m_nozzle->GetSize();
    int     x    = pos.x + size.GetWidth() / 2;
    int     y    = pos.y;
    m_nozzle->set_conn_point(wxPoint(x, y) + nozzle_offset);
}

void MaterialSlotArea::prepare_layout(wxWindow* parent)
{
    // 上方料槽所需容器
    m_slot_group       = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(81)));
    m_slot_group->Bind(wxEVT_LEFT_DOWN, &MaterialSlotArea::on_asides_mouse_down, this);
    m_slot_group->SetBackgroundColour(wxColour(255, 255, 255));
    // 准备四色料槽所需料槽
    for (int i = 0; i < 4; ++i) {
        MaterialSlotWgt* material_slot = new MaterialSlotWgt(m_slot_group, wxID_ANY, i + 1, wxDefaultPosition,
                                                             wxSize(FromDIP(62), FromDIP(80)));
        material_slot->set_slot_wgt_type(MaterialSlotWgt::MaterialStation);
        m_material_slots_four.push_back(material_slot);
    }
    // 准备外挂料槽所需料槽
    MaterialSlotWgt* material_slot = new MaterialSlotWgt(m_slot_group, wxID_ANY, 1, wxDefaultPosition, wxSize(FromDIP(62), FromDIP(80)));
    material_slot->set_slot_wgt_type(MaterialSlotWgt::IndependentMatl);
    m_material_slot_one.push_back(material_slot);

    // 准备下方喷嘴所需容器
    m_nozzle_win   = new wxWindow(parent, wxID_ANY, wxDefaultPosition,
                                            wxSize(m_slot_group->GetSize().GetWidth(), FromDIP(30))); // 与上边的四个料槽等宽
    m_nozzle_win->Bind(wxEVT_LEFT_DOWN, &MaterialSlotArea::on_asides_mouse_down, this);
    m_nozzle_win->SetBackgroundColour(wxColour(255, 255, 255));
    // 准备下方喷嘴
    m_nozzle = new Nozzle(m_nozzle_win, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(30), FromDIP(14)));
    m_nozzle->Bind(wxEVT_LEFT_DOWN, &MaterialSlotArea::on_asides_mouse_down, this);
   
}

void MaterialSlotArea::setup_layout_four(wxWindow* parent)
{
    // 整体布局所用的sizer
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    //布局上方四个料槽
    wxBoxSizer* slot_group_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < m_material_slot_one.size(); ++i) {
        m_material_slot_one[i]->Hide();
    }
    for (int i = 0; i < 4; ++i) {
        slot_group_sizer->Add(m_material_slots_four[i], 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
        m_material_slots_four[i]->Show();
        if (i < 3) {
            slot_group_sizer->AddSpacer(FromDIP(23));
        }
    }
    m_slot_group->SetSizer(slot_group_sizer);
    m_slot_group->Layout();
    slot_group_sizer->Fit(m_slot_group);
    //布局下方喷嘴
    wxBoxSizer* nozzle_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto        s            = m_material_slots_four[0]->GetSize();
    m_nozzle_win->SetMinSize(wxSize(m_slot_group->GetSize().GetWidth(), FromDIP(30)));
    nozzle_sizer->AddStretchSpacer();
    nozzle_sizer->Add(m_nozzle, 0, wxTOP | wxBOTTOM, 0);
    nozzle_sizer->AddStretchSpacer();
    m_nozzle_win->SetSizer(nozzle_sizer);
    m_nozzle_win->Layout();

    //整体布局
    sizer->AddSpacer(FromDIP(16));
    sizer->Add(m_slot_group, 0, wxLEFT, FromDIP(30));
    sizer->AddStretchSpacer();
    sizer->Add(m_nozzle_win, 0, wxLEFT, FromDIP(30));
    SetSizer(sizer);
    Layout();
    m_curr_slot_contaier = &m_material_slots_four;
    calculate_connection_points(m_slot_group->GetPosition(), m_nozzle_win->GetPosition());
    m_layout_mode = LayoutMode::Four;
}

void MaterialSlotArea::setup_layout_one(wxWindow* parent)
{
    // 整体布局所用的sizer
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    // 布局上方一个料槽
    wxBoxSizer* slot_group_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < m_material_slots_four.size(); ++i) {
        m_material_slots_four[i]->Hide();
    }

    slot_group_sizer->Add(m_material_slot_one[0], 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    m_material_slot_one[0]->Show();
    m_slot_group->SetSizer(slot_group_sizer);
    m_slot_group->Layout();
    slot_group_sizer->Fit(m_slot_group);
    // 布局下方喷嘴
    wxBoxSizer* nozzle_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle_win->SetMinSize(wxSize(m_slot_group->GetSize().GetWidth(), FromDIP(30)));
    nozzle_sizer->AddStretchSpacer();
    nozzle_sizer->Add(m_nozzle, 0, wxTOP | wxBOTTOM, 0);
    nozzle_sizer->AddStretchSpacer();
    m_nozzle_win->SetSizer(nozzle_sizer);
    m_nozzle_win->Layout();

    // 整体布局
    sizer->AddSpacer(FromDIP(13));
    sizer->Add(m_slot_group, 0, wxLEFT, FromDIP(30));
    sizer->AddStretchSpacer();
    sizer->Add(m_nozzle_win, 0, wxLEFT, FromDIP(30));
    SetSizer(sizer);
    Layout();
    m_curr_slot_contaier = &m_material_slot_one;
    calculate_connection_points(m_slot_group->GetPosition(), m_nozzle_win->GetPosition());
    m_layout_mode = LayoutMode::One;
}

void MaterialSlotArea::slot_selected_event(wxCommandEvent& event)
{
    if (!m_radio_changeable) {
        return;
    }
    //当有某个槽被点击了
    for (auto& slot : *m_curr_slot_contaier) {
        if (event.GetId() == slot->GetId()) {
            m_radio_slot = slot;
            m_radio_slot->set_selected(true); 
        } else {
            slot->set_selected(false);
        }
    }
    wxCommandEvent clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());//为了改变进丝按钮状态
    ProcessWindowEvent(clicked_event);
}
MaterialSlotArea* MaterialSlotArea::s_self = nullptr;
    
ColorButton::ColorButton(wxWindow*          parent,
                         wxWindowID         id,
                         const wxString&    label,
                         const wxPoint&     pos,
                         const wxSize&      size,
                         long               style,
                         const wxValidator& validator,
                         const wxString&    name) 
    : wxPanel(parent, id, pos, size, style, name)
    , m_color(wxColour(255, 255, 255))
    , m_unknow_color(this,"unknow_color_btn", 26)
    , m_mode(PaintMode::UnknowColor)
{ 
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(wxColour(255, 255, 255));
    connectEvent();
}

ColorButton::~ColorButton() {}

void ColorButton::set_color(const wxColour& color)
{
    m_color = color;
    if (m_color == wxColour("#FFFFFF")) {
        m_mode = PaintMode::WhiteWithCircle;
    } else {
        m_mode = PaintMode::Color;
    }
    Refresh();
}

wxColour& ColorButton::get_color() { return m_color; }

void ColorButton::paintEvent(wxPaintEvent& event)
{
    wxSize    size = GetSize();
    wxBufferedPaintDC                  dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    switch (m_mode) {
    case ColorButton::Color: {
        gc->SetBrush(wxBrush(wxColour(255, 255, 255)));
        gc->SetPen(wxPen(wxColour(255, 255, 255), 0));
        gc->DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
        gc->SetBrush(wxBrush(m_color));
        gc->SetPen(wxPen(m_color, 0));
        gc->DrawEllipse(0, 0, size.GetWidth() -1, size.GetHeight() -1);
        break;
    }
    case ColorButton::UnknowColor: {
        int iconW = m_unknow_color.GetBmpWidth();
        int iconH = m_unknow_color.GetBmpHeight();
        int iconX = (size.GetWidth() - iconW) / 2;
        int iconY = (size.GetHeight() - iconH) / 2;
        gc->DrawBitmap(m_unknow_color.bmp(), iconX, iconY, iconW, iconH);
        break;
    }
    case ColorButton::WhiteWithCircle: {
        gc->SetBrush(wxBrush(wxColour(255, 255, 255)));
        gc->SetPen(wxPen(wxColour(255, 255, 255), 0));
        gc->DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
        gc->SetBrush(wxBrush(m_color));
        gc->SetPen(wxPen(wxColour(51, 51, 51), 1));
        gc->DrawEllipse(1, 1, size.GetWidth() - 2, size.GetHeight() - 2);
        break;
    }
    default: break;
    }
}

void ColorButton::connectEvent()
{
    Bind(wxEVT_PAINT, &ColorButton::paintEvent, this); 
    Bind(wxEVT_LEFT_UP, [=](wxMouseEvent& event) {
        wxCommandEvent evt(wxEVT_BUTTON, GetId());
        evt.SetEventObject(this);
        ProcessEvent(evt);

        Refresh();
        event.Skip();
    });
}

RoundedButton::RoundedButton(wxWindow*          parent,
                             wxWindowID         id,
                             bool               isFill,
                             const wxString&    label,
                             const wxPoint&     pos,
                             const wxSize&      size,
                             long               style,
                             const wxValidator& validator,
                             const wxString&    name) 
    : wxPanel(parent, id, pos, size, style, name)
    , m_state(ButtonState::Normal)
    , m_is_fill(isFill)
    , m_bitmap_available(false)
    , m_radius(0.0)
{
    SetLabel(label);
    SetBackgroundColour(wxColour(255, 255, 255));
    connectEvent();
}

RoundedButton::~RoundedButton()
{}

void RoundedButton::set_bitmap(const wxBitmap& bitmap) 
{ 
    m_bitmap = bitmap;
    m_bitmap_available = true;
}

void RoundedButton::set_state_color(const wxColour& color, ButtonState state) 
{
    switch (state) {
    case RoundedButton::Normal: {
        m_normal_color = color;
        break;
    }
    case RoundedButton::Hovered: {
        m_hovered_color = color;
        break;
    }
    case RoundedButton::Pressed: {
        m_pressed_color = color;
        break;
    }
    case RoundedButton::Inavaliable: {
        m_inavaliable_color = color;
        break;
    }
    default: break;
    }
}

void RoundedButton::set_radius(double radius) { m_radius = radius; }

void RoundedButton::set_state(ButtonState state)
{
    m_state = state;
    Refresh();
}

void RoundedButton::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    // 根据状态绘制不同的背景颜色和前景颜色
    wxSize size = GetSize();
    wxColour txt_color;
    switch (m_state) {
    case ButtonState::Normal: {
        if (m_is_fill) {
            gc->SetBrush(wxBrush(m_normal_color));
            gc->SetPen(wxPen(m_normal_color, 0));
        } else {
            gc->SetPen(wxPen(m_normal_color, 1));
            txt_color = m_normal_color;
        }
        break;
    }
    case ButtonState::Hovered: {
        if (m_is_fill) {
            gc->SetBrush(wxBrush(m_hovered_color));
            gc->SetPen(wxPen(m_hovered_color, 0));
        } else {
            gc->SetPen(wxPen(m_hovered_color, 1));
            txt_color = m_hovered_color;
        }
        break;
    }
    case ButtonState::Pressed: {
        if (m_is_fill) {
            gc->SetBrush(wxBrush(m_pressed_color));
            gc->SetPen(wxPen(m_pressed_color, 0));
        } else {
            gc->SetPen(wxPen(m_pressed_color, 1));
            txt_color = m_pressed_color;
        }
        break;
    }
    default: break;
    }
    // 不可用优先级最高
    if (!IsEnabled()) {
        if (m_is_fill) {
            gc->SetBrush(wxBrush(m_inavaliable_color));
            gc->SetPen(wxPen(m_inavaliable_color, 0));
        } else {
            gc->SetPen(wxPen(m_inavaliable_color, 1));
            txt_color = m_inavaliable_color;
        }
    }
    gc->DrawRoundedRectangle(1, 1, size.GetWidth() - 2, size.GetHeight() - 2, m_radius);
    // 绘制文本
    int textX = (size.x - dc.GetTextExtent(GetLabel()).x) / 2;
    int textY = (size.y - dc.GetTextExtent(GetLabel()).y) / 2;
    if (!m_is_fill) {
        dc.SetTextForeground(txt_color);
    }
    dc.DrawText(GetLabel(), textX, textY);

    if (m_bitmap_available) {
        // 绘制图标
        int iconX = (size.GetWidth() - m_bitmap.GetWidth()) / 2;
        int iconY = (size.GetHeight() - m_bitmap.GetHeight()) / 2;
        gc->DrawBitmap(m_bitmap, iconX, iconY, m_bitmap.GetWidth(), m_bitmap.GetHeight());
    }
    
}

void RoundedButton::OnMouseDown(wxMouseEvent& event){
    m_state = ButtonState::Pressed;
    Refresh();
    wxCommandEvent clickEvent(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    ProcessWindowEvent(clickEvent);
}

void RoundedButton::OnMouseUp(wxMouseEvent& event) {
    m_state = ButtonState::Normal;
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    ProcessEvent(evt);
    Refresh();
    event.Skip();
}

void RoundedButton::OnMouseEnter(wxMouseEvent& event){
    m_state = ButtonState::Hovered;
    Refresh();
}

void RoundedButton::OnMouseLeave(wxMouseEvent& event){
    m_state = ButtonState::Normal;
    Refresh();
}

void RoundedButton::connectEvent()
{
    Bind(wxEVT_PAINT, &RoundedButton::paintEvent, this);
    Bind(wxEVT_LEFT_DOWN, &RoundedButton::OnMouseDown, this);
    Bind(wxEVT_LEFT_UP, &RoundedButton::OnMouseUp, this);
    Bind(wxEVT_ENTER_WINDOW, &RoundedButton::OnMouseEnter, this);
    Bind(wxEVT_LEAVE_WINDOW, &RoundedButton::OnMouseLeave, this);
}


IdentifyButton::IdentifyButton(wxWindow*          parent,
                               wxWindowID         id,
                               const wxString&    label,
                               const wxPoint&     pos,
                               const wxSize&      size,
                               long               style,
                               const wxValidator& validator,
                               const wxString&    name)
    : wxButton(parent, id, label, pos, size, style, validator, name), m_isSelected(false)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    Bind(wxEVT_PAINT, &IdentifyButton::paintEvent, this);
}

IdentifyButton::~IdentifyButton() {}

void IdentifyButton::set_bitmap(const ScalableBitmap& select, const ScalableBitmap& unselect, const ScalableBitmap& disabled)
{ 
    m_select_bitmap = select; 
    m_unselect_bitmap = unselect;
    m_disabled_bitmap = disabled;
}

void IdentifyButton::set_select_state(bool isSelected) 
{ 
    m_isSelected = isSelected; 
    Refresh();
}

void IdentifyButton::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    ScalableBitmap* bitmap = (m_isSelected) ? &m_select_bitmap : &m_unselect_bitmap;
    if (!IsEnabled()) {
        bitmap = &m_disabled_bitmap;
    }
    // 绘制图标
    int iconX = (GetSize().GetWidth() - bitmap->GetBmpWidth()) / 2;
    int iconY = (GetSize().GetHeight() - bitmap->GetBmpHeight()) / 2;
    dc.DrawBitmap((*bitmap).bmp(), iconX, iconY);
    // 根据状态绘制下方横线
    if (m_isSelected && IsEnabled()) {
        dc.SetBrush(wxBrush(wxColour(50, 141, 251)));
        dc.SetPen(wxPen(wxColour(50, 141, 251)));
        dc.DrawRectangle(0, GetSize().GetHeight() - FromDIP(2), GetSize().GetWidth(), FromDIP(2));
    }
}

int Palette::s_pid = -1;

Palette::Palette(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style, const wxString& name) 
    : wxDialog(parent, id, title, pos, size, wxNO_BORDER | wxFRAME_SHAPED, name)
    , m_seleced_color(wxColour(255, 255, 255))
{
    SetMinSize(wxSize(FromDIP(309), FromDIP(294)));
    setup_layout(this); 
    connectEvent();
}

Palette::~Palette() {}

void Palette::set_material_station_color_vector(std::vector<wxColour> color_vec) 
{ 
    
}

wxColour& Palette::get_seleced_color() { return m_seleced_color; }

void Palette::resizeEvent(wxSizeEvent& event)
{
    wxEventBlocker evtBlocker(this, wxEVT_SIZE);
    wxGraphicsPath path = wxGraphicsRenderer::GetDefaultRenderer()->CreatePath();
    path.AddRoundedRectangle(0, 0, GetSize().GetWidth(), GetSize().GetHeight(), 6);
    SetShape(path);
    event.Skip();
}

void Palette::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.SetPen(wxPen(wxColour(193, 193, 193), 1));
    int width  = GetSize().GetWidth();
    int height = GetSize().GetHeight();
    int radius = 6;
    dc.DrawRoundedRectangle(0, 0, width, height, radius);
}

void Palette::setup_layout(wxWindow* parent) 
{ 
    int         width         = GetSize().GetWidth() - FromDIP(4);//减去4是为了给paint时间画出的圆角矩形留空间
    int         height        = GetSize().GetHeight();
    wxBoxSizer* palette_sizer = new wxBoxSizer(wxVERTICAL);
    //关闭按钮布局
    wxBoxSizer* sizer_close    = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   area_close  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(10)));
    area_close->SetBackgroundColour(wxColour(255, 255, 255));
    wxButton* close_btn = new wxButton(area_close, wxID_CANCEL, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(10), FromDIP(10)), wxNO_BORDER);
    close_btn->SetBackgroundColour(wxColour(255, 255, 255));
    close_btn->SetBitmap(create_scaled_bitmap("color_close_btn", nullptr, 10));
    sizer_close->AddStretchSpacer();
    sizer_close->Add(close_btn, 0, wxTOP | wxBOTTOM, 0);
    sizer_close->AddSpacer(FromDIP(19));
    area_close->SetSizer(sizer_close);
    area_close->Layout();
    //材料站颜色标题布局
    wxBoxSizer* sizer_station_title = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   area_station_title  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(19)));
    area_station_title->SetBackgroundColour(wxColour(255, 255, 255));
    m_station_color_lab             = new wxStaticText(area_station_title, wxID_ANY, _L("Material Station"), wxDefaultPosition,
                                                       wxSize(FromDIP(249), FromDIP(19)), wxALIGN_LEFT);
    m_station_color_lab->SetBackgroundColour(wxColour(255, 255, 255));
    sizer_station_title->AddSpacer(FromDIP(27));
    sizer_station_title->Add(m_station_color_lab, 0, wxTOP | wxBOTTOM, 0);
    sizer_station_title->AddStretchSpacer();
    area_station_title->SetSizer(sizer_station_title);
    area_station_title->Layout();
    // 材料站颜色按钮布局
    wxBoxSizer* sizer_station_color = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   area_station_color  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(26)));
    area_station_color->SetBackgroundColour(wxColour(255, 255, 255));
    sizer_station_color->AddSpacer(FromDIP(27));
    int                   type = s_pid;
    std::vector<wxColour> all_color;
    {
        switch (type)
        {
        case AD5X:
        case GUIDER_4_PRO: {
            MaterialSlotArea* slot_area = MaterialSlotArea::get_inst();
            if (!slot_area)
                return;
            all_color = slot_area->get_all_material_color();
            break;
        }
        case C5:
        case C5P: {
            FFNozzles* nozzles = FFNozzles::get_inst();
            if (!nozzles)
                return;
            all_color = nozzles->GetAllColours();
            break;
        }
        }
    }
    //MaterialSlotArea*     slot_area = MaterialSlotArea::get_inst();
    //if (!slot_area)  return;
    //std::vector<wxColour> all_color(slot_area->get_all_material_color());
    for (auto& color : all_color) {
        ColorButton* color_btn = new ColorButton(area_station_color, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                 wxSize(FromDIP(26), FromDIP(26)));
        color_btn->set_color(color);
        m_station_color_btns.push_back(color_btn);
        sizer_station_color->Add(color_btn, 0, wxTOP | wxBOTTOM, 0);
        sizer_station_color->AddSpacer(FromDIP(30));
    }
    sizer_station_color->AddStretchSpacer();
    area_station_color->SetSizer(sizer_station_color);
    area_station_color->Layout();
    // 颜色库标题布局
    wxBoxSizer* sizer_lib_title = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   area_lib_title  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(19)));
    area_lib_title->SetBackgroundColour(wxColour(255, 255, 255));
    m_color_lib_lab = new wxStaticText(area_lib_title, wxID_ANY, _L("Color Library"), wxDefaultPosition, wxSize(FromDIP(250), FromDIP(19)),
                                       wxALIGN_LEFT);
    m_color_lib_lab->SetBackgroundColour(wxColour(255, 255, 255));
    sizer_lib_title->AddSpacer(FromDIP(27));
    sizer_lib_title->Add(m_color_lib_lab, 0, wxTOP | wxBOTTOM, 0);
    sizer_lib_title->AddStretchSpacer();
    area_lib_title->SetSizer(sizer_lib_title);
    area_lib_title->Layout();
    //颜色库按钮布局
    wxGridSizer* gridSizer      = new wxGridSizer(5, 5, FromDIP(7), FromDIP(30)); // 6 行 4 列，垂直水平间距为 7，30
    wxWindow*    area_lib_color = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(250), FromDIP(158)));
    area_lib_color->SetBackgroundColour(wxColour(255, 255, 255));
    for (int i = 0; i < 24; ++i) {
        ColorButton* color_btn = new ColorButton(area_lib_color, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                                 wxSize(FromDIP(26), FromDIP(26)));
        if (FFUtils::isNozzlesPrinter(type)) {
            color_btn->set_color(wxColour(u1_color_lib[i]));
        } else {
            color_btn->set_color(wxColour(color_lib[i]));
        }
        m_color_lib_btns.push_back(color_btn);
        gridSizer->Add(color_btn, 0, wxALIGN_CENTRE | wxALL, 0);
    }
    area_lib_color->SetSizer(gridSizer);
    area_lib_color->Layout();
    //整体布局
    palette_sizer->AddSpacer(FromDIP(9));
    palette_sizer->Add(area_close, 0, wxLEFT | wxRIGHT, FromDIP(2));
    palette_sizer->AddSpacer(FromDIP(6));
    palette_sizer->Add(area_station_title, 0, wxLEFT | wxRIGHT, FromDIP(2));
    palette_sizer->AddSpacer(FromDIP(7));
    palette_sizer->Add(area_station_color, 0, wxLEFT | wxRIGHT, FromDIP(2));
    palette_sizer->AddSpacer(FromDIP(17));
    palette_sizer->Add(area_lib_title, 0, wxLEFT | wxRIGHT, FromDIP(2));
    palette_sizer->AddSpacer(FromDIP(7));
    palette_sizer->Add(area_lib_color, 0, wxLEFT, FromDIP(28));
    palette_sizer->AddSpacer(FromDIP(17));
    SetSizer(palette_sizer);
    Layout();
    palette_sizer->Fit(this);
}

void Palette::connectEvent() 
{ 
    Bind(wxEVT_SIZE, &Palette::resizeEvent, this);
    Bind(wxEVT_PAINT, &Palette::paintEvent, this);
    assert(!m_color_lib_btns.empty());
    for (auto& btn : m_color_lib_btns) {
        Bind(wxEVT_COMMAND_BUTTON_CLICKED, &Palette::on_color_lib_clicked, this, btn->GetId());
    } 
}

void Palette::on_color_lib_clicked(wxCommandEvent& event) 
{ 
    wxObject* btn = event.GetEventObject(); 
    ColorButton* color_btn = static_cast<ColorButton*>(btn);
    m_seleced_color        = color_btn->get_color();
    wxCommandEvent clickEvent(wxEVT_COMMAND_BUTTON_CLICKED, wxID_OK);
    ProcessWindowEvent(clickEvent);
}

const char* Palette::color_lib[] = {"#FFFFFF", "#FEF043", "#DCF478", "#0ACC38", "#067749", "#0C6283", "#0DE2A0", "#75D9F3",
                                    "#45A8F9", "#2750E0", "#46328E", "#A03CF7", "#F330F9", "#D4B0DC", "#F95D73", "#F72224",
                                    "#7C4B00", "#F98D33", "#FDEBD5", "#D3C4A3", "#AF7836", "#898989", "#BCBCBC", "#161616"};

const char* Palette::u1_color_lib[] = {"#FFFFFF", "#FFF245", "#DEF578", "#21CC3D", "#167A4B", "#156682", "#24E4A0", "#7BD9F0",
                                       "#4CAAF8", "#2E54DD", "#48358C", "#A341F7", "#F435F6", "#D584DE", "#FA6173", "#F82D29",
                                       "#805003", "#F9903B", "#FCEBD7", "#D5C5A1", "#B17C38", "#8C8C89", "#BEBEBE", "#1B1B1B"};

int MaterialDialog::s_pid;

MaterialDialog::MaterialDialog(wxWindow*       parent,
                               wxWindowID      id,
                               const wxString& title,
                               const int&      state,
                               const wxPoint&  pos,
                               const wxSize&   size,
                               long            style,
                               const wxString& name)
    : wxDialog(parent, id, title, pos, size, wxNO_BORDER | wxFRAME_SHAPED, name)
    , m_material_name(wxEmptyString)
    , m_material_color(wxColour())
    , m_state(state)
    , m_curr_options(nullptr)
{
    setup_layout(this);
    connectEvent();
    set_info_state(get_info_state() | InfoState::NameKnown);
    m_material_name = m_comboBox->GetString(0);
}
MaterialDialog::~MaterialDialog() {}

wxPoint MaterialDialog::calculate_pop_position(const wxPoint& point, const wxSize& size)
{
    wxDisplay display;
    wxRect    screenRect = display.GetClientArea();

    wxPoint finally_pos = point; // 最终弹出位置
    // 先横向比较
    int min_X = screenRect.x;
    int max_X = screenRect.x + screenRect.width;
    int min_x = point.x;
    int max_x = point.x + size.GetWidth();
    if (min_x < min_X) { // 对话框左溢出屏幕
        finally_pos.x += (min_X - min_x);
    }
    if (max_x > max_X) { // 对话框右溢出屏幕
        finally_pos.x -= (max_x - max_X);
    }
    // 再纵向比较
    int min_Y = screenRect.y;
    int max_Y = screenRect.y + screenRect.height;
    int min_y = point.y;
    int max_y = point.y + size.GetHeight();
    if (min_y < min_Y) { // 对话框上溢出屏幕
        finally_pos.y += (min_Y - min_y);
    }
    if (max_y > max_Y) { // 对话框下溢出屏幕
        finally_pos.y -= (max_y - max_Y);
    }
    return finally_pos;
}

void MaterialDialog::set_printer_type(int curId) 
{ 
    s_pid = curId; 
}

void MaterialDialog::set_material_name(const wxString& name)
{
    m_material_name = name; // 这里还要同步名字到combobox
    int index = 0;
    if (!m_curr_options)
        return;
    for (; index < (*m_curr_options).size(); ++index) {
        if ((*m_curr_options)[index] == name) {
            break;
        }
    }
    if (index != (*m_curr_options).size()) {
        m_comboBox->SetSelection(index);
    }
}

void MaterialDialog::set_material_color(const wxColour& color)
{
    m_material_color = color;
    m_color_btn->set_color(m_material_color);
}

wxColour& MaterialDialog::get_material_color() { return m_material_color; }

wxString& MaterialDialog::get_material_name() { return m_material_name; }

int MaterialDialog::get_info_state() { return m_state; }

void MaterialDialog::set_info_state(int state){
    m_state = state;
    update_ok_state();
}

void MaterialDialog::resizeEvent(wxSizeEvent& event)
{
    wxEventBlocker evtBlocker(this, wxEVT_SIZE);
    wxGraphicsPath path = wxGraphicsRenderer::GetDefaultRenderer()->CreatePath();
    path.AddRoundedRectangle(0, 0, GetSize().GetWidth(), GetSize().GetHeight(), 6);
    SetShape(path);
    event.Skip();
}

void MaterialDialog::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
    dc.SetPen(wxPen(wxColour(193, 193, 193), 0));
    int width  = GetSize().GetWidth();
    int height = GetSize().GetHeight();
    int radius = 6; 
    dc.DrawRoundedRectangle(0, 0, width, height, radius); 
    event.Skip();
}

void MaterialDialog::setup_layout(wxWindow* parent) 
{ 
    wxBoxSizer* dialog_sizer = new wxBoxSizer(wxVERTICAL);
    //上半部分材料和颜色选择区
    wxBoxSizer* select_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   select_area  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(356), FromDIP(129)));
    select_area->SetBackgroundColour(wxColour(255, 255, 255));

    m_type_lab = new wxStaticText(select_area, wxID_ANY, _L("Filament type"), wxDefaultPosition, wxSize(FromDIP(347), FromDIP(19)),
                                  wxALIGN_LEFT);

    m_comboBox = new CustomOwnerDrawnComboBox(select_area, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(347), FromDIP(34)), 0,
                                              NULL, wxCB_READONLY);
    init_comboBox();

    m_color_lab = new wxStaticText(select_area, wxID_ANY, _L("Color"), wxDefaultPosition, wxSize(FromDIP(347), FromDIP(19)), wxALIGN_LEFT);

    wxBoxSizer* color_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   color_area  = new wxWindow(select_area, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(347), FromDIP(26)));
    color_area->SetBackgroundColour(wxColour(255, 255, 255));
    m_color_btn = new ColorButton(color_area, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxSize(FromDIP(26), FromDIP(26)), wxNO_BORDER);
    color_sizer->Add(m_color_btn, 0, wxTOP | wxBOTTOM, 0);
    color_sizer->AddStretchSpacer();
    color_area->SetSizer(color_sizer);
    color_area->Layout();

    select_sizer->Add(m_type_lab, 0, wxEXPAND |wxRIGHT, FromDIP(9));
    select_sizer->AddSpacer(FromDIP(7));
    select_sizer->Add(m_comboBox, 0, wxEXPAND | wxRIGHT, FromDIP(9));
    select_sizer->AddStretchSpacer();
    select_sizer->Add(m_color_lab, 0, wxEXPAND | wxRIGHT, FromDIP(9));
    select_sizer->AddSpacer(FromDIP(7));
    select_sizer->Add(color_area, 0, wxEXPAND | wxRIGHT, FromDIP(9));
    select_area->SetSizer(select_sizer);
    select_area->Layout();

    // 下半部分按钮区
    wxBoxSizer* button_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   button_area  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(GetSize().GetWidth(), FromDIP(36)));
    button_area->SetBackgroundColour(wxColour(255, 255, 255));
    m_cancel = new RoundedButton(button_area, wxID_CANCEL, false, _L("Cancel"), wxDefaultPosition, wxSize(FromDIP(87), FromDIP(36)));
    m_cancel->set_state_color(wxColour(65, 148, 136), RoundedButton::Normal);
    m_cancel->set_state_color(wxColour(101, 167, 158), RoundedButton::Hovered);
    m_cancel->set_state_color(wxColour(26, 134, 118), RoundedButton::Pressed);
    m_cancel->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_cancel->set_radius(4);
    m_cancel->SetForegroundColour(wxColour(65, 148, 136));

    m_OK     = new RoundedButton(button_area, wxID_OK, true, _L("OK"), wxDefaultPosition, wxSize(FromDIP(87), FromDIP(36)));
    m_OK->set_state_color(wxColour(65, 148, 136), RoundedButton::Normal);
    m_OK->set_state_color(wxColour(101, 167, 158), RoundedButton::Hovered);
    m_OK->set_state_color(wxColour(26, 134, 118), RoundedButton::Pressed);
    m_OK->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_OK->set_radius(4);
    m_OK->SetForegroundColour(wxColour(255, 255, 255));

    button_sizer->AddSpacer(FromDIP(107));
    button_sizer->Add(m_cancel, 0, wxTOP | wxBOTTOM, 0);
    button_sizer->AddSpacer(FromDIP(52));
    button_sizer->Add(m_OK, 0, wxTOP | wxBOTTOM, 0);
    button_sizer->AddStretchSpacer();
    button_area->SetSizer(button_sizer);
    button_area->Layout();
    //对话框整体布局
    dialog_sizer->AddSpacer(FromDIP(17));
    dialog_sizer->Add(select_area, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(32));
    dialog_sizer->AddStretchSpacer();
    dialog_sizer->Add(button_area, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(32));
    dialog_sizer->AddSpacer(FromDIP(18));
    SetSizer(dialog_sizer);
    Layout();
}

void MaterialDialog::connectEvent()
{
    Bind(wxEVT_SIZE, &MaterialDialog::resizeEvent, this);
    Bind(wxEVT_PAINT, &MaterialDialog::paintEvent, this);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialDialog::on_color_btn_clicked, this, m_color_btn->GetId());
    Bind(wxEVT_COMBOBOX, &MaterialDialog::on_comboBox_selected, this, m_comboBox->GetId());
}

void MaterialDialog::on_color_btn_clicked(wxCommandEvent& event) 
{ 
    wxSize   dialog_size(FromDIP(309), FromDIP(296));
    wxPoint  pos(GetScreenPosition().x + GetSize().GetWidth() + FromDIP(5), GetScreenPosition().y - (dialog_size.GetHeight() - GetSize().GetHeight())); // 预计弹出位置
    wxPoint  finally_pos = calculate_pop_position(pos, dialog_size);

    Palette::set_pid(s_pid);
    Palette palette(nullptr, wxID_ANY, wxEmptyString, finally_pos, dialog_size);
    if (palette.ShowModal() == wxID_OK) {
        set_material_color(palette.get_seleced_color());
        set_info_state(get_info_state() | InfoState::ColorKnown);
    } else {
        m_color_btn->Refresh();
    }
}

void MaterialDialog::on_comboBox_selected(wxCommandEvent& event)
{
    int      selectedIndex  = m_comboBox->GetSelection();
    wxString selectedString = m_comboBox->GetString(selectedIndex);
    set_material_name(selectedString);
}

void MaterialDialog::init_comboBox()
{   
    switch (s_pid) {
    case AD5X: {
        m_curr_options = &m_AD5X_options;
        break;
    }
    case GUIDER_4: {
        m_curr_options = &m_G4_options;
        break;
    }
    case GUIDER_4_PRO: {
        m_curr_options = &m_G4Pro_options;
        break;
    }
    case C5:
    case C5P: {
        m_curr_options = &m_U1_options;
        break;
    }
    default: {
        m_curr_options = &m_Other_options;
        break;
    }
    }

    m_comboBox->Clear();
    for (const auto& option : *m_curr_options) {
        m_comboBox->Append(option);
    }
    m_comboBox->SetSelection(0);

}

void MaterialDialog::update_ok_state()
{
    m_OK->Enable((m_state & InfoState::NameKnown) > 0 == (m_state & InfoState::ColorKnown) > 0);
    m_OK->Refresh();
}

MaterialPanel::MaterialPanel(wxWindow*       parent,
                             wxWindowID      winid,
                             const wxPoint&  pos,
                             const wxSize&   size,
                             long            style,
                             const wxString& name)
    : wxPanel(parent, winid, pos, size, style, name), m_cur_id(ComInvalidId)
{
    SetBackgroundColour(wxColour(248, 248, 248));
    //启动布局后各种界面均为默认状态
    setup_layout(this);
    connectEvent();

}

MaterialPanel::~MaterialPanel() {}

void MaterialPanel::init_material_panel() {}

void MaterialPanel::setCurId(int curId)
{
    m_cur_id = curId;
    m_material_slot->setCurId(curId);
    if (m_cur_id != -1) {
        ComDevDetailUpdateEvent event(COM_DEV_DETAIL_UPDATE_EVENT, m_cur_id, 0, MultiComMgr::inst()->devData(m_cur_id).devDetail);
        onComDevDetailUpdate(event);
    }
    m_recognized_btn->Enable(false);
    m_unrecognized_btn->Enable(false);
    m_tips_area->reset_printer_status();
}

void MaterialPanel::OnMouseDown(wxMouseEvent& event) { 
    m_material_slot->abandon_selected(); 
    m_supply_wire->Enable(false);
    m_withdrawn_wire->Enable(false);
}

void MaterialPanel::setup_layout(wxWindow* parent) 
{
    int width = GetSize().GetWidth();
    int height = GetSize().GetHeight();
    //MaterialPanel整体水平布局
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    //MaterialPanel左半部分操作区
    wxBoxSizer* operate_area_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow* operate_area         = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(379), height));
    operate_area->SetBackgroundColour(wxColour(255, 255, 255));
    operate_area->Bind(wxEVT_LEFT_DOWN, &MaterialPanel::OnMouseDown, this);

    //左半部分操作区的上边的切换按钮区
    wxBoxSizer* switch_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   switch_group = new wxWindow(operate_area, wxID_ANY, wxDefaultPosition,
                                            wxSize(operate_area->GetSize().GetWidth(), FromDIP(32)));
    switch_group->SetBackgroundColour(wxColour(255, 255, 255));
    switch_group->Bind(wxEVT_LEFT_DOWN, &MaterialPanel::OnMouseDown, this);

    m_recognized_btn = new IdentifyButton(switch_group, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(48), FromDIP(32)));
    m_recognized_btn->set_bitmap(ScalableBitmap(this, "four_color_select", 16), 
                                 ScalableBitmap(this, "four_color_unselect", 16),
                                 ScalableBitmap(this, "four_color_disabled", 16));
    m_recognized_btn->set_select_state(true);
    m_unrecognized_btn = new IdentifyButton(switch_group, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(48), FromDIP(32)));
    m_unrecognized_btn->set_bitmap(ScalableBitmap(this, "plug_slot_switch_btn_select", 16),
                                   ScalableBitmap(this, "plug_slot_switch_btn_unselect", 16),
                                   ScalableBitmap(this, "plug_slot_switch_btn_disabled", 16));
    m_unrecognized_btn->set_select_state(false);
    switch_sizer->Add(m_recognized_btn, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    switch_sizer->AddSpacer(FromDIP(32));
    switch_sizer->Add(m_unrecognized_btn, 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
    switch_sizer->AddStretchSpacer();
    switch_group->SetSizer(switch_sizer);
    switch_group->Layout();

    // 左半部分操作区的中间的料槽区
    m_material_slot = new MaterialSlotArea(operate_area, wxID_ANY, wxDefaultPosition,
                                           wxSize(operate_area->GetSize().GetWidth(), FromDIP(148)));//增加10
    m_material_slot->SetBackgroundColour(wxColour(255, 255, 255));
    // 左半部分操作区的下边的按钮区
    wxBoxSizer* btn_group_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow* button_group = new wxWindow(operate_area, wxID_ANY, wxDefaultPosition, wxSize(operate_area->GetSize().GetWidth(), FromDIP(34)));
    button_group->SetBackgroundColour(wxColour(255, 255, 255));
    button_group->Bind(wxEVT_LEFT_DOWN, &MaterialPanel::OnMouseDown, this);

    m_supply_wire = new RoundedButton(button_group, wxID_ANY, true, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(61), FromDIP(27)));
    m_supply_wire->set_state_color(wxColour(50, 141, 251), RoundedButton::Normal);
    m_supply_wire->set_state_color(wxColour(149, 197, 255), RoundedButton::Hovered);
    m_supply_wire->set_state_color(wxColour(17, 111, 223), RoundedButton::Pressed);
    m_supply_wire->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_supply_wire->set_radius(4);
    m_supply_wire->set_bitmap(create_scaled_bitmap("supply_wire", nullptr, 18));
    m_supply_wire->Enable(false);


    m_withdrawn_wire = new RoundedButton(button_group, wxID_ANY, true, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(61), FromDIP(27)));
    m_withdrawn_wire->set_state_color(wxColour(50, 141, 251), RoundedButton::Normal);
    m_withdrawn_wire->set_state_color(wxColour(149, 197, 255), RoundedButton::Hovered);
    m_withdrawn_wire->set_state_color(wxColour(17, 111, 223), RoundedButton::Pressed);
    m_withdrawn_wire->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_withdrawn_wire->set_radius(4);
    m_withdrawn_wire->set_bitmap(create_scaled_bitmap("withdrawn_wire", nullptr, 18));
    m_withdrawn_wire->Enable(false);
    btn_group_sizer->AddStretchSpacer();
    btn_group_sizer->Add(m_supply_wire, 0, wxTOP, FromDIP(3));
    btn_group_sizer->AddSpacer(FromDIP(22));
    btn_group_sizer->Add(m_withdrawn_wire, 0, wxTOP, FromDIP(3));
    btn_group_sizer->AddStretchSpacer();
    button_group->SetSizer(btn_group_sizer);
    button_group->Layout();
    // MaterialPanel左半部分操作区整体布局
    operate_area_sizer->Add(switch_group, 0, wxEXPAND | wxALL, 0);
    operate_area_sizer->Add(m_material_slot, 0, wxEXPAND | wxALL, 0);
    operate_area_sizer->Add(button_group, 0, wxEXPAND | wxALL, 0);
    operate_area->SetSizer(operate_area_sizer);
    operate_area->Layout();
    operate_area_sizer->Fit(operate_area);

    // MaterialPanel右半部分提示区
    m_tips_area                 = new TipsArea(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(310), height));
    
    //整体布局
    panel_sizer->Add(operate_area, 0, wxEXPAND | wxALL, 0);
    panel_sizer->AddSpacer(FromDIP(1));
    panel_sizer->Add(m_tips_area, 0, wxEXPAND | wxALL, 0);
    SetSizer(panel_sizer);
    Layout();
    panel_sizer->Fit(this);
}

void MaterialPanel::connectEvent() 
{ 
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_supply_wire_clicked, this, m_supply_wire->GetId()); 
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_withdrawn_wire_clicked, this, m_withdrawn_wire->GetId()); 
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_switch_matlStation_clicked, this, m_recognized_btn->GetId());
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_switch_indepMatl_clicked, this, m_unrecognized_btn->GetId());
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_slot_area_clicked, this, m_material_slot->GetId());
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanel::on_tips_area_cancel_clicked, this, m_tips_area->GetId());

    m_material_slot->Bind(wxEVT_LEFT_DOWN, &MaterialPanel::OnMouseDown, this);

    MultiComMgr::inst()->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &MaterialPanel::onComDevDetailUpdate, this);
}

void MaterialPanel::update_wire_btn_state()
{
    //用户选中某个料槽后才能判断按钮是否可用，若选中两个按钮均初始化为可用
    auto radio_slot       = m_material_slot->get_radio_slot();
    bool slot_is_executive = m_material_slot->is_executive_slot(radio_slot);
    bool supply_enable     = slot_is_executive;
    bool withdrawn_enable  = slot_is_executive;
    //查看打印机是否空闲或打印暂停
    bool free           = m_tips_area->get_tips_area_state() == TipsArea::TipsAreaState::Free;
    bool printingPaused = m_tips_area->get_tips_area_state() == TipsArea::TipsAreaState::PrintingPaused;
    if (!free && !printingPaused) {
        supply_enable    = false;
        withdrawn_enable = false;
    } 
    else if (printingPaused) {
        supply_enable    = !m_material_slot->hasMatlStation();
        withdrawn_enable = supply_enable;
    }
    m_supply_wire->Enable(supply_enable);
    m_withdrawn_wire->Enable(withdrawn_enable);
}

void MaterialPanel::update_cancel_btn_state() 
{
    //用户选中某个料槽后才能判断取消按钮是否可用，若选中取消按钮均初始化为可用
    auto radio_slot    = m_material_slot->get_radio_slot();
    bool slot_is_executive = m_material_slot->is_executive_slot(radio_slot);
    bool is_currentSlot    = m_material_slot->is_current_slot(radio_slot);
    bool enable            = slot_is_executive && is_currentSlot && m_tips_area->is_heating();
    m_tips_area->set_cancel_enable(enable);

}

void MaterialPanel::update_switch_btn_state() 
{ 
    auto printer_type = m_material_slot->get_printer_type(); 
    int                           hasMatlStation = m_material_slot->hasMatlStation();
    switch (printer_type) {
    case GUIDER_4:
    case GUIDER_4_PRO:
    case AD5X: 
        m_recognized_btn->Enable(hasMatlStation);
        m_unrecognized_btn->Enable(!hasMatlStation);
        m_recognized_btn->set_select_state(hasMatlStation);
        m_unrecognized_btn->set_select_state(!hasMatlStation);
        break;
    }
}

void MaterialPanel::on_supply_wire_clicked(wxCommandEvent& event) 
{ 
    m_material_slot->start_supply_wire();
}

void MaterialPanel::on_withdrawn_wire_clicked(wxCommandEvent& event)
{
    m_material_slot->start_withdrawn_wire();
}

void MaterialPanel::on_switch_matlStation_clicked(wxCommandEvent& event)
{ 
    m_material_slot->change_layout_mode(MaterialSlotArea::Four); 
    m_recognized_btn->set_select_state(true);
    m_unrecognized_btn->set_select_state(false);
    m_material_slot->abandon_selected();
    m_supply_wire->Enable(false);
    m_withdrawn_wire->Enable(false);
}

void MaterialPanel::on_switch_indepMatl_clicked(wxCommandEvent& event)
{ 
    m_material_slot->change_layout_mode(MaterialSlotArea::One);
    m_recognized_btn->set_select_state(false);
    m_unrecognized_btn->set_select_state(true);
    m_material_slot->abandon_selected();
    m_supply_wire->Enable(false);
    m_withdrawn_wire->Enable(false);
}

void MaterialPanel::on_slot_area_clicked(wxCommandEvent& event)
{
    update_wire_btn_state();
    update_cancel_btn_state();
}

void MaterialPanel::on_tips_area_cancel_clicked(wxCommandEvent& event) { m_material_slot->cancel_operation(); }

void MaterialPanel::onComDevDetailUpdate(ComDevDetailUpdateEvent& event)
{
    event.Skip();
    if (m_cur_id != event.id)
        return;
    const com_dev_data_t& data = MultiComMgr::inst()->devData(m_cur_id);
    //同步提示区的打印机状态
    m_tips_area->Synchronize_printer_status(data);
    // 同步料槽区的打印机状态
    m_material_slot->synchronize_printer_status(data);
    // 同步进退丝按钮的状态
    update_wire_btn_state();
    update_cancel_btn_state();
    //同步页面切换按钮状态
    update_switch_btn_state();
}


ProgressAreaU1::ProgressAreaU1(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_state_action(StateAction::Free)
    , m_state_step(StateStep::NoProcessed)
{
    setup_layout(this);
}

ProgressAreaU1::~ProgressAreaU1() {}

void ProgressAreaU1::set_state_action(StateAction action)
{ 
    // 该函数只改变不同任务文本内容
    m_state_action = action;
    switch (m_state_action) {
    case ProgressAreaU1::StateAction::Printing:
    case ProgressAreaU1::StateAction::PrintingPaused:
    case ProgressAreaU1::StateAction::Busy:
    case ProgressAreaU1::StateAction::Free:
    case ProgressAreaU1::StateAction::SupplyWire: {
        for (int i = 0; i < 4; ++i) {
            m_txt_group[i]->SetLabelText(m_supply_step[i]);
        }
        break;
    }
    case ProgressAreaU1::StateAction::WithdrawnWire: {
        for (int i = 0; i < 4; ++i) {
            m_txt_group[i]->SetLabelText(m_withdrawn_step[i]);
        }
        break;
    }
    case ProgressAreaU1::StateAction::Canceling:
    default: break;
    }
}

void ProgressAreaU1::set_state_step(StateStep step)
{
    m_state_step = step;
    wxColour dark_txt(51, 51, 51);
    wxColour light_txt(221, 221, 221);
    wxColour blue_txt(50, 141, 251);
    switch (m_state_step) {
    case ProgressAreaU1::StateStep::ConfirmNozzle: {
        m_btn_group[0]->set_state(ProgressNumber::Processing);
        m_txt_group[0]->SetForegroundColour(dark_txt);
        for (int i = 1; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    case ProgressAreaU1::StateStep::Heating: {
        m_btn_group[0]->set_state(ProgressNumber::Succeed);
        m_txt_group[0]->SetForegroundColour(blue_txt);
        m_btn_group[1]->set_state(ProgressNumber::Processing);
        m_txt_group[1]->SetForegroundColour(dark_txt);
        for (int i = 2; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    case ProgressAreaU1::StateStep::PushMaterials: {
        for (int i = 0; i < 2; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(blue_txt);
        }
        m_btn_group[2]->set_state(ProgressNumber::Processing);
        m_txt_group[2]->SetForegroundColour(dark_txt);
        m_btn_group[3]->set_state(ProgressNumber::NotProcess);
        m_txt_group[3]->SetForegroundColour(light_txt);
        break;
    }
    case ProgressAreaU1::StateStep::Finish: {
        for (int i = 0; i < 3; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(dark_txt);
        }
        m_btn_group[3]->set_state(ProgressNumber::Processing);
        m_txt_group[3]->SetForegroundColour(blue_txt);
        for (int i = 0; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::Succeed);
            m_txt_group[i]->SetForegroundColour(blue_txt);
        }
        break;
    }
    case ProgressAreaU1::StateStep::NoProcessed: {
        for (int i = 0; i < 4; ++i) {
            m_btn_group[i]->set_state(ProgressNumber::NotProcess);
            m_txt_group[i]->SetForegroundColour(light_txt);
        }
        break;
    }
    default: break;
    }
}

void ProgressAreaU1::setup_layout(wxWindow* parent)
{
    int         width          = GetSize().GetWidth();
    int         height         = GetSize().GetHeight();
    wxBoxSizer* progress_sizer = new wxBoxSizer(wxHORIZONTAL);
    // 序号按钮区布局
    wxBoxSizer* num_btn_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   num_btn_area  = new LineArea(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(21), height));
    m_btn_group.reserve(4);
    for (int i = 0; i < 4; ++i) {
        ProgressNumber* col_btn = new ProgressNumber(num_btn_area, wxID_ANY, i + 1, wxDefaultPosition, wxSize(FromDIP(21), FromDIP(21)));
        m_btn_group.push_back(col_btn);
        num_btn_sizer->Add(col_btn, 0, wxLEFT | wxRIGHT, 0);
        if (i < 3) {
            num_btn_sizer->AddStretchSpacer();
        }
    }
    num_btn_area->SetSizer(num_btn_sizer);
    num_btn_area->Layout();

    // 文本区布局
    wxBoxSizer* txt_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   txt_area  = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(204), height));
    txt_area->SetBackgroundColour(wxColour(255, 255, 255));
    m_txt_group.reserve(4);
    for (int i = 0; i < 4; ++i) {
        wxStaticText* txt = new wxStaticText(txt_area, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(204), FromDIP(17)),
                                             wxALIGN_LEFT);
        m_txt_group.push_back(txt);
        txt_sizer->Add(txt, 0, wxLEFT, FromDIP(9));
        if (i < 3) {
            txt_sizer->AddStretchSpacer();
        }
    }
    txt_area->SetSizer(txt_sizer);
    txt_area->Layout();

    // 整体布局
    progress_sizer->Add(num_btn_area, 0, wxLEFT | wxRIGHT, 0);
    progress_sizer->Add(txt_area, 0, wxLEFT | wxRIGHT, 0);
    progress_sizer->AddStretchSpacer();
    SetSizer(progress_sizer);
    Layout();
}


TipsAreaU1::TipsAreaU1(wxWindow*        parent,
                       wxWindowID       id,
                       const wxPoint&   pos,
                       const wxSize&    size,
                       long             style,
                       const wxString&  name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_state(TipsAreaU1State::Free)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    setup_layout(this);
    m_progress->set_state_action(ProgressAreaU1::StateAction::Free);
    m_progress->set_state_step(ProgressAreaU1::StateStep::NoProcessed);
}

TipsAreaU1::~TipsAreaU1() {}

void TipsAreaU1::reset_printer_status()
{
    m_progress->set_state_action(ProgressAreaU1::StateAction::Free);
    m_progress->set_state_step(ProgressAreaU1::StateStep::NoProcessed);
}

TipsAreaU1::TipsAreaU1State TipsAreaU1::get_tips_area_state() { return m_state; }

void TipsAreaU1::setup_layout(wxWindow* parent)
{
    // 布局进度信息控件
    wxBoxSizer* progress_sizer = new wxBoxSizer(wxVERTICAL);
    progress_sizer->AddStretchSpacer();


    m_tips_area_title = new wxStaticText(parent, wxID_ANY, _L("Tips"), wxDefaultPosition, wxSize(FromDIP(200), FromDIP(17)),
                                         wxALIGN_LEFT);
    m_tips_area_title->SetForegroundColour(wxColour(50, 141, 251));
    progress_sizer->Add(m_tips_area_title, 0, wxLEFT | wxRIGHT, FromDIP(27));
    progress_sizer->AddSpacer(FromDIP(8));

    m_progress = new ProgressAreaU1(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(200), FromDIP(141)));
    progress_sizer->Add(m_progress, 0, wxLEFT | wxRIGHT, FromDIP(27));
    progress_sizer->AddStretchSpacer();
    m_progress->Show();
    SetSizer(progress_sizer);
    Layout();
}

// TODO：后续对接设备的时候再修改该函数逻辑 目前只保证编译通过
void TipsAreaU1::Synchronize_printer_status(const com_dev_data_t& data)
{
    m_hasMatlStation = data.devDetail->hasMatlStation;
    if (m_hasMatlStation) {
        // 表示打印机接有四色材料站
        m_stateAction = data.devDetail->matlStationInfo.stateAction;
        m_stateStep   = data.devDetail->matlStationInfo.stateStep;
    } else {
        // 表示打印机未接有四色材料站
        m_stateAction = data.devDetail->indepMatlInfo.stateAction;
        m_stateStep   = data.devDetail->indepMatlInfo.stateStep;
    }

    m_state         = static_cast<TipsAreaU1State>(m_stateAction);
    auto pro_action = static_cast<ProgressAreaU1::StateAction>(m_stateAction);
    auto pro_step   = static_cast<ProgressAreaU1::StateStep>(m_stateStep);
    m_progress->set_state_action(pro_action);
    m_progress->set_state_step(pro_step);
}


MaterialSlotU1::MaterialSlotU1(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_material_info{wxEmptyString, wxColour()}
    , m_state(MaterialSlotU1::EmptyMat)
    , m_unknow_bmp(create_scaled_bitmap("unknown_u1_mat", nullptr, 66))
    , m_empty_bmp(create_scaled_bitmap("empty_u1_mat", nullptr, 66))
    , m_empty_nozzle_bmp(create_scaled_bitmap("empty_u1_nozzle", nullptr, 66))
{
    SetMinSize(wxSize(FromDIP(72), FromDIP(72)));
    SetBackgroundColour(wxColour(255, 255, 255));
    connectEvent();
}

MaterialSlotU1::~MaterialSlotU1() {}

MaterialInfo MaterialSlotU1::get_material_info() { return m_material_info; }

MaterialSlotU1::SlotState MaterialSlotU1::get_slot_state() { return m_state; }

void MaterialSlotU1::set_slot_state(SlotState state)
{
    m_state = state;
    Refresh();
}

void MaterialSlotU1::set_slot_selected(bool selected)
{
    m_selected = selected;
    Refresh();
}

void MaterialSlotU1::connectEvent()
{
    Bind(wxEVT_PAINT, &MaterialSlotU1::paintEvent, this);
}

void MaterialSlotU1::paintEvent(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    auto      w = GetSize().GetWidth();
    auto      h = GetSize().GetHeight();

    constexpr int canvasWidth = 66;
    constexpr int canvasHeight = 66;

    // 选中画出外围的线框
    if (m_selected) {
        dc.SetPen(wxPen(wxColor(50, 141, 251), 1));
        dc.DrawRoundedRectangle(0, 0, w, h, 4.0);
    }

    switch (m_state) {
    case MaterialSlotU1::Complete: {
        dc.SetBrush(wxBrush(m_material_info.m_color));
        dc.SetPen(wxPen(wxColor(196, 196, 196), 1));
        dc.DrawRoundedRectangle(3, 3, canvasWidth, canvasHeight, 4.0); // 画背景

        wxFont font(FromDIP(12), wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
        dc.SetFont(font);
        render_name(m_material_info.m_name, dc);
        break;
    }
    case MaterialSlotU1::UnknownMat: {
        int iconX = (w - m_unknow_bmp.GetWidth()) / 2;
        int iconY = (h - m_unknow_bmp.GetHeight()) / 2;
        dc.DrawBitmap(m_unknow_bmp, iconX, iconY);
        break;
    }
    case MaterialSlotU1::EmptyMat: {
        int iconX = (w - m_empty_bmp.GetWidth()) / 2;
        int iconY = (h - m_empty_bmp.GetHeight()) / 2;
        dc.DrawBitmap(m_empty_bmp, iconX, iconY);
        break;
    }
    case MaterialSlotU1::EmptyNozzle: {
        int iconX = (w - m_empty_nozzle_bmp.GetWidth()) / 2;
        int iconY = (h - m_empty_nozzle_bmp.GetHeight()) / 2;
        dc.DrawBitmap(m_empty_nozzle_bmp, iconX, iconY);
        break;
    }
    default: break;
    }
}

// TODO: 抗锯齿
void MaterialSlotU1::render_name(const wxString& name, wxPaintDC& dc)
{
    dc.SetFont(::Label::Body_9);
    auto width          = GetSize().GetWidth();
    auto height         = GetSize().GetHeight();

    if (name.size() <= 4) {
        auto nameSize = dc.GetTextExtent(name);
        int  name_x   = (width - nameSize.x) / 2;
        int  name_y   = (height - nameSize.y) / 2;
        dc.DrawText(name, name_x, name_y); // 画名字
    } else {
        size_t   p       = name.find('-');
        size_t   pos     = (p < 4) ? p : 4;
        wxString name1   = name.substr(0, pos);
        wxString name2   = name.substr(pos, name.size() - 1);

        auto     size1   = dc.GetTextExtent(name1);
        auto     size2   = dc.GetTextExtent(name2);
        int      name1_x = (width - name1.size() * FromDIP(12)) / 2;
        int      name2_x = (width - name2.size() * FromDIP(12)) / 2;
        int      name1_y = (height - FromDIP(12));
        int      name2_y = (height - FromDIP(12));
        dc.DrawText(name1, name1_x, name1_y);
        dc.DrawText(name2, name2_x, name2_y);
    }
}

bool MaterialSlotU1::get_user_choices()
{
    bool update_info = false;
    // 确定对话框弹出位置
    wxPoint pos(GetScreenPosition().x + FromDIP(91), GetScreenPosition().y - FromDIP(47)); // 预计弹出位置
    wxSize  dialog_size(FromDIP(422), FromDIP(224));
    wxPoint finally_pos = MaterialDialog::calculate_pop_position(pos, dialog_size);
    int     state       = 0;
    if (m_state == SlotState::Complete) {
        state = MaterialDialog::InfoState::NameKnown | MaterialDialog::InfoState::ColorKnown;
    }
    MaterialDialog material_dialog(this, wxID_ANY, wxEmptyString, state, finally_pos, dialog_size);
    material_dialog.init_comboBox();
    if (m_material_info.m_color.IsOk()) {
        material_dialog.set_material_color(m_material_info.m_color);
    }
    if (!m_material_info.m_name.empty()) {
        material_dialog.set_material_name(m_material_info.m_name);
    }
    if (material_dialog.ShowModal() == wxID_OK) {
        m_material_info.m_name  = material_dialog.get_material_name();
        m_material_info.m_color = material_dialog.get_material_color();
        update_info             = true;
    }
    if (!m_material_info.m_name.empty() && m_material_info.m_color.IsOk()) {
        set_slot_state(SlotState::Complete);
    }
    Refresh();
    return update_info;
}

void MaterialSlotU1::set_material_info(const MaterialInfo& info)
{
    m_material_info = info;
    set_slot_state(SlotState::Complete);
}


MaterialSlotWgtU1::MaterialSlotWgtU1(
    wxWindow* parent, wxWindowID id, const int number, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name), m_slot_ID(number), m_cur_id(ComInvalidId)
{
    SetMinSize(wxSize(FromDIP(72), FromDIP(150)));
    SetBackgroundColour(wxColour(255, 255, 255));
    setup_layout(this, number);
    connectEvent();
}

MaterialSlotWgtU1::~MaterialSlotWgtU1() {}

MaterialInfo MaterialSlotWgtU1::get_material_info() { return m_material_slot->get_material_info(); }

int MaterialSlotWgtU1::get_slot_ID() { return m_slot_ID; }

void MaterialSlotWgtU1::set_slot_state(MaterialSlotU1::SlotState slot_state) { m_material_slot->set_slot_state(slot_state); }

bool MaterialSlotWgtU1::get_slot_editable()
{
    switch (m_material_slot->get_slot_state())
    {
    case MaterialSlotU1::Complete:
        return true;
    default:
        return false;
    }
}
void MaterialSlotWgtU1::set_slot_selected(bool selected) { m_material_slot->set_slot_selected(selected); }

void MaterialSlotWgtU1::setCurId(int curId) { m_cur_id = curId; }

void MaterialSlotWgtU1::modify_slot()
{ 
    m_material_slot->get_user_choices();
}

void MaterialSlotWgtU1::set_material_info(const MaterialInfo& info)
{
    m_material_slot->set_material_info(info);
}

void MaterialSlotWgtU1::setup_layout(wxWindow* parent, const int& number)
{
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    m_number = new wxStaticText(parent, wxID_ANY, wxString::Format(wxT("%i"), number), wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)),
                                wxALIGN_CENTRE_HORIZONTAL);
    m_material_slot = new MaterialSlotU1(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(72), FromDIP(72)));
    sizer->AddSpacer(FromDIP(15));
    sizer->Add(m_number, 0, wxLEFT | wxRIGHT, (GetSize().GetWidth() - m_number->GetSize().GetWidth()) / 2);
    sizer->AddSpacer(FromDIP(15));
    sizer->Add(m_material_slot, 0, wxLEFT | wxRIGHT, 0);
    SetSizer(sizer);
    Layout();
    sizer->Fit(this);
}

void MaterialSlotWgtU1::connectEvent()
{
    m_material_slot->Bind(wxEVT_LEFT_DOWN, &MaterialSlotWgtU1::OnMouseDown, this);
    m_number->Bind(wxEVT_LEFT_DOWN, &MaterialSlotWgtU1::on_asides_mouse_down, this);
    Bind(wxEVT_LEFT_DOWN, &MaterialSlotWgtU1::on_asides_mouse_down, this);
}

void MaterialSlotWgtU1::OnMouseDown(wxMouseEvent& event)
{
    // 未安装喷头 不可被点击
    if (m_material_slot->get_slot_state() == MaterialSlotU1::SlotState::EmptyNozzle)
        return;

    m_material_slot->set_slot_selected(true);
    wxCommandEvent click_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId());
    ProcessWindowEvent(click_event);
}

void MaterialSlotWgtU1::on_asides_mouse_down(wxMouseEvent& event)
{
    m_material_slot->set_slot_selected(false);

    ChangeU1SlotEvent clicked_event(CHANGE_U1_SLOT, nullptr);
    ProcessWindowEvent(clicked_event);
}


MaterialSlotAreaU1::MaterialSlotAreaU1(
    wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name)
    : wxWindow(parent, id, pos, size, style, name)
    , m_radio_slot(nullptr)
    , m_hasMatlStation(1)
    , m_nozzle_has_wire(1)
    , m_currentLoadSlot(-1)
    , m_currentSlot(0)
    , m_radio_changeable(true)
{
    SetBackgroundColour(wxColour(255, 255, 255));
    prepare_layout(this);
    setup_layout(this);
    connectEvent();
    s_self = this;

    //// for test
    //m_radio_slot = m_material_slots[0];
    //m_radio_slot->set_material_info({"PLA", wxColor(255, 255, 0)});
}

MaterialSlotAreaU1::~MaterialSlotAreaU1() {}

MaterialSlotAreaU1* MaterialSlotAreaU1::get_inst() { return s_self; }

MaterialSlotWgtU1* MaterialSlotAreaU1::get_radio_slot() { return m_radio_slot; }

void MaterialSlotAreaU1::abandon_selected()
{
    if (!m_radio_changeable)
        return;
    if (m_radio_slot) {
        m_radio_slot = nullptr;
        for (auto* slot : m_material_slots)
            slot->set_slot_selected(false);
    }
}

std::vector<wxColour> MaterialSlotAreaU1::get_all_material_color()
{
    std::vector<wxColour> color_all;
    color_all.reserve(m_material_slots.size());
    for (auto* slot : m_material_slots) {
        if (slot->get_material_info().m_color.IsOk()) {
            color_all.push_back(slot->get_material_info().m_color);
        }
    }
    return color_all;
}

void MaterialSlotAreaU1::setCurId(int curId)
{
    m_currentLoadSlot = -1;
    if (m_radio_slot) {
        m_radio_slot = nullptr;
    }
}

void MaterialSlotAreaU1::set_radio_changeable(bool enable)
{
    m_radio_changeable = enable;
}

// TODO：后续对接设备的时候再修改该函数逻辑 目前只保证编译通过
void MaterialSlotAreaU1::synchronize_printer_status(const com_dev_data_t& data)
{
    m_hasMatlStation = data.devDetail->hasMatlStation;
    synchronize_matl_station(data);
    unsigned short curr_pid = 0;
    if (data.connectMode == 0) {
        curr_pid = data.lanDevInfo.pid;
    } else if (data.connectMode == 1) {
        curr_pid = data.devDetail->pid;
    }
    if (curr_pid == C5 || curr_pid == C5P) {
        MaterialStation::set_printer_type((FFPrinterPid)curr_pid);
    } else {
        MaterialStation::set_printer_type(OTHER);
    }

    // 同步喷嘴传感器的状态
    m_nozzle_has_wire = data.devDetail->hasRightFilament;
    // 同步有色料线的连接的状态
    m_currentLoadSlot = data.devDetail->matlStationInfo.currentLoadSlot - 1;
    if (m_currentLoadSlot < -1 || m_currentLoadSlot > 3) {
        m_currentLoadSlot = -1;
    }
    if (m_hasMatlStation) {
        // 表示打印机接有四色材料站
        m_state_action = (StateAction) data.devDetail->matlStationInfo.stateAction;
        m_state_step   = (StateStep) data.devDetail->matlStationInfo.stateStep;
    } else {
        // 表示打印机未接有四色材料站
        m_state_action = (StateAction) data.devDetail->indepMatlInfo.stateAction;
        m_state_step   = (StateStep) data.devDetail->indepMatlInfo.stateStep;
    }
    switch (m_state_action) {
    case MaterialSlotAreaU1::StateAction::Free: {
        set_radio_changeable(true);
        break;
    }
    case MaterialSlotAreaU1::StateAction::SupplyWire:
    case MaterialSlotAreaU1::StateAction::WithdrawnWire:
    case MaterialSlotAreaU1::StateAction::Canceling:
    case MaterialSlotAreaU1::StateAction::Printing:
    case MaterialSlotAreaU1::StateAction::Busy: {
        set_radio_changeable(false);
        break;
    }
    case MaterialSlotAreaU1::StateAction::PrintingPaused: {
        set_radio_changeable(true);
        break;
    }
    default: break;
    }
    Refresh();
}

void MaterialSlotAreaU1::synchronize_matl_station(const com_dev_data_t& data)
{
    // 同步U1料盘的状态
    int                    slot_cnt  = data.devDetail->matlStationInfo.slotCnt;
    fnet_matl_slot_info_t* slotInfos = data.devDetail->matlStationInfo.slotInfos;
    if (!slotInfos) {
        return;
    }
    for (int i = 0; i < slot_cnt; ++i) {
        int      slotId        = (slotInfos + i)->slotId;
        int      hasFilament   = (slotInfos + i)->hasFilament; // 1 true, 0 false，四色状态下hasFilament表示料盘是否为空
        wxString materialName  = (slotInfos + i)->materialName;
        wxColour materialColor = (slotInfos + i)->materialColor;
        MaterialSlotU1::SlotState slot_state;
        MaterialInfo           material_info{wxEmptyString, wxColour()};
        if (hasFilament) {
            if (!materialName.empty() && materialColor.IsOk()) {
                slot_state            = MaterialSlotU1::SlotState::Complete;
                material_info.m_name  = materialName;
                material_info.m_color = materialColor;
            } else {
                slot_state = MaterialSlotU1::SlotState::UnknownMat;
            }
        } else {
            slot_state = MaterialSlotU1::SlotState::EmptyMat;
            if (m_material_slots[i] == m_radio_slot) {
                m_radio_slot = nullptr;
            }
        }
        m_material_slots[i]->set_slot_state(slot_state);
        m_material_slots[i]->set_material_info(material_info);
    }
    int currentSlot = data.devDetail->matlStationInfo.currentSlot - 1; // currentSlot是从1开始，m_currentSlot要求从0开始
    m_currentSlot   = (currentSlot < 0 || currentSlot > 3) ? 0 : currentSlot;
}

void MaterialSlotAreaU1::on_asides_mouse_down(wxMouseEvent& event)
{
    abandon_selected();
    wxCommandEvent clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId()); // 为了改变进丝按钮状态
    ProcessWindowEvent(clicked_event);
}

void MaterialSlotAreaU1::connectEvent()
{
    Bind(wxEVT_LEFT_DOWN, &MaterialSlotAreaU1::on_asides_mouse_down, this);
    for (auto& slot : m_material_slots) {
        Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialSlotAreaU1::slot_selected_event, this, slot->GetId());
    }
}

void MaterialSlotAreaU1::prepare_layout(wxWindow* parent)
{
    for (int i = 0; i < 4; ++i) {
        MaterialSlotWgtU1* material_slot = new MaterialSlotWgtU1(this, wxID_ANY, i + 1, wxDefaultPosition,
                                                             wxSize(FromDIP(72), FromDIP(150)));
        m_material_slots.push_back(material_slot);
    }
}

void MaterialSlotAreaU1::setup_layout(wxWindow* parent)
{
    // 布局四个料槽
    wxBoxSizer* slot_group_sizer = new wxBoxSizer(wxHORIZONTAL);
    slot_group_sizer->AddStretchSpacer();
    for (int i = 0; i < m_material_slots.size(); ++i) {
        slot_group_sizer->Add(m_material_slots[i], 0, wxEXPAND | wxTOP | wxBOTTOM, 0);
        m_material_slots[i]->Show();
        if (i < 3) {
            slot_group_sizer->AddSpacer(FromDIP(24));
        }
    }
    slot_group_sizer->AddStretchSpacer();

    this->SetSizer(slot_group_sizer);
    this->Layout();
    slot_group_sizer->Fit(this);
}

void MaterialSlotAreaU1::slot_selected_event(wxCommandEvent& event)
{
    if (!m_radio_changeable) {
        return;
    }
    // 当有某个槽被点击了
    for (auto& slot : m_material_slots) {
        if (event.GetId() == slot->GetId()) {
            m_radio_slot = slot;
        }
    }
    wxCommandEvent clicked_event(wxEVT_COMMAND_BUTTON_CLICKED, GetId()); // 为了改变进丝按钮状态
    ProcessWindowEvent(clicked_event);
}

void MaterialSlotAreaU1::modify_current_slot()
{
    if (m_radio_slot)
        m_radio_slot->modify_slot();
}

void MaterialSlotAreaU1::set_select_slot(MaterialSlotWgtU1* slot)
{
    for (auto* currentSlot : m_material_slots)
    {
        if (currentSlot == slot)
            currentSlot->set_slot_selected(true);
        else
            currentSlot->set_slot_selected(false);
    }
}


MaterialPanelU1::MaterialPanelU1(wxWindow* parent,
                            wxWindowID winid,
                            const wxPoint& pos,
                            const wxSize& size,
                            long style,
                            const wxString& name)
    : wxPanel(parent, winid, pos, size, style, name), m_cur_id(ComInvalidId)
{
    SetBackgroundColour(wxColour(248, 248, 248));
    // 启动布局后各种界面均为默认状态
    setup_layout(this);
    connectEvent();
}
MaterialPanelU1::~MaterialPanelU1() {}

void MaterialPanelU1::init_material_panel() {}
void MaterialPanelU1::setCurId(int curId)
{
    m_cur_id = curId;
    m_material_slot->setCurId(curId);
    update_modify_btn_state();
    m_tips_area->reset_printer_status();
}

void MaterialPanelU1::OnMouseDown(wxMouseEvent& event)
{
    m_modify_btn->Enable(false);
    m_material_slot->abandon_selected();
}

void MaterialPanelU1::OnChangeU1Slot(ChangeU1SlotEvent& event)
{
    MaterialSlotWgtU1* slot = event._currentSlot;
    if (slot) {
        m_material_slot->set_select_slot(slot);
        if (slot->get_slot_editable()) {
            m_modify_btn->Enable(true);
            return;
        }
    }
    else {
        m_material_slot->set_select_slot(slot);
        m_modify_btn->Enable(false);
    }

}


void MaterialPanelU1::setup_layout(wxWindow* parent)
{
    int width  = GetSize().GetWidth();
    int height = GetSize().GetHeight();
    // MaterialPanel整体水平布局
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    // MaterialPanel左半部分操作区
    wxBoxSizer* operate_area_sizer = new wxBoxSizer(wxVERTICAL);
    wxWindow*   operate_area       = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(474), height));
    operate_area->SetBackgroundColour(wxColour(255, 255, 255));

    // 左半部分操作区的上边的空白区域
    wxBoxSizer* switch_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   switch_group = new wxWindow(operate_area, wxID_ANY, wxDefaultPosition,
                                            wxSize(operate_area->GetSize().GetWidth(), FromDIP(40)));
    switch_group->SetBackgroundColour(wxColour(255, 255, 255));
    switch_group->Bind(wxEVT_LEFT_DOWN, &MaterialPanelU1::OnMouseDown, this);

    // 左半部分操作区的中间的料槽区
    m_material_slot = new MaterialSlotAreaU1(operate_area, wxID_ANY, wxDefaultPosition,
                                           wxSize(operate_area->GetSize().GetWidth(), FromDIP(173))); // 增加10
    m_material_slot->SetBackgroundColour(wxColour(255, 255, 255));
    // 左半部分操作区的下边的按钮区
    wxBoxSizer* btn_group_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxWindow*   button_group    = new wxWindow(operate_area, wxID_ANY, wxDefaultPosition,
                                               wxSize(operate_area->GetSize().GetWidth(), FromDIP(42)));
    button_group->SetBackgroundColour(wxColour(255, 255, 255));
    button_group->Bind(wxEVT_LEFT_DOWN, &MaterialPanelU1::OnMouseDown, this);

    m_modify_btn = new RoundedButton(button_group, wxID_ANY, true, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(76), FromDIP(34)));
    m_modify_btn->set_state_color(wxColour(65, 148, 136), RoundedButton::Normal);
    m_modify_btn->set_state_color(wxColour(65, 148, 136), RoundedButton::Hovered);
    m_modify_btn->set_state_color(wxColour(65, 148, 136), RoundedButton::Pressed);
    m_modify_btn->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_modify_btn->set_radius(4);
    m_modify_btn->set_bitmap(create_scaled_bitmap("modify_material", nullptr, 23));
    m_modify_btn->Enable(false);

    btn_group_sizer->AddStretchSpacer();
    btn_group_sizer->Add(m_modify_btn, 0, wxALIGN_CENTER, 0);
    btn_group_sizer->AddStretchSpacer();
    button_group->SetSizer(btn_group_sizer);
    button_group->Layout();
    // MaterialPanel左半部分操作区整体布局
    operate_area_sizer->Add(switch_group, 0, wxEXPAND | wxALL, 0);
    operate_area_sizer->Add(m_material_slot, 0, wxEXPAND | wxALL, 0);
    operate_area_sizer->Add(button_group, 0, wxEXPAND | wxALL, 0);
    operate_area->SetSizer(operate_area_sizer);
    operate_area->Layout();
    operate_area_sizer->Fit(operate_area);

    // MaterialPanel右半部分提示区
    m_tips_area = new TipsAreaU1(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(375), height));

    // 整体布局
    panel_sizer->Add(operate_area, 0, wxEXPAND | wxALL, 0);
    panel_sizer->AddSpacer(FromDIP(1));
    panel_sizer->Add(m_tips_area, 0, wxEXPAND | wxALL, 0);
    SetSizer(panel_sizer);
    Layout();
    panel_sizer->Fit(this);
}

void MaterialPanelU1::connectEvent()
{
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanelU1::on_modify_btn_clicked, this, m_modify_btn->GetId()); // TODO: 材料信息修改
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &MaterialPanelU1::on_slot_area_clicked, this, m_material_slot->GetId());
    m_material_slot->Bind(wxEVT_LEFT_DOWN, &MaterialPanelU1::OnMouseDown, this);
    m_material_slot->Bind(CHANGE_U1_SLOT, &MaterialPanelU1::OnChangeU1Slot, this);

    MultiComMgr::inst()->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &MaterialPanelU1::onComDevDetailUpdate, this);
}

// TODO: 更新材料信息修改按钮状态
void MaterialPanelU1::update_modify_btn_state()
{
    auto radio_slot        = m_material_slot->get_radio_slot();
    if (!radio_slot) {
        m_modify_btn->Enable(false);
        return;
    }

    bool modify_enable = radio_slot->get_slot_editable();

    // TODO：后续对接设备的时候再修改此处逻辑 目前只保证编译通过
    // 查看打印机是否空闲或打印暂停
    //bool free           = m_tips_area->get_tips_area_state() == TipsAreaU1::TipsAreaU1State::Free;
    //bool printingPaused = m_tips_area->get_tips_area_state() == TipsAreaU1::TipsAreaU1State::PrintingPaused;
    //if (!free && !printingPaused) {
    //    modify_enable    = false;
    //} else if (printingPaused) {
    //    modify_enable    = !m_material_slot->hasMatlStation();
    //}

    m_modify_btn->Enable(modify_enable);
}

void MaterialPanelU1::on_modify_btn_clicked(wxCommandEvent& event)
{
    m_material_slot->modify_current_slot();
}

void MaterialPanelU1::on_slot_area_clicked(wxCommandEvent& event)
{
    update_modify_btn_state();
}

// TODO：根据接口信息数据更新打印机信息 如料盘信息 颜色信息 喷嘴信息 打印机状态信息等
void MaterialPanelU1::onComDevDetailUpdate(ComDevDetailUpdateEvent& event)
{
    event.Skip();
    if (m_cur_id != event.id)
        return;
    const com_dev_data_t& data = MultiComMgr::inst()->devData(m_cur_id);
    // 同步提示区的打印机状态
    m_tips_area->Synchronize_printer_status(data);
    // 同步料槽区的打印机状态
    m_material_slot->synchronize_printer_status(data);
    // 同步modify按钮的状态
    update_modify_btn_state();
}


MaterialStation::MaterialStation(wxWindow*       parent,
                                 wxWindowID      winid,
                                 const wxPoint&  pos,
                                 const wxSize&   size,
                                 long            style,
                                 const wxString& name)
    : wxPanel(parent, winid, pos, size, style, name)
{
    SetBackgroundColour(wxColour(255, 255, 255));

    create_panel(this);
}

MaterialStation::~MaterialStation() {} 

void MaterialStation::create_panel(wxWindow* parent)
{
    int         width  = GetSize().GetWidth();
    int         height = GetSize().GetHeight();
    wxBoxSizer* sizer                 = new wxBoxSizer(wxVERTICAL);
    // 材料站标题布局
    wxBoxSizer* bSizer_material_title = new wxBoxSizer(wxHORIZONTAL);
    m_material_title                  = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(35)));
    m_material_title->SetBackgroundColour(wxColour(248, 248, 248));
    m_staticText_title = new wxStaticText(m_material_title, wxID_ANY, _L("IFS"));
    m_staticText_title->SetForegroundColour(wxColour(51, 51, 51));
    bSizer_material_title->Add(m_staticText_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(11));
    bSizer_material_title->Add(0, 0, 1, wxEXPAND, 0);
    m_material_title->SetSizer(bSizer_material_title);
    m_material_title->Layout();

    //标题和内容中间的间隔
    wxWindow* separator_middle = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(3)));
    separator_middle->SetBackgroundColour(wxColour(240, 240, 240));
    // 材料站内容布局
    m_material_switch_panel = new wxSimplebook(parent, wxID_ANY, wxDefaultPosition, wxSize(width, FromDIP(339)));

    m_material_panel = new MaterialPanel(m_material_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_U1_panel       = new MaterialPanelU1(m_material_switch_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);

    m_material_switch_panel->AddPage(m_material_panel, wxEmptyString, true);
    m_material_switch_panel->AddPage(m_U1_panel, wxEmptyString, false);

    //整体布局
    sizer->Add(m_material_title, 0, wxEXPAND | wxALL, 0);
    sizer->Add(separator_middle, 0, wxEXPAND | wxALL, 0);
    sizer->Add(m_material_switch_panel, 0, wxEXPAND | wxALL, 0);
    SetSizer(sizer);
    Layout();
    Fit();
}

wxPanel* MaterialStation::GetPrintTitlePanel() { return m_material_title; }

void MaterialStation::show_material_panel(bool isShow) 
{ 
    m_material_switch_panel->SetSelection(0);
    m_material_switch_panel->Show(isShow);
}

void MaterialStation::show_material_panel(int pid)
{
    bool show = false;
    int selection = 0;
    if (pid == AD5X || pid == GUIDER_4 || pid == GUIDER_4_PRO) {
        selection = 0;
        show = true;
        MaterialStation::set_printer_type((FFPrinterPid)pid);
    } 
    else if (pid == C5 || pid == C5P) {
        selection = 1;
        show = true;
        MaterialStation::set_printer_type((FFPrinterPid)pid);
    }
    else {
        show = false;
        MaterialStation::set_printer_type((FFPrinterPid)pid);
    }

    m_material_switch_panel->SetSelection(selection);
    m_material_switch_panel->Show(show);
}

void MaterialStation::setCurId(int curId)
{
    auto type = MaterialStation::get_printer_type();
    if (type == AD5X || type == GUIDER_4 || type == GUIDER_4_PRO)
    {
        m_material_panel->setCurId(curId);
    }
    else if (type == C5 || type == C5P)
    {
        m_U1_panel->setCurId(curId);
    }
}

void MaterialStation::set_printer_type(FFPrinterPid type) { s_PrinterType = type; }

FFPrinterPid MaterialStation::get_printer_type() { return s_PrinterType; }

CustomOwnerDrawnComboBox::CustomOwnerDrawnComboBox(wxWindow*          parent,
                                                   wxWindowID         id,
                                                   const wxString&    value,
                                                   const wxPoint&     pos,
                                                   const wxSize&      size,
                                                   int                n,
                                                   const wxString     choices[],
                                                   long               style,
                                                   const wxValidator& validator,
                                                   const wxString&    name)
    : wxOwnerDrawnComboBox(parent, id, value, pos, size, n, choices, style, validator, name)
    , m_up(create_scaled_bitmap("arrow_up", nullptr, 5)) 
    , m_down(create_scaled_bitmap("arrow_down", nullptr, 5))
    , m_isExpanded(false)
{
    connectEvent();
}

wxCoord CustomOwnerDrawnComboBox::OnMeasureItem(size_t item) const
{
    return FromDIP(34); // 每个选项的高度; 
}

void CustomOwnerDrawnComboBox::OnDrawItem(wxDC& dc, const wxRect& rect, int item, int flags) const
{
    wxColour txt_color(51, 51, 51);
    wxColour background_color(255, 255, 255);
    if (flags & wxODCB_PAINTING_SELECTED) {
        txt_color        = wxColour(255, 255, 255);
        background_color = wxColour(50, 141, 251); // 悬停时的背景色
    }
    dc.SetBrush(wxBrush(background_color));
    dc.SetPen(wxPen(background_color, 0));
    dc.DrawRectangle(rect);

    wxString text = GetString(item);
    dc.SetTextBackground(txt_color);
    dc.DrawText(text, rect.x + FromDIP(17), rect.y + FromDIP(7));

}

void CustomOwnerDrawnComboBox::paint_expanded_border(wxPaintDC& dc, wxRect& rect)
{
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    int     radius       = 6;
    wxPoint left_top = rect.GetLeftTop();
    wxPoint right_top = rect.GetRightTop();
    wxPoint left_bottom = rect.GetLeftBottom();
    wxPoint right_bottom = rect.GetRightBottom();

    gc->SetPen(wxPen(wxColour(193, 193, 193), 1));                                                // 边框颜色和宽度
    gc->SetBrush(wxBrush(wxColour(255, 255, 255)));                                               // 背景颜色
    gc->DrawRoundedRectangle(rect.x +1, rect.y + 1, rect.width - 2, rect.height - 2, 6);                 //先画一个圆角矩形
    gc->SetPen(wxPen(wxColour(255, 255, 255), 0)); 
    gc->SetBrush(wxBrush(wxColour(255, 255, 255))); 
    gc->DrawRectangle(rect.x, rect.y + rect.height / 2, rect.width, rect.height / 2);              //绘制圆角矩形下半部分为白色以擦除

    gc->SetPen(wxPen(wxColour(193, 193, 193), 1));                                    // 边框颜色和宽度
    gc->SetBrush(wxBrush(wxColour(255, 255, 255)));                                   // 背景颜色
    gc->StrokeLine(left_bottom.x, left_bottom.y, right_bottom.x, right_bottom.y);//先画出底边
    gc->StrokeLine(left_top.x + 1, left_top.y + radius, left_bottom.x + 1, left_bottom.y);      // 画出左边
    gc->StrokeLine(right_top.x, right_top.y + radius, right_bottom.x, right_bottom.y);//画右边
}

void CustomOwnerDrawnComboBox::paint_collapse_border(wxPaintDC& dc, wxRect& rect)
{
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    gc->SetPen(wxPen(wxColour(193, 193, 193), 1));  // 边框颜色和宽度
    gc->SetBrush(wxBrush(wxColour(255, 255, 255))); // 背景颜色
    gc->DrawRoundedRectangle(rect.x + 1, rect.y + 1, rect.width -  2, rect.height - 2, 6);
}

void CustomOwnerDrawnComboBox::paintEvent(wxPaintEvent& event) 
{
    // 绘制圆角边框
    wxPaintDC dc(this);
    wxRect    rect = GetClientRect();
    if (m_isExpanded) {
        paint_expanded_border(dc, rect);
    } else {
        paint_collapse_border(dc, rect);
    }
    // 绘制内部文本
    dc.SetTextForeground(wxColour(51, 51, 51)); // 文本颜色
    dc.DrawText(GetValue(), rect.x + FromDIP(17), rect.y + FromDIP(7)); // 文本位置
    // 绘制箭头
    int symmetry_x = rect.GetRight() - FromDIP(15); //上下箭头对称轴x坐标
    int symmetry_y = (rect.GetHeight() - FromDIP(6)) / 2;   //上下箭头组成矩形的水平平分线y坐标
    wxPoint left_top(symmetry_x - FromDIP(6), symmetry_y - FromDIP(2));
    if (m_isExpanded) {
        dc.DrawBitmap(m_up, left_top);
    } else {
        dc.DrawBitmap(m_down, left_top);
    }
    
}

void CustomOwnerDrawnComboBox::connectEvent()
{ 
    Bind(wxEVT_PAINT, &CustomOwnerDrawnComboBox::paintEvent, this); 
    Bind(wxEVT_COMBOBOX_DROPDOWN, &CustomOwnerDrawnComboBox::OnDropdown, this);
    Bind(wxEVT_COMBOBOX_CLOSEUP, &CustomOwnerDrawnComboBox::OnCloseUp, this);
}

void CustomOwnerDrawnComboBox::OnDropdown(wxCommandEvent& event)
{
    m_isExpanded = true;
    Refresh();
}

void CustomOwnerDrawnComboBox::OnCloseUp(wxCommandEvent& event)
{
    m_isExpanded = false;
    Refresh();
}

FFNozzles* FFNozzles::s_self = nullptr;

FFNozzles* FFNozzles::get_inst() { return s_self; }

FFNozzles::FFNozzles(wxWindow* parent) : 
    wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{ 
    SetMinSize(wxSize(FromDIP(680), FromDIP(255)));
    SetSize(wxSize(FromDIP(680), FromDIP(255))); 
    SetBackgroundColour(*wxWHITE);
    auto sizer = new wxBoxSizer(wxVERTICAL);
    auto title_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(49)));
    title_panel->SetBackgroundColour(wxColour(248, 248, 248));
    auto title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto title       = new Label(title_panel, _L("Extruder & Filament"));
    title->SetBackgroundColour(wxColour(248, 248, 248));
    title_sizer->Add(title, 0, wxLEFT | wxALIGN_CENTER, FromDIP(14));
    title_sizer->AddStretchSpacer();
    title_panel->SetSizer(title_sizer);
    title_sizer->Fit(title_panel);
    title_panel->Layout();
    sizer->Add(title_panel, 0, wxALL | wxEXPAND, 0);
    sizer->AddSpacer(FromDIP(26));
    auto nozzle_panel = new wxPanel(this);
    nozzle_panel->SetBackgroundColour(*wxWHITE);
    auto nozzle_sizer = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < m_count; i++) {
        auto noz = new FFNozzle(nozzle_panel, i + 1, wxSize(FromDIP(77), FromDIP(66)));
        noz->Bind(wxEVT_LEFT_DOWN, [=](wxMouseEvent& event) { 
            if (!noz->FlashforgeEnabled() || noz->IsBusy()) {
                return;
            }
            for (auto other_noz : m_nozzles) {
                if (other_noz != noz) {
                    other_noz->Select(false);
                }
            }
            m_current_index = noz->GetIndex() - 1;
            if (m_edit_btn) {
                m_edit_btn->Enable(true);
            }
            event.Skip();
        });
        nozzle_sizer->Add(noz, 0, wxALL, 0);
        if (i != m_count - 1) {
            nozzle_sizer->AddSpacer(FromDIP(24));
        }
        m_nozzles.emplace_back(noz);
    }
    nozzle_panel->SetSizerAndFit(nozzle_sizer);
    nozzle_panel->Layout();
    sizer->Add(nozzle_panel, 0, wxALL | wxALIGN_CENTER, 0);
    sizer->AddSpacer(FromDIP(16));
    auto info_sizer = new wxBoxSizer(wxHORIZONTAL);
    ScalableBitmap info_icon(this, "info", 13);
    auto           info_bmp = new wxStaticBitmap(this, wxID_ANY, info_icon.bmp(), wxDefaultPosition, FromDIP(wxSize(13, 13)));
    info_sizer->Add(info_bmp, 0, wxALL, 0);
    info_sizer->AddSpacer(FromDIP(3));
    m_info_label = new Label(this, Label::Body_13, "");
    info_sizer->Add(m_info_label, 0, wxALL, 0);
    sizer->Add(info_sizer, 0, wxALL | wxALIGN_CENTER, 0);
    sizer->AddSpacer(FromDIP(10));
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_edit_btn = new RoundedButton(this, wxID_ANY, true, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(113), FromDIP(32)));
    m_edit_btn->SetMinSize(wxSize(FromDIP(113), FromDIP(32)));
    m_edit_btn->set_state_color(wxColour(65, 148, 136), RoundedButton::Normal);
    m_edit_btn->set_state_color(wxColour(101, 167, 158), RoundedButton::Hovered);
    m_edit_btn->set_state_color(wxColour(26, 134, 118), RoundedButton::Pressed);
    m_edit_btn->set_state_color(wxColour(221, 221, 221), RoundedButton::Inavaliable);
    m_edit_btn->set_radius(4);
    m_edit_btn->set_bitmap(create_scaled_bitmap("edit_white_btn", nullptr, 18));
    m_edit_btn->Enable(false);
    m_edit_btn->Bind(wxEVT_KILL_FOCUS, [=](wxFocusEvent& event) { 
        Refresh();
        return;
    });
    m_edit_btn->Bind(wxEVT_BUTTON, [=](wxCommandEvent& event) {
        // 确定对话框弹出位置
        wxPoint pos(GetScreenPosition().x + FromDIP(91), GetScreenPosition().y - FromDIP(47)); // 预计弹出位置
        wxSize  dialog_size(FromDIP(422), FromDIP(224));
        wxPoint finally_pos = MaterialDialog::calculate_pop_position(pos, dialog_size);
        int state = MaterialDialog::InfoState::NameKnown | MaterialDialog::InfoState::ColorKnown;
        MaterialDialog material_dialog(this, wxID_ANY, wxEmptyString, state, finally_pos, dialog_size);
        material_dialog.init_comboBox();
        if (m_current_index == -1) {
            return;
        }
        auto noz = m_nozzles[m_current_index];
        material_dialog.set_material_color(noz->GetMaterialColor());
        if (!noz->GetMaterialName().IsEmpty()) {
            material_dialog.set_material_name(noz->GetMaterialName());
        }
        if (material_dialog.ShowModal() == wxID_OK) {
            noz->SetMaterialInfo(m_current_index + 1, material_dialog.get_material_name(), material_dialog.get_material_color());
            //通讯改变喷嘴状态
            send_config_command();
        }
    });
    btn_sizer->Add(m_edit_btn, 0, wxALL, 0);
    sizer->Add(btn_sizer, 0, wxALL | wxALIGN_CENTER, 0);
    sizer->AddSpacer(FromDIP(40));
    SetSizer(sizer);
    sizer->Fit(this);
    Layout();
    MultiComMgr::inst()->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &FFNozzles::onComDevDetailUpdate, this);
    s_self = this;
}

void FFNozzles::SetOffline() 
{ 
    for (int i = 0; i < m_count; i++) {
        m_nozzles[i]->SetFlashforgeEnabled(false);
    }
    m_edit_btn->Enable(false);
}

void FFNozzles::SetCurId(int curId) 
{ 
    m_cur_id = curId; 
    if (m_cur_id != -1) {
        ComDevDetailUpdateEvent event(COM_DEV_DETAIL_UPDATE_EVENT, m_cur_id, 0, MultiComMgr::inst()->devData(m_cur_id).devDetail);
        onComDevDetailUpdate(event);
    }
    int pid = FFUtils::getPid(curId);
    if (pid == C5 || pid == C5P) {
        m_info_label->SetLabel(_L("Select an extruder to edit"));
    } else {
        m_info_label->SetLabel(_L("Edit or load filament after selecting an extruder"));
    }
    Layout();
}

void FFNozzles::SetCurState(bool isIdle)
{
    m_edit_btn->Enable(isIdle);
    for (int i = 0; i < m_nozzles.size(); i++) {
        m_nozzles[i]->SetBusy(!isIdle);
    }
}

std::vector<wxColour> FFNozzles::GetAllColours() 
{ 
    std::vector<wxColour> colors;
    for (auto noz : m_nozzles) {
        colors.emplace_back(noz->GetMaterialColor());
    }
    return colors;
}

void FFNozzles::onComDevDetailUpdate(ComDevDetailUpdateEvent& event) 
{
    event.Skip();
    if (m_cur_id != event.id) {
        return;
    }
    const com_dev_data_t& data = MultiComMgr::inst()->devData(m_cur_id);
    int                    slot_cnt  = data.devDetail->matlStationInfo.slotCnt;
    fnet_matl_slot_info_t* slotInfos = data.devDetail->matlStationInfo.slotInfos;
    if (!slotInfos) {
        return;
    }
    m_count = slot_cnt;
    for (int i = 0; i < slot_cnt; ++i) {
        int                    slotId        = (slotInfos + i)->slotId;
        int                    hasFilament   = (slotInfos + i)->hasFilament; // 1 true, 0 false，四色状态下hasFilament表示料盘是否为空
        wxString               materialName  = (slotInfos + i)->materialName;
        wxColour               materialColor = (slotInfos + i)->materialColor;
        auto                   noz = m_nozzles[i];
        if (hasFilament) {
            noz->SetFlashforgeEnabled(true);
            if (!materialName.empty() && materialColor.IsOk()) {
                noz->SetMaterialInfo(slotId, materialName, materialColor);
            }
        } else {
            noz->SetFlashforgeEnabled(false);
        }
    }
}

bool FFNozzles::send_config_command() 
{
    auto        noz = m_nozzles[m_current_index];
    std::string name(noz->GetMaterialName().ToStdString());
    std::string color_str(noz->GetMaterialColor().GetAsString(wxC2S_HTML_SYNTAX).c_str());
    ComCommand* comCommand = new ComMatlStationConfig(noz->GetIndex(), name, color_str);
    return Slic3r::GUI::MultiComMgr::inst()->putCommand(m_cur_id, comCommand);
}

FFNozzle::FFNozzle(wxWindow* parent, int index, wxSize size) : 
    wxPanel(parent, wxID_ANY, wxDefaultPosition, size), m_index(index)
{ 
    int h = size.x * 8 / 7;
    SetSize(wxSize(size.x, h));
    SetMinSize(wxSize(size.x, h));
    SetMaxSize(wxSize(size.x, h));
    SetBackgroundColour(*wxWHITE);
    SetDoubleBuffered(true);
    m_selected_image = ScalableBitmap(this, "nozzle_selected", 105);
    Bind(wxEVT_PAINT, &FFNozzle::paintEvent, this);
    Bind(wxEVT_LEFT_DOWN, [=](wxMouseEvent& event) { 
        if (!m_enabled || m_busy) {
            return;
        }
        Select(true);
        event.Skip();
    });
}

void FFNozzle::Select(bool flag) 
{ 
    m_selected = flag;
    Refresh();
}

bool FFNozzle::IsSelected() { return m_selected; }

bool FFNozzle::FlashforgeEnabled() 
{
    return m_enabled; 
}

void FFNozzle::SetFlashforgeEnabled(bool flag)
{ 
    m_enabled = flag; 
    Refresh();
}

bool FFNozzle::IsBusy() { return m_busy; }

void FFNozzle::SetBusy(bool flag) 
{ 
    m_busy = flag;
}

int FFNozzle::GetIndex() { return m_index; }

void FFNozzle::SetMaterialInfo(int index, wxString name, wxColour color)
{
    this->m_index          = index;
    this->m_material_name = name;
    this->m_material_color = color;
    Refresh();
}

wxColour FFNozzle::GetMaterialColor() { return m_material_color; }

wxString FFNozzle::GetMaterialName() { return m_material_name; }

void FFNozzle::paintEvent(wxPaintEvent& event) 
{
    wxBufferedPaintDC                  dc(this);
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (gc == nullptr) {
        return;
    }
    auto rect = GetClientRect();
    gc->SetBrush(*wxWHITE);
    gc->DrawRectangle(0, 0, rect.width, rect.height);
    auto paint_rect = rect.Inflate(FromDIP(-3));
    rect.Inflate(FromDIP(3));
    int  rect_height = paint_rect.height * 22 / (22 + 5);
    if (m_enabled && m_selected) {
        gc->DrawBitmap(m_selected_image.bmp(), 0, 0, rect.width, rect.height);
    }
    if (m_enabled) {
        gc->SetBrush(m_material_color);
    } else {
        gc->SetBrush(wxColour("#E8E8E8"));
    }
    gc->SetPen(wxPen(wxColour("#D9D9D9"), 1));
    gc->DrawRoundedRectangle(paint_rect.x, paint_rect.y, paint_rect.width, rect_height, paint_rect.width / 24);
    wxGraphicsPath triangle = gc->CreatePath();
    triangle.MoveToPoint(paint_rect.width / 2 - paint_rect.width / 6 + paint_rect.x, rect_height + paint_rect.y);
    triangle.AddLineToPoint(paint_rect.width / 2 + paint_rect.width / 6 + paint_rect.x, rect_height + paint_rect.y);
    triangle.AddLineToPoint(paint_rect.width / 2 + paint_rect.x, paint_rect.height + paint_rect.y - 2);
    triangle.CloseSubpath();
    gc->DrawPath(triangle);
    dc.SetFont(Label::sysFont(ToDIP(paint_rect.width / 5), false));
    if (m_enabled) {
        auto luminance = m_material_color.GetLuminance();
        dc.SetTextForeground(luminance > 0.6 ? *wxBLACK : *wxWHITE);
    } else {
        dc.SetTextForeground(wxColour("#A1A1A1"));
    }
    int sper = rect_height / 5;
    int      text_y         = paint_rect.y + sper;
    wxString index_str = wxString::Format("%d", m_index);
    auto     index_str_size = dc.GetTextExtent(index_str);
    dc.DrawText(index_str, (paint_rect.width - index_str_size.x) / 2 + paint_rect.x, text_y + paint_rect.y);
    text_y += index_str_size.y + FromDIP(8);
    wxString name_str = m_enabled ? m_material_name : "/";
    auto name_str_size = dc.GetTextExtent(name_str);
    dc.DrawText(name_str, (paint_rect.width - name_str_size.x) / 2 + paint_rect.x, text_y + paint_rect.y);
}

void FFNozzle::setMask(const wxString& mapName, bool like)
{
    (void) like;
    bool b = FFUtils::matchMaterialName(m_material_name, mapName);
    SetFlashforgeEnabled(!m_material_name.empty() && b);
    Refresh();
    Update();
}

} // namespace GUI
} // namespace Slic3r
