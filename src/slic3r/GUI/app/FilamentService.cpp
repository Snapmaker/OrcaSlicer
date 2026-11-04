#include "FilamentService.hpp"
#include "../Plater.hpp"
#include "../GUI_ObjectList.hpp"   // H2 3.1 顶部瘦身：ObjectList 完整型（原经 Plater.hpp→Search.hpp 传递链）        // 阶段 H1 2.2：F1/F3 门面直调（confirm/preset_bundle/app_config/get_tab/device_manager/get_connect_host）
#include "LayoutManager.hpp"     // 阶段 H1 2.1 模式：late_bind 后 sidebar/combos/panel 直读
#include "SlicingController.hpp" // 阶段 H1 2.2：ctor 注入 SlicingController&（schedule_background_process）
#include "PlaterEventRouter.hpp"  // 阶段 H1 2.2：EVT_FILAMENT_USAGE_CHANGED（queue_filament_usage_event）
#include "libslic3r/MixedFilamentConfigRemap.hpp"  // 阶段 I：上游 2.4.1 的耗材 ID 重映射 helpers

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>
#endif

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/colour.h>
#include <wx/string.h>
#include <wx/window.h>

#include "../DeviceManager.hpp"              // MachineObject / DeviceManager / VIRTUAL_TRAY_ID
#include "../GUI_App.hpp"                    // is_support_filament / AppConfig 等（无宿主反查）
#include "../I18N.hpp"                       // _L / into_u8
#include "../MainFrame.hpp"                  // MainFrame::tpMonitor
#include "../MsgDialog.hpp"                  // MessageDialog
#include "../NotificationManager.hpp"        // push_notification
#include "../PartPlate.hpp"                  // PartPlateList
#include "../PresetComboBoxes.hpp"           // PlaterPresetComboBox
#include "../Sidebar.hpp"                    // Sidebar（cpp 级允许）
#include "../Tab.hpp"                        // Tab::select_preset
#include "../filamentsync/FilamentData.hpp"  // FilamentData / MixedFilamentPreviewInfo / is_none_filament / getMainColor
#include "../filamentsync/SyncConfirmDialog.hpp"     // SyncRichConfirmDialog
#include "../filamentsync/SyncFilamentColorDialog.hpp" // SyncFilamentColorDialog
#include "NotificationHub.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r {
namespace GUI {
namespace app {

namespace {


std::string extract_base_filament_name(const std::string& full_name)
{
    std::string base = full_name;
    size_t at_pos = base.find('@');
    if (at_pos != std::string::npos) {
        base = base.substr(0, at_pos);
        base.erase(0, base.find_first_not_of(" \t\n\r"));
        base.erase(base.find_last_not_of(" \t\n\r") + 1);
    }
    return base;
}

// Resolve a machine filament name to a matching local filament preset.
// Filament presets follow the convention "BaseName @Model nozzle",
// e.g. "Generic PA-CF @U1 0.4 nozzle".  Split by '@' to extract the
// base name, trim, and compare exactly — so "Generic PA" does NOT
// accidentally match "Generic PA-CF".
Preset* resolve_filament_preset(PresetBundle* preset_bundle,
    const std::string& filament_name, const std::string& filament_type)
{
    if (!preset_bundle || filament_name.empty())
        return nullptr;

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    for (auto& preset : preset_bundle->filaments) {
        if (!preset.is_compatible || !preset.is_system)
            continue;

        std::string base = extract_base_filament_name(preset.name);
        if (to_lower(base) == to_lower(filament_name)) {
            return &preset;
        }
    }

    const std::string generic_prefix = "Generic ";
    std::string generic_base = generic_prefix + filament_type;
    for (auto& preset : preset_bundle->filaments) {
        if (!preset.is_compatible || !preset.is_system)
            continue;

        std::string base = extract_base_filament_name(preset.name);
        std::string type = preset.config.opt_string("filament_type", static_cast<unsigned int>(0));

        if ((to_lower(type) == to_lower(filament_type)) &&
                (to_lower(base) == to_lower(generic_base))) {
            return &preset;
        }
    }

    return nullptr;
}

void build_design_filament_list(PresetBundle* preset_bundle, std::vector<FilamentData>& out_list)
{
    if (!preset_bundle)
        return;

    const auto& filament_presets = preset_bundle->filament_presets;
    const auto* colors_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const auto* multiColorsOpt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_multi_colors");
    const auto* colorModeOpt   = preset_bundle->project_config.option<ConfigOptionInts>("filament_colour_mode");

    for (size_t i = 0; i < filament_presets.size(); ++i) {
        FilamentData fd;
        fd.m_index = i;

        Preset* preset = preset_bundle->filaments.find_preset(filament_presets[i]);
         if (preset) {
            fd.m_name = preset->label(false);
            const auto* type_opt = preset->config.option<ConfigOptionStrings>("filament_type");
            if (type_opt && !type_opt->values.empty())
                fd.m_type = type_opt->values[0];
        } else {
            fd.m_name = extract_base_filament_name(filament_presets[i]);
        }

        if (fd.m_type.empty()) {
            const auto* type_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_type");
            if (type_opt && i < type_opt->values.size())
                fd.m_type = type_opt->values[i];
        }

        {
            std::vector<std::string> filamentColors;
            FilamentColorMode mode = FilamentColorMode::Segment;

            if (multiColorsOpt && i < multiColorsOpt->values.size() && !multiColorsOpt->values[i].empty())
                filamentColors = SplitFilamentMultiColors(multiColorsOpt->values[i]);
            else if (colors_opt && i < colors_opt->values.size())
                filamentColors = {colors_opt->values[i]};

            if (colorModeOpt && i < colorModeOpt->values.size())
                mode = FilamentColorModeFromConfig(colorModeOpt->values[i]);

            fd.m_color = FilamentColor::FromColors(filamentColors, mode);
        }

        out_list.push_back(std::move(fd));
    }
}

void build_machine_filament_list(PresetBundle* preset_bundle, std::vector<FilamentData>& out_list)
{
    if (!preset_bundle)
        return;

    for (const auto& info : preset_bundle->m_connect_machine_info_list) {
        FilamentData fd;
        fd.m_index = info.index;
        fd.m_name  = info.filament_info;
        fd.m_type  = info.filament_type;

        if (!info.color_info.empty() || !info.multiColors.empty()) {
            fd.m_color = FilamentColor::FromColors(info.multiColors, info.colorMode, info.color_info);
        }

        out_list.push_back(std::move(fd));
    }
}

} // namespace

FilamentService::FilamentService(NotificationHub& hub, Plater* q, DynamicPrintConfig* config,
                             PartPlateList& partplate_list, MainFrame* main_frame,
                             SlicingController& slicing, std::atomic<bool>& filament_usage_sync_pending)
    : m_hub(hub)
    , m_q(q)
    , m_config(config)
    , m_partplate_list(partplate_list)
    , m_main_frame(main_frame)
    , m_slicing(slicing)
    , m_filament_usage_sync_pending(filament_usage_sync_pending)
{
}

void FilamentService::late_bind(LayoutManager& layout)
{
    m_layout = &layout;
}


// BBS.
wxColour FilamentService::get_next_color_for_filament()
{
    static int curr_color_filamenet = 0;
    // refs to https://www.ebaomonthly.com/window/photo/lesson/colorList.htm
    wxColour colors[FILAMENT_SYSTEM_COLORS_NUM] = {
        // ORCA updated all color palette
        wxColour("#00C1AE"),
        wxColour("#F4E2C1"),
        wxColour("#ED1C24"),
        wxColour("#00FF7F"),
        wxColour("#F26722"),
        wxColour("#FFEB31"),
        wxColour("#7841CE"),
        wxColour("#115877"),
        wxColour("#ED1E79"),
        wxColour("#2EBDEF"),
        wxColour("#345B2F"),
        wxColour("#800080"),
        wxColour("#FA8173"),
        wxColour("#800000"),
        wxColour("#F7B763"),
        wxColour("#A4C41E"),
    };
    return colors[curr_color_filamenet++ % FILAMENT_SYSTEM_COLORS_NUM];
}

void FilamentService::on_filaments_delete(size_t num_filaments, size_t filament_id, int replace_filament_id, const std::vector<unsigned char>& is_mixed_snapshot)
{
    // only update elements in plater
    update_filament_colors_in_full_config();

    // update fisrt print sequence and other layer sequence
    // move to partplate->on_filament_deleted
    /*Slic3r::GUI::PartPlateList &plate_list = get_partplate_list();
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate *part_plate = plate_list.get_plate(i);
        part_plate->update_first_layer_print_sequence_when_delete_filament(filament_id);
    }*/

    // Consume remap before updating volumes
    // This is used when merging mixed filaments to properly remap object filament IDs
    PresetBundle *preset_bundle = m_q->preset_bundle();
    std::vector<unsigned int> id_remap;
    if (preset_bundle != nullptr)
        id_remap = preset_bundle->consume_last_filament_id_remap();

    // Build state map for remap if available.
    // Use the remap for both pure-delete and merge paths so that mixed
    // filaments deleted by remove_physical_filament are correctly mapped
    // to NONE instead of being shifted onto wrong IDs.
    EnforcerBlockerStateMap state_map;
    bool should_remap_states = false;
    if (!id_remap.empty()) {
        should_remap_states = true;
        if (replace_filament_id >= 0) {
            // Merge: inject the merge target into the remap so the deleted
            // physical filament maps to the target instead of 0.
            size_t old_1based = filament_id + 1;
            size_t new_1based = replace_filament_id + 1;
            if (old_1based < id_remap.size())
                id_remap[old_1based] = (unsigned int)new_1based;
        }
        for (size_t i = 0; i < state_map.size(); ++i)
            state_map[i] = EnforcerBlockerType(i);
        for (size_t i = 1; i < state_map.size(); ++i) {
            const unsigned int mapped = i < id_remap.size() ? id_remap[i] : 0;
            if (mapped == 0 || mapped >= state_map.size() || mapped > num_filaments)
                state_map[i] = EnforcerBlockerType::NONE;
            else
                state_map[i] = EnforcerBlockerType(mapped);
        }
    }

    // update mmu paint data
    // During batch physical deletion, skip per-deletion painting remap —
    // cleanup applies ONE composite remap after the loop covering all K
    // deletions.  Applying both would double-remap.
    if (m_batch_physical_deletion == 0) {
        for (ModelObject* mo : m_q->model().objects) {
            for (ModelVolume* mv : mo->volumes) {
                if (should_remap_states) {
                    mv->remap_extruder_ids(num_filaments, state_map);
                } else {
                    mv->update_extruder_count_when_delete_filament(num_filaments, filament_id + 1,
                                                                   replace_filament_id + 1); // this function is 1 base
                }
            }
        }
    }

    // update UI
    m_layout->sidebar->on_filaments_delete(filament_id);

    // An explicit remap also covers mixed-row deletion/cascade cases that cannot
    // be expressed by the naive decrement path below.
    if (should_remap_states) {
        remap_dynamic_config_feature_filament_ids(*m_config, id_remap, num_filaments);
    } else {
        for (const std::string &key : mixed_filament_feature_keys()) {
            if (!m_config->has(key))
                continue;

            if (m_config->opt_int(key) == static_cast<int>(filament_id + 1)) {
                m_config->erase(key);
            } else {
                const int old_id = m_config->opt_int(key);
                const int new_id = old_id > static_cast<int>(filament_id) ? old_id - 1 : old_id;
                m_config->set(key, new_id);
            }
        }
    }

    // update object/volume/support(object and volume) filament id
    if (should_remap_states) {
        for (ModelObject* mo : m_q->model().objects) {
            remap_model_config_filament_ids(mo->config, id_remap, num_filaments);
            for (ModelVolume* mv : mo->volumes)
                remap_model_config_filament_ids(mv->config, id_remap, num_filaments);
            for (auto &layer_range : mo->layer_config_ranges)
                remap_model_config_filament_ids(layer_range.second, id_remap, num_filaments);
        }
        // Batch physical deletion defers list and scene refresh until its final
        // composite rebuild; other deletion paths refresh immediately.
        if (m_batch_physical_deletion == 0) {
            m_layout->sidebar->obj_list()->update_objects_list_filament_column(
                std::max<size_t>(m_layout->sidebar->combos_filament().size(), 1));
            m_layout->sidebar->obj_list()->refresh_layer_range_filament_items();
        }
    } else {
        m_layout->sidebar->obj_list()->update_objects_list_filament_column_when_delete_filament(filament_id, num_filaments, replace_filament_id);
    }

    // update customize gcode
    for (auto item = m_q->model().plates_custom_gcodes.begin(); item != m_q->model().plates_custom_gcodes.end(); ++item) {
        auto iter = std::remove_if(item->second.gcodes.begin(), item->second.gcodes.end(), [filament_id](const CustomGCode::Item& gcode_item) {
            return (gcode_item.type == CustomGCode::Type::ToolChange && gcode_item.extruder == filament_id + 1);
        });
        if (replace_filament_id == -1)
            item->second.gcodes.erase(iter, item->second.gcodes.end());
        else if (iter != item->second.gcodes.end()) {
            iter->extruder = replace_filament_id + 1;
        }

        for (auto& item : item->second.gcodes) {
            if (item.type == CustomGCode::Type::ToolChange && item.extruder > filament_id)
                item.extruder--;
        }
    }
}

// BBS.
void FilamentService::on_filaments_change(size_t num_filaments)
{
    // only update elements in plater
    update_filament_colors_in_full_config();

    const size_t old_num_filaments = m_layout->sidebar->combos_filament().size();
    const bool auto_generate_before = MixedFilamentManager::auto_generate_enabled();
    const bool allow_auto_gradients = m_q->confirm_auto_generated_gradients(num_filaments);
    auto summarize_uint_vector = [](const std::vector<unsigned int> &values, size_t max_items = 24) {
        std::string out = "[";
        const size_t n = std::min(values.size(), max_items);
        for (size_t i = 0; i < n; ++i) {
            if (i > 0)
                out += ",";
            out += std::to_string(values[i]);
        }
        if (values.size() > n)
            out += ",...";
        out += "]";
        return out;
    };
    auto summarize_used_states = [](const std::vector<bool> &used, size_t max_items = 24) {
        std::string out = "[";
        size_t total = 0;
        size_t emitted = 0;
        for (size_t i = 1; i < used.size(); ++i) {
            if (!used[i])
                continue;
            ++total;
            if (emitted < max_items) {
                if (emitted > 0)
                    out += ",";
                out += std::to_string(i);
                ++emitted;
            }
        }
        if (total > emitted)
            out += ",...";
        out += "] total=" + std::to_string(total);
        return out;
    };
    PresetBundle *preset_bundle = m_q->preset_bundle();
    if (preset_bundle != nullptr && auto_generate_before && !allow_auto_gradients)
        preset_bundle->update_multi_material_filament_presets(size_t(-1), old_num_filaments);
    // Consume remap before sidebar refresh, which may trigger config sync
    // paths that regenerate mixed filaments and clear this remap buffer.
    std::vector<unsigned int> id_remap;
    if (preset_bundle != nullptr)
        id_remap = preset_bundle->consume_last_filament_id_remap();

    size_t total_filaments = num_filaments;
    if (preset_bundle != nullptr)
        total_filaments = preset_bundle->mixed_filaments.total_filaments(num_filaments);

    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType(i);

    bool have_explicit_remap = false;
    bool should_remap_states = false;
    if (!id_remap.empty()) {
        have_explicit_remap = true;
        should_remap_states = true;
        for (size_t i = 1; i < state_map.size(); ++i) {
            const unsigned int mapped = i < id_remap.size() ? id_remap[i] : 0;
            if (mapped == 0 || mapped >= state_map.size() || mapped > total_filaments)
                state_map[i] = EnforcerBlockerType::NONE;
            else
                state_map[i] = EnforcerBlockerType(mapped);
        }
    }

    size_t changed_entries = 0;
    std::string changed_map_preview = "[";
    for (size_t i = 1; i < state_map.size(); ++i) {
        const unsigned int mapped = unsigned(state_map[i]);
        if (mapped == i)
            continue;
        ++changed_entries;
        if (changed_entries <= 24) {
            if (changed_entries > 1)
                changed_map_preview += ",";
            changed_map_preview += std::to_string(i) + "->" + std::to_string(mapped);
        }
    }
    if (changed_entries > 24)
        changed_map_preview += ",...";
    changed_map_preview += "]";
    BOOST_LOG_TRIVIAL(warning) << "MF_REMAP on_filaments_change"
                            << " old_physical=" << old_num_filaments
                            << " new_physical=" << num_filaments
                            << " total_filaments=" << total_filaments
                            << " id_remap_size=" << id_remap.size()
                            << " id_remap=" << summarize_uint_vector(id_remap)
                            << " explicit_remap=" << (have_explicit_remap ? 1 : 0)
                            << " should_remap_states=" << (should_remap_states ? 1 : 0)
                            << " changed_entries=" << changed_entries
                            << " changed_map=" << changed_map_preview;

    size_t obj_idx = 0;
    for (ModelObject* mo : m_q->model().objects) {
        size_t vol_idx = 0;
        for (ModelVolume* mv : mo->volumes) {
            std::string used_before;
            const bool has_mmu_paint = (mv != nullptr && !mv->mmu_segmentation_facets.empty());
            if (has_mmu_paint)
                used_before = summarize_used_states(mv->mmu_segmentation_facets.get_data().used_states);

            if (should_remap_states)
                mv->remap_extruder_ids(total_filaments, state_map);
            else
                mv->update_extruder_count(total_filaments);

            if (has_mmu_paint) {
                const std::string used_after = summarize_used_states(mv->mmu_segmentation_facets.get_data().used_states);
                BOOST_LOG_TRIVIAL(warning) << "MF_REMAP volume"
                                        << " obj_idx=" << obj_idx
                                        << " vol_idx=" << vol_idx
                                        << " obj_name=" << mo->name
                                        << " vol_name=" << mv->name
                                        << " before=" << used_before
                                        << " after=" << used_after;
            }
            ++vol_idx;
        }
        ++obj_idx;
    }

    // Keep UI refresh after model remap. Some UI update paths may trigger
    // scene/model sync that assumes already-remapped MMU state.
    m_layout->sidebar->on_filaments_change(num_filaments);
    m_layout->sidebar->obj_list()->update_objects_list_filament_column(num_filaments);

    Slic3r::GUI::PartPlateList &plate_list = m_partplate_list;
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate* part_plate = plate_list.get_plate(i);
        part_plate->update_first_layer_print_sequence(num_filaments);
    }

    // Adding/removing filament is a parameter change too: reset dismissal.
    m_hub.nm()->reset_pla_petg_mix_warning();
}

bool FilamentService::update_filament_colors_in_full_config()
{
    DynamicPrintConfig& project_config = m_q->preset_bundle()->project_config;
    ConfigOptionStrings* color_opt = project_config.option<ConfigOptionStrings>("filament_colour");

    m_config->option<ConfigOptionStrings>("filament_colour")->values = color_opt->values;
    return true;
}

void FilamentService::notify_filament_usage_changed()
{
    if (m_filament_usage_sync_pending.exchange(true))
        return;

    if (m_q)
        wxQueueEvent(m_q, new SimpleEvent(EVT_FILAMENT_USAGE_CHANGED, m_q));
}

void FilamentService::force_filament_colors_update()
{
//BBS: filament_color logic has been moved out of filament setting
#if 0
    bool update_scheduled = false;
    DynamicPrintConfig* config = m_config;
    const std::vector<std::string> filament_presets = m_q->preset_bundle()->filament_presets;
    if (filament_presets.size() > 1 &&
        m_config->option<ConfigOptionStrings>("filament_colour")->values.size() == filament_presets.size())
    {
        const PresetCollection& filaments = m_q->preset_bundle()->filaments;
        std::vector<std::string> filament_colors;
        filament_colors.reserve(filament_presets.size());

        for (const std::string& filament_preset : filament_presets)
            filament_colors.push_back(filaments.find_preset(filament_preset, true)->config.opt_string("filament_colour", (unsigned)0));

        if (config->option<ConfigOptionStrings>("filament_colour")->values != filament_colors) {
            config->option<ConfigOptionStrings>("filament_colour")->values = filament_colors;
            update_scheduled = true;
        }
    }

    if (update_scheduled) {
        update();
        m_layout->sidebar->obj_list()->update_filament_colors();
    }

    if (m_main_frame->is_loaded())
        m_slicing.schedule_background_process();
#endif
}

void FilamentService::force_print_bed_update()
{
    // Fill in the printer model key with something which cannot possibly be valid, so that Plater::on_config_change() will update the print bed
    // once a new Printer profile config is loaded.
    m_config->opt_string("printer_model", true) = "bbl_empty";
}

void FilamentService::set_global_filament_map(const std::vector<int>& filament_map)
{
    auto& project_config = m_q->preset_bundle()->project_config;
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_map;
}

std::vector<int> FilamentService::get_global_filament_map() const
{
    auto& project_config = m_q->preset_bundle()->project_config;
    return project_config.option<ConfigOptionInts>("filament_map")->values;
}


std::map<int, DynamicPrintConfig> FilamentService::build_filament_ams_list(MachineObject* obj)
{
    std::map<int, DynamicPrintConfig> filament_ams_list;
    if (!obj) return filament_ams_list;

    auto vt_tray = obj->vt_tray;
    if (obj->ams_support_virtual_tray) {
        DynamicPrintConfig vt_tray_config;
        vt_tray_config.set_key_value("filament_id", new ConfigOptionStrings{ vt_tray.setting_id });
        vt_tray_config.set_key_value("tag_uid", new ConfigOptionStrings{ vt_tray.tag_uid });
        vt_tray_config.set_key_value("filament_type", new ConfigOptionStrings{ vt_tray.type });
        vt_tray_config.set_key_value("tray_name", new ConfigOptionStrings{ std::string("Ext") });
        vt_tray_config.set_key_value("filament_colour", new ConfigOptionStrings{ into_u8(wxColour("#" + vt_tray.color).GetAsString(wxC2S_HTML_SYNTAX)) });
        vt_tray_config.set_key_value("filament_exist", new ConfigOptionBools{ true });

        vt_tray_config.set_key_value("filament_multi_colors", new ConfigOptionStrings{});
        for (int i = 0; i < vt_tray.cols.size(); ++i) {
            vt_tray_config.opt<ConfigOptionStrings>("filament_multi_colors")->values.push_back(into_u8(wxColour("#" + vt_tray.cols[i]).GetAsString(wxC2S_HTML_SYNTAX)));
        }
        filament_ams_list.emplace(VIRTUAL_TRAY_ID, std::move(vt_tray_config));
    }

    auto list = obj->amsList;
    for (auto ams : list) {
        char n = ams.first.front() - '0' + 'A';
        for (auto tray : ams.second->trayList) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                << boost::format(": ams %1% tray %2% id %3% color %4%") % ams.first % tray.first % tray.second->setting_id % tray.second->color;
            char t = tray.first.front() - '0' + '1';
            DynamicPrintConfig tray_config;
            tray_config.set_key_value("filament_id", new ConfigOptionStrings{ tray.second->setting_id });
            tray_config.set_key_value("tag_uid", new ConfigOptionStrings{ tray.second->tag_uid });
            tray_config.set_key_value("filament_type", new ConfigOptionStrings{ tray.second->type });
            tray_config.set_key_value("tray_name", new ConfigOptionStrings{ std::string(1, n) + std::string(1, t) });
            tray_config.set_key_value("filament_colour", new ConfigOptionStrings{ into_u8(wxColour("#" + tray.second->color).GetAsString(wxC2S_HTML_SYNTAX)) });
            tray_config.set_key_value("filament_exist", new ConfigOptionBools{ tray.second->is_exists });

            tray_config.set_key_value("filament_multi_colors", new ConfigOptionStrings{});
            for (int i = 0; i < tray.second->cols.size(); ++i) {
                tray_config.opt<ConfigOptionStrings>("filament_multi_colors")->values.push_back(into_u8(wxColour("#" + tray.second->cols[i]).GetAsString(wxC2S_HTML_SYNTAX)));
            }
            filament_ams_list.emplace(((n - 'A') * 4 + t - '1'), std::move(tray_config));
        }
    }
    return filament_ams_list;
}

void FilamentService::load_ams_list(std::string const &device, MachineObject* obj)
{
    std::map<int, DynamicPrintConfig> filament_ams_list = build_filament_ams_list(obj);

    m_ams_list_device = device;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1% items") % filament_ams_list.size();
    if (m_q->preset_bundle()->filament_ams_list == filament_ams_list)
        return;
    m_q->preset_bundle()->filament_ams_list = filament_ams_list;

    for (auto c : m_layout->sidebar->combos_filament())
        c->update();
}

void FilamentService::sync_ams_list()
{
    // Force load ams list
    auto obj = m_q->device_manager()->get_selected_machine();
    if (obj)
        m_layout->sidebar->load_ams_list(obj->dev_id, obj);

    auto & list = m_q->preset_bundle()->filament_ams_list;
    if (list.empty()) {
        MessageDialog dlg(m_layout->sidebar,
            _L("No AMS filaments. Please select a printer in 'Device' page to load AMS info."),
            _L("Sync filaments with AMS"), wxOK);
        dlg.ShowModal();
        return;
    }
    std::string ams_filament_ids = m_q->app_config()->get("ams_filament_ids", m_ams_list_device);
    std::vector<std::string> list2;
    if (!ams_filament_ids.empty())
        boost::algorithm::split(list2, ams_filament_ids, boost::algorithm::is_any_of(","));
    struct SyncAmsDialog : MessageDialog {
        SyncAmsDialog(wxWindow * parent, bool first): MessageDialog(parent,
            first
                ? _L("Sync filaments with AMS will drop all current selected filament presets and colors. Do you want to continue?")
                : _L("Already did a synchronization, do you want to sync only changes or resync all?"),
            _L("Sync filaments with AMS"), 0)
        {
            if (first) {
                add_button(wxID_YES, true, _L("Yes"));
            } else {
                add_button(wxID_OK, true, _L("Sync"));
                add_button(wxID_YES, false, _L("Resync"));
            }
            add_button(wxID_CANCEL, false, _L("Cancel"));
        }
    } dlg(m_layout->sidebar, ams_filament_ids.empty());
    auto res = dlg.ShowModal();
    if (res == wxID_CANCEL) return;
    list2.resize(list.size());
    auto iter = list.begin();
    for (int i = 0; i < list.size(); ++i, ++iter) {
        auto & ams = iter->second;
        auto filament_id = ams.opt_string("filament_id", 0u);
        ams.set_key_value("filament_changed", new ConfigOptionBool{res == wxID_YES || list2[i] != filament_id});
        list2[i] = filament_id;
    }

    // BBS:Record consumables information before synchronization
    std::vector<string> color_before_sync;
    std::vector<bool>   is_support_before;
    DynamicPrintConfig& project_config = m_q->preset_bundle()->project_config;
    ConfigOptionStrings* color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
    for (int i = 0; i < m_layout->sidebar->combos_filament().size(); ++i) {
        is_support_before.push_back(is_support_filament(i));
        color_before_sync.push_back(color_opt->values[i]);
    }

    unsigned int unknowns = 0;
    auto n = m_q->preset_bundle()->sync_ams_list(unknowns);
    if (n == 0) {
        MessageDialog dlg(m_layout->sidebar,
            _L("There are no compatible filaments, and sync is not performed."),
            _L("Sync filaments with AMS"), wxOK);
        dlg.ShowModal();
        return;
    }
    ams_filament_ids = boost::algorithm::join(list2, ",");
    m_q->app_config()->set("ams_filament_ids", m_ams_list_device, ams_filament_ids);
    if (unknowns > 0) {
        MessageDialog dlg(m_layout->sidebar,
            _L("There are some unknown filaments mapped to generic preset. Please update Snapmaker Orca or restart Snapmaker Orca to check if there is an update to system presets."),
            _L("Sync filaments with AMS"), wxOK);
        dlg.ShowModal();
    }
    on_filaments_change(n);
    for (auto& c : m_layout->sidebar->combos_filament())
        c->update();
    m_q->get_tab(static_cast<int>(Preset::TYPE_FILAMENT))->select_preset(m_q->preset_bundle()->filament_presets[0]);
    m_q->preset_bundle()->export_selections(*m_q->app_config());
    m_layout->sidebar->update_dynamic_filament_list();
    // Expand filament list
    m_layout->sidebar->filament_panel()->Show();
    m_layout->sidebar->filament_panel()->SetMaxSize({-1, -1});
    // BBS:Synchronized consumables information
    // auto calculation of flushing volumes
    for (int i = 0; i < m_layout->sidebar->combos_filament().size(); ++i) {
        if (i >= color_before_sync.size()) {
            m_layout->sidebar->auto_calc_flushing_volumes(i);
        }
        else {
            // if color changed
            if (color_before_sync[i] != color_opt->values[i]) {
                m_layout->sidebar->auto_calc_flushing_volumes(i);
            }
            // color don't change, but changes between supporting filament and non supporting filament
            else {
                bool flag = is_support_filament(i);
                if (flag != is_support_before[i])
                    m_layout->sidebar->auto_calc_flushing_volumes(i);
            }
        }
    }
    m_layout->sidebar->Layout();
}



} // namespace app
} // namespace GUI
} // namespace Slic3r
