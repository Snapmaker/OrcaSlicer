#include "FDMHollowing.hpp"

#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMeshSlicer.hpp"
#include "SLA/Hollowing.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

std::vector<ExPolygons> hollow_cavity_slices(const PrintObject &object, const std::vector<float> &slice_zs,
                                             const std::function<void()> &throw_if_canceled)
{
    const PrintObjectConfig &cfg = object.config();
    if (!cfg.hollow_interior.value || slice_zs.empty())
        return {};

    // The printable parts, in the frame the layers are sliced in.
    indexed_triangle_set its;
    const Transform3d trafo = object.trafo_centered();
    for (const ModelVolume *volume : object.model_object()->volumes)
        if (volume->is_model_part()) {
            indexed_triangle_set vits = volume->mesh().its;
            its_transform(vits, trafo * volume->get_matrix());
            its_merge(its, vits);
        }
    if (its.empty())
        return {};

    sla::HollowingConfig hc;
    hc.min_thickness    = cfg.hollow_shell_thickness.value;
    hc.quality          = 0.5;
    // Fills in cavity details narrower than about this, so thin features stay solid.
    hc.closing_distance = 2.;

    sla::JobController ctl;
    ctl.cancelfn = throw_if_canceled;
    sla::InteriorPtr interior = sla::generate_interior(TriangleMesh(std::move(its)), hc, ctl);
    if (!interior)
        return {};
    indexed_triangle_set cavity = sla::get_mesh(*interior);
    if (cavity.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Hollowing: " << object.model_object()->name << " is too thin to leave a cavity";
        return {};
    }
    // The interior surface faces into the cavity; slice it as a solid.
    if (its_volume(cavity) < 0.f)
        sla::swap_normals(cavity);
    throw_if_canceled();

    return slice_mesh_ex(cavity, slice_zs, throw_if_canceled);
}

} // namespace Slic3r
