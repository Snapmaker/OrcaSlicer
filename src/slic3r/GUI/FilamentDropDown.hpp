#ifndef slic3r_GUI_FilamentDropDown_hpp_
#define slic3r_GUI_FilamentDropDown_hpp_

// Forked for the filament menu: based on Widgets/DropDown (upstream e700c93d81) plus the
// grouping/two-level drill-down of BambuStudio's DropDown (77b9dd94d). Widgets/DropDown and
// Widgets/ComboBox are intentionally left untouched, so this copy is the single sync point.

#include <boost/date_time/posix_time/posix_time.hpp>
#include <wx/stattext.h>

#include <cstddef>
#include <vector>

#include "wxExtensions.hpp"
#include "Widgets/StateHandler.hpp"
#include "Widgets/PopupWindow.hpp"
#include "Widgets/DropDown.hpp"

#define DD_ITEM_STYLE_SPLIT_ITEM 0x0001
#define DD_ITEM_STYLE_DISABLED   0x0002
#define DD_ITEM_STYLE_DIMMED     0x0004

class FilamentDropDown : public PopupWindow
{
public:
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
    FilamentDropDown(std::vector<Item> &items);

    FilamentDropDown(wxWindow *parent, std::vector<Item> &items, long style = 0);

    void Create(wxWindow *parent, long style = 0);

public:
    void Invalidate(bool clear = false);

    void SetItems(const std::vector<Item> &new_items);

    int GetSelection() const { return selection; }

    void SetSelection(int n);

    wxString GetValue() const;
    void     SetValue(const wxString &value);

public:
    void SetCornerRadius(double radius);

    void SetBorderColor(StateColor const &color);

    void SetSelectorBorderColor(StateColor const &color);

    void SetTextColor(StateColor const &color);

    void SetSelectorBackgroundColor(StateColor const &color);

    void SetUseContentWidth(bool use, bool limit_max_content_width = false);

    void SetAlignIcon(bool align);

public:
    void Rescale();

    bool HasDismissLongTime();

    // The owner controls the grouped root/submenu lifetime.
    void PopupForParent();
    void DismissAll();

    int  GetVisibleCount() const { return static_cast<int>(count); }

    const std::vector<Item> &GetItems() const { return items; }

protected:
    void Dismiss() override;

    void OnDismiss() override;

private:
    void paintEvent(wxPaintEvent &evt);
    void paintNow();

    void render(wxDC &dc);

    int hoverIndex();
    int selectedItem();

    void messureSize();
    void autoPosition();
    void setGroup(const wxString &value);

    // some useful events
    void mouseDown(wxMouseEvent &event);
    void mouseReleased(wxMouseEvent &event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);
    void mouseMove(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);

    void sendDropDownEvent();

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_FilamentDropDown_hpp_
