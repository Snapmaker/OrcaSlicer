#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace GUI {

struct OfficialFilamentColor
{
    std::string name;
    std::string sku;
    int         mode { 0 };
    std::string primary_color;
    std::vector<std::string> colors;
};

struct OfficialFilamentMaterial
{
    std::string material_key;
    std::string vendor;
    std::string filament_type;
    std::string filament_sub_type;
    std::string file;
    std::vector<OfficialFilamentColor> colors;
};

class OfficialFilamentColorLibrary
{
public:
    static OfficialFilamentColorLibrary& instance();

    bool ensure_loaded();
    void reload();

    bool find_color_by_sku(const std::string& sku, OfficialFilamentMaterial& out_material, OfficialFilamentColor& out_color);
    bool find_material_by_key(const std::string& material_key, OfficialFilamentMaterial& out_material);
    std::vector<OfficialFilamentColor> get_colors_for_material(const std::string& material_key);

private:
    bool load_index();
    bool load_material_file(const std::string& file);
    void clear();

private:
    bool m_loaded { false };
    bool m_load_failed { false };

    std::unordered_map<std::string, OfficialFilamentMaterial> m_materials_by_key;
    std::unordered_map<std::string, std::string>              m_material_key_by_sku;
    std::unordered_map<std::string, OfficialFilamentColor>    m_colors_by_sku;
};

}} // namespace Slic3r::GUI
