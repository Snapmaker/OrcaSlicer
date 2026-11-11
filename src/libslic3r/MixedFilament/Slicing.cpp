#include "Slicing.hpp"
#include "../PrintConfig.hpp"
#include "../Layer.hpp"
#include "../ClipperUtils.hpp"
#include <numeric>
#include <cmath>
#include <cstdlib>
namespace Slic3r { namespace MixedFilamentSlicing {
bool bool_from_full_config(const DynamicPrintConfig& full_cfg, const char* key, bool fallback)
{
    if (!full_cfg.has(key))
        return fallback;
    if (const ConfigOptionBool* opt = full_cfg.option<ConfigOptionBool>(key))
        return opt->value;
    if (const ConfigOptionInt* opt = full_cfg.option<ConfigOptionInt>(key))
        return opt->value != 0;
    return fallback;
}

coordf_t float_from_full_config(const DynamicPrintConfig& full_cfg, const char* key, coordf_t fallback)
{
    if (!full_cfg.has(key))
        return fallback;
    if (const ConfigOptionFloat* opt = full_cfg.option<ConfigOptionFloat>(key))
        return coordf_t(opt->value);
    return coordf_t(full_cfg.opt_float(key));
}

unsigned int segmentation_channel_filament_id(size_t channel_idx)
{
    // MM segmentation reserves channel 0 for the parent/default region.
    // All remaining channels already line up with the 1-based filament IDs.
    return unsigned(channel_idx);
}

std::vector<unsigned int> decode_gradient_component_ids(const MixedFilament& mf, size_t num_physical)
{
    return MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, num_physical);
}

std::vector<int> decode_gradient_component_weights(const MixedFilament& mf, size_t expected_components)
{
    std::vector<int> out;
    if (mf.gradient_component_weights.empty() || expected_components == 0)
        return out;

    std::string token;
    for (const char c : mf.gradient_component_weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (out.size() != expected_components)
        return {};

    int sum = 0;
    for (const int v : out)
        sum += std::max(0, v);
    if (sum <= 0)
        return {};
    return out;
}

static void reduce_weight_counts_to_cycle_limit(std::vector<int>& counts, size_t cycle_limit)
{
    if (counts.empty() || cycle_limit == 0)
        return;

    int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0 || size_t(total) <= cycle_limit)
        return;

    std::vector<size_t> positive_indices;
    positive_indices.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] > 0)
            positive_indices.emplace_back(i);

    if (positive_indices.empty()) {
        counts.assign(counts.size(), 0);
        return;
    }

    std::vector<int> reduced(counts.size(), 0);
    if (cycle_limit < positive_indices.size()) {
        std::sort(positive_indices.begin(), positive_indices.end(), [&counts](size_t lhs, size_t rhs) {
            if (counts[lhs] != counts[rhs])
                return counts[lhs] > counts[rhs];
            return lhs < rhs;
        });
        for (size_t i = 0; i < cycle_limit; ++i)
            reduced[positive_indices[i]] = 1;
        counts = std::move(reduced);
        return;
    }

    size_t remaining_slots = cycle_limit;
    for (const size_t idx : positive_indices) {
        reduced[idx] = 1;
        --remaining_slots;
    }

    int              total_extras = 0;
    std::vector<int> extra_counts(counts.size(), 0);
    for (const size_t idx : positive_indices) {
        extra_counts[idx] = std::max(0, counts[idx] - 1);
        total_extras += extra_counts[idx];
    }
    if (remaining_slots == 0 || total_extras <= 0) {
        counts = std::move(reduced);
        return;
    }

    std::vector<double> remainders(counts.size(), -1.0);
    size_t              assigned_slots = 0;
    for (const size_t idx : positive_indices) {
        if (extra_counts[idx] == 0)
            continue;
        const double exact    = double(remaining_slots) * double(extra_counts[idx]) / double(total_extras);
        const int    assigned = int(std::floor(exact));
        reduced[idx] += assigned;
        assigned_slots += size_t(assigned);
        remainders[idx] = exact - double(assigned);
    }

    size_t missing_slots = remaining_slots > assigned_slots ? (remaining_slots - assigned_slots) : size_t(0);
    while (missing_slots > 0) {
        size_t best_idx       = size_t(-1);
        double best_remainder = -1.0;
        int    best_extra     = -1;
        for (const size_t idx : positive_indices) {
            if (extra_counts[idx] == 0)
                continue;
            if (remainders[idx] > best_remainder ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] > best_extra) ||
                (std::abs(remainders[idx] - best_remainder) <= 1e-9 && extra_counts[idx] == best_extra && idx < best_idx)) {
                best_idx       = idx;
                best_remainder = remainders[idx];
                best_extra     = extra_counts[idx];
            }
        }
        if (best_idx == size_t(-1))
            break;
        ++reduced[best_idx];
        remainders[best_idx] = -1.0;
        --missing_slots;
    }

    counts = std::move(reduced);
}

std::vector<unsigned int> build_weighted_gradient_sequence(const std::vector<unsigned int>& ids,
                                                           const std::vector<int>&          weights,
                                                           size_t                           max_cycle_limit)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int& c : counts)
            c = std::max(1, c / g);
    }

    constexpr size_t k_max_cycle       = 48;
    const size_t effective_cycle_limit = max_cycle_limit > 0 ? std::min(k_max_cycle, std::max<size_t>(1, max_cycle_limit)) : k_max_cycle;
    reduce_weight_counts_to_cycle_limit(counts, effective_cycle_limit);

    std::vector<unsigned int> reduced_ids;
    std::vector<int>          reduced_counts;
    reduced_ids.reserve(filtered_ids.size());
    reduced_counts.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] <= 0)
            continue;
        reduced_ids.emplace_back(filtered_ids[i]);
        reduced_counts.emplace_back(counts[i]);
    }
    if (reduced_ids.empty())
        return {};
    filtered_ids = std::move(reduced_ids);
    counts       = std::move(reduced_counts);

    const int total = std::accumulate(counts.begin(), counts.end(), 0);
    if (total <= 0)
        return {};

    const size_t cycle = size_t(total);

    std::vector<unsigned int> sequence;
    sequence.reserve(cycle);
    std::vector<int> emitted(counts.size(), 0);
    for (size_t pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double(pos + 1) * double(counts[i]) / double(total);
            const double score  = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx   = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

bool local_z_eligible_mixed_definition(const MixedFilament& definition)
{
    // Local-Z flow-height modulation should apply to all mixed rows that are
    // resolved as A/B blends on model surfaces, not only custom rows.
    return definition.enabled && !definition.deleted && definition.manual_pattern.empty() &&
           definition.distribution_mode != int(MixedFilament::SameLayerPointillisme);
}

ExPolygons collect_layer_region_slices(const Layer& layer)
{
    ExPolygons out;
    for (const LayerRegion* layerm : layer.regions())
        append(out, to_expolygons(layerm->slices.surfaces));
    if (!out.empty())
        out = union_ex(out);
    return out;
}
}} // namespace Slic3r::MixedFilamentSlicing
