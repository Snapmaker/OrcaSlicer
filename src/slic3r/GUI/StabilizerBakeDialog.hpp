#ifndef slic3r_GUI_StabilizerBakeDialog_hpp_
#define slic3r_GUI_StabilizerBakeDialog_hpp_

// The dialog shown before "Bake stabilizers..." (ObjectList::bake_stabilizers): where the baked
// stabilizers go. The tip diameter and gap are shown, not edited: the bake always uses the settings
// the object was sliced with (StabilizerBake.hpp). Built like SliceBakeDialog - the app's Label,
// ComboBox and DialogButtons widgets on a DPIDialog, dark mode through UpdateDlgDarkUI - and like it,
// it only collects the choice; the run is a background job (Jobs/StabilizerBakeJob).
//
// Study: tests/research_stabilizer_bake.md (phase 1, section 3).

#include "GUI_Utils.hpp"

#include "libslic3r/Support/StabilizerBake.hpp"

#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

class StabilizerBakeDialog : public DPIDialog
{
public:
    // `mode_label` names the object's stabilizer mode (Auto / Manual). `tip_diameter` and `sliced_gap`
    // are the object's resolved settings from the current slice. `by_object` warns that by-object
    // printing prints a separate object apart from the part; `part_allowed` is false when the object's
    // copies differ in rotation or scale, which one shared part cannot follow.
    StabilizerBakeDialog(wxWindow       *parent,
                         const wxString &object_name,
                         const wxString &mode_label,
                         double          tip_diameter,
                         double          sliced_gap,
                         bool            by_object,
                         bool            part_allowed);

    // Valid once ShowModal() returned wxID_OK.
    const StabilizerBakeOptions &options() const { return m_options; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    StabilizerBakePlacement placement() const;
    void                    update_placement();

    StabilizerBakeOptions m_options;
    double                m_sliced_gap   = 0.;
    bool                  m_part_allowed = true;

    ::ComboBox *m_placement_choice = nullptr;
    ::Label    *m_gap_value        = nullptr;
    ::Label    *m_gap_warning      = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_StabilizerBakeDialog_hpp_
