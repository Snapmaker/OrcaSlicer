#pragma once
#include "../Model.hpp"
#include <map>

namespace Slic3r {

// Per-material bounds use the transformed triangles that actually carry each material.
inline std::map<int, BoundingBoxf3> mixed_gradient_part_bounds(const ModelVolume& volume, const Transform3d& transform)
{
    std::map<int, BoundingBoxf3> bounds;
    const int                    base_material = std::max(1, volume.extruder_id());
    if (volume.mmu_segmentation_facets.empty()) {
        bounds.emplace(base_material, volume.mesh().transformed_bounding_box(transform));
    } else {
        std::vector<indexed_triangle_set> meshes;
        volume.mmu_segmentation_facets.get_facets(volume, meshes);
        for (size_t state = 0; state < meshes.size(); ++state) {
            const auto& mesh = meshes[state];
            if (mesh.indices.empty())
                continue;
            auto& box = bounds[state ? int(state) : base_material];
            for (const auto& face : mesh.indices)
                for (int i = 0; i < 3; ++i)
                    box.merge(Vec3d(transform * mesh.vertices[face[i]].cast<double>()));
        }
    }
    return bounds;
}

} // namespace Slic3r
