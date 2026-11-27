#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;

TEST_CASE("Round inward offsets preserve hole vertices when the exterior has a collinear point", "[ClipperUtils]")
{
    const Polygon outer{{0, 0}, {40000000, 0}, {40000000, 40000000}, {0, 40000000}};
    const Polygon hole{{6000000, 6000000}, {34000000, 6000000}, {34000000, 34000000}, {6000000, 34000000}};
    const float   inset         = 200000.f;
    const double  arc_tolerance = 12500.; // 0.0125 mm, as used for brim loops.
    const auto    expected      = diff_ex(offset_ex(ExPolygon(outer), -inset, jtRound, arc_tolerance),
                                          offset_ex(ExPolygon(hole), inset, jtRound, arc_tolerance));
    REQUIRE(expected.size() == 1);
    REQUIRE(expected.front().holes.size() == 1);

    ExPolygon frame(outer, hole);
    frame.holes.front().reverse();
    for (bool add_midpoint : {false, true}) {
        CAPTURE(add_midpoint);
        if (add_midpoint)
            frame.contour.points.insert(frame.contour.points.begin() + 1, Point(20000000, 0));
        CHECK(xor_ex(offset_ex(ExPolygons{frame}, -inset, jtRound, arc_tolerance), expected).empty());
        CHECK(xor_ex(offset_ex(to_polygons(frame), -inset, jtRound, arc_tolerance), expected).empty());
    }
}

TEST_CASE("Mixed flat offsets preserve winding across isolated and batched contours", "[ClipperUtils]")
{
    auto     rect = [](coord_t x, coord_t y, coord_t side) { return Polygon{{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}}; };
    Polygons flat;
    ExPolygons frames;
    for (int i = 0; i < 32; ++i) {
        const coord_t x     = i * 1200000;
        Polygon       outer = rect(x, 0, 500000);
        if (i == 0)
            outer.points.insert(outer.points.begin() + 1, Point(x + 250000, 50000));
        Polygon hole = rect(x + 100000, 100000, 300000);
        hole.reverse();
        flat.push_back(outer);
        flat.push_back(hole);
        frames.emplace_back(outer, hole);
    }
    const float delta    = 50000.f;
    const auto  expected = offset_ex(frames, delta, jtMiter, 3.);
    REQUIRE(expected.size() == frames.size());
    CHECK(xor_ex(offset_ex(flat, delta, jtMiter, 3.), expected).empty());
    // The duplicate contour contributes +2 outside the hole and +1 inside it.
    // Unioning same-sign overlaps before combining the hole would erase that distinction.
    flat.push_back(flat[2]);
    frames[1].holes.clear();
    CHECK(xor_ex(offset_ex(flat, delta, jtMiter, 3.), offset_ex(frames, delta, jtMiter, 3.)).empty());
    std::reverse(flat.begin(), flat.end());
    CHECK(xor_ex(offset_ex(flat, delta, jtMiter, 3.), offset_ex(frames, delta, jtMiter, 3.)).empty());
}

TEST_CASE("Convex inset batches match isolated offsets across corner eligibility thresholds", "[ClipperUtils]")
{
    const Polygon neighbor{{-500000, 1}, {1000, 1}, {1000, 1100000}, {-500000, 1100000}};
    const Polygon frame{{-500000, -500000}, {1500000, -500000}, {1500000, 1500000}, {-500000, 1500000}};
    auto          check_offsets = [&](const Polygon& contour, float inset) {
        const ExPolygons isolated = offset_ex(ExPolygon(contour), -inset, jtMiter, 3.);
        ExPolygons       pieces   = offset_ex(ExPolygon(neighbor), -inset, jtMiter, 3.);
        pieces.insert(pieces.end(), isolated.begin(), isolated.end());
        const ExPolygons expected = union_ex(pieces);
        CHECK(xor_ex(offset_ex(Polygons{contour, neighbor}, -inset, jtMiter, 3.), expected).empty());

        Polygon hole = contour;
        hole.reverse();
        const ExPolygons cut = diff_ex(offset_ex(ExPolygon(frame), inset, jtMiter, 3.), isolated);
        CHECK(xor_ex(offset_ex(ExPolygon(frame, hole), inset, jtMiter, 3.), cut).empty());
        CHECK(xor_ex(offset_ex(Polygons{frame, hole}, inset, jtMiter, 3.), cut).empty());
    };

    SECTION("Acute triangle tip crosses each cosine limit")
    {
        // At the tip, cos(turn) = (h*h - L*L)/(h*h + L*L). The adjacent integer
        // half-heights straddle -0.999 and -0.998 respectively for L = 1,000,000.
        for (coord_t h : {22366, 22367, 31638, 31639}) {
            CAPTURE(h);
            check_offsets(Polygon{{0, -h}, {1000000, 0}, {0, h}}, 500.f);
        }
    }

    SECTION("A short rectangle edge crosses each squared-length limit")
    {
        // For a right angle x = |delta|, so the two critical widths are 200,000 and 202,484.567...
        for (coord_t width : {199999, 200000, 200001, 202484, 202485}) {
            CAPTURE(width);
            check_offsets(Polygon{{0, 0}, {width, 0}, {width, 1000000}, {0, 1000000}}, 100000.f);
        }
    }
}

static Polygons diagonal_strips(size_t count)
{
    Polygons      strips;
    const coord_t length = coord_t(count) * 2000;
    for (size_t i = 0; i < count; ++i) {
        const coord_t x = coord_t(i) * 2000;
        strips.emplace_back(Points{{x, 0}, {x + length, length}, {x + length - 800, length + 800}, {x - 800, 800}});
    }
    return strips;
}

TEST_CASE("Ordered diagonal insets retain the single-path geometry in either direction", "[ClipperUtils]")
{
    Polygons strips = diagonal_strips(96);
    for (bool overlap : {false, true}) {
        CAPTURE(overlap);
        if (overlap)
            strips.back() = strips[strips.size() - 2];
        Polygons isolated;
        for (const Polygon& strip : strips)
            polygons_append(isolated, offset(strip, -50.f, jtMiter, 3.));
        const ExPolygons expected = union_ex(isolated);
        REQUIRE(expected.size() == strips.size() - size_t(overlap));
        for (bool reverse : {false, true}) {
            CAPTURE(reverse);
            if (reverse)
                std::reverse(strips.begin(), strips.end());
            CHECK(xor_ex(offset_ex(strips, -50.f, jtMiter, 3.), expected).empty());
            CHECK(xor_ex(union_ex(offset(strips, -50.f, jtMiter, 3.)), expected).empty());
        }
    }
}

TEST_CASE("Mixed flat offset and ordered strip performance", "[.mr13_bench]")
{
    auto median_ms = [](auto run) {
        std::array<double, 4> times;
        for (double& ms : times) {
            const auto start = std::chrono::steady_clock::now();
            run();
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        std::sort(times.begin() + 1, times.end()); // Exclude warm-up.
        return times[2];
    };
    Polygons flat;
    for (int y = 0; y < 150; ++y)
        for (int x = 0; x < 150; ++x) {
            const coord_t a = 1200000 * x, b = 1200000 * y;
            flat.emplace_back(Points{{a, b}, {a + 500000, b}, {a + 500000, b + 500000}, {a, b + 500000}});
            flat.emplace_back(
                Points{{a + 100000, b + 100000}, {a + 100000, b + 400000}, {a + 400000, b + 400000}, {a + 400000, b + 100000}});
        }
    flat.front().points.insert(flat.front().points.begin() + 1, Point(250000, 0));
    for (coord_t notch : {0, 50000}) {
        flat.front().points[1].y() = notch;
        ExPolygons   result;
        const double ms = median_ms([&] { result = offset_ex(flat, 50000.f, jtMiter, 3.); });
        REQUIRE(result.size() == 22500);
        CHECK(std::all_of(result.begin(), result.end(), [](const ExPolygon& e) { return e.holes.size() == 1; }));
        std::cout << "[MR13_BENCH] mixed_flat_22500 notch=" << notch << " median_ms=" << ms << '\n';
    }
    const Polygons strips = diagonal_strips(4000);
    Polygons       result;
    const double   ms = median_ms([&] { result = offset(strips, -50.f, jtMiter, 3.); });
    CHECK(result.size() == strips.size());
    std::cout << "[MR13_BENCH] diagonal_insets_4000 median_ms=" << ms << '\n';
}

// #define TESTS_EXPORT_SVGS

SCENARIO("Constant offset", "[ClipperUtils]") {
	coord_t s = 1000000;
	GIVEN("20mm box") {
		ExPolygon box20mm;
		box20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 20 * s, 20 * s}, Vec2crd{ 0, 20 * s} };
		std::vector<float> deltas_plus(box20mm.contour.points.size(), 1. * s);
		std::vector<float> deltas_minus(box20mm.contour.points.size(), - 1. * s);
		Polygons output;
		WHEN("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("plus 1mm, miter " << miter << "x") {
					output = Slic3r::offset(box20mm, 1. * s, jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_box20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 22^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(22. * 22. * s * s, 0.001));
					}
				}
				DYNAMIC_SECTION("minus 1mm, miter " << miter << "x") {
					output = Slic3r::offset(box20mm, - 1. * s, jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_box20mm_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 18^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(18. * 18. * s * s, 0.001));
					}
				}
			}
		}
		WHEN("Slic3r::variable_offset_outer/inner") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("plus 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_outer(box20mm, { deltas_plus }, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_box20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 22^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(22. * 22. * s * s, 0.001));
					}
				}
				DYNAMIC_SECTION("minus 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_inner(box20mm, { deltas_minus }, miter);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_box20mm_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(box20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area is 18^2mm2") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(18. * 18. * s * s, 0.001));
					}
				}
			}
		}
	}

	GIVEN("20mm box with 10mm hole") {
		ExPolygon box20mm;
		box20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 20 * s, 20 * s}, Vec2crd{ 0, 20 * s} };
		box20mm.holes.emplace_back(Slic3r::Polygon({ Vec2crd{ 5 * s, 5 * s }, Vec2crd{ 5 * s, 15 * s}, Vec2crd{ 15 * s, 15 * s}, Vec2crd{ 15 * s, 5 * s } }));
		std::vector<float> deltas_plus(box20mm.contour.points.size(), 1. * s);
		std::vector<float> deltas_minus(box20mm.contour.points.size(), -1. * s);
		ExPolygons output;
		SECTION("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("miter " << miter << "x") {
					WHEN("plus 1mm") {
						output = Slic3r::offset_ex(box20mm, 1. * s, jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("constant_offset_box20mm_10mm_hole_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 22^2-8^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE_THAT(output.front().area(), WithinRel((22. * 22. - 8. * 8.) * s * s, 0.001));
						}
					}
					WHEN("minus 1mm") {
						output = Slic3r::offset_ex(box20mm, - 1. * s, jtMiter, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("constant_offset_box20mm_10mm_hole_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 18^2-12^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE_THAT(output.front().area(), WithinRel((18. * 18. - 12. * 12.) * s * s, 0.001));
						}
					}
				}
			}
		}
		SECTION("Slic3r::variable_offset_outer()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("miter " << miter << "x") {
					WHEN("plus 1mm") {
						output = Slic3r::variable_offset_outer_ex(box20mm, { deltas_plus, deltas_plus }, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("variable_offset_box20mm_10mm_hole_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 22^2-8^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE_THAT(output.front().area(), WithinRel((22. * 22. - 8. * 8.) * s * s, 0.001));
						}
					}
					WHEN("minus 1mm") {
						output = Slic3r::variable_offset_inner_ex(box20mm, { deltas_minus, deltas_minus }, miter);
#ifdef TESTS_EXPORT_SVGS
						{
							SVG svg(debug_out_path("variable_offset_box20mm_10mm_hole_minus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
							svg.draw(box20mm, "blue");
							svg.draw_outline(to_polygons(output), "black", coord_t(scale_(0.01)));
						}
#endif
						THEN("Area is 18^2-12^2 mm2") {
							REQUIRE(output.size() == 1);
							REQUIRE_THAT(output.front().area(), WithinRel((18. * 18. - 12. * 12.) * s * s, 0.001));
						}
					}
				}
			}
		}
	}

	GIVEN("20mm right angle triangle") {
		ExPolygon triangle20mm;
		triangle20mm.contour.points = { Vec2crd{ 0, 0 }, Vec2crd{ 20 * s, 0 }, Vec2crd{ 0, 20 * s } };
		Polygons output;
		double offset = 1.;
		// Angle of the sharp corner bisector.
		double angle_bisector = M_PI / 8.;
		// Area tapered by mitering one sharp corner.
		double area_tapered = pow(offset * (1. / sin(angle_bisector) - 1.), 2.) * tan(angle_bisector);
		double l_triangle_side_offsetted = 20. + offset * (1. + 1. / tan(angle_bisector));
		double area_offsetted = (0.5 * l_triangle_side_offsetted * l_triangle_side_offsetted - 2. * area_tapered) * s * s;
		SECTION("Slic3r::offset()") {
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("Outer offset 1mm, miter " << miter << "x") {
					output = Slic3r::offset(triangle20mm, offset * s, jtMiter, 2.0);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("constant_offset_triangle20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(triangle20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area matches") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(area_offsetted, 0.001));
					}
				}
			}
		}
		SECTION("Slic3r::variable_offset_outer()") {
			std::vector<float> deltas(triangle20mm.contour.points.size(), 1. * s);
			for (double miter : { 2.0, 1.5, 1.2 }) {
				DYNAMIC_SECTION("Outer offset 1mm, miter " << miter << "x") {
					output = Slic3r::variable_offset_outer(triangle20mm, { deltas }, 2.0);
#ifdef TESTS_EXPORT_SVGS
					{
						SVG svg(debug_out_path("variable_offset_triangle20mm_plus1mm_miter%lf.svg", miter).c_str(), get_extents(output));
						svg.draw(triangle20mm, "blue");
						svg.draw_outline(output, "black", coord_t(scale_(0.01)));
					}
#endif
					THEN("Area matches") {
						REQUIRE(output.size() == 1);
						REQUIRE_THAT(output.front().area(), WithinRel(area_offsetted, 0.001));
					}
				}
			}
		}
	}
}
