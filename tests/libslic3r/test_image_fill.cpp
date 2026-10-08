// Image Fill (Phase 2) - the four things the acceptance bar names: projection maths, asset hash
// stability, subdivision producing a watertight mesh with no T-joints, and the 3MF round trip.
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md
#include <catch2/catch.hpp>

#include "libslic3r/ImageFill.hpp"
#include "libslic3r/ColorSplit.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/ObjColorMatch.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;

namespace {

std::vector<uint8_t> read_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The four-filament palette the projection tests use. Deliberately saturated and far apart, so
// "which filament did the solver pick" has one obvious right answer and the test is about the
// projection, not about the solver's tie-breaking.
const std::vector<std::array<float, 3>> kFilamentColors = {
    {1.f, 0.f, 0.f},   // 1 red
    {0.f, 1.f, 0.f},   // 2 green
    {0.f, 0.f, 1.f},   // 3 blue
    {1.f, 1.f, 1.f},   // 4 white
};
const std::vector<int> kFilamentIds = {1, 2, 3, 4};

// The state a given leaf of a given original facet came out with, read back through the fork's
// own decoder rather than out of my encoder - so the test measures what the slicer will see.
std::vector<int> leaf_states(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data,
                             size_t n_facets, int depth)
{
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    std::vector<int> out(n_facets * per, 0);
    TriangleSelector sel(mesh);
    sel.deserialize(data, /*needs_reset=*/true);
    // get_facets() reports leaves per state; match them back by their vertex coordinates, which
    // is exact because both sides compute the same midpoints from the same floats.
    std::map<std::array<float, 9>, size_t> index;
    for (size_t f = 0; f < n_facets; ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.its.indices[f];
        image_fill_subdivide(mesh.its.vertices[t(0)], mesh.its.vertices[t(1)], mesh.its.vertices[t(2)],
                             depth, leaves);
        for (size_t i = 0; i < leaves.size(); ++i)
            index.emplace(std::array<float, 9>{leaves[i].a.x(), leaves[i].a.y(), leaves[i].a.z(),
                                               leaves[i].b.x(), leaves[i].b.y(), leaves[i].b.z(),
                                               leaves[i].c.x(), leaves[i].c.y(), leaves[i].c.z()},
                          f * per + i);
    }
    // Up to OBJ_COLOR_MAX_SLOTS: the headless colour matcher can allocate 16 slots, so a scan
    // that stopped at 8 would silently report the leaves above it as unpainted.
    for (int state = 1; state <= 16; ++state) {
        const indexed_triangle_set its = sel.get_facets(EnforcerBlockerType(state));
        for (const Vec3i32 &t : its.indices) {
            const Vec3f &a = its.vertices[t(0)], &b = its.vertices[t(1)], &c = its.vertices[t(2)];
            auto it = index.find(std::array<float, 9>{a.x(), a.y(), a.z(), b.x(), b.y(), b.z(),
                                                      c.x(), c.y(), c.z()});
            if (it != index.end())
                out[it->second] = state;
        }
    }
    return out;
}

// A single axis-aligned quad in the XY plane at z = 0, as two triangles wound CCW. Enough for a
// projection test and small enough that every leaf can be named.
indexed_triangle_set quad_xy(float sx, float sy)
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(0, 0, 0), Vec3f(sx, 0, 0), Vec3f(sx, sy, 0), Vec3f(0, sy, 0)};
    its.indices  = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    return its;
}

} // namespace

// =============================================================================================
// 1. Asset hash stability
// =============================================================================================

TEST_CASE("Image Fill: the asset hash is stable and the store is content addressed", "[imagefill]")
{
    const std::vector<uint8_t> quad = read_fixture("quad_rgbw.png");

    SECTION("the hash of a fixture is the value the fixture generator printed")
    {
        // Pinned against tests/data/image_fill/make_fixtures.py's own output. If this moves, one
        // of the two changed and the round trip is no longer reproducible.
        CHECK(image_fill_sha256_hex(quad) ==
              "3d27b4ed2fdfdb12b533f2ddf6e113f5f6ad516b1acd9ebb3ed1de5476ec51c6");
        CHECK(image_fill_sha256_hex(read_fixture("stripes3.png")) ==
              "2d43f8c92b2d9da66a1bcac7dc453e87386edce930a992f616dc9704cbf19f2c");
        CHECK(image_fill_sha256_hex(read_fixture("ramp_kw.png")) ==
              "e621c916dce53449738f6d26208410c1e0000d26aaefc1e2947e462e57aa6055");
        CHECK(image_fill_sha256_hex(read_fixture("bands3.png")) ==
              "4838fcf29581e309d063ffa0121b7c805ba54bb1112d092ac2e4a0bfc3ade9fc");
        CHECK(image_fill_sha256_hex(read_fixture("wrap4.png")) ==
              "12654c63fe80a00e183f4b0f710f3c0a774b9cf142cf8bd968a4054ad45addeb");
    }

    SECTION("the empty hash is the well-known SHA-256 of nothing")
    {
        CHECK(image_fill_sha256_hex(nullptr, 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    SECTION("storing the same bytes twice makes one entry with one hash")
    {
        ImageAssetStore store;
        const std::string a = store.add(quad);
        const std::string b = store.add(quad);
        CHECK(a == b);
        CHECK(store.size() == 1);
        REQUIRE(store.find(a) != nullptr);
        CHECK(store.find(a)->bytes == quad);
    }

    SECTION("different bytes make different entries, and retain() drops the unreferenced one")
    {
        ImageAssetStore store;
        const std::string a = store.add(quad);
        const std::string b = store.add(read_fixture("stripes3.png"));
        REQUIRE(a != b);
        CHECK(store.size() == 2);
        store.retain({a});
        CHECK(store.size() == 1);
        CHECK(store.contains(a));
        CHECK_FALSE(store.contains(b));
    }

    SECTION("the fixture decodes to the pixels it was written with")
    {
        ImageAssetStore   store;
        const std::string sha = store.add(quad);
        const ImageAsset *px  = store.pixels(sha);
        REQUIRE(px != nullptr);
        CHECK(px->width == 2);
        CHECK(px->height == 2);
        REQUIRE(px->rgb.size() == 12);
        // Row 0 is the top row: red, green. Row 1: blue, white.
        CHECK(int(px->rgb[0]) == 255); CHECK(int(px->rgb[1]) == 0);   CHECK(int(px->rgb[2]) == 0);
        CHECK(int(px->rgb[3]) == 0);   CHECK(int(px->rgb[4]) == 255); CHECK(int(px->rgb[5]) == 0);
        CHECK(int(px->rgb[6]) == 0);   CHECK(int(px->rgb[7]) == 0);   CHECK(int(px->rgb[8]) == 255);
        CHECK(int(px->rgb[9]) == 255); CHECK(int(px->rgb[10]) == 255); CHECK(int(px->rgb[11]) == 255);
    }
}

// Regression (2026-10-07): libslic3r_tests crashed once with SIGSEGV in [ImageRow]'s
// multi-volume transform case. ImageAssetStore::pixels() decodes lazily and used to set
// `decoded = true` BEFORE it had filled width/height/rgb, so a second TBB fill thread asking for
// the same asset in that window got back a half-built ImageAsset - width/height set, rgb still
// empty or being resized - and image_fill_sample_pixel() read past the buffer. A Print owns a
// fresh copy of the model's store, so the first decode always happens inside make_fills()'s
// parallel loop, where two layers' top surfaces (or two regions' surfaces) can race for it.
// This hammers one fresh store from many threads at once, many times, and checks every caller
// got a complete image.
TEST_CASE("Image Fill: concurrent first decode of one asset hands every thread a complete image", "[imagefill]")
{
    // Big enough that the decode takes real time, so the race window is wide.
    const size_t w = 512, h = 512;
    std::vector<uint8_t> rgb(w * h * 3);
    for (size_t i = 0; i < rgb.size(); ++i)
        rgb[i] = uint8_t((i * 2654435761u) >> 24);
    const boost::filesystem::path tmp = boost::filesystem::temp_directory_path() /
                                        boost::filesystem::unique_path("image_fill_race_%%%%-%%%%-%%%%.png");
    REQUIRE(png::write_rgb_to_file(tmp.string(), w, h, rgb));
    std::vector<uint8_t> bytes;
    {
        boost::nowide::ifstream in(tmp.string(), std::ios::binary);
        REQUIRE(in.good());
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp, ec);
    REQUIRE(!bytes.empty());

    const unsigned n_threads = std::max(8u, std::thread::hardware_concurrency());
    std::atomic<int> incomplete{0};
    for (int round = 0; round < 40; ++round) {
        ImageAssetStore   store;
        const std::string sha = store.add(bytes);
        std::atomic<unsigned> ready{0};
        std::atomic<bool>     go{false};
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < n_threads; ++t)
            threads.emplace_back([&]() {
                ++ready;
                while (!go.load()) {}
                const ImageAsset *px = store.pixels(sha);
                // Checked before sampling, so the unfixed code fails this test instead of crashing it.
                if (px == nullptr || px->width != w || px->height != h || px->rgb.size() != w * h * 3) {
                    ++incomplete;
                    return;
                }
                const std::array<float, 3> c = image_fill_sample_pixel(*px, 0.999f, 0.001f);
                if (c[0] != px->rgb[(w * h - 1) * 3] / 255.f)
                    ++incomplete;
            });
        while (ready.load() < n_threads) {}
        go = true;
        for (std::thread &th : threads)
            th.join();
    }
    CHECK(incomplete.load() == 0);
}

// =============================================================================================
// 2. Projection maths - known points map to known pixels
// =============================================================================================

TEST_CASE("Image Fill: each projection maps known points to known image coordinates", "[imagefill]")
{
    BoundingBoxf3 box(Vec3d(0, 0, 0), Vec3d(10, 20, 40));

    SECTION("planar along Z takes u from x and v from y")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, box, Vec3f(0, 0, 7), u, v));
        CHECK(u == Approx(0.f)); CHECK(v == Approx(0.f));
        REQUIRE(image_fill_project(p, box, Vec3f(10, 20, 7), u, v));
        CHECK(u == Approx(1.f)); CHECK(v == Approx(1.f));
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 15, 7), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.75f));
    }

    SECTION("planar along X takes u from y and v from z, and along Y u from x and v from z")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        float u = 0, v = 0;
        p.axis = ImageFillAxis::X;
        REQUIRE(image_fill_project(p, box, Vec3f(3, 5, 10), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.25f));
        p.axis = ImageFillAxis::Y;
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 3, 30), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.75f));
    }

    SECTION("the flips are exactly one minus the unflipped answer")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;
        p.flip_u = true;
        p.flip_v = true;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, box, Vec3f(2.5f, 15, 7), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.25f));
    }

    SECTION("cylindrical about Z: u is the turn angle from +x, v runs up the axis")
    {
        BoundingBoxf3  cyl(Vec3d(-5, -5, 0), Vec3d(5, 5, 20));
        ImageFillParams p;
        p.projection = ImageFillProjection::Cylindrical;
        p.axis       = ImageFillAxis::Z;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, cyl, Vec3f(5, 0, 5), u, v));
        CHECK(u == Approx(0.5f));  CHECK(v == Approx(0.25f));   // +x is the middle of the image
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, 5, 10), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.5f));    // +y, a quarter turn on
        REQUIRE(image_fill_project(p, cyl, Vec3f(-5, 0, 20), u, v));
        CHECK(u == Approx(0.f));   CHECK(v == Approx(1.f));     // -x is the seam
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, -5, 0), u, v));
        CHECK(u == Approx(0.25f)); CHECK(v == Approx(0.f));     // -y
        // Exactly on the axis there is no angle, and the projection says so rather than guessing.
        CHECK_FALSE(image_fill_project(p, cyl, Vec3f(0, 0, 10), u, v));
    }

    SECTION("cylindrical about X wraps in the y/z plane")
    {
        BoundingBoxf3   cyl(Vec3d(0, -5, -5), Vec3d(30, 5, 5));
        ImageFillParams p;
        p.projection = ImageFillProjection::Cylindrical;
        p.axis       = ImageFillAxis::X;
        float u = 0, v = 0;
        REQUIRE(image_fill_project(p, cyl, Vec3f(15, 5, 0), u, v));
        CHECK(u == Approx(0.5f));  CHECK(v == Approx(0.5f));
        REQUIRE(image_fill_project(p, cyl, Vec3f(0, 0, 5), u, v));
        CHECK(u == Approx(0.75f)); CHECK(v == Approx(0.f));
    }

    SECTION("MeshUV is not computed here; the caller supplies it")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::MeshUV;
        float u = 0, v = 0;
        CHECK_FALSE(image_fill_project(p, box, Vec3f(1, 1, 1), u, v));
    }
}

TEST_CASE("Image Fill: a four-quadrant image lands on the four quadrants of the part", "[imagefill]")
{
    // The end-to-end statement of the projection: a 2x2 picture on a 4x4 mm quad, subdivided
    // twice, must put red in the quadrant the image's top-left pixel points at and so on. This is
    // the "known points map to known pixels" bar with the pixels actually read.
    ImageAssetStore store;
    const std::string sha = store.add(read_fixture("quad_rgbw.png"));

    const indexed_triangle_set mesh = quad_xy(4.f, 4.f);
    TriangleSelector::TriangleSplittingData empty;

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = 3;
    p.allowed     = kFilamentIds;

    ImageFillResult res = image_fill_compute(mesh, empty, p, store, kFilamentColors, kFilamentIds);
    REQUIRE(res.ok);
    CHECK(res.subdivision_used == 3);
    CHECK(res.leaves_total == 2u * 64u);
    CHECK(res.facets_painted == res.leaves_total);

    TriangleMesh     tm(mesh);
    const std::vector<int> states = leaf_states(tm, res.painting, mesh.indices.size(), 3);

    // Walk the leaves and assert on the quadrant each centroid lands in. v = y/4, and the image's
    // row 0 (red, green) is at v = 1, i.e. the HIGH-y half of the part.
    size_t checked = 0;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.indices[f];
        image_fill_subdivide(mesh.vertices[t(0)], mesh.vertices[t(1)], mesh.vertices[t(2)], 3, leaves);
        for (size_t i = 0; i < leaves.size(); ++i) {
            const Vec3f c = leaves[i].centroid();
            // Skip leaves whose centroid sits on the halfway line; the nearest-neighbour sampler
            // is allowed either pixel there and the test is not about that.
            if (std::abs(c.x() - 2.f) < 1e-3f || std::abs(c.y() - 2.f) < 1e-3f)
                continue;
            const int expect = (c.y() > 2.f) ? ((c.x() < 2.f) ? 1 : 2)    // top row: red, green
                                             : ((c.x() < 2.f) ? 3 : 4);   // bottom row: blue, white
            INFO("leaf centroid " << c.x() << "," << c.y());
            CHECK(states[f * 64 + i] == expect);
            ++checked;
        }
    }
    CHECK(checked > 100);
    CHECK(res.filaments_used == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("Image Fill: mesh UVs put the picture where the exporter drew it", "[imagefill]")
{
    // The glTF projection: the same quad, but the image is placed by TEXCOORD_0 rather than by
    // position - and with v running DOWN from the top-left, which is glTF's convention. The UVs
    // here rotate the picture a quarter turn against the planar case, so a projection that
    // quietly ignored them could not pass both this and the test above.
    ImageAssetStore   store;
    const std::string sha = store.add(read_fixture("quad_rgbw.png"));

    indexed_triangle_set mesh = quad_xy(4.f, 4.f);
    // (x, y) -> (u, v) = (y/4, x/4): u from y, v from x, and glTF's v grows downwards. Given
    // per FACE, three corners each, in the triangle's own vertex order - the corners of
    // quad_xy() are (0,0), (4,0), (4,4), (0,4) and the faces are (0,1,2) and (0,2,3).
    const Vec2f uv0(0.f, 0.f), uv1(0.f, 1.f), uv2(1.f, 1.f), uv3(1.f, 0.f);
    ImageFillUVs uvs = {{uv0, uv1, uv2}, {uv0, uv2, uv3}};

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::MeshUV;
    p.subdivision = 3;
    p.allowed     = kFilamentIds;

    TriangleSelector::TriangleSplittingData empty;
    ImageFillResult res = image_fill_compute(mesh, empty, p, store, kFilamentColors, kFilamentIds, uvs);
    REQUIRE(res.ok);

    TriangleMesh           tm(mesh);
    const std::vector<int> states = leaf_states(tm, res.painting, mesh.indices.size(), 3);
    size_t checked = 0;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        std::vector<ImageFillLeaf> leaves;
        const Vec3i32 &t = mesh.indices[f];
        image_fill_subdivide(mesh.vertices[t(0)], mesh.vertices[t(1)], mesh.vertices[t(2)], 3, leaves);
        for (size_t i = 0; i < leaves.size(); ++i) {
            const Vec3f c = leaves[i].centroid();
            if (std::abs(c.x() - 2.f) < 1e-3f || std::abs(c.y() - 2.f) < 1e-3f)
                continue;
            // u = y/4, glTF v = x/4 measured downwards, so the image's top row (red, green) is at
            // LOW x, and its left column (red, blue) is at LOW y.
            const int expect = (c.x() < 2.f) ? ((c.y() < 2.f) ? 1 : 2)
                                             : ((c.y() < 2.f) ? 3 : 4);
            INFO("leaf centroid " << c.x() << "," << c.y());
            CHECK(states[f * 64 + i] == expect);
            ++checked;
        }
    }
    CHECK(checked > 100);
}

// =============================================================================================
// 3. Subdivision: watertight, no T-joints
// =============================================================================================

TEST_CASE("Image Fill: subdivision is a conforming, T-joint-free refinement", "[imagefill]")
{
    SECTION("one triangle at depth n has 4^n leaves whose areas sum to the original")
    {
        const Vec3f a(0, 0, 0), b(6, 0, 0), c(0, 9, 0);
        const float area0 = 0.5f * (b - a).cross(c - a).norm();
        for (int depth = 0; depth <= 4; ++depth) {
            std::vector<ImageFillLeaf> leaves;
            image_fill_subdivide(a, b, c, depth, leaves);
            size_t expect = 1;
            for (int i = 0; i < depth; ++i) expect *= 4;
            REQUIRE(leaves.size() == expect);
            float sum = 0.f;
            for (const ImageFillLeaf &l : leaves)
                sum += 0.5f * (l.b - l.a).cross(l.c - l.a).norm();
            INFO("depth " << depth);
            CHECK(sum == Approx(area0).epsilon(1e-4));
        }
    }

    SECTION("a painted cube stays watertight with zero open edges after retriangulation")
    {
        // ColorSplit::extract_color_patches is the fork's own T-joint-free retriangulation: it
        // concatenates get_facets_strict() over every used state and then refuses a surface with
        // any open edge. Running the painted cube through it is the sharpest available statement
        // that the subdivision left no T-joint and no crack.
        ImageAssetStore   store;
        const std::string sha = store.add(read_fixture("quad_rgbw.png"));

        TriangleMesh cube = make_cube(10., 10., 10.);
        cube.its.indices.size();
        ImageFillParams p;
        p.asset       = sha;
        p.projection  = ImageFillProjection::Planar;
        p.axis        = ImageFillAxis::Z;
        // Every facet, on purpose: this test is about the subdivision leaving no crack, so it
        // wants the whole surface painted. The DEFAULT is now ImageFillFaces::Facing, which would
        // paint the top face only and leave extract_color_patches nothing to close.
        p.faces       = ImageFillFaces::All;
        p.allowed     = kFilamentIds;

        for (int depth : {0, 1, 2, 3}) {
            p.subdivision = depth;
            TriangleSelector::TriangleSplittingData empty;
            ImageFillResult res = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
            REQUIRE(res.ok);
            INFO("depth " << depth);
            ColorPatches patches = extract_color_patches(cube.its, res.painting);
            CHECK(its_num_open_edges(patches.surface) == 0);
            // Every leaf accounted for: the retriangulation must not lose or invent a facet's
            // worth of area either.
            double area = 0.;
            for (const Vec3i32 &t : patches.surface.indices) {
                const Vec3f &a = patches.surface.vertices[t(0)];
                const Vec3f &b = patches.surface.vertices[t(1)];
                const Vec3f &c = patches.surface.vertices[t(2)];
                area += 0.5 * double((b - a).cross(c - a).norm());
            }
            CHECK(area == Approx(600.).epsilon(1e-4));   // a 10 mm cube
        }
    }

    SECTION("the encoder round-trips through the fork's own decoder")
    {
        // image_fill_encode writes TriangleSelector's bitstream by hand. The proof that it writes
        // the right thing is that deserialize() reads it and serialize() gives the same bytes
        // back - i.e. the encoding is the canonical one, not merely one the decoder tolerates.
        TriangleMesh cube = make_cube(10., 10., 10.);
        const size_t n = cube.its.indices.size();
        const int    depth = 2;
        std::vector<int> states(n * 16);
        for (size_t i = 0; i < states.size(); ++i)
            states[i] = int(i % 5);      // 0..4, so both the 2-bit and the nibble-chain forms appear
        const auto encoded = image_fill_encode(n, depth, states);

        TriangleSelector sel(cube);
        sel.deserialize(encoded, /*needs_reset=*/true);
        const auto reserialized = sel.serialize();
        CHECK(reserialized.triangles_to_split == encoded.triangles_to_split);
        CHECK(reserialized.bitstream == encoded.bitstream);
    }

    SECTION("a high state uses the nibble chain and still round-trips")
    {
        TriangleMesh cube = make_cube(4., 4., 4.);
        const size_t n = cube.its.indices.size();
        std::vector<int> states(n, 0);
        states[0] = 20;      // > 3 + 15, so two chunks
        states[1] = 3;       // exactly the boundary into the chain form
        states[2] = 2;       // the plain 2-bit form
        const auto encoded = image_fill_encode(n, 0, states);
        TriangleSelector sel(cube);
        sel.deserialize(encoded, /*needs_reset=*/true);
        CHECK(sel.serialize().bitstream == encoded.bitstream);
        CHECK(sel.num_facets(EnforcerBlockerType(20)) == 1);
        CHECK(sel.num_facets(EnforcerBlockerType(3)) == 1);
        CHECK(sel.num_facets(EnforcerBlockerType(2)) == 1);
    }

    SECTION("the depth is capped so a dense mesh cannot explode")
    {
        // 6 is the ceiling; a target so fine it would need more is clamped there...
        CHECK(image_fill_depth_for_detail(100.f, 0.001f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 6);
        // ...and the leaf budget wins over the level when the mesh is already large.
        CHECK(image_fill_depth_for_detail(100.f, 0.001f, IMAGE_FILL_MAX_SUBDIVISION, 2000000) < 6);
        // A part already finer than the target is not subdivided at all.
        CHECK(image_fill_depth_for_detail(0.2f, 1.0f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 0);
        // Two millimetres of triangle against a half-millimetre target is two halvings.
        CHECK(image_fill_depth_for_detail(2.f, 0.5f, IMAGE_FILL_MAX_SUBDIVISION, 12) == 2);
    }
}

// =============================================================================================
// 4. The solver and the palette
// =============================================================================================

TEST_CASE("Image Fill: the palette is deterministic and the solver picks the nearest filament", "[imagefill]")
{
    SECTION("quantising the same samples twice gives the same palette in the same order")
    {
        std::vector<std::array<float, 3>> samples;
        for (int i = 0; i < 500; ++i) {
            const float t = float(i) / 499.f;
            samples.push_back({t, 1.f - t, 0.5f});
        }
        const ImageFillPalette a = image_fill_quantise(samples, 16);
        const ImageFillPalette b = image_fill_quantise(samples, 16);
        REQUIRE(a.colors.size() == b.colors.size());
        CHECK(a.colors.size() <= 16);
        for (size_t i = 0; i < a.colors.size(); ++i) {
            CHECK(a.colors[i][0] == b.colors[i][0]);
            CHECK(a.colors[i][1] == b.colors[i][1]);
            CHECK(a.colors[i][2] == b.colors[i][2]);
            CHECK(a.counts[i] == b.counts[i]);
        }
        // Nothing is thrown away: the counts still add up to the number of samples, even for the
        // buckets that were merged into a kept representative.
        size_t total = 0;
        for (size_t c : a.counts) total += c;
        CHECK(total == samples.size());
    }

    SECTION("the solver returns the obvious filament for an obvious colour")
    {
        ImageFillPalette pal;
        pal.colors = {{0.95f, 0.05f, 0.05f}, {0.05f, 0.9f, 0.1f}, {0.1f, 0.1f, 0.95f},
                      {0.98f, 0.98f, 0.98f}};
        image_fill_solve(pal, kFilamentColors, kFilamentIds);
        REQUIRE(pal.filament.size() == 4);
        CHECK(pal.filament[0] == 1);
        CHECK(pal.filament[1] == 2);
        CHECK(pal.filament[2] == 3);
        CHECK(pal.filament[3] == 4);
    }

    SECTION("only the allowed filaments are ever chosen")
    {
        ImageFillPalette pal;
        pal.colors = {{0.95f, 0.05f, 0.05f}, {0.05f, 0.9f, 0.1f}};
        const std::vector<std::array<float, 3>> two = {{0.f, 0.f, 1.f}, {1.f, 1.f, 1.f}};
        const std::vector<int>                  ids = {3, 4};
        image_fill_solve(pal, two, ids);
        for (int f : pal.filament)
            CHECK((f == 3 || f == 4));
    }

    SECTION("per-face colours become painting, and the same input twice gives the same painting")
    {
        TriangleMesh cube = make_cube(10., 10., 10.);
        std::vector<std::array<float, 3>> face_colors;
        for (size_t i = 0; i < cube.its.indices.size(); ++i)
            face_colors.push_back(kFilamentColors[i % 4]);
        const ImageFillResult a = image_fill_from_face_colors(cube.its, face_colors, kFilamentColors, kFilamentIds);
        const ImageFillResult b = image_fill_from_face_colors(cube.its, face_colors, kFilamentColors, kFilamentIds);
        REQUIRE(a.ok);
        CHECK(a.facets_painted == cube.its.indices.size());
        CHECK(a.painting.bitstream == b.painting.bitstream);
        CHECK(a.painting.triangles_to_split == b.painting.triangles_to_split);
        CHECK(a.filaments_used == std::vector<int>{1, 2, 3, 4});
    }

    SECTION("a run of image_fill_compute is deterministic")
    {
        ImageAssetStore   store;
        const std::string sha = store.add(read_fixture("stripes3.png"));
        TriangleMesh      cube = make_cube(10., 10., 10.);
        ImageFillParams   p;
        p.asset = sha;
        p.axis  = ImageFillAxis::Y;
        p.subdivision = 3;
        p.allowed = kFilamentIds;
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult a = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
        const ImageFillResult b = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
        REQUIRE(a.ok);
        REQUIRE(b.ok);
        CHECK(a.painting.bitstream == b.painting.bitstream);
        CHECK(a.painting.triangles_to_split == b.painting.triangles_to_split);
    }
}

TEST_CASE("Image Fill: the params string round-trips", "[imagefill]")
{
    ImageFillParams p;
    p.asset           = "3d27b4ed2fdfdb12b533f2ddf6e113f5f6ad516b1acd9ebb3ed1de5476ec51c6";
    p.projection      = ImageFillProjection::Cylindrical;
    p.axis            = ImageFillAxis::X;
    p.faces           = ImageFillFaces::Through;
    p.axis_negative   = true;
    p.flip_u          = true;
    p.subdivision     = 4;
    p.detail_mm       = 0.35f;
    p.background      = 2;
    p.selection_state = 3;
    p.allowed         = {1, 3, 5};

    ImageFillParams back;
    REQUIRE(ImageFillParams::from_string(p.to_string(), back));
    CHECK(back.asset == p.asset);
    CHECK(back.projection == p.projection);
    CHECK(back.axis == p.axis);
    CHECK(back.faces == ImageFillFaces::Through);
    CHECK(back.axis_negative);
    CHECK(back.flip_u);
    CHECK_FALSE(back.flip_v);
    CHECK(back.subdivision == 4);
    CHECK(back.detail_mm == Approx(0.35f));
    CHECK(back.background == 2);
    CHECK(back.selection_state == 3);
    CHECK(back.allowed == std::vector<int>{1, 3, 5});
    CHECK(back.to_string() == p.to_string());

    SECTION("a gradient survives too")
    {
        ImageFillParams g;
        g.gradient.enabled    = true;
        g.gradient.three_stop = true;
        g.gradient.stop_a     = {0.f, 0.f, 0.f};
        g.gradient.stop_b     = {0.5f, 0.25f, 0.125f};
        g.gradient.stop_c     = {1.f, 1.f, 1.f};
        g.gradient.direction  = 2;
        ImageFillParams gb;
        REQUIRE(ImageFillParams::from_string(g.to_string(), gb));
        CHECK(gb.gradient.enabled);
        CHECK(gb.gradient.three_stop);
        CHECK(gb.gradient.direction == 2);
        CHECK(gb.gradient.stop_b[1] == Approx(0.25f));
        CHECK(gb.to_string() == g.to_string());
    }

    SECTION("garbage is refused rather than silently accepted")
    {
        ImageFillParams bad;
        CHECK_FALSE(ImageFillParams::from_string("", bad));
        CHECK_FALSE(ImageFillParams::from_string("proj=1;axis=0", bad));   // no version marker
    }

    SECTION("a string from before the face rule existed reads as the new default, not as All")
    {
        // An annotation written by the first Phase 2 build has no `pf`. It painted every facet,
        // but re-applying it should do the RIGHT thing rather than reproduce the bug, so the
        // missing key means Facing - and the part keeps the painting it already has until the
        // user asks for a new one.
        ImageFillParams old;
        REQUIRE(ImageFillParams::from_string("v=1;img=abc;proj=0;axis=2;sub=4;f=1,2,3", old));
        CHECK(old.faces == ImageFillFaces::Facing);
        CHECK_FALSE(old.axis_negative);
        // ...and what this build writes always says which rule it used.
        CHECK(old.to_string().find(";pf=0") != std::string::npos);
    }

    SECTION("a box projection and its mirror flag survive the string")
    {
        ImageFillParams b;
        b.asset      = "abc";
        b.projection = ImageFillProjection::Box;
        b.box_mirror = true;
        b.subdivision = 3;
        ImageFillParams bb;
        REQUIRE(ImageFillParams::from_string(b.to_string(), bb));
        CHECK(bb.projection == ImageFillProjection::Box);
        CHECK(bb.box_mirror);
        CHECK(bb.to_string() == b.to_string());
        // proj=3 must survive the clamp that used to stop at 2, or every saved box fill would
        // read back as a mesh-UV fill and repaint the part differently on reload.
        CHECK(b.to_string().find(";proj=3") != std::string::npos);
        // The flag is written only when it is set, so an unmirrored box fill's string is the
        // shorter one and an old string without `bm` reads as false.
        ImageFillParams plain = b;
        plain.box_mirror = false;
        CHECK(plain.to_string().find(";bm=") == std::string::npos);
        ImageFillParams pb;
        REQUIRE(ImageFillParams::from_string(plain.to_string(), pb));
        CHECK_FALSE(pb.box_mirror);
    }

    SECTION("a gradient with no image paints without an asset")
    {
        ImageAssetStore store;
        TriangleMesh    cube = make_cube(10., 10., 10.);
        ImageFillParams g;
        g.gradient.enabled = true;
        g.gradient.stop_a  = {0.f, 0.f, 0.f};
        g.gradient.stop_b  = {1.f, 1.f, 1.f};
        g.gradient.direction = 1;
        g.subdivision = 2;
        g.allowed     = kFilamentIds;
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult r = image_fill_compute(cube.its, empty, g, store, kFilamentColors, kFilamentIds);
        REQUIRE(r.ok);
        CHECK(r.facets_painted > 0);
        CHECK(store.empty());   // a gradient costs the project no payload at all
    }
}

// =============================================================================================
// 5. The 3MF round trip
// =============================================================================================

TEST_CASE("Image Fill: apply, save, reload - the painting and the asset are bit-identical", "[imagefill][3mf]")
{
    const std::vector<uint8_t> png = read_fixture("quad_rgbw.png");

    Model        src_model;
    ModelObject *src_object = src_model.add_object();
    src_object->name = "cube";
    ModelVolume *src_volume = src_object->add_volume(make_cube(20., 20., 20.));
    src_volume->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();

    const std::string sha = src_model.image_assets.add(png);
    REQUIRE(src_model.image_assets.size() == 1);

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = 2;
    p.allowed     = kFilamentIds;

    const ImageFillResult applied = image_fill_apply(*src_volume, p, src_model.image_assets,
                                                     kFilamentColors, kFilamentIds);
    REQUIRE(applied.ok);
    REQUIRE(applied.facets_painted > 0);
    const auto src_painting = src_volume->mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(src_painting.bitstream.empty());

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "image_fill_project.3mf").string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored = store_bbs_3mf(store_params);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                               &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                               nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                   LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);

    REQUIRE(stored);
    REQUIRE(loaded);
    REQUIRE(dst_model.objects.size() == 1);
    REQUIRE(dst_model.objects[0]->volumes.size() == 1);
    const ModelVolume *dst_volume = dst_model.objects[0]->volumes[0];

    THEN("the painted facets come back bit-identical")
    {
        const auto dst_painting = dst_volume->mmu_segmentation_facets.get_data();
        CHECK(dst_painting.triangles_to_split == src_painting.triangles_to_split);
        CHECK(dst_painting.bitstream == src_painting.bitstream);
    }

    THEN("the image asset comes back byte-identical, under the same hash")
    {
        REQUIRE(dst_model.image_assets.size() == 1);
        const ImageAsset *a = dst_model.image_assets.find(sha);
        REQUIRE(a != nullptr);
        CHECK(a->bytes == png);
        // ...and the hash of what came back is still the key it was filed under, which is the
        // statement that the archive did not transform the bytes on the way through.
        CHECK(image_fill_sha256_hex(a->bytes) == sha);
    }

    THEN("the annotation comes back, so the dialog can re-open on it")
    {
        ImageFillParams back;
        REQUIRE(image_fill_params_of(*dst_volume, back));
        CHECK(back.asset == sha);
        CHECK(back.projection == ImageFillProjection::Planar);
        CHECK(back.axis == ImageFillAxis::Z);
        CHECK(back.subdivision == 2);
        CHECK(back.allowed == kFilamentIds);
    }

    THEN("re-applying the reloaded annotation reproduces the same painting")
    {
        // The point of storing the params at all: the fill is reproducible, not just preserved.
        ImageFillParams back;
        REQUIRE(image_fill_params_of(*dst_volume, back));
        Model         redo_model;
        ModelObject  *redo_object = redo_model.add_object();
        ModelVolume  *redo_volume = redo_object->add_volume(make_cube(20., 20., 20.));
        redo_object->add_instance();
        redo_model.image_assets.add(png);
        const ImageFillResult r = image_fill_apply(*redo_volume, back, redo_model.image_assets,
                                                   kFilamentColors, kFilamentIds);
        REQUIRE(r.ok);
        CHECK(redo_volume->mmu_segmentation_facets.get_data().bitstream == src_painting.bitstream);
    }
}

// =============================================================================================
// 6. glTF import: the before/after
// =============================================================================================
//
// The change to GLB import in this phase is that a baseColorTexture is sampled at 16 points per
// triangle instead of one, and that the ImageFill service - not ObjColorMatch's CIE76 pass -
// decides which of the chosen filaments each sample belongs to. This measures it, on two files the
// reader has never seen (tests/data/image_fill/agent_*.glb, written by make_glb.py from struct and
// zlib, deliberately low-poly with a detailed texture).
//
// Both paths run in ONE binary on ONE import, which is a sharper comparison than two builds: the
// only thing that differs is which of the two Model::import_multi_volume_face_color_deal overloads
// is called. The filaments are chosen by obj_color_auto_match, exactly as a headless import does,
// so both sides get the same spools.
//
// The metric is the OLD path's own: CIE76 dE between each leaf's true texture colour and the
// colour of the filament it was painted with, area-weighted (every leaf of a uniform subdivision
// has the same area). Measuring the new path with the incumbent's metric is the conservative way
// round.
namespace {

struct GlbBeforeAfter
{
    size_t triangles = 0;
    size_t leaves    = 0;
    size_t painted_old = 0, painted_new = 0;
    size_t filaments_old = 0, filaments_new = 0;
    double mean_de_old = 0., mean_de_new = 0.;
    double max_de_old  = 0., max_de_new  = 0.;
};

bool measure_glb(const std::string &file, GlbBeforeAfter &out)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + file;

    Model       probe;
    GltfInfo    info;
    std::string message;
    if (!load_gltf(path.c_str(), &probe, info, message))
        return false;
    if (info.face_colors.empty() || info.sub_face_colors.empty() || info.sub_face_depth <= 0)
        return false;

    size_t per = 1;
    for (int i = 0; i < info.sub_face_depth; ++i) per *= 4;
    out.triangles = info.face_colors.size();
    out.leaves    = out.triangles * per;

    // The spools a user would have loaded: four, none of them a match for the texture, so the
    // matcher has to add slots - which is what a real import does.
    const std::vector<RGBA> existing = {RGBA{1.f, 1.f, 1.f, 1.f}, RGBA{0.f, 0.f, 0.f, 1.f},
                                        RGBA{0.85f, 0.1f, 0.1f, 1.f}, RGBA{0.1f, 0.2f, 0.8f, 1.f}};
    ObjColorMatchResult match;
    if (!obj_color_auto_match(info.face_colors, false, existing, match))
        return false;
    if (match.filament_ids.size() != info.face_colors.size())
        return false;

    auto slot_color = [&](int id) -> RGBA {
        if (id >= 1 && size_t(id) <= existing.size())
            return existing[size_t(id) - 1];
        const size_t k = size_t(id) - 1 - existing.size();
        return k < match.added_colors.size() ? match.added_colors[k] : RGBA{0.5f, 0.5f, 0.5f, 1.f};
    };

    // --- the old path: one filament per triangle -------------------------------------------
    {
        Model       m;
        GltfInfo    i2;
        std::string msg2;
        if (!load_gltf(path.c_str(), &m, i2, msg2))
            return false;
        if (!Model::import_multi_volume_face_color_deal(match.filament_ids, match.first_extruder_id, &m))
            return false;
        const ModelVolume *v = m.objects.front()->volumes.front();
        TriangleSelector   sel(v->mesh());
        sel.deserialize(v->mmu_segmentation_facets.get_data(), true);
        std::vector<int> used;
        double sum = 0.;
        for (size_t t = 0; t < out.triangles; ++t) {
            const int id = int(match.filament_ids[t]);
            if (id > 1) {
                ++out.painted_old;
                if (std::find(used.begin(), used.end(), id) == used.end()) used.push_back(id);
            }
            const RGBA c = slot_color(id);
            for (size_t k = 0; k < per; ++k) {
                const float d = obj_color_distance(info.sub_face_colors[t * per + k], c);
                sum += double(d);
                out.max_de_old = std::max(out.max_de_old, double(d));
            }
        }
        out.painted_old *= per;   // in leaves, so the two sides are comparable
        out.filaments_old = used.size();
        out.mean_de_old   = sum / double(out.leaves);
    }

    // --- the new path: one filament per sub-facet --------------------------------------------
    {
        Model       m;
        GltfInfo    i2;
        std::string msg2;
        if (!load_gltf(path.c_str(), &m, i2, msg2))
            return false;
        if (!Model::import_multi_volume_face_color_deal(match.filament_ids, i2.face_colors,
                                                        i2.sub_face_colors, i2.sub_face_depth,
                                                        match.first_extruder_id, &m))
            return false;
        const ModelVolume     *v = m.objects.front()->volumes.front();
        TriangleMesh           tm(v->mesh());
        const std::vector<int> states = leaf_states(tm, v->mmu_segmentation_facets.get_data(),
                                                    out.triangles, info.sub_face_depth);
        std::vector<int> used;
        double           sum = 0.;
        for (size_t i = 0; i < out.leaves; ++i) {
            const int id = states[i];
            if (id > 1) {
                ++out.painted_new;
                if (std::find(used.begin(), used.end(), id) == used.end()) used.push_back(id);
            }
            const float d = obj_color_distance(info.sub_face_colors[i], slot_color(id));
            sum += double(d);
            out.max_de_new = std::max(out.max_de_new, double(d));
        }
        out.filaments_new = used.size();
        out.mean_de_new   = sum / double(out.leaves);
    }
    return true;
}

} // namespace

TEST_CASE("Image Fill: a textured GLB is at least as good through the new path", "[imagefill][glb]")
{
    // BEFORE anything about colour: both fixtures must be closed solids wound OUTWARD. Both were
    // not, once. A uniformly inverted mesh is closed and locally self-consistent, so admesh
    // repairs nothing and reports nothing; its_volume() simply comes back negative, and the
    // plater then deletes the object with "The volume of the object is zero" - after the user has
    // already answered the colour dialog. The plaque was subtler still: only its two flat faces
    // were reversed, so it imported with visibly wrong normals and a volume exactly a third of
    // the truth. Nothing in the import path checks this, so the fixtures check it here.
    SECTION("the fixtures are closed solids wound outward")
    {
        struct Expect { const char *file; float volume; };
        // 40 x 40 x 3 = 4800 exactly; the medallion is a 16-gon of radius 15, height 4:
        // 16 * 0.5 * 15^2 * sin(2pi/16) * 4.
        const float gon = 16.f * 0.5f * 15.f * 15.f * std::sin(6.28318530718f / 16.f) * 4.f;
        for (const Expect &e : {Expect{"agent_plaque.glb", 4800.f}, Expect{"agent_medallion.glb", gon}}) {
            Model       m;
            GltfInfo    info;
            std::string msg;
            INFO("fixture " << e.file);
            REQUIRE(load_gltf((std::string(TEST_DATA_DIR) + "/image_fill/" + e.file).c_str(), &m, info, msg));
            REQUIRE(m.objects.size() == 1u);
            const indexed_triangle_set &its = m.objects.front()->volumes.front()->mesh().its;
            // Positive, and the RIGHT size: a partly inverted solid is still positive, just wrong.
            CHECK(its_volume(its) == Approx(e.volume).epsilon(0.001));
        }
    }

    for (const char *file : {"agent_plaque.glb", "agent_medallion.glb"}) {
        GlbBeforeAfter r;
        INFO("file " << file);
        REQUIRE(measure_glb(file, r));

        // Printed so the numbers land in the run log and can be quoted in the status document.
        WARN("GLB before/after " << file << ": triangles=" << r.triangles << " leaves=" << r.leaves
             << " | OLD painted=" << r.painted_old << " filaments=" << r.filaments_old
             << " meanDE=" << r.mean_de_old << " maxDE=" << r.max_de_old
             << " | NEW painted=" << r.painted_new << " filaments=" << r.filaments_new
             << " meanDE=" << r.mean_de_new << " maxDE=" << r.max_de_new);

        // "At least as good" means the colour error, not the filament count: a search that
        // reproduces a texture with SIX spools where the incumbent needed eight has done better,
        // not worse, so counting filaments is not the bar. What is:
        //   - every leaf still gets a filament (nothing is left unpainted that used to be),
        //   - the mean error against the true texture colour does not rise,
        //   - and neither does the worst single leaf.
        CHECK(r.painted_new >= r.painted_old);
        CHECK(r.mean_de_new <= r.mean_de_old);
        CHECK(r.max_de_new <= r.max_de_old);
        CHECK(r.filaments_new >= 2);
    }
}

// =============================================================================================
// 7. Bar B: the project a real slice is run on
// =============================================================================================
//
// Bar B is "a real slice of an applied image on a 3-filament setup, previewed via --export-3mf".
// The slicer's CLI has no Image fill dialog - Snapmaker_Orca.cpp hands read_from_file a null
// ObjImportColorFn - so the project has to be made here, by the real image_fill_apply, and sliced
// from disk afterwards. This case is the maker: it writes the project and asserts the painting is
// what the slice is supposed to show, so the file the CLI slices is never a mystery.
//
// Run it on its own with:  libslic3r_tests.exe "[barb]"
// It leaves %TEMP%\snorca_tests\image_fill_bar_b.3mf behind on purpose.
TEST_CASE("Image Fill: write the Bar B project - a cube with a three-colour image on its top face",
          "[imagefill][barb]")
{
    // Three filaments, the three the image is made of, so the answer is unambiguous and a tool
    // change has to happen wherever the picture changes band.
    const std::vector<std::array<float, 3>> three = {{0.85f, 0.15f, 0.15f},
                                                     {0.15f, 0.75f, 0.25f},
                                                     {0.15f, 0.25f, 0.85f}};
    const std::vector<int> ids = {1, 2, 3};

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "image fill cube";
    // 30 mm so the bands are several millimetres wide and survive the slicer's own resolution.
    ModelVolume *volume = object->add_volume(make_cube(30., 30., 30.));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string sha = model.image_assets.add(read_fixture("stripes3.png"));

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;   // the picture lies on the top face, u from x, v from y
    // Every facet, so this project stays the one the spec's section 6.3 measured: 3072 painted
    // leaves on 12 facets, three filaments, and a slice with tool changes wherever the picture
    // changes band. The dialog's default is now Facing; All is still what a Bar B slice wants,
    // because a cube painted on one face alone would exercise far less of the MMU path.
    p.faces       = ImageFillFaces::All;
    p.subdivision = 4;                  // 16 x 16 leaves per facet: the bands land cleanly
    p.allowed     = ids;

    const ImageFillResult res = image_fill_apply(*volume, p, model.image_assets, three, ids);
    REQUIRE(res.ok);
    // stripes3.png is three horizontal bands, so all three filaments must appear...
    CHECK(res.filaments_used == std::vector<int>{1, 2, 3});
    // ...and the whole surface is painted, because every facet of a cube gets a sample.
    CHECK(res.facets_painted == res.leaves_total);
    CHECK(res.leaves_total == volume->mesh().its.indices.size() * 256);

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string out = (tmp_root / "image_fill_bar_b.3mf").string();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    StoreParams        sp;
    sp.path     = out.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    WARN("Bar B project written to " << out << " (" << res.facets_painted << " painted facets, "
         << res.filaments_used.size() << " filaments, subdivision " << res.subdivision_used << ")");
}

// =============================================================================================
// 7b. Phase 3, step 4 Bar B - the image row: a plaque, a black-to-white ramp, three filaments
// =============================================================================================
//
// Unlike the [barb] project above, this one carries NO mmu_segmentation_facets painting at all -
// the image row is not a per-facet feature. What it needs instead is a MixedFilament row whose
// distribution_mode is ImageWeighted, with image_fill_ref pointing at the ramp and
// gradient_component_ids naming the three allowed physical filaments, referenced from
// solid_infill_filament so Fill.cpp's split_top_infill_by_image_row() picks it up for the
// plaque's (only) top surface. See docs/superpowers/specs/2026-09-07-imagemap-phase3-imagerow.md.
//
// Run it on its own with:  libslic3r_tests.exe "[barb3]"
// It leaves %TEMP%\snorca_tests\image_row_bar_b.3mf behind on purpose - that is the file
// snorca_hubtest's bar_b_imgrow_p3s4.py slices from disk.
TEST_CASE("Image Fill Phase 3: write the Bar B project - a plaque with a black-to-white image row",
          "[imagefill][ImageRow][barb3]")
{
    const std::vector<std::string> colors = {"#000000", "#808080", "#FFFFFF"};

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "image row plaque";
    // 50 x 50 mm footprint, 3 mm tall - the plan's own Bar B shape.
    ModelVolume *volume = object->add_volume(make_cube(50., 50., 3.));
    volume->name        = "plaque";
    object->add_instance();
    object->ensure_on_bed();

    // ramp_kw.png is a 1 x 16 vertical ramp, white (row 0) to black (row 15) - see
    // docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md's fixture list. A Planar
    // projection along Z gives u from x, v from y (the same convention the [barb] cube above
    // documents), so the ramp varies along the plaque's Y axis: one edge samples near-white, the
    // opposite edge near-black, with every shade in between at the rows in between - exactly the
    // "8 bands, monotonically increasing dark coverage" shape Bar B measures.
    const std::string sha = model.image_assets.add(read_fixture("ramp_kw.png"));

    ImageFillParams p;
    p.asset      = sha;
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};

    // The row: three physical filaments (black, mid-grey, white - the id order matches `colors`
    // above 1-based), ImageWeighted, referencing the ramp. add_custom_filament()'s own A/B pair
    // (1, 2) is unused by ImageWeighted's sampling - image_row_context_for_region() reads
    // gradient_component_ids instead (see that field's own comment, MixedFilament.hpp) - but it
    // still has to be a valid distinct pair for add_custom_filament() to accept the row at all.
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    MixedFilament &mf = mgr.mixed_filaments().front();
    mf.distribution_mode      = int(MixedFilament::ImageWeighted);
    mf.gradient_component_ids = "123";
    mf.image_fill_ref          = MixedFilamentManager::encode_image_fill_ref(p.to_string());
    REQUIRE_FALSE(mf.image_fill_ref.empty());
    const std::string serialized = mgr.serialize_custom_entries();

    // Virtual ids are enumerated over enabled rows starting at num_physical + 1 (see
    // MixedFilamentManager's own allocator, MixedFilament.cpp) - with 3 physical filaments and
    // this row being the only, first, enabled custom row, its virtual id is 4.
    const int virtual_id = int(colors.size()) + 1;

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string out = (tmp_root / "image_row_bar_b.3mf").string();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    // mixed_filament_definitions is project-wide state (which rows exist at all), not a
    // per-region print setting, so it belongs on the global config - and does survive
    // --process-preset/--filament-presets at CLI slice time (verified empirically: a plain
    // global solid_infill_filament override here does NOT survive process-preset selection,
    // because that key IS part of the process preset's own domain; mixed_filament_definitions
    // is not).
    cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(serialized));
    if (ConfigOptionStrings *colours_opt = cfg.option<ConfigOptionStrings>("filament_colour", true))
        colours_opt->values = colors;

    // solid_infill_filament (and the two fill-direction keys below) are PrintRegionConfig, i.e.
    // per-part settings - putting them on the VOLUME's own config, the same place
    // image_fill_apply() writes image_fill_params (ImageFill.cpp), is what makes them survive
    // --process-preset selection: a part's own settings override the process preset for that
    // part, exactly like the GUI's "add settings" panel.
    volume->config.set_key_value("solid_infill_filament", new ConfigOptionInt(virtual_id));
    // Force horizontal fill lines (one Y per line), stacked along Y - so each line samples one
    // roughly-constant band of the ramp instead of sweeping across the whole gradient itself.
    volume->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));

    StoreParams sp;
    sp.path     = out.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));
    WARN("Bar B (image row) project written to " << out << " (virtual filament id "
         << virtual_id << ", image_fill_ref len " << mf.image_fill_ref.size() << ")");
}

// =============================================================================================
// 8. A face selection is a merge, not a replacement
// =============================================================================================

TEST_CASE("Image Fill: applying to a face selection leaves the rest of the painting alone",
          "[imagefill][selection]")
{
    // Paint a cube by hand: facets 0 and 1 with filament 2 (the selection to fill), facets 4 and 5
    // with filament 3 (paint the user made earlier and must not lose). Then fill only the
    // filament-2 faces with an image, and check both halves of the promise.
    TriangleMesh cube = make_cube(10., 10., 10.);
    const size_t n = cube.its.indices.size();
    REQUIRE(n == 12);
    std::vector<int> before(n, 0);
    before[0] = 2; before[1] = 2;
    before[4] = 3; before[5] = 3;
    const auto existing = image_fill_encode(n, 0, before);

    ImageAssetStore   store;
    const std::string sha = store.add(read_fixture("quad_rgbw.png"));

    ImageFillParams p;
    p.asset           = sha;
    p.projection      = ImageFillProjection::Planar;
    p.axis            = ImageFillAxis::Z;
    // The selection is the subject here, so the projection is not allowed to cull anything: the
    // two facets the user picked must be the two the fill lands on, whichever way they face.
    p.faces           = ImageFillFaces::All;
    p.subdivision     = 2;
    p.allowed         = kFilamentIds;
    p.selection_state = 2;

    const ImageFillResult res = image_fill_compute(cube.its, existing, p, store, kFilamentColors,
                                                   kFilamentIds);
    REQUIRE(res.ok);

    TriangleSelector sel(cube);
    sel.deserialize(res.painting, /*needs_reset=*/true);

    // Bits of one original triangle, so "carried over verbatim" can be asserted literally.
    auto bits_of = [](const TriangleSelector::TriangleSplittingData &d, int tri) {
        std::vector<bool> out;
        for (size_t k = 0; k < d.triangles_to_split.size(); ++k) {
            if (d.triangles_to_split[k].triangle_idx != tri)
                continue;
            const size_t b = size_t(d.triangles_to_split[k].bitstream_start_idx);
            const size_t e = (k + 1 < d.triangles_to_split.size())
                                 ? size_t(d.triangles_to_split[k + 1].bitstream_start_idx)
                                 : d.bitstream.size();
            out.assign(d.bitstream.begin() + b, d.bitstream.begin() + e);
            break;
        }
        return out;
    };

    THEN("the two facets that were not selected are carried over bit for bit")
    {
        REQUIRE_FALSE(bits_of(existing, 4).empty());
        CHECK(bits_of(res.painting, 4) == bits_of(existing, 4));
        CHECK(bits_of(res.painting, 5) == bits_of(existing, 5));
    }
    THEN("only four original triangles carry anything: the two filled and the two kept")
    {
        REQUIRE(res.painting.triangles_to_split.size() == 4);
        std::vector<int> ids;
        for (const auto &m : res.painting.triangles_to_split)
            ids.push_back(m.triangle_idx);
        CHECK(ids == std::vector<int>{0, 1, 4, 5});
    }
    THEN("the selected facets carry the image, and the leaf count adds up")
    {
        // 2 selected facets x 16 leaves, plus the 2 untouched facets that were never split.
        size_t leaves = 0;
        for (int st = 1; st <= 16; ++st)
            leaves += size_t(sel.num_facets(EnforcerBlockerType(st)));
        CHECK(leaves == 2u * 16u + 2u);
        // The quad image has four colours and the two selected facets span all of them, so the
        // fill is not simply the one flat filament it replaced.
        CHECK(res.filaments_used.size() >= 2);
        // The facets that were filled no longer read as a single undivided filament 2.
        CHECK(bits_of(res.painting, 0) != bits_of(existing, 0));
    }

    SECTION("a selection nothing is painted with is refused, rather than silently painting all")
    {
        ImageFillParams q = p;
        q.selection_state = 5;
        const ImageFillResult bad = image_fill_compute(cube.its, existing, q, store, kFilamentColors,
                                                       kFilamentIds);
        CHECK_FALSE(bad.ok);
        CHECK_FALSE(bad.error.empty());
    }
}

// =============================================================================================
// 9. Which faces a projection lands on, and how the two projections differ
// =============================================================================================
//
// The bugs these pin, both reported from a 30 mm cube with a three-vertical-band image:
//   1. "Flat, along an axis" with axis Z painted the bands on the top face AND down all four
//      sides, and on the bottom. A projection is a direction, not a solid: it lands on what faces
//      it and on nothing else.
//   2. "Wrapped around an axis" looked like the flat one. The wrap has to vary with the angle
//      about the axis, so a cube's four sides come out four different colours and its caps -
//      whose normals are parallel to the axis - come out unpainted.
//   3. Axis Z and axis Y gave the same answer. For an image whose colour depends only on u that
//      is arithmetic, not a bug: planar Z takes u from x and so does planar Y. It is pinned here
//      so nobody "fixes" it, together with axis X, which does differ.

namespace facing_test {

// The unit normal of an original facet, straight from the mesh.
Vec3f facet_normal(const indexed_triangle_set &its, size_t t)
{
    const Vec3i32 &f = its.indices[t];
    const Vec3f   &a = its.vertices[f(0)], &b = its.vertices[f(1)], &c = its.vertices[f(2)];
    return (b - a).cross(c - a).normalized();
}

// Which of the six faces of an axis-aligned box this facet belongs to: 0 = +X, 1 = -X, 2 = +Y,
// 3 = -Y, 4 = +Z, 5 = -Z. -1 if it is not axis aligned.
int cube_face_of(const indexed_triangle_set &its, size_t t)
{
    const Vec3f n = facet_normal(its, t);
    for (int ax = 0; ax < 3; ++ax) {
        if (n[ax] > 0.99f)  return ax * 2;
        if (n[ax] < -0.99f) return ax * 2 + 1;
    }
    return -1;
}

size_t leaves_per(int depth)
{
    size_t per = 1;
    for (int i = 0; i < depth; ++i) per *= 4;
    return per;
}

// The set of box faces that came out with any paint on them.
std::set<int> painted_faces(const TriangleMesh &cube, const std::vector<int> &states, int depth)
{
    const size_t per = leaves_per(depth);
    std::set<int> out;
    for (size_t t = 0; t < cube.its.indices.size(); ++t)
        for (size_t i = 0; i < per; ++i)
            if (states[t * per + i] != 0) { out.insert(cube_face_of(cube.its, t)); break; }
    return out;
}

std::vector<int> run_states(const TriangleMesh &cube, const ImageFillParams &p, const ImageAssetStore &store,
                     int depth)
{
    TriangleSelector::TriangleSplittingData empty;
    const ImageFillResult r = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
    REQUIRE(r.ok);
    return leaf_states(cube, r.painting, cube.its.indices.size(), depth);
}

} // namespace facing_test

using namespace facing_test;

TEST_CASE("Image Fill: a flat projection paints only the faces it points at", "[imagefill][faces]")
{
    ImageAssetStore   store;
    const std::string sha = store.add(read_fixture("bands3.png"));   // R | G | B, vertical bands
    TriangleMesh      cube = make_cube(20., 20., 20.);
    const int         depth = 2;

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.axis        = ImageFillAxis::Z;
    p.subdivision = depth;
    p.allowed     = kFilamentIds;

    SECTION("along +Z it lands on the top face: not the bottom, not the four sides")
    {
        const std::vector<int> st = run_states(cube, p, store, depth);
        CHECK(painted_faces(cube, st, depth) == std::set<int>{4});
        // ...and the three bands are all there, so "only the top" is not "only one band".
        std::set<int> used;
        for (int s : st) if (s != 0) used.insert(s);
        CHECK(used == std::set<int>{1, 2, 3});
    }

    SECTION("the other side of the axis lands on the bottom face instead")
    {
        ImageFillParams q = p;
        q.axis_negative = true;
        CHECK(painted_faces(cube, run_states(cube, q, store, depth), depth) == std::set<int>{5});
    }

    SECTION("through both sides lands on the top AND the bottom, and still not on the sides")
    {
        ImageFillParams q = p;
        q.faces = ImageFillFaces::Through;
        CHECK(painted_faces(cube, run_states(cube, q, store, depth), depth) == std::set<int>{4, 5});
    }

    SECTION("a face parallel to the axis is painted by neither rule")
    {
        // The four sides of a cube under a projection along Z have normal . axis == 0 exactly.
        // Stated directly against the predicate, so the rule is pinned and not only its effect.
        BoundingBoxf3 box(Vec3d(0, 0, 0), Vec3d(20, 20, 20));
        const Vec3f   side(1.f, 0.f, 0.f), top(0.f, 0.f, 1.f), bottom(0.f, 0.f, -1.f);
        const Vec3f   centre(10.f, 10.f, 10.f);
        ImageFillParams q = p;
        CHECK(image_fill_face_is_painted(q, box, top, centre));
        CHECK_FALSE(image_fill_face_is_painted(q, box, bottom, centre));
        CHECK_FALSE(image_fill_face_is_painted(q, box, side, centre));
        q.faces = ImageFillFaces::Through;
        CHECK(image_fill_face_is_painted(q, box, top, centre));
        CHECK(image_fill_face_is_painted(q, box, bottom, centre));
        CHECK_FALSE(image_fill_face_is_painted(q, box, side, centre));   // still not the sides
        q.faces = ImageFillFaces::All;
        CHECK(image_fill_face_is_painted(q, box, side, centre));         // All means all
    }

    SECTION("ImageFillFaces::All is the old behaviour, and is still reachable from code")
    {
        ImageFillParams q = p;
        q.faces = ImageFillFaces::All;
        CHECK(painted_faces(cube, run_states(cube, q, store, depth), depth) ==
              std::set<int>{0, 1, 2, 3, 4, 5});
    }
}

TEST_CASE("Image Fill: a wrap varies with the angle, and is not the flat projection",
          "[imagefill][faces]")
{
    ImageAssetStore   store;
    const std::string wrap  = store.add(read_fixture("wrap4.png"));    // W R R G G B B W
    const std::string bands = store.add(read_fixture("bands3.png"));   // R G B
    TriangleMesh      cube  = make_cube(20., 20., 20.);
    const int         depth = 2;
    const size_t      per   = leaves_per(depth);

    ImageFillParams cyl;
    cyl.asset       = wrap;
    cyl.projection  = ImageFillProjection::Cylindrical;
    cyl.axis        = ImageFillAxis::Z;
    cyl.subdivision = depth;
    cyl.allowed     = kFilamentIds;

    SECTION("four sides, four colours, and the caps left alone")
    {
        const std::vector<int> st = run_states(cube, cyl, store, depth);

        // The caps' normals are parallel to the axis: their outward radial component is exactly
        // zero, so a wrap does not reach them. That is the documented rule, not an accident.
        CHECK(painted_faces(cube, st, depth) == std::set<int>{0, 1, 2, 3});

        // wrap4.png is offset by half a band so each side face sits in the MIDDLE of one band:
        // seam on -X, u = 0.5 facing +X. Expected, per face: -X white(4), -Y red(1), +X green(2),
        // +Y blue(3). Leaves whose own u lands within a hair of a band edge are skipped - the
        // nearest-neighbour sampler is allowed either pixel there and this test is not about that.
        std::map<int, std::set<int>> per_face;
        size_t                       checked = 0;
        for (size_t t = 0; t < cube.its.indices.size(); ++t) {
            const int face = cube_face_of(cube.its, t);
            if (face > 3) continue;
            std::vector<ImageFillLeaf> leaves;
            const Vec3i32 &f = cube.its.indices[t];
            image_fill_subdivide(cube.its.vertices[f(0)], cube.its.vertices[f(1)],
                                 cube.its.vertices[f(2)], depth, leaves);
            for (size_t i = 0; i < leaves.size(); ++i) {
                const Vec3f c = leaves[i].centroid();
                const double two_pi = 6.283185307179586;
                const float  u = float(std::atan2(double(c.y()) - 10.0, double(c.x()) - 10.0) / two_pi + 0.5);
                bool        near_edge = false;
                for (float edge : {0.125f, 0.375f, 0.625f, 0.875f})
                    if (std::abs(u - edge) < 0.01f) near_edge = true;
                if (near_edge) continue;
                per_face[face].insert(st[t * per + i]);
                ++checked;
            }
        }
        REQUIRE(checked > 60);
        REQUIRE(per_face.size() == 4);
        for (const auto &kv : per_face) {
            INFO("box face " << kv.first);
            CHECK(kv.second.size() == 1);   // one colour per side, all the way across it
        }
        CHECK(per_face[0] == std::set<int>{2});   // +X green
        CHECK(per_face[1] == std::set<int>{4});   // -X white, the seam, both halves the same
        CHECK(per_face[2] == std::set<int>{3});   // +Y blue
        CHECK(per_face[3] == std::set<int>{1});   // -Y red
    }

    SECTION("the inward side is what the other direction selects")
    {
        // Nothing of a solid cube faces inwards, so asking for the inward surfaces of one is a
        // refusal with a message rather than a silent whole-part paint.
        ImageFillParams q = cyl;
        q.axis_negative = true;
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult r = image_fill_compute(cube.its, empty, q, store, kFilamentColors,
                                                     kFilamentIds);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
    }

    SECTION("flat and wrapped are different paintings of the same image on the same cube")
    {
        // The reported symptom was that the two dropdown entries produced the same result. With
        // the default face rule they cannot even touch the same facets...
        ImageFillParams flat;
        flat.asset       = bands;
        flat.projection  = ImageFillProjection::Planar;
        flat.axis        = ImageFillAxis::Z;
        flat.subdivision = depth;
        flat.allowed     = kFilamentIds;
        ImageFillParams wrapped = flat;
        wrapped.projection      = ImageFillProjection::Cylindrical;

        CHECK(painted_faces(cube, run_states(cube, flat, store, depth), depth) == std::set<int>{4});
        CHECK(painted_faces(cube, run_states(cube, wrapped, store, depth), depth) ==
              std::set<int>{0, 1, 2, 3});

        // ...and even with the culling switched off, so both paint all twelve facets, the two
        // answers differ on a large share of the leaves. This is the statement that the
        // cylindrical maths is a wrap and not a second copy of the planar one.
        flat.faces    = ImageFillFaces::All;
        wrapped.faces = ImageFillFaces::All;
        const std::vector<int> a = run_states(cube, flat, store, depth);
        const std::vector<int> b = run_states(cube, wrapped, store, depth);
        REQUIRE(a.size() == b.size());
        size_t differ = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++differ;
        INFO(differ << " of " << a.size() << " leaves differ");
        CHECK(differ > a.size() / 5);
    }
}

TEST_CASE("Image Fill: axis Y matches axis Z for a vertical-band image, and axis X does not",
          "[imagefill][faces]")
{
    // Reported as suspicious and deliberately NOT changed. A planar projection takes u and v from
    // the two axes that are not the projection axis, in ascending order: Z gives (x, y), Y gives
    // (x, z), X gives (y, z). So for an image whose colour depends only on u, Z and Y are the
    // same function of position - both read the colour off x - and the paintings are identical
    // facet for facet. Axis X reads it off y instead, and differs.
    ImageAssetStore   store;
    const std::string sha = store.add(read_fixture("bands3.png"));
    TriangleMesh      cube = make_cube(20., 20., 20.);
    const int         depth = 2;

    ImageFillParams p;
    p.asset       = sha;
    p.projection  = ImageFillProjection::Planar;
    p.subdivision = depth;
    p.allowed     = kFilamentIds;
    p.faces       = ImageFillFaces::All;   // compare the projections, not which faces they reach

    p.axis = ImageFillAxis::Z;
    const std::vector<int> z = run_states(cube, p, store, depth);
    p.axis = ImageFillAxis::Y;
    const std::vector<int> y = run_states(cube, p, store, depth);
    p.axis = ImageFillAxis::X;
    const std::vector<int> x = run_states(cube, p, store, depth);

    CHECK(z == y);   // arithmetic, not a bug
    CHECK(z != x);
    size_t differ = 0;
    for (size_t i = 0; i < z.size(); ++i)
        if (z[i] != x[i]) ++differ;
    CHECK(differ > z.size() / 5);

    SECTION("with the face rule on, Y and Z stop agreeing - they land on different faces")
    {
        p.faces = ImageFillFaces::Facing;
        p.axis  = ImageFillAxis::Z;
        const std::vector<int> fz = run_states(cube, p, store, depth);
        p.axis  = ImageFillAxis::Y;
        const std::vector<int> fy = run_states(cube, p, store, depth);
        CHECK(fz != fy);
        CHECK(painted_faces(cube, fz, depth) == std::set<int>{4});
        CHECK(painted_faces(cube, fy, depth) == std::set<int>{2});
    }
}

// =============================================================================================
// 9. The BOX projection - one image on all six sides
// =============================================================================================

namespace box_test {

// The orientation rule, written out here INDEPENDENTLY of the implementation, straight from the
// spec's table - so this test measures the rule rather than restating the code that implements it.
//
//   face  |  u (image right)  |  v (image up)  |  u when box_mirror
//    +X   |        +Y         |      +Z        |   +Y  (unchanged)
//    -X   |        +Y         |      +Z        |   -Y
//    +Y   |        +X         |      +Z        |   -X
//    -Y   |        +X         |      +Z        |   +X  (unchanged)
//    +Z   |        +X         |      +Y        |   +X  (unchanged)
//    -Z   |        +X         |      +Y        |   -X
struct FaceAxes { int ua; bool u_flipped_when_mirrored; int va; };

FaceAxes axes_of_face(int face)
{
    switch (face) {
    case 0: return {1, false, 2};   // +X
    case 1: return {1, true,  2};   // -X
    case 2: return {0, true,  2};   // +Y
    case 3: return {0, false, 2};   // -Y
    case 4: return {0, false, 1};   // +Z
    default: return {0, true, 1};   // -Z
    }
}

// The (u, v) the rule says a point on `face` of a cube spanning [0, side] must get.
void expected_uv(int face, const Vec3f &p, float side, bool mirror, float &u, float &v)
{
    const FaceAxes fa = axes_of_face(face);
    u = p[fa.ua] / side;
    v = p[fa.va] / side;
    if (mirror && fa.u_flipped_when_mirrored)
        u = 1.f - u;
}

// quad_rgbw.png is 2x2: row 0 (which is v = 1, the TOP of the image) is red, green; row 1
// (v = 0) is blue, white. So the filament a point must come out with is a pure function of
// which half of u and which half of v it is in.
int expected_filament_quad(float u, float v)
{
    if (v > 0.5f) return u < 0.5f ? 1 : 2;   // top row:    red   | green
    return              u < 0.5f ? 3 : 4;    // bottom row: blue  | white
}

// An icosphere: 12 vertices, 20 faces, each triangle split into 4 `subdiv` times and every
// vertex pushed back out to the radius. Chosen over its_make_sphere() on purpose - a UV sphere
// has degenerate zero-area triangles at its poles, which a projection is entitled to leave
// unpainted, and that would blunt the point of the "nothing is left unpainted" check.
indexed_triangle_set icosphere(float radius, int subdiv)
{
    const float t = (1.f + std::sqrt(5.f)) / 2.f;
    std::vector<Vec3f> v = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0},
        {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
        {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    std::vector<Vec3i32> f = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int s = 0; s < subdiv; ++s) {
        std::vector<Vec3i32> nf;
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            const std::pair<int, int> k(std::min(a, b), std::max(a, b));
            auto it = mid.find(k);
            if (it != mid.end()) return it->second;
            v.push_back((v[a] + v[b]) * 0.5f);
            const int idx = int(v.size()) - 1;
            mid.emplace(k, idx);
            return idx;
        };
        for (const Vec3i32 &tri : f) {
            const int a = midpoint(tri(0), tri(1)), b = midpoint(tri(1), tri(2)), c = midpoint(tri(2), tri(0));
            nf.push_back({tri(0), a, c});
            nf.push_back({tri(1), b, a});
            nf.push_back({tri(2), c, b});
            nf.push_back({a, b, c});
        }
        f.swap(nf);
    }
    indexed_triangle_set its;
    its.vertices.reserve(v.size());
    for (const Vec3f &p : v) its.vertices.push_back(p.normalized() * radius);
    its.indices = f;
    return its;
}

} // namespace box_test

TEST_CASE("Image Fill: the box projection paints all six sides, each the right way up",
          "[imagefill][faces][box]")
{
    using namespace box_test;

    ImageAssetStore   store;
    const std::string quad = store.add(read_fixture("quad_rgbw.png"));   // R G / B W
    const float       side = 20.f;
    TriangleMesh      cube = make_cube(double(side), double(side), double(side));
    const int         depth = 2;
    const size_t      per   = leaves_per(depth);

    ImageFillParams p;
    p.asset       = quad;
    p.projection  = ImageFillProjection::Box;
    p.subdivision = depth;
    p.allowed     = kFilamentIds;

    SECTION("every one of the six faces is painted, and all four quadrant colours are used")
    {
        const std::vector<int> st = run_states(cube, p, store, depth);
        CHECK(painted_faces(cube, st, depth) == std::set<int>{0, 1, 2, 3, 4, 5});
        // Nothing is left unpainted at all: a box projection reaches every facet by construction.
        CHECK(std::count(st.begin(), st.end(), 0) == 0);
        std::set<int> used;
        for (int s : st) used.insert(s);
        CHECK(used == std::set<int>{1, 2, 3, 4});
    }

    SECTION("the axis and the negative-side flag make no difference to it")
    {
        // Box has no axis of its own; if either of these changed the painting, the dialog would
        // be showing a control that silently matters.
        const std::vector<int> base = run_states(cube, p, store, depth);
        ImageFillParams q = p;
        q.axis = ImageFillAxis::X;
        CHECK(run_states(cube, q, store, depth) == base);
        q.axis = ImageFillAxis::Y;
        q.axis_negative = true;
        CHECK(run_states(cube, q, store, depth) == base);
        // ...and neither does "Faces", which is why the dialog disables it.
        ImageFillParams r = p;
        r.faces = ImageFillFaces::Through;
        CHECK(run_states(cube, r, store, depth) == base);
    }

    SECTION("each face carries the image's quadrants in the orientation the rule states")
    {
        for (bool mirror : {false, true}) {
            ImageFillParams q = p;
            q.box_mirror = mirror;
            const std::vector<int> st = run_states(cube, q, store, depth);
            INFO("box_mirror = " << mirror);
            size_t checked = 0;
            for (size_t t = 0; t < cube.its.indices.size(); ++t) {
                const int face = cube_face_of(cube.its, t);
                REQUIRE(face >= 0);
                std::vector<ImageFillLeaf> leaves;
                const Vec3i32 &tri = cube.its.indices[t];
                image_fill_subdivide(cube.its.vertices[tri(0)], cube.its.vertices[tri(1)],
                                     cube.its.vertices[tri(2)], depth, leaves);
                for (size_t i = 0; i < leaves.size(); ++i) {
                    const Vec3f c = leaves[i].centroid();
                    float u = 0.f, v = 0.f;
                    expected_uv(face, c, side, mirror, u, v);
                    // The nearest-neighbour sampler may take either pixel exactly on a boundary.
                    if (std::abs(u - 0.5f) < 1e-3f || std::abs(v - 0.5f) < 1e-3f) continue;
                    INFO("face " << face << " leaf " << c.x() << "," << c.y() << "," << c.z()
                                 << " -> u " << u << " v " << v);
                    CHECK(st[t * per + i] == expected_filament_quad(u, v));
                    ++checked;
                }
            }
            CHECK(checked > 60);
        }
    }

    SECTION("named corners, so the rule is legible and not only computed")
    {
        // The four corners of a face, as (filament, description). Read them off the table: the
        // image's top-left pixel is red, top-right green, bottom-left blue, bottom-right white,
        // and "top" is v = 1.
        const std::vector<int> plain    = run_states(cube, p, store, depth);
        ImageFillParams        m        = p;
        m.box_mirror                    = true;
        const std::vector<int> mirrored = run_states(cube, m, store, depth);

        auto state_at = [&](const std::vector<int> &st, int face, const Vec3f &want) {
            // The leaf of THAT face whose centroid is nearest `want`. Restricted to the face on
            // purpose: near an edge the nearest leaf of the neighbouring face can be closer, and
            // the question here is always "what did THIS face get here".
            size_t best = 0; float bd = 1e30f;
            for (size_t t = 0; t < cube.its.indices.size(); ++t) {
                if (cube_face_of(cube.its, t) != face) continue;
                std::vector<ImageFillLeaf> leaves;
                const Vec3i32 &tri = cube.its.indices[t];
                image_fill_subdivide(cube.its.vertices[tri(0)], cube.its.vertices[tri(1)],
                                     cube.its.vertices[tri(2)], depth, leaves);
                for (size_t i = 0; i < leaves.size(); ++i) {
                    const float d = (leaves[i].centroid() - want).squaredNorm();
                    if (d < bd) { bd = d; best = t * per + i; }
                }
            }
            return st[best];
        };

        // TOP (+Z): u = +X, v = +Y. Image top-left (red) is at low x, HIGH y.
        CHECK(state_at(plain, 4, Vec3f(1.f, 19.f, 20.f)) == 1);    // red
        CHECK(state_at(plain, 4, Vec3f(19.f, 19.f, 20.f)) == 2);   // green
        CHECK(state_at(plain, 4, Vec3f(1.f, 1.f, 20.f)) == 3);     // blue
        CHECK(state_at(plain, 4, Vec3f(19.f, 1.f, 20.f)) == 4);    // white
        // ...which is exactly what a flat projection along +Z gives, mirror or not: +Z is one of
        // the three faces the mirror leaves alone.
        CHECK(state_at(mirrored, 4, Vec3f(1.f, 19.f, 20.f)) == 1);

        // FRONT (-Y): u = +X, v = +Z. Red at low x, HIGH z - and the mirror leaves it alone.
        CHECK(state_at(plain, 3, Vec3f(1.f, 0.f, 19.f)) == 1);
        CHECK(state_at(plain, 3, Vec3f(19.f, 0.f, 19.f)) == 2);
        CHECK(state_at(mirrored, 3, Vec3f(1.f, 0.f, 19.f)) == 1);

        // BACK (+Y): u = +X by default, so red lands at LOW x - which, seen from behind the box,
        // is on the viewer's RIGHT: the picture reads mirrored. With box_mirror it moves to high
        // x, i.e. the viewer's left, and reads the right way round.
        CHECK(state_at(plain, 2, Vec3f(1.f, 20.f, 19.f)) == 1);     // red at low x: mirrored
        CHECK(state_at(mirrored, 2, Vec3f(19.f, 20.f, 19.f)) == 1); // red at high x: readable
        CHECK(state_at(mirrored, 2, Vec3f(1.f, 20.f, 19.f)) == 2);  // green takes low x

        // RIGHT (+X): u = +Y, v = +Z; the mirror leaves it alone.
        CHECK(state_at(plain, 0, Vec3f(20.f, 1.f, 19.f)) == 1);
        CHECK(state_at(mirrored, 0, Vec3f(20.f, 1.f, 19.f)) == 1);
        // LEFT (-X): u = +Y by default (mirrored from outside); box_mirror flips it.
        CHECK(state_at(plain, 1, Vec3f(0.f, 1.f, 19.f)) == 1);
        CHECK(state_at(mirrored, 1, Vec3f(0.f, 19.f, 19.f)) == 1);

        // BOTTOM (-Z): u = +X, v = +Y; box_mirror flips u.
        CHECK(state_at(plain, 5, Vec3f(1.f, 19.f, 0.f)) == 1);
        CHECK(state_at(mirrored, 5, Vec3f(19.f, 19.f, 0.f)) == 1);
    }

    SECTION("the top face agrees, leaf for leaf, with a flat projection along +Z")
    {
        // Not a restatement of the rule but a consequence of it: the default box orientation IS
        // the planar one on +X, -Y and +Z. If this ever stops holding, the two projections have
        // drifted apart and one of them is wrong.
        ImageFillParams flat = p;
        flat.projection = ImageFillProjection::Planar;
        flat.axis       = ImageFillAxis::Z;
        const std::vector<int> fz  = run_states(cube, flat, store, depth);
        const std::vector<int> box = run_states(cube, p, store, depth);
        size_t compared = 0;
        for (size_t t = 0; t < cube.its.indices.size(); ++t) {
            if (cube_face_of(cube.its, t) != 4) continue;   // +Z only; flat paints nothing else
            for (size_t i = 0; i < per; ++i) {
                CHECK(box[t * per + i] == fz[t * per + i]);
                ++compared;
            }
        }
        CHECK(compared > 0);
    }

    SECTION("mirroring changes the four side faces and the bottom, and leaves +Z alone")
    {
        ImageFillParams m = p;
        m.box_mirror = true;
        const std::vector<int> plain    = run_states(cube, p, store, depth);
        const std::vector<int> mirrored = run_states(cube, m, store, depth);
        CHECK(plain != mirrored);
        // -X, +Y and -Z are the three the mirror touches; +X, -Y and +Z it must not.
        for (size_t t = 0; t < cube.its.indices.size(); ++t) {
            const int face = cube_face_of(cube.its, t);
            if (face != 0 && face != 3 && face != 4) continue;
            for (size_t i = 0; i < per; ++i)
                CHECK(plain[t * per + i] == mirrored[t * per + i]);
        }
    }
}

TEST_CASE("Image Fill: the box projection leaves no facet of a sphere unpainted",
          "[imagefill][faces][box]")
{
    using namespace box_test;

    ImageAssetStore   store;
    const std::string quad = store.add(read_fixture("quad_rgbw.png"));
    // Two subdivisions of an icosahedron: 320 facets, none degenerate, normals in every
    // direction - so "the dominant axis decides" is being asked a real question 320 times.
    const indexed_triangle_set ball = icosphere(10.f, 2);
    REQUIRE(ball.indices.size() == 320u);
    const int    depth = 1;
    const size_t per   = leaves_per(depth);

    ImageFillParams p;
    p.asset       = quad;
    p.projection  = ImageFillProjection::Box;
    p.subdivision = depth;
    p.allowed     = kFilamentIds;

    TriangleSelector::TriangleSplittingData empty;
    const ImageFillResult r = image_fill_compute(ball, empty, p, store, kFilamentColors, kFilamentIds);
    REQUIRE(r.ok);
    CHECK(r.leaves_total == ball.indices.size() * per);
    // THE POINT OF THIS TEST: a tri-planar projection has no unreachable facet. A planar one
    // leaves the whole far hemisphere and the silhouette band unpainted; this leaves nothing.
    CHECK(r.facets_painted == r.leaves_total);

    TriangleMesh           tm(ball);
    const std::vector<int> st = leaf_states(tm, r.painting, ball.indices.size(), depth);
    CHECK(std::count(st.begin(), st.end(), 0) == 0);

    SECTION("every facet stays on ONE face: no leaf of a facet disagrees with its own normal")
    {
        // The seam question. The face is chosen from the FACET normal, so all 4^depth leaves of
        // one facet must be projected along the same axis - a seam can only ever fall along a
        // triangle edge, never across the middle of a triangle.
        for (size_t t = 0; t < ball.indices.size(); ++t) {
            const Vec3i32 &tri = ball.indices[t];
            const Vec3f    n   = (ball.vertices[tri(1)] - ball.vertices[tri(0)])
                                  .cross(ball.vertices[tri(2)] - ball.vertices[tri(0)]);
            const int face = image_fill_box_face(n);
            REQUIRE(face >= 0);
            BoundingBoxf3 box;
            for (const Vec3f &vtx : ball.vertices) box.merge(vtx.cast<double>());
            std::vector<ImageFillLeaf> leaves;
            image_fill_subdivide(ball.vertices[tri(0)], ball.vertices[tri(1)],
                                 ball.vertices[tri(2)], depth, leaves);
            for (const ImageFillLeaf &leaf : leaves) {
                float u = 0.f, v = 0.f;
                REQUIRE(image_fill_project(p, box, leaf.centroid(), u, v, &n));
                // The value the rule gives for THAT face, with the whole facet on one face.
                const FaceAxes fa = axes_of_face(face);
                const Vec3f    c  = leaf.centroid();
                const float    eu = float((c[fa.ua] - box.min[fa.ua]) / box.size()[fa.ua]);
                const float    ev = float((c[fa.va] - box.min[fa.va]) / box.size()[fa.va]);
                CHECK(u == Approx(eu).margin(1e-5));
                CHECK(v == Approx(ev).margin(1e-5));
            }
        }
    }

    SECTION("a planar projection on the same ball does leave facets unpainted")
    {
        // The control that makes the number above mean something.
        ImageFillParams flat = p;
        flat.projection = ImageFillProjection::Planar;
        flat.axis       = ImageFillAxis::Z;
        TriangleSelector::TriangleSplittingData e2;
        const ImageFillResult fr = image_fill_compute(ball, e2, flat, store, kFilamentColors, kFilamentIds);
        REQUIRE(fr.ok);
        CHECK(fr.facets_painted < fr.leaves_total);
    }
}

// =============================================================================================
// Phase 3, step 2: the image row sampler
// =============================================================================================
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-edgeslicer-plan.md, section 4, Phase 3.
// image_fill_sample_segment() has to answer the same question image_fill_compute() answers per
// leaf centroid - "what colour does the image put here" - but for an arbitrary point on a fill
// segment rather than a subdivision leaf. The tests below hold it to that in two ways: agreement
// with the real facet-painting pipeline on a cube (not merely "both call the same helper" in the
// abstract), and known colours at known positions against the same fixtures the phase 2 tests use.

TEST_CASE("Image Fill: the segment sampler agrees with the facet painter at facet centroids",
          "[imagefill][imagerow]")
{
    ImageAssetStore   store;
    const std::string quad = store.add(read_fixture("quad_rgbw.png"));
    TriangleMesh      cube = make_cube(20., 20., 20.);
    BoundingBoxf3     box;
    for (const Vec3f &v : cube.its.vertices) box.merge(v.cast<double>());

    // Runs image_fill_compute() at subdivision 0 - one leaf per facet, so a leaf's centroid IS
    // the facet's centroid - then, for every leaf compute() actually painted, asks the sampler
    // for the colour at that same centroid (a zero-length "segment") and replicates compute()'s
    // own nearest-palette lookup on the sampler's answer. The two must agree on the FILAMENT ID,
    // which only happens if they agreed on u, v and on the pixel first: end to end, not merely
    // "both call image_fill_project somewhere".
    auto check_against_compute = [&](const ImageFillParams &p) {
        TriangleSelector::TriangleSplittingData empty;
        const ImageFillResult r = image_fill_compute(cube.its, empty, p, store, kFilamentColors, kFilamentIds);
        REQUIRE(r.ok);
        REQUIRE(r.subdivision_used == 0);
        const std::vector<int> states = leaf_states(cube, r.painting, cube.its.indices.size(), 0);
        size_t checked = 0;
        for (size_t t = 0; t < cube.its.indices.size(); ++t) {
            if (states[t] == 0)
                continue;   // a facet Facing culled (the sides, for the flat case) - not this test's question
            const Vec3i32 &f = cube.its.indices[t];
            const Vec3f   &a = cube.its.vertices[f(0)], &b = cube.its.vertices[f(1)], &c = cube.its.vertices[f(2)];
            const Vec3f    n        = (b - a).cross(c - a);
            const Vec3f    centroid = (a + b + c) / 3.f;

            const std::vector<ImageRowSample> s = image_fill_sample_segment(p, box, store, centroid, centroid, n, 1.f);
            REQUIRE(s.size() == 1);
            REQUIRE(s[0].has_color);

            size_t best  = 0;
            float  bestd = std::numeric_limits<float>::max();
            for (size_t i = 0; i < r.palette.colors.size(); ++i) {
                const float dr = s[0].color[0] - r.palette.colors[i][0];
                const float dg = s[0].color[1] - r.palette.colors[i][1];
                const float db = s[0].color[2] - r.palette.colors[i][2];
                const float d  = dr * dr + dg * dg + db * db;
                if (d < bestd) { bestd = d; best = i; }
            }
            CHECK(r.palette.filament[best] == states[t]);
            ++checked;
        }
        CHECK(checked > 0);
    };

    SECTION("flat, along +Z")
    {
        ImageFillParams p;
        p.asset       = quad;
        p.projection  = ImageFillProjection::Planar;
        p.axis        = ImageFillAxis::Z;
        p.subdivision = 0;
        p.allowed     = kFilamentIds;
        check_against_compute(p);
    }
    SECTION("box (tri-planar) - every one of the twelve facets, not just the top")
    {
        ImageFillParams p;
        p.asset       = quad;
        p.projection  = ImageFillProjection::Box;
        p.subdivision = 0;
        p.allowed     = kFilamentIds;
        check_against_compute(p);
    }
}

TEST_CASE("Image Fill: the segment sampler reads known colours at known positions", "[imagefill][imagerow]")
{
    ImageAssetStore store;
    const Vec3f     up(0.f, 0.f, 1.f);

    SECTION("bands3.png: three vertical bands, colour depends only on u")
    {
        const std::string sha = store.add(read_fixture("bands3.png"));
        BoundingBoxf3      box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(9, 3, 0));

        ImageFillParams p;
        p.asset      = sha;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;

        const std::vector<ImageRowSample> s =
            image_fill_sample_segment(p, box, store, Vec3f(0.f, 1.5f, 0.f), Vec3f(9.f, 1.5f, 0.f), up, 1.f);
        REQUIRE(s.size() == 10);   // length 9, spacing 1 -> 9 intervals -> 10 samples, endpoints exact
        for (const ImageRowSample &sample : s) CHECK(sample.has_color);

        const std::array<float, 3> R{1.f, 0.f, 0.f}, G{0.f, 1.f, 0.f}, B{0.f, 0.f, 1.f};
        CHECK(s[1].color == R);   // x = 1, u = 1/9  -> red band
        CHECK(s[4].color == G);   // x = 4, u = 4/9  -> green band
        CHECK(s[7].color == B);   // x = 7, u = 7/9  -> blue band
        CHECK(s[4].s == Approx(4.f));
        CHECK(s.back().pos.x() == Approx(9.f));   // the endpoint lands exactly, not one spacing short
    }

    SECTION("quad_rgbw.png: R G / B W, colour depends on both u and v")
    {
        const std::string sha = store.add(read_fixture("quad_rgbw.png"));
        BoundingBoxf3      box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(4, 4, 0));

        ImageFillParams p;
        p.asset      = sha;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;

        // y = 3 -> v = 0.75 > 0.5, the TOP row: red on the left half, green on the right.
        const std::vector<ImageRowSample> top =
            image_fill_sample_segment(p, box, store, Vec3f(0.5f, 3.f, 0.f), Vec3f(3.5f, 3.f, 0.f), up, 0.5f);
        REQUIRE(top.size() == 7);
        CHECK(top.front().color == std::array<float, 3>{1.f, 0.f, 0.f});
        CHECK(top.back().color == std::array<float, 3>{0.f, 1.f, 0.f});

        // y = 1 -> v = 0.25 < 0.5, the BOTTOM row: blue then white.
        const std::vector<ImageRowSample> bottom =
            image_fill_sample_segment(p, box, store, Vec3f(0.5f, 1.f, 0.f), Vec3f(3.5f, 1.f, 0.f), up, 0.5f);
        CHECK(bottom.front().color == std::array<float, 3>{0.f, 0.f, 1.f});
        CHECK(bottom.back().color == std::array<float, 3>{1.f, 1.f, 1.f});
    }

    SECTION("a gradient, with no image at all, samples too")
    {
        ImageFillParams p;
        p.gradient.enabled   = true;
        p.gradient.stop_a    = {0.f, 0.f, 0.f};
        p.gradient.stop_b    = {1.f, 1.f, 1.f};
        p.gradient.direction = 0;   // along u
        BoundingBoxf3 box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(10, 1, 0));

        const std::vector<ImageRowSample> s =
            image_fill_sample_segment(p, box, store, Vec3f(0.f, 0.5f, 0.f), Vec3f(10.f, 0.5f, 0.f), up, 5.f);
        REQUIRE(s.size() == 3);
        for (const ImageRowSample &sample : s) CHECK(sample.has_color);
        CHECK(s.front().color[0] == Approx(0.f).margin(1e-5f));
        CHECK(s.back().color[0] == Approx(1.f).margin(1e-5f));
    }

    SECTION("MeshUV declines: a bare position has no UV to project through")
    {
        ImageFillParams p;
        p.projection = ImageFillProjection::MeshUV;
        BoundingBoxf3 box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(1, 1, 0));
        const std::vector<ImageRowSample> s = image_fill_sample_segment(p, box, store, Vec3f(0, 0, 0), Vec3f(1, 0, 0), up, 1.f);
        for (const ImageRowSample &sample : s) CHECK_FALSE(sample.has_color);
    }

    SECTION("no image and no gradient: every sample declines rather than guessing")
    {
        ImageFillParams p;   // asset empty, gradient disabled: nothing to sample
        BoundingBoxf3    box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(1, 1, 0));
        const std::vector<ImageRowSample> s = image_fill_sample_segment(p, box, store, Vec3f(0, 0, 0), Vec3f(1, 0, 0), up, 1.f);
        for (const ImageRowSample &sample : s) CHECK_FALSE(sample.has_color);
    }

    SECTION("a zero-length segment gives exactly one sample, at s = 0")
    {
        const std::string sha = store.add(read_fixture("bands3.png"));
        BoundingBoxf3      box;
        box.merge(Vec3d(0, 0, 0));
        box.merge(Vec3d(9, 3, 0));
        ImageFillParams p;
        p.asset      = sha;
        p.projection = ImageFillProjection::Planar;
        p.axis       = ImageFillAxis::Z;
        const std::vector<ImageRowSample> s =
            image_fill_sample_segment(p, box, store, Vec3f(4.f, 1.f, 0.f), Vec3f(4.f, 1.f, 0.f), up, 1.f);
        REQUIRE(s.size() == 1);
        CHECK(s[0].s == 0.f);
        CHECK(s[0].has_color);
    }
}

// =============================================================================================
// Phase 3, step 3: the XY split with dither
// =============================================================================================
//
// image_fill_dither_segment() turns the sampler's colour ramp into filament runs by carrying the
// quantisation error forward along the path (1-D error diffusion). Nothing here touches the
// slicing pipeline - no Fill.cpp, LayerRegion.cpp or ToolOrdering.cpp change - so these are tests
// of a pure function against hand-built ImageRowSample vectors, not of a slice.

TEST_CASE("Image Fill: the XY dither turns a sampled ramp into filament runs", "[imagefill][imagerow][dither]")
{
    const std::vector<std::array<float, 3>> kDarkLight = {{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}};
    const std::vector<int>                  kDarkLightIds = {1, 2};   // 1 = dark, 2 = light

    SECTION("a solid colour gives one run")
    {
        std::vector<ImageRowSample> samples;
        for (int i = 0; i < 20; ++i) {
            ImageRowSample s;
            s.s         = float(i) * 0.4f;
            s.has_color = true;
            s.color     = {1.f, 1.f, 1.f};
            samples.push_back(s);
        }
        const auto runs = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.4f);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].filament_id == 2);
        CHECK(runs[0].s0 == Approx(0.f));
        CHECK(runs[0].s1 == Approx(19.f * 0.4f));
    }

    SECTION("two runs of the same input are identical")
    {
        std::vector<ImageRowSample> samples;
        for (int i = 0; i < 40; ++i) {
            ImageRowSample s;
            s.s         = float(i) * 0.4f;
            s.has_color = true;
            s.color     = {0.5f, 0.5f, 0.5f};   // a uniform mid-grey: dithers into more than one run
            samples.push_back(s);
        }
        const auto r1 = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.f);
        const auto r2 = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.f);
        REQUIRE(r1.size() == r2.size());
        for (size_t i = 0; i < r1.size(); ++i) {
            CHECK(r1[i].s0 == r2[i].s0);
            CHECK(r1[i].s1 == r2[i].s1);
            CHECK(r1[i].filament_id == r2[i].filament_id);
        }
        CHECK(r1.size() > 1);
    }

    SECTION("a black-to-white ramp across 8 bands: dark-run length is monotonic, no reversal")
    {
        const int   bands    = 8;
        const int   per_band = 30;
        const float ds       = 1.f;   // mm between samples

        std::vector<ImageRowSample> samples;
        samples.reserve(size_t(bands * per_band));
        // Band 0 is white (g = 1), band 7 is black (g = 0): walking the ramp start to end goes
        // white -> black, so the dark filament's run length per band must climb monotonically
        // with no reversal - "a black-to-white ramp... monotonically increasing dark-filament
        // run length... with no reversal", read in the direction that makes it literally true.
        for (int b = 0; b < bands; ++b) {
            const float g = 1.f - float(b) / float(bands - 1);
            for (int i = 0; i < per_band; ++i) {
                ImageRowSample s;
                s.s         = float(samples.size()) * ds;
                s.has_color = true;
                s.color     = {g, g, g};
                samples.push_back(s);
            }
        }

        const auto runs = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.f);

        std::vector<float> dark_len(size_t(bands), 0.f);
        const float        band_span = float(per_band) * ds;
        for (const ImageRowRun &r : runs) {
            if (r.filament_id != 1)   // 1 = dark
                continue;
            for (int b = 0; b < bands; ++b) {
                const float band_lo  = float(b) * band_span;
                const float band_hi  = band_lo + band_span;
                const float overlap  = std::min(r.s1, band_hi) - std::max(r.s0, band_lo);
                if (overlap > 0.f) dark_len[size_t(b)] += overlap;
            }
        }
        for (int b = 0; b + 1 < bands; ++b) {
            INFO("band " << b << " dark_len=" << dark_len[size_t(b)] << " vs band " << (b + 1)
                          << " dark_len=" << dark_len[size_t(b + 1)]);
            CHECK(dark_len[size_t(b)] <= dark_len[size_t(b + 1)] + 1e-3f);
        }
        CHECK(dark_len.front() < dark_len.back());
    }

    SECTION("minimum run length is honoured")
    {
        // dark [0..9], a one-sample light BLIP at 10 (shorter than the floor), dark [11..29], a
        // five-sample light BLOCK at [30..34] (as long as the floor, so it should survive), dark
        // [35..39].
        std::vector<ImageRowSample> samples;
        for (int i = 0; i < 40; ++i) {
            ImageRowSample s;
            s.s         = float(i);
            s.has_color = true;
            const bool light = (i == 10) || (i >= 30 && i <= 34);
            s.color = light ? std::array<float, 3>{1.f, 1.f, 1.f} : std::array<float, 3>{0.f, 0.f, 0.f};
            samples.push_back(s);
        }

        const auto unmerged = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.f);
        // Without a floor the blip survives as its own (zero-length) run.
        CHECK(std::any_of(unmerged.begin(), unmerged.end(),
                          [](const ImageRowRun &r) { return r.filament_id == 2 && r.length() == 0.f; }));

        const auto merged = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 2.f);
        for (const ImageRowRun &r : merged)
            CHECK((r.length() >= 2.f || merged.size() == 1));
        // The blip is gone, folded into the dark it interrupted...
        const size_t light_runs =
            std::count_if(merged.begin(), merged.end(), [](const ImageRowRun &r) { return r.filament_id == 2; });
        CHECK(light_runs == 1);
        // ...but the five-sample block, which already met the floor, survived on its own.
        auto it = std::find_if(merged.begin(), merged.end(), [](const ImageRowRun &r) { return r.filament_id == 2; });
        REQUIRE(it != merged.end());
        CHECK(it->s0 == Approx(30.f));
        CHECK(it->s1 == Approx(34.f));
    }

    SECTION("declining samples extend the previous run rather than crashing")
    {
        std::vector<ImageRowSample> samples;
        ImageRowSample a; a.s = 0.f; a.has_color = true;  a.color = {1.f, 1.f, 1.f};
        ImageRowSample b; b.s = 1.f; b.has_color = false;   // e.g. a cylindrical sample exactly on the axis
        ImageRowSample c; c.s = 2.f; c.has_color = true;  c.color = {1.f, 1.f, 1.f};
        samples = {a, b, c};
        const auto runs = image_fill_dither_segment(samples, kDarkLight, kDarkLightIds, 0.f);
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].filament_id == 2);
    }

    SECTION("empty input and mismatched candidate arrays are refused rather than crashing")
    {
        CHECK(image_fill_dither_segment({}, kDarkLight, kDarkLightIds, 0.4f).empty());
        std::vector<ImageRowSample> one(1);
        one[0].has_color = true;
        CHECK(image_fill_dither_segment(one, {}, {}, 0.4f).empty());
        CHECK(image_fill_dither_segment(one, kDarkLight, {1}, 0.4f).empty());   // size mismatch
    }
}

TEST_CASE("Image Fill: the box face a normal belongs to", "[imagefill][box]")
{
    // The predicate on its own, stated against the numbering the rest of the tests use:
    // 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z.
    CHECK(image_fill_box_face(Vec3f(1.f, 0.f, 0.f)) == 0);
    CHECK(image_fill_box_face(Vec3f(-3.f, 0.f, 0.f)) == 1);
    CHECK(image_fill_box_face(Vec3f(0.f, 2.f, 0.f)) == 2);
    CHECK(image_fill_box_face(Vec3f(0.f, -1.f, 0.f)) == 3);
    CHECK(image_fill_box_face(Vec3f(0.f, 0.f, 0.5f)) == 4);
    CHECK(image_fill_box_face(Vec3f(0.f, 0.f, -0.5f)) == 5);
    // The dominant component wins, not the first non-zero one.
    CHECK(image_fill_box_face(Vec3f(0.3f, 0.9f, 0.2f)) == 2);
    CHECK(image_fill_box_face(Vec3f(-0.6f, 0.5f, 0.4f)) == 1);
    // A tie goes to the lower axis index, so the answer is the same on every run.
    CHECK(image_fill_box_face(Vec3f(1.f, 1.f, 0.f)) == 0);
    CHECK(image_fill_box_face(Vec3f(0.f, 1.f, 1.f)) == 2);
    CHECK(image_fill_box_face(Vec3f(1.f, 0.f, 1.f)) == 0);
    // A degenerate facet belongs nowhere, and a Box projection says so rather than guessing.
    CHECK(image_fill_box_face(Vec3f(0.f, 0.f, 0.f)) == -1);
    ImageFillParams p;
    p.projection = ImageFillProjection::Box;
    BoundingBoxf3 box(Vec3d(0, 0, 0), Vec3d(10, 10, 10));
    float u = 0.f, v = 0.f;
    const Vec3f zero(0.f, 0.f, 0.f), up(0.f, 0.f, 1.f);
    CHECK_FALSE(image_fill_project(p, box, Vec3f(5.f, 5.f, 10.f), u, v, &zero));
    // ...and asking without a normal at all is refused rather than answered wrongly.
    CHECK_FALSE(image_fill_project(p, box, Vec3f(5.f, 5.f, 10.f), u, v));
    REQUIRE(image_fill_project(p, box, Vec3f(2.f, 8.f, 10.f), u, v, &up));
    CHECK(u == Approx(0.2f));   // +Z: u = x, v = y
    CHECK(v == Approx(0.8f));
}
