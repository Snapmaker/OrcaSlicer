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


class PlateController
{
public:
    PlateController(Plater* q, PartPlateList& partplate_list, SnapshotService& snapshots, NotificationHub& hub, const DynamicPrintConfig* config);
    void late_bind(SlicingController& slicing, LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.1

    bool can_add_plate() const;                       // 原 priv:7417（上限 MAX_PLATES_COUNT 经 partplate_list）
    bool can_delete_plate() const;                    // 原 priv:7422（count>1 && !slicing_in_progress）
    Slic3r::BoundingBoxf bed_shape_bb() const;        // 原 priv:2203（scaled_bb 为私有 helper，原 priv:2210）
    PartPlateList& partplate_list() { return m_partplate_list; }

    int  duplicate_plate(int plate_index);            // 原 Plater:14425
    int  delete_plate(int plate_index);               // 原 Plater:14439（切片中拒绝返回 -1）
    void add_plate();                                 // 原 priv:6157 on_action_add_plate
    void on_plate_selected();                         // 原 priv:6483
    void set_bed_shape(const Pointfs& shape, const Pointfs& exclude_areas, const double printable_height,
                       const std::string& custom_texture, const std::string& custom_model, bool force_as_custom); // 原 priv:7364

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
