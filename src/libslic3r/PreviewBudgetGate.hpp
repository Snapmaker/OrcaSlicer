#pragma once
#ifndef slic3r_PreviewBudgetGate_hpp_
#define slic3r_PreviewBudgetGate_hpp_

#include "TriangleMeshDeal.hpp"

#include <functional>

namespace Slic3r {

// How a preview-update frame ended; callers that may apply the caches
// (apply_subdivision) decide on this instead of inspecting cache internals.
enum class PreviewUpdateOutcome {
    Ready,          // nothing to do, or the computation ran to completion for the current inputs
    Blocked,        // the user declined, or an earlier decline still applies: old preview kept
    InputsChanged,  // the confirmation dialog pumped events that changed the inputs: re-evaluate
    NotConfirmed,   // a ConfirmedOnly recompute found no confirmation recorded for these inputs
    Unavailable,    // re-entered during a modal dialog, no model object, or the computation failed
};

// Whether a preview update may show the budget dialog: the panel asks; an
// apply-time forced recompute whose confirmation was already obtained for the
// same inputs must never show another one.
enum class BudgetPromptMode { Ask, ConfirmedOnly };

// Frame-level state machine of the local subdivision preview's face-count
// budget gate. Pure logic without GUI dependencies: the gizmo feeds it the
// current selection size and level, executes the returned verdict, and asks
// the user only on a Confirm verdict; the unit tests drive the same
// transitions directly.
//
// A decline blocks the preview only for the (level, selection size) state it
// was raised for: an equal or larger selection stays blocked without
// re-asking every frame (the estimate is monotonic in the painted face
// count), a smaller selection is re-checked cheaply and the block is lifted
// as soon as it fits the budget, and a different level re-evaluates from
// scratch. The caller only consults the gate on frames where preview work is
// actually pending.
class PreviewBudgetGate
{
public:
    enum class Verdict {
        // Compute the preview now: within the budget, the user confirmed, or
        // the block was just lifted because the selection shrank back under
        // the budget.
        Compute,
        // The requested level exceeds the budget: ask the user before
        // computing. resolve_confirmation() records the answer.
        Confirm,
        // A previous decline still applies to this selection state: compute
        // nothing and do not ask again.
        Blocked,
    };

    // Evaluates the gate for a frame whose preview work is pending.
    Verdict evaluate(int requested_level, size_t painted, size_t total, size_t budget)
    {
        if (m_blocked) {
            if (requested_level != m_blocked_level) {
                // The level changed: re-evaluate from scratch, the decline
                // belonged to another level.
                m_blocked = false;
            } else if (painted >= m_blocked_painted) {
                // Not smaller than the declined selection: the estimate is
                // still over the budget, keep the block silently.
                return Verdict::Blocked;
            } else if (painted == 0) {
                // An empty selection computes nothing: lift the block.
                m_blocked = false;
                return Verdict::Compute;
            } else if (TriangleMeshDeal::local_subdivision_exceeds_face_budget(total, painted, requested_level, budget)) {
                // Shrank, but still over: keep the block silently.
                return Verdict::Blocked;
            } else {
                // Shrank back under the budget: lift the block and compute.
                m_blocked = false;
                return Verdict::Compute;
            }
        }
        if (requested_level <= 0 || painted == 0 ||
            ! TriangleMeshDeal::local_subdivision_exceeds_face_budget(total, painted, requested_level, budget))
            return Verdict::Compute;
        m_pending_level   = requested_level;
        m_pending_painted = painted;
        return Verdict::Confirm;
    }

    // Records the user's answer to the Confirm verdict of the same frame.
    // Returns the level the stepper must display: the requested level on
    // accept, the level actually shown on decline (the preview stays at what
    // is displayed, so the requested level must follow it).
    int resolve_confirmation(bool accepted, int requested_level, int shown_level)
    {
        if (accepted) {
            m_blocked = false;
            return requested_level;
        }
        m_blocked         = true;
        m_blocked_level   = m_pending_level;
        m_blocked_painted = m_pending_painted;
        return shown_level;
    }

    bool blocked() const { return m_blocked; }

    // Forget any declined state (the selection was rebuilt from scratch or
    // the gizmo was reopened).
    void reset()
    {
        m_blocked         = false;
        m_blocked_level   = 0;
        m_blocked_painted = 0;
    }

private:
    bool   m_blocked         = false;
    int    m_blocked_level   = 0;
    size_t m_blocked_painted = 0;
    // Confirmation context stashed by evaluate(), consumed by
    // resolve_confirmation().
    int    m_pending_level   = 0;
    size_t m_pending_painted = 0;
};

// Budget-side orchestration of the local subdivision preview, shared by the
// gizmo and the unit tests. The gizmo feeds one frame's inputs and injects
// the two GUI actions (the dialog and the expensive preview computation); the
// tests inject counted stand-ins, so the exact decision sequence - including
// the apply-time re-confirmation - runs identically in both.
class PreviewBudgetFlow
{
public:
    struct FrameInputs {
        int    requested_level = 0;
        int    shown_level     = 0;
        size_t painted         = 0;
        size_t total           = 0;
        size_t budget          = 0;
        bool   has_work        = false; // preview work is pending (dirty or growth pending)
    };

    struct Result {
        PreviewUpdateOutcome outcome       = PreviewUpdateOutcome::Ready;
        int                  stepper_level = 0; // level the stepper must display afterwards
    };

    // Re-gathers the selection / mesh face counts (the dialog may pump
    // events, so the inputs the question was raised for may be stale).
    using GatherFn  = std::function<void(size_t &painted, size_t &total)>;
    // Shows the budget dialog; returns whether the user accepted.
    using DialogFn  = std::function<bool()>;
    // Runs the expensive preview computation; returns false when it could not
    // complete (the caller must then not apply anything).
    using ComputeFn = std::function<bool()>;

    Result update(const FrameInputs &inputs, BudgetPromptMode mode, const GatherFn &gather, const DialogFn &ask, const ComputeFn &compute)
    {
        Result result;
        result.stepper_level = inputs.requested_level;
        if (! inputs.has_work || inputs.requested_level <= 0)
            return result; // Ready: nothing to compute this frame

        // A recorded one-shot confirmation authorizes exactly one recompute
        // for its inputs; anything else voids it.
        if (m_confirmed) {
            if (m_confirmed_level == inputs.requested_level && m_confirmed_painted == inputs.painted) {
                m_confirmed = false;
                result.outcome = compute() ? PreviewUpdateOutcome::Ready : PreviewUpdateOutcome::Unavailable;
                return result;
            }
            m_confirmed = false; // the inputs no longer match the confirmation
        }

        const PreviewBudgetGate::Verdict verdict = m_gate.evaluate(inputs.requested_level, inputs.painted, inputs.total, inputs.budget);
        if (verdict == PreviewBudgetGate::Verdict::Blocked) {
            result.outcome = PreviewUpdateOutcome::Blocked;
            return result;
        }
        if (verdict == PreviewBudgetGate::Verdict::Confirm) {
            if (mode == BudgetPromptMode::ConfirmedOnly) {
                // The forced recompute found no confirmation recorded for the
                // current inputs: it must neither compute silently nor ask
                // again. "No dialog" is NOT a decline - the caller aborts the
                // apply without touching the gate's block state.
                result.outcome = PreviewUpdateOutcome::NotConfirmed;
                return result;
            }
            const bool accepted = ask();
            // The dialog pumps events: the answer only covers the inputs the
            // question was raised for. Re-check before using it.
            size_t fresh_painted = 0, fresh_total = 0;
            gather(fresh_painted, fresh_total);
            if (fresh_painted != inputs.painted || fresh_total != inputs.total) {
                result.outcome = PreviewUpdateOutcome::InputsChanged;
                return result;
            }
            result.stepper_level = m_gate.resolve_confirmation(accepted, inputs.requested_level, inputs.shown_level);
            if (! accepted) {
                result.outcome = PreviewUpdateOutcome::Blocked;
                return result;
            }
        }
        result.outcome = compute() ? PreviewUpdateOutcome::Ready : PreviewUpdateOutcome::Unavailable;
        return result;
    }

    // Apply-time authorization: lifts the stale block and, when the given
    // inputs still exceed the budget, records a one-shot confirmation that
    // authorizes exactly one ConfirmedOnly recompute for exactly those
    // inputs. Call only after the user accepted the re-confirmation.
    void authorize_recompute(int level, size_t painted, size_t total, size_t budget)
    {
        m_gate.reset();
        m_confirmed = false;
        if (level > 0 && painted > 0 &&
            TriangleMeshDeal::local_subdivision_exceeds_face_budget(total, painted, level, budget)) {
            m_confirmed         = true;
            m_confirmed_level   = level;
            m_confirmed_painted = painted;
        }
    }

    bool blocked() const { return m_gate.blocked(); }

    // Forget any declined or confirmed state (the selection was rebuilt from
    // scratch or the gizmo was reopened).
    void reset()
    {
        m_gate.reset();
        m_confirmed         = false;
        m_confirmed_level   = 0;
        m_confirmed_painted = 0;
    }

private:
    PreviewBudgetGate m_gate;
    bool              m_confirmed         = false;
    int               m_confirmed_level   = 0;
    size_t            m_confirmed_painted = 0;
};

} // namespace Slic3r

#endif // slic3r_PreviewBudgetGate_hpp_
