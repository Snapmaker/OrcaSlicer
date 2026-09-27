#include "SSWCPProtocol.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <utility>

namespace Slic3r { namespace SSWCPProtocol {

namespace {

bool is_valid_flow_type(const std::string &value)
{
    // Only the standard and high-flow nozzle modes are recognized flow types.
    return value == FLOW_MODE_STANDARD || value == FLOW_MODE_HIGH_FLOW;
}

// One entry of a flow type list. An unstated entry is "standard" and does not count as a
// statement; false for a value that is neither unstated nor a known flow type.
bool append_flow_type(const nlohmann::json &entry, std::vector<std::string> &out, bool &any_stated)
{
    if (is_unstated_flow_type(entry)) {
        out.emplace_back(FLOW_MODE_STANDARD);
        return true;
    }
    if (!entry.is_string()) return false;
    const std::optional<std::string> type = normalize_flow_type(entry.get<std::string>());
    if (!type) return false;
    out.push_back(*type);
    any_stated = true;
    return true;
}

bool parse_system_flow_types(const nlohmann::json &value, std::vector<std::string> &out)
{
    std::vector<std::string> parsed;
    bool                     any_stated = false;
    const auto               append = [&parsed, &any_stated](const nlohmann::json &entry) {
        return append_flow_type(entry, parsed, any_stated);
    };

    if (value.is_array()) {
        for (const nlohmann::json &entry : value)
            if (!append(entry)) return false;
    } else if (!append(value)) {
        return false;
    }

    // Nothing but unstated entries: the firmware reports no flow types.
    if (!any_stated)
        parsed.clear();
    out = std::move(parsed);
    return true;
}

} // namespace

const char *to_wire(NozzleVolumeType type)
{
    return type == nvtHighFlow ? FLOW_MODE_HIGH_FLOW : FLOW_MODE_STANDARD;
}

std::optional<NozzleVolumeType> to_nozzle_volume_type(const std::string &wire)
{
    if (wire == FLOW_MODE_STANDARD)
        return nvtStandard;
    if (wire == FLOW_MODE_HIGH_FLOW)
        return nvtHighFlow;
    return std::nullopt;
}

std::optional<std::string> normalize_flow_type(const std::string &text)
{
    std::string word;
    for (const char c : text)
        if (std::isalnum(static_cast<unsigned char>(c)))
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (word == "standard")
        return std::string(FLOW_MODE_STANDARD);
    if (word == "highflow")
        return std::string(FLOW_MODE_HIGH_FLOW);
    return std::nullopt;
}

bool is_unstated_flow_type(const nlohmann::json &value)
{
    if (value.is_null())
        return true;
    if (!value.is_string())
        return false;
    const std::string text = value.get<std::string>();
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

bool append_nozzle_diameter(const nlohmann::json &value, std::vector<std::string> &out)
{
    struct Variant { double diameter; const char *text; };
    static const Variant variants[] = { { 0.2, "0.2" }, { 0.4, "0.4" }, { 0.6, "0.6" }, { 0.8, "0.8" } };
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        for (const Variant &variant : variants)
            if (text == variant.text) {
                out.emplace_back(text);
                return true;
            }
        return false;
    }

    if (!value.is_number())
        return false;

    const double diameter = value.get<double>();
    for (const Variant &variant : variants)
        if (std::abs(diameter - variant.diameter) < 1e-6) {
            out.emplace_back(variant.text);
            return true;
        }
    return false;
}

void parse_nozzle_diameters(const nlohmann::json &value, std::vector<std::string> &out)
{
    if (value.is_array()) {
        for (const nlohmann::json &entry : value)
            append_nozzle_diameter(entry, out);
    } else {
        append_nozzle_diameter(value, out);
    }
}

std::vector<std::string> build_filament_volume_types(const std::vector<int> *values, size_t count)
{
    std::vector<std::string> result(count, FLOW_MODE_STANDARD);
    if (values == nullptr) return result;
    for (size_t i = 0; i < std::min(count, values->size()); ++i)
        if ((*values)[i] == nvtHighFlow) result[i] = FLOW_MODE_HIGH_FLOW;
    return result;
}

std::vector<std::string> build_nozzle_volume_types(const std::vector<int> &values, size_t head_count)
{
    return build_filament_volume_types(&values, head_count);
}

std::vector<int> filament_volume_types_by_head(const std::vector<int> &nozzle_volume_types,
                                               const std::vector<size_t> &filament_head, size_t filament_count)
{
    std::vector<int> result(filament_count, int(nvtStandard));
    for (size_t filament = 0; filament < std::min(filament_count, filament_head.size()); ++filament)
        if (const size_t head = filament_head[filament]; head < nozzle_volume_types.size())
            result[filament] = nozzle_volume_types[head];
    return result;
}

size_t sync_reference_head(const std::vector<std::string> &flows, size_t head_count,
                           const std::function<bool(const std::string &diameter)> &size_offers,
                           const std::vector<std::string> &diameters)
{
    const size_t count = std::min(flows.size(), head_count);
    if (!size_offers) {
        for (size_t head = 0; head < count; ++head)
            if (flows[head] == FLOW_MODE_HIGH_FLOW)
                return head;
        return 0;
    }
    auto offers = [&size_offers, &diameters](size_t head) { return head < diameters.size() && size_offers(diameters[head]); };
    // The profile follows tool head 1 unless its nozzle size offers no High Flow while a High Flow
    // tool head's size does.
    if (head_count > 0 && offers(0))
        return 0;
    for (size_t head = 0; head < count; ++head)
        if (flows[head] == FLOW_MODE_HIGH_FLOW && offers(head))
            return head;
    return 0;
}

OptionalFlowTypesStatus parse_optional_flow_types(const nlohmann::json &object, const char *key, size_t count,
                                                   std::vector<std::string> &out)
{
    out.clear();
    const auto it = object.find(key);
    if (it == object.end()) return OptionalFlowTypesStatus::Missing;
    if (!it->is_array() || it->size() < count) return OptionalFlowTypesStatus::Invalid;

    std::vector<std::string> parsed;
    parsed.reserve(count);
    bool any_stated = false;
    for (size_t i = 0; i < count; ++i)
        if (!append_flow_type((*it)[i], parsed, any_stated)) return OptionalFlowTypesStatus::Invalid;
    // Nothing but unstated entries is no flow data.
    if (!any_stated) return OptionalFlowTypesStatus::Missing;
    out = std::move(parsed);
    return OptionalFlowTypesStatus::Valid;
}

bool parse_machine_info_response(const nlohmann::json &response, MachineInfo &out)
{
    const nlohmann::json *root = &response;
    if (const auto data = response.find("data"); data != response.end() && data->is_object())
        root = &*data;

    const auto system_info = root->find("system_info");
    if (system_info == root->end() || !system_info->is_object()) return false;
    const auto product_info = system_info->find("product_info");
    if (product_info == system_info->end() || !product_info->is_object()) return false;

    MachineInfo parsed;
    if (const auto model = product_info->find("machine_type"); model != product_info->end() && model->is_string())
        parsed.model = model->get<std::string>();
    if (const auto name = product_info->find("device_name"); name != product_info->end() && name->is_string())
        parsed.device_name = name->get<std::string>();

    if (const auto diameters = product_info->find("nozzle_diameter"); diameters != product_info->end())
        parse_nozzle_diameters(*diameters, parsed.nozzle_diameters);

    auto flows = product_info->find("nozzle_volume_type");
    if (flows == product_info->end())
        flows = product_info->find("nozzle_volume_types");
    if (flows != product_info->end() && !parse_system_flow_types(*flows, parsed.nozzle_volume_types))
        parsed.nozzle_volume_types.clear();

    out = std::move(parsed);
    return true;
}

bool select_complete_cached_nozzle_info(const std::vector<std::pair<std::string, std::string>> &slots,
                                         std::vector<std::string> &diameters, std::vector<std::string> &flows)
{
    if (slots.empty()) return false;

    std::vector<std::string> cached_diameters;
    std::vector<std::string> cached_flows;
    cached_diameters.reserve(slots.size());
    cached_flows.reserve(slots.size());
    bool flows_complete = true;
    for (const auto &[diameter, flow] : slots) {
        if (diameter.empty()) return false;
        cached_diameters.push_back(diameter);
        if (!is_valid_flow_type(flow))
            flows_complete = false;
        else
            cached_flows.push_back(flow);
    }

    diameters = std::move(cached_diameters);
    if (flows_complete)
        flows = std::move(cached_flows);
    // else: leave the caller's flows untouched. An incomplete cache must not wipe
    // values we already have (e.g. freshly resolved from objects.query); only a
    // complete, valid cache overrides them.
    return true;
}

std::string normalize_machine_model(const std::string &model)
{
    // CRITICAL: empty string must stay empty. Never default to a machine type,
    // because callers use the result for whitelist gating and persistent storage.
    if (model.empty())
        return "";

    // Known firmware aliases that map to Snapmaker U1.
    if (model == "lava" || model == "Snapmaker test")
        return "Snapmaker U1";

    return model;
}

bool parse_extruder_nozzle_info(const nlohmann::json &response,
                                 std::vector<std::string> &diameters,
                                 std::vector<std::string> &volume_types)
{
    diameters.clear();
    volume_types.clear();

    // Navigate to data.status
    const nlohmann::json *root = &response;
    if (const auto data = response.find("data"); data != response.end() && data->is_object())
        root = &*data;
    const auto status = root->find("status");
    if (status == root->end() || !status->is_object())
        return false;

    // Dynamically discover extruder objects: "extruder", "extruder1", "extruder2", ...
    // Moonraker names the first toolhead "extruder" and subsequent ones with a numeric suffix.
    // Collect matching keys with their ordinal index for stable ordering.
    struct ExtruderEntry { size_t index; std::string key; };
    std::vector<ExtruderEntry> extruders;
    for (auto it = status->begin(); it != status->end(); ++it) {
        const std::string &key = it.key();
        size_t index;
        if (key == "extruder") {
            index = 0;
        } else if (key.size() > 8 && key.compare(0, 8, "extruder") == 0 &&
                   std::all_of(key.begin() + 8, key.end(), [](unsigned char c) { return std::isdigit(c); })) {
            // Cap suffix length to avoid stoul overflow on pathological firmware keys.
            if (key.size() - 8 > 4) continue;
            index = std::stoul(key.substr(8));
        } else {
            continue;
        }
        if (it->is_object())
            extruders.push_back({index, key});
    }
    // Sort by toolhead index: extruder(0), extruder1(1), extruder2(2), ...
    std::sort(extruders.begin(), extruders.end(),
              [](const ExtruderEntry &a, const ExtruderEntry &b) { return a.index < b.index; });

    bool any_flow_stated = false;
    for (const auto &entry : extruders) {
        const auto extr = status->find(entry.key);
        const auto nd = extr->find("nozzle_diameter");
        if (nd == extr->end())
            continue;

        if (!append_nozzle_diameter(*nd, diameters))
            continue;

        // A tool head without a flow type, or with one this tree does not know, is a Standard one.
        const auto                 vt   = extr->find("nozzle_volume_type");
        std::optional<std::string> flow;
        if (vt != extr->end() && vt->is_string() && !is_unstated_flow_type(*vt)) {
            flow            = normalize_flow_type(vt->get<std::string>());
            any_flow_stated = true;
        }
        volume_types.push_back(flow ? *flow : std::string(FLOW_MODE_STANDARD));
    }

    // No tool head states a flow type: the firmware does not report them.
    if (!any_flow_stated)
        volume_types.clear();

    return !diameters.empty();
}

ResolveResult merge_machine_info(const nlohmann::json &system_info, const nlohmann::json &objects_query)
{
    ResolveResult result;
    MachineInfo  &mi = result.info;

    // model / device_name: system_info is authoritative, normalized for whitelist safety.
    MachineInfo sys_mi;
    if (!system_info.is_null() && parse_machine_info_response(system_info, sys_mi)) {
        mi.model       = normalize_machine_model(sys_mi.model);
        mi.device_name = sys_mi.device_name;
        // system_info nozzle data as fallback
        mi.nozzle_diameters    = sys_mi.nozzle_diameters;
        mi.nozzle_volume_types = sys_mi.nozzle_volume_types;
    }
    // nozzle: objects.query is preferred (real-time), overrides system_info.
    std::vector<std::string> obj_diameters, obj_flows;
    if (!objects_query.is_null() && parse_extruder_nozzle_info(objects_query, obj_diameters, obj_flows)) {
        mi.nozzle_diameters = std::move(obj_diameters);
        // Flow types are replaced only when objects.query states them; otherwise those of
        // system_info stay.
        if (!obj_flows.empty())
            mi.nozzle_volume_types = std::move(obj_flows);
    }

    if (mi.model.empty())
        result.status = ResolveStatus::NoResponse;
    else if (mi.nozzle_diameters.empty())
        result.status = ResolveStatus::GotIdentity;
    else
        result.status = ResolveStatus::Complete;
    return result;
}

FinishFilamentMappingEvent parse_finish_filament_mapping_event(const nlohmann::json &params)
{
    if (!params.is_object())
        return FinishFilamentMappingEvent::None;
    const auto event = params.find("event");
    if (event == params.end())
        return FinishFilamentMappingEvent::None;

    long long number = 0;
    if (event->is_number_integer()) {
        number = event->get<long long>();
    } else if (event->is_string()) {
        const std::string text = event->get<std::string>();
        if (text.empty() || text.size() > 9 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return FinishFilamentMappingEvent::None;
        number = std::stoll(text);
    } else {
        return FinishFilamentMappingEvent::None;
    }
    return number == static_cast<long long>(FinishFilamentMappingEvent::CustomFlowRegroup) ? FinishFilamentMappingEvent::CustomFlowRegroup :
                                                                                           FinishFilamentMappingEvent::None;
}

}} // namespace Slic3r::SSWCPProtocol
