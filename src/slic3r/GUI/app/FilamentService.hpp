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


class FilamentService {
public:
    explicit FilamentService(NotificationHub& hub, Plater* q, DynamicPrintConfig* config,
                             PartPlateList& partplate_list, MainFrame* main_frame,
                             SlicingController& slicing, std::atomic<bool>& filament_usage_sync_pending);
    void late_bind(LayoutManager& layout);   // 阶段 H1 2.2：layout 声明序晚于本服务，plater_init 内补注

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

    std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj);
    void load_ams_list(std::string const& device, MachineObject* obj);
    void sync_ams_list();

private:
    int m_batch_physical_deletion{0};
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
