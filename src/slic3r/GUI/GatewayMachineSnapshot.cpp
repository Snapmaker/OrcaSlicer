#include "GatewayMachineSnapshot.hpp"

#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>

#include <limits>
#include <utility>
#include <vector>

namespace {

bool parse_json_int64(const nlohmann::json& value, std::int64_t& result)
{
    if (value.is_number_unsigned()) {
        const std::uint64_t unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return false;
        result = static_cast<std::int64_t>(unsigned_value);
        return true;
    }
    if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
        return true;
    }
    return false;
}

bool parse_json_nonnegative_int(const nlohmann::json& value, std::int64_t& result)
{ return parse_json_int64(value, result) && result >= 0 && result <= static_cast<std::int64_t>(std::numeric_limits<int>::max()); }

std::string machine_filament_display_name(const nlohmann::json& filament)
{
    const auto name = filament.find("name");
    if (name != filament.end() && name->is_string() && !name->get<std::string>().empty())
        return name->get<std::string>();

    const std::string vendor   = filament.at("vendor").get<std::string>();
    const std::string type     = filament.at("type").get<std::string>();
    const std::string sub_type = filament.at("sub_type").get<std::string>();
    if (sub_type == "Support")
        return vendor + " Support For " + type;

    return vendor + " " + type + ((sub_type != "NONE" && !sub_type.empty()) ? " " + sub_type : "");
}

bool machine_infos_equal(const std::vector<ConnectMachineInfo>& machine_infos, const std::vector<ConnectMachineInfo>& next_machine_infos)
{
    if (machine_infos.size() != next_machine_infos.size())
        return false;

    for (size_t index = 0; index < machine_infos.size(); ++index) {
        const auto& machine_info      = machine_infos[index];
        const auto& next_machine_info = next_machine_infos[index];
        if (machine_info.index != next_machine_info.index || machine_info.filament_info != next_machine_info.filament_info ||
            machine_info.filament_type != next_machine_info.filament_type || machine_info.nozzle_info != next_machine_info.nozzle_info ||
            machine_info.color_info != next_machine_info.color_info || machine_info.multiColors != next_machine_info.multiColors ||
            machine_info.colorMode != next_machine_info.colorMode)
            return false;
    }
    return true;
}

} // namespace

namespace Slic3r { namespace GUI {

void GatewayMachineSnapshot::set_dependencies(PresetBundleAccessor preset_bundle_accessor, NativeUiRefresh refresh_native_ui)
{
    preset_bundle_accessor_ = std::move(preset_bundle_accessor);
    refresh_native_ui_      = std::move(refresh_native_ui);
}

void GatewayMachineSnapshot::reset()
{
    pending_snapshot_.reset();
    serial_number_.clear();
    revisions_.clear();
    initialized_             = false;
    has_active_device_       = false;
    active_device_connected_ = false;
    active_serial_number_.clear();
}

void GatewayMachineSnapshot::set_active_device(const std::string& serial_number, bool connected)
{
    if (serial_number.empty()) {
        clear();
        return;
    }

    auto pending_snapshot = std::move(pending_snapshot_);
    pending_snapshot_.reset();
    if (!has_active_device_ || active_serial_number_ != serial_number)
        clear();

    has_active_device_       = true;
    active_serial_number_    = serial_number;
    active_device_connected_ = connected;
    if (!connected)
        clear(serial_number);
    else if (pending_snapshot.has_value())
        apply(*pending_snapshot);
}

void GatewayMachineSnapshot::apply(const nlohmann::json& snapshot)
{
    if (!snapshot.is_object() || !snapshot.contains("revision") || !snapshot.contains("sn") || !snapshot.contains("nozzle_diameters") ||
        !snapshot.contains("filaments")) {
        BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot: required fields are missing";
        return;
    }

    std::int64_t revision = -1;
    if (!parse_json_int64(snapshot.at("revision"), revision) || revision < 0) {
        BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot revision";
        return;
    }

    const auto sn_value = snapshot.find("sn");
    if (!sn_value->is_string() || sn_value->get<std::string>().empty()) {
        BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot serial number";
        return;
    }
    const std::string serial_number = sn_value->get<std::string>();
    if (!has_active_device_) {
        pending_snapshot_ = snapshot;
        BOOST_LOG_TRIVIAL(info) << "deferred machine snapshot for " << serial_number << " until the active device is known";
        return;
    }
    if (!active_device_connected_ || serial_number != active_serial_number_) {
        BOOST_LOG_TRIVIAL(warning) << "ignored machine snapshot for inactive device " << serial_number;
        return;
    }

    const auto         previous_revision = revisions_.find(serial_number);
    const std::int64_t last_revision     = previous_revision == revisions_.end() ? -1 : previous_revision->second;
    if (revision <= last_revision) {
        BOOST_LOG_TRIVIAL(info) << "ignored stale machine snapshot " << serial_number << " revision " << revision;
        return;
    }

    const auto& nozzle_diameters = snapshot.at("nozzle_diameters");
    const auto& filaments_value  = snapshot.at("filaments");
    if (!nozzle_diameters.is_array() || !filaments_value.is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot arrays for " << serial_number;
        return;
    }

    std::map<int, std::pair<std::string, std::string>> next_filaments;
    std::vector<ConnectMachineInfo>                    next_machine_info;

    for (const auto& filament : filaments_value) {
        if (!filament.is_object() || !filament.contains("index") || !filament.contains("extruder") || !filament.contains("official") ||
            !filament.contains("vendor") || !filament.contains("type") || !filament.contains("sub_type") || !filament.contains("color")) {
            BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot filament for " << serial_number;
            return;
        }

        std::int64_t index_value          = -1;
        std::int64_t extruder_value       = -1;
        const bool   valid_filament_index = parse_json_nonnegative_int(filament.at("index"), index_value);
        const bool   valid_extruder       = parse_json_nonnegative_int(filament.at("extruder"), extruder_value);
        if (!valid_filament_index || !valid_extruder || !filament.at("official").is_boolean() || !filament.at("vendor").is_string() ||
            !filament.at("type").is_string() || !filament.at("sub_type").is_string() || !filament.at("color").is_string()) {
            BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot filament fields for " << serial_number;
            return;
        }

        const int index = static_cast<int>(index_value);
        if (next_filaments.count(index) != 0) {
            BOOST_LOG_TRIVIAL(warning) << "ignored machine snapshot with duplicate filament index " << index;
            return;
        }

        const auto& nozzle_value = filament.find("nozzle");
        std::string nozzle;
        if (nozzle_value != filament.end()) {
            if (!nozzle_value->is_string()) {
                BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot nozzle for " << serial_number;
                return;
            }
            nozzle = nozzle_value->get<std::string>();
        } else {
            if (index_value >= static_cast<std::int64_t>(nozzle_diameters.size()) || !nozzle_diameters[index_value].is_string()) {
                BOOST_LOG_TRIVIAL(warning) << "ignored machine snapshot with missing nozzle " << index;
                return;
            }
            nozzle = nozzle_diameters[index_value].get<std::string>();
        }

        ConnectMachineInfo machine_info;
        machine_info.index         = index;
        machine_info.filament_info = machine_filament_display_name(filament);
        machine_info.filament_type = filament.at("type").get<std::string>();
        machine_info.nozzle_info   = nozzle;
        machine_info.color_info    = NormalizeFilamentHexColor(filament.at("color").get<std::string>(), "#FFFFFF");

        const auto multi_colors = filament.find("multi_colors");
        if (multi_colors != filament.end()) {
            if (!multi_colors->is_array()) {
                BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot colors for " << serial_number;
                return;
            }
            for (const auto& color : *multi_colors) {
                if (!color.is_string())
                    continue;
                const std::string normalized_color = NormalizeFilamentHexColor(color.get<std::string>());
                if (!normalized_color.empty())
                    machine_info.multiColors.emplace_back(normalized_color);
            }
        }
        if (machine_info.multiColors.empty() && !machine_info.color_info.empty())
            machine_info.multiColors.emplace_back(machine_info.color_info);

        const auto color_mode = filament.find("color_mode");
        if (color_mode != filament.end()) {
            std::int64_t mode_value = 0;
            if (!parse_json_int64(*color_mode, mode_value) || mode_value < 0 || mode_value > std::numeric_limits<int>::max()) {
                BOOST_LOG_TRIVIAL(warning) << "ignored invalid machine snapshot color mode for " << serial_number;
                return;
            }
            machine_info.colorMode = FilamentColorModeFromConfig(static_cast<int>(mode_value));
        }
        if (machine_info.multiColors.size() <= 1)
            machine_info.colorMode = FilamentColorMode::Segment;

        next_filaments.emplace(index, std::make_pair(machine_info.filament_info, machine_info.color_info));
        next_machine_info.emplace_back(std::move(machine_info));
    }

    PresetBundle* preset_bundle = preset_bundle_accessor_ ? preset_bundle_accessor_() : nullptr;
    if (preset_bundle == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "cannot apply machine snapshot before preset bundle is ready";
        return;
    }

    const auto& previous_filaments    = preset_bundle->machine_filaments;
    const auto& previous_machine_info = preset_bundle->m_connect_machine_info_list;
    const bool  changed = previous_filaments != next_filaments || !machine_infos_equal(previous_machine_info, next_machine_info);

    preset_bundle->machine_filaments           = std::move(next_filaments);
    preset_bundle->m_connect_machine_info_list = std::move(next_machine_info);
    serial_number_                             = serial_number;
    revisions_[serial_number]                  = revision;
    initialized_                               = true;

    if (changed && refresh_native_ui_)
        refresh_native_ui_();

    BOOST_LOG_TRIVIAL(info) << "applied machine snapshot " << serial_number << " revision " << revision << " filaments "
                            << preset_bundle->machine_filaments.size();
}

void GatewayMachineSnapshot::clear(const std::string& serial_number)
{
    pending_snapshot_.reset();
    if (!serial_number.empty() && serial_number != serial_number_) {
        revisions_.erase(serial_number);
        return;
    }

    const bool clear_all     = serial_number.empty();
    const bool had_snapshot  = initialized_;
    bool       had_filaments = false;
    if (PresetBundle* preset_bundle = preset_bundle_accessor_ ? preset_bundle_accessor_() : nullptr) {
        had_filaments = !preset_bundle->machine_filaments.empty();
        preset_bundle->machine_filaments.clear();
        preset_bundle->m_connect_machine_info_list.clear();
    }
    serial_number_.clear();
    initialized_ = false;
    if (clear_all)
        revisions_.clear();
    else
        revisions_.erase(serial_number);
    if (clear_all) {
        has_active_device_       = false;
        active_device_connected_ = false;
        active_serial_number_.clear();
    }

    if (had_filaments && refresh_native_ui_)
        refresh_native_ui_();

    if (had_snapshot)
        BOOST_LOG_TRIVIAL(info) << "cleared machine snapshot " << (serial_number.empty() ? std::string{"all"} : serial_number);
}

}} // namespace Slic3r::GUI
