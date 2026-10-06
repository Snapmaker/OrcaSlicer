#ifndef slic3r_GUI_SimpleShapeDialog_hpp_
#define slic3r_GUI_SimpleShapeDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/SimpleShape.hpp"

#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

struct SimpleShapeDialogOptions
{
    // Project the shape onto the surface of the object
    bool allow_use_surface = false;
    bool use_surface       = true;
    // Editing of existing shape
    bool is_edit = false;
    // Offer filament selection
    bool allow_filament = true;
    // 0 .. default, otherwise 1 based filament index
    int extruder = 0;
};

// Collect parameters of simple shape (circle, square, star, ...) embossed as SVG volume.
// Geometry is created in libslic3r/SimpleShape.
class SimpleShapeDialog : public DPIDialog
{
public:
    SimpleShapeDialog(wxWindow *parent, const SimpleShapeParams &params, const SimpleShapeDialogOptions &options);

    // Valid after ShowModal() returns wxID_OK
    const SimpleShapeParams        &params() const { return m_params; }
    const SimpleShapeDialogOptions &options() const { return m_options; }

    // Last used values for new shape
    static SimpleShapeParams load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    SimpleShapeParams collect() const;
    void              update_enabled();
    void              refresh_preview();
    void              save_to_config() const;

    SimpleShapeParams        m_params;
    SimpleShapeDialogOptions m_options;
    std::vector<std::string> m_filament_colors;

    wxChoice         *m_type          = nullptr;
    wxSpinCtrlDouble *m_size          = nullptr;
    wxSpinCtrlDouble *m_depth         = nullptr;
    wxSpinCtrl       *m_star_points   = nullptr;
    wxSpinCtrl       *m_inner_ratio   = nullptr;
    wxSpinCtrlDouble *m_corner_radius = nullptr;
    wxCheckBox       *m_use_surface   = nullptr;
    wxChoice         *m_filament      = nullptr;
    wxStaticBitmap   *m_preview       = nullptr;
    wxStaticText     *m_info          = nullptr;

    std::vector<wxWindow *> m_star_controls;
    std::vector<wxWindow *> m_inner_controls;
    std::vector<wxWindow *> m_corner_controls;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SimpleShapeDialog_hpp_
