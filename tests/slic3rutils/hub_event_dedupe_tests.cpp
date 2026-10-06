// The hub's "is this an event it already has?" rule (HubEventDedupe) and the machine-wide claim on
// a printer's notifications (HubPushOwner).
//
// What these are about: on 2026-09-24 the owner's phone listed one print start twice and two HMS
// codes three times each. Several EdgeSlicer processes were running at once, each window with its
// own watcher and two data dirs with a hub each; every one of them saw the same printer. The hub is
// the one place that sees every window's report, so it is where "that is the same happening" is
// decided - and the claim is what keeps two hubs on one PC from both notifying the phone.

#include <catch2/catch.hpp>

#include "slic3r/GUI/HubEventDedupe.hpp"
#include "slic3r/GUI/HubPushOwner.hpp"

#include <boost/filesystem.hpp>

#include <deque>
#include <string>

using nlohmann::json;
using namespace Slic3r::GUI;

namespace {

const long long T0 = 1790000000000LL; // an arbitrary "now", in ms

json ev(const std::string& kind, long long instance, long long time, const std::string& job = "Cube.gcode.3mf",
        const std::string& code = "", const std::string& job_id = "")
{
    json e;
    e["kind"]     = kind;
    e["instance"] = instance;
    e["time"]     = time;
    e["printer"]  = { { "id", "01P00A000000001" }, { "name", "3DPO" }, { "kind", "bambu" } };
    if (!job.empty()) e["job"] = job;
    if (!code.empty()) e["code"] = code;
    if (!job_id.empty()) e["job_id"] = job_id;
    return e;
}

} // namespace

TEST_CASE("[HubEventDedupe] a start seen by a second window is the same start", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("started", 100, T0) };
    // Another window, ten seconds later, and then an hour later (its LAN session came round to
    // the printer late): the same print either way.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0), T0 + 10000) == 0);
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0), T0 + 3600000) == 0);
    // Another spelling of the same file is still the same print.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0, "/cache/cube.3mf"), T0 + 10000) == 0);
}

TEST_CASE("[HubEventDedupe] a start after the job ended is a new print", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("started", 100, T0), ev("finished", 100, T0 + 1000000) };
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0), T0 + 1000500) == -1);
}

TEST_CASE("[HubEventDedupe] a different job, or the same window, is not a duplicate", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("started", 100, T0) };
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0, "Benchy.3mf"), T0 + 1000) == -1);
    // The same window's watcher only reports edges: a repeat from it is real.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 100, 0), T0 + 1000) == -1);
}

TEST_CASE("[HubEventDedupe] the printer's job id decides where both sides have one", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("started", 100, T0, "Cube", "", "4471") };
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0, "Other name", "", "4471"), T0 + 1000) == 0);
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0, "Cube", "", "4472"), T0 + 1000) == -1);
    // One side without an id: the names decide.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("started", 200, 0, "cube.gcode.3mf"), T0 + 1000) == 0);
}

TEST_CASE("[HubEventDedupe] an HMS code from another window within half an hour is one event", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("error", 100, T0, "Golurk", "0701220000020025") };
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("error", 200, 0, "Golurk", "0701220000020025"), T0 + 60000) == 0);
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("error", 300, 0, "Golurk", "0701220000020025"), T0 + 29 * 60000) == 0);
    // Another code is another event.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("error", 200, 0, "Golurk", "0C00010000020015"), T0 + 60000) == -1);
    // Long after: the watcher only reports a code again once it cleared, so this is a new one.
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("error", 200, 0, "Golurk", "0701220000020025"), T0 + 31 * 60000) == -1);
}

TEST_CASE("[HubEventDedupe] the other kinds keep the two-minute window", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("paused", 100, T0) };
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("paused", 200, 0), T0 + 30000) == 0);
    REQUIRE(HubEventDedupe::find_duplicate(ring, ev("paused", 200, 0), T0 + 3 * 60000) == -1);
}

TEST_CASE("[HubEventDedupe] another printer is never a duplicate", "[HubEventDedupe]")
{
    std::deque<json> ring { ev("started", 100, T0) };
    json other = ev("started", 200, 0);
    other["printer"]["id"] = "01P00A000000002";
    REQUIRE(HubEventDedupe::find_duplicate(ring, other, T0 + 1000) == -1);
}

TEST_CASE("[HubEventDedupe] the uid is the hub instance and the id", "[HubEventDedupe]")
{
    REQUIRE(HubEventDedupe::uid("e45ee036f3f4325617df5466ccc2b292", 413) == "e45ee036f3f4325617df5466ccc2b292-413");
    REQUIRE(HubEventDedupe::uid("", 7) == "hub-7");
}

TEST_CASE("[HubPushOwner] one claim per printer, and only physical printers are claimed", "[HubPushOwner]")
{
    REQUIRE(HubPushOwner::claimable_kind("bambu"));
    REQUIRE(HubPushOwner::claimable_kind("snapmaker"));
    REQUIRE_FALSE(HubPushOwner::claimable_kind("printhost"));
    REQUIRE_FALSE(HubPushOwner::claimable_kind("connect"));
    REQUIRE(HubPushOwner::file_name("sm:A") != HubPushOwner::file_name("sm_A"));
    REQUIRE(HubPushOwner::file_name("sm:U1-0042").find(':') == std::string::npos);

    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
                                        boost::filesystem::unique_path("edgeslicer-push-owner-test-%%%%%%%%");
    {
        HubPushOwner first(dir.string()), second(dir.string());
        REQUIRE(first.claim("01P00A000000001"));
        REQUIRE(first.claim("01P00A000000001")); // held: asking again is a yes
#ifdef _WIN32
        // LockFileEx locks belong to the handle, so a second owner in this process stands in for
        // a second hub. (POSIX record locks belong to the process, so this half is Windows-only.)
        REQUIRE_FALSE(second.claim("01P00A000000001"));
        REQUIRE(second.claim("01P00A000000002")); // another printer is free
        first.release_all();
        REQUIRE(second.claim("01P00A000000001")); // and the claim moves once the first lets go
#endif
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
}
