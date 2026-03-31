#include "FilamentHotBedNozzleRules.hpp"
#include "Config.hpp"
#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <sstream>
#include <iomanip>

namespace Slic3r {

namespace pt = boost::property_tree;
namespace {
std::string to_upper_ascii(std::string s)
{
    for (char& c : s) {
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    }
    return s;
}

bool contains_token_ci(const std::string& text, const std::string& token)
{
    if (token.empty())
        return false;
    const std::string up_text  = to_upper_ascii(text);
    const std::string up_token = to_upper_ascii(token);
    return up_text.find(up_token) != std::string::npos;
}

bool match_any_rule_token_ci(const std::unordered_set<std::string>& tokens, const std::string& value)
{
    for (const std::string& token : tokens) {
        if (contains_token_ci(value, token))
            return true;
    }
    return false;
}

bool is_pla_type(const std::string& filament_type)
{
    return contains_token_ci(filament_type, "PLA");
}

bool is_tpu_type(const std::string& filament_type)
{
    return contains_token_ci(filament_type, "TPU");
}
} // namespace

FilamentHotBedNozzleRules& FilamentHotBedNozzleRules::singleton()
{
    static FilamentHotBedNozzleRules inst;
    return inst;
}

std::string bed_type_to_filament_rule_key(BedType bed_type)
{
    switch (bed_type) {
    case btPEI:  return "btPEI";
    case btGESP: return "btGESP";
    default:     return "";
    }
}

std::string nozzle_diameter_to_filament_rule_key(double nozzle_diameter_mm)
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << nozzle_diameter_mm;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out + "mm";
}

void FilamentHotBedNozzleRules::ensure_loaded()
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (m_loaded)
        return;
    // std::scoped_lock has no unlock(); load() takes the same recursive_mutex (nested lock is OK).
    load();
}

void FilamentHotBedNozzleRules::load()
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);

    m_bed_support_filament_types.clear();
    m_bed_warning_filament_types.clear();
    m_nozzle_forbidden_filament_presets.clear();
    m_loaded = false;

    const auto file_path =
        (boost::filesystem::path(Slic3r::resources_dir()) / "profiles" / "Snapmaker" / "filament" / "filament_hot_bed_nozzles.json").string();
    if (!boost::filesystem::exists(file_path)) {
        BOOST_LOG_TRIVIAL(warning) << "filament_hot_bed_nozzles.json not found: " << file_path;
        return;
    }

    try {
        pt::ptree root;
        pt::read_json(file_path, root);

        for (const auto& kv : root) {
            const std::string& rule_key = kv.first;
            const auto&        rule_obj = kv.second;
            auto& support_set = m_bed_support_filament_types[rule_key];
            auto& warning_set = m_bed_warning_filament_types[rule_key];
            auto& forbid_set  = m_nozzle_forbidden_filament_presets[rule_key];

            for (const auto& child : rule_obj) {
                if (child.first == "support") {
                    for (const auto& item : child.second)
                        support_set.insert(item.second.get_value<std::string>());
                } else if (child.first == "warning") {
                    for (const auto& item : child.second)
                        warning_set.insert(item.second.get_value<std::string>());
                } else if (child.first == "forbidden") {
                    for (const auto& item : child.second)
                        forbid_set.insert(item.second.get_value<std::string>());
                }
            }
        }
        m_loaded = true;
        BOOST_LOG_TRIVIAL(info) << "Loaded filament/hotbed/nozzle rules from " << file_path;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to parse filament_hot_bed_nozzles.json: " << e.what();
    }
}

bool FilamentHotBedNozzleRules::is_loaded() const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    return m_loaded;
}
bool FilamentHotBedNozzleRules::is_bed_filament_tips(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_warning_filament_types.find(bed_key);
    if (it == m_bed_warning_filament_types.end())
        return false;

    bool res = match_any_rule_token_ci(it->second, filament_type);

    return res;
}


bool FilamentHotBedNozzleRules::is_bed_filament_supported(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_support_filament_types.find(bed_key);
    if (it == m_bed_support_filament_types.end())
        return true;
    if (it->second.empty())
        return true;
    if (match_any_rule_token_ci(it->second, filament_type))
        return true;
    // Warning-tier materials are still "supported" (separate API reports warning).
    auto itw = m_bed_warning_filament_types.find(bed_key);
    bool res = false;
    res = itw != m_bed_warning_filament_types.end() && match_any_rule_token_ci(itw->second, filament_type);
    return res;
}

bool FilamentHotBedNozzleRules::is_bed_filament_warning(const std::string& bed_key, const std::string& filament_type) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_bed_warning_filament_types.find(bed_key);
    bool res = false;
    res =  (it != m_bed_warning_filament_types.end() && match_any_rule_token_ci(it->second, filament_type));
    return res;
}

bool FilamentHotBedNozzleRules::is_nozzle_filament_forbidden(const std::string& nozzle_key, const std::string& filament_preset_name) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    auto it = m_nozzle_forbidden_filament_presets.find(nozzle_key);
    bool res = false;
    res = (it != m_nozzle_forbidden_filament_presets.end() && it->second.find(filament_preset_name) != it->second.end());
    return res;
}

std::string FilamentHotBedNozzleRules::evaluate_nozzle_filament_mismatch(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return "";

    std::string nozzle_key;
    if (!cfg.nozzle_diameter.empty())
        nozzle_key = nozzle_diameter_to_filament_rule_key(cfg.nozzle_diameter.get_at(0));
    if (nozzle_key.empty())
        return "";

    const ConfigOptionStrings* filament_settings_id = cfg.option<ConfigOptionStrings>("filament_settings_id");
    for (unsigned int fid : used_filament_indices) {
        std::string preset_name;
        if (filament_settings_id != nullptr && !filament_settings_id->values.empty())
            preset_name = filament_settings_id->get_at(fid);
        if (is_nozzle_filament_forbidden(nozzle_key, preset_name))
            return preset_name.empty() ? "forbidden filament preset for current nozzle" : preset_name;
    }

    return "";
}

bool FilamentHotBedNozzleRules::evaluate_graphic_effect_bed_filament_mismatch(const PrintConfig& cfg, const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btGESP)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_GESP;
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;
        if (!is_bed_filament_supported(bed_key, ftype))
            return true;
        if (is_bed_filament_warning(bed_key, ftype))
            return true;
    }
    return false;
}
bool FilamentHotBedNozzleRules::evaluate_pei_bed_filament_mismatch_not_pla(const PrintConfig&               cfg,
                                                                   const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btPEI)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_PEI;
    bool res = false;//default not pla and need to show tips
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;
        // not_pla means every non-PLA type should be checked, including TPU.
        bool checkRes = is_pla_type(ftype);
        if (!checkRes)
        {
            res = !checkRes;
            break;
        }
    }
    return res;
}

bool FilamentHotBedNozzleRules::evaluate_pei_bed_filament_mismatch_tpu(const PrintConfig&               cfg,
                                                                       const std::vector<unsigned int>& used_filament_indices) const
{
    std::scoped_lock<std::recursive_mutex> lock(m_mutex);
    if (!m_loaded || used_filament_indices.empty())
        return false;
    if (static_cast<BedType>(cfg.curr_bed_type) != btPEI)
        return false;
    if (cfg.filament_type.empty())
        return false;

    const std::string bed_key = kBedKey_PEI;
    for (unsigned int fid : used_filament_indices) {
        const std::string ftype = cfg.filament_type.get_at(fid);
        if (ftype.empty())
            continue;

        if (!is_tpu_type(ftype))
            continue;
        // TPU on PEI: dedicated warning channel.
        if (is_bed_filament_tips(bed_key, ftype))
            return true;
    }
    return false;
}

} // namespace Slic3r
