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
        // Align selected only: which item stays put when an origin is not Auto. An explicit item
        // (anchor_item_set) wins while it is still selected; otherwise the mode decides.
        AlignMath::AnchorMode anchor_mode = AlignMath::AnchorMode::Last;
        bool                  anchor_item_set = false;
        int                   anchor_object_idx = -1;
        int                   anchor_instance_idx = -1;
        int                   anchor_volume_idx = -1; // -1 for a whole instance
    };

    // One selectable anchor: an object instance, or a part when parts are being aligned.
    struct AnchorCandidate {
        int         object_idx;
        int         instance_idx;
        int         volume_idx; // -1 for a whole instance
        std::string label;      // UTF-8; repeated names carry the instance / part number
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

    // The selected items an anchor can be chosen from, in the order align_objects() sees them.
    std::vector<AnchorCandidate> anchor_candidates() const;
    // Index into anchor_candidates() of the item that stays put for these options, or -1 when
    // nothing is fixed (anchor mode None). `explicit_missing` is set when an explicitly chosen item
    // has left the selection (the result is then the Last-selected item).
    int                          resolve_anchor_candidate(const AlignOptions &options, bool *explicit_missing = nullptr) const;

private:
    GLCanvas3D& m_canvas;

    // One thing that gets moved: a whole instance (volume_idx < 0) or one part of an instance.
    struct AlignItem {
        int           object_idx;
        int           instance_idx;
        int           volume_idx;
        BoundingBoxf3 box; // world space axis-aligned box (rotation is already baked in)
    };
    std::vector<AlignItem> collect_items(bool parts) const;
    AlignMath::AnchorPick  pick_anchor(const std::vector<AlignItem> &items, bool parts, const AlignOptions &options) const;

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
