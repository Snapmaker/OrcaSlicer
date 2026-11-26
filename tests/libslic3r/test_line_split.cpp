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
    auto run_pg_flow = [](PerimeterGeneratorType wall_gen, const ExPolygons &top_slice, const ExPolygons &lower_slice,
                          const Flow &flow, std::vector<const ExtrusionPath *> &paths_out,
                          ExtrusionEntityCollection &flattened_storage) {
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
    auto run_pg = [&](PerimeterGeneratorType wall_gen, const ExPolygons &top_slice, const ExPolygons &lower_slice,
                      std::vector<const ExtrusionPath *> &paths_out, ExtrusionEntityCollection &flattened_storage) {
        run_pg_flow(wall_gen, top_slice, lower_slice, Flow(0.42f, 0.2f, 0.4f), paths_out, flattened_storage);
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

        // Exact Flow(0.4, 0.2, 0.4) sensitivity check: lower y=0.2 -> ~8/18, lower y=0.200001 -> 0/26.
        {
            std::vector<const ExtrusionPath *> p0, p1;
            ExtrusionEntityCollection          s0, s1;
            const ExPolygons upper_repro = make_box_ex(0.8, -5.2, 9.2, 0.2);
            run_pg_flow(PerimeterGeneratorType::Arachne, upper_repro, make_box_ex(0., 0.2, 10., 10.),
                        Flow(0.4f, 0.2f, 0.4f), p0, s0);
            run_pg_flow(PerimeterGeneratorType::Arachne, upper_repro, make_box_ex(0., 0.200001, 10., 10.),
                        Flow(0.4f, 0.2f, 0.4f), p1, s1);
            double sup0 = 0., ovh0 = 0., sup1 = 0., ovh1 = 0.;
            for (const ExtrusionPath *p : p0)
                (p->role() == erOverhangPerimeter ? ovh0 : sup0) += p->length();
            for (const ExtrusionPath *p : p1)
                (p->role() == erOverhangPerimeter ? ovh1 : sup1) += p->length();
            CHECK(sup0 == Catch::Approx(double(scale_(8.))).margin(double(scale_(0.05))));
            CHECK(ovh0 == Catch::Approx(double(scale_(18.))).margin(double(scale_(0.05))));
            CHECK(sup1 == Catch::Approx(0.));
            CHECK(ovh1 == Catch::Approx(double(scale_(26.))).margin(double(scale_(0.05))));
        }
    }

    SECTION("Classic and Arachne grazing edge cases over corners, left edge, and 36.87 deg rotation")
    {
        const Flow flow04(0.4f, 0.2f, 0.4f);
        auto sum_roles = [](const std::vector<const ExtrusionPath *> &paths, double &supported_len, double &overhang_len) {
            supported_len = 0.;
            overhang_len  = 0.;
            for (const ExtrusionPath *p : paths) {
                CHECK(p->width > 0.f);
                if (p->role() == erOverhangPerimeter)
                    overhang_len += p->length();
                else
                    supported_len += p->length();
            }
        };

        // Case 2: Classic right edge passing support top corner -> 5.2mm supported along x=10.2, 20.8mm overhang.
        {
            std::vector<const ExtrusionPath *> paths;
            ExtrusionEntityCollection          storage;
            run_pg_flow(PerimeterGeneratorType::Classic, make_box_ex(10., 4.8, 17.2, 11.4),
                        make_box_ex(0., 0., 10., 10.), flow04, paths, storage);
            REQUIRE(! paths.empty());
            double sup = 0., ovh = 0.;
            sum_roles(paths, sup, ovh);
            CHECK(sup == Catch::Approx(double(scale_(5.2))));
            CHECK(ovh == Catch::Approx(double(scale_(20.8))));

            BoundingBox sup_bb;
            for (const ExtrusionPath *p : paths)
                if (p->role() != erOverhangPerimeter)
                    for (const Point &pt : p->polyline.points)
                        sup_bb.merge(pt);
            CHECK(sup_bb.min.x() == coord_t(scale_(10.2)));
            CHECK(sup_bb.max.x() == coord_t(scale_(10.2)));
            CHECK(sup_bb.min.y() == coord_t(scale_(5.0)));
            CHECK(sup_bb.max.y() == coord_t(scale_(10.2)));
        }

        // Case 4: Classic left edge spanning support top and bottom corners -> 0mm supported, 55.6mm overhang.
        {
            std::vector<const ExtrusionPath *> paths;
            ExtrusionEntityCollection          storage;
            run_pg_flow(PerimeterGeneratorType::Classic, make_box_ex(-8.2, -5.2, 0., 15.2),
                        make_box_ex(0., 0., 10., 10.), flow04, paths, storage);
            REQUIRE(! paths.empty());
            double sup = 0., ovh = 0.;
            sum_roles(paths, sup, ovh);
            CHECK(sup == Catch::Approx(0.));
            CHECK(ovh == Catch::Approx(double(scale_(55.6))));
        }

        // Case 3: Attachment geometry rotated by +36.87 deg and -36.87 deg via exact (0.8x -/+ 0.6y, +/-0.6x + 0.8y).
        {
            auto make_rot_box = [](double x0, double y0, double x1, double y1, double s) -> ExPolygons {
                const std::array<Vec2d, 4> corners{ Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1) };
                Polygon poly;
                for (const Vec2d &c : corners) {
                    const double rx = 0.8 * c.x() - s * c.y();
                    const double ry = s * c.x() + 0.8 * c.y();
                    poly.points.emplace_back(coord_t(std::llround(scale_(rx))), coord_t(std::llround(scale_(ry))));
                }
                return { ExPolygon(std::move(poly)) };
            };
            const ExPolygons upper_rot    = make_rot_box(0.8, -5.2, 9.2, 0.2, 0.6);
            const ExPolygons lower_rot    = make_rot_box(0., 0.2, 10., 10., 0.6);
            const ExPolygons lower_rot_in = make_rot_box(0., 0.199999, 10., 10., 0.6);

            std::vector<const ExtrusionPath *> paths_c, paths_a, paths_a_in;
            ExtrusionEntityCollection          st_c, st_a, st_a_in;
            run_pg_flow(PerimeterGeneratorType::Classic, upper_rot, lower_rot, flow04, paths_c, st_c);
            double sup_c = 0., ovh_c = 0.;
            sum_roles(paths_c, sup_c, ovh_c);
            CHECK(sup_c == Catch::Approx(0.));
            CHECK(ovh_c == Catch::Approx(double(scale_(26.))));

            // Under Clipper2's OffsetConcaveCrossing, Arachne's rotated +36.87 deg centerline lands exactly
            // on the NonHorizRight boundary 3x - 4y = 0 (perimeter 26.000000mm), which Clipper1 open-path
            // semantics classify as 0 / 26; shifting lower_rot 1nm inward (y = 0.199999) flips to 8 / 18.
            run_pg_flow(PerimeterGeneratorType::Arachne, upper_rot, lower_rot, flow04, paths_a, st_a);
            double sup_a = 0., ovh_a = 0.;
            sum_roles(paths_a, sup_a, ovh_a);
            CHECK(sup_a == Catch::Approx(0.));
            CHECK(ovh_a == Catch::Approx(double(scale_(26.))));

            run_pg_flow(PerimeterGeneratorType::Arachne, upper_rot, lower_rot_in, flow04, paths_a_in, st_a_in);
            double sup_a_in = 0., ovh_a_in = 0.;
            sum_roles(paths_a_in, sup_a_in, ovh_a_in);
            CHECK(sup_a_in == Catch::Approx(double(scale_(8.))).margin(double(scale_(0.05))));
            CHECK(ovh_a_in == Catch::Approx(double(scale_(18.))).margin(double(scale_(0.05))));

            // Under -36.87 deg rotation, the shared boundary is NonHorizLeft (included by Clipper1 -> 8 / 18).
            const ExPolygons upper_neg = make_rot_box(0.8, -5.2, 9.2, 0.2, -0.6);
            const ExPolygons lower_neg = make_rot_box(0., 0.2, 10., 10., -0.6);
            std::vector<const ExtrusionPath *> paths_cn, paths_an;
            ExtrusionEntityCollection          st_cn, st_an;
            run_pg_flow(PerimeterGeneratorType::Classic, upper_neg, lower_neg, flow04, paths_cn, st_cn);
            double sup_cn = 0., ovh_cn = 0.;
            sum_roles(paths_cn, sup_cn, ovh_cn);
            CHECK(sup_cn == Catch::Approx(double(scale_(8.))));
            CHECK(ovh_cn == Catch::Approx(double(scale_(18.))));

            run_pg_flow(PerimeterGeneratorType::Arachne, upper_neg, lower_neg, flow04, paths_an, st_an);
            double sup_an = 0., ovh_an = 0.;
            sum_roles(paths_an, sup_an, ovh_an);
            CHECK(sup_an == Catch::Approx(double(scale_(8.))).margin(double(scale_(0.05))));
            CHECK(ovh_an == Catch::Approx(double(scale_(18.))).margin(double(scale_(0.05))));
        }
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

        // Thin tapered wedge (height 0.80mm at x=0 down to 0.40mm at x=30) forces a single merged
        // variable-width Arachne wall whose width tapers smoothly across the clip edge at x=15.
        const ExPolygons thin_wedge{ ExPolygon(Polygon{
            { coord_t(scale_(0.)), coord_t(scale_(4.6)) },
            { coord_t(scale_(30.)), coord_t(scale_(4.8)) },
            { coord_t(scale_(30.)), coord_t(scale_(5.2)) },
            { coord_t(scale_(0.)), coord_t(scale_(5.4)) } }) };
        const ExPolygons mid_clip = make_box_ex(0., 0., 15., 10.);
        std::vector<const ExtrusionPath *> tw_paths;
        ExtrusionEntityCollection          tw_storage;
        run_pg(PerimeterGeneratorType::Arachne, thin_wedge, mid_clip, tw_paths, tw_storage);
        REQUIRE(! tw_paths.empty());
        float  tw_min_w = 1e9f, tw_max_w = 0.f;
        double tw_sup = 0., tw_ovh = 0.;
        for (const ExtrusionPath *p : tw_paths) {
            CHECK(p->width > 0.f);
            CHECK(p->width <= 1.2f);
            tw_min_w = std::min(tw_min_w, p->width);
            tw_max_w = std::max(tw_max_w, p->width);
            if (p->role() == erOverhangPerimeter)
                tw_ovh += p->length();
            else
                tw_sup += p->length();
        }
        CHECK(tw_sup > 0.);
        CHECK(tw_ovh > 0.);
        CHECK(tw_max_w - tw_min_w > 0.1f);
    }
}
