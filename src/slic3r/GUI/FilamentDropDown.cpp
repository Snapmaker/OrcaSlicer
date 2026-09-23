#include "FilamentDropDown.hpp"
#include "Widgets/Label.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>

#include <boost/log/trivial.hpp>

#include <wx/display.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/weakref.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#ifdef __WIN32__
#include <wx/msw/private.h>
#endif

namespace
{

size_t max_visible_row_count(int max_visible_rows)
{
    return max_visible_rows > 0 ? static_cast<size_t>(max_visible_rows) : size_t{1};
}

int multiply_to_int(int value, size_t multiplier)
{
    if (value <= 0 || multiplier == 0)
        return 0;

    const size_t positive_value = static_cast<size_t>(value);
    const size_t max_int        = static_cast<size_t>(std::numeric_limits<int>::max());
    if (positive_value > max_int / multiplier)
        return std::numeric_limits<int>::max();
    return static_cast<int>(positive_value * multiplier);
}

int add_to_int(int left, int right)
{
    if (right > 0 && left > std::numeric_limits<int>::max() - right)
        return std::numeric_limits<int>::max();
    if (right < 0 && left < std::numeric_limits<int>::min() - right)
        return std::numeric_limits<int>::min();
    return left + right;
}

} // namespace

FilamentDropDown::FilamentDropDown(std::vector<Item> &items)
    : items(items)
    , state_handler(this)
    , text_color(0x363636)
    , border_color(0xDBDBDB)
    , selector_border_color(std::make_pair(0x009688, static_cast<int>(StateColor::Hovered)),
                            std::make_pair(*wxWHITE, static_cast<int>(StateColor::Normal)))
    , selector_background_color(std::make_pair(0xBFE1DE, static_cast<int>(StateColor::Checked)),
                                std::make_pair(*wxWHITE, static_cast<int>(StateColor::Normal)))
{
}

bool FilamentDropDown::Create(wxWindow *parent, long style)
{
    if (parent == nullptr || !PopupWindow::Create(parent, wxPU_CONTAINS_CONTROLS))
        return false;

    Bind(wxEVT_LEFT_DOWN, &FilamentDropDown::mouseDown, this);
    Bind(wxEVT_LEFT_UP, &FilamentDropDown::mouseReleased, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &FilamentDropDown::mouseCaptureLost, this);
    Bind(wxEVT_MOTION, &FilamentDropDown::mouseMove, this);
    Bind(wxEVT_MOUSEWHEEL, &FilamentDropDown::mouseWheelMoved, this);
    Bind(wxEVT_PAINT, &FilamentDropDown::paintEvent, this);

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    state_handler.attach({&border_color, &text_color, &selector_border_color, &selector_background_color});
    state_handler.update_binds();
    if ((style & DD_NO_CHECK_ICON) == 0)
        check_bitmap = ScalableBitmap(this, "checked", 16);
    arrow_bitmap = ScalableBitmap(this, "hms_arrow", 16);
    text_off     = style & DD_NO_TEXT;

    // BBS set default font
    SetFont(Label::Body_14);
#ifdef __WXOSX__
    // PopupWindow releases mouse on idle, which may cause various problems,
    //  such as losting mouse move, and dismissing soon on first LEFT_DOWN event.
    Bind(wxEVT_IDLE, [](wxIdleEvent &evt) {});
#endif

    return true;
}

void FilamentDropDown::Invalidate(bool clear)
{
    if (clear)
    {
        selection = hover_item = -1;
        offset = wxPoint();
        SetToolTip(wxString());
    }
    if (selection >= 0 && static_cast<size_t>(selection) >= items.size())
        selection = -1;
    need_sync = true;
}

void FilamentDropDown::SetItems(const std::vector<Item> &new_items)
{
    items = new_items;
    if (use_flat_fallback)
        apply_flat_fallback();
    Invalidate(true);
    if (subDropDown != nullptr)
    {
        subDropDown->setGroup(wxString());
        subDropDown->SetItems(new_items);
    }
}

void FilamentDropDown::SetSelection(int n)
{
    if (n < 0 || static_cast<size_t>(n) >= items.size())
        n = -1;
    if (selection == n) return;
    selection = n;
    if (need_sync) // for icon Size
    {
        messureSize();
    }
    if (subDropDown)
        subDropDown->SetSelection(n);
    paintNow();
}

wxString FilamentDropDown::GetValue() const
{
    return selection >= 0 && static_cast<size_t>(selection) < items.size() ? items[selection].text : wxString();
}

void FilamentDropDown::SetValue(const wxString &value)
{
    auto i = std::find_if(items.begin(), items.end(), [&value](const Item &item)
                          {
                              return item.text == value;
                          });
    if (i == items.end())
    {
        SetSelection(-1);
        return;
    }

    const std::ptrdiff_t distance = std::distance(items.begin(), i);
    if (distance < 0 || static_cast<size_t>(distance) > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        SetSelection(-1);
        return;
    }
    SetSelection(static_cast<int>(distance));
}

void FilamentDropDown::SetCornerRadius(double radius)
{
    this->radius = radius;
    paintNow();
}

void FilamentDropDown::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    paintNow();
}

void FilamentDropDown::SetSelectorBorderColor(StateColor const &color)
{
    selector_border_color = color;
    state_handler.update_binds();
    paintNow();
}

void FilamentDropDown::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
    paintNow();
}

void FilamentDropDown::SetSelectorBackgroundColor(StateColor const &color)
{
    selector_background_color = color;
    state_handler.update_binds();
    paintNow();
}

void FilamentDropDown::SetUseContentWidth(bool use, bool limit_max_content_width)
{
    if (use_content_width == use)
        return;
    use_content_width = use;
    this->limit_max_content_width = limit_max_content_width;
    need_sync = true;
    messureSize();
}

void FilamentDropDown::SetAlignIcon(bool align)
{
    align_icon = align;
}

void FilamentDropDown::Rescale()
{
    need_sync = true;
    if (subDropDown != nullptr)
        subDropDown->Rescale();
}

bool FilamentDropDown::HasDismissLongTime()
{
    auto now = boost::posix_time::microsec_clock::universal_time();
    return !IsShown() && (now - dismissTime).total_milliseconds() >= 20;
}

void FilamentDropDown::PopupForParent()
{
    prepare_submenu();
    autoPosition();
    Popup(this);
}

void FilamentDropDown::prepare_submenu()
{
    if (mainDropDown != nullptr || use_flat_fallback || subDropDown != nullptr)
        return;

    const bool has_grouped_items = std::any_of(items.begin(), items.end(), [](const Item &item)
                                               {
                                                   return !item.group.IsEmpty();
                                               });
    if (!has_grouped_items)
        return;

    std::unique_ptr<FilamentDropDown> new_sub_drop_down = std::make_unique<FilamentDropDown>(items);
    new_sub_drop_down->mainDropDown            = this;
    new_sub_drop_down->check_bitmap            = check_bitmap;
    new_sub_drop_down->text_off                = text_off;
    new_sub_drop_down->use_content_width       = true;
    new_sub_drop_down->limit_max_content_width = true;
    new_sub_drop_down->max_visible_rows        = 8;
    if (!new_sub_drop_down->Create(GetParent()))
    {
        BOOST_LOG_TRIVIAL(warning)
            << "Could not create the filament submenu; falling back to a flat filament list.";
        apply_flat_fallback();
        return;
    }

    subDropDown = new_sub_drop_down.release();
    wxWeakRef<FilamentDropDown> weak_root(this);
    subDropDown->Bind(wxEVT_COMBOBOX, [weak_root](wxCommandEvent &e)
                      {
                          FilamentDropDown *root = weak_root.get();
                          if (root == nullptr)
                              return;
                          e.SetEventObject(root);
                          e.SetId(root->GetId());
                          root->GetEventHandler()->ProcessEvent(e);
                      });
#ifdef __WXGTK__
    subDropDown->Bind(wxEVT_IDLE, [weak_root](wxIdleEvent &evt)
                      {
                          FilamentDropDown *root = weak_root.get();
                          if (root == nullptr || root->subDropDown == nullptr || !root->subDropDown->IsShown())
                              return;
                          wxPoint mouse_pos = wxGetMousePosition();
                          wxRect  sub_rect  = root->subDropDown->GetScreenRect();
                          if (!sub_rect.Contains(mouse_pos))
                          {
                              wxPoint      local_pt = root->ScreenToClient(mouse_pos);
                              wxMouseEvent mouse_evt(wxEVT_MOTION);
                              mouse_evt.SetX(local_pt.x);
                              mouse_evt.SetY(local_pt.y);
                              wxPostEvent(root, mouse_evt);
                              evt.RequestMore();
                          }
                      });
#endif
}

void FilamentDropDown::apply_flat_fallback()
{
    use_flat_fallback = true;
    for (Item &item : items)
        item.group.clear();

    hover_item = -1;
    offset     = wxPoint();
    SetToolTip(wxString());
    need_sync = true;
}

void FilamentDropDown::DismissAll()
{
    if (subDropDown != nullptr)
    {
        // The child override intentionally keeps the root open while the
        // pointer is over it. An owner-driven close must close both windows.
        subDropDown->PopupWindow::Dismiss();
        subDropDown->Hide();
    }
    PopupWindow::Dismiss();
    Hide();
}

int FilamentDropDown::group_row_of(const wxString &target) const
{
    if (target.IsEmpty())
        return -1;

    std::set<wxString> seen;
    int                row = 0;
    for (const Item &item : items)
    {
        if (!item.group.IsEmpty() && !seen.insert(item.group).second)
            continue;
        if (item.group == target)
            return row;
        ++row;
    }
    return -1;
}

bool FilamentDropDown::openSelectionGroup()
{
    if (!group.IsEmpty() || subDropDown == nullptr || selection < 0 || static_cast<size_t>(selection) >= items.size())
        return false;

    const wxString target = items[selection].group;
    const int      row    = group_row_of(target);
    if (row < 0)
        return false;

    hover_item = row;

    auto &drop = *subDropDown;
    if (drop.group != target)
    {
        drop.setGroup(target);
        drop.messureSize();
    }
    drop.autoPosition();
    drop.paintNow();
    if (!drop.IsShown())
        drop.Popup(&drop);
    return true;
}

void FilamentDropDown::paintEvent(wxPaintEvent &evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxBufferedPaintDC dc(this);
    render(dc);
}

void FilamentDropDown::paintNow()
{
    Refresh();
}

static wxSize GetBmpSize(const wxBitmap &bmp)
{
    if (!bmp.IsOk())
        return wxSize(0, 0);
#ifdef __APPLE__
    return bmp.GetScaledSize();
#else
    return bmp.GetSize();
#endif
}

static wxString strip_group_prefix(const wxString &text, const wxString &group)
{
    if (group.EndsWith(' ')) return text;
    wxString prefix = group.BeforeFirst(' ');
    if (prefix.IsEmpty()) prefix = group;
    if (text.StartsWith(prefix))
        return text.substr(prefix.size()).Trim(false);
    return text;
}

static void _DrawSplitItem(const wxWindow *w,
                           wxDC &dc,
                           wxString split_text,
                           wxPoint start_pt,
                           int item_width,
                           int item_height)
{
    // save dc
    auto pre_clr = dc.GetTextForeground();
    auto pre_pen = dc.GetPen();
    dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(172, 172, 172)));
    dc.SetPen(StateColor::darkModeColorFor(wxColour(166, 169, 170)));
    // miner font
    auto font = w->GetFont();
    font.SetPointSize(font.GetPointSize() - 3);
    dc.SetFont(font);

    int spacing = w->FromDIP(8);

    if (!split_text.empty()) // Paiting: text + spacing + line + spacing
    {
        int    max_content_width = item_width - start_pt.x - 2 * spacing;
        wxSize tSize             = dc.GetMultiLineTextExtent(split_text);
    if (tSize.x > max_content_width)
    {
            split_text = wxControl::Ellipsize(split_text, dc, wxELLIPSIZE_END, max_content_width);
            tSize      = dc.GetMultiLineTextExtent(split_text);
        }

        dc.SetFont(font);
        dc.DrawText(split_text, start_pt);

        int line_width = item_width - start_pt.x - tSize.x - 2 * spacing;
        int line_y     = start_pt.y + (tSize.GetHeight() / 2);
        dc.DrawLine(start_pt.x + tSize.x + spacing,
                    line_y,
                    start_pt.x + tSize.x + line_width + spacing,
                    line_y); // draw right line
    } else // Paiting: line + spacing
    {
        int line_y     = start_pt.y + (item_height / 2);
        int line_width = item_width - start_pt.x - spacing;
        dc.DrawLine(start_pt.x, line_y, start_pt.x + line_width, line_y); // draw line
    }

    // restore dc
    dc.SetTextForeground(pre_clr);
    dc.SetPen(pre_pen);
    dc.SetFont(w->GetFont());
}

void FilamentDropDown::render(wxDC &dc)
{
    if (items.empty())
        return;

    int states = state_handler.states();
    if (subDropDown != nullptr)
        states |= subDropDown->state_handler.states();

    const wxSize size = GetSize();
    render_background(dc, size, states);

    const int selected_item = selectedItem();
    const int hover_index   = hoverIndex();
    wxRect    content        = {{0, offset.y}, rowSize};
    SelectionRenderContext selection_context{size, states, selected_item, hover_index, content};
    render_selection(dc, selection_context);

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const wxSize text_offset = (rowSize - textSize) / 2;
    content.Deflate(0, text_offset.y);
    render_scroll_bar(dc, size, content);

    content.x += 5;
    content.width -= 5;
    if (check_bitmap.bmp().IsOk())
    {
        const wxSize bitmap_size = check_bitmap.GetBmpSize();
        if (selected_item >= 0)
        {
            wxPoint point = content.GetLeftTop();
            point.y += (content.height - bitmap_size.y) / 2;
            point.y = add_to_int(point.y, multiply_to_int(rowSize.y, static_cast<size_t>(selected_item)));
            if (point.y + bitmap_size.y > 0 && point.y < size.y)
                dc.DrawBitmap(check_bitmap.bmp(), point);
        }
        content.x += bitmap_size.x + 5;
        content.width -= bitmap_size.x + 5;
    }

    render_items(dc, size, states, content);
}

void FilamentDropDown::render_background(wxDC &dc, const wxSize &size, int states)
{
    dc.SetPen(wxPen(border_color.colorForStates(states)));
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(GetBackgroundColour())));
    if (radius == 0)
        dc.DrawRectangle(0, 0, size.x, size.y);
    else
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
}

void FilamentDropDown::render_selection(wxDC &dc, SelectionRenderContext &context)
{
    const wxSize &size = context.size;
    if (hover_item >= 0 && (context.states & StateColor::Hovered) &&
        (context.hover_index < 0 ||
         !(items[context.hover_index].style & (DD_ITEM_STYLE_SPLIT_ITEM | DD_ITEM_STYLE_DISABLED))))
    {
        context.content.y = add_to_int(context.content.y, multiply_to_int(rowSize.y, static_cast<size_t>(hover_item)));
        if (context.content.GetBottom() > 0 && context.content.y < size.y)
        {
            if (context.selected_item == hover_item)
                dc.SetBrush(wxBrush(selector_background_color.colorForStates(context.states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_border_color.colorForStates(context.states)));
            context.content.Deflate(4, 1);
            dc.DrawRectangle(context.content);
            context.content.Inflate(4, 1);
        }
        context.content.y = offset.y;
    }
    if (context.selected_item >= 0 &&
        (context.selected_item != hover_item || (context.states & StateColor::Hovered) == 0))
    {
        context.content.y = add_to_int(context.content.y,
                                       multiply_to_int(rowSize.y, static_cast<size_t>(context.selected_item)));
        if (context.content.GetBottom() > 0 && context.content.y < size.y)
        {
            dc.SetBrush(wxBrush(selector_background_color.colorForStates(context.states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_background_color.colorForStates(context.states)));
            context.content.Deflate(4, 1);
            dc.DrawRectangle(context.content);
            context.content.Inflate(4, 1);
        }
        context.content.y = offset.y;
    }
}

void FilamentDropDown::render_scroll_bar(wxDC &dc, const wxSize &size, wxRect &content)
{
    const int total_height = multiply_to_int(rowSize.y, count);
    if (total_height > size.y)
    {
        const int height = total_height;
        const wxRect rect = {size.x - 6, -offset.y * size.y / height, 4, size.y * size.y / height};
        dc.SetPen(wxPen(border_color.defaultColor()));
        dc.SetBrush(wxBrush(*wxLIGHT_GREY));
        dc.DrawRoundedRectangle(rect, 2);
        content.width -= 6;
    }
}

void FilamentDropDown::render_items(wxDC &dc, const wxSize &size, int states, wxRect &rcContent)
{
    std::set<wxString> groups;
    // draw texts & icons
    size_t index = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto &item    = items[i];
        int   states2 = states;
        const bool is_dimmed = (item.style & DD_ITEM_STYLE_DIMMED) != 0;
        if ((item.style & DD_ITEM_STYLE_DISABLED) != 0)
            states2 &= ~StateColor::Enabled;
        // Skip by group
        if (group.IsEmpty())
        {
            if (!item.group.IsEmpty())
            {
                if (groups.find(item.group) != groups.end())
                    continue;
                groups.insert(item.group);
                states2 |= StateColor::Enabled;
            }
        }
        else
        {
            if (item.group != group)
                continue;
        }
        const bool is_hover = hover_item >= 0 && index == static_cast<size_t>(hover_item);
        ++index;
        if (rcContent.GetBottom() < 0)
        {
            rcContent.y += rowSize.y;
            continue;
        }
        if (rcContent.y > size.y) break;
        wxPoint pt = rcContent.GetLeftTop();

        if (item.style & DD_ITEM_STYLE_SPLIT_ITEM)
        {
            _DrawSplitItem(this, dc, item.text, pt, rowSize.GetWidth(), rowSize.GetHeight());
            rcContent.y += rowSize.GetHeight();
            continue;
        }

        const bool is_top_level_group = group.IsEmpty() && !item.group.IsEmpty();
        auto &     icon               = item.icon;
        auto       size2              = GetBmpSize(icon);
        if (iconSize.x > 0)
        {
            if (!is_top_level_group && icon.IsOk())
            {
                pt.y += (rcContent.height - size2.y) / 2;
                dc.DrawBitmap(icon, pt);
            }
            pt.x += iconSize.x + 5;
            pt.y = rcContent.y;
        }
        else if (!is_top_level_group && icon.IsOk())
        {
            pt.y += (rcContent.height - size2.y) / 2;
            dc.DrawBitmap(icon, pt);
            pt.x += size2.x + 5;
            pt.y = rcContent.y;
        }
        // Full-row bitmap (icon height >> text height) already contains the text.
        const bool icon_fills_row = !is_top_level_group && icon.IsOk() && size2.y > textSize.y * 2;
        auto       text           = group.IsEmpty() ? (item.group.IsEmpty() ? item.text : item.group)
                                                    : strip_group_prefix(item.text, group);
        if (!text_off && !text.IsEmpty() && !icon_fills_row)
        {
            wxSize tSize = dc.GetMultiLineTextExtent(text);
            if (pt.x + tSize.x > rcContent.GetRight())
            {
                if (is_hover && item.tip.IsEmpty())
                    SetToolTip(text);
                text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, rcContent.GetRight() - pt.x);
            }
            pt.y += (rcContent.height - textSize.y) / 2;
            dc.SetFont(GetFont());
            dc.SetTextForeground(is_dimmed ? wxColour(0xCE, 0xCE, 0xCE) : text_color.colorForStates(states2));
            dc.DrawText(text, pt);
            if (group.IsEmpty() && !item.group.IsEmpty())
            {
                auto szBmp = arrow_bitmap.GetBmpSize();
                pt.x       = rcContent.GetRight() - szBmp.x - 5;
                pt.y       = rcContent.y + (rcContent.height - szBmp.y) / 2;
                dc.DrawBitmap(arrow_bitmap.bmp(), pt);
            }
        }
        rcContent.y += rowSize.y;
    }
}

int FilamentDropDown::hoverIndex()
{
    if (hover_item < 0)
        return -1;
    if (count == items.size())
    {
        // "count == items.size()" implies no group folded a row, except when a group has
        // exactly one member; only then keep the shortcut, else the header row would be
        // reported as a positive index instead of -i-2.
        bool any_grouped_at_top_level = false;
        if (group.IsEmpty())
        {
            for (const auto &item : items)
                if (!item.group.IsEmpty())
                {
                    any_grouped_at_top_level = true;
                    break;
                }
        }
        if (!any_grouped_at_top_level)
            return hover_item;
    }
    size_t             index = 0;
    std::set<wxString> groups;
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty())
        {
            if (!item.group.IsEmpty())
            {
                if (groups.find(item.group) == groups.end())
                    groups.insert(item.group);
                else
                    continue;
            }
        }
        else
        {
            if (item.group != group)
                continue;
        }
        if (index == static_cast<size_t>(hover_item))
        {
            const size_t max_int = static_cast<size_t>(std::numeric_limits<int>::max());
            if (i > max_int || (!item.group.IsEmpty() && group.IsEmpty() && i > max_int - 2))
                return -1;
            return (item.group.IsEmpty() || !group.IsEmpty()) ? static_cast<int>(i) : -static_cast<int>(i) - 2;
        }
        ++index;
    }
    return -1;
}

int FilamentDropDown::selectedItem()
{
    if (selection < 0 || static_cast<size_t>(selection) >= items.size())
        return -1;
    if (count == items.size())
        return selection;
    auto &sel = items[selection];
    if (group.IsEmpty() ? !sel.group.IsEmpty() : sel.group != group)
        return -1;
    if (selection == 0)
        return 0;
    size_t             index = 0;
    std::set<wxString> groups;
    for (size_t i = 0; i < static_cast<size_t>(selection); ++i)
    {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty())
        {
            if (!item.group.IsEmpty())
            {
                if (groups.find(item.group) == groups.end())
                    groups.insert(item.group);
                else
                    continue;
            }
        }
        else
        {
            if (item.group != group)
                continue;
        }
        ++index;
    }
    return index > static_cast<size_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                         : static_cast<int>(index);
}

void FilamentDropDown::messureSize()
{
    if (!need_sync) return;
    textSize = wxSize();
    iconSize = wxSize();
    count    = 0;
    wxClientDC dc(GetParent() ? GetParent() : this);
    dc.SetFont(GetFont());
    std::set<wxString> groups;
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty())
        {
            if (!item.group.IsEmpty())
            {
                if (groups.find(item.group) == groups.end())
                    groups.insert(item.group);
                else
                    continue;
            }
        }
        else
        {
            if (item.group != group)
                continue;
        }
        ++count;
        wxSize size1;
        if (!text_off)
        {
            auto text = group.IsEmpty() ? (item.group.IsEmpty() ? item.text : item.group)
                                        : strip_group_prefix(item.text, group);
            size1     = dc.GetMultiLineTextExtent(text);
            if (group.IsEmpty() && !item.group.IsEmpty())
                size1.x += 5 + arrow_bitmap.GetBmpWidth();
        }
        const bool is_top_level_group = group.IsEmpty() && !item.group.IsEmpty();
        if (!is_top_level_group && item.icon.IsOk())
        {
            wxSize size2 = GetBmpSize(item.icon);
            if (size2.x > iconSize.x) iconSize = size2;
            if (!align_icon)
            {
                // Full-row bitmap (icon height >> text height): width = bitmap width only.
                if (size2.y > size1.y * 2)
                    size1.x = size2.x;
                else
                    size1.x += size2.x + (text_off ? 0 : 5);
            }
        }
        if (size1.x > textSize.x) textSize = size1;
    }
    if (!align_icon) iconSize.x = 0;
    wxSize szContent = textSize;
    szContent.x += 10;
    if (check_bitmap.bmp().IsOk())
    {
        auto szBmp = check_bitmap.GetBmpSize();
        szContent.x += szBmp.x + 5;
    }
    if (iconSize.x > 0) szContent.x += iconSize.x + (text_off ? 0 : 5);
    if (iconSize.y > szContent.y) szContent.y = iconSize.y;
    szContent.y += 10;
    const size_t max_rows = max_visible_row_count(max_visible_rows);
    if (count > max_rows)
        szContent.x = add_to_int(szContent.x, 6);
    if (GetParent() && group.IsEmpty())
    {
        auto x = GetParent()->GetSize().x;
        if (x > 0 && (!use_content_width || x > szContent.x))
            szContent.x = x;
    }
    rowSize = szContent;
    if (limit_max_content_width)
    {
        wxSize parent_size = GetParent()->GetSize();
        const int max_width = add_to_int(parent_size.x, parent_size.x);
        if (rowSize.x > max_width)
        {
            rowSize.x = max_width;
            szContent = rowSize;
        }
    }
    const size_t visible_rows = std::min(max_rows, std::max(count, size_t{1}));
    szContent.y                = multiply_to_int(szContent.y, visible_rows);
    if (items.size() > max_rows)
        szContent.y = add_to_int(szContent.y, rowSize.y / 2);
    wxWindow::SetSize(szContent);
#ifdef __WXGTK__
    // Gtk has a wrapper window for popup widget
    // Fix for GNOME Platform 48 X11 backend: ensure size is valid before calling gtk_window_resize
    int gtk_width  = szContent.x;
    int gtk_height = szContent.y;
    if (gtk_width <= 0) gtk_width = 100;
    if (gtk_height <= 0) gtk_height = 100;
    gtk_window_resize(GTK_WINDOW(m_widget), gtk_width, gtk_height);
#endif
    need_sync = false;
}

void FilamentDropDown::autoPosition()
{
    messureSize();
    const size_t max_rows = max_visible_row_count(max_visible_rows);
    wxPoint pos;
    wxSize  off;
    if (mainDropDown)
    {
        pos = mainDropDown->ClientToScreen(wxPoint(0, 0));
        off = mainDropDown->GetSize();
        pos.x += 6;
        pos.y = add_to_int(pos.y,
                           add_to_int(multiply_to_int(mainDropDown->rowSize.y,
                                                      static_cast<size_t>(std::max(mainDropDown->hover_item, 0))),
                                    mainDropDown->offset.y));
        off.x -= 12;
        off.y = 0;
    }
    else
    {
        pos   = GetParent()->ClientToScreen(wxPoint(0, -6));
        off   = GetParent()->GetSize();
        off.x = 0;
        off.y += 12;
    }
    wxPoint old  = GetPosition();
    wxSize  size = GetSize();
    Position(pos, off);
    if (old != GetPosition())
    {
        size              = rowSize;
        size.y             = multiply_to_int(rowSize.y, std::min(max_rows, count));
        if (count > max_rows)
            size.y = add_to_int(size.y, rowSize.y / 2);
#ifdef __WXGTK__
        if (size.x < 1) size.x = 1;
        if (size.y < 1) size.y = 1;
#endif
        if (size != GetSize())
        {
            wxWindow::SetSize(size);
            offset = wxPoint();
            Position(pos, off);
        }
    }
    if (GetPosition().y > pos.y)
    {
        // may exceed
        auto drect = wxDisplay(GetParent()).GetGeometry();
        if (GetPosition().y + size.y + 10 > drect.GetBottom())
        {
            if (use_content_width && count <= max_rows)
                size.x = add_to_int(size.x, 6);
            size.y = drect.GetBottom() - GetPosition().y - 10;
#ifdef __WXGTK__
            if (size.y < 1) size.y = 1;
            if (size.x < 1) size.x = 1;
#endif
            wxWindow::SetSize(size);
            if (selection >= 0)
            {
                const size_t selected_row = static_cast<size_t>(selection);
                const int    selected_top = multiply_to_int(rowSize.y, selected_row);
                const int    selected_bottom = multiply_to_int(rowSize.y, selected_row + 1);
                if (add_to_int(offset.y, selected_bottom) > size.y)
                    offset.y = size.y - selected_bottom;
                else if (add_to_int(offset.y, selected_top) < 0)
                    offset.y = -selected_top;
            }
        }
    }
}

void FilamentDropDown::setGroup(const wxString &value)
{
    if (group == value)
        return;

    group       = value;
    hover_item  = -1;
    offset      = wxPoint();
    need_sync   = true;
    SetToolTip(wxString());
}

void FilamentDropDown::mouseDown(wxMouseEvent &event)
{
    // Receivce unexcepted LEFT_DOWN on Mac after OnDismiss
    if (!IsShown())
        return;
    // force calc hover item again
    mouseMove(event);
    pressedDown = true;
    CaptureMouse();
    dragStart = event.GetPosition();
}

void FilamentDropDown::mouseReleased(wxMouseEvent &event)
{
    if (pressedDown)
    {
        dragStart = wxPoint();
        pressedDown = false;
        if (HasCapture())
            ReleaseMouse();
        if (hover_item < 0)
            return;

        // A top-level group header opens (or focuses) the drill-down submenu instead of
        // dismissing, so a narrow one-row group is still reachable by click.
        int idx = hoverIndex();
        if (idx < -1 && subDropDown)
        {
            const wxString &target_group = items[-idx - 2].group;
            auto &          drop         = *subDropDown;
            if (drop.group != target_group)
            {
                drop.setGroup(target_group);
                drop.messureSize();
                drop.autoPosition();
                drop.paintNow();
            }
            if (!drop.IsShown())
                drop.Popup(&drop);
            return;
        }

        if (hover_item >= 0 && (subDropDown == nullptr || subDropDown->group.empty())) // not moved
        {
            sendDropDownEvent();
            if (mainDropDown)
                mainDropDown->hover_item = -1; // To Dismiss mainDropDown
            DismissAndNotify();
        } else if (subDropDown)
            subDropDown->Popup(subDropDown);
    }
}

void FilamentDropDown::mouseCaptureLost(wxMouseCaptureLostEvent &event)
{
    wxMouseEvent evt;
    mouseReleased(evt);
}

void FilamentDropDown::mouseMove(wxMouseEvent &event)
{
    wxPoint pt = event.GetPosition();
#ifdef __WXOSX__
    if (mainDropDown)
    {
        auto size = GetSize();
        if (pt.x < 0 || pt.y < 0 || pt.x >= size.x || pt.y >= size.y)
        {
            auto diff = GetPosition() - mainDropDown->GetPosition();
            event.SetX(pt.x + diff.x);
            event.SetY(pt.y + diff.y);
            mainDropDown->mouseMove(event);
            return;
        }
    }
#endif
    if (pressedDown)
    {
        wxPoint pt2 = offset + pt - dragStart;
        wxSize  size = GetSize();
        dragStart    = pt;
        if (pt2.y > 0)
            pt2.y = 0;
        else
        {
            const int total_height = multiply_to_int(rowSize.y, count);
            if (add_to_int(pt2.y, total_height) < size.y)
                pt2.y = size.y - total_height;
        }
        if (pt2.y != offset.y)
        {
            offset     = pt2;
            hover_item = -1; // moved
        }
        else
        {
            return;
        }
    }
    if (rowSize.y > 0 && (!pressedDown || hover_item >= 0))
    {
        int hover = (pt.y - offset.y) / rowSize.y;
        if (hover < 0 || static_cast<size_t>(hover) >= count)
            hover = -1;
        if (hover == hover_item) return;
        hover_item = hover;
        int index  = hoverIndex();
        if (index < -1 && subDropDown)
        {
            SetToolTip(wxString());
            auto &drop     = *subDropDown;
            drop.setGroup(items[-index - 2].group);
            drop.messureSize();
            drop.autoPosition();
            drop.paintNow();
            if (!drop.IsShown())
                drop.Popup(&drop);
        }
        else if (index >= 0)
        {
            if (subDropDown)
            {
                subDropDown->setGroup(wxString());
                if (subDropDown->IsShown())
                    subDropDown->Dismiss();
            }
            SetToolTip(items[index].tip);
        }
        else
        {
            SetToolTip(wxString());
        }
    }
    paintNow();
}

void FilamentDropDown::mouseWheelMoved(wxMouseEvent &event)
{
    auto    delta = event.GetWheelRotation();
    wxSize  size  = GetSize();
    wxPoint pt2   = offset + wxPoint{0, delta};
    if (pt2.y > 0)
        pt2.y = 0;
    else
    {
        const int total_height = multiply_to_int(rowSize.y, count);
        if (add_to_int(pt2.y, total_height) < size.y)
            pt2.y = size.y - total_height;
    }
    if (pt2.y != offset.y)
    {
        offset = pt2;
    }
    else
    {
        return;
    }
    int hover = (event.GetPosition().y - offset.y) / rowSize.y;
    if (hover < 0 || static_cast<size_t>(hover) >= count)
        hover = -1;
    if (hover != hover_item)
    {
        hover_item = hover;
        const int index = hoverIndex();
        if (index >= 0)
            SetToolTip(items[index].tip);
    }
    paintNow();
}

void FilamentDropDown::sendDropDownEvent()
{
    int index = hoverIndex();
    if (index < 0 || static_cast<size_t>(index) >= items.size() || (items[index].style & DD_ITEM_STYLE_DISABLED))
        return;
    wxCommandEvent event(wxEVT_COMBOBOX, GetId());
    event.SetEventObject(this);
    event.SetInt(index);
    event.SetString(items[index].text);
    GetEventHandler()->ProcessEvent(event);
}

void FilamentDropDown::Dismiss()
{
    if (subDropDown && subDropDown->IsShown())
        return;
    PopupWindow::Dismiss();
}

void FilamentDropDown::OnDismiss()
{
    hover_item = -1;
    SetToolTip(wxString());

    if (mainDropDown)
    {
        const wxPoint &mouse_pos = wxGetMousePosition();
        if (!mainDropDown->GetScreenRect().Contains(mouse_pos))
            mainDropDown->DismissAndNotify();
        else
#ifdef __WIN32__
            SetActiveWindow(mainDropDown->GetHandle());
#else
            ;
#endif
        return;
    }
    if (subDropDown && subDropDown->IsShown())
        return;
    dismissTime = boost::posix_time::microsec_clock::universal_time();
    wxCommandEvent e(EVT_DISMISS);
    GetEventHandler()->ProcessEvent(e);
}
