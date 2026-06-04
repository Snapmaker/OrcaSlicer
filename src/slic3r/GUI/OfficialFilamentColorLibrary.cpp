#include "OfficialFilamentColorLibrary.hpp"

#include "FilamentColorUtils.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <exception>

namespace Slic3r { namespace GUI {
namespace {

boost::filesystem::path colours_root()
{
    return (boost::filesystem::path(Slic3r::resources_dir()) / "profiles" / "Snapmaker" / "colours").make_preferred();
}

std::string json_string(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int json_mode(const nlohmann::json& j)
{
    const auto it = j.find("mode");
    if (it == j.end() || !it->is_number_integer())
        return 0;
    return it->get<int>() == 1 ? 1 : 0;
}

std::vector<std::string> json_string_array(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> values;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array())
        return values;

    for (const nlohmann::json& item : *it) {
        if (item.is_string())
            values.emplace_back(item.get<std::string>());
    }
    return values;
}

std::string normalize_json_color(const std::string& color, bool& has_invalid_color)
{
    const std::string normalized = FilamentColorUtils::normalize_hex_color(color);
    if (!color.empty() && normalized.empty())
        has_invalid_color = true;
    return normalized;
}

bool load_json_file(const boost::filesystem::path& path, nlohmann::json& out)
{
    boost::nowide::ifstream ifs(path.string());
    if (!ifs.is_open()) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to open official filament color file: " << path.string();
        return false;
    }

    try {
        ifs >> out;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to parse official filament color file: " << path.string() << ", " << e.what();
        return false;
    }

    return true;
}

bool is_safe_relative_file(const std::string& file)
{
    if (file.empty())
        return false;

    const boost::filesystem::path path(file);
    if (path.is_absolute())
        return false;

    for (const boost::filesystem::path& part : path) {
        if (part.string() == "..")
            return false;
    }
    return true;
}

} // namespace

OfficialFilamentColorLibrary& OfficialFilamentColorLibrary::instance()
{
    static OfficialFilamentColorLibrary library;
    return library;
}

bool OfficialFilamentColorLibrary::ensure_loaded()
{
    if (m_loaded)
        return true;

    clear();
    if (!load_index()) {
        clear();
        return false;
    }
    m_loaded = true;
    return true;
}

void OfficialFilamentColorLibrary::reload()
{
    clear();
    m_loaded = false;
    ensure_loaded();
}

bool OfficialFilamentColorLibrary::find_color_by_sku(const std::string& sku, OfficialFilamentMaterial& out_material, OfficialFilamentColor& out_color)
{
    if (!ensure_loaded())
        return false;

    const auto color_it = m_colors_by_sku.find(sku);
    if (color_it == m_colors_by_sku.end())
        return false;

    const auto material_key_it = m_material_key_by_sku.find(sku);
    if (material_key_it == m_material_key_by_sku.end())
        return false;

    const auto material_it = m_materials_by_key.find(material_key_it->second);
    if (material_it == m_materials_by_key.end())
        return false;

    out_material = material_it->second;
    out_color    = color_it->second;
    return true;
}

bool OfficialFilamentColorLibrary::find_material_by_key(const std::string& material_key, OfficialFilamentMaterial& out_material)
{
    if (!ensure_loaded())
        return false;

    const auto material_it = m_materials_by_key.find(material_key);
    if (material_it == m_materials_by_key.end())
        return false;

    out_material = material_it->second;
    return true;
}

std::vector<OfficialFilamentColor> OfficialFilamentColorLibrary::get_colors_for_material(const std::string& material_key)
{
    OfficialFilamentMaterial material;
    if (!find_material_by_key(material_key, material))
        return {};
    return material.colors;
}

bool OfficialFilamentColorLibrary::load_index()
{
    nlohmann::json index;
    const auto index_path = colours_root() / "index.json";
    if (!load_json_file(index_path, index))
        return false;

    const auto materials_it = index.find("materials");
    if (materials_it == index.end() || !materials_it->is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "Missing official filament color materials index: " << index_path.string();
        return false;
    }

    bool loaded_any = false;
    for (const nlohmann::json& material_ref : *materials_it) {
        if (!material_ref.is_object()) {
            BOOST_LOG_TRIVIAL(warning) << "Skip invalid official filament color material index item: " << index_path.string();
            continue;
        }

        const std::string file = json_string(material_ref, "file");
        if (!is_safe_relative_file(file)) {
            BOOST_LOG_TRIVIAL(warning) << "Skip invalid official filament color file path: " << file;
            continue;
        }

        const bool loaded_material = load_material_file(file);
        loaded_any = loaded_material || loaded_any;
    }

    return loaded_any;
}

bool OfficialFilamentColorLibrary::load_material_file(const std::string& file)
{
    nlohmann::json material_json;
    if (!load_json_file(colours_root() / file, material_json))
        return false;

    OfficialFilamentMaterial material;
    material.material_key      = json_string(material_json, "material_key");
    material.vendor            = json_string(material_json, "vendor");
    material.filament_type     = json_string(material_json, "filament_type");
    material.filament_sub_type = json_string(material_json, "filament_sub_type");
    material.file              = file;

    if (material.material_key.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Skip official filament color file without material_key: " << file;
        return false;
    }

    if (m_materials_by_key.find(material.material_key) != m_materials_by_key.end()) {
        BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament color material_key: " << material.material_key;
        return false;
    }

    const auto colors_it = material_json.find("colors");
    if (colors_it == material_json.end() || !colors_it->is_array()) {
        BOOST_LOG_TRIVIAL(warning) << "Skip official filament color file without colors array: " << file;
        return false;
    }

    for (const nlohmann::json& color_json : *colors_it) {
        if (!color_json.is_object()) {
            BOOST_LOG_TRIVIAL(warning) << "Skip invalid color item in official filament color file: " << file;
            continue;
        }

        OfficialFilamentColor color;
        color.name = json_string(color_json, "name");
        color.sku = json_string(color_json, "sku");
        color.mode = json_mode(color_json);

        bool has_invalid_color = false;
        color.primary_color = normalize_json_color(json_string(color_json, "primary_color"), has_invalid_color);

        for (const std::string& raw_color : json_string_array(color_json, "colors")) {
            const std::string normalized_color = normalize_json_color(raw_color, has_invalid_color);
            if (!normalized_color.empty())
                color.colors.emplace_back(normalized_color);
        }

        if (has_invalid_color) {
            BOOST_LOG_TRIVIAL(warning) << "Skip color item with invalid color value in official filament color file: " << file;
            continue;
        }

        if (color.primary_color.empty() && !color.colors.empty())
            color.primary_color = color.colors.front();
        if (color.colors.empty() && !color.primary_color.empty())
            color.colors.emplace_back(color.primary_color);
        if (color.sku.empty() || color.primary_color.empty() || color.colors.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Skip incomplete color item in official filament color file: " << file;
            continue;
        }

        if (m_material_key_by_sku.find(color.sku) != m_material_key_by_sku.end()) {
            BOOST_LOG_TRIVIAL(warning) << "Skip duplicate filament color SKU: " << color.sku;
            continue;
        }

        m_material_key_by_sku.emplace(color.sku, material.material_key);
        m_colors_by_sku.emplace(color.sku, color);
        material.colors.emplace_back(std::move(color));
    }

    if (material.colors.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Skip official filament color file without valid colors: " << file;
        return false;
    }

    m_materials_by_key.emplace(material.material_key, std::move(material));

    return true;
}

void OfficialFilamentColorLibrary::clear()
{
    m_materials_by_key.clear();
    m_material_key_by_sku.clear();
    m_colors_by_sku.clear();
}

}} // namespace Slic3r::GUI
