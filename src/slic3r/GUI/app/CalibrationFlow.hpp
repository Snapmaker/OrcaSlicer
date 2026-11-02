// CalibrationFlow — 校准流编排服务（阶段 G2）。
// 承接：Plater 侧 9 公有校准流 + 5 私有 PA 辅助（含 F1 移交的 cut_horizontal）+
// priv record_start_print_preset（自宿主逐字迁入，宿主解引用改经 CalibrationFlowHost）。
// 依赖规则（B1/B2/B3/G1 护栏延续）：本 hpp 禁 include 任何宿主头（Plater/GUI_App/切片控制域）；
// 宿主能力经 CalibrationFlowHost 回调；通知经 NotificationHub& typed 注入（C 阶段先例）。
// 勘误：路线指引 §4.6 typed 注入草案不可行（宿主服务 ctor 需 Plater*，无头单测不可构造）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <wx/string.h>             // wxString（new_project 槽签名按引用）

#include "libslic3r/calib.hpp"     // Calib_Params（set_calib_params 槽签名）
#include "libslic3r/CutUtils.hpp"  // ModelObjectCutAttributes（cut_horizontal 签名）+ ModelObject*

class wxString;

namespace Slic3r {
class Model;
class NetworkAgent;
class PresetBundle;

namespace GUI {
class GLCanvas3D;
class MainFrame;
class ObjectList;
class PartPlateList;
class Plater;
class Tab;

namespace app {
class LayoutManager;
class NotificationHub;
class ObjectEditCommands;
class SceneRefreshCoordinator;
class SlicingController;
class ProjectSession;

// 阶段 H1 2.1：CalibrationFlowHost 消亡——F2 ctor typed 注入（session/model/partplate_list/main_frame/slicing/obj_cmds）
// + layout/refresh 经 late_bind（声明序 :840/:841 晚于本服务 :839）；F3（preset_bundle/get_tab/get_mode_str/get_agent）改 q 桥接。

class CalibrationFlow {
public:
    explicit CalibrationFlow(NotificationHub& hub, Plater* q, Model& model, PartPlateList& partplate_list,
                             MainFrame* main_frame, ProjectSession& session, SlicingController& slicing,
                             ObjectEditCommands& obj_cmds);
    void late_bind(LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.1：声明序晚于本服务，plater_init 内补注

    // —— 公有校准流（Plater 门面一行转发；签名不变）——
    void calib_pa(const Calib_Params& params);
    void calib_flowrate(bool is_linear, int pass);
    void calib_temp(const Calib_Params& params);
    void calib_max_vol_speed(const Calib_Params& params);
    void calib_retraction(const Calib_Params& params);
    void calib_VFA(const Calib_Params& params);
    void calib_input_shaping_freq(const Calib_Params& params);
    void calib_input_shaping_damp(const Calib_Params& params);
    void calib_junction_deviation(const Calib_Params& params);

    // —— F1 移交 / Host 槽消费（无 Plater 门面）——
    // cut_horizontal：原 Plater 私有方法随迁（F1 归属校正移交）；服务层公有化
    // 以便无头单测直构（Plater 侧零外部调用，无门面诉求）。
    void cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes);
    void _calib_pa_pattern_gen_gcode();                 // slicing_host 槽消费
    void record_start_print_preset(std::string action); // export_send_host 槽消费

private:
    // 原 Plater 私有辅助（唯一调用方在本服务内，不留门面）
    void adjust_settings_for_flowrate_calib(bool linear, int pass);   // 阶段 H1 2.1：原自由函数（宿主依赖故为成员，GateKeeper 先例）
    void _calib_pa_pattern(const Calib_Params& params);
    void _calib_pa_tower(const Calib_Params& params);
    void _calib_pa_select_added_objects();

    NotificationHub&    m_hub;
    Plater*             m_q{nullptr};
    Model&              m_model;
    PartPlateList&      m_partplate_list;
    MainFrame*          m_main_frame;
    ProjectSession&     m_session;
    SlicingController&  m_slicing;
    ObjectEditCommands& m_obj_cmds;
    LayoutManager*              m_layout{nullptr};   // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};  // late_bind
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
