#pragma once

#include <memory>
#include <string>
#include <vector>

#include "libslic3r/PrintBase.hpp"    // PrintStateBase::Warning / PrintBase

class wxWindow;                       // 全局命名空间前置声明（父窗口经 Host 注入）

namespace Slic3r {
class Model;
class ModelObject;
class StringObjectException;
class BBLStatusBar;                   // 定义在 Slic3r（非 Slic3r::GUI）
namespace GUI {
class NotificationManager;
class GLCanvas3D;
class View3D;
class DailyTipsWindow;
class Plater;                         // 仅作 ctor 直传（NotificationManager 构造用）
class PartPlateList;

namespace app {
class LayoutManager;
class ProjectSession;
class SceneRefreshCoordinator;
class SlicingController;


class NotificationHub
{
public:
    explicit NotificationHub(Plater* q, Model& model, PartPlateList& partplate_list);
    ~NotificationHub();                               // unique_ptr<NotificationManager> 需完整类型
    void late_bind(SlicingController& slicing, ProjectSession& session,
                   LayoutManager& layout, SceneRefreshCoordinator& refresh);   // 阶段 H1 2.1：声明序晚于本服务，plater_init 内补注

    NotificationManager* nm() const { return m_notification_manager.get(); }   // 原 priv 成员访问器
    bool slicing_cancel();                                     // 阶段 H1 2.1：原 Host 槽（多语句体）私有化

    void init();                                      // 原 init_notification_manager
    void add_warning(const PrintStateBase::Warning& warning, size_t oid);      // 无 wx，单测
    void clear_warnings();                                                     // 无 wx，单测
    std::string build_warnings_text() const;          // ← 原 :5977-5985 逐字拆出，无 wx，单测
    bool warnings_dialog();                           // MessageDialog 薄包装
    void actualize_slicing_warnings(const PrintBase& print);
    void actualize_object_warnings(const PrintBase& print);
    void process_validation_warning(const StringObjectException& warning) const;
    void update_all_plate_thumbnails(bool force_update);
    void sync_print_seq_warning_notification();       // 阶段 I：按当前盘生效序列同步黄色告警
    void invalid_all_plate_thumbnails();
    void force_update_all_plate_thumbnails();
    void notify_vhl_dithering_conflict(bool local_z_enabled);
    void show_status_message(std::string s);
    DailyTipsWindow* get_dailytips() const;           // 静态单例写法逐字保留
    std::shared_ptr<BBLStatusBar> statusbar();        // stub 原样（返回 nullptr）

    bool    is_dark() const { return m_is_dark; }                              // ← :915
    void    set_is_dark(bool v) { m_is_dark = v; }
    size_t  last_auto_gradient_prompt_count() const { return m_last_auto_gradient_prompt_physical_count; }  // ← :916
    void    set_last_auto_gradient_prompt_count(size_t v) { m_last_auto_gradient_prompt_physical_count = v; }
    bool    last_auto_gradient_prompt_accepted() const { return m_last_auto_gradient_prompt_accepted; }     // ← :917
    void    set_last_auto_gradient_prompt_accepted(bool v) { m_last_auto_gradient_prompt_accepted = v; }

private:
    std::unique_ptr<NotificationManager> m_notification_manager;   // ← priv:871
    std::vector<std::pair<PrintStateBase::Warning, size_t>> m_warnings;  // ← priv:1320 current_warnings
    bool    m_is_dark = false;
    size_t  m_last_auto_gradient_prompt_physical_count = 0;
    bool    m_last_auto_gradient_prompt_accepted = false;

    Plater*             m_q{nullptr};
    Model&              m_model;
    PartPlateList&      m_partplate_list;
    SlicingController*          m_slicing{nullptr};   // late_bind
    ProjectSession*             m_session{nullptr};   // late_bind
    LayoutManager*              m_layout{nullptr};    // late_bind
    SceneRefreshCoordinator*    m_refresh{nullptr};   // late_bind
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
