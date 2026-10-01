#include "FDMHollowing.hpp"

#include "ClipperUtils.hpp"
#include "I18N.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMeshSlicer.hpp"
#include "format.hpp"
#include "SLA/Hollowing.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {

const PrintRegionConfig *hollowing_config(const PrintObject &object, const ModelVolume &volume)
{
    const PrintObjectRegions *regions = object.shared_regions();
    if (regions == nullptr)
        return nullptr;
    // A part spanning several layer ranges takes the settings of the first one.
    for (const PrintObjectRegions::LayerRangeRegions &range : regions->layer_ranges)
        for (const PrintObjectRegions::VolumeRegion &vr : range.volume_regions)
            if (vr.model_volume == &volume && vr.region != nullptr)
                return &vr.region->config();
    return nullptr;
}

double hollowing_depth(const std::vector<ExPolygons> &slices, const std::vector<float> &slice_zs)
{
    const size_t n = std::min(slices.size(), slice_zs.size());
    if (n == 0)
        return 0.;
    // Upper bound: half the height, and half the narrower side of the widest layer.
    double hi = 0.5 * double(slice_zs[n - 1] - slice_zs[0]);
    double xy = 0.;
    for (size_t i = 0; i < n; ++i)
        if (!slices[i].empty()) {
            const Point size = get_extents(slices[i]).size();
            xy = std::max(xy, 0.5 * unscaled(std::min(size.x(), size.y())));
        }
    hi = std::min(hi, xy);

    // Is there a point at least r from the surface? Erode every layer by r, then look for a stack
    // of eroded layers 2r tall that still overlaps.
    auto fits = [&](double r) {
        std::vector<ExPolygons> eroded(n);
        for (size_t i = 0; i < n; ++i)
            eroded[i] = offset_ex(slices[i], -scaled<float>(r));
        for (size_t i = 0; i < n; ++i) {
            if (eroded[i].empty() || slice_zs[i] - r < slice_zs[0] || slice_zs[i] + r > slice_zs[n - 1])
                continue;
            // Layer i is the middle of the stack: everything within r below and above it.
            ExPolygons acc = eroded[i];
            for (size_t j = i; j-- > 0 && !acc.empty() && slice_zs[j] >= slice_zs[i] - r;)
                acc = intersection_ex(acc, eroded[j]);
            for (size_t j = i + 1; j < n && !acc.empty() && slice_zs[j] <= slice_zs[i] + r; ++j)
                acc = intersection_ex(acc, eroded[j]);
            if (!acc.empty())
                return true;
        }
        return false;
    };

    double lo = 0.;
    for (int it = 0; it < 12 && hi - lo > 0.02; ++it) {
        const double mid = 0.5 * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

std::vector<std::string> hollow_volume_slices(const PrintObject &object, const std::vector<float> &slice_zs,
                                              std::vector<VolumeSlices> &volume_slices,
                                              const std::function<void()> &throw_if_canceled)
{
    std::vector<std::string> warnings;
    if (slice_zs.empty())
        return warnings;

    const ModelObject &mo = *object.model_object();
    const size_t       num_parts = std::count_if(mo.volumes.begin(), mo.volumes.end(),
                                                 [](const ModelVolume *v) { return v->is_model_part(); });
    auto who = [&mo, num_parts](const ModelVolume &v) {
        return num_parts > 1 ? Slic3r::format(_u8L("Part \"%1%\" of \"%2%\""), v.name, mo.name) :
                               Slic3r::format("\"%1%\"", mo.name);
    };
    auto round_down = [](double v) { return std::floor(v * 10.) / 10.; };

    for (VolumeSlices &vs : volume_slices) {
        auto it = std::find_if(mo.volumes.begin(), mo.volumes.end(), [&vs](const ModelVolume *v) { return v->id() == vs.volume_id; });
        if (it == mo.volumes.end() || !(*it)->is_model_part())
            continue;
        const ModelVolume       &volume = **it;
        const PrintRegionConfig *cfg    = hollowing_config(object, volume);
        if (cfg == nullptr || !cfg->hollow_interior.value)
            continue;
        const double thickness = cfg->hollow_shell_thickness.value;

        indexed_triangle_set its = volume.mesh().its;
        its_transform(its, object.trafo_centered() * volume.get_matrix());
        if (its.empty())
            continue;

        sla::HollowingConfig hc;
        hc.min_thickness    = thickness;
        hc.quality          = 0.5;
        // Fills in cavity details narrower than about this, so thin features stay solid.
        hc.closing_distance = HOLLOWING_CLOSING_DISTANCE;

        sla::JobController ctl;
        ctl.cancelfn = throw_if_canceled;
        sla::InteriorPtr interior = sla::generate_interior(TriangleMesh(its), hc, ctl);
        throw_if_canceled();
        indexed_triangle_set cavity = interior ? sla::get_mesh(*interior) : indexed_triangle_set();

        if (cavity.empty()) {
            // Tell the user, with a shell thickness that would have worked.
            const double depth  = hollowing_depth(vs.slices, slice_zs);
            const double max_ok = round_down(depth - HOLLOWING_CLOSING_DISTANCE - 0.1);
            BOOST_LOG_TRIVIAL(info) << "Hollowing: " << mo.name << " / " << volume.name << " is too thin for a "
                                    << thickness << " mm shell, depth " << depth << " mm";
            warnings.emplace_back(max_ok >= 0.5 ?
                Slic3r::format(_u8L("%1% was not hollowed: a %2% mm shell leaves no room for a cavity inside it. "
                                    "A shell of up to about %3% mm would."),
                               who(volume), thickness, max_ok) :
                Slic3r::format(_u8L("%1% was not hollowed: it is too thin to leave a cavity inside any shell "
                                    "(about %2% mm at its thickest, and hollowing needs more than 5 mm)."),
                               who(volume), round_down(2. * depth)));
            continue;
        }

        // The interior surface faces into the cavity; slice it as a solid.
        if (its_volume(cavity) < 0.f)
            sla::swap_normals(cavity);

        // A part made of several separate bodies: say so when some of them were too thin.
        {
            std::vector<indexed_triangle_set> bodies = its_split(its);
            if (bodies.size() > 1) {
                std::vector<BoundingBoxf3> cavities;
                for (const indexed_triangle_set &c : its_split(cavity))
                    cavities.emplace_back(bounding_box(c));
                size_t hollowed = 0;
                for (const indexed_triangle_set &body : bodies) {
                    const BoundingBoxf3 bb = bounding_box(body);
                    if (std::any_of(cavities.begin(), cavities.end(), [&bb](const BoundingBoxf3 &c) { return bb.contains(c.center()); }))
                        ++hollowed;
                }
                if (hollowed < bodies.size())
                    warnings.emplace_back(Slic3r::format(
                        _u8L("%1% was hollowed only partly: %2% of its %3% separate bodies are too thin for a %4% mm "
                             "shell and stay solid."),
                        who(volume), bodies.size() - hollowed, bodies.size(), thickness));
            }
        }

        std::vector<ExPolygons> cut = slice_mesh_ex(cavity, slice_zs, throw_if_canceled);
        for (size_t i = 0; i < vs.slices.size() && i < cut.size(); ++i)
            if (!cut[i].empty() && !vs.slices[i].empty())
                vs.slices[i] = diff_ex(vs.slices[i], cut[i]);
        throw_if_canceled();
    }
    return warnings;
}

} // namespace Slic3r
