#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <sstream>
#include <vector>

using namespace Slic3r;

namespace {

static std::vector<std::string> split_rows(const std::string &serialized)
{
    std::vector<std::string> rows;
    std::stringstream ss(serialized);
    std::string row;
    while (std::getline(ss, row, ';')) {
        if (!row.empty())
            rows.push_back(row);
    }
    return rows;
}

static std::string join_rows(const std::vector<std::string> &rows)
{
    std::ostringstream ss;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i != 0)
            ss << ';';
        ss << rows[i];
    }
    return ss.str();
}

static unsigned int virtual_id_for_stable_id(const std::vector<MixedFilament> &mixed, size_t num_physical, uint64_t stable_id)
{
    unsigned int next_virtual_id = unsigned(num_physical + 1);
    for (const MixedFilament &mf : mixed) {
        if (!mf.enabled || mf.deleted)
            continue;
        if (mf.stable_id == stable_id)
            return next_virtual_id;
        ++next_virtual_id;
    }
    return 0;
}

} // namespace

TEST_CASE("Mixed filament remap follows stable row ids when same-pair rows reorder", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    bundle.update_multi_material_filament_presets();

    auto &mgr = bundle.mixed_filaments;
    auto &mixed = mgr.mixed_filaments();
    REQUIRE(mixed.size() == 1);

    mixed[0].deleted = true;
    mixed[0].enabled = false;

    const auto colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.add_custom_filament(1, 2, 25, colors);
    mgr.add_custom_filament(1, 2, 75, colors);

    auto &old_mixed = mgr.mixed_filaments();
    REQUIRE(old_mixed.size() == 3);
    REQUIRE(old_mixed[1].enabled);
    REQUIRE(old_mixed[2].enabled);
    const uint64_t first_custom_id = old_mixed[1].stable_id;
    const uint64_t second_custom_id = old_mixed[2].stable_id;

    std::vector<std::string> rows = split_rows(mgr.serialize_custom_entries());
    REQUIRE(rows.size() == 3);
    std::swap(rows[1], rows[2]);

    auto *definitions = bundle.project_config.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(definitions != nullptr);
    definitions->value = join_rows(rows);

    bundle.filament_presets.push_back(bundle.filament_presets.back());
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.push_back("#00FF00");
    bundle.update_multi_material_filament_presets(size_t(-1), 2);

    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();
    REQUIRE(remap.size() >= 5);

    const auto &rebuilt = bundle.mixed_filaments.mixed_filaments();
    const unsigned int new_first_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, first_custom_id);
    const unsigned int new_second_custom_virtual_id = virtual_id_for_stable_id(rebuilt, 3, second_custom_id);

    REQUIRE(new_first_custom_virtual_id != 0);
    REQUIRE(new_second_custom_virtual_id != 0);
    CHECK(remap[3] == new_first_custom_virtual_id);
    CHECK(remap[4] == new_second_custom_virtual_id);
}

TEST_CASE("Mixed filament remap keeps later painted colors stable when an earlier mixed row is deleted", "[MixedFilament]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"Default Filament", "Default Filament", "Default Filament", "Default Filament"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    bundle.update_multi_material_filament_presets();

    auto &mixed = bundle.mixed_filaments.mixed_filaments();
    REQUIRE(mixed.size() >= 10);

    const uint64_t stable_id_6 = mixed[1].stable_id;
    const uint64_t stable_id_7 = mixed[2].stable_id;
    const uint64_t stable_id_8 = mixed[3].stable_id;

    const std::vector<MixedFilament> old_mixed = mixed;
    mixed[0].enabled = false;
    mixed[0].deleted = true;

    bundle.update_mixed_filament_id_remap(old_mixed, 4, 4);
    const std::vector<unsigned int> remap = bundle.consume_last_filament_id_remap();

    REQUIRE(remap.size() >= 15);
    CHECK(remap[6] == virtual_id_for_stable_id(mixed, 4, stable_id_6));
    CHECK(remap[7] == virtual_id_for_stable_id(mixed, 4, stable_id_7));
    CHECK(remap[8] == virtual_id_for_stable_id(mixed, 4, stable_id_8));
}

TEST_CASE("Mixed filament grouped manual patterns normalize and round-trip", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#FF0000", "#0000FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("1/1/1/1/1/1/1/2, 1/1/1/2/1/1/1/1");
    REQUIRE(row.manual_pattern == "11111112,11121111");

    const std::string serialized = mgr.serialize_custom_entries();

    MixedFilamentManager loaded;
    loaded.load_custom_entries(serialized, colors);
    REQUIRE(loaded.mixed_filaments().size() == 1);
    CHECK(loaded.mixed_filaments().front().manual_pattern == "11111112,11121111");
    CHECK(loaded.mixed_filaments().front().mix_b_percent == 13);
}

TEST_CASE("Mixed filament perimeter resolver uses grouped manual patterns by inset", "[MixedFilament]")
{
    const std::vector<std::string> colors = {"#00FFFF", "#FF00FF"};

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);

    MixedFilament &row = mgr.mixed_filaments().front();
    row.manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(row.manual_pattern == "12,21");

    const unsigned int mixed_filament_id = 3;
    CHECK(mgr.resolve(mixed_filament_id, 2, 0) == 1);
    CHECK(mgr.resolve(mixed_filament_id, 2, 1) == 2);

    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 0) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 0) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 1) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 1) == 1);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 0, 3) == 2);
    CHECK(mgr.resolve_perimeter(mixed_filament_id, 2, 1, 3) == 1);

    const std::vector<unsigned int> ordered_layer0 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 0);
    const std::vector<unsigned int> ordered_layer1 = mgr.ordered_perimeter_extruders(mixed_filament_id, 2, 1);
    REQUIRE(ordered_layer0.size() == 2);
    REQUIRE(ordered_layer1.size() == 2);
    CHECK(ordered_layer0[0] == 1);
    CHECK(ordered_layer0[1] == 2);
    CHECK(ordered_layer1[0] == 2);
    CHECK(ordered_layer1[1] == 1);
}

TEST_CASE("ExtrusionPath copies preserve inset index", "[MixedFilament]")
{
    ExtrusionPath src(erPerimeter);
    src.inset_idx = 3;

    ExtrusionPath copied(src);
    CHECK(copied.inset_idx == 3);

    ExtrusionPath assigned(erExternalPerimeter);
    assigned.inset_idx = 0;
    assigned = src;
    CHECK(assigned.inset_idx == 3);
}
