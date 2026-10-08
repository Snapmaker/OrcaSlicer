#include <assert.h>
#include <stdio.h>
#include <atomic>
#include <memory>

#include <boost/log/trivial.hpp>

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../ImageFill.hpp"
#include "../ImageRowWalls.hpp"
#include "../Layer.hpp"
#include "../MixedFilament.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include "ExtrusionEntity.hpp"
#include "Fill.hpp"
#include "FillRectilinear.hpp"
#include "FillLightning.hpp"
#include "FillConcentricInternal.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"
#include "FillConcentric.hpp"
#include "libslic3r.h"

namespace Slic3r {

// Calculate infill rotation angle (in radians) for a given layer from a rotation template.
// Grammar subset handled (rotation only):
//   [±]α[*Z or !][joint][-][N|B|T][length][* or !]
//   [±]α*                    sets an initial angle only (no layer processed)
// Where:
// - α: angle in degrees. Without a sign it's absolute; with +/− it's relative. α% means a percentage of 360°.
// - Runtime: *Z repeats the instruction Z times; bare * is a no-op used for initialization; ! runs once globally and then stops.
// - Solid signs (D,S,O,M,R) are not processed here; if present they are treated as invalid/non-rotation characters.
// - Joint signs (shape of the turn across a range):
//     / linear;
//     N,n vertical sinus (n = lazy/half amplitude);
//     Z,z horizontal sinus (z = lazy/half amplitude);
//     $ arcsin; L quarter circle H→V; l quarter circle V→H;
//     U,u squared; Q,q cubic; ~ random; ^ pseudorandom; | middle step; # vertical step at end.
// - Counting / range length:
//     After the joint (or after α) a count determines duration of the turn:
//       N = layer count, B = bottom_shell_layers, T = top_shell_layers.
//     Prefix '-' flips the joint (swap initial/final orientation).
// - Length modifiers convert the count to a Z range instead of a pure layer count:
//     mm, cm, m, ' (feet), " (inches), # (standard height of N layers), % (percent of model height).
//
// Behavior:
// - The template string is tokenized by commas/whitespace and evaluated cyclically with one or more "ranges" per token.
// - Absolute α resets the accumulated angle at the start of its range; relative α accumulates.
// - *Z and ! control repetition and one-time execution of tokens across layers.
// - If the template contains no metalanguage symbols, it is treated as a simple comma-separated list of angles repeated by modulo.
// - Returns angle in radians for the requested layer_id. 0° aligns with +X; fillers may internally rotate as needed.
double calculate_infill_rotation_angle(const PrintObject* object,
                                       size_t             layer_id,
                                       const double&      fixed_infill_angle,
                                       const std::string& template_string)
{
    if (template_string.empty()) {
        return Geometry::deg2rad(fixed_infill_angle);
    }
    double             angle = 0.0;
    ConfigOptionFloats rotate_angles;
    const std::string  search_string = "/NnZz$LlUuQq~^|#";
    if (regex_search(template_string, std::regex("[+\\-%*@\'\"cm" + search_string + "]"))) { // template metalanguage of rotating infill
        std::regex                 del("[\\s,]+");
        std::sregex_token_iterator it(template_string.begin(), template_string.end(), del, -1);
        std::vector<std::string>   tk;
        std::sregex_token_iterator end;
        while (it != end) {
            tk.push_back(*it++);
        }
        int    t            = 0;
        int    repeats      = 0;
        double angle_add    = 0;
        double angle_steps  = 1;
        double angle_start  = 0;
        double limit_fill_z = object->get_layer(0)->bottom_z();
        double start_fill_z = limit_fill_z;
        bool   _noop        = false;
        auto              fill_form = std::string::npos;
        bool              _absolute = false;
        bool              _negative = false;
        std::vector<bool> stop(tk.size(), false);

        for (int i = 0; i <= layer_id; i++) {
            double fill_z = object->get_layer(i)->bottom_z();

            if (limit_fill_z < object->get_layer(i)->slice_z) {
                if (repeats) { // if repeats >0 then restore parameters for new iteration
                    limit_fill_z += limit_fill_z - start_fill_z;
                    start_fill_z = fill_z;
                    repeats--;
                } else {
                    start_fill_z = fill_z;
                    limit_fill_z = object->get_layer(i)->print_z;
                    // Solid handling removed: this function only computes rotation.
                    fill_form    = std::string::npos;
                    do {
                        if (!stop[t]) {
                            _noop     = false;
                            _absolute = false;
                            _negative = false;
                            angle_start += angle_add;
                            angle_add   = 0;
                            angle_steps = 1;
                            repeats     = 1;
                            if (tk[t].find('!') != std::string::npos) // this is an one-time instruction
                                stop[t] = true;

                            char* cs = &tk[t][0];

                            if ((cs[0] >= '0' && cs[0] <= '9') && !(cs[0] == '+' || cs[0] == '-')) // absolute/relative
                                _absolute = true;

                            angle_add = strtod(cs, &cs); // read angle parameter

                            if (cs[0] == '%') { // percentage of angles
                                angle_add *= 3.6;
                                cs = &cs[1];
                            }

                            int tit = tk[t].find('*');
                            if (tit != std::string::npos) // overall angle_cycles
                                repeats = strtol(&tk[t][tit + 1], &cs, 0);

                            if (repeats) {                                // run if overall cycles greater than 0
                                // Solid signs (D,S,O,M,R) are not handled here; if present they behave as invalid characters.

                                if (cs[0] == 'B') {
                                    angle_steps = object->print()->default_region_config().bottom_shell_layers.value;
                                } else if (cs[0] == 'T') {
                                    angle_steps = object->print()->default_region_config().top_shell_layers.value;
                                } else {
                                    fill_form = search_string.find(cs[0]);
                                    if (fill_form != std::string::npos)
                                        cs = &cs[1];

                                    _negative   = (cs[0] == '-'); // negative parameter
                                    angle_steps = abs(strtod(cs, &cs));

                                    if (angle_steps && cs[0] != '\0' && cs[0] != '!') {
                                        if (cs[0] == '%') // value in the percents of fill_z
                                            limit_fill_z = angle_steps * object->height() * 1e-8;
                                        else if (cs[0] == '#') // value in the feet
                                            limit_fill_z = angle_steps * object->config().layer_height;
                                        else if (cs[0] == '\'') // value in the feet
                                            limit_fill_z = angle_steps * 12 * 25.4;
                                        else if (cs[0] == '\"') // value in the inches
                                            limit_fill_z = angle_steps * 25.4;
                                        else if (cs[0] == 'c') // value in centimeters
                                            limit_fill_z = angle_steps * 10.;
                                        else if (cs[0] == 'm') {
                                            if (cs[1] == 'm') { // value in the millimeters
                                                limit_fill_z = angle_steps * 1.;
                                            } else{
                                                limit_fill_z = angle_steps * 1000.;
                                            }
                                        }
                                        limit_fill_z += fill_z;
                                        angle_steps = 0; // limit_fill_z has already count
                                    }
                                }
                                if (angle_steps) { // if limit_fill_z does not setting by lenght method. Get count the layer id above model height
                                    if (fill_form == std::string::npos && !_absolute)
                                        angle_add *= (int) angle_steps;
                                    int idx      = i + std::max(angle_steps - 1, 0.);
                                    int sdx      = std::max(0, idx - (int) object->layers().size());
                                    idx          = std::min(idx, (int) object->layers().size() - 1);
                                    limit_fill_z = object->get_layer(idx)->print_z + sdx * object->config().layer_height;
                                }
                                repeats = std::max(--repeats, 0);
                            } else
                                _noop = true; // set the dumb cycle
                            if (_absolute) {  // is absolute
                                angle_start = angle_add;
                                angle_add   = 0;
                            }
                        }
                        if (++t >= tk.size())
                            t = 0;
                    } while (std::all_of(stop.begin(), stop.end(), [](bool v) { return v; }) ?
                                 false :
                                 (t ? _noop : false) || stop[t]); // if this is a dumb instruction which never reaprated twice
                }
            }
            double top_z    = object->get_layer(i)->print_z;
            double negvalue = (_negative ? limit_fill_z - top_z : top_z - start_fill_z) / (limit_fill_z - start_fill_z);

            switch (fill_form) {
            case 0: break;                                                  // /-joint, linear
            case 1: negvalue -= sin(negvalue * PI * 2.) / (PI * 2.); break; // N-joint, sinus, vertical start
            case 2: negvalue -= sin(negvalue * PI * 2.) / (PI * 4.); break; // n-joint, sinus, vertical start, lazy
            case 3: negvalue += sin(negvalue * PI * 2.) / (PI * 2.); break; // Z-joint, sinus, horizontal start
            case 4: negvalue += sin(negvalue * PI * 2.) / (PI * 4.); break; // z-joint, sinus, horizontal start, lazy
            case 5: negvalue = asin(negvalue * 2. - 1.) / PI + 0.5; break;  // $-joint, arcsin
            case 6: negvalue = sin(negvalue * PI / 2.); break;              // L-joint, quarter of circle, horizontal start
            case 7: negvalue = 1. - cos(negvalue * PI / 2.); break;         // l-joint, quarter of circle, vertical start
            case 8: negvalue = 1. - pow(1. - negvalue, 2); break;           // U-joint, squared, x2
            case 9: negvalue = pow(1 - negvalue, 2); break;                 // u-joint, squared, x2 inverse
            case 10: negvalue = 1. - pow(1. - negvalue, 3); break;          // Q-joint, cubic, x3
            case 11: negvalue = pow(1. - negvalue, 3); break;               // q-joint, cubic, x3 inverse
            case 12: negvalue = (double) rand() / RAND_MAX; break;          // ~-joint, random, fill the whole angle
            case 13: negvalue += (double) rand() / RAND_MAX - 0.5; break;   // ^-joint, pseudorandom, disperse at middle line
            case 14: negvalue = 0.5; break;                                 // |-joint, like #-joint but placed at middle angle
            case 15: negvalue = _negative ? 0. : 1.; break;                 // #-joint, vertical at the end angle
            }
            angle = Geometry::deg2rad(angle_start + angle_add * negvalue);
        }
    } else {
        rotate_angles.deserialize(template_string);
        auto rotate_angle_idx = layer_id % rotate_angles.size();
        angle                 = Geometry::deg2rad(rotate_angles.values[rotate_angle_idx]);
    }
    return angle;
}

// Orca: check if current surface is the undertop (port of OrcaSlicer#15389)
static bool is_undertop(const Layer& layer, const Surface& current_surface)
{
    for (auto region : layer.upper_layer->regions())
        for (auto surface : region->fill_surfaces.surfaces)
            if (surface.is_top() && current_surface.expolygon.overlaps(surface.expolygon))
                return true;
    return false;
}

struct SurfaceFillParams
{
	// Zero based extruder ID.
    unsigned int 	extruder = 0;
	// Infill pattern, adjusted for the density etc.
    InfillPattern  	pattern = InfillPattern(0);

    // FillBase
    // in unscaled coordinates
    coordf_t    	spacing = 0.;
    // infill / perimeter overlap, in unscaled coordinates
    coordf_t    	overlap = 0.;
    // Angle as provided by the region config, in radians.
    float       	angle = 0.f;
    // Orca: is_using_template_angle
    bool        is_using_template_angle = false;
    // Is bridging used for this fill? Bridging parameters may be used even if this->flow.bridge() is not set.
    bool 			bridge;
    // Non-negative for a bridge.
    float 			bridge_angle = 0.f;

    // FillParams
    float       	density = 0.f;
    // Infill line multiplier count.
    int   multiline = 1;
    // Don't adjust spacing to fill the space evenly.
//    bool        	dont_adjust = false;
    // Length of the infill anchor along the perimeter line.
    // 1000mm is roughly the maximum length line that fits into a 32bit coord_t.
    float 			anchor_length     = 1000.f;
    float 			anchor_length_max = 1000.f;

    // width, height of extrusion, nozzle diameter, is bridge
    // For the output, for fill generator.
    Flow 			flow;

	// For the output
    ExtrusionRole	extrusion_role = ExtrusionRole(0);

	// Various print settings?

	// Index of this entry in a linear vector.
    size_t 			idx = 0;
	// infill speed settings
	float			sparse_infill_speed = 0;
	float			top_surface_speed = 0;
	float			solid_infill_speed = 0;
    // Ultra (over-support surfaces): the flow ratio and the speed an erBottomSurfaceOverSupport
    // fill will be extruded with. They are region keys, and two regions whose surfaces would
    // otherwise be filled identically are MERGED into one fill that is attributed to the first
    // region - GCode::extrude_infill then applies that one region's config to the lot. Naming
    // them here keeps two parts with their own values apart, the way solid_infill_speed already
    // does for internal solid infill. Zero for every other role, so nothing else changes.
    float			over_support_flow  = 0;
    float			over_support_speed = 0;

    // Params for lattice infill angles
    float lateral_lattice_angle_1 = 0.f;
    float lateral_lattice_angle_2 = 0.f;
    float infill_lock_depth          = 0;
    float skin_infill_depth          = 0;
    // Locked Zag per-band patterns. ipCount = "same as the sparse infill pattern" (the default),
    // which is what Locked Zag did before these options existed. Only read inside the
    // `pattern == ipLockedZag` guard below, so every other print leaves them at ipCount and
    // neither the ordering nor the equality below sees any change.
    InfillPattern locked_skin_infill_pattern     = ipCount;
    InfillPattern locked_skeleton_infill_pattern = ipCount;
    bool symmetric_infill_y_axis = false;

    // Params for Lateral honeycomb
    float infill_overhang_angle = 60.f;

    // Phase 3 (image row, sub-triangle resolution): the CONFIGURED 1-based virtual filament id
    // (region_config.solid_infill_filament.value, before mixed-filament resolution) when this is
    // an erTopSolidInfill fill whose row is an enabled ImageWeighted MixedFilament with a
    // decodable image_fill_ref; 0 otherwise - which is every fill in every print that does not
    // use the feature, so this field is always 0 there and changes no comparison, no grouping and
    // no G-code (Bar A). It exists so two top surfaces that would otherwise share one
    // SurfaceFillParams group (same pattern/density/flow/speed/role) but read DIFFERENT images -
    // or one reads an image and the other does not - are never merged into one SurfaceFill and
    // filled under a single region_id: group_fills() picks the group's FIRST region for
    // `fill.region_id` (same as the over_support_flow/over_support_speed precedent below), and
    // Layer::make_fills's split_top_infill_by_image_row() resolves the image row from THAT
    // region's config - a wrong merge would apply one part's image to another part's geometry.
    // Unlike over_support_flow/over_support_speed this never affects WALL generation (only fills
    // are split), so it is deliberately NOT added to Layer::is_perimeter_compatible - see the
    // phase 3 doc's "why not the fourth list" note.
    int             image_row_filament_id = 0;

	bool operator<(const SurfaceFillParams &rhs) const {
#define RETURN_COMPARE_NON_EQUAL(KEY) if (this->KEY < rhs.KEY) return true; if (this->KEY > rhs.KEY) return false;
#define RETURN_COMPARE_NON_EQUAL_TYPED(TYPE, KEY) if (TYPE(this->KEY) < TYPE(rhs.KEY)) return true; if (TYPE(this->KEY) > TYPE(rhs.KEY)) return false;

		// Sort first by decreasing bridging angle, so that the bridges are processed with priority when trimming one layer by the other.
		if (this->bridge_angle > rhs.bridge_angle) return true;
		if (this->bridge_angle < rhs.bridge_angle) return false;

		RETURN_COMPARE_NON_EQUAL(extruder);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, pattern);
		RETURN_COMPARE_NON_EQUAL(spacing);
		RETURN_COMPARE_NON_EQUAL(overlap);
		RETURN_COMPARE_NON_EQUAL(angle);
		RETURN_COMPARE_NON_EQUAL(is_using_template_angle);
		RETURN_COMPARE_NON_EQUAL(density);
		RETURN_COMPARE_NON_EQUAL(multiline);
//		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, dont_adjust);
		RETURN_COMPARE_NON_EQUAL(anchor_length);
		RETURN_COMPARE_NON_EQUAL(anchor_length_max);
		RETURN_COMPARE_NON_EQUAL(flow.width());
		RETURN_COMPARE_NON_EQUAL(flow.height());
		RETURN_COMPARE_NON_EQUAL(flow.nozzle_diameter());
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, bridge);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, extrusion_role);
		RETURN_COMPARE_NON_EQUAL(sparse_infill_speed);
		RETURN_COMPARE_NON_EQUAL(top_surface_speed);
		RETURN_COMPARE_NON_EQUAL(solid_infill_speed);
		RETURN_COMPARE_NON_EQUAL(over_support_flow);
		RETURN_COMPARE_NON_EQUAL(over_support_speed);
        RETURN_COMPARE_NON_EQUAL(lateral_lattice_angle_1);
		RETURN_COMPARE_NON_EQUAL(lateral_lattice_angle_2);
		RETURN_COMPARE_NON_EQUAL(symmetric_infill_y_axis);
		RETURN_COMPARE_NON_EQUAL(infill_lock_depth);
		RETURN_COMPARE_NON_EQUAL(skin_infill_depth);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, locked_skin_infill_pattern);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, locked_skeleton_infill_pattern);
		RETURN_COMPARE_NON_EQUAL(infill_overhang_angle);
		RETURN_COMPARE_NON_EQUAL(image_row_filament_id);

		return false;
	}

	bool operator==(const SurfaceFillParams &rhs) const {
		return  this->extruder 			== rhs.extruder 		&&
				this->pattern 			== rhs.pattern 			&&
				this->spacing 			== rhs.spacing 			&&
				this->overlap 			== rhs.overlap 			&&
				this->angle   			== rhs.angle   			&&
				this->is_using_template_angle == rhs.is_using_template_angle &&
				this->bridge   			== rhs.bridge   		&&
				this->bridge_angle 		== rhs.bridge_angle		&&
				this->density   		== rhs.density   		&&
				this->multiline             == rhs.multiline    &&
//				this->dont_adjust   	== rhs.dont_adjust 		&&
				this->anchor_length  	== rhs.anchor_length    &&
				this->anchor_length_max == rhs.anchor_length_max &&
				this->flow 				== rhs.flow 			&&
				this->extrusion_role	== rhs.extrusion_role	&&
				this->sparse_infill_speed	== rhs.sparse_infill_speed &&
				this->top_surface_speed		== rhs.top_surface_speed &&
				this->solid_infill_speed	== rhs.solid_infill_speed &&
				this->over_support_flow		== rhs.over_support_flow &&
				this->over_support_speed	== rhs.over_support_speed &&
                this->lateral_lattice_angle_1		== rhs.lateral_lattice_angle_1 &&
				this->lateral_lattice_angle_2	    == rhs.lateral_lattice_angle_2 &&
				this->infill_lock_depth      ==  rhs.infill_lock_depth &&
				this->skin_infill_depth      ==  rhs.skin_infill_depth &&
				this->locked_skin_infill_pattern     == rhs.locked_skin_infill_pattern &&
				this->locked_skeleton_infill_pattern == rhs.locked_skeleton_infill_pattern &&
                this->infill_overhang_angle == rhs.infill_overhang_angle &&
                this->image_row_filament_id == rhs.image_row_filament_id;
	}
};

struct SurfaceFill {
	SurfaceFill(const SurfaceFillParams& params) : region_id(size_t(-1)), surface(stCount, ExPolygon()), params(params) {}

	size_t 				region_id;
	Surface 			surface;
	ExPolygons       	expolygons;
	SurfaceFillParams	params;
    // BBS
    std::vector<size_t> region_id_group;
    ExPolygons          no_overlap_expolygons;
};


// Detect narrow infill regions
// Based on the anti-vibration algorithm from PrusaSlicer:
// https://github.com/prusa3d/PrusaSlicer/blob/5dc04b4e8f14f65bbcc5377d62cad3e86c2aea36/src/libslic3r/Fill/FillEnsuring.cpp#L37-L273

static coord_t _MAX_LINE_LENGTH_TO_FILTER() // 4 mm.
{
    return scaled<coord_t>(4.);
}
const constexpr size_t  MAX_SKIPS_ALLOWED           = 2; // Skip means propagation through long line.
const constexpr size_t  MIN_DEPTH_FOR_LINE_REMOVING = 5;

struct LineNode
{
    struct State
    {
        // The total number of long lines visited before this node was reached.
        // We just need the minimum number of all possible paths to decide whether we can remove the line or not.
        int min_skips_taken             = 0;
        // The total number of short lines visited before this node was reached.
        int total_short_lines           = 0;
        // Some initial line is touching some long line. This information is propagated to neighbors.
        bool initial_touches_long_lines = false;
        bool initialized                = false;

        void reset() {
            this->min_skips_taken            = 0;
            this->total_short_lines          = 0;
            this->initial_touches_long_lines = false;
            this->initialized                = false;
        }
    };

    explicit LineNode(const Line &line) : line(line) {}

    Line                   line;
    // Pointers to line nodes in the previous and the next section that overlap with this line.
    std::vector<LineNode*> next_section_overlapping_lines;
    std::vector<LineNode*> prev_section_overlapping_lines;

    bool                   is_removed = false;

    State                  state;

    // Return true if some initial line is touching some long line and this information was propagated into the current line.
    bool is_initial_line_touching_long_lines() const {
        if (prev_section_overlapping_lines.empty())
            return false;

        for (LineNode *line_node : prev_section_overlapping_lines) {
            if (line_node->state.initial_touches_long_lines)
                return true;
        }

        return false;
    }

    // Return true if the current line overlaps with some long line in the previous section.
    bool is_touching_long_lines_in_previous_layer() const {
        if (prev_section_overlapping_lines.empty())
            return false;

        const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
        for (LineNode *line_node : prev_section_overlapping_lines) {
            if (!line_node->is_removed && line_node->line.length() >= MAX_LINE_LENGTH_TO_FILTER)
                return true;
        }

        return false;
    }

    // Return true if the current line overlaps with some line in the next section.
    bool has_next_layer_neighbours() const {
        if (next_section_overlapping_lines.empty())
            return false;

        for (LineNode *line_node : next_section_overlapping_lines) {
            if (!line_node->is_removed)
                return true;
        }

        return false;
    }
};

using LineNodes = std::vector<LineNode>;

inline bool are_lines_overlapping_in_y_axes(const Line &first_line, const Line &second_line) {
    return (second_line.a.y() <= first_line.a.y() && first_line.a.y() <= second_line.b.y())
        || (second_line.a.y() <= first_line.b.y() && first_line.b.y() <= second_line.b.y())
        || (first_line.a.y() <= second_line.a.y() && second_line.a.y() <= first_line.b.y())
        || (first_line.a.y() <= second_line.b.y() && second_line.b.y() <= first_line.b.y());
}

bool can_line_note_be_removed(const LineNode &line_node) {
    const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
    return (line_node.line.length() < MAX_LINE_LENGTH_TO_FILTER)
        && (line_node.state.total_short_lines > int(MIN_DEPTH_FOR_LINE_REMOVING)
            || (!line_node.is_initial_line_touching_long_lines() && !line_node.has_next_layer_neighbours()));
}

// Remove the node and propagate its removal to the previous sections.
void propagate_line_node_remove(const LineNode &line_node) {
    std::queue<LineNode *> line_node_queue;
    for (LineNode *prev_line : line_node.prev_section_overlapping_lines) {
        if (prev_line->is_removed)
            continue;

        line_node_queue.emplace(prev_line);
    }

    for (; !line_node_queue.empty(); line_node_queue.pop()) {
        LineNode &line_to_check = *line_node_queue.front();

        if (can_line_note_be_removed(line_to_check)) {
            line_to_check.is_removed = true;

            for (LineNode *prev_line : line_to_check.prev_section_overlapping_lines) {
                if (prev_line->is_removed)
                    continue;

                line_node_queue.emplace(prev_line);
            }
        }
    }
}

// Filter out short extrusions that could create vibrations.
static std::vector<Lines> filter_vibrating_extrusions(const std::vector<Lines> &lines_sections) {
    // Initialize all line nodes.
    std::vector<LineNodes> line_nodes_sections(lines_sections.size());
    for (const Lines &lines_section : lines_sections) {
        const size_t section_idx = &lines_section - lines_sections.data();

        line_nodes_sections[section_idx].reserve(lines_section.size());
        for (const Line &line : lines_section) {
            line_nodes_sections[section_idx].emplace_back(line);
        }
    }

    // Precalculate for each line node which line nodes in the previous and next section this line node overlaps.
    for (auto curr_lines_section_it = line_nodes_sections.begin(); curr_lines_section_it != line_nodes_sections.end(); ++curr_lines_section_it) {
        if (curr_lines_section_it != line_nodes_sections.begin()) {
            const auto prev_lines_section_it = std::prev(curr_lines_section_it);
            for (LineNode &curr_line : *curr_lines_section_it) {
                for (LineNode &prev_line : *prev_lines_section_it) {
                    if (are_lines_overlapping_in_y_axes(curr_line.line, prev_line.line)) {
                        curr_line.prev_section_overlapping_lines.emplace_back(&prev_line);
                    }
                }
            }
        }

        if (std::next(curr_lines_section_it) != line_nodes_sections.end()) {
            const auto next_lines_section_it = std::next(curr_lines_section_it);
            for (LineNode &curr_line : *curr_lines_section_it) {
                for (LineNode &next_line : *next_lines_section_it) {
                    if (are_lines_overlapping_in_y_axes(curr_line.line, next_line.line)) {
                        curr_line.next_section_overlapping_lines.emplace_back(&next_line);
                    }
                }
            }
        }
    }

    const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
    // Select each section as the initial lines section and propagate line node states from this initial lines section to the last lines section.
    // During this propagation, we remove those lines that meet the conditions for its removal.
    // When some line is removed, we propagate this removal to previous layers.
    for (size_t initial_line_section_idx = 0; initial_line_section_idx < line_nodes_sections.size(); ++initial_line_section_idx) {
        // Stars from non-removed short lines.
        for (LineNode &initial_line : line_nodes_sections[initial_line_section_idx]) {
            if (initial_line.is_removed || initial_line.line.length() >= MAX_LINE_LENGTH_TO_FILTER)
                continue;

            initial_line.state.reset();
            initial_line.state.total_short_lines          = 1;
            initial_line.state.initial_touches_long_lines = initial_line.is_touching_long_lines_in_previous_layer();
            initial_line.state.initialized                = true;
        }

        // Iterate from the initial lines section until the last lines section.
        for (size_t propagation_line_section_idx = initial_line_section_idx; propagation_line_section_idx < line_nodes_sections.size(); ++propagation_line_section_idx) {
            // Before we propagate node states into next lines sections, we reset the state of all line nodes in the next line section.
            if (propagation_line_section_idx + 1 < line_nodes_sections.size()) {
                for (LineNode &propagation_line : line_nodes_sections[propagation_line_section_idx + 1]) {
                    propagation_line.state.reset();
                }
            }

            for (LineNode &propagation_line : line_nodes_sections[propagation_line_section_idx]) {
                if (propagation_line.is_removed || !propagation_line.state.initialized)
                    continue;

                for (LineNode *neighbour_line : propagation_line.next_section_overlapping_lines) {
                    if (neighbour_line->is_removed)
                        continue;

                    const bool is_short_line   = neighbour_line->line.length() < MAX_LINE_LENGTH_TO_FILTER;
                    const bool is_skip_allowed = propagation_line.state.min_skips_taken < int(MAX_SKIPS_ALLOWED);

                    if (!is_short_line && !is_skip_allowed)
                        continue;

                    const int neighbour_total_short_lines = propagation_line.state.total_short_lines + int(is_short_line);
                    const int neighbour_min_skips_taken   = propagation_line.state.min_skips_taken + int(!is_short_line);

                    if (neighbour_line->state.initialized) {
                        // When the state of the node was previously filled, then we need to update data in such a way
                        // that will maximize the possibility of removing this node.
                        neighbour_line->state.min_skips_taken = std::max(neighbour_line->state.min_skips_taken, neighbour_total_short_lines);
                        neighbour_line->state.min_skips_taken = std::min(neighbour_line->state.min_skips_taken, neighbour_min_skips_taken);

                        // We will keep updating neighbor initial_touches_long_lines until it is equal to false.
                        if (neighbour_line->state.initial_touches_long_lines) {
                            neighbour_line->state.initial_touches_long_lines = propagation_line.state.initial_touches_long_lines;
                        }
                    } else {
                        neighbour_line->state.total_short_lines          = neighbour_total_short_lines;
                        neighbour_line->state.min_skips_taken            = neighbour_min_skips_taken;
                        neighbour_line->state.initial_touches_long_lines = propagation_line.state.initial_touches_long_lines;
                        neighbour_line->state.initialized                = true;
                    }
                }

                if (can_line_note_be_removed(propagation_line)) {
                    // Remove the current node and propagate its removal to the previous sections.
                    propagation_line.is_removed = true;
                    propagate_line_node_remove(propagation_line);
                }
            }
        }
    }

    // Create lines sections without filtered-out lines.
    std::vector<Lines> lines_sections_out(line_nodes_sections.size());
    for (const std::vector<LineNode> &line_nodes_section : line_nodes_sections) {
        const size_t section_idx = &line_nodes_section - line_nodes_sections.data();

        for (const LineNode &line_node : line_nodes_section) {
            if (!line_node.is_removed) {
                lines_sections_out[section_idx].emplace_back(line_node.line);
            }
        }
    }

    return lines_sections_out;
}

void split_solid_surface(size_t layer_id, const SurfaceFill &fill, ExPolygons &normal_infill, ExPolygons &narrow_infill)
{
    assert(fill.surface.surface_type == stInternalSolid);

    const bool line_based_pattern =
        fill.params.pattern == ipRectilinear || fill.params.pattern == ipMonotonic ||
        fill.params.pattern == ipMonotonicLine || fill.params.pattern == ipAlignedRectilinear;

    // ORCA: For non-line patterns, split by a geometric "core" so only thin areas get rerouted.
    if (!line_based_pattern) {
        const coord_t scaled_spacing = scaled<coord_t>(fill.params.spacing);

        for (const ExPolygon &expolygon : fill.expolygons) {
            Polygons filled_area = to_polygons(expolygon);

            // "Core" area: open (erode+dilate) to drop thin features, then clamp back to the original polygon.
            Polygons inner_area  = intersection(filled_area, opening(filled_area, scaled_spacing, scaled_spacing));

            if (inner_area.empty()) {
                narrow_infill.emplace_back(expolygon);
                continue;
            }

            ExPolygons inner_ex = union_ex(inner_area);
            ExPolygons expolys{expolygon};
            ExPolygons narrow_ex = diff_ex(expolys, inner_ex);
            ExPolygons normal_ex = intersection_ex(expolys, inner_ex);

            append(normal_infill, normal_ex); // normal infill area
            append(narrow_infill, narrow_ex); // narrow infill area
        }

        return;
    }

    Polygons normal_fill_areas;  // Areas that filled with normal infill

    constexpr double connect_extrusions = true;

    const coord_t scaled_spacing                      = scaled<coord_t>(fill.params.spacing);
    double        distance_limit_reconnection         = 2.0 * double(scaled_spacing);
    double        squared_distance_limit_reconnection = distance_limit_reconnection * distance_limit_reconnection;
    // Calculate infill direction, see Fill::_infill_direction
    double        base_angle                          = fill.params.angle + float(M_PI / 2.);
    // For pattern other than ipAlignedRectilinear, the angle are alternated
    if (fill.params.pattern != ipAlignedRectilinear) {
        size_t idx = layer_id / fill.surface.thickness_layers;
        base_angle += (idx & 1) ? float(M_PI / 2.) : 0;
    }
    const double aligning_angle = -base_angle + PI;

	for (const ExPolygon &expolygon : fill.expolygons) {
        Polygons filled_area = to_polygons(expolygon);
        polygons_rotate(filled_area, aligning_angle);
        BoundingBox bb = get_extents(filled_area);

        Polygons inner_area = intersection(filled_area, opening(filled_area, 2 * scaled_spacing, 3 * scaled_spacing));

        inner_area = shrink(inner_area, scaled_spacing * 0.5 - scaled<double>(fill.params.overlap));

        AABBTreeLines::LinesDistancer<Line> area_walls{to_lines(inner_area)};

        const size_t  n_vlines = (bb.max.x() - bb.min.x() + scaled_spacing - 1) / scaled_spacing;
        const coord_t y_min    = bb.min.y();
        const coord_t y_max    = bb.max.y();
        Lines         vertical_lines(n_vlines);
        for (size_t i = 0; i < n_vlines; i++) {
            coord_t x           = bb.min.x() + i * double(scaled_spacing);
            vertical_lines[i].a = Point{x, y_min};
            vertical_lines[i].b = Point{x, y_max};
        }

        if (!vertical_lines.empty()) {
            vertical_lines.push_back(vertical_lines.back());
            vertical_lines.back().a = Point{coord_t(bb.min.x() + n_vlines * double(scaled_spacing) + scaled_spacing * 0.5), y_min};
            vertical_lines.back().b = Point{vertical_lines.back().a.x(), y_max};
        }

        std::vector<Lines> polygon_sections(n_vlines);

        for (size_t i = 0; i < n_vlines; i++) {
            const auto intersections = area_walls.intersections_with_line<true>(vertical_lines[i]);

            for (int intersection_idx = 0; intersection_idx < int(intersections.size()) - 1; intersection_idx++) {
                const auto &a = intersections[intersection_idx];
                const auto &b = intersections[intersection_idx + 1];
                if (area_walls.outside((a.first + b.first) / 2) < 0) {
                    if (std::abs(a.first.y() - b.first.y()) > scaled_spacing) {
                        polygon_sections[i].emplace_back(a.first, b.first);
                    }
                }
            }
        }

        polygon_sections = filter_vibrating_extrusions(polygon_sections);

        Polygons reconstructed_area{};
        // reconstruct polygon from polygon sections
        {
            struct TracedPoly
            {
                Points lows;
                Points highs;
            };

            std::vector<std::vector<Line>> polygon_sections_w_width = polygon_sections;
            for (auto &slice : polygon_sections_w_width) {
                for (Line &l : slice) {
                    l.a -= Point{0.0, 0.5 * scaled_spacing};
                    l.b += Point{0.0, 0.5 * scaled_spacing};
                }
            }

            std::vector<TracedPoly> current_traced_polys;
            for (const auto &polygon_slice : polygon_sections_w_width) {
                std::unordered_set<const Line *> used_segments;
                for (TracedPoly &traced_poly : current_traced_polys) {
                    auto candidates_begin = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.lows.back(),
                                                             [](const Point &low, const Line &seg) { return seg.b.y() > low.y(); });
                    auto candidates_end   = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.highs.back(),
                                                             [](const Point &high, const Line &seg) { return seg.a.y() > high.y(); });

                    bool segment_added = false;
                    for (auto candidate = candidates_begin; candidate != candidates_end && !segment_added; candidate++) {
                        if (used_segments.find(&(*candidate)) != used_segments.end()) {
                            continue;
                        }
                        if (connect_extrusions && (traced_poly.lows.back() - candidates_begin->a).cast<double>().squaredNorm() <
                                                      squared_distance_limit_reconnection) {
                            traced_poly.lows.push_back(candidates_begin->a);
                        } else {
                            traced_poly.lows.push_back(traced_poly.lows.back() + Point{scaled_spacing / 2, coord_t(0)});
                            traced_poly.lows.push_back(candidates_begin->a - Point{scaled_spacing / 2, 0});
                            traced_poly.lows.push_back(candidates_begin->a);
                        }

                        if (connect_extrusions && (traced_poly.highs.back() - candidates_begin->b).cast<double>().squaredNorm() <
                                                      squared_distance_limit_reconnection) {
                            traced_poly.highs.push_back(candidates_begin->b);
                        } else {
                            traced_poly.highs.push_back(traced_poly.highs.back() + Point{scaled_spacing / 2, 0});
                            traced_poly.highs.push_back(candidates_begin->b - Point{scaled_spacing / 2, 0});
                            traced_poly.highs.push_back(candidates_begin->b);
                        }
                        segment_added = true;
                        used_segments.insert(&(*candidates_begin));
                    }

                    if (!segment_added) {
                        // Zero or multiple overlapping segments. Resolving this is nontrivial,
                        // so we just close this polygon and maybe open several new. This will hopefully happen much less often
                        traced_poly.lows.push_back(traced_poly.lows.back() + Point{scaled_spacing / 2, 0});
                        traced_poly.highs.push_back(traced_poly.highs.back() + Point{scaled_spacing / 2, 0});
                        Polygon &new_poly = reconstructed_area.emplace_back(std::move(traced_poly.lows));
                        new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
                        traced_poly.lows.clear();
                        traced_poly.highs.clear();
                    }
                }

                current_traced_polys.erase(std::remove_if(current_traced_polys.begin(), current_traced_polys.end(),
                                                          [](const TracedPoly &tp) { return tp.lows.empty(); }),
                                           current_traced_polys.end());

                for (const auto &segment : polygon_slice) {
                    if (used_segments.find(&segment) == used_segments.end()) {
                        TracedPoly &new_tp = current_traced_polys.emplace_back();
                        new_tp.lows.push_back(segment.a - Point{scaled_spacing / 2, 0});
                        new_tp.lows.push_back(segment.a);
                        new_tp.highs.push_back(segment.b - Point{scaled_spacing / 2, 0});
                        new_tp.highs.push_back(segment.b);
                    }
                }
            }

            // add not closed polys
            for (TracedPoly &traced_poly : current_traced_polys) {
                Polygon &new_poly = reconstructed_area.emplace_back(std::move(traced_poly.lows));
                new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
            }
        }

        polygons_append(normal_fill_areas, reconstructed_area);
    }

    polygons_rotate(normal_fill_areas, -aligning_angle);

    // Do the split
    ExPolygons normal_fill_areas_ex = union_safety_offset_ex(normal_fill_areas);
    ExPolygons narrow_fill_areas    = diff_ex(fill.expolygons, normal_fill_areas_ex);

    // Merge very small areas that is smaller than a single line width to the normal infill if they touches
    for (auto iter = narrow_fill_areas.begin(); iter != narrow_fill_areas.end();) {
        auto shrinked_expoly = offset_ex(*iter, -scaled_spacing * 0.5);
        if (shrinked_expoly.empty()) {
            // Too small! Check if it touches any normal infills
            auto     expanede_exploy          = offset_ex(*iter, scaled_spacing * 0.3);
            Polygons normal_fill_area_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(normal_fill_areas_ex, get_extents(expanede_exploy));
            auto     touch_check              = intersection_ex(normal_fill_area_clipped, expanede_exploy);
            if (!touch_check.empty()) {
                normal_fill_areas_ex.emplace_back(*iter);
                iter = narrow_fill_areas.erase(iter);
                continue;
            }
        }
        iter++;
    }

    if (narrow_fill_areas.empty()) {
        // No split needed
        return;
    }

    // Expand the normal infills a little bit to avoid gaps between normal and narrow infills
    normal_infill = intersection_ex(offset_ex(normal_fill_areas_ex, scaled_spacing * 0.1), fill.expolygons);
    narrow_infill = narrow_fill_areas;

#ifdef DEBUG_SURFACE_SPLIT
    {
        BoundingBox bbox   = get_extents(fill.expolygons);
        bbox.offset(scale_(1.));
        ::Slic3r::SVG svg(debug_out_path("surface_split_%d.svg", layer_id), bbox);
        svg.draw(to_lines(fill.expolygons), "red", scale_(0.1));
        svg.draw(normal_infill, "blue", 0.5);
        svg.draw(narrow_infill, "green", 0.5);
        svg.Close();
    }
#endif
}

// =================================================================================================
// Phase 3 (image row, sub-triangle resolution), step 4: wiring the sampler and the dither
// (ImageFill.hpp, steps 2-3) into top-solid-infill fill generation.
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase3-imagerow.md, step 4.
//
// WHERE THIS RUNS: erTopSolidInfill only (not walls, not ironing yet - see the phase 3 doc's
// "what step 5 still needs"), and only when region_config.solid_infill_filament resolves through
// MixedFilamentManager to an ENABLED row whose distribution_mode is ImageWeighted and whose
// image_fill_ref decodes. Every other fill - which is every fill in a print that does not use
// this feature - takes none of the branches below, so Bar A stays byte-identical.
//
// COORDINATE SPACES: fill polylines live in the PrintObject's own working space (what
// PrintObject::trafo_centered() maps INTO); image_fill_project()'s `box`/`p`/`facet_normal`
// are in the volume's own MESH space (what ModelVolume::mesh() is expressed in - see
// image_fill_apply(), ImageFill.cpp, which passes volume.mesh().its straight through). This file
// applies (trafo_centered() * mv->get_matrix()).inverse() to go from a fill point back to mesh
// space - see the step 5 update below for why mv->get_matrix() is now part of that composition.
//
// STEP 5 UPDATE: the identity-matrix assumption above described step 4's state. It no longer
// holds - image_row_owning_volume() below resolves the ACTUAL ModelVolume backing a given
// PrintRegion (via PrintObjectRegions::layer_ranges[].volume_regions, the same map
// PrintObjectRegions builds for painted regions) instead of always taking the object's first
// model-part volume, and image_row_context_for_region() folds that volume's OWN local matrix
// (ModelVolume::get_matrix()) into the mesh<-print transform, the same composition
// PrintObject.cpp already uses for facet-modifier slicing (trafo_centered() * mv->get_matrix(),
// see PrintObject.cpp's slice_mesh_slabs() calls). A multi-volume object now resolves each
// LayerRegion against its own owning volume; a volume moved/rotated independently of its object
// (a non-identity ModelVolume::get_matrix()) is now sampled correctly instead of silently
// degrading to the un-split resolve() cycle. NOT fixed by this change (see the phase 3 doc's own
// "second, related simplification" note): sample spacing and run-length cuts still treat mesh-
// space arc length as equal to print-space arc length, which is only exact for a uniform-scale
// transform - a non-uniformly scaled volume or instance would still sample at a slightly wrong
// density and cut runs at slightly wrong lengths, though it will not crash or land colours
// grossly wrong. Tests: tests/libslic3r/test_image_row_transform.cpp.
namespace {

// Step 5, item 3: ironing and Arachne/Concentric-family top surfaces still fall back silently to
// the row's un-split resolve() cycle (see the phase 3 doc's "what is NOT wired" section - neither
// was touched by this session, deliberately: ironing's own fill loop has different geometry
// semantics from make_fills()'s, and a Concentric-family top surface never produces a plain
// ExtrusionPath for split_top_infill_by_image_row() to cut). The GUI checkbox's own tooltip says
// "top solid infill only" for exactly this reason (ImageFillDialog.cpp) - these two helpers turn
// that same fact into one log line per case, the first time it is actually hit, so a user or a
// future maintainer looking at the log (not just the tooltip) can tell the fallback fired.
void log_image_row_ironing_not_split_once()
{
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        BOOST_LOG_TRIVIAL(warning) << "Image row (ImageWeighted top-solid-infill dithering): ironing is not "
                                       "sub-triangle-resolved - an ironed surface prints with the row's own "
                                       "per-layer colour cycle instead. See docs/superpowers/specs/"
                                       "2026-09-07-imagemap-phase3-imagerow.md, step 5.";
}

void log_image_row_pattern_not_split_once()
{
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        BOOST_LOG_TRIVIAL(warning) << "Image row (ImageWeighted top-solid-infill dithering): this top surface's "
                                       "fill pattern does not produce a plain extrusion path (Concentric-family "
                                       "or another Arachne-driven pattern) - it is not sub-triangle-resolved and "
                                       "prints with the row's own per-layer colour cycle instead. Rectilinear and "
                                       "Monotonic top surfaces are unaffected. See docs/superpowers/specs/"
                                       "2026-09-07-imagemap-phase3-imagerow.md, step 5.";
}

// The cheap half: does this region's top-solid-infill filament name an ImageWeighted row at all?
// Returns the CONFIGURED (pre-resolution) 1-based virtual id, or 0. Used by group_fills() as the
// SurfaceFillParams key (image_row_filament_id) so two top surfaces reading different images -
// or one image and one plain filament - are never merged into one fill pass; see that field's own
// comment for why a wrong merge would be a real correctness bug, not just a missed optimisation.
unsigned int image_row_configured_virtual_id(const PrintObject &object, const PrintRegionConfig &region_config)
{
    const Print *print = object.print();
    if (print == nullptr)
        return 0;
    const size_t num_physical = print->config().filament_diameter.size();
    const unsigned int configured_id = unsigned(std::max(0, region_config.solid_infill_filament.value));
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    if (!mixed_mgr.is_mixed(configured_id, num_physical))
        return 0;
    const MixedFilament *mf = mixed_mgr.mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr || !mf->enabled || mf->distribution_mode != int(MixedFilament::ImageWeighted) || mf->image_fill_ref.empty())
        return 0;
    return configured_id;
}

// Step 5: resolves the ACTUAL ModelVolume that backs `region` on `object`, using the same
// volume<->region map PrintObjectRegions already builds for painted-region resolution
// (layer_ranges[].volume_regions - one entry per (layer range, source ModelVolume), each
// carrying the PrintRegion pointer it was clipped/merged into). Falls back to the object's first
// model-part volume when the map is unavailable or does not (yet) know this region - e.g. a
// PrintObject whose shared_regions() has not been (re)built at the point this is called - which
// reproduces step 4's original behaviour exactly for the single-volume case that behaviour was
// verified against.
const ModelVolume *image_row_owning_volume(const PrintObject &object, const PrintRegion &region)
{
    if (const PrintObjectRegions *shared = object.shared_regions(); shared != nullptr)
        for (const PrintObjectRegions::LayerRangeRegions &range : shared->layer_ranges)
            for (const PrintObjectRegions::VolumeRegion &vr : range.volume_regions)
                if (vr.region == &region && vr.model_volume != nullptr && vr.model_volume->is_model_part())
                    return vr.model_volume;
    const ModelObject *mo = object.model_object();
    if (mo != nullptr)
        for (const ModelVolume *v : mo->volumes)
            if (v != nullptr && v->is_model_part())
                return v;
    return nullptr;
}

// Everything image_fill_sample_segment()/image_fill_dither_segment() need for one region's top
// solid infill, built once per SurfaceFill (not per polyline). false means "not an image row here"
// (row missing/disabled/undecodable, fewer than 2 usable candidates, or no model-part volume
// found) and leaves `ctx` untouched - the caller then falls back to the ORIGINAL, unsplit fill.
struct ImageRowContext
{
    const MixedFilament             *row = nullptr;
    ImageFillParams                  params;
    std::vector<int>                 candidate_ids;       // physical, 1-based
    std::vector<std::array<float,3>> candidate_colors;     // parallel to candidate_ids, sRGB 0..1
    BoundingBoxf3                    mesh_box;
    Transform3d                      mesh_from_print = Transform3d::Identity();
    Vec3f                            facet_normal_mesh { 0.f, 0.f, 1.f };
    float                            min_run_len_mm = 0.4f;
    float                            sample_spacing_mm = 0.4f;
};

bool image_row_context_for_region(const PrintObject &object, const PrintRegion &region,
                                  float flow_width_mm, ImageRowContext &ctx)
{
    const PrintRegionConfig &region_config = region.config();
    const Print *print = object.print();
    if (print == nullptr)
        return false;
    const size_t num_physical = print->config().filament_diameter.size();
    const unsigned int configured_id = image_row_configured_virtual_id(object, region_config);
    if (configured_id == 0)
        return false;
    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    const MixedFilament *mf = mixed_mgr.mixed_filament_from_id(configured_id, num_physical);
    if (mf == nullptr)
        return false; // image_row_configured_virtual_id already checked this; defensive only.

    const std::string raw = MixedFilamentManager::decode_image_fill_ref(mf->image_fill_ref);
    if (raw.empty() || !ImageFillParams::from_string(raw, ctx.params))
        return false;

    // The row's allowed filaments: reuse gradient_component_ids (already decoded elsewhere for
    // the layer-cycle gradient sequence) rather than adding a parallel list - an ImageWeighted
    // row's "allowed" set IS that field, just consumed differently. Fall back to the plain A/B
    // pair for a row that was never given 3+ ids.
    std::vector<unsigned int> allowed = MixedFilamentManager::decode_gradient_component_ids(mf->gradient_component_ids, num_physical);
    if (allowed.size() < 2)
        allowed = {mf->component_a, mf->component_b};

    const std::vector<std::string> &filament_colours = print->config().filament_colour.values;
    for (unsigned int id : allowed) {
        if (id < 1 || id > num_physical || id > filament_colours.size())
            continue;
        ctx.candidate_ids.push_back(int(id));
        ctx.candidate_colors.push_back(MixedFilamentManager::hex_to_srgb01(filament_colours[id - 1]));
    }
    if (ctx.candidate_ids.size() < 2)
        return false;

    // Step 5: resolve the volume that ACTUALLY backs this region (multi-volume objects each get
    // their own volume here), not just "the object's first model-part volume".
    const ModelVolume *mv = image_row_owning_volume(object, region);
    if (mv == nullptr)
        return false;

    // Same box image_fill_compute() itself computes (ImageFill.cpp) - from the raw mesh, so a
    // point that lands in a facet's own plane samples exactly where Phase 2's facet painting did.
    for (const Vec3f &v : mv->mesh().its.vertices)
        ctx.mesh_box.merge(v.cast<double>());
    if (!ctx.mesh_box.defined)
        return false;

    // Mesh space <- print space: the object's own placement (trafo_centered(), which already
    // folds in the shared instance rotation/scale that every instance sharing this PrintObject
    // has in common - only per-instance translation is handled separately, at G-code emission)
    // composed with THIS VOLUME's own local matrix, exactly as PrintObject's own facet-modifier
    // slicing composes them (see PrintObject.cpp's slice_mesh_slabs() calls: trafo_centered() *
    // mv->get_matrix()). A volume moved/rotated/scaled independently of its object is now sampled
    // in the right place instead of silently falling back to the un-split resolve() cycle.
    ctx.mesh_from_print = (object.trafo_centered() * mv->get_matrix()).inverse();
    const Vec3f up_mesh = (ctx.mesh_from_print.linear().cast<float>() * Vec3f(0.f, 0.f, 1.f));
    ctx.facet_normal_mesh = up_mesh.norm() > 1e-9f ? Vec3f(up_mesh.normalized()) : Vec3f(0.f, 0.f, 1.f);

    ctx.row = mf;
    // Resolution is bounded by extrusion width, not by an arbitrary constant: a run shorter than
    // the nozzle can print is folded into a neighbour by image_fill_dither_segment() itself.
    const float w = flow_width_mm > 0.05f ? flow_width_mm : 0.4f;
    ctx.min_run_len_mm    = w;
    ctx.sample_spacing_mm = w;
    return true;
}

// Samples one straight ExtrusionPath's polyline through the image row and dithers it into runs.
// `print_z` is the layer's Z in the SAME mm units as the polyline's unscaled X/Y (print space).
// Returns empty when the path is degenerate (fewer than 2 points) or every sample declined a
// colour (image_fill_project() failing along the whole path - a cylindrical projection exactly on
// axis, say): the caller then keeps the path unsplit.
std::vector<ImageRowRun> image_row_runs_for_path(const ImageAssetStore &assets, const ImageRowContext &ctx,
                                                 const Polyline &polyline, double print_z)
{
    const Points &pts = polyline.points;
    if (pts.size() < 2)
        return {};

    std::vector<ImageRowSample> samples;
    float cumulative = 0.f;
    bool  any_color   = false;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Vec3d p0_print(unscale<double>(pts[i].x()),     unscale<double>(pts[i].y()),     print_z);
        const Vec3d p1_print(unscale<double>(pts[i + 1].x()), unscale<double>(pts[i + 1].y()), print_z);
        const Vec3f p0_mesh = (ctx.mesh_from_print * p0_print).cast<float>();
        const Vec3f p1_mesh = (ctx.mesh_from_print * p1_print).cast<float>();

        std::vector<ImageRowSample> seg = image_fill_sample_segment(ctx.params, ctx.mesh_box, assets, p0_mesh, p1_mesh,
                                                                     ctx.facet_normal_mesh, ctx.sample_spacing_mm);
        // seg[0] duplicates the previous segment's last sample (same physical point) - skip it
        // past the first segment so a point shared by two consecutive fill segments is sampled
        // exactly once, at one arc-length position, not twice at the same position under two
        // different running error-diffusion states.
        for (size_t k = (i == 0 ? 0 : 1); k < seg.size(); ++k) {
            ImageRowSample s = seg[k];
            s.s += cumulative;
            any_color = any_color || s.has_color;
            samples.push_back(s);
        }
        if (!seg.empty())
            cumulative += seg.back().s;
    }
    if (!any_color)
        return {};

    return image_fill_dither_segment(samples, ctx.candidate_colors, ctx.candidate_ids, ctx.min_run_len_mm);
}

// Cuts `src`'s polyline into `runs`' pieces (arc length in mm, print space - see the caller's
// stated assumption that mesh-space and print-space arc length agree, i.e. no non-uniform scale)
// and returns one new heap ExtrusionPath per run, in the SAME order as `runs`. Copies every
// attribute of `src` (role, flow, width, height, z_offset, extrusion_multiplier, reversibility)
// via ExtrusionPath's own copy constructor, so a run differs from the original path ONLY in its
// polyline - it prints at the same width, height and speed the un-split path would have.
std::vector<ExtrusionPath *> split_extrusion_path_by_runs(const ExtrusionPath &src, const std::vector<ImageRowRun> &runs)
{
    std::vector<ExtrusionPath *> out;
    out.reserve(runs.size());
    Polyline remaining = src.polyline;
    for (size_t i = 0; i < runs.size(); ++i) {
        Polyline piece;
        if (i + 1 == runs.size()) {
            // Last run takes whatever is left, so the pieces reconstruct the ORIGINAL polyline
            // exactly (no fp gap/overlap from run.s1 not landing exactly on remaining's own end).
            piece = remaining;
        } else {
            const double cut_mm = std::max(0.0, double(runs[i].length()));
            Polyline rest;
            if (!remaining.split_at_length(scale_(cut_mm), &piece, &rest) || piece.points.size() < 2) {
                // Degenerate cut (e.g. a run shorter than one internal unit): keep everything in
                // this run rather than emit a zero-length path, and stop - the remaining runs
                // have nothing left to claim. Rare; min_run_len_mm already keeps runs well above
                // this floor in practice.
                piece     = remaining;
                out.push_back(new ExtrusionPath(src));
                out.back()->polyline = piece;
                return out;
            }
            remaining = rest;
        }
        if (piece.points.size() < 2)
            continue;
        auto *path = new ExtrusionPath(src);
        path->polyline = piece;
        out.push_back(path);
    }
    return out;
}

// The whole per-surface post-process: for every ExtrusionPath child of `eec` (a plain, non-arachne
// top-solid-infill path - arachne/variable-width paths and anything already a collection are left
// untouched, a documented step-4 limitation, see the phase 3 doc), split it into per-run pieces
// and wrap each in its own small ExtrusionEntityCollection tagged with that run's physical
// filament (ExtrusionEntityCollection::image_row_extruder_1based). Non-ExtrusionPath children
// (e.g. the gap-fill collection _create_gap_fill nests inside `eec`) are copied through unchanged.
// Returns the replacement top-level entities for `eec`'s slot in layerm->fills.entities - always
// at least one (falls back to `eec` itself, untouched, when nothing could be split).
std::vector<ExtrusionEntityCollection *> split_top_infill_by_image_row(const ImageAssetStore &assets, const ImageRowContext &ctx,
                                                                       double print_z, ExtrusionEntityCollection *eec)
{
    std::vector<ExtrusionEntityCollection *> out;
    bool split_any = false;

    for (ExtrusionEntity *child : eec->entities) {
        auto *path_candidate = dynamic_cast<ExtrusionPath *>(child);
        // Only erTopSolidInfill paths are eligible: _create_gap_fill() may nest an erGapFill
        // ExtrusionPath in the same collection, and gap fill deliberately follows wall_filament
        // (see fill_filament_source()'s own comment in PrintRegion.cpp) rather than
        // solid_infill_filament - overriding it here would fight that fix.
        ExtrusionPath *path = (path_candidate != nullptr && path_candidate->role() == erTopSolidInfill) ? path_candidate : nullptr;
        // Step 5, item 3: a Concentric-family (or other Arachne-driven) top surface pattern never
        // produces a plain ExtrusionPath here - it is an ExtrusionMultiPath/ExtrusionLoop instead,
        // so path_candidate is null even though this child's own role IS erTopSolidInfill. That
        // case is not a gap-fill child (role would be erGapFill, not erTopSolidInfill) and not
        // covered by "only one run" below - flag it once so the fallback is visible in the log,
        // not just inferred from the checkbox's own tooltip.
        // PHASE 4, step 4: an Arachne/Concentric-family top surface produces an
        // ExtrusionMultiPath or ExtrusionLoop here rather than a plain ExtrusionPath, so
        // `path_candidate` is null even though the child's own role IS erTopSolidInfill. Phase 3
        // left those unsplit (they fell back to the row's per-layer colour cycle, and
        // log_image_row_pattern_not_split_once() said so). They are now split by the SAME
        // multi-path cutter the wall dithering uses (ImageRowWalls.cpp), which walks a sequence
        // of ExtrusionPaths and cuts by arc length while keeping each piece's own attributes -
        // exactly what a variable-width Arachne path needs, since its width varies from path to
        // path and a cut must not homogenise it.
        //
        // A top-surface Concentric LOOP is cut open by this, the same way a wall loop is: there
        // is no such thing as a partially-coloured closed loop. That is the intended trade - the
        // alternative is the phase 3 behaviour, i.e. no dither on those patterns at all.
        if (path_candidate == nullptr && child->role() == erTopSolidInfill) {
            ImageRowWallContext wall_like;
            wall_like.params            = ctx.params;
            wall_like.candidate_ids     = ctx.candidate_ids;
            wall_like.candidate_colors  = ctx.candidate_colors;
            wall_like.mesh_box          = ctx.mesh_box;
            wall_like.mesh_from_print   = ctx.mesh_from_print;
            wall_like.min_run_len_mm    = ctx.min_run_len_mm;
            wall_like.sample_spacing_mm = ctx.sample_spacing_mm;
            // A top surface is ONE plane: hand the cutter the same synthesised "up" normal
            // image_row_runs_for_path() uses, so a Box projection picks the top face rather
            // than a side face computed from the path's own tangent.
            wall_like.fixed_normal_mesh = ctx.facet_normal_mesh;
            std::vector<std::unique_ptr<ExtrusionEntityCollection>> split =
                image_row_split_wall_entity(assets, wall_like, *child, print_z);
            if (!split.empty()) {
                for (auto &c : split)
                    out.push_back(c.release());
                split_any = true;
                continue;
            }
            // Genuinely could not be split (one colour, or the projection declined everywhere):
            // fall through to the un-split path below and log the fallback once, as before.
            log_image_row_pattern_not_split_once();
        }
        std::vector<ImageRowRun> runs = path != nullptr ? image_row_runs_for_path(assets, ctx, path->polyline, print_z)
                                                        : std::vector<ImageRowRun>();
        if (path == nullptr || runs.size() < 2) {
            // Not an ExtrusionPath (leave grouped with the ORIGINAL role/extruder - e.g. gap
            // fill), or only one run (the whole path is one filament - still tag it, so it is not
            // silently printed with the region's un-split resolve() fallback instead of the
            // colour the image actually chose).
            auto *solo = new ExtrusionEntityCollection();
            solo->no_sort = eec->no_sort;
            solo->entities.push_back(child->clone());
            if (path != nullptr && runs.size() == 1)
                solo->image_row_extruder_1based = unsigned(runs.front().filament_id);
            out.push_back(solo);
            continue;
        }
        split_any = true;
        std::vector<ExtrusionPath *> pieces = split_extrusion_path_by_runs(*path, runs);
        for (size_t i = 0; i < pieces.size() && i < runs.size(); ++i) {
            auto *run_eec = new ExtrusionEntityCollection();
            run_eec->no_sort = eec->no_sort;
            run_eec->entities.push_back(pieces[i]);
            run_eec->image_row_extruder_1based = unsigned(runs[i].filament_id);
            out.push_back(run_eec);
        }
    }

    if (out.empty()) {
        out.push_back(eec);
        return out;
    }
    // eec's children were either cloned (solo case) or handed off to a new path object that owns
    // the same heap Polyline data by value (split case, via ExtrusionPath's copy ctor) - either
    // way `eec` itself is now unreferenced and must be deleted so its old children do not leak
    // AND are not double-freed (its dtor deletes `entities`, none of which are shared with `out`).
    (void)split_any;
    delete eec;
    return out;
}

} // namespace

std::vector<SurfaceFill> group_fills(const Layer &layer, LockRegionParam &lock_param)
{
	std::vector<SurfaceFill> surface_fills;
	// Fill in a map of a region & surface to SurfaceFillParams.
	std::set<SurfaceFillParams> 						set_surface_params;
	std::vector<std::vector<const SurfaceFillParams*>> 	region_to_surface_params(layer.regions().size(), std::vector<const SurfaceFillParams*>());
    SurfaceFillParams									params;
    bool 												has_internal_voids = false;
	const PrintObjectConfig&							object_config = layer.object()->config();

	auto append_flow_param = [](std::map<Flow, ExPolygons> &flow_params, Flow flow, const ExPolygon &exp) {
        auto it = flow_params.find(flow);
        if (it == flow_params.end())
            flow_params.insert({flow, {exp}});
        else
            it->second.push_back(exp);
    };

	auto append_density_param = [](std::map<float, ExPolygons> &density_params, float density, const ExPolygon &exp) {
        auto it = density_params.find(density);
        if (it == density_params.end())
            density_params.insert({density, {exp}});
        else
            it->second.push_back(exp);
    };

	for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
		const LayerRegion  &layerm = *layer.regions()[region_id];
		region_to_surface_params[region_id].assign(layerm.fill_surfaces.size(), nullptr);
	    for (const Surface &surface : layerm.fill_surfaces.surfaces)
	        if (surface.surface_type == stInternalVoid)
	        	has_internal_voids = true;
	        else {
		        const PrintRegionConfig &region_config = layerm.region().config();
		        FlowRole extrusion_role = surface.is_top() ? frTopSolidInfill : (surface.is_solid() ? frSolidInfill : frInfill);
		        bool     is_bridge 	    = layer.id() > 0 && surface.is_bridge();
                const unsigned int effective_extruder = layerm.extruder(extrusion_role);
		        params.extruder 	 = effective_extruder;
		        params.pattern 		 = region_config.sparse_infill_pattern.value;
		        params.density       = float(region_config.sparse_infill_density);
                params.lateral_lattice_angle_1 = region_config.lateral_lattice_angle_1;
                params.lateral_lattice_angle_2 = region_config.lateral_lattice_angle_2;
                params.infill_overhang_angle = region_config.infill_overhang_angle;
                if (params.pattern == ipLockedZag) {
                    params.infill_lock_depth = scale_(region_config.infill_lock_depth);
                    params.skin_infill_depth = scale_(region_config.skin_infill_depth);
                    // ipCount here means "keep the sparse infill pattern", which FillLockedZag
                    // reads as "draw the band with my own fill_surface()" - the pre-feature path.
                    params.locked_skin_infill_pattern     = region_config.locked_skin_infill_pattern.value;
                    params.locked_skeleton_infill_pattern = region_config.locked_skeleton_infill_pattern.value;
                }
                if (params.pattern == ipCrossZag || params.pattern == ipLockedZag) {
                    params.symmetric_infill_y_axis = region_config.symmetric_infill_y_axis;
                } else if (params.pattern == ipZigZag) {
                    params.symmetric_infill_y_axis = region_config.symmetric_infill_y_axis;
                }

                if (surface.is_solid()) {
                    if (surface.is_external() && !is_bridge) {
                        if (surface.is_top()) {
                            params.pattern = region_config.top_surface_pattern.value;
                            params.density = float(region_config.top_surface_density);
                        } else { // Surface is bottom
                            params.pattern = region_config.bottom_surface_pattern.value;
                            params.density = float(region_config.bottom_surface_density);
                        }
                    } else if (surface.is_solid_infill()) {
                        if (region_config.undertop_surface_pattern.value != ipCount && region_config.top_shell_layers > 2 &&
                            region_config.top_surface_density < 100 && layer.upper_layer != nullptr && is_undertop(layer, surface))
                            params.pattern = region_config.undertop_surface_pattern.value;
                        else
                            params.pattern = region_config.internal_solid_infill_pattern.value;
                        params.density = 100.f;
                    } else {
                        if (region_config.top_surface_pattern == ipMonotonic || region_config.top_surface_pattern == ipMonotonicLine)
                            params.pattern = ipMonotonic;
                        else
                            params.pattern = ipRectilinear;
                        params.density = 100.f;
                    }
                }
                // A 0% top surface density means "walls only" on the top layer: drop the surface here,
                // as the fillers divide their line spacing by the density.
                if (params.density <= 0)
                    continue;

				params.extrusion_role = erInternalInfill;
                if (is_bridge) {
                    if (surface.is_internal_bridge())
                        params.extrusion_role = erInternalBridgeInfill;
                    else
                        params.extrusion_role = erBridgeInfill;
                } else if (surface.is_solid()) {
                    if (surface.is_top()) {
                        params.extrusion_role = erTopSolidInfill;
                        // Phase 3 (image row): see this field's own comment above - 0 for every
                        // print that does not use the feature.
                        params.image_row_filament_id = int(image_row_configured_virtual_id(*layer.object(), region_config));
                    } else if (surface.is_bottom_over_support()) {
                        // Ultra (over-support surfaces): a bottom shell in geometry - the pattern,
                        // the density and the flow above all came from the bottom-surface branch -
                        // but its own role, so the preview names it and GCode::_extrude can give it
                        // over_support_flow / over_support_speed instead of the bridge settings.
                        params.extrusion_role = erBottomSurfaceOverSupport;
                    } else if (surface.is_bottom()) {
                        params.extrusion_role = erBottomSurface;
                    } else {
                        params.extrusion_role = erSolidInfill;
                    }
                }
                // Orca: apply fill multiline only for sparse infill
                params.multiline = params.extrusion_role == erInternalInfill ? int(region_config.fill_multiline) : 1;

                if (params.extrusion_role == erInternalInfill) {
                    params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.infill_direction.value,
                                                                   region_config.sparse_infill_rotate_template.value);
                    params.is_using_template_angle = !region_config.sparse_infill_rotate_template.value.empty();
                } else {
                    params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.solid_infill_direction.value,
                                                                   region_config.solid_infill_rotate_template.value);
                    params.is_using_template_angle = !region_config.solid_infill_rotate_template.value.empty();
                }
                params.bridge_angle = float(surface.bridge_angle);
                
                if (region_config.align_infill_direction_to_model) {
                    auto m = layer.object()->trafo().matrix();
                    params.angle += atan2((float) m(1, 0), (float) m(0, 0));
                }

                // Calculate the actual flow we'll be using for this infill.
		        params.bridge = is_bridge || Fill::use_bridge_flow(params.pattern);
                const bool is_thick_bridge = surface.is_bridge() && (surface.is_internal_bridge() ? object_config.thick_internal_bridges : object_config.thick_bridges);
				params.flow   = params.bridge ?
					//Orca: enable thick bridge based on config
					layerm.bridging_flow(extrusion_role, is_thick_bridge) :
					layerm.flow(extrusion_role, (surface.thickness == -1) ? layer.height : surface.thickness);
				// record speed params
                if (!params.bridge) {
                    const PrintConfig  &print_config = layer.object()->print()->config();
                    const unsigned int  filament_id  = params.extruder > 0 ? params.extruder - 1 : 0;
                    if (params.extrusion_role == erInternalInfill)
                        params.sparse_infill_speed = get_value_at(print_config, region_config.sparse_infill_speed, ConfigFlowDomain::Process, filament_id);
                    else if (params.extrusion_role == erTopSolidInfill) {
                        params.top_surface_speed = get_value_at(print_config, region_config.top_surface_speed, ConfigFlowDomain::Process, filament_id);
                    } else if (params.extrusion_role == erSolidInfill)
                        params.solid_infill_speed = get_value_at(print_config, region_config.internal_solid_infill_speed, ConfigFlowDomain::Process, filament_id);
                    else if (params.extrusion_role == erBottomSurfaceOverSupport) {
                        // Ultra (over-support surfaces): the EFFECTIVE speed, so that a speed of 0
                        // ("match the outer wall") still tells two parts with different outer wall
                        // speeds apart. GCode::_extrude resolves the same way.
                        params.over_support_flow  = float(region_config.over_support_flow.value);
                        params.over_support_speed = float(region_config.over_support_speed.value > 0. ?
                                                          region_config.over_support_speed.value :
                                                          region_config.get_abs_value("outer_wall_speed"));
                    }
                }
				// Calculate flow spacing for infill pattern generation.
		        if (surface.is_solid() || is_bridge) {
		            params.spacing = params.flow.spacing();
		            // Don't limit anchor length for solid or bridging infill.
		            params.anchor_length = 1000.f;
					params.anchor_length_max = 1000.f;
		        } else {
					// Internal infill pattern spacing must stay independent of the current layer height and first-layer line width,
					// so sparse infill stays aligned over all layers of the current region.
		            params.spacing = layerm.flow(frInfill, layer.object()->config().layer_height, false).spacing();
		            // Anchor a sparse infill to inner perimeters with the following anchor length:
		        	params.anchor_length = float(region_config.infill_anchor);
					if (region_config.infill_anchor.percent)
						params.anchor_length = float(params.anchor_length * 0.01 * params.spacing);
					params.anchor_length_max = float(region_config.infill_anchor_max);
					if (region_config.infill_anchor_max.percent)
						params.anchor_length_max = float(params.anchor_length_max * 0.01 * params.spacing);
					params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);
				}

				//get locked region param
				if (params.pattern == ipLockedZag){
					const PrintObject *object = layerm.layer()->object();
					auto nozzle_diameter = float(object->print()->config().nozzle_diameter.get_at(effective_extruder - 1));
					Flow skin_flow = params.bridge ? params.flow : Flow::new_from_config_width(extrusion_role, region_config.skin_infill_line_width, nozzle_diameter, float((surface.thickness == -1) ? layer.height : surface.thickness));
					//add skin flow
					append_flow_param(lock_param.skin_flow_params, skin_flow, surface.expolygon);

					Flow skeleton_flow = params.bridge ? params.flow : Flow::new_from_config_width(extrusion_role, region_config.skeleton_infill_line_width, nozzle_diameter, float((surface.thickness == -1) ? layer.height : surface.thickness)) ;
					// add skeleton flow
					append_flow_param(lock_param.skeleton_flow_params, skeleton_flow, surface.expolygon);

					// add skin density
					append_density_param(lock_param.skin_density_params, float(0.01 * region_config.skin_infill_density), surface.expolygon);

					// add skin density
					append_density_param(lock_param.skeleton_density_params, float(0.01 * region_config.skeleton_infill_density), surface.expolygon);

					// Locked Zag phase 2b: only the contour-hugging path needs the depths keyed by
					// region, because there the erosion is applied per outlook-trimmed area rather
					// than once to the whole surface. Leaving the maps empty otherwise keeps
					// FillLockedZag on its scalar-depth fallback, which is bit-for-bit the split
					// this code did before the feature landed.
					if (region_config.infill_instead_top_bottom_surfaces) {
						append_density_param(lock_param.skin_depths_params, float(scale_(region_config.skin_infill_depth)), surface.expolygon);
						append_density_param(lock_param.locked_depths_params, float(scale_(region_config.infill_lock_depth)), surface.expolygon);
					}

				}

                auto it_params = set_surface_params.find(params);

		        if (it_params == set_surface_params.end())
		        	it_params = set_surface_params.insert(it_params, params);
		        region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()] = &(*it_params);
		    }
	}

	surface_fills.reserve(set_surface_params.size());
	for (const SurfaceFillParams &params : set_surface_params) {
		const_cast<SurfaceFillParams&>(params).idx = surface_fills.size();
		surface_fills.emplace_back(params);
	}

	for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
		const LayerRegion &layerm = *layer.regions()[region_id];
	    for (const Surface &surface : layerm.fill_surfaces.surfaces)
	        if (surface.surface_type != stInternalVoid) {
	        	const SurfaceFillParams *params = region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()];
				if (params != nullptr) {
	        		SurfaceFill &fill = surface_fills[params->idx];
                    if (fill.region_id == size_t(-1)) {
	        			fill.region_id = region_id;
	        			fill.surface = surface;
	        			fill.expolygons.emplace_back(std::move(fill.surface.expolygon));
						//BBS
						fill.region_id_group.push_back(region_id);
						fill.no_overlap_expolygons = layerm.fill_no_overlap_expolygons;
					} else {
						fill.expolygons.emplace_back(surface.expolygon);
						//BBS
						auto t = find(fill.region_id_group.begin(), fill.region_id_group.end(), region_id);
						if (t == fill.region_id_group.end()) {
							fill.region_id_group.push_back(region_id);
							fill.no_overlap_expolygons = union_ex(fill.no_overlap_expolygons, layerm.fill_no_overlap_expolygons);
						}
					}
				}
	        }
	}

	{
		Polygons all_polygons;
		for (SurfaceFill &fill : surface_fills)
			if (! fill.expolygons.empty()) {
				if (fill.expolygons.size() > 1 || ! all_polygons.empty()) {
					Polygons polys = to_polygons(std::move(fill.expolygons));
		            // Make a union of polygons, use a safety offset, subtract the preceding polygons.
				    // Bridges are processed first (see SurfaceFill::operator<())
		            fill.expolygons = all_polygons.empty() ? union_safety_offset_ex(polys) : diff_ex(polys, all_polygons, ApplySafetyOffset::Yes);
					append(all_polygons, std::move(polys));
				} else if (&fill != &surface_fills.back())
					append(all_polygons, to_polygons(fill.expolygons));
	        }
	}

    // we need to detect any narrow surfaces that might collapse
    // when adding spacing below
    // such narrow surfaces are often generated in sloping walls
    // by bridge_over_infill() and combine_infill() as a result of the
    // subtraction of the combinable area from the layer infill area,
    // which leaves small areas near the perimeters
    // we are going to grow such regions by overlapping them with the void (if any)
    // TODO: detect and investigate whether there could be narrow regions without
    // any void neighbors
    if (has_internal_voids) {
    	// Internal voids are generated only if "infill_only_where_needed" or "infill_every_layers" are active.
        coord_t  distance_between_surfaces = 0;
        Polygons surfaces_polygons;
        Polygons voids;
		int      region_internal_infill = -1;
		int		 region_solid_infill = -1;
		int		 region_some_infill = -1;
    	for (SurfaceFill &surface_fill : surface_fills)
			if (! surface_fill.expolygons.empty()) {
    			distance_between_surfaces = std::max(distance_between_surfaces, surface_fill.params.flow.scaled_spacing());
				append((surface_fill.surface.surface_type == stInternalVoid) ? voids : surfaces_polygons, to_polygons(surface_fill.expolygons));
				if (surface_fill.surface.surface_type == stInternalSolid)
					region_internal_infill = (int)surface_fill.region_id;
				if (surface_fill.surface.is_solid())
					region_solid_infill = (int)surface_fill.region_id;
				if (surface_fill.surface.surface_type != stInternalVoid)
					region_some_infill = (int)surface_fill.region_id;
			}
    	if (! voids.empty() && ! surfaces_polygons.empty()) {
    		// First clip voids by the printing polygons, as the voids were ignored by the loop above during mutual clipping.
    		voids = diff(voids, surfaces_polygons);
	        // Corners of infill regions, which would not be filled with an extrusion path with a radius of distance_between_surfaces/2
	        Polygons collapsed = diff(
	            surfaces_polygons,
				opening(surfaces_polygons, float(distance_between_surfaces /2), float(distance_between_surfaces / 2 + ClipperSafetyOffset)));
	        //FIXME why the voids are added to collapsed here? First it is expensive, second the result may lead to some unwanted regions being
	        // added if two offsetted void regions merge.
	        // polygons_append(voids, collapsed);
	        ExPolygons extensions = intersection_ex(expand(collapsed, float(distance_between_surfaces)), voids, ApplySafetyOffset::Yes);
	        // Now find an internal infill SurfaceFill to add these extrusions to.
	        SurfaceFill *internal_solid_fill = nullptr;
			unsigned int region_id = 0;
			if (region_internal_infill != -1)
				region_id = region_internal_infill;
			else if (region_solid_infill != -1)
				region_id = region_solid_infill;
			else if (region_some_infill != -1)
				region_id = region_some_infill;
			const LayerRegion& layerm = *layer.regions()[region_id];
	        for (SurfaceFill &surface_fill : surface_fills)
	        	if (surface_fill.surface.surface_type == stInternalSolid && std::abs(layer.height - surface_fill.params.flow.height()) < EPSILON) {
	        		internal_solid_fill = &surface_fill;
	        		break;
	        	}
	        if (internal_solid_fill == nullptr) {
	        	// Produce another solid fill.
		        params.extruder 	 = layerm.extruder(frSolidInfill);
                const auto top_pattern = layerm.region().config().top_surface_pattern;
                if(top_pattern == ipMonotonic || top_pattern == ipMonotonicLine)
                    params.pattern = top_pattern;
                else
                    params.pattern 		 = ipRectilinear;
	            params.density 		 = 100.f;
		        params.extrusion_role = erSolidInfill;
		        const PrintRegionConfig &region_config = layerm.region().config();
                params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.solid_infill_direction.value,
                                                               region_config.solid_infill_rotate_template.value);
                params.is_using_template_angle = !region_config.solid_infill_rotate_template.value.empty();

                // calculate the actual flow we'll be using for this infill
				params.flow = layerm.flow(frSolidInfill);
		        params.spacing = params.flow.spacing();
				surface_fills.emplace_back(params);
				surface_fills.back().surface.surface_type = stInternalSolid;
				surface_fills.back().surface.thickness = layer.height;
				surface_fills.back().expolygons = std::move(extensions);
	        } else {
	        	append(extensions, std::move(internal_solid_fill->expolygons));
	        	internal_solid_fill->expolygons = union_ex(extensions);
	        }
		}
    }

	// BBS: detect narrow internal solid infill area and use ipConcentricInternal pattern instead
	if (layer.object()->config().detect_narrow_internal_solid_infill) {
		size_t surface_fills_size = surface_fills.size();
		for (size_t i = 0; i < surface_fills_size; i++) {
			if (surface_fills[i].surface.surface_type != stInternalSolid)
				continue;

			ExPolygons normal_infill;
            ExPolygons narrow_infill;
            split_solid_surface(layer.id(), surface_fills[i], normal_infill, narrow_infill);

			if (narrow_infill.empty()) {
				// BBS: has no narrow expolygon
				continue;
			} else if (normal_infill.empty()) {
				// BBS: all expolygons are narrow, directly change the fill pattern
				surface_fills[i].params.pattern = ipConcentricInternal;
			}
			else {
				// BBS: some expolygons are narrow, spilit surface_fills[i] and rearrange the expolygons
				params = surface_fills[i].params;
				params.pattern = ipConcentricInternal;
				surface_fills.emplace_back(params);
				surface_fills.back().region_id = surface_fills[i].region_id;
				surface_fills.back().surface.surface_type = stInternalSolid;
				surface_fills.back().surface.thickness = surface_fills[i].surface.thickness;
                surface_fills.back().region_id_group       = surface_fills[i].region_id_group;
                surface_fills.back().no_overlap_expolygons = surface_fills[i].no_overlap_expolygons;
			    // BBS: move the narrow expolygons to new surface_fills.back();
			    surface_fills.back().expolygons = std::move(narrow_infill);
			    // BBS: delete the narrow expolygons from old surface_fills
                surface_fills[i].expolygons = std::move(normal_infill);
			}
		}
	}

	// Orca: drop degenerate internal-solid slivers — patches under ~3 extrusion widths in both
	// directions. After the fill boundary inset such a patch can only emit a single sub-width
	// micro-zigzag with no structural value, and whether the patch survives the upstream boolean
	// chain sits on a numeric knife edge, which makes the G-code non-reproducible between
	// otherwise identical slices. Long thin regions are kept: only patches tiny in BOTH
	// dimensions are removed.
	for (SurfaceFill &fill : surface_fills) {
		if (fill.surface.surface_type != stInternalSolid)
			continue;
		const coord_t sspacing  = scale_(fill.params.spacing);
		const double  max_area  = 4.0 * sqr(double(sspacing));
		const coord_t max_dim   = 3 * sspacing;
		fill.expolygons.erase(std::remove_if(fill.expolygons.begin(), fill.expolygons.end(),
			[max_area, max_dim, &fill](const ExPolygon &ex) {
				BoundingBox bb = get_extents(ex);
				bool drop = bb.size().x() < max_dim && bb.size().y() < max_dim &&
				            std::abs(ex.area()) < max_area;
				if (getenv("ORCA_SLIVER_DEBUG"))
					fprintf(stderr, "SLIVER %s area=%.4fmm2 bbox=%.3fx%.3fmm spacing=%.3f\n",
					        drop ? "DROP" : "keep",
					        unscale<double>(1) * unscale<double>(1) * std::abs(ex.area()),
					        unscale<double>(bb.size().x()), unscale<double>(bb.size().y()),
					        fill.params.spacing);
				return drop;
			}),
			fill.expolygons.end());
	}
	surface_fills.erase(std::remove_if(surface_fills.begin(), surface_fills.end(),
		[](const SurfaceFill &fill) { return fill.expolygons.empty(); }),
		surface_fills.end());

	return surface_fills;
}

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
void export_group_fills_to_svg(const char *path, const std::vector<SurfaceFill> &fills)
{
    BoundingBox bbox;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            bbox.merge(get_extents(expoly));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            svg.draw(expoly, surface_type_to_color_name(fill.surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}
#endif

// Locked Zag phase 2b: hand the layer's own top/bottom fill surfaces to the sparse infill, so the
// skin band follows the model contour instead of a uniform depth-offset band, and fill those
// surfaces with sparse infill rather than solid. Ported from BambuStudio's Layer::set_outlook_range
// (src/libslic3r/Fill/Fill.cpp) - both trees AGPL-3.0; rewritten here because this tree's
// SurfaceCollection::keep_type has no ExPolygons out-parameter.
//
// Does nothing at all unless some region asks for it, and infill_instead_top_bottom_surfaces
// defaults to off - so for every existing profile this function leaves the layer untouched and
// lock_param.outlook stays empty.
void Layer::set_outlook_range(LockRegionParam &lock_param)
{
    for (size_t region_id = 0; region_id < this->regions().size(); ++region_id) {
        LayerRegion &layerm = *this->regions()[region_id];
        const PrintRegionConfig &config = layerm.region().config();
        if (! config.infill_instead_top_bottom_surfaces || config.sparse_infill_pattern != ipLockedZag)
            continue;

        // Everything that is not plain internal sparse infill - the top and bottom shells, the
        // solid infill under them, bridges - becomes skin, and is re-typed to stInternal so the
        // sparse infill (rather than the solid fills) is what draws it.
        ExPolygons non_internal;
        for (const Surface &surface : layerm.fill_surfaces.surfaces)
            if (surface.surface_type != stInternal)
                non_internal.push_back(surface.expolygon);
        if (non_internal.empty())
            continue;

        append(lock_param.outlook, non_internal);
        lock_param.outlook = union_safety_offset_ex(lock_param.outlook);

        layerm.fill_surfaces.keep_type(stInternal);
        layerm.fill_surfaces.append(std::move(non_internal), stInternal);
    }
}

// friend to Layer
void Layer::make_fills(FillAdaptive::Octree* adaptive_fill_octree, FillAdaptive::Octree* support_fill_octree, FillLightning::Generator* lightning_generator)
{
	for (LayerRegion *layerm : m_regions)
		layerm->fills.clear();


#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
//	this->export_region_fill_surfaces_to_svg_debug("10_fill-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    LockRegionParam lock_param;
    // Locked Zag contour-hugging skin; a no-op unless a region turns it on.
    this->set_outlook_range(lock_param);
    std::vector<SurfaceFill>     surface_fills = group_fills(*this, lock_param);
	const Slic3r::BoundingBox bbox 			= this->object()->bounding_box();
	const auto                resolution 	= this->object()->print()->config().resolution.value;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
	{
		static int iRun = 0;
		export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-10_fill-final-%d.svg", iRun ++).c_str(), surface_fills);
	}
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    for (SurfaceFill &surface_fill : surface_fills) {
        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        f->layer_id = this->id();
        f->z 		= this->print_z;
        f->angle 	= surface_fill.params.angle;
        f->is_using_template_angle = surface_fill.params.is_using_template_angle;
        // ZAA: keep the fill direction constant on contoured tops.
        if (surface_fill.region_id != size_t(-1)) {
            const PrintRegionConfig &zaa_rcfg = this->regions()[surface_fill.region_id]->region().config();
            f->dont_alternate_fill_direction = zaa_rcfg.zaa_enabled && zaa_rcfg.zaa_dont_alternate_fill_direction;
        }
        f->adapt_fill_octree   = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->print_config        = &this->object()->print()->config();
        f->print_object_config = &this->object()->config();
		if (surface_fill.params.pattern == ipConcentricInternal) {
            FillConcentricInternal *fill_concentric = dynamic_cast<FillConcentricInternal *>(f.get());
            assert(fill_concentric != nullptr);
            fill_concentric->print_config        = &this->object()->print()->config();
            fill_concentric->print_object_config = &this->object()->config();
        } else if (surface_fill.params.pattern == ipConcentric) {
            FillConcentric *fill_concentric = dynamic_cast<FillConcentric *>(f.get());
            assert(fill_concentric != nullptr);
            fill_concentric->print_config = &this->object()->print()->config();
            fill_concentric->print_object_config = &this->object()->config();
        } else if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler*>(f.get())->generator = lightning_generator;
        // calculate flow spacing for infill pattern generation
        bool using_internal_flow = ! surface_fill.surface.is_solid() && ! surface_fill.params.bridge;
        double link_max_length = 0.;
        if (! surface_fill.params.bridge) {
#if 0
            link_max_length = layerm.region()->config().get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > 80.) // 80%
                link_max_length = 3. * f->spacing;
#endif
        }

        LayerRegion* layerm = this->m_regions[surface_fill.region_id];

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = (coord_t)scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = coord_t(scale_(layerm->region().config().seam_gap.get_abs_value(surface_fill.params.flow.nozzle_diameter())));

        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density 		     = float(0.01 * surface_fill.params.density);
        params.multiline         = surface_fill.params.multiline;
		params.dont_adjust		 = false; //  surface_fill.params.dont_adjust;
        params.anchor_length     = surface_fill.params.anchor_length;
		params.anchor_length_max = surface_fill.params.anchor_length_max;
		params.resolution        = resolution;
        params.use_arachne       = surface_fill.params.pattern == ipConcentric || surface_fill.params.pattern == ipConcentricInternal;
        params.layer_height      = layerm->layer()->height;
        params.lateral_lattice_angle_1   = surface_fill.params.lateral_lattice_angle_1;
        params.lateral_lattice_angle_2   = surface_fill.params.lateral_lattice_angle_2;
        params.infill_overhang_angle   = surface_fill.params.infill_overhang_angle;

		// BBS
		params.flow = surface_fill.params.flow;
		params.extrusion_role = surface_fill.params.extrusion_role;
		params.using_internal_flow = using_internal_flow;
		params.no_extrusion_overlap = surface_fill.params.overlap;
        auto &region_config = layerm->region().config();
        params.config               = &region_config;
        params.pattern              = surface_fill.params.pattern;

        if( surface_fill.params.pattern == ipLockedZag ) {
			params.locked_zag = true;
            params.infill_lock_depth = surface_fill.params.infill_lock_depth;
            params.skin_infill_depth = surface_fill.params.skin_infill_depth;
            f->set_lock_region_param(lock_param);
            f->set_skin_and_skeleton_pattern(surface_fill.params.locked_skin_infill_pattern,
                                             surface_fill.params.locked_skeleton_infill_pattern);
		}
        if (surface_fill.params.pattern == ipCrossZag || surface_fill.params.pattern == ipLockedZag) {
            if (f->layer_id % 2 == 0) {
                params.horiz_move -= scale_(region_config.infill_shift_step) * (f->layer_id / 2);
            } else {
                params.horiz_move += scale_(region_config.infill_shift_step) * (f->layer_id / 2);
            }

            params.symmetric_infill_y_axis = surface_fill.params.symmetric_infill_y_axis;

        } else if (surface_fill.params.pattern == ipZigZag) {
            params.symmetric_infill_y_axis = surface_fill.params.symmetric_infill_y_axis;

        }
		if (surface_fill.params.pattern == ipGrid)
			params.can_reverse = false;

		// Phase 3 (image row): built once per SurfaceFill, not per ExPolygon - region_config and
		// flow are the same for every expolygon this SurfaceFill owns. `image_row_ctx_ok` false
		// (the overwhelming common case: any print not using the feature) means the loop below
		// runs exactly as it did before this phase, byte for byte.
		ImageRowContext image_row_ctx;
		const bool image_row_ctx_ok = surface_fill.params.extrusion_role == erTopSolidInfill &&
		                              surface_fill.params.image_row_filament_id != 0 &&
		                              image_row_context_for_region(*this->object(), layerm->region(),
		                                                           float(surface_fill.params.flow.width()), image_row_ctx);

		for (ExPolygon& expoly : surface_fill.expolygons) {

      f->no_overlap_expolygons = intersection_ex(surface_fill.no_overlap_expolygons, ExPolygons() = {expoly}, ApplySafetyOffset::Yes);
            if (params.symmetric_infill_y_axis) {
                params.symmetric_y_axis = f->extended_object_bounding_box().center().x();
                expoly.symmetric_y(params.symmetric_y_axis);
            }

			// Spacing is modified by the filler to indicate adjustments. Reset it for each expolygon.
			f->spacing = surface_fill.params.spacing;
			surface_fill.surface.expolygon = std::move(expoly);

			if(surface_fill.params.bridge && surface_fill.surface.is_external() && surface_fill.params.density > 99.0){
				params.density = layerm->region().config().bridge_density.get_abs_value(1.0);
				params.dont_adjust = true;
			}
            if(surface_fill.surface.is_internal_bridge()){
                params.density = f->print_object_config->internal_bridge_density.get_abs_value(1.0);
                params.dont_adjust = true;
            }
			// BBS: make fill
			ExtrusionEntitiesPtr &region_fill_entities = m_regions[surface_fill.region_id]->fills.entities;
			const size_t entities_before = region_fill_entities.size();
			f->fill_surface_extrusion(&surface_fill.surface,
				params,
				region_fill_entities);

			// Phase 3 (image row): fill_surface_extrusion() above pushed the new top-level
			// entities for this expolygon (normally exactly one collection; FillLockedZag's own
			// override can push more, so every newly pushed entity in [entities_before, end) is
			// handled, not just the last). Replace each with one small collection per dithered
			// run, tagged with its own physical filament - see
			// split_top_infill_by_image_row()'s own header comment.
			if (image_row_ctx_ok && region_fill_entities.size() > entities_before) {
				ExtrusionEntitiesPtr new_entities(region_fill_entities.begin() + entities_before, region_fill_entities.end());
				region_fill_entities.erase(region_fill_entities.begin() + entities_before, region_fill_entities.end());
				for (ExtrusionEntity *ee : new_entities) {
					auto *eec = dynamic_cast<ExtrusionEntityCollection *>(ee);
					assert(eec != nullptr);
					if (eec == nullptr) {
						// Should not happen (make_fills()'s own assert below expects every
						// top-level fill entity to be a collection) - keep it as-is rather than
						// drop geometry.
						region_fill_entities.push_back(ee);
						continue;
					}
					std::vector<ExtrusionEntityCollection *> replaced =
						split_top_infill_by_image_row(this->object()->model_object()->get_model()->image_assets,
						                              image_row_ctx, this->print_z, eec);
					for (ExtrusionEntityCollection *r : replaced)
						region_fill_entities.push_back(r);
				}
			}
		}
    }

    // add thin fill regions
    // Unpacks the collection, creates multiple collections per path.
    // The path type could be ExtrusionPath, ExtrusionLoop or ExtrusionEntityCollection.
    // Why the paths are unpacked?
	for (LayerRegion *layerm : m_regions)
	    for (const ExtrusionEntity *thin_fill : layerm->thin_fills.entities) {
	        ExtrusionEntityCollection &collection = *(new ExtrusionEntityCollection());
	        layerm->fills.entities.push_back(&collection);
	        collection.entities.push_back(thin_fill->clone());
	    }

#ifndef NDEBUG
	for (LayerRegion *layerm : m_regions)
	    for (size_t i = 0; i < layerm->fills.entities.size(); ++ i)
    	    assert(dynamic_cast<ExtrusionEntityCollection*>(layerm->fills.entities[i]) != nullptr);
#endif
}
/**
 * Generate sparse-infill polylines for anchoring/analysis purposes.
 *
 * This produces the geometric polylines of internal sparse infill for the current
 * layer (using the same infill pattern, angle, rotation template, and spacing that
 * normal slicing would use), but it does not create extrusion entities.
 *
 * The returned polylines are consumed by internal-bridge detection on the next
 * layer to derive anchor lines and compute the bridge direction over sparse infill.
 *
 * Notes:
 * - Only `stInternal` surfaces are considered.
 * - Rotation templates (e.g. `sparse_infill_rotate_template`) are applied so the
 *   anchors reflect the actual infill orientation.
 * - For lightning/adaptive patterns, the respective generators are wired so their
 *   polylines match the final infill layout.
 */
Polylines Layer::generate_sparse_infill_polylines_for_anchoring(FillAdaptive::Octree* adaptive_fill_octree, FillAdaptive::Octree* support_fill_octree,  FillLightning::Generator* lightning_generator) const
{
    LockRegionParam skin_inner_param;
    std::vector<SurfaceFill> surface_fills = group_fills(*this, skin_inner_param);
	const Slic3r::BoundingBox bbox = this->object()->bounding_box();
	const auto                resolution = this->object()->print()->config().resolution.value;

    Polylines sparse_infill_polylines{};

    for (SurfaceFill &surface_fill : surface_fills) {
		if (surface_fill.surface.surface_type != stInternal) {
			continue;
		}

        switch (surface_fill.params.pattern) {
        case ipCount: continue; break;
        case ipSupportBase: continue; break;
        case ipConcentricInternal: continue; break;
        case ipLightning:
		case ipAdaptiveCubic:
        case ipSupportCubic:
        case ipRectilinear:
        case ipMonotonic:
        case ipMonotonicLine:
        case ipAlignedRectilinear:
        case ipGrid:
        case ipLateralLattice:
        case ipTriangles:
        case ipStars:
        case ipCubic:
        case ipLine:
        case ipConcentric:
        case ipHoneycomb:
        case ipLateralHoneycomb:
        case ip3DHoneycomb:
        case ipGyroid:
        case ipTpmsD:
        case ipTpmsFK:
        case ipHilbertCurve:
        case ipArchimedeanChords:
        case ipOctagramSpiral:
        case ipZigZag:
        case ipCrossZag:
		case ipLockedZag: break;
        }

        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        f->layer_id = this->id() - this->object()->get_layer(0)->id(); // We need to subtract raft layers.
        f->z        = this->print_z;
        f->angle    = surface_fill.params.angle;
        f->is_using_template_angle = surface_fill.params.is_using_template_angle;
        // ZAA: keep the fill direction constant on contoured tops.
        if (surface_fill.region_id != size_t(-1)) {
            const PrintRegionConfig &zaa_rcfg = this->regions()[surface_fill.region_id]->region().config();
            f->dont_alternate_fill_direction = zaa_rcfg.zaa_enabled && zaa_rcfg.zaa_dont_alternate_fill_direction;
        }
        f->adapt_fill_octree   = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->print_config        = &this->object()->print()->config();
        f->print_object_config = &this->object()->config();

        if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler *>(f.get())->generator = lightning_generator;

        // calculate flow spacing for infill pattern generation
        double link_max_length = 0.;
        if (!surface_fill.params.bridge) {
#if 0
            link_max_length = layerm.region()->config().get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > 80.) // 80%
                link_max_length = 3. * f->spacing;
#endif
        }

        LayerRegion &layerm = *m_regions[surface_fill.region_id];

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = (coord_t) scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = coord_t(scale_(layerm.region().config().seam_gap.get_abs_value(surface_fill.params.flow.nozzle_diameter())));

        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density           = float(0.01 * surface_fill.params.density);
        params.dont_adjust       = false; //  surface_fill.params.dont_adjust;
        params.anchor_length     = surface_fill.params.anchor_length;
        params.anchor_length_max = surface_fill.params.anchor_length_max;
        params.resolution        = resolution;
        params.use_arachne       = false;
        params.layer_height      = layerm.layer()->height;
        params.lateral_lattice_angle_1   = surface_fill.params.lateral_lattice_angle_1;
        params.lateral_lattice_angle_2   = surface_fill.params.lateral_lattice_angle_2;
        params.infill_overhang_angle   = surface_fill.params.infill_overhang_angle;
        params.multiline         = surface_fill.params.multiline;
        // Orca: Match make_fills() when choosing the origin of plane-path patterns.
        // Without the sparse extrusion role, the filler uses each surface's bounds
        // instead of the object's bounds, so bridge anchors shift away from printed infill.
        params.extrusion_role            = surface_fill.params.extrusion_role;

        for (ExPolygon &expoly : surface_fill.expolygons) {
            // Spacing is modified by the filler to indicate adjustments. Reset it for each expolygon.
            f->spacing                     = surface_fill.params.spacing;
            surface_fill.surface.expolygon = std::move(expoly);
            try {
                Polylines polylines = f->fill_surface(&surface_fill.surface, params);
                sparse_infill_polylines.insert(sparse_infill_polylines.end(), polylines.begin(), polylines.end());
            } catch (InfillFailedException &) {}
        }
    }

    return sparse_infill_polylines;
}

// Create ironing extrusions over top surfaces.
void Layer::make_ironing()
{
	// LayerRegion::slices contains surfaces marked with SurfaceType.
	// Here we want to collect top surfaces extruded with the same extruder.
	// A surface will be ironed with the same extruder to not contaminate the print with another material leaking from the nozzle.

	// First classify regions based on the extruder used.
	struct IroningParams {
		InfillPattern pattern;
		int 		extruder 	= -1;
		bool 		just_infill = false;
		// Spacing of the ironing lines, also to calculate the extrusion flow from.
		double 		line_spacing;
		// Height of the extrusion, to calculate the extrusion flow from.
		double 		height;
		double 		speed;
		double 		angle;
        double 		inset;

		bool operator<(const IroningParams &rhs) const {
			if (this->extruder < rhs.extruder)
				return true;
			if (this->extruder > rhs.extruder)
				return false;
			if (int(this->just_infill) < int(rhs.just_infill))
				return true;
			if (int(this->just_infill) > int(rhs.just_infill))
				return false;
			if (this->line_spacing < rhs.line_spacing)
				return true;
			if (this->line_spacing > rhs.line_spacing)
				return false;
			if (this->height < rhs.height)
				return true;
			if (this->height > rhs.height)
				return false;
			if (this->speed < rhs.speed)
				return true;
			if (this->speed > rhs.speed)
				return false;
			if (this->angle < rhs.angle)
				return true;
			if (this->angle > rhs.angle)
				return false;
            if (this->inset < rhs.inset)
                return true;
            if (this->inset > rhs.inset)
                return false;
			return false;
		}

		bool operator==(const IroningParams &rhs) const {
			return this->extruder == rhs.extruder && this->just_infill == rhs.just_infill &&
				   this->line_spacing == rhs.line_spacing && this->height == rhs.height && this->speed == rhs.speed && this->angle == rhs.angle && this->pattern == rhs.pattern && this->inset == rhs.inset;
		}

		LayerRegion *layerm		= nullptr;

		// IdeaMaker: ironing
		// ironing flowrate (5% percent)
		// ironing speed (10 mm/sec)

		// Kisslicer:
		// iron off, Sweep, Group
		// ironing speed: 15 mm/sec

		// Cura:
		// Pattern (zig-zag / concentric)
		// line spacing (0.1mm)
		// flow: from normal layer height. 10%
		// speed: 20 mm/sec
	};

	std::vector<IroningParams> by_extruder;
    double default_layer_height = this->object()->config().layer_height;

	for (LayerRegion *layerm : m_regions)
		if (! layerm->slices.empty()) {
			IroningParams ironing_params;
			const PrintRegionConfig &config = layerm->region().config();
			if (config.ironing_type != IroningType::NoIroning &&
			    (config.ironing_type == IroningType::AllSolid ||
				    ((config.top_shell_layers > 0 || (this->object()->print()->config().spiral_mode && config.bottom_shell_layers > 1)) &&
					    (config.ironing_type == IroningType::TopSurfaces ||
					        (config.ironing_type == IroningType::TopmostOnly && layerm->layer()->upper_layer == nullptr))))) {
				if (config.wall_filament == config.solid_infill_filament || config.wall_loops == 0) {
					// Iron the whole face.
					ironing_params.extruder = config.solid_infill_filament;
				} else {
					// Iron just the infill.
					ironing_params.extruder = config.solid_infill_filament;
				}
				// PHASE 4, step 3: ironing IS now split per image, so ironing_params.extruder's
				// virtual id is only the fallback for the case the split declines (see the
				// split site further down in this function). Phase 3 left ironing on the row's
				// per-layer colour cycle, which on an image surface meant the ironed skin -
				// the layer you actually SEE - was a single flat colour smeared over a
				// correctly dithered surface, hiding the feature it was meant to finish.
				(void)config;
			}
			if (ironing_params.extruder != -1) {
				//TODO just_infill is currently not used.
				ironing_params.just_infill 	= false;
				ironing_params.line_spacing = std::max(IRONING_SPACING_MIN, double(config.ironing_spacing));
                ironing_params.inset 		= config.ironing_inset;
				ironing_params.height 		= default_layer_height * 0.01 * config.ironing_flow;
				ironing_params.speed 		= config.ironing_speed.values.front();
                ironing_params.angle        = (config.ironing_angle >= 0 ? config.ironing_angle : config.infill_direction) * M_PI / 180.;
				ironing_params.pattern      = config.ironing_pattern;
				ironing_params.layerm 		= layerm;
				by_extruder.emplace_back(ironing_params);
			}
		}
	std::sort(by_extruder.begin(), by_extruder.end());

    FillParams 			fill_params;
    fill_params.density 	 = 1.;
    fill_params.monotonic    = true;
    InfillPattern         f_pattern = ipRectilinear;
    std::unique_ptr<Fill> f         = std::unique_ptr<Fill>(Fill::new_from_type(f_pattern));
    f->set_bounding_box(this->object()->bounding_box());
    f->layer_id = this->id();
    f->z        = this->print_z;
    f->overlap  = 0;
	for (size_t i = 0; i < by_extruder.size();) {
		// Find span of regions equivalent to the ironing operation.
		IroningParams &ironing_params = by_extruder[i];
		// Create the filler object.
		if( f_pattern != ironing_params.pattern )
		{
            f_pattern               = ironing_params.pattern;
            f = std::unique_ptr<Fill>(Fill::new_from_type(f_pattern));
            f->set_bounding_box(this->object()->bounding_box());
            f->layer_id = this->id();
            f->z        = this->print_z;
            f->overlap  = 0;
		}

		size_t j = i;
		for (++ j; j < by_extruder.size() && ironing_params == by_extruder[j]; ++ j) ;

		// Create the ironing extrusions for regions <i, j)
		ExPolygons ironing_areas;
		double nozzle_dmr = this->object()->print()->config().nozzle_diameter.get_at(ironing_params.extruder - 1);
		if (ironing_params.just_infill) {
			//TODO just_infill is currently not used.
			// Just infill.
		} else {
			// Infill and perimeter.
			// Merge top surfaces with the same ironing parameters.
			Polygons polys;
			Polygons infills;
			for (size_t k = i; k < j; ++ k) {
				const IroningParams		 &ironing_params  = by_extruder[k];
				const PrintRegionConfig  &region_config   = ironing_params.layerm->region().config();
				bool					  iron_everything = region_config.ironing_type == IroningType::AllSolid;
				bool					  iron_completely = iron_everything;
				if (iron_everything) {
					// Check whether there is any non-solid hole in the regions.
					bool internal_infill_solid = region_config.sparse_infill_density.value > 95.;
					for (const Surface &surface : ironing_params.layerm->fill_surfaces.surfaces)
						if ((!internal_infill_solid && surface.surface_type == stInternal) || surface.surface_type == stInternalBridge || surface.surface_type == stInternalVoid) {
							// Some fill region is not quite solid. Don't iron over the whole surface.
							iron_completely = false;
							break;
						}
				}
				if (iron_completely) {
					// Iron everything. This is likely only good for solid transparent objects.
					for (const Surface &surface : ironing_params.layerm->slices.surfaces)
						polygons_append(polys, surface.expolygon);
				} else {
					for (const Surface &surface : ironing_params.layerm->slices.surfaces)
						if ((surface.surface_type == stTop && (region_config.top_shell_layers > 0 || this->object()->print()->config().spiral_mode)) || (iron_everything && (surface.surface_type == stBottom || surface.surface_type == stBottomOverSupport) && region_config.bottom_shell_layers > 0))
							// stBottomBridge is not being ironed on purpose, as it would likely destroy the bridges.
							polygons_append(polys, surface.expolygon);
				}
				if (iron_everything && ! iron_completely) {
					// Add solid fill surfaces. This may not be ideal, as one will not iron perimeters touching these
					// solid fill surfaces, but it is likely better than nothing.
					for (const Surface &surface : ironing_params.layerm->fill_surfaces.surfaces)
						if (surface.surface_type == stInternalSolid)
							polygons_append(infills, surface.expolygon);
				}
			}

			if (! infills.empty() || j > i + 1) {
				// Ironing over more than a single region or over solid internal infill.
				if (! infills.empty())
					// For IroningType::AllSolid only:
					// Add solid infill areas for layers, that contain some non-ironable infil (sparse infill, bridge infill).
					append(polys, std::move(infills));
				polys = union_safety_offset(polys);
			}
			// Trim the top surfaces with half the nozzle diameter.
            // BBS: ironing inset
            double ironing_areas_offset = ironing_params.inset == 0 ? float(scale_(0.5 * nozzle_dmr)) : scale_(ironing_params.inset);
			ironing_areas = intersection_ex(polys, offset(this->lslices, - ironing_areas_offset));
		}

        // Create the filler object.
        // ZAA: ironing over a contoured top keeps its direction too.
        {
            const PrintRegionConfig &zaa_rcfg = ironing_params.layerm->region().config();
            f->dont_alternate_fill_direction = zaa_rcfg.zaa_enabled && zaa_rcfg.zaa_dont_alternate_fill_direction;
        }
        f->spacing = ironing_params.line_spacing;
        f->angle = float(ironing_params.angle);
        f->link_max_length = (coord_t) scale_(3. * f->spacing);
		double  extrusion_height = ironing_params.height * f->spacing / nozzle_dmr;
		float  extrusion_width  = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle_dmr), float(extrusion_height));
		double flow_mm3_per_mm = nozzle_dmr * extrusion_height;
        Surface surface_fill(stTop, ExPolygon());
        for (ExPolygon &expoly : ironing_areas) {
			surface_fill.expolygon = std::move(expoly);
			Polylines polylines;
			try {
				polylines = f->fill_surface(&surface_fill, fill_params);
			} catch (InfillFailedException &) {
			}
	        if (! polylines.empty()) {
		        // Save into layer.
				ExtrusionEntityCollection *eec = nullptr;
		        ironing_params.layerm->fills.entities.push_back(eec = new ExtrusionEntityCollection());
		        // Don't sort the ironing infill lines as they are monotonicly ordered.
				eec->no_sort = true;
		        extrusion_entities_append_paths(
		            eec->entities, std::move(polylines),
		            erIroning,
		            flow_mm3_per_mm, extrusion_width, float(extrusion_height));
		        // PHASE 4, step 3: dither the ironing pass through the same image the top solid
		        // infill beneath it was dithered through, so the ironed skin matches the picture
		        // instead of covering it with one flat colour. The geometry here is plain
		        // ExtrusionPaths in a no_sort collection - structurally what
		        // split_top_infill_by_image_row() already handles - so the split is the same
		        // operation, only keyed on erIroning rather than erTopSolidInfill.
		        //
		        // The sampling resolution is the IRONING pass's own extrusion width, which is
		        // much narrower than a top-infill line (ironing lays a thin cover bead at
		        // ironing_spacing). That is deliberate: the ironed skin can therefore resolve the
		        // image at least as finely as the surface under it, never more coarsely.
		        ImageRowWallContext iron_ctx;
		        if (image_row_wall_iron_context(*this->object(), ironing_params.layerm->region(),
		                                        extrusion_width, iron_ctx)) {
		            std::vector<ExtrusionEntityCollection *> replacement;
		            for (ExtrusionEntity *child : eec->entities) {
		                std::vector<std::unique_ptr<ExtrusionEntityCollection>> split =
		                    image_row_split_wall_entity(this->object()->model_object()->get_model()->image_assets,
		                                                iron_ctx, *child, this->print_z);
		                if (split.empty()) {
		                    auto *solo = new ExtrusionEntityCollection();
		                    solo->no_sort = true;
		                    solo->entities.push_back(child->clone());
		                    replacement.push_back(solo);
		                } else {
		                    for (auto &c : split)
		                        replacement.push_back(c.release());
		                }
		            }
		            if (!replacement.empty()) {
		                ironing_params.layerm->fills.entities.pop_back();
		                delete eec;
		                for (ExtrusionEntityCollection *c : replacement)
		                    ironing_params.layerm->fills.entities.push_back(c);
		            }
		        }
		    }
		}

		// Regions up to j were processed.
		i = j;
	}
}

} // namespace Slic3r
