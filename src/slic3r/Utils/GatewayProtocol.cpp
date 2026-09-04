#include "GatewayProtocol.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace Gateway {
namespace {

bool get_string(const nlohmann::json& object, const char* key, std::string& value)
{
    const auto item = object.find(key);
    if (item == object.end() || !item->is_string())
        return false;
    value = item->get<std::string>();
    return true;
}

bool get_optional_string(const nlohmann::json& object, const char* key, std::string& value)
{
    const auto item = object.find(key);
    if (item == object.end())
        return true;
    if (!item->is_string())
        return false;
    value = item->get<std::string>();
    return true;
}

} // namespace

nlohmann::json build_jsonrpc_request(std::int64_t id, std::string_view method, const nlohmann::json& params)
{
    nlohmann::json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::string{method}}};
    request["params"] = params.is_null() ? nlohmann::json::object() : params;
    return request;
}

std::optional<nlohmann::json> parse_json_object(const std::string& body)
{
    if (body.empty())
        return std::nullopt;
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return std::nullopt;
    return parsed;
}

GatewayError parse_health(const std::string& body, HealthInfo& health)
{
    const auto parsed = parse_json_object(body);
    if (!parsed.has_value())
        return {GatewayErrorCode::InvalidResponse, "health response is not a JSON object"};

    const nlohmann::json& root    = *parsed;
    const auto            status  = root.find("status");
    const auto            ok      = root.find("ok");
    const bool            healthy = (status != root.end() && status->is_string() && status->get<std::string>() == "ok") ||
                                    (ok != root.end() && ok->is_boolean() && ok->get<bool>());
    if (!healthy)
        return {GatewayErrorCode::HealthNotReady, "health endpoint did not report ok"};

    const auto components = root.find("components");
    if (components == root.end() || !components->is_object())
        return {GatewayErrorCode::InvalidResponse, "health components are missing"};
    for (const char* component_name : {"ipc_server", "web_server"}) {
        const auto component = components->find(component_name);
        if (component == components->end() || !component->is_string() || component->get<std::string>() != "ok")
            return {GatewayErrorCode::HealthNotReady, std::string{"component is not ready: "} + component_name};
    }

    const auto server_url = root.find("server_url");
    if (server_url == root.end() || !server_url->is_object())
        return {GatewayErrorCode::InvalidResponse, "health server_url is missing"};
    if (!get_string(*server_url, "base_url", health.base_url) || health.base_url.empty())
        return {GatewayErrorCode::InvalidResponse, "health base_url is missing"};

    get_string(root, "cli_version", health.cli_version);
    const auto device_connected = root.find("device_connected");
    const auto device_sn        = root.find("sn");
    if (device_connected != root.end()) {
        if (!device_connected->is_boolean())
            return {GatewayErrorCode::InvalidResponse, "health device_connected must be a boolean"};
        if (device_sn != root.end() && !device_sn->is_string())
            return {GatewayErrorCode::InvalidResponse, "health sn must be a string"};

        health.has_device_state = true;
        health.device_connected = device_connected->get<bool>();
        if (device_sn != root.end())
            health.device_sn = device_sn->get<std::string>();
    }
    health.pages.clear();
    for (const auto& item : server_url->items()) {
        if (item.key() == "base_url")
            continue;
        if (item.value().is_string())
            health.pages.emplace(item.key(), item.value().get<std::string>());
    }
    return {};
}

std::optional<ActiveDeviceSnapshot> parse_active_device(const nlohmann::json& params)
{
    if (!params.is_object())
        return std::nullopt;

    const nlohmann::json* source = &params;
    const auto            device = params.find("device");
    if (device != params.end()) {
        if (!device->is_object())
            return std::nullopt;
        source = &*device;
    }

    ActiveDeviceSnapshot active_device;
    for (const char* key : {"sn", "device_sn", "serial_number"}) {
        if (get_string(*source, key, active_device.serial_number) && !active_device.serial_number.empty())
            break;
        active_device.serial_number.clear();
    }
    if (active_device.serial_number.empty())
        return std::nullopt;

    const auto connected = source->find("connected");
    if (connected == source->end() || !connected->is_boolean())
        return std::nullopt;
    active_device.connected = connected->get<bool>();

    if (!get_optional_string(*source, "machine_type", active_device.machine_type) ||
        !get_optional_string(*source, "device_name", active_device.device_name) ||
        !get_optional_string(*source, "preset_name", active_device.preset_name))
        return std::nullopt;

    for (const char* key : {"nozzle_diameters", "nozzle_sizes"}) {
        const auto diameters = source->find(key);
        if (diameters == source->end())
            continue;
        if (!diameters->is_array())
            return std::nullopt;
        for (const auto& diameter : *diameters) {
            if (!diameter.is_string())
                return std::nullopt;
            active_device.nozzle_diameters.push_back(diameter.get<std::string>());
        }
        break;
    }

    active_device.valid = true;
    return active_device;
}

RpcFrame classify_jsonrpc_message(const nlohmann::json& message)
{
    RpcFrame frame;
    if (!message.is_object())
        return frame;

    const auto id         = message.find("id");
    const auto method     = message.find("method");
    const bool has_id     = id != message.end() && (id->is_number_integer() || id->is_string());
    const bool has_result = message.contains("result");
    const bool has_error  = message.contains("error");

    if (has_id && has_result) {
        frame.type   = RpcFrameType::Result;
        frame.id     = id->is_number_integer() ? id->get<std::int64_t>() : 0;
        frame.result = *message.find("result");
        return frame;
    }
    if (has_id && has_error) {
        frame.type  = RpcFrameType::Error;
        frame.id    = id->is_number_integer() ? id->get<std::int64_t>() : 0;
        frame.error = *message.find("error");
        return frame;
    }
    if (method != message.end() && method->is_string()) {
        frame.type   = RpcFrameType::Notification;
        frame.method = method->get<std::string>();
        frame.params = message.value("params", nlohmann::json::object());
    }
    return frame;
}

namespace detail {

ReconnectPolicy::ReconnectPolicy(Config config) : config_(std::move(config))
{
    if (config_.initial_delay.count() <= 0)
        config_.initial_delay = std::chrono::milliseconds{1};
    if (config_.max_delay < config_.initial_delay)
        config_.max_delay = config_.initial_delay;
    config_.jitter_fraction = std::clamp(config_.jitter_fraction, 0.0, 1.0);
}

void ReconnectPolicy::record_connected(std::int64_t now_ms) { connected_since_ms_ = now_ms; }

void ReconnectPolicy::record_failure(std::int64_t now_ms)
{
    if (connected_since_ms_.has_value() && now_ms - *connected_since_ms_ >= config_.stable_connection_time.count())
        attempts_ = 0;
    ++attempts_;
    connected_since_ms_.reset();
}

void ReconnectPolicy::reset()
{
    attempts_ = 0;
    connected_since_ms_.reset();
}

bool ReconnectPolicy::should_retry() const { return config_.max_attempts == 0 || attempts_ < config_.max_attempts; }

std::chrono::milliseconds ReconnectPolicy::next_delay(double jitter_ratio) const
{
    if (attempts_ == 0)
        return config_.initial_delay;

    std::uint64_t       shift  = std::min<std::uint64_t>(attempts_ - 1, 20);
    const std::uint64_t base   = static_cast<std::uint64_t>(config_.initial_delay.count()) << shift;
    const std::int64_t  capped = std::min<std::uint64_t>(base, static_cast<std::uint64_t>(config_.max_delay.count()));
    const double        jitter = capped * config_.jitter_fraction * std::clamp(jitter_ratio, 0.0, 1.0);
    return std::chrono::milliseconds{capped + static_cast<std::int64_t>(jitter)};
}

} // namespace detail

}} // namespace Slic3r::Gateway
