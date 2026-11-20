#include "LocalZPlan.hpp"
#include <stdexcept>

namespace Slic3r {

LocalZPlan::LocalZPlan(std::vector<LocalZInterval> intervals, std::vector<SubLayerPlan> passes)
    : m_intervals(std::move(intervals)), m_passes(std::move(passes))
{
    for (size_t i = 0; i < m_intervals.size(); ++i) {
        const auto& interval = m_intervals[i];
        if ((i > 0 && m_intervals[i - 1].layer_id >= interval.layer_id) || interval.first_sublayer_idx > m_passes.size() ||
            interval.sublayer_count > m_passes.size() - interval.first_sublayer_idx)
            throw std::invalid_argument("Invalid Local-Z interval index");
        for (size_t j = 0; j < interval.sublayer_count; ++j)
            if (m_passes[interval.first_sublayer_idx + j].layer_id != interval.layer_id)
                throw std::invalid_argument("Local-Z pass belongs to a different layer");
    }
}

const LocalZInterval* LocalZPlan::interval_for_layer(size_t layer_id) const
{
    const auto it = std::lower_bound(m_intervals.begin(), m_intervals.end(), layer_id,
                                     [](const LocalZInterval& interval, size_t id) { return interval.layer_id < id; });
    return it != m_intervals.end() && it->layer_id == layer_id ? &*it : nullptr;
}

} // namespace Slic3r
