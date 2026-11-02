// SnapshotService + NotificationHub 单测（阶段 C，手册 §3.5 / §5.1）：无 wx 核心，Catch2 直构服务。
// H1 2.3 typed 化改造：SnapshotHost/空 Host 替身随 Host 消亡移除——svc ctor 改
// (Plater* q=nullptr, Model&, PartPlateList&)；q 桥面（single_blocks→Plater::single_blocks）
// 在无头态不可触达，触及即 AV——本文件用例全部避开（抑制期早退先于 q 桥；gizmos 栈用例不触发快照）。
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "slic3r/GUI/app/SnapshotService.hpp"
#include "slic3r/GUI/app/NotificationHub.hpp"
#include "slic3r/GUI/PartPlate.hpp"           // PartPlateList 无头构造（nullptr plater）

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;

TEST_CASE("suppress counter blocks take_snapshot before any side effect", "[SnapshotService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService svc(nullptr, model, ppl);

    // 抑制期零副作用：take_snapshot 在 m_prevent>0 早退（typed 化后先于任何成员触达）
    svc.suppress_snapshots();
    svc.take_snapshot("x");                       // 应在 m_prevent>0 早退
    REQUIRE(svc.inside_snapshot_capture());
    svc.allow_snapshots();
    REQUIRE_FALSE(svc.inside_snapshot_capture());
}

TEST_CASE("gizmos stack switching keeps separate stacks", "[SnapshotService]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    SnapshotService svc(nullptr, model, ppl);

    // 抑制期间进入 gizmos 栈：初始快照（"Gizmos-Initial"）在 m_prevent>0 早退，gizmos 栈保持空
    svc.suppress_snapshots();
    svc.enter_gizmos_stack();                     // 切到 gizmos 栈
    REQUIRE(&svc.undo_redo_stack() != &svc.undo_redo_stack_main());
    svc.allow_snapshots();

    // 空栈离开：changed=false（原 Plater.cpp:7752-7763 语义），并回到主栈
    REQUIRE_FALSE(svc.leave_gizmos_stack());
    REQUIRE(&svc.undo_redo_stack() == &svc.undo_redo_stack_main());
}

// —— NotificationHub：警告聚合无 wx 核心（手册 §5.1，2 例）——

static PrintStateBase::Warning make_warning(std::string msg, int id)
{
    PrintStateBase::Warning w;
    w.level      = PrintStateBase::WarningLevel::NON_CRITICAL;
    w.current    = true;
    w.message    = std::move(msg);
    w.message_id = id;
    return w;
}

TEST_CASE("add_warning dedups by message_id (and by message when id==0)", "[NotificationHub]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    NotificationHub hub(nullptr, model, ppl);

    // message_id != 0：同 id 去重，且覆盖为最新 message（原 :5937-5949 语义）
    hub.add_warning(make_warning("first", 7), 0);
    hub.add_warning(make_warning("second", 7), 0);
    std::string text = hub.build_warnings_text();
    REQUIRE(text.find("second") != std::string::npos);
    REQUIRE(text.find("first") == std::string::npos);

    hub.clear_warnings();

    // message_id == 0：仅当 message 完全相同才去重
    hub.add_warning(make_warning("dup", 0), 0);
    hub.add_warning(make_warning("dup", 0), 0);
    hub.add_warning(make_warning("other", 0), 0);
    text = hub.build_warnings_text();
    const size_t first_pos  = text.find("dup");
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(text.find("dup", first_pos + 1) == std::string::npos);   // 只出现一次
    REQUIRE(text.find("other") != std::string::npos);
}

TEST_CASE("build_warnings_text keeps header and first line of each message", "[NotificationHub]")
{
    Model model;
    PartPlateList ppl(nullptr, &model);
    NotificationHub hub(nullptr, model, ppl);

    // 空集：仅表头（原 warnings_dialog 在空集时提前 return，组装式样保持一致）
    const std::string empty_text = hub.build_warnings_text();
    REQUIRE(empty_text.find('\n') != std::string::npos);
    REQUIRE(empty_text.size() < 80);            // 只有一行表头，无消息体

    // 多行消息只取首行（原 :5980-5986 的 find_first_of('\n') 分支）
    hub.add_warning(make_warning("line1\nline2-should-be-dropped", 1), 0);
    const std::string text = hub.build_warnings_text();
    REQUIRE(text.find("line1") != std::string::npos);
    REQUIRE(text.find("line2-should-be-dropped") == std::string::npos);
    // 表头 + 空行 + 首行（原组装顺序：header "\n" → 每条前 "\n"）
    REQUIRE(text.find("line1") > text.find('\n'));
}
