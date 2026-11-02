// FilamentService — 耗材/AMS/批量删除编排服务（阶段 G1）。
// 承接：Plater 侧 10 函数 + m_batch_physical_deletion + Sidebar AMS 4 函数（自宿主逐字迁入，
// 宿主解引用改经 FilamentServiceHost）。
// 依赖规则（B1/B2/B3 护栏延续）：本 hpp 禁 include Plater.hpp / Sidebar.hpp / GUI_App.hpp；
// 宿主能力经 FilamentServiceHost 回调；通知经 NotificationHub& typed 注入（C 阶段先例）。
#pragma once

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"   // DynamicPrintConfig（build_filament_ams_list 按值返回）

class wxColour;
class wxWindow;

namespace Slic3r {
class AppConfig;
class DeviceManager;
class MachineObject;
class Model;
class PresetBundle;
class PrintHost;

namespace GUI {
class MainFrame;
class PartPlateList;
class Plater;
class PlaterPresetComboBox;
class Sidebar;
class Tab;

namespace app {
class LayoutManager;
class NotificationHub;
class SlicingController;

// 阶段 H1 2.2：FilamentServiceHost 消亡——F2 ctor typed 注入（config/model/partplate_list/main_frame/
// slicing/usage 旗标）+ layout late_bind（sidebar/combos/panel）；F1（queue_event/confirm）m_q 直调；
// F3（preset_bundle/plater_alive/app_config/get_connect_host/device_manager/get_tab）q 桥接。

class FilamentService {
public:
    explicit FilamentService(NotificationHub& hub, Plater* q, DynamicPrintConfig* config,
                             PartPlateList& partplate_list, MainFrame* main_frame,
                             SlicingController& slicing, std::atomic<bool>& filament_usage_sync_pending);
    void late_bind(LayoutManager& layout);   // 阶段 H1 2.2：layout 声明序晚于本服务，plater_init 内补注

    // —— Plater 公有门面一行转发（签名不变）——
    static wxColour get_next_color_for_filament();
    void on_filaments_delete(size_t num_filaments, size_t filament_id, int replace_filament_id,
                             const std::vector<unsigned char>& is_mixed_snapshot);
    void on_filaments_change(size_t num_filaments);
    bool update_filament_colors_in_full_config();
    void notify_filament_usage_changed();
    void force_filament_colors_update();
    void force_print_bed_update();
    void set_global_filament_map(const std::vector<int>& filament_map);
    std::vector<int> get_global_filament_map() const;
    int  batch_physical_deletion() const { return m_batch_physical_deletion; }
    void inc_batch_physical_deletion() { ++m_batch_physical_deletion; }
    void dec_batch_physical_deletion() { --m_batch_physical_deletion; }

    // —— Sidebar AMS 公有门面一行转发（签名不变）——
    std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj);
    void load_ams_list(std::string const& device, MachineObject* obj);
    void sync_ams_list();
    void show_sync_filament_dialog();

private:
    // 批量物理删除守卫计数（非原子 int，仅 UI 线程；原 Plater::priv 成员逐字迁入）
    int m_batch_physical_deletion{0};
    // AMS 设备名（原 Sidebar::priv 成员；仅被 4 个 AMS 函数使用，随功能迁入服务）
    std::string m_ams_list_device;
    NotificationHub& m_hub;
    Plater*                     m_q{nullptr};
    DynamicPrintConfig*         m_config;
    PartPlateList&              m_partplate_list;
    MainFrame*                  m_main_frame;
    SlicingController&          m_slicing;
    std::atomic<bool>&          m_filament_usage_sync_pending;
    LayoutManager*              m_layout{nullptr};   // late_bind
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
