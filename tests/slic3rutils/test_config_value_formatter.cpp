// Snapmaker Orca: regression tests for the preset diff / unsaved-changes value formatter.
// Presets with different extruder counts compare whole vectors (no "#index" suffix), and
// stale or foreign keys reach the formatter as well; none of that may crash or hide values.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using Slic3r::GUI::get_full_label;
using Slic3r::GUI::get_string_from_enum;
using Slic3r::GUI::get_string_value;

namespace {

std::string text_of(const wxString& value) { return std::string(value.utf8_str()); }

DynamicPrintConfig full_config()
{
    return DynamicPrintConfig::full_print_config();
}

// Enum vectors are ConfigOptionEnumsGeneric, which stores its values as ConfigOptionInts.
std::vector<int>& enum_values(DynamicPrintConfig& config, const std::string& key)
{
    auto* values = dynamic_cast<ConfigOptionInts*>(config.option(key, true));
    REQUIRE(values != nullptr);
    return values->values;
}

} // namespace

TEST_CASE("Formatter joins every element of a vector option compared as a whole", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config = full_config();
    config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = { 0.2, 0.3 };

    CHECK(text_of(get_string_value("extruder_layer_height", config)) == "0.2, 0.3");
    CHECK(text_of(get_string_value("extruder_layer_height#0", config)) == "0.2");
    CHECK(text_of(get_string_value("extruder_layer_height#1", config)) == "0.3");
}

TEST_CASE("Formatter reports an out-of-range element instead of reading past the vector", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config = full_config();
    config.option<ConfigOptionFloats>("extruder_layer_height", true)->values = { 0.2 };
    enum_values(config, "z_hop_types") = { 0 };

    // The other preset has more extruders than this one.
    CHECK(text_of(get_string_value("extruder_layer_height#3", config)) == "Undefined");
    CHECK(text_of(get_string_value("z_hop_types#3", config)) == "Undefined");
    CHECK(text_of(get_string_from_enum("z_hop_types", config, false, 3)) == "Undefined");
}

TEST_CASE("Formatter joins enum vectors and rejects values outside the label list", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config = full_config();
    std::vector<int>& types = enum_values(config, "z_hop_types");
    types = { 0, 1 };

    const std::string first  = text_of(get_string_value("z_hop_types#0", config));
    const std::string second = text_of(get_string_value("z_hop_types#1", config));
    CHECK_FALSE(first.empty());
    CHECK_FALSE(second.empty());
    CHECK(text_of(get_string_value("z_hop_types", config)) == first + ", " + second);

    types = { 9999 };
    CHECK(text_of(get_string_value("z_hop_types#0", config)) == "Undefined");
}

TEST_CASE("Formatter joins FloatsOrPercents vectors instead of printing the serialized blob", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config = full_config();

    const std::string fop_key = "overhang_1_4_speed";
    REQUIRE(config.def()->get(fop_key)->type == coFloatsOrPercents);

    auto* values = dynamic_cast<ConfigOptionVector<FloatOrPercent>*>(config.option(fop_key, true));
    REQUIRE(values != nullptr);
    values->values = { FloatOrPercent{ 50., true }, FloatOrPercent{ 0.4, false } };

    CHECK(text_of(get_string_value(fop_key, config)) == "50%, 0.4");
    CHECK(text_of(get_string_value(fop_key + "#1", config)) == "0.4");
    CHECK(text_of(get_string_value(fop_key + "#5", config)) == "Undefined");
}

TEST_CASE("Formatter survives a key without a definition", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config = full_config();
    CHECK(text_of(get_string_value("no_such_option_key#2", config)) == "N/A");
    CHECK(text_of(get_full_label("no_such_option_key#2", config)) == "N/A");
}

// A Snapmaker filament preset holds two columns, Standard and High Flow. The preset dialogs compare
// such a preset column by column ("key#0", "key#1"), with itself, with a preset of one column and
// with a preset whose Standard value is unset (nil) while the High Flow one is given.
TEST_CASE("Formatter shows both columns of a filament key with Standard and High Flow values", "[ConfigValueFormatter][HighFlow]")
{
    DynamicPrintConfig two_columns = full_config();
    two_columns.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard", "Direct Drive High Flow" };
    two_columns.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = { 22., 40. };
    two_columns.option<ConfigOptionInts>("nozzle_temperature", true)->values = { 215, 220 };
    two_columns.option<ConfigOptionFloats>("pressure_advance", true)->values = { 0.02, 0.024 };
    two_columns.option<ConfigOptionBools>("enable_pressure_advance", true)->values = { 0, 1 };

    SECTION("each column by its index and both when compared as a whole") {
        CHECK(text_of(get_string_value("filament_max_volumetric_speed#0", two_columns)) == "22");
        CHECK(text_of(get_string_value("filament_max_volumetric_speed#1", two_columns)) == "40");
        CHECK(text_of(get_string_value("filament_max_volumetric_speed", two_columns)) == "22, 40");
        CHECK(text_of(get_string_value("nozzle_temperature#1", two_columns)) == "220");
        CHECK(text_of(get_string_value("nozzle_temperature", two_columns)) == "215, 220");
        CHECK(text_of(get_string_value("pressure_advance#1", two_columns)) == "0.024");
        CHECK(text_of(get_string_value("enable_pressure_advance#0", two_columns)) == "false");
        CHECK(text_of(get_string_value("enable_pressure_advance#1", two_columns)) == "true");
        CHECK(text_of(get_string_value("enable_pressure_advance", two_columns)) == "false, true");
        CHECK(text_of(get_string_value("filament_extruder_variant#1", two_columns)) == "Direct Drive High Flow");
    }

    SECTION("the High Flow column of a preset that has none") {
        DynamicPrintConfig one_column = full_config();
        one_column.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "Direct Drive Standard" };
        one_column.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = { 22. };
        one_column.option<ConfigOptionInts>("nozzle_temperature", true)->values = { 215 };
        one_column.option<ConfigOptionBools>("enable_pressure_advance", true)->values = { 0 };

        CHECK(text_of(get_string_value("filament_max_volumetric_speed#1", one_column)) == "Undefined");
        CHECK(text_of(get_string_value("nozzle_temperature#1", one_column)) == "Undefined");
        CHECK(text_of(get_string_value("enable_pressure_advance#1", one_column)) == "Undefined");
        CHECK(text_of(get_string_value("filament_extruder_variant#1", one_column)) == "Undefined");
    }

    SECTION("an override that is unset for Standard and given for High Flow") {
        REQUIRE(two_columns.def()->get("filament_retraction_length")->nullable);
        auto* length = two_columns.option<ConfigOptionFloatsNullable>("filament_retraction_length", true);
        length->values = { ConfigOptionFloatsNullable::nil_value(), 0.8 };
        auto* wipe = two_columns.option<ConfigOptionBoolsNullable>("filament_wipe", true);
        wipe->values = { ConfigOptionBoolsNullable::nil_value(), 1 };
        auto* before_wipe = two_columns.option<ConfigOptionPercentsNullable>("filament_retract_before_wipe", true);
        before_wipe->values = { ConfigOptionPercentsNullable::nil_value(), 70. };
        std::vector<int>& hop_types = enum_values(two_columns, "filament_z_hop_types");
        hop_types = { ConfigOptionInts::nil_value(), 1 };

        CHECK(text_of(get_string_value("filament_retraction_length#0", two_columns)) == "N/A");
        CHECK(text_of(get_string_value("filament_retraction_length#1", two_columns)) == "0.8");
        CHECK(text_of(get_string_value("filament_retraction_length", two_columns)) == "N/A, 0.8");
        CHECK(text_of(get_string_value("filament_retraction_length#2", two_columns)) == "Undefined");
        CHECK(text_of(get_string_value("filament_wipe#0", two_columns)) == "N/A");
        CHECK(text_of(get_string_value("filament_wipe#1", two_columns)) == "true");
        CHECK(text_of(get_string_value("filament_retract_before_wipe#0", two_columns)) == "N/A");
        CHECK(text_of(get_string_value("filament_retract_before_wipe#1", two_columns)) == "70%");
        CHECK(text_of(get_string_value("filament_z_hop_types#0", two_columns)) == "N/A");
        CHECK_FALSE(text_of(get_string_value("filament_z_hop_types#1", two_columns)).empty());
        CHECK(text_of(get_string_value("filament_z_hop_types#1", two_columns)) != "Undefined");
        // The key has a label as long as one column holds a value.
        CHECK(text_of(get_full_label("filament_retraction_length#0", two_columns)) != "N/A");

        length->values = { ConfigOptionFloatsNullable::nil_value(), ConfigOptionFloatsNullable::nil_value() };
        CHECK(text_of(get_string_value("filament_retraction_length", two_columns)) == "N/A, N/A");
    }

    SECTION("a config that holds the plain flavour of a nullable key") {
        // The definition says nullable, the config was put together with the plain class (and the
        // other way round for a key that is not nullable).
        two_columns.set_key_value("filament_retraction_length", new ConfigOptionFloats({ 0.6, 0.8 }));
        two_columns.set_key_value("filament_wipe", new ConfigOptionBools({ false, true }));
        two_columns.set_key_value("filament_retract_before_wipe", new ConfigOptionPercents({ 60., 70. }));
        two_columns.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloatsNullable({ 22., 40. }));

        CHECK(text_of(get_string_value("filament_retraction_length#1", two_columns)) == "0.8");
        CHECK(text_of(get_string_value("filament_retraction_length", two_columns)) == "0.6, 0.8");
        CHECK(text_of(get_string_value("filament_wipe#1", two_columns)) == "true");
        CHECK(text_of(get_string_value("filament_retract_before_wipe#0", two_columns)) == "60%");
        CHECK(text_of(get_string_value("filament_max_volumetric_speed#1", two_columns)) == "40");
        CHECK(text_of(get_string_value("filament_max_volumetric_speed#2", two_columns)) == "Undefined");
    }
}
