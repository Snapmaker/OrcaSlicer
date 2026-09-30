#include <catch2/catch.hpp>

#include "slic3r/Utils/LibraryIndex.hpp"
#include "slic3r/Utils/MeshThumbnail.hpp"
#include "slic3r/Utils/StepMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <STEPControl_Writer.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <atomic>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::Library;
namespace fs = boost::filesystem;

// STEP previews for the Home tab's Library (Utils/StepMesh). The parts are made with OpenCASCADE
// and written as STEP, then read back the way the scan reads a user's file.

namespace {

struct TempDir
{
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("edgeslicer-step-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A 40 x 20 x 10 block with a 6 mm hole through it.
bool write_block_step(const fs::path& p)
{
    const TopoDS_Shape block = BRepPrimAPI_MakeBox(40., 20., 10.).Shape();
    const TopoDS_Shape hole  = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20., 10., -1.), gp::DZ()), 3., 12.).Shape();
    BRepAlgoAPI_Cut    cut(block, hole);
    if (!cut.IsDone())
        return false;
    STEPControl_Writer writer;
    if (writer.Transfer(cut.Shape(), STEPControl_AsIs) != IFSelect_RetDone)
        return false;
    return writer.Write(p.string().c_str()) == IFSelect_RetDone;
}

struct Box3
{
    float lo[3] { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    float hi[3] { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
};
Box3 bounds(const Triangles& t)
{
    Box3 b;
    for (size_t i = 0; i < t.size(); ++i) {
        b.lo[i % 3] = std::min(b.lo[i % 3], t[i]);
        b.hi[i % 3] = std::max(b.hi[i % 3], t[i]);
    }
    return b;
}

} // namespace

TEST_CASE("library: STEP files are tessellated for previews", "[Library]")
{
    TempDir        tmp;
    const fs::path step = tmp.path / "block.step";
    REQUIRE(write_block_step(step));

    Triangles tris;
    REQUIRE(read_step(step.string(), tris));
    CHECK(tris.size() % 9 == 0);
    CHECK(tris.size() / 9 > 12); // the hole's wall is curved
    const Box3 b = bounds(tris);
    CHECK_THAT(b.hi[0] - b.lo[0], Catch::Matchers::WithinAbs(40., 0.01));
    CHECK_THAT(b.hi[1] - b.lo[1], Catch::Matchers::WithinAbs(20., 0.01));
    CHECK_THAT(b.hi[2] - b.lo[2], Catch::Matchers::WithinAbs(10., 0.01));

    // Faces keep their outward winding: the top face's triangles face up.
    size_t up = 0, down = 0;
    for (size_t i = 0; i + 8 < tris.size(); i += 9) {
        if (tris[i + 2] < 9.99f || tris[i + 5] < 9.99f || tris[i + 8] < 9.99f)
            continue;
        const float ux = tris[i + 3] - tris[i], uy = tris[i + 4] - tris[i + 1];
        const float vx = tris[i + 6] - tris[i], vy = tris[i + 7] - tris[i + 1];
        (ux * vy - uy * vx > 0 ? up : down)++;
    }
    CHECK(up > 0);
    CHECK(down == 0);

    const std::string png = mesh_thumbnail_png(step.string(), "step");
    REQUIRE(png.size() > 8);
    CHECK(png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
}

TEST_CASE("library: STEP previews fail cleanly", "[Library]")
{
    TempDir        tmp;
    const fs::path step = tmp.path / "block.stp";
    REQUIRE(write_block_step(step));
    Triangles tris;

    SECTION("cancelled")
    {
        std::atomic<bool> cancel { true };
        CHECK_FALSE(read_step(step.string(), tris, step_limits(), &cancel));
        CHECK(tris.empty());
        CHECK(mesh_thumbnail_png(step.string(), "step", 256, &cancel).empty());
    }
    SECTION("too big")
    {
        MeshLimits small;
        small.max_bytes = 100;
        CHECK_FALSE(read_step(step.string(), tris, small));
    }
    SECTION("too many triangles")
    {
        MeshLimits few;
        few.max_triangles = 4;
        CHECK_FALSE(read_step(step.string(), tris, few));
        CHECK(tris.empty());
    }
    SECTION("not STEP")
    {
        const fs::path junk = tmp.path / "junk.step";
        boost::nowide::ofstream(junk.string().c_str()) << "ISO-10303-21;\nHEADER;\nnonsense\n";
        CHECK_FALSE(read_step(junk.string(), tris));
        CHECK_FALSE(read_step((tmp.path / "missing.step").string(), tris));
    }
}

TEST_CASE("library: the scan draws covers for STEP files", "[Library]")
{
    TempDir        tmp;
    const fs::path lib = tmp.path / "lib", cache = tmp.path / "cache";
    fs::create_directories(lib);
    REQUIRE(write_block_step(lib / "block.step"));
    Folder f;
    f.path = lib.string();
    std::atomic<bool> cancel { false };
    const Index index = scan({f}, Index(), {}, cache.string(), 1000, cancel);
    REQUIRE(index.entries.size() == 1);
    CHECK(index.entries[0].type == "step");
    CHECK(index.entries[0].has_thumbnail);
    CHECK(fs::is_regular_file(thumbnail_path(cache.string(), index.entries[0].id)));
}
