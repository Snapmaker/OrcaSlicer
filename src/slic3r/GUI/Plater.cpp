#include "Plater.hpp"
#include "Search.hpp"   // H2 3.1：真实供给头（Plater.hpp 传递链清算）
#include "Sidebar.hpp"    // 阶段 A：完整 Sidebar 类型（原由 Plater.hpp 内嵌声明提供）
#include "MixedFilamentDialog.hpp"
#include "MixedFilamentBatchDialog.hpp"
#include "MixedGradientSelector.hpp"
#include "MixedColorMatchPanel.hpp"
#include "MixedFilamentBadge.hpp"
#include "MixedFilamentColorMapPanel.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "libslic3r/FilamentColorLibrary.hpp" // kFullSpectrumSlotCount (recommended slot write-back)
#include "libslic3r/Config.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentConfigRemap.hpp"
#include "libslic3r/filament_mixer.h"
#include "common_func/common_func.hpp"
#include "slic3r/Utils/SnapLogClient.hpp"

#include <atomic>
#include <cstddef>
#include <array>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numeric>
#include <memory>
#include <limits>
#include <thread>
#include <vector>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <future>
#include <functional>
#include <sstream>
#include <utility>
#include <boost/algorithm/string.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/bmpcbox.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/filedlg.h>
#include <wx/dnd.h>
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/wupdlock.h>
#include <wx/numdlg.h>
#include <wx/debug.h>
#include <wx/busyinfo.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/event.h>
#include <wx/wrapsizer.h>
#include <wx/choice.h>
#include <wx/gauge.h>
#include <wx/slider.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#ifdef _WIN32
#include <wx/richtooltip.h>
#include <wx/custombgwin.h>
#include <wx/popupwin.h>
#endif
#include <wx/clrpicker.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/aui/aui.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/AMF.hpp"
//#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/SLA/ReprojectPointsOnMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetFlowVariant.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/FilamentHotBedNozzleRules.hpp"

// For stl export
#include "libslic3r/CSGMesh/ModelToCSGMesh.hpp"
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "FilamentGroupDialog.hpp"
#include "FlowTypeHelper.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "GUI_Factories.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "GLToolbar.hpp"
#include "GUI_Preview.hpp"
#include "3DBed.hpp"
#include "PartPlate.hpp"
#include "Camera.hpp"
#include "Mouse3DController.hpp"
#include "Tab.hpp"
#include "Jobs/OrientJob.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "Jobs/FillBedJob.hpp"
#include "Jobs/RotoptimizeJob.hpp"
#include "Jobs/SLAImportJob.hpp"
#include "Jobs/SLAImportDialog.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/NotificationProgressIndicator.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "SelectMachine.hpp"
#include "SendMultiMachinePage.hpp"
#include "SendToPrinter.hpp"
#include "PublishDialog.hpp"
#include "ModelMall.hpp"
#include "ConfigWizard.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/ColorSpaceConvert.hpp"
#include "../Utils/FixModelByWin10.hpp"
#include "../Utils/UndoRedo.hpp"
#include "../Utils/PresetUpdater.hpp"
#include "../Utils/Process.hpp"
#include "../Utils/GatewayProtocol.hpp"
#include "../Utils/GatewayDevice.hpp"
#include "RemovableDriveManager.hpp"
#include "InstanceCheck.hpp"
#include "NotificationManager.hpp"
#include "PresetComboBoxes.hpp"
#include "PlaterFilamentComboBox.hpp"
#include "MsgDialog.hpp"
#include "ProjectDirtyStateManager.hpp"
#include "Gizmos/GLGizmoSimplify.hpp" // create suggestion notification
#include "Gizmos/GLGizmoSVG.hpp" // Drop SVG file
#include "Gizmos/GizmoObjectManipulation.hpp"

// BBS
#include "Widgets/ProgressDialog.hpp"
#include "BBLStatusBar.hpp"
#include "BitmapCache.hpp"
#include "ParamsDialog.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Button.hpp"

#include "GUI_ObjectTable.hpp"
#include "libslic3r/Thread.hpp"

#ifdef __APPLE__
#include "Gizmos/GLGizmosManager.hpp"
#endif // __APPLE__

#include <libslic3r/CutUtils.hpp>
#include <wx/glcanvas.h>    // Needs to be last because reasons :-/
#include <libslic3r/miniz_extension.hpp>
#include "WipeTowerDialog.hpp"
#include "ObjColorDialog.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Platform.hpp"
#include "nlohmann/json.hpp"

#include "PhysicalPrinterDialog.hpp"
#include "PrintHostDialogs.hpp"
#include "PlateSettingsDialog.hpp"
#include "DailyTips.hpp"
#include "CreatePresetsDialog.hpp"
#include "FileArchiveDialog.hpp"
#include "StepMeshDialog.hpp"
#include "CloneDialog.hpp"
#include "WebPreprintDialog.hpp"

#include "filamentsync/SyncConfirmDialog.hpp"
#include "filamentsync/SyncFilamentColorDialog.hpp"

#include "sentry_wrapper/SentryWrapper.hpp"
#include <chrono>
#include "app/SnapshotService.hpp"   // 阶段 C：app::SnapshotService 完整类型（priv 成员；置于 include 块末尾）
#include "app/NotificationHub.hpp"   // 阶段 C：app::NotificationHub 完整类型（priv 成员）
#include "app/PlateController.hpp"   // 阶段 D1：app::PlateController 完整类型（priv 成员）
#include "app/SlicingController.hpp"   // 阶段 D2：app::SlicingController 完整类型（priv 成员）
#include "app/ProjectSession.hpp"   // 阶段 E1：app::ProjectSession 完整类型（priv 成员；置于 include 块末尾）
#include "app/ExportSendCoordinator.hpp"   // 阶段 E2：app::ExportSendCoordinator 完整类型（priv 成员；置于 include 块末尾）
#include "app/SelectionQuery.hpp"   // 阶段 F1：app::SelectionQuery 完整类型（priv 成员；置于 include 块末尾）
#include "app/ObjectEditCommands.hpp"   // 阶段 F1：app::ObjectEditCommands 完整类型（priv 成员）
#include "app/ArrangementService.hpp"   // 阶段 F2：app::ArrangementService 完整类型（priv 成员）
#include "app/ArrangeRequest.hpp"       // 阶段 F3：Request 值类型（写入面构造 + 门面转发）
#include "app/MixingGateKeeper.hpp"     // 阶段 B3：混色入口守门服务（去抖/门控/通知）
#include "app/FilamentService.hpp"      // 阶段 G1：耗材/AMS/批量删除编排服务
#include "app/CalibrationFlow.hpp"      // 阶段 G2：校准流编排服务
#include "app/LayoutManager.hpp"        // 阶段 G3：布局/视图/主题服务（18 成员 + 17 函数唯一归宿）
#include "app/SceneRefreshCoordinator.hpp"  // 阶段 G4：刷新调度服务（4 成员 + 14 函数唯一归宿）
#include "app/PlaterEventRouter.hpp"    // 阶段 G5：事件路由服务（138 Bind + 事件处理器唯一归宿）

using boost::optional;
namespace fs = boost::filesystem;
using Slic3r::_3DScene;
using Slic3r::Preset;
using Slic3r::GUI::format_wxstr;
using namespace nlohmann;

static const std::pair<unsigned int, unsigned int> THUMBNAIL_SIZE_3MF = { 512, 512 };

namespace Slic3r {
namespace GUI {

static void handle_newer_3mf_schema(wxWindow* parent,
                                    const DynamicPrintConfig& config)
{
    const ProjectSchemaDefinition& definition = ProjectSchemaRegistry::definition();
    const int file_schema = ProjectSchemaRegistry::version_from(config);
    if (!ProjectSchemaRegistry::is_newer(config))
        return;

    BOOST_LOG_TRIVIAL(info) << "3MF project schema is newer than supported"
                            << ": file_version=" << file_schema
                            << ", supported_version=" << definition.current_version;
    show_info(parent,
              _L("The current version of Snapmaker Orca cannot fully load this 3MF file. Please update to the latest version and try again."),
              _L("Loading 3MF"));
}


wxDECLARE_EVENT(EVT_SLICING_COMPLETED,               wxCommandEvent);
wxDECLARE_EVENT(EVT_EXPORT_BEGAN,                    wxCommandEvent);
wxDECLARE_EVENT(EVT_EXPORT_FINISHED,                 wxCommandEvent);
wxDECLARE_EVENT(EVT_DOWNLOAD_PROJECT,                wxCommandEvent);
wxDECLARE_EVENT(EVT_IMPORT_MODEL_ID,                 wxCommandEvent);
wxDECLARE_EVENT(EVT_PRINT_FINISHED,                  wxCommandEvent);
wxDECLARE_EVENT(EVT_SEND_CALIBRATION_FINISHED,       wxCommandEvent);
wxDECLARE_EVENT(EVT_SEND_FINISHED,                   wxCommandEvent);
wxDECLARE_EVENT(EVT_PUBLISH_FINISHED,                wxCommandEvent);
wxDECLARE_EVENT(EVT_RESTORE_PROJECT,                 wxCommandEvent);

wxDEFINE_EVENT(EVT_SLICING_UPDATE,                  SlicingStatusEvent);
wxDEFINE_EVENT(EVT_SLICING_COMPLETED,               wxCommandEvent);
wxDEFINE_EVENT(EVT_PROCESS_COMPLETED,               SlicingProcessCompletedEvent);
wxDEFINE_EVENT(EVT_EXPORT_BEGAN,                    wxCommandEvent);
wxDEFINE_EVENT(EVT_EXPORT_FINISHED,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_IMPORT_MODEL_ID,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD_PROJECT,                wxCommandEvent);
wxDEFINE_EVENT(EVT_PUBLISH,                         wxCommandEvent);
wxDEFINE_EVENT(EVT_OPEN_PLATESETTINGSDIALOG,        wxCommandEvent);
// BBS: backup & restore
wxDEFINE_EVENT(EVT_RESTORE_PROJECT,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_PRINT_FINISHED,                  wxCommandEvent);
wxDEFINE_EVENT(EVT_SEND_CALIBRATION_FINISHED,       wxCommandEvent);
wxDEFINE_EVENT(EVT_SEND_FINISHED,                   wxCommandEvent);
wxDEFINE_EVENT(EVT_PUBLISH_FINISHED,                wxCommandEvent);
//BBS: repair model
wxDEFINE_EVENT(EVT_REPAIR_MODEL,                    wxCommandEvent);
wxDEFINE_EVENT(EVT_FILAMENT_COLOR_CHANGED,          wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_PLUGIN_NETWORKING,       wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_PLUGINS_WHEN_LAUNCH,       wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_PLUGIN_HINT,             wxCommandEvent);
wxDEFINE_EVENT(EVT_PREVIEW_ONLY_MODE_HINT,          wxCommandEvent);
//BBS: change light/dark mode
wxDEFINE_EVENT(EVT_GLCANVAS_COLOR_MODE_CHANGED,     SimpleEvent);
//BBS: print
wxDEFINE_EVENT(EVT_PRINT_FROM_SDCARD_VIEW,          SimpleEvent);

wxDEFINE_EVENT(EVT_CREATE_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_MODIFY_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_FILAMENT_USAGE_CHANGED, SimpleEvent);
wxDEFINE_EVENT(EVT_ADD_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_DEL_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_ADD_CUSTOM_FILAMENT, ColorEvent);


#define PRINTER_THUMBNAIL_SIZE (wxSize(FromDIP(48), FromDIP(48)))
#define PRINTER_THUMBNAIL_SIZE_SMALL (wxSize(FromDIP(32), FromDIP(32)))
#define PRINTER_PANEL_SIZE_SMALL (wxSize(FromDIP(98), FromDIP(68)))
#define PRINTER_PANEL_SIZE_WIDEN (wxSize(FromDIP(136), FromDIP(68)))
#define PRINTER_PANEL_SIZE (wxSize(FromDIP(98), FromDIP(98)))


bool Plater::has_illegal_filename_characters(const wxString& wxs_name)
{
    return GUI::app::ProjectSession::has_illegal_filename_characters(wxs_name);
}

bool Plater::has_illegal_filename_characters(const std::string& name)
{
    return GUI::app::ProjectSession::has_illegal_filename_characters(name);
}

void Plater::show_illegal_characters_warning(wxWindow* parent)
{
    show_error(parent, _L("Invalid name, the following characters are not allowed:") + " <>:/\\|?*\"");
}

enum SlicedInfoIdx
{
    siFilament_m,
    siFilament_mm3,
    siFilament_g,
    siMateril_unit,
    siCost,
    siEstimatedTime,
    siWTNumbetOfToolchanges,
    siCount
};

enum class LoadFilesType {
    NoFile,
    Single3MF,
    SingleOther,
    Multiple3MF,
    MultipleOther,
    Multiple3MFOther,
};

enum class LoadType : unsigned char
{
    Unknown,
    OpenProject,
    LoadGeometry,
    LoadConfig
};
LoadType determine_load_type(std::string filename, std::string override_setting = "");
void read_binary_stl(const std::string& filename, std::string& model_id, std::string& code);

class SlicedInfo : public wxStaticBoxSizer
{
public:
    SlicedInfo(wxWindow *parent);
    void SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label="");

private:
    std::vector<std::pair<wxStaticText*, wxStaticText*>> info_vec;
};

SlicedInfo::SlicedInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _L("Sliced Info")), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());
    wxGetApp().UpdateDarkUI(GetStaticBox());

    auto *grid_sizer = new wxFlexGridSizer(2, 5, 15);
    grid_sizer->SetFlexibleDirection(wxVERTICAL);

    info_vec.reserve(siCount);

    auto init_info_label = [this, parent, grid_sizer](wxString text_label) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label);
        text->SetForegroundColour(*wxBLACK);
        text->SetFont(wxGetApp().small_font());
        auto info_label = new wxStaticText(parent, wxID_ANY, "N/A");
        info_label->SetForegroundColour(*wxBLACK);
        info_label->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        grid_sizer->Add(info_label, 0);
        info_vec.push_back(std::pair<wxStaticText*, wxStaticText*>(text, info_label));
    };

    init_info_label(_L("Used Filament (m)"));
    init_info_label(_L("Used Filament (mm³)"));
    init_info_label(_L("Used Filament (g)"));
    init_info_label(_L("Used Materials"));
    init_info_label(_L("Cost"));
    init_info_label(_L("Estimated time"));
    init_info_label(_L("Filament changes"));

    Add(grid_sizer, 0, wxEXPAND);
    this->Show(false);
}

void SlicedInfo::SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label/*=""*/)
{
    const bool show = text != "N/A";
    if (show)
        info_vec[idx].second->SetLabelText(text);
    if (!new_label.IsEmpty())
        info_vec[idx].first->SetLabelText(new_label);
    info_vec[idx].first->Show(show);
    info_vec[idx].second->Show(show);
}

static wxString temp_dir;

class PlaterDropTarget : public wxFileDropTarget
{
public:
    PlaterDropTarget(MainFrame& mainframe, Plater& plater) : m_mainframe(mainframe), m_plater(plater) {
        this->SetDefaultAction(wxDragCopy);
    }

    virtual bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames);

private:
    MainFrame& m_mainframe;
    Plater& m_plater;
};

namespace {
bool emboss_svg(Plater& plater, const wxString &svg_file, const Vec2d& mouse_drop_position)
{
    std::string svg_file_str = into_u8(svg_file);
    GLCanvas3D* canvas = plater.canvas3D();
    if (canvas == nullptr)
        return false;
    auto base_svg = canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg);
    if (base_svg == nullptr)
        return false;
    GLGizmoSVG* svg = dynamic_cast<GLGizmoSVG *>(base_svg);
    if (svg == nullptr)
        return false;

    // Refresh hover state to find surface point under mouse
    wxMouseEvent evt(wxEVT_MOTION);
    evt.SetPosition(wxPoint(mouse_drop_position.x(), mouse_drop_position.y()));
    canvas->on_mouse(evt); // call render where is call GLCanvas3D::_picking_pass()

    return svg->create_volume(svg_file_str, mouse_drop_position, ModelVolumeType::MODEL_PART);
}
}



// TODO: listen on dark ui change

// Plater / private


bool PlaterDropTarget::OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames)
{
#ifdef WIN32
    // hides the system icon
    this->MSWUpdateDragImageOnLeave();
#endif // WIN32

    m_mainframe.Raise();
    // Do not force the 3D editor before we even know what was dropped. When a
    // G-code is already loaded (only-gcode mode) the user is on the Preview
    // tab: load_gcode()'s same-file guard switches straight back to Preview
    // on a repeat drop, and the other load paths select their own final view,
    // so switching here would only flash the (empty) 3D editor.
    if (!m_plater.only_gcode_mode()) {
        m_mainframe.select_tab(size_t(MainFrame::tp3DEditor));
        if (wxGetApp().is_editor())
            m_plater.select_view_3D("3D");
    }

    // When only one .svg file is dropped on scene
    if (filenames.size() == 1) {
        const wxString &filename = filenames.Last();
        const wxString  file_extension = filename.substr(filename.length() - 4);
        if (file_extension.CmpNoCase(".svg") == 0) {
            // BBS: GUI refactor: move sidebar to the left
            const wxPoint offset  = m_plater.GetPosition() + m_plater.layout().current_panel->GetPosition();
            Vec2d mouse_position(x - offset.x, y - offset.y);
            // Scale for retina displays
            const GLCanvas3D *canvas = m_plater.canvas3D();
            canvas->apply_retina_scale(mouse_position);
            return emboss_svg(m_plater, filename, mouse_position);
        }
    }
    bool res = m_plater.load_files(filenames);
    m_mainframe.update_title();
    return res;
}

namespace {
void publish(Model &model, SaveStrategy strategy);
}

Plater::BatchPhysicalDeletionGuard::BatchPhysicalDeletionGuard(Plater &p) : m_plater(p) { (void)m_plater; wxGetApp().filament_svc().inc_batch_physical_deletion(); }
Plater::BatchPhysicalDeletionGuard::~BatchPhysicalDeletionGuard() { wxGetApp().filament_svc().dec_batch_physical_deletion(); }

app::SnapshotService& Plater::snapshots() const { return wxGetApp().snapshots(); }
app::NotificationHub& Plater::notification_hub() const { return wxGetApp().notification_hub(); }
app::PlateController& Plater::plates() const { return wxGetApp().plates(); }
app::SlicingController& Plater::slicing() const { return wxGetApp().slicing(); }
app::ProjectSession& Plater::session() const { return wxGetApp().session(); }
app::ExportSendCoordinator& Plater::ex() const { return wxGetApp().ex(); }
app::SelectionQuery& Plater::selq() const { return wxGetApp().selq(); }
app::ObjectEditCommands& Plater::obj_cmds() const { return wxGetApp().obj_cmds(); }
app::ArrangementService& Plater::arrange_svc() const { return wxGetApp().arrange_svc(); }
app::MixingGateKeeper& Plater::gate_keeper() const { return wxGetApp().gate_keeper(); }
app::FilamentService& Plater::filament_svc() const { return wxGetApp().filament_svc(); }
app::CalibrationFlow& Plater::calibration_svc() const { return wxGetApp().calibration_svc(); }
app::LayoutManager& Plater::layout() const { return wxGetApp().layout(); }
app::SceneRefreshCoordinator& Plater::refresh() const { return wxGetApp().refresh(); }
app::PlaterEventRouter& Plater::router() const { return wxGetApp().router(); }
NotificationManager* Plater::notification_manager() const { return wxGetApp().notification_hub().nm(); }
bool Plater::background_processing_enabled() const {
#ifdef SUPPORT_BACKGROUND_PROCESSING
    return get_config("background_processing") == "1";
#else
    return false;
#endif
}
bool Plater::is_slicing_in_progress() const {
    return slicing().is_slicing() || slicing().background_process().running();
}
void Plater::show_delayed_error_message() {
    if (refresh().has_delayed_error_message()) {
        std::string msg = refresh().take_delayed_error_message();
        GUI::show_error(this, msg);
    }
}
void Plater::single_snapshots_enter(SingleSnapshot *single) {
    if (m_single == nullptr) m_single = single;
}
void Plater::single_snapshots_leave(SingleSnapshot *single) {
    if (m_single == single) m_single = nullptr;
}

void Plater::plater_init()
{
    snapshots().late_bind(slicing(), session(), layout(), refresh());
    ex().late_bind(selq(), layout(), refresh(), calibration_svc());
    session().late_bind(ex(), selq(), layout(), refresh(), gate_keeper());
    slicing().late_bind(session(), ex(), arrange_svc(), gate_keeper(), calibration_svc(), layout(), refresh());

    notification_hub().late_bind(slicing(), session(), layout(), refresh());

    plates().late_bind(slicing(), layout(), refresh());
    gate_keeper().late_bind(layout());
    calibration_svc().late_bind(layout(), refresh());
    obj_cmds().late_bind(layout(), refresh());
    filament_svc().late_bind(layout());

// H1 2.3: SlicingControllerHost assembly removed (111 anchors typed: F2 ctor x12 + late_bind x7 + F3 q faces; 13 dead slots dropped).

// H1 2.3: ProjectSessionHost assembly removed (79 slots typed: F2 ctor + late_bind + F3 q bridges).
// H1 2.2: LayoutManagerHost removed - F2 ctor inject + refresh late_bind + F3 q bridges.
    layout().late_bind(refresh());


// H1 2.3: SceneRefreshHost assembly removed (21 slots typed: all ctor inject, zero late_bind).
    notification_hub().set_is_dark(wxGetApp().app_config->get("dark_color_mode") == "1");

    layout().aui()->SetManagedWindow(this);
    layout().aui()->SetDockSizeConstraint(1, 1);
    //layout().aui()->GetArtProvider()->SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 0);
    //layout().aui()->GetArtProvider()->SetMetric(wxAUI_DOCKART_SASH_SIZE, 2);
    layout().aui()->GetArtProvider()->SetMetric(wxAUI_DOCKART_CAPTION_SIZE, 18);
    layout().aui()->GetArtProvider()->SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);

    SetFont(Slic3r::GUI::wxGetApp().normal_font());

    //BBS: use the first partplate's print for background process
    partplate_list->update_slice_context_to_current_plate(slicing().background_process());
    /*
    slicing().background_process().set_fff_print(&slicing().fff_print());
    slicing().background_process().set_sla_print(&slicing().sla_print());
    slicing().background_process().set_gcode_result(&slicing().gcode_result());
    slicing().background_process().set_thumbnail_cb([this](const ThumbnailsParams& params) { return this->generate_thumbnails(params, Camera::EType::Ortho); });
    slicing().background_process().set_slicing_completed_event(EVT_SLICING_COMPLETED);
    slicing().background_process().set_finished_event(EVT_PROCESS_COMPLETED);
    slicing().background_process().set_export_began_event(EVT_EXPORT_BEGAN);
    // Default printer technology for default config.
    slicing().background_process().select_technology(session().printer_technology);
    // Register progress callback from the Print class to the Plater.

    auto statuscb = [this](const Slic3r::PrintBase::SlicingStatus &status) {
        wxQueueEvent(this, new Slic3r::SlicingStatusEvent(EVT_SLICING_UPDATE, 0, status));
    };
    slicing().fff_print().set_status_callback(statuscb);
    slicing().sla_print().set_status_callback(statuscb); */

    // BBS: to be checked. Not follow patch.
    slicing().background_process().set_thumbnail_cb([this](const ThumbnailsParams& params) { return this->generate_thumbnails(params, Camera::EType::Ortho); });
    slicing().background_process().set_slicing_completed_event(EVT_SLICING_COMPLETED);
    slicing().background_process().set_finished_event(EVT_PROCESS_COMPLETED);
    slicing().background_process().set_export_began_event(EVT_EXPORT_BEGAN);
    slicing().background_process().set_export_finished_event(EVT_EXPORT_FINISHED);

    auto* panel_3d = new wxPanel(this);
    layout().view3D = new View3D(panel_3d, layout().bed, &m_model, m_config, &slicing().background_process());
    //BBS: use partplater's gcode
    layout().preview = new Preview(panel_3d, layout().bed, &m_model, m_config, &slicing().background_process(), partplate_list->get_current_slice_result(), [this]() { schedule_background_process(); });

    layout().assemble_view = new AssembleView(panel_3d, layout().bed, &m_model, m_config, &slicing().background_process());

#ifdef __APPLE__
    // BBS
    // set default view_toolbar icons size equal to GLGizmosManager::Default_Icons_Size
    //view_toolbar.set_icons_size(GLGizmosManager::Default_Icons_Size);
#endif // __APPLE__

    layout().attach_views(layout().view3D, layout().preview, layout().assemble_view);   // 阶段 G3：panels 随服务


    slicing().init_update_timer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &evt)
    {
        if (!slicing().suppress_update_active())
            slicing().update_restart_background_process(false, false);
    });

    refresh().update();   // 阶段 G4：原 priv::update()（ctor 留守路径改写）

    // Orca: Make sidebar dockable
    layout().aui()->AddPane(layout().sidebar, wxAuiPaneInfo()
                                   .Name("sidebar")
                                   .Left()
                                   .CloseButton(false)
                                   .TopDockable(false)
                                   .BottomDockable(false)
                                   .Floatable(true)
                                   .BestSize(wxSize(42 * wxGetApp().em_unit(), 90 * wxGetApp().em_unit())));

    auto* panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->Add(layout().view3D, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(layout().preview, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(layout().assemble_view, 1, wxEXPAND | wxALL, 0);
    panel_3d->SetSizer(panel_sizer);
    layout().aui()->AddPane(panel_3d, wxAuiPaneInfo().Name("main").CenterPane().PaneBorder(false));

    layout().m_default_window_layout = layout().aui()->SavePerspective();

    {
        auto& sidebar = layout().aui()->GetPane(layout().sidebar);

        // Load previous window layout
        const auto cfg    = wxGetApp().app_config;
        wxString   layout_str = wxString::FromUTF8(cfg->get("window_layout"));   // 阶段 G3：局部 layout 改名（避让 priv::layout() accessor）
        if (!layout_str.empty()) {
            layout().aui()->LoadPerspective(layout_str, false);
            layout().sidebar_layout.is_collapsed = !sidebar.IsShown();
        }


        // Hide sidebar initially, will re-show it after initialization when we got proper window size
        sidebar.Hide();
        layout().aui()->Update();
    }

    menus->init(main_frame);



// H1 2.2: PlaterEventRouterHost removed - F2 ctor full inject (last service :842, no late deps) + F3x16 q bridges.
    router().install();   // EVT_* consts shared via PlaterEventRouter.hpp wxDECLARE_EVENT


    // Load the 3DConnexion device database.
    layout().mouse3d_controller.load_config(*wxGetApp().app_config);
    // Start the background thread to detect and connect to a HID device (Windows and Linux).
    // Connect to a 3DConnextion driver (OSX).
    layout().mouse3d_controller.init();
    //notification_manager = new NotificationManager(this);

    if (wxGetApp().is_editor()) {
        // Start the background thread and register this window as a target for update events.
        wxGetApp().removable_drive_manager()->init(this);
    }

    // Initialize the Undo / Redo stack with a first snapshot.
    //this->take_snapshot("New Project", UndoRedo::SnapshotType::ProjectSeparator);
    // Reset the "dirty project" flag.
    snapshots().undo_redo_stack_main().mark_current_as_saved();
    session().update_dirty_from_stack(false);
    //this->take_snapshot("New Project");
    // BBS: save project confirm
    up_to_date(true, false);
    up_to_date(true, true);
    m_model.set_need_backup();

    wxGetApp().other_instance_message_handler()->init(this);

    // collapse sidebar according to saved value
    //if (wxGetApp().is_editor()) {
    //    bool is_collapsed = wxGetApp().app_config->get("collapsed_sidebar") == "1";
    //    layout().sidebar->collapse(is_collapsed);
    //}
    layout().update_sidebar(true);

}




wxColour Plater::get_next_color_for_filament()
{
    return app::FilamentService::get_next_color_for_filament();
}

wxString Plater::get_slice_warning_string(GCodeProcessorResult::SliceWarning& warning)
{
    if (warning.msg == BED_TEMP_TOO_HIGH_THAN_FILAMENT) {
        return _L("The current hot bed temperature is relatively high. The nozzle may be clogged when printing this filament in a closed enclosure. Please open the front door and/or remove the upper glass.");
    } else if (warning.msg == NOZZLE_HRC_CHECKER) {
        return _L("The nozzle hardness required by the filament is higher than the default nozzle hardness of the printer. Please replace the hardened nozzle or filament, otherwise, the nozzle will be attrited or damaged.");
    } else if (warning.msg == NOT_SUPPORT_TRADITIONAL_TIMELAPSE) {
        return _L("Enabling traditional timelapse photography may cause surface imperfections. It is recommended to change to smooth mode.");
    } else if (warning.msg == NOT_GENERATE_TIMELAPSE) {
        return wxString();
    }
    else {
        return wxString(warning.msg);
    }
}


//BBS: add no slice option











void Plater::reset_all_gizmos() { obj_cmds().reset_all_gizmos(); }

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.

// BBS
std::string Plater::get_config(const std::string &key) const
{
    return wxGetApp().app_config->get(key);
}


void read_binary_stl(const std::string& filename, std::string& model_id, std::string& code) {
    std::ifstream file( encode_path(filename.c_str()), std::ios::binary);
    if (!file) {
        return;
    }

    try {
        // Read the first 80 bytes
        char data[80];
        file.read(data, 80);
        if (!file) {
            file.close();
            return;
        }

        if (data[0] == '\0' || data[0] == ' ') {
            file.close();
            return;
        }

        char magic[2] = { data[0], data[1] };
        if (magic[0] != 'M' || magic[1] != 'W') {
            file.close();
            return;
        }

        if (data[2] != ' ') {
            file.close();
            return;
        }

        char protocol_version[3] = { data[3], data[4], data[5] };

        //version
        if (protocol_version[0] != '1' || protocol_version[1] != '.' || protocol_version[2] != '0') {
            file.close();
            return;
        }

        std::vector<char*> tokens;
        std::istringstream iss(data);
        std::string token;
        while (std::getline(iss, token, ' ')) {
            char* tokenPtr = new char[token.length() + 1];
            std::strcpy(tokenPtr, token.c_str());
            tokens.push_back(tokenPtr);
        }

        //model id
        if (tokens.size() < 4) {
            file.close();
            return;
        }

        model_id = tokens[2];
        code = tokens[3];
        file.close();
    }
    catch (...) {
    }
    return;
}

// BBS: backup & restore


std::vector<size_t> Plater::load_model_objects(const ModelObjectPtrs& model_objects, bool allow_negative_z, bool split_object) { return session().load_model_objects(model_objects, allow_negative_z, split_object); }

// BBS
void Plater::load_auxiliary_files()
{
    std::string auxiliary_path = encode_path(m_model.get_auxiliary_file_temp_path().c_str());
    //wxGetApp().mainframe->m_project->Reload(auxiliary_path);
}

fs::path Plater::get_export_file_path(GUI::FileType file_type)
{
    return ex().get_export_file_path((int)file_type);
}

wxString Plater::get_export_file(GUI::FileType file_type)
{
    return ex().get_export_file((int)file_type);
}




const Selection& Plater::get_curr_selection()
{
    return selq().get_curr_selection();
}



int Plater::get_selected_volume_idx() const
{
    return selq().get_selected_volume_idx();
}


void Plater::select_curr_plate_all()
{
    layout().view3D->select_curr_plate_all();
    this->layout().sidebar->obj_list()->update_selections();
}

void Plater::remove_curr_plate_all()
{
    SingleSnapshot ss(this);
    layout().view3D->remove_curr_plate_all();
    this->layout().sidebar->obj_list()->update_selections();

    notify_filament_usage_changed();
}

void Plater::select_all()      { obj_cmds().select_all(); }






void Plater::reset(bool apply_presets_change)
{
    app::SnapshotService::Scope snapshot(this, "Reset Project", UndoRedo::SnapshotType::ProjectSeparator);

    notification_hub().clear_warnings();
    layout().first_enter_assemble = true;

    session().set_project_filename("");
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " call set_project_filename: empty";

    if (layout().view3D->is_layers_editing_enabled())
        layout().view3D->get_canvas3d()->force_main_toolbar_left_action(layout().view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    layout().view3D->get_canvas3d()->reset_all_gizmos();

    refresh().reset_gcode_toolpaths();   // 阶段 G4：原 priv::reset_gcode_toolpaths()（priv::reset 留守改写）
    // Explicitly free the previous slice's GCodeProcessorResult data before
    // the partplate list is reinit'd.  Without this, large moves vectors
    // (potentially several GB) from the previous slice stay alive until the
    // new slice's do_export replaces them — a major contributor to crashes
    // when loading a new 3MF after a heavy slice.
    {
        GCodeProcessorResult* prev_result = this->slicing().background_process().get_current_gcode_result();
        if (prev_result != nullptr) {
            BOOST_LOG_TRIVIAL(info) << "priv::reset: freeing GCodeProcessorResult moves="
                << prev_result->moves.size();
            prev_result->moves.clear();
            prev_result->moves.shrink_to_fit();
            prev_result->lines_ends.clear();
            prev_result->lines_ends.shrink_to_fit();
        }
    }
    // Reset the skip flag so a newly loaded project renders normally.
    // Without this, the flag from a previous heavy-slice session persists
    // and makes even small models show an empty preview.
    layout().preview->set_skip_toolpath_preview(false);

    layout().view3D->get_canvas3d()->reset_sequential_print_clearance();

    m_worker->cancel_all();

    //BBS: clear the partplate list's object before object cleared
    partplate_list->reinit();
    partplate_list->update_slice_context_to_current_plate(slicing().background_process());
    layout().preview->update_gcode_result(partplate_list->get_current_slice_result());

    // Stop and reset the Print content.
    this->slicing().background_process().reset();
    m_model.clear_objects();
    layout().assemble_view->get_canvas3d()->reset_explosion_ratio();
    refresh().update();   // 阶段 G4：原 priv::update()（priv::reset 留守改写）

    //BBS
    if (wxGetApp().is_editor()) {
        // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
        layout().sidebar->obj_list()->delete_all_objects_from_list();
        refresh().object_list_changed();   // 阶段 G4：原 priv::object_list_changed()（priv::reset 留守改写）
    }

    session().project.reset();

    //BBS: reset all project embedded presets
    wxGetApp().preset_bundle->reset_project_embedded_presets();
    if (apply_presets_change)
        wxGetApp().apply_keeped_preset_modifications();
    else
        wxGetApp().load_current_presets(false, false);

    //BBS
    m_model.calib_pa_pattern.reset();
    m_model.plates_custom_gcodes.clear();

    // BBS
    session().m_saved_timestamp = session().m_backup_timestamp = size_t(-1);

    // Save window layout
    layout().save_window_layout();   // 阶段 G3：窗口布局持久化随服务（原 :2479-2488 块）
}





void Plater::find_new_position(const ModelInstancePtrs &instances) { arrange_svc().find_new_position(instances); }



void Plater::scale_selection_to_fit_print_volume() { obj_cmds().scale_selection_to_fit_print_volume(); }



void Plater::notify_filament_compatibility_after_apply()
{
    if (session().printer_technology != ptFFF)
        return;
    if (session().only_gcode_mode())
        return;

    Slic3r::Print *print = slicing().background_process().fff_print();
    if (print == nullptr)
        return;

    Slic3r::NozzleFilamentRuleMismatch nozzle_mismatch;
    bool                               isGraphicMatch(false), isPeiBedMatchNotPla(false), isPeiBedMatchTpu(false);

    print->filament_rule_mismatch_flags(nozzle_mismatch, isGraphicMatch, isPeiBedMatchNotPla, isPeiBedMatchTpu,
                                        wxGetApp().preset_bundle);

    wxString filamentMismatchNozzleWarning;
    if (nozzle_mismatch.has_mismatch) {
        const wxString currentNozzle = wxString::FromUTF8(nozzle_mismatch.nozzle_diameter_mm);
        const wxString nozzleType    = nozzle_type_key_to_label(nozzle_mismatch.nozzle_type_key);
        const wxString filamentdata =
            nozzle_mismatch.filament_preset_name.empty() ? _L("(unknown)")
                                                         : wxString::FromUTF8(nozzle_mismatch.filament_preset_name);
        filamentMismatchNozzleWarning =
            wxString::Format(_L("Note: Using a %s mm %s nozzle for %s is not recommended."), currentNozzle, nozzleType, filamentdata);
    }
    wxString filamentMismatchPeiBedMsgNotPla  = wxString(_L("Note: Filament may not adhere well to the smooth PEI plate on the first layer. Apply glue before printing."));
    wxString filamentMismatchPeiBedMsgTpu     = wxString(_L("Note: Filament may stick too strongly to the smooth PEI plate. Apply glue to protect the plate and ease part removal."));
    wxString filamentMismatchGraphicBedMsg = wxString(_L("Note: Low adhesion to the graphic effect plate may cause failure. Use a different filament instead."));
   
    if (isPeiBedMatchTpu && isPeiBedMatchNotPla)
        isPeiBedMatchNotPla = false;

    if (isGraphicMatch || isPeiBedMatchNotPla)
    {
        notification_manager()->close_notification_of_type(NotificationType::CustomNotification);

        if (isGraphicMatch)
            notification_manager()->push_notification(into_u8(filamentMismatchGraphicBedMsg), 0);
        if (isPeiBedMatchNotPla)
            notification_manager()->push_notification(into_u8(filamentMismatchPeiBedMsgNotPla), 0);
        notification_manager()->set_slicing_progress_hidden();
    }

    if (nozzle_mismatch.has_mismatch)
        notification_manager()->push_notification(into_u8(filamentMismatchNozzleWarning), 0);

    if (isPeiBedMatchTpu)
    {
        notification_manager()->push_notification(into_u8(filamentMismatchPeiBedMsgTpu), 0);
    }
    if (gate_keeper().has_incompatible_mixed_filament_in_use()) {
        notification_manager()->push_notification(
            NotificationType::FilamentIncompatibleMixed,
            NotificationManager::NotificationLevel::ErrorNotificationLevel,
            into_u8(_L("Mixed filaments contain incompatible material types. Please correct the mixed filaments settings before slicing.")));
    } else {
        notification_manager()->close_notification_of_type(NotificationType::FilamentIncompatibleMixed);
    }

}


/**
 * Check whether the current plate uses any virtual (mixed) filament whose
 * constituent materials are incompatible.
 *
 * During drag operations the result is cached by MixingGateKeeper (drag-residual cache)
 * to prevent button state flicker.
 * @return true if an incompatible mixed filament is in use on the current plate
 */
/**
 * Centralized slice gate for the current plate.
 * Returns false if the plate cannot be sliced or if an incompatible mixed
 * filament is in use.
 * @return true if the current plate is ready to be sliced
 */
bool Plater::can_current_plate_be_sliced() const { return slicing().can_current_plate_be_sliced(); }


// Update background processing thread from the current config and Model.
// Returns a bitmask of UpdateBackgroundProcessReturnState.
unsigned int Plater::update_background_process(bool force_validation, bool postpone_error_messages, bool switch_print) { return slicing().update_background_process(force_validation, postpone_error_messages, switch_print); }

// Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
bool Plater::restart_background_process(unsigned int state) { return slicing().restart_background_process(state); }

unsigned int Plater::update_restart_background_process(bool force_update_scene, bool force_update_preview) { return slicing().update_restart_background_process(force_update_scene, force_update_preview); }


bool Plater::replace_volume_with_stl(int object_idx, int volume_idx, const fs::path& new_path, const std::string& snapshot) { return session().replace_volume_with_stl(object_idx, volume_idx, new_path, snapshot); }





void Plater::reload_all_from_disk() { session().reload_all_from_disk(); }

//BBS: add no_slice logic

// BBS















//BBS: add project slice logic




//BBS: add plate from toolbar


//BBS: remove plate from toolbar


//BBS: GUI refactor: GLToolbar


//BBS: GUI refactor: slice plate


//BBS: GUI refactor: slice all
































//BBS: add plate select logic






//BBS: add slice button status update logic






//BBS: repair model through netfabb


bool Plater::confirm_auto_generated_gradients(size_t num_physical)
{
    return confirm_auto_generated_gradients(this, num_physical);
}

bool Plater::confirm_auto_generated_gradients(wxWindow *parent, size_t num_physical)
{
    auto *app_config = wxGetApp().app_config;
    if (app_config == nullptr)
        return MixedFilamentManager::auto_generate_enabled();

    const bool pref_enabled = app_config->get_bool("auto_generate_gradients");
    if (!pref_enabled) {
        notification_hub().set_last_auto_gradient_prompt_count(0);
        notification_hub().set_last_auto_gradient_prompt_accepted(false);
        MixedFilamentManager::set_auto_generate_enabled(false);
        return false;
    }

    if (num_physical <= 4) {
        notification_hub().set_last_auto_gradient_prompt_count(0);
        notification_hub().set_last_auto_gradient_prompt_accepted(false);
        MixedFilamentManager::set_auto_generate_enabled(true);
        return true;
    }

    if (parent == nullptr || !parent->IsShownOnScreen()) {
        notification_hub().set_last_auto_gradient_prompt_count(0);
        notification_hub().set_last_auto_gradient_prompt_accepted(false);
        MixedFilamentManager::set_auto_generate_enabled(true);
        return true;
    }

    if (notification_hub().last_auto_gradient_prompt_count() == num_physical) {
        MixedFilamentManager::set_auto_generate_enabled(notification_hub().last_auto_gradient_prompt_accepted());
        return notification_hub().last_auto_gradient_prompt_accepted();
    }

    const size_t auto_gradient_count = num_physical * (num_physical - 1) / 2;
    const wxString message = wxString::Format(
        _L("Using %d physical filaments will create %d auto-generated gradients.\nDo you want to create them now?"),
        int(num_physical),
        int(auto_gradient_count));
    const int result = MessageDialog(parent,
                                     message,
                                     wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Auto gradients"),
                                     wxYES_NO | wxYES_DEFAULT | wxCENTRE | wxICON_QUESTION)
                           .ShowModal();
    const bool accepted = result == wxID_YES;
    notification_hub().set_last_auto_gradient_prompt_count(num_physical);
    notification_hub().set_last_auto_gradient_prompt_accepted(accepted);
    MixedFilamentManager::set_auto_generate_enabled(accepted);
    return accepted;
}






















//BBS: add part plate related logic






// Update the scene from the background processing,
// if the update message was received during mouse manipulation.


//BBS: add plate id for thumbnail generate param
void Plater::generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, Camera::EType camera_type, bool use_top_view, bool for_picking, bool ban_light)
{
    layout().view3D->get_canvas3d()->render_thumbnail(data, w, h, thumbnail_params, camera_type, use_top_view, for_picking, ban_light);
}

//BBS: add plate id for thumbnail generate param
ThumbnailsList Plater::generate_thumbnails(const ThumbnailsParams& params, Camera::EType camera_type)
{
    ThumbnailsList thumbnails;
    for (const Vec2d& size : params.sizes) {
        thumbnails.push_back(ThumbnailData());
        Point isize(size); // round to ints
        generate_thumbnail(thumbnails.back(), isize.x(), isize.y(), params, camera_type);
        if (!thumbnails.back().is_valid())
            thumbnails.pop_back();
    }
    return thumbnails;
}

void Plater::generate_calibration_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params)
{
    layout().preview->get_canvas3d()->render_calibration_thumbnail(data, w, h, thumbnail_params);
}

PlateBBoxData Plater::generate_first_layer_bbox()
{
    PlateBBoxData bboxdata;
    std::vector<BBoxData>& id_bboxes = bboxdata.bbox_objs;
    BoundingBoxf bbox_all;
    auto                   print = this->slicing().background_process().m_fff_print;
    auto curr_plate = this->partplate_list->get_curr_plate();
    auto curr_plate_seq = curr_plate->get_real_print_seq();
    bboxdata.is_seq_print = (curr_plate_seq == PrintSequence::ByObject);
    bboxdata.first_extruder = print->get_tool_ordering().first_extruder();
    bboxdata.bed_type       = bed_type_to_gcode_string(print->config().curr_bed_type.value);
    // get nozzle diameter
    auto opt_nozzle_diameters = print->config().option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters != nullptr)
        bboxdata.nozzle_diameter = float(opt_nozzle_diameters->get_at(bboxdata.first_extruder));
    //PrintObjectPtrs objects;
    //if (this->printer_technology == ptFFF) {
    //    objects = this->slicing().background_process().m_fff_print->objects().vector();
    //}
    //else {
    //    objects = this->slicing().background_process().m_sla_print->objects();
    //}
    auto objects = print->objects();
    auto orig = this->partplate_list->get_curr_plate()->get_origin();
    Vec2d orig2d = { orig[0], orig[1] };

    BBoxData data;
    for (auto obj : objects)
    {
        auto bb_scaled = obj->get_first_layer_bbox(data.area, data.layer_height, data.name);
        auto bb = unscaled(bb_scaled);
        bb.min -= orig2d;
        bb.max -= orig2d;
        bbox_all.merge(bb);
        data.area *= (SCALING_FACTOR * SCALING_FACTOR); // unscale area
        data.id = obj->id().id;
        data.bbox = { bb.min.x(),bb.min.y(),bb.max.x(),bb.max.y() };
        id_bboxes.emplace_back(data);
    }

    // add wipe tower bounding box
    if (print->has_wipe_tower()) {
        auto   wt_corners = print->first_layer_wipe_tower_corners();
        // when loading gcode.3mf, wipe tower info may not be correct
        if (!wt_corners.empty()) {
            BoundingBox bb_scaled = {wt_corners[0], wt_corners[2]};
            auto        bb        = unscaled(bb_scaled);
            bb.min -= orig2d;
            bb.max -= orig2d;
            bbox_all.merge(bb);
            data.name = "wipe_tower";
            data.id   = partplate_list->get_curr_plate()->get_index() + 1000;
            data.bbox = {bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()};
            id_bboxes.emplace_back(data);
        }
    }

    bboxdata.bbox_all = { bbox_all.min.x(),bbox_all.min.y(),bbox_all.max.x(),bbox_all.max.y() };
    return bboxdata;
}







//BBS
void Plater::set_project_name(const wxString& project_name)
{
    session().set_project_name(project_name);
}





void Plater::orient(const app::OrientRequest& req) { arrange_svc().orient(req); }   // F3：门面一行转发（原无参版消亡）

void Plater::get_print_job_data(PrintPrepareData* data)
{
    ex().get_print_job_data(data);
}

int Plater::get_send_calibration_finished_event()
{
    return EVT_SEND_CALIBRATION_FINISHED;
}

int Plater::get_print_finished_event()
{
    return EVT_PRINT_FINISHED;
}

int Plater::get_send_finished_event()
{
    return EVT_SEND_FINISHED;
}



#if 0
void Plater::update_partplate()
{
    sidebar().update_partplate(partplate_list);
}
#endif









#if ENABLE_ENHANCED_PRINT_VOLUME_FIT

#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT





void Plater::update_publish_dialog_status(wxString &msg, int percent)
{
    ex().update_publish_dialog_status(msg, percent);
}


//BBS: add bed exclude area













bool Plater::layers_height_allowed() const { return obj_cmds().layers_height_allowed(); }




/**
 * Show a warning when both Variable Layer Height and Subdivide Mix Layer
 * (dithering_local_z_mode) are enabled, as both features alter layer
 * heights and may produce unexpected results.
 * @param local_z_enabled true if the Subdivide Mix Layer setting is active
 */
bool Plater::has_incompatible_mixed_filament_in_use() const
{
    return gate_keeper().has_incompatible_mixed_filament_in_use();
}

void Plater::notify_vhl_dithering_conflict(bool local_z_enabled)
{
    notification_hub().notify_vhl_dithering_conflict(local_z_enabled);
}











// BBS: check need save or backup


void Plater::bring_instance_forward()
{
#ifdef __APPLE__
    wxGetApp().other_instance_message_handler()->bring_instance_forward();
    return;
#endif //__APPLE__
    if (main_frame == nullptr) {
        BOOST_LOG_TRIVIAL(debug) << "Couldnt bring instance forward - mainframe is null";
        return;
    }
    BOOST_LOG_TRIVIAL(debug) << "Snapmaker Orca window going forward";
    //this code maximize app window on Fedora
    {
        main_frame->Iconize(false);
        if (main_frame->IsMaximized())
            main_frame->Maximize(true);
        else
            main_frame->Maximize(false);
    }
    //this code maximize window on Ubuntu
    {
        main_frame->Restore();
        wxGetApp().GetTopWindow()->SetFocus();  // focus on my window
        wxGetApp().GetTopWindow()->Raise();  // bring window to front
        wxGetApp().GetTopWindow()->Show(true); // show the window
    }
}

//BBS: popup object table
bool Plater::PopupObjectTable(int object_id, int volume_id, const wxPoint& position)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, create ObjectTableDialog");
    int max_width{1920}, max_height{1080};

    max_width = GetMaxWidth();
    max_height = GetMaxHeight();
    ObjectTableDialog table_dialog(this, this, &m_model, wxSize(max_width, max_height));
    //m_popup_table = new ObjectTableDialog(this, this,  &model);

    wxRect rect = layout().sidebar->GetRect();
    wxPoint pos = layout().sidebar->ClientToScreen(wxPoint(rect.x, rect.y));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": show ObjectTableDialog");
    table_dialog.Popup(object_id, volume_id, pos);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" finished, will destroy ObjectTableDialog");
    return true;
}


// Plater / Public

Plater::Plater(wxWindow *parent, MainFrame *main_frame)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxGetApp().get_min_size())
    , main_frame(main_frame)
    , m_config(Slic3r::DynamicPrintConfig::new_from_defaults_keys({
        "printable_area", "bed_exclude_area", "bed_custom_texture", "bed_custom_model", "print_sequence",
        "extruder_clearance_radius", "extruder_clearance_height_to_lid", "extruder_clearance_height_to_rod",
		"nozzle_height", "skirt_type", "skirt_loops", "skirt_speed","min_skirt_length", "skirt_distance", "skirt_start_angle",
        "brim_width", "brim_object_gap", "brim_type", "nozzle_diameter", "single_extruder_multi_material", "preferred_orientation",
        "enable_prime_tower", "wipe_tower_x", "wipe_tower_y", "prime_tower_width", "prime_tower_brim_width", "prime_volume",
        "extruder_colour", "filament_colour", "material_colour", "printable_height", "printer_model", "printer_technology",
        // These values are necessary to construct SlicingParameters by the Canvas3D variable layer height editor.
        "layer_height", "initial_layer_print_height", "min_layer_height", "max_layer_height",
        "brim_width", "wall_loops", "wall_filament", "sparse_infill_density", "sparse_infill_filament", "solid_infill_filament", "top_shell_layers",
        "enable_support", "support_filament", "support_interface_filament",
        "support_top_z_distance", "support_bottom_z_distance", "raft_layers",
        "wipe_tower_rotation_angle", "wipe_tower_cone_angle", "wipe_tower_extra_spacing", "wipe_tower_extra_flow", "local_z_wipe_tower_purge_lines", "wipe_tower_max_purge_speed",
        "wipe_tower_wall_type", "wipe_tower_extra_rib_length","wipe_tower_rib_width","wipe_tower_fillet_wall",
        "wipe_tower_filament", "wipe_tower_wall_gap", "prime_tower_enable_framework",
        "best_object_pos"
        }))
        , partplate_list(std::make_unique<PartPlateList>(this, &m_model))
    , m_sla_import_dlg(new SLAImportDialog{this})
{
    GUI_App& app = wxGetApp();
    app.m_notification_hub = std::make_unique<app::NotificationHub>(this, m_model, *partplate_list);
    m_worker.reset(new PlaterWorker<BoostThreadWorker>(this, std::make_unique<NotificationProgressIndicator>(app.notification_hub().nm()), "ui_worker"));
    app.m_snapshots = std::make_unique<app::SnapshotService>(this, m_model, *partplate_list);
    app.m_plates = std::make_unique<app::PlateController>(this, *partplate_list, app.snapshots(), app.notification_hub(), m_config);
    app.m_slicing = std::make_unique<app::SlicingController>(this, *partplate_list, m_model, m_config, app.notification_hub(), app.snapshots(), app.plates(), *m_worker, main_frame, m_fff_print, m_sla_print, gcode_result);
    app.m_session = std::make_unique<app::ProjectSession>(this, &m_model, m_config, app.snapshots(), app.slicing(), *partplate_list, app.notification_hub(), app.plates(), main_frame);
    app.m_export_send = std::make_unique<app::ExportSendCoordinator>(this, &m_model, m_config, app.session(), app.slicing(), *partplate_list, app.notification_hub(), main_frame);
    app.m_sel_query = std::make_unique<app::SelectionQuery>(this, &m_model);
    app.m_obj_cmds = std::make_unique<app::ObjectEditCommands>(this, &m_model, m_config, app.selq(), app.session(), app.slicing(), app.notification_hub(), app.snapshots(), *partplate_list, *m_worker);
    app.m_arrange_svc = std::make_unique<app::ArrangementService>(this, &m_model, *m_worker, app.snapshots());
    app.m_gate_keeper = std::make_unique<app::MixingGateKeeper>(app.notification_hub(), this, *partplate_list, m_config, m_model, main_frame);
    app.m_filament_svc = std::make_unique<app::FilamentService>(app.notification_hub(), this, m_config, *partplate_list, main_frame, app.slicing(), filament_usage_sync_pending);
    app.m_calib_svc = std::make_unique<app::CalibrationFlow>(app.notification_hub(), this, m_model, *partplate_list, main_frame, app.session(), app.slicing(), app.obj_cmds());
    app.m_layout = std::make_unique<app::LayoutManager>(this, app.notification_hub(), app.session(), app.slicing(), *partplate_list, m_model, main_frame);
    app.m_refresh = std::make_unique<app::SceneRefreshCoordinator>(app.notification_hub(), this, m_model, *partplate_list, app.slicing(), app.session(), app.layout(), app.obj_cmds(), main_frame);
    menus = std::make_unique<MenuFactory>(obj_cmds(), selq());   // H2 2.3：§8 根治项——MenuFactory 构造注入 OE/SQ
    app.m_router = std::make_unique<app::PlaterEventRouter>(this, app.notification_hub(), app.slicing(), app.session(), app.ex(), app.plates(), app.obj_cmds(), app.layout(), app.refresh(), app.snapshots(), main_frame, *menus, *partplate_list, m_model, filament_usage_sync_pending);

    enable_wireframe(true);
    session().set_only_gcode(false);
    plater_init();
}

Plater::~Plater()
{
    m_worker.reset();
    if (m_config != nullptr)
        delete m_config;
    // Saves the database of visited (already shown) hints into hints.ini.
    notification_manager()->deactivate_loaded_hints();
    wxGetApp().shutdown_services();
}


bool Plater::Show(bool show)
{
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->show_option(show);
    return wxPanel::Show(show);
}

bool Plater::only_gcode_mode() { return session().only_gcode_mode(); }

bool Plater::using_exported_file() { return session().using_exported_file(); }



bool Plater::is_loading_project() const { return session().is_loading_project(); }

void Plater::set_plater_dirty(bool is_dirty) { session().set_plater_dirty(is_dirty); }
void Plater::update_project_dirty_from_presets() { session().update_project_dirty_from_presets(); }
void Plater::reset_project_dirty_initial_presets() { session().reset_project_dirty_initial_presets(); }
#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
void Plater::render_project_state_debug_window() const { session().dirty_state.render_debug_window(); }
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

Sidebar&        Plater::sidebar()           { return *layout().sidebar; }
const Model&    Plater::model() const       { return m_model; }
Model&          Plater::model()             { return m_model; }
const Print&    Plater::fff_print() const   { return slicing().fff_print(); }
Print&          Plater::fff_print()         { return slicing().fff_print(); }

int Plater::new_project(bool skip_confirm, bool silent, const wxString& project_name) { return session().new_project(skip_confirm, silent, project_name); }

LoadType determine_load_type(std::string filename, std::string override_setting);

// BBS: FIXME, missing resotre logic
// BBS: FIXME, missing resotre logic

// BBS: save logic
// BBS: save logic


//BBS import model by model id
void Plater::import_model_id(wxString download_info) { session().import_model_id(std::move(download_info)); }
//BBS download project by project id


void Plater::request_model_download(wxString url) { session().request_model_download(std::move(url)); }

void Plater::request_download_project(std::string project_id)
{
    wxCommandEvent* event = new wxCommandEvent(EVT_DOWNLOAD_PROJECT);
    event->SetString(project_id);
    wxQueueEvent(this, event);
}

// BBS: save logic
bool Plater::up_to_date(bool saved, bool backup)
{
    if (saved) {
        Slic3r::clear_other_changes(backup);
        return session().up_to_date(saved, backup);
    }
    return m_model.objects.empty() || (session().up_to_date(saved, backup) &&
                                        !Slic3r::has_other_changes(backup));
}


void Plater::calib_pa(const Calib_Params& params) { calibration_svc().calib_pa(params); }


void Plater::calib_temp(const Calib_Params& params) { calibration_svc().calib_temp(params); }

void Plater::calib_max_vol_speed(const Calib_Params& params) { calibration_svc().calib_max_vol_speed(params); }

void Plater::calib_retraction(const Calib_Params& params) { calibration_svc().calib_retraction(params); }

void Plater::calib_VFA(const Calib_Params& params) { calibration_svc().calib_VFA(params); }

void Plater::calib_input_shaping_freq(const Calib_Params& params) { calibration_svc().calib_input_shaping_freq(params); }

void Plater::calib_input_shaping_damp(const Calib_Params& params) { calibration_svc().calib_input_shaping_damp(params); }

void Plater::calib_junction_deviation(const Calib_Params& params) { calibration_svc().calib_junction_deviation(params); }

BuildVolume_Type Plater::get_build_volume_type() const { return layout().bed.get_build_volume_type(); }







//BBS: remove GCodeViewer as seperate APP logic


// BBS
wxString Plater::get_project_name()
{
    return session().get_project_name();
}

void Plater::update_all_plate_thumbnails(bool force_update)
{
    notification_hub().update_all_plate_thumbnails(force_update);
}

//invalid all plate's thumbnails
void Plater::invalid_all_plate_thumbnails()
{
    notification_hub().invalid_all_plate_thumbnails();
}

void Plater::force_update_all_plate_thumbnails()
{
    notification_hub().force_update_all_plate_thumbnails();
}

std::vector<size_t> Plater::load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi) { return session().load_files(input_files, strategy, ask_multi); }

// To be called when providing a list of files to the GUI slic3r on command line.
std::vector<size_t> Plater::load_files(const std::vector<std::string>& input_files, LoadStrategy strategy,  bool ask_multi) { return session().load_files(input_files, strategy, ask_multi); }


#define PROJECT_DROP_DIALOG_SELECT_PLANE_SIZE wxSize(FromDIP(350), FromDIP(120))

class ProjectDropDialog : public DPIDialog
{
private:
    wxColour          m_def_color = wxColour(255, 255, 255);
    int               m_action{1};
    bool              m_remember_choice{false};

public:
    ProjectDropDialog(const std::string &filename);

    wxPanel *     m_top_line;
    wxStaticText *m_fname_title;
    wxStaticText *m_fname_f;
    wxStaticText *m_fname_s;
    StaticBox * m_panel_select;

    void      on_select_ok(wxCommandEvent &event);
    void      on_select_cancel(wxCommandEvent &event);

    int       get_action() const { return m_action; }
    void      set_action(int index) { m_action = index; }

    wxBoxSizer *create_remember_checkbox(wxString title, wxWindow* parent, wxString tooltip);
    wxBoxSizer *create_item_radiobox(wxString title, wxWindow *parent, int select_id, int groupid);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

ProjectDropDialog::ProjectDropDialog(const std::string &filename)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                from_u8((boost::format(_utf8(L("Drop project file")))).str()),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_action(2)
{
    // def setting
    SetBackgroundColour(m_def_color);

    // icon
    std::string icon_path = (boost::format("%1%/images/Snapmaker_OrcaTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, 20);

    wxBoxSizer *m_sizer_name = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *m_sizer_fline = new wxBoxSizer(wxHORIZONTAL);

    m_fname_title = new wxStaticText(this, wxID_ANY, _L("Please select an action"), wxDefaultPosition, wxDefaultSize, 0);
    m_fname_title->Wrap(-1);
    m_fname_title->SetFont(::Label::Body_14);
    m_fname_title->SetForegroundColour(wxColour(107, 107, 107));
    m_fname_title->SetBackgroundColour(wxColour(255, 255, 255));

    m_sizer_fline->Add(m_fname_title, 0, wxALL, 0);
    m_sizer_fline->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    m_fname_f = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_fname_f->SetFont(::Label::Head_14);
    m_fname_f->Wrap(-1);
    m_fname_f->SetForegroundColour(wxColour(38, 46, 48));

    m_sizer_fline->Add(m_fname_f, 1, wxALL, 0);

    m_sizer_name->Add(m_sizer_fline, 1, wxEXPAND, 0);

    m_fname_s = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_fname_s->SetFont(::Label::Head_14);
    m_fname_s->Wrap(-1);
    m_fname_s->SetForegroundColour(wxColour(38, 46, 48));

    m_sizer_name->Add(m_fname_s, 1, wxALL, 0);

    m_sizer_main->Add(m_sizer_name, 1, wxEXPAND | wxLEFT | wxRIGHT, 20);

    auto radio_group = new RadioGroup(this, {
        _L("Open as project"),     // 0
        _L("Import geometry only") // 1
    }, wxVERTICAL);
    radio_group->SetMinSize(wxSize(FromDIP(300),-1));
    radio_group->SetSelection(get_action() - 1);
    radio_group->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this, radio_group](wxCommandEvent &e) {
        set_action(radio_group->GetSelection() + 1);
    });

    m_sizer_main->Add(radio_group, 0, wxEXPAND | wxLEFT | wxRIGHT, 20);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, 10);

    // wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);
    // Orca: hide the "Don't show again" checkbox, people keeps accidentally checked this then forgot
    // wxBoxSizer *m_sizer_left = new wxBoxSizer(wxHORIZONTAL);
    //
    // auto dont_show_again = create_remember_checkbox(_L("Remember my choice."), this, _L("This option can be changed later in preferences, under 'Load Behaviour'."));
    // m_sizer_left->Add(dont_show_again, 0, wxALL, 5);
    //
    // m_sizer_bottom->Add(m_sizer_left, 0, wxEXPAND, 5);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &ProjectDropDialog::on_select_ok, this);

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, &ProjectDropDialog::on_select_cancel, this);

    m_sizer_main->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);


    auto limit_width   = m_fname_f->GetSize().GetWidth() - 2;
    auto current_width = 0;
    auto cut_index     = 0;
    auto fstring       = wxString("");
    auto bstring       = wxString("");

    //auto file_name = from_u8(filename.c_str());
    auto file_name = wxString(filename);
    for (int x = 0; x < file_name.length(); x++) {
        current_width += m_fname_s->GetTextExtent(file_name[x]).GetWidth();
        cut_index = x;

        if (current_width > limit_width) {
            bstring += file_name[x];
        } else {
            fstring += file_name[x];
        }
    }

    m_fname_f->SetLabel(fstring);
    m_fname_s->SetLabel(bstring);

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *ProjectDropDialog::create_remember_checkbox(wxString title, wxWindow *parent, wxString tooltip)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    auto checkbox = new ::CheckBox(parent);
    checkbox->SetValue(m_remember_choice);
    checkbox->SetToolTip(tooltip);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144,144,144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    checkbox_title->SetToolTip(tooltip);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox](wxCommandEvent &e) {
        m_remember_choice = checkbox->GetValue();
        e.Skip();
    });

    return m_sizer_checkbox;
}

void ProjectDropDialog::on_select_ok(wxCommandEvent &event)
{
    if (m_remember_choice) {
        LoadType load_type = static_cast<LoadType>(get_action());
        switch (load_type)
        {
            case LoadType::OpenProject:
                wxGetApp().app_config->set(SETTING_PROJECT_LOAD_BEHAVIOUR, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL);
                break;
            case LoadType::LoadGeometry:
                wxGetApp().app_config->set(SETTING_PROJECT_LOAD_BEHAVIOUR, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY);
                break;
        }
    }

    EndModal(wxID_OK);
}

void ProjectDropDialog::on_select_cancel(wxCommandEvent &event)
{
    EndModal(wxID_CANCEL);
}

void ProjectDropDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

//BBS: remove GCodeViewer as seperate APP logic

bool Plater::load_files(const wxArrayString& filenames) { return session().load_files(filenames); }

LoadType determine_load_type(std::string filename, std::string override_setting)
{
    std::string setting;

    if (override_setting != "") {
        setting = override_setting;
    } else {
        setting = wxGetApp().app_config->get(SETTING_PROJECT_LOAD_BEHAVIOUR);
    }

    if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY) {
        return LoadType::LoadGeometry;
    } else if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK) {
        ProjectDropDialog dlg(filename);
        if (dlg.ShowModal() == wxID_OK) {
            int      choice    = dlg.get_action();
            LoadType load_type = static_cast<LoadType>(choice);
            wxGetApp().app_config->set("import_project_action", std::to_string(choice));

            // BBS: jump to plater panel
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            return load_type;
        }

        return LoadType::Unknown; // Cancel
    } else {
        return LoadType::OpenProject;
    }
}








void Plater::update(bool conside_update_flag, bool force_background_processing_update) { refresh().update_gate(conside_update_flag, force_background_processing_update); }



Worker &Plater::get_ui_job_worker() { return *m_worker; }

const Worker &Plater::get_ui_job_worker() const { return *m_worker; }



//BBS: add no_slice logic
void Plater::select_view_3D(const std::string& name, bool no_slice) { layout().select_view_3D(name, no_slice); }

void Plater::reload_paint_after_background_process_apply() {
    layout().preview->set_reload_paint_after_background_process_apply(true);
}

bool Plater::is_preview_shown() const { return layout().is_preview_shown(); }
bool Plater::is_view3D_shown() const { return layout().is_view3D_shown(); }




void Plater::enable_sidebar(bool enabled) { layout().enable_sidebar(enabled); }
bool Plater::is_sidebar_collapsed() const { return layout().sidebar_layout.is_collapsed; }
void Plater::collapse_sidebar(bool collapse) { layout().collapse_sidebar(collapse); }
Sidebar::DockingState Plater::get_sidebar_docking_state() const { return layout().get_sidebar_docking_state(); }

void Plater::reset_window_layout() { layout().reset_window_layout(); }

//BBS



// BBS: save logic

//BBS: trigger a restore project event
void Plater::trigger_restore_project(int skip_confirm) { session().trigger_restore_project(skip_confirm); }

//BBS


//BBS: delete all from model



void Plater::remove_selected() { obj_cmds().remove_selected(); }

void Plater::increase_instances(size_t num)
{
    // BBS
#if 0
    if (! can_increase_instances()) { return; }

    app::SnapshotService::Scope snapshot(this, "Increase Instances");

    int obj_idx = get_selected_object_idx();

    ModelObject* model_object = m_model.objects[obj_idx];
    ModelInstance* model_instance = model_object->instances.back();

    bool was_one_instance = model_object->instances.size()==1;

    double offset_base = canvas3D()->get_size_proportional_to_max_bed_size(0.05);
    double offset = offset_base;
    for (size_t i = 0; i < num; i++, offset += offset_base) {
        Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
        model_object->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(), model_instance->get_mirror());
//        print.get_object(obj_idx)->add_copy(Slic3r::to_2d(offset_vec));
    }

#ifdef SUPPORT_AUTO_CENTER
    if (get_config("autocenter") == "true")
        arrange();
#endif

    update();

    get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    sidebar().obj_list()->increase_object_instances(obj_idx, was_one_instance ? num + 1 : num);

    selection_changed();
    schedule_background_process();
#endif
}

void Plater::decrease_instances(size_t num)
{
    // BBS
#if 0
    if (! can_decrease_instances()) { return; }

    app::SnapshotService::Scope snapshot(this, "Decrease Instances");

    int obj_idx = get_selected_object_idx();

    ModelObject* model_object = m_model.objects[obj_idx];
    if (model_object->instances.size() > num) {
        for (size_t i = 0; i < num; ++ i)
            model_object->delete_last_instance();
        update();
        // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
        sidebar().obj_list()->decrease_object_instances(obj_idx, num);
    }
    else {
        remove(obj_idx);
    }

    if (!model_object->instances.empty())
        get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    selection_changed();
    schedule_background_process();
#endif
}

static long GetNumberFromUser(  const wxString& msg,
                                const wxString& prompt,
                                const wxString& title,
                                long value,
                                long min,
                                long max,
                                wxWindow* parent)
{
#ifdef _WIN32
    wxNumberEntryDialog dialog(parent, msg, prompt, title, value, min, max, wxDefaultPosition);
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() == wxID_OK)
        return dialog.GetValue();

    return -1;
#else
    return wxGetNumberFromUser(msg, prompt, title, value, min, max, parent);
#endif
}


void Plater::fill_bed_with_instances() { arrange_svc().fill_bed_with_instances(); }





void Plater::apply_cut_object_to_model(size_t obj_idx, const ModelObjectPtrs& new_objects) { obj_cmds().apply_cut_object_to_model(obj_idx, new_objects); }

void Plater::export_gcode(bool prefer_removable)
{
    ex().export_gcode(prefer_removable);
}

void Plater::send_to_printer(bool isall)
{
    ex().send_to_printer(isall);   // 阶段 E2：实现迁 app::ExportSendCoordinator
}

//BBS export gcode 3mf to file






// Following lambda generates a combined mesh for export with normals pointing outwards.
TriangleMesh Plater::combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func)
{
    return GUI::app::ExportSendCoordinator::combine_mesh_fff(mo, instance_id, notify_func);
}

// BBS export with/without boolean, however, stil merge mesh
#define EXPORT_WITH_BOOLEAN 0


namespace {
std::string get_file_name(const std::string &file_path)
{
    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    size_t pos_point          = file_path.find_last_of('.');
    size_t offset             = pos_last_delimiter + 1;
    size_t count              = pos_point - pos_last_delimiter - 1;
    return file_path.substr(offset, count);
}
using SvgFile = EmbossShape::SvgFile;
using SvgFiles = std::vector<SvgFile*>;
std::string create_unique_3mf_filepath(const std::string &file, const SvgFiles svgs)
{
    // const std::string MODEL_FOLDER = "3D/"; // copy from file 3mf.cpp
    std::string path_in_3mf = "3D/" + file + ".svg";
    size_t suffix_number = 0;
    bool is_unique = false;
    do{
        is_unique = true;
        path_in_3mf = "3D/" + file + ((suffix_number++)? ("_" + std::to_string(suffix_number)) : "") + ".svg";
        for (SvgFile *svgfile : svgs) {
            if (svgfile->path_in_3mf.empty())
                continue;
            if (svgfile->path_in_3mf.compare(path_in_3mf) == 0) {
                is_unique = false;
                break;
            }
        } 
    } while (!is_unique);
    return path_in_3mf;
}

bool set_by_local_path(SvgFile &svg, const SvgFiles& svgs)
{
    // Try to find already used svg file
    for (SvgFile *svg_ : svgs) {
        if (svg_->path_in_3mf.empty())
            continue;
        if (svg.path.compare(svg_->path) == 0) {
            svg.path_in_3mf = svg_->path_in_3mf;
            return true;
        }
    }
    return false;
}

/// <summary>
/// Function to secure private data before store to 3mf
/// </summary>
/// <param name="model">Data(also private) to clean before publishing</param>
void publish(Model &model, SaveStrategy strategy) {

    // SVG file publishing
    bool exist_new = false;
    SvgFiles svgfiles;
    for (ModelObject *object: model.objects){
        for (ModelVolume *volume : object->volumes) {
            if (!volume->emboss_shape.has_value())
                continue;
            if (volume->text_configuration.has_value())
                continue; // text dosen't have svg path

            assert(volume->emboss_shape->svg_file.has_value());
            if (!volume->emboss_shape->svg_file.has_value())
                continue;

            SvgFile* svg = &(*volume->emboss_shape->svg_file);
            if (svg->path_in_3mf.empty())
                exist_new = true;
            svgfiles.push_back(svg);
        }
    }

    // Orca: don't show this in silence mode
    if (exist_new && !(strategy & SaveStrategy::Silence)) {
        MessageDialog dialog(nullptr,
                             _L("Are you sure you want to store original SVGs with their local paths into the 3MF file?\n"
                                "If you hit 'NO', all SVGs in the project will not be editable any more."),
                             _L("Private protection"), wxYES_NO | wxICON_QUESTION);
        if (dialog.ShowModal() == wxID_NO){
            for (ModelObject *object : model.objects) 
                for (ModelVolume *volume : object->volumes)
                    if (volume->emboss_shape.has_value())
                        volume->emboss_shape.reset();
        }
    }

    for (SvgFile* svgfile : svgfiles){
        if (!svgfile->path_in_3mf.empty())
            continue; // already suggested path (previous save)

        // create unique name for svgs, when local path differ
        std::string filename = "unknown";
        if (!svgfile->path.empty()) {
            if (set_by_local_path(*svgfile, svgfiles))
                continue;
            // check whether original filename is already in:
            filename = get_file_name(svgfile->path);
        }
        svgfile->path_in_3mf = create_unique_3mf_filepath(filename, svgfiles);        
    }
}
}

// BBS: backup
int Plater::export_3mf(const boost::filesystem::path& output_path, SaveStrategy strategy, int export_plate_idx, Export3mfProgressFn proFn)
{
    return ex().export_3mf(output_path, strategy, export_plate_idx, proFn);
}

void Plater::publish_project()
{
    ex().publish_project();
}












//BBS: add multiple plate reslice logic
bool Plater::reslice() { return slicing().reslice(); }

void Plater::record_slice_preset(std::string action)
{
    // record slice preset
    try
    {
        json j;
        auto printer_preset = wxGetApp().preset_bundle->printers.get_edited_preset_with_vendor_profile().preset;
        if (printer_preset.is_system) {
            j["printer_preset_name"] = printer_preset.name;
        }
        else {
            j["printer_preset_name"] = printer_preset.config.opt_string("inherits");
        }
        const t_config_enum_values* keys_map = print_config_def.get("curr_bed_type")->enum_keys_map;
        if (keys_map) {
            for (auto item : *keys_map) {
                if (item.second == wxGetApp().preset_bundle->project_config.opt_enum<BedType>("curr_bed_type")) {
                    j["curr_bed_type"] = item.first;
                    break;
                }
            }
        }
        auto filament_presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < filament_presets.size(); ++i) {
            auto filament_preset = wxGetApp().preset_bundle->filaments.find_preset(filament_presets[i]);
            if (filament_preset->is_system) {
                j["filament_preset_" + std::to_string(i)] = filament_preset->name;
            }
            else {
                j["filament_preset_" + std::to_string(i)] = filament_preset->config.opt_string("inherits");
            }
        }

        Preset& print_preset = wxGetApp().preset_bundle->prints.get_edited_preset();
        if (print_preset.is_system) {
            j["process_preset"] = print_preset.name;
        }
        else {
            j["process_preset"] = print_preset.config.opt_string("inherits");
        }
        j["support_type"] = ConfigOptionEnum<SupportType>::get_enum_names().at(print_preset.config.opt_enum<SupportType>("support_type"));
        j["sparse_infill_pattern"] = ConfigOptionEnum<InfillPattern>::get_enum_names().at(print_preset.config.opt_enum<InfillPattern>("sparse_infill_pattern"));
        j["sparse_infill_density"] = print_preset.config.opt<ConfigOptionPercent>("sparse_infill_density")->value;

        j["brim_type"] = ConfigOptionEnum<BrimType>::get_enum_names().at(print_preset.config.opt_enum<BrimType>("brim_type"));
        j["user_mode"] = wxGetApp().get_mode_str();

        if (slicing().background_process().fff_print()) {
            const DynamicPrintConfig& full_config = slicing().background_process().fff_print()->full_print_config();
            json values = json::array();
            if (full_config.has("different_settings_to_system")) {
                std::vector<std::string> different_values = full_config.option<ConfigOptionStrings>("different_settings_to_system")->values;
                for (auto& item : different_values) {
                    values.push_back(item);
                }
            }
            j["different_settings_to_system"] = values;
        }

        j["record_event"] = action;
        NetworkAgent* agent = wxGetApp().getAgent();
    }
    catch (...)
    {
        return;
    }
}

//BBS: add project slicing related logic
int Plater::start_next_slice() { return slicing().start_next_slice(); }


void Plater::reslice_SLA_supports(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposPad, object, postpone_error_messages);
}

void Plater::reslice_SLA_hollowing(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposDrillHoles, object, postpone_error_messages);
}

void Plater::reslice_SLA_until_step(SLAPrintObjectStep step, const ModelObject &object, bool postpone_error_messages)
{
    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true, postpone_error_messages);
    if (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        layout().view3D->reload_scene(false);

    if (slicing().background_process().empty() || (state & UPDATE_BACKGROUND_PROCESS_INVALID))
        // Nothing to do on empty input or invalid configuration.
        return;

    // Limit calculation to the single object only.
    PrintBase::TaskParams task;
    task.single_model_object = object.id();
    // If the background processing is not enabled, calculate supports just for the single instance.
    // Otherwise calculate everything, but start with the provided object.
    if (!background_processing_enabled()) {
        task.single_model_instance_only = true;
        task.to_object_step = step;
    }
    slicing().background_process().set_task(task);
    // and let the background processing start.
    restart_background_process(state | UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
}

int Plater::send_gcode(int plate_idx, Export3mfProgressFn proFn)
{
    return ex().send_gcode(plate_idx, proFn);
}

int Plater::export_config_3mf(int plate_idx, Export3mfProgressFn proFn)
{
    return ex().export_config_3mf(plate_idx, proFn);
}

//BBS
void Plater::send_calibration_job_finished(wxCommandEvent & evt)
{
    ex().handle_send_calibration_job_finished(evt);
}

void Plater::print_job_finished(wxCommandEvent &evt)
{
    ex().handle_print_job_finished(evt);
}

void Plater::send_job_finished(wxCommandEvent& evt)
{
    ex().handle_send_job_finished(evt);
}

void Plater::publish_job_finished(wxCommandEvent &evt)
{
    ex().handle_publish_job_finished(evt);
}

// Called when the Eject button is pressed.
void Plater::eject_drive()
{
	wxBusyCursor wait;
    wxGetApp().removable_drive_manager()->set_and_verify_last_save_path(ex().get_last_output_dir_path());   // 阶段 E2：状态迁 EX，门面留守
	wxGetApp().removable_drive_manager()->eject_drive();
}

void Plater::take_snapshot(const std::string &snapshot_name) { snapshots().take_snapshot(snapshot_name); }

void Plater::take_snapshot(const std::string &snapshot_name, UndoRedo::SnapshotType snapshot_type) { snapshots().take_snapshot(snapshot_name, snapshot_type); }

void Plater::suppress_snapshots() { snapshots().suppress_snapshots(); }
void Plater::allow_snapshots() { snapshots().allow_snapshots(); }
void Plater::undo() { snapshots().undo(); }
void Plater::redo() { snapshots().redo(); }


bool Plater::undo_redo_string_getter(const bool is_undo, int idx, const char** out_text)
{
    return snapshots().undo_redo_string_getter(is_undo, idx, out_text);
}



void Plater::undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text)
{
    snapshots().undo_redo_topmost_string_getter(is_undo, out_text);
}

bool Plater::search_string_getter(int idx, const char** label, const char** tooltip)
{
    const Search::OptionsSearcher& search_list = layout().sidebar->get_searcher();

    if (0 <= idx && (size_t)idx < search_list.size()) {
        search_list[idx].get_marked_label_and_tooltip(label, tooltip);
        return true;
    }

    return false;
}

void Plater::on_filaments_delete(size_t num_filaments, size_t filament_id, int replace_filament_id, const std::vector<unsigned char>& is_mixed_snapshot)
{
    filament_svc().on_filaments_delete(num_filaments, filament_id, replace_filament_id, is_mixed_snapshot);
}


// BBS.
void Plater::on_filaments_change(size_t num_filaments)
{
    filament_svc().on_filaments_change(num_filaments);
}

void Plater::on_bed_type_change(BedType bed_type)
{
    sidebar().on_bed_type_change(bed_type);
}










PresetBundle* Plater::preset_bundle() { return wxGetApp().preset_bundle; }
AppConfig* Plater::app_config() { return wxGetApp().app_config; }
Tab* Plater::get_tab(int type) { return wxGetApp().get_tab((Preset::Type)type); }
std::string Plater::get_mode_str() { return wxGetApp().get_mode_str(); }
NetworkAgent* Plater::get_agent() { return wxGetApp().getAgent(); }
void Plater::select_tab_3d_editor() { wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor); }
void Plater::params_panel_switch_to_object() { wxGetApp().params_panel()->switch_to_object(); }
DeviceManager* Plater::device_manager() { return wxGetApp().getDeviceManager(); }
void Plater::get_connect_host(std::shared_ptr<PrintHost>& h) { wxGetApp().get_connect_host(h); }
ConfigOptionMode Plater::get_mode() { return wxGetApp().get_mode(); }
bool Plater::is_editor() { return wxGetApp().is_editor(); }
bool Plater::is_gcode_viewer() { return wxGetApp().is_gcode_viewer(); }
ParamsDialog* Plater::params_dialog() { return wxGetApp().params_dialog(); }
boost::asio::ip::port_type Plater::page_http_port() { return wxGetApp().get_page_http_port(); }
wxString Plater::international_url(const wxString& origin_url) { return wxGetApp().get_international_url(origin_url); }
void Plater::start_download(const std::string& url) { wxGetApp().start_download(url); }
PresetUpdater* Plater::preset_updater() { return wxGetApp().get_preset_updater(); }
RemovableDriveManager* Plater::removable_drive_manager() { return wxGetApp().removable_drive_manager(); }
void Plater::keyboard_shortcuts() { wxGetApp().keyboard_shortcuts(); }
void Plater::app_update_ui_from_settings() { wxGetApp().update_ui_from_settings(); }
bool Plater::dark_mode() { return wxGetApp().dark_mode(); }
void Plater::update_frame_dark_ui(wxFrame* frame) { wxGetApp().UpdateFrameDarkUI(frame); }
wxString Plater::inf_dialog_context() { return wxGetApp().get_inf_dialog_contect(); }
bool Plater::is_user_login() { return wxGetApp().is_user_login(); }
void Plater::show_down_net_plugin_dlg() { wxGetApp().ShowDownNetPluginDlg(); }
void Plater::publish_model(Model& model, SaveStrategy strategy) { publish(model, strategy); }
int Plater::get_process_completed_with_error() { return process_completed_with_error; }
void Plater::set_show_warning_dialog(bool v) { show_warning_dialog = v; }
void Plater::run_sla_import_job()
{
    auto &w = *m_worker;
    if (w.is_idle() && m_sla_import_dlg->ShowModal() == wxID_OK) {
        snapshots().take_snapshot(_u8L("Import SLA archive"));
        replace_job(w, std::make_unique<SLAImportJob>(m_sla_import_dlg));
    }
}
int Plater::determine_load_type(std::string filename, std::string override_setting) { return (int)Slic3r::GUI::determine_load_type(filename, override_setting); }
void Plater::handle_newer_3mf_schema(DynamicPrintConfig& config_loaded) { Slic3r::GUI::handle_newer_3mf_schema(this, config_loaded); }
void Plater::read_binary_stl(const std::string& filename, std::string& model_id, std::string& code) { Slic3r::GUI::read_binary_stl(filename, model_id, code); }
int Plater::check_and_keep_current_preset_changes(const wxString& caption, const wxString& header, int buttons, bool* postponed) { return wxGetApp().check_and_keep_current_preset_changes(caption, header, buttons, postponed); }
bool Plater::check_and_save_current_preset_changes(const wxString& caption, const wxString& header) { return wxGetApp().check_and_save_current_preset_changes(caption, header); }
void Plater::load_project_dialog(wxString& input_file) { wxGetApp().load_project(this, input_file); }
void Plater::update_saved_preset_from_current_preset() { wxGetApp().update_saved_preset_from_current_preset(); }
void Plater::import_model_dialog(wxArrayString& files) { wxGetApp().import_model(this, files); }
bool Plater::can_load_project() { return wxGetApp().can_load_project(); }
void Plater::import_zip_dialog(wxString& input_file) { wxGetApp().import_zip(this, input_file); }
void Plater::load_gcode_dialog(wxString& input_file) { wxGetApp().load_gcode(this, input_file); }
wxString Plater::model_file_wildcards() { return file_wildcards(FT_MODEL); }
void Plater::load_current_presets(bool show_sure_modification_dialog, bool show_unsaved_dialog) { wxGetApp().load_current_presets(show_sure_modification_dialog, show_unsaved_dialog); }
void Plater::update_nozzle_settings_after()
{
    wxTheApp->CallAfter([]() {
        if (wxGetApp().plater() != nullptr)
            wxGetApp().plater()->sidebar().update_nozzle_settings(true);
    });
}
void Plater::trigger_restore_project_evt(int skip_confirm)
{
    auto evt = new wxCommandEvent(EVT_RESTORE_PROJECT, GetId());
    evt->SetInt(skip_confirm);
    wxQueueEvent(this, evt);
}
void Plater::request_model_download_evt(const wxString& url)
{
    wxCommandEvent* event = new wxCommandEvent(EVT_IMPORT_MODEL_ID);
    event->SetString(url);
    wxQueueEvent(this, event);
}
void Plater::set_process_completed_with_error(int v) { process_completed_with_error = v; }
bool Plater::get_show_warning_dialog() { return show_warning_dialog; }
bool Plater::get_tracking_popup_menu() { return m_tracking_popup_menu; }
void Plater::set_tracking_popup_menu_error_message(const std::string& msg) { m_tracking_popup_menu_error_message = msg; }
void Plater::set_plater_drop_target() { SetDropTarget(new PlaterDropTarget(*wxGetApp().mainframe, *this)); }
void Plater::notify_object_config_changed() { wxGetApp().params_panel()->notify_object_config_changed(); }
void Plater::load_current_presets() { wxGetApp().load_current_presets(); }
void Plater::save_mode(int mode) { wxGetApp().save_mode(mode); }
bool Plater::single_blocks(bool cond)
{
    return m_single && !m_single->check(cond);
}

bool Plater::has_sliceable_plate_for_slice_all()
{
    return find_next_sliceable_plate_for_slice_all(0) >= 0;
}

bool Plater::is_plate_sliceable(int plate_index)
{
    PartPlate* plate = partplate_list->get_plate(plate_index);
    if (plate == nullptr || !plate->can_slice())
        return false;
    // GUI-layer blockers live here so call sites cannot drift. Add new
    // blockers as additional early-returns in this function; do NOT branch
    // on them at individual GLCanvas3D / MainFrame call sites.
    //
    // Mixing uses the cached lookup (refreshed in sync_filament_temp_mixing_notification,
    // which runs at every state-changing event) because the rendering hot path in
    // GLCanvas3D::_render_imgui_select_plate_toolbar hits this for every plate every
    // frame. cold_plate / flow_ratio_zero have no cached form yet; they recompute on
    // each call.
    if (gate_keeper().is_plate_blocked_by_filament_temp_mixing_cached(plate_index))
        return false;
    if (gate_keeper().is_plate_blocked_by_cold_plate(plate_index))
        return false;
    if (gate_keeper().is_plate_blocked_by_flow_ratio_zero(plate_index))
        return false;
    return true;
}

int Plater::find_next_sliceable_plate_for_slice_all(int start_plate_index)
{
    const int plate_count = partplate_list->get_plate_count();
    if (start_plate_index < 0)
        start_plate_index = 0;

    for (int plate_index = start_plate_index; plate_index < plate_count; ++plate_index)
    {
        if (is_plate_sliceable(plate_index))
            return plate_index;
    }

    return -1;
}

bool Plater::sync_filament_temp_mixing_notification()
{
    return gate_keeper().sync_filament_temp_mixing_notification();
}



bool Plater::sync_flow_ratio_zero_notification()
{
    return gate_keeper().sync_flow_ratio_zero_notification();
}



bool Plater::sync_cold_plate_notification()
{
    return gate_keeper().sync_cold_plate_notification();
}

void Plater::notify_filament_usage_changed()
{
    filament_svc().notify_filament_usage_changed();
}

void Plater::on_config_change(const DynamicPrintConfig &config)
{
    bool update_scheduled = false;
    bool bed_shape_changed = false;
    bool print_sequence_changed = false;
    t_config_option_keys diff_keys = m_config->diff(config);
    for (auto opt_key : diff_keys) {
        if (opt_key == "filament_colour") {
            update_scheduled = true; // update should be scheduled (for update 3DScene) #2738

            if (filament_svc().update_filament_colors_in_full_config()) {
                layout().sidebar->obj_list()->update_filament_colors();
                layout().sidebar->update_dynamic_filament_list();
                continue;
            }
        }
        if (opt_key == "material_colour") {
            update_scheduled = true; // update should be scheduled (for update 3DScene)
        }

        m_config->set_key_value(opt_key, config.option(opt_key)->clone());
        if (opt_key == "printer_technology") {
            this->set_printer_technology(config.opt_enum<PrinterTechnology>(opt_key));
            // print technology is changed, so we should to update a search list
            layout().sidebar->update_searcher();
            refresh().reset_gcode_toolpaths();   // 阶段 G4：原 reset_gcode_toolpaths()（printer_technology 变更路径）
            layout().view3D->get_canvas3d()->reset_sequential_print_clearance();
            //BBS: invalid all the slice results
            partplate_list->invalid_all_slice_result();
        }
        //BBS: add bed_exclude_area
        else if (opt_key == "printable_area" || opt_key == "bed_exclude_area"
            || opt_key == "bed_custom_texture" || opt_key == "bed_custom_model"
            || opt_key == "extruder_clearance_height_to_lid"
            || opt_key == "extruder_clearance_height_to_rod") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (opt_key == "bed_shape" || opt_key == "bed_custom_texture" || opt_key == "bed_custom_model") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (boost::starts_with(opt_key, "enable_prime_tower") ||
            boost::starts_with(opt_key, "prime_tower") ||
            boost::starts_with(opt_key, "wipe_tower") ||
            opt_key == "filament_minimal_purge_on_wipe_tower" ||
            opt_key == "single_extruder_multi_material" ||
            // BBS
            opt_key == "prime_volume") {
            update_scheduled = true;
        }
        else if(opt_key == "extruder_colour") {
            update_scheduled = true;
            //layout().sidebar->obj_list()->update_extruder_colors();
        }
        else if (opt_key == "printable_height") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (opt_key == "print_sequence") {
            update_scheduled = true;
            print_sequence_changed = true;
        }
        else if (opt_key == "printer_model") {
            refresh().reset_gcode_toolpaths();   // 阶段 G4：原 reset_gcode_toolpaths()
            // update to force bed selection(for texturing)
            bed_shape_changed = true;
            update_scheduled = true;
        }
        // Orca: update when *_filament changed
        else if (opt_key == "support_interface_filament" || opt_key == "support_filament" || opt_key == "wall_filament" ||
                 opt_key == "sparse_infill_filament" || opt_key == "solid_infill_filament") {
            update_scheduled = true;
        }
    }

    if (bed_shape_changed)
        set_bed_shape();

    if (update_scheduled)
        update();

    if (main_frame->is_loaded()) {
        slicing().schedule_background_process();
        update_title_dirty_status();
    }

    // Gate on the diff so unrelated option edits never resurrect the yellow
    // by-object warning once the slice guard has retired it.
    if (print_sequence_changed)
        notification_hub().sync_print_seq_warning_notification();

    notify_filament_usage_changed();

    // Any config change resets the user's dismissal of PLA/PETG mix warning
    // so the per-frame detection re-evaluates and re-shows if still applicable.
    get_notification_manager()->reset_pla_petg_mix_warning();
}

void Plater::set_bed_shape() const
{
    std::string texture_filename;
    auto bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const Preset* curr = &bundle->printers.get_selected_preset();
        if (curr->is_system)
            texture_filename = PresetUtils::system_printer_bed_texture(*curr);
        else {
            auto *printer_model = curr->config.opt<ConfigOptionString>("printer_model");
            if (printer_model != nullptr && ! printer_model->value.empty()) {
                texture_filename = bundle->get_texture_for_printer_model(printer_model->value);
            }
        }
    }
    set_bed_shape(m_config->option<ConfigOptionPoints>("printable_area")->values,
        //BBS: add bed exclude areas
        m_config->option<ConfigOptionPoints>("bed_exclude_area")->values,
        m_config->option<ConfigOptionFloat>("printable_height")->value,
        m_config->option<ConfigOptionString>("bed_custom_texture")->value.empty() ? texture_filename : m_config->option<ConfigOptionString>("bed_custom_texture")->value,
        m_config->option<ConfigOptionString>("bed_custom_model")->value);
}

//BBS: add bed exclude area
void Plater::set_bed_shape(const Pointfs& shape, const Pointfs& exclude_area, const double printable_height, const std::string& custom_texture, const std::string& custom_model, bool force_as_custom) const
{
    plates().set_bed_shape(make_counter_clockwise(shape), exclude_area, printable_height, custom_texture, custom_model, force_as_custom);
}




// Get vector of extruder colors considering filament color, if extruder color is undefined.
std::vector<std::string> Plater::get_extruder_colors_from_plater_config(const GCodeProcessorResult* const result, bool include_mixed) const
{
    if (result != nullptr && (wxGetApp().is_gcode_viewer() || session().only_gcode_mode()))
        return result->extruder_colors;
    else {
        if (wxGetApp().preset_bundle == nullptr)
            return {};

        const Slic3r::DynamicPrintConfig* config = &wxGetApp().preset_bundle->project_config;
        std::vector<std::string> filament_colors;
        if (!config->has("filament_colour")) // in case of a SLA print
            return filament_colors;

        filament_colors = (config->option<ConfigOptionStrings>("filament_colour"))->values;
        const size_t num_physical = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
        filament_colors.resize(num_physical, "#26A69A");

        if (include_mixed) {
            // Append display colours for enabled mixed (virtual) filaments.
            const auto &mixed_mgr = wxGetApp().preset_bundle->mixed_filaments;
            for (const auto &dc : mixed_mgr.display_colors())
                filament_colors.push_back(dc);
        }

        return filament_colors;
    }
}

/* Get vector of colors used for rendering of a Preview scene in "Color print" mode
 * It consists of extruder colors and colors, saved in model.custom_gcode_per_print_z
 */
std::vector<std::string> Plater::get_colors_for_color_print(const GCodeProcessorResult* const result) const
{
    std::vector<std::string> colors = get_extruder_colors_from_plater_config(result);

    if (result != nullptr && (wxGetApp().is_gcode_viewer() || session().only_gcode_mode())) {
        for (const CustomGCode::Item& code : result->custom_gcode_per_print_z) {
            if (code.type == CustomGCode::ColorChange)
                colors.emplace_back(code.color);
        }
    }
    else {
        //BBS
        colors.reserve(colors.size() + m_model.get_curr_plate_custom_gcodes().gcodes.size());
        for (const CustomGCode::Item& code : m_model.get_curr_plate_custom_gcodes().gcodes) {
            if (code.type == CustomGCode::ColorChange)
                colors.emplace_back(code.color);
        }
    }

    return colors;
}




void Plater::update_print_error_info(int code, std::string msg, std::string extra)
{
    ex().update_print_error_info(code, msg, extra);
}


wxString Plater::get_export_gcode_filename(const wxString & extension, bool only_filename, bool export_all) const
{
    return ex().get_export_gcode_filename(extension, only_filename, export_all);
}



const Selection &Plater::get_selection() const
{
    return selq().get_selection();
}

int Plater::get_selected_object_idx()
{
    return selq().get_selected_object_idx();
}



GLCanvas3D* Plater::canvas3D()
{
    // BBS modify layout().view3D->get_canvas3d() to current canvas
    return layout().get_current_canvas3D();
}

const GLCanvas3D* Plater::canvas3D() const
{
    // BBS modify layout().view3D->get_canvas3d() to current canvas
    return layout().get_current_canvas3D();
}

GLCanvas3D* Plater::get_view3D_canvas3D()
{
    return layout().view3D->get_canvas3d();
}

GLCanvas3D* Plater::get_preview_canvas3D()
{
    return layout().preview->get_canvas3d();
}

GLCanvas3D* Plater::get_assmeble_canvas3D()
{
    if (layout().assemble_view)
        return layout().assemble_view->get_canvas3d();
    return nullptr;
}

GLCanvas3D* Plater::get_current_canvas3D(bool exclude_preview)
{
    return layout().get_current_canvas3D(exclude_preview);
}

void Plater::arrange(const app::ArrangeRequest& req) { arrange_svc().arrange(req); }   // F3：门面一行转发（原无参版消亡）

void Plater::set_current_canvas_as_dirty() { refresh().set_current_canvas_as_dirty(); }

PrinterTechnology Plater::printer_technology() const
{
    return session().printer_technology;
}

const DynamicPrintConfig * Plater::config() const { return m_config; }

bool Plater::set_printer_technology(PrinterTechnology printer_technology)
{
    session().printer_technology = printer_technology;
    bool ret = slicing().background_process().select_technology(printer_technology);
    if (ret) {
        // Update the active presets.
    }
    //FIXME for SLA synchronize
    //slicing().background_process().apply(Model)!

    if (printer_technology == ptSLA) {   // 形参（原同名形参遮蔽，非成员）
        for (ModelObject* model_object : m_model.objects) {
            model_object->ensure_on_bed();
        }
    }


    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_menubar();

    layout().sidebar->get_searcher().set_printer_technology(printer_technology);

    notification_manager()->set_fff(printer_technology == ptFFF);
    if (ret) {
        // Only hide when the technology actually changed (ret): hiding on same-technology
        // preset reload (device connect) kills the popup of a running slice for good.
        notification_manager()->set_slicing_progress_hidden();
    }

    return ret;
}

void Plater::clear_before_change_mesh(int obj_idx) { obj_cmds().clear_before_change_mesh(obj_idx); }
void Plater::changed_mesh(int obj_idx)             { obj_cmds().changed_mesh(obj_idx); }

void Plater::changed_object(ModelObject &object) { refresh().changed_object(object); }
void Plater::changed_object(int obj_idx)         { refresh().changed_object(obj_idx); }


void Plater::schedule_background_process(bool schedule/* = true*/)
{
    if (schedule)
        slicing().schedule_background_process();

    slicing().set_suppress_update(false);
}

void Plater::reload_gcode_preview()
{
    refresh().reset_gcode_toolpaths();
    layout().preview->reload_print(false, session().only_gcode_mode());
}








void Plater::optimize_rotation() { arrange_svc().optimize_rotation(); }
void Plater::update_menus()         { menus->update(); }
// BBS
//void Plater::show_action_buttons(const bool ready_to_slice) const   { show_action_buttons(ready_to_slice); }

void Plater::fill_color(int extruder_id)
{
    if (obj_cmds().can_fillcolor()) {
        layout().assemble_view->get_canvas3d()->get_selection().fill_color(extruder_id);
    }
}

//BBS
//BBS: add clone
void Plater::clone_selection()             { obj_cmds().clone_selection(); }

std::vector<Vec2f> Plater::get_empty_cells(const Vec2f step)
{
    return app::ArrangementService::get_empty_cells(step, *wxGetApp().plater()->partplate_list);   // 静态门面：经 plater 实例取私有成员
}

void Plater::search(bool plater_is_active, Preset::Type type, wxWindow *tag, TextInput *etag, wxWindow *stag)
{
    if (plater_is_active) {
        if (is_preview_shown())
            return;
        // plater should be focused for correct navigation inside search window
        this->SetFocus();

        wxKeyEvent evt;
#ifdef __APPLE__
        evt.m_keyCode = 'f';
#else /* __APPLE__ */
        evt.m_keyCode = WXK_CONTROL_F;
#endif /* __APPLE__ */
        evt.SetControlDown(true);
        canvas3D()->on_char(evt);
    }
    else
        layout().sidebar->get_searcher().show_dialog(type, tag, etag, stag);
}

void Plater::msw_rescale()
{
    layout().msw_rescale();   // 阶段 G3：布局前缀（preview→view3D→sidebar）；menus/Layout 原序留守（D4）

    menus->msw_rescale();

    Layout();
    GetParent()->Layout();
}

void Plater::sys_color_changed()
{
    layout().sys_color_changed();   // 阶段 G3：布局前缀（preview→sidebar）；menus/ex/Layout 原序留守（D4）
    menus->sys_color_changed();
    ex().notify_sys_color_changed();   // 阶段 E2：对话框状态迁 EX（执行期补充触点）

    Layout();
    GetParent()->Layout();
}

// BBS
#if 0
bool Plater::init_view_toolbar()
{
    return init_view_toolbar();
}

void Plater::enable_view_toolbar(bool enable)
{
    view_toolbar.set_enabled(enable);
}
#endif


const Camera& Plater::get_camera() const
{
    return layout().camera;
}

Camera& Plater::get_camera()
{
    return layout().camera;
}

//BBS: partplate list related functions
PartPlateList& Plater::get_partplate_list()
{
    return *partplate_list;
}


//BBS: select Plate
int Plater::select_plate(int plate_index, bool need_slice) { return slicing().select_plate(plate_index, need_slice); }

int Plater::select_sliced_plate(int plate_index) { return slicing().select_sliced_plate(plate_index); }


void Plater::open_platesettings_dialog(wxCommandEvent& evt) {
    int plate_index = evt.GetInt();
    PlateSettingsDialog dlg(this, _L("Plate Settings"), evt.GetString() == "only_layer_sequence");
    PartPlate* curr_plate = partplate_list->get_curr_plate();
    dlg.sync_bed_type(curr_plate->get_bed_type(true));

    auto curr_print_seq = curr_plate->get_print_seq();
    if (curr_print_seq != PrintSequence::ByDefault) {
        dlg.sync_print_seq(int(curr_print_seq) + 1);
    }
    else
        dlg.sync_print_seq(0);

    auto first_layer_print_seq = curr_plate->get_first_layer_print_sequence();
    if (first_layer_print_seq.empty())
        dlg.sync_first_layer_print_seq(0);
    else
        dlg.sync_first_layer_print_seq(1, curr_plate->get_first_layer_print_sequence());

    auto other_layers_print_seq = curr_plate->get_other_layers_print_sequence();
    if (other_layers_print_seq.empty())
        dlg.sync_other_layers_print_seq(0, {});
    else {
        dlg.sync_other_layers_print_seq(1, curr_plate->get_other_layers_print_sequence());
    }

    dlg.sync_spiral_mode(curr_plate->get_spiral_vase_mode(), !curr_plate->has_spiral_mode_config());

    dlg.Bind(EVT_SET_BED_TYPE_CONFIRM, [this, plate_index, &dlg](wxCommandEvent& e) {
        PartPlate* curr_plate = partplate_list->get_curr_plate();
        BedType old_bed_type = curr_plate->get_bed_type(true);
        auto bt_sel = BedType(dlg.get_bed_type_choice());
        if (old_bed_type != bt_sel) {
            curr_plate->set_bed_type(bt_sel);
            update_project_dirty_from_presets();
            set_plater_dirty(true);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("select bed type %1% for plate %2% at plate side") % bt_sel % plate_index;

        if (dlg.get_first_layer_print_seq_choice() != 0)
            curr_plate->set_first_layer_print_sequence(dlg.get_first_layer_print_seq());
        else
            curr_plate->set_first_layer_print_sequence({});

        if (dlg.get_other_layers_print_seq_choice() != 0)
            curr_plate->set_other_layers_print_sequence(dlg.get_other_layers_print_seq_infos());
        else
            curr_plate->set_other_layers_print_sequence({});

        int ps_sel = dlg.get_print_seq_choice();
        if (ps_sel != 0)
            curr_plate->set_print_seq(PrintSequence(ps_sel - 1));
        else
            curr_plate->set_print_seq(PrintSequence::ByDefault);

        notification_hub().sync_print_seq_warning_notification();

        int spiral_sel = dlg.get_spiral_mode_choice();
        if (spiral_sel == 1) {
            curr_plate->set_spiral_vase_mode(true, false);
        }
        else if (spiral_sel == 2) {
            curr_plate->set_spiral_vase_mode(false, false);
        }
        else {
            curr_plate->set_spiral_vase_mode(false, true);
        }

        update_project_dirty_from_presets();
        set_plater_dirty(true);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("select print sequence %1% for plate %2% at plate side") % ps_sel % plate_index;
        update();
        wxGetApp().obj_list()->update_selections();
        });
    dlg.set_plate_name(from_u8(curr_plate->get_plate_name()));

    dlg.ShowModal();
    curr_plate->set_plate_name(dlg.get_plate_name().ToUTF8().data());
}

int Plater::duplicate_plate(int plate_index) { return plates().duplicate_plate(plate_index); }

//BBS: delete the plate, index= -1 means the current plate
int Plater::delete_plate(int plate_index) { return plates().delete_plate(plate_index); }

//BBS: set bed positions
void Plater::set_bed_position(Vec2d& pos)
{
    layout().bed.set_position(pos);
}

//BBS: is the background process slicing currently

//BBS: update slicing context
void Plater::update_slicing_context_to_current_partplate() { slicing().update_slicing_context_to_current_partplate(); }

//BBS: show object info
void Plater::show_object_info()
{
    NotificationManager *notify_manager = get_notification_manager();
    const Selection& selection = get_selection();
    int selCount = selection.get_volume_idxs().size();
    ModelObjectPtrs objects = m_model.objects;
    int obj_idx = selection.get_object_idx();
    std::string info_text;

    if (selCount > 1 && !selection.is_single_full_object()) {
        notify_manager->bbl_close_objectsinfo_notification();
        if (selection.get_mode() == Selection::EMode::Volume) {
            info_text += (boost::format(_utf8(L("Number of currently selected parts: %1%\n"))) % selCount).str();
        } else if (selection.get_mode() == Selection::EMode::Instance) {
            int content_count = selection.get_content().size();
            info_text += (boost::format(_utf8(L("Number of currently selected objects: %1%\n"))) % content_count).str();
        }
        notify_manager->bbl_show_objectsinfo_notification(info_text, false, !(layout().current_panel == layout().view3D));
        return;
    }
    else if (objects.empty() || (obj_idx < 0) || (obj_idx >= objects.size()) ||
        objects[obj_idx]->volumes.empty() ||// hack to avoid crash when deleting the last object on the bed
        (selection.is_single_full_object() && objects[obj_idx]->instances.size()> 1) ||
        !(selection.is_single_full_instance() || selection.is_single_volume()))
    {
        notify_manager->bbl_close_objectsinfo_notification();
        return;
    }

    const ModelObject* model_object = objects[obj_idx];
    int inst_idx = selection.get_instance_idx();
    if ((inst_idx < 0) || (inst_idx >= model_object->instances.size()))
    {
        notify_manager->bbl_close_objectsinfo_notification();
        return;
    }
    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    double koef = imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0f;

    ModelVolume* vol = nullptr;
    Transform3d t;
    int face_count;
    Vec3d size;
    if (selection.is_single_volume()) {
        std::vector<int> obj_idxs, vol_idxs;
        wxGetApp().obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
        if (vol_idxs.size() != 1)
        {
            //corner case when merge/split/remove
            return;
        }
        vol = model_object->volumes[vol_idxs[0]];
        t = model_object->instances[inst_idx]->get_matrix() * vol->get_matrix();
        info_text += (boost::format(_utf8(L("Part name: %1%\n"))) % vol->name).str();
        face_count = static_cast<int>(vol->mesh().facets_count());
        size = vol->get_convex_hull().transformed_bounding_box(t).size();
    }
    else {
        //int obj_idx, vol_idx;
        //wxGetApp().obj_list()->get_selected_item_indexes(obj_idx, vol_idx);
        //if (obj_idx < 0) {
        //    //corner case when merge/split/remove
        //    return;
        //}
        info_text += (boost::format(_utf8(L("Object name: %1%\n"))) % model_object->name).str();
        face_count = static_cast<int>(model_object->facets_count());
        size = model_object->instance_convex_hull_bounding_box(inst_idx).size();
    }

    //Vec3d size = vol ? vol->mesh().transformed_bounding_box(t).size() : model_object->instance_bounding_box(inst_idx).size();
    if (imperial_units)
        info_text += (boost::format(_utf8(L("Size: %1% x %2% x %3% in\n"))) %(size(0)*koef) %(size(1)*koef) %(size(2)*koef)).str();
    else
        info_text += (boost::format(_utf8(L("Size: %1% x %2% x %3% mm\n"))) %size(0) %size(1) %size(2)).str();

    const TriangleMeshStats& stats = vol ? vol->mesh().stats() : model_object->get_object_stl_stats();
    double volume_val = stats.volume;
    if (vol)
        volume_val *= std::fabs(t.matrix().block(0, 0, 3, 3).determinant());
    volume_val = volume_val * pow(koef,3);
    if (imperial_units)
        info_text += (boost::format(_utf8(L("Volume: %1% in³\n"))) %volume_val).str();
    else
        info_text += (boost::format(_utf8(L("Volume: %1% mm³\n"))) %volume_val).str();
    info_text += (boost::format(_utf8(L("Triangles: %1%\n"))) %face_count).str();

    wxString info_manifold;
    int non_manifold_edges = 0;
    auto mesh_errors = layout().sidebar->obj_list()->get_mesh_errors_info(&info_manifold, &non_manifold_edges);

    #ifndef __WINDOWS__
    if (non_manifold_edges > 0) {
        info_manifold += into_u8("\n" + _L("Tips:") + "\n" +_L("\"Fix Model\" feature is currently only on Windows. Please repair the model on Snapmaker Orca(windows) or CAD softwares."));
    }
    #endif //APPLE & LINUX

    info_manifold = "<Error>" + info_manifold + "</Error>";
    info_text += into_u8(info_manifold);
    notify_manager->bbl_show_objectsinfo_notification(info_text, is_windows10()&&(non_manifold_edges > 0), !(layout().current_panel == layout().view3D));
}



void Plater::post_process_string_object_exception(StringObjectException &err)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (err.type == StringExceptionType::STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE) {
        try {
            int extruder_id = atoi(err.params[2].c_str()) - 1;
            if (extruder_id < preset_bundle->filament_presets.size()) {
                std::string filament_name = preset_bundle->filament_presets[extruder_id];
                for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
                    if (filament_it->name == filament_name) {
                        if (filament_it->is_system) {
                            filament_name = filament_it->alias;
                        } else {
                            auto preset = preset_bundle->filaments.get_preset_base(*filament_it);
                            if (preset && !preset->alias.empty()) {
                                filament_name = preset->alias;
                            } else {
                                char target = '@';
                                size_t pos    = filament_name.find(target);
                                if (pos != std::string::npos) {
                                    filament_name = filament_name.substr(0, pos - 1);
                                }
                            }
                        }
                        break;
                    }
                }
                err.string = format(_L("Plate %d: %s is not suggested to be used to print filament %s (%s). "
                                       "If you still want to do this print job, please set this filament's bed temperature to non-zero."),
                             err.params[0], err.params[1], err.params[2], filament_name);
                err.string += "\n";
            }
        } catch (...) {
            ;
        }
    }

    return;
}

#if ENABLE_ENVIRONMENT_MAP
void Plater::init_environment_texture()
{
    if (environment_texture.get_id() == 0)
        environment_texture.load_from_file(resources_dir() + "/images/Pmetal_001.png", false, GLTexture::SingleThreaded, false);
}

unsigned int Plater::get_environment_texture_id() const
{
    return environment_texture.get_id();
}
#endif // ENABLE_ENVIRONMENT_MAP

const BuildVolume& Plater::build_volume() const
{
    return layout().bed.build_volume();
}

// BBS
#if 0
const GLToolbar& Plater::get_view_toolbar() const
{
    return view_toolbar;
}

GLToolbar& Plater::get_view_toolbar()
{
    return view_toolbar;
}
#endif

const GLToolbar& Plater::get_collapse_toolbar() const
{
    return layout().collapse_toolbar;
}

GLToolbar& Plater::get_collapse_toolbar()
{
    return layout().collapse_toolbar;
}



void Plater::post_slice_state_change_update() { slicing().post_slice_state_change_update(); }

const Mouse3DController& Plater::get_mouse3d_controller() const
{
    return layout().mouse3d_controller;
}

Mouse3DController& Plater::get_mouse3d_controller()
{
    return layout().mouse3d_controller;
}

NotificationManager * Plater::get_notification_manager()
{
    return notification_manager();
}

DailyTipsWindow* Plater::get_dailytips() const
{
    return notification_hub().get_dailytips();
}

const NotificationManager * Plater::get_notification_manager() const
{
    return notification_manager();
}

void Plater::init_notification_manager()
{
    notification_hub().init();
}

void Plater::show_status_message(std::string s)
{
    notification_hub().show_status_message(s);
}



bool Plater::can_add_model() const { return !slicing().is_background_process_slicing(); }
bool Plater::can_add_plate() const { return !slicing().is_background_process_slicing() && plates().can_add_plate(); }


void Plater::release_arrange_running() { arrange_svc().release_arrange_running(); }


//BBS support cut


bool Plater::can_undo() const { return IsShown() && layout().is_view3D_shown() && snapshots().undo_redo_stack().has_undo_snapshot(); }
bool Plater::can_redo() const { return IsShown() && layout().is_view3D_shown() && snapshots().undo_redo_stack().has_redo_snapshot(); }

//BBS

bool Plater::has_assmeble_view() const { return layout().has_assemble_view(); }



#if ENABLE_ENHANCED_PRINT_VOLUME_FIT

#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT


void Plater::enter_gizmos_stack() { snapshots().enter_gizmos_stack(); }
bool Plater::leave_gizmos_stack() { return snapshots().leave_gizmos_stack(); } // BBS: return false if not changed




void Plater::toggle_show_wireframe()
{
    layout().show_wireframe = !layout().show_wireframe;
}

bool Plater::is_show_wireframe() const
{
    return layout().show_wireframe;
}

void Plater::enable_wireframe(bool status)
{
    layout().wireframe_enabled = status;
}

bool Plater::is_wireframe_enabled() const
{
    return layout().wireframe_enabled;
}




// Wrapper around wxWindow::PopupMenu to suppress error messages popping out while tracking the popup menu.
bool Plater::PopupMenu(wxMenu *menu, const wxPoint& pos)
{
    // Don't want to wake up and trigger reslicing while tracking the pop-up menu.
    SuppressBackgroundProcessingUpdate sbpu;
    // When tracking a pop-up menu, postpone error messages from the slicing result.
    m_tracking_popup_menu = true;
    bool out = wxGetApp().mainframe->PopupMenu(menu, pos);
    m_tracking_popup_menu = false;
    if (! m_tracking_popup_menu_error_message.empty()) {
        // Don't know whether the CallAfter is necessary, but it should not hurt.
        // The menus likely sends out some commands, so we may be safer if the dialog is shown after the menu command is processed.
        wxString message = std::move(m_tracking_popup_menu_error_message);
        wxTheApp->CallAfter([message, this]() { show_error(this, message); });
        m_tracking_popup_menu_error_message.clear();
    }
    return out;
}








app::FilamentService& Plater::filament_service()
{
    return filament_svc();
}

// BBS
//BBS: add popup logic for table object


bool Plater::PopupObjectTableBySelection()
{
    wxDataViewItem item;
    int obj_idx, vol_idx;
    const wxPoint pos = wxPoint(0, 0);  //Fake position
    wxGetApp().obj_list()->get_selected_item_indexes(obj_idx, vol_idx, item);
    return PopupObjectTable(obj_idx, vol_idx, pos);
}

void Plater::update_title_dirty_status()
{
    session().update_title_dirty_status();
}


wxMenu* Plater::plate_menu()            { return menus->plate_menu();             }
wxMenu* Plater::object_menu()           { return menus->object_menu();            }
wxMenu* Plater::part_menu()             { return menus->part_menu();              }
wxMenu* Plater::text_part_menu()        { return menus->text_part_menu();         }
wxMenu* Plater::svg_part_menu()         { return menus->svg_part_menu();          }
wxMenu* Plater::sla_object_menu()       { return menus->sla_object_menu();        }
wxMenu* Plater::default_menu()          { return menus->default_menu();           }
wxMenu* Plater::instance_menu()         { return menus->instance_menu();          }
wxMenu* Plater::layer_menu()            { return menus->layer_menu();             }
wxMenu* Plater::multi_selection_menu()  { return menus->multi_selection_menu();   }
wxMenu* Plater::filament_action_menu(int active_filament_menu_id) { return menus->filament_action_menu(active_filament_menu_id); }
int     Plater::GetPlateIndexByRightMenuInLeftUI() { return plates().right_click_plate_index(); }
void    Plater::SetPlateIndexByRightMenuInLeftUI(int index) { plates().set_right_click_plate_index(index); }
SuppressBackgroundProcessingUpdate::SuppressBackgroundProcessingUpdate() :
    m_was_scheduled(wxGetApp().slicing().update_timer_running())
{
    wxGetApp().slicing().suppress_background_process(m_was_scheduled);
}

SuppressBackgroundProcessingUpdate::~SuppressBackgroundProcessingUpdate()
{
    wxGetApp().plater()->schedule_background_process(m_was_scheduled);
}

}}    // namespace Slic3r::GUI


