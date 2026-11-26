// Tests for the cheap pre-computation face-count estimate of the local
// subdivision preview (MR !21 review item 2). The estimate must be evaluable
// WITHOUT running the subdivision, be a conservative upper bound of what
// smooth_triangle_mesh_local_its actually produces (selected faces plus the
// untouched surroundings, including the boundary coordination), be exact in
// the trivial cases, and never overflow. The shared budget decision must flip
// exactly at the threshold.

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

#include "libslic3r/PreviewBudgetGate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshDeal.hpp"

using namespace Slic3r;

namespace {

// Closed cube built from 12 triangles (two per face), CCW seen from outside:
// every edge is shared by exactly two faces, so all faces are interior.
indexed_triangle_set cube_its()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                     Vec3f(0.f, 0.f, 1.f), Vec3f(1.f, 0.f, 1.f), Vec3f(1.f, 1.f, 1.f), Vec3f(0.f, 1.f, 1.f) };
    its.indices  = {
        stl_triangle_vertex_indices(0, 2, 1), stl_triangle_vertex_indices(0, 3, 2), // bottom
        stl_triangle_vertex_indices(4, 5, 6), stl_triangle_vertex_indices(4, 6, 7), // top
        stl_triangle_vertex_indices(0, 1, 5), stl_triangle_vertex_indices(0, 5, 4), // y = 0
        stl_triangle_vertex_indices(1, 2, 6), stl_triangle_vertex_indices(1, 6, 5), // x = 1
        stl_triangle_vertex_indices(2, 3, 7), stl_triangle_vertex_indices(2, 7, 6), // y = 1
        stl_triangle_vertex_indices(3, 0, 4), stl_triangle_vertex_indices(3, 4, 7), // x = 0
    };
    return its;
}

// Closed regular tetrahedron: 4 faces, all interior.
indexed_triangle_set tetra_its()
{
    indexed_triangle_set its;
    its.vertices = { Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f) };
    its.indices  = {
        stl_triangle_vertex_indices(0, 2, 1),
        stl_triangle_vertex_indices(0, 1, 3),
        stl_triangle_vertex_indices(1, 2, 3),
        stl_triangle_vertex_indices(2, 0, 3),
    };
    return its;
}

// Mirrors the production preview growth loop: subdivides the region (front of
// the face list) level by level and returns the final face count.
size_t subdivide_region_levels(const indexed_triangle_set& origin, const std::vector<uint32_t>& selection, int levels)
{
    indexed_triangle_set  current = origin;
    std::vector<uint32_t> region  = selection;
    for (int step = 0; step < levels; ++step) {
        bool                 ok = false;
        indexed_triangle_set next = TriangleMeshDeal::smooth_triangle_mesh_local_its(current, region, ok);
        REQUIRE(ok);
        const size_t unselected   = current.indices.size() - region.size();
        const size_t region_faces = next.indices.size() - unselected;
        region.assign(region_faces, 0);
        for (size_t i = 0; i < region_faces; ++i)
            region[i] = uint32_t(i);
        current = std::move(next);
    }
    return current.indices.size();
}

} // namespace

TEST_CASE("Local subdivision budget estimate is exact for the trivial cases", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    // No subdivision requested: the result is the input, exactly.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 7, 0, out));
    CHECK(out == 123);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 7, -2, out));
    CHECK(out == 123);
    // Nothing selected: unselected faces pass through untouched, exactly.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(123, 0, 3, out));
    CHECK(out == 123);
}

TEST_CASE("Local subdivision budget estimate grows by at most 4^level per painted face", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    // One painted face: 4^level - 1 additional faces at most.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 1, out));
    CHECK(out == 1003);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 2, out));
    CHECK(out == 1015);
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 1, 3, out));
    CHECK(out == 1063);
    // Linear in the painted count.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(1000, 5, 3, out));
    CHECK(out == 1000 + 5 * 63);
}

TEST_CASE("Local subdivision budget estimate never overflows", "[TriangleMeshDeal][LocalSubdivision]")
{
    size_t out = 0;
    const size_t max = std::numeric_limits<size_t>::max();
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max, 1, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max - 2, 1, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(0, max, 1, out));
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(max, max, 3, out));
    // The 4^level growth itself must not wrap at absurd levels.
    CHECK_FALSE(TriangleMeshDeal::estimated_local_subdivision_face_count(0, 1, 1000, out));
    // Representable sums still succeed.
    CHECK(TriangleMeshDeal::estimated_local_subdivision_face_count(max - 1000, 1, 1, out));
    CHECK(out == max - 997);
}

TEST_CASE("Estimate bounds the real local subdivision output", "[TriangleMeshDeal][LocalSubdivision]")
{
    // A closed tetrahedron: all faces interior, one step splits every face
    // 1-to-4, so the bound is attained exactly.
    {
        const indexed_triangle_set its   = tetra_its();
        const size_t               total = its.indices.size();
        const std::vector<uint32_t> all{ 0, 1, 2, 3 };
        size_t                      bound = 0;
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, total, 1, bound));
        CHECK(bound == total * 4);
        CHECK(subdivide_region_levels(its, all, 1) == bound);
    }

    // A closed cube, fully selected: exact again for every level (all faces
    // stay interior), which also checks the 4^level factor across steps.
    {
        const indexed_triangle_set its   = cube_its();
        const size_t               total = its.indices.size();
        std::vector<uint32_t>      all(total);
        for (size_t i = 0; i < total; ++i)
            all[i] = uint32_t(i);
        for (const int level : { 1, 2, 3 }) {
            size_t bound = 0;
            REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, total, level, bound));
            CHECK(bound == total * (size_t{ 1 } << (2 * level)));
            CHECK(subdivide_region_levels(its, all, level) == bound);
        }
    }

    // Partial selection: the boundary coordination produces 2-3 children per
    // boundary face, so the actual count stays strictly below the 4x bound
    // while never dropping below 2 children per selected face. The unselected
    // faces pass through untouched. Both claims are checked against the real
    // algorithm over two levels, mirroring the production growth loop.
    {
        const indexed_triangle_set its   = cube_its();
        const size_t               total = its.indices.size();
        const std::vector<uint32_t> some{ 0, 1, 4 };
        bool                        ok      = false;
        indexed_triangle_set        subdiv  = TriangleMeshDeal::smooth_triangle_mesh_local_its(its, some, ok);
        REQUIRE(ok);
        const size_t unselected = total - some.size();
        CHECK(subdiv.indices.size() >= unselected + 2 * some.size());
        CHECK(subdiv.indices.size() < unselected + 4 * some.size());
        size_t bound = 0;
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, some.size(), 1, bound));
        CHECK(subdiv.indices.size() <= bound);
        // Two levels: the region children become the next selection; the bound
        // keeps holding.
        REQUIRE(TriangleMeshDeal::estimated_local_subdivision_face_count(total, some.size(), 2, bound));
        CHECK(subdivide_region_levels(its, some, 2) <= bound);
    }
}

TEST_CASE("Budget decision flips exactly at the threshold", "[TriangleMeshDeal][LocalSubdivision]")
{
    // The same value the gizmo passes as LocalSubdivisionFaceBudget.
    const size_t budget = 1000000;
    // Level 3: every painted face costs at most 63 extra faces.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999937, 1, 3, budget)); // exactly 1,000,000
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999938, 1, 3, budget));       // 1,000,001
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 15873, 3, budget));  // 999,999
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 15874, 3, budget));        // 1,000,062
    // The review scenario: ~50k interior faces at level 3 cost several million.
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(0, 50000, 3, budget));
    // Normal previews keep computing without asking.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(400000, 9000, 3, budget)); // 967,000
    // An empty selection estimates exactly the untouched mesh (nothing grows):
    // the production gate short-circuits before the dialog for painted == 0,
    // while the pure decision reflects the face count of the unchanged result.
    CHECK_FALSE(TriangleMeshDeal::local_subdivision_exceeds_face_budget(999999, 0, 3, budget));
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(1000001, 0, 3, budget));
    // Overflow counts as exceeding - never as within budget.
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(std::numeric_limits<size_t>::max(), 1, 100, budget));
    CHECK(TriangleMeshDeal::local_subdivision_exceeds_face_budget(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), 3, budget));
}

TEST_CASE("Preview budget gate blocks for the declined selection state only", "[LocalSubdivision][BudgetGate]")
{
    const size_t      budget = 1000000;
    PreviewBudgetGate gate;

    // An over-budget selection asks for confirmation.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    // Decline: the stepper must follow the level that is actually shown.
    CHECK(gate.resolve_confirmation(false, 3, 2) == 2);
    CHECK(gate.blocked());

    // No edits (same selection size): no recompute, no dialog.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Blocked);
    // A larger selection stays blocked too (the estimate is monotonic).
    CHECK(gate.evaluate(3, 60000, 0, budget) == PreviewBudgetGate::Verdict::Blocked);
    CHECK(gate.blocked());

    // Erasing parts of the selection but still over the budget: silent block.
    CHECK(gate.evaluate(3, 20000, 0, budget) == PreviewBudgetGate::Verdict::Blocked);
    CHECK(gate.blocked());

    // Shrunk back under the budget: the block lifts and the preview computes.
    CHECK(gate.evaluate(3, 9000, 0, budget) == PreviewBudgetGate::Verdict::Compute);
    CHECK_FALSE(gate.blocked());
}

TEST_CASE("Preview budget gate recovers on erase-all and re-asks on level changes", "[LocalSubdivision][BudgetGate]")
{
    const size_t      budget = 1000000;
    PreviewBudgetGate gate;

    // Decline an over-budget level-3 selection.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(false, 3, 2) == 2);
    CHECK(gate.blocked());

    // Erase all: an empty selection always fits and the block lifts.
    CHECK(gate.evaluate(3, 0, 2000000, budget) == PreviewBudgetGate::Verdict::Compute);
    CHECK_FALSE(gate.blocked());

    // Level changes while blocked re-evaluate from scratch. Re-block at level 3,
    // then request level 2 with the same selection: 50,000 * 15 = 750,000 fits.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(false, 3, 2) == 2);
    CHECK(gate.evaluate(2, 50000, 0, budget) == PreviewBudgetGate::Verdict::Compute);
    CHECK_FALSE(gate.blocked());

    // Re-block at level 3 again; level 2 with a still-too-large selection
    // (80,000 * 15 = 1,200,000) asks again instead of staying silently blocked.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(false, 3, 2) == 2);
    CHECK(gate.evaluate(2, 80000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(true, 2, 2) == 2);
    CHECK_FALSE(gate.blocked());
}

TEST_CASE("Preview budget gate confirmation and reset", "[LocalSubdivision][BudgetGate]")
{
    const size_t      budget = 1000000;
    PreviewBudgetGate gate;

    // Accept: the level stays and the gate forgets the question.
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(true, 3, 2) == 3);
    CHECK_FALSE(gate.blocked());
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);

    // A within-budget selection computes without asking.
    CHECK(gate.evaluate(3, 9000, 0, budget) == PreviewBudgetGate::Verdict::Compute);

    // reset() forgets a decline (selection rebuilt from scratch / gizmo opened).
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
    CHECK(gate.resolve_confirmation(false, 3, 2) == 2);
    CHECK(gate.blocked());
    gate.reset();
    CHECK_FALSE(gate.blocked());
    CHECK(gate.evaluate(3, 50000, 0, budget) == PreviewBudgetGate::Verdict::Confirm);
}

namespace {

// Drives PreviewBudgetFlow through the same sequences the gizmo executes
// (panel frame, apply-time re-confirmation and forced recompute). The dialog
// and the expensive computation are injected stand-ins with call counters;
// last_computed_painted identifies which selection the produced caches were
// built for, mirroring the gizmo's guarantee that the previews are rebuilt
// from the current selectors inside the computation.
struct BudgetFlowHarness {
    PreviewBudgetFlow flow;
    int               ask_calls             = 0;
    int               compute_calls          = 0;
    bool              dialog_answer          = true;
    bool              compute_ok             = true;
    int               level                  = 0;
    int               shown                  = 0;
    size_t            painted                = 0;
    size_t            total                  = 0;
    bool              work_pending           = false;
    const size_t      budget                 = 1000000;
    size_t            last_computed_painted  = 0;
    size_t            applied_painted        = 0;
    bool              applied                = false;
    // Optional hook running while the apply-time dialog would be open.
    std::function<void()> on_dialog          = nullptr;

    PreviewUpdateOutcome frame(BudgetPromptMode mode)
    {
        const auto result = flow.update(
            PreviewBudgetFlow::FrameInputs{ level, shown, painted, total, budget, work_pending }, mode,
            [this](size_t &p, size_t &t) { p = painted; t = total; },
            [this]() { ++ask_calls; return dialog_answer; },
            [this]() { ++compute_calls; last_computed_painted = painted; return compute_ok; });
        level = result.stepper_level;
        return result.outcome;
    }

    // The gizmo's apply_subdivision budget orchestration: returns whether the
    // apply is allowed to proceed.
    bool apply()
    {
        const bool was_blocked_before_update = flow.blocked();
        const PreviewUpdateOutcome first      = frame(BudgetPromptMode::Ask);
        if (first == PreviewUpdateOutcome::Blocked && ! was_blocked_before_update)
            return false; // declined inside the update_preview call above
        if (first != PreviewUpdateOutcome::Ready && first != PreviewUpdateOutcome::Blocked)
            return false; // reentered / inputs changed / no object: nothing safe to apply
        if (painted == 0 || level <= 0)
            return false;

        if (flow.blocked()) { // was_blocked_before_update is necessarily true here
            // Re-confirm against the current selection; the dialog may pump
            // events that change the inputs afterwards, so the authorization
            // is recorded for the inputs gathered BEFORE the dialog.
            const size_t gathered_painted = painted;
            const size_t gathered_total   = total;
            const bool   over = TriangleMeshDeal::local_subdivision_exceeds_face_budget(gathered_total, gathered_painted, level, budget);
            if (over) {
                ++ask_calls;
                if (on_dialog)
                    on_dialog();
                if (! dialog_answer)
                    return false;
            }
            flow.authorize_recompute(level, gathered_painted, gathered_total, budget);
            // Forced recompute: no further dialog, the authorization must
            // match the current inputs, and the recompute must complete.
            if (frame(BudgetPromptMode::ConfirmedOnly) != PreviewUpdateOutcome::Ready)
                return false;
            if (painted == 0 || level <= 0)
                return false;
        }

        // The caches now match the current selection (the recompute above ran
        // for it); the final budget question was already answered.
        applied         = true;
        applied_painted = last_computed_painted;
        return true;
    }
};

} // namespace

TEST_CASE("Apply after a decline recomputes exactly once and uses the new selection", "[LocalSubdivision][BudgetFlow]")
{
    BudgetFlowHarness h;
    h.level = 3;
    h.shown = 3;
    h.painted = 50000;
    h.work_pending = true;

    // Panel: raising to level 3 exceeds the budget and the user declines; the
    // stepper follows the level that is actually shown (3: the preview was
    // already displayed at level 3), so the level stays put.
    h.dialog_answer = false;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Blocked);
    CHECK(h.ask_calls == 1);
    CHECK(h.compute_calls == 0);
    CHECK(h.level == 3);
    CHECK(h.flow.blocked());

    // The user enlarges the selection (still over the budget at level 2) and
    // clicks Apply, then confirms the re-confirmation.
    h.painted       = 80000;
    h.dialog_answer = true;
    REQUIRE(h.apply());
    // Exactly one re-confirmation dialog was shown in total (panel decline +
    // apply confirm; the forced recompute must not ask a third time).
    CHECK(h.ask_calls == 2);
    // The recompute really ran after the confirmation - the old caches were
    // never applied directly.
    CHECK(h.compute_calls == 1);
    CHECK(h.last_computed_painted == 80000);
    CHECK(h.applied_painted == 80000);
    CHECK_FALSE(h.flow.blocked());
}

TEST_CASE("Apply then declining again keeps the old preview and submits nothing", "[LocalSubdivision][BudgetFlow]")
{
    BudgetFlowHarness h;
    h.level = 3;
    h.shown = 3;
    h.painted = 50000;
    h.work_pending = true;
    h.dialog_answer = false;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Blocked);

    h.painted       = 80000;
    h.dialog_answer = false;
    CHECK_FALSE(h.apply());
    CHECK(h.compute_calls == 0); // no recompute
    CHECK(h.ask_calls == 2);     // panel decline + apply decline
    CHECK(h.flow.blocked());     // the old preview stays blocked and kept
    CHECK_FALSE(h.applied);
}

TEST_CASE("Apply aborts when the forced recompute cannot complete", "[LocalSubdivision][BudgetFlow]")
{
    BudgetFlowHarness h;
    h.level = 3;
    h.shown = 3;
    h.painted = 50000;
    h.work_pending = true;
    h.dialog_answer = false;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Blocked);

    // The recompute is authorized but fails (reentry / failure).
    h.painted = 80000;
    h.dialog_answer = true;
    h.compute_ok    = false;
    CHECK_FALSE(h.apply());
    CHECK(h.compute_calls == 1); // the recompute was attempted...
    CHECK_FALSE(h.applied);      // ...and nothing was applied
}

TEST_CASE("Apply aborts when the confirmed inputs change while the dialog is open", "[LocalSubdivision][BudgetFlow]")
{
    BudgetFlowHarness h;
    h.level = 3;
    h.shown = 3;
    h.painted = 50000;
    h.work_pending = true;
    h.dialog_answer = false;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Blocked);

    // The apply-time dialog pumps events that enlarge the selection beyond
    // the confirmed size: the old confirmation must not authorize the
    // recompute, and nothing may be applied.
    h.painted       = 80000;
    h.dialog_answer = true;
    h.on_dialog     = [&h]() { h.painted = 90000; };
    CHECK_FALSE(h.apply());
    CHECK(h.compute_calls == 0);
    CHECK_FALSE(h.applied);
}

TEST_CASE("Shrinking the selection back under the budget recovers and applies without extra confirmation", "[LocalSubdivision][BudgetFlow]")
{
    BudgetFlowHarness h;
    h.level = 3;
    h.shown = 3;
    h.painted = 50000;
    h.work_pending = true;
    h.dialog_answer = false;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Blocked);
    CHECK(h.flow.blocked());

    // The user erases most of the selection: the next frame re-checks cheaply,
    // lifts the block and computes the latest preview without any dialog.
    h.painted     = 9000;
    h.dialog_answer = true;
    CHECK(h.frame(BudgetPromptMode::Ask) == PreviewUpdateOutcome::Ready);
    CHECK(h.ask_calls == 1);      // only the initial decline
    CHECK(h.compute_calls == 1);
    CHECK_FALSE(h.flow.blocked());

    // Applying the recovered preview needs no further confirmation.
    h.work_pending = false;
    REQUIRE(h.apply());
    CHECK(h.ask_calls == 1);
    CHECK(h.applied_painted == 9000);
}
