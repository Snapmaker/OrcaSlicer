#include <catch2/catch.hpp>

#include "libslic3r/BambuConfigCompat.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::BambuConfigCompat;

// Bambu Studio writes a literal "nil" into the per-extruder slots a setting does not apply to.
// This fork types some of those options as scalars and many as plain non-nullable vectors (most
// of the speed / acceleration family became per-flow-variant vectors with the High-Flow work),
// so the value has to be translated before it reaches deserialize() - otherwise the option throws and
// PresetCollection::load_presets deletes the user's preset file.
//
// The rule these tests exist to protect: a nil slot means "not applicable", never zero. A speed
// or acceleration of 0 emits `G1 F0` and stalls the printer.

static const ConfigOptionDef* def_of(const std::string &key)
{
    return print_config_def.get(key);
}

static std::vector<std::string> V(std::initializer_list<const char*> items)
{
    std::vector<std::string> out;
    for (const char *s : items)
        out.emplace_back(s);
    return out;
}

TEST_CASE("has_nil spots the Bambu not-applicable marker", "[BambuCompat]")
{
    CHECK(has_nil(V({"100", "nil", "100"})));
    CHECK(has_nil(V({"nil"})));
    CHECK_FALSE(has_nil(V({"100", "200"})));
    CHECK_FALSE(has_nil(std::vector<std::string>{}));
    // "nil" is matched exactly; a value that merely contains those letters is data.
    CHECK_FALSE(has_nil(V({"nil_", "0nil"})));
}

// ---------------------------------------------------------------------------------------------
// Scalar targets: the COLLAPSE path.
//
// The High-Flow work (c3c82dbf64) retyped most of the speed / acceleration family to per-flow-
// variant vectors, so those keys now take the backfill path below. The collapse path is still
// live for the options that remain scalar here but are per-extruder vectors in Bambu Studio
// (docs/bambu-config-compat.md, table A). The key is picked from that list at run time and must
// still be scalar, so a future retype fails here loudly instead of silently testing a vector.
// ---------------------------------------------------------------------------------------------
static const ConfigOptionDef* live_scalar_float_key()
{
    // Bambu: coFloats. Here: coFloat. Any one of them exercises the same collapse code.
    for (const char *key : { "travel_speed_z", "top_solid_infill_flow_ratio", "nozzle_volume", "flush_multiplier" }) {
        const ConfigOptionDef *def = def_of(key);
        if (def != nullptr && def->is_scalar() && def->type == coFloat && ! def->nullable)
            return def;
    }
    return nullptr;
}

TEST_CASE("the collapse path still has a live scalar key to protect", "[BambuCompat]")
{
    const ConfigOptionDef *def = live_scalar_float_key();
    INFO("every table-A float key has been retyped to a vector: move the collapse tests to a new "
         "scalar key, or drop them if the collapse path is now dead");
    REQUIRE(def != nullptr);
    CHECK(def->is_scalar());
    CHECK_FALSE(def->nullable);
}

TEST_CASE("nil slots around identical values collapse to that value", "[BambuCompat]")
{
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    REQUIRE(def->is_scalar());

    const NilResult r = translate_nil_array(def, V({"12", "nil", "12", "nil", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "12");
    CHECK(r.changed());
    CHECK_FALSE(r.lossy());
    CHECK_FALSE(r.drop());
    CHECK(r.original == "12,nil,12,nil,nil");
}

TEST_CASE("a single surviving value collapses and is not called lossy", "[BambuCompat]")
{
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    const NilResult r = translate_nil_array(def, V({"nil", "nil", "25"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "25");
    CHECK_FALSE(r.lossy());
}

TEST_CASE("differing values take the documented rule and report the loss", "[BambuCompat]")
{
    // Same shape as the owner's 0.10mm @H2D 2W Arachne.json.
    // Majority of the non-nil slots wins: 200 appears twice, 160 once.
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    const NilResult r = translate_nil_array(def, V({"200", "nil", "200", "nil", "160"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "200");
    CHECK(r.lossy());
    CHECK(r.original == "200,nil,200,nil,160");
}

TEST_CASE("the majority wins even when it is not the first slot", "[BambuCompat]")
{
    // Same shape as 0.30mm @H2D - Custom ASA.json: 2000 twice beats 3000 once.
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    const NilResult r = translate_nil_array(def, V({"3000", "2000", "nil", "2000", "nil"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "2000");
}

TEST_CASE("a tie is broken by the first non-nil slot", "[BambuCompat]")
{
    // Bambu writes the primary extruder first, so the first slot is what a single-extruder
    // machine actually prints with.
    const ConfigOptionDef *def = live_scalar_float_key();
    REQUIRE(def != nullptr);
    const NilResult r = translate_nil_array(def, V({"120", "nil", "180"}));
    CHECK(r.fix == NilFix::CollapsedLossy);
    CHECK(r.value == "120");
}

TEST_CASE("percent values survive the collapse as percents", "[BambuCompat]")
{
    // No coFloatOrPercent option is still scalar-here / vector-in-Bambu since the High-Flow
    // retype, so the translator's handling of the type is pinned with a stand-in definition.
    ConfigOptionDef def;
    def.type = coFloatOrPercent;
    REQUIRE(def.is_scalar());
    const NilResult r = translate_nil_array(&def, V({"80%", "nil", "80%", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "80%");
}

TEST_CASE("an enum scalar collapses to its name", "[BambuCompat]")
{
    // nozzle_type is coEnum here and coEnums in Bambu.
    const ConfigOptionDef *def = def_of("nozzle_type");
    REQUIRE(def != nullptr);
    if (! def->is_scalar())
        return; // retyped: covered by the vector tests below
    const NilResult r = translate_nil_array(def, V({"hardened_steel", "nil", "hardened_steel", "nil"}));
    CHECK(r.fix == NilFix::Collapsed);
    CHECK(r.value == "hardened_steel");
}

// ---------------------------------------------------------------------------------------------
// Vector targets retyped by the High-Flow work: the BACKFILL path.
//
// These were scalars when this translation was written and are per-flow-variant vectors now
// (process_flow_variant_options()). Bambu writes one slot per extruder variant (4 for
// fdm_process_dual_common, 5-7 for the H2D/H2C process presets), and the fork's own BBL system
// profiles carry the same slot layout, so the slot count is kept and only the nil slots are
// filled. Slot 0 is Bambu's "extruder 1, Direct Drive Standard" and is also this fork's
// "standard" flow variant - the one the slicer reads - see the end-to-end test further down.
// ---------------------------------------------------------------------------------------------
static const ConfigOptionDef* retyped_vector(const char *key)
{
    const ConfigOptionDef *def = def_of(key);
    REQUIRE(def != nullptr);
    INFO(key << " is expected to be a non-nullable per-flow-variant vector since the High-Flow retype");
    REQUIRE_FALSE(def->is_scalar());
    REQUIRE_FALSE(def->nullable);
    CHECK(is_process_flow_variant_option(key));
    return def;
}

TEST_CASE("a retyped acceleration keeps its slot count and is backfilled", "[BambuCompat]")
{
    // The classic shape that motivated this module.
    const NilResult r = translate_nil_array(retyped_vector("top_surface_acceleration"), V({"100", "nil", "100", "nil", "nil"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "100,100,100,100,100");
    CHECK_FALSE(r.lossy());
    CHECK(r.original == "100,nil,100,nil,nil");
}

TEST_CASE("a retyped speed with a single surviving value fills every slot with it", "[BambuCompat]")
{
    // 0.10mm @H2D 2W Arachne.json writes sparse_infill_speed as ["nil","nil","nil","nil","200"].
    const NilResult r = translate_nil_array(retyped_vector("sparse_infill_speed"), V({"nil", "nil", "250"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "250,250,250");
    CHECK_FALSE(r.lossy());
}

TEST_CASE("a retyped speed with differing slots keeps every real value", "[BambuCompat]")
{
    // Real shape from 0.10mm @H2D 2W Arachne.json. Nothing Bambu wrote is dropped any more: the
    // 160 of the fifth variant survives in its own slot, only the nil slots are guesses.
    const NilResult r = translate_nil_array(retyped_vector("inner_wall_speed"), V({"200", "nil", "200", "nil", "160"}));
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "200,200,200,200,160");
    CHECK(r.lossy());
    CHECK(r.original == "200,nil,200,nil,160");
}

TEST_CASE("a retyped acceleration keeps slot 0 as Bambu wrote it", "[BambuCompat]")
{
    // Real shape from 0.30mm @H2D - Custom ASA.json. The old scalar collapse took the majority
    // (2000); as a vector, slot 0 keeps Bambu's extruder-1 standard value (3000) and the nils
    // take their preceding neighbour.
    const NilResult r = translate_nil_array(retyped_vector("outer_wall_acceleration"), V({"3000", "2000", "nil", "2000", "nil"}));
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "3000,2000,2000,2000,2000");
}

TEST_CASE("retyped percent vectors keep their percents per slot", "[BambuCompat]")
{
    // small_perimeter_speed is coFloatsOrPercents on both sides now.
    const NilResult r = translate_nil_array(retyped_vector("small_perimeter_speed"), V({"80%", "nil", "80%", "nil"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "80%,80%,80%,80%");
}

TEST_CASE("a retyped bool vector is backfilled too", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(retyped_vector("enable_overhang_speed"), V({"1", "nil", "1", "nil"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "1,1,1,1");
}

TEST_CASE("an all-nil array keeps no value at all", "[BambuCompat]")
{
    // Seen in ELEGOO PETG Rapid Flow Rate Calibrated.json as ["nil","nil"].
    // There is nothing to keep, and fabricating a number here is exactly the forbidden move.
    const NilResult r = translate_nil_array(def_of("filament_flow_ratio"), V({"nil", "nil"}));
    CHECK(r.fix == NilFix::AllNil);
    CHECK(r.drop());
    CHECK(r.value.empty());
}

TEST_CASE("a non-nullable vector keeps its slot count and is backfilled", "[BambuCompat]")
{
    // nozzle_temperature is coInts here: collapsing it to a scalar would destroy per-extruder
    // structure, so nil slots take the nearest real neighbour instead.
    const ConfigOptionDef *def = def_of("nozzle_temperature");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->is_scalar());
    REQUIRE_FALSE(def->nullable);

    const NilResult r = translate_nil_array(def, V({"250", "nil", "250", "nil"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "250,250,250,250");
    CHECK_FALSE(r.lossy());
}

TEST_CASE("a leading nil is filled from the value that follows", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(def_of("nozzle_temperature"), V({"nil", "nil", "240"}));
    CHECK(r.fix == NilFix::Backfilled);
    CHECK(r.value == "240,240,240");
}

TEST_CASE("a backfill over differing neighbours is reported as lossy", "[BambuCompat]")
{
    // Real shape from Bambu Lab H2C 0.4 nozzle - Custom.json.
    const NilResult r = translate_nil_array(def_of("machine_max_jerk_x"), V({"20", "9", "20", "9", "nil", "nil"}));
    CHECK(r.fix == NilFix::BackfilledLossy);
    CHECK(r.value == "20,9,20,9,9,9");
    CHECK(r.lossy());
}

TEST_CASE("an already-nullable option is left completely alone", "[BambuCompat]")
{
    // These deserialize "nil" natively into their own sentinel; rewriting them here would
    // destroy the very information the nullable type exists to carry.
    const ConfigOptionDef *def = def_of("filament_retraction_length");
    if (def != nullptr && def->nullable) {
        const NilResult r = translate_nil_array(def, V({"0.8", "nil", "0.8"}));
        CHECK(r.fix == NilFix::None);
        CHECK_FALSE(r.changed());
        CHECK(r.value.empty());
    }
}

TEST_CASE("an array with no nil is not touched", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(def_of("inner_wall_speed"), V({"200", "160"}));
    CHECK(r.fix == NilFix::None);
    CHECK_FALSE(r.changed());
}

TEST_CASE("an unknown option is not translated", "[BambuCompat]")
{
    const NilResult r = translate_nil_array(nullptr, V({"1", "nil"}));
    CHECK(r.fix == NilFix::None);
}

// ---------------------------------------------------------------------------------------------
// The regression that matters most: this week's `G1 F0` outage came from a speed reaching the
// G-code writer as zero. Translation must never be the thing that produces a zero.
// ---------------------------------------------------------------------------------------------
TEST_CASE("translation never invents a zero for a speed or acceleration", "[BambuCompat]")
{
    const char *speed_keys[] = {
        "inner_wall_speed", "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed",
        "top_surface_speed", "support_speed", "support_interface_speed", "gap_infill_speed",
        "initial_layer_speed", "initial_layer_infill_speed", "travel_speed", "bridge_speed",
        "top_surface_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
        "default_acceleration", "travel_acceleration", "initial_layer_acceleration",
    };
    const std::vector<std::vector<std::string>> inputs = {
        V({"200", "nil"}),
        V({"nil", "200"}),
        V({"200", "nil", "200", "nil", "160"}),
        V({"nil", "nil", "120"}),
        V({"100", "nil", "100", "nil", "nil"}),
        V({"3000", "2000", "nil", "2000", "nil"}),
    };
    for (const char *key : speed_keys) {
        const ConfigOptionDef *def = def_of(key);
        if (def == nullptr)
            continue;
        for (const std::vector<std::string> &in : inputs) {
            const NilResult r = translate_nil_array(def, in);
            if (! r.changed() || r.drop())
                continue;
            INFO("key " << key << " produced \"" << r.value << "\" from \"" << r.original << "\"");
            // None of the inputs above contains a zero, so no output slot may be zero either.
            std::string token;
            std::istringstream iss(r.value);
            while (std::getline(iss, token, ',')) {
                REQUIRE_FALSE(token.empty());
                CHECK(std::stod(token) != Approx(0.0));
            }
        }
    }
}

TEST_CASE("a zero Bambu really wrote is preserved, not scrubbed", "[BambuCompat]")
{
    // Bambu genuinely stores 0 for some of these as a "not set / use default" sentinel, e.g.
    // "overhang_1_4_speed": ["0","nil","0","nil"] and "travel_speed_z": ["0","0","0","0"].
    // Translation must carry that through faithfully - the rule is "never INVENT a zero", not
    // "never allow one". Both paths are checked.
    SECTION("vector target (backfill)") {
        const NilResult r = translate_nil_array(retyped_vector("overhang_1_4_speed"), V({"0", "nil", "0", "nil"}));
        CHECK(r.fix == NilFix::Backfilled);
        CHECK(r.value == "0,0,0,0");
    }
    SECTION("scalar target (collapse)") {
        const ConfigOptionDef *def = live_scalar_float_key();
        REQUIRE(def != nullptr);
        const NilResult r = translate_nil_array(def, V({"0", "nil", "0", "nil"}));
        CHECK(r.fix == NilFix::Collapsed);
        CHECK(r.value == "0");
    }
}

// ---------------------------------------------------------------------------------------------
// End-to-end for a retyped key: the backfilled vector must be read at the right slot.
//
// Bambu's arrays are indexed by extruder variant (print_extruder_variant, 5 entries for this
// H2D preset); this fork's are indexed by flow variant (process_flow_support). A Bambu preset
// carries no process_flow_support; Preset::normalize derives ["standard", "high_flow"] from its
// print_extruder_variant (BambuFlowSupport::derive, owner decision D1), so get_config_idx()
// resolves a Standard filament to slot 0 ("Direct Drive Standard" on extruder 1) and a High Flow
// filament to slot 1 ("Direct Drive High Flow"), what Bambu Studio itself prints with there.
// The extra slots are kept (Preset::normalize only ever grows a flow-variant vector), exactly as
// for the fork's own shipped BBL H2D profiles, which carry the same 5-7 slot layout.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a backfilled Bambu vector is read at the slot the slicer uses", "[BambuCompat]")
{
    const boost::filesystem::path file = boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat" / "0.30mm_@H2D_-_Custom_ASA.json";
    if (! boost::filesystem::exists(file)) {
        WARN("fixture missing: " << file.string());
        return;
    }

    DynamicPrintConfig loaded;
    std::map<std::string, std::string> key_values;
    std::string reason;
    loaded.load_from_json(file.string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
    REQUIRE(reason.empty());

    // Bambu wrote ["3000","2000","nil","2000","nil"] and ["30","50","nil","50","nil"].
    const auto *accel = loaded.option<ConfigOptionFloats>("outer_wall_acceleration");
    const auto *iface = loaded.option<ConfigOptionFloats>("support_interface_speed");
    REQUIRE(accel != nullptr);
    REQUIRE(iface != nullptr);
    CHECK(accel->values == std::vector<double>{ 3000, 2000, 2000, 2000, 2000 });
    CHECK(iface->values == std::vector<double>{ 30, 50, 50, 50, 50 });

    // Normalize as PresetCollection does on load (a print preset: flow-variant vectors are only
    // ever grown to process_flow_support's length, never truncated), then compose the way the
    // slicer sees it: full defaults with the preset on top.
    Preset::normalize(loaded);
    REQUIRE(loaded.option<ConfigOptionFloats>("outer_wall_acceleration")->size() == 5);
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(loaded, true);
    REQUIRE(full.option<ConfigOptionFloats>("outer_wall_acceleration")->size() == 5);

    for (FilamentVolumeType flow : { fvtStandard, fvtHighFlow }) {
        INFO("filament flow type " << to_string(flow));
        if (auto *types = full.option<ConfigOptionEnumsGeneric>("filament_volume_type", true))
            types->values.assign(1, int(flow));
        const bool   high_flow = flow == fvtHighFlow;
        const size_t idx       = get_config_idx(full, ConfigFlowDomain::Process, 0);
        CHECK(idx == (high_flow ? 1 : 0));
        CHECK(get_value_at(full, *full.option<ConfigOptionFloats>("outer_wall_acceleration"), ConfigFlowDomain::Process, 0) ==
              Approx(high_flow ? 2000. : 3000.));
        CHECK(get_value_at(full, *full.option<ConfigOptionFloats>("support_interface_speed"), ConfigFlowDomain::Process, 0) ==
              Approx(high_flow ? 50. : 30.));
        // Every flow-variant speed / acceleration the slicer can read is a real, non-zero value.
        for (const char *key : { "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "top_surface_acceleration" }) {
            const auto *opt = full.option<ConfigOptionFloats>(key);
            REQUIRE(opt != nullptr);
            INFO(key << " = " << opt->serialize());
            CHECK(get_value_at(full, *opt, ConfigFlowDomain::Process, 0) > 0.);
        }
    }

    // If the process preset is later given a high-flow column, Bambu's slot 1 ("Direct Drive High
    // Flow" on extruder 1) is what a high-flow filament reads - the two layouts agree on slots 0-1.
    full.option<ConfigOptionStrings>("process_flow_support", true)->values = { "standard", "high_flow" };
    full.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values.assign(1, int(fvtHighFlow));
    CHECK(get_config_idx(full, ConfigFlowDomain::Process, 0) == 1);
    CHECK(get_value_at(full, *full.option<ConfigOptionFloats>("outer_wall_acceleration"), ConfigFlowDomain::Process, 0) == Approx(2000.));
}

// ---------------------------------------------------------------------------------------------
// End-to-end: a real Bambu preset must survive the fork's own JSON loader.
// The fixtures are copies taken from the owner's Bambu Studio data; nothing reads the live
// folder at test time.
// ---------------------------------------------------------------------------------------------
static boost::filesystem::path fixture_dir()
{
    return boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat";
}

TEST_CASE("real Bambu presets load through the fork's json loader", "[BambuCompat]")
{
    const boost::filesystem::path dir = fixture_dir();
    if (! boost::filesystem::is_directory(dir)) {
        WARN("fixture directory missing: " << dir.string());
        return;
    }

    size_t seen = 0, loaded = 0, with_substitutions = 0;
    for (boost::filesystem::directory_iterator it(dir); it != boost::filesystem::directory_iterator(); ++it) {
        if (it->path().extension() != ".json")
            continue;
        ++seen;
        DynamicPrintConfig config;
        std::map<std::string, std::string> key_values;
        std::string reason;
        ConfigSubstitutions subst = config.load_from_json(
            it->path().string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
        INFO("preset " << it->path().filename().string() << " reason=" << reason);
        CHECK(reason.empty());
        if (reason.empty())
            ++loaded;
        if (! subst.empty())
            ++with_substitutions;

        // Whatever landed, no speed may have become zero unless Bambu wrote a zero.
        for (const char *key : {"inner_wall_speed", "outer_wall_speed", "sparse_infill_speed"}) {
            const ConfigOption *opt = config.option(key);
            if (opt == nullptr)
                continue;
            const std::string serialized = opt->serialize();
            INFO("preset " << it->path().filename().string() << " " << key << " = " << serialized);
            CHECK(serialized != "0");
        }
    }
    INFO("fixtures " << seen << ", loaded " << loaded << ", reporting substitutions " << with_substitutions);
    CHECK(seen > 0);
    CHECK(loaded == seen);
}

TEST_CASE("the 43 presets that were deleted on 2026-09-21 load again", "[BambuCompat]")
{
    // These are the exact files named in the owner's 2026-09-21-16-52-41.log.0 as
    // "parse config ... failed", recovered from the Bambu Studio originals. Before the
    // translation every one of them threw out of deserialize() and was deleted by
    // PresetCollection::load_presets. All 43 must now parse.
    const boost::filesystem::path dir = boost::filesystem::path(TEST_DATA_DIR) / "bambu_compat_43";
    if (! boost::filesystem::is_directory(dir)) {
        WARN("fixture directory missing: " << dir.string());
        return;
    }

    size_t seen = 0, loaded = 0, failed = 0, lossy_presets = 0;
    std::vector<std::string> failures;
    for (boost::filesystem::directory_iterator it(dir); it != boost::filesystem::directory_iterator(); ++it) {
        if (it->path().extension() != ".json")
            continue;
        ++seen;
        DynamicPrintConfig config;
        std::map<std::string, std::string> key_values;
        std::string reason;
        ConfigSubstitutions subst = config.load_from_json(
            it->path().string(), ForwardCompatibilitySubstitutionRule::Enable, key_values, reason);
        if (reason.empty()) {
            ++loaded;
            if (! subst.empty())
                ++lossy_presets;
        } else {
            ++failed;
            failures.push_back(it->path().filename().string() + ": " + reason);
        }
    }
    for (const std::string &f : failures)
        WARN("still failing: " << f);
    INFO("seen " << seen << ", loaded " << loaded << ", failed " << failed
         << ", reporting a lossy substitution " << lossy_presets);
    CHECK(seen == 43);
    CHECK(failed == 0);
    CHECK(loaded == seen);
}
