#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"

#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects);

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
    const ModelObjectPtrs& perform_by_contour(std::vector<Part> parts, int dowels_count);
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
