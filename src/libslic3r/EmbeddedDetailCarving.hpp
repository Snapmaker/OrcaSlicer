#ifndef slic3r_EmbeddedDetailCarving_hpp_
#define slic3r_EmbeddedDetailCarving_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

// Overlap-carving gate for slices_to_regions(): when the developer switch
// (enable_volume_containment_carving) is on, a cross-region volume pair whose
// 3D containment c = volume(earlier ∩ later) / volume(earlier) reaches
// EMBEDDED_AREA_ARBITRATION_RATIO is treated as an embedded pair (an inserted
// detail, a thin color skin, or decorations on a larger body). For those pairs
// each layer is carved by the smaller per-layer slice area - the rule 2.4.1
// applied everywhere, here confined to the embedded domain. Pairs below the
// ratio (mere crossings like a peg entering a notch, or the plaque-vs-relief
// case) keep the upstream "last listed wins" carving.
// Measured on real ticket models: seal body 0.96, seal details ~1.0, peg
// crossing a ring's notch 0.35, plaque mountain covered by text 0.45.
constexpr double EMBEDDED_AREA_ARBITRATION_RATIO = 0.6;

// True when the overlap containment reaches EMBEDDED_AREA_ARBITRATION_RATIO:
// the pair is embedded and each layer is carved by the smaller slice area.
// Below the ratio the upstream "last listed wins" carving applies. Both meshes
// must be in the same coordinate space. Returns false on measurement failure.
bool earlier_is_embedded_detail(const indexed_triangle_set &earlier, const indexed_triangle_set &current);

} // namespace Slic3r

#endif // slic3r_EmbeddedDetailCarving_hpp_
