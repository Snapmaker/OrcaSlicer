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


class CalibrationFlow {
public:
    explicit CalibrationFlow(NotificationHub& hub, Plater* q, Model& model, PartPlateList& partplate_list,
                             MainFrame* main_frame, ProjectSession& session, SlicingController& slicing,
                             ObjectEditCommands& obj_cmds);
    void late_bind(LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.1：声明序晚于本服务，plater_init 内补注

    void calib_pa(const Calib_Params& params);
    void calib_flowrate(bool is_linear, int pass);
    void calib_temp(const Calib_Params& params);
    void calib_max_vol_speed(const Calib_Params& params);
    void calib_retraction(const Calib_Params& params);
    void calib_VFA(const Calib_Params& params);
    void calib_input_shaping_freq(const Calib_Params& params);
    void calib_input_shaping_damp(const Calib_Params& params);
    void calib_junction_deviation(const Calib_Params& params);

    void cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes);
    void _calib_pa_pattern_gen_gcode();                 // slicing_host 槽消费
    void record_start_print_preset(std::string action); // export_send_host 槽消费

private:
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
