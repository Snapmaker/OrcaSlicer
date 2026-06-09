#include "FilamentColorLibrary.hpp"

#include "FilamentColorUtils.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <unordered_set>
#include <utility>

namespace Slic3r
{
namespace GUI
{
namespace
{

boost::filesystem::path FilamentsColoursPath()
{
    boost::filesystem::path path = boost::filesystem::path(Slic3r::resources_dir());
    path /= "profiles";
    path /= "Snapmaker";
    path /= "filament";
    path /= "filaments_colours.json";
    return path.make_preferred();
}

std::string JsonString(const nlohmann::json& j, const char* key)
{
    nlohmann::json::const_iterator it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int JsonMode(const nlohmann::json& j)
{
    nlohmann::json::const_iterator it = j.find("mode");
    if (it == j.end() || !it->is_number_integer())
        return 0;
    return it->get<int>() == 1 ? 1 : 0;
}

bool JsonBool(const nlohmann::json& j, const char* key, bool default_value)
{
    nlohmann::json::const_iterator it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : default_value;
}

std::unordered_map<std::string, std::string> JsonStringMap(const nlohmann::json& j, const char* key)
{
    std::unordered_map<std::string, std::string> values;
    nlohmann::json::const_iterator object_it = j.find(key);
    if (object_it == j.end() || !object_it->is_object())
        return values;

    for (nlohmann::json::const_iterator it = object_it->begin(); it != object_it->end(); ++it)
    {
        if (it.value().is_string())
            values.emplace(it.key(), it.value().get<std::string>());
    }
    return values;
}

std::vector<std::string> JsonStringArray(const nlohmann::json& j, const char* key)
{
    std::vector<std::string> values;
    nlohmann::json::const_iterator it = j.find(key);
    if (it == j.end() || !it->is_array())
        return values;

    for (const nlohmann::json& item : *it)
    {
        if (item.is_string())
            values.emplace_back(item.get<std::string>());
    }
    return values;
}

std::string NormalizeJsonColor(const std::string& color, bool& has_invalid_color)
{
    const std::string normalized = FilamentColorUtils::NormalizeHexColor(color);
    if (!color.empty() && normalized.empty())
        has_invalid_color = true;
    return normalized;
}

bool LoadJsonFile(const boost::filesystem::path& path, nlohmann::json& out)
{
    boost::nowide::ifstream ifs(path.string());
    if (!ifs.is_open())
    {
        BOOST_LOG_TRIVIAL(warning) << "Failed to open official filament color file: " << path.string();
        return false;
    }

    out = nlohmann::json::parse(ifs, nullptr, false);
    if (out.is_discarded())
    {
        BOOST_LOG_TRIVIAL(warning) << "Failed to parse official filament color file: " << path.string();
        return false;
    }

    return true;
}

} // namespace

FilamentColorLibrary& FilamentColorLibrary::Instance()
{
    static FilamentColorLibrary library;
    return library;
}

bool FilamentColorLibrary::EnsureLoaded()
{
    if (_loaded)
        return true;

    Clear();
    if (!LoadIndex())
    {
        Clear();
        return false;
    }
    _loaded = true;
    return true;
}

void FilamentColorLibrary::Reload()
{
    Clear();
    _loaded = false;
    EnsureLoaded();
}

bool FilamentColorLibrary::FindFilamentById(const std::string& filament_id, FilamentMaterial& out_material)
{
    if (!EnsureLoaded())
        return false;

    std::unordered_map<std::string, FilamentMaterial>::const_iterator filament_it = _filamentsById.find(filament_id);
    if (filament_it == _filamentsById.end())
        return false;

    out_material = filament_it->second;
    return true;
}

bool FilamentColorLibrary::FindFilamentByName(const std::string& filament_name, FilamentMaterial& out_material)
{
    if (!EnsureLoaded())
        return false;

    std::unordered_map<std::string, std::string>::const_iterator id_it = _filamentIdByName.find(filament_name);
    if (id_it == _filamentIdByName.end())
        return false;

    std::unordered_map<std::string, FilamentMaterial>::const_iterator filament_it = _filamentsById.find(id_it->second);
    if (filament_it == _filamentsById.end())
        return false;

    out_material = filament_it->second;
    return true;
}

bool FilamentColorLibrary::LoadIndex()
{
    nlohmann::json root;
    const boost::filesystem::path file_path = FilamentsColoursPath();
    if (!LoadJsonFile(file_path, root))
        return false;

    nlohmann::json::const_iterator filaments_it = root.find("filaments");
    if (filaments_it == root.end() || !filaments_it->is_array())
    {
        BOOST_LOG_TRIVIAL(warning) << "Missing official filament color filaments array: " << file_path.string();
        return false;
    }

    std::unordered_map<std::string, FilamentMaterial> filaments_by_id;
    std::unordered_map<std::string, std::string> filament_id_by_name;
    std::unordered_set<std::string> skus;

    const std::string vendor = JsonString(root, "vendor");
    for (const nlohmann::json& filament_json : *filaments_it)
    {
        if (!filament_json.is_object())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip invalid official filament item: " << file_path.string();
            continue;
        }

        if (!JsonBool(filament_json, "enabled", true))
            continue;

        FilamentMaterial filament;
        filament.filamentId = JsonString(filament_json, "filament_id");
        filament.filamentName = JsonString(filament_json, "filament_name");
        filament.vendor = vendor;
        filament.filamentType = JsonString(filament_json, "filament_type");
        filament.filamentSubType = JsonString(filament_json, "filament_sub_type");
        filament.file = "filament/filaments_colours.json";

        if (filament.filamentId.empty() || filament.filamentName.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without id or name: " << file_path.string();
            continue;
        }

        if (filaments_by_id.find(filament.filamentId) != filaments_by_id.end())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament id: " << filament.filamentId;
            continue;
        }

        if (filament_id_by_name.find(filament.filamentName) != filament_id_by_name.end())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament name: " << filament.filamentName;
            continue;
        }

        nlohmann::json::const_iterator colors_it = filament_json.find("filament_color");
        if (colors_it == filament_json.end() || !colors_it->is_array())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without colors array: " << filament.filamentId;
            continue;
        }

        for (const nlohmann::json& color_json : *colors_it)
        {
            if (!color_json.is_object())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip invalid color item for official filament: " << filament.filamentId;
                continue;
            }

            if (!JsonBool(color_json, "enabled", true))
                continue;

            FilamentColor color;
            color.sku = JsonString(color_json, "sku");
            color.colorNames = JsonStringMap(color_json, "color_name");
            color.mode = JsonMode(color_json);

            bool has_invalid_color = false;
            for (const std::string& raw_color : JsonStringArray(color_json, "filament_color"))
            {
                const std::string normalized_color = NormalizeJsonColor(raw_color, has_invalid_color);
                if (!normalized_color.empty())
                    color.colors.emplace_back(normalized_color);
            }

            if (has_invalid_color)
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip color item with invalid color value: " << color.sku;
                continue;
            }

            if (color.sku.empty() || color.colors.empty())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip incomplete color item for official filament: "
                                           << filament.filamentId;
                continue;
            }

            if (color.mode == 1 && color.colors.size() < 2)
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip gradient color item with fewer than two colors: " << color.sku;
                continue;
            }

            // Only dual-color (2 colors) is supported for non-gradient mode at this time.
            if (color.mode == 0 && color.colors.size() > 2)
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip unsupported multi color item: " << color.sku;
                continue;
            }

            if (skus.find(color.sku) != skus.end())
            {
                BOOST_LOG_TRIVIAL(warning) << "Skip duplicate official filament color SKU: " << color.sku;
                continue;
            }

            color.primaryColor = color.colors.front();
            skus.emplace(color.sku);
            filament.colors.emplace_back(std::move(color));
        }

        if (filament.colors.empty())
        {
            BOOST_LOG_TRIVIAL(warning) << "Skip official filament without valid colors: " << filament.filamentId;
            continue;
        }

        filament_id_by_name.emplace(filament.filamentName, filament.filamentId);
        filaments_by_id.emplace(filament.filamentId, std::move(filament));
    }

    if (filaments_by_id.empty())
    {
        BOOST_LOG_TRIVIAL(warning) << "Official filament color file has no valid filaments: " << file_path.string();
        return false;
    }

    _filamentsById = std::move(filaments_by_id);
    _filamentIdByName = std::move(filament_id_by_name);
    return true;
}

void FilamentColorLibrary::Clear()
{
    _filamentsById.clear();
    _filamentIdByName.clear();
}

} // namespace GUI
} // namespace Slic3r
