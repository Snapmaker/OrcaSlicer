#include <catch2/catch.hpp>

#include "slic3r/Utils/LibraryIndex.hpp"
#include "slic3r/Utils/MeshThumbnail.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <array>
#include <atomic>
#include <map>
#include <random>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Library;
namespace fs = boost::filesystem;

// The Home tab's Library (src/slic3r/GUI/HomePanel.cpp runs these off the GUI thread).

namespace {

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("edgeslicer-library-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream f(p.string().c_str(), std::ios::binary);
    f << data;
}

std::string slurp(const std::string& p)
{
    boost::nowide::ifstream f(p.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A 3MF with the given entries.
void write_zip(const fs::path& p, const std::map<std::string, std::string>& entries)
{
    fs::create_directories(p.parent_path());
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, p.string()));
    for (const auto& [name, data] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
}

const std::string PNG_A = std::string("\x89PNG\r\n\x1a\n", 8) + "cover-a";
const std::string PNG_B = std::string("\x89PNG\r\n\x1a\n", 8) + "plate-one";

const std::string RELS = R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
 <Relationship Target="/Auxiliaries/.thumbnails/thumbnail_3mf.png" Id="rel-2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail"/>
</Relationships>)";

const std::string MODEL_HEAD = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <metadata name="Application">BambuStudio-01.10</metadata>
 <metadata name="Title">Snorlax &amp; friends</metadata>
 <metadata name="Designer">Jane &#233;</metadata>
 <resources/>
</model>)";

const std::string SETTINGS = R"(<?xml version="1.0" encoding="UTF-8"?>
<config>
  <plate>
    <metadata key="plater_id" value="1"/>
    <metadata key="plater_name" value="Body"/>
  </plate>
  <plate>
    <metadata key="plater_id" value="3"/>
    <metadata key="plater_name" value="Ears &quot;L&quot;"/>
  </plate>
</config>)";

std::map<std::string, std::string> project_entries()
{
    return {{"_rels/.rels", RELS},
            {"3D/3dmodel.model", MODEL_HEAD},
            {"Metadata/model_settings.config", SETTINGS},
            {"Auxiliaries/.thumbnails/thumbnail_3mf.png", PNG_A},
            {"Metadata/plate_1.png", PNG_B},
            {"Metadata/plate_2.png", PNG_B},
            {"Metadata/plate_3.png", PNG_B}};
}

const Entry* find(const Index& index, const std::string& name)
{
    for (const Entry& e : index.entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

} // namespace

TEST_CASE("library: folder list round trip and cleanup", "[Library]")
{
    const auto folders = folders_from_json(R"([
        {"path": "/data/models/ ", "category": " Toys ", "vendor": "CPL3D"},
        {"path": "/data/models", "category": "dup"},
        {"path": "/nas/prints", "recursive": false},
        {"path": ""}, {"nopath": 1}, 5, "x"
    ])");
    REQUIRE(folders.size() == 2);
    CHECK(folders[0].path == "/data/models");
    CHECK(folders[0].category == "Toys");
    CHECK(folders[0].vendor == "CPL3D");
    CHECK(folders[0].recursive);
    CHECK(folders[1].path == "/nas/prints");
    CHECK_FALSE(folders[1].recursive);
    const auto again = folders_from_json(folders_to_json(folders));
    REQUIRE(again.size() == 2);
    CHECK(again[0].vendor == "CPL3D");
    CHECK(folders_from_json("not json").empty());
    CHECK(folders_from_json("{}").empty());
}

TEST_CASE("library: file types", "[Library]")
{
    CHECK(file_type("a.3mf") == "3mf");
    CHECK(file_type("A.3MF") == "3mf");
    CHECK(file_type("a.gcode.3mf") == "3mf");
    CHECK(file_type("a.stl") == "stl");
    CHECK(file_type("a.STEP") == "step");
    CHECK(file_type("a.stp") == "step");
    CHECK(file_type("a.obj") == "obj");
    CHECK(file_type("a.amf") == "amf");
    CHECK(file_type("a.gcode").empty());
    CHECK(file_type("a.zip").empty());
    CHECK(file_type("stl").empty());
    CHECK(file_type("").empty());
}

TEST_CASE("library: ids are stable and distinct", "[Library]")
{
    CHECK(entry_id("/a/b.3mf") == entry_id("/a/b.3mf"));
    CHECK(entry_id("/a/b.3mf") != entry_id("/a/c.3mf"));
    CHECK(entry_id("").size() == 16);
    CHECK(entry_id("") == "cbf29ce484222325"); // FNV-1a 64 offset basis
}

TEST_CASE("library: pieces of a 3MF read from text", "[Library]")
{
    CHECK(rels_thumbnail(RELS) == "Auxiliaries/.thumbnails/thumbnail_3mf.png");
    CHECK(rels_thumbnail("<Relationships/>").empty());
    // Attribute order and quoting do not matter.
    CHECK(rels_thumbnail("<Relationship Type='http://x/metadata/thumbnail' Target='/Metadata/thumbnail.png'/>") == "Metadata/thumbnail.png");

    CHECK(model_metadata(MODEL_HEAD, "Title") == "Snorlax & friends");
    CHECK(model_metadata(MODEL_HEAD, "Designer") == "Jane \xC3\xA9");
    CHECK(model_metadata(MODEL_HEAD, "License").empty());

    const auto names = plate_names(SETTINGS);
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Body");
    CHECK(names[1] == "");
    CHECK(names[2] == "Ears \"L\"");
    CHECK(plate_names("<config/>").empty());
    CHECK(plate_names(R"(<plate><metadata key="plater_id" value="999999"/></plate>)").empty());
}

TEST_CASE("library: reading a 3MF without loading it", "[Library]")
{
    TempDir tmp;
    SECTION("a slicer project")
    {
        const fs::path p = tmp.path / "snorlax.3mf";
        write_zip(p, project_entries());
        const ThreeMfInfo info = read_3mf(p.string());
        CHECK(info.ok);
        CHECK(info.plates == 3);
        CHECK(info.title == "Snorlax & friends");
        CHECK(info.designer == "Jane \xC3\xA9");
        CHECK(info.thumbnail_png == PNG_A); // the package thumbnail wins
        CHECK_FALSE(info.sliced);
        REQUIRE(info.plate_names.size() == 3);
        CHECK(info.plate_names[2] == "Ears \"L\"");
    }
    SECTION("no package thumbnail: plate 1")
    {
        const fs::path p = tmp.path / "plain.3mf";
        write_zip(p, {{"3D/3dmodel.model", "<model/>"}, {"Metadata/plate_1.png", PNG_B}, {"Metadata/plate_1.gcode", "G1"}});
        const ThreeMfInfo info = read_3mf(p.string());
        CHECK(info.thumbnail_png == PNG_B);
        CHECK(info.plates == 1);
        CHECK(info.sliced);
    }
    SECTION("another slicer's 3MF: Metadata/thumbnail.png")
    {
        const fs::path p = tmp.path / "prusa.3mf";
        write_zip(p, {{"3D/3dmodel.model", "<model/>"}, {"Metadata/thumbnail.png", PNG_A}});
        CHECK(read_3mf(p.string()).thumbnail_png == PNG_A);
    }
    SECTION("not a zip")
    {
        const fs::path p = tmp.path / "broken.3mf";
        write(p, "definitely not a zip");
        const ThreeMfInfo info = read_3mf(p.string());
        CHECK_FALSE(info.ok);
        CHECK(info.thumbnail_png.empty());
    }
}

TEST_CASE("library: scanning folders", "[Library]")
{
    TempDir tmp;
    const fs::path lib   = tmp.path / "lib";
    const fs::path cache = tmp.path / "cache";
    write_zip(lib / "toys" / "snorlax.3mf", project_entries());
    write(lib / "toys" / "gear.stl", "solid x\nendsolid x\n");
    write(lib / "parts" / "bracket.STEP", "ISO-10303-21;");
    write(lib / "readme.txt", "not a model");
    write(lib / ".git" / "hidden.stl", "solid");
    write(lib / "deep" / "a" / "b" / "c" / "far.obj", "o x");
    write(lib / "top.amf", "<amf/>");

    std::atomic<bool> cancel { false };
    std::vector<Folder> folders { {lib.generic_string(), true, "Models", "Mixed"} };

    const Index first = scan(folders, Index(), {}, cache.string(), 1000, cancel);
    CHECK(first.entries.size() == 5);
    CHECK(find(first, "hidden.stl") == nullptr);
    CHECK(find(first, "readme.txt") == nullptr);
    REQUIRE(first.folders.size() == 1);
    CHECK(first.folders[0].online);
    CHECK(first.folders[0].files == 5);

    const Entry* snorlax = find(first, "snorlax.3mf");
    REQUIRE(snorlax != nullptr);
    CHECK(snorlax->type == "3mf");
    CHECK(snorlax->plates == 3);
    CHECK(snorlax->rel_dir == "toys");
    CHECK(snorlax->category == "Models");
    CHECK(snorlax->vendor == "Mixed");
    CHECK(snorlax->added == 1000);
    CHECK(snorlax->has_thumbnail);
    CHECK(slurp(thumbnail_path(cache.string(), snorlax->id)) == PNG_A);
    const Entry* far = find(first, "far.obj");
    REQUIRE(far != nullptr);
    CHECK(far->rel_dir == "deep/a/b/c");
    CHECK(find(first, "top.amf")->rel_dir == "");
    CHECK(find(first, "bracket.STEP")->type == "step");
    // The page gets no full path.
    const nlohmann::json item = page_item(*snorlax);
    CHECK_FALSE(item.contains("path"));
    CHECK(item["rel_dir"] == "toys");
    CHECK(item["plates"] == 3);

    SECTION("an unchanged file is not read again; tags follow the folder")
    {
        // Break the cached thumbnail's source: an unchanged file keeps its entry without being opened.
        Index previous = first;
        for (Entry& e : previous.entries)
            if (e.name == "snorlax.3mf") e.title = "from the cache";
        folders[0].category = "Renamed";
        const Index second = scan(folders, previous, {}, cache.string(), 2000, cancel);
        CHECK(find(second, "snorlax.3mf")->title == "from the cache");
        CHECK(find(second, "snorlax.3mf")->added == 1000);
        CHECK(find(second, "snorlax.3mf")->category == "Renamed");
    }
    SECTION("a changed file is read again and keeps when it was added")
    {
        auto entries = project_entries();
        entries["3D/3dmodel.model"] = R"(<model><metadata name="Title">New title</metadata></model>)";
        write_zip(lib / "toys" / "snorlax.3mf", entries);
        fs::last_write_time(lib / "toys" / "snorlax.3mf", std::time(nullptr) + 100);
        const Index second = scan(folders, first, {}, cache.string(), 2000, cancel);
        CHECK(find(second, "snorlax.3mf")->title == "New title");
        CHECK(find(second, "snorlax.3mf")->added == 1000);
    }
    SECTION("hidden paths are left out and a removed file's thumbnail is deleted")
    {
        const std::string thumb = thumbnail_path(cache.string(), snorlax->id);
        const Index second = scan(folders, first, {snorlax->path}, cache.string(), 2000, cancel);
        CHECK(find(second, "snorlax.3mf") == nullptr);
        CHECK(second.entries.size() == 4);
        CHECK_FALSE(fs::exists(thumb));
    }
    SECTION("a deeper Library folder owns its files and its tags win")
    {
        folders.push_back({(lib / "toys").generic_string(), true, "Toys", "CPL3D"});
        const Index second = scan(folders, first, {}, cache.string(), 2000, cancel);
        CHECK(second.entries.size() == 5); // nothing listed twice
        CHECK(find(second, "snorlax.3mf")->vendor == "CPL3D");
        CHECK(find(second, "snorlax.3mf")->root == (lib / "toys").generic_string());
        CHECK(find(second, "snorlax.3mf")->rel_dir == "");
        CHECK(find(second, "bracket.STEP")->vendor == "Mixed");
    }
    SECTION("a non-recursive deeper folder only owns its own level")
    {
        folders.push_back({(lib / "deep").generic_string(), false, "Flat", ""});
        write(lib / "deep" / "shallow.stl", "solid");
        const Index second = scan(folders, first, {}, cache.string(), 2000, cancel);
        CHECK(find(second, "shallow.stl")->category == "Flat");
        CHECK(find(second, "far.obj")->category == "Models");
    }
    SECTION("a non-recursive folder lists only its own files")
    {
        folders[0].recursive = false;
        const Index second = scan(folders, first, {}, cache.string(), 2000, cancel);
        REQUIRE(second.entries.size() == 1);
        CHECK(second.entries[0].name == "top.amf");
    }
    SECTION("an unreachable folder keeps its last files and says it is offline")
    {
        fs::remove_all(lib);
        const Index second = scan(folders, first, {}, cache.string(), 2000, cancel);
        REQUIRE(second.folders.size() == 1);
        CHECK_FALSE(second.folders[0].online);
        CHECK(second.entries.size() == 5);
        CHECK(find(second, "snorlax.3mf")->has_thumbnail);
    }
    SECTION("depth and count limits")
    {
        ScanLimits shallow;
        shallow.max_depth = 2;
        CHECK(find(scan(folders, Index(), {}, cache.string(), 2000, cancel, {}, shallow), "far.obj") == nullptr);
        ScanLimits few;
        few.max_files = 2;
        CHECK(scan(folders, Index(), {}, cache.string(), 2000, cancel, {}, few).entries.size() == 2);
    }
    SECTION("cancelled")
    {
        cancel = true;
        CHECK(scan(folders, Index(), {}, cache.string(), 2000, cancel).entries.empty());
    }
    SECTION("the index survives a save and a load")
    {
        REQUIRE(save_index(cache.string(), first));
        const Index loaded = load_index(cache.string());
        CHECK(loaded.scanned_at == 1000);
        REQUIRE(loaded.entries.size() == first.entries.size());
        const Entry* e = find(loaded, "snorlax.3mf");
        REQUIRE(e != nullptr);
        CHECK(e->plate_names == snorlax->plate_names);
        CHECK(e->id == snorlax->id);
        REQUIRE(loaded.folders.size() == 1);
        CHECK(loaded.folders[0].files == 5);
    }
}

TEST_CASE("library: a missing or broken index is empty", "[Library]")
{
    TempDir tmp;
    CHECK(load_index(tmp.path.string()).entries.empty());
    write(tmp.path / "index.json", "{broken");
    CHECK(load_index(tmp.path.string()).entries.empty());
}

// ------------------------------------------------------------------------------ mesh pictures ----

namespace {

std::string binary_stl(const std::vector<std::array<float, 9>>& tris)
{
    std::string out(80, ' ');
    const uint32_t n = uint32_t(tris.size());
    out.append(reinterpret_cast<const char*>(&n), 4);
    for (const auto& t : tris) {
        const float normal[3] = {0, 0, 0};
        out.append(reinterpret_cast<const char*>(normal), 12);
        out.append(reinterpret_cast<const char*>(t.data()), 36);
        out.append(2, '\0');
    }
    return out;
}

const char* const CUBE_OBJ = "# a unit cube\n"
                             "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
                             "f 1 3 2\nf 1 4 3\nf 5 6 7 8\nf 1/1 2/2 6/3 5/4\nf 2//1 3//1 7//1 6//1\n"
                             "f -5 -1 -2 -6\nf 4 1 5 8\n";

} // namespace

TEST_CASE("library: meshes are read without a Model", "[Library]")
{
    Triangles t;
    SECTION("binary STL")
    {
        REQUIRE(parse_stl(binary_stl({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1}}), t));
        CHECK(t.size() == 18);
        CHECK(t[3] == 1.0f);
    }
    SECTION("a binary STL whose header starts with \"solid\" is still binary")
    {
        std::string b = binary_stl({{0, 0, 0, 1, 0, 0, 0, 1, 0}});
        b.replace(0, 5, "solid");
        REQUIRE(parse_stl(b, t));
        CHECK(t.size() == 9);
    }
    SECTION("ASCII STL, whatever the number format")
    {
        const std::string a = "solid part\n facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1.5e1 0 0\n"
                              "   vertex 0 -2.25 +3\n  endloop\n endfacet\nendsolid part\n";
        REQUIRE(parse_stl(a, t));
        REQUIRE(t.size() == 9);
        CHECK(t[3] == 15.0f);
        CHECK(t[7] == -2.25f);
        CHECK(t[8] == 3.0f);
    }
    SECTION("not a mesh")
    {
        CHECK_FALSE(parse_stl("", t));
        CHECK_FALSE(parse_stl("solid x\nendsolid x\n", t));
        CHECK_FALSE(parse_stl(std::string(90, 'z'), t));
    }
    SECTION("OBJ: polygons, texture and normal indices, negative indices")
    {
        REQUIRE(parse_obj(CUBE_OBJ, t));
        CHECK(t.size() / 9 == 12);
    }
    SECTION("OBJ: a face naming a vertex that is not there is left out")
    {
        REQUIRE(parse_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 1 2 9\nf 0 1 2\n", t));
        CHECK(t.size() == 9);
        CHECK_FALSE(parse_obj("o empty\n", t));
    }
    SECTION("AMF: vertices numbered per object")
    {
        const std::string amf = R"(<?xml version="1.0"?><amf unit="millimeter">
            <object id="0"><mesh><vertices>
              <vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex>
              <vertex><coordinates><x>1</x><y>0</y><z>0</z></coordinates></vertex>
              <vertex><coordinates><x>0</x><y>1</y><z>0</z></coordinates></vertex>
            </vertices><volume><triangle><v1>0</v1><v2>1</v2><v3>2</v3></triangle></volume></mesh></object>
            <object id="1"><mesh><vertices>
              <vertex><coordinates><x>5</x><y>5</y><z>5</z></coordinates></vertex>
              <vertex><coordinates><x>6</x><y>5</y><z>5</z></coordinates></vertex>
              <vertex><coordinates><x>5</x><y>6</y><z>5</z></coordinates></vertex>
            </vertices><volume><triangle><v1>0</v1><v2>2</v2><v3>1</v3></triangle>
              <triangle><v1>0</v1><v2>1</v2><v3>7</v3></triangle></volume></mesh></object></amf>)";
        REQUIRE(parse_amf(amf, t));
        REQUIRE(t.size() == 18);
        CHECK(t[9] == 5.0f); // the second object's first vertex, not the first object's
        CHECK(t[12] == 5.0f);
        CHECK(t[13] == 6.0f);
    }
    SECTION("limits")
    {
        MeshLimits one;
        one.max_triangles = 1;
        REQUIRE(parse_obj(CUBE_OBJ, t, one));
        CHECK(t.size() == 9); // stops once the limit is reached
        CHECK_FALSE(parse_stl(binary_stl({{0, 0, 0, 1, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1}}), t, one));
    }
}

TEST_CASE("library: a mesh is drawn on a transparent background", "[Library]")
{
    Triangles t;
    REQUIRE(parse_obj(CUBE_OBJ, t));
    const int size = 64;
    const std::vector<unsigned char> px = render_rgba(t, size);
    REQUIRE(px.size() == size_t(size) * size * 4);
    auto alpha = [&](int x, int y) { return px[(size_t(y) * size + x) * 4 + 3]; };
    CHECK(alpha(0, 0) == 0);                // corners are empty
    CHECK(alpha(size - 1, size - 1) == 0);
    CHECK(alpha(size / 2, size / 2) == 255); // the middle is the cube
    // Three faces are seen, lit differently.
    std::set<int> greys;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (alpha(x, y) == 255)
                greys.insert(px[(size_t(y) * size + x) * 4]);
    CHECK(greys.size() >= 3);

    const std::string png = encode_png(px, size);
    REQUIRE(png.size() > 8);
    CHECK(png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
    CHECK(render_rgba({}, size).empty());
    CHECK(encode_png({}, size).empty());
    // Everything in one point: nothing to draw.
    CHECK(render_rgba(Triangles(9, 1.0f), size).empty());
}

TEST_CASE("library: the scan draws covers for meshes", "[Library]")
{
    TempDir tmp;
    const fs::path lib = tmp.path / "lib", cache = tmp.path / "cache";
    write(lib / "cube.obj", CUBE_OBJ);
    write(lib / "tri.stl", binary_stl({{0, 0, 0, 10, 0, 0, 0, 10, 5}}));
    write(lib / "broken.stl", "solid x\nendsolid x\n");
    write(lib / "part.step", "ISO-10303-21;");
    Folder f;
    f.path = lib.string();
    std::atomic<bool> cancel { false };
    const Index index = scan({f}, Index(), {}, cache.string(), 1000, cancel);
    auto entry = [&](const std::string& name) -> const Entry* {
        for (const Entry& e : index.entries)
            if (e.name == name) return &e;
        return nullptr;
    };
    REQUIRE(entry("cube.obj"));
    CHECK(entry("cube.obj")->has_thumbnail);
    CHECK(fs::is_regular_file(thumbnail_path(cache.string(), entry("cube.obj")->id)));
    CHECK(entry("tri.stl")->has_thumbnail);
    CHECK_FALSE(entry("broken.stl")->has_thumbnail);
    CHECK_FALSE(entry("part.step")->has_thumbnail); // not drawn yet
    CHECK(mesh_thumbnail_png((lib / "cube.obj").string(), "step").empty());
}
