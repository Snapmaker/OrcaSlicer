#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Algorithm/LineSplit.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/SurfaceCollection.hpp"

using namespace Slic3r;

namespace {

ExPolygons make_box_ex(double x0, double y0, double x1, double y1)
{
    return { ExPolygon(Polygon{
        { scale_(x0), scale_(y0) },
        { scale_(x1), scale_(y0) },
        { scale_(x1), scale_(y1) },
        { scale_(x0), scale_(y1) } }) };
}

void collect_paths(const ExtrusionEntityCollection &coll, std::vector<const ExtrusionPath *> &out)
{
    for (const ExtrusionEntity *e : coll.entities) {
        if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            out.push_back(p);
        } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e)) {
            for (const ExtrusionPath &sub : mp->paths)
                out.push_back(&sub);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(e)) {
            for (const ExtrusionPath &sub : loop->paths)
                out.push_back(&sub);
        }
    }
}

} // namespace

TEST_CASE("Algorithm::split_line handles Inclusive boundary edges and zero-hit inputs", "[LineSplit]")
{
    const ExPolygons clip = make_box_ex(0., 0., 10., 10.);

    SECTION("Disjoint path returns empty")
    {
        const Points path{ { scale_(20.), scale_(0.) }, { scale_(30.), scale_(10.) } };
        CHECK(Algorithm::split_line(path, clip, false).empty());
        CHECK(Algorithm::split_line(path, clip, true).empty());
    }

    SECTION("Open path along top, bottom, and side boundaries uses Inclusive rule")
    {
        for (double y : { 0., 10. }) {
            const Points path{ { scale_(-2.), scale_(y) }, { scale_(12.), scale_(y) } };
            const Algorithm::SplittedLine res = Algorithm::split_line(path, clip, false);
            REQUIRE(res.size() >= 4);
            double clipped_len = 0.;
            for (size_t i = 0; i + 1 < res.size(); ++i) {
                CHECK(res[i].p != res[i + 1].p);
                if (res[i].clipped)
                    clipped_len += (res[i + 1].p - res[i].p).cast<double>().norm();
            }
            CHECK(clipped_len == Catch::Approx(double(scale_(10.))));
        }

        for (double x : { 0., 10. }) {
            const Points path{ { scale_(x), scale_(-2.) }, { scale_(x), scale_(12.) } };
            const Algorithm::SplittedLine res = Algorithm::split_line(path, clip, false);
            REQUIRE(res.size() >= 4);
            double clipped_len = 0.;
            for (size_t i = 0; i + 1 < res.size(); ++i) {
                CHECK(res[i].p != res[i + 1].p);
                if (res[i].clipped)
                    clipped_len += (res[i + 1].p - res[i].p).cast<double>().norm();
            }
            CHECK(clipped_len == Catch::Approx(double(scale_(10.))));
        }
    }

    SECTION("Closed loop straddling clip rectangle produces valid wrap-around junctions without duplicate points")
    {
        const Points loop{
            { scale_(5.), scale_(-2.) },
            { scale_(15.), scale_(-2.) },
            { scale_(15.), scale_(10.) },
            { scale_(5.), scale_(10.) }
        };
        const Algorithm::SplittedLine res = Algorithm::split_line(loop, clip, true);
        REQUIRE(! res.empty());
        double clipped_len = 0.;
        for (size_t i = 0; i < res.size(); ++i) {
            const auto &a = res[i];
            const auto &b = res[(i + 1) % res.size()];
            CHECK(a.p != b.p);
            if (a.clipped)
                clipped_len += (b.p - a.p).cast<double>().norm();
        }
        // Inside/inclusive boundary spans: bottom/top overlaps on x in [5, 10] -> top length 5, vertical leg at x=5 -> length 10.
        CHECK(clipped_len == Catch::Approx(double(scale_(15.))));
    }
}

TEST_CASE("FuzzySkin::apply_fuzzy_skin multi-region split across top boundary", "[FuzzySkin][LineSplit]")
{
    SurfaceCollection          slices;
    LayerRegionPtrs            compatible_regions;
    PrintRegionConfig          region_cfg;
    PrintObjectConfig          object_cfg;
    PrintConfig                print_cfg;
    ExtrusionEntityCollection  loops, gap_fill;
    SurfaceCollection          fill_surfaces;
    ExPolygons                 fill_no_overlap;

    Flow flow(0.42f, 0.2f, 0.4f);
    PerimeterGenerator pg(
        &slices, &compatible_regions, 0.2, 0.1, flow,
        &region_cfg, &object_cfg, &print_cfg, false,
        &loops, &gap_fill, &fill_surfaces, &fill_no_overlap);
    pg.layer_id = 2;

    FuzzySkinConfig cfg1{};
    cfg1.type              = FuzzySkinType::External;
    cfg1.thickness         = scale_(0.3);
    cfg1.point_distance    = scale_(0.8);
    cfg1.fuzzy_first_layer = true;
    cfg1.noise_type        = NoiseType::Classic;
    cfg1.noise_scale       = 1.0;
    cfg1.noise_octaves     = 1;
    cfg1.noise_persistence = 0.5;
    cfg1.mode              = FuzzySkinMode::Displacement;

    FuzzySkinConfig cfg2 = cfg1;
    cfg2.type            = FuzzySkinType::AllWalls;
    cfg2.thickness       = scale_(0.5);

    pg.regions_by_fuzzify.emplace(cfg1, make_box_ex(5., 0., 15., 10.));
    pg.regions_by_fuzzify.emplace(cfg2, make_box_ex(100., 100., 110., 110.));

    Arachne::ExtrusionLine line(0, false);
    line.is_closed = false;
    line.junctions.emplace_back(Point(scale_(0.), scale_(10.)), scale_(0.42), 0);
    line.junctions.emplace_back(Point(scale_(10.), scale_(10.)), scale_(0.42), 1);

    Feature::FuzzySkin::apply_fuzzy_skin(&line, pg, true);
    REQUIRE(line.junctions.size() > 2);
    CHECK(line.junctions.front().p == Point(scale_(0.), scale_(10.)));
    CHECK(line.junctions.back().p == Point(scale_(10.), scale_(10.)));
}

TEST_CASE("PerimeterGenerator classic and arachne overhang partitioning with exact Z interpolation", "[PerimeterGenerator]")
{
    auto run_pg = [](PerimeterGeneratorType wall_gen, const ExPolygons &top_slice, const ExPolygons &lower_slice,
                     std::vector<const ExtrusionPath *> &paths_out, ExtrusionEntityCollection &flattened_storage) {
        SurfaceCollection          slices;
        slices.append(top_slice, stInternal);
        LayerRegionPtrs            compatible_regions;
        PrintRegionConfig          region_cfg;
        region_cfg.wall_loops.value           = 1;
        region_cfg.detect_overhang_wall.value = true;
        PrintObjectConfig          object_cfg;
        object_cfg.wall_generator.value = wall_gen;
        object_cfg.raft_layers.value    = 0;
        PrintConfig                print_cfg;
        ExtrusionEntityCollection  loops, gap_fill;
        SurfaceCollection          fill_surfaces;
        ExPolygons                 fill_no_overlap;

        Flow flow(0.42f, 0.2f, 0.4f);
        PerimeterGenerator pg(
            &slices, &compatible_regions, 0.2, 0.3, flow,
            &region_cfg, &object_cfg, &print_cfg, false,
            &loops, &gap_fill, &fill_surfaces, &fill_no_overlap);
        pg.layer_id           = 1;
        pg.lower_slices       = &lower_slice;
        pg.ext_perimeter_flow = flow;
        pg.overhang_flow      = Flow::bridging_flow(0.4f, 0.4f);
        pg.solid_infill_flow  = flow;

        if (wall_gen == PerimeterGeneratorType::Arachne)
            pg.process_arachne();
        else
            pg.process_classic();

        flattened_storage = loops.flatten();
        paths_out.clear();
        collect_paths(flattened_storage, paths_out);
    };

    // Upper slice [0..30, 0..10] supported on [0..10, 0..10] -> must emit both supported perimeter and overhang perimeter.
    const ExPolygons upper = make_box_ex(0., 0., 30., 10.);
    const ExPolygons lower = make_box_ex(0., 0., 10., 10.);

    SECTION("Classic single-wall overhang produces non-empty supported and overhang segments summing to perimeter")
    {
        std::vector<const ExtrusionPath *> paths;
        ExtrusionEntityCollection          storage;
        run_pg(PerimeterGeneratorType::Classic, upper, lower, paths, storage);
        REQUIRE(! paths.empty());

        double supported_len = 0., overhang_len = 0.;
        for (const ExtrusionPath *p : paths) {
            CHECK(p->width > 0.f);
            if (p->role() == erOverhangPerimeter)
                overhang_len += p->length();
            else
                supported_len += p->length();
        }
        CHECK(supported_len > scale_(10.));
        CHECK(overhang_len > scale_(20.));
    }

    SECTION("Classic single-wall grazing overhang bottom edge emits 0mm supported and 26mm overhang")
    {
        // Repro: single wall 0.4mm width, 0.2mm height, upper [0.8,9.2]x[-5.2,0.2], lower [0,10]x[0.2,10].
        // Perimeter centerline runs at [1,9]x[-5,0] while grown_lower_slices reaches [0..10]x[0..10.2].
        SurfaceCollection          slices;
        slices.append(make_box_ex(0.8, -5.2, 9.2, 0.2), stInternal);
        LayerRegionPtrs            compatible_regions;
        PrintRegionConfig          region_cfg;
        region_cfg.wall_loops.value           = 1;
        region_cfg.detect_overhang_wall.value = true;
        PrintObjectConfig          object_cfg;
        object_cfg.wall_generator.value = PerimeterGeneratorType::Classic;
        object_cfg.raft_layers.value    = 0;
        PrintConfig                print_cfg;
        ExtrusionEntityCollection  loops, gap_fill;
        SurfaceCollection          fill_surfaces;
        ExPolygons                 fill_no_overlap;

        Flow flow(0.4f, 0.2f, 0.4f);
        PerimeterGenerator pg(
            &slices, &compatible_regions, 0.2, 0.3, flow,
            &region_cfg, &object_cfg, &print_cfg, false,
            &loops, &gap_fill, &fill_surfaces, &fill_no_overlap);
        const ExPolygons lower_repro = make_box_ex(0., 0.2, 10., 10.);
        pg.layer_id                  = 1;
        pg.lower_slices              = &lower_repro;
        pg.ext_perimeter_flow        = flow;
        pg.overhang_flow             = Flow::bridging_flow(0.4f, 0.4f);
        pg.solid_infill_flow         = flow;
        pg.process_classic();

        const ExtrusionEntityCollection flat = loops.flatten();
        std::vector<const ExtrusionPath *> paths;
        collect_paths(flat, paths);
        REQUIRE(! paths.empty());
        double supported_len = 0., overhang_len = 0.;
        for (const ExtrusionPath *p : paths) {
            if (p->role() == erOverhangPerimeter)
                overhang_len += p->length();
            else
                supported_len += p->length();
        }
        CHECK(supported_len == Catch::Approx(0.));
        CHECK(overhang_len == Catch::Approx(double(scale_(26.))));
    }

    SECTION("Arachne single-wall grazing overhang bottom edge emits 0mm supported and 26mm overhang")
    {
        std::vector<const ExtrusionPath *> paths;
        ExtrusionEntityCollection          storage;
        run_pg(PerimeterGeneratorType::Arachne, make_box_ex(0.8, -5.2, 9.2, 0.2), make_box_ex(0., 0.2, 10., 10.), paths, storage);
        REQUIRE(! paths.empty());

        double supported_len = 0., overhang_len = 0.;
        for (const ExtrusionPath *p : paths) {
            CHECK(p->width > 0.f);
            if (p->role() == erOverhangPerimeter)
                overhang_len += p->length();
            else
                supported_len += p->length();
        }
        CHECK(supported_len == Catch::Approx(0.));
        CHECK(overhang_len == Catch::Approx(double(scale_(26.))).margin(double(scale_(0.2))));
    }

    SECTION("Arachne overhang on variable-width wedge crossing clip vertex keeps interpolated positive width")
    {
        // Tapered wedge polygon whose variable-width centerline crosses the clip corner at x=10.
        const ExPolygons wedge{ ExPolygon(Polygon{
            { coord_t(scale_(0.)), coord_t(scale_(0.)) },
            { coord_t(scale_(30.)), coord_t(scale_(4.5)) },
            { coord_t(scale_(30.)), coord_t(scale_(5.5)) },
            { coord_t(scale_(0.)), coord_t(scale_(10.)) } }) };
        const ExPolygons corner_clip = make_box_ex(0., 0., 10., 5.);

        std::vector<const ExtrusionPath *> paths;
        ExtrusionEntityCollection          storage;
        run_pg(PerimeterGeneratorType::Arachne, wedge, corner_clip, paths, storage);
        REQUIRE(! paths.empty());

        double supported_len = 0., overhang_len = 0.;
        for (const ExtrusionPath *p : paths) {
            CHECK(p->width > 0.f);
            CHECK(p->width <= 1.2f);
            if (p->role() == erOverhangPerimeter)
                overhang_len += p->length();
            else
                supported_len += p->length();
        }
        CHECK(supported_len > 0.);
        CHECK(overhang_len > 0.);
    }
}
