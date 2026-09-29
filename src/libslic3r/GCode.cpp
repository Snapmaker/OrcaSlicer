#include "BoundingBox.hpp"
#include "Config.hpp"
#include "GCode/WipePathHelpers.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "GCode.hpp"
#include "LocalZOrderOptimizer.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "EdgeGrid.hpp"
#include "Geometry/ConvexHull.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/Thumbnails.hpp"
#include "ImageRowWalls.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "ShortestPath.hpp"
#include "MixedFilament.hpp"
#include "Print.hpp"
#include "Utils.hpp"
#include "BrimFilament.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include "LocalesUtils.hpp"
#include "libslic3r/format.hpp"
#include "Time.hpp"
#include "GCode/ExtrusionProcessor.hpp"
#include "GCode/PreCoolingInjector.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <numeric>
#include <math.h>
#include <optional>
#include <memory>
#include <set>
#include <stdlib.h>
#include <string>
#include <utility>
#include <string_view>

#include <regex>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>
#include "calib.hpp"
// Intel redesigned some TBB interface considerably when merging TBB with their oneAPI set of libraries, see GH #7332.
// We are using quite an old TBB 2017 U7. Before we update our build servers, let's use the old API, which is deprecated in up to date TBB.
#if !defined(TBB_VERSION_MAJOR)
#include <tbb/version.h>
#endif
#if !defined(TBB_VERSION_MAJOR)
static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
#include <tbb/parallel_pipeline.h>
using slic3r_tbb_filtermode = tbb::filter_mode;
#else
#include <tbb/pipeline.h>
using slic3r_tbb_filtermode = tbb::filter;
#endif

#include <Shiny/Shiny.h>

#include "miniz_extension.hpp"

using namespace std::literals::string_view_literals;

#if 0
// Enable debugging and asserts, even in the release build.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

//! macro used to mark string used at localization,
//! return same string
#define L(s) (s)
#define _(s) Slic3r::I18N::translate(s)

// Ultra (H2C rack): ports of BambuStudio GCode.cpp:119-155. The H2C change_filament template
// indexes these by *logical nozzle id*, not by extruder: an H2C has 2 extruders but up to 7
// logical nozzles (1 left + a rack of 6 on the right), so nozzle_diameter (one entry per
// extruder) is the wrong array and reads out of range for any rack slot past the first.
std::vector<double> get_nozzle_diameters_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result)
{
    std::vector<double> diameters;
    if (!group_result)
        return diameters;
    for (int id = 0;; ++id) {
        auto nozzle = group_result->get_nozzle_from_id(id);
        if (!nozzle)
            break;
        diameters.push_back(string_to_double_decimal_point(nozzle->diameter));
    }
    return diameters;
}

// Upstream feeds nozzle_volume_types as strings ("Standard", "TPU High Flow", ...); the H2C
// change_filament template compares it against those literals.
std::vector<std::string> get_nozzle_volume_types_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result)
{
    std::vector<std::string> volume_types;
    if (!group_result)
        return volume_types;

    int max_nozzle_id = -1;
    for (unsigned int filament_id : group_result->get_used_filaments()) {
        for (const auto &nozzle : group_result->get_nozzles_for_filament(int(filament_id))) {
            if (nozzle.group_id > max_nozzle_id)
                max_nozzle_id = nozzle.group_id;
        }
    }
    if (max_nozzle_id < 0)
        max_nozzle_id = 0;

    volume_types.resize(max_nozzle_id + 1, get_nozzle_volume_type_string(NozzleVolumeType::nvtStandard));
    for (int id = 0; id <= max_nozzle_id; ++id) {
        auto nozzle = group_result->get_nozzle_from_id(id);
        if (nozzle)
            volume_types[id] = get_nozzle_volume_type_string(nozzle->volume_type);
    }
    return volume_types;
}

// Ultra (H2C rack): the in-extruder nozzle-change block. This is BambuStudio
// WipeTower::ramming(WipeTower.cpp:3388-3568) evaluated with nozzle_change_line_count == 0,
// which is structurally what this fork has: its Orca-derived wipe tower carries no
// WipeTowerBlock and therefore no nozzle-change purge area, so upstream's ramming lines, its
// second M632/M633 pair and its reverse travel are all inside "if (nozzle_change_line_count > 0)"
// and drop out. What survives is the marker pair and the skippable pre-cool block - which is
// the whole of the rack-conditional content the firmware reads.
//   old/new_filament_id : filament (== "extruder" in this fork's naming) ids
//   old/new_nozzle_id   : logical nozzle ids from the plate grouping result
//   extruder_change     : true when the two nozzles sit on different extruders; upstream then
//                         emits the markers only, no M632/M633 (see the gold file: the 98
//                         extruder changes carry no interlock, the 112 rack changes do).
std::string format_nozzle_change_block(int  old_filament_id,
                                       int  new_filament_id,
                                       int  old_nozzle_id,
                                       int  new_nozzle_id,
                                       bool extruder_change,
                                       bool dynamic_nozzle_map,
                                       int  precool_temp,
                                       int  physical_extruder)
{
    char        buf[128];
    std::string gcode;
    snprintf(buf, sizeof(buf), "; NOZZLE_CHANGE_START OF%d NF%d ON%d NN%d\n", old_filament_id, new_filament_id, old_nozzle_id,
             new_nozzle_id);
    gcode += buf;

    if (!extruder_change) {
        // M632 S<filament> [H<nozzle>] M N - "prepare this filament, nozzle re-select allowed".
        // H is only written for a dynamic nozzle map (a selector machine); a static map writes no
        // H, matching the H-1 the same plate already puts on T<f> / M620 S<f>A / M620.6.
        gcode += "M632 S" + std::to_string(new_filament_id);
        if (dynamic_nozzle_map && new_nozzle_id >= 0)
            gcode += " H" + std::to_string(new_nozzle_id);
        gcode += " M N\n";

        // The pre-cool, inside the skippable block. T is the PHYSICAL extruder
        // (physical_extruder_map[logical extruder]); N0 marks the line slicer-generated.
        if (precool_temp != 0) {
            // M400 first: G1 is non-blocking, so an M104 cool-down must not start mid-travel.
            gcode += "M400\n";
            gcode += "M104";
            if (physical_extruder != -1)
                gcode += " T" + std::to_string(physical_extruder);
            gcode += " S" + std::to_string(precool_temp) + " N0 ;Wipe tower nozzle change pre cooling\n";
            gcode += "M106 S255\n";
        }
        gcode += "M633\n";
    }

    snprintf(buf, sizeof(buf), "; NOZZLE_CHANGE_END OF%d NF%d ON%d NN%d\n", old_filament_id, new_filament_id, old_nozzle_id,
             new_nozzle_id);
    gcode += buf;
    return gcode;
}

std::string GCode::nozzle_change_gcode(int old_filament_id, int new_filament_id, int old_nozzle_id, int new_nozzle_id, bool extruder_change) const
{
    auto group_result = m_curr_print ? m_curr_print->get_layered_nozzle_group_result() : nullptr;
    if (!group_result)
        return std::string();

    const int precool = m_config.filament_pre_cooling_temperature_nc.values.empty() ?
                            0 :
                            m_config.filament_pre_cooling_temperature_nc.get_at(old_filament_id);
    int       physical_extruder = -1;
    if (precool != 0) {
        const int logical_extruder = group_result->get_extruder_id(old_filament_id, m_layer_index);
        if (logical_extruder >= 0 && logical_extruder < int(m_config.physical_extruder_map.size()))
            physical_extruder = m_config.physical_extruder_map.get_at(logical_extruder);
    }
    return format_nozzle_change_block(old_filament_id, new_filament_id, old_nozzle_id, new_nozzle_id, extruder_change,
                                      group_result->is_support_dynamic_nozzle_map(), precool, physical_extruder);
}

static const float g_min_purge_volume      = 100.f;
static const float g_purge_volume_one_time = 135.f;
static const int   g_max_flush_count       = 4;
// static const size_t g_max_label_object = 64;

Vec2d travel_point_1;
Vec2d travel_point_2;
Vec2d travel_point_3;

static std::vector<Vec2d> get_path_of_change_filament(const Print& print)
{
    // give safe value in case there is no start_end_points in config
    std::vector<Vec2d> out_points;
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 0));
    out_points.emplace_back(Vec2d(54, 245));

    // get the start_end_points from config (20, -3) (54, 245)
    Pointfs points = print.config().start_end_points.values;
    if (points.size() != 2)
        return out_points;

    Vec2d start_point = points[0];
    Vec2d end_point   = points[1];

    // the cutter area size(18, 28)
    Pointfs excluse_area = print.config().bed_exclude_area.values;
    if (excluse_area.size() != 4)
        return out_points;

    double cutter_area_x = excluse_area[2].x() + 2;
    double cutter_area_y = excluse_area[2].y() + 2;

    double start_x_position = start_point.x();
    double end_x_position   = end_point.x();
    double end_y_position   = end_point.y();

    bool can_travel_form_left = true;

    // step 1: get the x-range intervals of all objects
    std::vector<std::pair<double, double>> object_intervals;
    for (PrintObject* print_object : print.objects()) {
        const PrintInstances& print_instances = print_object->instances();
        BoundingBoxf3         bounding_box    = print_instances[0].model_instance->get_object()->bounding_box_exact();

        if (bounding_box.min.x() < start_x_position && bounding_box.min.y() < cutter_area_y)
            can_travel_form_left = false;

        std::pair<double, double> object_scope = std::make_pair(bounding_box.min.x() - 2, bounding_box.max.x() + 2);
        if (object_intervals.empty())
            object_intervals.push_back(object_scope);
        else {
            std::vector<std::pair<double, double>> new_object_intervals;
            bool                                   intervals_intersect = false;
            std::pair<double, double>              new_merged_scope;
            for (auto object_interval : object_intervals) {
                if (object_interval.second >= object_scope.first && object_interval.first <= object_scope.second) {
                    if (intervals_intersect) {
                        new_merged_scope = std::make_pair(std::min(object_interval.first, new_merged_scope.first),
                                                          std::max(object_interval.second, new_merged_scope.second));
                    } else { // it is the first intersection
                        new_merged_scope = std::make_pair(std::min(object_interval.first, object_scope.first),
                                                          std::max(object_interval.second, object_scope.second));
                    }
                    intervals_intersect = true;
                } else {
                    new_object_intervals.push_back(object_interval);
                }
            }

            if (intervals_intersect) {
                new_object_intervals.push_back(new_merged_scope);
                object_intervals = new_object_intervals;
            } else
                object_intervals.push_back(object_scope);
        }
    }

    // step 2: get the available x-range
    std::sort(object_intervals.begin(), object_intervals.end(),
              [](const std::pair<double, double>& left, const std::pair<double, double>& right) { return left.first < right.first; });
    std::vector<std::pair<double, double>> available_intervals;
    double                                 start_position = 0;
    for (auto object_interval : object_intervals) {
        if (object_interval.first > start_position)
            available_intervals.push_back(std::make_pair(start_position, object_interval.first));
        start_position = object_interval.second;
    }
    available_intervals.push_back(std::make_pair(start_position, 255));

    // step 3: get the nearest path
    double new_path = 255;
    for (auto available_interval : available_intervals) {
        if (available_interval.first > end_x_position) {
            double distance = available_interval.first - end_x_position;
            new_path        = abs(end_x_position - new_path) < distance ? new_path : available_interval.first;
            break;
        } else {
            if (available_interval.second >= end_x_position) {
                new_path = end_x_position;
                break;
            } else if (!can_travel_form_left && available_interval.second < start_x_position) {
                continue;
            } else {
                new_path = available_interval.second;
            }
        }
    }

    // step 4: generate path points  (new_path == start_x_position means not need to change path)
    Vec2d out_point_1;
    Vec2d out_point_2;
    Vec2d out_point_3;
    if (new_path < start_x_position) {
        out_point_1 = Vec2d(start_x_position, cutter_area_y);
        out_point_2 = Vec2d(new_path, cutter_area_y);
        out_point_3 = Vec2d(new_path, end_y_position);
    } else {
        out_point_1 = Vec2d(new_path, 0);
        out_point_2 = Vec2d(new_path, 0);
        out_point_3 = Vec2d(new_path, end_y_position);
    }

    out_points.clear();
    out_points.emplace_back(out_point_1);
    out_points.emplace_back(out_point_2);
    out_points.emplace_back(out_point_3);

    return out_points;
}

// Only add a newline in case the current G-code does not end with a newline.
static inline void check_add_eol(std::string& gcode)
{
    if (!gcode.empty() && gcode.back() != '\n')
        gcode += '\n';
}

// BBS: publish the extruder-change long-retraction placeholders for one filament.
// long_retractions_when_ec / retraction_distances_when_ec are per-filament and NULLABLE: a filament
// whose preset does not mention the key carries nil, and nil must read as "feature off" rather than
// as the raw sentinel (a nil bool is 0xFF, which would test as true; a nil float is NaN). Guarding
// here keeps an old project or preset that predates these keys producing exactly today's K0 R0.
static void set_ec_retraction_placeholders(PlaceholderParser& pp, const PrintConfig& config, size_t filament_idx)
{
    const ConfigOptionBoolsNullable&  long_opt = config.long_retractions_when_ec;
    const ConfigOptionFloatsNullable& dist_opt = config.retraction_distances_when_ec;

    bool   long_retraction = false;
    double distance        = 0.;
    if (!long_opt.values.empty()) {
        const size_t i = std::min(filament_idx, long_opt.values.size() - 1);
        if (!long_opt.is_nil(i))
            long_retraction = long_opt.values[i] != 0;
    }
    if (!dist_opt.values.empty()) {
        const size_t i = std::min(filament_idx, dist_opt.values.size() - 1);
        if (!dist_opt.is_nil(i) && !std::isnan(dist_opt.values[i]))
            distance = dist_opt.values[i];
    }
    // The distance is meaningless with the feature off; keep the pair consistent (K0 always pairs with R0).
    if (!long_retraction)
        distance = 0.;

    pp.set("long_retraction_when_ec",     new ConfigOptionBool(long_retraction));
    pp.set("retraction_distance_when_ec", new ConfigOptionFloat(distance));
}

// BBS (H2C rack): the change_filament_gcode scalar filament_retract_length_nc (M620.11 O1 T<len>) is
// the OUTGOING filament's per-filament value - BambuStudio GCode.cpp:934 / :8170 read
// filament_retract_length_nc.get_at(old_filament_id). It tells the firmware how far to pull the
// filament back inside the hotend that is about to be parked, while that hotend is still on the
// toolhead (a racked nozzle cannot be heated or retracted). A negative id (no filament loaded yet)
// reads the first entry, as upstream's get_at(size_t(-1)) does. A nil entry falls back to the
// option default (10 mm): upstream resolves a nil filament override to the printer-side value,
// which for this key is its default; the raw nil would land "Tnan" on the wire.
static double outgoing_filament_retract_length_nc(const PrintConfig& config, int old_filament_id)
{
    const ConfigOptionFloatsNullable& opt = config.filament_retract_length_nc;
    const double fallback = static_cast<const ConfigOptionFloatsNullable*>(
                                print_config_def.get("filament_retract_length_nc")->default_value.get())->values.front();
    if (opt.values.empty())
        return fallback;
    const size_t i = (old_filament_id < 0 || size_t(old_filament_id) >= opt.values.size()) ? 0 : size_t(old_filament_id);
    if (opt.is_nil(i) || std::isnan(opt.values[i]))
        return fallback;
    return opt.values[i];
}

// Return true if tch_prefix is found in custom_gcode
static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
{
    bool   ok       = false;
    size_t from_pos = 0;
    size_t pos      = 0;
    while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
        if (pos + 1 == custom_gcode.size())
            break;
        from_pos = pos + 1;
        // only whitespace is allowed before the command
        while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
            if (!std::isspace(custom_gcode[pos]))
                goto NEXT;
        }
        {
            // we should also check that the extruder changes to what was expected
            std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
            unsigned           num = 0;
            if (ss >> num)
                ok = (num == next_extruder);
        }
    NEXT:;
    }
    return ok;
}

std::string OozePrevention::pre_toolchange(GCode& gcodegen)
{
    std::string gcode;

    unsigned int extruder_id        = gcodegen.writer().extruder()->id();
    const auto&  filament_idle_temp = gcodegen.config().idle_temperature;
    if (filament_idle_temp.get_at(extruder_id) == 0) {
        // There is no idle temperature defined in filament settings.
        // Use the delta value from print config.
        if (gcodegen.config().standby_temperature_delta.value != 0) {
            // we assume that heating is always slower than cooling, so no need to block
            gcode += gcodegen.writer().set_temperature(this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false,
                                                       extruder_id);
            gcode.pop_back();
            gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
        }
    } else {
        // Use the value from filament settings. That one is absolute, not delta.
        gcode += gcodegen.writer().set_temperature(filament_idle_temp.get_at(extruder_id), false, extruder_id);
        gcode.pop_back();
        gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
    }

    return gcode;
}

std::string OozePrevention::post_toolchange(GCode& gcodegen)
{
    return (gcodegen.config().standby_temperature_delta.value != 0) ?
               gcodegen.writer().set_temperature(this->_get_temp(gcodegen), gcodegen.config().tool_change_temprature_wait,
                                                 gcodegen.writer().extruder()->id()) :
               std::string();
}

int OozePrevention::_get_temp(const GCode& gcodegen) const
{
    // First layer temperature should be used when on the first layer (obviously) and when
    // "other layers" is set to zero (which means it should not be used).
    return (gcodegen.layer() == nullptr || gcodegen.layer()->id() == 0 ||
            gcodegen.config().nozzle_temperature.get_at(gcodegen.writer().extruder()->id()) == 0) ?
               gcodegen.config().nozzle_temperature_initial_layer.get_at(gcodegen.writer().extruder()->id()) :
               gcodegen.config().nozzle_temperature.get_at(gcodegen.writer().extruder()->id());
}

// Orca:
// Function to calculate the excess retraction length that should be retracted either before or after wiping
// in order for the wipe operation to respect the filament retraction speed
Wipe::RetractionValues Wipe::calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange)
{
    auto& writer      = gcodegen.writer();
    auto& config      = gcodegen.config();
    auto  extruder    = writer.extruder();
    auto  extruder_id = extruder->id();
    auto  last_pos    = gcodegen.last_pos();

    // Declare & initialize retraction lengths
    double retraction_length_remaining = 0, retractionBeforeWipe = 0, retractionDuringWipe = 0;

    // initialise the remaining retraction amount with the full retraction amount.
    retraction_length_remaining = toolchange ? extruder->retract_length_toolchange() : extruder->retraction_length();

    // nothing to retract - return early
    if (retraction_length_remaining <= EPSILON)
        return {0.f, 0.f};

    // calculate retraction before wipe distance from the user setting. Keep adding to this variable any excess retraction needed
    // to be performed before the wipe.
    retractionBeforeWipe = retraction_length_remaining * extruder->retract_before_wipe();
    retraction_length_remaining -= retractionBeforeWipe; // subtract it from the remaining retraction length

    // all of the retraction is to be done before the wipe
    if (retraction_length_remaining <= EPSILON)
        return {retractionBeforeWipe, 0.f};

    // Calculate wipe speed
    double wipe_speed = config.role_based_wipe_speed ? writer.get_current_speed() / 60.0 : config.get_abs_value("wipe_speed");
    wipe_speed        = std::max(wipe_speed, 10.0);

    // Process wipe path & calculate wipe path length
    double   wipe_dist = scale_(config.wipe_distance.get_at(extruder_id));
    Polyline wipe_path = {last_pos};
    wipe_path.append(this->path.points.begin() + 1, this->path.points.end());
    double wipe_path_length = std::min(wipe_path.length(), wipe_dist);

    // Calculate the maximum retraction amount during wipe
    retractionDuringWipe = config.retraction_speed.get_at(extruder_id) * unscale_(wipe_path_length) / wipe_speed;
    // If the maximum retraction amount during wipe is too small, return 0 and retract everything prior to the wipe.
    if (retractionDuringWipe <= EPSILON)
        return {retractionBeforeWipe, 0.f};

    // If the maximum retraction amount during wipe is greater than any remaining retraction length
    // return the remaining retraction length to be retracted during the wipe
    if (retractionDuringWipe - retraction_length_remaining > EPSILON)
        return {retractionBeforeWipe, retraction_length_remaining};

    // We will always proceed with incrementing the retraction amount before wiping with the difference
    // and return the maximum allowed wipe amount to be retracted during the wipe move
    retractionBeforeWipe += retraction_length_remaining - retractionDuringWipe;
    return {retractionBeforeWipe, retractionDuringWipe};
}

// Orca: rebuild the stored wipe path while preserving Polyline's boundary deduplication.
void Wipe::update_path(const ExtrusionPaths &paths, bool reverse)
{
    reset_path();
    for (const ExtrusionPath& extrusion_path : paths)
        path.append(extrusion_path.polyline);
    if (reverse)
        path.reverse();
}

std::string Wipe::wipe(GCode& gcodegen, double length, bool toolchange, bool is_last)
{
    std::string gcode;

    /*  Reduce feedrate a bit; travel speed is often too high to move on existing material.
        Too fast = ripping of existing material; too slow = short wipe path, thus more blob.  */
    double _wipe_speed = gcodegen.config().get_abs_value("wipe_speed"); // gcodegen.writer().config.travel_speed.values.front() * 0.8;
    if (gcodegen.config().role_based_wipe_speed)
        _wipe_speed = gcodegen.writer().get_current_speed() / 60.0;
    if (_wipe_speed < 10)
        _wipe_speed = 10;

    // SoftFever: allow 100% retract before wipe
    if (length >= 0) {
        /*  Calculate how long we need to travel in order to consume the required
            amount of retraction. In other words, how far do we move in XY at wipe_speed
            for the time needed to consume retraction_length at retraction_speed?  */
        // BBS
        double wipe_dist = scale_(gcodegen.config().wipe_distance.get_at(gcodegen.writer().extruder()->id()));

        /*  Take the stored wipe path and replace first point with the current actual position
            (they might be different, for example, in case of loop clipping).  */
        Polyline wipe_path;
        wipe_path.append(gcodegen.last_pos());
        wipe_path.append(this->path.points.begin() + 1, this->path.points.end());

        wipe_path.clip_end(wipe_path.length() - wipe_dist);

        // subdivide the retraction in segments
        if (!wipe_path.empty()) {
            // BBS. Handle short path case.
            if (wipe_path.length() < wipe_dist) {
                wipe_dist = wipe_path.length();
                // BBS: avoid to divide 0
                wipe_dist = wipe_dist < EPSILON ? EPSILON : wipe_dist;
            }

            // add tag for processor
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
            // BBS: don't need to enable cooling makers when this is the last wipe. Because no more cooling layer will clean this "_WIPE"
            // Softfever:
            std::string cooling_mark = "";
            if (gcodegen.enable_cooling_markers() && !is_last)
                cooling_mark = /*gcodegen.config().role_based_wipe_speed ? ";_EXTERNAL_PERIMETER" : */ ";_WIPE";

            gcode += gcodegen.writer().set_speed(_wipe_speed * 60, "", cooling_mark);
            for (const Line& line : wipe_path.lines()) {
                double segment_length = line.length();
                double dE             = length * (segment_length / wipe_dist);
                // BBS: fix this FIXME
                // FIXME one shall not generate the unnecessary G1 Fxxx commands, here wipe_speed is a constant inside this cycle.
                //  Is it here for the cooling markers? Or should it be outside of the cycle?
                // gcode += gcodegen.writer().set_speed(wipe_speed * 60, "", gcodegen.enable_cooling_markers() ? ";_WIPE" : "");
                gcode += gcodegen.writer().extrude_to_xy(gcodegen.point_to_gcode(line.b), -dE, "wipe and retract");
            }
            // add tag for processor
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
            gcodegen.set_last_pos(wipe_path.points.back());
        }

        // prevent wiping again on same path
        this->reset_path();
    }

    return gcode;
}

static inline Point wipe_tower_point_to_object_point(GCode& gcodegen, const Vec2f& wipe_tower_pt)
{
    return Point(scale_(wipe_tower_pt.x() - gcodegen.origin()(0)), scale_(wipe_tower_pt.y() - gcodegen.origin()(1)));
}

std::string WipeTowerIntegration::append_tcr(GCode& gcodegen, const WipeTower::ToolChangeResult& tcr, int new_extruder_id, double z) const
{
    if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
        throw Slic3r::InvalidArgument(Slic3r::format(
            "Error: WipeTowerIntegration::append_tcr unexpected toolchange: layer_idx=%1% tool_change_idx=%2% requested=%3% expected=%4% initial=%5%",
            m_layer_idx, m_tool_change_idx, new_extruder_id, tcr.new_tool, tcr.initial_tool));

    std::string gcode;

    // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
    // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
    float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

    auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
        Vec2f out = Eigen::Rotation2Df(alpha) * pt;
        out += m_wipe_tower_pos;
        return out;
    };

    Vec2f start_pos = tcr.start_pos;
    Vec2f end_pos   = tcr.end_pos;
    if (!tcr.priming) {
        start_pos = transform_wt_pt(start_pos);
        end_pos   = transform_wt_pt(end_pos);
    }

    Vec2f wipe_tower_offset   = tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos;
    float wipe_tower_rotation = tcr.priming ? 0.f : alpha;

    std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

    // BBS: add partplate logic
    Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));

    // BBS: toolchange gcode will move to start_pos,
    // so only perform movement when printing sparse partition to support upper layer.
    // start_pos is the position in plate coordinate.
    if (!tcr.priming && tcr.is_finish_first) {
        // Move over the wipe tower.
        gcode += gcodegen.retract();
        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed,
                                    "Travel to a Wipe Tower");
        gcode += gcodegen.unretract();
    }

    // BBS: if needed, write the gcode_label_objects_end then priming tower, if the retract, didn't did it.
    gcodegen.m_writer.add_object_end_labels(gcode);

    double current_z = gcodegen.writer().get_position().z();
    if (z == -1.) // in case no specific z was provided, print at current_z pos
        z = current_z;
    // Orca: wipe_tower_no_sparse_layers crash guard. With sparse layers skipped the tower is
    // compacted far below the object, so descending to it is only safe once the nozzle is parked
    // over the tower - which is what the is_finish_first travel above does. Otherwise the nozzle
    // is still over the model and this descent would drive it into the print, so defer it to the
    // re-descents below, which run after the travel to the tower.
    // Orca #15441: will_go_down also gates restoring layer Z around change_filament_gcode, so
    // custom toolchange travel cannot sweep through parts taller than the compacted tower.
    const bool will_go_down = !is_approx(z, current_z);
    const bool defer_compacted_descend = m_sparse_layers_skipped
        && !tcr.priming && !tcr.is_finish_first && (current_z - z) > EPSILON;
    if (will_go_down && !defer_compacted_descend) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
        gcode += gcodegen.writer().unretract();
    }
    // Tower compacted below the object, so any extrusion emitted without an explicit z has to be
    // pulled back down to it first.
    const bool compacted_below_object = m_sparse_layers_skipped && z >= 0. && (tcr.print_z - z) > EPSILON;

    // Process the end filament gcode.
    std::string end_filament_gcode_str;
    if (gcodegen.writer().extruder() != nullptr) {
        // Process the custom filament_end_gcode in case of single_extruder_multi_material.
        unsigned int       old_extruder_id    = gcodegen.writer().extruder()->id();
        const std::string& filament_end_gcode = gcodegen.config().filament_end_gcode.get_at(old_extruder_id);
        if (gcodegen.writer().extruder() != nullptr && !filament_end_gcode.empty()) {
            end_filament_gcode_str = gcodegen.placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_extruder_id);
            check_add_eol(end_filament_gcode_str);
        }
    }
    // Ultra (H2C nozzle rack): BambuStudio does NOT count the toolchange here - upstream
    // GCode.cpp:831 keeps this increment commented out. append_tcr also runs for a sparse
    // ("CP EMPTY GRID") wipe-tower layer, whose tcr G-code carries no [change_filament_gcode]
    // placeholder and therefore emits no tool change at all. Counting it there made
    // M620 O<n> skip (1, 2, 52, ...) and padded filament_sequence.json with phantom entries.
    // The count and the sequence entry now happen after the tcr G-code is built, gated on
    // custom_gcode_changes_tool - exactly as upstream GCode.cpp:1253-1257 does.

    // Ultra (H2C rack): the in-extruder nozzle-change block goes into the [filament_end_gcode]
    // slot, before filament_end_gcode and before change_filament_gcode - the same place
    // BambuStudio puts nozzle_change_gcode_trans (GCode.cpp:855-899). Rack machines only
    // (extruder_max_nozzle_count > 1); on P1S/H2D this whole block is skipped and the emitted
    // g-code is unchanged. See docs/superpowers/specs/2026-09-07-h2c-rack-nozzle-change.md.
    // BBS: a Bambu two-extruder printer that pre-heats (H2D, H2D Pro, X2D) gets the same markers around a
    // change onto the other extruder: Bambu Studio writes them from WipeTower::ramming for every
    // extruder change, and the idle-nozzle pre-cooling times the idle hotend from them.
    if ((has_nozzle_rack(gcodegen.m_config) || gcodegen.m_pre_cooling_markers) && new_extruder_id >= 0) {
        auto group_result = gcodegen.m_curr_print ? gcodegen.m_curr_print->get_layered_nozzle_group_result() : nullptr;
        int old_filament_id = gcodegen.writer().extruder() ? (int) gcodegen.writer().extruder()->id() : -1;
        if (group_result && old_filament_id >= 0 && old_filament_id != (int) new_extruder_id) {
            const int old_nozzle_id = group_result->get_nozzle_id(old_filament_id, gcodegen.m_layer_index);
            const int new_nozzle_id = group_result->get_nozzle_id((int) new_extruder_id, gcodegen.m_layer_index);
            if (old_nozzle_id >= 0 && new_nozzle_id >= 0 && old_nozzle_id != new_nozzle_id) {
                const bool extruder_change = group_result->get_extruder_id(old_filament_id, gcodegen.m_layer_index) !=
                                             group_result->get_extruder_id((int) new_extruder_id, gcodegen.m_layer_index);
                end_filament_gcode_str = gcodegen.nozzle_change_gcode(old_filament_id, (int) new_extruder_id, old_nozzle_id,
                                                                      new_nozzle_id, extruder_change) +
                                         end_filament_gcode_str;
            }
        }
    }

    // BBS: should be placed before toolchange parsing
    std::string toolchange_retract_str = gcodegen.retract(true, false);
    check_add_eol(toolchange_retract_str);

    // Orca #15441: the wipe tower can currently be printing below the topmost printed layer
    // (e.g. wipe_tower_no_sparse_layers keeps the tower shorter than the tallest object).
    // change_filament_gcode is free to perform arbitrary XY travel (parking, homing,
    // docking a tool, etc.), so restore Z to the real layer height before running it,
    // otherwise that travel can collide with already-printed parts taller than the tower.
    // toolchange_retract_str above already keeps us retracted for the whole toolchange, so
    // just move Z; deretraction_str (further below) brings Z back down to resume the tower.
    std::string restore_layer_z_str;
    if (will_go_down) {
        restore_layer_z_str += gcodegen.writer().travel_to_z(current_z,
            "Restore layer Z before toolchange to avoid collision with printed parts", true);
        Vec3d position{gcodegen.writer().get_position()};
        position.z() = current_z;
        gcodegen.writer().set_position(position);
        check_add_eol(restore_layer_z_str);
    }

    // Process the custom change_filament_gcode. If it is empty, provide a simple Tn command to change the filament.
    // Otherwise, leave control to the user completely.
    std::string        toolchange_gcode_str;
    const std::string& change_filament_gcode = gcodegen.config().change_filament_gcode.value;
    //        m_max_layer_z = std::max(m_max_layer_z, tcr.print_z);
    if (!change_filament_gcode.empty()) {
        DynamicConfig config;
        int           previous_extruder_id = gcodegen.writer().extruder() ? (int) gcodegen.writer().extruder()->id() : -1;
        config.set_key_value("previous_extruder", new ConfigOptionInt(previous_extruder_id));
        config.set_key_value("next_extruder", new ConfigOptionInt((int) new_extruder_id));
        // Ultra: single-mapped dual-nozzle change_filament vars (filament id == extruder id; hotend -1).
        config.set_key_value("current_filament_id", new ConfigOptionInt(std::max(0, previous_extruder_id)));
        config.set_key_value("next_filament_id", new ConfigOptionInt((int) new_extruder_id));
        config.set_key_value("current_hotend", new ConfigOptionInt(-1));
        config.set_key_value("next_hotend", new ConfigOptionInt(-1));
        // Ultra (H2C rack): port of BambuStudio GCode.cpp:960-966. The real logical-nozzle ids
        // and the per-nozzle-id arrays the H2C change_filament template indexes with them. Falls
        // back to the global single-mapped shim when the plate carries no grouping result.
        auto group_result_tc = (has_nozzle_rack(gcodegen.m_config) && gcodegen.m_curr_print) ?
                                   gcodegen.m_curr_print->get_layered_nozzle_group_result() :
                                   nullptr;
        if (auto group_result = group_result_tc) {
            config.set_key_value("current_nozzle_id", new ConfigOptionInt(std::max(0, group_result->get_nozzle_id(std::max(0, previous_extruder_id), gcodegen.m_layer_index))));
            config.set_key_value("next_nozzle_id", new ConfigOptionInt(std::max(0, group_result->get_nozzle_id((int) new_extruder_id, gcodegen.m_layer_index))));
            config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
            config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
        }
        config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
        config.set_key_value("toolchange_z", new ConfigOptionFloat(z));
        //            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        // BBS
        {
            GCodeWriter&     gcode_writer = gcodegen.m_writer;
            FullPrintConfig& full_config  = gcodegen.m_config;
            float old_retract_length = gcode_writer.extruder() != nullptr ? full_config.retraction_length.get_at(previous_extruder_id) : 0;
            float new_retract_length = full_config.retraction_length.get_at(new_extruder_id);
            float old_retract_length_toolchange = gcode_writer.extruder() != nullptr ?
                                                      full_config.retract_length_toolchange.get_at(previous_extruder_id) :
                                                      0;
            float new_retract_length_toolchange = full_config.retract_length_toolchange.get_at(new_extruder_id);
            int   old_filament_temp             = gcode_writer.extruder() != nullptr ?
                                                      (gcodegen.on_first_layer() ?
                                                           get_value_at(full_config, full_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, previous_extruder_id) :
                                                           get_value_at(full_config, full_config.nozzle_temperature, ConfigFlowDomain::Filament, previous_extruder_id)) :
                                                      210;
            int   new_filament_temp = gcodegen.on_first_layer() ? get_value_at(full_config, full_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, new_extruder_id) :
                                                                  get_value_at(full_config, full_config.nozzle_temperature, ConfigFlowDomain::Filament, new_extruder_id);
            Vec3d nozzle_pos        = gcode_writer.get_position();

            float purge_volume  = tcr.purge_volume < EPSILON ? 0 : std::max(tcr.purge_volume, g_min_purge_volume);
            float filament_area = float((M_PI / 4.f) * pow(full_config.filament_diameter.get_at(new_extruder_id), 2));
            // Ultra (Phase 7): a change between two filaments on DIFFERENT physical nozzles (per the grouping's
            // filament_map) is a nozzle switch, not a color purge — the next filament already sits in its own
            // nozzle, so no flush is needed. Zero the purge for cross-nozzle transitions (over-purge fix).
            {
                const auto& fm = full_config.filament_map.values; // 1-based nozzle per filament; empty on classic machines
                if (previous_extruder_id >= 0 && (size_t)previous_extruder_id < fm.size() && (size_t)new_extruder_id < fm.size()
                    && fm[previous_extruder_id] != fm[new_extruder_id])
                    purge_volume = 0.f;
            }
            float purge_length  = purge_volume / filament_area;

            int old_filament_e_feedrate = gcode_writer.extruder() != nullptr ?
                                              (int) (60.0 * get_value_at(full_config, full_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, previous_extruder_id) /
                                                     filament_area) :
                                              200;
            old_filament_e_feedrate     = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
            int new_filament_e_feedrate = (int) (60.0 * get_value_at(full_config, full_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, new_extruder_id) / filament_area);
            new_filament_e_feedrate     = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;

            config.set_key_value("max_layer_z", new ConfigOptionFloat(gcodegen.m_max_layer_z));
            config.set_key_value("relative_e_axis", new ConfigOptionBool(full_config.use_relative_e_distances));
            config.set_key_value("toolchange_count", new ConfigOptionInt((int) gcodegen.m_toolchange_count + 1));
            // BBS: fan speed is useless placeholer now, but we don't remove it to avoid
            // slicing error in old change_filament_gcode in old 3MF
            config.set_key_value("fan_speed", new ConfigOptionInt((int) 0));
            config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
            config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
            // BBS (H2C rack): BambuStudio GCode.cpp:934/976 - the outgoing filament's hotend-change retraction.
            config.set_key_value("filament_retract_length_nc",
                                 new ConfigOptionFloat(outgoing_filament_retract_length_nc(full_config, previous_extruder_id)));
            config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
            config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
            config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
            config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
            config.set_key_value("x_after_toolchange", new ConfigOptionFloat(start_pos(0)));
            config.set_key_value("y_after_toolchange", new ConfigOptionFloat(start_pos(1)));
            config.set_key_value("z_after_toolchange", new ConfigOptionFloat(nozzle_pos(2)));
            config.set_key_value("first_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
            config.set_key_value("second_flush_volume", new ConfigOptionFloat(purge_length / 2.f));
            config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
            config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
            config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
            config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
            config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
            config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
            config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
            config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));

            config.set_key_value("flush_length", new ConfigOptionFloat(purge_length));

            int   flush_count = std::min(g_max_flush_count, (int) std::round(purge_volume / g_purge_volume_one_time));
            float flush_unit  = purge_length / flush_count;
            int   flush_idx   = 0;
            for (; flush_idx < flush_count; flush_idx++) {
                char key_value[64] = {0};
                snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
            }

            for (; flush_idx < g_max_flush_count; flush_idx++) {
                char key_value[64] = {0};
                snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
                config.set_key_value(key_value, new ConfigOptionFloat(0.f));
            }
        }
        // Ultra: per-filament flush vectors for BBS 2.x change_filament templates (single-nozzle).
        {
            const ConfigBase& cfg = gcodegen.config();
            const auto* ft_opt = cfg.option<ConfigOptionInts>("filament_flush_temp");
            const auto* vs_opt = cfg.option<ConfigOptionFloats>("filament_flush_volumetric_speed");
            const auto* mv_opt = cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed");
            const auto* rh_opt = cfg.option<ConfigOptionInts>("nozzle_temperature_range_high");
            const size_t nf = mv_opt ? mv_opt->size() : 0;
            std::vector<int> fts; std::vector<double> vss;
            for (size_t i = 0; i < nf; ++i) {
                double vs = (vs_opt && i < vs_opt->size()) ? vs_opt->get_at(int(i)) : 0.;
                if (vs == 0.) vs = mv_opt->get_at(int(i));
                vss.push_back(vs);
                int ft = (ft_opt && i < ft_opt->size()) ? ft_opt->get_at(int(i)) : 0;
                if (ft == 0 && rh_opt) ft = rh_opt->get_at(int(i));
                fts.push_back(ft);
            }
            config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(vss));
            config.set_key_value("flush_temperatures", new ConfigOptionInts(fts));
        }
        // BBS: the extruder-change retraction pair must be published BEFORE the template is
        // expanded - change_filament_gcode is what reads it ({if long_retraction_when_ec} ...
        // R{retraction_distance_when_ec}) - and it is keyed by the filament being switched TO.
        set_ec_retraction_placeholders(gcodegen.placeholder_parser(), gcodegen.m_config, size_t(new_extruder_id));
        toolchange_gcode_str = gcodegen.placeholder_parser_process("change_filament_gcode", change_filament_gcode, new_extruder_id, &config);
        check_add_eol(toolchange_gcode_str);

        // retract before toolchange
        toolchange_gcode_str = toolchange_retract_str + toolchange_gcode_str;
        // BBS
        {
            // BBS: current position and fan_speed is unclear after interting change_filament_gcode
            check_add_eol(toolchange_gcode_str);
            toolchange_gcode_str += ";_FORCE_RESUME_FAN_SPEED\n";
            gcodegen.writer().set_current_position_clear(false);
            // BBS: check whether custom gcode changes the z position. Update if changed
            double temp_z_after_tool_change;
            if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_str, temp_z_after_tool_change)) {
                Vec3d pos = gcodegen.writer().get_position();
                pos(2)    = temp_z_after_tool_change;
                gcodegen.writer().set_position(pos);
            }
        }

        // move to start_pos for wiping after toolchange
        std::string start_pos_str;
        start_pos_str = gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed,
                                           "Move to start pos");
        check_add_eol(start_pos_str);
        toolchange_gcode_str += start_pos_str;

        // unretract before wiping
        toolchange_gcode_str += gcodegen.unretract();
        check_add_eol(toolchange_gcode_str);
    }

    // Orca #15441: mirror of restore_layer_z_str above - bring Z back down to the wipe tower layer
    // once change_filament_gcode has finished, so the rest of the tower prints at the right height.
    // The descent is wrapped in retract -> Z -> unretract, exactly like the "Travel down to the last
    // wipe tower layer" move above and like Bambu Studio's own no-sparse descent. That matters for more
    // than oozing: the tower G-code that follows prints its wall without an F word of its own (see
    // WipeTower::tool_change, writer.rectangle(wt_box)), so it inherits whatever feedrate the last move
    // carried. A bare Z move leaves the travel feedrate active and every compacted tower layer, the
    // first one on the bed included, got its wall extruded at travel speed (F60000 on the H2D/H2C).
    // The unretract leaves the deretraction feedrate instead, which is what the wall prints at when
    // the tower is not compacted.
    // With no retraction configured there is nothing to unretract, so the deretraction feedrate is
    // written out explicitly instead.
    auto descend_to_tower = [&gcodegen](double tower_z, const std::string &comment, bool force) {
        GCodeWriter &writer = gcodegen.writer();
        std::string  z_move = writer.travel_to_z(tower_z, comment, force);
        if (z_move.empty())
            return z_move;
        check_add_eol(z_move);
        std::string out = writer.retract();
        const bool  retracted_here = !out.empty();
        out += z_move;
        if (retracted_here)
            out += writer.unretract();
        else if (writer.extruder() != nullptr)
            out += writer.set_speed(60. * writer.extruder()->deretract_speed());
        check_add_eol(out);
        return out;
    };
    std::string deretraction_str;
    if (will_go_down) {
        deretraction_str += descend_to_tower(z, "Restore wipe tower layer Z after toolchange", true);
        Vec3d position{gcodegen.writer().get_position()};
        position.z() = z;
        gcodegen.writer().set_position(position);
    }

    std::string toolchange_command;
    if (tcr.priming || (new_extruder_id >= 0 && gcodegen.writer().need_toolchange(new_extruder_id)))
        toolchange_command = gcodegen.writer().toolchange(new_extruder_id);
    if (new_extruder_id >= 0)
        gcodegen.note_filament_loaded(new_extruder_id);
    if (!custom_gcode_changes_tool(toolchange_gcode_str, gcodegen.writer().toolchange_prefix(), new_extruder_id))
        toolchange_gcode_str += toolchange_command;
    else {
        // We have informed the m_writer about the current extruder_id, we can ignore the generated G-code.
    }

    gcodegen.placeholder_parser().set("current_extruder", new_extruder_id);
    gcodegen.placeholder_parser().set("retraction_distance_when_cut",
                                      gcodegen.m_config.retraction_distances_when_cut.get_at(new_extruder_id));
    gcodegen.placeholder_parser().set("long_retraction_when_cut", gcodegen.m_config.long_retractions_when_cut.get_at(new_extruder_id));
    // (the _ec pair was published above, before change_filament_gcode was expanded)

    // Process the start filament gcode.
    std::string        start_filament_gcode_str;
    const std::string& filament_start_gcode = gcodegen.config().filament_start_gcode.get_at(new_extruder_id);
    if (!filament_start_gcode.empty()) {
        // Process the filament_start_gcode for the active filament only.
        DynamicConfig config;
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(new_extruder_id));
        start_filament_gcode_str = gcodegen.placeholder_parser_process("filament_start_gcode", filament_start_gcode, new_extruder_id,
                                                                       &config);
        check_add_eol(start_filament_gcode_str);
    }

    // Orca: the custom change_filament_gcode lifts to the object layer height and the unretract
    // de-hops back to it, so every tower extrusion emitted after it (purge moves, and the wall
    // when it prints after the toolchange) would float above the compacted tower. Descend first.
    // Same retract -> Z -> unretract wrapping as deretraction_str above, so the tower wall does not
    // inherit the travel feedrate of the Z move. Usually a no-op: deretraction_str already descended.
    if (compacted_below_object)
        start_filament_gcode_str += descend_to_tower(z, "Descend to compacted wipe tower z (no sparse layers)", false);

    // Insert the end filament, toolchange, and start filament gcode into the generated gcode.
    DynamicConfig config;
    config.set_key_value("filament_end_gcode", new ConfigOptionString(end_filament_gcode_str));
    config.set_key_value("restore_layer_z_before_toolchange", new ConfigOptionString(restore_layer_z_str));
    config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
    config.set_key_value("deretraction_from_wipe_tower_generator", new ConfigOptionString(deretraction_str));
    config.set_key_value("filament_start_gcode", new ConfigOptionString(start_filament_gcode_str));
    std::string tcr_gcode,
        tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
    unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
    gcode += tcr_gcode;
    // Ultra (H2C nozzle rack): port of BambuStudio GCode.cpp:1253-1257. Only a tcr whose G-code
    // really contains the tool-change command counts towards toolchange_count / M620 O<n> and
    // towards the filament_sequence.json entry order.
    if (new_extruder_id >= 0 && custom_gcode_changes_tool(tcr_gcode, gcodegen.writer().toolchange_prefix(), new_extruder_id)) {
        gcodegen.m_toolchange_count++;
        gcodegen.record_filament_change(new_extruder_id);
    }
    check_add_eol(toolchange_gcode_str);

    // SoftFever: set new PA for new filament
    if (gcodegen.config().enable_pressure_advance.get_at(new_extruder_id)) {
        gcode += gcodegen.writer().set_pressure_advance(gcodegen.config().pressure_advance.get_at(new_extruder_id));
        // Orca: Adaptive PA
        // Reset Adaptive PA processor last PA value
        gcodegen.m_pa_processor->resetPreviousPA(gcodegen.config().pressure_advance.get_at(new_extruder_id));
    }

    // A phony move to the end position at the wipe tower.
    gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
    gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
    if (!is_approx(z, current_z)) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
        gcode += gcodegen.writer().unretract();
    }

    else {
        // Prepare a future wipe.
        gcodegen.m_wipe.reset_path();
        for (const Vec2f& wipe_pt : tcr.wipe_path)
            gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt)));
    }

    // Let the planner know we are traveling between objects.
    gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
    return gcode;
}

std::string WipeTowerIntegration::append_tcr2(GCode& gcodegen, const WipeTower::ToolChangeResult& tcr, int new_extruder_id, double z) const
{
    if (new_extruder_id != -1 && new_extruder_id != tcr.new_tool)
        throw Slic3r::InvalidArgument(Slic3r::format(
            "Error: WipeTowerIntegration::append_tcr unexpected toolchange: layer_idx=%1% tool_change_idx=%2% requested=%3% expected=%4% initial=%5%",
            m_layer_idx, m_tool_change_idx, new_extruder_id, tcr.new_tool, tcr.initial_tool));

    std::string gcode;

    // Toolchangeresult.gcode assumes the wipe tower corner is at the origin (except for priming lines)
    // We want to rotate and shift all extrusions (gcode postprocessing) and starting and ending position
    float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);

    auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
        Vec2f out = Eigen::Rotation2Df(alpha) * pt;
        out += m_wipe_tower_pos;
        return out;
    };

    Vec2f start_pos = tcr.start_pos;
    Vec2f end_pos   = tcr.end_pos;
    if (!tcr.priming) {
        start_pos = transform_wt_pt(start_pos);
        end_pos   = transform_wt_pt(end_pos);
    }

    Vec2f wipe_tower_offset   = tcr.priming ? Vec2f::Zero() : m_wipe_tower_pos;
    float wipe_tower_rotation = tcr.priming ? 0.f : alpha;
    Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));

    // For Snapmaker Artision
    gcodegen.m_next_wipe_x = 0;
    gcodegen.m_next_wipe_y = 0;
    auto transformed_pos   = Eigen::Rotation2Df(wipe_tower_rotation) * tcr.start_pos + wipe_tower_offset;
    gcodegen.m_next_wipe_x = transformed_pos(0);
    gcodegen.m_next_wipe_y = transformed_pos(1);

    std::string tcr_rotated_gcode = post_process_wipe_tower_moves(tcr, wipe_tower_offset, wipe_tower_rotation);

    gcode += gcodegen.writer().unlift(); // Make sure there is no z-hop (in most cases, there isn't).

    double current_z = gcodegen.writer().get_position().z();

    if (z == -1.) // in case no specific z was provided, print at current_z pos
        z = current_z;

    const bool needs_toolchange = gcodegen.writer().need_toolchange(new_extruder_id);
    const bool will_go_down     = !is_approx(z, current_z);
    const bool is_ramming       = (gcodegen.config().single_extruder_multi_material) ||
                            (!gcodegen.config().single_extruder_multi_material &&
                             gcodegen.config().filament_multitool_ramming.get_at(tcr.initial_tool));
    const bool should_travel_to_tower = !tcr.priming && (tcr.force_travel     // wipe tower says so
                                                         || !needs_toolchange // this is just finishing the tower with no toolchange
                                                         || is_ramming);

    if (should_travel_to_tower || gcodegen.m_need_change_layer_lift_z) {
        // FIXME: It would be better if the wipe tower set the force_travel flag for all toolchanges,
        // then we could simplify the condition and make it more readable.
        auto type = ZHopType(gcodegen.m_config.z_hop_types.get_at(gcodegen.m_writer.extruder()->id()));
        if (type == ZHopType::zhtAuto) {
            type = ZHopType::zhtSpiral;
        }
        auto lift_type = gcodegen.to_lift_type(type);

        if (gcodegen.m_config.z_hop_when_prime.get_at(gcodegen.m_writer.extruder()->id())) {
            gcode += gcodegen.retract(false, false, lift_type);
        }

        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_pos + plate_origin_2d), erMixed,
                                    "Travel to a Wipe Tower");
        gcode += gcodegen.unretract();
    } else {
        // When this is multiextruder printer without any ramming, we can just change
        // the tool without travelling to the tower.
    }

    if (will_go_down) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(z, "Travel down to the last wipe tower layer.");
        gcode += gcodegen.writer().unretract();
    }

    std::string toolchange_gcode_str;
    std::string deretraction_str;
    std::string restore_layer_z_str;
    if (tcr.priming || (new_extruder_id >= 0 && needs_toolchange)) {
        if (is_ramming)
            gcodegen.m_wipe.reset_path();                                           // We don't want wiping on the ramming lines.
        // Orca #15441: the wipe tower can currently be printing below the topmost printed layer
        // (e.g. wipe_tower_no_sparse_layers keeps the tower shorter than the tallest object).
        // change_filament_gcode is free to perform arbitrary XY travel (parking, homing,
        // docking a tool, etc.), so restore Z to the real layer height before running it,
        // otherwise that travel can collide with already-printed parts taller than the tower.
        // deretraction_str (below) brings Z back down to resume printing the wipe tower.
        if (will_go_down && gcodegen.config().enable_prime_tower) {
            restore_layer_z_str += gcodegen.writer().retract();
            restore_layer_z_str += gcodegen.writer().travel_to_z(current_z,
                "Restore layer Z before toolchange to avoid collision with printed parts");
            restore_layer_z_str += gcodegen.writer().unretract();
            check_add_eol(restore_layer_z_str);
        }
        toolchange_gcode_str = gcodegen.set_extruder(new_extruder_id, tcr.print_z); // TODO: toolchange_z vs print_z
        if (gcodegen.config().enable_prime_tower) {
            deretraction_str += gcodegen.writer().travel_to_z(z, "Force restore layer Z", true);
            Vec3d position{gcodegen.writer().get_position()};
            position.z() = z;
            gcodegen.writer().set_position(position);
            deretraction_str += gcodegen.unretract();
        }
    }

    // Insert the toolchange and deretraction gcode into the generated gcode.

    DynamicConfig config;
    config.set_key_value("change_filament_gcode", new ConfigOptionString(toolchange_gcode_str));
    config.set_key_value("deretraction_from_wipe_tower_generator", new ConfigOptionString(deretraction_str));
    config.set_key_value("restore_layer_z_before_toolchange", new ConfigOptionString(restore_layer_z_str));
    config.set_key_value("layer_num", new ConfigOptionInt(gcodegen.m_layer_index));
    config.set_key_value("layer_z", new ConfigOptionFloat(tcr.print_z));
    config.set_key_value("toolchange_z", new ConfigOptionFloat(z));

    std::string tcr_gcode,
        tcr_escaped_gcode = gcodegen.placeholder_parser_process("tcr_rotated_gcode", tcr_rotated_gcode, new_extruder_id, &config);
    unescape_string_cstyle(tcr_escaped_gcode, tcr_gcode);
    gcode += tcr_gcode;
    check_add_eol(toolchange_gcode_str);

    // SoftFever: set new PA for new filament
    if (new_extruder_id != -1 && gcodegen.config().enable_pressure_advance.get_at(new_extruder_id)) {
        gcode += gcodegen.writer().set_pressure_advance(gcodegen.config().pressure_advance.get_at(new_extruder_id));
        // Orca: Adaptive PA
        // Reset Adaptive PA processor last PA value
        gcodegen.m_pa_processor->resetPreviousPA(gcodegen.config().pressure_advance.get_at(new_extruder_id));
    }

    // A phony move to the end position at the wipe tower.
    gcodegen.writer().travel_to_xy((end_pos + plate_origin_2d).cast<double>());
    gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_pos + plate_origin_2d));
    if (!is_approx(z, current_z)) {
        gcode += gcodegen.writer().retract();
        gcode += gcodegen.writer().travel_to_z(current_z, "Travel back up to the topmost object layer.");
        gcode += gcodegen.writer().unretract();
    }

    else {
        // Prepare a future wipe.
        gcodegen.m_wipe.reset_path();
        for (const Vec2f& wipe_pt : tcr.wipe_path)
            gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, transform_wt_pt(wipe_pt)));
    }

    // Let the planner know we are traveling between objects.
    gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
    return gcode;
}

// This function postprocesses gcode_original, rotates and moves all G1 extrusions and returns resulting gcode
// Starting position has to be supplied explicitely (otherwise it would fail in case first G1 command only contained one coordinate)
std::string WipeTowerIntegration::post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr,
                                                                const Vec2f&                       translation,
                                                                float                              angle) const
{
    Vec2f extruder_offset = extruder_offset_at(tcr.initial_tool).cast<float>();

    std::istringstream gcode_str(tcr.gcode);
    std::string        gcode_out;
    std::string        line;
    Vec2f              pos = tcr.start_pos;
    auto  trans_pos = [wt_rot = Eigen::Rotation2Df(angle), &translation](const Vec2f& p) -> Vec2f { return wt_rot * p + translation; };
    Vec2f transformed_pos = trans_pos(pos);
    Vec2f old_pos(-1000.1f, -1000.1f);

    bool isFirstTransform = true;

    while (gcode_str) {
        std::getline(gcode_str, line); // we read the gcode line by line

        // All G1 commands should be translated and rotated. X and Y coords are
        // only pushed to the output when they differ from last time.
        // WT generator can override this by appending the never_skip_tag
        if (line.find("G1 ") == 0 || line.find("G2 ") == 0 || line.find("G3 ") == 0) {
            std::string cur_gcode_start = line.find("G1 ") == 0 ? "G1 " : (line.find("G2 ") == 0 ? "G2 " : "G3 ");
            bool        never_skip      = false;
            auto        it              = line.find(WipeTower::never_skip_tag());
            if (it != std::string::npos) {
                // remove the tag and remember we saw it
                never_skip = true;
                line.erase(it, it + WipeTower::never_skip_tag().size());
            }
            std::ostringstream line_out;
            std::istringstream line_str(line);
            line_str >> std::noskipws; // don't skip whitespace
            char ch = 0;
            while (line_str >> ch) {
                if (ch == 'X' || ch == 'Y')
                    line_str >> (ch == 'X' ? pos.x() : pos.y());
                else
                    line_out << ch;
            }

            transformed_pos = trans_pos(pos);

            if (transformed_pos != old_pos || never_skip) {
                line = line_out.str();
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(3) << cur_gcode_start;
                if (transformed_pos.x() != old_pos.x() || never_skip)
                    oss << " X" << transformed_pos.x() - extruder_offset.x();
                if (transformed_pos.y() != old_pos.y() || never_skip)
                    oss << " Y" << transformed_pos.y() - extruder_offset.y();
                oss << " ";
                line.replace(line.find(cur_gcode_start), 3, oss.str());
                old_pos = transformed_pos;
            }
            else {
                line = line_out.str();
            }
        }

        gcode_out += line + "\n";

        // If this was a toolchange command, we should change current extruder offset
        if (line == "[change_filament_gcode]") {
            // BBS
            if (!m_single_extruder_multi_material) {
                extruder_offset = extruder_offset_at(tcr.new_tool).cast<float>();

                // If the extruder offset changed, add an extra move so everything is continuous
                if (extruder_offset != extruder_offset_at(tcr.initial_tool).cast<float>()) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3) << "G1 X" << transformed_pos.x() - extruder_offset.x() << " Y"
                        << transformed_pos.y() - extruder_offset.y() << "\n";
                    gcode_out += oss.str();
                }
            }
        }
    }
    return gcode_out;
}

Vec2d WipeTowerIntegration::extruder_offset_at(size_t extruder_id) const
{
    if (m_single_extruder_multi_material) {
        return m_extruder_offsets[0];
    } else {
        // If the extruder_id is out of range, we return the offset of the first extruder. 
        if (extruder_id >= m_extruder_offsets.size())
            return m_extruder_offsets[0];
        else
            return m_extruder_offsets[extruder_id];
    }
}

std::string WipeTowerIntegration::prime(GCode& gcodegen)
{
    std::string gcode;
    if (!gcodegen.is_BBL_Printer()) {
        for (const WipeTower::ToolChangeResult& tcr : m_priming) {
            if (!tcr.extrusions.empty())
                gcode += append_tcr2(gcodegen, tcr, tcr.new_tool);
        }
    }
    return gcode;
}

std::string WipeTowerIntegration::tool_change(GCode& gcodegen, int extruder_id, bool finish_layer,
                                              bool local_z_unplanned, double local_z_nominal_layer_z)
{
    std::string gcode;
    auto realign_nominal_toolchange_idx = [&](int requested_extruder_id) {
        if (requested_extruder_id < 0 || m_layer_idx < 0 || m_layer_idx >= (int)m_tool_changes.size())
            return;
        auto &toolchanges = m_tool_changes[m_layer_idx];
        if (toolchanges.empty())
            return;
        if (size_t(m_tool_change_idx) < toolchanges.size() &&
            toolchanges[size_t(m_tool_change_idx)].new_tool == requested_extruder_id) {
            return;
        }

        auto find_match_from = [&](size_t start_idx) {
            return std::find_if(toolchanges.begin() + std::min(start_idx, toolchanges.size()), toolchanges.end(),
                                [requested_extruder_id](const WipeTower::ToolChangeResult &candidate) {
                                    return candidate.new_tool == requested_extruder_id;
                                });
        };

        auto it_match = find_match_from(size_t(std::max(0, m_tool_change_idx)));
        if (it_match == toolchanges.end())
            it_match = find_match_from(0);
        if (it_match == toolchanges.end())
            return;

        const int matched_idx = int(std::distance(toolchanges.begin(), it_match));
        BOOST_LOG_TRIVIAL(warning) << "Wipe tower nominal toolchange state mismatch, realigning"
                                   << " layer_idx=" << m_layer_idx
                                   << " tool_change_idx=" << m_tool_change_idx
                                   << " requested=" << requested_extruder_id
                                   << " matched_idx=" << matched_idx
                                   << " matched_new_tool=" << it_match->new_tool;
        m_tool_change_idx = matched_idx;
    };

    auto emit_local_z_unplanned_toolchange = [&]() -> std::string {
        if (extruder_id < 0 || !gcodegen.writer().need_toolchange(extruder_id))
            return "";
        const double current_z          = gcodegen.writer().get_position().z();
        const double tower_z            = local_z_nominal_layer_z >= 0. ? local_z_nominal_layer_z : current_z;
        const double toolchange_print_z = tower_z - gcodegen.config().z_offset.value;

        if (m_layer_idx >= 0 && size_t(m_layer_idx) < m_local_z_tool_changes.size() &&
            size_t(m_layer_idx) < m_local_z_tool_change_idx.size()) {
            size_t &local_z_tool_change_idx = m_local_z_tool_change_idx[size_t(m_layer_idx)];
            const auto &layer_local_z_tool_changes = m_local_z_tool_changes[size_t(m_layer_idx)];
            if (local_z_tool_change_idx < layer_local_z_tool_changes.size()) {
                const WipeTower::ToolChangeResult &local_z_tcr = layer_local_z_tool_changes[local_z_tool_change_idx];
                const int current_tool = gcodegen.writer().extruder() != nullptr ? int(gcodegen.writer().extruder()->id()) : -1;
                if (local_z_tcr.new_tool == extruder_id &&
                    (current_tool < 0 || local_z_tcr.initial_tool == current_tool)) {
                    ++local_z_tool_change_idx;
                    BOOST_LOG_TRIVIAL(debug) << "Local-Z toolchange emitted from preplanned wipe tower sequence"
                                             << " layer_idx=" << m_layer_idx
                                             << " extruder_id=" << extruder_id
                                             << " local_z_tool_change_idx=" << (local_z_tool_change_idx - 1);
                    return gcodegen.is_BBL_Printer() ? append_tcr(gcodegen, local_z_tcr, extruder_id, tower_z) :
                                                       append_tcr2(gcodegen, local_z_tcr, extruder_id, tower_z);
                }

                BOOST_LOG_TRIVIAL(warning) << "Local-Z preplanned wipe tower sequence mismatch, falling back"
                                           << " layer_idx=" << m_layer_idx
                                           << " requested_extruder=" << extruder_id
                                           << " current_tool=" << current_tool
                                           << " planned_initial=" << local_z_tcr.initial_tool
                                           << " planned_new=" << local_z_tcr.new_tool
                                           << " local_z_tool_change_idx=" << local_z_tool_change_idx;
            }
        }

        if (m_layer_idx < 0 || size_t(m_layer_idx) >= m_local_z_reserve_boxes.size() ||
            size_t(m_layer_idx) >= m_local_z_reserve_slot_idx.size()) {
            BOOST_LOG_TRIVIAL(debug) << "Local-Z unplanned toolchange using direct extruder switch"
                                     << " layer_idx=" << m_layer_idx
                                     << " extruder_id=" << extruder_id
                                     << " tool_change_idx=" << m_tool_change_idx;
            return gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);
        }

        size_t &slot_idx = m_local_z_reserve_slot_idx[size_t(m_layer_idx)];
        const auto &layer_slots = m_local_z_reserve_boxes[size_t(m_layer_idx)];
        if (slot_idx >= layer_slots.size() || layer_slots.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Local-Z toolchange reserve exhausted"
                                       << " layer_idx=" << m_layer_idx
                                       << " extruder_id=" << extruder_id
                                       << " reserved_slots=" << layer_slots.size()
                                       << " consumed_slots=" << slot_idx;
            return gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);
        }

        const WipeTower::box_coordinates &slot = layer_slots[slot_idx++];

        if (m_layer_idx >= (int)m_tool_changes.size() || m_tool_changes[m_layer_idx].empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Local-Z reserve has no matching wipe-tower layer metadata, using direct extruder switch"
                                     << " layer_idx=" << m_layer_idx
                                     << " extruder_id=" << extruder_id;
            return gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);
        }

        const float layer_height = m_tool_changes[m_layer_idx].front().layer_height;
        if (gcodegen.m_curr_print != nullptr && gcodegen.writer().extruder() != nullptr) {
            const Print&       print        = *gcodegen.m_curr_print;
            const PrintConfig& print_config = print.config();
            const size_t       current_tool = gcodegen.writer().extruder()->id();
            std::vector<std::vector<float>> wipe_volumes = WipeTower2::extract_wipe_volumes(print_config);

            WipeTower2 local_z_wipe_tower(print_config,
                                          print.default_region_config(),
                                          print.get_plate_index(),
                                          print.get_plate_origin(),
                                          wipe_volumes,
                                          current_tool);
            for (size_t extruder_idx = 0; extruder_idx < print_config.nozzle_diameter.size(); ++extruder_idx)
                local_z_wipe_tower.set_extruder(extruder_idx, print_config);

            local_z_wipe_tower.set_current_tool(current_tool);
            local_z_wipe_tower.set_layer(float(toolchange_print_z), layer_height, 0, m_layer_idx == 0, false);

            WipeTower::ToolChangeResult local_z_tcr =
                local_z_wipe_tower.local_z_tool_change(size_t(extruder_id), slot, float(print_config.prime_volume));
            BOOST_LOG_TRIVIAL(debug) << "Local-Z toolchange emitted via wipe tower mini-toolchange"
                                     << " layer_idx=" << m_layer_idx
                                     << " extruder_id=" << extruder_id
                                     << " reserve_slot=" << (slot_idx - 1);
            return gcodegen.is_BBL_Printer() ? append_tcr(gcodegen, local_z_tcr, extruder_id, tower_z) :
                                               append_tcr2(gcodegen, local_z_tcr, extruder_id, tower_z);
        }

        const float nozzle_diameter = float(gcodegen.config().nozzle_diameter.get_at(size_t(extruder_id)));
        const float line_width = nozzle_diameter * 1.25f;
        const float extra_flow = float(gcodegen.config().wipe_tower_extra_flow.value) / 100.f;
        const float extra_spacing = float(gcodegen.config().wipe_tower_extra_spacing.value) / 100.f;
        const float filament_area = float((M_PI / 4.f) * std::pow(gcodegen.config().filament_diameter.get_at(size_t(extruder_id)), 2));
        const float flow_per_mm =
            filament_area > 0.f ?
                (layer_height * (line_width - layer_height * float(1.f - M_PI / 4.f)) / filament_area) * std::max(0.f, extra_flow) :
                0.f;
        if (line_width <= 0.f || flow_per_mm <= 0.f)
            return gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);

        const float inset_x = std::min(line_width, std::max(0.f, (slot.ru.x() - slot.ld.x()) * 0.25f));
        const float inset_y = std::min(line_width * 0.5f, std::max(0.f, (slot.ru.y() - slot.rd.y()) * 0.25f));
        const float x_min   = slot.ld.x() + inset_x;
        const float x_max   = slot.rd.x() - inset_x;
        const float y_min   = slot.ld.y() + inset_y;
        const float y_max   = slot.lu.y() - inset_y;
        if (x_max - x_min <= float(EPSILON) || y_max - y_min <= float(EPSILON))
            return gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);

        std::vector<Vec2f> local_path;
        local_path.reserve(16);
        local_path.emplace_back(x_min, y_min);
        local_path.emplace_back(x_max, y_min);

        bool  moving_left = true;
        float y           = y_min;
        const float line_spacing = std::max(line_width, line_width * std::max(1.f, extra_spacing));
        while (y + line_spacing < y_max - float(EPSILON)) {
            y = std::min(y + line_spacing, y_max);
            local_path.emplace_back(moving_left ? x_max : x_min, y);
            local_path.emplace_back(moving_left ? x_min : x_max, y);
            moving_left = !moving_left;
        }

        const float alpha = m_wipe_tower_rotation / 180.f * float(M_PI);
        auto transform_wt_pt = [&alpha, this](const Vec2f& pt) -> Vec2f {
            return Eigen::Rotation2Df(alpha) * pt + m_wipe_tower_pos;
        };
        const Vec2f plate_origin_2d(m_plate_origin(0), m_plate_origin(1));
        const Vec2f start_pos = transform_wt_pt(local_path.front());
        const Vec2f start_machine = start_pos + plate_origin_2d;

        gcodegen.m_next_wipe_x = start_pos.x();
        gcodegen.m_next_wipe_y = start_pos.y();

        gcode += gcodegen.writer().unlift();

        if (gcodegen.writer().extruder() != nullptr) {
            auto type = ZHopType(gcodegen.m_config.z_hop_types.get_at(gcodegen.m_writer.extruder()->id()));
            if (type == ZHopType::zhtAuto)
                type = ZHopType::zhtSpiral;
            if (gcodegen.m_config.z_hop_when_prime.get_at(gcodegen.m_writer.extruder()->id()))
                gcode += gcodegen.retract(false, false, gcodegen.to_lift_type(type));
        }

        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        gcode += gcodegen.travel_to(wipe_tower_point_to_object_point(gcodegen, start_machine), erMixed,
                                    "Travel to Local-Z wipe tower reserve");
        gcode += gcodegen.unretract();

        if (!is_approx(tower_z, current_z)) {
            gcode += gcodegen.writer().retract();
            gcode += gcodegen.writer().travel_to_z(tower_z, "Travel to Local-Z tower layer");
            gcode += gcodegen.writer().unretract();
        }

        gcode += gcodegen.set_extruder(unsigned(extruder_id), toolchange_print_z);
        gcode += gcodegen.writer().travel_to_z(tower_z, "Force restore Local-Z tower Z", true);
        {
            Vec3d restored_position{gcodegen.writer().get_position()};
            restored_position.z() = tower_z;
            gcodegen.writer().set_position(restored_position);
        }
        gcode += gcodegen.unretract();
        gcode += gcodegen.writer().travel_to_xy(start_machine.cast<double>(), "Return to Local-Z wipe tower reserve");

        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + float_to_string_decimal_point(layer_height, 3) + "\n";
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) + ExtrusionEntity::role_to_string(erWipeTower) + "\n";
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + float_to_string_decimal_point(line_width, 3) + "\n";

        double feedrate = std::max(1.0, double(gcodegen.config().wipe_tower_max_purge_speed.value)) * 60.0;
        if (m_layer_idx == 0)
            feedrate = std::min(feedrate,
                                std::max(1.0, double(get_value_at(gcodegen.config(), gcodegen.config().initial_layer_speed,
                                                                  ConfigFlowDomain::Process, unsigned(extruder_id)))) *
                                    60.0);
        gcode += gcodegen.writer().set_speed(feedrate, "Local-Z wipe tower reserve");

        for (size_t point_idx = 1; point_idx < local_path.size(); ++point_idx) {
            const Vec2f machine_pt = transform_wt_pt(local_path[point_idx]) + plate_origin_2d;
            const double length = (local_path[point_idx] - local_path[point_idx - 1]).norm();
            if (length <= EPSILON)
                continue;
            gcode += gcodegen.writer().extrude_to_xy(machine_pt.cast<double>(), flow_per_mm * float(length),
                                                     point_idx == 1 ? "Local-Z tower purge" : std::string());
        }

        const Vec2f end_machine = transform_wt_pt(local_path.back()) + plate_origin_2d;
        gcodegen.set_last_pos(wipe_tower_point_to_object_point(gcodegen, end_machine));
        gcodegen.m_wipe.reset_path();
        for (size_t point_idx = local_path.size(); point_idx-- > 0;) {
            const Vec2f machine_pt = transform_wt_pt(local_path[point_idx]) + plate_origin_2d;
            gcodegen.m_wipe.path.points.emplace_back(wipe_tower_point_to_object_point(gcodegen, machine_pt));
        }

        if (!is_approx(tower_z, current_z)) {
            const Extruder *active_extruder = gcodegen.writer().extruder();
            const bool can_wipe = active_extruder != nullptr &&
                                  gcodegen.config().wipe.get_at(active_extruder->id()) &&
                                  gcodegen.m_wipe.has_path() &&
                                  scale_(gcodegen.config().wipe_distance.get_at(active_extruder->id())) > SCALED_EPSILON;
            if (can_wipe) {
                Wipe::RetractionValues wipe_retractions = gcodegen.m_wipe.calculateWipeRetractionLengths(gcodegen, false);
                gcode += gcodegen.writer().retract(true, wipe_retractions.retractLengthBeforeWipe);
                gcode += gcodegen.m_wipe.wipe(gcodegen, wipe_retractions.retractLengthDuringWipe, false, false);
            }
            gcode += gcodegen.writer().retract();
            gcodegen.m_wipe.reset_path();
            gcode += gcodegen.writer().travel_to_z(current_z, "Travel back to Local-Z pass");
            gcode += gcodegen.writer().unretract();
        }

        gcodegen.m_avoid_crossing_perimeters.use_external_mp_once();
        BOOST_LOG_TRIVIAL(debug) << "Local-Z toolchange emitted on wipe tower reserve"
                                 << " layer_idx=" << m_layer_idx
                                 << " extruder_id=" << extruder_id
                                 << " reserve_slot=" << (slot_idx - 1);
        return gcode;
    };

    if (local_z_unplanned)
        return emit_local_z_unplanned_toolchange();

    assert(m_layer_idx >= 0);
    if (m_layer_idx >= (int) m_tool_changes.size())
        return gcode;
    if (!gcodegen.is_BBL_Printer()) {
        if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
            if (m_layer_idx < (int) m_tool_changes.size()) {
                if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                    throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

                // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
                // resulting in a wipe tower with sparse layers.
                double wipe_tower_z  = -1;
                bool   ignore_sparse = false;
                if (m_sparse_layers_skipped) {
                    wipe_tower_z  = m_last_wipe_tower_print_z;
                    ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]) && m_layer_idx != 0;
                    if (m_tool_change_idx == 0 && !ignore_sparse)
                        wipe_tower_z = m_last_wipe_tower_print_z + m_tool_changes[m_layer_idx].front().layer_height;
                }

                if (!ignore_sparse) {
                    realign_nominal_toolchange_idx(extruder_id);
                    gcode += append_tcr2(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
                    m_last_wipe_tower_print_z = wipe_tower_z;
                }
            }
        }
    } else {
        // Calculate where the wipe tower layer will be printed. -1 means that print z will not change,
        // resulting in a wipe tower with sparse layers.
        double wipe_tower_z  = -1;
        bool   ignore_sparse = false;
        if (m_sparse_layers_skipped) {
            ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]);
            if (size_t(m_layer_idx) < m_compacted_tower_z.size())
                wipe_tower_z = m_compacted_tower_z[m_layer_idx];
        }

        if (m_enable_timelapse_print && m_is_first_print) {
            gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][0], m_tool_changes[m_layer_idx][0].new_tool, wipe_tower_z);
            m_tool_change_idx++;
            m_is_first_print = false;
        }

        if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
            if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
                throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

            if (!ignore_sparse) {
                realign_nominal_toolchange_idx(extruder_id);
                gcode += append_tcr(gcodegen, m_tool_changes[m_layer_idx][m_tool_change_idx++], extruder_id, wipe_tower_z);
            }
        }
    }

    return gcode;
}



bool WipeTowerIntegration::is_empty_wipe_tower_gcode(GCode& gcodegen, int extruder_id, bool finish_layer)
{
    assert(m_layer_idx >= 0);
    if (m_layer_idx >= (int) m_tool_changes.size())
        return true;

    bool ignore_sparse = false;
    if (m_sparse_layers_skipped)
        ignore_sparse = wipe_tower_layer_is_sparse(m_tool_changes[m_layer_idx]);

    if (m_enable_timelapse_print && m_is_first_print) {
        return false;
    }

    if (gcodegen.writer().need_toolchange(extruder_id) || finish_layer) {
        if (!(size_t(m_tool_change_idx) < m_tool_changes[m_layer_idx].size()))
            throw Slic3r::RuntimeError("Wipe tower generation failed, possibly due to empty first layer.");

        if (!ignore_sparse) {
            return false;
        }
    }

    return true;
}

// Print is finished. Now it remains to unload the filament safely with ramming over the wipe tower.
std::string WipeTowerIntegration::finalize(GCode& gcodegen)
{
    std::string gcode;
    if (!gcodegen.is_BBL_Printer()) {
        if (std::abs(gcodegen.writer().get_position().z() - m_final_purge.print_z) > EPSILON)
            gcode += gcodegen.change_layer(m_final_purge.print_z);
        gcode += append_tcr2(gcodegen, m_final_purge, -1);
    }

    return gcode;
}

const std::vector<std::string> ColorPrintColors::Colors = {"#C0392B", "#E67E22", "#F1C40F", "#27AE60", "#1ABC9C", "#2980B9", "#9B59B6"};

#define EXTRUDER_CONFIG(OPT) m_config.OPT.get_at(m_writer.extruder()->id())

void GCode::PlaceholderParserIntegration::reset()
{
    this->failed_templates.clear();
    this->output_config.clear();
    this->opt_position              = nullptr;
    this->opt_zhop                  = nullptr;
    this->opt_e_position            = nullptr;
    this->opt_e_retracted           = nullptr;
    this->opt_e_restart_extra       = nullptr;
    this->opt_extruded_volume       = nullptr;
    this->opt_extruded_weight       = nullptr;
    this->opt_extruded_volume_total = nullptr;
    this->opt_extruded_weight_total = nullptr;
    this->num_extruders             = 0;
    this->position.clear();
    this->e_position.clear();
    this->e_retracted.clear();
    this->e_restart_extra.clear();
}

void GCode::PlaceholderParserIntegration::init(const GCodeWriter& writer)
{
    this->reset();
    const std::vector<Extruder>& extruders = writer.extruders();
    if (!extruders.empty()) {
        this->num_extruders = extruders.back().id() + 1;
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_e_retracted     = new ConfigOptionFloats(e_retracted);
        this->opt_e_restart_extra = new ConfigOptionFloats(e_restart_extra);
        this->output_config.set_key_value("e_retracted", this->opt_e_retracted);
        this->output_config.set_key_value("e_restart_extra", this->opt_e_restart_extra);
        if (!writer.config.use_relative_e_distances) {
            e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            opt_e_position = new ConfigOptionFloats(e_position);
            this->output_config.set_key_value("e_position", opt_e_position);
        }
    }
    this->opt_extruded_volume       = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_weight       = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_volume_total = new ConfigOptionFloat(0.f);
    this->opt_extruded_weight_total = new ConfigOptionFloat(0.f);
    this->parser.set("extruded_volume", this->opt_extruded_volume);
    this->parser.set("extruded_weight", this->opt_extruded_weight);
    this->parser.set("extruded_volume_total", this->opt_extruded_volume_total);
    this->parser.set("extruded_weight_total", this->opt_extruded_weight_total);

    // Reserve buffer for current position.
    this->position.assign(3, 0);
    this->opt_position = new ConfigOptionFloats(this->position);
    this->output_config.set_key_value("position", this->opt_position);
    // Store zhop variable into the parser itself, it is a read-only variable to the script.
    this->opt_zhop = new ConfigOptionFloat(writer.get_zhop());
    this->parser.set("zhop", this->opt_zhop);
}

void GCode::PlaceholderParserIntegration::update_from_gcodewriter(const GCodeWriter& writer)
{
    memcpy(this->position.data(), writer.get_position().data(), sizeof(double) * 3);
    this->opt_position->values = this->position;
    this->opt_zhop->value      = writer.get_zhop();

    if (this->num_extruders > 0) {
        const std::vector<Extruder>& extruders = writer.extruders();
        assert(!extruders.empty() && num_extruders == extruders.back().id() + 1);
        this->e_retracted.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->e_restart_extra.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
        this->opt_extruded_volume->values.assign(num_extruders, 0);
        this->opt_extruded_weight->values.assign(num_extruders, 0);
        double total_volume = 0.;
        double total_weight = 0.;
        for (const Extruder& e : extruders) {
            this->e_retracted[e.id()]                 = e.retracted();
            this->e_restart_extra[e.id()]             = e.restart_extra();
            double v                                  = e.extruded_volume();
            double w                                  = v * e.filament_density() * 0.001;
            this->opt_extruded_volume->values[e.id()] = v;
            this->opt_extruded_weight->values[e.id()] = w;
            total_volume += v;
            total_weight += w;
        }
        opt_extruded_volume_total->value = total_volume;
        opt_extruded_weight_total->value = total_weight;
        opt_e_retracted->values          = this->e_retracted;
        opt_e_restart_extra->values      = this->e_restart_extra;
        if (!writer.config.use_relative_e_distances) {
            this->e_position.assign(MAXIMUM_EXTRUDER_NUMBER, 0);
            for (const Extruder& e : extruders)
                this->e_position[e.id()] = e.position();
            this->opt_e_position->values = this->e_position;
        }
    }
}

// Throw if any of the output vector variables were resized by the script.
void GCode::PlaceholderParserIntegration::validate_output_vector_variables()
{
    if (this->opt_position->values.size() != 3)
        throw Slic3r::RuntimeError("\"position\" output variable must not be resized by the script.");
    if (this->num_extruders > 0) {
        if (this->opt_e_position && this->opt_e_position->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_position\" output variable must not be resized by the script.");
        if (this->opt_e_retracted->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_retracted\" output variable must not be resized by the script.");
        if (this->opt_e_restart_extra->values.size() != MAXIMUM_EXTRUDER_NUMBER)
            throw Slic3r::RuntimeError("\"e_restart_extra\" output variable must not be resized by the script.");
    }
}

// Collect pairs of object_layer + support_layer sorted by print_z.
// object_layer & support_layer are considered to be on the same print_z, if they are not further than EPSILON.
std::vector<GCode::LayerToPrint> GCode::collect_layers_to_print(const PrintObject& object)
{
    std::vector<GCode::LayerToPrint> layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    /*
    // Calculate a minimum support layer height as a minimum over all extruders, but not smaller than 10um.
    // This is the same logic as in support generator.
    //FIXME should we use the printing extruders instead?
    double gap_over_supports = object.config().support_top_z_distance;
    // FIXME should we test object.config().support_material_synchronize_layers ? Currently the support layers are synchronized with object
    layers iff soluble supports. assert(!object.has_support() || gap_over_supports != 0. ||
    object.config().support_material_synchronize_layers); if (gap_over_supports != 0.) { gap_over_supports = std::max(0.,
    gap_over_supports);
        // Not a soluble support,
        double support_layer_height_min = 1000000.;
        for (auto lh : object.print()->config().min_layer_height.values)
            support_layer_height_min = std::min(support_layer_height_min, std::max(0.01, lh));
        gap_over_supports += support_layer_height_min;
    }*/

    std::vector<std::pair<double, double>> warning_ranges;

    // Pair the object layers with the support layers by z.
    size_t              idx_object_layer     = 0;
    size_t              idx_support_layer    = 0;
    const LayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()) {
        LayerToPrint layer_to_print;
        double       print_z_min = std::numeric_limits<double>::max();
        if (idx_object_layer < object.layers().size()) {
            layer_to_print.object_layer = object.layers()[idx_object_layer++];
            print_z_min                 = std::min(print_z_min, layer_to_print.object_layer->print_z);
        }

        if (idx_support_layer < object.support_layers().size()) {
            layer_to_print.support_layer = object.support_layers()[idx_support_layer++];
            print_z_min                  = std::min(print_z_min, layer_to_print.support_layer->print_z);
        }

        if (layer_to_print.object_layer && layer_to_print.object_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.object_layer = nullptr;
            --idx_object_layer;
        }

        if (layer_to_print.support_layer && layer_to_print.support_layer->print_z > print_z_min + EPSILON) {
            layer_to_print.support_layer = nullptr;
            --idx_support_layer;
        }

        layer_to_print.original_object = &object;
        layers_to_print.push_back(layer_to_print);

        bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions()) ||
                              (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions());

        // Check that there are extrusions on the very first layer. The case with empty
        // first layer may result in skirt/brim in the air and maybe other issues.
        if (layers_to_print.size() == 1u) {
            if (!has_extrusions)
                throw Slic3r::SlicingError(
                    _(L("One object has empty initial layer and can't be printed. Please Cut the bottom or enable supports.")),
                    object.id().id);
        }

        // In case there are extrusions on this layer, check there is a layer to lay it on.
        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            // Allow empty support layers, as the support generator may produce no extrusions for non-empty support regions.
            || (layer_to_print.support_layer /* && layer_to_print.support_layer->has_extrusions() */)) {
            double top_cd    = object.config().support_top_z_distance;
            double bottom_cd = object.config().support_bottom_z_distance == 0. ? top_cd : object.config().support_bottom_z_distance;
            // if (!object.print()->config().independent_support_layer_height)
            { // the actual support gap may be larger than the configured one due to rounding to layer height for organic support,
              // regardless of independent support layer height
                top_cd    = std::ceil(top_cd / object.config().layer_height) * object.config().layer_height;
                bottom_cd = std::ceil(bottom_cd / object.config().layer_height) * object.config().layer_height;
            }
            double extra_gap = (layer_to_print.support_layer ? bottom_cd : top_cd);

            // raft contact distance should not trigger any warning
            if (last_extrusion_layer && last_extrusion_layer->support_layer) {
                double raft_gap = object.config().raft_contact_distance.value;
                // if (!object.print()->config().independent_support_layer_height)
                {
                    raft_gap = std::ceil(raft_gap / object.config().layer_height) * object.config().layer_height;
                }
                extra_gap = std::max(extra_gap, object.config().raft_contact_distance.value);
            }
            double maximal_print_z = (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.) + layer_to_print.layer()->height +
                                     std::max(0., extra_gap);
            // Negative support_contact_z is not taken into account, it can result in false positives in cases

            if (has_extrusions && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON)
                warning_ranges.emplace_back(
                    std::make_pair((last_extrusion_layer ? last_extrusion_layer->print_z() : 0.), layers_to_print.back().print_z()));
        }
        // Remember last layer with extrusions.
        if (has_extrusions)
            last_extrusion_layer = &layers_to_print.back();
    }

    if (!warning_ranges.empty()) {
        std::string warning;
        size_t      i = 0;
        for (i = 0; i < std::min(warning_ranges.size(), size_t(5)); ++i)
            warning += Slic3r::format(_(L("Object can't be printed for empty layer between %1% and %2%.")), warning_ranges[i].first,
                                      warning_ranges[i].second) +
                       "\n";
        warning += Slic3r::format(_(L("Object: %1%")), object.model_object()->name) + "\n" +
                   _(L("Maybe parts of the object at these height are too thin, or the object has faulty mesh"));

        const_cast<Print*>(object.print())
            ->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, warning, PrintStateBase::SlicingEmptyGcodeLayers);
    }

    return layers_to_print;
}

// Prepare for non-sequential printing of multiple objects: Support resp. object layers with nearly identical print_z
// will be printed for  all objects at once.
// Return a list of <print_z, per object LayerToPrint> items.
std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> GCode::collect_layers_to_print(const Print& print)
{
    struct OrderingItem
    {
        coordf_t print_z;
        size_t   object_idx;
        size_t   layer_idx;
    };

    std::vector<std::vector<LayerToPrint>> per_object(print.objects().size(), std::vector<LayerToPrint>());
    std::vector<OrderingItem>              ordering;

    std::vector<Slic3r::SlicingError> errors;

    for (size_t i = 0; i < print.objects().size(); ++i) {
        try {
            per_object[i] = collect_layers_to_print(*print.objects()[i]);
        } catch (const Slic3r::SlicingError& e) {
            errors.push_back(e);
            continue;
        }
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const LayerToPrint& front = per_object[i].front();
        for (const LayerToPrint& ltp : per_object[i]) {
            ordering_item.print_z   = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.emplace_back(ordering_item);
        }
    }

    if (!errors.empty()) {
        throw Slic3r::SlicingErrors(errors);
    }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem& oi1, const OrderingItem& oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print;

    // Merge numerically very close Z values.
    for (size_t i = 0; i < ordering.size();) {
        // Find the last layer with roughly the same print_z.
        size_t   j    = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++j)
            ;
        // Merge into layers_to_print.
        std::pair<coordf_t, std::vector<LayerToPrint>> merged;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        merged.first = 0.5 * (ordering[i].print_z + ordering[j - 1].print_z);
        merged.second.assign(print.objects().size(), LayerToPrint());
        for (; i < j; ++i) {
            const OrderingItem& oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.emplace_back(std::move(merged));
    }

    return layers_to_print;
}

// free functions called by GCode::do_export()
namespace DoExport {
//    static void update_print_estimated_times_stats(const GCodeProcessor& processor, PrintStatistics& print_statistics)
//    {
//        const GCodeProcessorResult& result = processor.get_result();
//        print_statistics.estimated_normal_print_time =
//        get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
//        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
//            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";
//    }

static void update_print_estimated_stats(const GCodeProcessor&        processor,
                                         const std::vector<Extruder>& extruders,
                                         PrintStatistics&             print_statistics,
                                         const PrintConfig&           config)
{
    const GCodeProcessorResult& result = processor.get_result();
    double normal_print_time = result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    print_statistics.estimated_normal_print_time = get_time_dhms(normal_print_time);
    print_statistics.estimated_silent_print_time =
        processor.is_stealth_time_estimator_enabled() ?
            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) :
            "N/A";

    // update filament statictics
    double total_extruded_volume = 0.0;
    double total_used_filament   = 0.0;
    double total_weight          = 0.0;
    double total_cost            = 0.0;

    for (auto volume : result.print_statistics.total_volumes_per_extruder) {
        total_extruded_volume += volume.second;

        size_t extruder_id = volume.first;
        auto   extruder    = std::find_if(extruders.begin(), extruders.end(),
                                          [extruder_id](const Extruder& extr) { return extr.id() == extruder_id; });
        if (extruder == extruders.end())
            continue;

        double s      = PI * sqr(0.5 * extruder->filament_diameter());
        double weight = volume.second * extruder->filament_density() * 0.001;
        total_used_filament += volume.second / s;
        total_weight += weight;
        total_cost += weight * extruder->filament_cost() * 0.001;
    }

    total_cost += config.time_cost.getFloat() * (normal_print_time / 3600.0);

    print_statistics.total_extruded_volume = total_extruded_volume;
    print_statistics.total_used_filament   = total_used_filament;
    print_statistics.total_weight          = total_weight;
    print_statistics.total_cost            = total_cost;

    print_statistics.filament_stats = result.print_statistics.model_volumes_per_extruder;
}

// if any reserved keyword is found, returns a std::vector containing the first MAX_COUNT keywords found
// into pairs containing:
// first: source
// second: keyword
// to be shown in the warning notification
// The returned vector is empty if no keyword has been found
static std::vector<std::pair<std::string, std::string>> validate_custom_gcode(const Print& print)
{
    static const unsigned int                        MAX_TAGS_COUNT = 5;
    std::vector<std::pair<std::string, std::string>> ret;

    auto check = [&ret, is_bbl_printer = print.is_BBL_printer()](const std::string& source, const std::string& gcode) {
        std::vector<std::string> tags;
        if (GCodeProcessor::contains_reserved_tags(gcode, MAX_TAGS_COUNT, tags, is_bbl_printer)) {
            if (!tags.empty()) {
                size_t i = 0;
                while (ret.size() < MAX_TAGS_COUNT && i < tags.size()) {
                    ret.push_back({source, tags[i]});
                    ++i;
                }
            }
        }
    };

    const GCodeConfig& config = print.config();
    check(_(L("Machine start G-code")), config.machine_start_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Machine end G-code")), config.machine_end_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Before layer change G-code")), config.before_layer_change_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Layer change G-code")), config.layer_change_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Timelapse G-code")), config.time_lapse_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Change filament G-code")), config.change_filament_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Printing by object G-code")), config.printing_by_object_gcode.value);
    // if (ret.size() < MAX_TAGS_COUNT) check(_(L("Color Change G-code")), config.color_change_gcode.value);
    // Orca
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Change extrusion role G-code")), config.change_extrusion_role_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Pause G-code")), config.machine_pause_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT)
        check(_(L("Template Custom G-code")), config.template_custom_gcode.value);
    if (ret.size() < MAX_TAGS_COUNT) {
        for (const std::string& value : config.filament_start_gcode.values) {
            check(_(L("Filament start G-code")), value);
            if (ret.size() == MAX_TAGS_COUNT)
                break;
        }
    }
    if (ret.size() < MAX_TAGS_COUNT) {
        for (const std::string& value : config.filament_end_gcode.values) {
            check(_(L("Filament end G-code")), value);
            if (ret.size() == MAX_TAGS_COUNT)
                break;
        }
    }
    // BBS: no custom_gcode_per_print_z, don't need to check
    // if (ret.size() < MAX_TAGS_COUNT) {
    //     const CustomGCode::Info& custom_gcode_per_print_z = print.model().custom_gcode_per_print_z;
    //     for (const auto& gcode : custom_gcode_per_print_z.gcodes) {
    //         check(_(L("Custom G-code")), gcode.extra);
    //         if (ret.size() == MAX_TAGS_COUNT)
    //             break;
    //     }
    // }

    return ret;
}
} // namespace DoExport

bool GCode::is_BBL_Printer()
{
    if (m_curr_print)
        return m_curr_print->is_BBL_printer();
    return false;
}

void GCode::do_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_CLEAR();

    // BBS
    m_curr_print = print;

    GCodeWriter::full_gcode_comment = print->config().gcode_comments;
    CNumericLocalesSetter locales_setter;

    // Does the file exist? If so, we hope that it is still valid.
    if (print->is_step_done(psGCodeExport) && boost::filesystem::exists(boost::filesystem::path(path)))
        return;

    BOOST_LOG_TRIVIAL(info) << boost::format("Will export G-code to %1% soon") % path;
    GCodeProcessor::s_IsBBLPrinter = print->is_BBL_printer();
    print->set_started(psGCodeExport);

    // check if any custom gcode contains keywords used by the gcode processor to
    // produce time estimation and gcode toolpaths
    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        // print->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
        //     _(L("In the custom G-code were found reserved keywords:")) + "\n" +
        //     reports +
        //     _(L("This may cause problems in g-code visualization and printing time estimation.")));
        std::string temp = "Dangerous keywords in custom Gcode: " + reports +
                           "\nThis may cause problems in g-code visualization and printing time estimation.";
        BOOST_LOG_TRIVIAL(warning) << temp;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    fs::path file_path(path);
    fs::path folder = file_path.parent_path();
    if (!fs::exists(folder)) {
        fs::create_directory(folder);
        BOOST_LOG_TRIVIAL(error) << "[WARNING]: the parent path " + folder.string() + " is not there, create it!" << std::endl;
    }

    std::string path_tmp(path);
    path_tmp += ".tmp";

    m_processor.initialize(path_tmp);
    m_processor.set_print(print);
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (!file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n" << std::endl;
        if (!fs::exists(folder)) {
            // fs::create_directory(folder);
            BOOST_LOG_TRIVIAL(error) << "the parent path " + folder.string() + " is not there!!!" << std::endl;
        }
        throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");
    }

    try {
        this->_do_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception& /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    file.close();

    check_placeholder_parser_failed();

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (!m_placeholder_error_messages.empty()) {
        std::ostringstream message;
        message << "Some EditGcodeDialog defs were not specified properly. Do so in PrintConfig under SlicingStatesConfigDef:" << std::endl;
        for (const auto& error : m_placeholder_error_messages) {
            message << std::endl << error.first << ": " << std::endl;
            for (const auto& str : error.second)
                message << str << ", ";
            message.seekp(-2, std::ios_base::end);
            message << std::endl;
        }
        throw Slic3r::PlaceholderParserError(message.str());
    }
#endif

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();
    // Post-process the G-code to update time stamps.

    m_timelapse_warning_code = 0;
    if (m_config.printer_structure.value == PrinterStructure::psI3 && m_spiral_vase) {
        m_timelapse_warning_code += 1;
    }
    if (m_config.printer_structure.value == PrinterStructure::psI3 && print->config().print_sequence == PrintSequence::ByObject) {
        m_timelapse_warning_code += (1 << 1);
    }
    m_processor.result().timelapse_warning_code        = m_timelapse_warning_code;
    m_processor.result().support_traditional_timelapse = m_support_traditional_timelapse;

    bool activate_long_retraction_when_cut = false;
    for (const auto& extruder : m_writer.extruders())
        activate_long_retraction_when_cut |= (m_config.long_retractions_when_cut.get_at(extruder.id()) &&
                                              m_config.retraction_distances_when_cut.get_at(extruder.id()) > 0);

    m_processor.result().long_retraction_when_cut = activate_long_retraction_when_cut;
    // Ultra (H2C 3MF schema): hand the filament / nozzle entry order to the 3MF writer.
    m_processor.result().filament_change_sequence = m_filament_change_sequence;
    m_processor.result().nozzle_change_sequence   = m_nozzle_change_sequence;

    // Ultra (H2C nozzle rack): the per-filament physical group assignment that goes into
    // Metadata/filament_sequence.json as "optimal_assignment". Ported from BambuStudio
    // GCode.cpp:3482-3517; MultiNozzleUtils::find_optimal_physical_assignment is already in this
    // fork verbatim. Empty on machines with no grouping result (every single-nozzle printer), so
    // their output is unchanged.
    {
        std::vector<int> optimal_assignment;
        std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> group_result;
        if (m_curr_print)
            group_result = m_curr_print->get_layered_nozzle_group_result();
        if (group_result) {
            auto to_int = [](const std::vector<unsigned int>& in) {
                std::vector<int> out;
                out.reserve(in.size());
                for (unsigned int v : in) out.push_back(int(v));
                return out;
            };
            std::vector<int> logical_filaments;
            for (unsigned int f : group_result->get_used_filaments())
                logical_filaments.push_back(int(f));
            auto nozzle_list = group_result->get_used_nozzles_in_extruder();
            int  group_count = group_result->get_extruder_count();

            std::vector<int> filament_seq = to_int(m_filament_change_sequence);
            std::vector<int> nozzle_seq   = to_int(m_nozzle_change_sequence);

            double load_time   = m_config.machine_load_filament_time.value;
            double unload_time = m_config.machine_unload_filament_time.value;
            MultiNozzleUtils::FilamentChangeTimeParams time_params;
            time_params.selector_load_time   = static_cast<float>(load_time / 2);
            time_params.selector_unload_time = static_cast<float>(unload_time / 2);
            time_params.standard_load_time   = static_cast<float>(load_time);
            time_params.standard_unload_time = static_cast<float>(unload_time);

            bool can_compute = !logical_filaments.empty() && !nozzle_list.empty() && !filament_seq.empty() &&
                               !nozzle_seq.empty() && group_count > 0;

            std::vector<int> final_assignment(m_config.filament_map.values.size(), 0);
            if (can_compute) {
                auto used_assignment = MultiNozzleUtils::find_optimal_physical_assignment(
                    logical_filaments, nozzle_list, filament_seq, nozzle_seq, group_count, time_params);
                for (size_t idx = 0; idx < logical_filaments.size() && idx < used_assignment.size(); ++idx) {
                    int filament_id = logical_filaments[idx];
                    if (filament_id >= 0 && static_cast<size_t>(filament_id) < final_assignment.size())
                        final_assignment[filament_id] = used_assignment[idx];
                }
            }
            optimal_assignment = std::move(final_assignment);
        }
        m_processor.result().optimal_assignment = std::move(optimal_assignment);
    }

    { // BBS:check bed and filament compatible
        const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
        assert(bed_type_def != nullptr);
        const t_config_enum_values* bed_type_keys_map = bed_type_def->enum_keys_map;
        const ConfigOptionInts*     bed_temp_opt      = m_config.option<ConfigOptionInts>(get_bed_temp_key(m_config.curr_bed_type));
        for (auto extruder_id : m_initial_layer_extruders) {
            int cur_bed_temp = bed_temp_opt->get_at(extruder_id);
            if (cur_bed_temp == 0 && bed_type_keys_map != nullptr) {
                for (auto item : *bed_type_keys_map) {
                    if (item.second == m_config.curr_bed_type) {
                        m_processor.result().bed_match_result = BedMatchResult(false, item.first, extruder_id);
                        break;
                    }
                }
            }
            if (m_processor.result().bed_match_result.match == false)
                break;
        }
    }

    m_processor.finalize(true);
    //    DoExport::update_print_estimated_times_stats(m_processor, print->m_print_statistics);
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->m_print_statistics, print->config());
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        // set the filename to the correct value
        result->filename = path;
    }

    // BBS: add some log for error output
    BOOST_LOG_TRIVIAL(debug) << boost::format("Finished processing gcode to %1% ") % path_tmp;

    std::error_code ret = rename_file(path_tmp, path);
    if (ret) {
        throw Slic3r::RuntimeError(std::string("Failed to rename the output G-code file from ") + path_tmp + " to " + path + '\n' +
                                   "error code " + ret.message() + '\n' + "Is " + path_tmp + " locked?" + '\n');
    } else {
        BOOST_LOG_TRIVIAL(info) << boost::format("rename_file from %1% to %2% successfully") % path_tmp % path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    print->set_done(psGCodeExport);

    // `result` is optional (see the `result != nullptr` guard above); null-check it
    // here as well instead of dereferencing it on the is_BBL_Printer() path.
    if (is_BBL_Printer() && result != nullptr)
        result->label_object_enabled = m_enable_exclude_object;

    // Write the profiler measurements to file
    PROFILE_UPDATE();
    PROFILE_OUTPUT(debug_out_path("gcode-export-profile.txt").c_str());
}

// free functions called by GCode::_do_export()
namespace DoExport {
static void init_gcode_processor(const PrintConfig& config, GCodeProcessor& processor, bool& silent_time_estimator_enabled)
{
    silent_time_estimator_enabled = (config.gcode_flavor == gcfMarlinLegacy || config.gcode_flavor == gcfMarlinFirmware) &&
                                    config.silent_mode;
    processor.reset();
    processor.apply_config(config);
    processor.enable_stealth_time_estimator(silent_time_estimator_enabled);
}

#if 0
	static double autospeed_volumetric_limit(const Print &print)
	{
	    // get the minimum cross-section used in the print
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
	            const PrintRegion &region = object->printing_region(region_id);
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
	                if (region.config().get_abs_value("inner_wall_speed") == 0 ||
                        // BBS: remove small small_perimeter_speed config, and will absolutely
                        // remove related code if no other issue in the coming release.
	                    //region.config().get_abs_value("small_perimeter_speed") == 0 ||
	                    region.config().outer_wall_speed.values.front() == 0 ||
	                    region.config().get_abs_value("bridge_speed") == 0)
	                    mm3_per_mm.push_back(layerm->perimeters.min_mm3_per_mm());
	                if (region.config().get_abs_value("sparse_infill_speed") == 0 ||
	                    region.config().get_abs_value("internal_solid_infill_speed") == 0 ||
	                    region.config().get_abs_value("top_surface_speed") == 0 ||
                        region.config().get_abs_value("bridge_speed") == 0)
                    {
                        // Minimal volumetric flow should not be calculated over ironing extrusions.
                        // Use following lambda instead of the built-it method.
                        auto min_mm3_per_mm_no_ironing = [](const ExtrusionEntityCollection& eec) -> double {
                            double min = std::numeric_limits<double>::max();
                            for (const ExtrusionEntity* ee : eec.entities)
                                if (ee->role() != erIroning)
                                    min = std::min(min, ee->min_mm3_per_mm());
                            return min;
                        };

                        mm3_per_mm.push_back(min_mm3_per_mm_no_ironing(layerm->fills));
                    }
	            }
	        }
	        if (object->config().get_abs_value("support_speed") == 0 ||
	            object->config().get_abs_value("support_interface_speed") == 0)
	            for (auto layer : object->support_layers())
	                mm3_per_mm.push_back(layer->support_fills.min_mm3_per_mm());
	    }
	    // filter out 0-width segments
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        // In order to honor max_print_speed we need to find a target volumetric
	        // speed that we can use throughout the print. So we define this target
	        // volumetric speed as the volumetric speed produced by printing the
	        // smallest cross-section at the maximum speed: any larger cross-section
	        // will need slower feedrates.
	        volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * print.config().max_print_speed.value;
	        // limit such volumetric speed with max_volumetric_speed if set
            //BBS
	        //if (print.config().max_volumetric_speed.value > 0)
	        //    volumetric_speed = std::min(volumetric_speed, print.config().max_volumetric_speed.value);
	    }
	    return volumetric_speed;
	}
#endif

static void init_ooze_prevention(const Print& print, OozePrevention& ooze_prevention)
{
    ooze_prevention.enable = print.config().ooze_prevention.value && !print.config().single_extruder_multi_material;
}

// Fill in print_statistics and return formatted string containing filament statistics to be inserted into G-code comment section.
static std::string update_print_stats_and_format_filament_stats(const bool                   has_wipe_tower,
                                                                const WipeTowerData&         wipe_tower_data,
                                                                const std::vector<Extruder>& extruders,
                                                                PrintStatistics&             print_statistics)
{
    std::string filament_stats_string_out;

    print_statistics.clear();
    print_statistics.total_toolchanges = std::max(0, wipe_tower_data.number_of_toolchanges);
    if (!extruders.empty()) {
        std::pair<std::string, unsigned int> out_filament_used_mm("; filament used [mm] = ", 0);
        std::pair<std::string, unsigned int> out_filament_used_cm3("; filament used [cm3] = ", 0);
        std::pair<std::string, unsigned int> out_filament_used_g("; filament used [g] = ", 0);
        std::pair<std::string, unsigned int> out_filament_cost("; filament cost = ", 0);
        for (const Extruder& extruder : extruders) {
            double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] : 0.f);
            double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament[extruder.id()] * 2.4052f :
                                                                                    0.f); // assumes 1.75mm filament diameter
            double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
            double filament_cost   = filament_weight * extruder.filament_cost() * 0.001;
            auto   append          = [&extruder](std::pair<std::string, unsigned int>& dst, const char* tmpl, double value) {
                assert(is_decimal_separator_point());
                while (dst.second < extruder.id()) {
                    // Fill in the non-printing extruders with zeros.
                    dst.first += (dst.second > 0) ? ", 0" : "0";
                    ++dst.second;
                }
                if (dst.second > 0)
                    dst.first += ", ";
                char buf[64];
                sprintf(buf, tmpl, value);
                dst.first += buf;
                ++dst.second;
            };
            append(out_filament_used_mm, "%.2lf", used_filament);
            append(out_filament_used_cm3, "%.2lf", extruded_volume * 0.001);
            if (filament_weight > 0.) {
                print_statistics.total_weight = print_statistics.total_weight + filament_weight;
                append(out_filament_used_g, "%.2lf", filament_weight);
                if (filament_cost > 0.) {
                    print_statistics.total_cost = print_statistics.total_cost + filament_cost;
                    append(out_filament_cost, "%.2lf", filament_cost);
                }
            }
            print_statistics.total_used_filament += used_filament;
            print_statistics.total_extruded_volume += extruded_volume;
            print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
            print_statistics.total_wipe_tower_cost += has_wipe_tower ?
                                                          (extruded_volume - extruder.extruded_volume()) * extruder.filament_density() *
                                                              0.001 * extruder.filament_cost() * 0.001 :
                                                          0.;
        }
        filament_stats_string_out += out_filament_used_mm.first;
        filament_stats_string_out += "\n" + out_filament_used_cm3.first;
        if (out_filament_used_g.second)
            filament_stats_string_out += "\n" + out_filament_used_g.first;
        if (out_filament_cost.second)
            filament_stats_string_out += "\n" + out_filament_cost.first;
        filament_stats_string_out += "\n";
    }
    return filament_stats_string_out;
}
} // namespace DoExport

#if 0
// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print &print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject *po1, const PrintObject *po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject *object : objects)
        for (size_t i = 0; i < object->instances().size(); ++ i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}
#endif

// Produce a vector of PrintObjects in the order of their respective ModelObjects in print.model().
// BBS: add sort logic for seq-print
std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order)
{
    auto find_object_index = [](const Model& model, const ModelObject* obj) {
        for (int index = 0; index < model.objects.size(); index++) {
            if (model.objects[index] == obj)
                return index;
        }
        return -1;
    };

    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject* print_object : print.objects())
        for (const PrintInstance& print_instance : print_object->instances()) {
            if (init_order)
                const_cast<ModelInstance*>(print_instance.model_instance)->arrange_order = find_object_index(print.model(),
                                                                                                             print_object->model_object());
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
        }
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(),
              [](auto& l, auto& r) { return l.first->arrange_order < r.first->arrange_order; });
    if (init_order) {
        // Re-assign the arrange_order so each instance has a unique order number
        for (int k = 0; k < model_instance_to_print_instance.size(); k++) {
            const_cast<ModelInstance*>(model_instance_to_print_instance[k].first)->arrange_order = k + 1;
        }
    }

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject* model_object : print.model().objects)
        for (const ModelInstance* model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(),
                                       std::make_pair(model_instance, nullptr),
                                       [](auto& l, auto& r) { return l.first->arrange_order < r.first->arrange_order; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    std::sort(instances.begin(), instances.end(),
              [](auto& l, auto& r) { return l->model_instance->arrange_order < r->model_instance->arrange_order; });
    return instances;
}

enum BambuBedType {
    bbtUnknown              = 0,
    bbtCoolPlate            = 1,
    bbtEngineeringPlate     = 2,
    bbtHighTemperaturePlate = 3,
    bbtTexturedPEIPlate     = 4,
    bbtSuperTackPlate       = 5,
};

static BambuBedType to_bambu_bed_type(BedType type)
{
    BambuBedType bambu_bed_type = bbtUnknown;
    if (type == btPC)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btEP)
        bambu_bed_type = bbtEngineeringPlate;
    else if (type == btPEI)
        bambu_bed_type = bbtHighTemperaturePlate;
    else if (type == btPTE)
        bambu_bed_type = bbtTexturedPEIPlate;
    else if (type == btPCT)
        bambu_bed_type = bbtCoolPlate;
    else if (type == btSuperTack)
        bambu_bed_type = bbtSuperTackPlate;

    return bambu_bed_type;
}

void GCode::_do_export(Print& print, GCodeOutputStream& file, ThumbnailsGeneratorCallback thumbnail_cb)
{
    PROFILE_FUNC();

    // Snapmaker (feedrate guard): make a refused feedrate VISIBLE. The writer cannot reach Print,
    // so install a sink here that turns a guard trip into a real slicing warning naming the
    // config key that resolved badly. Without this the substitution was a log line only, and the
    // user got a file that quietly printed at the wrong speed with nothing to explain it.
    //
    // CRITICAL level, but note that WarningLevel is NOT what drives visibility here: the GUI picks
    // its notification level from message_type (Plater.cpp), and the CLI's own filter drops
    // anything left on SlicingDefaultNotification. That is why this warning has its own
    // SlicingInvalidPrintSpeed id, why Plater.cpp maps that id to SeriousWarningNotificationLevel
    // (a bold, non-fading notification), and why Snapmaker_Orca.cpp exits non-zero on it. The
    // CRITICAL level is still correct for any consumer that does read it.
    //
    // Reported once per offending setting - a setting that trips on thousands of paths yields one
    // warning, not thousands.
    m_writer.set_feedrate_guard_reporter(
        [&print, reported = std::make_shared<std::set<std::string>>()](const FeedrateOrigin &origin, double bad_value,
                                                                      double substituted) {
            const std::string setting = origin.setting != nullptr ? origin.setting : "unknown print speed setting";
            if (!reported->insert(setting).second)
                return;
            // mm/min back to mm/s for a message the user can compare against the UI, which shows
            // every one of these settings in mm/s.
            std::string message = Slic3r::format(_(L("Print speed setting \"%1%\" resolved to %2% (invalid); "
                                                     "printing at %3% mm/s instead.")),
                                                 setting, bad_value,
                                                 std::round(substituted / 60. * 10.) / 10.);
            if (origin.role_name != nullptr && *origin.role_name != '\0')
                message += " " + Slic3r::format(_(L("Affected feature: %1%.")), _(origin.role_name));
            if (origin.object_name != nullptr && *origin.object_name != '\0')
                message += " " + Slic3r::format(_(L("Object: %1%.")), origin.object_name);
            if (origin.layer_id >= 0)
                message += " " + Slic3r::format(_(L("Layer: %1%.")), origin.layer_id);
            message += "\n" + _(L("This setting should never be zero. Check it in Process settings - the printed "
                                  "result will not match the profile."));
            BOOST_LOG_TRIVIAL(error) << "feedrate guard: " << message;
            print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL, message,
                                          PrintStateBase::SlicingInvalidPrintSpeed);
        });

    // modifies m_silent_time_estimator_enabled
    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled);
    const bool is_bbl_printers = print.is_BBL_printer();
    m_calib_config.clear();
    // Orca: Calibration overrides are reapplied after object/region settings in _extrude().
    // Keep inward wiping from masking retraction and pressure advance artifacts.
    switch (print.calib_mode()) {
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
    case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_Retraction_tower:
        m_calib_config.set_key_value("wipe_inward", new ConfigOptionBool(false));
        break;
    default:
        break;
    }
    // resets analyzer's tracking data
    m_last_height  = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z  = 0.f;
    m_last_width   = 0.f;
    m_is_role_based_fan_on.fill(false);
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm = 0.;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    m_fan_mover.release();
    m_layer_time_speed_smoothing.reset();

    m_writer.set_is_bbl_machine(is_bbl_printers);

    // Bambu two-extruder printers, print by object: the filament -> nozzle grouping
    // (ToolOrdering::reorder_extruders_for_minimum_flush_volume) never runs for such a plate - it is
    // skipped for a sequential print, and Print::process builds no print-wide ToolOrdering for a
    // by-object plate even with one object - so nothing told the printer which hotend each filament
    // uses: the CONFIG_BLOCK kept the
    // project's stale filament_nozzle_map (an H2D plate with filament_map 2,1,2,2,... went out as
    // "1,0,0,0,..." - filament 5 on nozzle 0, the LEFT hotend, while its AMS feeds the right one),
    // slice_info gave every filament group 0 with no <nozzle> table, and the start G-code's
    // filament_map shim heated and selected the left hotend ("M104 T1 ; rise temp in advance",
    // "G151 P1 M"). Bambu Studio groups print-by-object plates too; group this one from the plate's
    // own map, before the header is written. A by-layer plate keeps the grouping ToolOrdering made.
    // (A by-object plate with ONE object and a prime tower does get the print-wide ToolOrdering, and
    // its grouping, from Print::_make_wipe_tower; keep that one.)
    if (is_bbl_printers && print.config().print_sequence == PrintSequence::ByObject && print.config().nozzle_diameter.size() >= 2 &&
        (print.objects().size() > 1 || !print.has_wipe_tower())) {
        std::vector<std::vector<unsigned int>> layer_filaments;
        for (const PrintObject *object : print.objects()) {
            ToolOrdering object_ordering(*object, (unsigned int) -1);
            for (const LayerTools &lt : object_ordering.layer_tools())
                if (!lt.extruders.empty())
                    layer_filaments.push_back(lt.extruders);
        }
        std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> grouping;
        if (!layer_filaments.empty()) {
            try {
                grouping = std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(ToolOrdering::group_by_plate_map(&print, layer_filaments));
                if (grouping->get_used_filaments().empty())
                    grouping.reset();
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "print-by-object nozzle grouping failed: " << e.what();
                grouping.reset();
            }
        }
        // Always replace: a result from an earlier by-layer slice of this Print must not describe this one.
        print.set_nozzle_group_result(grouping);
    }

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    if (print.config().print_sequence == PrintSequence::ByObject) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            zs.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            // BBS: merge numerically very close Z values.
            auto         end_it           = std::unique(zs.begin(), zs.end());
            unsigned int temp_layer_count = (unsigned int) (end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    temp_layer_count--;
            }
            m_layer_count += (unsigned int) (object->instances().size() * temp_layer_count);
        }
    } else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.push_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.push_back(layer->print_z);
        }
        if (!zs.empty()) {
            std::sort(zs.begin(), zs.end());
            // BBS: merge numerically very close Z values.
            auto end_it   = std::unique(zs.begin(), zs.end());
            m_layer_count = (unsigned int) (end_it - zs.begin());
            for (auto it = zs.begin(); it != end_it - 1; it++) {
                if (abs(*it - *(it + 1)) < EPSILON)
                    m_layer_count--;
            }
        }
    }
    print.throw_if_canceled();

    m_enable_cooling_markers = true;
    this->apply_print_config(print.config());

    // m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
    print.throw_if_canceled();

    if (print.config().spiral_mode.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());

    if (print.config().max_volumetric_extrusion_rate_slope.values.front() > 0) {
        m_pressure_equalizer            = make_unique<PressureEqualizer>(print.config());
        m_enable_extrusion_role_markers = (bool) m_pressure_equalizer;
    } else
        m_enable_extrusion_role_markers = false;

    if (m_config.small_area_infill_flow_compensation.value && !m_config.small_area_infill_flow_compensation_model.empty())
        m_small_area_infill_flow_compensator = make_unique<SmallAreaInfillFlowCompensator>(print.config());

    // Orca: Don't output Header block if BTT thumbnail is identified in the list
    // Get the thumbnails value as a string
    std::string thumbnails_value = print.config().option<ConfigOptionString>("thumbnails")->value;
    // search string for the BTT_TFT label
    bool has_BTT_thumbnail = (thumbnails_value.find("BTT_TFT") != std::string::npos);

    if (!has_BTT_thumbnail) {
        file.write_format("; HEADER_BLOCK_START\n");
        // Write information on the generator.
        file.write_format("; generated by %s on %s\n", Slic3r::header_slic3r_generated().c_str(), Slic3r::Utils::local_timestamp().c_str());
        if (is_bbl_printers)
            file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());
        // BBS: total layer number
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Total_Layer_Number_Placeholder).c_str());
        // Orca: extra check for bbl printer
        if (is_bbl_printers) {
            if (print.calib_params().mode == CalibMode::Calib_None) { // Don't support skipping in cali mode
                // list all label_object_id with sorted order here
                m_enable_exclude_object = true;
                m_label_objects_ids.clear();
                m_label_objects_ids.reserve(print.num_object_instances());
                for (const PrintObject* print_object : print.objects())
                    for (const PrintInstance& print_instance : print_object->instances())
                        m_label_objects_ids.push_back(print_instance.model_instance->get_labeled_id());

                std::sort(m_label_objects_ids.begin(), m_label_objects_ids.end());

                std::string objects_id_list = "; model label id: ";
                for (auto it = m_label_objects_ids.begin(); it != m_label_objects_ids.end(); it++)
                    objects_id_list += (std::to_string(*it) + (it != m_label_objects_ids.end() - 1 ? "," : "\n"));
                file.writeln(objects_id_list);
            } else {
                m_enable_exclude_object = false;
                m_label_objects_ids.clear();
            }
        }

        {
            std::string filament_density_list = "; filament_density: ";
            (filament_density_list += m_config.filament_density.serialize()) += '\n';
            file.writeln(filament_density_list);

            std::string filament_diameter_list = "; filament_diameter: ";
            (filament_diameter_list += m_config.filament_diameter.serialize()) += '\n';
            file.writeln(filament_diameter_list);

            coordf_t max_height_z = -1;
            for (const auto& object : print.objects())
                max_height_z = std::max(object->layers().back()->print_z, max_height_z);

            std::ostringstream max_height_z_tip;
            max_height_z_tip << "; max_z_height: " << std::fixed << std::setprecision(2) << max_height_z << '\n';
            file.writeln(max_height_z_tip.str());
        }

        // Ultra (H2C nozzle rack): ported from BambuStudio GCode.cpp:2461-2476. The printer reads
        // both lines out of a stored job: "filament" is the 1-based list of filament slots the job
        // needs (the AMS offering on the machine's own screen), "support_material_on_wipe_tower"
        // tells it whether the prime tower carries support material.
        //
        // This was originally gated on is_bbl_printers, which is false for any non-Bambu vendor
        // profile. That made us the outlier: BambuStudio writes both lines unconditionally
        // (GCode.cpp:2461-2476) and so does upstream OrcaSlicer - its Flashforge Creator 5 export
        // carries "; filament: 2,3,4,1", as does Flash Studio's. The C5 runs Bambu-derived
        // firmware that reads this header out of a stored job, and our export was the only one
        // of the three missing it. Match upstream and always write it.
        {
            // Upstream feeds this from Print::get_slice_used_filaments(false), which is set from
            // ToolOrdering::all_extruders() (Print.cpp:2298,2312). This fork keeps the tool ordering
            // on the wipe-tower data, so it is only populated for a multi-filament plate; fall back
            // to Print::extruders() (object + support + custom-G-code + wipe-tower filaments,
            // sorted unique) when there is none, which is the same set for a single-filament plate.
            std::vector<unsigned int> used_filaments = print.get_tool_ordering().all_extruders();
            if (used_filaments.empty())
                used_filaments = print.extruders(true);
            std::ostringstream out;
            out << "; filament: ";
            for (size_t idx = 0; idx < used_filaments.size(); ++idx) {
                if (idx != 0)
                    out << ',';
                out << used_filaments[idx] + 1;
            }
            file.writeln(out.str());

            file.write_format("; support_material_on_wipe_tower: %d\n", int(print.support_material_on_wipe_tower()));
        }

        file.write_format("; HEADER_BLOCK_END\n\n");
    }

    // BBS: write global config at the beginning of gcode file because printer
    // need these config information
    // Append full config, delimited by two 'phony' configuration keys
    // CONFIG_BLOCK_START and CONFIG_BLOCK_END. The delimiters are structured
    // as configuration key / value pairs to be parsable by older versions of
    // PrusaSlicer G-code viewer.
    {
        if (is_bbl_printers) {
            file.write("; CONFIG_BLOCK_START\n");
            std::string full_config;
            append_full_config(print, full_config);
            if (!full_config.empty())
                file.write(full_config);

            // SoftFever: write compatiple image
            int first_layer_bed_temperature = get_bed_temperature_max(print, true);
            file.write_format("; first_layer_bed_temperature = %d\n", first_layer_bed_temperature);
            file.write_format("; first_layer_temperature = %d\n", print.config().nozzle_temperature_initial_layer.get_at(0));
            file.write("; CONFIG_BLOCK_END\n\n");
        } else if (thumbnail_cb != nullptr) {
            // generate the thumbnails
            auto [thumbnails, errors] = GCodeThumbnails::make_and_check_thumbnail_list(print.full_print_config());

            if (errors != enum_bitmask<ThumbnailError>()) {
                std::string error_str = format("Invalid thumbnails value:");
                error_str += GCodeThumbnails::get_error_string(errors);
                throw Slic3r::ExportError(error_str);
            }

            if (!thumbnails.empty())
                GCodeThumbnails::export_thumbnails_to_file(
                    thumbnail_cb, print.get_plate_index(), thumbnails, [&file](const char* sz) { file.write(sz); },
                    [&print]() { print.throw_if_canceled(); });
        }
    }

    // Write some terse information on the slicing parameters.
    const PrintObject* first_object               = print.objects().front();
    const double       layer_height               = first_object->config().layer_height.value;
    const double       initial_layer_print_height = print.config().initial_layer_print_height.value;
    for (size_t region_id = 0; region_id < print.num_print_regions(); ++region_id) {
        const PrintRegion& region = print.get_print_region(region_id);
        file.write_format("; external perimeters extrusion width = %.2fmm\n",
                          region.flow(*first_object, frExternalPerimeter, layer_height).width());
        file.write_format("; perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frPerimeter, layer_height).width());
        file.write_format("; infill extrusion width = %.2fmm\n", region.flow(*first_object, frInfill, layer_height).width());
        file.write_format("; solid infill extrusion width = %.2fmm\n", region.flow(*first_object, frSolidInfill, layer_height).width());
        file.write_format("; top infill extrusion width = %.2fmm\n", region.flow(*first_object, frTopSolidInfill, layer_height).width());
        if (print.has_support_material())
            file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
        if (print.config().initial_layer_line_width.value > 0)
            file.write_format("; first layer extrusion width = %.2fmm\n",
                              region.flow(*first_object, frPerimeter, initial_layer_print_height, true).width());
        file.write_format("\n");
    }

    file.write_format("; EXECUTABLE_BLOCK_START\n");

    // Number the objects and their instances before any of the labels below can read the numbers.
    assign_object_and_instance_ids(print);

    // SoftFever
    if (m_enable_exclude_object)
        file.write(set_object_info(&print));

    // adds tags for time estimators
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    m_placeholder_parser_integration.parser = print.placeholder_parser();
    m_placeholder_parser_integration.parser.update_timestamp();
    m_placeholder_parser_integration.parser.update_user_name();
    m_placeholder_parser_integration.context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // Enable passing global variables between PlaceholderParser invocations.
    m_placeholder_parser_integration.context.global_config = std::make_unique<DynamicConfig>();
    print.update_object_placeholders(m_placeholder_parser_integration.parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    unsigned int initial_extruder_id = (unsigned int) -1;
    // BBS: first non-support filament extruder
    unsigned int                                      initial_non_support_extruder_id;
    unsigned int                                      final_extruder_id = (unsigned int) -1;
    bool                                              has_wipe_tower    = false;
    std::vector<const PrintInstance*>                 print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator print_object_instance_sequential_active;
    if (print.config().print_sequence == PrintSequence::ByObject) {
        // Order object instances for sequential print.
        print_object_instances_ordering = sort_object_instances_by_model_order(print);
        //        print_object_instances_ordering = sort_object_instances_by_max_z(print);
        // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
            if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1)) {
                // BBS: try to find the non-support filament extruder if is multi color and initial_extruder is support filament
                initial_non_support_extruder_id = initial_extruder_id;
                if (tool_ordering.all_extruders().size() > 1 && print.config().filament_is_support.get_at(initial_extruder_id)) {
                    bool has_non_support_filament = false;
                    for (unsigned int extruder : tool_ordering.all_extruders()) {
                        if (!print.config().filament_is_support.get_at(extruder)) {
                            has_non_support_filament = true;
                            break;
                        }
                    }
                    // BBS: find the non-support filament extruder of object
                    if (has_non_support_filament)
                        for (LayerTools layer_tools : tool_ordering.layer_tools()) {
                            if (!layer_tools.has_object)
                                continue;
                            for (unsigned int extruder : layer_tools.extruders) {
                                if (print.config().filament_is_support.get_at(extruder))
                                    continue;
                                initial_non_support_extruder_id = extruder;
                                break;
                            }
                        }
                }

                break;
            }
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No object can be printed. Maybe too small")));
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Print::extruders() expands mixed virtual IDs (wall_filament=3 on a 2-physical plate) to
        // physical components via MixedFilamentManager. Union ToolOrdering's resolved set too:
        // that is what the by-layer path feeds GCodeWriter, and it picks up any extra physical
        // IDs collect_extruders registered (image-row candidates, grouped patterns).
        std::vector<unsigned int> extruder_ids = print.extruders();
        append(extruder_ids, tool_ordering.all_extruders());
        sort_remove_duplicates(extruder_ids);
        this->set_extruders(extruder_ids);

        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
    } else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
        tool_ordering = print.tool_ordering();
        tool_ordering.assign_custom_gcodes(print);
        if (tool_ordering.all_extruders().empty())
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_(L("No object can be printed. Maybe too small")));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        // Orca: support all extruder priming
        initial_extruder_id = (!is_bbl_printers && has_wipe_tower && !print.config().single_extruder_multi_material_priming) ?
                                  // The priming towers will be skipped.
                                  tool_ordering.all_extruders().back() :
                                  // Don't skip the priming towers.
                                  tool_ordering.first_extruder();

        // BBS: try to find the non-support filament extruder if is multi color and initial_extruder is support filament
        if (initial_extruder_id != static_cast<unsigned int>(-1)) {
            initial_non_support_extruder_id = initial_extruder_id;
            if (tool_ordering.all_extruders().size() > 1 && print.config().filament_is_support.get_at(initial_extruder_id)) {
                bool has_non_support_filament = false;
                for (unsigned int extruder : tool_ordering.all_extruders()) {
                    if (!print.config().filament_is_support.get_at(extruder)) {
                        has_non_support_filament = true;
                        break;
                    }
                }
                // BBS: find the non-support filament extruder of object
                if (has_non_support_filament)
                    for (LayerTools layer_tools : tool_ordering.layer_tools()) {
                        if (!layer_tools.has_object)
                            continue;
                        for (unsigned int extruder : layer_tools.extruders) {
                            if (print.config().filament_is_support.get_at(extruder))
                                continue;
                            initial_non_support_extruder_id = extruder;
                            break;
                        }
                    }
            }
        }

        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in
        // Model::custom_gcode_per_print_z. Therefore initialize the printing extruders from there.
        this->set_extruders(tool_ordering.all_extruders());
        print_object_instances_ordering =
            // By default, order object instances using a nearest neighbor search.
            print.config().print_order == PrintOrder::Default ? chain_print_object_instances(print)
                                                                // Otherwise same order as the object list
                                                                :
                                                                sort_object_instances_by_model_order(print);
    }
    if (initial_extruder_id == (unsigned int) -1) {
        // Nothing to print!
        initial_extruder_id             = 0;
        initial_non_support_extruder_id = 0;
        final_extruder_id               = 0;
    } else {
        final_extruder_id = tool_ordering.last_extruder();
        assert(final_extruder_id != (unsigned int) -1);
    }
    print.throw_if_canceled();

    // Ultra: fold the first printing filament's Z offset into the job's Z offset
    // (per-filament squish / plate-surface compensation, OrcaSlicer #4660).
    if (initial_extruder_id != static_cast<unsigned int>(-1) &&
        initial_extruder_id < m_config.filament_z_offset.values.size()) {
        const double filament_z_offset = m_config.filament_z_offset.get_at(initial_extruder_id);
        if (filament_z_offset != 0.) {
            m_config.z_offset.value += filament_z_offset;
            BOOST_LOG_TRIVIAL(info) << "GCode: applying filament_z_offset " << filament_z_offset
                                    << " for filament " << initial_extruder_id;
        }
    }

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    // Construct the layer-time speed smoothing stage only when enabled so Off
    // keeps today's streaming path (no extra layer buffer). The TBB filter is still
    // present as a no-op identity when this pointer is null. spiral_mode is identity
    // inside the filter (no buffer / no F rewrite).
    if (m_config.layer_time_speed_smoothing.value != ltssmOff)
        m_layer_time_speed_smoothing = make_unique<LayerTimeSpeedSmoothingFilter>(m_config);

    // Orca: Initialise AdaptivePA processor filter
    m_pa_processor = std::make_unique<AdaptivePAProcessor>(*this, tool_ordering.all_extruders());

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print);

    // Disable fan.
    if (m_config.auxiliary_fan.value && print.config().close_fan_the_first_x_layers.get_at(initial_extruder_id)) {
        file.write(m_writer.set_fan(0));
        // BBS: disable additional fan
        file.write(m_writer.set_additional_fan(0));
    }

    // Update output variables after the extruders were initialized.
    m_placeholder_parser_integration.init(m_writer);
    // Let the start-up script prime the 1st printing tool.
    this->placeholder_parser().set("initial_tool", initial_extruder_id);
    this->placeholder_parser().set("initial_extruder", initial_extruder_id);
    // BBS
    this->placeholder_parser().set("initial_no_support_tool", initial_non_support_extruder_id);
    this->placeholder_parser().set("initial_no_support_extruder", initial_non_support_extruder_id);
    this->placeholder_parser().set("current_extruder", initial_extruder_id);
    // Orca: set the key for compatibilty
    this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(initial_extruder_id));
    this->placeholder_parser().set("temperature", new ConfigOptionInts(print.config().nozzle_temperature));

    this->placeholder_parser().set("retraction_distances_when_cut", new ConfigOptionFloats(m_config.retraction_distances_when_cut));
    this->placeholder_parser().set("long_retractions_when_cut", new ConfigOptionBools(m_config.long_retractions_when_cut));
    // BBS: initial extruder-change retraction values, plus the whole arrays (upstream publishes both).
    set_ec_retraction_placeholders(this->placeholder_parser(), m_config, size_t(initial_extruder_id));
    this->placeholder_parser().set("retraction_distances_when_ec", new ConfigOptionFloatsNullable(m_config.retraction_distances_when_ec));
    this->placeholder_parser().set("long_retractions_when_ec", new ConfigOptionBoolsNullable(m_config.long_retractions_when_ec));
    // Set variable for total layer count so it can be used in custom gcode.
    this->placeholder_parser().set("total_layer_count", m_layer_count);
    // Useful for sequential prints.
    this->placeholder_parser().set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    this->placeholder_parser().set("has_wipe_tower", has_wipe_tower);
    this->placeholder_parser().set("has_single_extruder_multi_material_priming",
                                   !is_bbl_printers && has_wipe_tower && print.config().single_extruder_multi_material_priming);
    this->placeholder_parser()
        .set("total_toolchanges",
             std::max(0,
                      print.wipe_tower_data()
                          .number_of_toolchanges)); // Check for negative toolchanges (single extruder mode) and set to 0 (no tool change).
    this->placeholder_parser().set("num_extruders", int(print.config().nozzle_diameter.values.size()));
    this->placeholder_parser().set("retract_length", new ConfigOptionFloats(print.config().retraction_length));

    // Orca: support max MAXIMUM_EXTRUDER_NUMBER extruders/filaments
    std::vector<unsigned char> is_extruder_used(std::max(size_t(MAXIMUM_EXTRUDER_NUMBER), print.config().filament_diameter.size()), 0);
    for (unsigned int extruder : tool_ordering.all_extruders())
        is_extruder_used[extruder] = true;
    this->placeholder_parser().set("is_extruder_used", new ConfigOptionBools(is_extruder_used));

    {
        BoundingBoxf bbox_bed(print.config().printable_area.values);
        Vec2f        plate_offset = m_writer.get_xy_offset();
        this->placeholder_parser().set("print_bed_min", new ConfigOptionFloats({bbox_bed.min.x(), bbox_bed.min.y()}));
        this->placeholder_parser().set("print_bed_max", new ConfigOptionFloats({bbox_bed.max.x(), bbox_bed.max.y()}));
        this->placeholder_parser().set("print_bed_size", new ConfigOptionFloats({bbox_bed.size().x(), bbox_bed.size().y()}));

        BoundingBoxf bbox;
        auto         pts = std::make_unique<ConfigOptionPoints>();
        if (print.calib_mode() == CalibMode::Calib_PA_Line || print.calib_mode() == CalibMode::Calib_PA_Pattern) {
            bbox = bbox_bed;
            bbox.offset(-25.0);
            // add 4 corner points of bbox into pts
            pts->values.reserve(4);
            pts->values.emplace_back(bbox.min.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.min.y());
            pts->values.emplace_back(bbox.max.x(), bbox.max.y());
            pts->values.emplace_back(bbox.min.x(), bbox.max.y());

        } else {
            // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
            // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
            // It does NOT encompass user extrusions generated by custom G-code,
            // therefore it does NOT encompass the initial purge line.
            // It does NOT encompass MMU/MMU2 starting (wipe) areas.
            pts->values.reserve(print.first_layer_convex_hull().size());
            for (const Point& pt : print.first_layer_convex_hull().points)
                pts->values.emplace_back(print.translate_to_print_space(pt));
            bbox = BoundingBoxf((pts->values));
        }
        this->placeholder_parser().set("first_layer_print_convex_hull", pts.release());
        this->placeholder_parser().set("first_layer_print_min", new ConfigOptionFloats({bbox.min.x(), bbox.min.y()}));
        this->placeholder_parser().set("first_layer_print_max", new ConfigOptionFloats({bbox.max.x(), bbox.max.y()}));
        this->placeholder_parser().set("first_layer_print_size", new ConfigOptionFloats({bbox.size().x(), bbox.size().y()}));

        {
            // use first layer convex_hull union with each object's bbox to check whether in head detect zone
            Polygons object_projections;
            for (auto& obj : print.objects()) {
                for (auto& instance : obj->instances()) {
                    const auto& bbox = instance.get_bounding_box();
                    Point       min_p{coord_t(scale_(bbox.min.x())), coord_t(scale_(bbox.min.y()))};
                    Point       max_p{coord_t(scale_(bbox.max.x())), coord_t(scale_(bbox.max.y()))};
                    Polygon     instance_projection = {{min_p.x(), min_p.y()},
                                                       {max_p.x(), min_p.y()},
                                                       {max_p.x(), max_p.y()},
                                                       {min_p.x(), max_p.y()}};
                    object_projections.emplace_back(std::move(instance_projection));
                }
            }
            object_projections.emplace_back(print.first_layer_convex_hull());

            Polygons project_polys = union_(object_projections);
            Polygon  head_wrap_detect_zone;
            for (auto& point : print.config().head_wrap_detect_zone.values)
                head_wrap_detect_zone.append(scale_(point).cast<coord_t>() + scale_(plate_offset).cast<coord_t>());

            this->placeholder_parser().set("in_head_wrap_detect_zone", !intersection_pl(project_polys, {head_wrap_detect_zone}).empty());
        }

        BoundingBoxf mesh_bbox(m_config.bed_mesh_min, m_config.bed_mesh_max);
        auto         mesh_margin = m_config.adaptive_bed_mesh_margin.value;
        mesh_bbox.min            = mesh_bbox.min.cwiseMax((bbox.min.array() - mesh_margin).matrix());
        mesh_bbox.max            = mesh_bbox.max.cwiseMin((bbox.max.array() + mesh_margin).matrix());
        this->placeholder_parser().set("adaptive_bed_mesh_min", new ConfigOptionFloats({mesh_bbox.min.x(), mesh_bbox.min.y()}));
        this->placeholder_parser().set("adaptive_bed_mesh_max", new ConfigOptionFloats({mesh_bbox.max.x(), mesh_bbox.max.y()}));

        auto probe_dist_x  = std::max(1., m_config.bed_mesh_probe_distance.value.x());
        auto probe_dist_y  = std::max(1., m_config.bed_mesh_probe_distance.value.y());
        int  probe_count_x = std::max(3, (int) std::ceil(mesh_bbox.size().x() / probe_dist_x));
        int  probe_count_y = std::max(3, (int) std::ceil(mesh_bbox.size().y() / probe_dist_y));
        auto bed_mesh_algo = "bicubic";
        if (probe_count_x * probe_count_y <= 6) { // lagrange needs up to a total of 6 mesh points
            bed_mesh_algo = "lagrange";
        } else if (print.config().gcode_flavor == gcfKlipper) {
            // bicubic needs 4 probe points per axis
            probe_count_x = std::max(probe_count_x, 4);
            probe_count_y = std::max(probe_count_y, 4);
        }
        this->placeholder_parser().set("bed_mesh_probe_count", new ConfigOptionInts({probe_count_x, probe_count_y}));
        this->placeholder_parser().set("bed_mesh_algo", bed_mesh_algo);
        // get center without wipe tower
        BoundingBoxf bbox_wo_wt; // bounding box without wipe tower
        for (auto& objPtr : print.objects()) {
            BBoxData data;
            bbox_wo_wt.merge(unscaled(objPtr->get_first_layer_bbox(data.area, data.layer_height, data.name)));
        }
        auto center = bbox_wo_wt.center();
        this->placeholder_parser().set("first_layer_center_no_wipe_tower", new ConfigOptionFloats{{center.x(), center.y()}});
    }
    bool activate_chamber_temp_control = false;
    auto max_chamber_temp              = 0;
    for (const auto& extruder : m_writer.extruders()) {
        activate_chamber_temp_control |= m_config.activate_chamber_temp_control.get_at(extruder.id());
        max_chamber_temp = std::max(max_chamber_temp, m_config.chamber_temperature.get_at(extruder.id()));
    }
    float outer_wall_volumetric_speed = 0.0f;
    {
        int curr_bed_type = m_config.curr_bed_type.getInt();

        std::string first_layer_bed_temp_str;
        const BedType curr_bed = static_cast<BedType>(curr_bed_type);
        // Some bed types (for example newly added btGESP) may not have dedicated temp options in old presets.
        // Fallback to PEI temps to avoid null-dereference during slicing placeholder setup.
        const std::string first_bed_temp_key = get_bed_temp_1st_layer_key(curr_bed).empty() ? get_bed_temp_1st_layer_key(btPEI) : get_bed_temp_1st_layer_key(curr_bed);
        const std::string bed_temp_key       = get_bed_temp_key(curr_bed).empty() ? get_bed_temp_key(btPEI) : get_bed_temp_key(curr_bed);
        const ConfigOptionInts* first_bed_temp_opt = m_config.option<ConfigOptionInts>(first_bed_temp_key);
        const ConfigOptionInts* bed_temp_opt       = m_config.option<ConfigOptionInts>(bed_temp_key);
        ConfigOptionInts        safe_zero_bed_temp({ 0 });
        if (first_bed_temp_opt == nullptr || bed_temp_opt == nullptr) {
            first_bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_1st_layer_key(btPEI));
            bed_temp_opt       = m_config.option<ConfigOptionInts>(get_bed_temp_key(btPEI));
        }
        if (first_bed_temp_opt == nullptr || bed_temp_opt == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "Missing bed temperature options for current bed type, using safe zero placeholders.";
            first_bed_temp_opt = &safe_zero_bed_temp;
            bed_temp_opt       = &safe_zero_bed_temp;
        }
        this->placeholder_parser().set("bbl_bed_temperature_gcode", new ConfigOptionBool(false));
        this->placeholder_parser().set("bed_temperature_initial_layer", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("bed_temperature", new ConfigOptionInts(*bed_temp_opt));
        // BBS: the single bed temperature value accommodates the highest-temperature filament of the print
        // (max over all used extruders instead of the initial extruder only).
        int bed_temp_single = 0;
        for (const unsigned int extruder_id : print.extruders())
            bed_temp_single = std::max(bed_temp_single, first_bed_temp_opt->get_at(extruder_id));
        this->placeholder_parser().set("bed_temperature_initial_layer_single", new ConfigOptionInt(bed_temp_single));
        this->placeholder_parser().set("bed_temperature_initial_layer_vector", new ConfigOptionString());
        this->placeholder_parser().set("chamber_temperature", new ConfigOptionInts(m_config.chamber_temperature));
        this->placeholder_parser().set("overall_chamber_temperature", new ConfigOptionInt(max_chamber_temp));

        // SoftFever: support variables `first_layer_temperature` and `first_layer_bed_temperature`
        this->placeholder_parser().set("first_layer_bed_temperature", new ConfigOptionInts(*first_bed_temp_opt));
        this->placeholder_parser().set("first_layer_temperature", new ConfigOptionInts(m_config.nozzle_temperature_initial_layer));
        this->placeholder_parser().set("max_print_height", new ConfigOptionInt(m_config.printable_height));
        this->placeholder_parser().set("z_offset", new ConfigOptionFloat(m_config.z_offset));
        this->placeholder_parser().set("model_name", new ConfigOptionString(print.get_model_name()));
        this->placeholder_parser().set("plate_number", new ConfigOptionString(print.get_plate_number_formatted()));
        this->placeholder_parser().set("plate_name", new ConfigOptionString(print.get_plate_name()));
        this->placeholder_parser().set("first_layer_height", new ConfigOptionFloat(m_config.initial_layer_print_height.value));

        // add during_print_exhaust_fan_speed
        std::vector<int> during_print_exhaust_fan_speed_num;
        during_print_exhaust_fan_speed_num.reserve(m_config.during_print_exhaust_fan_speed.size());
        for (const auto& item : m_config.during_print_exhaust_fan_speed.values)
            during_print_exhaust_fan_speed_num.emplace_back((int) (item / 100.0 * 255));
        this->placeholder_parser().set("during_print_exhaust_fan_speed_num", new ConfigOptionInts(during_print_exhaust_fan_speed_num));

        // calculate the volumetric speed of outer wall. Ignore per-object setting and multi-filament, and just use the default setting
        {
            float filament_max_volumetric_speed = m_config.option<ConfigOptionFloats>("filament_max_volumetric_speed")
                                                      ->get_at(initial_non_support_extruder_id);
            const double nozzle_diameter       = m_config.nozzle_diameter.get_at(initial_non_support_extruder_id);
            float        outer_wall_line_width = print.default_region_config().get_abs_value("outer_wall_line_width", nozzle_diameter);
            if (outer_wall_line_width == 0.0) {
                float default_line_width = print.default_object_config().get_abs_value("line_width", nozzle_diameter);
                outer_wall_line_width    = default_line_width == 0.0 ? nozzle_diameter : default_line_width;
            }
            Flow  outer_wall_flow       = Flow(outer_wall_line_width, m_config.layer_height,
                                               m_config.nozzle_diameter.get_at(initial_non_support_extruder_id));
            float outer_wall_speed      = print.default_region_config().outer_wall_speed.values.front();
            outer_wall_volumetric_speed = outer_wall_speed * outer_wall_flow.mm3_per_mm();
            if (outer_wall_volumetric_speed > filament_max_volumetric_speed)
                outer_wall_volumetric_speed = filament_max_volumetric_speed;
            this->placeholder_parser().set("outer_wall_volumetric_speed", new ConfigOptionFloat(outer_wall_volumetric_speed));
        }

        if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
            this->placeholder_parser().set("scan_first_layer", new ConfigOptionBool(false));
        }
    }

    // Compute chamber cooling mode based on all filaments used on this plate.
    constexpr int kChamberCoolingKeepWarm  = 0;
    constexpr int kChamberCoolingWeak      = 1;
    constexpr int kChamberCoolingStrong    = 2;
    constexpr int kVitrificationStrongCool = 50;  // <= 50: trigger strong cooling
    constexpr int kVitrificationWeakCool   = 70;  // <= 70: trigger weak cooling (and > 50)
    {
        int chamber_cooling_mode = kChamberCoolingKeepWarm;
        for (unsigned int extruder : tool_ordering.all_extruders()) {
            if (!m_config.filament_is_high_temperature.get_at(extruder)) {
                int vitrification = m_config.temperature_vitrification.get_at(extruder);
                if (vitrification <= kVitrificationStrongCool) {
                    chamber_cooling_mode = kChamberCoolingStrong;
                    break;
                } else if (vitrification <= kVitrificationWeakCool) {
                    chamber_cooling_mode = kChamberCoolingWeak;
                }
            }
        }
        this->placeholder_parser().set("chamber_cooling_mode", new ConfigOptionInt(chamber_cooling_mode));
    }

    // Ultra: publish BambuStudio 2.x placeholder vars used by newer machine gcode templates
    // (machine_start / change_filament). Single-nozzle: filament order == array index, no
    // per-nozzle remap (dual-nozzle grouping is a separate, deferred port).
    {
        int min_vitrification = std::numeric_limits<int>::max();
        for (unsigned int extruder : tool_ordering.all_extruders())
            min_vitrification = std::min(min_vitrification, m_config.temperature_vitrification.get_at(extruder));
        if (min_vitrification == std::numeric_limits<int>::max())
            min_vitrification = 0;
        this->placeholder_parser().set("min_vitrification_temperature", new ConfigOptionInt(min_vitrification));

        const auto* flush_temp_opt = m_config.option<ConfigOptionInts>("filament_flush_temp");
        const auto* flush_vspd_opt = m_config.option<ConfigOptionFloats>("filament_flush_volumetric_speed");
        const size_t num_filaments = m_config.filament_max_volumetric_speed.size();
        std::vector<int>    flush_temps;   flush_temps.reserve(num_filaments);
        std::vector<double> flush_vspeeds; flush_vspeeds.reserve(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            double vs = (flush_vspd_opt && i < flush_vspd_opt->size()) ? flush_vspd_opt->get_at(int(i)) : 0.;
            if (vs == 0.) vs = get_value_at(m_config, m_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, int(i));
            flush_vspeeds.push_back(vs);
            int ft = (flush_temp_opt && i < flush_temp_opt->size()) ? flush_temp_opt->get_at(int(i)) : 0;
            if (ft == 0) ft = m_config.nozzle_temperature_range_high.get_at(int(i));
            flush_temps.push_back(ft);
        }
        this->placeholder_parser().set("flush_volumetric_speeds", new ConfigOptionFloats(flush_vspeeds));
        this->placeholder_parser().set("flush_temperatures", new ConfigOptionInts(flush_temps));
        // Ultra: A2L time_lapse_gcode reads clear_to_x0 (BBS computes it per-layer via a
        // timelapse position-picker the fork lacks). Single-nozzle shim = always clear.
        this->placeholder_parser().set("clear_to_x0", new ConfigOptionBool(true));

        // Ultra: single-nozzle shims for other BBS 2.x machine/change_filament template vars the
        // fork's (single-nozzle) engine doesn't compute. filament_map = all-on-extruder-1 (correct
        // for one nozzle); optional features (prime-tower interface, wipe-avoid, tall-bed obstacle
        // detect) safely disabled. These are set globally so change_filament sees them too.
        {
            // Ultra (Phase 6): drive per-filament nozzle routing from the stored grouping result (H2D/H2C/X2D).
            // filament_map is 1-based extruder per filament; the change_filament template + printer firmware
            // route each filament to its nozzle from this. Null result (classic machines) -> all-1 (single nozzle).
            std::vector<int> fmap(num_filaments > 0 ? num_filaments : 1, 1);
            if (auto gr = print.get_layered_nozzle_group_result()) {
                auto em = gr->get_extruder_map(false); // 1-based, per filament index
                for (size_t i = 0; i < fmap.size() && i < em.size(); ++i)
                    if (em[i] >= 1) fmap[i] = em[i];
            } else if (print.is_BBL_printer() && print.config().nozzle_diameter.size() >= 2) {
                // A print-by-object plate with several objects has no grouping result (the grouping is
                // skipped for sequential prints), but its filament_map is still the plate's assignment,
                // which is what Bambu Studio publishes here. The all-1 shim sent the start G-code's
                // "M104 T{filament_map[..] % 2}" / "G151 P{..} M" to the LEFT hotend while the first
                // filament was loaded into the right one.
                const std::vector<int> &cfm = print.config().filament_map.values;
                for (size_t i = 0; i < fmap.size() && i < cfm.size(); ++i)
                    if (cfm[i] >= 1 && size_t(cfm[i]) <= print.config().nozzle_diameter.size())
                        fmap[i] = cfm[i];
            }
            this->placeholder_parser().set("filament_map", new ConfigOptionInts(fmap));
            bool all_bbl = true;
            const auto* vend = m_config.option<ConfigOptionStrings>("filament_vendor");
            if (vend)
                for (unsigned int e : tool_ordering.all_extruders())
                    if (int(e) < int(vend->size()) && vend->get_at(int(e)).find("Bambu") == std::string::npos) { all_bbl = false; break; }
            this->placeholder_parser().set("is_all_bbl_filament", new ConfigOptionBool(all_bbl));
            this->placeholder_parser().set("has_tpu_in_first_layer", new ConfigOptionBool(false));
            this->placeholder_parser().set("cooling_filter_enabled", new ConfigOptionBool(false));
            this->placeholder_parser().set("max_print_z", new ConfigOptionFloat(0.));
            this->placeholder_parser().set("is_prime_tower_interface", new ConfigOptionBool(false));
            this->placeholder_parser().set("filament_tower_interface_purge_volume", new ConfigOptionFloat(0.));
            this->placeholder_parser().set("filament_tower_interface_print_temp", new ConfigOptionInt(0));
            this->placeholder_parser().set("wipe_avoid_perimeter", new ConfigOptionBool(false));
            this->placeholder_parser().set("wipe_avoid_pos_x", new ConfigOptionFloat(0.));
        }

        // Ultra: dual-nozzle machine templates (H2D/H2D Pro/H2C) reference per-nozzle-id vars that
        // the BBS grouping engine normally computes. Single-mapped shim: everything runs on nozzle 0
        // (filament_map all-1). Per-nozzle arrays are sized to the physical nozzle count so template
        // index lookups (e.g. nozzle_diameter_at_nozzle_id[current_nozzle_id]) resolve. Real per-nozzle
        // assignment is the deferred grouping-engine wiring.
        {
            const auto& nd = m_config.nozzle_diameter.values;
            const size_t num_nozzles = std::max<size_t>(nd.size(), size_t(1));
            std::vector<double> nozzle_diams(nd.begin(), nd.end());
            if (nozzle_diams.empty()) nozzle_diams.push_back(0.4);

            std::vector<int> nozzle_vts;
            const auto* nvt = m_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
            for (size_t i = 0; i < num_nozzles; ++i)
                nozzle_vts.push_back((nvt && i < nvt->size()) ? nvt->get_at(int(i)) : 0);

            // Ultra (H2C rack): on a rack machine these arrays are indexed by LOGICAL NOZZLE id,
            // not by extruder, and there are more nozzles than extruders (H2C: 2 extruders,
            // up to 7 nozzles). Take them from the grouping result the way BambuStudio does
            // (GCode.cpp:119-155); keep the per-extruder shim when the plate has no grouping
            // result, so single-nozzle machines are unaffected.
            int shim_nozzle_id = 0;
            int shim_physical_extruder = 0;
            auto group_result_g = print.get_layered_nozzle_group_result();
            if (group_result_g && has_nozzle_rack(m_config)) {
                auto diams = get_nozzle_diameters_by_nozzle_id(group_result_g.get());
                if (!diams.empty())
                    nozzle_diams = std::move(diams);
                this->placeholder_parser().set("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result_g.get())));
                if (auto first = group_result_g->get_first_nozzle_for_filament(int(initial_extruder_id))) {
                    shim_nozzle_id = std::max(0, first->group_id);
                    if (first->extruder_id >= 0 && first->extruder_id < int(m_config.physical_extruder_map.size()))
                        shim_physical_extruder = m_config.physical_extruder_map.get_at(first->extruder_id);
                }
            } else {
                this->placeholder_parser().set("nozzle_volume_types", new ConfigOptionInts(nozzle_vts));
            }
            this->placeholder_parser().set("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(nozzle_diams));

            this->placeholder_parser().set("current_nozzle_id",              new ConfigOptionInt(shim_nozzle_id));
            this->placeholder_parser().set("next_nozzle_id",                 new ConfigOptionInt(shim_nozzle_id));
            this->placeholder_parser().set("initial_nozzle_id",              new ConfigOptionInt(shim_nozzle_id));
            this->placeholder_parser().set("curr_physical_extruder_id",      new ConfigOptionInt(shim_physical_extruder));
            this->placeholder_parser().set("most_used_physical_extruder_id", new ConfigOptionInt(shim_physical_extruder));
            this->placeholder_parser().set("new_extruder_retracted_length",  new ConfigOptionFloat(0.));
            this->placeholder_parser().set("initial_no_support_filament_id", new ConfigOptionInt(int(initial_extruder_id)));
            // *_hotend = the physical-nozzle id for the filament; BBS's NOZZLE_ID_FOR_GCODE returns -1
            // when there is no dynamic nozzle map (our single-mapped case), so all hotend vars are -1.
            this->placeholder_parser().set("initial_no_support_hotend",       new ConfigOptionInt(-1));
            this->placeholder_parser().set("initial_hotend",                  new ConfigOptionInt(-1));
            this->placeholder_parser().set("current_hotend",                  new ConfigOptionInt(-1));
            this->placeholder_parser().set("next_hotend",                     new ConfigOptionInt(-1));

            // Ultra: remaining single-mapped shims for dual-machine templates. Extruder-change (_ec)
            // retraction is no longer shimmed here - long_retractions_when_ec / retraction_distances_when_ec
            // are real per-filament options now and are published per toolchange (see
            // set_ec_retraction_placeholders). Timelapse position-picker (BBS computes per-layer) disabled;
            // first_filaments = the initial filament on every physical extruder (single-mapped).
            this->placeholder_parser().set("farthest_point_timelapse_enabled", new ConfigOptionBool(false));
            this->placeholder_parser().set("has_timelapse_safe_pos",           new ConfigOptionBool(false));
            this->placeholder_parser().set("timelapse_inline_photo",           new ConfigOptionBool(false));
            this->placeholder_parser().set("timelapse_pos_x",                  new ConfigOptionFloat(0.));
            this->placeholder_parser().set("timelapse_pos_y",                  new ConfigOptionFloat(0.));
            std::vector<int> first_fils(num_nozzles, int(initial_extruder_id));
            std::vector<int> first_non_support_fils = first_fils;
            if (print.is_BBL_printer() && num_nozzles >= 2) {
                // Bambu Studio GCode.cpp:2575-2705 (ToolOrdering::cal_non_support_filaments +
                // match_physical_extruder_for_each_filament): the first filament, and the first
                // non-support filament, each extruder prints, indexed by PHYSICAL extruder, -1 when an
                // extruder prints nothing. The start G-code's toolhead-offset calibration hands these to
                // the firmware per hotend ("M620.17 T<physical> S<temp> L<filament>"); the old shim
                // named the first filament for BOTH hotends, so the idle hotend was given a filament
                // that is fed to the other one.
                const std::vector<int> &fm  = print.config().filament_map.values;
                const std::vector<int> &pem = print.config().physical_extruder_map.values;
                std::vector<int> first_logical(num_nozzles, -1), first_ns_logical(num_nozzles, -1);
                auto scan = [&](const ToolOrdering &to) {
                    for (const LayerTools &lt : to)
                        for (unsigned int f : lt.extruders) {
                            if (f >= fm.size() || fm[f] < 1 || size_t(fm[f]) > num_nozzles)
                                continue;
                            const size_t e = size_t(fm[f] - 1);
                            if (first_logical[e] < 0)
                                first_logical[e] = int(f);
                            if (first_ns_logical[e] < 0 && !print.config().filament_is_support.get_at(f))
                                first_ns_logical[e] = int(f);
                        }
                };
                if (print.config().print_sequence == PrintSequence::ByObject) {
                    const PrintObject *prev = nullptr;
                    for (const PrintInstance *inst : print_object_instances_ordering) {
                        if (inst->print_object == prev)
                            continue;
                        prev = inst->print_object;
                        scan(ToolOrdering(*inst->print_object, (unsigned int) -1));
                        if (std::find(first_ns_logical.begin(), first_ns_logical.end(), -1) == first_ns_logical.end())
                            break;
                    }
                } else {
                    scan(tool_ordering);
                }
                auto to_physical = [&](const std::vector<int> &logical) {
                    std::vector<int> physical(num_nozzles, -1);
                    for (size_t e = 0; e < num_nozzles; ++e) {
                        const size_t p = (e < pem.size() && pem[e] >= 0 && size_t(pem[e]) < num_nozzles) ? size_t(pem[e]) : e;
                        physical[p] = logical[e];
                    }
                    return physical;
                };
                first_fils             = to_physical(first_logical);
                first_non_support_fils = to_physical(first_ns_logical);
            }
            this->placeholder_parser().set("first_filaments",             new ConfigOptionInts(first_fils));
            this->placeholder_parser().set("first_non_support_filaments", new ConfigOptionInts(first_non_support_fils));
            // H2C templates also reference these (single-mapped: hotend ids -1, no computed wipe-tower center).
            this->placeholder_parser().set("first_non_support_hotend",    new ConfigOptionInts(std::vector<int>(num_nozzles, -1)));
            this->placeholder_parser().set("wipe_tower_center_pos_valid", new ConfigOptionBool(false));

            // X2D templates additionally reference newer BBS fan/chamber vars the fork lacks. Shim:
            // initial_filament_id = the initial extruder; no first-layer aux-fan ramp (close=0, full from
            // layer 1, first-x speed 0 so it collapses to additional_cooling_fan_speed); chamber
            // autocooling max = full (U max_additional_fan/100). current/next_filament_id set globally
            // as a fallback for layer_change; the change_filament scopes override them per-toolchange.
            const size_t nf = (num_filaments > 0) ? num_filaments : size_t(1);
            this->placeholder_parser().set("initial_filament_id",                new ConfigOptionInt(int(initial_extruder_id)));
            this->placeholder_parser().set("current_filament_id",                new ConfigOptionInt(0));
            this->placeholder_parser().set("next_filament_id",                   new ConfigOptionInt(0));
            this->placeholder_parser().set("hold_chamber_temp_for_flat_print",   new ConfigOptionBool(false));
            this->placeholder_parser().set("max_additional_fan",                 new ConfigOptionInt(100));
            this->placeholder_parser().set("close_additional_fan_first_x_layers",new ConfigOptionInts(std::vector<int>(nf, 0)));
            this->placeholder_parser().set("additional_fan_full_speed_layer",    new ConfigOptionInts(std::vector<int>(nf, 1)));
            this->placeholder_parser().set("first_x_layer_fan_speed",            new ConfigOptionFloats(std::vector<double>(nf, 0.)));
            // Ultra: wipe-tower center coords used via the [opt_key] placeholder syntax in
            // change_filament (wipe_tower_center_pos_valid=false gates them).
            // The per-filament hotend-change (_nc) vars are NOT shimmed any more: the H2C template's
            // M620.15 P[filament_pre_cooling_temperature_nc[next_filament_id]] reads the real config
            // option, and M620.11 O1 T[filament_retract_length_nc] gets the outgoing filament's value
            // per toolchange (outgoing_filament_retract_length_nc), as in BambuStudio.
            this->placeholder_parser().set("wipe_tower_center_pos_x",             new ConfigOptionFloat(0.));
            this->placeholder_parser().set("wipe_tower_center_pos_y",             new ConfigOptionFloat(0.));
        }
    }

    std::string machine_start_gcode = this->placeholder_parser_process("machine_start_gcode", print.config().machine_start_gcode.value,
                                                                       initial_extruder_id);
    if (print.config().gcode_flavor != gcfKlipper) {
        // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
        this->_print_first_layer_bed_temperature(file, print, machine_start_gcode, true);
        // Set extruder(s) temperature before and after start G-code.
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, false);
    }

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                      ExtrusionEntity::role_to_string(erCustom).c_str());

    // Orca: set chamber temperature at the beginning of gcode file
    if (activate_chamber_temp_control && max_chamber_temp > 0)
        file.write(m_writer.set_chamber_temperature(max_chamber_temp, true)); // set chamber_temperature

    // Write the custom start G-code
    file.writeln(machine_start_gcode);
    // BBS: mark the end of the machine start G-code for the idle-nozzle pre-cooling (BambuStudio
    // GCode.cpp:2991). Written only when it applies, so no other printer's G-code changes.
    m_pre_cooling_markers = PreCooling::pre_cooling_active(print);
    if (m_pre_cooling_markers)
        file.write_format(";%s\n", PreCooling::MachineStartGCodeEndMarker);

    // BBS: gcode writer doesn't know where the real position of extruder is after inserting custom gcode
    m_writer.set_current_position_clear(false);
    m_start_gcode_filament = GCodeProcessor::get_gcode_last_filament(machine_start_gcode);
    m_filament_in_nozzle.clear();
    if (m_start_gcode_filament < 0 && is_bbl_multi_extruder()) {
        // The Bambu two-extruder start G-code loads the first filament with
        // "T[initial_no_support_filament_id] H[initial_no_support_hotend]", which the plain parser above
        // does not read ("T4 H-1" is not a number). The last such T<n> (n < 255; T1000/T1001 are
        // Bambu's own tool commands) is the filament left in the hotend.
        std::istringstream lines(machine_start_gcode);
        for (std::string line; std::getline(lines, line);) {
            const size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos || line[b] != 'T')
                continue;
            size_t e = b + 1;
            while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e])))
                ++e;
            if (e == b + 1 || (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e])) && line[e] != ';'))
                continue;
            const int t = std::atoi(line.substr(b + 1, e - b - 1).c_str());
            if (t >= 0 && t < 255)
                m_start_gcode_filament = t;
        }
    }

    // Ultra (H2C 3MF schema): mark the first filament used in the print, the way BambuStudio does
    // (GCode.cpp: file.write_format(";VT%d H%d\n", initial_extruder_id, initial_nozzle_id)).
    // H is the logical nozzle id, or -1 when the plate has no dynamic nozzle map. Written only for
    // machines that carry a nozzle grouping (H2D/H2C/X2D); a single-nozzle machine has none, so
    // its g-code is unchanged.
    m_filament_change_sequence.clear();
    m_nozzle_change_sequence.clear();
    if (auto group_result = print.get_layered_nozzle_group_result()) {
        int initial_nozzle_id = -1;
        auto first_nozzle = group_result->get_first_nozzle_for_filament(int(initial_extruder_id));
        if (group_result->is_support_dynamic_nozzle_map() && first_nozzle)
            initial_nozzle_id = first_nozzle->group_id;
        m_filament_change_sequence.emplace_back(initial_extruder_id);
        m_nozzle_change_sequence.emplace_back(first_nozzle ? first_nozzle->group_id : 0);
        file.write_format(";VT%d H%d\n", int(initial_extruder_id), initial_nozzle_id);
    }

    // flush FanMover buffer to avoid modifying the start gcode if it's manual.
    if (!machine_start_gcode.empty() && this->m_fan_mover.get() != nullptr)
        file.write(this->m_fan_mover.get()->process_gcode("", true));

    // Process filament-specific gcode.
    /* if (has_wipe_tower) {
         // Wipe tower will control the extruder switching, it will call the filament_start_gcode.
     } else {
             DynamicConfig config;
             config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(initial_extruder_id)));
             file.writeln(this->placeholder_parser_process("filament_start_gcode",
     print.config().filament_start_gcode.values[initial_extruder_id], initial_extruder_id, &config));
     }
 */
    if (is_bbl_printers) {
        this->_print_first_layer_extruder_temperatures(file, print, machine_start_gcode, initial_extruder_id, true);
    }
    // Orca: when activate_air_filtration is set on any extruder, find and set the highest during_print_exhaust_fan_speed
    bool activate_air_filtration        = false;
    int  during_print_exhaust_fan_speed = 0;
    for (const auto& extruder : m_writer.extruders()) {
        activate_air_filtration |= m_config.activate_air_filtration.get_at(extruder.id());
        if (m_config.activate_air_filtration.get_at(extruder.id()))
            during_print_exhaust_fan_speed = std::max(during_print_exhaust_fan_speed,
                                                      m_config.during_print_exhaust_fan_speed.get_at(extruder.id()));
    }
    if (activate_air_filtration)
        file.write(m_writer.set_exhaust_fan(during_print_exhaust_fan_speed, true));

    print.throw_if_canceled();

    // Set other general things.
    file.write(this->preamble());

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
    print.throw_if_canceled();

    // Collect custom seam data from all objects.
    std::function<void(void)> throw_if_canceled_func = [&print]() { print.throw_if_canceled(); };
    m_seam_placer.init(print, throw_if_canceled_func);

    // BBS: get path for change filament
    if (m_writer.multiple_extruders) {
        std::vector<Vec2d> points = get_path_of_change_filament(print);
        if (points.size() == 3) {
            travel_point_1 = points[0];
            travel_point_2 = points[1];
            travel_point_3 = points[2];
        }
    }

    // Orca: support extruder priming
    if (is_bbl_printers || !(has_wipe_tower && print.config().single_extruder_multi_material_priming)) {
        // Set initial extruder only after custom start G-code.
        // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
        file.write(this->set_extruder(initial_extruder_id, 0.));
    }
    // BBS: set that indicates objs with brim
    for (auto iter = print.m_brimMap.begin(); iter != print.m_brimMap.end(); ++iter) {
        if (!iter->second.empty())
            this->m_objsWithBrim.insert(iter->first);
    }
    for (auto iter = print.m_supportBrimMap.begin(); iter != print.m_supportBrimMap.end(); ++iter) {
        if (!iter->second.empty())
            this->m_objSupportsWithBrim.insert(iter->first);
    }
    if (this->m_objsWithBrim.empty() && this->m_objSupportsWithBrim.empty())
        m_brim_done = true;

    // SoftFever: calib
    if (print.calib_params().mode == CalibMode::Calib_PA_Line) {
        std::string gcode;
        if ((print.default_object_config().outer_wall_acceleration.values.front() > 0 &&
             print.default_object_config().outer_wall_acceleration.values.front() > 0)) {
            gcode += m_writer.set_print_acceleration(
                (unsigned int) floor(print.default_object_config().outer_wall_acceleration.values.front() + 0.5));
        }

        if (print.default_object_config().outer_wall_jerk.values.front() > 0) {
            double jerk = print.default_object_config().outer_wall_jerk.values.front();
            gcode += m_writer.set_jerk_xy(jerk);
        }

        auto params = print.calib_params();

        CalibPressureAdvanceLine pa_test(this);

        auto fast_speed = CalibPressureAdvance::find_optimal_PA_speed(print.full_print_config(), pa_test.line_width(),
                                                                      pa_test.height_layer());
        auto slow_speed = std::max(10.0, fast_speed / 10.0);
        if (fast_speed < slow_speed + 5)
            fast_speed = slow_speed + 5;

        pa_test.set_speed(fast_speed, slow_speed);
        pa_test.draw_numbers() = print.calib_params().print_numbers;
        gcode += pa_test.generate_test(params.start, params.step, std::llround(std::ceil((params.end - params.start) / params.step)) + 1);

        file.write(gcode);
    } else {
        // BBS: open spaghetti detector
        if (is_bbl_printers) {
            // if (print.config().spaghetti_detector.value)
            file.write("M981 S1 P20000 ;open spaghetti detector\n");
        }

        // Do all objects for each layer.
        if (print.config().print_sequence == PrintSequence::ByObject && !has_wipe_tower) {
            size_t             finished_objects = 0;
            const PrintObject* prev_object      = (*print_object_instance_sequential_active)->print_object;
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end();
                 ++print_object_instance_sequential_active) {
                const PrintObject& object = *(*print_object_instance_sequential_active)->print_object;
                if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                    tool_ordering                = ToolOrdering(object, final_extruder_id);
                    unsigned int new_extruder_id = tool_ordering.first_extruder();
                    if (new_extruder_id == (unsigned int) -1)
                        // Skip this object.
                        continue;
                    initial_extruder_id = new_extruder_id;
                    final_extruder_id   = tool_ordering.last_extruder();
                    assert(final_extruder_id != (unsigned int) -1);
                }
                print.throw_if_canceled();
                this->set_origin(unscale((*print_object_instance_sequential_active)->shift));

                // BBS: prime extruder if extruder change happens before this object instance
                bool prime_extruder = false;
                if (finished_objects > 0) {
                    // Move to the origin position for the copy we're going to print.
                    // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
                    m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
                    m_avoid_crossing_perimeters.use_external_mp_once();
                    // BBS. change tool before moving to origin point.
                    if (m_writer.need_toolchange(initial_extruder_id)) {
                        const PrintObjectConfig& object_config              = object.config();
                        coordf_t                 initial_layer_print_height = print.config().initial_layer_print_height.value;
                        file.write(this->set_extruder(initial_extruder_id, initial_layer_print_height, true));
                        prime_extruder = true;
                    } else {
                        file.write(this->retract());
                    }
                    file.write(m_writer.travel_to_z(m_max_layer_z));
                    file.write(this->travel_to(Point(0, 0), erNone, "move to origin position for next object"));
                    m_enable_cooling_markers = true;
                    // Disable motion planner when traveling to first object point.
                    m_avoid_crossing_perimeters.disable_once();
                    // Ff we are printing the bottom layer of an object, and we have already finished
                    // another one, set first layer temperatures. This happens before the Z move
                    // is triggered, so machine has more time to reach such temperatures.
                    this->placeholder_parser().set("current_object_idx", int(finished_objects));
                    std::string printing_by_object_gcode = this->placeholder_parser_process("printing_by_object_gcode",
                                                                                            print.config().printing_by_object_gcode.value,
                                                                                            initial_extruder_id);
                    // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                    this->_print_first_layer_bed_temperature(file, print, printing_by_object_gcode, false);
                    this->_print_first_layer_extruder_temperatures(file, print, printing_by_object_gcode, initial_extruder_id, false);
                    file.writeln(printing_by_object_gcode);
                }
                // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
                m_cooling_buffer->reset(this->writer().get_position());
                m_cooling_buffer->set_current_extruder(initial_extruder_id);
                if (m_layer_time_speed_smoothing)
                    m_layer_time_speed_smoothing->reset();
                // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
                // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
                // and export G-code into file.
                this->process_layers(print, tool_ordering, collect_layers_to_print(object),
                                     *print_object_instance_sequential_active - object.instances().data(), file, prime_extruder);
                // BBS: close powerlost recovery
                {
                    if (is_bbl_printers && m_second_layer_things_done) {
                        file.write("; close powerlost recovery\n");
                        file.write("M1003 S0\n");
                    }
                }
                ++finished_objects;
                // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
                // Reset it when starting another object from 1st layer.
                m_second_layer_things_done = false;
                prev_object                = &object;
            }
        } else {
            // Sort layers by Z.
            // All extrusion moves with the same top layer height are extruded uninterrupted.
            std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> layers_to_print = collect_layers_to_print(print);
            // Prusa Multi-Material wipe tower.
            if (has_wipe_tower && !layers_to_print.empty()) {
                m_wipe_tower.reset(new WipeTowerIntegration(print.config(), print.get_plate_index(), print.get_plate_origin(),
                                                            *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes,
                                                            print.wipe_tower_data().local_z_tool_changes,
                                                            print.wipe_tower_data().local_z_reserve_boxes,
                                                            *print.wipe_tower_data().final_purge.get()));
                // BBS
                file.write(m_writer.travel_to_z(initial_layer_print_height + m_config.z_offset.value, "Move to the first layer height"));

                if (!is_bbl_printers && print.config().single_extruder_multi_material_priming) {
                    file.write(m_wipe_tower->prime(*this));
                    // Verify, whether the print overaps the priming extrusions.
                    BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                    coordf_t     twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first +
                                                EPSILON;
                    for (const PrintObject* print_object : print.objects())
                        bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                    bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                    BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                    bbox_prime.offset(0.5f);
                    bool overlap = bbox_prime.overlap(bbox_print);

                    if (print.config().gcode_flavor == gcfMarlinLegacy || print.config().gcode_flavor == gcfMarlinFirmware) {
                        file.write(this->retract());
                        file.write("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                        if (overlap) {
                            // Wait for the user to remove the priming extrusions.
                            file.write("M1 Remove priming towers and click button.\n");
                        } else {
                            // Just wait for a bit to let the user check, that the priming succeeded.
                            // TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                            file.write("M1 S10\n");
                        }
                    } else {
                        // This is not Marlin, M1 command is probably not supported.
                        if (overlap) {
                            print.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                                                          _(L("Your print is very close to the priming regions. "
                                                              "Make sure there is no collision.")));
                        } else {
                            // Just continue printing, no action necessary.
                        }
                    }
                }
                print.throw_if_canceled();
            }
            // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
            // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
            // and export G-code into file.
            this->process_layers(print, tool_ordering, print_object_instances_ordering, layers_to_print, file);
            // BBS: close powerlost recovery
            {
                if (is_bbl_printers && m_second_layer_things_done) {
                    file.write("; close powerlost recovery\n");
                    file.write("M1003 S0\n");
                }
            }
            if (m_wipe_tower)
                // Purge the extruder, pull out the active filament.
                file.write(m_wipe_tower->finalize(*this));
        }
    }
    // BBS: the last retraction
    //  Write end commands to file.
    file.write(this->retract(false, true));

    // if needed, write the gcode_label_objects_end
    {
        std::string gcode;
        m_writer.add_object_change_labels(gcode);
        file.write(gcode);
    }

    file.write(m_writer.set_fan(0));
    // BBS: make sure the additional fan is closed when end
    if (m_config.auxiliary_fan.value)
        file.write(m_writer.set_additional_fan(0));
    if (is_bbl_printers) {
        // BBS: close spaghetti detector
        // Note: M981 is also used to tell xcam the last layer is finished, so we need always send it even if spaghetti option is disabled.
        // if (print.config().spaghetti_detector.value)
        file.write("M981 S0 P20000 ; close spaghetti detector\n");
    }

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                      ExtrusionEntity::role_to_string(erCustom).c_str());
    // BBS: mark the start of the machine end G-code for the idle-nozzle pre-cooling (BambuStudio GCode.cpp:3356).
    if (m_pre_cooling_markers)
        file.write_format(";%s\n", PreCooling::MachineEndGCodeStartMarker);

    // Process filament-specific gcode in extruder order.
    {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        // BBS
        config.set_key_value("layer_z", new ConfigOptionFloat(m_writer.get_position()(2) - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        // Ultra (H2C nozzle rack): ported from BambuStudio GCode.cpp:3370-3387. The H2 machine_end_gcode
        // pulls the loaded filament back to the AMS with "M620.11 P1 I[current_filament_id] ...". Without
        // current_filament_id in this scope it fell through to the global single-mapped shim (0), so the
        // job asked the AMS to retract slot 1 instead of the slot that is actually loaded.
        const int active_filament_id = m_writer.extruder() ? int(m_writer.extruder()->id()) : 0;
        if (print.config().single_extruder_multi_material) {
            // Process the filament_end_gcode for the active filament only.
            int extruder_id = m_writer.extruder()->id();
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
            config.set_key_value("current_filament_id", new ConfigOptionInt(extruder_id));
            file.writeln(this->placeholder_parser_process("filament_end_gcode", print.config().filament_end_gcode.get_at(extruder_id),
                                                          extruder_id, &config));
        } else {
            for (const std::string& end_gcode : print.config().filament_end_gcode.values) {
                int extruder_id = (unsigned int) (&end_gcode - &print.config().filament_end_gcode.values.front());
                config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                config.set_key_value("current_filament_id", new ConfigOptionInt(extruder_id));
                file.writeln(this->placeholder_parser_process("filament_end_gcode", end_gcode, extruder_id, &config));
            }
        }
        // machine_end_gcode runs for the filament that is actually loaded, not the last one iterated.
        config.set_key_value("current_filament_id", new ConfigOptionInt(active_filament_id));
        file.writeln(
            this->placeholder_parser_process("machine_end_gcode", print.config().machine_end_gcode, m_writer.extruder()->id(), &config));
    }
    file.write(m_writer.update_progress(m_layer_count, m_layer_count, true)); // 100%
    file.write(m_writer.postamble());

    if (activate_chamber_temp_control && max_chamber_temp > 0)
        file.write(m_writer.set_chamber_temperature(0, false)); // close chamber_temperature

    if (activate_air_filtration) {
        int complete_print_exhaust_fan_speed = 0;
        for (const auto& extruder : m_writer.extruders())
            if (m_config.activate_air_filtration.get_at(extruder.id()))
                complete_print_exhaust_fan_speed = std::max(complete_print_exhaust_fan_speed,
                                                            m_config.complete_print_exhaust_fan_speed.get_at(extruder.id()));
        file.write(m_writer.set_exhaust_fan(complete_print_exhaust_fan_speed, true));
    }
    // adds tags for time estimators
    file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());
    file.write_format("; EXECUTABLE_BLOCK_END\n\n");

    print.throw_if_canceled();

    // Get filament stats.
    file.write(DoExport::update_print_stats_and_format_filament_stats(
        // Const inputs
        has_wipe_tower, print.wipe_tower_data(), m_writer.extruders(),
        // Modifies
        print.m_print_statistics));
    print.m_print_statistics.initial_tool = initial_extruder_id;
    if (!is_bbl_printers) {
        // CONFIG_BLOCK first, time estimate after: some firmwares only scan the last N lines for
        // "estimated printing time", and a large config could push an estimate written before it
        // out of that window.
        file.write("; CONFIG_BLOCK_START\n");
        std::string full_config;
        append_full_config(print, full_config);
        if (!full_config.empty())
            file.write(full_config);

        // SoftFever: write compatiple info
        int first_layer_bed_temperature = get_bed_temperature_max(print, true);
        file.write_format("; first_layer_bed_temperature = %d\n", first_layer_bed_temperature);
        file.write_format("; bed_shape = %s\n", print.full_print_config().opt_serialize("printable_area").c_str());
        file.write_format("; first_layer_temperature = %d\n", print.config().nozzle_temperature_initial_layer.get_at(0));
        file.write_format("; first_layer_height = %.3f\n", print.config().initial_layer_print_height.value);

        // SF TODO
        //      file.write_format("; variable_layer_height = %d\n", print.ad.adaptive_layer_height ? 1 : 0);

        file.write("; CONFIG_BLOCK_END\n\n");

        file.write_format("; total filament used [g] = %.2lf\n", print.m_print_statistics.total_weight);
        file.write_format("; total filament cost = %.2lf\n", print.m_print_statistics.total_cost);
        if (print.m_print_statistics.total_toolchanges > 0)
            file.write_format("; total filament change = %i\n", print.m_print_statistics.total_toolchanges);
        file.write_format("; total layers count = %i\n", m_layer_count);
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());
        file.write("\n");
    }
    file.write("\n");

    print.throw_if_canceled();
}

// BBS
void GCode::check_placeholder_parser_failed()
{
    if (!m_placeholder_parser_integration.failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        std::string msg = Slic3r::format(_(L("Failed to generate G-code for invalid custom G-code.\n\n")));
        for (const auto& name_and_error : m_placeholder_parser_integration.failed_templates)
            msg += name_and_error.first + " " + name_and_error.second + "\n";
        msg += Slic3r::format(_(L("Please check the custom G-code or use the default custom G-code.")));
        throw Slic3r::PlaceholderParserError(msg);
    }
}

// BBL timelapse (BambuStudio ToolOrdering::calc_most_used_extruder): on a dual-nozzle machine the
// extruder that prints on the most layers takes the timelapse photo. 0 when the plate has a single
// extruder or no nozzle grouping result.
static int timelapse_most_used_extruder(const Print &print, const ToolOrdering &tool_ordering)
{
    const int num_extruders = int(print.config().nozzle_diameter.size());
    auto      group_result  = print.get_layered_nozzle_group_result();
    if (num_extruders < 2 || !group_result)
        return 0;
    std::vector<int> layers_used(num_extruders, 0);
    int              layer_idx = 0;
    for (const LayerTools &layer_tools : tool_ordering) {
        std::set<int> used;
        for (unsigned int filament : layer_tools.extruders) {
            const int extruder = group_result->get_extruder_id(int(filament), layer_idx);
            if (extruder >= 0 && extruder < num_extruders)
                used.insert(extruder);
        }
        for (int extruder : used)
            ++layers_used[extruder];
        ++layer_idx;
    }
    int most_used = 0;
    for (int extruder = 1; extruder < num_extruders; ++extruder)
        if (layers_used[extruder] >= layers_used[most_used])
            most_used = extruder;
    return most_used;
}

int GCode::timelapse_extruder_of_filament(int filament_id) const
{
    auto group_result = m_curr_print ? m_curr_print->get_layered_nozzle_group_result() : nullptr;
    if (!group_result || filament_id < 0)
        return 0;
    const int extruder = group_result->get_extruder_id(filament_id, m_layer_index);
    return extruder < 0 ? 0 : extruder;
}

int GCode::timelapse_physical_extruder(int extruder_id) const
{
    const ConfigOptionInts &map = m_config.physical_extruder_map;
    return (extruder_id >= 0 && extruder_id < int(map.values.size())) ? map.get_at(extruder_id) : extruder_id;
}

// Process all layers of all objects (non-sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(const Print&                                                       print,
                           const ToolOrdering&                                                tool_ordering,
                           const std::vector<const PrintInstance*>&                           print_object_instances_ordering,
                           const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>& layers_to_print,
                           GCodeOutputStream&                                                 output_stream)
{
    m_timelapse_photo_extruder = timelapse_most_used_extruder(print, tool_ordering);
    // The pipeline is variable: The vase mode filter is optional.
    size_t     layer_to_print_idx = 0;
    const auto generator          = tbb::make_filter<void, LayerResult>(
        slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &tool_ordering, &print_object_instances_ordering, &layers_to_print,
         &layer_to_print_idx](tbb::flow_control& fc) -> LayerResult {
            if (layer_to_print_idx >= layers_to_print.size()) {
                if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                    fc.stop();
                    return {};
                } else {
                    // Pressure equalizer need insert empty input. Because it returns one layer back.
                    // Insert NOP (no operation) layer;
                    ++layer_to_print_idx;
                    return LayerResult::make_nop_layer_result();
                }
            } else {
                const std::pair<coordf_t, std::vector<LayerToPrint>>& layer       = layers_to_print[layer_to_print_idx++];
                const LayerTools&                                     layer_tools = tool_ordering.tools_for_layer(layer.first);
                print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")), std::to_string(layer_to_print_idx)));
                if (m_wipe_tower && layer_tools.has_wipe_tower)
                    m_wipe_tower->next_layer();
                // BBS
                check_placeholder_parser_failed();
                print.throw_if_canceled();
                return this->process_layer(print, layer.second, layer_tools, &layer == &layers_to_print.back(),
                                                    &print_object_instances_ordering, size_t(-1));
            }
        });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<
        LayerResult,
        LayerResult>(slic3r_tbb_filtermode::serial_in_order, [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in) -> LayerResult {
        if (in.nop_layer_result)
            return in;

        spiral_mode.enable(in.spiral_vase_enable);
        bool last_layer = in.layer_id == layers_to_print.size() - 1;
        LayerResult out{spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush};
        out.last_layer = in.last_layer;
        return out;
    });
    const auto pressure_equalizer  = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
                                                                               [pressure_equalizer = this->m_pressure_equalizer.get()](
                                                                                   LayerResult in) -> LayerResult {
                                                                                   return pressure_equalizer->process_layer(std::move(in));
                                                                               });
    const auto cooling             = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
                                                                    [&cooling_buffer = *this->m_cooling_buffer.get()](
                                                                        LayerResult in) -> LayerResult {
                                                                        if (in.nop_layer_result)
                                                                            return in;
                                                                        in.gcode = cooling_buffer.process_layer(std::move(in.gcode),
                                                                                                                        in.layer_id,
                                                                                                                        in.cooling_buffer_flush);
                                                                        // Only a flushed layer carries G-code; the flag describes that flush.
                                                                        in.cooling_slowed_down = in.cooling_buffer_flush && cooling_buffer.last_layer_slowed_down();
                                                                        return in;
                                                                    });
    // After CoolingBuffer, before FanMover. Null pointer => identity, no extra layer buffer.
    // Enabled modes buffer every cooled layer and rewrite F on last_layer.
    const auto layer_time_speed_smoothing = tbb::make_filter<LayerResult, std::string>(
        slic3r_tbb_filtermode::serial_in_order,
        [&ltss = this->m_layer_time_speed_smoothing](LayerResult in) -> std::string {
            if (!ltss)
                return std::move(in.gcode);
            return ltss->process_layer(std::move(in.gcode), in.layer_id, in.last_layer, in.cooling_slowed_down);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
                                                                                [&pa_processor = *this->m_pa_processor](
                                                                                    std::string in) -> std::string {
                                                                                    return pa_processor.process_layer(std::move(in));
                                                                                });

    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
                                                            [&output_stream](std::string s) { output_stream.write(s); });

    const auto fan_mover = tbb::make_filter<std::string, std::string>(
        slic3r_tbb_filtermode::serial_in_order,
        [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in) -> std::string {
            CNumericLocalesSetter locales_setter;

            if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
                if (fan_mover.get() == nullptr)
                    fan_mover.reset(new Slic3r::FanMover(writer, std::abs((float) config.fan_speedup_time.value),
                                                         config.fan_speedup_time.value > 0, config.use_relative_e_distances.value,
                                                         config.fan_speedup_overhangs.value, (float) config.fan_kickstart.value));
                // flush as it's a whole layer
                return fan_mover->process_gcode(in, true);
            }
            return in;
        });

    // The pipeline elements are joined using const references, thus no copying is performed.
    // Layer-time speed smoothing sits after CoolingBuffer and before FanMover in every variant.
    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & spiral_mode & pressure_equalizer & cooling & layer_time_speed_smoothing & fan_mover &
                                       output);
    else if (m_spiral_vase)
        tbb::parallel_pipeline(12, generator & spiral_mode & cooling & layer_time_speed_smoothing & fan_mover & output);
    else if (m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & pressure_equalizer & cooling & layer_time_speed_smoothing & fan_mover &
                                       pa_processor_filter & output);
    else
        tbb::parallel_pipeline(12, generator & cooling & layer_time_speed_smoothing & fan_mover & pa_processor_filter & output);
}

// Process all layers of a single object instance (sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCode::process_layers(const Print&              print,
                           const ToolOrdering&       tool_ordering,
                           std::vector<LayerToPrint> layers_to_print,
                           const size_t              single_object_idx,
                           GCodeOutputStream&        output_stream,
                           // BBS
                           const bool prime_extruder)
{
    m_timelapse_photo_extruder = timelapse_most_used_extruder(print, tool_ordering);
    // The pipeline is variable: The vase mode filter is optional.
    size_t     layer_to_print_idx = 0;
    const auto generator =
        tbb::make_filter<void, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
                                            [this, &print, &tool_ordering, &layers_to_print, &layer_to_print_idx, single_object_idx,
                                             prime_extruder](tbb::flow_control& fc) -> LayerResult {
                                                if (layer_to_print_idx >= layers_to_print.size()) {
                                                    if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                                                        fc.stop();
                                                        return {};
                                                    } else {
                                                        // Pressure equalizer need insert empty input. Because it returns one layer back.
                                                        // Insert NOP (no operation) layer;
                                                        ++layer_to_print_idx;
                                                        return LayerResult::make_nop_layer_result();
                                                    }
                                                } else {
                                                    LayerToPrint& layer = layers_to_print[layer_to_print_idx++];
                                                    print.set_status(80, Slic3r::format(_(L("Generating G-code: layer %1%")),
                                                                                        std::to_string(layer_to_print_idx)));
                                                    // BBS
                                                    check_placeholder_parser_failed();
                                                    print.throw_if_canceled();
                                                    return this->process_layer(print, {std::move(layer)},
                                                                               tool_ordering.tools_for_layer(layer.print_z()),
                                                                               &layer == &layers_to_print.back(), nullptr,
                                                                               single_object_idx, prime_extruder);
                                                }
                                            });
    if (m_spiral_vase) {
        float nozzle_diameter  = EXTRUDER_CONFIG(nozzle_diameter);
        float max_xy_smoothing = m_config.get_abs_value("spiral_mode_max_xy_smoothing", nozzle_diameter);
        this->m_spiral_vase->set_max_xy_smoothing(max_xy_smoothing);
    }
    const auto spiral_mode = tbb::make_filter<
        LayerResult,
        LayerResult>(slic3r_tbb_filtermode::serial_in_order, [&spiral_mode = *this->m_spiral_vase.get(), &layers_to_print](LayerResult in) -> LayerResult {
        if (in.nop_layer_result)
            return in;
        spiral_mode.enable(in.spiral_vase_enable);
        bool last_layer = in.layer_id == layers_to_print.size() - 1;
        LayerResult out{spiral_mode.process_layer(std::move(in.gcode), last_layer), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush};
        out.last_layer = in.last_layer;
        return out;
    });
    const auto pressure_equalizer  = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
                                                                               [pressure_equalizer = this->m_pressure_equalizer.get()](
                                                                                   LayerResult in) -> LayerResult {
                                                                                   return pressure_equalizer->process_layer(std::move(in));
                                                                               });
    const auto cooling             = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
                                                                    [&cooling_buffer = *this->m_cooling_buffer.get()](
                                                                        LayerResult in) -> LayerResult {
                                                                        if (in.nop_layer_result)
                                                                            return in;
                                                                        in.gcode = cooling_buffer.process_layer(std::move(in.gcode),
                                                                                                                        in.layer_id,
                                                                                                                        in.cooling_buffer_flush);
                                                                        // Only a flushed layer carries G-code; the flag describes that flush.
                                                                        in.cooling_slowed_down = in.cooling_buffer_flush && cooling_buffer.last_layer_slowed_down();
                                                                        return in;
                                                                    });
    // After CoolingBuffer, before FanMover. Null pointer => identity, no extra layer buffer.
    // Enabled modes buffer every cooled layer and rewrite F on last_layer.
    const auto layer_time_speed_smoothing = tbb::make_filter<LayerResult, std::string>(
        slic3r_tbb_filtermode::serial_in_order,
        [&ltss = this->m_layer_time_speed_smoothing](LayerResult in) -> std::string {
            if (!ltss)
                return std::move(in.gcode);
            return ltss->process_layer(std::move(in.gcode), in.layer_id, in.last_layer, in.cooling_slowed_down);
        });
    const auto pa_processor_filter = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
                                                                                [&pa_processor = *this->m_pa_processor](
                                                                                    std::string in) -> std::string {
                                                                                    return pa_processor.process_layer(std::move(in));
                                                                                });

    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
                                                            [&output_stream](std::string s) { output_stream.write(s); });

    const auto fan_mover = tbb::make_filter<std::string, std::string>(
        slic3r_tbb_filtermode::serial_in_order,
        [&fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in) -> std::string {
            if (config.fan_speedup_time.value != 0 || config.fan_kickstart.value > 0) {
                if (fan_mover.get() == nullptr)
                    fan_mover.reset(new Slic3r::FanMover(writer, std::abs((float) config.fan_speedup_time.value),
                                                         config.fan_speedup_time.value > 0, config.use_relative_e_distances.value,
                                                         config.fan_speedup_overhangs.value, (float) config.fan_kickstart.value));
                // flush as it's a whole layer
                return fan_mover->process_gcode(in, true);
            }
            return in;
        });

    // The pipeline elements are joined using const references, thus no copying is performed.
    // Layer-time speed smoothing sits after CoolingBuffer and before FanMover in every variant.
    if (m_spiral_vase && m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & spiral_mode & pressure_equalizer & cooling & layer_time_speed_smoothing & fan_mover &
                                       output);
    else if (m_spiral_vase)
        tbb::parallel_pipeline(12, generator & spiral_mode & cooling & layer_time_speed_smoothing & fan_mover & output);
    else if (m_pressure_equalizer)
        tbb::parallel_pipeline(12, generator & pressure_equalizer & cooling & layer_time_speed_smoothing & fan_mover &
                                       pa_processor_filter & output);
    else
        tbb::parallel_pipeline(12, generator & cooling & layer_time_speed_smoothing & fan_mover & pa_processor_filter & output);
}


DynamicConfig GCode::build_placeholder_process_config(unsigned int current_extruder_id,
                                                       const DynamicConfig *config_override) const
{
    DynamicConfig process_config_override;
    const size_t process_config_idx = get_config_idx(m_config, ConfigFlowDomain::Process, current_extruder_id);
    for (const std::string &key : process_flow_variant_options()) {
        const ConfigOption *option = m_config.optptr(key);
        if (option == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "Missing process flow option: " << key;
            continue;
        }

        switch (option->type()) {
        case coFloats:
            process_config_override.set_key_value(
                key, new ConfigOptionFloat(static_cast<const ConfigOptionFloats *>(option)->get_at(process_config_idx)));
            break;
        case coFloatsOrPercents:
        {
            const FloatOrPercent value = static_cast<const ConfigOptionFloatsOrPercents *>(option)->get_at(process_config_idx);
            process_config_override.set_key_value(key, new ConfigOptionFloatOrPercent(value.value, value.percent));
            break;
        }
        case coBools:
            process_config_override.set_key_value(
                key, new ConfigOptionBool(static_cast<const ConfigOptionBools *>(option)->get_at(process_config_idx)));
            break;
        case coInts:
            process_config_override.set_key_value(
                key, new ConfigOptionInt(static_cast<const ConfigOptionInts *>(option)->get_at(process_config_idx)));
            break;
        case coPercents:
            process_config_override.set_key_value(
                key, new ConfigOptionPercent(static_cast<const ConfigOptionPercents *>(option)->get_at(process_config_idx)));
            break;
        default:
            BOOST_LOG_TRIVIAL(error) << "Invalid process flow option type for: " << key;
            continue;
        }
    }

    if (config_override != nullptr)
        process_config_override += *config_override;

    return process_config_override;
}

std::string GCode::placeholder_parser_process(const std::string&   name,
                                              const std::string&   templ,
                                              unsigned int         current_extruder_id,
                                              const DynamicConfig* config_override)
{
    // Orca: Added CMake config option since debug is rarely used in current workflow.
    // Also changed from throwing error immediately to storing messages till slicing is completed
    // to raise all errors at the same time.
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    if (config_override) {
        const auto& custom_gcode_placeholders = custom_gcode_specific_placeholders();

        // 1-st check: custom G-code "name" have to be present in s_CustomGcodeSpecificPlaceholders;
        // if (custom_gcode_placeholders.count(name) > 0) {
        //    const auto& placeholders = custom_gcode_placeholders.at(name);
        if (auto it = custom_gcode_placeholders.find(name); it != custom_gcode_placeholders.end()) {
            const auto& placeholders = it->second;

            for (const std::string& key : config_override->keys()) {
                // 2-nd check: "key" have to be present in s_CustomGcodeSpecificPlaceholders for "name" custom G-code ;
                if (std::find(placeholders.begin(), placeholders.end(), key) == placeholders.end()) {
                    auto& vector =
                        m_placeholder_error_messages[name +
                                                     " - option not specified for custom gcode type (s_CustomGcodeSpecificPlaceholders)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
                // 3-rd check: "key" have to be present in CustomGcodeSpecificConfigDef for "key" placeholder;
                if (!custom_gcode_specific_config_def.has(key)) {
                    auto& vector = m_placeholder_error_messages[name + " - option has no definition (CustomGcodeSpecificConfigDef)"];
                    if (std::find(vector.begin(), vector.end(), key) == vector.end())
                        vector.emplace_back(key);
                }
            }
        } else {
            auto& vector = m_placeholder_error_messages[name + " - gcode type not found in s_CustomGcodeSpecificPlaceholders"];
            if (vector.empty())
                vector.emplace_back("");
        }
    }
#endif

    PlaceholderParserIntegration& ppi = m_placeholder_parser_integration;
    DynamicConfig process_config_override =
        this->build_placeholder_process_config(current_extruder_id, config_override);

    try {
        ppi.update_from_gcodewriter(m_writer);
        std::string output = ppi.parser.process(templ, current_extruder_id, &process_config_override, &ppi.output_config, &ppi.context);
        ppi.validate_output_vector_variables();

        if (const std::vector<double>& pos = ppi.opt_position->values; ppi.position != pos) {
            // Update G-code writer.
            m_writer.set_position({pos[0], pos[1], pos[2]});
            this->set_last_pos(this->gcode_to_point({pos[0], pos[1]}));
        }

        for (const Extruder& e : m_writer.extruders()) {
            unsigned int eid = e.id();
            assert(eid < ppi.num_extruders);
            if (eid < ppi.num_extruders) {
                if (!m_writer.config.use_relative_e_distances && !is_approx(ppi.e_position[eid], ppi.opt_e_position->values[eid]))
                    const_cast<Extruder&>(e).set_position(ppi.opt_e_position->values[eid]);
                if (!is_approx(ppi.e_retracted[eid], ppi.opt_e_retracted->values[eid]) ||
                    !is_approx(ppi.e_restart_extra[eid], ppi.opt_e_restart_extra->values[eid]))
                    const_cast<Extruder&>(e).set_retracted(ppi.opt_e_retracted->values[eid], ppi.opt_e_restart_extra->values[eid]);
            }
        }

        return output;
    } catch (std::runtime_error& err) {
        // Collect the names of failed template substitutions for error reporting.
        auto it = ppi.failed_templates.find(name);
        if (it == ppi.failed_templates.end())
            // Only if there was no error reported for this template, store the first error message into the map to be reported.
            // We don't want to collect error message for each and every occurence of a single custom G-code section.
            ppi.failed_templates.insert(it, std::make_pair(name, std::string(err.what())));
        // Insert the macro error message into the G-code.
        return std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" + err.what() +
               "!!!!! End of an error report for the custom G-code template " + name + "\n\n";
    }
}

// Parse the custom G-code, try to find mcode_set_temp_dont_wait and mcode_set_temp_and_wait or optionally G10 with temperature inside the
// custom G-code. Returns true if one of the temp commands are found, and try to parse the target temperature value into temp_out.
static bool custom_gcode_sets_temperature(
    const std::string& gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int& temp_out)
{
    temp_out = -1;
    if (gcode.empty())
        return false;

    const char* ptr               = gcode.data();
    bool        temp_set_by_gcode = false;
    while (*ptr != 0) {
        // Skip whitespaces.
        for (; *ptr == ' ' || *ptr == '\t'; ++ptr)
            ;
        if (*ptr == 'M' ||                  // Line starts with 'M'. It is a machine command.
            (*ptr == 'G' && include_g10)) { // Only check for G10 if requested
            bool is_gcode = *ptr == 'G';
            ++ptr;
            // Parse the M or G code value.
            char* endptr = nullptr;
            int   mgcode = int(strtol(ptr, &endptr, 10));
            if (endptr != nullptr && endptr != ptr && is_gcode ?
                    // G10 found
                    mgcode == 10 :
                    // M104/M109 or M140/M190 found.
                    (mgcode == mcode_set_temp_dont_wait || mgcode == mcode_set_temp_and_wait)) {
                ptr = endptr;
                if (!is_gcode)
                    // Let the caller know that the custom M-code sets the temperature.
                    temp_set_by_gcode = true;
                // Now try to parse the temperature value.
                // While not at the end of the line:
                while (strchr(";\r\n\0", *ptr) == nullptr) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ptr)
                        ;
                    if (*ptr == 'S') {
                        // Skip whitespaces.
                        for (++ptr; *ptr == ' ' || *ptr == '\t'; ++ptr)
                            ;
                        // Parse an int.
                        endptr           = nullptr;
                        long temp_parsed = strtol(ptr, &endptr, 10);
                        if (endptr > ptr) {
                            ptr      = endptr;
                            temp_out = temp_parsed;
                            // Let the caller know that the custom G-code sets the temperature
                            // Only do this after successfully parsing temperature since G10
                            // can be used for other reasons
                            temp_set_by_gcode = true;
                        }
                    } else {
                        // Skip this word.
                        for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ptr)
                            ;
                    }
                }
            }
        }
        // Skip the rest of the line.
        for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ptr)
            ;
        // Skip the end of line indicators.
        for (; *ptr == '\r' || *ptr == '\n'; ++ptr)
            ;
    }
    return temp_set_by_gcode;
}

// Print the machine envelope G-code for the Marlin firmware based on the "machine_max_xxx" parameters.
// Do not process this piece of G-code by the time estimator, it already knows the values through another sources.
void GCode::print_machine_envelope(GCodeOutputStream& file, Print& print)
{
    const auto flavor = print.config().gcode_flavor.value;
    if ((flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfRepRapFirmware) &&
        print.config().emit_machine_limits_to_gcode.value == true) {
        int factor = flavor == gcfRepRapFirmware ? 60 : 1; // RRF M203 and M566 are in mm/min
        file.write_format("M201 X%d Y%d Z%d E%d\n", int(print.config().machine_max_acceleration_x.values.front() + 0.5),
                          int(print.config().machine_max_acceleration_y.values.front() + 0.5),
                          int(print.config().machine_max_acceleration_z.values.front() + 0.5),
                          int(print.config().machine_max_acceleration_e.values.front() + 0.5));
        file.write_format("M203 X%d Y%d Z%d E%d\n", int(print.config().machine_max_speed_x.values.front() * factor + 0.5),
                          int(print.config().machine_max_speed_y.values.front() * factor + 0.5),
                          int(print.config().machine_max_speed_z.values.front() * factor + 0.5),
                          int(print.config().machine_max_speed_e.values.front() * factor + 0.5));

        // Now M204 - acceleration. This one is quite hairy thanks to how Marlin guys care about
        // Legacy Marlin should export travel acceleration the same as printing acceleration.
        // MarlinFirmware has the two separated.
        int travel_acc = flavor == gcfMarlinLegacy ? int(print.config().machine_max_acceleration_extruding.values.front() + 0.5) :
                                                     int(print.config().machine_max_acceleration_travel.values.front() + 0.5);
        if (flavor == gcfRepRapFirmware)
            file.write_format("M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                              int(print.config().machine_max_acceleration_extruding.values.front() + 0.5), travel_acc);
        else if (flavor == gcfMarlinFirmware)
            // New Marlin uses M204 P[print] R[retract] T[travel]
            file.write_format("M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                              int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                              int(print.config().machine_max_acceleration_retracting.values.front() + 0.5),
                              int(print.config().machine_max_acceleration_travel.values.front() + 0.5));
        else
            file.write_format("M204 P%d R%d T%d\n", int(print.config().machine_max_acceleration_extruding.values.front() + 0.5),
                              int(print.config().machine_max_acceleration_retracting.values.front() + 0.5), travel_acc);

        assert(is_decimal_separator_point());
        file.write_format(flavor == gcfRepRapFirmware ? "M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/min\n" :
                                                        "M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
                          print.config().machine_max_jerk_x.values.front() * factor,
                          print.config().machine_max_jerk_y.values.front() * factor,
                          print.config().machine_max_jerk_z.values.front() * factor,
                          print.config().machine_max_jerk_e.values.front() * factor);

        // New Marlin uses M205 J[mm] for junction deviation (only apply if it is > 0)
        file.write_format(writer().set_junction_deviation(config().machine_max_junction_deviation.values.front()).c_str());
    }
}

// BBS
int GCode::get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const
{
    // Fall back to PEI temps for bed types without dedicated temp options (e.g. btDefault),
    // mirroring the placeholder setup in _do_export.
    std::string bed_temp_key = is_first_layer ? get_bed_temp_1st_layer_key(bed_type) : get_bed_temp_key(bed_type);
    if (bed_temp_key.empty())
        bed_temp_key = is_first_layer ? get_bed_temp_1st_layer_key(btPEI) : get_bed_temp_key(btPEI);
    const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(bed_temp_key);
    return bed_temp_opt->get_at(extruder_id);
}

// BBS
// Max bed temperature over all used extruders (object/support/wipe tower/brim-introduced extruders),
// so a compatible mixed print (e.g. PLA 65C + TPU 35C) always runs the highest bed temperature.
// A value of 0 means the filament does not support the current bed type, and is naturally skipped by max().
int GCode::get_bed_temperature_max(const Print& print, const bool is_first_layer) const
{
    int max_bed_temp = 0;
    for (const unsigned int extruder_id : print.extruders())
        max_bed_temp = std::max(
            max_bed_temp, get_bed_temperature(extruder_id, is_first_layer, print.config().curr_bed_type));
    return max_bed_temp;
}

// Write 1st layer bed temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M140 - Set Extruder Temperature
// M190 - Set Extruder Temperature and Wait
void GCode::_print_first_layer_bed_temperature(
    GCodeOutputStream& file, Print& print, const std::string& gcode, bool wait)
{
    // BBS: bed temperature accommodates the highest-temperature filament of the print
    // (max over all used extruders instead of the first printing extruder only).
    int bed_temp = get_bed_temperature_max(print, true);

    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);

    // Always call m_writer.set_bed_temperature() so it will set the internal "current" state of the bed temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_bed_temperature(bed_temp, wait);
    if (!temp_set_by_gcode)
        file.write(set_temp_gcode);
}

// Write 1st layer extruder temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M104 - Set Extruder Temperature
// M109 - Set Extruder Temperature and Wait
// RepRapFirmware: G10 Sxx
void GCode::_print_first_layer_extruder_temperatures(
    GCodeOutputStream& file, Print& print, const std::string& gcode, unsigned int first_printing_extruder_id, bool wait)
{
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode = -1;
    bool include_g10   = print.config().gcode_flavor == gcfRepRapFirmware;
    if (custom_gcode_sets_temperature(gcode, 104, 109, include_g10, temp_by_gcode)) {
        // Set the extruder temperature at m_writer, but throw away the generated G-code as it will be written with the custom G-code.
        int temp = print.config().nozzle_temperature_initial_layer.get_at(first_printing_extruder_id);
        if (temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else {
        // Custom G-code does not set the extruder temperature. Do it now.
        if (print.config().single_extruder_multi_material.value) {
            // Set temperature of the first printing extruder only.
            int temp = print.config().nozzle_temperature_initial_layer.get_at(first_printing_extruder_id);
            if (temp > 0)
                file.write(m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        } else if (is_bbl_multi_extruder()) {
            // Bambu two-extruder printer (reached between objects of a print-by-object plate). M104 T is
            // a PHYSICAL hotend: "M104 S<t> T<filament index>" named a hotend that does not exist for
            // filament slots >= 2 (e.g. "M104 S220 T6"), and the wrong hotend for slots 0/1.
            // Only the hotend that prints the next object is set, to its first filament's first-layer
            // temperature. The other hotend is idle: setting it to print temperature here heated a
            // hotend the idle-nozzle pre-cooling had just cooled, for the whole object (owner's H2C,
            // 2026-09-24). It is heated again by its own tool change ("M620.10 A1 ... P<temp>") or,
            // with pre-cooling, by the pre-heat timed for its next use.
            const int active_tool = temperature_tool_for_filament(int(first_printing_extruder_id));
            const int active_temp = print.config().nozzle_temperature_initial_layer.get_at(first_printing_extruder_id);
            if (active_temp > 0)
                file.write(m_writer.set_temperature(active_temp, wait, active_tool));
        } else {
            // Set temperatures of all the printing extruders.
            bool is_active   = true;
            int  target_temp = -1;
            int  target_tool = -1;
            for (unsigned int tool_id : print.extruders()) {
                is_active = true;
                int temp  = print.config().nozzle_temperature_initial_layer.get_at(tool_id);
                if (print.config().ooze_prevention.value && tool_id != first_printing_extruder_id) {
                    is_active = false;
                    if (print.config().idle_temperature.get_at(tool_id) == 0)
                        temp += print.config().standby_temperature_delta.value;
                    else
                        temp = print.config().idle_temperature.get_at(tool_id);
                }
                if (temp > 0) {
                    if (is_active) {
                        target_temp = temp;
                        target_tool = tool_id;
                    } else
                        file.write(m_writer.set_temperature(temp, wait, tool_id));
                }
            }
            if (target_temp != -1 && target_tool != -1) {
                file.write(m_writer.set_temperature(target_temp, wait, target_tool));
            }
        }
    }
}

inline GCode::ObjectByExtruder& object_by_extruder(std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>& by_extruder,
                                                   unsigned int                                                  extruder_id,
                                                   size_t                                                        object_idx,
                                                   size_t                                                        num_objects)
{
    std::vector<GCode::ObjectByExtruder>& objects_by_extruder = by_extruder[extruder_id];
    if (objects_by_extruder.empty())
        objects_by_extruder.assign(num_objects, GCode::ObjectByExtruder());
    return objects_by_extruder[object_idx];
}

static inline void apply_local_z_flow_height_override(ExtrusionPath& path, const double flow_height_override)
{
    if (flow_height_override <= EPSILON)
        return;
    if (path.height > EPSILON) {
        const double ratio = flow_height_override / path.height;
        path.mm3_per_mm *= ratio;
    }
    path.height = float(flow_height_override);
}

static inline void append_clipped_path(const ExtrusionPath& src_path,
                                       const ExPolygons*    include_masks,
                                       const ExPolygons*    exclude_masks,
                                       const double         flow_height_override,
                                       std::vector<ExtrusionPath>& dst)
{
    Polylines segments{src_path.polyline};
    if (include_masks != nullptr && !include_masks->empty())
        segments = intersection_pl(std::move(segments), *include_masks);
    if (exclude_masks != nullptr && !exclude_masks->empty())
        segments = diff_pl(std::move(segments), *exclude_masks);

    for (Polyline& segment : segments) {
        if (!segment.is_valid())
            continue;
        ExtrusionPath clipped(segment, src_path);
        apply_local_z_flow_height_override(clipped, flow_height_override);
        dst.emplace_back(std::move(clipped));
    }
}

static inline double local_z_polyline_distance2_to_point(const Polyline& polyline, const Point& point)
{
    if (polyline.points.empty())
        return std::numeric_limits<double>::max();
    if (polyline.points.size() == 1)
        return (polyline.first_point() - point).cast<double>().squaredNorm();
    const std::pair<int, Point> projected = foot_pt(polyline.points, point);
    return projected.first >= 0 ? (projected.second - point).cast<double>().squaredNorm()
                                : std::numeric_limits<double>::max();
}

static void local_z_order_clipped_paths_for_seam(std::vector<ExtrusionPath>& paths, const Point& seam_anchor)
{
    if (paths.empty())
        return;

    // Local-Z turns closed loops into open clipped fragments. Start the fragment
    // batch at the original seam anchor when possible so previewed restarts stay aligned.
    const double anchor_eps  = scaled<double>(0.02);
    const double anchor_eps2 = anchor_eps * anchor_eps;

    std::vector<ExtrusionPath> ordered;
    ordered.reserve(paths.size() + 1);

    size_t best_anchor_idx = size_t(-1);
    double best_anchor_d2  = std::numeric_limits<double>::max();
    for (size_t idx = 0; idx < paths.size(); ++idx) {
        const double d2 = local_z_polyline_distance2_to_point(paths[idx].polyline, seam_anchor);
        if (d2 < best_anchor_d2) {
            best_anchor_d2  = d2;
            best_anchor_idx = idx;
        }
    }

    Point current_pos = seam_anchor;
    if (best_anchor_idx != size_t(-1) && best_anchor_d2 <= anchor_eps2) {
        ExtrusionPath anchor_path = std::move(paths[best_anchor_idx]);
        paths.erase(paths.begin() + best_anchor_idx);

        Polyline head;
        Polyline tail;
        Point split_point = seam_anchor;
        anchor_path.polyline.split_at(split_point, &head, &tail);

        if (tail.is_valid()) {
            ExtrusionPath tail_path(std::move(tail), anchor_path);
            ordered.emplace_back(std::move(tail_path));
            current_pos = ordered.back().last_point();
        }

        if (head.is_valid()) {
            ExtrusionPath head_path(std::move(head), anchor_path);
            if (head_path.can_reverse())
                head_path.reverse();
            ordered.emplace_back(std::move(head_path));
            current_pos = ordered.back().last_point();
        }
    }

    while (!paths.empty()) {
        size_t best_idx       = 0;
        bool   best_reversed  = false;
        double best_endpoint2 = std::numeric_limits<double>::max();

        for (size_t idx = 0; idx < paths.size(); ++idx) {
            const double first_d2 = (paths[idx].first_point() - current_pos).cast<double>().squaredNorm();
            if (first_d2 < best_endpoint2) {
                best_endpoint2 = first_d2;
                best_idx       = idx;
                best_reversed  = false;
            }

            if (paths[idx].can_reverse()) {
                const double last_d2 = (paths[idx].last_point() - current_pos).cast<double>().squaredNorm();
                if (last_d2 < best_endpoint2) {
                    best_endpoint2 = last_d2;
                    best_idx       = idx;
                    best_reversed  = true;
                }
            }
        }

        ExtrusionPath next_path = std::move(paths[best_idx]);
        paths.erase(paths.begin() + best_idx);
        if (best_reversed)
            next_path.reverse();
        current_pos = next_path.last_point();
        ordered.emplace_back(std::move(next_path));
    }

    paths = std::move(ordered);
}

static inline void append_clipped_paths(std::vector<ExtrusionPath>&& paths, ExtrusionEntityCollection& dst)
{
    for (ExtrusionPath& path : paths)
        if (!path.empty())
            dst.append(std::move(path));
}

static inline ExPolygons local_z_compensate_masks(const ExPolygons& src_masks,
                                                  const float       delta_scaled,
                                                  const bool        fallback_to_source)
{
    if (src_masks.empty() || std::abs(delta_scaled) <= EPSILON)
        return src_masks;

    ExPolygons compensated = offset_ex(src_masks, delta_scaled);
    if (!compensated.empty() && compensated.size() > 1)
        compensated = union_ex(compensated);

    if (compensated.empty() && fallback_to_source)
        return src_masks;
    return compensated;
}

struct LocalZPathHeightStats
{
    size_t count { 0 };
    double min   { std::numeric_limits<double>::max() };
    double max   { 0.0 };
};

static inline void collect_local_z_path_height_stats(const ExtrusionEntity& entity, LocalZPathHeightStats& stats)
{
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&entity)) {
        const double h = path->height;
        ++stats.count;
        stats.min = std::min(stats.min, h);
        stats.max = std::max(stats.max, h);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath& p : multipath->paths) {
            const double h = p.height;
            ++stats.count;
            stats.min = std::min(stats.min, h);
            stats.max = std::max(stats.max, h);
        }
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&entity)) {
        for (const ExtrusionPath& p : loop->paths) {
            const double h = p.height;
            ++stats.count;
            stats.min = std::min(stats.min, h);
            stats.max = std::max(stats.max, h);
        }
    } else if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity* child : collection->entities)
            if (child != nullptr)
                collect_local_z_path_height_stats(*child, stats);
    }
}

static inline void finalize_local_z_path_height_stats(LocalZPathHeightStats& stats)
{
    if (stats.count == 0) {
        stats.min = 0.;
        stats.max = 0.;
    }
}

static inline LocalZPathHeightStats collect_local_z_path_height_stats(const ExtrusionEntityCollection& source)
{
    LocalZPathHeightStats stats;
    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity* entity : flattened.entities)
        if (entity != nullptr)
            collect_local_z_path_height_stats(*entity, stats);
    finalize_local_z_path_height_stats(stats);
    return stats;
}

static inline LocalZPathHeightStats collect_local_z_path_height_stats(const ExtrusionEntitiesPtr& source)
{
    LocalZPathHeightStats stats;
    for (const ExtrusionEntity* entity : source)
        if (entity != nullptr)
            collect_local_z_path_height_stats(*entity, stats);
    finalize_local_z_path_height_stats(stats);
    return stats;
}

static inline Polylines collect_local_z_polylines(const ExtrusionEntityCollection& source)
{
    Polylines lines;
    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity* entity : flattened.entities) {
        if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
            lines.emplace_back(path->polyline);
        } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            for (const ExtrusionPath& p : multipath->paths)
                lines.emplace_back(p.polyline);
        } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            for (const ExtrusionPath& p : loop->paths)
                lines.emplace_back(p.polyline);
        }
    }
    return lines;
}

using LocalZLoopSeamPlacer = std::function<bool(const ExtrusionLoop&, ExtrusionLoop&, Point&)>;

static std::unique_ptr<ExtrusionEntityCollection> clip_extrusion_collection_for_local_z(
    const ExtrusionEntityCollection& source,
    const ExPolygons*                include_masks,
    const ExPolygons*                exclude_masks,
    const double                     flow_height_override,
    const LocalZLoopSeamPlacer*      seam_placer = nullptr)
{
    if (source.entities.empty())
        return nullptr;

    if ((include_masks == nullptr || include_masks->empty()) &&
        (exclude_masks == nullptr || exclude_masks->empty()) &&
        flow_height_override <= EPSILON) {
        return std::make_unique<ExtrusionEntityCollection>(source);
    }

    auto out = std::make_unique<ExtrusionEntityCollection>();
    out->no_sort = source.no_sort;

    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity* entity : flattened.entities) {
        if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
            std::vector<ExtrusionPath> clipped_paths;
            append_clipped_path(*path, include_masks, exclude_masks, flow_height_override, clipped_paths);
            append_clipped_paths(std::move(clipped_paths), *out);
        } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            std::vector<ExtrusionPath> clipped_paths;
            for (const ExtrusionPath& path : multipath->paths)
                append_clipped_path(path, include_masks, exclude_masks, flow_height_override, clipped_paths);
            append_clipped_paths(std::move(clipped_paths), *out);
        } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            ExtrusionLoop seam_loop = *loop;
            Point         seam_anchor = loop->first_point();
            const bool    seam_ready =
                seam_placer != nullptr && (*seam_placer)(*loop, seam_loop, seam_anchor);
            std::vector<ExtrusionPath> clipped_paths;
            for (const ExtrusionPath& path : seam_loop.paths)
                append_clipped_path(path, include_masks, exclude_masks, flow_height_override, clipped_paths);
            if (seam_ready)
                local_z_order_clipped_paths_for_seam(clipped_paths, seam_anchor);
            append_clipped_paths(std::move(clipped_paths), *out);
        } else {
            // Fallback for unknown entity subclasses: keep behavior unchanged for now.
            if (include_masks == nullptr && exclude_masks == nullptr && flow_height_override <= EPSILON)
                out->append(*entity);
        }
    }

    if (out->entities.empty())
        return nullptr;

    return out;
}

static std::vector<unsigned int> decode_manual_pattern_sequence_for_gcode(const MixedFilament& mf, size_t num_physical)
{
    std::vector<unsigned int> sequence;
    if (mf.manual_pattern.empty())
        return sequence;

    const std::string flattened = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    if (flattened.empty())
        return sequence;

    const std::vector<std::string> group_strs = MixedFilamentManager::split_pattern_groups(flattened);
    for (const std::string &group : group_strs) {
        const std::vector<std::string> tokens =
            MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
        for (const std::string &token : tokens) {
            const unsigned int extruder_id =
                MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
            if (extruder_id >= 1 && extruder_id <= num_physical)
                sequence.emplace_back(extruder_id);
        }
    }
    return sequence;
}

static std::vector<unsigned int> decode_gradient_component_ids_for_gcode(const MixedFilament& mf, size_t num_physical)
{
    return MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, num_physical);
}

static std::vector<int> decode_gradient_component_weights_for_gcode(const MixedFilament& mf, size_t expected_components)
{
    std::vector<int> out;
    if (mf.gradient_component_weights.empty() || expected_components == 0)
        return out;
    std::string token;
    for (const char c : mf.gradient_component_weights) {
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            out.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        out.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (out.size() != expected_components)
        return {};
    return out;
}

static std::vector<unsigned int> build_weighted_gradient_sequence_for_gcode(const std::vector<unsigned int>& ids,
                                                                            const std::vector<int>&          weights)
{
    if (ids.empty())
        return {};

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int w = (i < weights.size()) ? std::max(0, weights[i]) : 0;
        if (w <= 0)
            continue;
        filtered_ids.emplace_back(ids[i]);
        counts.emplace_back(w);
    }
    if (filtered_ids.empty()) {
        filtered_ids = ids;
        counts.assign(ids.size(), 1);
    }

    int g = 0;
    for (const int c : counts)
        g = std::gcd(g, std::max(1, c));
    if (g > 1) {
        for (int &c : counts)
            c = std::max(1, c / g);
    }

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int k_max_cycle = 48;
    if (cycle > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(cycle);
        for (int &c : counts)
            c = std::max(1, int(std::round(double(c) * scale)));
        cycle = std::accumulate(counts.begin(), counts.end(), 0);
        while (cycle > k_max_cycle) {
            auto it = std::max_element(counts.begin(), counts.end());
            if (it == counts.end() || *it <= 1)
                break;
            --(*it);
            --cycle;
        }
    }
    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx = 0;
        double best_score = -1e9;
        for (size_t i = 0; i < counts.size(); ++i) {
            const double target = double((pos + 1) * counts[i]) / double(cycle);
            const double score = target - double(emitted[i]);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

static size_t unique_extruder_count_for_gcode(const std::vector<unsigned int>& sequence, size_t num_physical)
{
    if (sequence.empty() || num_physical == 0)
        return 0;
    std::vector<bool> seen(num_physical + 1, false);
    size_t            unique = 0;
    for (const unsigned int id : sequence) {
        if (id == 0 || id > num_physical)
            continue;
        if (!seen[id]) {
            seen[id] = true;
            ++unique;
        }
    }
    return unique;
}

static std::vector<unsigned int> pointillism_sequence_for_row_for_gcode(const MixedFilament& mf, size_t num_physical)
{
#if 0
    if (!mf.enabled || num_physical == 0 || mf.distribution_mode != int(MixedFilament::SameLayerPointillisme))
        return {};

    if (!mf.manual_pattern.empty())
        return decode_manual_pattern_sequence_for_gcode(mf, num_physical);

    const std::vector<unsigned int> gradient_ids = decode_gradient_component_ids_for_gcode(mf, num_physical);
    if (gradient_ids.size() >= 2) {
        const std::vector<int> gradient_weights = decode_gradient_component_weights_for_gcode(mf, gradient_ids.size());
        const std::vector<unsigned int> weighted =
            build_weighted_gradient_sequence_for_gcode(gradient_ids,
                gradient_weights.empty() ? std::vector<int>(gradient_ids.size(), 1) : gradient_weights);
        if (!weighted.empty())
            return weighted;
    }

    if (mf.component_a < 1 || mf.component_a > num_physical ||
        mf.component_b < 1 || mf.component_b > num_physical ||
        mf.component_a == mf.component_b)
        return {};

    int ratio_a = std::max(0, mf.ratio_a);
    int ratio_b = std::max(0, mf.ratio_b);
    if (ratio_a == 0 && ratio_b == 0)
        ratio_a = 1;
    if (ratio_a > 0 && ratio_b > 0) {
        const int g = std::gcd(ratio_a, ratio_b);
        if (g > 1) {
            ratio_a /= g;
            ratio_b /= g;
        }
    }

    constexpr int k_max_cycle = 24;
    if (ratio_a + ratio_b > k_max_cycle) {
        const double scale = double(k_max_cycle) / double(ratio_a + ratio_b);
        ratio_a = std::max(1, int(std::round(double(ratio_a) * scale)));
        ratio_b = std::max(1, int(std::round(double(ratio_b) * scale)));
    }

    const int cycle = std::max(1, ratio_a + ratio_b);
    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    for (int pos = 0; pos < cycle; ++pos) {
        const int b_before = (pos * ratio_b) / cycle;
        const int b_after  = ((pos + 1) * ratio_b) / cycle;
        sequence.emplace_back((b_after > b_before) ? mf.component_b : mf.component_a);
    }

    bool seen_a = false;
    bool seen_b = false;
    for (const unsigned int extruder_id : sequence) {
        seen_a = seen_a || extruder_id == mf.component_a;
        seen_b = seen_b || extruder_id == mf.component_b;
        if (seen_a && seen_b)
            break;
    }
    if (!seen_a || !seen_b)
        return {};
    return sequence;
#endif
    (void)mf;
    (void)num_physical;
    return {};
}

static void split_polyline_by_length_for_pointillism(const Polyline& src,
                                                     const double    split_length,
                                                     Polylines&      out)
{
    out.clear();
    if (!src.is_valid())
        return;
    if (split_length <= EPSILON) {
        out.emplace_back(src);
        return;
    }

    Polyline remainder = src;
    size_t   guard     = 0;
    while (remainder.is_valid() && remainder.points.size() >= 2 && ++guard < 200000) {
        if (remainder.length() <= split_length + EPSILON) {
            out.emplace_back(std::move(remainder));
            break;
        }
        Polyline head;
        Polyline tail;
        if (!remainder.split_at_length(split_length, &head, &tail) || !head.is_valid()) {
            out.emplace_back(std::move(remainder));
            break;
        }
        out.emplace_back(std::move(head));
        if (!tail.is_valid() || tail.points.size() < 2)
            break;
        remainder = std::move(tail);
    }
    if (out.empty())
        out.emplace_back(src);
}

static bool trim_polyline_for_pointillism_gap(Polyline& src, const double trim_each_end)
{
    if (!src.is_valid())
        return false;
    if (trim_each_end <= EPSILON)
        return true;

    const double original_len = src.length();
    if (original_len <= 2.0 * trim_each_end + EPSILON)
        return false;

    Polyline head;
    Polyline tail;
    if (!src.split_at_length(trim_each_end, &head, &tail) || !tail.is_valid() || tail.points.size() < 2)
        return false;
    src = std::move(tail);

    const double keep_len = src.length() - trim_each_end;
    if (keep_len <= EPSILON)
        return false;
    if (!src.split_at_length(keep_len, &head, &tail) || !head.is_valid() || head.points.size() < 2)
        return false;
    src = std::move(head);
    return src.is_valid() && src.points.size() >= 2;
}

struct PointillismPathSplitStats
{
    size_t segment_count { 0 };
    size_t bucket_count  { 0 };
};

// Sentinel used only in G-code generation to recognize pointillism path-domain
// split segments. This lets us apply per-segment runtime guards without
// affecting regular perimeter/infill paths.
static constexpr int k_pointillism_path_inset_marker = -7777;

static bool split_extrusion_collection_for_pointillism_paths(
    const ExtrusionEntityCollection&                         source,
    const std::vector<unsigned int>&                         sequence,
    size_t                                                   num_physical,
    const double                                             split_length_scaled,
    const double                                             split_gap_scaled,
    size_t                                                   sequence_phase,
    std::vector<std::unique_ptr<ExtrusionEntityCollection>>& out_by_extruder,
    PointillismPathSplitStats&                               out_stats)
{
    out_by_extruder.clear();
    out_by_extruder.resize(num_physical);
    out_stats = {};

    if (source.entities.empty() || sequence.empty() || num_physical == 0 || split_length_scaled <= EPSILON)
        return false;

    unsigned int fallback_extruder = 0;
    for (const unsigned int id : sequence) {
        if (id >= 1 && id <= num_physical) {
            fallback_extruder = id;
            break;
        }
    }
    if (fallback_extruder == 0)
        return false;

    size_t sequence_idx = sequence_phase % sequence.size();
    auto append_piece = [&](unsigned int extruder_id, const ExtrusionPath& src_path, Polyline& piece) {
        if (!piece.is_valid())
            return;
        if (extruder_id == 0 || extruder_id > num_physical)
            extruder_id = fallback_extruder;
        std::unique_ptr<ExtrusionEntityCollection>& dst = out_by_extruder[extruder_id - 1];
        if (!dst) {
            dst = std::make_unique<ExtrusionEntityCollection>();
            dst->no_sort = source.no_sort;
        }
        ExtrusionPath out_path(piece, src_path);
        out_path.inset_idx = k_pointillism_path_inset_marker;
        dst->append(std::move(out_path));
        ++out_stats.segment_count;
    };

    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity* entity : flattened.entities) {
        auto split_one_path = [&](const ExtrusionPath& path) {
            Polylines pieces;
            split_polyline_by_length_for_pointillism(path.polyline, split_length_scaled, pieces);
            const double trim_each_end = std::max(0.0, split_gap_scaled * 0.5);
            for (Polyline& piece : pieces) {
                if (trim_each_end > EPSILON && !trim_polyline_for_pointillism_gap(piece, trim_each_end)) {
                    ++sequence_idx;
                    continue;
                }
                unsigned int extruder_id = sequence[sequence_idx % sequence.size()];
                append_piece(extruder_id, path, piece);
                ++sequence_idx;
            }
        };

        if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
            split_one_path(*path);
        } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            for (const ExtrusionPath& path : multipath->paths)
                split_one_path(path);
        } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            for (const ExtrusionPath& path : loop->paths)
                split_one_path(path);
        }
    }

    for (const std::unique_ptr<ExtrusionEntityCollection>& bucket : out_by_extruder) {
        if (bucket && !bucket->entities.empty())
            ++out_stats.bucket_count;
    }
    return out_stats.segment_count > 0;
}

static bool split_extrusion_collection_for_multi_perimeter_pattern(
    const ExtrusionEntityCollection&                         source,
    const MixedFilamentManager&                              mixed_mgr,
    unsigned int                                             mixed_filament_id,
    size_t                                                   num_physical,
    int                                                      layer_index,
    std::vector<std::unique_ptr<ExtrusionEntityCollection>>& out_by_extruder,
    size_t&                                                  out_bucket_count,
    const PrintObject*                                       current_object = nullptr)
{
    out_by_extruder.clear();
    out_by_extruder.resize(num_physical);
    out_bucket_count = 0;

    if (source.entities.empty() || num_physical == 0)
        return false;

    size_t split_entities = 0;
    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity* entity : flattened.entities) {
        if (entity == nullptr)
            continue;

        int perimeter_index = entity->inset_idx;
        if (perimeter_index < 0)
            perimeter_index = entity->role() == erExternalPerimeter ? 0 : 1;

        const unsigned int extruder_id = mixed_mgr.resolve_perimeter(
            mixed_filament_id, num_physical, layer_index, perimeter_index, 0.f, 0.f, false, current_object);
        if (extruder_id == 0 || extruder_id > num_physical)
            continue;

        std::unique_ptr<ExtrusionEntityCollection>& bucket = out_by_extruder[extruder_id - 1];
        if (!bucket) {
            bucket = std::make_unique<ExtrusionEntityCollection>();
            bucket->no_sort = source.no_sort;
        }
        bucket->append(*entity);
        ++split_entities;
    }

    for (const std::unique_ptr<ExtrusionEntityCollection>& bucket : out_by_extruder) {
        if (bucket && !bucket->entities.empty())
            ++out_bucket_count;
    }
    return split_entities > 0;
}

inline std::vector<GCode::ObjectByExtruder::Island>& object_islands_by_extruder(
    std::map<unsigned int, std::vector<GCode::ObjectByExtruder>>& by_extruder,
    unsigned int                                                  extruder_id,
    size_t                                                        object_idx,
    size_t                                                        num_objects,
    size_t                                                        num_islands)
{
    std::vector<GCode::ObjectByExtruder::Island>& islands = object_by_extruder(by_extruder, extruder_id, object_idx, num_objects).islands;
    if (islands.empty())
        islands.assign(num_islands, GCode::ObjectByExtruder::Island());
    return islands;
}

std::vector<GCode::InstanceToPrint> GCode::sort_print_object_instances(
    std::vector<GCode::ObjectByExtruder>& objects_by_extruder,
    const std::vector<LayerToPrint>&      layers,
    // Ordering must be defined for normal (non-sequential print).
    const std::vector<const PrintInstance*>* ordering,
    // For sequential print, the instance of the object to be printing has to be defined.
    const size_t single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
        // Sequential print, single object is being printed.
        for (ObjectByExtruder& object_by_extruder : objects_by_extruder) {
            const size_t layer_id = &object_by_extruder - objects_by_extruder.data();
            // BBS:add the support of shared print object
            const PrintObject* print_object = layers[layer_id].original_object;
            // const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                out.emplace_back(object_by_extruder, layer_id, *print_object, single_object_instance_idx,
                                 print_object->instances()[single_object_instance_idx].model_instance->get_labeled_id());
        }
    } else {
        // Create mapping from PrintObject* to ObjectByExtruder*.
        std::vector<std::pair<const PrintObject*, ObjectByExtruder*>> sorted;
        sorted.reserve(objects_by_extruder.size());
        for (ObjectByExtruder& object_by_extruder : objects_by_extruder) {
            const size_t layer_id = &object_by_extruder - objects_by_extruder.data();
            // BBS:add the support of shared print object
            const PrintObject* print_object = layers[layer_id].original_object;
            // const PrintObject *print_object = layers[layer_id].object();
            if (print_object)
                sorted.emplace_back(print_object, &object_by_extruder);
        }
        std::sort(sorted.begin(), sorted.end());

        if (!sorted.empty()) {
            out.reserve(sorted.size());
            for (const PrintInstance* instance : *ordering) {
                const PrintObject& print_object = *instance->print_object;
                // BBS:add the support of shared print object
                // const PrintObject* print_obj_ptr = &print_object;
                // if (print_object.get_shared_object())
                //     print_obj_ptr = print_object.get_shared_object();
                std::pair<const PrintObject*, ObjectByExtruder*> key(&print_object, nullptr);
                auto                                             it = std::lower_bound(sorted.begin(), sorted.end(), key);
                if (it != sorted.end() && it->first == &print_object)
                    // ObjectByExtruder for this PrintObject was found.
                    out.emplace_back(*it->second, it->second - objects_by_extruder.data(), print_object,
                                     instance - print_object.instances().data(), instance->model_instance->get_labeled_id());
            }
        }
    }
    return out;
}

namespace ProcessLayer {

static std::string emit_custom_gcode_per_print_z(GCode&                   gcodegen,
                                                 const CustomGCode::Item* custom_gcode,
                                                 unsigned int             current_extruder_id,
                                                 // ID of the first extruder printing this layer.
                                                 unsigned int       first_extruder_id,
                                                 const PrintConfig& config)
{
    std::string gcode;
    // BBS
    bool single_filament_print = config.filament_diameter.size() == 1;

    if (custom_gcode != nullptr) {
        // Extruder switches are processed by LayerTools, they should be filtered out.
        assert(custom_gcode->type != CustomGCode::ToolChange);

        CustomGCode::Type gcode_type   = custom_gcode->type;
        bool              color_change = gcode_type == CustomGCode::ColorChange;
        bool              tool_change  = gcode_type == CustomGCode::ToolChange;
        // Tool Change is applied as Color Change for a single extruder printer only.
        assert(!tool_change || single_filament_print);

        std::string pause_print_msg;
        int         m600_extruder_before_layer = -1;
        if (color_change && custom_gcode->extruder > 0)
            m600_extruder_before_layer = custom_gcode->extruder - 1;
        else if (gcode_type == CustomGCode::PausePrint)
            pause_print_msg = custom_gcode->extra;
            // BBS: inserting color gcode is removed
#if 0
            // we should add or not colorprint_change in respect to nozzle_diameter count instead of really used extruders count
            if (color_change || tool_change)
            {
                assert(m600_extruder_before_layer >= 0);
                // Color Change or Tool Change as Color Change.
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Color_Change) + ",T" + std::to_string(m600_extruder_before_layer) + "," + custom_gcode->color + "\n";

                if (!single_filament_print && m600_extruder_before_layer >= 0 && first_extruder_id != (unsigned)m600_extruder_before_layer
                    // && !MMU1
                    ) {
                    //! FIXME_in_fw show message during print pause
                    DynamicConfig cfg;
                    cfg.set_key_value("color_change_extruder", new ConfigOptionInt(m600_extruder_before_layer));
                    gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id, &cfg);
                    gcode += "\n";
                    gcode += "M117 Change filament for Extruder " + std::to_string(m600_extruder_before_layer) + "\n";
                }
                else {
                    gcode += gcodegen.placeholder_parser_process("color_change_gcode", config.color_change_gcode, current_extruder_id);
                    gcode += "\n";
                    //FIXME Tell G-code writer that M600 filled the extruder, thus the G-code writer shall reset the extruder to unretracted state after
                    // return from M600. Thus the G-code generated by the following line is ignored.
                    // see GH issue #6362
                    gcodegen.writer().unretract();
                }
            }
            else {
#endif
        if (gcode_type == CustomGCode::PausePrint) // Pause print
        {
            // add tag for processor
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n";
            //! FIXME_in_fw show message during print pause
            // if (!pause_print_msg.empty())
            //     gcode += "M117 " + pause_print_msg + "\n";
            gcode += gcodegen.placeholder_parser_process("machine_pause_gcode", config.machine_pause_gcode, current_extruder_id) + "\n";
        } else {
            // add tag for processor
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Custom_Code) + "\n";
            if (gcode_type == CustomGCode::Template) // Template Custom Gcode
                gcode += gcodegen.placeholder_parser_process("template_custom_gcode", config.template_custom_gcode, current_extruder_id);
            else // custom Gcode
                gcode += custom_gcode->extra;
        }
        gcode += "\n";
#if 0
            }
#endif
    }

    return gcode;
}
} // namespace ProcessLayer

namespace Skirt {
static void skirt_loops_per_extruder_all_printing(const Print&                                       print,
                                                  const ExtrusionEntityCollection&                   skirt,
                                                  const LayerTools&                                  layer_tools,
                                                  std::map<unsigned int, std::pair<size_t, size_t>>& skirt_loops_per_extruder_out)
{
    // Prime all extruders printing over the 1st layer over the skirt lines.
    size_t n_loops            = skirt.entities.size();
    size_t n_tools            = layer_tools.extruders.size();
    size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;

    // BBS. Extrude skirt with first extruder if min_skirt_length is zero
    // ORCA: Always extrude skirt with first extruder, independantly of if the minimum skirt length is zero or not. The code below
    // is left as a placeholder for when a multiextruder support is implemented. Then we will need to extrude the skirt loops for each extruder.
    // const PrintConfig &config = print.config();
    // if (config.min_skirt_length.value < EPSILON) {
    skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, n_loops);
    //} else {
    //    for (size_t i = 0; i < n_loops; i += lines_per_extruder)
    //        skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i +
    //        lines_per_extruder, n_loops));
    //}
}

static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
    const Print&                     print,
    const ExtrusionEntityCollection& skirt,
    const LayerTools&                layer_tools,
    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>& skirt_done)
{
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
    // For sequential print, the following test may fail when extruding the 2nd and other objects.
    //  assert(skirt_done.empty());
    if (skirt_done.empty() && print.has_skirt() && !skirt.entities.empty() && layer_tools.has_skirt) {
        skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
        skirt_done.emplace_back(layer_tools.print_z);
    }
    return skirt_loops_per_extruder_out;
}

static std::map<unsigned int, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
    const Print&                     print,
    const ExtrusionEntityCollection& skirt,
    const LayerTools&                layer_tools,
    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>& skirt_done)
{
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
    if (print.has_skirt() && !skirt.entities.empty() && layer_tools.has_skirt &&
        // Not enough skirt layers printed yet.
        // FIXME infinite or high skirt does not make sense for sequential print!
        (skirt_done.size() < (size_t) print.config().skirt_height.value || print.has_infinite_skirt())) {
        bool valid = !skirt_done.empty() && skirt_done.back() < layer_tools.print_z - EPSILON;
        assert(valid);
        // This print_z has not been extruded yet (sequential print)
        // FIXME: The skirt_done should not be empty at this point. The check is a workaround
        if (valid) {
#if 0
                // Prime just the first printing extruder. This is original Slic3r's implementation.
                skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirt_loops.value);
#else
            // Prime all extruders planned for this layer, see
            skirt_loops_per_extruder_all_printing(print, skirt, layer_tools, skirt_loops_per_extruder_out);
#endif
            assert(!skirt_done.empty());
            skirt_done.emplace_back(layer_tools.print_z);
        }
    }
    return skirt_loops_per_extruder_out;
}

static Point find_start_point(ExtrusionLoop& loop, float start_angle)
{
    coord_t min_x = std::numeric_limits<coord_t>::max();
    coord_t max_x = std::numeric_limits<coord_t>::min();
    coord_t min_y = min_x;
    coord_t max_y = max_x;

    Points pts;
    loop.collect_points(pts);
    for (Point pt : pts) {
        if (pt.x() < min_x)
            min_x = pt.x();
        else if (pt.x() > max_x)
            max_x = pt.x();
        if (pt.y() < min_y)
            min_y = pt.y();
        else if (pt.y() > max_y)
            max_y = pt.y();
    }

    Point  center((min_x + max_x) / 2., (min_y + max_y) / 2.);
    double r       = center.distance_to(Point(min_x, min_y));
    double deg     = start_angle * PI / 180;
    double shift_x = r * std::cos(deg);
    double shift_y = r * std::sin(deg);
    return Point(center.x() + shift_x, center.y() + shift_y);
}

} // namespace Skirt

// Orca: Klipper can't parse object names with spaces and other spetical characters
std::string sanitize_instance_name(const std::string& name)
{
    // Replace sequences of non-word characters with an underscore
    std::string result = std::regex_replace(name, std::regex("[ !@#$%^&*()=+\\[\\]{};:\",']+"), "_");
    // Remove leading and trailing underscores
    if (!result.empty() && result.front() == '_') {
        result.erase(result.begin());
    }
    if (!result.empty() && result.back() == '_') {
        result.erase(result.end() - 1);
    }

    return result;
}

inline std::string get_instance_name(const PrintObject* object, size_t inst_id)
{
    auto obj_name = sanitize_instance_name(object->model_object()->name);
    auto name     = (boost::format("%1%_id_%2%_copy_%3%") % obj_name % object->get_id() % inst_id).str();
    return sanitize_instance_name(name);
}

inline std::string get_instance_name(const PrintObject* object, const PrintInstance& inst) { return get_instance_name(object, inst.id); }

std::string GCode::generate_skirt(const Print&                     print,
                                  const ExtrusionEntityCollection& skirt,
                                  const Point&                     offset,
                                  const float                      skirt_start_angle,
                                  const LayerTools&                layer_tools,
                                  const Layer&                     layer,
                                  unsigned int                     extruder_id)
{
    bool        first_layer = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    std::string gcode;
    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<unsigned int, std::pair<size_t, size_t>> skirt_loops_per_extruder;
    skirt_loops_per_extruder = first_layer ? Skirt::make_skirt_loops_per_extruder_1st_layer(print, skirt, layer_tools, m_skirt_done) :
                                             Skirt::make_skirt_loops_per_extruder_other_layers(print, skirt, layer_tools, m_skirt_done);

    if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
        const std::pair<size_t, size_t> loops = loops_it->second;

        set_origin(unscaled(offset));

        m_avoid_crossing_perimeters.use_external_mp();
        Flow layer_skirt_flow = print.skirt_flow().with_height(
            float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
        double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
        // Decide where to start looping:
        // - If it’s the first layer or if we do NOT want a single-wall skirt/draft shield,
        //   start from loops.first (all loops).
        // - Otherwise, if single_loop_draft_shield == true (and not the first layer),
        //   start from loops.second - 1 (just one loop).
        const size_t start_idx = (first_layer || !print.m_config.single_loop_draft_shield) ? loops.first : (loops.second - 1);

        // Loop over the skirt loops and extrude
        for (size_t i = start_idx; i < loops.second; ++i) {
            // Adjust flow according to this layer's layer height.
            ExtrusionLoop loop = *dynamic_cast<const ExtrusionLoop*>(skirt.entities[i]);
            for (ExtrusionPath& path : loop.paths) {
                path.height     = layer_skirt_flow.height();
                path.mm3_per_mm = mm3_per_mm;
            }

            // FIXME using the support_speed of the 1st object printed.
            if (first_layer && i == loops.first) {
                // set skirt start point location
                const Point desired_start_point = Skirt::find_start_point(loop, skirt_start_angle);
                gcode += this->extrude_loop(loop, "skirt", this->process_flow_value(m_config.support_speed), {}, &desired_start_point);
            } else
                gcode += this->extrude_loop(loop, "skirt", this->process_flow_value(m_config.support_speed));

            // If we only want a single wall on non-first layers, break now
            if (!first_layer && print.m_config.single_loop_draft_shield) {
                break;
            }
        }
        m_avoid_crossing_perimeters.use_external_mp(false);
        // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
        if (first_layer && loops.first == 0)
            m_avoid_crossing_perimeters.disable_once();
    }
    return gcode;
}

// In sequential mode, process_layer is called once per each object and its copy,
// therefore layers will contain a single entry and single_object_instance_idx will point to the copy of the object.
// In non-sequential mode, process_layer is called per each print_z height with all object and support layers accumulated.
// For multi-material prints, this routine minimizes extruder switches by gathering extruder specific extrusion paths
// and performing the extruder specific extrusions together.
LayerResult GCode::process_layer(const Print& print,
                                 // Set of object & print layers of the same PrintObject and with the same print_z.
                                 const std::vector<LayerToPrint>& layers,
                                 const LayerTools&                layer_tools,
                                 const bool                       last_layer,
                                 // Pairs of PrintObject index and its instance index.
                                 const std::vector<const PrintInstance*>* ordering,
                                 // If set to size_t(-1), then print all copies of all objects.
                                 // Otherwise print a single copy of a single object.
                                 const size_t single_object_instance_idx,
                                 // BBS
                                 const bool prime_extruder)
{
    assert(!layers.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);

    // First object, support and raft layer, if available.
    const Layer*        object_layer  = nullptr;
    const SupportLayer* support_layer = nullptr;
    const SupportLayer* raft_layer    = nullptr;
    for (const LayerToPrint& l : layers) {
        if (l.object_layer && !object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (!support_layer)
                support_layer = l.support_layer;
            if (!raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
    }

    const Layer* layer_ptr = nullptr;
    if (object_layer != nullptr)
        layer_ptr = object_layer;
    else if (support_layer != nullptr)
        layer_ptr = support_layer;
    const Layer& layer = *layer_ptr;
    LayerResult  result{{}, layer.id(), false, last_layer};
    result.last_layer = last_layer;
    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return result;

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    coordf_t print_z = layer.print_z;
    // BBS: using layer id to judge whether the layer is first layer is wrong. Because if the normal
    // support is attached above the object, and support layers has independent layer height, then the lowest support
    // interface layer id is 0.
    bool first_layer = (layer.id() == 0 && abs(layer.bottom_z()) < EPSILON);
    m_writer.set_is_first_layer(first_layer);
    unsigned int first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || !print.has_brim()) &&
                      (layer.id() >= (size_t) print.config().skirt_height.value && !print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion* layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_shell_layers.value) > layer.id() ||
                    layer_region->perimeters.items_count() > 1u || layer_region->fills.items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        // If we're going to apply spiralvase to this layer, disable loop clipping.
        m_enable_loop_clipping = !enable;
    }

    std::string gcode;
    assert(is_decimal_separator_point()); // for the sprintfs

    // add tag for processor
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // export layer z
    char buf[64];
    sprintf(buf, print.is_BBL_printer() ? "; Z_HEIGHT: %g\n" : ";Z:%g\n", print_z);
    gcode += buf;
    // export layer height
    float height = first_layer ? static_cast<float>(print_z) : static_cast<float>(print_z) - m_last_layer_z;
    sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), height);
    gcode += buf;
    // update caches
    m_last_layer_z = static_cast<float>(print_z);
    m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height  = height;

    // Set new layer - this will change Z and force a retraction if retract_when_changing_layer is enabled.
    if (!m_config.before_layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_change_gcode", print.config().before_layer_change_gcode.value,
                                                  m_writer.extruder()->id(), &config) +
                 "\n";
    }

    PrinterStructure printer_structure                           = m_config.printer_structure.value;
    bool             need_insert_timelapse_gcode_for_traditional = false;
    if (printer_structure == PrinterStructure::psI3 && !m_spiral_vase && (!m_wipe_tower || !m_wipe_tower->enable_timelapse_print()) &&
        print.config().print_sequence == PrintSequence::ByLayer) {
        need_insert_timelapse_gcode_for_traditional = true;
    }
    // Every BBL machine that is not an i3 (X1/P1 family excepted, whose older profiles still carry
    // the photo inside layer_change_gcode and leave time_lapse_gcode empty) takes its timelapse
    // photo from time_lapse_gcode, once per layer, as BambuStudio's process_layer does: at the
    // layer start on a single-nozzle machine, and on a dual-nozzle machine (H2D, H2C, X2D) while
    // the photo head is active, unless the smooth-timelapse tower owns the layer. Without this the
    // H2D/H2C/H2S/P2S G-code carried no photo command at all, so the printer's timelapse flag had
    // nothing to record.
    const bool bbl_layer_timelapse = is_BBL_Printer() && printer_structure != PrinterStructure::psI3 &&
                                     !m_config.time_lapse_gcode.value.empty();
    const bool bbl_dual_nozzle_timelapse = bbl_layer_timelapse && m_config.nozzle_diameter.size() == 2;
    if (bbl_dual_nozzle_timelapse && (!m_wipe_tower || !m_wipe_tower->enable_timelapse_print()))
        need_insert_timelapse_gcode_for_traditional = true;
    auto timelapse_on_photo_head = [this]() {
        return m_writer.extruder() != nullptr &&
               timelapse_extruder_of_filament(int(m_writer.extruder()->id())) == m_timelapse_photo_extruder;
    };
    bool has_insert_timelapse_gcode = false;
    bool has_wipe_tower             = (layer_tools.has_wipe_tower && m_wipe_tower);

    auto insert_timelapse_gcode = [this, print_z, &print, bbl_layer_timelapse]() -> std::string {
        std::string gcode_res;
        if (!m_config.time_lapse_gcode.value.empty() && m_writer.extruder() != nullptr) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            if (bbl_layer_timelapse) {
                // Per call, as BambuStudio's generate_timelapse_gcode sets them: the templates park
                // the head and lift Z only when the active head is not the photo head.
                config.set_key_value("most_used_physical_extruder_id",
                                     new ConfigOptionInt(timelapse_physical_extruder(m_timelapse_photo_extruder)));
                config.set_key_value("curr_physical_extruder_id",
                                     new ConfigOptionInt(timelapse_physical_extruder(
                                         timelapse_extruder_of_filament(int(m_writer.extruder()->id())))));
            }
            gcode_res = this->placeholder_parser_process("timelapse_gcode", print.config().time_lapse_gcode.value,
                                                         m_writer.extruder()->id(), &config) +
                        "\n";
        }
        return gcode_res;
    };

    // BBS: don't use lazy_raise when enable spiral vase
    gcode += this->change_layer(print_z); // this will increase m_layer_index
    m_layer                  = &layer;
    m_object_layer_over_raft = false;
    if (is_BBL_Printer()) {
        if (printer_structure == PrinterStructure::psI3 && !need_insert_timelapse_gcode_for_traditional && !m_spiral_vase &&
            print.config().print_sequence == PrintSequence::ByLayer) {
            std::string timepals_gcode = insert_timelapse_gcode();
            if (!timepals_gcode.empty()) {
                gcode += timepals_gcode;
                m_writer.set_current_position_clear(false);
                // BBS: check whether custom gcode changes the z position. Update if changed
                double temp_z_after_timepals_gcode;
                if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                    Vec3d pos = m_writer.get_position();
                    pos(2)    = temp_z_after_timepals_gcode;
                    m_writer.set_position(pos);
                }
            }
        } else if (bbl_layer_timelapse && !need_insert_timelapse_gcode_for_traditional && m_writer.extruder() != nullptr) {
            // Equivalent to the timelapse G-code the older profiles placed in layer_change_gcode.
            if (EXTRUDER_CONFIG(retract_when_changing_layer))
                gcode += this->retract(false, false, LiftType::NormalLift);
            std::string timepals_gcode = insert_timelapse_gcode();
            if (!timepals_gcode.empty()) {
                gcode += timepals_gcode;
                m_writer.set_current_position_clear(false);
                double temp_z_after_timepals_gcode;
                if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                    Vec3d pos = m_writer.get_position();
                    pos(2)    = temp_z_after_timepals_gcode;
                    m_writer.set_position(pos);
                }
                has_insert_timelapse_gcode = true;
            }
        }
    } else {
        if (!m_config.time_lapse_gcode.value.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            gcode += this->placeholder_parser_process("timelapse_gcode", print.config().time_lapse_gcode.value, m_writer.extruder()->id(),
                                                      &config) +
                     "\n";
        }
    }
    if (!m_config.layer_change_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        gcode += this->placeholder_parser_process("layer_change_gcode", print.config().layer_change_gcode.value, m_writer.extruder()->id(),
                                                  &config) +
                 "\n";
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
    }
    // BBS: set layer time fan speed after layer change gcode
    gcode += ";_SET_FAN_SPEED_CHANGING_LAYER\n";

    // Calibration Layer-specific GCode
    switch (print.calib_mode()) {
    case CalibMode::Calib_PA_Tower: {
        gcode += writer().set_pressure_advance(print.calib_params().start + static_cast<int>(print_z) * print.calib_params().step);
        break;
    }
    case CalibMode::Calib_Temp_Tower: {
        auto offset = static_cast<unsigned int>(print_z / 10.001) * 5;
        gcode += writer().set_temperature(print.calib_params().start - offset);
        break;
    }
    case CalibMode::Calib_VFA_Tower: {
        auto _speed = print.calib_params().start + std::floor(print_z / 5.0) * print.calib_params().step;
        m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloats { std::round(_speed) });
        break;
    }
    case CalibMode::Calib_Vol_speed_Tower: {
        auto _speed = print.calib_params().start + print_z * print.calib_params().step;
        m_calib_config.set_key_value("outer_wall_speed", new ConfigOptionFloats { std::round(_speed) });
        break;
    }
    case CalibMode::Calib_Retraction_tower: {
        auto          _length = print.calib_params().start + std::floor(std::max(0.0, print_z - 0.4)) * print.calib_params().step;
        DynamicConfig _cfg;
        _cfg.set_key_value("retraction_length", new ConfigOptionFloats{_length});
        writer().config.apply(_cfg);
        sprintf(buf, "; Calib_Retraction_tower: Z_HEIGHT: %g, length:%g\n", print_z, _length);
        gcode += buf;
        break;
    }
    case CalibMode::Calib_Input_shaping_freq: {
        if (m_layer_index == 1) {
            gcode += writer().set_input_shaping('A', print.calib_params().start, 0.f);
        } else {
            if (print.calib_params().freqStartX == print.calib_params().freqStartY &&
                print.calib_params().freqEndX == print.calib_params().freqEndY) {
                gcode += writer().set_input_shaping('A', 0.f,
                                                    (print.calib_params().freqStartX) +
                                                        ((print.calib_params().freqEndX) - (print.calib_params().freqStartX)) *
                                                            (m_layer_index - 2) / (m_layer_count - 3));
            } else {
                gcode += writer().set_input_shaping('X', 0.f,
                                                    (print.calib_params().freqStartX) +
                                                        ((print.calib_params().freqEndX) - (print.calib_params().freqStartX)) *
                                                            (m_layer_index - 2) / (m_layer_count - 3));
                gcode += writer().set_input_shaping('Y', 0.f,
                                                    (print.calib_params().freqStartY) +
                                                        ((print.calib_params().freqEndY) - (print.calib_params().freqStartY)) *
                                                            (m_layer_index - 2) / (m_layer_count - 3));
            }
        }
        break;
    }
    case CalibMode::Calib_Input_shaping_damp: {
        if (m_layer_index == 1) {
            gcode += writer().set_input_shaping('X', 0.f, print.calib_params().freqStartX);
            gcode += writer().set_input_shaping('Y', 0.f, print.calib_params().freqStartY);
        } else {
            gcode += writer().set_input_shaping('A',
                                                print.calib_params().start + ((print.calib_params().end) - (print.calib_params().start)) *
                                                                                 (m_layer_index) / (m_layer_count),
                                                0.f);
        }
        break;
    }
    case CalibMode::Calib_Junction_Deviation: {
        gcode += writer().set_junction_deviation(print.calib_params().start + ((print.calib_params().end) - (print.calib_params().start)) *
                                                                                  (m_layer_index) / (m_layer_count));
        break;
    }
    }

    // BBS
    if (first_layer) {
        // Orca: we don't need to optimize the Klipper as only set once
        if (this->process_flow_value(m_config.default_acceleration) > 0 && this->process_flow_value(m_config.initial_layer_acceleration) > 0) {
            gcode += m_writer.set_print_acceleration((unsigned int) floor(this->process_flow_value(m_config.initial_layer_acceleration) + 0.5));
        }

        if (this->process_flow_value(m_config.default_jerk) > 0 && this->process_flow_value(m_config.initial_layer_jerk) > 0) {
            gcode += m_writer.set_jerk_xy(this->process_flow_value(m_config.initial_layer_jerk));
        }

        if (m_writer.get_gcode_flavor() == gcfMarlinFirmware && this->process_flow_value(m_config.default_junction_deviation) > 0) {
            gcode += m_writer.set_junction_deviation(this->process_flow_value(m_config.default_junction_deviation));
        }
    }

    if (!first_layer && !m_second_layer_things_done) {
        if (print.is_BBL_printer()) {
            // BBS: open powerlost recovery
            {
                gcode += "; open powerlost recovery\n";
                gcode += "M1003 S1\n";
            }
            // BBS: open first layer inspection at second layer
            if (print.config().scan_first_layer.value) {
                // BBS: retract first to avoid droping when scan model
                gcode += this->retract();
                gcode += "M976 S1 P1 ; scan model before printing 2nd layer\n";
                gcode += "M400 P100\n";
                gcode += this->unretract();
            }
        }
        // Reset acceleration at sencond layer
        // Orca: only set once, don't need to call set_accel_and_jerk
        if (this->process_flow_value(m_config.default_acceleration) > 0 && this->process_flow_value(m_config.initial_layer_acceleration) > 0) {
            gcode += m_writer.set_print_acceleration((unsigned int) floor(this->process_flow_value(m_config.default_acceleration) + 0.5));
        }

        if (this->process_flow_value(m_config.default_jerk) > 0 && this->process_flow_value(m_config.initial_layer_jerk) > 0) {
            gcode += m_writer.set_jerk_xy(this->process_flow_value(m_config.default_jerk));
        }

        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // nozzle_temperature_initial_layer vs. temperature settings.
        if (is_bbl_multi_extruder() && !print.config().single_extruder_multi_material.value) {
            // Bambu two-extruder printer: M104 T is a PHYSICAL hotend, never "T<filament index>". Only
            // the hotend that is printing is set. The idle one gets its temperature from its own tool
            // change ("M620.10 A1 ... P<new_filament_temp>") or, with the idle-nozzle pre-cooling, from
            // the pre-heat timed for its next use; heating it here undid that pre-cool (on a
            // print-by-object plate, at the 2nd layer of every object).
            if (m_writer.extruder() != nullptr) {
                const int filament    = int(m_writer.extruder()->id());
                const int temperature = print.config().nozzle_temperature.get_at(filament);
                if (temperature > 0 && temperature != print.config().nozzle_temperature_initial_layer.get_at(filament))
                    gcode += m_writer.set_temperature(temperature, false, temperature_tool_for_filament(filament));
            }
        } else
        for (const Extruder& extruder : m_writer.extruders()) {
            if ((print.config().single_extruder_multi_material.value || m_ooze_prevention.enable) &&
                extruder.id() != m_writer.extruder()->id())
                // In single extruder multi material mode, set the temperature for the current extruder only.
                continue;
            int temperature = print.config().nozzle_temperature.get_at(extruder.id());
            if (temperature > 0 && temperature != print.config().nozzle_temperature_initial_layer.get_at(extruder.id()))
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }

        // BBS: bed temperature accommodates the highest-temperature filament of the print
        // (max over all used extruders instead of the first extruder of this layer only).
        int bed_temp = get_bed_temperature_max(print, false);
        gcode += m_writer.set_bed_temperature(bed_temp);
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    if (single_object_instance_idx == size_t(-1)) {
        // Normal (non-sequential) print.
        gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, layer_tools.custom_gcode, m_writer.extruder()->id(), first_extruder_id,
                                                             print.config());
    }

    // BBS: get next extruder according to flush and soluble
    auto get_next_extruder = [&](int current_extruder, const std::vector<unsigned int>& extruders) {
        std::vector<float> flush_matrix(cast<float>(m_config.flush_volumes_matrix.values));
        const unsigned int number_of_extruders = (unsigned int) (sqrt(flush_matrix.size()) + EPSILON);
        // Extract purging volumes for each extruder pair:
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < number_of_extruders; ++i)
            wipe_volumes.push_back(
                std::vector<float>(flush_matrix.begin() + i * number_of_extruders, flush_matrix.begin() + (i + 1) * number_of_extruders));
        unsigned int next_extruder = current_extruder;
        float        min_flush     = std::numeric_limits<float>::max();
        for (auto extruder_id : extruders) {
            if (print.config().filament_soluble.get_at(extruder_id) || extruder_id == current_extruder)
                continue;
            if (wipe_volumes[current_extruder][extruder_id] < min_flush) {
                next_extruder = extruder_id;
                min_flush     = wipe_volumes[current_extruder][extruder_id];
            }
        }
        return next_extruder;
    };

    for (const auto& layer_to_print : layers) {
        if (layer_to_print.object_layer) {
            const auto& regions               = layer_to_print.object_layer->regions();
            const bool  enable_overhang_speed = std::any_of(regions.begin(), regions.end(), [](const LayerRegion* r) {
                return r->has_extrusions() && r->region().config().enable_overhang_speed.values.front();
            });
            if (enable_overhang_speed) {
                m_extrusion_quality_estimator.prepare_for_new_layer(layer_to_print.original_object, layer_to_print.object_layer);
            }
        }
    }

    // Group extrusions by an extruder, then by an object, an island and a region.
    std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    bool is_anything_overridden = const_cast<LayerTools&>(layer_tools).wiping_extrusions().is_anything_overridden();
    const double nozzle_0_mm = m_config.nozzle_diameter.values.empty() ? 0.4 : m_config.nozzle_diameter.get_at(0);
    const double pointillism_pixel_size_cfg = std::max(0.0, double(m_config.mixed_filament_pointillism_pixel_size.value));
    const double pointillism_segment_len_mm = pointillism_pixel_size_cfg > EPSILON ?
        std::max(0.10, pointillism_pixel_size_cfg) :
        std::max(0.60, 1.60 * nozzle_0_mm);
    const double pointillism_line_gap_cfg_mm = std::max(0.0, double(m_config.mixed_filament_pointillism_line_gap.value));
    const double pointillism_line_gap_mm = std::min(pointillism_line_gap_cfg_mm, pointillism_segment_len_mm * 0.90);
    const double pointillism_segment_len_scaled = std::max<double>(scale_(0.10), scale_(pointillism_segment_len_mm));
    const double pointillism_line_gap_scaled = std::max<double>(0.0, scale_(pointillism_line_gap_mm));
    std::map<unsigned int, std::vector<unsigned int>> pointillism_sequence_cache;
    size_t pointillism_path_split_entities  = 0;
    size_t pointillism_path_split_segments  = 0;
    size_t pointillism_path_split_fallbacks = 0;

    auto configured_filament_id_1based = [&layer_tools](const GCode::ObjectByExtruder::Island::Region::Type entity_type,
                                                        const ExtrusionEntityCollection&                    entities,
                                                        const PrintRegion&                                  region) -> unsigned int {
        // Phase 4 (image row on WALLS): the per-run tag is now produced for perimeters too, not
        // only for fills, so the check is hoisted above the INFILL gate. It stays exact for every
        // other print because image_row_extruder_1based is 0 on every collection the image row
        // did not create - see ExtrusionEntityCollection::image_row_extruder_1based. A per-layer
        // extruder_override still wins, exactly as it does inside the INFILL branch below.
        if (entity_type != GCode::ObjectByExtruder::Island::Region::INFILL &&
            entities.image_row_extruder_1based != 0 && layer_tools.extruder_override == 0)
            return entities.image_row_extruder_1based;
        if (entity_type == GCode::ObjectByExtruder::Island::Region::INFILL) {
            if (layer_tools.extruder_override != 0)
                return layer_tools.extruder_override;
            // Phase 3 (image row): a per-run collection already knows its own physical filament
            // (0 for every collection in every print that does not use the feature) - see
            // ExtrusionEntityCollection::image_row_extruder_1based. A caller of THIS function
            // (pointillism_sequence_for_filament) uses the return value to look up a
            // SameLayerPointillisme sequence, which an image row is not, so returning the
            // already-physical id here is exactly right: is_mixed() on a physical id is false,
            // so that lookup harmlessly misses rather than misinterpreting it.
            if (entities.image_row_extruder_1based != 0)
                return entities.image_row_extruder_1based;
            const ExtrusionRole role = entities.entities.empty() ? erNone : entities.entities.front()->role();
            switch (fill_filament_source(region.config(), role)) {
            case FillFilamentSource::Wall:        return unsigned(region.config().wall_filament.value);
            case FillFilamentSource::SolidInfill: return unsigned(region.config().solid_infill_filament.value);
            case FillFilamentSource::SparseInfill:
            default:                              return unsigned(region.config().sparse_infill_filament.value);
            }
        }
        return layer_tools.extruder_override == 0 ? unsigned(region.config().wall_filament.value) : layer_tools.extruder_override;
    };

    auto configured_extruder_id = [&layer_tools](const GCode::ObjectByExtruder::Island::Region::Type entity_type,
                                                 const ExtrusionEntityCollection&                    entities,
                                                 const PrintRegion&                                  region) -> int {
        // Phase 4 (image row on WALLS): same hoist as configured_filament_id_1based's just above,
        // for the same reason and with the same "0 unless the feature is in use" guarantee.
        if (entity_type != GCode::ObjectByExtruder::Island::Region::INFILL &&
            entities.image_row_extruder_1based != 0 && layer_tools.extruder_override == 0)
            return int(entities.image_row_extruder_1based) - 1;
        if (entity_type == GCode::ObjectByExtruder::Island::Region::INFILL) {
            // Phase 3 (image row): see configured_filament_id_1based's own comment just above -
            // same field, same "0 unless the feature is in use" guarantee, and the same
            // per-layer extruder_override precedence (an override still wins over a run's own
            // filament, exactly as it wins over the region's nominal one).
            if (entities.image_row_extruder_1based != 0 && layer_tools.extruder_override == 0)
                return int(entities.image_row_extruder_1based) - 1;
            const ExtrusionRole role = entities.entities.empty() ? erNone : entities.entities.front()->role();
            switch (fill_filament_source(region.config(), role)) {
            case FillFilamentSource::Wall:        return int(layer_tools.wall_filament(region));
            case FillFilamentSource::SolidInfill: return int(layer_tools.solid_infill_filament(region));
            case FillFilamentSource::SparseInfill:
            default:                              return int(layer_tools.sparse_infill_filament(region));
            }
        }
        return int(layer_tools.wall_filament(region));
    };

    auto pointillism_sequence_for_filament = [&](unsigned int filament_id_1based) -> const std::vector<unsigned int>* {
        if (filament_id_1based == 0 || layer_tools.mixed_mgr == nullptr || layer_tools.num_physical == 0)
            return nullptr;
        auto cache_it = pointillism_sequence_cache.find(filament_id_1based);
        if (cache_it != pointillism_sequence_cache.end())
            return cache_it->second.empty() ? nullptr : &cache_it->second;

        std::vector<unsigned int> sequence;
        if (layer_tools.mixed_mgr->is_mixed(filament_id_1based, layer_tools.num_physical)) {
            const MixedFilament* mixed_row = layer_tools.mixed_mgr->mixed_filament_from_id(filament_id_1based, layer_tools.num_physical);
            if (mixed_row != nullptr)
                sequence = pointillism_sequence_for_row_for_gcode(*mixed_row, layer_tools.num_physical);
            if (unique_extruder_count_for_gcode(sequence, layer_tools.num_physical) < 2)
                sequence.clear();
        }

        auto inserted = pointillism_sequence_cache.emplace(filament_id_1based, std::move(sequence));
        return inserted.first->second.empty() ? nullptr : &inserted.first->second;
    };
    auto grouped_manual_pattern_mixed_filament_id =
        [&layer_tools, &configured_filament_id_1based](const GCode::ObjectByExtruder::Island::Region::Type entity_type,
                                                       const ExtrusionEntityCollection&                    entities,
                                                       const PrintRegion&                                  region) -> unsigned int {
        if (layer_tools.mixed_mgr == nullptr || layer_tools.num_physical == 0)
            return 0;

        auto has_grouped_pattern = [&layer_tools](unsigned int filament_id_1based) -> bool {
            if (!layer_tools.mixed_mgr->is_mixed(filament_id_1based, layer_tools.num_physical))
                return false;
            const MixedFilament* mixed_row = layer_tools.mixed_mgr->mixed_filament_from_id(filament_id_1based, layer_tools.num_physical);
            if (mixed_row == nullptr)
                return false;
            const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(mixed_row->manual_pattern);
            return normalized_pattern.find(',') != std::string::npos;
        };

        const unsigned int configured_filament_id = configured_filament_id_1based(entity_type, entities, region);
        if (has_grouped_pattern(configured_filament_id))
            return configured_filament_id;
        return 0;
    };
    // Compensate perimeter clipping at mixed-mask boundaries to avoid cracks from exact centerline clipping.
    constexpr double LOCAL_Z_PERIMETER_MASK_EXPAND_MM = 0.10;
    // Keep base exclusion smaller than mixed-pass inclusion to guarantee a slight overlap
    // instead of a moat at the boundary.
    constexpr double LOCAL_Z_BASE_MASK_EXPAND_MM      = 0.04;
    const float      local_z_perimeter_mask_expand    = float(scale_(LOCAL_Z_PERIMETER_MASK_EXPAND_MM));
    const float      local_z_base_mask_expand         = float(scale_(LOCAL_Z_BASE_MASK_EXPAND_MM));
    const bool       local_z_whole_objects_enabled    = print.full_print_config().opt_bool("dithering_local_z_whole_objects");
    const bool       local_z_infill_enabled           = print.full_print_config().opt_bool("dithering_local_z_infill");

    struct LocalZPassBucket {
        const SubLayerPlan*                                   plan { nullptr };
        std::vector<ExPolygons>                               compensated_masks_by_extruder;
        std::map<unsigned int, std::vector<ObjectByExtruder>> by_extruder;
    };
    struct LocalZLayerContext {
        bool                    enabled { false };
        ExPolygons              raw_mixed_masks_union;
        ExPolygons              mixed_masks_union;
        ExPolygons              mixed_masks_union_for_base_exclude;
        size_t                  local_clipped_collections { 0 };
        size_t                  base_clipped_collections { 0 };
        size_t                  base_clip_leak_warnings { 0 };
        std::vector<LocalZPassBucket> pass_buckets;
    };

    // Local-Z phase-b can run with wipe tower enabled, but wiping-overrides remain unsupported
    // because that flow emits a separate purge/print pass that is not Local-Z aware yet.
    const bool local_z_perimeter_runtime_supported = !is_anything_overridden;
    bool       local_z_phase_b_requested_for_layer = false;
    std::vector<LocalZLayerContext> local_z_layer_contexts;
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> local_z_clipped_collections;
    size_t local_z_rejected_context_logs = 0;
    if (local_z_perimeter_runtime_supported) {
        local_z_layer_contexts.resize(layers.size());
        for (size_t layer_to_print_idx = 0; layer_to_print_idx < layers.size(); ++layer_to_print_idx) {
            const LayerToPrint& layer_to_print = layers[layer_to_print_idx];
            if (layer_to_print.object_layer == nullptr)
                continue;

            const PrintObject* print_object = layer_to_print.original_object != nullptr ? layer_to_print.original_object : layer_to_print.object();
            if (print_object == nullptr)
                continue;

            const size_t layer_id = size_t(layer_to_print.object_layer->id());
            const auto& intervals = print_object->local_z_intervals();
            const auto& plans     = print_object->local_z_sublayer_plan();
            if (intervals.empty() || plans.empty())
                continue;

            auto it_interval = std::find_if(intervals.begin(), intervals.end(), [layer_id](const LocalZInterval& interval) {
                return interval.layer_id == layer_id;
            });
            if (it_interval == intervals.end())
                continue;
            if (!it_interval->has_mixed_paint || it_interval->sublayer_count <= 1 || it_interval->first_sublayer_idx >= plans.size())
                continue;
            local_z_phase_b_requested_for_layer = true;

            LocalZLayerContext& ctx = local_z_layer_contexts[layer_to_print_idx];
            ctx.enabled = true;
            const size_t first_idx = it_interval->first_sublayer_idx;
            const size_t end_idx   = std::min(plans.size(), first_idx + it_interval->sublayer_count);
            size_t       split_plan_count                    = 0;
            size_t       split_plan_with_raw_masks           = 0;
            size_t       split_plan_with_compensated_masks   = 0;
            size_t       raw_mask_polygon_count              = 0;
            size_t       compensated_mask_polygon_count      = 0;
            ExPolygons   raw_mixed_masks_union;
            for (size_t plan_idx = first_idx; plan_idx < end_idx; ++plan_idx) {
                const SubLayerPlan& plan = plans[plan_idx];
                if (!plan.split_interval)
                    continue;
                ++split_plan_count;

                LocalZPassBucket bucket;
                bucket.plan = &plan;
                const size_t plan_mask_slots = std::max(plan.painted_masks_by_extruder.size(), plan.fixed_painted_masks_by_extruder.size());
                bucket.compensated_masks_by_extruder.assign(plan_mask_slots, ExPolygons());
                bool pass_has_raw_masks         = false;
                bool pass_has_compensated_masks = false;
                ExPolygons fixed_raw_masks_union;
                for (const ExPolygons &fixed_masks : plan.fixed_painted_masks_by_extruder)
                    if (!fixed_masks.empty())
                        append(fixed_raw_masks_union, fixed_masks);
                if (!fixed_raw_masks_union.empty() && fixed_raw_masks_union.size() > 1)
                    fixed_raw_masks_union = union_ex(fixed_raw_masks_union);
                const ExPolygons fixed_compensated_guard =
                    fixed_raw_masks_union.empty() ? ExPolygons() : local_z_compensate_masks(fixed_raw_masks_union, local_z_perimeter_mask_expand, true);

                for (size_t extruder_id = 0; extruder_id < plan_mask_slots; ++extruder_id) {
                    const ExPolygons mixed_raw_masks =
                        extruder_id < plan.painted_masks_by_extruder.size() ? plan.painted_masks_by_extruder[extruder_id] : ExPolygons();
                    const ExPolygons fixed_raw_masks =
                        extruder_id < plan.fixed_painted_masks_by_extruder.size() ? plan.fixed_painted_masks_by_extruder[extruder_id] : ExPolygons();
                    if (mixed_raw_masks.empty() && fixed_raw_masks.empty())
                        continue;
                    pass_has_raw_masks = true;
                    raw_mask_polygon_count += mixed_raw_masks.size() + fixed_raw_masks.size();
                    if (!mixed_raw_masks.empty())
                        append(raw_mixed_masks_union, mixed_raw_masks);
                    if (!fixed_raw_masks.empty())
                        append(raw_mixed_masks_union, fixed_raw_masks);

                    ExPolygons compensated;
                    if (!mixed_raw_masks.empty()) {
                        ExPolygons compensated_mixed = local_z_compensate_masks(mixed_raw_masks, local_z_perimeter_mask_expand, true);
                        if (local_z_whole_objects_enabled && !fixed_compensated_guard.empty())
                            compensated_mixed = diff_ex(compensated_mixed, fixed_compensated_guard);
                        if (!compensated_mixed.empty())
                            append(compensated, compensated_mixed);
                    }
                    if (!fixed_raw_masks.empty())
                        append(compensated, fixed_raw_masks);
                    if (!compensated.empty() && compensated.size() > 1)
                        compensated = union_ex(compensated);
                    if (!compensated.empty()) {
                        pass_has_compensated_masks = true;
                        compensated_mask_polygon_count += compensated.size();
                    }
                    bucket.compensated_masks_by_extruder[extruder_id] = std::move(compensated);
                }
                if (pass_has_raw_masks)
                    ++split_plan_with_raw_masks;
                if (pass_has_compensated_masks) {
                    ++split_plan_with_compensated_masks;
                    ctx.pass_buckets.emplace_back(std::move(bucket));

                    const LocalZPassBucket& appended_bucket = ctx.pass_buckets.back();
                    for (const ExPolygons& masks : appended_bucket.compensated_masks_by_extruder)
                        append(ctx.mixed_masks_union, masks);
                }
            }
            if (!raw_mixed_masks_union.empty() && raw_mixed_masks_union.size() > 1)
                raw_mixed_masks_union = union_ex(raw_mixed_masks_union);
            ctx.raw_mixed_masks_union = raw_mixed_masks_union;
            if (!ctx.mixed_masks_union.empty() && ctx.mixed_masks_union.size() > 1) {
                ExPolygons merged_masks = union_ex(ctx.mixed_masks_union);
                if (!merged_masks.empty())
                    ctx.mixed_masks_union = std::move(merged_masks);
                else if (!raw_mixed_masks_union.empty())
                    ctx.mixed_masks_union = raw_mixed_masks_union;
            }
            if (ctx.mixed_masks_union.empty() && !raw_mixed_masks_union.empty())
                ctx.mixed_masks_union = raw_mixed_masks_union;
            const ExPolygons &base_exclude_source = !ctx.raw_mixed_masks_union.empty() ? ctx.raw_mixed_masks_union : ctx.mixed_masks_union;
            if (!base_exclude_source.empty()) {
                ctx.mixed_masks_union_for_base_exclude =
                    local_z_compensate_masks(base_exclude_source, local_z_base_mask_expand, true);
            }
            if (ctx.pass_buckets.empty() || ctx.mixed_masks_union.empty()) {
                ctx.enabled = false;
                if (local_z_rejected_context_logs < 50) {
                    ++local_z_rejected_context_logs;
                    BOOST_LOG_TRIVIAL(warning) << "Local-Z context rejected"
                                               << " print_z=" << print_z
                                               << " layer_id=" << layer_id
                                               << " first_idx=" << first_idx
                                               << " end_idx=" << end_idx
                                               << " interval_sublayer_count=" << it_interval->sublayer_count
                                               << " split_plan_count=" << split_plan_count
                                               << " split_plan_with_raw_masks=" << split_plan_with_raw_masks
                                               << " split_plan_with_compensated_masks=" << split_plan_with_compensated_masks
                                               << " raw_mask_polygon_count=" << raw_mask_polygon_count
                                               << " compensated_mask_polygon_count=" << compensated_mask_polygon_count
                                               << " pass_buckets=" << ctx.pass_buckets.size()
                                               << " mixed_mask_count=" << ctx.mixed_masks_union.size();
                }
            }
            BOOST_LOG_TRIVIAL(debug) << "Local-Z context"
                                     << " print_z=" << print_z
                                     << " layer_id=" << layer_id
                                     << " enabled=" << ctx.enabled
                                     << " split_pass_count=" << ctx.pass_buckets.size()
                                     << " mixed_mask_count=" << ctx.mixed_masks_union.size()
                                     << " base_exclude_mask_count=" << ctx.mixed_masks_union_for_base_exclude.size()
                                     << " perimeter_mask_expand_mm=" << LOCAL_Z_PERIMETER_MASK_EXPAND_MM
                                     << " base_mask_expand_mm=" << LOCAL_Z_BASE_MASK_EXPAND_MM;
        }
    } else {
        for (const LayerToPrint& layer_to_print : layers) {
            if (layer_to_print.object_layer == nullptr)
                continue;
            const PrintObject* print_object = layer_to_print.original_object != nullptr ? layer_to_print.original_object : layer_to_print.object();
            if (print_object == nullptr)
                continue;
            const size_t layer_id = size_t(layer_to_print.object_layer->id());
            const auto& intervals = print_object->local_z_intervals();
            auto it_interval = std::find_if(intervals.begin(), intervals.end(), [layer_id](const LocalZInterval& interval) {
                return interval.layer_id == layer_id;
            });
            if (it_interval != intervals.end() && it_interval->has_mixed_paint && it_interval->sublayer_count > 1) {
                local_z_phase_b_requested_for_layer = true;
                break;
            }
        }
    }

    const bool local_z_perimeter_phase_b_enabled =
        local_z_perimeter_runtime_supported &&
        std::any_of(local_z_layer_contexts.begin(), local_z_layer_contexts.end(), [](const LocalZLayerContext& ctx) { return ctx.enabled; });
    if (local_z_phase_b_requested_for_layer && !local_z_perimeter_runtime_supported) {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z phase-b disabled"
                                   << " print_z=" << print_z
                                   << " wipe_tower=" << has_wipe_tower
                                   << " wiping_overrides=" << is_anything_overridden;
        gcode += "; local-z perimeter phase-b disabled for this layer (wiping overrides active)\n";
    } else if (local_z_phase_b_requested_for_layer && !local_z_perimeter_phase_b_enabled) {
        BOOST_LOG_TRIVIAL(warning) << "Local-Z phase-b requested but no eligible contexts"
                                   << " print_z=" << print_z
                                   << " runtime_supported=" << local_z_perimeter_runtime_supported;
    }

    for (const LayerToPrint& layer_to_print : layers) {
        const size_t layer_to_print_idx = &layer_to_print - layers.data();
        if (layer_to_print.support_layer != nullptr) {
            const SupportLayer& support_layer = *layer_to_print.support_layer;
            const PrintObject&  object        = *layer_to_print.original_object;
            if (!support_layer.support_fills.entities.empty()) {
                ExtrusionRole role          = support_layer.support_fills.role();
                bool          has_support   = role == erMixed || role == erSupportMaterial || role == erSupportTransition;
                // v2.2 Task 3 (spec C6, third anchor): mirrors ToolOrdering.cpp's own
                // has_interface classification (~701-703, shared predicate in
                // BrimFilament.hpp/.cpp) - kept identical so the two duplicated role
                // classifications can't drift. In THIS specific block the erIroning
                // addition is inert for `single_extruder`/the bucket-key choice just
                // below (both are driven by has_support, which erIroning was never part
                // of, not by has_interface), but the mirror comment at this block's own
                // site (spec, "GCode support-bucket mirror check") is the reason to keep
                // it in lock-step with ToolOrdering.cpp's copy regardless.
                bool          has_interface = support_role_needs_interface_extruder(role);
                // Extruder ID of the support base. -1 if "don't care".
                unsigned int support_extruder = object.config().support_filament.value - 1;
                // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool support_dontcare = object.config().support_filament.value == 0;
                // Extruder ID of the support interface. -1 if "don't care".
                unsigned int interface_extruder = object.config().support_interface_filament.value - 1;
                // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
                bool interface_dontcare = object.config().support_interface_filament.value == 0;

                // Ultra (support groups, plan 2026-09-02 2c): the extruders a support group pins
                // for ITS OWN part's interface. They live in SupportLayer::interface_by_extruder
                // and are emitted from there; everything still in support_fills belongs to the
                // other groups and must never borrow them. Without this, an object whose support
                // filament is "don't care" resolved both the base and the neighbouring part's
                // interface to whatever extruder happened to be first/active on the layer - which,
                // once a group had forced its own filament onto that layer, was the group's. That
                // is what put the group's colour under every part. Empty for every object with no
                // such group, so nothing below this line can move for them.
                const std::vector<unsigned int> group_interface_extruders = object.support_group_interface_extruders();
                auto is_group_pinned = [&group_interface_extruders](unsigned int extruder_id) {
                    return std::binary_search(group_interface_extruders.begin(), group_interface_extruders.end(), extruder_id);
                };

                // BBS: apply wiping overridden extruders
                WipingExtrusions& wiping_extrusions = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                if (support_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        support_extruder = extruder_override;
                        support_dontcare = false;
                    }
                }

                if (interface_dontcare) {
                    int extruder_override = wiping_extrusions.get_support_interface_extruder_overrides(&object);
                    if (extruder_override >= 0) {
                        interface_extruder = extruder_override;
                        interface_dontcare = false;
                    }
                }

                // v2.5a Task 2b (spec: "mechanism B pin" - promoted from the original
                // spec's named "known remaining gap" to REQUIRED once the user's own
                // profile confirmed flush_into_support/flush_into_infill are both off:
                // mechanism A's WipingExtrusions override just above never fires for a
                // mode-active object in that profile shape - is_support_overriddable's
                // own v2.5a residual pin, ToolOrdering.cpp, already makes both
                // get_support_extruder_overrides/get_support_interface_extruder_
                // overrides above return -1 unconditionally for such an object,
                // flush_into_support or not - so mechanism B below - the "don't care"
                // resolution falling through to whatever extruder happens to be
                // first/active on THIS SPECIFIC layer - is the actual, dominant cause
                // of the reported khaki-randomly-mixed-into-white/teal-support
                // symptom: it varies layer to layer with toolchange order, with no
                // relationship to what geometry is actually nearby).
                //
                // A mode-active object (support_filament_matching.value) whose
                // support layer has at least one matched bucket
                // (support_layer.interface_by_extruder, populated per-layer by
                // chameleon_assign_support_interfaces, Print.cpp) pins any STILL-
                // "don't care" residual slot(s) - base and/or interface, whichever of
                // the two remain dontcare at this point - to that layer's DOMINANT
                // matched bucket's extruder instead: the bucket with the largest
                // total_path_length_mm (ties -> lowest extruder id, deterministic).
                // Both slots share the SAME one dominant extruder - interface_by_
                // extruder is a single map holding both roles' matched buckets (v2.1
                // Task 3), not two separate per-role maps, so there is only one
                // "dominant bucket" per layer to pick from. This introduces NO new
                // toolchange: that extruder is already registered on this exact layer
                // via ToolOrdering.cpp's own interface_by_extruder registration loop
                // (collect_extruders, ~734-736), since the bucket exists there in the
                // first place.
                //
                // A wholly-fallback layer (interface_by_extruder empty - nothing
                // matched on this layer at all) is untouched by this and keeps
                // falling through to the pre-v2.5a first/active-extruder rule below -
                // a documented, now much smaller, remaining gap (see this task's own
                // report: still deterministic per run, but layer-varying, same as
                // before this fix).
                //
                // Ultra (support groups, plan 2026-09-02 2c): the pin reads interface_by_extruder
                // as "what Chameleon matched on this layer", but since Stage 3 a support group's
                // own interface filament writes that very same map (SupportCommon.cpp) - and for
                // such an object the Chameleon pass itself has already stood down (Print.cpp), so
                // every bucket there is a GROUP's, not a match. Pinning to it handed the layer's
                // whole don't-care support - base and the other parts' interface alike - to the
                // group's filament, which is exactly the "the group's colour is under both parts"
                // report. The interlock is the same one the Chameleon pass, WipingExtrusions and
                // is_support_overriddable already use.
                if (object.config().support_filament_matching.value
                    && group_interface_extruders.empty()
                    && (support_dontcare || interface_dontcare)) {
                    // chameleon_dominant_matched_extruder (BrimFilament.hpp/.cpp) is
                    // the pure/testable core of this pin: largest total_path_length_mm
                    // bucket, ties -> lowest extruder id, -1 when interface_by_extruder
                    // is empty or holds only empty buckets (nothing matched this
                    // layer - falls through to the pre-v2.5a rule below unchanged).
                    const int dominant_extruder = chameleon_dominant_matched_extruder(support_layer.interface_by_extruder);
                    if (dominant_extruder >= 0) {
                        if (support_dontcare) {
                            support_extruder   = unsigned(dominant_extruder);
                            support_dontcare   = false;
                        }
                        if (interface_dontcare) {
                            interface_extruder = unsigned(dominant_extruder);
                            interface_dontcare = false;
                        }
                    }
                }

                // Support filament matching: a layer with nothing matched (no bucket for the pin
                // above) - the plate layer with the support brim pads, a layer without wall
                // samples, a layer whose buckets were all gated away - prints its leftover support
                // in the filament the pass chose for it (SupportLayer::chameleon_residual_extruder,
                // Print.cpp) instead of whatever extruder happens to be active. The plate layer
                // without a raft (chameleon_residual_all_roles) takes it for a configured filament
                // too, so its pads print with the columns standing on them. -1 on every layer of
                // an object that does not match, so nothing changes there. ToolOrdering's
                // collect_extruders registers the same extruder for the same condition.
                if (support_layer.chameleon_residual_extruder >= 0) {
                    const unsigned int pinned = unsigned(support_layer.chameleon_residual_extruder);
                    if (support_dontcare || support_layer.chameleon_residual_all_roles) {
                        support_extruder = pinned;
                        support_dontcare = false;
                    }
                    if (interface_dontcare || support_layer.chameleon_residual_all_roles) {
                        interface_extruder = pinned;
                        interface_dontcare = false;
                    }
                }

                // BBS: try to print support base with a filament other than interface filament
                if (support_dontcare && !interface_dontcare) {
                    unsigned int dontcare_extruder = first_extruder_id;
                    for (unsigned int extruder_id : layer_tools.extruders) {
                        if (print.config().filament_soluble.get_at(extruder_id))
                            continue;

                        // BBS: now we don't consider interface filament used in other object
                        if (extruder_id == interface_extruder)
                            continue;
                        // Ultra (support groups): a group's own interface filament is not a
                        // candidate for anybody else's support base.
                        if (is_group_pinned(extruder_id))
                            continue;

                        dontcare_extruder = extruder_id;
                        break;
                    }
#if 0
                    //BBS: not found a suitable extruder in current layer ,dontcare_extruider==first_extruder_id==interface_extruder
                    if (dontcare_extruder == interface_extruder && (object.config().support_interface_not_for_body && object.config().support_interface_filament.value!=0)) {
                        // BBS : get a suitable extruder from other layer
                        auto all_extruders = print.extruders();
                        dontcare_extruder = get_next_extruder(dontcare_extruder, all_extruders);
                    }
#endif

                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                } else if (support_dontcare || interface_dontcare) {
                    // Some support will be printed with "don't care" material, preferably non-soluble.
                    // Is the current extruder assigned a soluble filament?
                    unsigned int dontcare_extruder = first_extruder_id;
                    // Ultra (support groups): ... or the first/active extruder is one a support
                    // group pinned for its own interface, in which case falling through to it
                    // would paint this object's other support with the group's filament.
                    if (print.config().filament_soluble.get_at(dontcare_extruder) || is_group_pinned(dontcare_extruder)) {
                        // The last extruder printed on the previous layer extrudes soluble filament.
                        // Try to find a non-soluble extruder on the same layer.
                        for (unsigned int extruder_id : layer_tools.extruders)
                            if (!print.config().filament_soluble.get_at(extruder_id) && !is_group_pinned(extruder_id)) {
                                dontcare_extruder = extruder_id;
                                break;
                            }
                    }
                    if (support_dontcare)
                        support_extruder = dontcare_extruder;
                    if (interface_dontcare)
                        interface_extruder = dontcare_extruder;
                }
                // Both the support and the support interface are printed with the same extruder, therefore
                // the interface may be interleaved with the support base.
                bool single_extruder = !has_support || support_extruder == interface_extruder;
                // Assign an extruder to the base.
                ObjectByExtruder& obj      = object_by_extruder(by_extruder, has_support ? support_extruder : interface_extruder,
                                                                layer_to_print_idx, layers.size());
                obj.support                = &support_layer.support_fills;
                obj.support_extrusion_role = single_extruder ? erMixed : erSupportMaterial;
                if (!single_extruder && has_interface) {
                    ObjectByExtruder& obj_interface = object_by_extruder(by_extruder, interface_extruder, layer_to_print_idx, layers.size());
                    obj_interface.support           = &support_layer.support_fills;
                    obj_interface.support_extrusion_role = erSupportMaterialInterface;
                }
            }
        }

        if (layer_to_print.object_layer != nullptr) {
            const Layer& layer = *layer_to_print.object_layer;
            LocalZLayerContext* local_z_ctx =
                (local_z_perimeter_phase_b_enabled && layer_to_print_idx < local_z_layer_contexts.size() && local_z_layer_contexts[layer_to_print_idx].enabled)
                    ? &local_z_layer_contexts[layer_to_print_idx]
                    : nullptr;
            // We now define a strategy for building perimeters and fills. The separation
            // between regions doesn't matter in terms of printing order, as we follow
            // another logic instead:
            // - we group all extrusions by extruder so that we minimize toolchanges
            // - we start from the last used extruder
            // - for each extruder, we group extrusions by island
            // - for each island, we extrude perimeters first, unless user set the infill_first
            //   option
            // (Still, we have to keep track of regions because we need to apply their config)
            size_t                          n_slices             = layer.lslices.size();
            const std::vector<BoundingBox>& layer_surface_bboxes = layer.lslices_bboxes;
            // Traverse the slices in an increasing order of bounding box size, so that the islands inside another islands are tested first,
            // so we can just test a point inside ExPolygon::contour and we may skip testing the holes.
            std::vector<size_t> slices_test_order;
            slices_test_order.reserve(n_slices);
            for (size_t i = 0; i < n_slices; ++i)
                slices_test_order.emplace_back(i);
            std::sort(slices_test_order.begin(), slices_test_order.end(), [&layer_surface_bboxes](size_t i, size_t j) {
                const Vec2d s1 = layer_surface_bboxes[i].size().cast<double>();
                const Vec2d s2 = layer_surface_bboxes[j].size().cast<double>();
                return s1.x() * s1.y() < s2.x() * s2.y();
            });
            auto point_inside_surface = [&layer, &layer_surface_bboxes](const size_t i, const Point& point) {
                const BoundingBox& bbox = layer_surface_bboxes[i];
                return point(0) >= bbox.min(0) && point(0) < bbox.max(0) && point(1) >= bbox.min(1) && point(1) < bbox.max(1) &&
                       layer.lslices[i].contour.contains(point);
            };
            auto entity_matches_surface = [&point_inside_surface](const size_t i, const ExtrusionEntity& entity) {
                if (point_inside_surface(i, entity.first_point()))
                    return true;

                Polylines polylines;
                entity.collect_polylines(polylines);
                for (const Polyline& polyline : polylines) {
                    if (polyline.points.size() >= 2) {
                        const Point midpoint = (polyline.points.front() + polyline.points[1]) / 2;
                        if (point_inside_surface(i, midpoint))
                            return true;
                    }
                }

                Points points;
                entity.collect_points(points);
                if (!points.empty()) {
                    BoundingBox bbox(points);
                    if (bbox.defined && point_inside_surface(i, bbox.center()))
                        return true;
                }

                return false;
            };
            LocalZLoopSeamPlacer local_z_loop_seam_placer =
                [this, &layer](const ExtrusionLoop& src_loop, ExtrusionLoop& seam_loop, Point& seam_anchor) -> bool {
                    seam_loop = src_loop;
                    if (seam_loop.paths.empty() || m_config.spiral_mode)
                        return false;

                    const Point seam_reference = this->last_pos_defined() ? this->last_pos() : seam_loop.first_point();
                    float       seam_overhang  = std::numeric_limits<float>::lowest();
                    m_seam_placer.place_seam(&layer, seam_loop, seam_reference, seam_overhang);
                    seam_anchor = seam_loop.first_point();
                    return true;
                };

            // Phase 4 (image row on WALLS): one ImageRowWallContext per PrintRegion per layer,
            // built lazily and memoised - building it decodes the row's image reference and walks
            // the owning volume's vertices for a bounding box, which must not happen once per
            // loop. A region that is not an image-row wall memoises a null and is never retried.
            // The whole cache stays empty for every print that does not use the feature.
            std::vector<std::pair<const PrintRegion*, std::unique_ptr<ImageRowWallContext>>> image_row_wall_ctx_cache;
            const PrintObject* image_row_wall_object =
                layer_to_print.original_object != nullptr ? layer_to_print.original_object : layer_to_print.object();
            // A stand-in for an object with no Model behind it (never happens in a real slice;
            // the split simply finds no image and leaves the wall alone).
            static const ImageAssetStore image_row_wall_no_assets;
            const ImageAssetStore& image_row_wall_assets =
                (image_row_wall_object != nullptr && image_row_wall_object->model_object() != nullptr &&
                 image_row_wall_object->model_object()->get_model() != nullptr)
                    ? image_row_wall_object->model_object()->get_model()->image_assets
                    : image_row_wall_no_assets;
            auto image_row_wall_ctx_for = [&](const PrintRegion& region) -> const ImageRowWallContext* {
                if (image_row_wall_object == nullptr)
                    return nullptr;
                for (const auto& entry : image_row_wall_ctx_cache)
                    if (entry.first == &region)
                        return entry.second.get();
                std::unique_ptr<ImageRowWallContext> built;
                if (image_row_wall_configured_virtual_id(*image_row_wall_object, region) != 0) {
                    auto ctx = std::make_unique<ImageRowWallContext>();
                    // The external perimeter's own extrusion width sets the sampling resolution,
                    // so a wall dithers at exactly the granularity a top surface does.
                    float ext_width = 0.f;
                    for (const LayerRegion* lr : layer.regions())
                        if (lr != nullptr && &lr->region() == &region) {
                            ext_width = float(lr->flow(frExternalPerimeter).width());
                            break;
                        }
                    if (image_row_wall_context_for_region(*image_row_wall_object, region, ext_width, *ctx))
                        built = std::move(ctx);
                }
                image_row_wall_ctx_cache.emplace_back(&region, std::move(built));
                return image_row_wall_ctx_cache.back().second.get();
            };

            for (size_t region_id = 0; region_id < layer.regions().size(); ++region_id) {
                const LayerRegion* layerm = layer.regions()[region_id];
                if (layerm == nullptr)
                    continue;
                // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
                const PrintRegion& region = print.get_print_region(layerm->region().print_region_id());

                // Now we must process perimeters and infills and create islands of extrusions in by_region std::map.
                // It is also necessary to save which extrusions are part of MM wiping and which are not.
                // The process is almost the same for perimeters and infills - we will do it in a cycle that repeats twice:
                std::vector<unsigned int> printing_extruders;
                for (const ObjectByExtruder::Island::Region::Type entity_type :
                     {ObjectByExtruder::Island::Region::INFILL, ObjectByExtruder::Island::Region::PERIMETERS}) {
                    for (const ExtrusionEntity* ee :
                         (entity_type == ObjectByExtruder::Island::Region::INFILL) ? layerm->fills.entities : layerm->perimeters.entities) {
                        // extrusions represents infill or perimeter extrusions of a single island.
                        assert(dynamic_cast<const ExtrusionEntityCollection*>(ee) != nullptr);
                        const auto* extrusions = static_cast<const ExtrusionEntityCollection*>(ee);
                        if (extrusions->entities.empty()) // This shouldn't happen but first_point() would fail.
                            continue;

                        const ExtrusionEntityCollection* filtered_extrusions = extrusions;
                        const bool local_z_entity_enabled =
                            entity_type == ObjectByExtruder::Island::Region::PERIMETERS ||
                            (local_z_infill_enabled && entity_type == ObjectByExtruder::Island::Region::INFILL);
                        if (local_z_entity_enabled && local_z_ctx != nullptr && !local_z_ctx->mixed_masks_union.empty()) {
                            for (LocalZPassBucket& pass_bucket : local_z_ctx->pass_buckets) {
                                if (pass_bucket.plan == nullptr)
                                    continue;
                                for (size_t pass_extruder_id = 0; pass_extruder_id < pass_bucket.plan->painted_masks_by_extruder.size();
                                     ++pass_extruder_id) {
                                    const ExPolygons& pass_masks = pass_bucket.compensated_masks_by_extruder.empty()
                                        ? pass_bucket.plan->painted_masks_by_extruder[pass_extruder_id]
                                        : pass_bucket.compensated_masks_by_extruder[pass_extruder_id];
                                    if (pass_masks.empty())
                                        continue;
                                    auto clipped_local = clip_extrusion_collection_for_local_z(*extrusions,
                                                                                               &pass_masks,
                                                                                               nullptr,
                                                                                               pass_bucket.plan->flow_height,
                                                                                               &local_z_loop_seam_placer);
                                    if (!clipped_local)
                                        continue;

                                    const ExtrusionEntityCollection* clipped_ptr = clipped_local.get();
                                    const LocalZPathHeightStats local_height_stats = collect_local_z_path_height_stats(*clipped_ptr);
                                    ++local_z_ctx->local_clipped_collections;
                                    if (local_height_stats.count > 0 &&
                                        (std::abs(local_height_stats.min - pass_bucket.plan->flow_height) > 1e-3 ||
                                         std::abs(local_height_stats.max - pass_bucket.plan->flow_height) > 1e-3)) {
                                        BOOST_LOG_TRIVIAL(warning) << "Local-Z local pass height mismatch"
                                                                   << " print_z=" << print_z
                                                                   << " layer_to_print_idx=" << layer_to_print_idx
                                                                   << " plan_layer_id=" << pass_bucket.plan->layer_id
                                                                   << " pass_index=" << pass_bucket.plan->pass_index
                                                                   << " expected_height=" << pass_bucket.plan->flow_height
                                                                   << " observed_min=" << local_height_stats.min
                                                                   << " observed_max=" << local_height_stats.max
                                                                   << " path_count=" << local_height_stats.count;
                                    }
                                    local_z_clipped_collections.emplace_back(std::move(clipped_local));
                                    std::vector<ObjectByExtruder::Island>& islands = object_islands_by_extruder(
                                        pass_bucket.by_extruder, unsigned(pass_extruder_id), layer_to_print_idx, layers.size(), n_slices + 1);

                                    for (size_t i = 0; i <= n_slices; ++i) {
                                        const bool   last       = i == n_slices;
                                        const size_t island_idx = last ? n_slices : slices_test_order[i];
                                        if (last || entity_matches_surface(island_idx, *clipped_ptr)) {
                                            if (islands[island_idx].by_region.empty())
                                                islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                            islands[island_idx].by_region[region.print_region_id()].append(entity_type, clipped_ptr, nullptr);
                                            break;
                                        }
                                    }
                                }
                            }

                            const ExPolygons* base_exclude_masks =
                                local_z_ctx->mixed_masks_union_for_base_exclude.empty() ? &local_z_ctx->mixed_masks_union
                                                                                        : &local_z_ctx->mixed_masks_union_for_base_exclude;
                            auto clipped_base = clip_extrusion_collection_for_local_z(*extrusions,
                                                                                     nullptr,
                                                                                     base_exclude_masks,
                                                                                     0.,
                                                                                     &local_z_loop_seam_placer);
                            if (!clipped_base)
                                continue;
                            const LocalZPathHeightStats base_height_stats = collect_local_z_path_height_stats(*clipped_base);
                            ++local_z_ctx->base_clipped_collections;
                            const ExPolygons &mixed_leak_ref =
                                !local_z_ctx->raw_mixed_masks_union.empty() ? local_z_ctx->raw_mixed_masks_union : local_z_ctx->mixed_masks_union;
                            if (local_z_ctx->base_clip_leak_warnings < 3 && !mixed_leak_ref.empty()) {
                                Polylines clipped_base_lines = collect_local_z_polylines(*clipped_base);
                                if (!clipped_base_lines.empty()) {
                                    Polylines leaked_segments = intersection_pl(std::move(clipped_base_lines), mixed_leak_ref);
                                    if (!leaked_segments.empty()) {
                                        ++local_z_ctx->base_clip_leak_warnings;
                                        BOOST_LOG_TRIVIAL(warning) << "Local-Z base clip leak"
                                                                   << " print_z=" << print_z
                                                                   << " layer_to_print_idx=" << layer_to_print_idx
                                                                   << " leaked_segment_count=" << leaked_segments.size()
                                                                   << " warn_index=" << local_z_ctx->base_clip_leak_warnings;
                                    }
                                }
                            }
                            if (base_height_stats.count > 0 && local_z_ctx->base_clipped_collections <= 5) {
                                BOOST_LOG_TRIVIAL(debug) << "Local-Z base clip heights"
                                                         << " print_z=" << print_z
                                                         << " layer_to_print_idx=" << layer_to_print_idx
                                                         << " observed_min=" << base_height_stats.min
                                                         << " observed_max=" << base_height_stats.max
                                                         << " path_count=" << base_height_stats.count;
                            }
                            filtered_extrusions = clipped_base.get();
                            local_z_clipped_collections.emplace_back(std::move(clipped_base));
                        }

                        const unsigned int configured_filament_id = configured_filament_id_1based(entity_type, *filtered_extrusions, region);
                        const std::vector<unsigned int>* pointillism_sequence =
                            is_anything_overridden ? nullptr : pointillism_sequence_for_filament(configured_filament_id);
                        if (pointillism_sequence != nullptr) {
                            std::vector<std::unique_ptr<ExtrusionEntityCollection>> split_by_extruder;
                            PointillismPathSplitStats split_stats;
                            const size_t sequence_phase = pointillism_sequence->empty() ?
                                0 : size_t(std::max(0, layer_tools.layer_index)) % pointillism_sequence->size();
                            if (split_extrusion_collection_for_pointillism_paths(*filtered_extrusions,
                                                                                  *pointillism_sequence,
                                                                                  layer_tools.num_physical,
                                                                                  pointillism_segment_len_scaled,
                                                                                  pointillism_line_gap_scaled,
                                                                                  sequence_phase,
                                                                                  split_by_extruder,
                                                                                  split_stats) &&
                                split_stats.bucket_count >= 2) {
                                ++pointillism_path_split_entities;
                                pointillism_path_split_segments += split_stats.segment_count;
                                for (size_t extruder_idx = 0; extruder_idx < split_by_extruder.size(); ++extruder_idx) {
                                    std::unique_ptr<ExtrusionEntityCollection>& split_collection = split_by_extruder[extruder_idx];
                                    if (!split_collection || split_collection->entities.empty())
                                        continue;
                                    const ExtrusionEntityCollection* split_ptr = split_collection.get();
                                    local_z_clipped_collections.emplace_back(std::move(split_collection));
                                    std::vector<ObjectByExtruder::Island>& islands =
                                        object_islands_by_extruder(by_extruder, unsigned(extruder_idx), layer_to_print_idx, layers.size(), n_slices + 1);
                                    for (size_t i = 0; i <= n_slices; ++i) {
                                        const bool   last       = i == n_slices;
                                        const size_t island_idx = last ? n_slices : slices_test_order[i];
                                        if (last || entity_matches_surface(island_idx, *split_ptr)) {
                                            if (islands[island_idx].by_region.empty())
                                                islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                            islands[island_idx].by_region[region.print_region_id()].append(entity_type, split_ptr, nullptr);
                                            break;
                                        }
                                    }
                                }
                                continue;
                            }
                            ++pointillism_path_split_fallbacks;
                        }

                        // This extrusion is part of certain Region, which tells us which extruder should be used for it:
                        int correct_extruder_id = configured_extruder_id(entity_type, *filtered_extrusions, region);

                        // Phase 4 (image row on WALLS): an ImageWeighted row bound to this
                        // region's wall filament dithers the OUTER perimeter (and, when the
                        // region has more than one wall loop, the first inner one) into short
                        // per-filament runs - the same sampling, the same dither and the same
                        // per-run override phase 3 applies to a top surface's fill lines.
                        //
                        // This runs HERE, at G-code time, rather than in PerimeterGenerator,
                        // because a wall's seam is chosen at G-code time by SeamPlacer and the
                        // brief requires the seam to survive: local_z_loop_seam_placer (declared
                        // just above this loop) rotates the loop to its seam FIRST, and only the
                        // already-rotated geometry is then cut at the image's colour boundaries.
                        // A run boundary is therefore a colour boundary and nothing more - the
                        // nozzle still starts the wall exactly where SeamPlacer put it. Running
                        // here also means sampling AFTER fuzzy skin (applied inside
                        // PerimeterGenerator), so run boundaries land on the jittered geometry
                        // the nozzle really follows. See src/libslic3r/ImageRowWalls.hpp.
                        if (!is_anything_overridden &&
                            entity_type == ObjectByExtruder::Island::Region::PERIMETERS &&
                            layer_tools.extruder_override == 0) {
                            const ImageRowWallContext* wall_ctx = image_row_wall_ctx_for(region);
                            if (wall_ctx != nullptr) {
                                std::vector<std::unique_ptr<ExtrusionEntityCollection>> run_colls;
                                auto untouched = std::make_unique<ExtrusionEntityCollection>();
                                untouched->no_sort = filtered_extrusions->no_sort;
                                for (const ExtrusionEntity* e : filtered_extrusions->entities) {
                                    std::vector<std::unique_ptr<ExtrusionEntityCollection>> split;
                                    if (e != nullptr && image_row_wall_entity_is_claimed(*e, wall_ctx->split_first_inner)) {
                                        const ExtrusionEntity* to_split = e;
                                        ExtrusionLoop          seam_loop;
                                        if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(e)) {
                                            Point seam_anchor = loop->first_point();
                                            if (local_z_loop_seam_placer(*loop, seam_loop, seam_anchor))
                                                to_split = &seam_loop;
                                        }
                                        split = image_row_split_wall_entity(image_row_wall_assets, *wall_ctx, *to_split, print_z);
                                    }
                                    if (split.empty()) {
                                        if (e != nullptr)
                                            untouched->append(*e);
                                    } else {
                                        for (auto& c : split)
                                            run_colls.push_back(std::move(c));
                                    }
                                }
                                if (!run_colls.empty()) {
                                    // One island bucket per run, dispatched exactly the way the
                                    // outer/inner wall splitter just below dispatches its two
                                    // buckets, plus one for everything that stayed unsplit.
                                    auto dispatch = [&](const ExtrusionEntityCollection* coll, unsigned int extruder) {
                                        if (coll == nullptr || coll->entities.empty())
                                            return;
                                        if (!layer_tools.has_extruder(extruder))
                                            extruder = layer_tools.extruders.back();
                                        std::vector<ObjectByExtruder::Island>& islands =
                                            object_islands_by_extruder(by_extruder, extruder, layer_to_print_idx, layers.size(), n_slices + 1);
                                        for (size_t i = 0; i <= n_slices; ++i) {
                                            const bool   last       = i == n_slices;
                                            const size_t island_idx = last ? n_slices : slices_test_order[i];
                                            if (last || entity_matches_surface(island_idx, *coll)) {
                                                if (islands[island_idx].by_region.empty())
                                                    islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                                islands[island_idx].by_region[region.print_region_id()].append(entity_type, coll, nullptr);
                                                break;
                                            }
                                        }
                                    };
                                    for (auto& c : run_colls) {
                                        // image_row_extruder_1based is 1-BASED; the island
                                        // bucket key is 0-BASED - the canonical dispatch
                                        // further down this function keys it on
                                        // correct_extruder_id, i.e. on
                                        // LayerTools::wall_filament(), which returns a
                                        // zero-based extruder. Convert once, here.
                                        //
                                        // NOTE, found while writing this and deliberately
                                        // NOT 'fixed' here: layer_tools.extruders is
                                        // 1-based (ToolOrdering.cpp:938 and :941 register
                                        // solid_infill_filament()/sparse_infill_filament()
                                        // + 1), so every has_extruder() call on this path -
                                        // the canonical one included - tests a 0-based id
                                        // against a 1-based list and usually misses,
                                        // falling back to extruders.back(). That is a
                                        // pre-existing, file-wide off-by-one, not something
                                        // this feature introduces; matching the canonical
                                        // path's convention is what keeps image-row runs
                                        // behaving exactly like every other entity here,
                                        // and correcting it belongs in its own change with
                                        // its own Bar A.
                                        dispatch(c.get(), c->image_row_extruder_1based - 1);
                                        local_z_clipped_collections.emplace_back(std::move(c));
                                    }
                                    if (!untouched->entities.empty()) {
                                        // correct_extruder_id is already 0-based
                                        // (configured_extruder_id returns
                                        // layer_tools.wall_filament(region)).
                                        dispatch(untouched.get(), unsigned(std::max(0, correct_extruder_id)));
                                        local_z_clipped_collections.emplace_back(std::move(untouched));
                                    }
                                    continue;
                                }
                            }
                        }

                        // Ultra: outer walls may print with their own filament - split the island's
                        // perimeter collection into outer/inner sub-collections, one per extruder.
                        if (!is_anything_overridden &&
                            entity_type == ObjectByExtruder::Island::Region::PERIMETERS &&
                            layer_tools.outer_wall_filament(region) != layer_tools.wall_filament(region)) {
                            auto entity_is_outer = [](const ExtrusionEntity* e) -> bool {
                                auto paths_outer = [](const ExtrusionPaths& paths, int inset_idx) -> bool {
                                    if (inset_idx == 0)
                                        return true;
                                    if (inset_idx > 0)
                                        return false;
                                    for (const ExtrusionPath& p : paths)
                                        if (p.role() == erExternalPerimeter)
                                            return true;
                                    return false;
                                };
                                if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(e))
                                    return paths_outer(loop->paths, loop->inset_idx);
                                if (const auto* mp = dynamic_cast<const ExtrusionMultiPath*>(e))
                                    return paths_outer(mp->paths, mp->inset_idx);
                                return e->role() == erExternalPerimeter;
                            };
                            auto outer_coll = std::make_unique<ExtrusionEntityCollection>();
                            auto inner_coll = std::make_unique<ExtrusionEntityCollection>();
                            outer_coll->no_sort = filtered_extrusions->no_sort;
                            inner_coll->no_sort = filtered_extrusions->no_sort;
                            for (const ExtrusionEntity* e : filtered_extrusions->entities)
                                (entity_is_outer(e) ? outer_coll : inner_coll)->append(*e);
                            if (!outer_coll->entities.empty() && !inner_coll->entities.empty()) {
                                const std::pair<ExtrusionEntityCollection*, unsigned int> split_parts[] = {
                                    { inner_coll.get(), layer_tools.wall_filament(region) },
                                    { outer_coll.get(), layer_tools.outer_wall_filament(region) },
                                };
                                for (const auto& [split_ptr, split_extruder] : split_parts) {
                                    unsigned int extruder = split_extruder;
                                    if (!layer_tools.has_extruder(extruder))
                                        extruder = layer_tools.extruders.back();
                                    std::vector<ObjectByExtruder::Island>& islands =
                                        object_islands_by_extruder(by_extruder, extruder, layer_to_print_idx, layers.size(), n_slices + 1);
                                    for (size_t i = 0; i <= n_slices; ++i) {
                                        const bool   last       = i == n_slices;
                                        const size_t island_idx = last ? n_slices : slices_test_order[i];
                                        if (last || entity_matches_surface(island_idx, *split_ptr)) {
                                            if (islands[island_idx].by_region.empty())
                                                islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                            islands[island_idx].by_region[region.print_region_id()].append(entity_type, split_ptr, nullptr);
                                            break;
                                        }
                                    }
                                }
                                local_z_clipped_collections.emplace_back(std::move(outer_coll));
                                local_z_clipped_collections.emplace_back(std::move(inner_coll));
                                continue;
                            }
                            // Homogeneous island: keep the original collection, just pick the right extruder.
                            correct_extruder_id = int(inner_coll->entities.empty() ? layer_tools.outer_wall_filament(region)
                                                                                   : layer_tools.wall_filament(region));
                        }

                        if (!is_anything_overridden &&
                            entity_type == ObjectByExtruder::Island::Region::PERIMETERS &&
                            layer_tools.mixed_mgr != nullptr &&
                            layer_tools.num_physical > 0 &&
                            correct_extruder_id >= 0) {
                            const unsigned int mixed_filament_id =
                                grouped_manual_pattern_mixed_filament_id(entity_type, *filtered_extrusions, region);
                            if (mixed_filament_id != 0) {
                                std::vector<std::unique_ptr<ExtrusionEntityCollection>> split_by_extruder;
                                size_t bucket_count = 0;
                                const PrintObject* current_object_for_gradient =
                                    layer_to_print.original_object != nullptr ? layer_to_print.original_object : layer_to_print.object();
                                if (split_extrusion_collection_for_multi_perimeter_pattern(*filtered_extrusions,
                                                                                           *layer_tools.mixed_mgr,
                                                                                           mixed_filament_id,
                                                                                           layer_tools.num_physical,
                                                                                           layer_tools.layer_index,
                                                                                           split_by_extruder,
                                                                                           bucket_count,
                                                                                           current_object_for_gradient)) {
                                    if (bucket_count >= 2) {
                                        for (size_t extruder_idx = 0; extruder_idx < split_by_extruder.size(); ++extruder_idx) {
                                            std::unique_ptr<ExtrusionEntityCollection>& split_collection = split_by_extruder[extruder_idx];
                                            if (!split_collection || split_collection->entities.empty())
                                                continue;
                                            const ExtrusionEntityCollection* split_ptr = split_collection.get();
                                            local_z_clipped_collections.emplace_back(std::move(split_collection));
                                            std::vector<ObjectByExtruder::Island>& islands =
                                                object_islands_by_extruder(by_extruder, unsigned(extruder_idx), layer_to_print_idx, layers.size(), n_slices + 1);
                                            for (size_t i = 0; i <= n_slices; ++i) {
                                                const bool   last       = i == n_slices;
                                                const size_t island_idx = last ? n_slices : slices_test_order[i];
                                                if (last || entity_matches_surface(island_idx, *split_ptr)) {
                                                    if (islands[island_idx].by_region.empty())
                                                        islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                                    islands[island_idx].by_region[region.print_region_id()].append(entity_type, split_ptr, nullptr);
                                                    break;
                                                }
                                            }
                                        }
                                        continue;
                                    }
                                    if (bucket_count == 1) {
                                        for (size_t extruder_idx = 0; extruder_idx < split_by_extruder.size(); ++extruder_idx) {
                                            const std::unique_ptr<ExtrusionEntityCollection>& split_collection = split_by_extruder[extruder_idx];
                                            if (split_collection && !split_collection->entities.empty()) {
                                                // by_extruder keys and LayerTools runtime extruder IDs are zero-based here.
                                                correct_extruder_id = int(extruder_idx);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Let's recover vector of extruder overrides:
                        const WipingExtrusions::ExtruderPerCopy* entity_overrides = nullptr;
                        if (!layer_tools.has_extruder(correct_extruder_id)) {
                            // this entity is not overridden, but its extruder is not in layer_tools - we'll print it
                            // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare
                            // extruders are eradicated from layer_tools)
                            correct_extruder_id = layer_tools.extruders.back();
                        }
                        printing_extruders.clear();
                        if (is_anything_overridden) {
                            entity_overrides = const_cast<LayerTools&>(layer_tools)
                                                   .wiping_extrusions()
                                                   .get_extruder_overrides(filtered_extrusions, layer_to_print.original_object, correct_extruder_id,
                                                                           layer_to_print.object()->instances().size());
                            if (entity_overrides == nullptr) {
                                printing_extruders.emplace_back(correct_extruder_id);
                            } else {
                                printing_extruders.reserve(entity_overrides->size());
                                for (int extruder : *entity_overrides)
                                    printing_extruders.emplace_back(extruder >= 0 ?
                                                                        // at least one copy is overridden to use this extruder
                                                                        extruder :
                                                                        // at least one copy would normally be printed with this extruder
                                                                        // (see get_extruder_overrides function for explanation)
                                                                        static_cast<unsigned int>(-extruder - 1));
                                Slic3r::sort_remove_duplicates(printing_extruders);
                            }
                        } else
                            printing_extruders.emplace_back(correct_extruder_id);

                        // Now we must add this extrusion into the by_extruder map, once for each extruder that will print it:
                        for (unsigned int extruder : printing_extruders) {
                            std::vector<ObjectByExtruder::Island>& islands =
                                object_islands_by_extruder(by_extruder, extruder, layer_to_print_idx, layers.size(), n_slices + 1);
                            for (size_t i = 0; i <= n_slices; ++i) {
                                bool   last       = i == n_slices;
                                size_t island_idx = last ? n_slices : slices_test_order[i];
                                if ( // extrusions->first_point does not fit inside any slice
                                    last ||
                                    // extrusions->first_point fits inside ith slice
                                    entity_matches_surface(island_idx, *filtered_extrusions)) {
                                    if (islands[island_idx].by_region.empty())
                                        islands[island_idx].by_region.assign(print.num_print_regions(), ObjectByExtruder::Island::Region());
                                    islands[island_idx].by_region[region.print_region_id()].append(entity_type, filtered_extrusions,
                                                                                                   entity_overrides);
                                    break;
                                }
                            }
                        }
                    }
                }
            } // for regions
        }
    } // for objects

    if (local_z_perimeter_phase_b_enabled) {
        for (size_t layer_to_print_idx = 0; layer_to_print_idx < local_z_layer_contexts.size(); ++layer_to_print_idx) {
            const LocalZLayerContext& ctx = local_z_layer_contexts[layer_to_print_idx];
            if (!ctx.enabled)
                continue;
            BOOST_LOG_TRIVIAL(debug) << "Local-Z clipping summary"
                                     << " print_z=" << print_z
                                     << " layer_to_print_idx=" << layer_to_print_idx
                                     << " local_collections=" << ctx.local_clipped_collections
                                     << " base_collections=" << ctx.base_clipped_collections
                                     << " pass_count=" << ctx.pass_buckets.size();
        }
    }

    if (m_wipe_tower)
        m_wipe_tower->set_is_first_print(true);

    struct LocalZPassRef {
        size_t            layer_to_print_idx { 0 };
        LocalZPassBucket* bucket { nullptr };
    };

    auto same_local_z_pass_group = [](const LocalZPassRef& lhs, const LocalZPassRef& rhs) {
        assert(lhs.bucket != nullptr && rhs.bucket != nullptr);
        assert(lhs.bucket->plan != nullptr && rhs.bucket->plan != nullptr);
        // Buckets that land on the same Local-Z print plane are independent even if
        // they belong to different objects, so let phase-b order that whole plane
        // together instead of restarting the grouping per object/layer context.
        return std::abs(lhs.bucket->plan->print_z - rhs.bucket->plan->print_z) <= EPSILON;
    };

    auto local_z_bucket_extruders = [](const LocalZPassBucket& bucket) {
        std::vector<unsigned int> extruders;
        extruders.reserve(bucket.by_extruder.size());
        for (const auto& by_extruder_entry : bucket.by_extruder) {
            if (!by_extruder_entry.second.empty())
                extruders.push_back(by_extruder_entry.first);
        }
        return extruders;
    };

    auto shared_local_z_extruder = [](const std::vector<unsigned int>& lhs, const std::vector<unsigned int>& rhs) -> int {
        for (unsigned int extruder_id : lhs) {
            if (std::find(rhs.begin(), rhs.end(), extruder_id) != rhs.end())
                return static_cast<int>(extruder_id);
        }
        return -1;
    };

    std::vector<LocalZPassRef> local_z_pass_refs;
    if (local_z_perimeter_phase_b_enabled) {
        for (size_t layer_to_print_idx = 0; layer_to_print_idx < local_z_layer_contexts.size(); ++layer_to_print_idx) {
            LocalZLayerContext& ctx = local_z_layer_contexts[layer_to_print_idx];
            if (!ctx.enabled)
                continue;
            for (LocalZPassBucket& bucket : ctx.pass_buckets) {
                if (bucket.plan != nullptr && !bucket.by_extruder.empty())
                    local_z_pass_refs.push_back(LocalZPassRef{layer_to_print_idx, &bucket});
            }
        }
        std::sort(local_z_pass_refs.begin(), local_z_pass_refs.end(), [](const LocalZPassRef& lhs, const LocalZPassRef& rhs) {
            assert(lhs.bucket != nullptr && rhs.bucket != nullptr);
            assert(lhs.bucket->plan != nullptr && rhs.bucket->plan != nullptr);
            if (lhs.bucket->plan->print_z != rhs.bucket->plan->print_z)
                return lhs.bucket->plan->print_z < rhs.bucket->plan->print_z;
            if (lhs.layer_to_print_idx != rhs.layer_to_print_idx)
                return lhs.layer_to_print_idx < rhs.layer_to_print_idx;
            return lhs.bucket->plan->pass_index < rhs.bucket->plan->pass_index;
        });
    }

    if (local_z_perimeter_phase_b_enabled) {
        BOOST_LOG_TRIVIAL(info) << "Local-Z phase-b prepared"
                                << " print_z=" << print_z
                                << " path_passes=" << local_z_pass_refs.size();
        if (local_z_pass_refs.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Local-Z phase-b enabled but produced no path passes"
                                       << " print_z=" << print_z;
        }
    }

    std::vector<unsigned int> layer_extruders = layer_tools.extruders;
    for (const auto& by_extruder_entry : by_extruder) {
        if (std::find(layer_extruders.begin(), layer_extruders.end(), by_extruder_entry.first) == layer_extruders.end())
            layer_extruders.emplace_back(by_extruder_entry.first);
    }

    const double nominal_layer_z          = print_z + m_config.z_offset.value;
    int          nominal_layer_start_extruder = (m_writer.extruder() != nullptr) ? int(m_writer.extruder()->id()) : -1;

    if (!local_z_pass_refs.empty()) {
        int  local_z_phase_b_active_extruder = (m_writer.extruder() != nullptr) ? int(m_writer.extruder()->id()) : -1;
        BOOST_LOG_TRIVIAL(info) << "Local-Z phase-b emitting"
                                << " print_z=" << print_z
                                << " path_passes=" << local_z_pass_refs.size();
        gcode += "; local-z phase-b path passes begin\n";
        auto emit_local_z_toolchange = [&](unsigned int extruder_id, double toolchange_print_z) {
            if (has_wipe_tower && m_wipe_tower) {
                gcode += m_wipe_tower->tool_change(*this, int(extruder_id), false, true, print_z + m_config.z_offset.value);
                // Local-Z phase-b uses the wipe tower outside the normal per-layer
                // extruder loop, so mirror the usual toolchange bookkeeping here.
                // This forces the next object path to refresh WIDTH/HEIGHT tags
                // after prime tower G-code, keeping the preview in sync with the
                // actual local-Z pass height.
                m_last_processor_extrusion_role = erWipeTower;
            } else {
                gcode += this->set_extruder(extruder_id, toolchange_print_z);
            }
        };

        auto emit_local_z_pass_for_extruder = [&](const LocalZPassRef& pass_ref, unsigned int local_extruder_id) {
            assert(pass_ref.bucket != nullptr && pass_ref.bucket->plan != nullptr);
            auto by_extruder_it = pass_ref.bucket->by_extruder.find(local_extruder_id);
            if (by_extruder_it == pass_ref.bucket->by_extruder.end())
                return;

            std::vector<ObjectByExtruder>& objects_by_extruder = by_extruder_it->second;
            if (objects_by_extruder.empty())
                return;

            const SubLayerPlan& pass_plan = *pass_ref.bucket->plan;
            const double pass_z           = pass_plan.print_z + m_config.z_offset.value;
            const double saved_nominal_z  = m_nominal_z;
            const float  saved_last_layer_z = m_last_layer_z;
            // Ensure all travel/lift logic inside this pass references the micro-pass Z,
            // not the base layer nominal Z.
            m_nominal_z  = pass_z;
            m_last_layer_z = float(pass_z);
            BOOST_LOG_TRIVIAL(debug) << "Local-Z pass emit"
                                     << " print_z=" << print_z
                                     << " layer_to_print_idx=" << pass_ref.layer_to_print_idx
                                     << " layer_id=" << pass_plan.layer_id
                                     << " pass_index=" << pass_plan.pass_index
                                     << " dependency_group=" << pass_plan.dependency_group
                                     << " dependency_order=" << pass_plan.dependency_order
                                     << " pass_print_z=" << pass_plan.print_z
                                     << " pass_flow_height=" << pass_plan.flow_height
                                     << " extruder=" << local_extruder_id;
            if (std::abs(m_writer.get_position().z() - pass_z) > EPSILON) {
                gcode += this->retract(false, false, LiftType::NormalLift);
                gcode += m_writer.travel_to_z(pass_z, "Local-Z path pass");
            }
            if (std::abs(m_writer.get_position().z() - pass_z) > EPSILON) {
                BOOST_LOG_TRIVIAL(warning) << "Local-Z pass z restore"
                                           << " print_z=" << print_z
                                           << " layer_id=" << pass_plan.layer_id
                                           << " pass_index=" << pass_plan.pass_index
                                           << " dependency_group=" << pass_plan.dependency_group
                                           << " dependency_order=" << pass_plan.dependency_order
                                           << " extruder=" << local_extruder_id
                                           << " expected_pass_z=" << pass_z
                                           << " observed_z_after_toolchange=" << m_writer.get_position().z();
                gcode += m_writer.travel_to_z(pass_z, "Local-Z pass z restore");
            }
            std::vector<InstanceToPrint> instances_to_print =
                sort_print_object_instances(objects_by_extruder, layers, ordering, single_object_instance_idx);

            for (InstanceToPrint& instance_to_print : instances_to_print) {
                const LayerToPrint& layer_to_print = layers[instance_to_print.layer_id];
                const bool object_layer_over_raft =
                    layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                    instance_to_print.print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();

                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer                  = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                const bool saved_reduce_crossing_wall = m_config.reduce_crossing_wall.value;
                // Local-Z phase-b emits many short micro-passes. Avoid-crossing
                // travel planning is expensive and fragile on these fragments, so
                // keep travel simple here.
                m_config.reduce_crossing_wall.value = false;
                m_avoid_crossing_perimeters.disable_once();

                const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));

                for (ObjectByExtruder::Island& island : instance_to_print.object_by_extruder.islands) {
                    gcode += this->extrude_perimeters(print, island.by_region, first_layer, false);
                    gcode += this->extrude_infill(print, island.by_region, false);
                    gcode += this->extrude_perimeters(print, island.by_region, first_layer, true);
                    gcode += this->extrude_infill(print, island.by_region, true);
                }
                m_config.reduce_crossing_wall.value = saved_reduce_crossing_wall;
            }

            m_nominal_z   = saved_nominal_z;
            m_last_layer_z = saved_last_layer_z;
        };

        auto emit_local_z_legacy = [&](int active_extruder) {
            size_t pass_ref_idx = 0;
            while (pass_ref_idx < local_z_pass_refs.size()) {
                size_t pass_group_end = pass_ref_idx + 1;
                while (pass_group_end < local_z_pass_refs.size() &&
                       same_local_z_pass_group(local_z_pass_refs[pass_ref_idx], local_z_pass_refs[pass_group_end])) {
                    ++pass_group_end;
                }

                std::vector<unsigned int> pass_group_extruders;
                struct LocalZPassTaskRef {
                    const LocalZPassRef* pass_ref { nullptr };
                    unsigned int         extruder_id { 0 };
                };
                std::vector<LocalZPassTaskRef> pass_group_tasks;
                for (size_t group_idx = pass_ref_idx; group_idx < pass_group_end; ++group_idx) {
                    const LocalZPassRef& pass_ref = local_z_pass_refs[group_idx];
                    for (unsigned int extruder_id : local_z_bucket_extruders(*pass_ref.bucket)) {
                        pass_group_tasks.push_back(LocalZPassTaskRef{ &pass_ref, extruder_id });
                        if (std::find(pass_group_extruders.begin(), pass_group_extruders.end(), extruder_id) == pass_group_extruders.end())
                            pass_group_extruders.push_back(extruder_id);
                    }
                }

                std::vector<unsigned int> next_group_extruders;
                if (pass_group_end < local_z_pass_refs.size()) {
                    size_t next_group_end = pass_group_end + 1;
                    while (next_group_end < local_z_pass_refs.size() &&
                           same_local_z_pass_group(local_z_pass_refs[pass_group_end], local_z_pass_refs[next_group_end])) {
                        ++next_group_end;
                    }
                    for (size_t group_idx = pass_group_end; group_idx < next_group_end; ++group_idx) {
                        for (unsigned int extruder_id : local_z_bucket_extruders(*local_z_pass_refs[group_idx].bucket))
                            if (std::find(next_group_extruders.begin(), next_group_extruders.end(), extruder_id) == next_group_extruders.end())
                                next_group_extruders.push_back(extruder_id);
                    }
                }

                const int preferred_last_extruder = shared_local_z_extruder(pass_group_extruders, next_group_extruders);
                const std::vector<unsigned int> ordered_group_extruders =
                    LocalZOrderOptimizer::order_bucket_extruders(pass_group_extruders,
                                                                 active_extruder,
                                                                 preferred_last_extruder);

                for (unsigned int local_extruder_id : ordered_group_extruders) {
                    emit_local_z_toolchange(local_extruder_id, local_z_pass_refs[pass_ref_idx].bucket->plan->print_z);
                    for (const LocalZPassTaskRef& task_ref : pass_group_tasks) {
                        if (task_ref.extruder_id != local_extruder_id || task_ref.pass_ref == nullptr)
                            continue;
                        emit_local_z_pass_for_extruder(*task_ref.pass_ref, local_extruder_id);
                    }
                    active_extruder = int(local_extruder_id);
                }

                pass_ref_idx = pass_group_end;
            }

            return active_extruder;
        };

        const bool dependency_chain_mode =
            std::all_of(local_z_pass_refs.begin(), local_z_pass_refs.end(), [](const LocalZPassRef& pass_ref) {
                return pass_ref.bucket != nullptr &&
                       pass_ref.bucket->plan != nullptr &&
                       pass_ref.bucket->plan->dependency_group != 0;
            });

        if (dependency_chain_mode) {
            struct ChainKey {
                size_t layer_to_print_idx { 0 };
                size_t dependency_group { 0 };

                bool operator<(const ChainKey& rhs) const
                {
                    if (layer_to_print_idx != rhs.layer_to_print_idx)
                        return layer_to_print_idx < rhs.layer_to_print_idx;
                    return dependency_group < rhs.dependency_group;
                }
            };
            struct PassState {
                const LocalZPassRef*    pass_ref { nullptr };
                std::vector<unsigned int> remaining_extruders;
                size_t                    chain_idx { 0 };
                size_t                    chain_pos { 0 };
                bool                      ready { false };
                bool                      completed { false };
            };

            std::map<ChainKey, size_t> chain_index_by_key;
            std::vector<std::vector<size_t>> chains;
            std::vector<PassState>           pass_states;
            pass_states.reserve(local_z_pass_refs.size());
            for (const LocalZPassRef& pass_ref : local_z_pass_refs) {
                ChainKey chain_key { pass_ref.layer_to_print_idx, pass_ref.bucket->plan->dependency_group };
                auto [it_chain, inserted] = chain_index_by_key.emplace(chain_key, chains.size());
                if (inserted)
                    chains.emplace_back();

                const size_t chain_idx = it_chain->second;
                const size_t pass_state_idx = pass_states.size();
                pass_states.push_back(PassState{ &pass_ref, local_z_bucket_extruders(*pass_ref.bucket), chain_idx, 0, false, false });
                chains[chain_idx].push_back(pass_state_idx);
            }

            for (std::vector<size_t>& chain : chains) {
                std::sort(chain.begin(), chain.end(), [&pass_states](size_t lhs_idx, size_t rhs_idx) {
                    const SubLayerPlan& lhs = *pass_states[lhs_idx].pass_ref->bucket->plan;
                    const SubLayerPlan& rhs = *pass_states[rhs_idx].pass_ref->bucket->plan;
                    if (lhs.dependency_order != rhs.dependency_order)
                        return lhs.dependency_order < rhs.dependency_order;
                    if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                        return lhs.print_z < rhs.print_z;
                    return lhs.pass_index < rhs.pass_index;
                });
                for (size_t chain_pos = 0; chain_pos < chain.size(); ++chain_pos)
                    pass_states[chain[chain_pos]].chain_pos = chain_pos;
                if (!chain.empty())
                    pass_states[chain.front()].ready = true;
            }

            auto pass_contains_extruder = [](const PassState& pass_state, unsigned int extruder_id) {
                return std::find(pass_state.remaining_extruders.begin(), pass_state.remaining_extruders.end(), extruder_id) !=
                       pass_state.remaining_extruders.end();
            };
            auto choose_ready_extruder = [&](int active_extruder) -> int {
                std::vector<unsigned int> ready_extruders;
                for (const PassState& pass_state : pass_states) {
                    if (!pass_state.ready || pass_state.completed)
                        continue;
                    for (unsigned int extruder_id : pass_state.remaining_extruders)
                        if (std::find(ready_extruders.begin(), ready_extruders.end(), extruder_id) == ready_extruders.end())
                            ready_extruders.push_back(extruder_id);
                }
                if (ready_extruders.empty())
                    return -1;
                if (active_extruder >= 0 &&
                    std::find(ready_extruders.begin(), ready_extruders.end(), unsigned(active_extruder)) != ready_extruders.end()) {
                    return active_extruder;
                }

                int    best_extruder = -1;
                size_t best_ready_count = 0;
                size_t best_future_count = 0;
                for (unsigned int extruder_id : ready_extruders) {
                    size_t ready_count = 0;
                    size_t future_count = 0;
                    for (const PassState& pass_state : pass_states) {
                        if (pass_state.completed || !pass_contains_extruder(pass_state, extruder_id))
                            continue;
                        ++future_count;
                        if (pass_state.ready)
                            ++ready_count;
                    }

                    if (best_extruder < 0 ||
                        ready_count > best_ready_count ||
                        (ready_count == best_ready_count && future_count > best_future_count) ||
                        (ready_count == best_ready_count && future_count == best_future_count && extruder_id < unsigned(best_extruder))) {
                        best_extruder = int(extruder_id);
                        best_ready_count = ready_count;
                        best_future_count = future_count;
                    }
                }

                return best_extruder;
            };

            struct ScheduledPhase {
                unsigned int        extruder_id { 0 };
                std::vector<size_t> pass_state_indices;
            };

            bool                        dependency_scheduler_ok = true;
            size_t                      completed_passes = 0;
            int                         simulated_active_extruder = local_z_phase_b_active_extruder;
            std::vector<ScheduledPhase> scheduled_phases;
            while (dependency_scheduler_ok && completed_passes < pass_states.size()) {
                const int chosen_extruder = choose_ready_extruder(simulated_active_extruder);
                if (chosen_extruder < 0) {
                    dependency_scheduler_ok = false;
                    BOOST_LOG_TRIVIAL(warning) << "Local-Z dependency scheduler deadlocked, falling back"
                                               << " print_z=" << print_z
                                               << " pass_count=" << local_z_pass_refs.size();
                    break;
                }

                std::vector<size_t> ready_pass_indices;
                for (size_t pass_state_idx = 0; pass_state_idx < pass_states.size(); ++pass_state_idx) {
                    const PassState& pass_state = pass_states[pass_state_idx];
                    if (!pass_state.ready || pass_state.completed || !pass_contains_extruder(pass_state, unsigned(chosen_extruder)))
                        continue;
                    ready_pass_indices.push_back(pass_state_idx);
                }
                if (ready_pass_indices.empty()) {
                    dependency_scheduler_ok = false;
                    BOOST_LOG_TRIVIAL(warning) << "Local-Z dependency scheduler made no progress, falling back"
                                               << " print_z=" << print_z
                                               << " chosen_extruder=" << chosen_extruder
                                               << " pass_count=" << local_z_pass_refs.size();
                    break;
                }

                std::sort(ready_pass_indices.begin(), ready_pass_indices.end(), [&pass_states](size_t lhs_idx, size_t rhs_idx) {
                    const LocalZPassRef& lhs_ref = *pass_states[lhs_idx].pass_ref;
                    const LocalZPassRef& rhs_ref = *pass_states[rhs_idx].pass_ref;
                    const SubLayerPlan& lhs = *lhs_ref.bucket->plan;
                    const SubLayerPlan& rhs = *rhs_ref.bucket->plan;
                    if (std::abs(lhs.print_z - rhs.print_z) > EPSILON)
                        return lhs.print_z < rhs.print_z;
                    if (lhs_ref.layer_to_print_idx != rhs_ref.layer_to_print_idx)
                        return lhs_ref.layer_to_print_idx < rhs_ref.layer_to_print_idx;
                    return lhs.pass_index < rhs.pass_index;
                });
                scheduled_phases.push_back(ScheduledPhase{ unsigned(chosen_extruder), ready_pass_indices });

                std::vector<size_t> newly_completed;
                for (size_t pass_state_idx : ready_pass_indices) {
                    PassState& pass_state = pass_states[pass_state_idx];
                    auto it_extruder = std::find(pass_state.remaining_extruders.begin(),
                                                 pass_state.remaining_extruders.end(),
                                                 unsigned(chosen_extruder));
                    if (it_extruder == pass_state.remaining_extruders.end())
                        continue;
                    pass_state.remaining_extruders.erase(it_extruder);
                    if (pass_state.remaining_extruders.empty())
                        newly_completed.push_back(pass_state_idx);
                }

                for (size_t pass_state_idx : newly_completed) {
                    PassState& pass_state = pass_states[pass_state_idx];
                    if (pass_state.completed)
                        continue;
                    pass_state.ready = false;
                    pass_state.completed = true;
                    ++completed_passes;

                    const std::vector<size_t>& chain = chains[pass_state.chain_idx];
                    const size_t next_chain_pos = pass_state.chain_pos + 1;
                    if (next_chain_pos < chain.size())
                        pass_states[chain[next_chain_pos]].ready = true;
                }

                simulated_active_extruder = chosen_extruder;
            }

            if (!dependency_scheduler_ok) {
                local_z_phase_b_active_extruder = emit_local_z_legacy(local_z_phase_b_active_extruder);
            } else {
                for (const ScheduledPhase& scheduled_phase : scheduled_phases) {
                    emit_local_z_toolchange(scheduled_phase.extruder_id,
                                            pass_states[scheduled_phase.pass_state_indices.front()].pass_ref->bucket->plan->print_z);
                    for (size_t pass_state_idx : scheduled_phase.pass_state_indices)
                        emit_local_z_pass_for_extruder(*pass_states[pass_state_idx].pass_ref, scheduled_phase.extruder_id);
                }
                local_z_phase_b_active_extruder = simulated_active_extruder;
                BOOST_LOG_TRIVIAL(info) << "Local-Z dependency scheduler"
                                        << " print_z=" << print_z
                                        << " pass_count=" << local_z_pass_refs.size()
                                        << " chain_count=" << chains.size();
            }
        } else {
            local_z_phase_b_active_extruder = emit_local_z_legacy(local_z_phase_b_active_extruder);
        }

        nominal_layer_start_extruder = local_z_phase_b_active_extruder;

        if (std::abs(m_writer.get_position().z() - nominal_layer_z) > EPSILON) {
            gcode += this->retract(false, false, LiftType::NormalLift);
            gcode += m_writer.travel_to_z(nominal_layer_z, "Local-Z return to nominal layer");
        }
        gcode += "; local-z phase-b path passes end\n";
    }

    if (nominal_layer_start_extruder >= 0) {
        auto it = std::find(layer_extruders.begin(), layer_extruders.end(), unsigned(nominal_layer_start_extruder));
        if (it != layer_extruders.end())
            std::rotate(layer_extruders.begin(), it, layer_extruders.end());
    }
    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    for (unsigned int extruder_id : layer_extruders) {
        if (print.config().skirt_type == stCombined && !print.skirt().empty())
            gcode += generate_skirt(print, print.skirt(), Point(0, 0), layer.object()->config().skirt_start_angle, layer_tools, layer,
                                    extruder_id);

        std::string gcode_toolchange;
        if (has_wipe_tower) {
            if (!m_wipe_tower->is_empty_wipe_tower_gcode(*this, extruder_id, extruder_id == layer_extruders.back())) {
                if (need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode &&
                    (!bbl_dual_nozzle_timelapse || timelapse_on_photo_head())) {
                    gcode += this->retract(false, false, LiftType::NormalLift);
                    m_writer.add_object_change_labels(gcode);

                    std::string timepals_gcode = insert_timelapse_gcode();
                    if (!timepals_gcode.empty()) {
                        gcode += timepals_gcode;
                        m_writer.set_current_position_clear(false);
                        // BBS: check whether custom gcode changes the z position. Update if changed
                        double temp_z_after_timepals_gcode;
                        if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                            Vec3d pos = m_writer.get_position();
                            pos(2)    = temp_z_after_timepals_gcode;
                            m_writer.set_position(pos);
                        }
                    }
                    has_insert_timelapse_gcode = true;
                }
                gcode_toolchange = m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_extruders.back());
            }
        } else {
            if (bbl_dual_nozzle_timelapse && need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode &&
                m_writer.need_toolchange(extruder_id) && timelapse_on_photo_head()) {
                gcode += this->retract(false, false, LiftType::NormalLift);
                m_writer.add_object_change_labels(gcode);
                std::string timepals_gcode = insert_timelapse_gcode();
                if (!timepals_gcode.empty()) {
                    gcode += timepals_gcode;
                    m_writer.set_current_position_clear(false);
                    double temp_z_after_timepals_gcode;
                    if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                        Vec3d pos = m_writer.get_position();
                        pos(2)    = temp_z_after_timepals_gcode;
                        m_writer.set_position(pos);
                    }
                }
                has_insert_timelapse_gcode = true;
            }
            gcode_toolchange = this->set_extruder(extruder_id, print_z);
        }
        if (!gcode_toolchange.empty()) {
            // Disable vase mode for layers that has toolchange
            result.spiral_vase_enable = false;
        }
        gcode += std::move(gcode_toolchange);

        // let analyzer tag generator aware of a role type change
        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = erWipeTower;

        // Chameleon brim: print this extruder's foreign brim partitions (layer 0 only).
        // MUST run after the toolchange gcode above: this extruder isn't actually
        // active (loaded/primed) until gcode_toolchange has been appended, so emitting
        // here - rather than before the toolchange block - avoids extruding a foreign
        // partition while the previous extruder is still selected.
        // Iterate print.objects() (stable, model-derived order) rather than the
        // ObjectID-keyed map directly: ObjectID is an allocation-order id, not stable
        // across runs (see Print.cpp's object_ordinal comment), so iterating the map
        // by its own key order would make emission order - and thus travel moves/seams -
        // run-unstable even though the underlying geometry ordering is deterministic.
        if (first_layer && !print.get_brimMapByExtruder().empty()) {
            const auto& brim_map_by_extruder = print.get_brimMapByExtruder();
            for (const PrintObject* obj : print.objects()) {
                auto obj_entry = brim_map_by_extruder.find(obj->id());
                if (obj_entry == brim_map_by_extruder.end())
                    continue;
                auto it = obj_entry->second.find(extruder_id);
                if (it == obj_entry->second.end() || it->second.entities.empty())
                    continue;
                this->set_origin(0., 0.);
                m_avoid_crossing_perimeters.use_external_mp();
                for (const ExtrusionEntity* ee : it->second.entities)
                    gcode += this->extrude_entity(*ee, "brim", this->process_flow_value(m_config.support_speed));
                m_avoid_crossing_perimeters.use_external_mp(false);
                m_avoid_crossing_perimeters.disable_once();
            }
        }

        // Chameleon P2.1: print this extruder's matched support-interface AND matched
        // base partitions - interface_by_extruder (T1 storage) now holds BOTH roles (v2.1
        // Task 3: Print.cpp's chameleon_assign_support_interfaces calls
        // partition_support_entities once per role, sharing this one map/extruder-key
        // space). MUST run after the toolchange gcode above (same reasoning as the brim
        // block just above: this extruder isn't actually active/primed until
        // gcode_toolchange has been appended, so emitting before it would extrude under
        // the wrong tool).
        // The Print.cpp partition pass (Task 2/3) REMOVES matched entities from
        // support_fills - interface_by_extruder is the only place they still live, so
        // this block is not optional: skipping it silently loses that geometry.
        // Dedicated block, not routed through the (object, extruder) ObjectByExtruder
        // support buckets built below (~5416-5423 in the support-collection pass): those
        // buckets carry one scalar support_extrusion_role per (object, extruder) and would
        // collide with per-partition emission here (documented Part 1 bucket-collision
        // risk). Iterate `layers` in its existing run-stable order - mirrors the brim
        // block's use of print.objects() order rather than any allocation-order map key.
        for (const LayerToPrint& ltp : layers) {
            if (ltp.support_layer == nullptr)
                continue;
            const SupportLayer& sl = *ltp.support_layer;
            auto                 it = sl.interface_by_extruder.find(extruder_id);
            if (it == sl.interface_by_extruder.end() || it->second.entities.empty())
                continue;
            const PrintObject& obj  = *ltp.original_object;
            // I1 fix: apply this object's config before extruding its matched interface
            // partitions, mirroring the normal per-instance path (~6487:
            // m_config.apply(instance_to_print.print_object.config(), true) before support
            // extrusion). Without this, support_interface_speed/accel etc. resolve from
            // whatever object's config m_config last saw (a previous object on this layer,
            // or a previous layer's object), not this partition's own object.
            m_config.apply(obj.config(), true);
            m_layer                  = ltp.support_layer;
            m_object_layer_over_raft = false;
            for (const PrintInstance& instance : obj.instances()) {
                this->set_origin(unscale(instance.shift));
                // Mirror the normal per-instance loop's gcode_label_objects comment idiom
                // (~6542-6545 start / ~6697-6700 end) so comment-parsing cancel-object
                // tools (OctoPrint style) can attribute these partition extrusions too.
                if (this->config().gcode_label_objects) {
                    gcode += std::string("; printing object ") + obj.model_object()->name +
                             " id:" + std::to_string(obj.get_id()) + " copy " + std::to_string(instance.id) + "\n";
                }
                // I2 fix: wrap this instance's partition extrusions in the same
                // exclude-object (M624/M625, EXCLUDE_OBJECT_*, M486) label machinery the
                // normal per-instance loop uses (~6493-6513 start / ~6648-6667 end), so
                // firmware skip-object suppresses matched interface for an excluded
                // instance instead of extruding it into the void left by its skipped
                // supports. Part 1's brim block shares this gap but is layer-0-only
                // (out of scope here); this block runs on every layer.
                if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_start_str(std::string("; start printing object, unique label id: ") +
                                                      std::to_string(instance.model_instance->get_labeled_id()) + "\n" + "M624 " +
                                                      _encode_label_ids_to_base64({instance.model_instance->get_labeled_id()}) + "\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_start_str(std::string("EXCLUDE_OBJECT_START NAME=") +
                                                          get_instance_name(&obj, instance) + "\n");
                        } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            m_writer.set_object_start_str(std::string("M486 S") + std::to_string(instance.unique_id) + "\n");
                        }
                    }
                }

                // v2.1 Task 3: pass erMixed, not erSupportMaterialInterface - it->second
                // can now hold BOTH erSupportMaterial and erSupportMaterialInterface paths
                // (base entities the lateral rule matched to this extruder share this same
                // map bucket as matched interface entities). Passing
                // erSupportMaterialInterface here would silently DROP every base path in
                // it->second: extrude_support's own selection filter (~7375,
                // `role == support_extrusion_role`) would reject each erSupportMaterial
                // path and it would just never print. erMixed is this codebase's existing
                // precedent for "let each path's own role drive everything" (~5419:
                // `obj.support_extrusion_role = single_extruder ? erMixed :
                // erSupportMaterial` for the ordinary interleaved-support case) - under
                // erMixed, ~7375's filter becomes a pass-through
                // (`support_extrusion_role == erMixed && role != erIroning`), and every
                // downstream decision already keys off each PATH's own role(), not this
                // argument: the gcode comment label (~7389-7393, switches on `ee->role()`),
                // the speed resolution in _extrude (~7659-7662,
                // `path.role() == erSupportMaterial ? support_speed :
                // support_interface_speed`), the interface cooling-fan marker (~7962,
                // `path.role() == erSupportMaterialInterface`), and the support-island
                // retraction-skip check (~8554, keyed off the role travel_to receives,
                // which is `path.role()` per ~7528). So per-path flow/speed/labeling is
                // already correct with erMixed and no further change is needed here.
                gcode += this->extrude_support(it->second, erMixed);

                // v2.2 Task 3 (spec C6): make sure ironing is last, mirroring the
                // direct/non-chameleon support path's own erMixed+erIroning pair
                // (~6654-6657, "Make sure ironing is the last"). Verified by reading
                // GCode::extrude_support (~7388-7441, comment above cites its old line
                // numbers as ~7375/~7389-7393 - current numbers are ~7401/~7415-7419):
                // its erMixed branch is `(role == support_extrusion_role) ||
                // (support_extrusion_role == erMixed && role != erIroning)` - the
                // `role != erIroning` term means erMixed NEVER matches an erIroning
                // entity (leaf OR a whole nested collection, since role() is the same
                // virtual call either way - v2.2 Task 3's C7 change), so the call just
                // above unconditionally excludes this bucket's matched ironing (v2.2
                // Task 3's third partition_support_entities call, role_filter =
                // erIroning) no matter what else it->second holds. This second call with
                // support_extrusion_role = erIroning is what actually emits it - only
                // entities whose own role() == erIroning pass the first half of that
                // same OR (`role == support_extrusion_role`). When this bucket carries
                // no erIroning entity at all, `extrusions` (~7397-7404) ends up empty
                // and the function returns "" immediately (~7405-7406) before touching
                // m_last_pos/chain_and_reorder_extrusion_entities - a true no-op, not a
                // crash or a duplicate emission of the erMixed call's own paths.
                gcode += this->extrude_support(it->second, erIroning);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; stop printing object ") + obj.model_object()->name +
                             " id:" + std::to_string(obj.get_id()) + " copy " + std::to_string(instance.id) + "\n";
                }
                // Don't set m_gcode_label_objects_end if the start string never got
                // consumed (nothing was extruded for this instance's partition).
                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_end_str(std::string("; stop printing object, unique label id: ") +
                                                    std::to_string(instance.model_instance->get_labeled_id()) + "\n" + "M625\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_end_str(std::string("EXCLUDE_OBJECT_END NAME=") +
                                                        get_instance_name(&obj, instance) + "\n");
                        } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            m_writer.set_object_end_str(std::string("M486 S-1\n"));
                        }
                    }
                }
            }
        }

        auto objects_by_extruder_it = by_extruder.find(extruder_id);
        if (objects_by_extruder_it == by_extruder.end())
            continue;

        // BBS: ordering instances by extruder
        std::vector<InstanceToPrint> instances_to_print;
        bool                         has_prime_tower = print.config().enable_prime_tower && print.extruders().size() > 1 &&
                               ((print.config().print_sequence == PrintSequence::ByLayer &&
                                 print.config().print_order == PrintOrder::Default) ||
                                (print.config().print_sequence == PrintSequence::ByObject && print.objects().size() == 1));
        if (has_prime_tower) {
            int   plate_idx = print.get_plate_index();
            Point wt_pos(print.config().wipe_tower_x.get_at(plate_idx), print.config().wipe_tower_y.get_at(plate_idx));

            std::vector<GCode::ObjectByExtruder>& objects_by_extruder = objects_by_extruder_it->second;
            std::vector<const PrintObject*>       print_objects;
            for (int obj_idx = 0; obj_idx < objects_by_extruder.size(); obj_idx++) {
                auto& object_by_extruder = objects_by_extruder[obj_idx];
                if (object_by_extruder.islands.empty() && (object_by_extruder.support == nullptr || object_by_extruder.support->empty()))
                    continue;

                print_objects.push_back(print.get_object(obj_idx));
            }

            std::vector<const PrintInstance*> new_ordering = chain_print_object_instances(print_objects, &wt_pos);
            std::reverse(new_ordering.begin(), new_ordering.end());
            instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, layers, &new_ordering,
                                                             single_object_instance_idx);
        } else {
            instances_to_print = sort_print_object_instances(objects_by_extruder_it->second, layers, ordering, single_object_instance_idx);
        }

        // BBS
        if (print.config().skirt_type == stPerObject && print.config().print_sequence == PrintSequence::ByObject &&
            !layer.object()->object_skirt().empty() &&
            ((layer.id() < print.config().skirt_height || print.config().draft_shield == DraftShield::dsEnabled))) {
            for (InstanceToPrint& instance_to_print : instances_to_print) {
                if (instance_to_print.print_object.object_skirt().empty())
                    continue;

                if (this->m_objSupportsWithBrim.find(instance_to_print.print_object.id()) != this->m_objSupportsWithBrim.end() &&
                    print.m_supportBrimMap.at(instance_to_print.print_object.id()).entities.size() > 0)
                    continue;

                if (this->m_objsWithBrim.find(instance_to_print.print_object.id()) != this->m_objsWithBrim.end() &&
                    print.m_brimMap.at(instance_to_print.print_object.id()).entities.size() > 0)
                    continue;
                if (first_layer)
                    m_skirt_done.clear();

                if (layer.id() == 1 && m_skirt_done.size() > 1)
                    m_skirt_done.erase(m_skirt_done.begin() + 1, m_skirt_done.end());

                const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                gcode += generate_skirt(print, instance_to_print.print_object.object_skirt(), offset,
                                        instance_to_print.print_object.config().skirt_start_angle, layer_tools, layer, extruder_id);
            }
        }

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first
        // (infill/perimeter wiping feature):
        std::vector<ObjectByExtruder::Island::Region> by_region_per_copy_cache;
        for (int print_wipe_extrusions = is_anything_overridden; print_wipe_extrusions >= 0; --print_wipe_extrusions) {
            if (is_anything_overridden && print_wipe_extrusions == 0)
                gcode += "; PURGING FINISHED\n";

            for (InstanceToPrint& instance_to_print : instances_to_print) {
                if (print.config().skirt_type == stPerObject && !instance_to_print.print_object.object_skirt().empty() &&
                    print.config().print_sequence == PrintSequence::ByLayer &&
                    (layer.id() < print.config().skirt_height || print.config().draft_shield == DraftShield::dsEnabled)) {
                    if (first_layer)
                        m_skirt_done.clear();
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    gcode += generate_skirt(print, instance_to_print.print_object.object_skirt(), offset,
                                            instance_to_print.print_object.config().skirt_start_angle, layer_tools, layer, extruder_id);
                    if (instances_to_print.size() > 1 && &instance_to_print != &*(instances_to_print.end() - 1))
                        m_skirt_done.pop_back();
                }

                const auto&         inst           = instance_to_print.print_object.instances()[instance_to_print.instance_id];
                const LayerToPrint& layer_to_print = layers[instance_to_print.layer_id];
                // To control print speed of the 1st object layer printed over raft interface.
                bool object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
                                              instance_to_print.print_object.slicing_parameters().raft_layers() ==
                                                  layer_to_print.object_layer->id();
                m_config.apply(instance_to_print.print_object.config(), true);
                m_layer                  = layer_to_print.layer();
                m_object_layer_over_raft = object_layer_over_raft;
                if (m_config.reduce_crossing_wall)
                    m_avoid_crossing_perimeters.init_layer(*m_layer);

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; printing object ") + instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " + std::to_string(inst.id) + "\n";
                }
                // exclude objects
                if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_start_str(std::string("; start printing object, unique label id: ") +
                                                      std::to_string(instance_to_print.label_object_id) + "\n" + "M624 " +
                                                      _encode_label_ids_to_base64({instance_to_print.label_object_id}) + "\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_start_str(std::string("EXCLUDE_OBJECT_START NAME=") +
                                                          get_instance_name(&instance_to_print.print_object, inst.id) + "\n");
                        } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            std::string str = std::string("M486 S") + std::to_string(inst.unique_id) + "\n";
                            m_writer.set_object_start_str(str);
                        }
                    }
                }

                // Orca(#7946): set current obj regardless of the `enable_overhang_speed` value, because
                // `enable_overhang_speed` is a PrintRegionConfig and here we don't have a region yet.
                // And no side effect doing this even if `enable_overhang_speed` is off, so don't bother
                // checking anything here.
                m_extrusion_quality_estimator.set_current_object(&instance_to_print.print_object);

                // When starting a new object, use the external motion planner for the first travel move.
                const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                if (m_last_obj_copy != this_object_copy)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                m_last_obj_copy = this_object_copy;
                this->set_origin(unscale(offset));
                if (instance_to_print.object_by_extruder.support != nullptr) {
                    m_layer                  = layers[instance_to_print.layer_id].support_layer;
                    m_object_layer_over_raft = false;

                    // BBS: print supports' brims first
                    if (this->m_objSupportsWithBrim.find(instance_to_print.print_object.id()) != this->m_objSupportsWithBrim.end() &&
                        !print_wipe_extrusions) {
                        this->set_origin(0., 0.);
                        m_avoid_crossing_perimeters.use_external_mp();
                        for (const ExtrusionEntity* ee : print.m_supportBrimMap.at(instance_to_print.print_object.id()).entities) {
                            gcode += this->extrude_entity(*ee, "brim", this->process_flow_value(m_config.support_speed));
                        }
                        m_avoid_crossing_perimeters.use_external_mp(false);
                        // Allow a straight travel move to the first object point.
                        m_avoid_crossing_perimeters.disable_once();
                        this->m_objSupportsWithBrim.erase(instance_to_print.print_object.id());
                    }
                    // When starting a new object, use the external motion planner for the first travel move.
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    ExtrusionEntityCollection support_eec;

                    // BBS
                    WipingExtrusions& wiping_extrusions  = const_cast<LayerTools&>(layer_tools).wiping_extrusions();
                    bool              support_overridden = wiping_extrusions.is_support_overridden(layer_to_print.original_object);
                    bool support_intf_overridden = wiping_extrusions.is_support_interface_overridden(layer_to_print.original_object);

                    ExtrusionRole support_extrusion_role = instance_to_print.object_by_extruder.support_extrusion_role;
                    bool          is_overridden          = support_extrusion_role == erSupportMaterialInterface ? support_intf_overridden :
                                                                                                                  support_overridden;
                    if (is_overridden == (print_wipe_extrusions != 0)) {
                        gcode += this->extrude_support(
                            // support_extrusion_role is erSupportMaterial, erSupportTransition, erSupportMaterialInterface or erMixed for
                            // all extrusion paths.
                            *instance_to_print.object_by_extruder.support, support_extrusion_role);

                        // Make sure ironing is the last
                        if (support_extrusion_role == erMixed || support_extrusion_role == erSupportMaterialInterface) {
                            gcode += this->extrude_support(*instance_to_print.object_by_extruder.support, erIroning);
                        }
                    }

                    m_layer                  = layer_to_print.layer();
                    m_object_layer_over_raft = object_layer_over_raft;
                }
                // FIXME order islands?
                //  Sequential tool path ordering of multiple parts within the same object, aka. perimeter tracking (#5511)
                for (ObjectByExtruder::Island& island : instance_to_print.object_by_extruder.islands) {
                    const auto& by_region_specific = is_anything_overridden ?
                                                         island.by_region_per_copy(by_region_per_copy_cache,
                                                                                   static_cast<unsigned int>(instance_to_print.instance_id),
                                                                                   extruder_id, print_wipe_extrusions != 0) :
                                                         island.by_region;
                    // BBS: add brim by obj by extruder
                    if (first_layer) {
                        if (this->m_objsWithBrim.find(instance_to_print.print_object.id()) != this->m_objsWithBrim.end() &&
                            !print_wipe_extrusions) {
                            this->set_origin(0., 0.);
                            m_avoid_crossing_perimeters.use_external_mp();
                            for (const ExtrusionEntity* ee : print.m_brimMap.at(instance_to_print.print_object.id()).entities) {
                                gcode += this->extrude_entity(*ee, "brim", this->process_flow_value(m_config.support_speed));
                            }
                            m_avoid_crossing_perimeters.use_external_mp(false);
                            // Allow a straight travel move to the first object point.
                            m_avoid_crossing_perimeters.disable_once();
                            this->m_objsWithBrim.erase(instance_to_print.print_object.id());
                        }
                    }
                    // When starting a new object, use the external motion planner for the first travel move.
                    const Point& offset = instance_to_print.print_object.instances()[instance_to_print.instance_id].shift;
                    std::pair<const PrintObject*, Point> this_object_copy(&instance_to_print.print_object, offset);
                    if (m_last_obj_copy != this_object_copy)
                        m_avoid_crossing_perimeters.use_external_mp_once();
                    m_last_obj_copy = this_object_copy;
                    this->set_origin(unscale(offset));
                    // FIXME the following code prints regions in the order they are defined, the path is not optimized in any way.

                    auto has_infill = [](const std::vector<ObjectByExtruder::Island::Region>& by_region) {
                        for (auto region : by_region) {
                            if (!region.infills.empty())
                                return true;
                        }
                        return false;
                    };
                    {
                        // Print perimeters of regions that has is_infill_first == false
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false);
                        if (!has_wipe_tower && need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode &&
                            printer_structure == PrinterStructure::psI3 && has_infill(by_region_specific)) {
                            gcode += this->retract(false, false, LiftType::NormalLift);

                            std::string timepals_gcode = insert_timelapse_gcode();
                            if (!timepals_gcode.empty()) {
                                gcode += timepals_gcode;
                                m_writer.set_current_position_clear(false);
                                // BBS: check whether custom gcode changes the z position. Update if changed
                                double temp_z_after_timepals_gcode;
                                if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                                    Vec3d pos = m_writer.get_position();
                                    pos(2)    = temp_z_after_timepals_gcode;
                                    m_writer.set_position(pos);
                                }
                            }
                            has_insert_timelapse_gcode = true;
                        }
                        // Then print infill
                        gcode += this->extrude_infill(print, by_region_specific, false);
                        // Then the walls left hanging in mid air, now that the infill can anchor them
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, false, true);
                        // Then print perimeters of regions that has is_infill_first == true
                        gcode += this->extrude_perimeters(print, by_region_specific, first_layer, true);
                    }
                    // ironing
                    gcode += this->extrude_infill(print, by_region_specific, true);
                }

                if (this->config().gcode_label_objects) {
                    gcode += std::string("; stop printing object ") + instance_to_print.print_object.model_object()->name +
                             " id:" + std::to_string(instance_to_print.print_object.get_id()) + " copy " + std::to_string(inst.id) + "\n";
                }
                // exclude objects
                // Don't set m_gcode_label_objects_end if you don't had to write the m_gcode_label_objects_start.
                if (!m_writer.is_object_start_str_empty()) {
                    m_writer.set_object_start_str("");
                } else if (m_enable_exclude_object) {
                    if (is_BBL_Printer()) {
                        m_writer.set_object_end_str(std::string("; stop printing object, unique label id: ") +
                                                    std::to_string(instance_to_print.label_object_id) + "\n" + "M625\n");
                    } else {
                        const auto gflavor = print.config().gcode_flavor.value;
                        if (gflavor == gcfKlipper) {
                            m_writer.set_object_end_str(std::string("EXCLUDE_OBJECT_END NAME=") +
                                                        get_instance_name(&instance_to_print.print_object, inst.id) + "\n");
                        } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                            m_writer.set_object_end_str(std::string("M486 S-1\n"));
                        }
                    }
                }
            }
        }
    }
    if (first_layer) {
        for (auto iter = by_extruder.begin(); iter != by_extruder.end(); ++iter) {
            if (!iter->second.empty())
                m_initial_layer_extruders.insert(iter->first);
        }
    }

#if 0
    // Apply spiral vase post-processing if this layer contains suitable geometry
    // (we must feed all the G-code into the post-processor, including the first
    // bottom non-spiral layers otherwise it will mess with positions)
    // we apply spiral vase at this stage because it requires a full layer.
    // Just a reminder: A spiral vase mode is allowed for a single object per layer, single material print only.
    if (m_spiral_vase)
        gcode = m_spiral_vase->process_layer(std::move(gcode));

    // Apply cooling logic; this may alter speeds.
    if (m_cooling_buffer)
        gcode = m_cooling_buffer->process_layer(std::move(gcode), layer.id(),
            // Flush the cooling buffer at each object layer or possibly at the last layer, even if it contains just supports (This should not happen).
            object_layer || last_layer);

    file.write(gcode);
#endif

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z << log_memory_info();

    if ((!has_wipe_tower || bbl_dual_nozzle_timelapse) && need_insert_timelapse_gcode_for_traditional && !has_insert_timelapse_gcode) {
        if (bbl_dual_nozzle_timelapse) {
            // BambuStudio: a traditional photo taken on the other head, with no tower to hide the
            // head swap, can mark the surface.
            if (m_support_traditional_timelapse && m_config.timelapse_type.value == TimelapseType::tlTraditional &&
                !has_wipe_tower && !timelapse_on_photo_head())
                m_support_traditional_timelapse = false;
        } else if (m_support_traditional_timelapse)
            m_support_traditional_timelapse = false;

        gcode += this->retract(false, false, LiftType::NormalLift);
        m_writer.add_object_change_labels(gcode);

        std::string timepals_gcode = insert_timelapse_gcode();
        if (!timepals_gcode.empty()) {
            gcode += timepals_gcode;
            m_writer.set_current_position_clear(false);
            // BBS: check whether custom gcode changes the z position. Update if changed
            double temp_z_after_timepals_gcode;
            if (GCodeProcessor::get_last_z_from_gcode(timepals_gcode, temp_z_after_timepals_gcode)) {
                Vec3d pos = m_writer.get_position();
                pos(2)    = temp_z_after_timepals_gcode;
                m_writer.set_position(pos);
            }
        }
    }

    if (pointillism_path_split_entities > 0) {
        BOOST_LOG_TRIVIAL(warning) << "Same-layer pointillisme path-domain split"
                                   << " layer_id=" << layer.id()
                                   << " print_z=" << print_z
                                   << " entities=" << pointillism_path_split_entities
                                   << " segments=" << pointillism_path_split_segments
                                   << " segment_len_mm=" << pointillism_segment_len_mm
                                   << " line_gap_mm=" << pointillism_line_gap_mm
                                   << " split_fallbacks=" << pointillism_path_split_fallbacks;
    }

    result.gcode                = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}

void GCode::apply_print_config(const PrintConfig& print_config)
{
    m_writer.apply_print_config(print_config);
    m_config.apply(print_config);
    m_scaled_resolution     = scaled<double>(print_config.resolution.value);
    m_enable_exclude_object = m_config.exclude_object;

#if ORCA_CHECK_GCODE_PLACEHOLDERS
    // If the gcode value is empty, set a value so that the check code within the parser is run
    for (auto opt : std::initializer_list<ConfigOptionString*>{
             &m_config.machine_start_gcode,
             &m_config.machine_end_gcode,
             &m_config.before_layer_change_gcode,
             &m_config.layer_change_gcode,
             &m_config.time_lapse_gcode,
             &m_config.change_filament_gcode,
             &m_config.change_extrusion_role_gcode,
             &m_config.printing_by_object_gcode,
             &m_config.machine_pause_gcode,
             &m_config.template_custom_gcode,
         }) {
        if (opt->empty())
            opt->set(new ConfigOptionString(";VALUE FOR TESTING"));
    }
    for (auto opt : std::initializer_list<ConfigOptionStrings*>{&m_config.filament_start_gcode, &m_config.filament_end_gcode}) {
        if (opt->empty())
            for (int i = 0; i < opt->size(); ++i)
                opt->set_at(new ConfigOptionString(";VALUE FOR TESTING"), i, 0);
    }
#endif
}

// Ultra (dual-nozzle header): the "; filament_nozzle_map" / "; filament_volume_map" CONFIG_BLOCK lines
// for a plate the filament->nozzle grouping ran on, taken from that grouping result - the other half of
// BambuStudio's Print::update_filament_maps_to_config (Print.cpp:2840, static-map branch called from
// ToolOrdering.cpp:2804): the logical nozzle id per filament (= slice_info <filament group_id>) and the
// volume type of the nozzle each USED filament landed on (unused filaments keep their config value).
// ToolOrdering already writes filament_map; without this the other two lines printed the stale project
// values (e.g. filament_nozzle_map 1,0,0,... while filament_map put every filament on the right rack).
//
// Derived here, at header time, rather than written into the Print's configs from ToolOrdering: the fork's
// Print::apply has no BambuStudio-style mask for these keys and the GUI does not write them back to the
// project (it does for filament_map), so a config write would make every later apply() see a diff -
// a full re-slice, or an export-only re-run that re-applied the stale values to the header. Nothing in
// G-code generation reads either key, so the header is the only consumer to fix. Covers every grouping
// mode (auto, match, manual): they all store their result on the Print the same way.
// Gated like the grouping itself (ToolOrdering::reorder_extruders_for_minimum_flush_volume): a machine
// with distinct extruder variants and no sequential print, so a result left on the Print by an earlier
// slice of another printer or plate arrangement is never used.
static bool grouping_header_maps(const Print& print, std::vector<int>& nozzle_map, std::vector<int>& volume_map)
{
    auto group_result = print.get_layered_nozzle_group_result();
    if (!group_result || group_result->is_support_dynamic_nozzle_map())
        return false;
    int        extruder_count = 0;
    const bool is_sequential  = print.config().print_sequence == PrintSequence::ByObject && print.objects().size() > 1;
    // A Bambu print-by-object plate is grouped afresh at the top of GCode::_do_export, so its result
    // describes this slice too.
    if ((is_sequential && !print.is_BBL_printer()) ||
        !const_cast<DynamicPrintConfig&>(print.full_print_config()).support_different_extruders(extruder_count))
        return false;

    const DynamicPrintConfig& cfg = print.full_print_config();
    const auto*               fm  = cfg.option<ConfigOptionInts>("filament_map");
    const size_t              num_filaments = fm ? fm->values.size() : 0;
    nozzle_map = group_result->get_nozzle_map();
    if (nozzle_map.empty() || num_filaments == 0)
        return false;

    const auto* vm_opt = cfg.option<ConfigOptionInts>("filament_volume_map");
    volume_map         = vm_opt ? vm_opt->values : std::vector<int>();
    volume_map.resize(num_filaments, int(NozzleVolumeType::nvtStandard));
    const std::vector<int> grouped_volumes = group_result->get_volume_map();
    for (unsigned int f : group_result->get_used_filaments())
        if (f < volume_map.size() && f < grouped_volumes.size() && grouped_volumes[f] >= 0 &&
            grouped_volumes[f] <= int(NozzleVolumeType::nvtMaxNozzleVolumeType))
            volume_map[f] = grouped_volumes[f];
    return true;
}

void GCode::append_full_config(const Print& print, std::string& str)
{
    const DynamicPrintConfig& cfg = print.full_print_config();
    std::vector<int> grouped_nozzle_map, grouped_volume_map;
    const bool       use_grouped_maps = grouping_header_maps(print, grouped_nozzle_map, grouped_volume_map);
    // Sorted list of config keys, which shall not be stored into the G-code. Initializer list.
    static const std::set<std::string_view> banned_keys({"compatible_printers"sv, "compatible_prints"sv, "print_host"sv,
                                                         "print_host_webui"sv, "printhost_apikey"sv, "printhost_cafile"sv,
                                                         "printhost_user"sv, "printhost_password"sv, "printhost_port"sv});
    auto                                    is_banned = [](const std::string& key) { return banned_keys.find(key) != banned_keys.end(); };
    // BBS: the idle-nozzle pre-heating keys are dumped (as Bambu Studio does) only for a printer that
    // pre-heats; every other printer's config block stays as it was before the keys existed.
    static const std::set<std::string_view> pre_heating_keys({"enable_pre_heating"sv, "filament_pre_cooling_temperature"sv,
                                                              "filament_preheat_temperature_delta"sv});
    const bool dump_pre_heating_keys = print.config().enable_pre_heating.value;
    std::ostringstream                      ss;
    for (const std::string& key : cfg.keys()) {
        if (!dump_pre_heating_keys && pre_heating_keys.find(key) != pre_heating_keys.end())
            continue;
        if (!is_banned(key) && !cfg.option(key)->is_nil()) {
            if (key == "wipe_tower_x" || key == "wipe_tower_y") {
                ss << std::fixed << std::setprecision(3) << "; " << key << " = "
                   << dynamic_cast<const ConfigOptionFloats*>(cfg.option(key))->get_at(print.get_plate_index()) << "\n";
            }
            if (key == "extruder_colour")
                ss << "; " << key << " = " << cfg.opt_serialize("filament_colour") << "\n";
            else if (use_grouped_maps && key == "filament_nozzle_map")
                ss << "; " << key << " = " << ConfigOptionInts(grouped_nozzle_map).serialize() << "\n";
            else if (use_grouped_maps && key == "filament_volume_map")
                ss << "; " << key << " = " << ConfigOptionInts(grouped_volume_map).serialize() << "\n";
            else
                ss << "; " << key << " = " << cfg.opt_serialize(key) << "\n";
        }
    }
    str += ss.str();
}

void GCode::set_extruders(const std::vector<unsigned int>& extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    // enable wipe path generation if any extruder has wipe enabled
    m_wipe.enable = false;
    for (auto id : extruder_ids)
        if (m_config.wipe.get_at(id)) {
            m_wipe.enable = true;
            break;
        }
}

void GCode::set_origin(const Vec2d& pointf)
{
    // if origin increases (goes towards right), last_pos decreases because it goes towards left
    const Point translate(scale_(m_origin(0) - pointf(0)), scale_(m_origin(1) - pointf(1)));
    m_last_pos += translate;
    m_wipe.path.translate(translate);
    m_origin = pointf;
}

std::string GCode::preamble()
{
    std::string gcode = m_writer.preamble();

    /*  Perform a *silent* move to z_offset: we need this to initialize the Z
        position of our writer object so that any initial lift taking place
        before the first layer change will raise the extruder from the correct
        initial Z instead of 0.  */
    m_writer.travel_to_z(m_config.z_offset.value);

    return gcode;
}

// called by GCode::process_layer()
std::string GCode::change_layer(coordf_t print_z)
{
    std::string gcode;
    if (m_layer_count > 0)
        // Increment a progress bar indicator.
        gcode += m_writer.update_progress(++m_layer_index, m_layer_count);
    // BBS
    coordf_t z = print_z + m_config.z_offset.value; // in unscaled coordinates
    if (EXTRUDER_CONFIG(retract_when_changing_layer) && m_writer.will_move_z(z)) {
        LiftType lift_type = this->to_lift_type(ZHopType(EXTRUDER_CONFIG(z_hop_types)));
        // BBS: force to use SpiralLift when change layer if lift type is auto
        gcode += this->retract(false, false, ZHopType(EXTRUDER_CONFIG(z_hop_types)) == ZHopType::zhtAuto ? LiftType::SpiralLift : lift_type);
    }

    m_writer.add_object_change_labels(gcode);

    if (m_spiral_vase) {
        // BBS: force to normal lift immediately in spiral vase mode
        std::ostringstream comment;
        comment << "move to next layer (" << m_layer_index << ")";
        gcode += m_writer.travel_to_z(z, comment.str());
    }

    m_need_change_layer_lift_z = true;

    m_nominal_z                 = z;
    m_writer.get_position().z() = z;

    // forget last wiping path as wiping after raising Z is pointless
    // BBS. Dont forget wiping path to reduce stringing.
    // m_wipe.reset_path();

    return gcode;
}

static std::unique_ptr<EdgeGrid::Grid> calculate_layer_edge_grid(const Layer& layer)
{
    auto out = make_unique<EdgeGrid::Grid>();

    // Create the distance field for a layer below.
    const coord_t distance_field_resolution = coord_t(scale_(1.) + 0.5);
    out->create(layer.lslices, distance_field_resolution);
    out->calculate_sdf();
#if 0
        {
            static int iRun = 0;
            BoundingBox bbox = (*lower_layer_edge_grid)->bbox();
            bbox.min(0) -= scale_(5.f);
            bbox.min(1) -= scale_(5.f);
            bbox.max(0) += scale_(5.f);
            bbox.max(1) += scale_(5.f);
            EdgeGrid::save_png(*(*lower_layer_edge_grid), bbox, scale_(0.1f), debug_out_path("GCode_extrude_loop_edge_grid-%d.png", iRun++));
        }
#endif
    return out;
}

std::string GCode::extrude_loop(
    ExtrusionLoop loop, std::string description, double speed, const ExtrusionEntitiesPtr& region_perimeters, const Point* start_point,
    const WipeInwardSupport* wipe_support)
{
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation

    bool is_hole = (loop.loop_role() & elrHole) == elrHole;

    if (m_config.spiral_mode && !is_hole) {
        // if spiral vase, we have to ensure that all contour are in the same orientation.
        loop.make_counter_clockwise();
    }
    // if (loop.loop_role() == elrSkirt && (this->m_layer->id() % 2 == 1))
    //     loop.reverse();

    // find the point of the loop that is closest to the current extruder position
    // or randomize if requested;
    // or, if `start_point` is specified, start the loop at point closest to it
    Point last_pos      = start_point ? *start_point : this->last_pos();
    float seam_overhang = std::numeric_limits<float>::lowest();
    if (!m_config.spiral_mode && description == "perimeter") {
        assert(m_layer != nullptr);
        m_seam_placer.place_seam(m_layer, loop, last_pos, seam_overhang);
    } else
        loop.split_at(last_pos, false);

    const auto seam_scarf_type   = m_config.seam_slope_type.value;
    bool       enable_seam_slope = ((seam_scarf_type == SeamScarfType::External && !is_hole) || seam_scarf_type == SeamScarfType::All) &&
                             !m_config.spiral_mode &&
                             (loop.role() == erExternalPerimeter || (loop.role() == erPerimeter && m_config.seam_slope_inner_walls)) &&
                             layer_id() > 0;
    const auto nozzle_diameter = EXTRUDER_CONFIG(nozzle_diameter);
    if (enable_seam_slope && m_config.seam_slope_conditional.value) {
        enable_seam_slope = loop.is_smooth(m_config.scarf_angle_threshold.value * M_PI / 180., nozzle_diameter);
    }

    if (enable_seam_slope && m_config.seam_slope_conditional.value && m_config.scarf_overhang_threshold.value > 0.0f) {
        const auto _line_width = loop.role() == erExternalPerimeter ? m_config.outer_wall_line_width.get_abs_value(nozzle_diameter) :
                                                                      m_config.inner_wall_line_width.get_abs_value(nozzle_diameter);
        enable_seam_slope      = seam_overhang < m_config.scarf_overhang_threshold.value * 0.01f * _line_width;
    }

    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    const double seam_gap    = scale_(m_config.seam_gap.get_abs_value(nozzle_diameter));
    const double clip_length = m_enable_loop_clipping && !enable_seam_slope ? seam_gap : 0;

    // get paths
    ExtrusionPaths paths;
    loop.clip_end(clip_length, &paths);
    if (paths.empty())
        return "";

    // SoftFever: check loop lenght for small perimeter.
    double small_peri_speed = -1;
    if (speed == -1 && loop.length() <= SMALL_PERIMETER_LENGTH(this->process_flow_value(m_config.small_perimeter_threshold))) {
        if (this->process_flow_value(m_config.small_perimeter_speed).value == 0)
            small_peri_speed = this->process_flow_value(m_config.outer_wall_speed) * 0.5;
        else {
            const auto   sp_fop  = this->process_flow_value(m_config.small_perimeter_speed);
            const double sp_base = this->process_flow_value(m_config.outer_wall_speed);
            small_peri_speed = sp_fop.percent ? (sp_fop.value * 0.01 * sp_base) : sp_fop.value;
        }
    }

    // extrude along the path
    std::string gcode;

    // Orca:
    // Port of "wipe inside before extruding an external perimeter" feature from super slicer
    // If region perimeters size not greater than or equal to 2, then skip the wipe inside move as we will extrude in mid air
    // as no neighbouring perimeter exists. If an internal perimeter exists, we should find 2 perimeters touching the de-retraction point
    // 1 - the currently printed external perimeter and 2 - the neighbouring internal perimeter.
    if (m_config.wipe_before_external_loop.value && !paths.empty() && paths.front().size() > 1 && paths.back().size() > 1 &&
        paths.front().role() == erExternalPerimeter && region_perimeters.size() > 1) {
        const bool   is_full_loop_ccw = loop.polygon().is_counter_clockwise();
        bool         is_hole_loop     = (loop.loop_role() & ExtrusionLoopRole::elrHole) != 0; // loop.make_counter_clockwise();
        const double nozzle_diam      = nozzle_diameter;

        // note: previous & next are inverted to extrude "in the opposite direction, and we are "rewinding"
        Point previous_point = paths.front().polyline.points[1];
        Point current_point  = paths.front().polyline.points.front();
        Point next_point     = paths.back().polyline.points.back();

        // can happen if seam_gap is null
        if (next_point == current_point) {
            next_point = paths.back().polyline.points[paths.back().polyline.points.size() - 2];
        }

        Point a = next_point;     // second point
        Point b = previous_point; // second to last point
        if ((is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw)) {
            // swap points
            std::swap(a, b);
        }

        double angle = current_point.ccw_angle(a, b) / 3;

        // turn outwards if contour, turn inwwards if hole
        if (is_hole_loop ? !is_full_loop_ccw : is_full_loop_ccw)
            angle *= -1;

        Vec2d  current_pos = current_point.cast<double>();
        Vec2d  next_pos    = next_point.cast<double>();
        Vec2d  vec_dist    = next_pos - current_pos;
        double vec_norm    = vec_dist.norm();
        // Offset distance is the minimum between half the nozzle diameter or half the line width for the upcomming perimeter
        // This is to mimimize potential instances where the de-retraction is performed on top of a neighbouring
        // thin perimeter due to arachne reducing line width.
        coordf_t dist = std::min(scaled(nozzle_diam) * 0.5, scaled(paths.front().width) * 0.5);

        // FIXME Hiding the seams will not work nicely for very densely discretized contours!
        Point pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);

        // Search region perimeters for lines that are touching the de-retraction point.
        // If an internal perimeter exists, we should find 2 perimeters touching the de-retraction point
        // 1: the currently printed external perimeter and 2: the neighbouring internal perimeter.
        int discoveredTouchingLines = 0;
        for (const ExtrusionEntity* ee : region_perimeters) {
            auto                                potential_touching_line = ee->as_polyline();
            AABBTreeLines::LinesDistancer<Line> potential_touching_line_distancer{potential_touching_line.lines()};
            auto touching_line = potential_touching_line_distancer.all_lines_in_radius(pt, scale_(nozzle_diam));
            if (touching_line.size()) {
                discoveredTouchingLines++;
                if (discoveredTouchingLines > 1)
                    break; // found 2 touching lines. End the search early.
            }
        }
        // found 2 perimeters touching the de-retraction point. Its safe to deretract as the point will be
        // inside the model
        if (discoveredTouchingLines > 1) {
            // use extrude instead of travel_to_xy to trigger the unretract
            ExtrusionPath fake_path_wipe(Polyline{pt, current_point}, paths.front());
            fake_path_wipe.set_force_no_extrusion(true);
            fake_path_wipe.mm3_per_mm = 0;
            // fake_path_wipe.set_extrusion_role(erExternalPerimeter);
            gcode += extrude_path(fake_path_wipe, "move inwards before retraction/seam", speed);
        }
    }

    const auto speed_for_path = [&speed, &small_peri_speed](const ExtrusionPath& path) {
        // don't apply small perimeter setting for overhangs/bridges/non-perimeters
        const bool is_small_peri = is_perimeter(path.role()) && !is_bridge(path.role()) && small_peri_speed > 0;
        return is_small_peri ? small_peri_speed : speed;
    };

    // Orca: Adaptive PA: calculate average mm3_per_mm value over the length of the loop.
    // This is used for adaptive PA
    m_multi_flow_segment_path_pa_set             = false; // always emit PA on the first path of the loop
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm               = 0.0;
    double total_multipath_length                = 0.0;
    for (const ExtrusionPath& path : paths) {
        if (!path.is_force_no_extrusion()) {
            double path_length = unscale<double>(path.length()); // path length in mm
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;
    // Orca: end of multipath average mm3_per_mm value calculation

    if (!enable_seam_slope) {
        for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
            gcode += this->_extrude(*path, description, speed_for_path(*path));
            // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
            // as we have already set the PA value to the average flow over the totality of the path
            // in the first extrude move
            // TODO: testing is needed with slope seams and adaptive PA.
            m_multi_flow_segment_path_pa_set = true;
        }
    } else {
        // Create seam slope
        double start_slope_ratio;
        if (m_config.seam_slope_start_height.percent) {
            start_slope_ratio = m_config.seam_slope_start_height.value / 100.;
        } else {
            // Get the ratio against current layer height
            double h          = paths.front().height;
            start_slope_ratio = m_config.seam_slope_start_height.value / h;
        }
        if (start_slope_ratio >= 1)
            start_slope_ratio = 0.99;

        double loop_length = 0.;
        for (const auto& path : paths) {
            loop_length += unscale_(path.length());
        }

        const bool   slope_entire_loop = m_config.seam_slope_entire_loop;
        const double slope_min_length  = slope_entire_loop ? loop_length : std::min(m_config.seam_slope_min_length.value, loop_length);
        const int    slope_steps       = m_config.seam_slope_steps;
        const double slope_max_segment_length = scale_(slope_min_length / slope_steps);

        // Calculate the sloped loop
        ExtrusionLoopSloped new_loop(paths, seam_gap, slope_min_length, slope_max_segment_length, start_slope_ratio, loop.loop_role());
        new_loop.clip_slope(seam_gap);

        // Then extrude it
        for (const auto& p : new_loop.get_all_paths()) {
            gcode += this->_extrude(*p, description, speed_for_path(*p));
            // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
            // as we have already set the PA value to the average flow over the totality of the path
            // in the first extrude move
            m_multi_flow_segment_path_pa_set = true;
        }

        // Fix path for wipe
        if (!new_loop.ends.empty()) {
            paths.clear();
            // The start slope part is ignored as it overlaps with the end part
            paths.reserve(new_loop.paths.size() + new_loop.ends.size());
            paths.insert(paths.end(), new_loop.paths.begin(), new_loop.paths.end());
            paths.insert(paths.end(), new_loop.ends.begin(), new_loop.ends.end());
        }
    }

    // Orca: share the post-extrusion nozzle position between wipe_inward and wipe_on_loops.
    const bool is_ccw = loop.is_counter_clockwise();

    std::optional<Point> wipe_on_loops_dest;
    if (m_config.wipe_on_loops.value && paths.back().role() == erExternalPerimeter &&
        m_layer != nullptr && m_config.wall_loops.value > 1 && paths.front().size() >= 2 &&
        paths.back().polyline.points.size() >= 2)
        wipe_on_loops_dest = wipe_on_loops_destination(paths, scale_(nozzle_diameter), is_ccw, is_hole);

    bool wipe_inward_applied = false;
    // Orca: store loop paths in print order because inward offsets use this orientation.
    if (m_wipe.enable) {
        m_wipe.update_path(paths);

        // Orca: loop wipe paths retain print direction. Their material side is
        // therefore left for CCW contours and right for CW contours, with the
        // result inverted for holes. Only external perimeters are eligible.
        // Calibration overrides are applied during extrusion, after the region
        // context was created. Check the effective setting again at execution.
        if (m_config.wipe_inward && m_config.wipe_inward_distance.value > 0. &&
            wipe_support != nullptr && !wipe_support->inner_lines.empty() &&
            // A loop's role is its first path's role. An overhanging start must
            // not hide ordinary external-wall segments elsewhere in the loop.
            std::any_of(paths.begin(), paths.end(),
                [](const ExtrusionPath &path) { return is_external_perimeter(path.role()); }) &&
            m_wipe.path.points.size() >= 2) {
            // Orca: use the actual extrusion width from the path, not the config
            // value — outer_wall_line_width=0 (Auto) would make get_abs_value
            // return 0 and silently disable the feature, and Arachne may produce
            // a different width than the config default.
            const double outer_wall_line_width = paths.front().width;
            const double requested_offset = m_config.wipe_inward_distance.get_abs_value(outer_wall_line_width);
            const double offset_dist = scale_(std::min(requested_offset, outer_wall_line_width));
            if (offset_dist > SCALED_EPSILON) {
                const Point seam_start = paths.front().first_point();
                const Point seam_end   = paths.back().last_point();
                const Point wipe_start = wipe_on_loops_dest.value_or(seam_end);
                const double max_wipe_length = scale_(EXTRUDER_CONFIG(wipe_distance));
                // Orca: Wipe::wipe() replaces points[0] with last_pos and executes
                // from points[1]. The helper preserves that sentinel and atomically
                // replaces the remaining points, or leaves the path untouched.
                // Orca: a configured wall count does not guarantee that Arachne
                // generated an adjacent wall for this particular loop. Only
                // earlier entities are considered because later walls have
                // not been printed yet (for example with Outer/Inner order).
                // Inner walls determine the material side; every earlier wall
                // remains available to validate the executable wipe path.
                const double support_distance = scale_(std::max(nozzle_diameter, outer_wall_line_width));
                Polyline inward_path = m_wipe.path;
                if (offset_wipe_path_toward_support(
                        inward_path, seam_start, seam_end, wipe_start,
                        wipe_offset_direction(is_ccw, is_hole), offset_dist, max_wipe_length,
                        wipe_support->inner_lines, wipe_support->printed_lines,
                        m_wipe.path.lines(), support_distance)) {
                    m_wipe.path = std::move(inward_path);
                    wipe_inward_applied = true;
                }
            }
        }
    }

    // Orca: make the configured inward move before leaving the loop.
    if (wipe_on_loops_dest) {
        gcode += m_writer.extrude_to_xy(
            this->point_to_gcode(*wipe_on_loops_dest), 0, "move inwards before travel", true);
        this->set_last_pos(*wipe_on_loops_dest);
    }

    // Execute the accepted path before another extrusion replaces it. Wiping
    // must not force retraction or Z-hop across a short travel to the next wall.
    // Ordinary travel planning decides whether to retract from the new position.
    if (wipe_inward_applied)
        gcode += m_wipe.wipe(*this, 0.);

    return gcode;
}

std::string GCode::extrude_multi_path(ExtrusionMultiPath multipath, std::string description, double speed)
{
    // extrude along the path
    std::string gcode;

    // Orca: calculate multipath average mm3_per_mm value over the length of the path.
    // This is used for adaptive PA
    m_multi_flow_segment_path_pa_set             = false; // always emit PA on the first path of the multi-path
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    double weighted_sum_mm3_per_mm               = 0.0;
    double total_multipath_length                = 0.0;
    for (const ExtrusionPath& path : multipath.paths) {
        if (!path.is_force_no_extrusion()) {
            double path_length = unscale<double>(path.length()); // path length in mm
            weighted_sum_mm3_per_mm += path.mm3_per_mm * path_length;
            total_multipath_length += path_length;
        }
    }
    if (total_multipath_length > 0.0)
        m_multi_flow_segment_path_average_mm3_per_mm = weighted_sum_mm3_per_mm / total_multipath_length;
    // Orca: end of multipath average mm3_per_mm value calculation

    for (ExtrusionPath path : multipath.paths) {
        gcode += this->_extrude(path, description, speed);
        // Orca: Adaptive PA - dont adapt PA after the first pultipath extrusion is completed
        // as we have already set the PA value to the average flow over the totality of the path
        // in the first extrude move.
        m_multi_flow_segment_path_pa_set = true;
    }

    // BBS
    if (m_wipe.enable) {
        m_wipe.update_path(multipath.paths, true);
    }

    return gcode;
}

std::string GCode::extrude_entity(const ExtrusionEntity&      entity,
                                  std::string                 description,
                                  double                      speed,
                                  const ExtrusionEntitiesPtr& region_perimeters,
                                  const WipeInwardSupport*    wipe_support)
{
    if (const ExtrusionPath* path = dynamic_cast<const ExtrusionPath*>(&entity))
        return this->extrude_path(*path, description, speed);
    else if (const ExtrusionMultiPath* multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity))
        return this->extrude_multi_path(*multipath, description, speed);
    else if (const ExtrusionLoop* loop = dynamic_cast<const ExtrusionLoop*>(&entity))
        return this->extrude_loop(*loop, description, speed, region_perimeters, nullptr, wipe_support);
    else
        throw Slic3r::InvalidArgument("Invalid argument supplied to extrude()");
    return "";
}

std::string GCode::extrude_path(ExtrusionPath path, std::string description, double speed)
{
    // Orca: Reset average multipath flow as this is a single line, single extrude volumetric speed path
    m_multi_flow_segment_path_pa_set             = false;
    m_multi_flow_segment_path_average_mm3_per_mm = 0;
    //    description += ExtrusionEntity::role_to_string(path.role());
    std::string gcode = this->_extrude(path, description, speed);
    if (m_wipe.enable) {
        m_wipe.reset_path();
        m_wipe.path = std::move(path.polyline);
        m_wipe.path.reverse();
    }

    return gcode;
}

// Extrude perimeters: Decide where to put seams (hide or align seams).
std::string GCode::extrude_perimeters(const Print&                                         print,
                                      const std::vector<ObjectByExtruder::Island::Region>& by_region,
                                      bool                                                 is_first_layer,
                                      bool                                                 is_infill_first,
                                      bool                                                 unsupported_loops_only)
{
    std::string gcode;
    for (const ObjectByExtruder::Island::Region& region : by_region)
        if (!region.perimeters.empty()) {
            m_config.apply(print.get_print_region(&region - &by_region.front()).config());
            // BBS: for first layer, we always print wall firstly to get better bed adhesive force
            // This behaviour is same with cura
            const bool should_print = is_first_layer ? !is_infill_first : (m_config.is_infill_first == is_infill_first);
            if (!should_print)
                continue;

            // ORCA: loops flagged as extruded in mid air, out of reach of the layer below, are held back
            // for a second pass after the infill that anchors them. Infill already precedes infill first walls.
            const bool defer_unsupported = !is_infill_first;
            auto waits_for_infill = [](const ExtrusionEntity *ee) {
                return ee->is_loop() && static_cast<const ExtrusionLoop *>(ee)->print_after_infill;
            };

            // Build the printed prefix once in emission order, scoped to this
            // region. Disabled or zero-length wipes need no support geometry.
            std::optional<WipeInwardSupport> wipe_support;
            if (m_wipe.enable && m_config.wipe_inward && m_config.wipe_inward_distance.value > 0. &&
                scale_(EXTRUDER_CONFIG(wipe_distance)) > SCALED_EPSILON)
                wipe_support.emplace();
            for (const ExtrusionEntity* ee : region.perimeters) {
                if (defer_unsupported && waits_for_infill(ee) != unsupported_loops_only)
                    continue;
                gcode += this->extrude_entity(*ee, "perimeter", -1., region.perimeters,
                                             wipe_support ? &*wipe_support : nullptr);
                if (wipe_support)
                    wipe_support->append(*ee);
            }
        }
    return gcode;
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
std::string GCode::extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool ironing)
{
    std::string          gcode;
    ExtrusionEntitiesPtr extrusions;
    const char*          extrusion_name = ironing ? "ironing" : "infill";
    for (const ObjectByExtruder::Island::Region& region : by_region)
        if (!region.infills.empty()) {
            extrusions.clear();
            extrusions.reserve(region.infills.size());
            for (ExtrusionEntity* ee : region.infills)
                if ((ee->role() == erIroning) == ironing)
                    extrusions.emplace_back(ee);
            if (!extrusions.empty()) {
                m_config.apply(print.get_print_region(&region - &by_region.front()).config());
                chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);
                for (const ExtrusionEntity* fill : extrusions) {
                    auto* eec = dynamic_cast<const ExtrusionEntityCollection*>(fill);
                    if (eec) {
                        for (ExtrusionEntity* ee : eec->chained_path_from(m_last_pos).entities)
                            gcode += this->extrude_entity(*ee, extrusion_name);
                    } else
                        gcode += this->extrude_entity(*fill, extrusion_name);
                }
            }
        }
    return gcode;
}

std::string GCode::extrude_support(const ExtrusionEntityCollection& support_fills, const ExtrusionRole support_extrusion_role)
{
    static constexpr const char* support_label            = "support material";
    static constexpr const char* support_interface_label  = "support material interface";
    static constexpr const char* support_transition_label = "support transition";
    static constexpr const char* support_ironing_label    = "support ironing";

    std::string gcode;
    if (!support_fills.entities.empty()) {
        ExtrusionEntitiesPtr extrusions;
        extrusions.reserve(support_fills.entities.size());
        for (ExtrusionEntity* ee : support_fills.entities) {
            const auto role = ee->role();
            if ((role == support_extrusion_role) || (support_extrusion_role == erMixed && role != erIroning)) {
                extrusions.emplace_back(ee);
            }
        }
        if (extrusions.empty())
            return gcode;

        chain_and_reorder_extrusion_entities(extrusions, &m_last_pos);

        const double support_speed           = this->process_flow_value(m_config.support_speed);
        const double support_interface_speed = this->process_flow_value(m_config.support_interface_speed);
        for (const ExtrusionEntity* ee : extrusions) {
            ExtrusionRole role = ee->role();
            assert(role == erSupportMaterial || role == erSupportMaterialInterface || role == erSupportTransition || role == erIroning);
            const char* label = (role == erSupportMaterial) ?
                                    support_label :
                                    ((role == erSupportMaterialInterface) ?
                                         support_interface_label :
                                         ((role == erIroning) ? support_ironing_label : support_transition_label));
            // BBS
            // const double speed = (role == erSupportMaterial) ? support_speed : support_interface_speed;
            const double                     speed      = -1.0;
            const ExtrusionPath*             path       = dynamic_cast<const ExtrusionPath*>(ee);
            const ExtrusionMultiPath*        multipath  = dynamic_cast<const ExtrusionMultiPath*>(ee);
            const ExtrusionLoop*             loop       = dynamic_cast<const ExtrusionLoop*>(ee);
            const ExtrusionEntityCollection* collection = dynamic_cast<const ExtrusionEntityCollection*>(ee);
            if (path)
                gcode += this->extrude_path(*path, label, speed);
            else if (multipath) {
                gcode += this->extrude_multi_path(*multipath, label, speed);
            } else if (loop) {
                gcode += this->extrude_loop(*loop, label, speed);
            } else if (collection) {
                gcode += extrude_support(*collection, support_extrusion_role);
            } else {
                throw Slic3r::InvalidArgument("Unknown extrusion type");
            }
        }
    }
    return gcode;
}

bool GCode::GCodeOutputStream::is_error() const { return ::ferror(this->f); }

void GCode::GCodeOutputStream::flush() { ::fflush(this->f); }

void GCode::GCodeOutputStream::close()
{
    if (this->f) {
        ::fclose(this->f);
        this->f = nullptr;
    }
}

void GCode::GCodeOutputStream::write(const char* what)
{
    if (what != nullptr) {
        const char* gcode = what;
        // writes string to file
        fwrite(gcode, 1, ::strlen(gcode), this->f);
        // FIXME don't allocate a string, maybe process a batch of lines?
        m_processor.process_buffer(std::string(gcode));
    }
}

void GCode::GCodeOutputStream::writeln(const std::string& what)
{
    if (!what.empty())
        this->write(what.back() == '\n' ? what : what + '\n');
}

void GCode::GCodeOutputStream::write_format(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
#ifdef _MSC_VER
            ::_vscprintf(format, args2)
#else
            ::vsnprintf(nullptr, 0, format, args2)
#endif
            + 1;
        va_end(args2);
    }

    char  buffer[1024];
    bool  buffer_dynamic = buflen > 1024;
    char* bufptr         = buffer_dynamic ? (char*) malloc(buflen) : buffer;
    int   res            = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        this->write(bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

bool GCode::_needSAFC(const ExtrusionPath& path)
{
    if (!m_small_area_infill_flow_compensator || !m_config.small_area_infill_flow_compensation.value)
        return false;

    static const InfillPattern supported_patterns[] = {
        InfillPattern::ipRectilinear,
        InfillPattern::ipAlignedRectilinear,
        InfillPattern::ipMonotonic,
        InfillPattern::ipMonotonicLine,
    };

    return std::any_of(std::begin(supported_patterns), std::end(supported_patterns), [&](const InfillPattern pattern) {
        return this->on_first_layer() && this->config().bottom_surface_pattern == pattern ||
               path.role() == erSolidInfill && this->config().internal_solid_infill_pattern == pattern ||
               path.role() == erTopSolidInfill && this->config().top_surface_pattern == pattern;
    });
}

// ORCA: Overlap at or below which the overhang fan switches on; negative when it does not depend on overlap
// (Overhang_threshold_none cools every external perimeter).
static float overhang_fan_overlap_threshold(int overhang_fan_threshold)
{
    switch (overhang_fan_threshold) {
    case (int) Overhang_threshold_1_4: return 0.9f;
    case (int) Overhang_threshold_2_4: return 0.75f;
    case (int) Overhang_threshold_3_4: return 0.5f;
    case (int) Overhang_threshold_4_4: return 0.25f;
    case (int) Overhang_threshold_bridge: return 0.05f;
    default: return -1.f;
    }
}

std::string GCode::_extrude(const ExtrusionPath& path, std::string description, double speed)
{
    std::string gcode;

    if (is_bridge(path.role()))
        description += " (bridge)";

    const ExtrusionPathSloped* sloped = dynamic_cast<const ExtrusionPathSloped*>(&path);

    const auto get_sloped_z = [&sloped, this](double z_ratio) {
        const auto height = sloped->height;
        // Ultra: offset-layers shift (fraction of the layer height)
        const auto z_offset = sloped->z_offset;
        return lerp(m_nominal_z + z_offset * height - height, m_nominal_z + z_offset * height, z_ratio);
    };

    // ZAA (Z contouring). The path's BASE Z is the nominal layer Z plus the offset-layers shift;
    // the contour deltas stored on the path are relative to exactly that base, so the two compose
    // by addition and the flow below is derived once from the summed local height.
    const bool   zaa_contoured = path.z_contoured();
    const double zaa_base_z    = m_nominal_z + double(path.z_offset) * double(path.height);
    const double zaa_first_z   = zaa_contoured ? zaa_base_z + double(path.z_contour->z_at(path.first_point())) : zaa_base_z;

    bool slope_need_z_travel = false;
    if (sloped != nullptr && !sloped->is_flat()) {
        auto target_z       = get_sloped_z(sloped->slope_begin.z_ratio);
        slope_need_z_travel = m_writer.will_move_z(target_z);
    } else if (sloped == nullptr) {
        // Ultra: offset-layers paths must get their Z travel even when the XY start
        // point coincides with the previous path's end point (raised path after a
        // flat one, or a flat path after a raised one). ZAA rides on the same check.
        slope_need_z_travel = m_writer.will_move_z(zaa_first_z);
    }
    // Move to first point of extrusion path
    // path is 2D. But in slope lift case, lift z is done in travel_to function.
    // Add m_need_change_layer_lift_z when change_layer in case of no lift if m_last_pos is equal to path.first_point() by chance
    if (!m_last_pos_defined || m_last_pos != path.first_point() || m_need_change_layer_lift_z || slope_need_z_travel) {
        const bool _last_pos_undefined = !m_last_pos_defined;
        gcode += this->travel_to(path.first_point(), path.role(), "move to first " + description + " point",
                                 sloped == nullptr ? ((zaa_contoured || path.z_offset != 0.f) ? zaa_first_z : DBL_MAX)
                                                   : get_sloped_z(sloped->slope_begin.z_ratio));
        m_need_change_layer_lift_z = false;
        // Orca: force restore Z after unknown last pos
        if (_last_pos_undefined && !slope_need_z_travel) {
            gcode += this->writer().travel_to_z(m_last_layer_z, "force restore Z after unknown last pos", true);
        }
    }

    // ZAA: land on this path's own starting Z whatever the previous path left behind - a contoured
    // path ends wherever its last sample was, so the following path has to put Z back itself.
    // Gated on contouring having actually happened, so this whole block is inert when ZAA is off.
    if (sloped == nullptr && (zaa_contoured || m_zaa_z_dirty)) {
        const double zaa_current_z = m_writer.get_position().z();
        if (GCodeFormatter::quantize_xyzf(zaa_current_z) != GCodeFormatter::quantize_xyzf(zaa_first_z))
            gcode += this->writer().travel_to_z(zaa_first_z,
                                                zaa_contoured ? "set Z for contouring" : "reset Z after contouring", true);
        m_zaa_z_dirty = zaa_contoured;
    }

    // if needed, write the gcode_label_objects_end then gcode_label_objects_start
    // should be already done by travel_to, but just in case
    m_writer.add_object_change_labels(gcode);

    // compensate retraction
    gcode += this->unretract();
    m_config.apply(m_calib_config);

    const bool pointillism_path = path.inset_idx == k_pointillism_path_inset_marker;
    const double path_length_mm = unscale<double>(path.length());
    const double pointillism_pixel_size_mm = std::max(0.0, double(m_config.mixed_filament_pointillism_pixel_size.value));
    const double pointillism_nominal_segment_mm = pointillism_pixel_size_mm > EPSILON
        ? std::max(0.10, pointillism_pixel_size_mm)
        : std::max(0.20, double(m_config.nozzle_diameter.values.empty() ? 0.4 : m_config.nozzle_diameter.values.front()) * 2.0);
    const double pointillism_min_accel_switch_len_mm = std::max(0.30, pointillism_nominal_segment_mm * 1.5);
    const bool skip_accel_jerk_switch_for_short_pointillism =
        pointillism_path && path_length_mm <= pointillism_min_accel_switch_len_mm + EPSILON;

    // Orca: optimize for Klipper, set acceleration and jerk in one command
    unsigned int acceleration_i = 0;
    double       jerk           = 0;
    // adjust acceleration
    if (this->process_flow_value(m_config.default_acceleration) > 0) {
        double acceleration;
        const double outer_wall_acc = this->process_flow_value(m_config.outer_wall_acceleration);
        const double default_acc    = this->process_flow_value(m_config.default_acceleration);
        const auto   bridge_fop     = this->process_flow_value(m_config.bridge_acceleration);
        const auto   sparse_fop     = this->process_flow_value(m_config.sparse_infill_acceleration);
        const auto   solid_fop      = this->process_flow_value(m_config.internal_solid_infill_acceleration);
        const double bridge_acc     = bridge_fop.percent ? (bridge_fop.value * 0.01 * outer_wall_acc) : bridge_fop.value;
        const double sparse_acc     = sparse_fop.percent ? (sparse_fop.value * 0.01 * default_acc)    : sparse_fop.value;
        const double solid_acc      = solid_fop.percent  ? (solid_fop.value  * 0.01 * default_acc)    : solid_fop.value;
        if (this->on_first_layer() && this->process_flow_value(m_config.initial_layer_acceleration) > 0) {
            acceleration = this->process_flow_value(m_config.initial_layer_acceleration);
#if 0
        } else if (this->object_layer_over_raft() && m_config.first_layer_acceleration_over_raft.value > 0) {
            acceleration = m_config.first_layer_acceleration_over_raft.value;
#endif
        } else if (bridge_acc > 0 && is_bridge(path.role())) {
            acceleration = bridge_acc;
        } else if (sparse_acc > 0 && (path.role() == erInternalInfill)) {
            acceleration = sparse_acc;
        } else if (solid_acc > 0 && (path.role() == erSolidInfill)) {
            acceleration = solid_acc;
        } else if (this->process_flow_value(m_config.outer_wall_acceleration) > 0 && is_external_perimeter(path.role())) {
            acceleration = this->process_flow_value(m_config.outer_wall_acceleration);
        } else if (this->process_flow_value(m_config.inner_wall_acceleration) > 0 && is_internal_perimeter(path.role())) {
            acceleration = this->process_flow_value(m_config.inner_wall_acceleration);
        } else if (this->process_flow_value(m_config.top_surface_acceleration) > 0 && is_top_surface(path.role())) {
            acceleration = this->process_flow_value(m_config.top_surface_acceleration);
        } else {
            acceleration = this->process_flow_value(m_config.default_acceleration);
        }
        acceleration_i = (unsigned int) floor(acceleration + 0.5);
    }

    // adjust X Y jerk
    if (this->process_flow_value(m_config.default_jerk) > 0) {
        if (this->on_first_layer() && this->process_flow_value(m_config.initial_layer_jerk) > 0) {
            jerk = this->process_flow_value(m_config.initial_layer_jerk);
        } else if (this->process_flow_value(m_config.outer_wall_jerk) > 0 && is_external_perimeter(path.role())) {
            jerk = this->process_flow_value(m_config.outer_wall_jerk);
        } else if (this->process_flow_value(m_config.inner_wall_jerk) > 0 && is_internal_perimeter(path.role())) {
            jerk = this->process_flow_value(m_config.inner_wall_jerk);
        } else if (this->process_flow_value(m_config.top_surface_jerk) > 0 && is_top_surface(path.role())) {
            jerk = this->process_flow_value(m_config.top_surface_jerk);
        } else if (this->process_flow_value(m_config.infill_jerk) > 0 && is_infill(path.role())) {
            jerk = this->process_flow_value(m_config.infill_jerk);
        } else {
            jerk = this->process_flow_value(m_config.default_jerk);
        }
    }

    if (!skip_accel_jerk_switch_for_short_pointillism) {
        if (m_writer.get_gcode_flavor() == gcfKlipper) {
            gcode += m_writer.set_accel_and_jerk(acceleration_i, jerk);

        } else {
            gcode += m_writer.set_print_acceleration(acceleration_i);
            gcode += m_writer.set_jerk_xy(jerk);
        }
    }

    // calculate effective extrusion length per distance unit (e_per_mm)
    double filament_flow_ratio = m_config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    // We set _mm3_per_mm to effectove flow = Geometric volume * print flow ratio * filament flow ratio * role-based-flow-ratios
    auto _mm3_per_mm = path.mm3_per_mm * path.extrusion_multiplier * this->config().print_flow_ratio;
    _mm3_per_mm *= filament_flow_ratio;
    if (path.role() == erTopSolidInfill)
        _mm3_per_mm *= m_config.top_solid_infill_flow_ratio;
    else if (path.role() == erBottomSurface)
        _mm3_per_mm *= m_config.bottom_solid_infill_flow_ratio;
    // Ultra (over-support surfaces): the object config has already been applied into m_config for
    // the object being extruded, so this is the per-object value.
    else if (path.role() == erBottomSurfaceOverSupport || path.role() == erOverSupportPerimeter)
        _mm3_per_mm *= m_config.over_support_flow;
    else if (path.role() == erInternalBridgeInfill)
        _mm3_per_mm *= m_config.internal_bridge_flow;
    else if (sloped)
        _mm3_per_mm *= m_config.scarf_joint_flow_ratio;
    // Effective extrusion length per distance unit = (filament_flow_ratio/cross_section) * mm3_per_mm / print flow ratio
    // m_writer.extruder()->e_per_mm3() below is (filament flow ratio / cross-sectional area)
    double e_per_mm = m_writer.extruder()->e_per_mm3() * _mm3_per_mm;
    e_per_mm /= filament_flow_ratio;

    // set speed
    // Snapmaker: all role speeds are flow-variant vector options (coFloats / coFloatsOrPercents)
    // since the High-Flow change; resolve them variant-aware via process_flow_value like the
    // accelerations and jerks above. The old get_abs_value("<key>") path returned the first
    // vector slot for coFloats and hit undefined behavior for coFloatsOrPercents, which
    // produced denormal speeds and literal "G1 F0" output on internal bridges.
    // Snapmaker (feedrate guard): which config key this role's speed was read from. Recorded so
    // that if the value resolves to something the writer has to refuse, the resulting warning can
    // name the ACTUAL SETTING the user has to go fix - nothing in the UI suggests any of these
    // should ever be zero, so a message naming only the role would leave them hunting. Assigning a
    // string literal pointer costs nothing on the valid path; it is only ever read if the guard
    // fires.
    const char *speed_setting = nullptr;
    // A caller-supplied speed (skirt and brim pass support_speed explicitly, extrude_support
    // passes -1 and resolves below) did not come from this role switch, so attribute it to the
    // key those callers actually read rather than leaving the warning with no setting at all.
    if (speed != -1 && (path.role() == erSkirt || path.role() == erBrim))
        speed_setting = "support_speed";
    if (speed == -1) {
        if (path.role() == erPerimeter) {
            speed_setting = "inner_wall_speed";
            speed = this->process_flow_value(m_config.inner_wall_speed);
            if (sloped) {
                const double scarf = m_config.scarf_joint_speed.get_abs_value(speed);
                if (scarf < speed) {
                    speed         = scarf;
                    speed_setting = "scarf_joint_speed";
                }
            }
        } else if (path.role() == erExternalPerimeter) {
            speed_setting = "outer_wall_speed";
            speed = this->process_flow_value(m_config.outer_wall_speed);
            if (sloped) {
                const double scarf = m_config.scarf_joint_speed.get_abs_value(speed);
                if (scarf < speed) {
                    speed         = scarf;
                    speed_setting = "scarf_joint_speed";
                }
            }
        } else if (path.role() == erInternalBridgeInfill) {
            // internal_bridge_speed is a FloatOrPercent vector whose ratio_over target is
            // bridge_speed; resolve the percentage against the variant-matched bridge_speed.
            const auto   ib_fop  = this->process_flow_value(m_config.internal_bridge_speed);
            const double ib_base = this->process_flow_value(m_config.bridge_speed);
            speed                = ib_fop.percent ? (ib_fop.value * 0.01 * ib_base) : ib_fop.value;
            // A percentage resolves against bridge_speed, so a zero result can be either key's
            // fault. Name whichever one is actually bad; this is the exact pair that produced the
            // original G1 F0.
            speed_setting = (ib_fop.percent && !(ib_base > 0.)) ? "bridge_speed" : "internal_bridge_speed";
        } else if (path.role() == erOverhangPerimeter || path.role() == erSupportTransition || path.role() == erBridgeInfill) {
            speed_setting = "bridge_speed";
            speed = this->process_flow_value(m_config.bridge_speed);
        } else if (path.role() == erInternalInfill) {
            speed_setting = "sparse_infill_speed";
            speed = this->process_flow_value(m_config.sparse_infill_speed);
        } else if (path.role() == erSolidInfill) {
            speed_setting = "internal_solid_infill_speed";
            speed = this->process_flow_value(m_config.internal_solid_infill_speed);
        } else if (path.role() == erTopSolidInfill) {
            speed_setting = "top_surface_speed";
            speed = this->process_flow_value(m_config.top_surface_speed);
        } else if (path.role() == erIroning) {
            speed_setting = "ironing_speed";
            speed = this->process_flow_value(m_config.ironing_speed);
        } else if (path.role() == erBottomSurface) {
            speed_setting = "initial_layer_infill_speed";
            speed = this->process_flow_value(m_config.initial_layer_infill_speed);
        } else if (path.role() == erBottomSurfaceOverSupport || path.role() == erOverSupportPerimeter) {
            // Ultra (over-support surfaces / walls): 0 means "match the walls around this feature",
            // which is the whole point - the surface, and the wall continuing into it, should look
            // like the outer wall they are framed by, not like a bridge.
            speed_setting = "over_support_speed";
            speed = m_config.over_support_speed.value;
            if (speed <= 0.) {
                // Documented fallback, not a fault: 0 here MEANS "use the outer wall speed", so
                // if anything is wrong it is outer_wall_speed that the user must fix.
                speed_setting = "outer_wall_speed";
                speed = this->process_flow_value(m_config.outer_wall_speed);
            }
        } else if (path.role() == erGapFill) {
            speed_setting = "gap_infill_speed";
            speed = this->process_flow_value(m_config.gap_infill_speed);
        } else if (path.role() == erSupportMaterial || path.role() == erSupportMaterialInterface) {
            const double support_speed           = this->process_flow_value(m_config.support_speed);
            const double support_interface_speed = this->process_flow_value(m_config.support_interface_speed);
            const bool   is_support              = path.role() == erSupportMaterial;
            speed_setting                        = is_support ? "support_speed" : "support_interface_speed";
            speed                                = is_support ? support_speed : support_interface_speed;
        } else {
            throw Slic3r::InvalidArgument("Invalid speed");
        }
    }
    // BBS: if not set the speed, then use the filament_max_volumetric_speed directly.
    // Snapmaker: use !(speed >= eps) instead of speed == 0 so that denormals / NaN / garbage
    // from a misresolved config value also fall back to the volumetric floor instead of
    // being formatted into a literal "G1 F0". Legitimate speeds (>= ~1 mm/s) are unaffected.
    //
    // Snapmaker (feedrate guard): THIS is where a zero/denormal role speed is actually absorbed -
    // the writer-level guard downstream never sees it, because this line has already replaced it
    // with the volumetric ceiling. That ceiling is not a neutral choice: on the g1_error.3mf
    // bridge it is ~162 mm/s where the profile asks for 45, so a mistyped or misresolved setting
    // silently prints ~3.6x too fast. Report it here, with the setting name, for exactly the
    // reason the writer guard reports: the user must be told WHICH value was ignored.
    const bool speed_was_invalid = !(speed >= 1e-6);
    if (speed_was_invalid)
        speed = EXTRUDER_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm;
    if (this->on_first_layer()) {
        // BBS: for solid infill of initial layer, speed can be higher as long as
        // wall lines have be attached
        if (path.role() != erBottomSurface) {
            // This OVERRIDES the role speed outright on the first layer, so a bad
            // initial_layer_speed is the culprit here regardless of which role we are printing.
            speed_setting = "initial_layer_speed";
            speed = this->process_flow_value(m_config.initial_layer_speed);
        }
    } else if (m_config.slow_down_layers.values.front() > 1) {
        const auto _layer = layer_id();
        if (_layer > 0 && _layer < m_config.slow_down_layers.values.front()) {
            const bool perim = is_perimeter(path.role());
            const auto first_layer_speed = perim ? this->process_flow_value(m_config.initial_layer_speed) :
                                                   this->process_flow_value(m_config.initial_layer_infill_speed);
            if (first_layer_speed < speed) {
                speed = std::min(speed, Slic3r::lerp(first_layer_speed, speed, (double) _layer / m_config.slow_down_layers.values.front()));
                // The lerp floor is first_layer_speed, so if the result is unusable that key is
                // what dragged it down.
                if (!(speed >= 1e-6))
                    speed_setting = perim ? "initial_layer_speed" : "initial_layer_infill_speed";
            }
        }
    }
    // Override skirt speed if set
    if (path.role() == erSkirt) {
        const double skirt_speed = m_config.get_abs_value("skirt_speed");
        if (skirt_speed > 0.0) {
            speed_setting = "skirt_speed";
            speed = skirt_speed;
        }
    }
    // BBS: remove this config
    // else if (this->object_layer_over_raft())
    //     speed = m_config.get_abs_value("first_layer_speed_over_raft", speed);
    // if (m_config.max_volumetric_speed.value > 0) {
    //     // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
    //     speed = std::min(
    //         speed,
    //         m_config.max_volumetric_speed.value / _mm3_per_mm
    //     );
    // }
    if (EXTRUDER_CONFIG(filament_max_volumetric_speed) > 0) {
        // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
        speed = std::min(speed, EXTRUDER_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
    }
    // ORCA: resonance‑avoidance on short external perimeters
    {
        double ref_speed = speed; // stash the pre‑cap speed
        if (path.role() == erExternalPerimeter && m_config.resonance_avoidance.value) {
            // if our original speed was above “max”, disable RA for this loop
            if (ref_speed > m_config.max_resonance_avoidance_speed.value) {
                m_resonance_avoidance = false;
            }

            // re‑apply volumetric cap
            if (EXTRUDER_CONFIG(filament_max_volumetric_speed) > 0) {
                speed = std::min(speed, EXTRUDER_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
            }

            // if still in avoidance mode and under “max”, clamp to “min”
            if (m_resonance_avoidance && speed <= m_config.max_resonance_avoidance_speed.value) {
                speed = std::min(speed, m_config.min_resonance_avoidance_speed.value);
            }

            // reset flag for next segment
            m_resonance_avoidance = true;
        }
    }

    bool                        variable_speed = false;
    std::vector<ProcessedPoint> new_points{};

    // Ultra (over-support walls): a wall over support is a perimeter for every other purpose, but
    // it must never reach the overhang degree classification - it overlaps the lower layer by
    // nothing at all, so the estimator would grade it 4/4 and hand it the bridge speed, undoing the
    // whole feature. It is excluded here rather than dropped out of is_perimeter(), so seams,
    // travel/retraction and small-perimeter handling still see it as the wall it is.
    // docs/superpowers/specs/2026-09-05-over-support-surfaces.md
    if (this->process_flow_value(m_config.enable_overhang_speed) && !this->on_first_layer() && path.role() != erOverSupportPerimeter &&
        (is_bridge(path.role()) || is_perimeter(path.role()))) {
        bool   is_external = is_external_perimeter(path.role());
        double ref_speed   = is_external ? this->process_flow_value(m_config.outer_wall_speed) : this->process_flow_value(m_config.inner_wall_speed);
        if (ref_speed == 0)
            ref_speed = EXTRUDER_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm;

        if (EXTRUDER_CONFIG(filament_max_volumetric_speed) > 0) {
            ref_speed = std::min(ref_speed, EXTRUDER_CONFIG(filament_max_volumetric_speed) / _mm3_per_mm);
        }
        if (sloped) {
            ref_speed = std::min(ref_speed, m_config.scarf_joint_speed.get_abs_value(ref_speed));
        }

        ConfigOptionPercents overhang_overlap_levels({90, 75, 50, 25, 13, 0});

        // ORCA: Lets the path be split where the overhang fan switches, not only where the speed changes.
        // Bridges and overhang perimeters are cooled regardless of overlap.
        float fan_overlap_threshold = -1.f;
        if (EXTRUDER_CONFIG(enable_overhang_bridge_fan) && m_enable_cooling_markers && path.role() != erBridgeInfill &&
            path.role() != erOverhangPerimeter)
            fan_overlap_threshold = overhang_fan_overlap_threshold(EXTRUDER_CONFIG(overhang_fan_threshold));

        auto oh1_fop = this->process_flow_value(m_config.overhang_1_4_speed);
        auto oh2_fop = this->process_flow_value(m_config.overhang_2_4_speed);
        auto oh3_fop = this->process_flow_value(m_config.overhang_3_4_speed);
        auto oh4_fop = this->process_flow_value(m_config.overhang_4_4_speed);
        double oh1_abs = oh1_fop.percent ? (oh1_fop.value * 0.01 * ref_speed) : oh1_fop.value;
        double oh2_abs = oh2_fop.percent ? (oh2_fop.value * 0.01 * ref_speed) : oh2_fop.value;
        double oh3_abs = oh3_fop.percent ? (oh3_fop.value * 0.01 * ref_speed) : oh3_fop.value;
        double oh4_abs = oh4_fop.percent ? (oh4_fop.value * 0.01 * ref_speed) : oh4_fop.value;
        const bool slowdown_curled = this->process_flow_value(m_config.slowdown_for_curled_perimeters);

        if (slowdown_curled) {
            ConfigOptionFloatsOrPercents dynamic_overhang_speeds(
                {FloatOrPercent{100, true},
                 (oh1_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh1_abs * 100 / ref_speed, true},
                 (oh2_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh2_abs * 100 / ref_speed, true},
                 (oh3_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh3_abs * 100 / ref_speed, true},
                 (oh4_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh4_abs * 100 / ref_speed, true},
                 (oh4_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh4_abs * 100 / ref_speed, true}});

            new_points = m_extrusion_quality_estimator.estimate_extrusion_quality(path, overhang_overlap_levels, dynamic_overhang_speeds,
                                                                                  ref_speed, speed, slowdown_curled, fan_overlap_threshold);
        } else {
            ConfigOptionFloatsOrPercents dynamic_overhang_speeds(
                {FloatOrPercent{100, true},
                 (oh1_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh1_abs * 100 / ref_speed, true},
                 (oh2_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh2_abs * 100 / ref_speed, true},
                 (oh3_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh3_abs * 100 / ref_speed, true},
                 (oh4_abs < 0.5) ? FloatOrPercent{100, true} : FloatOrPercent{oh4_abs * 100 / ref_speed, true},
                 FloatOrPercent{this->process_flow_value(m_config.bridge_speed) * 100 / ref_speed, true}});

            new_points = m_extrusion_quality_estimator.estimate_extrusion_quality(path, overhang_overlap_levels, dynamic_overhang_speeds,
                                                                                  ref_speed, speed, slowdown_curled, fan_overlap_threshold);
        }
        variable_speed = std::any_of(new_points.begin(), new_points.end(), [speed](const ProcessedPoint& p) {
            return fabs(double(p.speed) - speed) > 1;
        }); // Ignore small speed variations (under 1mm/sec)
    }

    double F = speed * 60; // convert mm/sec to mm/min

    // Snapmaker (feedrate guard): hand the writer the provenance of this feedrate and the
    // preferred substitute, so that if F turns out to be non-positive / non-finite the guard can
    // name the offending setting in a user-visible warning instead of silently swapping in a
    // number. Both are plain stores of values already computed above - no formatting, no
    // allocation - so the valid path is unaffected.
    {
        FeedrateOrigin origin;
        origin.setting   = speed_setting;
        origin.role_name = ExtrusionEntity::role_to_cstr(path.role());
        if (m_layer != nullptr && m_layer->object() != nullptr && m_layer->object()->model_object() != nullptr)
            origin.object_name = m_layer->object()->model_object()->name.c_str();
        origin.layer_id = this->layer_id();
        m_writer.set_feedrate_origin(origin);
        // Preferred fallback: this object/region's resolved outer wall speed, converted to mm/min.
        // Resolved the same variant-aware way as every other role speed above. If it is itself
        // unusable the writer's ladder falls through to the last valid emitted speed.
        m_writer.set_guard_fallback_speed(this->process_flow_value(m_config.outer_wall_speed) * 60.);

        // The role speed was zero/denormal/NaN and the volumetric ceiling above stood in for it.
        // The writer guard will never fire for this path (F is now a large valid number), so
        // raise the warning from here instead - same sink, same message, same dedupe.
        if (speed_was_invalid)
            m_writer.report_bad_feedrate(0., F);
    }

    // ZAA (zaa_speed_scaling): emit a new feed rate for the contoured segments that follow.
    //
    // CoolingBuffer models one extrusion as ONE adjustable speed: the first
    // ";_EXTRUDE_SET_SPEED" line of a path becomes the block's speed modifier and every
    // movement line after it is folded into that modifier and then dropped
    // (CoolingBuffer.cpp ~470-491, `line.type = 0; // Don't store this line`). It also asserts
    // that no G1 inside such a block carries its own F (~476), so writing F into the movement
    // line is not an option either - and in Release that assert is gone and the line simply
    // escapes the layer-time slowdown: measured on the dome, contoured outer walls kept running
    // at 9124 mm/min where cooling had slowed the rest of the layer to 2841.
    //
    // So a ZAA speed change closes the current cooling block and opens a new one. Each block is
    // then independently adjustable, which is what lets the layer-time slowdown still reach
    // these moves. The cost is two extra lines per speed change, which is why the 5 % hysteresis
    // in ContourZ.hpp matters: it holds the changes to 2-3 % of contoured segments on a dome.
    //
    // The block additionally carries ZAA_COOLING_MARKER. Reopening the block was necessary but not
    // sufficient: the layer-time slowdown's usual operator caps every adjustable line at a COMMON
    // ceiling feed rate, which flattens exactly the per-segment ratio this feature creates (dome at
    // 0.2 mm: 205 distinct contoured feed rates spanning 3825-9659 mm/min became 23 spanning
    // 1200-3453, whole layers pinned to a single F). The marker tells CoolingBuffer to slow these
    // blocks PROPORTIONALLY instead, so F_seg/h_seg survives the slowdown and the layer still
    // reaches its cooling target.
    const auto zaa_emit_speed = [this, &gcode](double f, const std::string &cooling_comment) {
        if (m_enable_cooling_markers)
            gcode += ";_EXTRUDE_END\n";
        gcode += m_writer.set_speed(f, "", m_enable_cooling_markers
                                               ? cooling_comment + ZAA_COOLING_MARKER
                                               : cooling_comment);
    };

    // ZAA (zaa_speed_scaling): hold the volumetric flow of a contoured segment at the value this
    // role would have had at the nominal layer height, by scaling F by the inverse height ratio.
    //
    // The band is a full layer high, so at 0.2 mm layers a contoured bead swings from the nominal
    // 0.20 mm down to zaa_min_z (0.05 mm) at an unchanged F: the thin end is starved of material
    // and the Z axis is working hardest exactly where the bead is thinnest, which is what the
    // owner's 2026-09-08 print showed as a rougher surface at 0.2 mm than at thinner layers.
    //
    // The reference F is the one this path was about to run at, so the scaling composes with -
    // rather than overrides - every dynamic slowdown the fork already applies: the overhang
    // grading and curled-perimeter slowdown in new_points below (which sets its own per-point F,
    // scaled here in turn), small-perimeter / resonance avoidance, the initial-layer ramp, and the
    // filament_max_volumetric_speed cap that `speed` has already been clamped to.
    const bool   zaa_speed_scaling = zaa_contoured && m_config.zaa_speed_scaling.value;
    const double zaa_h_nominal     = double(path.height);
    const double zaa_max_vol       = EXTRUDER_CONFIG(filament_max_volumetric_speed);
    // The local height that set the F currently in force, mm; <= 0 means "no scaled F emitted yet
    // on this path", which forces the first contoured segment to emit one.
    double       zaa_speed_h_ref   = 0.;

    // Orca: Dynamic PA
    // If adaptive PA is enabled, by default evaluate PA on all extrusion moves
    bool is_pa_calib = m_curr_print->calib_mode() == CalibMode::Calib_PA_Line ||
                       m_curr_print->calib_mode() == CalibMode::Calib_PA_Pattern || m_curr_print->calib_mode() == CalibMode::Calib_PA_Tower;
    bool evaluate_adaptive_pa = false;
    bool role_change          = (m_last_extrusion_role != path.role());
    if (!is_pa_calib && EXTRUDER_CONFIG(adaptive_pressure_advance) && EXTRUDER_CONFIG(enable_pressure_advance)) {
        evaluate_adaptive_pa = true;
        // If we have already emmited a PA change because the m_multi_flow_segment_path_pa_set is set
        // skip re-issuing the PA change tag.
        if (m_multi_flow_segment_path_pa_set && evaluate_adaptive_pa)
            evaluate_adaptive_pa = false;
        // TODO: Explore forcing evaluation of PA if a role change is happening mid extrusion.
        // TODO: This would enable adapting PA for overhang perimeters as they are part of the current loop
        // TODO: The issue with simply enabling PA evaluation on a role change is that the speed change
        // TODO: is issued before the overhang perimeter role change is triggered
        // TODO: because for some reason (maybe path segmentation upstream?) there is a short path extruded
        // TODO: with the overhang speed and flow before the role change is flagged in the path.role() function.
        if (role_change)
            evaluate_adaptive_pa = true;
    }
    // Orca: End of dynamic PA trigger flag segment

    // Orca: process custom gcode for extrusion role change
    if (path.role() != m_last_extrusion_role && !m_config.change_extrusion_role_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(path.role())));
        config.set_key_value("last_extrusion_role", new ConfigOptionString(extrusion_role_to_string_for_parser(m_last_extrusion_role)));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_layer == nullptr ? m_last_height : m_layer->print_z));
        gcode += this->placeholder_parser_process("change_extrusion_role_gcode", m_config.change_extrusion_role_gcode.value,
                                                  m_writer.extruder()->id(), &config) +
                 "\n";
    }

    // extrude arc or line
    if (m_enable_extrusion_role_markers) {
        if (path.role() != m_last_extrusion_role) {
            char buf[32];
            sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(path.role()));
            gcode += buf;
        }
    }

    m_last_extrusion_role = path.role();

    // adds processor tags and updates processor tracking data
    // PrusaMultiMaterial::Writer may generate GCodeProcessor::Height_Tag lines without updating m_last_height
    // so, if the last role was erWipeTower we force export of GCodeProcessor::Height_Tag lines
    bool last_was_wipe_tower = (m_last_processor_extrusion_role == erWipeTower);
    char buf[64];
    assert(is_decimal_separator_point());

    if (path.role() != m_last_processor_extrusion_role) {
        m_last_processor_extrusion_role = path.role();
        sprintf(buf, ";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
                ExtrusionEntity::role_to_string(m_last_processor_extrusion_role).c_str());
        gcode += buf;
    }

    if (last_was_wipe_tower || m_last_width != path.width) {
        m_last_width = path.width;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width).c_str(), m_last_width);
        gcode += buf;
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    if (last_was_wipe_tower || (m_last_mm3_per_mm != path.mm3_per_mm)) {
        m_last_mm3_per_mm = path.mm3_per_mm;
        sprintf(buf, ";%s%f\n", GCodeProcessor::Mm3_Per_Mm_Tag.c_str(), m_last_mm3_per_mm);
        gcode += buf;
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    if (last_was_wipe_tower || std::abs(m_last_height - path.height) > EPSILON) {
        m_last_height = path.height;
        sprintf(buf, ";%s%g\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height).c_str(), m_last_height);
        gcode += buf;
    }

    // Orca: Dynamic PA
    // Post processor flag generation code segment when option to emit only at role changes is enabled
    // Variables published to the post processor:
    // 1) Tag to trigger a PA evaluation (because a role change was identified and the user has requested dynamic PA adjustments)
    // 2) Current extruder ID (to identify the PA model for the currently used extruder)
    // 3) mm3_per_mm value (to then multiply by the final model print speed after slowdown for cooling is applied)
    // 4) the current acceleration (to pass to the model for evaluation)
    // 5) whether this is an external perimeter (for future use)
    // 6) whether this segment is triggered because of a role change (to aid in calculation of average speed for the role)
    // This tag simplifies the creation of the gcode post processor while also keeping the feature decoupled from other tags.
    if (evaluate_adaptive_pa) {
        bool isOverhangPerimeter = (path.role() == erOverhangPerimeter);
        if (m_multi_flow_segment_path_average_mm3_per_mm > 0) {
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(),
                    m_multi_flow_segment_path_average_mm3_per_mm, acceleration_i,
                    ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)), role_change, isOverhangPerimeter);
            gcode += buf;
        } else if (_mm3_per_mm > 0) { // Triggered when extruding a single segment path (like a line).
                                      // Check if mm3_mm value is greater than zero as the wipe before external perimeter
                                      // is a zero mm3_mm path to force de-retraction to happen and we dont want
                                      // to issue a zero flow PA change command for this
            sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                    GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(), _mm3_per_mm,
                    acceleration_i, ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)), role_change,
                    isOverhangPerimeter);
            gcode += buf;
        }
    }

    auto overhang_fan_threshold     = EXTRUDER_CONFIG(overhang_fan_threshold);
    auto enable_overhang_bridge_fan = EXTRUDER_CONFIG(enable_overhang_bridge_fan);

    //    { "0%", Overhang_threshold_none },
    //    { "10%", Overhang_threshold_1_4 },
    //    { "25%", Overhang_threshold_2_4 },
    //    { "50%", Overhang_threshold_3_4 },
    //    { "75%", Overhang_threshold_4_4 },
    //    { "95%", Overhang_threshold_bridge }
    auto check_overhang_fan = [&overhang_fan_threshold](float overlap, ExtrusionRole role) {
        if (role == erBridgeInfill ||
            role == erOverhangPerimeter) { // ORCA: Split out bridge infill to internal and external to apply separate fan settings
            return true;
        }
        if (overhang_fan_threshold == Overhang_threshold_none)
            return is_external_perimeter(role);
        const float overlap_threshold = overhang_fan_overlap_threshold(overhang_fan_threshold);
        return overlap_threshold >= 0.f && overlap <= overlap_threshold;
    };

    std::string comment;
    if (m_enable_cooling_markers) {
        comment = ";_EXTRUDE_SET_SPEED";
        if (is_external_perimeter(path.role()))
            comment += ";_EXTERNAL_PERIMETER";
    }

    // Change fan speed based on current extrusion role
    auto append_role_based_fan_marker = [this, &gcode](const ExtrusionRole role, const std::string_view& marker_prefix, const bool fan_on) {
        assert(m_enable_cooling_markers);

        if (fan_on) {
            if (!m_is_role_based_fan_on[role]) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_START\n";
                m_is_role_based_fan_on[role] = true;
            }
        } else {
            if (m_is_role_based_fan_on[role]) {
                gcode += ";";
                gcode += marker_prefix;
                gcode += "_FAN_END\n";
                m_is_role_based_fan_on[role] = false;
            }
        }
    };
    auto apply_role_based_fan_speed = [&path, &append_role_based_fan_marker,
                                       supp_interface_fan_speed = EXTRUDER_CONFIG(support_material_interface_fan_speed),
                                       ironing_fan_speed        = EXTRUDER_CONFIG(ironing_fan_speed)] {
        append_role_based_fan_marker(erSupportMaterialInterface, "_SUPP_INTERFACE"sv,
                                     supp_interface_fan_speed >= 0 && path.role() == erSupportMaterialInterface);
        append_role_based_fan_marker(erIroning, "_IRONING"sv, ironing_fan_speed >= 0 && path.role() == erIroning);
    };

    if (!variable_speed) {
        // F is mm per minute.
        if ((std::abs(writer().get_current_speed() - F) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON)) {
            // ORCA: Adaptive PA code segment when adjusting PA within the same feature
            // There is a speed change coming out of an overhang region
            // or a flow change, so emit the flag to evaluate PA for the upcomming extrusion
            // Emit tag before new speed is set so the post processor reads the next speed immediately and uses it.
            // Dont emit tag if it has just already been emitted from a role change above
            if (_mm3_per_mm > 0 && EXTRUDER_CONFIG(adaptive_pressure_advance) && EXTRUDER_CONFIG(enable_pressure_advance) &&
                EXTRUDER_CONFIG(adaptive_pressure_advance_overhangs) && !evaluate_adaptive_pa) {
                if (writer().get_current_speed() >
                    F) { // Ramping down speed - use overhang logic where the minimum speed is used between current and upcoming extrusion
                    if (m_config.gcode_comments) {
                        sprintf(buf, "; Ramp down-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(), _mm3_per_mm,
                            acceleration_i, ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)),
                            1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not
                               // a role change,
                            // the properties of the extrusion in the overhang are different so it behaves similarly to a role
                            // change for the Adaptive PA post processor.
                            1);
                } else { // Ramping up speed - use baseline logic where max speed is used between current and upcoming extrusion
                    if (m_config.gcode_comments) {
                        sprintf(buf, "; Ramp up-non-variable\n");
                        gcode += buf;
                    }
                    sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(), _mm3_per_mm,
                            acceleration_i, ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)),
                            1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is not
                               // a role change,
                            // the properties of the extrusion in the overhang are different so it is technically similar to a role
                            // change for the Adaptive PA post processor.
                            0);
                }
                gcode += buf;
                m_last_mm3_mm = _mm3_per_mm;
            }
            // ORCA: End of adaptive PA code segment
        }

        gcode += m_writer.set_speed(F, "", comment);
        {
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    // BBS: Overhang_threshold_none means Overhang_threshold_1_4 and forcing cooling for all external
                    // perimeter
                    append_role_based_fan_marker(
                        erOverhangPerimeter, "_OVERHANG"sv,
                        (overhang_fan_threshold == Overhang_threshold_none && is_external_perimeter(path.role())) ||
                            (path.role() == erBridgeInfill ||
                             path.role() == erOverhangPerimeter)); // ORCA: Add support for separate internal bridge fan speed control

                    // ORCA: Add support for separate internal bridge fan speed control
                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }
            // BBS: use G1 if not enable arc fitting or has no arc fitting result or in spiral_mode mode or we are doing sloped extrusion
            // Attention: G2 and G3 is not supported in spiral_mode mode
            // ZAA: a contoured path carries per-point Z, which an arc move cannot express.
            if (!m_config.enable_arc_fitting || path.polyline.fitting_result.empty() || m_config.spiral_mode || sloped != nullptr ||
                zaa_contoured) {
                double path_length  = 0.;
                double total_length = sloped == nullptr ? 0. : path.polyline.length() * SCALING_FACTOR;
                for (const Line& line : path.polyline.lines()) {
                    std::string  tempDescription = description;
                    const double line_length     = line.length() * SCALING_FACTOR;
                    if (line_length < EPSILON)
                        continue;
                    path_length += line_length;
                    auto dE = e_per_mm * line_length;
                    if (_needSAFC(path)) {
                        auto oldE = dE;
                        dE        = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                        if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                            tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f", oldE, line_length);
                        }
                    }
                    if (zaa_contoured) {
                        // ZAA: variable Z per point, and the flow follows the LOCAL layer height.
                        // The height correction is applied exactly once here, on top of the
                        // e_per_mm that already carries offset-layers' bonding multiplier - the
                        // two are different quantities (a geometric height ratio and a bonding
                        // flow ratio) and the bonding layers are not contoured at all, see
                        // ContourZ.cpp.
                        const Vec2d  dest2d = this->point_to_gcode(line.b);
                        const double z_diff = double(path.z_contour->z_at(line.b));
                        // The local layer height varies ALONG the segment. Using the end point's
                        // height alone (upstream does) mis-extrudes every Z transition by half the
                        // step - measured at 12 % of the segment's flow on average and 19 % worst
                        // case on the wedge, alternating sign, which is a width variation the eye
                        // reads as fuzz. The trapezoid rule is the exact volume for a linear ramp.
                        const double z_prev = double(path.z_contour->z_at(line.a));
                        const double z_mean = 0.5 * (z_prev + z_diff);
                        const double ratio  = contour_z_extrusion_ratio(path.role() == erIroning, double(path.height), z_mean);
                        const double e      = dE * ratio;
                        // ZAA speed scaling. The segment's local height is exactly the height the
                        // E above was derived from (the trapezoid mean), so scaling F by the same
                        // ratio holds the extruder's melt rate at what the role was tuned for.
                        //
                        // It is emitted as its own `G1 F...` carrying the cooling marker, NOT as
                        // an F word inside the movement G1. CoolingBuffer only ever slows lines
                        // that sit inside a ";_EXTRUDE_SET_SPEED" block, and it asserts that no
                        // G1 inside such a block carries its own F (CoolingBuffer.cpp ~476). An
                        // F written into the movement line is therefore invisible to the layer-
                        // time slowdown: measured on the dome, contoured outer walls kept running
                        // at 9124 mm/min where cooling had slowed the rest of the layer to 2841.
                        // A separate F line costs one G-code line per speed change - and the 5 %
                        // hysteresis below is what keeps that from being one per move.
                        if (zaa_speed_scaling) {
                            const double h_seg = zaa_h_nominal + z_mean;
                            if (!contour_z_speed_within_hysteresis(zaa_speed_h_ref, h_seg)) {
                                const double zaa_F = contour_z_segment_feedrate(F, zaa_h_nominal, h_seg,
                                                                               zaa_max_vol, _mm3_per_mm * ratio);
                                if (zaa_F > 0. && std::abs(zaa_F - m_writer.get_current_speed()) > 1e-6)
                                    zaa_emit_speed(zaa_F, comment);
                                zaa_speed_h_ref = h_seg;
                            }
                        }
                        gcode += m_writer.extrude_to_xyz(Vec3d(dest2d.x(), dest2d.y(), zaa_base_z + z_diff), e,
                                                         GCodeWriter::full_gcode_comment ? tempDescription : "",
                                                         path.is_force_no_extrusion());
                    } else if (sloped == nullptr) {
                        // Normal extrusion
                        gcode += m_writer.extrude_to_xy(this->point_to_gcode(line.b), dE,
                                                        GCodeWriter::full_gcode_comment ? tempDescription : "",
                                                        path.is_force_no_extrusion());
                    } else {
                        // Sloped extrusion
                        const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                        Vec2d dest2d                  = this->point_to_gcode(line.b);
                        Vec3d dest3d(dest2d(0), dest2d(1), get_sloped_z(z_ratio));
                        gcode += m_writer.extrude_to_xyz(dest3d, dE * e_ratio, GCodeWriter::full_gcode_comment ? tempDescription : "",
                                                         path.is_force_no_extrusion());
                    }
                }
            } else {
                // BBS: start to generate gcode from arc fitting data which includes line and arc
                const std::vector<PathFittingData>& fitting_result = path.polyline.fitting_result;
                for (size_t fitting_index = 0; fitting_index < fitting_result.size(); fitting_index++) {
                    std::string tempDescription = description;
                    switch (fitting_result[fitting_index].path_type) {
                    case EMovePathType::Linear_move: {
                        size_t start_index = fitting_result[fitting_index].start_point_index;
                        size_t end_index   = fitting_result[fitting_index].end_point_index;
                        for (size_t point_index = start_index + 1; point_index < end_index + 1; point_index++) {
                            tempDescription          = description;
                            const Line   line        = Line(path.polyline.points[point_index - 1], path.polyline.points[point_index]);
                            const double line_length = line.length() * SCALING_FACTOR;
                            if (line_length < EPSILON)
                                continue;
                            auto dE = e_per_mm * line_length;
                            if (_needSAFC(path)) {
                                auto oldE = dE;
                                dE        = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                    tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f", oldE, line_length);
                                }
                            }
                            gcode += m_writer.extrude_to_xy(this->point_to_gcode(line.b), dE,
                                                            GCodeWriter::full_gcode_comment ? tempDescription : "",
                                                            path.is_force_no_extrusion());
                        }
                        break;
                    }
                    case EMovePathType::Arc_move_cw:
                    case EMovePathType::Arc_move_ccw: {
                        const ArcSegment& arc        = fitting_result[fitting_index].arc_data;
                        const double      arc_length = fitting_result[fitting_index].arc_data.length * SCALING_FACTOR;
                        if (arc_length < EPSILON)
                            continue;
                        const Vec2d center_offset = this->point_to_gcode(arc.center) - this->point_to_gcode(arc.start_point);
                        auto        dE            = e_per_mm * arc_length;
                        if (_needSAFC(path)) {
                            auto oldE = dE;
                            dE        = m_small_area_infill_flow_compensator->modify_flow(arc_length, dE, path.role());

                            if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                                tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f", oldE, arc_length);
                            }
                        }
                        gcode += m_writer.extrude_arc_to_xy(this->point_to_gcode(arc.end_point), center_offset, dE,
                                                            arc.direction == ArcDirection::Arc_Dir_CCW,
                                                            GCodeWriter::full_gcode_comment ? tempDescription : "",
                                                            path.is_force_no_extrusion());
                        break;
                    }
                    default:
                        // BBS: should never happen that a empty path_type has been stored
                        assert(0);
                        break;
                    }
                }
            }
        }
    } else {
        double last_set_speed = new_points[0].speed * 60.0;

        double total_length = 0;
        if (sloped != nullptr) {
            // Calculate total extrusion length
            Points p;
            p.reserve(new_points.size());
            std::transform(new_points.begin(), new_points.end(), std::back_inserter(p), [](const ProcessedPoint& pp) { return pp.p; });
            Polyline l(p);
            total_length = l.length() * SCALING_FACTOR;
        }
        gcode += m_writer.set_speed(last_set_speed, "", comment);
        Vec2d prev            = this->point_to_gcode_quantized(new_points[0].p);
        bool  pre_fan_enabled = false;
        bool  cur_fan_enabled = false;
        if (m_enable_cooling_markers && enable_overhang_bridge_fan)
            pre_fan_enabled = check_overhang_fan(new_points[0].overlap, path.role());

        if (path.role() == erInternalBridgeInfill) // ORCA: Add support for separate internal bridge fan speed control
            pre_fan_enabled = true;

        double path_length = 0.;
        for (size_t i = 1; i < new_points.size(); i++) {
            std::string           tempDescription     = description;
            const ProcessedPoint& processed_point     = new_points[i];
            const ProcessedPoint& pre_processed_point = new_points[i - 1];
            Vec2d                 p                   = this->point_to_gcode_quantized(processed_point.p);
            if (m_enable_cooling_markers) {
                if (enable_overhang_bridge_fan) {
                    cur_fan_enabled = check_overhang_fan(processed_point.overlap, path.role());
                    append_role_based_fan_marker(erOverhangPerimeter, "_OVERHANG"sv, pre_fan_enabled && cur_fan_enabled);
                    pre_fan_enabled = cur_fan_enabled;

                    // ORCA: Add support for separate internal bridge fan speed control
                    append_role_based_fan_marker(erInternalBridgeInfill, "_INTERNAL_BRIDGE"sv, path.role() == erInternalBridgeInfill);
                }

                apply_role_based_fan_speed();
            }

            const double line_length = (p - prev).norm();
            if (line_length < EPSILON)
                continue;
            path_length += line_length;
            double new_speed = pre_processed_point.speed * 60.0;

            if ((std::abs(last_set_speed - new_speed) > EPSILON) || (std::abs(_mm3_per_mm - m_last_mm3_mm) > EPSILON)) {
                // ORCA: Adaptive PA code segment when adjusting PA within the same feature
                // There is a speed change or flow change so emit the flag to evaluate PA for the upcomming extrusion
                // Emit tag before new speed is set so the post processor reads the next speed immediately and uses it.
                if (_mm3_per_mm > 0 && EXTRUDER_CONFIG(adaptive_pressure_advance) && EXTRUDER_CONFIG(enable_pressure_advance) &&
                    EXTRUDER_CONFIG(adaptive_pressure_advance_overhangs)) {
                    if (last_set_speed > new_speed) { // Ramping down speed - use overhang logic where the minimum speed is used between
                                                      // current and upcoming extrusion
                        if (m_config.gcode_comments) {
                            sprintf(buf, "; Ramp up-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(),
                                _mm3_per_mm, acceleration_i, ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)),
                                1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is
                                   // not a role change,
                                // the properties of the extrusion in the overhang are different so it is technically similar to a role
                                // change for the Adaptive PA post processor.
                                1);
                    } else { // Ramping up speed - use baseline logic where max speed is used between current and upcoming extrusion
                        if (m_config.gcode_comments) {
                            sprintf(buf, "; Ramp down-variable\n");
                            gcode += buf;
                        }
                        sprintf(buf, ";%sT%u MM3MM:%g ACCEL:%u BR:%d RC:%d OV:%d\n",
                                GCodeProcessor::reserved_tag(GCodeProcessor::ETags::PA_Change).c_str(), m_writer.extruder()->id(),
                                _mm3_per_mm, acceleration_i, ((path.role() == erBridgeInfill) || (path.role() == erOverhangPerimeter)),
                                1, // Force a dummy "role change" & "overhang perimeter" for the post processor, as, while technically it is
                                   // not a role change,
                                // the properties of the extrusion in the overhang are different so it is technically similar to a role
                                // change for the Adaptive PA post processor.
                                0);
                    }
                    gcode += buf;
                    m_last_mm3_mm = _mm3_per_mm;
                }
            } // ORCA: End of adaptive PA code segment

            // Ignore small speed variations - emit speed change if the delta between current and new is greater than 60mm/min / 1mm/sec
            // Reset speed to F if delta to F is less than 1mm/sec
            if ((std::abs(last_set_speed - new_speed) > 60)) {
                gcode += m_writer.set_speed(new_speed, "", comment);
                last_set_speed = new_speed;
                // ZAA: the base feed rate just changed, so any scaled F in force is stale.
                zaa_speed_h_ref = 0.;
            } else if ((std::abs(F - new_speed) <= 60)) {
                gcode += m_writer.set_speed(F, "", comment);
                last_set_speed = F;
                zaa_speed_h_ref = 0.;
            }
            auto dE = e_per_mm * line_length;
            if (_needSAFC(path)) {
                auto oldE = dE;
                dE        = m_small_area_infill_flow_compensator->modify_flow(line_length, dE, path.role());

                if (m_config.gcode_comments && oldE > 0 && oldE != dE) {
                    tempDescription += Slic3r::format(" | Old Flow Value: %0.5f Length: %0.5f", oldE, line_length);
                }
            }
            if (zaa_contoured) {
                // ZAA: see the constant-speed branch above, including the trapezoid height.
                const double z_diff = double(path.z_contour->z_at(processed_point.p));
                const double z_prev = double(path.z_contour->z_at(pre_processed_point.p));
                const double z_mean = 0.5 * (z_prev + z_diff);
                const double ratio  = contour_z_extrusion_ratio(path.role() == erIroning, double(path.height), z_mean);
                const double e      = dE * ratio;
                // ZAA speed scaling on top of the per-point dynamic speed: the reference is
                // last_set_speed, i.e. the F the overhang / curled-perimeter estimator just put in
                // force for this segment, so the two compose and the slower of the two wins.
                // Emitted as its own F line for the cooling reason documented in the
                // constant-speed branch above.
                if (zaa_speed_scaling) {
                    const double h_seg = zaa_h_nominal + z_mean;
                    if (!contour_z_speed_within_hysteresis(zaa_speed_h_ref, h_seg)) {
                        const double zaa_F = contour_z_segment_feedrate(last_set_speed, zaa_h_nominal, h_seg,
                                                                       zaa_max_vol, _mm3_per_mm * ratio);
                        if (zaa_F > 0. && std::abs(zaa_F - m_writer.get_current_speed()) > 1e-6)
                            zaa_emit_speed(zaa_F, comment);
                        zaa_speed_h_ref = h_seg;
                    }
                }
                gcode += m_writer.extrude_to_xyz(Vec3d(p.x(), p.y(), zaa_base_z + z_diff), e,
                                                 GCodeWriter::full_gcode_comment ? tempDescription : "");
            } else if (sloped == nullptr) {
                // Normal extrusion
                gcode += m_writer.extrude_to_xy(p, dE, GCodeWriter::full_gcode_comment ? tempDescription : "");
            } else {
                // Sloped extrusion
                const auto [z_ratio, e_ratio] = sloped->interpolate(path_length / total_length);
                Vec3d dest3d(p(0), p(1), get_sloped_z(z_ratio));
                gcode += m_writer.extrude_to_xyz(dest3d, dE * e_ratio, GCodeWriter::full_gcode_comment ? tempDescription : "");
            }

            prev = p;
        }
    }
    if (m_enable_cooling_markers) {
        gcode += ";_EXTRUDE_END\n";
    }

    if (path.role() != ExtrusionRole::erGapFill) {
        m_last_notgapfill_extrusion_role = path.role();
    }

    // Snapmaker (feedrate guard): this path's provenance stops being true once we leave it, so
    // drop it rather than let a later travel move or custom G-code inherit it and be blamed on
    // the wrong setting. The fallback speed is deliberately left in place - it stays a valid
    // substitute between extrusions.
    m_writer.clear_feedrate_origin();

    this->set_last_pos(path.last_point());
    return gcode;
}

// Orca: get string name of extrusion role. used for change_extruder_role_gcode
std::string GCode::extrusion_role_to_string_for_parser(const ExtrusionRole& role)
{
    switch (role) {
    case erPerimeter: return "Perimeter";
    case erExternalPerimeter: return "ExternalPerimeter";
    case erOverhangPerimeter: return "OverhangPerimeter";
    case erOverSupportPerimeter: return "OverSupportPerimeter";
    case erInternalInfill: return "InternalInfill";
    case erSolidInfill: return "SolidInfill";
    case erTopSolidInfill: return "TopSolidInfill";
    case erBottomSurface: return "BottomSurface";
    case erBottomSurfaceOverSupport: return "BottomSurfaceOverSupport";
    case erBridgeInfill:
    case erInternalBridgeInfill: return "BridgeInfill";
    case erGapFill: return "GapFill";
    case erIroning: return "Ironing";
    case erSkirt: return "Skirt";
    case erBrim: return "Brim";
    case erSupportMaterial: return "SupportMaterial";
    case erSupportMaterialInterface: return "SupportMaterialInterface";
    case erSupportTransition: return "SupportTransition";
    case erWipeTower: return "WipeTower";
    case erCustom:
    case erMixed:
    case erCount:
    case erNone:
    default: return "Mixed";
    }
}

std::string encodeBase64(uint64_t value)
{
    // Always use big endian mode
    uint8_t src[8];
    for (size_t i = 0; i < 8; i++)
        src[i] = (value >> (8 * i)) & 0xff;

    std::string dest;
    dest.resize(boost::beast::detail::base64::encoded_size(sizeof(src)));
    dest.resize(boost::beast::detail::base64::encode(&dest[0], src, sizeof(src)));
    return dest;
}

std::string GCode::_encode_label_ids_to_base64(std::vector<size_t> ids)
{
    assert(m_label_objects_ids.size() < 64);

    uint64_t bitset = 0;
    for (size_t id : ids) {
        auto index = std::lower_bound(m_label_objects_ids.begin(), m_label_objects_ids.end(), id);
        if (index != m_label_objects_ids.end() && *index == id)
            bitset |= (1ull << (index - m_label_objects_ids.begin()));
        else
            throw Slic3r::LogicError("Unknown label object id!");
    }
    if (bitset == 0)
        throw Slic3r::LogicError("Label object id error!");

    return encodeBase64(bitset);
}

// This method accepts &point in print coordinates.
std::string GCode::travel_to(const Point& point, ExtrusionRole role, std::string comment, double z /* = DBL_MAX*/)
{
    /*  Define the travel move as a line between current position and the taget point.
        This is expressed in print coordinates, so it will need to be translated by
        this->origin in order to get G-code coordinates.  */
    Polyline travel{this->last_pos(), point};

    // check whether a straight travel move would need retraction
    LiftType lift_type        = LiftType::SpiralLift;
    bool     needs_retraction = this->needs_retraction(travel, role, lift_type);
    // check whether wipe could be disabled without causing visible stringing
    bool could_be_wipe_disabled = false;
    // Save state of use_external_mp_once for the case that will be needed to call twice m_avoid_crossing_perimeters.travel_to.
    const bool  used_external_mp_once = m_avoid_crossing_perimeters.used_external_mp_once();
    std::string gcode;

    // Orca: we don't need to optimize the Klipper as only set once
    double       jerk_to_set         = 0.0;
    unsigned int acceleration_to_set = 0;
    if (this->on_first_layer()) {
        if (this->process_flow_value(m_config.default_acceleration) > 0 && this->process_flow_value(m_config.initial_layer_acceleration) > 0) {
            acceleration_to_set = (unsigned int) floor(this->process_flow_value(m_config.initial_layer_acceleration) + 0.5);
        }
        if (this->process_flow_value(m_config.default_jerk) > 0 && this->process_flow_value(m_config.initial_layer_jerk) > 0) {
            jerk_to_set = this->process_flow_value(m_config.initial_layer_jerk);
        }
    } else {
        if (this->process_flow_value(m_config.default_acceleration) > 0 && this->process_flow_value(m_config.travel_acceleration) > 0) {
            acceleration_to_set = (unsigned int) floor(this->process_flow_value(m_config.travel_acceleration) + 0.5);
        }
        if (this->process_flow_value(m_config.default_jerk) > 0 && this->process_flow_value(m_config.travel_jerk) > 0) {
            jerk_to_set = this->process_flow_value(m_config.travel_jerk);
        }
    }
    if (m_writer.get_gcode_flavor() == gcfKlipper) {
        gcode += m_writer.set_accel_and_jerk(acceleration_to_set, jerk_to_set);
    } else {
        gcode += m_writer.set_travel_acceleration(acceleration_to_set);
        gcode += m_writer.set_jerk_xy(jerk_to_set);
    }

    // if a retraction would be needed, try to use reduce_crossing_wall to plan a
    // multi-hop travel path inside the configuration space
    if (m_config.reduce_crossing_wall && !m_avoid_crossing_perimeters.disabled_once() && m_writer.is_current_position_clear())
    // BBS: don't generate detour travel paths when current position is unclea
    {
        travel = m_avoid_crossing_perimeters.travel_to(*this, point, &could_be_wipe_disabled);
        // check again whether the new travel path still needs a retraction
        needs_retraction = this->needs_retraction(travel, role, lift_type);
        // if (needs_retraction && m_layer_index > 1) exit(0);
    }

    // Re-allow reduce_crossing_wall for the next travel moves
    m_avoid_crossing_perimeters.reset_once_modifiers();

    // generate G-code for the travel move
    if (needs_retraction) {
        // ORCA: Fix scenario where wipe is disabled when avoid crossing perimeters was enabled even though a retraction move was performed.
        // This replicates the existing behaviour of always wiping when retracting
        /*if (m_config.reduce_crossing_wall && could_be_wipe_disabled)
            m_wipe.reset_path();*/

        Point last_post_before_retract = this->last_pos();
        gcode += this->retract(false, false, lift_type, role);
        // When "Wipe while retracting" is enabled, then extruder moves to another position, and travel from this position can cross perimeters.
        // Because of it, it is necessary to call avoid crossing perimeters again with new starting point after calling retraction()
        // FIXME Lukas H.: Try to predict if this second calling of avoid crossing perimeters will be needed or not. It could save computations.
        if (last_post_before_retract != this->last_pos() && m_config.reduce_crossing_wall) {
            // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value for
            // next call.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.use_external_mp_once();
            travel = m_avoid_crossing_perimeters.travel_to(*this, point);
            // If state of use_external_mp_once was changed reset it to right value.
            if (used_external_mp_once)
                m_avoid_crossing_perimeters.reset_once_modifiers();
        }
    } else {
        // Reset the wipe path when traveling, so one would not wipe along an old path.
        m_wipe.reset_path();
        // if (m_config.reduce_crossing_wall) {
        //     // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value
        //     for next call. if (used_external_mp_once) m_avoid_crossing_perimeters.use_external_mp_once(); travel =
        //     m_avoid_crossing_perimeters.travel_to(*this, point);
        //     // If state of use_external_mp_once was changed reset it to right value.
        //     if (used_external_mp_once) m_avoid_crossing_perimeters.reset_once_modifiers();
        // }
    }

    // if needed, write the gcode_label_objects_end then gcode_label_objects_start
    m_writer.add_object_change_labels(gcode);

    // use G1 because we rely on paths being straight (G0 may make round paths)
    if (travel.size() >= 2) {
        // Orca: use `travel_to_xyz` to ensure we start at the correct z, in case we moved z in custom/filament change gcode
        if (false /*m_spiral_vase*/) {
            // No lazy z lift for spiral vase mode
            for (size_t i = 1; i < travel.size(); ++i) {
                gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
            }
        } else {
            if (travel.size() == 2) {
                // No extra movements emitted by avoid_crossing_perimeters, simply move to the end point with z change
                const auto& dest2d = this->point_to_gcode(travel.points.back());
                Vec3d       dest3d(dest2d(0), dest2d(1), z == DBL_MAX ? m_nominal_z : z);
                gcode += m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                m_need_change_layer_lift_z = false;
            } else {
                // Extra movements emitted by avoid_crossing_perimeters, lift the z to normal height at the beginning, then apply the z
                // ratio at the last point
                for (size_t i = 1; i < travel.size(); ++i) {
                    if (i == 1) {
                        // Lift to normal z at beginning
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), m_nominal_z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment, m_need_change_layer_lift_z);
                        m_need_change_layer_lift_z = false;
                    } else if (z != DBL_MAX && i == travel.size() - 1) {
                        // Apply z_ratio for the very last point
                        Vec2d dest2d = this->point_to_gcode(travel.points[i]);
                        Vec3d dest3d(dest2d(0), dest2d(1), z);
                        gcode += m_writer.travel_to_xyz(dest3d, comment);
                    } else {
                        // For all points in between, no z change
                        gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), comment);
                    }
                }
            }
        }
        this->set_last_pos(travel.points.back());
    }

    return gcode;
}

// BBS
LiftType GCode::to_lift_type(ZHopType z_hop_types)
{
    switch (z_hop_types) {
    case ZHopType::zhtNormal: return LiftType::NormalLift;
    case ZHopType::zhtSlope: return LiftType::LazyLift;
    case ZHopType::zhtSpiral: return LiftType::SpiralLift;
    default:
        // if no corresponding lift type, use normal lift
        return LiftType::NormalLift;
    }
};

bool GCode::needs_retraction(const Polyline& travel, ExtrusionRole role, LiftType& lift_type)
{
    if (this->writer().extruder() == nullptr)
        return false;
    if (travel.length() < scale_(EXTRUDER_CONFIG(retraction_minimum_travel))) {
        // skip retraction if the move is shorter than the configured threshold
        return false;
    }

    // BBS: input travel polyline must be in current plate coordinate system
    auto is_through_overhang = [this](const Polyline& travel) {
        BoundingBox travel_bbox = get_extents(travel);
        travel_bbox.inflated(1);
        travel_bbox.defined = true;

        // do not scale for z
        const float             protect_z = 0.4;
        std::pair<float, float> z_range;
        z_range.second = m_layer ? m_layer->print_z : 0.f;
        z_range.first  = std::max(0.f, z_range.second - protect_z);
        std::vector<LayerPtrs>   layers_of_objects;
        std::vector<BoundingBox> boundingBox_for_objects;
        VecOfPoints              objects_instances_shift;
        std::vector<size_t> idx_of_object_sorted = m_curr_print->layers_sorted_for_object(z_range.first, z_range.second, layers_of_objects,
                                                                                          boundingBox_for_objects, objects_instances_shift);

        std::vector<bool> is_layers_of_objects_sorted(layers_of_objects.size(), false);

        for (size_t idx : idx_of_object_sorted) {
            for (const Point& instance_shift : objects_instances_shift[idx]) {
                BoundingBox instance_bbox = boundingBox_for_objects[idx];
                if (!instance_bbox.defined) // BBS: Don't need to check when bounding box of overhang area is empty(undefined)
                    continue;

                instance_bbox.offset(scale_(EPSILON));
                instance_bbox.translate(instance_shift.x(), instance_shift.y());
                if (!instance_bbox.overlap(travel_bbox))
                    continue;

                Polygons temp;
                temp.emplace_back(std::move(instance_bbox.polygon()));
                if (intersection_pl(travel, temp).empty())
                    continue;

                if (!is_layers_of_objects_sorted[idx]) {
                    std::sort(layers_of_objects[idx].begin(), layers_of_objects[idx].end(),
                              [](auto left, auto right) { return left->loverhangs_bbox.area() > right->loverhangs_bbox.area(); });
                    is_layers_of_objects_sorted[idx] = true;
                }

                for (const auto& layer : layers_of_objects[idx]) {
                    for (ExPolygon overhang : layer->loverhangs) {
                        overhang.translate(instance_shift);
                        BoundingBox bbox1 = get_extents(overhang);

                        if (!bbox1.overlap(travel_bbox))
                            continue;

                        if (intersection_pl(travel, overhang).empty())
                            continue;

                        return true;
                    }
                }
            }
        }
        return false;
    };

    float max_z_hop = 0.f;
    for (int i = 0; i < m_config.z_hop.size(); i++)
        max_z_hop = std::max(max_z_hop, (float) m_config.z_hop.get_at(i));
    float    travel_len_thresh = scale_(max_z_hop / tan(this->writer().extruder()->travel_slope()));
    float    accum_len         = 0.f;
    Polyline clipped_travel;

    clipped_travel.append(Polyline(travel.points[0], travel.points[1]));
    if (clipped_travel.length() > travel_len_thresh)
        clipped_travel.points.back() = clipped_travel.points.front() + (clipped_travel.points.back() - clipped_travel.points.front()) *
                                                                           (travel_len_thresh / clipped_travel.length());
    // BBS: translate to current plate coordinate system
    clipped_travel.translate(
        Point::new_scale(double(m_origin.x() - m_writer.get_xy_offset().x()), double(m_origin.y() - m_writer.get_xy_offset().y())));

    // BBS: force to retract when leave from external perimeter for a long travel
    // Better way is judging whether the travel move direction is same with last extrusion move.
    if (is_perimeter(m_last_processor_extrusion_role) && m_last_processor_extrusion_role != erPerimeter) {
        if (ZHopType(EXTRUDER_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
            lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::LazyLift;
        } else {
            lift_type = to_lift_type(ZHopType(EXTRUDER_CONFIG(z_hop_types)));
        }
        return true;
    }

    if (role == erSupportMaterial || role == erSupportTransition) {
        const SupportLayer* support_layer = dynamic_cast<const SupportLayer*>(m_layer);
        // FIXME support_layer->support_islands.contains should use some search structure!
        if (support_layer != NULL)
            // skip retraction if this is a travel move inside a support material island
            // FIXME not retracting over a long path may cause oozing, which in turn may result in missing material
            // at the end of the extrusion path!
            for (const ExPolygon& support_island : support_layer->support_islands)
                if (support_island.contains(travel))
                    return false;
        // reduce the retractions in lightning infills for tree support
        if (support_layer != NULL && support_layer->support_type == stInnerTree)
            for (auto& area : support_layer->base_areas)
                if (area.contains(travel))
                    return false;
    }
    // BBS: need retract when long moving to print perimeter to avoid dropping of material
    if (!is_perimeter(role) && m_config.reduce_infill_retraction && m_layer != nullptr && m_config.sparse_infill_density.value > 0 &&
        m_retract_when_crossing_perimeters.travel_inside_internal_regions(*m_layer, travel))
        // Skip retraction if travel is contained in an internal slice *and*
        // internal infill is enabled (so that stringing is entirely not visible).
        // FIXME any_internal_region_slice_contains() is potentionally very slow, it shall test for the bounding boxes first.
        return false;

    // retract if reduce_infill_retraction is disabled or doesn't apply when role is perimeter
    if (ZHopType(EXTRUDER_CONFIG(z_hop_types)) == ZHopType::zhtAuto) {
        lift_type = is_through_overhang(clipped_travel) ? LiftType::SpiralLift : LiftType::LazyLift;
    } else {
        lift_type = to_lift_type(ZHopType(EXTRUDER_CONFIG(z_hop_types)));
    }
    return true;
}

std::string GCode::retract(bool toolchange, bool is_last_retraction, LiftType lift_type, ExtrusionRole role)
{
    std::string gcode;

    if (m_writer.extruder() == nullptr)
        return gcode;

    // wipe (if it's enabled for this extruder and we have a stored wipe path and no-zero wipe distance)
    if (EXTRUDER_CONFIG(wipe) && m_wipe.has_path() && scale_(EXTRUDER_CONFIG(wipe_distance)) > SCALED_EPSILON) {
        Wipe::RetractionValues wipeRetractions = m_wipe.calculateWipeRetractionLengths(*this, toolchange);
        gcode += toolchange ? m_writer.retract_for_toolchange(true, wipeRetractions.retractLengthBeforeWipe) :
                              m_writer.retract(true, wipeRetractions.retractLengthBeforeWipe);
        gcode += m_wipe.wipe(*this, wipeRetractions.retractLengthDuringWipe, toolchange, is_last_retraction);
    }

    /*  The parent class will decide whether we need to perform an actual retraction
        (the extruder might be already retracted fully or partially). We call these
        methods even if we performed wipe, since this will ensure the entire retraction
        length is honored in case wipe path was too short.  */

    // // Snapmaker U1
    // std::string printer_model = this->m_curr_print->m_config.printer_model.value;
    // if (printer_model == "Snapmaker U1" && toolchange) {
    //     gcode += "M400\n";
    // }
    if ((!this->on_first_layer() || this->config().bottom_surface_pattern != InfillPattern::ipHilbertCurve) &&
        (role != erTopSolidInfill || this->config().top_surface_pattern != InfillPattern::ipHilbertCurve)) {
        gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();
    }

    gcode += m_writer.reset_e();
    // Orca: check if should + can lift (roughly from SuperSlicer)
    RetractLiftEnforceType retract_lift_type = RetractLiftEnforceType(EXTRUDER_CONFIG(retract_lift_enforce));

    bool needs_lift = toolchange || m_writer.extruder()->retraction_length() > 0 || m_config.use_firmware_retraction;

    bool last_fill_extrusion_role_top_infill = (this->m_last_notgapfill_extrusion_role == ExtrusionRole::erTopSolidInfill ||
                                                this->m_last_notgapfill_extrusion_role == ExtrusionRole::erIroning);

    // assume we can lift on retraction; conditions left explicit
    bool can_lift = true;

    if (retract_lift_type == RetractLiftEnforceType::rletAllSurfaces) {
        can_lift = true;
    } else if (this->m_layer_index == 0 && (retract_lift_type == RetractLiftEnforceType::rletBottomOnly ||
                                            retract_lift_type == RetractLiftEnforceType::rletTopAndBottom)) {
        can_lift = true;
    } else if (retract_lift_type == RetractLiftEnforceType::rletTopOnly || retract_lift_type == RetractLiftEnforceType::rletTopAndBottom) {
        can_lift = last_fill_extrusion_role_top_infill;
    } else {
        can_lift = false;
    }

    if (needs_lift && can_lift) {
        gcode += m_writer.lift(lift_type, m_spiral_vase != nullptr);
    }

    return gcode;
}

// Ultra (H2C 3MF schema): append one entry to the filament / logical-nozzle order. Mirrors what
// BambuStudio pushes into m_filament_change_sequence / m_nozzle_change_sequence on every filament
// change. The nozzle id comes from the plate's grouping result; 0 when the machine has none.
void GCode::record_filament_change(unsigned int filament_id)
{
    int nozzle_id = 0;
    if (m_curr_print) {
        if (auto group_result = m_curr_print->get_layered_nozzle_group_result()) {
            auto nozzle = group_result->get_first_nozzle_for_filament(int(filament_id));
            if (nozzle)
                nozzle_id = nozzle->group_id;
        }
    }
    m_filament_change_sequence.emplace_back(filament_id);
    m_nozzle_change_sequence.emplace_back(static_cast<unsigned int>(std::max(0, nozzle_id)));
}

bool GCode::is_bbl_multi_extruder() const
{
    return m_curr_print != nullptr && m_curr_print->is_BBL_printer() && m_config.nozzle_diameter.size() >= 2;
}

int GCode::logical_extruder_for_filament(int filament_id) const
{
    if (filament_id < 0 || !is_bbl_multi_extruder())
        return -1;
    // filament_map is 1-based. On a by-layer plate the grouping writes its result into the print
    // config (ToolOrdering::reorder_extruders_for_minimum_flush_volume); on a print-by-object plate
    // there is no grouping and filament_map is the plate's own (manual) map.
    const std::vector<int> &fm = m_config.filament_map.values;
    if (size_t(filament_id) < fm.size() && fm[size_t(filament_id)] >= 1 && size_t(fm[size_t(filament_id)]) <= m_config.nozzle_diameter.size())
        return fm[size_t(filament_id)] - 1;
    return -1;
}

int GCode::nozzle_key_for_filament(int filament_id) const
{
    const int extruder = logical_extruder_for_filament(filament_id);
    if (extruder < 0)
        return -1;
    // A rack (H2C) has several nozzles on one extruder: key on the logical nozzle id there.
    if (has_nozzle_rack(m_config) && m_curr_print != nullptr)
        if (auto group_result = m_curr_print->get_layered_nozzle_group_result()) {
            const int nozzle = group_result->get_nozzle_id(filament_id, m_layer_index);
            if (nozzle >= 0)
                return 1000 + nozzle;
        }
    return extruder;
}

int GCode::temperature_tool_for_filament(int filament_id) const
{
    const int extruder = logical_extruder_for_filament(filament_id);
    if (extruder < 0)
        return filament_id;
    const std::vector<int> &pem = m_config.physical_extruder_map.values;
    return size_t(extruder) < pem.size() ? pem[size_t(extruder)] : extruder;
}

void GCode::note_filament_loaded(int filament_id)
{
    const int key = nozzle_key_for_filament(filament_id);
    if (key >= 0)
        m_filament_in_nozzle[key] = filament_id;
}

bool GCode::cross_extruder_flush_volume(int old_filament_id, int new_filament_id, float &volume) const
{
    const int old_extruder = logical_extruder_for_filament(old_filament_id);
    const int new_extruder = logical_extruder_for_filament(new_filament_id);
    if (old_extruder < 0 || new_extruder < 0 || old_extruder == new_extruder)
        return false;
    volume = 0.f;
    auto it = m_filament_in_nozzle.find(nozzle_key_for_filament(new_filament_id));
    if (it == m_filament_in_nozzle.end() || it->second < 0 || it->second == new_filament_id)
        return true; // empty nozzle, or it already holds the new filament
    const std::vector<double> &matrix = m_config.flush_volumes_matrix.values;
    const size_t               n      = size_t(std::sqrt(double(matrix.size())) + EPSILON);
    const size_t               idx    = size_t(it->second) * n + size_t(new_filament_id);
    if (n > 0 && size_t(it->second) < n && size_t(new_filament_id) < n && idx < matrix.size())
        volume = float(matrix[idx] * m_config.flush_multiplier.value);
    return true;
}

std::string GCode::set_extruder(unsigned int extruder_id, double print_z, bool by_object)
{
    if (!m_writer.need_toolchange(extruder_id))
        return "";

    // if we are running a single-extruder setup, just set the extruder and return nothing
    if (!m_writer.multiple_extruders) {
        this->placeholder_parser().set("current_extruder", extruder_id);
        // BBS: keep the extruder-change retraction placeholders in step with the active filament.
        set_ec_retraction_placeholders(this->placeholder_parser(), m_config, size_t(extruder_id));

        std::string gcode;
        // Append the filament start G-code.
        const std::string& filament_start_gcode = m_config.filament_start_gcode.get_at(extruder_id);
        if (!filament_start_gcode.empty()) {
            // Process the filament_start_gcode for the filament.
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
            config.set_key_value("retraction_distance_when_cut",
                                 new ConfigOptionFloat(m_config.retraction_distances_when_cut.get_at(extruder_id)));
            config.set_key_value("long_retraction_when_cut", new ConfigOptionBool(m_config.long_retractions_when_cut.get_at(extruder_id)));

            gcode += this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, extruder_id, &config);
            check_add_eol(gcode);
        }
        if (get_value_at(m_config, m_config.enable_pressure_advance, ConfigFlowDomain::Filament, extruder_id)) {
            gcode += m_writer.set_pressure_advance(get_value_at(m_config, m_config.pressure_advance, ConfigFlowDomain::Filament, extruder_id));
            // Orca: Adaptive PA
            // Reset Adaptive PA processor last PA value
            m_pa_processor->resetPreviousPA(get_value_at(m_config, m_config.pressure_advance, ConfigFlowDomain::Filament, extruder_id));
        }

        gcode += m_writer.toolchange(extruder_id);
        return gcode;
    }

    // Bambu two-extruder printers: the start G-code has already loaded the first filament
    // ("M620 S<f>A ... T<f> H.. ... M621 S<f>A"). Bambu Studio adopts it (GCodeWriter::init_extruder,
    // GCode.cpp:2996) and never runs change_filament_gcode for it. Running it again after the prime
    // line told the firmware the hotend held filament 0 ("M620.11 ... I0", old temperature
    // "M620.10 A0 ... P0") whatever the start G-code had loaded, right before the first layer.
    if (m_writer.extruder() == nullptr && m_start_gcode_filament >= 0 && (unsigned int) m_start_gcode_filament == extruder_id &&
        is_bbl_multi_extruder()) {
        m_start_gcode_filament = -1;
        // Bookkeeping only: the start G-code sent the T command itself.
        m_writer.toolchange(extruder_id);
        note_filament_loaded(int(extruder_id));
        // _do_export already recorded it next to ";VT" when the plate has a grouping result.
        if (m_filament_change_sequence.empty())
            this->record_filament_change(extruder_id);
        this->placeholder_parser().set("current_extruder", extruder_id);
        this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(extruder_id));
        this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(extruder_id));
        set_ec_retraction_placeholders(this->placeholder_parser(), m_config, size_t(extruder_id));

        std::string gcode;
        const std::string& filament_start_gcode = m_config.filament_start_gcode.get_at(extruder_id);
        if (!filament_start_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
            gcode += this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, extruder_id, &config);
            check_add_eol(gcode);
        }
        if (get_value_at(m_config, m_config.enable_pressure_advance, ConfigFlowDomain::Filament, extruder_id))
            gcode += m_writer.set_pressure_advance(get_value_at(m_config, m_config.pressure_advance, ConfigFlowDomain::Filament, extruder_id));
        m_last_pos_defined = false;
        return gcode;
    }

    // BBS. Should be placed before retract.
    m_toolchange_count++;
    // Ultra (H2C 3MF schema): record this filament entry for filament_sequence.json.
    this->record_filament_change(extruder_id);

    // prepend retraction on the current extruder
    std::string gcode = this->retract(true, false);

    // Always reset the extrusion path, even if the tool change retract is set to zero.
    m_wipe.reset_path();

    // BBS: insert skip object label before change filament while by object
    if (by_object)
        m_writer.add_object_change_labels(gcode);

    if (m_writer.extruder() != nullptr) {
        // Process the custom filament_end_gcode. set_extruder() is only called if there is no wipe tower
        // so it should not be injected twice.
        unsigned int       old_extruder_id    = m_writer.extruder()->id();
        const std::string& filament_end_gcode = m_config.filament_end_gcode.get_at(old_extruder_id);
        if (!filament_end_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(m_writer.get_position().z() - m_config.z_offset.value));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(old_extruder_id)));
            gcode += placeholder_parser_process("filament_end_gcode", filament_end_gcode, old_extruder_id, &config);
            check_add_eol(gcode);
        }
    }

    // If ooze prevention is enabled, park current extruder in the nearest
    // standby point and set it to the standby temperature.
    if (m_ooze_prevention.enable && m_writer.extruder() != nullptr)
        gcode += m_ooze_prevention.pre_toolchange(*this);

    // BBS
    float new_retract_length            = m_config.retraction_length.get_at(extruder_id);
    float new_retract_length_toolchange = m_config.retract_length_toolchange.get_at(extruder_id);
    int   new_filament_temp             = this->on_first_layer() ? get_value_at(m_config, m_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, extruder_id) :
                                                                   get_value_at(m_config, m_config.nozzle_temperature, ConfigFlowDomain::Filament, extruder_id);
    // BBS: if print_z == 0 use first layer temperature
    if (abs(print_z) < EPSILON)
        new_filament_temp = get_value_at(m_config, m_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, extruder_id);

    Vec3d nozzle_pos = m_writer.get_position();
    float old_retract_length, old_retract_length_toolchange, wipe_volume;
    int   old_filament_temp, old_filament_e_feedrate;

    float filament_area = float((M_PI / 4.f) * pow(m_config.filament_diameter.get_at(extruder_id), 2));
    // BBS: add handling for filament change in start gcode
    int previous_extruder_id = -1;
    if (m_writer.extruder() != nullptr || m_start_gcode_filament != -1) {
        std::vector<float> flush_matrix(cast<float>(m_config.flush_volumes_matrix.values));
        const unsigned int number_of_extruders = (unsigned int) (sqrt(flush_matrix.size()) + EPSILON);
        if (m_writer.extruder() != nullptr)
            assert(m_writer.extruder()->id() < number_of_extruders);
        else
            assert(m_start_gcode_filament < number_of_extruders);

        previous_extruder_id          = m_writer.extruder() != nullptr ? m_writer.extruder()->id() : m_start_gcode_filament;
        old_retract_length            = m_config.retraction_length.get_at(previous_extruder_id);
        old_retract_length_toolchange = m_config.retract_length_toolchange.get_at(previous_extruder_id);
        old_filament_temp             = this->on_first_layer() ? get_value_at(m_config, m_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, previous_extruder_id) :
                                                                 get_value_at(m_config, m_config.nozzle_temperature, ConfigFlowDomain::Filament, previous_extruder_id);
        // Orca: always calculate wipe volume and hence provide correct flush_length, so that MMU devices with cutter and purge bin (e.g.
        // ERCF_v2 with a filament cutter or Filametrix can take advantage of it)
        // Orca #15289: bounds-check flush_volumes_matrix — partial/legacy configs can be shorter than filament count.
        size_t flush_idx = size_t(previous_extruder_id) * number_of_extruders + extruder_id;
        wipe_volume      = flush_idx < flush_matrix.size() ? flush_matrix[flush_idx] : 0.f;
        wipe_volume *= m_config.flush_multiplier;
        // A change onto the other extruder of a Bambu two-extruder printer purges what that nozzle
        // still holds (Bambu Studio GCode.cpp:8180-8196): nothing when it is empty or already holds
        // this filament, else the flush from the filament parked in it. Zeroing every cross-extruder
        // change (the old "Phase 7" rule) told the firmware "M620.10 ... L0" when it went back to a
        // nozzle loaded with a different colour.
        float cross_extruder_volume = 0.f;
        if (cross_extruder_flush_volume(previous_extruder_id, int(extruder_id), cross_extruder_volume)) {
            wipe_volume = cross_extruder_volume;
        } else {
            // Ultra (Phase 7): other machines with a filament_map: a nozzle switch is not a colour purge.
            const auto& fm = m_config.filament_map.values;
            if (previous_extruder_id >= 0 && (size_t)previous_extruder_id < fm.size() && (size_t)extruder_id < fm.size()
                && fm[previous_extruder_id] != fm[extruder_id])
                wipe_volume = 0.f;
        }

        old_filament_e_feedrate = (int) (60.0 * get_value_at(m_config, m_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, previous_extruder_id) / filament_area);
        old_filament_e_feedrate = old_filament_e_feedrate == 0 ? 100 : old_filament_e_feedrate;
        // BBS: must clean m_start_gcode_filament
        m_start_gcode_filament = -1;
    } else {
        old_retract_length            = 0.f;
        old_retract_length_toolchange = 0.f;
        old_filament_temp             = 0;
        wipe_volume                   = 0.f;
        old_filament_e_feedrate       = 200;
    }
    float wipe_length             = wipe_volume / filament_area;
    int   new_filament_e_feedrate = (int) (60.0 * get_value_at(m_config, m_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, extruder_id) / filament_area);
    new_filament_e_feedrate       = new_filament_e_feedrate == 0 ? 100 : new_filament_e_feedrate;

    DynamicConfig dyn_config;
    dyn_config.set_key_value("previous_extruder", new ConfigOptionInt(previous_extruder_id));
    dyn_config.set_key_value("next_extruder", new ConfigOptionInt((int) extruder_id));
    // Ultra: single-mapped dual-nozzle change_filament vars (filament id == extruder id; hotend -1).
    dyn_config.set_key_value("current_filament_id", new ConfigOptionInt(std::max(0, previous_extruder_id)));
    dyn_config.set_key_value("next_filament_id", new ConfigOptionInt((int) extruder_id));
    dyn_config.set_key_value("current_hotend", new ConfigOptionInt(-1));
    dyn_config.set_key_value("next_hotend", new ConfigOptionInt(-1));
    // Ultra (H2C rack): port of BambuStudio GCode.cpp:8227-8228 - the same real logical-nozzle
    // ids the wipe-tower path sets, for the toolchange path taken when the prime tower is off.
    auto group_result_se = (has_nozzle_rack(m_config) && m_curr_print) ? m_curr_print->get_layered_nozzle_group_result() : nullptr;
    if (auto group_result = group_result_se) {
        dyn_config.set_key_value("current_nozzle_id", new ConfigOptionInt(std::max(0, group_result->get_nozzle_id(std::max(0, previous_extruder_id), m_layer_index))));
        dyn_config.set_key_value("next_nozzle_id", new ConfigOptionInt(std::max(0, group_result->get_nozzle_id((int) extruder_id, m_layer_index))));
        dyn_config.set_key_value("nozzle_diameter_at_nozzle_id", new ConfigOptionFloats(get_nozzle_diameters_by_nozzle_id(group_result.get())));
        dyn_config.set_key_value("nozzle_volume_types", new ConfigOptionStrings(get_nozzle_volume_types_by_nozzle_id(group_result.get())));
    }
    dyn_config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
    dyn_config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
    dyn_config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
    dyn_config.set_key_value("relative_e_axis", new ConfigOptionBool(m_config.use_relative_e_distances));
    dyn_config.set_key_value("toolchange_count", new ConfigOptionInt((int) m_toolchange_count));
    // BBS: fan speed is useless placeholer now, but we don't remove it to avoid
    // slicing error in old change_filament_gcode in old 3MF
    dyn_config.set_key_value("fan_speed", new ConfigOptionInt((int) 0));
    dyn_config.set_key_value("old_retract_length", new ConfigOptionFloat(old_retract_length));
    dyn_config.set_key_value("new_retract_length", new ConfigOptionFloat(new_retract_length));
    // BBS (H2C rack): BambuStudio GCode.cpp:8170/8208/8247 - outgoing filament's value, 0 with none loaded.
    dyn_config.set_key_value("filament_retract_length_nc",
                             new ConfigOptionFloat(previous_extruder_id >= 0 ? outgoing_filament_retract_length_nc(m_config, previous_extruder_id) : 0.));
    dyn_config.set_key_value("old_retract_length_toolchange", new ConfigOptionFloat(old_retract_length_toolchange));
    dyn_config.set_key_value("new_retract_length_toolchange", new ConfigOptionFloat(new_retract_length_toolchange));
    dyn_config.set_key_value("old_filament_temp", new ConfigOptionInt(old_filament_temp));
    dyn_config.set_key_value("new_filament_temp", new ConfigOptionInt(new_filament_temp));
    dyn_config.set_key_value("x_after_toolchange", new ConfigOptionFloat(nozzle_pos(0)));
    dyn_config.set_key_value("y_after_toolchange", new ConfigOptionFloat(nozzle_pos(1)));
    dyn_config.set_key_value("z_after_toolchange", new ConfigOptionFloat(nozzle_pos(2)));
    dyn_config.set_key_value("first_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("second_flush_volume", new ConfigOptionFloat(wipe_length / 2.f));
    dyn_config.set_key_value("old_filament_e_feedrate", new ConfigOptionInt(old_filament_e_feedrate));
    dyn_config.set_key_value("new_filament_e_feedrate", new ConfigOptionInt(new_filament_e_feedrate));
    dyn_config.set_key_value("travel_point_1_x", new ConfigOptionFloat(float(travel_point_1.x())));
    dyn_config.set_key_value("travel_point_1_y", new ConfigOptionFloat(float(travel_point_1.y())));
    dyn_config.set_key_value("travel_point_2_x", new ConfigOptionFloat(float(travel_point_2.x())));
    dyn_config.set_key_value("travel_point_2_y", new ConfigOptionFloat(float(travel_point_2.y())));
    dyn_config.set_key_value("travel_point_3_x", new ConfigOptionFloat(float(travel_point_3.x())));
    dyn_config.set_key_value("travel_point_3_y", new ConfigOptionFloat(float(travel_point_3.y())));

    dyn_config.set_key_value("flush_length", new ConfigOptionFloat(wipe_length));

    int   flush_count = std::min(g_max_flush_count, (int) std::round(wipe_volume / g_purge_volume_one_time));
    float flush_unit  = wipe_length / flush_count;
    int   flush_idx   = 0;
    for (; flush_idx < flush_count; flush_idx++) {
        char key_value[64] = {0};
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(flush_unit));
    }

    for (; flush_idx < g_max_flush_count; flush_idx++) {
        char key_value[64] = {0};
        snprintf(key_value, sizeof(key_value), "flush_length_%d", flush_idx + 1);
        dyn_config.set_key_value(key_value, new ConfigOptionFloat(0.f));
    }

    // For Snapmaker Artisian
    dyn_config.set_key_value("next_wipe_x", new ConfigOptionFloat(m_next_wipe_x));
    dyn_config.set_key_value("next_wipe_y", new ConfigOptionFloat(m_next_wipe_y));

    // Process the custom change_filament_gcode.
    const std::string& change_filament_gcode = m_config.change_filament_gcode.value;
    std::string        toolchange_gcode_parsed;
    // Orca: Ignore change_filament_gcode if is the first call for a tool change and manual_filament_change is enabled
    if (!change_filament_gcode.empty() && !(m_config.manual_filament_change.value && m_toolchange_count == 1)) {
        dyn_config.set_key_value("toolchange_z", new ConfigOptionFloat(print_z));

        // Ultra: per-filament flush vectors for BBS 2.x change_filament templates (single-nozzle).
        {
            const auto* ft_opt = m_config.option<ConfigOptionInts>("filament_flush_temp");
            const auto* vs_opt = m_config.option<ConfigOptionFloats>("filament_flush_volumetric_speed");
            const size_t nf = m_config.filament_max_volumetric_speed.size();
            std::vector<int> fts; std::vector<double> vss;
            for (size_t i = 0; i < nf; ++i) {
                double vs = (vs_opt && i < vs_opt->size()) ? vs_opt->get_at(int(i)) : 0.;
                if (vs == 0.) vs = get_value_at(m_config, m_config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, int(i));
                vss.push_back(vs);
                int ft = (ft_opt && i < ft_opt->size()) ? ft_opt->get_at(int(i)) : 0;
                if (ft == 0) ft = m_config.nozzle_temperature_range_high.get_at(int(i));
                fts.push_back(ft);
            }
            dyn_config.set_key_value("flush_volumetric_speeds", new ConfigOptionFloats(vss));
            dyn_config.set_key_value("flush_temperatures", new ConfigOptionInts(fts));
        }

        // BBS: as above - the _ec pair has to be in the parser before the template reads it.
        set_ec_retraction_placeholders(this->placeholder_parser(), m_config, size_t(extruder_id));
        toolchange_gcode_parsed = placeholder_parser_process("change_filament_gcode", change_filament_gcode, extruder_id, &dyn_config);
        check_add_eol(toolchange_gcode_parsed);
        // FanMover skips spans bracketed by "; custom gcode" ... "; custom gcode end" (prefix match,
        // and it ignores comments shorter than 17 chars). "; custom gcode start" satisfies both.
        // Without these markers a fan-speedup/kickstart pass can split a G1 inside the user's
        // change_filament_gcode and splice a phantom waypoint into a hand-routed toolchange.
        if (!toolchange_gcode_parsed.empty()) {
            gcode += "; custom gcode start\n";
            gcode += toolchange_gcode_parsed;
            if (gcode.back() != '\n')
                gcode += '\n';
            gcode += "; custom gcode end\n";
        }

        // BBS
        {
            // BBS: gcode writer doesn't know where the extruder is and whether fan speed is changed after inserting tool change gcode
            // Set this flag so that normal lift will be used the first time after tool change.
            gcode += ";_FORCE_RESUME_FAN_SPEED\n";
            m_writer.set_current_position_clear(false);
            // BBS: check whether custom gcode changes the z position. Update if changed
            double temp_z_after_tool_change;
            if (GCodeProcessor::get_last_z_from_gcode(toolchange_gcode_parsed, temp_z_after_tool_change)) {
                Vec3d pos = m_writer.get_position();
                pos(2)    = temp_z_after_tool_change;
                m_writer.set_position(pos);
            }
        }
    }

    // BBS. Reset old extruder E-value.
    // Keep retract length because Custom GCode will guarantee retract length be the same as toolchange
    if (m_config.single_extruder_multi_material) {
        m_writer.reset_e();
    }

    // We inform the writer about what is happening, but we may not use the resulting gcode.
    std::string toolchange_command = m_writer.toolchange(extruder_id);
    if (!custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), extruder_id))
        gcode += toolchange_command;
    else {
        // user provided his own toolchange gcode, no need to do anything
    }
    note_filament_loaded(int(extruder_id));

    // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
    if (m_config.single_extruder_multi_material && !m_config.enable_prime_tower) {
        int temp = (m_layer_index <= 0 ? get_value_at(m_config, m_config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, extruder_id) :
                                         get_value_at(m_config, m_config.nozzle_temperature, ConfigFlowDomain::Filament, extruder_id));

        gcode += m_writer.set_temperature(temp, false);
    }

    this->placeholder_parser().set("current_extruder", extruder_id);
    this->placeholder_parser().set("retraction_distance_when_cut", m_config.retraction_distances_when_cut.get_at(extruder_id));
    this->placeholder_parser().set("long_retraction_when_cut", m_config.long_retractions_when_cut.get_at(extruder_id));
    // BBS: re-publish for everything that runs AFTER the toolchange (filament_start_gcode and the
    // rest of the layer). change_filament_gcode itself already saw these values above.
    set_ec_retraction_placeholders(this->placeholder_parser(), m_config, size_t(extruder_id));

    // Append the filament start G-code.
    const std::string& filament_start_gcode = m_config.filament_start_gcode.get_at(extruder_id);
    if (!filament_start_gcode.empty()) {
        // Process the filament_start_gcode for the new filament.
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(this->writer().get_position().z() - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
        gcode += this->placeholder_parser_process("filament_start_gcode", filament_start_gcode, extruder_id, &config);
        check_add_eol(gcode);
    }
    // Set the new extruder to the operating temperature.
    if (m_ooze_prevention.enable)
        gcode += m_ooze_prevention.post_toolchange(*this);

    if (get_value_at(m_config, m_config.enable_pressure_advance, ConfigFlowDomain::Filament, extruder_id)) {
        gcode += m_writer.set_pressure_advance(get_value_at(m_config, m_config.pressure_advance, ConfigFlowDomain::Filament, extruder_id));
    }
    // Orca: tool changer or IDEX's firmware may change Z position, so we set it to unknown/undefined
    m_last_pos_defined = false;

    return gcode;
}

inline std::string polygon_to_string(const Polygon& polygon, Print* print, bool is_print_space = false)
{
    std::ostringstream gcode;
    gcode << "[";
    for (const Point& p : polygon.points) {
        const auto v = is_print_space ? Vec2d(p.x(), p.y()) : print->translate_to_print_space(p);
        gcode << "[" << v.x() << "," << v.y() << "],";
    }
    const auto first_v = is_print_space ? Vec2d(polygon.points.front().x(), polygon.points.front().y()) :
                                          print->translate_to_print_space(polygon.points.front());
    gcode << "[" << first_v.x() << "," << first_v.y() << "]";
    gcode << "]";
    return gcode.str();
}
// Number every object and every instance of it, in print order, once per export.
//
// These counters name the object in the `; printing object <name> id:<N> copy <n>` comments and in
// the firmware exclude-object definitions. They used to be assigned inside set_object_info(), which
// is only called when the exclude-object feature is on AND returns early for BBL printers and for
// flavours without such a feature - so on every other printer the labels reported an uninitialised
// PrintObject::m_id and changed from one run of the same binary to the next. Numbering here, for
// every printer and every flavour, makes the labels a function of the print order alone.
void GCode::assign_object_and_instance_ids(Print& print)
{
    size_t object_id = 0;
    size_t unique_id = 0;
    for (PrintObject* object : print.objects()) {
        object->set_id(object_id++);
        size_t inst_id = 0;
        for (PrintInstance& inst : object->instances()) {
            inst.unique_id = unique_id++;
            inst.id        = inst_id++;
        }
    }
}

// this function iterator PrintObject and assign a seqential id to each object.
// this id is used to generate unique object id for each object.
std::string GCode::set_object_info(Print* print)
{
    const auto gflavor = print->config().gcode_flavor.value;
    if (print->is_BBL_printer() ||
        (gflavor != gcfKlipper && gflavor != gcfMarlinLegacy && gflavor != gcfMarlinFirmware && gflavor != gcfRepRapFirmware))
        return "";
    std::ostringstream gcode;
    // Orca: check if we are in pa calib mode
    if (print->calib_mode() == CalibMode::Calib_PA_Line || print->calib_mode() == CalibMode::Calib_PA_Pattern) {
        BoundingBoxf bbox_bed(print->config().printable_area.values);
        bbox_bed.offset(-25.0);
        Polygon polygon_bed;
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.min.y()));
        polygon_bed.append(Point(bbox_bed.max.x(), bbox_bed.max.y()));
        polygon_bed.append(Point(bbox_bed.min.x(), bbox_bed.max.y()));
        gcode << "EXCLUDE_OBJECT_DEFINE NAME="
              << "Orca-PA-Calibration-Test"
              << " CENTER=" << 0 << "," << 0 << " POLYGON=" << polygon_to_string(polygon_bed, print, true) << "\n";
    } else {
        for (PrintObject* object : print->objects()) {
            for (PrintInstance& inst : object->instances()) {
                auto bbox      = inst.get_bounding_box();
                auto center    = print->translate_to_print_space(Vec2d(bbox.center().x(), bbox.center().y()));
                auto inst_name = get_instance_name(object, inst);
                if (gflavor == gcfKlipper) {
                    gcode << "EXCLUDE_OBJECT_DEFINE NAME=" << inst_name << " CENTER=" << center.x() << "," << center.y()
                          << " POLYGON=" << polygon_to_string(inst.get_convex_hull_2d(), print) << "\n";
                } else if (gflavor == gcfMarlinLegacy || gflavor == gcfMarlinFirmware || gflavor == gcfRepRapFirmware) {
                    gcode << "M486 S" << std::to_string(inst.unique_id);
                    if (gflavor == gcfRepRapFirmware)
                        gcode << " A"
                              << "\"" << inst_name << "\"";
                    else
                        gcode << "\nM486 A" << inst_name;
                    gcode << "\nM486 S-1\n";
                }
            }
        }
    }

    return gcode.str();
}

// convert a model-space scaled point into G-code coordinates
Vec2d GCode::point_to_gcode(const Point& point) const
{
    Vec2d extruder_offset = EXTRUDER_CONFIG(extruder_offset);
    return unscale(point) + m_origin - extruder_offset;
}

// convert a model-space scaled point into G-code coordinates
Point GCode::gcode_to_point(const Vec2d& point) const
{
    Vec2d pt = point - m_origin;
    if (const Extruder* extruder = m_writer.extruder(); extruder)
        // This function may be called at the very start from toolchange G-code when the extruder is not assigned yet.
        pt += m_config.extruder_offset.get_at(extruder->id());
    return scaled<coord_t>(pt);
}

Vec2d GCode::point_to_gcode_quantized(const Point& point) const
{
    Vec2d p = this->point_to_gcode(point);
    return {GCodeFormatter::quantize_xyzf(p.x()), GCodeFormatter::quantize_xyzf(p.y())};
}

// Goes through by_region std::vector and returns reference to a subvector of entities, that are to be printed
// during infill/perimeter wiping, or normally (depends on wiping_entities parameter)
// Fills in by_region_per_copy_cache and returns its reference.
const std::vector<GCode::ObjectByExtruder::Island::Region>& GCode::ObjectByExtruder::Island::by_region_per_copy(
    std::vector<Region>& by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities) const
{
    bool has_overrides = false;
    for (const auto& reg : by_region)
        if (!reg.infills_overrides.empty() || !reg.perimeters_overrides.empty()) {
            has_overrides = true;
            break;
        }

    // Data is cleared, but the memory is not.
    by_region_per_copy_cache.clear();

    if (!has_overrides)
        // Simple case. No need to copy the regions.
        return wiping_entities ? by_region_per_copy_cache : this->by_region;

    // Complex case. Some of the extrusions of some object instances are to be printed first - those are the wiping extrusions.
    // Some of the extrusions of some object instances are printed later - those are the clean print extrusions.
    // Filter out the extrusions based on the infill_overrides / perimeter_overrides:

    for (const auto& reg : by_region) {
        by_region_per_copy_cache.emplace_back(); // creates a region in the newly created Island

        // Now we are going to iterate through perimeters and infills and pick ones that are supposed to be printed
        // References are used so that we don't have to repeat the same code
        for (int iter = 0; iter < 2; ++iter) {
            const ExtrusionEntitiesPtr& entities = (iter ? reg.infills : reg.perimeters);
            ExtrusionEntitiesPtr& target_eec = (iter ? by_region_per_copy_cache.back().infills : by_region_per_copy_cache.back().perimeters);
            const std::vector<const WipingExtrusions::ExtruderPerCopy*>& overrides = (iter ? reg.infills_overrides :
                                                                                             reg.perimeters_overrides);

            // Now the most important thing - which extrusion should we print.
            // See function ToolOrdering::get_extruder_overrides for details about the negative numbers hack.
            if (wiping_entities) {
                // Apply overrides for this region.
                for (unsigned int i = 0; i < overrides.size(); ++i) {
                    const WipingExtrusions::ExtruderPerCopy* this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which overrides the default one.
                    if (this_override != nullptr && (*this_override)[copy] == int(extruder))
                        target_eec.emplace_back(entities[i]);
                }
            } else {
                // Apply normal extrusions (non-overrides) for this region.
                unsigned int i = 0;
                for (; i < overrides.size(); ++i) {
                    const WipingExtrusions::ExtruderPerCopy* this_override = overrides[i];
                    // This copy (aka object instance) should be printed with this extruder, which shall be equal to the default one.
                    if (this_override == nullptr || (*this_override)[copy] == -int(extruder) - 1)
                        target_eec.emplace_back(entities[i]);
                }
                for (; i < entities.size(); ++i)
                    target_eec.emplace_back(entities[i]);
            }
        }
    }
    return by_region_per_copy_cache;
}

// This function takes the eec and appends its entities to either perimeters or infills of this Region (depending on the first parameter)
// It also saves pointer to ExtruderPerCopy struct (for each entity), that holds information about which extruders should be used for which copy.
void GCode::ObjectByExtruder::Island::Region::append(const Type                               type,
                                                     const ExtrusionEntityCollection*         eec,
                                                     const WipingExtrusions::ExtruderPerCopy* copies_extruder)
{
    // We are going to manipulate either perimeters or infills, exactly in the same way. Let's create pointers to the proper structure to
    // not repeat ourselves:
    ExtrusionEntitiesPtr*                                  perimeters_or_infills;
    std::vector<const WipingExtrusions::ExtruderPerCopy*>* perimeters_or_infills_overrides;

    switch (type) {
    case PERIMETERS:
        perimeters_or_infills           = &perimeters;
        perimeters_or_infills_overrides = &perimeters_overrides;
        break;
    case INFILL:
        perimeters_or_infills           = &infills;
        perimeters_or_infills_overrides = &infills_overrides;
        break;
    default: throw Slic3r::InvalidArgument("Unknown parameter!");
    }

    // First we append the entities, there are eec->entities.size() of them:
    size_t old_size = perimeters_or_infills->size();
    size_t new_size = old_size + (eec->can_sort() ? eec->entities.size() : 1);
    perimeters_or_infills->reserve(new_size);
    if (eec->can_sort()) {
        for (auto* ee : eec->entities)
            perimeters_or_infills->emplace_back(ee);
    } else
        perimeters_or_infills->emplace_back(const_cast<ExtrusionEntityCollection*>(eec));

    if (copies_extruder != nullptr) {
        // Don't reallocate overrides if not needed.
        // Missing overrides are implicitely considered non-overridden.
        perimeters_or_infills_overrides->reserve(new_size);
        perimeters_or_infills_overrides->resize(old_size, nullptr);
        perimeters_or_infills_overrides->resize(new_size, copies_extruder);
    }
}

// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for
// a single object, or for possibly multiple objects with multiple instances.

} // namespace Slic3r
