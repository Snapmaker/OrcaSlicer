#ifndef slic3r_MaterialPurgeTable_hpp_
#define slic3r_MaterialPurgeTable_hpp_

#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include "nlohmann/json.hpp"
#include "FlushVolPredictor.hpp"   // FlushDataset enum

// Normalize filament_type to a canonical material family. Composite suffixes
// (-CF/-GF/+/Silk/...) merge into the base family.
// Unknown values pass through unchanged (lookup misses -> pure-color path).
inline std::string normalize_material_family(const std::string& filament_type)
{
    static const std::unordered_map<std::string, std::string> family_map = {
        {"PLA","PLA"},{"PLA-CF","PLA"},{"PLA-GF","PLA"},{"PLA+","PLA"},
        {"PLA High Speed","PLA"},{"PLA Silk","PLA"},{"PLA Matte","PLA"},
        {"PETG","PETG"},{"PETG-CF","PETG"},{"PETG-GF","PETG"},
        {"ABS","ABS"},{"ABS-GF","ABS"},
        {"ASA","ASA"},{"ASA-CF","ASA"},{"ASA-Aero","ASA"},
        {"PA","PA"},{"PA-CF","PA"},{"PA-GF","PA"},{"PA6-CF","PA"},{"PAHT-CF","PA"},
        {"PC","PC"},{"PC-CF","PC"},
        {"TPU","TPU"},{"TPE","TPU"},
        {"PVA","PVA"},{"BVOH","PVA"},
        {"HIPS","HIPS"},{"PCTG","PCTG"},
    };
    auto it = family_map.find(filament_type);
    return it != family_map.end() ? it->second : filament_type;
}

// Directional material-family measured purge table loaded from JSON. Values are
// per-flow (standard, highflow). purge may be a number (same for both flows) or
// an object {"standard":..,"highflow":..}. Pure logic: no resources_dir / no
// logging, so it is unit-testable standalone.
class MaterialPurgeTable
{
public:
    MaterialPurgeTable() = default;
    explicit MaterialPurgeTable(const std::string& json_file)
    {
        std::ifstream in(json_file);
        if (!in.is_open())
            return;
        try {
            nlohmann::json j;
            in >> j;
            if (j.contains("pairs") && j["pairs"].is_array()) {
                for (const auto& item : j["pairs"]) {
                    std::string from_id = item.value("from", std::string());
                    std::string to_id   = item.value("to",   std::string());
                    if (from_id.empty() || to_id.empty() || !item.contains("purge"))
                        continue;
                    float std_v = 0.f, hf_v = 0.f;
                    const auto& p = item["purge"];
                    if (p.is_object()) {
                        std_v = p.value("standard", 0.f);
                        hf_v  = p.value("highflow", std_v);
                    } else {
                        std_v = p.get<float>();
                        hf_v  = std_v;
                    }
                    m_map[make_key(from_id, to_id)] = { std_v, hf_v };
                }
            }
            m_valid = true;
        } catch (const std::exception&) {
            m_map.clear();
            m_valid = false;
        }
    }

    bool lookup(const std::string& from_family, const std::string& to_family,
                int flow_dataset, float& purge) const
    {
        if (!m_valid || from_family.empty() || to_family.empty())
            return false;
        auto it = m_map.find(make_key(from_family, to_family));
        if (it == m_map.end())
            return false;
        purge = (flow_dataset == static_cast<int>(FlushDataset::HighFlow))
                    ? it->second.second : it->second.first;
        return true;
    }

    bool   valid() const { return m_valid; }
    size_t size()  const { return m_map.size(); }

private:
    static std::string make_key(const std::string& f, const std::string& t) { return f + "|" + t; }
    std::unordered_map<std::string, std::pair<float, float>> m_map;
    bool m_valid{ false };
};

// Runtime entry (defined in FlushVolPredictor.cpp): resolves the resource path,
// caches one table per path (thread-safe), and delegates to the class above.
bool query_material_purge_volume(const std::string& from_family, const std::string& to_family,
                                 int flow_dataset, float& purge);

#endif // slic3r_MaterialPurgeTable_hpp_
