#ifndef slic3r_GUI_Utils_hpp_
#define slic3r_GUI_Utils_hpp_

#include "widgets/DpiAware.hpp"
#include <memory>
#include <string>
#include <ostream>
#include <functional>

#include <boost/optional.hpp>

#include <wx/frame.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/panel.h>
#include <wx/dcclient.h>
#include <wx/debug.h>
#include <wx/settings.h>
#include <wx/dataview.h>
#include <wx/statbox.h>

#include <chrono>
#include "Event.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Color.hpp"


class wxCheckBox;
class wxTopLevelWindow;
class wxRect;

#define wxVERSION_EQUAL_OR_GREATER_THAN(major, minor, release) ((wxMAJOR_VERSION > major) || ((wxMAJOR_VERSION == major) && (wxMINOR_VERSION > minor)) || ((wxMAJOR_VERSION == major) && (wxMINOR_VERSION == minor) && (wxRELEASE_NUMBER >= release)))

namespace Slic3r {
namespace GUI {

inline int hex_to_int(const char c)
{
    return (c >= '0' && c <= '9') ? int(c - '0') : (c >= 'A' && c <= 'F') ? int(c - 'A') + 10 : (c >= 'a' && c <= 'f') ? int(c - 'a') + 10 : -1;
}

static ColorRGBA decode_color_to_float_array(const std::string color)
{
    ColorRGBA ret = ColorRGBA::BLACK();
    decode_color(color, ret);
    return ret;
}

extern CopyFileResult copy_file_gui(const std::string &from, const std::string &to, std::string& error_message, const bool with_check = false);

#ifdef _WIN32
// USB HID attach / detach events from Windows OS.
using HIDDeviceAttachedEvent = Event<std::string>;
using HIDDeviceDetachedEvent = Event<std::string>;
wxDECLARE_EVENT(EVT_HID_DEVICE_ATTACHED, HIDDeviceAttachedEvent);
wxDECLARE_EVENT(EVT_HID_DEVICE_DETACHED, HIDDeviceDetachedEvent);

// Disk aka Volume attach / detach events from Windows OS.
using VolumeAttachedEvent = SimpleEvent;
using VolumeDetachedEvent = SimpleEvent;
wxDECLARE_EVENT(EVT_VOLUME_ATTACHED, VolumeAttachedEvent);
wxDECLARE_EVENT(EVT_VOLUME_DETACHED, VolumeDetachedEvent);
#endif /* _WIN32 */

wxTopLevelWindow* find_toplevel_parent(wxWindow *window);

void on_window_geometry(wxTopLevelWindow *tlw, std::function<void()> callback);


int get_dpi_for_window(const wxWindow *window);
wxFont get_default_font_for_dpi(const wxWindow* window, int dpi);
inline wxFont get_default_font(const wxWindow* window) { return get_default_font_for_dpi(window, get_dpi_for_window(window)); }

bool check_dark_mode();
void update_dark_config();
#ifdef _WIN32
void update_dark_ui(wxWindow* window);
#endif


class EventGuard
{
    // This is a RAII-style smart-ptr-like guard that will bind any event to any event handler
    // and unbind it as soon as it goes out of scope or unbind() is called.
    // This can be used to solve the annoying problem of wx events being delivered to freed objects.

private:
    // This is a way to type-erase both the event type as well as the handler:

    struct EventStorageBase {
        virtual ~EventStorageBase() {}
    };

    template<class EvTag, class Fun>
    struct EventStorageFun : EventStorageBase {
        wxEvtHandler *emitter;
        EvTag tag;
        Fun fun;

        EventStorageFun(wxEvtHandler *emitter, const EvTag &tag, Fun fun)
            : emitter(emitter)
            , tag(tag)
            , fun(std::move(fun))
        {
            emitter->Bind(this->tag, this->fun);
        }

        virtual ~EventStorageFun() { emitter->Unbind(tag, fun); }
    };

    template<typename EvTag, typename Class, typename EvArg, typename EvHandler>
    struct EventStorageMethod : EventStorageBase {
        typedef void(Class::* MethodPtr)(EvArg &);

        wxEvtHandler *emitter;
        EvTag tag;
        MethodPtr method;
        EvHandler *handler;

        EventStorageMethod(wxEvtHandler *emitter, const EvTag &tag, MethodPtr method, EvHandler *handler)
            : emitter(emitter)
            , tag(tag)
            , method(method)
            , handler(handler)
        {
            emitter->Bind(tag, method, handler);
        }

        virtual ~EventStorageMethod() { emitter->Unbind(tag, method, handler); }
    };

    std::unique_ptr<EventStorageBase> event_storage;
public:
    EventGuard() {}
    EventGuard(const EventGuard&) = delete;
    EventGuard(EventGuard &&other) : event_storage(std::move(other.event_storage)) {}

    template<class EvTag, class Fun>
    EventGuard(wxEvtHandler *emitter, const EvTag &tag, Fun fun)
        :event_storage(new EventStorageFun<EvTag, Fun>(emitter, tag, std::move(fun)))
    {}

    template<typename EvTag, typename Class, typename EvArg, typename EvHandler>
    EventGuard(wxEvtHandler *emitter, const EvTag &tag, void(Class::* method)(EvArg &), EvHandler *handler)
        :event_storage(new EventStorageMethod<EvTag, Class, EvArg, EvHandler>(emitter, tag, method, handler))
    {}

    EventGuard& operator=(const EventGuard&) = delete;
    EventGuard& operator=(EventGuard &&other)
    {
        event_storage = std::move(other.event_storage);
        return *this;
    }

    void unbind() { event_storage.reset(nullptr); }
    explicit operator bool() const { return !!event_storage; }
};


class CheckboxFileDialog : public wxFileDialog
{
public:
    CheckboxFileDialog(wxWindow *parent,
        const wxString &checkbox_label,
        bool checkbox_value,
        const wxString &message = wxFileSelectorPromptStr,
        const wxString &default_dir = wxEmptyString,
        const wxString &default_file = wxEmptyString,
        const wxString &wildcard = wxFileSelectorDefaultWildcardStr,
        long style = wxFD_DEFAULT_STYLE,
        const wxPoint &pos = wxDefaultPosition,
        const wxSize &size = wxDefaultSize,
        const wxString &name = wxFileDialogNameStr
    );

    bool get_checkbox_value() const;

private:
    struct ExtraPanel : public wxPanel
    {
        wxCheckBox *cbox;

        ExtraPanel(wxWindow *parent);
        static wxWindow* ctor(wxWindow *parent);
    };

    wxString checkbox_label;
};


class WindowMetrics
{
private:
    wxRect rect;
    bool maximized;

    WindowMetrics() : maximized(false) {}
public:
    static WindowMetrics from_window(wxTopLevelWindow *window);
    static boost::optional<WindowMetrics> deserialize(const std::string &str);

    const wxRect& get_rect() const { return rect; }
    bool get_maximized() const { return maximized; }

    void sanitize_for_display(const wxRect &screen_rect);
    void center_for_display(const wxRect &screen_rect);
    std::string serialize() const;
};

std::ostream& operator<<(std::ostream &os, const WindowMetrics& metrics);

inline int hex_digit_to_int(const char c)
{
    return
        (c >= '0' && c <= '9') ? int(c - '0') :
        (c >= 'A' && c <= 'F') ? int(c - 'A') + 10 :
        (c >= 'a' && c <= 'f') ? int(c - 'a') + 10 : -1;
}

class TaskTimer
{
    std::chrono::milliseconds   start_timer;
    std::string                 task_name;
public:
    TaskTimer(std::string task_name);

    ~TaskTimer();
};

class KeyAutoRepeatFilter
{
    size_t m_count{ 0 };

public:
    void increase_count() { ++m_count; }
    void reset_count() { m_count = 0; }
    bool is_first() const { return m_count == 0; }
};


/* Image Generator */
#define _3MF_COVER_SIZE                  wxSize(240, 240)
#define PRINTER_THUMBNAIL_SMALL_SIZE     wxSize(252, 188)
#define PRINTER_THUMBNAIL_MIDDLE_SIZE    wxSize(680, 680)
#define GERNERATE_IMAGE_RESIZE           0
#define GERNERATE_IMAGE_CROP_VERTICAL    1

bool load_image(const std::string& filename, wxImage &image);
bool generate_image(const std::string &filename, wxImage &image, wxSize img_size, int method = GERNERATE_IMAGE_RESIZE);
int get_dpi_for_window(const wxWindow *window);

#ifdef __WXOSX__
void dataview_remove_insets(wxDataViewCtrl* dv);
#endif

#if defined(__WXOSX__) || defined(__linux__)
bool is_debugger_present();
#endif

/// <summary>
/// Make sure the given window fits inside current display
/// </summary>
void fit_in_display(wxTopLevelWindow& window, wxSize desired_size);


}}

#endif
