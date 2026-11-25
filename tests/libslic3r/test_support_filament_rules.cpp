#include <catch2/catch_test_macros.hpp>

#include "libslic3r/SupportFilamentRules.hpp"

using namespace Slic3r;

TEST_CASE("Support filament rules merge defaults with material overrides", "[Config][SupportFilament]")
{
    const std::vector<std::string> default_recommendations = {
        "support_top_z_distance=0",
        "support_interface_pattern=rectilinear_interlaced",
        "support_interface_spacing=0",
        "independent_support_layer_height=false"
    };
    const std::vector<std::string> support_pla_recommendations = {
        "support_interface_top_layers=4",
        "support_object_xy_distance=0.3"
    };

    const SupportFilamentRules rules =
        SupportFilamentRules::from_lists(default_recommendations, support_pla_recommendations);

    SECTION("other support materials inherit the default four recommendations")
    {
        const SupportParameterRule* rule = rules.rule(SupportFilamentKind::OtherSupport);
        REQUIRE(rule != nullptr);
        REQUIRE(rule->recommendations.size() == 4);
        CHECK(rule->recommendations.at("support_top_z_distance") == "0");
        CHECK(rule->recommendations.at("support_interface_spacing") == "0");
        CHECK(rule->recommendations.at("independent_support_layer_height") == "0");
        REQUIRE(rule->restore_check_keys.size() == 4);
    }

    SECTION("PVA inherits the defaults and only adds guide metadata")
    {
        const SupportParameterRule* rule = rules.rule(SupportFilamentKind::Pva);
        REQUIRE(rule != nullptr);
        REQUIRE(rule->id == "pva");
        REQUIRE(rule->recommendations.size() == 4);
        REQUIRE(rule->restore_check_keys.size() == 4);
    }

    SECTION("Support PLA merges defaults with its two overrides")
    {
        const SupportParameterRule* rule = rules.rule(SupportFilamentKind::SupportPla);
        REQUIRE(rule != nullptr);
        REQUIRE(rule->id == "support_pla");
        REQUIRE(rule->recommendations.size() == 6);
        CHECK(rule->recommendations.at("support_interface_top_layers") == "4");
        CHECK(rule->recommendations.at("support_object_xy_distance") == "0.3");
        CHECK(rule->recommendations.at("support_interface_pattern") == "rectilinear_interlaced");
        REQUIRE(rule->restore_check_keys.size() == 6);
    }
}

TEST_CASE("Support filament rules provide a fallback when no materials are configured", "[Config][SupportFilament]")
{
    const SupportFilamentRules rules = SupportFilamentRules::from_lists({"support_top_z_distance=0"}, {});
    CHECK(rules.loaded());

    const SupportParameterRule* rule = rules.rule(SupportFilamentKind::OtherSupport);
    REQUIRE(rule != nullptr);
    CHECK(rule->id == "default");
    REQUIRE(rule->recommendations.size() == 1);
    CHECK(rule->recommendations.at("support_top_z_distance") == "0");
}

TEST_CASE("Support filament rules reject non-process options", "[Config][SupportFilament]")
{
    const SupportFilamentRules rules = SupportFilamentRules::from_lists(
        {"print_settings_id=bad", "printer_settings_id=bad", "filament_type=PLA", "support_top_z_distance=0"},
        {"print_settings_id=bad", "support_interface_top_layers=4"});

    REQUIRE(rules.loaded());

    const SupportParameterRule* default_rule = rules.rule(SupportFilamentKind::OtherSupport);
    REQUIRE(default_rule != nullptr);
    REQUIRE(default_rule->recommendations.size() == 1);
    CHECK(default_rule->recommendations.at("support_top_z_distance") == "0");
    REQUIRE(default_rule->restore_check_keys.size() == 1);
    CHECK(default_rule->restore_check_keys.front() == "support_top_z_distance");

    const SupportParameterRule* pla_rule = rules.rule(SupportFilamentKind::SupportPla);
    REQUIRE(pla_rule != nullptr);
    REQUIRE(pla_rule->recommendations.size() == 2);
    REQUIRE(pla_rule->restore_check_keys.size() == 2);
    CHECK(pla_rule->recommendations.at("support_interface_top_layers") == "4");
}

TEST_CASE("Support filament rules are not loaded when the allow-list section is missing", "[Config][SupportFilament]")
{
    const SupportFilamentRules rules = SupportFilamentRules::from_lists({}, {});
    CHECK_FALSE(rules.loaded());
    CHECK(rules.rule(SupportFilamentKind::OtherSupport) == nullptr);
}

TEST_CASE("Support filament kind is resolved from effective filament config", "[Config][SupportFilament]")
{
    DynamicPrintConfig config                                             = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values     = {"PLA"};
    config.option<ConfigOptionBools>("filament_is_support", true)->values = {false};
    CHECK(classify_support_filament(config) == SupportFilamentKind::None);

    config.option<ConfigOptionBools>("filament_is_support", true)->values = {true};
    CHECK(classify_support_filament(config) == SupportFilamentKind::SupportPla);

    config.option<ConfigOptionStrings>("filament_type", true)->values = {"PVA"};
    CHECK(classify_support_filament(config) == SupportFilamentKind::Pva);

    config.option<ConfigOptionStrings>("filament_type", true)->values = {"PETG"};
    CHECK(classify_support_filament(config) == SupportFilamentKind::OtherSupport);

    config.option<ConfigOptionStrings>("filament_type", true)->values = {" pla "};
    CHECK(classify_support_filament(config) == SupportFilamentKind::SupportPla);

    config.option<ConfigOptionStrings>("filament_type", true)->values = {"PLA", "PVA"};
    config.option<ConfigOptionBools>("filament_is_support", true)->values = {false, true};
    CHECK(classify_support_filament(config, 0) == SupportFilamentKind::None);
    CHECK(classify_support_filament(config, 1) == SupportFilamentKind::Pva);
}

TEST_CASE("Support recommendation diffs only return changed values", "[Config][SupportFilament]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("support_top_z_distance", new ConfigOptionFloat(0));
    config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0));
    config.set_key_value("independent_support_layer_height", new ConfigOptionBool(false));

    const std::map<std::string, std::string> recommendations = {{"support_top_z_distance", "0"},
                                                                {"support_interface_top_layers", "4"},
                                                                {"support_interface_pattern", "rectilinear_interlaced"},
                                                                {"support_interface_spacing", "0"},
                                                                {"support_object_xy_distance", "0.3"},
                                                                {"independent_support_layer_height", "false"}};

    const std::map<std::string, std::string> diff = support_recommendation_diff(config, recommendations);

    REQUIRE(diff.size() == 3);
    CHECK(diff.at("support_interface_top_layers") == "4");
    CHECK(diff.at("support_interface_pattern") == "rectilinear_interlaced");
    CHECK(diff.at("support_object_xy_distance") == "0.3");
    CHECK(diff.find("independent_support_layer_height") == diff.end());
}

TEST_CASE("Support restore diffs use the selected process preset", "[Config][SupportFilament]")
{
    const std::vector<std::string> keys = {"support_top_z_distance",    "support_interface_top_layers", "support_interface_pattern",
                                           "support_interface_spacing", "support_object_xy_distance",   "independent_support_layer_height"};

    DynamicPrintConfig selected = DynamicPrintConfig::full_print_config();
    selected.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.2));
    selected.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    selected.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.5));
    selected.set_key_value("support_object_xy_distance", new ConfigOptionFloat(0.35));

    DynamicPrintConfig current = selected;
    current.set_key_value("support_top_z_distance", new ConfigOptionFloat(0));
    current.set_key_value("support_interface_top_layers", new ConfigOptionInt(4));
    current.set_key_value("support_object_xy_distance", new ConfigOptionFloat(0.3));

    const std::map<std::string, std::string> diff = support_restore_default_diff(current, selected, keys);

    REQUIRE(diff.size() == 3);
    CHECK(diff.at("support_top_z_distance") == "0.2");
    CHECK(diff.at("support_interface_top_layers") == "2");
    CHECK(diff.at("support_object_xy_distance") == "0.35");
}

TEST_CASE("Support guide choices are tracked per material kind", "[Config][SupportFilament]")
{
    CHECK(support_guide_choice_bit(SupportFilamentKind::SupportPla) == 1);
    CHECK(support_guide_choice_bit(SupportFilamentKind::Pva) == 2);
    CHECK(support_guide_choice_bit(SupportFilamentKind::OtherSupport) == 4);

    CHECK_FALSE(support_guide_choice_hidden(0, SupportFilamentKind::SupportPla));
    CHECK(support_guide_choice_hidden(1, SupportFilamentKind::SupportPla));
    CHECK_FALSE(support_guide_choice_hidden(1, SupportFilamentKind::Pva));
    CHECK(support_guide_choice_hidden(3, SupportFilamentKind::Pva));
    CHECK_FALSE(support_guide_choice_hidden(0, SupportFilamentKind::OtherSupport));
    CHECK(support_guide_choice_hidden(4, SupportFilamentKind::OtherSupport));
}

TEST_CASE("Missing support guide choice values default to show", "[Config][SupportFilament]")
{
    CHECK(support_guide_choice_from_string("") == 0);
    CHECK(support_guide_choice_from_string("invalid") == 0);
    CHECK(support_guide_choice_from_string("3") == 3);
}

TEST_CASE("Support materials with different filament types are incompatible", "[Config][SupportFilament]")
{
    CHECK(support_filament_types_compatible("PVA", "PVA"));
    CHECK(support_filament_types_compatible("PLA", " pla "));
    CHECK_FALSE(support_filament_types_compatible("PVA", "BVOH"));
    CHECK_FALSE(support_filament_types_compatible("PLA", "PVA"));
}

TEST_CASE("Support filament guide URLs follow the active language", "[Config][SupportFilament]")
{
    CHECK(support_filament_guide_url(SupportFilamentKind::SupportPla, "zh_CN")
          == "https://wiki.snapmaker.com/en/FAQ/Modules_Accessories_Materials/Support_For_PLA_Filament");
    CHECK(support_filament_guide_url(SupportFilamentKind::SupportPla, "en_US")
          == "https://wiki.snapmaker.com/en/FAQ/Modules_Accessories_Materials/Support_For_PLA_Filament");
    CHECK(support_filament_guide_url(SupportFilamentKind::Pva, "zh_TW")
          == "https://wiki.snapmaker.com/zh/general/manual/dissolving_PVA");
    CHECK(support_filament_guide_url(SupportFilamentKind::Pva, "en_US")
          == "https://wiki.snapmaker.com/en/snapmaker_artisan/troubleshooting/pva_tpu_breakaway_support");
    CHECK(support_filament_guide_url(SupportFilamentKind::OtherSupport, "en_US").empty());
}
