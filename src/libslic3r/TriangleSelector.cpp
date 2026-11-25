#include "TriangleSelector.hpp"
#include "Model.hpp"
#include "AABBTreeIndirect.hpp"

#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <boost/log/trivial.hpp>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <tbb/parallel_for.h>

#ifndef NDEBUG
//    #define EXPENSIVE_DEBUG_CHECKS
#endif // NDEBUG

namespace Slic3r {

void TriangleSelector::HeightRangeIndex::Build(const TriangleMesh& mesh, const Transform3d& effectiveTransform)
{
    constexpr size_t MAX_BUCKET_COUNT = 65536;
    constexpr size_t MAX_BUCKETS_PER_ROOT = 32;
    constexpr size_t MAX_REFERENCES_PER_ROOT_BUDGET = 4;

    *this = HeightRangeIndex();
    m_mesh = &mesh;
    m_effectiveTransform = effectiveTransform;
    m_initialized = true;

    const indexed_triangle_set& indexedTriangles = mesh.its;
    const size_t rootCount = indexedTriangles.indices.size();
    if (rootCount == 0)
        return;

    const float infinity = std::numeric_limits<float>::infinity();
    m_rootZSpans.assign(rootCount, std::make_pair(infinity, -infinity));
    bool hasFiniteSpan = false;
    float minimumZ = std::numeric_limits<float>::max();
    float maximumZ = std::numeric_limits<float>::lowest();

    for (size_t rootIndex = 0; rootIndex < rootCount; ++rootIndex)
    {
        const stl_triangle_vertex_indices& triangle = indexedTriangles.indices[rootIndex];
        float rootMinimumZ = std::numeric_limits<float>::max();
        float rootMaximumZ = std::numeric_limits<float>::lowest();
        bool validSpan = true;

        for (int vertexOffset = 0; vertexOffset < 3; ++vertexOffset)
        {
            const int vertexIndex = triangle(vertexOffset);
            if (vertexIndex < 0 || vertexIndex >= static_cast<int>(indexedTriangles.vertices.size()))
            {
                validSpan = false;
                break;
            }

            const stl_vertex& vertex = indexedTriangles.vertices[vertexIndex];
            const Vec3d worldVertex = effectiveTransform * Vec3d(vertex.x(), vertex.y(), vertex.z());
            if (!worldVertex.allFinite())
            {
                validSpan = false;
                break;
            }

            const float worldZ = static_cast<float>(worldVertex.z());
            if (!std::isfinite(worldZ))
            {
                validSpan = false;
                break;
            }

            rootMinimumZ = std::min(rootMinimumZ, worldZ);
            rootMaximumZ = std::max(rootMaximumZ, worldZ);
        }

        if (!validSpan)
        {
            m_rootZSpans[rootIndex] = std::make_pair(-infinity, infinity);
            continue;
        }

        m_rootZSpans[rootIndex] = std::make_pair(rootMinimumZ, rootMaximumZ);
        minimumZ = std::min(minimumZ, rootMinimumZ);
        maximumZ = std::max(maximumZ, rootMaximumZ);
        hasFiniteSpan = true;
    }

    if (hasFiniteSpan)
    {
        m_minZ = minimumZ;
        m_maxZ = maximumZ;
    }

    const size_t targetBucketCount = std::max<size_t>(1, std::min(rootCount, MAX_BUCKET_COUNT));
    const double zExtent = static_cast<double>(m_maxZ) - static_cast<double>(m_minZ);
    if (targetBucketCount > 1 && std::floor(zExtent / static_cast<double>(m_bucketSize)) + 1.0 > targetBucketCount)
    {
        const double requiredBucketSize = zExtent / static_cast<double>(targetBucketCount - 1);
        m_bucketSize = requiredBucketSize > static_cast<double>(std::numeric_limits<float>::max()) ?
            std::numeric_limits<float>::max() : std::max(static_cast<float>(requiredBucketSize), std::numeric_limits<float>::epsilon());
    }

    size_t bucketCount = 1;
    if (targetBucketCount > 1 && zExtent > 0.0)
    {
        const double calculatedBucketCount = std::floor(zExtent / static_cast<double>(m_bucketSize)) + 1.0;
        bucketCount = static_cast<size_t>(std::min(static_cast<double>(targetBucketCount), calculatedBucketCount));
    }
    m_buckets.resize(bucketCount);
    m_sourceQueryGeneration.assign(rootCount, 0);
    m_queryCandidates.reserve(std::min<size_t>(rootCount, 4096));

    const size_t maxTotalReferences = rootCount > std::numeric_limits<size_t>::max() / MAX_REFERENCES_PER_ROOT_BUDGET ?
        std::numeric_limits<size_t>::max() : rootCount * MAX_REFERENCES_PER_ROOT_BUDGET;
    for (size_t rootIndex = 0; rootIndex < rootCount; ++rootIndex)
    {
        const std::pair<float, float>& zSpan = m_rootZSpans[rootIndex];
        if (!std::isfinite(zSpan.first) || !std::isfinite(zSpan.second))
        {
            m_longSpanRoots.push_back(static_cast<uint32_t>(rootIndex));
            continue;
        }

        const size_t firstBucket = BucketIndex(zSpan.first);
        const size_t lastBucket = BucketIndex(zSpan.second);
        const size_t spanBucketCount = lastBucket - firstBucket + 1;
        if (spanBucketCount > MAX_BUCKETS_PER_ROOT || spanBucketCount > maxTotalReferences - m_totalBucketReferences)
        {
            m_longSpanRoots.push_back(static_cast<uint32_t>(rootIndex));
            continue;
        }

        for (size_t bucketIndex = firstBucket; bucketIndex <= lastBucket; ++bucketIndex)
            m_buckets[bucketIndex].push_back(static_cast<uint32_t>(rootIndex));
        m_totalBucketReferences += spanBucketCount;
    }
}

bool TriangleSelector::HeightRangeIndex::Matches(const TriangleMesh& mesh, const Transform3d& effectiveTransform) const
{
    return m_initialized && m_mesh == &mesh &&
           (m_effectiveTransform.matrix().array() == effectiveTransform.matrix().array()).all();
}

size_t TriangleSelector::HeightRangeIndex::BucketIndex(float zWorld) const
{
    if (m_buckets.size() <= 1 || zWorld <= m_minZ)
        return 0;
    if (zWorld >= m_maxZ)
        return m_buckets.size() - 1;

    const double bucketIndex = std::floor(
        (static_cast<double>(zWorld) - static_cast<double>(m_minZ)) / static_cast<double>(m_bucketSize));
    return std::min(static_cast<size_t>(std::max(0.0, bucketIndex)), m_buckets.size() - 1);
}

const std::vector<uint32_t>& TriangleSelector::HeightRangeIndex::Query(float zBottomWorld, float zTopWorld)
{
    constexpr float QUERY_TOLERANCE = 0.02f;

    m_queryCandidates.clear();
    if (!m_initialized || m_rootZSpans.empty() || !std::isfinite(zBottomWorld) || !std::isfinite(zTopWorld))
        return m_queryCandidates;

    if (zBottomWorld > zTopWorld)
        std::swap(zBottomWorld, zTopWorld);

    if (m_queryGeneration == std::numeric_limits<uint32_t>::max())
    {
        std::fill(m_sourceQueryGeneration.begin(), m_sourceQueryGeneration.end(), 0);
        m_queryGeneration = 1;
    }
    else
    {
        ++m_queryGeneration;
    }

    const float queryMinimumZ = zBottomWorld - QUERY_TOLERANCE;
    const float queryMaximumZ = zTopWorld + QUERY_TOLERANCE;
    const auto appendCandidate = [this, queryMinimumZ, queryMaximumZ](uint32_t sourceRoot)
    {
        if (sourceRoot >= m_rootZSpans.size() || m_sourceQueryGeneration[sourceRoot] == m_queryGeneration)
            return;

        m_sourceQueryGeneration[sourceRoot] = m_queryGeneration;
        const std::pair<float, float>& zSpan = m_rootZSpans[sourceRoot];
        if (zSpan.second < queryMinimumZ || zSpan.first > queryMaximumZ)
            return;

        m_queryCandidates.push_back(sourceRoot);
    };

    if (!m_buckets.empty() && queryMaximumZ >= m_minZ && queryMinimumZ <= m_maxZ)
    {
        const size_t firstBucket = BucketIndex(queryMinimumZ);
        const size_t lastBucket = BucketIndex(queryMaximumZ);
        for (size_t bucketIndex = firstBucket; bucketIndex <= lastBucket; ++bucketIndex)
            for (uint32_t sourceRoot : m_buckets[bucketIndex])
                appendCandidate(sourceRoot);
    }

    for (uint32_t sourceRoot : m_longSpanRoots)
        appendCandidate(sourceRoot);

    return m_queryCandidates;
}

// Check if the line is whole inside the sphere, or it is partially inside (intersecting) the sphere.
// Inspired by Christer Ericson's Real-Time Collision Detection, pp. 177-179.
static bool test_line_inside_sphere(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &sphere_p, const float sphere_radius)
{
    const float sphere_radius_sqr = Slic3r::sqr(sphere_radius);
    const Vec3f line_dir          = line_b - line_a;   // n
    const Vec3f origins_diff      = line_a - sphere_p; // m

    const float m_dot_m           = origins_diff.dot(origins_diff);
    // Check if any of the end-points of the line is inside the sphere.
    if (m_dot_m <= sphere_radius_sqr || (line_b - sphere_p).squaredNorm() <= sphere_radius_sqr)
        return true;

    // Check if the infinite line is going through the sphere.
    const float n_dot_n = line_dir.dot(line_dir);
    const float m_dot_n = origins_diff.dot(line_dir);

    const float eq_a    = n_dot_n;
    const float eq_b    = m_dot_n;
    const float eq_c    = m_dot_m - sphere_radius_sqr;

    const float discr = eq_b * eq_b - eq_a * eq_c;
    // A negative discriminant corresponds to the infinite line infinite not going through the sphere.
    if (discr < 0.f)
        return false;

    // Check if the finite line is going through the sphere.
    const float discr_sqrt = std::sqrt(discr);
    const float t1         = (-eq_b - discr_sqrt) / eq_a;
    if (0.f <= t1 && t1 <= 1.f)
        return true;

    const float t2 = (-eq_b + discr_sqrt) / eq_a;
    if (0.f <= t2 && t2 <= 1.f && discr_sqrt > 0.f)
        return true;

    return false;
}

// Check if the line is whole inside the finite cylinder, or it is partially inside (intersecting) the finite cylinder.
// Inspired by Christer Ericson's Real-Time Collision Detection, pp. 194-198.
static bool test_line_inside_cylinder(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &cylinder_P, const Vec3f &cylinder_Q, const float cylinder_radius)
{
    assert(cylinder_P != cylinder_Q);
    const Vec3f cylinder_dir                    = cylinder_Q - cylinder_P; // d
    auto        is_point_inside_finite_cylinder = [&cylinder_P, &cylinder_Q, &cylinder_radius, &cylinder_dir](const Vec3f &pt) {
        const Vec3f first_center_diff  = cylinder_P - pt;
        const Vec3f second_center_diff = cylinder_Q - pt;
        // First, check if the point pt is laying between planes defined by cylinder_p and cylinder_q.
        // Then check if it is inside the cylinder between cylinder_p and cylinder_q.
        return first_center_diff.dot(cylinder_dir) <= 0 && second_center_diff.dot(cylinder_dir) >= 0 &&
               (first_center_diff.cross(cylinder_dir).norm() / cylinder_dir.norm()) <= cylinder_radius;
    };

    // Check if any of the end-points of the line is inside the cylinder.
    if (is_point_inside_finite_cylinder(line_a) || is_point_inside_finite_cylinder(line_b))
       return true;

    // Check if the line is going through the cylinder.
    const Vec3f origins_diff = line_a - cylinder_P;     // m
    const Vec3f line_dir     = line_b - line_a;         // n

    const float m_dot_d = origins_diff.dot(cylinder_dir);
    const float n_dot_d = line_dir.dot(cylinder_dir);
    const float d_dot_d = cylinder_dir.dot(cylinder_dir);

    const float n_dot_n = line_dir.dot(line_dir);
    const float m_dot_n = origins_diff.dot(line_dir);
    const float m_dot_m = origins_diff.dot(origins_diff);

    const float eq_a    = d_dot_d * n_dot_n - n_dot_d * n_dot_d;
    const float eq_b    = d_dot_d * m_dot_n - n_dot_d * m_dot_d;
    const float eq_c    = d_dot_d * (m_dot_m - Slic3r::sqr(cylinder_radius)) - m_dot_d * m_dot_d;

    const float discr   = eq_b * eq_b - eq_a * eq_c;
    // A negative discriminant corresponds to the infinite line not going through the infinite cylinder.
    if (discr < 0.0f)
        return false;

    // Check if the finite line is going through the finite cylinder.
    const float discr_sqrt = std::sqrt(discr);
    const float t1         = (-eq_b - discr_sqrt) / eq_a;
    if (0.f <= t1 && t1 <= 1.f)
        if (const float cylinder_endcap_t1 = m_dot_d + t1 * n_dot_d; 0.f <= cylinder_endcap_t1 && cylinder_endcap_t1 <= d_dot_d)
            return true;

    const float t2 = (-eq_b + discr_sqrt) / eq_a;
    if (0.f <= t2 && t2 <= 1.f)
        if (const float cylinder_endcap_t2 = (m_dot_d + t2 * n_dot_d); 0.f <= cylinder_endcap_t2 && cylinder_endcap_t2 <= d_dot_d)
            return true;

    return false;
}

// Check if the line is whole inside the capsule, or it is partially inside (intersecting) the capsule.
static bool test_line_inside_capsule(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &capsule_p, const Vec3f &capsule_q, const float capsule_radius) {
    assert(capsule_p != capsule_q);

    // Check if the line intersect any of the spheres forming the capsule.
    if (test_line_inside_sphere(line_a, line_b, capsule_p, capsule_radius) || test_line_inside_sphere(line_a, line_b, capsule_q, capsule_radius))
        return true;

    // Check if the line intersects the cylinder between the centers of the spheres.
    return test_line_inside_cylinder(line_a, line_b, capsule_p, capsule_q, capsule_radius);
}

#ifndef NDEBUG
bool TriangleSelector::verify_triangle_midpoints(const Triangle &tr) const
{
    for (int i = 0; i < 3; ++ i) {
        int v1   = tr.verts_idxs[i];
        int v2   = tr.verts_idxs[next_idx_modulo(i, 3)];
        int vmid = this->triangle_midpoint(tr, v1, v2);
        assert(vmid >= -1);
        if (vmid != -1) {
            Vec3f c1 = 0.5f * (m_vertices[v1].v + m_vertices[v2].v);
            Vec3f c2 = m_vertices[vmid].v;
            float d  = (c2 - c1).norm();
            assert(std::abs(d) < EPSILON);
        }
    }
    return true;
}

bool TriangleSelector::verify_triangle_neighbors(const Triangle &tr, const Vec3i32 &neighbors) const
{
    assert(neighbors(0) >= -1);
    assert(neighbors(1) >= -1);
    assert(neighbors(2) >= -1);
    assert(verify_triangle_midpoints(tr));

    for (int i = 0; i < 3; ++i)
        if (neighbors(i) != -1) {
            const Triangle &tr2 = m_triangles[neighbors(i)];
            assert(verify_triangle_midpoints(tr2));
            int v1 = tr.verts_idxs[i];
            int v2 = tr.verts_idxs[next_idx_modulo(i, 3)];
            assert(tr2.verts_idxs[0] == v1 || tr2.verts_idxs[1] == v1 || tr2.verts_idxs[2] == v1);
            int j = tr2.verts_idxs[0] == v1 ? 0 : tr2.verts_idxs[1] == v1 ? 1 : 2;
            assert(tr2.verts_idxs[j] == v1);
            assert(tr2.verts_idxs[prev_idx_modulo(j, 3)] == v2);
        }
    return true;
}
#endif // NDEBUG

// sides_to_split==-1 : just restore previous split
void TriangleSelector::Triangle::set_division(int sides_to_split, int special_side_idx)
{
    assert(sides_to_split >= 0 && sides_to_split <= 3);
    assert(special_side_idx >= 0 && special_side_idx < 3);
    assert(sides_to_split == 1 || sides_to_split == 2 || special_side_idx == 0);
    this->number_of_splits = char(sides_to_split);
    this->special_side_idx = char(special_side_idx);
}

// Pre-computed barycentric resolver
// Real-time collision detection, Ericson, Chapter 3.4
// Cache inspired by Don Hatch at https://gamedev.stackexchange.com/questions/23743/whats-the-most-efficient-way-to-find-barycentric-coordinates/23745#comment390123_23745
struct Barycentric
{
public:
    Barycentric(const Vec3f& a, const Vec3f& b, const Vec3f& c)
        :a_(a)
    {
         // Pre-compute denominator
        const Vec3f v0 = b - a;
        const Vec3f v1 = c - a;
        const float d00 = v0.dot(v0);
        const float d01 = v0.dot(v1);
        const float d11 = v1.dot(v1);
        const float inv_denom_ = 1.0f / (d00 * d11 - d01 * d01);

        x1_ = (d11 * v0 - d01 * v1) * inv_denom_;
        x2_ = (d00 * v1 - d01 * v0) * inv_denom_;
    }

    Vec3f calc(const Vec3f& p) const
    {
        const Vec3f v2  = p - a_;
        const float v = v2.dot(x1_);
        const float w = v2.dot(x2_);
        const float u = 1.f - v - w;

        return {u, v, w};
    }

    bool is_point_inside_triangle(const Vec3f& p) const
    {
        const Vec3f barycentric_cords = calc(p);
        return std::all_of(begin(barycentric_cords), end(barycentric_cords), [](float cord) { return 0.f <= cord && cord <= 1.0; });
    }

    static Vec3f calc(const Vec3f& pt, const Vec3f& p1, const Vec3f& p2, const Vec3f& p3)
    {
        const std::array<Vec3f, 3> vec = {p2 - p1, p3 - p1, pt - p1};
        const float d00                = vec[0].dot(vec[0]);
        const float d01                = vec[0].dot(vec[1]);
        const float d11                = vec[1].dot(vec[1]);
        const float d20                = vec[2].dot(vec[0]);
        const float d21                = vec[2].dot(vec[1]);
        const float denom              = d00 * d11 - d01 * d01;

        const float v = (d11 * d20 - d01 * d21) / denom;
        const float w = (d00 * d21 - d01 * d20) / denom;
        const float u = 1.f - v - w;

        return {u, v, w};
    }

private:
    Vec3f a_;
    Vec3f x1_;
    Vec3f x2_;
};

static bool is_point_inside_triangle(const Vec3f &pt, const Vec3f &p1, const Vec3f &p2, const Vec3f &p3)
{
    Vec3f barycentric_cords = Barycentric::calc(pt, p1, p2, p3);
    return std::all_of(begin(barycentric_cords), end(barycentric_cords), [](float cord) { return 0.f <= cord && cord <= 1.0; });
}

int TriangleSelector::select_unsplit_triangle(const Vec3f &hit, int facet_idx, const Vec3i32 &neighbors) const
{
    assert(facet_idx < int(m_triangles.size()));
    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return -1;

    if (!tr->is_split()) {
        if (const std::array<int, 3> &t_vert = m_triangles[facet_idx].verts_idxs; is_point_inside_triangle(hit, m_vertices[t_vert[0]].v, m_vertices[t_vert[1]].v, m_vertices[t_vert[2]].v))
            return facet_idx;

        return -1;
    }

    assert(this->verify_triangle_neighbors(*tr, neighbors));

    int num_of_children = tr->number_of_split_sides() + 1;
    if (num_of_children != 1) {
        for (int i = 0; i < num_of_children; ++i) {
            assert(i < int(tr->children.size()));
            assert(tr->children[i] < int(m_triangles.size()));
            // Recursion, deep first search over the children of this triangle.
            // All children of this triangle were created by splitting a single source triangle of the original mesh.

            const std::array<int, 3> &t_vert = m_triangles[tr->children[i]].verts_idxs;
            if (is_point_inside_triangle(hit, m_vertices[t_vert[0]].v, m_vertices[t_vert[1]].v, m_vertices[t_vert[2]].v))
                return this->select_unsplit_triangle(hit, tr->children[i], this->child_neighbors(*tr, neighbors, i));
        }
    }

    return -1;
}

int TriangleSelector::select_unsplit_triangle(const Vec3f &hit, int facet_idx) const
{
    assert(facet_idx < int(m_triangles.size()));
    if (!m_triangles[facet_idx].valid())
        return -1;

    Vec3i32 neighbors = m_neighbors[facet_idx];
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
    return this->select_unsplit_triangle(hit, facet_idx, neighbors);
}

bool TriangleSelector::SetLeafStateWithoutCleanup(int triangleIndex, EnforcerBlockerType state, int* sourceTriangle)
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return false;

    Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid() || triangle.is_split() || triangle.get_state() == state)
        return false;

    const int source = triangle.source_triangle;
    triangle.set_state(state);
    ++m_stateRevision;
    OnSelectorMutation(MutationKind::State, source);
    if (sourceTriangle != nullptr)
        *sourceTriangle = source;
    return true;
}

bool TriangleSelector::SetLeafState(int triangleIndex, EnforcerBlockerType state, CleanupMode cleanupMode)
{
    int sourceTriangle = -1;
    if (!SetLeafStateWithoutCleanup(triangleIndex, state, &sourceTriangle))
        return false;

    if (cleanupMode == CleanupMode::Deferred)
        RecordDeferredCleanupRoot(static_cast<uint32_t>(sourceTriangle));
    else
        remove_useless_children(sourceTriangle);

    return true;
}

bool TriangleSelector::SubtreeAllLeavesHaveState(int triangleIndex, EnforcerBlockerType state) const
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return false;

    const Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid())
        return false;

    if (!triangle.is_split())
        return triangle.get_state() == state;

    const int childCount = triangle.number_of_split_sides() + 1;
    for (int childOffset = 0; childOffset < childCount; ++childOffset)
        if (!SubtreeAllLeavesHaveState(triangle.children[childOffset], state))
            return false;

    return true;
}

bool TriangleSelector::CollapseSubtreeToState(int triangleIndex, EnforcerBlockerType state)
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return false;

    const Triangle& triangleBefore = m_triangles[triangleIndex];
    if (!triangleBefore.valid())
        return false;

    const bool semanticStateChanged = !SubtreeAllLeavesHaveState(triangleIndex, state);
    const int  sourceTriangle       = triangleBefore.source_triangle;

    // May destroy children and emit Topology mutation notifications.
    undivide_triangle(triangleIndex);

    Triangle& collapsedLeaf = m_triangles[triangleIndex];
    assert(collapsedLeaf.valid());
    assert(!collapsedLeaf.is_split());

    if (collapsedLeaf.get_state() != state)
        collapsedLeaf.set_state(state);

    if (semanticStateChanged) {
        ++m_stateRevision;
        OnSelectorMutation(MutationKind::State, sourceTriangle);
    }

    return semanticStateChanged;
}

bool TriangleSelector::SelectSeedFillLeaf(int triangleIndex)
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return false;

    Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid() || triangle.is_split() || triangle.is_selected_by_seed_fill())
        return false;

    triangle.select_by_seed_fill();
    m_seedFillSelectedLeaves.push_back(triangleIndex);
    ++m_seedFillPreviewRevision;
    return true;
}

uint32_t TriangleSelector::BeginQueryGeneration(std::vector<uint32_t>& queryStamp, uint32_t& queryGeneration,
                                                size_t requiredSize)
{
    // Stamps are grown lazily here, not kept in sync by push_triangle. Valid only because a query
    // never adds triangles; stale stamps of reused slots are always older than the new generation.
    if (queryStamp.size() < requiredSize)
        queryStamp.resize(requiredSize, 0);

    if (queryGeneration == std::numeric_limits<uint32_t>::max())
    {
        std::fill(queryStamp.begin(), queryStamp.end(), 0);
        queryGeneration = 1;
    }
    else
    {
        ++queryGeneration;
    }

    return queryGeneration;
}

void TriangleSelector::PrepareForLeafIdentityChange()
{
    seed_fill_unselect_all_triangles();
    m_neighborCache.topologyRevision = 0;
    m_neighborCache.indexRevision = 0;
    OnLeafIdentityWillChange();
    assert(m_seedFillSelectedLeaves.empty());
}

void TriangleSelector::RecordDeferredCleanupRoot(uint32_t sourceTriangle)
{
    if (sourceTriangle < static_cast<uint32_t>(m_orig_size_indices))
        m_deferredCleanupRoots.push_back(sourceTriangle);
}

void TriangleSelector::ClearDeferredCleanup()
{
    m_deferredCleanupRoots.clear();
}

bool TriangleSelector::FlushDeferredCleanup()
{
    if (m_deferredCleanupRoots.empty())
        return false;

    // Drop roots that cannot merge anything before sorting: unsplit roots are
    // no-ops in remove_useless_children, and large original-leaf fills record
    // millions of them. Split state is judged here, at flush time.
    m_deferredCleanupRoots.erase(
        std::remove_if(m_deferredCleanupRoots.begin(), m_deferredCleanupRoots.end(),
            [this](uint32_t source)
            {
                if (source >= static_cast<uint32_t>(m_orig_size_indices))
                    return true;

                const Triangle& triangle = m_triangles[source];
                return !triangle.valid() || !triangle.is_split();
            }),
        m_deferredCleanupRoots.end());

    if (m_deferredCleanupRoots.empty())
        return false;

    std::sort(m_deferredCleanupRoots.begin(), m_deferredCleanupRoots.end());
    m_deferredCleanupRoots.erase(std::unique(m_deferredCleanupRoots.begin(), m_deferredCleanupRoots.end()),
                                 m_deferredCleanupRoots.end());

    std::vector<uint32_t> roots;
    roots.swap(m_deferredCleanupRoots);

    bool topologyChanged = false;
    for (uint32_t sourceTriangle : roots)
        topologyChanged |= remove_useless_children(static_cast<int>(sourceTriangle));
    return topologyChanged;
}

std::optional<std::array<Vec3f, 3>> TriangleSelector::GetLeafVertices(int triangleIndex) const
{
    if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
        return std::nullopt;

    const Triangle& triangle = m_triangles[triangleIndex];
    if (!triangle.valid() || triangle.is_split())
        return std::nullopt;

    std::array<Vec3f, 3> vertices;
    for (size_t vertexIndex = 0; vertexIndex < triangle.verts_idxs.size(); ++vertexIndex) {
        const int meshVertexIndex = triangle.verts_idxs[vertexIndex];
        if (meshVertexIndex < 0 || meshVertexIndex >= static_cast<int>(m_vertices.size()))
            return std::nullopt;
        vertices[vertexIndex] = m_vertices[meshVertexIndex].v;
    }
    return vertices;
}

void TriangleSelector::NotifyTopologyMutation(int sourceTriangle)
{
    if (sourceTriangle < 0 || sourceTriangle >= m_orig_size_indices)
        return;

    ++m_topologyRevision;
    OnSelectorMutation(MutationKind::Topology, sourceTriangle);
}

void TriangleSelector::NotifyAllStateMutation()
{
    ++m_stateRevision;
    OnSelectorMutation(MutationKind::AllState, -1);
}

void TriangleSelector::NotifyIndexRebuild()
{
    ++m_triangleIndexRevision;
    OnSelectorMutation(MutationKind::IndexRebuild, -1);
}

void TriangleSelector::NotifyFullReset()
{
    ++m_topologyRevision;
    ++m_triangleIndexRevision;
    ++m_stateRevision;
    OnSelectorMutation(MutationKind::FullReset, -1);
}

bool TriangleSelector::PrepareSelectionCursor(std::unique_ptr<Cursor> cursor)
{
    if (cursor == nullptr)
        return false;

    m_cursor = std::move(cursor);
    const bool isHeightRange = dynamic_cast<HeightRange*>(m_cursor.get()) != nullptr;
    const float baseLimit = isHeightRange ? 0.1f : 0.05f;
    // Base limits are in mm; widen the cap for downscaled meshes so it never goes below that in world space.
    const float limitCap = baseLimit * std::max(m_cursor->meshUnitsPerMm, 1.f);
    const float edgeLimit = isHeightRange ? limitCap : std::min(std::sqrt(m_cursor->radius_sqr) / 5.f, limitCap);
    if (m_old_cursor_radius_sqr != m_cursor->radius_sqr || m_edge_limit != edgeLimit)
        set_edge_limit(edgeLimit);
    m_old_cursor_radius_sqr = m_cursor->radius_sqr;
    return true;
}

void TriangleSelector::select_patch(int facet_start, std::unique_ptr<Cursor> &&cursor, EnforcerBlockerType new_state, const Transform3d& trafo_no_translate, bool triangle_splitting, float highlight_by_angle_deg, const bool select_partially)
{
    assert(facet_start < m_orig_size_indices);
    const bool isHeightRange = dynamic_cast<HeightRange*>(cursor.get()) != nullptr;
    assert(!isHeightRange);
    if (facet_start < 0 || facet_start >= m_orig_size_indices || cursor == nullptr || isHeightRange ||
        !PrepareSelectionCursor(std::move(cursor)))
        return;

    const float highlight_angle_limit = -cos(Geometry::deg2rad(highlight_by_angle_deg));

    // Keep track of original facets visited by this query without clearing an O(N) array.
    const uint32_t queryGeneration = BeginQueryGeneration(
        m_rootQueryStamp, m_rootQueryGeneration, static_cast<size_t>(m_orig_size_indices));

    // Now start with the facet the pointer points to and check all adjacent facets.
    std::vector<int> facetsToCheck;
    facetsToCheck.reserve(16);
    facetsToCheck.emplace_back(facet_start);

    const Matrix3f normalMatrix = static_cast<Matrix3f>(
        trafo_no_translate.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());

    // Breadth-first search around the hit point. facetsToCheck may grow significantly large.
    size_t facetIndex = 0;
    while (facetIndex < facetsToCheck.size())
    {
        const int facet = facetsToCheck[facetIndex];
        const Vec3f& facetNormal = m_face_normals[m_triangles[facet].source_triangle];
        const float worldNormalZ = (normalMatrix * facetNormal).normalized().z();
        if (m_rootQueryStamp[facet] != queryGeneration &&
            (highlight_by_angle_deg == 0.f || worldNormalZ < highlight_angle_limit))
        {
            if (select_triangle(facet, new_state, triangle_splitting, select_partially))
            {
                // Add neighboring facets to the list to be processed later.
                for (int neighborIndex : m_neighbors[facet])
                    if (neighborIndex >= 0 && m_cursor->is_facet_visible(neighborIndex, m_face_normals))
                        facetsToCheck.push_back(neighborIndex);
            }
        }
        m_rootQueryStamp[facet] = queryGeneration;
        ++facetIndex;
    }
}

void TriangleSelector::SelectHeightRange(const std::vector<uint32_t>& sourceRoots, std::unique_ptr<HeightRange>&& cursor,
                                         const HeightRangeSelectionOptions& options)
{
    if (sourceRoots.empty() || !PrepareSelectionCursor(std::move(cursor)))
        return;

    const float highlightAngleLimit = -cos(Geometry::deg2rad(options.highlightByAngleDeg));
    Matrix3f normalMatrix = Matrix3f::Identity();
    if (options.highlightByAngleDeg != 0.f)
    {
        normalMatrix = static_cast<Matrix3f>(
            options.transformNoTranslate.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());
    }

    // A root whose three vertices are all inside the band and which is already an unsplit leaf of the
    // target state is a no-op for select_triangle(); the precomputed spans detect that without transforms.
    const std::vector<std::pair<float, float>>* rootZSpans = options.rootZSpans;
    if (rootZSpans != nullptr && rootZSpans->size() != static_cast<size_t>(m_orig_size_indices))
        rootZSpans = nullptr;
    const bool canSkipPaintedRoots = rootZSpans != nullptr && !m_cursor->clipping_plane.is_active();
    const HeightRange& heightCursor = static_cast<const HeightRange&>(*m_cursor);
    // Margin absorbs rounding between the double-built spans and the float cursor transform.
    constexpr float SPAN_MARGIN = 1e-3f;
    const float insideBottomZ = heightCursor.GetBottomZ() - HeightRange::VERTEX_TOLERANCE + SPAN_MARGIN;
    const float insideTopZ =
        heightCursor.GetBottomZ() + heightCursor.GetHeight() + HeightRange::VERTEX_TOLERANCE - SPAN_MARGIN;

    for (uint32_t sourceRoot : sourceRoots)
    {
        if (sourceRoot >= static_cast<uint32_t>(m_orig_size_indices))
            continue;

        const int rootIndex = static_cast<int>(sourceRoot);
        const Triangle& rootTriangle = m_triangles[rootIndex];
        if (!rootTriangle.valid())
            continue;

        if (canSkipPaintedRoots && !rootTriangle.is_split() && rootTriangle.get_state() == options.state)
        {
            const std::pair<float, float>& zSpan = (*rootZSpans)[sourceRoot];
            if (zSpan.first > insideBottomZ && zSpan.second < insideTopZ)
                continue;
        }

        if (!m_cursor->is_edge_inside_cursor(rootTriangle, m_vertices))
            continue;

        if (options.highlightByAngleDeg != 0.f)
        {
            const Vec3f& facetNormal = m_face_normals[rootTriangle.source_triangle];
            const float worldNormalZ = (normalMatrix * facetNormal).normalized().z();
            if (!(worldNormalZ < highlightAngleLimit))
                continue;
        }

        select_triangle(rootIndex, options.state, options.triangleSplitting);
    }
}

bool TriangleSelector::is_facet_clipped(int facet_idx, const ClippingPlane &clp) const
{
    for (int vert_idx : m_triangles[facet_idx].verts_idxs)
        if (clp.is_active() && clp.is_mesh_point_clipped(m_vertices[vert_idx].v))
            return true;

    return false;
}

void TriangleSelector::seed_fill_select_triangles(const Vec3f &hit, int facet_start, const Transform3d& trafo_no_translate,
                                                  const ClippingPlane &clp, float seed_fill_angle, float highlight_by_angle_deg,
                                                  bool force_reselection)
{
    assert(facet_start < m_orig_size_indices);

    // Recompute seed fill only if the cursor is pointing on facet unselected by seed fill or a clipping plane is active.
    if (int start_facet_idx = select_unsplit_triangle(hit, facet_start); start_facet_idx >= 0 && m_triangles[start_facet_idx].is_selected_by_seed_fill() && !force_reselection && !clp.is_active())
        return;

    this->seed_fill_unselect_all_triangles();

    const uint32_t queryGeneration = BeginQueryGeneration(
        m_triangleQueryStamp, m_triangleQueryGeneration, m_triangles.size());
    std::queue<int>   facet_queue;
    facet_queue.push(facet_start);
    // Mark-on-enqueue: every queued facet gets exactly one processing pass.
    m_triangleQueryStamp[facet_start] = queryGeneration;

    const double facet_angle_limit     = cos(Geometry::deg2rad(seed_fill_angle)) - EPSILON;
    const float  highlight_angle_limit = -cos(Geometry::deg2rad(highlight_by_angle_deg));
    // The world-normal matrix is only needed by the overhang gate.
    const Matrix3f normal_matrix = highlight_by_angle_deg != 0.f ? static_cast<Matrix3f>(
        trafo_no_translate.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>()) : Matrix3f::Identity();

    // Depth-first traversal of neighbors of the face hit by the ray thrown from the mouse cursor.
    while (!facet_queue.empty()) {
        int current_facet = facet_queue.front();
        facet_queue.pop();

        // Keep the original overhang gate; world normal is only needed when it is active.
        bool passesOverhang = true;
        if (highlight_by_angle_deg != 0.f) {
            const Vec3f &facet_normal   = m_face_normals[m_triangles[current_facet].source_triangle];
            const float  world_normal_z = (normal_matrix * facet_normal).normalized().z();
            passesOverhang = world_normal_z < highlight_angle_limit;
        }

        if (passesOverhang) {
            if (m_triangles[current_facet].is_split()) {
                for (int split_triangle_idx = 0; split_triangle_idx <= m_triangles[current_facet].number_of_split_sides(); ++split_triangle_idx) {
                    assert(split_triangle_idx < int(m_triangles[current_facet].children.size()));
                    assert(m_triangles[current_facet].children[split_triangle_idx] < int(m_triangles.size()));
                    if (int child = m_triangles[current_facet].children[split_triangle_idx];
                        m_triangleQueryStamp[child] != queryGeneration) {
                        // Child triangle shares normal with its parent. Select it.
                        m_triangleQueryStamp[child] = queryGeneration;
                        facet_queue.push(child);
                    }
                }
            } else
                SelectSeedFillLeaf(current_facet);

            if (current_facet < m_orig_size_indices)
                // Propagate over the original triangles.
                for (int neighbor_idx : m_neighbors[current_facet]) {
                    assert(neighbor_idx >= -1);
                    if (neighbor_idx >= 0 && m_triangleQueryStamp[neighbor_idx] != queryGeneration &&
                        !is_facet_clipped(neighbor_idx, clp)) {
                        // Check if neighbour_facet_idx is satisfies angle in seed_fill_angle and append it to facet_queue if it do.
                        const Vec3f &n1 = m_face_normals[m_triangles[neighbor_idx].source_triangle];
                        const Vec3f &n2 = m_face_normals[m_triangles[current_facet].source_triangle];
                        if (std::clamp(n1.dot(n2), 0.f, 1.f) >= facet_angle_limit) {
                            m_triangleQueryStamp[neighbor_idx] = queryGeneration;
                            facet_queue.push(neighbor_idx);
                        }
                    }
                }
        }
    }
}

void TriangleSelector::precompute_all_neighbors_recursive(const int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated, std::vector<Vec3i32> &neighbors_out, std::vector<Vec3i32> &neighbors_propagated_out) const
{
    assert(facet_idx < int(m_triangles.size()));

    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return;

    neighbors_out[facet_idx]            = neighbors;
    neighbors_propagated_out[facet_idx] = neighbors_propagated;
    if (tr->is_split()) {
        assert(this->verify_triangle_neighbors(*tr, neighbors));

        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i = 0; i < num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                const Vec3i32 child_neighbors = this->child_neighbors(*tr, neighbors, i);
                this->precompute_all_neighbors_recursive(tr->children[i], child_neighbors,
                                                         this->child_neighbors_propagated(*tr, neighbors_propagated, i, child_neighbors), neighbors_out,
                                                         neighbors_propagated_out);
            }
        }
    }
}

const TriangleSelector::NeighborCache& TriangleSelector::EnsureNeighborCache() const
{
    if (m_neighborCache.topologyRevision == m_topologyRevision &&
        m_neighborCache.indexRevision == m_triangleIndexRevision &&
        m_neighborCache.neighbors.size() == m_triangles.size() &&
        m_neighborCache.propagated.size() == m_triangles.size())
        return m_neighborCache;

    m_neighborCache.neighbors.assign(m_triangles.size(), Vec3i32(-1, -1, -1));
    m_neighborCache.propagated.assign(m_triangles.size(), Vec3i32(-1, -1, -1));
    for (int facet_idx = 0; facet_idx < this->m_orig_size_indices; ++facet_idx)
    {
        m_neighborCache.neighbors[facet_idx] = m_neighbors[facet_idx];
        m_neighborCache.propagated[facet_idx] = m_neighborCache.neighbors[facet_idx];
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], m_neighborCache.neighbors[facet_idx]));
        if (m_triangles[facet_idx].is_split())
            this->precompute_all_neighbors_recursive(facet_idx, m_neighborCache.neighbors[facet_idx],
                m_neighborCache.propagated[facet_idx], m_neighborCache.neighbors, m_neighborCache.propagated);
    }
    m_neighborCache.topologyRevision = m_topologyRevision;
    m_neighborCache.indexRevision = m_triangleIndexRevision;
    return m_neighborCache;
}

// It appends all triangles that are touching the edge (vertexi, vertexj) of the triangle.
// It doesn't append the triangles that are touching the triangle only by part of the edge that means the triangles are from lower depth.
void TriangleSelector::append_touching_subtriangles(int itriangle, int vertexi, int vertexj, std::vector<int> &touching_subtriangles_out) const
{
    if (itriangle == -1)
        return;

    auto process_subtriangle = [this, &itriangle, &vertexi, &vertexj, &touching_subtriangles_out](const int subtriangle_idx, Partition partition) -> void {
        assert(subtriangle_idx != -1);
        if (!m_triangles[subtriangle_idx].is_split())
            touching_subtriangles_out.emplace_back(subtriangle_idx);
        else if (int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj); midpoint != -1)
            append_touching_subtriangles(subtriangle_idx, partition == Partition::First ? vertexi : midpoint, partition == Partition::First ? midpoint : vertexj, touching_subtriangles_out);
        else
            append_touching_subtriangles(subtriangle_idx, vertexi, vertexj, touching_subtriangles_out);
    };

    std::pair<int, int> touching = this->triangle_subtriangles(itriangle, vertexi, vertexj);
    if (touching.first != -1)
        process_subtriangle(touching.first, Partition::First);

    if (touching.second != -1)
        process_subtriangle(touching.second, Partition::Second);
}

// It appends all edges that are touching the edge (vertexi, vertexj) of the triangle and are not selected by seed fill
// It doesn't append the edges that are touching the triangle only by part of the edge that means the triangles are from lower depth.
void TriangleSelector::append_touching_edges(int itriangle, int vertexi, int vertexj, std::vector<Vec2i32> &touching_edges_out) const
{
    if (itriangle == -1)
        return;

    auto process_subtriangle = [this, &itriangle, &vertexi, &vertexj, &touching_edges_out](const int subtriangle_idx, Partition partition) -> void {
        assert(subtriangle_idx != -1);
        if (!m_triangles[subtriangle_idx].is_split()) {
            if (!m_triangles[subtriangle_idx].is_selected_by_seed_fill()) {
                int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj);
                if (partition == Partition::First && midpoint != -1) {
                    touching_edges_out.emplace_back(vertexi, midpoint);
                } else if (partition == Partition::First && midpoint == -1) {
                    touching_edges_out.emplace_back(vertexi, vertexj);
                } else {
                    assert(midpoint != -1 && partition == Partition::Second);
                    touching_edges_out.emplace_back(midpoint, vertexj);
                }
            }
        } else if (int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj); midpoint != -1)
            append_touching_edges(subtriangle_idx, partition == Partition::First ? vertexi : midpoint, partition == Partition::First ? midpoint : vertexj,
                                  touching_edges_out);
        else
            append_touching_edges(subtriangle_idx, vertexi, vertexj, touching_edges_out);
    };

    std::pair<int, int> touching = this->triangle_subtriangles(itriangle, vertexi, vertexj);
    if (touching.first != -1)
        process_subtriangle(touching.first, Partition::First);

    if (touching.second != -1)
        process_subtriangle(touching.second, Partition::Second);
}

// BBS: add seed_fill_angle parameter
void TriangleSelector::bucket_fill_select_triangles(const Vec3f& hit, int facet_start, const ClippingPlane &clp, float seed_fill_angle, bool propagate, bool force_reselection)
{
    int start_facet_idx = select_unsplit_triangle(hit, facet_start);
    assert(start_facet_idx != -1);
    // Recompute bucket fill only if the cursor is pointing on facet unselected by bucket fill or a clipping plane is active.
    if (start_facet_idx == -1 || (m_triangles[start_facet_idx].is_selected_by_seed_fill() && !force_reselection && !clp.is_active()))
        return;

    assert(!m_triangles[start_facet_idx].is_split());
    EnforcerBlockerType start_facet_state = m_triangles[start_facet_idx].get_state();
    this->seed_fill_unselect_all_triangles();

    if (!propagate) {
        SelectSeedFillLeaf(start_facet_idx);
        return;
    }

    // seed_fill_angle < 0.f to disable edge detection
    const double facet_angle_limit = (seed_fill_angle < 0.f ? -1.f : cos(Geometry::deg2rad(seed_fill_angle))) - EPSILON;

    // Reused scratch container: cleared per visit, no per-leaf vector allocation.
    auto collect_touching_triangles = [this](int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated,
                                             std::vector<int> &output) {
        assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
        output.clear();
        Vec3i32 vertices = {m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2]};
        append_touching_subtriangles(neighbors(0), vertices(1), vertices(0), output);
        append_touching_subtriangles(neighbors(1), vertices(2), vertices(1), output);
        append_touching_subtriangles(neighbors(2), vertices(0), vertices(2), output);

        for (int neighbor_idx : neighbors_propagated)
            if (neighbor_idx != -1 && !m_triangles[neighbor_idx].is_split())
                output.emplace_back(neighbor_idx);
    };

    // Decide BEFORE preparing the neighbor cache; the fast path must not allocate it.
    const bool originalMeshOnly = CanUseOriginalMeshFillPath();
    const NeighborCache* neighborCache = nullptr;
    if (!originalMeshOnly)
        neighborCache = &EnsureNeighborCache();

    const uint32_t queryGeneration = BeginQueryGeneration(
        m_triangleQueryStamp, m_triangleQueryGeneration, m_triangles.size());
    std::queue<int>    facet_queue;
    std::vector<int>   touching_triangles;
    if (!originalMeshOnly)
        touching_triangles.reserve(8);

    // Mark-on-enqueue: every queued facet gets exactly one processing pass.
    m_triangleQueryStamp[start_facet_idx] = queryGeneration;
    facet_queue.push(start_facet_idx);

    // Both paths share this filter so their semantics cannot drift apart.
    const auto tryEnqueue = [&](int currentFacet, int nextFacet)
    {
        if (nextFacet < 0)
            return;
        assert(nextFacet < static_cast<int>(m_triangles.size()));

        if (m_triangleQueryStamp[nextFacet] == queryGeneration)
            return;
        if (m_triangles[nextFacet].get_state() != start_facet_state)
            return;
        if (is_facet_clipped(nextFacet, clp))
            return;

        const Vec3f& n1 = m_face_normals[m_triangles[nextFacet].source_triangle];
        const Vec3f& n2 = m_face_normals[m_triangles[currentFacet].source_triangle];
        if (seed_fill_angle >= -EPSILON && std::clamp(n1.dot(n2), 0.f, 1.f) < facet_angle_limit)
            return;

        assert(!m_triangles[nextFacet].is_split());
        m_triangleQueryStamp[nextFacet] = queryGeneration;
        facet_queue.push(nextFacet);
    };

    while (!facet_queue.empty()) {
        int current_facet = facet_queue.front();
        facet_queue.pop();
        assert(!m_triangles[current_facet].is_split());

        SelectSeedFillLeaf(current_facet);

        if (originalMeshOnly) {
            // Original mesh: the three edge neighbors, in the original edge order.
            for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex)
                tryEnqueue(current_facet, m_neighbors[current_facet](edgeIndex));
        }
        else {
            collect_touching_triangles(current_facet, neighborCache->neighbors[current_facet], neighborCache->propagated[current_facet],
                                       touching_triangles);
            for (const int tr_idx : touching_triangles)
                tryEnqueue(current_facet, tr_idx);
        }
    }
}

// Selects either the whole triangle (discarding any children it had), or divides
// the triangle recursively, selecting just subtriangles truly inside the circle.
// This is done by an actual recursive call. Returns false if the triangle is
// outside the cursor.
// Called by select_patch() and by itself.
bool TriangleSelector::select_triangle(int facet_idx, EnforcerBlockerType type, bool triangle_splitting, bool select_partially)
{
    assert(facet_idx < int(m_triangles.size()));

    if (! m_triangles[facet_idx].valid())
        return false;

    Vec3i32 neighbors = m_neighbors[facet_idx];
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));

    if (! select_triangle_recursive(facet_idx, neighbors, type, triangle_splitting, select_partially))
        return false;

    // In case that all children are leafs and have the same state now,
    // they may be removed and substituted by the parent triangle.
    remove_useless_children(facet_idx);

#ifdef EXPENSIVE_DEBUG_CHECKS
    // Make sure that we did not lose track of invalid triangles.
    assert(m_invalid_triangles == std::count_if(m_triangles.begin(), m_triangles.end(),
               [](const Triangle& tr) { return ! tr.valid(); }));
#endif // EXPENSIVE_DEBUG_CHECKS

    // Do garbage collection maybe?
    if (2*m_invalid_triangles > int(m_triangles.size()))
        garbage_collect();

    return true;
}

// Return child of itriangle at a CCW oriented side (vertexi, vertexj), either first or 2nd part.
// If the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::neighbor_child(const Triangle &tr, int vertexi, int vertexj, Partition partition) const
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no upper / lower subtriangle sharing the edge.
        return -1;

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    int child_idx;
    if (tr.number_of_split_sides() == 1) {
        if (edge != next_idx_modulo(tr.special_side(), 3))
            // A child may or may not be split at this side.
            return this->neighbor_child(m_triangles[tr.children[edge == tr.special_side() ? 0 : 1]], vertexi, vertexj, partition);
        child_idx = partition == Partition::First ? 0 : 1;
    } else if (tr.number_of_split_sides() == 2) {
        if (edge == next_idx_modulo(tr.special_side(), 3))
            // A child may or may not be split at this side.
            return this->neighbor_child(m_triangles[tr.children[2]], vertexi, vertexj, partition);
        child_idx = edge == tr.special_side() ?
            (partition == Partition::First ? 0 : 1) :
            (partition == Partition::First ? 2 : 0);
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        switch(edge) {
        case 0:  child_idx = partition == Partition::First ? 0 : 1; break;
        case 1:  child_idx = partition == Partition::First ? 1 : 2; break;
        default: assert(edge == 2);
                 child_idx = partition == Partition::First ? 2 : 0; break;
        }
    }
    return tr.children[child_idx];
}

// Return child of itriangle at a CCW oriented side (vertexi, vertexj), either first or 2nd part.
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::neighbor_child(int itriangle, int vertexi, int vertexj, Partition partition) const
{
    return itriangle == -1 ? -1 : this->neighbor_child(m_triangles[itriangle], vertexi, vertexj, partition);
}

std::pair<int, int> TriangleSelector::triangle_subtriangles(int itriangle, int vertexi, int vertexj) const
{
    return itriangle == -1 ? std::make_pair(-1, -1) : Slic3r::TriangleSelector::triangle_subtriangles(m_triangles[itriangle], vertexi, vertexj);
}

std::pair<int, int> TriangleSelector::triangle_subtriangles(const Triangle &tr, int vertexi, int vertexj)
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no subtriangles touching the edge.
        return std::make_pair(-1, -1);

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    if (tr.number_of_split_sides() == 1) {
        return edge == next_idx_modulo(tr.special_side(), 3) ? std::make_pair(tr.children[0], tr.children[1]) :
                                                                     std::make_pair(tr.children[edge == tr.special_side() ? 0 : 1], -1);
    } else if (tr.number_of_split_sides() == 2) {
        return edge == next_idx_modulo(tr.special_side(), 3) ? std::make_pair(tr.children[2], -1) :
               edge == tr.special_side()                           ? std::make_pair(tr.children[0], tr.children[1]) :
                                                                     std::make_pair(tr.children[2], tr.children[0]);
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        return edge == 0 ? std::make_pair(tr.children[0], tr.children[1]) :
               edge == 1 ? std::make_pair(tr.children[1], tr.children[2]) :
                           std::make_pair(tr.children[2], tr.children[0]);
    }

    return std::make_pair(-1, -1);
}

// Return existing midpoint of CCW oriented side (vertexi, vertexj).
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::triangle_midpoint(const Triangle &tr, int vertexi, int vertexj) const
{
    if (tr.number_of_split_sides() == 0)
        // If this triangle is not split, then there is no upper / lower subtriangle sharing the edge.
        return -1;

    // Find the triangle edge.
    int edge = tr.verts_idxs[0] == vertexi ? 0 : tr.verts_idxs[1] == vertexi ? 1 : 2;
    assert(tr.verts_idxs[edge] == vertexi);
    assert(tr.verts_idxs[next_idx_modulo(edge, 3)] == vertexj);

    if (tr.number_of_split_sides() == 1) {
        return edge == next_idx_modulo(tr.special_side(), 3) ?
            m_triangles[tr.children[0]].verts_idxs[2] :
            this->triangle_midpoint(m_triangles[tr.children[edge == tr.special_side() ? 0 : 1]], vertexi, vertexj);
    } else if (tr.number_of_split_sides() == 2) {
        return edge == next_idx_modulo(tr.special_side(), 3) ?
                    this->triangle_midpoint(m_triangles[tr.children[2]], vertexi, vertexj) :
               edge == tr.special_side() ?
                    m_triangles[tr.children[0]].verts_idxs[1] :
                    m_triangles[tr.children[1]].verts_idxs[2];
    } else {
        assert(tr.number_of_split_sides() == 3);
        assert(tr.special_side() == 0);
        return
            (edge == 0) ? m_triangles[tr.children[0]].verts_idxs[1] :
            (edge == 1) ? m_triangles[tr.children[1]].verts_idxs[2] :
                          m_triangles[tr.children[2]].verts_idxs[2];
    }
}

// Return existing midpoint of CCW oriented side (vertexi, vertexj).
// If itriangle == -1 or if the side sharing (vertexi, vertexj) is not split, return -1.
int TriangleSelector::triangle_midpoint(int itriangle, int vertexi, int vertexj) const
{
    return itriangle == -1 ? -1 : this->triangle_midpoint(m_triangles[itriangle], vertexi, vertexj);
}

int TriangleSelector::triangle_midpoint_or_allocate(int itriangle, int vertexi, int vertexj)
{
    int midpoint = this->triangle_midpoint(itriangle, vertexi, vertexj);
    if (midpoint == -1) {
        Vec3f c = 0.5f * (m_vertices[vertexi].v + m_vertices[vertexj].v);
#ifdef EXPENSIVE_DEBUG_CHECKS
        // Verify that the vertex is really a new one.
        auto it = std::find_if(m_vertices.begin(), m_vertices.end(), [c](const Vertex &v) {
            return v.ref_cnt > 0 && (v.v - c).norm() < EPSILON; });
        assert(it == m_vertices.end());
#endif // EXPENSIVE_DEBUG_CHECKS
        // Allocate a new vertex, possibly reusing the free list.
        if (m_free_vertices_head == -1) {
            // Allocate a new vertex.
            midpoint = int(m_vertices.size());
            m_vertices.emplace_back(c);
        } else {
            // Reuse a vertex from the free list.
            assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
            midpoint = m_free_vertices_head;
            memcpy(&m_free_vertices_head, &m_vertices[midpoint].v[0], sizeof(m_free_vertices_head));
            assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
            m_vertices[midpoint].v = c;
        }
        assert(m_vertices[midpoint].ref_cnt == 0);
    } else {
#ifndef NDEBUG
        Vec3f c1 = 0.5f * (m_vertices[vertexi].v + m_vertices[vertexj].v);
        Vec3f c2 = m_vertices[midpoint].v;
        float d = (c2 - c1).norm();
        assert(std::abs(d) < EPSILON);
#endif // NDEBUG
        assert(m_vertices[midpoint].ref_cnt > 0);
    }
    return midpoint;
}

// Return neighbors of ith child of a triangle given neighbors of the triangle.
// Returns -1 if such a neighbor does not exist at all, or it does not exist
// at the same depth as the ith child.
// Using the same splitting strategy as TriangleSelector::split_triangle()
Vec3i32 TriangleSelector::child_neighbors(const Triangle &tr, const Vec3i32 &neighbors, int child_idx) const
{
    assert(this->verify_triangle_neighbors(tr, neighbors));

    assert(child_idx >= 0 && child_idx <= tr.number_of_split_sides());
    int   i = tr.special_side();
    int   j = next_idx_modulo(i, 3);
    int   k = next_idx_modulo(j, 3);

    Vec3i32 out;
    switch (tr.number_of_split_sides()) {
    case 1:
        switch (child_idx) {
        case 0:
            out(0) = neighbors(i);
            out(1) = this->neighbor_child(neighbors(j), tr.verts_idxs[k], tr.verts_idxs[j], Partition::Second);
            out(2) = tr.children[1];
            break;
        default:
            assert(child_idx == 1);
            out(0) = this->neighbor_child(neighbors(j), tr.verts_idxs[k], tr.verts_idxs[j], Partition::First);
            out(1) = neighbors(k);
            out(2) = tr.children[0];
            break;
        }
        break;

    case 2:
        switch (child_idx) {
        case 0:
            out(0) = this->neighbor_child(neighbors(i), tr.verts_idxs[j], tr.verts_idxs[i], Partition::Second);
            out(1) = tr.children[1];
            out(2) = this->neighbor_child(neighbors(k), tr.verts_idxs[i], tr.verts_idxs[k], Partition::First);
            break;
        case 1:
            assert(child_idx == 1);
            out(0) = this->neighbor_child(neighbors(i), tr.verts_idxs[j], tr.verts_idxs[i], Partition::First);
            out(1) = tr.children[2];
            out(2) = tr.children[0];
            break;
        default:
            assert(child_idx == 2);
            out(0) = neighbors(j);
            out(1) = this->neighbor_child(neighbors(k), tr.verts_idxs[i], tr.verts_idxs[k], Partition::Second);
            out(2) = tr.children[1];
            break;
        }
        break;

    case 3:
        assert(tr.special_side() == 0);
        switch (child_idx) {
        case 0:
            out(0) = this->neighbor_child(neighbors(0), tr.verts_idxs[1], tr.verts_idxs[0], Partition::Second);
            out(1) = tr.children[3];
            out(2) = this->neighbor_child(neighbors(2), tr.verts_idxs[0], tr.verts_idxs[2], Partition::First);
            break;
        case 1:
            out(0) = this->neighbor_child(neighbors(0), tr.verts_idxs[1], tr.verts_idxs[0], Partition::First);
            out(1) = this->neighbor_child(neighbors(1), tr.verts_idxs[2], tr.verts_idxs[1], Partition::Second);
            out(2) = tr.children[3];
            break;
        case 2:
            out(0) = this->neighbor_child(neighbors(1), tr.verts_idxs[2], tr.verts_idxs[1], Partition::First);
            out(1) = this->neighbor_child(neighbors(2), tr.verts_idxs[0], tr.verts_idxs[2], Partition::Second);
            out(2) = tr.children[3];
            break;
        default:
            assert(child_idx == 3);
            out(0) = tr.children[1];
            out(1) = tr.children[2];
            out(2) = tr.children[0];
            break;
        }
        break;

    default:
        assert(false);
    }

    assert(this->verify_triangle_neighbors(tr, neighbors));
    assert(this->verify_triangle_neighbors(m_triangles[tr.children[child_idx]], out));
    return out;
}

// Return neighbors of the ith child of a triangle given neighbors of the triangle.
// If such a neighbor doesn't exist, return the neighbor from the previous depth.
Vec3i32 TriangleSelector::child_neighbors_propagated(const Triangle &tr, const Vec3i32 &neighbors_propagated, int child_idx, const Vec3i32 &child_neighbors) const
{
    int i = tr.special_side();
    int j = next_idx_modulo(i, 3);
    int k = next_idx_modulo(j, 3);

    Vec3i32 out = child_neighbors;
    auto  replace_if_not_exists = [&out, &neighbors_propagated](int index_to_replace, int neighbor_idx) {
        if (out(index_to_replace) == -1)
            out(index_to_replace) = neighbors_propagated(neighbor_idx);
    };

    switch (tr.number_of_split_sides()) {
    case 1:
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, i);
            replace_if_not_exists(1, j);
            break;
        default:
            assert(child_idx == 1);
            replace_if_not_exists(0, j);
            replace_if_not_exists(1, k);
            break;
        }
        break;

    case 2:
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, i);
            replace_if_not_exists(2, k);
            break;
        case 1:
            assert(child_idx == 1);
            replace_if_not_exists(0, i);
            break;
        default:
            assert(child_idx == 2);
            replace_if_not_exists(0, j);
            replace_if_not_exists(1, k);
            break;
        }
        break;

    case 3:
        assert(tr.special_side() == 0);
        switch (child_idx) {
        case 0:
            replace_if_not_exists(0, 0);
            replace_if_not_exists(2, 2);
            break;
        case 1:
            replace_if_not_exists(0, 0);
            replace_if_not_exists(1, 1);
            break;
        case 2:
            replace_if_not_exists(0, 1);
            replace_if_not_exists(1, 2);
            break;
        default:
            assert(child_idx == 3);
            break;
        }
        break;

    default: assert(false);
    }

    return out;
}

bool TriangleSelector::select_triangle_recursive(int facet_idx, const Vec3i32 &neighbors, EnforcerBlockerType type, bool triangle_splitting, bool select_partially)
{
    assert(facet_idx < int(m_triangles.size()));

    Triangle* tr = &m_triangles[facet_idx];
    if (! tr->valid())
        return false;

    assert(this->verify_triangle_neighbors(*tr, neighbors));

    int num_of_inside_vertices = m_cursor->vertices_inside(*tr, m_vertices);

    if (num_of_inside_vertices == 0
     && ! m_cursor->is_pointer_in_triangle(*tr, m_vertices)
     && ! m_cursor->is_edge_inside_cursor(*tr, m_vertices))
        return false;

    if (num_of_inside_vertices == 3) {
        // dump any subdivision and select whole triangle
        CollapseSubtreeToState(facet_idx, type);
    } else {
        // the triangle is partially inside, let's recursively divide it
        // (if not already) and try selecting its children.

        if (! tr->is_split() && tr->get_state() == type) {
            // This is leaf triangle that is already of correct type as a whole.
            // No need to split, all children would end up selected anyway.
            return true;
        }

        if (triangle_splitting)
            split_triangle(facet_idx, neighbors);
        if ((!triangle_splitting || select_partially) && !m_triangles[facet_idx].is_split()) {
            // Preserve the remapped boundary while notifying the current selector caches.
            SetLeafStateWithoutCleanup(facet_idx, type);
            return true;
        }
        tr = &m_triangles[facet_idx]; // might have been invalidated by split_triangle().

        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i=0; i<num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                select_triangle_recursive(tr->children[i], this->child_neighbors(*tr, neighbors, i), type, triangle_splitting, select_partially);
                tr = &m_triangles[facet_idx]; // might have been invalidated
            }
        }
    }

    return true;
}

void TriangleSelector::set_facet(int facet_idx, EnforcerBlockerType state)
{
    assert(facet_idx < m_orig_size_indices);
    CollapseSubtreeToState(facet_idx, state);
}

// called by select_patch()->select_triangle()...select_triangle()
// to decide which sides of the triangle to split and to actually split it calling set_division() and perform_split().
void TriangleSelector::split_triangle(int facet_idx, const Vec3i32 &neighbors)
{
    if (m_triangles[facet_idx].is_split()) {
        // The triangle is divided already.
        return;
    }

    PrepareForLeafIdentityChange();

    Triangle* tr = &m_triangles[facet_idx];
    assert(this->verify_triangle_neighbors(*tr, neighbors));

    EnforcerBlockerType old_type = tr->get_state();
    const int sourceTriangle = tr->source_triangle;

    // If we got here, we are about to actually split the triangle.
    const double limit_squared = m_edge_limit_sqr;

    std::array<int, 3>& facet = tr->verts_idxs;
    std::array<const stl_vertex*, 3> pts = { &m_vertices[facet[0]].v,
                                             &m_vertices[facet[1]].v,
                                             &m_vertices[facet[2]].v};
    std::array<stl_vertex, 3> pts_transformed; // must stay in scope of pts !!!

    // In case the object is non-uniformly scaled, transform the
    // points to world coords.
    if (! m_cursor->uniform_scaling) {
        for (size_t i=0; i<pts.size(); ++i) {
            pts_transformed[i] = m_cursor->trafo * (*pts[i]);
            pts[i] = &pts_transformed[i];
        }
    }

    std::array<double, 3> sides = {(*pts[2] - *pts[1]).squaredNorm(),
                                   (*pts[0] - *pts[2]).squaredNorm(),
                                   (*pts[1] - *pts[0]).squaredNorm()};

    boost::container::small_vector<int, 3> sides_to_split;
    int side_to_keep = -1;
    for (int pt_idx = 0; pt_idx<3; ++pt_idx) {
        if (sides[pt_idx] > limit_squared)
            sides_to_split.push_back(pt_idx);
        else
            side_to_keep = pt_idx;
    }
    if (sides_to_split.empty()) {
        // This shall be unselected.
        tr->set_division(0, 0);
        return;
    }

    // Save how the triangle will be split. Second argument makes sense only for one
    // or two split sides, otherwise the value is ignored.
    tr->set_division(int(sides_to_split.size()),
        sides_to_split.size() == 2 ? side_to_keep : sides_to_split[0]);

    perform_split(facet_idx, neighbors, old_type);
    NotifyTopologyMutation(sourceTriangle);
}

// Is pointer in a triangle?
bool TriangleSelector::Cursor::is_pointer_in_triangle(const Triangle &tr, const std::vector<Vertex> &vertices) const {
    const Vec3f& p1 = vertices[tr.verts_idxs[0]].v;
    const Vec3f& p2 = vertices[tr.verts_idxs[1]].v;
    const Vec3f& p3 = vertices[tr.verts_idxs[2]].v;
    return this->is_pointer_in_triangle(p1, p2, p3);
}

// Determine whether this facet is potentially visible (still can be obscured).
bool TriangleSelector::Cursor::is_facet_visible(const Cursor &cursor, int facet_idx, const std::vector<Vec3f> &face_normals)
{
    assert(facet_idx < int(face_normals.size()));
    Vec3f n = face_normals[facet_idx];
    if (!cursor.uniform_scaling)
        n = cursor.trafo_normal * n;
    return n.dot(cursor.dir) < 0.f;
}

// How many vertices of a triangle are inside the circle?
int TriangleSelector::Cursor::vertices_inside(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    int inside = 0;
    for (size_t i = 0; i < 3; ++i)
        if (this->is_mesh_point_inside(vertices[tr.verts_idxs[i]].v))
            ++inside;

    return inside;
}

// Is any edge inside Sphere cursor?
bool TriangleSelector::Sphere::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a = pts[side];
        const Vec3f &edge_b = pts[side < 2 ? side + 1 : 0];
        if (test_line_inside_sphere(edge_a, edge_b, this->center, this->radius))
            return true;
    }
    return false;
}

// Is edge inside cursor?
bool TriangleSelector::Circle::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    const Vec3f &p = this->center;
    for (int side = 0; side < 3; ++side) {
        const Vec3f &a      = pts[side];
        const Vec3f &b      = pts[side < 2 ? side + 1 : 0];
        Vec3f        s      = (b - a).normalized();
        float        t      = (p - a).dot(s);
        Vec3f        vector = a + t * s - p;

        // vector is 3D vector from center to the intersection. What we want to
        // measure is length of its projection onto plane perpendicular to dir.
        float dist_sqr = vector.squaredNorm() - std::pow(vector.dot(this->dir), 2.f);
        if (dist_sqr < this->radius_sqr && t >= 0.f && t <= (b - a).norm())
            return true;
    }
    return false;
}

// BBS
bool TriangleSelector::HeightRange::is_pointer_in_triangle(const Vec3f& p1_, const Vec3f& p2_, const Vec3f& p3_) const
{
    return false;
}

bool TriangleSelector::HeightRange::is_mesh_point_inside(const Vec3f& point) const
{
    // just use 40% edge limit as tolerance
    const float tolerance = VERTEX_TOLERANCE;
    if (clipping_plane.is_active() && clipping_plane.is_mesh_point_clipped(point))
        return false;

    const Vec3f transformed_point = trafo * point;
    const float top_z = m_z_world + m_height + tolerance;
    const float bot_z = m_z_world - tolerance;

    return transformed_point.z() > bot_z && transformed_point.z() < top_z;
}

bool TriangleSelector::HeightRange::is_edge_inside_cursor(const Triangle& tr, const std::vector<Vertex>& vertices) const
{
    const float top_z = m_z_world + m_height + EPSILON;
    const float bot_z = m_z_world - EPSILON;
    std::array<Vec3f, 3> pts;
    bool allVerticesClipped = clipping_plane.is_active();
    for (int i = 0; i < 3; ++i)
    {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!clipping_plane.is_active() || !clipping_plane.is_mesh_point_clipped(pts[i]))
            allVerticesClipped = false;
        pts[i] = this->trafo * pts[i];
    }

    if (allVerticesClipped)
        return false;

    return !((pts[0].z() < bot_z && pts[1].z() < bot_z && pts[2].z() < bot_z) ||
             (pts[0].z() > top_z && pts[1].z() > top_z && pts[2].z() > top_z));
}

// Recursively remove all subtriangles.
void TriangleSelector::undivide_triangle(int facet_idx)
{
    assert(facet_idx < int(m_triangles.size()));
    Triangle& tr = m_triangles[facet_idx];

    if (tr.is_split()) {
        PrepareForLeafIdentityChange();
        const int sourceTriangle = tr.source_triangle;
        for (int i = 0; i <= tr.number_of_split_sides(); ++i) {
            int       child    = tr.children[i];
            Triangle &child_tr = m_triangles[child];
            assert(child_tr.valid());
            undivide_triangle(child);
            for (int j = 0; j < 3; ++j) {
                int     iv = child_tr.verts_idxs[j];
                Vertex &v  = m_vertices[iv];
                assert(v.ref_cnt > 0);
                if (-- v.ref_cnt == 0) {
                    // Release this vertex.
                    // Chain released vertices into a linked list through ref_cnt.
                    assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
                    memcpy(&m_vertices[iv].v[0], &m_free_vertices_head, sizeof(m_free_vertices_head));
                    m_free_vertices_head = iv;
                    assert(m_free_vertices_head >= -1 && m_free_vertices_head < int(m_vertices.size()));
                }
            }
            // Chain released triangles into a linked list through children[0].
            assert(child_tr.valid());
            child_tr.m_valid = false;
            assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
            assert(m_free_triangles_head == -1 || ! m_triangles[m_free_triangles_head].valid());
            child_tr.children[0] = m_free_triangles_head;
            m_free_triangles_head = child;
            assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
            ++m_invalid_triangles;
        }
        tr.set_division(0, 0); // not split
        NotifyTopologyMutation(sourceTriangle);
    }
}

bool TriangleSelector::remove_useless_children(int facet_idx)
{
    // Check that all children are leafs of the same type. If not, try to
    // make them (recursive call). Remove them if sucessful.

    assert(facet_idx < int(m_triangles.size()) && m_triangles[facet_idx].valid());
    Triangle& tr = m_triangles[facet_idx];

    if (! tr.is_split()) {
        // This is a leaf, there nothing to do. This can happen during the
        // first (non-recursive call). Shouldn't otherwise.
        return false;
    }

    bool topologyChanged = false;

    // Call this for all non-leaf children.
    for (int child_idx=0; child_idx<=tr.number_of_split_sides(); ++child_idx) {
        assert(child_idx < int(m_triangles.size()) && m_triangles[child_idx].valid());
        if (m_triangles[tr.children[child_idx]].is_split())
            topologyChanged |= remove_useless_children(tr.children[child_idx]);
    }


    // Return if a child is not leaf or two children differ in type.
    EnforcerBlockerType first_child_type = EnforcerBlockerType::NONE;
    for (int child_idx=0; child_idx<=tr.number_of_split_sides(); ++child_idx) {
        if (m_triangles[tr.children[child_idx]].is_split())
            return topologyChanged;
        if (child_idx == 0)
            first_child_type = m_triangles[tr.children[0]].get_state();
        else if (m_triangles[tr.children[child_idx]].get_state() != first_child_type)
            return topologyChanged;
    }

    // If we got here, the children can be removed.
    const bool semanticStateChanged = CollapseSubtreeToState(facet_idx, first_child_type);
    assert(! semanticStateChanged);
    return true;
}

void TriangleSelector::garbage_collect()
{
    PrepareForLeafIdentityChange();

    // First make a map from old to new triangle indices.
    int new_idx = m_orig_size_indices;
    std::vector<int> new_triangle_indices(m_triangles.size(), -1);
    for (int i = m_orig_size_indices; i<int(m_triangles.size()); ++i)
        if (m_triangles[i].valid())
            new_triangle_indices[i] = new_idx ++;

    // Now we know which vertices are not referenced anymore. Make a map
    // from old idxs to new ones, like we did for triangles.
    new_idx = m_orig_size_vertices;
    std::vector<int> new_vertices_indices(m_vertices.size(), -1);
    for (int i=m_orig_size_vertices; i<int(m_vertices.size()); ++i) {
        assert(m_vertices[i].ref_cnt >= 0);
        if (m_vertices[i].ref_cnt != 0)
            new_vertices_indices[i] = new_idx ++;
    }

    // We can remove all invalid triangles and vertices that are no longer referenced.
    m_triangles.erase(std::remove_if(m_triangles.begin()+m_orig_size_indices, m_triangles.end(),
                          [](const Triangle& tr) { return ! tr.valid(); }),
                      m_triangles.end());
    m_vertices.erase(std::remove_if(m_vertices.begin()+m_orig_size_vertices, m_vertices.end(),
                          [](const Vertex& vert) { return vert.ref_cnt == 0; }),
                      m_vertices.end());

    // Now go through all remaining triangles and update changed indices.
    for (Triangle& tr : m_triangles) {
        assert(tr.valid());

        if (tr.is_split()) {
            // There are children. Update their indices.
            for (int j=0; j<=tr.number_of_split_sides(); ++j) {
                assert(new_triangle_indices[tr.children[j]] != -1);
                tr.children[j] = new_triangle_indices[tr.children[j]];
            }
        }

        // Update indices into m_vertices. The original vertices are never
        // touched and need not be reindexed.
        for (int& idx : tr.verts_idxs) {
            if (idx >= m_orig_size_vertices) {
                assert(new_vertices_indices[idx] != -1);
                idx = new_vertices_indices[idx];
            }
        }
    }

    m_invalid_triangles = 0;
    m_free_triangles_head = -1;
    m_free_vertices_head = -1;
    NotifyIndexRebuild();
}

void TriangleSelector::remap_triangle_state(const EnforcerBlockerStateMap& state_map)
{
    if (m_triangles.empty())
        return;

    std::atomic<bool> stateChanged(false);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, m_triangles.size()),
                      [this, &state_map, &stateChanged](const tbb::blocked_range<size_t>& range) {
        for (size_t i = range.begin(); i != range.end(); ++i) {
            Triangle& tr = m_triangles[i];
            if (tr.valid() && !tr.is_split()) {
                const size_t currentState = static_cast<size_t>(tr.get_state());
                const EnforcerBlockerType newState = state_map[currentState];
                if (newState != tr.get_state()) {
                    tr.set_state(newState);
                    stateChanged.store(true, std::memory_order_relaxed);
                }
            }
        }
    });

    if (stateChanged.load(std::memory_order_relaxed))
        NotifyAllStateMutation();
}

TriangleSelector::TriangleSelector(const TriangleMesh& mesh, float edge_limit)
    : m_mesh{mesh}, m_neighbors(its_face_neighbors(mesh.its)), m_face_normals(its_face_normals(mesh.its)), m_edge_limit(edge_limit)
{
    reset();
}

void TriangleSelector::reset()
{
    PrepareForLeafIdentityChange();
    ClearDeferredCleanup();
    m_vertices.clear();
    m_triangles.clear();
    m_invalid_triangles = 0;
    m_free_triangles_head = -1;
    m_free_vertices_head = -1;
    m_vertices.reserve(m_mesh.its.vertices.size());
    for (const stl_vertex& vert : m_mesh.its.vertices)
        m_vertices.emplace_back(vert);
    m_triangles.reserve(m_mesh.its.indices.size());
    for (size_t i = 0; i < m_mesh.its.indices.size(); ++i) {
        const stl_triangle_vertex_indices &ind = m_mesh.its.indices[i];
        push_triangle(ind[0], ind[1], ind[2], int(i));
    }
    m_orig_size_vertices = int(m_vertices.size());
    m_orig_size_indices  = int(m_triangles.size());
    NotifyFullReset();
}

void TriangleSelector::set_edge_limit(float edge_limit)
{
    m_edge_limit_sqr = std::pow(edge_limit, 2.f);
}

int TriangleSelector::push_triangle(int a, int b, int c, int source_triangle, const EnforcerBlockerType state)
{
    for (int i : {a, b, c}) {
        assert(i >= 0 && i < int(m_vertices.size()));
        ++m_vertices[i].ref_cnt;
    }
    int idx;
    if (m_free_triangles_head == -1) {
        // Allocate a new triangle.
        assert(m_invalid_triangles == 0);
        idx = int(m_triangles.size());
        m_triangles.emplace_back(a, b, c, source_triangle, state);
    } else {
        // Reuse triangle from the free list.
        assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
        assert(! m_triangles[m_free_triangles_head].valid());
        assert(m_invalid_triangles > 0);
        idx = m_free_triangles_head;
        m_free_triangles_head = m_triangles[idx].children[0];
        -- m_invalid_triangles;
        assert(m_free_triangles_head >= -1 && m_free_triangles_head < int(m_triangles.size()));
        assert(m_free_triangles_head == -1 || ! m_triangles[m_free_triangles_head].valid());
        assert(m_invalid_triangles >= 0);
        assert((m_invalid_triangles == 0) == (m_free_triangles_head == -1));
        m_triangles[idx] = {a, b, c, source_triangle, state};
    }
    assert(m_triangles[idx].valid());
    return idx;
}

// called by deserialize() and select_patch()->select_triangle()->...select_triangle()->split_triangle()
// Split a triangle based on Triangle::number_of_split_sides() and Triangle::special_side()
// by allocating child triangles and midpoint vertices.
// Midpoint vertices are possibly reused by traversing children of neighbor triangles.
void TriangleSelector::perform_split(int facet_idx, const Vec3i32 &neighbors, EnforcerBlockerType old_state)
{
    assert(m_seedFillSelectedLeaves.empty());

    // Reserve space for the new triangles upfront, so that the reference to this triangle will not change.
    {
        size_t num_triangles_new = m_triangles.size() + m_triangles[facet_idx].number_of_split_sides() + 1;
        if (m_triangles.capacity() < num_triangles_new)
            m_triangles.reserve(next_highest_power_of_2(num_triangles_new));
    }

    Triangle &tr = m_triangles[facet_idx];
    assert(tr.is_split());

    // indices of triangle vertices
#ifdef NDEBUG
    boost::container::small_vector<int, 6> verts_idxs;
#else // NDEBUG
    // For easier debugging.
    std::vector<int> verts_idxs;
    verts_idxs.reserve(6);
#endif // NDEBUG
    for (int j=0, idx = tr.special_side(); j<3; ++j, idx = next_idx_modulo(idx, 3))
        verts_idxs.push_back(tr.verts_idxs[idx]);

    auto get_alloc_vertex = [this, &neighbors, &verts_idxs](int edge, int i1, int i2) -> int {
        return this->triangle_midpoint_or_allocate(neighbors(edge), verts_idxs[i1], verts_idxs[i2]);
    };

    int ichild = 0;
    switch (tr.number_of_split_sides()) {
    case 1:
        verts_idxs.insert(verts_idxs.begin()+2, get_alloc_vertex(next_idx_modulo(tr.special_side(), 3), 2, 1));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[2], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[2], verts_idxs[3], verts_idxs[0], tr.source_triangle, old_state);
        break;

    case 2:
        verts_idxs.insert(verts_idxs.begin()+1, get_alloc_vertex(tr.special_side(), 1, 0));
        verts_idxs.insert(verts_idxs.begin()+4, get_alloc_vertex(prev_idx_modulo(tr.special_side(), 3), 0, 3));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[4], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[1], verts_idxs[2], verts_idxs[4], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[2], verts_idxs[3], verts_idxs[4], tr.source_triangle, old_state);
        break;

    case 3:
        assert(tr.special_side() == 0);
        verts_idxs.insert(verts_idxs.begin()+1, get_alloc_vertex(0, 1, 0));
        verts_idxs.insert(verts_idxs.begin()+3, get_alloc_vertex(1, 3, 2));
        verts_idxs.insert(verts_idxs.begin()+5, get_alloc_vertex(2, 0, 4));
        tr.children[ichild ++] = push_triangle(verts_idxs[0], verts_idxs[1], verts_idxs[5], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[1], verts_idxs[2], verts_idxs[3], tr.source_triangle, old_state);
        tr.children[ichild ++] = push_triangle(verts_idxs[3], verts_idxs[4], verts_idxs[5], tr.source_triangle, old_state);
        tr.children[ichild   ] = push_triangle(verts_idxs[1], verts_idxs[3], verts_idxs[5], tr.source_triangle, old_state);
        break;

    default:
        break;
    }

#ifndef NDEBUG
    assert(this->verify_triangle_neighbors(tr, neighbors));
    for (int i = 0; i <= tr.number_of_split_sides(); ++i) {
        Vec3i32 n = this->child_neighbors(tr, neighbors, i);
        assert(this->verify_triangle_neighbors(m_triangles[tr.children[i]], n));
    }
#endif // NDEBUG
}

bool TriangleSelector::has_facets(EnforcerBlockerType state) const
{
    for (const Triangle& tr : m_triangles)
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state)
            return true;
    return false;
}

int TriangleSelector::num_facets(EnforcerBlockerType state) const
{
    int cnt = 0;
    for (const Triangle& tr : m_triangles)
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state)
            ++ cnt;
    return cnt;
}

indexed_triangle_set TriangleSelector::get_facets(EnforcerBlockerType state) const
{
    indexed_triangle_set out;
    std::vector<int> vertex_map(m_vertices.size(), -1);
    for (const Triangle& tr : m_triangles) {
        if (tr.valid() && ! tr.is_split() && tr.get_state() == state) {
            stl_triangle_vertex_indices indices;
            for (int i=0; i<3; ++i) {
                int j = tr.verts_idxs[i];
                if (vertex_map[j] == -1) {
                    vertex_map[j] = int(out.vertices.size());
                    out.vertices.emplace_back(m_vertices[j].v);
                }
                indices[i] = vertex_map[j];
            }
            out.indices.emplace_back(indices);
        }
    }
    return out;
}

// BBS
void TriangleSelector::get_facets(std::vector<indexed_triangle_set>& facets_per_type) const
{
    facets_per_type.clear();

    int max_state = int(EnforcerBlockerType::NONE);
    for (const Triangle &tr : m_triangles)
        if (tr.valid() && !tr.is_split())
            max_state = std::max(max_state, int(tr.get_state()));

    for (int type = int(EnforcerBlockerType::NONE); type <= max_state; ++type) {
        facets_per_type.emplace_back();
        indexed_triangle_set& its = facets_per_type.back();
        std::vector<int> vertex_map(m_vertices.size(), -1);

        for (const Triangle& tr : m_triangles) {
            if (tr.valid() && !tr.is_split() && tr.get_state() == (EnforcerBlockerType)type) {
                stl_triangle_vertex_indices indices;
                for (int i = 0; i < 3; ++i) {
                    int j = tr.verts_idxs[i];
                    if (vertex_map[j] == -1) {
                        vertex_map[j] = int(its.vertices.size());
                        its.vertices.emplace_back(m_vertices[j].v);
                    }
                    indices[i] = vertex_map[j];
                }
                its.indices.emplace_back(indices);
            }
        }
    }
}

indexed_triangle_set TriangleSelector::get_facets_strict(EnforcerBlockerType state) const
{
    indexed_triangle_set out;

    size_t num_vertices = 0;
    for (const Vertex &v : m_vertices)
        if (v.ref_cnt > 0)
            ++ num_vertices;
    out.vertices.reserve(num_vertices);
    std::vector<int> vertex_map(m_vertices.size(), -1);
    for (size_t i = 0; i < m_vertices.size(); ++ i)
        if (const Vertex &v = m_vertices[i]; v.ref_cnt > 0) {
            vertex_map[i] = int(out.vertices.size());
            out.vertices.emplace_back(v.v);
        }

    for (int itriangle = 0; itriangle < m_orig_size_indices; ++ itriangle)
        this->get_facets_strict_recursive(m_triangles[itriangle], m_neighbors[itriangle], state, out.indices);

    for (auto &triangle : out.indices)
        for (int i = 0; i < 3; ++ i)
            triangle(i) = vertex_map[triangle(i)];

    return out;
}

void TriangleSelector::get_facets_strict_recursive(
    const Triangle                              &tr,
    const Vec3i32                                 &neighbors,
    EnforcerBlockerType                          state,
    std::vector<stl_triangle_vertex_indices>    &out_triangles) const
{
    if (tr.is_split()) {
        for (int i = 0; i <= tr.number_of_split_sides(); ++ i)
            this->get_facets_strict_recursive(
                m_triangles[tr.children[i]],
                this->child_neighbors(tr, neighbors, i),
                state, out_triangles);
    } else if (tr.get_state() == state)
        this->get_facets_split_by_tjoints({tr.verts_idxs[0], tr.verts_idxs[1], tr.verts_idxs[2]}, neighbors, out_triangles);
}

void TriangleSelector::get_facets_split_by_tjoints(const Vec3i32 &vertices, const Vec3i32 &neighbors, std::vector<stl_triangle_vertex_indices> &out_triangles) const
{
// Export this triangle, but first collect the T-joint vertices along its edges.
    Vec3i32 midpoints(
        this->triangle_midpoint(neighbors(0), vertices(1), vertices(0)),
        this->triangle_midpoint(neighbors(1), vertices(2), vertices(1)),
        this->triangle_midpoint(neighbors(2), vertices(0), vertices(2)));
    int splits = (midpoints(0) != -1) + (midpoints(1) != -1) + (midpoints(2) != -1);
    switch (splits) {
    case 0:
        // Just emit this triangle.
        out_triangles.emplace_back(vertices(0), vertices(1), vertices(2));
        break;
    case 1:
    {
        // Split to two triangles
        int i = midpoints(0) != -1 ? 2 : midpoints(1) != -1 ? 0 : 1;
        int j = next_idx_modulo(i, 3);
        int k = next_idx_modulo(j, 3);
        this->get_facets_split_by_tjoints(
            { vertices(i), vertices(j), midpoints(j) },
            { neighbors(i),
              this->neighbor_child(neighbors(j), vertices(k), vertices(j), Partition::Second),
              -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(j), vertices(k), vertices(i) },
            { this->neighbor_child(neighbors(j), vertices(k), vertices(j), Partition::First),
              neighbors(k),
              -1 },
              out_triangles);
        break;
    }
    case 2:
    {
        // Split to three triangles.
        int i = midpoints(0) == -1 ? 2 : midpoints(1) == -1 ? 0 : 1;
        int j = next_idx_modulo(i, 3);
        int k = next_idx_modulo(j, 3);
        this->get_facets_split_by_tjoints(
            { vertices(i), midpoints(i), midpoints(k) },
            { this->neighbor_child(neighbors(i), vertices(j), vertices(i), Partition::Second),
              -1,
              this->neighbor_child(neighbors(k), vertices(i), vertices(k), Partition::First) },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(i), vertices(j), midpoints(k) },
            { this->neighbor_child(neighbors(i), vertices(j), vertices(i), Partition::First),
              -1, -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { vertices(j), vertices(k), midpoints(k) },
            { neighbors(j),
              this->neighbor_child(neighbors(k), vertices(i), vertices(k), Partition::Second),
              -1 },
              out_triangles);
        break;
    }
    default:
        assert(splits == 3);
        // Split to 4 triangles.
        this->get_facets_split_by_tjoints(
            { vertices(0), midpoints(0), midpoints(2) },
            { this->neighbor_child(neighbors(0), vertices(1), vertices(0), Partition::Second),
              -1, 
              this->neighbor_child(neighbors(2), vertices(0), vertices(2), Partition::First) },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(0), vertices(1), midpoints(1) },
            { this->neighbor_child(neighbors(0), vertices(1), vertices(0), Partition::First),
              this->neighbor_child(neighbors(1), vertices(2), vertices(1), Partition::Second),
              -1 },
              out_triangles);
        this->get_facets_split_by_tjoints(
            { midpoints(1), vertices(2), midpoints(2) },
            { this->neighbor_child(neighbors(1), vertices(2), vertices(1), Partition::First),
              this->neighbor_child(neighbors(2), vertices(0), vertices(2), Partition::Second),
              -1 },
              out_triangles);
        out_triangles.emplace_back(midpoints);
        break;
    }
}

std::vector<Vec2i32> TriangleSelector::get_seed_fill_contour() const
{
    if (m_seedFillSelectedLeaves.empty())
        return {};

    // No subdivision anywhere: check the three original neighbors directly,
    // skipping the roots array and the recursive walk entirely.
    if (CanUseOriginalMeshFillPath())
        return GetOriginalMeshSeedFillContour();

    std::vector<uint32_t> sourceRoots;
    sourceRoots.reserve(m_seedFillSelectedLeaves.size());
    bool originalLeavesOnly = true;
    for (int triangleIndex : m_seedFillSelectedLeaves)
    {
        if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
            continue;

        const Triangle& triangle = m_triangles[triangleIndex];
        if (!triangle.valid() || triangle.is_split() || !triangle.is_selected_by_seed_fill() ||
            triangle.source_triangle < 0 || triangle.source_triangle >= m_orig_size_indices)
            continue;

        sourceRoots.push_back(static_cast<uint32_t>(triangle.source_triangle));
        // Original leaves are pairwise distinct roots already: skip sort/unique.
        originalLeavesOnly &= triangleIndex == triangle.source_triangle;
    }

    if (!originalLeavesOnly)
    {
        std::sort(sourceRoots.begin(), sourceRoots.end());
        sourceRoots.erase(std::unique(sourceRoots.begin(), sourceRoots.end()), sourceRoots.end());
    }

    std::vector<Vec2i32> edgesOut;
    AppendSeedFillContourForUniqueRoots(sourceRoots, edgesOut);
    return edgesOut;
}

// Roots must already be range-checked and unique; appends contour edges without copying them again.
void TriangleSelector::AppendSeedFillContourForUniqueRoots(const std::vector<uint32_t>& sourceRoots,
                                                           std::vector<Vec2i32>& edgesOut) const
{
    for (uint32_t sourceRoot : sourceRoots)
    {
        const int rootIndex = static_cast<int>(sourceRoot);
        const Vec3i32 neighbors = m_neighbors[rootIndex];
        assert(verify_triangle_neighbors(m_triangles[rootIndex], neighbors));
        get_seed_fill_contour_recursive(rootIndex, neighbors, neighbors, edgesOut);
    }
}

bool TriangleSelector::CanUseOriginalMeshFillPath() const noexcept
{
    // Original roots stay at the front of m_triangles; equal sizes mean no valid children.
    return m_orig_size_indices >= 0 &&
           m_triangles.size() == static_cast<size_t>(m_orig_size_indices) &&
           m_neighbors.size() == static_cast<size_t>(m_orig_size_indices) &&
           m_invalid_triangles == 0;
}

std::vector<Vec2i32> TriangleSelector::GetOriginalMeshSeedFillContour() const
{
    assert(CanUseOriginalMeshFillPath());
    std::vector<Vec2i32> edges;

    for (int facet : m_seedFillSelectedLeaves)
    {
        if (facet < 0 || facet >= m_orig_size_indices)
            continue;

        const Triangle& triangle = m_triangles[facet];
        if (!triangle.valid() || triangle.is_split() || !triangle.is_selected_by_seed_fill())
            continue;

        for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex) {
            const int neighbor = m_neighbors[facet](edgeIndex);
            // Baseline open boundaries produce no edge; keep that behavior.
            if (neighbor < 0)
                continue;

            assert(neighbor < m_orig_size_indices);
            assert(!m_triangles[neighbor].is_split());

            if (!m_triangles[neighbor].is_selected_by_seed_fill())
                edges.emplace_back(triangle.verts_idxs[edgeIndex],
                                   triangle.verts_idxs[(edgeIndex + 1) % 3]);
        }
    }

    return edges;
}

void TriangleSelector::get_seed_fill_contour_recursive(const int facet_idx, const Vec3i32 &neighbors, const Vec3i32 &neighbors_propagated, std::vector<Vec2i32> &edges_out) const {
    assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
    assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
    const Triangle *tr = &m_triangles[facet_idx];
    if (!tr->valid())
        return;

    if (tr->is_split()) {
        int num_of_children = tr->number_of_split_sides() + 1;
        if (num_of_children != 1) {
            for (int i = 0; i < num_of_children; ++i) {
                assert(i < int(tr->children.size()));
                assert(tr->children[i] < int(m_triangles.size()));
                // Recursion, deep first search over the children of this triangle.
                // All children of this triangle were created by splitting a single source triangle of the original mesh.
                const Vec3i32 child_neighbors = this->child_neighbors(*tr, neighbors, i);
                this->get_seed_fill_contour_recursive(tr->children[i], child_neighbors,
                                                      this->child_neighbors_propagated(*tr, neighbors_propagated, i, child_neighbors), edges_out);
            }
        }
    } else if (tr->is_selected_by_seed_fill()) {
        Vec3i32 vertices = {m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2]};
        append_touching_edges(neighbors(0), vertices(1), vertices(0), edges_out);
        append_touching_edges(neighbors(1), vertices(2), vertices(1), edges_out);
        append_touching_edges(neighbors(2), vertices(0), vertices(2), edges_out);

        // It appends the edges that are touching the triangle only by part of the edge that means the triangles are from lower depth.
        for (int idx = 0; idx < 3; ++idx)
            if (int neighbor_tr_idx = neighbors_propagated(idx); neighbor_tr_idx != -1 && !m_triangles[neighbor_tr_idx].is_split() && !m_triangles[neighbor_tr_idx].is_selected_by_seed_fill())
                edges_out.emplace_back(vertices(idx), vertices(next_idx_modulo(idx, 3)));
    }
}

std::vector<int> TriangleSelector::get_facets_indices_by_state(EnforcerBlockerType state) const
{
    std::vector<int> out;
    for (int i = 0; i < m_orig_size_indices; ++i) {
        // Walk the whole subtree of the original triangle; any leaf in the wanted
        // state marks the original triangle as selected.
        bool              has_state = false;
        std::vector<int>  stack{i};
        while (! stack.empty()) {
            const Triangle& tr = m_triangles[stack.back()];
            stack.pop_back();
            if (tr.is_split()) {
                for (int j = 0; j <= tr.number_of_split_sides(); ++j)
                    stack.push_back(tr.children[j]);
            } else if (tr.get_state() == state) {
                has_state = true;
                break;
            }
        }
        if (has_state)
            out.push_back(i);
    }
    return out;
}

TriangleSelector::TriangleSplittingData TriangleSelector::serialize() const {
    // Each original triangle of the mesh is assigned a number encoding its state
    // or how it is split. Each triangle is encoded by 4 bits (xxyy) or by
    // 4 bits plus one or more extension nibbles:
    // leaf triangle: xx = EnforcerBlockerType (Only values 0, 1, and 2. Value 3 is used as an indicator for additional 4 bits.), yy = 0
    // leaf triangle: xx = 0b11, yy = 0b00, zzzz... = EnforcerBlockerType (subtracted by 3) in base-15 chunks
    // non-leaf:      xx = special side, yy = number of split sides
    // These are bitwise appended and formed into one 64-bit integer.

    // The function returns a map from original triangle indices to
    // stream of bits encoding state and offsprings.

    // Using an explicit function object to support recursive call of Serializer::serialize().
    // This is cheaper than the previous implementation using a recursive call of type erased std::function.
    // (std::function calls using a pointer, while this implementation calls directly).
    struct Serializer {
        const TriangleSelector* triangle_selector;
        TriangleSplittingData data;

        void serialize(int facet_idx) {
            const Triangle& tr = triangle_selector->m_triangles[facet_idx];

            // Always save number of split sides. It is zero for unsplit triangles.
            int split_sides = tr.number_of_split_sides();
            assert(split_sides >= 0 && split_sides <= 3);

            data.bitstream.push_back(split_sides & 0b01);
            data.bitstream.push_back(split_sides & 0b10);

            if (split_sides) {
                // If this triangle is split, save which side is split (in case
                // of one split) or kept (in case of two splits). The value will
                // be ignored for 3-side split.
                assert(tr.is_split() && split_sides > 0);
                assert(tr.special_side() >= 0 && tr.special_side() <= 3);
                data.bitstream.push_back(tr.special_side() & 0b01);
                data.bitstream.push_back(tr.special_side() & 0b10);
                // Now save all children.
                // Serialized in reverse order for compatibility with PrusaSlicer 2.3.1.
                for (int child_idx = split_sides; child_idx >= 0; -- child_idx)
                    this->serialize(tr.children[child_idx]);
            } else {
                // In case this is leaf, we better save information about its state.
                int n = int(tr.get_state());
                if (n <= static_cast<size_t>(EnforcerBlockerType::ExtruderMax))
                    data.used_states[n] = true;

                if (n >= 3) {
                    // Store "11" plus one or more 4-bit chunks of (n - 3), where
                    // 0b1111 indicates that another chunk follows.
                    data.bitstream.insert(data.bitstream.end(), { true, true });
                    n -= 3;
                    while (n >= 15) {
                        for (size_t bit_idx = 0; bit_idx < 4; ++bit_idx)
                            data.bitstream.push_back(uint64_t(0b1111) & (uint64_t(0b0001) << bit_idx));
                        n -= 15;
                    }
                    for (size_t bit_idx = 0; bit_idx < 4; ++bit_idx)
                        data.bitstream.push_back(n & (uint64_t(0b0001) << bit_idx));
                } else {
                    // Simple case, compatible with PrusaSlicer 2.3.1 and older for storing paint on supports and seams.
                    // Store 2 bits of n.
                    data.bitstream.push_back(n & 0b01);
                    data.bitstream.push_back(n & 0b10);
                }
            }
        }
    } out { this };

    out.data.triangles_to_split.reserve(m_orig_size_indices);
    for (int i=0; i<m_orig_size_indices; ++i)
        if (const Triangle& tr = m_triangles[i]; tr.is_split() || tr.get_state() != EnforcerBlockerType::NONE) {
            // Store index of the first bit assigned to ith triangle.
            out.data.triangles_to_split.emplace_back(i, int(out.data.bitstream.size()));
            // out the triangle bits.
            out.serialize(i);
        }

    // May be stored onto Undo / Redo stack, thus conserve memory.
    out.data.triangles_to_split.shrink_to_fit();
    out.data.bitstream.shrink_to_fit();
    return out.data;
}

void TriangleSelector::deserialize(const TriangleSplittingData& data,
                                   bool                         needs_reset,
                                   EnforcerBlockerType          max_ebt,
                                   EnforcerBlockerType          to_delete_filament,
                                   EnforcerBlockerType          replace_filament,
                                   const EnforcerBlockerStateMap* state_map)
{
    PrepareForLeafIdentityChange();
    ClearDeferredCleanup();
    if (needs_reset)
        reset(); // dump any current state
    for (auto [triangle_id, ibit] : data.triangles_to_split) {
        if (triangle_id >= int(m_triangles.size())) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "array bound:error:triangle_id >= int(m_triangles.size())";
            return;
        }
    }
    // Reserve number of triangles as if each triangle was saved with 4 bits.
    // With MMU painting this estimate may be somehow low, but better than nothing.
    m_triangles.reserve(std::max(m_mesh.its.indices.size(), data.bitstream.size() / 4));
    // Number of triangles is twice the number of vertices on a large manifold mesh of genus zero.
    // Here the triangles count account for both the nodes and leaves, thus the following line may overestimate.
    m_vertices.reserve(std::max(m_mesh.its.vertices.size(), m_triangles.size() / 2));

    // Vector to store all parents that have offsprings.
    struct ProcessingInfo {
        int facet_id = 0;
        Vec3i32 neighbors { -1, -1, -1 };
        int processed_children = 0;
        int total_children = 0;
    };
    // Depth-first queue of a source mesh triangle and its childern.
    // kept outside of the loop to avoid re-allocating inside the loop.
    std::vector<ProcessingInfo> parents;

    for (auto [triangle_id, ibit] : data.triangles_to_split) {
        assert(triangle_id < int(m_triangles.size()));
        assert(ibit < int(data.bitstream.size()));
        auto next_nibble = [&data, &ibit = ibit]() {
            int n = 0;
            for (int i = 0; i < 4; ++ i)
                n |= data.bitstream[ibit ++] << i;
            return n;
        };

        parents.clear();
        while (true) {
            // Read next triangle info.
            int code = next_nibble();
            int num_of_split_sides = code & 0b11;
            int num_of_children = num_of_split_sides == 0 ? 0 : num_of_split_sides + 1;
            bool is_split = num_of_children != 0;
            // Only valid if not is_split. Value of the second nibble was subtracted by 3, so it is added back.
            // auto state = is_split ? EnforcerBlockerType::NONE : EnforcerBlockerType((code & 0b1100) == 0b1100 ? next_nibble() + 3 : code >> 2);
            auto state = EnforcerBlockerType::NONE;
            //// BBS
            //if (state > max_ebt)
            //    state = EnforcerBlockerType::NONE;

            if (!is_split) {
                if ((code & 0b1100) == 0b1100) {
                    int next_code = next_nibble();
                    int num       = 0;
                    while (next_code == 0b1111) {
                        num++;
                        next_code = next_nibble();
                    }
                    state = EnforcerBlockerType(next_code + 15 * num + 3); // old:next_nibble() + 3;
                } else {
                    state = EnforcerBlockerType(code >> 2);
                }
            }

            if (state_map != nullptr && state != EnforcerBlockerType::NONE) {
                const size_t state_idx = static_cast<size_t>(state);
                state = state_idx < state_map->size() ? (*state_map)[state_idx] : EnforcerBlockerType::NONE;
            } else {
                if (state == to_delete_filament)
                    state = replace_filament;
                else if (to_delete_filament != EnforcerBlockerType::NONE && state != EnforcerBlockerType::NONE) {
                    state = state > to_delete_filament ? EnforcerBlockerType((int) state - 1) : state;
                }
            }

            if (state > max_ebt) {
                assert(false);
                state = EnforcerBlockerType::NONE;
            }

            // Only valid if is_split.
            int special_side = code >> 2;

            // Take care of the first iteration separately, so handling of the others is simpler.
            if (parents.empty()) {
                if (is_split) {
                    // root is split, add it into list of parents and split it.
                    // then go to the next.
                    Vec3i32 neighbors = m_neighbors[triangle_id];
                    parents.push_back({triangle_id, neighbors, 0, num_of_children});
                    m_triangles[triangle_id].set_division(num_of_split_sides, special_side);
                    perform_split(triangle_id, neighbors, EnforcerBlockerType::NONE);
                    continue;
                } else {
                    // root is not split. just set the state and that's it.
                    m_triangles[triangle_id].set_state(state);
                    break;
                }
            }

            // This is not the first iteration. This triangle is a child of last seen parent.
            assert(! parents.empty());
            assert(parents.back().processed_children < parents.back().total_children);

            if (ProcessingInfo& last = parents.back();  is_split) {
                // split the triangle and save it as parent of the next ones.
                const Triangle &tr = m_triangles[last.facet_id];
                int   child_idx = last.total_children - last.processed_children - 1;
                Vec3i32 neighbors = this->child_neighbors(tr, last.neighbors, child_idx);
                int this_idx = tr.children[child_idx];
                m_triangles[this_idx].set_division(num_of_split_sides, special_side);
                perform_split(this_idx, neighbors, EnforcerBlockerType::NONE);
                parents.push_back({this_idx, neighbors, 0, num_of_children});
            } else {
                // this triangle belongs to last split one
                int child_idx = last.total_children - last.processed_children - 1;
                m_triangles[m_triangles[last.facet_id].children[child_idx]].set_state(state);
                ++last.processed_children;
            }

            // If all children of the past parent triangle are claimed, move to grandparent.
            while (parents.back().processed_children == parents.back().total_children) {
                parents.pop_back();

                if (parents.empty())
                    break;

                // And increment the grandparent children counter, because
                // we have just finished that branch and got back here.
                ++parents.back().processed_children;
            }

            // In case we popped back the root, we should be done.
            if (parents.empty())
                break;
        }
    }

    NotifyFullReset();
}

void TriangleSelector::TriangleSplittingData::update_used_states(const size_t bitstream_start_idx) {
    assert(bitstream_start_idx < this->bitstream.size());
    assert(!this->bitstream.empty() && this->bitstream.size() != bitstream_start_idx);
    assert((this->bitstream.size() - bitstream_start_idx) % 4 == 0);

    if (this->bitstream.empty() || this->bitstream.size() == bitstream_start_idx)
        return;

    size_t nibble_idx = bitstream_start_idx;

    auto read_next_nibble = [&data_bitstream = std::as_const(this->bitstream), &nibble_idx]() -> uint8_t {
        assert(nibble_idx + 3 < data_bitstream.size());
        uint8_t code = 0;
        for (size_t bit_idx = 0; bit_idx < 4; ++bit_idx)
            code |= data_bitstream[nibble_idx++] << bit_idx;
        return code;
    };

    while (nibble_idx < this->bitstream.size()) {
        const uint8_t code = read_next_nibble();

        if (const bool is_split = (code & 0b11) != 0; is_split)
            continue;

        size_t facet_state = code >> 2;
        if ((code & 0b1100) == 0b1100) {
            size_t extension_count = 0;
            size_t next_code = read_next_nibble();
            while (next_code == 0b1111) {
                ++extension_count;
                next_code = read_next_nibble();
            }
            facet_state = next_code + 15 * extension_count + 3;
        }
        assert(facet_state < this->used_states.size());
        if (facet_state >= this->used_states.size())
            continue;

        this->used_states[facet_state] = true;
    }
}

// Lightweight variant of deserialization, which only tests whether a face of test_state exists.
bool TriangleSelector::has_facets(const TriangleSplittingData &data, const EnforcerBlockerType test_state) {
    // Depth-first queue of a number of unvisited children.
    // Kept outside of the loop to avoid re-allocating inside the loop.
    std::vector<int> parents_children;
    parents_children.reserve(64);

    for (const TriangleBitStreamMapping &triangle_id_and_ibit : data.triangles_to_split) {
        int ibit = triangle_id_and_ibit.bitstream_start_idx;
        assert(ibit < int(data.bitstream.size()));
        auto next_nibble = [&data, &ibit = ibit]() {
            int n = 0;
            for (int i = 0; i < 4; ++ i)
                n |= data.bitstream[ibit ++] << i;
            return n;
        };
        // < 0 -> negative of a number of children
        // >= 0 -> state
        auto num_children_or_state = [&next_nibble]() -> int {
            int code               = next_nibble();
            int num_of_split_sides = code & 0b11;
            if (num_of_split_sides != 0)
                return - num_of_split_sides - 1;

            if ((code & 0b1100) != 0b1100)
                return code >> 2;

            int extension_count = 0;
            int next_code = next_nibble();
            while (next_code == 0b1111) {
                ++extension_count;
                next_code = next_nibble();
            }
            return next_code + 15 * extension_count + 3;
        };

        int state = num_children_or_state();
        if (state < 0) {
            // Root is split.
            parents_children.clear();
            parents_children.emplace_back(- state);
            do {
                if (-- parents_children.back() >= 0) {
                    int state = num_children_or_state();
                    if (state < 0)
                        // Child is split.
                        parents_children.emplace_back(- state);
                    else if (state == int(test_state))
                        // Child is not split and a face of test_state was found.
                        return true;
                } else
                    parents_children.pop_back();
            } while (! parents_children.empty());
        } else if (state == int(test_state))
            // Root is not split and a face of test_state was found.
            return true;
    }

    return false;
}

void TriangleSelector::seed_fill_unselect_all_triangles()
{
    // Only a non-empty preview removal changes the preview revision.
    if (!m_seedFillSelectedLeaves.empty())
        ++m_seedFillPreviewRevision;

    for (int triangleIndex : m_seedFillSelectedLeaves)
    {
        if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
            continue;

        Triangle& triangle = m_triangles[triangleIndex];
        if (triangle.valid() && !triangle.is_split() && triangle.is_selected_by_seed_fill())
            triangle.unselect_by_seed_fill();
    }
    m_seedFillSelectedLeaves.clear();
}

void TriangleSelector::seed_fill_apply_on_triangles(EnforcerBlockerType new_state, CleanupMode cleanupMode)
{
    std::vector<uint32_t> touchedRoots;
    touchedRoots.reserve(m_seedFillSelectedLeaves.size());
    bool originalLeavesOnly = true;
    for (int triangleIndex : m_seedFillSelectedLeaves)
    {
        if (triangleIndex < 0 || triangleIndex >= static_cast<int>(m_triangles.size()))
            continue;

        Triangle& triangle = m_triangles[triangleIndex];
        if (!triangle.valid() || triangle.is_split() || !triangle.is_selected_by_seed_fill() ||
            triangle.get_state() == new_state)
            continue;
        if (triangle.source_triangle < 0 || triangle.source_triangle >= m_orig_size_indices)
            continue;

        triangle.set_state(new_state);
        touchedRoots.push_back(static_cast<uint32_t>(triangle.source_triangle));
        originalLeavesOnly &= triangleIndex == triangle.source_triangle;
    }

    seed_fill_unselect_all_triangles();
    if (touchedRoots.empty())
        return;

    // Changed original leaves are pairwise distinct roots already: skip sort/unique.
    if (!originalLeavesOnly)
    {
        std::sort(touchedRoots.begin(), touchedRoots.end());
        touchedRoots.erase(std::unique(touchedRoots.begin(), touchedRoots.end()), touchedRoots.end());
    }

    ++m_stateRevision;
    for (uint32_t sourceTriangle : touchedRoots)
    {
        OnSelectorMutation(MutationKind::State, static_cast<int>(sourceTriangle));
        RecordDeferredCleanupRoot(sourceTriangle);
    }

    if (cleanupMode == CleanupMode::Immediate)
        FlushDeferredCleanup();
}

TriangleSelector::Cursor::Cursor(const Vec3f &source_, float radius_world, const Transform3d &trafo_, const ClippingPlane &clipping_plane_)
    : source{source_}, trafo{trafo_.cast<float>()}, clipping_plane{clipping_plane_}
{
    Vec3d sf = Geometry::Transformation(trafo_).get_scaling_factor();
    if (is_approx(sf(0), sf(1)) && is_approx(sf(1), sf(2))) {
        radius          = float(radius_world / sf(0));
        radius_sqr      = float(Slic3r::sqr(radius_world / sf(0)));
        meshUnitsPerMm  = float(1. / sf(0));
        uniform_scaling = true;
    } else {
        // In case that the transformation is non-uniform, all checks whether
        // something is inside the cursor should be done in world coords.
        // First transform source in world coords and remember that we did this.
        source          = trafo * source;
        uniform_scaling = false;
        radius          = radius_world;
        radius_sqr      = Slic3r::sqr(radius_world);
        trafo_normal    = trafo.linear().inverse().transpose();
    }
}

TriangleSelector::SinglePointCursor::SinglePointCursor(const Vec3f& center_, const Vec3f& source_, float radius_world, const Transform3d& trafo_, const ClippingPlane &clipping_plane_)
    : center{center_}, Cursor(source_, radius_world, trafo_, clipping_plane_)
{
    // In case that the transformation is non-uniform, all checks whether
    // something is inside the cursor should be done in world coords.
    // Because of the center is transformed.
    if (!uniform_scaling)
        center = trafo * center;

    // Calculate dir, in whatever coords is appropriate.
    dir = (center - source).normalized();
}

TriangleSelector::DoublePointCursor::DoublePointCursor(const Vec3f &first_center_, const Vec3f &second_center_, const Vec3f &source_, float radius_world, const Transform3d &trafo_, const ClippingPlane &clipping_plane_)
    : first_center{first_center_}, second_center{second_center_}, Cursor(source_, radius_world, trafo_, clipping_plane_)
{
    if (!uniform_scaling) {
        first_center  = trafo * first_center_;
        second_center = trafo * second_center_;
    }

    // Calculate dir, in whatever coords is appropriate.
    dir = (first_center - source).normalized();
}

// Returns true if clipping plane is not active or if the point not clipped by clipping plane.
inline static bool is_mesh_point_not_clipped(const Vec3f &point, const TriangleSelector::ClippingPlane &clipping_plane)
{
    return !clipping_plane.is_active() || !clipping_plane.is_mesh_point_clipped(point);
}

// Is a point (in mesh coords) inside a Sphere cursor?
bool TriangleSelector::Sphere::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point = uniform_scaling ? point : Vec3f(trafo * point);
    if ((center - transformed_point).squaredNorm() < radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Circle cursor?
bool TriangleSelector::Circle::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f diff              = center - transformed_point;

    if ((diff - diff.dot(dir) * dir).squaredNorm() < radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Capsule3D cursor?
bool TriangleSelector::Capsule3D::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point  = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f first_center_diff  = this->first_center - transformed_point;
    const Vec3f second_center_diff = this->second_center - transformed_point;
    if (first_center_diff.squaredNorm() < this->radius_sqr || second_center_diff.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    // First, check if the point pt is laying between planes defined by first_center and second_center.
    // Then check if it is inside the cylinder between first_center and second_center.
    const Vec3f centers_diff = this->second_center - this->first_center;
    if (first_center_diff.dot(centers_diff) <= 0.f && second_center_diff.dot(centers_diff) >= 0.f && (first_center_diff.cross(centers_diff).norm() / centers_diff.norm()) <= this->radius)
        return is_mesh_point_not_clipped(point, clipping_plane);

    return false;
}

// Is a point (in mesh coords) inside a Capsule2D cursor?
bool TriangleSelector::Capsule2D::is_mesh_point_inside(const Vec3f &point) const
{
    const Vec3f transformed_point           = uniform_scaling ? point : Vec3f(trafo * point);
    const Vec3f first_center_diff           = this->first_center - transformed_point;
    const Vec3f first_center_diff_projected = first_center_diff - first_center_diff.dot(this->dir) * this->dir;
    if (first_center_diff_projected.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    const Vec3f second_center_diff           = this->second_center - transformed_point;
    const Vec3f second_center_diff_projected = second_center_diff - second_center_diff.dot(this->dir) * this->dir;
    if (second_center_diff_projected.squaredNorm() < this->radius_sqr)
        return is_mesh_point_not_clipped(point, clipping_plane);

    const Vec3f centers_diff           = this->second_center - this->first_center;
    const Vec3f centers_diff_projected = centers_diff - centers_diff.dot(this->dir) * this->dir;

    // First, check if the point is laying between first_center and second_center.
    if (first_center_diff_projected.dot(centers_diff_projected) <= 0.f && second_center_diff_projected.dot(centers_diff_projected) >= 0.f) {
        // Vector in the direction of line |AD| of the rectangle that intersects the circle with the center in first_center.
        const Vec3f rectangle_da_dir              = centers_diff.cross(this->dir);
        // Vector pointing from first_center to the point 'A' of the rectangle.
        const Vec3f first_center_rectangle_a_diff = rectangle_da_dir.normalized() * this->radius;
        const Vec3f rectangle_a                   = this->first_center - first_center_rectangle_a_diff;
        const Vec3f rectangle_d                   = this->first_center + first_center_rectangle_a_diff;
        // Now check if the point is laying inside the rectangle between circles with centers in first_center and second_center.
        if ((rectangle_a - transformed_point).dot(rectangle_da_dir) <= 0.f && (rectangle_d - transformed_point).dot(rectangle_da_dir) >= 0.f)
            return is_mesh_point_not_clipped(point, clipping_plane);
    }

    return false;
}

// p1, p2, p3 are in mesh coords!
static bool is_circle_pointer_inside_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_, const Vec3f &center, const Vec3f &dir, const bool uniform_scaling, const Transform3f &trafo) {
    const Vec3f& q1 = center + dir;
    const Vec3f& q2 = center - dir;

    auto signed_volume_sign = [](const Vec3f& a, const Vec3f& b,
                                 const Vec3f& c, const Vec3f& d) -> bool {
        return ((b-a).cross(c-a)).dot(d-a) > 0.;
    };

    // In case the object is non-uniformly scaled, do the check in world coords.
    const Vec3f& p1 = uniform_scaling ? p1_ : Vec3f(trafo * p1_);
    const Vec3f& p2 = uniform_scaling ? p2_ : Vec3f(trafo * p2_);
    const Vec3f& p3 = uniform_scaling ? p3_ : Vec3f(trafo * p3_);

    if (signed_volume_sign(q1,p1,p2,p3) == signed_volume_sign(q2,p1,p2,p3))
        return false;

    bool pos = signed_volume_sign(q1,q2,p1,p2);
    return signed_volume_sign(q1,q2,p2,p3) == pos && signed_volume_sign(q1,q2,p3,p1) == pos;
}

// p1, p2, p3 are in mesh coords!
bool TriangleSelector::SinglePointCursor::is_pointer_in_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_) const
{
    return is_circle_pointer_inside_triangle(p1_, p2_, p3_, center, dir, uniform_scaling, trafo);
}

// p1, p2, p3 are in mesh coords!
bool TriangleSelector::DoublePointCursor::is_pointer_in_triangle(const Vec3f &p1_, const Vec3f &p2_, const Vec3f &p3_) const
{
    return is_circle_pointer_inside_triangle(p1_, p2_, p3_, first_center, dir, uniform_scaling, trafo) ||
           is_circle_pointer_inside_triangle(p1_, p2_, p3_, second_center, dir, uniform_scaling, trafo);
}

bool line_plane_intersection(const Vec3f &line_a, const Vec3f &line_b, const Vec3f &plane_origin, const Vec3f &plane_normal, Vec3f &out_intersection)
{
    Vec3f line_dir      = line_b - line_a;
    float t_denominator = plane_normal.dot(line_dir);
    if (t_denominator == 0.f)
        return false;

    // Compute 'd' in plane equation by using some point (origin) on the plane
    float plane_d = plane_normal.dot(plane_origin);
    if (float t = (plane_d - plane_normal.dot(line_a)) / t_denominator; t >= 0.f && t <= 1.f) {
        out_intersection = line_a + t * line_dir;
        return true;
    }

    return false;
}

bool TriangleSelector::Capsule3D::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a = pts[side];
        const Vec3f &edge_b = pts[side < 2 ? side + 1 : 0];
        if (test_line_inside_capsule(edge_a, edge_b, this->first_center, this->second_center, this->radius))
            return true;
    }

    return false;
}

// Is edge inside cursor?
bool TriangleSelector::Capsule2D::is_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &vertices) const
{
    std::array<Vec3f, 3> pts;
    for (int i = 0; i < 3; ++i) {
        pts[i] = vertices[tr.verts_idxs[i]].v;
        if (!this->uniform_scaling)
            pts[i] = this->trafo * pts[i];
    }

    const Vec3f centers_diff                  = this->second_center - this->first_center;
    // Vector in the direction of line |AD| of the rectangle that intersects the circle with the center in first_center.
    const Vec3f rectangle_da_dir              = centers_diff.cross(this->dir);
    // Vector pointing from first_center to the point 'A' of the rectangle.
    const Vec3f first_center_rectangle_a_diff = rectangle_da_dir.normalized() * this->radius;
    const Vec3f rectangle_a                   = this->first_center - first_center_rectangle_a_diff;
    const Vec3f rectangle_d                   = this->first_center + first_center_rectangle_a_diff;

    auto edge_inside_rectangle = [&self = std::as_const(*this), &centers_diff](const Vec3f &edge_a, const Vec3f &edge_b, const Vec3f &plane_origin, const Vec3f &plane_normal) -> bool {
        Vec3f intersection(-1.f, -1.f, -1.f);
        if (line_plane_intersection(edge_a, edge_b, plane_origin, plane_normal, intersection)) {
            // Now check if the intersection point is inside the rectangle. That means it is between 'first_center' and 'second_center', resp. between 'A' and 'B'.
            if (self.first_center.dot(centers_diff) <= intersection.dot(centers_diff) && intersection.dot(centers_diff) <= self.second_center.dot(centers_diff))
                return true;
        }
        return false;
    };

    for (int side = 0; side < 3; ++side) {
        const Vec3f &edge_a     = pts[side];
        const Vec3f &edge_b     = pts[side < 2 ? side + 1 : 0];
        const Vec3f  edge_dir   = edge_b - edge_a;
        const Vec3f  edge_dir_n = edge_dir.normalized();

        float t1      = (this->first_center - edge_a).dot(edge_dir_n);
        float t2      = (this->second_center - edge_a).dot(edge_dir_n);
        Vec3f vector1 = edge_a + t1 * edge_dir_n - this->first_center;
        Vec3f vector2 = edge_a + t2 * edge_dir_n - this->second_center;

        // Vectors vector1 and vector2 are 3D vector from centers to the intersections. What we want to
        // measure is length of its projection onto plane perpendicular to dir.
        if (float dist = vector1.squaredNorm() - std::pow(vector1.dot(this->dir), 2.f); dist < this->radius_sqr && t1 >= 0.f && t1 <= edge_dir.norm())
            return true;

        if (float dist = vector2.squaredNorm() - std::pow(vector2.dot(this->dir), 2.f); dist < this->radius_sqr && t2 >= 0.f && t2 <= edge_dir.norm())
            return true;

        // Check if the edge is passing through the rectangle between first_center and second_center.
        if (edge_inside_rectangle(edge_a, edge_b, rectangle_a, (rectangle_d - rectangle_a)) || edge_inside_rectangle(edge_a, edge_b, rectangle_d, (rectangle_a - rectangle_d)))
            return true;
    }

    return false;
}

// ORCA: Helper to extract used states from serialized data
std::vector<EnforcerBlockerType> TriangleSelector::extract_used_facet_states(const TriangleSplittingData &data)
{
    std::vector<EnforcerBlockerType> out;
    for (size_t i = 0; i < data.used_states.size(); ++i) {
        if (data.used_states[i])
            out.push_back(static_cast<EnforcerBlockerType>(i));
    }
    return out;
}

static bool segments_intersect_proj(const Vec3f& p1, const Vec3f& p2, const Vec3f& p3, const Vec3f& p4, const std::pair<int, int>& proj)
{
    auto cross2d  = [](float ax, float ay, float bx, float by) -> float { return ax * by - ay * bx; };
    const auto [u_axis, v_axis] = proj;
    const auto u1 = p1(u_axis), v1 = p1(v_axis);
    const auto u2 = p2(u_axis), v2 = p2(v_axis);
    const auto u3 = p3(u_axis), v3 = p3(v_axis);
    const auto u4 = p4(u_axis), v4 = p4(v_axis);
    const float ru = u2 - u1, rv = v2 - v1;
    const float su = u4 - u3, sv = v4 - v3;
    const float denom = cross2d(ru, rv, su, sv);
    if (std::abs(denom) < 1e-10f)
        return false;
    const float dpu = u3 - u1, dpv = v3 - v1;
    const float t1 = cross2d(dpu, dpv, su, sv) / denom;
    const float t2 = cross2d(dpu, dpv, ru, rv) / denom;
    return 0.f <= t1 && t1 <= 1.0f && 0.f <= t2 && t2 <= 1.f;
};

class TriangleCursor : public TriangleSelector::SinglePointCursor
{
public:
    TriangleCursor() = delete;
    explicit TriangleCursor(const Vec3f& center_,
                            const Vec3f& source_,
                            float radius_world,
                            const Transform3d& trafo_,
                            const TriangleSelector::ClippingPlane& clipping_plane_,
                            const std::array<const Vec3f, 3>& vertices)
        : TriangleSelector::SinglePointCursor(center_, source_, radius_world, trafo_, clipping_plane_)
        , barycentric_(vertices[0], vertices[1], vertices[2])
    {}
    ~TriangleCursor() override = default;

    static std::unique_ptr<Cursor> build_cursor(const TriangleSelector& source_selector, const TriangleSelector::Triangle& tri)
    {
        const Vec3f& pv0 = source_selector.m_vertices[tri.verts_idxs[0]].v;
        const Vec3f& pv1 = source_selector.m_vertices[tri.verts_idxs[1]].v;
        const Vec3f& pv2 = source_selector.m_vertices[tri.verts_idxs[2]].v;

        // Calculate the centroid of the triangle
        const Vec3f center = (pv0 + pv1 + pv2) / 3.f;

        // Calculate the norm of the plane
        const Vec3f& norm = source_selector.m_face_normals[tri.source_triangle];

        // Calculate the min distance from the centroid to every edges
        const float radius = std::max(std::min(std::min(point_to_line_dist(center, pv0, pv1), point_to_line_dist(center, pv0, pv2)),
                                      point_to_line_dist(center, pv1, pv2)), 0.1f);

        return std::make_unique<TriangleCursor>(center, center + norm, radius, Transform3d::Identity(), TriangleSelector::ClippingPlane(),
                                                std::array<const Vec3f, 3>{pv0, pv1, pv2});
    }

    bool is_mesh_point_inside(const Vec3f& point) const override
    {
        return barycentric_.is_point_inside_triangle(point);
    }

    bool is_edge_inside_cursor(const TriangleSelector::Triangle& tr, const std::vector<TriangleSelector::Vertex>& vertices) const override
    {
        std::array<Vec3f, 3> pts_proj; // Projected point onto the plane
        std::array<float, 3> pts_dist; // Distance to the plane, positive means above, negative means below the plane
        for (int i = 0; i < 3; ++i) {
            Vec3f p = vertices[tr.verts_idxs[i]].v;
            if (!this->uniform_scaling)
                p = this->trafo * p;

            const Vec3f diff = p - center;
            pts_dist[i]      = diff.dot(dir);
            pts_proj[i]      = p - pts_dist[i] * dir;
        }

        for (int side = 0; side < 3; ++side) {
            const int idx_a = side;
            const int idx_b = side < 2 ? side + 1 : 0;

            // If both ends at the same side and farther than tolerance, skip
            const float dist_a = pts_dist[idx_a];
            const float dist_b = pts_dist[idx_b];
            if ((dist_a > tolerance && dist_b > tolerance) || (dist_a < -tolerance && dist_b < -tolerance)) {
                continue;
            }

            // Find the projected line segment which has distance to the plane within the tolerance
            Vec3f pt_a = pts_proj[idx_a];
            Vec3f pt_b = pts_proj[idx_b];
            if (std::abs(dist_a) > tolerance) {
                pt_a = (tolerance - dist_b) / (dist_a - dist_b) * (pts_proj[idx_a] - pts_proj[idx_b]) + pts_proj[idx_b];
            }
            if (std::abs(dist_b) > tolerance) {
                pt_b = (tolerance - dist_a) / (dist_b - dist_a) * (pts_proj[idx_b] - pts_proj[idx_a]) + pts_proj[idx_a];
            }

            // If any projected end is inside the triangle, then is in
            if (barycentric_.is_point_inside_triangle(pt_a) ||
                barycentric_.is_point_inside_triangle(pt_b)) {
                return true;
            }

            // Otherwise see if the segment (pt_a, pt_b) intersects the triangle
            {
                const Vec3f uvw_a = barycentric_.calc(pt_a);
                const Vec3f uvw_b = barycentric_.calc(pt_b);
                const Vec3f uvw_0 {1.f,0.f,0.f};
                const Vec3f uvw_1 {0.f,1.f,0.f};
                const Vec3f uvw_2 {0.f,0.f,1.f};
                constexpr std::pair<int, int> proj{0, 1};

                if (segments_intersect_proj(uvw_a, uvw_b, uvw_0, uvw_1, proj)||
                    segments_intersect_proj(uvw_a, uvw_b, uvw_0, uvw_2, proj)||
                    segments_intersect_proj(uvw_a, uvw_b, uvw_1, uvw_2, proj)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool is_facet_visible(int facet_idx, const std::vector<Vec3f>& face_normals) const override
    {
        const Vec3f& n = face_normals[facet_idx];

        return check_normal(n, this->dir);
    }

    static bool check_normal(const Vec3f& facet_norm, const Vec3f& camera_dir)
    {
        const float a = -(facet_norm.dot(camera_dir));
        return std::clamp(a, 0.f, 1.f) >= facet_angle_limit;
    }

    static constexpr float tolerance = 0.01f;

private:
    const Barycentric barycentric_;
    static const double facet_angle_limit;

    static float point_to_line_dist(const Vec3f& p, const Vec3f& a, const Vec3f& b)
    {
        const Eigen::ParametrizedLine<float, 3> line = Eigen::ParametrizedLine<float, 3>::Through(a, b);
        return line.distance(p);
    }
};
const double TriangleCursor::facet_angle_limit = cos(Geometry::deg2rad(5.0));


// Remap painting data from source mesh to target mesh using spatial mapping.
TriangleSelector::TriangleSplittingData TriangleSelector::remap_painting(
    const indexed_triangle_set& source_its,
    const TriangleSplittingData& source_painting,
    const indexed_triangle_set& target_its,
    const Transform3d& target_transform,
    const std::optional<std::reference_wrapper<const TriangleSplittingData>>& existing_painting)
{
    TriangleSelector::TriangleSplittingData result;
    if (source_painting.bitstream.empty())
        return result;

    // 1. Deserialize source painting
    TriangleMesh source_mesh(source_its);
    TriangleSelector source_selector(source_mesh);
    source_selector.deserialize(source_painting, false);

    // 2. Extract painted geometry
    std::vector<std::reference_wrapper<const Triangle>> painted_triangles;
    painted_triangles.reserve(source_selector.m_triangles.size());
    for (const Triangle& tr : source_selector.m_triangles) {
        if (tr.valid() && !tr.is_split() && tr.get_state() != EnforcerBlockerType::NONE) {
            painted_triangles.push_back(std::ref(tr));
        }
    }

    if (painted_triangles.empty())
        return result;

    // 3. Build AABB tree of target mesh so we could find nearest face quickly
    TriangleMesh target_mesh(target_its);
    target_mesh.transform(target_transform);
    AABBTreeIndirect::Tree3f target_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(target_mesh.its.vertices, target_mesh.its.indices);

    // Helper: check overlap between a paint triangle and a target triangle.
    // Uses 3D barycentric point-in-triangle tests and dominant-axis 2D projection
    // for edge-edge intersection to handle all triangle orientations correctly.
    auto check_overlap = [&](const Vec3f& pa, const Vec3f& pb, const Vec3f& pc,
                             const Vec3f& ta, const Vec3f& tb, const Vec3f& tc) -> bool {
        // Check if any target vertex is inside the paint triangle
        if (is_point_inside_triangle(ta, pa, pb, pc)) return true;
        if (is_point_inside_triangle(tb, pa, pb, pc)) return true;
        if (is_point_inside_triangle(tc, pa, pb, pc)) return true;

        // Check if any paint vertex is inside the target triangle
        if (is_point_inside_triangle(pa, ta, tb, tc)) return true;
        if (is_point_inside_triangle(pb, ta, tb, tc)) return true;
        if (is_point_inside_triangle(pc, ta, tb, tc)) return true;

        // Check edge-edge intersections using dominant-axis 2D projection.
        // Project onto the plane (XY, XZ, or YZ) where the triangles have the
        // most area, avoiding degenerate projections for vertical/angled surfaces.
        Vec3f n1 = (pb - pa).cross(pc - pa);
        Vec3f n2 = (tb - ta).cross(tc - ta);
        Vec3f n_abs = (n1.cwiseAbs() + n2.cwiseAbs());
        int axis = (n_abs.x() >= n_abs.y() && n_abs.x() >= n_abs.z()) ? 0
                 : (n_abs.y() >= n_abs.z()) ? 1 : 2;
        const int u_axis = (axis + 1) % 3;
        const int v_axis = (axis + 2) % 3;
        const std::pair<int, int> proj{u_axis, v_axis};

        if (segments_intersect_proj(pa, pb, ta, tb, proj)) return true;
        if (segments_intersect_proj(pa, pb, tb, tc, proj)) return true;
        if (segments_intersect_proj(pa, pb, tc, ta, proj)) return true;
        if (segments_intersect_proj(pb, pc, ta, tb, proj)) return true;
        if (segments_intersect_proj(pb, pc, tb, tc, proj)) return true;
        if (segments_intersect_proj(pb, pc, tc, ta, proj)) return true;
        if (segments_intersect_proj(pc, pa, ta, tb, proj)) return true;
        if (segments_intersect_proj(pc, pa, tb, tc, proj)) return true;
        if (segments_intersect_proj(pc, pa, tc, ta, proj)) return true;

        return false;
    };

    // 4. For each painted face, we find the nearest target face, and apply the TriangleCursor to paint it
    TriangleSelector target_selector(target_mesh);
    if (existing_painting) {
        // Restore existing painting first, if given
        target_selector.deserialize(existing_painting->get(), false);
    }
    for (auto tri_ref : painted_triangles) {
        const Triangle& tri = tri_ref.get();
        const Vec3f& pv0    = source_selector.m_vertices[tri.verts_idxs[0]].v;
        const Vec3f& pv1    = source_selector.m_vertices[tri.verts_idxs[1]].v;
        const Vec3f& pv2    = source_selector.m_vertices[tri.verts_idxs[2]].v;

        // Find ALL target faces whose bounding boxes overlap with the paint
        // triangle's bounding box, not just the nearest one.
        Eigen::AlignedBox3f pt_bbox;
        pt_bbox.extend(pv0);
        pt_bbox.extend(pv1);
        pt_bbox.extend(pv2);
        pt_bbox.min() -= Eigen::Vector3f::Constant(TriangleCursor::tolerance);
        pt_bbox.max() += Eigen::Vector3f::Constant(TriangleCursor::tolerance);

        AABBTreeIndirect::traverse(target_tree, AABBTreeIndirect::intersecting(pt_bbox), [&](const AABBTreeIndirect::Tree3f::Node& node) -> bool {
            size_t face_idx = node.idx;
            if (face_idx >= target_mesh.its.indices.size())
                return true;

            const Vec3f& norm_a = source_selector.m_face_normals[tri.source_triangle];
            const Vec3f& norm_b = target_selector.m_face_normals[face_idx];

            const Vec3i32& face = target_mesh.its.indices[face_idx];
            const Vec3f& ta     = target_mesh.its.vertices[face(0)];
            const Vec3f& tb     = target_mesh.its.vertices[face(1)];
            const Vec3f& tc     = target_mesh.its.vertices[face(2)];

            if (TriangleCursor::check_normal(norm_b, -norm_a) && check_overlap(pv0, pv1, pv2, ta, tb, tc)) {
                // Paint this face
                target_selector.select_patch(face_idx, TriangleCursor::build_cursor(source_selector, tri), tri.get_state(),
                                             Transform3d::Identity(), true, 0.f, true);
            }
            return true; // continue traversal
        });
    }

    return target_selector.serialize();
}

// 2D helpers for the planar paint-transfer test below. Both triangles are given
// in the same 2D coordinate system (orthogonally projected into the face plane).
static bool point_in_tri2d(const Vec2f& p, const Vec2f& t0, const Vec2f& t1, const Vec2f& t2)
{
    const auto cross2d = [](const Vec2f& o, const Vec2f& u, const Vec2f& v) {
        return (u - o).x() * (v - o).y() - (u - o).y() * (v - o).x();
    };
    const float d1 = cross2d(p, t0, t1);
    const float d2 = cross2d(p, t1, t2);
    const float d3 = cross2d(p, t2, t0);
    const bool negative = d1 < 0.f || d2 < 0.f || d3 < 0.f;
    const bool positive = d1 > 0.f || d2 > 0.f || d3 > 0.f;
    return ! (negative && positive); // points exactly on an edge count as inside
}

static bool seg_intersect2d(const Vec2f& p1, const Vec2f& p2, const Vec2f& q1, const Vec2f& q2)
{
    const auto cross2d = [](const Vec2f& o, const Vec2f& u, const Vec2f& v) {
        return (u - o).x() * (v - o).y() - (u - o).y() * (v - o).x();
    };
    const float d1 = cross2d(q1, q2, p1);
    const float d2 = cross2d(q1, q2, p2);
    const float d3 = cross2d(p1, p2, q1);
    const float d4 = cross2d(p1, p2, q2);
    return ((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) &&
           ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f));
}

// Exact 2D triangle-triangle overlap: an edge pair crossing, or one triangle's
// vertex inside the other. Collinear touching is covered by the vertex tests.
static bool tri_overlap_2d(const Vec2f& a0, const Vec2f& a1, const Vec2f& a2,
                           const Vec2f& b0, const Vec2f& b1, const Vec2f& b2)
{
    if (seg_intersect2d(a0, a1, b0, b1) || seg_intersect2d(a0, a1, b1, b2) || seg_intersect2d(a0, a1, b2, b0) ||
        seg_intersect2d(a1, a2, b0, b1) || seg_intersect2d(a1, a2, b1, b2) || seg_intersect2d(a1, a2, b2, b0) ||
        seg_intersect2d(a2, a0, b0, b1) || seg_intersect2d(a2, a0, b1, b2) || seg_intersect2d(a2, a0, b2, b0))
        return true;
    if (point_in_tri2d(a0, b0, b1, b2) || point_in_tri2d(a1, b0, b1, b2) || point_in_tri2d(a2, b0, b1, b2))
        return true;
    if (point_in_tri2d(b0, a0, a1, a2) || point_in_tri2d(b1, a0, a1, a2) || point_in_tri2d(b2, a0, a1, a2))
        return true;
    return false;
}

// Transfer painting from the source mesh onto a simplified target mesh: the
// painting state under sample points of every target face is queried from the
// deserialized source painting (closest leaf on the source surface), and target
// faces are subdivided adaptively while trusted samples disagree or differently
// painted geometry lies within the face's own reach - down to the LOCAL scale of
// the painting around them. Colored regions and their boundaries are therefore
// re-established as one continuous assignment over the whole target surface.
// All lengths are in mesh units of the volume's own coordinate space (the space
// the painting was made in), not world millimeters under volume scaling.
// Unlike remap_painting() this does not require the two surfaces to geometrically
// intersect, which a simplified surface cannot guarantee. See the declaration in
// TriangleSelector.hpp for the rationale and limits.
TriangleSelector::TriangleSplittingData TriangleSelector::remap_painting_by_projection(
    const indexed_triangle_set& source_its,
    const TriangleSplittingData& source_painting,
    const indexed_triangle_set& target_its,
    const Transform3d& target_transform,
    const std::optional<std::reference_wrapper<const TriangleSplittingData>>& existing_painting)
{
    TriangleSplittingData result;
    if (source_painting.bitstream.empty())
        return result;

    // 1. Deserialize the source painting; its subdivided leaves are the queryable
    // truth. Resolution policy is LOCAL: every query reports the mean edge length
    // of the leaf it hit, and a target face only subdivides down to the scale of
    // the painting that actually surrounds it - fine near fine strokes, cheap
    // where the painting itself is coarse. All lengths are in mesh units of the
    // volume's own coordinate space (the space the painting was made in), not
    // world mm under volume scaling.
    TriangleMesh source_mesh(source_its);
    TriangleSelector source_selector(source_mesh);
    source_selector.deserialize(source_painting, false);

    // Per-leaf data (index = filtered leaf index, see leaf_its below): mean edge
    // length, painting state, and the source triangle for normal lookups.
    std::vector<float>               leaf_scale;
    std::vector<EnforcerBlockerType> leaf_states;
    std::vector<int>                 leaf_source;
    // Painting-wide mean leaf edge over PAINTED leaves: only the fallback
    // resolution for faces whose queries were all untrusted.
    double leaf_edge_sum   = 0.0;
    size_t leaf_edge_count = 0;

    indexed_triangle_set leaf_its; // one face per source leaf, tiling the source surface
    leaf_its.vertices.reserve(source_selector.m_triangles.size() * 3);
    leaf_states.reserve(source_selector.m_triangles.size());
    leaf_source.reserve(source_selector.m_triangles.size());
    leaf_scale.reserve(source_selector.m_triangles.size());
    for (int i = 0; i < int(source_selector.m_triangles.size()); ++i) {
        const Triangle& tr = source_selector.m_triangles[i];
        if (!tr.valid() || tr.is_split())
            continue;
        const Vec3f& a = source_selector.m_vertices[tr.verts_idxs[0]].v;
        const Vec3f& b = source_selector.m_vertices[tr.verts_idxs[1]].v;
        const Vec3f& c = source_selector.m_vertices[tr.verts_idxs[2]].v;
        leaf_states.push_back(tr.get_state());
        leaf_source.push_back(tr.source_triangle);
        leaf_scale.push_back(((b - a).norm() + (c - b).norm() + (a - c).norm()) / 3.f);
        const unsigned base = unsigned(leaf_its.vertices.size());
        leaf_its.vertices.push_back(a);
        leaf_its.vertices.push_back(b);
        leaf_its.vertices.push_back(c);
        leaf_its.indices.emplace_back(base, base + 1, base + 2);
        if (tr.get_state() != EnforcerBlockerType::NONE) {
            leaf_edge_sum += double(leaf_scale.back()) * 3.0;
            leaf_edge_count += 3;
        }
    }
    if (leaf_states.empty())
        return result;
    const float fallback_resolution = std::max(0.05f, float(leaf_edge_sum / double(leaf_edge_count)));

    // 2. Target selector that receives the transferred painting, and the spatial
    // index over the source leaves for the state queries.
    TriangleMesh target_mesh(target_its);
    target_mesh.transform(target_transform);
    TriangleSelector target_selector(target_mesh);
    if (existing_painting)
        target_selector.deserialize(existing_painting->get(), false);
    AABBTreeIndirect::Tree3f leaf_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(leaf_its.vertices, leaf_its.indices);

    // 3. Query the painting state under a point of the target surface: the
    // closest source leaf and its state. The outcomes are kept strictly apart: a
    // hit on an unpainted leaf is a real NONE and takes part in the face
    // resolution, while an untrusted query (no hit, or a closest leaf on the far
    // side of a thin wall / oppositely oriented shell) is Unknown - unknowns
    // never vote and never turn into NONE.
    enum class QueryOutcome : unsigned char { Unpainted, Painted, Unknown };
    auto query_state = [&leaf_its, &leaf_tree, &leaf_states, &leaf_source, &leaf_scale, &source_selector](const Vec3f& point, const Vec3f& face_normal, QueryOutcome& outcome, float& hit_scale) -> EnforcerBlockerType {
        outcome = QueryOutcome::Unknown;
        hit_scale = 0.f;
        size_t hit_leaf = 0;
        Vec3f  hit_point = Vec3f::Zero();
        const float dist2 = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
            leaf_its.vertices, leaf_its.indices, leaf_tree, point, hit_leaf, hit_point);
        if (dist2 < 0.f)
            return EnforcerBlockerType::NONE;
        if (source_selector.m_face_normals[leaf_source[hit_leaf]].dot(face_normal) <= 0.f)
            return EnforcerBlockerType::NONE;
        outcome = leaf_states[hit_leaf] == EnforcerBlockerType::NONE ? QueryOutcome::Unpainted : QueryOutcome::Painted;
        hit_scale = leaf_scale[hit_leaf];
        return leaf_states[hit_leaf];
    };

    // Does any DIFFERENTLY painted source leaf overlap this face's region on the
    // surface? This is what makes "the samples agree" mean "the face is one
    // color": a stroke, an erased patch or a neighbor color that never touches a
    // sample point still rejects the verdict. For faces whose neighborhood is
    // planar (every candidate leaf vertex within plane_epsilon of the face
    // plane) the test is exact - both triangles are projected into the face
    // plane and compared in 2D. Otherwise the verdict is conservatively
    // rejected and subdivision resolves the face at the local painting scale.
    // Sidewalls and far-side leaves (normals outside the same hemisphere)
    // never trigger it.
    auto region_contains_other_color = [&leaf_tree, &leaf_states, &leaf_source, &source_selector](const Vec3f& a, const Vec3f& b, const Vec3f& c, const Vec3f& face_normal, EnforcerBlockerType state) -> bool {
        constexpr float plane_epsilon = 0.05f; // mesh units
        Eigen::AlignedBox3f bbox;
        bbox.extend(a);
        bbox.extend(b);
        bbox.extend(c);
        bbox.min() -= Eigen::Vector3f::Constant(plane_epsilon);
        bbox.max() += Eigen::Vector3f::Constant(plane_epsilon);

        // 2D frame of the face plane
        const Vec3f u_axis = face_normal.cross(Vec3f::UnitZ());
        const Vec3f uu = u_axis.norm() > 1e-6f ? u_axis.normalized() : face_normal.cross(Vec3f::UnitX()).normalized();
        const Vec3f vv = face_normal.cross(uu).normalized();
        auto to2d = [&a, &uu, &vv](const Vec3f& p) {
            const Vec3f d = p - a;
            return Vec2f(d.dot(uu), d.dot(vv));
        };
        const Vec2f f0 = Vec2f::Zero();
        const Vec2f f1 = to2d(b);
        const Vec2f f2 = to2d(c);

        bool found = false;
        AABBTreeIndirect::traverse(leaf_tree, AABBTreeIndirect::intersecting(bbox), [&](const AABBTreeIndirect::Tree3f::Node& node) -> bool {
            if (found)
                return false;
            const size_t leaf = node.idx;
            if (leaf >= leaf_states.size() || leaf_states[leaf] == state)
                return true; // same painting cannot invalidate uniformity
            const int src_tri = leaf_source[leaf];
            const Vec3f& leaf_normal = source_selector.m_face_normals[src_tri];
            if (leaf_normal.dot(face_normal) <= 0.f)
                return true; // sidewall / far side of a thin wall
            const Triangle& leaf_tri = source_selector.m_triangles[src_tri];
            const Vec3f& l0 = source_selector.m_vertices[leaf_tri.verts_idxs[0]].v;
            const Vec3f& l1 = source_selector.m_vertices[leaf_tri.verts_idxs[1]].v;
            const Vec3f& l2 = source_selector.m_vertices[leaf_tri.verts_idxs[2]].v;
            const float d0 = std::abs((l0 - a).dot(face_normal));
            const float d1 = std::abs((l1 - a).dot(face_normal));
            const float d2 = std::abs((l2 - a).dot(face_normal));
            if (d0 <= plane_epsilon && d1 <= plane_epsilon && d2 <= plane_epsilon) {
                // Planar neighborhood: exact overlap test in the face plane.
                if (tri_overlap_2d(f0, f1, f2, to2d(l0), to2d(l1), to2d(l2)))
                    found = true;
            } else {
                // Curved neighborhood: a differently painted leaf inside the
                // face's reach means the verdict cannot be trusted; subdivision
                // resolves the face at the local painting scale.
                found = true;
            }
            return ! found;
        });
        return found;
    };

    // Subdivide a leaf face 4-way without a paint cursor (split_triangle() cannot
    // be used here - it dereferences m_cursor). Mirrors split_triangle() mechanics:
    // full division + perform_split() + topology notification, so the selector's
    // caches and revision bookkeeping stay consistent.
    auto try_subdivide = [&target_selector](int facet_idx, const Vec3i32& neighbors) -> bool {
        Triangle& tr = target_selector.m_triangles[facet_idx];
        if (tr.is_split())
            return true;
        target_selector.PrepareForLeafIdentityChange();
        const int source_triangle = tr.source_triangle;
        const EnforcerBlockerType old_state = tr.get_state();
        tr.set_division(3, 0);
        target_selector.perform_split(facet_idx, neighbors, old_state);
        target_selector.NotifyTopologyMutation(source_triangle);
        return target_selector.m_triangles[facet_idx].is_split();
    };
    // 4. Resolve every target face as part of one continuous assignment over the
    // surface. Adaptive subdivision: a face is split while its trusted samples
    // disagree (a color boundary crosses it), none of them could be trusted yet
    // (deeper sample positions frequently resolve), or differently painted source
    // geometry lies within its reach - down to the LOCAL painting scale around it
    // (longest-edge test) or the safety limits, whichever stops first.
    constexpr int       max_depth    = 8;        // backstop; the edge test below stops first in practice
    constexpr long long max_splits   = 500000;   // hard resource bound, far above any real painting
    long long           splits_done  = 0;
    int                 unresolved_faces = 0;
    int                 depth_stop_faces = 0;
    int                 budget_blocked_faces = 0;

    std::function<void(int, const Vec3i32&, int)> paint_face = [&](int facet_idx, const Vec3i32& neighbors, int depth) {
        const Triangle tr = target_selector.m_triangles[facet_idx]; // copy: recursion invalidates references
        if (!tr.valid())
            return;

        const Vec3f a = target_selector.m_vertices[tr.verts_idxs[0]].v;
        const Vec3f b = target_selector.m_vertices[tr.verts_idxs[1]].v;
        const Vec3f c = target_selector.m_vertices[tr.verts_idxs[2]].v;
        const Vec3f face_normal = target_selector.m_face_normals[tr.source_triangle];
        const Vec3f centroid = (a + b + c) / 3.f;
        const float longest_edge = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});

        // Centroid + corners + edge midpoints. Deeper levels sample fresh
        // positions, so a boundary or stroke missed at one level is caught at the
        // next while the face is still larger than the local painting resolution.
        const Vec3f samples[7] = {
            centroid,
            a, b, c,
            (a + b) / 2.f, (b + c) / 2.f, (c + a) / 2.f
        };
        std::array<EnforcerBlockerType, 7> states;
        std::array<QueryOutcome, 7>        outcomes;
        std::array<float, 7>               scales;
        int known_cnt = 0;
        for (int i = 0; i < 7; ++i) {
            states[i] = query_state(samples[i], face_normal, outcomes[i], scales[i]);
            if (outcomes[i] != QueryOutcome::Unknown)
                ++known_cnt;
        }

        // Local resolution: the finest scale among the leaves the trusted samples
        // actually hit. Near a fine stroke the face subdivides down to that
        // stroke's own scale; where the painting itself is coarse, subdivision
        // stops early and the painting data stays small. Fallback: the painting-
        // wide mean when nothing on this face could be trusted.
        float local_limit = fallback_resolution;
        for (int i = 0; i < 7; ++i)
            if (outcomes[i] != QueryOutcome::Unknown && scales[i] >= 0.05f)
                local_limit = std::min(local_limit, scales[i]);

        // First trusted sample as the reference (the centroid may be unknown).
        int ref = -1;
        for (int i = 0; i < 7; ++i)
            if (outcomes[i] != QueryOutcome::Unknown) {
                ref = i;
                break;
            }

        // Uniform among the trusted samples? Two agreeing samples, or the trusted
        // centroid alone. The verdict is rejected when differently painted source
        // geometry (another color, an erased region, a stroke inside blank space)
        // lies within the face's own reach - sample luck must not hide it.
        bool uniform = ref >= 0 && (known_cnt >= 2 || outcomes[0] != QueryOutcome::Unknown);
        for (int i = ref + 1; uniform && i < 7; ++i)
            if (outcomes[i] != QueryOutcome::Unknown && states[i] != states[ref])
                uniform = false;
        if (uniform && region_contains_other_color(a, b, c, face_normal, states[ref]))
            uniform = false;
        if (uniform) {
            target_selector.CollapseSubtreeToState(facet_idx, states[ref]);
            return;
        }

        // Disagreement, or nothing trusted yet: descend.
        const bool may_subdivide = depth < max_depth && splits_done < max_splits && longest_edge > local_limit;
        if (longest_edge > local_limit) {
            if (depth >= max_depth)
                ++depth_stop_faces;
            if (splits_done >= max_splits)
                ++budget_blocked_faces;
        }

        if (target_selector.m_triangles[facet_idx].is_split()) {
            // Already subdivided (existing painting): descend into the current
            // children without spending budget.
            const Triangle split_tr = target_selector.m_triangles[facet_idx];
            const int num_of_children = split_tr.number_of_split_sides() + 1;
            for (int i = 0; i < num_of_children; ++i)
                paint_face(split_tr.children[i], target_selector.child_neighbors(split_tr, neighbors, i), depth);
        } else if (may_subdivide && try_subdivide(facet_idx, neighbors)) {
            ++splits_done;
            const Triangle split_tr = target_selector.m_triangles[facet_idx];
            const int num_of_children = split_tr.number_of_split_sides() + 1;
            for (int i = 0; i < num_of_children; ++i)
                paint_face(split_tr.children[i], target_selector.child_neighbors(split_tr, neighbors, i), depth + 1);
        } else if (ref >= 0) {
            // At the local resolution limit with mixed trusted samples: the
            // centroid sample decides when it is trusted, else the first trusted
            // one. A majority vote is deliberately not used - it would erase a
            // thin stroke that only clips a corner of the face.
            target_selector.CollapseSubtreeToState(facet_idx, outcomes[0] != QueryOutcome::Unknown ? states[0] : states[ref]);
        } else {
            // Nothing on this face could be trusted at any scale. Leave it
            // untouched rather than guessing a color, and count it for the log.
            ++unresolved_faces;
        }
    };

    for (int facet_idx = 0; facet_idx < int(target_mesh.its.indices.size()); ++facet_idx)
        paint_face(facet_idx, target_selector.m_neighbors[facet_idx], 0);

    size_t painting_leaves = 0;
    for (const Triangle& t2 : target_selector.m_triangles)
        painting_leaves += (t2.valid() && !t2.is_split()) ? 1 : 0;

    BOOST_LOG_TRIVIAL(info) << "remap_painting_by_projection: geometry_faces=" << target_mesh.its.indices.size()
                            << ", painting_leaves=" << painting_leaves
                            << ", splits=" << splits_done
                            << ", depth_stop_faces=" << depth_stop_faces
                            << ", budget_blocked_faces=" << budget_blocked_faces
                            << ", faces left unpainted (untrusted queries)=" << unresolved_faces;

    return target_selector.serialize();
}

} // namespace Slic3r
