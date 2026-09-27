#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp> 
#include <cereal/types/vector.hpp> 
#include <cereal/archives/binary.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

using namespace Slic3r;

SCENARIO("Generic config validation performs as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN( "initial_layer_line_width is set to 250%, a valid value") {
            config.set_deserialize_strict("initial_layer_line_width", "250%");
            THEN( "The config is read as valid.") {
                REQUIRE(config.validate().empty());
            }
        }
        WHEN( "initial_layer_line_width is set to -10, an invalid value") {
            config.set("initial_layer_line_width", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }

        WHEN( "wall_loops is set to -10, an invalid value") {
            config.set("wall_loops", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }
    }
}

SCENARIO("Config accessor functions perform as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN("A boolean option is set to a boolean value") {
            REQUIRE_NOTHROW(config.set("gcode_comments", true));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing a 0 or 1") {
            CHECK_NOTHROW(config.set_deserialize_strict("gcode_comments", "1"));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing something other than 0 or 1") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", "Z"), BadOptionTypeException);
            }
            AND_THEN("Value is unchanged.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to an int value") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", 1), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set from serialized string") {
            config.set_deserialize_strict("nozzle_temperature", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInts>("nozzle_temperature")->get_at(0) == 100);
            }
        }
#if 0
		//FIXME better design accessors for vector elements.
		WHEN("An integer-based option is set through the integer interface") {
            config.set("nozzle_temperature", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInts>("nozzle_temperature")->get_at(0) == 100);
            }
        }
#endif
        WHEN("An floating-point option is set through the integer interface") {
            config.set("inner_wall_speed", 10);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloats>("inner_wall_speed")->get_at(0) == 10.0);
            }
        }
        WHEN("A floating-point option is set through the double interface") {
            config.set("inner_wall_speed", 5.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloats>("inner_wall_speed")->get_at(0) == 5.5);
            }
        }
        WHEN("An integer-based option is set through the double interface") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("nozzle_temperature", 5.5), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set to a non-numeric value.") {
            THEN("A BadOptionTypeException exception is thown.") {
                REQUIRE_THROWS_AS(config.set_deserialize_strict("inner_wall_speed", "zzzz"), BadOptionValueException);
            }
            THEN("The value does not change.") {
                REQUIRE(config.opt<ConfigOptionFloats>("inner_wall_speed")->get_at(0) == 60.0);
            }
        }
        WHEN("A string option is set through the string interface") {
            config.set("machine_end_gcode", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the integer interface") {
            config.set("machine_end_gcode", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the double interface") {
            config.set("machine_end_gcode", 100.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == float_to_string_decimal_point(100.5));
            }
        }
        WHEN("A float or percent is set as a percent through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100%");
            THEN("Value and percent flag are 100/true") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == true);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100");
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the int interface.") {
            config.set("initial_layer_line_width", 100);
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the double interface.") {
            config.set("initial_layer_line_width", 100.5);
            THEN("Value and percent flag are 100.5/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100.5);
            }
        }
        WHEN("An invalid option is requested during set.") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1.0), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", "1"), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", true), UnknownOptionException);
            }
        }

        WHEN("An invalid option is requested during get.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }
        WHEN("An invalid option is requested during opt.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }

        WHEN("getX called on an unset option.") {
            THEN("The default is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.2);
                REQUIRE(config.opt_int("raft_layers") == 0);
                REQUIRE(config.opt_bool("enable_support") == false);
            }
        }

        WHEN("getFloat called on an option that has been set.") {
            config.set("layer_height", 0.5);
            THEN("The set value is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.5);
            }
        }
    }
}

SCENARIO("Config ini load/save interface", "[Config]") {
    WHEN("new_from_ini is called") {
		Slic3r::DynamicPrintConfig config;
		std::string path = std::string(TEST_DATA_DIR) + "/test_config/new_from_ini.ini";
		config.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Disable);
        THEN("Config object contains ini file options.") {
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.size() == 1);
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.front() == "#ABCD");
        }
    }
}

SCENARIO("DynamicPrintConfig serialization", "[Config]") {
    WHEN("DynamicPrintConfig is serialized and deserialized") {
        FullPrintConfig full_print_config;
        DynamicPrintConfig cfg;
        cfg.apply(full_print_config, false);

        std::string serialized;
        try {
            std::ostringstream ss;
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(cfg);
            serialized = ss.str();
        } catch (const std::runtime_error & /* e */) {
            // e.what();
        }

        THEN("Config object contains ini file options.") {
            DynamicPrintConfig cfg2;
            try {
                std::stringstream ss(serialized);
                cereal::BinaryInputArchive iarchive(ss);
                iarchive(cfg2);
            } catch (const std::runtime_error & /* e */) {
                // e.what();
            }
            REQUIRE(cfg == cfg2);
        }
    }
}

TEST_CASE("DynamicPrintConfig normalizes support filament types from filament_ids", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA", "PA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFS00", "GFS01" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { true, true };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA-S");
    CHECK(display_type == "Sup.PLA");

    CHECK(config.get_filament_type(display_type, 1) == "PA-S");
    CHECK(display_type == "Sup.PA");
}

TEST_CASE("DynamicPrintConfig keeps ordinary filament types unchanged", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFSL99" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { false };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA");
    CHECK(display_type == "PLA");
}

// True if the loader would drop or rename this key on the way in: PrintConfigDef::handle_legacy()
// clears obsolete keys (extruder_type and silent_mode are in that set although print_config_def
// still defines them), and a round trip cannot expect those back.
static bool retired_on_load(const std::string &key)
{
    t_config_option_key legacy_key = key;
    std::string         value;
    PrintConfigDef::handle_legacy(legacy_key, value);
    return legacy_key != key;
}

// The generic enum options name their values through a keys map borrowed from the option definition.
// A coEnums member of a static config class - FullPrintConfig, which full_print_config() is built
// from - is initialized from the definition's default value and used to be left without that map, so
// serializing it dereferenced a null pointer. store_bbs_3mf and save_to_json serialize the whole
// config, which is how a headless project save crashed.
SCENARIO("Generic enum options keep their keys map outside of a preset bundle", "[Config]") {
    GIVEN("the coEnums members of a static config class") {
        const FullPrintConfig &defaults = FullPrintConfig::defaults();
        THEN("each of them serializes by name") {
            size_t checked = 0;
            for (const std::string &key : defaults.keys()) {
                const ConfigOption *opt = defaults.option(key);
                if (opt->type() != coEnums)
                    continue;
                ++ checked;
                INFO("option " << key);
                const ConfigOptionDef *def = print_config_def.get(key);
                REQUIRE(def != nullptr);
                REQUIRE(def->enum_keys_map != nullptr);
                const std::vector<std::string> names = static_cast<const ConfigOptionVectorBase*>(opt)->vserialize();
                REQUIRE(! names.empty());
                for (const std::string &name : names)
                    CHECK(def->enum_keys_map->find(name) != def->enum_keys_map->end());
            }
            // nozzle_volume_type, extruder_type, z_hop_types and friends: the loop above proves nothing
            // unless there are such members.
            REQUIRE(checked > 0);
        }
    }

    GIVEN("a coEnums option that has no keys map at all") {
        ConfigOptionEnumsGeneric bare{ 1, 0 };
        REQUIRE(bare.keys_map == nullptr);
        THEN("it serializes as bare ordinals rather than crashing") {
            CHECK(bare.serialize() == "1,0");
            CHECK(bare.vserialize() == std::vector<std::string>{ "1", "0" });
        }
        THEN("it reads bare ordinals back, and nothing else") {
            ConfigOptionEnumsGeneric read;
            REQUIRE(read.deserialize("1,0"));
            CHECK(read.values == std::vector<int>{ 1, 0 });
            CHECK(! read.deserialize("Standard"));
        }
        THEN("assigning from an option that has a map hands the map over") {
            const t_config_enum_values &map = ConfigOptionEnum<NozzleVolumeType>::get_enum_values();
            ConfigOptionEnumsGeneric named(&map, 1, int(NozzleVolumeType::nvtHighFlow));
            bare.set(&named);
            CHECK(bare.keys_map == &map);
            CHECK(bare.serialize() == "High Flow");
        }
    }

    GIVEN("a coEnums option handed to a DynamicPrintConfig by hand") {
        DynamicPrintConfig config;
        config.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric{ int(NozzleVolumeType::nvtHighFlow) });
        THEN("it is bound to the definition's map on the way in") {
            CHECK(config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->keys_map == print_config_def.get("nozzle_volume_type")->enum_keys_map);
            CHECK(config.opt_serialize("nozzle_volume_type") == "High Flow");
        }
    }
}

SCENARIO("A config built from the static defaults survives a JSON round trip", "[Config]") {
    GIVEN("DynamicPrintConfig::full_print_config()") {
        const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        WHEN("it is written with save_to_json and read back") {
            const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(dir);
            const boost::filesystem::path path = dir / "full_print_config_round_trip.json";
            // This is the call that crashed on the first coEnums member.
            config.save_to_json(path.string(), "probe", "project", "1.0");

            DynamicPrintConfig loaded;
            std::map<std::string, std::string> key_values;
            std::string reason;
            const ConfigSubstitutions substitutions = loaded.load_from_json(path.string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
            boost::filesystem::remove(path);

            THEN("the header and every option the loader keeps come back, nothing substituted") {
                CHECK(key_values["name"] == "probe");
                CHECK(substitutions.empty());
                for (const std::string &key : config.keys()) {
                    if (retired_on_load(key))
                        continue;
                    INFO("option " << key);
                    CHECK(loaded.has(key));
                }
            }

            THEN("the coEnums options were written by name and read back to the same values") {
                for (const std::string &key : config.keys()) {
                    const ConfigOption *opt = config.option(key);
                    if (opt->type() != coEnums || retired_on_load(key))
                        continue;
                    INFO("option " << key);
                    REQUIRE(loaded.has(key));
                    CHECK(*loaded.option(key) == *opt);
                    const ConfigOptionDef *def = print_config_def.get(key);
                    for (const std::string &name : static_cast<const ConfigOptionVectorBase*>(loaded.option(key))->vserialize())
                        CHECK(def->enum_keys_map->find(name) != def->enum_keys_map->end());
                }
            }
        }
    }
}

// Ultra: 128 of the process profiles shipped under resources/profiles (107 Bambu Lab, 12 Qidi,
// 9 Flashforge) set prime_tower_brim_width to -1, upstream BambuStudio's "auto" sentinel
// (upstream src/libslic3r/PrintConfig.cpp:6247 declares min = -1 for exactly that reason).
// This fork declared min = 0, which the GUI never noticed - it does not run Slic3r::validate()
// on the preset-based slicing path - but the CLI does (src/Snapmaker_Orca.cpp: m_print_config
// .validate(true)), so every one of those presets was unsliceable from the command line without
// an explicit --prime-tower-brim-width override. Guard the declared range against a regression.
TEST_CASE("prime_tower_brim_width accepts the -1 auto sentinel shipped by BBL/Qidi/Flashforge profiles", "[Config]")
{
    const ConfigOptionDef *def = Slic3r::print_config_def.get("prime_tower_brim_width");
    REQUIRE(def != nullptr);
    CHECK(def->min <= -1.);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("prime_tower_brim_width", "-1");
    // under_cli = true: this is the code path the command line takes.
    const std::map<std::string, std::string> errors = config.validate(true);
    CHECK(errors.find("prime_tower_brim_width") == errors.end());
    CHECK(errors.empty());

    // Anything below the sentinel is still rejected.
    config.set_deserialize_strict("prime_tower_brim_width", "-2");
    CHECK(config.validate(true).count("prime_tower_brim_width") == 1);
}

TEST_CASE("save_to_json writes the same document to a stream as to a file", "[Config]") {
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("wall_loops", new ConfigOptionInt(3));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PETG" }));
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28\nG1 Z5"));

    const boost::filesystem::path dir  = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path path = dir / "save_to_json_stream.json";
    // Pass is_custom so the Ultra header round-trips on the file path (preset saves).
    config.save_to_json(path.string(), "test_preset", "User", "1.0.0.0", "1");
    std::string file_contents;
    {
        boost::nowide::ifstream ifs(path.string());
        file_contents.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::filesystem::remove(path);
    // The file format: four spaces per nesting level and a trailing newline.
    REQUIRE_FALSE(file_contents.empty());
    CHECK(file_contents.rfind("{\n    \"", 0) == 0);
    CHECK(file_contents.back() == '\n');

    std::ostringstream strict, replaced;
    config.save_to_json(strict, "test_preset", "User", "1.0.0.0", false, "1");
    config.save_to_json(replaced, "test_preset", "User", "1.0.0.0", true, "1");
    CHECK(strict.str() == file_contents);
    CHECK(replaced.str() == file_contents);
    CHECK(nlohmann::json::parse(strict.str())["machine_start_gcode"] == "G28\nG1 Z5");
    CHECK(nlohmann::json::parse(strict.str())["is_custom_defined"] == "1");
}

TEST_CASE("save_to_json replaces invalid UTF-8 in a stream only when asked", "[Config]") {
    DynamicPrintConfig config;
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28 ; \xff"));

    std::ostringstream strict, replaced;
    CHECK_THROWS_AS(config.save_to_json(strict, "test_preset", "User", "1.0.0.0"), nlohmann::json::type_error);
    REQUIRE_NOTHROW(config.save_to_json(replaced, "test_preset", "User", "1.0.0.0", true));
    CHECK(nlohmann::json::parse(replaced.str())["machine_start_gcode"] == "G28 ; \xEF\xBF\xBD");
}

TEST_CASE("save_to_json leaves an existing file untouched when the config cannot be serialized", "[Config]") {
    DynamicPrintConfig config;
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28 ; \xff"));

    const boost::filesystem::path dir  = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path path = dir / "save_to_json_untouched.json";
    {
        boost::nowide::ofstream ofs(path.string());
        ofs << "previous";
    }
    CHECK_THROWS_AS(config.save_to_json(path.string(), "test_preset", "User", "1.0.0.0"), nlohmann::json::type_error);

    std::string contents;
    {
        boost::nowide::ifstream ifs(path.string());
        contents.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::filesystem::remove(path);
    CHECK(contents == "previous");
}

// Regression test for a fork bug: "downward_check" was declared twice with disagreeing types
// (coStrings in CLIActionsConfigDef, coBool in CLIMiscConfigDef). Because DynamicPrintAndCLIConfig
// merges print_config_def + cli_actions_config_def + cli_transform_config_def + cli_misc_config_def
// into one map via std::map::insert (which KEEPS the first-seen entry on a key collision), the
// coStrings definition silently won, and `m_config.option<ConfigOptionBool>("downward_check")` in
// Snapmaker_Orca.cpp returned nullptr via dynamic_cast, so --downward_check was a silent no-op.
// This walks every option key registered across the four ConfigDef instances that
// DynamicPrintAndCLIConfig::PrintAndCLIConfigDef merges together and fails if any key is
// registered in more than one of them, so a colliding pair (same type or not) can never again
// hide behind std::map::insert's first-wins semantics.
TEST_CASE("PrintConfigDef and the CLI ConfigDefs never register the same option key twice", "[Config][ConfigDefs]")
{
    struct Source { const char *name; const ConfigDef *def; };
    const Source sources[] = {
        { "print_config_def",        &print_config_def },
        { "cli_actions_config_def",   &cli_actions_config_def },
        { "cli_transform_config_def", &cli_transform_config_def },
        { "cli_misc_config_def",      &cli_misc_config_def },
    };

    // opt_key -> list of (source name, type) it was found registered under.
    std::map<std::string, std::vector<std::pair<std::string, ConfigOptionType>>> seen;
    for (const Source &src : sources)
        for (const auto &kvp : src.def->options)
            seen[kvp.first].push_back({ src.name, kvp.second.type });

    std::vector<std::string> duplicates;
    for (const auto &entry : seen) {
        if (entry.second.size() <= 1)
            continue;
        std::string msg = entry.first + " registered in:";
        for (const auto &where : entry.second)
            msg += " " + where.first + "(type=" + std::to_string(int(where.second)) + ")";
        duplicates.push_back(msg);
    }

    INFO("Duplicate option key registrations found (first entry wins the merge silently): ");
    for (const std::string &d : duplicates)
        UNSCOPED_INFO(d);
    CHECK(duplicates.empty());
}

// Snapmaker #810: enabling small-area flow compensation must fall back to the
// PrintConfig default model (not an empty per-preset override). The toggle
// itself stays off until the user turns it on.
TEST_CASE("Small-area flow compensation default model is populated", "[Config][SAFC]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE_FALSE(config.opt_bool("small_area_infill_flow_compensation"));
    const auto *model = config.opt<ConfigOptionStrings>("small_area_infill_flow_compensation_model");
    REQUIRE(model != nullptr);
    REQUIRE_FALSE(model->values.empty());
    CHECK(model->values.front() == "0,0");
    CHECK(model->values.back() == "\n10,1");
}

// ---------------------------------------------------------------------------
// Snapmaker: regression tests for the "Invalid speed in 'G1 F0'" failure.
//
// internal_bridge_speed is a coFloatsOrPercents (flow-variant) option with
// ratio_over = bridge_speed. ConfigBase::get_abs_value() had no branch for
// coFloatsOrPercents and fell through to an invalid ConfigOptionFloatOrPercent
// cast, reading the vector's heap pointer bits as a double (a denormal). The
// gcode formatter then rounded F = speed * 60 to integer millimeters and
// emitted a literal "G1 F0", aborting the print on the U1.
// ---------------------------------------------------------------------------
TEST_CASE("get_abs_value resolves coFloatsOrPercents over its ratio_over target", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    // Defaults: internal_bridge_speed = 150%, bridge_speed = 25 mm/s -> 37.5 mm/s.
    // Must be a sane absolute speed, not a denormal garbage value.
    const double dflt = config.get_abs_value("internal_bridge_speed");
    REQUIRE(std::abs(dflt - 37.5) < 1e-9);
    REQUIRE(dflt > 1e-6);

    // Changing the ratio_over target must be honored: 150% of 30 -> 45.
    config.set("bridge_speed", 30);
    REQUIRE(std::abs(config.get_abs_value("internal_bridge_speed") - 45.0) < 1e-9);

    // An absolute (non-percent) entry passes through unchanged.
    config.set_deserialize_strict("internal_bridge_speed", "20");
    REQUIRE(config.get_abs_value("internal_bridge_speed") == 20.0);

    // A multi-variant vector resolves the percentage from slot 0: 120% of 30 -> 36.
    config.set_deserialize_strict("internal_bridge_speed", "120%,80%");
    REQUIRE(std::abs(config.get_abs_value("internal_bridge_speed") - 36.0) < 1e-9);
}

TEST_CASE("get_abs_value fails safe on unsupported option types", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // "printer_notes" is a coString. Previously this fell through to an
    // invalid ConfigOptionFloatOrPercent cast (undefined behavior); it must
    // now log and return 0 instead of reading garbage memory.
    REQUIRE(config.get_abs_value("printer_notes") == 0.0);
}

// Mirror of the erInternalBridgeInfill speed resolution in GCode::_extrude():
// a percentage resolves against the variant-matched bridge_speed, so the
// high-flow slot of internal_bridge_speed pairs with the high-flow slot of
// bridge_speed. This is the exact get_value_at / process_flow_value machinery
// _extrude relies on after the High-Flow migration.
TEST_CASE("internal bridge speed resolution is flow-variant aware", "[Config][FlowVariant]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // coStrings does not split on commas during deserialize; assign the vector directly.
    config.option<ConfigOptionStrings>("process_flow_support")->values = {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW};
    config.set_deserialize_strict("filament_volume_type", "high_flow");
    config.set_deserialize_strict("bridge_speed", "25,50");
    config.set_deserialize_strict("internal_bridge_speed", "150%,100%");

    const size_t idx = get_config_idx(config, ConfigFlowDomain::Process, 0);
    REQUIRE(idx == 1);

    const auto   ib_fop  = get_value_at(config, *config.option<ConfigOptionFloatsOrPercents>("internal_bridge_speed"),
                                        ConfigFlowDomain::Process, 0);
    const double ib_base = get_value_at(config, *config.option<ConfigOptionFloats>("bridge_speed"),
                                        ConfigFlowDomain::Process, 0);
    const double speed   = ib_fop.percent ? (ib_fop.value * 0.01 * ib_base) : ib_fop.value;

    // 100% of the high-flow bridge_speed (50), not 150% of 25 and not the
    // Standard slot's value - and far above the 1e-6 zero-guard floor.
    REQUIRE(std::abs(speed - 50.0) < 1e-9);
    REQUIRE(speed > 1e-6);
}

// Orca #15836 / Edge CLI --align-to-y-axis: the option lives only in
// CLIMiscConfigDef (default false). setup() later materializes every CLI
// default, so CLI::run must treat the key as "given" only when it appeared
// on the command line. Otherwise the default false would override the i3
// printer-structure default (align on).
TEST_CASE("CLI --align-to-y-axis is a misc bool whose default must stay implicit", "[Config][CLI][AlignToYAxis]")
{
    const ConfigOptionDef *def = cli_misc_config_def.get("align_to_y_axis");
    REQUIRE(def != nullptr);
    CHECK(def->type == coBool);
    const auto *dflt = dynamic_cast<const ConfigOptionBool *>(def->default_value.get());
    REQUIRE(dflt != nullptr);
    CHECK_FALSE(dflt->value);

    const std::vector<std::string> cli = def->cli_args("align_to_y_axis");
    REQUIRE_FALSE(cli.empty());
    CHECK(std::find(cli.begin(), cli.end(), "align-to-y-axis") != cli.end());

    std::ostringstream help;
    cli_misc_config_def.print_cli_help(help, false);
    CHECK(help.str().find("--align-to-y-axis") != std::string::npos);

    auto parse = [](std::initializer_list<const char *> args) {
        DynamicPrintAndCLIConfig config;
        t_config_option_keys extra;
        t_config_option_keys keys;
        std::vector<const char *> argv(args);
        REQUIRE(config.read_cli(int(argv.size()), argv.data(), &extra, &keys));
        return std::make_pair(std::move(config), std::move(keys));
    };

    {
        auto [config, keys] = parse({"prog", "--align-to-y-axis=0"});
        CHECK(std::find(keys.begin(), keys.end(), "align_to_y_axis") != keys.end());
        REQUIRE(config.has("align_to_y_axis"));
        CHECK_FALSE(config.opt_bool("align_to_y_axis"));
    }
    {
        auto [config, keys] = parse({"prog", "--align-to-y-axis=1"});
        CHECK(std::find(keys.begin(), keys.end(), "align_to_y_axis") != keys.end());
        REQUIRE(config.has("align_to_y_axis"));
        CHECK(config.opt_bool("align_to_y_axis"));
    }
    {
        // Bare bool flag (no =value) deserializes as true, same as --allow-rotations.
        auto [config, keys] = parse({"prog", "--align-to-y-axis"});
        CHECK(std::find(keys.begin(), keys.end(), "align_to_y_axis") != keys.end());
        REQUIRE(config.has("align_to_y_axis"));
        CHECK(config.opt_bool("align_to_y_axis"));
    }
    {
        auto [config, keys] = parse({"prog"});
        CHECK(std::find(keys.begin(), keys.end(), "align_to_y_axis") == keys.end());
        CHECK_FALSE(config.has("align_to_y_axis"));
        // setup() fills CLI defaults afterwards; that must not count as "given".
        // CLI::run uses m_given_option_keys (from opt_order), not config.has().
        config.option("align_to_y_axis", true);
        REQUIRE(config.has("align_to_y_axis"));
        CHECK_FALSE(config.opt_bool("align_to_y_axis"));
        CHECK(std::find(keys.begin(), keys.end(), "align_to_y_axis") == keys.end());
    }
}

// The settings combo box of a plain enum option shows the option's NUMERIC value as the list index and saves the
// picked index back as the value (Choice::set_value / Choice::get_value in src/slic3r/GUI/Field.cpp). So
// enum_values[i] must be the key of enum value i: a key inserted in the middle of the list (seam_position
// "aligned_front", 2026-09-26) makes every later value show - and, once picked, save - as its neighbour. The
// options Field.cpp maps through their keys instead (and host_type, which it shifts) are exempt.
TEST_CASE("Enum options list their keys in the order of their values", "[Config][ConfigDefs]")
{
    const std::set<std::string> mapped_by_key = {
        "top_surface_pattern", "undertop_surface_pattern", "bottom_surface_pattern", "internal_solid_infill_pattern",
        "sparse_infill_pattern", "support_base_pattern", "support_interface_pattern", "ironing_pattern",
        "support_ironing_pattern", "support_style", "curr_bed_type", "host_type",
    };
    // Already out of order on main when this test was written (2026-09-26); reported, not fixed here. Remove an
    // entry once its option is fixed - either list order or key mapping in Field.cpp.
    //  * locked_skin/skeleton_infill_pattern: a subset list starting with "default" (30), not key-mapped in Field.cpp.
    //  * nozzle_volume_type & co: "Hybrid" (2) and "TPU High Flow" (3) listed in order, "E3D High Flow" is 5 at index 4.
    const std::set<std::string> known_misordered = {
        "locked_skin_infill_pattern", "locked_skeleton_infill_pattern",
        "nozzle_volume_type", "default_nozzle_volume_type", "extruder_nozzle_volume_type",
    };
    std::vector<std::string> misordered;
    size_t                   checked = 0;
    for (const auto &[key, def] : print_config_def.options) {
        if ((def.type != coEnum && def.type != coEnums) || def.enum_keys_map == nullptr || def.enum_values.empty() ||
            mapped_by_key.count(key) != 0 || known_misordered.count(key) != 0)
            continue;
        ++checked;
        for (size_t i = 0; i < def.enum_values.size(); ++i) {
            const auto it = def.enum_keys_map->find(def.enum_values[i]);
            if (it == def.enum_keys_map->end() || it->second != int(i)) {
                misordered.push_back(key + "[" + std::to_string(i) + "] = " + def.enum_values[i] + " -> " +
                                     (it == def.enum_keys_map->end() ? std::string("unknown") : std::to_string(it->second)));
                break;
            }
        }
    }
    INFO("checked " << checked << " enum options");
    CHECK(checked > 20);
    for (const std::string &m : misordered)
        UNSCOPED_INFO(m);
    CHECK(misordered.empty());

    SECTION("seam_position: every listed key is its own value and round-trips")
    {
        const ConfigOptionDef *def = print_config_def.get("seam_position");
        REQUIRE(def != nullptr);
        REQUIRE(def->enum_values.size() == def->enum_labels.size());
        REQUIRE(def->enum_values.size() == def->enum_keys_map->size());
        CHECK(def->enum_values.back() == "aligned_front");
        CHECK(def->get_default_value<ConfigOptionEnum<SeamPosition>>()->value == spAligned);
        for (size_t i = 0; i < def->enum_values.size(); ++i) {
            ConfigOptionEnum<SeamPosition> opt;
            INFO("index " << i << ", key " << def->enum_values[i]);
            REQUIRE(opt.deserialize(def->enum_values[i]));
            CHECK(int(opt.value) == int(i));
            CHECK(opt.serialize() == def->enum_values[i]);
        }
    }
}
