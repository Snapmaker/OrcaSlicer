#ifndef slic3r_GUI_ImageTraceDialog_hpp_
#define slic3r_GUI_ImageTraceDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/ImageTrace.hpp"

#include <array>
#include <string>
#include <vector>

#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSlider;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

// Options of the dialog which are not stored in the trace itself
struct ImageTraceDialogOptions
{
    // Project the shapes onto the surface of the object
    bool allow_use_surface = false;
    bool use_surface       = true;
    // Editing of existing trace
    bool is_edit = false;
    // Offer filament selection for the colour levels
    bool allow_filaments = true;
    // 0 .. default, otherwise 1 based filament index, index by colour level.
    // Levels without a value get the loaded filament with the nearest colour.
    std::vector<int> extruders;
};

// Trace a PNG / JPG image into shapes, one embossed SVG part per colour level.
// Geometry is created in libslic3r/ImageTrace.
class ImageTraceDialog : public DPIDialog
{
public:
    // source_png .. downscaled copy of the image which is stored into the project for re-trace
    // image_name .. file name without extension, used as name of the parts
    ImageTraceDialog(wxWindow                      *parent,
                     const TraceImage              &image,
                     const std::string             &source_png,
                     const std::string             &image_name,
                     const ImageTraceParams        &params,
                     const ImageTraceDialogOptions &options);
    ~ImageTraceDialog() override;

    // Valid after ShowModal() returns wxID_OK
    const ImageTraceParams        &params() const { return m_params; }
    const ImageTraceDialogOptions &options() const { return m_options; }
    // Final trace including SVG data of the levels
    const ImageTraceResult        &result() const { return m_result; }
    const std::string             &image_name() const { return m_image_name; }

    // Last used values for new trace
    static ImageTraceParams load_from_config();

    // Read and decode an image file, an error is shown to the user
    // source_png .. downscaled copy for the project, name .. file name without extension
    static bool load_image_file(wxWindow *parent, const wxString &path, TraceImage &image, std::string &source_png, std::string &name);
    // Ask for an image file, empty when canceled
    static wxString choose_image_file(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    ImageTraceParams collect() const;
    void             update_enabled();
    void             schedule_preview();
    void             refresh_preview();
    void             refresh_source_preview();
    void             update_filament_rows();
    void             change_image();
    bool             finish();
    void             save_to_config() const;
    std::string      level_color(size_t level) const;
    int              nearest_filament(const std::array<uint8_t, 3> &color) const;
    wxBitmap         render_preview(const ImageTraceResult &result, int max_px) const;

    TraceImage              m_image;
    TraceImage              m_preview_image; // downscaled for fast preview
    std::string             m_source_png;
    std::string             m_image_name;
    ImageTraceParams        m_params;
    ImageTraceDialogOptions m_options;
    ImageTraceResult        m_preview;
    ImageTraceResult        m_result;
    std::vector<std::string> m_filament_colors;
    // filament of the level was chosen by user (or comes from edited parts)
    std::vector<bool>       m_user_filament;
    wxTimer                 m_timer;

    wxStaticText     *m_image_label   = nullptr;
    wxButton         *m_change_image  = nullptr;
    wxSpinCtrl       *m_colors        = nullptr;
    wxChoice         *m_shape_from    = nullptr;
    wxSlider         *m_threshold     = nullptr;
    wxCheckBox       *m_auto_threshold = nullptr;
    wxCheckBox       *m_invert        = nullptr;
    wxCheckBox       *m_remove_bg     = nullptr;
    wxSpinCtrlDouble *m_blur          = nullptr;
    wxSpinCtrlDouble *m_despeckle     = nullptr;
    wxSpinCtrlDouble *m_detail        = nullptr;
    wxChoice         *m_resolution    = nullptr;
    wxSpinCtrlDouble *m_width         = nullptr;
    wxStaticText     *m_height_label  = nullptr;
    wxSpinCtrlDouble *m_depth         = nullptr;
    wxCheckBox       *m_stacked       = nullptr;
    wxSpinCtrlDouble *m_depth_step    = nullptr;
    wxCheckBox       *m_use_surface   = nullptr;
    wxCheckBox       *m_keep_source   = nullptr;

    std::array<wxStaticBitmap *, IMAGE_TRACE_MAX_COLORS> m_level_swatch{};
    std::array<wxStaticText *, IMAGE_TRACE_MAX_COLORS>   m_level_label{};
    std::array<wxChoice *, IMAGE_TRACE_MAX_COLORS>       m_level_filament{};

    wxStaticBitmap *m_source_preview = nullptr;
    wxStaticBitmap *m_preview_bitmap = nullptr;
    wxStaticText   *m_info           = nullptr;
    wxStaticText   *m_warning        = nullptr;
    wxButton       *m_ok             = nullptr;

    std::vector<wxWindow *> m_one_color;      // only for one colour level
    std::vector<wxWindow *> m_brightness;     // only for brightness threshold
    std::vector<wxWindow *> m_multi_color;    // only for more colour levels
    std::vector<wxWindow *> m_stacked_controls;
};

// What to do with an image dropped onto the 3D scene
enum class ImageDropAction { Cancel, Trace, Relief, ColorFill };

// Ask the user what to do with dropped image
// can_color_fill .. there is an object to apply Image Fill to
ImageDropAction ask_image_drop_action(wxWindow *parent, const wxString &file_name, bool can_color_fill);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageTraceDialog_hpp_
