// SelectionQuery — Plater 选中状态查询服务（阶段 F1 纯搬移，手册《Plater阶段F1-对象编辑ObjectEditCommands与SelectionQuery抽取实施方案》）。
// hpp：wx 零依赖；依赖规则（AGENTS.md）：本目录禁止 include Plater/GUI_App 宿主头，
// q 与 priv 上下文只能经 Host 进入；Selection/GLCanvas3D 仅以指针/引用出现（前置声明，完整类型仅 cpp 使用）。
#pragma once

namespace Slic3r {
class Model;
namespace GUI {
class Plater;
class GLCanvas3D;
class Selection;

namespace app {

// 阶段 H1 2.1：SelectionQueryHost 消亡（两槽 [q] 门面直调，typed 化 = m_q 直取）。

// 选中状态唯一查询/操纵入口（只读查询 + 显式非 const 出口）；选中写操作本体仍在 canvas/视图。
class SelectionQuery {
public:
    SelectionQuery(Plater* q, Model* model);

    // —— 固定 view3D 画布语义（priv::get_selection 双重载逐字搬移）——
    const Selection& get_selection() const;
    Selection&       selection();                  // 非 const 出口（export_send_host.selection 槽消费；命令群选中重建写路径经此）
    // —— 当前画布语义（priv::get_curr_selection 逐字；canvas 为空返回服务内 static 空实例——析构期回退，引用稳定）——
    const Selection& get_curr_selection() const;

    int  get_selected_object_idx() const;          // idx<1000 裁剪魔数逐字保留（与 get_selected_volume_idx 的 >1000 不对称是历史语义，禁止"顺手统一"）
    int  get_selected_volume_idx() const;          // idx>1000 不对称边界逐字保留；多 volume 才返回 volume_idx
    bool is_selection_empty() const;               // is_empty() || is_wipe_tower()
    bool is_single_full_object_selection() const;

    // 裁剪魔数纯函数（原 priv::get_selected_object_idx 表达式逐字；公开 static 供单测，E2 join_gcode_filename D7 先例）
    static int trimmed_object_idx(int idx);

private:
    Plater* m_q{nullptr};   // 阶段 H1 2.1：两 canvas 槽改 m_q 门面直调
    Model*  m_model{nullptr};
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
