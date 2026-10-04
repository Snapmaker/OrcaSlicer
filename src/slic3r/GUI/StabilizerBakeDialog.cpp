#include "StabilizerBakeDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "format.hpp"

#include <wx/sizer.h>

namespace Slic3r { namespace GUI {

static wxString stab_bake_mm(double v) { return wxString::Format("%.2f ", v) + _L("mm"); }

StabilizerBakeDialog::StabilizerBakeDialog(wxWindow       *parent,
                                           const wxString &object_name,
                                           const wxString &mode_label,
                                           double          tip_diameter,
                                           double          sliced_gap,
                                           bool            by_object,
                                           bool            part_allowed)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Bake stabilizers"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_sliced_gap(sliced_gap)
    , m_part_allowed(part_allowed)
{
    // The same frame, fonts and widgets as SliceBakeDialog: white in light mode, mapped for dark mode
    // and the active theme by UpdateDlgDarkUI() at the end. Every text is a ::Label, never a bare
    // wxStaticText (see SliceBakeDialog / FillBedDialog for why).
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    const int wrap = FromDIP(380);
    auto v_sizer = new wxBoxSizer(wxVERTICAL);

    auto intro = new ::Label(this, Label::Body_14,
                             format_wxstr(_L("Turn the side stabilizers of \"%1%\" (%2%) into real geometry that prints in any slicer."),
                                          object_name, mode_label));
    intro->Wrap(wrap);
    v_sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto f_sizer = new wxFlexGridSizer(2, FromDIP(4), FromDIP(20));
    const wxSize input_size(FromDIP(220), -1);

    // ---- placement ----------------------------------------------------------------------------------
    const wxString placement_tip =
        _L("Separate object: a new object over the part with its own settings (no supports, no brim, solid, in the "
           "support filament). The tips touch the part and snap off. It does not move with the part.\n\n"
           "Part of this object: a new part that moves with the object. Parts of one object fuse where they touch, "
           "so its tips always stop short of the wall.");
    auto placement_label = new ::Label(this, Label::Body_14, _L("Placement") + ":");
    placement_label->SetToolTip(placement_tip);
    m_placement_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_placement_choice->Append(_L("Separate object (recommended)"));
    if (part_allowed)
        m_placement_choice->Append(_L("Part of this object"));
    m_placement_choice->SetSelection(0);
    m_placement_choice->SetToolTip(placement_tip);
    f_sizer->Add(placement_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_placement_choice, 0, wxALIGN_CENTER_VERTICAL);

    // ---- the sliced settings, read only -------------------------------------------------------------
    auto tip_label = new ::Label(this, Label::Body_14, _L("Tip diameter") + ":");
    auto tip_value = new ::Label(this, Label::Body_14, stab_bake_mm(tip_diameter));
    f_sizer->Add(tip_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(tip_value, 0, wxALIGN_CENTER_VERTICAL);

    auto gap_label = new ::Label(this, Label::Body_14, _L("Tip gap") + ":");
    m_gap_value    = new ::Label(this, Label::Body_14, stab_bake_mm(sliced_gap));
    f_sizer->Add(gap_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_gap_value, 0, wxALIGN_CENTER_VERTICAL);

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // A secondary note, in the light-mode grey SliceBakeDialog uses for its notes.
    auto add_note = [this, v_sizer, wrap](const wxString &text) {
        auto note = new ::Label(this, Label::Body_14, text);
        note->Wrap(wrap);
        note->SetForegroundColour(wxColour("#6B6B6B"));
        v_sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        return note;
    };
    add_note(_L("From the current slice; change them in the object's settings and re-slice."));
    if (tip_diameter < STABILIZER_BAKE_MIN_TIP_DIAMETER - EPSILON)
        add_note(format_wxstr(_L("Other slicers can drop tips thinner than %1% mm as too thin to print."),
                              STABILIZER_BAKE_MIN_TIP_DIAMETER));
    if (! part_allowed)
        add_note(_L("Part of this object is not offered: the object's copies are rotated or scaled differently."));

    // ---- the part's gap warning, in the app's warning orange ----------------------------------------
    // #FF6F00 is the light-mode warning colour; UpdateDlgDarkUI() maps it for dark mode and the theme.
    m_gap_warning = new ::Label(this, Label::Body_14,
                                format_wxstr(_L("The tips will be shortened to keep a %1% mm gap from the part, which a part of the "
                                                "same object needs so it does not fuse to it. The slice used %2% mm."),
                                             STABILIZER_BAKE_PART_MIN_GAP, wxString::Format("%.2f", sliced_gap)));
    m_gap_warning->Wrap(wrap);
    m_gap_warning->SetForegroundColour(wxColour("#FF6F00"));
    v_sizer->Add(m_gap_warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- notes in the normal text colour -------------------------------------------------------------
    wxString notes = _L("The object's live stabilizers are switched off. The baked stabilizers do not follow later moves, "
                        "rotations or scaling of the part, nor arrange: bake again after changing it.");
    if (by_object)
        notes += "\n\n" + _L("This plate prints by object: a separate stabilizer object is printed apart from the part, which "
                             "defeats it. Choose \"Part of this object\" or print by layer.");
    auto notes_text = new ::Label(this, Label::Body_14, notes);
    notes_text->Wrap(wrap);
    v_sizer->Add(notes_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_placement_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        e.Skip();
        update_placement();
    });

    // ---- buttons: the themed primary (Bake) and Cancel ----------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Bake"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_options.placement = placement();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(v_sizer);
    update_placement();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

StabilizerBakePlacement StabilizerBakeDialog::placement() const
{
    return m_part_allowed && m_placement_choice != nullptr && m_placement_choice->GetSelection() == 1 ?
               StabilizerBakePlacement::PartOfObject : StabilizerBakePlacement::SeparateObject;
}

void StabilizerBakeDialog::update_placement()
{
    const StabilizerBakePlacement p   = placement();
    const double                  gap = stabilizer_bake_gap(p, m_sliced_gap);
    // The gap the bake will use: the slice's, or the part's minimum.
    m_gap_value->SetLabel(stab_bake_mm(gap));
    m_gap_warning->Show(gap > m_sliced_gap + EPSILON);
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void StabilizerBakeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
