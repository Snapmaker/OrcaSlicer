#include "ModelObjectRepair.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

#include "BoundingBox.hpp"
#include "Exception.hpp"
#include "I18N.hpp"
#include "MeshBoolean.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "TryCatchSignal.hpp"
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

// Orca: Whether the mesh has the defects repair() fixes: open edges or self-intersections. A crash of the CGAL check,
// where try_catch_signal() traps it, counts as a defect: repair() is guarded the same way and reports the failure.
bool is_broken(const TriangleMesh &mesh)
{
    if (its_num_open_edges(mesh.its) != 0)
        return true;
    bool self_intersecting = true;
    try_catch_signal({SIGSEGV, SIGFPE}, [&self_intersecting, &mesh] {
        self_intersecting = MeshBoolean::cgal::does_self_intersect(mesh);
    }, [] {});
    return self_intersecting;
}

// Orca: Repairs the broken volume at ivolume: splits it into its shells, drops the parts that enclose no volume and
// repairs the broken parts. Returns the end of its parts, which start at ivolume.
size_t repair_volume(ModelObject                                          &model_object,
                     size_t                                                ivolume,
                     ModelObjectRepairReport                              &report,
                     const std::function<void(const char *, unsigned)>    &progress,
                     const std::function<void()>                          &throw_on_cancel)
{
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
        if (!is_broken(part_volume->mesh()))
            continue;
        TriangleMesh mesh              = part_volume->mesh();
        std::string  error;
        bool         self_intersecting = false;
        if (!MeshBoolean::cgal::repair(mesh, nullptr, &error, &self_intersecting))
            throw Slic3r::RuntimeError(error.empty() ? _u8L("Repair failed") : error);
        report.self_intersections_remain |= self_intersecting;

        part_volume->set_mesh(std::move(mesh));
        part_volume->calculate_convex_hull();
        part_volume->invalidate_convex_hull_2d();
        part_volume->set_new_unique_id();
    }
    return part_end;
}

// Orca: Whether volume is the one of object that the repair of its copy left untouched: they share the mesh, and
// dropping volumes did not move the transformation of the copy into the instances.
bool is_untouched_copy(const ModelVolume &volume, const ModelVolume &copy)
{
    return volume.get_mesh_shared_ptr() == copy.get_mesh_shared_ptr() && volume.get_matrix().matrix() == copy.get_matrix().matrix();
}

} // namespace

ModelObjectRepairReport repair_model_object(ModelObject                                               &model_object,
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

    ModelObjectRepairReport report;
    size_t                  ivolume  = volume_idx == -1 ? 0 : size_t(volume_idx);
    auto                    progress = [&](const char *msg, unsigned prcnt) {
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        on_progress(msg, std::min(100, int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total)))));
    };

    while (ivolume < model_object.volumes.size()) {
        throw_on_cancel();

        progress(L("Repairing model object"), 10);

        // Orca: A volume without defects stays as it is, even when it is made of several shells: splitting it would
        // drop its painted facets. The next volume follows the parts left of this one, even when all of them were dropped.
        const size_t part_end = is_broken(model_object.volumes[ivolume]->mesh()) ?
                                    repair_volume(model_object, ivolume, report, progress, throw_on_cancel) :
                                    ivolume + 1;

        progress(L("Repair finished"), 100);

        ivolume = part_end;
        if (volume_idx != -1)
            break;
    }

    model_object.invalidate_bounding_box();
    on_progress(L("Repair finished"), 100);
    return report;
}

void apply_repaired_volumes(ModelObject &object, const ModelObject &repaired)
{
    assert(object.instances.size() == repaired.instances.size());
    // Orca: The repair replaces, inserts and deletes volumes, the untouched ones keep their order.
    const size_t      old_count = object.volumes.size();
    std::vector<bool> kept(old_count, false);
    ModelVolumePtrs   volumes;
    volumes.reserve(repaired.volumes.size());
    size_t next = 0;
    for (const ModelVolume *repaired_volume : repaired.volumes) {
        size_t i = next;
        while (i < old_count && !is_untouched_copy(*object.volumes[i], *repaired_volume))
            ++i;
        if (i < old_count) {
            kept[i] = true;
            next    = i + 1;
            volumes.push_back(object.volumes[i]);
        } else
            // Appended to object.volumes behind the old volumes, which the loop above does not look past.
            volumes.push_back(object.add_volume(*repaired_volume));
    }
    for (size_t i = 0; i < old_count; ++i)
        if (!kept[i])
            delete object.volumes[i];
    object.volumes = std::move(volumes);

    for (size_t i = 0; i < object.instances.size(); ++i)
        object.instances[i]->set_transformation(repaired.instances[i]->get_transformation());
    object.invalidate_bounding_box();
}

} // namespace Slic3r
