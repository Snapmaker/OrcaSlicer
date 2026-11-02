// MixedFilamentsPanel — Sidebar 混色面板壳类化（阶段 B2，手册《Plater阶段B2-MixedFilamentsPanel类化实施方案》）。
// 自 Sidebar::priv / Sidebar.cpp 整体迁入：混色标题栏构造块 + 4 个混色方法 + 22 个成员 + MixedFilamentRowBinding。
// 依赖规则（B1 护栏延续）：本 hpp 禁 include Sidebar.hpp / Plater.hpp / GUI_App.hpp——宿主能力经 MixedFilamentsPanelHost 回调。
// 迁移对账：方法体与源逐字节一致（除宿主解引用 / this 前缀 / FromDIP 所有者前缀）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include <wx/colour.h>
#include <wx/tracker.h>      // wxTrackable（wxWeakRef 守卫基类；定义在 tracker.h）

class wxWindow;
class wxSizer;
class wxBoxSizer;
class wxPanel;
class wxScrolledWindow;
class wxStaticText;
class StaticBox;
class ScalableButton;
class Button;

namespace Slic3r {
namespace GUI {

// 宿主能力（Sidebar 装配一次；全部 lambda 捕获 [this] 指向 Sidebar）。
// Plater 操作经回调进入——保持本文件零 Plater.hpp 依赖。
struct MixedFilamentsPanelHost {
    // —— Sidebar ——
    std::function<size_t()>                       filament_count;                        // p->combos_filament.size()
    std::function<void(bool)>                     set_add_filament_enabled;              // p->m_bpButton_add_filament->Enable（含空判）
    std::function<void()>                         layout_scrolled;                       // Sidebar::m_scrolled_sizer->Layout()
    std::function<void()>                         layout_owner;                          // Sidebar::Layout()
    std::function<void(size_t, size_t)>           change_filament;                       // Sidebar::change_filament
    std::function<void()>                         set_skip_mixed_filament_sync_once;     // Sidebar::priv 共享去抖旗标（读/清在 Sidebar::on_filaments_change）
    // —— Plater ——
    std::function<void()>                         plater_post_slice_state_change_update; // Plater::post_slice_state_change_update
    std::function<void(size_t)>                   plater_on_filaments_change;            // Plater::on_filaments_change
    std::function<void(size_t, size_t, int, const std::vector<unsigned char>&)>
                                                  plater_on_filaments_delete;            // Plater::on_filaments_delete
    std::function<void()>                         plater_update;                         // Plater::update
    std::function<void()>                         plater_update_project_dirty_from_presets;  // Plater::update_project_dirty_from_presets
    std::function<void()>                         refresh_model_canvas_colors;           // 画布体积色/重绘编排
    std::function<wxColour()>                     next_color_for_filament;               // Plater::get_next_color_for_filament（static）
};

class MixedFilamentsPanel : public wxTrackable   // 原 wxWeakRef<Sidebar>(this) 守卫的等价基类
{
public:
    MixedFilamentsPanel(wxWindow* owner, MixedFilamentsPanelHost host);

    // 原 Sidebar 构造器内「Mixed Filaments Panel (Collapsible)」块（含 Add Gradient/Pattern/Color 按钮与折叠绑定）
    void init_mixed_filaments_panel(wxWindow* scrolled, wxBoxSizer* scrolled_sizer, const wxColour& title_bg);
    // 原 Sidebar 四方法（签名不变）
    void init_color_mix_panel(wxWindow* parent, wxSizer* sizer);
    void update_color_mix_panel();
    void update_mixed_filament_panel(bool sync_manager = true);
    void merge_mixed_filament(size_t from_id, size_t to_id, bool skip_update = false, bool skip_serialize = false);

    // Sidebar::get_ui_ordered_filament_ids 只读消费（原 Sidebar::priv 共享成员）
    const std::vector<uint64_t>& mixed_filament_ui_order() const { return m_mixed_filament_ui_order; }
    // Sidebar::msw_rescale / sys_color_changed 的混色控件刷新（原 Sidebar::priv 直接访问）
    void msw_rescale();
    void set_add_color_mix_enabled(bool enabled);

private:
    void layout_scrolled() { m_host.layout_scrolled(); }   // CallAfter 自引用复用

    wxWindow*               m_owner{nullptr};
    MixedFilamentsPanelHost m_host;

    // —— 自 Sidebar::priv 迁入（原序保留；前 16 混色面板壳，后 6 色混块）——
    StaticBox*          m_panel_mixed_filaments_title = nullptr;    // Collapsible title bar
    wxPanel*            m_panel_mixed_filaments_content = nullptr; // Content panel
    wxBoxSizer*         m_sizer_mixed_filaments_content = nullptr; // Content sizer
    ScalableButton*     m_mixed_filaments_icon = nullptr;          // Icon
    wxStaticText*       m_staticText_mixed_filaments = nullptr;    // Title text
    Button*             m_btn_add_gradient = nullptr;              // Add gradient button
    Button*             m_btn_add_pattern = nullptr;               // Add pattern button
    Button*             m_btn_add_color = nullptr;                 // Add color-match button
    Button*             m_btn_toggle_mixed_filaments = nullptr;   // Collapse/expand toggle button
    bool                m_mixed_filaments_collapsed = false;      // Collapse state
    std::unordered_set<size_t> m_expanded_mixed_filament_rows;    // Expanded row editors
    struct MixedFilamentRowBinding {
        size_t    mixed_id = size_t(-1);
        wxWindow *row      = nullptr;
    };
    std::vector<MixedFilamentRowBinding> m_mixed_filament_row_bindings;
    std::vector<uint64_t>                m_mixed_filament_ui_order;
    bool                                 m_mixed_filament_drag_active = false;
    size_t                               m_mixed_filament_drag_source_mixed_id = size_t(-1);
    StaticBox*      m_panel_color_mix_title   = nullptr;
    wxPanel*        m_panel_color_mix_content = nullptr;
    wxScrolledWindow* m_scrolled_color_mix    = nullptr;
    ScalableButton* m_color_mix_icon          = nullptr;
    ScalableButton* m_btn_add_color_mix       = nullptr;
    ScalableButton* m_btn_del_color_mix       = nullptr;
};

} // namespace GUI
} // namespace Slic3r
