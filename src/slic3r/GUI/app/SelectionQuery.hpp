#pragma once

namespace Slic3r {
class Model;
namespace GUI {
class Plater;
class GLCanvas3D;
class Selection;

namespace app {


class SelectionQuery {
public:
    SelectionQuery(Plater* q, Model* model);

    const Selection& get_selection() const;
    Selection&       selection();                  // 非 const 出口（export_send_host.selection 槽消费；命令群选中重建写路径经此）
    const Selection& get_curr_selection() const;

    int  get_selected_object_idx() const;          // idx<1000 裁剪魔数逐字保留（与 get_selected_volume_idx 的 >1000 不对称是历史语义，禁止"顺手统一"）
    int  get_selected_volume_idx() const;          // idx>1000 不对称边界逐字保留；多 volume 才返回 volume_idx
    bool is_selection_empty() const;               // is_empty() || is_wipe_tower()
    bool is_single_full_object_selection() const;

    static int trimmed_object_idx(int idx);

private:
    Plater* m_q{nullptr};   // 阶段 H1 2.1：两 canvas 槽改 m_q 门面直调
    Model*  m_model{nullptr};
};

} // namespace app
} // namespace GUI
} // namespace Slic3r
