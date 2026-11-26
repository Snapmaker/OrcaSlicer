#include "SupportFilamentDialogs.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/SupportFilamentRules.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StaticBox.hpp"

#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/panel.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r { namespace GUI {
namespace {

constexpr int GUIDE_DIALOG_WIDTH       = 580;
constexpr int GUIDE_CONTENT_WIDTH      = 539;
constexpr int GUIDE_TEXT_WIDTH         = 513;
constexpr int GUIDE_IMAGE_HEIGHT       = 206;
constexpr int GUIDE_IMAGE_HEIGHT_ZH_CN = 212;
constexpr int GUIDE_PANEL_WIDTH        = 322;
constexpr int GUIDE_PANEL_HEIGHT       = 238;
constexpr int TABLE_WIDTH        = 490;
constexpr int FIRST_COLUMN_WIDTH = 190;
constexpr int VALUE_COLUMN_WIDTH = 150;
constexpr int TABLE_ROW_HEIGHT   = 24;
constexpr int TABLE_CELL_PADDING = 6;

wxColour table_heading_background()
{
    return wxGetApp().dark_mode() ? wxColour(63, 63, 70) : wxColour(107, 107, 107);
}

wxColour table_row_background()
{
    return wxGetApp().dark_mode() ? wxColour(76, 76, 85) : wxColour(248, 248, 248);
}

bool is_zh_cn_language()
{
    return wxGetApp().current_language_code().BeforeFirst('_') == "zh";
}

wxFont guide_body_font()
{
    return is_zh_cn_language() ? Label::Body_13 : Label::Body_14;
}

wxSizer* CreateBulletRow(wxWindow* parent, const wxString& text)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    auto* bullet = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8("•"), wxDefaultPosition,
                                    wxSize(parent->FromDIP(14), -1));
    bullet->SetFont(guide_body_font());
    bullet->SetForegroundColour(wxColour("#18181B"));
    row->Add(bullet, 0, wxALIGN_TOP, 0);

    auto* label = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxSize(parent->FromDIP(GUIDE_TEXT_WIDTH), -1));
    label->SetFont(guide_body_font());
    label->SetForegroundColour(wxColour("#18181B"));
    label->Wrap(parent->FromDIP(GUIDE_TEXT_WIDTH));
    row->Add(label, 1, wxEXPAND, 0);

    return row;
}

std::string guide_image_name()
{
    return is_zh_cn_language() ? "support_filament_guide_zh_CN" : "support_filament_guide";
}

int guide_image_height()
{
    return is_zh_cn_language() ? GUIDE_IMAGE_HEIGHT_ZH_CN : GUIDE_IMAGE_HEIGHT;
}

void set_static_text_colour(wxWindow* window, const wxColour& colour)
{
    if (auto* text = dynamic_cast<wxStaticText*>(window))
        text->SetForegroundColour(colour);
    for (auto* child : window->GetChildren())
        set_static_text_colour(child, colour);
}

wxPanel* CreateTableCell(wxWindow* parent, int width, const wxString& text, bool heading)
{
    auto* cell = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(width), parent->FromDIP(TABLE_ROW_HEIGHT)),
                             wxTAB_TRAVERSAL);
    cell->SetBackgroundColour(heading ? table_heading_background() : table_row_background());

    auto* cell_sizer = new wxBoxSizer(wxVERTICAL);
    auto* text_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* label      = new wxStaticText(cell, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    label->SetFont(Label::Body_13);
    label->SetForegroundColour(heading ? *wxWHITE : wxColour(107, 107, 107));
    label->Wrap(-1);
    if (heading) {
        text_sizer->AddStretchSpacer();
        text_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, parent->FromDIP(TABLE_CELL_PADDING));
        text_sizer->AddStretchSpacer();
    } else {
        text_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, parent->FromDIP(TABLE_CELL_PADDING));
    }
    cell_sizer->Add(text_sizer, 1, wxALIGN_CENTER_VERTICAL, 0);
    cell->SetSizer(cell_sizer);
    return cell;
}

wxPanel* CreateTableRow(wxWindow* parent, const wxString& first, const wxString& second, const wxString& third, bool heading = false)
{
    auto* row = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(TABLE_WIDTH), parent->FromDIP(TABLE_ROW_HEIGHT)),
                            wxTAB_TRAVERSAL);
    row->SetBackgroundColour(heading ? table_heading_background() : table_row_background());

    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(CreateTableCell(row, FIRST_COLUMN_WIDTH, first, heading), 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer->Add(CreateTableCell(row, VALUE_COLUMN_WIDTH, second, heading), 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer->Add(CreateTableCell(row, VALUE_COLUMN_WIDTH, third, heading), 0, wxALIGN_CENTER_VERTICAL, 0);
    row->SetSizer(sizer);
    row->Layout();
    return row;
}

} // namespace

SupportParameterDiffDialog::SupportParameterDiffDialog(Mode mode, std::vector<Item> items)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                mode == Mode::ApplyRecommendation ? _L("Suggestion") : _L("Restore defaults"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    const bool recommendation = mode == Mode::ApplyRecommendation;
    SetBackgroundColour(*wxWHITE);

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    auto* top_line   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    top_line->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(top_line, 0, wxEXPAND, 0);
    main_sizer->AddSpacer(FromDIP(20));

    auto* message =
        new wxStaticText(this, wxID_ANY,
                         recommendation ?
                             _L("When a support material is used as the support interface, we recommend the following settings:") :
                             _L("You are no longer using support material. Restore these settings to the preset defaults?"),
                         wxDefaultPosition, wxSize(FromDIP(TABLE_WIDTH), -1));
    message->SetFont(Label::Body_13);
    message->SetForegroundColour(wxColour(38, 46, 48));
    message->Wrap(FromDIP(TABLE_WIDTH));
    main_sizer->Add(message, 0, wxLEFT | wxRIGHT, FromDIP(20));
    main_sizer->AddSpacer(FromDIP(12));

    auto* table_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(TABLE_WIDTH), -1), wxTAB_TRAVERSAL);
    table_panel->SetBackgroundColour(table_row_background());
    auto* table_sizer = new wxBoxSizer(wxVERTICAL);
    table_sizer->Add(CreateTableRow(table_panel, _L("Settings"), _L("Current Value"),
                                    recommendation ? _L("Recommended Value") : _L("Default Value"), true),
                     0, wxEXPAND, 0);
    for (const Item& item : items)
        table_sizer->Add(CreateTableRow(table_panel, item.name, item.current_value, item.target_value), 0, wxEXPAND, 0);
    table_panel->SetSizer(table_sizer);
    table_panel->Layout();
    main_sizer->Add(table_panel, 0, wxLEFT | wxRIGHT, FromDIP(20));
    main_sizer->AddSpacer(FromDIP(9));

    auto* button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->AddStretchSpacer();
    auto* no_button = new Button(this, _L("No"));
    no_button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    no_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_NO); });
    button_sizer->Add(no_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(ButtonProps::ChoiceButtonGap()));

    auto* yes_button = new Button(this, _L("Yes"));
    yes_button->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    yes_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_YES); });
    button_sizer->Add(yes_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(ButtonProps::ChoiceButtonGap()));
    button_sizer->Add(0, 0, 0, wxRIGHT, FromDIP(20));
    main_sizer->Add(button_sizer, 0, wxEXPAND | wxTOP, FromDIP(6));
    main_sizer->AddSpacer(FromDIP(18));

    SetSizer(main_sizer);
    Layout();
    Fit();
    Center(wxBOTH);
    Bind(wxEVT_SHOW, [yes_button](wxShowEvent& event) {
        event.Skip();
        if (event.IsShown())
            yes_button->CallAfter([yes_button]() { yes_button->SetFocus(); });
    });
    wxGetApp().UpdateDlgDarkUI(this);
}

void SupportParameterDiffDialog::on_dpi_changed(const wxRect& suggested_rect) { Fit(); }

SupportFilamentGuideDialog::SupportFilamentGuideDialog(const std::string& guide_url)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Support Material Usage Guide"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(wxGetApp().normal_font());

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* content = new wxBoxSizer(wxVERTICAL);
    root->Add(content, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    auto* subtitle = new wxStaticText(this, wxID_ANY, _L("Support Material Usage Guide"), wxDefaultPosition,
                                      wxSize(FromDIP(GUIDE_CONTENT_WIDTH), -1));
    subtitle->SetFont(guide_body_font());
    subtitle->SetForegroundColour(wxColour("#71717A"));
    content->Add(subtitle, 0, wxBOTTOM, FromDIP(12));

    content->Add(CreateBulletRow(this,
                                 _L("Using support material for the first time? "
                                    "We strongly recommend reading the official guide first to avoid unexpected print results.")),
                 0, wxLEFT, FromDIP(12));
    content->AddSpacer(FromDIP(10));
    content->Add(CreateBulletRow(this,
                                 _L("If you want to use this material for support printing, you can go to the process parameters and "
                                    "change the \"Support/Raft Interface\" parameter to the corresponding material number. "
                                    "We will recommend suitable support settings for you.")),
                 0, wxLEFT, FromDIP(12));
    content->AddSpacer(FromDIP(12));

    auto* image_panel = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(GUIDE_PANEL_WIDTH), FromDIP(GUIDE_PANEL_HEIGHT)),
                                      wxTAB_TRAVERSAL | wxBORDER_NONE);
    image_panel->SetBackgroundColorNormal(wxColour("#F7F7F7"));
    image_panel->SetCornerRadius(FromDIP(4));
    auto* image_sizer = new wxBoxSizer(wxVERTICAL);
    ScalableBitmap guide_bitmap(this, guide_image_name(), guide_image_height());
    m_guide_bitmap = new wxStaticBitmap(image_panel, wxID_ANY, guide_bitmap.bmp());
    image_sizer->Add(m_guide_bitmap, 1, wxALIGN_CENTER, 0);
    image_panel->SetSizer(image_sizer);
    content->Add(image_panel, 0, wxLEFT, FromDIP(12));

    root->AddSpacer(FromDIP(20));

    auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxBORDER_NONE);
    divider->SetBackgroundColour(wxColour("#DEDEE0"));
    root->Add(divider, 0, wxEXPAND, 0);

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    m_dont_show_checkbox = new ::CheckBox(this);
    footer->Add(m_dont_show_checkbox, 0, wxALIGN_CENTER_VERTICAL, 0);
    auto* checkbox_label = new wxStaticText(this, wxID_ANY, _L("Don't remind me again"));
    checkbox_label->SetFont(guide_body_font());
    checkbox_label->SetForegroundColour(wxColour("#18181B"));
    footer->Add(checkbox_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    footer->AddStretchSpacer();

    m_got_it_button = new Button(this, _L("Got it"));
    m_got_it_button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_got_it_button->SetMinSize(FromDIP(wxSize(is_zh_cn_language() ? 108 : 130, 32)));
    m_got_it_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    footer->Add(m_got_it_button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

    m_guide_button = new Button(this, _L("View User Guide"));
    m_guide_button->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_guide_button->SetMinSize(FromDIP(wxSize(is_zh_cn_language() ? 120 : 130, 32)));
    m_guide_button->Bind(wxEVT_BUTTON, [guide_url](wxCommandEvent&) { wxGetApp().open_browser_with_warning_dialog(from_u8(guide_url)); });
    footer->Add(m_guide_button, 0, wxALIGN_CENTER_VERTICAL, 0);

    root->AddSpacer(FromDIP(15));
    root->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    root->AddSpacer(FromDIP(14));

    SetSizer(root);
    SetMinSize(FromDIP(wxSize(GUIDE_DIALOG_WIDTH, -1)));
    Layout();
    Fit();
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        event.Skip();
        CallAfter([this]() { SetFocus(); });
    });
    Center(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
    if (wxGetApp().dark_mode())
        set_static_text_colour(this, *wxWHITE);
}

bool SupportFilamentGuideDialog::dont_show_again() const { return m_dont_show_checkbox != nullptr && m_dont_show_checkbox->GetValue(); }

void SupportFilamentGuideDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (m_dont_show_checkbox != nullptr)
        m_dont_show_checkbox->Rescale();
    if (m_got_it_button != nullptr)
        m_got_it_button->Rescale();
    if (m_guide_button != nullptr)
        m_guide_button->Rescale();
    if (m_guide_bitmap != nullptr) {
        ScalableBitmap guide_bitmap(this, guide_image_name(), guide_image_height());
        m_guide_bitmap->SetBitmap(guide_bitmap.bmp());
    }
    Fit();
}

void show_support_filament_guide_if_needed(const Preset& preset)
{
    const SupportFilamentKind kind = classify_support_filament(preset.config);
    if (kind == SupportFilamentKind::None)
        return;

    const std::string guide_url = support_filament_guide_url(kind, into_u8(wxGetApp().current_language_code()));
    if (guide_url.empty())
        return;

    const auto* vendor_opt = preset.config.option<ConfigOptionStrings>("filament_vendor");
    if (vendor_opt == nullptr || vendor_opt->empty())
        return;

    const std::string vendor = vendor_opt->get_at(0);
    if (!boost::algorithm::iequals(vendor, "Snapmaker"))
        return;

    AppConfig* app_config = wxGetApp().app_config;
    if (app_config == nullptr)
        return;

    const int choice = support_guide_choice_from_string(app_config->get("support_material_guide_choice"));
    if (support_guide_choice_hidden(choice, kind))
        return;

    SupportFilamentGuideDialog dialog(guide_url);
    dialog.ShowModal();
    if (!dialog.dont_show_again())
        return;

    const int updated_choice = choice | support_guide_choice_bit(kind);
    app_config->set("support_material_guide_choice", std::to_string(updated_choice));
    app_config->save();
}

}} // namespace Slic3r::GUI
