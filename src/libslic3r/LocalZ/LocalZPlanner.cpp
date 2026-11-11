#include "LocalZPlanner.hpp"
#include "LocalZHeight.hpp"
#include "../MixedFilament/Slicing.hpp"
#include "../MixedFilament/ChannelMap.hpp"
#include "../Slicing/PreparationDiagnostics.hpp"
#include "../Slicing/LayerGeometryBatch.hpp"
#include "../Print.hpp"
#include "../Slicing.hpp"
#include "../Layer.hpp"
#include "../ClipperUtils.hpp"
#include "../SVG.hpp"
#include "../I18N.hpp"
#include <boost/log/trivial.hpp>
#include <tbb/parallel_for.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <map>

#define L(s) Slic3r::I18N::translate(s)
namespace Slic3r {
using namespace MixedFilamentSlicing;

static bool fit_pass_heights_to_interval(std::vector<double>& passes, double base_height, double lo, double hi)
{
    if (passes.empty() || base_height <= EPSILON)
        return false;

    double sum   = std::accumulate(passes.begin(), passes.end(), 0.0);
    double delta = base_height - sum;

    auto within = [lo, hi](double h) { return h >= lo - EPSILON && h <= hi + EPSILON; };
    if (std::abs(delta) > EPSILON) {
        if (within(passes.back() + delta)) {
            passes.back() += delta;
            delta = 0.0;
        } else if (delta > 0.0) {
            for (size_t i = passes.size(); i > 0 && delta > EPSILON; --i) {
                double&      h    = passes[i - 1];
                const double room = hi - h;
                if (room <= EPSILON)
                    continue;
                const double take = std::min(room, delta);
                h += take;
                delta -= take;
            }
        } else {
            for (size_t i = passes.size(); i > 0 && delta < -EPSILON; --i) {
                double&      h    = passes[i - 1];
                const double room = h - lo;
                if (room <= EPSILON)
                    continue;
                const double take = std::min(room, -delta);
                h -= take;
                delta += take;
            }
        }
    }

    if (std::abs(delta) > 1e-6)
        return false;
    return std::all_of(passes.begin(), passes.end(), within);
}

static bool sanitize_local_z_pass_heights(std::vector<double>& passes, double base_height, double min_sublayer_height)
{
    if (passes.empty() || base_height <= EPSILON)
        return false;

    const double lo = std::max<double>(0.01, min_sublayer_height);
    const double hi = std::max<double>(lo, base_height);
    for (double& h : passes) {
        if (!std::isfinite(h))
            h = lo;
        h = std::clamp(h, lo, hi);
    }
    return fit_pass_heights_to_interval(passes, base_height, lo, hi);
}

static std::vector<double> build_uniform_local_z_pass_heights(double base_height, double min_sublayer_height, size_t max_passes_limit = 0)
{
    std::vector<double> out;
    if (base_height <= EPSILON)
        return out;

    const double lo         = std::max<double>(0.01, min_sublayer_height);
    const double hi         = std::max<double>(lo, base_height);
    size_t       min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
    size_t       max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
    size_t       pass_count = min_passes;

    if (max_passes >= min_passes) {
        const double target_step   = 0.5 * (lo + hi);
        const size_t target_passes = size_t(std::max<double>(1.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
        pass_count                 = std::clamp(target_passes, min_passes, max_passes);
    }

    if (max_passes_limit > 0) {
        const size_t capped_limit = std::max<size_t>(1, max_passes_limit);
        if (pass_count > capped_limit)
            pass_count = capped_limit;
    }

    if (pass_count == 1 && base_height >= 2.0 * lo - EPSILON && max_passes >= 2)
        pass_count = 2;

    if (pass_count <= 1) {
        out.emplace_back(base_height);
        return out;
    }

    const double uniform_height = base_height / double(pass_count);
    out.assign(pass_count, uniform_height);

    // Keep the accumulated numeric error at the very top of the interval.
    double accumulated = 0.0;
    for (size_t i = 0; i + 1 < out.size(); ++i)
        accumulated += out[i];
    out.back() = std::max<double>(EPSILON, base_height - accumulated);
    return out;
}

static std::vector<double> build_uniform_local_z_pass_heights_exact(double base_height, double min_sublayer_height, size_t pass_count)
{
    if (base_height <= EPSILON || pass_count == 0)
        return {};

    const double lo = std::max<double>(0.01, min_sublayer_height);
    const double hi = std::max<double>(lo, base_height);
    if (pass_count == 1)
        return {base_height};

    if (double(pass_count) * lo > base_height + EPSILON || double(pass_count) * hi < base_height - EPSILON)
        return {};

    std::vector<double> out(pass_count, base_height / double(pass_count));
    if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
        return {};
    return out;
}

static inline void compute_local_z_component_heights(
    int mix_b_percent, double base_height, double min_sublayer_height, double& h_a, double& h_b)
{
    const auto heights = mixed_filament_local_z_pair_heights(base_height, min_sublayer_height, mix_b_percent);
    h_a                = heights.first;
    h_b                = heights.second;
}

static bool choose_local_z_start_with_component_a(const std::vector<double>& pass_heights,
                                                  double                     expected_h_a,
                                                  double                     expected_h_b,
                                                  size_t                     cadence_index)
{
    // Match component fractions, since fitting to the nominal layer can change
    // the absolute pass heights. Absolute errors can tie and swap unequal components.
    double even_height  = 0.0;
    double total_height = 0.0;
    for (size_t pass_i = 0; pass_i < pass_heights.size(); ++pass_i) {
        total_height += pass_heights[pass_i];
        if ((pass_i % 2) == 0)
            even_height += pass_heights[pass_i];
    }
    const double ratio_a    = expected_h_a / std::max<double>(EPSILON, expected_h_a + expected_h_b);
    const double ratio_even = even_height / std::max<double>(EPSILON, total_height);
    const double err_ab     = std::abs(ratio_even - ratio_a);
    const double err_ba     = std::abs((1.0 - ratio_even) - ratio_a);

    if (err_ab + 1e-6 < err_ba)
        return true;
    if (err_ba + 1e-6 < err_ab)
        return false;

    // When the requested component heights are equal (for example 50/50),
    // either A/B or B/A is numerically identical. Preserve the existing
    // row cadence so equal-split layers keep the normal local-Z A/B/A/B
    // sequence instead of flipping AB|BA between nominal layers.
    if (std::abs(expected_h_a - expected_h_b) <= 1e-6) {
        return (cadence_index % 2) == 0;
    }

    return expected_h_a >= expected_h_b;
}

static std::vector<double> build_local_z_alternating_pass_heights(
    double base_height, double min_sublayer_height, double gradient_h_a, double gradient_h_b, size_t max_passes_limit = 0)
{
    if (base_height <= EPSILON)
        return {};

    const double minimum = std::max<double>(0.01, min_sublayer_height);
    if (gradient_h_a <= EPSILON || gradient_h_b <= EPSILON || base_height < 2.0 * minimum - EPSILON ||
        (max_passes_limit > 0 && max_passes_limit < 2))
        return {base_height};

    const double cycle_h  = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
    const double ratio_a  = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);
    const double height_a = std::clamp(base_height * ratio_a, minimum, base_height - minimum);
    return {height_a, base_height - height_a};
}

static std::vector<double> build_local_z_two_pass_heights(double base_height,
                                                          double min_sublayer_height,
                                                          double gradient_h_a,
                                                          double gradient_h_b)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, min_sublayer_height);
    const double hi = std::max<double>(lo, base_height);
    if (gradient_h_a <= EPSILON || gradient_h_b <= EPSILON || base_height < 2.0 * lo - EPSILON)
        return {base_height};

    const double cycle_h = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
    const double ratio_a = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);

    const double h_a_min = std::max(lo, base_height - hi);
    const double h_a_max = std::min(hi, base_height - lo);
    if (h_a_min > h_a_max + EPSILON)
        return {base_height};

    const double h_a = std::clamp(base_height * ratio_a, h_a_min, h_a_max);
    const double h_b = base_height - h_a;

    std::vector<double> out{h_a, h_b};
    if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
        return {base_height};
    return out;
}

static std::vector<double> build_local_z_pass_heights(
    double base_height, double min_sublayer_height, double preferred_a, double preferred_b, size_t max_passes_limit = 0)
{
    if (base_height <= EPSILON)
        return {};

    const double lo = std::max<double>(0.01, min_sublayer_height);
    const double hi = std::max<double>(lo, base_height);

    std::vector<double> cadence_unit;
    if (preferred_a > EPSILON)
        cadence_unit.push_back(std::clamp(preferred_a, lo, hi));
    if (preferred_b > EPSILON)
        cadence_unit.push_back(std::clamp(preferred_b, lo, hi));

    if (!cadence_unit.empty()) {
        std::vector<double> out;
        out.reserve(size_t(std::ceil(base_height / lo)) + 2);

        double z_used = 0.0;
        size_t idx    = 0;
        size_t guard  = 0;
        while (z_used + cadence_unit[idx] < base_height - EPSILON && guard++ < 100000) {
            out.push_back(cadence_unit[idx]);
            z_used += cadence_unit[idx];
            idx = (idx + 1) % cadence_unit.size();
        }

        const double remainder = base_height - z_used;
        if (remainder > EPSILON)
            out.push_back(remainder);

        if (fit_pass_heights_to_interval(out, base_height, lo, hi) && (max_passes_limit == 0 || out.size() <= max_passes_limit))
            return out;

        if (max_passes_limit > 0 && preferred_a > EPSILON && preferred_b > EPSILON)
            return build_local_z_alternating_pass_heights(base_height, min_sublayer_height, preferred_a, preferred_b, max_passes_limit);
    }

    return build_uniform_local_z_pass_heights(base_height, lo, max_passes_limit);
}

static std::vector<unsigned int> decode_blend_component_ids(const MixedFilament& mf, size_t num_physical)
{
    auto ids = decode_gradient_component_ids(mf, num_physical);
    if (ids.empty()) {
        if (mf.component_a > 0 && mf.component_a <= num_physical)
            ids.push_back(mf.component_a);
        if (mf.component_b > 0 && mf.component_b <= num_physical && mf.component_b != mf.component_a)
            ids.push_back(mf.component_b);
    }
    return ids;
}

static std::vector<int> decode_blend_component_weights(const MixedFilament& mf, size_t num_physical, size_t count)
{
    auto weights = decode_gradient_component_weights(mf, count);
    if (weights.empty() && count == 2)
        weights = {100 - std::clamp(mf.mix_b_percent, 0, 100), std::clamp(mf.mix_b_percent, 0, 100)};
    return weights;
}

static bool local_z_gradient_mixed_definition(const MixedFilament& definition)
{
    if (!local_z_eligible_mixed_definition(definition) || !definition.gradient_enabled)
        return false;

    const MixedFilament* pair = &definition;
    return pair && pair->component_a != pair->component_b;
}

static std::vector<int> local_z_continuous_layer_range(std::vector<int> layers)
{
    if (layers.empty())
        return {};

    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());

    std::vector<int> out;
    out.reserve(size_t(layers.back() - layers.front() + 1));
    for (int layer = layers.front(); layer <= layers.back(); ++layer)
        out.emplace_back(layer);
    return out;
}

static std::vector<double> build_local_z_gradient_stop_positions(const MixedFilament& definition,
                                                                 size_t               num_physical,
                                                                 size_t               component_count)
{
    const auto stops = mixed_gradient_stops(definition, num_physical);
    return {stops.begin(), stops.end()};
}

static bool local_z_direct_multicolor_definition(const MixedFilament&       definition,
                                                 size_t                     num_physical,
                                                 std::vector<unsigned int>* component_ids     = nullptr,
                                                 std::vector<int>*          component_weights = nullptr)
{
    if (!local_z_eligible_mixed_definition(definition) || definition.gradient_enabled)
        return false;

    const std::vector<unsigned int> ids = decode_blend_component_ids(definition, num_physical);
    if (ids.size() < 3)
        return false;

    if (component_ids != nullptr)
        *component_ids = ids;
    if (component_weights != nullptr) {
        std::vector<int> weights = decode_blend_component_weights(definition, num_physical, ids.size());
        if (weights.empty())
            weights.assign(ids.size(), 1);
        *component_weights = std::move(weights);
    }
    return true;
}

struct LocalZActivePair
{
    unsigned int component_a               = 0;
    unsigned int component_b               = 0;
    int          mix_b_percent             = 50;
    bool         uses_layer_cycle_sequence = false;
    bool         single_component          = false;

    bool valid_pair(size_t num_physical) const
    {
        return component_a > 0 && component_a <= num_physical && component_b > 0 && component_b <= num_physical &&
               (component_a != component_b || single_component);
    }
};

static void local_z_orient_pair_to_follow_previous(LocalZActivePair& pair, unsigned int previous_extruder)
{
    if (pair.single_component)
        return;

    // Gradient Local-Z rows use B/A pass order. If the previous emitted pass
    // was B, flip the pair so the next nominal layer starts with the other
    // component and does not create a doubled physical layer at the boundary.
    if (previous_extruder != 0 && previous_extruder == pair.component_b && previous_extruder != pair.component_a) {
        std::swap(pair.component_a, pair.component_b);
        pair.mix_b_percent = std::clamp(100 - pair.mix_b_percent, 1, 99);
    }
}

static bool local_z_gradient_active_pair_for_progress(const std::vector<unsigned int>& ids,
                                                      const std::vector<double>&       stop_positions,
                                                      double                           progress,
                                                      double                           middle_window_fraction,
                                                      LocalZActivePair&                pair_out,
                                                      const std::vector<float>&        solid_widths)
{
    if (ids.size() < 2)
        return false;
    MixedFilament entry;
    entry.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
    entry.gradient_stop_positions.assign(stop_positions.begin(), stop_positions.end());
    entry.gradient_solid_widths = solid_widths;
    const auto sample         = sample_mixed_gradient(entry, MixedFilamentManager::kMaxPhysicalFilaments, progress, middle_window_fraction);
    pair_out                  = {};
    pair_out.component_a      = sample.component_a;
    pair_out.component_b      = sample.component_b;
    pair_out.mix_b_percent    = sample.mix_b_percent;
    pair_out.single_component = sample.component_a == sample.component_b;
    return sample.component_a != 0;
}

static void append_local_z_pair_option(
    std::vector<LocalZActivePair>& out, unsigned int component_a, unsigned int component_b, int weight_a, int weight_b)
{
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return;

    LocalZActivePair pair;
    pair.component_a               = component_a;
    pair.component_b               = component_b;
    pair.uses_layer_cycle_sequence = true;

    const int safe_weight_a = std::max(0, weight_a);
    const int safe_weight_b = std::max(0, weight_b);
    const int pair_total    = std::max(1, safe_weight_a + safe_weight_b);
    pair.mix_b_percent      = std::clamp(int(std::lround(100.0 * double(safe_weight_b) / double(pair_total))), 1, 99);
    out.emplace_back(pair);
}

static std::vector<LocalZActivePair> build_local_z_pair_cycle_for_definition(const MixedFilament& definition, size_t num_physical)
{
    std::vector<LocalZActivePair> pair_options;
    if (definition.deleted || num_physical == 0 || definition.gradient_enabled)
        return pair_options;

    const std::vector<unsigned int> blend_ids = decode_blend_component_ids(definition, num_physical);
    if (blend_ids.size() < 3)
        return pair_options;

    std::vector<int> gradient_weights = decode_blend_component_weights(definition, num_physical, blend_ids.size());
    if (gradient_weights.empty())
        gradient_weights.assign(blend_ids.size(), 1);

    std::vector<int> pair_weights;
    if (blend_ids.size() >= 4) {
        append_local_z_pair_option(pair_options, blend_ids[0], blend_ids[1], gradient_weights[0], gradient_weights[1]);
        append_local_z_pair_option(pair_options, blend_ids[2], blend_ids[3], gradient_weights[2], gradient_weights[3]);
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[1]));
        pair_weights.emplace_back(std::max(1, gradient_weights[2] + gradient_weights[3]));
    } else {
        append_local_z_pair_option(pair_options, blend_ids[0], blend_ids[1], gradient_weights[0], gradient_weights[1]);
        append_local_z_pair_option(pair_options, blend_ids[0], blend_ids[2], gradient_weights[0], gradient_weights[2]);
        append_local_z_pair_option(pair_options, blend_ids[1], blend_ids[2], gradient_weights[1], gradient_weights[2]);
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[1]));
        pair_weights.emplace_back(std::max(1, gradient_weights[0] + gradient_weights[2]));
        pair_weights.emplace_back(std::max(1, gradient_weights[1] + gradient_weights[2]));
    }

    if (pair_options.size() < 2 || pair_options.size() != pair_weights.size())
        return {};

    std::vector<unsigned int> pair_ids(pair_options.size(), 0);
    for (size_t idx = 0; idx < pair_ids.size(); ++idx)
        pair_ids[idx] = unsigned(idx + 1);

    const size_t                    max_pair_layers = definition.local_z_max_sublayers >= 2 ?
                                                          std::max<size_t>(1, size_t(definition.local_z_max_sublayers) / 2) :
                                                          size_t(0);
    const std::vector<unsigned int> pair_sequence   = build_weighted_gradient_sequence(pair_ids, pair_weights, max_pair_layers);
    if (pair_sequence.empty())
        return {};

    std::vector<LocalZActivePair> out;
    out.reserve(pair_sequence.size());
    for (const unsigned int pair_token : pair_sequence) {
        if (pair_token < 1 || pair_token > pair_options.size())
            continue;
        out.emplace_back(pair_options[size_t(pair_token - 1)]);
    }
    return out;
}

static std::vector<double> build_local_z_direct_multicolor_pass_heights(const MixedFilament&    definition,
                                                                        const std::vector<int>& component_weights,
                                                                        double                  base_height,
                                                                        double                  min_sublayer_height,
                                                                        size_t                  component_count)
{
    if (base_height <= EPSILON || component_count == 0)
        return {};

    const double lo         = std::max<double>(0.01, min_sublayer_height);
    const double hi         = std::max<double>(lo, base_height);
    const size_t min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
    const size_t max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
    if (max_passes == 0)
        return {base_height};

    size_t pass_limit = max_passes;
    if (definition.local_z_max_sublayers >= 2)
        pass_limit = std::min(pass_limit, size_t(std::max(2, definition.local_z_max_sublayers)));
    pass_limit = std::max(pass_limit, min_passes);

    std::vector<int> positive_weights;
    positive_weights.reserve(component_weights.size());
    for (const int weight : component_weights)
        if (weight > 0)
            positive_weights.emplace_back(weight);
    if (positive_weights.empty())
        positive_weights.assign(component_count, 1);

    const int           total_weight = std::max(1, std::accumulate(positive_weights.begin(), positive_weights.end(), 0));
    std::vector<double> component_targets;
    component_targets.reserve(positive_weights.size());
    size_t ideal_passes = 0;
    for (const int weight : positive_weights) {
        const double target = base_height * double(weight) / double(total_weight);
        component_targets.emplace_back(target);
        ideal_passes += size_t(std::max<double>(1.0, std::ceil((target - EPSILON) / hi)));
    }

    const size_t preferred_passes = std::max(component_targets.size(), ideal_passes);
    const size_t desired_passes   = std::min(std::max(preferred_passes, min_passes), pass_limit);

    std::vector<double> bins = component_targets;
    while (bins.size() > desired_passes) {
        std::sort(bins.begin(), bins.end());
        const double merged = bins[0] + bins[1];
        bins.erase(bins.begin(), bins.begin() + 2);
        bins.emplace_back(merged);
    }

    while (bins.size() < desired_passes) {
        auto it = std::max_element(bins.begin(), bins.end());
        if (it == bins.end())
            break;
        const double value = *it;
        if (value < 2.0 * lo - EPSILON)
            break;
        double first  = std::clamp(value * 0.5, lo, value - lo);
        double second = value - first;
        if (first < lo - EPSILON || second < lo - EPSILON)
            break;
        *it = first;
        bins.emplace_back(second);
    }

    if (bins.empty())
        return build_uniform_local_z_pass_heights(base_height, lo, desired_passes);

    std::sort(bins.begin(), bins.end(), std::greater<double>());
    for (double& value : bins)
        value = std::clamp(value, lo, hi);
    if (fit_pass_heights_to_interval(bins, base_height, lo, hi))
        return bins;

    for (size_t pass_count = desired_passes; pass_count >= min_passes; --pass_count) {
        std::vector<double> exact = build_uniform_local_z_pass_heights_exact(base_height, min_sublayer_height, pass_count);
        if (!exact.empty())
            return exact;
        if (pass_count == min_passes)
            break;
    }

    return build_uniform_local_z_pass_heights(base_height, lo, desired_passes);
}

static std::vector<unsigned int> build_local_z_direct_multicolor_sequence(const std::vector<unsigned int>& component_ids,
                                                                          const std::vector<int>&          component_weights,
                                                                          const std::vector<double>&       pass_heights,
                                                                          std::vector<double>&             carry_error_mm)
{
    if (component_ids.empty() || pass_heights.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          filtered_weights;
    filtered_ids.reserve(component_ids.size());
    filtered_weights.reserve(component_ids.size());
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        const int weight = idx < component_weights.size() ? std::max(0, component_weights[idx]) : 0;
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(component_ids[idx]);
        filtered_weights.emplace_back(weight);
    }
    if (filtered_ids.empty()) {
        filtered_ids = component_ids;
        filtered_weights.assign(component_ids.size(), 1);
    }
    if (filtered_ids.empty())
        return {};

    if (carry_error_mm.size() != filtered_ids.size())
        carry_error_mm.assign(filtered_ids.size(), 0.0);

    const double total_height = std::accumulate(pass_heights.begin(), pass_heights.end(), 0.0);
    const int    total_weight = std::max(1, std::accumulate(filtered_weights.begin(), filtered_weights.end(), 0));

    std::vector<double> desired_heights(filtered_ids.size(), 0.0);
    for (size_t idx = 0; idx < filtered_ids.size(); ++idx)
        desired_heights[idx] = total_height * double(filtered_weights[idx]) / double(total_weight) + carry_error_mm[idx];

    std::vector<double>       assigned_heights(filtered_ids.size(), 0.0);
    std::vector<unsigned int> sequence;
    sequence.reserve(pass_heights.size());
    int previous_choice = -1;

    for (const double pass_height : pass_heights) {
        size_t best_idx   = 0;
        double best_score = -std::numeric_limits<double>::infinity();
        double best_need  = -std::numeric_limits<double>::infinity();
        for (size_t idx = 0; idx < filtered_ids.size(); ++idx) {
            const double remaining_need = desired_heights[idx] - assigned_heights[idx];
            double       score          = remaining_need;
            if (int(idx) == previous_choice)
                score -= 0.35 * pass_height;

            if (score > best_score + 1e-9 || (std::abs(score - best_score) <= 1e-9 &&
                                              (remaining_need > best_need + 1e-9 || (std::abs(remaining_need - best_need) <= 1e-9 &&
                                                                                     filtered_ids[idx] < filtered_ids[best_idx])))) {
                best_idx   = idx;
                best_score = score;
                best_need  = remaining_need;
            }
        }

        assigned_heights[best_idx] += pass_height;
        previous_choice = int(best_idx);
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    for (size_t idx = 0; idx < filtered_ids.size(); ++idx)
        carry_error_mm[idx] = desired_heights[idx] - assigned_heights[idx];

    const double error_sum = std::accumulate(carry_error_mm.begin(), carry_error_mm.end(), 0.0);
    if (!carry_error_mm.empty() && std::abs(error_sum) > 1e-9) {
        const double correction = error_sum / double(carry_error_mm.size());
        for (double& value : carry_error_mm)
            value -= correction;
    }

    return sequence;
}

struct LocalZIndependentDirectPass
{
    unsigned int extruder_id{0};
    double       height{0.0};
};

static std::vector<LocalZIndependentDirectPass> build_local_z_independent_direct_cadence(const std::vector<unsigned int>& component_ids,
                                                                                         const std::vector<int>&          component_weights,
                                                                                         const PrintConfig& print_config)
{
    std::vector<LocalZIndependentDirectPass> cadence;
    if (component_ids.empty())
        return cadence;

    // Scale the complete ratio until every participating nozzle can print its share.
    double height_per_weight = 0.0;
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        const int weight = idx < component_weights.size() ? component_weights[idx] : 0;
        if (component_ids[idx] > 0 && weight > 0)
            height_per_weight = std::max(height_per_weight,
                                         Slicing::min_layer_height_from_nozzle(print_config, component_ids[idx]) / double(weight));
    }
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        const int weight = idx < component_weights.size() ? component_weights[idx] : 0;
        if (component_ids[idx] == 0 || weight <= 0)
            continue;

        const double total_component_height = height_per_weight * double(weight);
        const double max_layer_height       = Slicing::max_layer_height_from_nozzle(print_config, component_ids[idx]);
        const size_t split_count            = std::max<size_t>(1, size_t(std::ceil(total_component_height / max_layer_height - EPSILON)));
        const double split_height           = total_component_height / double(split_count);
        for (size_t split_idx = 0; split_idx < split_count; ++split_idx)
            cadence.push_back(LocalZIndependentDirectPass{component_ids[idx], split_height});
    }
    return cadence;
}

static void append_local_z_independent_component_height(std::vector<LocalZIndependentDirectPass>& cadence,
                                                        unsigned int                              component_id,
                                                        double                                    total_height,
                                                        const PrintConfig&                        print_config)
{
    if (component_id == 0 || total_height <= EPSILON)
        return;

    const double max_layer_height = Slicing::max_layer_height_from_nozzle(print_config, component_id);
    const size_t split_count      = std::max<size_t>(1, size_t(std::ceil(total_height / max_layer_height - EPSILON)));
    const double split_height     = total_height / double(split_count);
    for (size_t split_idx = 0; split_idx < split_count; ++split_idx)
        cadence.push_back(LocalZIndependentDirectPass{component_id, split_height});
}

static std::vector<LocalZIndependentDirectPass> build_local_z_independent_gradient_cadence(const MixedGradientLocalZSample& sample,
                                                                                           double             available_height,
                                                                                           unsigned int       previous_extruder,
                                                                                           const PrintConfig& print_config)
{
    std::vector<LocalZIndependentDirectPass> cadence;
    if (available_height <= EPSILON || sample.mix.component_a == 0 || sample.mix.component_b == 0)
        return cadence;

    LocalZActivePair pair;
    pair.component_a      = sample.mix.component_a;
    pair.component_b      = sample.mix.component_b;
    pair.mix_b_percent    = sample.mix.mix_b_percent;
    pair.single_component = pair.component_a == pair.component_b;
    double height_a       = sample.height_a;
    double height_b       = sample.height_b;
    if (height_a + height_b > available_height) {
        const auto clipped = mixed_filament_local_z_pair_heights(available_height,
                                                                 Slicing::min_layer_height_from_nozzle(print_config, pair.component_a),
                                                                 Slicing::min_layer_height_from_nozzle(print_config, pair.component_b),
                                                                 pair.mix_b_percent);
        height_a           = clipped.first;
        height_b           = clipped.second;
    }
    local_z_orient_pair_to_follow_previous(pair, previous_extruder);
    if (pair.component_a != sample.mix.component_a)
        std::swap(height_a, height_b);
    // Retain the established B/A order and per-extruder maximum-height splitting.
    append_local_z_independent_component_height(cadence, pair.component_b, height_b, print_config);
    append_local_z_independent_component_height(cadence, pair.component_a, height_a, print_config);
    return cadence;
}

static LocalZActivePair derive_local_z_active_pair(const MixedFilament&                 definition,
                                                   const std::vector<LocalZActivePair>& pair_cycle,
                                                   size_t                               num_physical,
                                                   int                                  cadence_index)
{
    LocalZActivePair out;

    if (!pair_cycle.empty()) {
        const int    cycle_i = int(pair_cycle.size());
        const size_t pos     = size_t(((cadence_index % cycle_i) + cycle_i) % cycle_i);
        return pair_cycle[pos];
    }

    const MixedFilament pair      = definition;
    out.component_a               = pair.component_a;
    out.component_b               = pair.component_b;
    out.mix_b_percent             = pair.mix_b_percent;
    out.uses_layer_cycle_sequence = false;
    return out;
}

struct LocalZPlanDiagnostics
{
    bool                                            enabled = preparation_diagnostic("ORCA_LOCAL_Z_DEBUG");
    std::map<std::pair<size_t, size_t>, ExPolygons> base_masks;
    void                                            record(const SubLayerPlan& plan, const ExPolygons& masks)
    {
        if (enabled)
            base_masks[{plan.layer_id, plan.pass_index}] = masks;
    }
    const ExPolygons& masks_for(const SubLayerPlan& plan) const
    {
        static const ExPolygons empty;
        const auto              it = base_masks.find({plan.layer_id, plan.pass_index});
        return it == base_masks.end() ? empty : it->second;
    }
};

static void export_local_z_plan_debug(const PrintObject&           print_object,
                                      const std::vector<double>&   nozzle_min_layer_heights,
                                      const LocalZPlanDiagnostics& diagnostics)
{
    if (!diagnostics.enabled)
        return;

    const std::vector<LocalZInterval>& intervals = print_object.local_z_intervals();
    const std::vector<SubLayerPlan>&   plans     = print_object.local_z_sublayer_plan();
    if (intervals.empty() || plans.empty())
        return;

    const int     object_id = int(print_object.id().id);
    std::ofstream json(debug_out_path("local-z-plan-obj-%d.json", object_id), std::ios::out | std::ios::trunc);
    if (json.good()) {
        json << std::fixed << std::setprecision(6);
        json << "{\n";
        json << "  \"object_id\": " << object_id << ",\n";
        json << "  \"nozzle_min_layer_heights\": [";
        for (size_t i = 0; i < nozzle_min_layer_heights.size(); ++i)
            json << (i ? ", " : "") << nozzle_min_layer_heights[i];
        json << "],\n";
        json << "  \"interval_count\": " << intervals.size() << ",\n";
        json << "  \"sublayer_count\": " << plans.size() << ",\n";
        json << "  \"intervals\": [\n";
        for (size_t i = 0; i < intervals.size(); ++i) {
            const LocalZInterval& interval = intervals[i];
            json << "    {\"layer_id\": " << interval.layer_id << ", \"z_lo\": " << interval.z_lo << ", \"z_hi\": " << interval.z_hi
                 << ", \"base_height\": " << interval.base_height << ", \"sublayer_height\": " << interval.sublayer_height
                 << ", \"has_mixed_paint\": " << (interval.has_mixed_paint ? "true" : "false")
                 << ", \"sublayer_count\": " << interval.sublayer_count << "}";
            if (i + 1 < intervals.size())
                json << ",";
            json << "\n";
        }
        json << "  ],\n";
        json << "  \"sublayers\": [\n";
        for (size_t i = 0; i < plans.size(); ++i) {
            const SubLayerPlan& plan = plans[i];
            json << "    {\"layer_id\": " << plan.layer_id << ", \"pass_index\": " << plan.pass_index
                 << ", \"split_interval\": " << (plan.split_interval ? "true" : "false") << ", \"z_lo\": " << plan.z_lo
                 << ", \"z_hi\": " << plan.z_hi << ", \"print_z\": " << plan.print_z << ", \"flow_height\": " << plan.flow_height
                 << ", \"base_mask_count\": " << diagnostics.masks_for(plan).size() << ", \"painted_mask_counts\": [";
            for (size_t eidx = 0; eidx < plan.painted_masks_by_extruder.size(); ++eidx) {
                json << plan.painted_masks_by_extruder[eidx].size();
                if (eidx + 1 < plan.painted_masks_by_extruder.size())
                    json << ", ";
            }
            json << "]}";
            if (i + 1 < plans.size())
                json << ",";
            json << "\n";
        }
        json << "  ]\n";
        json << "}\n";
    }

    static const std::array<const char*, 10> colors{"#E53935", "#1E88E5", "#43A047", "#FB8C00", "#8E24AA",
                                                    "#00897B", "#6D4C41", "#3949AB", "#C0CA33", "#F4511E"};
    for (const SubLayerPlan& plan : plans) {
        bool has_painted = std::any_of(plan.painted_masks_by_extruder.begin(), plan.painted_masks_by_extruder.end(),
                                       [](const ExPolygons& masks) { return !masks.empty(); });
        if (!plan.split_interval && !has_painted)
            continue;
        if (!has_painted && diagnostics.masks_for(plan).empty())
            continue;

        std::vector<std::pair<ExPolygons, SVG::ExPolygonAttributes>> layers;
        if (!diagnostics.masks_for(plan).empty()) {
            layers.emplace_back(diagnostics.masks_for(plan),
                                SVG::ExPolygonAttributes("base", "#D6D6D6", "#6A6A6A", "#6A6A6A", scale_(0.03), 0.45f));
        }
        for (size_t eidx = 0; eidx < plan.painted_masks_by_extruder.size(); ++eidx) {
            if (plan.painted_masks_by_extruder[eidx].empty())
                continue;
            const char* color = colors[eidx % colors.size()];
            layers.emplace_back(plan.painted_masks_by_extruder[eidx],
                                SVG::ExPolygonAttributes("E" + std::to_string(eidx + 1), color, color, color, scale_(0.03), 0.55f));
        }
        if (!layers.empty()) {
            SVG::export_expolygons(debug_out_path("local-z-plan-obj-%d-layer-%d-pass-%d.svg", object_id, int(plan.layer_id),
                                                  int(plan.pass_index)),
                                   layers);
        }
    }
}

std::vector<uint8_t> local_z_enabled_mixed_rows(const Print& print)
{
    const PrintConfig&        print_cfg = print.config();
    const DynamicPrintConfig& full_cfg  = print.full_print_config();
    const bool   global_local_z         = bool_from_full_config(full_cfg, "dithering_local_z_mode", print_cfg.dithering_local_z_mode.value);
    const size_t num_physical           = print_cfg.filament_colour.size();
    const std::vector<MixedFilament> definitions = print.mixed_filament_manager().mixed_filament_legacy_rows();

    std::vector<uint8_t> enabled_rows(definitions.size(), uint8_t(0));
    for (size_t row_idx = 0; row_idx < definitions.size(); ++row_idx) {
        if (mixed_filament_definition_uses_local_z(definitions[row_idx], global_local_z))
            enabled_rows[row_idx] = uint8_t(1);
    }
    return enabled_rows;
}

static std::vector<std::vector<ExPolygons>> whole_object_local_z_segmentation_by_mixed_wall(const PrintObject& print_object,
                                                                                            const std::vector<uint8_t>& local_z_enabled_rows)
{
    std::vector<std::vector<ExPolygons>> segmentation;

    const Print* print = print_object.print();
    if (print == nullptr || print_object.layer_count() == 0)
        return segmentation;

    const size_t num_physical = print->config().filament_colour.size();
    const size_t num_total    = print->mixed_filament_manager().total_filaments(num_physical);
    if (num_total <= num_physical)
        return segmentation;

    segmentation.assign(print_object.layer_count(), std::vector<ExPolygons>(num_total + 1));
    const MixedFilamentManager& mixed_mgr           = print->mixed_filament_manager();
    size_t                      mixed_region_layers = 0;
    size_t                      mixed_region_count  = 0;

    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        const Layer& layer                  = *print_object.get_layer(int(layer_id));
        bool         layer_has_mixed_region = false;
        for (int region_id = 0; region_id < layer.region_count(); ++region_id) {
            const LayerRegion* layerm = layer.get_region(region_id);
            if (layerm == nullptr || layerm->slices.empty())
                continue;

            const unsigned int filament_id = unsigned(std::max(0, layerm->region().config().wall_filament.value));
            if (!mixed_mgr.is_mixed(filament_id, num_physical))
                continue;
            const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(filament_id, num_physical);
            if (mixed_idx < 0 || size_t(mixed_idx) >= local_z_enabled_rows.size() || local_z_enabled_rows[size_t(mixed_idx)] == 0)
                continue;
            if (filament_id >= segmentation[layer_id].size())
                continue;

            ExPolygons state_masks = to_expolygons(layerm->slices.surfaces);
            if (state_masks.empty())
                continue;

            append(segmentation[layer_id][filament_id], std::move(state_masks));
            layer_has_mixed_region = true;
            ++mixed_region_count;
        }

        if (layer_has_mixed_region) {
            ++mixed_region_layers;
            for (size_t channel_idx = num_physical + 1; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
                ExPolygons& state_masks = segmentation[layer_id][channel_idx];
                if (state_masks.size() > 1)
                    state_masks = union_ex(state_masks);
            }
        }
    }

    if (mixed_region_count == 0)
        return {};

    BOOST_LOG_TRIVIAL(info) << "Local-Z whole-object wall segmentation prepared"
                            << " object=" << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                            << " mixed_region_layers=" << mixed_region_layers << " mixed_region_count=" << mixed_region_count
                            << " physical_filaments=" << num_physical << " total_filaments=" << num_total;
    return segmentation;
}

std::vector<std::vector<ExPolygons>> local_z_planner_segmentation_with_whole_object_mixed_wall(
    const PrintObject&                          print_object,
    const std::vector<std::vector<ExPolygons>>& paint_segmentation,
    const std::vector<uint8_t>&                 local_z_enabled_rows,
    bool&                                       has_whole_object_scope)
{
    has_whole_object_scope = false;
    const Print* print     = print_object.print();
    if (print == nullptr || paint_segmentation.empty())
        return paint_segmentation;

    const size_t                         num_physical               = print->config().filament_colour.size();
    const MixedFilamentManager&          mixed_mgr                  = print->mixed_filament_manager();
    size_t                               excluded_physical_channels = 0;
    size_t                               excluded_mixed_channels    = 0;
    std::vector<std::vector<ExPolygons>> planner_paint_segmentation(paint_segmentation.size());
    for (size_t layer_id = 0; layer_id < paint_segmentation.size(); ++layer_id) {
        planner_paint_segmentation[layer_id].resize(paint_segmentation[layer_id].size());
        for (size_t channel_idx = 1; channel_idx < paint_segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons& state_masks = paint_segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;

            const unsigned int state_id               = segmentation_channel_filament_id(channel_idx);
            const bool         state_is_mixed         = mixed_mgr.is_mixed(state_id, num_physical);
            bool               mixed_row_uses_local_z = false;
            if (state_is_mixed) {
                const int mixed_idx    = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
                mixed_row_uses_local_z = mixed_idx >= 0 && size_t(mixed_idx) < local_z_enabled_rows.size() &&
                                         local_z_enabled_rows[size_t(mixed_idx)] != 0;
            }
            if (!mixed_filament_local_z_painted_override_uses_planner(state_is_mixed, mixed_row_uses_local_z)) {
                if (state_is_mixed)
                    ++excluded_mixed_channels;
                else
                    ++excluded_physical_channels;
                continue;
            }
            planner_paint_segmentation[layer_id][channel_idx] = state_masks;
        }
    }

    std::vector<std::vector<ExPolygons>> augmented = whole_object_local_z_segmentation_by_mixed_wall(print_object, local_z_enabled_rows);
    if (augmented.empty())
        return planner_paint_segmentation;
    has_whole_object_scope = true;

    size_t overlay_layers           = 0;
    size_t overlay_mixed_channels   = 0;
    size_t physical_override_layers = 0;

    for (size_t layer_id = 0; layer_id < augmented.size() && layer_id < paint_segmentation.size(); ++layer_id) {
        if (augmented[layer_id].size() < paint_segmentation[layer_id].size())
            augmented[layer_id].resize(paint_segmentation[layer_id].size());

        ExPolygons painted_overrides;
        for (size_t channel_idx = 1; channel_idx < paint_segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons& state_masks = paint_segmentation[layer_id][channel_idx];
            if (!state_masks.empty())
                append(painted_overrides, state_masks);
        }
        if (painted_overrides.size() > 1)
            painted_overrides = union_ex(painted_overrides);

        bool layer_has_overlay = false;
        if (!painted_overrides.empty()) {
            bool clipped_for_physical_override = false;
            for (size_t channel_idx = num_physical + 1; channel_idx < augmented[layer_id].size(); ++channel_idx) {
                ExPolygons& state_masks = augmented[layer_id][channel_idx];
                if (state_masks.empty())
                    continue;
                const ExPolygons clipped_masks = diff_ex(state_masks, painted_overrides);
                if (clipped_masks.size() != state_masks.size())
                    clipped_for_physical_override = true;
                state_masks = clipped_masks;
            }
            if (clipped_for_physical_override)
                ++physical_override_layers;
            layer_has_overlay = true;
        }

        for (size_t channel_idx = 1; channel_idx < planner_paint_segmentation[layer_id].size(); ++channel_idx) {
            const ExPolygons& state_masks = planner_paint_segmentation[layer_id][channel_idx];
            if (state_masks.empty())
                continue;

            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (channel_idx >= augmented[layer_id].size())
                augmented[layer_id].resize(channel_idx + 1);

            append(augmented[layer_id][channel_idx], state_masks);
            layer_has_overlay = true;
            if (mixed_mgr.is_mixed(state_id, num_physical))
                ++overlay_mixed_channels;
        }

        for (size_t channel_idx = num_physical + 1; channel_idx < augmented[layer_id].size(); ++channel_idx) {
            ExPolygons& state_masks = augmented[layer_id][channel_idx];
            if (state_masks.size() > 1)
                state_masks = union_ex(state_masks);
        }
        if (layer_has_overlay)
            ++overlay_layers;
    }

    if (overlay_layers > 0) {
        BOOST_LOG_TRIVIAL(info) << "Local-Z planner merged whole-object mixed wall masks with painted overrides"
                                << " object="
                                << (print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>"))
                                << " overlay_layers=" << overlay_layers << " overlay_mixed_channels=" << overlay_mixed_channels
                                << " physical_override_layers=" << physical_override_layers
                                << " excluded_physical_channels=" << excluded_physical_channels
                                << " excluded_mixed_channels=" << excluded_mixed_channels;
    }
    return augmented;
}

static std::vector<ExPolygons> collect_local_z_fixed_state_masks_by_extruder(const std::vector<ExPolygons>& layer_segmentation,
                                                                             const size_t                   num_physical)
{
    std::vector<ExPolygons> masks_by_extruder(num_physical);
    for (size_t channel_idx = 1; channel_idx < layer_segmentation.size(); ++channel_idx) {
        const ExPolygons& state_masks = layer_segmentation[channel_idx];
        if (state_masks.empty())
            continue;

        const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
        if (state_id == 0 || state_id > num_physical)
            continue;
        append(masks_by_extruder[state_id - 1], state_masks);
    }
    for (ExPolygons& masks : masks_by_extruder)
        if (masks.size() > 1)
            masks = union_ex(masks);
    return masks_by_extruder;
}

static std::vector<ExPolygons> build_local_z_transition_fixed_masks_for_pass(const std::vector<ExPolygons>& current_masks_by_extruder,
                                                                             const std::vector<ExPolygons>& prev_masks_by_extruder,
                                                                             const std::vector<ExPolygons>& next_masks_by_extruder,
                                                                             const size_t                   pass_idx,
                                                                             const size_t                   pass_count)
{
    if (pass_count <= 1)
        return current_masks_by_extruder;

    std::vector<ExPolygons> pass_masks_by_extruder(current_masks_by_extruder.size());
    const bool              is_lowest_pass  = pass_idx == 0;
    const bool              is_highest_pass = pass_idx + 1 >= pass_count;

    for (size_t extruder_idx = 0; extruder_idx < current_masks_by_extruder.size(); ++extruder_idx) {
        const ExPolygons& current_masks = current_masks_by_extruder[extruder_idx];
        if (current_masks.empty())
            continue;

        const ExPolygons prev_masks = extruder_idx < prev_masks_by_extruder.size() ? prev_masks_by_extruder[extruder_idx] : ExPolygons();
        const ExPolygons next_masks = extruder_idx < next_masks_by_extruder.size() ? next_masks_by_extruder[extruder_idx] : ExPolygons();

        const ExPolygons current_and_prev = prev_masks.empty() ? ExPolygons() : intersection_ex(current_masks, prev_masks);
        const ExPolygons current_and_next = next_masks.empty() ? ExPolygons() : intersection_ex(current_masks, next_masks);
        const ExPolygons persistent       = current_and_prev.empty() || current_and_next.empty() ?
                                                ExPolygons() :
                                                intersection_ex(current_and_prev, current_and_next);

        ExPolygons entering = current_and_next;
        if (!entering.empty() && !current_and_prev.empty())
            entering = diff_ex(entering, current_and_prev);

        ExPolygons exiting = current_and_prev;
        if (!exiting.empty() && !current_and_next.empty())
            exiting = diff_ex(exiting, current_and_next);

        ExPolygons covered;
        if (!persistent.empty())
            append(covered, persistent);
        if (!entering.empty())
            append(covered, entering);
        if (!exiting.empty())
            append(covered, exiting);
        if (covered.size() > 1)
            covered = union_ex(covered);

        const ExPolygons isolated = covered.empty() ? current_masks : diff_ex(current_masks, covered);

        ExPolygons assigned;
        if (!persistent.empty())
            append(assigned, persistent);
        if (is_lowest_pass && !exiting.empty())
            append(assigned, exiting);
        if (is_highest_pass) {
            if (!entering.empty())
                append(assigned, entering);
            if (!isolated.empty())
                append(assigned, isolated);
        }
        if (assigned.size() > 1)
            assigned = union_ex(assigned);
        pass_masks_by_extruder[extruder_idx] = std::move(assigned);
    }

    return pass_masks_by_extruder;
}

struct IndependentCadenceState
{
    bool                                     active{false};
    double                                   z_cursor{0.0};
    double                                   z_end{0.0};
    size_t                                   cadence_index{0};
    size_t                                   emitted_passes{0};
    std::vector<LocalZIndependentDirectPass> gradient_cadence;
    size_t                                   gradient_pass_index{0};
    unsigned int                             last_extruder{0};
};

struct LocalZRowState
{
    std::vector<double>     component_error;
    IndependentCadenceState independent;
    int                     cadence_index          = 0;
    int                     pair_cycle_index       = 0;
    uint8_t                 was_active             = 0;
    unsigned int            last_gradient_extruder = 0;
};

struct LocalZRow
{
    MixedFilament                            definition;
    double                                   min_sublayer_height = 0.01;
    uint8_t                                  uses_local_z = 0;
    uint8_t                                  is_gradient  = 0;
    std::vector<unsigned int>                gradient_components;
    std::vector<double>                      gradient_stops;
    std::vector<unsigned int>                direct_components;
    std::vector<int>                         direct_weights;
    std::vector<LocalZIndependentDirectPass> direct_cadence;
    uint8_t                                  uses_direct_solver = 0;
    std::vector<LocalZActivePair>            pair_cycle;
    uint8_t                                  uses_pair_cycle = 0;
    std::vector<int>                         gradient_layers;
    std::pair<double, double>                gradient_bounds = {};
    LocalZRowState                           state;
};

struct PreparedLocalZGeometry
{
    ExPolygons                mixed_masks, base_masks, fixed_union;
    std::vector<ExPolygons>   row_masks, owned_fixed_masks;
    std::vector<unsigned int> row_ids;
    std::vector<uint8_t>      row_active;
    size_t                    mixed_state_count, dominant_mixed_idx;
    bool                      has_mixed_paint;

    bool same_as(const PreparedLocalZGeometry& other) const
    {
        return mixed_masks == other.mixed_masks && base_masks == other.base_masks && fixed_union == other.fixed_union &&
               row_masks == other.row_masks && row_ids == other.row_ids && row_active == other.row_active &&
               mixed_state_count == other.mixed_state_count && dominant_mixed_idx == other.dominant_mixed_idx &&
               has_mixed_paint == other.has_mixed_paint;
    }
};

static PreparedLocalZGeometry prepare_local_z_geometry(const PrintObject&                          print_object,
                                                       const std::vector<std::vector<ExPolygons>>& segmentation,
                                                       const std::vector<LocalZRow>&               rows,
                                                       const std::vector<int>&                     mixed_rows,
                                                       size_t                                      num_physical,
                                                       bool                                        local_z_whole_objects,
                                                       PreparationProfile&                         prep_profile,
                                                       bool                                        capture_base_masks,
                                                       size_t                                      layer_id,
                                                       const std::vector<ExPolygons>*              prepared_fixed)
{
    const Layer&         layer           = *print_object.get_layer(int(layer_id));
    bool                 has_mixed_paint = false;
    PreparationTimer     scan_timer(&prep_profile, "active_scan");
    ExPolygons           mixed_masks;
    size_t               mixed_state_count = 0;
    std::vector<uint8_t> row_active_this_layer(rows.size(), uint8_t(0));
    size_t               dominant_mixed_idx  = size_t(-1);
    double               dominant_mixed_area = -1.0;
    for (size_t channel_idx = 0; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
        const ExPolygons& state_masks = segmentation[layer_id][channel_idx];
        if (state_masks.empty())
            continue;

        const int mixed_idx = mixed_rows[channel_idx];
        if (mixed_idx < 0 || size_t(mixed_idx) >= rows.size())
            continue;
        if (rows[size_t(mixed_idx)].uses_local_z == 0)
            continue;
        const MixedFilament& definition = rows[size_t(mixed_idx)].definition;
        if (!local_z_eligible_mixed_definition(definition))
            continue;
        has_mixed_paint                          = true;
        row_active_this_layer[size_t(mixed_idx)] = uint8_t(1);
        ++mixed_state_count;
        append(mixed_masks, state_masks);

        const double mixed_area = std::abs(area(state_masks));
        if (mixed_area > dominant_mixed_area) {
            dominant_mixed_area = mixed_area;
            dominant_mixed_idx  = size_t(mixed_idx);
        }
    }
    scan_timer.finish();
    PreparationTimer geometry_timer(&prep_profile, "geometry");
    if (!mixed_masks.empty())
        mixed_masks = union_ex(mixed_masks);
    if (layer_id == 0)
        has_mixed_paint = false;

    ExPolygons base_masks = capture_base_masks ? collect_layer_region_slices(layer) : ExPolygons();
    if (has_mixed_paint && !base_masks.empty() && !mixed_masks.empty()) {
        base_masks = diff_ex(base_masks, mixed_masks);
        if (!base_masks.empty()) {
            const Polygons filtered = opening(to_polygons(base_masks), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
            base_masks              = union_ex(filtered);
        }
    }

    std::vector<ExPolygons>   row_state_masks(rows.size());
    std::vector<unsigned int> row_state_ids(rows.size(), 0);
    std::vector<ExPolygons>   owned_fixed_masks(num_physical);
    for (size_t channel_idx = 0; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
        const ExPolygons& state_masks = segmentation[layer_id][channel_idx];
        if (state_masks.empty())
            continue;
        const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
        if (state_id >= 1 && state_id <= num_physical) {
            if (!prepared_fixed)
                append(owned_fixed_masks[state_id - 1], state_masks);
            continue;
        }
        const int mixed_idx = mixed_rows[channel_idx];
        if (mixed_idx < 0 || size_t(mixed_idx) >= rows.size())
            continue;
        const size_t row_idx = size_t(mixed_idx);
        if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_definition(rows[row_idx].definition))
            continue;
        row_state_ids[row_idx] = state_id;
        append(row_state_masks[row_idx], state_masks);
    }
    for (ExPolygons& state_masks : row_state_masks)
        if (state_masks.size() > 1)
            state_masks = union_ex(state_masks);
    for (ExPolygons& state_masks : owned_fixed_masks)
        if (state_masks.size() > 1)
            state_masks = union_ex(state_masks);
    const auto& fixed_state_masks_by_extruder = prepared_fixed ? *prepared_fixed : owned_fixed_masks;

    ExPolygons fixed_state_masks_union;
    for (const ExPolygons& state_masks : fixed_state_masks_by_extruder)
        if (!state_masks.empty())
            append(fixed_state_masks_union, state_masks);
    if (fixed_state_masks_union.size() > 1)
        fixed_state_masks_union = union_ex(fixed_state_masks_union);
    if (has_mixed_paint && local_z_whole_objects && !fixed_state_masks_union.empty()) {
        if (!base_masks.empty()) {
            base_masks = diff_ex(base_masks, fixed_state_masks_union);
            if (!base_masks.empty()) {
                const Polygons filtered = opening(to_polygons(base_masks), scaled<float>(5. * EPSILON), scaled<float>(5. * EPSILON));
                base_masks              = union_ex(filtered);
            }
        }
        append(mixed_masks, fixed_state_masks_union);
        if (mixed_masks.size() > 1)
            mixed_masks = union_ex(mixed_masks);
    }
    if (local_z_whole_objects && !fixed_state_masks_union.empty()) {
        constexpr double LOCAL_Z_WHOLE_OBJECT_FIXED_GUARD_MM = 0.10;
        ExPolygons       fixed_state_guard_masks = offset_ex(fixed_state_masks_union, float(scale_(LOCAL_Z_WHOLE_OBJECT_FIXED_GUARD_MM)));
        if (fixed_state_guard_masks.empty())
            fixed_state_guard_masks = fixed_state_masks_union;
        else if (fixed_state_guard_masks.size() > 1)
            fixed_state_guard_masks = union_ex(fixed_state_guard_masks);

        for (ExPolygons& state_masks : row_state_masks) {
            if (state_masks.empty())
                continue;
            state_masks = diff_ex(state_masks, fixed_state_guard_masks);
            if (state_masks.size() > 1)
                state_masks = union_ex(state_masks);
        }
    }

    geometry_timer.finish();
    return PreparedLocalZGeometry{std::move(mixed_masks),
                                  std::move(base_masks),
                                  std::move(fixed_state_masks_union),
                                  std::move(row_state_masks),
                                  std::move(owned_fixed_masks),
                                  std::move(row_state_ids),
                                  std::move(row_active_this_layer),
                                  mixed_state_count,
                                  dominant_mixed_idx,
                                  has_mixed_paint};
}

struct LocalZSettings
{
    bool     local_z_mode;
    bool     local_z_whole_objects;
    bool     local_z_preserve_first_layer;
    bool     local_z_direct_multicolor;
    bool     local_z_independent_layer_height;
    coordf_t preferred_a;
    coordf_t preferred_b;
    coordf_t gradient_nominal_height;
    double   gradient_middle_window_fraction;
};

static LocalZSettings resolve_local_z_settings(const Print& print, bool whole_object_scope_active)
{
    const DynamicPrintConfig& full_cfg  = print.full_print_config();
    const PrintConfig&        print_cfg = print.config();
    const bool local_z_mode             = bool_from_full_config(full_cfg, "dithering_local_z_mode", print_cfg.dithering_local_z_mode.value);
    const bool global_full_domain       = bool_from_full_config(full_cfg, "dithering_local_z_whole_objects",
                                                                print_cfg.dithering_local_z_whole_objects.value);
    const bool local_z_whole_objects    = mixed_filament_local_z_uses_full_domain(global_full_domain, whole_object_scope_active);
    const bool local_z_preserve_first_layer     = bool_from_full_config(full_cfg, "dithering_local_z_preserve_first_layer",
                                                                        print_cfg.dithering_local_z_preserve_first_layer.value);
    const bool local_z_direct_multicolor        = bool_from_full_config(full_cfg, "dithering_local_z_direct_multicolor",
                                                                        print_cfg.dithering_local_z_direct_multicolor.value);
    const bool local_z_independent_layer_height = local_z_direct_multicolor &&
                                                  (local_z_mode ||
                                                   bool_from_full_config(full_cfg, "dithering_local_z_independent_layer_height",
                                                                         print_cfg.dithering_local_z_independent_layer_height.value));
    coordf_t preferred_a             = float_from_full_config(full_cfg, "mixed_color_layer_height_a",
                                                              coordf_t(print_cfg.mixed_color_layer_height_a.value));
    coordf_t preferred_b             = float_from_full_config(full_cfg, "mixed_color_layer_height_b",
                                                              coordf_t(print_cfg.mixed_color_layer_height_b.value));
    coordf_t gradient_nominal_height = float_from_full_config(full_cfg, "dithering_local_z_gradient_layer_height",
                                                              coordf_t(print_cfg.dithering_local_z_gradient_layer_height.value));
    coordf_t gradient_middle_filament_window =
        float_from_full_config(full_cfg, "dithering_local_z_gradient_middle_filament_window",
                               coordf_t(print_cfg.dithering_local_z_gradient_middle_filament_window.value));
    preferred_a                                  = std::max<coordf_t>(0.f, preferred_a);
    preferred_b                                  = std::max<coordf_t>(0.f, preferred_b);
    gradient_nominal_height                      = std::max<coordf_t>(gradient_nominal_height, 0.01);
    gradient_middle_filament_window              = std::clamp<coordf_t>(gradient_middle_filament_window, 0.f, 100.f);
    const double gradient_middle_window_fraction = double(gradient_middle_filament_window) / 100.0;

    return {local_z_mode,
            local_z_whole_objects,
            local_z_preserve_first_layer,
            local_z_direct_multicolor,
            local_z_independent_layer_height,
            preferred_a,
            preferred_b,
            gradient_nominal_height,
            gradient_middle_window_fraction};
}

static std::vector<LocalZRow> prepare_local_z_rows(const Print& print, const LocalZSettings& settings)
{
    const auto&            print_cfg    = print.config();
    const size_t           num_physical = print_cfg.filament_colour.size();
    const auto&            definitions  = print.mixed_filament_manager().mixed_filament_legacy_rows();
    std::vector<LocalZRow> rows(definitions.size());
    const bool             automatic_heights = settings.preferred_a <= EPSILON && settings.preferred_b <= EPSILON;
    for (size_t i = 0; i < rows.size(); ++i) {
        auto& row        = rows[i];
        row.definition   = definitions[i];
        row.uses_local_z = mixed_filament_definition_uses_local_z(row.definition, settings.local_z_mode);
        row.is_gradient  = row.uses_local_z && local_z_gradient_mixed_definition(row.definition);
        if (row.is_gradient) {
            row.gradient_components = mixed_gradient_components(row.definition, num_physical);
            row.gradient_stops      = build_local_z_gradient_stop_positions(row.definition, num_physical, row.gradient_components.size());
        }
        if (settings.local_z_direct_multicolor && automatic_heights &&
            local_z_direct_multicolor_definition(row.definition, num_physical, &row.direct_components, &row.direct_weights)) {
            row.uses_direct_solver = true;
            row.state.component_error.assign(row.direct_components.size(), 0.0);
            row.direct_cadence = build_local_z_independent_direct_cadence(row.direct_components, row.direct_weights, print_cfg);
        }
        if (settings.local_z_independent_layer_height && automatic_heights && row.direct_cadence.empty() && row.uses_local_z &&
            local_z_eligible_mixed_definition(row.definition) && !row.definition.gradient_enabled) {
            const auto component_ids = decode_blend_component_ids(row.definition, num_physical);
            if (component_ids.size() >= 2) {
                auto component_weights = decode_blend_component_weights(row.definition, num_physical, component_ids.size());
                if (component_weights.empty())
                    component_weights.assign(component_ids.size(), 1);
                row.direct_cadence = build_local_z_independent_direct_cadence(component_ids, component_weights, print_cfg);
            }
        }
        const auto component_ids     = row.is_gradient ? row.gradient_components : decode_blend_component_ids(row.definition, num_physical);
        const auto component_weights = decode_blend_component_weights(row.definition, num_physical, component_ids.size());
        for (size_t component = 0; component < component_ids.size(); ++component) {
            if (row.is_gradient || component_weights.empty() || component_weights[component] > 0)
                row.min_sublayer_height = std::max(row.min_sublayer_height,
                                                   Slicing::min_layer_height_from_nozzle(print_cfg, component_ids[component]));
        }
        if (!row.uses_direct_solver) {
            row.pair_cycle      = build_local_z_pair_cycle_for_definition(row.definition, num_physical);
            row.uses_pair_cycle = !row.pair_cycle.empty();
        }
    }

    return rows;
}

static void build_local_z_plan(PrintObject&                                print_object,
                               const std::vector<std::vector<ExPolygons>>& segmentation,
                               bool                                        whole_object_scope_active,
                               const std::function<void()>&                throw_on_cancel)
{
    LocalZPlanDiagnostics diagnostics;
    PreparationProfile    prep_profile("local_z_plan");
    PreparationTimer      plan_timer(&prep_profile, "total");
    print_object.clear_local_z_plan();

    const Print*      print       = print_object.print();
    const std::string object_name = print_object.model_object() ? print_object.model_object()->name : std::string("<unknown>");
    if (print == nullptr || print_object.layer_count() == 0 || segmentation.size() != print_object.layer_count()) {
        BOOST_LOG_TRIVIAL(debug) << "Local-Z plan skipped: invalid preconditions"
                                 << " object=" << object_name << " print_ptr=" << (print != nullptr)
                                 << " layer_count=" << print_object.layer_count() << " segmentation_layers=" << segmentation.size();
        return;
    }

    const PrintConfig& print_cfg = print->config();
    const auto         settings  = resolve_local_z_settings(*print, whole_object_scope_active);

    const size_t num_physical = print_cfg.filament_colour.size();
    if (num_physical == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan skipped: no physical filaments"
                                   << " object=" << object_name;
        return;
    }

    std::vector<double> nozzle_min_layer_heights(num_physical), nozzle_max_layer_heights(num_physical);
    for (size_t i = 0; i < num_physical; ++i) {
        nozzle_min_layer_heights[i] = Slicing::min_layer_height_from_nozzle(print_cfg, int(i + 1));
        nozzle_max_layer_heights[i] = Slicing::max_layer_height_from_nozzle(print_cfg, int(i + 1));
    }

    const MixedFilamentManager& mixed_mgr = print->mixed_filament_manager();
    auto                        rows      = prepare_local_z_rows(*print, settings);
    const auto enabled_row_count = std::count_if(rows.begin(), rows.end(), [](const LocalZRow& row) { return row.uses_local_z != 0; });
    if (enabled_row_count == 0)
        return;

    BOOST_LOG_TRIVIAL(debug) << "Local-Z plan start"
                             << " object=" << object_name << " layers=" << print_object.layer_count() << " nozzle_height_limits=1"
                             << " preferred_a=" << settings.preferred_a << " preferred_b=" << settings.preferred_b
                             << " gradient_nominal_height=" << settings.gradient_nominal_height
                             << " direct_multicolor=" << (settings.local_z_direct_multicolor ? 1 : 0)
                             << " independent_layer_height=" << (settings.local_z_independent_layer_height ? 1 : 0)
                             << " global_mode=" << (settings.local_z_mode ? 1 : 0) << " enabled_rows=" << enabled_row_count
                             << " full_domain=" << (settings.local_z_whole_objects ? 1 : 0)
                             << " preserve_first_layer=" << (settings.local_z_preserve_first_layer ? 1 : 0)
                             << " gradient_middle_filament_window=" << (100.0 * settings.gradient_middle_window_fraction)
                             << " physical_filaments=" << num_physical;

    std::vector<LocalZInterval> intervals;
    std::vector<SubLayerPlan>   plans;
    intervals.reserve(print_object.layer_count());
    size_t mixed_intervals                       = 0;
    size_t split_intervals                       = 0;
    size_t non_split_mixed_intervals             = 0;
    size_t total_generated_sublayer_cnt          = 0;
    size_t total_mixed_state_layers              = 0;
    size_t forced_height_resolve_calls           = 0;
    size_t forced_height_resolve_invalid_target  = 0;
    size_t split_passes_total                    = 0;
    size_t split_passes_with_painted_masks       = 0;
    size_t split_intervals_without_painted_masks = 0;
    size_t strict_ab_assignments                 = 0;
    size_t alternating_height_intervals          = 0;

    // Keep local-Z cadence isolated per mixed row so independent painted zones
    // do not influence each other when resolving fallback cadence.
    // Multi-color layer-cycle rows choose a pair once per nominal layer/zone
    // and rotate that pair independently from per-subpass A/B cadence.
    // Painted-only Local-Z keeps cadence isolated per zone. Whole-object Local-Z
    // instead syncs newly introduced painted rows to the dominant mixed cadence
    // so painted islands do not restart their phase at the boundary.

    std::vector<uint8_t> independent_layers(segmentation.size(), uint8_t(0));
    for (size_t scan_layer = 0; scan_layer < segmentation.size(); ++scan_layer) {
        std::vector<uint8_t> row_active_scan(rows.size(), uint8_t(0));
        for (size_t channel_idx = 0; channel_idx < segmentation[scan_layer].size(); ++channel_idx) {
            if (segmentation[scan_layer][channel_idx].empty())
                continue;
            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;
            const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
            if (mixed_idx < 0 || size_t(mixed_idx) >= rows.size())
                continue;
            if (rows[size_t(mixed_idx)].uses_local_z == 0)
                continue;
            row_active_scan[size_t(mixed_idx)] = uint8_t(1);
        }
        bool any_active           = false;
        bool independent_eligible = true;
        for (size_t row_idx = 0; row_idx < row_active_scan.size(); ++row_idx) {
            if (row_active_scan[row_idx] == 0)
                continue;
            any_active           = true;
            independent_eligible = independent_eligible && (rows[row_idx].is_gradient != 0 || (settings.local_z_independent_layer_height &&
                                                                                               !rows[row_idx].direct_cadence.empty()));
        }
        independent_layers[scan_layer] = any_active && independent_eligible &&
                                         mixed_filament_local_z_should_subdivide_layer(scan_layer, settings.local_z_whole_objects,
                                                                                       settings.local_z_preserve_first_layer);
        for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
            if (rows[row_idx].is_gradient == 0 || row_active_scan[row_idx] == 0)
                continue;
            rows[row_idx].gradient_layers.push_back(int(scan_layer));
        }
    }

    for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
        if (rows[row_idx].is_gradient == 0)
            continue;
        rows[row_idx].gradient_layers = local_z_continuous_layer_range(std::move(rows[row_idx].gradient_layers));
    }

    for (size_t row_idx = 0; row_idx < rows.size(); ++row_idx) {
        const std::vector<int>& gradient_layers = rows[row_idx].gradient_layers;
        if (gradient_layers.empty())
            continue;
        const Layer* first_layer = print_object.get_layer(gradient_layers.front());
        const Layer* last_layer  = print_object.get_layer(gradient_layers.back());
        if (first_layer == nullptr || last_layer == nullptr)
            continue;
        rows[row_idx].gradient_bounds = {first_layer->print_z - first_layer->height, last_layer->print_z};
    }

    auto gradient_progress_for_definition = [&](size_t row_idx, size_t layer_id) -> std::optional<double> {
        if (row_idx >= rows.size() || rows[row_idx].is_gradient == 0)
            return std::nullopt;
        const auto& layers = rows[row_idx].gradient_layers;
        if (layers.empty())
            return std::nullopt;

        const int li = int(layer_id);
        if (li < layers.front() || li > layers.back())
            return std::nullopt;

        const int idx   = li - layers.front();
        const int count = layers.back() - layers.front() + 1;
        return count > 0 ? std::optional<double>((2.0 * double(idx) + 1.0) / (2.0 * double(count))) : std::optional<double>(0.5);
    };

    auto gradient_progress_step_for_definition = [&](size_t row_idx) -> double {
        if (row_idx >= rows.size() || rows[row_idx].is_gradient == 0)
            return 1.0;
        const auto& layers = rows[row_idx].gradient_layers;
        if (layers.empty())
            return 1.0;

        const int count = layers.back() - layers.front() + 1;
        return count > 0 ? 1.0 / double(count) : 1.0;
    };

    auto effective_gradient_active_pair_for_definition = [&](size_t row_idx, size_t layer_id, LocalZActivePair& pair_out) -> bool {
        if (row_idx >= rows.size())
            return false;

        const std::vector<unsigned int>& ids            = rows[row_idx].gradient_components;
        const std::optional<double>      progress       = gradient_progress_for_definition(row_idx, layer_id);
        const auto&                      stop_positions = rows[row_idx].gradient_stops;
        if (!progress)
            return false;

        if (!local_z_gradient_active_pair_for_progress(ids, stop_positions, *progress, settings.gradient_middle_window_fraction, pair_out,
                                                       rows[row_idx].definition.gradient_solid_widths))
            return false;
        local_z_orient_pair_to_follow_previous(pair_out, rows[row_idx].state.last_gradient_extruder);
        return pair_out.valid_pair(num_physical);
    };

    auto effective_gradient_heights_for_definition = [&](size_t row_idx, size_t layer_id, double base_height, double& h_a_out,
                                                         double& h_b_out) -> bool {
        if (row_idx >= rows.size() || rows[row_idx].is_gradient == 0 || base_height <= EPSILON)
            return false;

        LocalZActivePair active_pair;
        if (effective_gradient_active_pair_for_definition(row_idx, layer_id, active_pair)) {
            const double r_a = std::clamp(1.0 - double(active_pair.mix_b_percent) / 100.0, 0.01, 0.99);
            h_a_out          = r_a * base_height;
            h_b_out          = (1.0 - r_a) * base_height;
            return true;
        }

        const std::optional<double> progress = gradient_progress_for_definition(row_idx, layer_id);
        if (!progress)
            return false;

        const MixedFilament& gradient = rows[row_idx].definition;
        double r_a = double(gradient.gradient_start) + (double(gradient.gradient_end) - double(gradient.gradient_start)) * *progress;
        r_a        = std::clamp(r_a, 0.01, 0.99);
        h_a_out    = r_a * base_height;
        h_b_out    = (1.0 - r_a) * base_height;
        return true;
    };

    auto build_direct_pass_heights_for_definition = [&](size_t row_idx, size_t layer_id, double base_height) -> std::vector<double> {
        if (row_idx >= rows.size())
            return {};

        (void) layer_id;
        return build_local_z_direct_multicolor_pass_heights(rows[row_idx].definition, rows[row_idx].direct_weights, base_height,
                                                            rows[row_idx].min_sublayer_height, rows[row_idx].direct_components.size());
    };

    auto build_direct_sequence_for_definition = [&](size_t row_idx, size_t layer_id,
                                                    const std::vector<double>& pass_heights) -> std::vector<unsigned int> {
        if (row_idx >= rows.size() || pass_heights.empty())
            return {};

        (void) layer_id;
        return build_local_z_direct_multicolor_sequence(rows[row_idx].direct_components, rows[row_idx].direct_weights, pass_heights,
                                                        rows[row_idx].state.component_error);
    };

    auto row_is_active_on_layer = [&](size_t row_idx, size_t layer_id) {
        if (row_idx >= rows.size() || layer_id >= segmentation.size())
            return false;
        for (size_t channel_idx = 0; channel_idx < segmentation[layer_id].size(); ++channel_idx) {
            if (segmentation[layer_id][channel_idx].empty())
                continue;
            const unsigned int state_id = segmentation_channel_filament_id(channel_idx);
            if (!mixed_mgr.is_mixed(state_id, num_physical))
                continue;
            const int mixed_idx = mixed_mgr.mixed_index_from_filament_id(state_id, num_physical);
            if (mixed_idx >= 0 && size_t(mixed_idx) == row_idx)
                return true;
        }
        return false;
    };

    size_t channel_count = 0;
    for (const auto& channels : segmentation)
        channel_count = std::max(channel_count, channels.size());
    const std::vector<int> mixed_rows = mixed_filament_channel_rows(mixed_mgr, num_physical, channel_count);

    // Only immutable geometry is prepared in parallel. Cadence, gradient state,
    // error accumulation, dependency IDs and plan emission remain ordered below.
    auto prepare_geometry = [&](size_t layer_index, const std::vector<ExPolygons>* fixed) {
        throw_on_cancel();
        return prepare_local_z_geometry(print_object, segmentation, rows, mixed_rows, num_physical, settings.local_z_whole_objects,
                                        prep_profile, diagnostics.enabled, layer_index, fixed);
    };
    const bool                             parallel_geometry = preparation_experiment("local_z_geometry");
    const bool                             reuse_fixed_masks = preparation_experiment("fixed_masks") && !parallel_geometry;
    const bool                             validate_geometry = preparation_diagnostic("ORCA_PREP_VALIDATE_REUSE");
    std::array<std::vector<ExPolygons>, 3> fixed_mask_window;
    if (reuse_fixed_masks)
        fixed_mask_window[0] = collect_local_z_fixed_state_masks_by_extruder(segmentation[0], num_physical);
    // Limit retained work by both layers and input vertices. An oversized single
    // layer must still run; the input budget is not a hard output-memory bound.
    constexpr size_t                     batch_layer_limit = 64, batch_point_limit = 131072;
    std::vector<PreparedLocalZGeometry>  prepared_batch;
    std::vector<std::vector<ExPolygons>> batch_fixed_masks;
    size_t                               batch_begin = 0, batch_end = 0, fixed_begin = 0;
    size_t                               max_batch_layers = 0, max_batch_input_points = 0, max_batch_retained_points = 0;
    for (size_t layer_id = 0; layer_id < print_object.layer_count(); ++layer_id) {
        throw_on_cancel();
        if (parallel_geometry && layer_id == batch_end) {
            PreparationTimer batch_timer(&prep_profile, "geometry_batch_wall");
            prepared_batch.clear();
            batch_fixed_masks.clear();
            const LayerGeometryBatch batch = next_layer_geometry_batch(layer_id, segmentation.size(), batch_layer_limit, batch_point_limit,
                                                                       [&](size_t i) {
                                                                           size_t points = 0;
                                                                           for (const ExPolygons& masks : segmentation[i])
                                                                               points += count_points(masks);
                                                                           for (const LayerRegion* region :
                                                                                print_object.get_layer(int(i))->regions())
                                                                               for (const Surface& surface : region->slices.surfaces)
                                                                                   points += count_points(surface.expolygon);
                                                                           return points;
                                                                       });
            batch_begin                    = batch.begin;
            batch_end                      = batch.end;
            // Neighbor halos are immutable and shared by all consumers in this
            // batch. Two small halo recomputations avoid cross-batch ownership.
            fixed_begin            = batch.neighbor_begin;
            const size_t fixed_end = batch.neighbor_end;
            batch_fixed_masks.resize(fixed_end - fixed_begin);
            tbb::parallel_for(tbb::blocked_range<size_t>(fixed_begin, fixed_end, 1), [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    throw_on_cancel();
                    batch_fixed_masks[i - fixed_begin] = collect_local_z_fixed_state_masks_by_extruder(segmentation[i], num_physical);
                }
            });
            prepared_batch.resize(batch_end - batch_begin);
            tbb::parallel_for(tbb::blocked_range<size_t>(batch_begin, batch_end, 1), [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i)
                    prepared_batch[i - batch_begin] = prepare_geometry(i, &batch_fixed_masks[i - fixed_begin]);
            });
            if (prep_profile.enabled) {
                size_t retained_points = 0;
                for (const auto& item : prepared_batch) {
                    retained_points += count_points(item.mixed_masks) + count_points(item.base_masks) + count_points(item.fixed_union);
                    for (const ExPolygons& masks : item.row_masks)
                        retained_points += count_points(masks);
                }
                for (const auto& layer_masks : batch_fixed_masks)
                    for (const ExPolygons& masks : layer_masks)
                        retained_points += count_points(masks);
                max_batch_layers          = std::max(max_batch_layers, prepared_batch.size());
                max_batch_input_points    = std::max(max_batch_input_points, batch.input_points);
                max_batch_retained_points = std::max(max_batch_retained_points, retained_points);
            }
        }
        if (reuse_fixed_masks && layer_id + 1 < segmentation.size())
            fixed_mask_window[(layer_id + 1) % 3] = collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id + 1], num_physical);
        const auto*            cached_fixed = parallel_geometry ? &batch_fixed_masks[layer_id - fixed_begin] :
                                              reuse_fixed_masks ? &fixed_mask_window[layer_id % 3] :
                                                                  nullptr;
        PreparedLocalZGeometry prepared     = parallel_geometry ? std::move(prepared_batch[layer_id - batch_begin]) :
                                                                  prepare_geometry(layer_id, cached_fixed);
        if (validate_geometry && (parallel_geometry || reuse_fixed_masks)) {
            const PreparedLocalZGeometry reference = prepare_geometry(layer_id, nullptr);
            if (!prepared.same_as(reference) || *cached_fixed != reference.owned_fixed_masks)
                throw std::runtime_error("Prepared Local-Z geometry changed ordered masks or active rows");
        }
        const Layer&   layer = *print_object.get_layer(int(layer_id));
        LocalZInterval interval;
        interval.layer_id               = size_t(layer.id());
        interval.z_lo                   = layer.print_z - layer.height;
        interval.z_hi                   = layer.print_z;
        interval.base_height            = layer.height;
        interval.sublayer_height        = layer.height;
        interval.first_sublayer_idx     = plans.size();
        const bool subdivide_this_layer = mixed_filament_local_z_should_subdivide_layer(layer_id, settings.local_z_whole_objects,
                                                                                        settings.local_z_preserve_first_layer);

        interval.has_mixed_paint              = prepared.has_mixed_paint;
        auto&        mixed_masks              = prepared.mixed_masks;
        auto&        base_masks               = prepared.base_masks;
        auto&        row_state_masks          = prepared.row_masks;
        auto&        row_state_ids            = prepared.row_ids;
        auto&        fixed_state_masks_union  = prepared.fixed_union;
        const auto&  row_active_this_layer    = prepared.row_active;
        double       shared_min_sublayer_height = 0.01;
        for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx)
            if (row_active_this_layer[row_idx])
                shared_min_sublayer_height = std::max(shared_min_sublayer_height, rows[row_idx].min_sublayer_height);
        const size_t dominant_mixed_idx       = prepared.dominant_mixed_idx;
        const size_t mixed_state_count        = prepared.mixed_state_count;
        const size_t active_mixed_definitions = size_t(std::count(row_active_this_layer.begin(), row_active_this_layer.end(), uint8_t(1)));
        double       dominant_gradient_h_a = 0.0, dominant_gradient_h_b = 0.0;
        bool         dominant_gradient_valid = false, dominant_is_gradient = false;
        PreparationTimer cadence_timer(&prep_profile, "cadence");
        for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
            if (row_active_this_layer[row_idx] == 0) {
                if (row_idx < rows.size())
                    rows[row_idx].state.last_gradient_extruder = 0;
                continue;
            }
            if (row_active_this_layer[row_idx] != 0 && rows[row_idx].state.was_active == 0) {
                if (row_idx < rows.size())
                    rows[row_idx].state.last_gradient_extruder = 0;
                if (rows[row_idx].uses_direct_solver != 0 &&
                    rows[row_idx].state.component_error.size() == rows[row_idx].direct_components.size()) {
                    std::fill(rows[row_idx].state.component_error.begin(), rows[row_idx].state.component_error.end(), 0.0);
                }
                const bool can_sync_to_dominant = settings.local_z_whole_objects && dominant_mixed_idx < rows.size() &&
                                                  dominant_mixed_idx != row_idx && row_active_this_layer[dominant_mixed_idx] != 0;
                if (can_sync_to_dominant) {
                    rows[row_idx].state.cadence_index    = rows[dominant_mixed_idx].state.cadence_index;
                    rows[row_idx].state.pair_cycle_index = rows[dominant_mixed_idx].state.pair_cycle_index;
                } else {
                    rows[row_idx].state.cadence_index    = 0;
                    rows[row_idx].state.pair_cycle_index = 0;
                }
            }
        }
        std::vector<LocalZActivePair> row_active_pairs(rows.size());
        for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
            if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_definition(rows[row_idx].definition))
                continue;
            if (rows[row_idx].uses_direct_solver != 0)
                continue;

            LocalZActivePair gradient_pair;
            if (effective_gradient_active_pair_for_definition(row_idx, layer_id, gradient_pair)) {
                row_active_pairs[row_idx] = gradient_pair;
                continue;
            }

            const int cadence_index   = rows[row_idx].uses_pair_cycle != 0 ? rows[row_idx].state.pair_cycle_index :
                                                                             rows[row_idx].state.cadence_index;
            row_active_pairs[row_idx] = derive_local_z_active_pair(rows[row_idx].definition, rows[row_idx].pair_cycle, num_physical,
                                                                   cadence_index);
        }
        if (dominant_mixed_idx < rows.size()) {
            const LocalZActivePair& dominant_pair          = row_active_pairs[dominant_mixed_idx];
            const int               dominant_mix_b_percent = dominant_pair.valid_pair(num_physical) ? dominant_pair.mix_b_percent :
                                                                                                      rows[dominant_mixed_idx].definition.mix_b_percent;
            if (rows[dominant_mixed_idx].uses_direct_solver == 0) {
                dominant_is_gradient = effective_gradient_heights_for_definition(dominant_mixed_idx, layer_id, interval.base_height,
                                                                                 dominant_gradient_h_a, dominant_gradient_h_b);
                if (!dominant_is_gradient)
                    compute_local_z_component_heights(dominant_mix_b_percent, interval.base_height, shared_min_sublayer_height,
                                                      dominant_gradient_h_a, dominant_gradient_h_b);
                dominant_gradient_valid = true;
            }
        }
        cadence_timer.finish();
        total_mixed_state_layers += prepared.mixed_state_count;
        if (interval.has_mixed_paint)
            ++mixed_intervals;
        const auto&                   fixed_state_masks_by_extruder      = cached_fixed ? *cached_fixed : prepared.owned_fixed_masks;
        const std::vector<ExPolygons> owned_prev                         = !cached_fixed && layer_id > 0 ?
                                                                               collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id - 1],
                                                                                                                             num_physical) :
                                                                               std::vector<ExPolygons>(num_physical);
        const std::vector<ExPolygons> owned_next                         = !cached_fixed && layer_id + 1 < segmentation.size() ?
                                                                               collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id + 1],
                                                                                                                             num_physical) :
                                                                               std::vector<ExPolygons>(num_physical);
        const auto&                   prev_fixed_state_masks_by_extruder = layer_id == 0 ? owned_prev :
                                                                           parallel_geometry ? batch_fixed_masks[layer_id - 1 - fixed_begin] :
                                                                           reuse_fixed_masks ? fixed_mask_window[(layer_id + 2) % 3] :
                                                                                               owned_prev;
        const auto&                   next_fixed_state_masks_by_extruder = layer_id + 1 == segmentation.size() ? owned_next :
                                                                           parallel_geometry ? batch_fixed_masks[layer_id + 1 - fixed_begin] :
                                                                           reuse_fixed_masks ? fixed_mask_window[(layer_id + 1) % 3] :
                                                                                               owned_next;
        if (validate_geometry && cached_fixed &&
            ((layer_id > 0 && prev_fixed_state_masks_by_extruder !=
                                  collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id - 1], num_physical)) ||
             (layer_id + 1 < segmentation.size() &&
              next_fixed_state_masks_by_extruder !=
                  collect_local_z_fixed_state_masks_by_extruder(segmentation[layer_id + 1], num_physical))))
            throw std::runtime_error("Prepared Local-Z neighbor masks changed ordered geometry");
        PreparationTimer emit_timer(&prep_profile, "plan_assembly");
        size_t           active_row_mask_components = 0;
        size_t           active_row_mask_vertices   = 0;
        for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx)
            if (row_active_this_layer[row_idx] != 0) {
                active_row_mask_components += row_state_masks[row_idx].size();
                for (const ExPolygon& expoly : row_state_masks[row_idx]) {
                    active_row_mask_vertices += expoly.contour.points.size();
                    for (const Polygon& hole : expoly.holes)
                        active_row_mask_vertices += hole.points.size();
                }
            }

        const bool independent_interval = interval.has_mixed_paint && independent_layers[layer_id] != 0;

        if (independent_interval) {
            interval.independent_layer_height = true;
            interval.managed_masks            = mixed_masks;
            ++split_intervals;

            std::vector<SubLayerPlan> independent_plans;
            independent_plans.reserve(std::max<size_t>(2, active_mixed_definitions * 2));
            for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                if (row_active_this_layer[row_idx] == 0 || row_state_masks[row_idx].empty())
                    continue;

                IndependentCadenceState&                        state          = rows[row_idx].state.independent;
                const bool                                      gradient_row   = row_idx < rows.size() && rows[row_idx].is_gradient != 0;
                const std::vector<LocalZIndependentDirectPass>& direct_cadence = rows[row_idx].direct_cadence;
                if (!state.active || rows[row_idx].state.was_active == 0 || state.z_cursor > interval.z_hi + EPSILON) {
                    state             = IndependentCadenceState{};
                    state.active      = true;
                    state.z_cursor    = interval.z_lo;
                    size_t last_layer = layer_id;
                    while (last_layer + 1 < print_object.layer_count() && independent_layers[last_layer + 1] != 0 &&
                           row_is_active_on_layer(row_idx, last_layer + 1))
                        ++last_layer;
                    state.z_end = print_object.get_layer(int(last_layer))->print_z;
                }

                auto append_independent_pass = [&](double z_hi, double flow_height, unsigned int target_extruder) {
                    if (flow_height <= EPSILON || target_extruder == 0 || target_extruder > num_physical)
                        return;

                    SubLayerPlan plan;
                    plan.layer_id         = interval.layer_id;
                    plan.pass_index       = independent_plans.size();
                    plan.split_interval   = true;
                    plan.z_lo             = z_hi - flow_height;
                    plan.z_hi             = z_hi;
                    plan.print_z          = z_hi;
                    plan.flow_height      = flow_height;
                    plan.dependency_group = row_idx + 1;
                    plan.dependency_order = state.emitted_passes++;
                    plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    append(plan.painted_masks_by_extruder[target_extruder - 1], row_state_masks[row_idx]);
                    independent_plans.emplace_back(std::move(plan));
                    ++split_passes_total;
                    ++split_passes_with_painted_masks;
                    ++forced_height_resolve_calls;
                };

                auto ensure_gradient_cadence = [&]() {
                    if (!gradient_row)
                        return false;
                    if (state.gradient_pass_index < state.gradient_cadence.size())
                        return true;

                    const auto [domain_lo, domain_hi]    = rows[row_idx].gradient_bounds;
                    const double remaining_domain_height = state.z_end - state.z_cursor;
                    if (remaining_domain_height <= EPSILON)
                        return false;

                    const double cycle_height = std::min<double>(settings.gradient_nominal_height, remaining_domain_height);
                    // Sample at the nominal cycle midpoint, then extend only the solid-zone
                    // component. Sampling does not depend on the extension it is deciding.
                    const double progress     = std::clamp((state.z_cursor + 0.5 * cycle_height - domain_lo) / (domain_hi - domain_lo), 0.0,
                                                           1.0);
                    const auto   sample       = sample_mixed_gradient_local_z(rows[row_idx].definition, num_physical, progress,
                                                                              settings.gradient_middle_window_fraction,
                                                                              settings.gradient_nominal_height, nozzle_min_layer_heights,
                                                                              nozzle_max_layer_heights);
                    state.gradient_cadence    = build_local_z_independent_gradient_cadence(sample, remaining_domain_height,
                                                                                           state.last_extruder, print_cfg);
                    state.gradient_pass_index = 0;
                    return !state.gradient_cadence.empty();
                };

                auto current_cadence_pass = [&]() -> const LocalZIndependentDirectPass* {
                    if (gradient_row) {
                        if (!ensure_gradient_cadence())
                            return nullptr;
                        return &state.gradient_cadence[state.gradient_pass_index];
                    }
                    if (direct_cadence.empty())
                        return nullptr;
                    return &direct_cadence[state.cadence_index % direct_cadence.size()];
                };

                while (true) {
                    const LocalZIndependentDirectPass* cadence_pass = current_cadence_pass();
                    if (cadence_pass == nullptr)
                        break;
                    const double remaining_height = state.z_end - state.z_cursor;
                    if (remaining_height <= EPSILON)
                        break;
                    // At a gradient cycle boundary the next sample may select another tool.
                    // Reserve enough height for that row; within a cadence use the actual next nozzle.
                    unsigned int next_extruder = 0;
                    if (!gradient_row)
                        next_extruder = direct_cadence[(state.cadence_index + 1) % direct_cadence.size()].extruder_id;
                    else if (state.gradient_pass_index + 1 < state.gradient_cadence.size())
                        next_extruder = state.gradient_cadence[state.gradient_pass_index + 1].extruder_id;
                    const double next_minimum = next_extruder > 0 ? nozzle_min_layer_heights[next_extruder - 1] :
                                                                    rows[row_idx].min_sublayer_height;
                    const double height       = local_z_bounded_pass_height(cadence_pass->height, remaining_height,
                                                                            nozzle_min_layer_heights[cadence_pass->extruder_id - 1],
                                                                            nozzle_max_layer_heights[cadence_pass->extruder_id - 1],
                                                                            next_minimum);
                    if (height <= 0.0)
                        throw SlicingError(
                            L("Local-Z cannot fit the nozzle layer height limits into this mixed region. Check the nozzle minimum "
                              "and maximum layer heights in the printer settings."));
                    const double next_z = state.z_cursor + height;
                    if (next_z > interval.z_hi + EPSILON)
                        break;

                    append_independent_pass(std::min(next_z, interval.z_hi), height, cadence_pass->extruder_id);
                    state.z_cursor      = next_z;
                    state.last_extruder = cadence_pass->extruder_id;
                    if (gradient_row)
                        ++state.gradient_pass_index;
                    else
                        ++state.cadence_index;
                    if (state.z_cursor >= interval.z_hi - EPSILON)
                        break;
                }

                if (interval.z_hi >= state.z_end - EPSILON)
                    state.active = false;
            }

            if (settings.local_z_whole_objects && !fixed_state_masks_union.empty()) {
                const size_t fixed_dependency_group = rows.size() + 1;
                for (size_t extruder_idx = 0; extruder_idx < fixed_state_masks_by_extruder.size(); ++extruder_idx) {
                    if (fixed_state_masks_by_extruder[extruder_idx].empty())
                        continue;
                    SubLayerPlan plan;
                    plan.layer_id         = interval.layer_id;
                    plan.pass_index       = independent_plans.size();
                    plan.split_interval   = true;
                    plan.z_lo             = interval.z_lo;
                    plan.z_hi             = interval.z_hi;
                    plan.print_z          = interval.z_hi;
                    plan.flow_height      = interval.base_height;
                    plan.dependency_group = fixed_dependency_group;
                    plan.dependency_order = extruder_idx;
                    plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    append(plan.fixed_painted_masks_by_extruder[extruder_idx], fixed_state_masks_by_extruder[extruder_idx]);
                    independent_plans.emplace_back(std::move(plan));
                    ++split_passes_total;
                    ++split_passes_with_painted_masks;
                }
            }

            std::sort(independent_plans.begin(), independent_plans.end(), [](const SubLayerPlan& lhs, const SubLayerPlan& rhs) {
                if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                    return lhs.print_z < rhs.print_z;
                if (lhs.dependency_group != rhs.dependency_group)
                    return lhs.dependency_group < rhs.dependency_group;
                return lhs.dependency_order < rhs.dependency_order;
            });

            double min_pass_height = interval.base_height;
            double max_pass_height = 0.0;
            for (size_t pass_idx = 0; pass_idx < independent_plans.size(); ++pass_idx) {
                SubLayerPlan& plan = independent_plans[pass_idx];
                plan.pass_index    = pass_idx;
                min_pass_height    = std::min(min_pass_height, plan.flow_height);
                max_pass_height    = std::max(max_pass_height, plan.flow_height);
                if (pass_idx + 1 == independent_plans.size())
                    diagnostics.record(plan, base_masks);
                plans.emplace_back(std::move(plan));
                ++interval.sublayer_count;
                ++total_generated_sublayer_cnt;
            }
            interval.sublayer_height = independent_plans.empty() ? interval.base_height : min_pass_height;

            BOOST_LOG_TRIVIAL(debug) << "Local-Z independent interval"
                                     << " object=" << object_name << " layer_id=" << layer_id << " base_height=" << interval.base_height
                                     << " pass_count=" << interval.sublayer_count
                                     << " pass_min_height=" << (interval.sublayer_count == 0 ? 0.0 : min_pass_height)
                                     << " pass_max_height=" << max_pass_height << " active_mixed_definitions=" << active_mixed_definitions
                                     << " managed_mask_count=" << interval.managed_masks.size();

            for (size_t i = 0; i < rows.size(); ++i)
                rows[i].state.was_active = row_active_this_layer[i];
            intervals.emplace_back(std::move(interval));
            continue;
        }

        for (auto& row : rows)
            row.state.independent = IndependentCadenceState{};

        std::vector<std::vector<double>> isolated_row_pass_heights(rows.size());
        bool                             isolated_multi_row_mode = false;
        if (interval.has_mixed_paint && subdivide_this_layer && settings.preferred_a <= EPSILON && settings.preferred_b <= EPSILON &&
            active_mixed_definitions > 1) {
            size_t isolated_rows_with_split = 0;
            for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                if (row_active_this_layer[row_idx] == 0)
                    continue;

                std::vector<double> row_passes;
                if (rows[row_idx].uses_direct_solver != 0) {
                    row_passes = build_direct_pass_heights_for_definition(row_idx, layer_id, interval.base_height);
                } else {
                    double                  row_h_a           = 0.0;
                    double                  row_h_b           = 0.0;
                    const LocalZActivePair& active_pair       = row_active_pairs[row_idx];
                    const int               row_mix_b_percent = active_pair.valid_pair(num_physical) ? active_pair.mix_b_percent :
                                                                                                       rows[row_idx].definition.mix_b_percent;
                    const bool row_is_gradient = effective_gradient_heights_for_definition(row_idx, layer_id, interval.base_height, row_h_a,
                                                                                           row_h_b);
                    if (!row_is_gradient)
                        compute_local_z_component_heights(row_mix_b_percent, interval.base_height, rows[row_idx].min_sublayer_height,
                                                          row_h_a, row_h_b);
                    if (row_is_gradient)
                        std::swap(row_h_a, row_h_b);
                    row_passes = (row_is_gradient || active_pair.uses_layer_cycle_sequence) ?
                                     build_local_z_two_pass_heights(interval.base_height, rows[row_idx].min_sublayer_height, row_h_a,
                                                                    row_h_b) :
                                     build_local_z_alternating_pass_heights(interval.base_height, rows[row_idx].min_sublayer_height,
                                                                            row_h_a, row_h_b);
                }
                if (row_passes.empty())
                    row_passes.emplace_back(interval.base_height);
                if (!sanitize_local_z_pass_heights(row_passes, interval.base_height, rows[row_idx].min_sublayer_height))
                    row_passes = build_uniform_local_z_pass_heights(interval.base_height, rows[row_idx].min_sublayer_height);
                if (row_passes.size() > 1)
                    ++isolated_rows_with_split;
                isolated_row_pass_heights[row_idx] = std::move(row_passes);
            }
            if (isolated_rows_with_split > 0) {
                isolated_multi_row_mode = true;
                ++alternating_height_intervals;
            }
        }

        std::vector<double> pass_heights;
        if (interval.has_mixed_paint && subdivide_this_layer && !isolated_multi_row_mode) {
            // Local-Z mode emits an A/B pair for mixed regions and derives its
            // direct thickness ratio from the nominal layer height.
            if (settings.preferred_a <= EPSILON && settings.preferred_b <= EPSILON) {
                if (dominant_mixed_idx < rows.size() && rows[dominant_mixed_idx].uses_direct_solver != 0) {
                    pass_heights = build_direct_pass_heights_for_definition(dominant_mixed_idx, layer_id, interval.base_height);
                    if (pass_heights.size() > 1)
                        ++alternating_height_intervals;
                } else if (dominant_gradient_valid) {
                    if (dominant_is_gradient) {
                        pass_heights = build_local_z_two_pass_heights(interval.base_height, shared_min_sublayer_height,
                                                                      dominant_gradient_h_b, dominant_gradient_h_a);
                    } else {
                        const bool dominant_uses_pair_cycle = dominant_mixed_idx < rows.size() &&
                                                              row_active_pairs[dominant_mixed_idx].uses_layer_cycle_sequence;
                        pass_heights = dominant_uses_pair_cycle ?
                                           build_local_z_two_pass_heights(interval.base_height, shared_min_sublayer_height,
                                                                          dominant_gradient_h_a, dominant_gradient_h_b) :
                                           build_local_z_alternating_pass_heights(interval.base_height, shared_min_sublayer_height,
                                                                                  dominant_gradient_h_a, dominant_gradient_h_b);
                    }
                    if (pass_heights.size() > 1)
                        ++alternating_height_intervals;
                } else {
                    pass_heights = build_uniform_local_z_pass_heights(interval.base_height, shared_min_sublayer_height);
                }
            } else {
                pass_heights = build_local_z_pass_heights(interval.base_height, shared_min_sublayer_height, settings.preferred_a,
                                                          settings.preferred_b);
            }
        } else
            pass_heights.emplace_back(interval.base_height);

        if (interval.has_mixed_paint) {
            if (!sanitize_local_z_pass_heights(pass_heights, interval.base_height, shared_min_sublayer_height))
                pass_heights = build_uniform_local_z_pass_heights(interval.base_height, shared_min_sublayer_height);
        }

        // Ordinary two-color rows keep the planner's A/B order, just like isolated
        // rows. Sorting only single-row layers by height reverses their cadence
        // when a neighboring mixed region starts or ends.
        // Retain the existing height ordering for multicolor and pair-cycle rows.
        const bool uses_multicolor_order = dominant_mixed_idx < rows.size() &&
                                           (rows[dominant_mixed_idx].uses_direct_solver != 0 ||
                                            row_active_pairs[dominant_mixed_idx].uses_layer_cycle_sequence);
        if (uses_multicolor_order && !dominant_is_gradient && interval.has_mixed_paint && settings.preferred_a <= EPSILON &&
            settings.preferred_b <= EPSILON && pass_heights.size() == 2 && pass_heights[0] > pass_heights[1]) {
            std::swap(pass_heights[0], pass_heights[1]);
        }

        size_t pass_count_for_log      = pass_heights.size();
        double pass_min_height_for_log = pass_heights.empty() ? 0.0 : *std::min_element(pass_heights.begin(), pass_heights.end());
        double pass_max_height_for_log = pass_heights.empty() ? 0.0 : *std::max_element(pass_heights.begin(), pass_heights.end());

        const bool split_interval                 = interval.has_mixed_paint && (isolated_multi_row_mode || pass_heights.size() > 1);
        const bool force_height_resolve           = true;
        auto       build_whole_object_fixed_plans = [&](size_t first_pass_index) {
            std::vector<SubLayerPlan> fixed_plans;
            if (!settings.local_z_whole_objects || !subdivide_this_layer || fixed_state_masks_union.empty() ||
                interval.base_height <= EPSILON)
                return fixed_plans;

            const std::vector<double> fixed_z_cuts{interval.z_lo, interval.z_lo + 0.5 * interval.base_height, interval.z_hi};
            const size_t              fixed_pass_count       = fixed_z_cuts.size() - 1;
            const size_t              fixed_dependency_group = rows.size() + 1;
            for (size_t fixed_pass_idx = 0; fixed_pass_idx < fixed_pass_count; ++fixed_pass_idx) {
                const double z_lo        = fixed_z_cuts[fixed_pass_idx];
                const double z_hi        = fixed_z_cuts[fixed_pass_idx + 1];
                const double pass_height = z_hi - z_lo;
                if (pass_height <= EPSILON)
                    continue;

                const std::vector<ExPolygons> fixed_masks_for_pass =
                    build_local_z_transition_fixed_masks_for_pass(fixed_state_masks_by_extruder, prev_fixed_state_masks_by_extruder,
                                                                        next_fixed_state_masks_by_extruder, fixed_pass_idx, fixed_pass_count);

                SubLayerPlan fixed_plan;
                fixed_plan.layer_id         = interval.layer_id;
                fixed_plan.pass_index       = first_pass_index + fixed_plans.size();
                fixed_plan.split_interval   = true;
                fixed_plan.z_lo             = z_lo;
                fixed_plan.z_hi             = z_hi;
                fixed_plan.print_z          = z_hi;
                fixed_plan.flow_height      = pass_height;
                fixed_plan.dependency_group = fixed_dependency_group;
                fixed_plan.dependency_order = fixed_pass_idx;
                fixed_plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                fixed_plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());

                bool plan_has_fixed_masks = false;
                for (size_t extruder_idx = 0;
                     extruder_idx < fixed_masks_for_pass.size() && extruder_idx < fixed_plan.fixed_painted_masks_by_extruder.size();
                     ++extruder_idx) {
                    if (fixed_masks_for_pass[extruder_idx].empty())
                        continue;
                    append(fixed_plan.fixed_painted_masks_by_extruder[extruder_idx], fixed_masks_for_pass[extruder_idx]);
                    plan_has_fixed_masks = true;
                }
                if (!plan_has_fixed_masks)
                    continue;

                for (ExPolygons& masks : fixed_plan.fixed_painted_masks_by_extruder)
                    if (masks.size() > 1)
                        masks = union_ex(masks);
                fixed_plans.emplace_back(std::move(fixed_plan));
            }
            return fixed_plans;
        };
        if (split_interval) {
            ++split_intervals;
            bool interval_has_split_painted_masks = false;
            if (isolated_multi_row_mode) {
                std::vector<SubLayerPlan> isolated_plans;
                isolated_plans.reserve(std::max<size_t>(2, active_mixed_definitions * 2));

                for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                    if (row_active_this_layer[row_idx] == 0)
                        continue;
                    const ExPolygons& state_masks = row_state_masks[row_idx];
                    if (state_masks.empty())
                        continue;

                    const std::vector<double>& row_passes_raw         = isolated_row_pass_heights[row_idx];
                    const std::vector<double>  row_passes             = row_passes_raw.empty() ? std::vector<double>{interval.base_height} :
                                                                                                 row_passes_raw;
                    const LocalZActivePair&    active_pair            = row_active_pairs[row_idx];
                    const bool                 uses_direct_multicolor = rows[row_idx].uses_direct_solver != 0;
                    const bool                 valid_pair             = active_pair.valid_pair(num_physical);
                    const int orientation_cadence_index = active_pair.uses_layer_cycle_sequence ? rows[row_idx].state.pair_cycle_index :
                                                                                                  rows[row_idx].state.cadence_index;
                    const std::vector<unsigned int> direct_sequence = uses_direct_multicolor ?
                                                                          build_direct_sequence_for_definition(row_idx, layer_id,
                                                                                                               row_passes) :
                                                                          std::vector<unsigned int>();

                    bool start_with_a = true;
                    if (!uses_direct_multicolor && valid_pair && settings.preferred_a <= EPSILON && settings.preferred_b <= EPSILON) {
                        double     row_h_a         = 0.0;
                        double     row_h_b         = 0.0;
                        const bool row_is_gradient = effective_gradient_heights_for_definition(row_idx, layer_id, interval.base_height,
                                                                                               row_h_a, row_h_b);
                        if (!row_is_gradient)
                            compute_local_z_component_heights(active_pair.mix_b_percent, interval.base_height, shared_min_sublayer_height,
                                                              row_h_a, row_h_b);
                        start_with_a = row_is_gradient ?
                                           false :
                                           choose_local_z_start_with_component_a(row_passes, row_h_a, row_h_b, orientation_cadence_index);
                    }

                    double z_cursor             = interval.z_lo;
                    bool   row_used             = false;
                    size_t row_dependency_order = 0;
                    for (size_t pass_i = 0; pass_i < row_passes.size(); ++pass_i) {
                        if (z_cursor >= interval.z_hi - EPSILON)
                            break;

                        const double pass_height = std::min<double>(row_passes[pass_i], interval.z_hi - z_cursor);
                        if (pass_height <= EPSILON)
                            continue;
                        const double z_next = std::min<double>(interval.z_hi, z_cursor + pass_height);

                        SubLayerPlan plan;
                        plan.layer_id         = interval.layer_id;
                        plan.pass_index       = isolated_plans.size();
                        plan.split_interval   = true;
                        plan.z_lo             = z_cursor;
                        plan.z_hi             = z_next;
                        plan.print_z          = z_next;
                        plan.flow_height      = pass_height;
                        plan.dependency_group = row_idx + 1;
                        plan.dependency_order = row_dependency_order++;
                        plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                        plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                        ++split_passes_total;
                        ++forced_height_resolve_calls;

                        unsigned int target_extruder = 0;
                        if (uses_direct_multicolor) {
                            if (pass_i < direct_sequence.size())
                                target_extruder = direct_sequence[pass_i];
                        } else if (valid_pair) {
                            const bool even_pass = (pass_i % 2) == 0;
                            target_extruder      = even_pass ? (start_with_a ? active_pair.component_a : active_pair.component_b) :
                                                               (start_with_a ? active_pair.component_b : active_pair.component_a);
                            ++strict_ab_assignments;
                        }
                        if (target_extruder == 0) {
                            const unsigned int state_id = row_state_ids[row_idx];
                            if (state_id != 0) {
                                const int resolve_cadence_index = active_pair.uses_layer_cycle_sequence ?
                                                                      rows[row_idx].state.pair_cycle_index :
                                                                      rows[row_idx].state.cadence_index;
                                target_extruder = mixed_mgr.resolve(state_id, num_physical, resolve_cadence_index, float(plan.print_z),
                                                                    float(plan.flow_height), force_height_resolve);
                            }
                        }
                        if (target_extruder == 0 || target_extruder > num_physical) {
                            ++forced_height_resolve_invalid_target;
                        } else {
                            append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
                            if (!uses_direct_multicolor && row_idx < rows.size() && rows[row_idx].is_gradient != 0)
                                rows[row_idx].state.last_gradient_extruder = target_extruder;
                            ++split_passes_with_painted_masks;
                            interval_has_split_painted_masks = true;
                        }
                        for (ExPolygons& masks : plan.painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);

                        isolated_plans.emplace_back(std::move(plan));
                        row_used = true;
                        if (!uses_direct_multicolor && !active_pair.uses_layer_cycle_sequence)
                            ++rows[row_idx].state.cadence_index;
                        z_cursor = z_next;
                    }
                    if (row_used && !uses_direct_multicolor && active_pair.uses_layer_cycle_sequence)
                        ++rows[row_idx].state.pair_cycle_index;
                }

                if (!isolated_plans.empty()) {
                    auto sort_local_z_plans = [](std::vector<SubLayerPlan>& plans) {
                        std::sort(plans.begin(), plans.end(), [](const SubLayerPlan& lhs, const SubLayerPlan& rhs) {
                            if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                                return lhs.print_z < rhs.print_z;
                            if (std::abs(lhs.z_lo - rhs.z_lo) > EPSILON)
                                return lhs.z_lo < rhs.z_lo;
                            return lhs.pass_index < rhs.pass_index;
                        });
                    };

                    sort_local_z_plans(isolated_plans);

                    std::vector<SubLayerPlan> fixed_plans = build_whole_object_fixed_plans(isolated_plans.size());
                    if (!fixed_plans.empty()) {
                        isolated_plans.insert(isolated_plans.end(), std::make_move_iterator(fixed_plans.begin()),
                                              std::make_move_iterator(fixed_plans.end()));
                        sort_local_z_plans(isolated_plans);
                    }

                    double min_flow_height = isolated_plans.front().flow_height;
                    double max_flow_height = isolated_plans.front().flow_height;
                    for (size_t idx = 0; idx < isolated_plans.size(); ++idx) {
                        isolated_plans[idx].pass_index = idx;
                        min_flow_height                = std::min(min_flow_height, isolated_plans[idx].flow_height);
                        max_flow_height                = std::max(max_flow_height, isolated_plans[idx].flow_height);
                        for (ExPolygons& masks : isolated_plans[idx].painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);
                        for (ExPolygons& masks : isolated_plans[idx].fixed_painted_masks_by_extruder)
                            if (masks.size() > 1)
                                masks = union_ex(masks);
                        if (std::any_of(isolated_plans[idx].fixed_painted_masks_by_extruder.begin(),
                                        isolated_plans[idx].fixed_painted_masks_by_extruder.end(),
                                        [](const ExPolygons& masks) { return !masks.empty(); })) {
                            ++split_passes_with_painted_masks;
                            interval_has_split_painted_masks = true;
                        }
                    }
                    diagnostics.record(isolated_plans.back(), base_masks);
                    interval.sublayer_height = min_flow_height;
                    pass_count_for_log       = isolated_plans.size();
                    pass_min_height_for_log  = min_flow_height;
                    pass_max_height_for_log  = max_flow_height;
                    for (SubLayerPlan& plan : isolated_plans) {
                        plans.emplace_back(std::move(plan));
                        ++interval.sublayer_count;
                        ++total_generated_sublayer_cnt;
                    }
                }
            } else {
                // Derive per-row orientation against pass heights so each mixed row
                // maps thicker/thinner subpasses to the intended component.
                std::vector<uint8_t>                   start_with_component_a(rows.size(), uint8_t(1));
                std::vector<std::vector<unsigned int>> row_direct_pass_sequences(rows.size());
                size_t                                 single_dependency_group = 0;
                size_t                                 active_dependency_rows  = 0;
                for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                    if (row_state_masks[row_idx].empty() || row_state_ids[row_idx] == 0)
                        continue;
                    ++active_dependency_rows;
                    single_dependency_group = row_idx + 1;
                }
                if (active_dependency_rows != 1)
                    single_dependency_group = 0;
                if (settings.preferred_a <= EPSILON && settings.preferred_b <= EPSILON) {
                    for (size_t row_idx = 0; row_idx < row_active_this_layer.size(); ++row_idx) {
                        if (row_active_this_layer[row_idx] == 0 || !local_z_eligible_mixed_definition(rows[row_idx].definition))
                            continue;
                        if (rows[row_idx].uses_direct_solver != 0) {
                            row_direct_pass_sequences[row_idx] = build_direct_sequence_for_definition(row_idx, layer_id, pass_heights);
                            continue;
                        }

                        const LocalZActivePair& active_pair = row_active_pairs[row_idx];
                        if (!active_pair.valid_pair(num_physical))
                            continue;

                        double    row_h_a                   = 0.0;
                        double    row_h_b                   = 0.0;
                        const int orientation_cadence_index = active_pair.uses_layer_cycle_sequence ? rows[row_idx].state.pair_cycle_index :
                                                                                                      rows[row_idx].state.cadence_index;
                        const bool row_is_gradient = effective_gradient_heights_for_definition(row_idx, layer_id, interval.base_height,
                                                                                               row_h_a, row_h_b);
                        if (!row_is_gradient)
                            compute_local_z_component_heights(active_pair.mix_b_percent, interval.base_height, shared_min_sublayer_height,
                                                              row_h_a, row_h_b);
                        start_with_component_a[row_idx] = row_is_gradient ?
                                                              uint8_t(0) :
                                                              (choose_local_z_start_with_component_a(pass_heights, row_h_a, row_h_b,
                                                                                                     orientation_cadence_index) ?
                                                                   uint8_t(1) :
                                                                   uint8_t(0));
                    }
                }

                double z_cursor          = interval.z_lo;
                size_t pass_idx          = 0;
                interval.sublayer_height = *std::min_element(pass_heights.begin(), pass_heights.end());
                std::vector<uint8_t> row_seen_sequence_in_interval(rows.size(), uint8_t(0));
                for (const double pass_height_nominal : pass_heights) {
                    if (z_cursor >= interval.z_hi - EPSILON)
                        break;
                    const double pass_height = std::min<double>(pass_height_nominal, interval.z_hi - z_cursor);
                    const double z_next      = std::min<double>(interval.z_hi, z_cursor + pass_height);

                    SubLayerPlan plan;
                    plan.layer_id         = interval.layer_id;
                    plan.pass_index       = pass_idx;
                    plan.split_interval   = true;
                    plan.z_lo             = z_cursor;
                    plan.z_hi             = z_next;
                    plan.print_z          = z_next;
                    plan.flow_height      = pass_height;
                    plan.dependency_group = single_dependency_group;
                    plan.dependency_order = pass_idx;
                    plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
                    ++split_passes_total;
                    bool                 pass_has_painted_masks = false;
                    std::vector<uint8_t> row_seen_in_pass(rows.size(), uint8_t(0));

                    for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                        const ExPolygons& state_masks = row_state_masks[row_idx];
                        if (state_masks.empty())
                            continue;

                        const unsigned int state_id = row_state_ids[row_idx];
                        if (state_id == 0)
                            continue;
                        const MixedFilament& definition = rows[row_idx].definition;
                        if (!local_z_eligible_mixed_definition(definition))
                            continue;
                        const LocalZActivePair& active_pair            = row_active_pairs[row_idx];
                        const bool              uses_direct_multicolor = rows[row_idx].uses_direct_solver != 0;
                        row_seen_in_pass[row_idx]                      = uint8_t(1);
                        if (!uses_direct_multicolor && active_pair.uses_layer_cycle_sequence)
                            row_seen_sequence_in_interval[row_idx] = uint8_t(1);
                        ++forced_height_resolve_calls;
                        unsigned int target_extruder = 0;
                        if (uses_direct_multicolor) {
                            if (pass_idx < row_direct_pass_sequences[row_idx].size())
                                target_extruder = row_direct_pass_sequences[row_idx][pass_idx];
                        } else if (active_pair.valid_pair(num_physical)) {
                            const bool start_a   = start_with_component_a[row_idx] != 0;
                            const bool even_pass = (pass_idx % 2) == 0;
                            // Local-Z mode alternates A/B on every subpass.
                            target_extruder = even_pass ? (start_a ? active_pair.component_a : active_pair.component_b) :
                                                          (start_a ? active_pair.component_b : active_pair.component_a);
                            ++strict_ab_assignments;
                        }
                        if (target_extruder == 0) {
                            const int resolve_cadence_index = active_pair.uses_layer_cycle_sequence ? rows[row_idx].state.pair_cycle_index :
                                                                                                      rows[row_idx].state.cadence_index;
                            target_extruder = mixed_mgr.resolve(state_id, num_physical, resolve_cadence_index, float(plan.print_z),
                                                                float(plan.flow_height), force_height_resolve);
                        }
                        if (target_extruder == 0 || target_extruder > num_physical) {
                            ++forced_height_resolve_invalid_target;
                            continue;
                        }
                        append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
                        if (!uses_direct_multicolor && row_idx < rows.size() && rows[row_idx].is_gradient != 0)
                            rows[row_idx].state.last_gradient_extruder = target_extruder;
                        pass_has_painted_masks = true;
                    }
                    for (ExPolygons& masks : plan.painted_masks_by_extruder)
                        if (masks.size() > 1)
                            masks = union_ex(masks);
                    for (ExPolygons& masks : plan.fixed_painted_masks_by_extruder)
                        if (masks.size() > 1)
                            masks = union_ex(masks);
                    if (pass_has_painted_masks) {
                        ++split_passes_with_painted_masks;
                        interval_has_split_painted_masks = true;
                    }

                    if (z_next >= interval.z_hi - EPSILON)
                        diagnostics.record(plan, base_masks);

                    plans.emplace_back(std::move(plan));
                    ++interval.sublayer_count;
                    ++total_generated_sublayer_cnt;
                    ++pass_idx;
                    for (size_t mixed_idx = 0; mixed_idx < row_seen_in_pass.size(); ++mixed_idx)
                        if (row_seen_in_pass[mixed_idx] != 0 && rows[mixed_idx].uses_pair_cycle == 0 &&
                            rows[mixed_idx].uses_direct_solver == 0)
                            ++rows[mixed_idx].state.cadence_index;
                    z_cursor = z_next;
                }
                std::vector<SubLayerPlan> fixed_plans = build_whole_object_fixed_plans(pass_idx);
                for (SubLayerPlan& fixed_plan : fixed_plans) {
                    interval.sublayer_height = std::min(interval.sublayer_height, fixed_plan.flow_height);
                    plans.emplace_back(std::move(fixed_plan));
                    ++interval.sublayer_count;
                    ++total_generated_sublayer_cnt;
                    ++split_passes_with_painted_masks;
                    interval_has_split_painted_masks = true;
                }
                for (size_t row_idx = 0; row_idx < row_seen_sequence_in_interval.size(); ++row_idx)
                    if (row_seen_sequence_in_interval[row_idx] != 0 && rows[row_idx].uses_direct_solver == 0)
                        ++rows[row_idx].state.pair_cycle_index;
            }
            if (!interval_has_split_painted_masks)
                ++split_intervals_without_painted_masks;
        } else {
            if (interval.has_mixed_paint)
                ++non_split_mixed_intervals;
            SubLayerPlan plan;
            plan.layer_id         = interval.layer_id;
            plan.pass_index       = 0;
            plan.split_interval   = false;
            plan.z_lo             = interval.z_lo;
            plan.z_hi             = interval.z_hi;
            plan.print_z          = interval.z_hi;
            plan.flow_height      = interval.base_height;
            plan.dependency_order = 0;
            diagnostics.record(plan, base_masks);
            plan.painted_masks_by_extruder.assign(num_physical, ExPolygons());
            plan.fixed_painted_masks_by_extruder.assign(num_physical, ExPolygons());
            std::vector<uint8_t> row_seen_in_interval(rows.size(), uint8_t(0));

            for (size_t row_idx = 0; row_idx < row_state_masks.size(); ++row_idx) {
                const ExPolygons& state_masks = row_state_masks[row_idx];
                if (state_masks.empty())
                    continue;

                const unsigned int state_id = row_state_ids[row_idx];
                if (state_id == 0)
                    continue;
                const MixedFilament& definition = rows[row_idx].definition;
                if (!local_z_eligible_mixed_definition(definition))
                    continue;
                row_seen_in_interval[row_idx] = uint8_t(1);
                ++forced_height_resolve_calls;
                unsigned int target_extruder = 0;
                if (rows[row_idx].uses_direct_solver != 0) {
                    const std::vector<unsigned int> direct_sequence = build_direct_sequence_for_definition(row_idx, layer_id,
                                                                                                           std::vector<double>{
                                                                                                               interval.base_height});
                    if (!direct_sequence.empty())
                        target_extruder = direct_sequence.front();
                } else {
                    const int resolve_cadence_index = rows[row_idx].uses_pair_cycle != 0 ? rows[row_idx].state.pair_cycle_index :
                                                                                           rows[row_idx].state.cadence_index;
                    target_extruder                 = mixed_mgr.resolve(state_id, num_physical, resolve_cadence_index, float(plan.print_z),
                                                                        float(plan.flow_height), force_height_resolve);
                }
                if (target_extruder == 0 || target_extruder > num_physical) {
                    ++forced_height_resolve_invalid_target;
                    continue;
                }
                append(plan.painted_masks_by_extruder[target_extruder - 1], state_masks);
                if (row_idx < rows.size() && rows[row_idx].is_gradient != 0)
                    rows[row_idx].state.last_gradient_extruder = target_extruder;
            }
            for (size_t extruder_idx = 0; extruder_idx < fixed_state_masks_by_extruder.size(); ++extruder_idx)
                if (!fixed_state_masks_by_extruder[extruder_idx].empty())
                    append(plan.fixed_painted_masks_by_extruder[extruder_idx], fixed_state_masks_by_extruder[extruder_idx]);
            for (ExPolygons& masks : plan.painted_masks_by_extruder)
                if (masks.size() > 1)
                    masks = union_ex(masks);
            for (ExPolygons& masks : plan.fixed_painted_masks_by_extruder)
                if (masks.size() > 1)
                    masks = union_ex(masks);

            plans.emplace_back(std::move(plan));
            interval.sublayer_count = 1;
            ++total_generated_sublayer_cnt;
            for (size_t mixed_idx = 0; mixed_idx < row_seen_in_interval.size(); ++mixed_idx)
                if (row_seen_in_interval[mixed_idx] != 0)
                    if (rows[mixed_idx].uses_direct_solver == 0)
                        (rows[mixed_idx].uses_pair_cycle != 0 ? rows[mixed_idx].state.pair_cycle_index :
                                                                rows[mixed_idx].state.cadence_index)++;
        }

        if (interval.has_mixed_paint) {
            BOOST_LOG_TRIVIAL(debug) << "Local-Z interval"
                                     << " object=" << object_name << " layer_id=" << layer_id << " base_height=" << interval.base_height
                                     << " subdivision_allowed=" << (subdivide_this_layer ? 1 : 0) << " split=" << split_interval
                                     << " isolated_multi_row_mode=" << (isolated_multi_row_mode ? 1 : 0)
                                     << " active_mixed_definitions=" << active_mixed_definitions
                                     << " active_row_mask_components=" << active_row_mask_components
                                     << " active_row_mask_vertices=" << active_row_mask_vertices << " mixed_states=" << mixed_state_count
                                     << " pass_count=" << pass_count_for_log << " pass_min_height=" << pass_min_height_for_log
                                     << " pass_max_height=" << pass_max_height_for_log << " mixed_mask_count=" << mixed_masks.size()
                                     << " base_mask_count=" << base_masks.size();
        }

        for (size_t i = 0; i < rows.size(); ++i)
            rows[i].state.was_active = row_active_this_layer[i];
        intervals.emplace_back(std::move(interval));
    }

    if (!intervals.empty() && !plans.empty()) {
        if (prep_profile.enabled && parallel_geometry)
            BOOST_LOG_TRIVIAL(info) << "Preparation Local-Z batches max_layers=" << max_batch_layers
                                    << " max_input_points=" << max_batch_input_points
                                    << " max_retained_points=" << max_batch_retained_points
                                    << " retained_vertex_bytes=" << max_batch_retained_points * sizeof(Point);
        print_object.set_local_z_plan(std::move(intervals), std::move(plans));
        export_local_z_plan_debug(print_object, nozzle_min_layer_heights, diagnostics);
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan built"
                                   << " object=" << object_name << " mixed_intervals=" << mixed_intervals
                                   << " split_intervals=" << split_intervals << " non_split_mixed_intervals=" << non_split_mixed_intervals
                                   << " split_intervals_without_painted_masks=" << split_intervals_without_painted_masks
                                   << " sublayer_passes=" << total_generated_sublayer_cnt << " split_passes_total=" << split_passes_total
                                   << " split_passes_with_painted_masks=" << split_passes_with_painted_masks
                                   << " alternating_height_intervals=" << alternating_height_intervals
                                   << " strict_ab_assignments=" << strict_ab_assignments
                                   << " mixed_state_layers=" << total_mixed_state_layers
                                   << " forced_height_resolve_calls=" << forced_height_resolve_calls
                                   << " forced_height_resolve_invalid_target=" << forced_height_resolve_invalid_target
                                   << " nozzle_height_limits=1" << " preferred_a=" << settings.preferred_a
                                   << " preferred_b=" << settings.preferred_b;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z plan empty after build"
                                   << " object=" << object_name << " intervals=" << intervals.size() << " plans=" << plans.size()
                                   << " mixed_intervals=" << mixed_intervals;
    }
}

void finalize_local_z_plan(PrintObject& print_object, LocalZPlanningInput input, const std::function<void()>& throw_on_cancel)
{
    const Print* print = print_object.print();
    if (input.segmentation.empty()) {
        input.segmentation              = whole_object_local_z_segmentation_by_mixed_wall(print_object, local_z_enabled_mixed_rows(*print));
        input.whole_object_scope_active = true;
    } else {
        // Slicing repair may discard empty layers from the bottom and renumber
        // the remaining Layer IDs. Keep the captured paint channels aligned.
        if (input.segmentation.size() > print_object.layer_count())
            input.segmentation.erase(input.segmentation.begin(),
                                     input.segmentation.begin() + (input.segmentation.size() - print_object.layer_count()));
        for (size_t i = 0; i < input.segmentation.size(); ++i) {
            throw_on_cancel();
            const ExPolygons domain = collect_layer_region_slices(*print_object.get_layer(int(i)));
            for (ExPolygons& masks : input.segmentation[i])
                if (!masks.empty())
                    masks = intersection_ex(masks, domain);
        }
    }
    if (!input.segmentation.empty())
        build_local_z_plan(print_object, input.segmentation, input.whole_object_scope_active, throw_on_cancel);
}

} // namespace Slic3r
