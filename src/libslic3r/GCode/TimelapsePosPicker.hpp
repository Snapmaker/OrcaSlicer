#ifndef slic3r_GCode_TimelapsePosPicker_hpp_
#define slic3r_GCode_TimelapsePosPicker_hpp_

// Port of Bambu Studio's TimelapsePosPicker (src/libslic3r/GCode/TimelapsePosPicker.{hpp,cpp},
// v02.08.04.57). It picks the XY spot a BBL toolhead parks at for a timelapse photo: outside the
// printed objects (expanded by the extruder clearance), outside the cone between the camera and
// each object, inside the active extruder's printable area. The machine time_lapse_gcode gets it as
// timelapse_pos_x / timelapse_pos_y with has_timelapse_safe_pos, and hands it to the firmware
// (M9711 U<x> V<y>). Without it the H2D/H2C/H2S templates fall back to branches that park only the
// non-photo nozzle, so the two nozzles of a dual-nozzle machine were photographed differently.

#include <optional>
#include <unordered_map>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

const Point DefaultTimelapsePos = Point(0, 0);
const Point DefaultCameraPos    = Point(0, 0);

class Layer;
class Print;
class PrintObject;
struct PrintInstance;

struct PosPickCtx
{
    Point        curr_pos;            // scaled, G-code coordinates
    const Layer *curr_layer{nullptr};
    int          picture_extruder_id{0}; // the extruder that takes the photo
    int          curr_extruder_id{0};
    std::optional<std::vector<const PrintObject *>> printed_objects; // by-object mode only
    std::optional<Point> farthest_point; // plate-relative scaled; set: pick the safe spot nearest to it
};

// Data are stored without the plate offset.
class TimelapsePosPicker
{
public:
    TimelapsePosPicker()  = default;
    ~TimelapsePosPicker() = default;

    // Unscaled plate position (whole millimetres, as Bambu Studio hands it to the template), or
    // DefaultTimelapsePos when there is no safe spot.
    Point pick_pos(const PosPickCtx &ctx);
    void  init(const Print *print, const Point &plate_offset);
    void  reset();
    bool  get_is_clear_to_x0(const PosPickCtx &ctx);

private:
    void construct_printable_area_by_printer();

    Point pick_pos_for_curr_layer(const PosPickCtx &ctx);
    Point pick_pos_for_all_layer(const PosPickCtx &ctx);

    ExPolygons collect_object_slices_data(const Layer *curr_layer, float height_range, const std::vector<const PrintObject *> &object_list, bool by_object);
    Polygons   collect_limit_areas_for_camera(const std::vector<const PrintObject *> &object_list);
    Polygons   collect_limit_areas_for_rod(const std::vector<const PrintObject *> &object_list, const PosPickCtx &ctx);

    Polygon       expand_object_projection(const Polygon &poly, bool by_object, bool higher_than_curr = true);
    BoundingBoxf3 expand_object_bbox(const BoundingBoxf3 &bbox, bool by_object);

    Point get_objects_center(const std::vector<const PrintObject *> &object_list);

    Polygon                          get_limit_area_for_camera(const PrintObject *obj);
    std::vector<const PrintObject *> get_object_list(const std::optional<std::vector<const PrintObject *>> &printed_objects);

    double        get_raft_height(const PrintObject *obj);
    BoundingBoxf3 get_real_instance_bbox(const PrintInstance &instance);
    Point         get_object_center(const PrintObject *obj);

private:
    const Print            *print{nullptr};
    std::vector<ExPolygons> m_extruder_printable_area; // scaled
    Polygon                 m_bed_polygon;             // scaled
    Point                   m_plate_offset;            // unscaled
    int                     m_plate_height{0};         // unscaled
    int                     m_plate_width{0};          // unscaled

    PrintSequence      m_print_seq{PrintSequence::ByLayer};
    bool               m_based_on_all_layer{false};
    int                m_nozzle_height_to_rod{0};
    int                m_nozzle_clearance_radius{0};
    std::optional<int> m_liftable_extruder_id;
    std::optional<int> m_extruder_height_gap;

    std::unordered_map<const PrintInstance *, BoundingBoxf3> bbox_cache;

    std::optional<Point> m_all_layer_pos;
};

} // namespace Slic3r

#endif // slic3r_GCode_TimelapsePosPicker_hpp_
