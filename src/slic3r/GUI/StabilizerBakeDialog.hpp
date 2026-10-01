#ifndef slic3r_GUI_StabilizerBakeDialog_hpp_
#define slic3r_GUI_StabilizerBakeDialog_hpp_

// The options dialog shown before "Bake stabilizers..." (ObjectList::bake_stabilizers): where the
// baked stabilizers go, and the tip diameter and gap they are baked with. The geometry is all in
// libslic3r/Support/StabilizerBake; the run itself is a background job (Jobs/StabilizerBakeJob).
//
// Study: tests/research_stabilizer_bake.md (phase 1, section 3).

#include "GUI_Utils.hpp"

#include "libslic3r/Support/StabilizerBake.hpp"

class wxCheckBox;
class wxRadioButton;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

class StabilizerBakeDialog : public DPIDialog
{
public:
    // `defaults` from stabilizer_bake_defaults(); `mode_label` names the object's stabilizer mode
    // (Auto / Manual); `by_object` warns that by-object printing treats the separate object as one
    // to print apart; `part_allowed` is false when the object's instances differ in rotation or
    // scale, which one shared part cannot follow.
    StabilizerBakeDialog(wxWindow                    *parent,
                         const wxString              &object_name,
                         const wxString              &mode_label,
                         const StabilizerBakeOptions &defaults,
                         bool                         by_object,
                         bool                         part_allowed);

    // Valid once ShowModal() returned wxID_OK.
    const StabilizerBakeOptions &options() const { return m_options; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    bool validate();
    void update_gap_note();

    StabilizerBakeOptions m_options;

    wxRadioButton *m_separate_rb = nullptr;
    wxRadioButton *m_part_rb     = nullptr;
    wxTextCtrl    *m_tip_ctrl    = nullptr;
    wxTextCtrl    *m_gap_ctrl    = nullptr;
    wxCheckBox    *m_off_cb      = nullptr;
    wxStaticText  *m_gap_note    = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_StabilizerBakeDialog_hpp_
