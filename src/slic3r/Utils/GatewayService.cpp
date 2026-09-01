#include "GatewayService.hpp"

#include "Http.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <random>
#include <stdexcept>
#include <utility>

namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;

namespace Slic3r { namespace Gateway {
namespace {

std::int64_t wall_now_ms()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

GatewayError transport_error(const std::string& message) { return {GatewayErrorCode::TransportError, message}; }

std::string make_url(const std::string& host, std::uint16_t port, const std::string& path)
{ return "http://" + host + ":" + std::to_string(port) + path; }

std::string join_url(const std::string& base_url, const std::string& path)
{
    if (base_url.empty() || path.empty())
        return {};
    if (base_url.back() == '/' && path.front() == '/')
        return base_url.substr(0, base_url.size() - 1) + path;
    if (base_url.back() != '/' && path.front() != '/')
        return base_url + "/" + path;
    return base_url + path;
}

} // namespace

LibcurlHttpTransport::LibcurlHttpTransport(long connect_timeout_seconds, long total_timeout_seconds)
    : connect_timeout_seconds_(connect_timeout_seconds), total_timeout_seconds_(total_timeout_seconds)
{}

HttpResponse LibcurlHttpTransport::get(const std::string& url)
{
    HttpResponse response;
    bool         fulfilled = false;
    Http         request   = Http::get(url);
    request.remove_header("Origin").timeout_connect(connect_timeout_seconds_).timeout_max(total_timeout_seconds_);
    request.on_complete([&](std::string body, unsigned status) {
        if (!fulfilled) {
            fulfilled       = true;
            response.status = status;
            response.body   = std::move(body);
        }
    });
    request.on_error([&](std::string body, std::string error, unsigned status) {
        if (!fulfilled) {
            fulfilled       = true;
            response.status = status;
            response.body   = std::move(body);
            response.error  = std::move(error);
        }
    });
    request.perform_sync();
    return response;
}

HttpResponse LibcurlHttpTransport::post_json(const std::string& url, const std::string& body)
{
    HttpResponse response;
    bool         fulfilled = false;
    Http         request   = Http::post(url);
    request.remove_header("Origin")
        .header("Content-Type", "application/json")
        .timeout_connect(connect_timeout_seconds_)
        .timeout_max(total_timeout_seconds_)
        .set_post_body(body);
    request.on_complete([&](std::string response_body, unsigned status) {
        if (!fulfilled) {
            fulfilled       = true;
            response.status = status;
            response.body   = std::move(response_body);
        }
    });
    request.on_error([&](std::string response_body, std::string error, unsigned status) {
        if (!fulfilled) {
            fulfilled       = true;
            response.status = status;
            response.body   = std::move(response_body);
            response.error  = std::move(error);
        }
    });
    request.perform_sync();
    return response;
}

struct GatewayWebSocketTransport::Session : public std::enable_shared_from_this<Session>
{
    Session(std::shared_ptr<asio::io_context> io_context, std::string host, std::string port, std::string path, Listener listener)
        : io_context(std::move(io_context))
        , executor(this->io_context->get_executor())
        , resolver(executor)
        , stream(executor)
        , host(std::move(host))
        , port(std::move(port))
        , path(std::move(path))
        , listener(std::move(listener))
    {}

    void run()
    {
        post([self = shared_from_this()] { self->resolve(); });
    }

    void send_from_any_thread(std::string message)
    {
        post([self = shared_from_this(), message = std::move(message)]() mutable { self->queue_send(std::move(message)); });
    }

    void close_from_any_thread()
    {
        post([self = shared_from_this()] { self->close(); });
    }

private:
    template<typename Handler> void post(Handler&& handler)
    {
        asio::post(executor, [self = shared_from_this(), handler = std::forward<Handler>(handler)]() mutable { handler(); });
    }

    void resolve()
    {
        resolver.async_resolve(host, port,
                               [self = shared_from_this()](const boost::system::error_code&      error,
                                                           asio::ip::tcp::resolver::results_type results) {
                                   if (error)
                                       self->fail(error.message());
                                   else
                                       self->connect_socket(results);
                               });
    }

    void connect_socket(const asio::ip::tcp::resolver::results_type& results)
    {
        asio::async_connect(stream.next_layer(), results.begin(), results.end(),
                            [self = shared_from_this()](const boost::system::error_code& error, const asio::ip::tcp::resolver::iterator&) {
                                if (error)
                                    self->fail(error.message());
                                else
                                    self->handshake();
                            });
    }

    void handshake()
    {
        websocket::stream_base::timeout timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
        timeout.handshake_timeout               = std::chrono::seconds{5};
        timeout.idle_timeout                    = std::chrono::seconds{30};
        timeout.keep_alive_pings                = true;
        stream.set_option(timeout);
        stream.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
            request.erase(beast::http::field::origin);
            request.set(beast::http::field::user_agent, "Snapmaker_Orca");
        }));
        stream.async_handshake(host, path, [self = shared_from_this()](const boost::system::error_code& error) {
            if (error)
                self->fail(error.message());
            else
                self->on_handshake();
        });
    }

    void on_handshake()
    {
        connected.store(true, std::memory_order_release);
        if (listener.opened)
            listener.opened();
        if (!outbox.empty()) {
            do_write();
        }
        do_read();
    }

    void do_read()
    {
        stream.async_read(buffer, [self = shared_from_this()](const boost::system::error_code& error, std::size_t) {
            if (error) {
                self->fail(error.message());
                return;
            }
            if (self->listener.message)
                self->listener.message(beast::buffers_to_string(self->buffer.data()));
            self->buffer.clear();
            if (!self->closing)
                self->do_read();
        });
    }

    void queue_send(std::string message)
    {
        if (closing)
            return;
        outbox.push_back(std::move(message));
        if (connected.load(std::memory_order_acquire) && outbox.size() == 1)
            do_write();
    }

    void do_write()
    {
        if (outbox.empty() || writing)
            return;
        writing = true;
        stream.async_write(asio::buffer(outbox.front()), [self = shared_from_this()](const boost::system::error_code& error, std::size_t) {
            self->writing = false;
            if (error) {
                self->fail(error.message());
                return;
            }
            if (!self->outbox.empty())
                self->outbox.pop_front();
            if (!self->outbox.empty())
                self->do_write();
        });
    }

    void close()
    {
        if (closed.exchange(true))
            return;
        closing = true;
        boost::system::error_code error;
        stream.close(websocket::close_code::normal, error);
        if (listener.closed)
            listener.closed(error ? error.message() : std::string{});
        io_context->stop();
    }

    void fail(const std::string& reason)
    {
        if (closed.exchange(true))
            return;
        closing = true;
        boost::system::error_code error;
        stream.next_layer().close(error);
        if (listener.closed)
            listener.closed(reason);
        io_context->stop();
    }

    std::shared_ptr<asio::io_context>        io_context;
    asio::any_io_executor                    executor;
    asio::ip::tcp::resolver                  resolver;
    websocket::stream<asio::ip::tcp::socket> stream;
    std::string                              host;
    std::string                              port;
    std::string                              path;
    Listener                                 listener;
    beast::flat_buffer                       buffer;
    std::deque<std::string>                  outbox;
    std::atomic<bool>                        connected{false};
    bool                                     writing{false};
    bool                                     closing{false};
    std::atomic<bool>                        closed{false};
};

GatewayWebSocketTransport::GatewayWebSocketTransport(std::string host) : host_(std::move(host)) {}

GatewayWebSocketTransport::~GatewayWebSocketTransport()
{
    close();
    if (io_thread_.joinable())
        io_thread_.join();
}

void GatewayWebSocketTransport::set_listener(Listener listener)
{
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

void GatewayWebSocketTransport::connect(std::uint16_t port, const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
    if (io_thread_.joinable())
        io_thread_.join();

    auto     io_context = std::make_shared<asio::io_context>();
    Listener listener   = listener_;
    auto     session    = std::make_shared<Session>(io_context, host_, std::to_string(port), path, std::move(listener));
    session_            = session;
    io_thread_          = std::thread([io_context] { io_context->run(); });
    session->run();
}

bool GatewayWebSocketTransport::send(const std::string& message)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_.lock();
    }
    if (!session)
        return false;
    session->send_from_any_thread(message);
    return true;
}

void GatewayWebSocketTransport::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

void GatewayWebSocketTransport::close_locked()
{
    std::shared_ptr<Session> session = session_.lock();
    if (session)
        session->close_from_any_thread();
    if (io_thread_.joinable())
        io_thread_.join();
    session_.reset();
}

GatewayService::GatewayService(Config config, Dependencies dependencies)
    : config_(std::move(config)), dependencies_(std::move(dependencies)), reconnect_policy_(config_.reconnect)
{
    if (!dependencies_.process_manager || !dependencies_.http || !dependencies_.websocket)
        throw std::invalid_argument("GatewayService dependencies must not be null");
    if (!dependencies_.dispatcher)
        dependencies_.dispatcher = [](std::function<void()> task) { task(); };
    if (!dependencies_.now_ms)
        dependencies_.now_ms = wall_now_ms;
    dependencies_.websocket->set_listener({[this] { set_websocket_open(true); },
                                           [this](const std::string& message) { handle_websocket_message(message); },
                                           [this](const std::string& reason) { handle_websocket_closed(reason); }});
}

GatewayService::~GatewayService()
{
    stop();
    dependencies_.websocket->set_listener({});
}

bool GatewayService::start(const std::string& locale)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (worker_.joinable() && !stop_requested_)
            return false;
        if (worker_.joinable())
            worker_.join();
        stop_requested_ = false;
        state_          = ConnectionState::Connecting;
        port_           = 0;
        health_         = HealthInfo{};
        websocket_open_ = false;
        websocket_error_.clear();
    }
    worker_ = std::thread([this, locale] { run(locale); });
    return true;
}

void GatewayService::stop()
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stop_requested_ = true;
        state_          = ConnectionState::Disconnected;
        websocket_open_ = false;
    }
    state_condition_.notify_all();
    if (dependencies_.websocket)
        dependencies_.websocket->close();
    if (worker_.joinable())
        worker_.join();
    fail_pending({GatewayErrorCode::Cancelled, "GatewayService was stopped"});
    dispatch_state(ConnectionState::Disconnected, {GatewayErrorCode::Cancelled, "GatewayService was stopped"});
}

bool GatewayService::is_connected() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == ConnectionState::Connected;
}

bool GatewayService::wait_for_connected(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_condition_.wait_for(lock, timeout, [this] { return stop_requested_ || state_ == ConnectionState::Connected; }) &&
           state_ == ConnectionState::Connected;
}

ConnectionState GatewayService::state() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

std::uint16_t GatewayService::port() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return port_;
}

HealthInfo GatewayService::health() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return health_;
}

std::string GatewayService::base_url() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return health_.base_url;
}

std::string GatewayService::web_url(const std::string& page_key) const
{
    HealthInfo copied_health;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        copied_health = health_;
    }

    const auto page = copied_health.pages.find(page_key);
    if (page == copied_health.pages.end() || copied_health.base_url.empty())
        return {};
    if (page->second.empty())
        return {};

    return join_url(copied_health.base_url, page->second);
}

std::string GatewayService::localfile_url(const std::string& file_path) const
{
    if (file_path.empty())
        return {};
    return join_url(base_url(), "/localfile/" + Http::url_encode(file_path));
}

void GatewayService::set_state_callback(StateCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

void GatewayService::set_notification_handler(const std::string& method, NotificationCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    notification_handlers_[method] = std::move(callback);
}

void GatewayService::clear_notification_handler(const std::string& method)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    notification_handlers_.erase(method);
}

std::int64_t GatewayService::request(const std::string& method, const nlohmann::json& params, RpcCallback callback)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_ || state_ != ConnectionState::Connected || !websocket_open_) {
            const GatewayError error{GatewayErrorCode::NotConnected, "gateway websocket is not connected"};
            if (callback)
                dependencies_.dispatcher([callback = std::move(callback), error]() mutable { callback(error, nlohmann::json::object()); });
            return 0;
        }
    }

    return send_request(method, params, std::move(callback));
}

std::int64_t GatewayService::send_request(const std::string& method, const nlohmann::json& params, RpcCallback callback)
{
    const std::int64_t id = next_request_id();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.emplace(id, PendingRequest{std::move(callback)});
    }
    if (!dependencies_.websocket->send(build_jsonrpc_request(id, method, params).dump())) {
        complete_pending(id, {GatewayErrorCode::TransportError, "failed to queue websocket request"}, nlohmann::json::object());
        return id;
    }
    return id;
}

std::int64_t GatewayService::watch_device(const nlohmann::json& params, RpcCallback callback)
{ return request("action.device.watch", params.is_null() ? nlohmann::json::object() : params, std::move(callback)); }

GatewayService::ApiResult GatewayService::get_device(const std::optional<std::string>& serial_number)
{ return get_json(serial_number.has_value() ? config_.device_path + "/" + *serial_number : config_.device_path); }

GatewayService::ApiResult GatewayService::get_account() { return get_json(config_.account_path); }

PreprintStoreResult GatewayService::store_preprint_context(const std::string& id, const nlohmann::json& payload, int ttl_seconds)
{
    PreprintStoreResult result;
    if (id.empty() || ttl_seconds <= 0 || !payload.is_object()) {
        result.error = {GatewayErrorCode::InvalidRequest, "preprint store id, payload, or ttl is invalid"};
        return result;
    }

    const nlohmann::json request{{"id", id}, {"payload", payload}, {"ttl_seconds", ttl_seconds}};
    const ApiResult       api_result = post_json(config_.store_path, request);
    if (api_result.error) {
        result.error = api_result.error;
        return result;
    }

    const auto ok = api_result.value.find("ok");
    if (ok == api_result.value.end() || !ok->is_boolean() || !ok->get<bool>()) {
        result.error = {GatewayErrorCode::InvalidResponse, "store endpoint did not report ok=true"};
        return result;
    }

    result.ok = true;
    const auto data = api_result.value.find("data");
    if (data != api_result.value.end() && data->is_object()) {
        const auto file_exists = data->find("file_exists");
        if (file_exists != data->end() && file_exists->is_boolean())
            result.file_exists = file_exists->get<bool>();
    }
    return result;
}

void GatewayService::run(const std::string locale)
{
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stop_requested_)
                break;
            state_ = ConnectionState::Connecting;
        }
        dispatch_state(ConnectionState::Connecting, {});

        GatewayError error = connect_once(locale);
        if (!error) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (stop_requested_)
                    break;
                state_ = ConnectionState::Connected;
            }
            reconnect_policy_.record_connected(dependencies_.now_ms());
            dispatch_state(ConnectionState::Connected, {});

            std::unique_lock<std::mutex> lock(state_mutex_);
            state_condition_.wait(lock, [this] { return stop_requested_ || !websocket_open_; });
            if (stop_requested_)
                break;
            error = transport_error("websocket connection was closed");
        }

        reconnect_policy_.record_failure(dependencies_.now_ms());
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_          = ConnectionState::Disconnected;
            websocket_open_ = false;
        }
        fail_pending({GatewayErrorCode::NotConnected, "gateway connection was lost"});
        dependencies_.websocket->close();
        dispatch_state(ConnectionState::Disconnected, error);

        if (stop_requested_ || !reconnect_policy_.should_retry())
            break;

        static thread_local std::mt19937       random_generator{std::random_device{}()};
        std::uniform_real_distribution<double> distribution{0.0, 1.0};
        const auto                             delay = reconnect_policy_.next_delay(distribution(random_generator));
        std::unique_lock<std::mutex>           lock(state_mutex_);
        state_condition_.wait_for(lock, delay, [this] { return stop_requested_; });
    }
}

GatewayError GatewayService::connect_once(const std::string& locale)
{
    const ConnectionProcessManager::DiscoveryResult discovery = dependencies_.process_manager->discover_port(locale);
    if (discovery.error != ProcessDiscoveryError::None)
        return {GatewayErrorCode::ReconnectFailed, discovery.message};

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        port_ = discovery.port;
    }

    HealthInfo health;
    if (const GatewayError health_error = wait_for_health(discovery.port, health))
        return health_error;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        health_ = std::move(health);
    }

    if (const GatewayError websocket_error = connect_websocket(discovery.port))
        return websocket_error;
    return {};
}

GatewayError GatewayService::wait_for_health(std::uint16_t port, HealthInfo& health)
{
    const auto deadline = std::chrono::steady_clock::now() + config_.health_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (stop_requested_)
                return {GatewayErrorCode::Cancelled, "GatewayService was stopped"};
        }

        const HttpResponse response = dependencies_.http->get(make_url(config_.host, port, config_.health_path));
        if (response.error.empty() && response.status >= 200 && response.status < 300) {
            if (const GatewayError health_error = parse_health(response.body, health))
                return health_error;
            return {};
        }
        std::this_thread::sleep_for(config_.health_poll_interval);
    }
    return {GatewayErrorCode::HealthNotReady, "gateway health check timed out"};
}

GatewayError GatewayService::connect_websocket(std::uint16_t port)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        websocket_open_ = false;
        websocket_error_.clear();
    }
    dependencies_.websocket->connect(port, config_.websocket_path);

    std::unique_lock<std::mutex> lock(state_mutex_);
    if (!state_condition_.wait_for(lock, config_.health_timeout,
                                   [this] { return stop_requested_ || websocket_open_ || !websocket_error_.empty(); }))
        return transport_error("websocket connection timed out");
    if (stop_requested_)
        return {GatewayErrorCode::Cancelled, "GatewayService was stopped"};
    if (!websocket_error_.empty())
        return transport_error(websocket_error_);
    if (!websocket_open_)
        return transport_error("websocket did not connect");
    return {};
}

void GatewayService::handle_websocket_message(const std::string& message)
{
    const auto parsed = parse_json_object(message);
    if (!parsed.has_value())
        return;

    const RpcFrame frame = classify_jsonrpc_message(*parsed);
    if (frame.type == RpcFrameType::Result) {
        complete_pending(frame.id, {}, frame.result);
        return;
    }
    if (frame.type == RpcFrameType::Error) {
        const std::string error_message = frame.error.is_object() ? frame.error.value("message", "JSON-RPC error") : "JSON-RPC error";
        const int         error_code    = frame.error.is_object() ? frame.error.value("code", 0) : 0;
        complete_pending(frame.id, {GatewayErrorCode::RpcError, error_message, error_code}, nlohmann::json::object());
        return;
    }
    if (frame.type != RpcFrameType::Notification)
        return;

    NotificationCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        const auto                  handler = notification_handlers_.find(frame.method);
        if (handler != notification_handlers_.end())
            callback = handler->second;
    }
    if (callback)
        dependencies_.dispatcher([callback, params = frame.params]() mutable { callback(params); });
}

void GatewayService::handle_websocket_closed(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        websocket_open_  = false;
        websocket_error_ = reason;
    }
    state_condition_.notify_all();
}

void GatewayService::dispatch_state(ConnectionState state, GatewayError error)
{
    StateCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = state_callback_;
    }
    if (callback)
        dependencies_.dispatcher([callback, state, error]() mutable { callback(state, error); });
}

void GatewayService::complete_pending(std::int64_t id, GatewayError error, const nlohmann::json& result)
{
    PendingRequest request;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        const auto                  pending = pending_requests_.find(id);
        if (pending == pending_requests_.end())
            return;
        request = std::move(pending->second);
        pending_requests_.erase(pending);
    }
    if (request.callback)
        dependencies_.dispatcher([callback = std::move(request.callback), error, result]() mutable { callback(error, result); });
}

void GatewayService::fail_pending(const GatewayError& error)
{
    std::map<std::int64_t, PendingRequest> requests;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        requests.swap(pending_requests_);
    }
    for (auto& item : requests) {
        if (item.second.callback) {
            dependencies_.dispatcher(
                [callback = std::move(item.second.callback), error]() mutable { callback(error, nlohmann::json::object()); });
        }
    }
}

void GatewayService::set_websocket_open(bool opened)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        websocket_open_ = opened;
        if (opened)
            websocket_error_.clear();
    }
    state_condition_.notify_all();
}

GatewayService::ApiResult GatewayService::get_json(const std::string& path)
{
    ApiResult     result;
    std::uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != ConnectionState::Connected) {
            result.error = {GatewayErrorCode::NotConnected, "gateway is not connected"};
            return result;
        }
        port = port_;
    }
    const HttpResponse response = dependencies_.http->get(make_url(config_.host, port, path));
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        result.error = {GatewayErrorCode::HttpError,
                        response.error.empty() ? "request failed with HTTP status " + std::to_string(response.status) : response.error};
        return result;
    }
    const auto parsed = parse_json_object(response.body);
    if (!parsed.has_value()) {
        result.error = {GatewayErrorCode::InvalidResponse, "response is not a JSON object"};
        return result;
    }
    result.value = *parsed;
    return result;
}

GatewayService::ApiResult GatewayService::post_json(const std::string& path, const nlohmann::json& body)
{
    ApiResult   result;
    std::string base_url;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != ConnectionState::Connected) {
            result.error = {GatewayErrorCode::NotConnected, "gateway is not connected"};
            return result;
        }
        base_url = health_.base_url;
    }
    if (base_url.empty())
        return {{GatewayErrorCode::HealthNotReady, "gateway web base URL is not available"}, {}};

    const HttpResponse response = dependencies_.http->post_json(join_url(base_url, path), body.dump());
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        result.error = {GatewayErrorCode::HttpError,
                        response.error.empty() ? "request failed with HTTP status " + std::to_string(response.status) : response.error};
        return result;
    }
    const auto parsed = parse_json_object(response.body);
    if (!parsed.has_value()) {
        result.error = {GatewayErrorCode::InvalidResponse, "response is not a JSON object"};
        return result;
    }
    result.value = *parsed;
    return result;
}

std::int64_t GatewayService::next_request_id() { return next_id_.fetch_add(1, std::memory_order_relaxed); }

}} // namespace Slic3r::Gateway
