#include <catch2/catch.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;

namespace {

boost::filesystem::path profile_resources()
{
    return boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" / "profiles";
}

} // namespace

TEST_CASE("resource profile resolver derives a single printer vendor", "[Config][ResourceProfileResolver]")
{
    ResourceProfileResolver resolver(profile_resources());

    CHECK(resolver.vendor_for_printer("Snapmaker U1 (0.4 nozzle)", "Snapmaker U1") == "Snapmaker");
    CHECK(resolver.vendor_for_printer("Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab X1 Carbon") == "BBL");
    CHECK(resolver.vendor_for_printer("Snapmaker U1 (0.4 nozzle)", "Bambu Lab X1 Carbon").empty());

    CHECK(boost::filesystem::exists(resolver.vendor_resource("BBL", "cli_config.json")));
    CHECK_FALSE(boost::filesystem::exists(resolver.vendor_resource("Snapmaker", "cli_config.json")));
}

TEST_CASE("resource profile resolver returns flattened Snapmaker U1 presets", "[Config][ResourceProfileResolver]")
{
    ResourceProfileResolver                 resolver(profile_resources());
    ResourceProfileResolver::ResolvedPreset preset;

    REQUIRE(resolver.resolve_preset(Preset::TYPE_PRINTER, "Snapmaker U1 (0.4 nozzle)", "Snapmaker", preset));
    CHECK(preset.vendor_id == "Snapmaker");
    CHECK(preset.config.opt_string("printer_model") == "Snapmaker U1");
    CHECK(preset.config.opt_enum<GCodeFlavor>("gcode_flavor") == gcfKlipper);
    REQUIRE(preset.config.option<ConfigOptionFloats>("nozzle_diameter") != nullptr);
    CHECK(preset.config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);

    REQUIRE(resolver.resolve_preset(Preset::TYPE_PRINT, "0.20 Standard @Snapmaker U1 (0.4 nozzle)", "Snapmaker", preset));
    CHECK(preset.config.opt_float("layer_height") == Approx(0.2));
    REQUIRE(preset.config.option<ConfigOptionStrings>("compatible_printers") != nullptr);
    CHECK(preset.config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Snapmaker U1 (0.4 nozzle)"});

    REQUIRE(resolver.resolve_preset(Preset::TYPE_FILAMENT, "Generic PLA", "Snapmaker", preset));
    CHECK(preset.config.opt_string("filament_type", static_cast<unsigned int>(0)) == "PLA");

    // The same logical filament name is shipped by multiple vendors and must
    // not be selected without the printer-derived vendor context.
    CHECK_FALSE(resolver.resolve_preset(Preset::TYPE_FILAMENT, "Generic PLA", "", preset));
}

TEST_CASE("resource profile resolver returns model ids and keeps BBL working", "[Config][ResourceProfileResolver]")
{
    ResourceProfileResolver                       resolver(profile_resources());
    ResourceProfileResolver::ResolvedPrinterModel model;
    ResourceProfileResolver::ResolvedPreset       preset;

    REQUIRE(resolver.resolve_printer_model("Snapmaker U1", "Snapmaker", model));
    CHECK(model.model_id == "SM_U1");

    REQUIRE(resolver.resolve_printer_model("Bambu Lab X1 Carbon", "BBL", model));
    CHECK(model.model_id == "BL-P001");
    REQUIRE(resolver.resolve_preset(Preset::TYPE_PRINTER, "Bambu Lab X1 Carbon 0.4 nozzle", "BBL", preset));
    CHECK(preset.config.opt_string("printer_model") == "Bambu Lab X1 Carbon");

    // A vendor argument is a preference, not a hard scope. Stale or partial
    // project metadata may provide the wrong hint, but a unique profile/model
    // owner is still safe to select.
    REQUIRE(resolver.resolve_printer_model("Snapmaker U1", "BBL", model));
    CHECK(model.vendor_id == "Snapmaker");
    REQUIRE(resolver.resolve_preset(Preset::TYPE_PRINTER, "Snapmaker U1 (0.4 nozzle)", "BBL", preset));
    CHECK(preset.vendor_id == "Snapmaker");
}

TEST_CASE("resource profile resolver supports shared filament inheritance", "[Config][ResourceProfileResolver]")
{
    ResourceProfileResolver                 resolver(profile_resources());
    ResourceProfileResolver::ResolvedPreset preset;

    // Sovol's instantiated filament inherits Generic PLA @System from the
    // canonical OrcaFilamentLibrary bundle rather than from Sovol itself.
    REQUIRE(resolver.resolve_preset(Preset::TYPE_FILAMENT, "Sovol SV06 ACE PLA", "Sovol", preset));
    CHECK(preset.vendor_id == "Sovol");
    CHECK(preset.config.opt_string("filament_type", static_cast<unsigned int>(0)) == "PLA");

    // Directly selected shared presets are absent from the intentionally empty
    // OrcaFilamentLibrary manifest, but are valid for any printer vendor.
    REQUIRE(resolver.resolve_preset(Preset::TYPE_FILAMENT, "Generic PLA @System", "Sovol", preset));
    CHECK(preset.vendor_id == PresetBundle::ORCA_FILAMENT_LIBRARY);
    CHECK(preset.config.opt_string("filament_type", static_cast<unsigned int>(0)) == "PLA");
}
