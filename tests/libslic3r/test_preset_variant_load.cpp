// Preset / per-variant load robustness (upstream OrcaSlicer batch 1D: #13316, #14106, #14103).
//
// Upstream merges a child preset onto its parent per extruder variant
// (DynamicPrintConfig::update_non_diff_values_to_base_config / update_diff_values_to_child_config).
// Those merges assumed child and parent vectors had matching sizes: #13316 truncated a child that
// had more extruders than its parent, #14106 threw on a mismatch and the caller deleted the user's
// preset file. This tree has neither function: PresetCollection::load_presets (and the project
// preset loader) start from the parent's config and replace whole options with the child's
// (ConfigBase::apply), then Preset::normalize only ever grows short vectors. These tests pin that
// behaviour through the real loader, for the shapes upstream's regressions used.

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetFlowVariant.hpp"
#include "libslic3r/PresetQuarantine.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

// A scratch preset tree that removes itself. Never points at a user's data directory.
struct ScratchTree
{
    fs::path root;

    explicit ScratchTree(const std::string &tag)
    {
        root = fs::temp_directory_path() / fs::unique_path("orca_variant_load_" + tag + "_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScratchTree()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path write(const std::string &rel, const std::string &content) const
    {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p.string(), std::ios::binary);
        ofs << content;
        return p;
    }
};

// Installs `config` as a system preset called `name` in `collection`, the way a vendor bundle
// provides the parent of a user preset.
Preset &add_system_parent(PresetCollection &collection, const std::string &name, const DynamicPrintConfig &config)
{
    Preset &parent    = collection.load_preset("", name, config, false);
    parent.is_system  = true;
    parent.is_visible = true;
    return parent;
}

// Runs the user-preset loader over <root>/<subdir>.
void load_user_dir(PresetCollection &collection, const ScratchTree &tree, const std::string &subdir)
{
    PresetsConfigSubstitutions substitutions;
    collection.load_presets(tree.root.string(), subdir, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
}

template<class T> std::vector<T> values_of(const Preset &preset, const std::string &key)
{
    const auto *opt = dynamic_cast<const ConfigOptionVector<T> *>(preset.config.option(key));
    REQUIRE(opt != nullptr);
    return opt->values;
}

DynamicPrintConfig single_nozzle_parent(const PresetBundle &bundle)
{
    DynamicPrintConfig parent = bundle.printers.default_preset().config;
    parent.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    parent.set_key_value("printer_extruder_id", new ConfigOptionInts({1}));
    parent.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard"}));
    parent.set_key_value("retraction_length", new ConfigOptionFloats({0.8}));
    return parent;
}

} // namespace

// Orca #13316: an IDEX / two-tool user printer inheriting a single-nozzle base used to come back with
// its per-extruder vectors cut to the parent's single entry.
TEST_CASE("a child printer preset with more extruders than its parent keeps its vectors", "[PresetVariantLoad]")
{
    const bool semm = GENERATE(false, true);
    PresetBundle bundle;
    add_system_parent(bundle.printers, "Parent 1N", single_nozzle_parent(bundle));

    ScratchTree tree("idex");
    tree.write("machine/Child 2N.json", std::string("{\n") +
        "  \"type\": \"machine\",\n"
        "  \"name\": \"Child 2N\",\n"
        "  \"from\": \"User\",\n"
        "  \"version\": \"2.0.0.0\",\n"
        "  \"inherits\": \"Parent 1N\",\n"
        "  \"single_extruder_multi_material\": \"" + (semm ? "1" : "0") + "\",\n"
        "  \"nozzle_diameter\": [\"0.4\", \"0.4\"],\n"
        "  \"printer_extruder_id\": [\"1\", \"2\"],\n"
        "  \"printer_extruder_variant\": [\"Direct Drive Standard\", \"Direct Drive Standard\"],\n"
        "  \"retraction_length\": [\"1.5\", \"1.5\"]\n"
        "}\n");

    PresetQuarantine::take();
    load_user_dir(bundle.printers, tree, "machine");

    const Preset *child = bundle.printers.find_preset("Child 2N", false);
    REQUIRE(child != nullptr);
    CHECK(values_of<double>(*child, "nozzle_diameter") == std::vector<double>{0.4, 0.4});
    CHECK(values_of<int>(*child, "printer_extruder_id") == std::vector<int>{1, 2});
    CHECK(values_of<std::string>(*child, "printer_extruder_variant").size() == 2);
    CHECK(values_of<double>(*child, "retraction_length") == std::vector<double>{1.5, 1.5});
    CHECK(PresetQuarantine::peek().empty());
    PresetQuarantine::take();

    // The parent stays a single-nozzle printer: nothing of the child leaks into it.
    const Preset *parent = bundle.printers.find_preset("Parent 1N", false);
    REQUIRE(parent != nullptr);
    CHECK(values_of<int>(*parent, "printer_extruder_id") == std::vector<int>{1});
    CHECK(values_of<double>(*parent, "retraction_length") == std::vector<double>{0.8});
}

// The same child saved as a difference to its parent and loaded again must come back unchanged
// (Preset::save writes whole options, never a slice sized by the parent).
TEST_CASE("a child printer preset with more extruders than its parent survives save and reload", "[PresetVariantLoad]")
{
    PresetBundle bundle;
    const Preset &parent = add_system_parent(bundle.printers, "Parent 1N", single_nozzle_parent(bundle));

    ScratchTree tree("idex_roundtrip");
    Preset child(Preset::TYPE_PRINTER, "Child 2N", false);
    child.config = parent.config;
    child.config.set_key_value("inherits", new ConfigOptionString("Parent 1N"));
    child.config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    child.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.6}));
    child.config.set_key_value("printer_extruder_id", new ConfigOptionInts({1, 2}));
    child.config.set_key_value("printer_extruder_variant",
                               new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    child.config.set_key_value("retraction_length", new ConfigOptionFloats({1.5, 2.5}));
    child.version = Semver(std::string("2.0.0"));
    child.file    = (tree.root / "machine" / "Child 2N.json").string();
    DynamicPrintConfig parent_config = parent.config;
    REQUIRE(child.save(&parent_config));

    PresetBundle reloaded;
    add_system_parent(reloaded.printers, "Parent 1N", single_nozzle_parent(reloaded));
    load_user_dir(reloaded.printers, tree, "machine");

    const Preset *loaded = reloaded.printers.find_preset("Child 2N", false);
    REQUIRE(loaded != nullptr);
    CHECK(values_of<double>(*loaded, "nozzle_diameter") == std::vector<double>{0.4, 0.6});
    CHECK(values_of<int>(*loaded, "printer_extruder_id") == std::vector<int>{1, 2});
    CHECK(values_of<double>(*loaded, "retraction_length") == std::vector<double>{1.5, 2.5});
}

// The filament side of #13316 in this tree's flow-variant layout: a user filament that declares
// Standard + High Flow on top of a Standard-only parent keeps both columns.
TEST_CASE("a child filament with more flow variants than its parent keeps both columns", "[PresetVariantLoad][FilamentVariants]")
{
    PresetBundle bundle;
    DynamicPrintConfig parent = bundle.filaments.default_preset().config;
    parent.set_key_value("filament_flow_support", new ConfigOptionStrings({FLOW_MODE_STANDARD}));
    parent.set_key_value("nozzle_temperature", new ConfigOptionInts({220}));
    parent.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({12.}));
    add_system_parent(bundle.filaments, "Parent PLA", parent);

    ScratchTree tree("flowvariants");
    tree.write("filament/Child PLA HF.json", std::string("{\n") +
        "  \"type\": \"filament\",\n"
        "  \"name\": \"Child PLA HF\",\n"
        "  \"from\": \"User\",\n"
        "  \"version\": \"2.0.0.0\",\n"
        "  \"inherits\": \"Parent PLA\",\n"
        "  \"filament_flow_support\": [\"standard\", \"high_flow\"],\n"
        "  \"nozzle_temperature\": [\"205\", \"235\"],\n"
        "  \"filament_max_volumetric_speed\": [\"15\", \"28\"]\n"
        "}\n");

    load_user_dir(bundle.filaments, tree, "filament");

    const Preset *child = bundle.filaments.find_preset("Child PLA HF", false);
    REQUIRE(child != nullptr);
    CHECK(values_of<int>(*child, "nozzle_temperature") == std::vector<int>{205, 235});
    CHECK(values_of<double>(*child, "filament_max_volumetric_speed") == std::vector<double>{15., 28.});

    // Composed into a project, the segment takes the declared columns in order.
    ConfigOptionInts composed({0, 0, 0});
    compose_filament_flow_variant_segment(composed, *child->config.option<ConfigOptionInts>("nozzle_temperature"), 1, 2);
    CHECK(composed.values == std::vector<int>{0, 205, 235});
}

namespace {

std::string stride2_limits(const std::string &value, size_t count)
{
    std::string out = "[";
    for (size_t i = 0; i < count; ++i)
        out += (i ? ", \"" : "\"") + value + "\"";
    return out + "]";
}

} // namespace

// Orca #14106: a user printer preset on a 4-nozzle non-Bambu base whose stride-2 machine limits had
// been length-extended to nozzles*2 while the base carried no printer_extruder_variant. Upstream's
// set_only_diff threw "invalid diff_index size" and the loader deleted the file. Here the preset must
// load, keep its own limits, and never be quarantined.
TEST_CASE("a user printer preset whose machine limits are longer than its parent's loads intact", "[PresetVariantLoad]")
{
    PresetBundle bundle;
    DynamicPrintConfig parent = bundle.printers.default_preset().config;
    parent.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));
    parent.set_key_value("machine_max_acceleration_x",
                         new ConfigOptionFloats({25000, 25000, 25000, 25000, 25000, 25000, 25000, 25000}));
    parent.set_key_value("printer_extruder_variant", new ConfigOptionStrings());
    add_system_parent(bundle.printers, "Parent 4N", parent);

    ScratchTree tree("limits");
    const fs::path file = tree.write("machine/Child 4N.json", std::string("{\n") +
        "  \"type\": \"machine\",\n"
        "  \"name\": \"Child 4N\",\n"
        "  \"from\": \"User\",\n"
        "  \"version\": \"2.0.0.0\",\n"
        "  \"inherits\": \"Parent 4N\",\n"
        "  \"printer_extruder_id\": [\"1\", \"2\", \"3\", \"4\"],\n"
        "  \"printer_extruder_variant\": " + stride2_limits("Direct Drive Standard", 4) + ",\n"
        "  \"machine_max_acceleration_x\": " + stride2_limits("8000", 8) + ",\n"
        "  \"machine_max_acceleration_y\": " + stride2_limits("7000", 3) + "\n"
        "}\n");

    PresetQuarantine::take();
    load_user_dir(bundle.printers, tree, "machine");

    // Never destroyed, never moved aside.
    CHECK(fs::exists(file));
    CHECK(PresetQuarantine::peek().empty());
    PresetQuarantine::take();

    const Preset *child = bundle.printers.find_preset("Child 4N", false);
    REQUIRE(child != nullptr);
    CHECK(values_of<double>(*child, "machine_max_acceleration_x") == std::vector<double>(8, 8000.));
    // An odd-length override (neither nozzles nor nozzles*2) is kept as written, too.
    CHECK(values_of<double>(*child, "machine_max_acceleration_y") == std::vector<double>(3, 7000.));
    CHECK(values_of<int>(*child, "printer_extruder_id") == std::vector<int>{1, 2, 3, 4});
}

// The other direction: a user preset saved when its printer had fewer extruders (or fewer variants)
// than the parent has now. It loads with its own shorter vectors, compares against the parent per
// slot without reading past either end, and a dirty "key#i" slot transfers onto a longer config.
TEST_CASE("a user printer preset with shorter vectors than its parent loads, diffs and transfers", "[PresetVariantLoad]")
{
    PresetBundle bundle;
    DynamicPrintConfig parent = bundle.printers.default_preset().config;
    parent.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    parent.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    parent.set_key_value("retraction_length", new ConfigOptionFloats({0.8, 0.8}));
    parent.set_key_value("machine_max_acceleration_x", new ConfigOptionFloats({20000, 20000, 20000, 20000}));
    add_system_parent(bundle.printers, "Parent 2N", parent);

    ScratchTree tree("shorter");
    const fs::path file = tree.write("machine/Old Child.json", std::string("{\n") +
        "  \"type\": \"machine\",\n"
        "  \"name\": \"Old Child\",\n"
        "  \"from\": \"User\",\n"
        "  \"version\": \"1.9.0.0\",\n"
        "  \"inherits\": \"Parent 2N\",\n"
        "  \"retraction_length\": [\"1.5\"],\n"
        "  \"machine_max_acceleration_x\": [\"9000\", \"9000\"]\n"
        "}\n");

    PresetQuarantine::take();
    load_user_dir(bundle.printers, tree, "machine");
    CHECK(fs::exists(file));
    CHECK(PresetQuarantine::peek().empty());
    PresetQuarantine::take();

    // Look both up again: loading sorts the collection, so references taken before it are stale.
    const Preset *child = bundle.printers.find_preset("Old Child", false);
    REQUIRE(child != nullptr);
    const Preset *parent_found = bundle.printers.find_preset("Parent 2N", false);
    REQUIRE(parent_found != nullptr);
    const Preset &parent_preset = *parent_found;
    CHECK(values_of<double>(*child, "retraction_length") == std::vector<double>{1.5});
    CHECK(values_of<double>(*child, "machine_max_acceleration_x") == std::vector<double>{9000., 9000.});

    // Deep (per-slot) compare in both directions: no throw, no read past the shorter vector.
    std::vector<std::string> dirty;
    REQUIRE_NOTHROW(dirty = PresetCollection::dirty_options(child, &parent_preset, /*deep_compare=*/true));
    CHECK(std::find(dirty.begin(), dirty.end(), "retraction_length#0") != dirty.end());
    CHECK(std::find(dirty.begin(), dirty.end(), "machine_max_acceleration_x#1") != dirty.end());
    REQUIRE_NOTHROW(PresetCollection::dirty_options(&parent_preset, child, /*deep_compare=*/true));

    // Transferring those slots onto the longer parent writes only the named slots.
    DynamicPrintConfig transferred = parent_preset.config;
    REQUIRE_NOTHROW(transferred.apply_only(child->config, dirty));
    CHECK(transferred.option<ConfigOptionFloats>("retraction_length")->values == std::vector<double>{1.5, 0.8});
    CHECK(transferred.option<ConfigOptionFloats>("machine_max_acceleration_x")->values ==
          std::vector<double>{9000., 9000., 20000., 20000.});

    // Save as a difference to the parent and load again: the short vectors survive.
    Preset resaved = *child;
    resaved.file   = (tree.root / "machine2" / "Old Child.json").string();
    DynamicPrintConfig parent_config = parent_preset.config;
    REQUIRE(resaved.save(&parent_config));

    PresetBundle reloaded;
    add_system_parent(reloaded.printers, "Parent 2N", parent);
    load_user_dir(reloaded.printers, tree, "machine2");
    const Preset *again = reloaded.printers.find_preset("Old Child", false);
    REQUIRE(again != nullptr);
    CHECK(values_of<double>(*again, "retraction_length") == std::vector<double>{1.5});
    CHECK(values_of<double>(*again, "machine_max_acceleration_x") == std::vector<double>{9000., 9000.});
}
