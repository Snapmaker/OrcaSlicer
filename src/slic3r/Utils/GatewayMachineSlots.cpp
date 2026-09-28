#include "GatewayMachineSlots.hpp"

#include "GatewayProtocol.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SSWCPProtocol.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace Slic3r { namespace Gateway {
namespace {

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

std::string machine_filament_display_name(const std::string& vendor, const std::string& type, const std::string& sub_type)
{
    if (sub_type == "Support")
        return vendor + " Support For " + type;
    return vendor + " " + type + ((sub_type != "NONE" && !sub_type.empty()) ? " " + sub_type : "");
}

std::string nozzle_value(const nlohmann::json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number())
        return value.dump();
    return {};
}

std::string flow_value(const nlohmann::json& value)
{
    if (!value.is_string())
        return {};
    const std::string flow = value.get<std::string>();
    return flow == FLOW_MODE_HIGH_FLOW ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD;
}

void apply_config_nozzle_values(const nlohmann::json&     config,
                                size_t                    count,
                                std::vector<std::string>& diameters,
                                std::vector<std::string>& flows)
{
    if (const nlohmann::json* values = find_array(config, "nozzle_diameters"); values != nullptr)
        for (size_t index = 0; index < count && index < values->size(); ++index) {
            diameters.resize(std::max(diameters.size(), index + 1));
            diameters[index] = nozzle_value((*values)[index]);
        }
    if (const nlohmann::json* values = find_array(config, "nozzle_volume_types"); values != nullptr)
        for (size_t index = 0; index < count && index < values->size(); ++index) {
            flows.resize(std::max(flows.size(), index + 1));
            flows[index] = flow_value((*values)[index]);
        }
}

} // namespace

bool parse_gateway_machine_slots(const nlohmann::json& result, std::vector<::ConnectMachineInfo>& slots)
{
    slots.clear();
    const auto objects_value = parse_device_object_query_result(result);
    if (!objects_value.has_value() || !objects_value->is_object())
        return false;

    const nlohmann::json& objects     = *objects_value;
    const auto            config_item = objects.find("print_task_config");
    if (config_item == objects.end() || !config_item->is_object())
        return false;

    const nlohmann::json& config  = *config_item;
    const nlohmann::json* vendors = find_array(config, "filament_vendor");
    const nlohmann::json* types   = find_array(config, "filament_type");
    if (vendors == nullptr || types == nullptr)
        return false;

    const size_t count = std::max(vendors->size(), types->size());
    if (count > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    std::vector<std::string> nozzles;
    std::vector<std::string> flows;
    SSWCPProtocol::parse_extruder_objects(objects, nozzles, flows);
    apply_config_nozzle_values(config, count, nozzles, flows);
    nozzles.resize(count);
    flows.resize(count, FLOW_MODE_STANDARD);

    const nlohmann::json* sub_types    = find_array(config, "filament_sub_type");
    const nlohmann::json* exists       = find_array(config, "filament_exist");
    const nlohmann::json* multi_colors = find_array(config, "filament_color_multi");

    std::vector<::ConnectMachineInfo> parsed;
    parsed.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        std::string vendor          = array_string(*vendors, index, "NONE");
        std::string type            = array_string(*types, index, "NONE");
        const bool  has_exist_state = exists != nullptr && index < exists->size();
        if (has_exist_state && (!(*exists)[index].is_boolean() || !(*exists)[index].get<bool>())) {
            vendor = "NONE";
            type   = "NONE";
        }

        ::ConnectMachineInfo slot;
        slot.index         = static_cast<int>(index);
        slot.filament_info = machine_filament_display_name(vendor, type,
                                                           array_string(sub_types ? *sub_types : nlohmann::json::array(), index, "NONE"));
        slot.filament_type = type;
        slot.nozzle_info   = index < nozzles.size() ? nozzles[index] : std::string{};
        if (slot.nozzle_info.empty())
            return false;
        slot.nozzle_volume_type = index < flows.size() ? flows[index] : std::string{FLOW_MODE_STANDARD};
        slot.color_info         = NormalizeFilamentHexColor(filament_color(config, index), "#FFFFFF");

        if (multi_colors != nullptr && index < multi_colors->size() && (*multi_colors)[index].is_object()) {
            const auto& color_entry = (*multi_colors)[index];
            if (const auto colors = color_entry.find("colors"); colors != color_entry.end() && colors->is_array())
                for (const auto& color : *colors)
                    if (color.is_string()) {
                        const std::string normalized_color = NormalizeFilamentHexColor(color.get<std::string>());
                        if (!normalized_color.empty())
                            slot.multiColors.emplace_back(normalized_color);
                    }
            if (const auto mode = color_entry.find("mode"); mode != color_entry.end() && mode->is_number_integer() &&
                                                            mode->get<std::int64_t>() >= 0 &&
                                                            mode->get<std::int64_t>() <= std::numeric_limits<int>::max())
                slot.colorMode = FilamentColorModeFromConfig(static_cast<int>(mode->get<std::int64_t>()));
        }
        if (slot.multiColors.empty() && !slot.color_info.empty())
            slot.multiColors.emplace_back(slot.color_info);
        if (slot.multiColors.size() <= 1)
            slot.colorMode = FilamentColorMode::Segment;

        parsed.emplace_back(std::move(slot));
    }

    slots = std::move(parsed);
    return true;
}

bool gateway_delta_affects_machine_slots(const nlohmann::json& delta)
{
    if (!delta.is_object())
        return false;
    if (delta.contains("print_task_config"))
        return true;
    for (auto item = delta.begin(); item != delta.end(); ++item) {
        if (item.key() == "extruder")
            return true;
        if (item.key().size() > 8 && item.key().compare(0, 8, "extruder") == 0 &&
            std::all_of(item.key().begin() + 8, item.key().end(), [](unsigned char character) { return std::isdigit(character) != 0; }))
            return true;
    }
    if (const auto objects = delta.find("objects"); objects != delta.end() && objects->is_object())
        return gateway_delta_affects_machine_slots(*objects);
    return false;
}

bool gateway_machine_slots_equal(const std::vector<::ConnectMachineInfo>& left, const std::vector<::ConnectMachineInfo>& right)
{
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const ::ConnectMachineInfo& lhs = left[index];
        const ::ConnectMachineInfo& rhs = right[index];
        if (lhs.index != rhs.index || lhs.filament_info != rhs.filament_info || lhs.filament_type != rhs.filament_type ||
            lhs.nozzle_info != rhs.nozzle_info || lhs.nozzle_volume_type != rhs.nozzle_volume_type || lhs.color_info != rhs.color_info ||
            lhs.multiColors != rhs.multiColors || lhs.colorMode != rhs.colorMode)
            return false;
    }
    return true;
}

}} // namespace Slic3r::Gateway
