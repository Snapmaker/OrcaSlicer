// Snapmaker Orca: joined background workers that build the reduced render-LOD meshes as plain CPU
// data (GLModel::Geometry); jobs are cancelled through their shared slot. No wxWidgets or OpenGL,
// so unit tests need no application.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "libslic3r/MeshLod.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "GLModel.hpp"

namespace Slic3r {
namespace GUI {

// What the workers of one pool wait on, and the account of the geometry that is built but not
// yet taken over by the main thread. Held by shared_ptr from the pool and from every slot that
// carries such geometry, so a slot can return its bytes whether or not the pool still exists.
struct MeshLodPoolSync
{
    std::mutex              mutex;
    std::condition_variable cv;
    // Written with the mutex held (the workers' wait predicate reads it), atomic for the getter.
    std::atomic<size_t>     unpromoted_bytes{0};
};

// Job state shared (shared_ptr) by the pool and the job's owner (MeshLod, a test). The worker alone
// writes the pending geometry while Running; from Built on it belongs to the main thread. Cancel
// and publish exclude each other via the slot's mutex, so a cancelled slot never becomes Built.
class MeshLodSlot
{
public:
    enum State : int {
        Queued,          // waiting in the pool
        Running,         // a worker is building
        Built,           // pending geometry ready for the main thread
        Promoted,        // taken over by the main thread
        Rejected,        // the mesh yields no usable reduced copy, or the build failed; final
        RejectedMemory,  // not admitted at dequeue; may be retried
        OverBudget,      // built, but the VRAM budget was exhausted at promotion; may be retried
        Cancelled        // cancelled by the owner, by cancel_all() or by shutdown(); may be retried
    };

    State state() const { return m_state.load(); }
    bool  cancel_requested() const { return m_cancel.load(); }

    // Main thread. A queued job is dropped at dequeue, a running one stops at its next check,
    // and pending geometry that was already built is released. No effect on Promoted.
    void request_cancel();

    // Main thread, meaningful while state() == Built: the two levels (Middle, Small), either of
    // which may be empty, and their size in memory.
    const GLModel::Geometry& pending(size_t idx) const { return m_pending[idx]; }
    size_t pending_bytes() const { return m_pending_bytes; }
    bool   has_pending() const { return m_pending_bytes != 0 || !m_pending[0].vertices.empty() || !m_pending[1].vertices.empty(); }

    // Main thread. Ends a Built slot in `terminal` (Promoted or OverBudget): the pending geometry
    // is moved into out[0..1] when out is given and released otherwise, and its bytes go back to
    // the pool's account. False, without any effect, when the state is not Built.
    bool resolve_built(State terminal, GLModel::Geometry* out);

private:
    friend class MeshLodPool;

    // Worker (or the pool, for a job it never ran). Queued -> Running; false when the slot was
    // cancelled meanwhile, which leaves it Cancelled.
    bool begin_running();
    // Ends the job in `terminal`, or in Cancelled when the cancellation was requested before.
    // With terminal == Built, `built` is moved in strictly before the state is published.
    void finish(State terminal, GLModel::Geometry* built, const std::shared_ptr<MeshLodPoolSync>& sync);

    void release_pending_locked();

    std::mutex                       m_mutex;
    std::atomic<State>               m_state{Queued};
    std::atomic<bool>                m_cancel{false};
    GLModel::Geometry                m_pending[2];
    size_t                           m_pending_bytes{0};
    std::shared_ptr<MeshLodPoolSync> m_sync;   // set together with the pending geometry
};

struct MeshLodJob
{
    std::shared_ptr<const TriangleMesh> mesh;
    std::shared_ptr<MeshLodSlot>        slot;
    bool                                smooth_normals{false};
    size_t                              faces{0};   // the priority: larger meshes are built first
};

struct MeshLodPoolLimits
{
    // Workers pause while more than this much built geometry waits for the main thread.
    size_t max_unpromoted_bytes = size_t(256) << 20;
    // Meshes with more faces than this are built one at a time, whatever the thread count.
    size_t huge_mesh_faces      = 1000000;
};

class MeshLodPool
{
public:
    // Asked on the worker at dequeue with estimate_lod_job_bytes() of the job; false ends the job
    // as RejectedMemory. Has to be thread safe. Empty admits everything.
    using AdmitFn = std::function<bool(size_t job_bytes)>;
    // Test hook, called on the worker inside the job's task arena right before the build.
    using JobHook = std::function<void()>;
    // Called on the worker right after a job published its result (state Built), never for a job
    // that ended any other way. Has to be thread safe and must do no more than post a notice.
    using BuiltFn = std::function<void()>;

    using Limits = MeshLodPoolLimits;

    explicit MeshLodPool(unsigned threads, AdmitFn admit, LodParams params = {}, JobHook job_hook = {}, Limits limits = {}, BuiltFn on_built = {});
    ~MeshLodPool();   // = shutdown()
    MeshLodPool(const MeshLodPool&) = delete;
    MeshLodPool& operator=(const MeshLodPool&) = delete;

    // The methods below are called from one thread, the owner's (the main thread in the application).
    void   submit(MeshLodJob job);   // after shutdown() the job ends as Cancelled at once
    void   cancel_all();             // drops every queued job and cancels the running ones
    void   shutdown();               // cancel_all() and join; idempotent
    size_t unpromoted_bytes() const { return m_sync->unpromoted_bytes.load(); }
    size_t queued_jobs() const;
    size_t thread_count() const { return m_threads.size(); }   // 0 after shutdown()
    const LodParams& params() const { return m_params; }

private:
    void thread_function(unsigned idx);
    void worker_loop(unsigned idx);
    void run_job(const MeshLodJob& job);
    // Both with m_sync->mutex held.
    bool has_runnable_job() const;
    MeshLodJob pop_runnable_job();
    // Empties the queue and cancels the running jobs; with `stop` the workers are told to leave
    // as well, under the same lock, so that no job can start in between.
    void cancel_jobs(bool stop);

    const AdmitFn   m_admit;
    const JobHook   m_job_hook;
    const LodParams m_params;
    const Limits    m_limits;
    const BuiltFn   m_on_built;

    std::shared_ptr<MeshLodPoolSync> m_sync;
    // Guarded by m_sync->mutex. Largest first; jobs of equal size keep their order of submission.
    std::multimap<size_t, MeshLodJob, std::greater<size_t>> m_queue;
    std::vector<std::shared_ptr<MeshLodSlot>>               m_running;   // one entry per worker
    bool                                                    m_huge_running{false};
    std::atomic<bool>                                       m_stopping{false};   // written with the mutex held

    std::vector<std::thread> m_threads;
};

} // namespace GUI
} // namespace Slic3r
