#ifndef slic3r_Moonraker_hpp_
#define slic3r_Moonraker_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/TimeoutMap.hpp"

class MqttClient;

namespace Slic3r {

class DynamicPrintConfig;
class Http;
// Base class for communicating with Moonraker API
class Moonraker : public PrintHost
{
public:
    // Constructor takes printer configuration
    Moonraker(DynamicPrintConfig *config);
    ~Moonraker() override = default;

    // Get name of this print host type
    const char* get_name() const override;

    // Test connection to printer
    virtual bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg () const override;
    wxString get_test_failed_msg (wxString &msg) const override;

    // Upload file to printer
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;

    // Send G-code commands to printer
    bool send_gcodes(const std::vector<std::string>& codes, std::string& extraInfo) override;

    // Get printer information
    bool get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, nlohmann::json& response) override;

    // Configuration getters
    bool has_auto_discovery() const override { return true; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
    std::string get_host() const override { return m_host; }
    const std::string& get_apikey() const { return m_apikey; }
    const std::string& get_cafile() const { return m_cafile; }

    // Connect/disconnect from printer
    virtual bool connect(wxString& msg, const nlohmann::json& params) override;
    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override { return true; }

    // Async printer information methods
    virtual void async_get_system_info(std::function<void(const nlohmann::json& response)> callback) override {}
    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)>) override  {}
    virtual void async_subscribe_machine_info(std::function<void(const nlohmann::json&)>) override {}
    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)>)override {}
    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override {}
    virtual void async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)>) override {}
    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override{}

    virtual void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)>) override{}

    virtual void async_pause_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void async_resume_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void async_cancel_print_job(std::function<void(const nlohmann::json&)>) override{}

    virtual void test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)>) override {}

    virtual bool check_sn_arrived() override { return false; }

    // system
    virtual void async_machine_files_roots(std::function<void(const nlohmann::json& response)>) {}

    virtual void async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_camera_start(const std::string& domain, std::function<void(const nlohmann::json& response)>) {}

    virtual void async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)>) {}

protected:
    // Internal upload implementations
#ifdef WIN32
    virtual bool upload_inner_with_resolved_ip(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn, const boost::asio::ip::address& resolved_addr) const;
#endif
    virtual bool validate_version_text(const boost::optional<std::string> &version_text) const;
    virtual bool upload_inner_with_host(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const;

    // Connection parameters
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    // Helper methods
    virtual void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;

    std::string make_url_8080(const std::string& path) const;

private:
#ifdef WIN32
    bool test_with_resolved_ip(wxString& curl_msg) const;
#endif
};

// Extended Moonraker class that uses MQTT for communication
class Moonraker_Mqtt : public Moonraker
{
public:
    // Constructor
    Moonraker_Mqtt(DynamicPrintConfig* config, bool change_engine =  true);

    // Override connection methods
    virtual bool connect(wxString& msg, const nlohmann::json& params) override;
    virtual bool disconnect(wxString& msg, const nlohmann::json& params) override;

    // Override async information methods
    virtual void async_get_system_info(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_get_machine_info(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets, std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_subscribe_machine_info(std::function<void(const nlohmann::json&)>) override;
    virtual void async_get_machine_objects(std::function<void(const nlohmann::json& response)> callback) override;
    virtual void async_set_machine_subscribe_filter(const std::vector<std::pair<std::string, std::vector<std::string>>>& targets,
                                                    std::function<void(const nlohmann::json& response)>                  callback) override;
    virtual void async_unsubscribe_machine_info(std::function<void(const nlohmann::json&)>) override;
    virtual void async_send_gcodes(const std::vector<std::string>& scripts, std::function<void(const nlohmann::json&)>) override;
    virtual void async_get_printer_info(std::function<void(const nlohmann::json& response)> callback) override;

    virtual void async_start_print_job(const std::string& filename, std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_pause_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_resume_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void async_cancel_print_job(std::function<void(const nlohmann::json&)> cb) override;

    virtual void test_async_wcp_mqtt_moonraker(const nlohmann::json& mqtt_request_params, std::function<void(const nlohmann::json&)>) override;

    virtual bool check_sn_arrived() override;

    virtual void async_machine_files_roots(std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_metadata(const std::string& filename, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_thumbnails(const std::string& filename, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_machine_files_directory(const std::string& path, bool extend, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_camera_start(const std::string& domain, std::function<void(const nlohmann::json& response)>) override;

    virtual void async_canmera_stop(const std::string& domain, std::function<void(const nlohmann::json& response)>) override;

public:
    // MQTT message handler
    void on_mqtt_message_arrived(const std::string& topic, const std::string& payload);

private:
    // Helper methods for MQTT communication
    bool send_to_request(const std::string& method,
                        const nlohmann::json& params,
                        bool need_response,
                        std::function<void(const nlohmann::json& response)> callback,
                        std::function<void()> timeout_callback);

    bool add_response_target(const std::string& id,
                           std::function<void(const nlohmann::json&)> callback,
                           std::function<void()> timeout_callback = nullptr,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(80000));

    std::function<void(const nlohmann::json& response)> get_request_callback(const std::string& id);
    void delete_response_target(const std::string& id);

    bool wait_for_sn(int timeout_seconds = 6);

    // MQTT message handlers
    void on_response_arrived(const std::string& payload);
    void on_status_arrived(const std::string& payload);
    void on_notification_arrived(const std::string& payload);

public:
    // Callback structure for MQTT requests
    struct RequestCallback {
        std::function<void(const nlohmann::json&)> success_cb;  // Success callback
        std::function<void()> timeout_cb;                       // Timeout callback

        RequestCallback(
            std::function<void(const nlohmann::json&)> success,
            std::function<void()> timeout = nullptr)
            : success_cb(std::move(success))
            , timeout_cb(std::move(timeout))
        {}
    };

private:
    // Static MQTT client and related variables
    static std::shared_ptr<MqttClient> m_mqtt_client;
    static TimeoutMap<std::string, RequestCallback> m_request_cb_map;
    static std::function<void(const nlohmann::json&)> m_status_cb;

    // MQTT topics
    static std::string m_response_topic;
    static std::string m_status_topic;
    static std::string m_notification_topic;
    static std::string m_request_topic;

    // Printer serial number
    static std::string m_sn;
    static std::mutex m_sn_mtx;

private:
    boost::uuids::random_generator m_generator; // UUID generator for request IDs
};

}

#endif
