#include "ExtrusionEntity.hpp"
#include "Print.hpp"
#include "ToolOrdering.hpp"
#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "ParameterUtils.hpp"
#include "../BambuFlowSupport.hpp"
#include "../BrimFilament.hpp"
// Ultra (dual-nozzle): filament->nozzle grouping compute.
#include "../FilamentGroup.hpp"
#include "../FilamentGroupUtils.hpp"
#include "../ImageRowWalls.hpp"
#include "ToolOrderUtils.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
#endif

#include <cassert>
#include <limits>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <exception>

#include <boost/log/trivial.hpp>

#include <libslic3r.h>

namespace Slic3r {

const static bool g_wipe_into_objects = false;

namespace {

unsigned int resolve_mixed_with_layer_heights(const MixedFilamentManager *mixed_mgr,
                                              size_t                      num_physical,
                                              unsigned int                filament_id_1based,
                                              int                         layer_index,
                                              float                       layer_print_z,
                                              float                       layer_height,
                                              float                       layer_height_a,
                                              float                       layer_height_b,
                                              float                       base_layer_height,
                                              const PrintObject*          current_object = nullptr)
{
    if (!(mixed_mgr && mixed_mgr->is_mixed(filament_id_1based, num_physical)))
        return filament_id_1based;

    const MixedFilament *mixed_row = mixed_mgr->mixed_filament_from_id(filament_id_1based, num_physical);

    // Z-direction gradient short-circuits the legacy A/B layer cycle below: the
    // gradient path needs the per-(object, layer) run lookup performed inside
    // MixedFilamentManager::resolve.
    const bool gradient_active = 
        (mixed_row != nullptr && current_object != nullptr) &&
        (mixed_row->gradient_enabled && mixed_row->component_a != mixed_row->component_b) &&
        (layer_index > 0);

    if (mixed_row != nullptr && !mixed_row->custom && !gradient_active && (layer_height_a > 0.f || layer_height_b > 0.f)) {
        const float safe_base = std::max<float>(0.01f, base_layer_height);
        const int ratio_a = std::max(1, int(std::lround((layer_height_a > 0.f ? layer_height_a : safe_base) / safe_base)));
        const int ratio_b = std::max(1, int(std::lround((layer_height_b > 0.f ? layer_height_b : safe_base) / safe_base)));
        const int cycle   = ratio_a + ratio_b;

        if (cycle > 0) {
            if (mixed_row != nullptr) {
                const int pos = ((layer_index % cycle) + cycle) % cycle;
                return pos < ratio_a ? mixed_row->component_a : mixed_row->component_b;
            }
        }
    }

    return mixed_mgr->resolve(filament_id_1based, num_physical, layer_index, layer_print_z, layer_height, false, current_object);
}

bool has_grouped_manual_pattern(const MixedFilamentManager *mixed_mgr,
                                size_t                      num_physical,
                                unsigned int                filament_id_1based)
{
    if (!(mixed_mgr && mixed_mgr->is_mixed(filament_id_1based, num_physical)))
        return false;
    const MixedFilament *mixed_row = mixed_mgr->mixed_filament_from_id(filament_id_1based, num_physical);
    if (mixed_row == nullptr)
        return false;
    const std::string normalized = MixedFilamentManager::normalize_manual_pattern(mixed_row->manual_pattern);
    return normalized.find(',') != std::string::npos;
}

void append_unique_preserve_order(std::vector<unsigned int> &dst, unsigned int value)
{
    if (std::find(dst.begin(), dst.end(), value) == dst.end())
        dst.emplace_back(value);
}

// Orca #13582: Default starts each layer with the previous layer's last extruder (fewer toolchanges).
// Cyclic (and any non-Default mode) leaves the collected order so get_custom_seq can apply a fixed
// sequence. Null config keeps Default, matching Orca's handle_dontcare_extruder.
const PrintConfig *toolordering_print_config(const PrintConfig *ptr, const PrintObject *object)
{
    if (ptr)
        return ptr;
    if (object)
        return &object->print()->config();
    return nullptr;
}

bool use_default_toolchange_ordering(const PrintConfig *print_config)
{
    return print_config == nullptr || print_config->toolchange_ordering == ToolChangeOrderingType::Default;
}

void rotate_last_extruder_to_front(std::vector<unsigned int> &extruders, unsigned int last_extruder_id)
{
    for (size_t i = 1; i < extruders.size(); ++i) {
        if (extruders[i] == last_extruder_id) {
            std::rotate(extruders.begin(), extruders.begin() + i, extruders.begin() + i + 1);
            break;
        }
    }
}

bool internal_solid_infill_uses_sparse_filament(const PrintRegion &region, ExtrusionRole role)
{
    return role == erSolidInfill && std::abs(region.config().sparse_infill_density.value - 100.) < EPSILON;
}

unsigned int sparse_infill_filament_id_1based(const PrintRegion &region)
{
    return region.config().sparse_infill_filament.value;
}

unsigned int infill_filament_id_1based(const LayerTools &layer_tools, const PrintRegion &region, ExtrusionRole role)
{
    if (internal_solid_infill_uses_sparse_filament(region, role))
        return sparse_infill_filament_id_1based(region);
    return is_solid_infill(role) ? region.config().solid_infill_filament.value : sparse_infill_filament_id_1based(region);
}

unsigned int grouped_manual_pattern_mixed_filament_id_for_layer(const LayerTools& layer_tools,
                                                                unsigned int      configured_filament_id_1based)
{
    if (layer_tools.mixed_mgr == nullptr || layer_tools.num_physical == 0)
        return 0;

    if (has_grouped_manual_pattern(layer_tools.mixed_mgr, layer_tools.num_physical, configured_filament_id_1based))
        return configured_filament_id_1based;
    return 0;
}

unsigned int grouped_manual_pattern_infill_filament_1based(const LayerTools&  layer_tools,
                                                           const PrintRegion& region,
                                                           unsigned int       configured_filament_id_1based)
{
    const unsigned int grouped_id =
        grouped_manual_pattern_mixed_filament_id_for_layer(layer_tools, configured_filament_id_1based);
    if (grouped_id == 0)
        return 0;

    const int innermost_perimeter_index = std::max(0, region.config().wall_loops.value - 1);
    return layer_tools.mixed_mgr->resolve_perimeter(grouped_id,
                                                    layer_tools.num_physical,
                                                    layer_tools.layer_index,
                                                    innermost_perimeter_index,
                                                    float(layer_tools.print_z),
                                                    float(layer_tools.layer_height),
                                                    false,
                                                    layer_tools.current_object);
}

void remove_duplicates_preserve_order(std::vector<unsigned int> &values)
{
    std::vector<unsigned int> ordered;
    ordered.reserve(values.size());
    for (unsigned int value : values)
        append_unique_preserve_order(ordered, value);
    values = std::move(ordered);
}

} // namespace


// Shortest hamilton path problem
static std::vector<unsigned int> solve_extruder_order(const std::vector<std::vector<float>>& wipe_volumes, std::vector<unsigned int> all_extruders, std::optional<unsigned int> start_extruder_id) 
{
    // Snapmaker #754: an extruder id outside wipe_volumes is a near-null OOB
    // (Sentry SNAPMAKER_ORCA-7VR). Keep the input order rather than indexing the matrix.
    for (auto id : all_extruders) {
        if (id >= wipe_volumes.size())
            return all_extruders;
    }

    bool add_start_extruder_flag = false;

    if (start_extruder_id) {
        auto start_iter = std::find(all_extruders.begin(), all_extruders.end(), start_extruder_id);
        if (start_iter == all_extruders.end())
            all_extruders.insert(all_extruders.begin(), *start_extruder_id), add_start_extruder_flag = true;
        else
            std::swap(*all_extruders.begin(), *start_iter);
    }
    else {
        start_extruder_id = all_extruders.front();
    }

    unsigned int iterations = (1 << all_extruders.size());
    unsigned int final_state = iterations - 1;
    std::vector<std::vector<float>>cache(iterations, std::vector<float>(all_extruders.size(),0x7fffffff));
    std::vector<std::vector<int>>prev(iterations, std::vector<int>(all_extruders.size(), -1));
    cache[1][0] = 0.;
    for (unsigned int state = 0; state < iterations; ++state) {
        if (state & 1) {
            for (unsigned int target = 0; target < all_extruders.size(); ++target) {
                if (state >> target & 1) {
                    for (unsigned int mid_point = 0; mid_point < all_extruders.size(); ++mid_point) {
                        if(state>>mid_point&1){
                            auto tmp = cache[state - (1 << target)][mid_point] + wipe_volumes[all_extruders[mid_point]][all_extruders[target]];
                            if (cache[state][target] >tmp) {
                                cache[state][target] = tmp;
                                prev[state][target] = mid_point;
                            }
                        }
                    }
                }
            }
        }
    }

    //get res
    float cost = std::numeric_limits<float>::max();
    int final_dst =0;
    for (unsigned int dst = 0; dst < all_extruders.size(); ++dst) {
        if (all_extruders[dst] != start_extruder_id && cost > cache[final_state][dst]) {
            cost = cache[final_state][dst];
            final_dst = dst;
        }
    }

    std::vector<unsigned int>path;
    unsigned int curr_state = final_state;
    int curr_point = final_dst;
    while (curr_point != -1) {
        path.emplace_back(all_extruders[curr_point]);
        auto mid_point = prev[curr_state][curr_point];
        curr_state -= (1 << curr_point);
        curr_point = mid_point;
    };

    if (add_start_extruder_flag)
        path.pop_back();

    std::reverse(path.begin(), path.end());
    return path;
}

// Declared in ToolOrdering.hpp (exposed for unit testing). One-line public wrapper around the
// static DP solver so Catch2 [ToolOrdering][OOB] can link without exporting solve_extruder_order.
std::vector<unsigned int> get_extruders_order(const std::vector<std::vector<float>> &wipe_volumes,
                                              std::vector<unsigned int>              all_extruders,
                                              std::optional<unsigned int>            start_extruder_id)
{
    return solve_extruder_order(wipe_volumes, all_extruders, start_extruder_id);
}

// Returns true in case that extruder a comes before b (b does not have to be present). False otherwise.
bool LayerTools::is_extruder_order(unsigned int a, unsigned int b) const
{
    if (a == b)
        return false;

    for (auto extruder : extruders) {
        if (extruder == a)
            return true;
        if (extruder == b)
            return false;
    }

    return false;
}

unsigned int LayerTools::resolve_mixed_1based_at(unsigned int       filament_id,
                                                 float              layer_print_z,
                                                 float              layer_height,
                                                 const PrintObject *current_object) const
{
    return resolve_mixed_with_layer_heights(mixed_mgr,
                                            num_physical,
                                            filament_id,
                                            this->layer_index,
                                            layer_print_z,
                                            layer_height,
                                            mixed_layer_height_a,
                                            mixed_layer_height_b,
                                            mixed_base_layer_height,
                                            current_object != nullptr ? current_object : this->current_object);
}

// Resolve a 1-based filament ID through the mixed-filament manager for this layer.
unsigned int LayerTools::resolve_mixed_1based(unsigned int filament_id) const
{
    return resolve_mixed_1based_at(filament_id, float(this->print_z), float(this->layer_height), this->current_object);
}

// Wave A fix-wave / C-1 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md): wall_filament,
// sparse_infill_filament and solid_infill_filament all route their id through this one helper so
// they can never resolve the SAME configured filament id to DIFFERENT physical extruders again.
// Before this fix wall_filament called resolve_mixed_1based directly, skipping the
// grouped_manual_pattern_infill_filament_1based step sparse_infill_filament/solid_infill_filament
// already applied - harmless wherever wall_filament == sparse_infill_filament resolve to the SAME
// physical extruder either way (every non-mixed print, hence the unpainted byte-parity gate), but
// not for a mixed filament whose manual_pattern groups perimeters differently than the flattened
// (layer-cycle) pattern resolve_mixed_1based reads: gap fill (routed through wall_filament, since
// it is generated as part of the wall stack) could then print on a different physical extruder
// than the walls it sits directly behind, on a print with no MM painting anywhere.
unsigned int LayerTools::resolve_grouped_or_mixed_1based(const PrintRegion &region, unsigned int filament_id) const
{
    const unsigned int grouped = grouped_manual_pattern_infill_filament_1based(*this, region, filament_id);
    return (grouped != 0) ? grouped : resolve_mixed_1based(filament_id);
}

// Return a zero based extruder from the region, or extruder_override if overriden.
unsigned int LayerTools::wall_filament(const PrintRegion &region) const
{
	assert(region.config().wall_filament.value > 0);
	unsigned int id = (this->extruder_override == 0) ? region.config().wall_filament.value : this->extruder_override;
	return resolve_grouped_or_mixed_1based(region, id) - 1;
}

// Ultra: zero based extruder for outer walls (falls back to wall_filament when unset).
unsigned int LayerTools::outer_wall_filament(const PrintRegion &region) const
{
	if (this->extruder_override != 0)
		return resolve_mixed_1based(this->extruder_override) - 1;
	const int outer = region.config().outer_wall_filament.value;
	if (outer <= 0)
		return wall_filament(region);
	return resolve_mixed_1based(unsigned(outer)) - 1;
}

unsigned int LayerTools::sparse_infill_filament(const PrintRegion &region) const
{
	assert(region.config().wall_filament.value > 0);
	unsigned int id = (this->extruder_override == 0) ? sparse_infill_filament_id_1based(region) : this->extruder_override;
	return resolve_grouped_or_mixed_1based(region, id) - 1;
}

unsigned int LayerTools::solid_infill_filament(const PrintRegion &region) const
{
	assert(region.config().solid_infill_filament.value > 0);
	unsigned int id = (this->extruder_override == 0) ? region.config().solid_infill_filament.value : this->extruder_override;
	return resolve_grouped_or_mixed_1based(region, id) - 1;
}

// Returns a zero based extruder this eec should be printed with, according to PrintRegion config or extruder_override if overriden.
unsigned int LayerTools::extruder(const ExtrusionEntityCollection &extrusions, const PrintRegion &region) const
{
	assert(region.config().wall_filament.value > 0);
	assert(region.config().sparse_infill_filament.value > 0);
	assert(region.config().solid_infill_filament.value > 0);
    // Phase 3 (image row): this collection already carries its own physical filament (0 for
    // every collection in every print that does not use the feature) - see
    // ExtrusionEntityCollection::image_row_extruder_1based's own comment. Answering from the
    // region's config here would be wrong for the one caller of this method that is not
    // ToolOrdering::collect_extruders itself: WipingExtrusions::is_overriddable()'s soluble
    // check, which would otherwise ask "is the REGION's nominal filament soluble" about a
    // collection that never prints with it.
    if (extrusions.image_row_extruder_1based != 0)
        return resolve_mixed_1based(extrusions.image_row_extruder_1based) - 1;
    if (extrusions.has_infill()) {
        const ExtrusionRole role = extrusions.entities.empty() ? erNone : extrusions.entities.front()->role();
        if (internal_solid_infill_uses_sparse_filament(region, role))
            return sparse_infill_filament(region);
        return is_solid_infill(role) ? solid_infill_filament(region) : sparse_infill_filament(region);
    }
    return wall_filament(region);
}

static double calc_max_layer_height(const PrintConfig &config, double max_object_layer_height)
{
    double max_layer_height = std::numeric_limits<double>::max();
    for (size_t i = 0; i < config.nozzle_diameter.values.size(); ++ i) {
        double mlh = config.max_layer_height.values[i];
        if (mlh == 0.)
            mlh = 0.75 * config.nozzle_diameter.values[i];
        max_layer_height = std::min(max_layer_height, mlh);
    }
    // The Prusa3D Fast (0.35mm layer height) print profile sets a higher layer height than what is normally allowed
    // by the nozzle. This is a hack and it works by increasing extrusion width. See GH #3919.
    return std::max(max_layer_height, max_object_layer_height);
}

// For the use case when each object is printed separately
// (print->config().print_sequence == PrintSequence::ByObject is true).
ToolOrdering::ToolOrdering(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material)
{
    m_is_BBL_printer = object.print()->is_BBL_printer();
    m_print_full_config = &object.print()->full_print_config();
    m_print_config_ptr = &object.print()->config();
    m_print_object_ptr = &object;
    // Mixed filament support.
    m_mixed_mgr   = &object.print()->mixed_filament_manager();
    m_num_physical = object.print()->config().filament_diameter.size();
    update_mixed_layer_height_settings();
    if (object.layers().empty())
        return;

    // Initialize the print layers for just a single object.
    {
        std::vector<coordf_t> zs;
        zs.reserve(zs.size() + object.layers().size() + object.support_layers().size());
        for (auto layer : object.layers())
            zs.emplace_back(layer->print_z);
        for (auto layer : object.support_layers())
            zs.emplace_back(layer->print_z);
        this->initialize_layers(zs);
    }
    double max_layer_height = calc_max_layer_height(object.print()->config(), object.config().layer_height);

    // Collect extruders reuqired to print the layers.
    this->collect_extruders(object, std::vector<std::pair<double, unsigned int>>());

    // BBS
    // Reorder the extruders to minimize tool switches.
    std::vector<unsigned int> first_layer_tool_order;
    if (first_extruder == (unsigned int) -1) {
        first_layer_tool_order = generate_first_layer_tool_order(object);
    }

    if (!first_layer_tool_order.empty()) {
        this->reorder_extruders(first_layer_tool_order);
    } else {
        this->reorder_extruders(first_extruder);
    }

    this->fill_wipe_tower_partitions(object.print()->config(), object.layers().front()->print_z - object.layers().front()->height, max_layer_height);

    this->collect_extruder_statistics(prime_multi_material);

    this->mark_skirt_layers(object.print()->config(), max_layer_height);
}

bool ToolOrdering::insert_wipe_tower_extruder()
{
    if(!m_print_config_ptr->enable_prime_tower)
        return false;
    // In case that wipe_tower_extruder is set to non-zero, we must make sure that the extruder will be in the list.
    bool changed = false;
    if (m_print_config_ptr->wipe_tower_filament != 0) {
        for (LayerTools& lt : m_layer_tools) {
            if (lt.wipe_tower_partitions > 0) {
                lt.extruders.emplace_back(m_print_config_ptr->wipe_tower_filament - 1);
                sort_remove_duplicates(lt.extruders);
                changed = true;
            }
        }  
    }
    return changed;
}

// For the use case when all objects are printed at once.
// (print->config().print_sequence == PrintSequence::ByObject is false).
ToolOrdering::ToolOrdering(const Print &print, unsigned int first_extruder, bool prime_multi_material)
{
    m_is_BBL_printer = print.is_BBL_printer();
    m_print_full_config = &print.full_print_config();
    m_print_config_ptr = &print.config();
    // Ultra (dual-nozzle): non-const handle so reorder_extruders_for_minimum_flush_volume can store the
    // grouping result. Safe: ToolOrdering runs inside the owning Print's process().
    m_print = const_cast<Print*>(&print);
    // Mixed filament support.
    m_mixed_mgr   = &print.mixed_filament_manager();
    m_num_physical = print.config().filament_diameter.size();
    update_mixed_layer_height_settings();

    // Initialize the print layers for all objects and all layers.
    coordf_t object_bottom_z = 0.;
    coordf_t max_layer_height = 0.;
    {
        std::vector<coordf_t> zs;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers())
                zs.emplace_back(layer->print_z);
            for (auto layer : object->support_layers())
                zs.emplace_back(layer->print_z);

            // Find first object layer that is not empty and save its print_z
            for (const Layer* layer : object->layers())
                if (layer->has_extrusions()) {
                    object_bottom_z = layer->print_z - layer->height;
                    break;
                }

            max_layer_height = std::max(max_layer_height, object->config().layer_height.value);
        }
        this->initialize_layers(zs);
    }
    max_layer_height = calc_max_layer_height(print.config(), max_layer_height);

	// Use the extruder switches from Model::custom_gcode_per_print_z to override the extruder to print the object.
	// Do it only if all the objects were configured to be printed with a single extruder.
	std::vector<std::pair<double, unsigned int>> per_layer_extruder_switches;

    // BBS
	if (auto num_physical = unsigned(print.config().filament_diameter.size());
		num_physical > 1 && print.object_extruders().size() == 1 && // the current Print's configuration is CustomGCode::MultiAsSingle
        //BBS: replace model custom gcode with current plate custom gcode
        print.model().get_curr_plate_custom_gcodes().mode == CustomGCode::MultiAsSingle) {
		// Printing a single extruder platter on a printer with more than 1 extruder (or single-extruder multi-material).
		// There may be custom per-layer tool changes available at the model.
        const size_t num_filaments = (m_mixed_mgr == nullptr) ? num_physical : m_mixed_mgr->total_filaments(num_physical);
        per_layer_extruder_switches = custom_tool_changes(print.model().get_curr_plate_custom_gcodes(), num_filaments);
	}

    // Collect extruders reuqired to print the layers.
    for (auto object : print.objects())
        this->collect_extruders(*object, per_layer_extruder_switches);

    // Reorder the extruders to minimize tool switches.
    std::vector<unsigned int> first_layer_tool_order;
    if (first_extruder == (unsigned int)-1) {
        first_layer_tool_order = generate_first_layer_tool_order(print);
    }

    if (!first_layer_tool_order.empty()) {
        this->reorder_extruders(first_layer_tool_order);
    }
    else {
        this->reorder_extruders(first_extruder);
    }

    this->fill_wipe_tower_partitions(print.config(), object_bottom_z, max_layer_height);

    /*if (prime_multi_material) {
        std::map<unsigned int, int> extrudeCount;
        for (const LayerTools& lt : m_layer_tools) {
            for (unsigned int currentExtruder : lt.extruders) {
                extrudeCount[currentExtruder]++;
            }
        }

        unsigned int maxExtrude = -1;
        int maxCount = 0;
        for (auto& itPair : extrudeCount) {
            if (itPair.second > maxCount && !m_print_config_ptr->filament_soluble.get_at(itPair.first)) {
                maxCount = itPair.second;
                maxExtrude = itPair.first;
            }
        }
        const_cast<PrintConfig*>(m_print_config_ptr)->wipe_tower_filament.setInt(maxExtrude + 1);
    }*/

    if (this->insert_wipe_tower_extruder()) {
        // Now convert the 0-based list to 1-based again, because that is what reorder_extruder expects.
        for (LayerTools& lt : m_layer_tools) {
            for (auto& extruder : lt.extruders)
                ++extruder;
        }
        this->reorder_extruders(first_extruder);
        this->fill_wipe_tower_partitions(print.config(), object_bottom_z, max_layer_height);
    }

    // Chameleon brim: the union of foreign-extruder brim partitions into the first
    // layer's LayerTools.extruders used to happen here (ctor-time). It was moved to
    // Print::process() (psSkirtBrim, right after the partition pass fills
    // m_brimMapByExtruder) because this constructor runs during psWipeTower, which
    // executes BEFORE psSkirtBrim populates m_brimMapByExtruder - so a ctor-time hook
    // always observed an empty map on a fresh slice. See Print.cpp for the replacement.

    // H2 preload (preload_all_filaments): add every remaining plate filament to layer 1 so the prime
    // tower loads them all before layer 2. Must run after the reorder passes (the grouping used to
    // order the pre-changes is computed there) and before collect_extruder_statistics, which derives
    // m_first_printing_extruder - the filament machine_start_gcode loads - from layer 1's front.
    if (this->apply_preload_all_filaments())
        this->fill_wipe_tower_partitions(print.config(), object_bottom_z, max_layer_height);

    this->collect_extruder_statistics(prime_multi_material);

    this->mark_skirt_layers(print.config(), max_layer_height);
}

void ToolOrdering::update_mixed_layer_height_settings()
{
    const PrintConfig *cfg = m_print_config_ptr;
    if (cfg == nullptr && m_print_object_ptr != nullptr)
        cfg = &m_print_object_ptr->print()->config();

    m_mixed_layer_height_a = 0.f;
    m_mixed_layer_height_b = 0.f;
    if (m_print_full_config != nullptr &&
        m_print_full_config->has("mixed_color_layer_height_a") &&
        m_print_full_config->has("mixed_color_layer_height_b")) {
        m_mixed_layer_height_a = float(m_print_full_config->opt_float("mixed_color_layer_height_a"));
        m_mixed_layer_height_b = float(m_print_full_config->opt_float("mixed_color_layer_height_b"));
    } else if (cfg != nullptr) {
        m_mixed_layer_height_a = cfg->mixed_color_layer_height_a.value;
        m_mixed_layer_height_b = cfg->mixed_color_layer_height_b.value;
    }

    float base_height = 0.2f;
    if (m_print_object_ptr != nullptr)
        base_height = float(m_print_object_ptr->config().layer_height.value);
    else if (m_print_full_config != nullptr && m_print_full_config->has("layer_height"))
        base_height = float(m_print_full_config->opt_float("layer_height"));

    m_mixed_base_layer_height = std::max<float>(0.01f, base_height);
}

static void apply_first_layer_order(const DynamicPrintConfig* config, std::vector<unsigned int>& tool_order) {
    const ConfigOptionInts* first_layer_print_sequence_op = config->option<ConfigOptionInts>("first_layer_print_sequence");
    if (first_layer_print_sequence_op) {
        const std::vector<int>& print_sequence_1st = first_layer_print_sequence_op->values;
        if (print_sequence_1st.size() >= tool_order.size()) {
            std::sort(tool_order.begin(), tool_order.end(), [&print_sequence_1st](int lh, int rh) {
                auto lh_it = std::find(print_sequence_1st.begin(), print_sequence_1st.end(), lh);
                auto rh_it = std::find(print_sequence_1st.begin(), print_sequence_1st.end(), rh);

                if (lh_it == print_sequence_1st.end() || rh_it == print_sequence_1st.end())
                    return false;

                return lh_it < rh_it;
            });
        }
    }
}

// BBS
std::vector<unsigned int> ToolOrdering::generate_first_layer_tool_order(const Print& print)
{
    std::vector<unsigned int> tool_order;
    int initial_extruder_id = -1;
    std::map<int, double> min_areas_per_extruder;

    for (auto object : print.objects()) {
        auto first_layer = object->get_layer(0);
        for (auto layerm : first_layer->regions()) {
            int extruder_id = layerm->region().config().option("wall_filament")->getInt();
            
            for (auto expoly : layerm->raw_slices) {
                const double nozzle_diameter = print.config().nozzle_diameter.get_at(0);
                const coordf_t initial_layer_line_width = print.config().get_abs_value("initial_layer_line_width", nozzle_diameter);

                if (offset_ex(expoly, -0.2 * scale_(initial_layer_line_width)).empty())
                    continue;

                double contour_area = expoly.contour.area();
                auto iter = min_areas_per_extruder.find(extruder_id);
                if (iter == min_areas_per_extruder.end()) {
                    min_areas_per_extruder.insert({ extruder_id, contour_area });
                }
                else {
                    if (contour_area < min_areas_per_extruder.at(extruder_id)) {
                        min_areas_per_extruder[extruder_id] = contour_area;
                    }
                }
            }
        }
    }

    double max_minimal_area = 0.;
    for (auto ape : min_areas_per_extruder) {
        auto iter = tool_order.begin();
        for (; iter != tool_order.end(); iter++) {
            if (min_areas_per_extruder.at(*iter) < min_areas_per_extruder.at(ape.first))
                break;
        }

        tool_order.insert(iter, ape.first);
    }

    apply_first_layer_order(m_print_full_config, tool_order);

    return tool_order;
}

std::vector<unsigned int> ToolOrdering::generate_first_layer_tool_order(const PrintObject& object)
{
    std::vector<unsigned int> tool_order;
    int initial_extruder_id = -1;
    std::map<int, double> min_areas_per_extruder;
    auto first_layer = object.get_layer(0);
    for (auto layerm : first_layer->regions()) {
        int extruder_id = layerm->region().config().option("wall_filament")->getInt();
        for (auto expoly : layerm->raw_slices) {
            const double nozzle_diameter = object.print()->config().nozzle_diameter.get_at(0);
            const coordf_t line_width = object.config().get_abs_value("line_width", nozzle_diameter);

            if (offset_ex(expoly, -0.2 * scale_(line_width)).empty())
                continue;

            double contour_area = expoly.contour.area();
            auto iter = min_areas_per_extruder.find(extruder_id);
            if (iter == min_areas_per_extruder.end()) {
                min_areas_per_extruder.insert({ extruder_id, contour_area });
            }
            else {
                if (contour_area < min_areas_per_extruder.at(extruder_id)) {
                    min_areas_per_extruder[extruder_id] = contour_area;
                }
            }
        }
    }

    double max_minimal_area = 0.;
    for (auto ape : min_areas_per_extruder) {
        auto iter = tool_order.begin();
        for (; iter != tool_order.end(); iter++) {
            if (min_areas_per_extruder.at(*iter) < min_areas_per_extruder.at(ape.first))
                break;
        }

        tool_order.insert(iter, ape.first);
    }

    apply_first_layer_order(m_print_full_config, tool_order);

    return tool_order;
}

void ToolOrdering::initialize_layers(std::vector<coordf_t> &zs)
{
    sort_remove_duplicates(zs);
    // Merge numerically very close Z values.
    for (size_t i = 0; i < zs.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = zs[i] + EPSILON;
        for (; j < zs.size() && zs[j] <= zmax; ++ j) ;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        m_layer_tools.emplace_back(LayerTools(0.5 * (zs[i] + zs[j-1])));
        i = j;
    }
}

// Collect extruders reuqired to print layers.
void ToolOrdering::collect_extruders(const PrintObject &object, const std::vector<std::pair<double, unsigned int>> &per_layer_extruder_switches)
{
    for (LayerTools &layer_tools : m_layer_tools) {
        layer_tools.mixed_mgr                = m_mixed_mgr;
        layer_tools.num_physical             = m_num_physical;
        layer_tools.mixed_layer_height_a     = m_mixed_layer_height_a;
        layer_tools.mixed_layer_height_b     = m_mixed_layer_height_b;
        layer_tools.mixed_base_layer_height  = m_mixed_base_layer_height;
        // Set per-object context for the duration of this collect_extruders call.
        // Reset after the loops below so unrelated callers see nullptr.
        layer_tools.current_object           = &object;
    }

    // Collect the support extruders. Stamp layer_index only at this object's
    // support print_z (inherited from the last object layer at or below it).
    // ByLayer shares LayerTools across objects: writing every m_layer_tools
    // entry let a later shorter object overwrite a taller object's indices
    // above the short top, so mixed walls/infill resolved to unscheduled tools.
    const auto &object_layers = object.layers();
    auto inherited_layer_index = [&object_layers](coordf_t print_z) {
        int inherited = 0;
        for (size_t i = 0; i < object_layers.size(); ++i) {
            if (object_layers[i]->print_z <= print_z + EPSILON)
                inherited = int(i);
            else
                break;
        }
        return inherited;
    };

    for (auto support_layer : object.support_layers()) {
        LayerTools   &layer_tools = this->tools_for_layer(support_layer->print_z);
        layer_tools.layer_index        = inherited_layer_index(support_layer->print_z);
        layer_tools.object_layer_count = int(object_layers.size());
        layer_tools.layer_height = support_layer->height;
        ExtrusionRole role = support_layer->support_fills.role();
        bool         has_support        = role == erMixed || role == erSupportMaterial || role == erSupportTransition;
        // v2.2 Task 3 (spec C6, third anchor): support_role_needs_interface_extruder
        // (BrimFilament.hpp/.cpp) adds erIroning to this classification - a layer whose
        // support_fills collapsed to PURE erIroning (every erSupportMaterial/
        // erSupportMaterialInterface entity matched away by C6's third
        // partition_support_entities call and/or C7's whole-collection moves, residual
        // fallback ironing left behind) must still register the interface extruder here
        // or that ironing's toolchange never gets scheduled - see the shared predicate's
        // own header comment for the full investigation finding.
        bool         has_interface      = support_role_needs_interface_extruder(role);
        unsigned int extruder_support   = resolve_mixed(object.config().support_filament.value,
                                                        layer_tools.layer_index,
                                                        float(support_layer->print_z),
                                                        float(support_layer->height),
                                                        &object);
        unsigned int extruder_interface = resolve_mixed(object.config().support_interface_filament.value,
                                                        layer_tools.layer_index,
                                                        float(support_layer->print_z),
                                                        float(support_layer->height),
                                                        &object);
        // Support filament matching: the filament the pass chose for this layer's leftover support
        // (SupportLayer::chameleon_residual_extruder, -1 everywhere else) replaces "don't care" -
        // and, on a plate layer without a raft, a configured filament too - so the tool it prints
        // with is scheduled here. Mirrors the pin in GCode::process_layer exactly.
        if (support_layer->chameleon_residual_extruder >= 0) {
            const unsigned int pinned = unsigned(support_layer->chameleon_residual_extruder) + 1;
            if (object.config().support_filament.value == 0 || support_layer->chameleon_residual_all_roles)
                extruder_support = pinned;
            if (object.config().support_interface_filament.value == 0 || support_layer->chameleon_residual_all_roles)
                extruder_interface = pinned;
        }
        if (has_support)
            layer_tools.extruders.push_back(extruder_support);
        if (has_interface)
            layer_tools.extruders.push_back(extruder_interface);
        if (has_support || has_interface) {
            layer_tools.has_support = true;
            layer_tools.wiping_extrusions().is_support_overriddable_and_mark(role, object);
        }

        // Chameleon P2: register per-layer matched interface extruders (map keys are
        // 0-based; this collection phase is 1-based until reorder_extruders reindexes).
        for (const auto& kv : support_layer->interface_by_extruder)
            if (!kv.second.entities.empty())
                layer_tools.extruders.push_back(kv.first + 1);
        if (!support_layer->interface_by_extruder.empty())
            layer_tools.has_support = true;
    }

    // Extruder overrides are ordered by print_z.
    std::vector<std::pair<double, unsigned int>>::const_iterator it_per_layer_extruder_override;
	it_per_layer_extruder_override = per_layer_extruder_switches.begin();
    unsigned int extruder_override = 0;

    // BBS: collect first layer extruders of an object's wall, which will be used by brim generator
    int layerCount = 0;
    std::vector<int> firstLayerExtruders;
    firstLayerExtruders.clear();

    // Collect the object extruders.
    for (auto layer : object.layers()) {
        LayerTools &layer_tools = this->tools_for_layer(layer->print_z);
        // Store the sequential layer index and mixed-filament context for resolution.
        layer_tools.layer_index       = layerCount;
        layer_tools.object_layer_count = int(object.layers().size());
        layer_tools.layer_height      = layer->height;

        // Override extruder with the next 
    	for (; it_per_layer_extruder_override != per_layer_extruder_switches.end() && it_per_layer_extruder_override->first < layer->print_z + EPSILON; ++ it_per_layer_extruder_override)
    		extruder_override = (int)it_per_layer_extruder_override->second;

        // Store the current extruder override (set to zero if no overriden), so that layer_tools.wiping_extrusions().is_overridable_and_mark() will use it.
        layer_tools.extruder_override = extruder_override;

        // What extruders are required to print this object layer?
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegion &region = layerm->region();

            if (! layerm->perimeters.entities.empty()) {
                bool something_nonoverriddable = true;

                if (m_print_config_ptr) { // in this case print->config().print_sequence != PrintSequence::ByObject (see ToolOrdering constructors)
                    something_nonoverriddable = false;
                    for (const auto& eec : layerm->perimeters.entities) // let's check if there are nonoverriddable entities
                        if (!layer_tools.wiping_extrusions().is_overriddable_and_mark(dynamic_cast<const ExtrusionEntityCollection&>(*eec), *m_print_config_ptr, object, region))
                            something_nonoverriddable = true;
                }

                if (something_nonoverriddable){
                    const unsigned int configured_wall = (extruder_override == 0) ? region.config().wall_filament.value : extruder_override;
                    unsigned int       wall_ext        = resolve_mixed(configured_wall, layerCount, float(layer->print_z), float(layer->height), &object);
                    const unsigned int grouped_id =
                        grouped_manual_pattern_mixed_filament_id_for_layer(layer_tools, configured_wall);
                    if (grouped_id != 0) {
                        const std::vector<unsigned int> ordered =
                            m_mixed_mgr->ordered_perimeter_extruders(grouped_id,
                                                                     m_num_physical,
                                                                     layerCount,
                                                                     float(layer->print_z),
                                                                     float(layer->height));
                        if (!ordered.empty()) {
                            if (ordered.size() >= 2)
                                layer_tools.preserve_extruder_order = true;
                            for (unsigned int extruder_id : ordered) {
                                layer_tools.extruders.emplace_back(extruder_id);
                                if (layerCount == 0 &&
                                    std::find(firstLayerExtruders.begin(), firstLayerExtruders.end(), int(extruder_id)) == firstLayerExtruders.end())
                                    firstLayerExtruders.emplace_back(int(extruder_id));
                            }
                        } else {
                            layer_tools.extruders.emplace_back(wall_ext);
                            if (layerCount == 0)
                                firstLayerExtruders.emplace_back(wall_ext);
                        }
                    } else {
                        layer_tools.extruders.emplace_back(wall_ext);
                        if (layerCount == 0)
                            firstLayerExtruders.emplace_back(wall_ext);
                    }

                    // Phase 4 (image row on WALLS): when this region's wall filament names an
                    // enabled ImageWeighted row, GCode.cpp splits the outer perimeter into
                    // per-run collections, each tagged with ONE of the row's own candidate
                    // filaments. That split happens at G-CODE time, which is after this function
                    // runs - so unlike the fills loop below (which can read the tag off a
                    // collection the slicer already produced), this branch cannot see the runs.
                    // It therefore registers the row's whole CANDIDATE SET from the config, which
                    // is the set the runs are guaranteed to be drawn from. Registering the set is
                    // what makes GCode.cpp's own `layer_tools.has_extruder(correct_extruder_id)`
                    // check find each run's filament in the layer's tool list; without it a run
                    // would be silently reassigned to layer_tools.extruders.back(). Registering a
                    // filament that no run on THIS layer ends up using costs nothing: the list is
                    // deduplicated and reordered for minimum switches by reorder_extruders(), and
                    // an extruder with no extrusions emits no tool change.
                    if (extruder_override == 0 && m_mixed_mgr != nullptr && m_num_physical > 0) {
                        for (unsigned int cand : image_row_wall_candidate_filaments(object, region, m_num_physical)) {
                            layer_tools.extruders.emplace_back(cand);
                            if (layerCount == 0 &&
                                std::find(firstLayerExtruders.begin(), firstLayerExtruders.end(), int(cand)) == firstLayerExtruders.end())
                                firstLayerExtruders.emplace_back(int(cand));
                        }
                    }

                    // Ultra: outer walls may use their own filament
                    if (extruder_override == 0 && region.config().outer_wall_filament.value > 0) {
                        const unsigned int outer_ext = resolve_mixed(unsigned(region.config().outer_wall_filament.value),
                                                                     layerCount, float(layer->print_z), float(layer->height), &object);
                        layer_tools.extruders.emplace_back(outer_ext);
                        if (layerCount == 0)
                            firstLayerExtruders.emplace_back(outer_ext);
                    }
                }

                layer_tools.has_object = true;
            }

            bool has_sparse_infill = false;
            bool has_solid_infill  = false;
            bool something_nonoverriddable = false;
            for (const ExtrusionEntity *ee : layerm->fills.entities) {
                // fill represents infill extrusions of a single island.
                const auto *fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                ExtrusionRole role = fill->entities.empty() ? erNone : fill->entities.front()->role();

                // Phase 3 (image row): Fill.cpp's split_top_infill_by_image_row() already tagged
                // this collection with the ONE physical filament every entity inside it prints
                // with (image_row_extruder_1based; 0 for every collection in every print that
                // does not use the feature). Register that id directly and skip the
                // has_solid_infill/has_sparse_infill bucket below for this collection: that
                // bucket would otherwise register the ROW's un-split resolve() fallback (a
                // gradient-cycle single id, see MixedFilamentManager::resolve()) instead of - or
                // in addition to - the id this collection actually prints with, and could miss a
                // physical id the fallback's own per-layer cycle never visits on this layer.
                if (fill->image_row_extruder_1based != 0) {
                    layer_tools.extruders.emplace_back(resolve_mixed(fill->image_row_extruder_1based,
                                                                      layerCount, float(layer->print_z),
                                                                      float(layer->height), &object));
                    layer_tools.has_object = true;
                    continue;
                }

                // Wave A fix-wave / I-3 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md):
                // bucket through the SAME fill_filament_source resolution GCode.cpp's emission
                // uses (single source of truth), instead of a second, independently-drifted copy
                // of the same role rule. Gap fill now emits with wall_filament (PrintRegion.cpp),
                // and the perimeters loop above already registers wall_ext for this region - so a
                // gap-fill-only collection must contribute NOTHING here, not sparse_infill_filament,
                // or the layer's computed extruder set disagrees with what is actually emitted
                // (a spurious tool change / wipe-tower purge for an extruder nothing prints with).
                if (role != erNone) {
                    switch (fill_filament_source(region.config(), role)) {
                    case FillFilamentSource::Wall:        break; // covered by the perimeters loop above
                    case FillFilamentSource::SolidInfill: has_solid_infill  = true; break;
                    case FillFilamentSource::SparseInfill:
                    default:                              has_sparse_infill = true; break;
                    }
                }

                if (m_print_config_ptr) {
                    if (! layer_tools.wiping_extrusions().is_overriddable_and_mark(*fill, *m_print_config_ptr, object, region))
                        something_nonoverriddable = true;
                }
            }

            if (something_nonoverriddable || !m_print_config_ptr) {
            	if (extruder_override == 0) {
	                if (has_solid_infill) {
		                    layer_tools.extruders.emplace_back(layer_tools.solid_infill_filament(region) + 1);
	                }
	                if (has_sparse_infill) {
		                    layer_tools.extruders.emplace_back(layer_tools.sparse_infill_filament(region) + 1);
	                }
            	} else if (has_solid_infill || has_sparse_infill) {
            		layer_tools.extruders.emplace_back(resolve_mixed(extruder_override,
                                                                      layerCount,
                                                                      float(layer->print_z),
                                                                      float(layer->height),
                                                                      &object));
            	}
            }
            if (has_solid_infill || has_sparse_infill)
                layer_tools.has_object = true;
        }
        layerCount++;
    }

    sort_remove_duplicates(firstLayerExtruders);
    const_cast<PrintObject&>(object).object_first_layer_wall_extruders = firstLayerExtruders;

    for (auto& layer : m_layer_tools) {
        if (layer.preserve_extruder_order)
            remove_duplicates_preserve_order(layer.extruders);
        else
            sort_remove_duplicates(layer.extruders);

        // make sure that there are some tools for each object layer (e.g. tall wiping object will result in empty extruders vector)
        if (layer.extruders.empty() && layer.has_object)
            layer.extruders.emplace_back(0); // 0="dontcare" extruder - it will be taken care of in reorder_extruders

        // Reset per-object context now that this object's collection is done.
        layer.current_object = nullptr;
    }
}

// Reorder extruders to minimize layer changes.
void ToolOrdering::reorder_extruders(unsigned int last_extruder_id)
{
    if (m_layer_tools.empty())
        return;

    if (last_extruder_id == (unsigned int)-1) {
        // The initial print extruder has not been decided yet.
        // Initialize the last_extruder_id with the first non-zero extruder id used for the print.
        last_extruder_id = 0;
        for (size_t i = 0; i < m_layer_tools.size() && last_extruder_id == 0; ++ i) {
            const LayerTools &lt = m_layer_tools[i];
            for (unsigned int extruder_id : lt.extruders)
                if (extruder_id > 0) {
                    last_extruder_id = extruder_id;
                    break;
                }
        }
        if (last_extruder_id == 0)
            // Nothing to extrude.
            return;
    } else
        // 1 based index
        ++ last_extruder_id;

    for (LayerTools &lt : m_layer_tools) {
        if (lt.extruders.empty())
            continue;
        if (lt.extruders.size() == 1 && lt.extruders.front() == 0)
            lt.extruders.front() = last_extruder_id;
        else {
            if (lt.extruders.front() == 0)
                // Pop the "don't care" extruder, the "don't care" region will be merged with the next one.
                lt.extruders.erase(lt.extruders.begin());
            if (lt.preserve_extruder_order) {
                last_extruder_id = lt.extruders.back();
                continue;
            }
            // Default: start with the last used extruder to minimize tool changes. Cyclic skips this
            // so each layer keeps a fixed sequence (applied later in get_custom_seq).
            if (use_default_toolchange_ordering(toolordering_print_config(m_print_config_ptr, m_print_object_ptr)))
                rotate_last_extruder_to_front(lt.extruders, last_extruder_id);

            if (lt == m_layer_tools[0]) {
                // On first layer with wipe tower, prefer a soluble extruder
                // at the beginning, so it is not wiped on the first layer.
                if (m_print_config_ptr && m_print_config_ptr->enable_prime_tower) {
                    for (size_t i = 0; i<lt.extruders.size(); ++i)
                        if (m_print_config_ptr->filament_soluble.get_at(lt.extruders[i]-1)) { // 1-based...
                            std::swap(lt.extruders[i], lt.extruders.front());
                            break;
                        }
                }

                // Then, if we specified the tool order, apply it now
                apply_first_layer_order(m_print_full_config, lt.extruders);
            }
        }
        last_extruder_id = lt.extruders.back();
    }

    // Reindex the extruders, so they are zero based, not 1 based.
    for (LayerTools &lt : m_layer_tools)
        for (unsigned int &extruder_id : lt.extruders) {
            assert(extruder_id > 0);
            -- extruder_id;
        }

    // reorder the extruders for minimum flush volume
    reorder_extruders_for_minimum_flush_volume();
}

// BBS
void ToolOrdering::reorder_extruders(std::vector<unsigned int> tool_order_layer0)
{
    if (m_layer_tools.empty())
        return;

    if (tool_order_layer0.empty())
        return;

    // Reorder the extruders of first layer
    {
        LayerTools& lt = m_layer_tools[0];
        if (!lt.preserve_extruder_order) {
            std::vector<unsigned int> layer0_extruders = lt.extruders;
            lt.extruders.clear();
            for (unsigned int extruder_id : tool_order_layer0) {
                auto iter = std::find(layer0_extruders.begin(), layer0_extruders.end(), extruder_id);
                if (iter != layer0_extruders.end()) {
                    lt.extruders.push_back(extruder_id);
                    *iter = (unsigned int)-1;
                }
            }

            for (unsigned int extruder_id : layer0_extruders) {
                if (extruder_id == 0)
                    continue;

                if (extruder_id != (unsigned int)-1)
                    lt.extruders.push_back(extruder_id);
            }

            // all extruders are zero
            if (lt.extruders.empty()) {
                lt.extruders.push_back(tool_order_layer0[0]);
            }
        }
    }

    int last_extruder_id = m_layer_tools[0].extruders.back();
    for (int i = 1; i < m_layer_tools.size(); i++) {
        LayerTools& lt = m_layer_tools[i];

        if (lt.extruders.empty())
            continue;
        if (lt.extruders.size() == 1 && lt.extruders.front() == 0)
            lt.extruders.front() = last_extruder_id;
        else {
            if (lt.extruders.front() == 0)
                // Pop the "don't care" extruder, the "don't care" region will be merged with the next one.
                lt.extruders.erase(lt.extruders.begin());
            if (lt.preserve_extruder_order) {
                last_extruder_id = lt.extruders.back();
                continue;
            }
            // Default: start with the last used extruder to minimize tool changes. Cyclic skips this
            // so each layer keeps a fixed sequence (applied later in get_custom_seq).
            if (use_default_toolchange_ordering(toolordering_print_config(m_print_config_ptr, m_print_object_ptr)))
                rotate_last_extruder_to_front(lt.extruders, last_extruder_id);
        }
        last_extruder_id = lt.extruders.back();
    }

    // Reindex the extruders, so they are zero based, not 1 based.
    for (LayerTools& lt : m_layer_tools)
        for (unsigned int& extruder_id : lt.extruders) {
            assert(extruder_id > 0);
            --extruder_id;
        }

    // reorder the extruders for minimum flush volume
    reorder_extruders_for_minimum_flush_volume();
}

void ToolOrdering::fill_wipe_tower_partitions(const PrintConfig &config, coordf_t object_bottom_z, coordf_t max_layer_height)
{
    if (m_layer_tools.empty())
        return;

    // Count the minimum number of tool changes per layer.
    size_t last_extruder = size_t(-1);
    for (LayerTools &lt : m_layer_tools) {
        lt.wipe_tower_partitions = lt.extruders.size();
        if (! lt.extruders.empty()) {
            if (last_extruder == size_t(-1) || last_extruder == lt.extruders.front())
                // The first extruder on this layer is equal to the current one, no need to do an initial tool change.
                -- lt.wipe_tower_partitions;
            last_extruder = lt.extruders.back();
        }
    }

    // Propagate the wipe tower partitions down to support the upper partitions by the lower partitions.
    for (int i = int(m_layer_tools.size()) - 2; i >= 0; -- i)
        m_layer_tools[i].wipe_tower_partitions = std::max(m_layer_tools[i + 1].wipe_tower_partitions, m_layer_tools[i].wipe_tower_partitions);

    //FIXME this is a hack to get the ball rolling.
    for (LayerTools &lt : m_layer_tools)
        lt.has_wipe_tower = (lt.has_object && (config.timelapse_type == TimelapseType::tlSmooth || lt.wipe_tower_partitions > 0))
            || lt.print_z < object_bottom_z + EPSILON;

    // Test for a raft, insert additional wipe tower layer to fill in the raft separation gap.
    for (size_t i = 0; i + 1 < m_layer_tools.size(); ++ i) {
        const LayerTools &lt      = m_layer_tools[i];
        const LayerTools &lt_next = m_layer_tools[i + 1];
        if (lt.print_z < object_bottom_z + EPSILON && lt_next.print_z >= object_bottom_z + EPSILON) {
            // lt is the last raft layer. Find the 1st object layer.
            size_t j = i + 1;
            for (; j < m_layer_tools.size() && ! m_layer_tools[j].has_wipe_tower; ++ j);
            if (j < m_layer_tools.size()) {
                const LayerTools &lt_object = m_layer_tools[j];
                coordf_t gap = lt_object.print_z - lt.print_z;
                assert(gap > 0.f);
                if (gap > max_layer_height + EPSILON) {
                    // Insert one additional wipe tower layer between lh.print_z and lt_object.print_z.
                    LayerTools lt_new(0.5f * (lt.print_z + lt_object.print_z));
                    // Find the 1st layer above lt_new.
                    for (j = i + 1; j < m_layer_tools.size() && m_layer_tools[j].print_z < lt_new.print_z - EPSILON; ++ j);
                    if (j < m_layer_tools.size() && std::abs(m_layer_tools[j].print_z - lt_new.print_z) < EPSILON) {
						m_layer_tools[j].has_wipe_tower = true;
					} else if (j < m_layer_tools.size() && ! m_layer_tools[j].extruders.empty()) {
                        // The layer right above the inserted one may carry no extruders, e.g. when
                        // support generation was toggled off after a slice that had it enabled: the
                        // layer plan for the raft gap then contains no extrusions for some layers.
                        // lt_next.extruders.front() would dereference a null begin() and crash.
                        LayerTools &lt_extra = *m_layer_tools.insert(m_layer_tools.begin() + j, lt_new);
                        //LayerTools &lt_prev  = m_layer_tools[j];
                        LayerTools &lt_next  = m_layer_tools[j + 1];
                        assert(! m_layer_tools[j - 1].extruders.empty() && ! lt_next.extruders.empty());
                        // FIXME: Following assert tripped when running combine_infill.t. I decided to comment it out for now.
                        // If it is a bug, it's likely not critical, because this code is unchanged for a long time. It might
                        // still be worth looking into it more and decide if it is a bug or an obsolete assert.
                        //assert(lt_prev.extruders.back() == lt_next.extruders.front());
                        lt_extra.has_wipe_tower = true;
                        lt_extra.extruders.push_back(lt_next.extruders.front());
                        lt_extra.wipe_tower_partitions = lt_next.wipe_tower_partitions;
                    }
                }
            }
            break;
        }
    }

    // If the model contains empty layers (such as https://github.com/prusa3d/Slic3r/issues/1266), there might be layers
    // that were not marked as has_wipe_tower, even when they should have been. This produces a crash with soluble supports
    // and maybe other problems. We will therefore go through layer_tools and detect and fix this.
    // So, if there is a non-object layer starting with different extruder than the last one ended with (or containing more than one extruder),
    // we'll mark it with has_wipe tower.
    for (unsigned int i=0; i+1<m_layer_tools.size(); ++i) {
        LayerTools& lt = m_layer_tools[i];
        LayerTools& lt_next = m_layer_tools[i+1];
        if (lt.extruders.empty() || lt_next.extruders.empty())
            break;
        if (!lt_next.has_wipe_tower && (lt_next.extruders.front() != lt.extruders.back() || lt_next.extruders.size() > 1))
            lt_next.has_wipe_tower = true;
        // We should also check that the next wipe tower layer is no further than max_layer_height:
        unsigned int j = i+1;
        double last_wipe_tower_print_z = lt_next.print_z;
        while (++j < m_layer_tools.size()-1 && !m_layer_tools[j].has_wipe_tower)
            if (m_layer_tools[j+1].print_z - last_wipe_tower_print_z > max_layer_height + EPSILON) {
                m_layer_tools[j].has_wipe_tower = true;
                last_wipe_tower_print_z = m_layer_tools[j].print_z;
            }
    }

    // Calculate the wipe_tower_layer_height values.
    coordf_t wipe_tower_print_z_last = 0.;
    for (LayerTools &lt : m_layer_tools)
        if (lt.has_wipe_tower) {
            lt.wipe_tower_layer_height = lt.print_z - wipe_tower_print_z_last;
            wipe_tower_print_z_last = lt.print_z;
        }
}

// H2 preload (preload_all_filaments): the layer-1 ordering rule. See ToolOrdering.hpp for the
// contract and apply_preload_all_filaments() below for why this shape keeps the M620 O<n> /
// filament_sequence.json / optimal_assignment bookkeeping consistent by construction.
std::vector<unsigned int> preload_first_layer_tool_order(const std::vector<unsigned int> &layer0_extruders,
                                                         const std::vector<unsigned int> &used_filaments,
                                                         const std::function<int(unsigned int)> &extruder_of_filament)
{
    if (layer0_extruders.empty())
        return layer0_extruders;

    std::vector<unsigned int> missing;
    for (unsigned int e : used_filaments)
        if (std::find(layer0_extruders.begin(), layer0_extruders.end(), e) == layer0_extruders.end())
            missing.emplace_back(e);
    sort_remove_duplicates(missing);
    if (missing.empty())
        return layer0_extruders;   // layer 1 already touches every filament: nothing to preload.

    auto extruder_of = [&extruder_of_filament](unsigned int filament) -> int {
        if (!extruder_of_filament)
            return 0;
        const int e = extruder_of_filament(filament);
        return e < 0 ? 0 : e;
    };
    const int first_real_extruder = extruder_of(layer0_extruders.front());
    std::stable_sort(missing.begin(), missing.end(), [&](unsigned int a, unsigned int b) {
        const int ea = extruder_of(a), eb = extruder_of(b);
        if (ea != eb) {
            // The group layer 1 starts on goes last; the rest keep ascending extruder order.
            const bool a_last = (ea == first_real_extruder), b_last = (eb == first_real_extruder);
            if (a_last != b_last)
                return b_last;
            return ea < eb;
        }
        return a < b;
    });

    std::vector<unsigned int> out = std::move(missing);
    out.insert(out.end(), layer0_extruders.begin(), layer0_extruders.end());
    return out;
}

// H2 preload (preload_all_filaments). Owner's design: "trick it into auto-loading by including all
// colors on the first layer of the prime tower". Every filament the plate uses that layer 1 does not
// already print is PREPENDED to layer 1's tool sequence, so Print::_make_wipe_tower plans a real
// toolchange for each one (normal flush volume from flush_volumes_matrix * flush_multiplier) and
// GCode::WipeTowerIntegration::append_tcr emits the normal change_filament_gcode - which is why the
// M620 O<n> counter, filament_sequence.json / nozzle_sequence, optimal_assignment and the H2C rack
// interlock blocks all stay consistent by construction: nothing here is special-cased, these are
// ordinary tower toolchanges that happen to sit on layer 1.
//
// ORDERING RULE (layer 1):
//   [ preload-only filaments ] ++ [ layer 1's own sequence, unchanged ]
// The preload-only filaments are grouped by the physical extruder the dual-nozzle grouping assigned
// them to (so each extruder's loads are consecutive - a nozzle/rack change is never paid twice for
// the same extruder), the group belonging to layer 1's FIRST real filament comes last among the
// preload groups (so the run into the real print costs no extra extruder change), and within a group
// filaments are ascending by id (deterministic). Layer 1's own order - first_layer_print_sequence,
// the soluble-first preference, the min-area adhesion order - is never touched, so the sequence still
// ENDS on the filament layer 1 actually finishes printing with.
//
// Runs after reorder_extruders()/reorder_extruders_for_minimum_flush_volume(), so the filament->nozzle
// grouping is computed from the UNMODIFIED layer list: turning the option on cannot change the
// grouping, only what layer 1 does with it. The caller re-runs fill_wipe_tower_partitions() afterwards
// so the tower's layer-1 depth (WipeTower::plan_tower takes max over per-layer toolchanges_depth())
// accounts for the extra changes.
bool ToolOrdering::apply_preload_all_filaments()
{
    if (m_layer_tools.empty() || m_print_config_ptr == nullptr || m_print_full_config == nullptr)
        return false;
    if (!m_print_config_ptr->preload_all_filaments.value)
        return false;
    // The pre-changes purge into the prime tower; without it there is nowhere to put them.
    // Print::validate() refuses this combination up front, this is the belt-and-braces half.
    if (!m_print_config_ptr->enable_prime_tower.value)
        return false;
    // Same gate as the dual-nozzle grouping path: H2D/H2C/X2D only. Classic single-nozzle and
    // same-variant toolchanger machines (P1S/X1/U1) never enter here, so their g-code is untouched.
    {
        int extruder_count = 0;
        if (!const_cast<DynamicPrintConfig *>(m_print_full_config)->support_different_extruders(extruder_count))
            return false;
    }

    // Layer 1 == the first layer that actually prints something.
    size_t first_idx = size_t(-1);
    for (size_t i = 0; i < m_layer_tools.size(); ++i)
        if (!m_layer_tools[i].extruders.empty()) { first_idx = i; break; }
    if (first_idx == size_t(-1))
        return false;
    LayerTools &lt0 = m_layer_tools[first_idx];
    if (lt0.preserve_extruder_order)
        return false;

    // Every filament the plate uses (0-based).
    std::vector<unsigned int> used;
    for (const LayerTools &lt : m_layer_tools)
        append(used, lt.extruders);
    sort_remove_duplicates(used);

    // Group the preload-only filaments by the physical extruder the dual-nozzle grouping put them on,
    // so each extruder's loads stay together. Layer 0 is the right layer to ask: these changes happen
    // there, and append_tcr resolves the rack/nozzle ids for the same layer index.
    auto group = m_print ? m_print->get_layered_nozzle_group_result() : nullptr;
    std::vector<unsigned int> reordered = preload_first_layer_tool_order(
        lt0.extruders, used, [&group](unsigned int filament) -> int {
            return group ? group->get_extruder_id((int) filament, 0) : 0;
        });
    if (reordered.size() == lt0.extruders.size())
        return false;   // layer 1 already touches every filament: nothing to preload.
    lt0.extruders = std::move(reordered);

    {
        std::string seq;
        for (unsigned int e : lt0.extruders) seq += std::to_string(e) + " ";
        BOOST_LOG_TRIVIAL(warning) << "[H2 preload] first layer tool sequence (0-based filaments): " << seq;
    }
    return true;
}

void ToolOrdering::collect_extruder_statistics(bool prime_multi_material)
{
    m_first_printing_extruder = (unsigned int)-1;
    for (const auto &lt : m_layer_tools)
        if (! lt.extruders.empty()) {
            m_first_printing_extruder = lt.extruders.front();
            break;
        }

    m_last_printing_extruder = (unsigned int)-1;
    for (auto lt_it = m_layer_tools.rbegin(); lt_it != m_layer_tools.rend(); ++ lt_it)
        if (! lt_it->extruders.empty()) {
            m_last_printing_extruder = lt_it->extruders.back();
            break;
        }

    m_all_printing_extruders.clear();
    for (const auto &lt : m_layer_tools) {
        append(m_all_printing_extruders, lt.extruders);
        sort_remove_duplicates(m_all_printing_extruders);
    }

    if (prime_multi_material && ! m_all_printing_extruders.empty()) {
        // Reorder m_all_printing_extruders in the sequence they will be primed, the last one will be m_first_printing_extruder.
        // Then set m_first_printing_extruder to the 1st extruder primed.
        m_all_printing_extruders.erase(
            std::remove_if(m_all_printing_extruders.begin(), m_all_printing_extruders.end(),
                [ this ](const unsigned int eid) { return eid == m_first_printing_extruder; }),
            m_all_printing_extruders.end());
        m_all_printing_extruders.emplace_back(m_first_printing_extruder);
        m_first_printing_extruder = m_all_printing_extruders.front();
    }
}

// ============================================================================================
// Ultra (dual-nozzle): filament->nozzle grouping compute. Ported from Bambu Studio, scoped to
// grouping-model machines (H2D/H2C/X2D) via DynamicPrintConfig::support_different_extruders(). The
// classic single-/multi-extruder pipeline (incl. Snapmaker U1) never enters this path. Grouping inputs
// that BBS derives from geometry pre-slicing (geometric/flow unprintables, print time) are stubbed
// empty in this fork; grouping is therefore flush/color driven only. See Print::get_*_unprintable_*.
// ============================================================================================
namespace GroupReorder {

std::function<bool(int, std::vector<int>&)> create_custom_seq_function(
    const PrintConfig& print_config,
    bool include_first_layer,
    const std::vector<unsigned int>& first_layer_filaments = {})
{
    std::vector<LayerPrintSequence> other_layers_seqs = get_other_layers_print_sequence(
        print_config.other_layers_print_sequence_nums.value,
        print_config.other_layers_print_sequence.values);

    return [other_layers_seqs, include_first_layer, first_layer_filaments](int layer_idx, std::vector<int>& out_seq) -> bool {
        if (include_first_layer && layer_idx == 0 && !first_layer_filaments.empty()) {
            out_seq.resize(first_layer_filaments.size());
            std::transform(first_layer_filaments.begin(), first_layer_filaments.end(), out_seq.begin(), [](int v) { return v + 1; });
            return true;
        }
        for (size_t idx = other_layers_seqs.size() - 1; idx != size_t(-1); --idx) {
            const auto& other_layers_seq = other_layers_seqs[idx];
            if (layer_idx + 1 >= other_layers_seq.first.first && layer_idx + 1 <= other_layers_seq.first.second) {
                out_seq = other_layers_seq.second;
                return true;
            }
        }
        return false;
    };
}

// nozzle_volume_type is one value per extruder in Bambu Studio's profiles, but this fork's configs can
// carry a single value for a two-extruder machine (the H2D/H2C CONFIG_BLOCK reads
// "nozzle_volume_type = Standard"). Indexing values[1] then read past the end of the vector, and the
// garbage landed in the grouping's nozzle list (seen as filament_volume_map 1700932980 in a header).
// A missing entry is a Standard nozzle, as GCode.cpp's per-nozzle shim already assumes.
static NozzleVolumeType nozzle_volume_type_at(const PrintConfig& print_config, size_t idx)
{
    const auto& values = print_config.nozzle_volume_type.values;
    return idx < values.size() ? NozzleVolumeType(values[idx]) : NozzleVolumeType::nvtStandard;
}

// extruder_max_nozzle_count is one value per extruder, but nothing sizes it to the extruder count:
// a printer preset that does not set it (every non-Bambu profile) keeps the one-entry default, and a
// profile can write it shorter than nozzle_diameter. Reading values[idx] past the end returned heap
// garbage, which build_nozzle_list then expanded into that many nozzles - gigabytes within seconds
// (upstream's Custom MyToolChanger, five extruders). A missing or nil entry is one nozzle; the upper
// bound only keeps a corrupt value from doing the same (the H2C rack, the largest real cluster, is 6).
static int extruder_max_nozzle_count_at(const PrintConfig& print_config, size_t idx)
{
    constexpr int max_sane_count = 64;
    const auto&   values         = print_config.extruder_max_nozzle_count.values;
    if (idx >= values.size() || values[idx] == ConfigOptionIntsNullable::nil_value())
        return 1;
    return std::clamp(values[idx], 1, max_sane_count);
}

std::vector<MultiNozzleUtils::NozzleGroupInfo> build_nozzle_groups(const PrintConfig& print_config, size_t extruder_nums)
{
    std::vector<MultiNozzleUtils::NozzleGroupInfo> nozzle_groups;
    auto extruder_nozzle_counts = get_extruder_nozzle_stats(print_config.extruder_nozzle_stats.values);
    for (size_t idx = 0; idx < extruder_nums; ++idx) {
        if (idx >= extruder_nozzle_counts.size() || extruder_nozzle_counts[idx].empty()) {
            nozzle_groups.emplace_back(format_diameter_to_str(print_config.nozzle_diameter.values[idx]), nozzle_volume_type_at(print_config, idx), idx,
                                       extruder_max_nozzle_count_at(print_config, idx));
        } else {
            NozzleVolumeType type = nozzle_volume_type_at(print_config, idx);
            if (type == nvtHybrid) {
                for (auto [volume_type, count] : extruder_nozzle_counts[idx])
                    nozzle_groups.emplace_back(format_diameter_to_str(print_config.nozzle_diameter.values[idx]), volume_type, idx, count);
            } else
                nozzle_groups.emplace_back(format_diameter_to_str(print_config.nozzle_diameter.values[idx]), type, idx, extruder_nozzle_counts[idx][type]);
        }
    }
    return nozzle_groups;
}

std::vector<MultiNozzleUtils::NozzleInfo> build_default_nozzle_list(const PrintConfig& print_config, size_t extruder_nums)
{
    using namespace MultiNozzleUtils;
    std::vector<NozzleInfo> nozzle_list;
    for (size_t idx = 0; idx < extruder_nums; ++idx) {
        NozzleInfo tmp;
        tmp.diameter = format_diameter_to_str(print_config.nozzle_diameter.values[idx]);
        tmp.group_id = idx;
        tmp.extruder_id = idx;
        tmp.volume_type = nozzle_volume_type_at(print_config, idx);
        nozzle_list.emplace_back(std::move(tmp));
    }
    return nozzle_list;
}

std::vector<FlushMatrix> prepare_flush_matrices(const PrintConfig& print_config)
{
    size_t extruder_nums = print_config.nozzle_diameter.values.size();
    size_t filament_nums = print_config.filament_colour.values.size();
    std::vector<FlushMatrix> nozzle_flush_mtx;
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(print_config.flush_volumes_matrix.values, nozzle_id, extruder_nums)));
        // A flush matrix shorter than filaments x filaments per nozzle (a preset written for another
        // extruder count) must not be sliced past its end below: missing entries flush nothing.
        if (flush_matrix.size() < filament_nums * filament_nums)
            flush_matrix.resize(filament_nums * filament_nums, 0.f);
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < filament_nums; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * filament_nums, flush_matrix.begin() + (i + 1) * filament_nums));
        nozzle_flush_mtx.emplace_back(wipe_volumes);
    }
    auto flush_multiplies = (print_config.prime_volume_mode == PrimeVolumeMode::pvmFast) ? print_config.flush_multiplier_fast.values : std::vector<double>{};
    flush_multiplies.resize(extruder_nums, print_config.flush_multiplier.value);
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id)
        for (auto& vec : nozzle_flush_mtx[nozzle_id])
            for (auto& v : vec)
                v *= flush_multiplies[nozzle_id];
    return nozzle_flush_mtx;
}

FilamentGroupContext build_filament_group_context(
    Print* print,
    const std::vector<std::vector<unsigned int>>& layer_filaments,
    const std::vector<std::set<int>>& physical_unprintables,
    const std::vector<std::set<int>>& geometric_unprintables,
    const std::map<int, std::set<NozzleVolumeType>>& unprintable_volumes,
    FilamentMapMode mode,
    const std::unordered_map<int, int>& nozzle_status)
{
    using namespace MultiNozzleUtils;
    using namespace FilamentGroupUtils;

    FilamentGroupContext context;

    const auto& print_config = print->config();
    const size_t filament_nums = print_config.filament_colour.values.size();
    const size_t extruder_nums = print_config.nozzle_diameter.values.size();
    bool has_multiple_nozzle = std::any_of(print_config.extruder_max_nozzle_count.values.begin(), print_config.extruder_max_nozzle_count.values.end(),
                                           [](int v) { return v > 1; });

    auto nozzle_flush_mtx = prepare_flush_matrices(print_config);
    auto nozzle_groups = build_nozzle_groups(print_config, extruder_nums);

    std::vector<std::set<int>> ext_unprintable_filaments(2);
    collect_unprintable_limits(physical_unprintables, geometric_unprintables, ext_unprintable_filaments);

    bool ignore_ext_filament = false;
    // Ultra (Phase 10): the AMS slot budget comes ONLY from the Print member (set from the live AMS). We
    // deliberately never write extruder_ams_count in this fork (the web device UI reads it), so the config
    // key is only ever empty or a stale imported-3mf value — do NOT fall back to it, or that stale value
    // could silently drive grouping. Empty member -> empty -> calc_max_group_size default (flush path).
    const std::vector<std::string>& extruder_ams_count_str = print->get_ultra_ams_count();
    auto extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);
    std::vector<int> group_size = calc_max_group_size(extruder_ams_counts, ignore_ext_filament);

    if (print_config.has_filament_switcher) {
        int total_filaments = (int)filament_nums;
        for (auto& s : group_size)
            s = std::max(s, total_filaments);
    }

    // extruder_type is one value per extruder in Bambu's profiles but is dropped on load here (a legacy
    // key, PrintConfigDef::handle_legacy), so the config holds its one-entry default: a missing entry
    // is Direct Drive rather than whatever lies past the end of the vector.
    std::vector<bool> prefer_non_model_filament(extruder_nums, false);
    for (size_t idx = 0; idx < extruder_nums && idx < print_config.extruder_type.values.size(); ++idx)
        prefer_non_model_filament[idx] = (print_config.extruder_type.values[idx] == ExtruderType::etBowden);

    auto machine_filament_info = build_machine_filaments(print->get_extruder_filament_info(), extruder_ams_counts, ignore_ext_filament);

    // The grouping code walks filament_ids and indexes filament_info by the same position.
    // filament_ids is not normalized per-filament in this fork (see PrintConfig/Preset notes);
    // size it to the filament count so downstream indexing is safe. Empty ids just mean no
    // identity hint for grouping. Truncate if longer so we don't pair past the end.
    std::vector<std::string> filament_ids = print_config.filament_ids.values;
    if (filament_ids.size() != filament_nums)
        filament_ids.resize(filament_nums);
    std::vector<FilamentUsageType> filament_usage_types = print->get_filament_usage_type();

    FGMode fg_mode = mode == FilamentMapMode::fmmAutoForMatch ? FGMode::MatchMode : FGMode::FlushMode;
    context.model_info.flush_matrix = std::move(nozzle_flush_mtx);
    context.model_info.unprintable_filaments = ext_unprintable_filaments;
    context.model_info.layer_filaments = layer_filaments;
    context.model_info.filament_ids = filament_ids;
    context.model_info.unprintable_volumes = unprintable_volumes;

    // Consumers index filament_info by filament id, so it must span the filament count: a partial
    // or legacy config can leave any of these arrays short, and get_at clamps.
    context.model_info.filament_info.reserve(filament_nums);
    for (size_t idx = 0; idx < filament_nums; ++idx) {
        FilamentGroupUtils::FilamentInfo info;
        info.color      = print_config.filament_colour.get_at(idx);
        info.type       = print_config.filament_type.get_at(idx);
        info.is_support = print_config.filament_is_support.get_at(idx);
        // Ultra: usage_type is derived from object/support assignments and is sized by filament_type,
        // which can be shorter than filament_colour. Clamp or fall back rather than index past the end.
        if (idx < filament_usage_types.size())
            info.usage_type = filament_usage_types[idx];
        else
            info.usage_type = filament_usage_types.empty() ? FilamentUsageType::ModelOnly : filament_usage_types.front();
        context.model_info.filament_info.emplace_back(std::move(info));
    }

    context.speed_info.filament_print_time = print->get_filament_print_time();
    context.speed_info.group_with_time = print->config().group_algo_with_time;
    context.speed_info.filament_change_time = print->config().machine_load_filament_time + print->config().machine_unload_filament_time;
    context.speed_info.extruder_change_time = print->config().machine_switch_extruder_time;
    {
        double load_time = print->config().machine_load_filament_time;
        double unload_time = print->config().machine_unload_filament_time;
        context.speed_info.change_time_params.standard_load_time = static_cast<float>(load_time);
        context.speed_info.change_time_params.standard_unload_time = static_cast<float>(unload_time);
        context.speed_info.change_time_params.selector_load_time = static_cast<float>(load_time / 2);
        context.speed_info.change_time_params.selector_unload_time = static_cast<float>(unload_time / 2);
    }

    context.machine_info.machine_filament_info = machine_filament_info;
    context.machine_info.max_group_size = std::move(group_size);
    context.machine_info.master_extruder_id = print_config.master_extruder_id.value - 1;
    context.machine_info.prefer_non_model_filament = prefer_non_model_filament;

    context.group_info.total_filament_num = (int)(filament_nums);
    context.group_info.max_gap_threshold = 0.01;
    context.group_info.strategy = FGStrategy::BestCost;
    context.group_info.mode = fg_mode;
    context.group_info.ignore_ext_filament = ignore_ext_filament;
    context.group_info.has_filament_switcher = print_config.has_filament_switcher.value;

    if (mode == FilamentMapMode::fmmManual) {
        context.group_info.filament_volume_map = print_config.filament_volume_map.values;
        // FilamentGroup::rebuild_nozzle_unprintables indexes this with every used filament. Bambu Studio's
        // GUI keeps the project value sized to the filament count; this fork has no UI for it, so a project
        // usually carries the one-entry default and the manual grouping read past its end (undefined
        // behaviour: the same plate grouped differently from slice to slice). A filament without an entry
        // expects no particular nozzle volume type - Hybrid, as every automatic mode uses below.
        context.group_info.filament_volume_map.resize(filament_nums, (int)(NozzleVolumeType::nvtHybrid));
    } else
        context.group_info.filament_volume_map = std::vector<int>(filament_nums, (int)(NozzleVolumeType::nvtHybrid));

    context.nozzle_info.nozzle_list = build_nozzle_list(nozzle_groups);
    context.nozzle_info.extruder_nozzle_list = build_extruder_nozzle_list(context.nozzle_info.nozzle_list);

    if (context.nozzle_info.nozzle_list.empty())
        throw Slic3r::RuntimeError(std::string("No valid nozzle found. Please check nozzle count."));

    if (!nozzle_status.empty())
        context.nozzle_info.nozzle_status = nozzle_status;

    auto used_filaments = collect_sorted_used_filaments(layer_filaments);

    // add_volume_type_limits, only for single-nozzle-per-extruder dual machines
    if (!has_multiple_nozzle) {
        std::vector<std::set<int>> ext_unprintable_filaments_with_volume = ext_unprintable_filaments;
        for (auto& nozzle : context.nozzle_info.nozzle_list) {
            for (auto fil_id : used_filaments) {
                auto uv = context.model_info.unprintable_volumes[fil_id];
                // The unprintable limits are the engine's two extruders (collect_unprintable_limits).
                if (uv.count(nozzle.volume_type) && nozzle.extruder_id >= 0 && nozzle.extruder_id < (int) ext_unprintable_filaments_with_volume.size())
                    ext_unprintable_filaments_with_volume[nozzle.extruder_id].insert(fil_id);
            }
        }
        for (auto fil_id : used_filaments) {
            if (ext_unprintable_filaments_with_volume[0].count(fil_id) && ext_unprintable_filaments_with_volume[1].count(fil_id)) {
                ext_unprintable_filaments_with_volume[0].erase(fil_id);
                ext_unprintable_filaments_with_volume[1].erase(fil_id);
            }
        }
        context.model_info.unprintable_filaments = ext_unprintable_filaments_with_volume;
    }

    return context;
}

} // namespace GroupReorder

MultiNozzleUtils::LayeredNozzleGroupResult ToolOrdering::group_by_plate_map(Print *print, const std::vector<std::vector<unsigned int>> &layer_filaments)
{
    if (print == nullptr)
        return MultiNozzleUtils::LayeredNozzleGroupResult();
    const size_t extruders = print->config().nozzle_diameter.size();
    return get_recommended_filament_maps(print, layer_filaments, FilamentMapMode::fmmManual, std::vector<std::set<int>>(extruders),
                                         std::vector<std::set<int>>(extruders), std::map<int, std::set<NozzleVolumeType>>());
}

MultiNozzleUtils::LayeredNozzleGroupResult ToolOrdering::get_recommended_filament_maps(
    Print*                                            print,
    const std::vector<std::vector<unsigned int>>&     layer_filaments,
    const FilamentMapMode                             mode,
    const std::vector<std::set<int>>&                 physical_unprintables,
    const std::vector<std::set<int>>&                 geometric_unprintables,
    const std::map<int, std::set<NozzleVolumeType>>&  unprintable_volumes,
    const std::unordered_map<int, int>&               nozzle_status)
{
    using namespace FilamentGroupUtils;
    using namespace MultiNozzleUtils;
    using namespace GroupReorder;

    if (!print || layer_filaments.empty())
        return LayeredNozzleGroupResult();

    const auto& print_config = print->config();
    size_t filament_nums = print_config.filament_colour.values.size();
    size_t extruder_nums = print_config.nozzle_diameter.values.size();
    auto used_filaments = collect_sorted_used_filaments(layer_filaments);
    bool has_multiple_nozzle = std::any_of(print_config.extruder_max_nozzle_count.values.begin(), print_config.extruder_max_nozzle_count.values.end(),
                                           [](int v) { return v > 1; });
    bool has_multiple_extruder = extruder_nums > 1;

    auto nozzle_list = build_default_nozzle_list(print_config, extruder_nums);

    if (mode == FilamentMapMode::fmmManual && !has_multiple_nozzle) {
        // The map the plate asked for, not config().filament_map: an earlier pass of this slice (or the
        // previous slice) overwrote that with its computed result.
        auto manual_filament_map = print->filament_map_input();
        std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });
        auto result = LayeredNozzleGroupResult::create(manual_filament_map, nozzle_list, used_filaments);
        return result ? *result : LayeredNozzleGroupResult();
    }

    int master_extruder_id = print_config.master_extruder_id.value - 1;
    std::vector<int> ret(filament_nums, master_extruder_id);

    // The grouping engine knows two extruders (collect_unprintable_limits, the match mode's machine
    // filaments, the add_volume_type_limits pass below). DynamicPrintConfig::support_different_extruders()
    // keeps larger machines out of this path; should one get here anyway, each filament keeps its own
    // tool, as on any other toolchanger, instead of running the engine past its two extruders.
    if (extruder_nums > 2) {
        BOOST_LOG_TRIVIAL(warning) << "filament map: " << extruder_nums << " extruders, the nozzle grouping supports two; filaments keep their own tools";
        for (size_t f = 0; f < ret.size(); ++f)
            ret[f] = int(std::min(f, extruder_nums - 1));
        auto result_opt = LayeredNozzleGroupResult::create(ret, nozzle_list, used_filaments);
        return result_opt ? *result_opt : LayeredNozzleGroupResult();
    }

    if (has_multiple_extruder || has_multiple_nozzle) {
        auto context = build_filament_group_context(print, layer_filaments, physical_unprintables, geometric_unprintables, unprintable_volumes, mode, nozzle_status);

        if (has_multiple_nozzle && mode == FilamentMapMode::fmmManual) {
            auto manual_filament_map = print->filament_map_input();
            std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });
            {
                std::string map_str, used_str;
                for (int v : manual_filament_map) map_str += std::to_string(v) + " ";
                for (auto f : used_filaments) used_str += std::to_string(f) + " ";
                BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] manual grouping: filament->extruder(0-based) " << map_str << "| used " << used_str;
            }
            // calc_filament_group_for_manual_multi_nozzle indexes the map with every used filament
            // and the per-extruder unprintable list with the map value (BambuStudio does the same,
            // unchecked); a short map or a value other than left/right is a group error, not a crash.
            for (auto fid : used_filaments)
                if (fid >= manual_filament_map.size() || (manual_filament_map[fid] != 0 && manual_filament_map[fid] != 1))
                    throw Slic3r::RuntimeError(std::string("Group error in manual mode. Please check nozzle count or regroup."));
            ret = calc_filament_group_for_manual_multi_nozzle(manual_filament_map, context);
        } else if (has_multiple_nozzle && mode == FilamentMapMode::fmmAutoForMatch &&
                   std::any_of(context.machine_info.machine_filament_info.begin(), context.machine_info.machine_filament_info.end(),
                               [](const auto& per_nozzle) { return !per_nozzle.empty(); })) {
            ret = calc_filament_group_for_match_multi_nozzle(context);
        } else {
            // Match mode with no machine filaments to match against (project or preset asks for
            // "Auto For Match", no AMS data on this PC) used to throw out of the slice; group for
            // flush instead, which needs nothing from the printer.
            if (has_multiple_nozzle && mode == FilamentMapMode::fmmAutoForMatch)
                BOOST_LOG_TRIVIAL(warning) << "filament map: Auto For Match asked for but no AMS filament data is available; grouping for flush instead";
            FilamentGroup fg(context);
            if (!has_multiple_nozzle)
                fg.get_custom_seq = create_custom_seq_function(print_config, false);
            ret = fg.calc_filament_group();
        }

        if (has_multiple_nozzle) {
            auto result_opt = LayeredNozzleGroupResult::create(ret, context.nozzle_info.nozzle_list, used_filaments);
            if (!result_opt) return LayeredNozzleGroupResult();
            return *result_opt;
        }
    }

    auto result_opt = LayeredNozzleGroupResult::create(ret, nozzle_list, used_filaments);
    return result_opt ? *result_opt : LayeredNozzleGroupResult();
}

ToolOrdering::LayerData ToolOrdering::collect_layer_and_unprintable_data()
{
    LayerData data;
    for (auto& lt : m_layer_tools)
        data.layer_filaments.emplace_back(lt.extruders);
    data.used_filaments = collect_sorted_used_filaments(data.layer_filaments);
    // Ultra: geometric unprintables and mixed-filament expansion are not populated in this fork (stubbed).
    data.geometric_unprintables = m_print ? m_print->get_geometric_unprintable_filaments() : std::vector<std::set<int>>{};
    data.physical_unprintables = m_print ? m_print->get_physical_unprintable_filaments(data.used_filaments) : std::vector<std::set<int>>{};
    data.filament_unprintable_volumes = m_print ? m_print->get_filament_unprintable_flow(data.used_filaments) : std::map<int, std::set<NozzleVolumeType>>{};
    return data;
}

// Declared in ToolOrdering.hpp (exposed for unit testing).
std::vector<unsigned int> parse_cyclic_order(const std::string& str, unsigned int number_of_extruders)
{
    std::vector<unsigned int> order;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            size_t pos      = 0;
            int    filament = std::stoi(token, &pos); // stoi skips leading whitespace by itself
            // stoi stops at the first non-digit, so "2x" would parse as 2. Require the whole token to be
            // consumed (bar trailing whitespace) to drop it like any other garbage.
            if (token.find_first_not_of(" \t\r\n", pos) != std::string::npos)
                continue;
            if (filament >= 1 && (unsigned int)filament <= number_of_extruders
                && std::find(order.begin(), order.end(), (unsigned int)(filament - 1)) == order.end())
                order.emplace_back((unsigned int)(filament - 1));
        } catch (const std::exception&) {
            // Not a number, ignore it.
        }
    }
    return order;
}

std::vector<unsigned int> cyclic_order_from_customized_sequence(
    const std::vector<LayerPrintSequence>& other_layers_seqs, unsigned int filament_count)
{
    // The plate's customized "other layers print sequence" (set per plate through the filament
    // sequence UI) stores 1-based filament numbers per layer range. A cyclic layer outside every
    // customized range uses the first customized range's sequence as its base order, so a user who
    // customized the plate's filament order gets that same order from cyclic ordering without
    // duplicating it into toolchange_cyclic_order. Same validation as parse_cyclic_order: drop
    // out-of-range and duplicate entries. An empty result means "nothing customized" and leaves
    // the plain ascending order.
    for (const LayerPrintSequence& range_seq : other_layers_seqs) {
        std::vector<unsigned int> order;
        for (int filament_1based : range_seq.second) {
            if (filament_1based >= 1 && (unsigned int)filament_1based <= filament_count
                && std::find(order.begin(), order.end(), (unsigned int)(filament_1based - 1)) == order.end())
                order.emplace_back((unsigned int)(filament_1based - 1));
        }
        if (!order.empty())
            return order;
    }
    return {};
}

void apply_cyclic_order(std::vector<unsigned int>& filaments, const std::vector<unsigned int>& cyclic_order)
{
    std::sort(filaments.begin(), filaments.end());
    if (!cyclic_order.empty())
        std::stable_sort(filaments.begin(), filaments.end(), [&cyclic_order](unsigned int lhs, unsigned int rhs) {
            auto rank = [&cyclic_order](unsigned int filament) {
                return size_t(std::find(cyclic_order.begin(), cyclic_order.end(), filament) - cyclic_order.begin());
            };
            return rank(lhs) < rank(rhs);
        });
}

unsigned int cyclic_filament_count(size_t num_physical, const MixedFilamentManager *mixed_mgr, unsigned int number_of_extruders)
{
    // Physical + enabled mixed: the indices ToolOrdering already uses. Deliberately NOT the
    // flush-matrix sqrt, so a short flush_volumes_matrix cannot shrink the valid range and
    // silently drop the tail of a user sequence. Falls back to the extruder count only when
    // there is no physical count to go on at all.
    unsigned int count = (unsigned int) num_physical;
    if (mixed_mgr)
        count = (unsigned int) mixed_mgr->total_filaments(num_physical);
    if (count == 0)
        count = number_of_extruders;
    return count;
}

std::function<bool(int, std::vector<int>&)> make_cyclic_custom_seq(
    const std::vector<LayerPrintSequence>         &other_layers_seqs,
    const std::vector<std::vector<unsigned int>>  &layer_filaments,
    bool                                           use_cyclic_ordering,
    bool                                           cyclic_first_layer,
    const std::vector<unsigned int>               &cyclic_order)
{
    // other_layers_seq: the layer_idx and extruder_idx are base on 1.
    return [other_layers_seqs, layer_filaments, use_cyclic_ordering, cyclic_first_layer, cyclic_order]
           (int layer_idx, std::vector<int>& out_seq) -> bool {
        for (size_t idx = other_layers_seqs.size() - 1; idx != size_t(-1); --idx) {
            const auto &other_layers_seq = other_layers_seqs[idx];
            if (layer_idx + 1 >= other_layers_seq.first.first && layer_idx + 1 <= other_layers_seq.first.second) {
                out_seq = other_layers_seq.second;
                return true;
            }
        }

        // Skip the first layer unless the user asked for cyclic order on it, so it keeps the
        // adhesion-optimized / custom first-layer sequence.
        if (use_cyclic_ordering && layer_idx >= 0 && (layer_idx != 0 || cyclic_first_layer)
            && size_t(layer_idx) < layer_filaments.size()) {
            std::vector<unsigned int> ordered = layer_filaments[size_t(layer_idx)];
            apply_cyclic_order(ordered, cyclic_order);
            out_seq.resize(ordered.size());
            std::transform(ordered.begin(), ordered.end(), out_seq.begin(), [](auto item) { return int(item) + 1; });
            return true;
        }

        return false;
    };
}

void ToolOrdering::reorder_extruders_for_minimum_flush_volume()
{
    const PrintConfig *print_config = m_print_config_ptr;
    if (!print_config && m_print_object_ptr) {
        print_config = &(m_print_object_ptr->print()->config());
    }

    if (!print_config || m_layer_tools.empty())
        return;

    // Ultra (dual-nozzle): for grouping-model machines (H2D/H2C/X2D), compute the filament->nozzle grouping
    // and store it on the Print for downstream GCode per-layer nozzle queries. Gated on
    // support_different_extruders() (distinct extruder variants) so classic single-nozzle machines and the
    // U1 toolchanger (identical variants) never enter this path. Non-sequential prints only. The classic
    // flush-reorder body below runs unchanged in all cases.
    // reorder_extruders_for_minimum_flush_volume runs more than once per slice (BBS's filament-savings
    // estimate pass + the real pass). Each call recomputes and rewrites the stored result, filament_map
    // header, and placeholder together, so the LAST call wins and the grouping-dependent outputs stay
    // mutually consistent. (Do NOT guard on the already-stored result: it persists across slices and would
    // leave a re-slice single-mapped.) The grouping is non-deterministic, so the map may vary per slice.
    if (m_print && m_print_full_config) {
        int extruder_count = 0;
        bool is_sequential = (print_config->print_sequence == PrintSequence::ByObject) && (m_print->objects().size() > 1);
        if (!is_sequential && const_cast<DynamicPrintConfig*>(m_print_full_config)->support_different_extruders(extruder_count)) {
            LayerData layer_data = collect_layer_and_unprintable_data();
            // Ultra (Phase 10): force match mode from the Print flag (set when a live AMS is available) rather
            // than the config key, which we do NOT overwrite (the web device UI reads filament_map_mode).
            // A plate grouped by hand (filament_map_mode Manual from the plate config, the pre-slice
            // confirmation on a Bambu two-extruder printer) always wins over the live-AMS match mode.
            const FilamentMapMode config_mode = print_config->filament_map_mode.value;
            const bool            manual      = config_mode == FilamentMapMode::fmmManual || config_mode == FilamentMapMode::fmmNozzleManual;
            FilamentMapMode group_mode = (!manual && m_print->get_ultra_force_match_mode())
                                         ? FilamentMapMode::fmmAutoForMatch
                                         : config_mode;
            auto grouping = ToolOrdering::get_recommended_filament_maps(
                m_print, layer_data.layer_filaments, group_mode,
                layer_data.physical_unprintables, layer_data.geometric_unprintables, layer_data.filament_unprintable_volumes);
            m_print->set_nozzle_group_result(std::make_shared<MultiNozzleUtils::LayeredNozzleGroupResult>(grouping));
            // Ultra (Phase 6): write the 1-based filament->extruder map into the live config so process (wipe
            // tower), the config block, and gcode all route filaments to their assigned nozzle. This is the
            // simple half of BBS's update_filament_maps_to_config; the per-nozzle filament-variant config
            // cascade is deferred (identical-variant H2D nozzles make it a near-no-op).
            {
                std::vector<int> em = grouping.get_extruder_map(false); // 1-based, per filament index
                if (!em.empty()) {
                    // em is sized to the filament count and is the authoritative per-filament map; write it
                    // unconditionally. (A >= size guard silently skipped the write when full_print_config's
                    // preset filament_map had a different length, leaving a stale header.)
                    const_cast<PrintConfig&>(m_print->config()).filament_map.values = em;
                    // Also write full_print_config: the "; filament_map = " gcode header (append_full_config)
                    // reads THIS, and the Bambu printer routes filaments to nozzles from that header.
                    if (m_print_full_config) {
                        auto* opt = const_cast<DynamicPrintConfig*>(m_print_full_config)->option<ConfigOptionInts>("filament_map", true);
                        if (opt) opt->values = em;
                    }
                    // Owner decision D2: on a dual-nozzle printer with High Flow support each filament slices
                    // the column of the nozzle its extruder carries. The grouping above is what decides the
                    // extruder (an auto-grouped filament may land on the High Flow nozzle), so the per-filament
                    // flow type follows the final map here, before the wipe tower and G-code read it.
                    if (BambuFlowSupport::apply_filament_volume_types_from_map(const_cast<PrintConfig&>(m_print->config()))) {
                        if (m_print_full_config)
                            const_cast<DynamicPrintConfig*>(m_print_full_config)->option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values =
                                m_print->config().filament_volume_type.values;
                    }
                }
            }
            // Ultra (dual-nozzle): warning-level so it survives the fork's default log filter; lets a slice
            // log confirm grouping fired and show the filament->extruder assignment.
            {
                std::string map_str;
                for (int e : grouping.get_extruder_map(true)) map_str += std::to_string(e) + " ";
                BOOST_LOG_TRIVIAL(warning) << "[Ultra] nozzle grouping computed: extruders=" << extruder_count
                                           << " filament->extruder(0-based): " << map_str;
            }
        }
    }

    // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
    std::vector<float> flush_matrix(cast<float>(print_config->flush_volumes_matrix.values));
    const unsigned int number_of_extruders = (unsigned int) (sqrt(flush_matrix.size()) + EPSILON);
    // Extract purging volumes for each extruder pair:
    std::vector<std::vector<float>> wipe_volumes;
    if ((print_config->purge_in_prime_tower && print_config->single_extruder_multi_material) || m_is_BBL_printer) {
        for (unsigned int i = 0; i < number_of_extruders; ++i)
            wipe_volumes.push_back( std::vector<float>(flush_matrix.begin() + i * number_of_extruders,
                                                       flush_matrix.begin() + (i + 1) * number_of_extruders));
    } else {
        // populate wipe_volumes with prime_volume
        for (unsigned int i = 0; i < number_of_extruders; ++i)
            wipe_volumes.push_back(std::vector<float>(number_of_extruders, print_config->prime_volume));
    }

    auto extruders_to_hash_key = [](const std::vector<unsigned int>& extruders,
                                    std::optional<unsigned int>      initial_extruder_id) -> uint32_t {
        uint32_t hash_key = 0;
        // high 16 bit define initial extruder ,low 16 bit define extruder set
        if (initial_extruder_id)
            hash_key |= (1 << (16 + *initial_extruder_id));
        for (auto item : extruders)
            hash_key |= (1 << item);
        return hash_key;
    };

    std::vector<LayerPrintSequence> other_layers_seqs;
    const ConfigOptionInts *other_layers_print_sequence_op = print_config->option<ConfigOptionInts>("other_layers_print_sequence");
    const ConfigOptionInt *other_layers_print_sequence_nums_op = print_config->option<ConfigOptionInt>("other_layers_print_sequence_nums");
    if (other_layers_print_sequence_op && other_layers_print_sequence_nums_op) {
        const std::vector<int> &print_sequence = other_layers_print_sequence_op->values;
        int sequence_nums = other_layers_print_sequence_nums_op->value;
        other_layers_seqs = get_other_layers_print_sequence(sequence_nums, print_sequence);
    }

    // Snapshot per-layer filament sets before this pass mutates order. Ultra grouping above only
    // writes filament_map / nozzle grouping; it does not reorder lt.extruders.
    std::vector<std::vector<unsigned int>> layer_filaments;
    layer_filaments.reserve(m_layer_tools.size());
    for (const auto &lt : m_layer_tools)
        layer_filaments.emplace_back(lt.extruders);

    const bool use_cyclic_ordering =
        (print_config->toolchange_ordering == ToolChangeOrderingType::Cyclic);

    // By default the first layer keeps its adhesion-optimized order (and any custom first layer
    // sequence); the cyclic sequence is only forced onto it when the user opts in.
    const bool cyclic_first_layer = use_cyclic_ordering && print_config->toolchange_cyclic_first_layer.value;

    // Optional user defined cyclic sequence, given as 1-based filament numbers ("3,2,1,4"). Filaments
    // missing from it keep their ascending order after the listed ones, so a partial or bogus entry
    // still yields the default cyclic order. Count is physical + enabled mixed (the indices ToolOrdering
    // already uses), not flush-matrix sqrt, so a short flush_volumes_matrix cannot shrink the valid range.
    const unsigned int filament_count = cyclic_filament_count(m_num_physical, m_mixed_mgr, number_of_extruders);

    const std::vector<unsigned int> cyclic_order =
        use_cyclic_ordering ? parse_cyclic_order(print_config->toolchange_cyclic_order.value, filament_count)
                            : std::vector<unsigned int>();

    // No explicit cyclic string: fall back to the plate's customized filament sequence (the
    // per-plate "other layers print sequence") as the cyclic base order. The explicit string
    // keeps winning, and with no customization anywhere this stays empty, so cyclic layers keep
    // the plain ascending order exactly as before.
    const std::vector<unsigned int> effective_cyclic_order =
        cyclic_order.empty() ? cyclic_order_from_customized_sequence(other_layers_seqs, filament_count)
                             : cyclic_order;

    auto get_custom_seq = make_cyclic_custom_seq(other_layers_seqs, layer_filaments,
                                                 use_cyclic_ordering, cyclic_first_layer, effective_cyclic_order);

    std::optional<unsigned int> current_extruder_id;
    for (int i = 0; i < m_layer_tools.size(); ++i) {
        LayerTools& lt = m_layer_tools[i];
        if (lt.extruders.empty())
            continue;
        if (lt.preserve_extruder_order) {
            current_extruder_id = lt.extruders.back();
            continue;
        }

        std::vector<int> custom_extruder_seq;
        if (get_custom_seq(i, custom_extruder_seq) && !custom_extruder_seq.empty()) {
            std::vector<unsigned int> unsign_custom_extruder_seq;
            for (int extruder : custom_extruder_seq) {
                unsigned int unsign_extruder = static_cast<unsigned int>(extruder) - 1;
                auto it = std::find(lt.extruders.begin(), lt.extruders.end(), unsign_extruder);
                if (it != lt.extruders.end()) {
                    unsign_custom_extruder_seq.emplace_back(unsign_extruder);
                }
            }
            assert(lt.extruders.size() == unsign_custom_extruder_seq.size());
            lt.extruders = unsign_custom_extruder_seq;
            current_extruder_id = lt.extruders.back();
            continue;
        }

        // The algorithm complexity is O(n2*2^n)
        if (i != 0) {
            auto hash_key = extruders_to_hash_key(lt.extruders, current_extruder_id);
            auto iter = m_tool_order_cache.find(hash_key);
            if (iter == m_tool_order_cache.end()) {
                lt.extruders = get_extruders_order(wipe_volumes, lt.extruders, current_extruder_id);
                std::vector<uint8_t> hash_val;
                hash_val.reserve(lt.extruders.size());
                for (auto item : lt.extruders)
                    hash_val.emplace_back(static_cast<uint8_t>(item));
                m_tool_order_cache[hash_key] = hash_val;
            }
            else {
                std::vector<unsigned int>extruder_order;
                extruder_order.reserve(iter->second.size());
                for (auto item : iter->second)
                    extruder_order.emplace_back(static_cast<unsigned int>(item));
                lt.extruders = std::move(extruder_order);
            }
        }
        current_extruder_id = lt.extruders.back();
    }
}

// Layers are marked for infinite skirt aka draft shield. Not all the layers have to be printed.
void ToolOrdering::mark_skirt_layers(const PrintConfig &config, coordf_t max_layer_height)
{
    if (m_layer_tools.empty())
        return;

    if (m_layer_tools.front().extruders.empty()) {
        // Empty first layer, no skirt will be printed.
        //FIXME throw an exception?
        return;
    }

    size_t i = 0;
    for (;;) {
        m_layer_tools[i].has_skirt = true;
        size_t j = i + 1;
        for (; j < m_layer_tools.size() && ! m_layer_tools[j].has_object; ++ j);
        // i and j are two successive layers printing an object.
        if (j == m_layer_tools.size())
            // Don't print skirt above the last object layer.
            break;
        // Mark some printing intermediate layers as having skirt.
        double last_z = m_layer_tools[i].print_z;
        for (size_t k = i + 1; k < j; ++ k) {
            if (m_layer_tools[k + 1].print_z - last_z > max_layer_height + EPSILON) {
                // Layer k is the last one not violating the maximum layer height.
                // Don't extrude skirt on empty layers.
                while (m_layer_tools[k].extruders.empty())
                    -- k;
                if (m_layer_tools[k].has_skirt) {
                    // Skirt cannot be generated due to empty layers, there would be a missing layer in the skirt.
                    //FIXME throw an exception?
                    break;
                }
                m_layer_tools[k].has_skirt = true;
                last_z = m_layer_tools[k].print_z;
            }
        }
        i = j;
    }
}

// Assign a pointer to a custom G-code to the respective ToolOrdering::LayerTools.
// Ignore color changes, which are performed on a layer and for such an extruder, that the extruder will not be printing above that layer.
// If multiple events are planned over a span of a single layer, use the last one.

// BBS: replace model custom gcode with current plate custom gcode
static CustomGCode::Info custom_gcode_per_print_z;
void ToolOrdering::assign_custom_gcodes(const Print &print)
{
	// Only valid for non-sequential print.
	assert(print.config().print_sequence == PrintSequence::ByLayer);

    custom_gcode_per_print_z = print.model().get_curr_plate_custom_gcodes();
	if (custom_gcode_per_print_z.gcodes.empty())
		return;

    // BBS
	auto 						num_filaments = unsigned(print.config().filament_diameter.size());
	CustomGCode::Mode 			mode          =
		(num_filaments == 1) ? CustomGCode::SingleExtruder :
		print.object_extruders().size() == 1 ? CustomGCode::MultiAsSingle : CustomGCode::MultiExtruder;
    CustomGCode::Mode           model_mode    = print.model().get_curr_plate_custom_gcodes().mode;
	std::vector<unsigned char> 	extruder_printing_above(num_filaments, false);
	auto 						custom_gcode_it = custom_gcode_per_print_z.gcodes.rbegin();
	// Tool changes and color changes will be ignored, if the model's tool/color changes were entered in mm mode and the print is in non mm mode
	// or vice versa.
	bool 						ignore_tool_and_color_changes = (mode == CustomGCode::MultiExtruder) != (model_mode == CustomGCode::MultiExtruder);
	// If printing on a single extruder machine, make the tool changes trigger color change (M600) events.
	bool 						tool_changes_as_color_changes = mode == CustomGCode::SingleExtruder && model_mode == CustomGCode::MultiAsSingle;

	// From the last layer to the first one:
    coordf_t print_z_above = std::numeric_limits<coordf_t>::lowest();
	for (auto it_lt = m_layer_tools.rbegin(); it_lt != m_layer_tools.rend(); ++ it_lt) {
		LayerTools &lt = *it_lt;
		// Add the extruders of the current layer to the set of extruders printing at and above this print_z.
		for (unsigned int i : lt.extruders)
			extruder_printing_above[i] = true;
		// Skip all custom G-codes above this layer and skip all extruder switches.
		for (; custom_gcode_it != custom_gcode_per_print_z.gcodes.rend() && (
            (print_z_above > lt.print_z && custom_gcode_it->print_z > 0.5 * (lt.print_z + print_z_above))
            || custom_gcode_it->type == CustomGCode::ToolChange); ++ custom_gcode_it);
        print_z_above = lt.print_z;
		if (custom_gcode_it == custom_gcode_per_print_z.gcodes.rend())
			// Custom G-codes were processed.
			break;
		// Some custom G-code is configured for this layer or a layer below.
		const CustomGCode::Item &custom_gcode = *custom_gcode_it;
		// print_z of the layer below the current layer.
		coordf_t print_z_below = 0.;
		if (auto it_lt_below = it_lt; ++ it_lt_below != m_layer_tools.rend())
			print_z_below = it_lt_below->print_z;
        if (custom_gcode.print_z > 0.5 * (print_z_below + lt.print_z)) {
			// The custom G-code applies to the current layer.
			bool color_change = custom_gcode.type == CustomGCode::ColorChange;
			bool tool_change  = custom_gcode.type == CustomGCode::ToolChange;
			bool pause_or_custom_gcode = ! color_change && ! tool_change;
			bool apply_color_change = ! ignore_tool_and_color_changes &&
				// If it is color change, it will actually be useful as the exturder above will print.
                // BBS
				(color_change ? 
					mode == CustomGCode::SingleExtruder || 
						(custom_gcode.extruder <= int(num_filaments) && extruder_printing_above[unsigned(custom_gcode.extruder - 1)]) :
				 	tool_change && tool_changes_as_color_changes);
			if (pause_or_custom_gcode || apply_color_change)
        		lt.custom_gcode = &custom_gcode;
			// Consume that custom G-code event.
			++ custom_gcode_it;
		}
	}
}

const LayerTools& ToolOrdering::tools_for_layer(coordf_t print_z) const
{
    auto it_layer_tools = std::lower_bound(m_layer_tools.begin(), m_layer_tools.end(), LayerTools(print_z - EPSILON));
    assert(it_layer_tools != m_layer_tools.end());
    coordf_t dist_min = std::abs(it_layer_tools->print_z - print_z);
    for (++ it_layer_tools; it_layer_tools != m_layer_tools.end(); ++ it_layer_tools) {
        coordf_t d = std::abs(it_layer_tools->print_z - print_z);
        if (d >= dist_min)
            break;
        dist_min = d;
    }
    -- it_layer_tools;
    assert(dist_min < EPSILON);
    return *it_layer_tools;
}

// This function is called from Print::mark_wiping_extrusions and sets extruder this entity should be printed with (-1 .. as usual)
void WipingExtrusions::set_extruder_override(const ExtrusionEntity* entity, const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;

    auto entity_map_it = (entity_map.emplace(std::make_tuple(entity, object), ExtruderPerCopy())).first; // (add and) return iterator
    ExtruderPerCopy& copies_vector = entity_map_it->second;
    copies_vector.resize(num_of_copies, -1);

    if (copies_vector[copy_id] != -1)
        std::cout << "ERROR: Entity extruder overriden multiple times!!!\n";    // A debugging message - this must never happen.

    copies_vector[copy_id] = extruder;
}

// BBS
void WipingExtrusions::set_support_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;
    support_map.emplace(object, extruder);
}

void WipingExtrusions::set_support_interface_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies)
{
    something_overridden = true;
    support_intf_map.emplace(object, extruder);
}

// Finds first non-soluble extruder on the layer
int WipingExtrusions::first_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const
{
    const LayerTools& lt = *m_layer_tools;
    for (auto extruders_it = lt.extruders.begin(); extruders_it != lt.extruders.end(); ++extruders_it)
        if (!print_config.filament_soluble.get_at(*extruders_it) && !print_config.filament_is_support.get_at(*extruders_it))
            return (*extruders_it);

    return (-1);
}

// Finds last non-soluble extruder on the layer
int WipingExtrusions::last_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const
{
    const LayerTools& lt = *m_layer_tools;
    for (auto extruders_it = lt.extruders.rbegin(); extruders_it != lt.extruders.rend(); ++extruders_it)
        if (!print_config.filament_soluble.get_at(*extruders_it) && !print_config.filament_is_support.get_at(*extruders_it))
            return (*extruders_it);

    return (-1);
}

// Decides whether this entity could be overridden
bool WipingExtrusions::is_overriddable(const ExtrusionEntityCollection& eec, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) const
{
    if (print_config.filament_soluble.get_at(m_layer_tools->extruder(eec, region)))
        return false;

    if (object.config().flush_into_objects)
        return true;

    if (!object.config().flush_into_infill || eec.role() != erInternalInfill)
        return false;

    return true;
}

// BBS
bool WipingExtrusions::is_support_overriddable(const ExtrusionRole role, const PrintObject& object) const
{
    if (!object.config().flush_into_support)
        return false;

    // v2.5a (spec item 1, "residual pin", root cause A of the khaki-in-white/teal-
    // support symptom): a mode-active object (support_filament_matching.value, i.e.
    // opted into per-bucket color matching) is NEVER overriddable
    // by WipingExtrusions, for ANY role, regardless of the scalar support_filament/
    // support_interface_filament checks below. Matched buckets are already removed
    // from support_fills by chameleon_assign_support_interfaces (Print.cpp) and
    // never consult WipingExtrusions at all - what's LEFT in support_fills for a
    // mode-active object is either genuinely-unmatched-fallback geometry or a
    // v2.5a apply_bucket_caps redirect target (still living inside a MATCHED
    // bucket's own interface_by_extruder entry, never back in support_fills). Both
    // must keep whatever color chameleon_assign_support_interfaces (or the
    // dedicated-emission path) already decided for them - if this predicate stayed
    // true here, flush_into_support's mark_wiping_extrusions (below) could still
    // claim and repaint that residual geometry with an arbitrary purge-target
    // extruder on every qualifying toolchange, including one with no nearby
    // geometry at all (the reported symptom). Off (checkbox unchecked, the default) is
    // completely unaffected - falls straight through to the pre-v2.5a role-based
    // logic below, byte-identical.
    if (object.config().support_filament_matching.value)
        return false;

    // Ultra (support groups, plan 2026-09-02 Stage 3 R3.5): the same pin, for the same reason, when
    // a support group picks its own interface filament. That group's interface lives in
    // SupportLayer::interface_by_extruder and never reaches support_fills, but what IS left in
    // support_fills sits right next to it - and mark_wiping_extrusions would happily claim it and
    // repaint it with an arbitrary purge-target extruder, undoing a deliberately chosen interface
    // colour on the layers that matter most.
    if (object.has_support_group_interface_filament())
        return false;

    if (role == erMixed) {
        return object.config().support_filament == 0 || object.config().support_interface_filament == 0;
    }
    else if (role == erSupportMaterial || role == erSupportTransition) {
        return object.config().support_filament == 0;
    }
    else if (role == erSupportMaterialInterface) {
        return object.config().support_interface_filament == 0;
    }

    return false;
}

// Following function iterates through all extrusions on the layer, remembers those that could be used for wiping after toolchange
// and returns volume that is left to be wiped on the wipe tower.
float WipingExtrusions::mark_wiping_extrusions(const Print& print, unsigned int old_extruder, unsigned int new_extruder, float volume_to_wipe)
{
    const LayerTools& lt = *m_layer_tools;
    const float min_infill_volume = 0.f; // ignore infill with smaller volume than this

    if (! this->something_overridable || volume_to_wipe <= 0. || print.config().filament_soluble.get_at(old_extruder) || print.config().filament_soluble.get_at(new_extruder))
        return std::max(0.f, volume_to_wipe); // Soluble filament cannot be wiped in a random infill, neither the filament after it

    // BBS
    if (print.config().filament_is_support.get_at(old_extruder) || print.config().filament_is_support.get_at(new_extruder))
        return std::max(0.f, volume_to_wipe); // Support filament cannot be used to print support, infill, wipe_tower, etc.

    // we will sort objects so that dedicated for wiping are at the beginning:
    ConstPrintObjectPtrs object_list = print.objects().vector();
    // BBS: fix the exception caused by not fixed order between different objects
    std::sort(object_list.begin(), object_list.end(), [object_list](const PrintObject* a, const PrintObject* b) {
        if (a->config().flush_into_objects != b->config().flush_into_objects) {
            return a->config().flush_into_objects.getBool();
        }
        else {
            return a->id() < b->id();
        }
    });

    // We will now iterate through
    //  - first the dedicated objects to mark perimeters or infills (depending on infill_first)
    //  - second through the dedicated ones again to mark infills or perimeters (depending on infill_first)
    //  - then all the others to mark infills (in case that !infill_first, we must also check that the perimeter is finished already
    // this is controlled by the following variable:
    bool perimeters_done = false;

    for (int i=0 ; i<(int)object_list.size() + (perimeters_done ? 0 : 1); ++i) {
        if (!perimeters_done && (i==(int)object_list.size() || !object_list[i]->config().flush_into_objects)) { // we passed the last dedicated object in list
            perimeters_done = true;
            i=-1;   // let's go from the start again
            continue;
        }

        const PrintObject* object = object_list[i];

        // Finds this layer:
        const Layer* this_layer = object->get_layer_at_printz(lt.print_z, EPSILON);
        if (this_layer == nullptr)
        	continue;

        size_t num_of_copies = object->instances().size();

        // iterate through copies (aka PrintObject instances) first, so that we mark neighbouring infills to minimize travel moves
        for (unsigned int copy = 0; copy < num_of_copies; ++copy) {
            for (const LayerRegion *layerm : this_layer->regions()) {
                const auto &region = layerm->region();

                if (!object->config().flush_into_infill && !object->config().flush_into_objects && !object->config().flush_into_support)
                    continue;
                bool wipe_into_infill_only = !object->config().flush_into_objects && object->config().flush_into_infill;
                bool is_infill_first = region.config().is_infill_first;
                if (is_infill_first != perimeters_done || wipe_into_infill_only) {
                    for (const ExtrusionEntity* ee : layerm->fills.entities) {                      // iterate through all infill Collections
                        auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);

                        if (!is_overriddable(*fill, print.config(), *object, region))
                            continue;

                        if (wipe_into_infill_only && ! is_infill_first)
                            // In this case we must check that the original extruder is used on this layer before the one we are overridding
                            // (and the perimeters will be finished before the infill is printed):
                            if (!lt.is_extruder_order(lt.wall_filament(region), new_extruder))
                                continue;

                        if ((!is_entity_overridden(fill, object, copy) && fill->total_volume() > min_infill_volume))
                        {     // this infill will be used to wipe this extruder
                            set_extruder_override(fill, object, copy, new_extruder, num_of_copies);
                            if ((volume_to_wipe -= float(fill->total_volume())) <= 0.f)
                            	// More material was purged already than asked for.
	                            return 0.f;
                        }
                    }
                }

                // Now the same for perimeters - see comments above for explanation:
                if (object->config().flush_into_objects && is_infill_first == perimeters_done)
                {
                    for (const ExtrusionEntity* ee : layerm->perimeters.entities) {
                        auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                        if (is_overriddable(*fill, print.config(), *object, region) && !is_entity_overridden(fill, object, copy) && fill->total_volume() > min_infill_volume) {
                            set_extruder_override(fill, object, copy, new_extruder, num_of_copies);
                            if ((volume_to_wipe -= float(fill->total_volume())) <= 0.f)
                            	// More material was purged already than asked for.
	                            return 0.f;
                        }
                    }
                }
            }

            // BBS
            if (object->config().flush_into_support) {
                auto& object_config = object->config();
                const SupportLayer* this_support_layer = object->get_support_layer_at_printz(lt.print_z, EPSILON);

                do {
                    if (this_support_layer == nullptr)
                        break;

                    // v2.5a (spec item 1): route through is_support_overriddable
                    // instead of re-checking the scalar configs inline, so this site
                    // and is_support_overriddable's own role-dispatch table can never
                    // drift apart again - in particular, this is what makes the
                    // residual pin (is_support_overriddable's mode-active early
                    // return, above) apply here too: gating only that one site while
                    // leaving this inline duplicate unchanged would have been a
                    // silent regression (mode-active objects would still get their
                    // residual support_fills claimed/repainted by this branch).
                    bool support_overriddable = is_support_overriddable(erSupportMaterial, *object);
                    bool support_intf_overriddable = is_support_overriddable(erSupportMaterialInterface, *object);
                    // v2.5a (spec item 3, "matched-run guard"): defensive - a mode-
                    // active object must never reach set_support_extruder_override/
                    // set_support_interface_extruder_override below (the two
                    // functions that populate support_map/support_intf_map) with
                    // either flag true; is_support_overriddable's own residual pin
                    // (above) already guarantees this structurally, by construction
                    // of the two calls just above. Asserted rather than trusted
                    // silently so a future edit that reintroduces an inline
                    // duplicate at this site (the exact drift this refactor exists
                    // to make impossible) fails loudly in a debug build instead of
                    // silently letting WipingExtrusions claim/repaint a mode-active
                    // object's residual support_fills again - see is_support_
                    // overriddable's own comment for the full symptom this pin
                    // fixes. Matched buckets themselves never reach here at all
                    // (chameleon_assign_support_interfaces, Print.cpp, removes them
                    // from support_fills entirely and never calls into
                    // WipingExtrusions/support_map/support_intf_map for them).
                    assert(!object_config.support_filament_matching.value
                           || (!support_overriddable && !support_intf_overriddable));
                    if (!support_overriddable && !support_intf_overriddable)
                        break;

                    auto &entities = this_support_layer->support_fills.entities;
                    if (support_overriddable && !is_support_overridden(object) && !(object_config.support_interface_not_for_body.value && !support_intf_overriddable &&(new_extruder==object_config.support_interface_filament-1||old_extruder==object_config.support_interface_filament-1))) {
                        set_support_extruder_override(object, copy, new_extruder, num_of_copies);
                        for (const ExtrusionEntity* ee : entities) {
                            if (ee->role() == erSupportMaterial || ee->role() == erSupportTransition)
                                volume_to_wipe -= ee->total_volume();

                            if (volume_to_wipe <= 0.f)
                                return 0.f;
                        }
                    }

                    if (support_intf_overriddable && !is_support_interface_overridden(object)) {
                        set_support_interface_extruder_override(object, copy, new_extruder, num_of_copies);
                        for (const ExtrusionEntity* ee : entities) {
                            if (ee->role() == erSupportMaterialInterface)
                                volume_to_wipe -= ee->total_volume();

                            if (volume_to_wipe <= 0.f)
                                return 0.f;
                        }
                    }
                } while (0);
            }
        }
    }
	// Some purge remains to be done on the Wipe Tower.
    assert(volume_to_wipe > 0.);
    return volume_to_wipe;
}



// Called after all toolchanges on a layer were mark_infill_overridden. There might still be overridable entities,
// that were not actually overridden. If they are part of a dedicated object, printing them with the extruder
// they were initially assigned to might mean violating the perimeter-infill order. We will therefore go through
// them again and make sure we override it.
void WipingExtrusions::ensure_perimeters_infills_order(const Print& print)
{
	if (! this->something_overridable)
		return;

    const LayerTools& lt = *m_layer_tools;
    unsigned int first_nonsoluble_extruder = first_nonsoluble_extruder_on_layer(print.config());
    unsigned int last_nonsoluble_extruder = last_nonsoluble_extruder_on_layer(print.config());

    for (const PrintObject* object : print.objects()) {
        // Finds this layer:
        const Layer* this_layer = object->get_layer_at_printz(lt.print_z, EPSILON);
        if (this_layer == nullptr)
        	continue;
        size_t num_of_copies = object->instances().size();

        for (size_t copy = 0; copy < num_of_copies; ++copy) {    // iterate through copies first, so that we mark neighbouring infills to minimize travel moves
            for (const LayerRegion *layerm : this_layer->regions()) {
                const auto &region = layerm->region();
                //BBS
                if (!object->config().flush_into_infill && !object->config().flush_into_objects)
                    continue;

                bool is_infill_first = region.config().is_infill_first;
                for (const ExtrusionEntity* ee : layerm->fills.entities) {                      // iterate through all infill Collections
                    auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);

                    if (!is_overriddable(*fill, print.config(), *object, region)
                     || is_entity_overridden(fill, object, copy) )
                        continue;

                    // This infill could have been overridden but was not - unless we do something, it could be
                    // printed before its perimeter, or not be printed at all (in case its original extruder has
                    // not been added to LayerTools
                    // Either way, we will now force-override it with something suitable:
                    //BBS
                    if (is_infill_first
                    //BBS
                    //|| object->config().flush_into_objects  // in this case the perimeter is overridden, so we can override by the last one safely
                    || lt.is_extruder_order(lt.wall_filament(region), last_nonsoluble_extruder    // !infill_first, but perimeter is already printed when last extruder prints
                    || ! lt.has_extruder(lt.sparse_infill_filament(region)))) // we have to force override - this could violate infill_first (FIXME)
                        set_extruder_override(fill, object, copy, (is_infill_first ? first_nonsoluble_extruder : last_nonsoluble_extruder), num_of_copies);
                    else {
                        // In this case we can (and should) leave it to be printed normally.
                        // Force overriding would mean it gets printed before its perimeter.
                    }
                }

                // Now the same for perimeters - see comments above for explanation:
                for (const ExtrusionEntity* ee : layerm->perimeters.entities) {                      // iterate through all perimeter Collections
                    auto* fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                    if (is_overriddable(*fill, print.config(), *object, region) && ! is_entity_overridden(fill, object, copy))
                        set_extruder_override(fill, object, copy, (is_infill_first ? last_nonsoluble_extruder : first_nonsoluble_extruder), num_of_copies);
                }
            }
        }
    }
}

// Following function is called from GCode::process_layer and returns pointer to vector with information about which extruders should be used for given copy of this entity.
// If this extrusion does not have any override, nullptr is returned.
// Otherwise it modifies the vector in place and changes all -1 to correct_extruder_id (at the time the overrides were created, correct extruders were not known,
// so -1 was used as "print as usual").
// The resulting vector therefore keeps track of which extrusions are the ones that were overridden and which were not. If the extruder used is overridden,
// its number is saved as is (zero-based index). Regular extrusions are saved as -number-1 (unfortunately there is no negative zero).
const WipingExtrusions::ExtruderPerCopy* WipingExtrusions::get_extruder_overrides(const ExtrusionEntity* entity, const PrintObject* object, int correct_extruder_id, size_t num_of_copies)
{
	ExtruderPerCopy *overrides = nullptr;
    auto entity_map_it = entity_map.find(std::make_tuple(entity, object));
    if (entity_map_it != entity_map.end()) {
        overrides = &entity_map_it->second;
    	overrides->resize(num_of_copies, -1);
	    // Each -1 now means "print as usual" - we will replace it with actual extruder id (shifted it so we don't lose that information):
	    std::replace(overrides->begin(), overrides->end(), -1, -correct_extruder_id-1);
	}
    return overrides;
}

// BBS
int WipingExtrusions::get_support_extruder_overrides(const PrintObject* object)
{
    auto iter = support_map.find(object);
    if (iter != support_map.end())
        return iter->second;

    return -1;
}

int WipingExtrusions::get_support_interface_extruder_overrides(const PrintObject* object)
{
    auto iter = support_intf_map.find(object);
    if (iter != support_intf_map.end())
        return iter->second;

    return -1;
}

// Resolve a 1-based filament ID through the mixed-filament manager.
unsigned int ToolOrdering::resolve_mixed(unsigned int filament_id_1based,
                                         int          layer_index,
                                         float        layer_print_z,
                                         float        layer_height,
                                         const PrintObject* current_object) const
{
    return resolve_mixed_with_layer_heights(m_mixed_mgr,
                                            m_num_physical,
                                            filament_id_1based,
                                            layer_index,
                                            layer_print_z,
                                            layer_height,
                                            m_mixed_layer_height_a,
                                            m_mixed_layer_height_b,
                                            m_mixed_base_layer_height,
                                            current_object);
}

} // namespace Slic3r
