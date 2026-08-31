#include "PresetComboBoxes.hpp"

#include <cstddef>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>
#include <set>
#include <utility>
#include <algorithm>
#include <boost/algorithm/string.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/colordlg.h>
#include <wx/wupdlock.h>
#include <wx/menu.h>
#include <wx/odcombo.h>
#include <wx/listbook.h>

#ifdef _WIN32
#include <wx/msw/dcclient.h>
#include <wx/msw/private.h>
#endif

#include "libslic3r/libslic3r.h"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Color.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "FilamentColorUtils.hpp"
#include "NotificationManager.hpp"
#include "FilamentColorDialog.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Tab.hpp"
#include "ConfigWizard.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/UndoRedo.hpp"
#include "../Utils/ColorSpaceConvert.hpp"
#include "BitmapCache.hpp"
#include "SavePresetDialog.hpp"
#include "MsgDialog.hpp"
#include "ParamsDialog.hpp"
#include "FilamentPickerDialog.hpp"
#include "wxExtensions.hpp"

#include "DeviceCore/DevManager.h"

// A workaround for a set of issues related to text fitting into gtk widgets:
#if defined(__WXGTK20__) || defined(__WXGTK3__)
    #include <glib-2.0/glib-object.h>
    #include <pango-1.0/pango/pango-layout.h>
    #include <gtk/gtk.h>
#endif

using Slic3r::GUI::format_wxstr;

namespace Slic3r {
namespace GUI {

#define BORDER_W 10

namespace
{

std::string ConfigStringAt(const DynamicPrintConfig& config, const std::string& key, int index)
{
    if (index < 0 || !config.has(key))
        return {};

    const ConfigOptionStrings* option = config.option<ConfigOptionStrings>(key);
    const size_t value_index = static_cast<size_t>(index);
    return option != nullptr && option->values.size() > value_index ? option->values[value_index] : std::string();
}

int ConfigIntAt(const DynamicPrintConfig& config, const std::string& key, int index)
{
    if (index < 0 || !config.has(key))
        return 0;

    const ConfigOptionInts* option = config.option<ConfigOptionInts>(key);
    const size_t value_index = static_cast<size_t>(index);
    return option != nullptr && option->values.size() > value_index ? option->values[value_index] : 0;
}

ConfigOptionStrings* CloneStringOption(const DynamicPrintConfig& config, const std::string& key)
{
    const ConfigOptionStrings* option = config.option<ConfigOptionStrings>(key);
    return option != nullptr ? static_cast<ConfigOptionStrings*>(option->clone()) : new ConfigOptionStrings{};
}

ConfigOptionInts* CloneIntOption(const DynamicPrintConfig& config, const std::string& key)
{
    const ConfigOptionInts* option = config.option<ConfigOptionInts>(key);
    return option != nullptr ? static_cast<ConfigOptionInts*>(option->clone()) : new ConfigOptionInts{};
}

void ResizeStrings(ConfigOptionStrings* option, size_t size)
{
    if (option != nullptr && option->values.size() < size)
        option->values.resize(size);
}

void ResizeInts(ConfigOptionInts* option, size_t size)
{
    if (option != nullptr && option->values.size() < size)
        option->values.resize(size);
}

std::string FilamentBaseName(std::string name)
{
    name = Preset::remove_suffix_modified(name);
    return FilamentColorUtils::GetFilamentMatchName(name);
}

bool IsSnapmakerFilamentName(const std::string& name)
{
    return boost::algorithm::istarts_with(name, "Snapmaker");
}

bool FilamentMatchesPresetName(const FilamentColorInfo& filament, const std::string& presetName)
{
    const std::string filamentName = FilamentBaseName(filament.filamentName);
    const std::string currentFilamentName = FilamentBaseName(presetName);
    return !filamentName.empty() && filamentName == currentFilamentName;
}

wxSize FilamentColorPickerBitmapSize(const wxButton* picker)
{
    wxSize size = picker != nullptr ? picker->GetSize() : wxSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = picker != nullptr ? picker->GetClientSize() : wxSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        size = wxSize(20, 20);

    return size;
}

wxBitmap* GetFilamentColorPickerBitmap(const DynamicPrintConfig& config,
                                       int filamentIdx,
                                       const std::string& fallbackColor,
                                       const wxSize& size)
{
    const std::string multiColors = ConfigStringAt(config, "filament_multi_colors", filamentIdx);
    const int colorModeValue = ConfigIntAt(config, "filament_colour_mode", filamentIdx);
    const FilamentColorMode colorMode = FilamentColorModeFromConfig(colorModeValue);
    const std::string label = filamentIdx >= 0 ? std::to_string(filamentIdx + 1) : std::string();
    return FilamentColorUtils::GetFilamentColorIcon(multiColors, colorMode, fallbackColor, label,
                                                    std::max(1, size.GetWidth()), std::max(1, size.GetHeight()));
}

} // namespace

// ---------------------------------
// ***  PresetComboBox  ***
// ---------------------------------

/* For PresetComboBox we use bitmaps that are created from images that are already scaled appropriately for Retina
 * (Contrary to the intuition, the `scale` argument for Bitmap's constructor doesn't mean
 * "please scale this to such and such" but rather
 * "the wxImage is already sized for backing scale such and such". )
 * Unfortunately, the constructor changes the size of wxBitmap too.
 * Thus We need to use unscaled size value for bitmaps that we use
 * to avoid scaled size of control items.
 * For this purpose control drawing methods and
 * control size calculation methods (virtual) are overridden.
 **/

PresetComboBox::PresetComboBox(wxWindow* parent, Preset::Type preset_type, const wxSize& size, PresetBundle* preset_bundle/* = nullptr*/) :
    ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size, 0, nullptr, wxCB_READONLY),
    m_type(preset_type),
    m_last_selected(wxNOT_FOUND),
    m_em_unit(em_unit(this)),
    m_preset_bundle(preset_bundle ? preset_bundle : wxGetApp().preset_bundle)
{
#ifdef __WXMSW__
    if (preset_type == Preset::TYPE_FILAMENT)
        SetFont(Label::Body_13);
#endif // __WXMSW__

    switch (m_type)
    {
    case Preset::TYPE_PRINT: {
        m_collection = &m_preset_bundle->prints;
        m_main_bitmap_name = "cog";
        break;
    }
    case Preset::TYPE_FILAMENT: {
        m_collection = &m_preset_bundle->filaments;
        m_main_bitmap_name = "spool";
        break;
    }
    case Preset::TYPE_SLA_PRINT: {
        m_collection = &m_preset_bundle->sla_prints;
        m_main_bitmap_name = "cog";
        break;
    }
    case Preset::TYPE_SLA_MATERIAL: {
        m_collection = &m_preset_bundle->sla_materials;
        m_main_bitmap_name = "blank_16";
        break;
    }
    case Preset::TYPE_PRINTER: {
        m_collection = &m_preset_bundle->printers;
        m_main_bitmap_name = "printer";
        break;
    }
    default: break;
    }

    m_bitmapCompatible   = ScalableBitmap(this, "flag_green");
    m_bitmapIncompatible = ScalableBitmap(this, "flag_red");

    // parameters for an icon's drawing
    fill_width_height();

    // Snapmaker: the mouse wheel must not change the selection while the drop-down is closed.
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& e) {
        if (m_suppress_change)
            e.StopPropagation();
        else
            e.Skip();
    });
    Bind(wxEVT_COMBOBOX_DROPDOWN, [this](wxCommandEvent&) { m_suppress_change = false; });
    Bind(wxEVT_COMBOBOX_CLOSEUP,  [this](wxCommandEvent&) { m_suppress_change = true;  });

    Bind(wxEVT_COMBOBOX, &PresetComboBox::OnSelect, this);
}

void PresetComboBox::OnSelect(wxCommandEvent& evt)
{
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender")
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter
    auto selected_item = evt.GetSelection();

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX)
        this->SetSelection(m_last_selected);
    else if (on_selection_changed && (m_last_selected != selected_item || m_collection->current_is_dirty())) {
        m_last_selected = selected_item;
        on_selection_changed(selected_item);
        evt.StopPropagation();
    }
    evt.Skip();
}

PresetComboBox::~PresetComboBox()
{
}

BitmapCache& PresetComboBox::bitmap_cache()
{
    static BitmapCache bmps;
    return bmps;
}

void PresetComboBox::set_label_marker(int item, LabelItemType label_item_type)
{
    this->SetClientData(item, (void*)label_item_type);
}

bool PresetComboBox::set_printer_technology(PrinterTechnology pt)
{
    if (printer_technology != pt) {
        printer_technology = pt;
        return true;
    }
    return false;
}

void PresetComboBox::invalidate_selection()
{
    m_last_selected = INT_MAX; // this value means that no one item is selected
}

void PresetComboBox::validate_selection(bool predicate/*=false*/)
{
    if (predicate &&
        // just in case: mark m_last_selected as a first added element
        m_last_selected == INT_MAX)
        m_last_selected = GetCount() - 1;
}

void PresetComboBox::update_selection()
{
    /* If selected_preset_item is still equal to INT_MAX, it means that
     * there is no presets added to the list.
     * So, select last combobox item ("Add/Remove preset")
     */
    //validate_selection();
    if (m_last_selected == INT_MAX)
        m_last_selected = 1;

    SetSelection(m_last_selected);
#ifdef __WXMSW__
    // From the Windows 2004 the tooltip for preset combobox doesn't work after next call of SetTooltip()
    // (There was an issue, when tooltip doesn't appears after changing of the preset selection)
    // But this workaround seems to work: We should to kill tooltip and than set new tooltip value
    SetToolTip(NULL);
#endif
    SetToolTip(GetString(m_last_selected));

// A workaround for a set of issues related to text fitting into gtk widgets:
#if defined(__WXGTK20__) || defined(__WXGTK3__)
    // ORCA: m_widget may not be realized yet; the Flatpak/GTK3 build hit a null here.
    if (!m_widget)
        return;
    GtkWidget* widget = m_widget;
    if (GTK_IS_CONTAINER(widget)) {
        GList* children = gtk_container_get_children(GTK_CONTAINER(widget));
        if (children) {
            widget = GTK_WIDGET(children->data);
            g_list_free(children);
        }
    }
    if (GTK_IS_ENTRY(widget)) {
        // Set ellipsization for the entry
        gtk_entry_set_width_chars(GTK_ENTRY(widget), 20);  // Adjust this value as needed
        gtk_entry_set_max_width_chars(GTK_ENTRY(widget), 20);  // Adjust this value as needed
        // Create a PangoLayout for the entry and set ellipsization
        PangoLayout* layout = gtk_entry_get_layout(GTK_ENTRY(widget));
        if (layout) {
            pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        } else {
            g_warning("Unable to get PangoLayout from GtkEntry");
        }
    } else {
        g_warning("Expected GtkEntry, but got %s", G_OBJECT_TYPE_NAME(widget));
    }
#endif
}

int PresetComboBox::update_ams_color()
{
    if (m_filament_idx < 0) return -1;
    int idx = selected_ams_filament();

    auto& filaments = wxGetApp().preset_bundle->machine_filaments;
    const ConnectMachineInfo* machineInfo = nullptr;
    if (idx >= 0)
    {
        const std::vector<ConnectMachineInfo>& machineInfoList = wxGetApp().preset_bundle->m_connect_machine_info_list;
        const size_t machineInfoIndex = static_cast<size_t>(idx);
        if (machineInfoIndex < machineInfoList.size())
            machineInfo = &machineInfoList[machineInfoIndex];
    }

    int real_idx = -1;
    if (machineInfo != nullptr)
    {
        real_idx = machineInfo->index;
    }
    else if (idx >= 0)
    {
        int tmp = idx;
        for (auto iter = filaments.begin(); iter != filaments.end(); ++iter) {
            if (tmp == 0) {
                real_idx = iter->first;
                break;
            }

            tmp--;
        }
    }
    

    auto& filament_extruder_map = wxGetApp().app_config->get_filament_extruder_map_ref();
    if (real_idx >= 0) {
        filament_extruder_map[m_filament_idx] = real_idx;
    } else {
        if (filament_extruder_map.count(m_filament_idx)) {
            filament_extruder_map.erase(m_filament_idx);
        }
    }

    std::string color;
    std::string ctype;                    // ORCA: filament colour type reported by the AMS
    std::vector<std::string> colors;      // ORCA: filament_multi_colour values reported by the AMS
    std::vector<std::string> multiColors; // Snapmaker: FilamentColorUtils multi colours
    FilamentColorMode colorMode = FilamentColorMode::Segment;
    if (idx < 0)
    {
        // ORCA: The combo displays the preset alias while
        // the stored preset name usually carries a printer suffix. Resolving with the raw display
        // value via find_preset() fails for such presets, so this returned early and the
        // filament color swatch (clr_picker) kept showing the previous color. Prefer the
        // internal preset name stored per item, then fall back to alias resolution.
        std::string name;
        if (m_last_selected >= 0) {
            wxString stored = GetItemAlias(m_last_selected);
            if (!stored.empty())
                name = Preset::remove_suffix_modified(stored.ToUTF8().data());
        }
        if (name.empty())
            name = m_collection->get_preset_name_by_alias(Preset::remove_suffix_modified(GetValue().ToUTF8().data()));
        auto *preset = m_collection->find_preset(name);
        if (preset)
            color = preset->config.opt_string("default_filament_colour", 0u);
        if (color.empty()) return -1;
    }
    else if (machineInfo != nullptr)
    {
        color = machineInfo->color_info;
        multiColors = machineInfo->multiColors;
        colorMode = machineInfo->colorMode;
        if (color.empty() && !multiColors.empty())
            color = multiColors.front();
    }
    else
    {
        if (wxGetApp().preset_bundle->machine_filaments.size() > 0)
        {
            auto iter = wxGetApp().preset_bundle->machine_filaments.begin();
            for (size_t i = 0; iter != wxGetApp().preset_bundle->machine_filaments.end() && i < idx; ++i) {
                ++iter;
            }
            if (iter == wxGetApp().preset_bundle->machine_filaments.end())
            {
                return -1;
            }
            color = iter->second.second;
        }
        else
        {
            auto& ams_list = wxGetApp().preset_bundle->filament_ams_list;
            auto  iter     = ams_list.find(idx);
            if (iter == ams_list.end())
            {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": ams %1% out of range %2%") % idx % ams_list.size();
                return -1;
            }
            color = iter->second.opt_string("filament_colour", 0u);
            // ORCA: the AMS also reports a colour type and a multi-colour pack.
            ctype = iter->second.opt_string("filament_colour_type", 0u);
            if (const auto *multi = iter->second.opt<ConfigOptionStrings>("filament_multi_colour"); multi != nullptr)
                colors = multi->values;
        }
    }
    const std::string normalizedColor = FilamentColorUtils::NormalizeHexColor(color, "#26A69A");
    if (machineInfo != nullptr && multiColors.empty() && !normalizedColor.empty())
        multiColors.emplace_back(normalizedColor);
    int normalizedMode = FilamentColorModeToConfig(FilamentColorMode::Segment);
    std::string joinedMultiColors;
    if (machineInfo != nullptr)
    {
        joinedMultiColors = FilamentColorUtils::JoinMultiColors(multiColors);
        if (multiColors.size() > 1)
            normalizedMode = FilamentColorModeToConfig(colorMode);
    }

    DynamicPrintConfig* cfg = &wxGetApp().preset_bundle->project_config;
    const size_t targetSize = static_cast<size_t>(m_filament_idx) + 1;
    const size_t index = static_cast<size_t>(m_filament_idx);
    ConfigOptionStrings* filamentColors = CloneStringOption(*cfg, "filament_colour");
    ResizeStrings(filamentColors, targetSize);

    filamentColors->values[index] = normalizedColor.empty() ? color : normalizedColor;
    DynamicPrintConfig new_cfg;
    new_cfg.set_key_value("filament_colour", filamentColors);

    // ORCA: colour type + multi-colour pack. "filament_multi_colour" (Orca, space separated
    // string) is a different key from Snapmaker's "filament_multi_colors" written below.
    ConfigOptionStrings* colorTypes = CloneStringOption(*cfg, "filament_colour_type");
    ConfigOptionStrings* colorPack  = CloneStringOption(*cfg, "filament_multi_colour");
    ResizeStrings(colorTypes, targetSize);
    ResizeStrings(colorPack, targetSize);
    colorTypes->values[index] = ctype;
    std::string color_str = ""; // Translate multi color info to config storage format
    for (auto &c : colors) {
        if (c.empty()) continue;
        color_str += c + " ";
    }
    if (color_str.empty()) color_str = filamentColors->values[index];
    else color_str.erase(color_str.size() - 1);
    colorPack->values[index] = color_str;
    new_cfg.set_key_value("filament_colour_type", colorTypes);
    new_cfg.set_key_value("filament_multi_colour", colorPack);

    if (machineInfo != nullptr)
    {
        ConfigOptionStrings* filamentMultiColors = CloneStringOption(*cfg, "filament_multi_colors");
        ConfigOptionInts* filamentColourModes = CloneIntOption(*cfg, "filament_colour_mode");
        ResizeStrings(filamentMultiColors, targetSize);
        ResizeInts(filamentColourModes, targetSize);

        filamentMultiColors->values[index] = joinedMultiColors;
        filamentColourModes->values[index] = normalizedMode;
        new_cfg.set_key_value("filament_multi_colors", filamentMultiColors);
        new_cfg.set_key_value("filament_colour_mode", filamentColourModes);
    }
    cfg->apply(new_cfg);
    wxGetApp().plater()->on_config_change(new_cfg);
    //trigger the filament color changed
    wxCommandEvent *evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
    evt->SetInt(m_filament_idx);
    wxQueueEvent(wxGetApp().plater(), evt);
    return idx;
}

wxColor PresetComboBox::different_color(wxColor const &clr)
{
    if (clr.GetLuminance() < 0.51) return *wxWHITE;
    return *wxBLACK;
}

wxString PresetComboBox::get_tooltip(const Preset &preset)
{
    wxString tooltip = from_u8(preset.name);
    
    // Add filament notes if available for filament presets
    if (m_type == Preset::TYPE_FILAMENT) {
        const DynamicConfig* config = &preset.config;
        Tab* tab = wxGetApp().get_tab(m_type);
        if (tab && tab->current_preset_is_dirty() && tab->get_presets()->get_selected_preset().name == preset.name) {
            config = tab->get_config();
        }

        if (config->has("filament_notes")) {
            const ConfigOptionStrings* notes_opt = config->option<ConfigOptionStrings>("filament_notes");
            if (notes_opt && !notes_opt->values.empty() && !notes_opt->values[0].empty()) {
                std::string notes = notes_opt->values[0];
                // Truncate if longer than 200 characters
                if (notes.length() > 200) {
                    notes = notes.substr(0, 197) + "...";
                }
                tooltip += "\n" + from_u8(notes);
            }
        }
    }
    
    // BBS: FIXME
#if 0
    if (m_type == Preset::TYPE_FILAMENT) {
        int temperature[4] = { 0,0,0,0 };
        if (preset.config.has("nozzle_temperature_initial_layer")) //get the nozzle_temperature_initial_layer
            temperature[0] = preset.config.opt_int("nozzle_temperature_initial_layer", 0);
        if (preset.config.has("nozzle_temperature")) //get the nozzle temperature
            temperature[1] = preset.config.opt_int("nozzle_temperature", 0);
        if (preset.config.has("bed_temperature_initial_layer")) //get the bed_temperature_initial_layer
            temperature[2] = preset.config.opt_int("bed_temperature_initial_layer", 0);
        if (preset.config.has("bed_temperature")) //get the bed_temperature
            temperature[3] = preset.config.opt_int("bed_temperature", 0);

        tooltip += wxString::Format("\nNozzle First Layer:%d, Other Layer:%d\n Bed First Layer:%d, Other Layers:%d",
            temperature[0], temperature[1], temperature[2], temperature[3]);
    }
#endif
    return tooltip;
}

wxString PresetComboBox::get_preset_item_name(unsigned int index)
{
    if (m_type == Preset::TYPE_PRINTER) {
        int idx = selected_connected_printer();
        if (idx < 0) {
            m_selected_dev_id.clear();
            return GetString(index);
        }
        else {
            DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
            if (!dev) {
                assert(false);
                m_selected_dev_id.clear();
                return GetString(index);
            }

            std::map<std::string, MachineObject *> machine_list = dev->get_my_machine_list();
            if (machine_list.empty()) {
                assert(false);
                m_selected_dev_id.clear();
                return GetString(index);
            }

            auto iter = machine_list.begin();
            std::advance(iter, idx);
            if (iter != machine_list.end()) {
                m_selected_dev_id = iter->first;
                Preset* machine_preset = get_printer_preset(iter->second);
                if (machine_preset) {
                    return from_u8(machine_preset->name);
                }
            }
        }
    }

    m_selected_dev_id.clear();
    return GetString(index);
}

wxString PresetComboBox::get_preset_name(const Preset & preset)
{
    return from_u8(preset.name/* + suffix(preset)*/);
}

void PresetComboBox::update(std::string select_preset_name)
{
    Freeze();
    Clear();
    invalidate_selection();

    const std::deque<Preset>& presets = m_collection->get_presets();

    std::map<wxString, std::pair<wxBitmap*, bool>>  nonsys_presets;
    std::map<wxString, wxBitmap*>                   incomp_presets;

    wxString selected = "";
    if (!presets.front().is_visible)
        set_label_marker(Append(_L("System presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));

    for (size_t i = presets.front().is_visible ? 0 : m_collection->num_default_presets(); i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        if (!m_show_all && (!preset.is_visible || !preset.is_compatible))
            continue;

        // marker used for disable incompatible printer models for the selected physical printer
        bool is_enabled = m_type == Preset::TYPE_PRINTER && printer_technology != ptAny ? preset.printer_technology() == printer_technology : true;
        if (select_preset_name.empty() && is_enabled)
            select_preset_name = preset.name;

        wxBitmap* bmp = get_bmp(preset);
        assert(bmp);

        if (!is_enabled)
            incomp_presets.emplace(get_preset_name(preset), bmp);
        else if (preset.is_default || preset.is_system)
        {
            Append(get_preset_name(preset), *bmp);
            validate_selection(preset.name == select_preset_name);
        }
        else
        {
            nonsys_presets.emplace(get_preset_name(preset), std::pair<wxBitmap*, bool>(bmp, is_enabled));
            if (preset.name == select_preset_name || (select_preset_name.empty() && is_enabled))
                selected = get_preset_name(preset);
        }
        if (i + 1 == m_collection->num_default_presets())
            set_label_marker(Append(_L("System presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
    }
    if (!nonsys_presets.empty())
    {
        set_label_marker(Append(_L("User presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, std::pair<wxBitmap*, bool>>::iterator it = nonsys_presets.begin(); it != nonsys_presets.end(); ++it) {
            int item_id = Append(it->first, *it->second.first);
            bool is_enabled = it->second.second;
            if (!is_enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->first == selected);
        }
    }
    if (!incomp_presets.empty())
    {
        set_label_marker(Append(_L("Incompatible presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, wxBitmap*>::iterator it = incomp_presets.begin(); it != incomp_presets.end(); ++it) {
            set_label_marker(Append(it->first, *it->second), LABEL_ITEM_DISABLED);
        }
    }

    update_selection();
    Thaw();
}

bool PresetComboBox::is_selected_printer_model()
{
    auto selected_item = this->GetSelection();
    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    return marker == LABEL_ITEM_PRINTER_MODELS;
}

void PresetComboBox::show_all(bool show_all)
{
    m_show_all = show_all;
    update();
}

void PresetComboBox::update()
{
    this->update(into_u8(this->GetString(this->GetSelection())));
}

void PresetComboBox::update_from_bundle()
{
    this->update(m_collection->get_selected_preset().name);
}

void PresetComboBox::add_connected_printers(std::string selected, bool alias_name)
{
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return;

    std::map<std::string, MachineObject *> machine_list = dev->get_my_machine_list();
    if (machine_list.empty())
        return;

    set_label_marker(Append(_L("My Printer"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
    m_first_printer_idx = GetCount();
    for (auto iter = machine_list.begin(); iter != machine_list.end(); ++iter) {
        Preset* printer_preset = get_printer_preset(iter->second);
        if (!printer_preset)
            continue;
        printer_preset->is_visible = true;
        auto printer_model = printer_preset->config.opt_string("printer_model");
        boost::replace_all(printer_model, "Bambu Lab ", "");
        auto text = iter->second->get_dev_name() + " (" + printer_model + ")";
        int  item_id = Append(from_u8(text), wxNullBitmap, &m_first_printer_idx + std::distance(machine_list.begin(), iter));
        validate_selection(m_selected_dev_id == iter->first);
    }
    m_last_printer_idx = GetCount();
}

int PresetComboBox::selected_connected_printer() const
{
    if (m_first_printer_idx && m_last_selected >= m_first_printer_idx && m_last_selected < m_last_printer_idx) {
        return reinterpret_cast<int *>(GetClientData(m_last_selected)) - &m_first_printer_idx;
    }
    return -1;
}

bool PresetComboBox::add_ams_filaments(std::string selected, bool alias_name)
{
    bool selected_in_ams      = false;
    bool is_bbl_vendor_preset = m_preset_bundle->is_bbl_vendor();
    if (is_bbl_vendor_preset && !m_preset_bundle->filament_ams_list.empty()) {
        // When a filament track switch is installed and calibrated, every AMS filament is reachable
        // from both extruders, so present one deduplicated group instead of the Left/Right split.
        bool fila_switch_ready = wxGetApp().sidebar().is_fila_switch_ready();
        bool dual_extruder   = (m_preset_bundle->filament_ams_list.begin()->first & 0x10000) == 0;
        if (fila_switch_ready)
            set_label_marker(Append(_L("AMS filaments"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        else
            set_label_marker(Append(dual_extruder ? _L("Left filaments") : _L("AMS filament"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        m_first_ams_filament = GetCount();
        auto &filaments      = m_collection->get_presets();

        int icon_width = 24;
        for (auto &entry : m_preset_bundle->filament_ams_list) {
            auto &      tray        = entry.second;
            auto  name = tray.opt_string("tray_name", 0u);
            if (name.size() > 3)
                icon_width = 32;
        }

        // Deduplicate by (tray_name, filament_id) so a filament shared by both extruders is
        // listed once when the switch is ready. Uses Orca's tray naming/lookup, not BBS's.
        std::set<std::pair<std::string, std::string>> added_filaments;

        for (auto &entry : m_preset_bundle->filament_ams_list) {
            if (!fila_switch_ready && dual_extruder && (entry.first & 0x10000)) {
                dual_extruder = false;
                set_label_marker(Append(_L("Right filaments"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
            }
            auto &      tray        = entry.second;
            std::string filament_id = tray.opt_string("filament_id", 0u);
            auto        name        = tray.opt_string("tray_name", 0u);
            if (filament_id.empty()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":  %1% 's filament_id is empty.") % name;
                continue;
            }
            if (fila_switch_ready) {
                // skip the external spool and collapse duplicates shared across both extruders
                if (name == "Ext")
                    continue;
                if (!added_filaments.insert(std::make_pair(name, filament_id)).second)
                    continue;
            }
            auto iter = std::find_if(filaments.begin(), filaments.end(),
                [&filament_id, this](auto &f) { return f.is_compatible && m_collection->get_preset_base(f) == &f && f.filament_id == filament_id; });
            if (iter == filaments.end()) {
                auto filament_type = tray.opt_string("filament_type", 0u);
                if (!filament_type.empty()) {
                    filament_type = "Generic " + filament_type;
                    iter          = std::find_if(filaments.begin(), filaments.end(),
                                        [&filament_type](auto &f) { return f.is_compatible && f.is_system && boost::algorithm::starts_with(f.name, filament_type); });
                }
            }
            if (iter == filaments.end()) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": filament_id %1% not found or system or compatible") % filament_id;
                continue;
            }
            const_cast<Preset&>(*iter).is_visible = true;
            auto color = tray.opt_string("filament_colour", 0u);
            auto multi_color = tray.opt<ConfigOptionStrings>("filament_multi_colour")->values;
            wxBitmap bmp(*get_extruder_color_icon(color, name, icon_width, 16));
            auto text = get_preset_name(*iter);
            int      item_id = Append(text, bmp.ConvertToImage(), &m_first_ams_filament + entry.first);
            SetFlag(GetCount() - 1, (int) FilamentAMSType::FROM_AMS);
            if (text == selected) {
                DynamicPrintConfig *cfg    = &wxGetApp().preset_bundle->project_config;
                if (cfg) {
                    auto colors = static_cast<ConfigOptionStrings *>(cfg->option("filament_colour")->clone());
                    if (m_filament_idx < colors->values.size()) {
                        auto cur_color = colors->values[m_filament_idx];
                        if (color == cur_color) {
                            selected_in_ams = true;
                        }
                    }
                }
            }
            //validate_selection(id->value == selected); // can not select
        }
        m_last_ams_filament = GetCount();
    }
    return selected_in_ams;
}

int PresetComboBox::selected_ams_filament() const
{
    if (m_first_ams_filament && m_last_selected >= m_first_ams_filament && m_last_selected < m_last_ams_filament) {
        return reinterpret_cast<int *>(GetClientData(m_last_selected)) - &m_first_ams_filament;
    }
    return -1;
}

void PresetComboBox::msw_rescale()
{
    m_em_unit = em_unit(this);
    Rescale();

    m_bitmapIncompatible.msw_rescale();
    m_bitmapCompatible.msw_rescale();

    // parameters for an icon's drawing
    fill_width_height();

    // update the control to redraw the icons
    update();
}

void PresetComboBox::sys_color_changed()
{
    wxGetApp().UpdateDarkUI(this);
    msw_rescale();
}

void PresetComboBox::fill_width_height()
{
    // To avoid asserts, each added bitmap to wxBitmapCombobox should be the same size, so
    // set a bitmap's height to m_bitmapCompatible->GetHeight() and norm_icon_width to m_bitmapCompatible->GetWidth()
    icon_height     = m_bitmapCompatible.GetBmpHeight();
    norm_icon_width = m_bitmapCompatible.GetBmpWidth();

    /* It's supposed that standard size of an icon is 16px*16px for 100% scaled display.
    * So set sizes for solid_colored icons used for filament preset
    * and scale them in respect to em_unit value
    */
    const float scale_f = (float)m_em_unit * 0.1f;

    thin_icon_width = lroundf(8 * scale_f);          // analogue to 8px;
    wide_icon_width = norm_icon_width + thin_icon_width;

    space_icon_width      = lroundf(2 * scale_f);
    thin_space_icon_width = lroundf(4 * scale_f);
    wide_space_icon_width = lroundf(6 * scale_f);
}

wxString PresetComboBox::separator(const std::string& label)
{
    return wxString::FromUTF8(separator_head()) + _(label) + wxString::FromUTF8(separator_tail());
}

wxBitmap* PresetComboBox::get_bmp(  std::string bitmap_key, bool wide_icons, const std::string& main_icon_name,
                                    bool is_compatible/* = true*/, bool is_system/* = false*/, bool is_single_bar/* = false*/,
                                    const std::string& filament_rgb/* = ""*/, const std::string& extruder_rgb/* = ""*/, const std::string& material_rgb/* = ""*/)
{
    // BBS: no icon
#if 1
    static wxBitmap bmp;
    return &bmp;
#else
    // If the filament preset is not compatible and there is a "red flag" icon loaded, show it left
    // to the filament color image.
    if (wide_icons)
        bitmap_key += is_compatible ? ",cmpt" : ",ncmpt";

    bitmap_key += is_system ? ",syst" : ",nsyst";
    bitmap_key += ",h" + std::to_string(icon_height);
    bool dark_mode = wxGetApp().dark_mode();
    if (dark_mode)
        bitmap_key += ",dark";
    bitmap_key += material_rgb;

    wxBitmap* bmp = bitmap_cache().find(bitmap_key);
    if (bmp == nullptr) {
        // Create the bitmap with color bars.
        std::vector<wxBitmap> bmps;
        if (wide_icons)
            // Paint a red flag for incompatible presets.
            bmps.emplace_back(is_compatible ? bitmap_cache().mkclear(norm_icon_width, icon_height) : m_bitmapIncompatible.bmp());

        if (m_type == Preset::TYPE_FILAMENT && !filament_rgb.empty())
        {
            // BBS
            // Paint a lock at the system presets.
            bmps.emplace_back(bitmap_cache().mkclear(space_icon_width, icon_height));
        }
        else
        {
            // BBS
#if 0
            // Paint the color bars.
            bmps.emplace_back(bitmap_cache().mkclear(thin_space_icon_width, icon_height));
            if (m_type == Preset::TYPE_SLA_MATERIAL)
                bmps.emplace_back(create_scaled_bitmap(main_icon_name, this, 16, false, material_rgb));
            else
                bmps.emplace_back(create_scaled_bitmap(main_icon_name));
#endif
            // Paint a lock at the system presets.
            bmps.emplace_back(bitmap_cache().mkclear(wide_space_icon_width, icon_height));
        }
        bmps.emplace_back(is_system ? create_scaled_bitmap("unlock_normal") : bitmap_cache().mkclear(norm_icon_width, icon_height));
        bmp = bitmap_cache().insert(bitmap_key, bmps);
    }

    return bmp;
#endif
}

wxBitmap *PresetComboBox::get_bmp(Preset const &preset)
{
    static wxBitmap sbmp;
    if (m_type == Preset::TYPE_FILAMENT) {
        Preset const & preset2 = &m_collection->get_selected_preset() == &preset ? m_collection->get_edited_preset() : preset;
        wxString color = preset2.config.opt_string("default_filament_colour", 0);
        wxColour clr(color);
        if (clr.IsOk()) {
            std::string bitmap_key = "default_filament_colour_" + color.ToStdString();
            wxBitmap *bmp        = bitmap_cache().find(bitmap_key);
            if (bmp == nullptr) {
                wxImage img(16, 16);
                if (clr.Red() > 224 && clr.Blue() > 224 && clr.Green() > 224) {
                    img.SetRGB(wxRect({0, 0}, img.GetSize()), 128, 128, 128);
                    img.SetRGB(wxRect({1, 1}, img.GetSize() - wxSize{2, 2}), clr.Red(), clr.Green(), clr.Blue());
                } else {
                    img.SetRGB(wxRect({0, 0}, img.GetSize()), clr.Red(), clr.Green(), clr.Blue());
                }
                bmp = new wxBitmap(img);
                bmp = bitmap_cache().insert(bitmap_key, *bmp);
            }
            return bmp;
        }
    }
    return &sbmp;
}

wxBitmap *PresetComboBox::get_bmp(std::string        bitmap_key,
                                  const std::string &main_icon_name,
                                  const std::string &next_icon_name,
                                    bool is_enabled/* = true*/, bool is_compatible/* = true*/, bool is_system/* = false*/)
{
    // BBS: no icon
#if 1
    static wxBitmap bmp;
    return &bmp;
#else
    bitmap_key += !is_enabled ? "_disabled" : "";
    bitmap_key += is_compatible ? ",cmpt" : ",ncmpt";
    bitmap_key += is_system ? ",syst" : ",nsyst";
    bitmap_key += ",h" + std::to_string(icon_height);
    if (wxGetApp().dark_mode())
        bitmap_key += ",dark";

    wxBitmap* bmp = bitmap_cache().find(bitmap_key);
    if (bmp == nullptr) {
        // Create the bitmap with color bars.
        std::vector<wxBitmap> bmps;
        bmps.emplace_back(m_type == Preset::TYPE_PRINTER ? create_scaled_bitmap(main_icon_name, this, 16, !is_enabled) :
                          is_compatible ? m_bitmapCompatible.bmp() : m_bitmapIncompatible.bmp());
        // Paint a lock at the system presets.
        bmps.emplace_back(is_system ? create_scaled_bitmap(next_icon_name, this, 16, !is_enabled) : bitmap_cache().mkclear(norm_icon_width, icon_height));
        bmp = bitmap_cache().insert(bitmap_key, bmps);
    }

    return bmp;
#endif
}

bool PresetComboBox::is_selected_physical_printer()
{
    auto selected_item = this->GetSelection();
    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    return marker == LABEL_ITEM_PHYSICAL_PRINTER;
}


bool PresetComboBox::selection_is_changed_according_to_physical_printers()
{
    if (m_type != Preset::TYPE_PRINTER || !is_selected_physical_printer())
        return false;

    PhysicalPrinterCollection& physical_printers = m_preset_bundle->physical_printers;

    std::string selected_string = this->GetString(this->GetSelection()).ToUTF8().data();

    std::string old_printer_full_name, old_printer_preset;
    if (physical_printers.has_selection()) {
        old_printer_full_name = physical_printers.get_selected_full_printer_name();
        old_printer_preset = physical_printers.get_selected_printer_preset_name();
    }
    else
        old_printer_preset = m_collection->get_edited_preset().name;
    // Select related printer preset on the Printer Settings Tab
    physical_printers.select_printer(selected_string);
    std::string preset_name = physical_printers.get_selected_printer_preset_name();

    // if new preset wasn't selected, there is no need to call update preset selection
    if (old_printer_preset == preset_name) {
        // we need just to update according Plater<->Tab PresetComboBox
        if (dynamic_cast<PlaterPresetComboBox*>(this)!=nullptr) {
            wxGetApp().get_tab(m_type)->update_preset_choice();
            // Synchronize config.ini with the current selections.
            m_preset_bundle->export_selections(*wxGetApp().app_config);
        }
        else if (dynamic_cast<TabPresetComboBox*>(this)!=nullptr)
            wxGetApp().sidebar().update_presets(m_type);

        this->update();
        return true;
    }

    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (tab)
        tab->select_preset(preset_name, false, old_printer_full_name);
    return true;
}

// ---------------------------------
// ***  PlaterPresetComboBox  ***
// ---------------------------------

PlaterPresetComboBox::PlaterPresetComboBox(wxWindow *parent, Preset::Type preset_type) :
    PresetComboBox(parent, preset_type, wxSize(25 * wxGetApp().em_unit(), 30 * wxGetApp().em_unit() / 10)),
    m_connection_icon(this, "monitor_signal_strong", 16),
    m_machine_connecting_icon(this, "monitor_machine_working", 16),
    m_edit_icon(this, "edit", 16)
{
    GetDropDown().SetUseContentWidth(true,true);
    
    // 对于打印机类型的combo box，绑定自定义绘制和鼠标事件
    if (m_type == Preset::TYPE_PRINTER) {
        Bind(wxEVT_PAINT, &PlaterPresetComboBox::paintEvent, this);
        Bind(wxEVT_LEFT_DOWN, &PlaterPresetComboBox::onMouseLeftDown, this);
        Bind(wxEVT_LEFT_UP, &PlaterPresetComboBox::onMouseLeftUp, this);
        Bind(wxEVT_ENTER_WINDOW, &PlaterPresetComboBox::onMouseEnter, this);
        Bind(wxEVT_LEAVE_WINDOW, &PlaterPresetComboBox::onMouseLeave, this);
        Bind(wxEVT_MOTION, &PlaterPresetComboBox::onMouseMove, this);
    }

    if (m_type == Preset::TYPE_FILAMENT)
    {
        // BBS: not show color picker
#if 0
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            const Preset* selected_preset = m_collection->find_preset(m_preset_bundle->filament_presets[m_filament_idx]);
            // Wide icons are shown if the currently selected preset is not compatible with the current printer,
            // and red flag is drown in front of the selected preset.
            bool          wide_icons = selected_preset && !selected_preset->is_compatible;
            float scale = m_em_unit*0.1f;

            int shifl_Left = wide_icons ? int(scale * 16 + 0.5) : 0;
#if defined(wxBITMAPCOMBOBOX_OWNERDRAWN_BASED)
            shifl_Left  += int(scale * 4 + 0.5f); // IMAGE_SPACING_RIGHT = 4 for wxBitmapComboBox -> Space left of image
#endif
            int icon_right_pos = shifl_Left + int(scale * (24+4) + 0.5);
            int mouse_pos = event.GetLogicalPosition(wxClientDC(this)).x;
            if (mouse_pos < shifl_Left || mouse_pos > icon_right_pos ) {
                // Let the combo box process the mouse click.
                event.Skip();
                return;
            }

            // BBS
            // Swallow the mouse click and open the color picker.
            //ChangeExtruderColor();
        });
#endif
    }

    // BBS
    if (m_type == Preset::TYPE_FILAMENT) {
        clr_picker = new wxBitmapButton(parent, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(20), FromDIP(20)), wxBU_EXACTFIT | wxBU_AUTODRAW | wxBORDER_NONE);
        clr_picker->SetBitmapMargins(0, 0);
        clr_picker->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        clr_picker->SetToolTip(_L("Click to select filament color"));
#ifdef __WXGTK__
        RemoveButtonBorder(clr_picker);
#endif
        clr_picker->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            // ORCA: Bambu official filaments keep Orca's filament picker; every other filament
            // goes through the Snapmaker filament colour library dialog, which itself falls back
            // to the plain colour picker when the filament is not in the library.
            auto fila_type = Preset::remove_suffix_modified(GetValue().ToUTF8().data());
            if (!boost::algorithm::starts_with(fila_type, "Bambu")) {
                ChangeExtruderColor();
                return;
            }

            // Get filament_id from filament_presets
            const std::string& preset_name = m_preset_bundle->filament_presets[m_filament_idx];
            const Preset* selected_preset = m_collection->find_preset(preset_name);
            wxString fila_id = selected_preset ? wxString::FromUTF8(selected_preset->filament_id) : "GFA00";
            EncodedFilamentColor fila_color = get_cur_color_info();

            // Show filament picker dialog
            FilamentPickerDialog dialog(this, fila_id, fila_color, fila_type);

            if (!dialog.IsDataLoaded()) {
                // If FilamentPicker fails, fallback to default color picker
                show_default_color_picker();
            } else if (dialog.ShowModal() == wxID_OK) {
                // Get selected filament color data
                EncodedFilamentColor fila_color = dialog.GetSelectedFilamentColor();

                // Check if we have valid color data
                if (!fila_color.m_colors.empty()) {
                    // Convert to storage format
                    std::vector<std::string> colors;
                    for (const wxColour& color : fila_color.m_colors) {
                        colors.push_back(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
                    }

                    bool is_gradient = (fila_color.m_color_type == EncodedFilamentColor::ColorType::GRADIENT_CLR);
                    this->sync_colour_config(colors, is_gradient);
                } else {
                    // Fallback to basic color if no FilamentColor data
                    wxColour selected_color = dialog.GetSelectedColour();
                    if (selected_color.IsOk()) {
                        std::vector<std::string> color = {selected_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString()};
                        this->sync_colour_config(color, false);
                    }
                }
            }
            wxCommandEvent *evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
            evt->SetInt(m_filament_idx);
            wxQueueEvent(wxGetApp().plater(), evt);
        });
    }
    else {
        edit_btn = new ScalableButton(parent, wxID_ANY, "cog");
        edit_btn->SetToolTip(_L("Click to edit preset"));

        edit_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent)
            {
                // In a case of a physical printer, for its editing open PhysicalPrinterDialog
                if (m_type == Preset::TYPE_PRINTER
#ifdef __linux__
                    // To edit extruder color from the sidebar
                    || m_type == Preset::TYPE_FILAMENT
#endif //__linux__
                    )
                    show_edit_menu();
                else
                    switch_to_tab();
            });
#ifdef __linux__
        edit_btn->Hide();
#endif //__linux__
    }
}

PlaterPresetComboBox::~PlaterPresetComboBox()
{
    if (edit_btn)
        edit_btn->Destroy();

    // BBS.
    if (clr_picker)
        clr_picker->Destroy();
}

static void run_wizard(ConfigWizard::StartPage sp)
{
    wxGetApp().run_wizard(ConfigWizard::RR_USER, sp);
}

void PlaterPresetComboBox::OnSelect(wxCommandEvent &evt)
{
    auto selected_item = evt.GetSelection();

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX) {
        this->SetSelection(m_last_selected);
        if (LABEL_ITEM_WIZARD_ADD_PRINTERS == marker) {
            evt.Skip();
            return;
        }
        evt.StopPropagation();
        if (marker == LABEL_ITEM_MARKER || marker == LABEL_ITEM_DISABLED)
            return;
        //if (marker == LABEL_ITEM_WIZARD_PRINTERS)
        //    show_add_menu();
        //else {
            ConfigWizard::StartPage sp = ConfigWizard::SP_WELCOME;
            switch (marker) {
            case LABEL_ITEM_WIZARD_PRINTERS: sp = ConfigWizard::SP_PRINTERS; break;
            case LABEL_ITEM_WIZARD_FILAMENTS: sp = ConfigWizard::SP_FILAMENTS; break;
            case LABEL_ITEM_WIZARD_MATERIALS: sp = ConfigWizard::SP_MATERIALS; break;
            default: break;
            }
            wxTheApp->CallAfter([sp]() { run_wizard(sp); });
        //}
        return;
    } else if (marker == LABEL_ITEM_PHYSICAL_PRINTER ||  selected_item >= 0 || m_collection->current_is_dirty()) {
        m_last_selected = selected_item;
        if (m_type == Preset::TYPE_FILAMENT)
            update_ams_color();
    }

    evt.Skip();
}

void PlaterPresetComboBox::update_badge_according_flag() {
    auto selection   = GetSelection();
    auto select_flag = GetFlag(selection);
    auto ok          = select_flag == (int) PresetComboBox::FilamentAMSType::FROM_AMS;
    ShowBadge(ok);
}

bool PlaterPresetComboBox::switch_to_tab()
{
    Tab* tab = wxGetApp().get_tab(m_type);
    if (!tab)
        return false;

    const Preset* selected_filament_preset = nullptr;
    if (m_type == Preset::TYPE_FILAMENT)
    {
        const std::string& selected_preset = GetString(GetSelection()).ToUTF8().data();
        if (!boost::algorithm::starts_with(selected_preset, Preset::suffix_modified()))
        {
            const std::string& preset_name = wxGetApp().preset_bundle->filaments.get_preset_name_by_alias(selected_preset);
            if (wxGetApp().get_tab(m_type)->select_preset(preset_name))
                wxGetApp().get_tab(m_type)->get_combo_box()->set_filament_idx(m_filament_idx);
            else {
                return false;
            }
        }
    }

    /*
    if (int page_id = wxGetApp().tab_panel()->FindPage(tab); page_id != wxNOT_FOUND)
    {
        wxGetApp().tab_panel()->SetSelection(page_id);
        // Switch to Settings NotePad
        wxGetApp().mainframe->select_tab();

        //In a case of a multi-material printing, for editing another Filament Preset
        //it's needed to select this preset for the "Filament settings" Tab
        if (m_type == Preset::TYPE_FILAMENT && wxGetApp().extruders_edited_cnt() > 1)
        {
            const std::string& selected_preset = GetString(GetSelection()).ToUTF8().data();
            // Call select_preset() only if there is new preset and not just modified
            if (!boost::algorithm::ends_with(selected_preset, Preset::suffix_modified()))
            {
                const std::string& preset_name = wxGetApp().preset_bundle->filaments.get_preset_name_by_alias(selected_preset);
                wxGetApp().get_tab(m_type)->select_preset(preset_name);
            }
        }
    }
    */

    //BBS  Select NoteBook Tab params
    if (tab->GetParent() == wxGetApp().params_panel())
        wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    else {
        wxGetApp().params_dialog()->Popup();
        tab->OnActivate();
    }
    tab->restore_last_select_item();

    return true;
}

void PlaterPresetComboBox::ChangeExtruderColor()
{
    if (m_filament_idx < 0 || wxGetApp().preset_bundle == nullptr)
        return;

    const std::string filamentPresetName = CurrentFilamentPresetName();
    const std::string filamentBaseName = FilamentBaseName(filamentPresetName);
    if (!IsSnapmakerFilamentName(filamentBaseName))
    {
        SelectLegacyFilamentColor();
        return;
    }

    DynamicPrintConfig& config = wxGetApp().preset_bundle->project_config;
    const std::string currentColorHex = ConfigStringAt(config, "filament_colour", m_filament_idx);
    const std::string currentMultiColors = ConfigStringAt(config, "filament_multi_colors", m_filament_idx);
    const int currentModeValue = ConfigIntAt(config, "filament_colour_mode", m_filament_idx);

    FilamentColorLibrary& library = FilamentColorLibrary::Instance();
    FilamentColorInfo filament;
    const std::string filamentId = CurrentFilamentId();
    bool foundFilament = false;
    if (library.EnsureLoaded())
    {
        foundFilament = !filamentPresetName.empty() && library.FindFilamentByName(filamentPresetName, filament);
        if (!foundFilament)
            foundFilament = !filamentId.empty() && library.FindFilamentById(filamentId, filament) &&
                            FilamentMatchesPresetName(filament, filamentPresetName);
    }

    if (foundFilament && !filament.colors.empty())
    {
        const FilamentColorMode currentMode = FilamentColorModeFromConfig(currentModeValue);
        const FilamentColor currentColor = FilamentColor::FromMultiColors(currentMultiColors, currentMode, currentColorHex);
        FilamentColorDialog dialog(this, filament, currentColor);

        if (dialog.ShowModal() == wxID_OK)
        {
            ApplyFilamentColor(dialog.Selection());
        }
        return;
    }

    SelectLegacyFilamentColor();
}

void PlaterPresetComboBox::SelectLegacyFilamentColor()
{
    if (m_filament_idx < 0 || wxGetApp().preset_bundle == nullptr)
        return;

    DynamicPrintConfig& config = wxGetApp().preset_bundle->project_config;
    wxColour current_color(ConfigStringAt(config, "filament_colour", m_filament_idx));
    if (!current_color.IsOk())
        current_color = wxColour(0, 0, 0);

    m_clrData.SetColour(current_color);
    m_clrData.SetChooseFull(true);
    m_clrData.SetChooseAlpha(false);

    std::vector<std::string> custom_colors;
    if (wxGetApp().app_config != nullptr)
        custom_colors = wxGetApp().app_config->get_custom_color_from_config();

    const int custom_count = std::min(static_cast<int>(custom_colors.size()), CUSTOM_COLOR_COUNT);
    for (int i = 0; i < custom_count; ++i)
        m_clrData.SetCustomColour(i, string_to_wxColor(custom_colors[i]));

    wxColourDialog dialog(this, &m_clrData);
    dialog.SetTitle(_L("Please choose the filament color"));
    if (dialog.ShowModal() != wxID_OK)
        return;

    m_clrData = dialog.GetColourData();
    if (custom_colors.size() != CUSTOM_COLOR_COUNT)
        custom_colors.resize(CUSTOM_COLOR_COUNT);
    for (int i = 0; i < CUSTOM_COLOR_COUNT; ++i)
        custom_colors[i] = color_to_string(m_clrData.GetCustomColour(i));
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->save_custom_color_to_config(custom_colors);

    const std::string selected_color = m_clrData.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    ApplyFilamentColor(FilamentColor::FromMultiColors("", FilamentColorMode::Segment, selected_color));
}

void PlaterPresetComboBox::ApplyFilamentColor(const FilamentColor& colorData)
{
    if (m_filament_idx < 0 || wxGetApp().preset_bundle == nullptr)
        return;

    DynamicPrintConfig* config = &wxGetApp().preset_bundle->project_config;
    const size_t index = static_cast<size_t>(m_filament_idx);
    const size_t target_size = index + 1;

    const FilamentColor normalizedColor = FilamentColor::FromColors(colorData.colors, colorData.mode);
    const std::string normalizedPrimary = normalizedColor.PrimaryColor("#26A69A");
    const std::string multiColors = normalizedColor.ToMultiColorsString();
    const int normalizedMode = FilamentColorModeToConfig(normalizedColor.NormalizedMode());

    ConfigOptionStrings* filament_colors = CloneStringOption(*config, "filament_colour");
    ConfigOptionStrings* filament_multi_colors = CloneStringOption(*config, "filament_multi_colors");
    ConfigOptionInts* filament_colour_modes = CloneIntOption(*config, "filament_colour_mode");

    ResizeStrings(filament_colors, target_size);
    ResizeStrings(filament_multi_colors, target_size);
    ResizeInts(filament_colour_modes, target_size);

    filament_colors->values[index] = normalizedPrimary;
    filament_multi_colors->values[index] = multiColors;
    filament_colour_modes->values[index] = normalizedMode;

    if (wxGetApp().app_config != nullptr)
    {
        std::unordered_map<int, int>& filament_extruder_map = wxGetApp().app_config->get_filament_extruder_map_ref();
        if (filament_extruder_map.count(m_filament_idx))
            filament_extruder_map.erase(m_filament_idx);
    }

    DynamicPrintConfig new_config = *config;
    new_config.set_key_value("filament_colour", filament_colors);
    new_config.set_key_value("filament_multi_colors", filament_multi_colors);
    new_config.set_key_value("filament_colour_mode", filament_colour_modes);

    config->apply(new_config);
    wxGetApp().plater()->update_project_dirty_from_presets();
    if (wxGetApp().app_config != nullptr)
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    update();
    wxGetApp().plater()->on_config_change(new_config);

    wxCommandEvent* event = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
    event->SetInt(m_filament_idx);
    wxQueueEvent(wxGetApp().plater(), event);
}

std::string PlaterPresetComboBox::CurrentFilamentPresetName() const
{
    std::string presetName;
    if (m_preset_bundle != nullptr && m_filament_idx >= 0)
    {
        const size_t index = static_cast<size_t>(m_filament_idx);
        if (m_preset_bundle->filament_presets.size() > index)
            presetName = Preset::remove_suffix_modified(m_preset_bundle->filament_presets[index]);
    }

    if (presetName.empty() && GetSelection() != wxNOT_FOUND)
        presetName = Preset::remove_suffix_modified(GetString(GetSelection()).ToUTF8().data());

    if (!presetName.empty() && m_collection != nullptr)
        presetName = Preset::remove_suffix_modified(m_collection->get_preset_name_by_alias(presetName));

    return presetName;
}

std::string PlaterPresetComboBox::CurrentFilamentId() const
{
    if (m_preset_bundle == nullptr || m_collection == nullptr || m_filament_idx < 0)
        return {};

    const std::string presetName = CurrentFilamentPresetName();
    if (presetName.empty())
        return {};

    const Preset* preset = m_collection->find_preset(presetName, false, true);
    return preset != nullptr ? preset->filament_id : std::string();
}

void PlaterPresetComboBox::show_add_menu()
{
    wxMenu* menu = new wxMenu();

    append_menu_item(menu, wxID_ANY, _L("Add/Remove presets"), "",
        [](wxCommandEvent&) {
            wxTheApp->CallAfter([]() { run_wizard(ConfigWizard::SP_PRINTERS); });
        }, "menu_edit_preset", menu, []() { return true; }, wxGetApp().plater());

    wxGetApp().plater()->PopupMenu(menu);
}

void PlaterPresetComboBox::show_edit_menu()
{
    wxMenu* menu = new wxMenu();

    append_menu_item(menu, wxID_ANY, _L("Edit preset"), "",
        [this](wxCommandEvent&) { this->switch_to_tab(); }, "cog", menu, []() { return true; }, wxGetApp().plater());

#ifdef __linux__
    // To edit extruder color from the sidebar
    if (m_type == Preset::TYPE_FILAMENT) {
        append_menu_item(menu, wxID_ANY, _L("Change extruder color"), "",
            [this](wxCommandEvent&) { this->ChangeExtruderColor(); }, "blank_14", menu, []() { return true; }, wxGetApp().plater());
        wxGetApp().plater()->PopupMenu(menu);
        return;
    }
#endif //__linux__

    append_menu_item(menu, wxID_ANY, _L("Add/Remove presets"), "",
        [](wxCommandEvent&) {
            wxTheApp->CallAfter([]() { run_wizard(ConfigWizard::SP_PRINTERS); });
        }, "menu_edit_preset", menu, []() { return true; }, wxGetApp().plater());

    wxGetApp().plater()->PopupMenu(menu);
}

wxString PlaterPresetComboBox::get_preset_name(const Preset& preset)
{
    return from_u8(preset.label(false));
}

// Only the compatible presets are shown.
// If an incompatible preset is selected, it is shown as well.
void PlaterPresetComboBox::update()
{
    if (m_type == Preset::TYPE_FILAMENT &&
        (m_preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA ||
        m_preset_bundle->filament_presets.size() <= (size_t)m_filament_idx) )
        return;

    // Otherwise fill in the list from scratch.
    this->Freeze();
    this->Clear();
    invalidate_selection();

    const Preset* selected_filament_preset = nullptr;
    if (m_type == Preset::TYPE_FILAMENT)
    {
        // Snapmaker: multi-colour aware swatch (filament_multi_colors / filament_colour_mode).
        //unsigned char rgb[3];
        std::string filament_color = m_preset_bundle->project_config.opt_string("filament_colour", (unsigned int) m_filament_idx);
        wxColor clr(filament_color);
        const int picker_size = std::max(1, 20 * m_em_unit / 10);
        clr_picker->SetMinSize(wxSize(picker_size, picker_size));
        clr_picker->SetSize(picker_size, picker_size);
        clr_picker->SetBackgroundColour(clr);
        wxBitmap* color_bitmap = GetFilamentColorPickerBitmap(m_preset_bundle->project_config, m_filament_idx,
                                                             filament_color, FilamentColorPickerBitmapSize(clr_picker));
        if (color_bitmap != nullptr)
            clr_picker->SetBitmap(*color_bitmap);
#ifdef __WXOSX__
        clr_picker->SetLabel(clr_picker->GetLabel()); // Let setBezelStyle: be called
        clr_picker->Refresh();
#endif
        selected_filament_preset = m_collection->find_preset(m_preset_bundle->filament_presets[m_filament_idx]);
        if (!selected_filament_preset) {
            //can not find this filament, should be caused by project embedded presets, will be updated later
            Thaw();
            return;
        }
        //assert(selected_filament_preset);
    }

    bool has_selection = m_collection->get_selected_idx() != size_t(-1);
    const Preset* selected_preset = m_type == Preset::TYPE_FILAMENT ? selected_filament_preset : has_selection ? &m_collection->get_selected_preset() : nullptr;
    // Show wide icons if the currently selected preset is not compatible with the current printer,
    // and draw a red flag in front of the selected preset.
    bool wide_icons = selected_preset && !selected_preset->is_compatible;

    std::map<wxString, wxBitmap*> nonsys_presets;
    //BBS: add project embedded presets logic
    std::map<wxString, wxBitmap*>  project_embedded_presets;
    // ORCA: add bundle presets
    std::map<wxString, wxBitmap*> bundle_presets;
    std::map<wxString, wxBitmap *> system_presets;
    std::map<wxString, wxBitmap*>   machine_filament_presets;
    std::map<wxString, wxBitmap *>  uncompatible_presets;
    std::unordered_set<std::string> system_printer_models;
    std::map<wxString, wxString>   preset_descriptions;
    std::map<wxString, std::string> preset_filament_vendors;
    std::map<wxString, std::string> preset_filament_types;
    std::map<wxString, std::string> preset_aliases; // ORCA
    std::map<wxString, std::string> preset_bundle_ids;
    std::map<wxString, std::string> preset_bundle_names;
    //BBS:  move system to the end
    wxString selected_system_preset;
    wxString selected_user_preset;
    wxString selected_bundle_preset;
    wxString tooltip;
    const std::deque<Preset>& presets = m_collection->get_presets();

    //BBS:  move system to the end
    /*if (!presets.front().is_visible)
        this->set_label_marker(this->Append(separator(L("System presets")), wxNullBitmap));*/

    for (size_t i = presets.front().is_visible ? 0 : m_collection->num_default_presets(); i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        bool is_selected =  m_type == Preset::TYPE_FILAMENT ?
                            m_preset_bundle->filament_presets[m_filament_idx] == preset.name :
                            // The case, when some physical printer is selected
                            m_type == Preset::TYPE_PRINTER && m_preset_bundle->physical_printers.has_selection() ? false :
                            i == m_collection->get_selected_idx();

        if (!is_selected && !preset.is_visible)
        {
            continue;
        }
        else if (is_selected && !preset.is_visible)
        {
            Preset& selected_preset = const_cast<Preset&>(preset);
            selected_preset.is_visible = true;
        }

        bool single_bar = false;
        wxString name = from_u8(preset.name);
        preset_aliases[name] = get_preset_name(preset).utf8_string(); // ORCA

        // Track bundle names for bundled presets
        if (preset.is_from_bundle()) {
             m_preset_bundle->bundles.ReadLock();
            auto bundle_it = m_preset_bundle->bundles.m_bundles.find(preset.bundle_id);
            if (bundle_it != m_preset_bundle->bundles.m_bundles.end()) {
                preset_bundle_ids[name] = bundle_it->second.id;
                preset_bundle_names[name] = bundle_it->second.name;
            }
             m_preset_bundle->bundles.ReadUnlock();

        }

        if (m_type == Preset::TYPE_FILAMENT)
        {
#if 0
            // Assign an extruder color to the selected item if the extruder color is defined.
            filament_rgb = is_selected ? selected_filament_preset->config.opt_string("filament_colour", 0) :
                                         preset.config.opt_string("filament_colour", 0);
            extruder_rgb = (is_selected && !filament_color.empty()) ? filament_color : filament_rgb;
            single_bar = filament_rgb == extruder_rgb;

            bitmap_key += single_bar ? filament_rgb : filament_rgb + extruder_rgb;
#endif
            // ORCA allow caching vendor and type values for all presets instead just system ones
            // if (preset.is_system) { 
                if (!preset.is_compatible && preset_filament_vendors.count(name) > 0)
                    continue;
                else if (preset.is_compatible && preset_filament_vendors.count(name) > 0)
                    uncompatible_presets.erase(name);
                preset_filament_vendors[name] = preset.config.option<ConfigOptionStrings>("filament_vendor")->values.at(0);
                if (preset_filament_vendors[name] == "Bambu Lab")
                    preset_filament_vendors[name] = "Bambu";
                preset_filament_types[name] = preset.config.option<ConfigOptionStrings>("filament_type")->values.at(0);
            //}
        }
        wxBitmap* bmp = get_bmp(preset);
        assert(bmp);

        preset_descriptions.emplace(name, from_u8(preset.description));

        if (!preset.is_compatible) {
            if (boost::ends_with(name, " template"))
                continue;
            uncompatible_presets.emplace(name, bmp);
        }
        else if (preset.is_default || preset.is_system) {
            //BBS: move system to the end
            if (m_type == Preset::TYPE_PRINTER) {
                auto printer_model = preset.config.opt_string("printer_model");

                // ORCA: Make system printer presets display the dirty "*" prefix when edited.
                name = from_u8(is_selected && preset.is_dirty ? Preset::suffix_modified() + printer_model : printer_model);

                if (system_printer_models.count(printer_model) == 0) {
                    preset_aliases[name] = name.utf8_string(); // ORCA
                    system_presets.emplace(name, bmp);
                    system_printer_models.insert(printer_model);
                }
                else if (is_selected) {
                    const wxString alternate_name = from_u8(preset.is_dirty ? printer_model : Preset::suffix_modified() + printer_model);
                    // Remove the old preset name if exists, and add the new one with the same name but with modified suffix if needed.
                    if (system_presets.erase(alternate_name))
                        system_presets.emplace(name, bmp);

                    preset_aliases.erase(alternate_name);  // ORCA: do this to aliases too
                    preset_aliases[name] = name.utf8_string();
                }
            } else {
                system_presets.emplace(name, bmp);
            }

            if (is_selected) {
                tooltip = get_tooltip(preset);
                selected_system_preset = name;
            }
            //Append(get_preset_name(preset), *bmp);
            //validate_selection(is_selected);
            //if (is_selected)
                //BBS set tooltip
            //    tooltip = get_tooltip(preset);
        }
        //BBS: add project embedded preset logic
        else if (preset.is_project_embedded)
        {
            project_embedded_presets.emplace(name, bmp);
            if (is_selected) {
                selected_user_preset = name;
                tooltip = wxString::FromUTF8(preset.name.c_str());
            }
        }
        // ORCA: add bundle presets
        else if (preset.is_from_bundle())
        {
            bundle_presets.emplace(name, bmp);
            if (is_selected) {
                selected_bundle_preset = name;
                tooltip = get_tooltip(preset);
            }
        }
        else
        {
            nonsys_presets.emplace(name, bmp);
            if (is_selected) {
                selected_user_preset = name;
                //BBS set tooltip
                tooltip = get_tooltip(preset);
            }
        }
    }
    //if (m_type == Preset::TYPE_PRINTER)
    //    add_connected_printers("", true);
    bool selected_in_ams = false;
    if (m_type == Preset::TYPE_FILAMENT) {
        set_replace_text("Bambu", "BambuStudioBlack");
        // Orca: selected_system/user_preset hold the FULL preset name because Orca keys the maps above by
        // full name to avoid alias collisions (BBS keys by alias). add_ams_filaments() compares against
        // get_preset_name() which returns the alias, so resolve the selection back to its alias here.
        // Without this, e.g. "Bambu PLA Basic @BBL H2C" never equals the AMS tray alias "Bambu PLA Basic",
        // so the FROM_AMS flag is never set and update_sync_status() wipes the AMS sync check mark on the
        // filament cards for connected Bambu printers.
        wxString selected_full = selected_user_preset.empty() ? selected_system_preset : selected_user_preset;
        auto     alias_it      = preset_aliases.find(selected_full);
        wxString selected_alias = alias_it != preset_aliases.end() ? from_u8(alias_it->second) : selected_full;
        selected_in_ams = add_ams_filaments(into_u8(selected_alias), true);
    }

    // Snapmaker: filaments reported by the connected machine (SSWCP sync), filtered by nozzle.
    if (m_type == Preset::TYPE_FILAMENT && wxGetApp().preset_bundle->machine_filaments.size() > 0) {
        set_label_marker(Append(separator(L("Machine Filament")), wxNullBitmap));
        auto& filaments         = m_collection->get_presets();
        auto  machine_nozzles_list = wxGetApp().preset_bundle->m_connect_machine_info_list;
        m_first_ams_filament    = GetCount();

        std::string currentNozzleInfo;
        if (const auto* nd_opt = m_preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            nd_opt && !nd_opt->values.empty()) {
            currentNozzleInfo = float_to_string_decimal_point(nd_opt->values.front(), 2);
            while (!currentNozzleInfo.empty() && currentNozzleInfo.back() == '0')
                currentNozzleInfo.pop_back();
            if (!currentNozzleInfo.empty() && currentNozzleInfo.back() == '.')
                currentNozzleInfo.pop_back();
        }

        for (int i = 0; i < machine_nozzles_list.size(); i++) {
            std::string filament_name   = machine_nozzles_list[i].filament_info;
            std::string machine_nozzles = machine_nozzles_list[i].nozzle_info;

            // Filter by nozzle for display only; machine_filaments / m_connect_machine_info_list stay from sync (SSWCP).
            if (currentNozzleInfo != machine_nozzles)
                continue;

            auto item_iter = std::find_if(filaments.begin(), filaments.end(),
            [&filament_name, &machine_nozzles, &currentNozzleInfo](auto& f) {
                if (f.name == filament_name + " @U1 " + machine_nozzles + " nozzle")
                    if (f.is_compatible)
                        return true;

                if (f.name == filament_name + " @U1 " + machine_nozzles)
                    if (f.is_compatible)
                        return true;

                if (f.name == filament_name + " @U1")
                    if (f.is_compatible)
                        return true;

                if (f.name == filament_name)
                    if (f.is_compatible)
                        return true;

                return false;
            });

            if (item_iter != filaments.end()) {
                const_cast<Preset&>(*item_iter).is_visible = true;
                const ConnectMachineInfo& machineInfo = machine_nozzles_list[i];
                std::vector<std::string> colors = machineInfo.multiColors;
                if (colors.empty() && !machineInfo.color_info.empty())
                    colors.emplace_back(machineInfo.color_info);
                const std::string name = std::to_string(i + 1);
                wxBitmap* icon = FilamentColorUtils::GetFilamentColorIcon(colors, machineInfo.colorMode, name, 24, 16);
                if (icon == nullptr)
                    icon = get_extruder_color_icon(machineInfo.color_info, name, 24, 16);
                wxBitmap bmp(*icon);
                Append(get_preset_name(*item_iter), bmp.ConvertToImage(), &m_first_ams_filament + i);
            }
        }
        m_last_ams_filament = GetCount();
    }


    std::vector<wxString> filament_orders = {"Bambu PLA Basic", "Bambu PLA Matte", "Bambu PETG HF",    "Bambu ABS",      "Bambu PLA Silk", "Bambu PLA-CF",
                                                "Bambu PLA Galaxy", "Bambu PLA Metal", "Bambu PLA Marble", "Bambu PETG-CF", "Bambu PETG Translucent", "Bambu ABS-GF"};
    std::vector<std::string> first_vendors     = {"", "Bambu", "Generic"}; // Empty vendor for non-system presets
    std::vector<std::string> first_types     = {"PLA", "PETG", "ABS", "TPU"};
    auto  add_presets       = [this, &preset_descriptions, &filament_orders, &preset_filament_vendors, &first_vendors, &preset_filament_types, &preset_aliases, &preset_bundle_ids, &preset_bundle_names, &first_types, &selected_in_ams]
            (std::map<wxString, wxBitmap *> const &presets, wxString const &selected, std::string const &group, wxString const &groupName) {
        if (!presets.empty()) {
            set_label_marker(Append(_L(group), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
            if (m_type == Preset::TYPE_FILAMENT || m_type == Preset::TYPE_PRINTER) {
                std::vector<std::map<wxString, wxBitmap *>::value_type const*> list(presets.size(), nullptr);
                std::transform(presets.begin(), presets.end(), list.begin(), [](auto & pair) { return &pair; });
                bool groupByGroup = group != "System presets";
                //if (groupByGroup) {
                //    if (GetCount() == 1) Clear();
                //    else SetString(GetCount() - 1, "");
                //}
                if (m_type == Preset::TYPE_FILAMENT && (group == "System presets" || group == "Unsupported presets"))
                    std::sort(list.begin(), list.end(), [&filament_orders, &preset_filament_vendors, &first_vendors, &preset_filament_types, &first_types](auto *l, auto *r) {
                        { // Compare order
                            auto iter1 = std::find(filament_orders.begin(), filament_orders.end(), l->first);
                            auto iter2 = std::find(filament_orders.begin(), filament_orders.end(), r->first);
                            if (iter1 != iter2)
                                return iter1 < iter2;
                        }
                        { // Compare vendor
                            auto iter1 = std::find(first_vendors.begin(), first_vendors.end(), preset_filament_vendors[l->first]);
                            auto iter2 = std::find(first_vendors.begin(), first_vendors.end(), preset_filament_vendors[r->first]);
                            if (iter1 != iter2)
                                return iter1 < iter2;
                        }
                        { // Compare type
                            auto iter1 = std::find(first_types.begin(), first_types.end(), preset_filament_types[l->first]);
                            auto iter2 = std::find(first_types.begin(), first_types.end(), preset_filament_types[r->first]);
                            if (iter1 != iter2)
                                return iter1 < iter2;
                        }
                        return l->first < r->first;
                    });
                // ORCA add sorting support for vendor / type for user presets. also non grouped items
                if (groupName == "by_bundle" || groupName == "by_vendor" || groupName == "by_type" || groupName == ""){
                    auto by = groupName == "by_bundle" ? preset_bundle_names
                            : groupName == "by_vendor" ? preset_filament_vendors
                            : groupName == "by_type"   ? preset_filament_types
                            : preset_aliases;
                    std::sort(list.begin(), list.end(), [&by](auto *l, auto *r) {
                        auto get_key = [&](auto* item) -> std::pair<bool, std::string> {
                            std::string str = by.count(item->first) ? by.at(item->first) : "";
                            std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c);});
                            return {!str.empty(), str}; // is_valid, lower_case
                        };
                        auto [l_valid, l_lower] = get_key(l);
                        auto [r_valid, r_lower] = get_key(r);
                        return (l_valid != r_valid) ? l_valid > r_valid
                             : (l_lower != r_lower) ? l_lower < r_lower
                             : l->first < r->first;
                    });
                }
                bool unsupported = group == "Unsupported presets";
                for (auto it : list) {
                    // ORCA add sorting support for vendor / type for user presets
                    auto groupName2 = groupName == "by_bundle"   ? (preset_bundle_names[it->first].empty()     ? _L("Unspecified") : from_u8(preset_bundle_names[it->first]))
                                    : groupName == "by_type"     ? (preset_filament_types[it->first].empty()   ? _L("Unspecified") : from_u8(preset_filament_types[it->first]))
                                    : groupName == "by_vendor"   ? (preset_filament_vendors[it->first].empty() ? _L("Unspecified") : from_u8(preset_filament_vendors[it->first]))
                                    : groupByGroup               ? groupName
                                    : from_u8(preset_filament_vendors[it->first]);
                    int  index = groupName == "by_bundle"
                        ? Append(from_u8(preset_aliases[it->first]), *it->second,
                                 from_u8(preset_bundle_ids[it->first]), groupName2, nullptr,
                                 unsupported ? DD_ITEM_STYLE_DISABLED : 0)
                        : Append(from_u8(preset_aliases[it->first]), *it->second, groupName2, nullptr,
                                 unsupported ? DD_ITEM_STYLE_DISABLED : 0);
                    SetItemAlias(index, it->first);
                    if (unsupported)
                        set_label_marker(index, LABEL_ITEM_DISABLED);
                    else if (m_type == Preset::TYPE_PRINTER && group == "System presets" )
                        set_label_marker(index, LABEL_ITEM_PRINTER_MODELS);
                    SetItemTooltip(index, preset_descriptions[it->first]);
                    bool is_selected = it->first == selected;
                    validate_selection(is_selected);
                    if (is_selected  && selected_in_ams) {
                        SetFlag(GetCount() - 1, (int) FilamentAMSType::FROM_AMS);
                    }
                }
            } else {
                for (std::map<wxString, wxBitmap *>::const_iterator it = presets.begin(); it != presets.end(); ++it) {
                    int index = Append(from_u8(preset_aliases[it->first]), *it->second);
                    SetItemAlias(index, it->first);
                    SetItemTooltip(index, preset_descriptions[it->first]);
                    if (group == "System presets")
                        set_label_marker(GetCount() - 1, LABEL_ITEM_PRINTER_MODELS);
                    validate_selection(it->first == selected);
                }
            }
        }
    };

    //BBS: add project embedded preset logic
    add_presets(project_embedded_presets, selected_user_preset, L("Project-inside presets"), _L("Project") + " ");
    // ORCA add sorting support for vendor / type for user presets
    auto group_filament_presets    = wxGetApp().app_config->get("group_filament_presets");
    auto group_filament_presets_by = group_filament_presets  == "0" ? (_L("Custom") + " ") // Append all to "Custom" sub menu
                                   : group_filament_presets  == "2" ? "by_type"            // Create sub menus with filament type
                                   : group_filament_presets  == "3" ? "by_vendor"          // Create sub menus with filament vendor
                                   : "";                                                   // Use without sub menu
    // ORCA: the by_type/by_vendor grouping is derived from filament-only attributes
    // (filament_type/filament_vendor), which are empty for printer and material presets.
    // Applying it to non-filament combos buckets every user preset under "Unspecified",
    // so only group user presets by those attributes for the filament combobox.
    add_presets(nonsys_presets, selected_user_preset, L("User presets"),
                m_type == Preset::TYPE_FILAMENT ? group_filament_presets_by : wxString(""));
    // ORCA: add bundle presets with sub-dropdown grouping for filament and printer
    auto bundle_group_name = (m_type == Preset::TYPE_FILAMENT || m_type == Preset::TYPE_PRINTER) ? "by_bundle" : "";
    add_presets(bundle_presets, selected_bundle_preset, L("Bundle presets"), bundle_group_name);
    // BBS: move system to the end
    add_presets(system_presets, selected_system_preset, L("System presets"), _L("System"));
    // Orca: optionally show unsupported presets (controlled by developer preference, default off)
    if (wxGetApp().app_config->get_bool("show_unsupported_presets"))
        add_presets(uncompatible_presets, {}, L("Unsupported presets"), _L("Unsupported") + " ");

    if (m_type == Preset::TYPE_PRINTER || m_type == Preset::TYPE_FILAMENT || m_type == Preset::TYPE_SLA_MATERIAL) {
        wxBitmap* bmp = get_bmp("edit_preset_list", wide_icons, "edit_uni");
        assert(bmp);

        if (m_type == Preset::TYPE_FILAMENT)
            set_label_marker(Append(separator(L("Add/Remove filament")), *bmp), LABEL_ITEM_WIZARD_FILAMENTS);
        else if (m_type == Preset::TYPE_SLA_MATERIAL)
            set_label_marker(Append(separator(L("Add/Remove materials")), *bmp), LABEL_ITEM_WIZARD_MATERIALS);
        else {
            set_label_marker(Append(separator(L("Select/Remove printers (system presets)")), *bmp), LABEL_ITEM_WIZARD_PRINTERS);
            set_label_marker(Append(separator(L("Create printer")), *bmp), LABEL_ITEM_WIZARD_ADD_PRINTERS);
        }
    }

    update_selection();
    if (m_type == Preset::TYPE_FILAMENT) {
        if (wxGetApp().plater()->is_same_printer_for_connected_and_selected(false)) {
            update_badge_according_flag();
        }
    }
    Thaw();

    if (!tooltip.IsEmpty()) {
#ifdef __WXMSW__
        // From the Windows 2004 the tooltip for preset combobox doesn't work after next call of SetTooltip()
        // (There was an issue, when tooltip doesn't appears after changing of the preset selection)
        // But this workaround seems to work: We should to kill tooltip and than set new tooltip value
        // See, https://groups.google.com/g/wx-users/c/mOEe3fgHrzk
        SetToolTip(NULL);
#endif
        SetToolTip(tooltip);
    }

#ifdef __WXMSW__
    // Use this part of code just on Windows to avoid of some layout issues on Linux
    // Update control min size after rescale (changed Display DPI under MSW)
    if (GetMinWidth() != 10 * m_em_unit)
        SetMinSize(wxSize(10 * m_em_unit, GetSize().GetHeight()));
#endif //__WXMSW__
}

void PlaterPresetComboBox::msw_rescale()
{
    PresetComboBox::msw_rescale();
    SetMinSize({-1, 30 * m_em_unit / 10});

    if (clr_picker)
        clr_picker->SetSize(20 * m_em_unit / 10, 20 * m_em_unit / 10);
    // BBS
    if (edit_btn != nullptr)
        edit_btn->msw_rescale();
    
    // 重新缩放按钮图标
    m_connection_icon.msw_rescale();
    m_machine_connecting_icon.msw_rescale();
    m_edit_icon.msw_rescale();
}

// 设置按钮显示状态
void PlaterPresetComboBox::set_show_connection_button(bool show)
{
    if (m_show_connection_button != show) {
        m_show_connection_button = show;
        Refresh();
    }
}

void PlaterPresetComboBox::set_show_machine_connecting_button(bool show)
{
    if (m_show_machine_connecting_button != show) {
        m_show_machine_connecting_button = show;
        Refresh();
    }
}

void PlaterPresetComboBox::set_show_edit_button(bool show)
{
    if (m_show_edit_button != show) {
        m_show_edit_button = show;
        Refresh();
    }
}

// 绑定按钮事件处理函数
void PlaterPresetComboBox::bind_connection_button_handler(std::function<void()> handler)
{
    m_connection_btn_handler = handler;
}

void PlaterPresetComboBox::bind_machine_connecting_button_handler(std::function<void()> handler)
{
    m_machine_connecting_btn_handler = handler;
}

void PlaterPresetComboBox::bind_edit_button_handler(std::function<void()> handler)
{
    m_edit_btn_handler = handler;
}

void PlaterPresetComboBox::set_connection_tooltip(const wxString& tooltip)
{
    m_connection_tooltip = tooltip;
}

void PlaterPresetComboBox::set_machine_connecting_tooltip(const wxString& tooltip)
{
    m_machine_connecting_tooltip = tooltip;
}

// 计算各个区域的矩形
wxRect PlaterPresetComboBox::get_machine_connecting_btn_rect() const
{
    if (!m_show_machine_connecting_button)
        return wxRect();
    
    wxSize size = GetSize();
    wxSize icon_size = m_machine_connecting_icon.GetBmpSize();
    int x = 5 + 16 + 6; // 下拉箭头后（机器名称左边，间距从4改为6）
    int y = (size.y - icon_size.y) / 2;
    return wxRect(x, y, icon_size.x, icon_size.y);
}

wxRect PlaterPresetComboBox::get_edit_btn_rect() const
{
    if (!m_show_edit_button)
        return wxRect();
    
    wxSize size = GetSize();
    wxSize icon_size = m_edit_icon.GetBmpSize();
    
    // 编辑按钮在 connection_btn 的左边
    int right_offset = 8; // 右边距
    if (m_show_connection_button) {
        right_offset += 16 + 8; // connection_btn 宽度 + 增加间距
    }
    
    int x = size.x - icon_size.x - right_offset;
    int y = (size.y - icon_size.y) / 2;
    return wxRect(x, y, icon_size.x, icon_size.y);
}

wxRect PlaterPresetComboBox::get_connection_btn_rect() const
{
    if (!m_show_connection_button)
        return wxRect();
    
    wxSize size = GetSize();
    wxSize icon_size = m_connection_icon.GetBmpSize();
    int x = size.x - icon_size.x - 8; // 最右侧
    int y = (size.y - icon_size.y) / 2;
    return wxRect(x, y, icon_size.x, icon_size.y);
}

wxRect PlaterPresetComboBox::get_dropdown_rect() const
{
    wxSize size = GetSize();
    return wxRect(0, 0, 25, size.y); // 左侧25px为下拉箭头区域
}

// 自定义绘制
void PlaterPresetComboBox::paintEvent(wxPaintEvent& evt)
{
    if (m_type == Preset::TYPE_PRINTER) {
        wxPaintDC dc(this);
        render(dc);
    } else {
        evt.Skip();
    }
}

void PlaterPresetComboBox::render(wxDC& dc)
{
    int states = state_handler.states();
    wxSize size = GetSize();
    
    // 1. 绘制背景和边框
    StaticBox::render(dc);
    
    // 2. 绘制下拉箭头（左侧）
    // 使用 create_scaled_bitmap 创建下拉箭头图标
    wxBitmap dropdown_bmp = create_scaled_bitmap("drop_down", nullptr, 16);
    if (dropdown_bmp.IsOk()) {
        int x = 5;
        int y = (size.y - 16) / 2;
        dc.DrawBitmap(dropdown_bmp, wxPoint(x, y));
    }
    
    int left_offset = 5 + 16 + 6; // 下拉箭头后的起始位置（间距从4改为6）
    
    // 3. 绘制 machine_connecting_btn（在机器名称左边）
    if (m_show_machine_connecting_button && m_machine_connecting_icon.bmp().IsOk()) {
        wxRect rect = get_machine_connecting_btn_rect();
        dc.DrawBitmap(m_machine_connecting_icon.bmp(), wxPoint(rect.x, rect.y));
        left_offset += m_machine_connecting_icon.GetBmpSize().x + 6;
    }
    
    // 4. 绘制机器名称
    auto text = GetLabel();
    if (!text.IsEmpty()) {
        dc.SetFont(GetFont());
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour(38, 46, 48)));
        
        // 计算可用宽度（需要为右侧按钮预留空间）
        int right_reserve = 8; // 基础右边距
        if (m_show_edit_button) {
            right_reserve += 16 + 4; // 编辑按钮 + 间距
        }
        if (m_show_connection_button) {
            right_reserve += 16 + 8; // connection_btn + 增加间距
        }
        
        int available_width = size.x - left_offset - right_reserve;
        
        wxSize text_size = dc.GetTextExtent(text);
        if (text_size.x > available_width) {
            text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, available_width);
        }
        
        int text_x = left_offset;
        int text_y = (size.y - text_size.y) / 2;
        dc.DrawText(text, wxPoint(text_x, text_y));
    }
    
    // 5. 绘制编辑按钮（在 connection_btn 左边）
    if (m_show_edit_button && m_edit_icon.bmp().IsOk()) {
        wxRect rect = get_edit_btn_rect();
        dc.DrawBitmap(m_edit_icon.bmp(), wxPoint(rect.x, rect.y));
    }
    
    // 6. 绘制 connection_btn（最右侧）
    if (m_show_connection_button && m_connection_icon.bmp().IsOk()) {
        wxRect rect = get_connection_btn_rect();
        dc.DrawBitmap(m_connection_icon.bmp(), wxPoint(rect.x, rect.y));
    }
}

// 鼠标事件处理
void PlaterPresetComboBox::onMouseLeftDown(wxMouseEvent& evt)
{
    wxPoint pos = evt.GetPosition();
    
    // 检查是否点击了编辑按钮
    if (m_show_edit_button && get_edit_btn_rect().Contains(pos)) {
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 检查是否点击了连接按钮
    if (m_show_connection_button && get_connection_btn_rect().Contains(pos)) {
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 检查是否点击了machine_connecting按钮
    if (m_show_machine_connecting_button && get_machine_connecting_btn_rect().Contains(pos)) {
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 其他区域，允许触发下拉菜单
    evt.Skip();
}

void PlaterPresetComboBox::onMouseLeftUp(wxMouseEvent& evt)
{
    wxPoint pos = evt.GetPosition();
    
    // 检查是否点击了编辑按钮
    if (m_show_edit_button && get_edit_btn_rect().Contains(pos)) {
        if (m_edit_btn_handler) {
            m_edit_btn_handler();
        }
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 检查是否点击了连接按钮
    if (m_show_connection_button && get_connection_btn_rect().Contains(pos)) {
        if (m_connection_btn_handler) {
            m_connection_btn_handler();
        }
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 检查是否点击了machine_connecting按钮
    if (m_show_machine_connecting_button && get_machine_connecting_btn_rect().Contains(pos)) {
        if (m_machine_connecting_btn_handler) {
            m_machine_connecting_btn_handler();
        }
        evt.StopPropagation(); // 阻止事件传播，防止触发下拉框
        return;
    }
    
    // 其他区域，触发下拉菜单
    evt.Skip();
}

void PlaterPresetComboBox::onMouseEnter(wxMouseEvent& evt)
{
    evt.Skip();
}

void PlaterPresetComboBox::onMouseLeave(wxMouseEvent& evt)
{
    if (m_hover_state != HoverState::NONE) {
        m_hover_state = HoverState::NONE;
        Refresh();
    }
    evt.Skip();
}

void PlaterPresetComboBox::onMouseMove(wxMouseEvent& evt)
{
    wxPoint pos = evt.GetPosition();
    HoverState new_state = HoverState::NONE;
    
    // 检查鼠标位置
    if (m_show_edit_button && get_edit_btn_rect().Contains(pos)) {
        new_state = HoverState::EDIT_BTN;
        // 清除tooltip
        SetToolTip("");
    } else if (m_show_connection_button && get_connection_btn_rect().Contains(pos)) {
        new_state = HoverState::CONNECTION_BTN;
        // 显示连接按钮tooltip
        if (!m_connection_tooltip.IsEmpty()) {
            SetToolTip(m_connection_tooltip);
        }
    } else if (m_show_machine_connecting_button && get_machine_connecting_btn_rect().Contains(pos)) {
        new_state = HoverState::MACHINE_CONNECTING_BTN;
        // 显示机器连接按钮tooltip
        if (!m_machine_connecting_tooltip.IsEmpty()) {
            SetToolTip(m_machine_connecting_tooltip);
        }
    } else if (get_dropdown_rect().Contains(pos)) {
        new_state = HoverState::DROPDOWN;
        // 清除tooltip
        SetToolTip("");
    } else {
        // 清除tooltip
        SetToolTip("");
    }
    
    // 如果状态改变，触发重绘
    if (new_state != m_hover_state) {
        m_hover_state = new_state;
        Refresh();
    }
    
    evt.Skip();
}


EncodedFilamentColor PlaterPresetComboBox::get_cur_color_info()
{
    std::vector<std::string> filaments_multi_color = Slic3r::GUI::wxGetApp().plater()->get_filament_colors_render_info();
    std::vector<std::string> filament_color_type = Slic3r::GUI::wxGetApp().plater()->get_filament_color_render_type();

    if (m_filament_idx < 0 || m_filament_idx >= static_cast<int>(filaments_multi_color.size())) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": m_filament_idx %1% out of range %2%") % m_filament_idx % filaments_multi_color.size();
        return EncodedFilamentColor();
    }

    if (m_filament_idx >= static_cast<int>(filament_color_type.size())) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": m_filament_idx %1% out of range for color_type %2%") % m_filament_idx % filament_color_type.size();
        return EncodedFilamentColor();
    }
    std::string filament_color_info = filaments_multi_color[m_filament_idx];
    std::vector<std::string> colors;
    colors = Slic3r::split_string(filament_color_info, ' ');
    EncodedFilamentColor fila_color;
    for (const std::string& color_str : colors) {
        if (!color_str.empty()) {
            wxColour color(color_str);
            if (color.IsOk()) {
                fila_color.m_colors.insert(color);
            }
        }
    }

    fila_color.EndSet(filament_color_type[m_filament_idx] == "0" ? 0 : 1);
    return fila_color;
}

void PlaterPresetComboBox::show_default_color_picker()
{
    DynamicPrintConfig* cfg = &wxGetApp().preset_bundle->project_config;
    auto colors = static_cast<ConfigOptionStrings*>(cfg->option("filament_colour")->clone());
    wxColour current_clr(colors->values[m_filament_idx]);
    if (!current_clr.IsOk())
        current_clr = wxColour(0, 0, 0); // Don't set alfa to transparence

    m_clrData.SetColour(current_clr);

    wxColourData data = show_sys_picker_dialog(this, m_clrData);
    if (m_clrData.GetColour() != data.GetColour()) {
        std::vector<std::string> color = {data.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString()};
        m_clrData.SetColour(data.GetColour());
        sync_colour_config(color, false);
    }
}

void PlaterPresetComboBox::sync_colour_config(const std::vector<std::string> &clrs, bool is_gradient)
{
    DynamicPrintConfig *cfg = &wxGetApp().preset_bundle->project_config;

    // Clone the string vector and patch the value at current extruder index.
    auto multi_colour_opt = static_cast<ConfigOptionStrings *>(cfg->option("filament_multi_colour")->clone());
    auto colour_type_opt = static_cast<ConfigOptionStrings *>(cfg->option("filament_colour_type")->clone());
    auto colour_opt = static_cast<ConfigOptionStrings *>(cfg->option("filament_colour")->clone());

    if (m_filament_idx >= multi_colour_opt->values.size()) multi_colour_opt->values.resize(m_filament_idx + 1);
    if (m_filament_idx >= colour_type_opt->values.size()) colour_type_opt->values.resize(m_filament_idx + 1);
    if (m_filament_idx >= colour_opt->values.size()) colour_opt->values.resize(m_filament_idx + 1);

    std::string clr_str = "";
    for(auto &clr : clrs) {
        clr_str += clr + " ";
    }
    clr_str.pop_back();

    multi_colour_opt->values[m_filament_idx] = clr_str;
    colour_opt->values[m_filament_idx] = clrs[0];
    colour_type_opt->values[m_filament_idx] = is_gradient ? "0" : "1";
    DynamicPrintConfig cfg_new = *cfg;
    cfg_new.set_key_value("filament_multi_colour", multi_colour_opt);
    cfg_new.set_key_value("filament_colour", colour_opt);
    cfg_new.set_key_value("filament_colour_type", colour_type_opt);
    cfg->apply(cfg_new);

    wxGetApp().plater()->update_project_dirty_from_presets();

    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    update();  // refresh the preset combobox with new config

    wxGetApp().plater()->on_config_change(cfg_new);
}

// ---------------------------------
// ***  TabPresetComboBox  ***
// ---------------------------------

TabPresetComboBox::TabPresetComboBox(wxWindow* parent, Preset::Type preset_type) :
    // BBS: new layout
    PresetComboBox(parent, preset_type, wxSize(20 * wxGetApp().em_unit(), 30 * wxGetApp().em_unit() / 10))
{
    GetDropDown().SetUseContentWidth(true,true);
}

void TabPresetComboBox::OnSelect(wxCommandEvent &evt)
{
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender")
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter
    auto selected_item = evt.GetSelection();

    auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX) {
        this->SetSelection(m_last_selected);
        // BBS: Add/Remove filaments
        ConfigWizard::StartPage sp = ConfigWizard::SP_WELCOME;
        switch (marker) {
        case LABEL_ITEM_WIZARD_PRINTERS: sp = ConfigWizard::SP_PRINTERS; break;
        case LABEL_ITEM_WIZARD_FILAMENTS: sp = ConfigWizard::SP_FILAMENTS; break;
        case LABEL_ITEM_WIZARD_MATERIALS: sp = ConfigWizard::SP_MATERIALS; break;
        default: break;
        }
        if (sp != ConfigWizard::SP_WELCOME) {
            wxTheApp->CallAfter([this, sp]() {
                run_wizard(sp);
            });
        }
    }
    else if (on_selection_changed && (m_last_selected != selected_item || m_collection->current_is_dirty())) {
        m_last_selected = selected_item;
        // BBS: ams
        update_ams_color();
        on_selection_changed(selected_item);
    }

    evt.StopPropagation();
#ifdef __WXMSW__
    // From the Win 2004 preset combobox lose a focus after change the preset selection
    // and that is why the up/down arrow doesn't work properly
    // So, set the focus to the combobox explicitly
    this->SetFocus();
#endif
}

wxString TabPresetComboBox::get_preset_name(const Preset& preset)
{
    if (preset.is_from_bundle())
        return from_u8(preset.label(false));
    else
        return from_u8(preset.label(true));
}

// Update the choice UI from the list of presets.
// If show_incompatible, all presets are shown, otherwise only the compatible presets are shown.
// If an incompatible preset is selected, it is shown as well.
void TabPresetComboBox::update()
{
    Freeze();
    Clear();
    invalidate_selection();

    const std::deque<Preset>& presets = m_collection->get_presets();

    std::map<wxString, std::pair<wxBitmap*, bool>> nonsys_presets;
    //BBS: add project embedded presets logic
    std::map<wxString, std::pair<wxBitmap*, bool>>  project_embedded_presets;
    //BBS:  move system to the end
    std::map<wxString, std::pair<wxBitmap*, bool>>  system_presets;
    std::map<wxString, std::pair<wxBitmap*, bool>> machine_filament_presets;
    // ORCA: add bundle presets
    std::map<wxString, std::pair<wxBitmap*, bool>>  bundle_presets;
    std::map<wxString, wxString>                    preset_descriptions;
    std::map<wxString, std::string>                 preset_aliases; // ORCA
    std::map<wxString, std::string>                 preset_bundle_ids;
    std::map<wxString, std::string>                 preset_bundle_names;

    wxString selected = "";
    //BBS:  move system to the end
    /*if (!presets.front().is_visible)
        set_label_marker(Append(separator(L("System presets")), wxNullBitmap));*/
    size_t idx_selected = m_collection->get_selected_idx();

    if (m_type == Preset::TYPE_PRINTER && m_preset_bundle->physical_printers.has_selection()) {
        std::string sel_preset_name = m_preset_bundle->physical_printers.get_selected_printer_preset_name();
        Preset* preset = m_collection->find_preset(sel_preset_name);
        if (!preset)
            m_preset_bundle->physical_printers.unselect_printer();
    }

    for (size_t i = presets.front().is_visible ? 0 : m_collection->num_default_presets(); i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        if (!preset.is_visible || (!show_incompatible && !preset.is_compatible && i != idx_selected))
            continue;

        // marker used for disable incompatible printer models for the selected physical printer
        bool is_enabled = true;

        wxBitmap* bmp = get_bmp(preset);
        assert(bmp);

        const wxString name = from_u8(preset.name);
        preset_aliases[name] = get_preset_name(preset).utf8_string();
        if (preset.is_system)
            preset_descriptions.emplace(name, from_u8(preset.description));

        // ORCA: Track bundle names for bundled presets
        if (preset.is_from_bundle()) {
             m_preset_bundle->bundles.ReadLock();
            auto bundle_it = m_preset_bundle->bundles.m_bundles.find(preset.bundle_id);
            if (bundle_it != m_preset_bundle->bundles.m_bundles.end()) {
                preset_bundle_ids[name] = bundle_it->second.id;
                preset_bundle_names[name] = bundle_it->second.name;
            }
             m_preset_bundle->bundles.ReadUnlock();

        }

        if (preset.is_default || preset.is_system) {
            //BBS: move system to the end
            system_presets.emplace(name, std::pair<wxBitmap *, bool>(bmp, is_enabled));
            if (i == idx_selected)
                selected = name;
            //int item_id = Append(get_preset_name(preset), *bmp);
            //if (!is_enabled)
            //    set_label_marker(item_id, LABEL_ITEM_DISABLED);
            //validate_selection(i == idx_selected);
        }
        //BBS: add project embedded preset logic
        else if (preset.is_project_embedded)
        {
            //std::pair<wxBitmap*, bool> pair(bmp, is_enabled);
            project_embedded_presets.emplace(name, std::pair<wxBitmap *, bool>(bmp, is_enabled));
            if (i == idx_selected)
                selected = name;
        }
        // ORCA: add bundle presets
        else if (preset.is_from_bundle())
        {
            bundle_presets.emplace(name, std::pair<wxBitmap*, bool>(bmp, is_enabled));
            if (i == idx_selected)
                selected = name;
        }
        else
        {
            std::pair<wxBitmap*, bool> pair(bmp, is_enabled);
            nonsys_presets.emplace(name, std::pair<wxBitmap *, bool>(bmp, is_enabled));
            if (i == idx_selected)
                selected = name;
        }
    }

    if (m_type == Preset::TYPE_FILAMENT && m_preset_bundle->is_bbl_vendor())
        add_ams_filaments(into_u8(selected));
    
    if (m_type == Preset::TYPE_FILAMENT && wxGetApp().preset_bundle->m_connect_machine_info_list.size() > 0) {
        set_label_marker(Append(separator(L("Machine Filament")), wxNullBitmap));
        auto& filaments            = m_collection->get_presets();
        auto  machine_nozzles_list = wxGetApp().preset_bundle->m_connect_machine_info_list;
        m_first_ams_filament       = GetCount();

        std::string currentNozzleInfo;
        if (const auto* nd_opt = m_preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
            nd_opt && !nd_opt->values.empty()) {
            currentNozzleInfo = float_to_string_decimal_point(nd_opt->values.front(), 2);
            while (!currentNozzleInfo.empty() && currentNozzleInfo.back() == '0')
                currentNozzleInfo.pop_back();
            if (!currentNozzleInfo.empty() && currentNozzleInfo.back() == '.')
                currentNozzleInfo.pop_back();
        }

        for (int i = 0; i < machine_nozzles_list.size(); i++) {
            std::string filament_name   = machine_nozzles_list[i].filament_info;
            std::string machine_nozzles = machine_nozzles_list[i].nozzle_info;

            if (currentNozzleInfo != machine_nozzles)
                continue;

            auto item_iter = std::find_if(filaments.begin(), filaments.end(),[&filament_name, &machine_nozzles, &currentNozzleInfo](auto& f) {               

                if (f.name == filament_name + " @U1 " + machine_nozzles + " nozzle")
                    if (f.is_compatible)
                        return true;
                
                if (f.name == filament_name + " @U1 " + machine_nozzles)
                    if (f.is_compatible)
                        return true;
                
                if (f.name == filament_name + " @U1")
                    if (f.is_compatible)
                        return true;

                if (f.name == filament_name)
                    if (f.is_compatible)
                        return true;

                return false;                
                });

            if (item_iter != filaments.end()) {
                const_cast<Preset&>(*item_iter).is_visible = true;
                const ConnectMachineInfo& machineInfo = machine_nozzles_list[i];
                std::vector<std::string> colors = machineInfo.multiColors;
                if (colors.empty() && !machineInfo.color_info.empty())
                    colors.emplace_back(machineInfo.color_info);
                const std::string name = std::to_string(i + 1);
                wxBitmap* icon = FilamentColorUtils::GetFilamentColorIcon(colors, machineInfo.colorMode, name, 24, 16);
                if (icon == nullptr)
                    icon = get_extruder_color_icon(machineInfo.color_info, name, 24, 16);
                wxBitmap bmp(*icon);
                Append(get_preset_name(*item_iter), bmp.ConvertToImage(), &m_first_ams_filament + i);
            }
        }

        m_last_ams_filament = GetCount();
    }

    //BBS: add project embedded preset logic
    if (!project_embedded_presets.empty())
    {
        set_label_marker(Append(_L("Project-inside presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, std::pair<wxBitmap*, bool>>::iterator it = project_embedded_presets.begin(); it != project_embedded_presets.end(); ++it) {
            int item_id = Append(it->first, *it->second.first);
            SetItemTooltip(item_id, preset_descriptions[it->first]);
            bool is_enabled = it->second.second;
            if (!is_enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->first == selected);
        }
    }
    if (!nonsys_presets.empty())
    {
        set_label_marker(Append(_L("User presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, std::pair<wxBitmap*, bool>>::iterator it = nonsys_presets.begin(); it != nonsys_presets.end(); ++it) {
            int item_id = Append(it->first, *it->second.first);
            SetItemAlias(item_id, it->first);
            SetItemTooltip(item_id, preset_descriptions[it->first]);
            bool is_enabled = it->second.second;
            if (!is_enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->first == selected);
        }
    }
    // ORCA: add bundle presets with sub-dropdown grouping
    if (!bundle_presets.empty())
    {
        set_label_marker(Append(_L("Bundle presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, std::pair<wxBitmap*, bool>>::iterator it = bundle_presets.begin(); it != bundle_presets.end(); ++it) {
            // Get bundle name for grouping
            wxString bundle_name = _L("Unspecified");
            if (preset_bundle_names.count(it->first) > 0 && !preset_bundle_names[it->first].empty()) {
                bundle_name = from_u8(preset_bundle_names[it->first]);
            }
            // Use Append with group parameter for sub-dropdown grouping
            int item_id = Append(from_u8(preset_aliases[it->first]), *it->second.first, from_u8(preset_bundle_ids[it->first]), bundle_name);
            SetItemAlias(item_id, it->first);
            SetItemTooltip(item_id, preset_descriptions[it->first]);
            bool is_enabled = it->second.second;
            if (!is_enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->first == selected);
        }
    }
    //BBS: move system to the end
    if (!system_presets.empty())
    {
        set_label_marker(Append(_L("System presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (std::map<wxString, std::pair<wxBitmap*, bool>>::iterator it = system_presets.begin(); it != system_presets.end(); ++it) {
            int item_id = Append(it->first, *it->second.first);
            SetItemAlias(item_id, it->first);
            SetItemTooltip(item_id, preset_descriptions[it->first]);
            bool is_enabled = it->second.second;
            if (!is_enabled)
                set_label_marker(item_id, LABEL_ITEM_DISABLED);
            validate_selection(it->first == selected);
        }
    }

    update_selection();
    Thaw();
}

void TabPresetComboBox::msw_rescale()
{
    PresetComboBox::msw_rescale();
    // BBS: new layout
    wxSize sz = wxSize(20 * m_em_unit, 30 * m_em_unit / 10);
    SetMinSize(sz);
    SetSize(sz);
}

void TabPresetComboBox::update_dirty()
{
    // 1) Update the dirty flag of the current preset.
    m_collection->update_dirty();

    // 2) Update the labels.
    wxWindowUpdateLocker noUpdates(this);
    for (unsigned int ui_id = 0; ui_id < GetCount(); ++ui_id) {
        auto marker = reinterpret_cast<Marker>(this->GetClientData(ui_id));
        if (marker >= LABEL_ITEM_MARKER)
            continue;

        std::string   old_label = GetString(ui_id).utf8_str().data();
        std::string   preset_name = Preset::remove_suffix_modified(old_label);
        std::string   ph_printer_name;

        if (marker == LABEL_ITEM_PHYSICAL_PRINTER) {
            ph_printer_name = PhysicalPrinter::get_short_name(preset_name);
            preset_name = PhysicalPrinter::get_preset_name(preset_name);
        }

        Preset* preset = m_collection->find_preset(preset_name, false);
        if (preset) {
            std::string new_label = preset->label(true);

            if (marker == LABEL_ITEM_PHYSICAL_PRINTER)
                new_label = ph_printer_name + PhysicalPrinter::separator() + new_label;

            if (old_label != new_label) {
                SetString(ui_id, from_u8(new_label));
                SetItemBitmap(ui_id, *get_bmp(*preset));
                if (ui_id == GetSelection()) SetToolTip(wxString::FromUTF8(new_label.c_str())); // BBS
            }
        }
    }
#ifdef __APPLE__
    // wxWidgets on OSX do not upload the text of the combo box line automatically.
    // Force it to update by re-selecting.
    SetSelection(GetSelection());
#endif /* __APPLE __ */
}

} // namespace GUI
GUI::CalibrateFilamentComboBox::CalibrateFilamentComboBox(wxWindow *parent)
: PlaterPresetComboBox(parent, Preset::TYPE_FILAMENT)
{
    clr_picker->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    clr_picker->SetToolTip("");
    clr_picker->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {});
}

GUI::CalibrateFilamentComboBox::~CalibrateFilamentComboBox()
{
}

void GUI::CalibrateFilamentComboBox::load_tray(DynamicPrintConfig &config)
{
    m_tray_name = config.opt_string("tray_name", 0u);
    size_t pos = m_tray_name.find("HT-");
    if (pos != std::string::npos) {
        m_tray_name = m_tray_name.substr(pos + 3);
    }
    m_filament_id = config.opt_string("filament_id", 0u);
    m_tag_uid = config.opt_string("tag_uid", 0u);
    m_filament_type  = config.opt_string("filament_type", 0u);
    m_filament_color = config.opt_string("filament_colour", 0u);
    m_filament_exist = config.opt_bool("filament_exist", 0u);
    wxColor clr(m_filament_color);
    clr_picker->SetBitmap(*get_extruder_color_icon(m_filament_color, m_tray_name, FromDIP(20), FromDIP(20)));
#ifdef __WXOSX__
    clr_picker->SetLabel(clr_picker->GetLabel()); // Let setBezelStyle: be called
    clr_picker->Refresh();
#endif
    if (!m_filament_exist) {
        SetValue(_L("Empty"));
        m_selected_preset = nullptr;
        m_is_compatible = false;
        clr_picker->SetBitmap(*get_extruder_color_icon("#F0F0F0FF", m_tray_name, FromDIP(20), FromDIP(20)));
    } else {
        auto &filaments = m_collection->get_presets();
        auto  iter      = std::find_if(filaments.begin(), filaments.end(), [this](auto &f) {
            if (!f.is_system) // Only match system preset
                return false;
            bool is_compatible = m_preset_bundle->calibrate_filaments.find(&f) != m_preset_bundle->calibrate_filaments.end();
            return is_compatible && f.filament_id == m_filament_id;
            });

        // Prioritize matching system presets. If there are no system presets, match all presets.
        if (iter == filaments.end()) {
            iter = std::find_if(filaments.begin(), filaments.end(), [this](auto &f) {
                if (f.is_system) // Only match system preset
                    return false;
                bool is_compatible = m_preset_bundle->calibrate_filaments.find(&f) != m_preset_bundle->calibrate_filaments.end();
                return is_compatible && f.filament_id == m_filament_id;
            });
        }
        //if (iter == filaments.end() && !m_filament_type.empty()) {
        //    auto filament_type = "Generic " + m_filament_type;
        //    iter               = std::find_if(filaments.begin(), filaments.end(),
        //                        [this , &filament_type](auto &f) {
        //            bool is_compatible = m_preset_bundle->calibrate_filaments.find(&f) != m_preset_bundle->calibrate_filaments.end();
        //            return is_compatible && f.is_system && boost::algorithm::starts_with(f.name, filament_type); });
        //}
        if (iter != filaments.end()) {
            m_selected_preset = &*iter;
            m_is_compatible = true;
            SetValue(get_preset_name(*iter));
        }
        else {
            m_selected_preset = nullptr;
            m_is_compatible = false;
            SetValue(_L("Incompatible"));
        }
        Enable();
    }
}

void GUI::CalibrateFilamentComboBox::update()
{
    if (m_preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA)
        return;

    // Otherwise fill in the list from scratch.
    this->Freeze();
    this->Clear();
    invalidate_selection();

    const Preset* selected_filament_preset = nullptr;

    m_nonsys_presets.clear();
    m_system_presets.clear();

    wxString selected_preset = m_selected_preset ? get_preset_name(*m_selected_preset) : GetValue();

    wxString tooltip;
    const std::deque<Preset>& presets = m_collection->get_presets();

    for (size_t i = presets.front().is_visible ? 0 : m_collection->num_default_presets(); i < presets.size(); ++i)
    {
        const Preset& preset = presets[i];
        auto display_name = get_preset_name(preset);
        bool          is_selected   = m_selected_preset == &preset;
        if (m_preset_bundle->calibrate_filaments.empty()) {
            Thaw();
            return;
        }
        bool          is_compatible = m_preset_bundle->calibrate_filaments.find(&preset) != m_preset_bundle->calibrate_filaments.end();
        ;
        if (!preset.is_visible || (!is_compatible && !is_selected))
            continue;

        if (is_selected) {
            tooltip = get_tooltip(preset);
        }

        wxBitmap* bmp = get_bmp(preset);
        assert(bmp);

        if (preset.is_default || preset.is_system) {
            m_system_presets.emplace(display_name, std::make_pair( preset.name, bmp ));
        }
        else {
            m_nonsys_presets.emplace(display_name, std::make_pair( preset.name, bmp ));
        }

    }

    if (!m_nonsys_presets.empty())
    {
        set_label_marker(Append(_L("User presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (auto it = m_nonsys_presets.begin(); it != m_nonsys_presets.end(); ++it) {
            Append(it->first, *(it->second.second));
            validate_selection(it->first == selected_preset);
        }
    }
    if (!m_system_presets.empty())
    {
        set_label_marker(Append(_L("System presets"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM));
        for (auto it = m_system_presets.begin(); it != m_system_presets.end(); ++it) {
            Append(it->first, *(it->second.second));
            validate_selection(it->first == selected_preset);
        }
    }

    update_selection();
    Thaw();

    SetToolTip(NULL);
}

void GUI::CalibrateFilamentComboBox::msw_rescale()
{
    if (clr_picker) {
        clr_picker->SetSize(FromDIP(20), FromDIP(20));
        clr_picker->SetBitmap(*get_extruder_color_icon(m_filament_color, m_tray_name, FromDIP(20), FromDIP(20)));
    }
    // BBS
    if (edit_btn != nullptr)
        edit_btn->msw_rescale();
}

void GUI::CalibrateFilamentComboBox::OnSelect(wxCommandEvent &evt)
{
    auto marker = reinterpret_cast<Marker>(this->GetClientData(evt.GetSelection()));
    if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX) {
        this->SetSelection(evt.GetSelection() + 1);
        wxCommandEvent event(wxEVT_COMBOBOX);
        event.SetInt(evt.GetSelection() + 1);
        event.SetString(GetString(evt.GetSelection() + 1));
        wxPostEvent(this, event);
        return;
    }
    m_is_compatible = true;
    static_cast<FilamentComboBox*>(m_parent)->Enable(true);

    wxString display_name = evt.GetString();
    std::string preset_name;
    if (m_system_presets.find(evt.GetString()) != m_system_presets.end()) {
        preset_name = m_system_presets.at(display_name).first;
    }
    else if (m_nonsys_presets.find(evt.GetString()) != m_nonsys_presets.end()) {
        preset_name = m_nonsys_presets.at(display_name).first;
    }
    m_selected_preset       = m_collection->find_preset(preset_name);

    // if the selected preset is null, do not send tray_change event
    if (!m_selected_preset) {
        MessageDialog msg_dlg(nullptr, _L("The selected preset is null!"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    wxCommandEvent e(EVT_CALI_TRAY_CHANGED);
    e.SetEventObject(m_parent);
    wxPostEvent(m_parent, e);
}

void PlaterPresetComboBox::sys_color_changed()
{
    PresetComboBox::sys_color_changed();
    if (clr_picker) {
        clr_picker->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    }
}
} // namespace Slic3r
