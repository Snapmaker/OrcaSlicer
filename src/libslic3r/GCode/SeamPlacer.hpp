#ifndef libslic3r_SeamPlacer_hpp_
#define libslic3r_SeamPlacer_hpp_

#include <limits>
#include <optional>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/KDTreeIndirect.hpp"

namespace Slic3r {

class PrintObject;
class ExtrusionLoop;
class Print;
class Layer;

namespace EdgeGrid {
class Grid;
}

namespace SeamPlacerImpl {


struct GlobalModelInfo;
struct SeamComparator;

enum class EnforcedBlockedSeamPoint {
  Blocked = 0,
  Neutral = 1,
  Enforced = 2,
};

// struct representing single perimeter loop
struct Perimeter {
  size_t start_index{};
  size_t end_index{}; //inclusive!
  size_t seam_index{};
  float flow_width{};

  // During alignment, a final position may be stored here. In that case, finalized is set to true.
  // Note that final seam position is not limited to points of the perimeter loop. In theory it can be any position
  // Random position also uses this flexibility to set final seam point position
  bool finalized = false;
  Vec3f final_seam_position = Vec3f::Zero();
};

//Struct over which all processing of perimeters is done. For each perimeter point, its respective candidate is created,
// then all the needed attributes are computed and finally, for each perimeter one point is chosen as seam.
// This seam position can be then further aligned
struct SeamCandidate {
  SeamCandidate(const Vec3f &pos, Perimeter &perimeter,
                float local_ccw_angle,
                EnforcedBlockedSeamPoint type) :
                                                 position(pos), perimeter(perimeter), visibility(0.0f), overhang(0.0f), embedded_distance(0.0f), local_ccw_angle(
                                                                                                                                                     local_ccw_angle), type(type), central_enforcer(false) {
  }
  const Vec3f position;
  // pointer to Perimeter loop of this point. It is shared across all points of the loop
  Perimeter &perimeter;
  float visibility;
  float overhang;
  float unsupported_dist;
  // distance inside the merged layer regions, for detecting perimeter points which are hidden indside the print (e.g. multimaterial join)
  // Negative sign means inside the print, comes from EdgeGrid structure
  float embedded_distance;
  float local_ccw_angle;
  EnforcedBlockedSeamPoint type;
  bool central_enforcer; //marks this candidate as central point of enforced segment on the perimeter - important for alignment
  // Part joints (seam_prefer_part_joints): the point lies where two parts of the object (or a touching
  // object) meet. Only ever set for the aligned seam positions, and only when the object has touching parts,
  // so for everything else these stay false and the comparator never sees them differ.
  bool part_joint = false;
  // Middle of a run of part_joint points on this perimeter (one per run), the joint's own "central enforcer".
  bool central_part_joint = false;
};

struct SeamCandidateCoordinateFunctor {
  SeamCandidateCoordinateFunctor(const std::vector<SeamCandidate> &seam_candidates) :
                                                                                      seam_candidates(seam_candidates) {
  }
  const std::vector<SeamCandidate> &seam_candidates;
  float operator()(size_t index, size_t dim) const {
    return seam_candidates[index].position[dim];
  }
};
} // namespace SeamPlacerImpl

struct PrintObjectSeamData
{
  using SeamCandidatesTree = KDTreeIndirect<3, float, SeamPlacerImpl::SeamCandidateCoordinateFunctor>;

  struct LayerSeams
  {
    Slic3r::deque<SeamPlacerImpl::Perimeter> perimeters;
    std::vector<SeamPlacerImpl::SeamCandidate> points;
    std::unique_ptr<SeamCandidatesTree> points_tree;
  };
  // Map of PrintObjects (PO) -> vector of layers of PO -> vector of perimeter
  std::vector<LayerSeams> layers;
  // Map of PrintObjects (PO) -> vector of layers of PO -> unique_ptr to KD
  // tree of all points of the given layer

  void clear()
  {
    layers.clear();
  }
};

class SeamPlacer {
public:
  // Number of samples generated on the mesh. There are sqr_rays_per_sample_point*sqr_rays_per_sample_point rays casted from each samples
  static constexpr size_t raycasting_visibility_samples_count = 30000;
  static constexpr size_t fast_decimation_triangle_count_target = 16000;
  //square of number of rays per sample point
  static constexpr size_t sqr_rays_per_sample_point = 5;

  // snapping angle - angles larger than this value will be snapped to during seam painting
  static constexpr float sharp_angle_snapping_threshold = 55.0f * float(PI) / 180.0f;
  // overhang angle for seam placement that still yields good results, in degrees, measured from vertical direction
  static constexpr float overhang_angle_threshold = 45.0f * float(PI) / 180.0f;

  // determines angle importance compared to visibility ( neutral value is 1.0f. )
  static constexpr float angle_importance_aligned = 0.6f;
  static constexpr float angle_importance_nearest = 1.0f; // use much higher angle importance for nearest mode, to combat the visibility info noise

  // For long polygon sides, if they are close to the custom seam drawings, they are oversampled with this step size
  static constexpr float enforcer_oversampling_distance = 0.2f;

  // When searching for seam clusters for alignment:
  // following value describes, how much worse score can point have and still be picked into seam cluster instead of original seam point on the same layer
  static constexpr float seam_align_score_tolerance = 0.3f;
  // seam_align_tolerable_dist_factor - how far to search for seam from current position, final dist is seam_align_tolerable_dist_factor * flow_width
  static constexpr float seam_align_tolerable_dist_factor = 4.0f;
  // minimum number of seams needed in cluster to make alignment happen
  static constexpr size_t seam_align_minimum_string_seams = 6;
  // millimeters covered by spline; determines number of splines for the given string
  static constexpr size_t seam_align_mm_per_segment = 4.0f;

  //The following data structures hold all perimeter points for all PrintObject.
  std::unordered_map<const PrintObject*, PrintObjectSeamData> m_seam_per_object;

  void init(const Print &print, std::function<void(void)> throw_if_canceled_func);

  void place_seam(const Layer *layer, ExtrusionLoop &loop, const Point &last_pos, float& overhang) const;

  // The seam the slicer picks for one outer wall loop (Auto-paint seam, see plan_object_seams()).
  struct PlannedSeam
  {
    // Print object's centred coordinates (PrintObject::trafo_centered()), on the loop; z is the layer's slice_z.
    Vec3f  position   = Vec3f::Zero();
    // The outer wall line width of the loop.
    float  flow_width = 0.f;
    // The loop is the outline of a hole, not the outer outline of an island.
    bool   is_hole    = false;
  };

  // Where the slicer would put the seam of every outer wall loop of `po` (one vector per entry of po.layers()),
  // computed for `seam_position` and `prefer_part_joints` instead of the object's own seam_position and
  // seam_prefer_part_joints, which are left untouched. The positions are those init() stores for the aligned,
  // back and random modes (place_seam() then splits the outer loop there); spNearest, which depends on the
  // nozzle position, gets the best point of each loop by the same comparator. With use_painted_seams false,
  // painted seam enforcers and blockers are ignored, as if the object had none.
  // `po` must have its perimeters (posPerimeters done); `print` is the Print that owns it (other objects of it
  // are only looked at for joints with touching objects, and need no slices).
  static std::vector<std::vector<PlannedSeam>> plan_object_seams(const Print &print, const PrintObject &po,
                                                                  SeamPosition seam_position, bool prefer_part_joints,
                                                                  bool use_painted_seams,
                                                                  const std::function<void(void)> &throw_if_canceled_func);
private:
  void init_object(const Print &print, const PrintObject *po, SeamPosition seam_position, bool prefer_part_joints,
                   bool use_painted_seams, const std::function<void(void)> &throw_if_canceled_func);
  void gather_seam_candidates(const PrintObject *po, const SeamPlacerImpl::GlobalModelInfo &global_model_info);
  void calculate_candidates_visibility(const PrintObject *po,
                                       const SeamPlacerImpl::GlobalModelInfo &global_model_info);
  void calculate_overhangs_and_layer_embedding(const PrintObject *po);
  void align_seam_points(const PrintObject *po, const SeamPlacerImpl::SeamComparator &comparator);
  std::vector<std::pair<size_t, size_t>> find_seam_string(const PrintObject *po,
                                                          std::pair<size_t, size_t> start_seam,
                                                          const SeamPlacerImpl::SeamComparator &comparator) const;
  std::optional<std::pair<size_t, size_t>> find_next_seam_in_layer(
      const std::vector<PrintObjectSeamData::LayerSeams> &layers,
      const Vec3f& projected_position,
      const size_t layer_idx, const float max_distance,
      const SeamPlacerImpl::SeamComparator &comparator) const;
};

} // namespace Slic3r

#endif // libslic3r_SeamPlacer_hpp_
