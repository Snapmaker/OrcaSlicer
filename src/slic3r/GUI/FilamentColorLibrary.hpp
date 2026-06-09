#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r
{
namespace GUI
{

struct FilamentColor
{
    std::unordered_map<std::string, std::string> colorNames;
    std::string sku;
    int mode { 0 };
    std::string primaryColor;
    std::vector<std::string> colors;
};

struct FilamentMaterial
{
    std::string filamentId;
    std::string filamentName;
    std::string vendor;
    std::string filamentType;
    std::string filamentSubType;
    std::string file;
    std::vector<FilamentColor> colors;
};

class FilamentColorLibrary
{
public:
    static FilamentColorLibrary& Instance();

    bool EnsureLoaded();
    void Reload();

    bool FindFilamentById(const std::string& filament_id, FilamentMaterial& out_material);
    bool FindFilamentByName(const std::string& filament_name, FilamentMaterial& out_material);

private:
    bool LoadIndex();
    void Clear();

private:
    bool _loaded { false };

    std::unordered_map<std::string, FilamentMaterial> _filamentsById;
    std::unordered_map<std::string, std::string> _filamentIdByName;
};

} // namespace GUI
} // namespace Slic3r
