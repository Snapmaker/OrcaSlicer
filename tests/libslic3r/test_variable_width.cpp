#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/VariableWidth.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

using namespace Slic3r;

namespace {

// NOTE: Flow(width, height, nozzle_diameter) — the three constructor args. The
// ThickPolyline width values used below are extrusion SPACINGs (independent of
// flow.width()); variable_width() converts spacing -> extrusion width internally
// via width = spacing + height*(1 - pi/4).

// Every scenario uses the same flow: width 0.5mm, height 0.2mm, nozzle 0.4mm.
Flow test_flow() { return Flow(0.5f, 0.2f, 0.4f); }

// Build a ThickPolyline from mm points and mm widths. The ThickPolyline stores the
// width as a SCALED spacing (two entries per segment), so scale_() each value.
ThickPolyline make_tp(const std::vector<Vec2d> &pts_mm, const std::vector<double> &widths_mm)
{
    ThickPolyline tp;
    for (const Vec2d &p : pts_mm) tp.points.emplace_back(Point::new_scale(p.x(), p.y()));
    for (double w : widths_mm) tp.width.emplace_back(scale_(w));
    return tp;
}

// Constant-width ThickPolyline: (n-1)*2 width entries, all the same.
ThickPolyline constant_tp(const std::vector<Vec2d> &pts_mm, double width_mm)
{
    return make_tp(pts_mm, std::vector<double>((pts_mm.size() - 1) * 2, width_mm));
}

// The 10x10 closed square (CW in slicer's y-down coordinates; closing point repeated).
ThickPolyline square_tp(double width_mm)
{
    return constant_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 10.0, 10.0 }, { 0.0, 10.0 }, { 0.0, 0.0 } }, width_mm);
}

// Cast every entity in `out` to an ExtrusionPath and collect its width. The null
// check is a hard precondition (dereferencing a null path is UB), so REQUIRE.
std::vector<float> collect_path_widths(const std::vector<ExtrusionEntity *> &out)
{
    std::vector<float> widths;
    for (ExtrusionEntity *e : out) {
        auto *p = dynamic_cast<ExtrusionPath *>(e);
        REQUIRE(p != nullptr);
        widths.push_back(p->width);
    }
    return widths;
}

// RAII guard: variable_width() allocates its output entities with `new`, so delete
// them on scope exit. Unlike a manual free at the end of each THEN, this also runs
// when a preceding REQUIRE fails and unwinds — otherwise the entities leak (and are
// flagged by ASAN/valgrind, polluting later cases).
struct EntityGuard {
    std::vector<ExtrusionEntity *> &out;
    explicit EntityGuard(std::vector<ExtrusionEntity *> &o) : out(o) {}
    ~EntityGuard() {
        for (ExtrusionEntity *e : out) delete e;
        out.clear();
    }
    EntityGuard(const EntityGuard &) = delete;
    EntityGuard &operator=(const EntityGuard &) = delete;
};

} // namespace

SCENARIO("VariableWidth of a constant width reproduces the flow's area and width", "[VariableWidth]") {
    GIVEN("an open polyline with a constant width matching the flow spacing") {
        Flow flow = test_flow();
        // The ThickPolyline width is the extrusion SPACING; the flow converts
        // spacing -> extrusion width via width = spacing + height*(1 - pi/4).
        ThickPolyline tp = constant_tp({ { 0.0, 0.0 }, { 10.0, 0.0 } }, flow.spacing());

        WHEN("converted to extrusion entities") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("one extrusion path is produced with the flow's area and width") {
                REQUIRE(out.size() == 1);
                auto *path = dynamic_cast<ExtrusionPath *>(out[0]);
                REQUIRE(path != nullptr);
                // 1e-6 (tighter than the 1e-5 used for hand-derived goldens) is safe
                // here because 0.5f is exactly representable, so the spacing -> width
                // round-trip reproduces flow.width()/mm3_per_mm() exactly.
                CHECK_THAT(path->mm3_per_mm, WithinRel(flow.mm3_per_mm(), 1e-6));
                CHECK_THAT(path->width, WithinRel(flow.width(), 1e-6));
            }
        }
    }
}

SCENARIO("VariableWidth splits a polyline on a large width change", "[VariableWidth]") {
    GIVEN("two constant-width segments differing by more than the tolerance") {
        Flow flow = test_flow();
        // 0.4mm then 0.7mm spacing: the 0.3mm delta exceeds the 0.05mm tolerance.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 } },
                                   { 0.4, 0.4, 0.7, 0.7 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("the polyline is split into two paths with the propagated widths") {
                REQUIRE(out.size() == 2);
                auto *p0 = dynamic_cast<ExtrusionPath *>(out[0]);
                auto *p1 = dynamic_cast<ExtrusionPath *>(out[1]);
                REQUIRE(p0 != nullptr);
                REQUIRE(p1 != nullptr);
                // spacing -> width: +0.2*(1 - pi/4) = +0.0429203673205.
                CHECK_THAT(p0->width, WithinRel(0.4429203673205, 1e-5));
                CHECK_THAT(p1->width, WithinRel(0.7429203673205, 1e-5));
            }
        }
    }
}

SCENARIO("VariableWidth merges a small width change into one path", "[VariableWidth]") {
    GIVEN("two segments whose width differs by less than the tolerance") {
        Flow flow = test_flow();
        // 0.45mm then 0.48mm spacing: the 0.03mm delta is under the 0.05mm tolerance.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 } },
                                   { 0.45, 0.45, 0.48, 0.48 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("the polyline stays a single path with the weighted-average width") {
                REQUIRE(out.size() == 1);
                auto *p = dynamic_cast<ExtrusionPath *>(out[0]);
                REQUIRE(p != nullptr);
                // Weighted-average spacing (equal lengths) = (0.45+0.48)/2 = 0.465;
                // width = 0.465 + 0.2*(1 - pi/4) = 0.507920.
                REQUIRE_THAT(p->width, WithinRel(0.5079203673205, 1e-5));
            }
        }
    }
}

SCENARIO("VariableWidth subdivides a tapered single segment", "[VariableWidth]") {
    GIVEN("a single segment whose width tapers by more than the tolerance") {
        Flow flow = test_flow();
        // One segment 0.4 -> 0.7 mm: the taper is subdivided so the width changes
        // gradually (each step <= tolerance) rather than in one 0.3mm jump.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 10.0, 0.0 } }, { 0.4, 0.7 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("the segment is subdivided with adjacent width deltas within the tolerance") {
                REQUIRE(out.size() > 1);
                std::vector<float> widths = collect_path_widths(out);
                // 0.05mm = variable_width()'s internal tolerance (scale_(0.05)); the
                // +1e-4f absorbs float rounding in the per-segment width computation.
                for (size_t i = 1; i < widths.size(); ++i)
                    CHECK(std::abs(widths[i] - widths[i - 1]) <= 0.05f + 1e-4f);
            }
        }
    }
}

SCENARIO("VariableWidth emits a loop for a closed polyline", "[VariableWidth]") {
    GIVEN("a closed square polyline") {
        Flow flow = test_flow();
        ThickPolyline tp = square_tp(flow.spacing());

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("one extrusion loop is produced") {
                REQUIRE(out.size() == 1);
                REQUIRE(dynamic_cast<ExtrusionLoop *>(out[0]) != nullptr);
            }
        }
    }
}

SCENARIO("VariableWidth handles empty and degenerate input", "[VariableWidth]") {
    GIVEN("no polylines") {
        Flow flow = test_flow();
        std::vector<ExtrusionEntity *> out;
        EntityGuard guard(out);
        variable_width({}, erPerimeter, flow, out);
        THEN("the output is empty") {
            REQUIRE(out.empty());
        }
    }
    GIVEN("a single-point polyline") {
        Flow flow = test_flow();
        ThickPolyline tp = make_tp({ { 0.0, 0.0 } }, {});
        std::vector<ExtrusionEntity *> out;
        EntityGuard guard(out);
        variable_width({ tp }, erPerimeter, flow, out);
        THEN("the output is empty") {
            REQUIRE(out.empty());
        }
    }
}

// ---------------------------------------------------------------------------
// Sliding-window (max-min range) merging in thick_polyline_to_extrusion_paths_2
// ---------------------------------------------------------------------------

SCENARIO("VariableWidth merges a gradual monotonic taper with a sliding window", "[VariableWidth]") {
    GIVEN("five segments whose adjacent delta is under tolerance but the cumulative range exceeds it") {
        Flow flow = test_flow();
        // 0.40 -> 0.42 -> 0.44 -> 0.46 -> 0.48 -> 0.50 (spacing). Each adjacent delta is
        // 0.02mm (<= 0.05mm tolerance), so an adjacent-comparison merge would collapse all
        // five segments into one path. The sliding window instead tracks max-min and splits
        // once the running range exceeds 0.05mm.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 }, { 30.0, 0.0 }, { 40.0, 0.0 }, { 50.0, 0.0 } },
                                   { 0.40, 0.42, 0.42, 0.44, 0.44, 0.46, 0.46, 0.48, 0.48, 0.50 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("three windows are produced, not one and not five") {
                REQUIRE(out.size() == 3);
                std::vector<float> widths = collect_path_widths(out);
                // window [0.40,0.42,0.44] -> mean 0.42; [0.44,0.46,0.48] -> 0.46;
                // final [0.48,0.50] -> 0.48. NOTE: the final block averages each segment's
                // a_width only (not the trapezoidal (a+b)/2), so the last window reports
                // 0.48 rather than the trapezoidal mean 0.49 — pinned as-is.
                CHECK_THAT(widths[0], WithinRel(0.4629203673205, 1e-5));
                CHECK_THAT(widths[1], WithinRel(0.5029203673205, 1e-5));
                CHECK_THAT(widths[2], WithinRel(0.5229203673205, 1e-5));
            }
        }
    }
}

SCENARIO("VariableWidth sliding window handles a non-monotonic width profile", "[VariableWidth]") {
    GIVEN("constant-width segments that dip and rise within the tolerance") {
        Flow flow = test_flow();
        // 0.40 -> 0.46 -> 0.42 -> 0.46 -> 0.40. The middle three widths span 0.42..0.46
        // (range 0.04mm <= 0.05mm) and stay in one window; the 0.40 segments force a split
        // on each side (0.40->0.46 and 0.46->0.40 both exceed 0.05mm).
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 }, { 30.0, 0.0 }, { 40.0, 0.0 }, { 50.0, 0.0 } },
                                   { 0.40, 0.40, 0.46, 0.46, 0.42, 0.42, 0.46, 0.46, 0.40, 0.40 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("three windows are produced: 0.40, (0.46+0.42+0.46)/3, 0.40") {
                REQUIRE(out.size() == 3);
                std::vector<float> widths = collect_path_widths(out);
                // Middle window mean spacing = (0.46+0.42+0.46)/3 = 1.34/3 = 0.4466667.
                CHECK_THAT(widths[0], WithinRel(0.4429203673205, 1e-5));
                CHECK_THAT(widths[1], WithinRel(0.4895870339871667, 1e-5));
                CHECK_THAT(widths[2], WithinRel(0.4429203673205, 1e-5));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Length-weighted average width over merged (within-tolerance) segments
// ---------------------------------------------------------------------------

SCENARIO("VariableWidth length-weights the merged width of unequal-length segments", "[VariableWidth]") {
    GIVEN("two constant-width segments of very different lengths") {
        Flow flow = test_flow();
        // 2mm @ 0.45 then 18mm @ 0.48: a simple arithmetic mean would give 0.465, but the
        // correct length-weighted mean is (2*0.45 + 18*0.48)/20 = 0.477.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 2.0, 0.0 }, { 20.0, 0.0 } },
                                   { 0.45, 0.45, 0.48, 0.48 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("a single path carries the length-weighted average width") {
                REQUIRE(out.size() == 1);
                auto *p = dynamic_cast<ExtrusionPath *>(out[0]);
                REQUIRE(p != nullptr);
                // spacing = 0.477; width = 0.477 + 0.0429203673205.
                CHECK_THAT(p->width, WithinRel(0.5199203673205, 1e-5));
            }
        }
    }
    GIVEN("three constant-width segments of different lengths") {
        Flow flow = test_flow();
        // 5mm @ 0.42 + 5mm @ 0.44 + 10mm @ 0.46: length-weighted mean = 8.9/20 = 0.445.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 5.0, 0.0 }, { 10.0, 0.0 }, { 20.0, 0.0 } },
                                   { 0.42, 0.42, 0.44, 0.44, 0.46, 0.46 });

        WHEN("converted") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erPerimeter, flow, out);

            THEN("a single path carries the length-weighted average width") {
                REQUIRE(out.size() == 1);
                auto *p = dynamic_cast<ExtrusionPath *>(out[0]);
                REQUIRE(p != nullptr);
                // spacing = 0.445; width = 0.445 + 0.0429203673205.
                CHECK_THAT(p->width, WithinRel(0.4879203673205, 1e-5));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Closed gap-fill loops (closed_gap_fill_loop_to_extrusion_paths)
// ---------------------------------------------------------------------------

SCENARIO("VariableWidth emits a gap-fill loop with the flow width for a constant-width closed polyline", "[VariableWidth]") {
    GIVEN("a closed square gap-fill polyline at the flow spacing") {
        Flow flow = test_flow();
        ThickPolyline tp = square_tp(flow.spacing());

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erGapFill, flow, out);

            THEN("one loop reproduces the flow width and area") {
                REQUIRE(out.size() == 1);
                auto *loop = dynamic_cast<ExtrusionLoop *>(out[0]);
                REQUIRE(loop != nullptr);
                REQUIRE(loop->paths.size() == 1);
                // All segments equal -> average spacing == flow.spacing() -> width == flow.width().
                // 1e-6 relies on 0.5f being exactly representable (same as the open-path case).
                CHECK_THAT(loop->paths.front().width, WithinRel(flow.width(), 1e-6));
                CHECK_THAT(loop->paths.front().mm3_per_mm, WithinRel(flow.mm3_per_mm(), 1e-6));
            }
        }
    }
}

SCENARIO("VariableWidth length-weights the width of a tapered closed gap-fill loop", "[VariableWidth]") {
    GIVEN("a closed rectangle with wider vertical sides") {
        Flow flow = test_flow();
        // 20mm horizontal sides @ 0.4 + 10mm vertical sides @ 0.6:
        // weighted mean spacing = (2*20*0.4 + 2*10*0.6)/60 = 28/60 = 0.4666667.
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 20.0, 0.0 }, { 20.0, 10.0 }, { 0.0, 10.0 }, { 0.0, 0.0 } },
                                   { 0.4, 0.4, 0.6, 0.6, 0.4, 0.4, 0.6, 0.6 });

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erGapFill, flow, out);

            THEN("one loop carries the length-weighted average width") {
                REQUIRE(out.size() == 1);
                auto *loop = dynamic_cast<ExtrusionLoop *>(out[0]);
                REQUIRE(loop != nullptr);
                REQUIRE(loop->paths.size() == 1);
                // spacing = 28/60; width = 28/60 + 0.0429203673205 = 0.5095870339871667.
                CHECK_THAT(loop->paths.front().width, WithinRel(0.5095870339871667, 1e-5));
            }
        }
    }
}

SCENARIO("VariableWidth drops a degenerate closed gap-fill loop", "[VariableWidth]") {
    GIVEN("a closed loop of zero total length") {
        Flow flow = test_flow();
        ThickPolyline tp = make_tp({ { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 } },
                                   { 0.4, 0.4, 0.4, 0.4 });

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ tp }, erGapFill, flow, out);

            THEN("no entities are produced") {
                REQUIRE(out.empty());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Gap-fill closed-loop deduplication (are_near_duplicate_closed_gap_fill_loops)
// ---------------------------------------------------------------------------

SCENARIO("VariableWidth deduplicates identical closed gap-fill loops", "[VariableWidth]") {
    GIVEN("two identical closed squares") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = a;

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("only one loop survives") {
                REQUIRE(out.size() == 1);
            }
        }
    }
}

SCENARIO("VariableWidth deduplicates closed gap-fill loops modulo vertex start and direction", "[VariableWidth]") {
    GIVEN("the same square starting at a different vertex") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = constant_tp({ { 10.0, 0.0 }, { 10.0, 10.0 }, { 0.0, 10.0 }, { 0.0, 0.0 }, { 10.0, 0.0 } }, 0.4);

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("they are treated as one loop") {
                REQUIRE(out.size() == 1);
            }
        }
    }
    GIVEN("the same square traversed in reverse") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = constant_tp({ { 0.0, 0.0 }, { 0.0, 10.0 }, { 10.0, 10.0 }, { 10.0, 0.0 }, { 0.0, 0.0 } }, 0.4);

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("they are treated as one loop") {
                REQUIRE(out.size() == 1);
            }
        }
    }
}

SCENARIO("VariableWidth deduplicates closed gap-fill loops within tolerance only", "[VariableWidth]") {
    GIVEN("a vertex shifted within the 0.05mm point/bbox tolerance") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = constant_tp({ { 0.0, 0.0 }, { 10.02, 0.0 }, { 10.0, 10.0 }, { 0.0, 10.0 }, { 0.0, 0.0 } }, 0.4);

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("the loops are near-duplicates and merged") {
                REQUIRE(out.size() == 1);
            }
        }
    }
    GIVEN("a vertex shifted beyond the tolerance") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = constant_tp({ { 0.0, 0.0 }, { 10.5, 0.0 }, { 10.0, 10.0 }, { 0.0, 10.0 }, { 0.0, 0.0 } }, 0.4);

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("both loops are kept") {
                REQUIRE(out.size() == 2);
            }
        }
    }
    GIVEN("loops with different vertex counts") {
        Flow flow = test_flow();
        ThickPolyline a = square_tp(0.4);
        ThickPolyline b = constant_tp({ { 0.0, 0.0 }, { 10.0, 0.0 }, { 5.0, 10.0 }, { 0.0, 0.0 } }, 0.4);

        WHEN("converted as gap fill") {
            std::vector<ExtrusionEntity *> out;
            EntityGuard guard(out);
            variable_width({ a, b }, erGapFill, flow, out);

            THEN("they are not merged") {
                REQUIRE(out.size() == 2);
            }
        }
    }
}
