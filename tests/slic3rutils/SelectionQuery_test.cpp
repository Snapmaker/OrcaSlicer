// SelectionQuery 单测（阶段 F1，手册 §2.1 表 / tasks 2.1-2.2）：无 wx 对话框核心，Catch2 直构服务 + 假 Host 槽。
// TDD 红绿节奏：本文件先于服务实现落地——七函数在骨架期无定义，链接失败即红（tasks 2.1/2.2 验证，红相存证 audit/tdd_red.md）。
// 注：不构造 GLCanvas3D——其 ctor 尾段 load_arrange_settings() 直读 wxGetApp().app_config，无头进程必崩
// （audit/tdd_red.md 执行期修正 2）；分派方向以"槽位征询计数"负向钉住，正向（AssembleView 下跨视图）由手测 ⑧ 兜底。
#include "slic3r/GUI/app/SelectionQuery.hpp"
#include "slic3r/GUI/Selection.hpp"    // Selection 完整类型（is_empty；无宿主头穿透，F1 骨架期已核）
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

// —— 2.1 get_curr_selection 空画布回退（typed 化：q 可空缝下 canvas=nullptr → static 空实例；H1 2.3 改造）——

TEST_CASE("SelectionQuery get_curr_selection: null q falls back to static empty", "[SelectionQuery]")
{
    Model model;
    SelectionQuery sq(nullptr, &model);

    const Selection& s1 = sq.get_curr_selection();
    const Selection& s2 = sq.get_curr_selection();
    REQUIRE(s1.is_empty());
    REQUIRE(&s1 == &s2);   // 引用稳定：同一 static 实例（原实现注释 "During destruction..."）
}

// （原"分派方向负向钉"TEST_CASE 随 Host 消亡移除：typed 化后 current_canvas 读取为结构直连
//  m_q->get_current_canvas3D(false)，无槽可错接——H1 2.3，erratum §11 同批。）

// —— 2.2 裁剪魔数（static 纯函数，表达式逐字取自原 priv::get_selected_object_idx）——

TEST_CASE("trimmed_object_idx: idx<1000 boundary four points (999/1000/1001/-1)", "[SelectionQuery]")
{
    REQUIRE(SelectionQuery::trimmed_object_idx(999) == 999);
    REQUIRE(SelectionQuery::trimmed_object_idx(1000) == -1);   // object_idx 语义：1000 被裁掉
    REQUIRE(SelectionQuery::trimmed_object_idx(1001) == -1);
    REQUIRE(SelectionQuery::trimmed_object_idx(-1) == -1);
}

// get_selected_volume_idx 三分支判定镜像（原 priv 体逐字誊写；服务体保持逐字不动，镜像仅作语义钉子——
// 关键钉点：volume 侧 1000 通过 guard（与 object 侧不对称），多 volume 才返回 volume_idx。镜像漂移由
// 迁移 commit 的逐函数 diff 验证（tasks 5.5）兜底。）
static int selected_volume_idx_mirror(int raw_object_idx, size_t volume_count, int first_volume_idx)
{
    if ((0 > raw_object_idx) || (raw_object_idx > 1000))
        return -1;
    if (volume_count > 1)
        return first_volume_idx;
    return -1;
}

TEST_CASE("volume idx tri-branch: 1000 passes guard (asymmetric), multi-volume required", "[SelectionQuery]")
{
    REQUIRE(selected_volume_idx_mirror(999, 3, 7) == 7);     // 多 volume → volume_idx
    REQUIRE(selected_volume_idx_mirror(1000, 3, 7) == 7);    // 不对称：volume 侧 1000 通过（object 侧为 -1）
    REQUIRE(selected_volume_idx_mirror(1001, 3, 7) == -1);   // 越界
    REQUIRE(selected_volume_idx_mirror(-1, 3, 7) == -1);     // 负值
    REQUIRE(selected_volume_idx_mirror(0, 1, 7) == -1);      // 单 volume → -1
    REQUIRE(selected_volume_idx_mirror(0, 5, 2) == 2);       // 多 volume 正常返回
}
