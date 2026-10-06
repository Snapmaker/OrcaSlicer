#ifndef slic3r_GUI_CustomModelsMenu_hpp_
#define slic3r_GUI_CustomModelsMenu_hpp_

// "Add Custom Models": the right-click submenu next to "Add Primitive" and "Add Handy models".
// It lists the user's own library (<data_dir>/custom_models) and drops a chosen file onto the
// current plate. The rules for the library live in libslic3r/CustomModels.hpp (unit tested);
// this file is the menu and the two plater actions.

#include <memory>

#include <boost/filesystem/path.hpp>

class wxMenu;
class wxWindow;

namespace Slic3r {
namespace GUI {

// <data_dir>/custom_models
boost::filesystem::path custom_models_dir();

// Adds one library file to the current plate, as one undo step. An STL / OBJ / STEP is added like
// a handy model. A 3MF brings its objects with their object / part settings, modifiers and paint;
// the project's printer, filament and process presets and its plates are never touched.
void add_custom_model(const boost::filesystem::path &file);

// True when the selection is one or more whole objects / instances.
bool can_save_selection_as_custom_model();

// Asks for a name and saves the selected object(s) into the library as a single-object 3MF.
void save_selection_as_custom_model();

// Opens <data_dir>/custom_models in the file explorer, creating it first when needed.
void open_custom_models_folder();

// The submenu. One instance per menu it is appended to; the list part is rebuilt from the folder
// each time refresh() is called (right before the menu is shown), so a file dropped into the
// folder shows up without restarting.
class CustomModelsMenu
{
public:
    struct State; // what the menu and its event handler share

    CustomModelsMenu();
    ~CustomModelsMenu();

    // Builds the submenu. `owner` is the menu the submenu will be appended to (the menu that
    // receives the item events on Windows), `parent` the window that answers the enable checks.
    wxMenu *create(wxMenu *owner, wxWindow *parent);
    // Re-reads the library folder and rebuilds the list part of the submenu.
    void    refresh();

private:
    std::shared_ptr<State> m_state;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_CustomModelsMenu_hpp_
