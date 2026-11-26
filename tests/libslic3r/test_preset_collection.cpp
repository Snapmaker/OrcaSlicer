#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"

using namespace Slic3r;

TEST_CASE("PresetCollection::get_config_for_preset prefers the edited config", "[presets]")
{
    PresetCollection collection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                                static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()), "Default Filament");

    DynamicPrintConfig cfg = collection.get_edited_preset().config;
    cfg.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    cfg.set_key_value("filament_is_support", new ConfigOptionBools{true});
    collection.load_preset("test", "Support PLA", cfg, true);

    const Preset& edited = collection.get_edited_preset();
    REQUIRE(&collection.get_config_for_preset(edited) == &edited.config);

    Preset other(Preset::TYPE_FILAMENT, "Other", false);
    other.config = edited.config;
    other.config.set_key_value("filament_is_support", new ConfigOptionBools{false});
    REQUIRE(&collection.get_config_for_preset(other) == &other.config);
}
