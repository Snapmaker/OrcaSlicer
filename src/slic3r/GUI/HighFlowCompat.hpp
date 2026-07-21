#ifndef slic3r_GUI_HighFlowCompat_hpp_
#define slic3r_GUI_HighFlowCompat_hpp_

#include <optional>
#include <string>

namespace Slic3r { namespace GUI { namespace HighFlowCompat {

// MOCK implementation for debugging until the process team delivers the real
// filament / high-flow compatibility table. Checks both the filament_type and
// the preset display name, because e.g. wood-filled filaments usually carry
// filament_type "PLA" and only the preset name contains "Wood".
// Returns the display label (UTF-8, already translated) of the incompatible
// material group when the filament is not recommended on a high flow nozzle;
// std::nullopt when it is fine.
// Keep this signature stable: the real implementation only replaces the body.
std::optional<std::string> incompat_reason(const std::string &filament_type, const std::string &preset_name);

}}} // namespace Slic3r::GUI::HighFlowCompat

#endif // slic3r_GUI_HighFlowCompat_hpp_
