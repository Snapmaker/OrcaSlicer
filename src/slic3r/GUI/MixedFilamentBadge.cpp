#include "MixedFilamentBadge.hpp"

#include "Widgets/Label.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "BitmapCache.hpp"
#include "libslic3r/MixedFilament.hpp"

namespace Slic3r { namespace GUI {

wxColour interpolate_color(const std::vector<wxColour>& colors, double pos)
{
    if (colors.empty()) return *wxWHITE;
    if (colors.size() == 1) return colors[0];

    pos = std::max(0.0, std::min(1.0, pos));

    double segment_size = 1.0 / (colors.size() - 1);
    int segment = int(pos / segment_size);
    segment = std::min(segment, int(colors.size()) - 2);

    double local_pos = (pos - segment * segment_size) / segment_size;

    const wxColour& c1 = colors[segment];
    const wxColour& c2 = colors[segment + 1];

    int r = int(c1.Red()   * (1.0 - local_pos) + c2.Red()   * local_pos);
    int g = int(c1.Green() * (1.0 - local_pos) + c2.Green() * local_pos);
    int b = int(c1.Blue()  * (1.0 - local_pos) + c2.Blue()  * local_pos);

    return wxColour(r, g, b);
}

MixedFilamentBadge::MixedFilamentBadge(wxWindow* parent, wxWindowID id, int virtual_id,
                                       const MixedFilament& mf,
                                       const MixedFilamentDisplayContext& display_context,
                                       bool show_number, int badge_size)
    : wxButton(parent, id, wxString::Format("%d", virtual_id),
               wxDefaultPosition, wxSize(badge_size, badge_size),
               wxBU_EXACTFIT | wxBORDER_NONE)
    , m_show_number(show_number)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMinSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMaxSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    Bind(wxEVT_PAINT, &MixedFilamentBadge::on_paint, this);
    // Prevent wxButton from entering pressed/focused state that overrides custom paint
    Bind(wxEVT_SET_FOCUS, [](wxFocusEvent&) {});
    Bind(wxEVT_KILL_FOCUS, [](wxFocusEvent&) {});
    SetCanFocus(false);

    SetFont(badge_size >= 20 ? Label::Body_12 : Label::Body_8);

    m_solid_color = parse_mixed_color(mf.display_color);

    m_is_gradient = is_simple_gradient(mf);

    if (m_is_gradient) {
        auto get_color = [&](unsigned fid) -> wxColour {
            if (fid == 0 || fid > display_context.physical_colors.size()) return wxColour("#26A69A");
            return parse_mixed_color(display_context.physical_colors[fid - 1]);
        };
        const wxColour ca = get_color(mf.component_a);
        const wxColour cb = get_color(mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;
        m_gradient_colors.push_back(a_to_b ? ca : cb);
        m_gradient_colors.push_back(a_to_b ? cb : ca);
    }

    // Font color: for gradient use average luminance of the two endpoint colors
    if (m_is_gradient && m_gradient_colors.size() >= 2) {
        double lum0 = m_gradient_colors[0].GetLuminance();
        double lum1 = m_gradient_colors[1].GetLuminance();
        double avg_lum = (lum0 + lum1) * 0.5;
        SetForegroundColour(avg_lum < 0.51 ? *wxWHITE : *wxBLACK);
    } else {
        SetForegroundColour(m_solid_color.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    }
}

void MixedFilamentBadge::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    wxRect rect = GetClientRect();

    if (m_is_gradient && m_gradient_colors.size() >= 2) {
        // Draw gradient from bottom to top
        for (int y = rect.GetBottom(); y >= rect.GetTop(); --y) {
            double pos = double(rect.GetBottom() - y) / double(rect.GetHeight());
            wxColour color = interpolate_color(m_gradient_colors, pos);
            dc.SetPen(wxPen(color));
            dc.DrawLine(rect.GetLeft(), y, rect.GetRight(), y);
        }

        // Draw grey border only if BOTH endpoint colors are very light (R/G/B all > 224)
        const auto& c0 = m_gradient_colors[0];
        const auto& c1 = m_gradient_colors[1];
        bool both_light = (c0.Red() > 224 && c0.Green() > 224 && c0.Blue() > 224)
                       && (c1.Red() > 224 && c1.Green() > 224 && c1.Blue() > 224);
        if (both_light) {
            dc.SetPen(*wxGREY_PEN);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        }
    } else {
        // Draw solid color
        dc.SetBrush(wxBrush(m_solid_color));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(rect);

        // Draw grey border for very light colors (R/G/B all > 224)
        if (m_solid_color.Red() > 224 && m_solid_color.Green() > 224 && m_solid_color.Blue() > 224) {
            dc.SetPen(*wxGREY_PEN);
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
        }
    }

    // Draw text — compute color using same luminance rule as constructor
    if (m_show_number) {
        wxString label = GetLabel();
        wxFont font = GetFont();
        dc.SetFont(font);

        double text_lum;
        if (m_is_gradient && m_gradient_colors.size() >= 2) {
            double lum0 = m_gradient_colors[0].GetLuminance();
            double lum1 = m_gradient_colors[1].GetLuminance();
            text_lum = (lum0 + lum1) * 0.5;
        } else {
            text_lum = m_solid_color.GetLuminance();
        }
        dc.SetTextForeground(text_lum < 0.51 ? *wxWHITE : *wxBLACK);

        wxSize text_size = dc.GetTextExtent(label);
        int x = (rect.GetWidth() - text_size.GetWidth()) / 2;
        int y = (rect.GetHeight() - text_size.GetHeight()) / 2;
        dc.DrawText(label, rect.GetLeft() + x, rect.GetTop() + y);
    }
}

// ---------------------------------------------------------------------------
// get_color_block_bitmap_cached — unified cached bitmap factory
// ---------------------------------------------------------------------------

wxBitmap* get_color_block_bitmap_cached(const ColorBlockParams& params)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    // Build a deterministic cache key.
    std::string key;
    if (params.mode == ColorBlockParams::Gradient && params.gradient_colors.size() >= 2) {
        key = "grad:";
        key += params.gradient_colors[0].GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        for (size_t i = 1; i < params.gradient_colors.size(); ++i) {
            key += ":";
            key += params.gradient_colors[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        }
        key += "BT:h" + std::to_string(params.height)
            + ":w" + std::to_string(params.width)
            + ":" + params.label.ToStdString();
    } else {
        key = "solid:";
        key += params.solid_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        key += ":h" + std::to_string(params.height)
            + ":w" + std::to_string(params.width)
            + ":" + params.label.ToStdString();
    }

    wxBitmap* cached = cache.find(key);
    if (cached != nullptr)
        return cached;

    wxBitmap bmp(params.width, params.height);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    const bool use_small_font = std::min(params.width, params.height) < 20;
    dc.SetFont(use_small_font ? ::Label::Body_8 : ::Label::Body_12);

    bool very_light = false;

    if (params.mode == ColorBlockParams::Gradient && params.gradient_colors.size() >= 2) {
        const auto& stops = params.gradient_colors;
        for (int y = params.height - 1; y >= 0; --y) {
            double pos = double(params.height - 1 - y) / double(std::max(1, params.height - 1));
            wxColour col = interpolate_color(stops, pos);
            dc.SetPen(wxPen(col));
            dc.DrawLine(0, y, params.width, y);
        }

        very_light = true;
        for (const auto& c : stops) {
            if (c.Red() <= 224 || c.Green() <= 224 || c.Blue() <= 224) {
                very_light = false;
                break;
            }
        }

        double avg_lum = 0;
        for (const auto& c : stops) avg_lum += c.GetLuminance();
        avg_lum /= stops.size();
        dc.SetTextForeground(avg_lum < 0.51 ? *wxWHITE : *wxBLACK);
    } else {
        const wxColour& clr = params.solid_color;
        dc.SetBackground(wxBrush(clr));
        dc.Clear();
        dc.SetBrush(wxBrush(clr));
        very_light = (clr.Red() > 224 && clr.Green() > 224 && clr.Blue() > 224);
        dc.SetTextForeground(clr.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
    }

    if (very_light) {
        dc.SetPen(*wxGREY_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, params.width, params.height);
    }

    wxSize txt_sz = dc.GetTextExtent(params.label);
    dc.DrawText(params.label, (params.width - txt_sz.x) / 2, (params.height - txt_sz.y) / 2);
    dc.SelectObject(wxNullBitmap);

    return cache.insert(key, bmp);
}

// ---------------------------------------------------------------------------
// Free function — shared between badge and merge menus
// ---------------------------------------------------------------------------

wxBitmap* create_mixed_filament_menu_bitmap(const MixedFilament&               mf,
                                           const MixedFilamentDisplayContext& ctx,
                                           int  width, int  height,
                                           const wxString& label)
{
    ColorBlockParams params;
    params.width  = width;
    params.height = height;
    params.label  = label;

    const bool is_gradient = is_simple_gradient(mf);

    if (is_gradient) {
        auto get_c = [&](unsigned fid) -> wxColour {
            if (fid == 0 || fid > ctx.physical_colors.size())
                return wxColour("#26A69A");
            return parse_mixed_color(ctx.physical_colors[fid - 1]);
        };
        const wxColour ca = get_c(mf.component_a);
        const wxColour cb = get_c(mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;

        params.mode = ColorBlockParams::Gradient;
        params.gradient_colors.push_back(a_to_b ? ca : cb);
        params.gradient_colors.push_back(a_to_b ? cb : ca);
    } else {
        params.mode = ColorBlockParams::Solid;
        params.solid_color = parse_mixed_color(mf.display_color.empty() ? "#808080" : mf.display_color);
    }

    return get_color_block_bitmap_cached(params);
}

}} // namespace Slic3r::GUI
