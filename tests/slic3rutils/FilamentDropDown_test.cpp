#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/FilamentDropDown.hpp"

namespace
{

FilamentDropDown::Item make_item(const char *text, const char *group)
{
    FilamentDropDown::Item item;
    item.text  = wxString::FromUTF8(text);
    item.group = wxString::FromUTF8(group);
    return item;
}

wxString strip_prefix(const char *text, const char *group)
{
    return FilamentDropDown::strip_group_prefix(wxString::FromUTF8(text), wxString::FromUTF8(group));
}

} // namespace

TEST_CASE("FilamentDropDown maps grouped rows to item indices", "[GUI][FilamentDropDown]")
{
    std::vector<FilamentDropDown::Item> items{
        make_item("PLA A1", "Vendor A"),
        make_item("PLA A2", "Vendor A"),
        make_item("PLA B1", "Vendor B"),
        make_item("Loose", "")};

    const std::vector<FilamentDropDown::VisibleRow> rows =
        FilamentDropDown::build_visible_rows(items, wxString());

    REQUIRE(rows.size() == 3);
    CHECK(rows[0].item_index == 0);
    CHECK(rows[0].group_header);
    CHECK(rows[1].item_index == 2);
    CHECK(rows[1].group_header);
    CHECK(rows[2].item_index == 3);
    CHECK_FALSE(rows[2].group_header);

    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 0) == -2);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 1) == -4);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 2) == 3);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, -1) == -1);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 3) == -1);

    CHECK(FilamentDropDown::visible_row_for_item(rows, 0) == 0);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 1) == -1);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 2) == 1);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 3) == 2);

    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 0) == 0);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 1) == 0);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 2) == 1);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 3) == 2);
}

TEST_CASE("FilamentDropDown maps submenu rows without group sentinels", "[GUI][FilamentDropDown]")
{
    std::vector<FilamentDropDown::Item> items{
        make_item("PLA A1", "Vendor A"),
        make_item("PLA A2", "Vendor A"),
        make_item("PLA B1", "Vendor B")};

    const std::vector<FilamentDropDown::VisibleRow> rows =
        FilamentDropDown::build_visible_rows(items, wxString::FromUTF8("Vendor A"));

    REQUIRE(rows.size() == 2);
    CHECK_FALSE(rows[0].group_header);
    CHECK_FALSE(rows[1].group_header);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 0) == 0);
    CHECK(FilamentDropDown::item_index_for_visible_row(rows, 1) == 1);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 0) == 0);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 1) == 1);
    CHECK(FilamentDropDown::visible_row_for_item(rows, 2) == -1);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString::FromUTF8("Vendor A"), 0) == 0);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString::FromUTF8("Vendor A"), 1) == 1);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString::FromUTF8("Vendor A"), 2) == -1);
}

TEST_CASE("FilamentDropDown preserves singleton and folded group selection", "[GUI][FilamentDropDown]")
{
    std::vector<FilamentDropDown::Item> items{
        make_item("Single", "Vendor A"),
        make_item("First", "Vendor B"),
        make_item("Second", "Vendor B")};

    const std::vector<FilamentDropDown::VisibleRow> rows =
        FilamentDropDown::build_visible_rows(items, wxString());

    REQUIRE(rows.size() == 2);
    CHECK(rows[0].item_index == 0);
    CHECK(rows[0].group_header);
    CHECK(rows[1].item_index == 1);
    CHECK(rows[1].group_header);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 0) == 0);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 1) == 1);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString(), 2) == 1);

    const std::vector<FilamentDropDown::VisibleRow> submenu_rows =
        FilamentDropDown::build_visible_rows(items, wxString::FromUTF8("Vendor B"));
    REQUIRE(submenu_rows.size() == 2);
    CHECK_FALSE(submenu_rows[0].group_header);
    CHECK_FALSE(submenu_rows[1].group_header);
    CHECK(FilamentDropDown::selected_row_for_item(items, wxString::FromUTF8("Vendor B"), 2) == 1);
}

TEST_CASE("FilamentDropDown preserves mapping for rows reached after scrolling", "[GUI][FilamentDropDown]")
{
    std::vector<FilamentDropDown::Item> items;
    for (int index = 0; index < 20; ++index)
    {
        items.push_back(make_item("Loose", ""));
    }

    const std::vector<FilamentDropDown::VisibleRow> rows =
        FilamentDropDown::build_visible_rows(items, wxString());

    REQUIRE(rows.size() == items.size());
    const int scrolled_row = 15;
    const int item_index = FilamentDropDown::item_index_for_visible_row(rows, scrolled_row);
    CHECK(item_index == scrolled_row);
    CHECK(FilamentDropDown::visible_row_for_item(rows, item_index) == scrolled_row);

    for (int row = 0; row < static_cast<int>(rows.size()); ++row)
    {
        const int mapped_item = FilamentDropDown::item_index_for_visible_row(rows, row);
        REQUIRE(mapped_item >= 0);
        CHECK(FilamentDropDown::visible_row_for_item(rows, mapped_item) == row);
    }
}

TEST_CASE("FilamentDropDown strips whole group prefixes only", "[GUI][FilamentDropDown]")
{
    // Single-word vendor: the vendor prefix is removed.
    CHECK(strip_prefix("Snapmaker PLA SnapSpeed", "Snapmaker") == wxString::FromUTF8("PLA SnapSpeed"));
    CHECK(strip_prefix("Generic ABS", "Generic") == wxString::FromUTF8("ABS"));

    // Multi-word vendor: the whole vendor name is removed before the first-word fallback.
    CHECK(strip_prefix("PolyLite Pro PLA", "PolyLite Pro") == wxString::FromUTF8("PLA"));

    // First-word fallback when the preset name uses only the vendor's leading word.
    CHECK(strip_prefix("PolyLite PLA", "PolyLite Pro") == wxString::FromUTF8("PLA"));

    // Word-interior coincidence must not be stripped (group "Prusa Polymers" vs "Prusament ...").
    CHECK(strip_prefix("Prusament PVB @CORE One", "Prusa Polymers") == wxString::FromUTF8("Prusament PVB @CORE One"));

    // Preset names that do not start with the vendor name stay unchanged.
    CHECK(strip_prefix("Arena ABS @Arena X1C", "Orca Arena") == wxString::FromUTF8("Arena ABS @Arena X1C"));

    // Project/User pseudo-groups carry a trailing space and never strip.
    CHECK(strip_prefix("PLA Basic", "Custom ") == wxString::FromUTF8("PLA Basic"));
    CHECK(strip_prefix("PLA Basic", "Project ") == wxString::FromUTF8("PLA Basic"));
}
