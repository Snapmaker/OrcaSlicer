#include "MixedColorMatchHelpers.hpp"
#include "MixedGradientSelector.hpp"
#include <unordered_set>
#include <ColorSpaceConvert.hpp>
#include "MixedFilamentColorMapPanel.hpp"
#include "GUI_App.hpp"
#include "PresetBundle.hpp"
#include <algorithm>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {
wxColour parse_mixed_color(const std::string& value)
{
    wxColour color(value);
    if (!color.IsOk())
        color = wxColour("#26A69A");
    return color;
}

wxString normalize_color_match_hex(const wxString& value)
{
    wxString normalized = value;
    normalized.Trim(true);
    normalized.Trim(false);
    normalized.MakeUpper();
    if (!normalized.empty() && normalized[0] != '#')
        normalized.Prepend("#");
    return normalized;
}

bool try_parse_color_match_hex(const wxString& value, wxColour& color_out)
{
    const wxString normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7)
        return false;

    for (size_t idx = 1; idx < normalized.length(); ++idx) {
        const unsigned char ch = static_cast<unsigned char>(normalized[idx]);
        if (!std::isxdigit(ch))
            return false;
    }

    wxColour parsed(normalized);
    if (!parsed.IsOk())
        return false;

    color_out = parsed;
    return true;
}

std::vector<int> normalize_color_match_weights(const std::vector<int>& weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / count) : 0);

    int sum = 0;
    for (int& value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int                 assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        const std::vector<int>          raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult& recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    if (!recipe.gradient_component_ids.empty()) {
        ids     = decode_color_match_gradient_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()),
                                                ids.size());
    } else {
        ids     = {recipe.component_a, recipe.component_b};
        weights = {std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)), std::max(0, std::clamp(recipe.mix_b_percent, 0, 100))};
    }
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

wxBitmap make_color_match_swatch_bitmap(const wxColour& color, const wxSize& size)
{
    wxBitmap   bmp(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
    dc.SetBrush(wxBrush(color.IsOk() ? color : wxColour("#26A69A")));
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string>& physical_colors,
                                                                   int                             min_component_percent)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t                k_max_presets = 48;
    std::unordered_set<std::string> seen_colors;
    auto                            add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.GetAsString(wxC2S_HTML_SYNTAX)).ToStdString();
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    // Only generate 50:50 ratio for two-color combinations
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), 50,
                                                           min_component_percent));
        }
    }

    const size_t           triple_limit         = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({1, 1, 1}, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                const std::vector<unsigned int> ids = {unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1)};
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }

    const size_t quad_limit = std::min<size_t>(palette.size(), 5);
    for (size_t first_idx = 0; first_idx + 3 < quad_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_limit && presets.size() < k_max_presets; ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_limit && presets.size() < k_max_presets; ++fourth_idx) {
                    add_candidate(build_multi_color_match_candidate(palette,
                                                                    {unsigned(first_idx + 1), unsigned(second_idx + 1),
                                                                     unsigned(third_idx + 1), unsigned(fourth_idx + 1)},
                                                                    {25, 25, 25, 25}, min_component_percent));
                }
            }
        }
    }

    return presets;
}

double color_delta_e00(const wxColour& lhs, const wxColour& rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()), float(lhs.Green()), float(lhs.Blue()), &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()), float(rhs.Green()), float(rhs.Blue()), &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const wxColour&                 target_color,
                                                          int                             min_component_percent)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    std::vector<wxColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    auto consider_candidate = [&best, &target_color](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        candidate.delta_e = color_delta_e00(target_color, candidate.preview_color);
        if (!best.valid || candidate.delta_e + 1e-6 < best.delta_e)
            best = std::move(candidate);
    };

    const int loop_min_weight      = std::max(1, std::clamp(min_component_percent, 0, 50));
    const int loop_max_pair_weight = 100 - loop_min_weight;

    for (size_t left_idx = 0; left_idx < palette.size(); ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size(); ++right_idx) {
            for (int mix_b_percent = loop_min_weight; mix_b_percent <= loop_max_pair_weight; ++mix_b_percent)
                consider_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), mix_b_percent,
                                                                    min_component_percent));
        }
    }

    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(palette.size());
    for (size_t idx = 0; idx < palette.size(); ++idx)
        ranked_ids.emplace_back(color_delta_e00(target_color, palette[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first < rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(std::min<size_t>(palette.size(), 12));
    auto push_unique_id = [&candidate_pool](unsigned int filament_id) {
        if (filament_id == 0 || filament_id > 9)
            return;
        if (std::find(candidate_pool.begin(), candidate_pool.end(), filament_id) == candidate_pool.end())
            candidate_pool.emplace_back(filament_id);
    };

    const size_t general_pool_limit = std::min<size_t>(ranked_ids.size(), 8);
    for (size_t idx = 0; idx < general_pool_limit; ++idx)
        push_unique_id(ranked_ids[idx].second);

    size_t direct_token_count = 0;
    for (const auto& [distance, filament_id] : ranked_ids) {
        (void) distance;
        if (filament_id < 3 || filament_id > 9)
            continue;
        push_unique_id(filament_id);
        if (++direct_token_count >= 4)
            break;
    }

    if (candidate_pool.size() < 3)
        return best;

    std::vector<unsigned int> triple_pool = candidate_pool;
    std::sort(triple_pool.begin(), triple_pool.end());
    for (size_t first_idx = 0; first_idx + 2 < triple_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_pool.size(); ++third_idx) {
                const std::vector<unsigned int> ids = {triple_pool[first_idx], triple_pool[second_idx], triple_pool[third_idx]};
                if (std::any_of(ids.begin(), ids.end(), [](unsigned int filament_id) { return filament_id == 0 || filament_id > 9; }))
                    continue;

                for (int weight_a = loop_min_weight; weight_a <= 100 - 2 * loop_min_weight; ++weight_a) {
                    for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - loop_min_weight; ++weight_b) {
                        const int weight_c = 100 - weight_a - weight_b;
                        consider_candidate(
                            build_multi_color_match_candidate(palette, ids, {weight_a, weight_b, weight_c}, min_component_percent));
                    }
                }
            }
        }
    }

    if (candidate_pool.size() < 4)
        return best;

    std::vector<unsigned int> quad_pool(candidate_pool.begin(), candidate_pool.begin() + std::min<size_t>(candidate_pool.size(), 6));
    std::sort(quad_pool.begin(), quad_pool.end());
    for (size_t first_idx = 0; first_idx + 3 < quad_pool.size(); ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 2 < quad_pool.size(); ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx + 1 < quad_pool.size(); ++third_idx) {
                for (size_t fourth_idx = third_idx + 1; fourth_idx < quad_pool.size(); ++fourth_idx) {
                    const std::vector<unsigned int> ids = {quad_pool[first_idx], quad_pool[second_idx], quad_pool[third_idx],
                                                           quad_pool[fourth_idx]};

                    for (int weight_a = loop_min_weight; weight_a <= 100 - 3 * loop_min_weight; ++weight_a) {
                        for (int weight_b = loop_min_weight; weight_a + weight_b <= 100 - 2 * loop_min_weight; ++weight_b) {
                            for (int weight_c = loop_min_weight; weight_a + weight_b + weight_c <= 100 - loop_min_weight; ++weight_c) {
                                const int weight_d = 100 - weight_a - weight_b - weight_c;
                                consider_candidate(build_multi_color_match_candidate(palette, ids, {weight_a, weight_b, weight_c, weight_d},
                                                                                     min_component_percent));
                            }
                        }
                    }
                }
            }
        }
    }

    return best;
}

MixedFilamentDisplayContext build_mixed_filament_display_context(const std::vector<std::string>& physical_colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);

    auto* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return context;

    DynamicPrintConfig* print_cfg = &preset_bundle->prints.get_edited_preset().config;
    if (const ConfigOptionFloats* opt = preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")) {
        const size_t opt_count = opt->values.size();
        if (opt_count > 0) {
            for (size_t i = 0; i < context.num_physical; ++i)
                context.nozzle_diameters[i] = std::max(0.05, opt->get_at(unsigned(std::min(i, opt_count - 1))));
        }
    }

    auto get_mixed_bool = [preset_bundle, print_cfg](const std::string& key, bool fallback) {
        if (const ConfigOptionBool* opt = preset_bundle->project_config.option<ConfigOptionBool>(key))
            return opt->value;
        if (const ConfigOptionInt* opt = preset_bundle->project_config.option<ConfigOptionInt>(key))
            return opt->value != 0;
        if (print_cfg != nullptr) {
            if (const ConfigOptionBool* opt = print_cfg->option<ConfigOptionBool>(key))
                return opt->value;
            if (const ConfigOptionInt* opt = print_cfg->option<ConfigOptionInt>(key))
                return opt->value != 0;
        }
        return fallback;
    };
    auto get_mixed_float = [preset_bundle, print_cfg](const std::string& key, float fallback) {
        if (preset_bundle->project_config.has(key))
            return float(preset_bundle->project_config.opt_float(key));
        if (print_cfg != nullptr && print_cfg->has(key))
            return float(print_cfg->opt_float(key));
        return fallback;
    };

    context.preview_settings.mixed_lower_bound    = std::max(0.01, double(get_mixed_float("mixed_filament_height_lower_bound", 0.04f)));
    context.preview_settings.mixed_upper_bound    = std::max(context.preview_settings.mixed_lower_bound,
                                                             double(get_mixed_float("mixed_filament_height_upper_bound", 0.16f)));
    context.preview_settings.preferred_a_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_a", 0.f)));
    context.preview_settings.preferred_b_height   = std::max(0.0, double(get_mixed_float("mixed_color_layer_height_b", 0.f)));
    context.preview_settings.nominal_layer_height = 0.2;
    if (print_cfg != nullptr && print_cfg->has("layer_height"))
        context.preview_settings.nominal_layer_height = std::max(0.01, print_cfg->opt_float("layer_height"));
    if (print_cfg != nullptr && print_cfg->has("wall_loops"))
        context.preview_settings.wall_loops = std::max<size_t>(1, size_t(std::max(1, print_cfg->opt_int("wall_loops"))));
    context.preview_settings.local_z_mode              = get_mixed_bool("dithering_local_z_mode", false);
    context.preview_settings.local_z_direct_multicolor = get_mixed_bool("dithering_local_z_direct_multicolor", false) &&
                                                         context.preview_settings.preferred_a_height <= EPSILON &&
                                                         context.preview_settings.preferred_b_height <= EPSILON;
    context.component_bias_enabled = get_mixed_bool("mixed_filament_component_bias_enabled", false);

    return context;
}

wxColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult& recipe, const MixedFilamentDisplayContext& context)
{
    if (!recipe.valid)
        return recipe.preview_color.IsOk() ? recipe.preview_color : wxColour("#26A69A");

    MixedFilament entry;
    entry.component_a                = recipe.component_a;
    entry.component_b                = recipe.component_b;
    entry.mix_b_percent              = recipe.mix_b_percent;
    entry.manual_pattern             = recipe.manual_pattern;
    entry.gradient_component_ids     = recipe.gradient_component_ids;
    entry.gradient_component_weights = recipe.gradient_component_weights;
    entry.distribution_mode          = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    return parse_mixed_color(compute_mixed_filament_display_color(entry, context));
}

std::vector<unsigned int> decode_color_match_gradient_ids(const std::string& value)
{
    std::vector<unsigned int> ids;
    bool                      seen[10] = {false};
    for (const char ch : value) {
        if (ch < '1' || ch > '9')
            continue;
        const unsigned int id = unsigned(ch - '0');
        if (seen[id])
            continue;
        seen[id] = true;
        ids.emplace_back(id);
    }
    return ids;
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

MixedColorMatchRecipeResult build_pair_color_match_candidate(
    const std::vector<wxColour>& palette, unsigned int component_a, unsigned int component_b, int mix_b_percent, int min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100)},
                                          min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<wxColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size() || ids[idx] > 9)
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto& [weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid             = true;
    candidate.component_a       = ordered_ids[0];
    candidate.component_b       = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent     = pair_weight_total > 0 ?
                                      std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
                                      50;
    for (const unsigned int filament_id : ordered_ids)
        candidate.gradient_component_ids.push_back(char('0' + filament_id));
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed       = std::clamp(min_component_percent, 0, 50);
    int       active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }

    return sequence;
}

wxColour blend_sequence_filament_mixer(const std::vector<wxColour>& palette, const std::vector<unsigned int>& sequence)
{
    if (palette.empty() || sequence.empty())
        return wxColour("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<wxColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }

    return blend_multi_filament_mixer(colors, weights);
}

// ---------------------------------------------------------------------------
// Material compatibility
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, int>& filament_compatibility_group_map()
{
    static const std::unordered_map<std::string, int> m = {
        {"PLA", 1}, {"PLA-AERO", 1}, {"PLA-CF", 1},
        {"ABS", 2}, {"ABS-GF", 2},
        {"ASA", 3}, {"ASA-Aero", 3},
        {"PETG", 4}, {"PETG-CF", 4}, {"PETG-CF10", 4}, {"PETG-GF", 4},
        {"TPU", 5}, {"FLEX", 5}, {"EVA", 5}, {"SBS", 5},
        {"PCTG", 6},
        {"HIPS", 7},
        {"BVOH", 8}, {"PVA", 8}, {"PVB", 8},
        {"PA", 9}, {"PA-CF", 9}, {"PA-GF", 9}, {"PA6-CF", 9}, {"PA11-CF", 9},
        {"PC", 10}, {"PC-CF", 10},
        {"PP", 11}, {"PP-CF", 11}, {"PP-GF", 11},
        {"PE", 12}, {"PE-CF", 12},
        {"PET-CF", 13},
        {"PHA", 14},
        {"PPS", 15}, {"PPS-CF", 15}, {"PPA", 15}, {"PPA-CF", 15}, {"PPA-GF", 15},
    };
    return m;
}

static std::string normalize_filament_type(const std::string& type)
{
    std::string normalized = type;
    // Trim leading/trailing whitespace
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end = normalized.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos) {
        normalized = normalized.substr(start, end - start + 1);
    }
    // Convert to uppercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);
    return normalized;
}

int get_filament_compatibility_group(const std::string& filament_type)
{
    if (filament_type.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Empty filament type in compatibility check";
        return -1;
    }

    const auto& m = filament_compatibility_group_map();
    auto it = m.find(filament_type);
    if (it != m.end()) return it->second;
    std::string normalized = normalize_filament_type(filament_type);
    it = m.find(normalized);
    if (it != m.end()) return it->second;

    // Assign stable unique group IDs to unknown types (no hash collision risk)
    static std::unordered_map<std::string, int> unknown_groups;
    static int next_unknown_group = 100;
    auto uit = unknown_groups.find(normalized);
    if (uit != unknown_groups.end()) return uit->second;
    int group = next_unknown_group++;
    unknown_groups[normalized] = group;
    BOOST_LOG_TRIVIAL(info) << "Unknown filament type '" << filament_type
                            << "' assigned to compatibility group " << group;
    return group;
}

bool are_filaments_compatible(const std::vector<unsigned int>& filament_ids)
{
    if (filament_ids.size() <= 1) return true;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        BOOST_LOG_TRIVIAL(error) << "PresetBundle is null in filament compatibility check";
        return false;
    }

    const std::vector<std::string>& filament_presets = preset_bundle->filament_presets;
    
    int common_group = -1;
    std::string first_type_name;
    
    for (unsigned int id : filament_ids) {
        if (id >= filament_presets.size()) {
            BOOST_LOG_TRIVIAL(warning) << "Filament ID " << id << " out of range (max: " 
                                       << filament_presets.size() - 1 << ")";
            continue;
        }
        
        const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[id]);
        if (!preset) {
            BOOST_LOG_TRIVIAL(warning) << "Preset not found for filament ID " << id;
            continue;
        }
        
        auto* type_opt = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_type"));
        if (!type_opt || type_opt->values.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Filament type not found for preset '" 
                                       << preset->name << "'";
            continue;
        }
        
        const std::string& type_name = type_opt->values[0];
        int group = get_filament_compatibility_group(type_name);
        
        if (group < 0) {
            BOOST_LOG_TRIVIAL(warning) << "Invalid compatibility group for filament ID " << id;
            continue;
        }
        
        if (common_group < 0) {
            common_group = group;
            first_type_name = type_name;
            BOOST_LOG_TRIVIAL(debug) << "Reference filament type: '" << type_name 
                                     << "' (group " << group << ")";
        }
        else if (common_group != group) {
            BOOST_LOG_TRIVIAL(info) << "Incompatible filament types detected: '" 
                                    << first_type_name << "' (group " << common_group 
                                    << ") vs '" << type_name << "' (group " << group << ")";
            return false;
        }
    }
    
    if (common_group < 0) {
        BOOST_LOG_TRIVIAL(warning) << "No valid filament types found in compatibility check";
        return true;
    }
    
    BOOST_LOG_TRIVIAL(debug) << "All filaments compatible (group " << common_group << ")";
    return true;
}

}} // namespace Slic3r::GUI