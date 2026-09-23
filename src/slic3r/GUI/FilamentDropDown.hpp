#pragma once

// Fork of Widgets/DropDown (e700c93d81) + BambuStudio DropDown grouping (77b9dd94d); Widgets/* untouched.

#include <boost/date_time/posix_time/posix_time.hpp>
#include <wx/stattext.h>

#include <cstddef>
#include <limits>
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
        void *   data{nullptr};
        wxString group{};
        wxString tip{};
        int      style{0};
    };

private:
    std::vector<Item> items;
    size_t             count      = 0;
    wxString           group;
    bool               need_sync  = false;
    int                selection  = -1;
    int                hover_item = -1;

    FilamentDropDown * subDropDown{nullptr};
    FilamentDropDown * mainDropDown{nullptr};

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

    /** @brief Returns the selected row text or an empty string. */
    wxString GetValue() const;
    /** @brief Selects the row whose text equals @p value. */
    void     SetValue(const wxString &value);

public:
    /** @brief Updates the popup background corner radius. */
    void SetCornerRadius(double radius);

    /** @brief Updates the popup border colors. */
    void SetBorderColor(StateColor const &color);

    /** @brief Updates the selected-row border colors. */
    void SetSelectorBorderColor(StateColor const &color);

    /** @brief Updates the row text colors. */
    void SetTextColor(StateColor const &color);

    /** @brief Updates the selected-row background colors. */
    void SetSelectorBackgroundColor(StateColor const &color);

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

    int GetVisibleCount() const
    {
        return count > static_cast<size_t>(std::numeric_limits<int>::max()) ? std::numeric_limits<int>::max()
                                                                              : static_cast<int>(count);
    }

    const std::vector<Item> &GetItems() const { return items; }

protected:
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
    void setGroup(const wxString &value);
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
