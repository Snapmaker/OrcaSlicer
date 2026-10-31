#ifndef slic3r_EmbeddedDetailCarving_hpp_
#define slic3r_EmbeddedDetailCarving_hpp_

#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

// Overlap-carving ratios used by slices_to_regions(): an earlier-listed region
// counts as an embedded detail (e.g. embossed text or a color patch imported
// before its base plate) when the current, larger region covers at least
// EMBEDDED_DETAIL_COVER_RATIO of it and is at least EMBEDDED_DETAIL_SIZE_RATIO
// times its area. Anything else keeps the upstream "last listed wins" carving.
constexpr double EMBEDDED_DETAIL_COVER_RATIO = 0.9;
constexpr double EMBEDDED_DETAIL_SIZE_RATIO  = 1.1;

// True when `earlier` behaves as an embedded detail: `current` is larger and
// covers (nearly) all of it, so the overlapping slices belong to `earlier`
// instead of the later-listed `current` region.
inline bool earlier_is_embedded_detail(const ExPolygons &earlier, const ExPolygons &current)
{
    const double area_earlier = area(earlier);
    const double area_current = area(current);
    if (area_earlier <= 0. || area_current < EMBEDDED_DETAIL_SIZE_RATIO * area_earlier)
        return false;
    return area(intersection_ex(earlier, current)) >= EMBEDDED_DETAIL_COVER_RATIO * area_earlier;
}

} // namespace Slic3r

#endif // slic3r_EmbeddedDetailCarving_hpp_
