#ifndef slic3r_Utils_GatewayService_hpp_
#define slic3r_Utils_GatewayService_hpp_

#include "ConnectionProcessManager.hpp"
#include "GatewayProtocol.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace Gateway {

enum class ConnectionState { Disconnected, Connecting, Connected };

class HttpTransport
{
public:
    virtual ~HttpTransport()                                                        = default;
    virtual HttpResponse get(const std::string& url)                                = 0;
    virtual HttpResponse post_json(const std::string& url, const std::string& body) = 0;
};

class WebSocketTransport
{
public:
    struct Listener
    {
        std::function<void()>                           opened;
        std::function<void(const std::string& message)> message;
        std::function<void(const std::string& reason)>  closed;
    };

    virtual ~WebSocketTransport()                                     = default;
    virtual void set_listener(Listener listener)                      = 0;
    virtual void connect(std::uint16_t port, const std::string& path) = 0;
    virtual bool send(const std::string& message)                     = 0;
    virtual void close()                                              = 0;
};

class LibcurlHttpTransport final : public HttpTransport
{
public:
    explicit LibcurlHttpTransport(long connect_timeout_seconds = 2, long total_timeout_seconds = 5);
    HttpResponse get(const std::string& url) override;
    HttpResponse post_json(const std::string& url, const std::string& body) override;

private:
    long connect_timeout_seconds_;
    long total_timeout_seconds_;
};

class GatewayWebSocketTransport final : public WebSocketTransport
{
public:
    explicit GatewayWebSocketTransport(std::string host = "127.0.0.1");
    ~GatewayWebSocketTransport() override;

    void set_listener(Listener listener) override;
    void connect(std::uint16_t port, const std::string& path) override;
    bool send(const std::string& message) override;
    void close() override;

private:
    struct Session;

    std::string            host_;
    mutable std::mutex     mutex_;
    Listener               listener_;
    std::weak_ptr<Session> session_;
    std::thread            io_thread_;

    void close_locked();
};

class GatewayService
{
public:
    using NotificationCallback = std::function<void(const nlohmann::json& params)>;
    using RpcCallback          = std::function<void(GatewayError error, const nlohmann::json& result)>;
    using StateCallback        = std::function<void(ConnectionState state, const GatewayError& error)>;
    using Dispatcher           = std::function<void(std::function<void()> task)>;

    struct Config
    {
        std::string                     host{"127.0.0.1"};
        std::string                     health_path{"/health"};
        std::string                     websocket_path{"/ws"};
        std::string                     device_path{"/api/device"};
        std::string                     account_path{"/api/account"};
        std::string                     store_path{"/api/store"};
        std::chrono::milliseconds       health_poll_interval{std::chrono::milliseconds{100}};
        std::chrono::milliseconds       health_timeout{std::chrono::milliseconds{5000}};
        detail::ReconnectPolicy::Config reconnect;
    };

    struct Dependencies
    {
        std::shared_ptr<ConnectionProcessManager> process_manager;
        std::shared_ptr<HttpTransport>            http;
        std::shared_ptr<WebSocketTransport>       websocket;
        Dispatcher                                dispatcher;
        std::function<std::int64_t()>             now_ms;
    };

    struct ApiResult
    {
        GatewayError   error;
        nlohmann::json value{nlohmann::json::object()};
    };

    GatewayService(Config config, Dependencies dependencies);
    ~GatewayService();

    GatewayService(const GatewayService&)            = delete;
    GatewayService& operator=(const GatewayService&) = delete;

    bool            start(const std::string& locale);
    void            stop();
    bool            is_connected() const;
    bool            wait_for_connected(std::chrono::milliseconds timeout);
    ConnectionState state() const;
    std::uint16_t   port() const;
    HealthInfo      health() const;
    std::string     base_url() const;

    void set_state_callback(StateCallback callback);
    void set_notification_handler(const std::string& method, NotificationCallback callback);
    void clear_notification_handler(const std::string& method);

    std::int64_t request(const std::string& method, const nlohmann::json& params, RpcCallback callback);
    std::int64_t watch_device(const nlohmann::json& params, RpcCallback callback);
    ApiResult    get_device(const std::optional<std::string>& serial_number = std::nullopt);
    ApiResult    get_account();
    PreprintStoreResult store_preprint_context(const std::string& id, const nlohmann::json& payload, int ttl_seconds = 1800);
    std::string  web_url(const std::string& page_key) const;
    std::string  localfile_url(const std::string& file_path) const;

private:
    struct PendingRequest
    {
        RpcCallback callback;
    };

    void         run(const std::string locale);
    GatewayError connect_once(const std::string& locale);
    GatewayError wait_for_health(std::uint16_t port, HealthInfo& health);
    GatewayError connect_websocket(std::uint16_t port);
    void         handle_websocket_message(const std::string& message);
    void         handle_websocket_closed(const std::string& reason);
    void         dispatch_state(ConnectionState state, GatewayError error);
    void         complete_pending(std::int64_t id, GatewayError error, const nlohmann::json& result);
    void         fail_pending(const GatewayError& error);
    void         set_websocket_open(bool opened);
    ApiResult    get_json(const std::string& path);
    ApiResult    post_json(const std::string& path, const nlohmann::json& body);
    std::int64_t next_request_id();
    std::int64_t send_request(const std::string& method, const nlohmann::json& params, RpcCallback callback);

    Config                  config_;
    Dependencies            dependencies_;
    detail::ReconnectPolicy reconnect_policy_;

    mutable std::mutex      state_mutex_;
    std::condition_variable state_condition_;
    ConnectionState         state_{ConnectionState::Disconnected};
    std::uint16_t           port_{0};
    HealthInfo              health_;
    bool                    websocket_open_{false};
    std::string             websocket_error_;
    bool                    stop_requested_{false};
    std::thread             worker_;

    mutable std::mutex                          callback_mutex_;
    StateCallback                               state_callback_;
    std::map<std::string, NotificationCallback> notification_handlers_;

    std::mutex                             pending_mutex_;
    std::map<std::int64_t, PendingRequest> pending_requests_;
    std::atomic<std::int64_t>              next_id_{1};
};

}} // namespace Slic3r::Gateway

#endif // slic3r_Utils_GatewayService_hpp_
