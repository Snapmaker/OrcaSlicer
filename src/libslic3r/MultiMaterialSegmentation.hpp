#ifndef slic3r_MultiMaterialSegmentation_hpp_
#define slic3r_MultiMaterialSegmentation_hpp_

#include <utility>
#include <vector>

namespace Slic3r {

class ExPolygon;
class ModelVolume;
class PrintObject;
class PrintConfig;
class PrintObjectConfig;
class PrintRegionConfig;
class FacetsAnnotation;

using ExPolygons = std::vector<ExPolygon>;

struct ColoredLine
{
    Line line;
    int  color;
    int  poly_idx       = -1;
    int  local_line_idx = -1;
};

using ColoredLines = std::vector<ColoredLine>;

enum class IncludeTopAndBottomLayers {
    Yes,
    No
};

struct ModelVolumeFacetsInfo {
    const FacetsAnnotation &facets_annotation;
    // Indicate if model volume is painted.
    const bool              is_painted;
    // Indicate if the default extruder (TriangleStateType::NONE) should be replaced with the volume extruder.
    const bool              replace_default_extruder;
};

// Returns segmentation based on painting in segmentation gizmos.
std::vector<std::vector<ExPolygons>> segmentation_by_painting(const PrintObject                                               &print_object,
                                                              const std::function<ModelVolumeFacetsInfo(const ModelVolume &)> &extract_facets_info,
                                                              size_t                                                           num_facets_states,
                                                              float                                                            segmentation_max_width,
                                                              float                                                            segmentation_interlocking_depth,
                                                              // Wave A / C-1: the narrowest painted claim the band clamp's
                                                              // degradation ladder may emit (mm) - one external extrusion.
                                                              // Below it the clamp reverts to its pre-degradation no-op rather
                                                              // than cutting a sub-extrusion strip no toolpath can honour. See
                                                              // paint_depth_clamp_keep_core in the .cpp.
                                                              float                                                            segmentation_min_claim_width,
                                                              // WAVE B / Option N (.superpowers/sdd/2026-08-31-paint-depth/
                                                              // curved-gap-design.md): the painted claim's thickness measured
                                                              // NORMAL to the painted surface (mm), which bounds the top/bottom
                                                              // descent instead of top_shell_layers / bottom_shell_layers. 0
                                                              // means "no normal-thickness shell" - unlimited mode, and the fuzzy
                                                              // skin path, which has no top/bottom claim at all. See
                                                              // segmentation_top_and_bottom_layers in the .cpp.
                                                              float                                                            segmentation_normal_depth,
                                                              // ITEM 1 (.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-
                                                              // report.md, interclaim-sliver-investigation.md section 5 Option 1):
                                                              // the F1 wall-stack band width (mm, ext_perimeter_width +
                                                              // ext_perimeter_spacing, max across the object's printing regions),
                                                              // plumbed through to merge_segmented_layers's interior inter-claim
                                                              // absorb as its F1 guard. 0.f (paired with segmentation_normal_depth
                                                              // == 0.f) disables the absorb entirely.
                                                              float                                                            segmentation_wall_stack,
                                                              // ITEM 2 (interclaim-sliver-investigation.md loose end 3, shell-
                                                              // setting-and-gapfill-report.md): the WIDER kill width the #7104
                                                              // thin-projection filter uses when gap_infill_speed == 0 for at
                                                              // least one of the object's regions (mm), plumbed through to the
                                                              // absorb so its own kill width can track the wider sliver
                                                              // population that configuration produces instead of silently
                                                              // under-covering it. Fix-wave (absorb-tail-review.md Minor 4 / M4):
                                                              // PER COLOUR (indexed by extruder id) rather than a single object-
                                                              // wide max, 0.f for a colour with no gap-fill-disabled region of
                                                              // its own - see merge_segmented_layers's absorb stage in the .cpp.
                                                              // Empty (the fuzzy skin caller's value) is inert.
                                                              const std::vector<float>                                       &segmentation_claim_width_gapfill_off_by_color,
                                                              bool                                                             segmentation_interlocking_beam,
                                                              IncludeTopAndBottomLayers                                        include_top_and_bottom_layers,
                                                              const std::function<void()>                                     &throw_on_cancel_callback);

// ITEM 1 (.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-report.md): given a thin,
// fully-interior base island and the final per-colour claims on its layer (index 0 = base,
// ignored; index >= 1 = painted colours, the only eligible neighbours), returns the 1-based
// colour index of the claim with the largest shared area against the island dilated by `eps` -
// ties broken by the LOWEST colour index. Returns 0 if no painted claim touches the (dilated)
// island at all. Exposed (not static) so it is directly unit-testable. Determinism note
// (corrected by absorb-tail-review.md Minor 1 - this comment previously and WRONGLY called the
// comparison "exact-integer"): the area comparison is DOUBLE arithmetic (ExPolygon::area()),
// deterministic because the same inputs summed in the same order by the same binary always
// produce the same double, not because it is integer-exact - see the definition's own comment
// for the full argument and why a genuine int64 rewrite was considered and rejected.
size_t interclaim_absorb_winner(const ExPolygons &island, const std::vector<ExPolygons> &painted_claims, float eps);

// Fix-wave (absorb-tail-review.md Minor 4 / M4): resolves the interior inter-claim absorb's
// effective claim-width threshold for ONE base island from only the painted colours that
// actually border it (touch its `eps`-dilated outline - the SAME adjacency test
// interclaim_absorb_winner itself uses), rather than an object-wide MAX. claim_width_gapfill_
// off_by_color is indexed by colour (extruder id); a colour with no gap-fill-disabled region of
// its own, or an out-of-range index, contributes 0.f (no widening from that neighbour). Returns
// min_claim_width unchanged whenever none of the island's actual geometric neighbours has gap
// fill disabled, even if some OTHER, non-bordering colour elsewhere on the object does - fixing
// the pre-fix object-wide MAX's over-absorption of a genuine 0.45-0.75mm base gap between two
// colours that both have gap fill on. Exposed (not static) for the same reason as
// interclaim_absorb_winner above: directly unit-testable, independent of mesh/Clipper geometry.
float interclaim_absorb_effective_claim_width(const ExPolygons &island, const std::vector<ExPolygons> &painted_claims,
                                               const std::vector<float> &claim_width_gapfill_off_by_color,
                                               float min_claim_width, float eps);

// Returns multi-material segmentation based on painting in multi-material segmentation gizmo
std::vector<std::vector<ExPolygons>> multi_material_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

// Returns fuzzy skin segmentation based on painting in fuzzy skin segmentation gizmo
std::vector<std::vector<ExPolygons>> fuzzy_skin_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

// Effective outer-wall line width for a region, resolved against its own nozzle with PrintRegion::flow's fallback.
double resolve_outer_wall_line_width(const PrintRegionConfig &region_config, const PrintObjectConfig &object_config, const PrintConfig &print_config);

} // namespace Slic3r

namespace boost::polygon {
template<> struct geometry_concept<Slic3r::ColoredLine>
{
    typedef segment_concept type;
};

template<> struct segment_traits<Slic3r::ColoredLine>
{
    typedef coord_t       coordinate_type;
    typedef Slic3r::Point point_type;

    static inline point_type get(const Slic3r::ColoredLine &line, const direction_1d &dir)
    {
        return dir.to_int() ? line.line.b : line.line.a;
    }
};
} // namespace boost::polygon

#endif // slic3r_MultiMaterialSegmentation_hpp_
