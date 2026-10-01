#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/ProjectConfigFill.hpp"
#include "libslic3r/EnumChoice.hpp"
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

// CLI --load-settings / --downward_check (Snapmaker_Orca.cpp) call
// ConfigBase::load_from_json()'s 4-arg form, which does not flatten inherits. opt_float()
// then dereferences a null option<>() when printable_height lives only on the parent.
// These cases exercise the same guard as L2199 / L2681 / L4190.
namespace {
int guarded_cli_printable_height(const DynamicPrintConfig &config)
{
    int height = 0;
    if (config.option<ConfigOptionFloat>("printable_height"))
        height = static_cast<int>(config.opt_float("printable_height"));
    return height;
}

DynamicPrintConfig load_temp_json(const std::string &filename, const std::string &body, std::map<std::string, std::string> &key_values)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path path = dir / filename;
    {
        boost::nowide::ofstream ofs(path.string());
        ofs << body;
    }
    DynamicPrintConfig config;
    std::string        reason;
    const ConfigSubstitutions substitutions =
        config.load_from_json(path.string(), ForwardCompatibilitySubstitutionRule::EnableSilent, key_values, reason);
    boost::filesystem::remove(path);
    REQUIRE(reason.empty());
    REQUIRE(substitutions.empty());
    return config;
}
} // namespace

TEST_CASE("CLI printable_height guard survives load_from_json without inherit flatten", "[Config][CLI]")
{
    SECTION("empty project_settings-style JSON leaves the key absent") {
        std::map<std::string, std::string> key_values;
        DynamicPrintConfig config = load_temp_json("empty_project_settings.json", "{}\n", key_values);
        REQUIRE(config.option<ConfigOptionFloat>("printable_height") == nullptr);
        REQUIRE(guarded_cli_printable_height(config) == 0);
    }

    SECTION("4-arg load_from_json does not flatten a parent printable_height") {
        // Mirrors BBL nozzle variants: child inherits, height lives on the parent only.
        std::map<std::string, std::string> key_values;
        DynamicPrintConfig config = load_temp_json(
            "inheriting_machine.json",
            "{\n"
            "    \"type\": \"machine\",\n"
            "    \"name\": \"probe child\",\n"
            "    \"from\": \"system\",\n"
            "    \"inherits\": \"probe parent\",\n"
            "    \"printable_area\": [\"0x0\", \"256x0\", \"256x256\", \"0x256\"]\n"
            "}\n",
            key_values);
        REQUIRE(config.option<ConfigOptionString>("inherits") != nullptr);
        REQUIRE(config.opt_string("inherits") == "probe parent");
        REQUIRE(config.option<ConfigOptionFloat>("printable_height") == nullptr);
        REQUIRE(guarded_cli_printable_height(config) == 0);
    }

    SECTION("L4190 crash shape: local printable_area of 4 points, no printable_height") {
        std::map<std::string, std::string> key_values;
        DynamicPrintConfig config = load_temp_json(
            "area_without_height.json",
            "{\n"
            "    \"type\": \"machine\",\n"
            "    \"name\": \"probe area only\",\n"
            "    \"from\": \"system\",\n"
            "    \"printable_area\": [\"0x0\", \"220x0\", \"220x220\", \"0x220\"]\n"
            "}\n",
            key_values);
        const auto *area = config.option<ConfigOptionPoints>("printable_area");
        REQUIRE(area != nullptr);
        REQUIRE(area->values.size() >= 4);
        REQUIRE(config.option<ConfigOptionFloat>("printable_height") == nullptr);
        // Downward-check only reads height inside the size>=4 gate; the guard keeps the
        // struct default of 0, so the L4228 check marks the printer failed.
        REQUIRE(guarded_cli_printable_height(config) == 0);
    }

    SECTION("present printable_height still reads through the guard") {
        std::map<std::string, std::string> key_values;
        DynamicPrintConfig config = load_temp_json(
            "height_present.json",
            "{\n"
            "    \"type\": \"machine\",\n"
            "    \"name\": \"probe height\",\n"
            "    \"from\": \"system\",\n"
            "    \"printable_height\": \"256\"\n"
            "}\n",
            key_values);
        REQUIRE(config.option<ConfigOptionFloat>("printable_height") != nullptr);
        REQUIRE(guarded_cli_printable_height(config) == 256);
    }
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
    // The GUI Choice combo stores its row index as the value unless the option is mapped by key
    // (libslic3r/EnumChoice.hpp, used by Field.cpp). host_type has its own handling in Field.cpp and the
    // physical printer dialogs build their own combo for it.
    std::vector<std::string> misordered;
    size_t                   checked = 0;
    for (const auto &[key, def] : print_config_def.options) {
        if ((def.type != coEnum && def.type != coEnums) || def.enum_keys_map == nullptr || def.enum_values.empty() ||
            enum_choice_maps_by_key(key) || key == "host_type")
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

TEST_CASE("Key-mapped enum choices round-trip between stored value and combo row", "[Config][ConfigDefs]")
{
    // Every option Field.cpp maps by key: each listed key is a known value, the row a pick stores
    // shows that same row again, and the key survives a serialize round trip.
    size_t mapped = 0;
    for (const auto &[key, def] : print_config_def.options) {
        if (!enum_choice_maps_by_key(key))
            continue;
        ++mapped;
        INFO("option " << key);
        REQUIRE((def.type == coEnum || def.type == coEnums));
        REQUIRE(def.enum_keys_map != nullptr);
        REQUIRE(!def.enum_values.empty());
        REQUIRE(def.enum_labels.size() == def.enum_values.size());
        std::set<int> seen;
        for (size_t row = 0; row < def.enum_values.size(); ++row) {
            INFO("row " << row << ", key " << def.enum_values[row]);
            const int value = enum_choice_value_at_index(def, int(row));
            REQUIRE(value >= 0);
            CHECK(value == def.enum_keys_map->at(def.enum_values[row]));
            CHECK(seen.insert(value).second);
            CHECK(enum_choice_index_of_value(def, value) == int(row));
            std::unique_ptr<ConfigOption> opt(def.create_default_option());
            REQUIRE(opt->deserialize(def.enum_values[row]));
            CHECK(opt->serialize() == def.enum_values[row]);
            if (def.type == coEnum)
                CHECK(opt->getInt() == value);
        }
        CHECK(enum_choice_value_at_index(def, -1) == -1);
        CHECK(enum_choice_value_at_index(def, int(def.enum_values.size())) == -1);
    }
    // The helper names 16 options; a typo there would silently drop one from the mapping.
    CHECK(mapped == 16);

    SECTION("locked_*_infill_pattern: the first row is \"default\" (ipCount), not ipMonotonic")
    {
        for (const char *key : { "locked_skin_infill_pattern", "locked_skeleton_infill_pattern" }) {
            INFO("option " << key);
            const ConfigOptionDef *def = print_config_def.get(key);
            REQUIRE(def != nullptr);
            CHECK(def->get_default_value<ConfigOptionEnum<InfillPattern>>()->value == ipCount);
            CHECK(enum_choice_index_of_value(*def, ipCount) == 0);
            CHECK(enum_choice_value_at_index(*def, 0) == int(ipCount));
            const int grid_row = int(std::find(def->enum_values.begin(), def->enum_values.end(), "grid") - def->enum_values.begin());
            REQUIRE(grid_row < int(def->enum_values.size()));
            CHECK(enum_choice_value_at_index(*def, grid_row) == int(ipGrid));
            CHECK(enum_choice_index_of_value(*def, ipGrid) == grid_row);
            // Patterns the band menus leave out show no row of their own.
            for (InfillPattern p : { ipMonotonic, ipLockedZag, ipAdaptiveCubic, ipSupportCubic, ipLightning })
                CHECK(enum_choice_index_of_value(*def, p) == -1);
        }
    }

    SECTION("nozzle_volume_type & co: \"E3D High Flow\" is row 4 and value 5; no value 4")
    {
        for (const char *key : { "nozzle_volume_type", "default_nozzle_volume_type", "extruder_nozzle_volume_type" }) {
            INFO("option " << key);
            const ConfigOptionDef *def = print_config_def.get(key);
            REQUIRE(def != nullptr);
            REQUIRE(def->enum_values.size() == 5);
            CHECK(def->enum_values[4] == "E3D High Flow");
            CHECK(enum_choice_value_at_index(*def, 4) == int(nvtE3DHighFlow));
            CHECK(enum_choice_index_of_value(*def, nvtE3DHighFlow) == 4);
            CHECK(enum_choice_index_of_value(*def, 4) == -1);
            for (NozzleVolumeType t : { nvtStandard, nvtHighFlow, nvtHybrid, nvtTPUHighFlow })
                CHECK(enum_choice_value_at_index(*def, enum_choice_index_of_value(*def, t)) == int(t));
        }
    }
}

TEST_CASE("fill_missing_project_keys copies absent printer and process keys from the system preset", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    system.set_key_value("wall_loops", new ConfigOptionInt(4));
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    system.set_key_value("extruder_clearance_height_to_rod", new ConfigOptionFloat(27.5));
    project.set_key_value("wall_loops", new ConfigOptionInt(3));

    std::vector<std::string> filled_keys;
    const size_t             n = fill_missing_project_keys(project, system, Preset::print_options(), &filled_keys);
    REQUIRE(n == 1);
    REQUIRE(filled_keys == std::vector<std::string>{"sparse_infill_density"});
    REQUIRE(project.opt_int("wall_loops") == 3);
    REQUIRE(project.option<ConfigOptionPercent>("sparse_infill_density")->value == 15);
    REQUIRE(project.option("extruder_clearance_height_to_rod") == nullptr);

    const size_t n_printer = fill_missing_project_keys(project, system, Preset::printer_options());
    REQUIRE(n_printer == 1);
    REQUIRE_THAT(project.opt_float("extruder_clearance_height_to_rod"), Catch::Matchers::WithinAbs(27.5, 1e-9));
}

TEST_CASE("fill_missing_project_keys never overwrites a key the project already has", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("sparse_infill_density", new ConfigOptionPercent(42));
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));

    REQUIRE(fill_missing_project_keys(project, system, Preset::print_options()) == 0);
    REQUIRE(project.option<ConfigOptionPercent>("sparse_infill_density")->value == 42);
}

TEST_CASE("fill_missing_project_keys leaves mixed, flow-variant and mapping keys untouched", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    system.set_key_value("mixed_filament_definitions", new ConfigOptionString("0,1;1,0"));
    system.set_key_value("filament_volume_type", new ConfigOptionEnumsGeneric{int(FilamentVolumeType::fvtHighFlow)});
    system.set_key_value("nozzle_volume_type", new ConfigOptionEnumsGeneric{int(NozzleVolumeType::nvtHighFlow)});
    system.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    system.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(FilamentMapMode::fmmManual));
    system.set_key_value("printer_extruder_id", new ConfigOptionInts{0});
    system.set_key_value("enable_filament_mapping", new ConfigOptionBool(true));
    system.set_key_value("device_tool_count", new ConfigOptionInt(4));
    system.set_key_value("device_changer", new ConfigOptionString("ams"));
    system.set_key_value("dithering_local_z_mode", new ConfigOptionBool(true));
    system.set_key_value("wipe_tower_rotation_angle", new ConfigOptionFloat(45));
    system.set_key_value("mixed_filament_pointillism_pixel_size", new ConfigOptionFloat(0.2));
    system.set_key_value("inherits", new ConfigOptionString("parent"));
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));

    std::vector<std::string> options = Preset::print_options();
    options.insert(options.end(),
                   {"mixed_filament_definitions", "filament_volume_type", "nozzle_volume_type", "filament_map",
                    "filament_map_mode", "printer_extruder_id", "enable_filament_mapping", "device_tool_count",
                    "device_changer", "dithering_local_z_mode", "wipe_tower_rotation_angle",
                    "mixed_filament_pointillism_pixel_size", "inherits"});

    REQUIRE(fill_missing_project_keys(project, system, options) == 1);
    REQUIRE(project.has("sparse_infill_density"));
    REQUIRE_FALSE(project.has("mixed_filament_definitions"));
    REQUIRE_FALSE(project.has("filament_volume_type"));
    REQUIRE_FALSE(project.has("nozzle_volume_type"));
    REQUIRE_FALSE(project.has("filament_map"));
    REQUIRE_FALSE(project.has("filament_map_mode"));
    REQUIRE_FALSE(project.has("printer_extruder_id"));
    REQUIRE_FALSE(project.has("enable_filament_mapping"));
    REQUIRE_FALSE(project.has("device_tool_count"));
    REQUIRE_FALSE(project.has("device_changer"));
    REQUIRE_FALSE(project.has("dithering_local_z_mode"));
    REQUIRE_FALSE(project.has("wipe_tower_rotation_angle"));
    REQUIRE_FALSE(project.has("mixed_filament_pointillism_pixel_size"));
    REQUIRE_FALSE(project.has("inherits"));
    REQUIRE(project_config_fill_skip_keys().count("mixed_filament_definitions") == 1);
    REQUIRE(project_config_fill_skip_keys().count("filament_volume_type") == 1);
    REQUIRE(project_config_fill_skip_keys().count("enable_filament_mapping") == 1);
    REQUIRE(project_config_fill_skip_keys().count("device_tool_count") == 1);
    REQUIRE(project_config_fill_skip_keys().count("device_changer") == 1);
    REQUIRE(project_config_fill_skip_keys().count("filament_mapping_protocol") == 0);
    REQUIRE(project_config_fill_skip_keys().count("physical_filament_maps") == 0);
    REQUIRE(project_config_fill_skip_keys().count("thumbnails") == 0);
    REQUIRE(project_config_fill_skip_keys().count("process_flow_support") == 0);
}

TEST_CASE("fill_missing_project_keys fills filament_mapping_protocol from the printer preset", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    system.set_key_value("filament_mapping_protocol", new ConfigOptionString("snapmaker"));
    std::vector<std::string> options = {"filament_mapping_protocol"};
    REQUIRE(fill_missing_project_keys(project, system, options) == 1);
    REQUIRE(project.opt_string("filament_mapping_protocol") == "snapmaker");
}

TEST_CASE("fill_missing_project_keys skips keys handle_legacy drops on load", "[Config][CLI]")
{
    REQUIRE(project_config_key_dropped_on_load("silent_mode"));
    REQUIRE_FALSE(project_config_key_dropped_on_load("wall_loops"));

    DynamicPrintConfig project;
    DynamicPrintConfig system;
    system.set_key_value("silent_mode", new ConfigOptionBool(true));
    system.set_key_value("layer_height", new ConfigOptionFloat(0.2));

    REQUIRE(fill_missing_project_keys(project, system, Preset::printer_options()) == 0);
    REQUIRE_FALSE(project.has("silent_mode"));
    REQUIRE(fill_missing_project_keys(project, system, Preset::print_options()) == 1);
    REQUIRE_THAT(project.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
}

TEST_CASE("missing_project_keys is empty when the project already has every listed key", "[Config][CLI]")
{
    DynamicPrintConfig project;
    project.set_key_value("wall_loops", new ConfigOptionInt(3));
    project.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    const auto missing = missing_project_keys(project, {"wall_loops", "sparse_infill_density"});
    REQUIRE(missing.empty());
    REQUIRE(fill_missing_project_keys(project, project, {"wall_loops", "sparse_infill_density"}) == 0);
}

TEST_CASE("fill_cli_system_preset does not call find_system when nothing is missing", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("wall_loops", new ConfigOptionInt(3));
    project.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(99));
    int find_calls = 0;
    auto find = [&](const std::string &) -> const DynamicPrintConfig * {
        ++find_calls;
        return &system;
    };
    REQUIRE(fill_cli_system_preset(project, "Snapmaker U1 (0.4 nozzle)", {"wall_loops", "sparse_infill_density"}, find) == 0);
    REQUIRE(find_calls == 0);
    REQUIRE(project.option<ConfigOptionPercent>("sparse_infill_density")->value == 15);
}

TEST_CASE("snapshot is taken before create=true reads hide missing keys", "[Config][CLI]")
{
    DynamicPrintConfig loaded;
    loaded.set_key_value("wall_loops", new ConfigOptionInt(3));
    const auto present = project_config_snapshot_loaded_keys(loaded);
    REQUIRE(present.count("printer_model") == 0);
    REQUIRE(present.count("printable_area") == 0);

    loaded.option<ConfigOptionString>("printer_model", true);
    loaded.option<ConfigOptionPoints>("printable_area", true);
    REQUIRE(loaded.option("printer_model") != nullptr);
    REQUIRE(loaded.option("printable_area") != nullptr);

    DynamicPrintConfig system;
    system.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    system.set_key_value("printable_area", new ConfigOptionPoints{Vec2d(0, 0), Vec2d(250, 0), Vec2d(250, 250), Vec2d(0, 250)});
    std::vector<std::string> options = {"printer_model", "printable_area"};
    REQUIRE(missing_project_keys(loaded, options, &present) == options);
    REQUIRE(fill_missing_project_keys(loaded, system, options, nullptr, &present) == 2);
    REQUIRE(loaded.opt_string("printer_model") == "Snapmaker U1");
    REQUIRE(loaded.option<ConfigOptionPoints>("printable_area")->values.size() == 4);
}

TEST_CASE("inherits_group resolution selects which system preset the CLI fill copies from", "[Config][CLI]")
{
    const std::string current_printer = "My U1 copy";
    const std::string current_process = "My 0.20 copy";
    const std::string system_printer  = "Snapmaker U1 (0.4 nozzle)";
    const std::string system_process  = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    const std::string other_process   = "0.20 Standard @Snapmaker J1 (0.4 nozzle)";

    DynamicPrintConfig project;
    project.set_key_value("printer_settings_id", new ConfigOptionString(current_printer));
    project.set_key_value("print_settings_id", new ConfigOptionString(current_process));
    DynamicPrintConfig u1_process;
    u1_process.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    DynamicPrintConfig j1_process;
    j1_process.set_key_value("sparse_infill_density", new ConfigOptionPercent(20));

    int find_calls = 0;
    std::string asked;
    auto find = [&](const std::string &name) -> const DynamicPrintConfig * {
        ++find_calls;
        asked = name;
        if (name == system_process)
            return &u1_process;
        if (name == other_process)
            return &j1_process;
        if (name == current_process)
            return &j1_process;
        return nullptr;
    };

    const std::vector<std::string> renamed{system_process, "Generic PLA", system_printer};
    const std::string process_name = resolve_project_system_preset_name(current_process, &renamed, 1, false);
    REQUIRE(process_name == system_process);
    REQUIRE(fill_cli_system_preset(project, process_name, Preset::print_options(), find) == 1);
    REQUIRE(find_calls == 1);
    REQUIRE(asked == system_process);
    REQUIRE(project.option<ConfigOptionPercent>("sparse_infill_density")->value == 15);

    REQUIRE(resolve_project_system_preset_name(current_printer, nullptr, 1, true) == current_printer);
    const std::vector<std::string> empty_slots{"", "Generic PLA", ""};
    REQUIRE(resolve_project_system_preset_name(current_printer, &empty_slots, 1, true) == current_printer);
    const std::vector<std::string> mis_sized{system_process};
    REQUIRE(resolve_project_system_preset_name(current_printer, &mis_sized, 1, true) == current_printer);
    const std::vector<std::string> too_long{system_process, "a", "b", system_printer};
    REQUIRE(resolve_project_system_preset_name(current_printer, &too_long, 1, true) == current_printer);
}

TEST_CASE("cli_fill_from_system_preset skips fill so --process-preset values win", "[Config][CLI]")
{
    REQUIRE(cli_fill_from_system_preset(""));
    REQUIRE_FALSE(cli_fill_from_system_preset("Snapmaker U1 (0.4 nozzle)"));
    REQUIRE_FALSE(cli_fill_from_system_preset("0.20mm Standard @Snapmaker U1 (0.4 nozzle)"));

    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("wall_loops", new ConfigOptionInt(5));
    system.set_key_value("wall_loops", new ConfigOptionInt(2));
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));
    int find_calls = 0;
    auto find = [&](const std::string &) -> const DynamicPrintConfig * {
        ++find_calls;
        return &system;
    };

    const std::string process_override = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    if (cli_fill_from_system_preset(process_override))
        fill_cli_system_preset(project, "ignored", Preset::print_options(), find);
    REQUIRE(find_calls == 0);
    REQUIRE(project.opt_int("wall_loops") == 5);
    REQUIRE(project.option("sparse_infill_density") == nullptr);

    REQUIRE(fill_cli_system_preset(project, "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", Preset::print_options(), find) == 1);
    REQUIRE(find_calls == 1);
    REQUIRE(project.opt_int("wall_loops") == 5);
    REQUIRE(project.option<ConfigOptionPercent>("sparse_infill_density")->value == 15);
}

TEST_CASE("fill_missing_project_keys resizes per-extruder vectors to the project's nozzle_diameter", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    system.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4, 0.4, 0.4});
    system.set_key_value("retraction_length", new ConfigOptionFloats{1.0, 2.0, 3.0, 4.0});
    system.set_key_value("extruder_nozzle_count", new ConfigOptionInts{1, 1, 1, 1});
    system.set_key_value("extruder_printable_height", new ConfigOptionFloats{250, 250, 250, 250});

    REQUIRE(fill_missing_project_keys(project, system, Preset::printer_options()) >= 1);
    const auto *retract = project.option<ConfigOptionFloats>("retraction_length");
    REQUIRE(retract != nullptr);
    REQUIRE(retract->values.size() == 1);
    REQUIRE_THAT(retract->values.front(), Catch::Matchers::WithinAbs(1.0, 1e-9));
    const auto *noz_count = project.option<ConfigOptionInts>("extruder_nozzle_count");
    REQUIRE(noz_count != nullptr);
    REQUIRE(noz_count->values.size() == 1);
    REQUIRE(noz_count->values.front() == 1);
    const auto *height = project.option<ConfigOptionFloats>("extruder_printable_height");
    REQUIRE(height != nullptr);
    REQUIRE(height->values.size() == 1);
}

TEST_CASE("fill_missing_project_keys copies an empty default_filament_profile without throwing", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.6});
    system.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    system.set_key_value("default_filament_profile", new ConfigOptionStrings());
    REQUIRE(system.option<ConfigOptionStrings>("default_filament_profile")->values.empty());

    size_t n = 0;
    REQUIRE_NOTHROW(n = fill_missing_project_keys(project, system, Preset::printer_options()));
    REQUIRE(n >= 1);
    const auto *dfp = project.option<ConfigOptionStrings>("default_filament_profile");
    REQUIRE(dfp != nullptr);
    REQUIRE(dfp->values.empty());
}

TEST_CASE("fill_missing_project_keys skips flow-variant vectors when process_flow_support differs", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    project.set_key_value("process_flow_support", new ConfigOptionStrings{"Standard"});
    system.set_key_value("process_flow_support", new ConfigOptionStrings{"Standard", "High Flow"});
    system.set_key_value("inner_wall_speed", new ConfigOptionFloats{300, 600});
    system.set_key_value("sparse_infill_density", new ConfigOptionPercent(15));

    std::vector<std::string> filled_keys;
    fill_missing_project_keys(project, system, Preset::print_options(), &filled_keys);
    REQUIRE(std::find(filled_keys.begin(), filled_keys.end(), "inner_wall_speed") == filled_keys.end());
    REQUIRE_FALSE(project.has("inner_wall_speed"));
    REQUIRE(project.has("sparse_infill_density"));
}

TEST_CASE("fill_missing_project_keys fills process_flow_support with matching flow-variant vectors", "[Config][CLI]")
{
    DynamicPrintConfig project;
    DynamicPrintConfig system;
    system.set_key_value("process_flow_support", new ConfigOptionStrings{"Standard", "High Flow"});
    system.set_key_value("inner_wall_speed", new ConfigOptionFloats{300, 600});

    std::vector<std::string> filled_keys;
    fill_missing_project_keys(project, system, Preset::print_options(), &filled_keys);
    REQUIRE(std::find(filled_keys.begin(), filled_keys.end(), "process_flow_support") != filled_keys.end());
    REQUIRE(std::find(filled_keys.begin(), filled_keys.end(), "inner_wall_speed") != filled_keys.end());
    const auto *fs = project.option<ConfigOptionStrings>("process_flow_support");
    REQUIRE(fs != nullptr);
    REQUIRE(fs->values == std::vector<std::string>{"Standard", "High Flow"});
    const auto *speed = project.option<ConfigOptionFloats>("inner_wall_speed");
    REQUIRE(speed != nullptr);
    REQUIRE(speed->values.size() == 2);
    REQUIRE_THAT(speed->values[0], Catch::Matchers::WithinAbs(300, 1e-9));
    REQUIRE_THAT(speed->values[1], Catch::Matchers::WithinAbs(600, 1e-9));
}

TEST_CASE("project_config_fill_log_value truncates long G-code strings without splitting UTF-8", "[Config][CLI]")
{
    ConfigOptionString gcode(std::string(200, 'G'));
    const std::string  logged = project_config_fill_log_value(&gcode, 96);
    REQUIRE(logged.size() == 99);
    REQUIRE(logged.compare(96, 3, "...") == 0);

    // U+4E2D CJK '中' is E4 B8 AD; place it so byte 96 lands inside a 3-byte sequence.
    std::string wide = "x";
    for (int i = 0; i < 40; ++i)
        wide += "\xE4\xB8\xAD";
    ConfigOptionString wide_opt(wide);
    const std::string  cut = project_config_fill_log_value(&wide_opt, 96);
    REQUIRE(cut.size() >= 3);
    REQUIRE(cut.compare(cut.size() - 3, 3, "...") == 0);
    const std::string prefix = cut.substr(0, cut.size() - 3);
    REQUIRE(prefix.size() <= 96);
    REQUIRE(prefix.size() < wide.size());
    // Truncation landed on a code-point boundary: the next original byte is not a continuation.
    REQUIRE((static_cast<unsigned char>(wide[prefix.size()]) & 0xC0) != 0x80);
    REQUIRE(prefix.find('\xE4') != std::string::npos);
}

TEST_CASE("Edge grouping dialog gate is CUSTOM plus two distinct nozzle flow types", "[Config][FilamentGroup]")
{
    // Snapmaker's gate is any_nozzle_high_flow() plus FilamentGroupDialog(parent, all_high_flow).
    // Edge must not follow that: dialog only when grouping is custom AND nozzles mix flow types.
    CHECK(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 2));
    CHECK(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 3));
    CHECK_FALSE(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 1));
    CHECK_FALSE(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 0));
    CHECK_FALSE(filament_group_dialog_required(FILAMENT_GROUPING_STANDARD, 2));
    CHECK_FALSE(filament_group_dialog_required(FILAMENT_GROUPING_STANDARD, 1));
    CHECK_FALSE(filament_group_dialog_required("unknown", 2));
}

TEST_CASE("filament group dirty flag is set only on valid-to-invalid slice result", "[Config][FilamentGroup]")
{
    // Snap #930: Preview re-slice should re-confirm grouping after a param change
    // invalidates a previously sliced plate, not on first slice or re-validation.
    CHECK(filament_group_dirty_on_invalidation(true, false));
    CHECK_FALSE(filament_group_dirty_on_invalidation(false, false));
    CHECK_FALSE(filament_group_dirty_on_invalidation(false, true));
    CHECK_FALSE(filament_group_dirty_on_invalidation(true, true));
}

TEST_CASE("filament group slice decision covers prompt, remote skip, and sync", "[Config][FilamentGroup]")
{
    using D = FilamentGroupSliceDecision;

    // CUSTOM + mixed nozzles, person at the PC: show the dialog.
    CHECK(filament_group_slice_decision(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 2), true) == D::Prompt);
    // N2: phone / agent / hidden instance must not open the dialog.
    CHECK(filament_group_slice_decision(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 2), false) == D::SkipAndProceed);
    // STANDARD or a single flow type: sync, never prompt.
    CHECK(filament_group_slice_decision(filament_group_dialog_required(FILAMENT_GROUPING_STANDARD, 2), true) == D::Sync);
    CHECK(filament_group_slice_decision(filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 1), true) == D::Sync);
    CHECK(filament_group_slice_decision(false, false) == D::Sync);
}

TEST_CASE("filament group plate-pick continues only when grouping is accepted", "[Config][FilamentGroup]")
{
    const bool required = filament_group_dialog_required(FILAMENT_GROUPING_CUSTOM, 2);
    REQUIRE(required);

    // N1: dirty + interactive Cancel always aborts (no switch, no slice), even if
    // a sibling plate still reports is_slice_result_valid().
    CHECK_FALSE(filament_group_plate_pick_continues(true, required, true, false));
    CHECK(filament_group_plate_pick_continues(true, required, true, true));

    // N2: non-interactive (remote / hidden) proceeds without a confirmed dialog.
    CHECK(filament_group_plate_pick_continues(true, required, false, false));
    CHECK(filament_group_plate_pick_continues(true, required, false, true));

    // Clean pick: no grouping prompt, continue. Sync only when the dialog is not required.
    CHECK(filament_group_plate_pick_continues(false, required, true, false));
    CHECK_FALSE(filament_group_sync_on_clean_plate_pick(false, required));
    CHECK(filament_group_sync_on_clean_plate_pick(false, false));
    CHECK_FALSE(filament_group_sync_on_clean_plate_pick(true, false));

    // S4: a never-sliced plate is not dirty, so tab-in / pick does not prompt.
    CHECK_FALSE(filament_group_dirty_on_invalidation(false, false));
    CHECK(filament_group_plate_pick_continues(false, required, true, false));
}
