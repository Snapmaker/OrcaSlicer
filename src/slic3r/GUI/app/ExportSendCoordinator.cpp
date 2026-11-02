// ExportSendCoordinator — Plater 导出/发送/发布编排服务（阶段 E2 纯搬移，手册《Plater阶段E2-导出发送ExportSendCoordinator抽取实施方案》）。
// 依赖规则（AGENTS.md）：本文件禁止 include Plater/GUI_App 宿主头（护栏 #1，CI grep）；
// q 与 priv 上下文只能经 ExportSendCoordinatorHost 进入；FileType/file_wildcards/EVT_PUBLISHING_START 困头约束见 hpp 头注（design D2）。
#include "ExportSendCoordinator.hpp"
#include "../Plater.hpp"        // H1 2.3: F1/F3 q faces + publicized export faces + publish_model bridge
#include "SlicingController.hpp" // H1 2.3: typed inject SlicingController&
#include "NotificationHub.hpp"    // H1 2.3: m_hub.nm() 完整型（b23_2 C2027 x9）
#include "SelectionQuery.hpp"    // H1 2.3: late_bind selq (selection)
#include "LayoutManager.hpp"     // H1 2.3: late_bind layout (view3d/preview/is_preview_loaded)
#include "SceneRefreshCoordinator.hpp"  // H1 2.3: late_bind refresh (update_print_volume_state)
#include "CalibrationFlow.hpp"   // H1 2.3: late_bind calib (record_start_print_preset)

#include "ProjectSession.hpp"                // m_session 解引用（m_project_folder/m_project_name/get_project_filename）
#include "../I18N.hpp"                       // _L/_u8L（文件名推导 Untitled 回退、combine_mesh 失败文案）
#include "../GUI.hpp"                        // into_u8/into_path/from_u8/from_path/show_error（E1 cpp 同款）
#include "../format.hpp"                     // GUI::format（send_gcode_finish 完成文案）
#include "../BackgroundSlicingProcess.hpp"   // background_process 槽解引用（show_publish_dialog 取消分支 running/stop）
#include "../MainFrame.hpp"                  // 仅 cpp（手册 §3）：main_frame 槽解引用（select_tab/TabPosition）
#include "../GUI_Preview.hpp"                // View3D/Preview 完整类型（view3d/preview 槽解引用，E1 cpp 同款）
#include "../RemovableDriveManager.hpp"      // RemovableDriveManager 槽解引用（U 盘判定/回写）
#include "../Camera.hpp"                     // Camera::EType（export_3mf 缩略图槽 int 桥接还原）
#include "../GLToolbar.hpp"                   // EVT_GLTOOLBAR_SLICE_ALL + SimpleEvent（slice_all 槽逐字保留）
#include "libslic3r/TriangleMesh.hpp"        // TriangleMesh（combine_mesh_fff/export_stl）
#include "libslic3r/MeshBoolean.hpp"         // MeshBoolean::mcut/cgal（combine_mesh_fff/export_stl 布尔）
#include "libslic3r/CSGMesh/ModelToCSGMesh.hpp"  // csg::model_to_csgmesh（Plater.cpp:108 同款）
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"  // csg::check/perform_csgmesh_booleans（Plater.cpp:109 同款）
#include "libslic3r/Format/stl.hpp"          // Slic3r::store_stl（export_stl 落盘）
#include "libslic3r/SLAPrint.hpp"            // SLAPrint/SLAPrintObject（export_stl SLA 分支）
#include "libslic3r/AppConfig.hpp"           // AppConfig 完整类型（get/update_last_output_dir/get_devices）
#include "../NotificationManager.hpp"        // NotificationManager 完整类型（host 槽解引用）；传递可见 FileType/file_wildcards（E1 同款链）
#include "../Selection.hpp"                  // Selection（get_export_file_path/export_stl 选中推导）
#include "../PartPlate.hpp"                  // PartPlateList/PartPlate（get_export_gcode_filename 盘名/盘号）
#include "libslic3r/Model.hpp"               // Model/ModelObject（导出文件名推导/combine_mesh_fff）
#include "libslic3r/Utils.hpp"               // is_shapes_dir
#include <boost/format.hpp>                  // get_export_gcode_filename 盘号格式化
#include <boost/algorithm/string.hpp>        // boost::iequals（get_export_file __WXMSW__ 分支）
#include <wx/filedlg.h>                      // wxFileDialog（get_export_file；本 cpp 退出 PCH，wx 显式包含）
#include "../SelectMachine.hpp"              // SelectMachineDialog 完整类型（物理持有 + handle_print_* 解引用；E2 头文件解 Plater 后可达）
#include "../SendMultiMachinePage.hpp"       // SendMultiMachinePage
#include "../SendToPrinter.hpp"              // SendToPrinterDialog
#include "../PublishDialog.hpp"              // PublishDialog（publishing_*/show_publish_dialog 解引用）
#include "../PrintHostDialogs.hpp"           // PrintHostSendDialog（send_gcode_legacy 上传对话框）
#include "../WebPreprintDialog.hpp"          // WebPreprintDialog（新 Connect/U1 上传流）
#include "../MsgDialog.hpp"                  // GUI::MessageDialog（PrusaConnect 打印确认）
#include "../../Utils/ASCIIFolding.hpp"      // Slic3r::fold_utf8_to_ascii（上传文件名折叠）
#include "../CalibrationWizard.hpp"          // CalibrationWizard + EVT_CALIBRATION_JOB_FINISHED（send_calibration_job_finished 路由）
#include "../Monitor.hpp"                    // MonitorPanel（print_job_finished 跳转）
#include "../ReleaseNote.hpp"                // UpdatePluginDialog（update_plugin_when_launch）

namespace Slic3r {
namespace GUI {
namespace app {

// update_background_process 返回位常量（cpp-local 镜像：priv:1071-1081 与 SlicingController.cpp:38 各持同名拷贝；
// EX 镜像值以 priv 版为准——int 桥接 cpp-local enum 的 D2 过渡槽先例（先例槽 prepare_state_facade 已随 F3 删除）
enum {
    UPDATE_BACKGROUND_PROCESS_RESTART       = 1,
    UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE = 2,
    UPDATE_BACKGROUND_PROCESS_INVALID       = 4,
    UPDATE_BACKGROUND_PROCESS_FORCE_RESTART = 8,
    UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT  = 16,
};

ExportSendCoordinator::ExportSendCoordinator(Plater* q, Model* model, DynamicPrintConfig* config, app::ProjectSession& session,
                      SlicingController& slicing, PartPlateList& partplate_list, NotificationHub& hub, MainFrame* main_frame)
    : q(q), model(model), config(config), m_session(session)
    , m_slicing(slicing)
    , m_partplate_list(partplate_list)
    , m_hub(hub)
    , m_main_frame(main_frame)
{
}

void ExportSendCoordinator::late_bind(SelectionQuery& selq, LayoutManager& layout, SceneRefreshCoordinator& refresh, CalibrationFlow& calib)
{
    m_selq    = &selq;
    m_layout  = &layout;
    m_refresh = &refresh;
    m_calib   = &calib;
}

ExportSendCoordinator::~ExportSendCoordinator()
{
    // 四对话框以 q 为 wx 父，随 Plater 生命周期销毁（与原 priv 裸指针成员语义一致，此处不 delete）
}

// —— 2.3 可测核心（design D7）——

// join_gcode_filename：自 priv::get_export_gcode_filename（:4549）分支结构抽取的拼装纯函数，行为逐字对照。
// plate_index_str 由调用方算好传入（wxString，含前导 '_'，原 std::string 经 from_u8 的转换移至调用方）；
// folder 空分支的 model 依赖段（objects 非空判定 + 对象名去扩展名）经 untitled_object_name 注入——
// 调用方约定：objects 非空时传去扩展名对象名（可为空串），为空时传空串（空串即无回退候选）。
wxString ExportSendCoordinator::join_gcode_filename(const boost::filesystem::path& project_folder, const wxString& project_name,
                                                    const wxString& plate_index_str, const wxString& extension,
                                                    bool only_filename, bool export_all,
                                                    const wxString& untitled_object_name)
{
    if (!project_folder.empty()) {
        if (!only_filename) {
            if (export_all) {
                auto full_filename = project_folder / std::string((project_name + extension).mb_str(wxConvUTF8));
                return from_path(full_filename);
            } else {
                auto full_filename = project_folder / std::string((project_name + plate_index_str + extension).mb_str(wxConvUTF8));
                return from_path(full_filename);
            }
        } else {
            if (export_all)
                return project_name + extension;
            else
                return project_name + plate_index_str + extension;
        }
    } else {
        if (only_filename) {
            if (!untitled_object_name.empty() && project_name == _L("Untitled"))
                return untitled_object_name + plate_index_str + extension;

            if (export_all)
                return project_name + extension;
            else
                return project_name + plate_index_str + extension;
        }
        else
            return "";
    }
}

// exporting 状态谓词组（原 priv::exporting_status :1227 读写；int 桥接供 slicing_host 改接，design D4）
bool ExportSendCoordinator::exporting_active() const { return m_exporting_status != ExportingStatus::NOT_EXPORTING; }
bool ExportSendCoordinator::is_exporting_to_removable() const { return m_exporting_status == ExportingStatus::EXPORTING_TO_REMOVABLE; }
bool ExportSendCoordinator::is_exporting_to_local() const { return m_exporting_status == ExportingStatus::EXPORTING_TO_LOCAL; }
bool ExportSendCoordinator::is_not_exporting() const { return m_exporting_status == ExportingStatus::NOT_EXPORTING; }
bool ExportSendCoordinator::exporting_to_removable() const { return m_exporting_status == ExportingStatus::EXPORTING_TO_REMOVABLE; }
bool ExportSendCoordinator::exporting_to_local() const { return m_exporting_status == ExportingStatus::EXPORTING_TO_LOCAL; }
void ExportSendCoordinator::set_not_exporting() { m_exporting_status = ExportingStatus::NOT_EXPORTING; }
void ExportSendCoordinator::set_exporting_status(int status) { m_exporting_status = (ExportingStatus)status; }
bool ExportSendCoordinator::is_publishing() const { return m_is_publishing; }
void ExportSendCoordinator::set_is_publishing(bool v) { m_is_publishing = v; }

// —— 3.1 路径推导组（手册 §2.1 表 1/2/4；原 priv::get_export_file_path :2542 / get_export_file :2590 / get_export_gcode_filename :4549）——

boost::filesystem::path ExportSendCoordinator::get_export_file_path(int file_type)
{
    const GUI::FileType ft = (GUI::FileType)file_type;   // int 承载 GUI::FileType（design D2；枚举可见性经 NotificationManager.hpp 链，E1 同款）
    // Update printbility state of each of the ModelInstances.
    m_refresh->update_print_volume_state();

    const Selection& selection = m_selq->selection();
    int obj_idx = selection.get_object_idx();

    fs::path output_file;
    if (ft == FT_3MF)
        // for 3mf take the path from the project filename, if any
        output_file = into_path(m_session.get_project_filename(".3mf"));
    else if (ft == FT_STL) {
        if (obj_idx > 0 && obj_idx < model->objects.size() && selection.is_single_full_object()) {
            output_file = model->objects[obj_idx]->get_export_filename();
        }
        else {
            output_file = into_path(m_session.get_project_name());
        }
    }
    //bbs  name the project using the part name
    if (output_file.empty()) {
        if (m_session.get_project_name() != _L("Untitled")) {
            output_file = into_path(m_session.get_project_name() + ".3mf");
        }
    }

    if (output_file.empty())
    {
        // first try to get the file name from the current selection
        if ((0 <= obj_idx) && (obj_idx < (int)model->objects.size()))
            output_file = model->objects[obj_idx]->get_export_filename();

        if (output_file.empty())
            // Find the file name of the first printable object.
            output_file = model->propose_export_file_name_and_path();

        if (output_file.empty() && !model->objects.empty())
            // Find the file name of the first object.
            output_file = model->objects[0]->get_export_filename();

        if (output_file.empty())
            // Use _L("Untitled") name
            output_file = into_path(_L("Untitled"));
    }
    return output_file;
}

wxString ExportSendCoordinator::get_export_file(int file_type)
{
    const GUI::FileType ftype = (GUI::FileType)file_type;   // design D2
    wxString wildcard;
    switch (ftype) {
        case FT_STL:
        case FT_AMF:
        case FT_3MF:
        case FT_GCODE:
        case FT_OBJ:
            wildcard = file_wildcards(ftype);
        break;
        default:
            wildcard = file_wildcards(FT_MODEL);
        break;
    }

    fs::path output_file = get_export_file_path(file_type);

    wxString dlg_title;
    switch (ftype) {
        case FT_STL:
        {
            output_file.replace_extension("stl");
            dlg_title = _L("Export STL file:");
            break;
        }
        case FT_AMF:
        {
            // XXX: Problem on OS X with double extension?
            output_file.replace_extension("zip.amf");
            dlg_title = _L("Export AMF file:");
            break;
        }
        case FT_3MF:
        {
            output_file.replace_extension("3mf");
            dlg_title = _L("Save file as:");
            break;
        }
        case FT_OBJ:
        {
            output_file.replace_extension("obj");
            dlg_title = _L("Export OBJ file:");
            break;
        }
        default: break;
    }

    std::string out_dir = (boost::filesystem::path(output_file).parent_path()).string();

    wxFileDialog dlg(q, dlg_title,
        is_shapes_dir(out_dir) ? from_u8(q->app_config()->get_last_dir()) : from_path(output_file.parent_path()), from_path(output_file.filename()),
        wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxPD_APP_MODAL);

    int result = dlg.ShowModal();
    if (result == wxID_CANCEL)
        return "<cancel>";
    if (result != wxID_OK)
        return wxEmptyString;

    wxString out_path = dlg.GetPath();
    fs::path path(into_path(out_path));
#ifdef __WXMSW__
    if (boost::iequals(path.extension().string(), output_file.extension().string()) == false) {
        out_path += output_file.extension().string();
        boost::system::error_code ec;
        if (boost::filesystem::exists(into_u8(out_path), ec)) {
            auto result = MessageBox(q->GetHandle(),
                wxString::Format(_L("The file %s already exists\nDo you want to replace it?"), out_path),
                _L("Confirm Save As"),
                MB_YESNO | MB_ICONWARNING);
            if (result != IDYES)
                return wxEmptyString;
        }
    }
#endif
    q->app_config()->update_last_output_dir(path.parent_path().string());

    return out_path;
}

wxString ExportSendCoordinator::get_export_gcode_filename(const wxString& extension, bool only_filename, bool export_all) const
{
    std::string plate_index_str;
    auto plate_name = m_partplate_list.get_curr_plate()->get_plate_name();
    if (!plate_name.empty())
        plate_index_str = (boost::format("_%1%") % plate_name).str();
    else if (m_partplate_list.get_plate_count() > 1)
        plate_index_str = (boost::format("_plate_%1%") % std::to_string(m_partplate_list.get_curr_plate_index() + 1)).str();

    // folder 空分支的 model 依赖段（design D7）：objects 非空时注入去扩展名对象名（调用方约定见 join_gcode_filename 注释）
    wxString untitled_object_name;
    if (!model->objects.empty())
        untitled_object_name = wxString(fs::path(model->objects.front()->name).replace_extension().c_str());

    return join_gcode_filename(m_session.m_project_folder, m_session.m_project_name, from_u8(plate_index_str), extension,
                               only_filename, export_all, untitled_object_name);
}

// —— 3.3/3.5 打印与发送组（手册 §2.1 表 8-12；原 priv::on_action_print_* :3857-3900 / on_action_send_to_printer :3958 /
//      Plater::send_to_printer :6798 / send_gcode :7872 / send_gcode_legacy :7656 / export_config_3mf / send_gcode_finish / get_print_job_data）——

void ExportSendCoordinator::handle_print_plate()
{
    PresetBundle& preset_bundle = *q->preset_bundle();
    if (preset_bundle.use_bbl_network()) {
        // BBS
        if (!m_select_machine_dlg)
            m_select_machine_dlg = new SelectMachineDialog(q);
        m_select_machine_dlg->set_print_type(PrintFromType::FROM_NORMAL);
        m_select_machine_dlg->prepare(m_partplate_list.get_curr_plate_index());
        m_select_machine_dlg->ShowModal();
        m_calib->record_start_print_preset("print_plate");
    } else {
        send_gcode_legacy(PLATE_CURRENT_IDX, nullptr, true);
    }
}

void ExportSendCoordinator::handle_send_to_multi_machine()
{
    if (!m_send_multi_dlg)
        m_send_multi_dlg = new SendMultiMachinePage(q);
    m_send_multi_dlg->prepare(m_partplate_list.get_curr_plate_index());
    m_send_multi_dlg->ShowModal();
}

void ExportSendCoordinator::handle_print_plate_from_sdcard()
{
    //BBS
    if (!m_select_machine_dlg) m_select_machine_dlg = new SelectMachineDialog(q);
    m_select_machine_dlg->set_print_type(PrintFromType::FROM_SDCARD_VIEW);
    m_select_machine_dlg->prepare(0);
    m_select_machine_dlg->ShowModal();
}

void ExportSendCoordinator::handle_print_all()
{
    PresetBundle& preset_bundle = *q->preset_bundle();
    if (preset_bundle.use_bbl_network()) {
        // BBS
        if (!m_select_machine_dlg)
            m_select_machine_dlg = new SelectMachineDialog(q);
        m_select_machine_dlg->set_print_type(PrintFromType::FROM_NORMAL);
        m_select_machine_dlg->prepare(PLATE_ALL_IDX);
        m_select_machine_dlg->ShowModal();
        m_calib->record_start_print_preset("print_all");
    } else {
        send_gcode_legacy(PLATE_ALL_IDX, nullptr, true);
    }
}

void ExportSendCoordinator::send_to_printer(bool isall)
{
    if (!m_send_to_sdcard_dlg) m_send_to_sdcard_dlg = new SendToPrinterDialog(q);
    if (isall) {
        m_send_to_sdcard_dlg->prepare(PLATE_ALL_IDX);
    }
    else {
        m_send_to_sdcard_dlg->prepare(m_partplate_list.get_curr_plate_index());
    }

    m_send_to_sdcard_dlg->ShowModal();
}

void ExportSendCoordinator::send_gcode_finish(wxString name)
{
    auto out_str = GUI::format(_L("The file %s has been sent to the printer's storage space and can be viewed on the printer."), name);
    m_hub.nm()->push_exporting_finished_notification(out_str, "", false);
}

void ExportSendCoordinator::get_print_job_data(PrintPrepareData* data)
{
    if (data) {
        data->plate_idx = m_print_job_data.plate_idx;
        data->_3mf_path = m_print_job_data._3mf_path;
        data->_3mf_config_path = m_print_job_data._3mf_config_path;
    }
}

int ExportSendCoordinator::send_gcode(int plate_idx, Export3mfProgressFn proFn)
{
    int result = 0;
    /* generate 3mf */
    if (plate_idx == PLATE_CURRENT_IDX) {
        m_print_job_data.plate_idx = m_partplate_list.get_curr_plate_index();
    }
    else {
        m_print_job_data.plate_idx = plate_idx;
    }

    PartPlate* plate = m_partplate_list.get_curr_plate();
    try {
        m_print_job_data._3mf_path = fs::path(plate->get_tmp_gcode_path());
        m_print_job_data._3mf_path.replace_extension("3mf");
    }
    catch (std::exception&) {
        BOOST_LOG_TRIVIAL(error) << "generate 3mf path failed";
        return -1;
    }

    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithGcode | SaveStrategy::SkipAuxiliary;
#if !BBL_RELEASE_TO_PUBLIC
    //only save model in QA environment
    std::string sel = q->app_config()->get("iot_environment");
    if (sel == ENV_PRE_HOST)
        strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode;
#endif

    result = export_3mf(m_print_job_data._3mf_path, strategy, plate_idx, proFn);

    return result;
}

int ExportSendCoordinator::export_config_3mf(int plate_idx, Export3mfProgressFn proFn)
{
    int result = 0;
    /* generate 3mf */
    if (plate_idx == PLATE_CURRENT_IDX) {
        m_print_job_data.plate_idx = m_partplate_list.get_curr_plate_index();
    }
    else {
        m_print_job_data.plate_idx = plate_idx;
    }

    PartPlate* plate = m_partplate_list.get_curr_plate();
    try {
        m_print_job_data._3mf_config_path = fs::path(plate->get_temp_config_3mf_path());
    }
    catch (std::exception&) {
        BOOST_LOG_TRIVIAL(error) << "generate 3mf path failed";
        return -1;
    }

    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithSliceInfo | SaveStrategy::SkipAuxiliary;
    result = export_3mf(m_print_job_data._3mf_config_path, strategy, plate_idx, proFn);

    return result;
}

void ExportSendCoordinator::send_gcode_legacy(int plate_idx, Export3mfProgressFn proFn, bool use_3mf)
{
    // if physical_printer is selected, send gcode for this printer
    // DynamicPrintConfig* physical_printer_config = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();

    auto prepare_upload_filename_for_dialog = [this, use_3mf](fs::path output_file) {
        output_file = fs::path(Slic3r::fold_utf8_to_ascii(output_file.string()));
        if (use_3mf)
            output_file.replace_extension("3mf");

        PartPlate *current_plate = m_partplate_list.get_curr_plate();
        if (current_plate != nullptr) {
            const Print *current_print = current_plate->fff_print();
            if (current_print != nullptr && !current_print->print_statistics().estimated_normal_print_time.empty())
                return fs::path(current_print->print_statistics().finalize_output_path(output_file.string()));
        }

        if (current_plate != nullptr && current_plate->is_slice_result_valid() && current_plate->get_slice_result() != nullptr) {
            const auto &estimated_stats = current_plate->get_slice_result()->print_statistics;
            const float normal_time = estimated_stats.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
            if (normal_time > 0.0f) {
                std::string filename = output_file.string();
                const std::string normal_time_str = short_time(get_time_dhms(normal_time));
                boost::replace_all(filename, "{print_time}", normal_time_str);
                boost::replace_all(filename, "{normal_print_time}", normal_time_str);

                const float silent_time = estimated_stats.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
                if (silent_time > 0.0f)
                    boost::replace_all(filename, "{silent_print_time}", short_time(get_time_dhms(silent_time)));

                output_file = fs::path(filename);
            }
        }

        return output_file;
    };

    // 校验机型
    auto devices = q->app_config()->get_devices();
    std::string connect_preset = "";
    for (const auto device : devices) {
        if (device.connected) {
            connect_preset = device.preset_name;
        }
    }

    auto current_preset = q->preset_bundle()->printers.get_edited_preset();

    bool islegal = true;
    std::string c_preset = "";
    if (current_preset.is_system) {
        c_preset = current_preset.name;
    } else {
        auto base_preset = q->preset_bundle()->printers.get_preset_base(current_preset);
        c_preset         = base_preset->name;
    }

    c_preset.erase(std::remove(c_preset.begin(), c_preset.end(), '('), c_preset.end());
    c_preset.erase(std::remove(c_preset.begin(), c_preset.end(), ')'), c_preset.end());

    connect_preset.erase(std::remove(connect_preset.begin(), connect_preset.end(), '('), connect_preset.end());
    connect_preset.erase(std::remove(connect_preset.begin(), connect_preset.end(), ')'), connect_preset.end());

    islegal = (c_preset == connect_preset);

    DynamicPrintConfig* physical_printer_config = &q->preset_bundle()->printers.get_edited_preset().config;
    if (! physical_printer_config || model->objects.empty())
        return;

    PrintHostJob upload_job;

    // Snapmaker U1
    const auto preset = q->preset_bundle()->printers.get_edited_preset();
    auto       printer_config    = q->preset_bundle()->printers.get_edited_preset().config;
    auto       printer_model_opt = printer_config.option<ConfigOptionString>("printer_model");
    bool       is_snapmaker_u1   = false;
    if (printer_model_opt) {
        std::string printer_model = printer_model_opt->value;
        is_snapmaker_u1           = boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");
    }

    if (q->app_config()->get("use_new_connect") == "true" || is_snapmaker_u1) {
        // firstly upload and open upload download dialog,
        // get default name
        // Obtain default output path
        fs::path default_output_file;
        try {
            // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
            // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
            unsigned int state = q->update_restart_background_process(false, false);
            if (state & UPDATE_BACKGROUND_PROCESS_INVALID)
                return;
            default_output_file = m_slicing.background_process().output_filepath_for_project("");
        } catch (const Slic3r::PlaceholderParserError& ex) {
            // Show the error with monospaced font.
            show_error(q, ex.what(), true);
            return;
        } catch (const std::exception& ex) {
            show_error(q, ex.what(), false);
            return;
        }
        default_output_file = prepare_upload_filename_for_dialog(std::move(default_output_file));

        // get file path
        auto file_path = m_partplate_list.get_curr_plate()->get_tmp_gcode_path();
        upload_job.upload_data.source_path = file_path;
        upload_job.upload_data.upload_path = default_output_file;

        // upload or print
        // Repetier specific: Query the server for the list of file groups.
        wxArrayString groups;

        // PrusaLink specific: Query the server for the list of file groups.
        wxArrayString storage_paths;
        wxArrayString storage_names;

        auto                config = q->app_config();
        PrintHostSendDialog dlg(default_output_file, PrintHostPostUploadAction::StartPrint, groups, storage_paths, storage_names,
                                config->get_bool("open_device_tab_post_upload"));
        dlg.init();
        if (dlg.ShowModal() == wxID_CANCEL) {
            return;
        }
        config->set_bool("open_device_tab_post_upload", dlg.switch_to_device_tab());
        upload_job.switch_to_device_tab    = dlg.switch_to_device_tab();
        upload_job.upload_data.upload_path = dlg.filename();
        upload_job.upload_data.post_action = dlg.post_action();
        upload_job.upload_data.group       = dlg.group();
        upload_job.upload_data.storage     = dlg.storage();


        WebPreprintDialog* dialog = new WebPreprintDialog();
        dialog->set_swtich_to_device(dlg.switch_to_device_tab());
        dialog->set_send_page(dlg.post_action() == PrintHostPostUploadAction::None);
        dialog->set_gcode_file_name(upload_job.upload_data.source_path.string());
        dialog->set_display_file_name(upload_job.upload_data.upload_path.string());
        bool res = dialog->run();

        if (dialog->is_finish()) {
            m_main_frame->select_tab(MainFrame::TabPosition::tpMonitor);
        }

        // Use wx deferred destruction instead of a direct delete: the dialog was
        // just returning from its modal loop, and the destructor clears the
        // global registration. Clear it now so nothing can touch the pending
        // window before wx actually deletes it (Sentry SNAPMAKER_ORCA-21F).
        dialog->Destroy();
        wxGetApp().set_web_preprint_dialog(nullptr);

        return;
    }
    else {
        upload_job = PrintHostJob(physical_printer_config);
    }

    if (upload_job.empty())
        return;

    upload_job.upload_data.use_3mf = use_3mf;

    // Obtain default output path
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = q->update_restart_background_process(false, false);
        if (state & UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = m_slicing.background_process().output_filepath_for_project("");
    } catch (const Slic3r::PlaceholderParserError& ex) {
        // Show the error with monospaced font.
        show_error(q, ex.what(), true);
        return;
    } catch (const std::exception& ex) {
        show_error(q, ex.what(), false);
        return;
    }
    default_output_file = prepare_upload_filename_for_dialog(std::move(default_output_file));

    // Repetier specific: Query the server for the list of file groups.
    wxArrayString groups;
    {
        wxBusyCursor wait;
        upload_job.printhost->get_groups(groups);
    }

    // PrusaLink specific: Query the server for the list of file groups.
    wxArrayString storage_paths;
    wxArrayString storage_names;
    {
        wxBusyCursor wait;
        try {
            upload_job.printhost->get_storage(storage_paths, storage_names);
        } catch (const Slic3r::IOError& ex) {
            show_error(q, ex.what(), false);
            return;
        }
    }

    auto config = q->app_config();
    PrintHostSendDialog dlg(default_output_file, upload_job.printhost->get_post_upload_actions(), groups, storage_paths, storage_names, config->get_bool("open_device_tab_post_upload"));
    dlg.init();
    if (dlg.ShowModal() == wxID_OK) {
        config->set_bool("open_device_tab_post_upload", dlg.switch_to_device_tab());
        upload_job.switch_to_device_tab    = dlg.switch_to_device_tab();
        upload_job.upload_data.upload_path = dlg.filename();
        upload_job.upload_data.post_action = dlg.post_action();
        upload_job.upload_data.group       = dlg.group();
        upload_job.upload_data.storage     = dlg.storage();

        // Show "Is printer clean" dialog for PrusaConnect - Upload and print.
        if (std::string(upload_job.printhost->get_name()) == "PrusaConnect" && upload_job.upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
            GUI::MessageDialog dlg(nullptr, _L("Is the printer ready? Is the print sheet in place, empty and clean?"), _L("Upload and Print"), wxOK | wxCANCEL);
            if (dlg.ShowModal() != wxID_OK)
                return;
        }

        if (use_3mf) {
            // Process gcode
            const int result = send_gcode(plate_idx, nullptr);

            if (result < 0) {
                wxString msg = _L("Abnormal print file data. Please slice again");
                show_error(q, msg, false);
                return;
            }

            upload_job.upload_data.source_path = m_print_job_data._3mf_path;

        }

        export_gcode(fs::path(), false, std::move(upload_job));
    }
}

// —— 3.4 发布组（手册 §2.1；原 priv::on_action_publish :3911 / show_publish_dlg :4916 / update_publish_dialog_status :4910 /
//      Plater::publish_project :7662 空壳 / show_publish_dialog :10114；publishing_* 谓词供 slicing_host 改接消费）——

void ExportSendCoordinator::publish_project()
{
    // 空壳随迁（勘误 2：E1 实测空函数；SlicingController :704 调用点行为不变，是否删除留 G 复核）
    return;
}

bool ExportSendCoordinator::show_publish_dialog(bool show)
{
    if (q != nullptr) { BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":recevied publish event\n"; }

    if (!m_publish_dlg) m_publish_dlg = new PublishDialog(q);
    if (show) {
        m_publish_dlg->reset();
        m_publish_dlg->start_slicing();
        //m_publish_dlg->Show();
        m_publish_dlg->ShowModal();
    } else {
        m_publish_dlg->EndModal(wxID_OK);
        //cancel the slicing
        if (m_slicing.background_process().running())
            m_slicing.background_process().stop();
    }
    return true;
}

void ExportSendCoordinator::handle_publish(bool publishing_start)
{
    if (true) {
        if (publishing_start) {
            // update by background slicing process
            if (q->get_process_completed_with_error() >= 0) {
                wxString msg = _L("Please resolve the slicing errors and publish again.");
                this->m_publish_dlg->UpdateStatus(msg, false);
                return;
            }

            m_is_publishing = true;
            // if slicing is ready publish project, else slicing first
            if (m_partplate_list.is_all_slice_results_valid()) {
                publish_project();
            } else {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice project in background event\n";
                { SimpleEvent evt(EVT_GLTOOLBAR_SLICE_ALL); m_slicing.on_action_slice_all(); };
            }
        } else {
            m_is_publishing = false;
            show_publish_dialog(false);
        }
    }
}

void ExportSendCoordinator::update_publish_dialog_status(wxString& msg, int percent)
{
    if (m_publish_dlg)
        m_publish_dlg->UpdateStatus(msg, percent);
}

void ExportSendCoordinator::publishing_update_status(const wxString& msg, int percent)
{
    wxString m = msg;
    m_publish_dlg->UpdateStatus(m, percent, false);
}

void ExportSendCoordinator::publishing_cancel()
{
    m_publish_dlg->cancel();
}

bool ExportSendCoordinator::publishing_dlg_active() const
{
    return m_publish_dlg && !m_publish_dlg->was_cancelled();
}

bool ExportSendCoordinator::publish_dialog_shown() const
{
    return m_publish_dlg->IsShown();
}

SelectMachineDialog* ExportSendCoordinator::select_machine_dlg()
{
    if (!m_select_machine_dlg) m_select_machine_dlg = new SelectMachineDialog(q);
    return m_select_machine_dlg;
}

// —— 3.2 导出组 Part 1（手册 §2.1 表 3/导出 gcode 行；原 priv::export_gcode :3141/:3174、Plater::export_gcode :6620、
//      export_gcode_3mf :6725、export_core_3mf :6808）——

void ExportSendCoordinator::export_gcode(fs::path output_path, bool output_path_on_removable_media)
{
    wxCHECK_RET(!(output_path.empty()), "export_gcode: output_path and upload_job empty");

    BOOST_LOG_TRIVIAL(trace) << boost::format("export_gcode: output_path %1%")%output_path.string();
    if (model->objects.empty())
        return;

    if (m_slicing.background_process().is_export_scheduled()) {
        GUI::show_error(q, _L("Another export job is running."));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = q->update_background_process(true);
    if (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        m_layout->view3D->reload_scene(false);

    if ((state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    q->set_show_warning_dialog(true);
    if (! output_path.empty()) {
        m_slicing.background_process().schedule_export(output_path.string(), output_path_on_removable_media);
        m_hub.nm()->push_delayed_notification(NotificationType::ExportOngoing, []() {return true; }, 1000, 0);
    } else {
        BOOST_LOG_TRIVIAL(info) << "output_path  is empty";
    }

    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    m_slicing.background_process().set_task(PrintBase::TaskParams());
    q->restart_background_process(UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}

void ExportSendCoordinator::export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job)
{
    wxCHECK_RET(!(output_path.empty() && upload_job.empty()), "export_gcode: output_path and upload_job empty");

    if (model->objects.empty())
        return;

    if (m_slicing.background_process().is_export_scheduled()) {
        GUI::show_error(q, _L("Another export job is running."));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = q->update_background_process(true);
    if (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        m_layout->view3D->reload_scene(false);

    if ((state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    q->set_show_warning_dialog(true);
    if (! output_path.empty()) {
        m_slicing.background_process().schedule_export(output_path.string(), output_path_on_removable_media);
        m_hub.nm()->push_delayed_notification(NotificationType::ExportOngoing, []() {return true; }, 1000, 0);
    } else {
        m_slicing.background_process().schedule_upload(std::move(upload_job));
    }

    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    m_slicing.background_process().set_task(PrintBase::TaskParams());
    q->restart_background_process(UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}

void ExportSendCoordinator::export_gcode(bool prefer_removable)
{
    if (model->objects.empty())
        return;

    //if (get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
    //    return;

    if (q->get_process_completed_with_error() == m_partplate_list.get_curr_plate_index())
        return;

    // If possible, remove accents from accented latin characters.
    // This function is useful for generating file names to be processed by legacy firmwares.
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = q->update_restart_background_process(false, false);
        if (state & UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = m_slicing.background_process().output_filepath_for_project("");
    } catch (const Slic3r::PlaceholderParserError &ex) {
        // Show the error with monospaced font.
        show_error(q, ex.what(), true);
        return;
    } catch (const std::exception &ex) {
        show_error(q, ex.what(), false);
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    AppConfig 				&appconfig 				 = *q->app_config();
    RemovableDriveManager 	&removable_drive_manager = *q->removable_drive_manager();
    // Get a last save path, either to removable media or to an internal media.
    std::string      		 start_dir 				 = appconfig.get_last_output_dir(default_output_file.parent_path().string(), prefer_removable);
    if (prefer_removable) {
        // Returns a path to a removable media if it exists, prefering start_dir. Update the internal removable drives database.
        start_dir = removable_drive_manager.get_removable_drive_path(start_dir);
        if (start_dir.empty())
            // Direct user to the last internal media.
            start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);
    }

    fs::path output_path;
    {
        std::string ext = default_output_file.extension().string();
        wxFileDialog dlg(q, (m_session.printer_technology == ptFFF) ? _L("Save G-code file as:") : _L("Save SLA file as:"),
            start_dir,
            from_path(default_output_file.filename()),
            file_wildcards((m_session.printer_technology == ptFFF) ? FT_GCODE : FT_SL1, ext),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );
        if (dlg.ShowModal() == wxID_OK) {
            output_path = into_path(dlg.GetPath());
            while (ProjectSession::has_illegal_filename_characters(output_path.filename().string())) {
                show_error(q, _L("The provided file name is not valid.") + "\n" +
                    _L("The following characters are not allowed by a FAT file system:") + " <>:/\\|?*\"");
                dlg.SetFilename(from_path(output_path.filename()));
                if (dlg.ShowModal() == wxID_OK)
                    output_path = into_path(dlg.GetPath());
                else {
                    output_path.clear();
                    break;
                }
            }
        }
    }

    if (! output_path.empty()) {
        bool path_on_removable_media = removable_drive_manager.set_and_verify_last_save_path(output_path.string());
        //bool path_on_removable_media = false;
        m_hub.nm()->new_export_began(path_on_removable_media);
        m_exporting_status = path_on_removable_media ? ExportingStatus::EXPORTING_TO_REMOVABLE : ExportingStatus::EXPORTING_TO_LOCAL;
        m_last_output_path = output_path.string();
        m_last_output_dir_path = output_path.parent_path().string();
        export_gcode(output_path, path_on_removable_media);
        // Storing a path to AppConfig either as path to removable media or a path to internal media.
        // is_path_on_removable_drive() is called with the "true" parameter to update its internal database as the user may have shuffled the external drives
        // while the dialog was open.
        appconfig.update_last_output_dir(output_path.parent_path().string(), path_on_removable_media);

        try {
            json j;
            auto printer_config = q->preset_bundle()->printers.get_edited_preset_with_vendor_profile().preset;
            if (printer_config.is_system) {
                j["printer_preset"] = printer_config.name;
            } else {
                j["printer_preset"] = printer_config.config.opt_string("inherits");
            }

            PresetBundle *preset_bundle = q->preset_bundle();
            if (preset_bundle) {
                j["gcode_printer_model"] = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
            }
            NetworkAgent *agent = q->get_agent();
        } catch (...) {}

    }
}

void ExportSendCoordinator::export_gcode_3mf(bool export_all)
{
    if (model->objects.empty())
        return;

    if (q->get_process_completed_with_error() == m_partplate_list.get_curr_plate_index())
        return;

    //calc default_output_file, get default output file from background process
    fs::path default_output_file;
    AppConfig& appconfig = *q->app_config();
    std::string start_dir;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = q->update_restart_background_process(false, false);
        if (state & UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = m_slicing.background_process().output_filepath_for_project("");
    }
    catch (const Slic3r::PlaceholderParserError& ex) {
        // Show the error with monospaced font.
        show_error(q, ex.what(), true);
        return;
    }
    catch (const std::exception& ex) {
        show_error(q, ex.what(), false);
        return;
    }
    default_output_file.replace_extension(".gcode.3mf");
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));

    //Get a last save path
    start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);

    fs::path output_path;
    {
        std::string ext = default_output_file.extension().string();
        wxFileDialog dlg(q, _L("Save Sliced file as:"),
            start_dir,
            from_path(default_output_file.filename()),
            file_wildcards(FT_GCODE_3MF, ""),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );
        if (dlg.ShowModal() == wxID_OK) {
            output_path = into_path(dlg.GetPath());
            ext = output_path.extension().string();
            if (ext != ".3mf")
                output_path = output_path.string() + ".3mf";
        }
    }

    if (!output_path.empty()) {
        //BBS do not set to removable media path
        bool path_on_removable_media = false;
        m_hub.nm()->new_export_began(path_on_removable_media);
        m_exporting_status = path_on_removable_media ? ExportingStatus::EXPORTING_TO_REMOVABLE : ExportingStatus::EXPORTING_TO_LOCAL;
        //BBS do not save last output path
        m_last_output_path = output_path.string();
        m_last_output_dir_path = output_path.parent_path().string();
        int plate_idx = m_partplate_list.get_curr_plate_index();
        if (export_all)
            plate_idx = PLATE_ALL_IDX;
        export_3mf(output_path, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel, plate_idx); // BBS: silence

        RemovableDriveManager& removable_drive_manager = *q->removable_drive_manager();


        bool on_removable = removable_drive_manager.is_path_on_removable_drive(m_last_output_dir_path);


        // update last output dir
        appconfig.update_last_output_dir(output_path.parent_path().string(), false);
        m_hub.nm()->push_exporting_finished_notification(output_path.string(), m_last_output_dir_path, on_removable);
    }
}

void ExportSendCoordinator::export_core_3mf()
{
    wxString path = get_export_file((int)FT_3MF);
    if (path.empty())
        return;
    boost::filesystem::path path_u8(into_u8(path));
    export_3mf(path_u8, SaveStrategy::Silence);
}

// —— 3.2 导出组 Part 2（原 Plater::combine_mesh_fff :6817 static / export_stl :6884；注释死块 /*export_amf*/ 不迁随 Plater.cpp 删除）——

TriangleMesh ExportSendCoordinator::combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func)
{
    TriangleMesh mesh;

    std::vector<csg::CSGPart> csgmesh;
    csgmesh.reserve(2 * mo.volumes.size());
    bool has_splitable_volume = csg::model_to_csgmesh(mo, Transform3d::Identity(), std::back_inserter(csgmesh),
        csg::mpartsPositive | csg::mpartsNegative);

    std::string fail_msg = _u8L("Unable to perform boolean operation on model meshes. "
        "Only positive parts will be kept. You may fix the meshes and try again.");
    if (auto fail_reason_name = csg::check_csgmesh_booleans(Range{ std::begin(csgmesh), std::end(csgmesh) }); std::get<0>(fail_reason_name) != csg::BooleanFailReason::OK) {
        std::string name = std::get<1>(fail_reason_name);
        std::map<csg::BooleanFailReason, std::string> fail_reasons = {
            {csg::BooleanFailReason::OK, "OK"},
            {csg::BooleanFailReason::MeshEmpty, Slic3r::format( _u8L("Reason: part \"%1%\" is empty."), name)},
            {csg::BooleanFailReason::NotBoundAVolume, Slic3r::format(_u8L("Reason: part \"%1%\" does not bound a volume."), name)},
            {csg::BooleanFailReason::SelfIntersect, Slic3r::format(_u8L("Reason: part \"%1%\" has self intersection."), name)},
            {csg::BooleanFailReason::NoIntersection, Slic3r::format(_u8L("Reason: \"%1%\" and another part have no intersection."), name)} };
        fail_msg += " " + fail_reasons[std::get<0>(fail_reason_name)];
    }
    else {
        try {
            MeshBoolean::mcut::McutMeshPtr meshPtr = csg::perform_csgmesh_booleans_mcut(Range{ std::begin(csgmesh), std::end(csgmesh) });
            mesh = MeshBoolean::mcut::mcut_to_triangle_mesh(*meshPtr);
        }
        catch (...) {}
#if 0
        // if mcut fails, try again with CGAL
        if (mesh.empty()) {
            try {
                auto meshPtr = csg::perform_csgmesh_booleans(Range{ std::begin(csgmesh), std::end(csgmesh) });
                mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*meshPtr);
                }
            catch (...) {}
        }
#endif
    }

    if (mesh.empty()) {
        if (notify_func)
            notify_func(fail_msg);

        for (const ModelVolume* v : mo.volumes)
            if (v->is_model_part()) {
                TriangleMesh vol_mesh(v->mesh());
                vol_mesh.transform(v->get_matrix(), true);
                mesh.merge(vol_mesh);
            }
    }

    if (instance_id == -1) {
        TriangleMesh vols_mesh(mesh);
        mesh = TriangleMesh();
        for (const ModelInstance* i : mo.instances) {
            TriangleMesh m = vols_mesh;
            m.transform(i->get_matrix(), true);
            mesh.merge(m);
        }
    }
    else if (0 <= instance_id && instance_id < int(mo.instances.size()))
        mesh.transform(mo.instances[instance_id]->get_matrix(), true);
    return mesh;
}

// BBS export with/without boolean, however, stil merge mesh
#define EXPORT_WITH_BOOLEAN 0
void ExportSendCoordinator::export_stl(bool extended, bool selection_only, bool multi_stls)
{
    if (model->objects.empty()) { return; }

    wxString path;
    if (multi_stls) {
        wxDirDialog dlg(q, _L("Choose a directory"), from_u8(q->app_config()->get_last_dir()),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            path = dlg.GetPath() + "/";
        }
    } else {
        path = get_export_file((int)FT_STL);
    }
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);

    wxBusyCursor wait;
    const auto& selection = m_selq->selection();
    const auto obj_idx = selection.get_object_idx();

#if EXPORT_WITH_BOOLEAN
    if (selection_only && (obj_idx == -1 || selection.is_wipe_tower()))
        return;
#else
    // BBS support selecting multiple objects
    if (selection_only && selection.is_wipe_tower()) return;

    // BBS
    if (selection_only) {
        // only support selection single full object and mulitiple full object
        if (!selection.is_single_full_object() && !selection.is_multiple_full_object()) return;
    }

    // Following lambda generates a combined mesh for export with normals pointing outwards.
    auto mesh_to_export_fff_no_boolean = [this](const ModelObject &mo, int instance_id) {
        TriangleMesh mesh;

        //Prusa export negative parts
        std::vector<csg::CSGPart> csgmesh;
        csgmesh.reserve(2 * mo.volumes.size());
        csg::model_to_csgmesh(mo, Transform3d::Identity(), std::back_inserter(csgmesh),
                              csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);

        auto csgrange = range(csgmesh);
        if (csg::is_all_positive(csgrange)) {
            mesh = TriangleMesh{csg::csgmesh_merge_positive_parts(csgrange)};
        } else if (std::get<2>(csg::check_csgmesh_booleans(csgrange)) == csgrange.end()) {
            try {
                auto cgalm = csg::perform_csgmesh_booleans(csgrange);
                mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalm);
            } catch (...) {}
        }

        if (mesh.empty()) {
            m_hub.nm()->push_plater_error_notification(
                _u8L("Unable to perform boolean operation on model meshes. "
                     "Only positive parts will be exported."));

            for (const ModelVolume* v : mo.volumes)
                if (v->is_model_part()) {
                    TriangleMesh vol_mesh(v->mesh());
                    vol_mesh.transform(v->get_matrix(), true);
                    mesh.merge(vol_mesh);
                }
        }
        if (instance_id == -1) {
            TriangleMesh vols_mesh(mesh);
            mesh = TriangleMesh();
            for (const ModelInstance *i : mo.instances) {
                TriangleMesh m = vols_mesh;
                m.transform(i->get_matrix(), true);
                mesh.merge(m);
            }
        } else if (0 <= instance_id && instance_id < int(mo.instances.size()))
            mesh.transform(mo.instances[instance_id]->get_matrix(), true);
        return mesh;
    };
#endif
    auto mesh_to_export_sla = [&, this](const ModelObject& mo, int instance_id) {
        TriangleMesh mesh;

        const SLAPrintObject *object = m_slicing.sla_print().get_print_object_by_model_object_id(mo.id());

        if (auto m = object->get_mesh_to_print(); m.empty())
            mesh = combine_mesh_fff(mo, instance_id, [this](const std::string& msg) {return m_hub.nm()->push_plater_error_notification(msg); });
        else {
            const Transform3d mesh_trafo_inv = object->trafo().inverse();
            const bool is_left_handed = object->is_left_handed();

            auto pad_mesh = extended? object->pad_mesh() : TriangleMesh{};
            pad_mesh.transform(mesh_trafo_inv);

            auto supports_mesh = extended ? object->support_mesh() : TriangleMesh{};
            supports_mesh.transform(mesh_trafo_inv);

            const std::vector<SLAPrintObject::Instance>& obj_instances = object->instances();
            for (const SLAPrintObject::Instance& obj_instance : obj_instances) {
                auto it = std::find_if(object->model_object()->instances.begin(), object->model_object()->instances.end(),
                                       [&obj_instance](const ModelInstance *mi) { return mi->id() == obj_instance.instance_id; });
                assert(it != object->model_object()->instances.end());

                if (it != object->model_object()->instances.end()) {
                    const bool one_inst_only = selection_only && ! selection.is_single_full_object();

                    const int instance_idx = it - object->model_object()->instances.begin();
                    const Transform3d& inst_transform = one_inst_only
                                                            ? Transform3d::Identity()
                                                            : object->model_object()->instances[instance_idx]->get_transformation().get_matrix();

                    TriangleMesh inst_mesh;

                    if (!pad_mesh.empty()) {
                        TriangleMesh inst_pad_mesh = pad_mesh;
                        inst_pad_mesh.transform(inst_transform, is_left_handed);
                        inst_mesh.merge(inst_pad_mesh);
                    }

                    if (!supports_mesh.empty()) {
                        TriangleMesh inst_supports_mesh = supports_mesh;
                        inst_supports_mesh.transform(inst_transform, is_left_handed);
                        inst_mesh.merge(inst_supports_mesh);
                    }

                    TriangleMesh inst_object_mesh = object->get_mesh_to_print();

                    inst_object_mesh.transform(mesh_trafo_inv);
                    inst_object_mesh.transform(inst_transform, is_left_handed);

                    inst_mesh.merge(inst_object_mesh);

                           // ensure that the instance lays on the bed
                    inst_mesh.translate(0.0f, 0.0f, -inst_mesh.bounding_box().min.z());

                           // merge instance with global mesh
                    mesh.merge(inst_mesh);

                    if (one_inst_only)
                        break;
                }
            }
        }

        return mesh;
    };

    std::function<TriangleMesh(const ModelObject& mo, int instance_id)>
        mesh_to_export;

    if (m_session.printer_technology == ptFFF)
#if EXPORT_WITH_BOOLEAN
        mesh_to_export = [this](const ModelObject& mo, int instance_id) {return combine_mesh_fff(mo, instance_id,
            [this](const std::string& msg) {return m_hub.nm()->push_plater_error_notification(msg); }); };
#else
        mesh_to_export = mesh_to_export_fff_no_boolean;
#endif
    else
        mesh_to_export = mesh_to_export_sla;

    auto get_save_file = [](std::string const & dir, std::string const & name) {
        auto path = dir + name + ".stl";
        int n = 1;
        while (boost::filesystem::exists(path))
            path = dir + name + "(" + std::to_string(n++) + ").stl";
        return path;
    };

    TriangleMesh mesh;
    if (selection_only) {
        if (selection.is_single_full_object()) {
            const auto obj_idx = selection.get_object_idx();
            const ModelObject* model_object = model->objects[obj_idx];
            if (selection.get_mode() == Selection::Instance)
                mesh = mesh_to_export(*model_object, (model_object->instances.size() > 1) ? -1 : selection.get_instance_idx());
            else {
                const GLVolume* volume = selection.get_first_volume();
                mesh = model_object->volumes[volume->volume_idx()]->mesh();
                mesh.transform(volume->get_volume_transformation().get_matrix(), true);
            }

            if (model_object->instances.size() == 1) mesh.translate(-model_object->origin_translation.cast<float>());
        }
        else if (selection.is_multiple_full_object() && !multi_stls) {
            const std::set<std::pair<int, int>>& instances_idxs = m_selq->selection().get_selected_object_instances();
            for (const std::pair<int, int>& i : instances_idxs) {
                ModelObject* object = model->objects[i.first];
                mesh.merge(mesh_to_export(*object, i.second));
            }
        }
        else if (selection.is_multiple_full_object() && multi_stls) {
            const std::set<std::pair<int, int>> &instances_idxs = m_selq->selection().get_selected_object_instances();
            for (const std::pair<int, int> &i : instances_idxs) {
                ModelObject *object = model->objects[i.first];
                auto mesh = mesh_to_export(*object, i.second);
                mesh.translate(-object->origin_translation.cast<float>());

                Slic3r::store_stl(get_save_file(path_u8, object->name).c_str(), &mesh, true);
            }
            return;
        }
    }
    else if (!multi_stls) {
        for (const ModelObject* o : model->objects) {
            mesh.merge(mesh_to_export(*o, -1));
        }
    } else {
        for (const ModelObject* o : model->objects) {
            auto mesh = mesh_to_export(*o, -1);
            mesh.translate(-o->origin_translation.cast<float>());
            Slic3r::store_stl(get_save_file(path_u8, o->name).c_str(), &mesh, true);
        }
        return;
    }

    Slic3r::store_stl(path_u8.c_str(), &mesh, true);
}

// THUMBNAIL_SIZE_3MF 镜像常量（原 Plater.cpp:221 cpp-local static；export_3mf 缩略图生成尺寸，勘误随收口登记）
static const std::pair<unsigned int, unsigned int> THUMBNAIL_SIZE_3MF = { 512, 512 };

// —— 3.2 导出组 Part 2b（原 Plater::export_3mf :7226 巨体 / has_toolpaths_to_export :7491 / export_toolpaths_to_obj :7496）——

int ExportSendCoordinator::export_3mf(const boost::filesystem::path& output_path, SaveStrategy strategy, int export_plate_idx, Export3mfProgressFn proFn)
{
    int ret = 0;
    //if (model->objects.empty()) {
    //    MessageDialog dialog(nullptr, _L("No objects to export."), _L("Save project"), wxYES);
    //    if (dialog.ShowModal() == wxYES)
    //        return -1;
    //}

    if (output_path.empty())
        return -1;

    bool export_config = true;
    wxString path = from_path(output_path);

    if (!path.Lower().EndsWith(".3mf"))
        return -1;

    // take care about private data stored into .3mf
    // modify model
    q->publish_model(*model, strategy);   // Plater.cpp 局部 publish()（SVG 发布），Host 桥接

    DynamicPrintConfig cfg = q->preset_bundle()->full_config_secure();
    const std::string path_u8 = into_u8(path);
    wxBusyCursor wait;

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": path=%1%, backup=%2%, export_plate_idx=%3%, SaveStrategy=%4%")
        %output_path.string()%(strategy & SaveStrategy::Backup)%export_plate_idx %(unsigned int)strategy;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": path=%1%, backup=%2%, export_plate_idx=%3%, SaveStrategy=%4%")
        % std::string("") % (strategy & SaveStrategy::Backup) % export_plate_idx % (unsigned int)strategy;

    //BBS: add plate logic for thumbnail generate
    std::vector<ThumbnailData*> thumbnails;
    std::vector<ThumbnailData*> no_light_thumbnails;
    std::vector<ThumbnailData*> calibration_thumbnails;
    std::vector<ThumbnailData*> top_thumbnails;
    std::vector<ThumbnailData*> picking_thumbnails;
    std::vector<PlateBBoxData*> plate_bboxes;
    // BBS: backup
    if (!(strategy & SaveStrategy::Backup)) {
        for (int i = 0; i < m_partplate_list.get_plate_count(); i++) {
            ThumbnailData* thumbnail_data = &m_partplate_list.get_plate(i)->thumbnail_data;
            if (m_partplate_list.get_plate(i)->thumbnail_data.is_valid() &&  m_session.using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, true, true, i };
                q->generate_thumbnail(m_partplate_list.get_plate(i)->thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second,
                                    thumbnail_params, Camera::EType::Ortho, false, false, false);
            }
            thumbnails.push_back(thumbnail_data);

            ThumbnailData *no_light_thumbnail_data = &m_partplate_list.get_plate(i)->no_light_thumbnail_data;
            if (m_partplate_list.get_plate(i)->no_light_thumbnail_data.is_valid() && m_session.using_exported_file()) {
                // no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate thumbnail for gcode/exported mode of plate %1%") % i;
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = {{}, false, true, true, true, i};
                q->generate_thumbnail(m_partplate_list.get_plate(i)->no_light_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second,
                    thumbnail_params, Camera::EType::Ortho, false, false, true);
            }
            no_light_thumbnails.push_back(no_light_thumbnail_data);
            //ThumbnailData* calibration_data = &m_partplate_list.get_plate(i)->cali_thumbnail_data;
            //calibration_thumbnails.push_back(calibration_data);
            PlateBBoxData* plate_bbox_data = &m_partplate_list.get_plate(i)->cali_bboxes_data;
            plate_bboxes.push_back(plate_bbox_data);

            //generate top and picking thumbnails
            ThumbnailData* top_thumbnail = &m_partplate_list.get_plate(i)->top_thumbnail_data;
            if (top_thumbnail->is_valid() &&  m_session.using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate top_thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate top_thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, false, true, i };
                q->generate_thumbnail(m_partplate_list.get_plate(i)->top_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second,
                                    thumbnail_params, Camera::EType::Ortho, true, false, false);
            }
            top_thumbnails.push_back(top_thumbnail);

            ThumbnailData* picking_thumbnail = &m_partplate_list.get_plate(i)->pick_thumbnail_data;
            if (picking_thumbnail->is_valid() &&  m_session.using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate pick_thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate pick_thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, false, true, i };
                q->generate_thumbnail(m_partplate_list.get_plate(i)->pick_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second,
                                    thumbnail_params, Camera::EType::Ortho, true, true, false);
            }
            picking_thumbnails.push_back(picking_thumbnail);
        }

        if (m_partplate_list.get_curr_plate()->is_slice_result_valid()) {
            //BBS generate BBS calibration thumbnails
            int index = m_partplate_list.get_curr_plate_index();
            //ThumbnailData* calibration_data = calibration_thumbnails[index];
            //const ThumbnailsParams calibration_params = { {}, false, true, true, true, m_partplate_list.get_curr_plate_index() };
            //q->generate_calibration_thumbnail(*calibration_data, PartPlate::cali_thumbnail_width, PartPlate::cali_thumbnail_height, calibration_params);
            if (m_session.using_exported_file()) {
                //do nothing
            }
            else
                *plate_bboxes[index] = q->generate_first_layer_bbox();
        }
    }

    //BBS: add bbs 3mf logic
    PlateDataPtrs plate_data_list;
    m_partplate_list.store_to_3mf_structure(plate_data_list, (strategy & SaveStrategy::WithGcode || strategy & SaveStrategy::WithSliceInfo), export_plate_idx);

    // BBS: backup
    PresetBundle& preset_bundle = *q->preset_bundle();
    std::vector<Preset*> project_presets = preset_bundle.get_current_project_embedded_presets();

    StoreParams store_params;
    store_params.path  = path_u8.c_str();
    store_params.model = model;
    store_params.plate_data_list = plate_data_list;
    store_params.export_plate_idx = export_plate_idx;
    store_params.project_presets = project_presets;
    store_params.config = export_config ? &cfg : nullptr;
    store_params.thumbnail_data = thumbnails;
    store_params.no_light_thumbnail_data  = no_light_thumbnails;
    store_params.top_thumbnail_data = top_thumbnails;
    store_params.pick_thumbnail_data = picking_thumbnails;
    store_params.calibration_thumbnail_data = calibration_thumbnails;
    store_params.proFn = proFn;
    store_params.id_bboxes = plate_bboxes;//BBS
    store_params.project = &m_session.project;
    store_params.strategy = strategy | SaveStrategy::Zip64;


    // get type and color for platedata
    auto* filament_color = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_colour"));
    auto* nozzle_diameter_option = dynamic_cast<const ConfigOptionFloats*>(cfg.option("nozzle_diameter"));
    auto* filament_id_opt = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_ids"));
    std::string nozzle_diameter_str;
    if (nozzle_diameter_option)
        nozzle_diameter_str = nozzle_diameter_option->serialize();

    std::string printer_model_id = preset_bundle.printers.get_edited_preset().get_printer_type(&preset_bundle);

    for (int i = 0; i < plate_data_list.size(); i++) {
        PlateData *plate_data = plate_data_list[i];
        plate_data->printer_model_id = printer_model_id;
        plate_data->nozzle_diameters = nozzle_diameter_str;
        for (auto it = plate_data->slice_filaments_info.begin(); it != plate_data->slice_filaments_info.end(); it++) {
            std::string display_filament_type;
            it->type  = cfg.get_filament_type(display_filament_type, it->id);
            it->filament_id = filament_id_opt ? filament_id_opt->get_at(it->id) : "";
            it->color = filament_color ? filament_color->get_at(it->id) : "#FFFFFF";
            // save filament info used in curr plate
            int index = m_partplate_list.get_curr_plate_index();
            if (store_params.id_bboxes.size() > index) {
                store_params.id_bboxes[index]->filament_ids.push_back(it->id);
                store_params.id_bboxes[index]->filament_colors.push_back(it->color);
            }
        }
    }

    // handle Design Info
    bool has_design_info = false;
    ModelDesignInfo designInfo;
    if (model->design_info != nullptr) {
        if (!model->design_info->Designer.empty()) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, found designer = " << model->design_info->Designer;
            has_design_info = true;
        }
    }
    if (!has_design_info) {
        // add Designed Info
        if (model->design_info == nullptr) {
            // set designInfo before export and reset after export
            if (q->is_user_login()) {
                model->design_info                 = std::make_shared<ModelDesignInfo>();
                //model->design_info->Designer       = q->get_agent()->get_user_nickanme();
                model->design_info->Designer       = "";
                model->design_info->DesignerUserId = q->get_agent()->get_user_id();
                BOOST_LOG_TRIVIAL(trace) << "design_info prepare, designer = "<< "";
                BOOST_LOG_TRIVIAL(trace) << "design_info prepare, designer_user_id = " << model->design_info->DesignerUserId;
            }
        }
    }

    bool store_result = Slic3r::store_bbs_3mf(store_params);
    // reset designed info
    if (!has_design_info)
        model->design_info = nullptr;

    if (store_result) {
        if (!(store_params.strategy & SaveStrategy::Silence)) {
            // Success
            m_session.set_project_filename(path);
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << path;
        }
    }
    else {
        ret = -1;
    }

    if (project_presets.size() > 0)
    {
        for (unsigned int i = 0; i < project_presets.size(); i++)
        {
            delete project_presets[i];
        }
        project_presets.clear();
    }

    release_PlateData_list(plate_data_list);

    for (unsigned int i = 0; i < calibration_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        calibration_thumbnails[i]->reset();
    }
    for (unsigned int i = 0; i < no_light_thumbnails.size(); i++) {
        // release the data here, as it will always be generated when export
        no_light_thumbnails[i]->reset();
    }
    for (unsigned int i = 0; i < top_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        top_thumbnails[i]->reset();
    }
    top_thumbnails.clear();
    for (unsigned int i = 0; i < picking_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        picking_thumbnails[i]->reset();;
    }
    picking_thumbnails.clear();

    return ret;
}

bool ExportSendCoordinator::has_toolpaths_to_export() const
{
    return  m_layout->preview->get_canvas3d()->has_toolpaths_to_export();
}

void ExportSendCoordinator::export_toolpaths_to_obj() const
{
    if ((m_session.printer_technology != ptFFF) || !m_layout->is_preview_loaded())
        return;

    // const 方法内走非 const 内部路径（原 priv 同款语义；get_export_file 弹对话框）
    wxString path = const_cast<ExportSendCoordinator*>(this)->get_export_file((int)FT_OBJ);
    if (path.empty())
        return;

    wxBusyCursor wait;
    m_layout->preview->get_canvas3d()->export_toolpaths_to_obj(into_u8(path).c_str());
}

// —— 3.5 job_finished 四件（手册 §2.1；原 Plater::send_calibration_job_finished :8101 / print_job_finished :8115 /
//      send_job_finished :8137 / publish_job_finished :8152；Bind [q] lambda 走门面转发，wx 触点经 Host/[] 槽）——

void ExportSendCoordinator::handle_send_calibration_job_finished(wxCommandEvent& evt)
{
    m_main_frame->request_select_tab(MainFrame::TabPosition::tpCalibration);
    auto calibration_panel = m_main_frame->m_calibration;
    if (calibration_panel) {
        auto curr_wizard = static_cast<CalibrationWizard*>(calibration_panel->get_tabpanel()->GetPage(evt.GetInt()));
        wxCommandEvent event(EVT_CALIBRATION_JOB_FINISHED);
        event.SetString(evt.GetString());
        event.SetEventObject(curr_wizard);
        wxPostEvent(curr_wizard, event);
    }
    evt.Skip();
}

void ExportSendCoordinator::handle_print_job_finished(wxCommandEvent& evt)
{
    //start print failed
    if (q->inf_dialog_context().empty()) {
        hide_select_machine_dlg();
    }
    else {
        enter_prepare_mode();
    }


    Slic3r::DeviceManager* dev = q->device_manager();
    if (!dev) return;

    dev->set_selected_machine(evt.GetString().ToStdString());
    m_main_frame->request_select_tab(MainFrame::TabPosition::tpMonitor);
    //jump to monitor and select device status panel
    MonitorPanel* curr_monitor = m_main_frame->m_monitor;
    if(curr_monitor)
       curr_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PrinterTab::PT_STATUS);
}

void ExportSendCoordinator::handle_send_job_finished(wxCommandEvent& evt)
{
    Slic3r::DeviceManager* dev = q->device_manager();
    if (!dev) return;
    //dev->set_selected_machine(evt.GetString().ToStdString());

    send_gcode_finish(evt.GetString());
    hide_send_to_printer_dlg();
    //m_main_frame->request_select_tab(MainFrame::TabPosition::tpMonitor);
    ////jump to monitor and select device status panel
    //MonitorPanel* curr_monitor = m_main_frame->m_monitor;
    //if (curr_monitor)
    //    curr_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PrinterTab::PT_STATUS);
}

void ExportSendCoordinator::handle_publish_job_finished(wxCommandEvent& evt)
{
    m_publish_dlg->EndModal(wxID_OK);
   // GUI::wxGetApp().load_url(evt.GetString());
   //GUI::wxGetApp().open_publish_page_dialog(evt.GetString());
}

// —— 3.6 插件/错误/内联壳组（原 priv::install_network_plugin :4258 / update_plugin_when_launch :4264 /
//      show_install_plugin_hint :4286 / 内联壳 ×3 :966-977；update_print_error_info 门面 :9480）——

void ExportSendCoordinator::install_network_plugin()
{
    q->show_down_net_plugin_dlg();
    return;
}

void ExportSendCoordinator::update_plugin_when_launch()
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto cache_folder = data_dir_path / "ota";
    std::string changelog_file = cache_folder.string() + "/network_plugins.json";

    UpdatePluginDialog dlg(m_main_frame);
    dlg.update_info(changelog_file);
    auto result = dlg.ShowModal();

    auto app_config = q->app_config();
    if (!app_config) return;

    if (result == wxID_OK) {
        app_config->set("update_network_plugin", "true");
    }
    else if (result == wxID_NO) {
        app_config->set("update_network_plugin", "false");
    }
}

void ExportSendCoordinator::show_install_plugin_hint()
{
    m_hub.nm()->bbl_show_plugin_install_notification(into_u8(_L("Network Plug-in is not detected. Network related features are unavailable.")));
}

void ExportSendCoordinator::update_print_error_info(int code, std::string msg, std::string extra)
{
    if (m_select_machine_dlg) {
        m_select_machine_dlg->update_print_error_info(code, msg, extra);
    }

    if (m_send_to_sdcard_dlg) {
        m_send_to_sdcard_dlg->update_print_error_info(code, msg, extra);
    }
    if (m_main_frame->m_calibration)
        m_main_frame->m_calibration->update_print_error_info(code, msg, extra);
}

void ExportSendCoordinator::notify_send_to_sdcard_color_mode()
{
    if (m_send_to_sdcard_dlg) m_send_to_sdcard_dlg->on_change_color_mode();
}

void ExportSendCoordinator::notify_sys_color_changed()
{
    if (m_select_machine_dlg) m_select_machine_dlg->sys_color_changed();
}

// priv 内联壳 ×3 随迁（原 priv :966-977）
void ExportSendCoordinator::hide_select_machine_dlg()
{
    if (m_select_machine_dlg)
        m_select_machine_dlg->EndModal(wxID_OK);
}

void ExportSendCoordinator::enter_prepare_mode()
{
    if (m_select_machine_dlg)
        m_select_machine_dlg->prepare_mode();
}

void ExportSendCoordinator::hide_send_to_printer_dlg() { m_send_to_sdcard_dlg->EndModal(wxID_OK); }

} } } // namespace Slic3r::GUI::app
