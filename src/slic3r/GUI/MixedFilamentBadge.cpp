#include "MixedFilamentBadge.hpp"

#include "Widgets/Label.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "BitmapCache.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

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

namespace {

bool is_very_light_rgb(const wxColour& color)
{
    return color.Red() > 224 && color.Green() > 224 && color.Blue() > 224;
}

wxColour valid_or_fallback(const wxColour& color, const wxColour& fallback = wxColour("#808080"))
{
    return color.IsOk() ? color : fallback;
}

ColorBlockParams::Mode effective_mode(const ColorBlockParams& params)
{
    if (params.mode == ColorBlockParams::Gradient && params.colors.size() >= 2)
        return ColorBlockParams::Gradient;
    if (params.mode == ColorBlockParams::Segments && params.colors.size() >= 2)
        return ColorBlockParams::Segments;
    return ColorBlockParams::Solid;
}

wxColour effective_solid_color(const ColorBlockParams& params)
{
    if (!params.colors.empty())
        return valid_or_fallback(params.colors.front());
    return valid_or_fallback(params.solid_color);
}

std::vector<wxColour> effective_color_list(const ColorBlockParams& params)
{
    const ColorBlockParams::Mode mode = effective_mode(params);
    if (mode == ColorBlockParams::Solid)
        return {effective_solid_color(params)};

    std::vector<wxColour> colors;
    colors.reserve(params.colors.size());
    for (const wxColour& color : params.colors)
        colors.emplace_back(valid_or_fallback(color));
    return colors;
}

double color_block_text_luminance(const ColorBlockParams& params)
{
    const std::vector<wxColour> colors = effective_color_list(params);
    double luminance = 0.0;
    for (const wxColour& color : colors)
        luminance += color.GetLuminance();
    return colors.empty() ? 1.0 : luminance / double(colors.size());
}

bool color_block_needs_border(const ColorBlockParams& params)
{
    const ColorBlockParams::Mode mode   = effective_mode(params);
    const std::vector<wxColour>  colors = effective_color_list(params);

    if (mode == ColorBlockParams::Segments) {
        for (const wxColour& color : colors)
            if (is_very_light_rgb(color))
                return true;
        return false;
    }

    for (const wxColour& color : colors) {
        if (!is_very_light_rgb(color))
            return false;
    }
    return !colors.empty();
}

std::string color_block_cache_key(const ColorBlockParams& params)
{
    std::ostringstream key;
    key << "color-block:mode=" << int(effective_mode(params))
        << ":dir=" << int(params.gradient_direction)
        << ":h" << params.height
        << ":w" << params.width
        << ":label=" << params.label.ToStdString();

    if (effective_mode(params) == ColorBlockParams::Solid) {
        key << ':' << effective_solid_color(params).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    } else {
        for (const wxColour& color : effective_color_list(params))
            key << ':' << color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    }

    return key.str();
}

void draw_gradient_block(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colors, ColorBlockParams::GradientDirection direction)
{
    if (direction == ColorBlockParams::LeftToRight) {
        for (int x = rect.GetLeft(); x <= rect.GetRight(); ++x) {
            const double pos = double(x - rect.GetLeft()) / double(std::max(1, rect.GetWidth() - 1));
            dc.SetPen(wxPen(interpolate_color(colors, pos)));
            dc.DrawLine(x, rect.GetTop(), x, rect.GetBottom() + 1);
        }
        return;
    }

    for (int y = rect.GetBottom(); y >= rect.GetTop(); --y) {
        const double pos = double(rect.GetBottom() - y) / double(std::max(1, rect.GetHeight() - 1));
        dc.SetPen(wxPen(interpolate_color(colors, pos)));
        dc.DrawLine(rect.GetLeft(), y, rect.GetRight() + 1, y);
    }
}

void draw_segments_block(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colors)
{
    const size_t count = colors.size();
    for (size_t idx = 0; idx < count; ++idx) {
        const int x0 = rect.GetLeft() + int(std::floor(double(rect.GetWidth()) * double(idx) / double(count)));
        const int x1 = rect.GetLeft() + int(std::ceil(double(rect.GetWidth()) * double(idx + 1) / double(count)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(colors[idx]));
        dc.DrawRectangle(x0, rect.GetTop(), std::max(1, x1 - x0), rect.GetHeight());
    }
}

void draw_color_block(wxDC& dc, const wxRect& rect, const ColorBlockParams& params)
{
    const ColorBlockParams::Mode mode   = effective_mode(params);
    const std::vector<wxColour>  colors = effective_color_list(params);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(colors.front()));
    dc.DrawRectangle(rect);

    if (mode == ColorBlockParams::Gradient) {
        draw_gradient_block(dc, rect, colors, params.gradient_direction);
    } else if (mode == ColorBlockParams::Segments) {
        draw_segments_block(dc, rect, colors);
    } else {
        dc.SetBrush(wxBrush(colors.front()));
        dc.DrawRectangle(rect);
    }

    if (color_block_needs_border(params)) {
        dc.SetPen(*wxGREY_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

    if (!params.label.empty()) {
        dc.SetTextForeground(color_block_text_luminance(params) < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawLabel(params.label, rect, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }
}

} // namespace

MixedFilamentBadge::MixedFilamentBadge(wxWindow* parent, wxWindowID id, int virtual_id,
                                       const MixedFilament& mf,
                                       const MixedFilamentDisplayContext& display_context,
                                       bool show_number, int badge_size)
    : wxPanel(parent, id, wxDefaultPosition, wxSize(badge_size, badge_size), wxBORDER_NONE)
    , m_show_number(show_number)
    , m_label(wxString::Format("%d", virtual_id))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMinSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    SetMaxSize(parent->FromDIP(wxSize(badge_size, badge_size)));
    Bind(wxEVT_PAINT, &MixedFilamentBadge::on_paint, this);
    Bind(wxEVT_LEFT_UP, &MixedFilamentBadge::on_left_up, this);
    SetCursor(wxCursor(wxCURSOR_ARROW));

    SetFont(badge_size >= 20 ? Label::Body_12 : Label::Body_8);

    m_color_block.width              = badge_size;
    m_color_block.height             = badge_size;
    m_color_block.label              = m_label;
    m_color_block.gradient_direction = ColorBlockParams::BottomToTop;
    m_color_block.mode               = ColorBlockParams::Solid;
    m_color_block.solid_color        = parse_mixed_color(mf.display_color);

    if (is_simple_gradient(mf)) {
        auto get_color = [&](unsigned fid) -> wxColour {
            if (fid == 0 || fid > display_context.physical_colors.size()) return wxColour("#26A69A");
            return parse_mixed_color(display_context.physical_colors[fid - 1]);
        };
        const wxColour ca = get_color(mf.component_a);
        const wxColour cb = get_color(mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;
        m_color_block.mode = ColorBlockParams::Gradient;
        m_color_block.colors.push_back(a_to_b ? ca : cb);
        m_color_block.colors.push_back(a_to_b ? cb : ca);
    }

    SetForegroundColour(color_block_text_luminance(m_color_block) < 0.51 ? *wxWHITE : *wxBLACK);
}

void MixedFilamentBadge::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    dc.SetFont(GetFont());

    ColorBlockParams params = m_color_block;
    params.label = m_show_number ? m_label : wxString();
    draw_color_block(dc, GetClientRect(), params);
}

void MixedFilamentBadge::on_left_up(wxMouseEvent&)
{
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

// ---------------------------------------------------------------------------
// get_color_block_bitmap_cached — unified cached bitmap factory
// ---------------------------------------------------------------------------

wxBitmap* get_color_block_bitmap_cached(const ColorBlockParams& params)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    ColorBlockParams draw_params = params;
    draw_params.width  = std::max(1, params.width);
    draw_params.height = std::max(1, params.height);

    const std::string key = color_block_cache_key(draw_params);

    wxBitmap* cached = cache.find(key);
    if (cached != nullptr)
        return cached;

    wxBitmap bmp(draw_params.width, draw_params.height);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    const bool use_small_font = std::min(draw_params.width, draw_params.height) < 20;
    dc.SetFont(use_small_font ? ::Label::Body_8 : ::Label::Body_12);

    draw_color_block(dc, wxRect(0, 0, draw_params.width, draw_params.height), draw_params);
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
        params.gradient_direction = ColorBlockParams::BottomToTop;
        params.colors.push_back(a_to_b ? ca : cb);
        params.colors.push_back(a_to_b ? cb : ca);
    } else {
        params.mode = ColorBlockParams::Solid;
        params.solid_color = parse_mixed_color(mf.display_color.empty() ? "#808080" : mf.display_color);
    }

    return get_color_block_bitmap_cached(params);
}

}} // namespace Slic3r::GUI
