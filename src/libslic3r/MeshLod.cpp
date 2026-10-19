// Snapmaker Orca: level of detail for the 3D scene, the GUI-less part. See MeshLod.hpp.
#include "MeshLod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "QuadricEdgeCollapse.hpp"

namespace Slic3r {

namespace {

// Upstream's FINAL_FACE_LOW_COUNT and QEM_FACE_RATIO: a small source that lost more than half of
// its faces is not trusted to still look like itself.
constexpr size_t LOW_FACE_COUNT       = 1000;
constexpr float  LOW_FACE_MIN_KEPT    = 0.5f;
// A corner this close to the eye plane (or behind it) has no meaningful projection.
constexpr double MIN_CLIP_W           = 1e-6;

size_t size_class(size_t faces, const LodParams &params)
{
    return faces > params.super_large_faces ? 2 : faces > params.large_faces ? 1 : 0;
}

bool inside_grown_aabb(const indexed_triangle_set &its, const Vec3f &bb_min, const Vec3f &bb_max)
{
    for (const stl_vertex &v : its.vertices)
        for (int axis = 0; axis < 3; ++axis)
            if (!(v[axis] > bb_min[axis] && v[axis] < bb_max[axis]))
                return false;
    return true;
}

} // namespace

LodLevel select_lod_level(const BoundingBoxf3 &world_aabb, const Transform3d &view_proj,
                          int viewport_w, int viewport_h, LodLevel prev, const LodParams &params)
{
    if (!world_aabb.defined || viewport_w <= 0 || viewport_h <= 0)
        return LodLevel::High;

    const Eigen::Matrix4d &m = view_proj.matrix();
    double min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest();
    double min_y = min_x, max_y = max_x;
    for (int i = 0; i < 8; ++i) {
        const Eigen::Vector4d corner((i & 1) ? world_aabb.max.x() : world_aabb.min.x(),
                                     (i & 2) ? world_aabb.max.y() : world_aabb.min.y(),
                                     (i & 4) ? world_aabb.max.z() : world_aabb.min.z(), 1.);
        const Eigen::Vector4d clip = m * corner;
        if (!(clip.w() > MIN_CLIP_W))
            // At or behind the eye plane (or not a number): the object is around the camera.
            return LodLevel::High;
        const double x = 0.5 * (1. + clip.x() / clip.w()) * viewport_w;
        const double y = 0.5 * (1. - clip.y() / clip.w()) * viewport_h;
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
    }
    const double size_x = max_x - min_x;
    const double size_y = max_y - min_y;
    if (!std::isfinite(size_x) || !std::isfinite(size_y))
        return LodLevel::High;

    // Hysteresis: the threshold that would make the level leave `prev` is pushed away by the
    // band, so High is kept down to (1 - h) * max, Small up to (1 + h) * min, and Middle is left
    // only beyond the band on either side. With hysteresis == 0 this is upstream's rule.
    const double h          = std::clamp(double(params.hysteresis), 0., 0.9);
    const double scale      = params.pixel_scale > 0.f ? double(params.pixel_scale) : 1.;
    const double max_factor = scale * (prev == LodLevel::High  ? 1. - h : 1. + h);
    const double min_factor = scale * (prev == LodLevel::Small ? 1. + h : 1. - h);

    // Upstream's order: the High test comes first and needs EITHER axis, Small needs BOTH.
    if (size_x >= params.screen_max.x() * max_factor || size_y >= params.screen_max.y() * max_factor)
        return LodLevel::High;
    if (size_x <= params.screen_min.x() * min_factor && size_y <= params.screen_min.y() * min_factor)
        return LodLevel::Small;
    return LodLevel::Middle;
}

LodMeshes build_lod_meshes(const indexed_triangle_set &src, const LodParams &params,
                           const std::function<void()> &throw_on_cancel)
{
    LodMeshes out;
    const size_t src_faces = src.indices.size();
    if (src_faces == 0 || src.vertices.empty() || src_faces < params.min_faces)
        return out;

    const std::function<void()> check_cancel = throw_on_cancel ? throw_on_cancel : std::function<void()>([] {});
    check_cancel();

    // The source AABB comes straight from the vertices; no TriangleMesh copy is made for it.
    Vec3f bb_min = src.vertices.front(), bb_max = src.vertices.front();
    for (const stl_vertex &v : src.vertices) {
        bb_min = bb_min.cwiseMin(v);
        bb_max = bb_max.cwiseMax(v);
    }
    bb_min -= Vec3f::Constant(params.aabb_epsilon);
    bb_max += Vec3f::Constant(params.aabb_epsilon);

    // Simplifies a copy of the source. Returns an empty set when the result is rejected;
    // `reference_faces` is the face count the reduction is measured against.
    auto simplify = [&](float budget, size_t reference_faces, float &error_out) {
        indexed_triangle_set work = src;
        float                error = budget;
        its_quadric_edge_collapse(work, 0, &error, check_cancel);
        check_cancel();
        error_out = error;

        const size_t faces = work.indices.size();
        const bool   rejected =
            faces == 0 ||
            (src_faces < LOW_FACE_COUNT && float(faces) < float(src_faces) * LOW_FACE_MIN_KEPT) ||
            float(faces) > float(reference_faces) * params.min_reduction ||
            !inside_grown_aabb(work, bb_min, bb_max);
        if (rejected)
            work = indexed_triangle_set();
        return work;
    };

    const size_t cls = size_class(src_faces, params);
    out.middle     = simplify(params.middle_err[cls], src_faces, out.middle_error);
    out.small_mesh = simplify(params.small_err[cls], out.middle.indices.empty() ? src_faces : out.middle.indices.size(),
                              out.small_error);
    return out;
}

size_t estimate_lod_job_bytes(size_t faces, size_t vertices)
{
    // Per face: working copy and result (2 x 12), its_quadric_edge_collapse tables (64), the kept
    // Middle result and the render geometry of both levels. Per vertex: two positions and a
    // quadric with its neighbour range (88). Rounded up: an estimate must not be too low.
    constexpr size_t BYTES_PER_FACE   = 200;
    constexpr size_t BYTES_PER_VERTEX = 128;
    return faces * BYTES_PER_FACE + vertices * BYTES_PER_VERTEX;
}

} // namespace Slic3r
