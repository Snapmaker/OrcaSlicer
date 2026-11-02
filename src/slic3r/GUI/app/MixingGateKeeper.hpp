// MixingGateKeeper — 混色入口守门服务（阶段 B3）。
// 承接：去抖状态 12 成员 + 拖拽残值缓存 + 门控判定/通知函数族（自 Plater 逐字迁入）。
// 依赖规则（B1/B2 护栏延续）：本 hpp 禁 include Plater.hpp / Sidebar.hpp / GUI_App.hpp；
// 宿主能力经 GateKeeperHost 回调；通知经 NotificationHub& typed 注入（C 阶段先例）。
#pragma once

#include <atomic>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "MixingTypes.hpp"

class wxWindow;

namespace Slic3r {
class AppConfig;
class DynamicPrintConfig;
class Model;
class PresetBundle;

namespace GUI {
class MainFrame;
class PartPlate;
class PartPlateList;
class Plater;

namespace app {
class LayoutManager;
class NotificationHub;

// 阶段 H1 2.1：GateKeeperHost 消亡——F2 ctor typed 注入（partplate_list/config/model/main_frame）+ layout 经 late_bind（声明序 :840 晚于本服务 :837，不可 ctor 注入）；F1（dialog_parent/has_sliceable）改 m_q 门面直调；F3（preset_bundle/app_config）改 q 桥接。

class MixingGateKeeper {
public:
    explicit MixingGateKeeper(NotificationHub& hub, Plater* q, PartPlateList& partplate_list,
                              const DynamicPrintConfig* config, Model& model, MainFrame* main_frame);
    void late_bind(LayoutManager& layout);   // 阶段 H1 2.1：layout 声明序晚于本服务，plater_init 内补注

    // —— 判定 / 通知（Plater 公有门面一行转发；签名不变）——
    bool check_filament_temp_mixing(int plate_index);
    bool check_filament_temp_mixing(int plate_index, FilamentTempMixingDetail& detail);
    bool check_filament_temp_mixing();
    FilamentTempMixingState get_filament_temp_mixing_state(int plate_index);
    FilamentTempMixingState get_filament_temp_mixing_state();
    bool is_plate_blocked_by_filament_temp_mixing(int plate_index);
    bool is_plate_blocked_by_filament_temp_mixing_cached(int plate_index) const;
    bool sync_filament_temp_mixing_notification();

    bool check_flow_ratio_zero(int plate_index, FlowRatioZeroDetail& detail);
    bool is_plate_blocked_by_flow_ratio_zero(int plate_index);
    bool sync_flow_ratio_zero_notification();

    ColdPlateCompatResult get_cold_plate_compat_state(int plate_index) const;
    bool is_plate_blocked_by_cold_plate(int plate_index) const;
    bool sync_cold_plate_notification();

    bool guard_before_slice_plate();
    bool guard_before_slice_all();
    bool confirm_filament_temp_mixing_before_slice();
    bool confirm_filament_temp_mixing_before_slice_all();

    bool has_incompatible_mixed_filament_in_use() const;

private:
    // —— 迁移自 Plater.cpp 的文本/槽位 helper（原 static；宿主依赖故为成员）——
    std::string filament_display_label(int slot_1based);
    std::string format_filament_slot_list(const std::vector<int>& slots_1based);
    void        append_filament_temp_mixing_groups(std::string& out, const FilamentTempMixingDetail& detail);
    std::string filament_temp_mixing_warning_text(const FilamentTempMixingDetail& detail);
    std::string filament_temp_mixing_error_text(const FilamentTempMixingDetail& detail);
    std::string flow_ratio_zero_error_text(const FlowRatioZeroDetail& detail);
    std::string filament_temp_mixing_warning_text_slice_all(const std::vector<PlateMixingInfo>& plates);
    std::string filament_temp_mixing_error_text_slice_all(const std::vector<PlateMixingInfo>& plates);
    std::string cold_plate_error_text(const std::vector<int>& unsupported_slots_1_based);
    std::string cold_plate_serious_warning_text(const std::vector<int>& tpu_slots_1_based);
    void        collect_used_filament_slots_on_plate(PartPlate* plate, int num_filaments,
                                                     const DynamicPrintConfig* plater_working_config,
                                                     const DynamicPrintConfig& full_cfg,
                                                     std::set<int>& used_slots_0_based) const;

    NotificationHub&            m_hub;
    Plater*                     m_q{nullptr};
    PartPlateList&              m_partplate_list;
    const DynamicPrintConfig*   m_config;
    Model&                      m_model;
    MainFrame*                  m_main_frame;
    LayoutManager*              m_layout{nullptr};   // late_bind

    // —— 自 Plater::priv 迁入（原序；去抖 12 成员）——
    bool filament_temp_mixing_notification_initialized = false;
    int filament_temp_mixing_notification_plate = -1;
    FilamentTempMixingState filament_temp_mixing_notification_state = FilamentTempMixingState::Compatible;
    // NotificationManager::close_validate_* uses exact-text match, so we must
    // remember the exact strings we last pushed and use those (not freshly
    // regenerated ones) to close the previous notification. Empty = nothing pushed.
    std::string filament_temp_mixing_last_error_text;
    std::string filament_temp_mixing_last_warning_text;

    // Per-plate cached "blocked by filament temp mixing" flags, refreshed in
    // sync_filament_temp_mixing_notification() (which already runs at every
    // state-changing event). Read by hot rendering paths to avoid recomputing
    // check_filament_temp_mixing() for every plate every frame. Indexed by
    // plate index; default-empty / default-false means "not blocked".
    std::vector<bool> filament_temp_mixing_blocked_cache;

    // Same pattern as above, for the flow_ratio_zero banner.
    std::string flow_ratio_zero_last_error_text;

    bool                 cold_plate_notification_initialized = false;
    int                  cold_plate_notification_plate       = -1;
    ColdPlateCompatState cold_plate_notification_state       = ColdPlateCompatState::Compatible;
    // Cached text from the last push — used so close_validate_error_notification /
    // close_slicing_serious_warning_notification can match exactly (compare_text is exact-match).
    std::string          cold_plate_last_error_text;
    std::string          cold_plate_last_serious_warning_text;

    // 原 Plater::priv:1211（第 13 状态）：拖拽期残值缓存（has_incompatible_mixed_filament_in_use）
    mutable std::atomic<bool> m_cached_incompatible_mixed{false};
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
