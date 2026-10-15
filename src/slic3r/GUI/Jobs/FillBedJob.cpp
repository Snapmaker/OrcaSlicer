#include "FillBedJob.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libnest2d/common.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {
namespace GUI {

// Minimum grid capacity (rough cell count = floor(W/step_x) * floor(H/step_y))
// for FillBed to take the fast grid path; otherwise fall back to the NFP arrange.
// Empirical threshold : NFP path is noticeably slow even with
// only a few dozen candidate cells, so we prefer the grid path whenever the
// bed/step ratio leaves room for a reasonable number of copies.
static constexpr long long FILL_BED_GRID_PATH_MIN_CAPACITY = 50;

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
    if (auto wt = get_wipe_tower_arrangepoly(*m_plater))
        m_unselected.emplace_back(std::move(*wt));

    double sc = scaled<double>(1.) * scaled(1.);

    auto polys = offset_ex(m_selected.front().poly, params.min_obj_distance / 2);
    ExPolygon poly = polys.empty() ? m_selected.front().poly : polys.front();
    double poly_area = poly.area() / sc;
    double unsel_area = std::accumulate(m_unselected.begin(),
                                        m_unselected.end(), 0.,
                                        [](double s, const auto &ap) {
                                            // bed_idx of m_unselected items is normalized to 0 in prepare(), test against 0 instead of cur_plate_index.
                                            return s + (ap.bed_idx == 0) * ap.poly.area();
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
    params.do_final_align = !is_bbl;

    // Reuse ap.inflation computed by update_selected_items_inflation for the step, so the spacing matches the NFP path.
    const ArrangePolygon &tmpl = m_selected.front();
    const float inflation_mm = unscaled<float>(tmpl.inflation);
    const Vec2f step = unscaled<float>(get_extents(tmpl.poly).size()) + 2.f * Vec2f(inflation_mm, inflation_mm);

    // re-click consistency: pick path by grid capacity (stable across clicks).
    // Use double here so the floor() near-edge result is not perturbed by float rounding.
    // Use long long for the product so a (pathological) tiny step on a huge bed
    // cannot overflow signed int.
    const auto bed_size = get_extents(m_bedpts).size();
    const double bed_w = unscaled<double>(bed_size.x());
    const double bed_h = unscaled<double>(bed_size.y());
    const double step_x = double(step.x());
    const double step_y = double(step.y());
    const long long cells_x = (step_x > 0.0) ? (long long)std::floor(bed_w / step_x) : 0;
    const long long cells_y = (step_y > 0.0) ? (long long)std::floor(bed_h / step_y) : 0;
    const long long grid_cap = cells_x * cells_y;

    if (grid_cap > FILL_BED_GRID_PATH_MIN_CAPACITY) {
        auto offset_on_origin = tmpl.poly.contour.bounding_box().center();

        // Use m_bedpts (already shrunk by bed_shrink + brim_skirt_distance/2) as the safe area, so brim/skirt of edge copies stays inside the bed.
        BoundingBox shrunk_bb = Polygon{m_bedpts}.bounding_box();
        BoundingBoxf safe_area_2d(
            Vec2d(unscale<double>(shrunk_bb.min.x()), unscale<double>(shrunk_bb.min.y())),
            Vec2d(unscale<double>(shrunk_bb.max.x()), unscale<double>(shrunk_bb.max.y()))
        );
        std::vector<Vec2f> empty_cells = Plater::get_empty_cells(step, safe_area_2d);

        // Items in m_unselected with bed_idx == 0 have already been normalized to plate-0 LOCAL
        // coordinates in prepare(). They include both real objects and the virtual obstacles of
        // the current plate (excluded regions / wipe tower / non-preferred regions). These
        // virtual obstacles must be counted as occupied: the built-in exclude filtering of
        // Plater::get_empty_cells only takes effect for cur_plate == 0, for cur_plate > 0 it no
        // longer filters these regions.
        // Items with bed_idx > 0 are copies injected onto other plates by
        // preprocess_*_areas(num_plates = MAX_NUM_PLATES), irrelevant to this fill bed run, skip them.
        // Template instances in m_selected (priority > 0) use world coordinates, their bbox must
        // be stride-corrected before use.
        PartPlateList &plist = m_plater->get_partplate_list();
        const int plate_cols = plist.get_plate_cols();
        const int cur_plate_index = plist.get_curr_plate_index();
        const int cur_col = cur_plate_index % plate_cols;
        const int cur_row = cur_plate_index / plate_cols;
        const coord_t stride_dx = bed_stride_x(m_plater) * cur_col;
        const coord_t stride_dy = bed_stride_y(m_plater) * cur_row;

        std::vector<BoundingBoxf> occupied;
        auto add_occupied_local = [&](const ArrangePolygon &ap, bool needs_stride_correction) {
            BoundingBox sbb = ap.transformed_poly().contour.bounding_box();
            if (needs_stride_correction) {
                sbb.translate(-stride_dx, stride_dy);
            }
            occupied.emplace_back(
                Vec2d(unscale<double>(sbb.min.x()), unscale<double>(sbb.min.y())),
                Vec2d(unscale<double>(sbb.max.x()), unscale<double>(sbb.max.y()))
            );
        };
        size_t unsel_blocked = 0, tmpl_blocked = 0;
        for (const auto &ap : m_unselected) {
            if (ap.bed_idx != 0) continue;
            add_occupied_local(ap, false);
            ++unsel_blocked;
        }
        for (const auto &ap : m_selected) {
            if (ap.priority > 0) {
                add_occupied_local(ap, true);
                ++tmpl_blocked;
            }
        }

        const size_t cells_before = empty_cells.size();
        if (!occupied.empty()) {
            const double half_x = step(0) / 2.0;
            const double half_y = step(1) / 2.0;
            // Strict overlap test (with an epsilon tolerance), to avoid irregular holes in the grid caused by floating-point edge touching.
            auto strict_overlap = [](const BoundingBoxf &a, const BoundingBoxf &b) {
                const double ix = std::min(a.max.x(), b.max.x()) - std::max(a.min.x(), b.min.x());
                const double iy = std::min(a.max.y(), b.max.y()) - std::max(a.min.y(), b.min.y());
                return ix > EPSILON && iy > EPSILON;
            };
            empty_cells.erase(
                std::remove_if(empty_cells.begin(), empty_cells.end(),
                    [&](const Vec2f &c) {
                        BoundingBoxf cell(
                            Vec2d(c.x() - half_x, c.y() - half_y),
                            Vec2d(c.x() + half_x, c.y() + half_y)
                        );
                        for (const auto &bb : occupied)
                            if (strict_overlap(bb, cell)) return true;
                        return false;
                    }),
                empty_cells.end()
            );
        }

        // Template instances (priority > 0) stay where they are; new copies (priority == 0) fill
        // empty_cells in order, the overflow gets bed_idx = -1.
        // ap.translation of priority > 0 items is in cur_plate WORLD coordinates (from
        // mi->get_transformation()), and finalize will do `ap.translation += bed_stride_x * ap.col`,
        // so subtract one stride here to turn the translation into plate-0 LOCAL first; that later
        // +stride cancels out back to WORLD, so the template ModelInstance can set_offset to its
        // original position.
        size_t cell_idx = 0, placed = 0, skipped = 0;
        for (size_t i = 0; i < m_selected.size(); ++i) {
            if (ctl.was_canceled()) break;
            if (m_selected[i].priority > 0) {
                m_selected[i].bed_idx = 0;
                m_selected[i].translation(X) -= stride_dx;
                m_selected[i].translation(Y) += stride_dy;
                continue;
            }
            if (cell_idx < empty_cells.size()) {
                m_selected[i].translation = scaled<coord_t>(empty_cells[cell_idx]);
                m_selected[i].translation -= offset_on_origin;
                m_selected[i].bed_idx = 0;
                ++cell_idx;
                ++placed;
                m_selected[i].itemid = static_cast<int>(placed);
            } else {
                m_selected[i].bed_idx = -1;
                ++skipped;
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

            // Translate plate-0 LOCAL coordinates back to cur_plate WORLD coordinates, otherwise
            // new copies on a non-zero plate would land on plate 0. Both the grid and NFP paths
            // produce plate-0 LOCAL, so the stride must always be added back (template instances
            // were pre-corrected by subtracting it once in process, adding it here restores them).
            ap.row = ap.bed_idx / plate_cols;
            ap.col = ap.bed_idx % plate_cols;
            ap.translation(X) += bed_stride_x(m_plater) * ap.col;
            ap.translation(Y) -= bed_stride_y(m_plater) * ap.row;

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

}} // namespace Slic3r::GUI
