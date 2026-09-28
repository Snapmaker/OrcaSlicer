#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/FilamentSort.hpp"

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
        R"({"schema_version": 1, "sections": []})",
        R"({"schema_version": 1, "sections": {"filament_order": []}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": []}}})",
        R"({"schema_version": 1, "sections": {"filament_order": {"Snapmaker": [3]}}})",
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
