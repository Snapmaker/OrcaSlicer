#include <catch2/catch_test_macros.hpp>

// Snapmaker Orca: the expiry rules of the TimeoutMap behind SSWCP's instance list. The checker
// thread ticks once a second, so every case below waits a little over one or two ticks.
#include "slic3r/Utils/TimeoutMap.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

// Stands in for SSWCP_Instance: a shared_ptr value whose on_timeout() the checker thread calls.
struct Probe
{
    static std::atomic<int> timeouts;
    void on_timeout() { ++timeouts; }
};
std::atomic<int> Probe::timeouts{0};

using ProbeMap = TimeoutMap<int, std::shared_ptr<Probe>>;

// One checker tick is a second; a wait of 1.4 s always spans at least one tick.
constexpr auto ONE_TICK = 1400ms;

} // namespace

TEST_CASE("a snapshot is taken under the lock while the checker thread erases", "[TimeoutMap]")
{
    Probe::timeouts = 0;
    ProbeMap map;
    constexpr int count = 400;
    for (int i = 0; i < count; ++i)
        map.add(i, std::make_shared<Probe>(), 10ms);
    REQUIRE(map.size() == size_t(count));

    // Snapshots only ever shrink (nothing is added), every copied value is intact, and the walk
    // never touches the map's storage while the checker erases the expired entries.
    size_t     previous  = size_t(count);
    const auto deadline  = std::chrono::steady_clock::now() + 2 * ONE_TICK;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = map.get_snapshot();
        CHECK(snapshot.size() <= previous);
        for (const auto& entry : snapshot)
            CHECK(entry.second != nullptr);
        previous = snapshot.size();
        if (previous == 0)
            break;
    }
    CHECK(map.size() == 0);
    CHECK(map.get_snapshot().empty());
    CHECK(Probe::timeouts.load() == count);
}

TEST_CASE("a paused item outlives its timeout until it is armed again", "[TimeoutMap]")
{
    Probe::timeouts = 0;
    ProbeMap map;
    map.add(1, std::make_shared<Probe>(), 20ms);
    REQUIRE(map.pause(1));
    CHECK_FALSE(map.pause(2));   // unknown key

    std::this_thread::sleep_for(ONE_TICK);
    CHECK(map.size() == 1);
    CHECK(map.exists(1));
    CHECK(map.get(1) != nullptr);
    CHECK(Probe::timeouts.load() == 0);

    // update_timeout() ends the pause: the item is due again after the new timeout.
    REQUIRE(map.update_timeout(1, 20ms));
    std::this_thread::sleep_for(ONE_TICK);
    CHECK(map.size() == 0);
    CHECK(map.get(1) == nullptr);
    CHECK(Probe::timeouts.load() == 1);
}

TEST_CASE("a renewed item stays, an infinite item never expires", "[TimeoutMap]")
{
    Probe::timeouts = 0;
    ProbeMap map;
    map.add(1, std::make_shared<Probe>(), 200ms);
    map.add_infinite(2, std::make_shared<Probe>());

    // Renewals every 100 ms keep item 1 ahead of its 200 ms timeout across a tick.
    const auto deadline = std::chrono::steady_clock::now() + ONE_TICK;
    while (std::chrono::steady_clock::now() < deadline) {
        REQUIRE(map.update_timeout(1, 200ms));
        std::this_thread::sleep_for(100ms);
    }
    CHECK(map.size() == 2);
    CHECK(Probe::timeouts.load() == 0);

    // Left alone, item 1 goes; the infinite item is read back by every accessor.
    std::this_thread::sleep_for(ONE_TICK);
    CHECK(map.size() == 1);
    CHECK(Probe::timeouts.load() == 1);
    CHECK(map.exists(2));
    CHECK(map.get(2) != nullptr);
    CHECK(map.get_and_remove(2) != nullptr);
    CHECK(map.size() == 0);
}
