#include "ThemesPage.hpp"

#include <algorithm>
#include <set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <wx/clrpicker.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/filedlg.h>
#include <wx/fontenum.h>
#include <wx/log.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include "libslic3r/AppConfig.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Theme.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

static const wxColour TEXT_COLOUR(38, 46, 48);   // DESIGN_GRAY900_COLOR in Preferences.hpp
static const wxColour TITLE_COLOUR(50, 58, 61);  // DESIGN_GRAY800_COLOR
static const wxColour LINE_COLOUR(166, 169, 170); // DESIGN_GRAY400_COLOR
static const wxColour MUTED_COLOUR(144, 144, 144);
static const wxColour LINK_COLOUR(0, 150, 136);

static constexpr uintmax_t MAX_FONT_FILE  = 16 * 1024 * 1024;
static constexpr uintmax_t MAX_IMAGE_FILE = 8 * 1024 * 1024;

// The palette roles in the order and words the page shows them.
struct RoleText { const char* role; wxString label; wxString tip; };
static const std::vector<RoleText>& role_texts()
{
    static const std::vector<RoleText> r = {
        {"window_bg",        _L("Window"),           _L("Window and dialog backgrounds, inputs")},
        {"panel_bg",         _L("Panels"),           _L("Side panels, title strips, tab pages")},
        {"sidebar_bg",       _L("Sidebar"),          _L("Sidebar panels")},
        {"text",             _L("Text"),             _L("Main text and input text")},
        {"text_secondary",   _L("Secondary text"),   _L("Labels and secondary text")},
        {"text_disabled",    _L("Disabled text"),    _L("Disabled and dimmed text")},
        {"accent",           _L("Accent"),           _L("The highlight colour: confirm buttons, selected tab, checks, toggles")},
        {"accent_hover",     _L("Accent, hovered"),  _L("Accent buttons under the mouse")},
        {"accent_soft",      _L("Selection"),        _L("Selected rows and focused inputs")},
        {"accent_text",      _L("Text on accent"),   _L("Text on accent buttons and on the main tabs")},
        {"secondary_accent", _L("Second accent"),    _L("The secondary (orange) highlight")},
        {"button_bg",        _L("Buttons"),          _L("Regular buttons")},
        {"button_hover_bg",  _L("Buttons, hovered"), _L("Regular buttons under the mouse")},
        {"border",           _L("Borders"),          _L("Input and combo box borders")},
        {"separator",        _L("Lines"),            _L("Lines and dividers")},
        {"disabled_bg",      _L("Disabled controls"), _L("Backgrounds of disabled controls")},
        {"toggle_track",     _L("Switch track"),     _L("The track of on/off switches")},
        {"error",            _L("Errors"),           _L("Error text")},
        {"tabbar_bg",        _L("Tab bar"),          _L("The main tab bar: Home, Prepare, Preview, Device")},
        {"tabbar_hover",     _L("Tab bar, hovered"), _L("A main tab under the mouse")},
        {"titlebar_bg",      _L("Title bar"),        _L("The title bar behind the banner")},
        {"titlebar_text",    _L("Title bar text"),   _L("The title and menus in the title bar")},
        {"canvas_bg",        _L("3D view"),          _L("The 3D view background")},
        {"canvas_bg_top",    _L("3D view, top"),     _L("Makes the 3D view a gradient up to this colour")},
        {"icon",             _L("Icons"),            _L("The main line colour of the built-in icons")},
    };
    return r;
}

static wxString face_name(const ThemePack::Font& font) { return from_u8(font.face); }

static bool same_font(const ThemePack::Font& a, const ThemePack::Font& b) { return a.face == b.face && a.files == b.files; }

// A section heading with a rule after it, as on the other Preferences pages.
static wxSizer* section_title(wxWindow* parent, const wxString& title)
{
    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    auto text  = new wxStaticText(parent, wxID_ANY, title);
    text->SetForegroundColour(TITLE_COLOUR);
    text->SetFont(::Label::Head_13);
    auto line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(LINE_COLOUR);
    sizer->Add(text, 0, wxALIGN_CENTER | wxALL, 3);
    sizer->Add(0, 0, 0, wxLEFT, 9);
    sizer->Add(line, 1, wxALIGN_CENTER_VERTICAL, 0);
    return sizer;
}

static wxStaticText* body_text(wxWindow* parent, const wxString& text, int width = -1)
{
    auto t = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxSize(width, -1));
    t->SetForegroundColour(TEXT_COLOUR);
    t->SetFont(::Label::Body_13);
    return t;
}

static ComboBox* combo(wxWindow* parent, int width, const std::vector<wxString>& items)
{
    auto c = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(width, -1), 0, nullptr, wxCB_READONLY);
    c->SetFont(::Label::Body_13);
    c->GetDropDown().SetFont(::Label::Body_13);
    for (const auto& item : items)
        c->Append(item);
    return c;
}

static Button* button(wxWindow* parent, const wxString& label, const wxString& tip = {})
{
    auto b = new Button(parent, label);
    b->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    if (!tip.empty())
        b->SetToolTip(tip);
    return b;
}

// ------------------------------------------------------------------------------- preview ----

// A small picture of the window in the theme being edited: title bar and banner, main tabs, a
// sidebar with an input and a switch, the 3D view and two buttons. Drawn, not built from real
// controls, so it shows the edit before a restart.
class ThemePreview : public wxWindow
{
public:
    ThemePreview(wxWindow* parent, ThemesPage* page)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE), m_page(page)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(420), FromDIP(214)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

private:
    wxFont font(const ThemePack::Font& themed, int size, bool bold) const
    {
        wxFont f = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        f.SetPointSize(size);
        f.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        if (!themed.empty()) {
            wxFont t = f;
            if (t.SetFaceName(face_name(themed)) && t.IsOk())
                return t;
        }
        return f;
    }

    // Stock buttons are pills; stock boxes have small corners.
    double radius(int themed, int height, bool button) const
    {
        if (themed >= 0)
            return std::min<double>(FromDIP(themed), height / 2.0);
        return button ? height / 2.0 : FromDIP(4);
    }

    void paint()
    {
        wxAutoBufferedPaintDC paint_dc(this);
        wxGCDC                dc(paint_dc);
        const auto&           spec = m_page->edited();
        auto                  c    = [this](const char* role) { return m_page->role_colour(role); };
        const wxSize          size = GetClientSize();
        const int             W = size.x, H = size.y;

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c("window_bg")));
        dc.DrawRectangle(0, 0, W, H);

        // Title bar and banner.
        const int title_h = FromDIP(28);
        dc.SetBrush(wxBrush(c("titlebar_bg")));
        dc.DrawRectangle(0, 0, W, title_h);
        const wxImage& banner = m_page->banner_image();
        if (banner.IsOk() && banner.GetHeight() > 0) {
            const std::string& align = spec.banner_align;
            wxImage img = align == "stretch" ? banner.Scale(W, title_h, wxIMAGE_QUALITY_HIGH)
                                             : banner.Scale(std::max(1, banner.GetWidth() * title_h / banner.GetHeight()), title_h, wxIMAGE_QUALITY_HIGH);
            const wxBitmap bmp(img);
            dc.SetClippingRegion(0, 0, W, title_h);
            if (align == "tile") {
                for (int x = 0; x < W; x += std::max(1, bmp.GetWidth()))
                    dc.DrawBitmap(bmp, x, 0, true);
            } else {
                const int x = align == "center" ? (W - bmp.GetWidth()) / 2 : align == "right" ? W - bmp.GetWidth() : 0;
                dc.DrawBitmap(bmp, x, 0, true);
            }
            dc.DestroyClippingRegion();
        }
        dc.SetFont(font({}, 9, false));
        dc.SetTextForeground(c("titlebar_text"));
        const wxString title = "EdgeSlicer";
        const wxSize   tsz   = dc.GetTextExtent(title);
        dc.DrawText(title, (W - tsz.x) / 2, (title_h - tsz.y) / 2);
        dc.SetPen(wxPen(c("titlebar_text"), 1));
        const int g = FromDIP(8), gy = title_h / 2;
        for (int i = 0; i < 3; ++i) {
            const int x = W - FromDIP(18) - i * FromDIP(26);
            if (i == 0) {
                dc.DrawLine(x - g / 2, gy - g / 2, x + g / 2, gy + g / 2);
                dc.DrawLine(x - g / 2, gy + g / 2, x + g / 2, gy - g / 2);
            } else if (i == 1) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(x - g / 2, gy - g / 2, g, g);
            } else
                dc.DrawLine(x - g / 2, gy, x + g / 2, gy);
        }

        // Main tabs.
        const int tabs_y = title_h, tabs_h = FromDIP(30);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c("tabbar_bg")));
        dc.DrawRectangle(0, tabs_y, W, tabs_h);
        dc.SetFont(font(spec.button, 9, false));
        int x = FromDIP(10);
        const wxString tabs[] = {_L("Home"), _L("Prepare"), _L("Preview"), _L("Device")};
        for (int i = 0; i < 4; ++i) {
            const wxSize sz = dc.GetTextExtent(tabs[i]);
            const int    w  = sz.x + FromDIP(20), h = tabs_h - FromDIP(8), y = tabs_y + FromDIP(4);
            if (i == 1 || i == 2) {
                dc.SetBrush(wxBrush(c(i == 1 ? "accent" : "tabbar_hover")));
                dc.DrawRoundedRectangle(x, y, w, h, radius(spec.button_radius, h, true));
            }
            dc.SetTextForeground(c("accent_text"));
            dc.DrawText(tabs[i], x + FromDIP(10), y + (h - sz.y) / 2);
            x += w + FromDIP(6);
        }

        // Sidebar.
        const int body_y = tabs_y + tabs_h, side_w = W * 42 / 100;
        dc.SetBrush(wxBrush(c("panel_bg")));
        dc.DrawRectangle(0, body_y, side_w, H - body_y);
        const int pad = FromDIP(10);
        int       y   = body_y + pad;
        dc.SetPen(wxPen(c("icon"), std::max(1, FromDIP(1))));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(pad, y + FromDIP(2), FromDIP(11), FromDIP(11));
        dc.DrawLine(pad + FromDIP(3), y + FromDIP(7), pad + FromDIP(8), y + FromDIP(7));
        dc.SetFont(font(spec.heading, 10, true));
        dc.SetTextForeground(c("text"));
        dc.DrawText(_L("Process"), pad + FromDIP(17), y);
        y += dc.GetTextExtent("Pg").y + FromDIP(6);
        dc.SetPen(wxPen(c("separator"), 1));
        dc.DrawLine(pad, y, side_w - pad, y);
        y += FromDIP(8);

        dc.SetFont(font(spec.body, 9, false));
        const int line_h = dc.GetTextExtent("Pg").y;
        dc.SetTextForeground(c("text_secondary"));
        dc.DrawText(_L("Layer height"), pad, y + FromDIP(4));
        const int box_x = side_w - pad - FromDIP(70), box_h = line_h + FromDIP(8);
        dc.SetPen(wxPen(c("border"), 1));
        dc.SetBrush(wxBrush(c("window_bg")));
        dc.DrawRoundedRectangle(box_x, y, FromDIP(70), box_h, radius(spec.box_radius, box_h, false));
        dc.SetTextForeground(c("text"));
        dc.DrawText("0.2 mm", box_x + FromDIP(8), y + FromDIP(4));
        y += box_h + FromDIP(8);

        dc.SetTextForeground(c("text_secondary"));
        dc.DrawText(_L("Supports"), pad, y + FromDIP(2));
        const int sw_w = FromDIP(30), sw_h = FromDIP(16), sw_x = side_w - pad - sw_w;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(c("accent")));
        dc.DrawRoundedRectangle(sw_x, y, sw_w, sw_h, sw_h / 2.0);
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawCircle(sw_x + sw_w - sw_h / 2, y + sw_h / 2, sw_h / 2 - FromDIP(2));
        y += sw_h + FromDIP(8);

        dc.SetTextForeground(c("text_secondary"));
        dc.DrawText(_L("Brim"), pad, y + FromDIP(2));
        dc.SetBrush(wxBrush(c("toggle_track")));
        dc.DrawRoundedRectangle(sw_x, y, sw_w, sw_h, sw_h / 2.0);
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawCircle(sw_x + sw_h / 2, y + sw_h / 2, sw_h / 2 - FromDIP(2));
        y += sw_h + FromDIP(8);

        dc.SetTextForeground(c("text_disabled"));
        dc.DrawText(_L("Advanced settings"), pad, y);

        // 3D view: one colour, or a gradient up to canvas_bg_top.
        const wxRect canvas(side_w, body_y, W - side_w, H - body_y);
        dc.GradientFillLinear(canvas, c("canvas_bg"), c("canvas_bg_top"), wxNORTH);
        const wxColour bg = c("canvas_bg");
        const wxColour plate(bg.Red() * 85 / 100, bg.Green() * 85 / 100, bg.Blue() * 85 / 100);
        const int      px = canvas.x + canvas.width / 2, py = canvas.y + canvas.height * 45 / 100;
        const int      pw = canvas.width * 32 / 100, ph = canvas.height / 5;
        wxPoint        plate_pts[] = {{px - pw, py + ph}, {px + pw, py + ph}, {px + pw * 7 / 10, py - ph / 2}, {px - pw * 7 / 10, py - ph / 2}};
        dc.SetBrush(wxBrush(plate));
        dc.DrawPolygon(4, plate_pts);

        // Buttons: a regular one and a confirm one.
        dc.SetFont(font(spec.button, 9, false));
        const int      btn_h = line_h + FromDIP(10);
        int            bx    = W - pad;
        const int      by    = H - pad - btn_h;
        const wxString labels[] = {_L("Slice plate"), _L("Export")};
        for (int i = 0; i < 2; ++i) {
            const wxSize sz = dc.GetTextExtent(labels[i]);
            const int    w  = sz.x + FromDIP(24);
            bx -= w;
            dc.SetPen(i == 0 ? *wxTRANSPARENT_PEN : wxPen(c("border"), 1));
            dc.SetBrush(wxBrush(c(i == 0 ? "accent" : "button_bg")));
            dc.DrawRoundedRectangle(bx, by, w, btn_h, radius(spec.button_radius, btn_h, true));
            dc.SetTextForeground(c(i == 0 ? "accent_text" : "text"));
            dc.DrawText(labels[i], bx + FromDIP(12), by + (btn_h - sz.y) / 2);
            bx -= FromDIP(8);
        }

        dc.SetPen(wxPen(LINE_COLOUR, 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, W, H);
    }

    ThemesPage* m_page;
};

// --------------------------------------------------------------------------------- page ----

ThemesPage::ThemesPage(wxWindow* parent) : wxWindow(parent, wxID_ANY)
{
    SetBackgroundColour(*wxWHITE);

    wxArrayString faces = wxFontEnumerator::GetFacenames();
    std::set<wxString> seen;
    for (const auto& f : faces)
        if (!f.empty() && f[0] != '@' && seen.insert(f).second) // '@' faces are Windows' vertical variants
            m_faces.push_back(f);
    std::sort(m_faces.begin(), m_faces.end(), [](const wxString& a, const wxString& b) { return a.CmpNoCase(b) < 0; });

    auto sizer = new wxBoxSizer(wxVERTICAL);
    build_picker(sizer);
    m_preview = new ThemePreview(this, this);
    sizer->Add(m_preview, 0, wxEXPAND | wxLEFT | wxTOP, FromDIP(23) / 2);
    build_details(sizer);
    build_colours(sizer);
    build_fonts(sizer);
    build_shapes(sizer);
    build_titlebar(sizer);
    build_actions(sizer);
    SetSizer(sizer);

    fill_list();
    load(wxGetApp().app_config->get("ui_theme"));
    sizer->Fit(this);
}

wxWindow* ThemesPage::build_picker(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Theme")), 0, wxEXPAND | wxTOP, FromDIP(20));

    auto row = new wxBoxSizer(wxHORIZONTAL);
    m_list   = combo(this, FromDIP(200), {});
    m_list->SetToolTip(_L("The theme EdgeSlicer starts with. Default is the stock look and is always here to go back to."));
    m_list->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
        on_pick(e.GetSelection());
        e.Skip();
    });
    auto install = button(this, _L("Install..."), _L("Add a theme from a .zip file."));
    install->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_install(); });
    auto folder = button(this, _L("Open folder"), _L("The folder installed themes live in. A theme folder can be copied in by hand."));
    folder->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        boost::system::error_code ec;
        fs::create_directories(Theme::user_dir(), ec);
        desktop_open_any_folder(Theme::user_dir().string());
    });
    row->Add(0, 0, 0, wxLEFT, FromDIP(23));
    row->Add(m_list, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(install, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    row->Add(folder, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(row, 0, wxTOP, FromDIP(6));

    auto row2 = new wxBoxSizer(wxHORIZONTAL);
    m_default = button(this, _L("Back to Default"), _L("Start with the clean stock look again."));
    m_default->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_pick(0); });
    m_delete = button(this, _L("Delete"), _L("Delete this installed theme."));
    m_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_delete(); });
    row2->Add(0, 0, 0, wxLEFT, FromDIP(23));
    row2->Add(m_default, 0, wxALIGN_CENTER_VERTICAL);
    row2->Add(m_delete, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(row2, 0, wxTOP, FromDIP(6));

    m_note = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_note->SetForegroundColour(MUTED_COLOUR);
    m_note->SetFont(::Label::Body_12);
    sizer->Add(m_note, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
    return this;
}

void ThemesPage::build_details(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Name and look")), 0, wxEXPAND | wxTOP, FromDIP(20));
    auto grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));

    auto text_input = [this](TextInput*& input, std::string ThemePack::Spec::*member) {
        input = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(200), -1));
        input->GetTextCtrl()->Bind(wxEVT_TEXT, [this, input, member](wxCommandEvent& e) {
            if (!m_filling) {
                m_spec.*member = into_u8(input->GetTextCtrl()->GetValue());
                changed();
            }
            e.Skip();
        });
    };
    text_input(m_name, &ThemePack::Spec::name);
    text_input(m_author, &ThemePack::Spec::author);

    m_base = combo(this, FromDIP(200), {_L("Follow dark mode setting"), _L("Light"), _L("Dark")});
    m_base->SetToolTip(_L("The look the theme is laid over. Colours the theme leaves alone come from it. On macOS the system appearance decides."));
    m_base->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
        if (!m_filling) {
            const int sel = e.GetSelection();
            m_spec.base   = sel == 1 ? "light" : sel == 2 ? "dark" : "";
            changed();
        }
        e.Skip();
    });

    grid->Add(body_text(this, _L("Name")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_name);
    grid->Add(body_text(this, _L("Author")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_author);
    grid->Add(body_text(this, _L("Built on")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_base);
    sizer->Add(grid, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
}

void ThemesPage::build_colours(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Colours")), 0, wxEXPAND | wxTOP, FromDIP(20));
    auto hint = new wxStaticText(this, wxID_ANY, _L("Colours marked stock follow the light or dark look. Pick one to change it."));
    hint->SetForegroundColour(MUTED_COLOUR);
    hint->SetFont(::Label::Body_12);
    sizer->Add(hint, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));

    // Two columns of label, colour, stock/reset.
    auto grid = new wxFlexGridSizer(6, FromDIP(4), FromDIP(6));
    m_colours.reserve(role_texts().size());
    for (const auto& rt : role_texts()) {
        m_colours.push_back({rt.role});
        ColourRow& row = m_colours.back();
        row.label      = body_text(this, rt.label, FromDIP(112));
        row.label->SetToolTip(rt.tip);
        row.picker = new wxColourPickerCtrl(this, wxID_ANY, *wxWHITE, wxDefaultPosition, wxSize(FromDIP(44), FromDIP(24)));
        row.picker->SetToolTip(rt.tip);
        row.state = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(40), -1));
        row.state->SetFont(::Label::Body_12);
        const size_t index = m_colours.size() - 1;
        row.picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, index](wxColourPickerEvent& e) {
            ColourRow& r = m_colours[index];
            m_spec.palette[r.role] = into_u8(e.GetColour().GetAsString(wxC2S_HTML_SYNTAX)).substr(0, 7);
            update_colour_row(r);
            changed();
        });
        row.state->Bind(wxEVT_LEFT_UP, [this, index](wxMouseEvent&) {
            ColourRow& r = m_colours[index];
            if (m_spec.palette.erase(r.role) > 0) {
                update_colour_row(r);
                changed();
            }
        });
        grid->Add(row.label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row.picker, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row.state, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
    }
    sizer->Add(grid, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
}

void ThemesPage::build_fonts(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Fonts")), 0, wxEXPAND | wxTOP, FromDIP(20));
    auto grid = new wxFlexGridSizer(3, FromDIP(6), FromDIP(8));
    struct SlotText { ThemePack::Font ThemePack::Spec::*member; wxString label; wxString tip; };
    const SlotText slots[] = {
        {&ThemePack::Spec::body, _L("Text"), _L("All regular text. Wide display fonts can crowd fixed-size controls; they suit headings and buttons better.")},
        {&ThemePack::Spec::heading, _L("Headings"), _L("All bold titles.")},
        {&ThemePack::Spec::button, _L("Buttons"), _L("Every button label, the main tabs included.")},
    };
    m_fonts.reserve(3);
    for (const auto& s : slots) {
        m_fonts.push_back({s.member});
        const size_t index = m_fonts.size() - 1;
        FontSlot&    slot  = m_fonts.back();
        slot.combo         = combo(this, FromDIP(200), {});
        slot.combo->SetToolTip(s.tip);
        slot.combo->GetDropDown().Bind(wxEVT_COMBOBOX, [this, index](wxCommandEvent& e) {
            FontSlot& sl  = m_fonts[index];
            const int sel = e.GetSelection();
            if (!m_filling && sel >= 0 && size_t(sel) < sl.options.size()) {
                m_spec.*(sl.member) = sl.options[sel];
                changed();
            }
            e.Skip();
        });
        auto file = button(this, _L("From file..."), _L("Use a .ttf or .otf file. It is copied into the theme."));
        file->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { on_font_file(m_fonts[index]); });
        auto label = body_text(this, s.label, FromDIP(80));
        label->SetToolTip(s.tip);
        grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(slot.combo, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(file, 0, wxALIGN_CENTER_VERTICAL);
    }
    sizer->Add(grid, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
}

void ThemesPage::build_shapes(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Corners")), 0, wxEXPAND | wxTOP, FromDIP(20));
    std::vector<wxString> radii = {_L("Stock"), _L("0 (square)")};
    for (int i = 1; i <= 24; ++i)
        radii.push_back(wxString::Format("%d", i));
    auto grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8));
    auto add  = [&](ComboBox*& c, int ThemePack::Spec::*member, const wxString& label, const wxString& tip) {
        c = combo(this, FromDIP(120), radii);
        c->SetToolTip(tip);
        c->GetDropDown().Bind(wxEVT_COMBOBOX, [this, member](wxCommandEvent& e) {
            if (!m_filling) {
                m_spec.*member = e.GetSelection() - 1; // "Stock" is -1
                changed();
            }
            e.Skip();
        });
        auto t = body_text(this, label, FromDIP(80));
        t->SetToolTip(tip);
        grid->Add(t, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(c, 0, wxALIGN_CENTER_VERTICAL);
    };
    add(m_button_radius, &ThemePack::Spec::button_radius, _L("Buttons"),
        _L("Corner radius of buttons. Round controls stay round; pill-shaped buttons take these corners."));
    add(m_box_radius, &ThemePack::Spec::box_radius, _L("Boxes"), _L("Corner radius of inputs, combo boxes and cards."));
    sizer->Add(grid, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
}

void ThemesPage::build_titlebar(wxSizer* sizer)
{
    sizer->Add(section_title(this, _L("Title bar banner")), 0, wxEXPAND | wxTOP, FromDIP(20));
    auto row = new wxBoxSizer(wxHORIZONTAL);
    m_banner_name = body_text(this, wxEmptyString, FromDIP(160));
    auto choose   = button(this, _L("Choose image..."), _L("A PNG, JPG or BMP drawn behind the title bar, scaled to its height. It is copied into the theme."));
    choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_banner_file(); });
    m_banner_clear = button(this, _L("Remove"));
    m_banner_clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_spec.banner.clear();
        m_banner_name->SetLabel(_L("None"));
        changed();
    });
    row->Add(0, 0, 0, wxLEFT, FromDIP(23) / 2 + FromDIP(3));
    row->Add(m_banner_name, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(choose, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    row->Add(m_banner_clear, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(row, 0, wxTOP, FromDIP(6));

    auto row2 = new wxBoxSizer(wxHORIZONTAL);
    m_banner_align = combo(this, FromDIP(160), {_L("Left"), _L("Center"), _L("Right"), _L("Repeat across"), _L("Stretch to fill")});
    m_banner_align->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
        static const char* aligns[] = {"left", "center", "right", "tile", "stretch"};
        const int sel = e.GetSelection();
        if (!m_filling && sel >= 0 && sel < 5) {
            m_spec.banner_align = aligns[sel];
            changed();
        }
        e.Skip();
    });
    row2->Add(0, 0, 0, wxLEFT, FromDIP(23) / 2 + FromDIP(3));
    row2->Add(body_text(this, _L("Placement"), FromDIP(160)), 0, wxALIGN_CENTER_VERTICAL);
    row2->Add(m_banner_align, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    sizer->Add(row2, 0, wxTOP, FromDIP(6));

    auto hint = new wxStaticText(this, wxID_ANY, _L("Windows and Linux. Keep it low in contrast so the title and buttons stay readable."));
    hint->SetForegroundColour(MUTED_COLOUR);
    hint->SetFont(::Label::Body_12);
    sizer->Add(hint, 0, wxLEFT | wxTOP, FromDIP(23) / 2 + FromDIP(3));
}

void ThemesPage::build_actions(wxSizer* sizer)
{
    sizer->Add(section_title(this, wxEmptyString), 0, wxEXPAND | wxTOP, FromDIP(16));
    auto row  = new wxBoxSizer(wxHORIZONTAL);
    m_save    = new Button(this, _L("Save"));
    m_save->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save(!editable()); });
    m_save_as = button(this, _L("Save as new..."), _L("Save these settings as a new theme of your own."));
    m_save_as->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save(true); });
    m_discard = button(this, _L("Discard changes"));
    m_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load(m_id); });
    row->Add(0, 0, 0, wxLEFT, FromDIP(23) / 2 + FromDIP(3));
    row->Add(m_save, 0, wxALIGN_CENTER_VERTICAL);
    row->Add(m_save_as, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    row->Add(m_discard, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(row, 0, wxTOP, FromDIP(6));
}

// ------------------------------------------------------------------------------ state ----

void ThemesPage::fill_list()
{
    m_filling = true;
    m_list->Clear();
    m_ids.assign(1, std::string());
    m_list->Append(_L("Default (clean)"));
    int current = 0;
    for (const auto& t : Theme::available()) {
        m_ids.push_back(t.id);
        m_list->Append(from_u8(t.name) + (t.builtin ? wxString() : " (" + _L("yours") + ")"));
        if (t.id == m_id)
            current = int(m_ids.size()) - 1;
    }
    m_list->SetSelection(current);
    m_filling = false;
}

void ThemesPage::load(const std::string& id)
{
    m_id        = id;
    m_installed = false;
    m_dir.clear();
    m_spec = ThemePack::Spec{};
    if (!id.empty()) {
        bool        builtin = false;
        std::string error;
        if (Theme::read(id, m_spec, m_dir, builtin, error))
            m_installed = !builtin;
        else {
            MessageDialog(this, _L("The theme could not be read:") + "\n" + from_u8(error), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
            m_id.clear();
            m_dir.clear();
            m_spec = ThemePack::Spec{};
        }
    }
    m_spec.warnings.clear();
    m_imports.clear();
    m_dirty = false;
    m_banner_key.clear();
    m_banner = wxImage();

    // Point the list at it.
    const auto it = std::find(m_ids.begin(), m_ids.end(), m_id);
    m_filling     = true;
    m_list->SetSelection(it == m_ids.end() ? 0 : int(it - m_ids.begin()));
    m_filling = false;
    show_spec();
}

void ThemesPage::show_spec()
{
    m_filling = true;
    m_name->GetTextCtrl()->ChangeValue(from_u8(m_spec.name));
    m_author->GetTextCtrl()->ChangeValue(from_u8(m_spec.author));
    m_base->SetSelection(m_spec.base == "light" ? 1 : m_spec.base == "dark" ? 2 : 0);
    for (auto& row : m_colours)
        update_colour_row(row);
    for (auto& slot : m_fonts)
        fill_font_slot(slot, m_spec.*(slot.member));
    m_button_radius->SetSelection(m_spec.button_radius + 1);
    m_box_radius->SetSelection(m_spec.box_radius + 1);
    m_banner_name->SetLabel(m_spec.banner.empty() ? _L("None") : from_u8(fs::path(m_spec.banner).filename().string()));
    static const std::vector<std::string> aligns = {"left", "center", "right", "tile", "stretch"};
    const auto a = std::find(aligns.begin(), aligns.end(), m_spec.banner_align);
    m_banner_align->SetSelection(a == aligns.end() ? 0 : int(a - aligns.begin()));
    m_filling = false;
    update_state();
}

void ThemesPage::fill_font_slot(FontSlot& slot, const ThemePack::Font& current)
{
    const bool was_filling = m_filling;
    m_filling              = true;
    slot.combo->Clear();
    slot.options.clear();
    int selected = 0;
    slot.options.push_back({});
    slot.combo->Append(_L("Stock"));
    if (!current.empty() && !current.files.empty()) {
        // A face that comes with the theme's own files.
        slot.options.push_back(current);
        slot.combo->Append(face_name(current) + " (" + _L("in theme") + ")");
        selected = 1;
    } else if (!current.empty() && std::find(m_faces.begin(), m_faces.end(), face_name(current)) == m_faces.end()) {
        slot.options.push_back(current);
        slot.combo->Append(face_name(current) + " (" + _L("not installed") + ")");
        selected = 1;
    }
    for (const auto& face : m_faces) {
        ThemePack::Font f;
        f.face = into_u8(face);
        slot.options.push_back(f);
        slot.combo->Append(face);
        if (selected == 0 && same_font(f, current))
            selected = int(slot.options.size()) - 1;
    }
    slot.combo->SetSelection(selected);
    m_filling = was_filling;
}

wxColour ThemesPage::role_colour(const std::string& role) const
{
    if (auto it = m_spec.palette.find(role); it != m_spec.palette.end())
        return wxColour(from_u8(it->second));
    const wxColour stock(from_u8(ThemePack::stock_colour(role)));
    const bool dark = m_spec.base == "dark" || (m_spec.base.empty() && wxGetApp().app_config->get("dark_color_mode") == "1");
    if (!dark)
        return stock;
    if (role == "canvas_bg" || role == "canvas_bg_top")
        return wxColour(84, 84, 90); // DEFAULT_BG_LIGHT_COLOR_DARK in GLCanvas3D.cpp
    if (role == "titlebar_bg" || role == "titlebar_text")
        return stock;
    const auto& dark_map = StateColor::GetDarkMap();
    const auto  it       = dark_map.find(stock);
    return it == dark_map.end() ? stock : it->second;
}

const wxImage& ThemesPage::banner_image()
{
    fs::path path;
    if (!m_spec.banner.empty()) {
        if (auto it = m_imports.find(m_spec.banner); it != m_imports.end())
            path = it->second;
        else if (!m_dir.empty() && ThemePack::safe_relative_path(m_spec.banner))
            path = m_dir / fs::path(m_spec.banner).make_preferred();
    }
    if (path.string() == m_banner_key)
        return m_banner;
    m_banner_key = path.string();
    m_banner     = wxImage();
    boost::system::error_code ec;
    if (!path.empty() && fs::is_regular_file(path, ec) && fs::file_size(path, ec) <= MAX_IMAGE_FILE) {
        wxLogNull no_popups; // a bad file just shows no banner
        m_banner.LoadFile(from_path(path));
        if (m_banner.IsOk() && (m_banner.GetWidth() > 8192 || m_banner.GetHeight() > 1024))
            m_banner = wxImage();
    }
    return m_banner;
}

void ThemesPage::update_colour_row(ColourRow& row)
{
    const bool themed = m_spec.palette.count(row.role) > 0;
    row.picker->SetColour(role_colour(row.role));
    row.state->SetLabel(themed ? _L("reset") : _L("stock"));
    row.state->SetForegroundColour(themed ? LINK_COLOUR : MUTED_COLOUR);
    row.state->SetCursor(themed ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    row.state->SetToolTip(themed ? _L("Go back to the stock colour") : wxString());
    row.label->SetFont(themed ? ::Label::Head_13 : ::Label::Body_13);
}

void ThemesPage::update_state()
{
    // Stock colours depend on the base, so refresh the ones the theme leaves alone.
    for (auto& row : m_colours)
        if (m_spec.palette.count(row.role) == 0)
            row.picker->SetColour(role_colour(row.role));

    m_delete->Enable(editable());
    m_default->Enable(!m_id.empty());
    m_save->SetLabel(editable() ? _L("Save") : _L("Save as new..."));
    m_save->Enable(m_dirty);
    m_save_as->Show(editable());
    m_discard->Enable(m_dirty);

    wxString note;
    const std::string running = Theme::active_id();
    if (m_id != running)
        note = _L("Restart EdgeSlicer to switch to this theme.");
    else if (m_saved_running == m_id && !m_id.empty())
        note = _L("Saved. Restart EdgeSlicer to see the changes.");
    else
        note = _L("In use now.");
    if (m_id.empty())
        note += " " + _L("Default is the stock look: change anything below and save it as a theme of your own.");
    else if (!m_installed)
        note += " " + _L("This theme comes with EdgeSlicer; saving a change makes your own copy.");
    m_note->SetLabel(note);
    m_note->Wrap(FromDIP(460));

    Layout();
    if (wxWindow* parent = GetParent()) {
        parent->Layout();
        parent->FitInside();
    }
    m_preview->Refresh();
}

void ThemesPage::changed()
{
    if (m_filling)
        return;
    m_dirty = true;
    update_state();
}

bool ThemesPage::confirm_discard()
{
    if (!m_dirty)
        return true;
    MessageDialog ask(this, _L("Your changes to this theme are not saved. Discard them?"), _L("Theme"), wxYES_NO | wxICON_QUESTION);
    return ask.ShowModal() == wxID_YES;
}

// ---------------------------------------------------------------------------- actions ----

void ThemesPage::on_pick(int selection)
{
    if (m_filling || selection < 0 || size_t(selection) >= m_ids.size())
        return;
    const std::string id = m_ids[selection];
    if (id == m_id && !m_dirty)
        return;
    if (!confirm_discard()) {
        // Keep editing: point the list back at the theme being edited.
        const auto it = std::find(m_ids.begin(), m_ids.end(), m_id);
        m_filling     = true;
        m_list->SetSelection(it == m_ids.end() ? 0 : int(it - m_ids.begin()));
        m_filling = false;
        return;
    }
    // m_dirty is known to be discarded here, so reload even the same id.
    m_dirty = false;
    wxGetApp().app_config->set("ui_theme", id);
    wxGetApp().app_config->save();
    load(id);
}

void ThemesPage::on_install()
{
    if (!confirm_discard())
        return;
    wxFileDialog dialog(this, _L("Choose a theme"), wxEmptyString, wxEmptyString, _L("Theme (*.zip)") + "|*.zip;*.ZIP",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    bool        exists = false;
    std::string error;
    const auto  source = into_path(dialog.GetPath());
    std::string id     = Theme::install(source, false, exists, error);
    if (exists) {
        MessageDialog ask(this, wxString::Format(_L("A theme named \"%s\" is already installed. Replace it?"), from_u8(id)), _L("Theme"),
                          wxYES_NO | wxICON_QUESTION);
        id = ask.ShowModal() == wxID_YES ? Theme::install(source, true, exists, error) : std::string();
        if (id.empty() && error.empty())
            return;
    }
    if (id.empty()) {
        MessageDialog(this, _L("The theme could not be installed:") + "\n" + from_u8(error), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    wxGetApp().app_config->set("ui_theme", id);
    wxGetApp().app_config->save();
    m_id = id;
    fill_list();
    load(id);
}

void ThemesPage::on_delete()
{
    if (!editable())
        return;
    MessageDialog ask(this, wxString::Format(_L("Delete the theme \"%s\"? This cannot be undone."), from_u8(m_spec.name)), _L("Theme"),
                      wxYES_NO | wxICON_WARNING);
    if (ask.ShowModal() != wxID_YES)
        return;
    std::string error;
    if (!Theme::remove(m_id, error)) {
        MessageDialog(this, _L("The theme could not be deleted:") + "\n" + from_u8(error), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    // A shipped theme with the same folder name shows again; otherwise go back to Default.
    fs::path    dir;
    bool        builtin = false;
    std::string next    = Theme::locate(m_id, dir, builtin) ? m_id : std::string();
    if (wxGetApp().app_config->get("ui_theme") == m_id) {
        wxGetApp().app_config->set("ui_theme", next);
        wxGetApp().app_config->save();
    }
    m_id = next;
    fill_list();
    load(next);
}

std::string ThemesPage::import_path(const std::string& folder, const fs::path& file) const
{
    const std::string ext  = boost::algorithm::to_lower_copy(file.extension().string());
    std::string       stem = ThemePack::id_from_name(file.stem().string());
    return folder + "/" + stem + ext;
}

void ThemesPage::on_font_file(FontSlot& slot)
{
    wxFileDialog dialog(this, _L("Choose a font"), wxEmptyString, wxEmptyString,
                        _L("Fonts (*.ttf, *.otf)") + "|*.ttf;*.otf;*.TTF;*.OTF", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const fs::path file = into_path(dialog.GetPath());
    boost::system::error_code ec;
    std::string family;
    if (fs::file_size(file, ec) <= MAX_FONT_FILE && !ec) {
        boost::nowide::ifstream f(file.string().c_str(), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        family = ThemePack::font_family(data);
    }
    if (family.empty()) {
        MessageDialog(this, _L("That file could not be read as a font (a .ttf or .otf of at most 16 MB)."), _L("Theme"),
                      wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    const std::string rel = import_path("fonts", file);
    m_imports[rel]        = file;
    // So the preview can show it now; at startup the theme loads it again.
    wxFont::AddPrivateFont(from_path(file));

    // A second file of the same family (its bold, say) joins the first.
    ThemePack::Font& font = m_spec.*(slot.member);
    if (font.face == family) {
        if (std::find(font.files.begin(), font.files.end(), rel) == font.files.end())
            font.files.push_back(rel);
    } else
        font = {{rel}, family};
    fill_font_slot(slot, font);
    changed();
}

void ThemesPage::on_banner_file()
{
    wxFileDialog dialog(this, _L("Choose a banner image"), wxEmptyString, wxEmptyString,
                        _L("Images (*.png, *.jpg, *.bmp)") + "|*.png;*.jpg;*.jpeg;*.bmp;*.PNG;*.JPG;*.JPEG;*.BMP",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    const fs::path file = into_path(dialog.GetPath());
    boost::system::error_code ec;
    wxImage image;
    bool    ok = fs::file_size(file, ec) <= MAX_IMAGE_FILE && !ec;
    if (ok) {
        wxLogNull no_popups;
        ok = image.LoadFile(dialog.GetPath()) && image.IsOk() && image.GetWidth() <= 8192 && image.GetHeight() <= 1024;
    }
    if (!ok) {
        MessageDialog(this, _L("That image could not be used. A banner is a PNG, JPG or BMP of at most 8 MB and 8192 by 1024 pixels."),
                      _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
        return;
    }
    const std::string rel = import_path("images", file);
    m_imports[rel]        = file;
    m_spec.banner         = rel;
    m_banner_key.clear();
    m_banner_name->SetLabel(from_path(file.filename()));
    changed();
}

bool ThemesPage::save(bool as_new)
{
    ThemePack::Spec spec = m_spec;
    std::string     id   = m_id;
    if (as_new || !editable()) {
        const wxString suggested = m_id.empty()   ? _L("My theme")
                                   : m_installed ? from_u8(m_spec.name) + " " + _L("copy")
                                                 : _L("My") + " " + from_u8(m_spec.name);
        const wxString name = wxGetTextFromUser(_L("Name for the new theme:"), _L("Save theme"), suggested, this).Trim().Trim(false);
        if (name.empty())
            return false;
        spec.name = into_u8(name);
        if (spec.name.size() > 64)
            spec.name.resize(64);
        id = ThemePack::id_from_name(spec.name);
        // Never hide a shipped theme behind a copy with its folder name.
        if (Theme::shipped(id))
            id = ThemePack::id_from_name(id + " custom");
        if (id != m_id && Theme::installed(id)) {
            MessageDialog ask(this, wxString::Format(_L("A theme named \"%s\" is already installed. Replace it?"), from_u8(id)),
                              _L("Theme"), wxYES_NO | wxICON_QUESTION);
            if (ask.ShowModal() != wxID_YES)
                return false;
        }
    } else if (spec.name.empty()) {
        spec.name = id;
    }

    std::string error;
    if (!Theme::save(id, spec, m_dir, m_imports, error)) {
        MessageDialog(this, _L("The theme could not be saved:") + "\n" + from_u8(error), _L("Theme"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    }
    if (id == Theme::active_id())
        m_saved_running = id;
    wxGetApp().app_config->set("ui_theme", id);
    wxGetApp().app_config->save();
    m_id = id;
    fill_list();
    load(id);
    return true;
}

} // namespace GUI
} // namespace Slic3r
