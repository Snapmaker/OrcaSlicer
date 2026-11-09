#ifndef slic3r_GUI_FilamentGroupDialog_hpp_
#define slic3r_GUI_FilamentGroupDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

class Button;
class ComboBox;
class wxScrolledWindow;
class wxStaticText;

namespace Slic3r { namespace GUI {

// Edit the flow settings for the filaments used by the model. Changes remain
// local until Confirm; entries for unused filaments are preserved.
class FilamentGroupDialog : public DPIDialog
{
public:
    explicit FilamentGroupDialog(wxWindow* parent, bool default_all_high_flow = false);

    const std::vector<FilamentVolumeType>& mapping() const { return m_mapping; }
    void                                   move_filament(size_t filament_idx, bool to_high_flow);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    struct FilamentInfo
    {
        size_t      filament_idx;
        wxColour    color;
        std::string type_raw;
        std::string preset_name;
        bool        has_high_flow_settings;
        std::string copied_from;
    };

    void load_filaments();
    void assign_all(bool high_flow);
    void update_assignments();
    void update_warnings();
    void add_high_flow_settings(size_t filament_idx);
    void change_preset(size_t row, std::string name);

    std::vector<FilamentInfo>       m_filaments;
    std::vector<FilamentVolumeType> m_mapping;
    std::vector<ComboBox*>          m_choices;
    std::vector<wxStaticText*>      m_details;
    std::vector<wxStaticText*>      m_names;
    std::vector<Button*>            m_add_buttons;
    wxScrolledWindow*               m_list{nullptr};
    wxStaticText*                   m_summary{nullptr};
    wxBoxSizer*                     m_warning_sizer{nullptr};
    Button*                         m_confirm_button{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentGroupDialog_hpp_
