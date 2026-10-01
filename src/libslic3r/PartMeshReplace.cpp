#include "PartMeshReplace.hpp"

#include "Model.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

bool replace_part_mesh(ModelVolume &volume, TriangleMesh &&mesh)
{
    ModelObject *object  = volume.get_object();
    const bool   sinking = object != nullptr && object->min_z() < SINKING_Z_THRESHOLD;

    const bool had_paint = !volume.supported_facets.empty() || !volume.seam_facets.empty() ||
                           !volume.mmu_segmentation_facets.empty() || !volume.fuzzy_skin_facets.empty();
    volume.supported_facets.reset();
    volume.seam_facets.reset();
    volume.mmu_segmentation_facets.reset();
    volume.fuzzy_skin_facets.reset();

    volume.set_mesh(std::move(mesh));
    volume.calculate_convex_hull();
    volume.invalidate_convex_hull_2d();
    volume.set_new_unique_id();
    if (object != nullptr) {
        object->invalidate_bounding_box();
        if (!sinking)
            object->ensure_on_bed();
    }
    return had_paint;
}

} // namespace Slic3r
