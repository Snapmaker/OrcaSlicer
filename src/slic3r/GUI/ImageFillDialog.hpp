#ifndef slic3r_GUI_ImageFillDialog_hpp_
#define slic3r_GUI_ImageFillDialog_hpp_

// "Apply image fill...": pick a picture (or a gradient), a projection, the filaments it may use
// and how finely to subdivide, and see the result before applying it.
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md

#include "GUI_Utils.hpp"

#include "libslic3r/ImageFill.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class wxBitmapButton;
class wxCheckBox;
class wxCheckListBox;
class wxChoice;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

// One loaded filament, as the dialog needs it.
struct ImageFillFilament
{
    int                  id = 0;          // 1-based
    std::array<float, 3> color{0, 0, 0};  // sRGB 0..1
    std::string          name;            // for the checkbox label
};

class ImageFillDialog : public DPIDialog
{
public:
    // `filaments` are every loaded filament, physical and mixed, in id order. `initial` is what a
    // previous application recorded on the part (image_fill_params), so re-opening the dialog on
    // an already-filled part starts where the user left off. `mesh` and `existing` are the part
    // itself, used only for the preview - Apply re-runs the fill on the real volume.
    // `initial_image_row` is whether the part is ALREADY bound to an enabled ImageWeighted row
    // (Plater::apply_image_fill works this out from the part's own solid_infill_filament before
    // constructing the dialog) - so re-opening the dialog on a part using the feature starts with
    // the checkbox already ticked, the same promise `initial` makes for the projection settings.
    ImageFillDialog(wxWindow                                      *parent,
                    const std::vector<ImageFillFilament>          &filaments,
                    const ImageFillParams                         &initial,
                    ImageAssetStore                               *assets,
                    const indexed_triangle_set                    &mesh,
                    const TriangleSelector::TriangleSplittingData &existing,
                    const std::vector<int>                        &painted_states,
                    bool                                            initial_image_row = false);

    // Valid once ShowModal() returned wxID_OK. The asset, if the user picked a new image, is
    // already in the store this was constructed with.
    //
    // It RE-READS the controls before answering rather than trusting what the last change event
    // cached. The events are still what drives the preview, but Apply must be the widgets' own
    // answer: a control whose change event never arrived (a platform that does not send one for
    // a keyboard-driven wxChoice, a value typed into the spin control and committed by pressing
    // Apply) would otherwise apply the previous setting while the dialog showed the new one -
    // which is exactly the shape of "I chose Wrapped and got Flat".
    ImageFillParams params();

    // Whether "Nozzle-resolution dither (top surfaces)" is ticked - Plater::apply_image_fill()
    // reads this AFTER params() (collect() has already run by then) to decide whether to create
    // or update the part's ImageWeighted MixedFilament row, or drop the part's binding to one.
    // Phase 2's facet painting (image_fill_apply(), driven by params()) always happens regardless
    // of this box - see this control's own tooltip in the .cpp for why.
    bool image_row_dither() const;

    // Read an image file into the store (as the "Choose image..." button does), so the dialog can
    // start with an image dropped onto the 3D scene. Returns the asset id, empty on failure.
    static std::string add_image_file(const wxString &path, ImageAssetStore &assets);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void collect();          // widgets -> m_params
    void refresh_preview();  // m_params -> the thumbnail and the summary line
    void on_pick_image();

    ImageFillParams                                m_params;
    ImageAssetStore                               *m_assets = nullptr;
    const indexed_triangle_set                    &m_mesh;
    const TriangleSelector::TriangleSplittingData &m_existing;
    std::vector<ImageFillFilament>                 m_filaments;
    std::vector<int>                               m_painted_states;
    std::string                                    m_image_label;

    wxStaticText     *m_image_text      = nullptr;
    wxChoice         *m_source_choice   = nullptr;   // image / two-stop / three-stop gradient
    wxChoice         *m_projection      = nullptr;
    wxChoice         *m_axis            = nullptr;
    wxChoice         *m_faces           = nullptr;   // facing / through
    wxCheckBox       *m_from_negative   = nullptr;   // the -axis side, or inwards for a wrap
    wxCheckBox       *m_box_mirror      = nullptr;   // Box only: read the right way round outside
    wxChoice         *m_selection       = nullptr;   // whole part, or one painted state
    wxCheckBox       *m_flip_u          = nullptr;
    wxCheckBox       *m_flip_v          = nullptr;
    wxCheckListBox   *m_filament_list   = nullptr;
    wxSpinCtrlDouble *m_detail          = nullptr;
    wxCheckBox       *m_image_row       = nullptr;   // "Nozzle-resolution dither (top surfaces)"
    wxStaticBitmap   *m_preview         = nullptr;
    wxStaticText     *m_summary         = nullptr;
    wxButton         *m_ok              = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageFillDialog_hpp_
