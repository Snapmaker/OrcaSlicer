#ifndef slic3r_FilamentHotBedNozzleRules_hpp_
#define slic3r_FilamentHotBedNozzleRules_hpp_

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PrintConfig.hpp"

namespace Slic3r {

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

    // ① 喷嘴规格 vs 耗材：仅 JSON 中喷嘴直径键（如 "0.2mm"）的 "forbidden" 预设名；与当前热床类型无关。
    std::string evaluate_nozzle_filament_mismatch(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const;

    // ② 特效热床（Graphic Effect / btGESP）：仅当 curr_bed_type==btGESP 时校验；使用 JSON 键 "btGESP" 的 support/warning。
    bool evaluate_graphic_effect_bed_filament_mismatch(const PrintConfig& cfg,
                                                       const std::vector<unsigned int>& used_filament_indices) const;

    // ③ 光面 PEI 热床（btPEI）：仅当 curr_bed_type==btPEI 时校验；使用 JSON 键 "btPEI"。
    //    非 PLA 材料由 support 列表约束；TPU 等由 warning 列表单独提示（warning 仍算“可用但需提示”）。
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
