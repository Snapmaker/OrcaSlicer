#ifndef slic3r_EmbeddedDetailCarving_hpp_
#define slic3r_EmbeddedDetailCarving_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

// Overlap-carving band for slices_to_regions(): an earlier-listed region counts
// as an embedded, protruding detail (e.g. embossed text or a color patch imported
// before its base plate) when the later, larger region contains at least
// EMBEDDED_DETAIL_PARTIAL_RATIO but less than EMBEDDED_DETAIL_FULL_RATIO of its
// volume. Measured on real ticket models (TS20261028000011 + seal case): the
// seal's color detail sits at ~0.72 containment inside the surrounding part,
// while a peg crossing a ring's notch stays at ~0.35 and color patches fully
// hidden inside the body shell are at 1.0.
constexpr double EMBEDDED_DETAIL_PARTIAL_RATIO = 0.4;  // >= : protruding detail flips
constexpr double EMBEDDED_DETAIL_FULL_RATIO    = 0.95; // >= : fully hidden, body wins

// True when the overlap volume is in the partial band [PARTIAL, FULL) x
// volume(earlier): the earlier region is a protruding detail and keeps the
// overlap. Outside the band (mere crossing, or fully hidden inside the body)
// the upstream "last listed wins" carving applies. Both meshes must be in the
// same coordinate space. Returns false when the boolean intersection fails.
bool earlier_is_embedded_detail(const indexed_triangle_set &earlier, const indexed_triangle_set &current);

} // namespace Slic3r

#endif // slic3r_EmbeddedDetailCarving_hpp_
