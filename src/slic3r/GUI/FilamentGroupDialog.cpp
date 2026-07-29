#include "FilamentGroupDialog.hpp"

#include "FlowTypeHelper.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "HighFlowCompat.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <functional>
#include <set>

namespace Slic3r { namespace GUI {

namespace {

const char *DRAG_PREFIX = "sm_filament:";

// 20x20 colour block with the filament number inside and the material name below.
class FilamentChip : public wxPanel
{
public:
    FilamentChip(wxWindow *parent, size_t filament_idx, const wxColour &color, const wxString &label)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition,
                  wxSize(parent->FromDIP(32), parent->FromDIP(38)))
        , m_idx(filament_idx)
        , m_color(color)
        , m_label(label)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &FilamentChip::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, &FilamentChip::on_left_down, this);
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();

        const int block = FromDIP(20);
        const int x     = (GetSize().x - block) / 2;
        dc.SetPen(wxPen(wxColour("#DBDBDA")));
        dc.SetBrush(wxBrush(m_color));
        dc.DrawRectangle(x, 0, block, block);

        // White number on dark colours, black on light ones.
        const double lum = 0.299 * m_color.Red() + 0.587 * m_color.Green() + 0.114 * m_color.Blue();
        dc.SetTextForeground(lum < 160.0 ? *wxWHITE : *wxBLACK);
        dc.SetFont(Label::Body_13);
        const wxString num  = wxString::Format("%zu", m_idx + 1);
        const wxSize   next = dc.GetTextExtent(num);
        dc.DrawText(num, x + (block - next.x) / 2, (block - next.y) / 2);

        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#333333")));
        dc.SetFont(Label::Body_10);
        const wxSize lext = dc.GetTextExtent(m_label);
        dc.DrawText(m_label, (GetSize().x - lext.x) / 2, block + FromDIP(2));
    }

    void on_left_down(wxMouseEvent &)
    {
        wxTextDataObject data(wxString(DRAG_PREFIX) + wxString::Format("%zu", m_idx));
        wxDropSource     source(data, this);
        source.DoDragDrop(wxDrag_CopyOnly);
    }

    size_t   m_idx;
    wxColour m_color;
    wxString m_label;
};

// 20x20 swap button that swaps the contents of the standard and high flow groups.
class SwapButton : public wxPanel
{
public:
    SwapButton(wxWindow *parent, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition,
                  wxSize(parent->FromDIP(20), parent->FromDIP(20)))
        , m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Swap the two groups"));
        m_bitmap = ScalableBitmap(this, "icon_swap_groups", 14);
        Bind(wxEVT_PAINT, &SwapButton::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
            if (m_on_click)
                m_on_click();
        });
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxBitmap &bmp = m_bitmap.bmp();
        dc.DrawBitmap(bmp, (GetSize().x - bmp.GetWidth()) / 2, (GetSize().y - bmp.GetHeight()) / 2);
    }

    ScalableBitmap       m_bitmap;
    std::function<void()> m_on_click;
};

class GroupDropTarget : public wxTextDropTarget
{
public:
    GroupDropTarget(FilamentGroupDialog *dialog, bool high_flow) : m_dialog(dialog), m_high_flow(high_flow) {}

    bool OnDropText(wxCoord, wxCoord, const wxString &text) override
    {
        const wxString prefix = wxString(DRAG_PREFIX);
        if (!text.StartsWith(prefix))
            return false;
        unsigned long idx = 0;
        if (!text.Mid(prefix.size()).ToULong(&idx))
            return false;
        m_dialog->move_filament(size_t(idx), m_high_flow);
        return true;
    }

private:
    FilamentGroupDialog *m_dialog;
    bool                 m_high_flow;
};

} // anonymous namespace

FilamentGroupDialog::FilamentGroupDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                _L("Custom Filament Grouping"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    SetFont(Label::Body_14);

    load_filaments();
    m_mapping = wxGetApp().preset_bundle->get_filament_volume_types();
    m_mapping.resize(wxGetApp().preset_bundle->filament_presets.size(), fvtStandard);

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY, _L("We will slice based on the current assignment:"));
    intro->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    v_sizer->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // Group panel per Figma 27673:62102: #F3F3F3 rounded-8 background box,
    // title with 16px inset, chip grid with 8px inset.
    auto make_group = [this](const wxString &title, wxFlexGridSizer *&grid, bool high_flow) -> StaticBox * {
        StaticBox *box = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(328), -1));
        box->SetCornerRadius(FromDIP(8));
        box->SetBorderWidth(0);
        const wxColour box_bg = StateColor::darkModeColorFor(wxColour("#F3F3F3"));
        box->SetBackgroundColorNormal(box_bg);
        // Also set the plain wx background so children (title / chips) inherit it.
        box->SetBackgroundColour(box_bg);

        auto *box_sizer = new wxBoxSizer(wxVERTICAL);
        auto *label     = new wxStaticText(box, wxID_ANY, title);
        label->SetFont(Label::Body_14);
        label->SetBackgroundColour(box_bg);
        box_sizer->Add(label, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(16));
        box_sizer->AddSpacer(FromDIP(16));
        grid = new wxFlexGridSizer(0, 8, FromDIP(8), FromDIP(8));
        box_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box_sizer->AddStretchSpacer();
        box->SetSizer(box_sizer);
        box->SetMinSize(wxSize(FromDIP(328), FromDIP(148)));
        box->SetDropTarget(new GroupDropTarget(this, high_flow));
        return box;
    };

    auto *groups_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_std_box          = make_group(_L("Standard Nozzle"), m_std_grid, false);
    m_high_box         = make_group(_L("High Flow Nozzle"), m_high_grid, true);

    auto *swap = new SwapButton(this, [this]() { swap_groups(); });

    groups_sizer->Add(m_std_box, 0, wxALIGN_TOP);
    groups_sizer->Add(swap, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(16));
    groups_sizer->Add(m_high_box, 0, wxALIGN_TOP);
    v_sizer->Add(groups_sizer, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *tip = new wxStaticText(this, wxID_ANY, _L("Tip: You can drag filaments to assign them to different nozzles"));
    tip->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    v_sizer->Add(tip, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_warning_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(m_warning_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *dlg_btns = new DialogButtons(this, {"Cancel", "Confirm"});
    m_confirm_button = dlg_btns->GetCONFIRM();
    m_confirm_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        FlowType::apply_custom_mapping(m_mapping);
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND | wxALL, FromDIP(8));

    SetSizer(v_sizer);
    rebuild_chips();
    update_warnings();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentGroupDialog::load_filaments()
{
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    const auto   *colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");

    // Requirement: list only the filaments actually used by objects in the scene
    // (across all plates), not every filament configured in the sidebar. When the
    // used set cannot be determined (empty scene), fall back to all filaments.
    std::set<int> used; // 1-based filament ids
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        used = plater->get_partplate_list().get_extruders(true);

    for (size_t i = 0; i < bundle.filament_presets.size(); ++i) {
        if (!used.empty() && used.find(int(i) + 1) == used.end())
            continue;
        FilamentInfo info;
        info.filament_idx = i;
        info.color = colors != nullptr && i < colors->values.size() ? wxColour(from_u8(colors->values[i])) : *wxWHITE;
        const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i], false);
        info.type_raw    = preset != nullptr ? preset->config.opt_string("filament_type", 0u) : std::string("PLA");
        info.preset_name = bundle.filament_presets[i];
        info.label       = from_u8(info.type_raw);
        m_filaments.push_back(info);
    }
}

void FilamentGroupDialog::move_filament(size_t filament_idx, bool to_high_flow)
{
    if (filament_idx >= m_mapping.size())
        return;
    const FilamentVolumeType target = to_high_flow ? fvtHighFlow : fvtStandard;
    if (m_mapping[filament_idx] == target)
        return;
    m_mapping[filament_idx] = target;
    rebuild_chips();
    update_warnings();
}

void FilamentGroupDialog::swap_groups()
{
    for (const FilamentInfo &info : m_filaments)
        m_mapping[info.filament_idx] =
            m_mapping[info.filament_idx] == fvtHighFlow ? fvtStandard : fvtHighFlow;
    rebuild_chips();
    update_warnings();
}

void FilamentGroupDialog::rebuild_chips()
{
    m_std_grid->Clear(true);
    m_high_grid->Clear(true);
    for (const FilamentInfo &info : m_filaments) {
        const bool       high = m_mapping[info.filament_idx] == fvtHighFlow;
        StaticBox       *box  = high ? m_high_box : m_std_box;
        wxFlexGridSizer *grid = high ? m_high_grid : m_std_grid;
        grid->Add(new FilamentChip(box, info.filament_idx, info.color, info.label));
    }
    m_std_box->Layout();
    m_high_box->Layout();
    Layout();
    Fit();
}

void FilamentGroupDialog::update_warnings()
{
    m_warning_sizer->Clear(true);

    std::vector<HighFlowCompat::CompatibilityResult> warnings;
    bool has_unsupported = false;
    for (const FilamentInfo &info : m_filaments)
    {
        if (m_mapping[info.filament_idx] != fvtHighFlow)
            continue;

        const HighFlowCompat::CompatibilityResult result = HighFlowCompat::check(info.type_raw, info.preset_name);
        if (result.level == HighFlowCompat::CompatibilityLevel::Compatible)
            continue;

        has_unsupported |= result.level == HighFlowCompat::CompatibilityLevel::Unsupported;
        const auto duplicate = std::find_if(warnings.begin(), warnings.end(), [&result](const auto &warning) {
            return warning.level == result.level && warning.material == result.material;
        });
        if (duplicate == warnings.end())
            warnings.push_back(result);
    }

    const std::string diameter =
        wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("printer_variant");
    wxString not_recommended_materials;
    wxString unsupported_materials;
    for (const HighFlowCompat::CompatibilityResult &warning : warnings)
    {
        wxString material;
        if (warning.material == "CF or GF based filaments")
            material = _L("CF or GF based filaments");
        else
            material = from_u8(warning.material);

        wxString &materials = warning.level == HighFlowCompat::CompatibilityLevel::Unsupported ?
                                  unsupported_materials :
                                  not_recommended_materials;
        if (!materials.empty())
            materials += ", ";
        materials += material;
    }

    auto add_warning = [this](const wxString &message, bool is_error) {
        const wxColour background = StateColor::darkModeColorFor(wxColour(is_error ? "#FDE8E8" : "#FFFAF2"));
        const wxColour foreground = StateColor::darkModeColorFor(wxColour(is_error ? "#D32F2F" : "#FF8400"));
        const char *icon_name = is_error ? "error_icon_red_exclamation" : "icon_warning_triangle";

        StaticBox *bar = new StaticBox(this, wxID_ANY);
        bar->SetCornerRadius(FromDIP(4));
        bar->SetBorderWidth(0);
        bar->SetBackgroundColorNormal(background);
        bar->SetBackgroundColour(background);
        bar->SetMinSize(wxSize(-1, FromDIP(34)));

        auto *bar_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *icon = new wxStaticBitmap(bar, wxID_ANY, create_scaled_bitmap(icon_name, bar, 14));
        auto *text = new wxStaticText(bar, wxID_ANY, message);
        text->Wrap(FromDIP(660));
        icon->SetBackgroundColour(background);
        text->SetBackgroundColour(background);
        text->SetForegroundColour(foreground);
        bar_sizer->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(8));
        bar_sizer->AddSpacer(FromDIP(10));
        bar_sizer->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(8));
        bar->SetSizer(bar_sizer);
        m_warning_sizer->Add(bar, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    };

    if (!not_recommended_materials.empty())
    {
        add_warning(format_wxstr(_L("It is not recommended to print these filaments with the %1%mm high flow nozzle: %2%"),
                                 from_u8(diameter), not_recommended_materials),
                    false);
    }
    if (!unsupported_materials.empty())
    {
        add_warning(format_wxstr(_L("These filaments cannot be printed with the %1%mm high flow nozzle: %2%"),
                                 from_u8(diameter), unsupported_materials),
                    true);
    }

    m_confirm_button->Enable(!has_unsupported);
    Layout();
    Fit();
    Refresh();
}

void FilamentGroupDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
