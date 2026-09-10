#include "GatewayProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

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

const nlohmann::json* find_array(const nlohmann::json& object, const char* key)
{
    const auto item = object.find(key);
    return item != object.end() && item->is_array() ? &*item : nullptr;
}

std::string array_string(const nlohmann::json& array, size_t index, const std::string& fallback = {})
{ return index < array.size() && array[index].is_string() ? array[index].get<std::string>() : fallback; }

std::string filament_color(const nlohmann::json& config, size_t index)
{
    if (const nlohmann::json* colors = find_array(config, "filament_color_rgba");
        colors != nullptr && index < colors->size() && (*colors)[index].is_string())
        return (*colors)[index].get<std::string>();

    const nlohmann::json* colors = find_array(config, "filament_color");
    if (colors == nullptr || index >= colors->size())
        return "#FFFFFF";

    const nlohmann::json& value = (*colors)[index];
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_unsigned() || value.is_number_integer()) {
        const std::uint64_t argb = (value.is_number_unsigned() ? value.get<std::uint64_t>() :
                                                                 static_cast<std::uint64_t>(value.get<std::int64_t>())) &
                                   0xffffffffULL;
        const std::uint64_t rgba = ((argb << 8) & 0xffffffffULL) | (argb >> 24);
        std::ostringstream  stream;
        stream << std::uppercase << std::setfill('0') << std::setw(8) << std::hex << rgba;
        return stream.str();
    }
    return "#FFFFFF";
}

std::string nozzle_value(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number())
        return value.dump();
    return {};
}

std::vector<std::string> nozzle_diameters(const nlohmann::json& params, size_t filament_count)
{
    std::vector<std::string> diameters;
    if (const auto extruder = params.find("extruder"); extruder != params.end() && extruder->is_object()) {
        if (const auto value = extruder->find("nozzle_diameter"); value != extruder->end())
            diameters.push_back(nozzle_value(*value));
    }
    for (size_t index = 1; index < filament_count; ++index) {
        const std::string key      = "extruder" + std::to_string(index);
        const auto        extruder = params.find(key);
        if (extruder == params.end())
            continue;
        const auto value = extruder->find("nozzle_diameter");
        if (value != extruder->end())
            diameters.push_back(nozzle_value(*value));
    }
    return diameters;
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

std::string parse_device_sn(const nlohmann::json& params)
{
    if (!params.is_object())
        return {};

    const nlohmann::json* source = &params;
    const auto            device = params.find("device");
    if (device != params.end() && device->is_object())
        source = &*device;

    for (const char* key : {"sn", "device_sn", "serial_number"}) {
        const auto value = source->find(key);
        if (value != source->end() && value->is_string() && !value->get<std::string>().empty())
            return value->get<std::string>();
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
    active_device.serial_number = parse_device_sn(params);
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

std::optional<nlohmann::json> build_machine_snapshot_from_device_objects(const nlohmann::json& params, const std::string& serial_number)
{
    if (!params.is_object())
        return std::nullopt;

    const auto config_item = params.find("print_task_config");
    if (config_item == params.end() || !config_item->is_object() || serial_number.empty())
        return std::nullopt;

    const nlohmann::json& config  = *config_item;
    const nlohmann::json* vendors = find_array(config, "filament_vendor");
    const nlohmann::json* types   = find_array(config, "filament_type");
    if (vendors == nullptr || types == nullptr)
        return std::nullopt;

    const nlohmann::json*    sub_types    = find_array(config, "filament_sub_type");
    const nlohmann::json*    officials    = find_array(config, "filament_official");
    const nlohmann::json*    exists       = find_array(config, "filament_exist");
    const nlohmann::json*    extruders    = find_array(config, "extruder_map_table");
    const nlohmann::json*    multi_colors = find_array(config, "filament_color_multi");
    const size_t             count        = std::max(vendors->size(), types->size());
    std::vector<std::string> nozzles      = nozzle_diameters(params, count);

    nlohmann::json filaments = nlohmann::json::array();
    for (size_t index = 0; index < count; ++index) {
        std::string vendor          = array_string(*vendors, index, "NONE");
        std::string type            = array_string(*types, index, "NONE");
        const bool  has_exist_state = exists != nullptr && index < exists->size();
        const bool  exists_at_index = !has_exist_state || ((*exists)[index].is_boolean() && (*exists)[index].get<bool>());
        if (has_exist_state && !exists_at_index) {
            vendor = "NONE";
            type   = "NONE";
        }

        nlohmann::json colors = nlohmann::json::array();
        if (multi_colors != nullptr && index < multi_colors->size() && (*multi_colors)[index].is_object()) {
            const auto color_list = (*multi_colors)[index].find("colors");
            if (color_list != (*multi_colors)[index].end() && color_list->is_array()) {
                for (const auto& color : *color_list)
                    if (color.is_string())
                        colors.push_back(color.get<std::string>());
            }
        }

        std::int64_t extruder = static_cast<std::int64_t>(index);
        if (extruders != nullptr && index < extruders->size() && (*extruders)[index].is_number_integer()) {
            const std::int64_t mapped_extruder = (*extruders)[index].get<std::int64_t>();
            if (mapped_extruder >= 0 && mapped_extruder <= static_cast<std::int64_t>(std::numeric_limits<int>::max()))
                extruder = mapped_extruder;
        }

        nlohmann::json mode;
        bool           has_mode = false;
        if (multi_colors != nullptr && index < multi_colors->size() && (*multi_colors)[index].is_object()) {
            const auto mode_item = (*multi_colors)[index].find("mode");
            if (mode_item != (*multi_colors)[index].end() && mode_item->is_number_integer()) {
                mode     = *mode_item;
                has_mode = true;
            }
        }

        nlohmann::json filament{{"index", index},
                                {"extruder", extruder},
                                {"official", officials != nullptr && index < officials->size() && (*officials)[index].is_boolean() &&
                                                 (*officials)[index].get<bool>()},
                                {"vendor", vendor},
                                {"type", type},
                                {"sub_type", array_string(sub_types ? *sub_types : nlohmann::json::array(), index, "NONE")},
                                {"color", filament_color(config, index)},
                                {"multi_colors", std::move(colors)}};
        if (index < nozzles.size())
            filament["nozzle"] = nozzles[index];
        if (has_mode)
            filament["color_mode"] = mode.get<std::int64_t>();
        filaments.push_back(std::move(filament));
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return nlohmann::json{{"revision", std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()},
                          {"sn", serial_number},
                          {"nozzle_diameters", nozzles},
                          {"filaments", std::move(filaments)}};
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
