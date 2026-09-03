#include "GatewayDevice.hpp"
#include "GatewayService.hpp"

#include <boost/log/trivial.hpp>

#include <cmath>
#include <cstdlib>

namespace Slic3r { namespace Gateway {

namespace {

const nlohmann::json* find_object(const nlohmann::json& parent, const char* key)
{
    const auto it = parent.find(key);
    return it != parent.end() && it->is_object() ? &*it : nullptr;
}

std::string get_string(const nlohmann::json& parent, const char* key)
{
    const auto it = parent.find(key);
    return it != parent.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// Normalize a nozzle diameter into the "0.2"/"0.4"/"0.6"/"0.8" preset labels used by the UI.
// Diameters outside the supported preset catalog are dropped on purpose — same mapping as the
// legacy SSWCP::query_machine_info behavior.
void append_nozzle(const nlohmann::json& value, std::vector<std::string>& out_nozzle_diameters)
{
    double diameter = 0.0;
    if (value.is_number())
        diameter = value.get<double>();
    else if (value.is_string())
        diameter = std::atof(value.get<std::string>().c_str());
    if (std::fabs(diameter - 0.2) < 1e-6)
        out_nozzle_diameters.push_back("0.2");
    else if (std::fabs(diameter - 0.4) < 1e-6)
        out_nozzle_diameters.push_back("0.4");
    else if (std::fabs(diameter - 0.6) < 1e-6)
        out_nozzle_diameters.push_back("0.6");
    else if (std::fabs(diameter - 0.8) < 1e-6)
        out_nozzle_diameters.push_back("0.8");
}

} // namespace

bool GatewayDevice::query_machine_info(const std::shared_ptr<GatewayService>& gateway, std::string& out_model, std::vector<std::string>& out_nozzle_diameters,
                                        std::string& device_name)
{
    if (gateway == nullptr)
        return false;

    const GatewayService::ApiResult result = gateway->request_sync("machine.system_info", nlohmann::json::object(), std::chrono::milliseconds{5000});
    if (result.error) {
        // -32000 not_connected (no device) is an expected answer; anything else is worth a log line.
        if (result.error.code != GatewayErrorCode::NotConnected && result.error.code != GatewayErrorCode::RpcError)
            BOOST_LOG_TRIVIAL(warning) << "GatewayDevice::query_machine_info: gateway request failed: " << result.error.message;
        return false;
    }

    const nlohmann::json* system_info = find_object(result.value, "system_info");
    if (system_info == nullptr)
        return false;

    const nlohmann::json* product_info = find_object(*system_info, "product_info");
    if (product_info == nullptr)
        return false;

    std::string              model = get_string(*product_info, "machine_type");
    std::vector<std::string> nozzles;
    std::string              name = get_string(*product_info, "device_name");
    if (product_info->contains("nozzle_diameter")) {
        const nlohmann::json& nozzle_json = (*product_info)["nozzle_diameter"];
        if (nozzle_json.is_array()) {
            for (const auto& nozzle : nozzle_json)
                append_nozzle(nozzle, nozzles);
        } else {
            append_nozzle(nozzle_json, nozzles);
        }
    }

    // An answer without a machine type is not usable: callers gate on it (e.g. the
    // "Snapmaker U1" whitelist), so treat a malformed payload as a failed query.
    if (model.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "GatewayDevice::query_machine_info: machine.system_info returned no machine_type";
        return false;
    }

    out_model            = std::move(model);
    out_nozzle_diameters = std::move(nozzles);
    device_name          = std::move(name);

    return true;
}

bool GatewayDevice::is_device_connected(const std::shared_ptr<GatewayService>& gateway)
{
    std::string              model;
    std::vector<std::string> nozzles;
    std::string              name;
    return query_machine_info(gateway, model, nozzles, name);
}

}} // namespace Slic3r::Gateway
