#include "LocalZMaskPreparation.hpp"
#include "../ClipperUtils.hpp"

namespace Slic3r {

ExPolygons local_z_compensate_masks(const ExPolygons& masks, float delta_scaled, bool fallback_to_source)
{
    if (masks.empty() || std::abs(delta_scaled) <= EPSILON)
        return masks;
    ExPolygons compensated = offset_ex(masks, delta_scaled);
    if (compensated.size() > 1)
        compensated = union_ex(compensated);
    return compensated.empty() && fallback_to_source ? masks : compensated;
}

PreparedLocalZPassMasks prepare_local_z_pass_masks(const SubLayerPlan& plan)
{
    PreparedLocalZPassMasks result;
    result.by_extruder.resize(std::max(plan.painted_masks_by_extruder.size(), plan.fixed_painted_masks_by_extruder.size()));
    const float expansion = float(scale_(LOCAL_Z_PERIMETER_MASK_EXPAND_MM));
    ExPolygons  fixed_union;
    for (const auto& masks : plan.fixed_painted_masks_by_extruder)
        append(fixed_union, masks);
    if (fixed_union.size() > 1)
        fixed_union = union_ex(fixed_union);
    const ExPolygons guard = local_z_compensate_masks(fixed_union, expansion);
    const ExPolygons empty;
    for (size_t tool = 0; tool < result.by_extruder.size(); ++tool) {
        const auto& mixed = tool < plan.painted_masks_by_extruder.size() ? plan.painted_masks_by_extruder[tool] : empty;
        const auto& fixed = tool < plan.fixed_painted_masks_by_extruder.size() ? plan.fixed_painted_masks_by_extruder[tool] : empty;
        result.raw_polygon_count += mixed.size() + fixed.size();
        append(result.raw_union, mixed);
        append(result.raw_union, fixed);
        auto& compensated = result.by_extruder[tool];
        if (!mixed.empty()) {
            compensated = local_z_compensate_masks(mixed, expansion);
            if (!guard.empty())
                compensated = diff_ex(compensated, guard);
        }
        append(compensated, fixed);
        if (compensated.size() > 1)
            compensated = union_ex(compensated);
        result.compensated_polygon_count += compensated.size();
    }
    return result;
}

} // namespace Slic3r
