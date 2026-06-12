#pragma once

#include <cstddef>
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
    int mode { 0 }; // 0 = solid/averaged, 1 = gradient
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

    bool FindFilamentById(const std::string& filamentId, FilamentMaterial& outMaterial);
    bool FindFilamentByName(const std::string& filamentName, FilamentMaterial& outMaterial);

private:
    bool LoadIndex();
    bool FindFilamentByIndex(size_t index, FilamentMaterial& outMaterial) const;
    void Clear();

private:
    bool _loaded { false };

    std::vector<FilamentMaterial> _filaments;
    std::unordered_map<std::string, size_t> _filamentIndexById;
    std::unordered_map<std::string, size_t> _filamentIndexByName;
    std::unordered_map<std::string, size_t> _filamentIndexByMatchName;
};

} // namespace GUI
} // namespace Slic3r
