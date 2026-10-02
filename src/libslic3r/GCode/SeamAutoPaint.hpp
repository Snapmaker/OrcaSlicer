#ifndef libslic3r_SeamAutoPaint_hpp_
#define libslic3r_SeamAutoPaint_hpp_

// Auto-paint seam (seam painting gizmo): turn the seams the slicer would pick for an Aligned seam position
// (SeamPlacer::plan_object_seams) into painted seam enforcers, so the seam stays there whatever the object's
// seam position setting, and the user can touch the result up by hand.
//
// The seams of consecutive layers are chained (a seam links to the nearest seam of the layer below when it is
// within the distance the aligner itself bridges), and every link is painted as a capsule around the segment
// between the two seams, projected onto the model surface; a seam with no link gets a sphere. The strokes go
// through TriangleSelector::select_patch, the same call the brush uses, so the triangles are split along the
// strip edge exactly like hand painting.

#include <algorithm>
#include <functional>
#include <vector>

#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r {
namespace SeamAutoPaint {

// One model part the strips may be painted on.
struct Volume
{
    // The part's mesh, in its own coordinates.
    const indexed_triangle_set *its = nullptr;
    // Mesh coordinates -> the planned seams' coordinates, i.e. PrintObject::trafo_centered() * ModelVolume::get_matrix().
    Transform3d                 trafo = Transform3d::Identity();
    // Where the strips go (built over the same mesh).
    TriangleSelector           *selector = nullptr;
};

struct Params
{
    // Half the width of the painted strip, in mm. Zero or less: the outer wall line width of each loop, so the strip
    // is two line widths wide.
    float radius = 0.f;
    // Also paint the seams of hole outlines (by default only the outer outline of each island is painted).
    bool  holes  = false;
};

struct Stats
{
    // Seams painted, links between consecutive layers painted as capsules, select_patch() calls made.
    size_t seams   = 0;
    size_t links   = 0;
    size_t strokes = 0;
};

// Strip radius used for a seam with the given line width.
inline float strip_radius(const Params &params, float flow_width)
{
    return params.radius > 0.f ? params.radius : std::max(flow_width, 0.2f);
}

// Paints ENFORCER strips along the seams in `layers` (one vector per layer, bottom up, as plan_object_seams()
// returns them) into the selectors of `volumes`. Each seam is painted on the part whose surface is closest to it,
// and also on any other part whose surface passes within half the strip radius of that point (a seam in the joint
// of two parts paints both). Existing paint is kept; reset the selectors first to replace it.
Stats paint(const std::vector<std::vector<SeamPlacer::PlannedSeam>> &layers,
            std::vector<Volume>                                    &volumes,
            const Params                                           &params,
            const std::function<void()>                            &throw_if_canceled = {});

} // namespace SeamAutoPaint
} // namespace Slic3r

#endif // libslic3r_SeamAutoPaint_hpp_
