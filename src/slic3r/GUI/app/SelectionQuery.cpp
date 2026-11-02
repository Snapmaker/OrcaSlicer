// SelectionQuery 实现（阶段 F1，手册 §2.1/§3）：选中状态查询七函数逐字搬移自 Plater::priv（get_selection×2/get_curr_selection/get_selected_object_idx/get_selected_volume_idx，2026-10-19 基线 :2572-2612）与门面（is_selection_empty/is_single_full_object_selection）。
// 依赖规则：禁 include Plater/GUI_App 宿主头；GLCanvas3D/Selection/GLVolume 完整类型仅在本文件使用。
#include "SelectionQuery.hpp"

#include "../GLCanvas3D.hpp"
#include "../Plater.hpp"    // 阶段 H1 2.1：Host 消亡后 m_q 门面直调需完整类型（hpp 仍零宿主头）   // GLCanvas3D::get_selection()（槽位返回的画布上取选中）
#include "../3DScene.hpp"     // GLVolume::volume_idx()（get_selected_volume_idx 尾段；GLVolume 定义于此，非独立头）
#include "../Selection.hpp"    // Selection 完整类型（is_empty/is_wipe_tower/get_object_idx/get_first_volume/is_single_full_object）
#include "libslic3r/Model.hpp" // Model 完整类型（get_selected_volume_idx 读 objects[idx]->volumes；构造注入 Model*）

namespace Slic3r {
namespace GUI {
namespace app {

SelectionQuery::SelectionQuery(Plater* q, Model* model)
    : m_q(q), m_model(model)
{
}

// —— 固定 view3D 画布语义（priv::get_selection 双重载逐字；槽位 = [q] q->get_view3D_canvas3D()）——

const Selection& SelectionQuery::get_selection() const
{
    GLCanvas3D* canvas = m_q->get_view3D_canvas3D();
    if (!canvas) {
        // 与 get_curr_selection 同款析构期防御（生产期 canvas 恒非空；无头单测经此驱动谓词语义）
        static Selection empty_selection;
        return empty_selection;
    }
    return canvas->get_selection();
}

Selection& SelectionQuery::selection()
{
    return m_q->get_view3D_canvas3D()->get_selection();
}

// —— 当前画布语义（priv::get_curr_selection 逐字；析构期 static 空 Selection 回退，引用稳定）——

const Selection& SelectionQuery::get_curr_selection() const
{
    GLCanvas3D* canvas = m_q ? m_q->get_current_canvas3D(false) : nullptr;   // q 可空：无头单测缝（生产期 q 恒非空）
    if (!canvas) {
        // During destruction, return a reference to a static empty selection
        static Selection empty_selection;
        return empty_selection;
    }
    return canvas->get_selection();
}

// 裁剪魔数纯函数（原 priv::get_selected_object_idx 表达式逐字；公开 static 供单测，E2 join_gcode_filename D7 先例）
int SelectionQuery::trimmed_object_idx(int idx)
{
    return ((0 <= idx) && (idx < 1000)) ? idx : -1;
}

int SelectionQuery::get_selected_object_idx() const
{
    int idx = get_selection().get_object_idx();
    return trimmed_object_idx(idx);
}

// idx>1000 不对称边界逐字保留（与 get_selected_object_idx 的 <1000 是历史语义，禁止"顺手统一"）；
// model.objects[idx] → m_model->objects[idx]（留守成员注入引用，E1 勘误 1 模式）
int SelectionQuery::get_selected_volume_idx() const
{
    auto& selection = get_selection();
    int idx = selection.get_object_idx();
    if ((0 > idx) || (idx > 1000))
        return-1;
    const GLVolume* v = selection.get_first_volume();
    if (m_model->objects[idx]->volumes.size() > 1)
        return v->volume_idx();
    return -1;
}

bool SelectionQuery::is_selection_empty() const
{
    return get_selection().is_empty() || get_selection().is_wipe_tower();
}

bool SelectionQuery::is_single_full_object_selection() const
{
    return get_selection().is_single_full_object();
}

} // namespace app
} // namespace GUI
} // namespace Slic3r
