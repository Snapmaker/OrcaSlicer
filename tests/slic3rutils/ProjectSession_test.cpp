#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/app/ProjectSession.hpp"
#include "TypedChain.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::app;


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

    s.m_saved_timestamp = tc.snapshots.undo_redo_stack_main().active_snapshot_time();
    REQUIRE(s.up_to_date(/*saved=*/false, /*backup=*/false));
    s.m_saved_timestamp = 987654321;
    REQUIRE_FALSE(s.up_to_date(/*saved=*/false, /*backup=*/false));
}


TEST_CASE("has_illegal_filename_characters flags every illegal character in both overloads", "[ProjectSession]")
{
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

TEST_CASE("save-undo-resave chain keeps timestamp channels consistent", "[ProjectSession]")
{
    FakeWorker worker;
    TypedChain tc(worker);
    auto& s = tc.session;

    REQUIRE(s.up_to_date(true, false));
    REQUIRE(s.up_to_date(true, true));
    REQUIRE(s.m_saved_timestamp == s.m_backup_timestamp);

    s.m_saved_timestamp = 1;   // 模拟撤销使活动时间前移到 0、saved 记录的 1 已失效
    REQUIRE_FALSE(s.up_to_date(false, false));   // 标题脏标记应回归

    REQUIRE(s.up_to_date(true, false));
    REQUIRE(s.up_to_date(true, true));
    REQUIRE(s.m_saved_timestamp == tc.snapshots.undo_redo_stack_main().active_snapshot_time());
    REQUIRE(s.up_to_date(false, false));         // 净
    REQUIRE(s.is_presets_dirty() == false);      // 预设通道独立不误报
}
