// PlateController — Plater 多盘域服务（阶段 D1 纯搬移，手册《Plater阶段D1-多盘PlateController抽取实施方案》）。
// hpp 无 wx：依赖仅 libslic3r 数值/配置类型；GUI 类型全部前置声明。
// 依赖规则（AGENTS.md）：本目录禁止 include Plater/GUI_App 宿主头，上下文只能经 Host 进入。
#pragma once

#include <string>

#include "libslic3r/Point.hpp"        // Pointfs / Vec2d
#include "libslic3r/BoundingBox.hpp"  // BoundingBoxf / BoundingBox
#include "libslic3r/Config.hpp"       // ConfigOptionPoints

namespace Slic3r {
class DynamicPrintConfig;

namespace GUI {
class MainFrame;
class PartPlateList;
class ObjectList;
class Bed3D;
class Plater;
class View3D;
class Preview;

namespace app {
class LayoutManager;
class NotificationHub;
class SnapshotService;
class SceneRefreshCoordinator;
class SlicingController;

// 阶段 H1 2.1：PlateControllerHost 消亡——F2 ctor typed 注入（partplate_list/snapshots/config）
// + slicing/layout/refresh 经 late_bind（声明序 :831/:840/:841 晚于本服务 :826）；
// F1（plater_alive→true 常量 / zoom_to_all_plates→m_q->get_camera()）直调。

class PlateController
{
public:
    PlateController(Plater* q, PartPlateList& partplate_list, SnapshotService& snapshots, NotificationHub& hub, const DynamicPrintConfig* config);
    void late_bind(SlicingController& slicing, LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.1

    // —— 无 wx 核心（单测面）——
    bool can_add_plate() const;                       // 原 priv:7417（上限 MAX_PLATES_COUNT 经 partplate_list）
    bool can_delete_plate() const;                    // 原 priv:7422（count>1 && !slicing_in_progress）
    Slic3r::BoundingBoxf bed_shape_bb() const;        // 原 priv:2203（scaled_bb 为私有 helper，原 priv:2210）
    // H2 收口（用户授权接口扩展 2026-10-23）：盘列表访问器——消费方脱离 Plater::get_partplate_list() 门面
    PartPlateList& partplate_list() { return m_partplate_list; }

    // —— 流程（Host 副作用）——
    int  duplicate_plate(int plate_index);            // 原 Plater:14425
    int  delete_plate(int plate_index);               // 原 Plater:14439（切片中拒绝返回 -1）
    void add_plate();                                 // 原 priv:6157 on_action_add_plate
    void on_plate_selected();                         // 原 priv:6483
    void set_bed_shape(const Pointfs& shape, const Pointfs& exclude_areas, const double printable_height,
                       const std::string& custom_texture, const std::string& custom_model, bool force_as_custom); // 原 priv:7364

    // —— 右键选盘 UI 状态（原 priv:851 m_is_RightClickInLeftUI）——
    int  right_click_plate_index() const { return m_right_click_in_left_ui; }
    void set_right_click_plate_index(int index) { m_right_click_in_left_ui = index; }

private:
    Slic3r::BoundingBox scaled_bed_shape_bb() const;  // 原 priv:2210

    Plater*                     m_q{nullptr};
    PartPlateList&              m_partplate_list;
    SnapshotService&            m_snapshots;
    NotificationHub&            m_hub;                // 阶段 I：盘切换后重算 by-object 告警
    const DynamicPrintConfig*   m_config;
    SlicingController*          m_slicing{nullptr};   // late_bind
    LayoutManager*              m_layout{nullptr};    // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};   // late_bind
    int                 m_right_click_in_left_ui{-1};
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
