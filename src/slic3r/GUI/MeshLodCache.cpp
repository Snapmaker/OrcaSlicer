#include "MeshLodCache.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <thread>
#include <utility>

#include <boost/log/trivial.hpp>

#include <wx/timer.h>

#include "libslic3r/AppConfig.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "../Utils/CpuMemory.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// Memory that has to stay available beyond the estimated peak of a job.
constexpr uint64_t ADMISSION_HEADROOM_BYTES = uint64_t(2) << 30;
// acquire() removes the expired entries on every N-th call.
constexpr unsigned SWEEP_PERIOD = 64;

size_t video_memory_bytes(const GLModel::Geometry& geometry)
{
    return geometry.is_empty() ? 0 : geometry.vertices_size_bytes() + geometry.indices_size_bytes();
}

// Runs on a worker, at dequeue.
bool admit_by_available_memory(size_t job_bytes)
{
    const uint64_t available = CpuMemory::available_bytes();
    return available == 0 || available > uint64_t(job_bytes) + ADMISSION_HEADROOM_BYTES;
}

unsigned default_thread_count()
{
    return std::thread::hardware_concurrency() >= 8 ? 2 : 1;
}

size_t to_mib(size_t bytes) { return bytes >> 20; }

int64_t steady_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Runs on a cache worker after a job published its result and only posts to the event queue; the
// posted call reaches the cache through instance(), cleared by shutdown(). The workers are joined
// before the application object dies, so wxGetApp() stays valid here.
void post_canvas_wake(const std::shared_ptr<std::atomic<bool>>& posted)
{
    if (wxTheApp == nullptr)
        return;   // a cache without an application (unit test)
    if (posted->exchange(true))
        return;   // a request is on its way; it covers this job as well
    wxGetApp().CallAfter([] {
        if (MeshLodCache* cache = MeshLodCache::instance(); cache != nullptr)
            cache->request_canvas_wake();
    });
}

} // namespace

// ---------------------------------------------------------------------------------------------
// MeshLod
// ---------------------------------------------------------------------------------------------

MeshLod::MeshLod(std::shared_ptr<const TriangleMesh> mesh, std::shared_ptr<MeshLodBudget> budget)
    : m_mesh(std::move(mesh)), m_budget(std::move(budget))
{
    assert(m_mesh && m_budget);
}

MeshLod::~MeshLod()
{
    // Nothing here reaches the cache or the pool: the slot and the budget are shared objects of
    // their own, so this may run after both are gone.
    if (m_slot)
        m_slot->request_cancel();
    m_budget->used -= std::min(m_charged, m_budget->used);
}

GLModel* MeshLod::model(LodLevel level)
{
    if (level == LodLevel::High || !is_promoted())
        return nullptr;
    if (level == LodLevel::Small && m_models[1].is_initialized())
        return &m_models[1];
    return m_models[0].is_initialized() ? &m_models[0] : nullptr;
}

bool MeshLod::try_promote()
{
    if (!m_slot || m_slot->state() != MeshLodSlot::Built)
        return false;

    const size_t wanted = video_memory_bytes(m_slot->pending(0)) + video_memory_bytes(m_slot->pending(1));
    if (m_budget->used + wanted > m_budget->cap) {
        m_wanted = wanted;
        m_slot->resolve_built(MeshLodSlot::OverBudget, nullptr);
        BOOST_LOG_TRIVIAL(info) << "render LOD: a mesh of " << m_mesh->its.indices.size() << " faces stays at full detail, its reduced models need "
                                << to_mib(wanted) << " MiB and " << to_mib(m_budget->used) << " of " << to_mib(m_budget->cap) << " MiB are in use";
        return false;
    }

    GLModel::Geometry geometry[2];
    if (!m_slot->resolve_built(MeshLodSlot::Promoted, geometry))
        return false;
    for (size_t i = 0; i < 2; ++i)
        if (!geometry[i].is_empty()) {
            if (m_models[i].is_initialized())
                m_models[i].reset();
            m_models[i].init_from(std::move(geometry[i]));
        }
    m_charged      = wanted;
    m_budget->used += wanted;
    BOOST_LOG_TRIVIAL(info) << "render LOD: " << m_mesh->its.indices.size() << " faces reduced to " << m_models[0].indices_count() / 3 << " and "
                            << m_models[1].indices_count() / 3 << ", " << to_mib(m_budget->used) << " of " << to_mib(m_budget->cap) << " MiB in use";
    return true;
}

size_t MeshLod::promoted_faces() const
{
    return is_promoted() ? (m_models[0].indices_count() + m_models[1].indices_count()) / 3 : 0;
}

bool MeshLod::needs_retry() const
{
    switch (state()) {
    case MeshLodSlot::RejectedMemory:
    case MeshLodSlot::Cancelled:
        return true;
    case MeshLodSlot::OverBudget:
        // Building again is only worth it once the models would fit.
        return m_budget->used + m_wanted <= m_budget->cap;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// MeshLodCache
// ---------------------------------------------------------------------------------------------

MeshLodCache* MeshLodCache::s_instance = nullptr;

MeshLodCache::MeshLodCache()
    : m_budget(std::make_shared<MeshLodBudget>())
    , m_wake_posted(std::make_shared<std::atomic<bool>>(false))
    , m_threads(default_thread_count())
    , m_admit(admit_by_available_memory)
    , m_params()
    // The notice holds the flag, not the cache.
    , m_on_built([posted = m_wake_posted] { post_canvas_wake(posted); })
{
    assert(s_instance == nullptr);
    s_instance = this;
}

MeshLodCache::MeshLodCache(unsigned threads, MeshLodPool::AdmitFn admit, LodParams params, bool smooth_normals, MeshLodPool::BuiltFn on_built)
    : m_budget(std::make_shared<MeshLodBudget>())
    , m_wake_posted(std::make_shared<std::atomic<bool>>(false))
    , m_threads(std::max(threads, 1u))
    , m_admit(std::move(admit))
    , m_params(std::move(params))
    , m_on_built(std::move(on_built))
    , m_read_smooth_normals(false)
    , m_smooth_normals(smooth_normals)
{
    // Not the application's cache: instance() stays as it is.
}

MeshLodCache::~MeshLodCache()
{
    shutdown();
}

void MeshLodCache::set_enabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (m_shut_down)
        return;
    if (enabled) {
        // The workers exist only while the feature is on.
        m_pool = std::make_unique<MeshLodPool>(m_threads, m_admit, m_params, MeshLodPool::JobHook(), MeshLodPool::Limits(), m_on_built);
        BOOST_LOG_TRIVIAL(info) << "render LOD: enabled, " << m_pool->thread_count() << " worker(s), " << to_mib(m_budget->cap)
                                << " MiB for the reduced models";
    } else {
        join_pool();
        BOOST_LOG_TRIVIAL(info) << "render LOD: disabled, workers joined";
    }
}

void MeshLodCache::join_pool()
{
    if (!m_pool)
        return;
    // Cancels what is queued or running, then joins. The slots stay with their owners.
    m_pool->shutdown();
    m_pool.reset();
}

std::shared_ptr<MeshLod> MeshLodCache::acquire(const std::shared_ptr<const TriangleMesh>& mesh)
{
    if (!enabled() || !m_pool || !mesh || mesh->its.indices.size() < m_params.min_faces)
        return nullptr;

    if (++m_acquire_calls % SWEEP_PERIOD == 0)
        sweep();

    std::weak_ptr<MeshLod>& entry = m_entries[mesh.get()];
    std::shared_ptr<MeshLod> lod  = entry.lock();
    if (!lod) {
        lod   = std::make_shared<MeshLod>(mesh, m_budget);
        entry = lod;
        submit(*lod);
    } else if (lod->needs_retry())
        submit(*lod);
    return lod;
}

void MeshLodCache::submit(MeshLod& lod)
{
    if (m_read_smooth_normals)
        // The preference GLModel::init_from() reads for the full model, read on the main thread.
        m_smooth_normals = wxTheApp != nullptr && wxGetApp().app_config != nullptr &&
                           wxGetApp().app_config->get_bool(SETTING_OPENGL_PHONG_SMOOTH_NORMALS);

    // The previous slot, if any, is in a final state; a fresh one keeps its history out of the new job.
    lod.m_slot   = std::make_shared<MeshLodSlot>();
    lod.m_wanted = 0;

    MeshLodJob job;
    job.mesh           = lod.m_mesh;
    job.slot           = lod.m_slot;
    job.smooth_normals = m_smooth_normals;
    job.faces          = lod.m_mesh->its.indices.size();
    BOOST_LOG_TRIVIAL(info) << "render LOD: a mesh of " << job.faces << " faces queued, " << m_pool->queued_jobs() << " waiting before it";
    m_pool->submit(std::move(job));
}

void MeshLodCache::request_canvas_wake(bool after_interval)
{
    if (m_shut_down || wxTheApp == nullptr)
        return;
    const int64_t delay_ms = after_interval ? int64_t(500) : lod_wake_delay_ms(steady_now_ms(), m_last_wake_ms);
    if (delay_ms <= 0) {
        wake_canvases();
        return;
    }
    if (!m_wake_timer) {
        m_wake_timer = std::make_unique<wxTimer>();
        m_wake_timer->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { this->wake_canvases(); });
    }
    // A timer that runs already fires early enough for this request as well.
    if (!m_wake_timer->IsRunning())
        m_wake_timer->StartOnce(int(delay_ms));
}

void MeshLodCache::wake_canvases()
{
    // Cleared first: a job that finishes from here on posts a request of its own, and one that
    // finished before is Built already and is taken over by the scene pass asked for below.
    m_wake_posted->store(false);
    m_last_wake_ms = steady_now_ms();
    if (!enabled() || wxTheApp == nullptr)
        return;
    GUI_App& app = wxGetApp();
    // A closing application draws nothing any more; a GUI that is recreated reloads and draws
    // its scenes anyway, and its plater pointer is not to be trusted meanwhile.
    if (app.is_closing() || app.is_recreating_gui() || app.plater() == nullptr)
        return;
    Plater* plater = app.plater();
    for (GLCanvas3D* canvas : { plater->get_view3D_canvas3D(), plater->get_assmeble_canvas3D(), plater->get_preview_canvas3D() })
        if (canvas != nullptr)
            canvas->set_as_dirty();
    // Only the canvas that is shown draws, once, in the next idle event.
    wxWakeUpIdle();
    BOOST_LOG_TRIVIAL(info) << "render LOD: reduced models wait to be taken over, the 3D canvases are asked for one scene pass";
}

void MeshLodCache::sweep()
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
        it = it->second.expired() ? m_entries.erase(it) : std::next(it);
}

void MeshLodCache::shutdown()
{
    if (m_shut_down)
        return;
    m_shut_down = true;
    const bool had_pool = m_pool != nullptr;
    join_pool();
    // The timer calls into this object.
    m_wake_timer.reset();
    m_entries.clear();
    if (s_instance == this)
        s_instance = nullptr;
    if (had_pool)
        BOOST_LOG_TRIVIAL(info) << "render LOD: shut down, workers joined";
}

} // namespace GUI
} // namespace Slic3r
