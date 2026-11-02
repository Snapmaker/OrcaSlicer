#include "MixingColorMatchDialog.hpp"


namespace Slic3r {
namespace GUI {

MixedColorMatchRecipeResult prompt_best_color_match_recipe(wxWindow*                       parent,
                                                           const std::vector<std::string>& physical_colors,
                                                           const wxColour&                 initial_color)
{
    MixedFilamentColorMatchDialog dlg(parent, physical_colors, initial_color);
    dlg.begin_initial_recipe_load();
    if (dlg.ShowModal() != wxID_OK) {
        MixedColorMatchRecipeResult cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }

    return dlg.selected_recipe();
}

} // namespace GUI
} // namespace Slic3r
