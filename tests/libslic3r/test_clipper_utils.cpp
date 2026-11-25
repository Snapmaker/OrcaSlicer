#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <numeric>
#include <set>
#include <iostream>
#include <optional>
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

// An outline with a small clockwise hole well inside it, cut along its edge by three clip polygons that never reach the
// hole. Clipper2 puts the hole at the top level of its PolyTree, beside the outline rather than under it. The coordinates
// come from a sliced layer reduced to the few paths that still do this; most changes to them don't.
static const Polygons misnesting_subject{
    Polygon({ { 23000000, 31000000 }, { 7500000, 25900000 }, { 200000, 26450004 }, { 0, 26450004 }, { 0, 26550004 },
              { -100000, 26550004 }, { -300000, 26600000 }, { 13000000, 0 } }),
    Polygon({ { 10200000, 24250000 }, { 10500000, 24250000 }, { 9800000, 24000000 } }),
    Polygon({ { 4500000, 22500000 }, { 5000000, 22000000 }, { 4000000, 22000000 } }),
};
static const Polygons misnesting_clip{
    Polygon({ { 0, 26550004 }, { -200000, 26550004 }, { -200000, 26000000 } }),
    Polygon({ { 0, 26450004 }, { 100000, 26450004 }, { 0, 26000000 } }),
    Polygon({ { 10000000, 24000000 }, { 10199999, 24250000 }, { 10500000, 24250000 }, { 8000000, 26500000 } }),
};

// The case above inside a frame, a square outline with a square hole around the case, and with an island in its hole.
// Clipper2 then puts the hole beside the outline inside the frame's hole, where the frame and the outline both enclose
// it, and the island under the misplaced hole.
static Polygons misnesting_framed_subject()
{
    Polygons subject = misnesting_subject;
    subject.emplace_back(Polygon({ { -2000000, -2000000 }, { 25000000, -2000000 }, { 25000000, 33000000 }, { -2000000, 33000000 } }));
    subject.emplace_back(Polygon({ { -1000000, -1000000 }, { -1000000, 32000000 }, { 24000000, 32000000 }, { 24000000, -1000000 } }));
    subject.emplace_back(Polygon({ { 4400000, 22100000 }, { 4600000, 22100000 }, { 4500000, 22300000 } }));
    return subject;
}
// In the hole of the framed case, beside its island.
static const Point in_framed_hole(4500000, 22400000);
// In the island of the framed case.
static const Point in_framed_island(4500000, 22150000);

static void check_orientation(const ExPolygons &expolygons)
{
    for (const ExPolygon &expoly : expolygons) {
        CHECK(expoly.contour.is_counter_clockwise());
        for (const Polygon &hole : expoly.holes)
            CHECK(hole.is_clockwise());
    }
}

// How many of the expolygons contain the point, each on its own, as a per-piece operation sees them.
static size_t pieces_containing(const ExPolygons &expolygons, const Point &pt)
{
    return std::count_if(expolygons.begin(), expolygons.end(), [&pt](const ExPolygon &expoly) { return expoly.contains(pt); });
}

TEST_CASE("Every hole of a boolean result belongs to the expolygon around it", "[ClipperUtils]") {
    const Polygon &hole = misnesting_subject.back();
    REQUIRE(hole.is_clockwise());
    const ExPolygons result = diff_ex(misnesting_subject, misnesting_clip);
    check_orientation(result);
    CHECK(pieces_containing(result, hole.centroid()) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(misnesting_subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A misplaced hole goes to the innermost contour around it, an island in it stays filled", "[ClipperUtils]") {
    const Polygons   subject = misnesting_framed_subject();
    const ExPolygons result  = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    // Given to the frame instead of the outline, the hole would leave the outline covering it.
    CHECK(pieces_containing(result, in_framed_hole) == 0);
    CHECK(pieces_containing(result, in_framed_island) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A misplaced hole touching the contour around it goes to that contour", "[ClipperUtils]") {
    // The first two vertices of the hole the subject's triangle cuts round a fraction of a unit outside the outline, the
    // third is well inside it. Reduced from a sliced layer; Clipper2 puts the hole at the top level of its PolyTree.
    const Polygons subject{
        Polygon({ { 1000000, 5000000 }, { 1000000, 4000000 }, { 6000000, 0 } }),
        Polygon({ { 1024999, 4500000 }, { 1100011, 4574998 }, { 1100011, 4450000 } }),
    };
    const Polygons clip{
        Polygon({ { 1500000, 5000000 }, { 1062508, 4612507 }, { 1212486, 4462504 } }),
        Polygon({ { 1100000, 4430000 }, { 1024998, 4500001 }, { 2000000, 3000000 } }),
    };
    const ExPolygons result = diff_ex(subject, clip);
    check_orientation(result);
    CHECK(pieces_containing(result, subject.back().centroid()) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}

TEST_CASE("A misplaced island comes out as an expolygon of its own", "[ClipperUtils]") {
    // Clipper2 puts the island of this result directly under the outer contour, at the level of its holes, instead of
    // under the hole around it. Reduced from a sliced layer.
    const Polygons subject{
        Polygon({ { 5200000, 700000 }, { 0, 5000000 }, { 0, 0 } }),
        Polygon({ { 3100000, 825000 }, { 3250000, 900001 }, { 3250000, 1000000 }, { 4000000, 1500000 }, { 3000000, 500000 } }),
    };
    const Polygons clip{
        Polygon({ { 4500000, 1200000 }, { 4000000, 1000000 }, { 5000000, 800000 } }),
        Polygon({ { 3250000, 900001 }, { 3250000, 1000000 }, { 3000000, 1000000 }, { 3500000, 1100000 }, { 3100000, 1000000 }, { 3183343, 866672 } }),
        Polygon({ { 4000000, 1000000 }, { 4000000, 1000000 }, { 4000000, 1000000 } }),
    };
    const ExPolygons result = diff_ex(subject, clip);
    check_orientation(result);
    // In the island.
    CHECK(pieces_containing(result, Point(3230000, 1020000)) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}

TEST_CASE("Nested islands of an even-odd union keep their misplaced holes", "[ClipperUtils]") {
    // The framed case as one even-odd union: the frame is the top level, everything in its hole is nested, and Clipper2
    // puts the case's hole beside the outline among the nested islands.
    ExPolygons input;
    for (const Polygon &polygon : misnesting_framed_subject())
        input.emplace_back(polygon);
    for (const Polygon &polygon : misnesting_clip)
        input.emplace_back(polygon);
    ExPolygons       nested;
    const ExPolygons top_level = top_level_expolygons(input, &nested);
    REQUIRE(top_level.size() == 1);
    check_orientation(nested);
    CHECK(pieces_containing(nested, in_framed_hole) == 0);
    CHECK(pieces_containing(nested, in_framed_island) == 1);
}

TEST_CASE("Loops chained outside in keep their nesting where Clipper2 misplaces one", "[ClipperUtils]") {
    // An even-odd union of the case's paths leaves the hole at the top level of the PolyTree, among the outer loops.
    Polygons loops = misnesting_subject;
    polygons_append(loops, misnesting_clip);
    const Polygons chained = union_pt_chained_outside_in(loops);
    REQUIRE(chained.size() == 7);
    for (const Polygon &loop : chained)
        CHECK(loop.is_counter_clockwise());
    // The hole's loop comes right after the outline's, which has no other loop inside it.
    const auto outline = std::max_element(chained.begin(), chained.end(), [](const Polygon &a, const Polygon &b) { return a.area() < b.area(); });
    REQUIRE(outline + 1 != chained.end());
    CHECK((outline + 1)->area() == Catch::Approx(- misnesting_subject.back().area()));
}

TEST_CASE("A misplaced hole hugging an island inside it still goes to the outline", "[ClipperUtils]") {
    // The case's hole with an island filling most of it, 20 units from it: the hole's vertices all lie close to the
    // island and its middle inside it, yet the island is inside the hole, not around it.
    Polygons subject = misnesting_subject;
    subject.emplace_back(Polygon({ { 4000020, 22000010 }, { 4500000, 22150000 }, { 4999980, 22000010 },
                                   { 4650000, 22160000 }, { 4500000, 22499980 }, { 4350000, 22160000 } }));
    const ExPolygons result = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    // In the hole below the island, and in the island.
    CHECK(pieces_containing(result, Point(4500000, 22050000)) == 0);
    CHECK(pieces_containing(result, Point(4500000, 22300000)) == 1);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A concave misplaced hole touching the outline at every vertex goes to it", "[ClipperUtils]") {
    // The case with the outline drawn in to two more vertices and the hole replaced by a concave pentagon, each of its
    // vertices within 3 units of the outline; the middle of its bounding box is outside both.
    Polygons subject = misnesting_subject;
    subject.front().points.insert(subject.front().points.end(), { { 7000000, 20000000 }, { 20000000, 22000000 } });
    subject.back() = Polygon({ { 1500002, 22999999 }, { 20333332, 22999999 }, { 19999999, 22000001 }, { 6999998, 20000001 }, { 12999995, 13 } });
    const ExPolygons result = diff_ex(subject, misnesting_clip);
    check_orientation(result);
    CHECK(pieces_containing(result, Point(10000000, 22500000)) == 0);
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, misnesting_clip)), 1e-9));
}

TEST_CASE("A point strictly inside a polygon, where there is one", "[ClipperUtils]") {
    // A notch with a horizontal edge at the middle of its bounding box, so a line through that middle runs along the edge;
    // two lobes joined by a one unit neck and a square with a one unit spike, where the widest band between vertex heights
    // runs through the thin part.
    const Polygons wide{
        Polygon({ { 0, 0 }, { 2000000, 0 }, { 2000000, 5000000 }, { 8000000, 5000000 }, { 8000000, 0 }, { 10000000, 0 },
                  { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 0, 0 }, { 100, 0 }, { 100, 5 }, { 51, 5 }, { 51, 25 }, { 100, 25 }, { 100, 30 }, { 0, 30 }, { 0, 25 },
                  { 50, 25 }, { 50, 5 }, { 0, 5 } }),
        Polygon({ { 0, 0 }, { 1000000, 0 }, { 1000000, 1000000 }, { 500001, 1000000 }, { 500001, 5000000 }, { 500000, 5000000 },
                  { 500000, 1000000 }, { 0, 1000000 } }),
    };
    for (Polygon polygon : wide)
        for (int winding = 0; winding < 2; ++ winding, polygon.reverse()) {
            const std::optional<Point> pt = ClipperUtils::interior_point(polygon);
            REQUIRE(pt);
            CHECK(contains(polygon, *pt, false));
        }
    // One unit thick everywhere, these hold no grid point strictly inside: a strip, and rows one unit high joined at the
    // left.
    CHECK_FALSE(ClipperUtils::interior_point(Polygon({ { 0, 0 }, { 1000, 0 }, { 1000, 1 }, { 0, 1 } })));
    Polygon rows({ { 0, 0 }, { 100, 0 } });
    for (coord_t y = 1; y < 100; y += 2)
        rows.points.insert(rows.points.end(), { { 100, y }, { 1, y }, { 1, y + 1 }, { 100, y + 1 } });
    rows.points.emplace_back(0, 100);
    CHECK_FALSE(ClipperUtils::interior_point(rows));
}

TEST_CASE("A correctly nested result converts in the order of its tree", "[ClipperUtils]") {
    // Two squares, one with a hole holding an island, the other with two holes and a corner cut off. Clipper2 nests this
    // correctly, so the expolygons follow its tree, each path as Clipper2 returned it: an outline with its holes in tree
    // order, then the islands of those holes, outline by outline.
    const Polygons subject{
        Polygon({ { 0, 0 }, { 10000000, 0 }, { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 2000000, 2000000 }, { 2000000, 8000000 }, { 8000000, 8000000 }, { 8000000, 2000000 } }),
        Polygon({ { 4000000, 4000000 }, { 6000000, 4000000 }, { 6000000, 6000000 }, { 4000000, 6000000 } }),
        Polygon({ { 20000000, 0 }, { 32000000, 0 }, { 32000000, 12000000 }, { 20000000, 12000000 } }),
        Polygon({ { 21000000, 1000000 }, { 21000000, 4000000 }, { 24000000, 4000000 }, { 24000000, 1000000 } }),
        Polygon({ { 26000000, 6000000 }, { 26000000, 10000000 }, { 30000000, 10000000 }, { 30000000, 6000000 } }),
    };
    const Polygons clip{ Polygon({ { 31000000, -1000000 }, { 33000000, -1000000 }, { 33000000, 1000000 }, { 31000000, 1000000 } }) };
    ExPolygons expected(3);
    expected[0].contour = Polygon({ { 31000000, 1000000 }, { 32000000, 1000000 }, { 32000000, 12000000 }, { 20000000, 12000000 },
                                    { 20000000, 0 }, { 31000000, 0 } });
    expected[0].holes   = { Polygon({ { 26000000, 10000000 }, { 30000000, 10000000 }, { 30000000, 6000000 }, { 26000000, 6000000 } }),
                            Polygon({ { 21000000, 4000000 }, { 24000000, 4000000 }, { 24000000, 1000000 }, { 21000000, 1000000 } }) };
    expected[1].contour = Polygon({ { 10000000, 10000000 }, { 0, 10000000 }, { 0, 0 }, { 10000000, 0 } });
    expected[1].holes   = { Polygon({ { 2000000, 8000000 }, { 8000000, 8000000 }, { 8000000, 2000000 }, { 2000000, 2000000 } }) };
    expected[2].contour = Polygon({ { 6000000, 6000000 }, { 4000000, 6000000 }, { 4000000, 4000000 }, { 6000000, 4000000 } });
    CHECK(diff_ex(subject, clip) == expected);
}

TEST_CASE("Loops of a correctly nested union chain in the order of its tree", "[ClipperUtils]") {
    // Three nested squares and two more beside them, as concentric fill hands them over: chained outside in, depth first,
    // each loop as Clipper2 returned it, the holes reversed to run counter-clockwise.
    const Polygons loops{
        Polygon({ { 0, 0 }, { 10000000, 0 }, { 10000000, 10000000 }, { 0, 10000000 } }),
        Polygon({ { 1000000, 1000000 }, { 9000000, 1000000 }, { 9000000, 9000000 }, { 1000000, 9000000 } }),
        Polygon({ { 2000000, 2000000 }, { 8000000, 2000000 }, { 8000000, 8000000 }, { 2000000, 8000000 } }),
        Polygon({ { 20000000, 0 }, { 32000000, 0 }, { 32000000, 12000000 }, { 20000000, 12000000 } }),
        Polygon({ { 21000000, 1000000 }, { 31000000, 1000000 }, { 31000000, 11000000 }, { 21000000, 11000000 } }),
    };
    // The tree lists the group on the right first and chain_points() of this repository keeps the order of the tree. The
    // upstream test expects the left group first, which its chain_points() produces.
    const Polygons expected{
        Polygon({ { 32000000, 12000000 }, { 20000000, 12000000 }, { 20000000, 0 }, { 32000000, 0 } }),
        Polygon({ { 31000000, 1000000 }, { 31000000, 11000000 }, { 21000000, 11000000 }, { 21000000, 1000000 } }),
        Polygon({ { 10000000, 10000000 }, { 0, 10000000 }, { 0, 0 }, { 10000000, 0 } }),
        Polygon({ { 9000000, 1000000 }, { 9000000, 9000000 }, { 1000000, 9000000 }, { 1000000, 1000000 } }),
        Polygon({ { 8000000, 8000000 }, { 2000000, 8000000 }, { 2000000, 2000000 }, { 8000000, 2000000 } }),
    };
    CHECK(union_pt_chained_outside_in(loops) == expected);
}

TEST_CASE("A clockwise sliver with nothing around it comes back as Clipper2 returned it", "[ClipperUtils]") {
    // Rounding leaves a clockwise sliver of half a square unit beside the result's one piece, outside it. Reduced from a
    // sliced layer. It is no hole of anything, so it stays a contour of its own, neither dropped nor given to the piece.
    const Polygons subject{ Polygon({ { 875000, 200000 }, { 900002, 175002 }, { 950001, 150004 } }) };
    const Polygons clip{ Polygon({ { 879409, 185299 }, { 950000, 150004 }, { 1000000, 200000 } }) };
    const ExPolygons result = diff_ex(subject, clip);
    REQUIRE(result.size() == 2);
    CHECK(std::count_if(result.begin(), result.end(), [](const ExPolygon &expoly) { return expoly.contour.is_clockwise() && expoly.holes.empty(); }) == 1);
    CHECK(std::all_of(result.begin(), result.end(), [](const ExPolygon &expoly) { return expoly.holes.empty(); }));
    CHECK_THAT(area(result), Catch::Matchers::WithinRel(area(diff(subject, clip)), 1e-9));
}

TEST_CASE("An even-odd union of a path retracing its edges covers the same area as the flat union", "[ClipperUtils]") {
    // Building the PolyTree of this path leaves emptied output loops that point at each other, and looking for a
    // loop's owner through them recursed until the stack overflowed.
    const Polygons subject{ Polygon({ { 3, 0 }, { 3, 5 }, { 0, 3 }, { 5, 3 }, { 0, 5 }, { 5, 3 }, { 0, 3 },
                                      { 0, 0 }, { 0, 3 }, { 5, 3 }, { 0, 0 }, { 3, 3 }, { 2, 3 } }) };
    CHECK_THAT(area(union_ex(subject, pftEvenOdd)), Catch::Matchers::WithinRel(area(union_(subject, pftEvenOdd)), 1e-9));
}

// Every ring offset on its own, the results united the way the path by path offset of a batch unites them.
static ExPolygons offset_ring_by_ring(const Polygons &polygons, float delta)
{
    Polygons out;
    for (const Polygon &polygon : polygons)
        append(out, offset(polygon, delta, jtMiter));
    return union_ex(out);
}

TEST_CASE("A clockwise contour touching the contour around it is offset on its own", "[ClipperUtils]") {
    // A U, and in its notch a clockwise triangle touching the walls of the notch at its three vertices while lying
    // entirely in the notch: outside the material of the U.
    const Polygon u{ { { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 800, 1000 }, { 800, 200 }, { 200, 200 }, { 200, 1000 }, { 0, 1000 } } };
    const Polygon triangle{ { { 200, 500 }, { 800, 500 }, { 500, 200 } } };
    REQUIRE(triangle.is_clockwise());

    SECTION("The triangle in the notch stays a contour of its own")
    {
        const Polygons result = offset({ u, triangle }, 10.f, jtMiter);
        REQUIRE(result.size() == 2);
        const auto grown_u = std::max_element(result.begin(), result.end(), [](const Polygon &l, const Polygon &r) { return l.area() < r.area(); });
        CHECK(grown_u->area() == 576400.);
        // Offset like the hole it was taken for, the triangle vanished.
        const Polygon &shrunk_triangle = grown_u == result.begin() ? result[1] : result[0];
        CHECK(shrunk_triangle.is_counter_clockwise());
        CHECK(std::abs(shrunk_triangle.area()) == 76176.);
    }
    SECTION("A triangle with all its vertices in the material, its edges crossing the notch, covers the same region as ring by ring")
    {
        // Whichever vertex each ring starts at.
        const Polygon crossing{ { { 100, 600 }, { 900, 600 }, { 500, 100 } } };
        REQUIRE(crossing.is_clockwise());
        for (int start = 0; start < 3; ++ start) {
            Polygons rings{ u, crossing };
            std::rotate(rings[1].points.begin(), rings[1].points.begin() + start, rings[1].points.end());
            std::rotate(rings[0].points.begin(), rings[0].points.begin() + start, rings[0].points.end());
            const ExPolygons region = union_ex(offset(rings, 10.f, jtMiter));
            const ExPolygons reference = offset_ring_by_ring(rings, 10.f);
            CHECK(start_at_min(region) == start_at_min(reference));
            // Two pieces: the U, and the triangle shrunk inside the material of the U.
            CHECK(region.size() == 2);
        }
    }
    SECTION("A hole of the contour, away from its boundary, still shrinks as a hole")
    {
        // In the left arm of the U.
        const Polygon hole{ { { 50, 300 }, { 50, 400 }, { 150, 400 }, { 150, 300 } } };
        REQUIRE(hole.is_clockwise());
        const Polygons result = offset({ u, hole }, 10.f, jtMiter);
        REQUIRE(result.size() == 2);
        const auto grown_u = std::max_element(result.begin(), result.end(), [](const Polygon &l, const Polygon &r) { return l.area() < r.area(); });
        CHECK(grown_u->area() == 576400.);
        CHECK(grown_u->is_counter_clockwise());
        const Polygon &shrunk_hole = grown_u == result.begin() ? result[1] : result[0];
        CHECK(shrunk_hole.is_clockwise());
        CHECK(std::abs(shrunk_hole.area()) == 6400.);
    }
}
