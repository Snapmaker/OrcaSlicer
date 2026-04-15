#ifndef slic3r_MixedFilamentAdapter_hpp_
#define slic3r_MixedFilamentAdapter_hpp_

#include "MixedFilamentDialog.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

// BS Dialog output -> FS data model
MixedFilament dialog_result_to_mixed_filament(
    const MixedFilamentResult &result,
    const std::vector<std::string> &filament_colours);

// FS data model -> BS Dialog input (for editing)
MixedFilamentResult mixed_filament_to_dialog_result(
    const MixedFilament &mf);

// One-shot: Dialog result -> Manager + serialize to PrintConfig
void apply_dialog_result_to_manager(
    const MixedFilamentResult &result,
    MixedFilamentManager &mgr,
    const std::vector<std::string> &filament_colours,
    ConfigOptionString *opt_definitions);

// Update existing entry in Manager by index
void update_dialog_result_in_manager(
    const MixedFilamentResult &result,
    MixedFilamentManager &mgr,
    size_t mixed_index,
    const std::vector<std::string> &filament_colours,
    ConfigOptionString *opt_definitions);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MixedFilamentAdapter_hpp_
