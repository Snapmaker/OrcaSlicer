#ifndef slic3r_GUI_ThemesPage_hpp_
#define slic3r_GUI_ThemesPage_hpp_

#include <map>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/image.h>
#include <wx/window.h>

#include "slic3r/Utils/ThemePack.hpp"

class Button;
class ComboBox;
class TextInput;
class wxColourPickerCtrl;
class wxStaticText;

namespace Slic3r {
namespace GUI {

class ThemePreview;

// Preferences > Themes: pick the theme EdgeSlicer starts with, and change a theme's parts one by
// one (colours, fonts, corners, title bar banner) with a picture of the result, instead of editing
// theme.json by hand (docs/themes.md). "Default" is the stock look; it cannot be changed or
// deleted, so there is always a clean theme to go back to. Shipped themes cannot be changed
// either: saving one makes the user's own copy.
class ThemesPage : public wxWindow
{
public:
    explicit ThemesPage(wxWindow* parent);

    // What the preview draws: the colour a role will have, the theme, and its banner image.
    wxColour               role_colour(const std::string& role) const;
    const ThemePack::Spec& edited() const { return m_spec; }
    const wxImage&         banner_image();

private:
    struct ColourRow
    {
        std::string         role;
        wxStaticText*       label  = nullptr;
        wxColourPickerCtrl* picker = nullptr;
        wxStaticText*       state  = nullptr; // "stock", or "reset" to go back to it
    };
    struct FontSlot
    {
        ThemePack::Font ThemePack::Spec::*member;
        ComboBox*                          combo = nullptr;
        wxStaticText*                      sample = nullptr; // a line of text in the chosen font
        std::vector<ThemePack::Font>       options;
    };

    wxWindow* build_picker(wxSizer* sizer);
    void      build_details(wxSizer* sizer);
    void      build_colours(wxSizer* sizer);
    void      build_fonts(wxSizer* sizer);
    void      build_shapes(wxSizer* sizer);
    void      build_titlebar(wxSizer* sizer);
    void      build_actions(wxSizer* sizer);

    void fill_list();
    // Loads theme `id` ("" for Default) into the controls.
    void load(const std::string& id);
    void show_spec();
    void fill_font_slot(FontSlot& slot, const ThemePack::Font& current);
    // Hands the theme's own font files to the system, so the fonts can be drawn here even when
    // this theme is not the one running.
    void register_fonts();
    void update_font_sample(FontSlot& slot);
    void update_colour_row(ColourRow& row);
    void update_state();
    void changed();

    void on_pick(int selection);
    void on_install();
    void on_delete();
    void on_font_file(FontSlot& slot);
    void on_banner_file();
    bool save(bool as_new);
    bool confirm_discard();
    // After the theme in use changed, or a different one was chosen: themes load at startup, so
    // ask whether to restart EdgeSlicer now. "Later" leaves the note on the page.
    void offer_restart();

    // A file the user picked, as the path it gets inside the pack ("fonts/Name.ttf").
    std::string import_path(const std::string& folder, const boost::filesystem::path& file) const;

    bool editable() const { return !m_id.empty() && m_installed; }

    // The theme being edited.
    std::string                                    m_id;           // "" for Default
    bool                                           m_installed = false;
    boost::filesystem::path                        m_dir;          // its folder, empty for Default
    ThemePack::Spec                                m_spec;
    std::map<std::string, boost::filesystem::path> m_imports;      // picked files, by path in the pack
    bool                                           m_dirty   = false;
    bool                                           m_filling = false; // the controls are being set, not changed
    std::string                                    m_saved_running; // the running theme was saved over

    std::vector<std::string>  m_ids;   // the pick list, [0] = "" for Default
    std::vector<wxString>     m_faces; // installed font faces, sorted
    wxImage                   m_banner;
    std::string               m_banner_key;

    ComboBox*      m_list       = nullptr;
    Button*        m_delete     = nullptr;
    Button*        m_default    = nullptr;
    wxStaticText*  m_note       = nullptr;
    TextInput*     m_name       = nullptr;
    TextInput*     m_author     = nullptr;
    ComboBox*      m_base       = nullptr;
    std::vector<ColourRow> m_colours;
    std::vector<FontSlot>  m_fonts;
    ComboBox*      m_button_radius = nullptr;
    ComboBox*      m_box_radius    = nullptr;
    wxStaticText*  m_banner_name   = nullptr;
    Button*        m_banner_clear  = nullptr;
    ComboBox*      m_banner_align  = nullptr;
    ThemePreview*  m_preview    = nullptr;
    Button*        m_save       = nullptr;
    Button*        m_save_as    = nullptr;
    Button*        m_discard    = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
