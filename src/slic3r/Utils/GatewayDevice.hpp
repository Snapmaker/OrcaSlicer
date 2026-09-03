#ifndef slic3r_Utils_GatewayDevice_hpp_
#define slic3r_Utils_GatewayDevice_hpp_

#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace Gateway {

class GatewayService;

// Device-facing queries over the connection gateway, written against the
// snapmaker_connection API contract (WS JSON-RPC / HTTP). One method per
// legacy SSWCP/MQTT interface as it is migrated; nothing here may call back
// into the SSWCP/PrintHost stack.
class GatewayDevice
{
public:
    // Replacement for SSWCP::query_machine_info. Fresh values via the
    // transparent RPC machine.system_info (result.system_info.product_info).
    // Returns false when the gateway is down or no device is connected.
    static bool query_machine_info(const std::shared_ptr<GatewayService>& gateway, std::string& out_model, std::vector<std::string>& out_nozzle_diameters,
                                    std::string& device_name);

    // True when a device is connected through the gateway. machine.system_info
    // reaches the device, so a successful call proves a live device channel.
    static bool is_device_connected(const std::shared_ptr<GatewayService>& gateway);
};

}} // namespace Slic3r::Gateway

#endif // slic3r_Utils_GatewayDevice_hpp_
