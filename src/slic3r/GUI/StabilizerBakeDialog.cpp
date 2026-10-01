#include "StabilizerBakeDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include <wx/checkbox.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

StabilizerBakeDialog::StabilizerBakeDialog(wxWindow                    *parent,
                                           const wxString              &object_name,
                                           const wxString              &mode_label,
                                           const StabilizerBakeOptions &defaults,
                                           bool                         by_object,
                                           bool                         part_allowed)
    : DPIDialog(parent, wxID_ANY, _L("Bake stabilizers"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_options(defaults)
{
    wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);
    const int   gap  = FromDIP(10);

    wxStaticText *intro = new wxStaticText(this, wxID_ANY,
        format_wxstr(_L("Turn the side stabilizers of \"%1%\" (%2%) into real geometry that prints in any slicer. "
                        "They are planned from the current slice, so slice the plate after any change first."),
                     object_name, mode_label));
    intro->Wrap(FromDIP(420));
    root->Add(intro, 0, wxEXPAND | wxALL, gap);

    // ---- placement --------------------------------------------------------------------------------
    m_separate_rb = new wxRadioButton(this, wxID_ANY, _L("Separate object (recommended)"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_separate_rb->SetToolTip(_L("A new object next to the part with its own settings: no supports, no brim, solid, in the support "
                                 "filament. The tips touch the part and snap off. It does not move with the part."));
    m_part_rb = new wxRadioButton(this, wxID_ANY, _L("Part of this object (moves with it)"));
    m_part_rb->SetToolTip(format_wxstr(_L("A new part of the object. Parts of one object fuse where they touch, so the tips stop at "
                                          "least %1% mm short of the wall and do not touch it."),
                                       STABILIZER_BAKE_PART_MIN_GAP));
    m_separate_rb->SetValue(defaults.placement == StabilizerBakePlacement::SeparateObject || ! part_allowed);
    m_part_rb->SetValue(defaults.placement == StabilizerBakePlacement::PartOfObject && part_allowed);
    m_part_rb->Enable(part_allowed);
    root->Add(m_separate_rb, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);
    root->Add(m_part_rb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    if (! part_allowed) {
        wxStaticText *note = new wxStaticText(this, wxID_ANY, _L("Not available: the object's copies are rotated or scaled differently."));
        root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT, gap + FromDIP(16));
    }

    // ---- tip --------------------------------------------------------------------------------------
    auto add_number = [this, root, gap](const wxString &label, double value, const wxString &tooltip) {
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, label + ":"), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        wxTextCtrl *ctrl = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.2f", value), wxDefaultPosition, wxSize(FromDIP(80), -1));
        ctrl->SetToolTip(tooltip);
        row->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(new wxStaticText(this, wxID_ANY, " mm"), 0, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
        return ctrl;
    };
    m_tip_ctrl = add_number(_L("Tip diameter"), defaults.tip_diameter,
                            format_wxstr(_L("Other slicers can drop a tip thinner than %1% mm as too thin to print."),
                                         STABILIZER_BAKE_MIN_TIP_DIAMETER));
    m_gap_ctrl = add_number(_L("Tip gap"), defaults.tip_gap, _L("Space between each tip and the part. 0 touches it."));
    m_gap_note = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_gap_note, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);

    m_off_cb = new wxCheckBox(this, wxID_ANY, _L("Turn off the object's live stabilizers"));
    m_off_cb->SetValue(defaults.turn_off_live);
    m_off_cb->SetToolTip(_L("Otherwise EdgeSlicer prints its own stabilizers on top of the baked ones."));
    root->Add(m_off_cb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    // ---- notes ------------------------------------------------------------------------------------
    wxString notes = _L("The baked stabilizers do not follow later moves, rotations or scaling of the part, nor arrange: bake again "
                        "after changing it.");
    if (by_object)
        notes += "\n" + _L("This plate prints by object: a separate stabilizer object is printed apart from the part, which defeats "
                           "it. Choose \"Part of this object\" or print by layer.");
    wxStaticText *notes_text = new wxStaticText(this, wxID_ANY, notes);
    notes_text->Wrap(FromDIP(420));
    root->Add(notes_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    auto on_change = [this](wxCommandEvent &evt) { update_gap_note(); evt.Skip(); };
    m_separate_rb->Bind(wxEVT_RADIOBUTTON, on_change);
    m_part_rb->Bind(wxEVT_RADIOBUTTON, on_change);
    m_gap_ctrl->Bind(wxEVT_TEXT, on_change);
    update_gap_note();

    // ---- buttons ----------------------------------------------------------------------------------
    wxSizer *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (buttons != nullptr)
        root->Add(buttons, 0, wxEXPAND | wxALL, gap);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (validate())
            EndModal(wxID_OK);
    }, wxID_OK);

    SetSizer(root);
    root->SetSizeHints(this);
    Layout();
    root->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

static bool stab_bake_read_mm(wxTextCtrl *ctrl, double &out)
{
    wxString text = ctrl->GetValue();
    text.Trim(true).Trim(false);
    double value = 0.;
    if (! text.ToDouble(&value) && ! text.ToCDouble(&value))
        return false;
    out = value;
    return true;
}

void StabilizerBakeDialog::update_gap_note()
{
    double gap = 0.;
    const bool part = m_part_rb->GetValue();
    if (part && (! stab_bake_read_mm(m_gap_ctrl, gap) || gap < STABILIZER_BAKE_PART_MIN_GAP))
        m_gap_note->SetLabel(format_wxstr(_L("A part of the object bakes with a gap of %1% mm."), STABILIZER_BAKE_PART_MIN_GAP));
    else
        m_gap_note->SetLabel(wxEmptyString);
    Layout();
}

bool StabilizerBakeDialog::validate()
{
    double tip = 0., gap = 0.;
    if (! stab_bake_read_mm(m_tip_ctrl, tip) || tip < 0.3 || tip > 5.) {
        show_error(this, _L("Enter a tip diameter between 0.3 and 5 mm."));
        return false;
    }
    if (! stab_bake_read_mm(m_gap_ctrl, gap) || gap < 0. || gap > 2.) {
        show_error(this, _L("Enter a tip gap between 0 and 2 mm."));
        return false;
    }
    m_options.placement     = m_part_rb->GetValue() ? StabilizerBakePlacement::PartOfObject : StabilizerBakePlacement::SeparateObject;
    m_options.tip_diameter  = tip;
    m_options.tip_gap       = gap;
    m_options.turn_off_live = m_off_cb->GetValue();
    return true;
}

}} // namespace Slic3r::GUI
