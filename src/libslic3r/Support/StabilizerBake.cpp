#include "StabilizerBake.hpp"

#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>

namespace Slic3r {

double stabilizer_bake_gap(StabilizerBakePlacement placement, double sliced_gap)
{
    const double gap = std::max(0., sliced_gap);
    return placement == StabilizerBakePlacement::PartOfObject ? std::max(gap, STABILIZER_BAKE_PART_MIN_GAP) : gap;
}

StabilizerBakeResult bake_stabilizers(const PrintObject &object, const StabilizerBakeOptions &options)
{
    StabilizerBakeResult res;
    const PrintObjectConfig &cfg = object.config();
    if (object.layers().size() < 2) {
        res.error = "the object is not sliced";
        return res;
    }
    // The pillars stand on the bed, which a raft is not.
    if (cfg.raft_layers.value > 0) {
        res.error = "objects printed on a raft are not supported";
        return res;
    }

    stabilizers::StabilizerSettings st = stabilizers::settings_of(object);
    // Whatever the object's mode does live: Auto bakes the rings and the painted points, Manual the
    // painted points only.
    if (! st.ring_struts && ! st.painted_points) {
        res.error = "side stabilizers are off for this object";
        return res;
    }
    // Exactly the settings of the slice, so the bake is the stabilizers the preview showed. The one
    // exception is a part's tip gap, which never goes below STABILIZER_BAKE_PART_MIN_GAP: it only
    // moves the tip cut out along the strut. The struts are planned with the slice's own settings
    // (the planner moves pillars out for a large gap) and only the mesh takes the part's gap.
    stabilizers::StabilizerSettings mesh_st = st;
    mesh_st.tip_gap      = stabilizer_bake_gap(options.placement, st.tip_gap);
    res.sliced_tip_gap   = st.tip_gap;
    res.tip_diameter     = st.rings.tip_diameter;
    res.tip_gap          = mesh_st.tip_gap;
    res.layer_height     = cfg.layer_height.value;
    res.support_filament = cfg.support_filament.value;
    // Walls and infill are slicing settings: the baked body gets them as its own.
    res.wall_loops       = st.wall_loops;
    res.infill_density   = st.infill_density;
    res.infill_pattern   = st.infill_pattern;

    // The whole plan - pillars (tapered, columns) and braces too - exactly as the live generator prints it.
    res.plan   = stabilizers::plan_stabilizers(stabilizers::outlines_of(object), st, stabilizers::painted_spots(object), &res.plan_report);
    res.struts = res.plan.struts;
    if (res.struts.empty()) {
        res.error = "no stabilizer fits this object with its settings";
        return res;
    }

    stabilizers::MeshOptions mopts;
    mopts.segments = options.segments;
    res.mesh = stabilizers::stabilizer_mesh(res.plan, mesh_st, mopts, &res.mesh_report);
    if (res.mesh.indices.empty()) {
        res.error = "the stabilizer mesh came out empty";
        return res;
    }

    // The mesh is in the sliced frame: XY around the object's centre (print coordinates less the
    // instance shift), Z above the object's bottom. See SliceBakeFrame in SliceBake.hpp for the frames.
    if (options.placement == StabilizerBakePlacement::SeparateObject) {
        // World less the instance offset: each PrintInstance stands at shift = offset + center_offset,
        // so the mesh moves by center_offset and every instance gets offset = shift - center_offset.
        // (The full shift, not shift_without_plate_offset(): ModelInstance offsets include the plate's
        // own position.)
        const Vec2d c = unscaled(object.center_offset());
        for (Vec3f &v : res.mesh.vertices)
            v = Vec3f(v.x() + float(c.x()), v.y() + float(c.y()), v.z());
        for (const PrintInstance &pi : object.instances()) {
            const Point off(pi.shift.x() - object.center_offset().x(), pi.shift.y() - object.center_offset().y());
            const Vec2d o = unscaled(off);
            res.instance_offsets.emplace_back(o.x(), o.y(), 0.);
        }
    } else {
        // The source object's own coordinates: undo trafo_centered() (instance rotation, scale and Z).
        its_transform(res.mesh, object.trafo_centered().inverse(), true);
    }

    BOOST_LOG_TRIVIAL(info) << "Stabilizer bake: " << res.struts.size() << " struts, " << res.mesh_report.pillars << " pillars ("
                            << res.mesh_report.columns << " columns), " << res.mesh_report.braces << " braces, "
                            << res.mesh_report.triangles << " triangles, " << (res.mesh_report.unioned ? "unioned" : "overlapping shells")
                            << ", " << (res.mesh_report.closed ? "closed" : "NOT closed") << ", " << res.plan_report.unreachable.size()
                            << " painted point(s) unreachable";
    return res;
}

// Walls and infill of a baked body: the stabilizer walls with their sparse infill when set, else solid
// (three walls around 100% infill), as v1 baked.
static void set_body_infill(ModelConfigObject &config, const StabilizerBakeResult &result)
{
    const bool sparse = result.wall_loops > 0 && result.infill_density < 0.999;
    config.set_key_value("wall_loops", new ConfigOptionInt(sparse ? result.wall_loops : 3));
    config.set_key_value("sparse_infill_density", new ConfigOptionPercent(sparse ? 100. * result.infill_density : 100.));
    if (sparse)
        config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(result.infill_pattern));
}

std::string stabilizer_bake_object_name(const ModelObject &source)
{
    return (source.name.empty() ? std::string("object") : source.name) + " stabilizers";
}

ModelObject *apply_stabilizer_bake(Model &model, ModelObject &source, const StabilizerBakeResult &result,
                                   const StabilizerBakeOptions &options)
{
    if (result.mesh.indices.empty())
        return nullptr;

    // The filament: the source's support filament when it has one, else whatever the source prints with.
    const int source_extruder = source.config.has("extruder") ? source.config.opt_int("extruder") : 0;
    const int extruder        = result.support_filament > 0 ? result.support_filament : source_extruder;

    ModelObject *target = nullptr;
    if (options.placement == StabilizerBakePlacement::SeparateObject) {
        ModelObject *obj = model.add_object();
        obj->name        = stabilizer_bake_object_name(source);
        ModelVolume *vol = obj->add_volume(TriangleMesh(result.mesh));
        vol->name        = obj->name;
        // Settings that make it print like support - and that Orca and a Bambu-compatible export keep.
        // A plain project opened in Bambu Studio drops them all but the filament; the geometry still
        // prints there on default settings (45 degree struts, pillars four lines wide or more).
        obj->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        obj->config.set_key_value("enable_support", new ConfigOptionBool(false));
        obj->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
        set_body_infill(obj->config, result);
        obj->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));
        // The tips sit on the source's layers, so slice the stabilizers at the same height.
        if (result.layer_height > 0.)
            obj->config.set_key_value("layer_height", new ConfigOptionFloat(result.layer_height));
        if (result.instance_offsets.empty())
            obj->add_instance();
        for (const Vec3d &offset : result.instance_offsets) {
            ModelInstance *inst = obj->add_instance();
            // Rotation and scale are in the vertices already; only the position is the instance's.
            inst->set_offset(offset);
            inst->set_rotation(Vec3d::Zero());
            inst->set_scaling_factor(Vec3d::Ones());
            inst->set_mirror(Vec3d::Ones());
        }
        obj->invalidate_bounding_box();
        target = obj;
    } else {
        ModelVolume *vol = source.add_volume(TriangleMesh(result.mesh), ModelVolumeType::MODEL_PART);
        vol->name        = "Stabilizers";
        // Region settings are all a part can have of its own (supports and brim are object-wide).
        vol->config.set_key_value("extruder", new ConfigOptionInt(result.support_filament > 0 ? result.support_filament : 0));
        set_body_infill(vol->config, result);
        source.invalidate_bounding_box();
        target = &source;
    }

    // Not twice: the source would print its live stabilizers on top of the baked ones. Only the switch
    // goes; the other stabilizer settings stay for a later bake.
    source.config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));
    return target;
}

std::vector<std::string> objects_with_live_stabilizers(const Model &model, const DynamicPrintConfig &print_config)
{
    // Both are a plain on/off test: enable_support a bool, stabilizer_supports an enum whose 0 is Off.
    auto on = [](const ConfigOption *opt) { return opt != nullptr && (opt->type() == coBool ? opt->getBool() : opt->getInt() != 0); };
    auto resolved = [&print_config, &on](const ModelObject &obj, const char *key) {
        if (const ConfigOption *opt = obj.config.option(key); opt != nullptr)
            return on(opt);
        return on(print_config.option(key));
    };
    std::vector<std::string> out;
    for (const ModelObject *obj : model.objects)
        if (obj != nullptr && resolved(*obj, "stabilizer_supports") && resolved(*obj, "enable_support"))
            out.push_back(obj->name);
    return out;
}

} // namespace Slic3r
