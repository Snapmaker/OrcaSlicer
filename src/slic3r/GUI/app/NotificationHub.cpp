// NotificationHub — Plater 通知域服务实现（阶段 C 纯搬移，手册 §3.3 / §2.2）。
// ⚠ 本文件禁止 include Plater/GUI_App 宿主头（陷阱 #1、DoD #6）——
//   不得照抄 Plater.cpp 的 include 大杂烩，下列为按编译需要收敛的"真实集"：
#include "NotificationHub.hpp"
#include "../Plater.hpp"
#include "../GUI_ObjectList.hpp"   // H2 3.1 顶部瘦身：ObjectList 完整型（原经 Plater.hpp→Search.hpp 传递链）        // 阶段 H1 2.1：F1/F3 门面直调（view3D/preview canvas/sidebar 桥接）
#include "LayoutManager.hpp"     // 阶段 H1 2.1：late_bind 后 view3D 直读
#include "../GUI_Preview.hpp"    // 阶段 H1 2.1：View3D 完整类型（is_layers_editing_enabled）
#include "ProjectSession.hpp"    // 阶段 H1 2.1：late_bind 后 printer_technology/using_exported_file 直读
#include "SceneRefreshCoordinator.hpp"  // 阶段 H1 2.1：late_bind 后 skip_thumbnail_invalid 直调
#include "SlicingController.hpp" // 阶段 H1 2.1：late_bind 后 slicing_cancel 直调

#include "../NotificationManager.hpp"      // NotificationManager 完整类型（unique_ptr 析构）
#include "../MsgDialog.hpp"                 // MessageDialog（warnings_dialog / notify_vhl）
#include "../DailyTips.hpp"                 // DailyTipsWindow
#include "../GLCanvas3D.hpp"                // GLCanvas3D / ThumbnailsParams / Camera
#include "../PartPlate.hpp"                 // PartPlateList / PartPlate
#include "../GUI_Preview.hpp"               // View3D 完整类型
#include "../wxExtensions.hpp"              // from_u8
#include "../I18N.hpp"                      // _u8L / _L
#include "libslic3r/PrintBase.hpp"          // PrintBase / PrintStateBase / StringObjectException
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"             // 阶段 H1 2.1：Preset::TYPE_PRINT（jump_to_option 槽体逐字）              // Model / ModelObject
#include "boost/log/trivial.hpp"            // BOOST_LOG_TRIVIAL

namespace Slic3r {
namespace GUI {
namespace app {

NotificationHub::NotificationHub(Plater* q, Model& model, PartPlateList& partplate_list)
    : m_q(q)
    , m_model(model)
    , m_partplate_list(partplate_list)
    , m_notification_manager(std::make_unique<NotificationManager>(q))
{
}

NotificationHub::~NotificationHub() = default;

void NotificationHub::late_bind(SlicingController& slicing, ProjectSession& session,
                                LayoutManager& layout, SceneRefreshCoordinator& refresh)
{
    m_slicing = &slicing;
    m_session = &session;
    m_layout  = &layout;
    m_refresh = &refresh;
}

bool NotificationHub::slicing_cancel()
{
    if (m_slicing->background_process().idle())
        return false;
    m_slicing->background_process().stop();
    return true;
}

// 原 Plater::priv::add_warning（Plater.cpp:5937-5949）——纯聚合，无 wx。
void NotificationHub::add_warning(const PrintStateBase::Warning& warning, size_t oid)
{
    for (auto& it : m_warnings) {
        if (warning.message_id == it.first.message_id) {
            if (warning.message_id != 0 || (warning.message_id == 0 && warning.message == it.first.message))
            {
                if (warning.message_id != 0)
                    it.first.message = warning.message;
                return;
            }
        }
    }
    m_warnings.emplace_back(std::pair<PrintStateBase::Warning, size_t>(warning, oid));
}

// 原 Plater::priv::clear_warnings（Plater.cpp:5973-5976）——通知侧经 nm()，聚合侧无 wx。
void NotificationHub::clear_warnings()
{
    m_notification_manager->close_slicing_errors_and_warnings();
    this->m_warnings.clear();
}

// 原 Plater::priv::process_validation_warning（Plater.cpp:4074-4125）。
void NotificationHub::process_validation_warning(const StringObjectException& warning) const
{
    if (warning.string.empty())
        m_notification_manager->close_notification_of_type(NotificationType::ValidateWarning);
    else {
        std::string text = warning.string;
        auto po = dynamic_cast<PrintObjectBase const *>(warning.object);
        auto mo = po ? po->model_object() : dynamic_cast<ModelObject const *>(warning.object);
        auto action_fn = (mo || !warning.opt_key.empty()) ? [id = mo ? mo->id() : 0, opt = warning.opt_key, this](wxEvtHandler *) {
		    auto & objects = m_model.objects;
		    auto iter = id.id ? std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; }) : objects.end();
            if (iter != objects.end()) {
                m_q->select_tab_3d_editor();
			    m_q->sidebar().obj_list()->select_items({{*iter, nullptr}});
            }
            if (!opt.empty()) {
                if (iter != objects.end())
				    m_q->params_panel_switch_to_object();
                m_q->sidebar().jump_to_option(opt, Preset::TYPE_PRINT, L"");
		    }
		    return false;
	    } : std::function<bool(wxEvtHandler *)>();
        auto hypertext = (mo || !warning.opt_key.empty()) ? _u8L("Jump to") : "";
        if (mo) hypertext += std::string(" [") + mo->name + "]";
        if (!warning.opt_key.empty()) hypertext += std::string(" (") + warning.opt_key + ")";

        // BBS disable support enforcer
        //if (text == "_SUPPORTS_OFF") {
        //    text = _u8L("An object has custom support enforcers which will not be used "
        //                "because supports are disabled.")+"\n";
        //    hypertext = _u8L("Enable supports for enforcers only");

        //    action_fn = [](wxEvtHandler*) {
        //        Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        //        assert(print_tab);
        //        DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        //        config.set_key_value("enable_support", new ConfigOptionBool(true));
        //        config.set_key_value("auto_support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        //        print_tab->on_value_change("enable_support", config.opt_bool("enable_support"));
        //        print_tab->on_value_change("support_material_auto", config.opt_bool("support_material_auto"));
        //        return true;
        //    };
        //}

        m_notification_manager->push_notification(
            NotificationType::ValidateWarning,
            NotificationManager::NotificationLevel::WarningNotificationLevel,
            _u8L("WARNING:") + "\n" + text, hypertext, action_fn
        );
    }
}

// 原 Plater::priv::actualize_slicing_warnings（Plater.cpp:5951-5961）。
void NotificationHub::actualize_slicing_warnings(const PrintBase& print)
{
    std::vector<ObjectID> ids = print.print_object_ids();
    if (ids.empty()) {
        clear_warnings();
        return;
    }
    ids.emplace_back(print.id());
    std::sort(ids.begin(), ids.end());
    m_notification_manager->remove_slicing_warnings_of_released_objects(ids);
    m_notification_manager->set_all_slicing_warnings_gray(true);
}

// 原 Plater::priv::actualize_object_warnings（Plater.cpp:5963-5971）。
void NotificationHub::actualize_object_warnings(const PrintBase& print)
{
    std::vector<ObjectID> ids;
    for (const ModelObject* object : print.model().objects )
    {
        ids.push_back(object->id());
    }
    std::sort(ids.begin(), ids.end());
    m_notification_manager->remove_simplify_suggestion_of_released_objects(ids);
}

// 原 Plater::priv::warnings_dialog（Plater.cpp:5978-5995）——MessageDialog 薄包装，文本组装走 build_warnings_text()。
bool NotificationHub::warnings_dialog()
{
    if (m_warnings.empty())
        return true;
    std::string text = build_warnings_text();
    //text += "\n\nDo you still wish to export?";
    MessageDialog msg_window(m_q, from_u8(text), _L("warnings"), wxOK);
    const auto    res = msg_window.ShowModal();
    return res == wxID_OK;

}

// 原 Plater::priv::init_notification_manager（Plater.cpp:7146-7159）——cancel_callback 走 Host slicing_cancel()。
void NotificationHub::init()
{
    if (!m_notification_manager)
        return;
    m_notification_manager->init();

    auto cancel_callback = [this]() { return slicing_cancel(); };
    m_notification_manager->init_slicing_progress_notification(cancel_callback);
    m_notification_manager->set_fff(m_session->printer_technology == ptFFF);
    m_notification_manager->init_progress_indicator();
}

// 原 Plater::priv::statusbar（Plater.cpp:2188-2191）—— stub 原样（手册 TL;DR #6）。
std::shared_ptr<BBLStatusBar> NotificationHub::statusbar()
{
    return nullptr;
}

// 原 Plater::get_dailytips（Plater.cpp:14887-14891）——静态单例写法逐字保留（陷阱 #12）。
DailyTipsWindow* NotificationHub::get_dailytips() const
{
    static DailyTipsWindow* dailytips_win = new DailyTipsWindow();
    return dailytips_win;
}

// 原 Plater::show_status_message（Plater.cpp:14903-14906）——仅 trace 日志，原样随迁。
void NotificationHub::show_status_message(std::string s)
{
    BOOST_LOG_TRIVIAL(trace) << "show_status_message:" << s;
}

// 原 Plater::notify_vhl_dithering_conflict（Plater.cpp:7664-7689）——view3D/model 探测与弹窗父窗口走 Host。
void NotificationHub::notify_vhl_dithering_conflict(bool local_z_enabled)
{
    if (!local_z_enabled)
        return;

    bool has_adaptive = m_layout->view3D->is_layers_editing_enabled();
    if (!has_adaptive) {
        for (const auto* obj : m_model.objects) {
            if (!obj->layer_height_profile.empty()) {
                has_adaptive = true;
                break;
            }
        }
    }

    if (!has_adaptive)
        return;

    MessageDialog dialog(m_q,
        _L("Cannot enable both Variable Layer Height and Subdivide Mix Layer."),
        _L("Warning"), wxICON_WARNING | wxOK);
    dialog.ShowModal();
}

// 原 Plater::update_all_plate_thumbnails（Plater.cpp:9541-9554）。
void NotificationHub::update_all_plate_thumbnails(bool force_update)
{
    m_q->get_view3D_canvas3D()->make_current_for_postinit();
    bool rendered = false;
    for (int i = 0; i < m_partplate_list.get_plate_count(); i++) {
        PartPlate* plate = m_partplate_list.get_plate(i);
        ThumbnailsParams thumbnail_params = { {}, false, true, true, true, i};
        if (force_update || !plate->thumbnail_data.is_valid()) {
            m_q->get_view3D_canvas3D()->render_thumbnail(plate->thumbnail_data, plate->plate_thumbnail_width, plate->plate_thumbnail_height, thumbnail_params, Camera::EType::Ortho);
            rendered = true;
        }
        if (force_update || !plate->no_light_thumbnail_data.is_valid()) {
            m_q->get_view3D_canvas3D()->render_thumbnail(plate->no_light_thumbnail_data, plate->plate_thumbnail_width, plate->plate_thumbnail_height, thumbnail_params,
                                                    Camera::EType::Ortho,false,false,true);
        }
    }
    if (rendered)
        m_q->get_preview_canvas3D()->invalidate_select_plate_toolbar();
}

// 原 Plater::invalid_all_plate_thumbnails（Plater.cpp:9557-9567）——门控成员留守 Plater，经 Host 读取。
void NotificationHub::invalid_all_plate_thumbnails()
{
    if (m_session->using_exported_file() || m_refresh->skip_thumbnail_invalid())
        return;
    BOOST_LOG_TRIVIAL(info) << "thumb: invalid all";
    for (int i = 0; i < m_partplate_list.get_plate_count(); i++) {
        PartPlate* plate = m_partplate_list.get_plate(i);
        plate->thumbnail_data.reset();
        plate->no_light_thumbnail_data.reset();
    }
}

// 原 Plater::force_update_all_plate_thumbnails（Plater.cpp:9569-9577）。
void NotificationHub::force_update_all_plate_thumbnails()
{
    if (m_session->using_exported_file() || m_refresh->skip_thumbnail_invalid()) {
    }
    else {
        invalid_all_plate_thumbnails();
        update_all_plate_thumbnails(true);
    }
    m_q->get_preview_canvas3D()->update_plate_thumbnails();
}

// 原 Plater::priv::warnings_dialog 的文本组装段（Plater.cpp:5979-5988）逐字拆出（无 wx，进单测）。
std::string NotificationHub::build_warnings_text() const
{
    std::string text = _u8L("There are warnings after slicing models:") + "\n";
    for (auto const& it : m_warnings) {
        size_t next_n = it.first.message.find_first_of('\n', 0);
        text += "\n";
        if (next_n != std::string::npos)
            text += it.first.message.substr(0, next_n);
        else
            text += it.first.message;
    }
    return text;
}

// 阶段 I：上游 2.4.1 新增——按当前盘生效打印序列同步黄色告警（原 Plater 门面实现迁入）
void NotificationHub::sync_print_seq_warning_notification()
{
    NotificationManager* notify_manager = m_notification_manager.get();
    if (notify_manager == nullptr)
        return;

    // Suppress during startup / preset loading, before the 3D view is live.
    GLCanvas3D* view3d_canvas = m_q->get_view3D_canvas3D();
    if (view3d_canvas == nullptr || !view3d_canvas->is_initialized() || !view3d_canvas->is_rendering_enabled()) {
        notify_manager->bbl_close_seqprintinfo_notification();
        return;
    }

    // Effective sequence: an explicit per-plate value overrides the global
    // one (PartPlate::get_real_print_seq falls back to global on ByDefault).
    PartPlate* curr_plate = m_partplate_list.get_curr_plate();
    const bool by_object = curr_plate != nullptr &&
        curr_plate->get_real_print_seq() == PrintSequence::ByObject;

    if (by_object) {
        std::string info_text = _u8L("Warning:") + "\n" +
            _u8L("Printing by object with caution. This function may cause the print head to collide with printed parts during switching.");
        notify_manager->bbl_show_seqprintinfo_notification(info_text);
    } else {
        notify_manager->bbl_close_seqprintinfo_notification();
    }
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
