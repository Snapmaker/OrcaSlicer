#include "WipeTower.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <vector>
#include <numeric>
#include <sstream>
#include <iomanip>

#include "GCodeProcessor.hpp"
#include "GCodeWriter.hpp"
#include "BoundingBox.hpp"
#include "Circle.hpp"
#include "ClipperUtils.hpp"
#include "LocalesUtils.hpp"
#include "PrintConfig.hpp"
#include "Triangulation.hpp"


namespace Slic3r
{

bool wipe_tower_sparse_layers_skipped(const PrintConfig &config)
{
    return config.wipe_tower_no_sparse_layers.value && config.timelapse_type.value != TimelapseType::tlSmooth;
}

bool wipe_tower_layer_is_sparse(const std::vector<WipeTower::ToolChangeResult> &layer_tool_changes)
{
    return layer_tool_changes.size() == 1 && layer_tool_changes.front().initial_tool == layer_tool_changes.front().new_tool;
}

std::vector<float> compute_compacted_wipe_tower_z(const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
                                                  float base_z)
{
    std::vector<float> tower_z(tool_changes.size(), base_z);
    float              last = base_z;
    for (size_t i = 0; i < tool_changes.size(); ++i) {
        if (! tool_changes[i].empty() && ! wipe_tower_layer_is_sparse(tool_changes[i]))
            last += tool_changes[i].front().layer_height;
        tower_z[i] = last;
    }
    return tower_z;
}

static const double wipe_tower_wall_infill_overlap = 0.0;
// Rib wall path resolution and fillet sampling, as in Bambu Studio's WipeTower.cpp.
static constexpr double WIPE_TOWER_RESOLUTION         = 0.1;
static const double     SCALED_WIPE_TOWER_RESOLUTION  = WIPE_TOWER_RESOLUTION / SCALING_FACTOR;
static const double     WT_SIMPLIFY_TOLERANCE_SCALED  = 0.001 / SCALING_FACTOR;
static constexpr int    arc_fit_size                  = 20;

// Bambu Studio WipeTower.cpp:282 generate_rectange(): the band of half-width `offset` around a line.
static Polygon generate_rectange(const Line &line, coord_t offset)
{
    Point p1 = line.a;
    Point p2 = line.b;

    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();

    double length = std::sqrt(dx * dx + dy * dy);

    double ux = dx / length;
    double uy = dy / length;

    double vx = -uy;
    double vy = ux;

    double ox = vx * offset;
    double oy = vy * offset;

    Points rect;
    rect.resize(4);
    rect[0] = {p1.x() + ox, p1.y() + oy};
    rect[1] = {p1.x() - ox, p1.y() - oy};
    rect[2] = {p2.x() - ox, p2.y() - oy};
    rect[3] = {p2.x() + ox, p2.y() + oy};
    Polygon poly(rect);
    return poly;
}

// Bambu Studio WipeTower.cpp:633 generate_rectange_polygon().
static Polygon generate_rectange_polygon(const Vec2f &wt_box_min, const Vec2f &wt_box_max)
{
    Polygon res;
    res.points.push_back(scaled(wt_box_min));
    res.points.push_back(scaled(Vec2f{wt_box_max[0], wt_box_min[1]}));
    res.points.push_back(scaled(wt_box_max));
    res.points.push_back(scaled(Vec2f{wt_box_min[0], wt_box_max[1]}));
    return res;
}

// One straight or arc piece of a wall path (Bambu Studio WipeTower.cpp:311 Segment).
struct WallSegment
{
    Vec2f      start;
    Vec2f      end;
    bool       is_arc = false;
    ArcSegment arcsegment;
    WallSegment(const Vec2f &s, const Vec2f &e) : start(s), end(e) {}
};

inline float align_round(float value, float base)
{
    return std::round(value / base) * base;
}

inline float align_ceil(float value, float base)
{
    return std::ceil(value / base) * base;
}

inline float align_floor(float value, float base)
{
    return std::floor((value) / base) * base;
}

static bool is_valid_gcode(const std::string &gcode)
{
    int  str_size    = gcode.size();
    int  start_index = 0;
    int  end_index   = 0;
    bool is_valid    = false;
    while (end_index < str_size) {
        if (gcode[end_index] != '\n') {
            end_index++;
            continue;
        }

        if (end_index > start_index) {
            std::string line_str = gcode.substr(start_index, end_index - start_index);
            line_str.erase(0, line_str.find_first_not_of(" "));
            line_str.erase(line_str.find_last_not_of(" ") + 1);
            if (!line_str.empty() && line_str[0] != ';') {
                is_valid = true;
                break;
            }
        }

        start_index = end_index + 1;
        end_index   = start_index;
    }

    return is_valid;
}

class WipeTowerWriter
{
public:
	WipeTowerWriter(float layer_height, float line_width, GCodeFlavor flavor, const std::vector<WipeTower::FilamentParameters>& filament_parameters, bool has_nozzle_rack = false) :
		m_current_pos(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()),
		m_current_z(0.f),
		m_current_feedrate(0.f),
		m_layer_height(layer_height),
		m_extrusion_flow(0.f),
		m_preview_suppressed(false),
		m_elapsed_time(0.f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_default_analyzer_line_width(line_width),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_gcode_flavor(flavor),
        m_filpar(filament_parameters),
        m_has_nozzle_rack(has_nozzle_rack)
        {
            // ORCA: This class is only used by BBL printers, so set the parameter appropriately.
            // This fixes an issue where the wipe tower was using BBL tags resulting in statistics for purging in the purge tower not being displayed.
            GCodeProcessor::s_IsBBLPrinter = true;
            // adds tag for analyzer:
            std::ostringstream str;
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) << std::to_string(m_layer_height) << "\n"; // don't rely on GCodeAnalyzer knowing the layer height - it knows nothing at priming
            str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) << ExtrusionEntity::role_to_string(erWipeTower) << "\n";
            m_gcode += str.str();
            change_analyzer_line_width(line_width);
    }

    WipeTowerWriter& change_analyzer_line_width(float line_width) {
        // adds tag for analyzer:
        std::stringstream str;
        str << ";" << GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) << std::to_string(line_width) << "\n";
        m_gcode += str.str();
        return *this;
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& change_analyzer_mm3_per_mm(float len, float e) {
        static const float area = float(M_PI) * 1.75f * 1.75f / 4.f;
        float mm3_per_mm = (len == 0.f ? 0.f : area * e / len);
        // adds tag for processor:
        std::stringstream str;
        str << ";" << GCodeProcessor::Mm3_Per_Mm_Tag << mm3_per_mm << "\n";
        m_gcode += str.str();
        return *this;
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 set_initial_position(const Vec2f &pos, float width = 0.f, float depth = 0.f, float internal_angle = 0.f) {
        m_wipe_tower_width = width;
        m_wipe_tower_depth = depth;
        m_internal_angle = internal_angle;
		m_start_pos = this->rotate(pos);
		m_current_pos = pos;
		return *this;
	}

    WipeTowerWriter&				 set_initial_tool(size_t tool) { m_current_tool = tool; return *this; }

    // Rib wall: shift of the whole output (Bambu Studio's rib_offset), set before the initial position.
    WipeTowerWriter&            set_origin_offset(const Vec2f &offset) { m_origin_offset = offset; return *this; }
    WipeTowerWriter&            set_arc_fitting(bool enable) { m_enable_arc_fitting = enable; return *this; }

	WipeTowerWriter&				 set_z(float z) 
		{ m_current_z = z; return *this; }

	WipeTowerWriter& 			 set_extrusion_flow(float flow)
		{ m_extrusion_flow = flow; return *this; }

	WipeTowerWriter&				 set_y_shift(float shift) {
        m_current_pos.y() -= shift-m_y_shift;
        m_y_shift = shift;
        return (*this);
    }

    WipeTowerWriter&            disable_linear_advance() {
        if (m_gcode_flavor == gcfKlipper)
            m_gcode += "SET_PRESSURE_ADVANCE ADVANCE=0\n";
        else if (m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += std::string("M572 D") + std::to_string(m_current_tool) + " S0\n";
        else
            m_gcode += "M900 K0\n";

        return *this;
    }

	// Suppress / resume G-code preview in Slic3r. Slic3r will have difficulty to differentiate the various
	// filament loading and cooling moves from normal extrusion moves. Therefore the writer
	// is asked to suppres output of some lines, which look like extrusions.
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    WipeTowerWriter& suppress_preview() { change_analyzer_line_width(0.f); m_preview_suppressed = true; return *this; }
    WipeTowerWriter& resume_preview() { change_analyzer_line_width(m_default_analyzer_line_width); m_preview_suppressed = false; return *this; }
#else
    WipeTowerWriter& 			 suppress_preview() { m_preview_suppressed = true; return *this; }
	WipeTowerWriter& 			 resume_preview()   { m_preview_suppressed = false; return *this; }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

	WipeTowerWriter& 			 feedrate(float f)
	{
        if (f != m_current_feedrate) {
			m_gcode += "G1" + set_format_F(f) + "\n";
            m_current_feedrate = f;
        }
		return *this;
	}

	const std::string&   gcode() const { return m_gcode; }
	const std::vector<WipeTower::Extrusion>& extrusions() const { return m_extrusions; }
	float                x()     const { return m_current_pos.x(); }
	float                y()     const { return m_current_pos.y(); }
	const Vec2f& 		 pos()   const { return m_current_pos; }
	const Vec2f	 		 start_pos_rotated() const { return m_start_pos; }
	const Vec2f  		 pos_rotated() const { return this->rotate(m_current_pos); }
	float 				 elapsed_time() const { return m_elapsed_time; }
    float                get_and_reset_used_filament_length() { float temp = m_used_filament_length; m_used_filament_length = 0.f; return temp; }

	// Extrude with an explicitely provided amount of extrusion.
	WipeTowerWriter& extrude_explicit(float x, float y, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
	{
		if (x == m_current_pos.x() && y == m_current_pos.y() && e == 0.f && (f == 0.f || f == m_current_feedrate))
			// Neither extrusion nor a travel move.
			return *this;

		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        float len = std::sqrt(dx*dx+dy*dy);
        if (record_length)
            m_used_filament_length += e;

		// Now do the "internal rotation" with respect to the wipe tower center
		Vec2f rotated_current_pos(this->pos_rotated());
		Vec2f rot(this->rotate(Vec2f(x,y)));                               // this is where we want to go

        if (! m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
            // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
			// This is left zero if it is a travel move.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
			// Correct for the roundings of a squished extrusion.
			width += m_layer_height * float(1. - M_PI / 4.);
			if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
				m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
			m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
		}

		m_gcode += "G1";
        if (std::abs(rot.x() - rotated_current_pos.x()) > (float)EPSILON)
			m_gcode += set_format_X(rot.x());

        if (std::abs(rot.y() - rotated_current_pos.y()) > (float)EPSILON)
			m_gcode += set_format_Y(rot.y());


		if (e != 0.f)
			m_gcode += set_format_E(e);

		if (f != 0.f && f != m_current_feedrate) {
            if (limit_volumetric_flow) {
                float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
                f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            }
			m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

		// Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter& extrude_explicit(const Vec2f &dest, float e, float f = 0.f, bool record_length = false, bool limit_volumetric_flow = true)
		{ return extrude_explicit(dest.x(), dest.y(), e, f, record_length); }

    // Extrude along an arc with the extrusion amount given by m_extrusion_flow. Bambu Studio
    // WipeTower.cpp:821 extrude_arc_explicit(), minus its acceleration commands.
    WipeTowerWriter& extrude_arc(const ArcSegment &arc, float f = 0.f)
    {
        float x   = (float)unscale(arc.end_point).x();
        float y   = (float)unscale(arc.end_point).y();
        float len = unscaled<float>(arc.length);
        float e   = len * m_extrusion_flow;
        if (len < (float) EPSILON && e == 0.f && (f == 0.f || f == m_current_feedrate))
            // Neither extrusion nor a travel move.
            return *this;
        m_used_filament_length += e;

        // Now do the "internal rotation" with respect to the wipe tower center
        Vec2f rotated_current_pos(this->pos_rotated());
        Vec2f rot(this->rotate(Vec2f(x, y))); // this is where we want to go

        if (!m_preview_suppressed && e > 0.f && len > 0.f) {
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
            change_analyzer_mm3_per_mm(len, e);
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
            // Width of a squished extrusion, corrected for the roundings of the squished extrusions.
            float width = e * m_filpar[0].filament_area / (len * m_layer_height);
            // Correct for the roundings of a squished extrusion.
            width += m_layer_height * float(1. - M_PI / 4.);
            if (m_extrusions.empty() || m_extrusions.back().pos != rotated_current_pos)
                m_extrusions.emplace_back(WipeTower::Extrusion(rotated_current_pos, 0, m_current_tool));
            for (int j = 0; j < arc_fit_size; j++) {
                float cur_angle = arc.polar_start_theta + (float) j / arc_fit_size * arc.angle_radians;
                if (cur_angle > 2 * PI)
                    cur_angle -= 2 * PI;
                else if (cur_angle < 0)
                    cur_angle += 2 * PI;
                Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                m_extrusions.emplace_back(WipeTower::Extrusion(this->rotate(unscaled<float>(tmp)), width, m_current_tool));
            }
            m_extrusions.emplace_back(WipeTower::Extrusion(rot, width, m_current_tool));
        }

        m_gcode += arc.direction == ArcDirection::Arc_Dir_CCW ? "G3" : "G2";
        const Vec2f center_offset = this->rotate(unscaled<float>(arc.center)) - rotated_current_pos;
        m_gcode += set_format_X(rot.x());
        m_gcode += set_format_Y(rot.y());
        m_gcode += " I" + Slic3r::float_to_string_decimal_point(center_offset.x(), 3);
        m_gcode += " J" + Slic3r::float_to_string_decimal_point(center_offset.y(), 3);
        if (e != 0.f)
            m_gcode += set_format_E(e);
        if (f != 0.f && f != m_current_feedrate) {
            float e_speed = e / (((len == 0.f) ? std::abs(e) : len) / f * 60.f);
            f /= std::max(1.f, e_speed / m_filpar[m_current_tool].max_e_speed);
            m_gcode += set_format_F(f);
        }

        m_current_pos.x() = x;
        m_current_pos.y() = y;

        // Update the elapsed time with a rough estimate.
        m_elapsed_time += ((len == 0.f) ? std::abs(e) : len) / m_current_feedrate * 60.f;
        m_gcode += "\n";
        return *this;
    }

    // Extrude a closed wall once around, starting from the vertex closest to the current position.
    // Bambu Studio WipeTower.cpp:1030 polygon() (brim loops, pre_simplify) and :1272 generate_path()
    // (the wall itself); a single closed polygon has no gaps, so generate_path()'s retracts between
    // disjoint pieces never fire and are left out.
    WipeTowerWriter& polygon(const Polygon &wall_polygon, const float f, bool pre_simplify)
    {
        Polyline pl = to_polyline(wall_polygon);
        if (pre_simplify)
            pl.simplify(WT_SIMPLIFY_TOLERANCE_SCALED);
        std::vector<WallSegment> segments;
        if (m_enable_arc_fitting) {
            pl.simplify_by_fitting_arc(SCALED_WIPE_TOWER_RESOLUTION);
            for (const PathFittingData &fit : pl.fitting_result) {
                if (fit.path_type == EMovePathType::Linear_move) {
                    for (size_t j = fit.start_point_index; j < fit.end_point_index; ++j)
                        segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
                } else if (fit.path_type == EMovePathType::Arc_move_ccw || fit.path_type == EMovePathType::Arc_move_cw) {
                    segments.emplace_back(unscaled<float>(pl.points[fit.start_point_index]), unscaled<float>(pl.points[fit.end_point_index]));
                    segments.back().is_arc     = true;
                    segments.back().arcsegment = fit.arc_data;
                }
            }
        } else {
            pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);
            for (size_t j = 0; j + 1 < pl.points.size(); ++j)
                segments.emplace_back(unscaled<float>(pl.points[j]), unscaled<float>(pl.points[j + 1]));
        }
        if (segments.empty())
            return *this;

        int   index_of_closest = 0;
        float min_distance     = std::numeric_limits<float>::max();
        for (int i = 0; i < int(segments.size()); ++i) {
            const float distance = (segments[i].start - m_current_pos).squaredNorm();
            if (distance < min_distance) {
                min_distance     = distance;
                index_of_closest = i;
            }
        }
        int i = index_of_closest;
        travel(segments[i].start); // travel to the closest points
        do {
            segments[i].is_arc ? extrude_arc(segments[i].arcsegment, f) : extrude(segments[i].end, f);
            i = (i + 1) % int(segments.size());
        } while (i != index_of_closest);
        return *this;
    }

	// Travel to a new XY position. f=0 means use the current value.
	WipeTowerWriter& travel(float x, float y, float f = 0.f)
		{ return extrude_explicit(x, y, 0.f, f); }

	WipeTowerWriter& travel(const Vec2f &dest, float f = 0.f) 
		{ return extrude_explicit(dest.x(), dest.y(), 0.f, f); }

	// Extrude a line from current position to x, y with the extrusion amount given by m_extrusion_flow.
	WipeTowerWriter& extrude(float x, float y, float f = 0.f)
	{
		float dx = x - m_current_pos.x();
		float dy = y - m_current_pos.y();
        return extrude_explicit(x, y, std::sqrt(dx*dx+dy*dy) * m_extrusion_flow, f, true);
	}

	WipeTowerWriter& extrude(const Vec2f &dest, const float f = 0.f) 
		{ return extrude(dest.x(), dest.y(), f); }

    WipeTowerWriter& rectangle(const Vec2f& ld,float width,float height,const float f = 0.f)
    {
        Vec2f corners[4];
        corners[0] = ld;
        corners[1] = ld + Vec2f(width,0.f);
        corners[2] = ld + Vec2f(width,height);
        corners[3] = ld + Vec2f(0.f,height);
        int index_of_closest = 0;
        if (x()-ld.x() > ld.x()+width-x())    // closer to the right
            index_of_closest = 1;
        if (y()-ld.y() > ld.y()+height-y())   // closer to the top
            index_of_closest = (index_of_closest==0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y());      // travel to the closest corner
        travel(x(),corners[index_of_closest].y());

        int i = index_of_closest;
        do {
            ++i;
            if (i==4) i=0;
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter &rectangle_fill_box(const WipeTower* wipe_tower, const Vec2f &ld, float width, float height, const float f = 0.f)
    {
        bool need_change_flow = wipe_tower->need_thick_bridge_flow(ld.y());

        Vec2f corners[4];
        corners[0]           = ld;
        corners[1]           = ld + Vec2f(width, 0.f);
        corners[2]           = ld + Vec2f(width, height);
        corners[3]           = ld + Vec2f(0.f, height);
        int index_of_closest = 0;
        if (x() - ld.x() > ld.x() + width - x()) // closer to the right
            index_of_closest = 1;
        if (y() - ld.y() > ld.y() + height - y()) // closer to the top
            index_of_closest = (index_of_closest == 0 ? 3 : 2);

        travel(corners[index_of_closest].x(), y()); // travel to the closest corner
        travel(x(), corners[index_of_closest].y());

        int i = index_of_closest;
        bool flow_changed = false;
        do {
            ++i;
            if (i == 4) i = 0;
            if (need_change_flow) { 
                if (i == 1) {
                    // using bridge flow in bridge area, and add notes for gcode-check when flow changed
                    set_extrusion_flow(wipe_tower->extrusion_flow(0.2));
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
                    flow_changed = true;
                } else if (i == 2 && flow_changed) {
                    set_extrusion_flow(wipe_tower->get_extrusion_flow());
                    append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
                }
            }
            extrude(corners[i], f);
        } while (i != index_of_closest);
        return (*this);
    }

    WipeTowerWriter& rectangle(const WipeTower::box_coordinates& box, const float f = 0.f)
    {
        rectangle(Vec2f(box.ld.x(), box.ld.y()),
                  box.ru.x() - box.lu.x(),
                  box.ru.y() - box.rd.y(), f);
        return (*this);
    }

	WipeTowerWriter& load(float e, float f = 0.f)
	{
		if (e == 0.f && (f == 0.f || f == m_current_feedrate))
			return *this;
		m_gcode += "G1";
		if (e != 0.f)
			m_gcode += set_format_E(e);
		if (f != 0.f && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	WipeTowerWriter& retract(float e, float f = 0.f)
		{ return load(-e, f); }

    // Push e mm of filament through the standing nozzle (tower interface extra prime), counted as used.
    WipeTowerWriter& prime(float e, float f)
    {
        if (e <= 0.f)
            return *this;
        m_used_filament_length += e;
        m_elapsed_time += e / f * 60.f;
        return load(e, f);
    }

    // A wall cut open by tower interface gaps: the pieces in wall order, starting with the one that
    // begins nearest to the current position, travelling over each gap.
    WipeTowerWriter& wall_pieces(const Polylines &pieces, float f)
    {
        std::vector<std::vector<Vec2f>> paths;
        for (Polyline pl : pieces) {
            pl.simplify(SCALED_WIPE_TOWER_RESOLUTION);
            if (pl.points.size() < 2)
                continue;
            std::vector<Vec2f> path;
            for (const Point &pt : pl.points)
                path.push_back(unscaled<float>(pt));
            paths.push_back(std::move(path));
        }
        if (paths.empty())
            return *this;
        size_t first = 0;
        for (size_t i = 1; i < paths.size(); ++i)
            if ((paths[i].front() - m_current_pos).squaredNorm() < (paths[first].front() - m_current_pos).squaredNorm())
                first = i;
        for (size_t k = 0; k < paths.size(); ++k) {
            const std::vector<Vec2f> &path = paths[(first + k) % paths.size()];
            travel(path.front());
            for (size_t i = 1; i < path.size(); ++i)
                extrude(path[i], f);
        }
        return *this;
    }

// Loads filament while also moving towards given points in x-axis (x feedrate is limited by cutting the distance short if necessary)
    WipeTowerWriter& load_move_x_advanced(float farthest_x, float loading_dist, float loading_speed, float max_x_speed = 50.f)
    {
        float time = std::abs(loading_dist / loading_speed); // time that the move must take
        float x_distance = std::abs(farthest_x - x());       // max x-distance that we can travel
        float x_speed = x_distance / time;                   // x-speed to do it in that time

        if (x_speed > max_x_speed) {
            // Necessary x_speed is too high - we must shorten the distance to achieve max_x_speed and still respect the time.
            x_distance = max_x_speed * time;
            x_speed = max_x_speed;
        }

        float end_point = x() + (farthest_x > x() ? 1.f : -1.f) * x_distance;
        return extrude_explicit(end_point, y(), loading_dist, x_speed * 60.f, false, false);
    }

	// Elevate the extruder head above the current print_z position.
	WipeTowerWriter& z_hop(float hop, float f = 0.f)
	{ 
		m_gcode += std::string("G1") + set_format_Z(m_current_z + hop);
		if (f != 0 && f != m_current_feedrate)
			m_gcode += set_format_F(f);
		m_gcode += "\n";
		return *this;
	}

	// Lower the extruder head back to the current print_z position.
	WipeTowerWriter& z_hop_reset(float f = 0.f) 
		{ return z_hop(0, f); }

	// Move to x1, +y_increment,
	// extrude quickly amount e to x2 with feed f.
	WipeTowerWriter& ram(float x1, float x2, float dy, float e0, float e, float f)
	{
		extrude_explicit(x1, m_current_pos.y() + dy, e0, f, true, false);
		extrude_explicit(x2, m_current_pos.y(), e, 0.f, true, false);
		return *this;
	}

	// Let the end of the pulled out filament cool down in the cooling tube
	// by moving up and down and moving the print head left / right
	// at the current Y position to spread the leaking material.
	WipeTowerWriter& cool(float x1, float x2, float e1, float e2, float f)
	{
		extrude_explicit(x1, m_current_pos.y(), e1, f, false, false);
		extrude_explicit(x2, m_current_pos.y(), e2, false, false);
		return *this;
	}

    WipeTowerWriter& set_tool(size_t tool)
	{
		m_current_tool = tool;
		return *this;
	}

	// Set extruder temperature, don't wait by default.
	WipeTowerWriter& set_extruder_temp(int temperature, bool wait = false)
	{
        m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S" + std::to_string(temperature) + "\n";
        return *this;
    }

    // Wait for a period of time (seconds).
	WipeTowerWriter& wait(float time)
	{
        if (time==0.f)
            return *this;
        m_gcode += "G4 S" + Slic3r::float_to_string_decimal_point(time, 3) + "\n";
		return *this;
    }

	// Set speed factor override percentage.
	WipeTowerWriter& speed_override(int speed)
	{
        m_gcode += "M220 S" + std::to_string(speed) + "\n";
		return *this;
    }

	// Let the firmware back up the active speed override value.
	WipeTowerWriter& speed_override_backup()
    {
        // Ultra (H2C rack): BambuStudio WipeTower.cpp:1179-1185 emits this for gcfMarlinLegacy /
        // gcfMarlinFirmware; the gold H2C file carries one "M220 B" per toolchange. Orca had it
        // #if 0'd out ("BBL machine don't support speed backup"), which is wrong for the rack
        // machines. Gated on the rack so P1S/H2D output stays unchanged - see
        // docs/superpowers/specs/2026-09-07-h2c-rack-nozzle-change.md.
        if (m_has_nozzle_rack && (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware))
            m_gcode += "M220 B\n";
		return *this;
    }

	// Let the firmware restore the active speed override value.
	WipeTowerWriter& speed_override_restore()
	{
	    // Ultra (H2C rack): see speed_override_backup above (BambuStudio WipeTower.cpp:1188-1194).
        if (m_has_nozzle_rack && (m_gcode_flavor == gcfMarlinLegacy || m_gcode_flavor == gcfMarlinFirmware))
            m_gcode += "M220 R\n";
		return *this;
    }

	// Set digital trimpot motor
	WipeTowerWriter& set_extruder_trimpot(int current)
	{
        // BBS: don't control trimpot
#if 0
        if (m_gcode_flavor == gcfRepRapSprinter || m_gcode_flavor == gcfRepRapFirmware)
            m_gcode += "M906 E";
        else
            m_gcode += "M907 E";
        m_gcode += std::to_string(current) + "\n";
#endif
		return *this;
    }

	WipeTowerWriter& flush_planner_queue()
	{ 
		m_gcode += "G4 S0\n"; 
		return *this;
	}

	// Reset internal extruder counter.
	WipeTowerWriter& reset_extruder()
	{ 
		m_gcode += "G92 E0\n";
		return *this;
	}

	WipeTowerWriter& comment_with_value(const char *comment, int value)
    {
        m_gcode += std::string(";") + comment + std::to_string(value) + "\n";
		return *this;
    }


    WipeTowerWriter& set_fan(unsigned speed)
	{
		if (speed == m_last_fan_speed)
			return *this;
		if (speed == 0)
			m_gcode += "M107\n";
        else
            m_gcode += "M106 S" + std::to_string(unsigned(255.0 * speed / 100.0)) + "\n";
		m_last_fan_speed = speed;
		return *this;
	}

	WipeTowerWriter& append(const std::string& text) { m_gcode += text; return *this; }

    const std::vector<Vec2f>& wipe_path() const
    {
        return m_wipe_path;
    }

    WipeTowerWriter& add_wipe_point(const Vec2f& pt)
    {
        m_wipe_path.push_back(rotate(pt));
        return *this;
    }

    WipeTowerWriter& add_wipe_point(float x, float y)
    {
        return add_wipe_point(Vec2f(x, y));
    }

    // Wipe back along a wall for wipe_dist (Bambu Studio WipeTower.cpp:1260 add_wipe_path()).
    WipeTowerWriter& add_wipe_path(const Polygon &polygon, double wipe_dist)
    {
        int      closest_idx = polygon.closest_point_index(scaled(m_current_pos));
        Polyline wipe_path   = polygon.split_at_index(closest_idx);
        wipe_path.reverse();
        for (int i = 0; i < int(wipe_path.size()); ++i) {
            if (wipe_dist < EPSILON) break;
            add_wipe_point(unscaled<float>(wipe_path[i]));
            if (i != 0) wipe_dist -= (unscaled(wipe_path[i]) - unscaled(wipe_path[i - 1])).norm();
        }
        return *this;
    }

private:
	Vec2f         m_start_pos;
	Vec2f         m_current_pos;
    std::vector<Vec2f>  m_wipe_path;
	float    	  m_current_z;
	float 	  	  m_current_feedrate;
    size_t        m_current_tool;
	float 		  m_layer_height;
	float 	  	  m_extrusion_flow;
	bool		  m_preview_suppressed;
	std::string   m_gcode;
	std::vector<WipeTower::Extrusion> m_extrusions;
	float         m_elapsed_time;
	float   	  m_internal_angle = 0.f;
	float		  m_y_shift = 0.f;
	float 		  m_wipe_tower_width = 0.f;
	float		  m_wipe_tower_depth = 0.f;
    unsigned      m_last_fan_speed = 0;
    int           current_temp = -1;
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    const float   m_default_analyzer_line_width;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
    float         m_used_filament_length = 0.f;
    GCodeFlavor   m_gcode_flavor;
    bool          m_has_nozzle_rack = false;   // Ultra (H2C rack)
    bool          m_enable_arc_fitting = false;
    Vec2f         m_origin_offset = Vec2f::Zero();
    const std::vector<WipeTower::FilamentParameters>& m_filpar;

	std::string   set_format_X(float x)
    {
        m_current_pos.x() = x;
        return " X" + Slic3r::float_to_string_decimal_point(x, 3);
	}

	std::string   set_format_Y(float y) {
        m_current_pos.y() = y;
        return " Y" + Slic3r::float_to_string_decimal_point(y, 3);
	}

	std::string   set_format_Z(float z) {
        return " Z" + Slic3r::float_to_string_decimal_point(z, 3);
	}

	std::string   set_format_E(float e) {
        return " E" + Slic3r::float_to_string_decimal_point(e, 4);
	}

	std::string   set_format_F(float f) {
        char buf[64];
        sprintf(buf, " F%d", int(floor(f + 0.5f)));
        m_current_feedrate = f;
        return buf;
	}

	WipeTowerWriter& operator=(const WipeTowerWriter &rhs);

	// Rotate the point around center of the wipe tower about given angle (in degrees)
	Vec2f rotate(Vec2f pt) const
	{
		pt.x() -= m_wipe_tower_width / 2.f;
		pt.y() += m_y_shift - m_wipe_tower_depth / 2.f;
	    double angle = m_internal_angle * float(M_PI/180.);
	    double c = cos(angle);
	    double s = sin(angle);
	    Vec2f out(float(pt.x() * c - pt.y() * s) + m_wipe_tower_width / 2.f, float(pt.x() * s + pt.y() * c) + m_wipe_tower_depth / 2.f);
	    // Only a rib wall moves the tower, so every other output stays bit-identical.
	    if (m_origin_offset != Vec2f::Zero())
	        out += m_origin_offset;
	    return out;
	}

}; // class WipeTowerWriter



WipeTower::ToolChangeResult WipeTower::construct_tcr(WipeTowerWriter& writer,
                                                     bool priming,
                                                     size_t old_tool,
                                                     bool is_finish,
                                                     float purge_volume) const
{
    ToolChangeResult result;
    result.priming      = priming;
    result.initial_tool = int(old_tool);
    result.new_tool     = int(m_current_tool);
    result.print_z      = m_z_pos;
    result.layer_height = m_layer_height;
    result.elapsed_time = writer.elapsed_time();
    result.start_pos    = writer.start_pos_rotated();
    result.end_pos      = priming ? writer.pos() : writer.pos_rotated();
    result.gcode        = std::move(writer.gcode());
    result.extrusions   = std::move(writer.extrusions());
    result.wipe_path    = std::move(writer.wipe_path());
    result.is_finish_first = is_finish;
    // BBS
    result.purge_volume = purge_volume;
    return result;
}

// BBS
const std::map<float, float> WipeTower::min_depth_per_height = {
    {100.f, 20.f}, {250.f, 40.f}
};

float WipeTower::get_limit_depth_by_height(float max_height)
{
    float min_wipe_tower_depth = 0.f;
    auto  iter                 = WipeTower::min_depth_per_height.begin();
    while (iter != WipeTower::min_depth_per_height.end()) {
        auto curr_height_to_depth = *iter;

        // This is the case that wipe tower height is lower than the first min_depth_to_height member.
        if (curr_height_to_depth.first >= max_height) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        iter++;

        // If curr_height_to_depth is the last member, use its min_depth.
        if (iter == WipeTower::min_depth_per_height.end()) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        // If wipe tower height is between the current and next member, set the min_depth as linear interpolation between them
        auto next_height_to_depth = *iter;
        if (next_height_to_depth.first > max_height) {
            float height_base    = curr_height_to_depth.first;
            float height_diff    = next_height_to_depth.first - curr_height_to_depth.first;
            float min_depth_base = curr_height_to_depth.second;
            float depth_diff     = next_height_to_depth.second - curr_height_to_depth.second;

            min_wipe_tower_depth = min_depth_base + (max_height - curr_height_to_depth.first) / height_diff * depth_diff;
            break;
        }
    }
    return min_wipe_tower_depth;
}

float WipeTower::get_auto_brim_by_height(float max_height)
{
    if (max_height < 100)
        return max_height / 100.f * 8.f;
    return 8.f;
}

static const double wrapping_wipe_tower_depth = 10;
static const std::map<float, float> nozzle_diameter_to_nozzle_change_width{{0.2f, 0.5f}, {0.4f, 1.0f}, {0.6f, 1.2f}, {0.8f, 1.4f}};

float WipeTower::estimate_brim_real_width(float brim_width, float nozzle_diameter, float first_layer_height, bool type2)
{
    if (brim_width <= 0.f)
        return brim_width;
    const float spacing = nozzle_diameter * 1.25f - first_layer_height * float(1. - M_PI_4); // Width_To_Nozzle_Ratio
    if (spacing <= EPSILON)
        return brim_width;
    const int loops_num = int((brim_width + spacing / 2.f) / spacing);
    return loops_num * spacing + (type2 ? 0.f : spacing / 2.f);
}

float WipeTower::get_wrapping_detection_depth()
{
    return float(wrapping_wipe_tower_depth);
}

float WipeTower::nozzle_change_perimeter_width(float nozzle_diameter)
{
    auto it = nozzle_diameter_to_nozzle_change_width.find(nozzle_diameter);
    return it != nozzle_diameter_to_nozzle_change_width.end() ? it->second : 2.f * nozzle_diameter * 1.25f;
}

float WipeTower::estimate_tower_blocks_depth(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing)
{
    if (purges.empty() || layer_height < EPSILON || nozzle_diameter < EPSILON)
        return 0.f;
    const float pw         = nozzle_diameter * 1.25f; // Width_To_Nozzle_Ratio
    const float ncpw       = nozzle_change_perimeter_width(nozzle_diameter);
    const float line_width = width - 2.f * pw;
    if (line_width <= EPSILON)
        return 0.f;
    // Line cross-section as volume_to_length() sees it; the infill gap stretches the perimeter
    // width by the configured ratio and nozzle-change lines keep their own width
    // (calc_block_infill_gap).
    auto        line_area   = [layer_height](float w) { return layer_height * (w - layer_height * float(1. - M_PI_4)); };
    const float extra_width = (extra_spacing - 1.f) * pw;
    const float gap         = pw + extra_width;
    const float nc_gap      = ncpw + extra_width;
    // A layer purges into at most (filaments - 1) targets, so a category holding every filament
    // never sees its smallest purge (the layer's first filament) in its worst layer.
    struct Block { float depth = 0.f; float min_purge = 0.f; size_t filaments = 0; };
    std::map<int, Block> blocks;
    for (const PurgeEstimate &purge : purges) {
        Block      &block       = blocks[purge.category];
        const float purge_depth = std::ceil(purge.prime_volume / line_area(pw) / line_width) * gap;
        block.min_purge         = block.filaments == 0 ? purge_depth : std::min(block.min_purge, purge_depth);
        block.depth += purge_depth;
        ++block.filaments;
        if (purge.filament_change_length > EPSILON) {
            // The leaving filament is rammed over the nozzle-change flow, again in whole lines.
            const float filament_area = float(M_PI) * purge.filament_diameter * purge.filament_diameter / 4.f;
            const float nc_length     = purge.filament_change_length * filament_area / line_area(ncpw);
            block.depth += std::ceil(nc_length / (width - ncpw - pw)) * nc_gap;
        }
    }
    float depth = pw; // plan_tower_new starts the first block one perimeter width in
    for (const auto &[category, block] : blocks)
        depth += block.filaments == purges.size() ? block.depth - block.min_purge : block.depth;
    return depth;
}

float WipeTower::rib_footprint_side(float width, float depth, float rib_width, float extra_rib_length, float max_height)
{
    if (width < EPSILON || depth < EPSILON)
        return 0.f;
    // Ribs run the diagonal; below the height-based minimum they are extended rather than the
    // body, then by the extra length, never ending up shorter than the diagonal.
    const float diagonal   = std::sqrt(width * width + depth * depth);
    float       rib_length = diagonal;
    if (depth + EPSILON < get_limit_depth_by_height(max_height))
        rib_length = std::max(rib_length, get_limit_depth_by_height(max_height) * float(std::sqrt(2.)));
    rib_length = std::max(diagonal, rib_length + extra_rib_length);
    // Half the extension at each end of the diagonal plus half the rib width, projected onto the axes.
    const float rib_w    = std::min(rib_width, std::min(width, depth) / 2.f);
    const float per_side = ((rib_length - diagonal) / 2.f + rib_w / 2.f) / float(std::sqrt(2.));
    return std::max(width, depth) + 2.f * per_side;
}

float WipeTower::estimate_rib_tower_bbox_side(const std::vector<PurgeEstimate> &purges, float width, float layer_height, float nozzle_diameter, float extra_spacing, float rib_width, float extra_rib_length, float max_height)
{
    if (purges.empty() || width < EPSILON || layer_height < EPSILON || nozzle_diameter < EPSILON)
        return 0.f;
    const float pw     = nozzle_diameter * 1.25f; // Width_To_Nozzle_Ratio
    const float square = align_ceil(std::sqrt(estimate_tower_blocks_depth(purges, width, layer_height, nozzle_diameter, extra_spacing) * width), pw);
    const float depth  = estimate_tower_blocks_depth(purges, square, layer_height, nozzle_diameter, extra_spacing);
    return rib_footprint_side(square, depth, rib_width, extra_rib_length, max_height);
}

TriangleMesh WipeTower::its_make_rib_brim(const Polygon &brim, float layer_height)
{
    TriangleMesh res;
    if (brim.area() < scaled(EPSILON))
        return res;
    int offset = int(brim.size());
    res.its.vertices.reserve(brim.size() * 2);
    auto faces = Triangulation::triangulate(brim);
    res.its.indices.reserve(brim.size() * 2 + 2 * faces.size());
    for (auto &t : faces) res.its.indices.push_back({t[1], t[0], t[2]});
    for (auto &t : faces) res.its.indices.push_back({t[0] + offset, t[1] + offset, t[2] + offset});

    for (int i = 0; i < int(brim.size()); i++)
        res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), 0});
    for (int i = 0; i < int(brim.size()); i++)
        res.its.vertices.push_back({unscaled<float>(brim[i][0]), unscaled<float>(brim[i][1]), layer_height});

    for (int i = 0; i < offset; i++) {
        int a = i;
        int b = (i + 1) % offset;
        int c = i + offset;
        int d = b + offset;
        res.its.indices.push_back({a, b, c});
        res.its.indices.push_back({d, c, b});
    }
    return res;
}

// Bambu Studio WipeTower.cpp:128 rounding_polygon(): replaces every corner sharper than angle_tol
// by a circular arc of arc_fit_size points, tangent to both sides `rounding` mm from the corner.
Polygon WipeTower::rounding_polygon(Polygon &polygon, double rounding /*= 2.*/, double angle_tol /* = 30. / 180. * PI*/)
{
    if (polygon.points.size() < 3) return polygon;
    Polygon res;
    res.points.reserve(polygon.points.size() * 2);
    int    mod           = polygon.points.size();
    double cos_angle_tol = std::abs(std::cos(angle_tol));

    for (int i = 0; i < int(polygon.points.size()); i++) {
        Vec2d  a      = unscaled(polygon.points[(i - 1 + mod) % mod]);
        Vec2d  b      = unscaled(polygon.points[i]);
        Vec2d  c      = unscaled(polygon.points[(i + 1) % mod]);
        double ab_len = (a - b).norm();
        double bc_len = (b - c).norm();
        Vec2d  ab     = (b - a) / ab_len;
        Vec2d  bc     = (c - b) / bc_len;
        assert(ab_len != 0);
        assert(bc_len != 0);
        float cosangle = ab.dot(bc);
        cosangle       = std::clamp(cosangle, -1.f, 1.f);
        bool  is_ccw   = cross2(ab, bc) > 0;
        if (std::abs(cosangle) < cos_angle_tol) {
            float real_rounding_dis = std::min({rounding, ab_len / 2.1, bc_len / 2.1}); // 2.1 to ensure the points do not coincide
            Vec2d left              = b - ab * real_rounding_dis;
            Vec2d right             = b + bc * real_rounding_dis;
            {
                float half_angle = std::acos(cosangle) / 2.f;
                Vec2d dir        = (right - left).normalized();
                dir              = Vec2d{-dir[1], dir[0]};
                dir              = is_ccw ? dir : -dir;
                double dis       = real_rounding_dis / sin(half_angle);

                Vec2d      center = b + dir * dis;
                double     radius = (left - center).norm();
                ArcSegment arc(scaled(center), scaled(radius), scaled(left), scaled(right), is_ccw ? ArcDirection::Arc_Dir_CCW : ArcDirection::Arc_Dir_CW);
                int        n = arc_fit_size;
                for (int j = 0; j < n; j++) {
                    float cur_angle = arc.polar_start_theta + (float) j / n * arc.angle_radians;
                    if (cur_angle > 2 * PI)
                        cur_angle -= 2 * PI;
                    else if (cur_angle < 0)
                        cur_angle += 2 * PI;
                    Point tmp = arc.center + Point{arc.radius * std::cos(cur_angle), arc.radius * std::sin(cur_angle)};
                    res.points.push_back(tmp);
                }
            }
            res.points.push_back(scaled(right));
        } else
            res.points.push_back(polygon.points[i]);
    }
    res.remove_duplicate_points();
    res.points.shrink_to_fit();
    return res;
}

// Bambu Studio WipeTower.cpp:5041 generate_rib_polygon(). The ribs run along the diagonals of the
// whole tower (m_wipe_tower_width x m_wipe_tower_depth), which sits m_y_shift below this layer's
// local origin, and are extended past the corners by a length that shrinks linearly from its
// first-layer value to nothing at the top of the tower.
Polygon WipeTower::generate_rib_polygon(const box_coordinates &wt_box) const
{
    auto    get_current_layer_rib_len = [](float cur_height, float max_height, float max_len) -> float { return std::abs(max_height - cur_height) / max_height * max_len; };
    coord_t diagonal_width            = scaled(m_rib_width) / 2;
    float   a = this->m_wipe_tower_width, b = this->m_wipe_tower_depth;
    Line    line_1(Point::new_scale(Vec2f{0, 0}), Point::new_scale(Vec2f{a, b}));
    Line    line_2(Point::new_scale(Vec2f{a, 0}), Point::new_scale(Vec2f{0, b}));
    float   diagonal_extra_length = std::max(0.f, m_rib_length - (float) unscaled(line_1.length())) / 2.f;
    diagonal_extra_length         = scaled(get_current_layer_rib_len(this->m_z_pos, this->m_wipe_tower_height, diagonal_extra_length));
    Point   y_shift{0, scaled(this->m_y_shift)};

    line_1.extend(double(diagonal_extra_length));
    line_2.extend(double(diagonal_extra_length));
    line_1.translate(-y_shift);
    line_2.translate(-y_shift);

    Polygon poly_1 = generate_rectange(line_1, diagonal_width);
    Polygon poly_2 = generate_rectange(line_2, diagonal_width);
    Polygon poly;
    poly.points.push_back(Point::new_scale(wt_box.ld));
    poly.points.push_back(Point::new_scale(wt_box.rd));
    poly.points.push_back(Point::new_scale(wt_box.ru));
    poly.points.push_back(Point::new_scale(wt_box.lu));

    Polygons p_1_2 = union_({poly_1, poly_2, poly});
    return p_1_2.front();
}

// The wall Bambu Studio's generate_support_wall_new() (WipeTower.cpp:5073) extrudes for a rib
// wall: the rib polygon, filleted and unioned with the box again when fillet_wall is on.
Polygon WipeTower::rib_wall_polygon(const box_coordinates &wt_box) const
{
    Polygon wall_polygon = generate_rib_polygon(wt_box);
    if (m_used_fillet) {
        wall_polygon           = rounding_polygon(wall_polygon);
        Polygon wt_box_polygon = generate_rectange_polygon(wt_box.ld, wt_box.ru);
        wall_polygon           = union_({wall_polygon, wt_box_polygon}).front();
    }
    return wall_polygon;
}

void WipeTower::record_outer_wall(const Polygon &wall)
{
    // To the frame the writer emits in: the layer's y shift, then the rib offset.
    Polyline pl = to_polyline(wall);
    pl.translate(scaled(m_rib_offset.x()), scaled(m_rib_offset.y() + m_y_shift));
    m_outer_wall[m_z_pos].push_back(std::move(pl));
}

float WipeTower::wall_feedrate(size_t tool) const
{
    // Bambu Studio WipeTower.cpp:3591 (finish_layer_new): the wall speed, capped by prime_tower_max_speed.
    return is_first_layer() ? std::min(m_first_layer_speed * 60.f, m_max_speed) :
                              std::min(60.0f * m_filpar[tool].max_e_speed / m_extrusion_flow, m_max_speed);
}

WipeTower::WipeTower(const PrintConfig& config, int plate_idx, Vec3d plate_origin, const float prime_volume, size_t initial_tool, const float wipe_tower_height) :
    m_semm(config.single_extruder_multi_material.value),
    m_wipe_tower_pos(config.wipe_tower_x.get_at(plate_idx), config.wipe_tower_y.get_at(plate_idx)),
    m_wipe_tower_width(float(config.prime_tower_width)),
    // BBS
    m_wipe_tower_height(wipe_tower_height),
    m_wipe_tower_rotation_angle(float(config.wipe_tower_rotation_angle)),
    m_wipe_tower_brim_width(float(config.prime_tower_brim_width)),
    m_y_shift(0.f),
    m_z_pos(0.f),
    //m_bridging(float(config.wipe_tower_bridging)),
    m_bridging(10.f),
    m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(config)),
    m_gcode_flavor(config.gcode_flavor),
    m_has_nozzle_rack(Slic3r::has_nozzle_rack(config)),
    m_travel_speed(float(get_value_at(config, config.travel_speed, ConfigFlowDomain::Process, initial_tool))),
    m_current_tool(initial_tool),
    //wipe_volumes(flush_matrix)
    m_wipe_volume(prime_volume),
    m_enable_timelapse_print(config.timelapse_type.value == TimelapseType::tlSmooth)
{
    // Read absolute value of first layer speed, if given as percentage,
    // it is taken over following default. Speeds from config are not
    // easily accessible here.
    const float default_speed = 60.f;
    m_first_layer_speed = float(get_value_at(config, config.initial_layer_speed, ConfigFlowDomain::Process, initial_tool));
    if (m_first_layer_speed == 0.f) // just to make sure autospeed doesn't break it.
        m_first_layer_speed = default_speed / 2.f;

    // prime_tower_brim_width -1 is Bambu Studio's "auto" brim, which the H2D/H2C/X2D/H2S/P2S/A2L
    // process presets ship. Bambu Studio resolves it from the tower height (plan_tower_new), and so
    // does estimate_wipe_tower_footprint() for the plate preview and the clearance checks. This
    // generator never did: finish_layer() turned -1 into zero brim loops, so those printers got a
    // tower with no brim at all while the preview promised one.
    if (m_wipe_tower_brim_width < 0.f)
        m_wipe_tower_brim_width = get_auto_brim_by_height(m_wipe_tower_height);

    // Bambu Studio's prime_tower_rib_wall / _rib_width / _extra_rib_length / _fillet_wall /
    // _max_speed (WipeTower.cpp:1773-1779), under the names this fork shares with Orca.
    m_use_rib_wall       = config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib;
    m_rib_width          = float(config.wipe_tower_rib_width.value);
    m_extra_rib_length   = float(config.wipe_tower_extra_rib_length.value);
    m_used_fillet        = config.wipe_tower_fillet_wall.value;
    m_max_speed          = float(config.wipe_tower_max_purge_speed.value) * 60.f;
    m_enable_arc_fitting = config.enable_arc_fitting.value && std::abs(m_wipe_tower_rotation_angle) < EPSILON;

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_cooling_tube_retraction = float(config.cooling_tube_retraction);
        m_cooling_tube_length     = float(config.cooling_tube_length);
        m_parking_pos_retraction  = float(config.parking_pos_retraction);
        m_extra_loading_move      = float(config.extra_loading_move);
        m_set_extruder_trimpot    = config.high_current_on_filament_swap;
    }
#endif
    // Calculate where the priming lines should be - very naive test not detecting parallelograms etc.
    const std::vector<Vec2d>& bed_points = config.printable_area.values;
    BoundingBoxf bb(bed_points);
    m_bed_width = float(bb.size().x());
    m_bed_shape = (bed_points.size() == 4 ? RectangularBed : CircularBed);

    if (m_bed_shape == CircularBed) {
        // this may still be a custom bed, check that the points are roughly on a circle
        double r2 = std::pow(m_bed_width/2., 2.);
        double lim2 = std::pow(m_bed_width/10., 2.);
        Vec2d center = bb.center();
        for (const Vec2d& pt : bed_points)
            if (std::abs(std::pow(pt.x()-center.x(), 2.) + std::pow(pt.y()-center.y(), 2.) - r2) > lim2) {
                m_bed_shape = CustomBed;
                break;
            }
    }

    m_bed_bottom_left = m_bed_shape == RectangularBed
                  ? Vec2f(bed_points.front().x(), bed_points.front().y())
                  : Vec2f::Zero();

    m_interface    = TowerInterface::Settings::from_config(config);
    m_use_gap_wall = config.wipe_tower_wall_gap.value;
    m_shared_bed   = TowerInterface::shared_printable_box(config);
}



void WipeTower::set_extruder(size_t idx, const PrintConfig& config)
{
    //while (m_filpar.size() < idx+1)   // makes sure the required element is in the vector
    m_filpar.push_back(FilamentParameters());

    m_filpar[idx].material = config.filament_type.get_at(idx);
    // m_filpar[idx].is_soluble = config.filament_soluble.get_at(idx);
    m_filpar[idx].is_soluble = config.wipe_tower_filament == 0 ? config.filament_soluble.get_at(idx) : (idx != size_t(config.wipe_tower_filament - 1));
    // BBS
    m_filpar[idx].is_support = config.filament_is_support.get_at(idx);
    m_filpar[idx].nozzle_temperature = get_value_at(config, config.nozzle_temperature, ConfigFlowDomain::Filament, idx);
    m_filpar[idx].nozzle_temperature_initial_layer = get_value_at(config, config.nozzle_temperature_initial_layer, ConfigFlowDomain::Filament, idx);

    // If this is a single extruder MM printer, we will use all the SE-specific config values.
    // Otherwise, the defaults will be used to turn off the SE stuff.
    // BBS: remove useless config
#if 0
    if (m_semm) {
        m_filpar[idx].loading_speed           = float(config.filament_loading_speed.get_at(idx));
        m_filpar[idx].loading_speed_start     = float(config.filament_loading_speed_start.get_at(idx));
        m_filpar[idx].unloading_speed         = float(config.filament_unloading_speed.get_at(idx));
        m_filpar[idx].unloading_speed_start   = float(config.filament_unloading_speed_start.get_at(idx));
        m_filpar[idx].delay                   = float(config.filament_toolchange_delay.get_at(idx));
        m_filpar[idx].cooling_moves           = config.filament_cooling_moves.get_at(idx);
        m_filpar[idx].cooling_initial_speed   = float(config.filament_cooling_initial_speed.get_at(idx));
        m_filpar[idx].cooling_final_speed     = float(config.filament_cooling_final_speed.get_at(idx));
    }
#endif

    m_filpar[idx].filament_area = float((M_PI/4.f) * pow(config.filament_diameter.get_at(idx), 2)); // all extruders are assumed to have the same filament diameter at this point
    float nozzle_diameter = float(config.nozzle_diameter.get_at(idx));
    m_filpar[idx].nozzle_diameter = nozzle_diameter; // to be used in future with (non-single) multiextruder MM

    float max_vol_speed = float(get_value_at(config, config.filament_max_volumetric_speed, ConfigFlowDomain::Filament, idx));
    if (max_vol_speed!= 0.f)
        m_filpar[idx].max_e_speed = (max_vol_speed / filament_area());
    m_filpar[idx].wipe_dist = float(config.wipe_distance.get_at(idx));

    m_filpar[idx].kind                  = TowerInterface::filament_kind(config, (unsigned int) idx);
    m_filpar[idx].interface_temperature = TowerInterface::interface_temperature(config.filament_tower_interface_print_temp.get_at(idx),
                                                                                config.nozzle_temperature_range_high.get_at(idx),
                                                                                m_filpar[idx].nozzle_temperature);
    m_filpar[idx].run_in_distance       = std::max(0.f, float(config.filament_tower_interface_pre_extrusion_dist.get_at(idx)));
    m_filpar[idx].extra_prime_length    = std::max(0.f, float(config.filament_tower_interface_pre_extrusion_length.get_at(idx)));
    // Bambu Studio names the nozzle of an interface M109 / M104 on multi-nozzle machines
    // (format_line_M109: T<physical extruder> ... N0).
    if (config.nozzle_diameter.values.size() > 1 && idx < config.filament_map.values.size()) {
        const int logical = config.filament_map.values[idx] - 1;
        if (logical >= 0)
            m_filpar[idx].physical_extruder = logical < int(config.physical_extruder_map.values.size()) ? config.physical_extruder_map.values[logical] : logical;
    }

    m_perimeter_width = nozzle_diameter * Width_To_Nozzle_Ratio; // all extruders are now assumed to have the same diameter
    // BBS: remove useless config
#if 0
    if (m_semm) {
        std::istringstream stream{config.filament_ramming_parameters.get_at(idx)};
        float speed = 0.f;
        stream >> m_filpar[idx].ramming_line_width_multiplicator >> m_filpar[idx].ramming_step_multiplicator;
        m_filpar[idx].ramming_line_width_multiplicator /= 100;
        m_filpar[idx].ramming_step_multiplicator /= 100;
        while (stream >> speed)
            m_filpar[idx].ramming_speed.push_back(speed);
    }
#endif

    m_used_filament_length.resize(std::max(m_used_filament_length.size(), idx + 1)); // makes sure that the vector is big enough so we don't have to check later
}



// Returns gcode to prime the nozzles at the front edge of the print bed.
std::vector<WipeTower::ToolChangeResult> WipeTower::prime(
	// print_z of the first layer.
	float 						initial_layer_print_height,
	// Extruder indices, in the order to be primed. The last extruder will later print the wipe tower brim, print brim and the object.
	const std::vector<unsigned int> &tools,
	// If true, the last priming are will be the same as the other priming areas, and the rest of the wipe will be performed inside the wipe tower.
	// If false, the last priming are will be large enough to wipe the last extruder sufficiently.
    bool 						/*last_wipe_inside_wipe_tower*/)
{
    return std::vector<ToolChangeResult>();
}

WipeTower::ToolChangeResult WipeTower::tool_change(size_t tool, bool extrude_perimeter, bool first_toolchange_to_nonsoluble)
{
    size_t old_tool = m_current_tool;

    float wipe_depth = 0.f;
	float wipe_length = 0.f;
    float purge_volume = 0.f;
    const WipeTowerInfo::ToolChange *planned = nullptr;

	// Finds this toolchange info
	if (tool != (unsigned int)(-1))
	{
		for (const auto &b : m_layer_info->tool_changes)
			if ( b.new_tool == tool ) {
                wipe_length = b.wipe_length;
                wipe_depth = b.required_depth;
                purge_volume = b.purge_volume;
                planned = &b;
				break;
			}
	}
	else {
		// Otherwise we are going to Unload only. And m_layer_info would be invalid.
	}

    box_coordinates cleaning_box(
		Vec2f(m_perimeter_width, m_perimeter_width),
		m_wipe_tower_width - 2 * m_perimeter_width,
        (tool != (unsigned int)(-1) ? wipe_depth + m_depth_traversed - m_perimeter_width
                                    : m_wipe_tower_depth - m_perimeter_width));

	WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_y_shift(m_y_shift + (tool!=(unsigned int)(-1) && (m_current_shape == SHAPE_REVERSED) ? m_layer_info->depth - m_layer_info->toolchanges_depth(): 0.f))
		.append(";--------------------\n"
				"; CP TOOLCHANGE START\n")
		.comment_with_value(" toolchange #", m_num_tool_changes + 1); // the number is zero-based


    if (tool != (unsigned)(-1))
        writer.append(std::string("; material : " + (m_current_tool < m_filpar.size() ? m_filpar[m_current_tool].material : "(NONE)") + " -> " + m_filpar[tool].material + "\n").c_str())
              .append(";--------------------\n");

    writer.speed_override_backup();
	writer.speed_override(100);

	Vec2f initial_position = cleaning_box.ld + Vec2f(0.f, m_depth_traversed);
    writer.set_initial_position(initial_position, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    // Increase the extruder driver current to allow fast ramming.
    //BBS
	//if (m_set_extruder_trimpot)
	//	writer.set_extruder_trimpot(750);

    // Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
    if (tool != (unsigned int)-1){ 			// This is not the last change.
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material,
                          is_first_layer() ? m_filpar[tool].nozzle_temperature_initial_layer : m_filpar[tool].nozzle_temperature);
        toolchange_Change(writer, tool, m_filpar[tool].material); // Change the tool, set a speed override for soluble and flex materials.
        toolchange_Load(writer, cleaning_box);
        // BBS
        //writer.travel(writer.x(), writer.y()-m_perimeter_width); // cooling and loading were done a bit down the road
        if (extrude_perimeter) {
            box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED) ? m_layer_info->toolchanges_depth() - m_layer_info->depth : 0.f),
                m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
            // align the perimeter

            Vec2f pos = initial_position;
            switch (m_cur_layer_id % 4){
            case 0:
                pos = wt_box.ld;
                break;
            case 1:
                pos = wt_box.rd;
                break;
            case 2:
                pos = wt_box.ru;
                break;
            case 3:
                pos = wt_box.lu;
                break;
            default: break;
            }
            writer.set_initial_position(pos, m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

            wt_box = align_perimeter(wt_box);
            if (print_wall_with_interface_gaps(writer, wt_box, wall_feedrate(m_current_tool))) {
                // Cut open for a tower interface run-in.
            } else if (m_use_rib_wall) {
                const Polygon wall = rib_wall_polygon(wt_box);
                writer.polygon(wall, wall_feedrate(m_current_tool), false);
                record_outer_wall(wall);
            } else
                writer.rectangle(wt_box);
        }

        {
            writer.travel(Vec2f(0, 0));
            writer.travel(initial_position);
        }
        const bool at_interface = planned != nullptr && planned->is_interface;
        if (at_interface)
            interface_before_wipe(writer, *planned);
        toolchange_Wipe(writer, cleaning_box, wipe_length);     // Wipe the newly loaded filament until the end of the assigned wipe area.
        if (at_interface)
            interface_after_wipe(writer, *planned);
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");
        ++ m_num_tool_changes;
    } else
        toolchange_Unload(writer, cleaning_box, m_filpar[m_current_tool].material, m_filpar[m_current_tool].nozzle_temperature);

    m_depth_traversed += wipe_depth;

    //BBS
	//if (m_set_extruder_trimpot)
	//	writer.set_extruder_trimpot(550);    // Reset the extruder current to a normal value.
	writer.speed_override_restore();
    writer.feedrate(m_travel_speed * 60.f)
          .flush_planner_queue()
          .reset_extruder()
          .append("; CP TOOLCHANGE END\n"
                  ";------------------\n"
                  "\n\n");

    // Ask our writer about how much material was consumed:
    if (m_current_tool < m_used_filament_length.size())
        m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, false, purge_volume);
}


// Ram the hot material out of the melt zone, retract the filament into the cooling tubes and let it cool.
void WipeTower::toolchange_Unload(
	WipeTowerWriter &writer,
	const box_coordinates 	&cleaning_box,
	const std::string&		 current_material,
	const int 				 new_temperature)
{
    // BBS: toolchange unload is done in change_filament_gcode
#if 0
	float xl = cleaning_box.ld.x() + 1.f * m_perimeter_width;
	float xr = cleaning_box.rd.x() - 1.f * m_perimeter_width;

	const float line_width = m_perimeter_width * m_filpar[m_current_tool].ramming_line_width_multiplicator;       // desired ramming line thickness
	const float y_step = line_width * m_filpar[m_current_tool].ramming_step_multiplicator * m_extra_spacing; // spacing between lines in mm

    writer.append("; CP TOOLCHANGE UNLOAD\n")
        .change_analyzer_line_width(line_width);

	unsigned i = 0;										// iterates through ramming_speed
	m_left_to_right = true;								// current direction of ramming
	float remaining = xr - xl ;							// keeps track of distance to the next turnaround
	float e_done = 0;									// measures E move done from each segment

	writer.travel(xl, cleaning_box.ld.y() + m_depth_traversed + y_step/2.f ); // move to starting position

    // if the ending point of the ram would end up in mid air, align it with the end of the wipe tower:
    if (m_layer_info > m_plan.begin() && m_layer_info < m_plan.end() && (m_layer_info-1!=m_plan.begin() || !m_adhesion )) {

        // this is y of the center of previous sparse infill border
        float sparse_beginning_y = 0.f;
        if (m_current_shape == SHAPE_REVERSED)
            sparse_beginning_y += ((m_layer_info-1)->depth - (m_layer_info-1)->toolchanges_depth())
                                      - ((m_layer_info)->depth-(m_layer_info)->toolchanges_depth()) ;
        else
            sparse_beginning_y += (m_layer_info-1)->toolchanges_depth() + m_perimeter_width;

        float sum_of_depths = 0.f;
        for (const auto& tch : m_layer_info->tool_changes) {  // let's find this toolchange
            if (tch.old_tool == m_current_tool) {
                sum_of_depths += tch.ramming_depth;
                float ramming_end_y = sum_of_depths;
                ramming_end_y -= (y_step/m_extra_spacing-m_perimeter_width) / 2.f;   // center of final ramming line

                if ( (m_current_shape == SHAPE_REVERSED   && ramming_end_y < sparse_beginning_y - 0.5f*m_perimeter_width  ) ||
                     (m_current_shape == SHAPE_NORMAL && ramming_end_y > sparse_beginning_y + 0.5f*m_perimeter_width  )  )
                {
                    writer.extrude(xl + tch.first_wipe_line-1.f*m_perimeter_width,writer.y());
                    remaining -= tch.first_wipe_line-1.f*m_perimeter_width;
                }
                break;
            }
            sum_of_depths += tch.required_depth;
        }
    }

    writer.disable_linear_advance();

    // now the ramming itself:
    while (i < m_filpar[m_current_tool].ramming_speed.size())
    {
        const float x = volume_to_length(m_filpar[m_current_tool].ramming_speed[i] * 0.25f, line_width, m_layer_height);
        const float e = m_filpar[m_current_tool].ramming_speed[i] * 0.25f / filament_area(); // transform volume per sec to E move;
        const float dist = std::min(x - e_done, remaining);		  // distance to travel for either the next 0.25s, or to the next turnaround
        const float actual_time = dist/x * 0.25f;
        writer.ram(writer.x(), writer.x() + (m_left_to_right ? 1.f : -1.f) * dist, 0.f, 0.f, e * (dist / x), dist / (actual_time / 60.f));
        remaining -= dist;

		if (remaining < WT_EPSILON)	{ // we reached a turning point
			writer.travel(writer.x(), writer.y() + y_step, 7200);
			m_left_to_right = !m_left_to_right;
			remaining = xr - xl;
		}
		e_done += dist; // subtract what was actually done
		if (e_done > x - WT_EPSILON) { // current segment finished
			++i;
			e_done = 0;
		}
	}
	Vec2f end_of_ramming(writer.x(),writer.y());
    writer.change_analyzer_line_width(m_perimeter_width);   // so the next lines are not affected by ramming_line_width_multiplier

    // Retraction:
    float old_x = writer.x();
    float turning_point = (!m_left_to_right ? xl : xr );
    if (m_semm && (m_cooling_tube_retraction != 0 || m_cooling_tube_length != 0)) {
        float total_retraction_distance = m_cooling_tube_retraction + m_cooling_tube_length/2.f - 15.f; // the 15mm is reserved for the first part after ramming
        writer.suppress_preview()
              .retract(15.f, m_filpar[m_current_tool].unloading_speed_start * 60.f) // feedrate 5000mm/min = 83mm/s
              .retract(0.70f * total_retraction_distance, 1.0f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.20f * total_retraction_distance, 0.5f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .retract(0.10f * total_retraction_distance, 0.3f * m_filpar[m_current_tool].unloading_speed * 60.f)
              .resume_preview();
    }
    // Wipe tower should only change temperature with single extruder MM. Otherwise, all temperatures should
    // be already set and there is no need to change anything. Also, the temperature could be changed
    // for wrong extruder.
    if (m_semm) {
        if (new_temperature != 0 && (new_temperature != m_old_temperature || is_first_layer()) ) { 	// Set the extruder temperature, but don't wait.
            // If the required temperature is the same as last time, don't emit the M104 again (if user adjusted the value, it would be reset)
            // However, always change temperatures on the first layer (this is to avoid issues with priming lines turned off).
            writer.set_extruder_temp(new_temperature, false);
            m_old_temperature = new_temperature;
        }
    }

    // Cooling:
    const int& number_of_moves = m_filpar[m_current_tool].cooling_moves;
    if (number_of_moves > 0) {
        const float& initial_speed = m_filpar[m_current_tool].cooling_initial_speed;
        const float& final_speed   = m_filpar[m_current_tool].cooling_final_speed;

        float speed_inc = (final_speed - initial_speed) / (2.f * number_of_moves - 1.f);

        writer.suppress_preview()
              .travel(writer.x(), writer.y() + y_step);
        old_x = writer.x();
        turning_point = xr-old_x > old_x-xl ? xr : xl;
        for (int i=0; i<number_of_moves; ++i) {
            float speed = initial_speed + speed_inc * 2*i;
            writer.load_move_x_advanced(turning_point, m_cooling_tube_length, speed);
            speed += speed_inc;
            writer.load_move_x_advanced(old_x, -m_cooling_tube_length, speed);
        }
    }

    // let's wait is necessary:
    writer.wait(m_filpar[m_current_tool].delay);
    // we should be at the beginning of the cooling tube again - let's move to parking position:
    writer.retract(-m_cooling_tube_length/2.f+m_parking_pos_retraction-m_cooling_tube_retraction, 2000);

	// this is to align ramming and future wiping extrusions, so the future y-steps can be uniform from the start:
    // the perimeter_width will later be subtracted, it is there to not load while moving over just extruded material
	writer.travel(end_of_ramming.x(), end_of_ramming.y() + (y_step/m_extra_spacing-m_perimeter_width) / 2.f + m_perimeter_width, 2400.f);

	writer.resume_preview()
		  .flush_planner_queue();
#endif
}

// Change the tool, set a speed override for soluble and flex materials.
void WipeTower::toolchange_Change(
	WipeTowerWriter &writer,
    const size_t 	new_tool,
    const std::string&  new_material)
{
    // Ask the writer about how much of the old filament we consumed:
    if (m_current_tool < m_used_filament_length.size())
    	m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    // This is where we want to place the custom gcodes. We will use placeholders for this.
    // These will be substituted by the actual gcodes when the gcode is generated.
    writer.append("[filament_end_gcode]\n");
    // Orca #15441: restore Z to the real topmost printed layer before running change_filament_gcode.
    // The wipe tower can be printing below that height (e.g. wipe_tower_no_sparse_layers), and
    // change_filament_gcode is free to travel anywhere on the bed, so it must not run while the
    // toolhead sits lower than already-printed parts. See append_tcr() in GCode.cpp.
    writer.append("[restore_layer_z_before_toolchange]\n");
    writer.append("[change_filament_gcode]\n");
    // Orca #15441: bring Z back down to the wipe tower layer once change_filament_gcode has finished,
    // so the rest of the tower (filament_start_gcode, wipe, ...) prints at the right height.
    writer.append("[deretraction_from_wipe_tower_generator]\n");

    // BBS: do travel in GCode::append_tcr() for lazy_lift
#if 0
    // Travel to where we assume we are. Custom toolchange or some special T code handling (parking extruder etc)
    // gcode could have left the extruder somewhere, we cannot just start extruding. We should also inform the
    // postprocessor that we absolutely want to have this in the gcode, even if it thought it is the same as before.
    Vec2f current_pos = writer.pos_rotated();
    writer.feedrate(m_travel_speed * 60.f)
          .append(std::string("G1 X") + Slic3r::float_to_string_decimal_point(current_pos.x())
                             +  " Y"  + Slic3r::float_to_string_decimal_point(current_pos.y())
                             + never_skip_tag() + "\n");
#endif

    // The toolchange Tn command will be inserted later, only in case that the user does
    // not provide a custom toolchange gcode.
	writer.set_tool(new_tool); // This outputs nothing, the writer just needs to know the tool has changed.
    writer.append("[filament_start_gcode]\n");

	writer.flush_planner_queue();
	m_current_tool = new_tool;
}

void WipeTower::toolchange_Load(
	WipeTowerWriter &writer,
	const box_coordinates  &cleaning_box)
{
    // BBS: tool load is done in change_filament_gcode
#if 0
    if (m_semm && (m_parking_pos_retraction != 0 || m_extra_loading_move != 0)) {
        float xl = cleaning_box.ld.x() + m_perimeter_width * 0.75f;
        float xr = cleaning_box.rd.x() - m_perimeter_width * 0.75f;
        float oldx = writer.x();	// the nozzle is in place to do the first wiping moves, we will remember the position

        // Load the filament while moving left / right, so the excess material will not create a blob at a single position.
        float turning_point = ( oldx-xl < xr-oldx ? xr : xl );
        float edist = m_parking_pos_retraction+m_extra_loading_move;

        writer.append("; CP TOOLCHANGE LOAD\n")
              .suppress_preview()
              .load(0.2f * edist, 60.f * m_filpar[m_current_tool].loading_speed_start)
              .load_move_x_advanced(turning_point, 0.7f * edist,        m_filpar[m_current_tool].loading_speed)  // Fast phase
              .load_move_x_advanced(oldx,          0.1f * edist, 0.1f * m_filpar[m_current_tool].loading_speed)  // Super slow*/

              .travel(oldx, writer.y()) // in case last move was shortened to limit x feedrate
              .resume_preview();

        // Reset the extruder current to the normal value.
        if (m_set_extruder_trimpot)
            writer.set_extruder_trimpot(550);
    }
#endif
}

// Wipe the newly loaded filament until the end of the assigned wipe area.
void WipeTower::toolchange_Wipe(
	WipeTowerWriter &writer,
	const box_coordinates  &cleaning_box,
	float wipe_length)
{
	// Increase flow on first layer, slow down print.
    writer.set_extrusion_flow(m_extrusion_flow * (is_first_layer() ? 1.15f : 1.f))
		  .append("; CP TOOLCHANGE WIPE\n");

    // BBS: add the note for gcode-check, when the flow changed, the width should follow the change
    if (is_first_layer()) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(1.15 * m_perimeter_width) + "\n");
    }

	const float& xl = cleaning_box.ld.x();
	const float& xr = cleaning_box.rd.x();

	// Variables x_to_wipe and traversed_x are here to be able to make sure it always wipes at least
    //   the ordered volume, even if it means violating the box. This can later be removed and simply
    // wipe until the end of the assigned area.

    float x_to_wipe = wipe_length;
    float dy = m_layer_info->extra_spacing * m_perimeter_width;

    // Bambu Studio's toolchange_wipe_new() (WipeTower.cpp:4034) caps the purge at prime_tower_max_speed.
    const float max_wipe_speed = std::min(4800.f, m_max_speed);
    const float target_speed = is_first_layer() ? std::min(m_first_layer_speed * 60.f, max_wipe_speed) : max_wipe_speed;
    float wipe_speed = 0.33f * target_speed;

    float start_y = writer.y();

#if 0
    // if there is less than 2.5*m_perimeter_width to the edge, advance straightaway (there is likely a blob anyway)
    if ((m_left_to_right ? xr-writer.x() : writer.x()-xl) < 2.5f*m_perimeter_width) {
        writer.travel((m_left_to_right ? xr-m_perimeter_width : xl+m_perimeter_width),writer.y()+dy);
        m_left_to_right = !m_left_to_right;
    }
#endif

    m_left_to_right = true;

    // BBS: do not need to move dy
#if 0
    if (m_depth_traversed != 0)
        writer.travel(xl, writer.y() + dy);
#endif
    
    bool need_change_flow = false;
    // now the wiping itself:
	for (int i = 0; true; ++i)	{
		if (i!=0) {
            if      (wipe_speed < 0.34f * target_speed) wipe_speed = 0.375f * target_speed;
            else if (wipe_speed < 0.377 * target_speed) wipe_speed = 0.458f * target_speed;
            else if (wipe_speed < 0.46f * target_speed) wipe_speed = 0.875f * target_speed;
            else wipe_speed = std::min(target_speed, wipe_speed + 50.f);
		}

        // BBS: check the bridging area and use the bridge flow
        if (need_change_flow || need_thick_bridge_flow(writer.y())) {
            writer.set_extrusion_flow(extrusion_flow(0.2));
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(0.2) + "\n");
            need_change_flow = true;
        }

        if (m_left_to_right)
            writer.extrude(xr + wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);
        else
            writer.extrude(xl - wipe_tower_wall_infill_overlap * m_perimeter_width, writer.y(), wipe_speed);

        // BBS: recover the flow in non-bridging area
        if (need_change_flow) {
            writer.set_extrusion_flow(m_extrusion_flow);
            writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height) + std::to_string(m_layer_height) + "\n");
        }

        if (writer.y() - float(EPSILON) > cleaning_box.lu.y())
            break;		// in case next line would not fit

        x_to_wipe -= (xr - xl);
		if (x_to_wipe < WT_EPSILON) {
            // BBS: Delete some unnecessary travel
            //writer.travel(m_left_to_right ? xl + 1.5f*m_perimeter_width : xr - 1.5f*m_perimeter_width, writer.y(), 7200);
			break;
		}
		// stepping to the next line:
        writer.extrude(writer.x(), writer.y() + dy);
		m_left_to_right = !m_left_to_right;
	}

    float end_y = writer.y();

    // We may be going back to the model - wipe the nozzle. If this is followed
    // by finish_layer, this wipe path will be overwritten.
    //writer.add_wipe_point(writer.x(), writer.y())
    //      .add_wipe_point(writer.x(), writer.y() - dy)
    //      .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y() - dy);
    // BBS: modify the wipe_path after toolchange
    writer.add_wipe_point(writer.x(), writer.y())
          .add_wipe_point(! m_left_to_right ? m_wipe_tower_width : 0.f, writer.y());

    if (m_layer_info != m_plan.end() && m_current_tool != m_layer_info->tool_changes.back().new_tool)
        m_left_to_right = !m_left_to_right;

    writer.set_extrusion_flow(m_extrusion_flow); // Reset the extrusion flow.
    // BBS: add the note for gcode-check when the flow changed
    if (is_first_layer()) {
        writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width) + std::to_string(m_perimeter_width) + "\n");
    }
}



// BBS
WipeTower::box_coordinates WipeTower::align_perimeter(const WipeTower::box_coordinates& perimeter_box)
{
    box_coordinates aligned_box = perimeter_box;

    float spacing = m_extra_spacing * m_perimeter_width;
    float up = perimeter_box.lu(1) - m_perimeter_width;
    up = align_ceil(up, spacing);
    up += m_perimeter_width;
    up = std::min(up, m_wipe_tower_depth);

    float down = perimeter_box.ld(1) - m_perimeter_width;
    down = align_floor(down, spacing);
    down += m_perimeter_width;
    down = std::max(down, -m_y_shift);

    aligned_box.lu(1) = aligned_box.ru(1) = up;
    aligned_box.ld(1) = aligned_box.rd(1) = down;

    return aligned_box;
}

WipeTower::ToolChangeResult WipeTower::finish_layer(bool extrude_perimeter, bool extruder_fill)
{
	assert(! this->layer_finished());
    m_current_layer_finished = true;

    size_t old_tool = m_current_tool;

	WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack);
	writer.set_extrusion_flow(m_extrusion_flow)
		.set_z(m_z_pos)
		.set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");

	// Slow down on the 1st layer.
    bool first_layer = is_first_layer();
    // BBS: speed up perimeter speed to 90mm/s for non-first layer (prime_tower_max_speed, 90 mm/s by default)
    float           feedrate   = wall_feedrate(m_current_tool);
    float fill_box_y = m_layer_info->toolchanges_depth() + m_perimeter_width;
    box_coordinates fill_box(Vec2f(m_perimeter_width, fill_box_y),
                             m_wipe_tower_width - 2 * m_perimeter_width, m_layer_info->depth - fill_box_y);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                 m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // inner perimeter of the sparse section, if there is space for it:
    if (fill_box.ru.y() - fill_box.rd.y() > m_perimeter_width - WT_EPSILON)
        writer.rectangle_fill_box(this, fill_box.ld, fill_box.rd.x() - fill_box.ld.x(), fill_box.ru.y() - fill_box.rd.y(), feedrate);

    // we are in one of the corners, travel to ld along the perimeter:
    // BBS: Delete some unnecessary travel
    //if (writer.x() > fill_box.ld.x() + EPSILON) writer.travel(fill_box.ld.x(), writer.y());
    //if (writer.y() > fill_box.ld.y() + EPSILON) writer.travel(writer.x(), fill_box.ld.y());

    // Extrude infill to support the material to be printed above.
    const float dy = (fill_box.lu.y() - fill_box.ld.y() - m_perimeter_width);
    float left = fill_box.lu.x() + 2*m_perimeter_width;
    float right = fill_box.ru.x() - 2 * m_perimeter_width;
    std::vector<Vec2f> finish_rect_wipe_path;
    if (extruder_fill && dy > m_perimeter_width)
    {
        writer.travel(fill_box.ld + Vec2f(m_perimeter_width * 2, 0.f))
              .append(";--------------------\n"
                      "; CP EMPTY GRID START\n")
              .comment_with_value(" layer #", m_num_layer_changes + 1);

        // Is there a soluble filament wiped/rammed at the next layer?
        // If so, the infill should not be sparse.
        bool solid_infill = m_layer_info+1 == m_plan.end()
                          ? false
                          : std::any_of((m_layer_info+1)->tool_changes.begin(),
                                        (m_layer_info+1)->tool_changes.end(),
                                   [this](const WipeTowerInfo::ToolChange& tch) {
                                       return m_filpar[tch.new_tool].is_soluble
                                           || m_filpar[tch.old_tool].is_soluble;
                                   });
        solid_infill |= first_layer && m_adhesion;

        if (solid_infill) {
            float sparse_factor = 1.5f; // 1=solid, 2=every other line, etc.
            if (first_layer) { // the infill should touch perimeters
                left  -= m_perimeter_width;
                right += m_perimeter_width;
                sparse_factor = 1.f;
            }
            float y = fill_box.ld.y() + m_perimeter_width;
            int n = dy / (m_perimeter_width * sparse_factor);
            float spacing = (dy-m_perimeter_width)/(n-1);
            int i=0;
            for (i=0; i<n; ++i) {
                writer.extrude(writer.x(), y, feedrate)
                      .extrude(i%2 ? left : right, y);
                y = y + spacing;
            }
            writer.extrude(writer.x(), fill_box.lu.y());
        } else {
            // Extrude an inverse U at the left of the region and the sparse infill.
            writer.extrude(fill_box.lu + Vec2f(m_perimeter_width * 2, 0.f), feedrate);

            const int n = 1+int((right-left)/m_bridging);
            const float dx = (right-left)/n;
            for (int i=1;i<=n;++i) {
                float x=left+dx*i;
                writer.travel(x,writer.y());
                writer.extrude(x,i%2 ? fill_box.rd.y() : fill_box.ru.y());
            }
            // BBS: add wipe_path for this case: only with finish rectangle
            finish_rect_wipe_path.emplace_back(writer.pos());
            finish_rect_wipe_path.emplace_back(Vec2f(left + dx * n, n % 2 ? fill_box.ru.y() : fill_box.rd.y()));
        }

        writer.append("; CP EMPTY GRID END\n"
                      ";------------------\n\n\n\n\n\n\n");
    }

    // outer perimeter (always):
    // BBS
    box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)),
        m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
    wt_box = align_perimeter(wt_box);
    // Bambu Studio finish_layer_new() (WipeTower.cpp:3699): the rib wall is needed for the brim
    // loops even when the tool change already printed it.
    Polygon outer_wall;
    if (m_use_rib_wall)
        outer_wall = rib_wall_polygon(wt_box);
    if (extrude_perimeter && ! print_wall_with_interface_gaps(writer, wt_box, feedrate)) {
        if (m_use_rib_wall) {
            writer.polygon(outer_wall, feedrate, false);
            record_outer_wall(outer_wall);
        } else
            writer.rectangle(wt_box, feedrate);
    }
    // A tower interface run-in also crosses the brim loops (the chamfer above the first layer).
    const size_t layer_id = size_t(m_layer_info - m_plan.begin());
    const std::vector<TowerInterface::GapPoint> no_gaps;
    const std::vector<TowerInterface::GapPoint> &loop_gaps = layer_id < m_interface_gaps.size() ? m_interface_gaps[layer_id] : no_gaps;
    auto print_loop = [&writer, &loop_gaps, feedrate, this](const Polygon &loop, bool pre_simplify) {
        if (loop_gaps.empty()) {
            writer.polygon(loop, feedrate, pre_simplify);
        } else {
            Polygon inserted;
            writer.wall_pieces(TowerInterface::cut_wall_gaps(loop, loop_gaps, 2.5f * m_perimeter_width, inserted), feedrate);
        }
    };

    // brim chamfer
    float spacing = m_perimeter_width - m_layer_height * float(1. - M_PI_4);
    // How many perimeters shall the brim have?
    int loops_num = (m_wipe_tower_brim_width + spacing / 2.f) / spacing;
    const float max_chamfer_width = 3.f;
    if (!first_layer) {
        // stop print chamfer if depth changes
        if (m_layer_info->depth != m_plan.front().depth) {
            loops_num = 0;
        }
        else {
            // limit max chamfer width to 3 mm
            int chamfer_loops_num = (int)(max_chamfer_width / spacing);
            int dist_to_1st = m_layer_info - m_plan.begin() - m_first_layer_idx;
            // A layer below the tower's first real layer gets no chamfer. With "no sparse layers"
            // the first real layer is the first tool change, which can sit far up the print; the
            // layers below it are sparse and never printed, but their walls still go into
            // m_outer_wall for the conflict checker. Counting a negative distance there added a
            // brim loop per layer down - hundreds of loops reaching across the bed, which the
            // checker then reported as the tower hitting the objects.
            loops_num = dist_to_1st < 0 ? 0 : std::min(loops_num, chamfer_loops_num) - dist_to_1st;
        }
    }

    if (loops_num > 0 && m_use_rib_wall) {
        // Bambu Studio finish_layer_new() (WipeTower.cpp:3726): the brim follows the rib wall.
        for (int i = 0; i < loops_num; ++i) {
            Polygons grown = offset(outer_wall, scaled(spacing));
            if (grown.empty())
                break;
            outer_wall = std::move(grown.front());
            print_loop(outer_wall, true);
            record_outer_wall(outer_wall);
        }
        if (first_layer)
            m_wipe_tower_brim_width_real = loops_num * spacing + spacing / 2.f;
    } else if (loops_num > 0) {
        box_coordinates box = wt_box;
        for (size_t i = 0; i < loops_num; ++i) {
            box.expand(spacing);
            // Explicit feedrate: on a layer whose wall was printed by the toolchange (extrude_perimeter
            // false, e.g. the first layer of a no-sparse tower) the last F in the writer is a travel, and
            // the brim would inherit it. Where the wall was just printed this emits no F at all.
            if (loop_gaps.empty())
                writer.rectangle(box, feedrate);
            else
                print_loop(generate_rectange_polygon(box.ld, box.ru), false);
        }

        if (first_layer) {
            // Save actual brim width to be later passed to the Print object, which will use it
            // for skirt calculation and pass it to GLCanvas for precise preview box
            m_wipe_tower_brim_width_real = wt_box.ld.x() - box.ld.x() + spacing / 2.f;
        }
        wt_box = box;
    }

    if (m_use_rib_wall && (extrude_perimeter || loops_num > 0)) {
        // Bambu Studio WipeTower.cpp:3749: wipe back along the wall just printed.
        writer.add_wipe_path(outer_wall, m_filpar[m_current_tool].wipe_dist);
    } else {
        // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
        Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd :
                       (writer.pos() == wt_box.rd ? wt_box.ru :
                       (writer.pos() == wt_box.ru ? wt_box.lu :
                        wt_box.ld)));

        // BBS: add wipe_path for this case: only with finish rectangle
        if (finish_rect_wipe_path.size() == 2 && finish_rect_wipe_path[0] == writer.pos())
            target = finish_rect_wipe_path[1];

        writer.add_wipe_point(writer.pos())
              .add_wipe_point(target);
    }

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (! m_sparse_layers_skipped || toolchanges_on_layer)
        if (m_current_tool < m_used_filament_length.size())
            m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, 0.f);
}

// Appends a toolchange into m_plan and calculates neccessary depth of the corresponding box
void WipeTower::plan_toolchange(float z_par, float layer_height_par, unsigned int old_tool,
                                unsigned int new_tool, float wipe_volume, float purge_volume)
{
	assert(m_plan.empty() || m_plan.back().z <= z_par + WT_EPSILON);	// refuses to add a layer below the last one

	if (m_plan.empty() || m_plan.back().z + WT_EPSILON < z_par) // if we moved to a new layer, we'll add it to m_plan first
		m_plan.push_back(WipeTowerInfo(z_par, layer_height_par));

    if (m_first_layer_idx == size_t(-1) && (! m_sparse_layers_skipped || old_tool != new_tool))
        m_first_layer_idx = m_plan.size() - 1;

    if (old_tool == new_tool)	// new layer without toolchanges - we are done
        return;

	// this is an actual toolchange - let's calculate depth to reserve on the wipe tower
    float depth = 0.f;
    float width = m_wipe_tower_width - 2 * m_perimeter_width;

    // BBS: if the wipe tower width is too small, the depth will be infinity
    if (width <= EPSILON)
        return;

    // BBS: remove old filament ramming and first line
#if 0
	float length_to_extrude = volume_to_length(0.25f * std::accumulate(m_filpar[old_tool].ramming_speed.begin(), m_filpar[old_tool].ramming_speed.end(), 0.f),
										m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator,
										layer_height_par);
	depth = (int(length_to_extrude / width) + 1) * (m_perimeter_width * m_filpar[old_tool].ramming_line_width_multiplicator * m_filpar[old_tool].ramming_step_multiplicator);
    float ramming_depth = depth;
    length_to_extrude = width*((length_to_extrude / width)-int(length_to_extrude / width)) - width;
    float first_wipe_line = -length_to_extrude;
    length_to_extrude += volume_to_length(wipe_volume, m_perimeter_width, layer_height_par);
    length_to_extrude = std::max(length_to_extrude,0.f);

    depth += (int(length_to_extrude / width) + 1) * m_perimeter_width;
    depth *= m_extra_spacing;

    m_plan.back().tool_changes.push_back(WipeTowerInfo::ToolChange(old_tool, new_tool, depth, ramming_depth, first_wipe_line, wipe_volume));
#else
    float length_to_extrude = volume_to_length(wipe_volume, m_perimeter_width, layer_height_par);

    depth += std::ceil(length_to_extrude / width) * m_perimeter_width;
    //depth *= m_extra_spacing;

    m_plan.back().tool_changes.push_back(WipeTowerInfo::ToolChange(old_tool, new_tool, depth, 0.f, 0.f, wipe_volume, length_to_extrude, purge_volume));
#endif
}



void WipeTower::make_rib_tower_square()
{
    // Bambu Studio plan_tower_new() (WipeTower.cpp:4524-4535): a rib tower is square, with the
    // purge area it would have had at the configured width.
    float max_depth = 0.f;
    for (const auto &info : m_plan)
        max_depth = std::max(max_depth, info.toolchanges_depth());
    if (max_depth < EPSILON)
        return;
    max_depth += m_perimeter_width;
    const float square_width = align_ceil(std::sqrt(max_depth * m_wipe_tower_width * m_extra_spacing), m_perimeter_width);
    if (square_width - 2 * m_perimeter_width <= EPSILON)
        return;
    m_wipe_tower_width = square_width;
    // plan_toolchange() at the new line length.
    const float line_len = m_wipe_tower_width - 2 * m_perimeter_width;
    for (auto &info : m_plan)
        for (auto &toolchange : info.tool_changes)
            toolchange.required_depth = std::ceil(volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height) / line_len) * m_perimeter_width;
}

void WipeTower::plan_tower()
{
    if (m_use_rib_wall)
        make_rib_tower_square();

    // BBS
    // calculate extra spacing
    float max_depth = 0.f;
    for (auto& info : m_plan)
        max_depth = std::max(max_depth, info.toolchanges_depth());

    float min_wipe_tower_depth = 0.f;
    auto iter = WipeTower::min_depth_per_height.begin();
    while (iter != WipeTower::min_depth_per_height.end()) {
        auto curr_height_to_depth = *iter;

        // This is the case that wipe tower height is lower than the first min_depth_to_height member.
        if (curr_height_to_depth.first >= m_wipe_tower_height) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        iter++;

        // If curr_height_to_depth is the last member, use its min_depth.
        if (iter == WipeTower::min_depth_per_height.end()) {
            min_wipe_tower_depth = curr_height_to_depth.second;
            break;
        }

        // If wipe tower height is between the current and next member, set the min_depth as linear interpolation between them
        auto next_height_to_depth = *iter;
        if (next_height_to_depth.first > m_wipe_tower_height) {
            float height_base = curr_height_to_depth.first;
            float height_diff = next_height_to_depth.first - curr_height_to_depth.first;
            float min_depth_base = curr_height_to_depth.second;
            float depth_diff = next_height_to_depth.second - curr_height_to_depth.second;

            min_wipe_tower_depth = min_depth_base + (m_wipe_tower_height - curr_height_to_depth.first) / height_diff * depth_diff;
            break;
        }
    }

    {
        if (m_enable_timelapse_print && max_depth < EPSILON) {
            max_depth = min_wipe_tower_depth;
            // Bambu Studio WipeTower.cpp:4558: an idle rib tower is a square of the minimum depth.
            if (m_use_rib_wall)
                m_wipe_tower_width = max_depth;
        }

        if (max_depth + EPSILON < min_wipe_tower_depth) {
            // Bambu Studio WipeTower.cpp:4561-4566: a rib wall reaches the stability minimum by
            // lengthening the ribs, not by spreading the purge lines.
            if (m_use_rib_wall) {
                m_rib_length    = std::max(m_rib_length, min_wipe_tower_depth * float(std::sqrt(2.)));
                m_extra_spacing = 1.f;
            } else
                m_extra_spacing = min_wipe_tower_depth / max_depth;
        } else
            m_extra_spacing = 1.f;

        for (int idx = 0; idx < m_plan.size(); idx++) {
            auto& info = m_plan[idx];
            if (idx == 0 && m_extra_spacing > 1.f + EPSILON) {
                // apply solid fill for the first layer
                info.extra_spacing = 1.f;
                for (auto& toolchange : info.tool_changes) {
                    float x_to_wipe = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                    float line_len = m_wipe_tower_width - 2 * m_perimeter_width;
                    float x_to_wipe_new = x_to_wipe * m_extra_spacing;
                    x_to_wipe_new = std::floor(x_to_wipe_new / line_len) * line_len;
                    x_to_wipe_new = std::max(x_to_wipe_new, x_to_wipe);

                    int line_count = std::ceil((x_to_wipe_new - WT_EPSILON) / line_len);
                    toolchange.required_depth = line_count * m_perimeter_width;
                    toolchange.wipe_volume = x_to_wipe_new / x_to_wipe * toolchange.wipe_volume;
                    toolchange.wipe_length = x_to_wipe_new;
                }
            }
            else {
                info.extra_spacing = m_extra_spacing;
                for (auto& toolchange : info.tool_changes) {
                    toolchange.required_depth *= m_extra_spacing;
                    toolchange.wipe_length = volume_to_length(toolchange.wipe_volume, m_perimeter_width, info.height);
                }
            }
        }
    }

	// Calculate m_wipe_tower_depth (maximum depth for all the layers) and propagate depths downwards
	m_wipe_tower_depth = 0.f;
	for (auto& layer : m_plan)
		layer.depth = 0.f;

    float max_depth_for_all = 0;
    for (int layer_index = int(m_plan.size()) - 1; layer_index >= 0; --layer_index)
	{
		float this_layer_depth = std::max(m_plan[layer_index].depth, m_plan[layer_index].toolchanges_depth());
        if (m_enable_timelapse_print && this_layer_depth < EPSILON)
            this_layer_depth = min_wipe_tower_depth;

		m_plan[layer_index].depth = this_layer_depth;

		if (this_layer_depth > m_wipe_tower_depth - m_perimeter_width)
			m_wipe_tower_depth = this_layer_depth + m_perimeter_width;

		for (int i = layer_index - 1; i >= 0 ; i--)
		{
			if (m_plan[i].depth - this_layer_depth < 2*m_perimeter_width )
				m_plan[i].depth = this_layer_depth;
		}

        if (m_enable_timelapse_print && layer_index == 0)
            max_depth_for_all = m_plan[0].depth;
    }

    if (m_enable_timelapse_print) {
        for (int i = int(m_plan.size()) - 1; i >= 0; i--) {
            m_plan[i].depth = max_depth_for_all;
        }
    }

    if (m_use_rib_wall) {
        // Bambu Studio WipeTower.cpp:4603-4607.
        float diagonal = std::sqrt(m_wipe_tower_depth * m_wipe_tower_depth + m_wipe_tower_width * m_wipe_tower_width);
        m_rib_length   = std::max({m_rib_length, diagonal});
        m_rib_length += m_extra_rib_length;
        m_rib_length = std::max(diagonal, m_rib_length);
        m_rib_width  = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f); // Ensure that the rib wall of the wipetower are attached to the infill.
    }
}

void WipeTower::set_rib_offset()
{
    // Bambu Studio measures the first layer's wall as it extrudes it (generate_support_wall_new(),
    // WipeTower.cpp:5117) and GCode.cpp shifts every tower move by the result. Here it is measured
    // up front so the writer can emit the shifted coordinates directly: same state as generate()
    // sets for the first printed layer.
    m_rib_offset    = Vec2f::Zero();
    m_rib_footprint = Vec2f::Zero();
    m_first_layer_wall.clear();
    auto first = std::find_if(m_plan.begin(), m_plan.end(), [this](const WipeTowerInfo &info) { return info.depth >= m_perimeter_width; });
    if (first == m_plan.end())
        return;
    const float saved_z = m_z_pos, saved_y_shift = m_y_shift;
    m_z_pos = first->z;
    if (first->depth < m_wipe_tower_depth - m_perimeter_width)
        m_y_shift = align_round((m_wipe_tower_depth - first->depth) / 2.f, m_extra_spacing * m_perimeter_width);
    box_coordinates wt_box(Vec2f(0.f, 0.f), m_wipe_tower_width, first->depth + m_perimeter_width);
    wt_box = align_perimeter(wt_box);
    Polygon wall = rib_wall_polygon(wt_box);
    wall.translate(0, scaled(m_y_shift));
    const BoundingBox bbox = get_extents(wall);
    m_rib_offset    = Vec2f(-unscaled<float>(bbox.min.x()), -unscaled<float>(bbox.min.y()));
    m_rib_footprint = unscaled<float>(bbox.size());
    wall.translate(-bbox.min.x(), -bbox.min.y());
    m_first_layer_wall = std::move(wall);
    m_z_pos   = saved_z;
    m_y_shift = saved_y_shift;
}

void WipeTower::save_on_last_wipe()
{
    for (m_layer_info=m_plan.begin();m_layer_info<m_plan.end();++m_layer_info) {
        set_layer(m_layer_info->z, m_layer_info->height, 0, m_layer_info->z == m_plan.front().z, m_layer_info->z == m_plan.back().z);
        if (m_layer_info->tool_changes.size()==0)   // we have no way to save anything on an empty layer
            continue;

        // Which toolchange will finish_layer extrusions be subtracted from?
        // BBS: consider both soluable and support properties
        int idx = first_toolchange_to_nonsoluble_nonsupport(m_layer_info->tool_changes);

        for (int i=0; i<int(m_layer_info->tool_changes.size()); ++i) {
            auto& toolchange = m_layer_info->tool_changes[i];
            tool_change(toolchange.new_tool);

            if (i == idx) {
                float width = m_wipe_tower_width - 3*m_perimeter_width; // width we draw into
                float length_to_save = finish_layer().total_extrusion_length_in_plane();
                float length_to_wipe = volume_to_length(toolchange.wipe_volume,
                                      m_perimeter_width, m_layer_info->height)  - toolchange.first_wipe_line - length_to_save;

                length_to_wipe = std::max(length_to_wipe,0.f);
                float depth_to_wipe = m_perimeter_width * (std::floor(length_to_wipe/width) + ( length_to_wipe > 0.f ? 1.f : 0.f ) ) * m_extra_spacing;

                toolchange.required_depth = toolchange.ramming_depth + depth_to_wipe;
            }
        }
    }
}


// BBS: consider both soluable and support properties
// Return index of first toolchange that switches to non-soluble and non-support extruder
// ot -1 if there is no such toolchange.
int WipeTower::first_toolchange_to_nonsoluble_nonsupport(
        const std::vector<WipeTowerInfo::ToolChange>& tool_changes) const
{
    for (size_t idx=0; idx<tool_changes.size(); ++idx)
        if (! m_filpar[tool_changes[idx].new_tool].is_soluble && ! m_filpar[tool_changes[idx].new_tool].is_support)
            return idx;
    return -1;
}

static WipeTower::ToolChangeResult merge_tcr(WipeTower::ToolChangeResult& first,
                                             WipeTower::ToolChangeResult& second)
{
    assert(first.new_tool == second.initial_tool);
    WipeTower::ToolChangeResult out = first;
    if (first.end_pos != second.start_pos)
        out.gcode += "G1 X" + Slic3r::float_to_string_decimal_point(second.start_pos.x(), 3)
                     + " Y" + Slic3r::float_to_string_decimal_point(second.start_pos.y(), 3)
                     + " F7200\n";
    out.gcode += second.gcode;
    out.extrusions.insert(out.extrusions.end(), second.extrusions.begin(), second.extrusions.end());
    out.end_pos = second.end_pos;
    out.wipe_path = second.wipe_path;
    out.initial_tool = first.initial_tool;
    out.new_tool = second.new_tool;

    // BBS
    out.purge_volume += second.purge_volume;
    return out;
}


// Processes vector m_plan and calls respective functions to generate G-code for the wipe tower
// Resulting ToolChangeResults are appended into vector "result"
void WipeTower::generate(std::vector<std::vector<WipeTower::ToolChangeResult>> &result)
{
	if (m_plan.empty())
        return;

    m_extra_spacing = 1.f;

    // Bambu Studio generate_new() (WipeTower.cpp:4672): the ribs taper to the real tower top.
    if (m_use_rib_wall)
        m_wipe_tower_height = m_plan.back().z;

	plan_tower();
    // BBS
#if 0
    for (int i=0;i<5;++i) {
        save_on_last_wipe();
        plan_tower();
    }
#endif
    m_outer_wall.clear();
    if (m_use_rib_wall)
        set_rib_offset();
    plan_interfaces();

    m_layer_info = m_plan.begin();

    // we don't know which extruder to start with - we'll set it according to the first toolchange
    for (const auto& layer : m_plan) {
        if (!layer.tool_changes.empty()) {
            m_current_tool = layer.tool_changes.front().old_tool;
            break;
        }
    }

    for (auto& used : m_used_filament_length) // reset used filament stats
        used = 0.f;

    m_old_temperature = -1; // reset last temperature written in the gcode
    int index = 0;
    std::vector<WipeTower::ToolChangeResult> layer_result;
	for (auto layer : m_plan)
	{
        m_cur_layer_id = index++;
        set_layer(layer.z, layer.height, 0, false/*layer.z == m_plan.front().z*/, layer.z == m_plan.back().z);
        // BBS
        //m_internal_rotation += 180.f;

        if (m_layer_info->depth < m_perimeter_width)
            continue;

        if (m_layer_info->depth < m_wipe_tower_depth - m_perimeter_width) {
            // align y shift to perimeter width
            float dy = m_extra_spacing * m_perimeter_width;
            m_y_shift = (m_wipe_tower_depth - m_layer_info->depth) / 2.f;
            m_y_shift = align_round(m_y_shift, dy);
        }

        // BBS: consider both soluable and support properties
        int idx = first_toolchange_to_nonsoluble_nonsupport (layer.tool_changes);
        ToolChangeResult finish_layer_tcr;
        ToolChangeResult timelapse_wall;

        if (idx == -1) {
            // if there is no toolchange switching to non-soluble, finish layer
            // will be called at the very beginning. That's the last possibility
            // where a nonsoluble tool can be.
            if (m_enable_timelapse_print) {
                timelapse_wall = only_generate_out_wall();
            }
            finish_layer_tcr = finish_layer(m_enable_timelapse_print ? false : true, layer.extruder_fill);
        }

        for (int i=0; i<int(layer.tool_changes.size()); ++i) {
            if (i == 0 && m_enable_timelapse_print) {
                timelapse_wall = only_generate_out_wall();
            }

            if (i == idx) {
                layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool, m_enable_timelapse_print ? false : true));
                // finish_layer will be called after this toolchange
                finish_layer_tcr = finish_layer(false, layer.extruder_fill);
            }
            else {
                if (idx == -1 && i == 0) {
                    layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool, false, true));
                } else {
                    layer_result.emplace_back(tool_change(layer.tool_changes[i].new_tool));
                }
            }
        }

        if (layer_result.empty()) {
            // there is nothing to merge finish_layer with
            layer_result.emplace_back(std::move(finish_layer_tcr));
        }
        else {
            if (idx == -1)
                layer_result[0] = merge_tcr(finish_layer_tcr, layer_result[0]);
            else if (is_valid_gcode(finish_layer_tcr.gcode))
                layer_result[idx] = merge_tcr(layer_result[idx], finish_layer_tcr);
        }

        if (m_enable_timelapse_print) {
            layer_result.insert(layer_result.begin(), std::move(timelapse_wall));
        }

		result.emplace_back(std::move(layer_result));
	}
}

WipeTower::ToolChangeResult WipeTower::only_generate_out_wall()
{
    size_t old_tool = m_current_tool;

    WipeTowerWriter writer(m_layer_height, m_perimeter_width, m_gcode_flavor, m_filpar, m_has_nozzle_rack);
    writer.set_extrusion_flow(m_extrusion_flow)
        .set_z(m_z_pos)
        .set_initial_tool(m_current_tool)
        .set_origin_offset(m_rib_offset)
        .set_arc_fitting(m_enable_arc_fitting)
        .set_y_shift(m_y_shift - (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f));

    // Slow down on the 1st layer.
    // BBS: speed up perimeter speed to 90mm/s for non-first layer (prime_tower_max_speed, 90 mm/s by default)
    float           feedrate   = wall_feedrate(m_current_tool);
    float           fill_box_y = m_layer_info->toolchanges_depth() + m_perimeter_width;
    box_coordinates fill_box(Vec2f(m_perimeter_width, fill_box_y), m_wipe_tower_width - 2 * m_perimeter_width, m_layer_info->depth - fill_box_y);

    writer.set_initial_position((m_left_to_right ? fill_box.ru : fill_box.lu), // so there is never a diagonal travel
                                m_wipe_tower_width, m_wipe_tower_depth, m_internal_rotation);

    bool toolchanges_on_layer = m_layer_info->toolchanges_depth() > WT_EPSILON;

    // we are in one of the corners, travel to ld along the perimeter:
    // BBS: Delete some unnecessary travel
    //if (writer.x() > fill_box.ld.x() + EPSILON) writer.travel(fill_box.ld.x(), writer.y());
    //if (writer.y() > fill_box.ld.y() + EPSILON) writer.travel(writer.x(), fill_box.ld.y());
    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start) + "\n");
    // outer perimeter (always):
    // BBS
    box_coordinates wt_box(Vec2f(0.f, (m_current_shape == SHAPE_REVERSED ? m_layer_info->toolchanges_depth() : 0.f)), m_wipe_tower_width, m_layer_info->depth + m_perimeter_width);
    wt_box = align_perimeter(wt_box);
    if (m_use_rib_wall) {
        // Bambu Studio only_generate_out_wall() (WipeTower.cpp:5023-5030).
        const Polygon wall = rib_wall_polygon(wt_box);
        if (! print_wall_with_interface_gaps(writer, wt_box, feedrate)) {
            writer.polygon(wall, feedrate, false);
            record_outer_wall(wall);
        }
        writer.add_wipe_path(wall, m_filpar[m_current_tool].wipe_dist);
    } else {
        if (! print_wall_with_interface_gaps(writer, wt_box, feedrate))
            writer.rectangle(wt_box, feedrate);

        // Now prepare future wipe. box contains rectangle that was extruded last (ccw).
        Vec2f target = (writer.pos() == wt_box.ld ? wt_box.rd : (writer.pos() == wt_box.rd ? wt_box.ru : (writer.pos() == wt_box.ru ? wt_box.lu : wt_box.ld)));
        writer.add_wipe_point(writer.pos()).add_wipe_point(target);
    }

    writer.append(";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End) + "\n");

    // Ask our writer about how much material was consumed.
    // Skip this in case the layer is sparse and config option to not print sparse layers is enabled.
    if (!m_sparse_layers_skipped || toolchanges_on_layer)
        if (m_current_tool < m_used_filament_length.size()) m_used_filament_length[m_current_tool] += writer.get_and_reset_used_filament_length();

    return construct_tcr(writer, false, old_tool, true, 0.f);
}

void WipeTower::plan_interfaces()
{
    m_interface_gaps.assign(m_plan.size(), {});
    m_run_in_reserve = 0.f;
    if (! m_interface.any())
        return;
    const bool run_in = m_interface.run_in && m_use_gap_wall;

    // y shift of each layer, as generate() sets it: a run-in gap is also cut into the
    // GAP_LAYERS - 1 layers below, in the same place on the bed.
    std::vector<float> y_shift(m_plan.size(), 0.f);
    {
        float shift = m_y_shift;
        for (size_t layer_id = 0; layer_id < m_plan.size(); ++layer_id) {
            const float depth = m_plan[layer_id].depth;
            if (depth >= m_perimeter_width && depth < m_wipe_tower_depth - m_perimeter_width)
                shift = align_round((m_wipe_tower_depth - depth) / 2.f, m_extra_spacing * m_perimeter_width);
            y_shift[layer_id] = shift;
        }
    }

    std::vector<unsigned int> filaments;
    for (size_t layer_id = 0; layer_id < m_plan.size(); ++layer_id) {
        WipeTowerInfo &layer     = m_plan[layer_id];
        float          traversed = 0.f; // m_depth_traversed when tool_change() starts each change
        for (WipeTowerInfo::ToolChange &tc : layer.tool_changes) {
            for (size_t f : { tc.old_tool, tc.new_tool })
                if (std::find(filaments.begin(), filaments.end(), (unsigned int) f) == filaments.end())
                    filaments.push_back((unsigned int) f);
            // tool_change() purges from the first planned change to this filament.
            float wipe_depth = tc.required_depth;
            for (const WipeTowerInfo::ToolChange &b : layer.tool_changes)
                if (b.new_tool == tc.new_tool) {
                    wipe_depth = b.required_depth;
                    break;
                }
            // The tower's first layer is never an interface (Bambu Studio: "first layer never be contact").
            tc.is_interface = layer_id != m_first_layer_idx && layer.depth >= m_perimeter_width &&
                           TowerInterface::triggers(m_interface.trigger, m_filpar[tc.old_tool].kind, m_filpar[tc.new_tool].kind);
            tc.run_in    = tc.is_interface && run_in && m_filpar[tc.new_tool].run_in_distance > EPSILON;
            if (tc.run_in) {
                // The purge starts one perimeter in from the tower's left side and runs to the right,
                // so the run-in comes in from the left along its first line.
                tc.run_in_cross = Vec2f(0.f, m_perimeter_width + traversed);
                for (int below = 0; below < TowerInterface::GAP_LAYERS && below <= int(layer_id); ++below) {
                    const size_t lower = layer_id - below;
                    m_interface_gaps[lower].push_back({ Vec2f(0.f, tc.run_in_cross.y() + y_shift[layer_id] - y_shift[lower]), true });
                }
            }
            traversed += wipe_depth;
        }
    }
    if (run_in) {
        std::vector<TowerInterface::FilamentKind> kinds;
        std::vector<float>                        distances;
        for (const FilamentParameters &fp : m_filpar) {
            kinds.push_back(fp.kind);
            distances.push_back(fp.run_in_distance);
        }
        m_run_in_reserve = float(TowerInterface::run_in_reserve(m_interface, kinds, distances, filaments, m_perimeter_width));
    }
}

bool WipeTower::print_wall_with_interface_gaps(WipeTowerWriter &writer, const box_coordinates &wt_box, float feedrate)
{
    const size_t layer_id = size_t(m_layer_info - m_plan.begin());
    if (layer_id >= m_interface_gaps.size() || m_interface_gaps[layer_id].empty())
        return false;
    const Polygon wall = m_use_rib_wall ? rib_wall_polygon(wt_box) : generate_rectange_polygon(wt_box.ld, wt_box.ru);
    if (m_use_rib_wall)
        record_outer_wall(wall);
    Polygon inserted;
    writer.wall_pieces(TowerInterface::cut_wall_gaps(wall, m_interface_gaps[layer_id], 2.5f * m_perimeter_width, inserted), feedrate);
    return true;
}

void WipeTower::interface_before_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change)
{
    const FilamentParameters &fp = m_filpar[tool_change.new_tool];
    writer.append("; tower interface\n");
    if (m_interface.temp && fp.interface_temperature != fp.nozzle_temperature)
        writer.append(GCodeWriter::set_temperature(fp.interface_temperature, m_gcode_flavor, true, fp.physical_extruder, "tower interface temperature"));

    const Vec2f start  = writer.pos();
    bool        primed = false;
    if (tool_change.run_in) {
        // Out through the gap along the purge's first line, then run in along the same line: nothing
        // is ever dragged over the wall.
        const Vec2f cross  = tool_change.run_in_cross;
        const float alpha  = m_wipe_tower_rotation_angle * float(M_PI / 180.);
        const Vec2f shift  = Vec2f(0.f, m_y_shift) + m_rib_offset; // what the writer adds (no internal rotation here)
        auto        to_bed = [alpha, shift, this](const Vec2f &p) -> Vec2f {
            return Vec2f(Eigen::Rotation2Df(alpha) * (p + shift)) + m_wipe_tower_pos;
        };
        const float dist = TowerInterface::clamp_to_bed(cross, Vec2f(-1.f, 0.f), fp.run_in_distance, to_bed, m_shared_bed);
        if (dist > EPSILON) {
            const Vec2f inside(m_perimeter_width, cross.y());
            const Vec2f outside = cross - Vec2f(dist, 0.f);
            // toolchange_Wipe() starts its first line at a third of the purge speed.
            const float speed = 0.33f * std::min(4800.f, m_max_speed);
            writer.append("; tower interface run-in\n").travel(inside, m_travel_speed * 60.f).travel(outside);
            if (m_interface.extra_prime) {
                writer.prime(fp.extra_prime_length + TowerInterface::EXTRA_PRIME_BASE, TowerInterface::PRIME_FEEDRATE);
                primed = true;
            }
            writer.extrude(inside, speed).extrude(start, speed);
        }
    }
    if (m_interface.extra_prime && ! primed)
        writer.prime(fp.extra_prime_length + TowerInterface::EXTRA_PRIME_BASE, TowerInterface::PRIME_FEEDRATE);
}

void WipeTower::interface_after_wipe(WipeTowerWriter &writer, const WipeTowerInfo::ToolChange &tool_change)
{
    const FilamentParameters &fp = m_filpar[tool_change.new_tool];
    // Back to the normal temperature; wait only if that means heating up.
    if (m_interface.temp && fp.interface_temperature != fp.nozzle_temperature)
        writer.append(GCodeWriter::set_temperature(fp.nozzle_temperature, m_gcode_flavor, fp.nozzle_temperature > fp.interface_temperature,
                                                   fp.physical_extruder, "tower interface done, normal temperature"));
}

bool WipeTower::get_floating_area(float &start_pos_y, float &end_pos_y) const {
    if (m_layer_info == m_plan.begin() || (m_layer_info - 1) == m_plan.begin())
        return false;

    float last_layer_fill_box_y = (m_layer_info - 1)->toolchanges_depth() + m_perimeter_width;
    float last_layer_wipe_depth = (m_layer_info - 1)->depth;
    if (last_layer_wipe_depth - last_layer_fill_box_y <= 2 * m_perimeter_width)
        return false;

    start_pos_y = last_layer_fill_box_y + m_perimeter_width;
    end_pos_y   = last_layer_wipe_depth - m_perimeter_width;

    return true;
}

bool WipeTower::need_thick_bridge_flow(float pos_y) const {
    if (m_extrusion_flow >= extrusion_flow(0.2))
        return false;

    float y_min = 0., y_max = 0.;
    if (get_floating_area(y_min, y_max)) {
        return pos_y > y_min && pos_y < y_max;
    }
    return false;
}

} // namespace Slic3r
