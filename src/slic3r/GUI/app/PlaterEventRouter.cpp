// PlaterEventRouter — 事件路由服务实现（阶段 G5）。
// 迁移源：Plater.cpp priv ctor Bind 区 138 处（手册 §2.3）+ 50 个 on_* 处理器 + 3 个 EVT_ 命名
// 处理器（install_network_plugin/update_plugin_when_launch/show_install_plugin_hint——有 Bind，
// 壳消亡口径不适用，勘误登记）+ show_right_click_menu helper（§2.2-3 随处理器迁）。
// 等价改写豁免口径（逐字审计）：①priv accessor 调用（layout()/slicing()/session()/ex()/plates()/
// obj_cmds()/refresh()）→ typed 引用成员 ②q-> 公有门面保持 ③wxGetApp/main_frame 触点→host 槽
// （陷阱 8：本 cpp 零 wxGetApp）④priv helper（reset_all_gizmos/filament_usage_sync_pending/menus/
// partplate_list/model）→host 槽 ⑤&priv::on_x→&Impl::on_x。
// 边界：wxEVT_TIMER bind（SlicingController 定时器域）与视图构造/AUI/相机/快照初始化**留守 ctor**
// （手册 §2.3/陷阱——本服务零触碰）。
// Windows/asio 前置块必须置于一切 include 之前（G2 勘误 10）。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>              // CP_ACP
#endif

#include "PlaterEventRouter.hpp"
#include "../Plater.hpp"
#include "../Search.hpp"   // H2 3.1 顶部瘦身：wxCUSTOMEVT_JUMP_TO_* 声明（原经 Plater.hpp 传递）        // H1 2.2: F3 q bridges + reset_all_gizmos public face\n
#include "../MainFrame.hpp"      // H1 2.2: typed inject MainFrame*\n
#include "../GUI_Factories.hpp"  // H1 2.2: MenuFactory complete type (typed inject menus)\n
#include "../PartPlate.hpp"      // H1 2.2: typed inject PartPlateList& (complete type)\n

#include <utility>

#include <wx/bookctrl.h>         // wxBookCtrlEvent
#include <wx/settings.h>         // wxSystemSettings（on_apple_change_color_mode）
#include <wx/combobox.h>         // ComboBox（on_select_bed_type）
#include <wx/glcanvas.h>         // wxGLCanvas（install 画布指针）
#include <wx/aui/aui.h>          // wxAuiManager（sidebar pane IDLE bind）

#include "../Event.hpp"             // SimpleEvent/IntEvent/ColorEvent
#include "../GUI_Utils.hpp"         // HIDDeviceAttachedEvent/VolumeAttachedEvent
#include "../GLCanvas3D.hpp"        // RBtnEvent/RBtnPlateEvent/HeightProfileSmoothEvent/get_model_volume
#include "../GUI_Preview.hpp"       // View3D/Preview/AssembleView/GLCanvas3D 完整类型
#include "../MainFrame.hpp"         // m_tabpanel/m_printer_view/load_printer_url/update_slice_print_status
#include "../NotificationManager.hpp"  // 通知事件类型 + nm() 解引用
#include "../RemovableDriveManager.hpp"// RemovableDriveEjectEvent 族
#include "../InstanceCheck.hpp"     // LoadFromOtherInstanceEvent 族
#include "../BackgroundSlicingProcess.hpp"  // SlicingStatusEvent/SlicingProcessCompletedEvent
#include "../PartPlate.hpp"         // PartPlate：get_bed_type/update_slice_result_valid_state（PartPlateList 同头）
#include "../Plater.hpp"            // 18 个 EVT_* 常量 wxDECLARE_EVENT（cpp 级宿主头——勘误登记）
#include "../GUI_App.hpp"           // is_support_filament 自由函数（非 wxGetApp 调用）
#include "../GUI_Factories.hpp"     // MenuFactory（menus 槽解引用）
#include "../DeviceManager.hpp"     // device_manager 槽解引用：reload_printer_settings
#include "../../Utils/PresetUpdater.hpp"  // preset_updater 槽解引用（Slic3r::PresetUpdater）
#include "../FlowTypeHelper.hpp"    // GUI::FlowType（restore_project lambda）
#include "../Sidebar.hpp"           // layout.sidebar 解引用：update_printer_thumbnail/auto_calc_flushing_volumes 等
#include "../PresetComboBoxes.hpp"  // PlaterPresetComboBox/PresetComboBox::LabelItemType
#include "../GUI_ObjectList.hpp"    // ObjectDataViewModelNode + obj_list 槽解引用
#include "../MsgDialog.hpp"         // MessageDialog
#include "../CreatePresetsDialog.hpp"  // CreateFilamentPresetDialog/EditFilamentPresetDialog/CreatePresetSuccessfulDialog
#include "../HttpServer.hpp"        // LOCALHOST_URL
#include "../format.hpp"            // GUI::format（通知文案）
#include "../Tab.hpp"               // get_tab 槽返回值
#include "../ParamsDialog.hpp"      // params_dialog 槽返回值
#include "../I18N.hpp"              // _L
#include "libslic3r/PresetBundle.hpp"  // PresetBundle/PhysicalPrinterCollection/put_other_changes
#include "libslic3r/Preset.hpp"     // Preset::Type
#include "libslic3r/AppConfig.hpp"  // AppConfig
#include "libslic3r/Format/bbs_3mf.hpp"  // Slic3r::has_restore_data/backup_soon
#include "NotificationHub.hpp"      // m_hub：nm()/set_is_dark/is_dark
#include "SlicingController.hpp"
#include "ProjectSession.hpp"
#include "ExportSendCoordinator.hpp"
#include "PlateController.hpp"
#include "ObjectEditCommands.hpp"
#include "LayoutManager.hpp"
#include "SceneRefreshCoordinator.hpp"
#include "SnapshotService.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

namespace Slic3r {
namespace GUI {

namespace app {

// —— Impl：事件签名类型（SlicingStatusEvent 困 BackgroundSlicingProcess.hpp、EVT_* 困 Plater.hpp）
//    全部承载于此——hpp 保持零宿主头（勘误登记：手册 §3 草案处理器入类 → Impl 承载，bodies 逐字不变）——
struct PlaterEventRouter::Impl {
    Plater* q;
    NotificationHub& hub;
    SlicingController& slicing;
    ProjectSession& session;
    ExportSendCoordinator& ex;
    PlateController& plates;
    ObjectEditCommands& obj_cmds;
    LayoutManager& layout;
    SceneRefreshCoordinator& refresh;
    SnapshotService& snapshots;
    MainFrame* main_frame{nullptr};
    MenuFactory& menus;
    PartPlateList& partplate_list;
    Model& model;
    std::atomic<bool>& filament_usage_sync_pending;

    // —— on_* 事件处理器（50 + EVT_ 命名 3 + helper 1，逐字迁入；accessor 调用→typed 成员）——

    // 原 priv::on_slicing_update（:2617 一行壳）
    void on_slicing_update(SlicingStatusEvent& evt) { slicing.handle_slicing_update(evt); }
    void on_slicing_completed(wxCommandEvent& evt) { slicing.handle_slicing_completed(evt); }
    void on_export_began(wxCommandEvent& evt) { slicing.handle_export_began(); }
    void on_export_finished(wxCommandEvent& evt) { slicing.handle_export_finished(); }
    void on_slicing_began() { slicing.handle_slicing_began(); }
    void on_process_completed(SlicingProcessCompletedEvent& evt) { slicing.handle_process_completed(evt); }
    void on_slice_button_status(bool enable) { slicing.handle_slice_button_status(enable); }
    void on_action_slice_plate(SimpleEvent&) { slicing.on_action_slice_plate(); }
    void on_action_slice_all(SimpleEvent&) { slicing.on_action_slice_all(); }

    // 原 priv::on_action_*（E2 导出/发送族，ex() 壳逐字）
    void on_action_publish(wxCommandEvent& event) { ex.handle_publish(event.GetInt() == EVT_PUBLISHING_START); }
    void on_action_print_plate(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print plate event\n" ;
        }
        ex.handle_print_plate();
    }
    void on_action_send_to_multi_machine(SimpleEvent&) { ex.handle_send_to_multi_machine(); }
    void on_action_print_plate_from_sdcard(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print plate event\n";
        }
        ex.handle_print_plate_from_sdcard();
    }
    void on_action_print_all(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print all event\n" ;
        }
        ex.handle_print_all();
    }
    void on_action_export_gcode(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export gcode event\n" ;
        }
        ex.export_gcode(false);
    }
    void on_action_send_gcode(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export gcode event\n" ;
        }
        ex.send_gcode_legacy();
    }
    void on_action_export_sliced_file(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n" ;
        }
        ex.export_gcode_3mf();
    }
    void on_action_export_all_sliced_file(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export all sliced file event\n";
        }
        ex.export_gcode_3mf(true);
    }
    void on_action_export_to_sdcard(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n";
        }
        ex.send_to_printer();
    }
    void on_action_export_to_sdcard_all(SimpleEvent&)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n";
        }
        ex.send_to_printer(true);
    }
    void install_network_plugin(wxCommandEvent&) { ex.install_network_plugin(); }
    void update_plugin_when_launch(wxCommandEvent&) { ex.update_plugin_when_launch(); }
    void show_install_plugin_hint(wxCommandEvent&) { ex.show_install_plugin_hint(); }

    // 原 priv::on_action_*（E1 项目族 / D1 多盘族，session()/plates() 壳逐字）
    void on_action_open_project(SimpleEvent&) { if (q != nullptr) session.on_action_open_project(); }
    void on_action_request_model_id(wxCommandEvent& evt) { if (q != nullptr) session.on_action_request_model_id(evt.GetString()); }
    void on_action_download_project(wxCommandEvent& evt) { if (q != nullptr) session.on_action_download_project(evt.GetString()); }
    void show_preview_only_hint(wxCommandEvent&) { session.show_preview_only_hint(); }
    void on_action_add_plate(SimpleEvent&) { plates.add_plate(); }
    void on_plate_selected(SimpleEvent&) { plates.on_plate_selected(); }

    // 原 priv::on_action_*（F1 对象编辑族，obj_cmds() 壳逐字）
    void on_action_split_objects(SimpleEvent&) { obj_cmds.on_action_split_objects(); }
    void on_action_split_volumes(SimpleEvent&) { obj_cmds.on_action_split_volumes(); }
    void on_object_select(SimpleEvent&) { obj_cmds.on_object_select(); }
    void on_repair_model(wxCommandEvent&) { obj_cmds.on_repair_model(); }
    void on_action_layersediting(SimpleEvent&) { obj_cmds.on_action_layersediting(); }

    // 原 priv::on_action_add/_del_plate/_select_sliced_plate（q 门面逐字）
    void on_action_add(SimpleEvent&)
    {
        if (q != nullptr) {
            session.add_model(false, "");
        }
    }
    void on_action_del_plate(SimpleEvent&)
    {
        if (q != nullptr) {
            q->delete_plate();
        }
    }
    void on_action_select_sliced_plate(wxCommandEvent& evt)
    {
        if (q != nullptr) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received select sliced plate event\n" ;
        }
        q->select_sliced_plate(evt.GetInt());
    }

    // 原 priv::on_combobox_select（:2389-2412 逐字；layout()/互调经 Impl）
    void on_combobox_select(wxCommandEvent& evt)
    {
        PlaterPresetComboBox* preset_combo_box = dynamic_cast<PlaterPresetComboBox*>(evt.GetEventObject());
        if (preset_combo_box) {
            this->on_select_preset(evt);
            layout.sidebar->update_printer_thumbnail();

            Preset::Type preset_type = preset_combo_box->get_type();
            if (preset_type == Preset::TYPE_PRINTER) {
                layout.sidebar->update_nozzle_settings(true);
            }
        }
        else {
            this->on_select_bed_type(evt);
        }
    }

    // 原 priv::on_select_bed_type（:2414-2475 逐字；wxGetApp→host 槽）
    void on_select_bed_type(wxCommandEvent& evt)
    {
        ComboBox* combo = static_cast<ComboBox*>(evt.GetEventObject());
        int selection = combo->GetSelection();
        const std::vector<std::string>& combo_values = layout.sidebar->get_bed_type_combo_enum_values();
        std::string bed_type_name = (combo_values.size() > (size_t)selection)
            ? combo_values[selection]
            : print_config_def.get("curr_bed_type")->enum_values[selection];

        Slic3r::PresetBundle& preset_bundle = *q->preset_bundle();
        DynamicPrintConfig& proj_config = q->preset_bundle()->project_config;
        const t_config_enum_values* keys_map = print_config_def.get("curr_bed_type")->enum_keys_map;

        if (keys_map) {
            BedType new_bed_type = btCount;
            for (auto item : *keys_map) {
                if (item.first == bed_type_name) {
                    new_bed_type = (BedType)item.second;
                    break;
                }
            }

            if (new_bed_type != btCount) {
                BedType old_bed_type = proj_config.opt_enum<BedType>("curr_bed_type");

                std::string printer_name = q->preset_bundle()->printers.get_selected_preset_name();
                auto saved_bed_type_str = q->app_config()->get_printer_setting(printer_name, "curr_bed_type");

                if (old_bed_type != new_bed_type || saved_bed_type_str.empty()) {
                    proj_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(new_bed_type));

                    q->update_project_dirty_from_presets();

                    q->on_config_change(q->preset_bundle()->full_config());

                    AppConfig* app_config = q->app_config();
                    app_config->set("curr_bed_type", std::to_string(int(new_bed_type)));
                    app_config->set_printer_setting(q->preset_bundle()->printers.get_selected_preset_name(),
                                                    "curr_bed_type", std::to_string(int(new_bed_type)));

                    auto plate_list = partplate_list.get_plate_list();
                    for (auto plate : plate_list) {
                        if (plate->get_bed_type() == btDefault) {
                            plate->update_slice_result_valid_state(false);
                        }
                    }

                    layout.view3D->get_canvas3d()->render();
                    layout.preview->msw_rescale();
                }
            }
        }
    }

    // 原 priv::on_select_preset（:2477-2615 逐字；wxGetApp→host 槽、partplate_list/model→host）
    void on_select_preset(wxCommandEvent& evt)
    {
        PlaterPresetComboBox* combo = static_cast<PlaterPresetComboBox*>(evt.GetEventObject());
        Preset::Type preset_type    = combo->get_type();

        int selection = evt.GetSelection();

        auto marker = reinterpret_cast<size_t>(combo->GetClientData(selection));
        if (PresetComboBox::LabelItemType::LABEL_ITEM_WIZARD_ADD_PRINTERS == marker) {
            layout.sidebar->create_printer_preset();
            return;
        }

        auto idx = combo->get_filament_idx();

        // BBS:Save the plate parameters before switching
        PartPlateList& old_plate_list = partplate_list;
        PartPlate* old_plate = old_plate_list.get_selected_plate();
        Vec3d old_plate_pos = old_plate->get_center_origin();

        // BBS: Save the model in the current platelist
        std::vector<std::vector<int>> plate_object;
        for (size_t i = 0; i < old_plate_list.get_plate_count(); ++i) {
            PartPlate* plate = old_plate_list.get_plate(i);
            std::vector<int> obj_idxs;
            for (int obj_idx = 0; obj_idx < model.objects.size(); obj_idx++) {
                if (plate && plate->contain_instance(obj_idx, 0)) {
                    obj_idxs.emplace_back(obj_idx);
                }
            }
            plate_object.emplace_back(obj_idxs);
        }

        bool flag = is_support_filament(idx);
        std::string preset_name = q->preset_bundle()->get_preset_name_by_alias(preset_type,
            Preset::remove_suffix_modified(combo->GetString(selection).ToUTF8().data()));

        if (preset_type == Preset::TYPE_FILAMENT) {
            q->preset_bundle()->set_filament_preset(idx, preset_name);
            q->update_project_dirty_from_presets();
            q->preset_bundle()->export_selections(*q->app_config());
            layout.sidebar->update_dynamic_filament_list();
            layout.sidebar->update_color_mix_panel();
            bool flag_is_change = is_support_filament(idx);
            if (flag != flag_is_change) {
                layout.sidebar->auto_calc_flushing_volumes(idx);
            }
        }
        bool select_preset = !combo->selection_is_changed_according_to_physical_printers();
        if (preset_type == Preset::TYPE_FILAMENT && layout.sidebar->is_multifilament()) {
            combo->update();
        }
        else if (select_preset) {
            if (preset_type == Preset::TYPE_PRINTER) {
                Slic3r::PhysicalPrinterCollection& physical_printers = q->preset_bundle()->physical_printers;
                if (combo->is_selected_physical_printer())
                    preset_name = physical_printers.get_selected_printer_preset_name();
                else
                    physical_printers.unselect_printer();

                if (marker == PresetComboBox::LABEL_ITEM_PRINTER_MODELS) {
                    auto preset = q->preset_bundle()->get_similar_printer_preset(preset_name, {});
                    if (preset == nullptr) {
                        MessageDialog dlg(layout.sidebar, _L(""), _L(""));
                        dlg.ShowModal();
                    }
                    preset->is_visible = true; // force visible
                    preset_name = preset->name;
                }
            }
            wxWindowUpdateLocker noUpdates2(layout.sidebar->filament_panel());
            q->get_tab((int)preset_type)->select_preset(preset_name);
        }

        // update plater with new config
        q->on_config_change(q->preset_bundle()->full_config());
        if (preset_type == Preset::TYPE_PRINTER) {
            q->sidebar().obj_list()->update_object_list_by_printer_technology();

            // BBS:Model reset by plate center
            PartPlateList& cur_plate_list = partplate_list;
            PartPlate* cur_plate = cur_plate_list.get_curr_plate();
            Vec3d cur_plate_pos = cur_plate->get_center_origin();

            if (old_plate_pos.x() != cur_plate_pos.x() || old_plate_pos.y() != cur_plate_pos.y()) {
                for (int i = 0; i < plate_object.size(); ++i) {
                    layout.view3D->select_object_from_idx(plate_object[i]);
                    layout.sidebar->obj_list()->update_selections();
                    layout.view3D->center_selected_plate(i);
                }

                layout.view3D->deselect_all();
            }

#if 0   // do not toggle auto calc when change printer
            // update flush matrix
            size_t filament_size = q->get_extruder_colors_from_plater_config().size();
            for (size_t idx = 0; idx < filament_size; ++idx)
                q->sidebar().auto_calc_flushing_volumes(idx);
#endif
        }

#ifdef __WXMSW__
        // From the Win 2004 preset combobox lose a focus after change the preset selection
        // and that is why the up/down arrow doesn't work properly
        // So, set the focus to the combobox explicitly
        combo->SetFocus();
#endif
        if (preset_type == Preset::TYPE_FILAMENT && q->app_config()->get("auto_calculate_when_filament_change") == "true") {
            q->sidebar().auto_calc_flushing_volumes(idx);
        }

        // BBS: log modify of filament selection
        Slic3r::put_other_changes();

        // update slice state and set bedtype default for 3rd-party printer
        auto plate_list = partplate_list.get_plate_list();
        for (auto plate : plate_list) {
             plate->update_slice_result_valid_state(false);
        }
    }

    // 原 priv::on_tab_selection_changing（:2694-2750 逐字；main_frame/wxGetApp→host）
    void on_tab_selection_changing(wxBookCtrlEvent& e)
    {
        MainFrame* mf = main_frame;
        if (!(mf && mf->m_tabpanel && e.GetId() == mf->m_tabpanel->GetId())) {
            e.Skip();
            return;
        }

        const int new_sel = e.GetSelection();
        layout.sidebar_layout.show = new_sel == MainFrame::tp3DEditor || new_sel == MainFrame::tpPreview;
        layout.update_sidebar();
        int old_sel = e.GetOldSelection();
        if (q->preset_bundle() && q->preset_bundle()->use_bbl_device_tab() && new_sel == MainFrame::tpMonitor &&
            q->app_config()->get("use_new_connect") != "true") {
            if (q->get_agent() == nullptr) {
                e.Veto();
                BOOST_LOG_TRIVIAL(info) << boost::format("skipped tab switch from %1% to %2%, lack of network plugins") % old_sel % new_sel;
                if (q) {
                    wxCommandEvent* evt = new wxCommandEvent(EVT_INSTALL_PLUGIN_HINT);
                    wxQueueEvent(q, evt);
                }
            }
        } else {
            if (new_sel == MainFrame::tpMonitor && q->preset_bundle() != nullptr) {
                auto     cfg = q->preset_bundle()->printers.get_edited_preset().config;
                wxString url = cfg.opt_string("print_host_webui").empty() ? cfg.opt_string("print_host") : cfg.opt_string("print_host_webui");
                if (mf->m_printer_view && url.empty()) {
                    // It's missing_connection page, reload so that we can replay the gif image
                }
                if (mf->m_printer_view) {
                    auto printer_model_opt = cfg.option<ConfigOptionString>("printer_model");
                    bool is_snapmaker_u1   = false;
                    if (printer_model_opt) {
                        const std::string& printer_model = printer_model_opt->value;
                        is_snapmaker_u1 = boost::icontains(printer_model, "Snapmaker") && boost::icontains(printer_model, "U1");
                    }
                    const bool showing_u1 = mf->m_printer_view->is_u1_device_page();
                    if (is_snapmaker_u1 && !showing_u1) {
                        wxString u1_url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(q->page_http_port()) +
                                                             "/web/flutter_web/index.html?path=2");
                        mf->load_printer_url(q->international_url(u1_url));
                    } else if (!is_snapmaker_u1 && showing_u1) {
                        wxString non_u1 = url;
                        if (non_u1.empty()) {
                            std::string base_url = LOCALHOST_URL + std::to_string(q->page_http_port());
                            non_u1               = wxString::Format("%s/web/orca/missing_connection.html", from_u8(base_url));
                        } else if (!non_u1.Lower().starts_with("http"))
                            non_u1 = wxString::Format("http://%s", non_u1);
                        mf->load_printer_url(non_u1);
                    }
                }
            }
        }
    }

    // 原 priv::on_filament_color_changed（:2922-2942 逐字）
    void on_filament_color_changed(wxCommandEvent& event)
    {
        int modify_id = event.GetInt();

        auto& ams_multi_color_filment = q->preset_bundle()->ams_multi_color_filment;
        if (modify_id >= 0 && modify_id < ams_multi_color_filment.size())
            ams_multi_color_filment[modify_id].clear();

        if (q->app_config()->get("auto_calculate") == "true") {
            layout.sidebar->auto_calc_flushing_volumes(modify_id);
        }

        // Regenerate mixed filaments and refresh the mixed panel only. Color
        // changes do not alter filament IDs, so the full on_filaments_change()
        // path is unnecessary and can re-enter UI rebuilds mid-update.
        q->preset_bundle()->update_multi_material_filament_presets();
        layout.sidebar->update_mixed_filament_panel();
        layout.sidebar->update_color_mix_panel();

        if (GLCanvas3D* canvas = q->get_view3D_canvas3D())
            canvas->get_gizmos_manager().update_data();
    }

    // 原 priv::on_apple_change_color_mode（:2965-2974 逐字；m_notification_hub→hub）
    void on_apple_change_color_mode(wxSysColourChangedEvent& evt)
    {
        hub.set_is_dark(wxSystemSettings::GetAppearance().IsDark());
        if (layout.view3D->get_canvas3d() && layout.view3D->get_canvas3d()->is_initialized()) {
            layout.view3D->get_canvas3d()->on_change_color_mode(hub.is_dark());
            layout.preview->get_canvas3d()->on_change_color_mode(hub.is_dark());
            layout.assemble_view->get_canvas3d()->on_change_color_mode(hub.is_dark());
        }

        layout.apply_color_mode();
    }

    // 原 priv::on_change_color_mode（:2976-2984 逐字）
    void on_change_color_mode(SimpleEvent& evt)
    {
        hub.set_is_dark(q->app_config()->get("dark_color_mode") == "1");
        layout.view3D->get_canvas3d()->on_change_color_mode(hub.is_dark());
        layout.preview->get_canvas3d()->on_change_color_mode(hub.is_dark());
        layout.assemble_view->get_canvas3d()->on_change_color_mode(hub.is_dark());
        ex.notify_send_to_sdcard_color_mode();

        layout.apply_color_mode();
    }

    // 原 Plater.cpp 静态 get_position（:2988-3002 逐字随迁）
    static void get_position(wxWindowBase* child, wxWindowBase* until_parent, int& x, int& y)
    {
        int res_x = 0, res_y = 0;

        while (child != until_parent && child != nullptr) {
            int _x, _y;
            child->GetPosition(&_x, &_y);
            res_x += _x;
            res_y += _y;

            child = child->GetParent();
        }

        x = res_x;
        y = res_y;
    }

    // 原 priv::show_right_click_menu（:3004-3020 逐字）
    void show_right_click_menu(Vec2d mouse_position, wxMenu* menu)
    {
        int x, y;
        get_position(layout.current_panel, main_frame, x, y);
        wxPoint position(static_cast<int>(mouse_position.x() + x), static_cast<int>(mouse_position.y() + y));
#ifdef __linux__
        // For some reason on Linux the menu isn't displayed if the position is
        // specified (even though the position is sane).
        position = wxDefaultPosition;
#endif
        GLCanvas3D& canvas = *q->canvas3D();
        canvas.apply_retina_scale(mouse_position);
        canvas.set_popup_menu_position(mouse_position);
        q->PopupMenu(menu, position);
        canvas.clear_popup_menu_position();
    }

    // 原 priv::on_right_click（:3022-3084 逐字；判定核抽 plan_right_click_menu_kind——D7）
    void on_right_click(RBtnEvent& evt)
    {
        int obj_idx = q->get_selected_object_idx();

        wxMenu* menu = nullptr;
        MenuFactory& menus = menus;
        const bool no_object         = obj_idx == -1;
        const bool clicked_empty     = evt.data.second;
        const bool selection_empty   = q->get_selection().is_empty();
        const bool panel_is_assemble = layout.current_panel == layout.assemble_view;
        const bool sla               = session.printer_technology == ptSLA;

        if (no_object) { // no one or several object are selected
            if (clicked_empty) { // right button was clicked on empty space
                if (!selection_empty) // several objects are selected in 3DScene
                    return;
                menu = menus.default_menu();
            }
            else {
                menu = panel_is_assemble ? menus.assemble_multi_selection_menu() : menus.multi_selection_menu();
            }
        }
        else {
            // If in 3DScene is(are) selected volume(s), but right button was clicked on empty space
            if (clicked_empty)
                return;

            // Each context menu respects to the selected item in ObjectList,
            // so this selection should be updated before menu creation
            q->sidebar().obj_list()->update_selections();

            if (sla)
                menu = menus.sla_object_menu();
            else {
                const Selection& selection = q->get_selection();
                const bool is_some_full_instances = selection.is_single_full_instance() ||
                                                    selection.is_single_full_object() ||
                                                    selection.is_multiple_full_instance();
                const bool is_part = selection.is_single_volume() || selection.is_single_modifier();

                if (panel_is_assemble) {
                    menu = is_some_full_instances ? menus.assemble_object_menu() :
                           is_part                ? menus.assemble_part_menu()    : menus.assemble_multi_selection_menu();
                } else {
                    if (is_some_full_instances)
                        menu = sla ? menus.sla_object_menu() : menus.object_menu();
                    else if (is_part) {
                        const GLVolume* gl_volume = selection.get_first_volume();
                        const ModelVolume* model_volume = get_model_volume(*gl_volume, selection.get_model()->objects);
                        menu = (model_volume != nullptr && model_volume->is_text()) ? menus.text_part_menu() :
                               (model_volume != nullptr && model_volume->is_svg())  ? menus.svg_part_menu()  :
                               menus.part_menu();
                    } else
                        menu = menus.multi_selection_menu();
                }
            }
        }

        if (q != nullptr && menu) {
            show_right_click_menu(evt.data.first, menu);
        }
    }

    // 原 priv::on_plate_right_click（:3087-3091 逐字）
    void on_plate_right_click(RBtnPlateEvent& evt)
    {
        wxMenu* menu = menus.plate_menu();
        show_right_click_menu(evt.data.first, menu);
    }

    // 原 priv::on_update_geometry（:3093-3096 空体逐字）
    void on_update_geometry(Vec3dsEvent<2>&) {}

    // 原 priv::on_3dcanvas_mouse_dragging_started/finished（:3098-3113 逐字；G4 D3 consume）
    void on_3dcanvas_mouse_dragging_started(SimpleEvent&)
    {
        layout.view3D->get_canvas3d()->reset_sequential_print_clearance();
    }
    void on_3dcanvas_mouse_dragging_finished(SimpleEvent&)
    {
        if (refresh.consume_delayed_scene_refresh()) {
            refresh.update_sla_scene();
        }
    }

    // 原 priv::on_create_filament（:3350-3361 逐字；wxGetApp→host）
    void on_create_filament(SimpleEvent&)
    {
        CreateFilamentPresetDialog dlg(main_frame);
        int res = dlg.ShowModal();
        if (wxID_OK == res) {
            main_frame->update_side_preset_ui();
            layout.update_ui_from_settings();
            layout.sidebar->update_all_preset_comboboxes();
            CreatePresetSuccessfulDialog success_dlg(main_frame, SuccessType::FILAMENT);
            int res2 = success_dlg.ShowModal();
        }
    }

    // 原 priv::on_modify_filament（:3363-3390 逐字）
    void on_modify_filament(SimpleEvent& evt)
    {
        Filamentinformation* filament_info = static_cast<Filamentinformation*>(evt.GetEventObject());
        int res;
        std::shared_ptr<Preset> need_edit_preset;
        {
            EditFilamentPresetDialog dlg(main_frame, filament_info);
            res = dlg.ShowModal();
            need_edit_preset = dlg.get_need_edit_preset();
        }
        main_frame->update_side_preset_ui();
        layout.update_ui_from_settings();
        layout.sidebar->update_all_preset_comboboxes();
        if (wxID_EDIT == res) {
            Tab* tab = q->get_tab((int)Preset::Type::TYPE_FILAMENT);
            if (tab == nullptr) { return; }
            q->params_dialog()->Popup();
            tab->restore_last_select_item();
            tab->set_just_edit(true);
            tab->select_preset(need_edit_preset->name);
            if (!need_edit_preset->is_compatible) tab->select_preset(need_edit_preset->name);
        }
    }

    // 原 priv::on_add_filament/on_delete_filament/on_add_custom_filament（:3392-3403 逐字）
    void on_add_filament(SimpleEvent&) { layout.sidebar->add_filament(); }
    void on_delete_filament(SimpleEvent&) { layout.sidebar->delete_filament(); }
    void on_add_custom_filament(ColorEvent& evt) { layout.sidebar->add_custom_filament(evt.data); }

    // —— install/uninstall/rebind（原 priv ctor Bind 区 :1530-1548/:1549/:1625-1963 逐字平移）——
    void install();
    void uninstall();
};

// —— install：priv ctor 138 处 Bind 逐字平移（wxEVT_TIMER bind 与视图构造/AUI/相机/快照初始化留守 ctor——定时器域纪律）——
void PlaterEventRouter::Impl::install()
{
    MainFrame* mf = main_frame;
    wxGLCanvas* view3D_canvas = layout.view3D->get_wxglcanvas();
    wxGLCanvas* preview_canvas = layout.preview->get_wxglcanvas();
    wxGLCanvas* assemble_canvas = layout.assemble_view->get_wxglcanvas();

    // 原 :1608-1613（sidebar wxEVT_IDLE 尺寸追踪——pane info 为 aui 管理器所有，引用捕获安全）
    {
        auto& sidebar_pane = layout.aui()->GetPane(layout.sidebar);
        layout.sidebar->Bind(wxEVT_IDLE, [&sidebar_pane](wxIdleEvent& e) {
            if (sidebar_pane.IsShown() && sidebar_pane.IsDocked() && sidebar_pane.rect.GetWidth() > 0) {
                sidebar_pane.BestSize(sidebar_pane.rect.GetWidth(), sidebar_pane.best_size.GetHeight());
            }
            e.Skip();
        });
    }

    // 原 :1530-1548（q 事件族）
    q->Bind(EVT_SLICING_UPDATE, &Impl::on_slicing_update, this);
    q->Bind(EVT_PUBLISH, &Impl::on_action_publish, this);
    q->Bind(EVT_REPAIR_MODEL, &Impl::on_repair_model, this);
    q->Bind(EVT_FILAMENT_COLOR_CHANGED, &Impl::on_filament_color_changed, this);
    q->Bind(EVT_INSTALL_PLUGIN_NETWORKING, &Impl::install_network_plugin, this);
    q->Bind(EVT_INSTALL_PLUGIN_HINT, &Impl::show_install_plugin_hint, this);
    q->Bind(EVT_UPDATE_PLUGINS_WHEN_LAUNCH, &Impl::update_plugin_when_launch, this);
    q->Bind(EVT_PREVIEW_ONLY_MODE_HINT, &Impl::show_preview_only_hint, this);
    q->Bind(EVT_GLCANVAS_COLOR_MODE_CHANGED, &Impl::on_change_color_mode, this);
    q->Bind(wxEVT_SYS_COLOUR_CHANGED, &Impl::on_apple_change_color_mode, this);
    q->Bind(EVT_CREATE_FILAMENT, &Impl::on_create_filament, this);
    q->Bind(EVT_MODIFY_FILAMENT, &Impl::on_modify_filament, this);
    q->Bind(EVT_ADD_CUSTOM_FILAMENT, &Impl::on_add_custom_filament, this);
    q->Bind(EVT_FILAMENT_USAGE_CHANGED, [this](SimpleEvent&) {
        filament_usage_sync_pending = false;
        q->sync_filament_temp_mixing_notification();
        q->sync_flow_ratio_zero_notification();
        q->sync_cold_plate_notification();
    });
    // 原 :1549（tabpanel）
    if (mf && mf->m_tabpanel)
        mf->m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGING, &Impl::on_tab_selection_changing, this);

    // 原 :1625-1639（sidebar + q 编辑器事件族）
    if (q->is_editor()) {
        layout.sidebar->Bind(wxEVT_COMBOBOX, &Impl::on_combobox_select, this);
        layout.sidebar->Bind(EVT_OBJ_LIST_OBJECT_SELECT, [this](wxEvent&) { refresh.selection_changed(); });
        q->Bind(EVT_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { slicing.schedule_background_process(); });
        // jump to found option from SearchDialog
        q->Bind(wxCUSTOMEVT_JUMP_TO_OPTION, [this](wxCommandEvent& evt) { layout.sidebar->jump_to_option(evt.GetInt()); });
        q->Bind(wxCUSTOMEVT_JUMP_TO_OBJECT, [this](wxCommandEvent& evt) {
            auto client_data = evt.GetClientData();
            ObjectDataViewModelNode* data = static_cast<ObjectDataViewModelNode*>(client_data);
            layout.sidebar->jump_to_object(data);
            }
        );
    }

    // 原 :1645-1732（view3D_canvas 事件族）
    if (q->is_editor()) {
        view3D_canvas->Bind(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) {
            refresh.clear_delayed_error_message();
            slicing.start_update_timer();
            });
        view3D_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &Impl::on_object_select, this);
        view3D_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &Impl::on_right_click, this);
        view3D_canvas->Bind(EVT_GLCANVAS_PLATE_RIGHT_CLICK, &Impl::on_plate_right_click, this);
        view3D_canvas->Bind(EVT_GLCANVAS_REMOVE_OBJECT, [this](SimpleEvent&) { q->remove_selected(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE, [this](SimpleEvent& evt) {
            q->arrange(app::ArrangeRequest::all_plates()); });
        view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE_PARTPLATE, [this](SimpleEvent& evt) {
            q->arrange(app::ArrangeRequest::current_plate()); });
        view3D_canvas->Bind(EVT_GLCANVAS_ORIENT, [this](SimpleEvent& evt) {
            q->orient(app::OrientRequest::selection()); });
        view3D_canvas->Bind(EVT_GLCANVAS_ORIENT_PARTPLATE, [this](SimpleEvent& evt) {
            q->orient(app::OrientRequest::current_plate()); });
        view3D_canvas->Bind(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, [this](SimpleEvent&) { q->select_curr_plate_all(); });

        view3D_canvas->Bind(EVT_GLCANVAS_SELECT_ALL, [this](SimpleEvent&) { q->select_all(); });
        view3D_canvas->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { q->keyboard_shortcuts(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INCREASE_INSTANCES, [this](Event<int>& evt)
            { if (evt.data == 1) q->increase_instances(); else if (obj_cmds.can_decrease_instances()) q->decrease_instances(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_MOVED, [this](SimpleEvent&) { refresh.update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_FORCE_UPDATE, [this](SimpleEvent&) { refresh.update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_ROTATED, [this](SimpleEvent&) { refresh.update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_SCALED, [this](SimpleEvent&) { refresh.update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, [this](Event<bool>& evt) { on_slice_button_status(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_GEOMETRY, &Impl::on_update_geometry, this);
        view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, &Impl::on_3dcanvas_mouse_dragging_started, this);
        view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, &Impl::on_3dcanvas_mouse_dragging_finished, this);
        view3D_canvas->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { layout.select_next_view_3D(); });
        view3D_canvas->Bind(EVT_GLCANVAS_RESETGIZMOS, [this](SimpleEvent&) { q->reset_all_gizmos(); });
        view3D_canvas->Bind(EVT_GLCANVAS_UNDO, [this](SimpleEvent&) { snapshots.undo(); });
        view3D_canvas->Bind(EVT_GLCANVAS_REDO, [this](SimpleEvent&) { snapshots.redo(); });
        view3D_canvas->Bind(EVT_GLCANVAS_COLLAPSE_SIDEBAR, [this](SimpleEvent&) { q->collapse_sidebar(!q->is_sidebar_collapsed()); });
        view3D_canvas->Bind(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, [this](SimpleEvent&) { layout.view3D->get_canvas3d()->reset_layer_height_profile(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, [this](Event<float>& evt) { layout.view3D->get_canvas3d()->adaptive_layer_height_profile(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, [this](HeightProfileSmoothEvent& evt) { layout.view3D->get_canvas3d()->smooth_layer_height_profile(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_RELOAD_FROM_DISK, [this](SimpleEvent&) { q->reload_all_from_disk(); });

        // 3DScene/Toolbar:
        view3D_canvas->Bind(EVT_GLTOOLBAR_ADD, &Impl::on_action_add, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE, [this](SimpleEvent&) { q->remove_selected(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE_ALL, [this](SimpleEvent&) { obj_cmds.delete_all_objects_from_model(); });

        view3D_canvas->Bind(EVT_GLTOOLBAR_ADD_PLATE, &Impl::on_action_add_plate, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_DEL_PLATE, &Impl::on_action_del_plate, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_CUT, [this](SimpleEvent&) { obj_cmds.cut_selection_to_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_COPY, [this](SimpleEvent&) { obj_cmds.copy_selection_to_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_PASTE, [this](SimpleEvent&) { obj_cmds.paste_from_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_LAYERSEDITING, &Impl::on_action_layersediting, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_CLONE, [this](SimpleEvent&) { q->clone_selection(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_MORE, [this](SimpleEvent&) { q->increase_instances(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_FEWER, [this](SimpleEvent&) { q->decrease_instances(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_OBJECTS, &Impl::on_action_split_objects, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_VOLUMES, &Impl::on_action_split_volumes, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_OPEN_PROJECT, &Impl::on_action_open_project, this);
        view3D_canvas->Bind(EVT_GLVIEWTOOLBAR_ASSEMBLE, [this](SimpleEvent&) { q->select_view_3D("Assemble"); });
        view3D_canvas->Bind(EVT_GLCANVAS_SWITCH_TO_OBJECT, [mf](SimpleEvent&) {
                if (mf && mf->m_param_panel) {
                    mf->m_param_panel->switch_to_object(false);
                }
            });
        view3D_canvas->Bind(EVT_GLCANVAS_SWITCH_TO_GLOBAL, [mf](SimpleEvent&) {
                if (mf && mf->m_param_panel) {
                    mf->m_param_panel->switch_to_global();
                }
            });
    }
    view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [this](SimpleEvent&) { q->set_bed_shape(); });

    // 原 :1735-1767（Preview 事件族）
    layout.preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { q->keyboard_shortcuts(); });
    layout.preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [this](SimpleEvent&) { q->set_bed_shape(); });
    layout.preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_UPDATE, [this](SimpleEvent&) {
            layout.preview->get_canvas3d()->set_as_dirty();
        });
    if (q->is_editor()) {
        layout.preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { layout.select_next_view_3D(); });
        layout.preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_COLLAPSE_SIDEBAR, [this](SimpleEvent&) { q->collapse_sidebar(!q->is_sidebar_collapsed()); });
        layout.preview->get_wxglcanvas()->Bind(EVT_CUSTOMEVT_TICKSCHANGED, [this](wxCommandEvent& event) {
            Type tick_event_type = (Type)event.GetInt();
            Model& plater_model = q->model();
            //BBS: replace model custom gcode with current plate custom gcode
            plater_model.plates_custom_gcodes[plater_model.curr_plate_index] = layout.preview->get_canvas3d()->get_gcode_viewer().get_layers_slider()->GetTicksValues();

            // BBS set to invalid state only
            if (tick_event_type == Type::ToolChange || tick_event_type == Type::Custom || tick_event_type == Type::Template || tick_event_type == Type::PausePrint) {
                PartPlate* plate = q->get_partplate_list().get_curr_plate();
                if (plate) {
                    plate->update_slice_result_valid_state(false);
                }
            }
            q->set_plater_dirty(true);

            layout.preview->on_tick_changed(tick_event_type);

            // update slice and print button
            if (MainFrame* mfx = main_frame)
                mfx->update_slice_print_status(MainFrame::SlicePrintEventType::eEventSliceUpdate, true, false);
            refresh.set_need_update(true);
        });
    }
    if (q->is_gcode_viewer())
        layout.preview->Bind(EVT_GLCANVAS_RELOAD_FROM_DISK, [this](SimpleEvent&) { this->session.reload_gcode_from_disk(); });

    // 原 :1770-1779（assemble_canvas 事件族）
    if (q->is_editor()) {
        assemble_canvas->Bind(EVT_GLTOOLBAR_FILLCOLOR, [this](IntEvent& evt) { q->fill_color(evt.get_data()); });
        assemble_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &Impl::on_object_select, this);
        assemble_canvas->Bind(EVT_GLVIEWTOOLBAR_3D, [this](SimpleEvent&) { q->select_view_3D("3D"); });
        assemble_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &Impl::on_right_click, this);
        assemble_canvas->Bind(EVT_GLCANVAS_FORCE_UPDATE, [this](SimpleEvent&) { refresh.update(); });
        assemble_canvas->Bind(EVT_GLCANVAS_UNDO, [this](SimpleEvent&) { snapshots.undo(); });
        assemble_canvas->Bind(EVT_GLCANVAS_REDO, [this](SimpleEvent&) { snapshots.redo(); });
    }

    // 原 :1781-1823（q 编辑器事件族二）
    if (q->is_editor()) {
        q->Bind(EVT_SLICING_COMPLETED, &Impl::on_slicing_completed, this);
        q->Bind(EVT_PROCESS_COMPLETED, &Impl::on_process_completed, this);
        q->Bind(EVT_EXPORT_BEGAN, &Impl::on_export_began, this);
        q->Bind(EVT_EXPORT_FINISHED, &Impl::on_export_finished, this);
        q->Bind(EVT_GLVIEWTOOLBAR_3D, [this](SimpleEvent&) { q->select_view_3D("3D"); });
        //BBS: set on_slice to false
        q->Bind(EVT_GLVIEWTOOLBAR_PREVIEW, [this](SimpleEvent&) {
            if (q->is_view3D_shown()) {
                PartPlate* curr = q->get_partplate_list().get_curr_plate();
                if (curr && !q->is_plate_sliceable(q->get_partplate_list().get_curr_plate_index())) {
                    q->sync_filament_temp_mixing_notification();
                    q->sync_flow_ratio_zero_notification();
                    q->sync_cold_plate_notification();
                    q->select_view_3D("Preview", true);
                    return;
                }
            }
            q->select_view_3D("Preview", false);
        });
        q->Bind(EVT_GLTOOLBAR_SLICE_PLATE, &Impl::on_action_slice_plate, this);
        q->Bind(EVT_GLTOOLBAR_SLICE_ALL, &Impl::on_action_slice_all, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_PLATE, &Impl::on_action_print_plate, this);
        q->Bind(EVT_PRINT_FROM_SDCARD_VIEW, &Impl::on_action_print_plate_from_sdcard, this);
        q->Bind(EVT_GLTOOLBAR_SELECT_SLICED_PLATE, &Impl::on_action_select_sliced_plate, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_ALL, &Impl::on_action_print_all, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_GCODE, &Impl::on_action_export_gcode, this);
        q->Bind(EVT_GLTOOLBAR_SEND_GCODE, &Impl::on_action_send_gcode, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_SLICED_FILE, &Impl::on_action_export_sliced_file, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE, &Impl::on_action_export_all_sliced_file, this);
        q->Bind(EVT_GLTOOLBAR_SEND_TO_PRINTER, &Impl::on_action_export_to_sdcard, this);
        q->Bind(EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL, &Impl::on_action_export_to_sdcard_all, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE, &Impl::on_action_send_to_multi_machine, this);
        q->Bind(EVT_GLCANVAS_PLATE_SELECT, &Impl::on_plate_selected, this);
        q->Bind(EVT_DOWNLOAD_PROJECT, &Impl::on_action_download_project, this);
        q->Bind(EVT_IMPORT_MODEL_ID, &Impl::on_action_request_model_id, this);
        q->Bind(EVT_PRINT_FINISHED, [this](wxCommandEvent& evt) { q->print_job_finished(evt); });
        q->Bind(EVT_SEND_CALIBRATION_FINISHED, [this](wxCommandEvent& evt) { q->send_calibration_job_finished(evt); });
        q->Bind(EVT_SEND_FINISHED, [this](wxCommandEvent& evt) { q->send_job_finished(evt); });
        q->Bind(EVT_PUBLISH_FINISHED, [this](wxCommandEvent& evt) { q->publish_job_finished(evt); });
        q->Bind(EVT_OPEN_PLATESETTINGSDIALOG, [this](wxCommandEvent& evt) { q->open_platesettings_dialog(evt); });
    }

    // 原 :1852-1898（HID + 通知点击 + 可移动盘族）
#ifdef _WIN32
    q->Bind(EVT_HID_DEVICE_ATTACHED, [this](HIDDeviceAttachedEvent& evt) {
        layout.mouse3d_controller.device_attached(evt.data);
        });
    q->Bind(EVT_HID_DEVICE_DETACHED, [this](HIDDeviceAttachedEvent& evt) {
        layout.mouse3d_controller.device_detached(evt.data);
        });
#endif
    if (q->is_editor()) {
        q->Bind(EVT_EJECT_DRIVE_NOTIFICAION_CLICKED, [this](EjectDriveNotificationClickedEvent&) { q->eject_drive(); });
        q->Bind(EVT_EXPORT_GCODE_NOTIFICAION_CLICKED, [this](ExportGcodeNotificationClickedEvent&) { q->export_gcode(true); });
        q->Bind(EVT_PRESET_UPDATE_AVAILABLE_CLICKED, [this](PresetUpdateAvailableClickedEvent&) { q->preset_updater()->on_update_notification_confirm(); });
        q->Bind(EVT_PRINTER_CONFIG_UPDATE_AVAILABLE_CLICKED, [this](PrinterConfigUpdateAvailableClickedEvent&) {
            q->preset_updater()->do_printer_config_update();
            q->device_manager()->reload_printer_settings(); });

        this->q->Bind(EVT_REMOVABLE_DRIVE_EJECTED, [this](RemovableDriveEjectEvent& evt) {
            if (evt.data.second) {
                hub.nm()->close_notification_of_type(NotificationType::ExportFinished);
                hub.nm()->push_notification(NotificationType::CustomNotification,
                                            NotificationManager::NotificationLevel::RegularNotificationLevel,
                                            format(_L("Successfully unmounted. The device %s (%s) can now be safely removed from the computer."), evt.data.first.name, evt.data.first.path)
                );
            } else {
                hub.nm()->push_notification(NotificationType::CustomNotification,
                                            NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                            format(_L("Ejecting of device %s (%s) has failed."), evt.data.first.name, evt.data.first.path)
                );
            }
        });
        q->Bind(EVT_REMOVABLE_DRIVES_CHANGED, [this](RemovableDrivesChangedEvent&) {
            // Close notification ExportingFinished but only if last export was to removable
            hub.nm()->device_ejected();
        });
#ifdef _WIN32
        q->Bind(EVT_VOLUME_ATTACHED, [this](VolumeAttachedEvent& evt) { q->removable_drive_manager()->volumes_changed(); });
        q->Bind(EVT_VOLUME_DETACHED, [this](VolumeDetachedEvent& evt) { q->removable_drive_manager()->volumes_changed(); });
#endif
    }

    // 原 :1913-1940（restore project；last_backup 由 app_config 槽读取——原 ctor 时点语义保持）
    if (q->is_editor()) {
        auto last_backup = q->app_config()->get_last_backup_dir();
        q->Bind(EVT_RESTORE_PROJECT, [this, last = last_backup](wxCommandEvent& e) {
            std::string last_b = last;
            std::string originfile;
            if (Slic3r::has_restore_data(last_b, originfile)) {
                auto result = MessageDialog(q, _L("Previous unsaved project detected, do you want to restore it?"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Restore"), wxYES_NO | wxYES_DEFAULT | wxCENTRE).ShowModal();
                if (result == wxID_YES) {
                    this->session.load_project(from_path(last_b), from_path(originfile));
                    Slic3r::backup_soon();
                    return;
                }
            }
            try {
                if (originfile != "<lock>") // see bbs_3mf.cpp for lock detail
                    boost::filesystem::remove_all(last_b);
            }
            catch (...) {}
            int skip_confirm = e.GetInt();
            q->new_project(skip_confirm, true);
            // Startup blank project: restore the flow types remembered in AppConfig
            GUI::FlowType::restore_nozzle_volume_types_from_app_config();
            });
    }

    // 原 :1942-1962（跨实例事件族）
    q->Bind(EVT_LOAD_MODEL_OTHER_INSTANCE, [this](LoadFromOtherInstanceEvent& evt) {
        BOOST_LOG_TRIVIAL(trace) << "Received load from other instance event.";
        wxArrayString input_files;
        for (size_t i = 0; i < evt.data.size(); ++i) {
            input_files.push_back(from_u8(evt.data[i].string()));
        }
        if (MainFrame* mfx = main_frame)
            mfx->Raise();
        q->load_files(input_files);
    });

    q->Bind(EVT_START_DOWNLOAD_OTHER_INSTANCE, [this](StartDownloadOtherInstanceEvent& evt) {
        BOOST_LOG_TRIVIAL(trace) << "Received url from other instance event.";
        if (MainFrame* mfx = main_frame)
            mfx->Raise();
        for (size_t i = 0; i < evt.data.size(); ++i) {
            q->start_download(evt.data[i]);
        }
    });
    q->Bind(EVT_INSTANCE_GO_TO_FRONT, [this](InstanceGoToFrontEvent&) {
        q->bring_instance_forward();
    });
}

// —— uninstall：install 的对称解绑（成员函数指针绑定 Unbind + lambda 绑定以 Unbind(wxEVT_ANY…) 不可行，
//    wx 语义：lambda Bind 无法具名解绑——uninstall 采用 wxWindow::Unbind 全事件族逐条对称、
//    lambda 绑定随目标窗口析构自然消亡（install 仅在 ctor 调一次，uninstall/rebind 供视图重挂扩展）——
void PlaterEventRouter::Impl::uninstall()
{
    MainFrame* mf = main_frame;
    if (mf && mf->m_tabpanel)
        mf->m_tabpanel->Unbind(wxEVT_NOTEBOOK_PAGE_CHANGING, &Impl::on_tab_selection_changing, this);

    q->Unbind(EVT_SLICING_UPDATE, &Impl::on_slicing_update, this);
    q->Unbind(EVT_PUBLISH, &Impl::on_action_publish, this);
    q->Unbind(EVT_REPAIR_MODEL, &Impl::on_repair_model, this);
    q->Unbind(EVT_FILAMENT_COLOR_CHANGED, &Impl::on_filament_color_changed, this);
    q->Unbind(EVT_INSTALL_PLUGIN_NETWORKING, &Impl::install_network_plugin, this);
    q->Unbind(EVT_INSTALL_PLUGIN_HINT, &Impl::show_install_plugin_hint, this);
    q->Unbind(EVT_UPDATE_PLUGINS_WHEN_LAUNCH, &Impl::update_plugin_when_launch, this);
    q->Unbind(EVT_PREVIEW_ONLY_MODE_HINT, &Impl::show_preview_only_hint, this);
    q->Unbind(EVT_GLCANVAS_COLOR_MODE_CHANGED, &Impl::on_change_color_mode, this);
    q->Unbind(wxEVT_SYS_COLOUR_CHANGED, &Impl::on_apple_change_color_mode, this);
    q->Unbind(EVT_CREATE_FILAMENT, &Impl::on_create_filament, this);
    q->Unbind(EVT_MODIFY_FILAMENT, &Impl::on_modify_filament, this);
    q->Unbind(EVT_ADD_CUSTOM_FILAMENT, &Impl::on_add_custom_filament, this);
}

// —— Router 公有面（pimpl 委托）——

PlaterEventRouter::PlaterEventRouter(Plater* q, NotificationHub& hub,
                                     SlicingController& slicing, ProjectSession& session,
                                     ExportSendCoordinator& ex, PlateController& plates,
                                     ObjectEditCommands& obj_cmds, LayoutManager& layout,
                                     SceneRefreshCoordinator& refresh, SnapshotService& snapshots,
                                      MainFrame* main_frame, MenuFactory& menus, PartPlateList& partplate_list,
                                      Model& model, std::atomic<bool>& filament_usage_sync_pending)
    : m_impl(new Impl{q, hub, slicing, session, ex, plates, obj_cmds, layout, refresh, snapshots, main_frame, menus, partplate_list, model, filament_usage_sync_pending})
{
}

PlaterEventRouter::~PlaterEventRouter() = default;


void PlaterEventRouter::install() { m_impl->install(); }
void PlaterEventRouter::uninstall() { m_impl->uninstall(); }
void PlaterEventRouter::rebind() { m_impl->uninstall(); m_impl->install(); }

// —— 纯函数两件（D7；判定语义 = 原 on_right_click/on_tab_selection_changing 分支逐字）——

PlaterEventRouter::RightClickMenuKind PlaterEventRouter::plan_right_click_menu_kind(
    bool no_object, bool clicked_empty, bool selection_empty, bool panel_is_assemble,
    bool sla, bool some_full_instances, bool is_part,
    bool model_volume_text, bool model_volume_svg)
{
    if (no_object) {
        if (clicked_empty) return selection_empty ? RightClickMenuKind::Default : RightClickMenuKind::None;
        return panel_is_assemble ? RightClickMenuKind::AssembleMultiSelection : RightClickMenuKind::MultiSelection;
    }
    if (clicked_empty) return RightClickMenuKind::None;
    if (sla) return RightClickMenuKind::SlaObjectMenu;
    if (panel_is_assemble) {
        return some_full_instances ? RightClickMenuKind::AssembleObjectMenu :
               is_part             ? RightClickMenuKind::AssemblePartMenu    : RightClickMenuKind::AssembleMultiSelection;
    }
    if (some_full_instances) return RightClickMenuKind::ObjectMenu;
    if (is_part) {
        return model_volume_text ? RightClickMenuKind::TextPartMenu :
               model_volume_svg  ? RightClickMenuKind::SvgPartMenu  : RightClickMenuKind::PartMenu;
    }
    return RightClickMenuKind::MultiSelection;
}

PlaterEventRouter::TabSelectionPlan PlaterEventRouter::plan_tab_selection(bool is_tabpanel_event, int new_sel, int tp3d, int tp_preview,
                                                                          bool monitor_guard_active, bool has_agent)
{
    TabSelectionPlan plan;
    if (!is_tabpanel_event) {          // Ignore event raised by child controls（调用方 e.Skip()+return）
        plan.veto_skip = true;
        return plan;
    }
    plan.sidebar_show = new_sel == tp3d || new_sel == tp_preview;   // 原 :2703 sidebar_layout.show
    if (monitor_guard_active && !has_agent) {                       // 原 :2706-2715 Monitor 无网络插件守卫
        plan.veto_skip = true;
        plan.install_hint = true;
    }
    return plan;
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
