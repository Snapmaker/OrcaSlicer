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

    svc.suppress_snapshots();
    svc.enter_gizmos_stack();                     // 切到 gizmos 栈
    REQUIRE(&svc.undo_redo_stack() != &svc.undo_redo_stack_main());
    svc.allow_snapshots();

    REQUIRE_FALSE(svc.leave_gizmos_stack());
    REQUIRE(&svc.undo_redo_stack() == &svc.undo_redo_stack_main());
}


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

    hub.add_warning(make_warning("first", 7), 0);
    hub.add_warning(make_warning("second", 7), 0);
    std::string text = hub.build_warnings_text();
    REQUIRE(text.find("second") != std::string::npos);
    REQUIRE(text.find("first") == std::string::npos);

    hub.clear_warnings();

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

    const std::string empty_text = hub.build_warnings_text();
    REQUIRE(empty_text.find('\n') != std::string::npos);
    REQUIRE(empty_text.size() < 80);            // 只有一行表头，无消息体

    hub.add_warning(make_warning("line1\nline2-should-be-dropped", 1), 0);
    const std::string text = hub.build_warnings_text();
    REQUIRE(text.find("line1") != std::string::npos);
    REQUIRE(text.find("line2-should-be-dropped") == std::string::npos);
    REQUIRE(text.find("line1") > text.find('\n'));
}
