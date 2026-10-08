#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/LayerTimeSpeedSmoothingFilter.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/TimelapsePosPicker.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "EdgeGrid.hpp"
#include "GCode/ThumbnailData.hpp"
#include "libslic3r/ObjectID.hpp"
#include "GCode/ExtrusionProcessor.hpp"

#include "GCode/PressureEqualizer.hpp"
#include "GCode/SmallAreaInfillFlowCompensator.hpp"
// ORCA: post processor below used for Dynamic Pressure advance
#include "GCode/AdaptivePAProcessor.hpp"

#include <memory>
#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <cfloat>

namespace Slic3r {

// Forward declarations.
class GCode;
struct WipeInwardSupport;

namespace { struct Item; }
struct PrintInstance;
class ConstPrintObjectPtrsAdaptor;

class OozePrevention {
public:
    bool enable;

    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);

private:
    int _get_temp(const GCode &gcodegen) const;
};

class Wipe {
public:
    bool enable;
    Polyline path;
    struct RetractionValues{
        double retractLengthBeforeWipe;
        double retractLengthDuringWipe;
    };

    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, double length, bool toolchange = false, bool is_last = false);
    RetractionValues calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange);
    // Orca: rebuild the stored path while deduplicating shared path boundaries.
    void update_path(const ExtrusionPaths &paths, bool reverse = false);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        // BBS: add partplate logic
        const int                                                    plate_idx,
        const Vec3d                                                  plate_origin,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &local_z_tool_changes,
        const std::vector<std::vector<WipeTower::box_coordinates>>  &local_z_reserve_boxes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ print_config.prime_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.get_at(plate_idx)), float(print_config.wipe_tower_y.get_at(plate_idx))),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_extruder_offsets(print_config.extruder_offset.values),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_local_z_tool_changes(local_z_tool_changes),
        m_local_z_reserve_boxes(local_z_reserve_boxes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_local_z_tool_change_idx(local_z_tool_changes.size(), 0),
        m_local_z_reserve_slot_idx(local_z_reserve_boxes.size(), 0),
        m_plate_origin(plate_origin),
        m_single_extruder_multi_material(print_config.single_extruder_multi_material),
        m_enable_timelapse_print(print_config.timelapse_type.value == TimelapseType::tlSmooth),
        m_is_first_print(true),
        m_last_wipe_tower_print_z(print_config.z_offset.value),
        m_sparse_layers_skipped(wipe_tower_sparse_layers_skipped(print_config))
    {
        // Precomputed rather than accumulated while emitting, so that the clearance validator and
        // the emitter cannot disagree about where the compacted tower sits on any given layer.
        if (m_sparse_layers_skipped)
            m_compacted_tower_z = compute_compacted_wipe_tower_z(tool_changes, float(print_config.z_offset.value));
    }

    std::string prime(GCode &gcodegen);
    void next_layer() {
        ++ m_layer_idx;
        m_tool_change_idx = 0;
        if (m_layer_idx >= 0 && size_t(m_layer_idx) < m_local_z_tool_change_idx.size())
            m_local_z_tool_change_idx[size_t(m_layer_idx)] = 0;
        if (m_layer_idx >= 0 && size_t(m_layer_idx) < m_local_z_reserve_slot_idx.size())
            m_local_z_reserve_slot_idx[size_t(m_layer_idx)] = 0;
    }
    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer,
                            bool local_z_unplanned = false,
                            double local_z_nominal_layer_z = -1.);
    bool is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

    bool is_first_print() const { return m_is_first_print;}
    void set_is_first_print(bool is) { m_is_first_print = is; }

    bool enable_timelapse_print() const { return m_enable_timelapse_print; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    std::string append_tcr2(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;
    // Left / right edges of the wipe tower, for the planning of wipe moves.

    Vec2d extruder_offset_at(size_t extruder_id) const;

private:
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    const std::vector<Vec2d>                                     m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_local_z_tool_changes;
    const std::vector<std::vector<WipeTower::box_coordinates>>  &m_local_z_reserve_boxes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    std::vector<size_t>                                          m_local_z_tool_change_idx;
    std::vector<size_t>                                          m_local_z_reserve_slot_idx;

    // BBS
    Vec3d                                                        m_plate_origin;
    bool                                                         m_single_extruder_multi_material;
    bool                                                         m_enable_timelapse_print;
    bool                                                         m_is_first_print;
    double                                                       m_last_wipe_tower_print_z = 0.f;
    // wipe_tower_no_sparse_layers, as answered by the shared compaction rule rather than by the raw
    // option: smooth timelapse keeps a tower on every layer regardless.
    const bool                                                   m_sparse_layers_skipped;
    // Print z of the compacted tower per planned layer. Empty when the tower is not compacted.
    std::vector<float>                                           m_compacted_tower_z;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

struct LayerResult {
    std::string gcode;
    size_t      layer_id;
    // Is spiral vase post processing enabled for this layer?
    bool        spiral_vase_enable { false };
    // Should the cooling buffer content be flushed at the end of this layer?
    bool        cooling_buffer_flush { false };
	// Is indicating if this LayerResult should be processed, or it is just inserted artificial LayerResult.
    // It is used for the pressure equalizer because it needs to buffer one layer back.
    bool        nop_layer_result { false };
    // True for the last layer of a process_layers() run. Layer-time speed smoothing
    // uses this to drain its all-layer buffer and rewrite F.
    bool        last_layer { false };
    // Set by the cooling stage: CoolingBuffer slowed this layer down to slow_down_layer_time,
    // so its F words already carry the cooling speeds. Layer-time speed smoothing then never
    // slows it further (see LayerTimeSpeedSmoothingFilter).
    bool        cooling_slowed_down { false };

    static LayerResult make_nop_layer_result() { return {"", std::numeric_limits<coord_t>::max(), false, false, true, false, false}; }
};

namespace MultiNozzleUtils { class NozzleGroupResultBase; }

// Ultra (H2C rack): the per-logical-nozzle arrays the H2C change_filament template indexes
// with current_nozzle_id / next_nozzle_id, and the pure formatter for the in-extruder
// nozzle-change block. Ports of BambuStudio GCode.cpp:119-155 and WipeTower.cpp:3388-3568
// (with nozzle_change_line_count == 0). Free functions so libslic3r_tests can reach them -
// see tests/libslic3r/test_h2c_rack_nozzle_change.cpp.
std::vector<double>      get_nozzle_diameters_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result);
std::vector<std::string> get_nozzle_volume_types_by_nozzle_id(const MultiNozzleUtils::NozzleGroupResultBase *group_result);
std::string              format_nozzle_change_block(int  old_filament_id,
                                                    int  new_filament_id,
                                                    int  old_nozzle_id,
                                                    int  new_nozzle_id,
                                                    bool extruder_change,
                                                    bool dynamic_nozzle_map,
                                                    int  precool_temp,
                                                    int  physical_extruder);

class GCode {

public:
    GCode() :
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true),
        m_resonance_avoidance(true),
        m_enable_cooling_markers(false),
        m_enable_extrusion_role_markers(false),
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1),
        m_layer(nullptr),
        m_object_layer_over_raft(false),
        //m_volumetric_speed(0),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
        m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_silent_time_estimator_enabled(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        // BBS
        m_toolchange_count(0),
        m_nominal_z(0.)
        {}
    ~GCode() = default;

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);

    //BBS: set offset for gcode writer
    void set_gcode_offset(double x, double y) { m_writer.set_xy_offset(x, y); m_processor.set_xy_offset(x, y);}

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    const Point&    last_pos() const { return m_last_pos; }
    Vec2d           point_to_gcode(const Point &point) const;
    Point           gcode_to_point(const Vec2d &point) const;
    Vec2d point_to_gcode_quantized(const Point& point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser_integration.parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser_integration.parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    std::string     extrusion_role_to_string_for_parser(const ExtrusionRole &);

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    std::string     travel_to(const Point& point, ExtrusionRole role, std::string comment, double z = DBL_MAX);
    bool            needs_retraction(const Polyline& travel, ExtrusionRole role, LiftType& lift_type);
    std::string     retract(bool toolchange = false, bool is_last_retraction = false, LiftType lift_type = LiftType::NormalLift, ExtrusionRole role = erNone);
    std::string     unretract() { return m_writer.unlift() + m_writer.unretract(); }
    std::string     set_extruder(unsigned int extruder_id, double print_z, bool by_object=false);
    // Emit PA for this filament's active flow variant (Standard / High-Flow), not raw column = id.
    // reset_adaptive=false keeps the two set_extruder sites that historically skipped the reset.
    std::string     set_filament_pressure_advance(unsigned filament_id, bool reset_adaptive = true);
    // Orca: Adaptive PA. Tell the adaptive PA processor a tool change has just set the PA to `pa`.
    // Returns G-code to append right after that PA command (empty outside the layer pipeline).
    std::string     reset_adaptive_pa(double pa);
    bool is_BBL_Printer();

    // SoftFever
    static void assign_object_and_instance_ids(Print& print);
    std::string set_object_info(Print* print);

    // append full config to the given string
    static void append_full_config(const Print& print, std::string& str);

    // Object and support extrusions of the same PrintObject at the same print_z.
    // public, so that it could be accessed by free helper functions from GCode.cpp
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr), original_object(nullptr) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const PrintObject*  original_object; //BBS: used for shared object logic
        const Layer* 		layer()   const
        {
            if (object_layer != nullptr)
                return object_layer;

            if (support_layer != nullptr)
                return support_layer;

            return nullptr;
        }

        const PrintObject* 	object()   const
        {
            return (this->layer() != nullptr) ? this->layer()->object() : nullptr;
        }
        coordf_t            print_z() const
        {
            coordf_t sum_z = 0.;
            size_t count = 0;
            if (object_layer != nullptr) {
                sum_z += object_layer->print_z;
                count++;
            }

            if (support_layer != nullptr) {
                sum_z += support_layer->print_z;
                count++;
            }

            return sum_z / count;
        }
    };

private:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE *f, GCodeProcessor &processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        bool is_open() const { return f; }
        bool is_error() const;

        void flush();
        void close();

        // Write a string into a file.
        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        // Write a string into a file.
        // Add a newline, if the string does not end with a newline already.
        // Used to export a custom G-code section processed by the PlaceholderParser.
        void writeln(const std::string& what);

        // Formats and write into a file the given data.
        void write_format(const char* format, ...);

    private:
        FILE *f = nullptr;
        GCodeProcessor &m_processor;
    };
    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);

    std::string generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id);

    LayerResult process_layer(
        const Print                     &print,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		// Pairs of PrintObject index and its instance index.
		const std::vector<const PrintInstance*> *ordering,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        const size_t                     single_object_idx = size_t(-1),
        // BBS
        const bool                       prime_extruder = false);
    // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                                                         &print,
        const ToolOrdering                                                  &tool_ordering,
        const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
        GCodeOutputStream                                                   &output_stream);
    // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                             &print,
        const ToolOrdering                      &tool_ordering,
        std::vector<LayerToPrint>                layers_to_print,
        const size_t                             single_object_idx,
        GCodeOutputStream                       &output_stream,
        // BBS
        const bool                               prime_extruder = false);

    //BBS
    void check_placeholder_parser_failed();

    /*
     * Build scalar process options for custom G-code evaluation.
     * Flow-variant vectors are resolved for current_extruder_id, and explicit overrides take precedence.
    */
    DynamicConfig build_placeholder_process_config(unsigned int current_extruder_id,
                                                   const DynamicConfig *config_override) const;

    void            set_last_pos(const Point &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    // BBS
    std::string     change_layer(coordf_t print_z);
    using ConstExtrusionEntitiesPtr = std::vector<const ExtrusionEntity*>;
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string     extrude_entity(const ExtrusionEntity &entity, std::string description = "", double speed = -1.,
                                   const ConstExtrusionEntitiesPtr& region_perimeters = {},
                                   const WipeInwardSupport* wipe_support = nullptr);
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string     extrude_loop(ExtrusionLoop loop, std::string description, double speed = -1.,
                                 const ConstExtrusionEntitiesPtr& region_perimeters = {},
                                 const Point* start_point = nullptr, const WipeInwardSupport* wipe_support = nullptr);
    std::string     extrude_multi_path(ExtrusionMultiPath multipath, std::string description = "", double speed = -1.);
    std::string     extrude_path(ExtrusionPath path, std::string description = "", double speed = -1.);
    
    // Orca: Adaptive PA variables
    // Used for adaptive PA when extruding paths with multiple, varying flow segments.
    // This contains the sum of the mm3_per_mm values weighted by the length of each path segment.
    // The m_multi_flow_segment_path_pa_set constrains the PA change request to the first extrusion segment.
    // It sets the mm3_mm value for the adaptive PA post processor to be the average of that path
    // as calculated and stored in the m_multi_segment_path_average_mm3_per_mm value
    double          m_multi_flow_segment_path_average_mm3_per_mm = 0;
    bool            m_multi_flow_segment_path_pa_set = false;
    // Adaptive PA last set flow to enable issuing of PA change commands when adaptive PA for overhangs
    // is enabled
    double          m_last_mm3_mm = 0;
    // Orca: Adaptive PA code segment end

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface / erSupportTransition or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
            	// Non-owned references to LayerRegion::perimeters::entities
                ConstExtrusionEntitiesPtr perimeters;
            	// Non-owned references to LayerRegion::fills::entities
                ConstExtrusionEntitiesPtr infills;

                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;

	            enum Type {
	            	PERIMETERS,
	            	INFILL,
	            };

                // Appends perimeter/infill entities and writes don't indices of those that are not to be extruder as part of perimeter/infill wiping
                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;                                    // all extrusions for this island, grouped by regions

            // Fills in by_region_per_copy_cache and returns its reference.
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id, size_t label_object_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id), label_object_id(label_object_id) {}

		// Repository
		ObjectByExtruder		&object_by_extruder;
		// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
        //BBS: Unique id to label object to support skiping during printing
        const size_t             label_object_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const std::vector<LayerToPrint> 				&layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_perimeters(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool is_first_layer, bool is_infill_first, bool unsupported_loops_only = false);
    std::string     extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool ironing);
    std::string     extrude_support(const ExtrusionEntityCollection& support_fills, const ExtrusionRole support_extrusion_role);

    // BBS
    LiftType to_lift_type(ZHopType z_hop_types);

    std::set<ObjectID>              m_objsWithBrim; // indicates the objs with brim
    std::set<ObjectID>              m_objSupportsWithBrim; // indicates the objs' supports with brim
    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;

    ExtrusionQualityEstimator m_extrusion_quality_estimator;


    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    // Per-filament flow ratio / max volumetric speed / PA enable that _extrude
    // reads on every path, resolved in apply_print_config (the only place the
    // filament options of m_config change during an export).
    ResolvedFilamentFlow                m_filament_flow;
    DynamicConfig                       m_calib_config;
    // scaled G-code resolution
    double                              m_scaled_resolution;
    GCodeWriter                         m_writer;

    struct PlaceholderParserIntegration {
        void reset();
        void init(const GCodeWriter &config);
        void update_from_gcodewriter(const GCodeWriter &writer);
        void validate_output_vector_variables();

        PlaceholderParser                   parser;
        // For random number generator etc.
        PlaceholderParser::ContextData      context;
        // Collection of templates, on which the placeholder substitution failed.
        std::map<std::string, std::string>  failed_templates;
        // Input/output from/to custom G-code block, for returning position, retraction etc.
        DynamicConfig                       output_config;
        ConfigOptionFloats                 *opt_position { nullptr };
        ConfigOptionFloat                  *opt_zhop { nullptr };
        ConfigOptionFloats                 *opt_e_position { nullptr };
        ConfigOptionFloats                 *opt_e_retracted { nullptr };
        ConfigOptionFloats                 *opt_e_restart_extra { nullptr };
        ConfigOptionFloats                 *opt_extruded_volume { nullptr };
        ConfigOptionFloats                 *opt_extruded_weight { nullptr };
        ConfigOptionFloat                  *opt_extruded_volume_total { nullptr };
        ConfigOptionFloat                  *opt_extruded_weight_total { nullptr };
        // Caches of the data passed to the script.
        size_t                              num_extruders;
        std::vector<double>                 position;
        std::vector<double>                 e_position;
        std::vector<double>                 e_retracted;
        std::vector<double>                 e_restart_extra;
    } m_placeholder_parser_integration;

    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    bool                                m_enable_loop_clipping;
    //resonance avoidance
    bool                                m_resonance_avoidance; 
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _OVERHANG_FAN_START, _OVERHANG_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    
    bool m_enable_exclude_object;
    std::vector<size_t> m_label_objects_ids;
    // Object label names by instance, built on first use from the ids assign_object_and_instance_ids() assigns.
    std::unordered_map<const PrintInstance*, std::string> m_instance_names;
    const std::string& instance_name(const PrintInstance &instance);
    std::string _encode_label_ids_to_base64(std::vector<size_t> ids);
    // ORCA: Add support for role based fan speed control
    std::array<bool, ExtrusionRole::erCount> m_is_role_based_fan_on;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Keeps track of the last extrusion role passed to the processor
    ExtrusionRole                       m_last_processor_extrusion_role;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. In sequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    // m_layer is an object layer and it is being printed over raft surface.
    bool                                m_object_layer_over_raft;
    //double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;
    // To ignore gapfill role for retract_lift_enforce
    ExtrusionRole                       m_last_notgapfill_extrusion_role;
    // Support for G-Code Processor
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    // ZAA (Z contouring): true while the writer Z is still wherever the last contoured path left
    // it. Nothing acts on the ZAA Z-reset unless contouring actually happened, so a build with
    // zaa_enabled off emits byte-identical G-code.
    bool                                m_zaa_z_dirty{ false };
    float                               m_max_layer_z{ 0.0f };
    // Filament mass (g) printed up to the previous layer change, for curr_layer_mass.
    double                              m_last_layer_accumulated_mass{ 0.0 };
    float                               m_last_width{ 0.0f };

    // SM_Orca
    float                               m_next_wipe_x {0.0f};
    float                               m_next_wipe_y {0.0f};
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    double                              m_last_mm3_per_mm;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    // Always check gcode placeholders when building in debug mode.
#if !defined(NDEBUG)
#define ORCA_CHECK_GCODE_PLACEHOLDERS 1
#endif
    
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    std::map<std::string, std::vector<std::string>> m_placeholder_error_messages;
#endif

    Point                               m_last_pos;
    bool                                m_last_pos_defined;

    std::unique_ptr<CoolingBuffer>                 m_cooling_buffer;
    // After CoolingBuffer / before FanMover. Null when mode is Off (streaming, no extra buffer).
    std::unique_ptr<LayerTimeSpeedSmoothingFilter> m_layer_time_speed_smoothing;
    std::unique_ptr<SpiralVase>                    m_spiral_vase;

    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    
    std::unique_ptr<AdaptivePAProcessor>      m_pa_processor;
    // True while a layer pipeline that ends in the AdaptivePAProcessor filter is running.
    bool                                      m_pa_reset_in_band { false };

    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;

    std::unique_ptr<SmallAreaInfillFlowCompensator> m_small_area_infill_flow_compensator;
    
    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    int m_timelapse_warning_code = 0;
    bool m_support_traditional_timelapse = true;
    // BBS: this export gets Bambu's idle-nozzle pre-cooling (GCode/PreCoolingInjector); set in _do_export.
    bool m_pre_cooling_markers = false;
    // BBL timelapse on a dual-nozzle machine: the (logical, 0-based) extruder that takes the
    // per-layer photo, BambuStudio's ToolOrdering::get_most_used_extruder(). Set at the start of
    // every process_layers() run; 0 whenever the plate has no nozzle grouping.
    int m_timelapse_photo_extruder = 0;
    int timelapse_extruder_of_filament(int filament_id) const;
    int timelapse_physical_extruder(int extruder_id) const;

    // BambuStudio's timelapse position picker (GCode/TimelapsePosPicker): the safe spot the
    // time_lapse_gcode hands the firmware (M9711 U/V), so both nozzles of a dual-nozzle machine
    // are photographed the same way. Initialised at the start of every export.
    TimelapsePosPicker m_timelapse_pos_picker;
    // Objects printed so far in a print-by-object export, the current one last (BambuStudio's
    // m_printed_objects); the picker keeps clear of the finished ones.
    std::vector<const PrintObject*> m_printed_objects;

    // BambuStudio 2.8 farthest-point timelapse (machine option farthest_point_timelapse, traditional
    // mode, non-i3): take the photo where the layer reaches farthest from the camera.
    struct FarthestPointTimelapseContext {
        // Whether farthest-point timelapse is active for this layer
        bool    enabled{false};
        // The farthest extrusion point from camera (0,0) in global scaled coordinates (includes inst.shift)
        Point   farthest_point{0, 0};
        // farthest_point in mm (the frame of point_to_gcode() without the extruder offset)
        Vec2d   farthest_gcode_pos{0, 0};
        // Extruder index (0-based) that prints the farthest point
        int     farthest_extruder_id{0};
        // Whether the farthest point is printed by the photo head (the most used extruder)
        bool    farthest_is_photo_head{false};
        // Whether the photo has already been inserted on this layer (inline or at a tool change)
        bool    inserted_this_layer{false};
        // The photo head: m_timelapse_photo_extruder
        int     most_used_extruder{0};
    };
    FarthestPointTimelapseContext m_farthest_point_timelapse;
    void compute_farthest_point(const std::vector<LayerToPrint> &layers, const LayerTools &layer_tools,
                                const std::map<std::pair<const SupportLayer *, ExtrusionRole>, unsigned int> &support_filaments);

    struct TimelapseGCodeResult {
        std::string gcode;
        Point       safe_pos{DefaultTimelapsePos};
    };
    // BambuStudio's generate_timelapse_gcode: expands time_lapse_gcode for the active filament, with
    // the picked safe position (none when skip_pos_pick: the inline farthest-point photo).
    TimelapseGCodeResult generate_timelapse_gcode(const Print &print, coordf_t print_z, int photo_extruder, bool skip_pos_pick = false);
    // Inline farthest-point photo: called with the end point of every extrusion move.
    void check_and_insert_inline_timelapse(std::string &gcode, const Point &endpoint_scaled);

    bool m_silent_time_estimator_enabled;

    // Processor
    GCodeProcessor m_processor;

    //some post-processing on the file, with their data class
    std::unique_ptr<FanMover> m_fan_mover;

    // BBS
    Print* m_curr_print = nullptr;
    unsigned int m_toolchange_count;
    // Ultra (H2C 3MF schema): the order filaments were entered, and the logical nozzle each
    // entry ran on. BambuStudio records these on GCode and copies them to GCodeProcessorResult
    // for Metadata/filament_sequence.json; the H2C reads the nozzle order from there.
    std::vector<unsigned int> m_filament_change_sequence;
    std::vector<unsigned int> m_nozzle_change_sequence;
    // Records one filament entry. -1 nozzle id == "no dynamic nozzle map", as in BBS's
    // NOZZLE_ID_FOR_GCODE macro.
    void record_filament_change(unsigned int filament_id);
    // Ultra (H2C rack): the in-extruder nozzle-change block emitted around a rack change.
    std::string nozzle_change_gcode(int old_filament_id, int new_filament_id, int old_nozzle_id, int new_nozzle_id, bool extruder_change) const;
    coordf_t m_nominal_z;
    bool m_need_change_layer_lift_z = false;
    int m_start_gcode_filament = -1;

    // Bambu two-extruder printers (H2D/H2C/X2D). A nozzle keeps the last filament loaded into it while
    // the other extruder prints, so a change back into it has to purge THAT filament, which is what
    // Bambu Studio's switch_to_nozzle() does (GCode.cpp:8180). Key: nozzle_key_for_filament().
    std::map<int, int> m_filament_in_nozzle;
    // The nozzle a filament prints from: the logical nozzle id on a rack machine (H2C), else the
    // 1-based extruder from filament_map; -1 on a printer with one extruder.
    int  nozzle_key_for_filament(int filament_id) const;
    // The logical (0-based) extruder a filament is assigned to, -1 when there is only one.
    int  logical_extruder_for_filament(int filament_id) const;
    // M104/M109 T on a Bambu two-extruder printer names the PHYSICAL extruder (the start template's
    // "T{filament_map[..] % 2}", the end G-code's T0/T1, Bambu Studio's pre-cool lines), never a
    // filament slot. Returns filament_id unchanged on every other printer.
    int  temperature_tool_for_filament(int filament_id) const;
    bool is_bbl_multi_extruder() const;
    void note_filament_loaded(int filament_id);
    // Purge volume for a change old -> new that crosses extruders: the flush from the filament the
    // target nozzle still holds, 0 when it holds nothing or already holds the new filament.
    // Returns false (and leaves volume alone) when old and new share an extruder.
    bool cross_extruder_flush_volume(int old_filament_id, int new_filament_id, float &volume) const;

    std::set<unsigned int>                  m_initial_layer_extruders;
    // BBS
    int get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const;
    // BBS: max bed temperature over all used extruders, so that the bed temperature always
    // accommodates the highest-temperature filament of a compatible mixed print (e.g. PLA + TPU).
    int get_bed_temperature_max(const Print& print, const bool is_first_layer) const;

    std::string _extrude(const ExtrusionPath &path, const std::string &path_description = "", double speed = -1);
    bool _needSAFC(const ExtrusionPath &path);

    // Snapmaker: flow variant — read a process-domain vector option
    template<typename VectorOption>
    auto process_flow_value(const VectorOption &opt) const -> decltype(opt.get_at(0))
    {
        // The slot is resolved once per export (m_filament_flow.process_config_idx); the same
        // slot get_value_at(m_config, opt, ConfigFlowDomain::Process, id) picks.
        return opt.get_at(m_filament_flow.process_config_idx_for(m_config, m_writer.extruder() != nullptr ? m_writer.extruder()->id() : 0));
    }

    void print_machine_envelope(GCodeOutputStream &file, Print &print);
    void _print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, bool wait);
    void _print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    // On the first printing layer. This flag triggers first layer speeds.
    //BBS
    bool    on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0 && abs(m_layer->bottom_z()) < EPSILON; }
    int layer_id() const {
        if (m_layer == nullptr)
            return -1;
        return m_layer->id();
    }
    // To control print speed of 1st object layer over raft interface.
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder,
        unsigned int                                           extruder_id,
        size_t                                                 object_idx,
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder,
        unsigned int                                            extruder_id,
        size_t                                                  object_idx,
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    friend class Wipe;
    friend class WipeTowerIntegration;
    friend class PressureEqualizer;
    friend class Print;
    friend class SmallAreaInfillFlowCompensator;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order = false);

// The overhang data ExtrusionQualityEstimator needs for the object layers in `layers` that process_layer() prepares it
// for, computed ahead of the generator.
std::vector<PrecomputedOverhangLayer> precompute_overhang_layers(const std::vector<GCode::LayerToPrint> &layers);

}

#endif
