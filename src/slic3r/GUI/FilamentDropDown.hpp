#pragma once

// Fork of Widgets/DropDown (e700c93d81) + BambuStudio DropDown grouping (77b9dd94d); Widgets/* untouched.

#include <boost/date_time/posix_time/posix_time.hpp>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include <cstddef>
#include <limits>
#include <set>
#include <vector>

#include "wxExtensions.hpp"
#include "Widgets/StateHandler.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/DropDown.hpp"

constexpr int DD_ITEM_STYLE_SPLIT_ITEM = 0x0001;
constexpr int DD_ITEM_STYLE_DISABLED   = 0x0002;
constexpr int DD_ITEM_STYLE_DIMMED     = 0x0004;

/** @brief Draws the grouped, two-level filament preset popup. */
class FilamentDropDown : public PopupWindow
{
public:
    /** @brief Describes one selectable or header row in the popup. */
    struct Item
    {
        wxString text;
        wxBitmap icon;
        wxString group{};
        wxString tip{};
        int      style{0};
    };

    /** @brief Describes one row visible after grouping and filtering. */
    struct VisibleRow
    {
        size_t item_index{0};
        bool   group_header{false};
    };

    /** @brief Builds the visible-row mapping without creating a wx window. */
    static std::vector<VisibleRow> build_visible_rows(const std::vector<Item> &items, const wxString &group)
    {
        std::vector<VisibleRow> rows;
        std::set<wxString> groups;
        rows.reserve(items.size());

        for (size_t index = 0; index < items.size(); ++index)
        {
            const Item &item = items[index];
            if (!group.IsEmpty() && item.group != group)
                continue;

            const bool is_group_header = group.IsEmpty() && !item.group.IsEmpty();
            if (is_group_header && !groups.insert(item.group).second)
                continue;

            rows.push_back({index, is_group_header});
        }

        return rows;
    }

    /** @brief Converts a visible row into an item index or a group-header sentinel. */
    static int item_index_for_visible_row(const std::vector<VisibleRow> &rows, int visible_row)
    {
        if (visible_row < 0 || static_cast<size_t>(visible_row) >= rows.size())
            return -1;

        const VisibleRow &row     = rows[static_cast<size_t>(visible_row)];
        const size_t      max_int = static_cast<size_t>(std::numeric_limits<int>::max());
        if (row.item_index > max_int || (row.group_header && row.item_index > max_int - 2))
            return -1;

        if (row.group_header)
            return -static_cast<int>(row.item_index) - 2;
        return static_cast<int>(row.item_index);
    }

    /** @brief Finds the visible row containing an item, or -1 when it is hidden. */
    static int visible_row_for_item(const std::vector<VisibleRow> &rows, int item_index)
    {
        if (item_index < 0)
            return -1;

        const size_t item_index_value = static_cast<size_t>(item_index);
        for (size_t row_index = 0; row_index < rows.size(); ++row_index)
        {
            if (rows[row_index].item_index != item_index_value)
                continue;
            if (row_index > static_cast<size_t>(std::numeric_limits<int>::max()))
                return -1;
            return static_cast<int>(row_index);
        }
        return -1;
    }

    /** @brief Maps a selected item to its visible row, including a folded group header. */
    static int selected_row_for_item(const std::vector<Item> &items, const wxString &group, int item_index)
    {
        if (item_index < 0 || static_cast<size_t>(item_index) >= items.size())
            return -1;

        const Item &item = items[static_cast<size_t>(item_index)];
        if (!group.IsEmpty() && item.group != group)
            return -1;

        const std::vector<VisibleRow> rows = build_visible_rows(items, group);
        const int visible_row = visible_row_for_item(rows, item_index);
        if (visible_row >= 0)
            return visible_row;
        if (!group.IsEmpty() || item.group.IsEmpty())
            return -1;

        for (size_t row_index = 0; row_index < rows.size(); ++row_index)
        {
            const VisibleRow &row = rows[row_index];
            if (!row.group_header || items[row.item_index].group != item.group)
                continue;
            if (row_index > static_cast<size_t>(std::numeric_limits<int>::max()))
                return -1;
            return static_cast<int>(row_index);
        }
        return -1;
    }

    /** @brief Removes a redundant vendor/group prefix, matching only at a word boundary. */
    static wxString strip_group_prefix(const wxString &text, const wxString &group)
    {
        // Project/User pseudo-groups carry a trailing space and keep their text unchanged.
        if (group.EndsWith(' '))
            return text;

        const wxString candidates[2] = {group, group.BeforeFirst(' ')};
        for (const wxString &prefix : candidates)
        {
            if (prefix.IsEmpty() || !text.StartsWith(prefix))
                continue;
            // A genuine prefix ends the text or is followed by a space; otherwise it matched inside a word
            // (e.g. group "Prusa Polymers" against the text "Prusament PVB @CORE One").
            if (text.length() > prefix.length() && text[prefix.length()] != ' ')
                continue;
            return text.substr(prefix.size()).Trim(false);
        }
        return text;
    }

private:
    std::vector<Item> items;
    size_t             count      = 0;
    wxString           group;
    bool               need_sync  = false;
    int                selection  = -1;
    int                hover_item = -1;

    FilamentDropDown * subDropDown{nullptr};
    FilamentDropDown * mainDropDown{nullptr};
    wxWeakRef<FilamentDropDown> mainDropDownWeak;
    wxTimer                    submenu_motion_timer;

    double radius                  = 0;
    bool   use_content_width       = false;
    bool   limit_max_content_width = false;
    bool   align_icon              = false;
    bool   text_off                = false;
    bool   use_flat_fallback       = false;
    int    max_visible_rows        = 15;

    wxSize textSize;
    wxSize iconSize;
    wxSize rowSize{100, 30};

    StateHandler state_handler;
    StateColor   text_color;
    StateColor   border_color;
    StateColor   selector_border_color;
    StateColor   selector_background_color;
    ScalableBitmap check_bitmap;
    ScalableBitmap arrow_bitmap;

    bool                     pressedDown = false;
    boost::posix_time::ptime dismissTime;
    wxPoint                  offset;
    wxPoint                  dragStart;

public:
    /** @brief Creates an unparented popup that will be initialized by Create(). */
    FilamentDropDown(std::vector<Item> &items);

    ~FilamentDropDown() override;

    /** @brief Initializes the wx popup and returns false when the parent cannot create it. */
    bool Create(wxWindow *parent, long style = 0);

public:
    /** @brief Marks cached geometry stale and optionally clears interaction state. */
    void Invalidate(bool clear = false);

    /** @brief Replaces popup rows and clears state that refers to the previous rows. */
    void SetItems(const std::vector<Item> &new_items);

    int GetSelection() const { return selection; }

    /** @brief Selects a valid row index or clears the selection for an invalid index. */
    void SetSelection(int n);

    /** @brief Chooses whether the popup follows parent width or its row content width. */
    void SetUseContentWidth(bool use, bool limit_max_content_width = false);

    /** @brief Chooses whether rows reserve a common icon column. */
    void SetAlignIcon(bool align);

public:
    /** @brief Invalidates this popup and its submenu geometry after a DPI change. */
    void Rescale();

    /** @brief Reports whether enough time elapsed to reopen after dismissal. */
    bool HasDismissLongTime();

    // The owner controls the grouped root/submenu lifetime.
    /** @brief Opens the root popup at its parent-relative position. */
    void PopupForParent();
    /** @brief Dismisses both root and visible submenu. */
    void DismissAll();

    /** @brief Opens the group that contains the current selection. */
    bool openSelectionGroup();

protected:
    bool ProcessLeftDown(wxMouseEvent &event) override;
    void Dismiss() override;

    void OnDismiss() override;

private:
    void paintEvent(wxPaintEvent &evt);
    void paintNow();

    /** @brief Carries the mutable drawing state for selection highlighting. */
    struct SelectionRenderContext
    {
        const wxSize &size;
        int           states;
        int           selected_item;
        int           hover_index;
        wxRect &      content;
    };

    void render(wxDC &dc);
    void render_background(wxDC &dc, const wxSize &size, int states);
    void render_selection(wxDC &dc, SelectionRenderContext &context);
    void render_scroll_bar(wxDC &dc, const wxSize &size, wxRect &content);
    void render_items(wxDC &dc, const wxSize &size, int states, wxRect &content);

    int hoverIndex();
    int selectedItem();
    int group_row_of(const wxString &target) const;

    void messureSize();
    void autoPosition();
    void ensure_row_visible(int row);
    void setGroup(const wxString &value);
    void show_submenu();
    void on_submenu_motion_timer(wxTimerEvent &event);
    std::vector<VisibleRow> visible_rows() const;
    /** @brief Creates the submenu before showing the root popup when grouped rows are present. */
    void prepare_submenu();
    /** @brief Permanently switches this popup instance to its selectable flat-list fallback. */
    void apply_flat_fallback();

    // some useful events
    void mouseDown(wxMouseEvent &event);
    void mouseReleased(wxMouseEvent &event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);
    void mouseMove(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);

    void sendDropDownEvent();

};
