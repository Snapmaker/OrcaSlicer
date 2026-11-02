// MixingGateKeeper 实现（阶段 B3）。
// 迁移纪律：门控函数族 + helper 簇自 Plater.cpp 逐字迁入；改写仅限宿主解引用
// （priv 成员 / 全局反查 / 通知管理器 / 工作配置 → GateKeeperHost / NotificationHub）。
// 零行为纪律：门控条件、去抖窗口、通知文本逐字保持（close_validate_* 精确文本匹配）。
// 依赖规则：本 cpp 禁 include Plater.hpp / Sidebar.hpp / GUI_App.hpp。
#include "MixingGateKeeper.hpp"
#include "../Plater.hpp"        // 阶段 H1 2.1：F1/F3 门面直调（has_sliceable/preset_bundle/app_config 桥接）
#include "../MainFrame.hpp"      // 阶段 H1 2.1：update_slice_print_status（typed 注入 MainFrame*）
#include "LayoutManager.hpp"     // 阶段 H1 2.1：late_bind 后 view3D 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.1：View3D::is_dragging（View3D 定义于 GUI_Preview.hpp）

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include <boost/log/trivial.hpp>

#include "../I18N.hpp"
#include "../format.hpp"                   // Slic3r::GUI::format（冷盘文本）
#include "../MixedColorMatchHelpers.hpp"   // is_filament_compatible
#include "../MsgDialog.hpp"                // MessageDialog
#include "../NotificationManager.hpp"      // NotificationManager::close_validate_* / push_*
#include "../PartPlate.hpp"                // PartPlate / PartPlateList
#include "NotificationHub.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {
namespace app {

MixingGateKeeper::MixingGateKeeper(NotificationHub& hub, Plater* q, PartPlateList& partplate_list,
                              const DynamicPrintConfig* config, Model& model, MainFrame* main_frame)
    : m_hub(hub)
    , m_q(q)
    , m_partplate_list(partplate_list)
    , m_config(config)
    , m_model(model)
    , m_main_frame(main_frame)
{
}

void MixingGateKeeper::late_bind(LayoutManager& layout)
{
    m_layout = &layout;
}

// ===== 自 Plater.cpp:262-571 迁入的 helper 簇 =====
static std::string tr_u8(const char* s)
{
    if (s == nullptr)
        return std::string();
    const wxString        ws  = _L(s);
    const wxScopedCharBuffer buf = ws.utf8_str();
    if (buf.data() == nullptr)
        return std::string();
    return std::string(buf.data(), buf.length());
}

// Build a user-facing label for a single filament slot, in the form
// "[n] PresetName". Falls back to "[n] (unknown)" if the preset can't be
// resolved — same defensive style as the null checks in check_filament_temp_mixing.
std::string MixingGateKeeper::filament_display_label(int slot_1based)
{
    const int      slot_0_based = slot_1based - 1;
    PresetBundle*  bundle       = m_q->preset_bundle();
    std::string    name         = tr_u8("unknown");
    if (bundle != nullptr
        && slot_0_based >= 0
        && slot_0_based < static_cast<int>(bundle->filament_presets.size()))
    {
        const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot_0_based], true);
        if (preset != nullptr)
            name = preset->name;
    }
    return std::string("[") + std::to_string(slot_1based) + "] " + name;
}

// Compose a single comma-separated line of "[n] Preset" labels from a list
// of 1-based slot numbers. Used inside High/Low temperature group lines.
std::string MixingGateKeeper::format_filament_slot_list(const std::vector<int>& slots_1based)
{
    std::string out;
    for (size_t i = 0; i < slots_1based.size(); ++i)
    {
        if (i != 0)
            out += ", ";
        out += filament_display_label(slots_1based[i]);
    }
    return out;
}

// Append the High/Low temperature grouping block (two lines, only groups that
// are non-empty) to `out`. Called by both the single-plate and slice-all
// text builders so the layout stays identical.
void MixingGateKeeper::append_filament_temp_mixing_groups(std::string& out, const FilamentTempMixingDetail& detail)
{
    if (!detail.high_temp_slots_1based.empty())
    {
        out += tr_u8("High temperature:");
        out += " ";
        out += format_filament_slot_list(detail.high_temp_slots_1based);
        out += "\n";
    }
    if (!detail.low_temp_slots_1based.empty())
    {
        out += tr_u8("Low temperature:");
        out += " ";
        out += format_filament_slot_list(detail.low_temp_slots_1based);
        out += "\n";
    }
}

std::string MixingGateKeeper::filament_temp_mixing_warning_text(const FilamentTempMixingDetail& detail)
{
    std::string out = tr_u8("Detected both high and low temperature materials. "
                            "Mixed printing may result in extruder clogging, "
                            "nozzle damage, or layer adhesion issues.");
    out += "\n\n";
    append_filament_temp_mixing_groups(out, detail);
    return out;
}

std::string MixingGateKeeper::filament_temp_mixing_error_text(const FilamentTempMixingDetail& detail)
{
    std::string out = tr_u8("Detected both high and low temperature materials. "
                            "Mixed printing may result in extruder clogging, "
                            "nozzle damage, or layer adhesion issues.");
    out += "\n\n";
    append_filament_temp_mixing_groups(out, detail);
    out += "\n";
    out += tr_u8("To continue printing, enable \"Allow high/low temperature filament mixing\" in Preferences.");
    return out;
}

// Build the single-line banner for the flow-ratio-zero pre-slice blocker.
// Layout (single line, semicolon-separated):
//   "Flow ratio is 0%, ... no valid toolpath. Filament(s): [1] PLA White; [3] PETG Black. Please set ..."
// Uses std::string += concatenation (no boost::format) to avoid format_error exceptions,
// matching the filament_temp_mixing_*_text blueprints above.
std::string MixingGateKeeper::flow_ratio_zero_error_text(const FlowRatioZeroDetail& detail)
{
    std::string out = tr_u8("Flow ratio is 0%, resulting in zero extrusion "
                            "and no valid toolpath. ");
    out += tr_u8("Filament(s):");
    out += " ";
    for (size_t i = 0; i < detail.offender_slots_1based.size(); ++i)
    {
        if (i != 0)
            out += "; ";
        out += filament_display_label(detail.offender_slots_1based[i]);
    }
    out += ". ";
    out += tr_u8("Please set the flow ratio to a value greater than 0.");
    return out;
}

// Slice-all variants: list every plate that has a mixing conflict, each with
// its own High/Low grouping. Plates without conflicts are not shown.
std::string MixingGateKeeper::filament_temp_mixing_warning_text_slice_all(const std::vector<PlateMixingInfo>& plates)
{
    std::string out = tr_u8("The following plates contain mixed high and low temperature materials:");
    out += "\n\n";
    for (const PlateMixingInfo& info : plates)
    {
        out += tr_u8("Plate");
        out += " " + std::to_string(info.plate_index_1based) + "\n";
        append_filament_temp_mixing_groups(out, info.detail);
        out += "\n";
    }
    return out;
}

std::string MixingGateKeeper::filament_temp_mixing_error_text_slice_all(const std::vector<PlateMixingInfo>& plates)
{
    std::string out = tr_u8("The following plates contain mixed high and low temperature materials:");
    out += "\n\n";
    for (const PlateMixingInfo& info : plates)
    {
        out += tr_u8("Plate");
        out += " " + std::to_string(info.plate_index_1based) + "\n";
        append_filament_temp_mixing_groups(out, info.detail);
        out += "\n";
    }
    out += tr_u8("To continue printing, enable \"Allow high/low temperature filament mixing\" in Preferences.");
    return out;
}

/// \brief Compose error text for unsupported filaments on the Cool Steel Plate.
/// \param[in] unsupported_slots_1_based  Offending filament slots (1-based).
/// \return Single-line string in the form
///         "Cool Steel Plate is not recommended for printing [filaments]. ..."
std::string MixingGateKeeper::cold_plate_error_text(
    const std::vector<int>&  unsupported_slots_1_based)
{
    return Slic3r::GUI::format(
        _u8L("The Cool Steel Plate is not recommended for %1%. To continue printing, set the bed temperature above 0°C for this filament."),
        format_filament_slot_list(unsupported_slots_1_based));
}

/// \brief Compose TPU serious-warning text for the Cool Steel Plate.
/// \param[in] tpu_slots_1_based   TPU filament slots (1-based).
std::string MixingGateKeeper::cold_plate_serious_warning_text(
    const std::vector<int>&  tpu_slots_1_based)
{
    return Slic3r::GUI::format(
        _u8L("The Cool Steel Plate is not recommended for %1%. It may be hard to remove. Use a textured PEI plate or heat the bed."),
        format_filament_slot_list(tpu_slots_1_based));
}

static bool model_object_is_on_plate(PartPlate* plate, size_t obj_idx, const ModelObject* model_object)
{
    if (model_object == nullptr)
        return false;

    if (plate == nullptr)
        return false;

    const int object_index = static_cast<int>(obj_idx);
    const int instance_count = static_cast<int>(model_object->instances.size());
    for (int instance_index = 0; instance_index < instance_count; ++instance_index)
    {
        if (plate->contain_instance(object_index, instance_index))
            return true;
    }

    return false;
}

static void collect_filament_slots_from_config(
    const DynamicPrintConfig& config,
    int num_filaments, std::set<int>& used_slots_0_based)
{
    // Support/feature filaments
    static const std::vector<const char*> feature_keys = {
        "support_filament",
        "support_interface_filament",
        "wall_filament",
        "sparse_infill_filament",
        "solid_infill_filament",
        "wipe_tower_filament"
    };
    for (const char* key : feature_keys)
    {
        const ConfigOptionInt* option = config.option<ConfigOptionInt>(key);
        if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
            used_slots_0_based.insert(option->value - 1);
    }

    // Primary filament (extruder)
    const ConfigOptionInt* extruder_option = config.option<ConfigOptionInt>("extruder");
    if (extruder_option != nullptr && extruder_option->value >= 1 && extruder_option->value <= num_filaments)
        used_slots_0_based.insert(extruder_option->value - 1);
}

static void collect_filament_slots_from_model_config(
    const ModelConfigObject& config,
    int num_filaments, std::set<int>& used_slots_0_based)
{
    // Primary filament (extruder)
    if (config.has("extruder"))
    {
        const int extruder_id = config.extruder();
        if (extruder_id >= 1 && extruder_id <= num_filaments)
            used_slots_0_based.insert(extruder_id - 1);
    }

    // Support/feature filaments
    static const std::vector<const char*> feature_keys = {
        "support_filament",
        "support_interface_filament",
        "wall_filament",
        "sparse_infill_filament",
        "solid_infill_filament",
        "wipe_tower_filament"
    };
    for (const char* key : feature_keys)
    {
        if (config.has(key))
        {
            const int val = config.opt_int(key);
            if (val >= 1 && val <= num_filaments)
                used_slots_0_based.insert(val - 1);
        }
    }
}

/// \brief Collect the 0-based filament slots actually used by objects on the given plate.
/// \details Mirrors the slot-collection block of MixingGateKeeper::check_filament_temp_mixing so that
///          cold-plate incompatibility checking uses the same definition of "used filament".
///          Includes: plate config, per-object/volume configs, plus Plater working config
///          (wipe_tower / support / wall / infill defaults when any object uses extruder=0).
/// \param[in]  plate                 Non-null target plate.
/// \param[in]  num_filaments         Total number of filaments in the current configuration.
/// \param[in]  plater_working_config The Plater's current working config (m_config).
/// \param[in]  full_cfg              Merged full config (for resolving the global default extruder).
/// \param[out] used_slots_0_based    Populated with every 0-based filament slot referenced on the plate.
void MixingGateKeeper::collect_used_filament_slots_on_plate(
    PartPlate* plate,
    int num_filaments,
    const DynamicPrintConfig* plater_working_config,
    const DynamicPrintConfig& full_cfg,
    std::set<int>& used_slots_0_based) const
{
    if (plate == nullptr || num_filaments <= 0)
        return;

    // Plate-local config
    collect_filament_slots_from_config(*plate->config(), num_filaments, used_slots_0_based);

    // Per-object + per-volume config
    bool uses_default_extruder = false;
    for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
        const ModelObject* model_object = m_model.objects[obj_idx];
        if (!model_object_is_on_plate(plate, obj_idx, model_object))
            continue;
        collect_filament_slots_from_model_config(model_object->config, num_filaments, used_slots_0_based);

        if (!model_object->config.has("extruder") || model_object->config.extruder() == 0)
            uses_default_extruder = true;

        for (const ModelVolume* model_volume : model_object->volumes) {
            collect_filament_slots_from_model_config(model_volume->config, num_filaments, used_slots_0_based);
            for (int extruder_id : model_volume->get_extruders()) {
                if (extruder_id >= 1 && extruder_id <= num_filaments)
                    used_slots_0_based.insert(extruder_id - 1);
            }
        }
    }

    // Plater working config — global features (always apply) + feature-specific
    // keys (only when at least one object uses the default extruder).
    if (plater_working_config != nullptr) {
        static const std::vector<const char*> always_collect = {"wipe_tower_filament", "support_filament", "support_interface_filament"};
        for (const char* key : always_collect) {
            const ConfigOptionInt* option = plater_working_config->option<ConfigOptionInt>(key);
            if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                used_slots_0_based.insert(option->value - 1);
        }

        if (uses_default_extruder) {
            static const std::vector<const char*> default_keys = {"wall_filament", "sparse_infill_filament", "solid_infill_filament"};
            for (const char* key : default_keys) {
                const ConfigOptionInt* option = plater_working_config->option<ConfigOptionInt>(key);
                if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                    used_slots_0_based.insert(option->value - 1);
            }
        }
    }

    // Resolve the global default extruder if any object on this plate uses extruder=0.
    // plater_working_config does not include "extruder"; read from full_cfg instead.
    if (uses_default_extruder) {
        const ConfigOptionInt* extruder_opt = full_cfg.option<ConfigOptionInt>("extruder");
        if (extruder_opt != nullptr && extruder_opt->value >= 1 && extruder_opt->value <= num_filaments)
            used_slots_0_based.insert(extruder_opt->value - 1);
    }
}

// ===== 门控函数族（自 Plater.cpp 逐字迁入）=====
bool MixingGateKeeper::check_filament_temp_mixing(int plate_index)
{
    FilamentTempMixingDetail unused;
    return check_filament_temp_mixing(plate_index, unused);
}

bool MixingGateKeeper::check_filament_temp_mixing(int plate_index, FilamentTempMixingDetail& detail)
{
    detail.high_temp_slots_1based.clear();
    detail.low_temp_slots_1based.clear();

    // Boundary checks
    PartPlate* plate = nullptr;
    const DynamicPrintConfig&  full_cfg             = m_q->preset_bundle()->full_config();
    const ConfigOptionStrings* filament_type_option = full_cfg.option<ConfigOptionStrings>("filament_type");
    {
        if (filament_type_option == nullptr || filament_type_option->values.empty())
            return true;

        const int plate_count = m_partplate_list.get_plate_count();
        if (plate_index < 0 || plate_index >= plate_count)
            return true;

        plate = m_partplate_list.get_plate(plate_index);
        if (plate == nullptr)
            return true;

        bool has_object_on_plate = false;
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx)
        {
            const ModelObject* model_object = m_model.objects[obj_idx];
            if (model_object_is_on_plate(plate, obj_idx, model_object))
            {
                has_object_on_plate = true;
                break;
            }
        }
        if (!has_object_on_plate)
            return true;
    }

    // Collect filament slots actually used on this plate
    std::set<int> used_slots_0_based;
    {
        // Plate config
        const int num_filaments = static_cast<int>(filament_type_option->values.size());
        collect_filament_slots_from_config(*plate->config(), num_filaments, used_slots_0_based);

        // ModelObject config
        bool uses_default_extruder = false;
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx)
        {
            const ModelObject* model_object = m_model.objects[obj_idx];
            if (!model_object_is_on_plate(plate, obj_idx, model_object))
                continue;
            collect_filament_slots_from_model_config(model_object->config, num_filaments, used_slots_0_based);

            if (!model_object->config.has("extruder") || model_object->config.extruder() == 0)
                uses_default_extruder = true;

            // ModelVolume config
            for (const ModelVolume* model_volume : model_object->volumes)
            {
                collect_filament_slots_from_model_config(model_volume->config, num_filaments, used_slots_0_based);
                for (int extruder_id : model_volume->get_extruders())
                {
                    if (extruder_id >= 1 && extruder_id <= num_filaments)
                        used_slots_0_based.insert(extruder_id - 1);
                }
            }
        }

        // Collect from the Plater working config. The approach balances
        // sensitivity against false positives:
        // - Global features (wipe tower, support) always apply → always collected.
        // - Feature-specific keys (wall_filament, infill) depend on the global
        //   process defaults. They are only collected when at least one object
        //   on the plate uses the default extruder (e=0), which means those
        //   defaults WILL affect the actual slicing output.
        {
            // Always collect: features that cannot be overridden per-object.
            static const std::vector<const char*> always_collect = {"wipe_tower_filament", "support_filament", "support_interface_filament"};
            for (const char* key : always_collect)
            {
                const ConfigOptionInt* option = m_config->option<ConfigOptionInt>(key);
                if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                    used_slots_0_based.insert(option->value - 1);
            }

            // If any object uses e=0, the global process defaults for
            // wall / infill extruders apply and must be collected.
            if (uses_default_extruder)
            {
                static const std::vector<const char*> default_keys = {"wall_filament", "sparse_infill_filament", "solid_infill_filament"};
                for (const char* key : default_keys)
                {
                    const ConfigOptionInt* option = m_config->option<ConfigOptionInt>(key);
                    if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                        used_slots_0_based.insert(option->value - 1);
                }
            }
        }

        // Resolve the global default extruder if any object on this plate
        // uses extruder=0. The priv working config does not include the "extruder" key
        // (it is not in the initializer list at priv constructor), so we
        // must read it from full_config() instead.
        if (uses_default_extruder)
        {
            const ConfigOptionInt* extruder_opt = full_cfg.option<ConfigOptionInt>("extruder");
            if (extruder_opt != nullptr && extruder_opt->value >= 1 && extruder_opt->value <= num_filaments)
                used_slots_0_based.insert(extruder_opt->value - 1);
        }
    }
    if (used_slots_0_based.empty())
        return true;

    // Read filament_is_high_temperature directly from each filament preset's
    // own config rather than through full_config(). full_config() builds a
    // merged snapshot that may lag behind when called from Sidebar hooks
    // (the edited preset config hasn't been committed yet).
    //
    // `used_slots_0_based` is a std::set<int> so iteration is ascending and
    // de-duplicated; the 1-based vectors we fill here inherit that ordering.
    bool has_high = false, has_low = false;
    {
        PresetBundle* bundle = m_q->preset_bundle();
        for (int slot : used_slots_0_based)
        {
            if (slot < 0 || slot >= static_cast<int>(bundle->filament_presets.size()))
                continue;
            const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot], true);
            if (preset == nullptr)
                continue;
            const bool is_high = preset->config.opt_bool("filament_is_high_temperature", 0);
            if (is_high)
            {
                has_high = true;
                detail.high_temp_slots_1based.push_back(slot + 1);
            }
            else
            {
                has_low = true;
                detail.low_temp_slots_1based.push_back(slot + 1);
            }
        }
    }

    const bool compatible = !(has_high && has_low);
    if (compatible)
    {
        // No conflict — clear detail so callers can't read stale partial data.
        detail.high_temp_slots_1based.clear();
        detail.low_temp_slots_1based.clear();
    }
    return compatible;
}

bool MixingGateKeeper::check_filament_temp_mixing()
{
    return check_filament_temp_mixing(m_partplate_list.get_curr_plate_index());
}

FilamentTempMixingState MixingGateKeeper::get_filament_temp_mixing_state(int plate_index)
{
    if (check_filament_temp_mixing(plate_index))
        return FilamentTempMixingState::Compatible;

    return m_q->app_config()->get_bool("allow_filament_temp_mixing") ?
        FilamentTempMixingState::AllowedWarning :
        FilamentTempMixingState::BlockedError;
}

FilamentTempMixingState MixingGateKeeper::get_filament_temp_mixing_state()
{
    return get_filament_temp_mixing_state(m_partplate_list.get_curr_plate_index());
}

bool MixingGateKeeper::is_plate_blocked_by_filament_temp_mixing(int plate_index)
{
    return get_filament_temp_mixing_state(plate_index) == FilamentTempMixingState::BlockedError;
}

bool MixingGateKeeper::is_plate_blocked_by_filament_temp_mixing_cached(int plate_index) const
{
    if (plate_index < 0 || plate_index >= static_cast<int>(filament_temp_mixing_blocked_cache.size()))
        return false;
    return filament_temp_mixing_blocked_cache[plate_index];
}

bool MixingGateKeeper::sync_filament_temp_mixing_notification()
{
    // 1. Always close the previously-pushed notifications using their exact
    //    cached text. NotificationManager::close_validate_* matches on text,
    //    so we cannot use a freshly regenerated template string here — it
    //    would not match the previous (possibly different) body.
    //    This runs before the curr_plate null check so that an early return
    //    does not leak a stale notification on the screen.
    NotificationManager* nm = m_hub.nm();
    if (!filament_temp_mixing_last_error_text.empty())
    {
        nm->close_validate_error_notification(filament_temp_mixing_last_error_text);
        filament_temp_mixing_last_error_text.clear();
    }
    if (!filament_temp_mixing_last_warning_text.empty())
    {
        nm->close_validate_warning_notification(filament_temp_mixing_last_warning_text);
        filament_temp_mixing_last_warning_text.clear();
    }

    // 2. Refresh the per-plate blocked cache used by hot rendering paths.
    //    sync_filament_temp_mixing_notification() is invoked at every state-
    //    changing event (plate switch, filament change, bed-type change, etc.),
    //    so refreshing here keeps the cache fresh without recomputing every
    //    frame in GLCanvas3D::_render_imgui_select_plate_toolbar.
    const int plate_count = m_partplate_list.get_plate_count();
    filament_temp_mixing_blocked_cache.assign(plate_count, false);
    for (int i = 0; i < plate_count; ++i)
    {
        filament_temp_mixing_blocked_cache[i] =
            (get_filament_temp_mixing_state(i) == FilamentTempMixingState::BlockedError);
    }

    PartPlate* curr_plate = m_partplate_list.get_curr_plate();
    if (curr_plate == nullptr)
    {
        BOOST_LOG_TRIVIAL(warning) << "[Plater] sync_filament_temp_mixing_notification: curr_plate is null";
        return true;
    }

    const int curr_plate_index = m_partplate_list.get_curr_plate_index();
    const FilamentTempMixingState mixing_state = get_filament_temp_mixing_state(curr_plate_index);
    bool slicing_allowed = true;

    // 3. Compute the per-plate detail (used by both warning and error text).
    FilamentTempMixingDetail detail;
    if (mixing_state != FilamentTempMixingState::Compatible)
        check_filament_temp_mixing(curr_plate_index, detail);

    switch (mixing_state)
    {
    case FilamentTempMixingState::Compatible:
        // Filament temp mixing is compatible — only clear our own notification,
        // do NOT touch m_apply_invalid. Bed type mismatch or other validation
        // errors must not be cleared by the filament temp mixing system.
        slicing_allowed = true;
        break;
    case FilamentTempMixingState::AllowedWarning: {
        // push_notification stores the body verbatim (no auto prefix), while
        // close_validate_warning_notification(text) compares against
        // "WARNING:\n" + text. So the pushed body must include the WARNING
        // prefix, but the cached value must NOT — that way close-time text
        // matches the pushed text exactly.
        const std::string warning_body = filament_temp_mixing_warning_text(detail);
        nm->push_notification(
            NotificationType::ValidateWarning,
            NotificationManager::NotificationLevel::WarningNotificationLevel,
            tr_u8("WARNING:") + "\n" + warning_body);
        filament_temp_mixing_last_warning_text = warning_body;
        slicing_allowed = true;
        break;
    }
    case FilamentTempMixingState::BlockedError: {
        StringObjectException err;
        err.type   = STRING_EXCEPT_FILAMENTS_MIXING_TEMP;
        err.string = filament_temp_mixing_error_text(detail);
        nm->push_validate_error_notification(err);
        filament_temp_mixing_last_error_text = err.string;
        // Blocking is enforced through get_enable_slice_status() / find_next_sliceable_plate_for_slice_all()
        // which independently check is_plate_blocked_by_filament_temp_mixing().
        // Do NOT set m_apply_invalid - that flag belongs to the background validation system.
        slicing_allowed = false;
        break;
    }
    default:
        BOOST_LOG_TRIVIAL(warning) << "[Plater] sync_filament_temp_mixing_notification: unknown mixing_state " << static_cast<int>(mixing_state);
        slicing_allowed = true;
        break;
    }

    filament_temp_mixing_notification_initialized = true;
    filament_temp_mixing_notification_plate = curr_plate_index;
    filament_temp_mixing_notification_state = mixing_state;

    const bool can_slice = curr_plate->can_slice() && slicing_allowed;
    if (m_main_frame)
        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, can_slice);
    return slicing_allowed;
}

bool MixingGateKeeper::check_flow_ratio_zero(int plate_index, FlowRatioZeroDetail& detail)
{
    detail.offender_slots_1based.clear();

    // Boundary checks (mirror check_filament_temp_mixing).
    PartPlate* plate = nullptr;
    const DynamicPrintConfig&  full_cfg             = m_q->preset_bundle()->full_config();
    const ConfigOptionStrings* filament_type_option = full_cfg.option<ConfigOptionStrings>("filament_type");
    {
        if (filament_type_option == nullptr || filament_type_option->values.empty())
            return true;

        const int plate_count = m_partplate_list.get_plate_count();
        if (plate_index < 0 || plate_index >= plate_count)
            return true;

        plate = m_partplate_list.get_plate(plate_index);
        if (plate == nullptr)
            return true;

        bool has_object_on_plate = false;
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
            const ModelObject* model_object = m_model.objects[obj_idx];
            if (model_object_is_on_plate(plate, obj_idx, model_object)) {
                has_object_on_plate = true;
                break;
            }
        }
        if (!has_object_on_plate)
            return true;
    }

    // Collect filament slots actually used on this plate (same shared logic).
    std::set<int> used_slots_0_based;
    {
        const int num_filaments = static_cast<int>(filament_type_option->values.size());
        collect_filament_slots_from_config(*plate->config(), num_filaments, used_slots_0_based);

        bool uses_default_extruder = false;
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
            const ModelObject* model_object = m_model.objects[obj_idx];
            if (!model_object_is_on_plate(plate, obj_idx, model_object))
                continue;
            collect_filament_slots_from_model_config(model_object->config, num_filaments, used_slots_0_based);

            if (!model_object->config.has("extruder") || model_object->config.extruder() == 0)
                uses_default_extruder = true;

            for (const ModelVolume* model_volume : model_object->volumes) {
                collect_filament_slots_from_model_config(model_volume->config, num_filaments, used_slots_0_based);
                for (int extruder_id : model_volume->get_extruders()) {
                    if (extruder_id >= 1 && extruder_id <= num_filaments)
                        used_slots_0_based.insert(extruder_id - 1);
                }
            }
        }

        {
            static const std::vector<const char*> always_collect = {"wipe_tower_filament", "support_filament", "support_interface_filament"};
            for (const char* key : always_collect) {
                const ConfigOptionInt* option = m_config->option<ConfigOptionInt>(key);
                if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                    used_slots_0_based.insert(option->value - 1);
            }

            if (uses_default_extruder) {
                static const std::vector<const char*> default_keys = {"wall_filament", "sparse_infill_filament", "solid_infill_filament"};
                for (const char* key : default_keys) {
                    const ConfigOptionInt* option = m_config->option<ConfigOptionInt>(key);
                    if (option != nullptr && option->value >= 1 && option->value <= num_filaments)
                        used_slots_0_based.insert(option->value - 1);
                }
            }
        }

        if (uses_default_extruder) {
            const ConfigOptionInt* extruder_opt = full_cfg.option<ConfigOptionInt>("extruder");
            if (extruder_opt != nullptr && extruder_opt->value >= 1 && extruder_opt->value <= num_filaments)
                used_slots_0_based.insert(extruder_opt->value - 1);
        }
    }
    if (used_slots_0_based.empty())
        return true;

    // Read filament_flow_ratio from each used preset's own config (same defensive
    // pattern as filament_is_high_temperature in check_filament_temp_mixing).
    // `<=` rather than `== 0.0` to dodge floating-point comparison traps.
    {
        PresetBundle* bundle = m_q->preset_bundle();
        for (int slot : used_slots_0_based) {
            if (slot < 0 || slot >= static_cast<int>(bundle->filament_presets.size()))
                continue;
            const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot], true);
            if (preset == nullptr)
                continue;
            const double ratio = preset->config.opt_float("filament_flow_ratio", 0);
            if (ratio <= 0.0)
                detail.offender_slots_1based.push_back(slot + 1);
        }
    }

    return detail.offender_slots_1based.empty();
}

bool MixingGateKeeper::is_plate_blocked_by_flow_ratio_zero(int plate_index)
{
    FlowRatioZeroDetail unused;
    return !check_flow_ratio_zero(plate_index, unused);
}

bool MixingGateKeeper::sync_flow_ratio_zero_notification()
{
    PartPlate* curr_plate = m_partplate_list.get_curr_plate();
    if (curr_plate == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "[Plater] sync_flow_ratio_zero_notification: curr_plate is null";
        return true;
    }

    const int curr_plate_index = m_partplate_list.get_curr_plate_index();
    FlowRatioZeroDetail detail;
    const bool blocked = !check_flow_ratio_zero(curr_plate_index, detail);
    bool slicing_allowed = true;

    NotificationManager* nm = m_hub.nm();

    // 1. Close any previously-pushed banner using its exact cached text.
    //    close_validate_error_notification() matches by text, so we must use
    //    the cached string (which may differ from a freshly regenerated body).
    if (!flow_ratio_zero_last_error_text.empty()) {
        nm->close_validate_error_notification(flow_ratio_zero_last_error_text);
        flow_ratio_zero_last_error_text.clear();
    }

    // 2. Push a fresh banner only if blocked. Blocking itself is enforced
    //    independently by get_enable_slice_status() / find_next_sliceable_plate_for_slice_all()
    //    via is_plate_blocked_by_flow_ratio_zero(), so we do NOT touch m_apply_invalid
    //    (that flag belongs to the background validation system).
    if (blocked) {
        StringObjectException err;
        err.type   = STRING_EXCEPT_FLOW_RATIO_ZERO;
        err.string = flow_ratio_zero_error_text(detail);
        nm->push_validate_error_notification(err);
        flow_ratio_zero_last_error_text = err.string;
        slicing_allowed = false;
    }

    const bool can_slice = curr_plate->can_slice() && slicing_allowed;
    if (m_main_frame)
        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, can_slice);
    return slicing_allowed;
}

ColdPlateCompatResult MixingGateKeeper::get_cold_plate_compat_state(int plate_index) const
{
    ColdPlateCompatResult result;

    // Boundary check
    const int plate_count = m_partplate_list.get_plate_count();
    if (plate_index < 0 || plate_index >= plate_count) {
        BOOST_LOG_TRIVIAL(warning) << "[Plater] get_cold_plate_compat_state: invalid plate_index=" << plate_index;
        return result;
    }

    // Early exit: only relevant under the Cool Steel Plate bed type.
    // Cool Steel Plate reuses the Supertack bed-type enumerator (its config key
    // is "Supertack Plate" in s_keys_map_BedType), so compare against btSuperTack directly.
    const BedType curr_bed = m_q->preset_bundle()->project_config.opt_enum<BedType>("curr_bed_type");
    if (curr_bed != btSuperTack)
        return result;

    // Need a non-null plate and at least one object on it
    const DynamicPrintConfig& full_cfg = m_q->preset_bundle()->full_config();
    const ConfigOptionStrings* filament_type_option = full_cfg.option<ConfigOptionStrings>("filament_type");
    if (filament_type_option == nullptr || filament_type_option->values.empty())
        return result;

    PartPlate* plate = m_partplate_list.get_plate(plate_index);
    if (plate == nullptr)
        return result;

    bool has_object_on_plate = false;
    for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
        const ModelObject* model_object = m_model.objects[obj_idx];
        if (model_object_is_on_plate(plate, obj_idx, model_object)) {
            has_object_on_plate = true;
            break;
        }
    }
    if (!has_object_on_plate)
        return result;

    // Collect used slots once (shared with filament_temp_mixing for parity).
    std::set<int> used_slots_0_based;
    const int num_filaments = static_cast<int>(filament_type_option->values.size());
    collect_used_filament_slots_on_plate(plate, num_filaments, m_config, full_cfg, used_slots_0_based);
    if (used_slots_0_based.empty())
        return result;

    // Single pass over used slots: classify each preset into unsupported /
    // TPU. State is derived at the end: BlockedError wins over SeriousWarning.
    PresetBundle* bundle = m_q->preset_bundle();
    for (int slot : used_slots_0_based) {
        if (slot < 0 || slot >= static_cast<int>(bundle->filament_presets.size()))
            continue;
        const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot], true);
        if (preset == nullptr)
            continue;

        const ConfigOptionStrings* ftype = preset->config.option<ConfigOptionStrings>("filament_type");
        const std::string type_str = (ftype != nullptr && !ftype->values.empty())
            ? ftype->values[0]
            : std::string("?");
        if (type_str == "TPU") {
            result.uses_tpu = true;
            result.tpu_slots_1_based.push_back(slot + 1);
        }

        const int t_other = preset->config.opt_int("supertack_plate_temp", 0);
        const int t_first = preset->config.opt_int("supertack_plate_temp_initial_layer", 0);
        if (t_first <= 0 || t_other <= 0)
            result.unsupported_slots_1_based.push_back(slot + 1);
    }

    if (!result.unsupported_slots_1_based.empty())
        result.state = ColdPlateCompatState::BlockedError;
    else if (result.uses_tpu)
        result.state = ColdPlateCompatState::SeriousWarning;
    else
        result.state = ColdPlateCompatState::Compatible;
    return result;
}

bool MixingGateKeeper::is_plate_blocked_by_cold_plate(int plate_index) const
{
    return get_cold_plate_compat_state(plate_index).state == ColdPlateCompatState::BlockedError;
}

bool MixingGateKeeper::sync_cold_plate_notification()
{
    PartPlate* curr_plate = m_partplate_list.get_curr_plate();
    if (curr_plate == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "[Plater] sync_cold_plate_notification: curr_plate is null";
        return true;
    }

    const int                   curr_plate_index = m_partplate_list.get_curr_plate_index();
    const ColdPlateCompatResult compat          = get_cold_plate_compat_state(curr_plate_index);
    const ColdPlateCompatState  state           = compat.state;
    bool                        slicing_allowed = true;

    // Always close the previously-pushed notification first. close_validate_error_notification
    // and close_slicing_serious_warning_notification use exact-text matching, so we MUST pass
    // the previously-pushed text (cached in cold_plate_last_*), not a fresh empty/template
    // text. This is the only way to guarantee stale notifications disappear across state changes.
    if (!cold_plate_last_error_text.empty()) {
        m_hub.nm()->close_validate_error_notification(cold_plate_last_error_text);
        cold_plate_last_error_text.clear();
    }
    if (!cold_plate_last_serious_warning_text.empty()) {
        m_hub.nm()->close_slicing_serious_warning_notification(cold_plate_last_serious_warning_text);
        cold_plate_last_serious_warning_text.clear();
    }

    switch (state)
    {
    case ColdPlateCompatState::Compatible:
        slicing_allowed = true;
        break;
    case ColdPlateCompatState::SeriousWarning: {
        // TPU is compatible but warrants a non-blocking serious warning.
        const std::string text = cold_plate_serious_warning_text(compat.tpu_slots_1_based);
        m_hub.nm()->push_slicing_serious_warning_notification(text, std::vector<ModelObject const*>());
        cold_plate_last_serious_warning_text = text;
        slicing_allowed = true;
        break;
    }
    case ColdPlateCompatState::BlockedError: {
        // Merge all unsupported filaments (already collected in compat) into a single error notification.
        StringObjectException err;
        err.type   = STRING_EXCEPT_COLD_PLATE_INCOMPATIBLE;
        err.string = cold_plate_error_text(compat.unsupported_slots_1_based);
        m_hub.nm()->push_validate_error_notification(err);
        cold_plate_last_error_text = err.string;
        slicing_allowed = false;
        break;
    }
    default:
        BOOST_LOG_TRIVIAL(warning) << "[Plater] sync_cold_plate_notification: unknown state "
                                   << static_cast<int>(state);
        slicing_allowed = true;
        break;
    }

    cold_plate_notification_initialized = true;
    cold_plate_notification_plate       = curr_plate_index;
    cold_plate_notification_state       = state;

    // Button enable/disable is gated by MainFrame::get_enable_slice_status(), which checks
    // is_plate_blocked_by_cold_plate() directly. The can_slice arg is ignored for
    // eEventPlateUpdate — pass true to match filament_temp_mixing's contract.
    const bool can_slice = curr_plate->can_slice() && slicing_allowed;
    if (m_main_frame)
        m_main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, can_slice);
    return slicing_allowed;
}

bool MixingGateKeeper::guard_before_slice_plate()
{
    sync_filament_temp_mixing_notification();
    sync_flow_ratio_zero_notification();
    sync_cold_plate_notification();
    return confirm_filament_temp_mixing_before_slice();
}

bool MixingGateKeeper::guard_before_slice_all()
{
    sync_flow_ratio_zero_notification();
    return confirm_filament_temp_mixing_before_slice_all();
}

bool MixingGateKeeper::confirm_filament_temp_mixing_before_slice()
{
    const FilamentTempMixingState state = get_filament_temp_mixing_state();
    switch (state)
    {
    case FilamentTempMixingState::Compatible:
        return true;
    case FilamentTempMixingState::BlockedError:
        sync_filament_temp_mixing_notification();
        sync_cold_plate_notification();
        return false;
    case FilamentTempMixingState::AllowedWarning:
        break;
    default:
        BOOST_LOG_TRIVIAL(warning) << "[Plater] confirm_filament_temp_mixing_before_slice: unknown state "
                                   << static_cast<int>(state);
        return true;
    }

    // Show the actual high/low filament breakdown so the user can see which
    // slots are involved before deciding. MessageDialog takes wxString, so
    // convert the UTF-8 body produced by the text builder.
    FilamentTempMixingDetail detail;
    check_filament_temp_mixing(m_partplate_list.get_curr_plate_index(), detail);
    const std::string body = filament_temp_mixing_warning_text(detail)
                             + "\n"
                             + tr_u8("Do you want to continue?");

    MessageDialog dlg(m_q, wxString::FromUTF8(body.c_str()),
                      _L("Confirm slicing"), wxICON_WARNING | wxOK | wxCANCEL);
    dlg.SetButtonLabel(wxID_OK, _L("Confirm"));
    dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
    return dlg.ShowModal() == wxID_OK;
}

bool MixingGateKeeper::confirm_filament_temp_mixing_before_slice_all()
{
    if (!m_q->has_sliceable_plate_for_slice_all())
        return false;

    // Collect every sliceable, not-yet-sliced plate that is currently in the
    // AllowedWarning state, along with its high/low filament breakdown so the
    // confirmation dialog can show per-plate details.
    std::vector<PlateMixingInfo> mixing_plates;
    for (int plate_index = 0; plate_index < m_partplate_list.get_plate_count(); ++plate_index)
    {
        PartPlate* plate = m_partplate_list.get_plate(plate_index);
        if (plate == nullptr || !plate->can_slice() || plate->is_slice_result_valid())
            continue;
        if (get_filament_temp_mixing_state(plate_index) != FilamentTempMixingState::AllowedWarning)
            continue;

        PlateMixingInfo info;
        info.plate_index_1based = plate_index + 1;
        // state == AllowedWarning guarantees a mixing conflict exists, so the
        // detail will be populated.
        check_filament_temp_mixing(plate_index, info.detail);
        mixing_plates.push_back(info);
    }

    if (mixing_plates.empty())
        return true;

    const std::string body = filament_temp_mixing_warning_text_slice_all(mixing_plates)
                             + "\n"
                             + tr_u8("Do you want to continue?");

    MessageDialog dlg(m_q, wxString::FromUTF8(body.c_str()),
                      _L("Confirm slicing"), wxICON_WARNING | wxOK | wxCANCEL);
    dlg.SetButtonLabel(wxID_OK, _L("Confirm"));
    dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
    return dlg.ShowModal() == wxID_OK;
}

bool MixingGateKeeper::has_incompatible_mixed_filament_in_use() const
{
    // During drag operations the plate/model state is in flux;
    // return the last known good result to avoid button flicker.
    if (m_layout && m_layout->view3D && m_layout->view3D->is_dragging())
        return m_cached_incompatible_mixed;

    PresetBundle *preset_bundle = m_q ? m_q->preset_bundle() : nullptr;
    if (!preset_bundle) return false;

    const size_t num_physical = preset_bundle->filament_presets.size();
    if (num_physical < 2) return false;

    auto &mgr = preset_bundle->mixed_filaments;

    // Early exit: if no incompatible mixed filament exists in the project,
    // we can skip the per-object scan entirely.
    bool any_incompatible = false;
    for (const auto &mf : mgr.mixed_filaments()) {
        if (mf.enabled && !mf.deleted && !is_filament_compatible(mf)) {
            any_incompatible = true;
            break;
        }
    }
    if (!any_incompatible) {
        m_cached_incompatible_mixed = false;
        return false;
    }

    const PartPlate *part_plate = m_partplate_list.get_curr_plate();
    if (!part_plate) return false;

    ModelObjectPtrs plate_objects = part_plate->get_objects_on_this_plate();

    std::set<unsigned int> used_virtual_1based;
    for (auto *obj : plate_objects) {
        if (!obj) continue;
        for (auto *vol : obj->volumes) {
            if (!vol) continue;
            for (int eid_1based : vol->get_extruders()) {
                if (eid_1based > static_cast<int>(num_physical))
                    used_virtual_1based.insert(static_cast<unsigned int>(eid_1based));
            }
        }
    }

    for (unsigned int fid : used_virtual_1based) {
        const MixedFilament *mf = mgr.mixed_filament_from_id(fid, num_physical);
        if (mf && !is_filament_compatible(*mf)) {
            BOOST_LOG_TRIVIAL(info) << "Slicing blocked: incompatible mixed filament (virtual ID "
                                    << fid << ") in use on current plate";
            m_cached_incompatible_mixed = true;
            return true;
        }
    }
    m_cached_incompatible_mixed = false;
    return false;
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
