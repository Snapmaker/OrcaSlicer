#ifndef slic3r_LocalZPlan_hpp_
#define slic3r_LocalZPlan_hpp_

#include "../ExPolygon.hpp"
#include <algorithm>
#include <vector>

namespace Slic3r {

struct SubLayerPlan
{
    // Layer::id(), including the raft offset; never an index into object.layers().
    size_t                  layer_id{0};
    size_t                  pass_index{0};
    bool                    split_interval{false};
    double                  z_lo{0.0};
    double                  z_hi{0.0};
    double                  print_z{0.0};
    double                  flow_height{0.0};
    size_t                  dependency_group{0};
    size_t                  dependency_order{0};
    std::vector<ExPolygons> painted_masks_by_extruder;
    std::vector<ExPolygons> fixed_painted_masks_by_extruder;
};

struct LocalZInterval
{
    // Layer::id(), including the raft offset.
    size_t     layer_id{0};
    double     z_lo{0.0};
    double     z_hi{0.0};
    double     base_height{0.0};
    double     sublayer_height{0.0};
    bool       has_mixed_paint{false};
    bool       independent_layer_height{false};
    ExPolygons managed_masks;
    size_t     first_sublayer_idx{0};
    size_t     sublayer_count{0};

    // Independent cadence owns its masks even while waiting for the next pass.
    // All consumers must use this contract, rather than infer ownership from
    // the number of passes emitted within this nominal layer.
    bool is_managed(const std::vector<SubLayerPlan>& plans) const
    {
        if (!has_mixed_paint || first_sublayer_idx > plans.size() || sublayer_count > plans.size() - first_sublayer_idx)
            return false;
        if (independent_layer_height && !managed_masks.empty())
            return true;
        return sublayer_count > 1 && std::any_of(plans.begin() + first_sublayer_idx, plans.begin() + first_sublayer_idx + sublayer_count,
                                                 [](const SubLayerPlan& plan) { return plan.split_interval; });
    }
};

// Own the interval index and its pass storage together. Publication validates
// lookup ordering and references once, before any consumer can borrow a plan.
class LocalZPlan
{
public:
    LocalZPlan() = default;
    LocalZPlan(std::vector<LocalZInterval> intervals, std::vector<SubLayerPlan> passes);
    const std::vector<LocalZInterval>& intervals() const { return m_intervals; }
    const std::vector<SubLayerPlan>&   passes() const { return m_passes; }
    const LocalZInterval*              interval_for_layer(size_t layer_id) const;

private:
    std::vector<LocalZInterval> m_intervals;
    std::vector<SubLayerPlan>   m_passes;
};

} // namespace Slic3r

#endif
