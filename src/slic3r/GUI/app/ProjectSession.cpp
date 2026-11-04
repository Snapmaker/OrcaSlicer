#include "ProjectSession.hpp"

#include "slic3r/GUI/app/SnapshotService.hpp"
#include "../Plater.hpp"                   // H1 2.3: q faces + F3 bridges + publicized load_auxiliary_files
#include "../Tab.hpp"                      // H1 2.3: get_tab()->update() Tab 完整型（b23_4 C2027）
#include "SlicingController.hpp"           // H1 2.3: m_slicing typed（background_process/select_plate/schedule）
#include "NotificationHub.hpp"             // H1 2.3: m_hub.nm()
#include "MixingGateKeeper.hpp"            // H1 2.3: late_bind gate_keeper（sync_* ×3）
#include "SelectionQuery.hpp"              // H1 2.3: late_bind selq（get_selection）
#include "LayoutManager.hpp"               // H1 2.3: late_bind layout（view3d/preview/obj_list/bed/camera 族）
#include "SceneRefreshCoordinator.hpp"     // H1 2.3: late_bind refresh（update/skip_thumbnail/object_list_changed）
#include "ExportSendCoordinator.hpp"       // H1 2.3: late_bind ex（ask_export_path/ask_save_file/export_3mf_to）
#include "slic3r/GUI/app/PlateController.hpp"

#include "../MainFrame.hpp"               // 仅 cpp（手册 §3）：m_main_frame 解引用（SetTitle/topbar/update_title）
#include "../I18N.hpp"                    // _L
#include "../format.hpp"                  // format_wxstr
#include "../MsgDialog.hpp"               // MessageDialog（save_project_if_dirty/close_with_confirm 脏确认）
#include "../GUI.hpp"                     // into_u8/into_path/from_u8/from_path/show_error
#include "../NotificationManager.hpp"     // NotificationType（new/load_project 通知关闭组）
#include "../UnsavedChangesDialog.hpp"    // ActionButtons（check_and_keep_current_preset_changes 按钮位）
#include "../Camera.hpp"                  // REQUIRES_ZOOM_TO_ALL_PLATE
#include "../FlowTypeHelper.hpp"          // FlowType::reset_nozzle_volume_types_to_standard
#include "../PartPlate.hpp"               // PartPlateList/PartPlate（load_files 清盘/重建/平移）
#include "../GUI_Preview.hpp"             // View3D/Preview（view3d/preview 槽解引用）
#include "../GLCanvas3D.hpp"              // GLCanvas3D/SceneRaycaster（register_raycasters 等）
#include "../Selection.hpp"               // Selection（load_files 自动选盘）
#include "../Gizmos/GLGizmosManager.hpp"  // get_gizmos_manager（is_enabled 门控）
#include "../Gizmos/GLGizmoSimplify.hpp"  // add_simplify_suggestion_notification
#include "../3DBed.hpp"                   // Bed3D::Axes::DefaultTipRadius（translate_old 复位）
#include "../GUI_ObjectList.hpp"          // obj_list 槽解引用（update_selections/add_objects_to_list）
#include "../BackgroundSlicingProcess.hpp" // background_process 槽解引用（calib_mode 复位）
#include "../GUI_Utils.hpp"               // find_toplevel_parent
#include "../ObjColorDialog.hpp"          // obj_color_fun 对话框
#include "../StepMeshDialog.hpp"          // STEP 网格设置对话框
#include "../FileArchiveDialog.hpp"       // preview_zip_archive 选件对话框
#include "slic3r/Utils/UndoRedo.hpp"      // UndoRedo::Stack（SnapshotService.hpp 已带，此处显式）
#include "nlohmann/json.hpp"              // save_project 的 j 统计块（原样保留）
#include "libslic3r/miniz_extension.hpp"  // mz_zip_* / open_zip_reader（preview_zip_archive）
#include "libslic3r/Utils.hpp"            // encode_path
#include "libslic3r/LocalesUtils.hpp"     // string_to_double_decimal_point
#include "libslic3r/PresetBundle.hpp"     // preset_bundle 槽解引用
#include "common_func/common_func.hpp"    // Snapmaker_VERSION
#include "libslic3r/AppConfig.hpp"        // AppConfig（app_config 槽解引用）
#include "libslic3r/SLA/ReprojectPointsOnMesh.hpp"  // sla::reproject_points_and_holes
#include <wx/busyinfo.h>                  // wxBusyInfo（replace/reload 进程提示）
#include <wx/glcanvas.h>                  // wxGLCanvas 完整类型（get_wxglcanvas → wxWindow* 上溯转换）
#include "../wxExtensions.hpp"            // FromDIP（import_model_id 对话框尺寸；原经 Plater.cpp 传递可见）
#include <boost/uuid/uuid.hpp>            // import_model_id 下载唯一名
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <wx/progdlg.h>                   // ProgressDialog
#include <wx/stdpaths.h>                  // wxStandardPaths（preview_zip_archive 临时目录）
#include <wx/filename.h>                  // wxFileExists（load_gcode）
#include <boost/nowide/convert.hpp>       // boost::nowide::widen
#include <boost/algorithm/string.hpp>     // iends_with/icontains

#include "libslic3r/Format/bbs_3mf.hpp"   // SaveStrategy / LoadStrategy（完整定义）+ put_other_changes
#include "libslic3r/Model.hpp"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <cstring>

namespace fs = boost::filesystem;

static wxString dots("...", wxConvUTF8);

namespace Slic3r {
namespace GUI {
namespace app {

Model& ProjectSession::model() const { return *m_model; }   // H2 收口（用户授权接口扩展 2026-10-23）

ProjectSession::ProjectSession(Plater* q, Model* model, DynamicPrintConfig* config, SnapshotService& snapshots,
                               SlicingController& slicing, PartPlateList& partplate_list, NotificationHub& hub,
                               PlateController& plates, MainFrame* main_frame)
    : m_q(q), m_model(model), m_config(config), m_snapshots(snapshots)
    , m_slicing(slicing), m_partplate_list(partplate_list), m_hub(hub), m_plates(plates)
    , m_main_frame(main_frame)
{
}

void ProjectSession::late_bind(ExportSendCoordinator& ex, SelectionQuery& selq, LayoutManager& layout,
                               SceneRefreshCoordinator& refresh, MixingGateKeeper& gate_keeper)
{
    m_ex          = &ex;
    m_selq        = &selq;
    m_layout      = &layout;
    m_refresh     = &refresh;
    m_gate_keeper = &gate_keeper;
}

ProjectSession::~ProjectSession() = default;


const std::regex ProjectSession::pattern_bundle(".*[.](amf|amf[.]xml|zip[.]amf|3mf)", std::regex::icase);
const std::regex ProjectSession::pattern_3mf(".*3mf", std::regex::icase);
const std::regex ProjectSession::pattern_zip_amf(".*[.]zip[.]amf", std::regex::icase);
const std::regex ProjectSession::pattern_any_amf(".*[.](amf|amf[.]xml|zip[.]amf)", std::regex::icase);
const std::regex ProjectSession::pattern_prusa(".*bbl", std::regex::icase);


bool ProjectSession::has_illegal_filename_characters(const wxString& wxs_name)
{
    std::string name = into_u8(wxs_name);
    return has_illegal_filename_characters(name);
}

bool ProjectSession::has_illegal_filename_characters(const std::string& name)
{
    const char* illegal_characters = "<>:/\\|?*\"";
    for (size_t i = 0; i < std::strlen(illegal_characters); i++)
        if (name.find_first_of(illegal_characters[i]) != std::string::npos)
            return true;

    return false;
}

void ProjectSession::show_illegal_characters_warning(wxWindow* parent)
{
    show_error(parent, _L("Invalid name, the following characters are not allowed:") + " <>:/\\|?*\"");
}


void ProjectSession::set_plater_dirty(bool is_dirty) { dirty_state.set_plater_dirty(is_dirty); }
bool ProjectSession::is_project_dirty() const { return dirty_state.is_dirty(); }
bool ProjectSession::is_presets_dirty() const { return dirty_state.is_presets_dirty(); }

void ProjectSession::update_project_dirty_from_presets()
{
    // BBS: backup
    Slic3r::put_other_changes();   // 备份副作用序冻结（手册 E1 陷阱 6）：在 dirty_state 调用之前，逐字保留
    dirty_state.update_from_presets();
}

int ProjectSession::save_project_if_dirty(const wxString& reason)
{
    int res = wxID_NO;
    if (dirty_state.is_dirty()) {
        MainFrame* mainframe = m_main_frame;   // 原 wxGetApp().mainframe
        if (mainframe->can_save_as()) {
            wxString suggested_project_name;
            wxString project_name = suggested_project_name = get_project_filename(".3mf");
            if (suggested_project_name.IsEmpty()) {
                fs::path output_file = m_ex->get_export_file_path(FT_3MF);   // 缝合（勘误 4）：原 get_export_file_path，E2 归 EX
                suggested_project_name = output_file.empty() ? _L("Untitled") : from_u8(output_file.stem().string());
            }
            res = MessageDialog(mainframe, reason + "\n" + format_wxstr(_L("Do you want to save changes to \"%1%\"?"), suggested_project_name),
                                wxString(SLIC3R_APP_FULL_NAME), wxYES_NO | wxCANCEL).ShowModal();
            if (res == wxID_YES)
                if (!mainframe->save_project_as(project_name))
                    res = wxID_CANCEL;
        }
    }
    return res;
}

void ProjectSession::reset_project_dirty_after_save() { m_snapshots.undo_redo_stack_main().mark_current_as_saved(); dirty_state.reset_after_save(); }
void ProjectSession::reset_project_dirty_initial_presets() { dirty_state.reset_initial_presets(); }

bool ProjectSession::up_to_date(bool saved, bool backup)
{
    size_t& last_time = backup ? m_backup_timestamp : m_saved_timestamp;
    if (saved) {
        last_time = m_snapshots.undo_redo_stack_main().active_snapshot_time();
        if (!backup)
            m_snapshots.undo_redo_stack_main().mark_current_as_saved();
        return true;
    }
    else {
        return !m_snapshots.undo_redo_stack_main().has_real_change_from(last_time);
    }
}

void ProjectSession::update_dirty_from_stack(bool m) { dirty_state.update_from_undo_redo_stack(m); }

void ProjectSession::update_title_dirty_status()
{
    if (m_project_name.empty())
        return;

    wxString title;
    if (is_project_dirty())
        title = "*" + m_project_name;
    else
        title = m_project_name;

#ifdef __WINDOWS__
    m_main_frame->topbar()->SetTitle(title);
#else
    m_main_frame->SetTitle(title);
    m_main_frame->update_title_colour_after_set_title();
#endif
}


wxString ProjectSession::get_project_filename(const wxString& extension) const
{
    if (m_project_name.empty())
        return "";
    else {
        auto full_filename = m_project_folder / std::string((m_project_name + extension).mb_str(wxConvUTF8));
        return m_project_folder.empty() ? "" : from_path(full_filename);
    }
}

wxString ProjectSession::get_project_name() const
{
    return m_project_name;
}

void ProjectSession::set_project_name(const wxString& project_name)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " project is:" << project_name;
    m_project_name = project_name;
    //update topbar title
#ifdef __WINDOWS__
    m_main_frame->SetTitle(m_project_name + " - " + wxString(SLIC3R_APP_NAME));
    m_main_frame->topbar()->SetTitle(m_project_name);
#else
    m_main_frame->SetTitle(m_project_name);
    if (!m_project_name.IsEmpty())
        m_main_frame->update_title_colour_after_set_title();
#endif
}

void ProjectSession::set_project_filename(const wxString& filename)
{
    boost::filesystem::path full_path = into_path(filename);
    boost::filesystem::path ext = full_path.extension();
    //if (boost::iequals(ext.string(), ".amf")) {
    //    // Remove the first extension.
    //    full_path.replace_extension("");
    //    // It may be ".zip.amf".
    //    if (boost::iequals(full_path.extension().string(), ".zip"))
    //        // Remove the 2nd extension.
    //        full_path.replace_extension("");
    //} else {
    //    // Remove just one extension.
    //    full_path.replace_extension("");
    //}
    full_path.replace_extension("");

    m_project_folder = full_path.parent_path();
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " project folder is:" << m_project_folder.string();

    //BBS
    wxString project_name = from_u8(full_path.filename().string());
    set_project_name(project_name);
    // record filename for hint when open exported file/.gcode
    if (only_gcode_mode())
        m_preview_only_filename = std::string((project_name + ".gcode").mb_str());
    if (using_exported_file())
        m_preview_only_filename = std::string((project_name + ".3mf").mb_str());

    m_main_frame->update_title();

    if (!m_project_folder.empty() && !only_gcode_mode())
        m_main_frame->add_to_recent_projects(filename);
}


bool        ProjectSession::only_gcode_mode() const { return m_only_gcode; }
void        ProjectSession::set_only_gcode(bool only_gcode) { m_only_gcode = only_gcode; }
bool        ProjectSession::using_exported_file() const { return m_exported_file; }
void        ProjectSession::set_using_exported_file(bool exported_file) { m_exported_file = exported_file; }
bool        ProjectSession::is_loading_project() const { return m_loading_project; }
void        ProjectSession::set_loading_project(bool loading) { m_loading_project = loading; }
std::string ProjectSession::get_preview_only_filename() const { return m_preview_only_filename; }
int         ProjectSession::valid_plates_count() const { return m_valid_plates_count; }
void        ProjectSession::set_valid_plates_count(int count) { m_valid_plates_count = count; }
const wxString& ProjectSession::get_last_loaded_gcode() const { return m_last_loaded_gcode; }
void        ProjectSession::reset_last_loaded_gcode() { m_last_loaded_gcode = ""; }


int ProjectSession::new_project(bool skip_confirm, bool silent, const wxString& project_name)
{
    bool transfer_preset_changes = false;
    // BBS: save confirm
    auto check = [this, &transfer_preset_changes](bool yes_or_no) {
        wxString header = _L("Some presets are modified.") + "\n" +
            (yes_or_no ? _L("You can keep the modified presets to the new project or discard them") :
                _L("You can keep the modified presets to the new project, discard or save changes as new presets."));
        int act_buttons = ActionButtons::KEEP | ActionButtons::REMEMBER_CHOISE;
        if (!yes_or_no)
            act_buttons |= ActionButtons::SAVE;
        return m_q->check_and_keep_current_preset_changes(_L("Creating a new project"), header, act_buttons, &transfer_preset_changes);
    };
    int result;
    if (!skip_confirm && (result = close_with_confirm(check)) == wxID_CANCEL)
        return wxID_CANCEL;

    set_only_gcode(false);
    set_using_exported_file(false);
    set_loading_project(false);
    m_hub.nm()->bbl_close_plateinfo_notification();
    m_hub.nm()->bbl_close_preview_only_notification();
    m_hub.nm()->bbl_close_3mf_warn_notification();
    m_hub.nm()->close_notification_of_type(NotificationType::PlaterError);
    m_hub.nm()->close_notification_of_type(NotificationType::PlaterWarning);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingError);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingSeriousWarning);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingWarning);

    if (!silent)
        m_main_frame->select_tab(MainFrame::tp3DEditor);

    //get_partplate_list().reinit();
    //get_partplate_list().update_slice_context_to_current_plate(p->slicing().background_process());
    //p->preview->update_gcode_result(p->m_partplate_list.get_current_slice_result());
    m_q->reset(transfer_preset_changes);
    reset_project_dirty_after_save();
    reset_project_dirty_initial_presets();
    m_q->update_saved_preset_from_current_preset();
    update_project_dirty_from_presets();

    //reset project
    project.reset();
    //set project name
    if (project_name.empty())
        set_project_name(_L("Untitled"));
    else
        set_project_name(project_name);

    m_snapshots.take_snapshot("New Project", UndoRedo::SnapshotType::ProjectSeparator);
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);

    Model m;
    m_model->load_from(m); // new id avoid same path name

    //select first plate
    m_partplate_list.select_plate(0);
    { SimpleEvent e(EVT_GLCANVAS_PLATE_SELECT); m_plates.on_plate_selected(); }

    m_q->load_auxiliary_files();
    m_q->app_config()->update_last_backup_dir(m_model->get_backup_path());

    // BBS set default view and zoom
    m_layout->select_view_3D("3D");
    m_layout->select_view("topfront");
    m_layout->camera.requires_zoom_to_bed = true;
    m_layout->enable_sidebar(!only_gcode_mode());

    up_to_date(true, false);
    up_to_date(true, true);

    // Snapmaker: the printer preset carries no per-nozzle flow-type field, so a new
    // project must start every nozzle at standard flow rather than inheriting the
    // previous project's nozzle_volume_type (which lives in the shared project config).
    GUI::FlowType::reset_nozzle_volume_types_to_standard();
    // Rebuild the nozzle panel so the flow combos reflect the reset. Entry points that
    // select a preset first (e.g. the home-page device card -> sw_NewProject) already
    // rebuilt the panel via on_select_preset BEFORE this reset ran, so without an
    // explicit rebuild here the combos would keep showing the pre-reset flow types.
    // Deferred with CallAfter to avoid destroying a combo mid-event on the select path.
    m_q->update_nozzle_settings_after();

    return wxID_YES;
}

// BBS: FIXME, missing resotre logic
void ProjectSession::load_project(const wxString& filename2, const wxString& originfile)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "filename is: " << filename2 << "and originfile is: " << originfile;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__;
    auto filename = filename2;
    auto check = [&filename, this] (bool yes_or_no) {
        if (!yes_or_no && !m_q->check_and_save_current_preset_changes(_L("Load project"),
                _L("Some presets are modified.")))
            return false;
        if (filename.empty()) {
            // Ask user for a project file name.
            m_q->load_project_dialog(filename);
        }
        return !filename.empty();
    };

    // BSS: save project, force close
    int result;
    if ((result = close_with_confirm(check)) == wxID_CANCEL) {
        return;
    }

    // BBS
    if (is_loading_project()) {
        //some error cases happens
        //return directly
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": current loading other project, return directly");
        return;
    }
    else
        set_loading_project(true);

    set_only_gcode(false);
    set_using_exported_file(false);
    m_hub.nm()->bbl_close_plateinfo_notification();
    m_hub.nm()->bbl_close_preview_only_notification();
    m_hub.nm()->bbl_close_3mf_warn_notification();
    m_hub.nm()->close_notification_of_type(NotificationType::PlaterError);
    m_hub.nm()->close_notification_of_type(NotificationType::PlaterWarning);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingError);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingSeriousWarning);
    m_hub.nm()->close_notification_of_type(NotificationType::SlicingWarning);

    auto path     = into_path(filename);

    auto strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
    if (originfile == "<silence>") {
        strategy = strategy | LoadStrategy::Silence;
    } else if (originfile == "<loadall>") {
        // Do nothing
    } else if (originfile != "-") {
        strategy = strategy | LoadStrategy::Restore;
    } else {
        switch (m_q->determine_load_type(filename.ToStdString(), "")) {
            case 1 /*LoadType::OpenProject*/: break; // Do nothing
            case 2 /*LoadType::LoadGeometry*/:; strategy = LoadStrategy::LoadModel; break;
            default: return; // User cancelled
        }
    }
    bool load_restore = strategy & LoadStrategy::Restore;

    // Take the Undo / Redo snapshot.
    m_q->reset(false);

    m_snapshots.take_snapshot("Load Project", UndoRedo::SnapshotType::ProjectSeparator);
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);   // 原嵌套版 TakeSnapshot

    std::vector<fs::path> input_paths;
    input_paths.push_back(path);
    if (strategy & LoadStrategy::Restore)
        input_paths.push_back(into_u8(originfile));

    std::vector<size_t> res = load_files(input_paths, strategy, false);

    reset_project_dirty_initial_presets();
    update_project_dirty_from_presets();
    m_q->preset_bundle()->export_selections(*m_q->app_config());

    // if res is empty no data has been loaded
    if (!res.empty() && (load_restore || !(strategy & LoadStrategy::Silence))) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << (load_restore ? originfile : filename);
        set_project_filename(load_restore ? originfile : filename);
        if (load_restore && originfile.IsEmpty()) {
        set_project_name(_L("Untitled"));
        }

    } else {
        if (using_exported_file()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " using ecported set project filename: " << filename;
            set_project_filename(filename);
        }

    }

    // BBS set default 3D view and direction after loading project
    //p->select_view_3D("3D");
    if (!using_exported_file()) {
        m_layout->select_view("topfront");
        m_layout->camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
        m_main_frame->select_tab(MainFrame::tp3DEditor);
    }
    else {
        m_partplate_list.select_plate_view();
    }

    m_layout->enable_sidebar(!only_gcode_mode());

    m_q->app_config()->update_last_backup_dir(m_model->get_backup_path());
    if (load_restore && !originfile.empty()) {
        m_q->app_config()->update_skein_dir(into_path(originfile).parent_path().string());
        m_q->app_config()->update_config_dir(into_path(originfile).parent_path().string());
    }

    if (!load_restore)
        up_to_date(true, false);
    else
        update_dirty_from_stack(true);
    up_to_date(true, true);

    m_q->params_panel_switch_to_object();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " load project done";
    set_loading_project(false);
}

// BBS: save logic
int ProjectSession::save_project(bool saveAs)
{
    //if (up_to_date(false, false)) // should we always save
    //    return;
    auto filename = get_project_filename(".3mf");
    if (!saveAs && filename.IsEmpty())
        saveAs = true;
    if (saveAs)
        filename = m_ex->get_export_file(FT_3MF);   // 缝合：原 p->get_export_file（E2 归 EX）
    if (filename.empty())
        return wxID_NO;
    if (filename == "<cancel>")
        return wxID_CANCEL;

    //BBS export 3mf without gcode
    if (m_ex->export_3mf(into_path(filename), SaveStrategy::SplitModel | SaveStrategy::ShareMesh | SaveStrategy::FullPathSources, -1) < 0) {   // 缝合：原 Plater::export_3mf（E2 归 EX）
        MessageDialog(m_q, _L("Failed to save the project.\nPlease check whether the folder exists online or if other programs open the project file."),
            _L("Save project"), wxOK | wxICON_WARNING).ShowModal();
        return wxID_CANCEL;
    }

    Slic3r::remove_backup(*m_model, false);

    set_project_filename(filename);
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << filename;

    up_to_date(true, false);
    up_to_date(true, true);

    m_q->update_saved_preset_from_current_preset();
    reset_project_dirty_after_save();
    try {
        json j;
        boost::uintmax_t size = boost::filesystem::file_size(into_path(filename));
        j["file_size"] = size;
        j["file_name"] = std::string(filename.mb_str());

        NetworkAgent* agent = m_q->get_agent();
    }
    catch (...) {}

    update_title_dirty_status();
    return wxID_YES;
}

int ProjectSession::close_with_confirm(std::function<bool(bool)> second_check)
{
    // Session end: discard without dialog (see wxEVT_QUERY_END_SESSION handler).
    if (wxGetApp().in_session_end())
        return wxID_NO;

    if (up_to_date(false, false)) {
        if (second_check && !second_check(false)) return wxID_CANCEL;
        m_model->set_backup_path("");
        return wxID_NO;
    }

    MessageDialog dlg(m_q, _L("The current project has unsaved changes, save it before continue?"),
        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
    dlg.show_dsa_button(_L("Remember my choice."));
    auto choise = m_q->app_config()->get("save_project_choise");
    auto result = choise.empty() ? dlg.ShowModal() : choise == "yes" ? wxID_YES : wxID_NO;
    if (result == wxID_CANCEL)
        return result;
    else {
        if (dlg.get_checkbox_state())
            m_q->app_config()->set("save_project_choise", result == wxID_YES ? "yes" : "no");
        if (result == wxID_YES) {
            result = save_project();
            if (result == wxID_CANCEL) {
                if (choise.empty())
                    return result;
                else
                    result = wxID_NO;
            }
        }
    }

    if (second_check && !second_check(result == wxID_YES)) return wxID_CANCEL;

    m_model->set_backup_path("");
    up_to_date(true, false);
    up_to_date(true, true);

    return result;
}

//BBS: trigger a restore project event
void ProjectSession::trigger_restore_project(int skip_confirm)
{
    m_q->trigger_restore_project_evt(skip_confirm);
}


std::vector<size_t> ProjectSession::load_files(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy, bool ask_multi) {
    //BBS: wish to reset state when load a new file
    m_slicing.set_slice_all_only_has_gcode(false);
    //BBS: wish to reset all plates stats item selected state when load a new file
    m_layout->preview->get_canvas3d()->reset_select_plate_toolbar_selection();
    std::vector<size_t> loaded = load_files_core(input_files, strategy, ask_multi);
    if (!loaded.empty())
    {
        // After loading a project, initialize the filament temp mixing state
        // for ALL plates, not just the current one. This ensures each plate's
        // After loading a project, force a filament usage sync so that
        // the current plate's notification and slice button reflect the
        // filament temp mixing state. The blocking itself is enforced via
        // is_plate_blocked_by_filament_temp_mixing() independently of
        // m_apply_invalid; no per-plate flag initialization is needed.
        m_q->notify_filament_usage_changed();
        m_gate_keeper->sync_filament_temp_mixing_notification();
        m_gate_keeper->sync_flow_ratio_zero_notification();
        m_gate_keeper->sync_cold_plate_notification();
    }
    return loaded;
}
std::vector<size_t> ProjectSession::load_files(const std::vector<std::string>& input_files, LoadStrategy strategy,  bool ask_multi)
{
    std::vector<fs::path> paths;
    paths.reserve(input_files.size());
    for (const std::string& path : input_files)
        paths.emplace_back(path);
    return load_files(paths, strategy, ask_multi);
}
std::vector<size_t> ProjectSession::load_files_core(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy, bool ask_multi)
{
    std::vector<size_t> empty_result;
    bool dlg_cont = true;
    bool is_user_cancel = false;
    bool translate_old = false;
    int current_width = 0, current_depth = 0, current_height = 0;

    if (input_files.empty()) { return std::vector<size_t>(); }
    
    // SoftFever: ugly fix so we can exist pa calib mode
    m_slicing.background_process().fff_print()->calib_mode() = CalibMode::Calib_None;


    // BBS
    int filaments_cnt = m_config->opt<ConfigOptionStrings>("filament_colour")->values.size();
    bool one_by_one = input_files.size() == 1 || printer_technology == ptSLA/* || filaments_cnt <= 1*/;
    if (! one_by_one) {
        for (const auto &path : input_files) {
            if (std::regex_match(path.string(), pattern_bundle)) {
                one_by_one = true;
                break;
            }
        }
    }

    bool load_model = strategy & LoadStrategy::LoadModel;
    bool load_config = strategy & LoadStrategy::LoadConfig;
    bool imperial_units = strategy & LoadStrategy::ImperialUnits;
    bool silence = strategy & LoadStrategy::Silence;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": load_model %1%, load_config %2%, input_files size %3%")%load_model %load_config %input_files.size();

    const auto loading = _L("Loading") + dots;
    ProgressDialog dlg(loading, "", 100, find_toplevel_parent(m_q), wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_APP_MODAL);
    wxBusyCursor busy;

    auto *new_model = (!load_model || one_by_one) ? nullptr : new Slic3r::Model();
    std::vector<size_t> obj_idxs;

    std::string  designer_model_id;
    std::string  designer_country_code;

    int answer_convert_from_meters          = wxOK_DEFAULT;
    int answer_convert_from_imperial_units  = wxOK_DEFAULT;
    int tolal_model_count                   = 0;

    int progress_percent = 0;
    int total_files = input_files.size();
    const int stage_percent[IMPORT_STAGE_MAX+1] = {
            5,      // IMPORT_STAGE_RESTORE
            10,     // IMPORT_STAGE_OPEN
            30,     // IMPORT_STAGE_READ_FILES
            50,     // IMPORT_STAGE_EXTRACT
            60,     // IMPORT_STAGE_LOADING_OBJECTS
            70,     // IMPORT_STAGE_LOADING_PLATES
            80,     // IMPORT_STAGE_FINISH
            85,     // IMPORT_STAGE_ADD_INSTANCE
            90,      // IMPORT_STAGE_UPDATE_GCODE
            92,     // IMPORT_STAGE_CHECK_MODE_GCODE
            95,     // UPDATE_GCODE_RESULT
            98,     // IMPORT_LOAD_CONFIG
            99,     // IMPORT_LOAD_MODEL_OBJECTS
            100
     };
    const int step_percent[LOAD_STEP_STAGE_NUM+1] = {
            5,     // LOAD_STEP_STAGE_READ_FILE
            30,     // LOAD_STEP_STAGE_GET_SOLID
            60,     // LOAD_STEP_STAGE_GET_MESH
            100
     };

    const float INPUT_FILES_RATIO            = 0.7;
    const float INIT_MODEL_RATIO             = 0.75;
    const float CENTER_AROUND_ORIGIN_RATIO   = 0.8;
    const float LOAD_MODEL_RATIO             = 0.9;
    bool hasRawGeometryImport = false;

    for (size_t i = 0; i < input_files.size(); ++i) {
        int file_percent = 0;

#ifdef _WIN32
        auto path = input_files[i];
        // On Windows, we swap slashes to back slashes, see GH #6803 as read_from_file() does not understand slashes on Windows thus it assignes full path to names of loaded objects.
        path.make_preferred();
#else  // _WIN32
       // Don't make a copy on Posix. Slash is a path separator, back slashes are not accepted as a substitute.
        const auto &path = input_files[i];
#endif // _WIN32
        const auto filename         = path.filename();
        int  progress_percent = static_cast<int>(100.0f * static_cast<float>(i) / static_cast<float>(input_files.size()));
        const auto real_filename    = (strategy & LoadStrategy::Restore) ? input_files[++i].filename() : filename;
        const auto dlg_info         = _L("Loading file") + ": " + from_path(real_filename);
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": load file %1%") % filename;
        dlg_cont = dlg.Update(progress_percent, dlg_info);
        if (!dlg_cont) return empty_result;

        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        // const bool type_zip_amf = !type_3mf && std::regex_match(path.string(), pattern_zip_amf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
        // const bool type_prusa   = std::regex_match(path.string(), pattern_prusa);
        const bool shouldInitializeAssemblyPosition = boost::algorithm::iends_with(path.string(), ".stl") ||
                                                      boost::algorithm::iends_with(path.string(), ".obj") ||
                                                      boost::algorithm::iends_with(path.string(), ".glb") ||
                                                      boost::algorithm::iends_with(path.string(), ".gltf") ||
                                                      boost::algorithm::iends_with(path.string(), ".fbx");
        hasRawGeometryImport = hasRawGeometryImport || shouldInitializeAssemblyPosition;

        Slic3r::Model model;
        // BBS: add auxiliary files related logic
        bool load_aux = strategy & LoadStrategy::LoadAuxiliary, load_old_project = false;
        if (load_model && load_config && type_3mf) {
            load_aux = true;
            strategy = strategy | LoadStrategy::LoadAuxiliary;
        }
        if (load_config) strategy = strategy | LoadStrategy::CheckVersion;
        bool is_project_file = false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": is_project_file %1%, type_3mf %2%") % is_project_file % type_3mf;
        try {
            if (type_3mf) {
                DynamicPrintConfig config;
                Semver             file_version;
                {
                    DynamicPrintConfig config_loaded;

                    // BBS: add part plate related logic
                    PlateDataPtrs             plate_data;
                    En3mfType                 en_3mf_file_type = En3mfType::From_BBS;
                    ConfigSubstitutionContext config_substitutions{ForwardCompatibilitySubstitutionRule::Enable};
                    std::vector<Preset *>     project_presets;
                    // BBS: backup & restore
                    m_refresh->set_skip_thumbnail_invalid(true);
                    model = Slic3r::Model::read_from_archive(path.string(), &config_loaded, &config_substitutions, en_3mf_file_type, strategy, &plate_data, &project_presets,
                                                             &file_version,
                                                             [this, &dlg, real_filename, &progress_percent, &file_percent, stage_percent, INPUT_FILES_RATIO, total_files, i,
                                                              &is_user_cancel](int import_stage, int current, int total, bool &cancel) {
                                                                 bool     cont = true;
                                                                 float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)stage_percent[import_stage] + (float)current * (float)(stage_percent[import_stage + 1] - stage_percent[import_stage]) /(float) total) / (float)total_files;
                                                                 BOOST_LOG_TRIVIAL(trace) << "load_3mf_file: percent(float)=" << percent_float << ", stage = " << import_stage << ", curr = " << current << ", total = " << total;
                                                                 progress_percent = (int)percent_float;
                                                                 wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                                                 cont          = dlg.Update(progress_percent, msg);
                                                                 cancel        = !cont;
                                                                 if (cancel)
                                                                     is_user_cancel = cancel;
                                                             });
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                            << boost::format(", plate_data.size %1%, project_preset.size %2%, is_bbs_3mf %3%, file_version %4% \n") % plate_data.size() %
                                                   project_presets.size() % (en_3mf_file_type == En3mfType::From_BBS) % file_version.to_string();

                    m_q->handle_newer_3mf_schema(config_loaded);

                    auto imported_string_count = [&config_loaded](const char *key) -> size_t {
                        if (const auto *opt = config_loaded.option<ConfigOptionStrings>(key))
                            return opt->values.size();
                        return 0;
                    };
                    auto imported_float_count = [&config_loaded](const char *key) -> size_t {
                        if (const auto *opt = config_loaded.option<ConfigOptionFloats>(key))
                            return opt->values.size();
                        return 0;
                    };

                    std::vector<std::string> imported_filament_colors;
                    size_t imported_physical_filaments = 0;
                    if (const auto *filament_colors_opt = config_loaded.option<ConfigOptionStrings>("filament_colour")) {
                        imported_filament_colors = filament_colors_opt->values;
                        imported_physical_filaments = imported_filament_colors.size();
                    }
                    if (imported_physical_filaments == 0)
                        imported_physical_filaments = imported_string_count("filament_settings_id");
                    if (imported_physical_filaments == 0)
                        imported_physical_filaments = imported_string_count("filament_ids");
                    if (imported_physical_filaments == 0)
                        imported_physical_filaments = imported_string_count("default_filament_colour");
                    if (imported_physical_filaments == 0)
                        imported_physical_filaments = imported_float_count("nozzle_diameter");

                    // 1. add extruder for prusa model if the number of existing extruders is not enough
                    // 2. add extruder for BBS or Other model if only import geometry
                    if (en_3mf_file_type == En3mfType::From_Prusa || (load_model && !load_config)) {
                        std::set<int> extruderIds;
                        for (ModelObject *o : model.objects) {
                            if (o->config.option("extruder")) extruderIds.insert(o->config.extruder());
                            for (auto volume : o->volumes) {
                                if (volume->config.option("extruder")) extruderIds.insert(volume->config.extruder());
                                for (int extruder : volume->get_extruders()) { extruderIds.insert(extruder); }
                            }
                        }
                        int size = extruderIds.size() == 0 ? 0 : *(extruderIds.rbegin());
                        const bool geometry_only_project_import = load_model && !load_config && imported_physical_filaments > 0;
                        const size_t desired_physical_filaments = geometry_only_project_import ?
                            std::min({imported_physical_filaments, size_t(MAXIMUM_EXTRUDER_NUMBER), size_t(MAXIMUM_FILAMENT_NUMBER)}) : 0;
                        BOOST_LOG_TRIVIAL(info) << "3MF geometry import filament detection"
                                                << " imported_physical=" << imported_physical_filaments
                                                << " imported_colors=" << imported_filament_colors.size()
                                                << " config_filament_settings_id=" << imported_string_count("filament_settings_id")
                                                << " config_filament_ids=" << imported_string_count("filament_ids")
                                                << " config_default_filament_colour=" << imported_string_count("default_filament_colour")
                                                << " config_nozzle_diameter=" << imported_float_count("nozzle_diameter")
                                                << " model_max_extruder=" << size
                                                << " geometry_only_project_import=" << (geometry_only_project_import ? 1 : 0);
                        if (geometry_only_project_import)
                            size = int(desired_physical_filaments);

                        PresetBundle *preset_bundle = m_q->preset_bundle();
                        if (geometry_only_project_import && preset_bundle != nullptr) {
                            const size_t current_num_filaments = preset_bundle->filament_presets.size();
                            const bool current_project_empty = m_model->objects.empty();
                            if (current_project_empty) {
                                static const t_config_option_keys imported_project_option_keys = {
                                    "filament_colour",
                                    "filament_multi_colors",
                                    "filament_colour_mode",
                                    "mixed_filament_definitions",
                                    "mixed_filament_gradient_mode",
                                    "mixed_filament_height_lower_bound",
                                    "mixed_filament_height_upper_bound",
                                    "mixed_filament_advanced_dithering",
                                    "mixed_filament_pointillism_pixel_size",
                                    "mixed_filament_pointillism_line_gap",
                                    "mixed_filament_component_bias_enabled",
                                    "mixed_filament_surface_indentation"
                                };
                                preset_bundle->project_config.apply_only(config_loaded, imported_project_option_keys, true);
                                if (current_num_filaments != desired_physical_filaments) {
                                    m_q->confirm_auto_generated_gradients(desired_physical_filaments);
                                    preset_bundle->set_num_filaments(unsigned(desired_physical_filaments));
                                } else
                                    preset_bundle->update_multi_material_filament_presets();
                                BOOST_LOG_TRIVIAL(info) << "3MF geometry import applied imported project config"
                                                        << " current_num_filaments=" << current_num_filaments
                                                        << " desired_physical_filaments=" << desired_physical_filaments
                                                        << " mixed_enabled=" << preset_bundle->mixed_filaments.enabled_count();
                                m_q->on_filaments_change(desired_physical_filaments);
                            } else if (current_num_filaments < desired_physical_filaments) {
                                std::vector<std::string> new_colors;
                                if (imported_filament_colors.size() > current_num_filaments) {
                                    new_colors.assign(imported_filament_colors.begin() + current_num_filaments,
                                                      imported_filament_colors.begin() + desired_physical_filaments);
                                }
                                m_q->confirm_auto_generated_gradients(desired_physical_filaments);
                                preset_bundle->set_num_filaments(unsigned(desired_physical_filaments), new_colors);
                                m_q->on_filaments_change(desired_physical_filaments);
                            }
                        }

                        int filament_size = (int)m_layout->sidebar->combos_filament().size();
                        const int filament_limit = std::min({size, int(MAXIMUM_EXTRUDER_NUMBER), int(MAXIMUM_FILAMENT_NUMBER)});
                        while (filament_size < filament_limit) {
                            int         filament_count = filament_size + 1;
                            m_q->confirm_auto_generated_gradients(filament_count);
                            wxColour    new_col        = m_q->get_next_color_for_filament();
                            std::string new_color      = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
                            m_q->preset_bundle()->set_num_filaments(filament_count, new_color);
                            m_q->on_filaments_change(filament_count);
                            ++filament_size;
                        }
                        m_q->get_tab(Preset::TYPE_PRINT)->update();
                    }

                    std::string import_project_action = m_q->app_config()->get("import_project_action");
                    int load_type = 0;   // int 桥接（LoadType：0 Unknown/1 OpenProject/2 LoadGeometry/3 LoadConfig）
                    if (import_project_action.empty())
                        load_type = 0 /*Unknown*/;
                    else
                        load_type  = std::stoi(import_project_action);

                    Semver app_version = *(Semver::parse(Snapmaker_VERSION));
                    if (en_3mf_file_type == En3mfType::From_Prusa) {
                        // do not reset the model config
                        load_config = false;
                        if(load_type != 2 /*LoadGeometry*/)
                            show_info(m_q, _L("The 3mf is not supported by Snapmaker Orca, load geometry data only."), _L("Load 3mf"));
                    }
                    // else if (load_config && (file_version.maj() != app_version.maj())) {
                    //     // version mismatch, only load geometries
                    //     load_config = false;
                    //     if (!load_model) {
                    //         // only load config case, return directly
                    //         show_info(m_q, _L("The Config cannot be loaded."), _L("Load 3mf"));
                    //         m_refresh->set_skip_thumbnail_invalid(false);
                    //         return empty_result;
                    //     }
                    //     load_old_project = true;
                    //     // select view to 3D
                    //     m_layout->select_view_3D("3D");
                    //     // select plate 0 as default
                    //     m_slicing.select_plate(0, false);
                    //     if (load_type != LoadType::LoadGeometry) {
                    //         if (en_3mf_file_type == En3mfType::From_BBS)
                    //             show_info(m_q, _L("The 3mf is generated by old Snapmaker Orca, load geometry data only."), _L("Load 3mf"));
                    //         else
                    //             show_info(m_q, _L("The 3mf is not supported by Snapmaker_Orca, load geometry data only."), _L("Load 3mf"));
                    //     }
                    //     for (ModelObject *model_object : model.objects) {
                    //         model_object->config.reset();
                    //         // Is there any modifier or advanced config data?
                    //         for (ModelVolume *model_volume : model_object->volumes) model_volume->config.reset();
                    //     }
                    // }
                    // Orca: check if the project is created with Snapmaker Orca 2.2.0-alpha and use the sparse infill rotation template for non-safe infill patterns
                    else if (false) {
                        if (!config_loaded.opt_string("sparse_infill_rotate_template").empty()) {
                            const auto _sparse_infill_pattern =
                                config_loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value;
                            bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                                     _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                                     _sparse_infill_pattern == ipLockedZag;
                            if (!is_safe_to_rotate) {
                                wxString msg_text = _(
                                    L("This project was created with an Snapmaker Orca and uses "
                                      "infill rotation template settings that may not work properly with your current infill pattern. "
                                      "This could result in weak support or print quality issues."));
                                msg_text += "\n\n" +
                                            _(L("Would you like Snapmaker Orca to automatically fix this by clearing the rotation template settings?"));
                                MessageDialog dialog(m_q, msg_text, "", wxICON_WARNING | wxYES | wxNO);
                                dialog.SetButtonLabel(wxID_YES, _L("Yes"));
                                dialog.SetButtonLabel(wxID_NO, _L("No"));
                                if (dialog.ShowModal() == wxID_YES) {
                                    config_loaded.opt_string("sparse_infill_rotate_template") = "";
                                }
                            }
                        }

                    } else if (false) {
                        if (config_substitutions.unrecogized_keys.size() > 0) {
                            wxString text  = wxString::Format(_L("The 3mf's version %s is newer than %s's version %s, found following unrecognized keys:"),
                                                             file_version.to_string(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string());
                            text += "\n";
                            bool     first = true;
                            // std::string context = into_u8(text);
                            wxString context = text;
                            // if (m_q->app_config()->get("user_mode") == "develop") {
                            //     for (auto &key : config_substitutions.unrecogized_keys) {
                            //         context += "  -";
                            //         context += key;
                            //         context += ";\n";
                            //         first = false;
                            //     }
                            // }
                            wxString append = _L("You'd better upgrade your software.\n");
                            context += "\n\n";
                            // context += into_u8(append);
                            context += append;
                            show_info(m_q, context, _L("Newer 3mf version"));
                        }
                        else {
                            //if the minor version is not matched
                            if (/*file_version.min() != app_version.min()*/ false) {
                                wxString text  = wxString::Format(_L("The 3mf's version %s is newer than %s's version %s, Suggest to upgrade your software."),
                                                 file_version.to_string(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string());
                                text += "\n";
                                show_info(m_q, text, _L("Newer 3mf version"));
                            }
                        }
                    } else if (!load_config) {
                        // reset config except color
                        for (ModelObject *model_object : model.objects) {
                            bool has_extruder = model_object->config.has("extruder");
                            int  extruder_id  = -1;
                            // save the extruder information before reset
                            if (has_extruder) { extruder_id = model_object->config.extruder(); }

                            model_object->config.reset();

                            // restore the extruder after reset
                            if (has_extruder) { model_object->config.set("extruder", extruder_id); }

                            // Is there any modifier or advanced config data?
                            for (ModelVolume *model_volume : model_object->volumes) {
                                has_extruder = model_volume->config.has("extruder");
                                if (has_extruder) { extruder_id = model_volume->config.extruder(); }

                                model_volume->config.reset();

                                if (has_extruder) { model_volume->config.set("extruder", extruder_id); }
                            }
                        }
                    }

                    // plate data
                    if (plate_data.size() > 0) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf UPDATE_GCODE_RESULT \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = dlg.Update(progress_percent, msg);
                        if (!dlg_cont) {
                            m_refresh->set_skip_thumbnail_invalid(false);
                            return empty_result;
                        }

                        Semver old_version(1, 5, 9);
                        if ((en_3mf_file_type == En3mfType::From_BBS) && (file_version < old_version) && load_model && load_config && !config_loaded.empty()) {
                            translate_old = true;
                            m_partplate_list.get_plate_size(current_width, current_depth, current_height);
                        }

                        if (load_config) {
                            if (translate_old) {
                                //set the size back
                                m_partplate_list.reset_size(current_width + Bed3D::Axes::DefaultTipRadius, current_depth + Bed3D::Axes::DefaultTipRadius, current_height, false);
                            }
                            m_partplate_list.load_from_3mf_structure(plate_data);
                            m_partplate_list.update_slice_context_to_current_plate(m_slicing.background_process());
                            m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
                            release_PlateData_list(plate_data);
                            m_layout->sidebar->obj_list()->reload_all_plates();
                        } else {
                            m_partplate_list.reload_all_objects();
                        }
                    }

                    // BBS:: project embedded presets
                    if ((project_presets.size() > 0) && load_config) {
                        // load project embedded presets
                        PresetsConfigSubstitutions preset_substitutions;
                        PresetBundle &             preset_bundle = *m_q->preset_bundle();
                        preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                        if (!preset_substitutions.empty()) show_substitutions_info(preset_substitutions);
                    }
                    if (project_presets.size() > 0) {
                        for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                        project_presets.clear();
                    }

                    if (load_config && !config_loaded.empty()) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf IMPORT_LOAD_CONFIG \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = dlg.Update(progress_percent, msg);
                        if (!dlg_cont) {
                            m_refresh->set_skip_thumbnail_invalid(false);
                            return empty_result;
                        }

                        // Based on the printer technology field found in the loaded config, select the base for the config,
                        PrinterTechnology printer_technology = Preset::printer_technology(config_loaded);

                        config.apply(static_cast<const ConfigBase &>(FullPrintConfig::defaults()));
                        // and place the loaded config over the base.
                        config += std::move(config_loaded);
                        std::map<std::string, std::string> validity = config.validate();
                        if (!validity.empty()) {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("Param values in 3mf error: ");
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it)
                                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("%1%: %2%")%it->first %it->second;
                            //
                            NotificationManager *notify_manager = m_hub.nm();
                            std::string error_message = L("Invalid values found in the 3mf:");
                            error_message += "\n";
                            bool any_entry = false;
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it) {
                                // filament_flow_ratio == 0 is handled by the dedicated
                                // flow_ratio_zero pre-slice banner (which also names the
                                // specific offending filament slot). Skip it here to avoid
                                // a redundant, less informative notification.
                                //
                                // KNOWN LIMITATION: flow_ratio_zero only fires for slots
                                // actually used by an object on the current plate. If the
                                // loaded 3MF has flow_ratio == 0 on a slot that no object
                                // references, this skip suppresses the generic 3mf warning
                                // AND flow_ratio_zero does not fire either -> the offender
                                // is invisible at load time. It will surface only when the
                                // user later assigns an object to that slot. Accepted
                                // trade-off: silent on unused slots is better than two
                                // competing notifications on used slots.
                                if (it->first == "filament_flow_ratio")
                                    continue;
                                error_message += "-" + it->first + ": " + it->second + "\n";
                                any_entry = true;
                            }
                            if (any_entry) {
                                error_message += "\n";
                                error_message += L("Please correct them in the param tabs");
                                notify_manager->bbl_show_3mf_warn_notification(error_message);
                            }
                        }
                    }
                    if (!config_substitutions.empty()) show_substitutions_info(config_substitutions.substitutions, filename.string());

                    // BBS
                    if (load_model && !load_config) {
                        ;
                    }
                    else {
                        m_model->plates_custom_gcodes = model.plates_custom_gcodes;
                        m_model->design_info = model.design_info;
                        m_model->model_info = model.model_info;
                    }
                }

                if (load_config) {
                    if (!config.empty()) {
                        Preset::normalize(config);
                        PresetBundle *preset_bundle = m_q->preset_bundle();

                        auto choise = m_q->app_config()->get("no_warn_when_modified_gcodes");
                        if (choise.empty() || choise != "true") {
                            // BBS: first validate the printer
                            // validate the system profiles
                            std::set<std::string> modified_gcodes;
                            int validated = preset_bundle->validate_presets(filename.string(), config, modified_gcodes);
                            if (validated == VALIDATE_PRESETS_MODIFIED_GCODES) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(m_q, _L("The 3mf has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                MessageDialog dlg(m_q, _L("The 3mf has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    m_q->app_config()->set("no_warn_when_modified_gcodes", "true");
                            }
                            else if ((validated == VALIDATE_PRESETS_PRINTER_NOT_FOUND) || (validated == VALIDATE_PRESETS_FILAMENTS_NOT_FOUND)) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(m_q, _L("The 3mf has the following customized filament or printer presets:") + warning_message + _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                MessageDialog dlg(m_q, _L("The 3mf has the following customized filament or printer presets:") + from_u8(warning_message)+ _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    m_q->app_config()->set("no_warn_when_modified_gcodes", "true");
                            }
                        }

                        //always load config
                        {
                            // BBS: save the wipe tower pos in file here, will be used later
                            ConfigOptionFloats* wipe_tower_x_opt = config.opt<ConfigOptionFloats>("wipe_tower_x");
                            ConfigOptionFloats* wipe_tower_y_opt = config.opt<ConfigOptionFloats>("wipe_tower_y");
                            std::optional<ConfigOptionFloats>file_wipe_tower_x;
                            std::optional<ConfigOptionFloats>file_wipe_tower_y;
                            if (wipe_tower_x_opt)
                                file_wipe_tower_x = *wipe_tower_x_opt;
                            if (wipe_tower_y_opt)
                                file_wipe_tower_y = *wipe_tower_y_opt;

                            preset_bundle->load_config_model(filename.string(), std::move(config), file_version);

                            ConfigOption* bed_type_opt = preset_bundle->project_config.option("curr_bed_type");
                            if (bed_type_opt != nullptr) {
                                BedType bed_type = (BedType)bed_type_opt->getInt();
                                // update app config for bed type
                                bool is_bbl_preset = preset_bundle->is_bbl_vendor();
                                if (is_bbl_preset) {
                                    AppConfig* app_config = m_q->app_config();
                                    if (app_config)
                                        app_config->set("curr_bed_type", std::to_string(int(bed_type)));
                                }
                                m_q->on_bed_type_change(bed_type);
                            }

                            // BBS: moved this logic to presetcollection
                            //{
                            //    // After loading of the presets from project, check if they are visible.
                            //    // Set them to visible if they are not.

                            //    auto update_selected_preset_visibility = [](PresetCollection& presets, std::vector<std::string>& names) {
                            //        if (!presets.get_selected_preset().is_visible) {
                            //            assert(presets.get_selected_preset().name == presets.get_edited_preset().name);
                            //            presets.get_selected_preset().is_visible = true;
                            //            presets.get_edited_preset().is_visible = true;
                            //            names.emplace_back(presets.get_selected_preset().name);
                            //        }
                            //    };

                            //    std::vector<std::string> names;
                            //    if (printer_technology == ptFFF) {
                            //        update_selected_preset_visibility(preset_bundle->prints, names);
                            //        for (const std::string& filament : preset_bundle->filament_presets) {
                            //            Preset* preset = preset_bundle->filaments.find_preset(filament);
                            //            if (preset && !preset->is_visible) {
                            //                preset->is_visible = true;
                            //                names.emplace_back(preset->name);
                            //                if (preset->name == preset_bundle->filaments.get_edited_preset().name)
                            //                    preset_bundle->filaments.get_selected_preset().is_visible = true;
                            //            }
                            //        }
                            //    }
                            //    else {
                            //        update_selected_preset_visibility(preset_bundle->sla_prints, names);
                            //        update_selected_preset_visibility(preset_bundle->sla_materials, names);
                            //    }
                            //    update_selected_preset_visibility(preset_bundle->printers, names);

                            //    preset_bundle->update_compatible(PresetSelectCompatibleType::Never);

                            //    // show notification about temporarily installed presets
                            //    if (!names.empty()) {
                            //        std::string notif_text = into_u8(_L_PLURAL("The preset below was temporarily installed on the active instance of PrusaSlicer",
                            //                                                   "The presets below were temporarily installed on the active instance of PrusaSlicer",
                            //                                                   names.size())) + ":";
                            //        for (std::string& name : names)
                            //            notif_text += "\n - " + name;
                            //        m_hub.nm()->push_notification(NotificationType::CustomNotification,
                            //            NotificationManager::NotificationLevel::PrintInfoNotificationLevel, notif_text);
                            //    }
                            //}

                            // BBS
                            // if (printer_technology == ptFFF)
                            //    CustomGCode::update_custom_gcode_per_print_z_from_config(model.custom_gcode_per_print_z, &preset_bundle->project_config);

                            // For exporting from the amf/3mf we shouldn't check printer_presets for the containing information about "Print Host upload"
                            // BBS: add preset combo box re-active logic
                            // currently found only needs re-active here
                            m_q->load_current_presets(false, false);
                            // Some preset-tab refresh paths rebuild printer/filament UI from the
                            // active presets but do not preserve the mixed manager instance.
                            // Rebuild it explicitly from project_config before clamping object IDs.
                            preset_bundle->update_multi_material_filament_presets();
                            // Update filament colors for the MM-printer profile in the full config
                            // to avoid black (default) colors for Extruders in the ObjectList,
                            // when for extruder colors are used filament colors
                            m_q->on_filaments_change(preset_bundle->filament_presets.size());
                            is_project_file = true;

                            //BBS: rewrite wipe tower pos stored in 3mf file , the code above should be seriously reconsidered
                            {
                                DynamicConfig& proj_cfg = m_q->preset_bundle()->project_config;
                                ConfigOptionFloats* wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
                                ConfigOptionFloats* wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
                                if (file_wipe_tower_x)
                                    *wipe_tower_x = *file_wipe_tower_x;
                                if (file_wipe_tower_y)
                                    *wipe_tower_y = *file_wipe_tower_y;
                            }
                        }
                    }
                    if (!silence) m_q->app_config()->update_config_dir(path.parent_path().string());
                }
            } else {
                // BBS: add plate data related logic
                PlateDataPtrs plate_data;
                // BBS: project embedded settings
                std::vector<Preset *> project_presets;
                bool                  is_xxx;
                Semver                file_version;
                
                //ObjImportColorFn obj_color_fun=nullptr;
                auto obj_color_fun = [this, &path](std::vector<RGBA> &input_colors, bool is_single_color, std::vector<unsigned char> &filament_ids,
                                                   unsigned char &first_extruder_id) {
                    if (!boost::iends_with(path.string(), ".obj")) { return; }
                    const std::vector<std::string> extruder_colours = m_q->get_extruder_colors_from_plater_config();
                    ObjColorDialog                 color_dlg(nullptr, input_colors, is_single_color, extruder_colours, filament_ids, first_extruder_id);
                    if (color_dlg.ShowModal() != wxID_OK) { 
                        filament_ids.clear();
                    }
                };
                if (boost::iends_with(path.string(), ".stp") ||
                    boost::iends_with(path.string(), ".step")) {
                        double linear = string_to_double_decimal_point(m_q->app_config()->get("linear_defletion"));
                        if (linear <= 0) linear = 0.003;
                        double angle = string_to_double_decimal_point(m_q->app_config()->get("angle_defletion"));
                        if (angle <= 0) angle = 0.5;
                        bool split_compound = m_q->app_config()->get_bool("is_split_compound");
                        model = Slic3r::Model:: read_from_step(path.string(), strategy,
                        [this, &dlg, real_filename, &progress_percent, &file_percent, step_percent, INPUT_FILES_RATIO, total_files, i](int load_stage, int current, int total, bool &cancel)
                        {
                                bool     cont = true;
                                float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)step_percent[load_stage] + (float)current * (float)(step_percent[load_stage + 1] - step_percent[load_stage]) / (float)total) / (float)total_files;
                                BOOST_LOG_TRIVIAL(trace) << "load_step_file: percent(float)=" << percent_float << ", stage = " << load_stage << ", curr = " << current << ", total = " << total;
                                progress_percent = (int)percent_float;
                                wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                cont          = dlg.Update(progress_percent, msg);
                                cancel        = !cont;
                        },
                        [this](int isUtf8StepFile) {
                            if (!isUtf8StepFile) {
                                const auto no_warn = m_q->app_config()->get_bool("step_not_utf8_no_warn");
                                if (!no_warn) {
                                    MessageDialog dlg(nullptr, _L("Name of components inside step file is not UTF8 format!") + "\n\n" + _L("The name may show garbage characters!"),
                                                      wxString(SLIC3R_APP_FULL_NAME " - ") + _L("Attention!"), wxOK | wxICON_INFORMATION);
                                    dlg.show_dsa_button(_L("Remember my choice."));
                                    dlg.ShowModal();
                                    if (dlg.get_checkbox_state()) {
                                        m_q->app_config()->set_bool("step_not_utf8_no_warn", true);
                                    }
                                }
                            }
                        },
                        [this, &path, &is_user_cancel, &linear, &angle, &split_compound](Slic3r::Step& file, double& linear_value, double& angle_value, bool& is_split)-> int {
                            if (m_q->app_config()->get_bool("enable_step_mesh_setting")) {
                                StepMeshDialog mesh_dlg(nullptr, file, linear, angle);
                                if (mesh_dlg.ShowModal() == wxID_OK) {
                                    linear_value = mesh_dlg.get_linear_defletion();
                                    angle_value  = mesh_dlg.get_angle_defletion();
                                    is_split     = mesh_dlg.get_split_compound_value();
                                    return 1;
                                }
                            }else {
                                linear_value = linear;
                                angle_value = angle;
                                is_split = split_compound;
                                return 1;
                            }
                            is_user_cancel = true;
                            return -1;
                        }, linear, angle, split_compound);
                }else {
                    model = Slic3r::Model:: read_from_file(
                    path.string(), nullptr, nullptr, strategy, &plate_data, &project_presets, &is_xxx, &file_version, nullptr,
                    [this, &dlg, real_filename, &progress_percent, &file_percent, INPUT_FILES_RATIO, total_files, i, &designer_model_id, &designer_country_code](int current, int total, bool &cancel, std::string &mode_id, std::string &code)
                    {
                            designer_model_id = mode_id;
                            designer_country_code = code;

                            bool     cont = true;
                            float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * 100.0f * ((float)current / (float)total) / (float)total_files;
                            BOOST_LOG_TRIVIAL(trace) << "load_stl_file: percent(float)=" << percent_float << ", curr = " << current << ", total = " << total;
                            progress_percent = (int)percent_float;
                            wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                            cont          = dlg.Update(progress_percent, msg);
                            cancel        = !cont;
                    },
                    nullptr, 0, obj_color_fun);
                }

                if (designer_model_id.empty() && boost::algorithm::iends_with(path.string(), ".stl")) {
                    m_q->read_binary_stl(path.string(), designer_model_id, designer_country_code);
                }

                if (type_any_amf && is_xxx) imperial_units = true;

                for (auto obj : model.objects) {
                    if (obj->name.empty()) {
                        obj->name = fs::path(obj->input_file).filename().string();
                    }
                    obj->rotate(Geometry::deg2rad(m_config->opt_float("preferred_orientation")), Axis::Z);
                }

                if (plate_data.size() > 0) {
                    m_partplate_list.load_from_3mf_structure(plate_data);
                    m_partplate_list.update_slice_context_to_current_plate(m_slicing.background_process());
                    m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
                    release_PlateData_list(plate_data);
                    m_layout->sidebar->obj_list()->reload_all_plates();
                }

                // BBS:: project embedded presets
                if (project_presets.size() > 0) {
                    // load project embedded presets
                    PresetsConfigSubstitutions preset_substitutions;
                    PresetBundle &             preset_bundle = *m_q->preset_bundle();
                    preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                    if (!preset_substitutions.empty()) show_substitutions_info(preset_substitutions);

                    for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                    project_presets.clear();
                }
            }
        } catch (const ConfigurationError &e) {
            std::string message = GUI::format(_L("Failed loading file \"%1%\". An invalid configuration was found."), filename.string()) + "\n\n" + e.what();
            GUI::show_error(m_q, message);
            continue;
        } catch (const std::exception &e) {
            if (!is_user_cancel)
                GUI::show_error(m_q, e.what());
            continue;
        }

        progress_percent = 100.0f * (float)i / (float)total_files + INIT_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            m_refresh->set_skip_thumbnail_invalid(false);
            return empty_result;
        }

        if (load_model) {
            // The model should now be initialized
            auto convert_from_imperial_units = [](Model &model, bool only_small_volumes) { model.convert_from_imperial_units(only_small_volumes); };

            // BBS: add load_old_project logic
            if ((!is_project_file) && (!load_old_project)) {
                // if (!is_project_file) {
                if (int deleted_objects = model.removed_objects_with_zero_volume(); deleted_objects > 0) {
                    MessageDialog(m_q, _L("Objects with zero volume removed"), _L("The volume of the object is zero"), wxICON_INFORMATION | wxOK).ShowModal();
                }
                if (imperial_units)
                    // Convert even if the object is big.
                    convert_from_imperial_units(model, false);
                else if (model.looks_like_saved_in_meters()) {
                    // BBS do not handle look like in meters
                    MessageDialog dlg(m_q,
                                      format_wxstr(_L("The object from file %s is too small, and maybe in meters or inches.\n Do you want to scale to millimeters?"),
                                                   from_path(filename)),
                                      _L("Object too small"), wxICON_QUESTION | wxYES_NO);
                    int           answer = dlg.ShowModal();
                    if (answer == wxID_YES) model.convert_from_meters(true);
                } else if (model.looks_like_imperial_units()) {
                    // BBS do not handle look like in meters
                    MessageDialog dlg(m_q,
                                      format_wxstr(_L("The object from file %s is too small, and maybe in meters or inches.\n Do you want to scale to millimeters?"),
                                                   from_path(filename)),
                                      _L("Object too small"), wxICON_QUESTION | wxYES_NO);
                    int           answer = dlg.ShowModal();
                    if (answer == wxID_YES) convert_from_imperial_units(model, true);
                }
                // else if (model.looks_like_imperial_units()) {
                // BBS do not handle look like in imperial
                // auto convert_model_if = [convert_from_imperial_units](Model& model, bool condition) {
                //    if (condition)
                //        //FIXME up-scale only the small parts?
                //        convert_from_imperial_units(model, true);
                //};
                // if (answer_convert_from_imperial_units == wxOK_DEFAULT) {
                //    RichMessageDialog dlg(q, format_wxstr(_L_PLURAL(
                //        "The dimensions of the object from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of the object?",
                //        "The dimensions of some objects from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of these objects?", model.objects.size()), from_path(filename))
                //        + "\n", _L("The object is too small"), wxICON_QUESTION | wxYES_NO);
                //    dlg.ShowCheckBox(_L("Apply to all the remaining small objects being loaded."));
                //    int answer = dlg.ShowModal();
                //    if (dlg.IsCheckBoxChecked())
                //        answer_convert_from_imperial_units = answer;
                //    else
                //        convert_model_if(model, answer == wxID_YES);
                //}
                // convert_model_if(model, answer_convert_from_imperial_units == wxID_YES);
            }

             if (!is_project_file && model.looks_like_multipart_object()) {
               MessageDialog msg_dlg(m_q, _L(
                    "This file contains several objects positioned at multiple heights.\n"
                    "Instead of considering them as multiple objects, should \n"
                    "the file be loaded as a single object having multiple parts?") + "\n",
                    _L("Multi-part object detected"), wxICON_WARNING | wxYES | wxNO);
                if (msg_dlg.ShowModal() == wxID_YES) {
                    model.convert_multipart_object(filaments_cnt);
                }
            }
        }
        // else if ((wxGetApp().get_mode() == comSimple) && (type_3mf || type_any_amf) && model_has_advanced_features(model)) {
        //    MessageDialog msg_dlg(m_q, _L("This file cannot be loaded in a simple mode. Do you want to switch to an advanced mode?")+"\n",
        //        _L("Detected advanced data"), wxICON_WARNING | wxYES | wxNO);
        //    if (msg_dlg.ShowModal() == wxID_YES) {
        //        Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
        //        m_layout->view3D->set_as_dirty();
        //    }
        //    else
        //        return obj_idxs;
        //}

        progress_percent = 100.0f * (float)i / (float)total_files + CENTER_AROUND_ORIGIN_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            m_refresh->set_skip_thumbnail_invalid(false);
            return empty_result;
        }

        int model_idx = 0;
        for (ModelObject *model_object : model.objects) {
            if (!type_3mf && !type_any_amf) model_object->center_around_origin(false);

            // BBS
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_LOAD_MODEL_OBJECTS \n");
            wxString msg = wxString::Format("Loading file: %s", from_path(real_filename));
            model_idx++;
            dlg_cont = dlg.Update(progress_percent, msg);
            if (!dlg_cont) {
                m_refresh->set_skip_thumbnail_invalid(false);
                return empty_result;
            }

            if (!model_object->instances.empty())
                model_object->ensure_on_bed(is_project_file);
        }

        tolal_model_count += model_idx;

        progress_percent = 100.0f * (float)i / (float)total_files + LOAD_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            m_refresh->set_skip_thumbnail_invalid(false);
            return empty_result;
        }

        if (one_by_one) {
            // BBS: add load_old_project logic
            if (type_3mf && !is_project_file && !load_old_project)
                // if (type_3mf && !is_project_file)
                model.center_instances_around_point(m_layout->bed.build_volume().bed_center());
            // BBS: add auxiliary files logic
            // BBS: backup & restore
            if (load_aux) {
                m_model->load_from(model);
                m_q->load_auxiliary_files();
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before load_model_objects, count %1%")%model.objects.size();
            auto loaded_idxs = load_model_objects(model.objects, is_project_file);
            obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
            if (shouldInitializeAssemblyPosition)
            {
                ModelObjectPtrs loadedObjects;
                loadedObjects.reserve(loaded_idxs.size());
                for (const size_t objectIndex : loaded_idxs)
                {
                    if (objectIndex < m_model->objects.size())
                    {
                        loadedObjects.push_back(m_model->objects[objectIndex]);
                    }
                }
                m_model->InitializeAssemblyPositions(loadedObjects);
            }

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished load_model_objects");
            wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
            dlg_cont     = dlg.Update(progress_percent, msg);
            if (!dlg_cont) {
                m_refresh->set_skip_thumbnail_invalid(false);
                return empty_result;
            }
        } else {
            // This must be an .stl or .obj file, which may contain a maximum of one volume.
            for (const ModelObject *model_object : model.objects) {
                new_model->add_object(*model_object);

                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":" << __LINE__ << boost::format(", added object %1%")%model_object->name;
                wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                dlg_cont     = dlg.Update(progress_percent, msg);
                if (!dlg_cont) {
                    m_refresh->set_skip_thumbnail_invalid(false);
                    return empty_result;
                }
            }
        }
    }

    if (new_model != nullptr && new_model->objects.size() > 1) {
        //BBS do not popup this dialog

        if (ask_multi) {
            MessageDialog msg_dlg(m_q, _L("Load these files as a single object with multiple parts?\n"), _L("Object with multiple parts was detected"),
                                  wxICON_WARNING | wxYES | wxNO);
            if (msg_dlg.ShowModal() == wxID_YES) { new_model->convert_multipart_object(filaments_cnt); }
        }

        auto loaded_idxs = load_model_objects(new_model->objects);
        obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
        if (hasRawGeometryImport)
        {
            ModelObjectPtrs loadedObjects;
            loadedObjects.reserve(loaded_idxs.size());
            for (const size_t objectIndex : loaded_idxs)
            {
                if (objectIndex < m_model->objects.size())
                {
                    loadedObjects.push_back(m_model->objects[objectIndex]);
                }
            }
            m_model->InitializeAssemblyPositions(loadedObjects);
        }
    }

    if (new_model) delete new_model;

    //BBS: translate old 3mf to correct positions
    if (translate_old) {
        //translate the objects
        int plate_count = m_partplate_list.get_plate_count();
        for (int index = 1; index < plate_count; index ++) {
            PartPlate* cur_plate = (PartPlate *)m_partplate_list.get_plate(index);

            Vec3d cur_origin = cur_plate->get_origin();
            Vec3d new_origin = m_partplate_list.compute_origin_using_new_size(index, current_width, current_depth);

            cur_plate->translate_all_instance(new_origin - cur_origin);
        }
        m_layout->view3D->get_canvas3d()->remove_raycasters_for_picking(SceneRaycaster::EType::Bed);
        m_partplate_list.reset_size(current_width, current_depth, current_height, true, true);
        m_partplate_list.register_raycasters_for_picking(*m_layout->view3D->get_canvas3d());
    }

    //BBS: add gcode loading logic in the end
    set_using_exported_file(false);
    m_refresh->set_skip_thumbnail_invalid(false);
    if (load_model && load_config) {
        if (m_model->objects.empty()) {
            m_partplate_list.load_gcode_files();
            PartPlate * first_plate = nullptr, *cur_plate = nullptr;
            int plate_cnt = m_partplate_list.get_plate_count();
            int index = 0, first_plate_index = 0;
            set_valid_plates_count(0);
            for (index = 0; index < plate_cnt; index ++)
            {
                cur_plate = m_partplate_list.get_plate(index);
                if (!first_plate && cur_plate->is_slice_result_valid()) {
                    first_plate = cur_plate;
                    first_plate_index = index;
                }
                if (cur_plate->is_slice_result_valid())
                    set_valid_plates_count(valid_plates_count() + 1);
            }
            if (first_plate&&first_plate->is_slice_result_valid()) {
                set_using_exported_file(true);
                //select plate 0 as default
                m_slicing.select_plate(first_plate_index, false);
                //set to 3d tab
                m_layout->select_view_3D("Preview");
                m_main_frame->select_tab(MainFrame::tpPreview);
            }
            else {
                //set to 3d tab
                m_layout->select_view_3D("3D");
                //select plate 0 as default
                m_slicing.select_plate(0, false);
            }
        }
        else {
            //set to 3d tab
            m_layout->select_view_3D("3D");
            //select plate 0 as default
            m_slicing.select_plate(0, false);
        }
    }
    else {
        //always set to 3D after loading files
        m_layout->select_view_3D("3D");
        m_main_frame->select_tab(MainFrame::tp3DEditor);
    }

    if (load_model) {
        if (!silence) m_q->app_config()->update_skein_dir(input_files[input_files.size() - 1].parent_path().make_preferred().string());
        // XXX: Plater.pm had @loaded_files, but didn't seem to fill them with the filenames...
    }

    // automatic selection of added objects
    if (!obj_idxs.empty() && m_layout->view3D != nullptr) {
        // update printable state for new volumes on canvas3D
        m_q->canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

        if (!load_config) {
            Selection& selection = m_layout->view3D->get_canvas3d()->get_selection();
            selection.clear();
            for (size_t idx : obj_idxs) {
                selection.add_object((unsigned int)idx, false);
            }
        }
        // BBS: update object list selection
        m_layout->sidebar->obj_list()->update_selections();

        if (m_layout->view3D->get_canvas3d()->get_gizmos_manager().is_enabled())
            // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
            m_layout->view3D->get_canvas3d()->update_gizmos_on_off_state();
    }

    GLGizmoSimplify::add_simplify_suggestion_notification(
        obj_idxs, m_model->objects, *m_hub.nm());

    //set designer_model_id
    m_model->stl_design_id = designer_model_id;
    m_model->stl_design_country = designer_country_code;
    //if (!designer_model_id.empty() && m_model->stl_design_id.empty() && !designer_country_code.empty()) {
    //    m_model->stl_design_id = designer_model_id;
    //    m_model->stl_design_country = designer_country_code;
    //}
    //else {
    //    m_model->stl_design_id = "";
    //    m_model->stl_design_country = "";
    //}

    if (tolal_model_count <= 0 && !using_exported_file()) {
        dlg.Hide();
        if (!is_user_cancel) {
            MessageDialog msg(m_main_frame, _L("The file does not contain any geometry data."), _L("Warning"), wxYES | wxICON_WARNING);
            if (msg.ShowModal() == wxID_YES) {}
        }
    }
    return obj_idxs;
}
 #define AUTOPLACEMENT_ON_LOAD

std::vector<size_t> ProjectSession::load_model_objects(const std::vector<ModelObject*>& model_objects, bool allow_negative_z, bool split_object)
{
    const Vec3d bed_size = Slic3r::to_3d(m_layout->bed.build_volume().bounding_volume2d().size(), 1.0) - 2.0 * Vec3d::Ones();

#ifndef AUTOPLACEMENT_ON_LOAD
    // bool need_arrange = false;
#endif /* AUTOPLACEMENT_ON_LOAD */
    bool scaled_down = false;
    std::vector<size_t> obj_idxs;
    unsigned int obj_count = m_model->objects.size();

#ifdef AUTOPLACEMENT_ON_LOAD
    ModelInstancePtrs new_instances;
#endif /* AUTOPLACEMENT_ON_LOAD */
    for (ModelObject *model_object : model_objects) {
        auto *object = m_model->add_object(*model_object);
        object->sort_volumes(true);
        std::string object_name = object->name.empty() ? fs::path(object->input_file).filename().string() : object->name;
        obj_idxs.push_back(obj_count++);

        if (model_object->instances.empty()) {
#ifdef AUTOPLACEMENT_ON_LOAD
            object->center_around_origin();
            new_instances.emplace_back(object->add_instance());
#else /* AUTOPLACEMENT_ON_LOAD */
            // if object has no defined position(s) we need to rearrange everything after loading
            // need_arrange = true;
             // add a default instance and center object around origin
            object->center_around_origin();  // also aligns object to Z = 0
            ModelInstance* instance = object->add_instance();

            //BBS calc transformation
            Geometry::Transformation t = instance->get_transformation();
            instance->set_offset(Slic3r::to_3d(m_layout->bed.build_volume().bed_center(), -object->origin_translation(2)));
#endif /* AUTOPLACEMENT_ON_LOAD */
        }

        //BBS: when the object is too large, let the user choose whether to scale it down
        for (size_t i = 0; i < object->instances.size(); ++i) {
            ModelInstance* instance = object->instances[i];
            const Vec3d size = object->instance_bounding_box(i).size();
            const Vec3d ratio = size.cwiseQuotient(bed_size);
            const double max_ratio = std::max(ratio(0), ratio(1));
            if (max_ratio > 10000) {
                MessageDialog dlg(m_q, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"), _L("Object too large"),
                                  wxICON_QUESTION | wxYES);
                int           answer = dlg.ShowModal();
                // the size of the object is too big -> this could lead to overflow when moving to clipper coordinates,
                // so scale down the mesh
                object->scale_mesh_after_creation(1. / max_ratio);
                object->origin_translation = Vec3d::Zero();
                object->center_around_origin();
                scaled_down = true;
                break;
            }
            else if (max_ratio > 10) {
                MessageDialog dlg(m_q, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"), _L("Object too large"),
                                  wxICON_QUESTION | wxYES_NO);
                int           answer = dlg.ShowModal();
                if (answer == wxID_YES) {
                    instance->set_scaling_factor(instance->get_scaling_factor() / max_ratio);
                    scaled_down = true;
                }
            }
        }

        object->ensure_on_bed(allow_negative_z);
        if (!split_object) {
            //BBS initial assemble transformation
            for (ModelObject* model_object : m_model->objects) {
                //BBS initialize assemble transformation
                for (int i = 0; i < model_object->instances.size(); i++) {
                    if (!model_object->instances[i]->is_assemble_initialized()) {
                        model_object->instances[i]->set_assemble_transformation(model_object->instances[i]->get_transformation());
                    }
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", loaded objects, begin to auto placement");
#ifdef AUTOPLACEMENT_ON_LOAD
#if 0
    // FIXME distance should be a config value /////////////////////////////////
    auto min_obj_distance = static_cast<coord_t>(6/SCALING_FACTOR);
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("printable_area");
    assert(bed_shape_opt);
    auto& bedpoints = bed_shape_opt->values;
    Polyline bed; bed.points.reserve(bedpoints.size());
    for(auto& v : bedpoints) bed.append(Point::new_scale(v(0), v(1)));

    // BBS: get wipe tower of current plate
    int cur_plate_idx = m_partplate_list.get_curr_plate_index();
    std::pair<bool, GLCanvas3D::WipeTowerInfo> wti = m_layout->view3D->get_canvas3d()->get_wipe_tower_info(cur_plate_idx);

    arr::find_new_position(model, new_instances, min_obj_distance, bed, wti);

    // it remains to move the wipe tower:
    m_layout->view3D->get_canvas3d()->arrange_wipe_tower(wti);
#else
    // BBS: find an empty cell to put the copied object
    for (auto& instance : new_instances) {
        auto offset = instance->get_offset();
        auto start_point = m_layout->bed.build_volume().bounding_volume2d().center();
        bool plate_empty = m_partplate_list.get_curr_plate()->empty();
        Vec3d displacement;
        if (plate_empty)
            displacement = {start_point(0), start_point(1), offset(2)};
        else {
            auto empty_cell = m_q->canvas3D()->get_nearest_empty_cell(Vec2d({start_point(0), start_point(1)}).cast<float>());
            displacement    = {empty_cell.x(), empty_cell.y(), offset(2)};
        }
        instance->set_offset(displacement);
    }
#endif

#endif /* AUTOPLACEMENT_ON_LOAD */

    //BBS: remove the auto scaled_down logic when load models
    //if (scaled_down) {
    //    GUI::show_info(m_q,
    //        _L("Your object appears to be too large, so it was automatically scaled down to fit your print bed."),
    //        _L("Object too large?"));
    //}

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished auto placement, before add_objects_to_list");
    m_hub.nm()->close_notification_of_type(NotificationType::UpdatedItemsInfo);

    if (obj_idxs.size() > 1) {
        std::vector<size_t> obj_idxs_1 (obj_idxs.begin(), obj_idxs.end() - 1);

        m_layout->sidebar->obj_list()->add_objects_to_list(obj_idxs_1, false);
        m_layout->sidebar->obj_list()->add_object_to_list(obj_idxs[obj_idxs.size() - 1]);
    }
    else
        m_layout->sidebar->obj_list()->add_objects_to_list(obj_idxs);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", after add_objects_to_list");
    m_refresh->update(0);   // 原 priv::update()（默认 flags=0）
    // Update InfoItems in ObjectList after update() to use of a correct value of the GLCanvas3D::is_sinking(),
    // which is updated after a m_layout->view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH) call
    for (const size_t idx : obj_idxs)
        m_layout->sidebar->obj_list()->update_info_items(idx);

    m_refresh->object_list_changed();

    m_slicing.schedule_background_process();

    return obj_idxs;
}
bool ProjectSession::load_files(const wxArrayString& filenames)
{
    const std::regex pattern_drop(".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip)", std::regex::icase);
    const std::regex pattern_gcode_drop(".*[.](gcode|g)", std::regex::icase);

    std::vector<fs::path> normal_paths;
    std::vector<fs::path> gcode_paths;

    for (const auto& filename : filenames) {
        fs::path path(into_path(filename));
        if (std::regex_match(path.string(), pattern_drop))
            normal_paths.push_back(std::move(path));
        else if (std::regex_match(path.string(), pattern_gcode_drop))
            gcode_paths.push_back(std::move(path));
        else
            continue;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": normal_paths %1%, gcode_paths %2%")%normal_paths.size() %gcode_paths.size();
    if (normal_paths.empty() && gcode_paths.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": can not find valid path, return directly");
        // Likely no supported files
        return false;
    }
    else if (normal_paths.empty()){
        //only gcode files
        if (gcode_paths.size() > 1) {
            show_info(m_q, _L("Only one G-code file can be opened at the same time."), _L("G-code loading"));
            return false;
        }
        load_gcode(from_path(gcode_paths.front()));
        return true;
    }

    if (!gcode_paths.empty()) {
        show_info(m_q, _L("G-code files cannot be loaded with models together!"), _L("G-code loading"));
        return false;
    }

    //// searches for project files
    //for (std::vector<fs::path>::const_reverse_iterator it = normal_paths.rbegin(); it != normal_paths.rend(); ++it) {
    //    std::string filename = (*it).filename().string();
    //    ////BBS: only 3mf will be treated as project file
    //    if (open_3mf_file((*it)))
    //        return true;
    //}

    //// other files
    std::string snapshot_label;
    assert(!normal_paths.empty());
    if (normal_paths.size() == 1) {
        snapshot_label = "Load File";
        snapshot_label += ": ";
        snapshot_label += encode_path(normal_paths.front().filename().string().c_str());
    } else {
        snapshot_label = "Load Files";
        snapshot_label += ": ";
        snapshot_label += encode_path(normal_paths.front().filename().string().c_str());
        for (size_t i = 1; i < normal_paths.size(); ++i) {
            snapshot_label += ", ";
            snapshot_label += encode_path(normal_paths[i].filename().string().c_str());
        }
    }

    //load_files(normal_paths, LoadStrategy::LoadModel, false);

    // BBS: check file types
    std::sort(normal_paths.begin(), normal_paths.end(), [](fs::path obj1, fs::path obj2) { return obj1.filename().string() < obj2.filename().string(); });

    auto loadfiles_type  = 0;   // int 桥接（LoadFilesType 为 cpp-local enum：0 NoFile/1 Single3MF/2 SingleOther/3 Multiple3MF/4 MultipleOther/5 Multiple3MFOther）
    auto amf_files_count = get_3mf_file_count(normal_paths);

    if (normal_paths.size() > 1 && amf_files_count < normal_paths.size()) { loadfiles_type = 5; }
    if (normal_paths.size() > 1 && amf_files_count == normal_paths.size()) { loadfiles_type = 3; }
    if (normal_paths.size() > 1 && amf_files_count == 0) { loadfiles_type = 4; }
    if (normal_paths.size() == 1 && amf_files_count == 1) { loadfiles_type = 1; };
    if (normal_paths.size() == 1 && amf_files_count == 0) { loadfiles_type = 2; };

    auto first_file = std::vector<fs::path>{};
    auto tmf_file   = std::vector<fs::path>{};
    auto other_file = std::vector<fs::path>{};
    auto res        = true;

    if (only_gcode_mode() || using_exported_file()) {
        if ((loadfiles_type == 2)
            || (loadfiles_type == 4)) {
            show_info(m_q, _L("Cannot add models when in preview mode!"), _L("Add Models"));
            return false;
        }
    }

    // Orca: Iters through given paths and imports files from zip then remove zip from paths
    // returns true if zip files were found
    auto handle_zips = [this](std::vector<fs::path>& paths) { // NOLINT(*-no-recursion) - Recursion is intended and should be managed properly
        bool res = false;
        for (auto it = paths.begin(); it != paths.end();) {
            if (boost::algorithm::iends_with(it->string(), ".zip")) {
                res = true;
                preview_zip_archive(*it);
                it = paths.erase(it);
            } else
                it++;
        }
        return res;
    };

    switch (loadfiles_type) {
    case 1 /*Single3MF*/:
        open_3mf_file(normal_paths[0]);
        break;

    case 2 /*SingleOther*/: {
        m_snapshots.take_snapshot(snapshot_label);
        app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
        if (handle_zips(normal_paths)) return true;
        if (load_files(normal_paths, LoadStrategy::LoadModel, false).empty()) { res = false; }
        break;
    }
    case 3 /*Multiple3MF*/:
        first_file = std::vector<fs::path>{normal_paths[0]};
        for (auto i = 0; i < normal_paths.size(); i++) {
            if (i > 0) { other_file.push_back(normal_paths[i]); }
        };

        open_3mf_file(first_file[0]);
        if (load_files(other_file, LoadStrategy::LoadModel, false).empty()) {  res = false;  }
        break;

    case 4 /*MultipleOther*/: {
        m_snapshots.take_snapshot(snapshot_label);
        app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
        if (handle_zips(normal_paths)) {
            if (normal_paths.empty()) return true;
        }
        if (load_files(normal_paths, LoadStrategy::LoadModel, true).empty()) { res = false; }
        break;
    }

    case 5 /*Multiple3MFOther*/:
        for (const auto &path : normal_paths) {
            if (boost::iends_with(path.filename().string(), ".3mf")){
                if (first_file.size() <= 0)
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            } else {
                other_file.push_back(path);
            }
        }

        open_3mf_file(first_file[0]);
        if (load_files(tmf_file, LoadStrategy::LoadModel, false).empty()) {  res = false;  }
        if (res && handle_zips(other_file)) {
            if (normal_paths.empty()) return true;
        }
        if (load_files(other_file, LoadStrategy::LoadModel, false).empty()) {  res = false;  }
        break;
    default: break;
    }

    return res;
}
bool ProjectSession::open_3mf_file(const boost::filesystem::path &file_path)
{
    std::string filename = encode_path(file_path.filename().string().c_str());
    if (!boost::algorithm::iends_with(filename, ".3mf")) {
        return false;
    }

    bool not_empty_plate = !m_model->objects.empty();
    bool load_setting_ask_when_relevant = m_q->app_config()->get(SETTING_PROJECT_LOAD_BEHAVIOUR) == OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT;
    int load_type = m_q->determine_load_type(filename, (not_empty_plate && load_setting_ask_when_relevant) ? OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK : "");   // int 桥接（LoadType：0 Unknown/1 OpenProject/2 LoadGeometry/3 LoadConfig）

    if (load_type == 0 /*Unknown*/) return false;

    switch (load_type) {
        case 1 /*OpenProject*/: {
            if (m_q->can_load_project())
                load_project(from_path(file_path), "<loadall>");
            break;
        }
        case 2 /*LoadGeometry*/: {
            m_snapshots.take_snapshot("Import Object");
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
            load_files({file_path}, LoadStrategy::LoadModel, false);
            break;
        }
        case 3 /*LoadConfig*/: {
            load_files({file_path}, LoadStrategy::LoadConfig, false);
            break;
        }
        case 0 /*Unknown*/: {
            assert(false);
            break;
        }
    }

    return true;
}
int ProjectSession::get_3mf_file_count(std::vector<boost::filesystem::path> paths)
{
    auto count = 0;
    for (const auto &path : paths) {
        if (boost::iends_with(path.filename().string(), ".3mf")) {
            count++;
        }
    }
    return count;
}
void ProjectSession::add_file()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " entry";
    wxArrayString input_files;
    m_q->import_model_dialog(input_files);
    if (input_files.empty()) return;

    std::vector<fs::path> paths;
    for (const auto &file : input_files) paths.emplace_back(into_path(file));

    std::string snapshot_label;
    assert(!paths.empty());

    snapshot_label = "Import Objects";
    snapshot_label += ": ";
    snapshot_label += encode_path(paths.front().filename().string().c_str());
    for (size_t i = 1; i < paths.size(); ++i) {
        snapshot_label += ", ";
        snapshot_label += encode_path(paths[i].filename().string().c_str());
    }

    // BBS: check file types
    auto loadfiles_type  = 0;   // int 桥接（LoadFilesType 为 cpp-local enum：0 NoFile/1 Single3MF/2 SingleOther/3 Multiple3MF/4 MultipleOther/5 Multiple3MFOther）
    auto amf_files_count = get_3mf_file_count(paths);

    if (paths.size() > 1 && amf_files_count < paths.size()) { loadfiles_type = 5; }
    if (paths.size() > 1 && amf_files_count == paths.size()) { loadfiles_type = 3; }
    if (paths.size() > 1 && amf_files_count == 0) { loadfiles_type = 4; }
    if (paths.size() == 1 && amf_files_count == 1) { loadfiles_type = 1; };
    if (paths.size() == 1 && amf_files_count == 0) { loadfiles_type = 2; };

    auto first_file = std::vector<fs::path>{};
    auto tmf_file   = std::vector<fs::path>{};
    auto other_file = std::vector<fs::path>{};

    switch (loadfiles_type)
    {
    case 1 /*Single3MF*/:
        open_3mf_file(paths[0]);
    	break;

    case 2 /*SingleOther*/: {
        m_snapshots.take_snapshot(snapshot_label);
        app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
        if (!load_files(paths, LoadStrategy::LoadModel, false).empty()) {
            if (get_project_name() == _L("Untitled") && paths.size() > 0) {
                boost::filesystem::path full_path(paths[0].string());
                set_project_name(from_u8(full_path.stem().string()));
            }
            m_main_frame->update_title();
            if (m_q->app_config()->get("recent_models") == "true")
                m_main_frame->add_to_recent_projects(paths[0].wstring());
        }
        break;
    }
    case 3 /*Multiple3MF*/:
        first_file = std::vector<fs::path>{paths[0]};
        for (auto i = 0; i < paths.size(); i++) {
            if (i > 0) { other_file.push_back(paths[i]); }
        };

        open_3mf_file(first_file[0]);
        if (!load_files(other_file, LoadStrategy::LoadModel, false).empty()) { m_main_frame->update_title(); }
        break;

    case 4 /*MultipleOther*/: {
        m_snapshots.take_snapshot(snapshot_label);
        app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
        if (!load_files(paths, LoadStrategy::LoadModel, true).empty()) {
            if (get_project_name() == _L("Untitled") && paths.size() > 0) {
                boost::filesystem::path full_path(paths[0].string());
                set_project_name(from_u8(full_path.stem().string()));
            }
            m_main_frame->update_title();
            if (m_q->app_config()->get("recent_models") == "true")
                for (auto &path : paths)
                    m_main_frame->add_to_recent_projects(path.wstring());
        }
        break;
    }
    case 5 /*Multiple3MFOther*/:
        for (const auto &path : paths) {
            if (boost::iends_with(path.filename().string(), ".3mf")) {
                if (first_file.size() <= 0)
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            } else {
                other_file.push_back(path);
            }
        }

        open_3mf_file(first_file[0]);
        load_files(tmf_file, LoadStrategy::LoadModel, false);
        if (!load_files(other_file, LoadStrategy::LoadModel, false).empty()) {
            m_main_frame->update_title();
            if (m_q->app_config()->get("recent_models") == "true")
                for (auto &file : other_file)
                    m_main_frame->add_to_recent_projects(file.wstring());
        }
        break;
    default:break;
    }
}
void ProjectSession::add_model(bool imperial_units, std::string fname)
{
    wxArrayString input_files;

    std::vector<fs::path> paths;
    if (fname.empty()) {
        m_q->import_model_dialog(input_files);
        if (input_files.empty())
            return;

        for (const auto& file : input_files)
            paths.emplace_back(into_path(file));
    }
    else {
        paths.emplace_back(fname);
    }

    std::string snapshot_label;
    assert(! paths.empty());
    if (paths.size() == 1) {
        snapshot_label = "Import Object";
        snapshot_label += ": ";
        snapshot_label += encode_path(paths.front().filename().string().c_str());
    } else {
        snapshot_label = "Import Objects";
        snapshot_label += ": ";
        snapshot_label += paths.front().filename().string().c_str();
        for (size_t i = 1; i < paths.size(); ++ i) {
            snapshot_label += ", ";
            snapshot_label += encode_path(paths[i].filename().string().c_str());
        }
    }

    m_snapshots.take_snapshot(snapshot_label);
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);

    // BBS: check file types
    auto loadfiles_type  = 0;   // int 桥接（LoadFilesType 为 cpp-local enum：0 NoFile/1 Single3MF/2 SingleOther/3 Multiple3MF/4 MultipleOther/5 Multiple3MFOther）
    auto amf_files_count = get_3mf_file_count(paths);

    if (paths.size() > 1 && amf_files_count < paths.size()) { loadfiles_type = 5; }
    if (paths.size() > 1 && amf_files_count == paths.size()) { loadfiles_type = 3; }
    if (paths.size() > 1 && amf_files_count == 0) { loadfiles_type = 4; }
    if (paths.size() == 1 && amf_files_count == 1) { loadfiles_type = 1; };
    if (paths.size() == 1 && amf_files_count == 0) { loadfiles_type = 2; };

    bool ask_multi = false;

    if (loadfiles_type == 4)
        ask_multi = true;

    auto strategy = LoadStrategy::LoadModel;
    if (imperial_units) strategy = strategy | LoadStrategy::ImperialUnits;
    if (!load_files(paths, strategy, ask_multi).empty()) {

        if (get_project_name() == _L("Untitled") && paths.size() > 0) {
            boost::filesystem::path full_path(paths[0].string());
            set_project_name(from_u8(full_path.stem().string()));
        }

        m_main_frame->update_title();
    }
}
bool ProjectSession::preview_zip_archive(const boost::filesystem::path& archive_path)
{
    //std::vector<fs::path> unzipped_paths;
    std::vector<fs::path> non_project_paths;
    std::vector<fs::path> project_paths;
    try
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        if (!open_zip_reader(&archive, archive_path.string())) {
            // TRN %1% is archive path
            std::string err_msg = GUI::format(_u8L("Loading of a ZIP archive on path %1% has failed."), archive_path.string());
            throw Slic3r::FileIOError(err_msg);
        }
        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
        mz_zip_archive_file_stat stat;
        // selected_paths contains paths and its uncompressed size. The size is used to distinguish between files with same path.
        std::vector<std::pair<fs::path, size_t>> selected_paths;
        FileArchiveDialog dlg(static_cast<wxWindow*>(m_main_frame), &archive, selected_paths);
        if (dlg.ShowModal() == wxID_OK)
        {
            std::string archive_path_string = archive_path.string();
            archive_path_string = archive_path_string.substr(0, archive_path_string.size() - 4);
            fs::path archive_dir(wxStandardPaths::Get().GetTempDir().utf8_str().data());

            for (auto& path_w_size : selected_paths) {
                const fs::path& path = path_w_size.first;
                size_t size = path_w_size.second;
                // find path in zip archive
                for (mz_uint i = 0; i < num_entries; ++i) {
                    if (mz_zip_reader_file_stat(&archive, i, &stat)) {
                        if (size != stat.m_uncomp_size) // size must fit
                            continue;
                        wxString wname = boost::nowide::widen(stat.m_filename);
                        std::string name = into_u8(wname);
                        fs::path archive_path(name);

                        std::string extra(1024, 0);
                        size_t extra_size = mz_zip_reader_get_filename_from_extra(&archive, i, extra.data(), extra.size());
                        if (extra_size > 0) {
                            archive_path = fs::path(extra.substr(0, extra_size));
                            name = archive_path.string();
                        }

                        if (archive_path.empty())
                            continue;
                        if (path != archive_path)
                            continue;
                        // decompressing
                        try
                        {
                            std::replace(name.begin(), name.end(), '\\', '/');
                            // rename if file exists
                            std::string filename = path.filename().string();
                            std::string extension = path.extension().string();
                            std::string just_filename = filename.substr(0, filename.size() - extension.size());
                            std::string final_filename = just_filename;

                            size_t version = 0;
                            while (fs::exists(archive_dir / (final_filename + extension)))
                            {
                                ++version;
                                final_filename = just_filename + "(" + std::to_string(version) + ")";
                            }
                            filename = final_filename + extension;
                            fs::path final_path = archive_dir / filename;
                            std::string buffer((size_t)stat.m_uncomp_size, 0);
                            // Decompress action. We already has correct file index in stat structure.
                            mz_bool res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
                            if (res == 0) {
                                // TRN: First argument = path to file, second argument = error description
                                wxString error_log = GUI::format_wxstr(_L("Failed to unzip file to %1%: %2%"), final_path.string(), mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
                                BOOST_LOG_TRIVIAL(error) << error_log;
                                show_error(nullptr, error_log);
                                break;
                            }
                            // write buffer to file
                            fs::fstream file(final_path, std::ios::out | std::ios::binary | std::ios::trunc);
                            file.write(buffer.c_str(), buffer.size());
                            file.close();
                            if (!fs::exists(final_path)) {
                                wxString error_log = GUI::format_wxstr(_L("Failed to find unzipped file at %1%. Unzipping of file has failed."), final_path.string());
                                BOOST_LOG_TRIVIAL(error) << error_log;
                                show_error(nullptr, error_log);
                                break;
                            }
                            BOOST_LOG_TRIVIAL(info) << "Unzipped " << final_path;
                            if (!boost::algorithm::iends_with(filename, ".3mf") && !boost::algorithm::iends_with(filename, ".amf")) {
                                non_project_paths.emplace_back(final_path);
                                break;
                            }
                            // if 3mf - read archive headers to find project file
                            if (/*(boost::algorithm::iends_with(filename, ".3mf") && !is_project_3mf(final_path.string())) ||*/
                                (boost::algorithm::iends_with(filename, ".amf") && !boost::algorithm::iends_with(filename, ".zip.amf"))) {
                                non_project_paths.emplace_back(final_path);
                                break;
                            }

                            project_paths.emplace_back(final_path);
                            break;
                        }
                        catch (const std::exception& e)
                        {
                            // ensure the zip archive is closed and rethrow the exception
                            close_zip_reader(&archive);
                            throw Slic3r::FileIOError(e.what());
                        }
                    }
                }
            }
            close_zip_reader(&archive);
            if (non_project_paths.size() + project_paths.size() != selected_paths.size())
                BOOST_LOG_TRIVIAL(error) << "Decompresing of archive did not retrieve all files. Expected files: "
                                         << selected_paths.size()
                                         << " Decopressed files: "
                                         << non_project_paths.size() + project_paths.size();
        } else {
            close_zip_reader(&archive);
            return false;
        }

    }
    catch (const Slic3r::FileIOError& e) {
        // zip reader should be already closed or not even opened
        GUI::show_error(m_q, e.what());
        return false;
    }
    // none selected
    if (project_paths.empty() && non_project_paths.empty())
    {
        return false;
    }

    // 1 project file and some models - behave like drag n drop of 3mf and then load models
    if (project_paths.size() == 1)
    {
        wxArrayString aux;
        aux.Add(from_u8(project_paths.front().string()));
        bool loaded3mf = load_files(aux);
        load_files(non_project_paths, LoadStrategy::LoadModel, false);
        boost::system::error_code ec;
        if (loaded3mf) {
            fs::remove(project_paths.front(), ec);
            if (ec)
                BOOST_LOG_TRIVIAL(error) << ec.message();
        }
        for (const fs::path& path : non_project_paths) {
            // Delete file from temp file (path variable), it will stay only in app memory.
            boost::system::error_code ec;
            fs::remove(path, ec);
            if (ec)
                BOOST_LOG_TRIVIAL(error) << ec.message();
        }
        return true;
    }

    // load all projects and all models as geometry
    load_files(project_paths, LoadStrategy::LoadModel, false);
    load_files(non_project_paths, LoadStrategy::LoadModel, false);


    for (const fs::path& path : project_paths) {
        // Delete file from temp file (path variable), it will stay only in app memory.
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << ec.message();
    }
    for (const fs::path& path : non_project_paths) {
        // Delete file from temp file (path variable), it will stay only in app memory.
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << ec.message();
    }

    return true;
}



static const wxString DEFAULT_PROJECT_NAME = "Untitled";

void ProjectSession::load_gcode()
{
    // Ask user for a gcode file name.
    wxString input_file;
    m_q->load_gcode_dialog(input_file);
    // And finally load the gcode file.
    load_gcode(input_file);
}

//BBS: remove GCodeViewer as seperate APP logic
void ProjectSession::load_gcode(const wxString& filename)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " entry and filename: " << filename;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__;
    if (! is_gcode_file(into_u8(filename)))
        return;

    if (get_last_loaded_gcode() == filename && only_gcode_mode()) {
        // The same G-code is already loaded: reloading would be a no-op, so
        // just make sure the user is looking at its preview — callers may
        // have left the UI on another view (e.g. the 3D editor after a
        // drag & drop).
        m_main_frame->select_tab(MainFrame::tpPreview);
        m_layout->set_current_panel(m_layout->preview, true);
        GLCanvas3D* canvas = m_layout->get_current_canvas3D();
        if (canvas)
            canvas->render();
        return;
    }

    // Reject a missing / inaccessible file up front. Without this check the
    // code below would walk through process_file -> parse_file_raw_internal,
    // which used to crash on a NULL FILE* (now it just returns false), and
    // would otherwise surface the misleading "does not contain valid G-code"
    // message even when the real problem is that the file doesn't exist at all.
    if (!wxFileExists(filename))
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": file does not exist: " << filename;
        MessageDialog(m_q,
            _L("The selected file") + ":\n" + filename + "\n" + _L("does not exist."),
            wxString(GCODEVIEWER_APP_NAME) + " - " + _L("Error occurs while loading G-code file"),
            wxCLOSE | wxICON_WARNING | wxCENTRE).ShowModal();
        return;
    }

    m_last_loaded_gcode = filename;

    // BSS: create a new project when load_gcode, force close previous one
    if (new_project(false, true) != wxID_YES)
        return;

    set_only_gcode(true);

    // cleanup view before to start loading/processing
    //BBS: update gcode to current partplate's
    GCodeProcessorResult* current_result = m_partplate_list.get_current_slice_result();
    Print& current_print = m_partplate_list.get_current_fff_print();
    //BBS:already reset in new_project
    //current_result->reset();
    //p->slicing().gcode_result().reset();
    m_layout->preview->reload_print(false, only_gcode_mode());
    m_main_frame->select_tab(MainFrame::tpPreview);
    m_layout->set_current_panel(m_layout->preview, true);
    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (canvas)
        canvas->render();
    //p->notification_manager()->bbl_show_plateinfo_notification(into_u8(_L("Preview only mode for gcode file.")));

    wxBusyCursor wait;

    // process gcode
    GCodeProcessor processor;
    try
    {
        GCodeProcessor::s_IsBBLPrinter = m_q->preset_bundle()->is_bbl_vendor();
        processor.process_file(filename.ToUTF8().data());
    }
    catch (const std::exception& ex)
    {
        show_error(m_q, ex.what());
        return;
    }
    *current_result = std::move(processor.extract_result());
    //current_result->filename = filename;

    BedType bed_type = current_result->bed_type;
    if (bed_type != BedType::btCount) {
        DynamicPrintConfig &proj_config = m_q->preset_bundle()->project_config;
        proj_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
        m_q->on_bed_type_change(bed_type);
    }

    current_print.apply(*m_model, m_q->preset_bundle()->full_config());

    //BBS: add cost info when drag in gcode
    auto& ps = current_result->print_statistics;
    double total_cost = 0.0;
    for (auto volume : ps.total_volumes_per_extruder) {
        size_t extruder_id = volume.first;
        double density = current_result->filament_densities.at(extruder_id);
        double cost = current_result->filament_costs.at(extruder_id);
        double weight = volume.second * density * 0.001;
        total_cost += weight * cost * 0.001;
    }
    current_print.print_statistics().total_cost = total_cost;

    current_print.set_gcode_file_ready();

    // show results
    m_layout->preview->reload_print(false, only_gcode_mode());
    //BBS: zoom to bed 0 for gcode preview
    //m_layout->preview->get_canvas3d()->zoom_to_gcode();
    m_layout->preview->get_canvas3d()->zoom_to_plate(0);

    if (m_layout->preview->get_canvas3d()->get_gcode_layers_zs().empty()) {
        MessageDialog(m_q, _L("The selected file") + ":\n" + filename + "\n" + _L("does not contain valid G-code."),
            wxString(GCODEVIEWER_APP_NAME) + " - " + _L("Error occurs while loading G-code file"), wxCLOSE | wxICON_WARNING | wxCENTRE).ShowModal();
        set_project_filename(DEFAULT_PROJECT_NAME);
    } else {
        set_project_filename(filename);
    }

    // Orca: Fix crash when loading gcode file multiple times
    if (only_gcode_mode()) {
        m_layout->view3D->get_canvas3d()->remove_raycasters_for_picking(SceneRaycaster::EType::Bed);
    }
}

void ProjectSession::reload_gcode_from_disk()
{
    wxString filename(get_last_loaded_gcode());
    reset_last_loaded_gcode();
    load_gcode(filename);
}


bool ProjectSession::replace_volume_with_stl(int object_idx, int volume_idx, const boost::filesystem::path& new_path, const std::string& snapshot)
{
    const std::string path = new_path.string();
    wxBusyCursor wait;

    Model new_model;
    try {
        new_model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        for (ModelObject* model_object : new_model.objects) {
            model_object->center_around_origin();
            model_object->ensure_on_bed();
        }
    }
    catch (std::exception&) {
        // error while loading
        return false;
    }

    if (new_model.objects.size() > 1 || new_model.objects.front()->volumes.size() > 1) {
        MessageDialog dlg(m_q, _L("Unable to replace with more than one volume"), _L("Error during replace"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
        return false;
    }

    GLCanvas3D* canvas = m_layout->get_current_canvas3D();
    if (!canvas)
        return false;
    wxBusyInfo info(_L("Replace from:") + " " + from_u8(path), canvas->get_wxglcanvas());

    if (!snapshot.empty())
        m_snapshots.take_snapshot(snapshot);

    ModelObject* old_model_object = m_model->objects[object_idx];
    ModelVolume* old_volume = old_model_object->volumes[volume_idx];

    bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;

    ModelObject* new_model_object = new_model.objects.front();
    old_model_object->add_volume(*new_model_object->volumes.front());
    ModelVolume* new_volume = old_model_object->volumes.back();
    new_volume->set_new_unique_id();
    new_volume->config.apply(old_volume->config);
    new_volume->set_type(old_volume->type());
    new_volume->set_material_id(old_volume->material_id());
    new_volume->set_transformation(old_volume->get_transformation());
    new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
    assert(!old_volume->source.is_converted_from_inches || !old_volume->source.is_converted_from_meters);
    if (old_volume->source.is_converted_from_inches)
        new_volume->convert_from_imperial_units();
    else if (old_volume->source.is_converted_from_meters)
        new_volume->convert_from_meters();
    new_volume->supported_facets.assign(old_volume->supported_facets);
    new_volume->seam_facets.assign(old_volume->seam_facets);
    new_volume->mmu_segmentation_facets.assign(old_volume->mmu_segmentation_facets);
    new_volume->fuzzy_skin_facets.assign(old_volume->fuzzy_skin_facets);
    std::swap(old_model_object->volumes[volume_idx], old_model_object->volumes.back());
    old_model_object->delete_volume(old_model_object->volumes.size() - 1);
    if (!sinking)
        old_model_object->ensure_on_bed();
    old_model_object->sort_volumes(true);

    // if object has just one volume, rename object too
    if (old_model_object->volumes.size() == 1)
        old_model_object->name = old_model_object->volumes.front()->name;

    // update new name in ObjectList
    m_layout->sidebar->obj_list()->update_name_in_list(object_idx, volume_idx);

    sla::reproject_points_and_holes(old_model_object);

    return true;
}
void ProjectSession::replace_with_stl()
{
    if (! m_layout->view3D->get_canvas3d()->get_gizmos_manager().check_gizmos_closed_except(GLGizmosManager::EType::Undefined))
        return;

    const Selection& selection = m_selq->get_selection();

    if (selection.is_wipe_tower() || m_selq->get_selection().get_volume_idxs().size() != 1)
        return;

    const GLVolume* v = selection.get_first_volume();
    int object_idx = v->object_idx();
    int volume_idx = v->volume_idx();

    // collects paths of files to load

    const ModelObject* object = m_model->objects[object_idx];
    const ModelVolume* volume = object->volumes[volume_idx];

    fs::path input_path;
    if (!volume->source.input_file.empty() && fs::exists(volume->source.input_file))
        input_path = volume->source.input_file;

    wxString title = _L("Select a new file");
    title += ":";
    wxFileDialog dialog(m_q, title, "", from_u8(input_path.filename().string()), m_q->model_file_wildcards(), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    fs::path out_path = dialog.GetPath().ToUTF8().data();
    if (out_path.empty()) {
        MessageDialog dlg(m_q, _L("File for the replace wasn't selected"), _L("Error during replace"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
        return;
    }

    if (!replace_volume_with_stl(object_idx, volume_idx, out_path, "Replace with STL"))
        return;

    // update 3D scene
    m_refresh->update(0);   // 原 priv::update()（默认 flags=0）

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < m_model->objects.size(); ++i) {
        m_layout->view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }
}
#if ENABLE_RELOAD_FROM_DISK_REWORK
std::vector<std::pair<int, int>> ProjectSession::reloadable_volumes(const Model &model, const Selection &selection)
{
    std::vector<std::pair<int, int>> ret;
    const std::set<unsigned int> &   selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs) {
        const GLVolume &v     = *selection.get_volume(idx);
        const int       o_idx = v.object_idx();
        if (0 <= o_idx && o_idx < int(model.objects.size())) {
            const ModelObject *obj   = model.objects[o_idx];
            const int          v_idx = v.volume_idx();
            if (0 <= v_idx && v_idx < int(obj->volumes.size())) {
                const ModelVolume *vol = obj->volumes[v_idx];
                if (!vol->source.is_from_builtin_objects && !vol->source.input_file.empty() && !fs::path(vol->source.input_file).extension().string().empty())
                    ret.push_back({o_idx, v_idx});
            }
        }
    }
    return ret;
}
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
void ProjectSession::reload_all_from_disk()
{
    if (m_model->objects.empty())
        return;

    m_snapshots.take_snapshot("Reload all");
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);

    Selection& selection = const_cast<Selection&>(m_selq->get_selection());
    Selection::IndicesList curr_idxs = selection.get_volume_idxs();
    // reload from disk uses selection
    m_q->select_all();
    reload_from_disk();
    // restore previous selection
    selection.clear();
    for (unsigned int idx : curr_idxs) {
        selection.add(idx, false);
    }
}
void ProjectSession::import_model_id(wxString download_info)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " download info: " << download_info;

    wxString download_origin_url = download_info;
    wxString download_url;
    wxString filename;
    wxString separator = "&name=";

    try
    {
        size_t namePos = download_info.Find(separator);
        if (namePos != wxString::npos) {
            download_url = download_info.Mid(0, namePos);
            filename = download_info.Mid(namePos + separator.Length());

        }
        else {
            fs::path download_path = fs::path(download_origin_url.wx_str());
            download_url = download_origin_url;
            filename = download_path.filename().string();
        }

    }
    catch (const std::exception&)
    {
        //wxString sError = error.what();
    }

    bool download_ok = false;
    int retry_count = 0;
    const int max_retries = 3;

    /* jump to 3D eidtor */
    m_main_frame->select_tab((size_t)MainFrame::TabPosition::tp3DEditor);

    /* prepare progress dialog */
    bool cont = true;
    bool cont_dlg = true;
    bool cancel = false;
    wxString msg;
    wxString dlg_title = _L("Importing Model");

    int percent = 0;
    ProgressDialog dlg(dlg_title,
        wxString(' ', 100) + "\n\n\n\n",
        100,    // range
        m_q,   // parent
        wxPD_CAN_ABORT |
        wxPD_APP_MODAL |
        wxPD_AUTO_HIDE |
        wxPD_SMOOTH);

    boost::filesystem::path target_path;

    //reset params
    project.reset();

    /* prepare project and profile */
    boost::thread import_thread = Slic3r::create_thread([this, &percent, &cont, &cancel, &retry_count, max_retries, &msg, &target_path, &download_ok, download_url, &filename] {

        // Orca: NetworkAgent is not needed and only prevents this from running
//        NetworkAgent* m_agent = Slic3r::GUI::wxGetApp().getAgent();
//        if (!m_agent) return;

        int res = 0;
        std::string http_body;

        msg = _L("prepare 3mf file...");

        //gets the number of files with the same name
        std::vector<wxString>   vecFiles;
        bool                    is_already_exist = false;


        target_path = fs::path(m_q->app_config()->get("download_path"));

        try
        {
            vecFiles.clear();
            wxString extension = fs::path(filename.wx_str()).extension().c_str();


            //check file suffix
            if (!extension.Contains(".3mf")) {
                msg = _L("Download failed, unknown file format.");
                return;
            }

            auto name = filename.substr(0, filename.length() - extension.length() - 1);

            for (const auto& iter : boost::filesystem::directory_iterator(target_path))
            {
                if (boost::filesystem::is_directory(iter.path()))
                    continue;

                wxString sFile = iter.path().filename().string().c_str();
                if (strstr(sFile.c_str(), name.c_str()) != NULL) {
                    vecFiles.push_back(sFile);
                }

                if (sFile == filename) is_already_exist = true;
            }
        }
        catch (const std::exception&)
        {
            //wxString sError = error.what();
        }

        //update filename
        if (is_already_exist && vecFiles.size() >= 1) {
            wxString extension = fs::path(filename.wx_str()).extension().c_str();
            wxString name = filename.substr(0, filename.length() - extension.length());
            filename = wxString::Format("%s(%d)%s", name, vecFiles.size() + 1, extension).ToStdString();
        }


        msg = _L("downloading project...");

        //target_path = wxStandardPaths::Get().GetTempDir().utf8_str().data();


        //target_path = wxGetApp().get_local_models_path().c_str();
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        std::string unique = to_string(uuid).substr(0, 6);

        if (filename.empty()) {
            filename = "untitled.3mf";
        }

        //target_path /= (boost::format("%1%_%2%.3mf") % filename % unique).str();
        target_path /= fs::path(filename.wc_str());

        fs::path tmp_path = target_path;
        tmp_path += format(".%1%", ".download");

        auto filesize = 0;
        bool size_limit = false;
        auto http = Http::get(download_url.ToStdString());

        while (cont && retry_count < max_retries) {
            retry_count++;
            http.on_progress([&percent, &cont, &msg, &filesize, &size_limit](Http::Progress progress, bool& cancel) {

                    if (!cont) cancel = true;
                    if (progress.dltotal != 0) {

                        if (filesize == 0) {
                            filesize = progress.dltotal;
                            double megabytes = static_cast<double>(progress.dltotal) / (1024 * 1024);
                            //The maximum size of a 3mf file is 500mb
                            if (megabytes > 500) {
                                cont = false;
                                size_limit = true;
                            }
                        }
                        percent = progress.dlnow * 100 / progress.dltotal;
                    }

                    if (size_limit) {
                        msg = _L("Download failed, File size exception.");
                    }
                    else {
                        msg = wxString::Format(_L("Project downloaded %d%%"), percent);
                    }
                })
                .on_error([&msg, &cont, &retry_count, max_retries](std::string body, std::string error, unsigned http_status) {
                    (void)body;
                    BOOST_LOG_TRIVIAL(error) << format("Error getting: `%1%`: HTTP %2%, %3%",
                        body,
                        http_status,
                        error);

                    if (retry_count == max_retries) {
                        msg = _L("Importing to Snapmaker Orca failed. Please download the file and manually import it.");
                        cont = false;
                    }
                })
                .on_complete([&cont, &download_ok, tmp_path, target_path](std::string body, unsigned /* http_status */) {
                        fs::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
                        file.write(body.c_str(), body.size());
                        file.close();
                        fs::rename(tmp_path, target_path);
                        cont = false;
                        download_ok = true;
                }).perform_sync();

                // for break while
                //cont = false;
        }

    });

    while (cont && cont_dlg) {
        wxMilliSleep(50);
        cont_dlg = dlg.Update(percent, msg);
        if (!cont_dlg) {
            cont = cont_dlg;
            cancel = true;
        }

        if (download_ok)
            break;
    }

    if (import_thread.joinable())
        import_thread.join();

    dlg.Hide();
    dlg.Close();
    if (download_ok) {
        BOOST_LOG_TRIVIAL(trace) << "import_model_id: target_path = " << target_path.string();
        /* load project */
        // Orca: If download is a zip file, treat it as if file has been drag and dropped on the plater
        if (target_path.extension() == ".zip")
            load_files(wxArrayString(1, target_path.string()));
        else
            load_project(target_path.wstring());
        /*BBS set project info after load project, project info is reset in load project */
        //project.project_model_id = model_id;
        //project.project_design_id = design_id;
        AppConfig* config = m_q->app_config();
        if (config) {
            project.project_country_code = config->get_country_code();
        }

        // show save new project
        set_project_filename(target_path.wstring());
        m_hub.nm()->push_import_finished_notification(target_path.string(), target_path.parent_path().string(), false);
    }
    else {
        if (!msg.empty()) {
            MessageDialog msg_wingow(nullptr, msg, wxEmptyString, wxICON_WARNING | wxOK);
            msg_wingow.SetSize(wxSize(msg_wingow.FromDIP(480), -1));   // 原 this->FromDIP（Plater 成员）；对话框自身 DPI 缩放等价
            msg_wingow.ShowModal();
        }
        return;
    }
}
void ProjectSession::download_project(const wxString& project_id)
{
    return;
}
void ProjectSession::request_model_download(wxString url)
{
    m_q->request_model_download_evt(url);
}
void ProjectSession::import_zip_archive()
{
    wxString input_file;
    m_q->import_zip_dialog(input_file);
    if (input_file.empty())
        return;

    wxArrayString arr;
    arr.Add(input_file);
    load_files(arr);
}
void ProjectSession::import_sl1_archive()
{
    m_q->run_sla_import_job();
}void ProjectSession::extract_config_from_project()
{
    wxString input_file;
    m_q->load_project_dialog(input_file);

    if (! input_file.empty())
        load_files({ into_path(input_file) }, LoadStrategy::LoadConfig, false);
}

void ProjectSession::reload_from_disk()
{
#if ENABLE_RELOAD_FROM_DISK_REWORK
    // collect selected reloadable ModelVolumes
    std::vector<std::pair<int, int>> selected_volumes = reloadable_volumes(*m_model, m_selq->get_selection());
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and reloadable volumes number is: " << selected_volumes.size();
    // nothing to reload, return
    if (selected_volumes.empty())
        return;

    std::sort(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first < v2.first) || (v1.first == v2.first && v1.second < v2.second);
        });
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first == v2.first) && (v1.second == v2.second);
        }), selected_volumes.end());
#else
    m_snapshots.take_snapshot("Reload from disk");
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);

    const Selection& selection = m_selq->get_selection();

    if (selection.is_wipe_tower())
        return;

    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return object_idx < other.object_idx || (object_idx == other.object_idx && volume_idx < other.volume_idx); }
        bool operator == (const SelectedVolume& other) const { return object_idx == other.object_idx && volume_idx == other.volume_idx; }
    };
    std::vector<SelectedVolume> selected_volumes;

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
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    // collects paths of files to load
    std::vector<fs::path> input_paths;
    std::vector<fs::path> missing_input_paths;
#if ENABLE_RELOAD_FROM_DISK_REWORK
    std::vector<std::pair<fs::path, fs::path>> replace_paths;
    for (auto [obj_idx, vol_idx] : selected_volumes) {
        const ModelObject *object = m_model->objects[obj_idx];
        const ModelVolume *volume = object->volumes[vol_idx];
        if (fs::exists(volume->source.input_file))
            input_paths.push_back(volume->source.input_file);
        else {
            // searches the source in the same folder containing the object
            bool found = false;
            if (!object->input_file.empty()) {
                fs::path object_path = fs::path(object->input_file).remove_filename();
                if (!object_path.empty()) {
                    object_path /= fs::path(volume->source.input_file).filename();
                    if (fs::exists(object_path)) {
                        input_paths.push_back(object_path);
                        found = true;
                    }
                }
            }
            if (!found)
                missing_input_paths.push_back(volume->source.input_file);
        }
    }
#else
    std::vector<fs::path> replace_paths;
    for (const SelectedVolume& v : selected_volumes) {
        const ModelObject* object = m_model->objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];

        if (!volume->source.input_file.empty()) {
            if (fs::exists(volume->source.input_file))
                input_paths.push_back(volume->source.input_file);
            else {
                // searches the source in the same folder containing the object
                bool found = false;
                if (!object->input_file.empty()) {
                    fs::path object_path = fs::path(object->input_file).remove_filename();
                    if (!object_path.empty()) {
                        object_path /= fs::path(volume->source.input_file).filename();
                        const std::string source_input_file = object_path.string();
                        if (fs::exists(source_input_file)) {
                            input_paths.push_back(source_input_file);
                            found = true;
                        }
                    }
                }
                if (!found)
                    missing_input_paths.push_back(volume->source.input_file);
            }
        }
        else if (!object->input_file.empty() && volume->is_model_part() && !volume->name.empty() && !volume->source.is_from_builtin_objects)
            missing_input_paths.push_back(volume->name);
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    std::sort(missing_input_paths.begin(), missing_input_paths.end());
    missing_input_paths.erase(std::unique(missing_input_paths.begin(), missing_input_paths.end()), missing_input_paths.end());

    while (!missing_input_paths.empty()) {
        // ask user to select the missing file
        fs::path search = missing_input_paths.back();
        wxString title = _L("Please select a file");
#if defined(__APPLE__)
        title += " (" + from_u8(search.filename().string()) + ")";
#endif // __APPLE__
        title += ":";
        wxFileDialog dialog(m_q, title, "", from_u8(search.filename().string()), file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::string sel_filename_path = dialog.GetPath().ToUTF8().data();
        std::string sel_filename = fs::path(sel_filename_path).filename().string();
        if (boost::algorithm::iequals(search.filename().string(), sel_filename)) {
            input_paths.push_back(sel_filename_path);
            missing_input_paths.pop_back();

            fs::path sel_path = fs::path(sel_filename_path).remove_filename().string();

            std::vector<fs::path>::iterator it = missing_input_paths.begin();
            while (it != missing_input_paths.end()) {
                // try to use the path of the selected file with all remaining missing files
                fs::path repathed_filename = sel_path;
                repathed_filename /= it->filename();
                if (fs::exists(repathed_filename)) {
                    input_paths.push_back(repathed_filename.string());
                    it = missing_input_paths.erase(it);
                }
                else
                    ++it;
            }
        }
        else {
            wxString      message = _L("Do you want to replace it") + " ?";
            MessageDialog dlg(m_q, message, _L("Message"), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
            if (dlg.ShowModal() == wxID_YES)
#if ENABLE_RELOAD_FROM_DISK_REWORK
                replace_paths.emplace_back(search, sel_filename_path);
#else
                replace_paths.emplace_back(sel_filename_path);
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
            missing_input_paths.pop_back();
        }
    }

    std::sort(input_paths.begin(), input_paths.end());
    input_paths.erase(std::unique(input_paths.begin(), input_paths.end()), input_paths.end());

    std::sort(replace_paths.begin(), replace_paths.end());
    replace_paths.erase(std::unique(replace_paths.begin(), replace_paths.end()), replace_paths.end());

#if ENABLE_RELOAD_FROM_DISK_REWORK
    m_snapshots.take_snapshot("Reload from disk");
    app::SnapshotService::SuppressScope suppress_scope(m_snapshots);
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    std::vector<wxString> fail_list;

    // load one file at a time
    for (size_t i = 0; i < input_paths.size(); ++i) {
        const auto& path = input_paths[i].string();
        auto obj_color_fun = [this, &path](std::vector<RGBA> &input_colors, bool is_single_color, std::vector<unsigned char> &filament_ids, unsigned char &first_extruder_id) {
            if (!boost::iends_with(path, ".obj")) { return; }
            const std::vector<std::string> extruder_colours = m_q->get_extruder_colors_from_plater_config();
            ObjColorDialog                 color_dlg(nullptr, input_colors, is_single_color, extruder_colours, filament_ids, first_extruder_id);
            if (color_dlg.ShowModal() != wxID_OK) { filament_ids.clear(); }
        };
        wxBusyCursor wait;
        GLCanvas3D* canvas = m_layout->get_current_canvas3D();
        if (!canvas)
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "canvas is nullptr";
            continue;
        }
        wxBusyInfo info(_L("Reload from:") + " " + from_u8(path), canvas->get_wxglcanvas());

        Model new_model;
        try
        {
            //BBS: add plate data related logic
            PlateDataPtrs plate_data;
            //BBS: project embedded settings
            std::vector<Preset*> project_presets;

            // BBS: backup
            if (boost::iends_with(path, ".stp") ||
                boost::iends_with(path, ".step")) {
                double linear = string_to_double_decimal_point(m_q->app_config()->get("linear_defletion"));
                double angle = string_to_double_decimal_point(m_q->app_config()->get("angle_defletion"));
                bool   is_split = m_q->app_config()->get_bool("is_split_compound");
                new_model       = Model::read_from_step(path, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, nullptr, nullptr, nullptr, linear, angle, is_split);
            }else {
                new_model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, &plate_data, &project_presets, nullptr, nullptr, nullptr, nullptr, nullptr, 0, obj_color_fun);
            }


            for (ModelObject* model_object : new_model.objects)
            {
                model_object->center_around_origin();
                model_object->ensure_on_bed();
            }

            if (plate_data.size() > 0)
            {
                //partplate_list.load_from_3mf_structure(plate_data);
                m_partplate_list.update_slice_context_to_current_plate(m_slicing.background_process());
                m_layout->preview->update_gcode_result(m_partplate_list.get_current_slice_result());
                release_PlateData_list(plate_data);
                m_layout->sidebar->obj_list()->reload_all_plates();
            }
        }
        catch (std::exception&)
        {
            // error while loading
            return;
        }

#if ENABLE_RELOAD_FROM_DISK_REWORK
        for (auto [obj_idx, vol_idx] : selected_volumes) {
            ModelObject *old_model_object = m_model->objects[obj_idx];
            ModelVolume *old_volume       = old_model_object->volumes[vol_idx];

            bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;

            bool has_source = !old_volume->source.input_file.empty() &&
                              boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (has_source || has_name) {
                int  new_volume_idx = -1;
                int  new_object_idx = -1;
                bool match_found    = false;
                // take idxs from the matching volume
                if (has_source && old_volume->source.object_idx < int(new_model.objects.size())) {
                    const ModelObject *obj = new_model.objects[old_volume->source.object_idx];
                    if (old_volume->source.volume_idx < int(obj->volumes.size())) {
                        if (obj->volumes[old_volume->source.volume_idx]->source.input_file == old_volume->source.input_file) {
                            new_volume_idx = old_volume->source.volume_idx;
                            new_object_idx = old_volume->source.object_idx;
                            match_found    = true;
                        }
                    }
                }

                if (!match_found && has_name) {
                    // take idxs from the 1st matching volume
                    for (size_t o = 0; o < new_model.objects.size(); ++o) {
                        ModelObject *obj   = new_model.objects[o];
                        bool         found = false;
                        for (size_t v = 0; v < obj->volumes.size(); ++v) {
                            if (obj->volumes[v]->name == old_volume->name) {
                                new_volume_idx = (int) v;
                                new_object_idx = (int) o;
                                found          = true;
                                break;
                            }
                        }
                        if (found) break;
                        // BBS: step model,object loaded as a volume. GUI_ObfectList.cpp load_modifier()
                        if (obj->name == old_volume->name) {
                            new_object_idx = (int) o;
                            break;
                        }
                    }
                }

                if (new_object_idx < 0 || int(new_model.objects.size()) <= new_object_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                ModelObject *new_model_object = new_model.objects[new_object_idx];
                if (int(new_model_object->volumes.size()) <= new_volume_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }

                ModelVolume *new_volume = nullptr;
                // BBS: step model
                if (new_volume_idx < 0 && new_object_idx >= 0) {
                    TriangleMesh mesh = new_model_object->mesh();
                    new_volume = old_model_object->add_volume(std::move(mesh));
                    new_volume->name  = new_model_object->name;
                    new_volume->source.input_file = new_model_object->input_file;
                }else {
                    new_volume = old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
                    // new_volume = old_model_object->volumes.back();
                }
                
                new_volume->set_new_unique_id();
                new_volume->config.apply(old_volume->config);
                new_volume->set_type(old_volume->type());
                new_volume->set_material_id(old_volume->material_id());

                new_volume->source.mesh_offset = old_volume->source.mesh_offset;
                new_volume->set_transformation(old_volume->get_transformation());

                new_volume->source.object_idx = old_volume->source.object_idx;
                new_volume->source.volume_idx = old_volume->source.volume_idx;
                assert(!old_volume->source.is_converted_from_inches || !old_volume->source.is_converted_from_meters);
                if (old_volume->source.is_converted_from_inches)
                    new_volume->convert_from_imperial_units();
                else if (old_volume->source.is_converted_from_meters)
                    new_volume->convert_from_meters();
                std::swap(old_model_object->volumes[vol_idx], old_model_object->volumes.back());
                old_model_object->delete_volume(old_model_object->volumes.size() - 1);
                if (!sinking) old_model_object->ensure_on_bed();
                old_model_object->sort_volumes(m_q->app_config()->get("order_volumes") == "1");

                sla::reproject_points_and_holes(old_model_object);

                // Fix warning icon in object list
                wxGetApp().obj_list()->update_item_error_icon(obj_idx, vol_idx);
            }
        }
#else
        // update the selected volumes whose source is the current file
        for (const SelectedVolume& sel_v : selected_volumes) {
            ModelObject* old_model_object = m_model->objects[sel_v.object_idx];
            ModelVolume* old_volume = old_model_object->volumes[sel_v.volume_idx];

            bool sinking = old_model_object->bounding_box().min.z() < SINKING_Z_THRESHOLD;

            bool has_source = !old_volume->source.input_file.empty() && boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (has_source || has_name) {
                int new_volume_idx = -1;
                int new_object_idx = -1;
//                if (has_source) {
//                    // take idxs from source
//                    new_volume_idx = old_volume->source.volume_idx;
//                    new_object_idx = old_volume->source.object_idx;
//                }
//                else {
                    // take idxs from the 1st matching volume
                    for (size_t o = 0; o < new_model.objects.size(); ++o) {
                        ModelObject* obj = new_model.objects[o];
                        bool found = false;
                        for (size_t v = 0; v < obj->volumes.size(); ++v) {
                            if (obj->volumes[v]->name == old_volume->name) {
                                new_volume_idx = (int)v;
                                new_object_idx = (int)o;
                                found = true;
                                break;
                            }
                        }
                        if (found)
                            break;
                    }
//                }

                if (new_object_idx < 0 || int(new_model.objects.size()) <= new_object_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                ModelObject* new_model_object = new_model.objects[new_object_idx];
                if (new_volume_idx < 0 || int(new_model_object->volumes.size()) <= new_volume_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }

                old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
                ModelVolume* new_volume = old_model_object->volumes.back();
                new_volume->set_new_unique_id();
                new_volume->config.apply(old_volume->config);
                new_volume->set_type(old_volume->type());
                new_volume->set_material_id(old_volume->material_id());
                new_volume->set_transformation(old_volume->get_transformation());
                new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
                new_volume->source.object_idx = old_volume->source.object_idx;
                new_volume->source.volume_idx = old_volume->source.volume_idx;
                assert(! old_volume->source.is_converted_from_inches || ! old_volume->source.is_converted_from_meters);
                if (old_volume->source.is_converted_from_inches)
                    new_volume->convert_from_imperial_units();
                else if (old_volume->source.is_converted_from_meters)
                    new_volume->convert_from_meters();
                std::swap(old_model_object->volumes[sel_v.volume_idx], old_model_object->volumes.back());
                old_model_object->delete_volume(old_model_object->volumes.size() - 1);
                if (!sinking)
                    old_model_object->ensure_on_bed();
                old_model_object->sort_volumes(true);

                sla::reproject_points_and_holes(old_model_object);
            }
        }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
    }

#if ENABLE_RELOAD_FROM_DISK_REWORK
    for (auto [src, dest] : replace_paths) {
        for (auto [obj_idx, vol_idx] : selected_volumes) {
            if (boost::algorithm::iequals(m_model->objects[obj_idx]->volumes[vol_idx]->source.input_file, src.string()))
                // When an error occurs, either the dest parsing error occurs, or the number of objects in the dest is greater than 1 and cannot be replaced, and cannot be replaced in this loop.
                if (!replace_volume_with_stl(obj_idx, vol_idx, dest, "")) break;
        }
    }
#else
    for (size_t i = 0; i < replace_paths.size(); ++i) {
        const auto& path = replace_paths[i].string();
        for (const SelectedVolume& sel_v : selected_volumes) {
            ModelObject* old_model_object = m_model->objects[sel_v.object_idx];
            ModelVolume* old_volume = old_model_object->volumes[sel_v.volume_idx];
            bool has_source = !old_volume->source.input_file.empty() && boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            if (!replace_volume_with_stl(sel_v.object_idx, sel_v.volume_idx, path, "")) {
                fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
            }
        }
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    if (!fail_list.empty()) {
        wxString message = _L("Unable to reload:") + "\n";
        for (const wxString& s : fail_list) {
            message += s + "\n";
        }
        MessageDialog dlg(m_q, message, _L("Error during reload"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
    }

    // update 3D scene
    m_refresh->update(0);   // 原 priv::update()（默认 flags=0）

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < m_model->objects.size(); ++i) {
        m_layout->view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " finish.";
}



void ProjectSession::on_action_open_project()
{
    load_project();
}

void ProjectSession::on_action_request_model_id(const wxString& model_id)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received import model id event\n";
    import_model_id(model_id);
}

void ProjectSession::on_action_download_project(const wxString& project_id)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received download project event\n";
    download_project(project_id);
}

void ProjectSession::show_preview_only_hint()
{
    m_hub.nm()->bbl_show_preview_only_notification(into_u8(_L("Preview only mode:\nThe loaded file contains G-code only, cannot enter the Prepare page.")));
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
