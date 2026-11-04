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


const Selection& SelectionQuery::get_selection() const
{
    GLCanvas3D* canvas = m_q->get_view3D_canvas3D();
    if (!canvas) {
        static Selection empty_selection;
        return empty_selection;
    }
    return canvas->get_selection();
}

Selection& SelectionQuery::selection()
{
    return m_q->get_view3D_canvas3D()->get_selection();
}


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

int SelectionQuery::trimmed_object_idx(int idx)
{
    return ((0 <= idx) && (idx < 1000)) ? idx : -1;
}

int SelectionQuery::get_selected_object_idx() const
{
    int idx = get_selection().get_object_idx();
    return trimmed_object_idx(idx);
}

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
