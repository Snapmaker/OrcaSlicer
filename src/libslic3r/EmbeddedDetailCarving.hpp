#ifndef slic3r_EmbeddedDetailCarving_hpp_
#define slic3r_EmbeddedDetailCarving_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

// Overlap-carving: an earlier-listed region counts as an embedded detail (e.g.
// embossed text or a color patch imported before its base plate) when at least
// EMBEDDED_DETAIL_CONTAINMENT_RATIO of its volume lies inside the later, larger
// region. Overlaps without embedding keep the upstream "last listed wins"
// carving. Measured on real ticket models (TS20261028000011 + seal case): the
// seal's color detail contains ~0.72 of its volume inside the surrounding part,
// while a peg crossing a ring's notch stays at ~0.35.
constexpr double EMBEDDED_DETAIL_CONTAINMENT_RATIO = 0.5;

// True when at least EMBEDDED_DETAIL_CONTAINMENT_RATIO of `earlier`'s volume lies
// inside `current` and `current` is not smaller than `earlier`. Both meshes must
// be in the same coordinate space. Returns false when the boolean intersection
// fails, keeping the upstream semantics.
bool earlier_is_embedded_detail(const indexed_triangle_set &earlier, const indexed_triangle_set &current);

} // namespace Slic3r

#endif // slic3r_EmbeddedDetailCarving_hpp_
