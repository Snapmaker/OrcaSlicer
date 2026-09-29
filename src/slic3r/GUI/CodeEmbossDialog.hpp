#ifndef slic3r_GUI_CodeEmbossDialog_hpp_
#define slic3r_GUI_CodeEmbossDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/CodeEmboss.hpp"

#include <array>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

// Options of the dialog which are not stored in the code itself
struct CodeEmbossDialogOptions
{
    // Light (background) part is possible only for object parts,
    // negative volumes and modifiers use only dark modules (and logo)
    bool allow_light_part = true;
    // Project the code onto the surface of the object
    bool allow_use_surface = false;
    bool use_surface       = true;
    // Editing of existing code
    bool is_edit = false;
    // Offer filament selection for parts
    bool allow_filaments = true;
    // 0 .. default, otherwise 1 based filament index, index by CodePartRole
    std::array<int, 3> extruders = {0, 0, 0};
};

// Collect parameters of QR code / barcode which is embossed as SVG parts
// (dark modules, light background and optional logo).
// Geometry is created in libslic3r/CodeEmboss.
class CodeEmbossDialog : public DPIDialog
{
public:
    CodeEmbossDialog(wxWindow                    *parent,
                     const CodeEmbossParams      &params,
                     const CodeEmbossDialogOptions &options,
                     const ExPolygons            &logo = {});

    // Valid after ShowModal() returns wxID_OK
    const CodeEmbossParams        &params() const { return m_params; }
    const CodeEmbossDialogOptions &options() const { return m_options; }
    const ExPolygons              &logo() const { return m_logo; }

    // Last used values for new code
    static CodeEmbossParams load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    CodeEmbossParams collect() const;
    void             update_enabled();
    void             refresh_preview();
    void             choose_logo();
    void             save_to_config() const;
    wxBitmap         render_preview(const CodeEmbossResult &result, int max_px) const;

    CodeEmbossParams        m_params;
    CodeEmbossDialogOptions m_options;
    ExPolygons              m_logo;
    std::string             m_logo_name;
    bool                    m_is_valid = false;
    std::vector<std::string> m_filament_colors;

    wxChoice         *m_symbology    = nullptr;
    wxTextCtrl       *m_text         = nullptr;
    wxChoice         *m_ecc          = nullptr;
    wxSpinCtrlDouble *m_module_size  = nullptr;
    wxSpinCtrlDouble *m_bar_height   = nullptr;
    wxSpinCtrl       *m_quiet_zone   = nullptr;
    wxSpinCtrlDouble *m_dark_depth   = nullptr;
    wxCheckBox       *m_light_part   = nullptr;
    wxSpinCtrlDouble *m_light_depth  = nullptr;
    wxCheckBox       *m_use_surface  = nullptr;
    wxChoice         *m_dark_filament  = nullptr;
    wxChoice         *m_light_filament = nullptr;
    wxChoice         *m_logo_filament  = nullptr;

    wxCheckBox       *m_has_logo     = nullptr;
    wxButton         *m_logo_button  = nullptr;
    wxStaticText     *m_logo_label   = nullptr;
    wxSpinCtrl       *m_logo_size    = nullptr;
    wxSpinCtrl       *m_logo_margin  = nullptr;
    wxChoice         *m_logo_clear   = nullptr;
    wxSpinCtrlDouble *m_logo_depth   = nullptr;

    wxStaticBitmap   *m_preview      = nullptr;
    wxStaticText     *m_info         = nullptr;
    wxStaticText     *m_warning      = nullptr;
    wxButton         *m_ok           = nullptr;

    std::vector<wxWindow *> m_qr_only;
    std::vector<wxWindow *> m_linear_only;
    std::vector<wxWindow *> m_logo_controls;
    std::vector<wxWindow *> m_light_controls;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_CodeEmbossDialog_hpp_
