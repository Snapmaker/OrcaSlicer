// Snapmaker Orca: reduced render models of the plater meshes, shared by the instances and canvases
// showing the same mesh; main thread only. Replaces upstream's g_meshVolumesMap: each mesh's MeshLod
// is owned by its GLVolumes via shared_ptr, so deleting a volume needs no unregistering.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include "libslic3r/MeshLod.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "GLModel.hpp"
#include "MeshLodPool.hpp"

class wxTimer;

namespace Slic3r {
namespace GUI {

// Video memory the promoted LOD models may use, all meshes together. Plain counters, read and
// written by the main thread only; shared by the cache and every MeshLod, whichever dies first.
struct MeshLodBudget
{
    size_t cap{size_t(512) << 20};
    size_t used{0};
};

class MeshLod
{
public:
    MeshLod(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<MeshLodBudget> budget);
    // Cancels the job if it is still queued or running, returns the charge to the budget; the
    // two models free their OpenGL buffers, which needs the same context as GLVolume::model.
    ~MeshLod();
    MeshLod(const MeshLod&) = delete;
    MeshLod& operator=(const MeshLod&) = delete;

    // The model to draw `level` with, nullptr when the full model has to be drawn: for High,
    // before the promotion and for a level the build rejected. Small falls back to Middle.
    GLModel* model(LodLevel level);

    // Takes over the geometry of a finished job. True when the models became available by this
    // call; false when there was nothing to take or when the budget is exhausted (OverBudget).
    bool try_promote();
    bool is_promoted() const { return m_slot && m_slot->state() == MeshLodSlot::Promoted; }
    // Faces of both promoted models together; lets the caller limit the work of one frame.
    size_t promoted_faces() const;

    // RejectedMemory and Cancelled always; OverBudget once the budget has room for the models.
    bool needs_retry() const;

    const std::shared_ptr<const TriangleMesh>& mesh() const { return m_mesh; }
    MeshLodSlot::State state() const { return m_slot ? m_slot->state() : MeshLodSlot::Rejected; }

private:
    friend class MeshLodCache;   // hands out the slot of the job it submits

    std::shared_ptr<const TriangleMesh> m_mesh;     // keeps the address the cache is keyed by alive
    std::shared_ptr<MeshLodSlot>        m_slot;
    std::shared_ptr<MeshLodBudget>      m_budget;   // the only link to anything outside
    GLModel                             m_models[2];
    size_t                              m_charged{0};
    size_t                              m_wanted{0};   // what the promotion that went over budget asked for
};

class MeshLodCache
{
public:
    // The application's cache: nullptr as long as none exists (command line slicing, calibration
    // helpers, unit tests) and after shutdown().
    static MeshLodCache* instance() { return s_instance; }

    // 1 worker, 2 on machines with 8 or more hardware threads; admission against the memory the
    // system can still hand out. The cache starts disabled and without a worker: the pool is
    // created by set_enabled(true), so a feature that is switched off owns no thread.
    MeshLodCache();
    // For tests: no admission rule of its own, no preference read, and the notice of a finished
    // job goes to on_built instead of the application.
    MeshLodCache(unsigned threads, MeshLodPool::AdmitFn admit, LodParams params, bool smooth_normals, MeshLodPool::BuiltFn on_built = {});
    ~MeshLodCache();
    MeshLodCache(const MeshLodCache&) = delete;
    MeshLodCache& operator=(const MeshLodCache&) = delete;

    // Enabling creates the worker pool; disabling cancels all jobs and joins the workers. Existing
    // reduced models die with their GLVolumes on the next scene reload. No effect after shutdown().
    void set_enabled(bool enabled);
    bool enabled() const { return m_enabled && !m_shut_down; }

    // nullptr while disabled, after shutdown() and for a mesh under LodParams::min_faces.
    // Otherwise the MeshLod of the mesh, created and its job submitted on the first call; a job
    // that ended in a state worth a retry is submitted again with a fresh slot.
    std::shared_ptr<MeshLod> acquire(const std::shared_ptr<const TriangleMesh>& mesh);

    // The pool exists only while the cache is enabled; all three are 0 without it.
    size_t               worker_count() const { return m_pool ? m_pool->thread_count() : 0; }
    size_t               queued_jobs() const { return m_pool ? m_pool->queued_jobs() : 0; }
    size_t               unpromoted_bytes() const { return m_pool ? m_pool->unpromoted_bytes() : 0; }
    const MeshLodBudget& budget() const { return *m_budget; }
    // Does not touch what is promoted already; a smaller cap only stops further promotions.
    void                 set_budget_cap(size_t bytes) { m_budget->cap = bytes; }
    const LodParams&     params() const { return m_params; }
    size_t               entries() const { return m_entries.size(); }

    // Main thread, called per finished job. Requests one scene pass of the shown canvas so an idle
    // scene still promotes finished models (GLVolumeCollection::update_lod()); at most one wake per
    // lod_wake_delay_ms(), after_interval waits a full one. No-op while disabled or without a plater.
    void request_canvas_wake(bool after_interval = false);

    // Cancels and joins the workers and clears instance(). Idempotent. MeshLod objects may
    // outlive it, and the cache itself.
    void shutdown();

private:
    void submit(MeshLod& lod);
    void sweep();
    void join_pool();
    void wake_canvases();

    // Keyed by address. The address cannot be reused while its entry is alive, because the MeshLod
    // owns the mesh, and an expired entry is replaced.
    std::map<const TriangleMesh*, std::weak_ptr<MeshLod>> m_entries;
    std::shared_ptr<MeshLodBudget> m_budget;
    // Shared with the notice the workers of the application's cache give (m_on_built, which is
    // why it stands before it): set by the worker that posts a wake request, cleared by the main
    // thread when it wakes the canvases, so at most one request is on its way.
    std::shared_ptr<std::atomic<bool>> m_wake_posted;
    // What the pool is created from, every time the cache is enabled.
    const unsigned                 m_threads;
    const MeshLodPool::AdmitFn     m_admit;
    const LodParams                m_params;
    const MeshLodPool::BuiltFn     m_on_built;
    std::unique_ptr<MeshLodPool>   m_pool;   // nullptr while disabled and after shutdown()
    bool                           m_enabled{false};
    bool                           m_shut_down{false};
    bool                           m_read_smooth_normals{true};   // from the preference, per job
    bool                           m_smooth_normals{false};
    unsigned                       m_acquire_calls{0};
    // The wake of the canvases.
    int64_t                            m_last_wake_ms{-1};
    std::unique_ptr<wxTimer>           m_wake_timer;   // created by the first request that has to wait

    static MeshLodCache* s_instance;
};

} // namespace GUI
} // namespace Slic3r
