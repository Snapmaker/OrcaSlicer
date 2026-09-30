// Snapmaker Orca: level of detail for the 3D scene, the GUI-less part. Thresholds, level rule,
// error budgets and rejection rules follow upstream #737 (3ca60288d6); the functions are pure,
// independent of GLVolume, threads and OpenGL, so they are unit tested and run on any thread.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "BoundingBox.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

enum class LodLevel : unsigned char { High = 0, Middle = 1, Small = 2 };

struct LodParams
{
    // Projected size of the world AABB in pixels.
    Vec2i32 screen_min{150, 110};       // <= BOTH   -> Small (upstream LOD_SCREEN_MIN)
    Vec2i32 screen_max{300, 200};       // >= EITHER -> High  (upstream LOD_SCREEN_MAX)
    // Both thresholds are multiplied by it, so that a HiDPI framebuffer switches levels at the
    // same apparent size as a standard one.
    float   pixel_scale       = 1.f;
    // Relative band around both thresholds. A level is left only once the size has crossed the
    // threshold by this fraction, which keeps an object sitting on a threshold from flickering.
    float   hysteresis        = 0.10f;
    // Meshes with fewer faces get no reduced copies at all.
    size_t  min_faces         = 20000;
    // Size classes of the source: normal, large (> large_faces), super large (> super_large_faces).
    size_t  large_faces       = 100000;
    size_t  super_large_faces = 500000;
    // Error budgets of its_quadric_edge_collapse per size class (normal / large / super large).
    // They are quadric values, that is squared distances, not millimetres.
    float   middle_err[3]     = {0.1f, 0.05f, 0.08f};
    float   small_err[3]      = {0.5f, 0.3f, 0.4f};
    // Middle is kept when it has at most this fraction of the source's faces; Small is kept when
    // it has at most this fraction of Middle's faces (of the source's when Middle was rejected).
    float   min_reduction     = 0.7f;
    // A reduced mesh has to stay inside the source AABB grown by this much on every side.
    float   aabb_epsilon      = 1.0f;
};

// Pure. Compares the projected pixel size of world_aabb with the thresholds, relative to prev.
// High for an undefined box, an empty viewport or a corner at/behind the eye (clip w <= 1e-6).
// view_proj.matrix() = projection.matrix() * view.matrix(); an affine product drops perspective.
LodLevel select_lod_level(const BoundingBoxf3 &world_aabb, const Transform3d &view_proj,
                          int viewport_w, int viewport_h, LodLevel prev, const LodParams &params = {});

struct LodMeshes
{
    indexed_triangle_set middle, small_mesh;
    // Error of the last collapsed edge, as reported back by its_quadric_edge_collapse.
    float                middle_error{0.f}, small_error{0.f};
};

// Pure, cancellable. Both levels are simplified from src, sequentially, with independent budgets.
// A level stays empty below min_faces, when a vertex leaves the grown AABB or min_reduction is missed.
// throw_on_cancel (may be empty) cancels by throwing, also from tbb::parallel_for worker threads.
LodMeshes build_lod_meshes(const indexed_triangle_set &src, const LodParams &params,
                           const std::function<void()> &throw_on_cancel);

// Pure. Height above which volumes stay at full detail: the lowest extruder_printable_height, as
// the shader darkens what exceeds it. DBL_MAX with fewer than two entries or no value; nil (NaN)
// entries are skipped.
inline double lod_pin_height(const std::vector<double> &extruder_printable_heights)
{
    if (extruder_printable_heights.size() <= 1)
        return DBL_MAX;
    double lowest = DBL_MAX;
    for (const double height : extruder_printable_heights)
        if (!std::isnan(height) && height < lowest)
            lowest = height;
    return lowest;
}

// Pure. Paces the resubmission of reduced-model jobs worth a retry (cancelled, not admitted, over
// the VRAM budget). The interval doubles up to 16 s while passes resubmit, and resets to 1 s once
// one finds nothing. Milliseconds of any monotonic clock.
struct LodRetryTimer
{
    static constexpr int64_t min_interval_ms = 1000;
    static constexpr int64_t max_interval_ms = 16000;

    // Never while the feature is off: a disabled cache hands out nothing.
    bool due(bool cache_enabled, int64_t now_ms) const
    {
        return cache_enabled && (m_last_ms < 0 || now_ms - m_last_ms >= m_interval_ms);
    }
    // After a pass that was due; resubmitted is the number of jobs it submitted again.
    void passed(int64_t now_ms, size_t resubmitted)
    {
        m_last_ms     = now_ms;
        m_interval_ms = resubmitted > 0 ? std::min<int64_t>(m_interval_ms * 2, max_interval_ms) : min_interval_ms;
    }
    int64_t interval_ms() const { return m_interval_ms; }

private:
    int64_t m_last_ms{-1};
    int64_t m_interval_ms{min_interval_ms};
};

// Pure. A finished job wakes the canvas to take its models over, at most once per min_interval_ms.
// Returns the delay for a request at now_ms (0: now); last_wake_ms < 0 means none yet.
inline int64_t lod_wake_delay_ms(int64_t now_ms, int64_t last_wake_ms, int64_t min_interval_ms = 500)
{
    if (last_wake_ms < 0 || now_ms < last_wake_ms)
        return 0;
    const int64_t elapsed = now_ms - last_wake_ms;
    return elapsed >= min_interval_ms ? 0 : min_interval_ms - elapsed;
}

// Estimate of the transient memory peak of one build_lod_meshes() call followed by the
// conversion of its results into render geometry. Monotonic in both arguments.
size_t estimate_lod_job_bytes(size_t faces, size_t vertices);

} // namespace Slic3r
