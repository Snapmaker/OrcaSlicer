#ifndef slic3r_DeviceData_hpp_
#define slic3r_DeviceData_hpp_

#include <wx/event.h>
#include "nlohmann/json.hpp"
#include "FlashNetwork.h"
#include "MultiComEvent.hpp"

using namespace nlohmann;  // json open source library
//using namespace std;

namespace Slic3r {

namespace GUI {

#define CONNECTTYPE_LAN "lan"
#define CONNECTTYPE_CLOUD "cloud"

enum ConnectMode {
    UNKNOW_MODE = -1,
    LAN_MODE,
    WAN_MODE
};

enum ActiveState {
    NotActive, 
    Active, 
    UpdateToDate
};

enum DeviceType {
    DT_USER,
    DT_LOCAL,
    DT_BOTH,
};

struct device_wan_info
{
    std::string name;
    std::string bind_dev_id;
    std::string dev_topic;
    int pid;
    std::string serialNum;
};

struct id_connect_mode
{
    com_id_t id;
    ComConnectMode mode;
};

struct BindInfo
{
    std::string    dev_id;
    std::string    bind_id;
    std::string    dev_topic;
    std::string    dev_ip;
    unsigned short dev_port;
    std::string    dev_name;
    unsigned short dev_pid;
    unsigned short dev_bind_type;
    std::string    img;
};

class DeviceObject
{
public:
    DeviceObject(const std::string& dev_id, const std::string& dev_name);
    DeviceObject(const fnet_lan_dev_info &devInfo);
    DeviceObject(const device_wan_info &wanInfo);
    ~DeviceObject();

    bool        is_lan_mode_in_scan_print();
    bool        is_lan_mode_printer();
    bool        has_access_right();

    void        set_user_access_code(const std::string& code, bool only_refresh = true);
    std::string get_user_access_code(bool inner = false);
    void        erase_user_access_code();

    bool        is_avaliable();

    void        set_online_state(bool on_off);
    bool        is_online();

    void        set_active_state(ActiveState state);
    ActiveState get_active_state();

    void        set_connection_type(const std::string& connectType);
    std::string connection_type();

    void        reset_update_time();

    fnet_lan_dev_info *get_lan_dev_info();
    //void               set_lan_dev_info(fnet_lan_dev_info *info);
    void               set_lan_dev_info(const fnet_lan_dev_info &info);
    void               set_wan_dev_info(const device_wan_info& info);

    void               init_lan_obj();
    void               init_wan_obj();
    std::string        get_dev_name();
    void               set_dev_name(const std::string& name);
    std::string        get_dev_ip();
    unsigned short     get_dev_port();
    std::string        get_dev_id(); // serialNumber
    unsigned short     get_dev_pid();
    unsigned short     get_dev_bind_type();
    std::string        get_wan_dev_id();
    std::string        get_wan_dev_topic();

    static bool is_in_printing_status(const std::string& status);
    void        set_print_state(const std::string& status);

    /* common apis */
    bool        is_in_printing();

    void        set_connecting(bool connecting);
    bool        is_connecting();

    void        set_connected_ready(bool ready);
    bool        is_connected_ready();

    std::string get_printer_thumbnail_img_str();
    void        set_device_type(DeviceType type);
    DeviceType  device_type();

    int connectMode();

    BindInfo* get_bind_info();

private:
    fnet_lan_dev_info *m_lan_info { nullptr };
    device_wan_info   *m_wan_info { nullptr };
    std::string        m_dev_id;
    std::string        m_dev_name;

    std::string                           m_user_access_code;
    std::string                           m_bind_state;               /* free | occupied */
    ActiveState         m_active_state = NotActive; // 0 - not active, 1 - active, 2 - update-to-date
    bool                m_is_online;
    bool                m_is_connecting { false };
    bool                m_is_connected_ready { true };
    std::string                           m_dev_connection_type; /* lan | cloud */
    DeviceType          m_deviceType;
    std::chrono::system_clock::time_point last_update_time; /* last received print data from machine */

    /* printing status */
    std::string m_printStatus; /* enum string: FINISH, SLICING, RUNNING, PAUSE, INIT, FAILED */
};

typedef std::map<std::string, std::string> MacInfoMap;

class DeviceListUpdateEvent : public wxCommandEvent
{
public:
    enum class UpdateType : int {
        UpdateType_Null = 0,
        UpdateType_Add,
        UpdateType_Remove,
        UpdateType_Update,
    };

public:
    DeviceListUpdateEvent(wxEventType type) : wxCommandEvent(type) {}
    DeviceListUpdateEvent(wxEventType type, UpdateType op, const std::string& dev_id, int conn_id)
        : wxCommandEvent(type), m_dev_id(dev_id), m_operator(op), m_conn_id(conn_id) {}

    DeviceListUpdateEvent *Clone() const {
        return new DeviceListUpdateEvent(GetEventType(), m_operator, m_dev_id, m_conn_id);
    }
    void SetDeviceId(const std::string& dev_id) { m_dev_id = dev_id;}
    const std::string& GetDeviceId() const { return m_dev_id;}
    void SetOperator(UpdateType op) { m_operator = op; }
    UpdateType GetOperator() const { return m_operator; }
    int GetConnectionId() const {return m_conn_id;}
    void SetConnectionId(int conn_id) { m_conn_id = conn_id;}

private:
    int         m_conn_id {-1};
    UpdateType  m_operator {UpdateType::UpdateType_Null};
    std::string m_dev_id;
};
wxDECLARE_EVENT(EVT_DEVICE_LIST_UPDATED, DeviceListUpdateEvent);

class LocalDeviceNameChangeEvent : public wxCommandEvent
{
public:
    std::string dev_id;
    std::string dev_name;

    LocalDeviceNameChangeEvent(wxEventType type, const std::string& sn, const std::string& name)
        : wxCommandEvent(type), dev_id(sn), dev_name(name) {}

    LocalDeviceNameChangeEvent *Clone() const {
        return new LocalDeviceNameChangeEvent(GetEventType(), dev_id, dev_name);
    }
};
wxDECLARE_EVENT(EVT_LOCAL_DEVICE_NAME_CHANGED, LocalDeviceNameChangeEvent);

class DeviceObjectOpr : public wxEvtHandler
{
public:
    DeviceObjectOpr();
    ~DeviceObjectOpr();

public:
    void update_scan_machine();
    void clear_scan_machine();
    void update_scan_list(const std::vector<fnet_lan_dev_info>& infos);
    void read_local_machine_from_config();

    void get_scan_machine(std::map<std::string, DeviceObject*>& macList);
    void get_local_machine(std::map<std::string, DeviceObject *>& macList);
    void get_user_machine(std::map<std::string, DeviceObject*>& macList);
    bool my_machine_empty();

    /* return machine has access code and user machine if login*/
    void get_my_machine_list(std::map<std::string, DeviceObject*>& devList);

    // clear user machine
    void clear_user_machine();

    bool          set_selected_machine(const std::string& dev_id, bool my_machine = false);
    DeviceObject* get_selected_machine();

    // Adds a printer the user typed in (serial number, IP address, check code) instead of one a LAN
    // scan found. `info` carries the serial, name, ip, port and product id; the printer is
    // connected at once and, when it answers, saved - so it is in the list at the next start too.
    // Returns false when the serial number is empty or the connection could not be started.
    bool          add_manual_lan_machine(const fnet_lan_dev_info& info, const std::string& check_code);

    // Starts a connection to every saved printer that has an address and a check code and is not
    // connected or connecting. Saved printers used to wait for a LAN scan nothing ever ran, so they
    // sat Offline for ever. Safe to call repeatedly (each tab activation does).
    void          connect_saved_machines();

    // Builds the lan record FlashNetwork wants from the pieces a saved/typed printer has.
    static fnet_lan_dev_info make_lan_info(const std::string& serial, const std::string& name,
                                           const std::string& ip, unsigned short port, unsigned short pid);

    void unbind_lan_machine(DeviceObject *obj);
    ComErrno unbind_wan_machine(const std::string& dev_id, const std::string& bind_id, const std::string& dev_topic);
    std::string find_dev_from_id(id_connect_mode& mode, int connectId);
    

private:
    DeviceObject* get_scan_device(const std::string& dev_id);
    // Opens the FlashNetwork LAN connection for `obj` (needs its lan info and check code) and
    // records it in m_lan_dev_connect_map.
    bool connect_lan(DeviceObject* obj);

    // before connect, scan machine's access code which hasn't written in config file
    void get_my_machine_list_v2(std::map<std::string, DeviceObject*> & devList, bool my_machine = false);
    
    void sendDeviceListUpdateEvent(const std::string& dev_id, int conn_id, bool wan_offline = false);

    void removeUserDev(DeviceObject *obj);

private:
    void onConnectExit(ComConnectionExitEvent &event);
    void onConnectReady(ComConnectionReadyEvent &event);
    void onConnectWanDevInfoUpdate(ComWanDevInfoUpdateEvent &event);

private:
    std::string                m_selected_machine; /* dev_id */
    std::map<std::string, DeviceObject*> m_scan_devices; /* dev_id -> DeviceObject*, scan in lan (only lan connectMode, and wan connectMode)   */
    std::map<std::string, DeviceObject*> m_old_devices;
    std::map<std::string, DeviceObject*> m_user_devices; /* dev_id -> DeviceObject*, when user login, the user's devices that has bound. And machine connected successfully. */
    std::map<std::string, DeviceObject*> m_old_user_devices;
    std::map<std::string, DeviceObject*> m_local_devices; /* dev_id -> DeviceObject*,  in lan connectMode, device has input access code. Read data from appconfig. */
    //map<std::string, com_id_t>             m_dev_connect_map;   /* dev_id -> connectId */
    std::map<std::string, id_connect_mode> m_lan_dev_connect_map;
    std::map<std::string, id_connect_mode> m_wan_dev_connect_map;
};

}

}

#endif