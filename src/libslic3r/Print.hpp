#ifndef slic3r_Print_hpp_
#define slic3r_Print_hpp_

#include "Fill/FillAdaptive.hpp"
#include "Fill/FillLightning.hpp"
#include "PrintBase.hpp"

#include "BoundingBox.hpp"
#include "Polygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"
#include "Point.hpp"
#include "Slicing.hpp"
#include "TriangleMeshSlicer.hpp"
#include "GCode/ToolOrdering.hpp"
#include "MultiNozzleUtils.hpp" // Ultra (dual-nozzle): NozzleGroupResult held on Print
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "GCode/ThumbnailData.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "MixedFilament.hpp"
#include "libslic3r.h"

#include <Eigen/Geometry>

#include <functional>
#include <optional>
#include <set>
#include <vector>

#include "calib.hpp"

namespace Slic3r {

class GCode;
class Layer;
class ModelObject;
class Print;
class PrintObject;
class SupportLayer;
// BBS
class TreeSupportData;
class TreeSupport;
class PresetCollection;
class PresetBundle;
struct NozzleFilamentRuleMismatch;
struct ExtrusionLayers;

#define MAX_OUTER_NOZZLE_DIAMETER   4
// BBS: move from PrintObjectSlice.cpp
struct VolumeSlices
{
    ObjectID                volume_id;
    std::vector<ExPolygons> slices;
};

struct groupedVolumeSlices
{
    int                     groupId = -1;
    std::vector<ObjectID>   volume_ids;
    ExPolygons              slices;
};

// Phase A local-Z dithering planner cache.
struct LocalZInterval
{
    size_t layer_id { 0 };
    double z_lo { 0.0 };
    double z_hi { 0.0 };
    double base_height { 0.0 };
    double sublayer_height { 0.0 };
    bool   has_mixed_paint { false };
    size_t first_sublayer_idx { 0 };
    size_t sublayer_count { 0 };
};

struct SubLayerPlan
{
    size_t layer_id { 0 };
    size_t pass_index { 0 };
    bool   split_interval { false };
    double z_lo { 0.0 };
    double z_hi { 0.0 };
    double print_z { 0.0 };
    double flow_height { 0.0 };
    size_t dependency_group { 0 };
    size_t dependency_order { 0 };
    std::vector<ExPolygons> painted_masks_by_extruder;
    std::vector<ExPolygons> fixed_painted_masks_by_extruder;
    ExPolygons              base_masks;
};

enum SupportNecessaryType {
    NoNeedSupp=0,
    SharpTail,
    Cantilever,
    LargeOverhang,
};

namespace FillAdaptive {
    struct Octree;
    struct OctreeDeleter;
    using OctreePtr = std::unique_ptr<Octree, OctreeDeleter>;
};

namespace FillLightning {
    class Generator;
    struct GeneratorDeleter;
    using GeneratorPtr = std::unique_ptr<Generator, GeneratorDeleter>;
}; // namespace FillLightning

// Print step IDs for keeping track of the print state.
// The Print steps are applied in this order.
enum PrintStep {
    psWipeTower,
    // Ordering of the tools on PrintObjects for a multi-material print.
    // psToolOrdering is a synonym to psWipeTower, as the Wipe Tower calculates and modifies the ToolOrdering,
    // while if printing without the Wipe Tower, the ToolOrdering is calculated as well.
    psToolOrdering = psWipeTower,
    psSkirtBrim,
    // Last step before G-code export, after this step is finished, the initial extrusion path preview
    // should be refreshed.
    psSlicingFinished = psSkirtBrim,
    psGCodeExport,
    psConflictCheck,
    psCount
};

enum PrintObjectStep {
    posSlice, posPerimeters,posEstimateCurledExtrusions, posPrepareInfill,
    posInfill, posIroning, posContouring, posSupportMaterial, posSimplifyPath, posSimplifySupportPath,
    // BBS
    posDetectOverhangsForLift,
    posSimplifyWall, posSimplifyInfill,
    posCount,
};

// Which of a PrintRegion's three configured filaments an extrusion that lives in a
// LayerRegion's *fills* collection has to follow.
//
// The fills collection is not homogeneous: besides real infill it also carries the
// perimeter generator's GAP FILL, which Fill::make_fill() copies over from
// LayerRegion::thin_fills (Fill/Fill.cpp, "add thin fill regions"). Deciding the filament
// from the collection's BUCKET ("this came out of layerm->fills, so it is infill") is
// therefore wrong for gap fill; it has to be decided from the extrusion ROLE. This helper
// is that decision, shared by GCode::process_layer's two extruder-id lambdas so the rule
// exists once.
enum class FillFilamentSource {
    Wall,
    SolidInfill,
    SparseInfill,
};

// `role` is the role of the collection's leading entity, as GCode::process_layer reads it.
FillFilamentSource fill_filament_source(const PrintRegionConfig &config, ExtrusionRole role);

// A PrintRegion object represents a group of volumes to print
// sharing the same config (including the same assigned extruder(s))
class PrintRegion
{
public:
    PrintRegion() = default;
    PrintRegion(const PrintRegionConfig &config);
    PrintRegion(const PrintRegionConfig &config, const size_t config_hash, int print_object_region_id = -1) : m_config(config), m_config_hash(config_hash), m_print_object_region_id(print_object_region_id) {}
    PrintRegion(PrintRegionConfig &&config);
    PrintRegion(PrintRegionConfig &&config, const size_t config_hash, int print_object_region_id = -1) : m_config(std::move(config)), m_config_hash(config_hash), m_print_object_region_id(print_object_region_id) {}
    ~PrintRegion() = default;

// Methods NOT modifying the PrintRegion's state:
public:
    const PrintRegionConfig&    config() const throw() { return m_config; }
    size_t                      config_hash() const throw() { return m_config_hash; }
    // Identifier of this PrintRegion in the list of Print::m_print_regions.
    int                         print_region_id() const throw() { return m_print_region_id; }
    int                         print_object_region_id() const throw() { return m_print_object_region_id; }
	// 1-based extruder identifier for this region and role.
	unsigned int 				extruder(FlowRole role) const;
    Flow                        flow(const PrintObject &object, FlowRole role, double layer_height, bool first_layer = false) const;
    // Average diameter of nozzles participating on extruding this region.
    coordf_t                    nozzle_dmr_avg(const PrintConfig &print_config) const;
    // Average diameter of nozzles participating on extruding this region.
    coordf_t                    bridging_height_avg(const PrintConfig &print_config) const;

    // Collect 0-based extruder indices used to print this region's object.
	void                        collect_object_printing_extruders(const Print &print, std::vector<unsigned int> &object_extruders) const;
	static void                 collect_object_printing_extruders(const PrintConfig &print_config, const PrintRegionConfig &region_config, const bool has_brim, std::vector<unsigned int> &object_extruders);

// Methods modifying the PrintRegion's state:
public:
    void                        set_config(const PrintRegionConfig &config) { m_config = config; m_config_hash = m_config.hash(); }
    void                        set_config(PrintRegionConfig &&config) { m_config = std::move(config); m_config_hash = m_config.hash(); }
    void                        config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false)
                                        { m_config.apply_only(other, keys, ignore_nonexistent); m_config_hash = m_config.hash(); }
private:
    friend Print;
    friend void print_region_ref_inc(PrintRegion&);
    friend void print_region_ref_reset(PrintRegion&);
    friend int  print_region_ref_cnt(const PrintRegion&);

    PrintRegionConfig  m_config;
    size_t             m_config_hash;
    int                m_print_region_id { -1 };
    int                m_print_object_region_id { -1 };
    int                m_ref_cnt { 0 };
};

inline bool operator==(const PrintRegion &lhs, const PrintRegion &rhs) { return lhs.config_hash() == rhs.config_hash() && lhs.config() == rhs.config(); }
inline bool operator!=(const PrintRegion &lhs, const PrintRegion &rhs) { return ! (lhs == rhs); }

template<typename T>
class ConstVectorOfPtrsAdaptor {
public:
    // Returning a non-const pointer to const pointers to T.
    T * const *             begin() const { return m_data->data(); }
    T * const *             end()   const { return m_data->data() + m_data->size(); }
    const T*                front() const { return m_data->front(); }
    // BBS
    const T*                back()  const { return m_data->back(); }
    size_t                  size()  const { return m_data->size(); }
    bool                    empty() const { return m_data->empty(); }
    const T*                operator[](size_t i) const { return (*m_data)[i]; }
    const T*                at(size_t i) const { return m_data->at(i); }
    std::vector<const T*>   vector() const { return std::vector<const T*>(this->begin(), this->end()); }
protected:
    ConstVectorOfPtrsAdaptor(const std::vector<T*> *data) : m_data(data) {}
private:
    const std::vector<T*> *m_data;
};

typedef std::vector<Layer*>       LayerPtrs;
typedef std::vector<const Layer*> ConstLayerPtrs;
class ConstLayerPtrsAdaptor : public ConstVectorOfPtrsAdaptor<Layer> {
    friend PrintObject;
    ConstLayerPtrsAdaptor(const LayerPtrs *data) : ConstVectorOfPtrsAdaptor<Layer>(data) {}
};

typedef std::vector<SupportLayer*>        SupportLayerPtrs;
typedef std::vector<const SupportLayer*>  ConstSupportLayerPtrs;
class ConstSupportLayerPtrsAdaptor : public ConstVectorOfPtrsAdaptor<SupportLayer> {
    friend PrintObject;
    ConstSupportLayerPtrsAdaptor(const SupportLayerPtrs *data) : ConstVectorOfPtrsAdaptor<SupportLayer>(data) {}
};

// Single instance of a PrintObject.
// As multiple PrintObjects may be generated for a single ModelObject (their instances differ in rotation around Z),
// ModelObject's instancess will be distributed among these multiple PrintObjects.
struct PrintInstance
{
    // Parent PrintObject
    PrintObject 		*print_object;
    // Source ModelInstance of a ModelObject, for which this print_object was created.
	const ModelInstance *model_instance;
	// Shift of this instance's center into the world coordinates.
	Point 				 shift;
    
    BoundingBoxf3   get_bounding_box();
    Polygon get_convex_hull_2d();
    // SoftFever
    //
    // instance id
    // Numbered by GCode::assign_object_and_instance_ids() before any G-code is written. Default
    // to 0 so that a PrintInstance built anywhere else can never carry an indeterminate label.
    size_t               id{0};
    // Orca: unique id used by marlin/rrf cancel object feature
    size_t               unique_id{0};

    //BBS: instance_shift is too large because of multi-plate, apply without plate offset.
    Point shift_without_plate_offset() const;
};

typedef std::vector<PrintInstance> PrintInstances;

class PrintObjectRegions
{
public:
    // Bounding box of a ModelVolume transformed into the working space of a PrintObject, possibly
    // clipped by a layer range modifier.
    // Only Eigen types of Nx16 size are vectorized. This bounding box will not be vectorized.
    static_assert(sizeof(Eigen::AlignedBox<float, 3>) == 24, "Eigen::AlignedBox<float, 3> is not being vectorized, thus it does not need to be aligned");
    using BoundingBox = Eigen::AlignedBox<float, 3>;
    struct VolumeExtents {
        ObjectID             volume_id;
        BoundingBox          bbox;
    };

    struct VolumeRegion
    {
        // ID of the associated ModelVolume.
        const ModelVolume   *model_volume { nullptr };
        // Index of a parent VolumeRegion.
        int                  parent { -1 };
        // Pointer to PrintObjectRegions::all_regions, null for a negative volume.
        PrintRegion         *region { nullptr };
        // Pointer to VolumeExtents::bbox.
        const BoundingBox   *bbox { nullptr };
        // To speed up merging of same regions.
        const VolumeRegion  *prev_same_region { nullptr };
    };

    struct PaintedRegion
    {
        // 1-based extruder identifier.
        unsigned int     extruder_id;
        // Index of a parent VolumeRegion.
        int              parent { -1 };
        // Pointer to PrintObjectRegions::all_regions.
        PrintRegion     *region { nullptr };
    };

    struct LayerRangeRegions;

    struct FuzzySkinPaintedRegion
    {
        enum class ParentType { VolumeRegion, PaintedRegion };

        ParentType   parent_type { ParentType::VolumeRegion };
        // Index of a parent VolumeRegion or PaintedRegion.
        int          parent { -1 };
        // Pointer to PrintObjectRegions::all_regions.
        PrintRegion *region { nullptr };

        PrintRegion *parent_print_object_region(const LayerRangeRegions &layer_range) const;
        int          parent_print_object_region_id(const LayerRangeRegions &layer_range) const;
    };

    // One slice over the PrintObject (possibly the whole PrintObject) and a list of ModelVolumes and their bounding boxes
    // possibly clipped by the layer_height_range.
    struct LayerRangeRegions
    {
        t_layer_height_range        layer_height_range;
        // Config of the layer range, null if there is just a single range with no config override.
        // Config is owned by the associated ModelObject.
        const DynamicPrintConfig*   config { nullptr };
        // Volumes sorted by ModelVolume::id().
        std::vector<VolumeExtents>  volumes;

        // Sorted in the order of their source ModelVolumes, thus reflecting the order of region clipping, modifier overrides etc.
        std::vector<VolumeRegion>           volume_regions;
        std::vector<PaintedRegion>          painted_regions;
        std::vector<FuzzySkinPaintedRegion> fuzzy_skin_painted_regions;

        bool has_volume(const ObjectID id) const {
            auto it = lower_bound_by_predicate(this->volumes.begin(), this->volumes.end(), [id](const VolumeExtents &l) { return l.volume_id < id; });
            return it != this->volumes.end() && it->volume_id == id;
        }
    };

    std::vector<std::unique_ptr<PrintRegion>>   all_regions;
    std::vector<LayerRangeRegions>              layer_ranges;
    // Transformation of this ModelObject into one of the associated PrintObjects (all PrintObjects derived from a single modelObject differ by a Z rotation only).
    // This transformation is used to calculate VolumeExtents.
    Transform3d                                 trafo_bboxes;
    std::vector<ObjectID>                       cached_volume_ids;

    void ref_cnt_inc() { ++ m_ref_cnt; }
    void ref_cnt_dec() { if (-- m_ref_cnt == 0) delete this; }
    void clear() {
        all_regions.clear();
        layer_ranges.clear();
        cached_volume_ids.clear();
    }

private:
    friend class PrintObject;
    // Number of PrintObjects generated from the same ModelObject and sharing the regions.
    // ref_cnt could only be modified by the main thread, thus it does not need to be atomic.
    size_t                                      m_ref_cnt{ 0 };
};

class PrintObject : public PrintObjectBaseWithState<Print, PrintObjectStep, posCount>
{
private: // Prevents erroneous use by other classes.
    typedef PrintObjectBaseWithState<Print, PrintObjectStep, posCount> Inherited;

public:
    // Size of an object: XYZ in scaled coordinates. The size might not be quite snug in XY plane.
    const Vec3crd&               size() const			{ return m_size; }
    const PrintObjectConfig&     config() const         { return m_config; }
    void                         configBrimWidth(double m)      {m_config.brim_width.value = m; }
    ConstLayerPtrsAdaptor        layers() const         { return ConstLayerPtrsAdaptor(&m_layers); }
    ConstSupportLayerPtrsAdaptor support_layers() const { return ConstSupportLayerPtrsAdaptor(&m_support_layers); }
    const Transform3d&           trafo() const          { return m_trafo; }
    // Trafo with the center_offset() applied after the transformation, to center the object in XY before slicing.
    Transform3d                  trafo_centered() const
        { Transform3d t = this->trafo(); t.pretranslate(Vec3d(- unscale<double>(m_center_offset.x()), - unscale<double>(m_center_offset.y()), 0)); return t; }
    const PrintInstances&        instances() const      { return m_instances; }
    PrintInstances &instances() { return m_instances; }

    // Whoever will get a non-const pointer to PrintObject will be able to modify its layers.
    LayerPtrs&                   layers()               { return m_layers; }
    SupportLayerPtrs&            support_layers()       { return m_support_layers; }

    template<typename PolysType>
    static void remove_bridges_from_contacts(
        const Layer* lower_layer,
        const Layer* current_layer,
        float extrusion_width,
        PolysType* overhang_regions,
        float max_bridge_length = scale_(10),
        bool break_bridge=false);

    // Bounding box is used to align the object infill patterns, and to calculate attractor for the rear seam.
    // The bounding box may not be quite snug.
    BoundingBox                  bounding_box() const   { return BoundingBox(Point(- m_size.x() / 2, - m_size.y() / 2), Point(m_size.x() / 2, m_size.y() / 2)); }
    // Height is used for slicing, for sorting the objects by height for sequential printing and for checking vertical clearence in sequential print mode.
    // The height is snug.
    coord_t 				     height() const         { return m_size.z(); }
    double                      max_z() const         { return m_max_z; }
    // Centering offset of the sliced mesh from the scaled and rotated mesh of the model.
    const Point& 			     center_offset() const  { return m_center_offset; }

    // BBS
    void generate_support_preview();
    const std::vector<VolumeSlices>& firstLayerObjSlice() const { return firstLayerObjSliceByVolume; }
    std::vector<VolumeSlices>& firstLayerObjSliceMod() { return firstLayerObjSliceByVolume; }
    const std::vector<groupedVolumeSlices>& firstLayerObjGroups() const { return firstLayerObjSliceByGroups; }
    std::vector<groupedVolumeSlices>& firstLayerObjGroupsMod() { return firstLayerObjSliceByGroups; }

    bool                         has_brim() const       {
        return ((this->config().brim_type != btNoBrim && this->config().brim_width.value > 0.) || this->config().brim_type == btAutoBrim
            || (this->config().brim_type == btPainted && !this->model_object()->brim_points.empty()))
            && ! this->has_raft();
    }

    // BBS
    const ExtrusionEntityCollection& object_skirt() const {
        return m_skirt;
    }

    // This is the *total* layer count (including support layers)
    // this value is not supposed to be compared with Layer::id
    // since they have different semantics.
    size_t 			total_layer_count() const { return this->layer_count() + this->support_layer_count(); }
    size_t 			layer_count() const { return m_layers.size(); }
    void 			clear_layers();
    const Layer* 	get_layer(int idx) const { return m_layers[idx]; }
    Layer* 			get_layer(int idx) 		 { return m_layers[idx]; }
    // Get a layer exactly at print_z.
    const Layer*	get_layer_at_printz(coordf_t print_z) const;
    Layer*			get_layer_at_printz(coordf_t print_z);
    // Get a layer approximately at print_z.
    const Layer*	get_layer_at_printz(coordf_t print_z, coordf_t epsilon) const;
    Layer*			get_layer_at_printz(coordf_t print_z, coordf_t epsilon);
    int             get_layer_idx_get_printz(coordf_t print_z, coordf_t epsilon);
    // BBS
    const Layer*    get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) const;
    Layer*          get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon);

    // Get the first layer approximately bellow print_z.
    const Layer*	get_first_layer_bellow_printz(coordf_t print_z, coordf_t epsilon) const;

    // print_z: top of the layer; slice_z: center of the layer.
    Layer*          add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z);

    // BBS
    SupportLayer* add_tree_support_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z);
    std::shared_ptr<TreeSupportData> alloc_tree_support_preview_cache();
    void clear_tree_support_preview_cache() { m_tree_support_preview_cache.reset(); }
    const std::vector<LocalZInterval>& local_z_intervals() const { return m_local_z_intervals; }
    const std::vector<SubLayerPlan>&   local_z_sublayer_plan() const { return m_local_z_sublayer_plan; }
    void                                set_local_z_plan(std::vector<LocalZInterval> intervals, std::vector<SubLayerPlan> sublayers)
    {
        m_local_z_intervals = std::move(intervals);
        m_local_z_sublayer_plan = std::move(sublayers);
    }
    void                                clear_local_z_plan()
    {
        m_local_z_intervals.clear();
        m_local_z_sublayer_plan.clear();
    }

    size_t          support_layer_count() const { return m_support_layers.size(); }
    void            clear_support_layers();
    SupportLayer*   get_support_layer(int idx) { return idx<m_support_layers.size()? m_support_layers[idx]:nullptr; }
    const SupportLayer* get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon) const;
    SupportLayer*   get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon);
    SupportLayer*   add_support_layer(int id, int interface_id, coordf_t height, coordf_t print_z);
    SupportLayerPtrs::iterator insert_support_layer(SupportLayerPtrs::iterator pos, size_t id, size_t interface_id, coordf_t height, coordf_t print_z, coordf_t slice_z);

    // Initialize the layer_height_profile from the model_object's layer_height_profile, from model_object's layer height table, or from slicing parameters.
    // Returns true, if the layer_height_profile was changed.
    static bool     update_layer_height_profile(const ModelObject &model_object,
                                                const SlicingParameters &slicing_parameters,
                                                std::vector<coordf_t> &layer_height_profile,
                                                const PrintObject *print_object = nullptr);

    // Collect the slicing parameters, to be used by variable layer thickness algorithm,
    // by the interactive layer height editor and by the printing process itself.
    // The slicing parameters are dependent on various configuration values
    // (layer height, first layer height, raft settings, print nozzle diameter etc).
    const SlicingParameters&    slicing_parameters() const { return m_slicing_params; }
    // Orca: XYZ shrinkage compensation has introduced the const Vec3d &object_shrinkage_compensation parameter to the function below
    static SlicingParameters    slicing_parameters(const DynamicPrintConfig &full_config, const ModelObject &model_object, float object_max_z, const Vec3d &object_shrinkage_compensation);

    size_t                      num_printing_regions() const throw() { return m_shared_regions->all_regions.size(); }
    const PrintRegion&          printing_region(size_t idx) const throw() { return *m_shared_regions->all_regions[idx].get(); }
    //FIXME returing all possible regions before slicing, thus some of the regions may not be slicing at the end.
    std::vector<std::reference_wrapper<const PrintRegion>> all_regions() const;
    const PrintObjectRegions*   shared_regions() const throw() { return m_shared_regions; }

    bool                        has_support()           const { return m_config.enable_support || m_config.enforce_support_layers > 0; }
    bool                        has_raft()              const { return m_config.raft_layers > 0; }
    bool                        has_support_material()  const { return this->has_support() || this->has_raft(); }
    // Checks if the model object is painted using the multi-material painting gizmo.
    bool                        is_mm_painted()         const { return this->model_object()->is_mm_painted(); }
    // Checks if the model object is painted using the fuzzy skin painting gizmo.
    bool                        is_fuzzy_skin_painted() const { return this->model_object()->is_fuzzy_skin_painted(); }
    // Paint Depth Stage 2 (Task 3 item 1, docs/superpowers/specs/2026-08-31-paint-depth-design.md
    // Stage 2(a), docs/superpowers/plans/2026-08-31-paint-depth.md Task 3 item 1): true when this
    // object has painted regions AND paint depth is bounded (paint_depth_mode != unlimited) - i.e.
    // bleed path (c), bare dark/light Z interfaces. When true, PrintObject.cpp's
    // detect_surfaces_type() and discover_vertical_shells() treat the object as if
    // interface_shells were enabled (solid skin at every region boundary, color boundaries
    // included), the same mechanism the "Interface shells" setting already provides - see those
    // call sites for why this OR's into interface_shells rather than reclassifying only
    // paint-caused boundaries (LayerRegion::slices carries no "why did this region differ"
    // provenance to distinguish a color split from an unrelated modifier/volume split).
    bool                        has_bounded_paint_depth() const { return this->is_mm_painted() && m_config.paint_depth_mode.value != pdmUnlimited; }

    // returns 0-based indices of extruders used to print the object (without brim, support and other helper extrusions)
    std::vector<unsigned int>   object_extruders() const;

    // Called by make_perimeters()
    void slice();

    // Ultra (support groups): a set of MODEL_PART volumes that resolve to the same support
    // configuration. Group 0 is always the default group - the object's own Support settings -
    // even when no part carries an override, so downstream code can always address it.
    // docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 3.4.
    struct SupportGroup {
        // Display name. "" = the default group (parts with no support_group key).
        std::string                      name;
        // The object's PrintObjectConfig with this group's part-level support overrides applied.
        PrintObjectConfig                config;
        // The MODEL_PART volumes resolving to this group, in ModelObject::volumes order.
        std::vector<const ModelVolume*>  volumes;
        // Per object layer, the union of `volumes` sliced at that layer's slice_z. Empty until
        // Stage 3 fills it; nothing in Stage 2 reads it.
        std::vector<Polygons>            mask;
    };
    // Groups keyed by RESOLVED config, not by name: two parts land in the same group iff their
    // configs do not differ over the part-level support key set. A part whose override happens to
    // equal the object value therefore collapses into the default group and size() stays 1, which
    // is the whole of the off-mode guarantee - callers must take today's code path unchanged when
    // size() == 1.
    std::vector<SupportGroup>   support_groups() const;
    // True when any MODEL_PART volume of `object` asks for a soluble interface. See 3.6: a
    // per-part top Z distance cannot be honoured, so the strictest group wins object-wide.
    static bool                 support_groups_want_soluble(const ModelObject &object);
    // Ultra (support groups, plan Stage 3 3.2): fill SupportGroup::mask - per object layer, the
    // union of that group's volumes sliced at the layer's slice_z. Every group gets its own parts,
    // group 0 included: it is the complement at CLAIM level, not at mask level, and its own
    // footprint is what stops another group's claim from reaching across it. A no-op when
    // `groups.size() <= 1`, so the off-mode path never slices anything extra.
    void                        support_group_masks(std::vector<SupportGroup> &groups) const;
    // Ultra (support groups, plan Stage 3 3.7 / R3.5): true when some MODEL_PART volume pins a
    // support interface filament of its own, different from the object's. Both the Chameleon pass
    // and WipingExtrusions write / repaint the same SupportLayer::interface_by_extruder map this
    // feature owns, so both stand down for such an object.
    bool                        has_support_group_interface_filament() const;
    // Ultra (support groups): the 0-based extruders a support group pins for its own interface -
    // sorted, unique, and empty for every object without such a group. Those extruders belong to
    // one part's interface and to nothing else, so nobody else's "don't care" support may resolve
    // to them; GCode.cpp's support-extruder resolution excludes them for that reason.
    std::vector<unsigned int>   support_group_interface_extruders() const;
    // Ultra (support groups, plan Stage 4b): true when some support group asks for a different
    // number of interface layers than the object does. Classic tree supports cannot honour that -
    // the roof layer count comes out of influence-area propagation in draw_circles(), object-wide -
    // so the user is told rather than left wondering. Organic trees and normal supports do honour
    // it, and never raise the notice.
    bool                        has_support_group_interface_layer_override() const;
    // Ultra (support groups, Stage 5): the name of the group that makes the WHOLE object soluble
    // through the rule of plan 3.6, or "" when no group does (and "" when the user asked for a
    // zero gap themselves - there is nothing to tell them then).
    std::string                 support_group_soluble_name() const;
    // Ultra (support groups, Stage 5 / R3.4): the 0-based interface extruders a group pins that
    // sit on a nozzle of a different diameter than the object's support interface. Their
    // interface is extruded at a different width, which need not tile with the object's.
    std::vector<unsigned int>   support_group_interface_extruders_other_nozzle() const;
    // Ultra (support groups, Stage 5): the 1-based interface filament slots some group asks for
    // that this printer does not have. Sorted and unique; empty on every well-formed project.
    std::vector<int>            support_group_unresolvable_interface_filaments() const;

    // Modifier volumes that cannot affect this slice, by name, in ModelObject::volumes order.
    // Both are read off m_shared_regions after the regions are built, so they say what the region
    // builder ACTUALLY did rather than re-deriving it, and both are empty for a healthy object.
    //
    // Modifiers that got a region, but one identical to their parent's - they override no setting,
    // so PrintApply stored them as an alias of the parent's own PrintRegion and they cannot print
    // any differently. An extruder override is NOT counted as "no overrides": a differing extruder
    // folds into wall_filament/sparse_infill_filament/solid_infill_filament, which makes the
    // region config differ and gives the modifier a region of its own.
    std::vector<std::string>    modifiers_without_overrides() const;
    // Modifiers that got no region in ANY layer range - no part's extruded bounding box intersects
    // them - so their geometry never reaches the slice at all.
    std::vector<std::string>    modifiers_without_parent() const;

    // Ultra (support groups, plan Stage 3 3.1): slice an explicit set of volumes at this object's
    // layer Zs and union them per layer. This is the body slice_support_volumes() always had; that
    // function is now a two-liner over it, so enforcer / blocker behaviour is unchanged by
    // construction and support_group_masks() reuses exactly the same machinery.
    std::vector<Polygons>       slice_volumes_at_layers(const std::vector<const ModelVolume*> &volumes) const;
    // Helpers to slice support enforcer / blocker meshes by the support generator.
    std::vector<Polygons>       slice_support_volumes(const ModelVolumeType model_volume_type) const;
    std::vector<Polygons>       slice_support_blockers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_BLOCKER); }
    // Ultra (over-support surfaces): support enforcers / blockers projected onto this object's
    // layers, built exactly the way SupportAnnotations builds them for the support generator
    // (volume slices plus the painted facets, blockers expanded by the same epsilon). Used by
    // detect_surfaces_type, which runs long before the support generator does.
    // docs/superpowers/specs/2026-09-05-over-support-surfaces.md
    void                        slice_support_annotations(std::vector<Polygons> &enforcers, std::vector<Polygons> &blockers) const;
    std::vector<Polygons>       slice_support_enforcers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_ENFORCER); }

    // Ultra (over-support surfaces / walls): the one slice-time reconstruction of "where will the
    // support generator put material under this object", shared by the bottom-face classifier
    // (detect_surfaces_type) and the wall classifier (PerimeterGenerator). Building it twice with
    // two copies of the predicate is how the two would drift apart, so there is only one.
    // docs/superpowers/specs/2026-09-05-over-support-surfaces.md
    struct OverSupportSettings
    {
        // any_region_over_support() && has_support() && support_top_z_distance > 0. Stage 5 made
        // over_support_surfaces a PrintRegionConfig key, so the switch itself is read per PART by
        // the two consumers; what this struct carries is only the object-wide half.
        bool                  on         = false;
        // An auto support type that will actually carry the overhangs it detects.
        bool                  is_auto    = false;
        // Anything to do at all: an auto type, or at least one layer with an enforcer.
        bool                  active     = false;
        // Scaled max_bridge_length where the generators refuse to support a bridgeable overhang
        // (remove_bridges_from_contacts); 0 means "nothing is bridgeable".
        coord_t               bridgeable = 0;
        std::vector<Polygons> enforcers;
        std::vector<Polygons> blockers;
    };
    OverSupportSettings         over_support_settings() const;
    // Stage 5: over_support_surfaces lives on PrintRegionConfig, so the object-wide question
    // "does any part of this object ask for the feature" needs the regions, not m_config.
    bool                        any_region_over_support() const;
    // Region of the plane that has support material under it, for the layer with this Layer::id()
    // (which starts at raft_layers(), not at 0). Returns nullptr when the feature stands down -
    // which is also every build with the switch off, so the perimeter generator's original code
    // path is taken verbatim.
    const Polygons*             over_support_below(size_t layer_id) const;
    // Scaled max_bridge_length for the perimeter-bridge refusal, 0 = nothing is bridgeable.
    coord_t                     over_support_bridgeable() const { return m_over_support_bridgeable; }

    // Helpers to project custom facets on slices
    void project_and_append_custom_facets(bool seam, EnforcerBlockerType type, std::vector<Polygons>& expolys, std::vector<std::pair<Vec3f,Vec3f>>* vertical_points=nullptr) const;

    //BBS
    BoundingBox get_first_layer_bbox(float& area, float& layer_height, std::string& name);
    void         get_certain_layers(float start, float end, std::vector<LayerPtrs> &out, std::vector<BoundingBox> &boundingbox_objects);
    Points       get_instances_shift_without_plate_offset();
    PrintObject* get_shared_object() const { return m_shared_object; }
    void         set_shared_object(PrintObject *object);
    void         clear_shared_object();
    void         copy_layers_from_shared_object();
    void         copy_layers_overhang_from_shared_object();

    // BBS: Boundingbox of the first layer
    BoundingBox                 firstLayerObjectBrimBoundingBox;

    // BBS: returns 1-based indices of extruders used to print the first layer wall of objects
    std::vector<int>            object_first_layer_wall_extruders;

    // SoftFever
    size_t get_id() const { return m_id; }
    void set_id(size_t id) { m_id = id; }

  private:
    // to be called from Print only.
    friend class Print;

	PrintObject(Print* print, ModelObject* model_object, const Transform3d& trafo, PrintInstances&& instances);
	~PrintObject();

    void                    config_apply(const ConfigBase &other, bool ignore_nonexistent = false) { m_config.apply(other, ignore_nonexistent); }
    void                    config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) { m_config.apply_only(other, keys, ignore_nonexistent); }
    PrintBase::ApplyStatus  set_instances(PrintInstances &&instances);
    // Invalidates the step, and its depending steps in PrintObject and Print.
    bool                    invalidate_step(PrintObjectStep step);
    // Invalidates all PrintObject and Print steps.
    bool                    invalidate_all_steps();
    // Invalidate steps based on a set of parameters changed.
    // It may be called for both the PrintObjectConfig and PrintRegionConfig.
    bool                    invalidate_state_by_config_options(
        const ConfigOptionResolver &old_config, const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys);
    // If ! m_slicing_params.valid, recalculate.
    void                    update_slicing_parameters();

    static PrintObjectConfig object_config_from_model_object(const PrintObjectConfig &default_object_config, const ModelObject &object, size_t num_extruders);

private:
    void make_perimeters();
    void prepare_infill();
    void infill();
    void ironing();
    // ZAA (Z contouring), posContouring: runs after ironing, before path simplification.
    void contour_z();
    bool need_z_contouring() const;
    void generate_support_material();
    void estimate_curled_extrusions();
    void simplify_extrusion_path();

    void slice_volumes();
    //BBS
    ExPolygons _shrink_contour_holes(double contour_delta, double hole_delta, const ExPolygons& polys) const;
    // BBS
    void detect_overhangs_for_lift();
    void clear_overhangs_for_lift();

   void _transform_hole_to_polyholes();

    // Ultra (over-support walls): fill / drop m_over_support_below around the perimeter pass.
    void build_over_support_below();
    void clear_over_support_below();

    // Has any support (not counting the raft).
    void detect_surfaces_type();
    void process_external_surfaces();
    void discover_vertical_shells();
    void bridge_over_infill();
    void clip_fill_surfaces();
    void discover_horizontal_shells();
    void combine_infill();
    void _generate_support_material();
    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> prepare_adaptive_infill_data(
        const std::vector<std::pair<const Surface*, float>>& surfaces_w_bottom_z) const;
    FillLightning::GeneratorPtr prepare_lightning_infill_data();

    // BBS
    SupportNecessaryType is_support_necessary();

    // XYZ in scaled coordinates
    Vec3crd									m_size;
    double                                  m_max_z;
    PrintObjectConfig                       m_config;
    // Translation in Z + Rotation + Scaling / Mirroring.
    Transform3d                             m_trafo = Transform3d::Identity();
    // Slic3r::Point objects in scaled G-code coordinates
    std::vector<PrintInstance>              m_instances;
    // The mesh is being centered before thrown to Clipper, so that the Clipper's fixed coordinates require less bits.
    // This is the adjustment of the  the Object's coordinate system towards PrintObject's coordinate system.
    Point                                   m_center_offset;

    // Object split into layer ranges and regions with their associated configurations.
    // Shared among PrintObjects created for the same ModelObject.
    PrintObjectRegions                     *m_shared_regions { nullptr };

    SlicingParameters                       m_slicing_params;
    LayerPtrs                               m_layers;
    SupportLayerPtrs                        m_support_layers;
    std::vector<LocalZInterval>             m_local_z_intervals;
    std::vector<SubLayerPlan>               m_local_z_sublayer_plan;
    // BBS
    std::shared_ptr<TreeSupportData>        m_tree_support_preview_cache;

    // this is set to true when LayerRegion->slices is split in top/internal/bottom
    // so that next call to make_perimeters() performs a union() before computing loops
    bool                    				m_typed_slices = false;

    // Ultra (over-support walls): per-layer "there is support under here", alive only for the
    // duration of the perimeter pass (built at the top of make_perimeters, dropped at its end).
    // Empty vector = the feature stands down and no wall is reclassified.
    std::vector<Polygons>                   m_over_support_below;
    coord_t                                 m_over_support_bridgeable = 0;

    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> m_adaptive_fill_octrees;
    FillLightning::GeneratorPtr m_lightning_generator;

    std::vector < VolumeSlices >            firstLayerObjSliceByVolume;
    std::vector<groupedVolumeSlices>        firstLayerObjSliceByGroups;

    // BBS: per object skirt
    ExtrusionEntityCollection               m_skirt;

    PrintObject*                            m_shared_object{ nullptr };

    
    // SoftFever
    //
    // object id, assigned in print order by GCode::assign_object_and_instance_ids(). Default to 0:
    // it used to be left uninitialised, and the `; printing object <name> id:<N>` labels then
    // printed whatever the heap happened to hold, which differed between two runs of one binary.
    size_t               m_id{0};
    void apply_conical_overhang();

 public:
    //BBS: When printing multi-material objects, this settings will make slicer to clip the overlapping object parts one by the other.
    //(2nd part will be clipped by the 1st, 3rd part will be clipped by the 1st and 2nd etc).
    // This was a per-object setting and now we default enable it.
    static bool clip_multipart_objects;
    static bool infill_only_where_needed;
};

struct FakeWipeTower
{
    // generate fake extrusion
    Vec2f pos;
    float width;
    float height;
    float layer_height;
    float depth;
    std::vector<std::pair<float, float>> z_and_depth_pairs;
    float brim_width;
    float rotation_angle;
    float cone_angle;
    Vec2d plate_origin;
    std::map<float, Polylines> outer_wall;

    void set_fake_extrusion_data(Vec2f p, float w, float h, float lh, float d, float bd, Vec2d o)
    {
        pos          = p;
        width        = w;
        height       = h;
        layer_height = lh;
        depth        = d;
        brim_width   = bd;
        plate_origin = o;
    }
    void set_fake_extrusion_data(const Vec2f& p, float w, float h, float lh, float d, const std::vector<std::pair<float, float>>& zad, float bd, float ra, float ca, const Vec2d& o)
    {
        pos = p;
        width = w;
        height = h;
        layer_height = lh;
        depth = d;
        z_and_depth_pairs = zad;
        brim_width = bd;
        rotation_angle = ra;
        cone_angle = ca;
        plate_origin = o;
    }
    void set_pos(Vec2f p) { pos = p; }
    void set_pos_and_rotation(const Vec2f& p, float rotation) { pos = p; rotation_angle = rotation; }

    std::vector<ExtrusionPaths> getFakeExtrusionPathsFromWipeTower() const
    {
        int   d         = scale_(depth);
        int   w         = scale_(width);
        int   bd        = scale_(brim_width);
        Point minCorner = {scale_(pos.x()), scale_(pos.y())};
        Point maxCorner = {minCorner.x() + w, minCorner.y() + d};

        std::vector<ExtrusionPaths> paths;
        for (float h = 0.f; h < height; h += layer_height) {
            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, layer_height);
            path.polyline = {minCorner, {maxCorner.x(), minCorner.y()}, maxCorner, {minCorner.x(), maxCorner.y()}, minCorner};
            paths.push_back({path});

            if (h == 0.f) { // add brim
                ExtrusionPath fakeBrim(ExtrusionRole::erBrim, 0.0, 0.0, layer_height);
                Point         wtbminCorner = {minCorner - Point{bd, bd}};
                Point         wtbmaxCorner = {maxCorner + Point{bd, bd}};
                fakeBrim.polyline          = {wtbminCorner, {wtbmaxCorner.x(), wtbminCorner.y()}, wtbmaxCorner, {wtbminCorner.x(), wtbmaxCorner.y()}, wtbminCorner};
                paths.back().push_back(fakeBrim);
            }
        }
        return paths;
    }

    std::vector<ExtrusionPaths> getFakeExtrusionPathsFromWipeTower2() const
    {
        float h = height;
        float lh = layer_height;
        int   d = scale_(depth);
        int   w = scale_(width);
        int   bd = scale_(brim_width);
        Point minCorner = { -bd, -bd };
        Point maxCorner = { minCorner.x() + w + bd, minCorner.y() + d + bd };

        const auto [cone_base_R, cone_scale_x] = WipeTower2::get_wipe_tower_cone_base(width, height, depth, cone_angle);

        std::vector<ExtrusionPaths> paths;
        for (float hh = 0.f; hh < h; hh += lh) {
            
            if (hh != 0.f) {
                // The wipe tower may be getting smaller. Find the depth for this layer.
                size_t i = 0;
                for (i=0; i<z_and_depth_pairs.size()-1; ++i)
                    if (hh >= z_and_depth_pairs[i].first && hh < z_and_depth_pairs[i+1].first)
                        break;
                d = scale_(z_and_depth_pairs[i].second);
                minCorner = {0.f, -d/2 + scale_(z_and_depth_pairs.front().second/2.f)};
                maxCorner = { minCorner.x() + w, minCorner.y() + d };
            }


            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, lh);
            path.polyline = { minCorner, {maxCorner.x(), minCorner.y()}, maxCorner, {minCorner.x(), maxCorner.y()}, minCorner };
            paths.push_back({ path });

            // We added the border, now add several parallel lines so we can detect an object that is fully inside the tower.
            // For now, simply use fixed spacing of 3mm.
            for (coord_t y=minCorner.y()+scale_(3.); y<maxCorner.y(); y+=scale_(3.)) {
                path.polyline = { {minCorner.x(), y}, {maxCorner.x(), y} };
                paths.back().emplace_back(path);
            }

            // And of course the stabilization cone and its base...
            if (cone_base_R > 0.) {
                path.polyline.clear();
                double r = cone_base_R * (1 - hh/height);
                for (double alpha=0; alpha<2.01*M_PI; alpha+=2*M_PI/20.)
                    path.polyline.points.emplace_back(Point::new_scale(width/2. + r * std::cos(alpha)/cone_scale_x, depth/2. + r * std::sin(alpha)));
                paths.back().emplace_back(path);
                if (hh == 0.f) { // Cone brim.
                    for (float bw=brim_width; bw>0.f; bw-=3.f) {
                        path.polyline.clear();
                        for (double alpha=0; alpha<2.01*M_PI; alpha+=2*M_PI/20.) // see load_wipe_tower_preview, where the same is a bit clearer
                            path.polyline.points.emplace_back(Point::new_scale(
                                width/2. + cone_base_R * std::cos(alpha)/cone_scale_x * (1. + cone_scale_x*bw/cone_base_R),
                                depth/2. + cone_base_R * std::sin(alpha) * (1. + bw/cone_base_R))
                            );
                        paths.back().emplace_back(path);
                    }
                }
            }

            // Only the first layer has brim.
            if (hh == 0.f) {
                minCorner = minCorner + Point(bd, bd);
                maxCorner = maxCorner - Point(bd, bd);
            }
        }

        // Rotate and translate the tower into the final position.
        for (ExtrusionPaths& ps : paths) {
            for (ExtrusionPath& p : ps) {
                p.polyline.rotate(Geometry::deg2rad(rotation_angle));
                p.polyline.translate(scale_(pos.x()), scale_(pos.y()));
            }
        }

        return paths;
    }

    ExtrusionLayers getTrueExtrusionLayersFromWipeTower() const;
};

struct WipeTowerData
{
    // Following section will be consumed by the GCodeGenerator.
    // Tool ordering of a non-sequential print has to be known to calculate the wipe tower.
    // Cache it here, so it does not need to be recalculated during the G-code generation.
    ToolOrdering                                         &tool_ordering;
    // Cache of tool changes per print layer.
    std::unique_ptr<std::vector<WipeTower::ToolChangeResult>> priming;
    std::vector<std::vector<WipeTower::ToolChangeResult>> tool_changes;
    std::vector<std::vector<WipeTower::ToolChangeResult>> local_z_tool_changes;
    std::unique_ptr<WipeTower::ToolChangeResult>          final_purge;
    std::vector<float>                                    used_filament;
    int                                                   number_of_toolchanges;

    // Depth of the wipe tower to pass to GLCanvas3D for exact bounding box:
    float                                                 depth;
    // Effective width (a rib wall squares the tower): the estimate until generation, then the
    // generated width, so it never disagrees with depth.
    float                                                 width;
    std::vector<std::pair<float, float>>                  z_and_depth_pairs;
    std::vector<std::vector<WipeTower::box_coordinates>>  local_z_reserve_boxes;
    float                                                 brim_width;
    float                                                 height;
    // First-layer outline of the generated tower (brim included), used as the generation-time
    // backstop so a brim that grew over exclusion or off the bed cannot write G-code.
    struct WipeTowerMeshData
    {
        Polygon bottom;
    };
    std::optional<WipeTowerMeshData>                      wipe_tower_mesh_data;

    void construct_mesh(float width, float depth, float height, float brim_width, bool is_rib_wipe_tower, float rib_width, float rib_length, bool fillet_wall, float cone_angle = 0.f);

    void clear() {
        priming.reset(nullptr);
        tool_changes.clear();
        local_z_tool_changes.clear();
        final_purge.reset(nullptr);
        used_filament.clear();
        number_of_toolchanges = -1;
        depth = 0.f;
        width = 0.f;
        local_z_reserve_boxes.clear();
        brim_width = 0.f;
        wipe_tower_mesh_data.reset();
    }

private:
	// Only allow the WipeTowerData to be instantiated internally by Print, 
	// as this WipeTowerData shares reference to Print::m_tool_ordering.
	friend class Print;
	WipeTowerData(ToolOrdering &tool_ordering) : tool_ordering(tool_ordering) { clear(); }
	WipeTowerData(const WipeTowerData & /* rhs */) = delete;
	WipeTowerData &operator=(const WipeTowerData & /* rhs */) = delete;
};

struct PrintStatistics
{
    PrintStatistics() { clear(); }
    std::string                     estimated_normal_print_time;
    std::string                     estimated_silent_print_time;
    double                          total_used_filament;
    double                          total_extruded_volume;
    double                          total_cost;
    int                             total_toolchanges;
    double                          total_weight;
    double                          total_wipe_tower_cost;
    double                          total_wipe_tower_filament;
    unsigned int                    initial_tool;
    std::map<size_t, double>        filament_stats;

    // Config with the filled in print statistics.
    DynamicConfig           config() const;
    // Config with the statistics keys populated with placeholder strings.
    static DynamicConfig    placeholders();
    // Replace the print statistics placeholders in the path.
    std::string             finalize_output_path(const std::string &path_in) const;

    void clear() {
        total_used_filament    = 0.;
        total_extruded_volume  = 0.;
        total_cost             = 0.;
        total_toolchanges      = 0;
        total_weight           = 0.;
        total_wipe_tower_cost  = 0.;
        total_wipe_tower_filament = 0.;
        initial_tool           = 0;
        filament_stats.clear();
    }
    static const std::string FilamentUsedG;
    static const std::string FilamentUsedGMask;
    static const std::string TotalFilamentUsedG;
    static const std::string TotalFilamentUsedGMask;
    static const std::string TotalFilamentUsedGValueMask;
    static const std::string FilamentUsedCm3;
    static const std::string FilamentUsedCm3Mask;
    static const std::string FilamentUsedMm;
    static const std::string FilamentUsedMmMask;
    static const std::string FilamentCost;
    static const std::string FilamentCostMask;
    static const std::string TotalFilamentCost;
    static const std::string TotalFilamentCostMask;
    static const std::string TotalFilamentCostValueMask;
    static const std::string TotalFilamentUsedWipeTower;
    static const std::string TotalFilamentUsedWipeTowerValueMask;
    
};

typedef std::vector<PrintObject*>       PrintObjectPtrs;
typedef std::vector<const PrintObject*> ConstPrintObjectPtrs;
class ConstPrintObjectPtrsAdaptor : public ConstVectorOfPtrsAdaptor<PrintObject> {
    friend Print;
    ConstPrintObjectPtrsAdaptor(const PrintObjectPtrs *data) : ConstVectorOfPtrsAdaptor<PrintObject>(data) {}
};

typedef std::vector<PrintRegion*>       PrintRegionPtrs;
/*
typedef std::vector<const PrintRegion*> ConstPrintRegionPtrs;
class ConstPrintRegionPtrsAdaptor : public ConstVectorOfPtrsAdaptor<PrintRegion> {
    friend Print;
    ConstPrintRegionPtrsAdaptor(const PrintRegionPtrs *data) : ConstVectorOfPtrsAdaptor<PrintRegion>(data) {}
};
*/

enum FilamentTempType {
    HighTemp=0,
    LowTemp,
    HighLowCompatible,
    Undefine
};
// The complete print tray with possibly multiple objects.
class Print : public PrintBaseWithState<PrintStep, psCount>
{
private: // Prevents erroneous use by other classes.
    typedef PrintBaseWithState<PrintStep, psCount> Inherited;
    // Bool indicates if supports of PrintObject are top-level contour.
    typedef std::pair<PrintObject *, bool>         PrintObjectInfo;

public:
    Print() = default;
	virtual ~Print() { this->clear(); }

	PrinterTechnology	technology() const noexcept override { return ptFFF; }

    // Methods, which change the state of Print / PrintObject / PrintRegion.
    // The following methods are synchronized with process() and export_gcode(),
    // so that process() and export_gcode() may be called from a background thread.
    // In case the following methods need to modify data processed by process() or export_gcode(),
    // a cancellation callback is executed to stop the background processing before the operation.
    void                clear() override;
    bool                empty() const override { return m_objects.empty(); }
    // List of existing PrintObject IDs, to remove notifications for non-existent IDs.
    std::vector<ObjectID> print_object_ids() const override;

    ApplyStatus         apply(const Model &model, DynamicPrintConfig config) override;

    void                process(long long *time_cost_with_cache = nullptr, bool use_cache = false) override;
    // Slices `object` and generates its walls (posSlice, posPerimeters) and nothing else, for tools that need an
    // object's perimeters without a full slice of the plate (Auto-paint seam, SeamPlacer::plan_object_seams()).
    // The other objects of the print are left unsliced. Throws CanceledException when canceled.
    void                process_perimeters_only(PrintObject &object);
    // Exports G-code into a file name based on the path_template, returns the file path of the generated G-code file.
    // If preview_data is not null, the preview_data is filled in for the G-code visualization (not used by the command line Slic3r).
    std::string         export_gcode(const std::string& path_template, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    //return 0 means successful
    int                 export_cached_data(const std::string& dir_path, bool with_space=false);
    int                 load_cached_data(const std::string& directory);

    // Ultra (dual-nozzle): the filament->nozzle grouping result, computed externally (GUI/CLI) before
    // process() for AMS-multi-nozzle machines (H2D/H2C/X2D). Null for classic single-/multi-extruder
    // machines (e.g. Snapmaker U1 toolchanger), which keep the existing per-extruder pipeline.
    // Ultra (H2C 3MF schema): does any object route its support (or support interface) to a
    // filament that the object body does not use, so the prime tower carries support material?
    // Ported from BambuStudio Print::support_material_on_wipe_tower.
    bool support_material_on_wipe_tower() const;
    void set_nozzle_group_result(const std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> result) { m_nozzle_group_result = result; }
    const std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> get_nozzle_group_result() const { return m_nozzle_group_result; }
    std::shared_ptr<MultiNozzleUtils::LayeredNozzleGroupResult> get_layered_nozzle_group_result() const {
        return std::dynamic_pointer_cast<MultiNozzleUtils::LayeredNozzleGroupResult>(m_nozzle_group_result);
    }

    // Ultra (dual-nozzle): inputs to the filament->nozzle grouping compute. In BBS these are populated by
    // GUI slice-prep; this fork does not populate them yet (stubbed-input grouping), so the getters return
    // empty and the grouper treats every filament as reachable on every nozzle. Setters exist so a later
    // input-population pass can fill them without touching the grouping code.
    const std::vector<std::vector<DynamicPrintConfig>>& get_extruder_filament_info() const { return m_extruder_filament_info; }
    void set_extruder_filament_info(const std::vector<std::vector<DynamicPrintConfig>>& v) { m_extruder_filament_info = v; }
    std::unordered_map<int, std::unordered_map<int, double>> get_filament_print_time() const { return m_filament_print_time; }
    void set_filament_print_time(const std::unordered_map<int, std::unordered_map<int, double>>& v) { m_filament_print_time = v; }
    const std::vector<std::set<int>>& get_geometric_unprintable_filaments() const { return m_geometric_unprintable_filaments; }
    void set_geometric_unprintable_filaments(const std::vector<std::set<int>>& v) { m_geometric_unprintable_filaments = v; }
    // Computed on demand: usage type is config-derived (real); physical/flow unprintables are stubbed empty
    // (the config subsystem they need is not ported).
    std::vector<FilamentUsageType> get_filament_usage_type() const;
    std::vector<std::set<int>> get_physical_unprintable_filaments(const std::vector<unsigned int>& used_filaments) const;
    std::map<int, std::set<NozzleVolumeType>> get_filament_unprintable_flow(const std::vector<unsigned int>& used_filaments) const;

    // Ultra (Phase 10): AMS-aware grouping inputs carried OUTSIDE the print config. The per-nozzle AMS slot
    // budget (extruder_ams_count "cap#numBanks" per nozzle) and the "force match mode" flag are set from the
    // connected printer's AMS before slicing. Kept as Print members (not config keys) because
    // extruder_ams_count / filament_map_mode are config the fragile Bambu web device view reads, and writing
    // grouping values into them nulls its connected machine (breaks the print menu). The grouping reads these
    // members instead. Empty/false -> grouping uses its config default (flush).
    const std::vector<std::string>& get_ultra_ams_count() const { return m_ultra_ams_count; }
    void set_ultra_ams_count(const std::vector<std::string>& v) { m_ultra_ams_count = v; }
    bool get_ultra_force_match_mode() const { return m_ultra_force_match_mode; }
    void set_ultra_force_match_mode(bool v) { m_ultra_force_match_mode = v; }

    // The slice's grouping (ToolOrdering, dual-nozzle Bambu) rewrites the Print's filament_map with the
    // map it computed. On a rack printer (H2C) that map can differ from the plate's manual map for
    // filaments the plate does not use, and comparing the next apply()'s unchanged manual map with it
    // invalidated the finished slice (Print plate on the H2C, 2026-09-23). True when the incoming map is
    // the same input the current map was computed from, i.e. nothing changed and the sliced map stays.
    static bool keep_sliced_filament_map(const std::vector<int> &incoming, const std::vector<int> &sliced_input,
                                         const std::vector<int> &current)
    {
        return !sliced_input.empty() && incoming == sliced_input && incoming != current;
    }
    // The filament_map the settings asked for (the manual map of a hand-grouped plate), as opposed to
    // config().filament_map, which holds the grouping's result once a slice has run. Falls back to the
    // config value for a Print that was never applied a map.
    const std::vector<int>& filament_map_input() const
    {
        return m_filament_map_input.size() == m_config.filament_map.values.size() ? m_filament_map_input : m_config.filament_map.values;
    }
    // Print config keys that differed in the last apply(), empty when nothing did.
    const std::vector<std::string>& last_apply_changed_keys() const { return m_last_apply_changed_keys; }

    // methods for handling state
    bool                is_step_done(PrintStep step) const { return Inherited::is_step_done(step); }
    // Returns true if an object step is done on all objects and there's at least one object.
    bool                is_step_done(PrintObjectStep step) const;
    // Returns true if the last step was finished with success.
    bool                finished() const override { return this->is_step_done(psGCodeExport); }

    bool                has_infinite_skirt() const;
    bool                has_skirt() const;
    bool                has_brim() const;
    //BBS
    bool                has_auto_brim() const    {
        return std::any_of(m_objects.begin(), m_objects.end(), [](PrintObject* object) { return object->config().brim_type == btAutoBrim; });
    }

    // Returns an empty string if valid, otherwise returns an error message.
    StringObjectException validate(StringObjectException *warning = nullptr, Polygons* collison_polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr) const override;
    double              skirt_first_layer_height() const;
    Flow                brim_flow() const;
    Flow                skirt_flow() const;

    std::vector<unsigned int> object_extruders() const;
    std::vector<unsigned int> support_material_extruders() const;
    std::vector<unsigned int> extruders(bool conside_custom_gcode = false) const;
    // On-demand evaluation vs filament_hot_bed_nozzles.json (calls extruders(true) once internally).
    void                filament_rule_mismatch_flags(NozzleFilamentRuleMismatch& out_nozzle_mismatch,
                                                     bool& out_gesp,
                                                     bool& out_pei_not_pla,
                                                     bool& out_pei_tpu,
                                                     const PresetBundle* preset_bundle = nullptr) const;
    
    double              max_allowed_layer_height() const;
    bool                has_support_material() const;
    // Make sure the background processing has no access to this model_object during this call!
    void                auto_assign_extruders(ModelObject* model_object) const;

    const PrintConfig&          config() const { return m_config; }
    const PrintObjectConfig&    default_object_config() const { return m_default_object_config; }
    const PrintRegionConfig& default_region_config() const { return m_default_region_config; }
    const MixedFilamentManager& mixed_filament_manager() const { return m_mixed_filament_mgr; }
    MixedFilamentManager&       mixed_filament_manager()       { return m_mixed_filament_mgr; }
    ConstPrintObjectPtrsAdaptor objects() const { return ConstPrintObjectPtrsAdaptor(&m_objects); }
    PrintObject*                get_object(size_t idx) { return const_cast<PrintObject*>(m_objects[idx]); }
    const PrintObject*          get_object(size_t idx) const { return m_objects[idx]; }
    // PrintObject by its ObjectID, to be used to uniquely bind slicing warnings to their source PrintObjects
    // in the notification center.
    const PrintObject*          get_object(ObjectID object_id) const {
        auto it = std::find_if(m_objects.begin(), m_objects.end(),
            [object_id](const PrintObject *obj) { return obj->id() == object_id; });
        return (it == m_objects.end()) ? nullptr : *it;
    }
    //BBS: Function to get m_brimMap;
    std::map<ObjectID, ExtrusionEntityCollection>&
        get_brimMap() { return m_brimMap; }
    // Chameleon brim: brim runs whose nearest wall belongs to a different
    // extruder than the object's own, keyed by object then by extruder.
    // const: ToolOrdering (a non-friend) reads this after brim generation.
    const std::map<ObjectID, std::map<unsigned int, ExtrusionEntityCollection>>&
        get_brimMapByExtruder() const { return m_brimMapByExtruder; }

    // How many of PrintObject::copies() over all print objects are there?
    // If zero, then the print is empty and the print shall not be executed.
    unsigned int                num_object_instances() const;

    // For Perl bindings.
    PrintObjectPtrs&            objects_mutable() { return m_objects; }
    PrintRegionPtrs&            print_regions_mutable() { return m_print_regions; }
    std::vector<size_t>         layers_sorted_for_object(float start, float end, std::vector<LayerPtrs> &layers_of_objects, std::vector<BoundingBox> &boundingBox_for_objects, VecOfPoints& objects_instances_shift);
    const ExtrusionEntityCollection& skirt() const { return m_skirt; }
    // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
    // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
    // It does NOT encompass user extrusions generated by custom G-code,
    // therefore it does NOT encompass the initial purge line.
    // It does NOT encompass MMU/MMU2 starting (wipe) areas.
    const Polygon&                   first_layer_convex_hull() const { return m_first_layer_convex_hull; }

    const PrintStatistics&      print_statistics() const { return m_print_statistics; }
    PrintStatistics&            print_statistics() { return m_print_statistics; }

    // Wipe tower support.
    bool                        has_wipe_tower() const;
    const WipeTowerData&        wipe_tower_data(size_t filaments_cnt = 0) const;
    const ToolOrdering& 		tool_ordering() const { return m_tool_ordering; }

    bool                        enable_timelapse_print() const;

	std::string                 output_filename(const std::string &filename_base = std::string()) const override;

	std::string                 get_model_name() const;
	std::string                 get_plate_number_formatted() const;

    size_t                      num_print_regions() const throw() { return m_print_regions.size(); }
    const PrintRegion&          get_print_region(size_t idx) const  { return *m_print_regions[idx]; }
    const ToolOrdering&         get_tool_ordering() const { return m_wipe_tower_data.tool_ordering; }
    const FakeWipeTower& get_fake_wipe_tower() const { return m_fake_wipe_tower; }

    //BBS: plate's origin related functions
    void set_plate_origin(Vec3d origin) { m_origin = origin; }
    const Vec3d get_plate_origin() const { return m_origin; }
    //BBS: export gcode from previous gcode file from 3mf
    void set_gcode_file_ready();
    void set_gcode_file_invalidated();
    void export_gcode_from_previous_file(const std::string& file, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    //BBS: add modify_count logic
    int get_modified_count() const {return m_modified_count;}
    //BBS: add status for whether support used
    bool is_support_used() const {return m_support_used;}
    std::string get_conflict_string() const
    {
        std::string result;
        if (m_conflict_result) {
            result = "Found gcode path conflicts between object " + m_conflict_result.value()._objName1 + " and " + m_conflict_result.value()._objName2;
        }

        return result;
    }

    //BBS
    static StringObjectException sequential_print_clearance_valid(const Print &print, Polygons *polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr);
    // Orca: pre-slice clearance check for a prime tower compacted by "No sparse layers".
    static StringObjectException compacted_wipe_tower_clearance_valid(const Print &print, Polygons *polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr);
    ConflictResultOpt            get_conflict_result() const { return m_conflict_result; }

    // Return 4 wipe tower corners in the world coordinates (shifted and rotated), including the wipe tower brim.
    Points first_layer_wipe_tower_corners(bool check_wipe_tower_existance=true) const;

    //SoftFever
    bool &is_BBL_printer() { return m_isBBLPrinter; }
    const bool is_BBL_printer() const { return m_isBBLPrinter; }
    CalibMode& calib_mode() { return m_calib_params.mode; }
    const CalibMode calib_mode() const { return m_calib_params.mode; }
    void set_calib_params(const Calib_Params& params);
    const Calib_Params& calib_params() const { return m_calib_params; }
    Vec2d translate_to_print_space(const Vec2d &point) const;
    // scaled point
    Vec2d translate_to_print_space(const Point &point) const;
    // Orca: precise counterpart of compacted_wipe_tower_clearance_valid(), run once the tower exists.
    void                validate_compacted_wipe_tower_clearance() const;
    static FilamentTempType get_filament_temp_type(const std::string& filament_type);
    static int get_hrc_by_nozzle_type(const NozzleType& type);
    static bool check_multi_filaments_compatibility(const std::vector<std::string>& filament_types);
    // similar to check_multi_filaments_compatibility, but the input is int, and may be negative (means unset)
    static bool is_filaments_compatible(const std::vector<int>& types);
    // get the compatible filament type of a multi-material object
    // Rule:
    // 1. LowTemp+HighLowCompatible=LowTemp
    // 2. HighTemp++HighLowCompatible=HighTemp
    // 3. LowTemp+HighTemp+...=HighLowCompatible
    // Unset types are just ignored.
    static int get_compatible_filament_type(const std::set<int>& types);

    bool is_all_objects_are_short() const {
        return std::all_of(this->objects().begin(), this->objects().end(), [&](PrintObject* obj) { return obj->height() < scale_(this->config().nozzle_height.value); });
    }
    
    // Orca: Implement prusa's filament shrink compensation approach
    // Returns if all used filaments have same shrinkage compensations.
     bool has_same_shrinkage_compensations() const;
    // Returns scaling for each axis representing shrinkage compensations in each axis.
     Vec3d shrinkage_compensation() const;

    std::tuple<float, float> object_skirt_offset(double margin_height = 0) const;

protected:
    // Invalidates the step, and its depending steps in Print.
    bool                invalidate_step(PrintStep step);

private:
    //BBS
    static StringObjectException check_multi_filament_valid(const Print &print);

    bool                invalidate_state_by_config_options(const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys);

    void                _make_skirt();
    void                _make_wipe_tower();
    void                finalize_first_layer_convex_hull();

    // Islands of objects and their supports extruded at the 1st layer.
    Polygons            first_layer_islands() const;

    PrintConfig                             m_config;
    PrintObjectConfig                       m_default_object_config;
    PrintRegionConfig                       m_default_region_config;
    MixedFilamentManager                    m_mixed_filament_mgr;
    PrintObjectPtrs                         m_objects;
    PrintRegionPtrs                         m_print_regions;
    // Ultra (dual-nozzle): filament->nozzle grouping result set externally before process(); null on
    // classic machines. Reset in clear().
    std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> m_nozzle_group_result;
    // Ultra (dual-nozzle): grouping inputs (see getters). Empty until an input-population pass fills them.
    std::vector<std::vector<DynamicPrintConfig>>            m_extruder_filament_info;
    std::unordered_map<int, std::unordered_map<int, double>> m_filament_print_time;
    std::vector<std::set<int>>                             m_geometric_unprintable_filaments;
    // Ultra (Phase 10): AMS-aware grouping inputs kept out of the print config (see accessors).
    std::vector<std::string>                              m_ultra_ams_count;
    bool                                                  m_ultra_force_match_mode = false;
    // filament_map as the config last handed it to apply(): the grouping's INPUT. A slice on a
    // dual-nozzle Bambu printer overwrites m_config.filament_map with the map it computed (ToolOrdering),
    // so apply() compares an incoming map with this, not with that output (see keep_sliced_filament_map).
    std::vector<int>                                      m_filament_map_input;
    // Print config keys that differed in the last apply() (diagnostics for the GUI).
    std::vector<std::string>                              m_last_apply_changed_keys;

    //SoftFever
    // Set by the GUI (BackgroundSlicingProcess) and the CLI before export. It must still have a defined
    // value for a Print built any other way (tests, tools): as an uninitialized member of a stack Print it
    // was garbage, and a non-zero byte moved the whole config dump into the G-code header.
    bool m_isBBLPrinter = false;

    // Ordered collections of extrusion paths to build skirt loops and brim.
    ExtrusionEntityCollection               m_skirt;
    // BBS: collecting extrusion paths to build brim by objs
    std::map<ObjectID, ExtrusionEntityCollection>         m_brimMap;
    std::map<ObjectID, ExtrusionEntityCollection>         m_supportBrimMap;
    // Chameleon brim: foreign-extruder brim runs partitioned out of m_brimMap,
    // keyed by object then by the extruder that should print them.
    std::map<ObjectID, std::map<unsigned int, ExtrusionEntityCollection>> m_brimMapByExtruder;
    // Convex hull of the 1st layer extrusions.
    // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
    // It does NOT encompass user extrusions generated by custom G-code,
    // therefore it does NOT encompass the initial purge line.
    // It does NOT encompass MMU/MMU2 starting (wipe) areas.
    Polygon                                 m_first_layer_convex_hull;
    Points                                  m_skirt_convex_hull;

    // Following section will be consumed by the GCodeGenerator.
    ToolOrdering 							m_tool_ordering;
    WipeTowerData                           m_wipe_tower_data {m_tool_ordering};

    // Estimated print time, filament consumed.
    PrintStatistics                         m_print_statistics;
    bool                                    m_support_used {false};

    //BBS: plate's origin
    // Eigen does not value-initialise a Vec3d member, and a Print built without
    // set_plate_origin() (the test harness, the CLI) then reads garbage here.
    // wipe_tower_x + m_origin(0) became +-inf, scale_() of which is INT64_MIN, and
    // the translated prime-tower hull tripped ClipperLib's range test in validate().
    Vec3d   m_origin { Vec3d::Zero() };
    //BBS: modified_count
    int     m_modified_count {0};
    //BBS
    ConflictResultOpt m_conflict_result;
    FakeWipeTower     m_fake_wipe_tower;
    
    //SoftFever: calibration
    Calib_Params m_calib_params;

    // To allow GCode to set the Print's GCodeExport step status.
    friend class GCode;
    // Allow PrintObject to access m_mutex and m_cancel_callback.
    friend class PrintObject;

public:
    //BBS: this was a print config and now seems to be useless so we move it to here
    // ORCA: parameter below is now back to being a user option (min_skirt_length)
    //static float min_skirt_length;
};


// ---------------------------------------------------------------------------------------------
// Clearance rule for a prime tower compacted by wipe_tower_no_sparse_layers. Shared by the precise
// check that runs on the real extrusions, the pre-slice estimate that feeds the plater with collision
// polygons, and the plater's own live preview while the user drags the tower or an object around.
// Keeping the rule in one place is what stops those three from drifting apart and reporting different
// things for the same plate.
// ---------------------------------------------------------------------------------------------

// Half of a clearance distance, the share each of the two outlines carries. Sequential printing splits
// extruder_clearance_radius between the two object hulls this way; the tower checks split their
// clearances between the tower ring and the instance hull for the same reason, so that the two
// outlines the plater draws touch precisely when the check trips. The 0.2 mm comes off first: it is
// the rounding slack the sequential check applies, 0.1 mm per side.
inline double compacted_tower_half_clearance(double clearance) { return 0.5 * (clearance - 0.2); }

// Keep-out geometry a compacted tower projects onto the plate, derived from its bare footprint.
struct CompactedTowerZone
{
    // Footprint the checks work on: the raw outline grown by the spiral Z-hop envelope.
    Polygon     hull;
    // hull grown by half the toolhead radius; an object whose own half-grown hull reaches into it is
    // hit by the head body. This is also the ring the plater draws.
    Polygons    grown_body;
    // hull grown by half the bare nozzle cone radius, the innermost tier.
    Polygons    grown_nozzle;
    // hull bounding box, the Y band the rod sweeps.
    BoundingBox bbox_rod;
    // Full body clearance, of which grown_body carries half. Which of the two tiers applies is decided
    // per object rather than here; see compacted_wipe_tower_clearance().
    double      body_radius { 0. };

    bool empty() const { return hull.points.empty(); }
};

// Per-side padding a bare wipe tower outline needs before the clearance checks may treat it as the
// tower's footprint. Callers whose outline already carries the first-layer brim pass zero for it.
// Shared by the pre-slice estimate and the plater's live preview: both start from an outline that
// falls short of the printed tower in the same two ways, and padding them by different amounts is
// exactly how the preview and the validation behind it would end up disagreeing.
double compacted_tower_footprint_padding(const PrintConfig &config, double brim_width);

// Grow a bare tower footprint (bed frame, scaled) into its keep-out zone.
CompactedTowerZone compacted_wipe_tower_zone(const PrintConfig &config, const Polygon &tower_footprint);

// How far an object may rise above the compacted tower base before the toolhead hits it.
struct CompactedTowerClearance
{
    // Height the object may reach above the tower base. Zero means it may not rise at all.
    double allowed_rise;
    // Clearance that applies once the object stands clear of the toolhead in XY, i.e. rod or lid.
    double far_clearance;
    // The object sits within the toolhead radius, so the head body limits it rather than the rod.
    bool   near_body;
    // Horizontal clearance this particular object has to keep from the tower: the full toolhead
    // radius once it rises past the nozzle cone, the bare cone while it stays below. It is what the
    // error message quotes and what the plater grows the object outline by.
    double body_clearance;
};

// object_rise is the height above the tower base that the caller is going to compare against
// allowed_rise. It also selects the horizontal tier, so the two cannot disagree.
CompactedTowerClearance compacted_wipe_tower_clearance(const PrintConfig &config, const CompactedTowerZone &zone,
                                                      const Polygon &inst_hull, double object_rise);

// This object was judged on a tier reaching past the bare nozzle cone, so the wide ring is the one its
// outline has to be drawn against.
inline bool compacted_tower_body_tier(const CompactedTowerClearance &clearance)
{
    return clearance.body_clearance > double(MAX_OUTER_NOZZLE_DIAMETER);
}

// Keep-out rings to draw around the tower. The nozzle one always applies; the wide body one is drawn
// only when some object on the plate is actually measured against it, otherwise it would show a
// keep-out zone no object can violate.
Polygons compacted_wipe_tower_rings(const CompactedTowerZone &zone, bool any_body_tier);

// Outline to hand the plater for an offending object: the instance hull grown by the same half
// clearance the check grew it by, which is CompactedTowerClearance::body_clearance for that object.
// Sequential printing reports its hulls the same way, and it doubles as the fix for the bare hull
// being unusable on screen, where drawn flat it hides under the object and drawn at the height limit
// it ends up buried inside the mesh.
Polygon compacted_wipe_tower_offender_outline(const Polygon &inst_hull, double body_clearance);

} /* slic3r_Print_hpp_ */

#endif
