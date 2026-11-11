#include "../MixedFilament.hpp"
#include "../PrintConfig.hpp"
#include "../FullSpectrumKSPairResidual.hpp"
#include "../filament_mixer.h"
#include "../libslic3r.h"
#include "Internal.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <locale>
#include <unordered_set>

namespace Slic3r { namespace MixedFilamentInternal {
unsigned int decode_manual_pattern_preview_token(const std::string& token,
                                                 unsigned int       component_a,
                                                 unsigned int       component_b,
                                                 size_t             num_physical)
{
    if (token == "1")
        return (component_a >= 1 && component_a <= num_physical) ? component_a : 0;
    if (token == "2")
        return (component_b >= 1 && component_b <= num_physical) ? component_b : 0;

    char* end        = nullptr;
    errno            = 0;
    unsigned long id = std::strtoul(token.c_str(), &end, 10);
    if (errno != ERANGE && *end == '\0' && id >= 1 && id <= num_physical)
        return unsigned(id);

    return 0;
}

std::vector<unsigned int> build_grouped_manual_pattern_preview_sequence(
    const std::string& pattern, unsigned int component_a, unsigned int component_b, size_t num_physical, size_t wall_loops)
{
    std::vector<unsigned int> sequence;
    if (num_physical == 0)
        return sequence;

    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(pattern);
    if (normalized.empty())
        return sequence;

    const std::vector<std::string> groups = split_manual_pattern_groups(normalized);
    if (groups.empty())
        return sequence;

    if (groups.size() == 1) {
        const std::vector<std::string> tokens = MixedFilamentManager::split_pattern_group_to_tokens(groups[0], num_physical);
        sequence.reserve(tokens.size());
        for (const std::string& token : tokens) {
            const unsigned int extruder_id = decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
        return sequence;
    }

    // Build per-group token vectors for indexed access
    std::vector<std::vector<std::string>> group_tokens;
    group_tokens.reserve(groups.size());
    for (const std::string& group : groups)
        group_tokens.push_back(MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical));

    constexpr size_t k_max_preview_cycle = 48;
    size_t           cycle               = 1;
    for (const auto& tokens : group_tokens) {
        if (tokens.empty())
            continue;
        cycle = std::lcm(cycle, tokens.size());
        if (cycle >= k_max_preview_cycle) {
            cycle = k_max_preview_cycle;
            break;
        }
    }

    const size_t preview_wall_loops = std::max<size_t>(1, wall_loops == 0 ? groups.size() : wall_loops);
    sequence.reserve(preview_wall_loops * cycle);
    for (size_t layer_idx = 0; layer_idx < cycle; ++layer_idx) {
        for (size_t wall_idx = 0; wall_idx < preview_wall_loops; ++wall_idx) {
            const auto& tokens = group_tokens[std::min(wall_idx, group_tokens.size() - 1)];
            if (tokens.empty())
                continue;
            const std::string& token       = tokens[layer_idx % tokens.size()];
            const unsigned int extruder_id = decode_manual_pattern_preview_token(token, component_a, component_b, num_physical);
            if (extruder_id != 0)
                sequence.emplace_back(extruder_id);
        }
    }

    return sequence;
}

std::pair<int, int> effective_pair_preview_ratios(int percent_b)
{
    const int mix_b   = std::clamp(percent_b, 0, 100);
    int       ratio_a = 1;
    int       ratio_b = 0;

    if (mix_b >= 100) {
        ratio_a = 0;
        ratio_b = 1;
    } else if (mix_b > 0) {
        const int  pct_b        = mix_b;
        const int  pct_a        = 100 - pct_b;
        const bool b_is_major   = pct_b >= pct_a;
        const int  major_pct    = b_is_major ? pct_b : pct_a;
        const int  minor_pct    = b_is_major ? pct_a : pct_b;
        const int  major_layers = std::max(1, int(std::lround(double(major_pct) / double(std::max(1, minor_pct)))));
        ratio_a                 = b_is_major ? 1 : major_layers;
        ratio_b                 = b_is_major ? major_layers : 1;
    }

    if (ratio_a > 0 && ratio_b > 0) {
        const int g = std::gcd(ratio_a, ratio_b);
        if (g > 1) {
            ratio_a /= g;
            ratio_b /= g;
        }
    }

    return {std::max(0, ratio_a), std::max(0, ratio_b)};
}

std::vector<unsigned int> build_effective_pair_preview_sequence(unsigned int component_a,
                                                                unsigned int component_b,
                                                                int          percent_b,
                                                                bool         limit_cycle)
{
    std::vector<unsigned int> sequence;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return sequence;

    auto [ratio_a, ratio_b]   = effective_pair_preview_ratios(percent_b);
    constexpr int k_max_cycle = 24;
    if (limit_cycle && ratio_a > 0 && ratio_b > 0 && ratio_a + ratio_b > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(ratio_a + ratio_b);
        ratio_a            = std::max(1, int(std::round(double(ratio_a) * scale)));
        ratio_b            = std::max(1, int(std::round(double(ratio_b) * scale)));
    }
    if (ratio_a == 0 && ratio_b == 0)
        ratio_a = 1;

    const int cycle = std::max(1, ratio_a + ratio_b);
    sequence.reserve(size_t(cycle));
    for (int pos = 0; pos < cycle; ++pos) {
        const int b_before = (pos * ratio_b) / cycle;
        const int b_after  = ((pos + 1) * ratio_b) / cycle;
        sequence.emplace_back((b_after > b_before) ? component_b : component_a);
    }
    return sequence;
}

std::string blend_display_color_from_sequence(const std::vector<std::string>&    colors,
                                              size_t                             num_physical,
                                              const std::vector<unsigned int>&   sequence,
                                              const std::string&                 fallback,
                                              const MixedFilamentDisplayContext& context)
{
    if (colors.empty() || sequence.empty() || num_physical == 0)
        return fallback;

    std::vector<size_t> counts(num_physical + 1, size_t(0));
    size_t              total = 0;
    for (const unsigned int id : sequence) {
        if (id == 0 || id > num_physical)
            continue;
        ++counts[id];
        ++total;
    }
    if (total == 0)
        return fallback;

    std::vector<MixedFilamentColorInput> color_percents;
    color_percents.reserve(num_physical);
    for (size_t id = 1; id <= num_physical; ++id) {
        if (counts[id] == 0 || id > colors.size())
            continue;
        color_percents.push_back({colors[id - 1], int(counts[id]), physical_td_for_id(context, unsigned(id)),
                                  physical_material_id_for_id(context, unsigned(id))});
    }
    if (color_percents.empty())
        return fallback;

    if (color_percents.size() == 1)
        return color_percents.front().color_hex;

    return MixedFilamentManager::blend_color_multi(color_percents, context.color_engine.value_or(MixedFilamentManager::color_engine()));
}

}} // namespace Slic3r::MixedFilamentInternal

namespace Slic3r {
std::vector<double> mixed_filament_preview_pass_heights(const MixedFilamentPreviewSettings& settings,
                                                        int                                 mix_b_percent,
                                                        int                                 max_sublayers_limit)
{
    const double nominal_layer_height = settings.nominal_layer_height;
    const double lower_bound          = settings.mixed_lower_bound;
    const double upper_bound          = settings.mixed_upper_bound;
    const double preferred_a_height   = settings.preferred_a_height;
    const double preferred_b_height   = settings.preferred_b_height;

    if (nominal_layer_height <= EPSILON)
        return {};

    const double base_height      = nominal_layer_height;
    const double lo               = std::max<double>(0.01, lower_bound);
    const double hi               = std::max<double>(lo, upper_bound);
    const size_t max_passes_limit = max_sublayers_limit >= 2 ? size_t(max_sublayers_limit) : size_t(0);

    auto fit_pass_heights_to_interval = [](std::vector<double>& passes, double total_height, double local_lo, double local_hi) {
        if (passes.empty() || total_height <= EPSILON)
            return false;

        const auto within = [local_lo, local_hi](double value) { return value >= local_lo - 1e-6 && value <= local_hi + 1e-6; };

        double sum = 0.0;
        for (const double h : passes)
            sum += h;

        double delta = total_height - sum;
        if (std::abs(delta) > 1e-6) {
            if (delta > 0.0) {
                for (double& h : passes) {
                    if (delta <= 1e-6)
                        break;
                    const double room = local_hi - h;
                    if (room <= 1e-6)
                        continue;
                    const double take = std::min(room, delta);
                    h += take;
                    delta -= take;
                }
            } else {
                for (auto it = passes.rbegin(); it != passes.rend() && delta < -1e-6; ++it) {
                    const double room = *it - local_lo;
                    if (room <= 1e-6)
                        continue;
                    const double take = std::min(room, -delta);
                    *it -= take;
                    delta += take;
                }
            }
        }

        if (std::abs(delta) > 1e-6)
            return false;
        return std::all_of(passes.begin(), passes.end(), within);
    };

    auto build_uniform = [&fit_pass_heights_to_interval, base_height, lo, hi, max_passes_limit]() {
        std::vector<double> out;
        size_t              min_passes = size_t(std::max<double>(1.0, std::ceil((base_height - EPSILON) / hi)));
        size_t              max_passes = size_t(std::max<double>(1.0, std::floor((base_height + EPSILON) / lo)));
        size_t              pass_count = min_passes;

        if (max_passes >= min_passes) {
            const double target_step   = 0.5 * (lo + hi);
            const size_t target_passes = size_t(std::max<double>(1.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
            pass_count                 = std::clamp(target_passes, min_passes, max_passes);
        }

        if (max_passes_limit > 0 && pass_count > max_passes_limit)
            pass_count = max_passes_limit;

        if (pass_count == 1 && base_height >= 2.0 * lo - EPSILON && max_passes >= 2)
            pass_count = 2;

        if (pass_count <= 1) {
            out.emplace_back(base_height);
            return out;
        }

        out.assign(pass_count, base_height / double(pass_count));
        double accumulated = 0.0;
        for (size_t i = 0; i + 1 < out.size(); ++i)
            accumulated += out[i];
        out.back() = std::max<double>(EPSILON, base_height - accumulated);
        if (!fit_pass_heights_to_interval(out, base_height, lo, hi) && max_passes_limit == 0) {
            out.assign(pass_count, base_height / double(pass_count));
            accumulated = 0.0;
            for (size_t i = 0; i + 1 < out.size(); ++i)
                accumulated += out[i];
            out.back() = std::max<double>(EPSILON, base_height - accumulated);
        }
        return out;
    };

    auto build_alternating = [&build_uniform, &fit_pass_heights_to_interval, base_height, lo, hi, max_passes_limit](double gradient_h_a,
                                                                                                                    double gradient_h_b) {
        if (base_height < 2.0 * lo - EPSILON)
            return std::vector<double>{base_height};

        const double cycle_h = std::max<double>(EPSILON, gradient_h_a + gradient_h_b);
        const double ratio_a = std::clamp(gradient_h_a / cycle_h, 0.0, 1.0);

        size_t min_passes = size_t(std::max<double>(2.0, std::ceil((base_height - EPSILON) / hi)));
        if ((min_passes % 2) != 0)
            ++min_passes;

        size_t max_passes = size_t(std::max<double>(2.0, std::floor((base_height + EPSILON) / lo)));
        if ((max_passes % 2) != 0)
            --max_passes;
        if (max_passes_limit > 0) {
            size_t capped_limit = std::max<size_t>(2, max_passes_limit);
            if ((capped_limit % 2) != 0)
                --capped_limit;
            if (capped_limit >= 2)
                max_passes = std::min(max_passes, capped_limit);
        }
        if (max_passes < 2)
            return build_uniform();
        if (min_passes > max_passes)
            min_passes = max_passes;
        if (min_passes < 2)
            min_passes = 2;
        if ((min_passes % 2) != 0)
            ++min_passes;
        if (min_passes > max_passes)
            return build_uniform();

        const double target_step   = 0.5 * (lo + hi);
        size_t       target_passes = size_t(std::max<double>(2.0, std::llround(base_height / std::max<double>(target_step, EPSILON))));
        if ((target_passes % 2) != 0) {
            const size_t round_up   = (target_passes < max_passes) ? (target_passes + 1) : max_passes;
            const size_t round_down = (target_passes > min_passes) ? (target_passes - 1) : min_passes;
            if (round_up > max_passes)
                target_passes = round_down;
            else if (round_down < min_passes)
                target_passes = round_up;
            else
                target_passes = ((round_up - target_passes) <= (target_passes - round_down)) ? round_up : round_down;
        }
        target_passes = std::clamp(target_passes, min_passes, max_passes);

        bool                has_best = false;
        std::vector<double> best_passes;
        double              best_ratio_error   = 0.0;
        size_t              best_pass_distance = 0;
        double              best_max_height    = 0.0;
        size_t              best_pass_count    = 0;

        for (size_t pass_count = min_passes; pass_count <= max_passes; pass_count += 2) {
            const size_t pair_count = pass_count / 2;
            if (pair_count == 0)
                continue;
            const double pair_h = base_height / double(pair_count);

            const double h_a_min = std::max(lo, pair_h - hi);
            const double h_a_max = std::min(hi, pair_h - lo);
            if (h_a_min > h_a_max + EPSILON)
                continue;

            const double h_a = std::clamp(pair_h * ratio_a, h_a_min, h_a_max);
            const double h_b = pair_h - h_a;

            std::vector<double> out;
            out.reserve(pass_count);
            for (size_t pair_idx = 0; pair_idx < pair_count; ++pair_idx) {
                out.emplace_back(h_a);
                out.emplace_back(h_b);
            }
            if (!fit_pass_heights_to_interval(out, base_height, lo, hi))
                continue;

            const double ratio_actual  = (h_a + h_b > EPSILON) ? (h_a / (h_a + h_b)) : 0.5;
            const double ratio_error   = std::abs(ratio_actual - ratio_a);
            const size_t pass_distance = (pass_count > target_passes) ? (pass_count - target_passes) : (target_passes - pass_count);
            const double max_height    = std::max(h_a, h_b);

            const bool better_ratio       = !has_best || (ratio_error + 1e-6 < best_ratio_error);
            const bool similar_ratio      = has_best && std::abs(ratio_error - best_ratio_error) <= 1e-6;
            const bool better_distance    = similar_ratio && (pass_distance < best_pass_distance);
            const bool similar_distance   = similar_ratio && (pass_distance == best_pass_distance);
            const bool better_max_height  = similar_distance && (max_height + 1e-6 < best_max_height);
            const bool similar_max_height = similar_distance && std::abs(max_height - best_max_height) <= 1e-6;
            const bool better_pass_count  = similar_max_height && (pass_count > best_pass_count);

            if (better_ratio || better_distance || better_max_height || better_pass_count) {
                has_best           = true;
                best_passes        = std::move(out);
                best_ratio_error   = ratio_error;
                best_pass_distance = pass_distance;
                best_max_height    = max_height;
                best_pass_count    = pass_count;
            }
        }

        return has_best ? best_passes : build_uniform();
    };

    if (preferred_a_height > EPSILON || preferred_b_height > EPSILON) {
        std::vector<double> cadence_unit;
        if (preferred_a_height > EPSILON)
            cadence_unit.push_back(std::clamp(preferred_a_height, lo, hi));
        if (preferred_b_height > EPSILON)
            cadence_unit.push_back(std::clamp(preferred_b_height, lo, hi));

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
        }

        if (preferred_a_height > EPSILON && preferred_b_height > EPSILON)
            return build_alternating(preferred_a_height, preferred_b_height);
        return build_uniform();
    }

    const int    mix_b        = std::clamp(mix_b_percent, 0, 100);
    const double pct_b        = double(mix_b) / 100.0;
    const double pct_a        = 1.0 - pct_b;
    const double gradient_h_a = lo + pct_a * (hi - lo);
    const double gradient_h_b = lo + pct_b * (hi - lo);
    return build_alternating(gradient_h_a, gradient_h_b);
}
} // namespace Slic3r

namespace Slic3r {
MixedFilamentPreviewSettings mixed_filament_preview_settings(const DynamicPrintConfig& project_config, const DynamicPrintConfig* print_cfg)
{
    MixedFilamentPreviewSettings settings;
    auto                         get_mixed_bool = [&project_config, print_cfg](const std::string& key, bool fallback) {
        if (const ConfigOptionBool* opt = project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt* opt = project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [&project_config, print_cfg](const std::string& key, float fallback) {
        if (const auto* opt = project_config.option<ConfigOptionPercent>(key))
            return float(opt->value);
        if (project_config.has(key))
            return float(project_config.opt_float(key));
        if (print_cfg != nullptr)
            if (const auto* opt = print_cfg->option<ConfigOptionPercent>(key))
                return float(opt->value);
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    settings.mixed_lower_bound  = std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.06f)));
    settings.mixed_upper_bound  = std::max(settings.mixed_lower_bound, double(get_mixed_float("mixed_filament_height_upper_bound", 0.16f)));
    settings.preferred_a_height = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    settings.preferred_b_height = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    settings.local_z_mode              = get_mixed_bool("dithering_local_z_mode", false);
    settings.local_z_direct_multicolor = (settings.local_z_mode || get_mixed_bool("dithering_local_z_direct_multicolor", true)) &&
                                         settings.preferred_a_height <= EPSILON && settings.preferred_b_height <= EPSILON;
    settings.local_z_independent_layer_height = settings.local_z_mode || get_mixed_bool("dithering_local_z_independent_layer_height", true);
    settings.gradient_cycle_height            = get_mixed_float("dithering_local_z_gradient_layer_height", 0.20f);
    settings.gradient_middle_window           = get_mixed_float("dithering_local_z_gradient_middle_filament_window", 3.f) / 100.0;
    return settings;
}
} // namespace Slic3r
