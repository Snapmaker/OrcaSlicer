#include <catch2/catch_test_macros.hpp>

#include "libslic3r/MixedFilament/Recommendations.hpp"
#include "libslic3r/MixedFilament/Serialization.hpp"
#include "libslic3r/MixedFilamentColorPrediction.hpp"
#include "libslic3r/MixedFilamentConfigRemap.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <chrono>
#include <cstdio>
#include <set>

using namespace Slic3r;

namespace {
MixedFilamentDisplayContext palette(std::vector<std::string> colors)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = colors.size();
    context.physical_colors = std::move(colors);
    context.compatible_filaments.assign(context.num_physical, std::vector<bool>(context.num_physical, true));
    return context;
}

MixedFilamentDefinition blend(uint64_t id, std::vector<MixedFilamentWeightedComponent> components)
{
    MixedFilamentDefinition definition;
    definition.identity.stable_id      = id;
    definition.source.kind             = MixedFilamentSourceKind::Custom;
    definition.recipe.blend.components = std::move(components);
    return definition;
}

using Key = std::vector<std::pair<unsigned int, int>>;
Key key(const MixedFilamentRecommendation& item)
{
    Key result;
    for (const auto& component : item.definition.recipe.blend.components)
        result.emplace_back(component.filament.id, component.percent);
    return result;
}
} // namespace

TEST_CASE("Recommendations implement all PRD ratios with physical slot identities", "[MixedManage]")
{
    const auto                   context = palette({"#008BB3", "#AD4A76", "#EBBE00"});
    MixedFilamentRecommendations catalog(context, {"PLA", "PLA", "PLA"}, {});
    REQUIRE(catalog.recommendations.size() == 13);
    std::set<Key> expected = {{{1, 50}, {2, 50}},          {{1, 33}, {2, 67}},          {{1, 67}, {2, 33}},
                              {{1, 50}, {3, 50}},          {{1, 33}, {3, 67}},          {{1, 67}, {3, 33}},
                              {{2, 50}, {3, 50}},          {{2, 33}, {3, 67}},          {{2, 67}, {3, 33}},
                              {{1, 33}, {2, 33}, {3, 34}}, {{1, 50}, {2, 25}, {3, 25}}, {{1, 25}, {2, 50}, {3, 25}},
                              {{1, 25}, {2, 25}, {3, 50}}};
    for (const auto& item : catalog.recommendations) {
        CHECK(expected.erase(key(item)) == 1);
        CHECK_FALSE(item.registered());
    }
    CHECK(expected.empty());
}

TEST_CASE("Recommendations filter compatibility and deduplicate by color plus material type", "[MixedManage]")
{
    auto context = palette({"#ff0000", "#FF0000", "#00FF00", "#FF0000"});
    for (size_t i = 0; i < 3; ++i)
        context.compatible_filaments[i][3] = context.compatible_filaments[3][i] = false;
    const auto                   original = blend(19, {{{2}, 50}, {{3}, 50}});
    MixedFilamentRecommendations catalog(context, {"PLA", " pla ", "PLA", "PETG"}, {original});
    CHECK(catalog.representative_count == 3);
    CHECK(catalog.excluded_incompatible);
    REQUIRE(catalog.recommendations.size() == 3);
    for (const auto& item : catalog.recommendations) {
        CHECK(item.definition.recipe.blend.components[0].filament.id == 1);
        CHECK(item.definition.recipe.blend.components[1].filament.id == 3);
    }
    REQUIRE(catalog.saved.size() == 1);
    CHECK(catalog.saved[0].definition.recipe.blend.components[0].filament.id == 2);

    SECTION("All combinations incompatible")
    {
        context.compatible_filaments[0][2] = context.compatible_filaments[2][0] = false;
        MixedFilamentRecommendations empty(context, {"PLA", "PLA", "ABS", "PETG"}, {});
        CHECK(empty.excluded_incompatible);
        CHECK(empty.recommendations.empty());
    }
    SECTION("Only one representative")
    {
        MixedFilamentRecommendations empty(palette({"#ff0000", "#FF0000"}), {"PLA", "PLA"}, {});
        CHECK(empty.representative_count == 1);
        CHECK(empty.recommendations.empty());
        CHECK_FALSE(empty.excluded_incompatible);
    }
    SECTION("Same RGB with different compatible types is retained")
    {
        MixedFilamentRecommendations two(palette({"#ff0000", "#FF0000"}), {"PLA", "PLA+"}, {});
        CHECK(two.representative_count == 2);
        CHECK(two.recommendations.size() == 3);
    }
}

TEST_CASE("Manage color sorting uses PRD HSL families with deterministic ties", "[MixedManage]")
{
    const std::vector<std::string> ordered = {"#FF8080", "#FF0000", "#800000", "#FF8000", "#FFFF00", "#00FF00", "#00FFFF",
                                              "#0000FF", "#8000FF", "#FF00FF", "#FFFFFF", "#808080", "#000000"};
    for (size_t i = 1; i < ordered.size(); ++i)
        CHECK(mixed_color_sort_key(ordered[i - 1]) < mixed_color_sort_key(ordered[i]));
    CHECK(mixed_color_sort_key("#ff0000") == mixed_color_sort_key("#FF0000"));
    // All have L=50%; hue precedes saturation, then stronger saturation first.
    CHECK(mixed_color_sort_key("#00AAFF") < mixed_color_sort_key("#3399CC"));
    CHECK(mixed_color_sort_key("#3399CC") < mixed_color_sort_key("#2680D9"));
}

TEST_CASE("Selection only stages unregistered recommendations and leaves saved recipes alone", "[MixedManage]")
{
    const auto context                 = palette({"#FF0000", "#00FF00", "#0000FF"});
    auto       ordinary                = blend(10, {{{1}, 50}, {{2}, 50}});
    auto       gradient                = ordinary;
    gradient.identity.stable_id        = 11;
    gradient.behavior.gradient.enabled = true;
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({ordinary, gradient}, context.physical_colors);
    const auto                   before = manager.serialize_custom_entries();
    MixedFilamentRecommendations catalog(context, {"PLA", "PLA", "PLA"}, manager.mixed_filament_definitions());
    REQUIRE(catalog.saved.size() == 2);
    CHECK(catalog.saved[0].filament_id == 4);
    CHECK(catalog.saved[1].filament_id == 5);
    CHECK(catalog.selection_state() == 0);
    catalog.select_all(true, 2);
    CHECK(catalog.additions() == 8);
    CHECK(catalog.selection_state() == 1);
    CHECK(catalog.selection_state(2) == 2);
    catalog.saved[0].removed = true;
    catalog.select_all(true);
    CHECK(catalog.additions() == 12);
    CHECK(catalog.selection_state() == 2);
    catalog.select_all(false);
    CHECK(catalog.additions() == 0);
    CHECK(catalog.removals() == 1);
    CHECK(catalog.saved[0].filament_id == 4);
    CHECK(manager.serialize_custom_entries() == before);
    const auto result = catalog.prepare(manager);
    CHECK(manager.serialize_custom_entries() == before);
    CHECK(result.registered_count() == 1);
    CHECK(result.filament_id_from_stable_id(11, 3).value() == 4);
    CHECK_FALSE(result.filament_id_from_stable_id(10, 3).has_value());
    CHECK(result.mixed_filament_definitions()[1].behavior.gradient.enabled);
}

TEST_CASE("Manage supports the full 2600-candidate palette and 256 saved mixes", "[MixedManage]")
{
    std::vector<std::string> colors;
    for (unsigned int i = 0; i < 16; ++i) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", i * 16, 255 - i * 16, i * 8);
        colors.emplace_back(hex);
    }
    auto                         context = palette(colors);
    const auto                   start   = std::chrono::steady_clock::now();
    MixedFilamentRecommendations catalog(context, std::vector<std::string>(16, "PLA"), {});
    INFO("Generation took " << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " seconds");
    REQUIRE(catalog.recommendations.size() == 2600);
    size_t pairs = 0;
    for (size_t i = 0; i < catalog.recommendations.size(); ++i) {
        pairs += catalog.recommendations[i].definition.recipe.blend.components.size() == 2;
        if (i)
            CHECK(mixed_color_sort_key(catalog.recommendations[i - 1].definition.presentation.display_color) <=
                  mixed_color_sort_key(catalog.recommendations[i].definition.presentation.display_color));
    }
    CHECK(pairs == 360);
    const auto initial_first = key(catalog.recommendations.front());
    catalog.select_all(true);
    CHECK(catalog.exceeds_limit());
    MixedFilamentManager manager;
    CHECK_THROWS_AS(catalog.prepare(manager), std::length_error);
    CHECK(manager.registered_count() == 0);
    catalog.select_all(false);
    for (size_t i = 0; i < 256; ++i)
        catalog.recommendations[i].selected = true;
    CHECK_FALSE(catalog.exceeds_limit());
    auto candidate = catalog.prepare(manager);
    CHECK(candidate.registered_count() == 256);
    CHECK(candidate.total_filaments(16) == 272);
    CHECK_FALSE(candidate.add_custom_filament_definition(blend(0, {{{1}, 50}, {{2}, 50}}), colors));
    const auto json = serialize_mixed_filaments_json(candidate.mixed_filament_definitions(), 16);
    CHECK(deserialize_mixed_filaments_json(json, 16).size() == 256);
    CHECK(key(catalog.recommendations.front()) == initial_first);
    catalog.recommendations[256].selected = true;
    CHECK(catalog.exceeds_limit());
    CHECK_THROWS_AS(catalog.prepare(manager), std::length_error);
    CHECK(manager.registered_count() == 0);

    MixedFilamentRecommendations edit(context, std::vector<std::string>(16, "PLA"), candidate.mixed_filament_definitions());
    auto                         unregistered = std::find_if(edit.recommendations.begin(), edit.recommendations.end(),
                                                             [](const auto& item) { return !item.registered(); });
    REQUIRE(unregistered != edit.recommendations.end());
    unregistered->selected = true;
    CHECK(edit.exceeds_limit());
    edit.saved.front().removed = true;
    CHECK_FALSE(edit.exceeds_limit());
    CHECK(edit.prepare(candidate).registered_count() == 256);
}

TEST_CASE("Manage previews use calibration identities and FilamentMixer fallback", "[MixedManage]")
{
    auto                           context = palette({"#FFFFFF", "#AD4A76"});
    const std::vector<std::string> ids =
        {"panchromasnapseed_w6_1_c5_8_m5_2_y4_5_sc46_lh0p08_sd10_00a0cc_c34c7e_ffb717_e4e5e1_20260625_164811_523__slot_4__td_6.1",
         "panchroma_g6_8y9_7m5_0c6_4_sc185_lh0p08_sd10p8_008bb3_ad4a76_ebbe00_7b7f80_20260612_192835_266__slot_2__td_5"};
    const auto old_engine = MixedFilamentManager::color_engine();
    context.color_engine  = MixedFilamentColorEngine::FilamentMixer; // Manage has an explicit automatic policy.
    SECTION("Identified materials") { context.physical_material_ids = ids; }
    SECTION("Uncalibrated materials") { context.physical_material_ids = {"unrecognized:a", "unrecognized:b"}; }
    SECTION("One unidentified component falls back for the recipe") { context.physical_material_ids = {ids[0], "unrecognized:b"}; }
    MixedFilamentRecommendations catalog(context, {"PLA", "PLA"}, {});
    for (const auto& item : catalog.recommendations) {
        std::vector<MixedFilamentColorInput> inputs;
        for (const auto& component : item.definition.recipe.blend.components) {
            const auto slot = component.filament.id - 1;
            inputs.push_back({context.physical_colors[slot], component.percent, std::nullopt, context.physical_material_ids[slot]});
        }
        const bool calibrated = context.physical_material_ids == ids;
        CHECK(item.definition.presentation.display_color ==
              MixedFilamentManager::blend_color_multi(inputs, calibrated ? MixedFilamentColorEngine::FullSpectrumKSPairResidual :
                                                                           MixedFilamentColorEngine::FilamentMixer));
    }
    CHECK(MixedFilamentManager::color_engine() == old_engine);
}

TEST_CASE("Recommendation loading rejects incomplete or oversized palettes", "[MixedManage]")
{
    CHECK_THROWS(MixedFilamentRecommendations(palette({"#FF0000"}), {"PLA"}, {}));
    CHECK_THROWS(MixedFilamentRecommendations(palette(std::vector<std::string>(17, "#FF0000")), std::vector<std::string>(17, "PLA"), {}));
    auto context = palette({"#FF0000", "#0000FF"});
    CHECK_THROWS(MixedFilamentRecommendations(context, {"PLA"}, {}));
    context.compatible_filaments.clear();
    CHECK_THROWS(MixedFilamentRecommendations(context, {"PLA", "PLA"}, {}));
}

TEST_CASE("Removing edited generated mixes survives regeneration and legacy reload", "[MixedManage]")
{
    const auto context        = palette({"#FF0000", "#00FF00", "#0000FF"});
    auto       first          = blend(17, {{{1}, 33}, {{2}, 67}});
    first.source.origin_auto  = true;
    auto second               = first;
    second.identity.stable_id = 18;
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions({first, second}, context.physical_colors);
    MixedFilamentRecommendations catalog(context, {"PLA", "PLA", "PLA"}, manager.mixed_filament_definitions());
    catalog.saved[0].removed = true;
    catalog.saved[1].removed = true;
    auto result              = catalog.prepare(manager);
    CHECK(result.registered_count() == 0);
    const auto serialized = result.serialize_custom_entries();
    const bool old_auto   = MixedFilamentManager::auto_generate_enabled();
    MixedFilamentManager::set_auto_generate_enabled(true);
    result.auto_generate(context.physical_colors);
    result.clear_custom_entries();
    result.load_custom_entries(serialized, context.physical_colors);
    MixedFilamentManager::set_auto_generate_enabled(old_auto);
    CHECK(result.registered_count() == 2); // Only the other two pairs may be generated.
    CHECK_FALSE(result.filament_id_from_stable_id(17, 3));
    for (const auto& definition : result.mixed_filament_definitions())
        if (!definition.visibility.tombstoned) {
            const auto ids = definition.recipe.blend.component_ids();
            CHECK(std::find(ids.begin(), ids.end(), 3u) != ids.end());
        }
}

TEST_CASE("Disabled recipes count toward the mixed limit and auto generation cannot overfill it", "[MixedManage]")
{
    const std::vector<std::string>       colors(16, "#808080");
    std::vector<MixedFilamentDefinition> definitions(256);
    for (size_t i = 0; i < definitions.size(); ++i) {
        definitions[i]                    = blend(i + 1, {{{1}, 50}, {{2}, 50}});
        definitions[i].visibility.enabled = false;
    }
    MixedFilamentManager manager;
    manager.set_mixed_filament_definitions(definitions, colors);
    CHECK(manager.registered_count() == 256);
    CHECK(manager.enabled_count() == 0);
    CHECK_FALSE(manager.add_custom_filament_definition(blend(0, {{{1}, 50}, {{2}, 50}}), colors));
    const bool old_auto = MixedFilamentManager::auto_generate_enabled();
    MixedFilamentManager::set_auto_generate_enabled(true);
    manager.auto_generate(colors);
    MixedFilamentManager::set_auto_generate_enabled(old_auto);
    CHECK(manager.registered_count() == 256);
    CHECK(manager.enabled_count() == 0);
}
