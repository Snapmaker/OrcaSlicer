#ifndef slic3r_WidgetInspector_hpp_
#define slic3r_WidgetInspector_hpp_

#ifdef ORCA_WIDGET_INSPECTOR

#include <wx/eventfilter.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <memory>
#include <string>

class wxEvent;
class wxPoint;
class wxWindow;

namespace Slic3r {
namespace GUI {

class WidgetHighlightOverlay;
class WidgetInspectorTimer;

// A small in-process alternative to Qt Inspector. Press Ctrl+Shift+F12, move
// the mouse over a widget to see the red highlight, then click it to open its
// AI locator. Press the shortcut again to cancel.
class WidgetInspector final : public wxEventFilter
{
public:
    WidgetInspector();
    ~WidgetInspector() override;

    static void enable();
    static void disable();

    int FilterEvent(wxEvent& event) override;

private:
    friend class WidgetInspectorTimer;

    void set_armed(bool armed);
    void update_highlight();
    void inspect_pointer_position();
    void ensure_overlay(wxWindow& target);
    static std::string build_ai_locator(wxWindow& window, const wxPoint& position);
    static std::string build_debug_report(wxWindow& window, const wxPoint& position);

    bool m_armed {false};
    wxPoint m_last_mouse_position {wxDefaultPosition};
    wxWeakRef<WidgetHighlightOverlay> m_overlay;
    wxWeakRef<wxWindow> m_dialog;
    std::unique_ptr<WidgetInspectorTimer> m_timer;
};

} // namespace GUI
} // namespace Slic3r

#else

namespace Slic3r {
namespace GUI {

// Compile-time no-op unless the dedicated Windows build option is enabled.
class WidgetInspector final
{
public:
    WidgetInspector() = delete;
    static void enable() {}
    static void disable() {}
};

} // namespace GUI
} // namespace Slic3r

#endif // ORCA_WIDGET_INSPECTOR

#endif // slic3r_WidgetInspector_hpp_
