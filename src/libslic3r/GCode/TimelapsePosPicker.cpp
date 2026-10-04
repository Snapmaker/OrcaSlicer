// Port of Bambu Studio's src/libslic3r/GCode/TimelapsePosPicker.cpp (v02.08.04.57). Differences:
// - the prime tower footprint comes from the generated tower outline (WipeTowerData::wipe_tower_mesh_data),
//   the fork has no WipeTowerData::bbx;
// - extruder_clearance_max_radius (Bambu's clearance, H2D 96 mm) falls back to Bambu's default (68 mm)
//   when a config has 0 (Bambu Studio refuses 0 in its config check);
// - PrintInstance::get_bounding_box() is not const here.
#include "TimelapsePosPicker.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

constexpr int FILTER_THRESHOLD   = 5;
constexpr int MAX_CANDIDATE_SIZE = 5;

namespace Slic3r {

// Bambu Studio's extruder_clearance_max_radius; its default (68 mm) when unset or 0.
static double clearance_max_radius(const PrintConfig &config)
{
    const double r = config.extruder_clearance_max_radius.value;
    return r > 0. ? r : 68.;
}

void TimelapsePosPicker::init(const Print *print_, const Point &plate_offset)
{
    reset();
    m_plate_offset = plate_offset;
    print          = print_;

    m_nozzle_height_to_rod    = print_->config().extruder_clearance_height_to_rod;
    m_nozzle_clearance_radius = int(clearance_max_radius(print_->config()));
    if (print_->config().nozzle_diameter.size() > 1 && print_->config().extruder_printable_height.values.size() > 1) {
        m_liftable_extruder_id = print_->config().extruder_printable_height.values[0] < print_->config().extruder_printable_height.values[1] ? 0 : 1;
        // Only honor the dual-nozzle height gap when more than one extruder is actually used.
        // Otherwise a dual-nozzle machine printing with a single extruder would needlessly
        // retreat to the trash bin for every shot below the gap.
        auto nozzle_group = print_->get_nozzle_group_result();
        if (nozzle_group && nozzle_group->get_used_extruders().size() > 1)
            m_extruder_height_gap = int(std::abs(print_->config().extruder_printable_height.values[0] -
                                                 print_->config().extruder_printable_height.values[1]));
    }
    m_print_seq          = print_->config().print_sequence.value;
    m_based_on_all_layer = print_->config().timelapse_type == TimelapseType::tlSmooth;

    construct_printable_area_by_printer();
}

void TimelapsePosPicker::reset()
{
    print = nullptr;
    m_bed_polygon.clear();
    m_extruder_printable_area.clear();
    m_all_layer_pos = std::nullopt;
    bbox_cache.clear();

    m_print_seq               = PrintSequence::ByObject;
    m_nozzle_height_to_rod    = 0;
    m_nozzle_clearance_radius = 0;
    m_liftable_extruder_id    = std::nullopt;
    m_extruder_height_gap     = std::nullopt;
    m_based_on_all_layer      = false;
}

std::vector<const PrintObject *> TimelapsePosPicker::get_object_list(const std::optional<std::vector<const PrintObject *>> &printed_objects)
{
    if (printed_objects.has_value())
        return std::vector<const PrintObject *>(printed_objects->begin(), printed_objects->end());
    return std::vector<const PrintObject *>(print->objects().begin(), print->objects().end());
}

// The bed minus the excluded area and the prime tower, intersected with each extruder's printable area.
void TimelapsePosPicker::construct_printable_area_by_printer()
{
    const PrintConfig &config         = print->config();
    const size_t       extruder_count = config.nozzle_diameter.size();
    m_extruder_printable_area.clear();
    m_extruder_printable_area.resize(extruder_count);

    for (size_t idx = 0; idx < config.printable_area.values.size(); ++idx)
        m_bed_polygon.points.emplace_back(coord_t(scale_(config.printable_area.values[idx].x())),
                                          coord_t(scale_(config.printable_area.values[idx].y())));

    auto bed_bbox  = get_extents(m_bed_polygon);
    m_plate_height = int(unscale_(bed_bbox.max.y()));
    m_plate_width  = int(unscale_(bed_bbox.max.x()));

    Polygon bed_exclude_area;
    for (size_t idx = 0; idx < config.bed_exclude_area.values.size(); ++idx)
        bed_exclude_area.points.emplace_back(coord_t(scale_(config.bed_exclude_area.values[idx].x())),
                                             coord_t(scale_(config.bed_exclude_area.values[idx].y())));

    // Bambu Studio: the tower's bounding box (brim included) at the tower position, grown by half
    // the clearance radius.
    Polygons wipe_tower_area;
    if (print->has_wipe_tower()) {
        const WipeTowerData &wtd = print->wipe_tower_data();
        if (wtd.wipe_tower_mesh_data.has_value() && !wtd.wipe_tower_mesh_data->bottom.points.empty()) {
            Polygon footprint = wtd.wipe_tower_mesh_data->bottom;
            footprint.rotate(Geometry::deg2rad(config.wipe_tower_rotation_angle.value));
            const int plate_idx = print->get_plate_index();
            footprint.translate(Point(scale_(config.wipe_tower_x.get_at(plate_idx)), scale_(config.wipe_tower_y.get_at(plate_idx))));
            const BoundingBox bb = get_extents(footprint);
            Polygon box{bb.min, Point(bb.max.x(), bb.min.y()), bb.max, Point(bb.min.x(), bb.max.y())};
            Polygon grown = expand_object_projection(box, m_print_seq == PrintSequence::ByObject);
            if (!grown.empty())
                wipe_tower_area.emplace_back(std::move(grown));
        }
    }

    for (size_t idx = 0; idx < extruder_count; ++idx) {
        ExPolygons printable_area = diff_ex(diff(Polygons{m_bed_polygon}, Polygons{bed_exclude_area}), wipe_tower_area);
        if (idx < config.extruder_printable_area.size()) {
            Polygon extruder_printable_area;
            for (size_t j = 0; j < config.extruder_printable_area.values[idx].size(); ++j)
                extruder_printable_area.points.emplace_back(coord_t(scale_(config.extruder_printable_area.values[idx][j].x())),
                                                            coord_t(scale_(config.extruder_printable_area.values[idx][j].y())));
            if (!extruder_printable_area.empty())
                printable_area = intersection_ex(printable_area, Polygons{extruder_printable_area});
        }
        m_extruder_printable_area[idx] = printable_area;
    }
}

// Object footprints (instance bounding boxes, expanded by the clearance) whose height range meets
// [print_z + height_range, print_z] (or [print_z, print_z + height_range]).
ExPolygons TimelapsePosPicker::collect_object_slices_data(const Layer *layer, float height_range, const std::vector<const PrintObject *> &object_list, bool by_object)
{
    auto range_intersect = [](int left1, int right1, int left2, int right2) {
        if (left1 <= left2 && left2 <= right1)
            return true;
        if (left2 <= left1 && left1 <= right2)
            return true;
        return false;
    };
    ExPolygons ret;
    float      z_target = float(layer->print_z);
    float      z_low    = height_range < 0 ? float(layer->print_z) + height_range : float(layer->print_z);
    float      z_high   = height_range < 0 ? float(layer->print_z) : float(layer->print_z) + height_range;
    if (z_low <= 0)
        return to_expolygons({m_bed_polygon});

    for (auto &obj : object_list) {
        for (auto &instance : obj->instances()) {
            auto instance_bbox          = get_real_instance_bbox(instance);
            bool higher_than_curr_layer = (obj == object_list.back()) ? false : instance_bbox.max.z() > z_target;
            if (range_intersect(int(instance_bbox.min.z()), int(instance_bbox.max.z()), int(z_low), int(z_high))) {
                ExPolygon expoly;
                expoly.contour = {{coord_t(scale_(instance_bbox.min.x())), coord_t(scale_(instance_bbox.min.y()))},
                                  {coord_t(scale_(instance_bbox.max.x())), coord_t(scale_(instance_bbox.min.y()))},
                                  {coord_t(scale_(instance_bbox.max.x())), coord_t(scale_(instance_bbox.max.y()))},
                                  {coord_t(scale_(instance_bbox.min.x())), coord_t(scale_(instance_bbox.max.y()))}};
                expoly.contour = expand_object_projection(expoly.contour, by_object, higher_than_curr_layer);
                ret.emplace_back(std::move(expoly));
            }
        }
    }
    ret = union_ex(ret);
    return ret;
}

Polygons TimelapsePosPicker::collect_limit_areas_for_camera(const std::vector<const PrintObject *> &object_list)
{
    Polygons ret;
    for (auto &obj : object_list)
        ret.emplace_back(get_limit_area_for_camera(obj));
    ret = union_(ret);
    return ret;
}

// scaled data
Polygons TimelapsePosPicker::collect_limit_areas_for_rod(const std::vector<const PrintObject *> &object_list, const PosPickCtx &ctx)
{
    double                           rod_limit_height = m_nozzle_height_to_rod + ctx.curr_layer->print_z;
    std::vector<const PrintObject *> rod_collision_candidates;
    for (auto &obj : object_list) {
        if (ctx.printed_objects && obj == ctx.printed_objects->back())
            continue;
        auto bbox = get_real_instance_bbox(obj->instances().front());
        if (bbox.max.z() >= rod_limit_height)
            rod_collision_candidates.push_back(obj);
    }

    if (rod_collision_candidates.empty())
        return {};

    std::vector<BoundingBoxf3> collision_obj_bboxs;
    for (auto obj : rod_collision_candidates)
        collision_obj_bboxs.emplace_back(expand_object_bbox(get_real_instance_bbox(obj->instances().front()), m_print_seq == PrintSequence::ByObject));

    std::sort(collision_obj_bboxs.begin(), collision_obj_bboxs.end(), [&](const auto &lbbox, const auto &rbbox) {
        if (lbbox.min.y() == rbbox.min.y())
            return lbbox.max.y() < rbbox.max.y();
        return lbbox.min.y() < rbbox.min.y();
    });

    std::vector<std::pair<int, int>> object_y_ranges = {{0, 0}};
    for (auto &bbox : collision_obj_bboxs) {
        if (object_y_ranges.back().second >= bbox.min.y())
            object_y_ranges.back().second = int(bbox.max.y());
        else
            object_y_ranges.emplace_back(int(bbox.min.y()), int(bbox.max.y()));
    }

    if (object_y_ranges.back().second < m_plate_height)
        object_y_ranges.emplace_back(m_plate_height, m_plate_height);

    int   lower_y_pos = -1, upper_y_pos = -1;
    Point unscaled_curr_pos = {coord_t(unscale_(ctx.curr_pos.x()) - m_plate_offset.x()), coord_t(unscale_(ctx.curr_pos.y()) - m_plate_offset.y())};

    for (size_t idx = 1; idx < object_y_ranges.size(); ++idx) {
        if (unscaled_curr_pos.y() >= object_y_ranges[idx - 1].second && unscaled_curr_pos.y() <= object_y_ranges[idx].first) {
            lower_y_pos = object_y_ranges[idx - 1].second;
            upper_y_pos = object_y_ranges[idx].first;
            break;
        }
    }

    if (lower_y_pos == -1 && upper_y_pos == -1)
        return {m_bed_polygon};

    Polygons ret;
    ret.emplace_back(Polygon{Point{coord_t(scale_(0)), coord_t(scale_(0))}, Point{coord_t(scale_(m_plate_width)), coord_t(scale_(0))},
                             Point{coord_t(scale_(m_plate_width)), coord_t(scale_(lower_y_pos))}, Point{coord_t(scale_(0)), coord_t(scale_(lower_y_pos))}});
    ret.emplace_back(Polygon{Point{coord_t(scale_(0)), coord_t(scale_(upper_y_pos))}, Point{coord_t(scale_(m_plate_width)), coord_t(scale_(upper_y_pos))},
                             Point{coord_t(scale_(m_plate_width)), coord_t(scale_(m_plate_height))},
                             Point{coord_t(scale_(0)), coord_t(scale_(m_plate_height))}});
    return ret;
}

// expand the object expolygon by safe distance, scaled data
Polygon TimelapsePosPicker::expand_object_projection(const Polygon &poly, bool by_object, bool higher_than_curr)
{
    const double max_radius = clearance_max_radius(print->config());
    float        radius     = 0;
    if (by_object && higher_than_curr)
        radius = float(scale_(max_radius));
    else
        radius = float(scale_(max_radius / 2));

    // the input poly is bounding box, so we get the first offseted polygon is ok
    auto ret = offset(poly, radius);
    if (ret.empty())
        return {};
    return ret[0];
}

// unscaled data
BoundingBoxf3 TimelapsePosPicker::expand_object_bbox(const BoundingBoxf3 &bbox, bool by_object)
{
    const double max_radius = clearance_max_radius(print->config());
    double       radius     = by_object ? max_radius : max_radius / 2;

    BoundingBoxf3 ret = bbox;
    ret.min.x() -= radius;
    ret.min.y() -= radius;
    ret.max.x() += radius;
    ret.max.y() += radius;
    return ret;
}

double TimelapsePosPicker::get_raft_height(const PrintObject *obj)
{
    if (!obj || !obj->has_raft())
        return 0;
    auto   slice_params              = obj->slicing_parameters();
    int    base_raft_layers          = int(slice_params.base_raft_layers);
    double base_raft_height          = slice_params.base_raft_layer_height;
    int    interface_raft_layers     = int(slice_params.interface_raft_layers);
    double interface_raft_height     = slice_params.interface_raft_layer_height;
    double contact_raft_layer_height = slice_params.contact_raft_layer_height;

    double ret = print->config().initial_layer_print_height;
    if (base_raft_layers - 1 > 0)
        ret += (base_raft_layers - 1) * base_raft_height;
    if (interface_raft_layers - 1 > 0)
        ret += (interface_raft_layers - 1) * interface_raft_height;
    if (obj->config().raft_layers > 1)
        ret += contact_raft_layer_height;

    return ret + slice_params.gap_raft_object;
}

// the real instance bounding box: plate offset removed, raft height added. unscaled data
BoundingBoxf3 TimelapsePosPicker::get_real_instance_bbox(const PrintInstance &instance)
{
    auto iter = bbox_cache.find(&instance);
    if (iter != bbox_cache.end())
        return iter->second;

    auto   bbox        = const_cast<PrintInstance &>(instance).get_bounding_box();
    double raft_height = get_raft_height(instance.print_object);
    bbox.max.z() += raft_height;
    bbox.min.x() -= m_plate_offset.x();
    bbox.max.x() -= m_plate_offset.x();
    bbox.min.y() -= m_plate_offset.y();
    bbox.max.y() -= m_plate_offset.y();

    bbox_cache[&instance] = bbox;
    return bbox;
}

// The quadrilateral between the camera (plate origin) and the object, grown by half the clearance.
Polygon TimelapsePosPicker::get_limit_area_for_camera(const PrintObject *obj)
{
    if (!obj)
        return {};
    auto  bbox   = get_real_instance_bbox(obj->instances().front());
    float radius = float(m_nozzle_clearance_radius) / 2;

    auto offset_bbox = bbox.inflated(std::sqrt(2.) * radius);
    // Constrain the coordinates to the first quadrant.
    Polygon ret = {DefaultCameraPos,
                   Point{coord_t(std::max(scale_(offset_bbox.max.x()), 0.)), coord_t(std::max(scale_(offset_bbox.min.y()), 0.))},
                   Point{coord_t(std::max(scale_(offset_bbox.max.x()), 0.)), coord_t(std::max(scale_(offset_bbox.max.y()), 0.))},
                   Point{coord_t(std::max(scale_(offset_bbox.min.x()), 0.)), coord_t(std::max(scale_(offset_bbox.max.y()), 0.))}};
    return ret;
}

// The point of the safe areas nearest to curr_pos (penalised for standing in front of the camera),
// curr_pos itself when it is already safe, DefaultTimelapsePos when nothing is safe. With a
// farthest point, the safe point nearest to it instead.
static Point pick_pos_internal(const Point &curr_pos, const ExPolygons &safe_areas, const ExPolygons &path_collision_area,
                               bool detect_path_collision, const std::optional<Point> &farthest_point = std::nullopt)
{
    struct CandidatePoint
    {
        double dist;
        Point  point;
        bool   operator<(const CandidatePoint &other) const { return dist < other.dist; }
    };

    if (std::any_of(safe_areas.begin(), safe_areas.end(), [&curr_pos](const ExPolygon &p) { return p.contains(curr_pos); }))
        return curr_pos;

    if (safe_areas.empty())
        return DefaultTimelapsePos;

    std::priority_queue<CandidatePoint> max_heap;

    const double candidate_point_segment = scale_(5.);
    const double weight_of_camera        = 1. / 3.;
    auto penaltyFunc = [&farthest_point, weight_of_camera](const Point &curr_post, const Point &CameraPos, const Point &candidatet) -> double {
        if (farthest_point.has_value()) {
            // Prefer candidate closest to the farthest point (L1 norm)
            return double((farthest_point.value() - candidatet).cwiseAbs().sum());
        }
        // move distance + Camera occlusion penalty function
        return double((curr_post - candidatet).cwiseAbs().sum()) - weight_of_camera * double((CameraPos - candidatet).cwiseAbs().sum());
    };

    for (const auto &expoly : safe_areas) {
        Polygons polys = to_polygons(expoly);
        for (auto &poly : polys) {
            for (size_t idx = 0; idx < poly.points.size(); ++idx) {
                double best_penalty   = std::numeric_limits<double>::max();
                Point  best_candidate = DefaultTimelapsePos; // the best candidate of the current line
                if (double((poly.points[idx] - poly.points[next_idx_modulo(idx, poly.points)]).cwiseAbs().sum()) < candidate_point_segment) {
                    best_candidate = poly.points[idx]; // only check the start point if the line is short
                    best_penalty   = penaltyFunc(curr_pos, DefaultCameraPos, best_candidate);
                } else {
                    Point  direct_of_line = poly.points[next_idx_modulo(idx, poly.points)] - poly.points[idx];
                    double length_L1      = double(direct_of_line.cwiseAbs().sum());
                    int    num_steps      = static_cast<int>(length_L1 / candidate_point_segment); // 5 mm steps along long lines
                    // divide by length_L1 instead of steps, so the step length does not lose accuracy
                    direct_of_line.x() = static_cast<coord_t>(static_cast<double>(direct_of_line.x()) * candidate_point_segment / length_L1);
                    direct_of_line.y() = static_cast<coord_t>(static_cast<double>(direct_of_line.y()) * candidate_point_segment / length_L1);
                    for (int line_seg_i = 0; line_seg_i <= num_steps; ++line_seg_i) {
                        Point  candidate = poly.points[idx] + direct_of_line * line_seg_i;
                        double dist      = penaltyFunc(curr_pos, DefaultCameraPos, candidate);
                        if (dist < best_penalty) {
                            best_penalty   = dist;
                            best_candidate = candidate;
                        } // only push the best point of the whole line into the heap
                    }
                }
                max_heap.push({best_penalty, best_candidate});
                if (max_heap.size() > MAX_CANDIDATE_SIZE)
                    max_heap.pop();
            }
        }
    }

    std::vector<Point> top_candidates;
    while (!max_heap.empty()) {
        top_candidates.push_back(max_heap.top().point);
        max_heap.pop();
    }
    std::reverse(top_candidates.begin(), top_candidates.end());

    for (auto &p : top_candidates) {
        if (!detect_path_collision)
            return p;
        Polyline path(curr_pos, p);
        if (intersection_pl(Polylines{path}, path_collision_area).empty())
            return p;
    }

    return DefaultTimelapsePos;
}

Point TimelapsePosPicker::pick_pos(const PosPickCtx &ctx)
{
    Point res;
    if (m_based_on_all_layer)
        res = pick_pos_for_all_layer(ctx);
    else
        res = pick_pos_for_curr_layer(ctx);

    return {coord_t(unscale_(res.x())), coord_t(unscale_(res.y()))};
}

// centre of an object, scaled data
Point TimelapsePosPicker::get_object_center(const PrintObject *obj)
{
    if (!obj)
        return {};
    // in Bambu Studio each object has one instance
    const auto &instance      = obj->instances().front();
    auto        instance_bbox = get_real_instance_bbox(instance);
    return {coord_t(scale_((instance_bbox.min.x() + instance_bbox.max.x()) / 2)), coord_t(scale_((instance_bbox.min.y() + instance_bbox.max.y()) / 2))};
}

// scaled data
Point TimelapsePosPicker::pick_pos_for_curr_layer(const PosPickCtx &ctx)
{
    float height_gap = 0;
    if (ctx.curr_extruder_id != ctx.picture_extruder_id) {
        if (m_liftable_extruder_id.has_value() && ctx.picture_extruder_id != m_liftable_extruder_id && m_extruder_height_gap.has_value())
            height_gap = float(-*m_extruder_height_gap);
    }

    bool                             by_object   = m_print_seq == PrintSequence::ByObject;
    std::vector<const PrintObject *> object_list = get_object_list(ctx.printed_objects);

    ExPolygons layer_slices       = collect_object_slices_data(ctx.curr_layer, height_gap, object_list, by_object);
    Polygons   camera_limit_areas = collect_limit_areas_for_camera(object_list);
    Polygons   rod_limit_areas;
    if (by_object && ctx.printed_objects && !ctx.printed_objects->empty())
        rod_limit_areas = collect_limit_areas_for_rod(object_list, ctx);
    ExPolygons unplacable_area = union_ex(union_ex(layer_slices, camera_limit_areas), rod_limit_areas);
    const size_t curr_extruder = size_t(std::clamp(ctx.curr_extruder_id, 0, int(m_extruder_printable_area.size()) - 1));
    ExPolygons   extruder_printable_area = m_extruder_printable_area.empty() ? ExPolygons() : m_extruder_printable_area[curr_extruder];

    ExPolygons safe_area = diff_ex(extruder_printable_area, unplacable_area);
    safe_area            = opening_ex(safe_area, float(scale_(FILTER_THRESHOLD)));

    Point center_p;
    if (by_object && ctx.printed_objects && !ctx.printed_objects->empty())
        center_p = get_object_center(ctx.printed_objects->back());
    else
        center_p = get_objects_center(object_list);

    ExPolygons path_collision_area;
    if (by_object && ctx.printed_objects && !ctx.printed_objects->empty()) {
        auto object_without_curr = ctx.printed_objects;
        if (object_without_curr && !object_without_curr->empty())
            object_without_curr->pop_back();

        ExPolygons layer_slices_without_curr = collect_object_slices_data(ctx.curr_layer, height_gap, get_object_list(object_without_curr), by_object);
        path_collision_area                  = union_ex(layer_slices_without_curr, rod_limit_areas);
    }

    return pick_pos_internal(center_p, safe_area, path_collision_area, by_object, ctx.farthest_point);
}

// The average centre of all instances of the objects, scaled data.
Point TimelapsePosPicker::get_objects_center(const std::vector<const PrintObject *> &object_list)
{
    if (object_list.empty())
        return Point(0, 0);
    double sum_x = 0.0, sum_y = 0.0;
    size_t total_instances = 0;
    for (auto &obj : object_list) {
        for (auto &instance : obj->instances()) {
            const auto &bbox = get_real_instance_bbox(instance);
            sum_x += (bbox.min.x() + bbox.max.x()) / 2.;
            sum_y += (bbox.min.y() + bbox.max.y()) / 2.;
            total_instances += 1;
        }
    }
    if (total_instances == 0)
        return Point(0, 0);
    return Point{coord_t(scale_(sum_x / total_instances)), coord_t(scale_(sum_y / total_instances))};
}

Point TimelapsePosPicker::pick_pos_for_all_layer(const PosPickCtx &ctx)
{
    bool by_object = m_print_seq == PrintSequence::ByObject;
    if (by_object)
        return DefaultTimelapsePos;

    // In smooth timelapse both nozzles share a single fixed picture position. While the
    // current layer is still below the dual-nozzle height gap, the idle (lower) nozzle can
    // collide with printed parts, so always retreat to the trash bin position regardless of
    // which extruder takes the picture. Traditional (per-layer) mode is handled separately.
    if (m_extruder_height_gap.has_value() && ctx.curr_layer->print_z <= *m_extruder_height_gap)
        return DefaultTimelapsePos;
    if (m_all_layer_pos)
        return *m_all_layer_pos;

    Polygons object_projections;
    auto     object_list = get_object_list(std::nullopt);
    for (auto &obj : object_list) {
        for (auto &instance : obj->instances()) {
            const auto &bbox = get_real_instance_bbox(instance);
            Point       min_p{coord_t(scale_(bbox.min.x())), coord_t(scale_(bbox.min.y()))};
            Point       max_p{coord_t(scale_(bbox.max.x())), coord_t(scale_(bbox.max.y()))};
            Polygon     obj_proj{{min_p.x(), min_p.y()}, {max_p.x(), min_p.y()}, {max_p.x(), max_p.y()}, {min_p.x(), max_p.y()}};
            object_projections.emplace_back(expand_object_projection(obj_proj, by_object));
        }
    }

    object_projections          = union_(object_projections);
    Polygons camera_limit_areas = collect_limit_areas_for_camera(object_list);
    Polygons unplacable_area    = union_(object_projections, camera_limit_areas);

    ExPolygons extruder_printable_area;
    if (m_extruder_printable_area.size() > 1)
        extruder_printable_area = intersection_ex(m_extruder_printable_area[0], m_extruder_printable_area[1]);
    else if (m_extruder_printable_area.size() == 1)
        extruder_printable_area = m_extruder_printable_area.front();

    ExPolygons safe_area = diff_ex(extruder_printable_area, unplacable_area);
    safe_area            = opening_ex(safe_area, float(scale_(FILTER_THRESHOLD)));

    Point starting_pos = get_objects_center(object_list);

    m_all_layer_pos = pick_pos_internal(starting_pos, safe_area, {}, by_object);
    return *m_all_layer_pos;
}

bool TimelapsePosPicker::get_is_clear_to_x0(const PosPickCtx &ctx)
{
    bool                             by_object   = m_print_seq == PrintSequence::ByObject;
    std::vector<const PrintObject *> object_list = get_object_list(ctx.printed_objects);

    auto range_intersect = [](int left1, int right1, int left2, int right2) {
        if (left1 <= left2 && left2 <= right1)
            return true;
        if (left2 <= left1 && left1 <= right2)
            return true;
        return false;
    };

    ExPolygons   unclear_area;
    const Layer *layer    = ctx.curr_layer;
    float        z_target = float(layer->print_z);
    float        z_low    = float(layer->print_z - 0.5);
    float        z_high   = float(layer->print_z + 0.5);

    for (auto &obj : object_list) {
        for (auto &instance : obj->instances()) {
            auto instance_bbox        = get_real_instance_bbox(instance);
            bool is_curr_obj          = (obj == object_list.back()) || (!by_object);
            bool higher_than_curr_pos = instance_bbox.max.z() > z_target;
            if (!is_curr_obj && range_intersect(int(instance_bbox.min.z()), int(instance_bbox.max.z()), int(z_low), int(z_high))) {
                ExPolygon expoly;
                expoly.contour = {{coord_t(scale_(instance_bbox.min.x())), coord_t(scale_(instance_bbox.min.y()))},
                                  {coord_t(scale_(instance_bbox.max.x())), coord_t(scale_(instance_bbox.min.y()))},
                                  {coord_t(scale_(instance_bbox.max.x())), coord_t(scale_(instance_bbox.max.y()))},
                                  {coord_t(scale_(instance_bbox.min.x())), coord_t(scale_(instance_bbox.max.y()))}};
                expoly.contour = expand_object_projection(expoly.contour, by_object, higher_than_curr_pos);
                unclear_area.emplace_back(std::move(expoly));
            }
        }
    }

    Point curr_pos_in_plate = {coord_t(ctx.curr_pos.x() - scale_(m_plate_offset.x())), coord_t(ctx.curr_pos.y() - scale_(m_plate_offset.y()))};
    for (const ExPolygon &expoly : unclear_area) {
        BoundingBox bbox = expoly.contour.bounding_box();
        if (curr_pos_in_plate.y() < bbox.min.y() || curr_pos_in_plate.y() > bbox.max.y())
            continue;
        if (bbox.min.x() <= curr_pos_in_plate.x())
            return false;
    }

    return true;
}

} // namespace Slic3r
