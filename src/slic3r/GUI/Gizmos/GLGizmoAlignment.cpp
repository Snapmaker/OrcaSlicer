#include "GLGizmoAlignment.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>

namespace Slic3r {
namespace GUI {


// AlignType already encodes the axis and the side, so nothing is parsed out of a name.
bool GLGizmoAlignment::decode_align_type(AlignType type, int &axis, AlignMath::Side &side)
{
    using T = AlignType;
    using S = AlignMath::Side;
    switch (type) {
    case T::X_MIN:    axis = 0; side = S::Min;    return true;
    case T::CENTER_X: axis = 0; side = S::Center; return true;
    case T::X_MAX:    axis = 0; side = S::Max;    return true;
    case T::Y_MIN:    axis = 1; side = S::Min;    return true;
    case T::CENTER_Y: axis = 1; side = S::Center; return true;
    case T::Y_MAX:    axis = 1; side = S::Max;    return true;
    case T::Z_MIN:    axis = 2; side = S::Min;    return true;
    case T::CENTER_Z: axis = 2; side = S::Center; return true;
    case T::Z_MAX:    axis = 2; side = S::Max;    return true;
    default:          return false;
    }
}

GLGizmoAlignment::GLGizmoAlignment(GLCanvas3D& canvas) : m_canvas(canvas)
{
}

bool GLGizmoAlignment::items_are_parts(bool to_parent) const
{
    const Selection &selection = get_selection();
    if (to_parent)
        return is_part_align_parent();
    // Inter-item: parts of one object, or several loose parts / modifiers.
    return selection.is_single_full_object() || selection.is_multiple_volume() || selection.is_multiple_modifier() ||
           selection.is_single_volume() || selection.is_single_modifier();
}

bool GLGizmoAlignment::align_objects(AlignType type, const AlignOptions &options)
{
    int             axis = 0;
    AlignMath::Side side = AlignMath::Side::Min;
    if (!decode_align_type(type, axis, side) || !validate_selection_for_align())
        return false;

    Selection &selection = get_selection();
    const bool parts     = items_are_parts(options.to_parent);

    const std::vector<AlignItem> items = collect_items(parts);
    if (items.empty())
        return false;

    AlignMath::AxisRequest request;
    request.button = side;
    request.origin = options.origin[axis];
    if (options.to_parent) {
        request.reference  = AlignMath::Reference::Fixed;
        request.fixed      = {m_parent_box.min[axis], m_parent_box.max[axis]};
        request.edge_inset = m_parent_inset[axis];
    } else if (request.origin != AlignMath::Origin::Auto) {
        // The anchor stays put (Last / First / a chosen item), or with mode None nothing is
        // fixed and the items go to the selection's own extremes.
        const AlignMath::AnchorPick anchor = pick_anchor(items, parts, options);
        if (anchor.use_union) {
            request.reference = AlignMath::Reference::Union;
        } else {
            request.reference = AlignMath::Reference::Anchor;
            request.anchor    = anchor.index;
        }
    } else {
        request.reference = AlignMath::Reference::Union;
    }

    std::vector<AlignMath::Span> spans;
    spans.reserve(items.size());
    for (const AlignItem &item : items)
        spans.push_back({item.box.min[axis], item.box.max[axis]});
    const std::vector<double> offsets = AlignMath::axis_offsets(spans, request);

    selection.setup_cache();
    bool moved = false;
    for (size_t i = 0; i < items.size(); ++i) {
        if (offsets[i] == 0.)
            continue;
        Vec3d displacement = Vec3d::Zero();
        displacement[axis] = offsets[i];
        if (parts)
            selection.translate(items[i].object_idx, items[i].instance_idx, items[i].volume_idx, displacement, false);
        else
            apply_transformation(items[i].object_idx, items[i].instance_idx, displacement);
        moved = true;
    }
    if (!moved)
        return true; // already aligned: no model change, so no empty undo step either

    // The Move gizmo's own translate does this too: without it the same part of the object's
    // other instances keeps its old transform and do_move() may write that one back.
    if (parts)
        selection.synchronize_unselected_volumes();

    const std::string name = options.to_parent ? (parts ? _u8L("Align to object") : _u8L("Align to plate")) : _u8L("Align selected");
    finish_operation(name, parts);
    return true;
}

bool GLGizmoAlignment::distribute_objects(AlignType type)
{
    if (!can_distribute(type)) {
        return false;
    }

    switch (type) {
    case AlignType::DISTRIBUTE_X:
        return distribute_x();
    case AlignType::DISTRIBUTE_Y:
        return distribute_y();
    case AlignType::DISTRIBUTE_Z:
        return distribute_z();
    default:
            return false;
    }
}

bool GLGizmoAlignment::distribute_y()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.y(); },
        1,
        _u8L("Distribute")
    );
}

bool GLGizmoAlignment::distribute_x()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.x(); },
        0,
        _u8L("Distribute")
    );
}

bool GLGizmoAlignment::distribute_z()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.z(); },
        2,
        _u8L("Distribute")
    );
}

template<typename GetCoordFunc>
bool GLGizmoAlignment::distribute_objects_generic(GetCoordFunc get_coord, int axis, const std::string& operation_name)
{
    const Selection& selection = get_selection();

    if (selection.is_single_full_object() || selection.is_multiple_volume() || selection.is_multiple_modifier()) {
        std::vector<const GLVolume*> volumes;
        for (unsigned int idx : selection.get_volume_idxs()) {
            const GLVolume* v = selection.get_volume(idx);
            if (v) volumes.push_back(v);
        }

        if (volumes.size() < 3) return false;
        get_selection().setup_cache();

        struct VolEntry { const GLVolume* v; double coord; };
        std::vector<VolEntry> entries;
        entries.reserve(volumes.size());
        for (const GLVolume* v : volumes) {
            BoundingBoxf3 bbox = v->transformed_convex_hull_bounding_box();
            Vec3d c = bbox.center();
            double coord = (axis == 0 ? c.x() : axis == 1 ? c.y() : c.z());
            entries.push_back({v, coord});
        }

        std::sort(entries.begin(), entries.end(), [](const VolEntry& a, const VolEntry& b){ return a.coord < b.coord; });

        BoundingBoxf3 selection_bbox = selection.get_bounding_box();
        BoundingBoxf3 left_bbox      = entries.front().v->transformed_convex_hull_bounding_box();
        BoundingBoxf3 right_bbox     = entries.back().v->transformed_convex_hull_bounding_box();
        auto   axis_extent = [&](const BoundingBoxf3 &bb) { return (axis == 0 ? (bb.max.x() - bb.min.x()) : axis == 1 ? (bb.max.y() - bb.min.y()) : (bb.max.z() - bb.min.z())); };
        double left_width  = axis_extent(left_bbox);
        double right_width = axis_extent(right_bbox);
        double bbox_min    = (axis == 0 ? selection_bbox.min.x() : axis == 1 ? selection_bbox.min.y() : selection_bbox.min.z());
        double bbox_max    = (axis == 0 ? selection_bbox.max.x() : axis == 1 ? selection_bbox.max.y() : selection_bbox.max.z());
        double min_center  = bbox_min + left_width * 0.5;
        double max_center  = bbox_max - right_width * 0.5;
        double total_distance = max_center - min_center;
        double interval       = (entries.size() > 1) ? total_distance / (entries.size() - 1) : 0;
        for (size_t i = 1; i < entries.size() - 1; ++i) {
            double target_coord  = min_center + i * interval;
            double current_coord = entries[i].coord;
            double offset        = target_coord - current_coord;

            if (std::abs(offset) > 1e-6) {
                Vec3d displacement = Vec3d::Zero();
                displacement[axis] = offset;
                const GLVolume *v  = entries[i].v;
                get_selection().translate(v->object_idx(), v->instance_idx(), v->volume_idx(), displacement);
            }
        }

        get_selection().synchronize_unselected_volumes();
        finish_operation(operation_name,true);
        return true;
    }
    BoundingBoxf3 big_bb;
    auto          objects = get_selected_objects_info(big_bb);
    if (objects.size() < 3) return false;

    get_selection().setup_cache();

    std::sort(objects.begin(), objects.end(),
        [&get_coord](const ObjectInfo& a, const ObjectInfo& b) {
            return get_coord(a) < get_coord(b);
        });

    double min_coord = get_coord(objects.front());
    double max_coord = get_coord(objects.back());
    double total_distance = max_coord - min_coord;
    double interval = (objects.size() > 1) ? total_distance / (objects.size() - 1) : 0;
    for (size_t i = 1; i < objects.size() - 1; ++i) {
        double target_coord = min_coord + i * interval;
        double current_coord = get_coord(objects[i]);
        double offset = target_coord - current_coord;

        if (std::abs(offset) > 1e-6) {
            Vec3d displacement = Vec3d::Zero();
            displacement[axis] = offset;
            apply_transformation(objects[i].object_idx, objects[i].instance_idx, displacement);
        }
    }

    finish_operation(operation_name);
    return true;
}

bool GLGizmoAlignment::can_align(AlignType type) const
{
    return validate_selection_for_align();
}

bool GLGizmoAlignment::can_distribute(AlignType type) const
{
    const Selection& selection = get_selection();
    if (selection.is_single_full_object()) {
        size_t count = selection.get_volume_idxs().size();
        if (count >= 3)
            return true;
    }
    if (selection.is_single_volume() ||
        selection.is_single_modifier()) {
        return false;
    }

    if (selection.is_multiple_volume() || selection.is_multiple_modifier()) {
        size_t count = selection.get_volume_idxs().size();
        if (count < 3) return false;
        return (type == AlignType::DISTRIBUTE_X ||
                type == AlignType::DISTRIBUTE_Y ||
                type == AlignType::DISTRIBUTE_Z);
    }

    if (selection.is_multiple_full_object()) {
        BoundingBoxf3 big_bb;
        auto          objects = get_selected_objects_info(big_bb);
        if (objects.size() < 3) return false;
        return (type == AlignType::DISTRIBUTE_X || type == AlignType::DISTRIBUTE_Y || type == AlignType::DISTRIBUTE_Z);
    }

    return validate_selection_for_distribute();
}

bool GLGizmoAlignment::is_part_align_parent() const
{
    const Selection &selection = get_selection();
    if (selection.is_multiple_volume() || selection.is_multiple_modifier() || selection.is_single_volume() || selection.is_single_modifier()) {
        return true;
    }
    return false;
}

std::vector<GLGizmoAlignment::ObjectInfo> GLGizmoAlignment::get_selected_objects_info(BoundingBoxf3 &big_bb) const
{
    std::vector<ObjectInfo> objects;
    Selection& selection = get_selection();

    if (selection.is_empty()) return objects;

    const auto& content = selection.get_content();
    for (const auto& obj_it : content) {
        for (const auto& inst_idx : obj_it.second) {
            int obj_idx = obj_it.first;

            if (obj_idx >= 0 && obj_idx < selection.get_model()->objects.size()) {
                ModelObject* object = selection.get_model()->objects[obj_idx];
                if (inst_idx >= 0 && inst_idx < object->instances.size()) {
                    BoundingBoxf3 bbox = object->instance_bounding_box(inst_idx);
                    big_bb.merge(bbox);
                    objects.emplace_back(obj_idx, inst_idx, bbox);
                }
            }
        }
    }

    return objects;
}

void GLGizmoAlignment::set_parent_box(const BoundingBoxf3 &bb, const Vec3d &edge_inset) {
    m_parent_box   = bb;
    m_parent_inset = edge_inset;
}

std::vector<GLGizmoAlignment::AlignItem> GLGizmoAlignment::collect_items(bool parts) const
{
    const Selection &       selection = get_selection();
    std::vector<AlignItem> items;
    if (parts) {
        for (unsigned int idx : selection.get_volume_idxs()) {
            const GLVolume *volume = selection.get_volume(idx);
            if (volume != nullptr)
                items.push_back({volume->object_idx(), volume->instance_idx(), volume->volume_idx(), volume->transformed_convex_hull_bounding_box()});
        }
    } else {
        BoundingBoxf3 big_bb;
        for (const ObjectInfo &info : get_selected_objects_info(big_bb))
            items.push_back({info.object_idx, info.instance_idx, -1, info.bbox});
    }
    return items;
}

AlignMath::AnchorPick GLGizmoAlignment::pick_anchor(const std::vector<AlignItem> &items, bool parts, const AlignOptions &options) const
{
    const Selection &selection = get_selection();
    // Index in `items` of the item that contains volume `volume_idx` (a GLVolume index), or NO_ITEM.
    const auto item_of_volume = [&](int volume_idx) -> size_t {
        const GLVolume *v = volume_idx >= 0 ? selection.get_volume((unsigned int) volume_idx) : nullptr;
        if (v == nullptr)
            return AlignMath::NO_ITEM;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].object_idx == v->object_idx() && items[i].instance_idx == v->instance_idx() && (!parts || items[i].volume_idx == v->volume_idx()))
                return i;
        return AlignMath::NO_ITEM;
    };

    size_t explicit_index = AlignMath::NO_ITEM;
    if (options.anchor_item_set) {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].object_idx == options.anchor_object_idx && items[i].instance_idx == options.anchor_instance_idx &&
                (!parts || items[i].volume_idx == options.anchor_volume_idx))
                explicit_index = i;
    }
    return AlignMath::resolve_anchor(options.anchor_mode, options.anchor_item_set, explicit_index, item_of_volume(selection.get_anchor_volume_idx()),
                                     item_of_volume(selection.get_first_selected_volume_idx()));
}

std::vector<GLGizmoAlignment::AnchorCandidate> GLGizmoAlignment::anchor_candidates() const
{
    const bool                     parts = items_are_parts(false);
    const std::vector<AlignItem>   items = collect_items(parts);
    const Model *                  model = get_selection().get_model();
    std::vector<std::string>       names;
    std::vector<int>               numbers;
    for (const AlignItem &item : items) {
        std::string name = _u8L("Object");
        int         number = item.instance_idx + 1;
        if (model != nullptr && item.object_idx >= 0 && item.object_idx < (int) model->objects.size()) {
            const ModelObject *object = model->objects[(size_t) item.object_idx];
            name = object->name;
            if (parts && item.volume_idx >= 0 && item.volume_idx < (int) object->volumes.size()) {
                const std::string &part_name = object->volumes[(size_t) item.volume_idx]->name;
                if (!part_name.empty())
                    name += " / " + part_name;
                number = item.volume_idx + 1;
            }
        }
        names.push_back(name);
        numbers.push_back(number);
    }
    const std::vector<std::string> labels = AlignMath::disambiguate_names(names, numbers);
    std::vector<AnchorCandidate>   out;
    for (size_t i = 0; i < items.size(); ++i)
        out.push_back({items[i].object_idx, items[i].instance_idx, items[i].volume_idx, labels[i]});
    return out;
}

int GLGizmoAlignment::resolve_anchor_candidate(const AlignOptions &options, bool *explicit_missing) const
{
    const bool                   parts = items_are_parts(false);
    const std::vector<AlignItem> items = collect_items(parts);
    if (items.empty())
        return -1;
    const AlignMath::AnchorPick pick = pick_anchor(items, parts, options);
    if (explicit_missing != nullptr)
        *explicit_missing = pick.explicit_missing;
    return pick.use_union ? -1 : (int) pick.index;
}

Selection& GLGizmoAlignment::get_selection() const
{
    return m_canvas.get_selection();
}

void GLGizmoAlignment::apply_transformation(int obj_idx, int inst_idx, const Vec3d& displacement)
{
    get_selection().translate(obj_idx, inst_idx, displacement);
}

void GLGizmoAlignment::finish_operation(const std::string &operation_name, bool force_volume_move)
{
    get_selection().notify_instance_update(-1, -1);
    // do_move() takes the one and only undo snapshot (the model is still unchanged at that point).
    // Whole objects skip the "fix flying instances" pass, parts keep it.
    m_canvas.do_move(operation_name, force_volume_move, /*fix_flying_instances=*/force_volume_move);
}

bool GLGizmoAlignment::validate_selection_for_align() const
{
    const Selection& selection = get_selection();

    if (selection.is_single_full_object() ||
        selection.is_single_volume() ||
        selection.is_single_modifier()) {
        return true;
    }

    return selection.get_volume_idxs().size() >= 2;
}

bool GLGizmoAlignment::validate_selection_for_distribute() const
{
    const Selection& selection = get_selection();

    if (selection.is_single_full_object() || selection.is_multiple_full_object()) {
        std::set<int> object_indices;
        for (int volume_idx : selection.get_volume_idxs()) {
            const GLVolume* volume = selection.get_volume(volume_idx);
            if (volume) {
                object_indices.insert(volume->object_idx());
            }
        }
        return object_indices.size() >= 3;
    }

    return selection.get_volume_idxs().size() >= 3;
}

} // namespace GUI
} // namespace Slic3r
