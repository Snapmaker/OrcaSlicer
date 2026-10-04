#include "ModelArrange.hpp"

#include <libslic3r/Model.hpp>
#include <libslic3r/Geometry/ConvexHull.hpp>
#include <libslic3r/Print.hpp>
#include "MTUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

// Ultra: would support material be generated for this instance at all? The automatic kinds attach to
// facets that face down more steeply than the threshold (measured from the horizontal, as the setting
// is) and are not the object's own base on the bed; the manual kinds attach only where the user painted
// an enforcer. A flat print says no on both counts, and arrange then owes it no support clearance.
// Facet-level and in world space, so a rotated instance is judged as it will print.
bool instance_may_get_support(const ModelInstance &instance, SupportType support_type, int threshold_deg)
{
    const ModelObject *object = instance.get_object();
    if (object == nullptr)
        return false;
    const bool automatic = support_type == stNormalAuto || support_type == stTreeAuto;
    if (! automatic) {
        for (const ModelVolume *mv : object->volumes)
            if (mv->is_model_part() && ! mv->supported_facets.empty())
                return true;
        return false;
    }
    // A downward-facing facet whose slope from the horizontal is below the threshold is an overhang:
    // slope = acos(-nz), so overhang <=> -nz > cos(threshold). Threshold 0 means "auto" in the UI;
    // the slicer's own default of 30 deg stands in for it here.
    const double thr     = threshold_deg > 0 ? double(threshold_deg) : 30.0;
    const double cos_thr = std::cos(thr * M_PI / 180.0);
    const Transform3d inst_tr = instance.get_matrix();
    // The object's lowest point in world space: a facet lying on the bed is the base, not an overhang.
    double z_min = std::numeric_limits<double>::max();
    std::vector<std::pair<const ModelVolume *, Transform3d>> parts;
    for (const ModelVolume *mv : object->volumes) {
        if (! mv->is_model_part())
            continue;
        const Transform3d tr = inst_tr * mv->get_matrix();
        for (const Vec3f &v : mv->mesh().its.vertices)
            z_min = std::min(z_min, (tr * v.cast<double>()).z());
        parts.emplace_back(mv, tr);
    }
    const double bed_eps = 0.05;
    for (const auto &[mv, tr] : parts) {
        const indexed_triangle_set &its = mv->mesh().its;
        for (const Vec3i32 &f : its.indices) {
            const Vec3d a = tr * its.vertices[f(0)].cast<double>();
            const Vec3d b = tr * its.vertices[f(1)].cast<double>();
            const Vec3d c = tr * its.vertices[f(2)].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            const double l = n.norm();
            if (l <= 1e-12)
                continue;
            if (-n.z() / l <= cos_thr)
                continue; // not facing down steeply enough to need support
            if (std::max({a.z(), b.z(), c.z()}) <= z_min + bed_eps)
                continue; // the base on the bed
            return true;
        }
    }
    return false;
}

} // namespace Slic3r

namespace Slic3r {

arrangement::ArrangePolygons get_arrange_polys(const Model &model, ModelInstancePtrs &instances)
{
    size_t count = 0;
    for (auto obj : model.objects) count += obj->instances.size();

    ArrangePolygons input;
    input.reserve(count);
    instances.clear(); instances.reserve(count);
    ArrangePolygon ap;
    for (ModelObject *mo : model.objects)
        for (ModelInstance *minst : mo->instances) {
            minst->get_arrange_polygon(&ap);
            input.emplace_back(ap);
            instances.emplace_back(minst);
        }

    return input;
}

bool apply_arrange_polys(ArrangePolygons &input, ModelInstancePtrs &instances, VirtualBedFn vfn)
{
    bool ret = true;

    for(size_t i = 0; i < input.size(); ++i) {
        if (input[i].bed_idx != 0) { ret = false; if (vfn) vfn(input[i]); }
        if (input[i].bed_idx >= 0)
            instances[i]->apply_arrange_result(input[i].translation.cast<double>(),
                                               input[i].rotation);
    }

    return ret;
}

Slic3r::arrangement::ArrangePolygon get_arrange_poly(const Model &model)
{
    ArrangePolygon ap;
    Points &apts = ap.poly.contour.points;
    for (const ModelObject *mo : model.objects)
        for (const ModelInstance *minst : mo->instances) {
            ArrangePolygon obj_ap;
            minst->get_arrange_polygon(&obj_ap);
            ap.poly.contour.rotate(obj_ap.rotation);
            ap.poly.contour.translate(obj_ap.translation.x(), obj_ap.translation.y());
            const Points &pts = obj_ap.poly.contour.points;
            std::copy(pts.begin(), pts.end(), std::back_inserter(apts));
        }

    apts = std::move(Geometry::convex_hull(apts).points);
    return ap;
}

void duplicate(Model &model, Slic3r::arrangement::ArrangePolygons &copies, VirtualBedFn vfn)
{
    for (ModelObject *o : model.objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        o->instances.clear();
        for (const ModelInstance *i : instances) {
            for (arrangement::ArrangePolygon &ap : copies) {
                if (ap.bed_idx != 0) vfn(ap);
                ModelInstance *instance = o->add_instance(*i);
                Vec2d pos = unscale(ap.translation);
                instance->set_offset(instance->get_offset() + to_3d(pos, 0.));
            }
        }
        o->invalidate_bounding_box();
    }
}

void duplicate_objects(Model &model, size_t copies_num)
{
    for (ModelObject *o : model.objects) {
        // make a copy of the pointers in order to avoid recursion when appending their copies
        ModelInstancePtrs instances = o->instances;
        for (const ModelInstance *i : instances)
            for (size_t k = 2; k <= copies_num; ++ k)
                o->add_instance(*i);
    }
}

// Set up arrange polygon for a ModelInstance and Wipe tower
template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = obj.get_arrange_polygon(config);
    //BBS: always set bed_idx to 0 to use original transforms with no bed_idx
    //if this object is not arranged, it can keep the original transforms
    //ap.bed_idx        = ap.translation.x() / bed_stride_x(plater);
    ap.bed_idx = 0;
    ap.setter = [obj](const ArrangePolygon& p) {
        if (p.is_arranged()) {
            Vec2d t = p.translation.cast<double>();
            //BBS: change to sudoku-style computation, do it in partplate list
            //t.x() += p.bed_idx * bed_stride(plater);
            //t.x() += col * bed_stride_x(plater);
            //t.y() -= row * bed_stride_y(plater);
            T{ obj }.apply_arrange_result(t, p.rotation, p.itemid);
        }
    };

    return ap;
}

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance* inst, const Slic3r::DynamicPrintConfig& config)
{
    return get_arrange_poly(PtrWrapper{ inst },config);
}

ArrangePolygon get_instance_arrange_poly(ModelInstance* instance, const Slic3r::DynamicPrintConfig& config)
{
    ArrangePolygon ap = get_arrange_poly(PtrWrapper{ instance }, config);

    const unsigned int filament_id = ap.extrude_ids.empty() ? 0u : unsigned(std::max(0, ap.extrude_ids.front() - 1));

    //BBS: add temperature information
    if (config.has("curr_bed_type")) {
        ap.bed_temp = 0;
        ap.first_bed_temp = 0;
        BedType curr_bed_type = config.opt_enum<BedType>("curr_bed_type");

        const ConfigOptionInts* bed_opt = config.option<ConfigOptionInts>(get_bed_temp_key(curr_bed_type));
        if (bed_opt != nullptr)
            ap.bed_temp = bed_opt->get_at(filament_id);

        const ConfigOptionInts* bed_opt_1st_layer = config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(curr_bed_type));
        if (bed_opt_1st_layer != nullptr)
            ap.first_bed_temp = bed_opt_1st_layer->get_at(filament_id);
    }

    if (const auto *nozzle_temp = config.option<ConfigOptionInts>("nozzle_temperature"))
        ap.print_temp = get_value_at(config, *nozzle_temp, ConfigFlowDomain::Filament, filament_id);
    if (const auto *initial_temp = config.option<ConfigOptionInts>("nozzle_temperature_initial_layer"))
        ap.first_print_temp = get_value_at(config, *initial_temp, ConfigFlowDomain::Filament, filament_id);

    if (config.has("temperature_vitrification")) {
        ap.vitrify_temp = config.opt_int("temperature_vitrification", filament_id);
    }

    // get filament temp types
    auto* filament_types_opt = dynamic_cast<const ConfigOptionStrings*>(config.option("filament_type"));
    if (filament_types_opt) {
        std::set<int> filament_temp_types;
        for (auto i : ap.extrude_ids) {
            std::string type_str = filament_types_opt->get_at(i-1);
            int temp_type = Print::get_filament_temp_type(type_str);
            filament_temp_types.insert(temp_type);
        }
        ap.filament_temp_type = Print::get_compatible_filament_type(filament_temp_types);
    }

    // get brim width
    auto obj = instance->get_object();

    ap.brim_width = 1.0;
    // For by-layer printing, need to shrink bed a little, so the support won't go outside bed.
    // We set it to 5mm because that's how much a normal support will grow by default.
    // normal support 5mm, other support 22mm, no support 0mm
    auto supp_type_ptr = obj->get_config_value<ConfigOptionBool>(config, "enable_support");
    auto support_type_ptr = obj->get_config_value<ConfigOptionEnum<SupportType>>(config, "support_type");
    auto support_type = support_type_ptr->value;
    auto enable_support = supp_type_ptr->getBool();
    int support_int = support_type_ptr->getInt();

    // Ultra: the clearance is for support material that will actually be printed. "Enable support"
    // is on in most people's global presets, and a flat print (nothing under the threshold angle,
    // nothing painted) generates none - yet every such object was still spaced 6 mm (24 mm with tree
    // support) apart, and Fill bed's gap floor blamed a brim for it (owner, 2026-09-16). So the
    // instance is scanned for what support would attach to first; with nothing to attach to it keeps
    // the flat 1 mm.
    const int threshold_deg = obj->get_config_value<ConfigOptionInt>(config, "support_threshold_angle")->getInt();
    if (enable_support && !instance_may_get_support(*instance, support_type, threshold_deg))
        enable_support = false;

    if (enable_support && (support_type == stNormalAuto || support_type == stNormal))
        ap.brim_width = 6.0;
    else if (enable_support) {
        ap.brim_width = 24.0; // 2*MAX_BRANCH_RADIUS_FIRST_LAYER
        ap.has_tree_support = true;
    }

    auto size = obj->instance_convex_hull_bounding_box(instance).size();
    ap.height = size.z();
    ap.name = obj->name;
    return ap;
}

} // namespace Slic3r
