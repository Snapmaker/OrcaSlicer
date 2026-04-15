#include "MixedFilamentAdapter.hpp"

namespace Slic3r {
namespace GUI {

MixedFilament dialog_result_to_mixed_filament(
    const MixedFilamentResult &result,
    const std::vector<std::string> &filament_colours)
{
    MixedFilament mf;

    if (result.components.size() < 2)
        return mf;

    mf.component_a = result.components[0];
    mf.component_b = result.components[1];
    mf.mix_b_percent = (result.ratios.size() >= 2) ? result.ratios[1] : 50;
    mf.enabled = true;
    mf.custom = true;
    mf.distribution_mode = int(MixedFilament::Simple);

    if (result.gradient_enabled) {
        mf.distribution_mode = int(MixedFilament::LayerCycle);
        if (result.gradient_direction == 0)
            mf.gradient_component_ids = std::to_string(mf.component_a) + std::to_string(mf.component_b);
        else
            mf.gradient_component_ids = std::to_string(mf.component_b) + std::to_string(mf.component_a);
    }

    // Compute display color
    if (mf.component_a >= 1 && mf.component_a <= filament_colours.size() &&
        mf.component_b >= 1 && mf.component_b <= filament_colours.size()) {
        mf.display_color = MixedFilamentManager::blend_color(
            filament_colours[mf.component_a - 1],
            filament_colours[mf.component_b - 1],
            100 - mf.mix_b_percent, mf.mix_b_percent);
    }

    return mf;
}

MixedFilamentResult mixed_filament_to_dialog_result(const MixedFilament &mf)
{
    MixedFilamentResult result;
    result.components = {mf.component_a, mf.component_b};
    result.ratios = {100 - mf.mix_b_percent, mf.mix_b_percent};
    result.gradient_enabled = (mf.distribution_mode == int(MixedFilament::LayerCycle));
    result.gradient_direction = 0;
    if (result.gradient_enabled && !mf.gradient_component_ids.empty()) {
        // Decode first ID from gradient_component_ids (each char is '1'-'9')
        unsigned int first_id = 0;
        char ch = mf.gradient_component_ids[0];
        if (ch >= '1' && ch <= '9')
            first_id = unsigned(ch - '0');
        // If first gradient component is component_b, direction is reversed
        if (first_id > 0 && first_id == mf.component_b)
            result.gradient_direction = 1;
    }
    return result;
}

void apply_dialog_result_to_manager(
    const MixedFilamentResult &result,
    MixedFilamentManager &mgr,
    const std::vector<std::string> &filament_colours,
    ConfigOptionString *opt_definitions)
{
    MixedFilament mf = dialog_result_to_mixed_filament(result, filament_colours);
    mgr.mixed_filaments().push_back(mf);
    if (opt_definitions)
        opt_definitions->value = mgr.serialize_custom_entries();
}

void update_dialog_result_in_manager(
    const MixedFilamentResult &result,
    MixedFilamentManager &mgr,
    size_t mixed_index,
    const std::vector<std::string> &filament_colours,
    ConfigOptionString *opt_definitions)
{
    auto &entries = mgr.mixed_filaments();
    if (mixed_index >= entries.size())
        return;

    MixedFilament mf = dialog_result_to_mixed_filament(result, filament_colours);
    // Preserve stable_id and origin flags from existing entry
    mf.stable_id = entries[mixed_index].stable_id;
    mf.origin_auto = entries[mixed_index].origin_auto;
    entries[mixed_index] = mf;

    if (opt_definitions)
        opt_definitions->value = mgr.serialize_custom_entries();
}

} // namespace GUI
} // namespace Slic3r
