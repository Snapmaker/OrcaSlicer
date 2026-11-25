#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <numeric>
#include <set>
#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperZUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinRel;

// Clipper may start an output polygon at any of its vertices.
static Polygon start_at_min(Polygon polygon)
{
    std::rotate(polygon.points.begin(), std::min_element(polygon.points.begin(), polygon.points.end()), polygon.points.end());
    return polygon;
}
static Polygons start_at_min(Polygons polygons)
{
    for (Polygon &polygon : polygons)
        polygon = start_at_min(std::move(polygon));
    return polygons;
}
static ExPolygons start_at_min(ExPolygons expolygons)
{
    for (ExPolygon &expolygon : expolygons) {
        expolygon.contour = start_at_min(std::move(expolygon.contour));
        expolygon.holes   = start_at_min(std::move(expolygon.holes));
    }
    return expolygons;
}

SCENARIO("Various Clipper operations - xs/t/11_clipper.t", "[ClipperUtils]") {
	// CCW oriented contour
	Slic3r::Polygon   square{ { 200, 100 }, {200, 200}, {100, 200}, {100, 100} };
	// CW oriented contour
	Slic3r::Polygon   hole_in_square{ { 160, 140 }, { 140, 140 }, { 140, 160 }, { 160, 160 } };
	Slic3r::ExPolygon square_with_hole(square, hole_in_square);
	GIVEN("square_with_hole") {
        WHEN("offset") {
            Polygons result = Slic3r::offset(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(Polygons {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 155, 145 }, { 145, 145 }, { 145, 155 }, { 155, 155 } } }));
            }
        }
        WHEN("offset_ex") {
            ExPolygons result = Slic3r::offset_ex(square_with_hole, 5.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(ExPolygons { {
                    { { 205, 205 }, { 95, 205 }, { 95, 95 }, { 205, 95 }, },
                    { { 145, 145 }, { 145, 155 }, { 155, 155 }, { 155, 145 } } } }));
            }
        }
        WHEN("offset2_ex") {
            ExPolygons result = Slic3r::offset2_ex({ square_with_hole }, 5.f, -2.f);
            THEN("offset matches") {
                REQUIRE(start_at_min(result) == start_at_min(ExPolygons { {
                    { { 203, 203 }, { 97, 203 }, { 97, 97 }, { 203, 97 } },
                    { { 143, 143 }, { 143, 157 }, { 157, 157 }, { 157, 143 } } } }));
            }
        }
    }
    GIVEN("square_with_hole 2") {
        Slic3r::ExPolygon square_with_hole(
            { { 20000000, 20000000 }, { 0, 20000000 }, { 0, 0 }, { 20000000, 0 } },
            { { 5000000, 15000000 }, { 15000000, 15000000 }, { 15000000, 5000000 }, { 5000000, 5000000 } });
        WHEN("offset2_ex") {
            Slic3r::ExPolygons result = Slic3r::offset2_ex(ExPolygons { square_with_hole }, -1.f, 1.f);
            THEN("offset matches") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("square and hole") {
        WHEN("diff_ex") {
            ExPolygons result = Slic3r::diff_ex(Polygons{ square }, Polygons{ hole_in_square });
            THEN("hole is created") {
                REQUIRE(result.size() == 1);
                REQUIRE(square_with_hole.area() == result.front().area());
            }
        }
    }
    GIVEN("polyline") {
        Polyline polyline { { 50, 150 }, { 300, 150 } };
        WHEN("intersection_pl") {
            Polylines result = Slic3r::intersection_pl(Polylines { polyline }, Polygons { square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 2);
            }
            THEN("result lines have correct length") {
                // results are in no particular order
                REQUIRE(result[0].length() == 40);
                REQUIRE(result[1].length() == 40);
            }
        }
        WHEN("diff_pl") {
            Polylines result = Slic3r::diff_pl({ polyline }, Polygons{ square, hole_in_square });
            THEN("correct number of result lines") {
                REQUIRE(result.size() == 3);
            }
            // results are in no particular order
            THEN("the left result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 50; }) == 1);
            }
            THEN("the right result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 100; }) == 1);
            }
            THEN("the central result line has correct length") {
                REQUIRE(std::count_if(result.begin(), result.end(), [](const Polyline &pl) { return pl.length() == 20; }) == 1);
            }
        }
    }
	GIVEN("Clipper bug #96 / Slic3r issue #2028") {
		Slic3r::Polyline subject{
			{ 44735000, 31936670 }, { 55270000, 31936670 }, { 55270000, 25270000 }, { 74730000, 25270000 }, { 74730000, 44730000 }, { 68063296, 44730000 }, { 68063296, 55270000 }, { 74730000, 55270000 },
			{ 74730000, 74730000 }, { 55270000, 74730000 }, { 55270000, 68063296 }, { 44730000, 68063296 }, { 44730000, 74730000 }, { 25270000, 74730000 }, { 25270000, 55270000 }, { 31936670, 55270000 },
			{ 31936670, 44730000 }, { 25270000, 44730000 }, { 25270000, 25270000 }, { 44730000, 25270000 }, { 44730000, 31936670 } };
		Slic3r::Polygon clip { {75200000, 45200000}, {54800000, 45200000}, {54800000, 24800000}, {75200000, 24800000} };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons { clip });
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
		}
	}
	GIVEN("Clipper bug #122") {
		Slic3r::Polyline subject { { 1975, 1975 }, { 25, 1975 }, { 25, 25 }, { 1975, 25 }, { 1975, 1975 } };
		Slic3r::Polygons clip { { { 2025, 2025 }, { -25, 2025 } , { -25, -25 }, { 2025, -25 } },
								{ { 525, 525 }, { 525, 1475 }, { 1475, 1475 }, { 1475, 525 } } };
		Slic3r::Polylines result = Slic3r::intersection_pl({ subject }, clip);
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
			REQUIRE(result.front().points.size() == 5);
		}
	}
	GIVEN("Clipper bug #126") {
		Slic3r::Polyline subject { { 200000, 19799999 }, { 200000, 200000 }, { 24304692, 200000 }, { 15102879, 17506106 }, { 13883200, 19799999 }, { 200000, 19799999 } };
		Slic3r::Polygon clip { { 15257205, 18493894 }, { 14350057, 20200000 }, { -200000, 20200000 }, { -200000, -200000 }, { 25196917, -200000 } };
        Slic3r::Polylines result = Slic3r::intersection_pl(subject, Polygons { clip });
		THEN("intersection_pl - result is not empty") {
			REQUIRE(result.size() == 1);
		}
		THEN("intersection_pl - result has same length as subject polyline") {
			REQUIRE_THAT(result.front().length(), WithinRel(subject.length(), 0.001));
		}
	}

#if 0
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([50, 150], [300, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[100, 150], [200, 150]], 'clipped line orientation is preserved';
	}
	{
		# Clipper does not preserve polyline orientation
		my $polyline = Slic3r::Polyline->new([300, 150], [50, 150]);
		my $result = Slic3r::Geometry::Clipper::intersection_pl([$polyline], [$square]);
		is scalar(@$result), 1, 'intersection_pl - correct number of result lines';
		is_deeply $result->[0]->pp, [[200, 150], [100, 150]], 'clipped line orientation is preserved';
	}
	{
		# Disabled until Clipper bug #127 is fixed
		my $subject = [
			Slic3r::Polyline->new([-90000000, -100000000], [-90000000, 100000000]), # vertical
				Slic3r::Polyline->new([-100000000, -10000000], [100000000, -10000000]), # horizontal
				Slic3r::Polyline->new([-100000000, 0], [100000000, 0]), # horizontal
				Slic3r::Polyline->new([-100000000, 10000000], [100000000, 10000000]), # horizontal
		];
		my $clip = Slic3r::Polygon->new(# a circular, convex, polygon
			[99452190, 10452846], [97814760, 20791169], [95105652, 30901699], [91354546, 40673664], [86602540, 50000000],
			[80901699, 58778525], [74314483, 66913061], [66913061, 74314483], [58778525, 80901699], [50000000, 86602540],
			[40673664, 91354546], [30901699, 95105652], [20791169, 97814760], [10452846, 99452190], [0, 100000000],
			[-10452846, 99452190], [-20791169, 97814760], [-30901699, 95105652], [-40673664, 91354546],
			[-50000000, 86602540], [-58778525, 80901699], [-66913061, 74314483], [-74314483, 66913061],
			[-80901699, 58778525], [-86602540, 50000000], [-91354546, 40673664], [-95105652, 30901699],
			[-97814760, 20791169], [-99452190, 10452846], [-100000000, 0], [-99452190, -10452846],
			[-97814760, -20791169], [-95105652, -30901699], [-91354546, -40673664], [-86602540, -50000000],
			[-80901699, -58778525], [-74314483, -66913061], [-66913061, -74314483], [-58778525, -80901699],
			[-50000000, -86602540], [-40673664, -91354546], [-30901699, -95105652], [-20791169, -97814760],
			[-10452846, -99452190], [0, -100000000], [10452846, -99452190], [20791169, -97814760],
			[30901699, -95105652], [40673664, -91354546], [50000000, -86602540], [58778525, -80901699],
			[66913061, -74314483], [74314483, -66913061], [80901699, -58778525], [86602540, -50000000],
			[91354546, -40673664], [95105652, -30901699], [97814760, -20791169], [99452190, -10452846], [100000000, 0]
			);
		my $result = Slic3r::Geometry::Clipper::intersection_pl($subject, [$clip]);
		is scalar(@$result), scalar(@$subject), 'intersection_pl - expected number of polylines';
		is sum(map scalar(@$_), @$result), scalar(@$subject) * 2, 'intersection_pl - expected number of points in polylines';
	}
#endif
}

SCENARIO("Various Clipper operations - t/clipper.t", "[ClipperUtils]") {
    GIVEN("square with hole") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polygon   square2 { { 5, 12 }, { 25, 12 }, { 25, 18 }, { 5, 18 } };
        // CW oriented contour
        Slic3r::Polygon   hole_in_square { { 14, 14 }, { 14, 16 }, { 16, 16 }, { 16, 14 } };
        WHEN("intersection_ex with another square") {
            ExPolygons intersection = Slic3r::intersection_ex(Polygons{ square, hole_in_square }, Polygons{ square2 });
            THEN("intersection area matches (hole is preserved)") {
                ExPolygon match({ { 20, 18 }, { 10, 18 }, { 10, 12 }, { 20, 12 } },
                                { { 14, 16 }, { 16, 16 }, { 16, 14 }, { 14, 14 } });
                REQUIRE(intersection.size() == 1);
                REQUIRE_THAT(intersection.front().area(), WithinRel(match.area(), 0.001));
            }
        }
    }
    GIVEN("square with hole 2") {
        // CCW oriented contour
        Slic3r::Polygon   square { { 0, 0 }, { 40, 0 }, { 40, 40 }, { 0, 40 } };
        Slic3r::Polygon   square2 { { 10, 10 }, { 30, 10 }, { 30, 30 }, { 10, 30 } };
        // CW oriented contour
        Slic3r::Polygon   hole { { 15, 15 }, { 15, 25 }, { 25, 25 }, {25, 15 } };
        WHEN("union_ex with another square") {
            ExPolygons union_ = Slic3r::union_ex({ square, square2, hole });
            THEN("union of two ccw and one cw is a contour with no holes") {
                REQUIRE(union_.size() == 1);
                REQUIRE(union_.front() == ExPolygon { { 40, 40 }, { 0, 40 }, { 0, 0 }, { 40, 0 } } );
            }
        }
        WHEN("diff_ex with another square") {
			ExPolygons diff = Slic3r::diff_ex(Polygons{ square, square2 }, Polygons{ hole });
            THEN("difference of a cw from two ccw is a contour with one hole") {
                REQUIRE(diff.size() == 1);
                REQUIRE_THAT(diff.front().area(), WithinRel(ExPolygon({ {40, 40}, {0, 40}, {0, 0}, {40, 0} }, { {15, 25}, {25, 25}, {25, 15}, {15, 15} }).area(), 0.001));
            }
        }
    }
    GIVEN("yet another square") {
        Slic3r::Polygon  square { { 10, 10 }, { 20, 10 }, { 20, 20 }, { 10, 20 } };
        Slic3r::Polyline square_pl = square.split_at_first_point();
        WHEN("no-op diff_pl") {
            Slic3r::Polylines res = Slic3r::diff_pl({ square_pl }, Polygons{});
            THEN("returns the right number of polylines") {
                REQUIRE(res.size() == 1);
            }
            THEN("returns the unmodified input polyline") {
                REQUIRE(res.front().points.size() == square_pl.points.size());
            }
        }
    }
}

TEST_CASE("Top level expolygons of an even-odd union", "[ClipperUtils]") {
    // Create a polygon representing unit box
    Polygon unitbox;
    const auto UNIT = coord_t(1. / SCALING_FACTOR);
    unitbox.points = { Vec2crd{0, 0}, Vec2crd{UNIT, 0}, Vec2crd{UNIT, UNIT}, Vec2crd{0, UNIT}};
    
    Polygon box_frame = unitbox;
    box_frame.scale(20, 10);
    
    Polygon hole_left = unitbox;
    hole_left.scale(8);
    hole_left.translate(UNIT, UNIT);
    hole_left.reverse();
    
    Polygon hole_right = hole_left;
    hole_right.translate(UNIT * 10, 0);
    
    Polygon inner_left = unitbox;
    inner_left.scale(4);
    inner_left.translate(UNIT * 3, UNIT * 3);
    
    Polygon inner_right = inner_left;
    inner_right.translate(UNIT * 10, 0);

    ExPolygons reference;
    for (const Polygon &polygon : union_({box_frame, hole_left, hole_right, inner_left, inner_right}))
        reference.emplace_back(polygon);

    double area_sum = box_frame.area() + hole_left.area() +
                      hole_right.area() + inner_left.area() +
                      inner_right.area();
    
    REQUIRE(area_sum > 0);

    ExPolygons nested;
    ExPolygons top_level = top_level_expolygons(reference, &nested);
    auto area = [](const ExPolygons &expolys) {
        return std::accumulate(expolys.begin(), expolys.end(), 0.0, [](double a, const ExPolygon &e) { return a + e.area(); });
    };
    REQUIRE(top_level.size() == 1);
    REQUIRE(top_level.front().holes.size() == 2);
    REQUIRE(nested.size() == 2);
    REQUIRE(area_sum == Catch::Approx(area(top_level) + area(nested)));
    REQUIRE(top_level_expolygons(reference).size() == 1);
}

TEST_CASE("Top level contours of an even-odd Z-union", "[ClipperUtils]") {
    // Islands of nested objects carrying the Z index of their source object, like Brim.cpp does.
    const auto square = [](coord_t lo, coord_t hi, coord_t z) {
        ClipperZUtils::ZPath path;
        path.emplace_back(lo, lo, z);
        path.emplace_back(hi, lo, z);
        path.emplace_back(hi, hi, z);
        path.emplace_back(lo, hi, z);
        return path;
    };
    const auto max_zfill = [](const ClipperZUtils::ZPoint &e1bot, const ClipperZUtils::ZPoint &e1top, const ClipperZUtils::ZPoint &e2bot,
                              const ClipperZUtils::ZPoint &e2top, ClipperZUtils::ZPoint &pt) {
        pt.z() = std::max(std::max(e1bot.z(), e1top.z()), std::max(e2bot.z(), e2top.z()));
    };
    std::vector<std::pair<unsigned, std::set<coord_t>>> contours;
    const auto collect = [&contours](const ClipperZUtils::ZPath &path, unsigned depth) {
        std::set<coord_t> zs;
        for (const ClipperZUtils::ZPoint &p : path)
            zs.insert(p.z());
        contours.emplace_back(depth, std::move(zs));
    };

    SECTION("Fully nested islands: only the outermost island is top level") {
        ClipperZUtils::ZPaths islands;
        islands.emplace_back(square(0, 3000, 1));
        islands.emplace_back(square(1000, 2000, 2));
        islands.emplace_back(square(1300, 1700, 3));
        ClipperZUtils::clip_zpaths_polytree(ctUnion, islands, false, {}, max_zfill, collect, false, pftEvenOdd);

        // The even-odd union of nested contours is a ring: the middle island becomes a hole
        // of the outer island, the innermost island re-emerges inside that hole. Only the
        // outer island stays top level, matching the Clipper1 PolyTree ChildCount() walk.
        REQUIRE(contours.size() == 3);
        std::sort(contours.begin(), contours.end());
        REQUIRE(contours[0] == std::make_pair(0u, std::set<coord_t>{1}));
        REQUIRE(contours[1] == std::make_pair(1u, std::set<coord_t>{2}));
        REQUIRE(contours[2] == std::make_pair(2u, std::set<coord_t>{3}));
    }

    SECTION("Partially overlapping islands: both are top level") {
        ClipperZUtils::ZPaths islands;
        islands.emplace_back(square(0, 2000, 1));
        islands.emplace_back(square(1000, 3000, 2));
        ClipperZUtils::clip_zpaths_polytree(ctUnion, islands, false, {}, max_zfill, collect, false, pftEvenOdd);

        // Overlapping islands do not nest: no contour of the result is a hole, and both
        // objects appear on the top level contours (here corner-touching L shapes).
        REQUIRE(! contours.empty());
        std::set<coord_t> zs;
        for (const auto &contour : contours) {
            REQUIRE(contour.first == 0u);
            zs.insert(contour.second.begin(), contour.second.end());
        }
        REQUIRE(zs == std::set<coord_t>{1, 2});
    }
}

SCENARIO("union_ is idempotent", "[ClipperUtils]") {
    GIVEN("two overlapping squares") {
        Polygons squares {
            { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } },
            { { 50, 50 }, { 150, 50 }, { 150, 150 }, { 50, 150 } },
        };
        WHEN("union_ is applied twice") {
            Polygons once  = union_(squares);
            Polygons twice = union_(once);
            THEN("the result is one polygon and union is idempotent") {
                REQUIRE(once.size() == 1);
                REQUIRE(once == twice);
            }
        }
    }
    GIVEN("a single clean square") {
        Polygons squares { { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } } };
        THEN("union_ of a single polygon preserves its area") {
            Polygons result = union_(squares);
            REQUIRE(result.size() == 1);
            REQUIRE_THAT(result.front().area(), WithinRel(10000.0, 0.001));
        }
    }
}

SCENARIO("closing∘opening round-trips a clean shape", "[ClipperUtils]") {
    GIVEN("a clean square") {
        Polygon square { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
        WHEN("opening then closing with delta 5") {
            Polygons result = closing(opening({ square }, 5.f), 5.f);
            THEN("the area is conserved") {
                REQUIRE(result.size() == 1);
                REQUIRE_THAT(result.front().area(), WithinRel(square.area(), 0.001));
            }
        }
    }
    GIVEN("a square with a thin 4-wide bump") {
        Polygon bumped { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 52, 100 }, { 52, 104 }, { 48, 104 }, { 48, 100 }, { 0, 100 } };
        WHEN("opening with delta 5") {
            Polygons result = opening({ bumped }, 5.f);
            THEN("the thin bump is removed") {
                REQUIRE(result.size() == 1);
                REQUIRE_THAT(result.front().area(), WithinRel(10000.0, 0.001));
            }
        }
    }
    GIVEN("a square with a thin 4-wide notch") {
        Polygon notched { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 52, 100 }, { 52, 96 }, { 48, 96 }, { 48, 100 }, { 0, 100 } };
        WHEN("closing with delta 5") {
            Polygons result = closing({ notched }, 5.f);
            THEN("the thin notch is filled") {
                REQUIRE(result.size() == 1);
                REQUIRE_THAT(result.front().area(), WithinRel(10000.0, 0.001));
            }
        }
    }
}

SCENARIO("top_level_islands extracts nested islands", "[ClipperUtils]") {
    GIVEN("a nested outer square and inner island") {
        Polygon outer { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
        Polygon inner { { 40, 40 }, { 60, 40 }, { 60, 60 }, { 40, 60 } };
        WHEN("top_level_islands is called") {
            Polygons result = top_level_islands({ outer, inner });
            THEN("only the outermost contour is returned") {
                REQUIRE(result.size() == 1);
                REQUIRE_THAT(result.front().area(), WithinRel(outer.area(), 0.001));
            }
        }
    }
    GIVEN("two disjoint islands") {
        Polygon a { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
        Polygon b { { 20, 0 }, { 30, 0 }, { 30, 10 }, { 20, 10 } };
        WHEN("top_level_islands is called") {
            Polygons result = top_level_islands({ a, b });
            THEN("both top-level islands are returned") {
                REQUIRE(result.size() == 2);
            }
        }
    }

}

SCENARIO("simplify_polygons conserves union area", "[ClipperUtils]") {
    GIVEN("two overlapping squares") {
        Polygons squares {
            { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } },
            { { 50, 50 }, { 150, 50 }, { 150, 150 }, { 50, 150 } },
        };
        WHEN("simplify_polygons is called") {
            Polygons result = simplify_polygons(squares);
            THEN("the union area is conserved (overlap not double-counted)") {
                REQUIRE(result.size() == 1);
                REQUIRE_THAT(result.front().area(), WithinRel(union_(squares).front().area(), 0.001));
            }
        }
    }
}

SCENARIO("empty input produces empty output", "[ClipperUtils]") {
    GIVEN("an empty polygon set") {
        Polygons empty;
        THEN("all operations return empty without crashing") {
            REQUIRE(union_(empty).empty());
            REQUIRE(opening(empty, 5.f).empty());
            REQUIRE(closing(empty, 5.f).empty());
            REQUIRE(top_level_islands(empty).empty());
            REQUIRE(simplify_polygons(empty).empty());
        }
    }
}

TEST_CASE("Clipper2 CheckSplitOwner cyclic split does not stack overflow", "[ClipperUtils]") {
    const Polygons subject{ Polygon({
        {3, 0}, {3, 5}, {0, 3}, {5, 3}, {0, 5}, {5, 3}, {0, 3},
        {0, 0}, {0, 3}, {5, 3}, {0, 0}, {3, 3}, {2, 3}
    }) };
    const ExPolygons ex   = union_ex(subject, pftEvenOdd);
    const Polygons   flat = union_(subject, pftEvenOdd);
    double ex_area = 0., flat_area = 0.;
    for (const ExPolygon &e : ex)
        ex_area += e.area();
    for (const Polygon &p : flat)
        flat_area += p.area();
    REQUIRE(ex_area == Catch::Approx(flat_area));
    REQUIRE(ex_area > 0.);
}

TEST_CASE("contour_to_polygons does not leak 2-point Joined end_type", "[ClipperUtils]") {
    const Polygon two_pt({ {0, 0}, {100, 0} });
    const Polygon square({ {500, 0}, {700, 0}, {700, 200}, {500, 200} });
    const Polygons fwd = contour_to_polygons(Polygons{ two_pt, square }, 20.f);
    const Polygons rev = contour_to_polygons(Polygons{ square, two_pt }, 20.f);
    auto total_area = [](const Polygons &ps) {
        double a = 0.;
        for (const Polygon &p : ps)
            a += p.area();
        return a;
    };
    REQUIRE(fwd.size() == rev.size());
    REQUIRE(total_area(fwd) == Catch::Approx(total_area(rev)));
}

TEST_CASE("PolyTree64 misnesting is repaired across all five consumers", "[ClipperUtils]") {
    const Polygons subjA{
        Polygon({ {23000000, 31000000}, {7500000, 25900000}, {200000, 26450004}, {0, 26450004},
                  {0, 26550004}, {-100000, 26550004}, {-300000, 26600000}, {13000000, 0} }),
        Polygon({ {10200000, 24250000}, {10500000, 24250000}, {9800000, 24000000} }),
        Polygon({ {4500000, 22500000}, {5000000, 22000000}, {4000000, 22000000} }),
    };
    const Polygons clipA{
        Polygon({ {0, 26550004}, {-200000, 26550004}, {-200000, 26000000} }),
        Polygon({ {0, 26450004}, {100000, 26450004}, {0, 26000000} }),
        Polygon({ {10000000, 24000000}, {10199999, 24250000}, {10500000, 24250000}, {8000000, 26500000} }),
    };
    const Polygons subjB{
        Polygon({ {5200000, 700000}, {0, 5000000}, {0, 0} }),
        Polygon({ {3100000, 825000}, {3250000, 900001}, {3250000, 1000000}, {4000000, 1500000}, {3000000, 500000} }),
    };
    const Polygons clipB{
        Polygon({ {4500000, 1200000}, {4000000, 1000000}, {5000000, 800000} }),
        Polygon({ {3250000, 900001}, {3250000, 1000000}, {3000000, 1000000}, {3500000, 1100000}, {3100000, 1000000}, {3183343, 866672} }),
        Polygon({ {4000000, 1000000}, {4000000, 1000000}, {4000000, 1000000} }),
    };

    auto make_all_ccw = [](const Polygons &a, const Polygons &b) {
        Polygons out;
        for (Polygon p : a) {
            if (p.area() < 0.)
                p.reverse();
            out.emplace_back(std::move(p));
        }
        for (Polygon p : b) {
            if (p.area() < 0.)
                p.reverse();
            out.emplace_back(std::move(p));
        }
        return out;
    };
    auto check_expolygons_oriented = [](const ExPolygons &exs) {
        for (const ExPolygon &e : exs) {
            REQUIRE(e.contour.area() > 0.);
            for (const Polygon &h : e.holes)
                REQUIRE(h.area() < 0.);
        }
    };

    SECTION("diff_ex repairs root CW holes and direct-child CCW islands") {
        const ExPolygons dA = diff_ex(subjA, clipA);
        check_expolygons_oriented(dA);
        size_t holesA = 0;
        for (const ExPolygon &e : dA)
            holesA += e.holes.size();
        REQUIRE(holesA >= 1);

        const ExPolygons dB = diff_ex(subjB, clipB);
        check_expolygons_oriented(dB);
        REQUIRE(dB.size() >= 2);
    }

    SECTION("top_level_expolygons separates top-level contours and nested islands cleanly") {
        const Polygons allA = make_all_ccw(subjA, clipA);
        ExPolygons inA;
        for (const Polygon &p : allA)
            inA.emplace_back(p);
        ExPolygons nestedA;
        const ExPolygons topA = top_level_expolygons(inA, &nestedA);
        check_expolygons_oriented(topA);
        check_expolygons_oriented(nestedA);

        const Polygons allB = make_all_ccw(subjB, clipB);
        ExPolygons inB;
        for (const Polygon &p : allB)
            inB.emplace_back(p);
        ExPolygons nestedB;
        const ExPolygons topB = top_level_expolygons(inB, &nestedB);
        check_expolygons_oriented(topB);
        check_expolygons_oriented(nestedB);
        REQUIRE(nestedB.size() == 2);
    }

    SECTION("top_level_islands excludes misplaced CW holes") {
        const Polygons allA = make_all_ccw(subjA, clipA);
        const Polygons tli  = top_level_islands(allA);
        REQUIRE(! tli.empty());
        for (const Polygon &p : tli)
            REQUIRE(p.area() > 0.);
    }

    SECTION("union_pt_chained_outside_in orients all loops CCW and respects 3-level nesting") {
        Polygons framed = make_all_ccw(subjA, clipA);
        framed.emplace_back(Polygon({ {-5000000, -5000000}, {30000000, -5000000}, {30000000, 35000000}, {-5000000, 35000000} }));
        framed.emplace_back(Polygon({ {-3000000, -3000000}, {28000000, -3000000}, {28000000, 33000000}, {-3000000, 33000000} }));
        const Polygons chained = union_pt_chained_outside_in(framed);
        REQUIRE(chained.size() >= 8);
        for (const Polygon &p : chained)
            REQUIRE(p.area() > 0.);
        for (size_t i = 0; i < chained.size(); ++i) {
            for (size_t j = i + 1; j < chained.size(); ++j) {
                if (chained[j].area() > chained[i].area()) {
                    bool all_in = true;
                    for (const Point &pt : chained[i].points)
                        if (! contains(chained[j], pt, true)) {
                            all_in = false;
                            break;
                        }
                    REQUIRE_FALSE(all_in);
                }
            }
        }
    }

    SECTION("clip_zpaths_polytree does not attribute nested object at depth 0") {
        const Polygons allA = make_all_ccw(subjA, clipA);
        ClipperZUtils::ZPaths zpaths;
        for (size_t i = 0; i < allA.size(); ++i) {
            ClipperZUtils::ZPath zp;
            for (const Point &pt : allA[i].points)
                zp.emplace_back(pt.x(), pt.y(), coord_t(i + 1));
            zpaths.emplace_back(std::move(zp));
        }
        auto max_zfill = [](const ClipperZUtils::ZPoint &e1b, const ClipperZUtils::ZPoint &e1t,
                            const ClipperZUtils::ZPoint &e2b, const ClipperZUtils::ZPoint &e2t, ClipperZUtils::ZPoint &pt) {
            pt.z() = std::max(std::max(e1b.z(), e1t.z()), std::max(e2b.z(), e2t.z()));
        };
        std::set<coord_t> depth0_objs;
        ClipperZUtils::clip_zpaths_polytree(ctUnion, zpaths, false, {}, max_zfill,
            [&depth0_objs](const ClipperZUtils::ZPath &path, unsigned depth) {
                if (depth == 0) {
                    REQUIRE(ClipperZUtils::area(path) > 0.);
                    for (const ClipperZUtils::ZPoint &p : path)
                        if (p.z() > 0)
                            depth0_objs.insert(p.z());
                }
            }, false, pftEvenOdd);
        REQUIRE(depth0_objs.count(1) == 1);
        REQUIRE(depth0_objs.count(3) == 0);
    }
}

TEST_CASE("offset(Polygons) preserves CW contour in concave notch and drops zero-area paths on shrink", "[ClipperUtils]") {
    SECTION("CW triangle inside concave U-shape notch is not swallowed") {
        const Polygon u_shape({ {0, 0}, {1000, 0}, {1000, 1000}, {800, 1000}, {800, 200}, {200, 200}, {200, 1000}, {0, 1000} });
        const Polygon cw_tri({ {400, 500}, {500, 800}, {600, 500} });
        REQUIRE(u_shape.area() > 0.);
        REQUIRE(cw_tri.area() < 0.);
        const Polygons res = offset(Polygons{ u_shape, cw_tri }, 10.f);
        REQUIRE(res.size() == 2);
        std::vector<double> areas{ res[0].area(), res[1].area() };
        std::sort(areas.begin(), areas.end());
        // The CW triangle in the concave notch survives as a shrunk standalone contour, while the U-shape expands.
        REQUIRE(areas[0] > 15000.);
        REQUIRE(areas[0] < std::abs(cw_tri.area()));
        REQUIRE(areas[1] > u_shape.area());
    }

    SECTION("Zero-area bow-tie mixed with valid square vanishes under negative offset") {
        const Polygon bow_tie({ {0, 0}, {200, 200}, {200, 0}, {0, 200} });
        const Polygon square({ {500, 0}, {700, 0}, {700, 200}, {500, 200} });
        REQUIRE(bow_tie.area() == 0.);
        const Polygons res = offset(Polygons{ bow_tie, square }, -10.f);
        REQUIRE(res.size() == 1);
        REQUIRE(res.front().area() == Catch::Approx(180. * 180.));
    }

    SECTION("Zero-area bow-tie with inner CW path does not trigger inverted positive group offset") {
        const Polygon big_bow_tie({ {0, 0}, {1000, 1000}, {1000, 0}, {0, 1000} });
        const Polygon inner_cw({ {200, 400}, {200, 600}, {400, 600}, {400, 400} });
        REQUIRE(big_bow_tie.area() == 0.);
        REQUIRE(inner_cw.area() < 0.);
        const Polygons res = offset(Polygons{ big_bow_tie, inner_cw }, 10.f);
        REQUIRE(! res.empty());
        double total_area = 0.;
        bool   has_ccw    = false;
        for (const Polygon &p : res) {
            total_area += p.area();
            if (p.area() > 0.)
                has_ccw = true;
        }
        REQUIRE(has_ccw);
        REQUIRE(total_area > 0.);
    }
}
