#include "BambuSendMapping.hpp"

#include "DeviceManager.hpp"
#include "SelectMachine.hpp" // CloudTaskNozzleId, FilamentMapNozzleId, get_nozzle_volume_type_cloud_string
#include "libslic3r/BambuExtruderMap.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace Slic3r {
namespace GUI {
namespace BambuSendMapping {

using nlohmann::json;

int auto_map(MachineObject* obj, const std::vector<FilamentInfo>& filaments, const std::vector<int>& filament_map,
             const std::vector<int>& physical_extruder_map, std::vector<FilamentInfo>& result)
{
    if (!obj)
        return -1;
    if (filament_map.empty())
        return obj->ams_filament_mapping(filaments, result);

    /* Two-extruder job: a filament can only be fed from an AMS connected to the extruder the
     * G-code prints it with, so map each side against its own AMS units, as BambuStudio's
     * do_ams_mapping does. Mapping every filament against every AMS let the 2026-09-23 H2C job
     * (sliced all on the right rack) send filament 10 from the left extruder's AMS HT; the
     * printer then re-arranged the filaments itself. */
    int                       rc = 0;
    std::vector<FilamentInfo> per_side[3]; // left, right, unknown
    for (const FilamentInfo& f : filaments) {
        const int logical = (f.id >= 0 && f.id < (int) filament_map.size()) ? filament_map[f.id] - 1 : -1;
        per_side[(logical == 0 || logical == 1) ? logical : 2].push_back(f);
    }
    result.clear();
    for (int side = 0; side < 3; ++side) {
        if (per_side[side].empty())
            continue;
        std::vector<FilamentInfo> side_result;
        const int only_physical = side < 2 ? BambuExtruderMap::logical_to_physical(physical_extruder_map, side) : -1;
        const int side_rc       = obj->ams_filament_mapping(per_side[side], side_result, std::vector<int>(), only_physical);
        if (side_rc != 0)
            rc = side_rc;
        result.insert(result.end(), side_result.begin(), side_result.end());
    }
    std::sort(result.begin(), result.end(), [](const FilamentInfo& a, const FilamentInfo& b) { return a.id < b.id; });
    return rc;
}

std::vector<int> wrong_extruder(MachineObject* obj, const std::vector<FilamentInfo>& result, const std::vector<int>& filament_map,
                                const std::vector<int>& physical_extruder_map)
{
    if (!obj || filament_map.empty())
        return {};
    std::vector<BambuExtruderMap::MappedTray> mapped;
    for (const FilamentInfo& f : result) {
        if (f.tray_id < 0 || f.ams_id.empty())
            continue;
        auto ams_it = obj->amsList.find(f.ams_id);
        if (ams_it == obj->amsList.end() || !ams_it->second)
            continue; // external spool or unknown unit: no AMS binding to check
        mapped.push_back({ f.id, ams_it->second->nozzle });
    }
    return BambuExtruderMap::filaments_on_wrong_extruder(filament_map, physical_extruder_map, mapped);
}

/* project_config "filament_map" numbers the nozzles 1 = left, 2 = right; the print task
 * numbers them 1 = left, 0 = right. Ported from BambuStudio SelectMachine.cpp. */
static int task_nozzle_id(int nozzle_id)
{
    if (nozzle_id == (int) FilamentMapNozzleId::NOZZLE_LEFT)
        return (int) CloudTaskNozzleId::NOZZLE_LEFT;
    if (nozzle_id == (int) FilamentMapNozzleId::NOZZLE_RIGHT)
        return (int) CloudTaskNozzleId::NOZZLE_RIGHT;
    /* unsupported nozzle id - pass it through rather than asserting in a send path */
    BOOST_LOG_TRIVIAL(error) << "convert_filament_map_nozzle_id, unexpected nozzle id " << nozzle_id;
    return nozzle_id;
}

bool compose(const std::vector<FilamentInfo>& result, const std::vector<FilamentInfo>& filaments, const ComposeInput& in,
             std::string& v0, std::string& v1, std::string& info)
{
    if (result.empty())
        return false;
    bool   valid   = true;
    size_t invalid = 0;
    for (const FilamentInfo& r : result)
        if (r.tray_id == -1) {
            valid = false;
            ++invalid;
        }
    if (invalid == result.size())
        return false;

    json j0 = json::array(), j1 = json::array(), ji = json::array();
    for (size_t i = 0; i < in.project_filament_count; ++i) {
        int  tray_id = -1;
        json item1;
        item1["ams_id"]  = 0xff;
        item1["slot_id"] = 0xff;
        json item;
        item["ams"]          = tray_id;
        item["targetColor"]  = "";
        item["filamentId"]   = "";
        item["filamentType"] = "";
        for (size_t k = 0; k < result.size(); ++k) {
            if (result[k].id != (int) i)
                continue;
            tray_id              = result[k].tray_id;
            item["ams"]          = tray_id;
            item["filamentType"] = k < filaments.size() ? filaments[k].type : result[k].type;
            if (i < in.filament_ids.size())
                item["filamentId"] = in.filament_ids[i];
            /* nozzle id */
            if (i < in.nozzle_filament_map.size())
                item["nozzleId"] = task_nozzle_id(in.nozzle_filament_map[i]);
            item["sourceColor"] = k < filaments.size() ? filaments[k].color : result[k].color;
            item["targetColor"] = result[k].color;
            try {
                if (result[k].ams_id.empty() || result[k].slot_id.empty()) { // invalid case
                    item1["ams_id"]  = 255;
                    item1["slot_id"] = 255;
                } else {
                    item1["ams_id"]  = std::stoi(result[k].ams_id);
                    item1["slot_id"] = std::stoi(result[k].slot_id);
                }
            } catch (...) {}
        }
        j0.push_back(tray_id);
        j1.push_back(item1);
        ji.push_back(item);
    }
    v0   = j0.dump();
    v1   = j1.dump();
    info = ji.dump();
    return valid;
}

std::string nozzles_info(const std::vector<double>& diameters, const std::vector<int>& volume_types)
{
    json arr = json::array();
    /* only the two-nozzle machines carry nozzles info */
    if (diameters.size() != 2)
        return arr.dump();
    for (size_t i = 0; i < diameters.size(); ++i) {
        json n;
        n["id"]       = (int) (i == (size_t) ConfigNozzleIdx::NOZZLE_LEFT ? CloudTaskNozzleId::NOZZLE_LEFT : CloudTaskNozzleId::NOZZLE_RIGHT);
        n["type"]     = nullptr;
        n["flowSize"] = i < volume_types.size() ? get_nozzle_volume_type_cloud_string(volume_types[i]) : std::string("standard_flow");
        n["diameter"] = diameters[i];
        arr.push_back(n);
    }
    return arr.dump();
}

} // namespace BambuSendMapping
} // namespace GUI
} // namespace Slic3r
