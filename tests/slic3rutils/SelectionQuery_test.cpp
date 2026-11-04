#include "slic3r/GUI/app/SelectionQuery.hpp"
#include "slic3r/GUI/Selection.hpp"    // Selection 完整类型（is_empty；无宿主头穿透，F1 骨架期已核）
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;


TEST_CASE("SelectionQuery get_curr_selection: null q falls back to static empty", "[SelectionQuery]")
{
    Model model;
    SelectionQuery sq(nullptr, &model);

    const Selection& s1 = sq.get_curr_selection();
    const Selection& s2 = sq.get_curr_selection();
    REQUIRE(s1.is_empty());
    REQUIRE(&s1 == &s2);   // 引用稳定：同一 static 实例（原实现注释 "During destruction..."）
}



TEST_CASE("trimmed_object_idx: idx<1000 boundary four points (999/1000/1001/-1)", "[SelectionQuery]")
{
    REQUIRE(SelectionQuery::trimmed_object_idx(999) == 999);
    REQUIRE(SelectionQuery::trimmed_object_idx(1000) == -1);   // object_idx 语义：1000 被裁掉
    REQUIRE(SelectionQuery::trimmed_object_idx(1001) == -1);
    REQUIRE(SelectionQuery::trimmed_object_idx(-1) == -1);
}

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
