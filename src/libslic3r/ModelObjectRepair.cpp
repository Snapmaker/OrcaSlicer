#include "ModelObjectRepair.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "BoundingBox.hpp"
#include "Exception.hpp"
#include "I18N.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "format.hpp"

namespace Slic3r {

namespace {

// Orca: Determines if a mesh is degenerate or represents a non-3dimensional part by checking volume and bounding box dimensions.
bool is_not_3dimensional_part(const TriangleMesh &mesh)
{
    if (mesh.its.indices.empty())
        return true;

    indexed_triangle_set tmp = mesh.its;
    its_remove_degenerate_faces(tmp, true);
    if (tmp.indices.empty())
        return true;

    const BoundingBoxf3 bbox = mesh.bounding_box();
    const Vec3d size = bbox.size();
    const double min_dim = std::min(size.x(), std::min(size.y(), size.z()));
    const double max_dim = std::max(size.x(), std::max(size.y(), size.z()));
    if (min_dim <= EPSILON)
        return true;

    const double volume = std::abs(its_volume(mesh.its));
    const double bbox_volume = size.x() * size.y() * size.z();
    if (volume <= EPSILON)
        return true;

    const double min_relative_thickness = 1e-6;
    const double min_volume_ratio = 1e-6;
    if (min_dim / max_dim <= min_relative_thickness)
        return true;
    if (bbox_volume > 0.0 && volume / bbox_volume <= min_volume_ratio)
        return true;

    return false;
}

} // namespace

void repair_model_object(ModelObject                                               &model_object,
                         int                                                        volume_idx,
                         const std::function<void(const char *message, int percent)> &on_progress,
                         const std::function<void()>                               &throw_on_cancel)
{
    size_t ivolume = 0;
    auto progress = [&](const char *msg, unsigned prcnt) {
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        on_progress(msg, int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total))));
    };

    size_t start_volume = volume_idx == -1 ? 0 : size_t(volume_idx);
    size_t end_volume   = volume_idx == -1 ? std::numeric_limits<size_t>::max() : size_t(volume_idx);

    for (ivolume = start_volume; ivolume < model_object.volumes.size(); ++ivolume) {
        if (volume_idx != -1 && ivolume > end_volume)
            break;
        throw_on_cancel();

        progress(L("Repairing model object"), 10);

        ModelVolume *volume = model_object.volumes[ivolume];

        // Orca: Split splittable volumes into parts for individual processing.
        size_t parts_count = 1;
        if (volume->is_splittable()) {
            parts_count = volume->split(1);
            if (parts_count > 1) {
                const std::string msg = Slic3r::format(L("Split into %1% parts"), parts_count);
                progress(msg.c_str(), 10);
            }
        }

        size_t part_end = std::min(ivolume + parts_count - 1, model_object.volumes.size() - 1);
        if (volume_idx != -1)
            end_volume = part_end;

        size_t removed_parts = 0;
        for (size_t idx = part_end + 1; idx > ivolume; --idx) {
            const size_t part_idx = idx - 1;
            const ModelVolume *part_volume = model_object.volumes[part_idx];
            if (!is_not_3dimensional_part(part_volume->mesh()))
                continue;

            model_object.delete_volume(part_idx);
            ++removed_parts;
            if (part_end > 0)
                --part_end;
            else
                part_end = 0;
            if (volume_idx != -1)
                end_volume = part_end;
        }

        if (removed_parts >= parts_count) {
            ivolume = part_end;
            progress(L("Repair finished"), 100);
            continue;
        }

        for (size_t part_idx = ivolume; part_idx <= part_end && part_idx < model_object.volumes.size(); ++part_idx) {
            ModelVolume *part_volume = model_object.volumes[part_idx];
            TriangleMesh mesh = part_volume->mesh();
            if (its_num_open_edges(mesh.its) != 0) {
                std::string error;
                if (!MeshBoolean::cgal::repair(mesh, nullptr, &error))
                    throw Slic3r::RuntimeError(error.empty() ? L("Repair failed") : error.c_str());

                part_volume->set_mesh(std::move(mesh));
                part_volume->calculate_convex_hull();
                part_volume->invalidate_convex_hull_2d();
                part_volume->set_new_unique_id();
            }
        }

        ivolume = part_end;

        progress(L("Repair finished"), 100);
    }

    model_object.invalidate_bounding_box();

    if (ivolume > 0)
        --ivolume;
    progress(L("Repair finished"), 100);
}

} // namespace Slic3r
