#include "PlaterFilamentComboBox.hpp"

#include "GUI_App.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/tokenzr.h>
#include <wx/scrolwin.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <boost/log/trivial.hpp>

namespace Slic3r
{
namespace GUI
{

namespace
{

wxWeakRef<FilamentDropDown> s_active_popup;
wxWeakRef<PlaterFilamentComboBox> s_active_owner;

#ifdef __WXOSX__
// wxOSX reposts an outside click after dismissing a transient popup. The repost is a copied
// wxMouseEvent, so it keeps the original timestamp and screen position but has a different object
// address. Detect that copy at the filament combo boundary without modifying wxWidgets globally.
class RepostedClickDetector : public wxEventFilter
{
public:
    static void install()
    {
        if (s_detector == nullptr)
        {
            // This observer intentionally lives until process termination because wx event filters
            // may outlive individual combo boxes during application shutdown.
            s_detector = new RepostedClickDetector();
            wxEvtHandler::AddFilter(s_detector);
        }
    }

    static bool consume_repost(const wxEvent &event)
    {
        for (const wxEvent *&pending : s_pending)
        {
            if (pending != &event)
                continue;
            pending = nullptr;
            return true;
        }
        return false;
    }

    int FilterEvent(wxEvent &event) override
    {
        if (event.GetEventType() != wxEVT_LEFT_DOWN || event.GetTimestamp() <= 0)
            return Event_Skip;

        const auto *mouse_event = dynamic_cast<const wxMouseEvent *>(&event);
        auto       *window      = dynamic_cast<wxWindow *>(event.GetEventObject());
        if (mouse_event == nullptr || window == nullptr)
            return Event_Skip;

        const wxPoint            screen_position = window->ClientToScreen(mouse_event->GetPosition());
        const auto               now             = std::chrono::steady_clock::now();
        const long               timestamp       = event.GetTimestamp();
        constexpr auto           max_age          = std::chrono::seconds(2);

        for (ClickRecord &record : s_history)
        {
            if (!record.valid)
                continue;
            if (now - record.observed_at > max_age)
            {
                record.valid = false;
                continue;
            }
            if (record.timestamp != timestamp || record.event == &event ||
                record.screen_position != screen_position)
                continue;

            for (const wxEvent *&pending : s_pending)
            {
                if (pending == nullptr)
                {
                    pending = &event;
                    break;
                }
            }
            return Event_Skip;
        }

        ClickRecord &record   = s_history[s_next];
        record.valid          = true;
        record.timestamp      = timestamp;
        record.screen_position = screen_position;
        record.event          = &event;
        record.observed_at    = now;
        s_next                = (s_next + 1) % s_history.size();
        return Event_Skip;
    }

private:
    struct ClickRecord
    {
        bool                                   valid;
        long                                   timestamp;
        wxPoint                                screen_position;
        const wxEvent                         *event;
        std::chrono::steady_clock::time_point observed_at;
    };

    static constexpr size_t k_history = 16;
    static constexpr size_t k_pending = 16;

    inline static RepostedClickDetector             *s_detector = nullptr;
    inline static std::array<ClickRecord, k_history> s_history{};
    inline static std::array<const wxEvent *, k_pending> s_pending{};
    inline static size_t                             s_next = 0;
};
#endif

constexpr const char *g_topn_file_name      = "filament_topn.json";
constexpr const char *g_snapmaker_vendor    = "Snapmaker";
constexpr const char *g_bambu_vendor        = "Bambu";
constexpr const char *g_bambu_lab_vendor    = "Bambu Lab";

wxWindow *scroll_parent(wxWindow *window)
{
    wxWindow *current = window;
    while (current != nullptr && current->GetParent() != nullptr) {
        wxWindow *parent = current->GetParent();
        if (dynamic_cast<wxScrollHelper *>(parent) != nullptr)
            return current;
        current = parent;
    }
    return nullptr;
}

std::filesystem::path filament_topn_path()
{
    std::filesystem::path system_path = std::filesystem::u8path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR /
                                         g_snapmaker_vendor / "filament" / g_topn_file_name;
    std::error_code filesystem_error;
    if (std::filesystem::exists(system_path, filesystem_error))
        return system_path;
    if (filesystem_error)
        BOOST_LOG_TRIVIAL(warning) << "FilamentTopNOrder could not inspect " << system_path.u8string() << ": "
                                   << filesystem_error.message();

    return std::filesystem::u8path(Slic3r::resources_dir()) / "profiles" / g_snapmaker_vendor / "filament" /
           g_topn_file_name;
}

/** @brief Loads the TopN configuration once from the user or resource path. */
FilamentTopNOrder load_filament_topn_order()
{
    const std::filesystem::path path = filament_topn_path();
    std::ifstream               stream(path);
    if (!stream)
    {
        BOOST_LOG_TRIVIAL(warning) << "FilamentTopNOrder failed to open " << path.u8string();
        return FilamentTopNOrder{};
    }

    FilamentTopNOrder order = FilamentTopNOrder::from_stream(stream);
    if (order.empty())
        BOOST_LOG_TRIVIAL(warning) << "FilamentTopNOrder has invalid or empty configuration: " << path.u8string();
    else
        BOOST_LOG_TRIVIAL(info) << "FilamentTopNOrder loaded: " << path.u8string();
    return order;
}

/**
 * @brief Returns the process-wide immutable TopN configuration.
 *
 * The one-time load is intentional: popup refreshes and preset updates in the
 * running process must not perform file I/O or change the active ordering. If
 * PresetUpdater replaces the deployed file, the new order takes effect after
 * the next application restart.
 */
const FilamentTopNOrder &filament_topn_order()
{
    static const FilamentTopNOrder order = load_filament_topn_order();
    return order;
}

std::string config_string(const Preset *preset, const char *key)
{
    if (preset == nullptr)
        return {};

    const auto *option = preset->config.option<ConfigOptionStrings>(key);
    if (option == nullptr || option->values.empty())
        return {};
    return option->values.front();
}

bool is_missing_vendor(const std::string &vendor)
{
    wxString label = from_u8(vendor);
    label.Trim(true).Trim(false);
    return label.empty() || label == wxString::FromUTF8("(Undefined)");
}

std::string vendor_from_display_name(const wxString &display_name)
{
    wxString name = display_name;
    name.Trim(true).Trim(false);
    wxStringTokenizer words(name);
    return words.HasMoreTokens() ? into_u8(words.GetNextToken()) : std::string();
}

} // namespace

PlaterFilamentComboBox::PlaterFilamentComboBox(wxWindow *parent, Preset::Type preset_type)
    : PlaterPresetComboBox(parent, preset_type)
{
#ifdef __WXOSX__
    RepostedClickDetector::install();
#endif
    Bind(wxEVT_LEFT_DOWN, &PlaterFilamentComboBox::on_mouse_down, this);
    Bind(wxEVT_LEFT_DCLICK, &PlaterFilamentComboBox::on_mouse_down, this);
    Bind(wxEVT_KEY_DOWN, &PlaterFilamentComboBox::on_key_down, this);

    if (preset_type != Preset::TYPE_FILAMENT)
        return;

    // Load the TopN order once; popup refreshes never perform file I/O.
    m_system_vendor_sorter   = std::make_unique<SystemFilamentVendorSorter>();
    m_system_filament_sorter = std::make_unique<SystemFilamentSorter>(filament_topn_order());
    m_project_sorter         = std::make_unique<FilamentSorter>();
    m_user_sorter            = std::make_unique<FilamentSorter>();

    std::unique_ptr<FilamentDropDown> popup = std::make_unique<FilamentDropDown>(m_popup_items);
    if (!popup->Create(this))
    {
        BOOST_LOG_TRIVIAL(warning)
            << "Could not create the grouped filament popup; falling back to the standard dropdown.";
        return;
    }
    m_popup = popup.release();
    m_popup->SetUseContentWidth(true, true);
    m_popup->Bind(wxEVT_COMBOBOX, &PlaterFilamentComboBox::on_popup_selection, this);
    m_popup->Bind(EVT_DISMISS, &PlaterFilamentComboBox::on_popup_dismiss, this);

    // Text-control key events do not reach this window's handlers; bind them separately.
    if (GetTextCtrl() != nullptr)
        GetTextCtrl()->Bind(wxEVT_KEY_DOWN, &PlaterFilamentComboBox::on_key_down, this);

    m_top_level = wxGetTopLevelParent(this);
    if (m_top_level != nullptr) {
        m_top_level->Bind(wxEVT_MOVE, &PlaterFilamentComboBox::on_top_level_move, this);
        m_top_level->Bind(wxEVT_SIZE, &PlaterFilamentComboBox::on_top_level_size, this);
    }

    // WHY: the scrolled content panel, not the top-level frame, moves when the sidebar scrolls.
    // The top-level MOVE handler cannot observe that child movement, so both bindings are needed
    // to dismiss a popup that would otherwise remain detached from its combo box.
    m_scroll_parent = scroll_parent(this);
    if (m_scroll_parent != nullptr && m_scroll_parent != m_top_level)
        m_scroll_parent->Bind(wxEVT_MOVE, &PlaterFilamentComboBox::on_scroll_parent_move, this);
}

void PlaterFilamentComboBox::set_project_sorter(std::unique_ptr<FilamentSorter> sorter)
{
    m_project_sorter = std::move(sorter);
    rebuild_popup_rows();
}

void PlaterFilamentComboBox::set_user_sorter(std::unique_ptr<FilamentSorter> sorter)
{
    m_user_sorter = std::move(sorter);
    rebuild_popup_rows();
}

void PlaterFilamentComboBox::set_system_vendor_sorter(std::unique_ptr<FilamentVendorSorter> sorter)
{
    if (sorter == nullptr)
        sorter = std::make_unique<SystemFilamentVendorSorter>();
    m_system_vendor_sorter = std::move(sorter);
    rebuild_popup_rows();
}

void PlaterFilamentComboBox::set_system_filament_sorter(std::unique_ptr<FilamentSorter> sorter)
{
    if (sorter == nullptr)
        sorter = std::make_unique<SystemFilamentSorter>(filament_topn_order());
    m_system_filament_sorter = std::move(sorter);
    rebuild_popup_rows();
}

PlaterFilamentComboBox::~PlaterFilamentComboBox()
{
    // Unbind the popup's handlers before closing it: close_popup() dismisses the popup, whose
    // OnDismiss() posts EVT_DISMISS back to this window; that must not reach a partially
    // destroyed object. Keep this order.
    if (m_popup != nullptr)
    {
        m_popup->Unbind(wxEVT_COMBOBOX, &PlaterFilamentComboBox::on_popup_selection, this);
        m_popup->Unbind(EVT_DISMISS, &PlaterFilamentComboBox::on_popup_dismiss, this);
    }

    close_popup(false);

    if (m_top_level != nullptr) {
        m_top_level->Unbind(wxEVT_MOVE, &PlaterFilamentComboBox::on_top_level_move, this);
        m_top_level->Unbind(wxEVT_SIZE, &PlaterFilamentComboBox::on_top_level_size, this);
    }

    if (m_scroll_parent != nullptr && m_scroll_parent != m_top_level)
        m_scroll_parent->Unbind(wxEVT_MOVE, &PlaterFilamentComboBox::on_scroll_parent_move, this);

    if (GetTextCtrl() != nullptr)
        GetTextCtrl()->Unbind(wxEVT_KEY_DOWN, &PlaterFilamentComboBox::on_key_down, this);
}

void PlaterFilamentComboBox::update()
{
    // WHY: the base update() below can emit a selection change that reaches
    // Plater::priv::on_select_preset(), which calls combo->update() again (Plater.cpp:14993).
    // The nested call must not dismiss the popup or rebuild the rows a second time.
    if (m_rebuilding)
        return;

    close_popup(true);
    m_rebuilding = true;

    // Parent update() first; this class only changes how rows are presented.
    PlaterPresetComboBox::update();
    rebuild_popup_rows();

    m_rebuilding = false;
}

void PlaterFilamentComboBox::msw_rescale()
{
    PlaterPresetComboBox::msw_rescale();
    if (m_popup != nullptr) {
        m_popup->Rescale();
        m_popup->Invalidate();
    }
}

PlaterFilamentComboBox::Section PlaterFilamentComboBox::section_from_header(const wxString &text) const
{
    if (text == separator("Project-inside presets"))
        return Section::Project;
    if (text == separator("User presets"))
        return Section::User;
    if (text == separator("System presets"))
        return Section::System;
    return Section::Other;
}

PlaterFilamentComboBox::PopupRow PlaterFilamentComboBox::make_header(const wxString &text,
                                                                      Section section) const
{
    PopupRow row;
    row.item.text  = text;
    row.item.style = 0;
    row.section    = section;
    row.header     = true;
    return row;
}

std::string PlaterFilamentComboBox::preset_vendor(const Preset *preset) const
{
    std::string vendor = config_string(preset, "filament_vendor");
    // Historical spelling: normalise it in a case-insensitive way, like every other vendor comparison here.
    if (from_u8(vendor).CmpNoCase(wxString::FromUTF8(g_bambu_lab_vendor)) == 0)
        vendor = g_bambu_vendor;
    return vendor;
}

std::string PlaterFilamentComboBox::preset_filament_product(const Preset *preset) const
{
    if (preset == nullptr)
        return {};

    wxString product = from_u8(Preset::remove_suffix_modified(preset->name));
    product.Trim(true).Trim(false);

    wxString vendor = from_u8(preset_vendor(preset));
    vendor.Trim(true).Trim(false);
    if (vendor.empty() || vendor == wxString::FromUTF8("(Undefined)")) {
        wxStringTokenizer words(product);
        if (words.HasMoreTokens())
            vendor = words.GetNextToken();
    }

    if (!vendor.empty() && product.length() > vendor.length() && product.Left(vendor.length()).CmpNoCase(vendor) == 0 &&
        product[vendor.length()] == ' ')
        product = product.Mid(vendor.length() + 1);

    const int printer_suffix = product.Find(" @");
    if (printer_suffix != wxNOT_FOUND)
        product = product.Left(printer_suffix);

    product.Trim(true).Trim(false);
    return into_u8(product);
}

wxString PlaterFilamentComboBox::popup_group(Section section, const std::string &vendor) const
{
    switch (section) {
    case Section::Project: return _L("Project") + " ";
    case Section::User:    return _L("Custom") + " ";
    case Section::System:
        return vendor.empty() ? _L("System") : from_u8(vendor);
    case Section::Other:   return wxString();
    }
    return wxString();
}

bool PlaterFilamentComboBox::is_system_row(const PopupRow &row) const
{
    return !row.header && row.combo_index >= 0 && row.section == Section::System;
}

void PlaterFilamentComboBox::rebuild_popup_rows()
{
    m_rows.clear();
    m_popup_to_combo.clear();
    m_popup_items.clear();

    if (m_popup == nullptr || m_collection == nullptr || m_type != Preset::TYPE_FILAMENT)
        return;

    Section current_section = Section::Other;
    for (unsigned int combo_index = 0; combo_index < GetCount(); ++combo_index) {
        if (combo_index > static_cast<unsigned int>(std::numeric_limits<int>::max()))
            break;
        const wxString text = GetString(combo_index);
        const Marker marker = reinterpret_cast<Marker>(GetClientData(combo_index));

        if (marker >= LABEL_ITEM_MARKER && marker < LABEL_ITEM_MAX && marker != LABEL_ITEM_DISABLED) {
            if (marker == LABEL_ITEM_WIZARD_FILAMENTS) {
                PopupRow row;
                row.item.text       = text;
                row.item.icon       = GetItemBitmap(combo_index);
                row.item.tip        = GetItemTooltip(combo_index);
                if (row.item.tip.IsEmpty())
                    row.item.tip = text;
                row.combo_index     = static_cast<int>(combo_index);
                row.section         = Section::Other;
                m_rows.push_back(std::move(row));
            } else {
                current_section = section_from_header(text);
                m_rows.push_back(make_header(text, current_section));
            }
            continue;
        }

        PopupRow row;
        row.item.text       = text;
        row.item.icon       = GetItemBitmap(combo_index);
        row.item.tip        = GetItemTooltip(combo_index);
        row.item.style      = marker == LABEL_ITEM_DISABLED ? DD_ITEM_STYLE_DISABLED : 0;
        row.combo_index     = static_cast<int>(combo_index);
        row.section         = current_section;
        row.sort_item.display_name   = text;
        row.sort_item.original_index = combo_index;

        // AMS/machine rows stay outside Project/User/System even for system presets.
        if (current_section != Section::Other) {
            const std::string alias = Preset::remove_suffix_modified(into_u8(text));
            const std::string &resolved_name = m_collection->get_preset_name_by_alias(alias);
            const Preset *preset = m_collection->find_preset(resolved_name);
            if (preset != nullptr) {
                if (preset->is_project_embedded)
                    row.section = Section::Project;
                else if (preset->is_default || preset->is_system)
                    row.section = Section::System;
                else
                    row.section = Section::User;

                row.sort_item.vendor           = preset_vendor(preset);
                row.sort_item.filament_product = preset_filament_product(preset);
                row.item.tip                    = get_tooltip(*preset);
            }
        }

        // Fall back to the item text when no tooltip was resolved.
        if (row.item.tip.IsEmpty())
            row.item.tip = row.item.text;

        // Group placeholder/empty system vendors by the display name's first word.
        if (row.section == Section::System && is_missing_vendor(row.sort_item.vendor)) {
            row.sort_item.vendor = vendor_from_display_name(row.item.text);
            if (row.sort_item.vendor.empty())
                continue;
        }

        if (row.section == Section::System)
            row.sort_item.vendor = canonical_vendor(row.sort_item.vendor);

        if (current_section == Section::Other) {
            row.item.group.clear();
        } else {
            row.item.group = popup_group(row.section, row.sort_item.vendor);
        }

        m_rows.push_back(std::move(row));
    }

    // Drop a system header that has no rows left.
    for (auto it = m_rows.begin(); it != m_rows.end();) {
        if (!it->header || it->section != Section::System) {
            ++it;
            continue;
        }

        const auto next = std::next(it);
        if (next == m_rows.end() || next->header || next->section != Section::System)
            it = m_rows.erase(it);
        else
            ++it;
    }

    // Null project/user sorters preserve base order.
    sort_section_rows(Section::Project, m_project_sorter.get());
    sort_section_rows(Section::User, m_user_sorter.get());
    sort_system_rows();

    m_popup_items.reserve(m_rows.size());
    m_popup_to_combo.reserve(m_rows.size());
    for (const PopupRow &row : m_rows) {
        m_popup_items.push_back(row.item);
        m_popup_to_combo.push_back(row.combo_index);
    }

    m_popup->SetItems(m_popup_items);
    int popup_selection = -1;
    const int combo_selection = GetSelection();
    for (size_t i = 0; i < m_popup_to_combo.size(); ++i) {
        if (i > static_cast<size_t>(std::numeric_limits<int>::max()))
            break;
        if (m_popup_to_combo[i] == combo_selection) {
            popup_selection = static_cast<int>(i);
            break;
        }
    }
    m_popup->SetSelection(popup_selection);
}

void PlaterFilamentComboBox::sort_section_rows(Section section, const FilamentSorter *sorter)
{
    if (sorter == nullptr)
        return;

    for (size_t begin = 0; begin < m_rows.size();) {
        while (begin < m_rows.size() && (m_rows[begin].header || m_rows[begin].section != section))
            ++begin;
        size_t end = begin;
        while (end < m_rows.size() && !m_rows[end].header && m_rows[end].section == section)
            ++end;
        if (begin != end) {
            std::stable_sort(m_rows.begin() + static_cast<std::ptrdiff_t>(begin),
                             m_rows.begin() + static_cast<std::ptrdiff_t>(end),
                             [sorter](const PopupRow &left, const PopupRow &right) {
                                 return sorter->less(left.sort_item, right.sort_item);
                             });
        }
        begin = end;
    }
}

void PlaterFilamentComboBox::sort_system_rows()
{
    // Vendor-equivalent rows delegate to the filament sorter (keeps a strict weak ordering).
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (!is_system_row(m_rows[i]))
            continue;

        size_t end = i;
        while (end < m_rows.size() && is_system_row(m_rows[end]))
            ++end;

        std::stable_sort(m_rows.begin() + static_cast<std::ptrdiff_t>(i),
                         m_rows.begin() + static_cast<std::ptrdiff_t>(end),
                         [this](const PopupRow &left, const PopupRow &right) {
                             if (m_system_vendor_sorter->less(left.sort_item.vendor, right.sort_item.vendor))
                                 return true;
                             if (m_system_vendor_sorter->less(right.sort_item.vendor, left.sort_item.vendor))
                                 return false;
                             return m_system_filament_sorter->less(left.sort_item, right.sort_item);
                         });
        // Land on the first row after this system section.
        i = end > 0 ? end - 1 : end;
    }
}

void PlaterFilamentComboBox::show_popup()
{
    if (m_popup == nullptr || !IsEnabled())
        return;

    // Dismiss a flat popup opened before this handler took ownership.
    if (GetDropDown().IsShown())
        GetDropDown().Dismiss();

    if (m_popup_visible)
    {
        close_popup(true);
        return;
    }

    // wxOSX and wxGTK may repost the click that dismissed the transient popup to this control.
    // Keep the base ComboBox debounce so that dismissing never immediately reopens the popup.
    if (!m_popup->HasDismissLongTime())
        return;

    if (s_active_owner && s_active_owner.get() != this)
        s_active_owner->close_popup(true);
    else if (s_active_popup && s_active_popup.get() != m_popup)
        s_active_popup->DismissAll();

    update();
    if (m_popup_items.empty())
        return;

    s_active_popup = m_popup;
    s_active_owner = this;
    // WHY: update() above rebuilds m_popup_items and may synchronously notify selection listeners.
    // Set the visible flag only after that work succeeds, so callbacks cannot observe a half-open
    // popup and close/reopen it before PopupForParent() owns the window.
    m_popup_visible = true;
    wxCommandEvent open_event(wxEVT_COMBOBOX_DROPDOWN, GetId());
    open_event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(open_event);
    m_popup->PopupForParent();
    m_popup->openSelectionGroup();
}

void PlaterFilamentComboBox::close_popup(bool notify)
{
    const bool was_visible = m_popup_visible;
    m_popup_visible = false;
    if (s_active_popup && s_active_popup.get() == m_popup)
        s_active_popup = nullptr;
    if (s_active_owner && s_active_owner.get() == this)
        s_active_owner = nullptr;

    if (m_popup != nullptr)
        m_popup->DismissAll();

    if (notify && was_visible) {
        wxCommandEvent close_event(wxEVT_COMBOBOX_CLOSEUP, GetId());
        close_event.SetEventObject(this);
        GetEventHandler()->ProcessEvent(close_event);
    }
}

void PlaterFilamentComboBox::on_popup_selection(wxCommandEvent &event)
{
    const int popup_index = event.GetInt();
    if (popup_index < 0 || static_cast<size_t>(popup_index) >= m_popup_to_combo.size())
        return;

    const int combo_index = m_popup_to_combo[popup_index];
    if (combo_index < 0 || static_cast<unsigned int>(combo_index) >= GetCount())
        return;

    close_popup(true);
    SetSelection(combo_index);

    wxCommandEvent select_event(wxEVT_COMBOBOX, GetId());
    select_event.SetEventObject(this);
    select_event.SetInt(combo_index);
    select_event.SetString(GetString(combo_index));
    GetEventHandler()->ProcessEvent(select_event);
}

void PlaterFilamentComboBox::on_popup_dismiss(wxCommandEvent &event)
{
    event.StopPropagation();
    if (m_popup_visible)
        close_popup(true);
    else {
        if (s_active_popup && s_active_popup.get() == m_popup)
            s_active_popup = nullptr;
        if (s_active_owner && s_active_owner.get() == this)
            s_active_owner = nullptr;
    }
}

void PlaterFilamentComboBox::on_mouse_down(wxMouseEvent &event)
{
#ifdef __WXOSX__
    const bool reposted_click = RepostedClickDetector::consume_repost(event);
    if (reposted_click && !m_popup_visible)
    {
        // The popup already consumed the physical click. Do not reopen this combo when wxOSX
        // delivers its geometry-based repost to the wrong control.
        event.Skip(false);
        event.StopPropagation();
        return;
    }
#endif
    if (m_popup == nullptr) {
        // Let ComboBox's static event table open its existing flat popup.
        event.Skip();
        return;
    }

    SetFocus();
    show_popup();

    // Reset the skip flag so wxWidgets doesn't reach ComboBox's static table.
    event.Skip(false);
    event.StopPropagation();
}

void PlaterFilamentComboBox::on_key_down(wxKeyEvent &event)
{
    // Alt combinations (e.g. Alt+Space system menu) must not be consumed here.
    if (event.AltDown()) {
        event.Skip();
        return;
    }

    switch (event.GetKeyCode()) {
    case WXK_RETURN:
    case WXK_SPACE:
    case WXK_DOWN:
        if (m_popup == nullptr) {
            // Preserve the base ComboBox keyboard behaviour after popup creation fails.
            event.Skip();
            return;
        }
        show_popup();
        event.Skip(false);
        event.StopPropagation();
        return;
    case WXK_ESCAPE:
        if (m_popup_visible) {
            close_popup(true);
            event.Skip(false);
            event.StopPropagation();
            return;
        }
        break;
    default:
        break;
    }
    event.Skip();
}

void PlaterFilamentComboBox::on_scroll_parent_move(wxMoveEvent &event)
{
    close_popup(true);
    event.Skip();
}

void PlaterFilamentComboBox::on_top_level_move(wxMoveEvent &event)
{
    close_popup(true);
    event.Skip();
}

void PlaterFilamentComboBox::on_top_level_size(wxSizeEvent &event)
{
    close_popup(true);
    event.Skip();
}

} // namespace GUI
} // namespace Slic3r
