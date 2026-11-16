#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/Arachne/BeadingStrategy/BeadingStrategy.hpp"
#include "libslic3r/Arachne/BeadingStrategy/DistributedBeadingStrategy.hpp"
#include "libslic3r/Arachne/BeadingStrategy/WideningBeadingStrategy.hpp"
#include "libslic3r/Polygon.hpp"

#include <vector>

using namespace Slic3r;

namespace {

using namespace Slic3r::Arachne;

// Nominal bead width: 0.4 mm in scaled coordinates (SCALING_FACTOR = 1e-6).
// Use a plain integer constant rather than `scaled<coord_t>(0.4)` so every golden
// value below is derived with exact integer arithmetic — the beading math operates
// on coord_t integers, and off-by-one truncation from `scaled(0.4)` would only
// obscure the boundary cases being tested.
constexpr coord_t W = 400000;

// Pure-math beading strategy under test. transition length / angle do not affect
// compute()/getOptimalBeadCount(), so any values are fine for those.
BeadingStrategyPtr make_distributed(coord_t width = W)
{
    return std::make_unique<DistributedBeadingStrategy>(width, W, pi_div(3.0), 0.5, 0.5, 2);
}

// WideningBeadingStrategy wrapping a distributed strategy. min_input_width
// (min_feature_size) is BELOW the parent's 0-wall threshold (0.5W), and
// min_output_width (min_bead_width) is 0.5W — mirroring production, where
// min_feature_size < min_bead_width. This makes the "force to 1" branch of
// getOptimalBeadCount reachable: for thickness in [0.25W, 0.5W) the parent returns
// 0 and widening forces 1.
BeadingStrategyPtr make_widening()
{
    auto parent = make_distributed(W);
    return std::make_unique<WideningBeadingStrategy>(std::move(parent), W / 4, W / 2);
}

// Invariant shared by every DistributedBeadingStrategy::compute(): the sum of all
// bead widths plus left_over must reproduce the input thickness exactly (integer
// arithmetic). NOTE: WideningBeadingStrategy intentionally violates this for thin
// features (it over-extrudes up to min_output_width), so do not apply it there.
coord_t total_coverage(const BeadingStrategy::Beading &b)
{
    coord_t sum = b.left_over;
    for (coord_t width : b.bead_widths)
        sum += width;
    return sum;
}

} // namespace

SCENARIO("Arachne: WallToolPaths constructs and generates without Print", "[Arachne]")
{
    GIVEN("a 10x10mm square outline and a hand-filled WallToolPathsParams (bypassing make_paths_params)")
    {
        const coord_t S = scaled<coord_t>(10.0);
        Polygon square{ Point(0, 0), Point(S, 0), Point(S, S), Point(0, S) };
        Polygons outline{ square };

        WallToolPathsParams params;
        params.min_bead_width                 = 0.2f;  // mm
        params.min_feature_size               = 0.2f;
        params.min_length_factor              = 0.5f;
        params.wall_transition_length         = 0.4f;
        params.wall_transition_angle          = 25.0f;
        params.wall_transition_filter_deviation = 0.025f;
        params.wall_distribution_count        = 2;
        params.is_top_or_bottom_layer         = false;

        WHEN("constructing WallToolPaths from these params and calling generate() (no Print / PrintObject involved)")
        {
            WallToolPaths wtp(outline, W, W, /*inset_count=*/3, /*wall_0_inset=*/0, /*layer_height=*/0.2, params);
            const auto &toolpaths = wtp.generate();

            THEN("it generates 3 inset walls, covers only the boundary, and pins widths exactly")
            {
                // 10x10mm square, bead_width_0 == bead_width_x == W, no thinning: every
                // wall has constant width W. The derived quantities are all integer
                // (coord_t), so they are pinned exactly — no tolerance needed.
                REQUIRE(toolpaths.size() == 3); // inset count == inset_count

                for (size_t i = 0; i < toolpaths.size(); ++i) {
                    CAPTURE(i);
                    REQUIRE_FALSE(toolpaths[i].empty()); // every inset (incl. inner, inset_idx > 0) exists
                    for (const auto &line : toolpaths[i]) {
                        CHECK(line.inset_idx == i);        // numbered outer-to-inner 0/1/2
                        CHECK(line.junctions.size() >= 4); // a square has at least 4 corners
                        for (const auto &j : line.junctions) {
                            // j.w comes from the double-precision skeleton (Voronoi/medial
                            // axis) converted to coord_t, so it may be off by one
                            // cross-platform; assert the W ± 1 neighborhood instead of ==.
                            CHECK(j.w >= W - 1);
                            CHECK(j.w <= W + 1);
                        }
                    }
                }

                // Coverage: the walls only fill the boundary, so an inner contour
                // (where infill goes) must remain for a 10x10mm region.
                REQUIRE_FALSE(wtp.getInnerContour().empty());
            }
        }
    }
}

SCENARIO("Arachne: DistributedBeadingStrategy wall-count table (0.5x-4x line width)", "[Arachne]")
{
    GIVEN("optimal_width = W and split/add thresholds both 0.5")
    {
        auto strat = make_distributed(W);

        WHEN("region width sweeps from 0 to 4x line width")
        {
            THEN("the optimal wall-count table is {0:0, just-below-0.5x:0, 0.5x:1, 1x:1, 1.5x:2, 2x:2, 2.5x:3, 3x:3, 4x:4}")
            {
                // naive_count = thickness / W; a bead is added once the remainder
                // reaches half a line width.
                REQUIRE(strat->getOptimalBeadCount(0) == 0);            // thickness == 0
                REQUIRE(strat->getOptimalBeadCount(W / 2 - 1) == 0);    // just below the 0.5W threshold: rem < add(0.5W)
                REQUIRE(strat->getOptimalBeadCount(W / 2) == 1);        // 0.5x: naive=0, rem=0.5W >= add(0.5W)
                REQUIRE(strat->getOptimalBeadCount(W) == 1);            // 1x:   naive=1, rem=0   <  split(0.5W)
                REQUIRE(strat->getOptimalBeadCount(3 * W / 2) == 2);    // 1.5x: naive=1, rem=0.5W >= split(0.5W)
                REQUIRE(strat->getOptimalBeadCount(2 * W) == 2);        // 2x:   naive=2, rem=0   <  add(0.5W)
                REQUIRE(strat->getOptimalBeadCount(5 * W / 2) == 3);    // 2.5x: naive=2, rem=0.5W >= add(0.5W)
                REQUIRE(strat->getOptimalBeadCount(3 * W) == 3);        // 3x:   naive=3, rem=0   <  split(0.5W)
                REQUIRE(strat->getOptimalBeadCount(4 * W) == 4);        // 4x:   naive=4, rem=0   <  add(0.5W)
            }
        }
    }
}

SCENARIO("Arachne: even/odd wall-count branches use different split/add thresholds", "[Arachne]")
{
    GIVEN("a DistributedBeadingStrategy with split=0.3, add=0.5 (unequal thresholds)")
    {
        auto strat = std::make_unique<DistributedBeadingStrategy>(W, W, pi_div(3.0), 0.3, 0.5, 2);

        WHEN("naive_count is odd (1) and the remainder lies in the open interval (0.3W, 0.5W)")
        {
            const coord_t rem = 2 * W / 5; // 0.4W, strictly between split=0.3W and add=0.5W
            THEN("the odd branch uses wall_split_middle_threshold(0.3) -> 0.4W >= 0.3W, adding one wall")
            {
                // If add=0.5 were (wrongly) used, 0.4W < 0.5W would give 1 wall; so == 2
                // pins the odd branch to the split threshold.
                REQUIRE(strat->getOptimalBeadCount(W + rem) == 2);
            }
        }

        WHEN("naive_count is even (2) and the remainder lies in the same open interval (0.3W, 0.5W)")
        {
            const coord_t rem = 2 * W / 5; // 0.4W
            THEN("the even branch uses wall_add_middle_threshold(0.5) -> 0.4W < 0.5W, not adding a wall")
            {
                // If split=0.3 were (wrongly) used, 0.4W >= 0.3W would give 3 walls; so == 2
                // pins the even branch to the add threshold.
                REQUIRE(strat->getOptimalBeadCount(2 * W + rem) == 2);
            }
        }
    }
}

SCENARIO("Arachne: DistributedBeadingStrategy compute pure math table", "[Arachne]")
{
    GIVEN("optimal_width = W, with no Print / geometry dependency")
    {
        auto strat = make_distributed(W);

        WHEN("bead_count == 0")
        {
            auto b = strat->compute(W, 0);
            THEN("it produces no beads and leaves the whole thickness as left_over")
            {
                REQUIRE(b.bead_widths.empty());
                REQUIRE(b.toolpath_locations.empty());
                REQUIRE(b.left_over == W);
                REQUIRE(b.total_thickness == W);
            }
        }

        WHEN("bead_count == 1")
        {
            auto b = strat->compute(W, 1);
            THEN("a single bead spans the whole thickness, centered")
            {
                REQUIRE(b.bead_widths == std::vector<coord_t>{ W });
                REQUIRE(b.toolpath_locations == std::vector<coord_t>{ W / 2 });
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == W);
            }
        }

        WHEN("bead_count == 2 and thickness == 2W")
        {
            auto b = strat->compute(2 * W, 2);
            THEN("two equal beads of width W at W/2 and 3W/2")
            {
                REQUIRE(b.bead_widths == (std::vector<coord_t>{ W, W }));
                REQUIRE(b.toolpath_locations == (std::vector<coord_t>{ W / 2, 3 * W / 2 }));
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == 2 * W);
            }
        }

        WHEN("bead_count == 3 and thickness == 3W")
        {
            auto b = strat->compute(3 * W, 3);
            THEN("three equal beads of width W, symmetric and centered, no leftover")
            {
                REQUIRE(b.bead_widths == (std::vector<coord_t>{ W, W, W }));
                REQUIRE(b.toolpath_locations == (std::vector<coord_t>{ W / 2, 3 * W / 2, 5 * W / 2 }));
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == 3 * W);
            }
        }

        WHEN("thickness is 3.2W (to_be_divided = 0.2W > 0), bead_count == 3")
        {
            const coord_t thickness = 3 * W + W / 5;
            auto          b         = strat->compute(thickness, 3);

            THEN("the distributed weighting path runs: middle bead absorbs the extra, ends stay symmetric")
            {
                // distribution_radius = 2 -> weights [0, 1, 0]; the middle bead takes
                // the whole 0.2W surplus, so [W, W + W/5, W].
                REQUIRE(b.bead_widths == (std::vector<coord_t>{ W, W + W / 5, W }));
                REQUIRE(b.bead_widths[1] > b.bead_widths[0]); // middle wider than the outer beads
                REQUIRE(b.bead_widths[0] == b.bead_widths[2]); // symmetric outer beads
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == thickness);
            }
        }

#ifdef NDEBUG
        // This case only runs under NDEBUG (Release): DistributedBeadingStrategy::compute()
        // ends with `assert(total_bead_width + left_over == thickness)`, which fires for the
        // odd thickness 2W+1 and would abort the whole test binary (not just this case) in a
        // Debug/ASAN build. In Release the assert is compiled out, so the quirk is observable.
        WHEN("bead_count == 2 and thickness is odd (2W + 1)")
        {
            const coord_t thickness = 2 * W + 1;
            auto          b         = strat->compute(thickness, 2);

            THEN("integer truncation drops the odd unit (known quirk, pinned not fixed)")
            {
                // outer_width = thickness / 2 is computed twice; 2*(800001/2) == 800000,
                // so one unit is left uncovered. Reachable from the skeleton's
                // double->coord_t conversion.
                REQUIRE(b.bead_widths == (std::vector<coord_t>{ W, W }));
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == thickness - 1); // == 2W, not 2W + 1
            }
        }
#endif
    }
}

SCENARIO("Arachne: WideningBeadingStrategy single centered wall", "[Arachne]")
{
    GIVEN("a Widening wrapping Distributed, min_input_width = 0.25W < min_output_width = 0.5W")
    {
        auto strat = make_widening();

        WHEN("thickness is below min_input_width")
        {
            THEN("the optimal bead count is 0 (too thin to print)")
            {
                REQUIRE(strat->getOptimalBeadCount(W / 8) == 0);       // 0.125W < 0.25W
                REQUIRE(strat->getOptimalBeadCount(W / 4 - 1) == 0);   // just below min_input boundary
            }
        }

        WHEN("thickness is in [min_input_width, 0.5W) where the parent returns 0")
        {
            const coord_t thickness = 3 * W / 10; // 0.3W: parent's add threshold (0.5W) rejects it
            THEN("getOptimalBeadCount forces 1 wall (widening's own branch, not the parent's)")
            {
                // Parent Distributed returns 0 here (rem=0.3W < add 0.5W); widening's
                // "ret < 1 -> 1" branch is what turns it into 1.
                REQUIRE(strat->getOptimalBeadCount(W / 4) == 1); // exactly at min_input
                REQUIRE(strat->getOptimalBeadCount(thickness) == 1);
            }

            AND_WHEN("compute is called")
            {
                auto b = strat->compute(thickness, 1);
                THEN("a single centered wall is over-extruded up to min_output_width (0.5W)")
                {
                    REQUIRE(b.bead_widths == std::vector<coord_t>{ W / 2 });          // max(0.3W, 0.5W)
                    REQUIRE(b.toolpath_locations == std::vector<coord_t>{ thickness / 2 }); // centered
                    REQUIRE(b.left_over == 0);
                    // NOTE: no total_coverage == thickness here — widening intentionally
                    // over-extrudes (bead width 0.5W > feature thickness 0.3W).
                }
            }
        }

        WHEN("thickness >= optimal_width (1W)")
        {
            THEN("widening delegates to the parent strategy")
            {
                REQUIRE(strat->getOptimalBeadCount(W) == 1); // parent: naive=1, rem=0 -> 1

                auto b = strat->compute(W, 1);
                REQUIRE(b.bead_widths == std::vector<coord_t>{ W });
                REQUIRE(b.toolpath_locations == std::vector<coord_t>{ W / 2 });
                REQUIRE(b.left_over == 0);
                REQUIRE(total_coverage(b) == W);
            }
        }
    }
}

SCENARIO("Arachne: generated line widths are conserved and within [bead_width ± ε]", "[Arachne]")
{
    GIVEN("a DistributedBeadingStrategy with optimal_width = W")
    {
        auto strat = make_distributed(W);

        WHEN("thickness is an integer multiple of the line width (1x/2x/3x/4x)")
        {
            THEN("every generated line width is exactly W (ε = 0, exact integers) and width is conserved")
            {
                for (coord_t n : { coord_t(1), coord_t(2), coord_t(3), coord_t(4) }) {
                    const coord_t thickness = n * W;
                    auto          b         = strat->compute(thickness, n);
                    CAPTURE(n);

                    REQUIRE(b.bead_widths.size() == static_cast<size_t>(n));
                    for (coord_t width : b.bead_widths)
                        CHECK(width == W); // within [W - 0, W + 0]; CHECK to see all independent widths
                    REQUIRE(b.left_over == 0);
                    REQUIRE(total_coverage(b) == thickness);
                }
            }
        }

        WHEN("thickness is not an integer multiple (5.5W)")
        {
            const coord_t thickness = 5 * W + W / 2; // 5.5W
            auto          b         = strat->compute(thickness, 5);

            THEN("widths deviate from W but stay bounded, and width is conserved")
            {
                REQUIRE(b.bead_widths.size() == 5);
                for (coord_t width : b.bead_widths) {
                    CHECK(width >= W);    // never shrinks below the nominal bead
                    CHECK(width < 2 * W); // surplus (< W) can at most double a bead
                }
                REQUIRE(total_coverage(b) == thickness); // conservation is the core ±ε invariant
            }
        }
    }
}
