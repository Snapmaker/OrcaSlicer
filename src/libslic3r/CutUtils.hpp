#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"

#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo, KeepPaint };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects, const std::vector<std::pair<std::optional<TriangleSelector::SavedPainting>, Transform3d>>& saved_paintings,
                  const std::vector<std::pair<ModelVolume*, int>>& output_sources = {});

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    struct Groove
    {
        float depth{ 0.f };
        float width{ 0.f };
        float flaps_angle{ 0.f };
        float angle{ 0.f };
        float depth_init{ 0.f };
        float width_init{ 0.f };
        float flaps_angle_init{ 0.f };
        float angle_init{ 0.f };
        float depth_tolerance{ 0.1f };
        float width_tolerance{ 0.1f };
    };

    struct Part
    {
        bool selected;
        bool is_modifier;
    };

    const ModelObjectPtrs& perform_with_plane();
    const ModelObjectPtrs& perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count);

    // Preprocessing the cut gizmo runs before a by-contour cut: a plane pre-cut
    // into the upper/lower parts (kept as whole volumes), then a split of every
    // splittable part into its connected components. Each source volume's
    // painting is carried through both steps ALWAYS, independently of the
    // keep_painting preference: the selection built on this model lives until
    // the user performs the cut and the preference may change in between. The
    // final cut alone decides, through its KeepPaint attribute, whether the
    // painting is written back or dropped.
    static Model prepare_for_contour_cut(const ModelObject& object, int instance_idx, const Transform3d& cut_matrix);

    /// Cut the object with a single dovetail groove (legacy signature).
    /// \param groove Dovetail groove dimensions and angles.
    /// \param rotation_m Rotation from world to the groove coordinate system.
    /// \param keep_as_parts Keep the cut result as a single object when true.
    /// \return Resulting model objects after the cut.
    const ModelObjectPtrs& perform_with_groove(const Groove&      groove,
                                               const Transform3d& rotation_m,
                                               bool               keep_as_parts = false);

    /// Cut the object with one or more parallel dovetail grooves.
    /// \param groove Dovetail groove dimensions and angles.
    /// \param rotation_m Rotation from world to the groove coordinate system.
    /// \param groove_count Number of parallel grooves to cut.
    /// \param groove_gap Gap between adjacent grooves.
    /// \param radius Cut plane radius used to compute the groove spacing width.
    /// \param keep_as_parts Keep the cut result as a single object when true.
    /// \return Resulting model objects after the cut.
    const ModelObjectPtrs& perform_with_groove(const Groove&      groove,
                                               const Transform3d& rotation_m,
                                               const int          groove_count,
                                               const float        groove_gap,
                                               const float        radius,
                                               bool               keep_as_parts = false);

    /// Calculate the outer X width of one dovetail groove in the cut plane.
    /// \param groove Dovetail groove dimensions and angles.
    /// \param radius Cut plane radius used by the preview mesh.
    /// \return Full outer groove width used for spacing multiple grooves.
    static float calculate_groove_width(const Cut::Groove& groove, const float radius);

}; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
