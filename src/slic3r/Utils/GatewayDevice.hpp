#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

struct ConnectMachineInfo;

namespace Slic3r {

namespace Gateway {

class GatewayService;

// Device-facing queries over the connection gateway, written against the
// snapmaker_connection API contract (WS JSON-RPC / HTTP). One method per
// legacy SSWCP/MQTT interface as it is migrated; nothing here may call back
// into the SSWCP/PrintHost stack.
class GatewayDevice
{
public:
    // Replacement for SSWCP::query_machine_info. Fresh values via the gateway's
    // read-only device snapshot GET /api/cache/all (data.info.product for the model,
    // data.objects.extruder* for the per-extruder nozzle_volume_type).
    // Returns false when the gateway is down or no device is connected.
    static bool query_machine_info(const std::shared_ptr<GatewayService>& gateway,
                                   std::string&                           out_model,
                                   std::vector<std::string>&              out_nozzle_diameters,
                                   std::vector<std::string>&              out_nozzle_volume_types,
                                   std::string&                           device_name);

    static bool parse_machine_slots(const nlohmann::json& result, std::vector<ConnectMachineInfo>& slots);
    static bool delta_affects_machine_slots(const nlohmann::json& delta);
    static bool machine_slots_equal(const std::vector<ConnectMachineInfo>& left, const std::vector<ConnectMachineInfo>& right);
};

}} // namespace Slic3r::Gateway
