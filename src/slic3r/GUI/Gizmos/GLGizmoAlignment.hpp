#ifndef slic3r_GLGizmoAlignment_hpp_
#define slic3r_GLGizmoAlignment_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/AlignMath.hpp"
#include "slic3r/GUI/Selection.hpp"
#include <string>
#include <vector>
#include <functional>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;

class GLGizmoAlignment
{
public:
    enum class AlignType {
        NONE = -1,
        CENTER_X,
        CENTER_Y,
        CENTER_Z,
        Y_MAX,
        Y_MIN,
        X_MAX,
        X_MIN,
        Z_MAX,
        Z_MIN,
        DISTRIBUTE_X,
        DISTRIBUTE_Y,
        DISTRIBUTE_Z
    };
    struct ObjectInfo {
        int object_idx;
        int instance_idx;
        BoundingBoxf3 bbox;
        Vec3d center;

        ObjectInfo(int obj_idx, int inst_idx, const BoundingBoxf3& bb)
            : object_idx(obj_idx), instance_idx(inst_idx), bbox(bb), center(bb.center()) {}
    };

    // What the Align row asks for besides the button itself.
    struct AlignOptions {
        // Per axis (X, Y, Z): which point of each moved item (or of the whole selection in
        // plate / object mode) is brought to the target. Auto = the same side as the button.
        AlignMath::Origin origin[3] = {AlignMath::Origin::Auto, AlignMath::Origin::Auto, AlignMath::Origin::Auto};
        // true: align the selection (as one rigid group) to the plate / parent object box set with
        // set_parent_box(). false: align the selected items to each other.
        bool to_parent = false;
    };

    explicit GLGizmoAlignment(GLCanvas3D& canvas);
    ~GLGizmoAlignment() = default;

    // AlignType already encodes axis (0 X, 1 Y, 2 Z) and side; false for NONE and Distribute.
    static bool decode_align_type(AlignType type, int &axis, AlignMath::Side &side);

    bool align_objects(AlignType type, const AlignOptions &options);
    bool distribute_objects(AlignType type);

    bool distribute_x();
    bool distribute_y();
    bool distribute_z();

    bool can_align(AlignType type) const;
    bool can_distribute(AlignType type) const;

    std::vector<ObjectInfo> get_selected_objects_info(BoundingBoxf3 &big_bb) const;
    bool                    is_part_align_parent() const;
    // The reference for plate / object mode. `edge_inset` pulls edge targets inward per axis (the
    // 0.1 mm plate shrink); see AlignMath::AxisRequest::edge_inset.
    void                    set_parent_box(const BoundingBoxf3 &bb, const Vec3d &edge_inset = Vec3d::Zero());

    // The item that stays put in inter-item mode when an origin is not Auto: the last-selected
    // object (or part). Empty when it cannot be determined (the lowest index is then used).
    // The text is UTF-8 and ready to show.
    std::string             anchor_description() const;

private:
    GLCanvas3D& m_canvas;

    template<typename GetCoordFunc>
    bool distribute_objects_generic(GetCoordFunc get_coord, int axis,
                                  const std::string& operation_name);

    Selection& get_selection() const;
    void apply_transformation(int obj_idx, int inst_idx, const Vec3d& displacement);
    // Commits the moved GLVolumes into the model through ONE undo snapshot named `operation_name`.
    // Parts (force_volume_move) keep the "fix flying instances" pass; whole objects skip it so an
    // object can be placed on top of another and stay there (as Snap to surface does).
    void       finish_operation(const std::string &operation_name, bool force_volume_move = false);

    // True when the items of this alignment are volumes (parts) rather than whole instances.
    bool items_are_parts(bool to_parent) const;

    bool validate_selection_for_align() const;
    bool validate_selection_for_distribute() const;

private:
    BoundingBoxf3 m_parent_box;
    Vec3d         m_parent_inset{Vec3d::Zero()};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAlignment_hpp_
