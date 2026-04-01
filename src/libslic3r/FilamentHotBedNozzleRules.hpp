#ifndef slic3r_FilamentHotBedNozzleRules_hpp_
#define slic3r_FilamentHotBedNozzleRules_hpp_

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

class PresetBundle;

// JSON: resources/profiles/Snapmaker/filament/filament_hot_bed_nozzles.json
// Keys: bed ids (btPEI, btGESP), nozzle ids ("0.2mm"), each with support/warning/forbidden arrays.
class FilamentHotBedNozzleRules
{
public:
    static constexpr const char* kBedKey_PEI  = "btPEI";
    static constexpr const char* kBedKey_GESP = "btGESP";

    static FilamentHotBedNozzleRules& singleton();

    void load();
    // Loads JSON once if not already loaded (Print::extruders uses this; GUI::load forces reload via load()).
    void ensure_loaded();
    bool is_loaded() const;

    // "Supported" includes warning-tier materials (they are allowed but need a separate warning UI).
    bool is_bed_filament_tips(const std::string& bed_key, const std::string& filament_type) const;
    bool is_bed_filament_supported(const std::string& bed_key, const std::string& filament_type) const;
    bool is_bed_filament_warning(const std::string& bed_key, const std::string& filament_type) const;
    bool is_nozzle_filament_forbidden(const std::string& nozzle_key, const std::string& filament_preset_name) const;
    
    std::string evaluate_nozzle_filament_mismatch(const PrintConfig& cfg,
                                                  const std::vector<unsigned int>& used_filament_indices,
                                                  const PresetBundle* preset_bundle = nullptr) const;

    
    bool evaluate_graphic_effect_bed_filament_mismatch(const PrintConfig& cfg,
                                                       const std::vector<unsigned int>& used_filament_indices) const;

    bool evaluate_pei_bed_filament_mismatch_not_pla(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const;
    bool evaluate_pei_bed_filament_mismatch_tpu(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const;

private:
    FilamentHotBedNozzleRules() = default;

    mutable std::recursive_mutex m_mutex;
    bool m_loaded{ false };
    std::unordered_map<std::string, std::unordered_set<std::string>> m_bed_support_filament_types;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_bed_warning_filament_types;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_nozzle_forbidden_filament_presets;
};

std::string bed_type_to_filament_rule_key(BedType bed_type);
std::string nozzle_diameter_to_filament_rule_key(double nozzle_diameter_mm);

} // namespace Slic3r

#endif
