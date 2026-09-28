#ifndef slic3r_GatewayMachineSlots_hpp_
#define slic3r_GatewayMachineSlots_hpp_

#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace Slic3r { namespace Gateway {

bool parse_gateway_machine_slots(const nlohmann::json& result, std::vector<::ConnectMachineInfo>& slots);
bool gateway_delta_affects_machine_slots(const nlohmann::json& delta);
bool gateway_machine_slots_equal(const std::vector<::ConnectMachineInfo>& left, const std::vector<::ConnectMachineInfo>& right);

}} // namespace Slic3r::Gateway

#endif // slic3r_GatewayMachineSlots_hpp_
