#include <catch2/catch.hpp>

#include "slic3r/Utils/HubHandover.hpp"

using namespace Slic3r::HubHandover;

// The hub lifecycle rules (HubHandover.hpp): a slicer that finds a hub running from another install
// asks it to hand over, and a hub whose install is gone exits. Paths and states are faked.

static const char* const INSTALLED = "C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe";
static const char* const TEST_COPY = "C:\\Dev\\EdgeSlicerTest\\hubhandover\\EdgeSlicer.exe";

TEST_CASE("hub handover: paths compare after case and slash normalisation", "[HubHandover]")
{
    SECTION("slashes and case, on Windows")
    {
        CHECK(same_exe("C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", "c:/dev/edgeslicerbuilds/current/EdgeSlicer.EXE", true));
        CHECK(same_exe("C:/Dev//EdgeSlicerBuilds/./current/EdgeSlicer.exe", "C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", true));
        CHECK(same_exe("C:\\Dev\\x\\..\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", INSTALLED, true));
    }
    SECTION("the extended-length prefix is not part of the path")
    {
        CHECK(same_exe("\\\\?\\C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", INSTALLED, true));
        CHECK(same_exe("\\\\?\\UNC\\server\\share\\app\\EdgeSlicer.exe", "\\\\server\\share\\app\\EdgeSlicer.exe", true));
        CHECK(!same_exe("\\\\?\\UNC\\server\\share\\app\\EdgeSlicer.exe", "\\server\\share\\app\\EdgeSlicer.exe", true));
    }
    SECTION("case counts where the file system is case sensitive")
    {
        CHECK(!same_exe("/opt/EdgeSlicer/edgeslicer", "/opt/edgeslicer/edgeslicer", false));
        CHECK(same_exe("/opt/EdgeSlicer//bin/../edgeslicer/", "/opt/EdgeSlicer/edgeslicer", false));
    }
    SECTION("different files stay different")
    {
        CHECK(!same_exe(INSTALLED, TEST_COPY, true));
        CHECK(!same_exe("C:\\Dev\\a\\EdgeSlicer.exe", "C:\\Dev\\a\\EdgeSlicer2.exe", true));
        CHECK(!same_exe("D:\\Dev\\a\\EdgeSlicer.exe", "C:\\Dev\\a\\EdgeSlicer.exe", true));
        // ".." cannot climb above the root, so the two spellings below are the same place.
        CHECK(same_exe("C:\\..\\Dev\\a.exe", "C:\\Dev\\a.exe", true));
    }
    SECTION("an empty path is never the same as anything, itself included")
    {
        CHECK(!same_exe("", "", true));
        CHECK(!same_exe(INSTALLED, "", true));
        CHECK(normalize_path("", true).empty());
    }
}

TEST_CASE("hub handover: foreign vs same exe", "[HubHandover]")
{
    SECTION("the hub reports its exe")
    {
        CHECK(judge_exe(INSTALLED, INSTALLED, "", true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, "c:/dev/edgeslicerbuilds/CURRENT/edgeslicer.exe", "", true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, TEST_COPY, "", true) == ExeVerdict::Foreign);
        // What the hub says about itself wins; the pid's image is only the fallback.
        CHECK(judge_exe(INSTALLED, INSTALLED, TEST_COPY, true) == ExeVerdict::Same);
    }
    SECTION("an old hub without the field is judged by its pid's image path")
    {
        CHECK(judge_exe(INSTALLED, "", INSTALLED, true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, "", TEST_COPY, true) == ExeVerdict::Foreign);
    }
    SECTION("nothing to compare is never foreign")
    {
        CHECK(judge_exe(INSTALLED, "", "", true) == ExeVerdict::Unknown);
        CHECK(judge_exe("", TEST_COPY, TEST_COPY, true) == ExeVerdict::Unknown);
    }
}

TEST_CASE("hub handover: the decision table", "[HubHandover]")
{
    Facts f;

    SECTION("no hub: start our own")
    {
        f.hub_alive = false;
        f.exe       = ExeVerdict::Foreign; // stale facts do not matter without a hub
        CHECK(plan(f) == Step::SpawnOwn);
    }
    SECTION("same exe: use it (two windows of one install do not fight)")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Same;
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("unknown exe: use it")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Unknown;
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("foreign exe: ask it to hand over, and start ours once it is gone")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Foreign;
        REQUIRE(plan(f) == Step::ReplaceRunning);
        CHECK(after_quit(true) == Outcome::SpawnOwn);
    }
    SECTION("foreign exe and the hub stays: leave it running, no second hub, no kill")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Foreign;
        REQUIRE(plan(f) == Step::ReplaceRunning);
        CHECK(after_quit(false) == Outcome::LeaveRunning);
    }
    SECTION("a process the hub started itself never asks for a handover")
    {
        f.hub_alive        = true;
        f.exe              = ExeVerdict::Foreign;
        f.handover_allowed = false; // hub-spawned instance, or this process already had its one reclaim
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("another version still replaces the hub, as it always has")
    {
        f.hub_alive       = true;
        f.version_differs = true;
        f.exe             = ExeVerdict::Same;
        CHECK(plan(f) == Step::ReplaceRunning);
        f.handover_allowed = false;
        CHECK(plan(f) == Step::ReplaceRunning);
    }
}

TEST_CASE("hub handover: the hub's check of its own install", "[HubHandover]")
{
    SECTION("everything there: nothing happens, the count resets")
    {
        SelfCheck r = self_check(0, true, true);
        CHECK(r.misses == 0);
        CHECK(!r.quit);
        r = self_check(1, true, true);
        CHECK(r.misses == 0);
        CHECK(!r.quit);
    }
    SECTION("one miss is a warning, not an exit")
    {
        SelfCheck r = self_check(0, true, false);
        CHECK(r.misses == 1);
        CHECK(!r.quit);
    }
    SECTION("consecutive misses end it, whichever half is gone")
    {
        CHECK(self_check(1, true, false).quit);  // web pages gone, exe still there (a locked exe survives a delete)
        CHECK(self_check(1, false, true).quit);  // exe gone
        CHECK(self_check(1, false, false).quit);
        CHECK(self_check(5, true, false).quit);
    }
    SECTION("a recovery in between starts the count again")
    {
        SelfCheck r = self_check(0, true, false);
        REQUIRE(!r.quit);
        r = self_check(r.misses, true, true);
        REQUIRE(r.misses == 0);
        r = self_check(r.misses, true, false);
        CHECK(!r.quit);
        CHECK(r.misses == 1);
    }
    SECTION("the intervals mean 'about a minute' for a deleted folder")
    {
        CHECK(SELF_CHECK_INTERVAL_S * SELF_CHECK_MISSES <= 60);
        CHECK(SELF_CHECK_MISSES >= 2);
    }
}

TEST_CASE("hub handover: only well-formed UTF-8 goes into hub.json", "[HubHandover]")
{
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("C:\\Dev\\EdgeSlicer.exe"));
    CHECK(is_valid_utf8("C:\\Users\\J\xC3\xBCrgen\\EdgeSlicer.exe")); // u-umlaut
    CHECK(is_valid_utf8("\xE2\x82\xAC"));                              // euro sign
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));                          // 4-byte
    CHECK(!is_valid_utf8("C:\\Users\\J\xFCrgen\\EdgeSlicer.exe"));     // ANSI u-umlaut
    CHECK(!is_valid_utf8("\xC3"));                                     // truncated
    CHECK(!is_valid_utf8("\xC0\x80"));                                 // overlong
    CHECK(!is_valid_utf8("\xED\xA0\x80"));                             // surrogate
}
