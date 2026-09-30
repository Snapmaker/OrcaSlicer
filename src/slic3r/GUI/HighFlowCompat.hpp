#ifndef slic3r_GUI_HighFlowCompat_hpp_
#define slic3r_GUI_HighFlowCompat_hpp_

#include <string>

namespace Slic3r { namespace GUI { namespace HighFlowCompat {

enum class CompatibilityLevel
{
    Compatible,
    NotRecommended,
    Unsupported
};

struct CompatibilityResult
{
    CompatibilityLevel level { CompatibilityLevel::Compatible };
    std::string         material;
};

/*
 * Checks whether a filament can be assigned to a high-flow nozzle.
 * filament_type is the configured material type and preset_name is its display name.
 * Returns the compatibility level and the material label used by the warning UI.
*/
CompatibilityResult check(const std::string &filament_type, const std::string &preset_name);

/*
 * check() of a preset and of the system preset it was made from (ancestor_name, empty for none):
 * a user preset "Flex 85" made from Snapmaker TPU 85A is rated as TPU 85A. The more severe result wins.
*/
CompatibilityResult check(const std::string &filament_type, const std::string &preset_name, const std::string &ancestor_name);

}}} // namespace Slic3r::GUI::HighFlowCompat

#endif // slic3r_GUI_HighFlowCompat_hpp_
