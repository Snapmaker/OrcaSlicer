#include "SeamAutoPaint.hpp"

#include <algorithm>
#include <limits>
#include <memory>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/AABBTreeIndirect.hpp"

namespace Slic3r {
namespace SeamAutoPaint {

namespace {

struct VolumeData
{
    AABBTreeIndirect::Tree<3, float> tree;
    Transform3d                      inverse      = Transform3d::Identity();
    Transform3d                      no_translate = Transform3d::Identity();
};

// A planned seam, projected onto the model surface, with the parts it gets painted on.
struct Anchor
{
    bool  valid  = false;
    bool  linked = false;
    // The seam as planned (on the outer wall centre line) and its projection onto the closest part surface.
    Vec3f seam    = Vec3f::Zero();
    Vec3f surface = Vec3f::Zero();
    float flow_width = 0.f;
    float radius     = 0.f;
    // (volume index, facet of that volume's mesh closest to `surface`) for each part painted.
    std::vector<std::pair<size_t, int>> owners;
};

// Closest point of volume `v` to `point` (seam coordinates). Returns the facet and the point in seam coordinates.
bool closest_on_volume(const Volume &v, const VolumeData &data, const Vec3f &point, int &facet, Vec3f &hit_out, float &dist)
{
    if (data.tree.empty())
        return false;
    const Vec3f local = (data.inverse * point.cast<double>()).cast<float>();
    size_t      hit_idx = size_t(-1);
    Vec3f       hit     = Vec3f::Zero();
    const float sqr     = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(v.its->vertices, v.its->indices,
                                                                                    data.tree, local, hit_idx, hit);
    if (sqr < 0.f || hit_idx >= v.its->indices.size())
        return false;
    hit_out = (v.trafo * hit.cast<double>()).cast<float>();
    dist    = (hit_out - point).norm();
    facet   = int(hit_idx);
    return true;
}

Vec3f to_local(const VolumeData &data, const Vec3f &point) { return (data.inverse * point.cast<double>()).cast<float>(); }

} // namespace

Stats paint(const std::vector<std::vector<SeamPlacer::PlannedSeam>> &layers,
            std::vector<Volume>                                    &volumes,
            const Params                                           &params,
            const std::function<void()>                            &throw_if_canceled)
{
    auto check_cancel = [&throw_if_canceled]() {
        if (throw_if_canceled)
            throw_if_canceled();
    };

    Stats stats;
    std::vector<VolumeData> data(volumes.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, volumes.size()), [&volumes, &data](const tbb::blocked_range<size_t> &r) {
        for (size_t i = r.begin(); i < r.end(); ++i) {
            const Volume &v = volumes[i];
            if (v.its == nullptr || v.selector == nullptr || v.its->indices.empty())
                continue;
            data[i].tree         = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(v.its->vertices, v.its->indices);
            data[i].inverse      = v.trafo.inverse();
            data[i].no_translate = v.trafo;
            data[i].no_translate.translation() = Vec3d::Zero();
        }
    });
    check_cancel();

    // Project every seam onto the surface and find the parts it is painted on.
    std::vector<std::vector<Anchor>> anchors(layers.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, layers.size()),
                      [&layers, &volumes, &data, &anchors, &params](const tbb::blocked_range<size_t> &r) {
        for (size_t layer_idx = r.begin(); layer_idx < r.end(); ++layer_idx) {
            const std::vector<SeamPlacer::PlannedSeam> &seams = layers[layer_idx];
            std::vector<Anchor>                        &out   = anchors[layer_idx];
            out.resize(seams.size());
            for (size_t seam_idx = 0; seam_idx < seams.size(); ++seam_idx) {
                const SeamPlacer::PlannedSeam &seam = seams[seam_idx];
                if (seam.is_hole && !params.holes)
                    continue;
                Anchor &anchor    = out[seam_idx];
                anchor.seam       = seam.position;
                anchor.flow_width = seam.flow_width;
                anchor.radius     = strip_radius(params, seam.flow_width);

                float best_dist = std::numeric_limits<float>::max();
                for (size_t v = 0; v < volumes.size(); ++v) {
                    int   facet;
                    Vec3f hit;
                    float dist;
                    if (closest_on_volume(volumes[v], data[v], seam.position, facet, hit, dist) && dist < best_dist) {
                        best_dist      = dist;
                        anchor.surface = hit;
                    }
                }
                if (best_dist == std::numeric_limits<float>::max())
                    continue;
                for (size_t v = 0; v < volumes.size(); ++v) {
                    int   facet;
                    Vec3f hit;
                    float dist;
                    if (closest_on_volume(volumes[v], data[v], anchor.surface, facet, hit, dist) && dist <= 0.5f * anchor.radius)
                        anchor.owners.emplace_back(v, facet);
                }
                anchor.valid = !anchor.owners.empty();
            }
        }
    });
    check_cancel();

    auto stroke = [&volumes, &data, &stats](size_t v, int facet, const Vec3f &a, const Vec3f &b, float radius) {
        const VolumeData &vd      = data[v];
        const Vec3f       a_local = to_local(vd, a);
        const Vec3f       b_local = to_local(vd, b);
        // The camera position only matters to the 2D cursors; these are the 3D ones.
        const Vec3f       source  = a_local + Vec3f::UnitZ();
        std::unique_ptr<TriangleSelector::Cursor> cursor;
        if ((a - b).squaredNorm() < 1e-8f)
            cursor = std::make_unique<TriangleSelector::Sphere>(a_local, source, radius, volumes[v].trafo, TriangleSelector::ClippingPlane());
        else
            cursor = std::make_unique<TriangleSelector::Capsule3D>(a_local, b_local, source, radius, volumes[v].trafo,
                                                                   TriangleSelector::ClippingPlane());
        volumes[v].selector->select_patch(facet, std::move(cursor), EnforcerBlockerType::ENFORCER, vd.no_translate, true, 0.f);
        ++stats.strokes;
    };

    // Chain each seam to the nearest seam of the layer below, if the aligner could have made that step.
    for (size_t layer_idx = 1; layer_idx < anchors.size(); ++layer_idx) {
        std::vector<Anchor> &below = anchors[layer_idx - 1];
        for (Anchor &upper : anchors[layer_idx]) {
            if (!upper.valid)
                continue;
            Anchor *lower     = nullptr;
            float   best_dist = std::numeric_limits<float>::max();
            for (Anchor &candidate : below) {
                if (!candidate.valid)
                    continue;
                const float dist = (candidate.seam.head<2>() - upper.seam.head<2>()).norm();
                if (dist < best_dist) {
                    best_dist = dist;
                    lower     = &candidate;
                }
            }
            if (lower == nullptr ||
                best_dist > SeamPlacer::seam_align_tolerable_dist_factor * std::max(std::max(upper.flow_width, lower->flow_width), 0.2f))
                continue;
            const float radius = std::max(upper.radius, lower->radius);
            // Each part is painted once per link, starting from the end of the link that lies on it.
            std::vector<size_t> done;
            for (const auto &[v, facet] : upper.owners) {
                stroke(v, facet, lower->surface, upper.surface, radius);
                done.push_back(v);
            }
            for (const auto &[v, facet] : lower->owners)
                if (std::find(done.begin(), done.end(), v) == done.end())
                    stroke(v, facet, lower->surface, upper.surface, radius);
            upper.linked = lower->linked = true;
            ++stats.links;
        }
        check_cancel();
    }

    for (const std::vector<Anchor> &layer : anchors)
        for (const Anchor &anchor : layer) {
            if (!anchor.valid)
                continue;
            ++stats.seams;
            if (!anchor.linked)
                for (const auto &[v, facet] : anchor.owners)
                    stroke(v, facet, anchor.surface, anchor.surface, anchor.radius);
        }

    return stats;
}

} // namespace SeamAutoPaint
} // namespace Slic3r
