#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "libslic3r/ArcFitter.hpp"
#include "libslic3r/Circle.hpp"

using namespace Slic3r;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Build a scaled Point from millimetres.
Point pt(double x_mm, double y_mm) { return Point::new_scale(x_mm, y_mm); }

// Sample n points (inclusive of both endpoints) on a circular arc, in millimetres.
Points arc_points(double cx, double cy, double radius, double start_deg, double end_deg, int n)
{
    Points pts;
    for (int i = 0; i < n; ++i) {
        double deg = start_deg + (end_deg - start_deg) * double(i) / double(n - 1);
        double rad = deg * kPi / 180.0;
        pts.emplace_back(pt(cx + radius * std::cos(rad), cy + radius * std::sin(rad)));
    }
    return pts;
}

// Number of segments arc fitting produces for a given tolerance (in mm).
size_t segment_count(const Points &pts, double tolerance_mm)
{
    std::vector<PathFittingData> result;
    ArcFitter::do_arc_fitting(pts, result, scale_(tolerance_mm));
    return result.size();
}
} // namespace

SCENARIO("ArcFitter fits sampled circle points to a single counter-clockwise arc", "[ArcFitter]") {
    GIVEN("a quarter circle sampled counter-clockwise") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 10);

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the whole polyline becomes one arc segment") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_arc_move());
                REQUIRE(result[0].path_type == EMovePathType::Arc_move_ccw);
                REQUIRE(result[0].arc_data.is_arc);
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == pts.size() - 1);
            }
            THEN("the fitted arc approximates the source circle") {
                // The circle is fit through the chord midpoint of the two middle
                // points, which sits slightly inside the circle, so the fitted
                // center and radius are a little off: center ≈ (−0.09, −0.09) mm,
                // radius ≈ 10.09 mm for this 10°-step 90° arc.
                REQUIRE_THAT(result[0].arc_data.radius, WithinRel(scale_(10.0), 0.02));
                REQUIRE(std::abs(result[0].arc_data.center.x()) <= scale_(0.2));
                REQUIRE(std::abs(result[0].arc_data.center.y()) <= scale_(0.2));
            }
        }
    }
}

SCENARIO("ArcFitter reports the correct direction for a clockwise arc", "[ArcFitter]") {
    GIVEN("a quarter circle sampled clockwise") {
        Points pts = arc_points(0.0, 0.0, 10.0, 90.0, 0.0, 10);

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the polyline becomes one clockwise arc") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].path_type == EMovePathType::Arc_move_cw);
                REQUIRE(result[0].arc_data.is_arc);
            }
        }
    }
}

SCENARIO("ArcFitter falls back to a linear move for collinear points", "[ArcFitter]") {
    GIVEN("points lying on a straight line") {
        Points pts = { pt(0.0, 0.0), pt(10.0, 0.0), pt(20.0, 0.0), pt(30.0, 0.0) };

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the whole polyline is a single linear move") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == pts.size() - 1);
            }
        }
    }
}

SCENARIO("ArcFitter handles fewer than three points", "[ArcFitter]") {
    GIVEN("zero points") {
        Points pts;

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("a degenerate linear move is emitted with an underflowed end index") {
                // Known production bug: points.size() - 1 underflows to SIZE_MAX when
                // points is empty (ArcFitter.cpp do_arc_fitting). Pin the current
                // behavior rather than silently fixing production code.
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == std::numeric_limits<size_t>::max());
            }
        }
    }
    GIVEN("a single point") {
        Points pts = { pt(5.0, 5.0) };

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the result is one degenerate linear move") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == 0);
            }
        }
    }
    GIVEN("two points") {
        Points pts = { pt(0.0, 0.0), pt(10.0, 0.0) };

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the result is one linear move spanning both points") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == 1);
            }
        }
    }
}

SCENARIO("ArcFitter yields fewer segments with a looser tolerance", "[ArcFitter]") {
    GIVEN("a circular arc whose interior points deviate from the circle") {
        // 120° arc with radius alternating 10.0 / 10.15 mm: the best-fit circle
        // lands midway (~10.075 mm), so interior points sit ~0.075 mm off it,
        // which a 0.05 mm tolerance cannot absorb.
        Points pts;
        for (int i = 0; i < 11; ++i) {
            double deg = 120.0 * double(i) / 10.0;
            double rad = deg * kPi / 180.0;
            double r   = 10.0 + (i % 2 == 0 ? 0.0 : 0.15);
            pts.emplace_back(pt(r * std::cos(rad), r * std::sin(rad)));
        }

        WHEN("the tolerance varies") {
            size_t tight = segment_count(pts, 0.05);
            size_t loose = segment_count(pts, 0.5);

            THEN("a tight tolerance splits the arc") {
                REQUIRE(tight >= 2);
            }
            THEN("a loose tolerance fits a single segment") {
                REQUIRE(loose == 1);
            }
            THEN("looser tolerance yields fewer segments") {
                REQUIRE(loose < tight);
            }
        }
    }
}

SCENARIO("ArcFitter splits a mixed arc-and-line polyline", "[ArcFitter]") {
    GIVEN("a quarter arc followed by a straight segment") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 10); // dense arc, ends at (0, 10)
        pts.emplace_back(pt(-10.0, 10.0));
        pts.emplace_back(pt(-20.0, 10.0));

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the polyline is split into arc and linear parts") {
                REQUIRE(result.size() >= 2);
                bool has_arc = false, has_line = false;
                for (auto &seg : result) { // is_arc_move()/is_linear_move() are non-const
                    has_arc  |= seg.is_arc_move();
                    has_line |= seg.is_linear_move();
                }
                REQUIRE(has_arc);
                REQUIRE(has_line);
            }
            THEN("the arc comes first and the straight line last") {
                REQUIRE(result.front().is_arc_move());
                REQUIRE(result.back().is_linear_move());
            }
        }
    }
}

SCENARIO("ArcFitter simplify collapses collinear points", "[ArcFitter]") {
    GIVEN("zero points") {
        Points pts;
        std::vector<PathFittingData> result;

        WHEN("do_arc_fitting_and_simplify is applied") {
            ArcFitter::do_arc_fitting_and_simplify(pts, result, scale_(0.05));

            THEN("a degenerate linear move is emitted with an underflowed end index") {
                // Same known production bug as do_arc_fitting (points.size() - 1
                // underflows when points is empty).
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].end_point_index == std::numeric_limits<size_t>::max());
            }
        }
    }
    GIVEN("collinear points") {
        Points pts = { pt(0.0, 0.0), pt(10.0, 0.0), pt(20.0, 0.0), pt(30.0, 0.0) };
        std::vector<PathFittingData> result;

        WHEN("do_arc_fitting_and_simplify is applied") {
            ArcFitter::do_arc_fitting_and_simplify(pts, result, scale_(0.05));

            THEN("Douglas-Peucker keeps only the two endpoints") {
                REQUIRE(pts.size() == 2);
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == 1);
            }
        }
    }
}

SCENARIO("ArcFitter simplify with zero tolerance skips arc fitting", "[ArcFitter]") {
    GIVEN("arc points and a zero tolerance") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 7);
        std::vector<PathFittingData> result;

        WHEN("do_arc_fitting_and_simplify is applied") {
            ArcFitter::do_arc_fitting_and_simplify(pts, result, 0.0);

            THEN("arc fitting is bypassed and non-collinear points are kept") {
                // tolerance == 0 skips do_arc_fitting (single linear move), and
                // Douglas-Peucker with tolerance 0 keeps every non-collinear point.
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_linear_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == 6);
                REQUIRE(pts.size() == 7);
            }
        }
    }
}

SCENARIO("ArcFitter fits three closely-spaced points to a minimal arc", "[ArcFitter]") {
    GIVEN("three non-collinear points on a shallow arc") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 10.0, 3); // 0°, 5°, 10°

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("a single counter-clockwise arc is produced") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_arc_move());
                REQUIRE(result[0].path_type == EMovePathType::Arc_move_ccw);
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == 2);
            }
        }
    }
}

SCENARIO("ArcFitter falls back to linear for sparsely sampled arcs", "[ArcFitter]") {
    GIVEN("the same quarter circle at two sampling densities") {
        Points sparse = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 5);   // 22.5° steps
        Points dense  = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 10);  // 10° steps

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> r_sparse, r_dense;
            ArcFitter::do_arc_fitting(sparse, r_sparse, scale_(0.05));
            ArcFitter::do_arc_fitting(dense, r_dense, scale_(0.05));

            THEN("sparse sampling is too coarse for the tolerance and becomes linear") {
                // The 22.5° chord sagitta (≈0.19 mm) exceeds the 0.05 mm tolerance.
                REQUIRE(r_sparse.size() == 1);
                REQUIRE(r_sparse[0].is_linear_move());
            }
            THEN("dense sampling fits a single arc") {
                REQUIRE(r_dense.size() == 1);
                REQUIRE(r_dense[0].is_arc_move());
            }
        }
    }
}

SCENARIO("ArcFitter fits an arc crossing the zero angle", "[ArcFitter]") {
    GIVEN("a counter-clockwise arc crossing 0°") {
        Points pts = arc_points(0.0, 0.0, 10.0, 350.0, 370.0, 6); // 350°→370°(=10°)

        WHEN("arc fitting is applied") {
            std::vector<PathFittingData> result;
            ArcFitter::do_arc_fitting(pts, result, scale_(0.05));

            THEN("the whole arc fits as one counter-clockwise arc") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_arc_move());
                REQUIRE(result[0].path_type == EMovePathType::Arc_move_ccw);
                REQUIRE(result[0].arc_data.is_arc);
            }
        }
    }
}

SCENARIO("ArcFitter simplify handles a mixed arc-and-line polyline", "[ArcFitter]") {
    GIVEN("a quarter arc followed by a straight segment") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 10);
        pts.emplace_back(pt(-10.0, 10.0));
        pts.emplace_back(pt(-20.0, 10.0));
        const size_t original_size = pts.size();
        std::vector<PathFittingData> result;

        WHEN("do_arc_fitting_and_simplify is applied") {
            ArcFitter::do_arc_fitting_and_simplify(pts, result, scale_(0.05));

            THEN("arc and linear parts survive and points are reduced") {
                REQUIRE(result.size() >= 2);
                bool has_arc = false, has_line = false;
                for (auto &seg : result) { // is_arc_move()/is_linear_move() are non-const
                    has_arc  |= seg.is_arc_move();
                    has_line |= seg.is_linear_move();
                }
                REQUIRE(has_arc);
                REQUIRE(has_line);
                REQUIRE(pts.size() < original_size);
            }
            THEN("indices are contiguous and consistent with the simplified points") {
                REQUIRE(result.front().start_point_index == 0);
                for (size_t i = 0; i + 1 < result.size(); ++i)
                    REQUIRE(result[i].end_point_index == result[i + 1].start_point_index);
                REQUIRE(result.back().end_point_index == pts.size() - 1);
            }
        }
    }
}

SCENARIO("ArcFitter simplify thins a densely sampled arc", "[ArcFitter]") {
    GIVEN("a densely sampled quarter arc") {
        Points pts = arc_points(0.0, 0.0, 10.0, 0.0, 90.0, 20);
        const size_t original_size = pts.size();
        std::vector<PathFittingData> result;

        WHEN("do_arc_fitting_and_simplify is applied") {
            ArcFitter::do_arc_fitting_and_simplify(pts, result, scale_(0.05));

            THEN("the arc is preserved but the points are thinned") {
                REQUIRE(result.size() == 1);
                REQUIRE(result[0].is_arc_move());
                REQUIRE(result[0].start_point_index == 0);
                REQUIRE(result[0].end_point_index == pts.size() - 1);
                REQUIRE(pts.size() < original_size);
                REQUIRE(pts.size() >= 2);
            }
        }
    }
}
