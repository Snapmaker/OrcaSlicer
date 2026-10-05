#ifndef slic3r_GUI_NewTempInput_hpp_
#define slic3r_GUI_NewTempInput_hpp_

#include "../wxExtensions.hpp"
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/simplebook.h>
#include <wx/odcombo.h>
#include "SwitchButton.hpp"
#include "StaticBox.hpp"
#include "Label.hpp"
#include "Button.hpp"
#include "FFButton.hpp"
#include "ComboBox.hpp"
#include "slic3r/GUI/TitleDialog.hpp"
#include "slic3r/GUI/FlashForge/FlashNetwork.h"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/GUI/FlashForge/ComCommand.hpp"
#include "slic3r/GUI/FFUtils.hpp"

namespace Slic3r { namespace GUI {

wxDECLARE_EVENT(EVT_CANCEL_PRINT_CLICKED, wxCommandEvent);
wxDECLARE_EVENT(EVT_CONTINUE_PRINT_CLICKED, wxCommandEvent);
wxDECLARE_EVENT(EVT_HIDE_PANEL, wxCommandEvent);

class PosCtrlButton : public Button
{
public:
    PosCtrlButton(wxWindow* parent, int* step = nullptr);
    void SetCurId(int curId);

protected:
    void render(wxDC& dc);

private:
    int           m_cur_id = -1;
    int           m_dirState[4];
    wxRect        m_dirRect[4];
    int           m_mouse_down{-1};
    int           m_arrow_size;
    const std::string  m_arrows[4]  = {"up", "left", "down", "right"};
    const wxPoint m_axisDir[4] = {
        wxPoint(1, 1),
        wxPoint(0, -1),
        wxPoint(1, -1),
        wxPoint(0, 1),
    };
    int* m_step{nullptr};
};

class CancelPrint : public TitleDialog
{
public:
    CancelPrint(const wxString& info, const wxString& leftBtnTxt, const wxString& rightBtnTxt, const wxString& title = "Cancel Print");

protected:
    void on_dpi_changed(const wxRect& suggested_rect) {};

private:
    wxBoxSizer*   m_sizer_main{nullptr};
    wxStaticText* m_info{nullptr};
    FFButton*     m_confirm_btn{nullptr};
    FFButton*     m_cancel_btn{nullptr};
};

class ShowTip : public TitleDialog
{
public:
    ShowTip(const wxString& info);
    void SetLabel(const wxString& info);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) {};

private:
    wxBoxSizer*   m_sizer_main{nullptr};
    wxStaticText* m_info{nullptr};
};

class NewTempInput : public StaticBox
{
    bool hover;

    bool           m_read_only{ false };
    wxSize         labelSize;
    ScalableBitmap normal_icon;

    StateColor label_color;
    StateColor text_color;

    wxTextCtrl* text_ctrl;
    wxStaticText* warning_text;

    int curr_temp = INT_MAX;
    int target_temp = INT_MAX;
    int  max_temp = 0;
    int  min_temp = 0;
    bool warning_mode = false;
    int  m_nozzle_index{ -1 };

    int              padding_left = 0;
    static const int TempInputWidth = 200;
    static const int TempInputHeight = 50;

public:
    enum WarningType {
        WARNING_TOO_HIGH,
        WARNING_TOO_LOW,
        WARNING_UNKNOWN,
    };

    NewTempInput();

    NewTempInput(wxWindow* parent,
        wxString       normal_icon = "",
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long           style = 0);

public:
    void Create(wxWindow* parent,
        wxString       text,
        wxString       label = "",
        wxString       normal_icon = "",
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long           style = 0);

    wxPopupTransientWindow* wdialog{ nullptr };
    int                     temp_type;
    bool                    m_target_temp_enable = false;

    void SetTagTemp(int temp, bool notifyModify = false);
    void SetCurrTemp(int temp, bool notifyModify = false);

    bool AllisNum(std::string str);
    void SetFinish();
    void Warning(bool warn, WarningType type = WARNING_UNKNOWN);
    void SetNozzleIndex(int index);

    void SetReadOnly(bool ro) { m_read_only = ro; }

    void SetMaxTemp(int temp);
    void SetMinTemp(int temp);

    void SetNormalIcon(wxString normalIcon);
    void EnableTargetTemp(bool visible);

    int GetType() { return temp_type; }

    int GetTagTemp();
    int GetCurrTemp() { return curr_temp; }
    int      get_max_temp() { return max_temp; }
    void     SetLabel(const wxString& label);

    void SetTextColor(StateColor const& color);

    void SetLabelColor(StateColor const& color);

    virtual void Rescale();

    virtual bool Enable(bool enable = true) override;

    virtual void SetMinSize(const wxSize& size) override;

    wxTextCtrl* GetTextCtrl() { return text_ctrl; }

    wxTextCtrl const* GetTextCtrl() const { return text_ctrl; }

protected:
    virtual void OnEdit() {}

    virtual void DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

    void DoSetToolTipText(wxString const& tip) override;

private:
    void lostFocusmodifyTemp();
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void mouseEnterWindow(wxMouseEvent& event);
    void mouseLeaveWindow(wxMouseEvent& event);

    DECLARE_EVENT_TABLE()
};

class IconText : public wxPanel
{
public:
    IconText();
    IconText(wxWindow*      parent,
             wxString       icon     = "",
             int            iconSize = 12,
             wxString       text     = "",
             int            textSize = 12,
             const wxPoint& pos      = wxDefaultPosition,
             const wxSize&  size     = wxDefaultSize,
             long           style    = wxTAB_TRAVERSAL);
    ~IconText() {};
    void create_panel(wxWindow* parent, wxString icon, int iconSize, wxString text, int textSize);
    void setText(wxString text);
    void setTextForegroundColour(wxColour colour);
    void setTextBackgroundColor(wxColour colour);

private:
    wxBitmap        m_icon;
    wxStaticBitmap* m_icon_staticbitmap{nullptr};
    Label*          m_text_ctrl{nullptr};
};

class MachineComboBox : public wxOwnerDrawnComboBox
{
public:
    MachineComboBox(wxWindow*       parent,
                    wxWindowID      id,
                    const wxString& value     = wxEmptyString,
                    const wxPoint&  pos       = wxDefaultPosition,
                    const wxSize&   size      = wxDefaultSize,
                    int             n         = 0,
                    const wxString  choices[] = NULL);

protected:
    void OnPaint(wxPaintEvent& evt);
    void OnDrawItem(wxDC& dc, const wxRect& rect, int item, int flags) const override;
    wxCoord OnMeasureItem(size_t item) const override;
    void    OnMouseEnter(wxMouseEvent& evt);
    void    OnMouseLeave(wxMouseEvent& evt);
    void    OnDropDown(wxCommandEvent& evt);
    void    OnCloseUp(wxCommandEvent& evt);

private:
    StateColor m_bgColor;
    StateColor m_textColor;
    StateColor m_borderColor;
    wxColour m_dropdownBgColor;
    wxColour m_itemSelectedBg;
    wxColour m_itemSelectedText;
    int      m_cornerRadius;
    int      m_itemHeight;
    bool     m_isHover;
    bool     m_isDropped;

    ScalableBitmap m_arrowUp;
    ScalableBitmap m_arrowDown;
};

class MachineIconCombo : public wxPanel
{
public:
    MachineIconCombo(wxWindow*       parent,
                     wxWindowID      id,
                     const wxString& icon      = wxEmptyString,
                     int             iconSize  = 16,
                     const wxPoint&  pos       = wxDefaultPosition,
                     const wxSize&   size      = wxDefaultSize,
                     int             n         = 0,
                     const wxString  choices[] = NULL);
    MachineComboBox* combobox();

private:
    ScalableBitmap   m_icon;
    MachineComboBox* m_combobox;
};

class IconBottonText : public wxPanel
{
public:
    IconBottonText(wxWindow*      parent,
                   wxString       icon          = "",
                   int            iconSize      = 12,
                   wxString       text          = "",
                   int            textSize      = 12,
                   wxString       secondIcon    = "",
                   wxString       thirdIcon     = "",
                   bool           positiveOrder = true,
                   const wxPoint& pos           = wxDefaultPosition,
                   const wxSize&  size          = wxDefaultSize,
                   long           style         = wxTAB_TRAVERSAL);
    ~IconBottonText() {};
    void     create_panel(wxWindow* parent,
                          wxString  icon,
                          int       iconSize,
                          wxString  text,
                          int       textSize,
                          wxString  secondIcon    = "",
                          wxString  thirdIcon     = "",
                          bool      positiveOrder = true);
    void     setLimit(double min, double max);
    void     setAdjustValue(double value);
    wxString getTextValue();
    void     setText(wxString text);
    void     setCurValue(double value);
    void     setPoint(int value);
    void     checkValue();

private:
    void onTextChange(wxCommandEvent& event);
    void onTextFocusOut(wxFocusEvent& event);
    void onDecBtnClicked(wxMouseEvent& event);
    void onIncBtnClicked(wxMouseEvent& event);

private:
    double        m_min;
    double        m_max;
    double        m_adjust_value;
    double        m_cur_value;
    int           m_point = 0;
    wxBitmap      m_icon;
    FFPushButton* m_dec_btn{nullptr};
    FFPushButton* m_inc_btn{nullptr};
    wxTextCtrl*   m_text_ctrl{nullptr};
    wxStaticText* m_unitLabel{nullptr};
};

class StartFiltering : public wxPanel
{
public:
    StartFiltering(wxWindow* parent);
    ~StartFiltering() {};
    void setCurId(int curId);
    void create_panel(wxWindow* parent);
    void setBtnState(bool internalOpen, bool externalOpen);

private:
    void onAirFilterToggled(wxCommandEvent& event);

private:
    SwitchButton* m_internal_circulate_switch; // 内循环过滤
    SwitchButton* m_external_circulate_switch; // 外循环过滤

    int m_cur_id;
};

class DeviceInfoPanel : public wxPanel 
{
public:
    DeviceInfoPanel(wxWindow* parent, wxSize size = wxDefaultSize);
    void SetDeviceInfo(wxString machineType,
        wxString sprayNozzle,
        wxString printSize,
        wxString version,
        wxString number,
        wxString time,
        wxString material,
        wxString ip);

private:
    void setupLayoutDeviceInfo(wxBoxSizer* deviceStateSizer, wxPanel* parent);
    Label* m_machine_type_data{ nullptr };
    Label* m_spray_nozzle_data{ nullptr };
    Label* m_print_size_data{ nullptr };
    Label* m_firmware_version_data{ nullptr };
    Label* m_serial_number_data{ nullptr };
    Label* m_cumulative_print_time{ nullptr };
    Label* m_private_material_data{ nullptr };
    Label* m_ipAddr{ nullptr };
};

class TempMixDevice : public wxPanel
{
public:
    TempMixDevice(wxWindow*      parent,
                  bool           idle         = false,
                  wxString       nozzleTemp   = "--",
                  wxString       platformTemp = "--",
                  wxString       cavityTemp   = "--",
                  const wxPoint& pos          = wxDefaultPosition,
                  const wxSize&  size         = wxDefaultSize,
                  long           style        = wxTAB_TRAVERSAL);
    ~TempMixDevice() {};

    void setState(int state, bool lampState = false);
    void setCurId(int curId);
    void reInitProductState();
    void reInitPage();
    void setDevProductAuthority(const fnet_dev_product_t& data);
    void lostFocusmodifyTemp();
    void changeMachineType(unsigned short pid);
    void setDisabledMoveCtrl(bool b);
    void setDisabledExtruderCtrl(bool b);

    void create_panel(wxWindow* parent, bool idle, wxString nozzleTemp, wxString platformTemp, wxString cavityTemp);

    void setupLayoutIdleDeviceState(wxBoxSizer* deviceStateSizer, wxPanel* parent, bool idle);

    void connectEvent();
    void onDevInfoBtnClicked(wxMouseEvent& event);
    void onLampBtnClicked(wxMouseEvent& event);
    void onFilterBtnClicked(wxMouseEvent& event);

    void setDeviceInfoBtnIcon(const wxString& icon);

    void modifyTemp(wxString nozzleTemp   = "--",
                    wxString platformTemp = "--",
                    wxString cavityTemp   = "--",
                    int      topTemp      = 0,
                    int      bottomTemp   = 0,
                    int      chamberTemp  = 0);
    void modifyDeviceInfo(wxString machineType,
                          wxString sprayNozzle,
                          wxString printSize,
                          wxString version,
                          wxString number,
                          wxString time,
                          wxString material,
                          wxString ip);
    void modifyDeviceLampState(bool bOpen);
    void modifyDeviceFilterState(bool internalOpen, bool externalOpen);
    void modifyG3UClearFanState(bool bOpen);
    void modifyDevicePositonState(double x, double y, double z);
    void hideMonitorPanel(bool b = false);

private:
    wxPanel* m_panel_idle_device_state;
    DeviceInfoPanel* m_panel_idle_device_info;
    wxPanel* m_panel_idle_device_title;

    Button* m_idle_device_info_button;
    Button* m_idle_lamp_control_button;
    Button* m_idle_filter_button;

    DeviceInfoPanel*        m_panel_u_device;
    StartFiltering* m_panel_circula_filter; // 空闲状态，过滤按钮

    /*TempInput* m_top_btn{nullptr};
    TempInput* m_bottom_btn{nullptr};
    TempInput* m_mid_btn{nullptr};*/

    // TempButton *m_top_btn{nullptr};
    // TempButton *m_bottom_btn{nullptr};
    // TempButton *m_mid_btn{nullptr};

    int m_cur_id = -1;

    double m_right_target_temp  = 0.00;
    double m_plat_target_temp   = 0.00;
    double m_cavity_target_temp = 0.00;

    bool m_g3uMachine      = false;
    bool m_clearFanPressed = false;

    Button*              m_plate_up_btn{nullptr};
    Button*              m_plate_down_btn{nullptr};
    Button*              m_extruder_up_btn{nullptr};
    Button*              m_extruder_down_btn{nullptr};
    wxStaticText*        m_pos_title_text{nullptr};
    wxStaticText*        m_x_text{nullptr};
    wxStaticText*        m_y_text{nullptr};
    wxStaticText*        m_z_text{nullptr};
    Button*              m_zero_btn{nullptr};
    Button*              m_btn_step1{nullptr};
    Button*              m_btn_step50{nullptr};
    Button*              m_btn_step100{nullptr};
    PosCtrlButton*       m_pos_btn{nullptr};  
    wxStaticText*        m_extruder_title{nullptr};
    wxPanel*             m_blank_page{nullptr};
    wxPanel*             m_extruderLine{nullptr};
    wxPanel*             m_extruderSperator{nullptr};
    wxPanel*             m_extruderSperator1{nullptr};
    wxPanel*             m_plateSperator{nullptr};
    wxBoxSizer*          m_vSizer3{nullptr};
    std::vector<Button*> m_btn_step;
    int                  m_pos_ctrl_step = 1;
};

class NewTempInputPanel : public wxPanel 
{
public:
    NewTempInputPanel(wxWindow* parent);
    void UpdateTempatrue(const com_dev_data_t& data);
    void ReInitTempature(int curId);
    void SwitchTargetTemp(bool flag);

private:
    std::unordered_map<std::string, NewTempInput*> m_tempInputs;
    wxBoxSizer*             m_tempSizer;
    int m_cur_id = -1;
    // What the rows in m_tempInputs were built for. UpdateTempatrue and lostTempModify follow
    // this, not a fresh product id lookup, so they only ever touch rows that exist.
    FFTempLayout   m_layout       = FFTempLayout::Generic;
    unsigned short m_layout_pid   = 0;
    bool           m_layout_known = false;
    void buildTempLayout(int curId, unsigned short pid, int nozzleCnt);
    NewTempInput* tempInput(const char* key) const;
    void setTemps(const char* key, double curr, double target);
    void lostTempModify();
};
   
}} // namespace Slic3r::GUI

#endif // !slic3r_GUI_TempInput_hpp_
