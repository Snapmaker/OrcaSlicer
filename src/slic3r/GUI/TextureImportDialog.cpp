#include <array>
#include <cstdlib>
#include <functional>
#include <GL/glew.h>

#include "TextureImportDialog.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
// Phase 3 restores these with the auto-mix feature:
//   #include "ColorDecomposeDialog.hpp"
//   #include "ColorDecomposeSupport.hpp"
//   #include "libslic3r/ColorDecomposeRecipe.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "FilamentColorUtils.hpp"
#include "Widgets/ComboBox.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/FullSpectrumMaterialDatabaseProfile.h"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Color.hpp"
#include <optional>
#include <variant>
#include <wx/weakref.h>
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticLine.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <opencv2/core.hpp>

#include <utility>
#include <string>
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/Widgets/PopupWindow.hpp"
#include <vector>
#include "slic3r/GUI/wxExtensions.hpp"
#include <math.h>
#include "slic3r/GUI/GUI_Utils.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <memory>
#include <mutex>
#include <wx/button.h>
#include <wx/colour.h>
#include <wx/colordlg.h>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/evtloop.h>
#include <wx/settings.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/popupwin.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/object.h>
#include <wx/spinctrl.h>
#include <wx/progdlg.h>
#include <wx/peninfobase.h>
#include <wx/statline.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/msgdlg.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>
#include <cstdint>

#include <boost/log/trivial.hpp>
#include <wx/window.h>

static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE = "PLA Basic";
static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE = "PLA";
static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_NAME       = "Bambu PLA Basic";

// "What is CMYW mixing?" opens this page in the default browser. Placeholder
// until the official wiki page is published — swap the URL here only.
static constexpr const char* CMYW_WIKI_URL = "https://github.com/SoftFever/OrcaSlicer/wiki";

static bool is_dark() { return Slic3r::GUI::wxGetApp().dark_mode(); }

static wxColour texture_import_gray9000()
{
    return wxColour(38, 46, 48);
}

static wxColour texture_import_text_colour()
{
    return StateColor::darkModeColorFor(texture_import_gray9000());
}

// StaticLine::SetLineColour stores the raw key and resolves it itself when it
// paints (dark-mode aware), so sinks pass SEPARATOR_COLOUR_KEY directly.
static constexpr const char* SEPARATOR_COLOUR_KEY = "#CECECE";

// Orca's confirm palette, applied here rather than through Button::SetStyle because these buttons
// keep custom pill geometry that SetStyle resets. The Disabled entries are load-bearing: without
// one, StateColor::colorForStates falls through to the Normal entry and a disabled button paints
// as a live accent button.
static void apply_accent_button_colours(Button* btn)
{
    btn->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour("#6B6B6A"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal)));
}

// The disabled (muted) look from the 2026-11 design: grey pill that reads as
// "nothing to apply" and is NOT clickable. Every state gets an explicit entry
// (same load-bearing pattern as apply_accent_button_colours) so no widget
// state can fall through to a stale colour from a previous styling.
static void apply_muted_button_colours(Button* btn)
{
    const wxColour bg("#EBEBEB");
    const wxColour fg("#71717A");
    btn->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(bg, StateColor::Disabled),
        std::pair<wxColour, int>(bg, StateColor::Pressed),
        std::pair<wxColour, int>(bg, StateColor::Hovered),
        std::pair<wxColour, int>(bg, StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::pair<wxColour, int>(bg, StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::pair<wxColour, int>(fg, StateColor::Disabled),
        std::pair<wxColour, int>(fg, StateColor::Pressed),
        std::pair<wxColour, int>(fg, StateColor::Hovered),
        std::pair<wxColour, int>(fg, StateColor::Normal)));
}

// Font point sizes in this dialog were authored against MSW semantics, where
// a point renders at ~4/3 px (96 DPI): "10pt == 14px design", "9pt == 12px".
// wxOSX maps a point 1:1 to an AppKit point (1 logical px; system default is
// 13pt), so the same numbers render 25-30% smaller on macOS. Scale the
// authored point by 4/3 on Apple so both platforms show the same pixel size;
// other platforms keep the authored value untouched.
static int texture_font_pt(int msw_pt)
{
#ifdef __APPLE__
    return std::max(1, (int)std::lround(msw_pt * 4.0 / 3.0));
#else
    return msw_pt;
#endif
}

static wxFont texture_import_section_title_font(wxWindow* win)
{
    wxFont font = win ? win->GetFont() : wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetPointSize(texture_font_pt(9));
    font.MakeBold();
    return font;
}

static wxSize gl_viewport_size(wxWindow* win, const wxSize& logical_size)
{
    wxSize viewport_size = logical_size;
#ifdef __APPLE__
    const double scale = win ? win->GetContentScaleFactor() : 1.0;
    if (scale > 0.0) {
        viewport_size.x = std::max(1, (int)std::round(viewport_size.x * scale));
        viewport_size.y = std::max(1, (int)std::round(viewport_size.y * scale));
    }
#else
    (void)win;
#endif
    return viewport_size;
}

class ScopedInteractiveBusyCursorSuspender
{
public:
    ScopedInteractiveBusyCursorSuspender()
    {
#if defined(__WXMSW__) || defined(__APPLE__)
        while (wxIsBusy()) {
            wxEndBusyCursor();
            ++m_suspended_count;
        }
#endif
    }

    ~ScopedInteractiveBusyCursorSuspender()
    {
#if defined(__WXMSW__) || defined(__APPLE__)
        for (int i = 0; i < m_suspended_count; ++i)
            wxBeginBusyCursor();
#endif
    }

private:
#if defined(__WXMSW__) || defined(__APPLE__)
    int m_suspended_count = 0;
#endif
};

// Strip the "@machine [nozzle]" display suffix so all variants of one
// material dedup to a single family ("Bambu ABS @BBL X1C 0.8 nozzle" and
// "Bambu ABS @BBL A1" both become "Bambu ABS").
static std::string strip_machine_suffix(const std::string& name)
{
    const size_t at = name.find(" @");
    return (at == std::string::npos) ? name : name.substr(0, at);
}

// Use a continuous floating-point path, not separately rounded line segments.
static void texture_draw_glyph(wxAutoBufferedPaintDC& dc, wxWindow* win,
                               double bx, double by, const wxColour& colour,
                               bool check = false)
{
    const double scale = win->FromDIP(100) / 100.0;
    const double x1 = check ? 2.3 : 3.0, y1 = check ? 5.7 : 6.0;
    const double x2 = check ? 4.7 : 7.5, y2 = check ? 8.3 : 9.6;
    const double x3 = check ? 9.7 : 12.0, y3 = check ? 3.9 : 6.0;
#if wxUSE_GRAPHICS_CONTEXT
    if (std::unique_ptr<wxGraphicsContext> gc{wxGraphicsContext::Create(dc)}) {
        gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
        gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(colour)
            .Width((check ? 1.5 : 1.2) * scale).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
        auto path = gc->CreatePath();
        path.MoveToPoint(bx + x1 * scale, by + y1 * scale);
        path.AddLineToPoint(bx + x2 * scale, by + y2 * scale);
        path.AddLineToPoint(bx + x3 * scale, by + y3 * scale);
        gc->StrokePath(path);
        return;
    }
#endif
    auto round = [](double v) { return static_cast<int>(std::lround(v)); };
    wxPoint points[] = {{round(bx + x1 * scale), round(by + y1 * scale)},
                        {round(bx + x2 * scale), round(by + y2 * scale)},
                        {round(bx + x3 * scale), round(by + y3 * scale)}};
    if (!check) {
        const int cx = round(bx + 7.5 * scale);
        const int dx = std::max(1, round(4.5 * scale));
        points[0].x = cx - dx; points[1].x = cx; points[2].x = cx + dx;
    }
    wxPen pen(colour, std::max(1, round((check ? 1.5 : 1.2) * scale)));
    pen.SetCap(wxCAP_ROUND);
    pen.SetJoin(wxJOIN_ROUND);
    dc.SetPen(pen);
    dc.DrawLines(3, points);
}

// Draw a 1px border hugging a window's edge. GDI strokes inside the path
// while CoreGraphics centres the stroke on it, so a naive DrawRect(0,0,w,h)
// loses its outer half to window clipping on macOS (buttons looked
// borderless); insetting the path by 1 keeps the full border everywhere.
// The caller must set the fill brush beforehand; the border pen is set here.
static void texture_draw_edge_border(wxDC& dc, const wxSize& sz, const wxColour& colour, double radius = 0.0)
{
    dc.SetPen(wxPen(colour, 1));
    if (radius > 0.0)
        dc.DrawRoundedRectangle(1, 1, sz.x - 2, sz.y - 2, radius);
    else
        dc.DrawRectangle(1, 1, sz.x - 2, sz.y - 2);
}

static bool needs_filament_swatch_border(const wxColour& colour){
    if (is_dark())
        return colour.Red() < 45 && colour.Green() < 45 && colour.Blue() < 45;
    return colour.Red() > 224 && colour.Green() > 224 && colour.Blue() > 224;
}

static wxColour filament_swatch_border_colour()
{
    return is_dark() ? wxColour(207, 207, 207) : wxColour(130, 130, 128);
}

static void draw_filament_swatch_border(wxDC& dc, const wxColour& colour,
                                        int x, int y, int w, int h, int radius = 0)
{
    if (!needs_filament_swatch_border(colour))
        return;

    dc.SetPen(wxPen(filament_swatch_border_colour(), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    if (radius > 0)
        dc.DrawRoundedRectangle(x, y, w, h, radius);
    else
        dc.DrawRectangle(x, y, w, h);
}

static void draw_filament_swatch_ellipse_border(wxDC& dc, const wxColour& colour,
                                                int x, int y, int w, int h)
{
    if (!needs_filament_swatch_border(colour))
        return;

    dc.SetPen(wxPen(filament_swatch_border_colour(), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawEllipse(x, y, w, h);
}

static wxString ellipsize_text(wxDC& dc, wxString text, int max_width)
{
    if (max_width <= 0)
        return wxEmptyString;
    if (dc.GetTextExtent(text).x <= max_width)
        return text;

    const wxString ellipsis = "...";
    while (!text.empty() && dc.GetTextExtent(text + ellipsis).x > max_width)
        text.RemoveLast();
    if (text.empty() && dc.GetTextExtent(ellipsis).x > max_width)
        return wxString();
    return text + ellipsis;
}

// ============================================================
// AccentSlider — thin track + accent-coloured triangle thumb
// ============================================================

class AccentSlider : public wxPanel {
public:
    AccentSlider(wxWindow* parent, int value, int minVal, int maxVal,
                 const wxPoint& pos = wxDefaultPosition,
                 const wxSize& size = wxDefaultSize);
    ~AccentSlider() override;
    int  GetValue() const;
    void SetValue(int val);
    bool Enable(bool enable = true) override;
private:
    void OnPaint(wxPaintEvent&);
    void OnMouse(wxMouseEvent&);
    int  xFromValue() const;
    int  valueFromX(int x) const;
    int  m_value, m_min, m_max;
    bool m_dragging = false;
};

AccentSlider::AccentSlider(wxWindow* parent, int value, int minVal, int maxVal,
                           const wxPoint& pos, const wxSize& size)
    : wxPanel(parent, wxID_ANY, pos, size.IsFullySpecified() ? size : wxSize(-1, parent->FromDIP(24)),
              wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
    , m_value(std::clamp(value, minVal, maxVal)), m_min(minVal), m_max(maxVal)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(-1, FromDIP(24)));

    Bind(wxEVT_PAINT,     &AccentSlider::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        Refresh();
    });
    Bind(wxEVT_LEFT_DOWN, &AccentSlider::OnMouse, this);
    Bind(wxEVT_LEFT_UP,   &AccentSlider::OnMouse, this);
    Bind(wxEVT_MOTION,    &AccentSlider::OnMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
}

AccentSlider::~AccentSlider()
{
    // See MixedFilamentDialog::~MixedFilamentDialog: a widget destroyed while it
    // still holds the capture wedges mouse input for the whole application.
    if (HasCapture())
        ReleaseMouse();
}

int AccentSlider::GetValue() const { return m_value; }

void AccentSlider::SetValue(int val)
{
    val = std::clamp(val, m_min, m_max);
    if (val != m_value) { m_value = val; Refresh(); }
}

bool AccentSlider::Enable(bool enable)
{
    bool ok = wxPanel::Enable(enable);
    Refresh();
    return ok;
}

int AccentSlider::xFromValue() const
{
    wxSize sz = GetClientSize();
    int margin = FromDIP(6);
    int track_w = sz.x - 2 * margin;
    if (m_max <= m_min || track_w <= 0) return margin;
    return margin + (m_value - m_min) * track_w / (m_max - m_min);
}

int AccentSlider::valueFromX(int x) const
{
    wxSize sz = GetClientSize();
    int margin = FromDIP(6);
    int track_w = sz.x - 2 * margin;
    if (track_w <= 0 || m_max <= m_min) return m_min;
    int val = m_min + (x - margin) * (m_max - m_min) / track_w;
    return std::clamp(val, m_min, m_max);
}

void AccentSlider::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();

    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    // 2026-11 step-1 design: 3px rounded track (#EBEBEB / #009688) with a
    // 10px circular thumb (white fill, accent border).
    const int margin = FromDIP(6);
    const int track_y = sz.y / 2;
    const int thumb_r = FromDIP(5);

    wxColour accent_clr = StateColor::darkModeColorFor(IsEnabled() ? wxColour("#009688") : wxColour("#ACACAC"));
    wxColour track_clr  = StateColor::darkModeColorFor(IsEnabled() ? wxColour("#EBEBEB") : wxColour("#DFDFDF"));

    int tx = xFromValue();

    const int track_h = std::max(2, FromDIP(3)); // 3px design track, DPI-scaled
    const int track_top = track_y - track_h / 2;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(track_clr));
    dc.DrawRoundedRectangle(margin, track_top, std::max(0, sz.x - 2 * margin), track_h, FromDIP(1));

    if (tx - margin > 0) {
        dc.SetBrush(wxBrush(accent_clr));
        dc.DrawRoundedRectangle(margin, track_top, tx - margin, track_h, FromDIP(1));
    }

    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
    dc.SetPen(wxPen(accent_clr, 1));
    dc.DrawCircle(tx, track_y, thumb_r);
}

void AccentSlider::OnMouse(wxMouseEvent& evt)
{
    if (!IsEnabled()) return;

    auto update = [&](int x) {
        int nv = valueFromX(x);
        if (nv != m_value) {
            m_value = nv;
            Refresh();
            wxCommandEvent e(wxEVT_SLIDER, GetId());
            e.SetEventObject(this);
            ProcessWindowEvent(e);
        }
    };

    if (evt.LeftDown()) {
        m_dragging = true;
        if (!HasCapture()) CaptureMouse();
        update(evt.GetX());
    } else if (evt.LeftUp()) {
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
    } else if (evt.Dragging() && m_dragging) {
        update(evt.GetX());
    }
}

namespace Slic3r { namespace GUI {

static constexpr int kMixModeFontPt = 10;

static void texture_update_mix_mode_metrics(wxWindow* trigger)
{
    if (!trigger) return;
    wxFont font = trigger->GetFont();
    font.SetPointSize(texture_font_pt(kMixModeFontPt));
    font.SetWeight(wxFONTWEIGHT_MEDIUM);
    wxClientDC dc(trigger);
    dc.SetFont(font);
    int width = 0, height = 0;
    for (const wxString& label : {_L("Direct Filament Mapping"), _L("CMYW Auto Mix")}) {
        const wxSize extent = dc.GetTextExtent(label);
        width = std::max(width, extent.x);
        height = std::max(height, extent.y);
    }
    trigger->SetMinSize(wxSize(std::max(trigger->FromDIP(149), width + trigger->FromDIP(48)),
                               std::max(trigger->FromDIP(30), height + trigger->FromDIP(8))));
}

wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_DONE, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_ERROR, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_MESH_REPAIR_DECISION, wxCommandEvent);

static std::array<float, 4> parse_color_string(const std::string& hex)
{
    std::array<float, 4> c = {1.f, 1.f, 1.f, 1.f};
    if (hex.size() >= 7 && hex[0] == '#') {
        unsigned long val = std::strtoul(hex.c_str() + 1, nullptr, 16);
        c[0] = ((val >> 16) & 0xFF) / 255.f;
        c[1] = ((val >>  8) & 0xFF) / 255.f;
        c[2] = ((val      ) & 0xFF) / 255.f;
    }
    return c;
}

static wxString rgb_to_hex(const std::array<std::size_t, 3>& c)
{
    return wxString::Format("#%02X%02X%02X",
        (unsigned)c[0], (unsigned)c[1], (unsigned)c[2]);
}

// Self-destroying transient popup for the mix-mode dropdown: a bare
// wxPopupTransientWindow only HIDES on Dismiss(), so keeping a raw pointer
// guard in the dialog would block every reopen after the first dismiss.
// Destroying from OnDismiss (deferred) keeps the guard meaningful — same
// pattern as FilamentSelectPopup::schedule_destroy().
class MixModePopup : public wxPopupTransientWindow
{
public:
    using wxPopupTransientWindow::wxPopupTransientWindow;

private:
    void OnDismiss() override
    {
        wxPopupTransientWindow::OnDismiss();
        CallAfter([this]() { Destroy(); });
    }
};

static wxString filament_name_to_wx_string(const std::string& name)
{
    wxString utf8_name = wxString::FromUTF8(name.c_str());
    if (!utf8_name.empty() || name.empty())
        return utf8_name;
    return wxString(name);
}

static std::string texture_normalize_color_hex(std::string hex)
{
    if (hex.empty())
        return "#808080";
    if (hex.front() != '#')
        hex = "#" + hex;
    // Local copy of decompose_normalize_color_hex (upstream ColorDecomposeSupport.cpp):
    // truncate to #RRGGBB and uppercase.
    if (hex.size() >= 7)
        hex = hex.substr(0, 7);
    std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return hex;
}

static std::string texture_rgba_to_hex(const std::array<float, 4>& rgba)
{
    return wxString::Format("#%02X%02X%02X",
        (unsigned char)std::clamp(rgba[0] * 255.f, 0.f, 255.f),
        (unsigned char)std::clamp(rgba[1] * 255.f, 0.f, 255.f),
        (unsigned char)std::clamp(rgba[2] * 255.f, 0.f, 255.f)).ToStdString();
}

static bool texture_entry_is_physical(TextureFilamentKind kind)
{
    return kind == TextureFilamentKind::ExistingPhysical || kind == TextureFilamentKind::NewPhysical;
}

static bool texture_entry_is_mixed(TextureFilamentKind kind)
{
    return kind == TextureFilamentKind::ExistingMixed || kind == TextureFilamentKind::NewMixed;
}

static bool texture_entry_is_pla_basic(const TextureFilamentEntry& entry)
{
    return entry.type == DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE || entry.type == DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE ||
           entry.name.find(DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE) != std::string::npos ||
           entry.preset_name.find(DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE) != std::string::npos;
}

static bool starts_with_preset_name(const std::string& name, const char* prefix)
{
    const size_t prefix_len = std::strlen(prefix);
    return name.size() >= prefix_len && name.compare(0, prefix_len, prefix) == 0;
}

static bool texture_entry_official_basic(const TextureFilamentEntry& entry)
{
    if (!texture_entry_is_physical(entry.kind))
        return false;
    // Local stand-in for official_basic_type_from_preset_name
    // (upstream ColorDecomposeSupport): recognize the default virtual filament
    // preset family.
    auto official_basic_type_from_preset_name = [](const std::string& name) -> std::string {
        return starts_with_preset_name(name, DEFAULT_VIRTUAL_FILAMENT_NAME)
            ? std::string(DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE) : std::string();
    };
    // NewPhysical entries are created by add_virtual_filament with a fixed Bambu Basic name.
    if (entry.kind == TextureFilamentKind::NewPhysical)
        return !official_basic_type_from_preset_name(entry.name).empty();
    // ExistingPhysical: resolve the filament preset name from project_config_index.
    auto& pb = *wxGetApp().preset_bundle;
    const size_t cfg = entry.project_config_index;
    if (cfg < pb.filament_presets.size())
        return !official_basic_type_from_preset_name(pb.filament_presets[cfg]).empty();
    return false;
}

static std::string resolve_default_virtual_filament_preset_name()
{
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return {};

    auto valid_preset_name = [preset_bundle](const std::string& name) -> bool {
        return !name.empty() && preset_bundle->filaments.find_preset(name, false) != nullptr;
    };

    // Doc §3.3.4: auto-added filaments take the type of the project's LAST
    // filament, exactly like manually adding one more filament.
    if (!preset_bundle->filament_presets.empty()) {
        const std::string& last = preset_bundle->filament_presets.back();
        if (valid_preset_name(last))
            return last;
    }

    const auto* default_profiles = preset_bundle->printers.get_selected_preset()
        .config.option<ConfigOptionStrings>("default_filament_profile");
    if (default_profiles) {
        for (const std::string& name : default_profiles->values) {
            if (starts_with_preset_name(name, DEFAULT_VIRTUAL_FILAMENT_NAME) && valid_preset_name(name))
                return name;
        }
    }

    for (const Preset& preset : preset_bundle->filaments.get_presets()) {
        if (preset.is_system && preset.is_visible && preset.is_compatible &&
            starts_with_preset_name(preset.name, DEFAULT_VIRTUAL_FILAMENT_NAME)) {
            return preset.name;
        }
    }

    for (const Preset& preset : preset_bundle->filaments.get_presets()) {
        if (preset.is_visible && preset.is_compatible &&
            starts_with_preset_name(preset.name, DEFAULT_VIRTUAL_FILAMENT_NAME)) {
            return preset.name;
        }
    }

    std::string selected = preset_bundle->filaments.get_selected_preset_name();
    return valid_preset_name(selected) ? selected : std::string();
}

// ============================================================
// FilamentSelectPopup
// ============================================================

class FilamentSelectPopup : public PopupWindow
{
public:
    FilamentSelectPopup(wxWindow* parent,
                        const std::vector<TextureFilamentEntry>& entries,
                        const std::vector<std::array<float, 4>>& colors_rgba,
                        const std::vector<std::string>          names,
                        size_t                                   existing_count,
                        int                                      popup_width,
                        wxWindow*                                dialog_anchor,
                        std::function<void(int)>                 on_select,
                        std::function<void()>                    on_add_filament_request,
                        std::function<void()>                    on_decompose_color,
                        std::function<bool()>                    can_add_filament,
                        std::function<void(bool)>                on_close,
                        bool                                     allow_mixed,
                        std::vector<int>                         display_numbers)
        : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_entries(entries)
        , m_colors_rgba(colors_rgba)
        , m_names(names)
        , m_dialog_anchor(dialog_anchor)
        , m_on_select(std::move(on_select))
        , m_on_add_filament_request(std::move(on_add_filament_request))
        , m_on_decompose_color(std::move(on_decompose_color))
        , m_can_add_filament(std::move(can_add_filament))
        , m_on_close(std::move(on_close))
        , m_display_numbers(std::move(display_numbers))
    {
        wxColour pop_bg = StateColor::darkModeColorFor(*wxWHITE);
        SetBackgroundColour(pop_bg);

        m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_content->SetBackgroundColour(pop_bg);
        m_content->SetScrollRate(0, FromDIP(20));
        auto* outer = new wxBoxSizer(wxVERTICAL);

        // 2026-11 "耗材丝" card: a fixed 390px-wide, ≤472px-tall white card
        // (Figma: background #FFFFFF, shadow 0 0 25px rgba(0,0,0,0.1), radius
        // 8). wx popup windows cannot paint an outer glow, so the card relies
        // on its outline-free white surface; the shadow is approximated by the
        // OS popup border where available.
        // Width = the target CHIP's width (row box minus its leading 20px
        // numbered swatch + 8px gap = 420): left AND right edges align with
        // the chip the popup opens from (2026-11 feedback).
        const int pop_w   = std::max(FromDIP(320), popup_width - FromDIP(28));
        const int row_h   = FromDIP(32);
        const int pad     = FromDIP(16);
        // 2026-11 feedback: the card's initial view is the section header
        // plus AT MOST 3 rows; the rest stays reachable via the scrollbar.
        const int max_visible_rows = 3;
        const int max_card_h = FromDIP(472);
        const wxColour header_clr = StateColor::darkModeColorFor(wxColour("#71717A"));

        // 2026-11 耗材丝 card: section header 14px medium #71717A with a
        // #DBDBDB divider; entries are bordered 32px chips.
        auto add_section_header = [&](const wxString& label) {
            auto* hdr = new wxStaticText(m_content, wxID_ANY, label);
            wxFont hf = hdr->GetFont();
            hf.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            hf.SetWeight(wxFONTWEIGHT_MEDIUM);
            hdr->SetFont(hf);
            hdr->SetForegroundColour(header_clr);
            outer->Add(hdr, 0, wxLEFT | wxRIGHT | wxTOP, pad);
            auto* line = new StaticLine(m_content);
            line->SetLineColour(StateColor::darkModeColorFor(wxColour("#DBDBDB")));
            outer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
            outer->AddSpacer(FromDIP(12));
        };

        auto add_section = [&](const wxString& label, TextureFilamentKind kind) {
            bool has_any = false;
            for (const auto& entry : m_entries) {
                if (entry.kind == kind) {
                    has_any = true;
                    break;
                }
            }
            if (!has_any)
                return;
            add_section_header(label);
            for (const auto& entry : m_entries) {
                if (entry.kind != kind)
                    continue;
                wxPanel* row = texture_entry_is_mixed(entry.kind) ? create_mixed_item_row(entry, row_h)
                                                                  : create_item_row((size_t)entry.dialog_index, row_h);
                outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
            }
        };

        // Section order matches compute_display_numbers() so the visible IDs
        // ascend monotonically (ExistingPhysical -> NewPhysical -> ExistingMixed
        // -> NewMixed) instead of jumping (e.g. 1,2 -> 7 -> 3,4,5,6 -> 8,9,10).
        add_section(_L("Project Physical Filaments"), TextureFilamentKind::ExistingPhysical);
        add_section(_L("New Physical Filaments"), TextureFilamentKind::NewPhysical);
        if (allow_mixed) {
            add_section(_L("Project Mixed Filaments"), TextureFilamentKind::ExistingMixed);
            add_section(_L("New Mixed Filaments"), TextureFilamentKind::NewMixed);
        }
        const int visible_count = static_cast<int>(std::count_if(m_entries.begin(), m_entries.end(),
            [allow_mixed](const TextureFilamentEntry& entry) {
                return allow_mixed || texture_entry_is_physical(entry.kind);
            }));

        // "+ Add Material" hands the flow to the dialog: it resolves one of the
        // user's CONFIGURED material families (doc §3.3.4) and opens the fork's
        // official filament color panel (FilamentColorDialog, same one the
        // sidebar swatch uses); the raw wxColourDialog is only the fallback
        // when the color library has no match for any configured preset.
        // 2026-11 耗材丝 footer: full-width "＋ 添加耗材" pill (#EBEBEB,
        // radius 4, 40px tall) with a drawn plus glyph, replacing the plain
        // teal text link.
        const bool add_enabled = !m_can_add_filament || m_can_add_filament();
        const wxColour btn_bg_on  = StateColor::darkModeColorFor(wxColour("#EBEBEB"));
        const wxColour btn_bg_off = StateColor::darkModeColorFor(wxColour("#F3F3F3"));
        const wxColour btn_fg_on  = StateColor::darkModeColorFor(wxColour("#18181B"));
        const wxColour btn_fg_off = StateColor::darkModeColorFor(wxColour("#9A9A9A"));
        auto* add_label = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                      wxSize(pop_w - 2 * FromDIP(21), FromDIP(40)));
        add_label->SetBackgroundColour(add_enabled ? btn_bg_on : btn_bg_off);
        add_label->SetBackgroundStyle(wxBG_STYLE_PAINT);
        add_label->SetCursor(wxCursor(add_enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        if (!add_enabled)
            add_label->SetToolTip(wxString::Format(
                _L_PLURAL("The project supports up to %d filament. Extra filaments will be discarded.",
                          "The project supports up to %d filaments. Extra filaments will be discarded.",
                          (int)MAXIMUM_EXTRUDER_NUMBER),
                (int)MAXIMUM_EXTRUDER_NUMBER));
        add_label->Bind(wxEVT_PAINT, [add_enabled, btn_bg_on, btn_bg_off, btn_fg_on, btn_fg_off](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(add_enabled ? btn_bg_on : btn_bg_off));
            dc.Clear();
            dc.SetBrush(wxBrush(add_enabled ? btn_bg_on : btn_bg_off));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, p->FromDIP(4));

            const wxColour fg = add_enabled ? btn_fg_on : btn_fg_off;
            wxFont f = p->GetFont();
            f.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            f.SetWeight(wxFONTWEIGHT_MEDIUM);
            dc.SetFont(f);
            const wxString text = _L("Add Filament");
            const int glyph = p->FromDIP(10);
            const int gap = p->FromDIP(6);
            const wxSize tsz = dc.GetTextExtent(text);
            const int total = glyph + gap + tsz.x;
            int x = (sz.x - total) / 2;
            const int cy = sz.y / 2;
            // Figma 16x16 plus icon: centred cross, not a shifted bar.
            const int cx = x + glyph / 2;
            dc.SetPen(wxPen(fg, p->FromDIP(2)));
            dc.DrawLine(cx - glyph / 2, cy, cx + glyph / 2, cy);
            dc.DrawLine(cx, cy - glyph / 2, cx, cy + glyph / 2);
            x += glyph + gap;
            dc.SetTextForeground(fg);
            dc.DrawText(text, x, (sz.y - tsz.y) / 2);
        });
        add_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_can_add_filament && !m_can_add_filament())
                return;
            auto on_add_request = m_on_add_filament_request;
            close_popup(true);
            if (on_add_request) on_add_request();
        });

        m_content->SetSizer(outer);
        m_content->FitInside();

        auto* top_sizer = new wxBoxSizer(wxVERTICAL);
        // Initial list area = one section header + AT MOST 3 rows, measured
        // forward from the sizer's own geometry. Rows carry a 12px bottom
        // border and headers/dividers add fixed overhead, so deriving the
        // height by subtracting raw row heights from the full content min
        // height barely shrank the card (2026-11 feedback).
        const auto& content_items = outer->GetChildren();
        int header_block = FromDIP(16 + 6 + 12); // title top pad + divider pad + spacer
        if (!content_items.empty())
            header_block += content_items[0]->GetMinSize().y; // section title text
        if (content_items.size() > 1)
            header_block += content_items[1]->GetMinSize().y; // divider line
        const int row_step = row_h + FromDIP(12);             // row + its bottom border
        int list_h = header_block
            + std::min(visible_count, max_visible_rows) * row_step;

        // Size the card at its DESIGN size first - never pre-shrunk by the
        // surrounding screen space (2026-11 feedback).
        m_content->SetMinSize(wxSize(pop_w, list_h));
        m_content->SetMaxSize(wxSize(pop_w, list_h));
        top_sizer->Add(m_content, 0, wxEXPAND);

        top_sizer->AddSpacer(FromDIP(4));
        top_sizer->Add(add_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(16));
        SetSizerAndFit(top_sizer);

        // 2026-11 feedback: open on whichever side can display the FULL
        // initial size - below the anchor row first, then above it. Only
        // when NEITHER side fits does the card clamp to the larger side's
        // room; the scrolled list gives up the space the footer needs and
        // keeps the rest reachable via its scrollbar.
        const int natural_h = top_sizer->GetMinSize().y;
        const int footer_h  = natural_h - list_h;
        int height_cap = natural_h;
        if (m_dialog_anchor) {
            wxRect work;
            const int display_idx = wxDisplay::GetFromWindow(m_dialog_anchor.get());
            work = display_idx != wxNOT_FOUND ? wxDisplay(display_idx).GetClientArea()
                                              : wxDisplay().GetClientArea();
            const int anchor_top    = m_dialog_anchor->ClientToScreen(wxPoint(0, 0)).y;
            const int anchor_bottom =
                m_dialog_anchor->ClientToScreen(wxPoint(0, m_dialog_anchor->GetSize().y)).y;
            const int room_below = work.GetBottom() - anchor_bottom - FromDIP(8);
            const int room_above = anchor_top - work.GetTop() - FromDIP(8);
            if (room_below >= natural_h)
                m_place_above = false;
            else if (room_above >= natural_h)
                m_place_above = true;
            else
                m_place_above = room_above > room_below;
            const int room = m_place_above ? room_above : room_below;
            height_cap = std::max(row_h * 2 + FromDIP(64),
                                  std::min(natural_h, room));
        }
        if (height_cap < natural_h) {
            list_h = std::max(row_h, std::min(list_h, height_cap - footer_h));
            m_content->SetMinSize(wxSize(pop_w, list_h));
            m_content->SetMaxSize(wxSize(pop_w, list_h));
            Fit();
        }
        SetSize(pop_w, std::min(top_sizer->GetMinSize().y, height_cap));
    }

    // Open direction, decided during construction: true = above the anchor.
    bool place_above() const { return m_place_above; }

    // Explicit Dismiss() need not invoke OnDismiss() on every wx port.
    void close_popup(bool from_action = false)
    {
        if (m_destroy_scheduled)
            return;
        m_closing_from_action = from_action;
        wxWeakRef<FilamentSelectPopup> self(this);
        Dismiss();
        if (self.get())
            self->finish_close();
    }

private:
    void OnDismiss() override
    {
        wxPopupTransientWindow::OnDismiss();
        finish_close();
    }

    void finish_close()
    {
        if (m_destroy_scheduled)
            return;
        restore_cursor_state();
        if (m_on_close) m_on_close(m_closing_from_action);
        m_closing_from_action = false;
        schedule_destroy();
    }

    void choose_filament(int idx)
    {
        // Copy before dismissing: the popup owns the original callback.
        auto on_select = m_on_select;
        close_popup(true);
        if (on_select) on_select(idx);
    }

    void restore_cursor_state()
    {
        SetCursor(wxNullCursor);
        if (m_content)
            m_content->SetCursor(wxNullCursor);
        if (auto* anchor = m_dialog_anchor.get(); anchor && !anchor->IsBeingDeleted())
            anchor->SetCursor(wxCursor(wxCURSOR_HAND));
        wxSetCursor(wxNullCursor);
    }

    void schedule_destroy()
    {
        if (m_destroy_scheduled)
            return;
        m_destroy_scheduled = true;
        CallAfter([this]() { Destroy(); });
    }

    wxPanel* create_item_row(size_t idx, int row_h)
    {
        // 2026-11 耗材丝 entries: 32px bordered chips (#DBDBDB, radius 4)
        // with a flat 20px numbered swatch and a 14px name.
        wxColour row_bg    = StateColor::darkModeColorFor(*wxWHITE);
        wxColour hover_bg  = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        wxColour chip_bd   = StateColor::darkModeColorFor(wxColour("#DBDBDB"));
        wxColour name_fg   = StateColor::darkModeColorFor(wxColour("#18181B"));

        wxPanel* row = new wxPanel(m_content, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h));
        row->SetBackgroundColour(row_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(wxCURSOR_HAND));

        const int sq     = row->FromDIP(20);
        const int sq_r   = row->FromDIP(0);
        const int sq_x   = row->FromDIP(8);
        const int gap1   = row->FromDIP(8);

        wxColour fil_clr = idx < m_colors_rgba.size()
            ? wxColour((unsigned char)(m_colors_rgba[idx][0] * 255.f),
                       (unsigned char)(m_colors_rgba[idx][1] * 255.f),
                       (unsigned char)(m_colors_rgba[idx][2] * 255.f))
            : wxColour(128, 128, 128);

        wxString name_str = (idx < m_names.size()) ? filament_name_to_wx_string(m_names[idx])
                                                    : wxString::Format(_L("Filament %d"), display_number((int)idx));
        row->SetToolTip(name_str);

        row->Bind(wxEVT_PAINT, [this, idx, sq, sq_r, sq_x, gap1, fil_clr, name_str, row_bg, hover_bg, chip_bd, name_fg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();
            // Clear the full buffer, including the inset border and rounded corners.
            dc.SetBackground(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.Clear();

            bool hovered = (m_hover_idx == (int)idx);
            dc.SetBrush(wxBrush(hovered ? hover_bg : row_bg));
            texture_draw_edge_border(dc, sz, chip_bd, p->FromDIP(4));

            int sq_y = (sz.y - sq) / 2;
            wxColour paint_clr = fil_clr;
            if (idx < m_colors_rgba.size()) {
                paint_clr = wxColour((unsigned char)(m_colors_rgba[idx][0] * 255.f),
                                     (unsigned char)(m_colors_rgba[idx][1] * 255.f),
                                     (unsigned char)(m_colors_rgba[idx][2] * 255.f));
            }
            dc.SetBrush(wxBrush(paint_clr));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(sq_x, sq_y, sq, sq, sq_r);
            draw_filament_swatch_border(dc, paint_clr, sq_x, sq_y, sq, sq, sq_r);

            {
                wxFont nf = p->GetFont();
                nf.SetPointSize(texture_font_pt(9));
                dc.SetFont(nf);
                dc.SetTextForeground(paint_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                wxString ns = wxString::Format("%d", display_number((int)idx));
                wxSize tsz = dc.GetTextExtent(ns);
                dc.DrawText(ns, sq_x + (sq - tsz.x) / 2, sq_y + (sq - tsz.y) / 2);
            }

            // Material name
            {
                wxFont mf = p->GetFont();
                mf.SetPointSize(texture_font_pt(10));
                dc.SetFont(mf);
                dc.SetTextForeground(name_fg);
                int tx = sq_x + sq + gap1;
                wxString display = ellipsize_text(dc, name_str, sz.x - tx - p->FromDIP(4));
                wxSize tsz = dc.GetTextExtent(display);
                if (!display.empty())
                    dc.DrawText(display, tx, (sz.y - tsz.y) / 2);
            }
        });

        row->Bind(wxEVT_MOTION, [this, idx](wxMouseEvent& evt) {
            if (m_hover_idx != (int)idx) {
                m_hover_idx = (int)idx;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            if (m_hover_idx != -1) {
                m_hover_idx = -1;
                m_content->Refresh();
            }
            evt.Skip();
        });

        row->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            choose_filament(static_cast<int>(idx));
        });

        return row;
    }

    wxPanel* create_mixed_item_row(const TextureFilamentEntry& entry, int row_h)
    {
        wxColour row_bg    = StateColor::darkModeColorFor(*wxWHITE);
        wxColour hover_bg  = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        wxColour chip_bd   = StateColor::darkModeColorFor(wxColour("#DBDBDB"));
        wxColour name_fg   = StateColor::darkModeColorFor(wxColour("#18181B"));
        const int idx = entry.dialog_index;

        wxPanel* row = new wxPanel(m_content, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h));
        row->SetBackgroundColour(row_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        row->SetToolTip(entry.name.empty() ? wxString::Format(_L("Filament %d"), display_number(idx)) : filament_name_to_wx_string(entry.name));

        row->Bind(wxEVT_PAINT, [this, entry, idx, row_bg, hover_bg, chip_bd, name_fg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();
            // Clear the full buffer, including the inset border and rounded corners.
            dc.SetBackground(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.Clear();
            const bool hovered = (m_hover_idx == idx);
            dc.SetBrush(wxBrush(hovered ? hover_bg : row_bg));
            texture_draw_edge_border(dc, sz, chip_bd, p->FromDIP(4));

            wxFont font = p->GetFont();
            font.SetPointSize(texture_font_pt(9));
            dc.SetFont(font);
            int x = p->FromDIP(2);
            const int sw = p->FromDIP(22);
            const int sw_r = p->FromDIP(2);
            const int y = (sz.y - sw) / 2;

            for (size_t ci = 0; ci < entry.mixed_components.size() && ci < entry.mixed_ratios.size(); ++ci) {
                if (ci > 0) {
                    dc.SetTextForeground(name_fg);
                    wxString plus = "+";
                    wxSize psz = dc.GetTextExtent(plus);
                    dc.DrawText(plus, x, (sz.y - psz.y) / 2);
                    x += psz.x + p->FromDIP(4);
                }

                const unsigned int comp_id = entry.mixed_components[ci];
                const int comp_dialog_idx = comp_id >= 1 ? (int)comp_id - 1 : -1;
                wxColour comp_clr("#D9D9D9");
                if (comp_dialog_idx >= 0 && comp_dialog_idx < (int)m_colors_rgba.size()) {
                    const auto& c = m_colors_rgba[comp_dialog_idx];
                    comp_clr = wxColour((unsigned char)(c[0] * 255.f),
                                        (unsigned char)(c[1] * 255.f),
                                        (unsigned char)(c[2] * 255.f));
                }

                dc.SetBrush(wxBrush(comp_clr));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(x, y, sw, sw, sw_r);
                draw_filament_swatch_border(dc, comp_clr, x, y, sw, sw, sw_r);

                wxString num = wxString::Format("%d", display_number(comp_dialog_idx));
                wxSize nsz = dc.GetTextExtent(num);
                dc.SetTextForeground(comp_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                dc.DrawText(num, x + (sw - nsz.x) / 2, y + (sw - nsz.y) / 2);
                x += sw + p->FromDIP(4);

                dc.SetTextForeground(name_fg);
                wxString pct = wxString::Format("%d%%", entry.mixed_ratios[ci]);
                wxSize psz = dc.GetTextExtent(pct);
                dc.DrawText(pct, x, (sz.y - psz.y) / 2);
                x += psz.x + p->FromDIP(4);
            }
        });

        row->Bind(wxEVT_MOTION, [this, idx](wxMouseEvent& evt) {
            if (m_hover_idx != idx) {
                m_hover_idx = idx;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            if (m_hover_idx != -1) {
                m_hover_idx = -1;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            choose_filament(idx);
        });

        return row;
    }

    wxScrolledWindow*                         m_content = nullptr;
    std::vector<TextureFilamentEntry>          m_entries;
    std::vector<std::array<float, 4>>          m_colors_rgba;
    std::vector<std::string>                   m_names;
    wxWeakRef<wxWindow>                       m_dialog_anchor;
    std::function<void(int)>                   m_on_select;
    std::function<void()>                      m_on_add_filament_request;
    std::function<void()>                      m_on_decompose_color;
    std::function<bool()>                      m_can_add_filament;
    std::function<void(bool)>                  m_on_close;
    // 1-based display number per dialog_index, mirroring the post-apply
    // sidebar ordering (ExistingPhysical, NewPhysical, ExistingMixed, NewMixed).
    std::vector<int>                           m_display_numbers;
    int                                        m_hover_idx = -1;
    bool                                       m_closing_from_action = false;
    bool                                       m_destroy_scheduled = false;
    bool                                       m_place_above = false;

    // Returns the display number for a dialog_index, falling back to idx + 1
    // when no mapping is available (e.g. index out of range).
    int display_number(int idx) const {
        return (idx >= 0 && idx < (int)m_display_numbers.size() && m_display_numbers[idx] > 0)
            ? m_display_numbers[idx] : idx + 1;
    }
};

// ============================================================
// ============================================================

// ============================================================
// TexturePreviewCanvas
// ============================================================

TexturePreviewCanvas::TexturePreviewCanvas(wxWindow* parent, const wxGLAttributes& attrs)
    : wxGLCanvas(parent, attrs, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    m_context = new wxGLContext(this);

    Bind(wxEVT_PAINT, &TexturePreviewCanvas::on_paint, this);
    Bind(wxEVT_SIZE,  &TexturePreviewCanvas::on_size,  this);
    Bind(wxEVT_MOUSEWHEEL,  &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_DOWN,   &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP,     &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_DOWN,  &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_UP,    &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_UP,   &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MOTION,      &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_drag_mode = DragMode::None;
        m_reset_overlay_pressed = false;
    });
    Bind(wxEVT_LEAVE_WINDOW, &TexturePreviewCanvas::on_mouse, this);
}

TexturePreviewCanvas::~TexturePreviewCanvas()
{
    // See MixedFilamentDialog::~MixedFilamentDialog: a widget destroyed while it
    // still holds the capture wedges mouse input for the whole application.
    if (HasCapture())
        ReleaseMouse();

    if (m_context) {
        SetCurrent(*m_context);
        if (m_tex_id)
            glDeleteTextures(1, &m_tex_id);
        for (unsigned int id : m_gl_tex_ids)
            if (id) glDeleteTextures(1, &id);
        for (unsigned int id : {m_reset_icon_tex, m_reset_icon_hover_tex,
                                m_reset_icon_dark_tex, m_reset_icon_dark_hover_tex})
            if (id) glDeleteTextures(1, &id);
        for (auto& g : m_orig_group_vbos)
            if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (m_painted_vbo)
            glDeleteBuffers(1, &m_painted_vbo);
        if (m_painted_color_vbo)
            glDeleteBuffers(1, &m_painted_color_vbo);
        delete m_context;
    }
}

void TexturePreviewCanvas::set_mesh_data(
    const std::vector<std::array<float, 3>>& vertices,
    const std::vector<std::array<int, 3>>&   indices)
{
    m_vertices = vertices;
    m_indices  = indices;
    update_bounding_box();
    compute_smooth_normals();
    m_orig_vbos_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::compute_smooth_normals()
{
    m_vertex_normals.clear();
    if (m_vertices.empty() || m_indices.empty()) return;

    m_vertex_normals.resize(m_vertices.size(), {0.f, 0.f, 0.f});

    for (const auto& face : m_indices) {
        int i0 = face[0], i1 = face[1], i2 = face[2];
        if (i0 < 0 || i0 >= (int)m_vertices.size() ||
            i1 < 0 || i1 >= (int)m_vertices.size() ||
            i2 < 0 || i2 >= (int)m_vertices.size())
            continue;

        const auto& v0 = m_vertices[i0];
        const auto& v1 = m_vertices[i1];
        const auto& v2 = m_vertices[i2];

        float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
        float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
        float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);

        m_vertex_normals[i0][0] += nx; m_vertex_normals[i0][1] += ny; m_vertex_normals[i0][2] += nz;
        m_vertex_normals[i1][0] += nx; m_vertex_normals[i1][1] += ny; m_vertex_normals[i1][2] += nz;
        m_vertex_normals[i2][0] += nx; m_vertex_normals[i2][1] += ny; m_vertex_normals[i2][2] += nz;
    }

    for (auto& n : m_vertex_normals) {
        float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
}

void TexturePreviewCanvas::set_texture_data(
    const std::vector<std::array<float, 2>>& uvs,
    const unsigned char* tex_data, int tex_w, int tex_h, int tex_channels)
{
    m_uvs = uvs;
    m_tex_w = tex_w;
    m_tex_h = tex_h;
    m_tex_channels = tex_channels;
    m_tex_dirty = true;

    size_t sz = (size_t)tex_w * tex_h * tex_channels;
    m_tex_data.assign(tex_data, tex_data + sz);
    Refresh();
}

void TexturePreviewCanvas::set_texture_render_data(
    const std::vector<std::vector<unsigned char>>& tex_pixels_rgb,
    const std::vector<int>& tex_widths,
    const std::vector<int>& tex_heights,
    const std::vector<std::array<std::array<float,2>, 3>>& face_uvs,
    const std::vector<int>& face_tex_ids)
{
    m_tex_pixels_rgb = tex_pixels_rgb;
    m_tex_widths     = tex_widths;
    m_tex_heights    = tex_heights;
    m_face_uvs       = face_uvs;
    m_face_tex_ids   = face_tex_ids;
    m_multi_tex_dirty = true;
    m_orig_vbos_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::upload_textures()
{
    if (!m_multi_tex_dirty) return;
    m_multi_tex_dirty = false;

    for (unsigned int id : m_gl_tex_ids)
        if (id) glDeleteTextures(1, &id);
    m_gl_tex_ids.clear();

    m_gl_tex_ids.resize(m_tex_pixels_rgb.size(), 0);
    for (size_t i = 0; i < m_tex_pixels_rgb.size(); ++i) {
        if (m_tex_pixels_rgb[i].empty() || m_tex_widths[i] <= 0 || m_tex_heights[i] <= 0)
            continue;
        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_tex_widths[i], m_tex_heights[i],
                     0, GL_RGB, GL_UNSIGNED_BYTE, m_tex_pixels_rgb[i].data());
        m_gl_tex_ids[i] = tex_id;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void TexturePreviewCanvas::set_painted_mesh_data(
    const std::vector<std::array<float, 3>>& vertices,
    const std::vector<std::array<int, 3>>&   indices)
{
    m_painted_vertices = vertices;
    m_painted_indices  = indices;
    update_painted_bounding_box();
    m_painted_vbo_dirty = true;
    Refresh();
}

static void convert_face_colors(const std::vector<std::array<std::size_t, 3>>& src,
                                std::vector<std::array<float, 3>>& dst)
{
    dst.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i)
        dst[i] = { src[i][0] / 255.f, src[i][1] / 255.f, src[i][2] / 255.f };
}

void TexturePreviewCanvas::set_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors)
{
    convert_face_colors(face_colors, m_face_colors_rgb);
    m_painted_color_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::set_original_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors)
{
    convert_face_colors(face_colors, m_original_face_colors_rgb);
    m_orig_vbos_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::set_filament_color_map(
    const std::map<std::array<std::size_t, 3>, std::array<float, 3>>& color_map)
{
    m_color_map = color_map;
    m_filament_colors_rgb.resize(m_face_colors_rgb.size());
    for (size_t i = 0; i < m_face_colors_rgb.size(); ++i) {
        std::array<std::size_t, 3> key = {
            (std::size_t)(m_face_colors_rgb[i][0] * 255.f + 0.5f),
            (std::size_t)(m_face_colors_rgb[i][1] * 255.f + 0.5f),
            (std::size_t)(m_face_colors_rgb[i][2] * 255.f + 0.5f)
        };
        auto it = color_map.find(key);
        if (it != color_map.end())
            m_filament_colors_rgb[i] = it->second;
        else
            m_filament_colors_rgb[i] = m_face_colors_rgb[i];
    }
    m_painted_color_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::set_simplified_mode(RenderMode mode)
{
    if (mode == RenderMode::Original)
        return; // the left pane is permanently the original view
    if (m_mode != mode) {
        m_mode = mode;
        m_painted_color_dirty = true; // color source switches with the mode
        Refresh();
    }
}

void TexturePreviewCanvas::set_computing_overlay(bool show)
{
    // 2026-11 feedback: do NOT show the "Computing texture colors..." caption
    // over the preview — the modal ProgressDialog already communicates
    // progress. The flag is kept so callers don't change.
    if (m_computing_overlay == show)
        return;
    m_computing_overlay = show;
    Refresh();
}

void TexturePreviewCanvas::reset_view()
{
    m_zoom  = 1.0f;
    m_rot_x = 30.0f;
    m_rot_y = 30.0f;
    m_pan_x = 0.0f;
    m_pan_y = 0.0f;
    Refresh();
}

// The two panes as equal squares with a fixed gap between them (2026-11
// step-1 design): the gap shows the surrounding panel background, replacing
// the old full-bleed split with a drawn divider line.
std::pair<wxRect, wxRect> TexturePreviewCanvas::pane_rects() const
{
    wxSize sz = GetClientSize();
    const int gap = FromDIP(20);
    const int pane_w = std::max(0, (sz.x - gap) / 2);
    return { wxRect(0, 0, pane_w, sz.y),
             wxRect(pane_w + gap, 0, std::max(0, sz.x - pane_w - gap), sz.y) };
}

std::vector<wxRect> TexturePreviewCanvas::reset_overlay_rects() const
{
    const int button_size = FromDIP(40);
    const int margin = FromDIP(20);
    std::vector<wxRect> rects;
    const auto panes = pane_rects();
    for (const wxRect& pane : {panes.first, panes.second}) {
        if (pane.width <= 0 || pane.height <= 0)
            continue;
        rects.emplace_back(std::max(pane.GetLeft() + margin, pane.GetRight() - margin - button_size + 1),
                           std::max(pane.GetTop() + margin, pane.GetBottom() - margin - button_size + 1),
                           button_size,
                           button_size);
    }
    return rects;
}

unsigned int TexturePreviewCanvas::upload_reset_icon_texture(const std::string& icon_name)
{
    wxBitmap bmp = create_scaled_bitmap(icon_name, this, 40);
    if (!bmp.IsOk())
        return 0;

    wxImage image = bmp.ConvertToImage();
    if (!image.IsOk())
        return 0;

    const int w = image.GetWidth();
    const int h = image.GetHeight();
    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    if (!rgb || w <= 0 || h <= 0)
        return 0;

    std::vector<unsigned char> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        rgba[(size_t)i * 4 + 0] = rgb[i * 3 + 0];
        rgba[(size_t)i * 4 + 1] = rgb[i * 3 + 1];
        rgba[(size_t)i * 4 + 2] = rgb[i * 3 + 2];
        rgba[(size_t)i * 4 + 3] = alpha ? alpha[i] : 255;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex_id;
}

void TexturePreviewCanvas::upload_reset_icon_textures()
{
    if (m_reset_icon_tex && m_reset_icon_hover_tex && m_reset_icon_dark_tex && m_reset_icon_dark_hover_tex)
        return;

    // 2026-11: dedicated reset glyph (white circle + wireframe cube,
    // resources/images/texture_view_reset.svg); the canvas_zoom family stays
    // as fallback when the asset is unavailable.
    const unsigned int reset_tex = upload_reset_icon_texture("texture_view_reset");
    if (!m_reset_icon_tex)
        m_reset_icon_tex = reset_tex ? reset_tex : upload_reset_icon_texture("canvas_zoom");
    if (!m_reset_icon_hover_tex)
        m_reset_icon_hover_tex = reset_tex ? reset_tex : upload_reset_icon_texture("canvas_zoom_hover");
    if (!m_reset_icon_dark_tex)
        m_reset_icon_dark_tex = reset_tex ? reset_tex : upload_reset_icon_texture("canvas_zoom_dark");
    if (!m_reset_icon_dark_hover_tex)
        m_reset_icon_dark_hover_tex = reset_tex ? reset_tex : upload_reset_icon_texture("canvas_zoom_dark_hover");
}

bool TexturePreviewCanvas::handle_reset_overlay_mouse(wxMouseEvent& evt)
{
    if (evt.Leaving()) {
        if (m_reset_overlay_pressed) {
            m_reset_overlay_hovered = false;
            m_reset_overlay_pressed = false;
            SetCursor(wxCursor(wxCURSOR_ARROW));
            if (HasCapture())
                ReleaseMouse();
            Refresh();
            return true;
        }
        if (m_reset_overlay_hovered) {
            m_reset_overlay_hovered = false;
            SetCursor(wxCursor(wxCURSOR_ARROW));
            Refresh();
        }
        return false;
    }

    bool over = false;
    for (const wxRect& rc : reset_overlay_rects())
        over = over || rc.Contains(evt.GetPosition());
    if (over != m_reset_overlay_hovered) {
        m_reset_overlay_hovered = over;
        SetCursor(wxCursor(over ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    }

    if (m_drag_mode != DragMode::None && !m_reset_overlay_pressed)
        return false;

    if (evt.LeftDown() && over) {
        m_reset_overlay_pressed = true;
        if (!HasCapture())
            CaptureMouse();
        Refresh();
        return true;
    }

    if (evt.LeftUp() && m_reset_overlay_pressed) {
        const bool activate = over;
        m_reset_overlay_pressed = false;
        if (HasCapture())
            ReleaseMouse();
        if (activate)
            reset_view();
        else
            Refresh();
        return true;
    }

    return over;
}

void TexturePreviewCanvas::update_bounding_box()
{
    if (m_vertices.empty()) return;
    std::array<float, 3> mn = m_vertices[0], mx = m_vertices[0];
    for (const auto& v : m_vertices) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], v[i]);
            mx[i] = std::max(mx[i], v[i]);
        }
    }
    m_center = { (mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2 };
    float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
    m_radius = std::sqrt(dx*dx + dy*dy + dz*dz) / 2.0f;
    if (m_radius < 1e-6f) m_radius = 1.0f;
}

void TexturePreviewCanvas::ensure_gl_ready()
{
    if (m_gl_initialized) return;

    // BBS loads the GL entry points here with GLEW; Orca loads them centrally in
    // OpenGLManager, so only check that this has already happened (glad leaves unresolved
    // entry points null) and drain any stale error state.
    if (glGetString == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "TexturePreviewCanvas: OpenGL functions are not loaded yet";
        return;
    }
    while (glGetError() != GL_NO_ERROR) {}

    m_gl_initialized = true;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    GLfloat light_pos[]     = { 0.5f, 1.0f, 1.0f, 0.0f };
    GLfloat light_ambient[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    GLfloat light_diffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  light_diffuse);
}

void TexturePreviewCanvas::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    if (!m_context) return;
    SetCurrent(*m_context);
    ensure_gl_ready();
    render();
    SwapBuffers();
}

void TexturePreviewCanvas::on_size(wxSizeEvent&)
{
    Refresh();
}

void TexturePreviewCanvas::on_mouse(wxMouseEvent& evt)
{
    if (handle_reset_overlay_mouse(evt))
        return;

    if (evt.LeftDown()) {
        m_drag_mode = DragMode::Rotate;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.LeftUp()) {
        if (m_drag_mode == DragMode::Rotate) {
            m_drag_mode = DragMode::None;
            if (HasCapture()) ReleaseMouse();
        }
    }
    else if (evt.RightDown()) {
        m_drag_mode = DragMode::Pan;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.MiddleDown()) {
        m_drag_mode = DragMode::Pan;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.RightUp() || evt.MiddleUp()) {
        if (m_drag_mode == DragMode::Pan) {
            m_drag_mode = DragMode::None;
            if (HasCapture()) ReleaseMouse();
        }
    }
    else if (evt.Dragging() && m_drag_mode != DragMode::None) {
        wxPoint pos = evt.GetPosition();
        float dx = (float)(pos.x - m_last_mouse_pos.x);
        float dy = (float)(pos.y - m_last_mouse_pos.y);

        // The camera is shared by both halves, so a single update here drives
        // the whole split view — no mirroring pass needed.
        const float pan_radius = std::max(m_radius, m_painted_radius);
        if (m_drag_mode == DragMode::Rotate) {
            m_rot_y += dx * 0.5f;
            m_rot_x += dy * 0.5f;
            m_rot_x = std::max(-89.0f, std::min(89.0f, m_rot_x));
        } else if (m_drag_mode == DragMode::Pan) {
            wxSize sz = GetClientSize();
            if (sz.x > 0)
                m_pan_x += dx / (float)sz.x * pan_radius * 2.0f / m_zoom;
            if (sz.y > 0)
                m_pan_y -= dy / (float)sz.y * pan_radius * 2.0f / m_zoom;
        }

        m_last_mouse_pos = pos;
        Refresh();
    }
    else if (evt.GetWheelRotation() != 0) {
        float delta = evt.GetWheelRotation() > 0 ? 1.1f : 0.9f;
        m_zoom *= delta;
        m_zoom = std::max(0.1f, std::min(20.0f, m_zoom));
        Refresh();
    }
}

void TexturePreviewCanvas::setup_pane_projection(int vp_w, int vp_h, float radius, const std::array<float, 3>& center)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)vp_w / (float)vp_h;
    float dist = radius * 3.0f / m_zoom;
    float near_plane = dist * 0.01f;
    float far_plane  = dist * 10.0f;
    float fov_rad    = 45.0f * static_cast<float>(M_PI) / 180.0f;
    float f          = 1.0f / std::tan(fov_rad / 2.0f);
    float proj[16]   = {};
    proj[0]  = f / aspect;
    proj[5]  = f;
    proj[10] = (far_plane + near_plane) / (near_plane - far_plane);
    proj[11] = -1.0f;
    proj[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    glMultMatrixf(proj);

    // One shared camera; only the centering translation differs per pane.
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -dist);
    glTranslatef(m_pan_x, m_pan_y, 0.0f);
    glRotatef(m_rot_x, 1.0f, 0.0f, 0.0f);
    // Mesh data is Z-up like the 3D scene; map world Z to screen up and yaw around world Z.
    glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(m_rot_y, 0.0f, 0.0f, 1.0f);
    glTranslatef(-center[0], -center[1], -center[2]);
}

bool TexturePreviewCanvas::vbo_supported() const
{
    // GL 1.5 core entry points, resolved centrally by OpenGLManager like the
    // rest of the fixed-function calls this canvas uses.
    return glGenBuffers != nullptr && glBindBuffer != nullptr && glBufferData != nullptr;
}

void TexturePreviewCanvas::rebuild_original_vbos()
{
    if (!m_orig_vbos_dirty)
        return;
    m_orig_vbos_dirty = false;

    for (auto& g : m_orig_group_vbos)
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
    m_orig_group_vbos.clear();

    if (!vbo_supported() || m_vertices.empty() || m_indices.empty())
        return;

    const bool has_smooth = (m_vertex_normals.size() == m_vertices.size());
    const bool any_uv     = !m_face_uvs.empty() && m_face_uvs.size() == m_indices.size();

    // Faces are grouped by texture id; UVs are per-face-corner, so the corners
    // are expanded into a flat (non-indexed) stream per group.
    std::map<int, std::vector<size_t>> tex_groups;
    for (size_t fi = 0; fi < m_indices.size(); ++fi) {
        int tid = any_uv && fi < m_face_tex_ids.size() ? m_face_tex_ids[fi] : -1;
        bool tex_ok = tid >= 0 && tid < (int)m_tex_pixels_rgb.size() && !m_tex_pixels_rgb[tid].empty();
        tex_groups[tex_ok ? tid : -1].push_back(fi);
    }

    m_orig_group_vbos.reserve(tex_groups.size());
    for (const auto& [tid, face_list] : tex_groups) {
        const bool textured = (tid >= 0);
        // (pos3, nrm3, uv2) for textured groups; (pos3, nrm3, col3) otherwise.
        const size_t stride = textured ? 8 : 9;
        std::vector<float> buf;
        buf.reserve(face_list.size() * 3 * stride);

        for (size_t fi : face_list) {
            const auto& face = m_indices[fi];
            bool face_ok = true;
            for (int vi = 0; vi < 3; ++vi)
                if (face[vi] < 0 || face[vi] >= (int)m_vertices.size()) { face_ok = false; break; }
            if (!face_ok)
                continue;

            std::array<float, 3> nrm = {0.f, 1.f, 0.f};
            if (has_smooth) {
                nrm = m_vertex_normals[face[0]];
            } else {
                const auto& v0 = m_vertices[face[0]];
                const auto& v1 = m_vertices[face[1]];
                const auto& v2 = m_vertices[face[2]];
                float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (len > 1e-8f) { nrm = {nx/len, ny/len, nz/len}; }
            }

            // Per-face base color for untextured groups: same RGB on all corners.
            std::array<float, 3> face_rgb = {0.7f, 0.7f, 0.7f};
            if (!textured && fi < m_original_face_colors_rgb.size())
                face_rgb = m_original_face_colors_rgb[fi];

            for (int vi = 0; vi < 3; ++vi) {
                const auto& p = m_vertices[face[vi]];
                buf.insert(buf.end(), {p[0], p[1], p[2], nrm[0], nrm[1], nrm[2]});
                if (textured)
                    buf.insert(buf.end(), {m_face_uvs[fi][vi][0], m_face_uvs[fi][vi][1]});
                else
                    buf.insert(buf.end(), {face_rgb[0], face_rgb[1], face_rgb[2]});
            }
        }

        if (buf.empty())
            continue;

        OriginalGroupVBO g;
        g.corner_count = (int)(buf.size() / stride);
        g.has_uv       = textured;
        g.tex_index    = textured ? tid : -1;
        glGenBuffers(1, &g.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_orig_group_vbos.push_back(g);
    }
}

void TexturePreviewCanvas::rebuild_painted_vbos()
{
    if (!m_painted_vbo_dirty)
        return;
    m_painted_vbo_dirty = false;

    if (m_painted_vbo) { glDeleteBuffers(1, &m_painted_vbo); m_painted_vbo = 0; }
    m_painted_corner_count = 0;
    if (!vbo_supported() || m_painted_vertices.empty() || m_painted_indices.empty())
        return;

    // Flat-shaded corners: (pos3, face-normal3).
    std::vector<float> buf;
    buf.reserve(m_painted_indices.size() * 3 * 6);

    for (const auto& face : m_painted_indices) {
        bool face_ok = true;
        for (int vi = 0; vi < 3; ++vi)
            if (face[vi] < 0 || face[vi] >= (int)m_painted_vertices.size()) { face_ok = false; break; }
        if (!face_ok)
            continue;

        const auto& v0 = m_painted_vertices[face[0]];
        const auto& v1 = m_painted_vertices[face[1]];
        const auto& v2 = m_painted_vertices[face[2]];
        float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
        float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
        float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
        float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len < 1e-8f) { nx = 0.f; ny = 0.f; nz = 1.f; }
        else { nx /= len; ny /= len; nz /= len; }

        for (int vi = 0; vi < 3; ++vi) {
            const auto& p = m_painted_vertices[face[vi]];
            buf.insert(buf.end(), {p[0], p[1], p[2], nx, ny, nz});
        }
    }

    if (buf.empty())
        return;

    m_painted_corner_count = (int)(buf.size() / 6);
    glGenBuffers(1, &m_painted_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_painted_vbo);
    glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TexturePreviewCanvas::rebuild_painted_color_vbo()
{
    if (!m_painted_color_dirty)
        return;
    m_painted_color_dirty = false;

    if (m_painted_color_vbo) { glDeleteBuffers(1, &m_painted_color_vbo); m_painted_color_vbo = 0; }
    if (!vbo_supported() || m_painted_indices.empty())
        return;

    // Source follows the pane mode; sizes must match the geometry or the draw
    // path falls back to a flat gray.
    const bool use_filament = (m_mode == RenderMode::FilamentMap)
                              && m_filament_colors_rgb.size() == m_painted_indices.size();
    const auto& colors = use_filament ? m_filament_colors_rgb : m_face_colors_rgb;
    if (colors.size() != m_painted_indices.size())
        return;

    std::vector<float> buf;
    buf.reserve(colors.size() * 9);
    for (const auto& c : colors)
        buf.insert(buf.end(), {c[0], c[1], c[2], c[0], c[1], c[2], c[0], c[1], c[2]});

    glGenBuffers(1, &m_painted_color_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_painted_color_vbo);
    glBufferData(GL_ARRAY_BUFFER, buf.size() * sizeof(float), buf.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TexturePreviewCanvas::render()
{
    wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    wxSize viewport_sz = gl_viewport_size(this, sz);
    const float scale_x = (float)viewport_sz.x / (float)sz.x;
    const float scale_y = (float)viewport_sz.y / (float)sz.y;

    upload_textures();
    rebuild_original_vbos();
    rebuild_painted_vbos();
    rebuild_painted_color_vbo();

    // The canvas surround is the panel background; each pane clears to the
    // preview grey inside its own scissor rect, so the gap between the panes
    // reads as the design's visual split instead of a drawn divider line.
    const wxColour surround_clr = StateColor::darkModeColorFor(*wxWHITE);
    glClearColor(surround_clr.Red() / 255.f, surround_clr.Green() / 255.f, surround_clr.Blue() / 255.f, 1.0f);

    glViewport(0, 0, viewport_sz.x, viewport_sz.y);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Pane placeholder tone: same plate as the mixed-color-match preview
    // panel (#E7E7E7, dark #54545B). #F5F5F5 has NO dark-mode mapping and
    // stayed light grey in the dark theme (2026-11 feedback).
    const wxColour pane_clr = StateColor::darkModeColorFor(wxColour("#E7E7E7"));
    const auto panes = pane_rects();
    auto draw_pane = [&, this](const wxRect& pane, float radius, const std::array<float, 3>& center, auto&& draw_fn) {
        if (pane.width <= 0 || pane.height <= 0) return;
        const int x = (int)std::lround(pane.x * scale_x);
        const int y = viewport_sz.y - (int)std::lround((pane.y + pane.height) * scale_y);
        const int w = (int)std::lround(pane.width * scale_x);
        const int h = (int)std::lround(pane.height * scale_y);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w, h);
        glViewport(x, y, w, h);
        glClearColor(pane_clr.Red() / 255.f, pane_clr.Green() / 255.f, pane_clr.Blue() / 255.f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        setup_pane_projection(w, h, radius, center);
        draw_fn();
    };

    // ---- Left pane: original textured model ----
    draw_pane(panes.first, m_radius, m_center, [this]() { draw_original_pane(); });

    // ---- Right pane: simplified multi-color / filament-mapped result ----
    draw_pane(panes.second, m_painted_radius, m_painted_center, [this]() { draw_simplified_pane(); });

    // ---- Overlays span the full canvas: rounded pane corners, badges, reset buttons ----
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, viewport_sz.x, viewport_sz.y);
    render_pane_rounding(viewport_sz, scale_x, scale_y);
    render_pane_badges(sz, viewport_sz);
    render_reset_overlay(sz, viewport_sz);
}

void TexturePreviewCanvas::draw_original_pane()
{
    if (m_vertices.empty() || m_indices.empty())
        return;

    if (!m_orig_group_vbos.empty()) {
        draw_original_vbos();
        return;
    }

    // Fallback for drivers without VBO support.
    if (!m_face_uvs.empty())
        render_textured_original();
    else
        render_mesh(RenderMode::Original);
}

void TexturePreviewCanvas::draw_simplified_pane()
{
    if (m_painted_vertices.empty() || m_painted_indices.empty())
        return;

    if (m_painted_vbo != 0) {
        draw_painted_vbos();
        return;
    }

    render_mesh(m_mode); // immediate-mode fallback
}

void TexturePreviewCanvas::draw_original_vbos()
{
    glEnable(GL_LIGHTING);

    for (const auto& g : m_orig_group_vbos) {
        const bool textured = g.has_uv
            && g.tex_index >= 0 && g.tex_index < (int)m_gl_tex_ids.size()
            && m_gl_tex_ids[g.tex_index] != 0;

        if (textured) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, m_gl_tex_ids[g.tex_index]);
            glColor3f(1.0f, 1.0f, 1.0f);
        } else {
            glDisable(GL_TEXTURE_2D);
        }

        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        if (g.has_uv) {
            const GLsizei stride = 8 * sizeof(float);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_NORMAL_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glVertexPointer(3, GL_FLOAT, stride, (const void*)0);
            glNormalPointer(GL_FLOAT, stride, (const void*)(3 * sizeof(float)));
            glTexCoordPointer(2, GL_FLOAT, stride, (const void*)(6 * sizeof(float)));
        } else {
            const GLsizei stride = 9 * sizeof(float);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_NORMAL_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glVertexPointer(3, GL_FLOAT, stride, (const void*)0);
            glNormalPointer(GL_FLOAT, stride, (const void*)(3 * sizeof(float)));
            glColorPointer(3, GL_FLOAT, stride, (const void*)(6 * sizeof(float)));
        }
        glDrawArrays(GL_TRIANGLES, 0, g.corner_count);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        if (textured) {
            glDisable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
}

void TexturePreviewCanvas::draw_painted_vbos()
{
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);

    glBindBuffer(GL_ARRAY_BUFFER, m_painted_vbo);
    const GLsizei stride = 6 * sizeof(float);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, stride, (const void*)0);
    glNormalPointer(GL_FLOAT, stride, (const void*)(3 * sizeof(float)));

    if (m_painted_color_vbo != 0) {
        glBindBuffer(GL_ARRAY_BUFFER, m_painted_color_vbo);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(3, GL_FLOAT, 0, (const void*)0);
        glBindBuffer(GL_ARRAY_BUFFER, m_painted_vbo);
    } else {
        glColor3f(0.7f, 0.7f, 0.7f);
    }

    glDrawArrays(GL_TRIANGLES, 0, m_painted_corner_count);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
}

void TexturePreviewCanvas::render_pane_rounding(const wxSize& viewport_size, float scale_x, float scale_y)
{
    if (viewport_size.x <= 0 || viewport_size.y <= 0)
        return;

    const wxColour surround = StateColor::darkModeColorFor(*wxWHITE);
    const float r = (float)FromDIP(8) * scale_x;
    if (r < 1.0f)
        return;

    glPushAttrib(GL_ENABLE_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, viewport_size.x, viewport_size.y, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor3ub(surround.Red(), surround.Green(), surround.Blue());

    const auto panes = pane_rects();
    for (const wxRect* pane : {&panes.first, &panes.second}) {
        if (pane->width <= 0 || pane->height <= 0)
            continue;
        const float x0 = pane->x * scale_x,            y0 = pane->y * scale_y;
        const float x1 = (pane->x + pane->width) * scale_x, y1 = (pane->y + pane->height) * scale_y;
        // Rounded-rect corner: the kept pane is bounded by a quarter arc of
        // radius r CENTRED AT THE INSET POINT (corner + r along the inward
        // diagonal). The mask is the corner-square region OUTSIDE that arc —
        // fan it from the corner point; the arc must bow TOWARD the corner.
        // (Arcs centred on the corner itself bow the other way and produce the
        // inverted, concave-looking corner.)
        struct Corner { float cx, cy, ix, iy, a0, a1; };
        const float PI_F = (float)M_PI;
        const Corner corners[4] = {
            { x0, y0, +1.f, +1.f, PI_F,        1.5f * PI_F }, // TL: arc centre (x0+r, y0+r)
            { x1, y0, -1.f, +1.f, 1.5f * PI_F, 2.0f * PI_F }, // TR: arc centre (x1-r, y0+r)
            { x1, y1, -1.f, -1.f, 0.0f,        0.5f * PI_F }, // BR: arc centre (x1-r, y1-r)
            { x0, y1, +1.f, -1.f, 0.5f * PI_F, PI_F }        // BL: arc centre (x0+r, y1-r)
        };
        const int segments = 12;
        for (const Corner& c : corners) {
            const float pcx = c.cx + c.ix * r;
            const float pcy = c.cy + c.iy * r;
            glBegin(GL_TRIANGLE_FAN);
            glVertex2f(c.cx, c.cy); // corner point
            for (int i = 0; i <= segments; ++i) {
                const float a = c.a0 + (c.a1 - c.a0) * (float)i / (float)segments;
                glVertex2f(pcx + r * std::cos(a), pcy + r * std::sin(a));
            }
            glEnd();
        }
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

// Renders ONLY the badge caption as a coverage mask: white glyphs on a black
// 24bpp bitmap, luminance promoted to alpha. The translucent pill itself is
// drawn in GL (render_pane_badges), so no GDI+/DIB alpha premultiplication is
// involved — that path painted dark fringes around the glyphs.
unsigned int TexturePreviewCanvas::upload_badge_texture(const wxString& text)
{
    if (text.empty())
        return 0;

    wxFont font = GetFont();
    font.SetPointSize(texture_font_pt(9));
    wxClientDC measure_dc(this);
    measure_dc.SetFont(font);
    const wxSize tsz = measure_dc.GetTextExtent(text);
    if (tsz.x <= 0 || tsz.y <= 0)
        return 0;

    wxBitmap bmp(tsz.x, tsz.y, 24);
    {
        wxMemoryDC dc(bmp);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        dc.SetFont(font);
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(text, 0, 0);
    }

    wxImage image = bmp.ConvertToImage();
    const unsigned char* data = image.GetData();
    if (!image.IsOk() || !data || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0;

    const int n = image.GetWidth() * image.GetHeight();
    std::vector<unsigned char> rgba((size_t)n * 4);
    for (int i = 0; i < n; ++i) {
        // White text on black: the red channel IS the glyph coverage.
        rgba[(size_t)i * 4 + 0] = 255;
        rgba[(size_t)i * 4 + 1] = 255;
        rgba[(size_t)i * 4 + 2] = 255;
        rgba[(size_t)i * 4 + 3] = data[i * 3];
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.GetWidth(), image.GetHeight(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex_id;
}

void TexturePreviewCanvas::set_badge_labels(const wxString& left, const wxString& right)
{
    m_badge_left_text  = left;
    m_badge_right_text = right;
    m_badge_tex_dirty  = true;
    Refresh();
}

void TexturePreviewCanvas::render_pane_badges(const wxSize& logical_size, const wxSize& viewport_size)
{
    if (logical_size.x <= 0 || logical_size.y <= 0 || viewport_size.x <= 0 || viewport_size.y <= 0)
        return;

    if (m_badge_tex_dirty) {
        if (m_badge_left_tex)  { glDeleteTextures(1, &m_badge_left_tex);  m_badge_left_tex = 0; }
        if (m_badge_right_tex) { glDeleteTextures(1, &m_badge_right_tex); m_badge_right_tex = 0; }
        m_badge_left_tex  = upload_badge_texture(m_badge_left_text);
        m_badge_right_tex = upload_badge_texture(m_badge_right_text);
        m_badge_tex_dirty = false;
    }

    struct Badge { unsigned int tex; wxPoint pos; };
    const auto panes = pane_rects();
    // 12px inset keeps clear daylight between the pill and the white corner
    // masks (which reach 8px along the pane edges).
    const wxPoint inset(FromDIP(12), FromDIP(12));
    const Badge badges[2] = {
        { m_badge_left_tex,  panes.first.GetTopLeft() + inset },
        { m_badge_right_tex, panes.second.GetTopLeft() + inset }
    };

    const float sx = (float)viewport_size.x / (float)logical_size.x;
    const float sy = (float)viewport_size.y / (float)logical_size.y;
    const int pad_x = FromDIP(8);
    const int pad_y = FromDIP(4);
    const float pill_r = (float)FromDIP(4) * sx;

    // Filled rounded rectangle as a fan from the rect centre (the shape is
    // convex, so a centre fan tiles it exactly).
    auto draw_rounded_rect = [](float x, float y, float w, float h, float r) {
        r = std::min(r, std::min(w, h) / 2.0f);
        auto pt = [&](int cx, int cy, float a) {
            return std::make_pair(cx + r * std::cos(a), cy + r * std::sin(a));
        };
        const int segments = 6;
        auto arc = [&](int cx, int cy, float a0, float a1) {
            for (int i = 0; i <= segments; ++i) {
                const float a = a0 + (a1 - a0) * (float)i / (float)segments;
                const auto p = pt(cx, cy, a);
                glVertex2f(p.first, p.second);
            }
        };
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(x + w / 2.0f, y + h / 2.0f);
        glVertex2f(x, y + r);
        arc((int)(x + r), (int)(y + r),     (float)M_PI, 1.5f * (float)M_PI); // TL
        glVertex2f(x + w - r, y);
        arc((int)(x + w - r), (int)(y + r), -0.5f * (float)M_PI, 0.0f);       // TR
        glVertex2f(x + w, y + h - r);
        arc((int)(x + w - r), (int)(y + h - r), 0.0f, 0.5f * (float)M_PI);    // BR
        glVertex2f(x + r, y + h);
        arc((int)(x + r), (int)(y + h - r), 0.5f * (float)M_PI, (float)M_PI); // BL
        // A triangle fan does NOT close back to its first rim vertex: without
        // this duplicate the wedge between the two left-edge rays (centre to
        // (x,y+r) and centre to (x,y+h-r)) stays unfilled — a grey triangle
        // over the pill's left half, right across the first caption glyph.
        glVertex2f(x, y + r);
        glEnd();
    };

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, viewport_size.x, viewport_size.y, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    for (const Badge& badge : badges) {
        if (!badge.tex)
            continue;
        int tw = 0, th = 0;
        glBindTexture(GL_TEXTURE_2D, badge.tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        if (tw <= 0 || th <= 0)
            continue;

        const float w = (tw + 2 * pad_x) * sx;
        const float h = (th + 2 * pad_y) * sy;
        const float x0 = badge.pos.x * sx;
        const float y0 = badge.pos.y * sy;

        // Pill: rgba(0,0,0,0.4) rounded rect drawn directly in GL.
        glDisable(GL_TEXTURE_2D);
        glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
        draw_rounded_rect(x0, y0, w, h, pill_r);

        // Caption: white modulated by the glyph-coverage alpha texture.
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        const float tx0 = x0 + pad_x * sx;
        const float ty0 = y0 + pad_y * sy;
        const float tx1 = tx0 + tw * sx;
        const float ty1 = ty0 + th * sy;
        glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(tx0, ty0);
            glTexCoord2f(1.0f, 0.0f); glVertex2f(tx1, ty0);
            glTexCoord2f(1.0f, 1.0f); glVertex2f(tx1, ty1);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(tx0, ty1);
        glEnd();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glBindTexture(GL_TEXTURE_2D, 0);
    glPopAttrib();
}

void TexturePreviewCanvas::update_painted_bounding_box()
{
    if (m_painted_vertices.empty()) {
        m_painted_radius = 1.0f;
        m_painted_center = {0.f, 0.f, 0.f};
        return;
    }
    std::array<float, 3> mn = m_painted_vertices[0], mx = m_painted_vertices[0];
    for (const auto& v : m_painted_vertices) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], v[i]);
            mx[i] = std::max(mx[i], v[i]);
        }
    }
    m_painted_center = { (mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2 };
    float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
    m_painted_radius = std::sqrt(dx*dx + dy*dy + dz*dz) / 2.0f;
    if (m_painted_radius < 1e-6f) m_painted_radius = 1.0f;
}

void TexturePreviewCanvas::render_reset_overlay(const wxSize& logical_size, const wxSize& viewport_size)
{
    if (logical_size.x <= 0 || logical_size.y <= 0 || viewport_size.x <= 0 || viewport_size.y <= 0)
        return;

    upload_reset_icon_textures();

    const unsigned int tex_id = is_dark() ? m_reset_icon_dark_tex : m_reset_icon_tex;
    const unsigned int tex_id_hover = is_dark() ? m_reset_icon_dark_hover_tex : m_reset_icon_hover_tex;
    if (!tex_id)
        return;

    const float sx = (float)viewport_size.x / (float)logical_size.x;
    const float sy = (float)viewport_size.y / (float)logical_size.y;

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, viewport_size.x, viewport_size.y, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // One reset button per pane; the hovered one (if any) gets the hover icon
    // and full opacity, the other stays dimmed.
    const wxPoint mouse = ScreenToClient(wxGetMousePosition());
    for (const wxRect& rc : reset_overlay_rects()) {
        const bool hovered = rc.Contains(mouse) && m_reset_overlay_hovered;
        const unsigned int rect_tex = (hovered && tex_id_hover) ? tex_id_hover : tex_id;
        if (!rect_tex)
            continue;
        glBindTexture(GL_TEXTURE_2D, rect_tex);
        const float alpha = hovered ? 1.0f : 0.78f;
        const float x0 = rc.GetLeft() * sx;
        const float y0 = rc.GetTop() * sy;
        const float x1 = (rc.GetLeft() + rc.GetWidth()) * sx;
        const float y1 = (rc.GetTop() + rc.GetHeight()) * sy;

        glColor4f(1.0f, 1.0f, 1.0f, alpha);
        glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
            glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
            glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
        glEnd();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glBindTexture(GL_TEXTURE_2D, 0);
    glPopAttrib();
}

void TexturePreviewCanvas::render_textured_original()
{
    if (m_vertices.empty() || m_indices.empty()) return;
    if (m_face_uvs.empty() || m_face_tex_ids.empty()) return;
    if (m_face_uvs.size() != m_indices.size()) return;

    upload_textures();

    const bool has_smooth = (m_vertex_normals.size() == m_vertices.size());

    // Group faces by texture id for batch rendering
    std::map<int, std::vector<size_t>> tex_groups;
    for (size_t fi = 0; fi < m_indices.size(); ++fi) {
        int tid = (fi < m_face_tex_ids.size()) ? m_face_tex_ids[fi] : -1;
        tex_groups[tid].push_back(fi);
    }

    glEnable(GL_LIGHTING);
    glColor3f(1.0f, 1.0f, 1.0f);

    for (const auto& [tid, face_list] : tex_groups) {
        bool tex_bound = false;
        if (tid >= 0 && tid < (int)m_gl_tex_ids.size() && m_gl_tex_ids[tid] != 0) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, m_gl_tex_ids[tid]);
            tex_bound = true;
            // Reset to white per textured group: an earlier untextured group
            // leaves glColor at its last face color, and GL_MODULATE would tint
            // the texture with it (green x red = black).
            glColor3f(1.0f, 1.0f, 1.0f);
        } else {
            glDisable(GL_TEXTURE_2D);
        }

        glBegin(GL_TRIANGLES);
        for (size_t fi : face_list) {
            const auto& face = m_indices[fi];
            const auto& uvs  = m_face_uvs[fi];

            if (!tex_bound) {
                if (fi < m_original_face_colors_rgb.size())
                    glColor3fv(m_original_face_colors_rgb[fi].data());
                else
                    glColor3f(0.7f, 0.7f, 0.7f);
            }

            for (int vi = 0; vi < 3; ++vi) {
                int idx = face[vi];
                if (idx < 0 || idx >= (int)m_vertices.size()) continue;

                if (has_smooth) {
                    glNormal3fv(m_vertex_normals[idx].data());
                } else if (vi == 0) {
                    const auto& v0 = m_vertices[face[0]];
                    const auto& v1 = m_vertices[face[1]];
                    const auto& v2 = m_vertices[face[2]];
                    float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                    float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                    float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                    float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                    if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
                    glNormal3f(nx, ny, nz);
                }

                if (tex_bound)
                    glTexCoord2fv(uvs[vi].data());
                glVertex3fv(m_vertices[idx].data());
            }
        }
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void TexturePreviewCanvas::render_mesh(RenderMode mode)
{
    // Immediate-mode fallback (drivers without VBO support). `mode` selects the
    // pane content: Original draws the input mesh, MultiColor/FilamentMap draw
    // the remeshed geometry.
    const bool use_painted = (mode != RenderMode::Original)
                             && !m_painted_vertices.empty()
                             && !m_painted_indices.empty();
    if (!use_painted && (m_vertices.empty() || m_indices.empty())) return;

    // Original mode with texture data: use proper texture mapping
    if (mode == RenderMode::Original && !m_face_uvs.empty()) {
        render_textured_original();
        return;
    }

    // For Multi-Color / FilamentMap, use the painted (remeshed) geometry if available;
    // the face color arrays match the painted mesh, not the original mesh.
    const auto& verts   = use_painted ? m_painted_vertices : m_vertices;
    const auto& faces   = use_painted ? m_painted_indices  : m_indices;

    const std::vector<std::array<float, 3>>* colors_ptr = nullptr;
    if (mode == RenderMode::Original && !m_original_face_colors_rgb.empty()
        && m_original_face_colors_rgb.size() == m_indices.size()) {
        colors_ptr = &m_original_face_colors_rgb;
    } else if (mode == RenderMode::FilamentMap && !m_filament_colors_rgb.empty()
               && m_filament_colors_rgb.size() == faces.size()) {
        colors_ptr = &m_filament_colors_rgb;
    } else if (!m_face_colors_rgb.empty() && m_face_colors_rgb.size() == faces.size()) {
        colors_ptr = &m_face_colors_rgb;
    }

    // Use smooth normals for the original mesh when available
    const bool has_smooth = !use_painted
                            && (m_vertex_normals.size() == m_vertices.size());

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);

    glBegin(GL_TRIANGLES);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        if (colors_ptr)
            glColor3fv((*colors_ptr)[fi].data());
        else
            glColor3f(0.7f, 0.7f, 0.7f);

        const auto& face = faces[fi];
        for (int vi = 0; vi < 3; ++vi) {
            int idx = face[vi];
            if (idx < 0 || idx >= (int)verts.size()) continue;

            if (has_smooth && idx < (int)m_vertex_normals.size()) {
                glNormal3fv(m_vertex_normals[idx].data());
            } else if (vi == 0) {
                const auto& v0 = verts[face[0]];
                const auto& v1 = verts[face[1]];
                const auto& v2 = verts[face[2]];
                float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
                glNormal3f(nx, ny, nz);
            }

            glVertex3fv(verts[idx].data());
        }
    }
    glEnd();
}





// ============================================================
// AddMaterialDialog — "+ Add Material" (doc §3.3.4). Top: ComboBox
// selecting one of the material families the user checked on the
// system-materials page. Below: the official filament color panel —
// same swatch icons, selection ring, "Other Colors" row and button
// styling as the fork's FilamentColorDialog, embedded here so the
// shared dialog itself stays untouched.
// ============================================================

static wxString add_material_color_name(const Slic3r::FilamentColorItem& item)
{
    std::string lang = "en";
    if (wxGetApp().app_config) {
        const std::string configured = wxGetApp().app_config->get("language");
        if (!configured.empty())
            lang = (configured == "zh-cn") ? "zh_CN" : configured;
    }
    auto it = item.colorNames.find(lang);
    if (it != item.colorNames.end() && !it->second.empty())
        return wxString::FromUTF8(it->second);
    it = item.colorNames.find("en");
    if (it != item.colorNames.end() && !it->second.empty())
        return wxString::FromUTF8(it->second);
    return wxString::FromUTF8(item.colorData.PrimaryColor());
}

// Color swatch painted from a bitmap rendered ONCE at construction (the
// official multi-color icon renderer) plus the panel's teal selection ring —
// per-paint work stays a single DrawBitmap so dialog repaints never churn.
class AddMaterialSwatch : public wxPanel
{
public:
    AddMaterialSwatch(wxWindow* parent, const Slic3r::FilamentColorItem& item, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_item(&item)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        const wxString name = add_material_color_name(item);
        if (!name.empty())
            SetToolTip(name);
        Bind(wxEVT_PAINT, &AddMaterialSwatch::OnPaint, this);
    }

    void SetSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, size.x, size.y);
        if (m_item == nullptr)
            return;

        const int border = std::max(FromDIP(1), 2);
        const int inset  = border + FromDIP(1);
        const int w = std::max(1, size.GetWidth() - inset * 2);
        const int h = std::max(1, size.GetHeight() - inset * 2);

        // Render once, cache, redraw from cache (no per-paint rasterization).
        if (!m_bitmap.IsOk() || m_bitmap.GetWidth() != w || m_bitmap.GetHeight() != h) {
            wxBitmap* rendered = Slic3r::GUI::FilamentColorUtils::GetFilamentColorIcon(
                m_item->colorData.colors, m_item->colorData.NormalizedMode(), "", w, h);
            if (rendered != nullptr)
                m_bitmap = *rendered;
        }
        if (m_bitmap.IsOk())
            dc.DrawBitmap(m_bitmap, inset, inset, true);

        if (m_selected) {
            const int middle = std::max(0, size.GetHeight() - border * 2);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#019687")));
            dc.DrawRectangle(0, 0, size.GetWidth(), border);
            dc.DrawRectangle(0, border, border, middle);
            dc.DrawRectangle(size.GetWidth() - border, border, border, middle);
            dc.DrawRectangle(0, size.GetHeight() - border, size.GetWidth(), border);
        }
    }

    const Slic3r::FilamentColorItem* m_item = nullptr;
    wxBitmap m_bitmap;
    bool m_selected = false;
};

class AddMaterialDialog : public DPIDialog
{
public:
    struct Family {
        std::string              preset_name;
        wxString                 label;
        Slic3r::FilamentColorInfo info;
        bool                     official = false;
    };

    AddMaterialDialog(wxWindow* parent, std::vector<Family> families, int default_family)
        : DPIDialog(parent, wxID_ANY, _L("+ Add Material"),
                    wxDefaultPosition, wxDefaultSize,
                    (wxDEFAULT_DIALOG_STYLE) & ~(wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER))
        , m_families(std::move(families))
        , m_current_family(std::clamp(default_family, 0, std::max(0, (int)m_families.size() - 1)))
    {
        build_ui();
        wxGetApp().UpdateDlgDarkUI(this);
        refit();
        CenterOnParent();
    }

    const std::string& preset_name() const { return m_selected_preset; }
    const std::string& color_hex() const { return m_selected_hex; }
    std::array<float, 4> color_rgba() const { return parse_color_string(m_selected_hex); }

private:
    void on_dpi_changed(const wxRect&) override { refit(); }

    void refit()
    {
        // Clear the min size before fitting: a MinSize latched from a taller
        // state (family with more colors) blocked Fit() from shrinking when
        // switching to a smaller family, stranding white space below the
        // Cancel/OK row instead of keeping them pinned to the bottom edge.
        SetMinSize(wxDefaultSize);
        Fit();
        SetMinSize(GetSize());
        // Growing/shrinking content (official grid on/off) must never push
        // the Cancel/OK row off-screen: keep the whole dialog inside the
        // display work area after every size change (2026-11 feedback).
        int display_idx = wxDisplay::GetFromWindow(this);
        if (display_idx == wxNOT_FOUND)
            display_idx = wxDisplay::GetFromPoint(GetPosition());
        wxRect work = display_idx != wxNOT_FOUND
            ? wxDisplay(display_idx).GetClientArea()
            : wxDisplay().GetClientArea();
        wxPoint pos = GetPosition();
        pos.x = std::clamp(pos.x, work.GetLeft(),
            std::max(work.GetLeft(), work.GetRight()  - GetSize().x));
        pos.y = std::clamp(pos.y, work.GetTop(),
            std::max(work.GetTop(),   work.GetBottom() - GetSize().y));
        Move(pos);
    }

    void build_ui()
    {
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));

        // 2026-11 Figma "官方耗材" card: 295 wide, 16px margins (263 content).
        m_dialog_width   = FromDIP(295);
        m_content_margin = FromDIP(16);
        m_content_width  = m_dialog_width - m_content_margin * 2;

        auto* root = new wxBoxSizer(wxVERTICAL);

        // ---- Header row: "选择耗材" + 199x32 family selector chip ----
        auto* header_row = new wxBoxSizer(wxHORIZONTAL);
        auto* select_lbl = new wxStaticText(this, wxID_ANY, _L("Select Filament"));
        {
            wxFont sf = select_lbl->GetFont();
            sf.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            sf.SetWeight(wxFONTWEIGHT_MEDIUM);
            select_lbl->SetFont(sf);
        }
        select_lbl->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#18181B")));
        header_row->Add(select_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        // Custom trigger chip (199x32, border #DBDBDB) + bounded downward
        // popup: the shared ComboBox/DropDown list cannot be height-capped
        // (up to 15 rows) and grew far past the card (2026-11 feedback).
        m_family_trigger = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                       wxSize(FromDIP(199), FromDIP(32)), wxTAB_TRAVERSAL);
        m_family_trigger->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        m_family_trigger->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_family_trigger->SetCursor(wxCursor(wxCURSOR_HAND));
        m_family_trigger->SetToolTip(_L("Material type of the new filament"));
        m_family_trigger->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(p->GetBackgroundColour()));
            dc.Clear();
            dc.SetBrush(wxBrush(p->GetBackgroundColour()));
            texture_draw_edge_border(dc, sz, StateColor::darkModeColorFor(wxColour("#DBDBDB")));

            wxFont f = p->GetFont();
            f.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            f.SetWeight(wxFONTWEIGHT_MEDIUM);
            dc.SetFont(f);
            dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#18181B")));
            const wxString label = m_families.empty() ? wxString()
                                                      : m_families[m_current_family].label;
            const wxString shown = ellipsize_text(dc, label, sz.x - p->FromDIP(8) - p->FromDIP(20) - p->FromDIP(16));
            const wxSize tsz = dc.GetTextExtent(shown);
            dc.DrawText(shown, p->FromDIP(8), (sz.y - tsz.y) / 2);

            texture_draw_glyph(dc, p, sz.x - p->FromDIP(22),
                (sz.y - p->FromDIP(16)) / 2.0,
                StateColor::darkModeColorFor(wxColour("#949494")));
        });
        m_family_trigger->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_family_popup) {
                m_family_popup->Dismiss();
                return;
            }
            show_family_popup();
        });
        header_row->Add(m_family_trigger, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(header_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, m_content_margin);
        root->AddSpacer(FromDIP(20)); // ColorPresetGrid gap (2026-11 Figma)

        // ---- Current-selection preview row (60x60 icon + name/sku) ----
        m_preview_panel = new wxPanel(this, wxID_ANY);
        m_preview_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_preview_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_preview_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            const int side = FromDIP(60);
            // Cached bitmap: re-rendered only when the selected color changes.
            if (!m_preview_bitmap.IsOk() || m_preview_bitmap_hex != m_selected_hex) {
                m_preview_bitmap_hex = m_selected_hex.empty() ? std::string("#FFFFFF") : m_selected_hex;
                wxBitmap* rendered = Slic3r::GUI::FilamentColorUtils::GetFilamentColorIcon(
                    {m_preview_bitmap_hex}, Slic3r::FilamentColorMode::Segment, "", side, side);
                if (rendered != nullptr)
                    m_preview_bitmap = *rendered;
            }
            if (m_preview_bitmap.IsOk())
                dc.DrawBitmap(m_preview_bitmap, 0, (sz.y - side) / 2, true);
        });
        // Only as wide as the 60px swatch itself: the old full-content-width
        // panel pushed the name/sku column to the right edge (2026-11 feedback).
        m_preview_panel->SetMinSize(wxSize(FromDIP(60), FromDIP(60)));

        auto* info_sizer = new wxBoxSizer(wxVERTICAL);
        m_name_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        wxFont name_font = m_name_label->GetFont().Bold();
        name_font.SetPointSize(texture_font_pt(10));
        m_name_label->SetFont(name_font);
        m_name_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        m_sku_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        {
            wxFont sf = m_sku_label->GetFont();
            sf.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            sf.SetWeight(wxFONTWEIGHT_MEDIUM);
            m_sku_label->SetFont(sf);
        }
        m_sku_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#18181B")));
        info_sizer->Add(m_name_label, 0, wxBOTTOM, FromDIP(6));
        info_sizer->Add(m_sku_label, 0);

        auto* preview_row = new wxBoxSizer(wxHORIZONTAL);
        preview_row->Add(m_preview_panel, 0, wxRIGHT, FromDIP(12));
        preview_row->Add(info_sizer, 1, wxALIGN_CENTER_VERTICAL);
        // Saved as items so the whole preview block collapses for
        // non-official families (2026-11: stale preview / leftover gap).
        m_preview_item = root->Add(preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT, m_content_margin);
        m_preview_gap = root->AddSpacer(FromDIP(20));

        // ---- "Official Filaments" section ----
        m_official_label = new wxStaticText(this, wxID_ANY, _L("Official Filaments"));
        wxFont of = m_official_label->GetFont();
        of.SetPointSize(texture_font_pt(10));
        of.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_official_label->SetFont(of);
        m_official_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#18181B")));

        root->Add(m_official_label, 0, wxLEFT | wxRIGHT, m_content_margin);

        m_swatch_panel = new wxPanel(this, wxID_ANY);
        m_swatch_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_swatch_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_swatch_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
        m_swatch_grid = new wxGridSizer(0, 8, 0, FromDIP(6));
        m_swatch_panel->SetSizer(m_swatch_grid);
        m_official_gap1 = root->AddSpacer(FromDIP(20));
        root->Add(m_swatch_panel, 0, wxLEFT | wxRIGHT, m_content_margin);

        // ---- "Other Colors" row (gray band like MoreColorPanel) ----
        m_other_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                    wxSize(m_content_width, FromDIP(40)));
        m_other_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_other_panel->SetCursor(wxCursor(wxCURSOR_HAND));
        m_other_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#EBEBEB"))));
            dc.Clear();
            wxFont f = p->GetFont();
            f.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
            f.SetWeight(wxFONTWEIGHT_MEDIUM);
            dc.SetFont(f);
            dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#18181B")));
            // Plus glyph + text, centered (Figma "其他颜色" pill).
            const wxString label = _L("Other Colors");
            wxCoord tw = 0, th = 0;
            dc.GetTextExtent(label, &tw, &th);
            // Figma 16x16 plus icon: two 1.4px strokes crossing at the centre.
            const int glyph = p->FromDIP(10);
            const int total = glyph + p->FromDIP(6) + tw;
            int x = std::max(0, (sz.x - total) / 2);
            const int cy = sz.y / 2;
            const int cx = x + glyph / 2;
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#71717A")), p->FromDIP(2)));
            dc.DrawLine(cx - glyph / 2, cy, cx + glyph / 2, cy);
            dc.DrawLine(cx, cy - glyph / 2, cx, cy + glyph / 2);
            dc.DrawText(label, x + glyph + p->FromDIP(6), std::max(0, (sz.y - th) / 2));
        });
        m_other_panel->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            wxColourData cd;
            cd.SetColour(wxColour(0, 0, 0));
            cd.SetChooseFull(true);
            wxColourDialog dlg(this, &cd);
            if (dlg.ShowModal() != wxID_OK)
                return;
            const wxColour clr = dlg.GetColourData().GetColour();
            m_selected_hex = wxString::Format("#%02X%02X%02X",
                clr.Red(), clr.Green(), clr.Blue()).ToStdString();
            m_selected_name = _L("Other Colors");
            m_selected_sku.clear();
            update_selection_ui();
        });
        m_official_gap2 = root->AddSpacer(FromDIP(20));
        root->Add(m_other_panel, 0, wxLEFT | wxRIGHT, m_content_margin);

        // ---- Divider + Cancel/OK (official button styling) ----
        root->AddSpacer(FromDIP(20));
        auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(m_dialog_width, FromDIP(1)));
        divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
        root->Add(divider, 0, wxEXPAND);

        const wxSize button_size(FromDIP(124), FromDIP(38));
        auto* btns = new wxBoxSizer(wxHORIZONTAL);
        auto* btn_cancel = new Button(this, _L("Cancel"), wxEmptyString, wxBORDER_NONE, 0, wxID_CANCEL);
        btn_cancel->SetMinSize(button_size);
        btn_cancel->SetSize(button_size);
        btn_cancel->SetCornerRadius(FromDIP(4));
        btn_cancel->SetBorderWidth(FromDIP(1));
        btn_cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
        btn_cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
        btn_cancel->SetTextColorNormal(wxColour("#242424"));
        btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

        auto* btn_ok = new Button(this, _L("OK"), wxEmptyString, wxBORDER_NONE, 0, wxID_OK);
        btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        btn_ok->SetMinSize(button_size);
        btn_ok->SetSize(button_size);
        btn_ok->SetCornerRadius(FromDIP(4));
        btn_ok->SetBackgroundColorNormal(wxColour("#019687"));
        btn_ok->SetBorderColorNormal(wxColour("#019687"));
        btn_ok->SetTextColorNormal(wxColour("#FFFFFF"));
        btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

        btns->AddStretchSpacer();
        btns->Add(btn_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        btns->Add(btn_ok, 0, wxALIGN_CENTER_VERTICAL);
        btns->AddStretchSpacer();
        root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(12));

        SetSizer(root);
        select_family(m_current_family);
    }

    // Family list popup: max 8 visible rows (scrollable beyond), always
    // opens DOWNWARD under the trigger chip.
    void show_family_popup()
    {
        if (!m_family_trigger || m_family_popup || m_families.empty())
            return;
        BOOST_LOG_TRIVIAL(warning) << "texture family popup: "
            << m_families.size() << " families";

        auto* popup = new MixModePopup(this, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS);
        m_family_popup = popup;
        popup->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& e) {
            e.Skip();
            m_family_popup = nullptr;
        });

        const wxColour pop_bg = StateColor::darkModeColorFor(*wxWHITE);
        // Width locked to the trigger (2026-11 feedback: popup was wider);
        // height = up to 8 rows, clamped to the on-screen room below the
        // trigger so the dialog stays reachable when it opens low on screen
        // (2026-11 feedback: bottom-aligned dialog + downward popup).
        const int item_w = m_family_trigger->GetSize().x;
        const int item_h = FromDIP(32);
        int list_h = std::min((int)m_families.size(), 8) * item_h;

        // Never open beyond the work-area bottom: cut the VISIBLE height to
        // what fits (min 2 rows) — the hidden entries stay reachable via the
        // list's own scrollbar instead of being chopped off-screen.
        {
            wxRect work;
            const int display_idx = wxDisplay::GetFromWindow(m_family_trigger);
            work = display_idx != wxNOT_FOUND ? wxDisplay(display_idx).GetClientArea()
                                              : wxDisplay().GetClientArea();
            // Anchor may sit partly off-screen (room < 0): clamp BEFORE use
            // so a negative room can never pass through as a height.
            const int room = std::max(item_h * 2,
                work.GetBottom() - m_family_trigger->ClientToScreen(
                    wxPoint(0, m_family_trigger->GetSize().y)).y - FromDIP(8));
            if (room < list_h)
                list_h = room;
        }

        auto* scroll = new wxScrolledWindow(popup, wxID_ANY, wxDefaultPosition,
                                            wxSize(item_w, list_h), wxVSCROLL);
        scroll->SetScrollRate(0, FromDIP(16));
        scroll->SetBackgroundColour(pop_bg);
        auto* list_sizer = new wxBoxSizer(wxVERTICAL);
        for (int i = 0; i < (int)m_families.size(); ++i) {
            auto* row = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxSize(item_w, item_h));
            row->SetBackgroundColour(pop_bg);
            row->SetBackgroundStyle(wxBG_STYLE_PAINT);
            row->SetCursor(wxCursor(wxCURSOR_HAND));
            row->Bind(wxEVT_PAINT, [this, i, pop_bg](wxPaintEvent& e) {
                auto* p = static_cast<wxPanel*>(e.GetEventObject());
                wxAutoBufferedPaintDC dc(p);
                const wxSize sz = p->GetClientSize();
                dc.SetBackground(wxBrush(pop_bg));
                dc.Clear();
                const bool hovered = (m_family_hover_row == i);
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(StateColor::darkModeColorFor(hovered ? wxColour("#F4F4F4") : wxColour("#FFFFFF"))));
                dc.DrawRectangle(0, 0, sz.x, sz.y);

                wxFont f = p->GetFont();
                f.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
                f.SetWeight(wxFONTWEIGHT_MEDIUM);
                dc.SetFont(f);
                const bool selected = (m_current_family == i);
                dc.SetTextForeground(StateColor::darkModeColorFor(selected ? wxColour("#009688") : wxColour("#18181B")));
                const wxString label = ellipsize_text(dc, m_families[i].label, sz.x - p->FromDIP(8) - p->FromDIP(20));
                const wxSize tsz = dc.GetTextExtent(label);
                dc.DrawText(label, p->FromDIP(8), (sz.y - tsz.y) / 2);
                if (selected) {
                    texture_draw_glyph(dc, p, sz.x - p->FromDIP(20),
                        (sz.y - p->FromDIP(12)) / 2.0,
                        StateColor::darkModeColorFor(wxColour("#009688")), true);
                }
            });
            row->Bind(wxEVT_MOTION, [this, i, scroll](wxMouseEvent& evt) {
                evt.Skip();
                // Member-tracked hover: refresh only the affected rows so no
                // stale shading lingers on neighbors during scroll
                // (2026-11 feedback - hover shadow overlap).
                if (m_family_hover_row != i) {
                    m_family_hover_row = i;
                    scroll->Refresh();
                }
            });
            row->Bind(wxEVT_LEAVE_WINDOW, [this, scroll](wxMouseEvent& evt) {
                evt.Skip();
                if (m_family_hover_row != -1) {
                    m_family_hover_row = -1;
                    scroll->Refresh();
                }
            });
            row->Bind(wxEVT_LEFT_DOWN, [this, i, popup](wxMouseEvent&) {
                popup->Dismiss();
                select_family(i);
            });
            list_sizer->Add(row, 0, wxEXPAND);
        }
        scroll->SetSizer(list_sizer);
        // Virtual (scrollable) extent = ALL rows; the window height stays
        // list_h. Do NOT clamp the scrolled window with Min/MaxSize — those
        // constrain the viewport, and the popup below them re-derives from
        // GetBestSize, which previously cut the popup to the visible rows
        // and made later entries unreachable (UI 显示限制，非数据问题).
        const int total_h = item_h * (int)m_families.size();
        scroll->SetVirtualSize(wxSize(item_w, total_h));
        scroll->SetScrollRate(0, FromDIP(16));

        auto* top = new wxBoxSizer(wxVERTICAL);
        top->Add(scroll, 1, wxEXPAND);
        popup->SetSizer(top);
        popup->SetMinSize(wxSize(item_w, list_h));
        popup->SetSize(item_w, list_h);

        wxPoint pos = m_family_trigger->ClientToScreen(wxPoint(0, m_family_trigger->GetSize().y + 2));
        wxRect work;
        const int display_idx = wxDisplay::GetFromPoint(pos);
        work = display_idx != wxNOT_FOUND ? wxDisplay(display_idx).GetClientArea()
                                          : wxDisplay().GetClientArea();
        pos.x = std::clamp(pos.x, work.GetLeft(),
                           std::max(work.GetLeft(), work.GetRight() - item_w));
        popup->Move(pos); // Move, not Position(): Position() flips the popup ABOVE the anchor when it does not fit below
        popup->Popup();
    }

    void select_family(int idx)
    {
        if (idx < 0 || idx >= (int)m_families.size())
            return;
        m_current_family  = idx;
        m_selected_preset = m_families[idx].preset_name;
        BOOST_LOG_TRIVIAL(warning) << "texture family selected: "
            << m_families[idx].label << " colors="
            << m_families[idx].info.colors.size();
        if (m_family_trigger)
            m_family_trigger->Refresh();

        rebuild_swatches();

        // Preselect the family's first official color.
        const auto& colors = m_families[idx].info.colors;
        if (!colors.empty()) {
            select_color_item(colors.front());
        } else {
            m_selected_hex.clear();
            m_selected_name = wxEmptyString;
            m_selected_sku.clear();
            update_selection_ui();
        }
        // Size the window to the FINAL state (after visibility toggles):
        // refit inside rebuild_swatches would measure a mid-transition layout.
        refit();
        Layout();
        Refresh();
    }

    void rebuild_swatches()
    {
        // Freeze through the child churn so the swap is invisible; the grid
        // sizer is created once and reused (no per-switch sizer leak).
        Freeze();
        m_swatches.clear();
        m_swatch_grid->Clear(true);

        const auto& colors = m_families[m_current_family].info.colors;
        const int swatch_size = FromDIP(24) + FromDIP(3) * 2;

        // 2026-11 requirement: families without official colors (custom
        // presets outside the library) show no official grid at all - only
        // the "Other Colors" button below. The whole preview block collapses
        // with it (2026-11: stale preview / leftover gap).
        const bool has_official = !colors.empty();
        m_official_label->Show(has_official);
        m_swatch_panel->Show(has_official);
        if (m_official_gap1) m_official_gap1->Show(has_official);
        if (m_official_gap2) m_official_gap2->Show(has_official);
        if (m_preview_item) m_preview_item->Show(has_official);
        if (m_preview_gap) m_preview_gap->Show(has_official);

        for (const auto& item : colors) {
            auto* swatch = new AddMaterialSwatch(m_swatch_panel, item, wxSize(swatch_size, swatch_size));
            const std::string hex = item.colorData.PrimaryColor();
            swatch->Bind(wxEVT_LEFT_DOWN, [this, hex, item](wxMouseEvent&) {
                select_color_item(item);
            });
            m_swatch_grid->Add(swatch, 0);
            m_swatches.emplace_back(swatch, hex);
        }

        // Rows match the 8-column grid; only computed for official families
        // (non-official ones keep the collapsed layout). The window is sized
        // to the final state by refit() at the end of select_family().
        m_swatch_panel->SetMinSize(wxDefaultSize);
        m_swatch_panel->InvalidateBestSize();
        if (has_official) {
            const int columns = 8;
            const int rows = std::max(2, (int)((colors.size() + columns - 1) / columns));
            m_swatch_panel->SetMinSize(
                wxSize(m_content_width, rows * swatch_size));
            m_swatch_panel->Layout();
        }
        Thaw();
    }

    void select_color_item(const Slic3r::FilamentColorItem& item)
    {
        m_selected_hex  = item.colorData.PrimaryColor();
        m_selected_name = add_material_color_name(item);
        m_selected_sku  = item.sku;
        update_selection_ui();
    }

    void update_selection_ui()
    {
        m_name_label->SetLabel(m_selected_name);
        m_sku_label->SetLabel(m_selected_sku.empty() ? wxString() : "sku " + m_selected_sku);
        m_sku_label->Show(!m_families.empty() &&
            !m_families[m_current_family].info.colors.empty() &&
            !m_selected_sku.empty());
        for (auto& [swatch, hex] : m_swatches)
            swatch->SetSelected(hex == m_selected_hex);
        m_preview_panel->Refresh();
        Layout();
    }

    std::vector<Family> m_families;
    int m_current_family = 0;
    std::string m_selected_preset;
    std::string m_selected_hex;
    wxString m_selected_name;
    wxString m_selected_sku;
    std::vector<std::pair<AddMaterialSwatch*, std::string>> m_swatches;

    int m_dialog_width = 0;
    int m_content_margin = 0;
    int m_content_width = 0;
    wxPanel*             m_family_trigger = nullptr;
    wxPopupTransientWindow* m_family_popup = nullptr;
    // Hover row index for the open family dropdown; -1 = none.
    int m_family_hover_row = -1;
    wxPanel* m_preview_panel = nullptr;
    wxBitmap m_preview_bitmap;
    std::string m_preview_bitmap_hex;
    wxStaticText* m_name_label = nullptr;
    wxStaticText* m_sku_label = nullptr;
    wxPanel* m_swatch_panel = nullptr;
    wxStaticText* m_official_label = nullptr;
    wxGridSizer* m_swatch_grid = nullptr;
    wxSizerItem* m_official_gap1 = nullptr;
    wxSizerItem* m_official_gap2 = nullptr;
    // Preview row + trailing gap: collapsed for non-official families.
    wxSizerItem* m_preview_item = nullptr;
    wxSizerItem* m_preview_gap = nullptr;
    wxPanel* m_other_panel = nullptr;
};


// ============================================================
// TextureImportDialog
// ============================================================

static int unique_face_color_count(const std::vector<std::array<std::size_t, 3>>& colors)
{
    std::set<std::array<std::size_t, 3>> unique_colors(colors.begin(), colors.end());
    return (int)unique_colors.size();
}

// ============================================================
// Gap area merge (small-fragment absorption)
// ============================================================

// Right end of the gap area slider, in percent of the total patch area.
static constexpr double kGapAreaMaxPercent = 5.0;
// Step-1 parameter layout: reserve room for the localized Gap Area (%) label.
static constexpr int kGapSliderWidthDip = 188;
static constexpr int kColorPresetGapDip = 16;
// CIEDE2000 difference at which a neighbor's boundary counts half when the gap
// merge picks the region a fragment is absorbed into.
static constexpr double kGapMergeColorSigma = 10.0;

// Same-color edge-connected component on the simplified mesh.
struct ColorPatch {
    int                         patch_id    = -1;
    double                      patch_area  = 0.0;
    std::vector<int>            patch_faces;
    std::array<std::size_t, 3>  color_label{};
};

// One step of the gap area region merge: the patches in [member_begin, member_end) of
// GapPreviewState::merge_members formed a region that was absorbed by a neighbor
// and recolored from from_color to to_color.
struct GapMergeEvent {
    double                     key_area = 0.0; // area of the absorbed region at the time it was absorbed
    std::array<std::size_t, 3> from_color{};
    std::array<std::size_t, 3> to_color{};
    int                        member_begin = 0;
    int                        member_end   = 0;
};

struct GapPreviewState {
    std::vector<ColorPatch> color_patches;
    // key_area never decreases along merge_events, so a threshold selects a prefix.
    std::vector<GapMergeEvent> merge_events;
    std::vector<int>           merge_members;
    // Number of merge_events currently reflected in display_face_colors.
    int                                        applied_events = 0;
    double                                     gap_total_area = 0.0;
    // Largest merge percent that keeps every palette color alive (computed with
    // the patch set); the slider is clamped to this when applying.
    double                                     max_merge_percent = kGapAreaMaxPercent;
    std::vector<std::array<std::size_t, 3>>    simplified_face_colors;
    std::vector<std::array<std::size_t, 3>>    simplified_cluster_colors;
    std::vector<std::array<std::size_t, 3>>    display_face_colors;
};

namespace {

indexed_triangle_set painted_mesh_to_its(const Slic3r::PaintedMesh& painted)
{
    indexed_triangle_set its;
    its.vertices.reserve(painted.vertices.size());
    for (const auto& v : painted.vertices)
        its.vertices.emplace_back(v[0], v[1], v[2]);
    its.indices.reserve(painted.indices.size());
    for (const auto& f : painted.indices)
        its.indices.emplace_back(f[0], f[1], f[2]);
    return its;
}

// Same-color edge-connected components on the simplified mesh. shared_out[p][q]
// receives the boundary length shared by patches p and q.
std::vector<ColorPatch> build_color_patches(
    const Slic3r::PaintedMesh& painted,
    const std::vector<std::array<std::size_t, 3>>& face_colors,
    std::vector<std::unordered_map<int, double>>& shared_out)
{
    shared_out.clear();
    std::vector<ColorPatch> patches;
    const size_t nfaces = painted.indices.size();
    if (nfaces == 0 || face_colors.size() != nfaces)
        return patches;

    indexed_triangle_set its = painted_mesh_to_its(painted);
    if (its.indices.size() != nfaces)
        return patches;

    const std::vector<Vec3i32> neighbors = its_face_neighbors_par(its);
    std::vector<int>   face_to_patch(nfaces, -1);
    std::vector<char>  visited(nfaces, 0);

    auto face_area = [&](int fi) -> double {
        const auto& tri = its.indices[fi];
        if (tri[0] < 0 || tri[1] < 0 || tri[2] < 0)
            return 0.0;
        if (tri[0] >= (int)its.vertices.size() ||
            tri[1] >= (int)its.vertices.size() ||
            tri[2] >= (int)its.vertices.size())
            return 0.0;
        const Vec3f& a = its.vertices[tri[0]];
        const Vec3f& b = its.vertices[tri[1]];
        const Vec3f& c = its.vertices[tri[2]];
        return 0.5 * (b - a).cross(c - a).cast<double>().norm();
    };

    for (size_t start = 0; start < nfaces; ++start) {
        if (visited[start])
            continue;

        ColorPatch patch;
        patch.patch_id = (int)patches.size();
        patch.color_label = face_colors[start];

        std::queue<int> q;
        q.push((int)start);
        visited[start] = 1;

        while (!q.empty()) {
            const int fi = q.front();
            q.pop();
            patch.patch_faces.push_back(fi);
            patch.patch_area += face_area(fi);
            face_to_patch[fi] = (int)patches.size();

            if (fi >= (int)neighbors.size())
                continue;
            for (int e = 0; e < 3; ++e) {
                const int nb = neighbors[fi][e];
                if (nb < 0 || nb >= (int)nfaces || visited[nb])
                    continue;
                if (face_colors[nb] != face_colors[start])
                    continue;
                visited[nb] = 1;
                q.push(nb);
            }
        }

        patches.push_back(std::move(patch));
    }

    const int npatches = (int)patches.size();
    std::vector<std::unordered_map<int, double>>& shared = shared_out;
    shared.assign(npatches, {});
    for (int fi = 0; fi < (int)nfaces; ++fi) {
        if (fi >= (int)neighbors.size())
            continue;
        const int pid = face_to_patch[fi];
        if (pid < 0)
            continue;
        for (int e = 0; e < 3; ++e) {
            const int nb = neighbors[fi][e];
            if (nb <= fi || nb >= (int)nfaces)
                continue;
            const int qid = face_to_patch[nb];
            if (qid < 0 || qid == pid)
                continue;
            const Vec2i32 edge = its_triangle_edge(its.indices[fi], e);
            if (edge[0] < 0 || edge[1] < 0 ||
                edge[0] >= (int)its.vertices.size() ||
                edge[1] >= (int)its.vertices.size())
                continue;
            const double len = (its.vertices[edge[0]] - its.vertices[edge[1]]).cast<double>().norm();
            shared[pid][qid] += len;
            shared[qid][pid] += len;
        }
    }
    return patches;
}

// Greedy region merge on the patch adjacency graph. The smallest live region is
// absorbed into the neighbor with the best shared boundary length weighted by
// color affinity 1 / (1 + (dE00 / sigma)^2), so a close color wins over a longer
// contact with a contrasting one. The absorbed region takes that neighbor's
// color, the merged region keeps the summed area, and every other former
// neighbor of the absorbed region with that same color joins it too, so
// adjacent live regions never share a color. All other regions only grow, so
// the recorded key areas never decrease. Merging stops at max_key_area.
void build_gap_merge_events(
    GapPreviewState& state,
    std::vector<std::unordered_map<int, double>> shared,
    double max_key_area)
{
    using FaceColor = std::array<std::size_t, 3>;
    state.merge_events.clear();
    state.merge_members.clear();
    state.applied_events = 0;

    const int n = (int)state.color_patches.size();
    if (n == 0 || (int)shared.size() != n)
        return;

    // Region colors always stay within the simplified palette, so regions carry a
    // palette index and the color affinity is looked up from a K x K table.
    std::vector<FaceColor>   palette;
    std::map<FaceColor, int> palette_index;
    std::vector<double>      area(n);
    std::vector<int>         color(n);
    std::vector<std::vector<int>> members(n);
    std::vector<int>         version(n, 0);
    std::vector<char>        alive(n, 1);
    for (int i = 0; i < n; ++i) {
        const FaceColor& label = state.color_patches[i].color_label;
        auto [it, inserted] = palette_index.emplace(label, (int)palette.size());
        if (inserted)
            palette.push_back(label);
        area[i]    = state.color_patches[i].patch_area;
        color[i]   = it->second;
        members[i] = {i};
    }

    const int k = (int)palette.size();
    std::vector<double> affinity((size_t)k * k, 1.0);
    for (int i = 0; i < k; ++i) {
        for (int j = i + 1; j < k; ++j) {
            const double de = Slic3r::compute_delta_e(palette[i], palette[j]) / kGapMergeColorSigma;
            const double w  = 1.0 / (1.0 + de * de);
            affinity[(size_t)i * k + j] = w;
            affinity[(size_t)j * k + i] = w;
        }
    }

    struct HeapItem {
        double area;
        int    region;
        int    version;
        bool operator>(const HeapItem& rhs) const {
            if (area != rhs.area)
                return area > rhs.area;
            return region > rhs.region;
        }
    };
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>> heap;
    for (int i = 0; i < n; ++i) {
        if (!shared[i].empty())
            heap.push({area[i], i, version[i]});
    }

    // Fold two regions into one carrying merged_color. The side with more
    // neighbors survives so adjacency updates stay small-to-large.
    auto fold = [&](int keep, int drop, int merged_color) -> int {
        if (shared[keep].size() < shared[drop].size())
            std::swap(keep, drop);
        area[keep] += area[drop];
        if (members[keep].size() < members[drop].size())
            members[keep].swap(members[drop]);
        members[keep].insert(members[keep].end(), members[drop].begin(), members[drop].end());
        std::vector<int>().swap(members[drop]);

        shared[keep].erase(drop);
        for (const auto& [nb, len] : shared[drop]) {
            if (nb == keep)
                continue;
            shared[keep][nb] += len;
            auto& nb_shared = shared[nb];
            nb_shared.erase(drop);
            nb_shared[keep] += len;
        }
        std::unordered_map<int, double>().swap(shared[drop]);

        color[keep] = merged_color;
        alive[drop] = 0;
        return keep;
    };

    std::vector<int> same_color;
    while (!heap.empty()) {
        const HeapItem top = heap.top();
        heap.pop();
        const int a = top.region;
        if (!alive[a] || top.version != version[a])
            continue;
        if (area[a] >= max_key_area)
            break;
        if (shared[a].empty())
            continue;

        int    b          = -1;
        double best_score = -1.0;
        double best_area  = -1.0;
        const double* affinity_row = affinity.data() + (size_t)color[a] * k;
        for (const auto& [q, len] : shared[a]) {
            const double score = len * affinity_row[color[q]];
            const double qarea = area[q];
            if (score > best_score ||
                (score == best_score && (qarea > best_area || (qarea == best_area && q < b)))) {
                b          = q;
                best_score = score;
                best_area  = qarea;
            }
        }
        if (b < 0)
            continue;

        const int target_color = color[b];
        GapMergeEvent ev;
        ev.key_area     = area[a];
        ev.from_color   = palette[color[a]];
        ev.to_color     = palette[target_color];
        ev.member_begin = (int)state.merge_members.size();
        state.merge_members.insert(state.merge_members.end(), members[a].begin(), members[a].end());
        ev.member_end   = (int)state.merge_members.size();
        state.merge_events.push_back(ev);

        same_color.clear();
        for (const auto& [q, len] : shared[a]) {
            if (q != b && color[q] == target_color)
                same_color.push_back(q);
        }

        int merged = fold(b, a, target_color);
        for (int c : same_color)
            merged = fold(merged, c, target_color);
        ++version[merged];
        if (!shared[merged].empty())
            heap.push({area[merged], merged, version[merged]});
    }
}

// Largest merge percent whose event prefix keeps every palette color alive.
// Colors never revive once emptied (no region carries them, so they can never
// be a merge target again), which makes the safe prefixes a contiguous range:
// a single counting walk over the event list finds the first event that would
// empty a color, and that event's key_area bounds the safe threshold. Pure
// bookkeeping — the display colors are never touched.
double palette_safe_gap_cap(const GapPreviewState& state)
{
    if (state.merge_events.empty() || state.gap_total_area <= 0.0)
        return kGapAreaMaxPercent;

    std::map<std::array<std::size_t, 3>, std::size_t> color_face_count;
    for (const auto& color : state.simplified_face_colors)
        ++color_face_count[color];

    const int npatches = (int)state.color_patches.size();
    for (size_t ei = 0; ei < state.merge_events.size(); ++ei) {
        const GapMergeEvent& ev = state.merge_events[ei];
        if (ev.from_color == ev.to_color)
            continue;
        std::size_t member_faces = 0;
        for (int m = ev.member_begin; m < ev.member_end; ++m) {
            const int pid = state.merge_members[m];
            if (pid >= 0 && pid < npatches)
                member_faces += state.color_patches[pid].patch_faces.size();
        }
        auto it = color_face_count.find(ev.from_color);
        if (it == color_face_count.end() || it->second <= member_faces) {
            // This event would empty from_color: stop before it. A threshold at
            // exactly key_area selects the prefix below it (lower_bound counts
            // key_area < threshold), so floor to hundredths to stay safe even
            // when adjacent events share a key_area.
            const double percent = std::floor(ev.key_area / state.gap_total_area * 100.0 * 100.0) / 100.0;
            return std::clamp(percent, 0.0, kGapAreaMaxPercent);
        }
        it->second                      -= member_faces;
        color_face_count[ev.to_color]   += member_faces;
    }
    return kGapAreaMaxPercent;
}

void build_gap_preview_regions(
    GapPreviewState& state,
    const Slic3r::PaintedMesh& painted,
    const std::vector<std::array<std::size_t, 3>>& face_colors)
{
    std::vector<std::unordered_map<int, double>> shared;
    state.color_patches = build_color_patches(painted, face_colors, shared);
    state.gap_total_area = 0.0;
    for (const auto& patch : state.color_patches)
        state.gap_total_area += patch.patch_area;
    build_gap_merge_events(state, std::move(shared),
                           (kGapAreaMaxPercent / 100.0) * state.gap_total_area);
    state.max_merge_percent = palette_safe_gap_cap(state);
}

std::unique_ptr<GapPreviewState> make_gap_preview_state(const Slic3r::PaintedMesh& painted)
{
    auto state = std::make_unique<GapPreviewState>();
    state->simplified_face_colors = painted.face_colors;
    state->simplified_cluster_colors = painted.cluster_colors;
    state->display_face_colors = painted.face_colors;
    build_gap_preview_regions(*state, painted, painted.face_colors);
    return state;
}

void write_patch_display_color(
    GapPreviewState& state,
    const ColorPatch& patch,
    const std::array<std::size_t, 3>& color)
{
    for (int fi : patch.patch_faces) {
        if (fi >= 0 && fi < (int)state.display_face_colors.size())
            state.display_face_colors[fi] = color;
    }
}

std::vector<std::array<std::size_t, 3>> remaining_cluster_colors(
    const std::vector<std::array<std::size_t, 3>>& simplified_clusters,
    const std::vector<std::array<std::size_t, 3>>& display_colors)
{
    std::set<std::array<std::size_t, 3>> remaining(display_colors.begin(), display_colors.end());
    std::vector<std::array<std::size_t, 3>> ordered;
    ordered.reserve(remaining.size());
    for (const auto& color : simplified_clusters) {
        auto it = remaining.find(color);
        if (it == remaining.end())
            continue;
        ordered.push_back(color);
        remaining.erase(it);
    }
    for (const auto& color : display_colors) {
        auto it = remaining.find(color);
        if (it == remaining.end())
            continue;
        ordered.push_back(color);
        remaining.erase(it);
    }
    return ordered;
}

} // namespace

// The gap slider works in hundredths of a percent (0..500 <=> 0.00..5.00).
static int gap_value_to_slider(double value)
{
    return (int)std::lround(std::clamp(value, 0.0, kGapAreaMaxPercent) * 100.0);
}

wxBEGIN_EVENT_TABLE(TextureImportDialog, DPIDialog)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_4,    TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_8,    TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_16,   TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_32,   TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_64,   TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_APPLY,   TextureImportDialog::on_apply_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_SKIP,    TextureImportDialog::on_skip_clicked)
    EVT_BUTTON(wxID_OK,                              TextureImportDialog::on_ok_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_NEXT,    TextureImportDialog::on_next_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_BACK,    TextureImportDialog::on_back_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_RESET,   TextureImportDialog::on_reset_clicked)
wxEND_EVENT_TABLE()

TextureImportDialog::TextureImportDialog(
    wxWindow*                        parent,
    const Slic3r::TexturedMesh&      textured_mesh,
    const std::vector<TextureFilamentEntry>& filament_entries,
    std::function<bool()>            initial_cancel_callback,
    std::function<bool(int)>         initial_progress_callback)
    : DPIDialog(parent, wxID_ANY, _L("Import Model"),
                wxDefaultPosition, wxDefaultSize,
                (wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) & ~(wxMINIMIZE_BOX | wxMAXIMIZE_BOX))
    , m_textured_mesh(textured_mesh)
    , m_filament_entries(filament_entries)
    , m_initial_cancel_callback(std::move(initial_cancel_callback))
    , m_initial_progress_callback(std::move(initial_progress_callback))
{
    // 2026-11 two-step design keeps the standard native title bar (same as the
    // other dialogs in the app); the step indicator sits under it. 789x710 is
    // the Figma client size for the content below the caption.
    SetSize(wxSize(FromDIP(789), FromDIP(710) + FromDIP(30)));

    m_filament_colors_rgba.reserve(m_filament_entries.size());
    m_filament_color_strs.reserve(m_filament_entries.size());
    m_filament_names.reserve(m_filament_entries.size());
    for (size_t i = 0; i < m_filament_entries.size(); ++i) {
        auto& entry = m_filament_entries[i];
        entry.dialog_index = (int)i;
        entry.color_hex = texture_normalize_color_hex(entry.color_hex);
        if (entry.name.empty())
            entry.name = GUI::format(_u8L("Filament %d"), i + 1);
        m_filament_color_strs.push_back(entry.color_hex);
        m_filament_names.push_back(entry.name);
        m_filament_colors_rgba.push_back(parse_color_string(entry.color_hex));
    }

    m_existing_filament_count = m_filament_colors_rgba.size();
    m_default_virtual_filament_preset_name = resolve_default_virtual_filament_preset_name();

    Bind(EVT_TEXTURE_COMPUTE_DONE,     &TextureImportDialog::on_computation_complete, this);
    Bind(EVT_TEXTURE_COMPUTE_PROGRESS, &TextureImportDialog::on_computation_progress, this);
    Bind(EVT_TEXTURE_COMPUTE_ERROR,    &TextureImportDialog::on_computation_error,    this);
    Bind(EVT_TEXTURE_MESH_REPAIR_DECISION, &TextureImportDialog::on_mesh_repair_decision_required, this);

    // Doc §3.3.1: leaving via the title-bar X or Esc exits the WHOLE import
    // command, so guard it behind a confirmation. Declining returns to the
    // dialog / running computation untouched.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
        if (evt.CanVeto() && !confirm_terminate_import()) {
            evt.Veto();
            return;
        }
        m_cancel_flag = true;
        if (m_initial_computation_pending) {
            // The pump loop in ShowModal() observes these flags and returns
            // wxID_CANCEL itself; calling EndModal here would double-modal.
            m_initial_computation_cancelled = true;
            m_initial_computation_pending = false;
            return;
        }
        if (m_state == TextureImportState::Computing)
            cancel_computation();
        EndModal(wxID_CANCEL);
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& evt) {
        if (evt.GetKeyCode() == WXK_ESCAPE) {
            Close(); // routes through the wxEVT_CLOSE_WINDOW confirm path
            return;
        }
        evt.Skip();
    });

    build_ui();
    SetMinSize(wxSize(FromDIP(789), FromDIP(740)));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    m_preview_canvas->set_mesh_data(m_textured_mesh.vertices, m_textured_mesh.indices);

    // Pre-computed face colors (OBJ vertex colors / MTL face colors):
    // use them directly as the Original preview, skip texture decode.
    if (!m_textured_mesh.precomputed_face_colors.empty()) {
        m_original_color_count = unique_face_color_count(m_textured_mesh.precomputed_face_colors);
        m_preview_canvas->set_original_face_colors(m_textured_mesh.precomputed_face_colors);
    } else if (!m_textured_mesh.textures.empty()) {
        std::vector<std::vector<unsigned char>> tex_pixels_rgb;
        std::vector<int> tex_widths, tex_heights;
        tex_pixels_rgb.reserve(m_textured_mesh.textures.size());
        tex_widths.reserve(m_textured_mesh.textures.size());
        tex_heights.reserve(m_textured_mesh.textures.size());

        for (const auto& ti : m_textured_mesh.textures) {
            std::vector<unsigned char> bgr_pixels;
            int w = 0, h = 0;
            if (Slic3r::decode_texture_to_pixels(ti, bgr_pixels, w, h) && !bgr_pixels.empty()) {
                // Convert BGR to RGB for OpenGL
                for (size_t p = 0; p < bgr_pixels.size(); p += 3)
                    std::swap(bgr_pixels[p], bgr_pixels[p + 2]);
                tex_pixels_rgb.push_back(std::move(bgr_pixels));
            } else {
                tex_pixels_rgb.push_back({});
            }
            tex_widths.push_back(w);
            tex_heights.push_back(h);
        }

        const size_t nf = m_textured_mesh.indices.size();
        const bool has_mapping = !m_textured_mesh.material_texture_map.empty();

        // Build per-face UV array
        std::vector<std::array<std::array<float,2>, 3>> face_uvs(nf);
        for (size_t fi = 0; fi < nf; ++fi) {
            if (m_textured_mesh.has_face_uvs()) {
                const auto& ui = m_textured_mesh.uv_indices[fi];
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = ui[vi];
                    if (idx >= 0 && static_cast<size_t>(idx) < m_textured_mesh.uv_coords.size())
                        face_uvs[fi][vi] = m_textured_mesh.uv_coords[idx];
                    else
                        face_uvs[fi][vi] = {0.f, 0.f};
                }
            } else if (!m_textured_mesh.uvs.empty()) {
                const auto& face = m_textured_mesh.indices[fi];
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = face[vi];
                    if (idx >= 0 && static_cast<size_t>(idx) < m_textured_mesh.uvs.size())
                        face_uvs[fi][vi] = m_textured_mesh.uvs[idx];
                    else
                        face_uvs[fi][vi] = {0.f, 0.f};
                }
            }
        }

        // Build per-face texture index
        std::vector<int> face_tex_ids(nf, 0);
        for (size_t fi = 0; fi < nf; ++fi) {
            int mat_idx = (fi < m_textured_mesh.material_ids.size())
                          ? m_textured_mesh.material_ids[fi] : -1;
            if (has_mapping && mat_idx >= 0
                && static_cast<size_t>(mat_idx) < m_textured_mesh.material_texture_map.size())
                face_tex_ids[fi] = m_textured_mesh.material_texture_map[mat_idx];
            else if (!tex_pixels_rgb.empty())
                face_tex_ids[fi] = 0;
            else
                face_tex_ids[fi] = -1;
        }

        m_preview_canvas->set_texture_render_data(
            tex_pixels_rgb, tex_widths, tex_heights, face_uvs, face_tex_ids);

        // Color count shown under the original-project pane (§3.2): the number of distinct
        // colors the input actually carries, before any clustering.
        std::unordered_set<uint32_t> unique_tex_colors;
        for (const auto& pixels : tex_pixels_rgb) {
            if (pixels.empty()) continue;
            for (size_t p = 0; p + 2 < pixels.size(); p += 3)
                unique_tex_colors.insert((uint32_t(pixels[p]) << 16) | (uint32_t(pixels[p + 1]) << 8) | uint32_t(pixels[p + 2]));
        }
        if (!unique_tex_colors.empty())
            m_original_color_count = (long long)unique_tex_colors.size();

        // Still sample per-face colors as fallback preview colors; only use
        // their count when the finer texture-pixel count was unavailable.
        std::vector<std::array<std::size_t, 3>> orig_colors;
        if (Slic3r::sample_original_face_colors(m_textured_mesh, orig_colors)) {
            if (m_original_color_count <= 0)
                m_original_color_count = unique_face_color_count(orig_colors);
            m_preview_canvas->set_original_face_colors(orig_colors);
        }
    }

    // Doc §3.3.1: default color count is 4 (matching 4-tool machines), or the
    // original model's color count when it carries fewer.
    {
        int default_color_count = 4;
        if (m_original_color_count > 0)
            default_color_count = (int)std::min<long long>(4, m_original_color_count);
        set_color_count_value(std::clamp(default_color_count, 1, (int)max_filament_count()), true);
    }

    set_state(TextureImportState::Idle);
    update_preview_color_labels();
}

void TextureImportDialog::update_preview_color_labels()
{
    // The color-count captions are a step-1 concern; the step-2 design hides
    // them (the mapping card takes that vertical space instead).
    const bool captions_visible = (m_step == ImportStep::Simplify);

    if (m_lbl_orig_colors) {
        if (m_original_color_count > 0) {
            const long long shown = std::min<long long>(m_original_color_count, (long long)std::numeric_limits<int>::max());
            m_lbl_orig_colors->SetLabel(wxString::Format(_L("Before simplification: %d colors"), (int)shown));
        } else {
            m_lbl_orig_colors->SetLabel(wxEmptyString);
        }
        m_lbl_orig_colors->Show(captions_visible && m_original_color_count > 0);
    }

    if (m_lbl_result_colors) {
        if (!m_painted.cluster_colors.empty()) {
            m_lbl_result_colors->SetLabel(wxString::Format(_L("After simplification: %d colors"),
                                                           (int)m_painted.cluster_colors.size()));
        } else {
            m_lbl_result_colors->SetLabel(wxEmptyString);
        }
        m_lbl_result_colors->Show(captions_visible && !m_painted.cluster_colors.empty());
    }
}

TextureImportDialog::~TextureImportDialog()
{
    dismiss_filament_popup();
    m_cancel_flag = true;
    if (m_worker && m_worker->joinable())
        m_worker->join();
}

bool TextureImportDialog::confirm_terminate_import()
{
    MessageDialog dlg(this,
        _L("This action will end the entire import process. Are you sure you want to continue?"),
        _L("Texture Import Warning"), wxOK | wxCANCEL | wxICON_WARNING);
    return dlg.ShowModal() == wxID_OK;
}

int TextureImportDialog::ShowModal()
{
    if (m_state == TextureImportState::Idle && m_painted.face_colors.empty()) {
        // Initial run uses the explicit default count (set in the constructor
        // per doc section 3.3.1: min(4, original palette)), not the pipeline's
        // auto mode (remote's "auto when <=4" variant is intentionally not
        // taken here; the doc mandates a deterministic default).
        start_computation(false, true);

        while (m_initial_computation_pending) {
            if (auto* event_loop = wxEventLoopBase::GetActive())
                event_loop->Yield();
            else
                wxYield();
            if (m_progress_dlg && m_progress_dlg->WasCancelled())
                m_cancel_flag = true;
            if (m_initial_cancel_callback && m_initial_cancel_callback())
                m_cancel_flag = true;
            wxMilliSleep(10);
        }

        if (m_worker && m_worker->joinable())
            m_worker->join();
        m_worker.reset();

        if (m_initial_computation_cancelled || m_initial_computation_failed)
            return wxID_CANCEL;
    }

    ScopedInteractiveBusyCursorSuspender busy_cursor_suspender;
    return DPIDialog::ShowModal();
}

void TextureImportDialog::build_ui()
{
    const wxColour dialog_bg = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(dialog_bg);
    SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));

    wxBoxSizer* root_sizer = new wxBoxSizer(wxVERTICAL);

    // Breathing room between the native title bar and the step indicator
    // (2026-11 feedback: the previous zero gap read as cramped).
    root_sizer->AddSpacer(FromDIP(12));

    build_step_indicator(root_sizer);

    // Content: the step-1 params card and the step-2 mapping panel live in the
    // same vertical sizer as the (always-visible) preview, so switching steps
    // only flips Show()/Hide() — the GL canvas never changes parent.
    wxPanel* content = new wxPanel(this, wxID_ANY);
    content->SetBackgroundColour(dialog_bg);
    wxBoxSizer* content_sizer = new wxBoxSizer(wxVERTICAL);
    build_params_panel(content, content_sizer);
    build_preview_panel(content, content_sizer);
    build_mapping_panel(content, content_sizer);
    content->SetSizer(content_sizer);
    root_sizer->Add(content, 1, wxEXPAND);

    build_bottom_buttons(root_sizer);

    SetSizer(root_sizer);
    Layout();
    Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);
}

void TextureImportDialog::build_step_indicator(wxSizer* root_sizer)
{
    m_step_indicator = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_step_indicator->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_step_indicator->SetMinSize(wxSize(-1, FromDIP(38)));
    m_step_indicator->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_step_indicator->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_step_indicator);
        const wxSize sz = m_step_indicator->GetClientSize();
        dc.SetBackground(wxBrush(m_step_indicator->GetBackgroundColour()));
        dc.Clear();

        const wxString labels[2] = { _L("Color Simplification"), _L("Filament Mapping & Mixing") };
        const bool active[2] = { m_step == ImportStep::Simplify, m_step == ImportStep::Mapping };

        const int dot = FromDIP(18);
        const int gap_dot_label = FromDIP(4);
        const int gap_label_line = FromDIP(16);
        const int line_len = FromDIP(20);

        wxFont label_font = m_step_indicator->GetFont();
        label_font.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
        label_font.SetWeight(wxFONTWEIGHT_MEDIUM);
        wxFont num_font = label_font;

        int total_w = 2 * dot + gap_dot_label;
        int text_w[2], text_h = 0;
        for (int i = 0; i < 2; ++i) {
            dc.SetFont(label_font);
            text_w[i] = dc.GetTextExtent(labels[i]).x;
            text_h = std::max(text_h, dc.GetTextExtent(labels[i]).y);
            total_w += text_w[i];
        }
        total_w += gap_label_line + line_len + gap_label_line;

        int x = (sz.x - total_w) / 2;
        const int y_dot = (sz.y - dot) / 2;
        const int y_text = (sz.y - text_h) / 2;

        for (int i = 0; i < 2; ++i) {
            const wxColour accent = StateColor::darkModeColorFor(wxColour("#009688"));
            const wxColour inactive_bg = StateColor::darkModeColorFor(wxColour("#F3F3F3"));
            const wxColour inactive_fg = StateColor::darkModeColorFor(wxColour("#71717A"));
            const wxColour active_fg = StateColor::darkModeColorFor(wxColour("#18181B"));

            // Only the CURRENT step lights up teal; past and future steps
            // stay grey (step-2 design shows step 1 greyed while on step 2).
            const bool lit = active[i];
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(lit ? accent : inactive_bg));
            dc.DrawCircle(x + dot / 2, y_dot + dot / 2, dot / 2);

            dc.SetFont(num_font);
            dc.SetTextForeground(lit ? StateColor::darkModeColorFor(wxColour("#FCFCFC")) : inactive_fg);
            const wxString num = wxString::Format("%d", i + 1);
            const wxSize nsz = dc.GetTextExtent(num);
            dc.DrawText(num, x + (dot - nsz.x) / 2, y_dot + (dot - nsz.y) / 2);

            x += dot + gap_dot_label;
            dc.SetFont(label_font);
            dc.SetTextForeground(active[i] ? active_fg : inactive_fg);
            dc.DrawText(labels[i], x, y_text);
            x += text_w[i];

            if (i == 0) {
                x += gap_label_line;
                const int line_y = sz.y / 2;
                dc.SetPen(wxPen(inactive_fg, 1));
                dc.DrawLine(x, line_y, x + line_len, line_y);
                x += line_len + gap_label_line;
            }
        }
    });

    root_sizer->Add(m_step_indicator, 0, wxEXPAND);
}

void TextureImportDialog::update_step_indicator()
{
    if (m_step_indicator)
        m_step_indicator->Refresh();
}

// Canvas height per step: Figma step 1 = 348x348 square panes, step 2 =
// 348x248 wide panes with the mapping card below. The dialog keeps the pane
// width (~364 at our content width), so the height switches with the step.
void TextureImportDialog::apply_preview_metrics()
{
    if (!m_preview_canvas)
        return;
    // Step 2 panes taller (2026-11 feedback: add height, sit closer to the
    // step indicator — the freed space comes from the gap above).
    const int h = (m_step == ImportStep::Simplify) ? FromDIP(364) : FromDIP(280);
    m_preview_canvas->SetMinSize(wxSize(-1, h));
    m_preview_canvas->SetMaxSize(wxSize(-1, h));

    // Step 1 only: extra air between the params card and the panes (2026-11
    // feedback — the first-step preview sat too high); hidden on step 2.
    if (m_preview_step_spacer)
        m_preview_step_spacer->Show(m_step == ImportStep::Simplify);
}

void TextureImportDialog::build_preview_panel(wxWindow* parent, wxSizer* sizer)
{
    // Muted caption color: same pair as the mixed feature's label text
    // (#8F8F8F, dark #8A8A95). #71717A has NO dark-mode mapping and washed
    // out on the dark dialog background (2026-11 feedback).
    const wxColour secondary_fg = StateColor::darkModeColorFor(wxColour("#8F8F8F"));

    // Same split-screen idea as before (one GL canvas, one camera, two
    // viewports), restyled per the 2026-11 step-1 design: the canvas renders
    // the two panes as rounded squares with a gap, each carrying an overlay
    // badge; the color counts are centered captions under the panes, mirroring
    // the "view + caption" pattern of MixedColorMatchPanel.
    wxPanel* preview_container = new wxPanel(parent, wxID_ANY);
    preview_container->SetBackgroundColour(parent->GetBackgroundColour());

    wxBoxSizer* container_sizer = new wxBoxSizer(wxVERTICAL);

    wxGLAttributes canvas_attrs;
    canvas_attrs.PlatformDefaults().RGBA().DoubleBuffer().Depth(24).EndList();
    m_preview_canvas = new TexturePreviewCanvas(preview_container, canvas_attrs);
    // Canvas height is step-dependent (apply_preview_metrics): step 1 shows
    // square panes (~364, Figma 348x348), step 2 shows the shorter 248-high
    // wide panes so the matching card below keeps its designed space.
    apply_preview_metrics();
    m_preview_canvas->set_badge_labels(_L("Original Model"), _L("Simplified"));

    auto add_pane_count = [&](wxSizer* row, wxStaticText*& count_label) {
        // Fixed pane-width label (348 = (716 - 20) / 2) with centred text:
        // wxST_NO_AUTORESIZE stops MSW from auto-resizing the control when
        // SetLabel() runs (e.g. "简化后：N 颜色" after Apply), which used to
        // drift the centre off the pane (2026-11 feedback).
        count_label = new wxStaticText(preview_container, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxSize(FromDIP(348), -1),
                                       wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
        wxFont f = count_label->GetFont();
        f.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
        count_label->SetFont(f);
        count_label->SetForegroundColour(secondary_fg);
        row->Add(count_label, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));
    };

    wxBoxSizer* counts_sizer = new wxBoxSizer(wxHORIZONTAL);
    add_pane_count(counts_sizer, m_lbl_orig_colors);
    counts_sizer->AddSpacer(FromDIP(20)); // same gap as between the panes, so each caption centers under its own pane
    add_pane_count(counts_sizer, m_lbl_result_colors);

    container_sizer->Add(m_preview_canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    container_sizer->Add(counts_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    preview_container->SetSizer(container_sizer);
    // Proportion 0: the pane height is fully driven by apply_preview_metrics();
    // proportion 1 made it compete with the mapping card and squeezed the
    // card below its 4-row height on step 2 (2026-11 feedback).
    // The extra spacer above is shown on step 1 only (panes sit lower under
    // the params card); apply_preview_metrics toggles it.
    m_preview_step_spacer = new wxPanel(parent, wxID_ANY, wxDefaultPosition,
                                        wxSize(0, FromDIP(24)));
    m_preview_step_spacer->SetBackgroundColour(parent->GetBackgroundColour());
    sizer->Add(m_preview_step_spacer, 0, wxLEFT | wxRIGHT, FromDIP(20));
    sizer->Add(preview_container, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    m_preview_canvas->set_simplified_mode(TexturePreviewCanvas::RenderMode::MultiColor);
}

void TextureImportDialog::build_params_panel(wxWindow* parent, wxSizer* sizer)
{
    const wxColour label_fg = StateColor::darkModeColorFor(wxColour("#18181B"));

    m_params_panel = new wxPanel(parent, wxID_ANY);
    m_params_panel->SetBackgroundColour(parent->GetBackgroundColour());

    auto* card_sizer = new wxBoxSizer(wxVERTICAL);

    auto add_param_label = [&](wxWindow* parent_win, const wxString& text) {
        auto* lbl = new wxStaticText(parent_win, wxID_ANY, text);
        wxFont f = lbl->GetFont();
        f.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
        f.SetWeight(wxFONTWEIGHT_MEDIUM);
        lbl->SetFont(f);
        lbl->SetForegroundColour(label_fg);
        return lbl;
    };

    // ---- Row 1: color count + presets --------------------------------------
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* group = new wxBoxSizer(wxHORIZONTAL);
        group->Add(add_param_label(m_params_panel, _L("Color Count")),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

        m_color_slider = new AccentSlider(m_params_panel, m_param_color_count, 1, (int)max_filament_count());
        m_color_slider->SetMinSize(wxSize(FromDIP(205), FromDIP(24)));
        // Plain numeric input (TextInput container + native text control):
        // digits only, out-of-range input stays visible for the inline red
        // error; no spin arrows (2026-11 design / doc 3.3.1 note 3).
        m_color_spin = new TextInput(m_params_panel, wxString::Format("%d", m_param_color_count),
                                     "", "", wxDefaultPosition,
                                     wxSize(FromDIP(42), FromDIP(24)), wxTE_PROCESS_ENTER);

        m_color_slider->Bind(wxEVT_SLIDER, &TextureImportDialog::on_color_slider_changed, this);
        m_color_spin->Bind(wxEVT_TEXT, &TextureImportDialog::on_color_spin_text_changed, this);

        group->Add(m_color_slider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        group->Add(m_color_spin, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(group, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();

        // Doc section 3.3.1 note 1: quick presets are 4/8/16/32/64; no Auto shortcut.
        m_btn_color_4    = new Button(m_params_panel, "4");
        m_btn_color_4->SetId(ID_COLOR_4);
        m_btn_color_8    = new Button(m_params_panel, "8");
        m_btn_color_8->SetId(ID_COLOR_8);
        m_btn_color_16   = new Button(m_params_panel, "16");
        m_btn_color_16->SetId(ID_COLOR_16);
        m_btn_color_32   = new Button(m_params_panel, "32");
        m_btn_color_32->SetId(ID_COLOR_32);
        m_btn_color_64   = new Button(m_params_panel, "64");
        m_btn_color_64->SetId(ID_COLOR_64);

        {
            StateColor preset_bg(
                std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed | StateColor::Checked),
                std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered | StateColor::Checked),
                std::pair<wxColour, int>(wxColour("#00A48E"), StateColor::Checked),
                std::pair<wxColour, int>(wxColour("#DBDBDB"), StateColor::Pressed),
                std::pair<wxColour, int>(wxColour("#E1E1E1"), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour("#EBEBEB"), StateColor::Normal));
            StateColor preset_bd(
                std::pair<wxColour, int>(wxColour("#00A48E"), StateColor::Checked),
                std::pair<wxColour, int>(wxColour("#EBEBEB"), StateColor::Normal));
            StateColor preset_text(
                std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Checked),
                std::pair<wxColour, int>(wxColour("#71717A"), StateColor::Normal));

            for (auto* btn : {m_btn_color_4, m_btn_color_8, m_btn_color_16, m_btn_color_32, m_btn_color_64}) {
                btn->SetCornerRadius(FromDIP(4));
                btn->SetMinSize(wxSize(FromDIP(56), FromDIP(24)));
                btn->SetBackgroundColor(preset_bg);
                btn->SetBorderColor(preset_bd);
                btn->SetTextColor(preset_text);
            }
        }

        update_color_count_preset_buttons();

        auto* pills = new wxBoxSizer(wxHORIZONTAL);
        for (auto* btn : {m_btn_color_4, m_btn_color_8, m_btn_color_16, m_btn_color_32, m_btn_color_64})
            pills->Add(btn, 0, wxLEFT, btn == m_btn_color_4 ? 0 : FromDIP(kColorPresetGapDip));
        row->Add(pills, 0, wxALIGN_CENTER_VERTICAL);

        card_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ---- Divider ------------------------------------------------------------
    {
        auto* divider = new StaticLine(m_params_panel);
        divider->SetLineColour(wxColour("#DEDEE0"));
        card_sizer->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }

    // ---- Row 2: smooth level + gap area -------------------------------------
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto build_slider_group = [&](const wxString& label, AccentSlider*& slider, TextInput*& input,
                                      int value, int min_v, int max_v, int slider_width_dip,
                                      void (TextureImportDialog::*on_slider)(wxCommandEvent&),
                                      void (TextureImportDialog::*on_text)(wxCommandEvent&)) {
            auto* group = new wxBoxSizer(wxHORIZONTAL);
            group->Add(add_param_label(m_params_panel, label),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            slider = new AccentSlider(m_params_panel, value, min_v, max_v);
            slider->SetMinSize(wxSize(FromDIP(slider_width_dip), FromDIP(24)));
            // Plain numeric input, same as the color-count one (no arrows).
            input = new TextInput(m_params_panel, wxString::Format("%d", value),
                                  "", "", wxDefaultPosition,
                                  wxSize(FromDIP(42), FromDIP(24)), wxTE_PROCESS_ENTER);
            slider->Bind(wxEVT_SLIDER, on_slider, this);
            input->Bind(wxEVT_TEXT, on_text, this);
            group->Add(slider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            group->Add(input, 0, wxALIGN_CENTER_VERTICAL);
            return group;
        };

        row->Add(build_slider_group(_L("Smooth Level"), m_smooth_slider, m_smooth_spin,
                                    m_param_smooth, 0, 10, 208,
                                    &TextureImportDialog::on_smooth_slider_changed,
                                    &TextureImportDialog::on_smooth_spin_text_changed),
                 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();
        row->Add(build_slider_group(_L("Gap Area (%)"), m_gap_slider, m_gap_spin,
                                    gap_value_to_slider(m_param_gap_area), 0,
                                    gap_value_to_slider(kGapAreaMaxPercent), kGapSliderWidthDip,
                                    &TextureImportDialog::on_gap_slider_changed,
                                    &TextureImportDialog::on_gap_spin_text_changed),
                 0, wxALIGN_CENTER_VERTICAL);

        card_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    }


    // ---- Row 3: dirty hint + Apply -------------------------------------------
    {
        m_btn_apply = new Button(m_params_panel, _L("Apply"));
        m_btn_apply->SetId(ID_BTN_APPLY);
        m_btn_apply->SetCornerRadius(FromDIP(8));
        m_btn_apply->SetMinSize(wxSize(FromDIP(56), FromDIP(32)));
        style_apply_button(false);

        // Defer attaching the Apply tooltip until the dialog has actually
        // been shown. On macOS, AppKit creates NSTrackingArea and dispatches a
        // synthetic mouseEntered: as soon as the window first becomes visible,
        // which would otherwise pop the native tooltip without the user actually
        // hovering when the cursor happens to land on this button as the dialog
        // appears.
        Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
            e.Skip();
            if (!e.IsShown() || m_initial_tooltips_set)
                return;
            m_initial_tooltips_set = true;
            CallAfter([this]() {
                if (m_btn_apply)
                    m_btn_apply->SetToolTip(_L("Convert texture to painting using the specified color count and smooth level"));
            });
        });

        m_hint_label = new wxStaticText(m_params_panel, wxID_ANY,
            _L("Tip: parameters changed, click \"Apply\" on the right to take effect"));
        {
            wxFont hf = m_hint_label->GetFont();
            hf.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
            hf.SetWeight(wxFONTWEIGHT_MEDIUM);
            m_hint_label->SetFont(hf);
        }
        m_hint_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF9F00")));
        m_hint_label->Hide();

        // Hint pinned LEFT (2026-11 design), Apply pinned right. The red
        // out-of-range error lives in the SAME slot as the hint (only one of
        // them is ever visible), so toggling it never shifts the layout and
        // the hint/apply positions stay put (2026-11 feedback).
        m_gap_warning_label = new wxStaticText(m_params_panel, wxID_ANY, wxEmptyString);
    {
        wxFont gf = m_gap_warning_label->GetFont();
        gf.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
        gf.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_gap_warning_label->SetFont(gf);
    }
    m_gap_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF383C")));
    m_gap_warning_label->Hide();

    m_smooth_warning_label = new wxStaticText(m_params_panel, wxID_ANY, wxEmptyString);
    {
        wxFont sf = m_smooth_warning_label->GetFont();
        sf.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
        sf.SetWeight(wxFONTWEIGHT_MEDIUM);
        m_smooth_warning_label->SetFont(sf);
    }
    m_smooth_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF383C")));
    m_smooth_warning_label->Hide();

    m_color_warning_label = new wxStaticText(m_params_panel, wxID_ANY, wxEmptyString);
        {
            wxFont wf = m_color_warning_label->GetFont();
            wf.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
            wf.SetWeight(wxFONTWEIGHT_MEDIUM);
            m_color_warning_label->SetFont(wf);
        }
        m_color_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF383C")));
        m_color_warning_label->Hide();

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(m_hint_label, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(m_color_warning_label, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(m_gap_warning_label, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(m_smooth_warning_label, 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();
        row->Add(m_btn_apply, 0, wxALIGN_CENTER_VERTICAL);
        card_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }

    // Abandoned edits: an empty box (everything deleted, focus moved away)
    // refills from the parameter so the box and the slider never disagree.
    // clear_error is invoked after every programmatic rewrite: ChangeValue()
    // fires no text event, so the red-error flags would otherwise keep
    // pointing at the previously typed value (2026-11 feedback: refilled
    // "5.00" still showed the stale red hint).
    auto refill_on_kill_focus = [this](TextInput* input, int TextureImportDialog::*member,
                                       std::function<void()> clear_error) {
        if (!input) return;
        auto* tc = input->GetTextCtrl();
        if (!tc) return;
        // Abandoned edit: empty box refills from the parameter.
        tc->Bind(wxEVT_KILL_FOCUS, [this, tc, member, clear_error](wxFocusEvent& evt) {
            evt.Skip();
            if (tc->GetValue().empty()) {
                tc->ChangeValue(wxString::Format("%d", this->*member));
                if (clear_error)
                    clear_error();
            }
        });
        // Enter commits: only an IN-RANGE text normalizes to the canonical
        // "%d" form. Out-of-range or partial text stays as typed with the
        // red error - no silent restore (2026-11 unified rule).
        tc->Bind(wxEVT_TEXT_ENTER, [this, tc, member, clear_error](wxCommandEvent&) {
            long raw = 0;
            if (!tc->GetValue().ToLong(&raw) || raw != (long)(this->*member))
                return; // raw != clamped param means out of range: keep it
            tc->ChangeValue(wxString::Format("%d", this->*member));
            tc->SetInsertionPointEnd();
            if (clear_error)
                clear_error();
        });
        // Digit-only filter; navigation/editing/modifier keys pass through.
        tc->Bind(wxEVT_CHAR, [](wxKeyEvent& evt) {
            const int key = evt.GetKeyCode();
            if (wxIsdigit(key) || key == WXK_BACK || key == WXK_DELETE
                || key == WXK_LEFT || key == WXK_RIGHT || key == WXK_HOME || key == WXK_END
                || evt.ControlDown() || evt.AltDown())
                evt.Skip();
        });
    };
    refill_on_kill_focus(m_color_spin, &TextureImportDialog::m_param_color_count,
        [this]() {
            m_color_max_exceeded = false;
            update_color_count_warning();
            update_confirm_button_state();
        });
    refill_on_kill_focus(m_smooth_spin, &TextureImportDialog::m_param_smooth,
        [this]() {
            m_smooth_out_of_range     = false;
            m_smooth_raw_out_of_range = false;
            update_smooth_input_error_style();
            update_confirm_button_state();
        });
    {
        // Gap value is a double; reuse the same abandon-refill behaviour.
        auto* tc = m_gap_spin ? m_gap_spin->GetTextCtrl() : nullptr;
        if (tc) {
            // ChangeValue() fires no text event, so every programmatic
            // rewrite of the gap box refreshes the error state explicitly -
            // otherwise the red hint keeps pointing at the previously typed
            // out-of-range value after the refill (2026-11 feedback).
            auto clear_gap_error = [this]() {
                m_gap_out_of_range = false;
                update_gap_input_error_style();
                if (m_gap_warning_label)
                    m_gap_warning_label->Show(false);
                update_confirm_button_state();
            };
            // Same abandon-refill rule as the int boxes: refill ONLY an
            // empty box; typed text - including out-of-range - stays as is
            // and keeps the red error (2026-11 unified rule).
            tc->Bind(wxEVT_KILL_FOCUS, [this, tc, clear_gap_error](wxFocusEvent& evt) {
                evt.Skip();
                if (tc->GetValue().empty()) {
                    tc->ChangeValue(wxString::Format("%.2f", m_param_gap_area));
                    clear_gap_error();
                }
            });
            // Commit on Enter: only an IN-RANGE text formats to two decimals;
            // out-of-range text stays as typed - no silent restore
            // (2026-11 unified rule).
            tc->Bind(wxEVT_TEXT_ENTER, [this, tc, clear_gap_error](wxCommandEvent&) {
                double raw = 0.0;
                if (!tc->GetValue().ToDouble(&raw)
                    || std::round(raw * 100.0) / 100.0 != m_param_gap_area)
                    return; // raw != clamped param means out of range: keep it
                tc->ChangeValue(wxString::Format("%.2f", m_param_gap_area));
                tc->SetInsertionPointEnd();
                clear_gap_error();
            });
        }
    }

    // Click-to-focus forwarding: a click anywhere on a spin box focuses its
    // text control. The box's own geometry already tries to cover clicks with
    // the text control, but layout passes (e.g. after editing another box)
    // can leave stale geometry behind — this makes focusing deterministic
    // regardless (2026-11 feedback: after editing the second box, the first
    // box could no longer be focused by clicking).
    auto forward_focus_to_text = [](TextInput* input) {
        if (!input) return;
        input->Bind(wxEVT_LEFT_DOWN, [input](wxMouseEvent& evt) {
            evt.Skip();
            if (auto* tc = input->GetTextCtrl())
                tc->SetFocus();
        });
    };
    forward_focus_to_text(m_color_spin);
    forward_focus_to_text(m_smooth_spin);
    forward_focus_to_text(m_gap_spin);

    m_params_panel->SetSizer(card_sizer);
    // Space the whole params card (color row / smooth+gap rows / hint+apply)
    // away from the step indicator (2026-11 feedback: it sat too close).
    sizer->AddSpacer(FromDIP(16));
    sizer->Add(m_params_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));
    // Only 4px under the params card: on step 2 the preview sits here and
    // must hug the step indicator (2026-11 pixel audit showed a 64px gap).
    sizer->AddSpacer(FromDIP(4));
}

void TextureImportDialog::build_mapping_panel(wxWindow* parent, wxSizer* sizer)
{
    // Step-2 host panel (2026-11 step-2 Figma): "Filament Matching" header with
    // the CMYW auto-mix pill and the wiki link, then the mapping list inside a
    // #F8F7F7 rounded card. The preview stays above this panel in the content
    // sizer; the color-count captions are a step-1-only element.
    m_mapping_panel = new wxPanel(parent, wxID_ANY);
    m_mapping_panel->SetBackgroundColour(parent->GetBackgroundColour());
    auto* panel_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- Header row: [耗材匹配][mode dropdown] …… [wiki link] ----
    {
        auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);

        auto* lbl_matching = new wxStaticText(m_mapping_panel, wxID_ANY, _L("Filament Matching"));
        wxFont hf = lbl_matching->GetFont();
        hf.SetPointSize(texture_font_pt(10)); // 14px at default MSW DPI
        hf.SetWeight(wxFONTWEIGHT_MEDIUM);
        lbl_matching->SetFont(hf);
        lbl_matching->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#18181B")));
        header_sizer->Add(lbl_matching, 0, wxALIGN_CENTER_VERTICAL);

        // Mix-mode dropdown (2026-11 design, 148x28 border #DEDEE0): custom
        // trigger + popup instead of the shared ComboBox — its DropDown
        // auto-positions (and flips upward near the dialog bottom), while the
        // design requires the list to always open DOWNWARD.
        // 2026-11 Figma: 149x30, border #DEDEE0, padding 12/16, gap 8.
        m_mix_mode_trigger = new wxPanel(m_mapping_panel, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxTAB_TRAVERSAL);
        // Width follows the longest label: the Figma 149px box fits the zh-CN
        // text, but "Direct Filament Mapping" at the design size (and wider
        // locales) needs more room than 149 - 36 - 12, so a fixed width clips.
        texture_update_mix_mode_metrics(m_mix_mode_trigger);
        m_mix_mode_trigger->SetBackgroundColour(m_mapping_panel->GetBackgroundColour());
        m_mix_mode_trigger->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_mix_mode_trigger->SetCursor(wxCursor(wxCURSOR_HAND));
        m_mix_mode_trigger->SetToolTip(_L("Choose the one-click auto-mix mode for texture color import"));
        m_mix_mode_trigger->Bind(wxEVT_PAINT, [this](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(p->GetBackgroundColour()));
            dc.Clear();
            dc.SetBrush(wxBrush(p->GetBackgroundColour()));
            texture_draw_edge_border(dc, sz, StateColor::darkModeColorFor(wxColour("#DEDEE0")));

            const int bx = p->FromDIP(12);
            texture_draw_glyph(dc, p, bx, (sz.y - p->FromDIP(16)) / 2.0,
                StateColor::darkModeColorFor(wxColour("#949494")));

            wxFont tf = p->GetFont();
            tf.SetPointSize(texture_font_pt(kMixModeFontPt));
            tf.SetWeight(wxFONTWEIGHT_MEDIUM);
            dc.SetFont(tf);
            dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#18181B")));
            const wxString label = m_mix_mode_index == 1 ? _L("CMYW Auto Mix")
                                                         : _L("Direct Filament Mapping");
            const wxSize tsz = dc.GetTextExtent(label);
            // Label right of the chevron box + 8px gap (2026-11 layout).
            dc.DrawText(label, bx + p->FromDIP(16) + p->FromDIP(8), (sz.y - tsz.y) / 2);
        });
        m_mix_mode_trigger->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_mix_mode_popup) {
                // Open already: dismiss (OnDismiss self-destroys and clears
                // the guard via the wxEVT_DESTROY bind).
                m_mix_mode_popup->Dismiss();
                return;
            }
            show_mix_mode_popup();
        });
        header_sizer->Add(m_mix_mode_trigger, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

        header_sizer->AddStretchSpacer();

        auto* wiki_link = new wxStaticText(m_mapping_panel, wxID_ANY, _L("What is CMYW mixing?"));
        {
            wxFont lf = wiki_link->GetFont();
            lf.SetPointSize(texture_font_pt(9)); // 12px at default MSW DPI
            wiki_link->SetFont(lf);
        }
        wiki_link->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#00A48E")));
        wiki_link->SetCursor(wxCursor(wxCURSOR_HAND));
        wiki_link->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent&) {
            wxLaunchDefaultBrowser(CMYW_WIKI_URL);
        });
        header_sizer->Add(wiki_link, 0, wxALIGN_CENTER_VERTICAL);

        // Figma: the header row (28px content) carries 16px vertical padding
        // and a further 12px gap to the list card.
        panel_sizer->AddSpacer(FromDIP(20));
        panel_sizer->Add(header_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        panel_sizer->AddSpacer(FromDIP(8));
    }

    // ---- Mapping list card (#F8F7F7, radius 8, 16px padding) ----
    {
        const wxColour card_bg = StateColor::darkModeColorFor(wxColour("#F8F7F7"));
        auto* card = new wxPanel(m_mapping_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        card->SetBackgroundColour(card_bg);
        card->SetBackgroundStyle(wxBG_STYLE_PAINT);
        card->Bind(wxEVT_PAINT, [this, card_bg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.Clear();
            dc.SetBrush(wxBrush(card_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(8));
        });

        m_mapping_scroll = new wxScrolledWindow(card, wxID_ANY, wxDefaultPosition,
                                                wxSize(-1, FromDIP(188)));
        m_mapping_scroll->SetScrollRate(0, FromDIP(20));
        m_mapping_scroll->SetBackgroundColour(card_bg);
        m_mapping_scroll->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

        m_mapping_sizer = new wxBoxSizer(wxVERTICAL);
        m_mapping_scroll->SetSizer(m_mapping_sizer);

        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        // Equal 12px padding above and below the row list (the last row sat
        // ~4px from the card bottom, tighter than the top; 2026-11 feedback).
        card_sizer->AddSpacer(FromDIP(12));
        card_sizer->Add(m_mapping_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        card_sizer->AddSpacer(FromDIP(12));
        card->SetSizer(card_sizer);

        // Fixed height (Figma 204px card) — never stretch, so leftover dialog
        // space stays BELOW the card as clean margin instead of hollow room
        // inside it.
        panel_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    }

    m_mapping_panel->SetSizer(panel_sizer);
    m_mapping_panel->Hide();
    sizer->Add(m_mapping_panel, 1, wxEXPAND | wxLEFT | wxRIGHT);
    sizer->AddSpacer(FromDIP(16));
}

void TextureImportDialog::build_bottom_buttons(wxSizer* sizer)
{
    // Bottom bar (2026-11 design): white with a top hairline, right-aligned
    // 160x36 buttons. Step 1 shows Skip Matching + Next; step 2 shows Back +
    // Confirm (interim until the step-2 Figma lands).
    const wxColour bar_bg = StateColor::darkModeColorFor(*wxWHITE);
    const wxColour bar_border = StateColor::darkModeColorFor(wxColour("#DEDEE0"));

    auto* bar = new wxPanel(this, wxID_ANY);
    bar->SetBackgroundColour(bar_bg);
    bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
    bar->Bind(wxEVT_PAINT, [bar_bg, bar_border](wxPaintEvent& e) {
        auto* p = static_cast<wxPanel*>(e.GetEventObject());
        wxAutoBufferedPaintDC dc(p);
        wxSize sz = p->GetClientSize();
        dc.SetBackground(wxBrush(bar_bg));
        dc.Clear();
        dc.SetPen(wxPen(bar_border, 1));
        dc.DrawLine(0, 0, sz.x, 0);
    });

    auto* bar_sizer = new wxBoxSizer(wxVERTICAL);

    m_drop_warning_label = new wxStaticText(bar, wxID_ANY,
        wxString::Format(
            _L_PLURAL("The project supports up to %d filament. Extra filaments will be discarded.",
                      "The project supports up to %d filaments. Extra filaments will be discarded.",
                      (int)max_filament_count()),
            (int)max_filament_count()));
    m_drop_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF6F00")));
    m_drop_warning_label->SetFont(texture_import_section_title_font(bar));
    m_drop_warning_label->Hide();
    bar_sizer->Add(m_drop_warning_label, 0, wxALIGN_LEFT | wxTOP | wxLEFT, FromDIP(8));

    auto style_ghost_button = [](Button* btn) {
        btn->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour("#E1E1E1"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal)));
        btn->SetBorderColor(StateColor(
            std::pair<wxColour, int>(wxColour("#DEDEE0"), StateColor::Normal)));
        btn->SetTextColor(StateColor(
            std::pair<wxColour, int>(wxColour("#6B6B6A"), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour("#71717A"), StateColor::Normal)));
    };

    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_skip = new Button(bar, _L("Skip Matching"));
    m_btn_skip->SetId(ID_BTN_SKIP);
    m_btn_skip->SetToolTip(_L("Skip filament mapping and import as a single-color model"));
    m_btn_skip->SetCornerRadius(FromDIP(4));
    m_btn_skip->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
    style_ghost_button(m_btn_skip);

    m_btn_back = new Button(bar, _L("Previous Step"));
    m_btn_back->SetId(ID_BTN_BACK);
    m_btn_back->SetToolTip(_L("Return to the color simplification step"));
    m_btn_back->SetCornerRadius(FromDIP(4));
    m_btn_back->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
    style_ghost_button(m_btn_back);
    m_btn_back->Bind(wxEVT_BUTTON, &TextureImportDialog::on_back_clicked, this);

    m_btn_reset = new Button(bar, _L("Reset"));
    m_btn_reset->SetId(ID_BTN_RESET);
    m_btn_reset->SetToolTip(_L("Reset filament mapping to the automatic match"));
    m_btn_reset->SetCornerRadius(FromDIP(4));
    m_btn_reset->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
    style_ghost_button(m_btn_reset);
    m_btn_reset->Bind(wxEVT_BUTTON, &TextureImportDialog::on_reset_clicked, this);

    m_btn_next = new Button(bar, _L("Next"));
    m_btn_next->SetId(ID_BTN_NEXT);
    m_btn_next->SetToolTip(_L("Apply the current parameters and continue to filament mapping"));
    m_btn_next->SetCornerRadius(FromDIP(4));
    m_btn_next->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
    apply_accent_button_colours(m_btn_next);
    m_btn_next->Bind(wxEVT_BUTTON, &TextureImportDialog::on_next_clicked, this);

    m_btn_ok = new Button(bar, _L("Confirm"));
    m_btn_ok->SetId(wxID_OK);
    m_btn_ok->SetCornerRadius(FromDIP(4));
    m_btn_ok->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
    apply_accent_button_colours(m_btn_ok);

    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_skip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    btn_sizer->Add(m_btn_next, 0, wxALIGN_CENTER_VERTICAL);
    btn_sizer->Add(m_btn_back, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    btn_sizer->Add(m_btn_reset, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    btn_sizer->Add(m_btn_ok,   0, wxALIGN_CENTER_VERTICAL);

    bar_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(12));
    bar->SetSizer(bar_sizer);

    sizer->Add(bar, 0, wxEXPAND);
    update_step_buttons();
}

void TextureImportDialog::update_step_buttons()
{
    if (!m_btn_skip) return;
    const bool simplify = (m_step == ImportStep::Simplify);
    // Figma places Skip Matching on step 1's bottom bar; step 2 shows Back +
    // Reset + Confirm.
    m_btn_skip->Show(simplify);
    m_btn_next->Show(simplify);
    m_btn_back->Show(!simplify);
    m_btn_reset->Show(!simplify);
    m_btn_ok->Show(!simplify);
    Layout();
}

void TextureImportDialog::switch_step(ImportStep step)
{
    if (m_step == step) return;
    m_step = step;

    const bool simplify = (step == ImportStep::Simplify);

    // Flicker fix: freeze the WHOLE dialog while both the panel swap and the
    // canvas resize happen — the previous sequence (Show/Hide -> canvas
    // SetMinSize/SetMaxSize -> Layout) repainted mid-transition and striped
    // the GL canvas. One thaw after everything settled = one clean repaint.
    Freeze();

    if (m_params_panel)  m_params_panel->Show(simplify);
    if (m_mapping_panel) m_mapping_panel->Show(!simplify);

    // One canvas for both steps: step 1 previews the cluster colors, step 2
    // previews the filament-mapped result.
    if (m_preview_canvas) {
        m_preview_canvas->set_simplified_mode(simplify
            ? TexturePreviewCanvas::RenderMode::MultiColor
            : TexturePreviewCanvas::RenderMode::FilamentMap);
        if (!simplify)
            update_filament_color_map();
    }

    apply_preview_metrics();
    Layout();

    update_step_indicator();
    update_step_buttons();
    update_preview_color_labels();
    update_drop_warning_visibility();
    update_ui_for_state();

    Thaw();
}

void TextureImportDialog::on_next_clicked(wxCommandEvent&)
{
    if (m_state == TextureImportState::Computing || !color_count_input_valid())
        return;

    // Dirty parameters (or no result yet): recompute first, advance once the
    // async completion lands. Otherwise jump straight to the mapping step.
    // Dirty-but-not-applied edits keep the PREVIOUS applied values (2026-11
    // feedback): proceed to mapping without recomputing. Recompute only when
    // there is no result at all yet.
    if (!has_valid_result()) {
        m_pending_step_advance = true;
        start_computation();
        return;
    }
    switch_step(ImportStep::Mapping);
}

void TextureImportDialog::on_back_clicked(wxCommandEvent&)
{
    if (m_state == TextureImportState::Computing)
        return;
    m_pending_step_advance = false;
    switch_step(ImportStep::Simplify);
}

// Step-2 "Reset": undo any one-click mix (snapshot) or manual reassignments
// and return to the automatic match baseline.
void TextureImportDialog::on_reset_clicked(wxCommandEvent&)
{
    if (m_state == TextureImportState::Computing)
        return;

    if (m_auto_mix_snapshot) {
        reset_auto_mix();
        return;
    }
    if (m_painted.cluster_colors.empty())
        return;

    do_auto_match();
    compact_used_virtual_filaments();
    rebuild_mapping_rows();
    update_filament_color_map();
    update_drop_warning_visibility();
    update_confirm_button_state();
    set_mix_mode_ui(0);
    Layout();
}

// ---- State machine ----

void TextureImportDialog::set_state(TextureImportState new_state)
{
    m_state = new_state;
    update_ui_for_state();
    if (new_state == TextureImportState::Ready && m_auto_mix_pending) {
        m_auto_mix_pending = false;
        apply_auto_standard_mix(TextureAutoMixMode::CMYW);
    }
}

void TextureImportDialog::update_ui_for_state()
{
    bool computing = (m_state == TextureImportState::Computing);
    bool ready     = (m_state == TextureImportState::Ready);
    bool idle      = (m_state == TextureImportState::Idle);
    bool valid     = has_valid_result();

    m_color_slider->Enable(!computing);
    m_color_spin->Enable(!computing);
    m_smooth_slider->Enable(!computing);
    m_smooth_spin->Enable(!computing);
    if (m_gap_slider) m_gap_slider->Enable(!computing);
    if (m_gap_spin)   m_gap_spin->Enable(!computing);
    m_btn_color_4->Enable(!computing);
    m_btn_color_8->Enable(!computing);
    m_btn_color_16->Enable(!computing);
    m_btn_color_32->Enable(!computing);
    m_btn_color_64->Enable(!computing);
    if (m_mix_mode_trigger) {
        m_mix_mode_trigger->Enable(!computing);
        m_mix_mode_trigger->Refresh();
    }

    m_btn_ok->Enable(ready && valid);
    m_btn_skip->Enable(ready || idle);
    // "Next" follows the same input gating as Apply; "Back" only waits out a
    // running computation.
    if (m_btn_next)
        m_btn_next->Enable(!computing && color_count_input_valid() && !m_color_max_exceeded
                           && !m_gap_out_of_range && !m_smooth_out_of_range);
    if (m_btn_back)
        m_btn_back->Enable(!computing);

    m_preview_canvas->set_computing_overlay(computing);

    if (ready && valid)
        // 2026-11 spec: confirm always reads as clickable accent here -
        // unapplied param edits do not mute it (they are discarded on
        // confirm, the last applied result is exported).
        style_confirm_button(false);
    else if (m_hint_label)
        m_hint_label->Hide();

    // Apply is clickable (accent #009688) only while it would apply something:
    // parameters diverge from the preview AND the input is valid. Grey =
    // disabled (2026-11 design), also while the color count exceeds the
    // original palette (doc section 3.3.1 note 4) or a computation runs.
    if (m_gap_warning_label) {
        m_gap_warning_label->SetLabel(_L("Notice: Gap area out of range (0.00-5.00%)."));
        m_gap_warning_label->Show(m_gap_out_of_range);
    }
    style_apply_button(!computing && color_count_input_valid() && !m_color_max_exceeded
                       && !m_gap_out_of_range && !m_smooth_out_of_range && is_params_dirty());

    m_btn_ok->Refresh();
    Layout();
}

// ---- Async computation ----

void TextureImportDialog::start_computation(bool auto_color, bool initial)
{
    cancel_computation();

    m_cancel_flag = false;
    m_current_computation_initial = initial;
    m_current_computation_auto_color = auto_color;
    if (initial) {
        m_initial_computation_pending = true;
        m_initial_computation_cancelled = false;
        m_initial_computation_failed = false;
    }
    set_state(TextureImportState::Computing);

    bool silent_initial = initial && static_cast<bool>(m_initial_cancel_callback);
    if (!silent_initial) {
        m_progress_dlg = new ProgressDialog(
            _L("Processing"), _L("Computing texture colors..."),
            100, initial ? GetParent() : this, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
    }

    Slic3r::TexturePaintingSettings settings;
    settings.target_colors_num = auto_color ? 0 : (size_t)m_param_color_count;
    settings.smooth_weight     = m_param_smooth / 10.0;
    settings.mesh_repair_decision = m_mesh_repair_decision;
    settings.mesh_repair_callback = [](const indexed_triangle_set& mesh,
                                       indexed_triangle_set&       repaired_mesh,
                                       std::function<void(const char*, unsigned)> progress_callback,
                                       std::function<bool()>       cancel_callback,
                                       std::string*                error_message) -> bool {
        if (progress_callback)
            progress_callback(_u8L("Repairing mesh").c_str(), 0);

        struct Canceled {};
        TriangleMesh tm(mesh);
        try {
            if (!repair_mesh(tm, nullptr, error_message, [&cancel_callback](int) {
                    if (cancel_callback && cancel_callback())
                        throw Canceled();
                }))
                return false;
        } catch (const Canceled &) {
            return false;
        }
        repaired_mesh = tm.its;
        if (progress_callback)
            progress_callback(_u8L("Repairing mesh").c_str(), 100);
        return true;
    };

    Slic3r::TexturedMesh mesh_copy = m_textured_mesh;
    wxEvtHandler* handler = this;

    m_worker = std::make_unique<std::thread>([this, settings, mesh_copy, handler]() {
        Slic3r::PaintedMesh result;

        auto progress_cb = [handler](int percent, const char*) {
            auto* evt = new wxCommandEvent(EVT_TEXTURE_COMPUTE_PROGRESS);
            evt->SetInt(percent);
            wxQueueEvent(handler, evt);
        };

        auto cancel_cb = [this]() -> bool {
            return m_cancel_flag.load();
        };

        auto worker_settings = settings;
        bool mesh_repair_decision_required = false;
        worker_settings.mesh_repair_decision_required = &mesh_repair_decision_required;
        bool ok;
        if (!mesh_copy.precomputed_face_colors.empty()) {
            ok = Slic3r::face_colors_to_painting(
                mesh_copy, result, worker_settings, progress_cb, cancel_cb);
        } else {
            ok = Slic3r::texture_to_painting(mesh_copy, result, worker_settings, progress_cb, cancel_cb);
        }

        if (m_cancel_flag.load()) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_ERROR));
            return;
        }

        if (!ok && mesh_repair_decision_required) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_MESH_REPAIR_DECISION));
            return;
        }

        // Same thread builds the gap merge preview (patches + precomputed merge
        // events) so the UI thread only replays it; the event list is what makes
        // the slider cheap to drag.
        std::unique_ptr<GapPreviewState> gap_state;
        if (ok) {
            const auto t0 = std::chrono::steady_clock::now();
            gap_state = make_gap_preview_state(result);
            BOOST_LOG_TRIVIAL(info)
                << "TextureImport: gap preview build took="
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count()
                << " ms patches=" << gap_state->color_patches.size()
                << " merge_events=" << gap_state->merge_events.size();
        }

        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            m_pending_result       = std::move(result);
            m_pending_gap_preview  = std::move(gap_state);
        }

        if (ok) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_DONE));
        } else {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_ERROR));
        }
    });
}


void TextureImportDialog::cancel_computation()
{
    m_cancel_flag = true;
    if (m_worker && m_worker->joinable())
        m_worker->join();
    m_worker.reset();

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    if (m_current_computation_initial) {
        m_initial_computation_cancelled = true;
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
    }
}

void TextureImportDialog::on_computation_progress(wxCommandEvent& evt)
{
    if (m_progress_dlg) {
        if (!m_progress_dlg->Update(evt.GetInt()))
            m_cancel_flag = true;
    } else if (m_current_computation_initial && m_initial_progress_callback) {
        if (!m_initial_progress_callback(evt.GetInt()))
            m_cancel_flag = true;
    }
}

void TextureImportDialog::on_computation_complete(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_painted = std::move(m_pending_result);
    }

    // Install the worker-built gap patch set, replay it at the current slider
    // value (a recompute keeps the user's gap setting — fork divergence,
    // upstream resets the slider instead) and fold the merged colors into
    // m_painted BEFORE the match chain below, so matching, rows and the
    // filament-map view are all built on the final merged palette. The merge
    // must not reduce the palette, so the threshold snaps down to the largest
    // value keeping every color alive, and the slider follows.
    {
        std::unique_ptr<GapPreviewState> pending_gap;
        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            pending_gap = std::move(m_pending_gap_preview);
        }
        const double cap = m_keep_colors
            ? (pending_gap ? pending_gap->max_merge_percent : kGapAreaMaxPercent)
            : kGapAreaMaxPercent;
        set_gap_value(std::min(m_param_gap_area, cap), true);
        install_gap_preview(std::move(pending_gap));
        fold_gap_into_painted();
    }

    int actual_colors = (int)m_painted.cluster_colors.size();
    if (actual_colors >= 1 && actual_colors <= (int)max_filament_count()) {
        set_color_count_value(actual_colors, true);
    }

    m_preview_canvas->set_painted_mesh_data(m_painted.vertices, m_painted.indices);
    m_preview_canvas->set_face_colors(m_painted.face_colors);

    // A fresh texture computation replaces m_painted, so virtual filaments from
    // the previous computation must not consume capacity when deciding whether
    // this run drops extra colors. Rebuild virtual filaments from this result.
    m_current_matches.clear();
    if (m_filament_colors_rgba.size() > m_existing_filament_count)
        m_filament_colors_rgba.resize(m_existing_filament_count);
    if (m_filament_color_strs.size() > m_existing_filament_count)
        m_filament_color_strs.resize(m_existing_filament_count);
    if (m_filament_names.size() > m_existing_filament_count)
        m_filament_names.resize(m_existing_filament_count);
    if (m_filament_entries.size() > m_existing_filament_count)
        m_filament_entries.resize(m_existing_filament_count);
    while (m_filament_entries.size() < m_existing_filament_count) {
        TextureFilamentEntry entry;
        entry.kind = TextureFilamentKind::ExistingPhysical;
        entry.dialog_index = (int)m_filament_entries.size();
        entry.project_config_index = m_filament_entries.size();
        m_filament_entries.push_back(entry);
    }
    for (size_t i = 0; i < m_filament_entries.size(); ++i) {
        m_filament_entries[i].dialog_index = (int)i;
        m_filament_entries[i].color_hex = i < m_filament_color_strs.size() ?
            texture_normalize_color_hex(m_filament_color_strs[i]) : "#808080";
        m_filament_entries[i].name = i < m_filament_names.size() ?
            m_filament_names[i] : GUI::format(_u8L("Filament %d"), i + 1);
    }
    m_new_filament_colors.clear();
    m_new_filament_preset_names.clear();
    m_new_mixed_filaments.clear();

    do_auto_match();
    compact_used_virtual_filaments();
    sort_current_matches_by_filament_index();
    update_filament_color_map();
    rebuild_mapping_rows();
    // A fresh computation rebuilds the baseline mapping and DISCARDS manual
    // row adjustments; the stale mix snapshot must not be restored onto the
    // new clusters either. The selected MATCHING MODE however survives a
    // re-Apply (2026-11 spec): a CMYW user gets CMYW re-run on the fresh
    // result via the m_auto_mix_pending hook in set_state(Ready). The
    // cancel and failure paths of on_computation_complete return BEFORE
    // this code, so the previous successful mapping and mode survive
    // untouched there.
    m_auto_mix_snapshot.reset();
    const bool reapply_mix = (m_mix_mode_index == 1);
    m_auto_mix_pending = reapply_mix;

    m_applied_color_count = m_param_color_count;
    m_applied_smooth      = m_param_smooth;
    m_applied_gap_area    = m_param_gap_area;

    set_state(TextureImportState::Ready);
    update_drop_warning_visibility();
    // Direct-mapping baseline: reflect it in the dropdown. A re-applied
    // CMYW — or its failure fallback to direct mapping inside
    // apply_auto_standard_mix — already synced the mode UI in set_state.
    if (!reapply_mix)
        set_mix_mode_ui(0);

    GetSizer()->Layout();

    // Step-aware preview mode: the mapping step previews the filament-mapped
    // result, the simplification step the raw cluster colors.
    m_preview_canvas->set_simplified_mode(m_step == ImportStep::Mapping
        ? TexturePreviewCanvas::RenderMode::FilamentMap
        : TexturePreviewCanvas::RenderMode::MultiColor);
    update_preview_color_labels();

    if (initial) {
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
    }

    // "Next" on dirty parameters: the computation that just finished was
    // launched for the step switch, so carry it out now.
    if (m_pending_step_advance) {
        m_pending_step_advance = false;
        if (has_valid_result())
            switch_step(ImportStep::Mapping);
    }
}

void TextureImportDialog::on_computation_error(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }
    m_pending_step_advance = false;

    if (m_cancel_flag.load()) {
        if (initial) {
            m_initial_computation_cancelled = true;
            m_initial_computation_pending = false;
            m_current_computation_initial = false;
            return;
        }
        if (has_valid_result()) {
            if (m_applied_color_count >= 0) {
                m_param_color_count = m_applied_color_count;
                m_color_slider->SetValue(m_param_color_count);
                if (m_color_spin && m_color_spin->GetTextCtrl())
                    m_color_spin->GetTextCtrl()->ChangeValue(wxString::Format("%d", m_param_color_count));
                update_color_count_preset_buttons();
            }
            if (m_applied_smooth >= 0) {
                m_param_smooth = m_applied_smooth;
                m_smooth_slider->SetValue(m_param_smooth);
                if (m_smooth_spin && m_smooth_spin->GetTextCtrl())
                    m_smooth_spin->GetTextCtrl()->ChangeValue(wxString::Format("%d", m_param_smooth));
            }
            // Gap restore: applied value is a percent double; the slider
            // works in hundredths and the box shows two decimals. The old
            // code passed the double to SetValue and formatted it with %d,
            // which produced garbage like -1202590843 in the box.
            m_param_gap_area = m_applied_gap_area;
            if (m_gap_slider)
                m_gap_slider->SetValue(gap_value_to_slider(m_param_gap_area));
            if (m_gap_spin && m_gap_spin->GetTextCtrl())
                m_gap_spin->GetTextCtrl()->ChangeValue(wxString::Format("%.2f", m_param_gap_area));
            set_state(TextureImportState::Ready);
            return;
        }
        set_state(TextureImportState::Idle);
        return;
    }

    if (initial) {
        m_initial_computation_failed = true;
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
        m_fallback_to_geometry_only = true;
        return;
    }

    set_state(TextureImportState::Error);
    Slic3r::GUI::MessageDialog dlg(initial ? GetParent() : this,
        _L("Computation failed. Please adjust parameters and retry."),
        _L("Error"), wxOK | wxICON_ERROR);
    dlg.ShowModal();
}

void TextureImportDialog::on_mesh_repair_decision_required(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;
    bool auto_color = m_current_computation_auto_color;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    Slic3r::GUI::MessageDialog dlg(initial ? GetParent() : this,
        _L("The mesh has non-manifold geometry or open boundaries. You can import it as-is or repair it before importing."),
        _L("Mesh repair"), wxYES_NO | wxCANCEL | wxICON_WARNING | wxYES_DEFAULT);
    // 2026-11 feedback: "Repair and import" sits in the leading position.
    // YES lays out before NO on Windows, so YES carries the repair action
    // and keeps the accent/default styling.
    dlg.SetButtonLabel(wxID_YES, _L("Repair and import"), true);
    dlg.SetButtonLabel(wxID_NO, _L("Import without repair"));
    dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
    if (auto* yes_btn = dynamic_cast<Button*>(dlg.FindWindow(wxID_YES))) {
        yes_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        yes_btn->SetMinSize(wxSize(FromDIP(160), FromDIP(24)));
    }
    if (auto* no_btn = dynamic_cast<Button*>(dlg.FindWindow(wxID_NO))) {
        no_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        no_btn->SetMinSize(wxSize(FromDIP(180), FromDIP(24)));
    }
    dlg.Layout();
    dlg.Fit();
    dlg.CenterOnParent();
    int ret = dlg.ShowModal();
    if (ret == wxID_CANCEL) {
        m_cancel_flag = true;
        if (initial) {
            m_initial_computation_cancelled = true;
            m_initial_computation_pending = false;
            m_current_computation_initial = false;
        } else if (has_valid_result()) {
            set_state(TextureImportState::Ready);
        } else {
            set_state(TextureImportState::Idle);
        }
        return;
    }
    m_mesh_repair_decision = (ret == wxID_YES)
        ? Slic3r::TexturePaintingSettings::MeshRepairDecision::RepairAndImport
        : Slic3r::TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;

    start_computation(auto_color, initial);
}

// ---- Mapping ----

void TextureImportDialog::update_filament_color_map()
{
    std::map<std::array<std::size_t, 3>, std::array<float, 3>> color_map;
    for (const auto& m : m_current_matches) {
        if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
            color_map[m.cluster_color] = {
                m_filament_colors_rgba[m.filament_index][0],
                m_filament_colors_rgba[m.filament_index][1],
                m_filament_colors_rgba[m.filament_index][2]
            };
        }
    }
    m_preview_canvas->set_filament_color_map(color_map);
}

// Canonical ordering used on the very first display after a computation:
// sort ascending by filament_index, and push unmapped (filament_index < 0)
// entries to the end. This gives the user a stable, predictable mapping
// layout regardless of the cluster discovery order.
void TextureImportDialog::sort_current_matches_by_filament_index()
{
    std::stable_sort(m_current_matches.begin(), m_current_matches.end(),
        [](const auto& lhs, const auto& rhs) {
            const bool lhs_valid = lhs.filament_index >= 0;
            const bool rhs_valid = rhs.filament_index >= 0;

            if (lhs_valid != rhs_valid)
                return lhs_valid;
            if (!lhs_valid)
                return false;

            return lhs.filament_index < rhs.filament_index;
        });
}

// Preserve the row order the user is currently looking at across a
// re-computation (e.g. when auto-merge is toggled). We key on cluster_index
// because it survives compact_used_virtual_filaments() and filament-index
// renumbering, whereas filament_index does not.
//
// Behaviour:
//   * Entries whose cluster_index appeared in `previous_matches` keep their
//     previous relative order.
//   * Entries whose cluster_index is new (not in `previous_matches`) are
//     appended at the end, in their current relative order.
//
// Assumption: each cluster_index appears at most once in both vectors. This
// is currently guaranteed by do_auto_match(), which emits exactly one match
// per cluster. If that invariant ever changes, the std::map::emplace below
// silently keeps only the first occurrence and the order will be wrong.

size_t TextureImportDialog::max_filament_count() const
{
    // Fork adaptation: upstream caps at EnforcerBlockerType::ExtruderMax (32 there); this fork's
    // ExtruderMax is 255 while the sidebar only accepts MAXIMUM_EXTRUDER_NUMBER filaments, so the
    // dialog cap must match the sidebar or add_custom_filament would silently drop the overflow.
    return MAXIMUM_EXTRUDER_NUMBER;
}

bool TextureImportDialog::can_add_virtual_filament() const
{
    return m_filament_colors_rgba.size() < max_filament_count();
}

int TextureImportDialog::find_closest_filament_index(const std::array<std::size_t, 3>& color) const
{
    int best_idx = -1;
    double best_delta = std::numeric_limits<double>::max();
    const size_t filament_count = std::min(m_filament_colors_rgba.size(), max_filament_count());
    for (size_t i = 0; i < filament_count; ++i) {
        const double delta = Slic3r::compute_delta_e(color, m_filament_colors_rgba[i]);
        if (delta < best_delta) {
            best_delta = delta;
            best_idx = (int)i;
        }
    }
    return best_idx;
}

int TextureImportDialog::add_virtual_filament(const std::array<float, 4>& rgba, const std::string& hex,
                                              const std::string& preset_name)
{
    if (m_filament_color_strs.size() != m_filament_colors_rgba.size() ||
        m_filament_names.size() != m_filament_colors_rgba.size() ||
        m_filament_entries.size() != m_filament_colors_rgba.size()) {
        return -1;
    }

    const std::string normalized_hex    = texture_normalize_color_hex(hex);
    const std::string effective_preset  = preset_name.empty() ? m_default_virtual_filament_preset_name : preset_name;

    // Doc §3.3.4: adding a filament whose type+color already exists merges
    // into that filament instead of creating a duplicate (always on — the
    // checkbox is gone). Runs before the cap check so merging never fails.
    for (size_t i = 0; i < m_filament_entries.size(); ++i) {
        const auto& entry = m_filament_entries[i];
        if (!texture_entry_is_physical(entry.kind))
            continue;
        if (entry.color_hex != normalized_hex)
            continue;
        const std::string entry_preset = !entry.preset_name.empty()
            ? entry.preset_name : m_default_virtual_filament_preset_name;
        if (entry_preset == effective_preset)
            return entry.dialog_index;
    }

    if (!can_add_virtual_filament()) {
        // Mark that this do_auto_match() run hit the filament cap and had to
        // drop at least one cluster. The mapping itself still falls back via
        // find_closest_filament_index() below; this flag only drives the
        // inline orange warning above the bottom buttons.
        m_filaments_dropped = true;
        return -1;
    }

    // Display name follows the preset so the row reads like a real filament.
    std::string display_name = effective_preset;
    if (auto* preset_bundle = wxGetApp().preset_bundle) {
        if (auto* preset = preset_bundle->filaments.find_preset(effective_preset, false))
            display_name = preset->label(false);
    }
    if (display_name.empty())
        display_name = DEFAULT_VIRTUAL_FILAMENT_NAME;

    const int new_idx = (int)m_filament_colors_rgba.size();
    m_filament_colors_rgba.push_back(rgba);
    m_filament_color_strs.push_back(normalized_hex);
    m_filament_names.push_back(display_name);
    TextureFilamentEntry entry;
    entry.kind = TextureFilamentKind::NewPhysical;
    entry.dialog_index = new_idx;
    entry.project_config_index = size_t(-1);
    entry.color_hex = normalized_hex;
    entry.name = display_name;
    entry.preset_name = effective_preset;
    m_filament_entries.push_back(entry);
    m_new_filament_colors.push_back(rgba);
    m_new_filament_preset_names.push_back(effective_preset);
    return new_idx;
}

int TextureImportDialog::add_virtual_mixed_filament(const std::string& color_hex,
                                                    const std::vector<int>& component_dialog_indices,
                                                    const std::vector<int>& ratios)
{
    if (m_filament_color_strs.size() != m_filament_colors_rgba.size() ||
        m_filament_names.size() != m_filament_colors_rgba.size() ||
        m_filament_entries.size() != m_filament_colors_rgba.size()) {
        return -1;
    }
    if (component_dialog_indices.size() < 2 || component_dialog_indices.size() != ratios.size())
        return -1;
    for (int idx : component_dialog_indices) {
        if (idx < 0 || idx >= (int)m_filament_entries.size() ||
            !texture_entry_is_physical(m_filament_entries[idx].kind)) {
            return -1;
        }
    }
    if (!can_add_virtual_filament()) {
        m_filaments_dropped = true;
        return -1;
    }

    const int new_idx = (int)m_filament_colors_rgba.size();
    TextureFilamentEntry entry;
    entry.kind = TextureFilamentKind::NewMixed;
    entry.dialog_index = new_idx;
    entry.project_config_index = size_t(-1);
    entry.color_hex = texture_normalize_color_hex(color_hex);
    entry.name = DEFAULT_VIRTUAL_FILAMENT_NAME;
    entry.mixed_ratios = ratios;
    for (int idx : component_dialog_indices)
        entry.mixed_components.push_back((unsigned int)(idx + 1));

    TextureNewMixedFilament mixed;
    mixed.dialog_index = entry.dialog_index;
    mixed.color_hex = entry.color_hex;
    mixed.component_dialog_indices = component_dialog_indices;
    mixed.ratios = ratios;

    m_filament_entries.push_back(entry);
    m_filament_color_strs.push_back(entry.color_hex);
    m_filament_names.push_back(entry.name);
    m_filament_colors_rgba.push_back(parse_color_string(entry.color_hex));
    m_new_mixed_filaments.push_back(mixed);
    return entry.dialog_index;
}

void TextureImportDialog::compact_used_virtual_filaments()
{
    if (m_current_matches.empty())
        return;

    const std::vector<std::array<float, 4>> old_colors = m_filament_colors_rgba;
    const std::vector<std::string> old_color_strs = m_filament_color_strs;
    const std::vector<std::string> old_names = m_filament_names;
    const std::vector<TextureFilamentEntry> old_entries = m_filament_entries;

    auto old_new_mixed_has_valid_components = [&old_entries, &old_colors](const TextureFilamentEntry& entry) {
        if (entry.kind != TextureFilamentKind::NewMixed)
            return true;
        if (entry.mixed_components.size() < 2 || entry.mixed_components.size() != entry.mixed_ratios.size())
            return false;
        for (unsigned int comp : entry.mixed_components) {
            int comp_idx = comp >= 1 ? (int)comp - 1 : -1;
            if (comp_idx < 0 || comp_idx >= (int)old_entries.size() || comp_idx >= (int)old_colors.size() ||
                !texture_entry_is_physical(old_entries[comp_idx].kind)) {
                return false;
            }
        }
        return true;
    };

    std::set<int> used_virtual_indices;
    for (const auto& m : m_current_matches) {
        if (m.filament_index >= (int)m_existing_filament_count &&
            m.filament_index < (int)old_colors.size()) {
            if (m.filament_index < (int)old_entries.size() &&
                old_entries[m.filament_index].kind == TextureFilamentKind::NewMixed &&
                !old_new_mixed_has_valid_components(old_entries[m.filament_index])) {
                continue;
            }
            used_virtual_indices.insert(m.filament_index);
        }
    }
    bool added_dependency = true;
    while (added_dependency) {
        added_dependency = false;
        std::vector<int> current_used(used_virtual_indices.begin(), used_virtual_indices.end());
        for (int used_idx : current_used) {
            if (used_idx < 0 || used_idx >= (int)old_entries.size() ||
                old_entries[used_idx].kind != TextureFilamentKind::NewMixed ||
                !old_new_mixed_has_valid_components(old_entries[used_idx]))
                continue;
            for (unsigned int comp : old_entries[used_idx].mixed_components) {
                int comp_idx = comp >= 1 ? (int)comp - 1 : -1;
                if (comp_idx >= (int)m_existing_filament_count && comp_idx < (int)old_entries.size() &&
                    used_virtual_indices.insert(comp_idx).second) {
                    added_dependency = true;
                }
            }
        }
    }

    std::vector<std::array<float, 4>> compact_colors;
    std::vector<std::string> compact_color_strs;
    std::vector<std::string> compact_names;
    std::vector<TextureFilamentEntry> compact_entries;
    compact_colors.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_color_strs.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_names.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_entries.reserve(m_existing_filament_count + used_virtual_indices.size());

    const size_t existing_count = std::min(m_existing_filament_count, old_colors.size());
    for (size_t i = 0; i < existing_count; ++i) {
        compact_colors.push_back(old_colors[i]);
        compact_color_strs.push_back(i < old_color_strs.size() ? old_color_strs[i] : "");
        compact_names.push_back(i < old_names.size() ? old_names[i] : GUI::format(_u8L("Filament %d"), i + 1));
        TextureFilamentEntry entry = i < old_entries.size() ? old_entries[i] : TextureFilamentEntry{};
        entry.dialog_index = (int)i;
        entry.color_hex = texture_normalize_color_hex(compact_color_strs.back());
        entry.name = compact_names.back();
        compact_entries.push_back(entry);
    }

    std::map<int, int> old_to_new;
    std::vector<std::array<float, 4>> compact_new_colors;
    std::vector<std::string> compact_new_preset_names;
    compact_new_colors.reserve(used_virtual_indices.size());
    compact_new_preset_names.reserve(used_virtual_indices.size());

    for (int old_idx : used_virtual_indices) {
        old_to_new[old_idx] = (int)compact_colors.size();
        compact_colors.push_back(old_colors[old_idx]);
        compact_color_strs.push_back(old_idx < (int)old_color_strs.size() ? old_color_strs[old_idx] : "");
        compact_names.push_back(old_idx < (int)old_names.size() ? old_names[old_idx] : DEFAULT_VIRTUAL_FILAMENT_NAME);
        TextureFilamentEntry entry = old_idx < (int)old_entries.size() ? old_entries[old_idx] : TextureFilamentEntry{};
        entry.dialog_index = (int)compact_entries.size();
        entry.color_hex = texture_normalize_color_hex(compact_color_strs.back());
        entry.name = compact_names.back();
        if (entry.kind == TextureFilamentKind::NewPhysical) {
            compact_new_colors.push_back(old_colors[old_idx]);
            compact_new_preset_names.push_back(entry.preset_name.empty() ? m_default_virtual_filament_preset_name : entry.preset_name);
        }
        compact_entries.push_back(entry);
    }

    m_filament_colors_rgba = std::move(compact_colors);
    m_filament_color_strs = std::move(compact_color_strs);
    m_filament_names = std::move(compact_names);
    m_filament_entries = std::move(compact_entries);
    m_new_filament_colors = std::move(compact_new_colors);
    m_new_filament_preset_names = std::move(compact_new_preset_names);
    m_new_mixed_filaments.clear();
    std::set<int> invalid_compacted_mixed_indices;
    for (auto& entry : m_filament_entries) {
        if (entry.kind != TextureFilamentKind::NewMixed)
            continue;
        TextureNewMixedFilament mixed;
        mixed.dialog_index = entry.dialog_index;
        mixed.color_hex = entry.color_hex;
        mixed.ratios = entry.mixed_ratios;
        mixed.component_dialog_indices.reserve(entry.mixed_components.size());
        bool valid_components = entry.mixed_components.size() >= 2 &&
            entry.mixed_components.size() == entry.mixed_ratios.size();
        for (unsigned int comp : entry.mixed_components) {
            int old_comp_idx = comp >= 1 ? (int)comp - 1 : -1;
            if (old_comp_idx < 0) {
                valid_components = false;
                break;
            }
            auto remap_it = old_to_new.find(old_comp_idx);
            int new_comp_idx = remap_it != old_to_new.end() ? remap_it->second : old_comp_idx;
            if (new_comp_idx < 0 || new_comp_idx >= (int)m_filament_entries.size() ||
                !texture_entry_is_physical(m_filament_entries[new_comp_idx].kind)) {
                valid_components = false;
                break;
            }
            mixed.component_dialog_indices.push_back(new_comp_idx);
        }
        if (!valid_components) {
            invalid_compacted_mixed_indices.insert(entry.dialog_index);
            continue;
        }
        entry.mixed_components.clear();
        for (int comp_idx : mixed.component_dialog_indices)
            entry.mixed_components.push_back((unsigned int)(comp_idx + 1));
        m_new_mixed_filaments.push_back(mixed);
    }

    auto find_closest_physical_filament_index = [this](const std::array<std::size_t, 3>& color) {
        int best_idx = -1;
        double best_delta = std::numeric_limits<double>::max();
        const size_t filament_count = std::min(m_filament_colors_rgba.size(), max_filament_count());
        for (size_t i = 0; i < filament_count && i < m_filament_entries.size(); ++i) {
            if (!texture_entry_is_physical(m_filament_entries[i].kind))
                continue;
            const double delta = Slic3r::compute_delta_e(color, m_filament_colors_rgba[i]);
            if (delta < best_delta) {
                best_delta = delta;
                best_idx = (int)i;
            }
        }
        return best_idx;
    };

    for (auto& m : m_current_matches) {
        auto it = old_to_new.find(m.filament_index);
        if (it != old_to_new.end()) {
            m.filament_index = it->second;
        } else if (m.filament_index >= (int)m_existing_filament_count) {
            m.filament_index = find_closest_filament_index(m.cluster_color);
        }
        if (invalid_compacted_mixed_indices.count(m.filament_index) > 0) {
            int fallback_idx = find_closest_physical_filament_index(m.cluster_color);
            m.filament_index = fallback_idx >= 0 ? fallback_idx : find_closest_filament_index(m.cluster_color);
        }

        if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
            m.filament_color = m_filament_colors_rgba[m.filament_index];
            m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
            if (m.filament_index >= (int)m_existing_filament_count)
                m.delta_e = 0.0;
        }
    }
}

std::vector<int> TextureImportDialog::compute_display_numbers() const
{
    // Assigns each entry a 1-based display number in the order the sidebar will
    // show after apply: ExistingPhysical, NewPhysical, ExistingMixed, NewMixed.
    // This keeps the dialog's visible IDs in sync with the post-apply sidebar,
    // instead of the raw dialog_index (which interleaves physicals and mixeds
    // by processing order and causes e.g. CMYW to show 4,5,6,8 instead of 3,4,5,6).
    // MUST mirror ordering in apply_textured_mesh_import_result (Plater.cpp:9896):
    //   - ExistingPhysical keeps its project_config_index
    //   - NewPhysical is inserted at existing_physical_count + new_order
    //   - ExistingMixed shifts to project_config_index + new_physical_count
    //   - NewMixed is appended after all existing mixeds
    std::vector<int> result(m_filament_entries.size(), 0);
    int next = 1;

    auto assign_group = [&](TextureFilamentKind kind, bool by_project_config_index) {
        if (by_project_config_index) {
            std::vector<const TextureFilamentEntry*> group;
            for (const auto& e : m_filament_entries)
                if (e.kind == kind)
                    group.push_back(&e);
            std::sort(group.begin(), group.end(),
                      [](const TextureFilamentEntry* a, const TextureFilamentEntry* b) {
                          return a->project_config_index < b->project_config_index;
                      });
            for (const auto* e : group) {
                if (e->dialog_index >= 0 && e->dialog_index < (int)result.size())
                    result[e->dialog_index] = next;
                ++next;
            }
        } else {
            for (const auto& e : m_filament_entries) {
                if (e.kind != kind)
                    continue;
                if (e.dialog_index >= 0 && e.dialog_index < (int)result.size())
                    result[e.dialog_index] = next;
                ++next;
            }
        }
    };

    assign_group(TextureFilamentKind::ExistingPhysical, true);
    assign_group(TextureFilamentKind::NewPhysical,      false);
    assign_group(TextureFilamentKind::ExistingMixed,    true);
    assign_group(TextureFilamentKind::NewMixed,         false);
    return result;
}

void TextureImportDialog::dismiss_filament_popup()
{
    if (!m_filament_popup) {
        m_filament_popup_row = -1;
        return;
    }

    FilamentSelectPopup* popup = m_filament_popup;
    m_filament_popup = nullptr;
    m_filament_popup_row = -1;
    popup->close_popup();
}

void TextureImportDialog::select_mix_mode(int index)
{
    if (index != 0 && index != 1)
        return;
    // Selecting the active mode must not overwrite the direct-mapping snapshot.
    if (index == m_mix_mode_index &&
        (index == 1 ? (m_auto_mix_snapshot.has_value() || m_auto_mix_pending)
                    : (!m_auto_mix_snapshot && !m_auto_mix_pending)))
        return;
    set_mix_mode_ui(index);
    // The dismissing popup tears itself down via CallAfter; refresh the
    // trigger again after that lands so its surface is never left stale.
    if (m_mix_mode_trigger)
        CallAfter([this]() {
            // Queued events can outlive the dialog; both liveness checks are
            // required (pointer nulling happens on the destroy path).
            if (m_mix_mode_trigger && !m_mix_mode_trigger->IsBeingDeleted()
                && !IsBeingDeleted()) {
                m_mix_mode_trigger->Raise();
                m_mix_mode_trigger->Refresh();
            }
        });
    // 0 = Direct Filament Mapping (baseline), 1 = CMYW Auto Mix (one-click
    // mix); mirrors the old checkbox on/off behaviour. Selecting CMYW while
    // the mapping rows are not ready yet (still computing, or no clusters)
    // arms m_auto_mix_pending so the mix runs as soon as the rows exist.
    if (index == 1) {
        if (m_state == TextureImportState::Ready && !m_current_matches.empty() && !m_mapping_rows.empty())
            apply_auto_standard_mix(TextureAutoMixMode::CMYW);
        else
            m_auto_mix_pending = true;
    }
    else {
        m_auto_mix_pending = false;
        reset_auto_mix();
    }
}

void TextureImportDialog::set_mix_mode_ui(int index)
{
    if (index != 0 && index != 1)
        return;
    m_mix_mode_index = index;
    if (m_mix_mode_trigger) {
        // Raise too: the CMYW mix path rebuilds the mapping rows below this
        // trigger, and on some z-order paths the trigger ended up underneath
        // and looked "gone" (2026-11 feedback).
        m_mix_mode_trigger->Raise();
        m_mix_mode_trigger->Refresh();
    }
}

// Mode list popup: always opens DOWNWARD below the trigger (2026-11 design;
// the shared ComboBox cannot be forced to drop down).
static bool texture_import_nozzle_08();
void TextureImportDialog::show_mix_mode_popup()
{
    if (!m_mix_mode_trigger || m_mix_mode_popup)
        return;

    auto* popup = new MixModePopup(this, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS);
    m_mix_mode_popup = popup;
    popup->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& e) {
        e.Skip();
        m_mix_mode_popup = nullptr;
    });

    const wxColour pop_bg = StateColor::darkModeColorFor(*wxWHITE);
    auto* panel = new wxPanel(popup, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    panel->SetBackgroundColour(pop_bg);
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->Bind(wxEVT_PAINT, [this, pop_bg](wxPaintEvent& e) {
        auto* p = static_cast<wxPanel*>(e.GetEventObject());
        wxAutoBufferedPaintDC dc(p);
        const wxSize sz = p->GetClientSize();
        dc.SetBackground(wxBrush(pop_bg));
        dc.Clear();
        dc.SetBrush(wxBrush(pop_bg));
        texture_draw_edge_border(dc, sz, StateColor::darkModeColorFor(wxColour("#DBDBDB")), FromDIP(8));
    });

    const int item_w = m_mix_mode_trigger->GetSize().x;
    const int item_h = FromDIP(36);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const wxString labels[2] = { _L("Direct Filament Mapping"), _L("CMYW Auto Mix") };
    const bool nozzle_08 = texture_import_nozzle_08();
    for (int i = 0; i < 2; ++i) {
        const bool disabled = (i == 1 && nozzle_08);
        auto* row = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(item_w, item_h));
        row->SetBackgroundColour(pop_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(disabled ? wxCURSOR_ARROW : wxCURSOR_HAND));
        if (disabled)
            row->SetToolTip(_L("Mixing is not supported for the 0.8 mm nozzle."));
        row->Bind(wxEVT_PAINT, [this, i, labels, pop_bg, disabled](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBackground(wxBrush(pop_bg));
            dc.Clear();
            // Hover fill #BEE1DB (2026-11 spec, 24px-high band); selected row
            // carries the check on the LEFT, text stays #18181B. The disabled
            // CMYW option (0.8 mm nozzle) draws grey with no hover fill.
            const wxPoint mouse = p->ScreenToClient(wxGetMousePosition());
            const bool hovered = !disabled && p->GetClientRect().Contains(mouse);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(hovered ? wxColour("#BEE1DB") : wxColour("#FFFFFF"))));
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            const bool selected = (m_mix_mode_index == i);
            const int text_x = p->FromDIP(36);
            if (selected) {
                texture_draw_glyph(dc, p, p->FromDIP(12),
                    (sz.y - p->FromDIP(12)) / 2.0,
                    StateColor::darkModeColorFor(wxColour("#7B8282")), true);
            }
            wxFont f = p->GetFont();
            f.SetPointSize(texture_font_pt(kMixModeFontPt));
            f.SetWeight(wxFONTWEIGHT_MEDIUM);
            dc.SetFont(f);
            dc.SetTextForeground(StateColor::darkModeColorFor(disabled ? wxColour("#949494") : wxColour("#18181B")));
            const wxSize tsz = dc.GetTextExtent(labels[i]);
            dc.DrawText(labels[i], text_x, (sz.y - tsz.y) / 2);
        });
        row->Bind(wxEVT_LEFT_DOWN, [this, i, popup, disabled](wxMouseEvent&) {
            if (disabled)
                return;
            popup->Dismiss();
            select_mix_mode(i);
        });
        // Hover fill is computed from the mouse position at paint time, so
        // refresh on enter/leave/move.
        row->Bind(wxEVT_MOTION, [](wxMouseEvent& evt) {
            evt.Skip();
            static_cast<wxWindow*>(evt.GetEventObject())->Refresh();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [](wxMouseEvent& evt) {
            evt.Skip();
            static_cast<wxWindow*>(evt.GetEventObject())->Refresh();
        });
        sizer->Add(row, 0, wxEXPAND);
    }
    panel->SetSizer(sizer);
    panel->SetSize(item_w, item_h * 2);
    popup->SetSize(item_w, item_h * 2);

    // Always drop DOWN from the trigger's bottom edge; clamp horizontally to
    // the display work area.
    wxPoint pos = m_mix_mode_trigger->ClientToScreen(wxPoint(0, m_mix_mode_trigger->GetSize().y + 2));
    wxRect display_rect;
    const int display_idx = wxDisplay::GetFromPoint(pos);
    display_rect = display_idx != wxNOT_FOUND ? wxDisplay(display_idx).GetClientArea()
                                              : wxDisplay().GetClientArea();
    pos.x = std::clamp(pos.x, display_rect.GetLeft(),
                       std::max(display_rect.GetLeft(), display_rect.GetRight() - item_w));
    popup->Move(pos); // Move, not Position(): Position() flips the popup ABOVE the anchor when it does not fit below
    popup->Popup();
}

// Calibrated CMYW base set used by the auto-mix path below; resolved next to
// add_decomposed_mixed_filament (both consumers share the same base colours).
struct CymwBaseColor
{
    std::string hex;
    std::string name;
    std::string material_id;
};
static std::vector<CymwBaseColor> cymw_base_colors();

// True when any nozzle of the current printer is 0.8 mm; CMYW mixing is
// unsupported for that diameter (shared by the dropdown disabled state and
// the runtime gate).
static bool texture_import_nozzle_08()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr)
        return false;
    const auto* opt = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt == nullptr)
        return false;
    for (double nozzle_mm : opt->values)
        if (nozzle_mm - 0.8 == 0.0)
            return true;
    return false;
}

void TextureImportDialog::apply_auto_standard_mix(TextureAutoMixMode mode)
{
    if (mode != TextureAutoMixMode::CMYW)
        return;
    // Keep the first pre-mix snapshot until the user leaves CMYW.
    if (m_auto_mix_snapshot)
        return;
    if (m_state != TextureImportState::Ready || m_current_matches.empty() || m_mapping_rows.empty())
        return;

    // 0.8 mm nozzles cannot mix: warn and fall back to direct mapping even if
    // a Full Spectrum preset for the nozzle were shipped later.
    if (texture_import_nozzle_08()) {
        MessageDialog dlg(this,
            _L("Mixing is not supported for the 0.8 mm nozzle. Direct Filament Mapping will be used instead."),
            _L("Texture Import Warning"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        set_mix_mode_ui(0);
        return;
    }

    // Doc section 3.3.5 note 1: mixing needs a nozzle with a Full Spectrum (CMYW)
    // preset; refuse otherwise and keep the mapping untouched.
    if (!Slic3r::GUI::full_spectrum_preset_exists_for_current_nozzle()) {
        MessageDialog dlg(this,
            _L("The current nozzle diameter does not support CMYW auto mixing. Please change the nozzle diameter or disable auto mixing."),
            _L("Texture Import Warning"), wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        set_mix_mode_ui(0);
        return;
    }

    // Snapshot the whole mapping state so reset_auto_mix() can restore it
    // verbatim (the user may decline the mixed result per doc §3.3.5).
    AutoMixSnapshot snap;
    snap.colors_rgba       = m_filament_colors_rgba;
    snap.color_strs        = m_filament_color_strs;
    snap.names             = m_filament_names;
    snap.entries           = m_filament_entries;
    snap.new_colors        = m_new_filament_colors;
    snap.new_preset_names  = m_new_filament_preset_names;
    snap.new_mixed         = m_new_mixed_filaments;
    snap.matches           = m_current_matches;
    snap.filaments_dropped = m_filaments_dropped;

    auto restore_snapshot = [this, &snap]() {
        m_filament_colors_rgba      = snap.colors_rgba;
        m_filament_color_strs       = snap.color_strs;
        m_filament_names            = snap.names;
        m_filament_entries          = snap.entries;
        m_new_filament_colors       = snap.new_colors;
        m_new_filament_preset_names = snap.new_preset_names;
        m_new_mixed_filaments       = snap.new_mixed;
        m_current_matches           = snap.matches;
        m_filaments_dropped         = snap.filaments_dropped;
    };
    auto abort_apply = [this, &restore_snapshot]() {
        restore_snapshot();
        update_drop_warning_visibility();
        set_mix_mode_ui(0);
    };

    // Drop the direct-mapping virtual leftovers so the CMYW base colours and
    // the new mixed recipes are numbered right after the project's existing
    // filaments (max existing number + 1) instead of after the leftovers.
    // References to existing mixed filaments are kept: a repeat match of the
    // same model reuses them (and the base entries they depend on) instead of
    // creating duplicates.
    for (auto& m : m_current_matches) {
        if (m.filament_index < 0 || m.filament_index >= (int)m_filament_entries.size() ||
            !texture_entry_is_mixed(m_filament_entries[m.filament_index].kind))
            m.filament_index = 0;
    }
    compact_used_virtual_filaments();

    // CMYW base palette: the calibrated 0.2 mm cyan/magenta/yellow/white
    // family from the K/S material database. Project filaments calibrated to
    // the same kit (filament_full_spectrum_material_id) are reused in place so
    // the spectral engine resolves their measured spectra; unconfigured
    // materials are auto-added as virtual filaments (doc section 3.3.5 note 3).
    const std::vector<CymwBaseColor> base_colors = cymw_base_colors();
    if (base_colors.size() < 2) {
        abort_apply();
        return;
    }
    const std::string base_preset = Slic3r::GUI::full_spectrum_preset_name();

    const ConfigOptionStrings* proj_material_ids = nullptr;
    if (PresetBundle* pb = wxGetApp().preset_bundle)
        proj_material_ids = pb->project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");

    std::vector<int> base_idx;
    std::vector<std::string> palette_hex;
    base_idx.reserve(base_colors.size());
    palette_hex.reserve(base_colors.size());
    for (const CymwBaseColor& base : base_colors) {
        const std::string norm = texture_normalize_color_hex(base.hex);
        int idx = -1;

        // 1) Reuse the project physical filament calibrated to this material.
        if (proj_material_ids) {
            for (size_t j = 0; j < proj_material_ids->values.size(); ++j) {
                if (proj_material_ids->values[j] != base.material_id)
                    continue;
                for (size_t i = 0; i < m_filament_entries.size(); ++i) {
                    const TextureFilamentEntry& e = m_filament_entries[i];
                    if (e.kind == TextureFilamentKind::ExistingPhysical && e.project_config_index == j) {
                        idx = static_cast<int>(i);
                        break;
                    }
                }
                if (idx >= 0)
                    break;
            }
        }

        // 2) Reuse a physical entry carrying the base colour AND the Full
        // Spectrum preset: colour alone is not enough — a same-colour filament
        // of an incompatible material would pass the import but fail the
        // slice-time compatibility check.
        if (idx < 0) {
            for (size_t i = 0; i < m_filament_entries.size(); ++i) {
                const TextureFilamentEntry& entry = m_filament_entries[i];
                if (!texture_entry_is_physical(entry.kind))
                    continue;
                const std::string entry_hex = texture_normalize_color_hex(
                    entry.color_hex.empty() ? m_filament_color_strs[i] : entry.color_hex);
                if (entry_hex != norm)
                    continue;
                std::string entry_preset = entry.preset_name;
                if (entry_preset.empty() && entry.kind == TextureFilamentKind::ExistingPhysical) {
                    if (PresetBundle* pb = wxGetApp().preset_bundle) {
                        if (entry.project_config_index < pb->filament_presets.size())
                            entry_preset = pb->filament_presets[entry.project_config_index];
                    }
                }
                if (entry_preset == base_preset) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
        }

        // 3) Otherwise create a virtual physical entry for the base colour,
        // typed after the Full Spectrum preset (add_virtual_filament names it
        // from the preset label, e.g. "Snapmaker PLA Full Spectrum").
        if (idx < 0) {
            idx = add_virtual_filament(parse_color_string(norm), norm, base_preset);
            if (idx < 0) {
                abort_apply();
                return;
            }
        }

        base_idx.push_back(idx);
        palette_hex.push_back(texture_normalize_color_hex(
            m_filament_entries[idx].color_hex.empty() ? m_filament_color_strs[idx] : m_filament_entries[idx].color_hex));
    }

    // Recipes of mixed filaments that survived the compaction (a previous
    // match of the same model, or project mixed rows), keyed by component
    // colours + ratios so a repeat match reuses them instead of duplicating.
    auto recipe_key_for_entry = [this](const TextureFilamentEntry& entry) -> std::string {
        if (entry.mixed_components.size() < 2 || entry.mixed_components.size() != entry.mixed_ratios.size())
            return std::string();
        std::string key;
        for (size_t mi = 0; mi < entry.mixed_components.size(); ++mi) {
            const int comp_idx = entry.mixed_components[mi] >= 1 ? (int)entry.mixed_components[mi] - 1 : -1;
            if (comp_idx < 0 || comp_idx >= (int)m_filament_color_strs.size())
                return std::string();
            if (mi > 0) key += '/';
            key += texture_normalize_color_hex(m_filament_color_strs[comp_idx]) + ':' + std::to_string(entry.mixed_ratios[mi]);
        }
        return key;
    };
    std::map<std::string, int> existing_recipe_idx;
    for (const auto& entry : m_filament_entries) {
        if (!texture_entry_is_mixed(entry.kind))
            continue;
        const std::string key = recipe_key_for_entry(entry);
        if (!key.empty() && entry.dialog_index >= 0)
            existing_recipe_idx.emplace(key, entry.dialog_index);
    }

    // Best CMYW recipe per cluster color; identical recipes share one mixed
    // filament so the 64-filament budget is not wasted on duplicates. Recipes
    // are predicted with the calibrated K/S spectral engine (the base hexes
    // resolve to their measured spectra) instead of the legacy RGB mixer.
    MixedFilamentDisplayContext context = build_mixed_filament_display_context(palette_hex);
    // Stamp the selected slots' calibrated identity: the context resolves
    // materials by matching the project's colours, which cannot name the
    // virtual base filaments, so assign the base material ids explicitly.
    context.physical_material_ids.assign(palette_hex.size(), "");
    for (size_t k = 0; k < palette_hex.size() && k < base_colors.size(); ++k)
        context.physical_material_ids[k] = base_colors[k].material_id;
    std::map<std::string, int> recipe_mixed_idx;
    for (auto& match : m_current_matches) {
        const wxColour target((unsigned char)match.cluster_color[0],
                              (unsigned char)match.cluster_color[1],
                              (unsigned char)match.cluster_color[2]);

        // Reuse a CMYW base filament when the target is already close to a
        // pure base colour (ΔE2000 ≤ 5), so near-pure targets do not spawn
        // junk recipes. The scan is limited to the base set: the direct
        // mapping baseline contains an exact-colour filament for every
        // cluster, and scanning those would short-circuit every mix.
        {
            constexpr double kReuseDeltaEThreshold = 5.0;
            int best_idx = -1;
            double best_delta = std::numeric_limits<double>::max();
            for (int base : base_idx) {
                const double delta = Slic3r::compute_delta_e(match.cluster_color, m_filament_colors_rgba[base]);
                if (delta < best_delta) {
                    best_delta = delta;
                    best_idx = base;
                }
            }
            if (best_idx >= 0 && best_delta <= kReuseDeltaEThreshold) {
                match.filament_index = best_idx;
                match.filament_color = m_filament_colors_rgba[best_idx];
                match.delta_e = best_delta;
                continue;
            }
        }

        const auto recipe = Slic3r::GUI::build_best_color_match_recipe(
            palette_hex, target, 5, 100, false, &context);
        if (!recipe.valid)
            continue; // keep the previous mapping for this cluster

        const std::vector<int> weights = expand_color_match_recipe_weights(recipe, palette_hex.size());
        std::vector<int> components;
        std::vector<int> ratios;
        std::vector<std::string> component_hexes;
        for (size_t k = 0; k < weights.size(); ++k) {
            if (weights[k] <= 0)
                continue;
            components.push_back(base_idx[k]);
            ratios.push_back(weights[k]);
            component_hexes.push_back(palette_hex[k]);
        }
        if (components.size() < 2) {
            // Single-component recipe: point at the base filament directly.
            if (components.size() == 1) {
                match.filament_index = components.front();
                match.filament_color = m_filament_colors_rgba[components.front()];
                match.delta_e = recipe.delta_e;
            }
            continue;
        }

        std::string key;
        for (size_t k = 0; k < components.size(); ++k) {
            if (k > 0) key += '/';
            key += component_hexes[k] + ':' + std::to_string(ratios[k]);
        }

        int target_idx = -1;
        auto it = recipe_mixed_idx.find(key);
        if (it != recipe_mixed_idx.end()) {
            target_idx = it->second;
        }
        else {
            const auto eit = existing_recipe_idx.find(key);
            if (eit != existing_recipe_idx.end()) {
                // Reuse the mixed filament created by an earlier match of the
                // same model instead of creating a duplicate.
                target_idx = eit->second;
                recipe_mixed_idx[key] = target_idx;
            }
        }
        if (target_idx < 0) {
            const wxColour& pc = recipe.preview_color;
            const std::string mixed_hex = wxString::Format("#%02X%02X%02X",
                pc.Red(), pc.Green(), pc.Blue()).ToStdString();
            const int mixed_idx = add_virtual_mixed_filament(mixed_hex, components, ratios);
            if (mixed_idx < 0) {
                abort_apply();
                return;
            }
            recipe_mixed_idx[key] = mixed_idx;
            target_idx = mixed_idx;
        }

        if (target_idx >= 0 && target_idx < (int)m_filament_colors_rgba.size()) {
            match.filament_index = target_idx;
            match.filament_color = m_filament_colors_rgba[target_idx];
            match.delta_e = recipe.delta_e;
        }
    }

    m_auto_mix_snapshot = std::move(snap);

    // Drop mixed filaments the rematch no longer references (e.g. the colour
    // set changed between runs) so no orphan recipes reach the confirm step;
    // compact also renumbers the survivors.
    compact_used_virtual_filaments();

    // The compaction above also drops base colours no recipe references; keep
    // the full four-colour Full Spectrum base set in the list regardless.
    for (const std::string& hex : palette_hex) {
        const std::string norm = texture_normalize_color_hex(hex);
        bool found = false;
        for (size_t i = 0; i < m_filament_entries.size(); ++i) {
            if (!texture_entry_is_physical(m_filament_entries[i].kind))
                continue;
            const std::string entry_hex = texture_normalize_color_hex(
                m_filament_entries[i].color_hex.empty() ? m_filament_color_strs[i] : m_filament_entries[i].color_hex);
            if (entry_hex == norm) {
                found = true;
                break;
            }
        }
        if (!found)
            add_virtual_filament(parse_color_string(norm), norm, base_preset);
    }

    rebuild_mapping_rows();
    update_filament_color_map();
    update_drop_warning_visibility();
    update_confirm_button_state();
    GetSizer()->Layout();
}

void TextureImportDialog::reset_auto_mix()
{
    if (m_auto_mix_snapshot) {
        const auto& snap = *m_auto_mix_snapshot;
        m_filament_colors_rgba      = snap.colors_rgba;
        m_filament_color_strs       = snap.color_strs;
        m_filament_names            = snap.names;
        m_filament_entries          = snap.entries;
        m_new_filament_colors       = snap.new_colors;
        m_new_filament_preset_names = snap.new_preset_names;
        m_new_mixed_filaments       = snap.new_mixed;
        m_current_matches           = snap.matches;
        m_filaments_dropped         = snap.filaments_dropped;
    }
    m_auto_mix_snapshot.reset();
    m_auto_mix_pending = false;
    set_mix_mode_ui(0);

    rebuild_mapping_rows();
    update_filament_color_map();
    update_drop_warning_visibility();
    update_confirm_button_state();
    Layout();
}

// CYMW V3 base set: the calibrated 0.2 mm cyan/magenta/yellow/white family
// embedded in the K/S database (FullSpectrumMaterialDatabaseProfile). Every
// target colour is mixed from these four base colours instead of the
// project's arbitrary palette. Carries the material id so project filaments
// calibrated to the same kit can be reused in place.
static std::vector<CymwBaseColor> cymw_base_colors()
{
    using Slic3r::FullSpectrumMaterialDatabaseProfileData::MATERIAL_COUNT;
    using Slic3r::FullSpectrumMaterialDatabaseProfileData::MATERIAL_HEX;
    using Slic3r::FullSpectrumMaterialDatabaseProfileData::MATERIAL_ID;

    static const std::unordered_map<std::string, std::string> kNames = {
        {"#DA548F", "Full Spectrum Magenta"},
        {"#008FC0", "Full Spectrum Cyan"},
        {"#D9B500", "Full Spectrum Yellow"},
        {"#E2E5E0", "Full Spectrum White"},
    };

    std::vector<CymwBaseColor> base;
    for (size_t i = 0; i < MATERIAL_COUNT; ++i) {
        if (std::string(MATERIAL_ID[i]).compare(0, 8, "cymw_v3_") != 0)
            continue;
        const std::string hex = MATERIAL_HEX[i];
        const auto it = kNames.find(hex);
        base.push_back({hex, it != kNames.end() ? it->second
                                                : "Full Spectrum " + std::to_string(base.size() + 1),
                        MATERIAL_ID[i]});
    }
    return base;
}

bool TextureImportDialog::add_decomposed_mixed_filament(size_t row_index)
{
    if (row_index >= m_mapping_rows.size())
        return false;

    // If the filament list already holds a filament whose colour matches the
    // row's target — physical or mixed, no distinction — use it directly
    // instead of creating a new mixed recipe. Same CIEDE2000 threshold as
    // do_auto_match's NEW_FILAMENT_THRESHOLD.
    {
        constexpr double kReuseDeltaEThreshold = 5.0;
        int best_idx = -1;
        double best_delta = std::numeric_limits<double>::max();
        const std::array<std::size_t, 3> target_rgb = m_mapping_rows[row_index].source_color;
        const size_t scan_count = std::min(m_filament_entries.size(), m_filament_colors_rgba.size());
        for (size_t i = 0; i < scan_count; ++i) {
            const double delta = Slic3r::compute_delta_e(target_rgb, m_filament_colors_rgba[i]);
            if (delta < best_delta) {
                best_delta = delta;
                best_idx = static_cast<int>(i);
            }
        }
        if (best_idx >= 0 && best_delta <= kReuseDeltaEThreshold) {
            m_mapping_rows[row_index].target_filament_idx = best_idx;
            if (row_index < m_current_matches.size()) {
                m_current_matches[row_index].filament_index = best_idx;
                m_current_matches[row_index].filament_color = m_filament_colors_rgba[best_idx];
                m_current_matches[row_index].delta_e = best_delta;
            }
            rebuild_mapping_rows();
            update_filament_color_map();
            return true;
        }
    }

    const std::vector<CymwBaseColor> base_colors = cymw_base_colors();
    if (base_colors.size() < 2)
        return false;

    // Project filaments calibrated to the CYMW kit (filament_full_spectrum_material_id
    // starting with cymw_v3_) are reused in place: no new physical filament is
    // created and the existing numbers stay valid, so the new mixed recipes are
    // numbered from the project's max filament + 1.
    const ConfigOptionStrings* proj_material_ids = nullptr;
    if (PresetBundle* pb = wxGetApp().preset_bundle)
        proj_material_ids = pb->project_config.option<ConfigOptionStrings>("filament_full_spectrum_material_id");

    std::vector<int> base_dialog_indices;
    std::vector<std::string> base_hexes;
    base_dialog_indices.reserve(base_colors.size());
    base_hexes.reserve(base_colors.size());
    for (const CymwBaseColor& base : base_colors) {
        const std::string norm = texture_normalize_color_hex(base.hex);
        int idx = -1;

        // 1) Reuse the project physical filament calibrated to this material.
        if (proj_material_ids) {
            for (size_t j = 0; j < proj_material_ids->values.size(); ++j) {
                if (proj_material_ids->values[j] != base.material_id)
                    continue;
                for (size_t i = 0; i < m_filament_entries.size(); ++i) {
                    const TextureFilamentEntry& e = m_filament_entries[i];
                    if (e.kind == TextureFilamentKind::ExistingPhysical &&
                        e.project_config_index == j) {
                        idx = static_cast<int>(i);
                        break;
                    }
                }
                if (idx >= 0)
                    break;
            }
        }

        // 2) Reuse any physical entry already carrying the base colour.
        if (idx < 0) {
            for (size_t i = 0; i < m_filament_entries.size(); ++i) {
                if (!texture_entry_is_physical(m_filament_entries[i].kind))
                    continue;
                const std::string entry_hex = texture_normalize_color_hex(
                    m_filament_entries[i].color_hex.empty() ? m_filament_color_strs[i] : m_filament_entries[i].color_hex);
                if (entry_hex == norm) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
        }

        // 3) Otherwise create a virtual physical entry for the base colour.
        if (idx < 0) {
            const wxColour c(norm);
            const std::array<float, 4> rgba = {c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f, 1.0f};
            idx = add_virtual_filament(rgba, norm);
            if (idx < 0)
                return false;
            m_filament_names[idx] = base.name;
            m_filament_entries[idx].name = base.name;
        }

        base_dialog_indices.push_back(idx);
        base_hexes.push_back(texture_normalize_color_hex(
            m_filament_entries[idx].color_hex.empty() ? m_filament_color_strs[idx] : m_filament_entries[idx].color_hex));
    }

    const FilamentMappingRow &row = m_mapping_rows[row_index];
    wxColour target;
    const bool target_ok = !row.source_hex.empty() &&
                           try_parse_color_match_hex(wxString::FromUTF8(row.source_hex), target);
    if (!target_ok)
        target = wxColour(static_cast<unsigned char>(row.source_color[0]),
                          static_cast<unsigned char>(row.source_color[1]),
                          static_cast<unsigned char>(row.source_color[2]));

    // CMYW match: search the recipe (components + ratios) whose predicted
    // blend is closest to the row's colour, evaluated with the calibrated K/S
    // engine (the four base hexes resolve to their measured spectra). The
    // category filter is bypassed: the base set is a calibrated kit and the
    // dialog entries have no project filament category to resolve against.
    const MixedFilamentDisplayContext context = build_mixed_filament_display_context(base_hexes);
    const MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(base_hexes, target, 5, 100, false, &context);
    if (!recipe.valid)
        return false;

    const std::vector<int> weights = expand_color_match_recipe_weights(recipe, base_hexes.size());
    std::vector<int> component_dialog_indices;
    std::vector<int> ratios;
    for (size_t k = 0; k < weights.size(); ++k) {
        if (weights[k] <= 0)
            continue;
        component_dialog_indices.push_back(base_dialog_indices[k]);
        ratios.push_back(weights[k]);
    }
    if (component_dialog_indices.size() < 2)
        return false;

    // If the list already holds a mixed filament built from the identical
    // recipe (same CYMW components and ratios), reuse it. The predicted colour
    // of an imperfect mix can sit far from its target (ΔE beyond the colour
    // reuse threshold above), so recipe identity — not colour distance — is
    // what dedupes repeat imports of the same model.
    {
        int reused_mixed_idx = -1;
        for (size_t i = 0; i < m_filament_entries.size(); ++i) {
            const TextureFilamentEntry& e = m_filament_entries[i];
            if (!texture_entry_is_mixed(e.kind))
                continue;
            std::vector<int> entry_weights(base_dialog_indices.size(), 0);
            bool all_components_are_bases = true;
            for (size_t mi = 0; mi < e.mixed_components.size() && mi < e.mixed_ratios.size(); ++mi) {
                const int comp_di = e.mixed_components[mi] >= 1 ? static_cast<int>(e.mixed_components[mi]) - 1 : -1;
                int base_k = -1;
                for (size_t k = 0; k < base_dialog_indices.size(); ++k) {
                    if (base_dialog_indices[k] == comp_di) {
                        base_k = static_cast<int>(k);
                        break;
                    }
                }
                if (base_k < 0) {
                    all_components_are_bases = false;
                    break;
                }
                entry_weights[base_k] += e.mixed_ratios[mi];
            }
            if (!all_components_are_bases || entry_weights != weights)
                continue;
            reused_mixed_idx = static_cast<int>(i);
            break;
        }
        if (reused_mixed_idx >= 0) {
            m_mapping_rows[row_index].target_filament_idx = reused_mixed_idx;
            if (row_index < m_current_matches.size()) {
                m_current_matches[row_index].filament_index = reused_mixed_idx;
                m_current_matches[row_index].filament_color = m_filament_colors_rgba[reused_mixed_idx];
            }
            rebuild_mapping_rows();
            update_filament_color_map();
            return true;
        }
    }

    const std::string mixed_hex = texture_normalize_color_hex(
        recipe.preview_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    const int new_idx = add_virtual_mixed_filament(mixed_hex, component_dialog_indices, ratios);
    if (new_idx < 0)
        return false;

    m_mapping_rows[row_index].target_filament_idx = new_idx;
    if (row_index < m_current_matches.size())
        m_current_matches[row_index].filament_index = new_idx;
    rebuild_mapping_rows();
    update_filament_color_map();
    return true;
}

void TextureImportDialog::dismiss_filament_popup_on_wheel(wxMouseEvent& evt)
{
    dismiss_filament_popup();
    evt.Skip();
}

void TextureImportDialog::show_filament_popup(size_t row_index)
{
    if (row_index >= m_mapping_rows.size()) return;

    if (m_skip_next_filament_popup_row == (int)row_index) {
        m_skip_next_filament_popup_row = -1;
        return;
    }

    if (m_filament_popup && m_filament_popup->IsShown()) {
        if (m_filament_popup_row == (int)row_index) {
            dismiss_filament_popup();
            return;
        }
        dismiss_filament_popup();
    }

    const auto display_numbers = compute_display_numbers();
    auto display_number = [display_numbers](int idx) -> int {
        return (idx >= 0 && idx < (int)display_numbers.size() && display_numbers[idx] > 0)
            ? display_numbers[idx] : idx + 1;
    };

    wxPanel* tp = m_mapping_rows[row_index].target_panel;
    if (!tp) return;
    const wxWeakRef<wxWindow> anchor(tp);

    auto on_select = [this, row_index, anchor](int idx) {
        // Run on the dialog after the popup mouse handler has unwound.
        CallAfter([this, row_index, anchor, idx]() {
            if (IsBeingDeleted() || !anchor.get() || anchor->IsBeingDeleted() ||
                row_index >= m_mapping_rows.size() ||
                m_mapping_rows[row_index].target_panel != anchor.get() ||
                row_index >= m_current_matches.size() ||
                idx < 0 || static_cast<size_t>(idx) >= m_filament_entries.size() ||
                static_cast<size_t>(idx) >= m_filament_colors_rgba.size())
                return;
            assign_filament_to_row(row_index, idx);
        });
    };

    auto on_add_filament_request = [this, row_index, anchor]() {
        CallAfter([this, row_index, anchor]() {
            if (IsBeingDeleted() || !anchor.get() || anchor->IsBeingDeleted() ||
                row_index >= m_mapping_rows.size() ||
                m_mapping_rows[row_index].target_panel != anchor.get())
                return;
            request_add_filament(row_index);
        });
    };

    auto on_decompose_color = [this, row_index]() {
        CallAfter([this, row_index]() {
            add_decomposed_mixed_filament(row_index);
        });
    };

    auto on_close = [this, row_index](bool closed_by_action) {
        if (m_filament_popup_row == (int)row_index) {
            m_filament_popup = nullptr;
            m_filament_popup_row = -1;
        }
        if (!closed_by_action) {
            m_skip_next_filament_popup_row = (int)row_index;
            CallAfter([this, row_index]() {
                if (m_skip_next_filament_popup_row == (int)row_index)
                    m_skip_next_filament_popup_row = -1;
            });
        }
    };

    auto* popup = new FilamentSelectPopup(
        this, m_filament_entries, m_filament_colors_rgba, m_filament_names,
        m_existing_filament_count, tp->GetSize().x, tp, on_select, on_add_filament_request,
        on_decompose_color,
        [this]() { return can_add_virtual_filament(); },
        on_close,
        m_mix_mode_index == 1,
        display_numbers);

    // Anchor under the target CHIP (x = swatch 20 + 8 gap), not under the
    // leading numbered swatch (2026-11 feedback). Vertical side follows the
    // popup's own decision: full size below when it fits, else above.
    wxPoint pos = tp->ClientToScreen(wxPoint(FromDIP(28), tp->GetSize().y + 2));
    if (popup->place_above())
        pos.y = tp->ClientToScreen(wxPoint(0, 0)).y - popup->GetSize().y - 2;
    wxRect display_rect;
    int display_idx = wxDisplay::GetFromPoint(pos);
    if (display_idx != wxNOT_FOUND)
        display_rect = wxDisplay(display_idx).GetClientArea();
    else
        display_rect = wxDisplay().GetClientArea();
    pos.x = std::clamp(pos.x, display_rect.GetLeft(),
                       std::max(display_rect.GetLeft(), display_rect.GetRight() - popup->GetSize().x));
    popup->Move(pos); // Move, not Position(): Position() re-flips the popup relative to its anchor
    popup->Bind(wxEVT_DESTROY, [this, popup](wxWindowDestroyEvent& e) {
        e.Skip();
        if (m_filament_popup == popup) {
            m_filament_popup = nullptr;
            m_filament_popup_row = -1;
        }
    });
    m_filament_popup = popup;
    m_filament_popup_row = (int)row_index;
    popup->Popup();
}

void TextureImportDialog::assign_filament_to_row(size_t row_index, int filament_idx)
{
    if (filament_idx < 0)
        return;
    if (row_index < m_mapping_rows.size()) {
        m_mapping_rows[row_index].target_filament_idx = filament_idx;
        if (row_index < m_current_matches.size())
            m_current_matches[row_index].filament_index = filament_idx;
    }
    rebuild_mapping_rows();
    update_filament_color_map();
}

void TextureImportDialog::legacy_add_material_picker(size_t row_index)
{
    wxColourData cd;
    cd.SetColour(wxColour(0, 0, 0));
    cd.SetChooseFull(true);
    wxColourDialog dlg(this, &cd);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const wxColour clr = dlg.GetColourData().GetColour();
    std::array<float, 4> rgba = {clr.Red() / 255.f, clr.Green() / 255.f,
                                 clr.Blue() / 255.f, 1.0f};
    std::string hex = wxString::Format("#%02X%02X%02X",
        clr.Red(), clr.Green(), clr.Blue()).ToStdString();
    assign_filament_to_row(row_index, add_virtual_filament(rgba, hex));
}

std::vector<TextureImportDialog::AddMaterialFamily> TextureImportDialog::collect_add_material_families()
{
    // 2026-11 rethink: iterate the PresetCollection EXACTLY like
    // PresetComboBox::update() (the plater filament dropdown / the vendor
    // picker's data source) does — the same deque, the same
    // is_visible/is_compatible gate, the same default-preset windowing. Any
    // filament the user can pick in the plater dropdown is therefore
    // addable here; anything they cannot pick is not. Names come from the
    // collection itself, never from string lookups.
    std::vector<AddMaterialFamily> families;
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return families;

    auto& library = Slic3r::FilamentColorLibrary::Instance();
    const bool library_ok = library.EnsureLoaded();

    // User-chosen colors per preset name (project slot i pairs
    // filament_presets[i] with filament_colour[i]).
    std::map<std::string, std::vector<std::string>> project_preset_colors;
    {
        const auto* slot_colors = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const size_t slot_count = preset_bundle->filament_presets.size();
        for (size_t i = 0; i < slot_count; ++i) {
            const std::string& name = preset_bundle->filament_presets[i];
            if (name.empty())
                continue;
            std::string hex = (slot_colors && i < slot_colors->values.size())
                ? slot_colors->values[i] : std::string();
            if (!hex.empty())
                project_preset_colors[name].push_back(std::move(hex));
        }
    }

    // No secondary dedup inside try_add: the outer base-name dedup already
    // guarantees one entry per family, and library entries can share a
    // filament_id (141703112701 is on both PLA Matte and PLA SnapSpeed, GFU99
    // on both TPU families) - an id-keyed set silently dropped real
    // materials (2026-11 missing-materials checklist P0).
    auto try_add = [&](const Preset& preset) {
        Slic3r::FilamentColorInfo info;
        const bool in_library = library_ok
            && library.FindFilamentByName(preset.name, info) && !info.colors.empty();
        const auto user_colors_it = project_preset_colors.find(preset.name);

        // 2026-11 doc (TextureImportDialog_UI_fix): officialness needs BOTH
        // the profile vendor and a color-library hit. Only a Snapmaker-vendor
        // preset that the library actually knows keeps its palette;
        // everything else (third-party brands, generics, and Snapmaker
        // materials the library does not list, e.g. Snapmaker Nylon) gets no
        // palette and the dialog shows only the "Other Colors" button.
        const auto* vendor_opt = preset.config.option<ConfigOptionStrings>("filament_vendor");
        bool snapmaker_brand = false;
        if (vendor_opt) {
            for (const std::string& vendor : vendor_opt->values) {
                std::string low;
                low.reserve(vendor.size());
                for (char ch : vendor)
                    low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                if (low.find("snapmaker") != std::string::npos) {
                    snapmaker_brand = true;
                    break;
                }
            }
        }
        const bool official_family = snapmaker_brand && in_library;

        if (official_family && user_colors_it != project_preset_colors.end()) {
            // Official family: append the colors the user picked for it
            // (dedup by primary hex).
            auto to_hex = [](const std::string& c) {
                std::string h = c;
                std::transform(h.begin(), h.end(), h.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                return h;
            };
            std::set<std::string> known;
            for (const auto& item : info.colors)
                known.insert(to_hex(item.colorData.PrimaryColor()));
            for (const std::string& hex : user_colors_it->second) {
                if (!known.insert(to_hex(hex)).second)
                    continue;
                Slic3r::FilamentColorItem item;
                item.colorData = Slic3r::FilamentColor::FromColors(
                    {hex}, Slic3r::FilamentColorMode::Segment, hex);
                item.colorNames["en"] = item.colorData.PrimaryColor(hex);
                info.colors.push_back(std::move(item));
            }
        }
        if (!official_family) {
            // No official palette (doc sections 1 and 3.2): drop the colors
            // so the dialog collapses to the picker + "Other Colors" +
            // buttons. Keep the identity fields - an empty filamentId would
            // collapse every following family in the seen_ids dedup.
            info.colors.clear();
            info.filamentId   = "preset:" + preset.name;
            info.filamentName = preset.label(false).empty() ? preset.name : preset.label(false);
        }
        AddMaterialFamily family;
        family.preset_name = preset.name;
        family.label       = preset.label(false).empty() ? preset.name : preset.label(false);
        // Grouping: Snapmaker-brand families lead the list even when the
        // library has no palette for them (2026-11 feedback - Snapmaker
        // Nylon must not sink to the bottom).
        family.official    = snapmaker_brand;
        family.info        = std::move(info);
        BOOST_LOG_TRIVIAL(info) << "texture add-material family: " << preset.name
                                << " colors=" << family.info.colors.size()
                                << (snapmaker_brand ? " (snapmaker)" : " (third-party)")
                                << (official_family ? " library-palette" : " no-palette");
        families.push_back(std::move(family));
    };

    // Project-loaded presets first (most-recent slot first), then the whole
    // visible+compatible collection sweep — same gate as the plater dropdown.
    std::set<std::string> done;
    for (auto it = preset_bundle->filament_presets.rbegin();
         it != preset_bundle->filament_presets.rend(); ++it) {
        const Preset* preset = preset_bundle->filaments.find_preset(*it, false);
        if (!preset)
            continue;
        // Same material across machine/nozzle variants: one family per base
        // name (2026-11 feedback - duplicated Bambu ABS entries).
        const std::string base = strip_machine_suffix(preset->name);
        if (!done.insert("base:" + base).second)
            continue;
        try_add(*preset);
    }

    // Same population as PresetComboBox::update(): everything the plater
    // dropdown can show, INCLUDING incompatible presets (it lists them in an
    // "Incompatible presets" section). Only invisible entries are skipped.
    const std::deque<Preset>& presets = preset_bundle->filaments.get_presets();
    const size_t default_count = preset_bundle->filaments.num_default_presets();
    const size_t begin = presets.front().is_visible ? 0 : default_count;
    for (size_t i = begin; i < presets.size(); ++i) {
        const Preset& preset = presets[i];
        if (!preset.is_visible)
            continue;
        const std::string base = strip_machine_suffix(preset.name);
        if (!done.insert("base:" + base).second)
            continue;
        try_add(preset);
    }

    // 2026-11 feedback: Snapmaker-brand families lead the list (by vendor,
    // regardless of whether the library has a palette for them); everything
    // else follows alphabetically.
    std::sort(families.begin(), families.end(),
        [](const AddMaterialFamily& a, const AddMaterialFamily& b) {
            // Snapmaker official families lead; the rest follows
            // alphabetically. Officialness is by BRAND, not by palette
            // availability - a Snapmaker material without colors still
            // belongs to the official group (2026-11 feedback).
            if (a.official != b.official)
                return a.official;
            return wxString(a.label.c_str()).CmpNoCase(wxString(b.label.c_str())) < 0;
        });

    return families;
}

// Doc section 3.3.4 add material: AddMaterialDialog = family ComboBox on top + the
// official filament color panel embedded below (same swatch icons, ring,
// Other Colors and buttons as FilamentColorDialog's look). The raw native
// picker remains the fallback when no checked family resolves in the
// official color library.
void TextureImportDialog::request_add_filament(size_t row_index)
{
    if (!can_add_virtual_filament())
        return;

    // Cache: collect_add_material_families walks ~2000 presets and queries
    // the official library per preset; re-running it on every dropdown open
    // caused the reported multi-second lag. Rebuild only when the project
    // preset/color state changes.
    {
        const auto* slot_colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        std::string state_key = std::to_string(wxGetApp().preset_bundle->filament_presets.size());
        // Slot preset NAMES must invalidate too: same slot count and colors
        // with a swapped preset would otherwise reuse a stale list
        // (2026-11 missing-materials checklist P1).
        for (const auto& name : wxGetApp().preset_bundle->filament_presets)
            state_key += "|n:" + name;
        if (slot_colors)
            for (const auto& c : slot_colors->values)
                state_key += "|" + c;
        if (state_key != m_families_state_key) {
            BOOST_LOG_TRIVIAL(warning) << "texture add-material: recollecting families";
            m_families_cache = collect_add_material_families();
            m_families_state_key = state_key;
            BOOST_LOG_TRIVIAL(warning) << "texture add-material: collected "
                << m_families_cache.size() << " families";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "texture add-material: cached families used ("
                << m_families_cache.size() << ")";
        }
    }
    const std::vector<AddMaterialFamily>& families = m_families_cache;
    if (families.empty()) {
        legacy_add_material_picker(row_index);
        return;
    }

    std::vector<AddMaterialDialog::Family> dialog_families;
    dialog_families.reserve(families.size());
    for (const auto& family : families) {
        AddMaterialDialog::Family df;
        df.preset_name = family.preset_name;
        df.label       = wxString::FromUTF8(family.label);
        df.info        = family.info;
        dialog_families.push_back(std::move(df));
    }

    // Default family = the project's LAST filament slot preset (2026-11
    // feedback: e.g. a slot ending with "Snapmaker PLA SnapSpeed" preselects
    // that family), falling back to the first entry.
    int default_family = 0;
    if (auto* bundle = wxGetApp().preset_bundle;
        bundle && !bundle->filament_presets.empty()) {
        const std::string& last_slot = bundle->filament_presets.back();
        for (size_t i = 0; i < dialog_families.size(); ++i) {
            if (dialog_families[i].preset_name == last_slot) {
                default_family = (int)i;
                break;
            }
        }
    }
    AddMaterialDialog dlg(this, std::move(dialog_families), default_family);
    // Figma: the add-material card sits to the RIGHT of the import dialog and
    // must not cover it. Place after construction (ctor centers) and clamp to
    // the display work area.
    {
        const wxRect parent_rect = GetScreenRect();
        const wxSize dlg_size = dlg.GetSize();
        // Right of the import dialog, bottom edges aligned (2026-11 Figma).
        wxPoint pos(parent_rect.GetRight() + FromDIP(12),
                    parent_rect.GetBottom() - dlg_size.y);
        wxRect work;
        const int display_idx = wxDisplay::GetFromPoint(pos);
        work = display_idx != wxNOT_FOUND ? wxDisplay(display_idx).GetClientArea()
                                          : wxDisplay().GetClientArea();
        pos.x = std::min(pos.x, std::max(work.GetLeft(), work.GetRight() - dlg_size.x - FromDIP(8)));
        pos.y = std::clamp(pos.y, work.GetTop(), std::max(work.GetTop(), work.GetBottom() - dlg_size.y));
        dlg.Move(pos); // reposition only: keep the fitted (dynamic) size
    }
    if (dlg.ShowModal() != wxID_OK || dlg.color_hex().empty())
        return;

    assign_filament_to_row(row_index,
        add_virtual_filament(dlg.color_rgba(), dlg.color_hex(), dlg.preset_name()));
}

void TextureImportDialog::do_auto_match()
{
    if (m_painted.cluster_colors.empty()) return;

    // do_auto_match() always rebuilds the baseline mapping without any mixed
    // filaments; the auto-mix checkbox is resynced by its callers.

    // Reset the "filaments were dropped" flag at the start of every run, so it
    // strictly reflects what happens during *this* match (no historical
    // accumulation). add_virtual_filament() will flip it back to true if and
    // only if it hits the global filament cap below.
    m_filaments_dropped = false;

    // Drop any virtual filaments left over from previous match runs that the
    // current m_current_matches no longer references. Without this, the
    // residual virtual filaments inflate m_filament_colors_rgba.size() at the
    // entry of this match, which can make add_virtual_filament() fail (and
    // wrongly flip m_filaments_dropped to true) even when the *real* count
    // of needed virtual filaments for this run is well below the global cap.
    // This is purely a state cleanup; it does not change any mapping rule.
    compact_used_virtual_filaments();

    const auto previous_matches = m_current_matches;

    std::map<std::array<std::size_t, 3>, int> previous_virtual_by_cluster;
    for (const auto& match : previous_matches) {
        if (match.filament_index >= (int)m_existing_filament_count &&
            match.filament_index < (int)m_filament_entries.size() &&
            texture_entry_is_physical(m_filament_entries[match.filament_index].kind)) {
            previous_virtual_by_cluster[match.cluster_color] = match.filament_index;
        }
    }

    auto find_virtual_filament_by_color = [this](const std::array<std::size_t, 3>& color) -> int {
        std::string hex = rgb_to_hex(color).ToStdString();
        for (size_t i = m_existing_filament_count; i < m_filament_color_strs.size(); ++i) {
            if (m_filament_color_strs[i] == hex &&
                i < m_filament_entries.size() && texture_entry_is_physical(m_filament_entries[i].kind))
                return (int)i;
        }
        return -1;
    };

    auto get_or_add_virtual_filament = [this, &previous_virtual_by_cluster, &find_virtual_filament_by_color](
        const std::array<std::size_t, 3>& color) -> int {
        auto previous_it = previous_virtual_by_cluster.find(color);
        if (previous_it != previous_virtual_by_cluster.end() &&
            previous_it->second >= (int)m_existing_filament_count &&
            previous_it->second < (int)m_filament_colors_rgba.size()) {
            return previous_it->second;
        }

        int existing_idx = find_virtual_filament_by_color(color);
        if (existing_idx >= 0)
            return existing_idx;

        std::array<float, 4> rgba = {
            color[0] / 255.f,
            color[1] / 255.f,
            color[2] / 255.f,
            1.f
        };
        return add_virtual_filament(rgba, rgb_to_hex(color).ToStdString());
    };

    // Match clusters to closest existing filaments; filaments with poor match
    // get virtual ones, which add_virtual_filament() auto-merges by type+color
    // into identical existing filaments (doc §3.3.4, always on).
    {
        std::vector<std::string> names;
        for (size_t i = 0; i < m_existing_filament_count; ++i)
            names.push_back(m_filament_names.size() > i ? m_filament_names[i] : GUI::format(_u8L("Filament %d"), i + 1));

        std::vector<std::array<float, 4>> existing_filament_colors(
            m_filament_colors_rgba.begin(),
            m_filament_colors_rgba.begin() + std::min(m_existing_filament_count, m_filament_colors_rgba.size()));

        m_current_matches = Slic3r::match_clusters_to_filaments(
            m_painted.cluster_colors, existing_filament_colors, names);

        // For clusters with poor match (CIEDE2000 ΔE > 5), create virtual filaments.
        constexpr double NEW_FILAMENT_THRESHOLD = 5.0;
        std::map<std::array<std::size_t, 3>, int> virtual_color_index;

        for (auto& m : m_current_matches) {
            if (m.delta_e <= NEW_FILAMENT_THRESHOLD)
                continue;

            auto it = virtual_color_index.find(m.cluster_color);
            if (it != virtual_color_index.end()) {
                m.filament_index = it->second;
            } else {
                int new_idx = get_or_add_virtual_filament(m.cluster_color);
                if (new_idx >= 0)
                    virtual_color_index[m.cluster_color] = new_idx;
                m.filament_index = new_idx >= 0 ? new_idx : find_closest_filament_index(m.cluster_color);
            }
            if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
                m.filament_color = m_filament_colors_rgba[m.filament_index];
                m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
                if (m.filament_index >= (int)m_existing_filament_count)
                    m.delta_e = 0.0;
            }
        }
    }

    update_filament_color_map();
}

// Recommended mix-ratio bounds, mirroring the manual batch dialog's ratio
// guard (MixedFilamentBatchDialog::check_manual_recipe_ratio).
static constexpr int kTextureAutoMixMinComponentPercent = 15;
static constexpr int kTextureAutoMixMaxPairComponentPercent = 85;
static constexpr int kTextureAutoMixMaxMultiComponentPercent = 70;

// Return the specific component warnings using the same 1-based display
// numbers as the mapping chips. Keep the existing inclusive bounds.
static std::vector<wxString> mixed_ratio_warnings(const TextureFilamentEntry& entry,
                                                const std::vector<int>& numbers)
{
    std::vector<wxString> warnings;
    if (!texture_entry_is_mixed(entry.kind) || entry.mixed_components.size() < 2 ||
        entry.mixed_components.size() != entry.mixed_ratios.size())
        return warnings;
    const int max_pct = entry.mixed_ratios.size() >= 3 ? kTextureAutoMixMaxMultiComponentPercent
                                                       : kTextureAutoMixMaxPairComponentPercent;
    for (size_t i = 0; i < entry.mixed_ratios.size(); ++i) {
        const unsigned int id = entry.mixed_components[i];
        if (id == 0 || id > numbers.size() || numbers[id - 1] <= 0)
            continue;
        const int number = numbers[id - 1];
        const int ratio = entry.mixed_ratios[i];
        if (ratio < kTextureAutoMixMinComponentPercent)
            warnings.push_back(wxString::Format(_L("Filament %d mix ratio is too low and may affect the mixing result."), number));
        else if (ratio > max_pct)
            warnings.push_back(wxString::Format(_L("Filament %d mix ratio is too high and may affect the mixing result."), number));
    }
    return warnings;
}

// Inline warning band aligned with the target chip. The label wraps instead
// of widening the mapping list when the locale or DPI needs more space.
static wxPanel* texture_make_ratio_warning(wxWindow* parent, const wxString& message, int width)
{
    auto* band = new wxPanel(parent, wxID_ANY);
    const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFF4E8"));
    const wxColour fg = StateColor::darkModeColorFor(wxColour("#FF8400"));
    band->SetBackgroundColour(bg);
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    const int icon_size = band->FromDIP(14);
    const int pad = band->FromDIP(4);
    auto* icon = new wxPanel(band, wxID_ANY, wxDefaultPosition, wxSize(icon_size, icon_size));
    icon->SetMinSize(wxSize(icon_size, icon_size));
    icon->SetBackgroundColour(bg);
    icon->SetBackgroundStyle(wxBG_STYLE_PAINT);
    icon->Bind(wxEVT_PAINT, [bg, fg](wxPaintEvent& e) {
        auto* p = static_cast<wxPanel*>(e.GetEventObject());
        wxAutoBufferedPaintDC dc(p);
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
        const wxSize sz = p->GetClientSize();
        // Figma warning SVG: 14x14 viewBox, 6.25 radius, 1.5 stroke.
        // Keep fractional coordinates through rasterization (including Retina).
        const double side = std::min(sz.x, sz.y);
        if (side <= 0.0) return;
        const double scale = side / 14.0;
#if wxUSE_GRAPHICS_CONTEXT
        if (std::unique_ptr<wxGraphicsContext> gc{wxGraphicsContext::Create(dc)}) {
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
            gc->Translate((sz.x - side) / 2.0, (sz.y - side) / 2.0);
            gc->Scale(scale, scale);
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(fg).Width(1.5)));
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->DrawEllipse(0.75, 0.75, 12.5, 12.5);
            auto mark = gc->CreatePath();
            mark.MoveToPoint(6.48884, 8.35);
            mark.AddCurveToPoint(6.51085, 8.47633, 6.57425, 8.59053, 6.66802, 8.67275);
            mark.AddCurveToPoint(6.76178, 8.75497, 6.87998, 8.8, 7.00203, 8.8);
            mark.AddCurveToPoint(7.12409, 8.8, 7.24228, 8.75497, 7.33604, 8.67275);
            mark.AddCurveToPoint(7.42981, 8.59053, 7.49321, 8.47633, 7.51522, 8.35);
            mark.AddLineToPoint(7.85735, 3.4);
            mark.AddCurveToPoint(7.85687, 3.16146, 7.7666, 2.93283, 7.6063, 2.76416);
            mark.AddCurveToPoint(7.446, 2.59548, 7.22873, 2.5005, 7.00203, 2.5);
            mark.AddCurveToPoint(6.88322, 2.50308, 6.76623, 2.53148, 6.65815, 2.58348);
            mark.AddCurveToPoint(6.55006, 2.63548, 6.45311, 2.71, 6.37317, 2.80253);
            mark.AddCurveToPoint(6.29322, 2.89507, 6.23194, 3.0037, 6.19302, 3.12186);
            mark.AddCurveToPoint(6.15409, 3.24002, 6.13834, 3.36525, 6.14672, 3.49);
            mark.AddLineToPoint(6.48884, 8.35);
            mark.CloseSubpath();
            mark.MoveToPoint(7.00203, 9.7);
            mark.AddCurveToPoint(6.83287, 9.7, 6.6675, 9.75278, 6.52684, 9.85168);
            mark.AddCurveToPoint(6.38619, 9.95057, 6.27656, 10.0911, 6.21182, 10.2556);
            mark.AddCurveToPoint(6.14709, 10.42, 6.13015, 10.601, 6.16315, 10.7756);
            mark.AddCurveToPoint(6.19615, 10.9502, 6.27761, 11.1105, 6.39723, 11.2364);
            mark.AddCurveToPoint(6.51685, 11.3623, 6.66925, 11.448, 6.83517, 11.4827);
            mark.AddCurveToPoint(7.00108, 11.5174, 7.17306, 11.4996, 7.32935, 11.4315);
            mark.AddCurveToPoint(7.48563, 11.3634, 7.61922, 11.248, 7.7132, 11.1);
            mark.AddCurveToPoint(7.80718, 10.952, 7.85735, 10.778, 7.85735, 10.6);
            mark.AddCurveToPoint(7.86424, 10.4799, 7.84684, 10.3597, 7.8063, 10.2472);
            mark.AddCurveToPoint(7.76575, 10.1347, 7.70299, 10.0326, 7.62215, 9.94749);
            mark.AddCurveToPoint(7.5413, 9.86242, 7.44423, 9.79638, 7.33733, 9.75371);
            mark.AddCurveToPoint(7.23043, 9.71105, 7.11615, 9.69274, 7.00203, 9.7);
            mark.CloseSubpath();
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(fg));
            gc->FillPath(mark, wxWINDING_RULE);
            return;
        }
#endif
        // Integer fallback when the graphics backend is unavailable.
        const int cx = sz.x / 2, cy = sz.y / 2;
        const int r = std::max(1, static_cast<int>(std::floor(6.25 * scale)));
        wxPen pen(fg, std::max(1, static_cast<int>(std::lround(1.5 * scale))));
        pen.SetCap(wxCAP_ROUND);
        dc.SetPen(pen);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawCircle(cx, cy, r);
        dc.DrawLine(cx, cy - p->FromDIP(4), cx, cy + p->FromDIP(1));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(fg));
        dc.DrawCircle(cx, cy + p->FromDIP(4), std::max(1, p->FromDIP(1)));
    });
    auto* label = new wxStaticText(band, wxID_ANY, message);
    wxFont font = label->GetFont();
    font.SetPointSize(texture_font_pt(9));
    label->SetFont(font);
    label->SetForegroundColour(fg);
    label->SetBackgroundColour(bg);
    label->Wrap(std::max(1, width - icon_size - 3 * pad));
    line->Add(icon, 0, wxALIGN_TOP | wxLEFT | wxTOP, pad);
    line->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxALL, pad);
    band->SetSizer(line);
    band->SetMinSize(wxSize(width, std::max(band->FromDIP(24), line->GetMinSize().y)));
    band->SetMaxSize(wxSize(width, -1));
    return band;
}

void TextureImportDialog::rebuild_mapping_rows()
{
    // Reset, DPI changes and recomputation can also destroy the popup anchor.
    dismiss_filament_popup();
    m_mapping_scroll->Freeze();
    m_mapping_sizer->Clear(true);
    m_mapping_rows.clear();

    if (m_current_matches.empty()) {
        m_mapping_scroll->FitInside();
        m_mapping_scroll->Thaw();
        return;
    }

    auto get_target_wxcolor = [this](int idx) -> wxColour {
        if (idx >= 0 && idx < (int)m_filament_colors_rgba.size()) {
            const auto& c = m_filament_colors_rgba[idx];
            return wxColour((unsigned char)(c[0] * 255.f),
                            (unsigned char)(c[1] * 255.f),
                            (unsigned char)(c[2] * 255.f));
        }
        return wxColour(128, 128, 128);
    };

    const auto display_numbers = compute_display_numbers();
    auto display_number = [display_numbers](int idx) -> int {
        return (idx >= 0 && idx < (int)display_numbers.size() && display_numbers[idx] > 0)
            ? display_numbers[idx] : idx + 1;
    };

    auto get_filament_label = [this, display_number](int idx) -> wxString {
        if (idx >= 0 && idx < (int)m_filament_names.size())
            return filament_name_to_wx_string(m_filament_names[idx]);
        return wxString::Format(_L("Filament %d"), display_number(idx));
    };

    // 2026-11 step-2 design: row = [color circle 20 | hex chip 150] gap 12
    // [numbered swatch 20 | target chip 424]. Chips carry a #DBDBDB border and
    // the card's #F8F7F7 fill; the target chip holds either the material name
    // + chevron, or the mixed composition + a "New" tag for new mixes.
    const wxColour row_bg     = m_mapping_scroll->GetBackgroundColour();
    const wxColour chip_bd    = StateColor::darkModeColorFor(wxColour("#DBDBDB"));
    const wxColour hex_fg     = StateColor::darkModeColorFor(wxColour("#71717A"));
    const wxColour name_fg    = StateColor::darkModeColorFor(wxColour("#18181B"));
    const int row_h           = FromDIP(32);
    const int swatch          = FromDIP(20);
    // Source group = 178 (circle + hex chip) + a 24px arrow zone; the arrow
    // between source and target lives OUTSIDE the hex chip (2026-11 feedback).
    // Per the 716x204 Figma SVG: source group = circle(0..20) + chip(28..178)
    // + arrow(190..210) = 210; 12px gap; target group = swatch + 8 + 420 chip
    // (chip slightly shorter than the 434 spec so the scrollbar never brushes
    // the chevron when it appears, 2026-11 feedback).
    const int src_group_w     = FromDIP(210);
    const int tgt_group_w     = FromDIP(448);
    const int chip_w          = FromDIP(150);
    const int tgt_chip_w      = FromDIP(420);
    const int gap_after_swatch = FromDIP(28); // swatch + 8px gap = chip x offset

    m_mapping_rows.resize(m_current_matches.size());
    for (size_t ci = 0; ci < m_current_matches.size(); ++ci) {
        auto& row = m_mapping_rows[ci];
        row.cluster_id = m_current_matches[ci].cluster_index;
        row.source_color = m_current_matches[ci].cluster_color;
        row.source_hex = rgb_to_hex(row.source_color).ToStdString();
        row.target_filament_idx = m_current_matches[ci].filament_index;

        const wxColour src_wx_color(
            (unsigned char)row.source_color[0],
            (unsigned char)row.source_color[1],
            (unsigned char)row.source_color[2]);

        wxPanel* row_panel = new wxPanel(m_mapping_scroll, wxID_ANY);
        row_panel->SetBackgroundColour(row_bg);
        row_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);
        wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        // --- Source: color circle + hex chip ---
        row.source_panel = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition,
                                       wxSize(src_group_w, row_h),
                                       wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        row.source_panel->SetMinSize(wxSize(src_group_w, row_h));
        row.source_panel->SetMaxSize(wxSize(src_group_w, row_h));
        row.source_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row.source_panel->Bind(wxEVT_PAINT, [this, ci, src_wx_color, row_bg, chip_bd, hex_fg, row_h, swatch, chip_w, gap_after_swatch](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBrush(wxBrush(row_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            if (ci >= m_mapping_rows.size()) return;

            // Source color circle (20px)
            const int cy = (sz.y - swatch) / 2;
            dc.SetBrush(wxBrush(src_wx_color));
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D1D5DC")), 1));
            dc.DrawEllipse(0, cy, swatch, swatch);

            // Hex chip: square corners (Figma masks are sharp rects).
            const int chip_x = gap_after_swatch;
            dc.SetBrush(wxBrush(row_bg));
            // Inset y/height by 1 like texture_draw_edge_border: the chip's
            // top/bottom stroke would be half-clipped at the row edges on
            // macOS; chip_x is already interior so only the vertical needs it.
            dc.SetPen(wxPen(chip_bd, 1));
            dc.DrawRectangle(chip_x, 1, chip_w, sz.y - 2);

            {
                wxFont hf = p->GetFont();
                hf.SetPointSize(texture_font_pt(10));
                dc.SetFont(hf);
                dc.SetTextForeground(hex_fg);
                const wxString hex_str = wxString::Format("#%s", m_mapping_rows[ci].source_hex.substr(1));
                const wxSize tsz = dc.GetTextExtent(hex_str);
                dc.DrawText(hex_str, chip_x + p->FromDIP(8), (sz.y - tsz.y) / 2);
            }

            {
                // Source-to-target arrow in the group's trailing 24px zone —
                // NOT inside the hex chip (2026-11 feedback).
                // Vector 794 (716x204 SVG): hairline shaft 206..221.5 + solid
                // head to 226 -> row-relative 190..210, row-centred.
                const int mid_y = sz.y / 2;
                const int ax0 = FromDIP(190);
                const int shaft_end = ax0 + FromDIP(15);
                const int ax1 = ax0 + FromDIP(20);
                dc.SetPen(wxPen(hex_fg, 1));
                dc.DrawLine(ax0, mid_y, shaft_end, mid_y);
                dc.SetBrush(wxBrush(hex_fg));
                wxPoint head[3] = {
                    { ax1, mid_y },
                    { shaft_end, mid_y - FromDIP(3) },
                    { shaft_end, mid_y + FromDIP(3) }
                };
                dc.DrawPolygon(3, head);
            }
        });
        row.source_panel->Bind(wxEVT_SIZE, [](wxSizeEvent& e) {
            e.Skip();
            static_cast<wxWindow*>(e.GetEventObject())->Refresh();
        });
        row.source_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);
        row_sizer->Add(row.source_panel, 0, wxALIGN_CENTER_VERTICAL);

        row_sizer->AddSpacer(FromDIP(12));

        // --- Target: numbered swatch + material chip (name / mixed recipe) ---
        row.target_panel = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition,
                                       wxSize(tgt_group_w, row_h),
                                       wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        row.target_panel->SetMinSize(wxSize(tgt_group_w, row_h));
        row.target_panel->SetMaxSize(wxSize(tgt_group_w, row_h));
        row.target_panel->SetToolTip(get_filament_label(row.target_filament_idx));
        row.target_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row.target_panel->SetCursor(wxCursor(wxCURSOR_HAND));

        row.target_panel->Bind(wxEVT_PAINT, [this, ci, get_target_wxcolor, get_filament_label,
                                              display_number, row_bg, chip_bd, hex_fg, name_fg,
                                              row_h, swatch, tgt_chip_w, gap_after_swatch](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            const wxSize sz = p->GetClientSize();
            dc.SetBrush(wxBrush(row_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            if (ci >= m_mapping_rows.size()) return;
            const int fil_idx = m_mapping_rows[ci].target_filament_idx;

            // Numbered target swatch (20px, flat square)
            const int sy = (sz.y - swatch) / 2;
            const wxColour fil_clr = get_target_wxcolor(fil_idx);
            dc.SetBrush(wxBrush(fil_clr));
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D1D5DC")), 1));
            dc.DrawRectangle(0, sy, swatch, swatch);
            {
                wxFont nf = p->GetFont();
                nf.SetPointSize(texture_font_pt(10));
                dc.SetFont(nf);
                dc.SetTextForeground(fil_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                const wxString ns = wxString::Format("%d", display_number(fil_idx));
                const wxSize nsz = dc.GetTextExtent(ns);
                dc.DrawText(ns, (swatch - nsz.x) / 2, sy + (swatch - nsz.y) / 2);
            }

            // Chip (square corners per Figma SVG)
            const int chip_x = gap_after_swatch;
            dc.SetBrush(wxBrush(row_bg));
            // Same 1px vertical inset as the hex chip: keeps the top/bottom
            // stroke inside the row on macOS (see texture_draw_edge_border).
            dc.SetPen(wxPen(chip_bd, 1));
            dc.DrawRectangle(chip_x, 1, tgt_chip_w, sz.y - 2);

            const bool is_mixed = (fil_idx >= 0 && fil_idx < (int)m_filament_entries.size()
                                   && texture_entry_is_mixed(m_filament_entries[fil_idx].kind));
            const bool is_new_mix = (fil_idx >= 0 && fil_idx < (int)m_filament_entries.size()
                                     && m_filament_entries[fil_idx].kind == TextureFilamentKind::NewMixed);

            int x = chip_x + p->FromDIP(8);
            const int x_max = chip_x + tgt_chip_w - p->FromDIP(28);
            if (is_mixed) {
                // Mixed composition: [numbered swatch + %] xN with crossed
                // separators, plus a "New" tag on freshly added mixes.
                const TextureFilamentEntry& entry = m_filament_entries[fil_idx];
                wxFont mf = p->GetFont();
                mf.SetPointSize(texture_font_pt(10));
                dc.SetFont(mf);
                for (size_t mi = 0; mi < entry.mixed_components.size() && mi < entry.mixed_ratios.size(); ++mi) {
                    if (x > x_max) break;

                    const unsigned int comp_id = entry.mixed_components[mi];
                    const int comp_idx = comp_id >= 1 ? (int)comp_id - 1 : -1;
                    wxColour comp_clr(128, 128, 128);
                    if (comp_idx >= 0 && comp_idx < (int)m_filament_colors_rgba.size()) {
                        const auto& c = m_filament_colors_rgba[comp_idx];
                        comp_clr = wxColour((unsigned char)(c[0] * 255.f),
                                            (unsigned char)(c[1] * 255.f),
                                            (unsigned char)(c[2] * 255.f));
                    }
                    const int csy = (sz.y - swatch) / 2;
                    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#D1D5DC")), 1));
                    dc.SetBrush(wxBrush(comp_clr));
                    dc.DrawRectangle(x, csy, swatch, swatch);
                    dc.SetTextForeground(comp_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                    const wxString cnum = wxString::Format("%d", display_number(comp_idx));
                    const wxSize cnz = dc.GetTextExtent(cnum);
                    dc.DrawText(cnum, x + (swatch - cnz.x) / 2, csy + (swatch - cnz.y) / 2);
                    x += swatch + p->FromDIP(4);

                    dc.SetTextForeground(name_fg);
                    const wxString pct = wxString::Format("%d%%", entry.mixed_ratios[mi]);
                    const wxSize psz = dc.GetTextExtent(pct);
                    dc.DrawText(pct, x, (sz.y - psz.y) / 2);
                    x += psz.x + p->FromDIP(6);

                    if (mi + 1 < entry.mixed_components.size()) {
                        // crossed separator
                        const int cx0 = x + p->FromDIP(1);
                        const int cy0 = (sz.y - p->FromDIP(8)) / 2;
                        dc.SetPen(wxPen(hex_fg, 1));
                        dc.DrawLine(cx0, cy0, cx0 + p->FromDIP(6), cy0 + p->FromDIP(8));
                        dc.DrawLine(cx0, cy0 + p->FromDIP(8), cx0 + p->FromDIP(6), cy0);
                        x += p->FromDIP(12);
                    }
                }

                if (is_new_mix && x + p->FromDIP(40) < x_max) {
                    // "New" tag: teal-tinted pill (15% teal over the card bg).
                    const int tag_h = p->FromDIP(18);
                    const int tag_y = (sz.y - tag_h) / 2;
                    const wxColour tag_bg(
                        (unsigned char)(0.15 * 0 + 0.85 * 248),
                        (unsigned char)(0.15 * 150 + 0.85 * 247),
                        (unsigned char)(0.15 * 136 + 0.85 * 247));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(tag_bg));
                    dc.DrawRoundedRectangle(x + p->FromDIP(4), tag_y, p->FromDIP(37), tag_h, tag_h / 2);
                    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#009688")));
                    const wxString tag = _L("New");
                    const wxSize tsz2 = dc.GetTextExtent(tag);
                    dc.DrawText(tag, x + p->FromDIP(4) + (p->FromDIP(37) - tsz2.x) / 2, tag_y + (tag_h - tsz2.y) / 2);
                }
            } else {
                // Physical material: ellipsized name + down chevron
                wxFont nf2 = p->GetFont();
                nf2.SetPointSize(texture_font_pt(10));
                dc.SetFont(nf2);
                dc.SetTextForeground(name_fg);
                wxString name_str = get_filament_label(fil_idx);
                name_str = ellipsize_text(dc, name_str, x_max - x);
                const wxSize tsz = dc.GetTextExtent(name_str);
                dc.DrawText(name_str, x, (sz.y - tsz.y) / 2);

            }

            texture_draw_glyph(dc, p, chip_x + tgt_chip_w - p->FromDIP(28),
                (sz.y - p->FromDIP(16)) / 2.0,
                StateColor::darkModeColorFor(wxColour("#949494")));
        });

        row.target_panel->Bind(wxEVT_LEFT_DOWN, [this, ci](wxMouseEvent&) {
            show_filament_popup(ci);
        });
        row.target_panel->Bind(wxEVT_SIZE, [](wxSizeEvent& e) {
            e.Skip();
            static_cast<wxWindow*>(e.GetEventObject())->Refresh();
        });
        row.target_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

        row_sizer->Add(row.target_panel, 0, wxALIGN_CENTER_VERTICAL);

        row_panel->SetSizer(row_sizer);
        // 20px only BETWEEN rows (Figma 4+12+4 rhythm); no trailing gap after
        // the last row, so 4 rows = 4x32 + 3x20 = 188 and never overflow. A
        // row whose target is a mixed filament with an out-of-range ratio
        // carries the warning right below it instead (same thresholds as the
        // manual batch dialog's ratio guard).
        const bool last_row = (ci + 1 == m_current_matches.size());
        std::vector<wxString> warnings;
        if (row.target_filament_idx >= 0 && row.target_filament_idx < (int)m_filament_entries.size())
            warnings = mixed_ratio_warnings(m_filament_entries[row.target_filament_idx], display_numbers);
        m_mapping_sizer->Add(row_panel, 0, wxEXPAND | wxBOTTOM,
                             !warnings.empty() ? FromDIP(4) : (last_row ? 0 : FromDIP(20)));
        for (size_t wi = 0; wi < warnings.size(); ++wi) {
            auto* band = texture_make_ratio_warning(m_mapping_scroll, warnings[wi], tgt_chip_w);
            auto* warning_row = new wxBoxSizer(wxHORIZONTAL);
            warning_row->AddSpacer(src_group_w + FromDIP(12) + gap_after_swatch);
            warning_row->Add(band, 0);
            m_mapping_sizer->Add(warning_row, 0, wxBOTTOM,
                wi + 1 < warnings.size() ? FromDIP(4) : (last_row ? 0 : FromDIP(20)));
        }
    }

    m_mapping_scroll->FitInside();
    m_mapping_scroll->Layout();
    m_mapping_scroll->Thaw();
}


std::vector<Slic3r::FilamentMatch> TextureImportDialog::build_matches_from_rows() const
{
    std::vector<Slic3r::FilamentMatch> matches(m_mapping_rows.size());
    for (size_t i = 0; i < m_mapping_rows.size(); ++i) {
        auto& m = matches[i];
        m.cluster_index = m_mapping_rows[i].cluster_id;
        m.cluster_color = m_mapping_rows[i].source_color;

        int sel = m_mapping_rows[i].target_filament_idx;
        if (sel >= 0 && sel < (int)m_filament_colors_rgba.size()) {
            m.filament_index = sel;
            m.filament_color = m_filament_colors_rgba[sel];
            m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
        }
    }
    return matches;
}

// ---- Event handlers ----

void TextureImportDialog::update_color_count_preset_buttons()
{
    // While the raw input is out of range (e.g. 65 typed into 1..64) the
    // clamped parameter must NOT light a pill: nothing is selected (2026-11
    // feedback).
    const bool out_of_range = m_color_max_exceeded;
    if (m_btn_color_4)  m_btn_color_4->SetValue(!out_of_range && m_param_color_count == 4);
    if (m_btn_color_8)  m_btn_color_8->SetValue(!out_of_range && m_param_color_count == 8);
    if (m_btn_color_16) m_btn_color_16->SetValue(!out_of_range && m_param_color_count == 16);
    if (m_btn_color_32) m_btn_color_32->SetValue(!out_of_range && m_param_color_count == 32);
    if (m_btn_color_64) m_btn_color_64->SetValue(!out_of_range && m_param_color_count == 64);
}

bool TextureImportDialog::color_count_input_valid() const
{
    // Without a measurable original palette (geometry-only input) there is
    // nothing to exceed; otherwise the requested count must not surpass it
    // (doc section 3.3.1 note 4: over-count warns and blocks proceeding).
    if (m_original_color_count <= 0)
        return true;
    return (long long)m_param_color_count <= m_original_color_count;
}

void TextureImportDialog::update_color_count_warning()
{
    if (!m_color_warning_label)
        return;

    // 2026-11 design: one red inline error for any invalid color-count input —
    // a raw value outside 1..max_filament_count() (m_color_max_exceeded also
    // catches sub-1 input, see on_color_spin_text_changed) or a (clamped)
    // count above the original model's palette.
    const bool invalid = m_color_max_exceeded || !color_count_input_valid();

    update_color_input_error_style();
    // The dirty-hint / error gating must refresh with the flag change (the
    // typed path lands here directly, not through set_color_count_value).
    update_confirm_button_state();

    if (!invalid) {
        if (m_color_warning_label->IsShown()) {
            m_color_warning_label->Hide();
            Layout();
        }
    } else {
        m_color_warning_label->SetLabel(_L("Tip: color count exceeds the original model color count"));
        m_color_warning_label->Show();
        Layout();
    }
}

// Out-of-range color input: the box turns red (border + digits); a valid
// input restores the TextInput factory look (hover keeps its teal accent).
void TextureImportDialog::update_smooth_input_error_style()
{
    if (!m_smooth_spin)
        return;
    if (m_smooth_out_of_range) {
        const wxColour err("#FF383C");
        m_smooth_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(err, StateColor::Disabled),
            std::pair<wxColour, int>(err, StateColor::Hovered),
            std::pair<wxColour, int>(err, StateColor::Normal)));
        if (auto* tc = m_smooth_spin->GetTextCtrl()) {
            tc->SetForegroundColour(StateColor::darkModeColorFor(err));
            tc->Refresh();
        }
    } else {
        // TextInput::Create defaults (TextInput.cpp).
        m_smooth_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(0x00, 0x96, 0x88), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Normal)));
        if (auto* tc = m_smooth_spin->GetTextCtrl()) {
            // TextInput default Normal text (#262E30 -> #EFEFF0 in dark mode).
            tc->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
            tc->Refresh();
        }
    }
    m_smooth_spin->Refresh();
}

void TextureImportDialog::update_gap_input_error_style()
{
    if (!m_gap_spin)
        return;
    if (m_gap_out_of_range) {
        const wxColour err("#FF383C");
        m_gap_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(err, StateColor::Disabled),
            std::pair<wxColour, int>(err, StateColor::Hovered),
            std::pair<wxColour, int>(err, StateColor::Normal)));
        if (auto* tc = m_gap_spin->GetTextCtrl()) {
            tc->SetForegroundColour(StateColor::darkModeColorFor(err));
            tc->Refresh();
        }
    } else {
        // TextInput::Create defaults (TextInput.cpp).
        m_gap_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(0x00, 0x96, 0x88), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Normal)));
        if (auto* tc = m_gap_spin->GetTextCtrl()) {
            // TextInput default Normal text (#262E30 -> #EFEFF0 in dark mode).
            tc->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
            tc->Refresh();
        }
    }
    m_gap_spin->Refresh();
}

void TextureImportDialog::update_color_input_error_style()
{
    if (!m_color_spin)
        return;

    if (m_color_max_exceeded) {
        const wxColour err("#FF383C");
        m_color_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(err, StateColor::Disabled),
            std::pair<wxColour, int>(err, StateColor::Hovered),
            std::pair<wxColour, int>(err, StateColor::Normal)));
        if (auto* tc = m_color_spin->GetTextCtrl()) {
            tc->SetForegroundColour(StateColor::darkModeColorFor(err));
            tc->Refresh();
        }
    } else {
        // TextInput::Create defaults (TextInput.cpp).
        m_color_spin->SetBorderColor(StateColor(
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(0x00, 0x96, 0x88), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(0xDB, 0xDB, 0xDB), StateColor::Normal)));
        if (auto* tc = m_color_spin->GetTextCtrl()) {
            // TextInput default Normal text (#262E30 -> #EFEFF0 in dark mode).
            tc->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
            tc->Refresh();
        }
    }
    m_color_spin->Refresh();
}

void TextureImportDialog::set_color_count_value(int value, bool update_spin)
{
    m_param_color_count = std::clamp(value, 1, (int)max_filament_count());
    m_color_max_exceeded = (value > m_param_color_count);
    m_color_slider->SetValue(m_param_color_count);
    if (update_spin && m_color_spin && m_color_spin->GetTextCtrl())
        m_color_spin->GetTextCtrl()->ChangeValue(wxString::Format("%d", m_param_color_count));
    update_color_count_preset_buttons();
    update_color_count_warning();
    update_confirm_button_state();
}

void TextureImportDialog::set_smooth_value(int value, bool update_spin)
{
    m_param_smooth = std::clamp(value, 0, 10);
    m_smooth_slider->SetValue(m_param_smooth);
    if (update_spin && m_smooth_spin && m_smooth_spin->GetTextCtrl())
        m_smooth_spin->GetTextCtrl()->ChangeValue(wxString::Format("%d", m_param_smooth));
    update_confirm_button_state();
}

void TextureImportDialog::preview_spin_text_value(TextInput* input, AccentSlider* slider, int& param,
                                                  int min_value, int max_value, const wxString& text,
                                                  std::function<void()> on_value_changed)
{
    long value;
    if (!text.ToLong(&value)) {
        // Empty / partial input: leave the box as typed (Backspace deletes
        // char by char); the parameter keeps the slider current value.
        return;
    }

    wxTextCtrl* tc = input->GetTextCtrl();
    long parsed = value;
    value = std::clamp((int)parsed, min_value, max_value);

    // An out-of-range value (e.g. 65 typed into a 1..64 box) is NOT
    // normalized back into the box: the raw input stays visible so the
    // inline red error has something to point at (2026-11 feedback). Only
    // the cosmetic leading-zero case ("05") is rewritten. The parameter
    // itself is clamped, so Apply / Next stay pipeline-safe.
    if (text.length() > 1 && text[0] == '0') {
        const wxString normalized = wxString::Format("%ld", value);
        long pos = tc->GetInsertionPoint();
        tc->ChangeValue(normalized);
        tc->SetInsertionPoint(std::min<long>(normalized.length(), std::max(0L, pos - 1)));
    }

    param = (int)value;
    slider->SetValue(param);
    if (on_value_changed)
        on_value_changed();
    update_confirm_button_state();
}

void TextureImportDialog::on_color_preset_clicked(wxCommandEvent& evt)
{
    int id = evt.GetId();
    int color_count = m_param_color_count;
    if (id == ID_COLOR_4)  { color_count = 4; }
    if (id == ID_COLOR_8)  { color_count = 8; }
    if (id == ID_COLOR_16) { color_count = 16; }
    if (id == ID_COLOR_32) { color_count = 32; }
    if (id == ID_COLOR_64) { color_count = 64; }

    set_color_count_value(color_count, true);
}

void TextureImportDialog::on_color_slider_changed(wxCommandEvent&)
{
    set_color_count_value(m_color_slider->GetValue(), true);
}

void TextureImportDialog::on_color_spin_text_changed(wxCommandEvent& evt)
{
    // The typed-text path changes m_param_* live (before Enter/KILL_FOCUS fires
    // the spin event), so it must refresh the dirty-dependent buttons itself —
    // otherwise Apply keeps a stale highlight until the next slider/preset
    // interaction (the "sometimes right, sometimes wrong" symptom).
    preview_spin_text_value(m_color_spin, m_color_slider, m_param_color_count,
                            1, (int)max_filament_count(), evt.GetString(),
                            [this]() {
                                update_color_count_preset_buttons();
                                update_confirm_button_state();
                            });
    // Flag raw input OUTSIDE 1..max (the value itself was already clamped
    // live by preview_spin_text_value) so the inline red error can show —
    // covers both "65" and "0"-style input per the 2026-11 boundary note.
    long parsed = 0;
    const bool out_of_range = evt.GetString().ToLong(&parsed)
                              && (parsed > (long)max_filament_count() || parsed < 1);
    if (out_of_range != m_color_max_exceeded) {
        m_color_max_exceeded = out_of_range;
        update_color_count_warning();
    }
}

void TextureImportDialog::on_smooth_slider_changed(wxCommandEvent&)
{
    set_smooth_value(m_smooth_slider->GetValue(), true);
}

void TextureImportDialog::on_smooth_spin_text_changed(wxCommandEvent& evt)
{
    long raw = 0;
    const bool raw_ok = evt.GetString().ToLong(&raw);
    // Raw text outside 0..10: same red-error pattern as color count / gap.
    m_smooth_raw_out_of_range = raw_ok && (raw < 0 || raw > 10);
    m_smooth_out_of_range = m_smooth_raw_out_of_range;
    preview_spin_text_value(m_smooth_spin, m_smooth_slider, m_param_smooth,
                            0, 10, evt.GetString(),
                            [this]() { update_confirm_button_state(); });
    update_smooth_input_error_style();
}

void TextureImportDialog::on_gap_spin_text_changed(wxCommandEvent& evt)
{
    // Two-decimal UX fix: update the parameter and slider LIVE but never
    // rewrite the box while typing - ChangeValue("x.xx") per keystroke
    // fought the caret and made decimals hard to enter. The box text is
    // formatted only on an in-range Enter commit (see the bindings on
    // m_gap_spin in build_params_panel); out-of-range text stays as typed.
    double parsed = 0.0;
    if (!evt.GetString().ToDouble(&parsed))
        return; // partial input: keep as typed
    // Same red-error pattern as the color-count input: out-of-range raw
    // text (outside 0.00-5.00) stays visible, turns the box red and gates
    // Apply/Next; the parameter itself is clamped for pipeline safety.
    m_gap_out_of_range = (parsed < 0.0 || parsed > kGapAreaMaxPercent);
    set_gap_value(parsed, /*update_spin=*/false);
    update_gap_input_error_style();
    update_confirm_button_state();
    if (m_gap_warning_label) {
        m_gap_warning_label->SetLabel(_L("Notice: Gap area out of range (0.00-5.00%)."));
        m_gap_warning_label->Show(m_gap_out_of_range);
    }
}

// ---- Gap area ----

void TextureImportDialog::set_gap_value(double value, bool update_spin)
{
    value = std::round(std::clamp(value, 0.0, kGapAreaMaxPercent) * 100.0) / 100.0;
    m_param_gap_area = value;
    if (m_gap_slider)
        m_gap_slider->SetValue(gap_value_to_slider(m_param_gap_area));
    if (update_spin && m_gap_spin && m_gap_spin->GetTextCtrl())
        m_gap_spin->GetTextCtrl()->ChangeValue(wxString::Format("%.2f", m_param_gap_area));
    update_confirm_button_state();
}

int TextureImportDialog::visible_simplified_color_count() const
{
    const GapPreviewState& state = gap_preview();
    if (!state.display_face_colors.empty())
        return unique_face_color_count(state.display_face_colors);
    if (!state.simplified_face_colors.empty())
        return unique_face_color_count(state.simplified_face_colors);
    return (int)m_painted.cluster_colors.size();
}

void TextureImportDialog::update_color_counts_label()
{
    if (!m_lbl_color_counts)
        return;
    if (m_original_color_count <= 0) {
        m_lbl_color_counts->SetLabel(wxString());
        return;
    }
    m_lbl_color_counts->SetLabel(wxString::Format(_L("Original %d colors / Simplified %d colors"),
                                                  m_original_color_count,
                                                  visible_simplified_color_count()));
}

GapPreviewState& TextureImportDialog::gap_preview()
{
    if (!m_gap_preview)
        m_gap_preview = std::make_unique<GapPreviewState>();
    return *m_gap_preview;
}

const GapPreviewState& TextureImportDialog::gap_preview() const
{
    static const GapPreviewState empty;
    return m_gap_preview ? *m_gap_preview : empty;
}

int TextureImportDialog::gap_event_count_for_threshold(double area_threshold) const
{
    const GapPreviewState& state = gap_preview();
    int lo = 0, hi = (int)state.merge_events.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (state.merge_events[mid].key_area < area_threshold)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void TextureImportDialog::refresh_gap_preview()
{
    const GapPreviewState& state = gap_preview();
    if (m_preview_canvas && !state.display_face_colors.empty())
        m_preview_canvas->set_face_colors(state.display_face_colors);
}

void TextureImportDialog::apply_gap_area()
{
    GapPreviewState& state = gap_preview();
    if (state.color_patches.empty())
        return;
    if (state.display_face_colors.size() != state.simplified_face_colors.size()) {
        state.display_face_colors = state.simplified_face_colors;
        state.applied_events = 0;
    }
    if (state.display_face_colors.size() != m_painted.indices.size())
        return;

    // Regions whose merged area is below this share of the mesh are absorbed by a neighbor.
    const double area_threshold = (m_param_gap_area / 100.0) * state.gap_total_area;
    const int target = gap_event_count_for_threshold(area_threshold);
    const int npatches = (int)state.color_patches.size();
    auto write_event = [&](const GapMergeEvent& ev, const std::array<std::size_t, 3>& color) {
        for (int m = ev.member_begin; m < ev.member_end; ++m) {
            const int pid = state.merge_members[m];
            if (pid >= 0 && pid < npatches)
                write_patch_display_color(state, state.color_patches[pid], color);
        }
    };

    // Replay forward when the threshold grows and undo in reverse when it shrinks,
    // so any threshold reproduces the same colors regardless of slider history.
    for (int i = state.applied_events; i < target; ++i)
        write_event(state.merge_events[i], state.merge_events[i].to_color);
    for (int i = state.applied_events - 1; i >= target; --i)
        write_event(state.merge_events[i], state.merge_events[i].from_color);
    state.applied_events = target;

    refresh_gap_preview();
    update_color_counts_label();
}

void TextureImportDialog::install_gap_preview(std::unique_ptr<GapPreviewState> preview)
{
    if (preview)
        m_gap_preview = std::move(preview);
    else if (!m_gap_preview)
        m_gap_preview = std::make_unique<GapPreviewState>();
    GapPreviewState& state = gap_preview();
    if (!state.simplified_face_colors.empty()) {
        state.display_face_colors = state.simplified_face_colors;
        state.applied_events = 0;
    }
    // A recompute keeps the user's gap setting: the merge list is replayed at the
    // current threshold instead of resetting the slider (divergence from upstream).
    apply_gap_area();
}

bool TextureImportDialog::fold_gap_into_painted()
{
    GapPreviewState& state = gap_preview();
    if (state.display_face_colors.empty()) {
        if (!state.simplified_face_colors.empty() &&
            state.simplified_face_colors.size() == m_painted.face_colors.size())
            state.display_face_colors = state.simplified_face_colors;
        else
            return false;
    }
    if (state.display_face_colors.size() != m_painted.face_colors.size())
        return false;

    m_painted.face_colors = state.display_face_colors;
    m_painted.cluster_colors = remaining_cluster_colors(
        state.simplified_cluster_colors, state.display_face_colors);

    if (m_preview_canvas)
        m_preview_canvas->set_face_colors(m_painted.face_colors);
    return true;
}

void TextureImportDialog::commit_gap_area_to_painted()
{
    if (!fold_gap_into_painted())
        return;

    // Migrate from what the rows currently show: the user's manual picks live
    // in the UI rows, not in m_current_matches, so reading the rows is what
    // keeps those bindings across the rebuild below.
    const auto previous_matches = build_matches_from_rows();
    do_auto_match();

    std::vector<Slic3r::FilamentMatch> ordered;
    ordered.reserve(m_current_matches.size());
    std::set<std::array<std::size_t, 3>> used;
    for (const auto& prev : previous_matches) {
        auto it = std::find_if(m_current_matches.begin(), m_current_matches.end(),
            [&](const Slic3r::FilamentMatch& match) {
                return match.cluster_color == prev.cluster_color;
            });
        if (it == m_current_matches.end())
            continue;
        it->filament_index = prev.filament_index;
        it->filament_color = prev.filament_color;
        it->delta_e = prev.delta_e;
        ordered.push_back(*it);
        used.insert(it->cluster_color);
    }
    for (const auto& match : m_current_matches) {
        if (used.count(match.cluster_color))
            continue;
        ordered.push_back(match);
    }
    m_current_matches = std::move(ordered);
    compact_used_virtual_filaments();

    if (m_preview_canvas)
        m_preview_canvas->set_face_colors(m_painted.face_colors);
    update_filament_color_map();
    update_color_counts_label();
    if (m_mapping_scroll)
        rebuild_mapping_rows();
}

void TextureImportDialog::on_gap_slider_changed(wxCommandEvent&)
{
    if (m_state == TextureImportState::Computing)
        return;
    if (!m_gap_slider)
        return;
    set_gap_value(m_gap_slider->GetValue() / 100.0, true);
    // Gap is Apply-gated like color count / smooth: the value only marks the
    // params dirty here and takes effect in on_apply_clicked. Uncomment to
    // restore live drag preview.
    // apply_gap_area();
}

void TextureImportDialog::on_gap_spin_commit()
{
    if (m_state == TextureImportState::Computing)
        return;
    if (!m_gap_spin || !m_gap_spin->GetTextCtrl())
        return;
    const wxString text = m_gap_spin->GetTextCtrl()->GetValue();
    double parsed = 0.0;
    if (!text.ToDouble(&parsed)) {
        m_gap_spin->GetTextCtrl()->ChangeValue(wxString::Format("%.2f", m_param_gap_area));
        return;
    }
    set_gap_value(parsed, true);
    // apply_gap_area();  // Apply-gated, see on_gap_slider_changed.
}

void TextureImportDialog::on_keep_colors_toggled(wxCommandEvent&)
{
    m_keep_colors = !m_keep_colors_cb || m_keep_colors_cb->GetValue();
    // Apply-gated like the gap value itself: toggling marks the params dirty.
    update_confirm_button_state();
}

void TextureImportDialog::on_apply_clicked(wxCommandEvent&)
{
    // Full recompute when the clustering inputs changed (or there is nothing
    // to fold into yet); the gap fold then happens in on_computation_complete.
    if (m_param_color_count != m_applied_color_count
        || m_param_smooth != m_applied_smooth
        || !has_valid_result()) {
        start_computation();
        return;
    }

    // Gap-only change: replay the precomputed merge list at the new threshold
    // and fold it into m_painted without re-running the clustering. With Keep
    // Colors the merge must not reduce the palette, so the threshold snaps
    // down to the largest value keeping every color alive, and the slider
    // follows; without it the merge may absorb whole colors and the count
    // slider syncs to the merged result.
    const double cap = m_keep_colors ? gap_preview().max_merge_percent : kGapAreaMaxPercent;
    set_gap_value(std::min(m_param_gap_area, cap), true);
    // A changed palette invalidates the saved pre-mix mapping. Restore it
    // before remapping so a later mode switch cannot restore stale clusters.
    if (m_auto_mix_snapshot || m_auto_mix_pending)
        reset_auto_mix();
    apply_gap_area();
    commit_gap_area_to_painted();
    // Same value as the clustering output when Keep Colors is on (no change).
    set_color_count_value(visible_simplified_color_count(), true);
    m_applied_gap_area    = m_param_gap_area;
    m_applied_keep_colors = m_keep_colors;
    m_applied_color_count = m_param_color_count;
    m_applied_smooth      = m_param_smooth;
    update_confirm_button_state();
}

void TextureImportDialog::on_skip_clicked(wxCommandEvent&)
{
    m_skipped = true;
    m_new_filament_colors.clear();
    m_new_filament_preset_names.clear();
    m_new_mixed_filaments.clear();
    m_current_matches.clear();
    cancel_computation();
    EndModal(wxID_CANCEL);
}

bool TextureImportDialog::has_valid_result() const
{
    if (m_painted.face_colors.empty() || m_current_matches.empty() || m_mapping_rows.empty())
        return false;

    if (m_mapping_rows.size() != m_current_matches.size())
        return false;

    const int filament_count = (int)std::min(m_filament_colors_rgba.size(), max_filament_count());
    for (const auto& row : m_mapping_rows) {
        if (row.target_filament_idx < 0 || row.target_filament_idx >= filament_count)
            return false;
    }
    return true;
}

bool TextureImportDialog::is_params_dirty() const
{
    if (m_applied_color_count < 0)
        return false;
    return m_param_color_count != m_applied_color_count
        || m_param_smooth      != m_applied_smooth
        || m_param_gap_area    != m_applied_gap_area;
}

void TextureImportDialog::update_drop_warning_visibility()
{
    if (!m_drop_warning_label) return;
    // Show only when the most recent do_auto_match() ran into the filament
    // cap AND we are in the Ready state. The flag is reset at every
    // do_auto_match() entry, so any "clean" re-run automatically hides the
    // warning even if a previous run had dropped clusters.
    // Scoped to the mapping step: the two-step design has no such element on
    // the simplification page (2026-11 feedback: it wrongly appeared at the
    // bottom after a next/back round trip).
    const bool show = (m_state == TextureImportState::Ready) && m_filaments_dropped
                      && (m_step == ImportStep::Mapping);
    if (m_drop_warning_label->IsShown() == show) return;
    m_drop_warning_label->Show(show);
    Layout();
}

void TextureImportDialog::update_confirm_button_state()
{
    // Apply must follow the validity of the color-count input on every value
    // change, not only on state changes (doc section 3.3.1 note 4 blocks proceeding
    // while the count exceeds the original palette). "Next" shares the gate.
    const bool color_input_ok = m_state != TextureImportState::Computing
                                && color_count_input_valid() && !m_color_max_exceeded;
    const bool gap_input_ok = !m_gap_out_of_range;
    const bool smooth_input_ok = !m_smooth_out_of_range;
    // Error-first priority (2026-11 feedback): at most ONE red message is
    // visible (color > gap > smooth); the orange change-hint only shows
    // when no red error is active. Same slot, same timing as color count.
    const bool color_err = m_color_max_exceeded;
    const bool gap_err = m_gap_out_of_range;
    const bool smooth_err = m_smooth_out_of_range;
    const bool any_red = color_err || gap_err || smooth_err;
    if (m_color_warning_label) {
        m_color_warning_label->SetLabel(_L("Tip: color count exceeds the original model color count"));
        m_color_warning_label->Show(color_err);
    }
    if (m_gap_warning_label) {
        m_gap_warning_label->SetLabel(_L("Notice: Gap area out of range (0.00-5.00%)."));
        m_gap_warning_label->Show(gap_err && !color_err);
    }
    if (m_smooth_warning_label) {
        m_smooth_warning_label->SetLabel(_L("Notice: Smooth level out of range (0-10)."));
        m_smooth_warning_label->Show(smooth_err && !color_err && !gap_err);
    }
    if (m_hint_label)
        m_hint_label->Show(is_params_dirty() && !any_red);
    style_apply_button(color_input_ok && gap_input_ok && smooth_input_ok && is_params_dirty());
    if (m_btn_next)
        m_btn_next->Enable(color_input_ok && gap_input_ok && smooth_input_ok);

    if (m_state != TextureImportState::Ready)
        return;

    if (!has_valid_result()) {
        m_btn_ok->Enable(false);
        if (m_hint_label) m_hint_label->Hide();
        m_btn_ok->Refresh();
        Layout();
        return;
    }

    m_btn_ok->Enable(true);
    // 2026-11 spec: confirming with UNAPPLIED step-1 param edits is allowed -
    // it exports the last APPLIED simplification and the mapping built on it.
    // The button therefore always reads as the clickable accent here; the
    // muted "go apply first" look read as a disabled control (2026-11
    // feedback). The step-1 change hint above is untouched.
    apply_accent_button_colours(m_btn_ok);
    m_btn_ok->UnsetToolTip();

    m_btn_ok->Refresh();
    Layout();
}

// Both state updaters land here: the Confirm button reads as accent only while it would apply
// exactly what the preview shows.
void TextureImportDialog::style_confirm_button(bool dirty)
{
    if (dirty) {
        apply_muted_button_colours(m_btn_ok);
        m_btn_ok->SetToolTip(_L("Reminder: parameters changed, click Apply to take effect"));
    } else {
        apply_accent_button_colours(m_btn_ok);
        m_btn_ok->UnsetToolTip();
    }
    if (m_hint_label)
        m_hint_label->Show(dirty && !m_color_max_exceeded
                           && !m_gap_out_of_range && !m_smooth_out_of_range
                           && color_count_input_valid());
}

// Step-1 design: Apply is a grey DISABLED pill while the preview already shows
// the current parameters (or input is invalid / computing); it turns into the
// clickable #009688 accent pill exactly when clicking it would apply something.
void TextureImportDialog::style_apply_button(bool actionable)
{
    if (!m_btn_apply)
        return;
    if (actionable) {
        apply_accent_button_colours(m_btn_apply);
        m_btn_apply->Enable(true);
    } else {
        apply_muted_button_colours(m_btn_apply);
        m_btn_apply->Enable(false);
    }
    m_btn_apply->Refresh();
}

void TextureImportDialog::on_ok_clicked(wxCommandEvent&)
{
    // 2026-11 spec: UNAPPLIED step-1 param edits do NOT block confirming -
    // confirm exports the last APPLIED simplification (m_painted) and the
    // mapping built on it, manual row adjustments included. The unapplied
    // values are simply discarded with the dialog.
    if (m_state != TextureImportState::Ready || !has_valid_result())
        return;

    // The gap merge is folded into m_painted at Apply time (both the
    // recompute path and the gap-only fast path), so nothing to fold here; an
    // unapplied gap change is blocked by is_params_dirty above. Uncommenting
    // this would re-run do_auto_match on stale m_current_matches and wipe the
    // user's manual row picks.
    // commit_gap_area_to_painted();

    m_current_matches = build_matches_from_rows();
    if (m_current_matches.empty())
        return;

    compact_used_virtual_filaments();

    EndModal(wxID_OK);
}

// ---- Result accessors ----

Slic3r::PaintedMesh TextureImportDialog::get_painted_mesh() const
{
    return m_painted;
}

std::vector<Slic3r::FilamentMatch> TextureImportDialog::get_matches() const
{
    if (!m_current_matches.empty())
        return m_current_matches;
    return build_matches_from_rows();
}

int TextureImportDialog::get_simplified_color_count() const
{
    return (int)m_painted.cluster_colors.size();
}

void TextureImportDialog::on_dpi_changed(const wxRect&)
{
    // All control sizes below are baked into persistent properties (min size,
    // corner radius, fixed wxSize) using FromDIP() at build time. The base
    // DPIAware::rescale() only rescales fonts; it does not recompute these
    // stored pixel values. Re-apply them here so the layout stays consistent
    // when the dialog is dragged to a screen with a different DPI.
    SetMinSize(wxSize(FromDIP(789), FromDIP(740)));

    for (Button* btn : {m_btn_color_4, m_btn_color_8, m_btn_color_16, m_btn_color_32, m_btn_color_64}) {
        if (btn) {
            btn->SetCornerRadius(FromDIP(4));
            btn->SetMinSize(wxSize(FromDIP(56), FromDIP(24)));
            if (auto* owner = btn->GetContainingSizer()) {
                if (auto* item = owner->GetItem(btn))
                    item->SetBorder(btn == m_btn_color_4 ? 0 : FromDIP(kColorPresetGapDip));
            }
        }
    }

    if (m_btn_apply) {
        m_btn_apply->SetCornerRadius(FromDIP(8));
        m_btn_apply->SetMinSize(wxSize(FromDIP(56), FromDIP(32)));
    }

    if (m_color_spin)
        m_color_spin->SetMinSize(wxSize(FromDIP(42), FromDIP(24)));
    if (m_smooth_spin)
        m_smooth_spin->SetMinSize(wxSize(FromDIP(42), FromDIP(24)));
    if (m_gap_spin)
        m_gap_spin->SetMinSize(wxSize(FromDIP(42), FromDIP(24)));

    if (m_color_slider)
        m_color_slider->SetMinSize(wxSize(FromDIP(205), FromDIP(24)));
    if (m_smooth_slider)
        m_smooth_slider->SetMinSize(wxSize(FromDIP(208), FromDIP(24)));
    if (m_gap_slider)
        m_gap_slider->SetMinSize(wxSize(FromDIP(kGapSliderWidthDip), FromDIP(24)));

    if (m_step_indicator)
        m_step_indicator->SetMinSize(wxSize(-1, FromDIP(38)));

    if (m_mapping_scroll) {
        m_mapping_scroll->SetMinSize(wxSize(-1, FromDIP(200)));
        m_mapping_scroll->SetScrollRate(0, FromDIP(20));
    }

    for (Button* btn : {m_btn_skip, m_btn_next, m_btn_back, m_btn_reset, m_btn_ok}) {
        if (btn) {
            btn->SetCornerRadius(FromDIP(4));
            btn->SetMinSize(wxSize(FromDIP(160), FromDIP(36)));
        }
    }

    texture_update_mix_mode_metrics(m_mix_mode_trigger);
    apply_preview_metrics();

    // Badge textures were rasterized at the old scale (fonts + px measure);
    // invalidate so the canvas re-renders them at the new DPI.
    if (m_preview_canvas)
        m_preview_canvas->invalidate_badge_textures();

    // Mapping rows store their panel sizes (source/target/arrow/row height)
    // as fixed FromDIP min/max sizes, so rebuild them to pick up the new DPI.
    rebuild_mapping_rows();

    if (wxSizer* sizer = GetSizer())
        sizer->Layout();
    Layout();
    Refresh();
    wxGetApp().UpdateDlgDarkUI(this);
}

}} // namespace Slic3r::GUI
