// Presentation ported from FullSpectrum main (cc152a84dde):
// MixedFilamentBatchDialog, MFDBatchRecommendedAccordion and FilamentCardMixed.
#include "MixedFilamentManageWidgets.hpp"
#include "GUI_App.hpp"
#include "MFDTheme.hpp"
#include "MixedFilamentBadge.hpp"
#include "Widgets/Label.hpp"
#include <wx/dcgraph.h>
#include <wx/scrolwin.h>
#include <algorithm>
#include <memory>
#include <utility>

namespace Slic3r::GUI {
namespace {
void paint_clr_swatch(wxDC& context, const wxSize& size, const wxColor& color, const wxString& text, bool is_dark, int padding)
{
    // Draw the swatch box (optionally inset by padding)
    int x = padding;
    int y = padding;
    int w = size.x - 2 * padding;
    int h = size.y - 2 * padding;

    if (w <= 0 || h <= 0)
        return;

    // Draw the swatch background
    context.SetPen(*wxTRANSPARENT_PEN);
    context.SetBrush(wxBrush(color));
    context.DrawRectangle(x, y, w, h);

    // optional border
    if (is_dark && color.Red() < 45 && color.Green() < 45 && color.Blue() < 45) {
        context.SetPen(wxPen(wxColour(130, 130, 128), 1)); // grey border for very dark colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    } else if (!is_dark && color.Red() > 224 && color.Green() > 224 && color.Blue() > 224) {
        context.SetPen(wxPen(wxColour(207, 207, 207), 1)); // light grey border for very light colors
        context.SetBrush(*wxTRANSPARENT_BRUSH);
        context.DrawRectangle(x, y, w, h);
    }

    // text
    context.SetFont(::Label::Body_12);
    context.SetTextForeground(color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);

    const wxSize  text_size = context.GetTextExtent(text);
    const wxPoint text_baseline_start_pos(x + (w - text_size.x) / 2, y + (h - text_size.y) / 2);
    context.DrawText(text, text_baseline_start_pos.x, text_baseline_start_pos.y);
}

void paint_clr_swatch_gradient(
    wxDC& context, const wxSize& size, const std::vector<wxColor>& colors, const wxString& text, bool is_dark, int padding)
{
    int x = padding;
    int y = padding;
    int w = size.x - 2 * padding;
    int h = size.y - 2 * padding;

    if (w <= 0 || h <= 0)
        return;

    if (colors.empty()) {
        paint_clr_swatch(context, size, wxColour("#26A69A"), text, is_dark, padding);
        return;
    }

    if (colors.size() == 1) {
        paint_clr_swatch(context, size, colors[0], text, is_dark, padding);
        return;
    }

    wxUnusedVar(is_dark);
    ColorBlockParams params;
    params.mode               = ColorBlockParams::Gradient;
    params.gradient_colors    = colors;
    params.width              = w;
    params.height             = h;
    params.label              = text;
    wxBitmap* gradient_bitmap = get_color_block_bitmap_cached(params);
    if (gradient_bitmap != nullptr)
        context.DrawBitmap(*gradient_bitmap, x, y, true);
}

} // namespace

BatchSwatchGrid::BatchSwatchGrid(wxWindow* parent, std::vector<ManageSwatch> swatches, std::function<void(size_t)> on_toggled)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS)
    , m_swatches(std::move(swatches))
    , m_marks(m_swatches.size(), 0)
    , m_on_toggled(std::move(on_toggled))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    rescale();
    Bind(wxEVT_PAINT, &BatchSwatchGrid::on_paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        // Wrap at the actual client width. Sizer negotiation can later revisit
        // a wider provisional width even when this window's size is unchanged.
        if (update_columns(GetClientSize().x)) {
            CallAfter([this] {
                for (auto* parent = GetParent(); parent; parent = parent->GetParent()) {
                    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(parent)) {
                        scroll->Layout();
                        scroll->FitInside();
                        break;
                    }
                }
            });
        }
        event.Skip();
    });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) { update_hover(hit_test(event.GetPosition())); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { update_hover(hit_test(event.GetPosition())); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { update_hover(-1); });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        const int index = hit_test(event.GetPosition());
        if (index >= 0 && m_swatches[index].editable && m_on_toggled) {
            SetFocus();
            m_focused = index;
            update_hover(index);
            m_on_toggled(size_t(index));
        }
    });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
        if (m_focused < 0 && !m_swatches.empty())
            m_focused = 0;
        event.Skip();
    });
    Bind(wxEVT_KEY_DOWN, &BatchSwatchGrid::on_key_down, this);
}

wxSize BatchSwatchGrid::DoGetBestClientSize() const
{
    const int rows = (int(m_swatches.size()) + m_columns - 1) / m_columns;
    return wxSize(m_pitch, rows * m_pitch);
}

bool BatchSwatchGrid::update_columns(int width)
{
    if (width <= 0)
        return false;
    const int columns = std::max(1, width / m_pitch);
    if (columns == m_columns)
        return false;
    m_columns = columns;
    InvalidateBestSize();
    SetMinSize(DoGetBestClientSize());
    update_hover(-1);
    Refresh(false);
    return true;
}

void BatchSwatchGrid::rescale()
{
    m_margin  = FromDIP(2);
    m_pitch   = FromDIP(28) + 2 * m_margin;
    m_columns = std::max(1, GetClientSize().x / m_pitch);
    InvalidateBestSize();
    SetMinSize(DoGetBestClientSize());
    Refresh(false);
}

wxRect BatchSwatchGrid::visible_rect() const
{
    if (!IsShownOnScreen())
        return {};
    wxRect visible = GetClientRect();
    // The grid can be thousands of pixels tall. Clip against all ancestor client
    // areas, including the outer scroller, before allocating a paint buffer.
    for (auto* parent = GetParent(); parent; parent = parent->GetParent()) {
        visible.Intersect(wxRect(ScreenToClient(parent->ClientToScreen(wxPoint(0, 0))), parent->GetClientSize()));
        if (parent->IsTopLevel())
            break;
    }
    return visible;
}

wxRect BatchSwatchGrid::tile_rect(size_t index) const
{
    return wxRect(int(index % m_columns) * m_pitch + m_margin, int(index / m_columns) * m_pitch + m_margin, m_pitch - 2 * m_margin,
                  m_pitch - 2 * m_margin);
}

int BatchSwatchGrid::hit_test(const wxPoint& point) const
{
    if (!GetClientRect().Contains(point))
        return -1;
    const int column = point.x / m_pitch;
    const int index  = (point.y / m_pitch) * m_columns + column;
    return column < m_columns && index < int(m_swatches.size()) && tile_rect(size_t(index)).Contains(point) ? index : -1;
}

void BatchSwatchGrid::refresh_tile(int index)
{
    if (index < 0)
        return;
    const wxRect dirty = tile_rect(size_t(index)).Intersect(visible_rect());
    if (!dirty.IsEmpty())
        RefreshRect(dirty, false);
}

void BatchSwatchGrid::set_mark(size_t index, int mark)
{
    if (m_marks[index] != mark) {
        m_marks[index] = mark;
        refresh_tile(int(index));
    }
}

void BatchSwatchGrid::update_hover(int index)
{
    if (index == m_hovered)
        return;
    const int previous = m_hovered;
    m_hovered          = index;
    refresh_tile(previous);
    refresh_tile(index);
    SetCursor(wxCursor(index >= 0 && m_swatches[index].editable ? wxCURSOR_HAND : wxCURSOR_ARROW));
    if (index >= 0)
        SetToolTip(m_swatches[index].tooltip);
    else
        UnsetToolTip();
}

void BatchSwatchGrid::on_paint(wxPaintEvent&)
{
    wxPaintDC paint(this);
    wxRect    clip = visible_rect().Intersect(GetUpdateRegion().GetBox());
    if (clip.IsEmpty())
        return;
    // Buffer only the damaged, visible rectangle, not the full scrollable grid.
    wxBitmap buffer;
    buffer.CreateScaled(clip.width, clip.height, -1, GetContentScaleFactor());
    wxMemoryDC memory(buffer);
    {
        wxGCDC dc(memory);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const int first = (clip.y / m_pitch) * m_columns;
        const int end   = std::min(int(m_swatches.size()), (clip.GetBottom() / m_pitch + 1) * m_columns);
        for (int index = first; index < end; ++index) {
            const wxRect rect = tile_rect(size_t(index));
            if (!rect.Intersects(clip))
                continue;
            dc.SetDeviceOrigin(rect.x - clip.x, rect.y - clip.y);
            const auto&  swatch  = m_swatches[index];
            const wxSize size    = rect.GetSize();
            const int    padding = index == m_hovered ? 0 : FromDIP(2);
            if (!swatch.gradient.empty())
                paint_clr_swatch_gradient(dc, size, swatch.gradient, swatch.label, wxGetApp().dark_mode(), padding);
            else
                paint_clr_swatch(dc, size, swatch.color, swatch.label, wxGetApp().dark_mode(), padding);
            const wxColour text = swatch.color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE;
            dc.SetPen(wxPen(text, FromDIP(2)));
            if (m_marks[index] < 0)
                dc.DrawLine(size.x - FromDIP(9), FromDIP(4), size.x - FromDIP(4), FromDIP(9));
            else if (m_marks[index] > 0) {
                const int cx = size.x / 2, cy = size.y / 2, r = size.x / 4;
                dc.DrawLine(cx - r, cy, cx + r, cy);
                dc.DrawLine(cx, cy - r, cx, cy + r);
            }
        }
    }
    paint.Blit(clip.x, clip.y, clip.width, clip.height, &memory, 0, 0);
}

void BatchSwatchGrid::focus_tile(int index)
{
    if (m_swatches.empty())
        return;
    m_focused = std::clamp(index, 0, int(m_swatches.size()) - 1);
    update_hover(m_focused);
    // Keep keyboard navigation inside the outer scrolling surface.
    for (auto* parent = GetParent(); parent; parent = parent->GetParent()) {
        auto* scroll = dynamic_cast<wxScrolledWindow*>(parent);
        if (!scroll)
            continue;
        const wxRect  tile     = tile_rect(size_t(m_focused));
        const wxPoint position = scroll->ScreenToClient(ClientToScreen(tile.GetTopLeft()));
        const int     delta    = position.y < 0 ? position.y : std::max(0, position.y + tile.height - scroll->GetClientSize().y);
        int           unit_x = 0, unit_y = 0, start_x = 0, start_y = 0;
        scroll->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        scroll->GetViewStart(&start_x, &start_y);
        if (delta && unit_y > 0)
            scroll->Scroll(start_x, std::max(0, start_y + (delta > 0 ? (delta + unit_y - 1) / unit_y : (delta - unit_y + 1) / unit_y)));
        break;
    }
}

void BatchSwatchGrid::on_key_down(wxKeyEvent& event)
{
    if (m_swatches.empty()) {
        event.Skip();
        return;
    }
    const int current = std::max(0, m_focused);
    switch (event.GetKeyCode()) {
    case WXK_LEFT: focus_tile(current - 1); break;
    case WXK_RIGHT: focus_tile(current + 1); break;
    case WXK_UP: focus_tile(current - m_columns); break;
    case WXK_DOWN: focus_tile(current + m_columns); break;
    case WXK_HOME: focus_tile(0); break;
    case WXK_END: focus_tile(int(m_swatches.size()) - 1); break;
    case WXK_TAB: Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward); break;
    case WXK_SPACE:
    case WXK_RETURN:
        if (m_focused >= 0 && m_swatches[m_focused].editable && m_on_toggled)
            m_on_toggled(size_t(m_focused));
        break;
    default: event.Skip();
    }
}

BatchCheckBox::BatchCheckBox(wxWindow* parent, std::function<void()> on_clicked)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(16), parent->FromDIP(16)), wxBORDER_NONE | wxWANTS_CHARS)
    , m_on_clicked(std::move(on_clicked))
{
    rescale();
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetDoubleBuffered(true);

    Bind(wxEVT_PAINT, &BatchCheckBox::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, &BatchCheckBox::on_enter, this);
    Bind(wxEVT_LEAVE_WINDOW, &BatchCheckBox::on_leave, this);
    Bind(wxEVT_LEFT_UP, &BatchCheckBox::on_left_up, this);
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (IsEnabled() && (event.GetKeyCode() == WXK_SPACE || event.GetKeyCode() == WXK_RETURN)) {
            if (m_on_clicked)
                m_on_clicked();
        } else
            event.Skip();
    });
}

void BatchCheckBox::set_state(int state)
{
    if (m_state != state) {
        m_state = state;
        Refresh();
    }
}

void BatchCheckBox::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC    dc(pdc);
    wxSize    s = GetClientSize();

    dc.SetBackground(wxBrush(GetParent() ? GetParent()->GetBackgroundColour() : StateColor::darkModeColorFor(*wxWHITE)));
    dc.Clear();

    // Checkbox border (adaptive color on dark mode and hover)
    wxColor border_color;
    if (wxGetApp().dark_mode()) {
        border_color = m_hovered ? *wxWHITE : wxColor(80, 80, 80);
    } else {
        border_color = m_hovered ? wxColor(26, 26, 26) : wxColor(204, 204, 204);
    }

    dc.SetPen(wxPen(border_color, FromDIP(1)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, s.x, s.y, FromDIP(2));

    if (m_state == 1) {
        // State 1: Box
        dc.SetBrush(wxBrush(wxColour("#009688")));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(s.x * 0.25, s.y * 0.25, s.x * 0.5, s.y * 0.5, FromDIP(1));
    } else if (m_state == 2) {
        // State 2: Checked
        dc.SetPen(wxPen(wxColour("#009688"), FromDIP(2)));
        dc.DrawLine(s.x * 0.25, s.y * 0.55, s.x * 0.45, s.y * 0.75);
        dc.DrawLine(s.x * 0.45, s.y * 0.75, s.x * 0.75, s.y * 0.35);
    }
}

void BatchCheckBox::on_enter(wxMouseEvent& event)
{
    m_hovered = true;
    Refresh();
    event.Skip();
}

void BatchCheckBox::on_leave(wxMouseEvent& event)
{
    m_hovered = false;
    Refresh();
    event.Skip();
}

void BatchCheckBox::on_left_up(wxMouseEvent& event)
{
    if (IsEnabled() && m_on_clicked) {
        SetFocus();
        m_on_clicked();
    }
    event.Skip();
}

CollapsibleSubSection::CollapsibleSubSection(wxWindow* parent, const wxString& title)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
{
    build_ui(title);
}

void CollapsibleSubSection::build_ui(const wxString& title)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &CollapsibleSubSection::on_paint, this);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(main_sizer);

    // Header Panel
    m_header = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(24)), wxBORDER_NONE);
    m_header->SetBackgroundColour(MFDTheme::card_background());
    m_header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_header->SetCursor(wxCursor(wxCURSOR_HAND));
    m_header->SetDoubleBuffered(true);

    wxBoxSizer* header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_header->SetSizer(header_sizer);

    // Spacer for chevron drawing space
    header_sizer->AddSpacer(FromDIP(20));

    // Title
    m_lbl_title = new wxStaticText(m_header, wxID_ANY, title);
    m_lbl_title->SetFont(::Label::Body_13);
    MFDTheme::apply_text(m_lbl_title, MFDTheme::muted_text(), m_header->GetBackgroundColour());

    // Info Count
    m_lbl_info = new wxStaticText(m_header, wxID_ANY, "");
    m_lbl_info->SetFont(::Label::Body_12);
    MFDTheme::apply_text(m_lbl_info, MFDTheme::secondary_text(), m_header->GetBackgroundColour());

    // Custom Checkbox
    m_check_box = new BatchCheckBox(m_header, [this]() {
        if (on_check_clicked)
            on_check_clicked();
    });

    header_sizer->Add(m_lbl_title, 0, wxALIGN_CENTER_VERTICAL);
    header_sizer->AddStretchSpacer(1);
    header_sizer->Add(m_lbl_info, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    header_sizer->Add(m_check_box, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));

    // Body Panel
    m_body = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_body->SetBackgroundColour(MFDTheme::card_background());

    main_sizer->Add(m_header, 0, wxEXPAND | wxALL, FromDIP(4));
    main_sizer->Add(m_body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(4));

    // Hover highlights tracking
    auto header_hover = std::make_shared<bool>(false);

    auto enter_header = [this, header_hover](wxMouseEvent& e) {
        *header_hover = true;
        m_lbl_title->SetForegroundColour(MFDTheme::primary_text());
        m_header->Refresh();
        e.Skip();
    };
    auto leave_header = [this, header_hover](wxMouseEvent& e) {
        wxPoint pos  = wxGetMousePosition();
        wxRect  rect = m_header->GetScreenRect();
        if (!rect.Contains(pos)) {
            *header_hover = false;
            m_lbl_title->SetForegroundColour(MFDTheme::muted_text());
            m_header->Refresh();
        }
        e.Skip();
    };

    m_header->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_header->Bind(wxEVT_LEAVE_WINDOW, leave_header);
    m_lbl_title->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_lbl_title->Bind(wxEVT_LEAVE_WINDOW, leave_header);
    m_lbl_info->Bind(wxEVT_ENTER_WINDOW, enter_header);
    m_lbl_info->Bind(wxEVT_LEAVE_WINDOW, leave_header);

    m_header->Bind(wxEVT_PAINT, [this, header_hover](wxPaintEvent&) {
        wxPaintDC pdc(m_header);
        wxGCDC    dc(pdc);
        wxSize    s = m_header->GetClientSize();

        dc.SetBackground(wxBrush(MFDTheme::card_background()));
        dc.Clear();

        // Draw antialiased thin chevron arrow
        wxColor chevron_color;
        if (wxGetApp().dark_mode()) {
            chevron_color = *header_hover ? *wxWHITE : wxColour(150, 150, 150);
        } else {
            chevron_color = *header_hover ? wxColour(26, 26, 26) : wxColour(102, 102, 102);
        }
        dc.SetPen(wxPen(chevron_color, FromDIP(1)));
        int cx = FromDIP(10);
        int cy = s.y / 2;
        if (m_collapsed) {
            // Point up (caret shape ^)
            dc.DrawLine(cx - 4, cy + 2, cx, cy - 2);
            dc.DrawLine(cx, cy - 2, cx + 4, cy + 2);
        } else {
            // Point down (V shape)
            dc.DrawLine(cx - 4, cy - 2, cx, cy + 2);
            dc.DrawLine(cx, cy + 2, cx + 4, cy - 2);
        }
    });

    m_header->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);
    m_lbl_title->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);
    m_lbl_info->Bind(wxEVT_LEFT_UP, &CollapsibleSubSection::on_header_click, this);

    m_body->Show(!m_collapsed);
}

void CollapsibleSubSection::on_paint(wxPaintEvent&)
{
    wxPaintDC pdc(this);
    wxGCDC    dc(pdc);
    wxSize    size = GetSize();

    // Draw parent background color first
    wxColour parent_bg = GetParent() ? GetParent()->GetBackgroundColour() : GetBackgroundColour();
    dc.SetBrush(wxBrush(parent_bg));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.x, size.y);

    // Rounded card with slightly darker grey border
    wxColour card_bg = MFDTheme::card_background();
    dc.SetBrush(wxBrush(card_bg));
    dc.SetPen(wxPen(MFDTheme::card_border(), 1));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 4);
}

void CollapsibleSubSection::on_header_click(wxMouseEvent&) { toggle(); }

void CollapsibleSubSection::toggle()
{
    m_collapsed = !m_collapsed;
    m_body->Show(!m_collapsed);
    m_header->Refresh();

    Layout();

    // Bubble layout up directly to the scrolled window
    wxWindow*         p        = GetParent();
    wxScrolledWindow* scrolled = nullptr;
    while (p) {
        if (auto* s = dynamic_cast<wxScrolledWindow*>(p)) {
            scrolled = s;
            break;
        }
        p = p->GetParent();
    }
    if (scrolled) {
        scrolled->Layout();
        scrolled->FitInside();
    } else {
        Layout();
    }

    if (on_toggle)
        on_toggle(m_collapsed);
}

void CollapsibleSubSection::set_check_state(int state)
{
    if (m_check_box)
        m_check_box->set_state(state);
}

int CollapsibleSubSection::get_check_state() const { return m_check_box ? m_check_box->get_state() : 0; }

void CollapsibleSubSection::set_info(int add_count, int del_count)
{
    wxString txt = "";
    if (add_count > 0 || del_count > 0) {
        if (add_count > 0)
            txt += wxString::Format("+%d", add_count);
        if (del_count > 0) {
            if (!txt.IsEmpty())
                txt += " ";
            txt += wxString::Format("-%d", del_count);
        }
    }
    if (m_lbl_info->GetLabel() != txt) {
        m_lbl_info->SetLabel(txt);
        m_header->Layout();
    }
}

void CollapsibleSubSection::set_check_enabled(bool enabled)
{
    if (m_check_box->IsEnabled() == enabled)
        return;
    m_check_box->Enable(enabled);
    m_check_box->SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
}

void CollapsibleSubSection::rescale()
{
    m_header->SetMinSize(wxSize(-1, FromDIP(24)));
    m_check_box->rescale();
    Layout();
}

void BatchCheckBox::rescale()
{
    SetMinSize(FromDIP(wxSize(16, 16)));
    SetSize(GetMinSize());
}

} // namespace Slic3r::GUI
