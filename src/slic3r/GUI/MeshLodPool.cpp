#include "MeshLodPool.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include <boost/log/trivial.hpp>

#include <tbb/task_arena.h>

#include "libslic3r/Thread.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// Thrown by a job's cancellation callback; never leaves MeshLodPool::run_job().
struct LodCancelled {};

size_t geometry_bytes(const GLModel::Geometry& geometry)
{
    return geometry.vertices.size() * sizeof(float) + geometry.indices.size() * sizeof(unsigned int);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// MeshLodSlot
// ---------------------------------------------------------------------------------------------

void MeshLodSlot::release_pending_locked()
{
    for (GLModel::Geometry& geometry : m_pending)
        geometry = GLModel::Geometry();
    const size_t bytes = m_pending_bytes;
    m_pending_bytes = 0;
    // Lock order: the slot's mutex first, the pool's second. The pool never touches a slot while
    // it holds its own mutex.
    const std::shared_ptr<MeshLodPoolSync> sync = std::move(m_sync);
    m_sync.reset();
    if (sync && bytes > 0) {
        {
            std::lock_guard<std::mutex> lock(sync->mutex);
            sync->unpromoted_bytes -= bytes;
        }
        // The workers may be paused by the back-pressure.
        sync->cv.notify_all();
    }
}

void MeshLodSlot::request_cancel()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancel = true;
    if (m_state == Built) {
        // The worker is done with the slot; nobody will take the geometry over any more.
        release_pending_locked();
        m_state = Cancelled;
    }
}

bool MeshLodSlot::resolve_built(State terminal, GLModel::Geometry* out)
{
    assert(terminal == Promoted || terminal == OverBudget);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != Built)
        return false;
    if (out != nullptr)
        for (size_t i = 0; i < 2; ++i)
            out[i] = std::move(m_pending[i]);
    release_pending_locked();
    m_state = terminal;
    return true;
}

bool MeshLodSlot::begin_running()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != Queued)
        return false;
    if (m_cancel) {
        m_state = Cancelled;
        return false;
    }
    m_state = Running;
    return true;
}

void MeshLodSlot::finish(State terminal, GLModel::Geometry* built, const std::shared_ptr<MeshLodPoolSync>& sync)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != Queued && m_state != Running)
        return;
    if (m_cancel) {
        // Requested before this publication: the result is dropped by the caller, unseen.
        m_state = Cancelled;
        return;
    }
    if (terminal == Built) {
        assert(built != nullptr && sync);
        size_t bytes = 0;
        for (size_t i = 0; i < 2; ++i) {
            m_pending[i] = std::move(built[i]);
            bytes += geometry_bytes(m_pending[i]);
        }
        m_pending_bytes = bytes;
        m_sync          = sync;
        std::lock_guard<std::mutex> sync_lock(sync->mutex);
        sync->unpromoted_bytes += bytes;
    }
    // Everything the main thread will read is in place: publish.
    m_state = terminal;
}

// ---------------------------------------------------------------------------------------------
// MeshLodPool
// ---------------------------------------------------------------------------------------------

MeshLodPool::MeshLodPool(unsigned threads, AdmitFn admit, LodParams params, JobHook job_hook, Limits limits, BuiltFn on_built)
    : m_admit(std::move(admit))
    , m_job_hook(std::move(job_hook))
    , m_params(std::move(params))
    , m_limits(limits)
    , m_on_built(std::move(on_built))
    , m_sync(std::make_shared<MeshLodPoolSync>())
{
    threads = std::max(threads, 1u);
    m_running.resize(threads);
    m_threads.reserve(threads);
    for (unsigned idx = 0; idx < threads; ++idx)
        m_threads.emplace_back([this, idx] { this->thread_function(idx); });
}

MeshLodPool::~MeshLodPool()
{
    shutdown();
}

void MeshLodPool::submit(MeshLodJob job)
{
    assert(job.mesh && job.slot);
    if (!job.slot)
        return;
    if (!job.mesh) {
        job.slot->finish(MeshLodSlot::Rejected, nullptr, nullptr);
        return;
    }
    std::shared_ptr<MeshLodSlot> refused;
    {
        std::lock_guard<std::mutex> lock(m_sync->mutex);
        if (m_stopping)
            refused = job.slot;
        else {
            const size_t faces = job.faces;
            m_queue.emplace(faces, std::move(job));
        }
    }
    if (refused) {
        // Shut down: the job never runs.
        refused->request_cancel();
        refused->finish(MeshLodSlot::Cancelled, nullptr, nullptr);
        return;
    }
    // Every worker may be needed to find the one that is allowed to take the job.
    m_sync->cv.notify_all();
}

void MeshLodPool::cancel_jobs(bool stop)
{
    decltype(m_queue)                         dropped;
    std::vector<std::shared_ptr<MeshLodSlot>> running;
    {
        std::lock_guard<std::mutex> lock(m_sync->mutex);
        if (stop)
            m_stopping = true;
        dropped.swap(m_queue);
        for (const std::shared_ptr<MeshLodSlot>& slot : m_running)
            if (slot)
                running.emplace_back(slot);
    }
    // Outside of the pool's mutex: a slot takes its own mutex first and the pool's second.
    for (auto& [faces, job] : dropped) {
        (void) faces;
        job.slot->request_cancel();
        job.slot->finish(MeshLodSlot::Cancelled, nullptr, nullptr);
    }
    for (const std::shared_ptr<MeshLodSlot>& slot : running)
        slot->request_cancel();
    if (stop)
        m_sync->cv.notify_all();
    // `dropped` releases the meshes of the queued jobs here.
}

void MeshLodPool::cancel_all()
{
    cancel_jobs(false);
}

void MeshLodPool::shutdown()
{
    cancel_jobs(true);
    for (std::thread& thread : m_threads)
        if (thread.joinable())
            thread.join();
    m_threads.clear();
}

size_t MeshLodPool::queued_jobs() const
{
    std::lock_guard<std::mutex> lock(m_sync->mutex);
    return m_queue.size();
}

bool MeshLodPool::has_runnable_job() const
{
    if (!m_huge_running)
        return !m_queue.empty();
    // Largest first: once a job is not huge, none of the following is.
    return !m_queue.empty() && m_queue.rbegin()->first <= m_limits.huge_mesh_faces;
}

MeshLodJob MeshLodPool::pop_runnable_job()
{
    auto it = m_queue.begin();
    if (m_huge_running)
        it = m_queue.lower_bound(m_limits.huge_mesh_faces);   // descending order: the first job with faces <= limit
    assert(it != m_queue.end());
    MeshLodJob job = std::move(it->second);
    m_queue.erase(it);
    return job;
}

void MeshLodPool::thread_function(unsigned idx)
{
    // Nothing escapes a worker: an exception leaving a std::thread terminates the application.
    try {
        set_current_thread_name(("orca_lod_" + std::to_string(idx)).c_str());
        worker_loop(idx);
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "render LOD: worker " << idx << " stopped: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "render LOD: worker " << idx << " stopped by an unknown exception";
    }
}

void MeshLodPool::worker_loop(unsigned idx)
{
    for (;;) {
        MeshLodJob job;
        bool       huge = false;
        {
            std::unique_lock<std::mutex> lock(m_sync->mutex);
            m_sync->cv.wait(lock, [this] {
                return m_stopping.load() ||
                       (m_sync->unpromoted_bytes.load() <= m_limits.max_unpromoted_bytes && has_runnable_job());
            });
            if (m_stopping)
                return;
            job  = pop_runnable_job();
            huge = job.faces > m_limits.huge_mesh_faces;
            if (huge)
                m_huge_running = true;
            // Same lock as the pop: cancel_jobs() finds the job either queued or running.
            m_running[idx] = job.slot;
        }

        run_job(job);

        {
            std::lock_guard<std::mutex> lock(m_sync->mutex);
            m_running[idx].reset();
            if (huge)
                m_huge_running = false;
        }
        if (huge)
            m_sync->cv.notify_all();
        // `job` releases the mesh here, before the worker waits again.
    }
}

void MeshLodPool::run_job(const MeshLodJob& job)
{
    MeshLodSlot& slot = *job.slot;
    if (!slot.begin_running())
        return;

    MeshLodSlot::State result = MeshLodSlot::Rejected;
    GLModel::Geometry  built[2];
    const auto         start = std::chrono::steady_clock::now();
    try {
        const indexed_triangle_set& its = job.mesh->its;
        if (m_admit && !m_admit(estimate_lod_job_bytes(its.indices.size(), its.vertices.size()))) {
            BOOST_LOG_TRIVIAL(info) << "render LOD: a mesh of " << its.indices.size()
                                    << " faces is not admitted, too little memory is available";
            result = MeshLodSlot::RejectedMemory;
        } else {
            // Called from inside the simplification as well; reads atomics only.
            const std::function<void()> throw_on_cancel = [this, &slot] {
                if (slot.cancel_requested() || m_stopping.load())
                    throw LodCancelled();
            };

            LodMeshes meshes;
            // One slot, taken by this thread: the tbb::parallel_for loops of the simplification
            // stay on the worker instead of spreading over the cores that slicing may be using.
            tbb::task_arena arena(1);
            arena.execute([this, &job, &meshes, &throw_on_cancel] {
                if (m_job_hook)
                    m_job_hook();
                meshes = build_lod_meshes(job.mesh->its, m_params, throw_on_cancel);
            });

            throw_on_cancel();
            if (!meshes.middle.indices.empty())
                built[0] = GLModel::make_geometry(meshes.middle, job.smooth_normals);
            meshes.middle = indexed_triangle_set();
            throw_on_cancel();
            if (!meshes.small_mesh.indices.empty())
                built[1] = GLModel::make_geometry(meshes.small_mesh, job.smooth_normals);
            meshes.small_mesh = indexed_triangle_set();

            if (!built[0].is_empty() || !built[1].is_empty())
                result = MeshLodSlot::Built;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (result == MeshLodSlot::Built)
                BOOST_LOG_TRIVIAL(info) << "render LOD: a mesh of " << its.indices.size() << " faces reduced in " << ms << " ms";
            else
                BOOST_LOG_TRIVIAL(info) << "render LOD: a mesh of " << its.indices.size() << " faces yields no usable reduced copy (" << ms << " ms)";
        }
    } catch (const LodCancelled&) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        BOOST_LOG_TRIVIAL(info) << "render LOD: the job of a mesh of " << job.faces << " faces cancelled after " << ms << " ms";
        result = MeshLodSlot::Cancelled;
    } catch (const std::exception& ex) {
        // std::bad_alloc included: the mesh simply keeps its full detail.
        BOOST_LOG_TRIVIAL(error) << "render LOD: building the reduced meshes failed: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "render LOD: building the reduced meshes failed by an unknown exception";
    }

    slot.finish(result, built, m_sync);

    // The result waits for the main thread now. A job whose cancellation came first ended
    // Cancelled inside finish() and tells nobody.
    if (result == MeshLodSlot::Built && m_on_built && !slot.cancel_requested()) {
        try {
            m_on_built();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "render LOD: the notice of a finished job failed";
        }
    }
}

} // namespace GUI
} // namespace Slic3r
