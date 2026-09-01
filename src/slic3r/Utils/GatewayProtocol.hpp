#ifndef slic3r_Utils_GatewayProtocol_hpp_
#define slic3r_Utils_GatewayProtocol_hpp_

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r { namespace Gateway {

enum class GatewayErrorCode {
    None,
    Cancelled,
    NotConnected,
    TransportError,
    HttpError,
    InvalidRequest,
    InvalidResponse,
    HealthNotReady,
    RpcError,
    ReconnectFailed,
};

struct GatewayError
{
    GatewayErrorCode code{GatewayErrorCode::None};
    std::string      message;
    int              rpc_code{0};

    explicit operator bool() const { return code != GatewayErrorCode::None; }
};

struct HealthInfo
{
    std::string                        cli_version;
    std::string                        base_url;
    std::map<std::string, std::string> pages;
};

struct HttpResponse
{
    unsigned    status{0};
    std::string body;
    std::string error;
};

struct PreprintStoreResult
{
    GatewayError error;
    bool        ok{false};
    bool        file_exists{true};
};

enum class RpcFrameType { Unknown, Result, Error, Notification };

struct RpcFrame
{
    RpcFrameType   type{RpcFrameType::Unknown};
    std::int64_t   id{0};
    std::string    method;
    nlohmann::json result;
    nlohmann::json error;
    nlohmann::json params;
};

nlohmann::json                build_jsonrpc_request(std::int64_t id, std::string_view method, const nlohmann::json& params);
GatewayError                  parse_health(const std::string& body, HealthInfo& health);
RpcFrame                      classify_jsonrpc_message(const nlohmann::json& message);
std::optional<nlohmann::json> parse_json_object(const std::string& body);

namespace detail {

class ReconnectPolicy
{
public:
    struct Config
    {
        std::chrono::milliseconds initial_delay{std::chrono::milliseconds{250}};
        std::chrono::milliseconds max_delay{std::chrono::milliseconds{5000}};
        std::chrono::milliseconds stable_connection_time{std::chrono::milliseconds{30000}};
        std::uint32_t             max_attempts{0}; // Zero means unlimited.
        double                    jitter_fraction{0.0};
    };

    explicit ReconnectPolicy(Config config);

    void                      record_connected(std::int64_t now_ms);
    void                      record_failure(std::int64_t now_ms);
    void                      reset();
    bool                      should_retry() const;
    std::chrono::milliseconds next_delay(double jitter_ratio = 0.0) const;

    std::uint32_t attempts() const { return attempts_; }

private:
    Config                      config_;
    std::uint32_t               attempts_{0};
    std::optional<std::int64_t> connected_since_ms_;
};

} // namespace detail

}} // namespace Slic3r::Gateway

#endif // slic3r_Utils_GatewayProtocol_hpp_
