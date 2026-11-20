#include "Recommendations.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace {
std::string normalized_hex(std::string hex)
{
    if (hex.size() != 7 || hex.front() != '#' || !std::all_of(hex.begin() + 1, hex.end(), [](unsigned char c) { return std::isxdigit(c); }))
        throw std::invalid_argument("Invalid physical filament color");
    std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return hex;
}

std::string normalized_type(std::string type)
{
    const auto first = type.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    type = type.substr(first, type.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return type;
}

using RecipeKey = std::vector<std::pair<unsigned int, int>>;
RecipeKey recipe_key(const MixedFilamentDefinition& definition)
{
    // A gradient or explicit pattern is not the same saved filament as a flat
    // recommendation, even when its aggregate weights happen to match.
    if (definition.recipe.kind != MixedFilamentRecipeKind::WeightedBlend || definition.behavior.gradient.enabled ||
        definition.behavior.surface_bias.component_a_offset_mm != 0.f || definition.behavior.surface_bias.component_b_offset_mm != 0.f)
        return {};
    RecipeKey key;
    for (const auto& component : definition.recipe.blend.components)
        key.emplace_back(component.filament.id, component.percent);
    std::sort(key.begin(), key.end());
    return key;
}
} // namespace

MixedColorSortKey mixed_color_sort_key(const std::string& hex)
{
    const std::string color = normalized_hex(hex);
    const double      r     = std::stoi(color.substr(1, 2), nullptr, 16) / 255.;
    const double      g     = std::stoi(color.substr(3, 2), nullptr, 16) / 255.;
    const double      b     = std::stoi(color.substr(5, 2), nullptr, 16) / 255.;
    const double      hi = std::max({r, g, b}), lo = std::min({r, g, b}), delta = hi - lo;
    const double      lightness  = (hi + lo) / 2.;
    const double      saturation = delta == 0. ? 0. : delta / (1. - std::abs(2. * lightness - 1.));
    double            hue        = 0.;
    if (delta != 0.) {
        hue = hi == r ? (g - b) / delta : (hi == g ? (b - r) / delta + 2. : (r - g) / delta + 4.);
        hue *= 60.;
        if (hue < 0.)
            hue += 360.;
    }
    const int family = saturation < .1 ? 8 :
                                         (hue < 15. || hue >= 345. ? 0 :
                                          hue < 45.                ? 1 :
                                          hue < 75.                ? 2 :
                                          hue < 165.               ? 3 :
                                          hue < 195.               ? 4 :
                                          hue < 255.               ? 5 :
                                          hue < 285.               ? 6 :
                                                                     7);
    return {family, -lightness, hue, -saturation, color};
}

MixedFilamentRecommendations::MixedFilamentRecommendations(const MixedFilamentDisplayContext&          context,
                                                           const std::vector<std::string>&             material_types,
                                                           const std::vector<MixedFilamentDefinition>& definitions)
    : m_context(context), m_definitions(definitions)
{
    m_context.color_engine = MixedFilamentColorEngine::FullSpectrumKSPairResidual;
    const size_t count     = context.physical_colors.size();
    if (count < 2 || count > MAX_PHYSICAL_FILAMENTS || context.num_physical != count || material_types.size() != count ||
        context.compatible_filaments.size() != count)
        throw std::invalid_argument("Invalid recommendation palette");
    for (const auto& row : context.compatible_filaments)
        if (row.size() != count)
            throw std::invalid_argument("Invalid compatibility matrix");

    unsigned int                             virtual_id = unsigned(count);
    std::map<RecipeKey, std::vector<size_t>> registered;
    for (const auto& definition : definitions) {
        if (definition.visibility.tombstoned)
            continue;
        const unsigned int id  = definition.visibility.enabled ? ++virtual_id : 0;
        const auto         key = recipe_key(definition);
        if (!key.empty())
            registered[key].push_back(saved.size());
        saved.push_back({definition, id, false});
    }

    std::set<std::pair<std::string, std::string>> seen;
    std::vector<unsigned int>                     representatives;
    for (size_t i = 0; i < count; ++i)
        if (seen.emplace(normalized_hex(context.physical_colors[i]), normalized_type(material_types[i])).second)
            representatives.push_back(unsigned(i + 1));
    representative_count = representatives.size();

    auto compatible = [&](const std::vector<unsigned int>& ids) {
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (!context.compatible_filaments[ids[i] - 1][ids[j] - 1]) {
                    excluded_incompatible = true;
                    return false;
                }
        return true;
    };
    auto add = [&](const std::vector<unsigned int>& ids, const std::vector<int>& weights) {
        MixedFilamentRecommendation item;
        auto&                       definition = item.definition;
        definition.source.kind                 = MixedFilamentSourceKind::Custom;
        definition.behavior.distribution       = ids.size() == 2 ? MixedFilamentDistributionMode::Simple :
                                                                   MixedFilamentDistributionMode::LayerCycle;
        definition.presentation.ui_mode        = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            definition.recipe.blend.components.push_back({{ids[i]}, weights[i]});
        }
        definition.presentation.display_color = blend_mixed_components(ids, weights, m_context);
        const auto found                      = registered.find(recipe_key(definition));
        if (found != registered.end())
            item.saved_indices = found->second;
        recommendations.push_back(std::move(item));
    };
    for (size_t i = 0; i < representatives.size(); ++i)
        for (size_t j = i + 1; j < representatives.size(); ++j) {
            const std::vector<unsigned int> pair{representatives[i], representatives[j]};
            if (compatible(pair)) {
                add(pair, {50, 50});
                add(pair, {33, 67});
                add(pair, {67, 33});
            }
            for (size_t k = j + 1; k < representatives.size(); ++k) {
                const std::vector<unsigned int> triple{representatives[i], representatives[j], representatives[k]};
                if (compatible(triple)) {
                    add(triple, {33, 33, 34});
                    add(triple, {50, 25, 25});
                    add(triple, {25, 50, 25});
                    add(triple, {25, 25, 50});
                }
            }
        }
    // Precompute keys: the comparator never reparses colors, and equal colors
    // keep deterministic recipe-generation order.
    std::vector<std::pair<MixedColorSortKey, size_t>> order;
    for (size_t i = 0; i < recommendations.size(); ++i)
        order.emplace_back(mixed_color_sort_key(recommendations[i].definition.presentation.display_color), i);
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto unsorted = std::move(recommendations);
    for (const auto& entry : order)
        recommendations.push_back(std::move(unsorted[entry.second]));
}

size_t MixedFilamentRecommendations::additions(size_t component_count) const
{
    return std::count_if(recommendations.begin(), recommendations.end(), [=](const auto& item) {
        return !item.registered() && item.selected &&
               (component_count == 0 || item.definition.recipe.blend.components.size() == component_count);
    });
}

size_t MixedFilamentRecommendations::removals() const
{
    return std::count_if(saved.begin(), saved.end(), [](const auto& item) { return item.removed; });
}

int MixedFilamentRecommendations::selection_state(size_t component_count) const
{
    size_t available = 0;
    for (const auto& item : recommendations)
        if (!item.registered() && (component_count == 0 || item.definition.recipe.blend.components.size() == component_count))
            ++available;
    const size_t selected = additions(component_count);
    return selected == 0 ? 0 : selected == available ? 2 : 1;
}

void MixedFilamentRecommendations::select_all(bool selected, size_t component_count)
{
    for (auto& item : recommendations)
        if (!item.registered() && (component_count == 0 || item.definition.recipe.blend.components.size() == component_count))
            item.selected = selected;
}

bool MixedFilamentRecommendations::exceeds_limit() const { return saved.size() - removals() + additions() > MAXIMUM_MIXED_FILAMENT_NUMBER; }

MixedFilamentManager MixedFilamentRecommendations::prepare(const MixedFilamentManager& current) const
{
    if (exceeds_limit())
        throw std::length_error("Mixed filament limit exceeded");
    // Preserve disabled rows, tombstones, stable identities and existing order.
    auto                                 definitions = m_definitions;
    std::vector<MixedFilamentDefinition> auto_tombstones;
    for (auto& definition : definitions)
        for (const auto& item : saved)
            if (item.removed && item.definition.identity.stable_id == definition.identity.stable_id) {
                definition.visibility.tombstoned = true;
                definition.visibility.enabled    = false;
                // Edited auto pairs must not reappear as a fresh 50/50 mix on
                // the next regeneration. Retain a tombstone for the base pair.
                if (definition.source.kind == MixedFilamentSourceKind::Custom && definition.source.origin_auto &&
                    definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend && !definition.behavior.gradient.enabled) {
                    const auto pair     = definition.recipe.blend.primary_pair_or();
                    const auto ids      = std::minmax(pair.component_a.id, pair.component_b.id);
                    auto       existing = std::find_if(definitions.begin(), definitions.end(), [&](const auto& other) {
                        const auto other_pair = other.recipe.blend.primary_pair_or();
                        return other.source.kind == MixedFilamentSourceKind::AutoGenerated &&
                               std::minmax(other_pair.component_a.id, other_pair.component_b.id) == ids;
                    });
                    if (existing != definitions.end()) {
                        existing->visibility.tombstoned = true;
                        existing->visibility.enabled    = false;
                    } else if (std::none_of(auto_tombstones.begin(), auto_tombstones.end(), [&](const auto& other) {
                                   const auto other_pair = other.recipe.blend.primary_pair_or();
                                   return std::minmax(other_pair.component_a.id, other_pair.component_b.id) == ids;
                               })) {
                        MixedFilamentDefinition tombstone;
                        tombstone.source.origin_auto      = true;
                        tombstone.recipe.blend.components = {{{ids.first}, 50}, {{ids.second}, 50}};
                        tombstone.visibility.tombstoned   = true;
                        tombstone.visibility.enabled      = false;
                        auto_tombstones.push_back(std::move(tombstone));
                    }
                }
            }
    definitions.insert(definitions.end(), auto_tombstones.begin(), auto_tombstones.end());
    for (const auto& item : recommendations)
        if (!item.registered() && item.selected)
            definitions.push_back(item.definition);
    MixedFilamentManager candidate = current;
    candidate.set_display_context(m_context);
    candidate.set_mixed_filament_definitions(std::move(definitions), m_context.physical_colors);
    return candidate;
}

} // namespace Slic3r
