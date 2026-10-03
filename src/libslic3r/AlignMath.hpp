#pragma once

// Pure one-axis alignment maths for the Move gizmo's "Align" row (no wx, no Selection, no Model).
//
// Vocabulary
//   Button  (Side)    which place the move goes to: the Min / Center / Max of the reference.
//   Origin            which point of each MOVED item is brought there: Auto, Center, Min or Max.
//                     Auto means "the same side as the button" (edge to edge for Min/Max, centre
//                     to centre for Center), i.e. the behaviour the panel always had.
//   Reference         what the button's side is measured on:
//                       Union   the extremes of the items themselves (inter-item, Auto behaviour),
//                       Anchor  one of the items, which stays put (inter-item with an origin),
//                       Fixed   a fixed span (the plate or the parent object); the items then move
//                               as one rigid group so their spacing is preserved.
//
// Min / Max are the lower / higher world coordinate on the axis: Left / Right on X, Front / Back
// on Y, Bottom / Top on Z. Every span is the world-space extent of one item's axis-aligned box, so
// rotated items are handled through their bounding box.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace AlignMath {

enum class Side { Min, Center, Max };

// Order matches the dropdown: Auto, Center, then the low edge, then the high edge.
enum class Origin { Auto = 0, Center = 1, Min = 2, Max = 3 };

enum class Reference { Union, Anchor, Fixed };

// Displacements below this are returned as exactly zero (same guard the panel always used).
constexpr double ALIGN_EPSILON = 1e-6;

// The side of a moved item that is brought to the target.
inline Side pick_side(Origin origin, Side button)
{
    switch (origin) {
    case Origin::Center: return Side::Center;
    case Origin::Min:    return Side::Min;
    case Origin::Max:    return Side::Max;
    case Origin::Auto:
    default:             return button;
    }
}

// Stable config strings: "auto", "center", "min", "max".
inline const char *origin_key(Origin origin)
{
    switch (origin) {
    case Origin::Center: return "center";
    case Origin::Min:    return "min";
    case Origin::Max:    return "max";
    case Origin::Auto:
    default:             return "auto";
    }
}

// Unknown text falls back to Auto, so a stale or hand-edited config can never break the panel.
inline Origin origin_from_key(const std::string &key)
{
    if (key == "center") return Origin::Center;
    if (key == "min")    return Origin::Min;
    if (key == "max")    return Origin::Max;
    return Origin::Auto;
}

// World extent of one item on one axis.
struct Span
{
    double lo = 0.;
    double hi = 0.;

    double center() const { return 0.5 * (lo + hi); }
    double at(Side side) const
    {
        switch (side) {
        case Side::Min: return lo;
        case Side::Max: return hi;
        case Side::Center:
        default:        return center();
        }
    }
};

inline Span union_of(const std::vector<Span> &spans)
{
    Span u = spans.empty() ? Span{} : spans.front();
    for (const Span &s : spans) {
        u.lo = std::min(u.lo, s.lo);
        u.hi = std::max(u.hi, s.hi);
    }
    return u;
}

struct AxisRequest
{
    Side      button    = Side::Min;
    Origin    origin    = Origin::Auto;
    Reference reference = Reference::Union;
    // Reference::Anchor: index into the span list of the item that stays put. An index out of
    // range falls back to Reference::Union.
    std::size_t anchor = 0;
    // Reference::Fixed: the span the button's side is measured on (the plate, the parent object).
    Span fixed;
    // Reference::Fixed only: pulls an EDGE target inward by this much (the 0.1 mm plate shrink).
    // It is applied only when an edge of the items goes to the same-side edge of `fixed`, i.e.
    // when the item stays inside the span. A centre target, or an edge that deliberately goes to
    // the opposite edge (origin Left, button Right), is exact.
    double edge_inset = 0.;
};

// One displacement per input span, along this single axis. Items that already sit on the target
// get exactly 0, and so does the anchor.
inline std::vector<double> axis_offsets(const std::vector<Span> &spans, const AxisRequest &req)
{
    std::vector<double> out(spans.size(), 0.);
    if (spans.empty())
        return out;

    const Side pick = pick_side(req.origin, req.button);
    Reference  ref  = req.reference;
    if (ref == Reference::Anchor && req.anchor >= spans.size())
        ref = Reference::Union;

    switch (ref) {
    case Reference::Anchor: {
        const double target = spans[req.anchor].at(req.button);
        for (std::size_t i = 0; i < spans.size(); ++i)
            if (i != req.anchor)
                out[i] = target - spans[i].at(pick);
        break;
    }
    case Reference::Fixed: {
        double target = req.fixed.at(req.button);
        if (req.edge_inset != 0. && req.button != Side::Center && pick == req.button)
            target += (req.button == Side::Min) ? req.edge_inset : -req.edge_inset;
        const double delta = target - union_of(spans).at(pick);
        std::fill(out.begin(), out.end(), delta);
        break;
    }
    case Reference::Union:
    default: {
        const double target = union_of(spans).at(req.button);
        for (std::size_t i = 0; i < spans.size(); ++i)
            out[i] = target - spans[i].at(pick);
        break;
    }
    }

    for (double &d : out)
        if (std::abs(d) < ALIGN_EPSILON)
            d = 0.;
    return out;
}

} // namespace AlignMath
} // namespace Slic3r
