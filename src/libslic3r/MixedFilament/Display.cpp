#include "../MixedFilament.hpp"
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

namespace Slic3r {
using namespace MixedFilamentInternal;
std::optional<double> physical_td_for_id(const MixedFilamentDisplayContext& context, unsigned int id)
{
    if (!MixedFilamentManager::use_td_for_color_prediction() || id == 0 || id > context.physical_tds.size())
        return std::nullopt;

    const double td = context.physical_tds[id - 1];
    if (!std::isfinite(td) || td <= EPSILON)
        return std::nullopt;
    return td;
}

std::optional<std::string> physical_material_id_for_id(const MixedFilamentDisplayContext& context, unsigned int id)
{
    if (id == 0 || id > context.physical_material_ids.size() || context.physical_material_ids[id - 1].empty())
        return std::nullopt;
    return context.physical_material_ids[id - 1];
}

std::vector<FullSpectrumKSPairResidualColorInput> full_spectrum_inputs_from_mixed_inputs(
    const std::vector<MixedFilamentColorInput>& color_percents)
{
    std::vector<FullSpectrumKSPairResidualColorInput> inputs;
    inputs.reserve(color_percents.size());
    const bool use_td = MixedFilamentManager::use_td_for_color_prediction();
    for (const MixedFilamentColorInput& input : color_percents)
        inputs.push_back({input.color_hex, input.percent, use_td ? input.td_mm : std::nullopt, input.material_id});
    return inputs;
}

int mixed_filament_effective_local_z_preview_mix_b_percent(const MixedFilament& mf, const MixedFilamentPreviewSettings& preview_settings)
{
    if (!preview_settings.local_z_mode)
        return std::clamp(mf.mix_b_percent, 0, 100);

    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (!normalized_pattern.empty() || mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return std::clamp(mf.mix_b_percent, 0, 100);

    const std::vector<unsigned int> gradient_ids = MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0);
    if (gradient_ids.size() >= 3)
        return std::clamp(mf.mix_b_percent, 0, 100);

    if (preview_settings.local_z_independent_layer_height)
        return std::clamp(mf.mix_b_percent, 0, 100);
    int          requested       = mf.mix_b_percent;
    const double preferred_total = preview_settings.preferred_a_height + preview_settings.preferred_b_height;
    if (preferred_total > EPSILON)
        requested = int(std::lround(100.0 * preview_settings.preferred_b_height / preferred_total));
    const auto   heights = mixed_filament_local_z_pair_heights(preview_settings.nominal_layer_height, preview_settings.mixed_lower_bound,
                                                               requested);
    const double total   = heights.first + heights.second;
    return total > EPSILON ? int(std::lround(100.0 * heights.second / total)) : std::clamp(requested, 0, 100);
}

bool mixed_filament_supports_bias_apparent_color(const MixedFilament&                mf,
                                                 const MixedFilamentPreviewSettings& preview_settings,
                                                 bool                                bias_mode_enabled)
{
    if (!bias_mode_enabled)
        return false;
    if (preview_settings.local_z_mode)
        return false;
    if (mf.distribution_mode == int(MixedFilament::SameLayerPointillisme))
        return false;
    if (!MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty())
        return false;
    if (MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0).size() >= 3)
        return false;
    return mf.component_a >= 1 && mf.component_b >= 1 && mf.component_a != mf.component_b;
}

std::pair<int, int> mixed_filament_apparent_pair_percentages(const MixedFilament&                mf,
                                                             const MixedFilamentPreviewSettings& preview_settings,
                                                             const std::vector<double>&          nozzle_diameters,
                                                             bool                                bias_mode_enabled)
{
    const int base_b = mixed_filament_effective_local_z_preview_mix_b_percent(mf, preview_settings);
    if (!mixed_filament_supports_bias_apparent_color(mf, preview_settings, bias_mode_enabled))
        return {100 - base_b, base_b};

    const double reference_nozzle_mm = MixedFilamentManager::mixed_filament_reference_nozzle_mm(mf.component_a, mf.component_b,
                                                                                                nozzle_diameters);
    const int    apparent_b          = MixedFilamentManager::apparent_mix_b_percent(base_b, mf.component_a_surface_offset,
                                                                                    mf.component_b_surface_offset, float(reference_nozzle_mm));
    return {100 - apparent_b, apparent_b};
}

std::string compute_mixed_filament_display_color(const MixedFilament& entry, const MixedFilamentDisplayContext& context)
{
    constexpr const char* fallback = "#26A69A";
    if (context.num_physical == 0 || context.physical_colors.empty())
        return fallback;

    if (mixed_filament_supports_bias_apparent_color(entry, context.preview_settings, context.component_bias_enabled) &&
        entry.component_a >= 1 && entry.component_b >= 1 && entry.component_a <= context.num_physical &&
        entry.component_b <= context.num_physical && entry.component_a <= context.physical_colors.size() &&
        entry.component_b <= context.physical_colors.size()) {
        const auto [apparent_pct_a, apparent_pct_b] = mixed_filament_apparent_pair_percentages(entry, context.preview_settings,
                                                                                               context.nozzle_diameters,
                                                                                               context.component_bias_enabled);
        return MixedFilamentManager::blend_color(context.physical_colors[entry.component_a - 1],
                                                 context.physical_colors[entry.component_b - 1], apparent_pct_a, apparent_pct_b,
                                                 physical_td_for_id(context, entry.component_a),
                                                 physical_td_for_id(context, entry.component_b),
                                                 physical_material_id_for_id(context, entry.component_a),
                                                 physical_material_id_for_id(context, entry.component_b));
    }

    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(entry.manual_pattern);
    if (!normalized_pattern.empty()) {
        const std::vector<unsigned int> sequence = build_grouped_manual_pattern_preview_sequence(normalized_pattern, entry.component_a,
                                                                                                 entry.component_b, context.num_physical,
                                                                                                 context.preview_settings.wall_loops);
        if (!sequence.empty())
            return blend_display_color_from_sequence(context.physical_colors, context.num_physical, sequence, fallback, context);
    }

    if (entry.distribution_mode != int(MixedFilament::Simple)) {
        const std::vector<unsigned int> gradient_ids = MixedFilamentManager::decode_gradient_component_ids(entry.gradient_component_ids,
                                                                                                           context.num_physical);
        if (gradient_ids.size() >= 3) {
            const std::vector<int> gradient_weights = decode_gradient_component_weights(entry.gradient_component_weights,
                                                                                        gradient_ids.size());
            if (context.preview_settings.local_z_mode && context.preview_settings.local_z_direct_multicolor)
                return blend_mixed_components(gradient_ids,
                                              gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights,
                                              context);
            const std::vector<unsigned int> sequence = build_weighted_gradient_sequence(gradient_ids,
                                                                                        gradient_weights.empty() ?
                                                                                            std::vector<int>(gradient_ids.size(), 1) :
                                                                                            gradient_weights);
            if (!sequence.empty())
                return blend_display_color_from_sequence(context.physical_colors, context.num_physical, sequence, fallback, context);
        }
    }

    const int  effective_mix_b                    = mixed_filament_effective_local_z_preview_mix_b_percent(entry, context.preview_settings);
    const bool same_layer_mode                    = entry.distribution_mode == int(MixedFilament::SameLayerPointillisme);
    const std::vector<unsigned int> pair_sequence = build_effective_pair_preview_sequence(entry.component_a, entry.component_b,
                                                                                          effective_mix_b, same_layer_mode);
    if (!pair_sequence.empty())
        return blend_display_color_from_sequence(context.physical_colors, context.num_physical, pair_sequence, fallback, context);

    if (entry.component_a == 0 || entry.component_b == 0 || entry.component_a > context.num_physical ||
        entry.component_b > context.num_physical || entry.component_a > context.physical_colors.size() ||
        entry.component_b > context.physical_colors.size()) {
        return fallback;
    }

    const int mix_b = std::clamp(entry.mix_b_percent, 0, 100);
    return MixedFilamentManager::blend_color(context.physical_colors[entry.component_a - 1], context.physical_colors[entry.component_b - 1],
                                             100 - mix_b, mix_b, physical_td_for_id(context, entry.component_a),
                                             physical_td_for_id(context, entry.component_b),
                                             physical_material_id_for_id(context, entry.component_a),
                                             physical_material_id_for_id(context, entry.component_b));
}

std::string MixedFilamentManager::blend_color_multi(const std::vector<std::pair<std::string, int>>& color_percents)
{
    std::vector<MixedFilamentColorInput> inputs;
    inputs.reserve(color_percents.size());
    for (const auto& [hex, percent] : color_percents)
        inputs.push_back({hex, percent, std::nullopt, std::nullopt});
    return blend_color_multi(inputs);
}

std::string MixedFilamentManager::blend_color_multi(const std::vector<MixedFilamentColorInput>& color_percents)
{
    return blend_color_multi(color_percents, color_engine());
}

std::string MixedFilamentManager::blend_color_multi(const std::vector<MixedFilamentColorInput>& color_percents,
                                                    MixedFilamentColorEngine                    engine)
{
    if (color_percents.empty())
        return "#000000";
    if (color_percents.size() == 1)
        return color_percents.front().color_hex;

    if (engine == MixedFilamentColorEngine::FullSpectrumKSPairResidual) {
        if (const auto calibrated = full_spectrum_ks_blend_color_multi(full_spectrum_inputs_from_mixed_inputs(color_percents)))
            return *calibrated;
    }

    struct WeightedColor
    {
        RGB color;
        int pct;
    };
    std::vector<WeightedColor> colors;
    colors.reserve(color_percents.size());

    int total_pct = 0;
    for (const MixedFilamentColorInput& input : color_percents) {
        const int pct = input.percent;
        if (pct <= 0)
            continue;
        colors.push_back({parse_hex_color(input.color_hex), pct});
        total_pct += pct;
    }
    if (colors.empty() || total_pct <= 0)
        return "#000000";

    unsigned char r               = static_cast<unsigned char>(colors.front().color.r);
    unsigned char g               = static_cast<unsigned char>(colors.front().color.g);
    unsigned char b               = static_cast<unsigned char>(colors.front().color.b);
    int           accumulated_pct = colors.front().pct;

    for (size_t i = 1; i < colors.size(); ++i) {
        const auto& next      = colors[i];
        const int   new_total = accumulated_pct + next.pct;
        if (new_total <= 0)
            continue;
        const float t = static_cast<float>(next.pct) / static_cast<float>(new_total);
        filament_mixer_lerp(r, g, b, static_cast<unsigned char>(next.color.r), static_cast<unsigned char>(next.color.g),
                            static_cast<unsigned char>(next.color.b), t, &r, &g, &b);
        accumulated_pct = new_total;
    }

    return rgb_to_hex({int(r), int(g), int(b)});
}

std::string MixedFilamentManager::blend_color(const std::string& color_a, const std::string& color_b, int ratio_a, int ratio_b)
{
    return blend_color(color_a, color_b, ratio_a, ratio_b, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
}

std::string MixedFilamentManager::blend_color(const std::string&           color_a,
                                              const std::string&           color_b,
                                              int                          ratio_a,
                                              int                          ratio_b,
                                              const std::optional<double>& td_a_mm,
                                              const std::optional<double>& td_b_mm)
{
    return blend_color(color_a, color_b, ratio_a, ratio_b, td_a_mm, td_b_mm, std::nullopt, std::nullopt);
}

std::string MixedFilamentManager::blend_color(const std::string&                color_a,
                                              const std::string&                color_b,
                                              int                               ratio_a,
                                              int                               ratio_b,
                                              const std::optional<double>&      td_a_mm,
                                              const std::optional<double>&      td_b_mm,
                                              const std::optional<std::string>& material_id_a,
                                              const std::optional<std::string>& material_id_b)
{
    const std::optional<double> active_td_a = use_td_for_color_prediction() ? td_a_mm : std::nullopt;
    const std::optional<double> active_td_b = use_td_for_color_prediction() ? td_b_mm : std::nullopt;
    if (color_engine() == MixedFilamentColorEngine::FullSpectrumKSPairResidual) {
        if (const auto calibrated = full_spectrum_ks_blend_color_multi(
                {{color_a, std::max(0, ratio_a), active_td_a, material_id_a}, {color_b, std::max(0, ratio_b), active_td_b, material_id_b}}))
            return *calibrated;
    }

    const int   safe_a = std::max(0, ratio_a);
    const int   safe_b = std::max(0, ratio_b);
    const int   total  = safe_a + safe_b;
    const float t      = (total > 0) ? (static_cast<float>(safe_b) / static_cast<float>(total)) : 0.5f;

    const RGB rgb_a = parse_hex_color(color_a);
    const RGB rgb_b = parse_hex_color(color_b);

    unsigned char out_r = static_cast<unsigned char>(rgb_a.r);
    unsigned char out_g = static_cast<unsigned char>(rgb_a.g);
    unsigned char out_b = static_cast<unsigned char>(rgb_a.b);
    filament_mixer_lerp(static_cast<unsigned char>(rgb_a.r), static_cast<unsigned char>(rgb_a.g), static_cast<unsigned char>(rgb_a.b),
                        static_cast<unsigned char>(rgb_b.r), static_cast<unsigned char>(rgb_b.g), static_cast<unsigned char>(rgb_b.b), t,
                        &out_r, &out_g, &out_b);

    return rgb_to_hex({int(out_r), int(out_g), int(out_b)});
}

float MixedFilamentManager::max_component_surface_offset_mm(float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    return std::clamp(safe_reference, 0.01f, 0.35f);
}

float MixedFilamentManager::max_pair_bias_mm(float reference_width_mm) { return max_component_surface_offset_mm(reference_width_mm); }

std::pair<float, float> MixedFilamentManager::surface_offset_pair_from_signed_bias(float bias_mm, float reference_width_mm)
{
    const float clamped_bias = std::clamp(bias_mm, -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm));
    if (clamped_bias > EPSILON)
        return std::make_pair(0.f, clamped_bias);
    if (clamped_bias < -EPSILON)
        return std::make_pair(-clamped_bias, 0.f);
    return std::make_pair(0.f, 0.f);
}

float MixedFilamentManager::bias_ui_value_from_surface_offsets(float component_a_surface_offset,
                                                               float component_b_surface_offset,
                                                               float reference_width_mm)
{
    return std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                      -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm));
}

int MixedFilamentManager::apparent_mix_b_percent(int   mix_b_percent,
                                                 float component_a_surface_offset,
                                                 float component_b_surface_offset,
                                                 float reference_width_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(reference_width_mm));
    const float shift_pct      = -100.f *
                            std::clamp(canonical_signed_bias_value(component_a_surface_offset, component_b_surface_offset),
                                       -max_pair_bias_mm(reference_width_mm), max_pair_bias_mm(reference_width_mm)) /
                            safe_reference;
    return clamp_int(int(std::lround(float(clamp_int(mix_b_percent, 0, 100)) + shift_pct)), 0, 100);
}

double MixedFilamentManager::mixed_filament_reference_nozzle_mm(unsigned int               component_a,
                                                                unsigned int               component_b,
                                                                const std::vector<double>& nozzle_diameters)
{
    std::vector<double> samples;
    samples.reserve(2);

    auto append_if_valid = [&samples, &nozzle_diameters](unsigned int component_id) {
        if (component_id >= 1 && component_id <= nozzle_diameters.size())
            samples.emplace_back(std::max(0.05, nozzle_diameters[size_t(component_id - 1)]));
    };

    append_if_valid(component_a);
    append_if_valid(component_b);

    if (samples.empty())
        return 0.4;
    return std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}
std::string compute_mixed_filament_display_color(const MixedFilamentDefinition& definition, const MixedFilamentDisplayContext& context)
{
    return compute_mixed_filament_display_color(mixed_filament_legacy_row_from_definition(definition), context);
}

} // namespace Slic3r
