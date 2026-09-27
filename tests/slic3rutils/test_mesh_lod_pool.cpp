// Snapmaker Orca: the worker pool and the cache of the render LOD. Neither needs an application,
// a window or an OpenGL context, so the cases run unconditionally.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <tbb/task_arena.h>

#include "libslic3r/MeshLod.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/MeshLodCache.hpp"
#include "slic3r/GUI/MeshLodPool.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

using Clock = std::chrono::steady_clock;
using SlotPtr = std::shared_ptr<MeshLodSlot>;

// its_make_sphere yields about sectors^2 faces.
std::shared_ptr<const TriangleMesh> sphere_mesh(int sectors)
{
    return std::make_shared<const TriangleMesh>(its_make_sphere(20., 2. * M_PI / sectors));
}

MeshLodJob job_for(const std::shared_ptr<const TriangleMesh>& mesh, const SlotPtr& slot)
{
    MeshLodJob job;
    job.mesh           = mesh;
    job.slot           = slot;
    job.smooth_normals = false;
    job.faces          = mesh->its.indices.size();
    return job;
}

bool is_final(MeshLodSlot::State state)
{
    return state != MeshLodSlot::Queued && state != MeshLodSlot::Running;
}

// Polls from the test's thread; the workers never assert.
bool wait_until(const std::function<bool()>& condition, std::chrono::seconds timeout = std::chrono::seconds(120))
{
    const Clock::time_point end = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool wait_final(const MeshLod& lod)
{
    return wait_until([&lod] { return is_final(lod.state()); });
}

// An admission stub that holds the first job it sees until the test lets it go, and records
// the order in which the jobs reached it.
struct GatedAdmission
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    open{false};
    std::vector<size_t>     order;
    size_t                  refused_bytes{0};

    bool admit(size_t job_bytes)
    {
        std::unique_lock<std::mutex> lock(mutex);
        order.push_back(job_bytes);
        cv.notify_all();
        cv.wait(lock, [this] { return open; });
        return job_bytes != refused_bytes;
    }
    void wait_entered(size_t jobs = 1)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this, jobs] { return order.size() >= jobs; });
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("Cancelled jobs never publish a result and the pool shuts down promptly", "[MeshLodPool]")
{
    // How long the workers get before half of the jobs are cancelled: none of them has started,
    // the first ones run, some are built already.
    const int head_start_ms = GENERATE(0, 30, 300);

    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(224);
    REQUIRE(mesh->its.indices.size() > 49000);

    std::vector<SlotPtr> slots;
    MeshLodPool          pool(2, {});
    for (int i = 0; i < 50; ++i) {
        slots.emplace_back(std::make_shared<MeshLodSlot>());
        pool.submit(job_for(mesh, slots.back()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(head_start_ms));

    for (size_t i = 0; i < slots.size(); i += 2)
        slots[i]->request_cancel();

    const Clock::time_point start = Clock::now();
    pool.shutdown();
    const auto shutdown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    CHECK(shutdown_ms < 2000);
    CHECK(pool.queued_jobs() == 0);

    size_t built_bytes = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        const MeshLodSlot::State state = slots[i]->state();
        INFO("slot " << i << " state " << int(state));
        CHECK(is_final(state));
        if (i % 2 == 0)
            CHECK(state == MeshLodSlot::Cancelled);
        if (state == MeshLodSlot::Cancelled)
            CHECK_FALSE(slots[i]->has_pending());
        else {
            // Only a job that finished before the shutdown is left.
            CHECK(state == MeshLodSlot::Built);
            CHECK(slots[i]->has_pending());
            built_bytes += slots[i]->pending_bytes();
        }
    }
    CHECK(pool.unpromoted_bytes() == built_bytes);

    // Whoever drops a built result returns its bytes, with or without the pool.
    for (const SlotPtr& slot : slots)
        slot->request_cancel();
    CHECK(pool.unpromoted_bytes() == 0);
    for (const SlotPtr& slot : slots)
        CHECK_FALSE(slot->has_pending());
}

TEST_CASE("A single worker builds the largest mesh first and respects the admission", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> gate_mesh = sphere_mesh(150);
    const std::shared_ptr<const TriangleMesh> small     = sphere_mesh(170);
    const std::shared_ptr<const TriangleMesh> medium    = sphere_mesh(200);
    const std::shared_ptr<const TriangleMesh> refused   = sphere_mesh(220);
    const std::shared_ptr<const TriangleMesh> large     = sphere_mesh(240);
    REQUIRE(gate_mesh->its.indices.size() >= LodParams().min_faces);

    auto bytes_of = [](const std::shared_ptr<const TriangleMesh>& mesh) {
        return estimate_lod_job_bytes(mesh->its.indices.size(), mesh->its.vertices.size());
    };

    GatedAdmission admission;
    admission.refused_bytes = bytes_of(refused);

    const SlotPtr gate_slot = std::make_shared<MeshLodSlot>(), small_slot = std::make_shared<MeshLodSlot>(),
                  medium_slot = std::make_shared<MeshLodSlot>(), refused_slot = std::make_shared<MeshLodSlot>(),
                  large_slot = std::make_shared<MeshLodSlot>();
    const std::vector<SlotPtr> slots{gate_slot, small_slot, medium_slot, refused_slot, large_slot};

    MeshLodPool pool(1, [&admission](size_t job_bytes) { return admission.admit(job_bytes); });
    // The only worker is held inside the admission of the first job while the others queue up.
    pool.submit(job_for(gate_mesh, gate_slot));
    admission.wait_entered();
    pool.submit(job_for(small, small_slot));
    pool.submit(job_for(large, large_slot));
    pool.submit(job_for(refused, refused_slot));
    pool.submit(job_for(medium, medium_slot));
    CHECK(pool.queued_jobs() == 4);
    admission.release();

    REQUIRE(wait_until([&slots] {
        for (const SlotPtr& slot : slots)
            if (!is_final(slot->state()))
                return false;
        return true;
    }));
    pool.shutdown();

    const std::vector<size_t> expected{bytes_of(gate_mesh), bytes_of(large), bytes_of(refused), bytes_of(medium), bytes_of(small)};
    CHECK(admission.order == expected);

    CHECK(refused_slot->state() == MeshLodSlot::RejectedMemory);
    CHECK_FALSE(refused_slot->has_pending());
    for (const SlotPtr& slot : {gate_slot, small_slot, medium_slot, large_slot}) {
        CHECK(slot->state() == MeshLodSlot::Built);
        CHECK_FALSE(slot->pending(0).is_empty());
    }
}

TEST_CASE("Huge meshes are built one at a time while smaller ones keep flowing", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> huge_a = sphere_mesh(240);
    const std::shared_ptr<const TriangleMesh> huge_b = sphere_mesh(220);
    const std::shared_ptr<const TriangleMesh> small  = sphere_mesh(150);

    MeshLodPoolLimits limits;
    limits.huge_mesh_faces = 30000;
    REQUIRE(huge_b->its.indices.size() > limits.huge_mesh_faces);
    REQUIRE(small->its.indices.size() <= limits.huge_mesh_faces);

    auto bytes_of = [](const std::shared_ptr<const TriangleMesh>& mesh) {
        return estimate_lod_job_bytes(mesh->its.indices.size(), mesh->its.vertices.size());
    };

    GatedAdmission admission;
    const SlotPtr  slot_a = std::make_shared<MeshLodSlot>(), slot_b = std::make_shared<MeshLodSlot>(), slot_small = std::make_shared<MeshLodSlot>();
    MeshLodPool    pool(2, [&admission](size_t job_bytes) { return admission.admit(job_bytes); }, LodParams(), {}, limits);
    pool.submit(job_for(huge_a, slot_a));
    admission.wait_entered(1);
    pool.submit(job_for(huge_b, slot_b));
    pool.submit(job_for(small, slot_small));
    // The second worker passes the larger job over, because a huge one is being built.
    admission.wait_entered(2);
    const MeshLodSlot::State state_b_while_a_runs = slot_b->state();
    admission.release();

    REQUIRE(wait_until([&] { return is_final(slot_a->state()) && is_final(slot_b->state()) && is_final(slot_small->state()); }));
    pool.shutdown();
    CHECK(state_b_while_a_runs == MeshLodSlot::Queued);
    const std::vector<size_t> expected{bytes_of(huge_a), bytes_of(small), bytes_of(huge_b)};
    CHECK(admission.order == expected);
    CHECK(slot_a->state() == MeshLodSlot::Built);
    CHECK(slot_b->state() == MeshLodSlot::Built);
    CHECK(slot_small->state() == MeshLodSlot::Built);
}

TEST_CASE("Workers pause while built geometry waits for the main thread", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(150);
    MeshLodPoolLimits limits;
    limits.max_unpromoted_bytes = 1;

    std::vector<SlotPtr> slots;
    MeshLodPool          pool(2, {}, LodParams(), {}, limits);
    const SlotPtr        first = std::make_shared<MeshLodSlot>();
    pool.submit(job_for(mesh, first));
    REQUIRE(wait_until([&first] { return is_final(first->state()); }));
    REQUIRE(first->state() == MeshLodSlot::Built);
    CHECK(pool.unpromoted_bytes() == first->pending_bytes());

    for (int i = 0; i < 2; ++i) {
        slots.emplace_back(std::make_shared<MeshLodSlot>());
        pool.submit(job_for(mesh, slots.back()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(pool.queued_jobs() == 2);
    CHECK(slots[0]->state() == MeshLodSlot::Queued);
    CHECK(slots[1]->state() == MeshLodSlot::Queued);

    // The main thread takes the result over: the workers go on.
    GLModel::Geometry taken[2];
    CHECK(first->resolve_built(MeshLodSlot::Promoted, taken));
    CHECK_FALSE(taken[0].is_empty());
    CHECK(first->state() == MeshLodSlot::Promoted);
    CHECK_FALSE(first->has_pending());
    REQUIRE(wait_until([&slots] { return is_final(slots[0]->state()) || is_final(slots[1]->state()); }));

    // Dropping a result instead of promoting it releases the workers as well.
    REQUIRE(wait_until([&slots] {
        for (const SlotPtr& slot : slots)
            if (slot->state() == MeshLodSlot::Built)
                slot->request_cancel();
        return slots[0]->state() == MeshLodSlot::Cancelled && slots[1]->state() == MeshLodSlot::Cancelled;
    }));
    pool.shutdown();
    CHECK(pool.unpromoted_bytes() == 0);
}

TEST_CASE("No LOD cache exists without the application and a volume collection does not opt in by itself", "[MeshLodPool]")
{
    // The command line slicer, the calibration helpers and the thumbnail collections rely on both.
    CHECK(MeshLodCache::instance() == nullptr);
    GLVolumeCollection volumes;
    CHECK_FALSE(volumes.lod_enabled());
}

TEST_CASE("A job runs in a task arena with a single slot", "[MeshLodPool]")
{
    std::atomic<int> concurrency_in_job{-1};
    const SlotPtr    slot = std::make_shared<MeshLodSlot>();
    {
        MeshLodPool pool(1, {}, LodParams(), [&concurrency_in_job] { concurrency_in_job = tbb::this_task_arena::max_concurrency(); });
        pool.submit(job_for(sphere_mesh(150), slot));
        REQUIRE(wait_until([&slot] { return is_final(slot->state()); }));
    }
    CHECK(concurrency_in_job.load() == 1);
    CHECK(slot->state() == MeshLodSlot::Built);
}

TEST_CASE("The application's cache registers itself and unregisters at shutdown", "[MeshLodPool]")
{
    REQUIRE(MeshLodCache::instance() == nullptr);
    {
        MeshLodCache cache;
        CHECK(MeshLodCache::instance() == &cache);
        CHECK_FALSE(cache.enabled());
        // Switched off, the feature owns no thread.
        CHECK(cache.worker_count() == 0);
        cache.shutdown();
        CHECK(MeshLodCache::instance() == nullptr);
        cache.shutdown();
    }
    CHECK(MeshLodCache::instance() == nullptr);
}

TEST_CASE("The workers of the cache exist only while it is enabled", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(150);
    MeshLodCache cache(2, {}, LodParams(), false);
    CHECK(cache.worker_count() == 0);

    cache.set_enabled(true);
    CHECK(cache.worker_count() == 2);
    cache.set_enabled(true);
    CHECK(cache.worker_count() == 2);

    cache.set_enabled(false);
    CHECK(cache.worker_count() == 0);
    CHECK(cache.queued_jobs() == 0);
    CHECK(cache.unpromoted_bytes() == 0);

    // The pool of the second round is a new one, and it works.
    cache.set_enabled(true);
    CHECK(cache.worker_count() == 2);
    const std::shared_ptr<MeshLod> lod = cache.acquire(mesh);
    REQUIRE(lod != nullptr);
    REQUIRE(wait_final(*lod));
    CHECK(lod->state() == MeshLodSlot::Built);

    cache.shutdown();
    CHECK(cache.worker_count() == 0);
    // Nothing brings the workers back after the shutdown.
    cache.set_enabled(false);
    cache.set_enabled(true);
    CHECK(cache.worker_count() == 0);
    CHECK_FALSE(cache.enabled());
    CHECK(cache.acquire(mesh) == nullptr);
}

TEST_CASE("Disabling the cache joins a worker that is in the middle of a job", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(400);
    MeshLodCache cache(1, {}, LodParams(), false);
    cache.set_enabled(true);
    const std::shared_ptr<MeshLod> lod = cache.acquire(mesh);
    REQUIRE(lod != nullptr);
    REQUIRE(wait_until([&lod] { return lod->state() != MeshLodSlot::Queued; }));

    const Clock::time_point start = Clock::now();
    cache.set_enabled(false);
    CHECK(Clock::now() - start < std::chrono::seconds(2));
    CHECK(cache.worker_count() == 0);
    // Joined: the job is over, whichever way it ended.
    CHECK(is_final(lod->state()));
    CHECK((lod->state() == MeshLodSlot::Cancelled || lod->state() == MeshLodSlot::Built));
}

TEST_CASE("The cache hands out nothing while disabled, for a small mesh and after shutdown", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(150);
    const std::shared_ptr<const TriangleMesh> tiny = sphere_mesh(40);
    REQUIRE(tiny->its.indices.size() < LodParams().min_faces);

    MeshLodCache cache(1, {}, LodParams(), false);
    CHECK(MeshLodCache::instance() == nullptr);
    CHECK(cache.acquire(mesh) == nullptr);
    CHECK(cache.queued_jobs() == 0);

    cache.set_enabled(true);
    CHECK(cache.acquire(tiny) == nullptr);
    CHECK(cache.acquire(nullptr) == nullptr);
    CHECK(cache.acquire(mesh) != nullptr);

    cache.shutdown();
    CHECK(cache.acquire(mesh) == nullptr);
}

TEST_CASE("Volumes of one mesh share the reduced models and the budget gets its bytes back", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(200);
    MeshLodCache cache(1, {}, LodParams(), false);
    cache.set_enabled(true);

    std::shared_ptr<MeshLod> first  = cache.acquire(mesh);
    std::shared_ptr<MeshLod> second = cache.acquire(mesh);
    REQUIRE(first != nullptr);
    CHECK(first == second);
    CHECK(first->model(LodLevel::Middle) == nullptr);

    REQUIRE(wait_final(*first));
    REQUIRE(first->state() == MeshLodSlot::Built);
    CHECK(cache.unpromoted_bytes() > 0);
    CHECK(first->try_promote());
    CHECK_FALSE(first->try_promote());
    CHECK(cache.unpromoted_bytes() == 0);
    CHECK(cache.budget().used > 0);

    CHECK(first->model(LodLevel::High) == nullptr);
    GLModel* middle = first->model(LodLevel::Middle);
    GLModel* small  = first->model(LodLevel::Small);
    REQUIRE(middle != nullptr);
    REQUIRE(small != nullptr);
    CHECK(middle->indices_count() < mesh->its.indices.size() * 3);
    CHECK(small->indices_count() < middle->indices_count());
    CHECK(first->promoted_faces() == (middle->indices_count() + small->indices_count()) / 3);

    first.reset();
    CHECK(cache.budget().used > 0);
    second.reset();
    CHECK(cache.budget().used == 0);

    // The expired entry is replaced, not revived.
    std::shared_ptr<MeshLod> again = cache.acquire(mesh);
    REQUIRE(again != nullptr);
    CHECK(again->state() != MeshLodSlot::Promoted);
    CHECK(cache.entries() == 1);
}

TEST_CASE("A mesh over the budget keeps its full detail until the budget has room", "[MeshLodPool]")
{
    const std::shared_ptr<const TriangleMesh> mesh = sphere_mesh(200);
    MeshLodCache cache(1, {}, LodParams(), false);
    cache.set_enabled(true);
    cache.set_budget_cap(1);

    const std::shared_ptr<MeshLod> lod = cache.acquire(mesh);
    REQUIRE(lod != nullptr);
    REQUIRE(wait_final(*lod));
    CHECK_FALSE(lod->try_promote());
    CHECK(lod->state() == MeshLodSlot::OverBudget);
    CHECK(cache.unpromoted_bytes() == 0);
    CHECK(cache.budget().used == 0);
    CHECK(lod->model(LodLevel::Small) == nullptr);

    // No room: asking again does not build again.
    CHECK_FALSE(lod->needs_retry());
    CHECK(cache.acquire(mesh) == lod);
    CHECK(lod->state() == MeshLodSlot::OverBudget);

    cache.set_budget_cap(size_t(512) << 20);
    CHECK(lod->needs_retry());
    CHECK(cache.acquire(mesh) == lod);
    CHECK(lod->state() != MeshLodSlot::OverBudget);
    REQUIRE(wait_final(*lod));
    CHECK(lod->try_promote());
    CHECK(lod->model(LodLevel::Small) != nullptr);
}

TEST_CASE("Disabling the cache cancels its jobs and enabling it again retries them", "[MeshLodPool]")
{
    GatedAdmission admission;
    const std::shared_ptr<const TriangleMesh> running_mesh = sphere_mesh(150);
    const std::shared_ptr<const TriangleMesh> queued_mesh  = sphere_mesh(160);

    MeshLodCache cache(1, [&admission](size_t job_bytes) { return admission.admit(job_bytes); }, LodParams(), false);
    cache.set_enabled(true);
    const std::shared_ptr<MeshLod> running = cache.acquire(running_mesh);
    admission.wait_entered();
    const std::shared_ptr<MeshLod> queued = cache.acquire(queued_mesh);
    if (running == nullptr || queued == nullptr)
        admission.release();   // a failing REQUIRE must not leave the worker waiting at the gate
    REQUIRE(running != nullptr);
    REQUIRE(queued != nullptr);

    // Disabling joins the workers, and the running job waits at the gate: it is opened from a
    // second thread as soon as the queued job is seen dropped, that is after the cancellation.
    std::thread releaser([&admission, &queued] {
        wait_until([&queued] { return queued->state() == MeshLodSlot::Cancelled; });
        admission.release();
    });
    cache.set_enabled(false);
    releaser.join();
    // The queued job is dropped at once, the running one at its next look at the flag.
    CHECK(queued->state() == MeshLodSlot::Cancelled);
    CHECK(cache.worker_count() == 0);
    CHECK(cache.queued_jobs() == 0);
    CHECK(cache.acquire(queued_mesh) == nullptr);
    REQUIRE(wait_final(*running));
    CHECK(running->state() == MeshLodSlot::Cancelled);
    CHECK(running->needs_retry());

    cache.set_enabled(true);
    CHECK(cache.acquire(running_mesh) == running);
    CHECK(cache.acquire(queued_mesh) == queued);
    REQUIRE(wait_final(*running));
    REQUIRE(wait_final(*queued));
    CHECK(running->state() == MeshLodSlot::Built);
    CHECK(queued->state() == MeshLodSlot::Built);
}

TEST_CASE("A job that was not admitted is built by a later acquire of the mesh its owner holds", "[MeshLodPool]")
{
    // What GLVolumeCollection::update_lod() does for a volume that a scene reload kept.
    std::atomic<bool> memory_short{true};
    MeshLodCache cache(1, [&memory_short](size_t) { return !memory_short.load(); }, LodParams(), false);
    cache.set_enabled(true);

    const std::shared_ptr<MeshLod> lod = cache.acquire(sphere_mesh(150));
    REQUIRE(lod != nullptr);
    REQUIRE(wait_final(*lod));
    CHECK(lod->state() == MeshLodSlot::RejectedMemory);
    CHECK(lod->needs_retry());

    // Still short: the retry ends the same way and stays worth another one.
    CHECK(cache.acquire(lod->mesh()) == lod);
    REQUIRE(wait_final(*lod));
    CHECK(lod->state() == MeshLodSlot::RejectedMemory);

    memory_short = false;
    CHECK(cache.acquire(lod->mesh()) == lod);
    REQUIRE(wait_final(*lod));
    CHECK(lod->state() == MeshLodSlot::Built);
    CHECK_FALSE(lod->needs_retry());
    CHECK(cache.entries() == 1);

    // Disabled, nothing is submitted: the retry pass never runs then.
    cache.set_enabled(false);
    CHECK(cache.acquire(lod->mesh()) == nullptr);
}

TEST_CASE("A worker gives notice of a result that waits for the main thread and of nothing else", "[MeshLodPool]")
{
    std::atomic<int>  notices{0};
    std::atomic<bool> memory_short{false};
    const SlotPtr built = std::make_shared<MeshLodSlot>(), refused = std::make_shared<MeshLodSlot>(),
                  useless = std::make_shared<MeshLodSlot>(), second = std::make_shared<MeshLodSlot>();
    {
        MeshLodPool pool(1, [&memory_short](size_t) { return !memory_short.load(); }, LodParams(), {}, {},
                         [&notices] { ++notices; });
        pool.submit(job_for(sphere_mesh(150), built));
        REQUIRE(wait_until([&built] { return is_final(built->state()); }));
        REQUIRE(built->state() == MeshLodSlot::Built);
        REQUIRE(wait_until([&notices] { return notices.load() == 1; }));

        // Not admitted: nothing to take over, no notice.
        memory_short = true;
        pool.submit(job_for(sphere_mesh(150), refused));
        REQUIRE(wait_until([&refused] { return is_final(refused->state()); }));
        CHECK(refused->state() == MeshLodSlot::RejectedMemory);
        memory_short = false;

        // Under the minimum face count: no reduced copy, no notice.
        pool.submit(job_for(sphere_mesh(40), useless));
        REQUIRE(wait_until([&useless] { return is_final(useless->state()); }));
        CHECK(useless->state() == MeshLodSlot::Rejected);

        // The worker is past both jobs once the next result is announced.
        pool.submit(job_for(sphere_mesh(160), second));
        REQUIRE(wait_until([&notices] { return notices.load() == 2; }));
        CHECK(second->state() == MeshLodSlot::Built);
    }
    CHECK(notices.load() == 2);
}

TEST_CASE("The cache hands the notice of a finished job to its pool and wakes nothing without an application", "[MeshLodPool]")
{
    std::atomic<int> notices{0};
    MeshLodCache cache(1, {}, LodParams(), false, [&notices] { ++notices; });
    cache.set_enabled(true);
    const std::shared_ptr<MeshLod> lod = cache.acquire(sphere_mesh(150));
    REQUIRE(lod != nullptr);
    REQUIRE(wait_until([&notices] { return notices.load() == 1; }));
    CHECK(lod->state() == MeshLodSlot::Built);

    // Every pool the cache creates gets it.
    cache.set_enabled(false);
    cache.set_enabled(true);
    const std::shared_ptr<MeshLod> other = cache.acquire(sphere_mesh(160));
    REQUIRE(other != nullptr);
    REQUIRE(wait_until([&notices] { return notices.load() == 2; }));

    // No application, no canvas: both forms of the request return without touching anything.
    cache.request_canvas_wake();
    cache.request_canvas_wake(true);
    cache.shutdown();
    cache.request_canvas_wake();
    SUCCEED("no wake without an application");
}

TEST_CASE("Reduced models outlive the cache that made them", "[MeshLodPool]")
{
    // The order at application exit is not fixed: the last volume may die after the cache.
    std::shared_ptr<MeshLod> built, unfinished;
    {
        MeshLodCache cache(1, {}, LodParams(), false);
        cache.set_enabled(true);
        built = cache.acquire(sphere_mesh(150));
        REQUIRE(built != nullptr);
        REQUIRE(wait_final(*built));
        unfinished = cache.acquire(sphere_mesh(240));
        REQUIRE(unfinished != nullptr);
    }
    CHECK(is_final(unfinished->state()));
    CHECK(built->state() == MeshLodSlot::Built);
    CHECK(built->try_promote());
    CHECK(built->model(LodLevel::Middle) != nullptr);
    unfinished.reset();
    built.reset();
    SUCCEED("destroyed after the cache");
}
