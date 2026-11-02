// SnapshotService — Plater 快照域服务实现（阶段 C 纯搬移，手册 §3.2 / §2.1）。
// ⚠ 本文件禁止 include Plater/GUI_App 宿主头（陷阱 #1、DoD #6）——
//   不得照抄 Plater.cpp 的 include 大杂烩，下列为按编译需要收敛的"真实集"：
#include "SnapshotService.hpp"
#include "../Plater.hpp"        // 阶段 H1 2.2：F3 q 桥接 + layers_height_allowed/show_delayed_error_message 公有面
#include "LayoutManager.hpp"     // 阶段 H1 2.1 模式：late_bind 后 view3D/obj_list/assemble_view/preview 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.2：View3D/Preview/AssembleView 完整类型
#include "ProjectSession.hpp"    // 阶段 H1 2.2：late_bind 后 printer_technology/update_dirty_from_stack 直调
#include "SceneRefreshCoordinator.hpp"  // 阶段 H1 2.2：late_bind 后 update 直调
#include "SlicingController.hpp" // 阶段 H1 2.2：late_bind 后 background_process 直读

#include "../GLCanvas3D.hpp"          // GLCanvas3D / CanvasType / get_gizmos_manager
#include "../GUI_ObjectList.hpp"      // ObjectList（itSettings 见 ObjectDataViewModel.hpp:32，随链可见）
#include "../GUI_Preview.hpp"         // View3D / Preview / AssembleView 完整类型
#include "../Gizmos/GLGizmosManager.hpp"
#include "../PartPlate.hpp"           // PartPlateList
#include "../BackgroundSlicingProcess.hpp"
#include "libslic3r/Model.hpp"        // Model / model_has_advanced_features
#include "libslic3r/PresetBundle.hpp" // PresetBundle / PRESET_PRINTER_NAME
#include "libslic3r/PrintConfig.hpp"  // DynamicPrintConfig / ConfigOptionFloats（wipe-tower 段）
#include "libslic3r/Utils.hpp"        // log_memory_info / format_memsize_MB
#include "boost/log/trivial.hpp"      // BOOST_LOG_TRIVIAL

namespace Slic3r {
namespace GUI {
namespace app {

// 原 Plater::priv::UpdateParams（Plater.cpp:964-968，priv 私有枚举，服务不可见）——
// 值逐字保留；G 阶段随 SceneRefreshCoordinator 收口。
enum class UpdateParams {
    FORCE_FULL_SCREEN_REFRESH          = 1,
    FORCE_BACKGROUND_PROCESSING_UPDATE = 2,
    POSTPONE_VALIDATION_ERROR_MESSAGE  = 4,
};

SnapshotService::SnapshotService(Plater* q, Model& model, PartPlateList& partplate_list)
    : m_q(q)
    , m_model(model)
    , m_partplate_list(partplate_list)
{
}

void SnapshotService::late_bind(SlicingController& slicing, ProjectSession& session, LayoutManager& layout,
                                SceneRefreshCoordinator& refresh)
{
    m_slicing = &slicing;
    m_session = &session;
    m_layout  = &layout;
    m_refresh = &refresh;
}

// —— 阶段 H2 1.1：Plater 嵌套三件套替身（§5.3 别名原形；体逐字对齐原 Plater.hpp :795/:811/:845 嵌套类）——
SnapshotService::SuppressScope::SuppressScope(Plater* plater)
    : m_s(plater->snapshots())   // 经 friend 取服务引用（Plater 私有 accessor）——与服务原生形态同一存储
{
    m_s.suppress_snapshots();
}
SnapshotService::Scope::Scope(Plater* plater, const std::string& snapshot_name) : m_plater(plater)
{
    m_plater->take_snapshot(snapshot_name);
    m_plater->suppress_snapshots();
}
SnapshotService::Scope::Scope(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type) : m_plater(plater)
{
    m_plater->take_snapshot(snapshot_name, snapshot_type);
    m_plater->suppress_snapshots();
}
SnapshotService::Scope::~Scope()
{
    m_plater->allow_snapshots();
}

SnapshotService::SingleScope::SingleScope(Plater* plater) : m_plater(plater)
{
    m_plater->single_snapshots_enter(this);
}
SnapshotService::SingleScope::~SingleScope()
{
    m_plater->single_snapshots_leave(this);
}
bool SnapshotService::SingleScope::check(bool modify)
{
    if (token && (this->modify || !modify)) return false;
    token = true;
    this->modify = modify;
    return true;
}

// 原 Plater::priv::enter_gizmos_stack（Plater.cpp:7740-7750）。
void SnapshotService::enter_gizmos_stack()
{
    assert(m_active == &m_main);
    if (m_active == &m_main) {
        m_active = &m_gizmos;
        assert(m_active->empty());
        // Take the initial snapshot of the gizmos.
        // Not localized on purpose, the text will never be shown to the user.
        this->take_snapshot(std::string("Gizmos-Initial"));
    }
}

// 原 Plater::priv::leave_gizmos_stack（Plater.cpp:7752-7763）。
bool SnapshotService::leave_gizmos_stack()
{
    bool changed = false;
    assert(m_active == &m_gizmos);
    if (m_active == &m_gizmos) {
        assert(! m_active->empty());
        changed = m_gizmos.has_undo_snapshot();
        m_active->clear();
        m_active = &m_main;
    }
    return changed;
}

// 原 Plater::priv::get_active_snapshot_index（Plater.cpp:7765-7771）。
int SnapshotService::get_active_snapshot_index()
{
    const size_t active_snapshot_time = this->undo_redo_stack().active_snapshot_time();
    const std::vector<UndoRedo::Snapshot>& ss_stack = this->undo_redo_stack().snapshots();
    const auto it = std::lower_bound(ss_stack.begin(), ss_stack.end(), UndoRedo::Snapshot(active_snapshot_time));
    return it - ss_stack.begin();
}

// 原 Plater::priv::take_snapshot（Plater.cpp:7773-7851）。
void SnapshotService::take_snapshot(const std::string& snapshot_name, const UndoRedo::SnapshotType snapshot_type)
{
    if (m_prevent > 0)
        return;
    assert(m_prevent >= 0);
    // BBS: single snapshot
    // 原 `m_single && !m_single->check(...)`：机制留守 priv，经 Host 门控消费（陷阱 #7——
    // 整个原表达式作为实参一次传入，短路语义等价）。
    if (m_q->single_blocks(snapshot_modifies_project(snapshot_type) && (snapshot_name.empty() || snapshot_name.back() != '!')))
        return;
    UndoRedo::SnapshotData snapshot_data;
    snapshot_data.snapshot_type      = snapshot_type;
    snapshot_data.printer_technology = m_session->printer_technology;
    if (m_layout->view3D->is_layers_editing_enabled())
        snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (m_layout->sidebar->obj_list()->is_selected(itSettings)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        snapshot_data.layer_range_idx = m_layout->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (m_layout->sidebar->obj_list()->is_selected(itLayer)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        snapshot_data.layer_range_idx = m_layout->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (m_layout->sidebar->obj_list()->is_selected(itLayerRoot))
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;

    // If SLA gizmo is active, ask it if it wants to trigger support generation
    // on loading this snapshot.
    if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    // BBS: add partplate logic
    if (m_session->printer_technology == ptFFF) {
        const DynamicPrintConfig& config = m_q->preset_bundle()->prints.get_edited_preset().config;
        const DynamicPrintConfig& proj_cfg = m_q->preset_bundle()->project_config;
        const ConfigOptionFloats* tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
        const ConfigOptionFloats* tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
        assert(tower_x_opt->values.size() == tower_y_opt->values.size());
        m_model.wipe_tower.positions.clear();
        m_model.wipe_tower.positions.resize(tower_x_opt->values.size());
        for (int plate_idx = 0; plate_idx < tower_x_opt->values.size(); plate_idx++) {
            ModelWipeTower& tower = m_model.wipe_tower;

            tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
            tower.rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
        }
    }
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (!canvas) {
        // During destruction, skip snapshot
        return;
    }
    
    const GLGizmosManager& gizmos = canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? m_layout->assemble_view->get_canvas3d()->get_gizmos_manager() : m_layout->view3D->get_canvas3d()->get_gizmos_manager();

    if (snapshot_type == UndoRedo::SnapshotType::ProjectSeparator)
        this->undo_redo_stack().clear();
    this->undo_redo_stack().take_snapshot(snapshot_name, m_model, m_layout->get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? m_layout->assemble_view->get_canvas3d()->get_selection() : m_layout->view3D->get_canvas3d()->get_selection(), gizmos, m_partplate_list, snapshot_data);
    if (snapshot_type == UndoRedo::SnapshotType::LeavingGizmoWithAction) {
        // Filter all but the last UndoRedo::SnapshotType::GizmoAction in a row between the last UndoRedo::SnapshotType::EnteringGizmo and UndoRedo::SnapshotType::LeavingGizmoWithAction.
        // The remaining snapshot will be renamed to a more generic name,
        // depending on what gizmo is being left.
        if (gizmos.get_current() != nullptr) {
            std::string new_name = gizmos.get_current()->get_action_snapshot_name();
            this->undo_redo_stack().reduce_noisy_snapshots(new_name);
        }
    } else if (snapshot_type == UndoRedo::SnapshotType::ProjectSeparator) {
        // Reset the "dirty project" flag.
        m_main.mark_current_as_saved();
    }
    //BBS: add PartPlateList as the paremeter for take_snapshot
    this->undo_redo_stack().release_least_recently_used();

    m_session->update_dirty_from_stack(m_main.project_modified());

    // Save the last active preset name of a particular printer technology.
    ((m_session->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = m_q->preset_bundle()->printers.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot taken: " << snapshot_name << ", Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

// 原 Plater::priv::undo（Plater.cpp:7853-7870）。
void SnapshotService::undo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    // BBS: undo-redo until modify record
    while (--it_current != snapshots.begin() && !snapshot_modifies_project(*it_current));
    if (it_current == snapshots.begin()) return;
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (!canvas) return;
    if (canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        if (it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::GizmoAction &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::EnteringGizmo &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::LeavingGizmoNoAction &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::LeavingGizmoWithAction)
            return;
    }
    this->undo_redo_to(it_current);
}

// 原 Plater::priv::redo（Plater.cpp:7872-7882）。
void SnapshotService::redo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    // BBS: undo-redo until modify record
    while (it_current != snapshots.end() && !snapshot_modifies_project(*it_current++));
    if (it_current != snapshots.end()) {
        while (it_current != snapshots.end() && !snapshot_modifies_project(*it_current++));
        this->undo_redo_to(--it_current);
    }
}

// 原 Plater::priv::undo_redo_to(size_t)（Plater.cpp:7884-7890）。
// 注：原 :7893-7905 的 up_to_date 留守 Plater（ProjectSession 域，手册 §2.3）。
void SnapshotService::undo_redo_to(size_t time_to_load)
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(time_to_load));
    assert(it_current != snapshots.end());
    this->undo_redo_to(it_current);
}

// 原 Plater::priv::undo_redo_to(iterator)（Plater.cpp:7907-8049）。
void SnapshotService::undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot)
{
    // Make sure that no updating function calls take_snapshot until we are done.
    // 原 `SuppressSnapshots snapshot_supressor(q);`（:7910）→ 服务原生 RAII 替身（陷阱 #2）。
    SuppressScope snapshot_supressor(*this);

    bool 				temp_snapshot_was_taken 	= this->undo_redo_stack().temp_snapshot_active();
    PrinterTechnology 	new_printer_technology 		= it_snapshot->snapshot_data.printer_technology;
    bool 				printer_technology_changed 	= m_session->printer_technology != new_printer_technology;
    if (printer_technology_changed) {
        //BBS do not support SLA
    }
    // Save the last active preset name of a particular printer technology.
    ((m_session->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = m_q->preset_bundle()->printers.get_selected_preset_name();
    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    // BBS: add partplate logic
    if (m_session->printer_technology == ptFFF) {
        const DynamicPrintConfig& config = m_q->preset_bundle()->prints.get_edited_preset().config;
        const DynamicPrintConfig& proj_cfg = m_q->preset_bundle()->project_config;
        const ConfigOptionFloats* tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
        const ConfigOptionFloats* tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
        assert(tower_x_opt->values.size() == tower_y_opt->values.size());
        m_model.wipe_tower.positions.clear();
        m_model.wipe_tower.positions.resize(tower_x_opt->values.size());
        for (int plate_idx = 0; plate_idx < tower_x_opt->values.size(); plate_idx++) {
            ModelWipeTower& tower = m_model.wipe_tower;

            tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
            tower.rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
        }
    }
    const int layer_range_idx = it_snapshot->snapshot_data.layer_range_idx;
    // Flags made of Snapshot::Flags enum values.
    unsigned int new_flags = it_snapshot->snapshot_data.flags;
    UndoRedo::SnapshotData top_snapshot_data;
    top_snapshot_data.printer_technology = m_session->printer_technology;
    if (m_layout->view3D->is_layers_editing_enabled())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (m_layout->sidebar->obj_list()->is_selected(itSettings)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = m_layout->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (m_layout->sidebar->obj_list()->is_selected(itLayer)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = m_layout->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (m_layout->sidebar->obj_list()->is_selected(itLayerRoot))
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;
    bool   		 new_variable_layer_editing_active = (new_flags & UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE) != 0;
    bool         new_selected_settings_on_sidebar  = (new_flags & UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR) != 0;
    bool         new_selected_layer_on_sidebar     = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR) != 0;
    bool         new_selected_layerroot_on_sidebar = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR) != 0;

    if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    // Disable layer editing before the Undo / Redo jump.
    if (!new_variable_layer_editing_active && m_layout->view3D->is_layers_editing_enabled())
        m_layout->view3D->get_canvas3d()->force_main_toolbar_left_action(m_layout->view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));

    // Make a copy of the snapshot, undo/redo could invalidate the iterator
    const UndoRedo::Snapshot snapshot_copy = *it_snapshot;
    // Do the jump in time.
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (!canvas) return;
    
    bool is_assemble = canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView;
    if (it_snapshot->timestamp < this->undo_redo_stack().active_snapshot_time() ?
        this->undo_redo_stack().undo(m_model, is_assemble ? m_layout->assemble_view->get_canvas3d()->get_selection() : m_layout->view3D->get_canvas3d()->get_selection(), is_assemble ? m_layout->assemble_view->get_canvas3d()->get_gizmos_manager() : m_layout->view3D->get_canvas3d()->get_gizmos_manager(), m_partplate_list, top_snapshot_data, it_snapshot->timestamp) :
        this->undo_redo_stack().redo(m_model, is_assemble ? m_layout->assemble_view->get_canvas3d()->get_gizmos_manager() : m_layout->view3D->get_canvas3d()->get_gizmos_manager(), m_partplate_list, it_snapshot->timestamp)) {
        if (printer_technology_changed) {
            // Switch to the other printer technology. Switch to the last printer active for that particular technology.
            AppConfig *app_config = m_q->app_config();
            app_config->set("presets", PRESET_PRINTER_NAME, (new_printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name);
            //FIXME Why are we reloading the whole preset bundle here? Please document. This is fishy and it is unnecessarily expensive.
            // Anyways, don't report any config value substitutions, they have been already reported to the user at application start up.
            m_q->preset_bundle()->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
            // load_current_presets() calls Tab::load_current_preset() -> TabPrint::update() -> Object_list::update_and_show_object_settings_item(),
            // but the Object list still keeps pointer to the old Model. Avoid a crash by removing selection first.
            m_layout->sidebar->obj_list()->unselect_objects();
            // Load the currently selected preset into the GUI, update the preset selection box.
            // This also switches the printer technology based on the printer technology of the active printer profile.
            m_q->load_current_presets();
        }
        //FIXME updating the Print config from the Wipe tower config values at the ModelWipeTower.
        // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
        // BBS: add partplate logic
        if (m_session->printer_technology == ptFFF) {
            const DynamicPrintConfig& config = m_q->preset_bundle()->prints.get_edited_preset().config;
            const DynamicPrintConfig& proj_cfg = m_q->preset_bundle()->project_config;
            ConfigOptionFloats* tower_x_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_x"));
            ConfigOptionFloats* tower_y_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_y"));
            // BBS: don't support wipe tower rotation
            //double current_rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
            bool need_update = false;
            if (tower_x_opt->values.size() != m_model.wipe_tower.positions.size()) {
                tower_x_opt->clear();
                ConfigOptionFloat default_tower_x(40.f);
                tower_x_opt->resize(m_model.wipe_tower.positions.size(), &default_tower_x);
                need_update = true;
            }

            if (tower_y_opt->values.size() != m_model.wipe_tower.positions.size()) {
                tower_y_opt->clear();
                ConfigOptionFloat default_tower_y(200.f);
                tower_y_opt->resize(m_model.wipe_tower.positions.size(), &default_tower_y);
                need_update = true;
            }

            for (int plate_idx = 0; plate_idx < m_model.wipe_tower.positions.size(); plate_idx++) {
                if (Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx)) != m_model.wipe_tower.positions[plate_idx]) {
                    ConfigOptionFloat tower_x_new(m_model.wipe_tower.positions[plate_idx].x());
                    ConfigOptionFloat tower_y_new(m_model.wipe_tower.positions[plate_idx].y());
                    tower_x_opt->set_at(&tower_x_new, plate_idx, 0);
                    tower_y_opt->set_at(&tower_y_new, plate_idx, 0);
                    need_update = true;
                }
            }

            if (need_update) {
                // update print to current plate (preview->m_process)
                m_partplate_list.update_slice_context_to_current_plate(m_slicing->background_process());
                m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
                m_refresh->update(0);
            }
        }
        // set selection mode for ObjectList on sidebar
        m_layout->sidebar->obj_list()->set_selection_mode(new_selected_settings_on_sidebar  ? ObjectList::SELECTION_MODE::smSettings :
                                                      new_selected_layer_on_sidebar     ? ObjectList::SELECTION_MODE::smLayer :
                                                      new_selected_layerroot_on_sidebar ? ObjectList::SELECTION_MODE::smLayerRoot :
                                                                                          ObjectList::SELECTION_MODE::smUndef);
        if (new_selected_settings_on_sidebar || new_selected_layer_on_sidebar)
            m_layout->sidebar->obj_list()->set_selected_layers_range_idx(layer_range_idx);

        this->update_after_undo_redo(snapshot_copy, temp_snapshot_was_taken);
        // Enable layer editing after the Undo / Redo jump.
        if (!m_layout->view3D->is_layers_editing_enabled() && m_q->layers_height_allowed() && new_variable_layer_editing_active)
            m_layout->view3D->get_canvas3d()->force_main_toolbar_left_action(m_layout->view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    }

    m_session->update_dirty_from_stack(m_main.project_modified());
    m_q->update_title_dirty_status();
}

// 原 Plater::priv::update_after_undo_redo（Plater.cpp:8051-8091）。
void SnapshotService::update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool /* temp_snapshot_was_taken */)
{
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (!canvas) return;
    
    bool is_assemble = canvas->get_canvas_type() == GLCanvas3D::CanvasAssembleView;
    is_assemble ? m_layout->assemble_view->get_canvas3d()->get_selection().clear() : m_layout->view3D->get_canvas3d()->get_selection().clear();
    // Update volumes from the deserializd model, always stop / update the background processing (for both the SLA and FFF technologies).
    m_refresh->update((unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE | (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
    // Release old snapshots if the memory allocated is excessive. This may remove the top most snapshot if jumping to the very first snapshot.
    //if (temp_snapshot_was_taken)
    // Release the old snapshots always, as it may have happened, that some of the triangle meshes got deserialized from the snapshot, while some
    // triangle meshes may have gotten released from the scene or the background processing, therefore now being calculated into the Undo / Redo stack size.
        this->undo_redo_stack().release_least_recently_used();
    //YS_FIXME update obj_list from the deserialized model (maybe store ObjectIDs into the tree?) (no selections at this point of time)
        is_assemble ?
            m_layout->assemble_view->get_canvas3d()->get_selection().set_deserialized(GUI::Selection::EMode(this->undo_redo_stack().selection_deserialized().mode), this->undo_redo_stack().selection_deserialized().volumes_and_instances) :
            m_layout->view3D->get_canvas3d()->get_selection().set_deserialized(GUI::Selection::EMode(this->undo_redo_stack().selection_deserialized().mode), this->undo_redo_stack().selection_deserialized().volumes_and_instances);
    is_assemble ?
        m_layout->assemble_view->get_canvas3d()->get_gizmos_manager().update_after_undo_redo(snapshot) :
        m_layout->view3D->get_canvas3d()->get_gizmos_manager().update_after_undo_redo(snapshot);

    m_layout->sidebar->obj_list()->update_after_undo_redo();

    if (m_q->get_mode() == comSimple && model_has_advanced_features(m_model)) {
        // If the user jumped to a snapshot that require user interface with advanced features, switch to the advanced mode without asking.
        // There is a little risk of surprising the user, as he already must have had the advanced or advanced mode active for such a snapshot to be taken.
        m_q->save_mode(comAdvanced);
        m_layout->view3D->set_as_dirty();
    }

    // this->update() above was called with POSTPONE_VALIDATION_ERROR_MESSAGE, so that if an error message was generated when updating the back end, it would not open immediately,
    // but it would be saved to be show later. Let's do it now. We do not want to display the message box earlier, because on Windows & OSX the message box takes over the message
    // queue pump, which in turn executes the rendering function before a full update after the Undo / Redo jump.
    m_q->show_delayed_error_message();

    //FIXME what about the state of the manipulators?
    //FIXME what about the focus? Cursor in the side panel?

    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot reloaded. Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

// 原 Plater::undo_redo_string_getter（Plater.cpp:12666-12677）——本体实现迁服务，门面保留转发。
bool SnapshotService::undo_redo_string_getter(const bool is_undo, int idx, const char** out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = this->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = this->get_active_snapshot_index() + (is_undo ? -(++idx) : idx);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        *out_text = ss_stack[idx_in_ss_stack].name.c_str();
        return true;
    }

    return false;
}

// 原 Plater::undo_redo_topmost_string_getter（Plater.cpp:12680-12690）——同上。
void SnapshotService::undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = this->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = this->get_active_snapshot_index() + (is_undo ? -1 : 0);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        out_text = ss_stack[idx_in_ss_stack].name;
        return;
    }

    out_text = "";
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
