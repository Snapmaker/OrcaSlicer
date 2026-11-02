// ProjectSession 单测（阶段 E1，手册 §2 / §4 步 2-3）：无 wx 对话框核心，Catch2 + TypedChain 直构。
// up_to_date 用例基于空撤销栈：StackImpl 初始 m_active_snapshot_time==0（active_snapshot_time()
// 合法返回 0，不触 take_snapshot——其需 GLGizmosManager/GLCanvas3D，无法无头构造）。
// H1 2.3 typed 化改造：ProjectSessionHost 随 Host 消亡移除——TypedChain 无头闭包直构 tc.session。
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/app/ProjectSession.hpp"
#include "TypedChain.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

// —— 2.1 up_to_date 时间戳判定（手册 §2.1 表 7；原 priv::up_to_date :6953-6965）——

TEST_CASE("up_to_date(saved) writes active snapshot time into saved channel", "[ProjectSession]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    auto& s = tc.session;

    s.m_saved_timestamp = 12345;   // 陈旧值，必须被覆写
    REQUIRE(s.up_to_date(/*saved=*/true, /*backup=*/false));
    REQUIRE(s.m_saved_timestamp == tc.snapshots.undo_redo_stack_main().active_snapshot_time());
}

TEST_CASE("up_to_date(backup) writes backup channel only, leaves saved channel untouched", "[ProjectSession]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    auto& s = tc.session;

    s.m_saved_timestamp  = 444;
    s.m_backup_timestamp = 555;
    REQUIRE(s.up_to_date(/*saved=*/true, /*backup=*/true));
    REQUIRE(s.m_backup_timestamp == tc.snapshots.undo_redo_stack_main().active_snapshot_time());
    REQUIRE(s.m_saved_timestamp == 444);   // backup 通道独立：saved 不被触碰
}

TEST_CASE("up_to_date(dirty check) via has_real_change_from judges clean and stale", "[ProjectSession]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    auto& s = tc.session;

    // 时间戳与活动快照时间一致 → 净
    s.m_saved_timestamp = tc.snapshots.undo_redo_stack_main().active_snapshot_time();
    REQUIRE(s.up_to_date(/*saved=*/false, /*backup=*/false));
    // 时间戳为陈旧值（栈中无此前时间）→ 脏
    s.m_saved_timestamp = 987654321;
    REQUIRE_FALSE(s.up_to_date(/*saved=*/false, /*backup=*/false));
}

// —— 2.2 has_illegal_filename_characters 两 overload（原 Plater 静态组 :609/:615）——

TEST_CASE("has_illegal_filename_characters flags every illegal character in both overloads", "[ProjectSession]")
{
    // 非法字符集逐字对照原实现：<>:/\|?*"
    const std::string illegal = "<>:/\\|?*\"";
    for (char c : illegal) {
        std::string bad = std::string("bad") + c + "name";
        CAPTURE(bad);
        REQUIRE(ProjectSession::has_illegal_filename_characters(bad));
        REQUIRE(ProjectSession::has_illegal_filename_characters(wxString(bad.c_str())));
    }
}

TEST_CASE("has_illegal_filename_characters accepts clean names and empty string", "[ProjectSession]")
{
    REQUIRE_FALSE(ProjectSession::has_illegal_filename_characters(std::string("good_name-1.2")));
    REQUIRE_FALSE(ProjectSession::has_illegal_filename_characters(wxString("good_name-1.2")));
    REQUIRE_FALSE(ProjectSession::has_illegal_filename_characters(std::string("")));
    REQUIRE_FALSE(ProjectSession::has_illegal_filename_characters(wxString(wxEmptyString)));
}

// —— 5.1 脏状态 × 撤销栈交互专项（E 阶段退出准则：保存→撤销→标题脏标记→再保存链）——
// 说明：真实 take_snapshot 需 GLGizmosManager/GLCanvas3D（无头不可构造），
// 本组以时间戳通道编排钉住保存/撤销/再保存的判定链语义；完整 GUI 链由手测 ② 钉住。
TEST_CASE("save-undo-resave chain keeps timestamp channels consistent", "[ProjectSession]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    auto& s = tc.session;

    // 保存：saved 通道捕获活动快照时间（空栈=0），backup 通道同步
    REQUIRE(s.up_to_date(true, false));
    REQUIRE(s.up_to_date(true, true));
    REQUIRE(s.m_saved_timestamp == s.m_backup_timestamp);

    // 撤销后栈推进（时间戳落后）：脏标记回归
    s.m_saved_timestamp = 1;   // 模拟撤销使活动时间前移到 0、saved 记录的 1 已失效
    REQUIRE_FALSE(s.up_to_date(false, false));   // 标题脏标记应回归

    // 再保存：脏清除，双通道重新对齐
    REQUIRE(s.up_to_date(true, false));
    REQUIRE(s.up_to_date(true, true));
    REQUIRE(s.m_saved_timestamp == tc.snapshots.undo_redo_stack_main().active_snapshot_time());
    REQUIRE(s.up_to_date(false, false));         // 净
    REQUIRE(s.is_presets_dirty() == false);      // 预设通道独立不误报
}
