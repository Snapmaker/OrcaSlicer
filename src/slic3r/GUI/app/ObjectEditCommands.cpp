// ObjectEditCommands 实现（阶段 F1，手册 §2.2/§3）：删除/变换/剪贴板克隆/网格变更/动作分发 + 谓词组逐字搬移自
// Plater::priv（2026-10-19 基线）与 Plater 门面体。逐函数体保持原顺序与注释；仅做宿主触点替换：
//   model.→m_model->、get_selection()→m_selq、sidebar->obj_list()/wxGetApp().obj_list()→m_layout->sidebar->obj_list()、
//   view3D→m_layout->view3D、get_current_canvas3D()→m_layout->get_current_canvas3D()、update()/object_list_changed()/...→Host 槽、
//   slicing()→m_slicing.、partplate_list→m_partplate_list、嵌套版 TakeSnapshot→TakeSnapshotScope。
// 依赖规则：禁直接 include Plater/GUI_App 宿主头（NotificationManager/GUI/MsgDialog 的传递包含与 E1/E2 cpp 同款先例）。
#include "ObjectEditCommands.hpp"
#include "../Plater.hpp"
#include "../GUI_ObjectList.hpp"   // H2 3.1 顶部瘦身：ObjectList 完整型（原经 Plater.hpp→Search.hpp 传递链）        // 阶段 H1 2.2：F1/F3 门面直调（IsShown/notify_filament_usage_changed/preset_bundle/CloneDialog）
#include "../CloneDialog.hpp"   // 阶段 H1 2.2：show_clone_dialog 薄壳（原文内联）
#include "LayoutManager.hpp"     // 阶段 H1 2.2：late_bind 后 sidebar/view3D/bed 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.2：View3D 完整类型
#include "SceneRefreshCoordinator.hpp"  // 阶段 H1 2.2：late_bind 后 update/object_list_changed 直调
#include "../Jobs/Worker.hpp"    // 阶段 H1 2.2：Worker&（cancel_all/is_idle）

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX                 // 防 min/max 宏泄漏（E2 同款 Windows/asio 前置块）
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN      // 防 winsock.h 拖入与 asio 的 winsock2 冲突
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP（WIN32_LEAN_AND_MEAN 下 asio socket_ops.ipp 需要；实测缺失）
#endif
// 注意：必须早于 SlicingController.hpp（经 BackgroundSlicingProcess.hpp→Utils/PrintHost.hpp 引入 asio，实测）

#include "SelectionQuery.hpp"
#include "ProjectSession.hpp"
#include "SlicingController.hpp"
#include "NotificationHub.hpp"
#include "SnapshotService.hpp"

#include <algorithm>
#include <set>

#include <wx/busyinfo.h>                       // wxBusyCursor

#include "../GLCanvas3D.hpp"                   // GLCanvas3D::delete_selected/set_selected_visible/reset_all_gizmos/get_selection/get_gizmos_manager/reset_sequential_print_clearance
#include "../GUI_Preview.hpp"                  // View3D（select_all/deselect_all/center_selected/mirror_selection/...）
#include "../Selection.hpp"                    // Selection/Selection::Clipboard/GLVolume 选择集
#include "../3DScene.hpp"                      // GLVolume::volume_idx
#include "../PartPlate.hpp"                    // PartPlateList::notify_instance_removed/clear
#include "../GUI.hpp"                          // warning_catcher
#include "../MsgDialog.hpp"                    // InfoDialog
#include "../NotificationManager.hpp"          // NotificationManager / NotificationType（clear_before_change_mesh / layersediting）
#include "../I18N.hpp"                         // _L/_u8L
#include "libslic3r/Model.hpp"                 // Model/ModelObject/ModelVolume/ConversionType
#include "libslic3r/PresetBundle.hpp"          // PresetBundle::printers（can_copy/can_paste SLA 判定）
#include "libslic3r/SLA/ReprojectPointsOnMesh.hpp" // sla::reproject_points_and_holes（changed_mesh）

namespace Slic3r {
namespace GUI {
namespace app {

ObjectEditCommands::ObjectEditCommands(Plater* q, Model* model, DynamicPrintConfig* config,
                                       SelectionQuery& selq, ProjectSession& session,
                                       SlicingController& slicing, NotificationHub& notifications,
                                       SnapshotService& snapshots, PartPlateList& partplate_list, Worker& worker)
    : m_q(q), m_model(model), m_config(config)
    , m_selq(selq), m_session(session), m_slicing(slicing)
    , m_notifications(notifications), m_snapshots(snapshots)
    , m_partplate_list(partplate_list)
    , m_worker(worker)
{
}

void ObjectEditCommands::late_bind(LayoutManager& layout, SceneRefreshCoordinator& refresh)
{
    m_layout  = &layout;
    m_refresh = &refresh;
}

// 原嵌套版 TakeSnapshot 语义逐字：take → suppress（构造），allow（析构）
ObjectEditCommands::TakeSnapshotScope::TakeSnapshotScope(SnapshotService& snapshots, const std::string& name)
    : m_snapshots(snapshots)
{
    m_snapshots.take_snapshot(name);
    m_snapshots.suppress_snapshots();
}

ObjectEditCommands::TakeSnapshotScope::~TakeSnapshotScope()
{
    m_snapshots.allow_snapshots();
}

// ============================ 删除组 ============================

void ObjectEditCommands::remove(size_t obj_idx)
{
    if (m_layout->view3D->is_layers_editing_enabled())
        m_layout->view3D->enable_layers_editing(false);

    m_worker.cancel_all();
    m_model->delete_object(obj_idx);
    //BBS: notify partplate the instance removed
    m_partplate_list.notify_instance_removed(obj_idx, -1);
    m_refresh->update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    m_layout->sidebar->obj_list()->delete_object_from_list(obj_idx);
    m_refresh->object_list_changed();
    m_q->notify_filament_usage_changed();
}

bool ObjectEditCommands::delete_object_from_model(size_t obj_idx, bool refresh_immediately)
{
    // check if object isn't cut
    // show warning message that "cut consistancy" will not be supported any more
    ModelObject *obj = m_model->objects[obj_idx];
    if (obj->is_cut()) {
        InfoDialog dialog(m_q, _L("Delete object which is a part of cut object"),
                          _L("You try to delete an object which is a part of a cut object.\n"
                             "This action will break a cut correspondence.\n"
                             "After that model consistency can't be guaranteed."),
                          false, wxYES | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dialog.SetButtonLabel(wxID_YES, _L("Delete"));
        if (dialog.ShowModal() == wxID_CANCEL)
            return false;
    }

    std::string snapshot_label = "Delete Object";
    if (!obj->name.empty())
        snapshot_label += ": " + obj->name;
    TakeSnapshotScope snapshot(m_snapshots, snapshot_label);
    m_worker.cancel_all();

    if (obj->is_cut())
        m_layout->sidebar->obj_list()->invalidate_cut_info_for_object(obj_idx);

    m_model->delete_object(obj_idx);
    //BBS: notify partplate the instance removed
    m_partplate_list.notify_instance_removed(obj_idx, -1);

    //BBS
    if (refresh_immediately) {
        m_refresh->update();
        m_refresh->object_list_changed();
    }

    m_q->notify_filament_usage_changed();

    return true;
}

void ObjectEditCommands::delete_all_objects_from_model()
{
    TakeSnapshotScope snapshot(m_snapshots, "Delete All Objects");

    if (m_layout->view3D->is_layers_editing_enabled())
        m_layout->view3D->enable_layers_editing(false);

    m_refresh->reset_gcode_toolpaths();
    m_slicing.gcode_result().reset();

    m_layout->view3D->get_canvas3d()->reset_sequential_print_clearance();

    m_worker.cancel_all();

    // Stop and reset the Print content.
    m_slicing.background_process().reset();

    //BBS: update partplate
    m_partplate_list.clear();

    m_model->clear_objects();
    m_refresh->update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    m_layout->sidebar->obj_list()->delete_all_objects_from_list();
    m_refresh->object_list_changed();

    //BBS
    m_model->calib_pa_pattern.reset();
    m_model->plates_custom_gcodes.clear();
    m_q->notify_filament_usage_changed();
}

void ObjectEditCommands::remove_selected()
{
    /*if (m_selq.get_selection().is_empty())
        return;*/
    if (m_selq.get_curr_selection().is_empty())
        return;

    // BBS: check before deleting object
    if (!can_delete())
        return;

    TakeSnapshotScope snapshot(m_snapshots, "Delete Selected Objects");
    m_worker.cancel_all();

    //BBS delete current selected
    // p->view3D->delete_selected();
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (canvas) {
        canvas->delete_selected();
        m_q->notify_filament_usage_changed();
    }
}

// ============================ 变换组 ============================

void ObjectEditCommands::center_selection()
{
    m_layout->view3D->center_selected();
}

void ObjectEditCommands::drop_selection()
{
    m_layout->view3D->drop_selected();
}

void ObjectEditCommands::mirror(Axis axis)
{
    m_layout->view3D->mirror_selection(axis);
}

void ObjectEditCommands::split_object()
{
    int obj_idx = m_selq.get_selected_object_idx();
    if (obj_idx == -1)
        return;

    // we clone model object because split_object() adds the split volumes
    // into the same model object, thus causing duplicates when we call load_model_objects()
    Model new_model = *m_model;
    ModelObject* current_model_object = new_model.objects[obj_idx];

    wxBusyCursor wait;
    ModelObjectPtrs new_objects;
    current_model_object->split(&new_objects);
    if (new_objects.size() == 1)
        // #ysFIXME use notification
        Slic3r::GUI::warning_catcher(m_q, _L("The selected object couldn't be split."));
    else
    {
        // BBS no solid parts removed
        // If we splited object which is contain some parts/modifiers then all non-solid parts (modifiers) were deleted
        //if (current_model_object->volumes.size() > 1 && current_model_object->volumes.size() != new_objects.size())
        //    notification_manager()->push_notification(NotificationType::CustomNotification,
        //        NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
        //        _u8L("All non-solid parts (modifiers) were deleted"));

        TakeSnapshotScope snapshot(m_snapshots, "Split to Objects");

        remove(obj_idx);

        // load all model objects at once, otherwise the plate would be rearranged after each one
        // causing original positions not to be kept
        //BBS: set split_object to true to avoid re-compute assemble matrix
        std::vector<size_t> idxs = m_session.load_model_objects(new_objects, false, true);

        // select newly added objects
        for (size_t idx : idxs)
        {
            m_selq.selection().add_object((unsigned int)idx, false);
        }
    }
}

void ObjectEditCommands::split_volume()
{
    m_layout->sidebar->obj_list()->split();
}

void ObjectEditCommands::scale_selection_to_fit_print_volume()
{
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
    m_layout->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(m_layout->bed.build_volume());
#else
    m_layout->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(*m_config);
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT
}

void ObjectEditCommands::convert_unit(ConversionType conv_type)
{
    std::vector<int> obj_idxs, volume_idxs;
    m_layout->sidebar->obj_list()->get_selection_indexes(obj_idxs, volume_idxs);
    if (obj_idxs.empty() && volume_idxs.empty())
        return;

    TakeSnapshotScope snapshot(m_snapshots, conv_type == ConversionType::CONV_FROM_INCH  ? "Convert from imperial units" :
                                conv_type == ConversionType::CONV_TO_INCH    ? "Revert conversion from imperial units" :
                                conv_type == ConversionType::CONV_FROM_METER ? "Convert from meters" : "Revert conversion from meters");
    wxBusyCursor wait;

    ModelObjectPtrs objects;
    std::reverse(obj_idxs.begin(), obj_idxs.end());
    for (int obj_idx : obj_idxs) {
        ModelObject *object = m_model->objects[obj_idx];
        object->convert_units(objects, conv_type, volume_idxs);
        remove(obj_idx);
    }
    std::reverse(objects.begin(), objects.end());
    m_session.load_model_objects(objects);

    Selection& selection = m_layout->view3D->get_canvas3d()->get_selection();
    size_t last_obj_idx = m_model->objects.size() - 1;

    if (volume_idxs.empty()) {
        for (size_t i = 0; i < objects.size(); ++i)
            selection.add_object((unsigned int)(last_obj_idx - i), i == 0);
    }
    else {
        for (int vol_idx : volume_idxs)
            selection.add_volume(last_obj_idx, vol_idx, 0, false);
    }
}

void ObjectEditCommands::set_selected_visible(bool visible)
{
    if (m_selq.get_curr_selection().is_empty())
        return;

    TakeSnapshotScope snapshot(m_snapshots, "Set Selected Objects Visible in AssembleView");
    m_worker.cancel_all();

    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (canvas)
        canvas->set_selected_visible(visible);
}

void ObjectEditCommands::exit_gizmo()
{
    m_layout->view3D->exit_gizmo();
}

// ============================ 剪贴板/克隆组 ============================

void ObjectEditCommands::cut_selection_to_clipboard()
{
    TakeSnapshotScope snapshot(m_snapshots, "Cut Selected Objects");
    if (can_cut_to_clipboard() && !m_layout->sidebar->obj_list()->cut_to_clipboard()) {
        m_layout->view3D->get_canvas3d()->get_selection().cut_to_clipboard();
    }
}

void ObjectEditCommands::copy_selection_to_clipboard()
{
    // At first try to copy selected values to the ObjectList's clipboard
    // to check if Settings or Layers are selected in the list
    // and then copy to 3DCanvas's clipboard if not
    if (can_copy_to_clipboard() && !m_layout->sidebar->obj_list()->copy_to_clipboard())
        m_layout->view3D->get_canvas3d()->get_selection().copy_to_clipboard();
}

void ObjectEditCommands::paste_from_clipboard()
{
    if (!can_paste_from_clipboard())
        return;

    TakeSnapshotScope snapshot(m_snapshots, "Paste From Clipboard");

    // At first try to paste values from the ObjectList's clipboard
    // to check if Settings or Layers were copied
    // and then paste from the 3DCanvas's clipboard if not
    if (!m_layout->sidebar->obj_list()->paste_from_clipboard())
        m_layout->view3D->get_canvas3d()->get_selection().paste_from_clipboard();
}

void ObjectEditCommands::clone_selection()
{
    if (m_selq.is_selection_empty())
        return;
    { CloneDialog dlg(m_q); dlg.ShowModal(); }   // CloneDialog 薄壳（原 CloneDialog dlg(this); dlg.ShowModal();）
}

void ObjectEditCommands::apply_cut_object_to_model(size_t obj_idx, const std::vector<ModelObject*>& new_objects)
{
    m_model->delete_object(obj_idx);
    m_layout->sidebar->obj_list()->delete_object_from_list(obj_idx);

    // suppress to call selection update for Object List to avoid call of early Gizmos on/off update
    m_session.load_model_objects(new_objects, false, false);

    // now process all updates of the 3d scene
    m_refresh->update();
    // Update InfoItems in ObjectList after update() to use of a correct value of the GLCanvas3D::is_sinking(),
    // which is updated after a view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH) call
    for (size_t idx = 0; idx < m_model->objects.size(); idx++)
        m_layout->sidebar->obj_list()->update_info_items(idx);

    Selection& selection = m_selq.selection();
    size_t last_id = m_model->objects.size() - 1;
    for (size_t i = 0; i < new_objects.size(); ++i)
        selection.add_object((unsigned int)(last_id - i), i == 0);

    // UIThreadWorker w;
    // arrange(w, true);
    // w.wait_for_idle();
}

// ============================ 网格/gizmo 组 ============================

void ObjectEditCommands::clear_before_change_mesh(int obj_idx)
{
    ModelObject* mo = m_model->objects[obj_idx];

    // If there are custom supports/seams/mmu/fuzzy skin segmentation, remove them. Fixed mesh
    // may be different and they would make no sense.
    bool paint_removed = false;
    for (ModelVolume* mv : mo->volumes) {
        paint_removed |= ! mv->supported_facets.empty() || ! mv->seam_facets.empty() || ! mv->mmu_segmentation_facets.empty() || !mv->fuzzy_skin_facets.empty();
        mv->supported_facets.reset();
        mv->seam_facets.reset();
        mv->mmu_segmentation_facets.reset();
        mv->fuzzy_skin_facets.reset();
    }
    if (paint_removed) {
        // snapshot_time is captured by copy so the lambda knows where to undo/redo to.
        m_notifications.nm()->push_notification(
                    NotificationType::CustomSupportsAndSeamRemovedAfterRepair,
                    NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                    _u8L("Custom supports and color painting were removed before repairing."));
    }
}

void ObjectEditCommands::changed_mesh(int obj_idx)
{
    ModelObject* mo = m_model->objects[obj_idx];
    sla::reproject_points_and_holes(mo);
    m_refresh->update();
    m_refresh->object_list_changed();
    m_slicing.schedule_background_process();
}

void ObjectEditCommands::reset_all_gizmos()
{
    m_layout->view3D->get_canvas3d()->reset_all_gizmos();
}

// ============================ 动作分发 ============================

void ObjectEditCommands::select_all()
{
    m_layout->view3D->select_all();
    m_layout->sidebar->obj_list()->update_selections();
}

void ObjectEditCommands::deselect_all()
{
    m_layout->view3D->deselect_all();
}

void ObjectEditCommands::on_object_select()
{
    m_layout->sidebar->obj_list()->update_selections();
    m_refresh->selection_changed();
}

void ObjectEditCommands::on_action_split_objects()
{
    split_object();
}

void ObjectEditCommands::on_action_split_volumes()
{
    split_volume();
}

void ObjectEditCommands::on_repair_model()
{
    m_layout->sidebar->obj_list()->fix_through_netfabb();
}

void ObjectEditCommands::on_action_layersediting()
{
    const bool enabling = !m_layout->view3D->is_layers_editing_enabled();
    m_layout->view3D->enable_layers_editing(enabling);
    m_notifications.nm()->set_move_from_overlay(m_layout->view3D->is_layers_editing_enabled());
    if (enabling) {
        bool has_local_z = false;
        if (const auto* opt = m_q->preset_bundle()->project_config.option<ConfigOptionBool>("dithering_local_z_mode"))
            has_local_z = opt->value;
        m_notifications.notify_vhl_dithering_conflict(has_local_z);
    }
}

// ============================ 谓词组 ============================

bool ObjectEditCommands::can_delete() const
{
    return !m_selq.get_selection().is_empty() && !m_selq.get_selection().is_wipe_tower();
}

bool ObjectEditCommands::can_delete_all() const
{
    return !m_model->objects.empty();
}

bool ObjectEditCommands::can_fix_through_netfabb() const
{
    std::vector<int> obj_idxs, vol_idxs;
    m_layout->sidebar->obj_list()->get_selection_indexes(obj_idxs, vol_idxs);

#if FIX_THROUGH_NETFABB_ALWAYS
    // Fixing always.
    return ! obj_idxs.empty() || ! vol_idxs.empty();
#else // FIX_THROUGH_NETFABB_ALWAYS
    // Fixing only if the model is not manifold.
    if (vol_idxs.empty()) {
        for (auto obj_idx : obj_idxs)
            if (m_model->objects[obj_idx]->get_repaired_errors_count() > 0)
                return true;
        return false;
    }

    int obj_idx = obj_idxs.front();
    for (auto vol_idx : vol_idxs)
        if (m_model->objects[obj_idx]->get_repaired_errors_count(vol_idx) > 0)
            return true;
    return false;
#endif // FIX_THROUGH_NETFABB_ALWAYS
}

bool ObjectEditCommands::can_simplify() const
{
    // is object for simplification selected
    if (m_selq.get_selected_object_idx() < 0) return false;
    // is already opened?
    if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().get_current_type() ==
        GLGizmosManager::EType::Simplify)
        return false;
    return true;
}

bool ObjectEditCommands::can_increase_instances() const
{
    if (!m_worker.is_idle()
     || m_layout->view3D->get_canvas3d()->get_gizmos_manager().is_in_editing_mode())
            return false;

    int obj_idx = m_selq.get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)m_model->objects.size())
        && !m_layout->sidebar->obj_list()->has_selected_cut_object()
        && std::all_of(m_model->objects[obj_idx]->instances.begin(), m_model->objects[obj_idx]->instances.end(), [](auto& inst) {return inst->printable; });
}

bool ObjectEditCommands::can_decrease_instances() const
{
    if (!m_worker.is_idle()
     || m_layout->view3D->get_canvas3d()->get_gizmos_manager().is_in_editing_mode())
            return false;

    int obj_idx = m_selq.get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)m_model->objects.size()) && (m_model->objects[obj_idx]->instances.size() > 1)
        && !m_layout->sidebar->obj_list()->has_selected_cut_object();
}

bool ObjectEditCommands::can_split(bool to_objects) const
{
    return m_layout->sidebar->obj_list()->is_splittable(to_objects);
}

bool ObjectEditCommands::can_split_to_objects() const
{
    return can_split(true);
}

bool ObjectEditCommands::can_split_to_volumes() const
{
    return (m_session.printer_technology != ptSLA) && can_split(false);
}

bool ObjectEditCommands::can_set_instance_to_object() const
{
    const int obj_idx = m_selq.get_selected_object_idx();
    return 0 <= obj_idx && obj_idx < (int)m_model->objects.size() && m_model->objects[obj_idx]->instances.size() > 1;
}

bool ObjectEditCommands::can_fillcolor() const
{
    //BBS TODO
    return true;
}

#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
bool ObjectEditCommands::can_scale_to_print_volume() const
{
    const BuildVolume_Type type = m_layout->bed.build_volume().type();
    return !m_layout->sidebar->obj_list()->has_selected_cut_object()
        && !m_layout->view3D->get_canvas3d()->get_selection().is_empty()
        && (type == BuildVolume_Type::Rectangle || type == BuildVolume_Type::Circle);
}
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

bool ObjectEditCommands::can_mirror() const
{
    return !m_layout->sidebar->obj_list()->has_selected_cut_object()
        && m_selq.get_selection().is_from_single_instance();
}

bool ObjectEditCommands::layers_height_allowed() const
{
    if (m_session.printer_technology != ptFFF)
        return false;

    int obj_idx = m_selq.get_selected_object_idx();
    return 0 <= obj_idx && obj_idx < (int)m_model->objects.size() && m_model->objects[obj_idx]->max_z() > SINKING_Z_THRESHOLD && m_layout->view3D->is_layers_editing_allowed();
}

bool ObjectEditCommands::can_layers_editing() const
{
    return layers_height_allowed();
}

bool ObjectEditCommands::can_cut_to_clipboard() const
{
    if (m_selq.is_selection_empty())
        return false;
    return true;
}

bool ObjectEditCommands::can_copy_to_clipboard() const
{
    if (!m_q->IsShown() || !m_layout->is_view3D_shown())
        return false;

    if (m_selq.is_selection_empty())
        return false;

    const Selection& selection = m_selq.get_selection();
    if ((m_q->preset_bundle()->printers.get_edited_preset().printer_technology() == ptSLA) && !selection.is_sla_compliant())
        return false;

    return true;
}

bool ObjectEditCommands::can_paste_from_clipboard() const
{
    if (!m_q->IsShown() || !m_layout->is_view3D_shown()) return false;

    const Selection& selection = m_selq.get_selection();
    const Selection::Clipboard& clipboard = selection.get_clipboard();

    if (clipboard.is_empty() && m_layout->sidebar->obj_list()->clipboard_is_empty())
        return false;

    if ((m_q->preset_bundle()->printers.get_edited_preset().printer_technology() == ptSLA) && !clipboard.is_sla_compliant())
        return false;

    Selection::EMode mode = clipboard.get_mode();
    if ((mode == Selection::Volume) && !selection.is_from_single_instance())
        return false;

    if ((mode == Selection::Instance) && (selection.get_mode() != Selection::Instance))
        return false;

    return true;
}

bool ObjectEditCommands::can_replace_with_stl() const
{
    return !m_layout->sidebar->obj_list()->has_selected_cut_object()
        && m_selq.get_selection().get_volume_idxs().size() == 1;
}

bool ObjectEditCommands::can_reload_from_disk() const
{
    if (m_layout->sidebar->obj_list()->has_selected_cut_object())
        return false;

#if ENABLE_RELOAD_FROM_DISK_REWORK
    // collect selected reloadable ModelVolumes
    std::vector<std::pair<int, int>> selected_volumes = ProjectSession::reloadable_volumes(*m_model, m_selq.get_selection());
    // nothing to reload, return
    if (selected_volumes.empty())
        return false;
#else
    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return (object_idx < other.object_idx) || ((object_idx == other.object_idx) && (volume_idx < other.volume_idx)); }
        bool operator == (const SelectedVolume& other) const { return (object_idx == other.object_idx) && (volume_idx == other.volume_idx); }
    };
    std::vector<SelectedVolume> selected_volumes;

    const Selection& selection = m_selq.get_selection();

    // collects selected ModelVolumes
    const std::set<unsigned int>& selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs) {
        const GLVolume* v = selection.get_volume(idx);
        int v_idx = v->volume_idx();
        if (v_idx >= 0) {
            int o_idx = v->object_idx();
            if (0 <= o_idx && o_idx < (int)m_model->objects.size())
                selected_volumes.push_back({ o_idx, v_idx });
        }
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

#if ENABLE_RELOAD_FROM_DISK_REWORK
    std::sort(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first < v2.first) || (v1.first == v2.first && v1.second < v2.second);
        });
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first == v2.first) && (v1.second == v2.second);
        }), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> paths;
    for (auto [obj_idx, vol_idx] : selected_volumes) {
        paths.push_back(m_model->objects[obj_idx]->volumes[vol_idx]->source.input_file);
    }
#else
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> paths;
    for (const SelectedVolume& v : selected_volumes) {
        const ModelObject* object = m_model->objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];
        if (!volume->source.input_file.empty())
            paths.push_back(volume->source.input_file);
        else if (!object->input_file.empty() && !volume->name.empty() && !volume->source.is_from_builtin_objects)
            paths.push_back(volume->name);
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    return !paths.empty();
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
