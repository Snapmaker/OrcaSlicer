#ifndef slic3r_DeviceListPanel_hpp_
#define slic3r_DeviceListPanel_hpp_
#include <map>
#include <set>
#include <wx/simplebook.h>
#include <wx/tglbtn.h>
#include <wx/timer.h>
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "MultiComDef.hpp"
#include "MultiComMgr.hpp"
#include "MultiComEvent.hpp"
#include "DeviceData.hpp"
#include "DeviceFilterPopup.hpp"

class FFToggleButton;
class FFBitmapToggleButton;
class FFCheckBox;

namespace Slic3r {
namespace GUI {


class DropDownButton : public wxPanel
{
public:
    DropDownButton(wxWindow* parent = nullptr, const wxString& text = wxEmptyString, const wxBitmap& bitmap = wxNullBitmap);
    ~DropDownButton();

    void setText(const wxString& text);

private:
    wxPoint convertEventPoint(const wxMouseEvent& event);
    void updateMinSize();
    void onLeftDown(wxMouseEvent& event);
    void onLeftUp(wxMouseEvent& event);

private:
    wxStaticText*   m_text;
    wxStaticBitmap* m_bitmap;
    wxBoxSizer*     m_sizer;
};


class DeviceItemPanel : public wxPanel
{
public:
    DeviceItemPanel(wxWindow *parent);
    virtual ~DeviceItemPanel();

    void blockMouseEvent(bool block);

protected:
    void leaveWindow();
    void mouseDown(wxMouseEvent &event);
    void mouseReleased(wxMouseEvent &event);
    void onEnter(wxMouseEvent &event);
    void onLeave(wxMouseEvent &event);
    void onMotion(wxMouseEvent &event);
    void onPaint(wxPaintEvent& event);

protected:
    virtual wxPoint convertEventPoint(wxMouseEvent& event);
    virtual void sendEvent() {};
    bool isPointIn(const wxPoint& pt);
    void bindEvent(bool bind);
    void do_render(wxDC& dc);

protected:
    bool            m_hovered {false};
    bool            m_pressed {false};
    bool            m_blockFlag {false};    
    wxBoxSizer*     m_main_sizer {nullptr};
    wxColour        m_bg_color = wxColour("#ffffff");
    wxColour        m_border_color = wxColour("#ffffff");
    wxColour        m_border_hover_color = wxColour("#999999");
    wxColour        m_border_press_color = wxColour("#328DFB");

    static std::map<unsigned short, wxBitmap> m_machineBitmapMap;
};


class DeviceInfoItemPanel : public DeviceItemPanel
{
public:
    struct DeviceInfo {
        bool lanFlag {true};  // lan or not
        int conn_id {ComInvalidId};
        unsigned short pid {0};
        std::string name;
        std::string placement;
        std::string status;
        std::string errorCode;
        int progress {0};
    };

    DeviceInfoItemPanel(wxWindow *parent, const DeviceInfo& info, wxWindow* event_handle = nullptr);

    void updateInfo(const DeviceInfo& info);
    const DeviceInfo& deviceInfo() const;
    void blockMouseEvent(bool block);
    void setDevId(const std::string& id);

private:
    wxPoint convertEventPoint(wxMouseEvent& event) override;
    void sendEvent() override;
    void updateStatus();
    static wxBitmap machineBitmap(unsigned short pid);
    void bindEvent(bool bind);

private:
    std::string     m_dev_id;
    DeviceInfo      m_info;
    bool            m_unbinding {false};
    wxStaticText*   m_name_text {nullptr};
    wxStaticBitmap* m_icon {nullptr};
    wxStaticBitmap* m_warning_icon {nullptr};
    wxStaticText*   m_placement_text {nullptr};
    wxStaticText*   m_status_text {nullptr};
    wxStaticText*   m_progress_text {nullptr};
    ScalableButton* m_exit_btn{nullptr};
    wxWindow*       m_event_handle {nullptr};
};
//wxDECLARE_EVENT(EVT_DEVICE_ITEM_SELECTED, wxCommandEvent);


class DeviceStaticItemPanel : public DeviceItemPanel
{
public:
    DeviceStaticItemPanel(wxWindow* parent, std::string status, int count);
    void setStatus(const std::string& status);
    std::string getStatus() const { return m_status; }
    void setCount(int count);
    int getCount() const { return m_count; }
    void blockMouseEvent(bool block);

private:
    wxPoint convertEventPoint(wxMouseEvent& event) override;
    void bindEvent(bool bind);

private:
    int             m_count {0};
    std::string     m_status;
    wxStaticText*   m_status_text {nullptr};
    wxStaticText*   m_count_text {nullptr};
};


struct StringCompareFunc {
    bool operator()(const std::string& lhs, const std::string& rhs) const {
        return lhs < rhs;
    }
};
struct WXStringCompareFunc {
    bool operator()(const wxString& lhs, const wxString& rhs) const {
        return lhs < rhs;
    }
};
typedef std::map<std::string, DeviceFilterItem*, StringCompareFunc> PlacementItemMap;
typedef std::map<wxString, DeviceStatusFilterItem*, WXStringCompareFunc> StatusItemMap;
typedef std::map<unsigned short, DeviceTypeFilterItem*> DeviceTypeItemMap;

//class DeviceListUpdateEvent;
class DeviceListPanel : public wxPanel
{
public:
    enum FilterPopupType {
        Filter_Popup_Type_None = 0,     // no popup
        Filter_Popup_Type_Placement,    // placement popup
        Filter_Popup_Type_Status,       // status popup
        Filter_Popup_Type_Device_Type,  // device type popup
    };

public:
    DeviceListPanel(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, 
            const wxSize& size = wxDefaultSize);
    ~DeviceListPanel();

    void msw_rescale();
    void OnActivate();

private:
    void build();
    void connectEvent();
    void initLocalDevice(std::map<std::string, DeviceInfoItemPanel::DeviceInfo>& deviceInfoMap);
    void initWlanDevice(std::map<std::string, DeviceInfoItemPanel::DeviceInfo>& deviceInfoMap);
    void initDeviceList();
    bool updateFilterMap();
    bool updatePlacementMap();
    bool updateStatusMap();
    void updateTypeMap();
    void updateFilterTitle();
    void updateStaticMap();
    void updateDeviceSizer();
    void filterDeviceList();
    void updateDeviceWindowSize();
    bool getDeviceInfo(DeviceInfoItemPanel::DeviceInfo& info, int conn_id);
    void updateDeviceInfo(const std::string& dev_id, const DeviceInfoItemPanel::DeviceInfo& info);
    void updateDeviceList();
    void copyDeviceInfo(DeviceInfoItemPanel::DeviceInfo& dest, const DeviceInfoItemPanel::DeviceInfo& source);

    void onFilterButtonClicked(wxMouseEvent &event);
    void onTestConnection(wxCommandEvent &event);
    void onAddPrinter(wxCommandEvent &event);
    void onNetworkTypeToggled(wxCommandEvent& event);
    void onStaticModeToggled(wxCommandEvent &event);
    void onDeviceListUpdated(DeviceListUpdateEvent& event);
    void onLocalDeviceNameChanged(LocalDeviceNameChangeEvent& event);
    void onComDevDetailUpdate(ComDevDetailUpdateEvent& event);
    void onComWanDeviceInfoUpdate(ComWanDevInfoUpdateEvent& event);
    void onPopupShow(wxShowEvent& event);
    void onFilterItemClicked(DeviceFilterEvent& event);
    void onRefreshTimeout(wxTimerEvent& event);

private:
    struct DeviceKey {
        int priority {0};
        std::string dev_id;   // serial number
        std::string dev_name; // sort key

        DeviceKey(const std::string& devID) : dev_id(devID) {}
        DeviceKey(int _priority, const std::string& devID, const std::string& devName)
            : priority(_priority), dev_id(devID), dev_name(devName) {}
        bool operator < (const DeviceKey& other) const {
            return dev_id < other.dev_id;
        }
    };
    
    struct DeviceKeySortFunc {
        bool operator()(const DeviceKey& lhs, const DeviceKey& rhs) const {
            //if (lhs.priority != rhs.priority) {
            //    return lhs.priority > rhs.priority;
            //}
            if (lhs.dev_name == rhs.dev_name) {
                return lhs.dev_id < rhs.dev_id;
            }
            return lhs.dev_name < rhs.dev_name;
        }
    };
    typedef std::map<DeviceKey, DeviceInfoItemPanel*> DeviceItemMap;
    typedef std::map<DeviceKey, DeviceInfoItemPanel*, DeviceKeySortFunc> DeviceItemMapSort;
    typedef std::set<DeviceKey, DeviceKeySortFunc> DeviceKeySet;
    int generateNewPriorityId();
    void updatePriorityId();

    struct DeviceCacheData {
        std::string dev_id;
        DeviceListUpdateEvent::UpdateType op;
        DeviceInfoItemPanel::DeviceInfo device_info;
    };
    typedef std::map<DeviceKey, DeviceCacheData> DeviceCacheDataMap;

private: 
    DropDownButton* m_placement_btn {nullptr};
    DropDownButton *m_status_btn {nullptr};
    DropDownButton *m_type_btn {nullptr};
    FFToggleButton* m_wlan_btn {nullptr};
    FFToggleButton* m_lan_btn {nullptr};
    FFBitmapToggleButton* m_static_btn {nullptr};
    // Opens the LAN connection test / diagnostics dialog, prefilled from the selected device.
    // It is here rather than on a device card because the case that needs it most is the one
    // where no card appeared at all.
    wxButton* m_test_btn {nullptr};
    // Adds a printer by serial number, IP address and check code (FFAddPrinterDialog).
    wxButton* m_add_btn {nullptr};

    wxWebView*        m_webBanner{nullptr};
    wxSimplebook*   m_simple_book {nullptr};
    wxPanel*        m_no_device_panel {nullptr};
    wxStaticBitmap* m_no_device_bitmap {nullptr};
    wxStaticText*   m_no_device_staticText {nullptr};
    wxBoxSizer     *m_no_device_sizer {nullptr};
    wxScrolledWindow* m_device_scrolled_window {nullptr};
    wxPanel*        m_device_panel {nullptr};
    wxGridSizer*    m_device_sizer {nullptr};
    
    FilterPopupType m_filter_popup_type {Filter_Popup_Type_None};
    DeviceFilterPopupWindow *m_filter_popup;

    DeviceCacheDataMap  m_device_data_cached;
    wxTimer             m_refresh_timer;
    DeviceItemMap       m_device_map;
    std::map<std::string, DeviceStaticItemPanel*> m_device_stat_map;
    DeviceFilterItem*   m_default_filter_item {nullptr};
    PlacementItemMap    m_placement_item_map;
    StatusItemMap       m_status_item_map;
    DeviceTypeItemMap   m_type_item_map;
    bool                m_filter_placement_default {true};
    std::string         m_filter_placement;
    std::string         m_filter_placement_trimmed;
    bool                m_filter_status_default {true};
    wxString            m_filter_status;
    std::set<unsigned short> m_filter_types;

    static int m_last_priority_id;
};

} // GUI
} // Slic3r

#endif /* slic3r_DeviceListPanel_hpp_ */
