#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <set>
#include <iostream>
#include <boost/filesystem.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperZUtils.hpp"
#include "libslic3r/Emboss.hpp"
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

    SECTION("Zero-area bow-tie mixed with valid square shrinks positive-winding lobe under negative offset (Clipper1 semantics)") {
        const Polygon bow_tie({ {0, 0}, {200, 200}, {200, 0}, {0, 200} });
        const Polygon square({ {500, 0}, {700, 0}, {700, 200}, {500, 200} });
        REQUIRE(bow_tie.area() == 0.);
        const Polygons res = offset(Polygons{ bow_tie, square }, -10.f);
        REQUIRE(res.size() == 2);
        const double total = std::accumulate(res.begin(), res.end(), 0., [](double acc, const Polygon &p) { return acc + p.area(); });
        REQUIRE(total == Catch::Approx(38176.).margin(20.));
    }

    SECTION("Collinear zero-area polygon mixed with valid square vanishes under negative offset") {
        const Polygon collinear({ {0, 0}, {200, 0}, {100, 0} });
        const Polygon square({ {500, 0}, {700, 0}, {700, 200}, {500, 200} });
        REQUIRE(collinear.area() == 0.);
        const Polygons res = offset(Polygons{ collinear, square }, -10.f);
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

TEST_CASE("simplify_polygons, simplify_polygons_ex and polygons_simplify split self-touching contours", "[ClipperUtils]") {
    auto has_dup_vertex = [](const Polygon &p) {
        std::set<Point> s;
        for (const Point &pt : p.points)
            if (! s.insert(pt).second)
                return true;
        return false;
    };

    SECTION("Vertex-touching hole is split into separate outer contour and CW hole") {
        const Polygon self_touch_hole({ {0, 0}, {500, 0}, {300, 500}, {700, 500}, {500, 0}, {1000, 0}, {1000, 1000}, {0, 1000} });
        const Polygons polys = simplify_polygons({ self_touch_hole });
        REQUIRE(polys.size() == 2);
        for (const Polygon &p : polys)
            REQUIRE_FALSE(has_dup_vertex(p));

        const ExPolygons exs = simplify_polygons_ex({ self_touch_hole });
        REQUIRE(exs.size() == 1);
        REQUIRE(exs.front().holes.size() == 1);
        REQUIRE_FALSE(has_dup_vertex(exs.front().contour));
        REQUIRE_FALSE(has_dup_vertex(exs.front().holes.front()));
        REQUIRE(exs.front().area() == Catch::Approx(self_touch_hole.area()));
    }

    SECTION("Half-integer diagonal midpoint re-nests odd-coordinate touching hole accurately") {
        const Polygon diag_touch({ {0, 0}, {5000, 0}, {1001, 5001}, {5001, 5001}, {5000, 0}, {10001, 0}, {10001, 10001}, {0, 10001} });
        const ExPolygons exs = simplify_polygons_ex({ diag_touch });
        REQUIRE(exs.size() == 1);
        REQUIRE(exs.front().holes.size() == 1);
        REQUIRE(exs.front().area() == Catch::Approx(diag_touch.area()));
    }

    SECTION("Self-touching horizontal bowtie splits in simplify_polygons, simplify_polygons_ex and polygons_simplify") {
        const Polygon hbow({ {0, 0}, {1000, 1000}, {2000, 0}, {2000, 2000}, {1000, 1000}, {0, 2000} });
        const Polygons sp = simplify_polygons({ hbow });
        REQUIRE(sp.size() == 2);
        for (const Polygon &p : sp)
            REQUIRE_FALSE(has_dup_vertex(p));

        const ExPolygons sp_ex = simplify_polygons_ex({ hbow });
        REQUIRE(sp_ex.size() == 2);
        for (const ExPolygon &e : sp_ex) {
            REQUIRE(e.holes.empty());
            REQUIRE_FALSE(has_dup_vertex(e.contour));
        }

        const Polygons ps_strict = polygons_simplify({ hbow }, 1.0);
        REQUIRE(ps_strict.size() == 2);
        for (const Polygon &p : ps_strict)
            REQUIRE_FALSE(has_dup_vertex(p));
    }
}

TEST_CASE("ExPolygon opening and safety-offset clipping normalize input orientations consistently", "[ClipperUtils]") {
    const Polygon   cw_contour({ {0, 0}, {0, 1000}, {1000, 1000}, {1000, 0} });
    const Polygon   ccw_hole({ {300, 300}, {700, 300}, {700, 700}, {300, 700} });
    const ExPolygons inverted{ ExPolygon(cw_contour, ccw_hole) };

    const Polygons   op    = opening(inverted, 10.f);
    const ExPolygons op_ex = opening_ex(inverted, 10.f);
    REQUIRE(op.size() == 2);
    REQUIRE(op_ex.size() == 1);
    double op_area = 0.;
    for (const Polygon &p : op)
        op_area += p.area();
    REQUIRE(op_area == Catch::Approx(op_ex.front().area()));

    const Polygon    subj({ {0, 0}, {1000, 0}, {1000, 1000}, {0, 1000} });
    const ExPolygons diff_safe = diff_ex(Polygons{ subj }, inverted, ApplySafetyOffset::Yes);
    REQUIRE(diff_safe.size() == 1);
    REQUIRE(diff_safe.front().holes.empty());
    REQUIRE(diff_safe.front().area() == Catch::Approx(380. * 380.));
}

TEST_CASE("MR13 open polyline clipping Clipper1ExcludeTop semantics and partition", "[ClipperUtils]")
{
    const Polygon clip_sq({ { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 0, 1000 } });

    SECTION("Horizontal top and standalone bottom open edges match Clipper1 exclusion semantics")
    {
        const Polylines top_edge{ Polyline{ { -200, 1000 }, { 1200, 1000 } } };
        const Polylines top_in  = intersection_pl(top_edge, Polygons{ clip_sq });
        const Polylines top_out = diff_pl(top_edge, Polygons{ clip_sq });
        CHECK(top_in.empty());
        REQUIRE(top_out.size() == 1);
        CHECK(top_out.front().length() == Catch::Approx(1400.));

        const Polylines bot_edge{ Polyline{ { -200, 0 }, { 1200, 0 } } };
        const Polylines bot_in  = intersection_pl(bot_edge, Polygons{ clip_sq });
        const Polylines bot_out = diff_pl(bot_edge, Polygons{ clip_sq });
        CHECK(bot_in.empty());
        REQUIRE(bot_out.size() == 1);
        CHECK(bot_out.front().length() == Catch::Approx(1400.));

        const Polygon wide_clip({ { coord_t(scale_(-0.2)), coord_t(scale_(0.)) },
                                  { coord_t(scale_(10.2)), coord_t(scale_(0.)) },
                                  { coord_t(scale_(10.2)), coord_t(scale_(10.2)) },
                                  { coord_t(scale_(-0.2)), coord_t(scale_(10.2)) } });
        for (bool rev : { false, true }) {
            Polyline line{ { coord_t(scale_(1.)), 0 }, { coord_t(scale_(9.)), 0 } };
            if (rev)
                line.reverse();
            CHECK(intersection_pl(line, Polygons{ wide_clip }).empty());
            const Polylines d = diff_pl(line, Polygons{ wide_clip });
            REQUIRE(d.size() == 1);
            CHECK(d.front().length() == Catch::Approx(scale_(8.)));
        }
    }

    SECTION("clip_closed_polygon_split matches Clipper1 across all 4 rotations and 2 orientations")
    {
        auto check_loop_partition = [](const Points &base_ccw, const Polygons &clip, double expected_in, double expected_out) {
            for (int rev = 0; rev < 2; ++rev) {
                Points pts = base_ccw;
                if (rev)
                    std::reverse(pts.begin(), pts.end());
                for (size_t rot = 0; rot < pts.size(); ++rot) {
                    Polygon poly(pts);
                    std::rotate(poly.points.begin(), poly.points.begin() + rot, poly.points.end());
                    Polylines inside_out, outside_out;
                    ClipperUtils::clip_closed_polygon_split(poly, clip, inside_out, outside_out);
                    double in_len = 0., out_len = 0.;
                    for (const Polyline &pl : inside_out)
                        in_len += pl.length();
                    for (const Polyline &pl : outside_out)
                        out_len += pl.length();
                    CHECK(in_len == Catch::Approx(expected_in));
                    CHECK(out_len == Catch::Approx(expected_out));
                    CHECK(in_len + out_len == Catch::Approx(expected_in + expected_out));

                    const Polylines pl_in  = intersection_pl(Polygons{ poly }.front().split_at_first_point(), clip);
                    const Polylines pl_out = diff_pl(Polygons{ poly }.front().split_at_first_point(), clip);
                    double          pi_len = 0., po_len = 0.;
                    for (const Polyline &pl : pl_in)
                        pi_len += pl.length();
                    for (const Polyline &pl : pl_out)
                        po_len += pl.length();
                    CHECK(pi_len == Catch::Approx(expected_in));
                    CHECK(po_len == Catch::Approx(expected_out));
                }
            }
        };

        // Original 10mm square on 10mm square: Left/Bottom/Right = 30mm inside, Top = 10mm outside.
        const Polygon sq10({ { 0, 0 }, { coord_t(scale_(10.)), 0 }, { coord_t(scale_(10.)), coord_t(scale_(10.)) }, { 0, coord_t(scale_(10.)) } });
        check_loop_partition(sq10.points, Polygons{ sq10 }, scale_(30.), scale_(10.));

        // Lower-edge grazing loop [1,9]x[-5,0] on [-0.2,10.2]x[0,10.2]: 0mm inside, 26mm outside.
        const Polygon lower_clip({ { coord_t(scale_(-0.2)), 0 },
                                   { coord_t(scale_(10.2)), 0 },
                                   { coord_t(scale_(10.2)), coord_t(scale_(10.2)) },
                                   { coord_t(scale_(-0.2)), coord_t(scale_(10.2)) } });
        const Points  bot_grazing{
            { coord_t(scale_(1.)), coord_t(scale_(-5.)) },
            { coord_t(scale_(9.)), coord_t(scale_(-5.)) },
            { coord_t(scale_(9.)), 0 },
            { coord_t(scale_(1.)), 0 }
        };
        check_loop_partition(bot_grazing, Polygons{ lower_clip }, 0., scale_(26.));

        // Right-side grazing loop [10,15]x[1,9] on [0,10]^2: 0mm inside, 26mm outside.
        const Points right_grazing{
            { coord_t(scale_(10.)), coord_t(scale_(1.)) },
            { coord_t(scale_(15.)), coord_t(scale_(1.)) },
            { coord_t(scale_(15.)), coord_t(scale_(9.)) },
            { coord_t(scale_(10.)), coord_t(scale_(9.)) }
        };
        check_loop_partition(right_grazing, Polygons{ sq10 }, 0., scale_(26.));

        // Right-side pass-through over top corner: [10.2,17]x[5,11.2] on [-0.2,10.2]^2 -> 5.2mm inside (left x=10.2 up to y=10.2), 20.8mm outside.
        const Polygon lower_grown({ { coord_t(scale_(-0.2)), coord_t(scale_(-0.2)) },
                                    { coord_t(scale_(10.2)), coord_t(scale_(-0.2)) },
                                    { coord_t(scale_(10.2)), coord_t(scale_(10.2)) },
                                    { coord_t(scale_(-0.2)), coord_t(scale_(10.2)) } });
        const Points right_top_corner{
            { coord_t(scale_(10.2)), coord_t(scale_(5.)) },
            { coord_t(scale_(17.)), coord_t(scale_(5.)) },
            { coord_t(scale_(17.)), coord_t(scale_(11.2)) },
            { coord_t(scale_(10.2)), coord_t(scale_(11.2)) }
        };
        check_loop_partition(right_top_corner, Polygons{ lower_grown }, scale_(5.2), scale_(20.8));

        // Verify exact supported endpoints (10.2, 5.0) <-> (10.2, 10.2).
        Polylines corner_in, corner_out;
        ClipperUtils::clip_closed_polygon_split(Polygon(right_top_corner), Polygons{ lower_grown }, corner_in, corner_out);
        REQUIRE(corner_in.size() == 1);
        CHECK(corner_in.front().length() == Catch::Approx(double(scale_(5.2))));
        const BoundingBox ci_bb(corner_in.front().points);
        CHECK(ci_bb.min.x() == coord_t(scale_(10.2)));
        CHECK(ci_bb.max.x() == coord_t(scale_(10.2)));
        CHECK(ci_bb.min.y() == coord_t(scale_(5.)));
        CHECK(ci_bb.max.y() == coord_t(scale_(10.2)));

        // Left-side spanning past top & bottom corners: [-8, -0.2]x[-5, 15] on [-0.2,10.2]^2 -> 0mm inside, 55.6mm outside.
        const Points left_spanning{
            { coord_t(scale_(-8.)), coord_t(scale_(-5.)) },
            { coord_t(scale_(-0.2)), coord_t(scale_(-5.)) },
            { coord_t(scale_(-0.2)), coord_t(scale_(15.)) },
            { coord_t(scale_(-8.)), coord_t(scale_(15.)) }
        };
        check_loop_partition(left_spanning, Polygons{ lower_grown }, 0., scale_(55.6));

        // Rotated (+36.87 deg and -36.87 deg) grazing loops: 0mm inside, 26mm outside.
        auto rot_pts = [](const Points &in, double c, double s) -> Points {
            Points out;
            out.reserve(in.size());
            for (const Point &p : in)
                out.emplace_back(coord_t(std::llround(c * p.x() - s * p.y())),
                                 coord_t(std::llround(s * p.x() + c * p.y())));
            return out;
        };
        for (double s : { 0.6, -0.6 }) {
            const double exp_in  = (s > 0.) ? 0. : scale_(8.);
            const double exp_out = (s > 0.) ? scale_(26.) : scale_(18.);
            check_loop_partition(rot_pts(bot_grazing, 0.8, s), Polygons{ Polygon(rot_pts(lower_clip.points, 0.8, s)) }, exp_in, exp_out);
        }

        // Clip polygon containing a CW hole grazed along hole top (Clipper1 NonZero bottom edge -> inside)
        // and hole bottom (Clipper1 top edge -> outside).
        const Polygon outer_clip({ { 0, 0 }, { coord_t(scale_(20.)), 0 }, { coord_t(scale_(20.)), coord_t(scale_(20.)) }, { 0, coord_t(scale_(20.)) } });
        const Polygon hole_clip({ { coord_t(scale_(5.)), coord_t(scale_(5.)) }, { coord_t(scale_(5.)), coord_t(scale_(15.)) }, { coord_t(scale_(15.)), coord_t(scale_(15.)) }, { coord_t(scale_(15.)), coord_t(scale_(5.)) } });
        const Points  hole_graze_top{
            { coord_t(scale_(6.)), coord_t(scale_(6.)) },
            { coord_t(scale_(14.)), coord_t(scale_(6.)) },
            { coord_t(scale_(14.)), coord_t(scale_(15.)) },
            { coord_t(scale_(6.)), coord_t(scale_(15.)) }
        };
        {
            Polylines hg_in, hg_out;
            ClipperUtils::clip_closed_polygon_split(Polygon(hole_graze_top), Polygons{ outer_clip, hole_clip }, hg_in, hg_out);
            double hi = 0., ho = 0.;
            for (const Polyline &pl : hg_in)
                hi += pl.length();
            for (const Polyline &pl : hg_out)
                ho += pl.length();
            CHECK(hi + ho == Catch::Approx(double(scale_(34.))));
            CHECK(hi == Catch::Approx(double(scale_(8.))));
            CHECK(ho == Catch::Approx(double(scale_(26.))));
        }
        const Points hole_graze_bottom{
            { coord_t(scale_(6.)), coord_t(scale_(5.)) },
            { coord_t(scale_(14.)), coord_t(scale_(5.)) },
            { coord_t(scale_(14.)), coord_t(scale_(14.)) },
            { coord_t(scale_(6.)), coord_t(scale_(14.)) }
        };
        {
            Polylines hg_in, hg_out;
            ClipperUtils::clip_closed_polygon_split(Polygon(hole_graze_bottom), Polygons{ outer_clip, hole_clip }, hg_in, hg_out);
            double hi = 0., ho = 0.;
            for (const Polyline &pl : hg_in)
                hi += pl.length();
            for (const Polyline &pl : hg_out)
                ho += pl.length();
            CHECK(hi + ho == Catch::Approx(double(scale_(34.))));
            CHECK(hi == Catch::Approx(0.));
            CHECK(ho == Catch::Approx(double(scale_(34.))));
        }
    }

    SECTION("Long zigzag polyline across rectangle preserves all crossings and total length")
    {
        Polyline zigzag;
        const size_t segs = 4096;
        zigzag.points.reserve(segs + 1);
        for (size_t i = 0; i <= segs; ++i)
            zigzag.points.emplace_back(coord_t(scale_((i % 2 == 0) ? -1. : 1.)), coord_t(scale_(double(i) * 0.02)));
        const Polygon rect({ { 0, coord_t(scale_(-1.)) }, { coord_t(scale_(2.)), coord_t(scale_(-1.)) }, { coord_t(scale_(2.)), coord_t(scale_(100.)) }, { 0, coord_t(scale_(100.)) } });
        const Polylines in_res  = intersection_pl(zigzag, Polygons{ rect });
        const Polylines out_res = diff_pl(zigzag, Polygons{ rect });
        CHECK(in_res.size() == 2048);
        CHECK(out_res.size() == 2049);
        double in_len = 0., out_len = 0.;
        for (const Polyline &pl : in_res)
            in_len += pl.length();
        for (const Polyline &pl : out_res)
            out_len += pl.length();
        CHECK(in_len + out_len == Catch::Approx(zigzag.length()));
    }

    SECTION("Four-point path along clip top preserves vertices while excluding shared top edge")
    {
        const Polygon box({ { 0, coord_t(scale_(-10.)) },
                            { coord_t(scale_(10.)), coord_t(scale_(-10.)) },
                            { coord_t(scale_(10.)), 0 },
                            { 0, 0 } });
        for (bool rev : { false, true }) {
            Polyline subj{
                { coord_t(scale_(1.)), coord_t(scale_(-5.)) },
                { coord_t(scale_(2.)), 0 },
                { coord_t(scale_(8.)), 0 },
                { coord_t(scale_(9.)), coord_t(scale_(-5.)) }
            };
            if (rev)
                subj.reverse();
            const Polylines inside  = intersection_pl(subj, Polygons{ box });
            const Polylines outside = diff_pl(subj, Polygons{ box });
            REQUIRE(inside.size() == 2);
            REQUIRE(outside.size() == 1);
            CHECK(outside.front().length() == Catch::Approx(scale_(6.)));
        }
    }
}

TEST_CASE("Open clipping preserves rounded scan columns and boundary insertion order", "[ClipperUtils]")
{
    using namespace ClipperUtils;
    SECTION("A wall crosses the exact edge but both ends occupy its integer scan column")
    {
        const Points                      wall{{2520001, 8640001}, {280001, 959999}, {-4520001, 2359999}, {-2280001, 10040001}};
        const ClipEdgesIndex              index(Polygons{Polygon{{2856001, 9792000}, {-55999, -192000}, {9736000, -3048000}}});
        std::vector<OpenClipPathInterval> intervals;
        index.clip_path(wall, true, OpenClipBoundaryRule::Clipper1ExcludeTop, intervals);
        REQUIRE_FALSE(intervals.empty());
        CHECK(intervals.front().seg_idx == 0);
        CHECK(intervals.front().inside);
        CHECK(intervals.front().p0 == wall[0]);
        CHECK(intervals.front().p1 == wall[1]);
    }

    SECTION("Rotated walls sharing a rounded column at one end keep their supported lengths")
    {
        auto check = [](const Points& wall, const Polygon& support, double supported_mm) {
            Polylines supported, overhang;
            clip_closed_polygon_split(Polygon(wall), Polygons{support}, supported, overhang);
            double supported_length = 0., overhang_length = 0.;
            for (const auto& line : supported)
                supported_length += line.length();
            for (const auto& line : overhang)
                overhang_length += line.length();
            CHECK(std::abs(unscale<double>(supported_length) - supported_mm) < 0.00001);
            CHECK(std::abs(unscale<double>(supported_length + overhang_length) - 26.) < 0.00002);
        };
        check({{10276802, -622401}, {7588799, -4838402}, {843198, -537599}, {3531201, 3678402}},
              Polygon{{8600640, -5483520}, {-168640, 107520}, {-5652160, -8493120}}, 7.842263);
        check({{8647059, 5588234}, {4882352, -1470588}, {470588, 882354}, {4235295, 7941177}},
              Polygon{{4799999, 9000001}, {-4200000, 13800000}, {-94118, -176470}}, 0.);
    }

    SECTION("A near-coincident edge preserves separate crossings on the same scanbeam")
    {
        using XRange = std::pair<coord_t, coord_t>;
        for (coord_t shift : {-1, 1}) {
            Polygon near{{shift, 0}, {50000000 + shift, 1}, {50000000, -1500000}, {0, -1500000}};
            near.make_counter_clockwise();
            for (coord_t x : {10000000, 30000000, 60000000}) {
                const coord_t       right = x + 10000000;
                const Polygons      clip{near, Polygon{{x, -10000}, {right, -10000}, {right, 10000}, {x, 10000}}};
                std::vector<XRange> expected;
                if (shift < 0)
                    expected.emplace_back(0, 49999999);
                if (shift > 0 || x > 50000000)
                    expected.emplace_back(x, right);

                Polyline line{{0, 0}, {100000000, 2}};
                for (bool reverse : {false, true}) {
                    CAPTURE(shift, x, reverse);
                    if (reverse)
                        line.reverse();
                    std::vector<XRange> actual;
                    for (const Polyline& part : intersection_pl(line, clip))
                        actual.push_back(std::minmax(part.first_point().x(), part.last_point().x()));
                    std::sort(actual.begin(), actual.end());
                    CHECK(actual == expected);
                }
            }
        }
    }

    SECTION("A shallow crossing keeps its intersection when translated across the print bed")
    {
        for (const coord_t y : {0, 150000000, 250000000}) {
            const Polygon region{{0, y}, {100000000, y + 2}, {100000000, y + 1000000}, {0, y + 1000000}};
            for (const coord_t start_offset : {7, 16}) {
                const coord_t end_offset = start_offset == 7 ? 2 : 4;
                const Point   cut(start_offset == 7 ? 77777778 : 80000000, y + 2);
                Polyline      line{{-start_offset, y}, {100000000 + end_offset, y + 2}};
                for (bool reverse : {false, true}) {
                    if (reverse)
                        line.reverse();
                    const Polylines inside = intersection_pl(line, Polygons{region});
                    REQUIRE(inside.size() == 1);
                    CHECK(inside.front().first_point() == (reverse ? cut : Point(0, y)));
                    CHECK(inside.front().last_point() == (reverse ? Point(0, y) : cut));
                }
            }
        }
    }

    SECTION("Clipping a single segment agrees with clipping a two-point path")
    {
        const ClipEdgesIndex index(Polygons{Polygon{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}}});
        for (const auto rule :
             {OpenClipBoundaryRule::Inclusive, OpenClipBoundaryRule::Clipper1ExcludeTop, OpenClipBoundaryRule::Clipper1ClipFirst}) {
            for (coord_t y : {0, 500, 1000}) {
                const Points                      line{{200, y}, {800, y}};
                std::vector<OpenClipInterval>     segment;
                std::vector<OpenClipPathInterval> path;
                index.clip_segment(line.front(), line.back(), rule, segment);
                index.clip_path(line, false, rule, path);
                REQUIRE(segment.size() == 1);
                REQUIRE(path.size() == 1);
                CHECK(segment[0].inside == path[0].inside);
                CHECK(segment[0].p0 == path[0].p0);
                CHECK(segment[0].p1 == path[0].p1);
            }
        }
    }
}

TEST_CASE("Short open paths graze a densely tessellated contour", "[ClipperUtils]")
{
    constexpr int vertices = 8192;
    Points        ring;
    for (int i = 0; i < vertices; ++i) {
        const double angle = 2. * PI * i / vertices;
        ring.emplace_back(coord_t(std::llround(50000000. * std::cos(angle))), coord_t(std::llround(50000000. * std::sin(angle))));
    }
    Polylines lines;
    for (int i = 0; i < vertices; i += 32)
        lines.emplace_back(Polyline{ring[i], ring[i + 1]});
    const Polylines clipped = intersection_pl(lines, Polygons{Polygon(ring)});
    REQUIRE(clipped.size() == 128);
    double length = 0.;
    for (const Polyline& line : clipped) {
        REQUIRE(line.size() == 2);
        length += line.length();
    }
    CHECK(length * SCALING_FACTOR == Catch::Approx(4.90873552).margin(1.e-7));
}

TEST_CASE("Unrelated open paths do not change each other's shared-edge ownership", "[ClipperUtils]")
{
    Polygons  boxes;
    Polylines edges;
    for (int i = 0; i < 17; ++i) {
        const coord_t x = i * 1200000, y = i * 379;
        boxes.emplace_back(Polygon{{x, y}, {x + 500000, y}, {x + 500000, y + 500000}, {x, y + 500000}});
        edges.emplace_back(Polyline{{x + 50000, y + 500000}, {x + 450000, y + 500000}});
    }
    for (int order = 0; order < 2; ++order) {
        CHECK(intersection_pl(edges, boxes).empty());
        const Polylines outside = diff_pl(edges, boxes);
        REQUIRE(outside.size() == edges.size());
        for (size_t i = 0; i < edges.size(); ++i)
            CHECK(outside[i].points == edges[i].points);
        std::reverse(edges.begin(), edges.end());
    }
}

TEST_CASE("MR13 negative offset hole isolation and offset2 preservation", "[ClipperUtils]")
{
    SECTION("Island inside hole of vanishing outer frame survives offset_ex and offset2_ex")
    {
        ExPolygon frame(Polygon{ { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 0, 1000 } });
        frame.holes.push_back(Polygon{ { 100, 100 }, { 100, 900 }, { 900, 900 }, { 900, 100 } });
        ExPolygon inner_island(Polygon{ { 300, 300 }, { 700, 300 }, { 700, 700 }, { 300, 700 } });

        const ExPolygons shrunk = offset_ex(ExPolygons{ frame, inner_island }, -120.f);
        REQUIRE(shrunk.size() == 1);
        CHECK(shrunk.front().area() == Catch::Approx(160. * 160.));

        const ExPolygons shrunk2 = offset2_ex(ExPolygons{ frame, inner_island }, 10.f, -130.f);
        REQUIRE(shrunk2.size() == 1);
        CHECK(shrunk2.front().area() == Catch::Approx(160. * 160.));
    }

    SECTION("Concave holeless polygon negative offset never deletes or chips nearby independent island")
    {
        const Polygon concave_a({
            { 0, 0 },
            { coord_t(scale_(10.)), 0 },
            { coord_t(scale_(10.)), coord_t(scale_(10.)) },
            { 0, coord_t(scale_(10.)) },
            { 0, coord_t(scale_(7.25)) },
            { coord_t(scale_(4.8)), coord_t(scale_(7.25)) },
            { coord_t(scale_(9.95)), coord_t(scale_(5.)) },
            { coord_t(scale_(4.8)), coord_t(scale_(2.75)) },
            { 0, coord_t(scale_(2.75)) }
        });
        const Polygon island_b({
            { coord_t(scale_(10.05)), coord_t(scale_(3.85)) },
            { coord_t(scale_(12.35)), coord_t(scale_(3.85)) },
            { coord_t(scale_(12.35)), coord_t(scale_(6.15)) },
            { coord_t(scale_(10.05)), coord_t(scale_(6.15)) }
        });

        for (bool swap_order : { false, true }) {
            const ExPolygons input = swap_order
                ? ExPolygons{ ExPolygon(island_b), ExPolygon(concave_a) }
                : ExPolygons{ ExPolygon(concave_a), ExPolygon(island_b) };
            const ExPolygons res = offset_ex(input, -float(scale_(1.)), jtMiter, 3.0);
            REQUIRE(res.size() == 3);

            bool   found_b = false;
            double min_b_area_err = 1e30;
            for (const ExPolygon &ex : res) {
                const BoundingBox bb = get_extents(ex);
                const Point       ctr = bb.center();
                if (std::abs(ctr.x() - scale_(11.2)) < scale_(0.05) &&
                    std::abs(ctr.y() - scale_(5.0)) < scale_(0.05)) {
                    found_b        = true;
                    min_b_area_err = std::abs(ex.area() - scale_(0.3) * scale_(0.3));
                    CHECK(ex.area() == Catch::Approx(scale_(0.3) * scale_(0.3)).epsilon(1e-3));
                }
            }
            CHECK(found_b);
            CHECK(min_b_area_err < scale_(0.001) * scale_(0.001));

            const Polygons poly_res = offset(swap_order ? Polygons{ island_b, concave_a } : Polygons{ concave_a, island_b },
                                             -float(scale_(1.)), jtMiter, 3.0);
            CHECK(poly_res.size() == 3);
        }

        // Verify a larger neighbor is neither deleted nor partially clipped.
        const Polygon large_b({
            { coord_t(scale_(10.05)), coord_t(scale_(2.5)) },
            { coord_t(scale_(15.05)), coord_t(scale_(2.5)) },
            { coord_t(scale_(15.05)), coord_t(scale_(7.5)) },
            { coord_t(scale_(10.05)), coord_t(scale_(7.5)) }
        });
        const ExPolygons solo_b   = offset_ex(ExPolygons{ ExPolygon(large_b) }, -float(scale_(1.)), jtMiter, 3.0);
        const ExPolygons pair_res = offset_ex(ExPolygons{ ExPolygon(concave_a), ExPolygon(large_b) }, -float(scale_(1.)), jtMiter, 3.0);
        REQUIRE(pair_res.size() == 3);
        ExPolygons right_parts;
        for (const ExPolygon &ex : pair_res)
            if (get_extents(ex).center().x() > scale_(10.))
                right_parts.push_back(ex);
        REQUIRE(right_parts.size() == 1);
        CHECK(right_parts.front().area() == Catch::Approx(scale_(3.) * scale_(3.)));
        CHECK(xor_ex(right_parts, solo_b).empty());
    }

    SECTION("Multi-hole positive offset preserves small adjacent hole and merges touching entities")
    {
        Polygon a_cw({
            { 0, 0 },
            { coord_t(scale_(10.)), 0 },
            { coord_t(scale_(10.)), coord_t(scale_(10.)) },
            { 0, coord_t(scale_(10.)) },
            { 0, coord_t(scale_(7.25)) },
            { coord_t(scale_(4.8)), coord_t(scale_(7.25)) },
            { coord_t(scale_(9.95)), coord_t(scale_(5.)) },
            { coord_t(scale_(4.8)), coord_t(scale_(2.75)) },
            { 0, coord_t(scale_(2.75)) }
        });
        a_cw.reverse();
        Polygon b_cw({
            { coord_t(scale_(10.05)), coord_t(scale_(3.85)) },
            { coord_t(scale_(12.35)), coord_t(scale_(3.85)) },
            { coord_t(scale_(12.35)), coord_t(scale_(6.15)) },
            { coord_t(scale_(10.05)), coord_t(scale_(6.15)) }
        });
        b_cw.reverse();

        ExPolygon frame(Polygon{
            { coord_t(scale_(-5.)), coord_t(scale_(-5.)) },
            { coord_t(scale_(30.)), coord_t(scale_(-5.)) },
            { coord_t(scale_(30.)), coord_t(scale_(15.)) },
            { coord_t(scale_(-5.)), coord_t(scale_(15.)) }
        });
        frame.holes.push_back(a_cw);
        frame.holes.push_back(b_cw);

        const ExPolygons grown = offset_ex(ExPolygons{ frame }, float(scale_(1.)), jtMiter, 3.0);
        REQUIRE(grown.size() == 1);
        CHECK(grown.front().holes.size() == 3);

        // Build exact independent reference: expanded outer minus separately shrunk holes.
        Polygon a_ccw = a_cw;
        a_ccw.reverse();
        Polygon b_ccw = b_cw;
        b_ccw.reverse();
        Polygons holes_ref = offset(a_ccw, -float(scale_(1.)), jtMiter, 3.0);
        Polygons b_ref     = offset(b_ccw, -float(scale_(1.)), jtMiter, 3.0);
        REQUIRE(b_ref.size() == 1);
        CHECK(b_ref.front().area() == Catch::Approx(scale_(0.3) * scale_(0.3)).epsilon(1e-3));
        holes_ref.insert(holes_ref.end(), b_ref.begin(), b_ref.end());
        const ExPolygons ref = diff_ex(offset_ex(frame.contour, float(scale_(1.)), jtMiter, 3.0), holes_ref);
        CHECK(xor_ex(grown, ref).empty());

        const ExPolygons poly_grown = offset_ex(Polygons{ frame.contour, a_cw, b_cw }, float(scale_(1.)), jtMiter, 3.0);
        REQUIRE(poly_grown.size() == 1);
        CHECK(poly_grown.front().holes.size() == 3);
        CHECK(xor_ex(poly_grown, ref).empty());

        // Two adjacent holed entities whose outer boundaries merge when grown by +1 mm.
        ExPolygon e1(Polygon{ { 0, 0 }, { coord_t(scale_(10.)), 0 }, { coord_t(scale_(10.)), coord_t(scale_(10.)) }, { 0, coord_t(scale_(10.)) } });
        e1.holes.push_back(Polygon{ { coord_t(scale_(3.)), coord_t(scale_(7.)) }, { coord_t(scale_(7.)), coord_t(scale_(7.)) }, { coord_t(scale_(7.)), coord_t(scale_(3.)) }, { coord_t(scale_(3.)), coord_t(scale_(3.)) } });
        ExPolygon e2(Polygon{ { coord_t(scale_(11.)), 0 }, { coord_t(scale_(21.)), 0 }, { coord_t(scale_(21.)), coord_t(scale_(10.)) }, { coord_t(scale_(11.)), coord_t(scale_(10.)) } });
        e2.holes.push_back(Polygon{ { coord_t(scale_(14.)), coord_t(scale_(7.)) }, { coord_t(scale_(18.)), coord_t(scale_(7.)) }, { coord_t(scale_(18.)), coord_t(scale_(3.)) }, { coord_t(scale_(14.)), coord_t(scale_(3.)) } });
        const ExPolygons merged_grown = offset_ex(ExPolygons{ e1, e2 }, float(scale_(1.)), jtMiter, 2.0);
        REQUIRE(merged_grown.size() == 1);
        CHECK(merged_grown.front().holes.size() == 2);

        const ExPolygons merged_flat = offset_ex(Polygons{ e1.contour, e1.holes.front(), e2.contour, e2.holes.front() }, float(scale_(1.)), jtMiter, 2.0);
        CHECK(xor_ex(merged_grown, merged_flat).empty());

        // P1 #3 flat vs structured counterexample: concave CW hole near outer frame edge must never bulge outer frame.
        ExPolygon tight_frame(Polygon{
            { coord_t(scale_(-15.)), coord_t(scale_(-0.4)) },
            { coord_t(scale_(10.4)), coord_t(scale_(-0.4)) },
            { coord_t(scale_(10.4)), coord_t(scale_(10.4)) },
            { coord_t(scale_(-15.)), coord_t(scale_(10.4)) }
        });
        Polygon sq_hole({
            { coord_t(scale_(-12.)), coord_t(scale_(3.)) },
            { coord_t(scale_(-12.)), coord_t(scale_(6.)) },
            { coord_t(scale_(-9.5)), coord_t(scale_(6.)) },
            { coord_t(scale_(-9.5)), coord_t(scale_(3.)) }
        });
        tight_frame.holes.push_back(a_cw);
        tight_frame.holes.push_back(sq_hole);

        const ExPolygons tight_ex = offset_ex(ExPolygons{ tight_frame }, float(scale_(1.)), jtMiter, 3.0);
        REQUIRE(tight_ex.size() == 1);
        CHECK(tight_ex.front().holes.size() == 3);
        CHECK(get_extents(tight_ex).max.x() == coord_t(scale_(11.4)));

        for (bool swap_holes : { false, true }) {
            const Polygons flat_in = swap_holes
                ? Polygons{ tight_frame.contour, sq_hole, a_cw }
                : Polygons{ tight_frame.contour, a_cw, sq_hole };
            const ExPolygons tight_flat = offset_ex(flat_in, float(scale_(1.)), jtMiter, 3.0);
            REQUIRE(tight_flat.size() == 1);
            CHECK(tight_flat.front().holes.size() == 3);
            CHECK(get_extents(tight_flat).max.x() == coord_t(scale_(11.4)));
            CHECK(xor_ex(tight_flat, tight_ex).empty());
        }

        // Nested island inside a hole: both structured ExPolygons and flat Polygons agree for +1mm and -1mm.
        ExPolygon outer_ring(Polygon{ { 0, 0 }, { coord_t(scale_(30.)), 0 }, { coord_t(scale_(30.)), coord_t(scale_(30.)) }, { 0, coord_t(scale_(30.)) } });
        outer_ring.holes.push_back(Polygon{ { coord_t(scale_(5.)), coord_t(scale_(25.)) }, { coord_t(scale_(25.)), coord_t(scale_(25.)) }, { coord_t(scale_(25.)), coord_t(scale_(5.)) }, { coord_t(scale_(5.)), coord_t(scale_(5.)) } });
        ExPolygon nested_island(Polygon{ { coord_t(scale_(10.)), coord_t(scale_(10.)) }, { coord_t(scale_(20.)), coord_t(scale_(10.)) }, { coord_t(scale_(20.)), coord_t(scale_(20.)) }, { coord_t(scale_(10.)), coord_t(scale_(20.)) } });
        const Polygons nested_flat{ outer_ring.contour, outer_ring.holes.front(), nested_island.contour };
        for (float d : { float(scale_(1.)), -float(scale_(1.)) }) {
            const ExPolygons nested_ex_res   = offset_ex(ExPolygons{ outer_ring, nested_island }, d, jtMiter, 2.0);
            const ExPolygons nested_flat_res = offset_ex(nested_flat, d, jtMiter, 2.0);
            REQUIRE(nested_ex_res.size() == 2);
            CHECK(xor_ex(nested_ex_res, nested_flat_res).empty());
        }

        // Lone CW polygon under positive offset follows NonZero semantics (contracts into positive offset ref).
        {
            Polygon cw_sq({ { 0, 0 }, { 0, coord_t(scale_(10.)) }, { coord_t(scale_(10.)), coord_t(scale_(10.)) }, { coord_t(scale_(10.)), 0 } });
            const Polygons cw_res = offset(Polygons{ cw_sq }, float(scale_(1.)), jtMiter, 2.0);
            REQUIRE(cw_res.size() == 1);
            CHECK(cw_res.front().area() == Catch::Approx(scale_(8.) * scale_(8.)));
        }

        // Clean-convex fast-batch grid verification (holed squares +0.05mm, solid squares -0.1mm).
        {
            ExPolygons grid_holed;
            Polygons   grid_solid;
            for (int iy = 0; iy < 10; ++iy) {
                for (int ix = 0; ix < 10; ++ix) {
                    const coord_t ox = coord_t(scale_(ix * 1.2));
                    const coord_t oy = coord_t(scale_(iy * 1.2));
                    ExPolygon cell(Polygon{
                        { ox, oy },
                        { ox + coord_t(scale_(0.5)), oy },
                        { ox + coord_t(scale_(0.5)), oy + coord_t(scale_(0.5)) },
                        { ox, oy + coord_t(scale_(0.5)) }
                    });
                    grid_solid.push_back(cell.contour);
                    cell.holes.push_back(Polygon{
                        { ox + coord_t(scale_(0.1)), oy + coord_t(scale_(0.4)) },
                        { ox + coord_t(scale_(0.4)), oy + coord_t(scale_(0.4)) },
                        { ox + coord_t(scale_(0.4)), oy + coord_t(scale_(0.1)) },
                        { ox + coord_t(scale_(0.1)), oy + coord_t(scale_(0.1)) }
                    });
                    grid_holed.push_back(std::move(cell));
                }
            }
            const ExPolygons holed_out = offset_ex(grid_holed, float(scale_(0.05)), jtMiter, 2.0);
            REQUIRE(holed_out.size() == 100);
            double hole_area_sum = 0.;
            for (const ExPolygon &ex : holed_out) {
                REQUIRE(ex.holes.size() == 1);
                hole_area_sum += std::abs(ex.holes.front().area());
            }
            CHECK(hole_area_sum == Catch::Approx(100. * scale_(0.2) * scale_(0.2)));

            const Polygons solid_out = offset(grid_solid, -float(scale_(0.1)), jtMiter, 2.0);
            REQUIRE(solid_out.size() == 100);
            double solid_area_sum = 0.;
            for (const Polygon &p : solid_out)
                solid_area_sum += p.area();
            CHECK(solid_area_sum == Catch::Approx(100. * scale_(0.3) * scale_(0.3)));
        }
    }

    SECTION("Standalone bowtie negative offset keeps positive lobe while collinear vanishes")
    {
        const Polygon bow_tie({ {0, 0}, {200, 200}, {200, 0}, {0, 200} });
        const Polygons bt_res = offset(bow_tie, -10.f);
        REQUIRE(bt_res.size() == 1);
        CHECK(bt_res.front().area() == Catch::Approx(5776.).margin(20.));

        const Polygon collinear({ {0, 0}, {200, 0}, {100, 0} });
        CHECK(offset(collinear, -10.f).empty());
    }
}

TEST_CASE("MR13 simplify_polygons_ex self-touching vertex-on-edge and near-boundary holes", "[ClipperUtils]")
{
    SECTION("Fixture 1: Emboss heal_polygons and simplify_polygons_ex separate pinch")
    {
        const Polygon p({ { 0, 0 }, { 5, 0 }, { 1, 5 }, { 5, 5 }, { 5, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        const ExPolygons simp = simplify_polygons_ex(Polygons{ p });
        double total_a = 0.;
        size_t total_holes = 0;
        for (const ExPolygon &ex : simp) {
            total_a += ex.area();
            total_holes += ex.holes.size();
        }
        CHECK(total_a == Catch::Approx(90.));

        const auto healed = Emboss::heal_polygons(Polygons{ p }, true, 10);
        CHECK(healed.is_healed);
        REQUIRE(healed.expolygons.size() == 1);
        CHECK(healed.expolygons.front().holes.size() == 1);
        double healed_a = 0.;
        for (const ExPolygon &ex : healed.expolygons)
            healed_a += ex.area();
        CHECK(healed_a == Catch::Approx(92.));
    }

    SECTION("Fixture 2: Overlapping triangles touching at base vertex")
    {
        const Polygon t1({ { 3, 2 }, { 3, 0 }, { 1, 0 } });
        const Polygon t2({ { 2, 0 }, { 4, 3 }, { 2, 3 } });
        const ExPolygons simp = simplify_polygons_ex(Polygons{ t1, t2 });
        REQUIRE(! simp.empty());
        double total_a = 0.;
        for (const ExPolygon &ex : simp)
            total_a += ex.area();
        const ExPolygons u_ex = union_ex(Polygons{ t1, t2 });
        double expected_a = 0.;
        for (const ExPolygon &ex : u_ex)
            expected_a += ex.area();
        CHECK(total_a == Catch::Approx(expected_a));
    }

    SECTION("Near-boundary hole within 1-3 scaled units is preserved during hole re-nesting")
    {
        const Polygon outer({ { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 500, 1000 }, { 500, 1 }, { 500, 1000 }, { 0, 1000 } });
        Polygon hole({ { 1, 1 }, { 1, 999 }, { 499, 999 }, { 499, 1 } });
        const ExPolygons simp = simplify_polygons_ex(Polygons{ outer, hole });
        double total_a = 0.;
        size_t hole_cnt = 0;
        for (const ExPolygon &ex : simp) {
            total_a += ex.area();
            hole_cnt += ex.holes.size();
        }
        CHECK(hole_cnt >= 1);
        CHECK(total_a == Catch::Approx(1000. * 1000. - 498. * 998.).margin(10.));
    }

    SECTION("Concentric 256-vertex rings positive miter offset correctness")
    {
        const size_t n = 256;
        Polygon outer, inner;
        for (size_t i = 0; i < n; ++i) {
            const double ang = 2. * M_PI * double(i) / double(n);
            outer.points.emplace_back(coord_t(std::llround(scale_(10.0) * std::cos(ang))),
                                      coord_t(std::llround(scale_(10.0) * std::sin(ang))));
            inner.points.emplace_back(coord_t(std::llround(scale_(9.9) * std::cos(-ang))),
                                      coord_t(std::llround(scale_(9.9) * std::sin(-ang))));
        }
        ExPolygon ring(outer);
        ring.holes.push_back(inner);
        const ExPolygons res = offset_ex(ExPolygons{ ring }, float(scale_(0.01)), jtMiter, 2.0);
        REQUIRE(res.size() == 1);
        CHECK(res.front().holes.size() == 1);
    }
}

TEST_CASE("ClipperUtils MR13 benchmarks", "[ClipperUtils][.mr13_bench]")
{
    auto median_ms = [](auto &&fn) {
        fn(); // warmup
        std::array<double, 3> ts{};
        for (double &t : ts) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const auto t1 = std::chrono::steady_clock::now();
            t = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        std::sort(ts.begin(), ts.end());
        return ts[1];
    };

    for (size_t n : { size_t(2048), size_t(8192), size_t(16384) }) {
        Polygon outer, inner;
        outer.points.reserve(n);
        inner.points.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const double ang = 2. * M_PI * double(i) / double(n);
            outer.points.emplace_back(coord_t(std::llround(scale_(10.0) * std::cos(ang))),
                                      coord_t(std::llround(scale_(10.0) * std::sin(ang))));
            inner.points.emplace_back(coord_t(std::llround(scale_(9.9) * std::cos(-ang))),
                                      coord_t(std::llround(scale_(9.9) * std::sin(-ang))));
        }
        ExPolygon ring(std::move(outer));
        ring.holes.push_back(std::move(inner));
        const Polygons flat_input{ ring.contour, ring.holes.front() };
        ExPolygons     input{ std::move(ring) };
        size_t         res_sz      = 0;
        size_t         flat_res_sz = 0;
        const double   ms_ex = median_ms([&]() {
            const ExPolygons out = offset_ex(input, float(scale_(0.01)), jtMiter, 2.0);
            res_sz               = out.size();
        });
        const double ms_flat = median_ms([&]() {
            const Polygons out = offset(flat_input, float(scale_(0.01)), jtMiter, 2.0);
            flat_res_sz        = out.size();
        });
        CHECK(res_sz == 1);
        CHECK(flat_res_sz == 2);
        std::cout << "[MR13_BENCH] concentric_rings n=" << n << " ex_median_ms=" << ms_ex << " flat_median_ms=" << ms_flat << "\n";
    }

    {
        ExPolygons islands;
        islands.reserve(14400);
        for (int iy = 0; iy < 120; ++iy) {
            for (int ix = 0; ix < 120; ++ix) {
                const coord_t x0 = coord_t(ix * 1000);
                const coord_t y0 = coord_t(iy * 1000);
                islands.emplace_back(Polygon{ { x0, y0 }, { x0 + 500, y0 }, { x0 + 500, y0 + 500 }, { x0, y0 + 500 } });
            }
        }
        size_t out_cnt = 0;
        const double ms = median_ms([&]() {
            const ExPolygons out = offset_ex(islands, -50.f);
            out_cnt = out.size();
        });
        CHECK(out_cnt == 14400);
        std::cout << "[MR13_BENCH] holeless_14400_shrink count=" << out_cnt << " median_ms=" << ms << "\n";
    }

    {
        const Polygon p({ { 0, 0 }, { 5, 0 }, { 1, 5 }, { 5, 5 }, { 5, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
        const auto healed = Emboss::heal_polygons(Polygons{ p }, true, 10);
        size_t total_holes = 0;
        double total_area  = 0.;
        for (const ExPolygon &ex : healed.expolygons) {
            total_holes += ex.holes.size();
            total_area  += ex.area();
        }
        std::cout << "[MR13_BENCH] emboss_heal entities=" << healed.expolygons.size()
                  << " holes=" << total_holes
                  << " area=" << total_area
                  << " is_healed=" << (healed.is_healed ? 1 : 0) << "\n";
    }
}
