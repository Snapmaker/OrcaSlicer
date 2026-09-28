#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/FilamentSort.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

using namespace Slic3r::GUI;

namespace
{

FilamentSortItem make_item(const char *display_name,
                           const char *vendor,
                           const char *filament_product,
                           size_t original_index)
{
    FilamentSortItem item;
    item.display_name     = wxString::FromUTF8(display_name);
    item.vendor           = vendor;
    item.filament_product = filament_product;
    item.original_index   = original_index;
    return item;
}

FilamentOrder parse_order(const char *json)
{
    std::istringstream stream(json);
    return FilamentOrder::from_stream(stream);
}

} // namespace

TEST_CASE("FilamentOrder parses valid configuration and matches vendor and product case-insensitively",
          "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "high_flow": {
                "not_recommended_filaments": ["PLA Wood"],
                "unavailable_filaments": ["TPU 85A"]
            },
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    CHECK(order.rank("sNaPmAkEr", "PLA SnapSpeed") == 1);
    // Product names are authored in both the preset files and filament_allow_list.json, so a casing drift
    // must not drop the entry: they match case-insensitively, like the vendor key.
    CHECK(order.rank("Snapmaker", "pla matte") == 0);
    CHECK(order.rank("Snapmaker", "Pla sNApsPeed") == 1);
    CHECK(order.rank("sNaPmAkEr", "plA mATTe") == 0);
    CHECK(order.rank("Generic", "PLA Matte") == std::numeric_limits<size_t>::max());
    CHECK(order.rank("Snapmaker", "Unknown") == std::numeric_limits<size_t>::max());
}

TEST_CASE("FilamentOrder rejects invalid configurations and supports sort fallback", "[GUI][FilamentSort]")
{
    const std::vector<const char *> invalid_configs{
        "{",
        R"({"schema_version": 1, "order": {"Snapmaker": ["PLA Matte"]}})",
        R"({"schema_version": 2, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})",
        R"({"schema_version": 1.0, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})",
        R"({"schema_version": 1, "sections": []})",
        R"({"schema_version": 1, "sections": {}})",
        R"({"schema_version": 1, "sections": {"filament_order": []}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": []}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": [3]}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": [""]}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": "PLA Matte"}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"": ["PLA Matte"]}}})",
    };

    for (const char *json : invalid_configs)
    {
        CHECK(parse_order(json).empty());
    }

    const SystemFilamentSorter sorter(parse_order("{"));
    const FilamentSortItem first  = make_item("Alpha", "Snapmaker", "Unknown", 0);
    const FilamentSortItem second = make_item("Beta", "Snapmaker", "Unknown", 1);
    CHECK(sorter.less(first, second));
    CHECK_FALSE(sorter.less(second, first));
}

TEST_CASE("FilamentSorter applies case-insensitive name order and stable index tie-break", "[GUI][FilamentSort]")
{
    const FilamentSorter sorter;
    const FilamentSortItem alpha = make_item("alpha", "", "", 0);
    const FilamentSortItem beta  = make_item("Beta", "", "", 1);
    const FilamentSortItem first = make_item("PLA", "", "", 1);
    const FilamentSortItem second = make_item("pla", "", "", 2);

    CHECK(sorter.less(alpha, beta));
    CHECK_FALSE(sorter.less(beta, alpha));
    CHECK(sorter.less(first, second));
    CHECK_FALSE(sorter.less(second, first));
}

TEST_CASE("SystemFilamentVendorSorter prioritizes Snapmaker and Generic", "[GUI][FilamentSort]")
{
    const SystemFilamentVendorSorter sorter;

    CHECK(sorter.less("Snapmaker", "Generic"));
    CHECK(sorter.less("Generic", "Other"));
    CHECK(sorter.less("Another", "Other"));
    CHECK_FALSE(sorter.less("Other", "Generic"));
    CHECK_FALSE(sorter.less("SNAPMAKER", "Snapmaker"));
}

TEST_CASE("SystemFilamentSorter applies configured order only to Snapmaker", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");
    const SystemFilamentSorter sorter(order);

    const FilamentSortItem ordered_first = make_item("Z First", "Snapmaker", "PLA Matte", 0);
    const FilamentSortItem ordered_second = make_item("A Second", "Snapmaker", "PLA SnapSpeed", 1);
    const FilamentSortItem unknown = make_item("B Unknown", "Snapmaker", "ABS", 2);
    CHECK(sorter.less(ordered_first, ordered_second));
    CHECK(sorter.less(ordered_second, unknown));

    const FilamentSortItem generic_first = make_item("A Generic", "Generic", "PLA SnapSpeed", 3);
    const FilamentSortItem generic_second = make_item("B Generic", "Generic", "PLA Matte", 4);
    CHECK(sorter.less(generic_first, generic_second));
}

TEST_CASE("FilamentOrder keeps the first rank when a product name repeats", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA Matte", "PLA Matte", "PLA SnapSpeed"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    CHECK(order.rank("Snapmaker", "PLA SnapSpeed") == 2);
}

TEST_CASE("FilamentOrder matches non-ASCII product names", "[GUI][FilamentSort]")
{
    const FilamentOrder order = parse_order(R"({
        "schema_version": 1,
        "sections": {
            "filament_order": {
                "Snapmaker": ["PLA 哑光", "TPU 95A"]
            }
        }
    })");

    REQUIRE_FALSE(order.empty());
    CHECK(order.rank("Snapmaker", "PLA 哑光") == 0);
    CHECK(order.rank("Snapmaker", "TPU 95A") == 1);
    CHECK(order.rank("Snapmaker", "PLA 亮光") == std::numeric_limits<size_t>::max());
}

TEST_CASE("filament_product_key strips the vendor prefix and the printer suffix", "[GUI][FilamentSort]")
{
    CHECK(filament_product_key("Snapmaker PLA SnapSpeed", "Snapmaker") == "PLA SnapSpeed");

    // The strip ignores case, like the configured order's product matching: a casing drift between the
    // vendor label and the preset name must still map to the ordered entry.
    CHECK(filament_product_key("snapmaker PLA Matte", "Snapmaker") == "PLA Matte");
    CHECK(filament_product_key("SNAPMaker ABS", "Snapmaker") == "ABS");

    // Printer variants share one product entry.
    CHECK(filament_product_key("PLA Matte @BBL X1C", "Snapmaker") == "PLA Matte");

    // A word-interior coincidence is not a prefix (vendor "Prusa Polymers" against "Prusament ...").
    CHECK(filament_product_key("Prusament PVB @CORE One", "Prusa Polymers") == "Prusament PVB");

    // An unset vendor, or the schema placeholder, falls back to the preset name's leading word.
    CHECK(filament_product_key("Generic ABS", "") == "ABS");
    CHECK(filament_product_key("Generic ABS", "(Undefined)") == "ABS");
    CHECK(filament_product_key("SingleWord", "") == "SingleWord");
}

TEST_CASE("choose_allow_list_copy prefers the deployed user copy", "[GUI][FilamentSort]")
{
    const std::filesystem::path user_copy    = "system/Snapmaker/filament/filament_allow_list.json";
    const std::filesystem::path shipped_copy = "resources/profiles/Snapmaker/filament/filament_allow_list.json";

    CHECK(choose_allow_list_copy(user_copy, shipped_copy, true) == user_copy);
    // A missing user copy falls back to the shipped one instead of leaving the order unconfigured.
    CHECK(choose_allow_list_copy(user_copy, shipped_copy, false) == shipped_copy);
}

TEST_CASE("is_snapmaker_vendor matches the vendor label case-insensitively", "[GUI][FilamentSort]")
{
    // Preset files spell the vendor inconsistently, so the TopN gate must not depend on casing.
    CHECK(is_snapmaker_vendor("Snapmaker"));
    CHECK(is_snapmaker_vendor("SNAPMAKER"));
    CHECK(is_snapmaker_vendor("sNaPmAkEr"));

    // Only a whole-label match counts: neighbouring vendors must not take the Snapmaker path.
    CHECK_FALSE(is_snapmaker_vendor("Generic"));
    CHECK_FALSE(is_snapmaker_vendor("Snapmaker Lab"));
    CHECK_FALSE(is_snapmaker_vendor("Bambu Lab"));
    CHECK_FALSE(is_snapmaker_vendor(""));
}

TEST_CASE("canonical_vendor normalizes the known system vendors and passes others through", "[GUI][FilamentSort]")
{
    CHECK(canonical_vendor("snapmaker") == "Snapmaker");
    CHECK(canonical_vendor("SNAPMAKER") == "Snapmaker");
    CHECK(canonical_vendor("Snapmaker") == "Snapmaker");
    CHECK(canonical_vendor("generic") == "Generic");
    CHECK(canonical_vendor("GENERIC") == "Generic");

    // Unknown vendors keep their spelling; the empty label stays empty rather than becoming a vendor.
    CHECK(canonical_vendor("Bambu Lab") == "Bambu Lab");
    CHECK(canonical_vendor("Snapmaker Lab") == "Snapmaker Lab");
    CHECK(canonical_vendor("") == "");

    // System rows are grouped by the canonical label, so the TopN gate must agree with it.
    CHECK(is_snapmaker_vendor(canonical_vendor("snapmaker")));
    CHECK_FALSE(is_snapmaker_vendor(canonical_vendor("generic")));
}

TEST_CASE("FilamentOrder::from_file reads a configuration file and reports unusable ones as empty",
          "[GUI][FilamentSort]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "filament_order_from_file_test.json";

    SECTION("valid configuration")
    {
        std::ofstream stream(path);
        REQUIRE(stream.good());
        stream << R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": ["PLA Matte"]}}})";
        // Close before reading back: the write is not on disk while the stream still buffers it.
        stream.close();

        const FilamentOrder order = FilamentOrder::from_file(path);
        CHECK_FALSE(order.empty());
        CHECK(order.rank("Snapmaker", "PLA Matte") == 0);
    }

    SECTION("missing file")
    {
        std::filesystem::remove(path);
        std::string reason;
        CHECK(FilamentOrder::from_file(path, &reason).empty());
        // The loader logs this reason, so a file that is not there stays distinguishable from one
        // that is there but rejected.
        CHECK(reason == "cannot be opened");
    }

    SECTION("malformed file")
    {
        std::ofstream stream(path);
        REQUIRE(stream.good());
        stream << "{";
        stream.close();

        std::string reason;
        CHECK(FilamentOrder::from_file(path, &reason).empty());
        CHECK(reason == "has an invalid or empty configuration");
    }

    std::filesystem::remove(path);
}

TEST_CASE("the shipped allow-list parses and orders the Snapmaker products", "[GUI][FilamentSort]")
{
    // The shipped file is a data contract with PresetUpdater, which deploys it to the user data
    // directory. A typo in it (schema version, vendor key, syntax) would otherwise drop the whole
    // vendor order at runtime without any visible error.
    std::string         reason;
    const FilamentOrder order = FilamentOrder::from_file(FILAMENT_ALLOW_LIST_FILE, &reason);
    INFO("loader reason: " << reason);
    REQUIRE_FALSE(order.empty());

    // A shipped product proves the vendor key matched; an unknown one proves the name fallback.
    CHECK(order.rank("Snapmaker", "PLA SnapSpeed") != std::numeric_limits<size_t>::max());
    CHECK(order.rank("Snapmaker", "Not A Shipped Product") == std::numeric_limits<size_t>::max());
}
