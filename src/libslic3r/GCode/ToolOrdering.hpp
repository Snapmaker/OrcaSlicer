// Ordering of the tools to minimize tool switches.

#ifndef slic3r_ToolOrdering_hpp_
#define slic3r_ToolOrdering_hpp_

#include "../libslic3r.h"
#include "../MixedFilament.hpp"
#include "../ParameterUtils.hpp" // LayerPrintSequence, used by make_cyclic_custom_seq
#include "../MultiNozzleUtils.hpp" // Ultra (dual-nozzle): grouping result types

#include <utility>
#include <set>
#include <map>
#include <vector>
#include <functional>
#include <string>
#include <optional>

#include <boost/container/small_vector.hpp>

namespace Slic3r {

class Print;
class PrintObject;
class LayerTools;
namespace CustomGCode { struct Item; }
class PrintRegion;

// Object of this class holds information about whether an extrusion is printed immediately
// after a toolchange (as part of infill/perimeter wiping) or not. One extrusion can be a part
// of several copies - this has to be taken into account.
class WipingExtrusions
{
public:
    bool is_anything_overridden() const {   // if there are no overrides, all the agenda can be skipped - this function can tell us if that's the case
        return something_overridden;
    }

    // When allocating extruder overrides of an object's ExtrusionEntity, overrides for maximum 3 copies are allocated in place.
    typedef boost::container::small_vector<int32_t, 3> ExtruderPerCopy;

    // This is called from GCode::process_layer - see implementation for further comments:
    const ExtruderPerCopy* get_extruder_overrides(const ExtrusionEntity* entity, const PrintObject* object, int correct_extruder_id, size_t num_of_copies);
    int get_support_extruder_overrides(const PrintObject* object);
    int get_support_interface_extruder_overrides(const PrintObject* object);

    // This function goes through all infill entities, decides which ones will be used for wiping and
    // marks them by the extruder id. Returns volume that remains to be wiped on the wipe tower:
    float mark_wiping_extrusions(const Print& print, unsigned int old_extruder, unsigned int new_extruder, float volume_to_wipe);

    void ensure_perimeters_infills_order(const Print& print);

    bool is_overriddable(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) const;
    bool is_overriddable_and_mark(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) {
    	bool out = this->is_overriddable(ee, print_config, object, region);
    	this->something_overridable |= out;
    	return out;
    }

    // BBS
    bool is_support_overriddable(const ExtrusionRole role, const PrintObject& object) const;
    bool is_support_overriddable_and_mark(const ExtrusionRole role, const PrintObject& object) {
        bool out = this->is_support_overriddable(role, object);
        this->something_overridable |= out;
        return out;
    }

    bool is_support_overridden(const PrintObject* object) const {
        return support_map.find(object) != support_map.end();
    }

    bool is_support_interface_overridden(const PrintObject* object) const {
        return support_intf_map.find(object) != support_intf_map.end();
    }

    void set_layer_tools_ptr(const LayerTools* lt) { m_layer_tools = lt; }

private:
    int first_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;
    int last_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;

    // This function is called from mark_wiping_extrusions and sets extruder that it should be printed with (-1 .. as usual)
    void set_extruder_override(const ExtrusionEntity* entity, const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    // BBS
    void set_support_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    void set_support_interface_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);

    // Returns true in case that entity is not printed with its usual extruder for a given copy:
    bool is_entity_overridden(const ExtrusionEntity* entity, const PrintObject *object, size_t copy_id) const {
        auto it = entity_map.find(std::make_tuple(entity, object));
        return it == entity_map.end() ? false : it->second[copy_id] != -1;
    }

    std::map<std::tuple<const ExtrusionEntity*, const PrintObject *>, ExtruderPerCopy> entity_map;  // to keep track of who prints what
    // BBS
    std::map<const PrintObject*, int> support_map;
    std::map<const PrintObject*, int> support_intf_map;
    bool something_overridable = false;
    bool something_overridden = false;
    const LayerTools* m_layer_tools = nullptr;    // so we know which LayerTools object this belongs to
};

// H2 preload (preload_all_filaments): layer 1's tool-sequence rule, factored out of ToolOrdering so
// it can be exercised without building a Print. Given layer 1's own (already ordered) sequence, the
// set of filaments used anywhere on the plate, and a filament -> physical-extruder lookup, returns
// layer 1's new sequence:
//
//     [ preload-only filaments ] ++ [ layer0_extruders, unchanged ]
//
// The preload-only filaments (used on the plate but not printed on layer 1) are grouped by physical
// extruder so each extruder's loads are consecutive, the group that layer 1's FIRST filament belongs
// to is placed last among them, and within a group filaments are ascending by id. Layer 1's own order
// is never reordered, so the result still ends on the filament layer 1 actually finishes with.
std::vector<unsigned int> preload_first_layer_tool_order(const std::vector<unsigned int> &layer0_extruders,
                                                         const std::vector<unsigned int> &used_filaments,
                                                         const std::function<int(unsigned int)> &extruder_of_filament);

class LayerTools
{
public:
    LayerTools(const coordf_t z) : print_z(z) {}

    // Changing these operators to epsilon version can make a problem in cases where support and object layers get close to each other.
    // In case someone tries to do it, make sure you know what you're doing and test it properly (slice multiple objects at once with supports).
    bool operator< (const LayerTools &rhs) const { return print_z < rhs.print_z; }
    bool operator==(const LayerTools &rhs) const { return print_z == rhs.print_z; }

    bool is_extruder_order(unsigned int a, unsigned int b) const;
    bool has_extruder(unsigned int extruder) const { return std::find(this->extruders.begin(), this->extruders.end(), extruder) != this->extruders.end(); }

    // Resolve a 1-based filament ID the same way ToolOrdering::collect_extruders
    // schedules support: MixedFilamentManager::resolve plus the auto-row A/B
    // layer-height cycle (mixed_color_layer_height_a/_b). GCode support
    // bucketing must use this, not mixed_mgr->resolve() directly.
    unsigned int resolve_mixed_1based_at(unsigned int        filament_id,
                                         float               layer_print_z,
                                         float               layer_height,
                                         const PrintObject  *current_object) const;

    // Return a zero based extruder from the region, or extruder_override if overriden.
    unsigned int wall_filament(const PrintRegion &region) const;
    unsigned int outer_wall_filament(const PrintRegion &region) const;
    unsigned int sparse_infill_filament(const PrintRegion &region) const;
    unsigned int solid_infill_filament(const PrintRegion &region) const;
	// Returns a zero based extruder this eec should be printed with, according to PrintRegion config or extruder_override if overriden.
	unsigned int extruder(const ExtrusionEntityCollection &extrusions, const PrintRegion &region) const;

    coordf_t 					print_z	= 0.;
    bool 						has_object = false;
    bool						has_support = false;
    // Zero based extruder IDs, ordered to minimize tool switches.
    std::vector<unsigned int> 	extruders;
    bool                        preserve_extruder_order = false;
    // If per layer extruder switches are inserted by the G-code preview slider, this value contains the new (1 based) extruder, with which the whole object layer is being printed with.
    // If not overriden, it is set to 0.
    unsigned int 				extruder_override = 0;
    // Sequential layer index (0-based), used by mixed-filament resolution.
    int                         layer_index = 0;
    // Total number of object layers for the current print object.
    int                         object_layer_count = 0;
    // Actual layer height for this print_z where available.
    coordf_t                    layer_height = 0.;
    // Should a skirt be printed at this layer?
    // Layers are marked for infinite skirt aka draft shield. Not all the layers have to be printed.
    bool                        has_skirt = false;
    // Will there be anything extruded on this layer for the wipe tower?
    // Due to the support layers possibly interleaving the object layers,
    // wipe tower will be disabled for some support only layers.
    bool 						has_wipe_tower = false;
    // Number of wipe tower partitions to support the required number of tool switches
    // and to support the wipe tower partitions above this one.
    size_t                      wipe_tower_partitions = 0;
    coordf_t 					wipe_tower_layer_height = 0.;
    // Custom G-code (color change, extruder switch, pause) to be performed before this layer starts to print.
    const CustomGCode::Item    *custom_gcode = nullptr;

    WipingExtrusions& wiping_extrusions() {
        m_wiping_extrusions.set_layer_tools_ptr(this);
        return m_wiping_extrusions;
    }

    // Mixed-filament resolution context (set by ToolOrdering during collect_extruders).
    const MixedFilamentManager *mixed_mgr    = nullptr;
    size_t                      num_physical = 0;
    // Optional mixed-layer cadence override from print settings.
    float                       mixed_layer_height_a    = 0.f;
    float                       mixed_layer_height_b    = 0.f;
    float                       mixed_base_layer_height = 0.2f;
    const PrintObject          *current_object = nullptr;

private:
    // Resolve a 1-based filament ID through the mixed-filament manager for this layer.
    unsigned int resolve_mixed_1based(unsigned int filament_id) const;
    // Wave A fix-wave / C-1 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md): resolve a
    // 1-based filament id for a wall/gap-fill/infill role through the grouped-manual-pattern
    // resolution first (if the id is a mixed filament with a grouped manual pattern), falling
    // back to resolve_mixed_1based otherwise. Shared by wall_filament, sparse_infill_filament and
    // solid_infill_filament so the three lookups can never diverge again - see wall_filament's
    // own comment for why that mattered.
    unsigned int resolve_grouped_or_mixed_1based(const PrintRegion &region, unsigned int filament_id) const;
    // This object holds list of extrusion that will be used for extruder wiping
    WipingExtrusions m_wiping_extrusions;
};

class ToolOrdering
{
public:
    ToolOrdering() = default;

    // For the use case when each object is printed separately
    // (print->config().print_sequence == PrintSequence::ByObject is true).
    ToolOrdering(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material = false);

    // For the use case when all objects are printed at once.
    // (print->config().print_sequence == PrintSequence::ByObject is false).
    ToolOrdering(const Print& print, unsigned int first_extruder, bool prime_multi_material = false);

    void 				clear() {
        m_layer_tools.clear(); m_tool_order_cache.clear(); 
    }

    // Only valid for non-sequential print:
	// Assign a pointer to a custom G-code to the respective ToolOrdering::LayerTools.
	// Ignore color changes, which are performed on a layer and for such an extruder, that the extruder will not be printing above that layer.
	// If multiple events are planned over a span of a single layer, use the last one.
	void 				assign_custom_gcodes(const Print &print);

    // Get the first extruder printing, including the extruder priming areas, returns -1 if there is no layer printed.
    unsigned int   		first_extruder() const { return m_first_printing_extruder; }

    // Get the first extruder printing the layer_tools, returns -1 if there is no layer printed.
    unsigned int   		last_extruder() const { return m_last_printing_extruder; }

    // For a multi-material print, the printing extruders are ordered in the order they shall be primed.
    const std::vector<unsigned int>& all_extruders() const { return m_all_printing_extruders; }

    // Find LayerTools with the closest print_z.
    const LayerTools&	tools_for_layer(coordf_t print_z) const;
    LayerTools&			tools_for_layer(coordf_t print_z) { return const_cast<LayerTools&>(std::as_const(*this).tools_for_layer(print_z)); }

    const LayerTools&   front()       const { return m_layer_tools.front(); }
    const LayerTools&   back()        const { return m_layer_tools.back(); }
    std::vector<LayerTools>::const_iterator begin() const { return m_layer_tools.begin(); }
    std::vector<LayerTools>::const_iterator end()   const { return m_layer_tools.end(); }
    bool 				empty()       const { return m_layer_tools.empty(); }
    std::vector<LayerTools>& layer_tools() { return m_layer_tools; }
    bool 				has_wipe_tower() const { return ! m_layer_tools.empty() && m_first_printing_extruder != (unsigned int)-1 && m_layer_tools.front().has_wipe_tower; }
    // A print-by-object plate on a grouping-model machine (H2D/H2C/X2D) skips the grouping of
    // reorder_extruders_for_minimum_flush_volume(); this groups it from the plate's own filament
    // map (manual mode) for the given per-layer filament lists. See GCode::_do_export.
    static MultiNozzleUtils::LayeredNozzleGroupResult group_by_plate_map(Print *print, const std::vector<std::vector<unsigned int>> &layer_filaments);

private:
    void				initialize_layers(std::vector<coordf_t> &zs);
    void 				collect_extruders(const PrintObject &object, const std::vector<std::pair<double, unsigned int>> &per_layer_extruder_switches);
    void				reorder_extruders(unsigned int last_extruder_id);
    // BBS
    void                reorder_extruders(std::vector<unsigned int> tool_order_layer0);
    void 				fill_wipe_tower_partitions(const PrintConfig &config, coordf_t object_bottom_z, coordf_t max_layer_height);
    bool                insert_wipe_tower_extruder();   
    // H2 preload (preload_all_filaments): prepend every plate filament that layer 1 does not
    // already print to layer 1's tool sequence, so the prime tower performs a real toolchange
    // for each of them on layer 1. Returns true when it changed the first layer.
    bool                apply_preload_all_filaments();
    void                mark_skirt_layers(const PrintConfig &config, coordf_t max_layer_height);
    void 				collect_extruder_statistics(bool prime_multi_material);
    void                reorder_extruders_for_minimum_flush_volume();

    // Ultra (dual-nozzle): per-slice grouping inputs, and the compute entry that turns them into a
    // filament->nozzle grouping result. Only invoked for grouping-model machines (H2D/H2C/X2D); classic
    // machines skip this path entirely. See reorder_extruders_for_minimum_flush_volume().
    struct LayerData {
        std::vector<std::vector<unsigned int>>       layer_filaments;
        std::vector<unsigned int>                    used_filaments;
        std::vector<std::set<int>>                   physical_unprintables;
        std::vector<std::set<int>>                   geometric_unprintables;
        std::map<int, std::set<NozzleVolumeType>>    filament_unprintable_volumes;
    };
    LayerData collect_layer_and_unprintable_data();
    static MultiNozzleUtils::LayeredNozzleGroupResult get_recommended_filament_maps(
        Print*                                            print,
        const std::vector<std::vector<unsigned int>>&     layer_filaments,
        const FilamentMapMode                             mode,
        const std::vector<std::set<int>>&                 physical_unprintables,
        const std::vector<std::set<int>>&                 geometric_unprintables,
        const std::map<int, std::set<NozzleVolumeType>>&  unprintable_volumes,
        const std::unordered_map<int, int>&               nozzle_status = {});

    // BBS
    std::vector<unsigned int> generate_first_layer_tool_order(const Print& print);
    std::vector<unsigned int> generate_first_layer_tool_order(const PrintObject& object);
    void                      update_mixed_layer_height_settings();

    // Resolve a 1-based filament ID through the mixed-filament manager.
    // Returns the resolved physical extruder (1-based).  If the ID is not a
    // mixed filament or no manager is set, returns the input unchanged.
    unsigned int resolve_mixed(unsigned int filament_id_1based,
                               int          layer_index,
                               float        layer_print_z = 0.f,
                               float        layer_height  = 0.f,
                               const PrintObject* current_object = nullptr) const;

    std::vector<LayerTools>    m_layer_tools;
    // First printing extruder, including the multi-material priming sequence.
    unsigned int               m_first_printing_extruder = (unsigned int)-1;
    // Final printing extruder.
    unsigned int               m_last_printing_extruder  = (unsigned int)-1;
    // All extruders, which extrude some material over m_layer_tools.
    std::vector<unsigned int>  m_all_printing_extruders;
    std::unordered_map<uint32_t, std::vector<uint8_t>> m_tool_order_cache;
    const DynamicPrintConfig*  m_print_full_config = nullptr;
    const PrintConfig*         m_print_config_ptr = nullptr;
    const PrintObject*         m_print_object_ptr = nullptr;
    // Ultra (dual-nozzle): non-const handle to the owning Print, set in the Print constructor so the
    // grouping compute can store its result. Null in the ByObject/PrintObject path (grouping is skipped
    // there anyway). See reorder_extruders_for_minimum_flush_volume().
    Print*                     m_print = nullptr;
    bool                       m_is_BBL_printer = false;
    // Mixed filament support: pointer to manager (owned by Print) and
    // number of physical extruders.
    const MixedFilamentManager* m_mixed_mgr    = nullptr;
    size_t                      m_num_physical  = 0;
    float                       m_mixed_layer_height_a    = 0.f;
    float                       m_mixed_layer_height_b    = 0.f;
    float                       m_mixed_base_layer_height = 0.2f;
};

// Parse the user defined cyclic toolchange sequence ("3,2 , 1 , 4") into 0-based filament indices.
// Out-of-range entries, duplicates and non-numeric tokens are dropped, so a partially valid string
// still orders the filaments it does name. Exposed for unit testing.
std::vector<unsigned int> parse_cyclic_order(const std::string& str, unsigned int number_of_extruders);

// Classic DP flush-volume solver used by reorder_extruders_for_minimum_flush_volume.
// Snapmaker #754: exposed so Catch2 can drive the wipe_volumes OOB guard without constructing a Print.
std::vector<unsigned int> get_extruders_order(const std::vector<std::vector<float>> &wipe_volumes,
                                              std::vector<unsigned int>              all_extruders,
                                              std::optional<unsigned int>            start_extruder_id);

// Reorder a layer's 0-based filaments for cyclic ordering: ascending by default, or following
// cyclic_order when non-empty. Filaments absent from the sequence keep ascending order after
// the listed ones. Exposed for unit testing.
void apply_cyclic_order(std::vector<unsigned int>& filaments, const std::vector<unsigned int>& cyclic_order);

// The filament-index range a cyclic sequence is validated against: physical filaments plus the
// enabled mixed ones, i.e. exactly the indices ToolOrdering itself uses. Deliberately not the
// sqrt of the flush matrix, so a short flush_volumes_matrix cannot silently truncate a user
// sequence. number_of_extruders is only the fallback when there is no physical count at all.
// Exposed for unit testing.
unsigned int cyclic_filament_count(size_t num_physical, const MixedFilamentManager *mixed_mgr, unsigned int number_of_extruders);

// Derive a cyclic base order from the plate's customized filament sequence (the per-plate
// "other layers print sequence" ranges): the first customized range with a usable sequence
// supplies the order, its 1-based filament numbers converted to 0-based indices. Entries
// outside [1, filament_count] and duplicates are dropped, mirroring parse_cyclic_order.
// Returns empty when nothing is customized, leaving the caller the plain ascending order.
// Exposed for unit testing.
std::vector<unsigned int> cyclic_order_from_customized_sequence(
    const std::vector<LayerPrintSequence> &other_layers_seqs, unsigned int filament_count);

// Build the per-layer custom-sequence callback used by reorder_extruders_for_minimum_flush_volume.
// An explicit other_layers_print_sequence always wins; otherwise, under cyclic ordering, the layer
// is reordered by cyclic_order - except layer 0, which keeps its adhesion-optimized order unless
// cyclic_first_layer is set. Returns false when the layer should keep the order it already has.
// Exposed for unit testing.
std::function<bool(int, std::vector<int>&)> make_cyclic_custom_seq(
    const std::vector<LayerPrintSequence>        &other_layers_seqs,
    const std::vector<std::vector<unsigned int>> &layer_filaments,
    bool                                          use_cyclic_ordering,
    bool                                          cyclic_first_layer,
    const std::vector<unsigned int>              &cyclic_order);


} // namespace SLic3r

#endif /* slic3r_ToolOrdering_hpp_ */
