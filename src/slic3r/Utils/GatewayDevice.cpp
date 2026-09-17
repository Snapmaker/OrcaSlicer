#include "GatewayDevice.hpp"
#include "GatewayService.hpp"

#include "libslic3r/SSWCPProtocol.hpp"
#include "libslic3r/PrintConfig.hpp"

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

bool GatewayDevice::query_machine_info(const std::shared_ptr<GatewayService>& gateway,
                                       std::string&                           out_model,
                                       std::vector<std::string>&              out_nozzle_diameters,
                                       std::vector<std::string>&              out_nozzle_volume_types,
                                       std::string&                           device_name)
{
    if (gateway == nullptr)
        return false;

    // GET /api/cache/all is the gateway's read-only full snapshot of the current device, so the
    // firmware passthrough machine.system_info is no longer needed for these fields.
    const GatewayService::ApiResult result = gateway->get_device();
    if (result.error) {
        // NotConnected (gateway process not ready yet) is an expected answer; anything else is worth a log line.
        if (result.error.code != GatewayErrorCode::NotConnected)
            BOOST_LOG_TRIVIAL(warning) << "GatewayDevice::query_machine_info: GET /api/cache/all failed: " << result.error.message;
        return false;
    }

    const nlohmann::json* data     = find_object(result.value, "data");
    const nlohmann::json* info     = data == nullptr ? nullptr : find_object(*data, "info");
    const nlohmann::json* product  = info == nullptr ? nullptr : find_object(*info, "product");
    const nlohmann::json* identity = data == nullptr ? nullptr : find_object(*data, "identity");

    std::string model = product == nullptr ? std::string{} : get_string(*product, "machine_type");
    if (model.empty() && identity != nullptr)
        model = get_string(*identity, "productName");
    std::vector<std::string> nozzles;
    std::string name = identity == nullptr ? std::string{} : get_string(*identity, "name");
    if (name.empty() && identity != nullptr)
        name = get_string(*identity, "device_name");
    if (name.empty() && product != nullptr)
        name = get_string(*product, "device_name");
    if (product != nullptr && product->contains("nozzle_diameter")) {
        const nlohmann::json& nozzle_json = (*product)["nozzle_diameter"];
        if (nozzle_json.is_array()) {
            for (const auto& nozzle : nozzle_json)
                append_nozzle(nozzle, nozzles);
        } else if (!nozzle_json.is_null()) {
            append_nozzle(nozzle_json, nozzles);
        }
    }

    // nozzle_volume_type lives on the extruder objects in data.objects, not in data.info.product.
    std::vector<std::string> flows;
    const nlohmann::json* objects = data == nullptr ? nullptr : find_object(*data, "objects");
    if (objects == nullptr || objects->empty()) {
        // Preserve the legacy resolver's behavior when the snapshot has no runtime objects.
        if (!nozzles.empty())
            flows.assign(nozzles.size(), FLOW_MODE_STANDARD);
    } else {
        std::vector<std::string> object_diameters;
        if (!SSWCPProtocol::parse_extruder_objects(*objects, object_diameters, flows) || object_diameters.size() != nozzles.size())
            flows.clear();
    }

    // An answer without a machine type is not usable: callers gate on it (e.g. the
    // "Snapmaker U1" whitelist), so treat a malformed payload as a failed query.
    if (model.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "GatewayDevice::query_machine_info: GET /api/cache/all returned no machine_type";
        return false;
    }

    out_model               = std::move(model);
    out_nozzle_diameters    = std::move(nozzles);
    out_nozzle_volume_types = std::move(flows);
    device_name             = std::move(name);

    return true;
}

bool GatewayDevice::is_device_connected(const std::shared_ptr<GatewayService>& gateway)
{
    std::string              model;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    std::string              name;
    return query_machine_info(gateway, model, nozzles, flows, name);
}

}} // namespace Slic3r::Gateway
