#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("deep_diff distinguishes absolute and percentage speeds for each variant", "[PresetDiff][Config]")
{
    const size_t changed_index = GENERATE(size_t(0), size_t(1));
    Preset       reference(Preset::TYPE_PRINT, "ref");
    reference.config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{50., false}, {50., false}});

    Preset edited = reference;
    edited.config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed")->values[changed_index].percent = true;

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    REQUIRE(diff == std::vector<std::string>{"small_perimeter_speed#" + std::to_string(changed_index)});

    DynamicPrintConfig transferred = reference.config;
    transferred.apply_only(edited.config, diff);
    REQUIRE(*transferred.option("small_perimeter_speed") == *edited.config.option("small_perimeter_speed"));
}

// Orca #16046 / Edge: there is no update_diff_values_to_child_config. Preset::normalize
// resizes a short user vector by duplicating values.front() (Config.hpp, not back()).
// A child that saved a single nozzle_temperature therefore keeps that value on both
// Standard and High-Flow columns instead of refilling the unlisted variant from the parent.
TEST_CASE("normalize keeps a user nozzle_temperature across unlisted flow variants",
          "[PresetDiff][FilamentVariants]")
{
    DynamicPrintConfig child;
    child.set_key_value("filament_diameter", new ConfigOptionFloats{1.75});
    child.set_key_value("filament_flow_support",
                        new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    child.set_key_value("nozzle_temperature", new ConfigOptionInts{199});

    Preset::normalize(child);
    const auto *temps = child.option<ConfigOptionInts>("nozzle_temperature");
    REQUIRE(temps != nullptr);
    REQUIRE(temps->values == std::vector<int>{199, 199});

    DynamicPrintConfig both;
    both.set_key_value("filament_diameter", new ConfigOptionFloats{1.75});
    both.set_key_value("filament_flow_support",
                       new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    both.set_key_value("nozzle_temperature", new ConfigOptionInts{199, 231});

    Preset::normalize(both);
    const auto *kept = both.option<ConfigOptionInts>("nozzle_temperature");
    REQUIRE(kept != nullptr);
    REQUIRE(kept->values == std::vector<int>{199, 231});
}
