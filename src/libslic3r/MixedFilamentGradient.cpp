#include "MixedFilament.hpp"
#include "MixedFilamentColorPrediction.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

std::vector<unsigned int> mixed_gradient_components(const MixedFilament& entry, size_t num_physical)
{
    auto ids = MixedFilamentManager::decode_gradient_component_ids(entry.gradient_component_ids, num_physical);
    if (ids.size() < 2)
        ids = {entry.component_a, entry.component_b};
    if (entry.gradient_start < entry.gradient_end)
        std::reverse(ids.begin(), ids.end());
    return ids;
}

std::vector<float> mixed_gradient_stops(const MixedFilament& entry, size_t num_physical)
{
    const auto   ids   = mixed_gradient_components(entry, num_physical);
    const size_t count = ids.size() >= 2 ? ids.size() * 2 - 1 : 0;
    if (count == 0)
        return {};
    auto stops = entry.gradient_stop_positions;
    if (stops.size() != count || std::any_of(stops.begin(), stops.end(), [](float x) { return !std::isfinite(x); })) {
        stops.resize(count);
        for (size_t i = 0; i < count; ++i)
            stops[i] = float(i) / float(count - 1);
    }
    constexpr float gap = 0.0001f;
    stops.front()       = 0.f;
    stops.back()        = 1.f;
    for (size_t i = 1; i < count; ++i)
        stops[i] = std::max(std::clamp(stops[i], 0.f, 1.f), stops[i - 1] + gap);
    stops.back() = 1.f;
    for (size_t i = count - 1; i > 0; --i)
        stops[i - 1] = std::min(stops[i - 1], stops[i] - gap);
    stops.front() = 0.f;
    return stops;
}

std::vector<float> mixed_gradient_solid_half_widths(const MixedFilament& entry, size_t num_physical, double fallback)
{
    const auto         stops = mixed_gradient_stops(entry, num_physical);
    const size_t       count = mixed_gradient_components(entry, num_physical).size();
    std::vector<float> widths(count, 0.f);
    if (!std::isfinite(fallback))
        fallback = 0.03;
    for (size_t i = 1; i + 1 < count; ++i) {
        const double requested = i < entry.gradient_solid_widths.size() && std::isfinite(entry.gradient_solid_widths[i]) ?
                                     entry.gradient_solid_widths[i] :
                                     fallback;
        widths[i] = float(std::max(0.0, std::min({std::clamp(requested, 0.0, 1.0) * 0.5, double(stops[2 * i] - stops[2 * i - 1]),
                                                  double(stops[2 * i + 1] - stops[2 * i])})));
    }
    return widths;
}

MixedGradientSample sample_mixed_gradient(const MixedFilament& entry, size_t num_physical, double progress, double middle_window)
{
    const auto ids   = mixed_gradient_components(entry, num_physical);
    const auto stops = mixed_gradient_stops(entry, num_physical);
    if (ids.size() < 2 || stops.empty())
        return {};
    const double t = std::isfinite(progress) ? std::clamp(progress, 0.0, 1.0) : 0.0;
    if (t <= 0.0)
        return {ids.front(), ids.front(), 0};
    if (t >= 1.0)
        return {ids.back(), ids.back(), 0};
    const auto half_widths = mixed_gradient_solid_half_widths(entry, num_physical, middle_window);
    auto       half_width  = [&](size_t color) { return double(half_widths[color]); };
    for (size_t color = 1; color + 1 < ids.size(); ++color) {
        if (std::abs(t - stops[2 * color]) <= half_width(color))
            return {ids[color], ids[color], 0};
    }
    for (size_t i = 0; i + 1 < ids.size(); ++i) {
        const double lo = stops[2 * i] + half_width(i);
        const double hi = stops[2 * i + 2] - half_width(i + 1);
        if (t > hi && i + 2 < ids.size())
            continue;
        if (hi <= lo + 2e-8)
            return {ids[i + 1], ids[i + 1], 0};
        const double midpoint = std::clamp(double(stops[2 * i + 1]), lo + 1e-8, hi - 1e-8);
        const double ratio    = t <= midpoint ? 0.5 * (t - lo) / (midpoint - lo) : 0.5 + 0.5 * (t - midpoint) / (hi - midpoint);
        const int    percent  = int(std::lround(100.0 * std::clamp(ratio, 0.0, 1.0)));
        if (percent == 0)
            return {ids[i], ids[i], 0};
        if (percent == 100)
            return {ids[i + 1], ids[i + 1], 0};
        return {ids[i], ids[i + 1], percent};
    }
    return {ids.back(), ids.back(), 0};
}

std::string blend_mixed_components(const std::vector<unsigned int>&   ids,
                                   const std::vector<int>&            weights,
                                   const MixedFilamentDisplayContext& context)
{
    std::vector<MixedFilamentColorInput> inputs;
    for (size_t i = 0; i < ids.size() && i < weights.size(); ++i) {
        const unsigned int id = ids[i];
        if (id == 0 || id > context.physical_colors.size() || weights[i] <= 0)
            continue;
        MixedFilamentColorInput input{context.physical_colors[id - 1], weights[i]};
        if (id <= context.physical_tds.size() && context.physical_tds[id - 1] > 0.0)
            input.td_mm = context.physical_tds[id - 1];
        if (id <= context.physical_material_ids.size() && !context.physical_material_ids[id - 1].empty())
            input.material_id = context.physical_material_ids[id - 1];
        inputs.push_back(std::move(input));
    }
    return blend_mixed_color_inputs_auto(inputs, context.color_engine.value_or(MixedFilamentManager::color_engine()));
}

std::string mixed_gradient_display_color(const MixedFilament& entry, const MixedFilamentDisplayContext& context, double progress)
{
    const auto sample = sample_mixed_gradient(entry, context.num_physical, progress, context.preview_settings.gradient_middle_window);
    // The editor and Prepare view describe the requested gradient, like FS.
    // Applying the physical minimum here clips the transition to a narrow
    // range (or a single 50/50 color when a cycle fits only two minimum passes).
    // The slicing planner and layered preview still enforce printable heights.
    return blend_mixed_components({sample.component_a, sample.component_b}, {100 - sample.mix_b_percent, sample.mix_b_percent}, context);
}

} // namespace Slic3r
