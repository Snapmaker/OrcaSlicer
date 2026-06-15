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
    if (colors.empty())
        return *wxWHITE;
    if (colors.size() == 1)
        return colors[0];

    pos = std::max(0.0, std::min(1.0, pos));

    double segment_size = 1.0 / (colors.size() - 1);
    int segment = static_cast<int>(pos / segment_size);
    segment = std::min(segment, static_cast<int>(colors.size()) - 2);

    double local_pos = (pos - segment * segment_size) / segment_size;

    const wxColour& c1 = colors[segment];
    const wxColour& c2 = colors[segment + 1];

    int r = static_cast<int>(c1.Red() * (1.0 - local_pos) + c2.Red() * local_pos);
    int g = static_cast<int>(c1.Green() * (1.0 - local_pos) + c2.Green() * local_pos);
    int b = static_cast<int>(c1.Blue() * (1.0 - local_pos) + c2.Blue() * local_pos);

    return wxColour(r, g, b);
}

namespace {

bool IsVeryLightRgb(const wxColour& color)
{
    return color.Red() > 224 && color.Green() > 224 && color.Blue() > 224;
}

wxColour ValidOrFallback(const wxColour& color, const wxColour& fallback = wxColour("#808080"))
{
    return color.IsOk() ? color : fallback;
}

wxColour PhysicalColorAt(const std::vector<std::string>& physical_colors, unsigned filament_id)
{
    if (filament_id == 0 || filament_id > physical_colors.size())
        return wxColour("#26A69A");

    return parse_mixed_color(physical_colors[filament_id - 1]);
}

ColorBlockParams::Mode EffectiveMode(const ColorBlockParams& params)
{
    if (params.mode == ColorBlockParams::Mode::Gradient && params.colors.size() >= 2)
        return ColorBlockParams::Mode::Gradient;
    if (params.mode == ColorBlockParams::Mode::Segment && params.colors.size() >= 2)
        return ColorBlockParams::Mode::Segment;
    return ColorBlockParams::Mode::Solid;
}

wxColour EffectiveSolidColor(const ColorBlockParams& params)
{
    if (!params.colors.empty())
        return ValidOrFallback(params.colors.front());
    return ValidOrFallback(params.solid_color);
}

std::vector<wxColour> EffectiveColorList(const ColorBlockParams& params)
{
    const ColorBlockParams::Mode mode = EffectiveMode(params);
    if (mode == ColorBlockParams::Mode::Solid)
        return {EffectiveSolidColor(params)};

    std::vector<wxColour> colors;
    colors.reserve(params.colors.size());
    for (const wxColour& color : params.colors)
        colors.emplace_back(ValidOrFallback(color));
    return colors;
}

double ColorBlockTextLuminance(const ColorBlockParams& params)
{
    const std::vector<wxColour> colors = EffectiveColorList(params);
    double luminance = 0.0;
    for (const wxColour& color : colors)
        luminance += color.GetLuminance();
    return colors.empty() ? 1.0 : luminance / static_cast<double>(colors.size());
}

bool ColorBlockNeedsBorder(const ColorBlockParams& params)
{
    const ColorBlockParams::Mode mode = EffectiveMode(params);
    const std::vector<wxColour> colors = EffectiveColorList(params);

    if (mode == ColorBlockParams::Mode::Segment) {
        for (const wxColour& color : colors)
            if (IsVeryLightRgb(color))
                return true;
        return false;
    }

    for (const wxColour& color : colors) {
        if (!IsVeryLightRgb(color))
            return false;
    }
    return !colors.empty();
}

std::string ColorBlockCacheKey(const ColorBlockParams& params, const wxColour& lightBorderColor)
{
    std::ostringstream key;
    key << "color-block:mode=" << static_cast<int>(EffectiveMode(params))
        << ":dir=" << static_cast<int>(params.gradient_direction)
        << ":h" << params.height
        << ":w" << params.width
        << ":label=" << params.label.ToStdString();

    if (EffectiveMode(params) == ColorBlockParams::Mode::Solid) {
        key << ':' << EffectiveSolidColor(params).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    } else {
        for (const wxColour& color : EffectiveColorList(params))
            key << ':' << color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    }

    if (lightBorderColor.IsOk())
        key << ":border=" << lightBorderColor.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();

    return key.str();
}

void DrawGradientBlock(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colors, ColorBlockParams::GradientDirection direction)
{
    if (direction == ColorBlockParams::LeftToRight)
    {
        const int segment_count = static_cast<int>(colors.size()) - 1;
        int left = rect.GetLeft();
        for (int index = 0; index < segment_count; ++index)
        {
            const int right = index == segment_count - 1 ? rect.GetRight() + 1 : rect.GetLeft() + rect.GetWidth() * (index + 1) / segment_count;
            const int width = right - left;
            if (width > 0)
                dc.GradientFillLinear( wxRect(left, rect.GetTop(), width, rect.GetHeight()), colors[index], colors[index + 1], wxEAST);
            left = right;
        }
        return;
    }

    for (int y = rect.GetBottom(); y >= rect.GetTop(); --y)
    {
        const double pos = static_cast<double>(rect.GetBottom() - y) /
            static_cast<double>(std::max(1, rect.GetHeight() - 1));
        dc.SetPen(wxPen(interpolate_color(colors, pos)));
        dc.DrawLine(rect.GetLeft(), y, rect.GetRight() + 1, y);
    }
}

void DrawSegmentsBlock(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colors)
{
    const int count = static_cast<int>(colors.size());
    int left = rect.GetLeft();
    for (int index = 0; index < count; ++index)
    {
        const int right = index == count - 1
            ? rect.GetRight() + 1
            : rect.GetLeft() + rect.GetWidth() * (index + 1) / count;
        const int width = right - left;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(colors[static_cast<size_t>(index)]));
        if (width > 0)
            dc.DrawRectangle(left, rect.GetTop(), width, rect.GetHeight());
        left = right;
    }
}

void DrawColorBlock(wxDC& dc, const wxRect& rect, const ColorBlockParams& params,
                      const wxColour& lightBorderColor = wxNullColour)
{
    const ColorBlockParams::Mode mode = EffectiveMode(params);
    const std::vector<wxColour> colors = EffectiveColorList(params);

    dc.SetPen(*wxTRANSPARENT_PEN);
    if (mode == ColorBlockParams::Mode::Gradient) {
        DrawGradientBlock(dc, rect, colors, params.gradient_direction);
    } else if (mode == ColorBlockParams::Mode::Segment) {
        DrawSegmentsBlock(dc, rect, colors);
    } else {
        dc.SetBrush(wxBrush(colors.front()));
        dc.DrawRectangle(rect);
    }

    if (ColorBlockNeedsBorder(params))
    {
        if (lightBorderColor.IsOk())
        {
            dc.SetPen(wxPen(lightBorderColor, 1));
        }
        else
        {
            dc.SetPen(*wxGREY_PEN);
        }
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

    if (!params.label.empty()) {
        dc.SetTextForeground(ColorBlockTextLuminance(params) < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawLabel(params.label, rect, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }
}

} // namespace

MixedFilamentBadge::MixedFilamentBadge(wxWindow* parent, wxWindowID id, int virtual_id, const MixedFilament& mf,
                                       const MixedFilamentDisplayContext& display_context, bool show_number, int badge_size)
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

    m_color_block.width = badge_size;
    m_color_block.height = badge_size;
    m_color_block.label = m_label;
    m_color_block.gradient_direction = ColorBlockParams::BottomToTop;
    m_color_block.mode = ColorBlockParams::Mode::Solid;
    m_color_block.solid_color = parse_mixed_color(mf.display_color);

    if (is_simple_gradient(mf)) {
        const wxColour ca = PhysicalColorAt(display_context.physical_colors, mf.component_a);
        const wxColour cb = PhysicalColorAt(display_context.physical_colors, mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;
        m_color_block.mode = ColorBlockParams::Mode::Gradient;
        m_color_block.colors.push_back(a_to_b ? ca : cb);
        m_color_block.colors.push_back(a_to_b ? cb : ca);
    }

    SetForegroundColour(ColorBlockTextLuminance(m_color_block) < 0.51 ? *wxWHITE : *wxBLACK);
}

void MixedFilamentBadge::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    dc.SetFont(GetFont());

    ColorBlockParams params = m_color_block;
    params.label = m_show_number ? m_label : wxString();
    DrawColorBlock(dc, GetClientRect(), params);
}

void MixedFilamentBadge::on_left_up(wxMouseEvent&)
{
    wxCommandEvent evt(wxEVT_BUTTON, GetId());
    evt.SetEventObject(this);
    ProcessWindowEvent(evt);
}

// ---------------------------------------------------------------------------
// get_color_block_bitmap_cached - unified cached bitmap factory
// ---------------------------------------------------------------------------

wxBitmap* get_color_block_bitmap_cached(const ColorBlockParams& params, const wxColour& lightBorderColor)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    ColorBlockParams draw_params = params;
    draw_params.width = std::max(1, params.width);
    draw_params.height = std::max(1, params.height);

    const std::string key = ColorBlockCacheKey(draw_params, lightBorderColor);

    wxBitmap* cached = cache.find(key);
    if (cached != nullptr)
        return cached;

    wxBitmap bmp(draw_params.width, draw_params.height);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    const bool use_small_font = std::min(draw_params.width, draw_params.height) < 20;
    dc.SetFont(use_small_font ? ::Label::Body_8 : ::Label::Body_12);

    DrawColorBlock(dc, wxRect(0, 0, draw_params.width, draw_params.height), draw_params, lightBorderColor);
    dc.SelectObject(wxNullBitmap);

    return cache.insert(key, bmp);
}

// ---------------------------------------------------------------------------
// Free function - shared between badge and merge menus
// ---------------------------------------------------------------------------

wxBitmap* create_mixed_filament_menu_bitmap(const MixedFilament& mf, const MixedFilamentDisplayContext& ctx,
                                           int width, int height, const wxString& label)
{
    ColorBlockParams params;
    params.width = width;
    params.height = height;
    params.label = label;

    const bool is_gradient = is_simple_gradient(mf);

    if (is_gradient) {
        const wxColour ca = PhysicalColorAt(ctx.physical_colors, mf.component_a);
        const wxColour cb = PhysicalColorAt(ctx.physical_colors, mf.component_b);
        const bool a_to_b = mf.gradient_start >= mf.gradient_end;

        params.mode = ColorBlockParams::Mode::Gradient;
        params.gradient_direction = ColorBlockParams::BottomToTop;
        params.colors.push_back(a_to_b ? ca : cb);
        params.colors.push_back(a_to_b ? cb : ca);
    } else {
        params.mode = ColorBlockParams::Mode::Solid;
        params.solid_color = parse_mixed_color(mf.display_color.empty() ? "#808080" : mf.display_color);
    }

    return get_color_block_bitmap_cached(params);
}

}} // namespace Slic3r::GUI
