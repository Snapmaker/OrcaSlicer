#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "slic3r/GUI/Gizmos/EmbossFaceList.hpp"

using namespace Slic3r::GUI;

// Text tool font search (2026-10-06): typing in the font combo's search box blanked the whole list.
// The search filters Facenames::faces_names and hands back indices that pick rows out of
// Facenames::faces. The font-list cache (load()) filled faces but not faces_names, so on every run
// after the first the search ran over an empty list and matched nothing.

namespace {

struct TestFace
{
    std::string name;
    int         texture_index = 0;
};

std::string name_of(const TestFace &f) { return f.name; }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Same rule as ImGuiWrapper::bbl_combo_with_filter: case-insensitive substring, earlier match first,
// list order kept between matches at the same position.
std::vector<int> search(const std::string &pattern, const std::vector<std::string> &all_items)
{
    std::vector<std::pair<int, int>> hits;
    const std::string p = lower(pattern);
    for (int i = 0; i < int(all_items.size()); ++i) {
        size_t pos = lower(all_items[i]).find(p);
        if (pos != std::string::npos)
            hits.push_back({i, int(pos)});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) { return b.second > a.second; });
    std::vector<int> idx;
    for (const auto &h : hits)
        idx.push_back(h.first);
    return idx;
}

std::vector<std::string> shown_names(const std::vector<TestFace> &faces, const std::vector<int> &rows)
{
    std::vector<std::string> out;
    for (int r : rows)
        out.push_back(faces[r].name);
    return out;
}

std::vector<TestFace> sample_faces()
{
    return {{"Arial"}, {"Arial Black"}, {"Bahnschrift"}, {"Comic Sans MS"}, {"Consolas"}, {"Noto Sans Symbols 2"}, {"Segoe UI"}};
}

} // namespace

TEST_CASE("Font search over a list restored from the cache finds fonts", "[EmbossFontSearch]")
{
    // What load() produces: faces filled, names not.
    std::vector<TestFace>    faces = sample_faces();
    std::vector<std::string> names;

    SECTION("before the fix: an empty name list matches nothing") {
        std::vector<int> filtered = search("con", names);
        REQUIRE(emboss_face_list::visible_rows(true, filtered, faces.size()).empty());
    }

    SECTION("after syncing, the search shows the matching fonts by name") {
        REQUIRE(emboss_face_list::sync_names(faces, names, name_of));
        REQUIRE(names.size() == faces.size());
        std::vector<int> rows = emboss_face_list::visible_rows(true, search("con", names), faces.size());
        REQUIRE(shown_names(faces, rows) == std::vector<std::string>{"Consolas"});

        rows = emboss_face_list::visible_rows(true, search("SANS", names), faces.size());
        REQUIRE(shown_names(faces, rows) == std::vector<std::string>{"Comic Sans MS", "Noto Sans Symbols 2"});

        // matches at the start come first, then list order
        rows = emboss_face_list::visible_rows(true, search("a", names), faces.size());
        REQUIRE(shown_names(faces, rows).front() == "Arial");
        REQUIRE(shown_names(faces, rows)[1] == "Arial Black");
    }

    SECTION("clearing the search shows every font again") {
        emboss_face_list::sync_names(faces, names, name_of);
        std::vector<int> rows = emboss_face_list::visible_rows(false, {}, faces.size());
        REQUIRE(rows.size() == faces.size());
        REQUIRE(shown_names(faces, rows).back() == "Segoe UI");
    }
}

TEST_CASE("sync_names leaves an in-step list alone and repairs a stale one", "[EmbossFontSearch]")
{
    std::vector<TestFace>    faces = sample_faces();
    std::vector<std::string> names;
    REQUIRE(emboss_face_list::sync_names(faces, names, name_of));
    REQUIRE_FALSE(emboss_face_list::sync_names(faces, names, name_of));

    // Same size, different content (e.g. the OS font list changed): rebuilt, not trusted by size.
    names[2] = "Wingdings";
    REQUIRE(emboss_face_list::sync_names(faces, names, name_of));
    REQUIRE(names[2] == "Bahnschrift");

    // A second load from the cache must not double the list.
    names.insert(names.end(), names.begin(), names.end());
    REQUIRE(emboss_face_list::sync_names(faces, names, name_of));
    REQUIRE(names.size() == faces.size());
}

TEST_CASE("visible_rows never yields an index past the faces list", "[EmbossFontSearch]")
{
    REQUIRE(emboss_face_list::visible_rows(true, {0, 5, -1, 2, 9}, 3) == std::vector<int>{0, 2});
    REQUIRE(emboss_face_list::visible_rows(false, {7}, 2) == std::vector<int>{0, 1});
    REQUIRE(emboss_face_list::visible_rows(true, {}, 4).empty());
}

TEST_CASE("Removing an unloadable font keeps faces and names together", "[EmbossFontSearch]")
{
    std::vector<TestFace>    faces = sample_faces();
    std::vector<std::string> names;
    emboss_face_list::sync_names(faces, names, name_of);

    // "Consolas" can't be selected -> removed from both lists
    REQUIRE(emboss_face_list::erase_face(faces, names, 4));
    REQUIRE(faces.size() == 6);
    REQUIRE(names.size() == 6);
    for (size_t i = 0; i < faces.size(); ++i)
        REQUIRE(names[i] == faces[i].name);
    REQUIRE(emboss_face_list::visible_rows(true, search("con", names), faces.size()).empty());
    REQUIRE(shown_names(faces, emboss_face_list::visible_rows(true, search("seg", names), faces.size())) ==
            std::vector<std::string>{"Segoe UI"});

    // Out of range: nothing changes.
    REQUIRE_FALSE(emboss_face_list::erase_face(faces, names, 6));
    REQUIRE(faces.size() == 6);

    // Names out of step (the old cache path): the face goes, names are dropped for a rebuild
    // instead of erasing a slot that does not exist.
    names.clear();
    REQUIRE(emboss_face_list::erase_face(faces, names, 0));
    REQUIRE(faces.size() == 5);
    REQUIRE(names.empty());
    REQUIRE(emboss_face_list::sync_names(faces, names, name_of));
    REQUIRE(names.front() == "Arial Black");
}
