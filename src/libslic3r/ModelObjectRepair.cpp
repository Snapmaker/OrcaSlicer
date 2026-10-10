#include "ModelObjectRepair.hpp"

#include <algorithm>
#include <cmath>
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

// Orca: Whether the volume, or any of the shells it would be split into, encloses a volume.
bool has_3dimensional_part(const ModelVolume &volume)
{
    if (!volume.is_splittable())
        return !is_not_3dimensional_part(volume.mesh());
    for (indexed_triangle_set &shell : its_split(volume.mesh().its))
        if (!is_not_3dimensional_part(TriangleMesh(std::move(shell))))
            return true;
    return false;
}

} // namespace

void repair_model_object(ModelObject                                               &model_object,
                         int                                                        volume_idx,
                         const std::function<void(const char *message, int percent)> &on_progress,
                         const std::function<void()>                               &throw_on_cancel)
{
    if (volume_idx < -1 || volume_idx >= int(model_object.volumes.size()))
        throw Slic3r::InvalidArgument("repair_model_object: no such volume");
    // Orca: Checked shell by shell before anything changes: ModelVolume::split() alone drops parts with a degenerate
    // convex hull, and the shells of a volume may enclose a signed volume together while each of them is flat.
    if (std::none_of(model_object.volumes.begin(), model_object.volumes.end(), [&throw_on_cancel](const ModelVolume *v) {
            throw_on_cancel();
            return has_3dimensional_part(*v);
        }))
        throw Slic3r::RuntimeError(_u8L("The object has no part that encloses a volume"));

    size_t ivolume  = volume_idx == -1 ? 0 : size_t(volume_idx);
    auto   progress = [&](const char *msg, unsigned prcnt) {
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        on_progress(msg, std::min(100, int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total)))));
    };

    while (ivolume < model_object.volumes.size()) {
        throw_on_cancel();

        progress(L("Repairing model object"), 10);

        ModelVolume *volume = model_object.volumes[ivolume];

        // Orca: Split splittable volumes into parts for individual processing.
        size_t parts_count = 1;
        if (volume->is_splittable()) {
            parts_count = volume->split(1);
            if (parts_count > 1) {
                // Orca: Translated here, the receiver cannot look the formatted message up in the catalog.
                const std::string msg = Slic3r::format(_u8L("Split into %1% parts"), parts_count);
                progress(msg.c_str(), 10);
            }
        }

        // Orca: The parts of this volume are [ivolume, part_end).
        size_t part_end = std::min(ivolume + parts_count, model_object.volumes.size());

        for (size_t part_idx = part_end; part_idx-- > ivolume;) {
            if (!is_not_3dimensional_part(model_object.volumes[part_idx]->mesh()))
                continue;
            if (model_object.volumes.size() == 1)
                throw Slic3r::RuntimeError(_u8L("The object has no part that encloses a volume"));
            model_object.delete_volume(part_idx);
            --part_end;
        }

        for (size_t part_idx = ivolume; part_idx < part_end; ++part_idx) {
            throw_on_cancel();
            ModelVolume *part_volume = model_object.volumes[part_idx];
            TriangleMesh mesh = part_volume->mesh();
            if (its_num_open_edges(mesh.its) != 0) {
                std::string error;
                if (!MeshBoolean::cgal::repair(mesh, nullptr, &error))
                    throw Slic3r::RuntimeError(error.empty() ? _u8L("Repair failed") : error);

                part_volume->set_mesh(std::move(mesh));
                part_volume->calculate_convex_hull();
                part_volume->invalidate_convex_hull_2d();
                part_volume->set_new_unique_id();
            }
        }

        progress(L("Repair finished"), 100);

        // Orca: The next volume follows the parts left of this one, even when all of them were dropped.
        ivolume = part_end;
        if (volume_idx != -1)
            break;
    }

    model_object.invalidate_bounding_box();
    on_progress(L("Repair finished"), 100);
}

} // namespace Slic3r
