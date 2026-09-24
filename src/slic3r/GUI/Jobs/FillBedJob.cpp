#include "FillBedJob.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libnest2d/common.hpp"

#include <algorithm>
#include <numeric>

namespace Slic3r {
namespace GUI {

//BBS: add partplate related logic
void FillBedJob::prepare()
{
    PartPlateList& plate_list = m_plater->get_partplate_list();

    m_locked.clear();
    m_selected.clear();
    m_unselected.clear();
    m_bedpts.clear();

    params = init_arrange_params(m_plater);

    m_object_idx = m_plater->get_selected_object_idx();
    if (m_object_idx == -1)
        return;

    //select current plate at first
    int sel_id = m_plater->get_selection().get_instance_idx();
    sel_id = std::max(sel_id, 0);

    int sel_ret = plate_list.select_plate_by_obj(m_object_idx, sel_id);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":select plate obj_id %1%, ins_id %2%, ret %3%}") % m_object_idx % sel_id % sel_ret;

    PartPlate* plate = plate_list.get_curr_plate();
    Model& model = m_plater->model();
    BoundingBox plate_bb = plate->get_bounding_box_crd();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate_index = plate->get_index();

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    m_selected.reserve(model_object->instances.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
        {
            bool selected = (oidx == m_object_idx);

            ArrangePolygon ap = get_instance_arrange_poly(mo->instances[inst_idx], global_config);
            BoundingBox ap_bb = ap.transformed_poly().contour.bounding_box();
            ap.name = mo->name;

            if (selected)
            {
                if (mo->instances[inst_idx]->printable)
                {
                    ++ap.priority;
                    ap.itemid = m_selected.size();
                    m_selected.emplace_back(ap);
                }
                else
                {
                    if (plate_bb.contains(ap_bb))
                    {
                        ap.bed_idx = 0;
                        ap.itemid = m_unselected.size();
                        ap.row = cur_plate_index / plate_cols;
                        ap.col = cur_plate_index % plate_cols;
                        ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                        ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                        m_unselected.emplace_back(ap);
                    }
                    else
                    {
                        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                        ap.itemid = m_locked.size();
                        m_locked.emplace_back(ap);
                    }
                }
            }
            else
            {
                if (plate_bb.contains(ap_bb))
                {
                    ap.bed_idx = 0;
                    ap.itemid = m_unselected.size();
                    ap.row = cur_plate_index / plate_cols;
                    ap.col = cur_plate_index % plate_cols;
                    ap.translation(X) -= bed_stride_x(m_plater) * ap.col;
                    ap.translation(Y) += bed_stride_y(m_plater) * ap.row;
                    m_unselected.emplace_back(ap);
                }
                else
                {
                    ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
                    ap.itemid = m_locked.size();
                    m_locked.emplace_back(ap);
                }
            }
        }
    }
    /*
    for (ModelInstance *inst : model_object->instances)
        if (inst->printable) {
            ArrangePolygon ap = get_arrange_poly(inst);
            // Existing objects need to be included in the result. Only
            // the needed amount of object will be added, no more.
            ++ap.priority;
            m_selected.emplace_back(ap);
        }*/

    if (m_selected.empty()) return;

    //add the virtual object into unselect list if has
    double scaled_exclusion_gap = scale_(1);
    plate_list.preprocess_exclude_areas(params.excluded_regions, 1, scaled_exclusion_gap);
    plate_list.preprocess_exclude_areas(m_unselected);

    m_bedpts = get_bed_shape(*m_plater->config());

    auto &objects = m_plater->model().objects;
    /*BoundingBox bedbb = get_extents(m_bedpts);

    for (size_t idx = 0; idx < objects.size(); ++idx)
        if (int(idx) != m_object_idx)
            for (ModelInstance *mi : objects[idx]->instances) {
                ArrangePolygon ap = get_arrange_poly(mi);
                auto ap_bb = ap.transformed_poly().contour.bounding_box();

                if (ap.bed_idx == 0 && !bedbb.contains(ap_bb))
                    ap.bed_idx = arrangement::UNARRANGED;

                m_unselected.emplace_back(ap);
            }*/
    // Filling is about fitting as many copies as possible, so the tower goes into a corner
    // first instead of staying wherever it was left, possibly mid-plate.
    if (auto wt = move_wipe_tower_to_nearest_corner(*m_plater))
        m_unselected.emplace_back(std::move(*wt));

    double sc = scaled<double>(1.) * scaled(1.);

    auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
    ExPolygon poly = polys.empty() ? m_selected.front().poly : polys.front();
    double poly_area = poly.area() / sc;
    double unsel_area = std::accumulate(m_unselected.begin(),
                                        m_unselected.end(), 0.,
                                        [cur_plate_index](double s, const auto &ap) {
                                            //BBS: m_unselected instance is in the same partplate
                                            return s + (ap.bed_idx == cur_plate_index) * ap.poly.area();
                                            //return s + (ap.bed_idx == 0) * ap.poly.area();
                                        }) / sc;

    double fixed_area = unsel_area + m_selected.size() * poly_area;
    double bed_area   = Polygon{m_bedpts}.area() / sc;

    // This is the maximum number of items, the real number will always be close but less.
    int needed_items = (bed_area - fixed_area) / poly_area;

    //int sel_id = m_plater->get_selection().get_instance_idx();
    // if the selection is not a single instance, choose the first as template
    //sel_id = std::max(sel_id, 0);
    ModelInstance *mi = model_object->instances[sel_id];
    ArrangePolygon template_ap = get_instance_arrange_poly(mi, global_config);

    for (int i = 0; i < needed_items; ++i) {
        ArrangePolygon ap = template_ap;
        ap.poly = m_selected.front().poly;
        ap.bed_idx = PartPlateList::MAX_PLATES_COUNT;
        ap.itemid = -1;
        ap.setter = [this](const ArrangePolygon &p) {
            ModelObject *mo = m_plater->model().objects[m_object_idx];
            ModelObject* newObj = m_plater->model().add_object(*mo);
            newObj->name = mo->name +" "+ std::to_string(p.itemid);
            for (ModelInstance *newInst : newObj->instances) { newInst->apply_arrange_result(p.translation.cast<double>(), p.rotation); }
            //m_plater->sidebar().obj_list()->paste_objects_into_list({m_plater->model().objects.size()-1});
        };
        m_selected.emplace_back(ap);
    }

    m_status_range = m_selected.size();

    // The strides have to be removed from the fixed items. For the
    // arrangeable (selected) items bed_idx is ignored and the
    // translation is irrelevant.
    //BBS: remove logic for unselected object
    /*double stride = bed_stride(m_plater);
    for (auto &p : m_unselected)
        if (p.bed_idx > 0)
            p.translation(X) -= p.bed_idx * stride;*/
}

void FillBedJob::process(Ctl &ctl)
{
    auto statustxt = _u8L("Filling");
    ctl.call_on_main_thread([this] { prepare(); }).wait();
    ctl.update_status(0, statustxt);

    if (m_object_idx == -1 || m_selected.empty()) return;

    update_arrange_params(params, m_plater->config(), m_selected);
    m_bedpts = get_shrink_bedpts(m_plater->config(), params);

    auto &partplate_list               = m_plater->get_partplate_list();
    auto &print                        = wxGetApp().plater()->get_partplate_list().get_current_fff_print();
    const Slic3r::DynamicPrintConfig& global_config = wxGetApp().preset_bundle->full_config();
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const bool is_bbl = wxGetApp().preset_bundle->is_bbl_vendor();
    if (is_bbl && params.avoid_extrusion_cali_region && global_config.opt_bool("scan_first_layer"))
        partplate_list.preprocess_nonprefered_areas(m_unselected, MAX_NUM_PLATES);
    
    update_selected_items_inflation(m_selected, m_plater->config(), params);
    update_unselected_items_inflation(m_unselected, m_plater->config(), params);

    bool do_stop = false;
    params.stopcondition = [&ctl, &do_stop]() {
        return ctl.was_canceled() || do_stop;
    };

    params.progressind = [this, &ctl, &statustxt](unsigned st,std::string str="") {
         if (st > 0)
             ctl.update_status(st * 100 / status_range(), statustxt + " " + str);
    };

    params.on_packed = [&do_stop] (const ArrangePolygon &ap) {
        do_stop = ap.bed_idx > 0 && ap.priority == 0;
    };
    // final align用的是凸包，在有fixed item的情况下可能找到的参考点位置是错的，这里就不做了。见STUDIO-3265
    // This holds for every vendor, not just BBL: filling the bed always leaves fixed
    // items around (other objects, wipe tower, excluded regions), so re-centering the
    // packed pile on its convex hull shifts copies over the plate boundary. Keeping
    // the alignment off also makes the placer check bin containment strictly.
    params.do_final_align = false;

    if (m_selected.size() > 100){
        // too many items, just find grid empty cells to put them.
        // The footprint has to be measured on the rotated outline, and every copy is
        // centered on its cell by that footprint: ArrangePolygon::translation is the
        // instance offset, and the instance origin is not necessarily the center of
        // the object's convex hull.
        std::vector<BoundingBox> footprints;
        footprints.reserve(m_selected.size());
        Point cell_size(0, 0);
        for (const ArrangePolygon &ap : m_selected) {
            ExPolygon rotated = ap.poly;
            rotated.rotate(ap.rotation);
            footprints.emplace_back(get_extents(rotated));
            const Point sz = footprints.back().size();
            cell_size = Point(std::max(cell_size.x(), sz.x()), std::max(cell_size.y(), sz.y()));
        }

        Vec2f step = unscaled<float>(cell_size) + Vec2f(m_selected.front().brim_width, m_selected.front().brim_width);
        std::vector<Vec2f> empty_cells = Plater::get_empty_cells(step);

        // get_empty_cells() grids the raw build volume, so it knows neither the
        // skirt/brim margin nor the objects already sitting on the plate. Drop every
        // cell a copy would not fully fit into instead of laying it over the plate
        // boundary or on top of an existing object.
        // It also works in the current plate's coordinates, while m_bedpts and
        // m_unselected are normalized to the first plate, so shift those over first.
        const int   plate_cols   = std::max(1, partplate_list.get_plate_cols());
        const int   plate_idx    = partplate_list.get_curr_plate_index();
        const Point plate_offset(coord_t( bed_stride_x(m_plater) * (plate_idx % plate_cols)),
                                 coord_t(-bed_stride_y(m_plater) * (plate_idx / plate_cols)));

        BoundingBox bed_bb(m_bedpts);
        bed_bb.translate(plate_offset);

        std::vector<BoundingBox> blockers;
        blockers.reserve(m_unselected.size());
        for (const ArrangePolygon &ap : m_unselected) {
            BoundingBox bb = get_extents(ap.transformed_poly());
            bb.offset(ap.inflation);
            bb.translate(plate_offset);
            blockers.emplace_back(bb);
        }

        size_t next_cell = 0;
        for (size_t i = 0; i < m_selected.size(); i++) {
            m_selected[i].bed_idx = -1;
            while (next_cell < empty_cells.size()) {
                const Point t = scaled<coord_t>(empty_cells[next_cell++]) - footprints[i].center();
                BoundingBox placed = footprints[i];
                placed.translate(t);
                placed.offset(m_selected[i].inflation);
                if (!bed_bb.contains(placed))
                    continue;
                if (std::any_of(blockers.begin(), blockers.end(),
                                [&placed](const BoundingBox &bb) { return bb.overlap(placed); }))
                    continue;
                m_selected[i].translation = t;
                m_selected[i].bed_idx     = 0;
                break;
            }
        }
    }
    else
        arrangement::arrange(m_selected, m_unselected, m_bedpts, params);

    // finalize just here.
    ctl.update_status(100, ctl.was_canceled() ?
                                       _u8L("Bed filling canceled.") :
                                       _u8L("Bed filling done."));
}

FillBedJob::FillBedJob() : m_plater{wxGetApp().plater()} {}

void FillBedJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    if (m_object_idx == -1) return;

    ModelObject *model_object = m_plater->model().objects[m_object_idx];
    if (model_object->instances.empty()) return;

    //BBS: partplate
    PartPlateList& plate_list = m_plater->get_partplate_list();
    int plate_cols = plate_list.get_plate_cols();
    int cur_plate = plate_list.get_curr_plate_index();

    size_t inst_cnt = model_object->instances.size();

    int added_cnt = std::accumulate(m_selected.begin(), m_selected.end(), 0, [](int s, auto &ap) {
        return s + int(ap.priority == 0 && ap.bed_idx == 0);
    });

    Model& model = m_plater->model();
    const size_t oldObjectCount = model.objects.size();

    if (added_cnt > 0) {
        //BBS: adjust the selected instances
        for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != 0) {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":skipped: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
                /*if (ap.itemid == -1)*/
                    continue;
                ap.bed_idx = plate_list.get_plate_count();
            }
            else
                ap.bed_idx = cur_plate;

            if (m_selected.size() <= 100) {
                ap.row = ap.bed_idx / plate_cols;
                ap.col = ap.bed_idx % plate_cols;
                ap.translation(X) += bed_stride_x(m_plater) * ap.col;
                ap.translation(Y) -= bed_stride_y(m_plater) * ap.row;
            }

            ap.apply();

            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":selected: bed_id %1%, trans {%2%,%3%}") % ap.bed_idx % unscale<double>(ap.translation(X)) % unscale<double>(ap.translation(Y));
        }

        const size_t newObjectCount = model.objects.size();
        if (newObjectCount > oldObjectCount)
        {
            ModelObjectPtrs newObjects(model.objects.begin() + oldObjectCount, model.objects.end());
            model.InitializeAssemblyPositions(newObjects);
        }

        auto obj_list = m_plater->sidebar().obj_list();
        for (size_t i = oldObjectCount; i < newObjectCount; i++) {
            obj_list->add_object_to_list(i, true, true, false);
            obj_list->update_printable_state(i, 0);
        }

        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": paste_objects_into_list";

        /*for (ArrangePolygon& ap : m_selected) {
            if (ap.bed_idx != arrangement::UNARRANGED && (ap.priority != 0 || ap.bed_idx == 0))
                ap.apply();
        }*/

        //model_object->ensure_on_bed();
        //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": model_object->ensure_on_bed()";

        m_plater->update();
    }
}

namespace {

// The object's outline on the plate, concavities and all. The arranger itself only ever sees a
// convex hull, which cannot tell that two hearts turned towards each other interlock. Taken from
// a handful of horizontal sections rather than by projecting every facet: a section is cheap, and
// their union is the silhouette to within the sampling.
ExPolygons object_outline(const ModelObject &mo, const Transform3d &trafo)
{
    static const int SECTIONS = 12;

    BoundingBoxf3 bb;
    for (const ModelVolume *v : mo.volumes)
        if (v->is_model_part())
            bb.merge(v->mesh().transformed_bounding_box(trafo * v->get_matrix()));
    if (!bb.defined || bb.max.z() - bb.min.z() <= 0.)
        return {};

    std::vector<float> zs;
    zs.reserve(SECTIONS);
    for (int i = 0; i < SECTIONS; ++i)
        zs.emplace_back(float(bb.min.z() + (bb.max.z() - bb.min.z()) * (double(i) + 0.5) / double(SECTIONS)));

    Polygons all;
    for (const ModelVolume *v : mo.volumes)
        if (v->is_model_part()) {
            MeshSlicingParams params;
            params.mode  = MeshSlicingParams::SlicingMode::Positive; // holes closed: nothing fits in them
            params.trafo = trafo * v->get_matrix();
            for (const Polygons &section : slice_mesh(v->mesh().its, zs, params))
                append(all, section);
        }

    // A section of a detailed mesh carries thousands of points, and every one of them would be
    // walked again for each overlap test the nesting below runs.
    return expolygons_simplify(union_ex(all), scaled<double>(0.2));
}

ExPolygons turned(const ExPolygons &outline, double angle, coord_t inflation)
{
    ExPolygons out = outline;
    for (ExPolygon &ex : out)
        ex.rotate(angle);

    return inflation > 0 ? offset_ex(out, float(inflation)) : out;
}

// Copy i of a rosette sits at angle 2*pi*i/n from the group's origin, turned by that same angle.
Vec2crd rosette_offset(double radius, size_t i, size_t n)
{
    const double a = 2. * PI * double(i) / double(n);
    return Vec2crd(coord_t(radius * std::cos(a)), coord_t(radius * std::sin(a)));
}

// Close the rosette up until its copies almost touch. Returns the radius they settle at, or
// reach when they never come apart.
double nest_radius(const std::vector<ExPolygons> &shapes, double reach)
{
    const size_t n        = shapes.size();
    auto         overlaps = [&shapes, n](double radius) {
        std::vector<ExPolygons> at(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec2crd off = rosette_offset(radius, i, n);
            at[i]             = shapes[i];
            for (ExPolygon &ex : at[i])
                ex.translate(off.x(), off.y());
        }
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (!intersection_ex(at[i], at[j]).empty())
                    return true;
        return false;
    };

    if (overlaps(reach))
        return reach;

    double lo = 0., hi = reach; // lo overlaps, hi does not
    for (int i = 0; i < 12; ++i) {
        const double mid = 0.5 * (lo + hi);
        (overlaps(mid) ? lo : hi) = mid;
    }
    return hi;
}

Polygon cluster_hull(const ExPolygons &outline, const std::vector<FillBedCluster> &members)
{
    Points pts;
    for (const FillBedCluster &member : members)
        for (const ExPolygon &ex : turned(outline, member.rotation, 0)) {
            Polygon contour = ex.contour;
            contour.translate(member.offset);
            append(pts, contour.points);
        }

    return Geometry::convex_hull(std::move(pts));
}

// Look for a group of copies that takes less room per copy than a copy on its own: two turned
// half around, or four turned quarter by quarter, pushed together until they nearly touch. This
// is what packs hearts or wedges into each other's hollows. Falls back to a single copy.
//
// Which way each copy faces is searched too. Turning a copy by its own place in the rosette
// leaves it side-on to the middle; the layout people lay out by hand - four hearts meeting at
// their points - needs a turn on top of that, and which one depends on how the shape sits in
// its own frame.
//
// The score is the area of the group's convex hull per copy, against that of one copy's hull:
// the hull is what the packer works with, so the hollows a neighbour fills are exactly the
// room that stops being wasted.
std::vector<FillBedCluster> build_cluster(const ExPolygons &outline, coord_t inflation)
{
    static const int PHASES = 8;

    const std::vector<FillBedCluster> single{{Vec2crd(0, 0), 0.}};

    const BoundingBox bb = get_extents(outline);
    if (bb.size().x() <= 0 || bb.size().y() <= 0)
        return single;

    const double                reach  = 2. * bb.size().cast<double>().norm();
    double                      best   = cluster_hull(outline, single).area();
    std::vector<FillBedCluster> winner = single;
    if (best <= 0.)
        return single;

    for (const size_t n : {size_t(2), size_t(4)})
        for (int p = 0; p < PHASES; ++p) {
            const double phase = 2. * PI * double(p) / double(PHASES);

            std::vector<ExPolygons> shapes;
            std::vector<double>     turns;
            shapes.reserve(n);
            turns.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                turns.emplace_back(2. * PI * double(i) / double(n) + phase);
                shapes.emplace_back(turned(outline, turns.back(), inflation));
            }

            const double radius = nest_radius(shapes, reach);
            if (radius >= reach)
                continue;

            std::vector<FillBedCluster> members;
            members.reserve(n);
            for (size_t i = 0; i < n; ++i)
                members.push_back({rosette_offset(radius, i, n), turns[i]});

            const double area = cluster_hull(outline, members).area() / double(n);
            if (area < 0.98 * best) { // has to be a real gain, not rounding
                best   = area;
                winner = members;
            }
        }

    BOOST_LOG_TRIVIAL(info) << "fill bed: nested " << winner.size() << " copies per group, "
                            << "hull area per copy " << best;
    return winner;
}

} // namespace

FillBedOptionsJob::FillBedOptionsJob() : m_plater{wxGetApp().plater()}
{
    // A packs for count: copies nested into each other, half turns allowed, and only
    // TIGHT_GAP_MM between them, so their brims may touch and a support that spreads wide can
    // reach a neighbour. B keeps the arranger's brim / support ring - a 12 mm radius on a tree
    // support - and still lets the packer turn the copies. C lays them out all facing the same
    // way, which is the plate to pick when the copies have to come off the bed alike.
    m_variants = {
        {_u8L("A Max"),      true,  false, true,  0., 0.},
        {_u8L("B Balanced"), true,  true,  false, 3., 2.},
        {_u8L("C Grid"),     false, true,  false, 8., 5.},
    };
}

void FillBedOptionsJob::prepare()
{
    m_fixed.clear();
    m_tower_pos.reset();

    m_object_idx = m_plater->get_selected_object_idx();
    if (m_object_idx == -1)
        return;

    ModelObject *mo = m_plater->model().objects[m_object_idx];
    m_instance_idx  = std::max(m_plater->get_selection().get_instance_idx(), 0);
    if (m_instance_idx >= int(mo->instances.size()))
        m_instance_idx = 0;

    m_params = init_arrange_params(m_plater);

    const Slic3r::DynamicPrintConfig &global_config = wxGetApp().preset_bundle->full_config();
    ModelInstance                    *mi            = mo->instances[m_instance_idx];
    m_template          = get_instance_arrange_poly(mi, global_config);
    m_template.setter   = nullptr; // copies are made in finalize(); the source stays put
    m_template.priority = 0;

    // In the same frame as m_template.poly: the Z turn and the position on the plate are the
    // arranger's to choose, everything else is baked in.
    Vec3d rotation = mi->get_rotation();
    rotation.z()   = 0.;
    Geometry::Transformation trafo(mi->get_transformation());
    trafo.set_offset(mi->get_offset().z() * Vec3d::UnitZ());
    trafo.set_rotation(rotation);
    m_outline = object_outline(*mo, trafo.get_matrix());

    // The new plates start out empty but for their excluded regions and, on a multi-material
    // print, the prime tower - pushed into a corner, as fill bed does.
    PartPlateList &plate_list = m_plater->get_partplate_list();
    plate_list.preprocess_exclude_areas(m_params.excluded_regions, 1, scale_(1));
    plate_list.preprocess_exclude_areas(m_fixed, 1);
    if (auto wt = get_wipe_tower_corner_arrangepoly(*m_plater)) {
        m_tower_pos = unscaled(wt->translation);
        m_fixed.emplace_back(std::move(*wt));
    }
}

void FillBedOptionsJob::process(Ctl &ctl)
{
    ctl.call_on_main_thread([this] { prepare(); }).wait();
    if (m_object_idx == -1)
        return;

    const DynamicPrintConfig *print_cfg = m_plater->config();
    const std::string         status    = _u8L("Preparing plate options");

    for (size_t v = 0; v < m_variants.size(); ++v) {
        if (ctl.was_canceled())
            return;

        Variant &variant = m_variants[v];
        variant.placed.clear();
        ctl.update_status(int(v * 100 / m_variants.size()), status + " " + variant.label);

        arrangement::ArrangeParams params = m_params;
        params.allow_rotations  = variant.rotations;
        params.allow_half_turns = variant.rotations;
        params.min_obj_distance = 0; // brim / support aware spacing; the variant's gap goes on top
        params.do_final_align   = false;

        ArrangePolygons probe{m_template};
        update_arrange_params(params, print_cfg, probe);
        params.bed_shrink_x += float(variant.edge_mm);
        params.bed_shrink_y += float(variant.edge_mm);
        const Points bedpts = get_shrink_bedpts(print_cfg, params);

        update_selected_items_inflation(probe, print_cfg, params);
        const coord_t inflation = (variant.support_room ? probe.front().inflation : scaled(TIGHT_GAP_MM / 2.))
                                  + scaled(variant.gap_mm / 2.);

        // Copies turned into one another can take less room per copy than a copy on its own, and
        // the packer only ever sees convex hulls, so the interlocking is worked out here and
        // handed to it as a single item.
        ArrangePolygon templ = m_template;
        variant.cluster.assign(1, FillBedCluster{Vec2crd(0, 0), 0.});
        if (variant.nest && !m_outline.empty()) {
            variant.cluster = build_cluster(m_outline, inflation);
            if (variant.cluster.size() > 1)
                templ.poly = ExPolygon(cluster_hull(m_outline, variant.cluster));
        }
        const int per_group = int(variant.cluster.size());

        // An upper bound on the groups that fit; the packer stops as soon as the plate is full.
        const ExPolygons grown     = offset_ex(templ.poly, float(inflation));
        const double     item_area = grown.empty() ? templ.poly.area() : grown.front().area();
        const double     bed_area  = std::abs(Polygon{bedpts}.area());
        const int        count     = std::clamp(int(bed_area / std::max(item_area, 1.)) + 1, 1,
                                                std::max(1, MAX_COPIES_PER_OPTION / per_group));

        ArrangePolygons items(count, templ);
        for (int i = 0; i < count; ++i) {
            items[i].itemid    = i;
            items[i].bed_idx   = PartPlateList::MAX_PLATES_COUNT;
            items[i].inflation = inflation;
        }
        ArrangePolygons fixed = m_fixed;
        update_unselected_items_inflation(fixed, print_cfg, params);

        bool plate_full = false;
        params.stopcondition = [&ctl, &plate_full]() { return ctl.was_canceled() || plate_full; };
        params.on_packed     = [&plate_full](const ArrangePolygon &ap) { plate_full = ap.bed_idx > 0; };
        params.progressind   = [](unsigned, std::string) {};

        arrangement::arrange(items, fixed, bedpts, params);

        for (const ArrangePolygon &ap : items)
            if (ap.bed_idx == 0)
                variant.placed.emplace_back(ap);
        variant.capped = int(variant.placed.size()) * per_group >= MAX_COPIES_PER_OPTION;
    }

    ctl.update_status(100, ctl.was_canceled() ? _u8L("Bed filling canceled.") : _u8L("Bed filling done."));
}

void FillBedOptionsJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr || m_object_idx == -1)
        return;

    NotificationManager *notifications = m_plater->get_notification_manager();
    if (std::all_of(m_variants.begin(), m_variants.end(), [](const Variant &v) { return v.placed.empty(); })) {
        notifications->push_notification(NotificationType::BBLPlateInfo, NotificationManager::NotificationLevel::WarningNotificationLevel,
                                         _u8L("Not a single copy of the selected object fits on an empty plate."));
        return;
    }

    // Create every plate before placing anything: a new plate can re-flow the plate grid,
    // which moves the origins of the plates already there.
    PartPlateList   &plate_list = m_plater->get_partplate_list();
    std::vector<int> plate_idxs(m_variants.size(), -1);
    bool             out_of_plates = false;
    for (size_t v = 0; v < m_variants.size(); ++v) {
        if (m_variants[v].placed.empty())
            continue;
        plate_idxs[v] = plate_list.create_plate();
        if (plate_idxs[v] < 0) {
            out_of_plates = true;
            break;
        }
    }

    Model       &model       = m_plater->model();
    const size_t old_count   = model.objects.size();
    int          first_plate = -1;
    for (size_t v = 0; v < m_variants.size(); ++v) {
        const int plate_idx = plate_idxs[v];
        if (plate_idx < 0)
            continue;
        if (first_plate < 0)
            first_plate = plate_idx;

        const Variant &variant = m_variants[v];
        PartPlate     *plate   = plate_list.get_plate(plate_idx);
        const Vec3d    origin  = plate->get_origin();
        const Vec2d    shift(scale_(origin.x()), scale_(origin.y()));
        for (const ArrangePolygon &ap : variant.placed) {
            // The group was packed as one item, so its copies turn and travel with it.
            const double c = std::cos(ap.rotation), s = std::sin(ap.rotation);
            for (const FillBedCluster &member : variant.cluster) {
                // add_object() can reallocate model.objects, so the source is looked up each time.
                ModelObject *src = model.objects[m_object_idx];
                ModelObject *obj = model.add_object(*src);
                obj->clear_instances();
                ModelInstance *inst = obj->add_instance(*src->instances[m_instance_idx]);
                const Vec2d in_group(c * member.offset.x() - s * member.offset.y(),
                                     s * member.offset.x() + c * member.offset.y());
                inst->apply_arrange_result(ap.translation.cast<double>() + in_group + shift,
                                           ap.rotation + member.rotation);
                // Register the copy with the plate it now stands on. add_to_plate() would move it
                // to the plate centre first, stacking every copy on one spot.
                plate_list.notify_instance_update(int(model.objects.size()) - 1, 0, true);
            }
        }

        const std::string count = std::to_string(variant.placed.size() * variant.cluster.size())
                                  + (variant.capped ? "+" : "");
        plate->set_plate_name((boost::format(_u8L("%1% - %2% copies")) % variant.label % count).str());

        if (m_tower_pos) {
            DynamicConfig    &proj_cfg = wxGetApp().preset_bundle->project_config;
            ConfigOptionFloat wipe_tower_x(m_tower_pos->x());
            ConfigOptionFloat wipe_tower_y(m_tower_pos->y());
            proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true)->set_at(&wipe_tower_x, plate_idx, 0);
            proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true)->set_at(&wipe_tower_y, plate_idx, 0);
        }
    }

    const size_t new_count = model.objects.size();
    if (new_count > old_count) {
        ModelObjectPtrs new_objects(model.objects.begin() + old_count, model.objects.end());
        model.InitializeAssemblyPositions(new_objects);

        auto obj_list = m_plater->sidebar().obj_list();
        for (size_t i = old_count; i < new_count; ++i) {
            obj_list->add_object_to_list(i, true, true, false);
            obj_list->update_printable_state(i, 0);
        }
    }

    if (out_of_plates)
        notifications->push_notification(NotificationType::BBLPlateInfo, NotificationManager::NotificationLevel::WarningNotificationLevel,
                                         _u8L("The maximum number of plates was reached, so not every option could be added."));

    m_plater->update();
    if (first_plate >= 0)
        m_plater->select_plate(first_plate);
}

}} // namespace Slic3r::GUI
