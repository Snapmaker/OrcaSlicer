// Sidebar: Prepare 工作区右侧参数面板（打印机/耗材/混色面板宿主）。
// 阶段 A 自 Plater.{hpp,cpp} 原样抽出（commit ①纯搬移）；归属规划见
// openspec/changes/phase-a-sidebar-extraction 与 Plater 解耦主文档。
#ifndef SLIC3R_GUI_SIDEBAR_HPP_
#define SLIC3R_GUI_SIDEBAR_HPP_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/colour.h>   // wxColour（按值参数）
#include <wx/panel.h>    // 基类 wxPanel
#include <wx/sizer.h>    // wxBoxSizer* / wxSizer*
#include <wx/string.h>   // wxString

#include "libslic3r/Config.hpp"       // ConfigOptionMode（按值成员 m_mode）
#include "libslic3r/Preset.hpp"       // Slic3r::Preset::Type（按值参数）
#include "libslic3r/PrintConfig.hpp"  // BedType（按值参数）、DynamicPrintConfig（按值返回）
// 注意：此处不得 include Search.hpp——其链路经 GUI_ObjectList.hpp 回到 Plater.hpp，会形成
// include 环（pragma once 跳过导致 Sidebar 类在 Plater.hpp 解析时未定义）。Search::OptionsSearcher
// 仅以引用出现，用下方前置声明；完整类型由 Sidebar.cpp 自行 include Search.hpp。

class wxButton;

class ComboBox;       // Widgets/ComboBox.hpp（全局作用域）
class ScalableButton; // wxExtensions.hpp（全局作用域）

namespace Slic3r {

class MachineObject;  // DeviceManager.hpp（Slic3r::MachineObject，注意不在 GUI 子命名空间）

namespace Search {
class OptionsSearcher;  // Search.hpp（前置声明，get_searcher 返回引用）
}

namespace GUI {

class Plater;
class ObjectList;
class ObjectSettings;
class ObjectLayers;
class ConfigOptionsGroup;
class PlaterPresetComboBox;
class ObjectDataViewModelNode;
struct BatchMatchResult;

// 原 Plater.cpp:902-906（定义上移：set_btn_label 按值传参，Plater 侧调用点需要完整类型）
enum class ActionButtonType : int {
    abReslice,
    abExport,
    abSendGCode
};

// 原 Plater.hpp:115-122
class SidebarProps
{
public:
    static int TitlebarMargin();
    static int ContentMargin();
    static int IconSpacing();
    static int ElementSpacing();
};

// 原 Plater.hpp:124-241（原样搬入，禁止改动）
class Sidebar : public wxPanel
{
    ConfigOptionMode    m_mode;
public:
    enum DockingState
    {
        None, Left, Right
    };

    Sidebar(Plater *parent);
    Sidebar(Sidebar &&) = delete;
    Sidebar(const Sidebar &) = delete;
    Sidebar &operator=(Sidebar &&) = delete;
    Sidebar &operator=(const Sidebar &) = delete;
    ~Sidebar();

    void create_printer_preset();
    void init_filament_combo(PlaterPresetComboBox **combo, const int filament_idx);
    void remove_unused_filament_combos(const size_t current_extruder_count);
    void update_all_preset_comboboxes(bool reload_printer_view = true);
    //void update_partplate(PartPlateList& list);
    void update_presets(Slic3r::Preset::Type preset_type);
    //BBS
    void update_presets_from_to(Slic3r::Preset::Type preset_type, std::string from, std::string to);

    void change_top_border_for_mode_sizer(bool increase_border);
    void msw_rescale();
    void sys_color_changed();
    void search();
    void jump_to_option(size_t selected);
    void jump_to_option(const std::string& opt_key, Preset::Type type, const std::wstring& category);
    // BBS. Add on_filaments_change() method.
    void on_filaments_change(size_t num_filaments);
    void change_filament(size_t from_id, size_t to_id);
    void merge_mixed_filament(size_t from_id, size_t to_id,
                              bool skip_update = false, bool skip_serialize = false);
    void add_filament();
    void delete_filament(size_t filament_id  = size_t(-1), int replace_filament_id = -1,
                         bool skip_dependency_check = false,
                         bool skip_update = false); // 0 base, -1 means default
    // on_progress (optional) is invoked once per physical-filament deletion inside the
    // batch loop, passing (current, total) where total = redundant physical count.
    // Callers that show a progress dialog map (current/total) to a percentage and
    // call progress.Update so the bar reflects real progress during the (potentially
    // many-second) deletion loop. Default-empty: existing callers see no change.
    void cleanup_unused_filaments_after_batch_match(const BatchMatchResult& match_result,
                                                    std::function<void(int /*current*/, int /*total*/)> on_progress = nullptr);
    void add_custom_filament(wxColour new_col);
    void edit_filament();

    void on_filaments_delete(size_t filament_id);
    void init_color_mix_panel(wxWindow* parent, wxSizer* sizer);
    void update_color_mix_panel();
    void update_mixed_filament_panel(bool sync_manager = true);
    std::vector<unsigned int> get_ui_ordered_filament_ids() const;
    // BBS
    void on_bed_type_change(BedType bed_type);
    void load_ams_list(std::string const & device, MachineObject* obj);
    std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj);
    void sync_ams_list();
    void show_sync_filament_dialog();
    // 2.8 ②：main 版同步对话框续段 + 标题喷嘴同步回 streams（bac6fd5 hpp:236 签名逐字）
    void continue_sync_filament_dialog(bool got_machine_info, const std::string& machine_type);
    void apply_nozzle_sync_result(bool got_machine_info, std::string machine_type,
                                  std::vector<std::string> nozzle_diameters,
                                  std::vector<std::string> nozzle_volume_types,
                                  std::string device_name);
    // Orca
    void show_SEMM_buttons(bool bshow);
    void update_dynamic_filament_list();

    void update_nozzle_settings(bool switch_machine = false);

    ObjectList*             obj_list();
    ObjectSettings*         obj_settings();
    ObjectLayers*           obj_layers();
    wxPanel*                scrolled_panel();
    wxPanel* print_panel();
    wxPanel* filament_panel();

    ConfigOptionsGroup*     og_freq_chng_params(const bool is_fff);
    wxButton*               get_wiping_dialog_button();

    // BBS
    void                    enable_buttons(bool enable);
    void                    set_btn_label(const ActionButtonType btn_type, const wxString& label) const;
    bool                    show_reslice(bool show) const;
	bool                    show_export(bool show) const;
	bool                    show_send(bool show) const;
    bool                    show_eject(bool show)const;
	bool                    show_export_removable(bool show) const;
	bool                    get_eject_shown() const;
    bool                    is_multifilament();
    void                    update_mode();
    bool                    is_collapsed();
    void                    collapse(bool collapse);
    void                    update_searcher();
    void                    update_ui_from_settings();
	bool                    show_object_list(bool show) const;
    void                    finish_param_edit();
    void                    auto_calc_flushing_volumes(const int modify_id);
    void                    jump_to_object(ObjectDataViewModelNode* item);
    void                    can_search();
#ifdef _MSW_DARK_MODE
    void                    show_mode_sizer(bool show);
#endif

    std::vector<PlaterPresetComboBox*>&   combos_filament();
    Search::OptionsSearcher&        get_searcher();
    std::string&                    get_search_line();
    void                            update_printer_thumbnail();
    const std::vector<std::string>& get_bed_type_combo_enum_values() const { return m_bed_type_combo_enum_values; }

private:
    struct priv;
    std::unique_ptr<priv> p;

    wxBoxSizer* m_scrolled_sizer = nullptr;
    ComboBox* m_bed_type_list = nullptr;
    std::vector<std::string> m_bed_type_combo_enum_values;
    ScalableButton* connection_btn = nullptr;
    ScalableButton* machine_connecting_btn = nullptr;
    ScalableButton* ams_btn = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_GUI_SIDEBAR_HPP_
