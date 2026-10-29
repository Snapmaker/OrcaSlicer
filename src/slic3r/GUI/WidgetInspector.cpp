#include "WidgetInspector.hpp"

#ifndef ORCA_WIDGET_INSPECTOR
#error "WidgetInspector requires SLIC3R_WIDGET_INSPECTOR=ON on Windows."
#endif

#include "PlaterFilamentComboBox.hpp"
#include "PresetComboBoxes.hpp"
#include "Widgets/Label.hpp"

#include <wx/aui/auibar.h>
#include <wx/bookctrl.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/popupwin.h>
#include <wx/region.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/window.h>
#include <wx/msw/wrapwin.h>

#include <boost/core/demangle.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace {

std::unique_ptr<WidgetInspector>& inspector_instance()
{
    static std::unique_ptr<WidgetInspector> instance;
    return instance;
}

std::string yaml_quote(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                    out << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(c) << std::dec;
                else
                    out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}

std::string yaml_quote_wx(const wxString& value) { return yaml_quote(value.ToUTF8().data()); }
std::string to_utf8(const wxString& value) { return std::string(value.ToUTF8().data()); }

std::string wx_class_name(const wxWindow& window)
{
    const wxClassInfo* class_info = window.GetClassInfo();
    return class_info == nullptr ? std::string("unknown") : to_utf8(class_info->GetClassName());
}

std::string cpp_class_name(const wxWindow& window) { return boost::core::demangle(typeid(window).name()); }

std::string pointer_text(const void* pointer)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
    return stream.str();
}

std::string rect_text(const wxRect& rect)
{
    std::ostringstream stream;
    stream << "[" << rect.x << ", " << rect.y << ", " << rect.width << ", " << rect.height << "]";
    return stream.str();
}

const PlaterPresetComboBox* neighbor_filament_combo(const wxWindow& window)
{
    if (const auto* combo = dynamic_cast<const PlaterPresetComboBox*>(window.GetNextSibling()); combo != nullptr)
        return combo;
    return dynamic_cast<const PlaterPresetComboBox*>(window.GetPrevSibling());
}
std::string vector_text(double x, double y, double w, double h)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(4) << "[" << x << ", " << y << ", " << w << ", " << h << "]";
    return stream.str();
}

bool copy_text_to_clipboard(const std::string& text)
{
    if (!wxTheClipboard->Open())
        return false;

    const bool copied = wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(text)));
    wxTheClipboard->Close();
    return copied;
}

std::string tooltip_text(const wxWindow& window) { return to_utf8(window.GetToolTipText()); }

std::string value_text(const wxWindow& window)
{
    if (const auto* text = dynamic_cast<const wxTextEntry*>(&window); text != nullptr)
        return to_utf8(text->GetValue());
    if (const auto* combo = dynamic_cast<const wxComboBoxBase*>(&window); combo != nullptr) {
        const int selection = combo->GetSelection();
        return selection == wxNOT_FOUND ? std::string{} : to_utf8(combo->wxItemContainerImmutable::GetStringSelection());
    }
    if (const auto* combo = dynamic_cast<const ::ComboBox*>(&window); combo != nullptr)
        return to_utf8(combo->GetValue());
    return {};
}

std::string target_role(const wxWindow& window)
{
    const std::string class_name = cpp_class_name(window);
    if (dynamic_cast<const PlaterFilamentComboBox*>(&window) != nullptr)
        return "filament_preset_combo";
    if (dynamic_cast<const PlaterPresetComboBox*>(&window) != nullptr)
        return "preset_combo";
    if (dynamic_cast<const wxBookCtrlBase*>(&window) != nullptr)
        return "book_or_tab_control";
    if (dynamic_cast<const wxAuiToolBar*>(&window) != nullptr)
        return "aui_toolbar";
    if (dynamic_cast<const wxScrolledWindow*>(&window) != nullptr)
        return "scrolled_container";
    if (dynamic_cast<const wxTextEntry*>(&window) != nullptr)
        return "text_input";
    if (class_name.find("SideButton") != std::string::npos)
        return "side_button";
    if (class_name.find("ScalableButton") != std::string::npos)
        return "scalable_button";
    if (class_name.find("::Button") != std::string::npos)
        return "custom_button";
    if (dynamic_cast<const wxButton*>(&window) != nullptr)
        return "button";
    if (dynamic_cast<const wxControl*>(&window) != nullptr)
        return "control";
    if (dynamic_cast<const wxPanel*>(&window) != nullptr)
        return "panel";
    return "window";
}
std::string semantic_id(const wxWindow& window)
{
    if (const auto* combo = dynamic_cast<const PlaterPresetComboBox*>(&window); combo != nullptr) {
        const int index = combo->get_filament_idx();
        if (index >= 0)
            return "plater.sidebar.filament_management.row[" + std::to_string(index) + "].preset_combo";
    }
    if (const auto* combo = neighbor_filament_combo(window); combo != nullptr) {
        const int index = combo->get_filament_idx();
        if (index >= 0) {
            if (dynamic_cast<const wxBitmapButton*>(combo->clr_picker) == &window)
                return "plater.sidebar.filament_management.row[" + std::to_string(index) + "].color_picker";
            if (combo->edit_btn != nullptr && combo->edit_btn->GetId() == window.GetId())
                return "plater.sidebar.filament_management.row[" + std::to_string(index) + "].edit_button";
        }
    }
    return "wx.semantic_id.unregistered";
}

struct ChildInfo
{
    int index{-1};
    int same_class_index{-1};
    int child_count{0};
    int same_class_count{0};
};

ChildInfo child_info(const wxWindow& window)
{
    ChildInfo result;
    const wxWindow* parent = window.GetParent();
    if (parent == nullptr)
        return result;

    const std::string class_name = cpp_class_name(window);
    int index = 0;
    int same_class_index = 0;
    for (wxWindowList::compatibility_iterator node = parent->GetChildren().GetFirst(); node != nullptr; node = node->GetNext()) {
        const wxWindow* child = node->GetData();
        if (child == &window)
            result.index = index;
        if (cpp_class_name(*child) == class_name) {
            if (child == &window)
                result.same_class_index = same_class_index;
            ++same_class_index;
        }
        ++index;
    }
    result.child_count = index;
    result.same_class_count = same_class_index;
    return result;
}

std::string window_summary(wxWindow& window, const std::string& indent)
{
    const ChildInfo info = child_info(window);
    std::ostringstream out;
    out << indent << "  cpp_class: " << yaml_quote(cpp_class_name(window)) << "\n"
        << indent << "  wx_class: " << yaml_quote(wx_class_name(window)) << "\n"
        << indent << "  id: " << window.GetId() << "\n"
        << indent << "  name: " << yaml_quote_wx(window.GetName()) << "\n"
        << indent << "  label: " << yaml_quote_wx(window.GetLabel()) << "\n"
        << indent << "  tooltip: " << yaml_quote(tooltip_text(window)) << "\n"
        << indent << "  role: " << yaml_quote(target_role(window)) << "\n"
        << indent << "  value: " << yaml_quote(value_text(window)) << "\n"
        << indent << "  child_index: " << info.index << "\n"
        << indent << "  same_class_index: " << info.same_class_index << "\n"
        << indent << "  same_class_count: " << info.same_class_count << "\n";
    return out.str();
}

void append_navigation(wxWindow& window, std::vector<std::string>& navigation)
{
    for (wxWindow* current = &window; current != nullptr; current = current->GetParent()) {
        if (auto* book = dynamic_cast<wxBookCtrlBase*>(current); book != nullptr) {
            const int selection = book->GetSelection();
            if (selection != wxNOT_FOUND)
                navigation.push_back(to_utf8(book->GetPageText(static_cast<size_t>(selection))));
        } else if (!current->GetLabel().IsEmpty()) {
            navigation.push_back(to_utf8(current->GetLabel()));
        }
    }
    std::reverse(navigation.begin(), navigation.end());
}

void append_geometry(wxWindow& window, std::ostringstream& out)
{
    const wxRect screen_rect = window.GetScreenRect();
    wxWindow* top = wxGetTopLevelParent(&window);
    if (top == nullptr)
        top = &window;
    const wxRect top_rect = top->GetScreenRect();
    const double scale = window.GetContentScaleFactor();

    out << "geometry:\n"
        << "  content_scale: " << scale << "\n"
        << "  screen_rect_px: " << rect_text(screen_rect) << "\n"
        << "  rect_relative_to_top_window_logical: "
        << vector_text((screen_rect.x - top_rect.x) / scale, (screen_rect.y - top_rect.y) / scale, screen_rect.width / scale, screen_rect.height / scale) << "\n";

    if (wxWindow* parent = window.GetParent(); parent != nullptr) {
        const wxRect parent_rect = parent->GetScreenRect();
        out << "  rect_relative_to_parent_logical: "
            << vector_text((screen_rect.x - parent_rect.x) / scale, (screen_rect.y - parent_rect.y) / scale, screen_rect.width / scale, screen_rect.height / scale) << "\n";
        if (parent_rect.width > 0 && parent_rect.height > 0) {
            out << "  normalized_rect_in_parent: "
                << vector_text(double(screen_rect.x - parent_rect.x) / parent_rect.width,
                               double(screen_rect.y - parent_rect.y) / parent_rect.height,
                               double(screen_rect.width) / parent_rect.width,
                               double(screen_rect.height) / parent_rect.height) << "\n";
        }
    }
}

const char* aui_item_kind_name(int kind)
{
    switch (kind) {
    case wxITEM_NORMAL: return "wxITEM_NORMAL";
    case wxITEM_CHECK: return "wxITEM_CHECK";
    case wxITEM_RADIO: return "wxITEM_RADIO";
    case wxITEM_DROPDOWN: return "wxITEM_DROPDOWN";
    case wxITEM_SEPARATOR: return "wxITEM_SEPARATOR";
    default: return "wxITEM_MAX";
    }
}

std::string aui_toolbar_report(wxWindow& window, const wxPoint& position)
{
    auto* toolbar = dynamic_cast<wxAuiToolBar*>(&window);
    if (toolbar == nullptr)
        return {};

    const wxPoint client_position = toolbar->ScreenToClient(position);
    wxAuiToolBarItem* item = toolbar->FindToolByPosition(client_position.x, client_position.y);
    if (item == nullptr)
        return "aui_toolbar:\n  item_found: false\n";

    std::ostringstream out;
    out << "aui_toolbar:\n"
        << "  item_found: true\n"
        << "  item_class: " << yaml_quote(boost::core::demangle(typeid(*item).name())) << "\n"
        << "  item_id: " << item->GetId() << "\n"
        << "  item_kind: " << aui_item_kind_name(item->GetKind()) << "\n"
        << "  item_label: " << yaml_quote_wx(item->GetLabel()) << "\n"
        << "  item_short_help: " << yaml_quote_wx(item->GetShortHelp()) << "\n";
    return out.str();
}

std::string neighborhood_report(wxWindow& window)
{
    std::ostringstream out;
    out << "neighbors:\n";
    bool emitted = false;
    const auto append = [&](const char* relation, wxWindow* neighbor) {
        if (neighbor == nullptr)
            return;
        out << "  " << relation << ":\n" << window_summary(*neighbor, "  ");
        emitted = true;
    };
    append("previous_sibling", window.GetPrevSibling());
    append("next_sibling", window.GetNextSibling());
    if (!emitted)
        out << "  present: false\n";
    return out.str();
}

class WidgetInspectorDialog final : public wxDialog
{
public:
    WidgetInspectorDialog(wxWindow* parent, std::string ai_locator, std::string debug_report)
        : wxDialog(parent, wxID_ANY, "Widget Inspector", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
          , m_ai_locator(std::move(ai_locator))
          , m_debug_report(std::move(debug_report))
    {
        SetMinClientSize(wxSize(860, 520));
        SetClientSize(wxSize(1020, 620));

        auto* task_label = new wxStaticText(this, wxID_ANY, "Agent task");
        task_label->SetFont(Label::Head_12);
        m_action_text = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
        m_action_text->SetFont(Label::Body_12);
        m_action_text->SetHint("Describe what the agent should do with this widget...");

        auto* copy_prompt = new wxButton(this, wxID_ANY, "Generate && Copy Agent Prompt");
        copy_prompt->Bind(wxEVT_BUTTON, [this, copy_prompt](wxCommandEvent&) {
            const bool copied = copy_text_to_clipboard(build_agent_prompt(to_utf8(m_action_text->GetValue())));
            copy_prompt->SetLabel(copied ? "Agent Prompt Copied" : "Copy Failed");
        });

        auto* close = new wxButton(this, wxID_CANCEL, "Close");
        auto* task_button_sizer = new wxBoxSizer(wxHORIZONTAL);
        task_button_sizer->Add(copy_prompt, 1, wxEXPAND);
        task_button_sizer->Add(close, 0, wxLEFT, 8);

        auto* task_sizer = new wxBoxSizer(wxVERTICAL);
        task_sizer->Add(task_label, 0, wxEXPAND | wxBOTTOM, 8);
        task_sizer->Add(m_action_text, 1, wxEXPAND);
        task_sizer->Add(task_button_sizer, 0, wxEXPAND | wxTOP, 8);

        auto* debug_label = new wxStaticText(this, wxID_ANY, "Debug info");
        debug_label->SetFont(Label::Head_12);
        m_debug_text = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(m_debug_report), wxDefaultPosition, wxDefaultSize,
                                          wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxTE_DONTWRAP);
        m_debug_text->SetFont(Label::Body_12);

        auto* copy_debug = new wxButton(this, wxID_ANY, "Copy Debug Info");
        copy_debug->Bind(wxEVT_BUTTON, [this, copy_debug](wxCommandEvent&) {
            const bool copied = copy_text_to_clipboard(m_debug_report);
            copy_debug->SetLabel(copied ? "Copied" : "Copy Failed");
        });

        auto* debug_sizer = new wxBoxSizer(wxVERTICAL);
        debug_sizer->Add(debug_label, 0, wxEXPAND | wxBOTTOM, 8);
        debug_sizer->Add(m_debug_text, 1, wxEXPAND);
        debug_sizer->Add(copy_debug, 0, wxEXPAND | wxTOP, 8);

        auto* root = new wxBoxSizer(wxHORIZONTAL);
        root->Add(task_sizer, 1, wxEXPAND | wxALL, 12);
        root->Add(debug_sizer, 2, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, 12);
        SetSizer(root);

        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Destroy(); });
    }

    void update_reports(std::string ai_locator, std::string debug_report)
    {
        m_ai_locator   = std::move(ai_locator);
        m_debug_report = std::move(debug_report);
        m_debug_text->ChangeValue(wxString::FromUTF8(m_debug_report));
        m_debug_text->SetSelection(0, 0);
        Raise();
    }

private:
    std::string build_agent_prompt(const std::string& action) const
    {
        std::ostringstream out;
        out << "Widget Location and Action Task\n\n"
            << "Workspace/source root: use the current OrcaSlicer workspace/repository root.\n\n"
            << "User action request:\n"
            << (action.empty() ? std::string("- No action provided. Please locate and explain this widget.") : action)
            << "\n\n"
            << "Goal:\n"
            << "1. Locate the selected wxWidgets control in the OrcaSlicer source code.\n"
            << "2. Identify its construction, event bindings, state updates, and relevant business logic.\n"
            << "3. Answer the user action request. Modify files only if the user action explicitly asks for changes.\n\n"
            << "Required output:\n"
            << "- Actual C++ type and semantic meaning\n"
            << "- Construction location as absolute file paths with line numbers\n"
            << "- Event binding locations as absolute file paths with line numbers\n"
            << "- State/read/write locations as absolute file paths with line numbers\n"
            << "- Reasoning and any ambiguity\n\n"
            << "Locator priority, highest first:\n"
            << "1. semantic.semantic_id\n"
            << "2. semantic.target_role\n"
            << "3. identity.cpp_class and business indexes\n"
            << "4. identity.label / tooltip / value\n"
            << "5. neighbors\n"
            << "6. locator.chain\n"
            << "7. geometry\n\n"
            << "Important:\n"
            << "- row[N] indexes are 0-based unless explicitly labeled 1_based.\n"
            << "- geometry is fallback-only and unstable across window move, resize, scroll, layout, language, and DPI changes.\n"
            << "- Do not use object addresses, HWND, or screen coordinates to infer source locations.\n"
            << "- wxClassInfo can be less precise than cpp_class because custom widgets may not register a dynamic class.\n"
            << "- Empty labels such as panel/button are not semantic names.\n\n"
            << "AI Locator:\n"
            << m_ai_locator;
        return out.str();
    }

    std::string m_ai_locator;
    std::string m_debug_report;
    wxTextCtrl* m_action_text {nullptr};
    wxTextCtrl* m_debug_text {nullptr};
};


} // namespace


class WidgetHighlightOverlay final : public wxPopupWindow
{
public:
    explicit WidgetHighlightOverlay(wxWindow* parent) : wxPopupWindow(parent, wxBORDER_NONE | wxFRAME_SHAPED), m_last_rect(wxDefaultPosition, wxDefaultSize)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(255, 45, 32));
        SetSize(4, 4);
        // Keep the highlight visible to the user while making it invisible to
        // Windows hit testing. This prevents the overlay from selecting itself.
        ::SetWindowLongPtr(GetHandle(), GWL_EXSTYLE, ::GetWindowLongPtr(GetHandle(), GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

    void hide()
    {
        Hide();
        m_last_rect = wxRect(wxDefaultPosition, wxDefaultSize);
    }

    void show_around(const wxRect& target_rect)
    {
        if (IsShown() && target_rect == m_last_rect)
            return;

        const int border = std::max(2, FromDIP(2));
        const int width = std::max(4, target_rect.width + border * 2);
        const int height = std::max(4, target_rect.height + border * 2);
        const int x = target_rect.x + target_rect.width / 2 - width / 2;
        const int y = target_rect.y + target_rect.height / 2 - height / 2;

        SetSize(x, y, width, height, wxSIZE_USE_EXISTING);
        wxRegion region(0, 0, width, height);
        if (width > border * 2 && height > border * 2)
            region.Subtract(wxRegion(border, border, width - border * 2, height - border * 2));
        SetShape(region);
        m_last_rect = target_rect;

        if (!IsShown())
            Show();
        Raise();
        Refresh();
    }

private:
    wxRect m_last_rect;

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
    }
};

class WidgetInspectorTimer final : public wxTimer
{
public:
    explicit WidgetInspectorTimer(WidgetInspector* inspector) : m_inspector(inspector) {}

private:
    void Notify() override { m_inspector->update_highlight(); }
    WidgetInspector* m_inspector;
};

WidgetInspector::WidgetInspector() : m_timer(std::make_unique<WidgetInspectorTimer>(this)) { wxEvtHandler::AddFilter(this); }

WidgetInspector::~WidgetInspector()
{
    if (m_timer != nullptr)
        m_timer->Stop();
    if (m_overlay != nullptr) {
        m_overlay->Destroy();
        m_overlay = nullptr;
    }
    if (m_dialog != nullptr) {
        m_dialog->Destroy();
        m_dialog = nullptr;
    }

    wxEvtHandler::RemoveFilter(this);
}

void WidgetInspector::disable()
{
    inspector_instance().reset();
}

void WidgetInspector::enable()
{
    auto& instance = inspector_instance();
    if (instance == nullptr) {
        instance = std::make_unique<WidgetInspector>();
        BOOST_LOG_TRIVIAL(info) << "[WidgetInspector] enabled; press Ctrl+Shift+F12";
    }
}

void WidgetInspector::set_armed(bool armed)
{
    m_armed = armed;
    m_last_mouse_position = wxDefaultPosition;
    if (m_timer != nullptr) {
        if (armed)
            m_timer->Start(25);
        else
            m_timer->Stop();
    }
    if (!armed && m_overlay != nullptr) {
        m_overlay->Destroy();
        m_overlay = nullptr;
    }
    BOOST_LOG_TRIVIAL(info) << "[WidgetInspector] picker " << (armed ? "armed" : "disarmed");
}

void WidgetInspector::ensure_overlay(wxWindow& target)
{
    wxWindow* parent = wxGetTopLevelParent(&target);
    if (parent == nullptr)
        parent = &target;
    if (m_overlay == nullptr) {
        m_overlay = new WidgetHighlightOverlay(parent);
        return;
    }

    auto* overlay = m_overlay.get();
    if (overlay->GetParent() == parent)
        return;

    overlay->Destroy();
    m_overlay = nullptr;
    m_overlay = new WidgetHighlightOverlay(parent);
}

void WidgetInspector::update_highlight()
{
    const wxPoint position = ::wxGetMousePosition();
    if (position == m_last_mouse_position)
        return;
    m_last_mouse_position = position;

    wxWindow* target = wxFindWindowAtPoint(position);
    if (target == m_overlay.get())
        return;
    if (target == nullptr) {
        if (m_overlay != nullptr)
            m_overlay->hide();
        return;
    }

    ensure_overlay(*target);
    if (m_overlay != nullptr)
        m_overlay->show_around(target->GetScreenRect());
}

int WidgetInspector::FilterEvent(wxEvent& event)
{
    const wxEventType type = event.GetEventType();
    if (type == wxEVT_CHAR_HOOK || type == wxEVT_KEY_DOWN) {
        auto& key_event = static_cast<wxKeyEvent&>(event);
        const bool shortcut = key_event.ControlDown() && key_event.ShiftDown() && key_event.GetKeyCode() == WXK_F12;
        if (shortcut) {
            set_armed(!m_armed);
            if (m_armed)
                update_highlight();
            return Event_Processed;
        }
        return Event_Skip;
    }

    if (type != wxEVT_LEFT_DOWN || !m_armed)
        return Event_Skip;

    if (wxWindow* captured_window = wxWindow::GetCapture())
        captured_window->ReleaseMouse();

    set_armed(false);
    inspect_pointer_position();
    return Event_Ignore;
}

void WidgetInspector::inspect_pointer_position()
{
    const wxPoint position = ::wxGetMousePosition();
    wxWindow* window = wxFindWindowAtPoint(position);
    if (window == nullptr) {
        const std::string report = "[WidgetInspector] no wxWindow at pointer";
        BOOST_LOG_TRIVIAL(warning) << report;
        if (copy_text_to_clipboard(report))
            BOOST_LOG_TRIVIAL(warning) << "[WidgetInspector] no-window report copied to clipboard";
        else
            BOOST_LOG_TRIVIAL(warning) << "[WidgetInspector] failed to copy no-window report";
        return;
    }

    const std::string ai_locator = build_ai_locator(*window, position);
    const std::string debug_report = build_debug_report(*window, position);
    if (m_dialog == nullptr) {
        m_dialog = new WidgetInspectorDialog(wxGetTopLevelParent(window), std::move(ai_locator), std::move(debug_report));
        m_dialog->Show();
        return;
    }

    if (auto* dialog = dynamic_cast<WidgetInspectorDialog*>(m_dialog.get()); dialog != nullptr)
        dialog->update_reports(std::move(ai_locator), std::move(debug_report));
}

std::string WidgetInspector::build_ai_locator(wxWindow& window, const wxPoint& position)
{
    std::vector<std::string> navigation;
    append_navigation(window, navigation);

    std::ostringstream out;
    out << "format_version: 1\n\n"
        << "semantic:\n"
        << "  app_view: " << (navigation.empty() ? yaml_quote(std::string("unknown")) : yaml_quote(navigation.front())) << "\n"
        << "  navigation:\n";
    for (const std::string& item : navigation)
        out << "    - " << yaml_quote(item) << "\n";
    out << "  target_role: " << yaml_quote(target_role(window)) << "\n"
        << "  semantic_id: " << yaml_quote(semantic_id(window)) << "\n\n";

    out << "identity:\n" << window_summary(window, "");
    if (const auto* combo = dynamic_cast<const PlaterPresetComboBox*>(&window); combo != nullptr && combo->get_filament_idx() >= 0) {
        const int index = combo->get_filament_idx();
        out << "  filament_index_0_based: " << index << "\n"
            << "  filament_index_1_based: " << index + 1 << "\n";
    }

    out << "\nlocator:\n"
        << "  strategy: ancestor_chain_plus_semantic\n"
        << "  chain:\n";
    int depth = 0;
    for (wxWindow* parent = &window; parent != nullptr; parent = parent->GetParent(), ++depth) {
        out << "    - depth: " << depth << "\n" << window_summary(*parent, "      ");
    }

    out << "\n" << neighborhood_report(window) << "\n";
    append_geometry(window, out);
    out << "\n" << aui_toolbar_report(window, position);
    return out.str();
}

std::string WidgetInspector::build_debug_report(wxWindow& window, const wxPoint& position)
{
    std::ostringstream out;
    out << build_ai_locator(window, position) << "\n"
        << "ephemeral_debug:\n"
        << "  object_address: " << pointer_text(&window) << "\n"
        << "  event_handler_address: " << pointer_text(window.GetEventHandler()) << "\n"
        << "  native_hwnd: " << pointer_text(window.GetHandle()) << "\n"
        << "  note: addresses are valid only during this process run\n";
    return out.str();
}

} // namespace GUI
} // namespace Slic3r
