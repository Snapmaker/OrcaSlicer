#include "PlateController.hpp"
#include "../Plater.hpp"        // 阶段 H1 2.1：F1 门面直调（get_camera zoom_to_all_plates）
#include "LayoutManager.hpp"     // 阶段 H1 2.1：late_bind 后 sidebar/bed/view3D/preview 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.1：View3D/Preview 完整类型
#include "SceneRefreshCoordinator.hpp"  // 阶段 H1 2.1：late_bind 后 update 直调
#include "SlicingController.hpp" // 阶段 H1 2.1：late_bind 后 slicing_in_progress/background_process 直读
#include "SnapshotService.hpp"   // 阶段 H1 2.1：ctor 注入 SnapshotService&（take_snapshot）
#include "NotificationHub.hpp"   // 阶段 I：ctor 注入 NotificationHub&（盘切换后重算 by-object 告警）

#include "../PartPlate.hpp"          // PartPlateList / PartPlate / MAX_PLATES_COUNT
#include "../GUI_ObjectList.hpp"     // ObjectList
#include "../3DBed.hpp"               // Bed3D / Axes::DefaultTipRadius
#include "../GUI_Preview.hpp"        // View3D / Preview 完整类型
#include "libslic3r/Geometry.hpp"    // Polygon::new_scale
#include "libslic3r/libslic3r.h"     // SCALING_FACTOR(_INTERNAL[_LARGE_PRINTER]) / LARGE_BED_THRESHOLD（全局，定义于 libslic3r.cpp）
#include "boost/log/trivial.hpp"     // BOOST_LOG_TRIVIAL

namespace Slic3r {
namespace GUI {
namespace app {

PlateController::PlateController(Plater* q, PartPlateList& partplate_list, SnapshotService& snapshots,
                                 NotificationHub& hub, const DynamicPrintConfig* config)
    : m_q(q)
    , m_partplate_list(partplate_list)
    , m_snapshots(snapshots)
    , m_hub(hub)
    , m_config(config)
{
}

void PlateController::late_bind(SlicingController& slicing, LayoutManager& layout, SceneRefreshCoordinator& refresh)
{
    m_slicing = &slicing;
    m_layout  = &layout;
    m_refresh = &refresh;
}

bool PlateController::can_add_plate() const
{
    return m_partplate_list.get_plate_count() < PartPlateList::MAX_PLATES_COUNT;
}

bool PlateController::can_delete_plate() const
{
    return m_partplate_list.get_plate_count() > 1 && (!(m_slicing && m_slicing->is_slicing()) || (m_slicing && m_slicing->background_process().running()));
}

BoundingBoxf PlateController::bed_shape_bb() const
{
    BoundingBox bb = scaled_bed_shape_bb();
    return BoundingBoxf(unscale(bb.min), unscale(bb.max));
}

BoundingBox PlateController::scaled_bed_shape_bb() const
{
    const auto *bed_shape_opt = m_config->opt<ConfigOptionPoints>("printable_area");
    const auto printable_area = Slic3r::Polygon::new_scale(bed_shape_opt->values);
    return printable_area.bounding_box();
}

int PlateController::duplicate_plate(int plate_index)
{
    int index = plate_index, ret;
    if (plate_index == -1)
        index = m_partplate_list.get_curr_plate_index();

    ret = m_partplate_list.duplicate_plate(index);

    //need to call update
    m_refresh->update();
    return ret;
}

int PlateController::delete_plate(int plate_index)
{
    int index = plate_index, ret;

    if (m_slicing->is_slicing() || m_slicing->background_process().running())
        return -1;

    if (plate_index == -1)
        index = m_partplate_list.get_curr_plate_index();

    m_snapshots.take_snapshot("delete partplate");

    m_slicing->background_process().set_fff_print(nullptr);

    ret = m_partplate_list.delete_plate(index);
    // Deleting can reselect another plate (PartPlateList::delete_plate),
    // so re-evaluate the by-object warning against the new current plate.
    if (!ret)
        m_hub.sync_print_seq_warning_notification();

    m_partplate_list.update_slice_context_to_current_plate(m_slicing->background_process());
    m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());;
    m_layout->sidebar->obj_list()->reload_all_plates();

    // BBS update default view
    //get_camera().select_view("topfront");
    //get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;

    //need to call update
    m_refresh->update();
    return ret;
}

void PlateController::add_plate()
{
    if (true) {
        m_snapshots.take_snapshot("add partplate");
        m_partplate_list.create_plate();
        int new_plate = m_partplate_list.get_plate_count() - 1;
        m_partplate_list.select_plate(new_plate);
        m_hub.sync_print_seq_warning_notification();
        m_refresh->update();

        // BBS set default view
        //q->get_camera().select_view("topfront");
        m_q->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
    }
}

void PlateController::on_plate_selected()
{
    BOOST_LOG_TRIVIAL(debug) << "PlateController::on_plate_selected:received plate selected event\n";
    m_layout->sidebar->obj_list()->on_plate_selected(m_partplate_list.get_curr_plate_index());
    m_hub.sync_print_seq_warning_notification();
}

void PlateController::set_bed_shape(const Pointfs& shape, const Pointfs& exclude_areas, const double printable_height,
                                    const std::string& custom_texture, const std::string& custom_model, bool force_as_custom)
{
    //Orca: reduce resolution for large bed printer
    BoundingBoxf bed_size = get_extents(shape);
    if (bed_size.size().maxCoeff() <= LARGE_BED_THRESHOLD)
        SCALING_FACTOR = SCALING_FACTOR_INTERNAL;
    else
        SCALING_FACTOR = SCALING_FACTOR_INTERNAL_LARGE_PRINTER;

    //BBS: add shape position
    Vec2d shape_position = m_partplate_list.get_current_shape_position();
    bool new_shape = m_layout->bed.set_shape(shape, printable_height, custom_model, force_as_custom, shape_position);

    float prev_height_lid, prev_height_rod;
    m_partplate_list.get_height_limits(prev_height_lid, prev_height_rod);
    double height_to_lid = m_config->opt_float("extruder_clearance_height_to_lid");
    double height_to_rod = m_config->opt_float("extruder_clearance_height_to_rod");

    Pointfs prev_exclude_areas = m_partplate_list.get_exclude_area();
    new_shape |= (height_to_lid != prev_height_lid) || (height_to_rod != prev_height_rod) || (prev_exclude_areas != exclude_areas);
    if (!new_shape && m_partplate_list.get_logo_texture_filename() != custom_texture) {
        m_partplate_list.update_logo_texture_filename(custom_texture);
    }
    if (new_shape) {
        if (m_layout->view3D) m_layout->view3D->bed_shape_changed();
        if (m_layout->preview) m_layout->preview->bed_shape_changed();

        //BBS: update part plate's size
        // BBS: to be checked
        Vec3d max = m_layout->bed.extended_bounding_box().max;
        Vec3d min = m_layout->bed.extended_bounding_box().min;
        double z = m_config->opt_float("printable_height");

        //Pointfs& exclude_areas = config->option<ConfigOptionPoints>("bed_exclude_area")->values;
        m_partplate_list.reset_size(max.x() - min.x() - Bed3D::Axes::DefaultTipRadius, max.y() - min.y() - Bed3D::Axes::DefaultTipRadius, z);
        m_partplate_list.set_shapes(shape, exclude_areas, custom_texture, height_to_lid, height_to_rod);

        Vec2d new_shape_position = m_partplate_list.get_current_shape_position();
        if (shape_position != new_shape_position)
            m_layout->bed.set_shape(shape, printable_height, custom_model, force_as_custom, new_shape_position);
    }
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
