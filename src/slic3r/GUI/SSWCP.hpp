// Web communication protocol implementation for Slicer Studio
#ifndef SSWCP_HPP
#define SSWCP_HPP

#include <iostream>
#include <mutex>
#include <stack>

#include <wx/webview.h>
#include <unordered_set>
#include <unordered_map>
#include "nlohmann/json.hpp"
#include "Bonjour.hpp"
#include "slic3r/Utils/TimeoutMap.hpp"
#include "slic3r/Utils/PrintHost.hpp"

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// Base class for handling web communication instances
class SSWCP_Instance : public std::enable_shared_from_this<SSWCP_Instance>
{
public:
    // Types of communication instances
    enum INSTANCE_TYPE {
        COMMON,             // Common instance type
        MACHINE_FIND,       // For machine discovery
        MACHINE_CONNECT,    // For machine connection
        MACHINE_OPTION,     // For machine options/settings
    };

public:
    // Constructor initializes instance with command and parameters
    SSWCP_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : m_cmd(cmd), m_header(header), m_webview(webview), m_event_id(event_id), m_param_data(data)
    {}

    virtual ~SSWCP_Instance() {}

    // Process the command
    virtual void process();

    // Send response back to JavaScript
    virtual void send_to_js();

    // Clean up after job completion
    virtual void finish_job();

    // General failure
    void handle_general_fail();

    // Get instance type
    virtual INSTANCE_TYPE getType() { return m_type; }

    // Check if instance is stopped
    virtual bool is_stop() { return false; }
    virtual void set_stop(bool stop) {}

    // Mark instance as illegal (invalid)
    virtual void set_Instance_illegal();
    virtual bool is_Instance_illegal();

    // Get associated webview
    wxWebView* get_web_view() const;

public:
    // Handle timeout event
    virtual void on_timeout();

    static void on_mqtt_msg_arrived(std::shared_ptr<SSWCP_Instance> obj, const json& response);

private:
    // Test methods
    void sync_test();
    void async_test();

    void test_mqtt_request();

public:
    std::string m_cmd;           // Command to execute
    json        m_header;        // Request header
    wxWebView*  m_webview;       // Associated webview
    std::string m_event_id;      // Event identifier
    json        m_param_data;    // Command parameters

    json        m_res_data;      // Response data
    int         m_status = 200;  // Response status code
    std::string m_msg = "OK";    // Response message

protected:
    INSTANCE_TYPE m_type = COMMON;  // Instance type

private:
    bool m_illegal = false;      // Invalid flag
    std::mutex m_illegal_mtx;    // Mutex for illegal flag
};

// Instance class for handling machine connection
class SSWCP_MachineConnect_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineConnect_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_CONNECT;
    }

    ~SSWCP_MachineConnect_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:
    // Connection test methods
    void sw_test_connect();
    void sw_connect();
    void sw_disconnect();

    void sw_get_connect_machine();

    void sw_connect_other_device();

private:
    std::thread m_work_thread;  // Worker thread
};

// Instance class for handling machine discovery
class SSWCP_MachineFind_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineFind_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_FIND;
    }

    void process() override;

    bool is_stop() { return m_stop; }
    void set_stop(bool stop);

private:
    // Machine discovery methods
    void sw_GetMachineFindSupportInfo();
    void sw_StartMachineFind();
    void sw_StopMachineFind();

private:
    void add_machine_to_list(const json& machine_info);
    void onOneEngineEnd();

private:
    std::mutex m_machine_list_mtx;  // Mutex for machine list
    std::mutex m_stop_mtx;          // Mutex for stop flag

private:
    std::unordered_map<std::string, json> m_machine_list;  // Found machines

private:
    int m_engine_end_count = 0;                         // Counter for finished discovery engines
    std::vector<std::shared_ptr<Bonjour>> m_engines;    // Discovery engines
    bool m_stop = false;                                // Stop flag
};

// Instance class for handling machine options
class SSWCP_MachineOption_Instance : public SSWCP_Instance
{
public:
    SSWCP_MachineOption_Instance(std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {
        m_type = MACHINE_OPTION;
    }

    ~SSWCP_MachineOption_Instance()
    {
        if (m_work_thread.joinable())
            m_work_thread.detach();
    }

    void process() override;

private:

    // Machine option methods
    void sw_SendGCodes();
    void sw_GetMachineState();
    void sw_GetPrintInfo();
    void sw_SubscribeMachineState();
    void sw_UnSubscribeMachineState();
    void sw_GetMachineObjects();
    void sw_SetMachineSubscribeFilter();
    void sw_GetSystemInfo();
    void sw_MachinePrintStart();
    void sw_MachinePrintPause();
    void sw_MachinePrintResume();
    void sw_MachinePrintCancel();

    // SYSTEM
    void sw_MachineFilesRoots();
    void sw_MachineFilesMetadata();
    void sw_MachineFilesThumbnails();
    void sw_MachineFilesGetDirectory();
    void sw_CameraStartMonitor();
    void sw_CameraStopMonitor();

    // PrePrint
    void sw_GetFileFilamentMapping();
    void sw_SetFilamentMappingComplete();

    // Download machine file
    void sw_DownloadMachineFile();

    // upload file to machine
    void sw_UploadFiletoMachine();


private:
    std::thread m_work_thread;  // Worker thread
};

// Main SSWCP class for managing communication instances
class SSWCP
{
public:
    // Handle incoming web messages
    static void handle_web_message(std::string message, wxWebView* webview);

    // Create new SSWCP instance
    static std::shared_ptr<SSWCP_Instance> create_sswcp_instance(
        std::string cmd, const json& header, const json& data, std::string event_id, wxWebView* webview);

    // Delete instance
    static void delete_target(SSWCP_Instance* target);

    // Stop machine discovery
    static void stop_machine_find();

    // Stop machine subscription
    static void stop_subscribe_machine();

    // Handle webview deletion
    static void on_webview_delete(wxWebView* webview);

    // query the info of the machine
    static bool query_machine_info(std::shared_ptr<PrintHost>& host, std::string& out_model, std::vector<std::string>& out_nozzle_diameters, int timeout_second = 5);

    // update the active file name
    static void update_active_filename(const std::string& filename);

    // update the display name
    static void update_display_filename(const std::string& display_name);

    // get the active file name
    static std::string get_active_filename();

    static std::string get_display_filename();
    
private:
    static std::unordered_set<std::string> m_machine_find_cmd_list;     // Machine find commands
    static std::unordered_set<std::string> m_machine_option_cmd_list;   // Machine option commands
    static std::unordered_set<std::string> m_machine_connect_cmd_list;  // Machine connect commands
    static TimeoutMap<SSWCP_Instance*, std::shared_ptr<SSWCP_Instance>> m_instance_list;  // Active instances
    static constexpr std::chrono::milliseconds DEFAULT_INSTANCE_TIMEOUT{80000}; // Default timeout (8s)

    static std::string m_active_gcode_filename; // name of the file which is pretend to be upload and print
    static std::string m_display_gcode_filename; // name for display
}; 

class MachineIPType
{
public:
    static MachineIPType* getInstance();

    void add_instance(const std::string& ip, const std::string& machine_type)
    {
        m_map_mtx.lock();
        m_ip_type_map[ip] = machine_type;
        m_map_mtx.unlock();
    }

    bool get_machine_type(const std::string& ip, std::string& output)
    {
        bool res = true;
        m_map_mtx.lock();
        if (m_ip_type_map.count(ip)) {
            output = m_ip_type_map[ip];
        } else {
            res = false;
        }
        m_map_mtx.unlock();
        return res;
    }

private:
    std::mutex                                   m_map_mtx;
    std::unordered_map<std::string, std::string> m_ip_type_map;

};

}};

#endif
