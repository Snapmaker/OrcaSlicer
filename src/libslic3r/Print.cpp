#include "Config.hpp"
#include "Exception.hpp"
#include "Print.hpp"
#include <cstdlib>
#include <cstdio>
#include "BoundingBox.hpp"
#include "Brim.hpp"
#include "BrimFilament.hpp"
#include "ClipperUtils.hpp"
#include "Extruder.hpp"
#include "Flow.hpp"
#include "Geometry/ConvexHull.hpp"
#include "I18N.hpp"
#include "LocalZOrderOptimizer.hpp"
#include "ShortestPath.hpp"
#include "Thread.hpp"
#include "Time.hpp"
#include "GCode.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "GCode/WipeTowerEstimate.hpp"
#include "Geometry.hpp"
#include "Utils.hpp"
#include "PrintConfig.hpp"
#include "FilamentHotBedNozzleRules.hpp"
#include "Model.hpp"
#include "format.hpp"
#include <float.h>
#include <cmath>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

//BBS: add json support
#include "nlohmann/json.hpp"

#include "GCode/ConflictChecker.hpp"

#include <codecvt>

using namespace nlohmann;

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

template class PrintState<PrintStep, psCount>;
template class PrintState<PrintObjectStep, posCount>;

PrintRegion::PrintRegion(const PrintRegionConfig &config) : PrintRegion(config, config.hash()) {}
PrintRegion::PrintRegion(PrintRegionConfig &&config) : PrintRegion(std::move(config), config.hash()) {}

namespace {

constexpr double LOCAL_Z_PERIMETER_MASK_EXPAND_MM = 0.10;

struct LocalZWipeTowerToolchange
{
    unsigned int old_tool { 0 };
    unsigned int new_tool { 0 };
};

struct LocalZWipeTowerPassRef
{
    size_t                    layer_to_print_idx { 0 };
    const SubLayerPlan       *plan { nullptr };
    std::vector<unsigned int> extruders;
};

static inline ExPolygons local_z_compensate_masks_for_wipe_tower(const ExPolygons &src_masks,
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

static bool local_z_segments_exist(Polylines segments)
{
    for (Polyline &segment : segments) {
        if (segment.is_valid())
            return true;
    }
    return false;
}

static bool extrusion_collection_has_local_z_segment(const ExtrusionEntityCollection &source,
                                                     const ExPolygons               &include_masks)
{
    if (source.entities.empty() || include_masks.empty())
        return false;

    ExtrusionEntityCollection flattened = source.flatten(false);
    for (const ExtrusionEntity *entity : flattened.entities) {
        if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity)) {
            if (local_z_segments_exist(intersection_pl(Polylines{path->polyline}, include_masks)))
                return true;
        } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            for (const ExtrusionPath &path : multipath->paths) {
                if (local_z_segments_exist(intersection_pl(Polylines{path.polyline}, include_masks)))
                    return true;
            }
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            for (const ExtrusionPath &path : loop->paths) {
                if (local_z_segments_exist(intersection_pl(Polylines{path.polyline}, include_masks)))
                    return true;
            }
        }
    }

    return false;
}

static bool layer_has_local_z_extrusions(const Layer &layer, const ExPolygons &pass_masks, bool include_infill)
{
    if (pass_masks.empty())
        return false;

    for (const LayerRegion *layer_region : layer.regions()) {
        for (const ExtrusionEntity *entity : layer_region->perimeters.entities) {
            const auto *extrusions = dynamic_cast<const ExtrusionEntityCollection*>(entity);
            if (extrusions == nullptr)
                continue;
            if (extrusion_collection_has_local_z_segment(*extrusions, pass_masks))
                return true;
        }
        if (!include_infill)
            continue;
        for (const ExtrusionEntity *entity : layer_region->fills.entities) {
            const auto *extrusions = dynamic_cast<const ExtrusionEntityCollection*>(entity);
            if (extrusions == nullptr)
                continue;
            if (extrusion_collection_has_local_z_segment(*extrusions, pass_masks))
                return true;
        }
    }

    return false;
}

static inline int shared_local_z_extruder_for_wipe_tower(const std::vector<unsigned int> &lhs,
                                                         const std::vector<unsigned int> &rhs)
{
    for (unsigned int extruder_id : lhs) {
        if (std::find(rhs.begin(), rhs.end(), extruder_id) != rhs.end())
            return static_cast<int>(extruder_id);
    }
    return -1;
}

static std::vector<unsigned int> rotate_extruders_to_start_with(const std::vector<unsigned int> &extruders,
                                                                unsigned int                      start_extruder)
{
    std::vector<unsigned int> rotated = extruders;
    auto it = std::find(rotated.begin(), rotated.end(), start_extruder);
    if (it != rotated.end())
        std::rotate(rotated.begin(), it, rotated.end());
    return rotated;
}

static std::vector<LocalZWipeTowerToolchange> collect_local_z_wipe_tower_toolchanges(
    const Print                              &print,
    const std::vector<GCode::LayerToPrint>   &layers,
    int                                       start_extruder)
{
    std::vector<LocalZWipeTowerPassRef> pass_refs;
    const bool  local_z_whole_objects_enabled = print.full_print_config().opt_bool("dithering_local_z_whole_objects");
    const bool  local_z_infill_enabled        = print.full_print_config().opt_bool("dithering_local_z_infill");
    const float local_z_perimeter_mask_expand = float(scale_(LOCAL_Z_PERIMETER_MASK_EXPAND_MM));

    for (size_t layer_to_print_idx = 0; layer_to_print_idx < layers.size(); ++layer_to_print_idx) {
        const GCode::LayerToPrint &layer_to_print = layers[layer_to_print_idx];
        if (layer_to_print.object_layer == nullptr)
            continue;

        const PrintObject *print_object =
            layer_to_print.original_object != nullptr ? layer_to_print.original_object : layer_to_print.object();
        if (print_object == nullptr)
            continue;

        const size_t       layer_id   = size_t(layer_to_print.object_layer->id());
        const auto        &intervals  = print_object->local_z_intervals();
        const auto        &plans      = print_object->local_z_sublayer_plan();
        const auto interval_it = std::find_if(intervals.begin(), intervals.end(), [layer_id](const LocalZInterval &interval) {
            return interval.layer_id == layer_id;
        });
        if (interval_it == intervals.end() || !interval_it->has_mixed_paint || interval_it->sublayer_count <= 1 ||
            interval_it->first_sublayer_idx >= plans.size()) {
            continue;
        }

        const size_t first_idx = interval_it->first_sublayer_idx;
        const size_t end_idx   = std::min(plans.size(), first_idx + interval_it->sublayer_count);
        for (size_t plan_idx = first_idx; plan_idx < end_idx; ++plan_idx) {
            const SubLayerPlan &plan = plans[plan_idx];
            if (!plan.split_interval)
                continue;

            const size_t plan_mask_slots =
                std::max(plan.painted_masks_by_extruder.size(), plan.fixed_painted_masks_by_extruder.size());
            std::vector<ExPolygons> compensated_masks_by_extruder(plan_mask_slots, ExPolygons());

            ExPolygons fixed_raw_masks_union;
            for (const ExPolygons &fixed_masks : plan.fixed_painted_masks_by_extruder) {
                if (!fixed_masks.empty())
                    append(fixed_raw_masks_union, fixed_masks);
            }
            if (!fixed_raw_masks_union.empty() && fixed_raw_masks_union.size() > 1)
                fixed_raw_masks_union = union_ex(fixed_raw_masks_union);

            const ExPolygons fixed_compensated_guard =
                fixed_raw_masks_union.empty() ?
                    ExPolygons() :
                    local_z_compensate_masks_for_wipe_tower(fixed_raw_masks_union, local_z_perimeter_mask_expand, true);

            for (size_t extruder_id = 0; extruder_id < plan_mask_slots; ++extruder_id) {
                const ExPolygons mixed_raw_masks =
                    extruder_id < plan.painted_masks_by_extruder.size() ? plan.painted_masks_by_extruder[extruder_id] : ExPolygons();
                const ExPolygons fixed_raw_masks =
                    extruder_id < plan.fixed_painted_masks_by_extruder.size() ? plan.fixed_painted_masks_by_extruder[extruder_id] :
                                                                               ExPolygons();
                if (mixed_raw_masks.empty() && fixed_raw_masks.empty())
                    continue;

                ExPolygons compensated;
                if (!mixed_raw_masks.empty()) {
                    ExPolygons compensated_mixed =
                        local_z_compensate_masks_for_wipe_tower(mixed_raw_masks, local_z_perimeter_mask_expand, true);
                    if (local_z_whole_objects_enabled && !fixed_compensated_guard.empty())
                        compensated_mixed = diff_ex(compensated_mixed, fixed_compensated_guard);
                    if (!compensated_mixed.empty())
                        append(compensated, compensated_mixed);
                }
                if (!fixed_raw_masks.empty())
                    append(compensated, fixed_raw_masks);
                if (!compensated.empty() && compensated.size() > 1)
                    compensated = union_ex(compensated);
                compensated_masks_by_extruder[extruder_id] = std::move(compensated);
            }

            LocalZWipeTowerPassRef pass_ref;
            pass_ref.layer_to_print_idx = layer_to_print_idx;
            pass_ref.plan               = &plan;
            for (size_t extruder_id = 0; extruder_id < plan.painted_masks_by_extruder.size(); ++extruder_id) {
                if (extruder_id >= compensated_masks_by_extruder.size())
                    continue;
                const ExPolygons &pass_masks = compensated_masks_by_extruder[extruder_id];
                if (pass_masks.empty())
                    continue;
                if (layer_has_local_z_extrusions(*layer_to_print.object_layer, pass_masks, local_z_infill_enabled))
                    pass_ref.extruders.push_back(unsigned(extruder_id));
            }

            if (!pass_ref.extruders.empty())
                pass_refs.emplace_back(std::move(pass_ref));
        }
    }

    std::sort(pass_refs.begin(), pass_refs.end(), [](const LocalZWipeTowerPassRef &lhs, const LocalZWipeTowerPassRef &rhs) {
        assert(lhs.plan != nullptr && rhs.plan != nullptr);
        if (lhs.plan->print_z != rhs.plan->print_z)
            return lhs.plan->print_z < rhs.plan->print_z;
        if (lhs.layer_to_print_idx != rhs.layer_to_print_idx)
            return lhs.layer_to_print_idx < rhs.layer_to_print_idx;
        return lhs.plan->pass_index < rhs.plan->pass_index;
    });

    auto collect_toolchanges_legacy = [&](int start_tool) {
        std::vector<LocalZWipeTowerToolchange> legacy_toolchanges;
        int active_extruder = start_tool;
        size_t pass_ref_idx = 0;
        while (pass_ref_idx < pass_refs.size()) {
            size_t pass_group_end = pass_ref_idx + 1;
            while (pass_group_end < pass_refs.size() &&
                   std::abs(pass_refs[pass_ref_idx].plan->print_z - pass_refs[pass_group_end].plan->print_z) <= EPSILON) {
                ++pass_group_end;
            }

            std::vector<unsigned int> pass_group_extruders;
            for (size_t group_idx = pass_ref_idx; group_idx < pass_group_end; ++group_idx)
                for (unsigned int extruder_id : pass_refs[group_idx].extruders)
                    if (std::find(pass_group_extruders.begin(), pass_group_extruders.end(), extruder_id) == pass_group_extruders.end())
                        pass_group_extruders.push_back(extruder_id);

            std::vector<unsigned int> next_group_extruders;
            if (pass_group_end < pass_refs.size()) {
                size_t next_group_end = pass_group_end + 1;
                while (next_group_end < pass_refs.size() &&
                       std::abs(pass_refs[pass_group_end].plan->print_z - pass_refs[next_group_end].plan->print_z) <= EPSILON) {
                    ++next_group_end;
                }
                for (size_t group_idx = pass_group_end; group_idx < next_group_end; ++group_idx)
                    for (unsigned int extruder_id : pass_refs[group_idx].extruders)
                        if (std::find(next_group_extruders.begin(), next_group_extruders.end(), extruder_id) == next_group_extruders.end())
                            next_group_extruders.push_back(extruder_id);
            }

            const int preferred_last_extruder =
                shared_local_z_extruder_for_wipe_tower(pass_group_extruders, next_group_extruders);
            const std::vector<unsigned int> ordered_group_extruders =
                LocalZOrderOptimizer::order_bucket_extruders(pass_group_extruders, active_extruder, preferred_last_extruder);

            for (unsigned int extruder_id : ordered_group_extruders) {
                if (active_extruder >= 0 && active_extruder != int(extruder_id))
                    legacy_toolchanges.push_back(LocalZWipeTowerToolchange{unsigned(active_extruder), extruder_id});
                active_extruder = int(extruder_id);
            }

            pass_ref_idx = pass_group_end;
        }

        return legacy_toolchanges;
    };

    const bool dependency_chain_mode =
        !pass_refs.empty() &&
        std::all_of(pass_refs.begin(), pass_refs.end(), [](const LocalZWipeTowerPassRef &pass_ref) {
            return pass_ref.plan != nullptr && pass_ref.plan->dependency_group != 0;
        });
    if (!dependency_chain_mode)
        return collect_toolchanges_legacy(start_extruder);

    struct ChainKey {
        size_t layer_to_print_idx { 0 };
        size_t dependency_group { 0 };

        bool operator<(const ChainKey &rhs) const
        {
            if (layer_to_print_idx != rhs.layer_to_print_idx)
                return layer_to_print_idx < rhs.layer_to_print_idx;
            return dependency_group < rhs.dependency_group;
        }
    };
    struct PassState {
        const LocalZWipeTowerPassRef *pass_ref { nullptr };
        std::vector<unsigned int>     remaining_extruders;
        size_t                        chain_idx { 0 };
        size_t                        chain_pos { 0 };
        bool                          ready { false };
        bool                          completed { false };
    };

    std::map<ChainKey, size_t> chain_index_by_key;
    std::vector<std::vector<size_t>> chains;
    std::vector<PassState>           pass_states;
    pass_states.reserve(pass_refs.size());
    for (const LocalZWipeTowerPassRef &pass_ref : pass_refs) {
        ChainKey chain_key { pass_ref.layer_to_print_idx, pass_ref.plan->dependency_group };
        auto [it_chain, inserted] = chain_index_by_key.emplace(chain_key, chains.size());
        if (inserted)
            chains.emplace_back();

        const size_t chain_idx = it_chain->second;
        const size_t pass_state_idx = pass_states.size();
        pass_states.push_back(PassState{ &pass_ref, pass_ref.extruders, chain_idx, 0, false, false });
        chains[chain_idx].push_back(pass_state_idx);
    }

    for (std::vector<size_t> &chain : chains) {
        std::sort(chain.begin(), chain.end(), [&pass_states](size_t lhs_idx, size_t rhs_idx) {
            const SubLayerPlan &lhs = *pass_states[lhs_idx].pass_ref->plan;
            const SubLayerPlan &rhs = *pass_states[rhs_idx].pass_ref->plan;
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

    auto pass_contains_extruder = [](const PassState &pass_state, unsigned int extruder_id) {
        return std::find(pass_state.remaining_extruders.begin(), pass_state.remaining_extruders.end(), extruder_id) !=
               pass_state.remaining_extruders.end();
    };

    auto choose_ready_extruder = [&](int active_extruder) -> int {
        std::vector<unsigned int> ready_extruders;
        for (const PassState &pass_state : pass_states) {
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
            for (const PassState &pass_state : pass_states) {
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

    std::vector<LocalZWipeTowerToolchange> toolchanges;
    int                                    active_extruder = start_extruder;
    size_t                                 completed_passes = 0;
    while (completed_passes < pass_states.size()) {
        const int chosen_extruder = choose_ready_extruder(active_extruder);
        if (chosen_extruder < 0) {
            BOOST_LOG_TRIVIAL(warning) << "Local-Z wipe tower dependency scheduler deadlocked, falling back"
                                       << " start_extruder=" << start_extruder
                                       << " pass_count=" << pass_refs.size();
            return collect_toolchanges_legacy(start_extruder);
        }

        if (active_extruder >= 0 && active_extruder != chosen_extruder)
            toolchanges.push_back(LocalZWipeTowerToolchange{unsigned(active_extruder), unsigned(chosen_extruder)});
        active_extruder = chosen_extruder;

        bool                completed_any = false;
        std::vector<size_t> newly_completed;
        for (size_t pass_state_idx = 0; pass_state_idx < pass_states.size(); ++pass_state_idx) {
            PassState &pass_state = pass_states[pass_state_idx];
            if (!pass_state.ready || pass_state.completed)
                continue;

            auto it_extruder = std::find(pass_state.remaining_extruders.begin(),
                                         pass_state.remaining_extruders.end(),
                                         unsigned(chosen_extruder));
            if (it_extruder == pass_state.remaining_extruders.end())
                continue;

            pass_state.remaining_extruders.erase(it_extruder);
            completed_any = true;
            if (pass_state.remaining_extruders.empty())
                newly_completed.push_back(pass_state_idx);
        }

        if (!completed_any) {
            BOOST_LOG_TRIVIAL(warning) << "Local-Z wipe tower dependency scheduler made no progress, falling back"
                                       << " start_extruder=" << start_extruder
                                       << " active_extruder=" << active_extruder
                                       << " chosen_extruder=" << chosen_extruder
                                       << " pass_count=" << pass_refs.size();
            return collect_toolchanges_legacy(start_extruder);
        }

        for (size_t pass_state_idx : newly_completed) {
            PassState &pass_state = pass_states[pass_state_idx];
            if (pass_state.completed)
                continue;

            pass_state.ready = false;
            pass_state.completed = true;
            ++completed_passes;

            const std::vector<size_t> &chain = chains[pass_state.chain_idx];
            const size_t next_chain_pos = pass_state.chain_pos + 1;
            if (next_chain_pos < chain.size())
                pass_states[chain[next_chain_pos]].ready = true;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Local-Z wipe tower dependency scheduler"
                            << " start_extruder=" << start_extruder
                            << " pass_count=" << pass_refs.size()
                            << " chain_count=" << chains.size()
                            << " toolchanges=" << toolchanges.size();
    return toolchanges;
}

} // namespace

//BBS
// ORCA: Now this is a parameter
//float Print::min_skirt_length = 0;

void Print::clear()
{
	std::scoped_lock<std::mutex> lock(this->state_mutex());
    // The following call should stop background processing if it is running.
    this->invalidate_all_steps();
	for (PrintObject *object : m_objects)
		delete object;
	m_objects.clear();
    m_print_regions.clear();
    m_model.clear_objects();
    // Ultra (dual-nozzle): drop any stale grouping result so a re-sliced job recomputes it.
    m_nozzle_group_result.reset();
}

// Ultra (dual-nozzle): classify each filament as model-only / support-only / hybrid from object extruder
// usage and support-filament assignments. Config-derived, cheap; feeds the grouping context.
std::vector<FilamentUsageType> Print::get_filament_usage_type() const
{
    std::vector<FilamentUsageType> filament_usage_types;
    std::set<int> model_filaments, support_filaments; // 0-based
    for (auto* obj : m_objects) {
        auto obj_filaments = obj->object_extruders();
        model_filaments.insert(obj_filaments.begin(), obj_filaments.end());
        int support_fil           = obj->config().support_filament - 1;
        int support_interface_fil = obj->config().support_interface_filament - 1;
        if (support_fil >= 0) support_filaments.insert(support_fil);
        if (support_interface_fil >= 0) support_filaments.insert(support_interface_fil);
    }
    for (int idx = 0; idx < m_config.filament_type.size(); ++idx) {
        bool is_model   = model_filaments.count(idx) > 0;
        bool is_support = support_filaments.count(idx) > 0;
        if (is_model && is_support)
            filament_usage_types.emplace_back(FilamentUsageType::Hybrid);
        else if (is_support)
            filament_usage_types.emplace_back(FilamentUsageType::SupportOnly);
        else
            filament_usage_types.emplace_back(FilamentUsageType::ModelOnly);
    }
    return filament_usage_types;
}

// Ultra (dual-nozzle): stubbed empty. The BBS versions derive per-nozzle physical/flow reachability from
// the filament_printable bitmask and the filament/extruder variant subsystem, neither of which is ported.
// Empty means the grouper imposes no hardware reachability constraint (color grouping still applies).
std::vector<std::set<int>> Print::get_physical_unprintable_filaments(const std::vector<unsigned int>& /*used_filaments*/) const
{
    return std::vector<std::set<int>>(m_config.nozzle_diameter.size());
}

std::map<int, std::set<NozzleVolumeType>> Print::get_filament_unprintable_flow(const std::vector<unsigned int>& /*used_filaments*/) const
{
    return {};
}

// Called by Print::apply().
// This method only accepts PrintConfig option keys.
bool Print::invalidate_state_by_config_options(const ConfigOptionResolver & /* new_config */, const std::vector<t_config_option_key> &opt_keys)
{
    if (opt_keys.empty())
        return false;

    // Cache the plenty of parameters, which influence the G-code generator only,
    // or they are only notes not influencing the generated G-code.
    static std::unordered_set<std::string> steps_gcode = {
        //BBS
        "additional_cooling_fan_speed",
        "reduce_crossing_wall",
        "max_travel_detour_distance",
        "printable_area",
        //BBS: add bed_exclude_area
        "bed_exclude_area",
        "thumbnail_size",
        "before_layer_change_gcode",
        "enable_pressure_advance",
        "pressure_advance",
        "enable_overhang_bridge_fan",
        "overhang_fan_speed",
        "overhang_fan_threshold",
        "slow_down_for_layer_cooling",
        "default_acceleration",
        "deretraction_speed",
        "close_fan_the_first_x_layers",
        "machine_end_gcode",
        "printing_by_object_gcode",
        "filament_end_gcode",
        // BBS (H2C rack): only read by change_filament_gcode (M620.11 O1 T<len>).
        "filament_retract_length_nc",
        // BBS: read only by the idle-nozzle pre-cooling / pre-heating of the G-code post-processor.
        "enable_pre_heating",
        "filament_pre_cooling_temperature",
        "filament_preheat_temperature_delta",
        "post_process",
        "extruder_clearance_height_to_rod",
        "extruder_clearance_height_to_lid",
        "extruder_clearance_radius",
        "nozzle_height",
        "extruder_colour",
        "extruder_offset",
        "filament_flow_ratio",
        "reduce_fan_stop_start_freq",
        "dont_slow_down_outer_wall",
        "fan_cooling_layer_time",
        "full_fan_speed_layer",
        "fan_kickstart",
        "fan_speedup_overhangs",
        "fan_speedup_time",
        "filament_colour",
        "filament_multi_colors",
        "filament_colour_mode",
        "default_filament_colour",
        "filament_diameter",
        "filament_density",
        "filament_cost",
        "filament_notes",
        "outer_wall_acceleration",
        "inner_wall_acceleration",
        "initial_layer_acceleration",
        "top_surface_acceleration",
        "bridge_acceleration",
        "travel_acceleration",
        "sparse_infill_acceleration",
        "internal_solid_infill_acceleration",
        // BBS
        "supertack_plate_temp_initial_layer",
        "cool_plate_temp_initial_layer",
        "textured_cool_plate_temp_initial_layer",
        "eng_plate_temp_initial_layer",
        "hot_plate_temp_initial_layer",
        "textured_plate_temp_initial_layer",
        "graphic_effect_plate_temp_initial_layer",
        "gcode_add_line_number",
        "layer_change_gcode",
        "time_lapse_gcode",
        "fan_min_speed",
        "fan_max_speed",
        "printable_height",
        "slow_down_min_speed",
        "max_volumetric_extrusion_rate_slope",
        "max_volumetric_extrusion_rate_slope_segment_length",
        "extrusion_rate_smoothing_external_perimeter_only",
        "reduce_infill_retraction",
        "filename_format",
        "retraction_minimum_travel",
        "retract_before_wipe",
        "retract_when_changing_layer",
        "retraction_length",
        "retract_length_toolchange",
        "z_hop",
        "travel_slope",
        "retract_lift_above",
        "retract_lift_below", 
        "retract_lift_enforce",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retraction_speed",
        "use_firmware_retraction",
        "slow_down_layer_time",
        "standby_temperature_delta",
        "preheat_time",
        "delta_temperature",
        "preheat_steps",
        "machine_start_gcode",
        "filament_start_gcode",
        "change_filament_gcode",
        "wipe",
        // BBS
        "wipe_distance",
        "curr_bed_type",
        "nozzle_volume",
        "nozzle_hrc",
        "required_nozzle_HRC",
        "upward_compatible_machine",
        "is_infill_first",
        // Orca
        "chamber_temperature",
        "thumbnails",
        "thumbnails_format",
        "seam_gap",
        "role_based_wipe_speed",
        "wipe_speed",
        "use_relative_e_distances",
        "accel_to_decel_enable",
        "accel_to_decel_factor",
        "wipe_on_loops",
        "wipe_inward",
        "wipe_inward_distance",
        "gcode_comments",
        "gcode_label_objects", 
        "exclude_object",
        "support_material_interface_fan_speed",
        "internal_bridge_fan_speed", // ORCA: Add support for separate internal bridge fan speed control
        "ironing_fan_speed",
        "single_extruder_multi_material_priming",
        "activate_air_filtration",
        "during_print_exhaust_fan_speed",
        "complete_print_exhaust_fan_speed",
        "activate_chamber_temp_control",
        "manual_filament_change",
        "disable_m73",
        "use_firmware_retraction",
        "enable_long_retraction_when_cut",
        "long_retractions_when_cut",
        "retraction_distances_when_cut",
        "filament_long_retractions_when_cut",
        "filament_retraction_distances_when_cut",
        // Edge: layer-time speed smoothing (G-code export only; no reslice).
        "layer_time_speed_smoothing",
        "layer_time_speed_max_variation",
        "layer_time_speed_max_speedup",
        "layer_time_speed_max_slowdown",
        "layer_time_speed_max_time_increase",
        "layer_time_speed_slowdown_scope"
    };

    static std::unordered_set<std::string> steps_ignore;

    std::vector<PrintStep> steps;
    std::vector<PrintObjectStep> osteps;
    bool invalidated = false;

    for (const t_config_option_key &opt_key : opt_keys) {
        if (steps_gcode.find(opt_key) != steps_gcode.end()) {
            // These options only affect G-code export or they are just notes without influence on the generated G-code,
            // so there is nothing to invalidate.
            steps.emplace_back(psGCodeExport);
        } else if (steps_ignore.find(opt_key) != steps_ignore.end()) {
            // These steps have no influence on the G-code whatsoever. Just ignore them.
        } else if (
               opt_key == "filament_volume_type"
            || opt_key == "nozzle_volume_type"
            || opt_key == "filament_flow_support"
            || opt_key == "process_flow_support"
            || opt_key == "printer_flow_support"
            || opt_key == "filament_flow_step_size") {
            // Snapmaker: switching a filament's flow variant changes the values read out of
            // flow-variant arrays without the arrays themselves changing.
            invalidated |= this->invalidate_all_steps();
        } else if (
               opt_key == "skirt_type"
            || opt_key == "skirt_loops"
            || opt_key == "skirt_speed"
            || opt_key == "skirt_height"
            || opt_key == "min_skirt_length"
            || opt_key == "single_loop_draft_shield"
            || opt_key == "draft_shield"
            || opt_key == "skirt_distance"
            || opt_key == "skirt_start_angle"
            || opt_key == "ooze_prevention"
            || opt_key == "wipe_tower_x"
            || opt_key == "wipe_tower_y"
            || opt_key == "wipe_tower_rotation_angle") {
            steps.emplace_back(psSkirtBrim);
        } else if (
               opt_key == "initial_layer_print_height"
            || opt_key == "nozzle_diameter"
            || opt_key == "filament_shrink"
            || opt_key == "filament_shrinkage_compensation_z"
            || opt_key == "resolution"
            || opt_key == "precise_z_height"
            || opt_key == "dithering_z_step_size"
            || opt_key == "dithering_local_z_mode"
            || opt_key == "dithering_local_z_whole_objects"
            || opt_key == "dithering_local_z_direct_multicolor"
            || opt_key == "dithering_step_painted_zones_only"
            || opt_key == "mixed_filament_gradient_mode"
            || opt_key == "mixed_filament_height_lower_bound"
            || opt_key == "mixed_filament_height_upper_bound"
            || opt_key == "mixed_filament_advanced_dithering"
            || opt_key == "mixed_filament_component_bias_enabled"
            || opt_key == "mixed_filament_surface_indentation"
            || opt_key == "mixed_filament_region_collapse"
            || opt_key == "mixed_filament_definitions"
            // Spiral Vase forces different kind of slicing than the normal model:
            // In Spiral Vase mode, holes are closed and only the largest area contour is kept at each layer.
            // Therefore toggling the Spiral Vase on / off requires complete reslicing.
            || opt_key == "spiral_mode"
            // Which of two overlapping parts carves the other is decided inside
            // slices_to_regions(), so toggling it has to re-slice the object.
            || opt_key == "enable_order_independent_overlap_carving") {
            osteps.emplace_back(posSlice);
        } else if (
               opt_key == "print_sequence"
            || opt_key == "filament_type"
            || opt_key == "chamber_temperature"
            || opt_key == "nozzle_temperature_initial_layer"
            || opt_key == "filament_minimal_purge_on_wipe_tower"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "filament_loading_speed"
            || opt_key == "filament_loading_speed_start"
            || opt_key == "filament_unloading_speed"
            || opt_key == "filament_unloading_speed_start"
            || opt_key == "filament_toolchange_delay"
            || opt_key == "filament_cooling_moves"
            || opt_key == "filament_stamping_loading_speed"
            || opt_key == "filament_stamping_distance"
            || opt_key == "filament_cooling_initial_speed"
            || opt_key == "filament_cooling_final_speed"
            || opt_key == "filament_ramming_parameters"
            || opt_key == "filament_multitool_ramming"
            || opt_key == "filament_multitool_ramming_volume"
            || opt_key == "filament_multitool_ramming_flow"
            || opt_key == "filament_max_volumetric_speed"
            || opt_key == "gcode_flavor"
            || opt_key == "single_extruder_multi_material"
            || opt_key == "nozzle_temperature"
            // BBS
            || opt_key == "supertack_plate_temp"
            || opt_key == "cool_plate_temp"
            || opt_key == "textured_cool_plate_temp"
            || opt_key == "eng_plate_temp"
            || opt_key == "hot_plate_temp"
            || opt_key == "textured_plate_temp" 
            || opt_key == "graphic_effect_plate_temp"
            || opt_key == "enable_prime_tower"
            || opt_key == "preload_all_filaments"
            || opt_key == "prime_tower_width"
            || opt_key == "prime_tower_brim_width"
            || opt_key == "first_layer_print_sequence"
            || opt_key == "other_layers_print_sequence"
            || opt_key == "other_layers_print_sequence_nums" 
            || opt_key == "toolchange_ordering"
            || opt_key == "toolchange_cyclic_order"
            || opt_key == "toolchange_cyclic_first_layer" 
            || opt_key == "wipe_tower_bridging"
            || opt_key == "wipe_tower_extra_flow"
            || opt_key == "wipe_tower_no_sparse_layers"
            || opt_key == "flush_volumes_matrix"
            || opt_key == "prime_volume"
            || opt_key == "prime_tower_brim_chamfer"
            || opt_key == "prime_tower_brim_chamfer_max_width"
            || opt_key == "flush_into_infill"
            || opt_key == "flush_into_support"
            || opt_key == "initial_layer_infill_speed"
            || opt_key == "travel_speed"
            || opt_key == "travel_speed_z"
            || opt_key == "initial_layer_speed"
            || opt_key == "initial_layer_travel_speed"
            || opt_key == "slow_down_layers"
            || opt_key == "idle_temperature" 
            || opt_key == "filament_tower_ironing_area"
            || opt_key == "wipe_tower_cone_angle"
            || opt_key == "wipe_tower_extra_spacing"
            || opt_key == "wipe_tower_max_purge_speed"
            || opt_key == "wipe_tower_wall_type"
            || opt_key == "wipe_tower_extra_rib_length"
            || opt_key == "wipe_tower_rib_width"
            || opt_key == "wipe_tower_fillet_wall"
            || opt_key == "wipe_tower_wall_gap"
            || opt_key == "wipe_tower_interface_temp"
            || opt_key == "wipe_tower_interface_run_in"
            || opt_key == "wipe_tower_interface_extra_prime"
            || opt_key == "wipe_tower_interface_trigger"
            || opt_key == "enable_tower_interface_features"
            || opt_key == "filament_tower_interface_print_temp"
            || opt_key == "filament_tower_interface_pre_extrusion_dist"
            || opt_key == "filament_tower_interface_pre_extrusion_length"
            || opt_key == "wipe_tower_filament"
            || opt_key == "wiping_volumes_extruders"
            || opt_key == "dithering_local_z_infill"
            || opt_key == "enable_filament_ramming"
            || opt_key == "purge_in_prime_tower"
            || opt_key == "z_offset"
            || opt_key == "support_multi_bed_types"
            // Chameleon brim: changes which extruder each brim run prints under
            // (Print::process's psSkirtBrim partition pass) and, when it fires, which
            // extruders psWipeTower's ToolOrdering must know about for layer 0 - both
            // steps need to rerun together, same as the other options in this branch.
            || opt_key == "brim_filament_source"
            ) {
            steps.emplace_back(psWipeTower);
            steps.emplace_back(psSkirtBrim);
            // Chameleon P2: chameleon_assign_support_interfaces (Print.cpp ~2434) gates
            // the interface-partition pass off for ByObject sequence, but a prior ByLayer
            // pass may have already partitioned support_fills / populated
            // SupportLayer::interface_by_extruder. Toggling print_sequence alone doesn't
            // undo that - the stale partitions survive on the still-valid SupportLayers
            // and keep flowing into ToolOrdering + GCode emission even though the gate
            // says the feature should be off. Force posSupportMaterial to re-run (in
            // either direction) for any object opted into cross-extruder matching
            // (v2.6: support_filament_matching is a plain bool now - the gate below is
            // `.value`, so this check mirrors it), so fresh layers are generated and the
            // gate's decision actually takes effect. Only objects that opted in
            // are checked; other objects on mixed plates are over-invalidated as a result
            // (osteps applies to all objects below), which is accepted rather than
            // threading a per-object condition through the loop.
            if (opt_key == "print_sequence"
                && std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObject *object) {
                       return object->config().support_filament_matching.value;
                   }))
                osteps.emplace_back(posSupportMaterial);
        } else if (opt_key == "filament_soluble"
                || opt_key == "filament_is_support"
                || opt_key == "independent_support_layer_height"
                || opt_key == "top_z_overrides_xy_distance") {
            steps.emplace_back(psWipeTower);
            // Soluble support interface / non-soluble base interface produces non-soluble interface layers below soluble interface layers.
            // Thus switching between soluble / non-soluble interface layer material may require recalculation of supports.
            //FIXME Killing supports on any change of "filament_soluble" is rough. We should check for each object whether that is necessary.
            osteps.emplace_back(posSupportMaterial);
            osteps.emplace_back(posSimplifySupportPath);
        } else if (
               opt_key == "initial_layer_line_width"
            || opt_key == "min_layer_height"
            || opt_key == "max_layer_height"
            //|| opt_key == "resolution"
            //BBS: when enable arc fitting, we must re-generate perimeter
            || opt_key == "enable_arc_fitting"
            || opt_key == "print_order"
            || opt_key == "wall_sequence") {
            osteps.emplace_back(posPerimeters);
            osteps.emplace_back(posEstimateCurledExtrusions);
            osteps.emplace_back(posInfill);
            osteps.emplace_back(posSupportMaterial);
			osteps.emplace_back(posSimplifyPath);
            osteps.emplace_back(posSimplifyInfill);
            osteps.emplace_back(posSimplifySupportPath);
            steps.emplace_back(psSkirtBrim);
        }
        else if (opt_key == "z_hop_types") {
            osteps.emplace_back(posDetectOverhangsForLift);
        } else {
            // for legacy, if we can't handle this option let's invalidate all steps
            //FIXME invalidate all steps of all objects as well?
            invalidated |= this->invalidate_all_steps();
            // Continue with the other opt_keys to possibly invalidate any object specific steps.
        }
    }

    sort_remove_duplicates(steps);
    for (PrintStep step : steps)
        invalidated |= this->invalidate_step(step);
    sort_remove_duplicates(osteps);
    for (PrintObjectStep ostep : osteps)
        for (PrintObject *object : m_objects)
            invalidated |= object->invalidate_step(ostep);

    return invalidated;
}

void Print::set_calib_params(const Calib_Params& params) {
    m_calib_params = params;
    m_calib_params.mode = params.mode;
}

bool Print::invalidate_step(PrintStep step)
{
	bool invalidated = Inherited::invalidate_step(step);
    // Propagate to dependent steps.
    if (step != psGCodeExport)
        invalidated |= Inherited::invalidate_step(psGCodeExport);
    return invalidated;
}

// returns true if an object step is done on all objects
// and there's at least one object
bool Print::is_step_done(PrintObjectStep step) const
{
    if (m_objects.empty())
        return false;
    std::scoped_lock<std::mutex> lock(this->state_mutex());
    for (const PrintObject *object : m_objects)
        if (! object->is_step_done_unguarded(step))
            return false;
    return true;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::object_extruders() const
{
    std::vector<unsigned int> extruders;
    extruders.reserve(m_print_regions.size() * m_objects.size() * 3);

    //Orca: Collect extruders from all regions.
    for (const PrintObject *object : m_objects)
		for (const PrintRegion &region : object->all_regions())
        	region.collect_object_printing_extruders(*this, extruders);

    for (const PrintObject* object : m_objects) {
        const ModelObject* mo = object->model_object();
        for (const ModelVolume* mv : mo->volumes) {
            std::vector<int> volume_extruders = mv->get_extruders();
            for (int extruder : volume_extruders) {
                assert(extruder > 0);
                extruders.push_back(extruder - 1);
            }
        }

        // layer range
        for (auto layer_range : mo->layer_config_ranges) {
            if (layer_range.second.has("extruder")) {
                //BBS: actually when user doesn't change filament by height range(value is default 0), height range should not save key "extruder".
                //Don't know why height range always save key "extruder" because of no change(should only save difference)...
                //Add protection here to avoid overflow
                auto value = layer_range.second.option("extruder")->getInt();
                if (value > 0)
                    extruders.push_back(value - 1);
            }
        }
    }
    sort_remove_duplicates(extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::support_material_extruders() const
{
    std::vector<unsigned int> extruders;
    bool support_uses_current_extruder = false;
    // BBS
    auto num_extruders = (unsigned int)m_config.filament_diameter.size();

    for (PrintObject *object : m_objects) {
        if (object->has_support_material()) {
        	assert(object->config().support_filament >= 0);
            if (object->config().support_filament == 0)
                support_uses_current_extruder = true;
            else {
            	unsigned int i = (unsigned int)object->config().support_filament - 1;
                extruders.emplace_back((i >= num_extruders) ? 0 : i);
            }
        	assert(object->config().support_interface_filament >= 0);
            if (object->config().support_interface_filament == 0)
                support_uses_current_extruder = true;
            else {
            	unsigned int i = (unsigned int)object->config().support_interface_filament - 1;
                extruders.emplace_back((i >= num_extruders) ? 0 : i);
            }
        }
    }

    if (support_uses_current_extruder)
        // Add all object extruders to the support extruders as it is not know which one will be used to print supports.
        append(extruders, this->object_extruders());

    sort_remove_duplicates(extruders);
    return extruders;
}

// returns 0-based indices of used extruders
std::vector<unsigned int> Print::extruders(bool conside_custom_gcode) const
{
    std::vector<unsigned int> extruders = this->object_extruders();
    append(extruders, this->support_material_extruders());

    if (conside_custom_gcode) {
        //BBS
        const size_t num_physical = m_config.filament_colour.size();
        const size_t num_filaments = m_mixed_filament_mgr.total_filaments(num_physical);
        if (m_model.plates_custom_gcodes.find(m_model.curr_plate_index) != m_model.plates_custom_gcodes.end()) {
            for (auto item : m_model.plates_custom_gcodes.at(m_model.curr_plate_index).gcodes) {
                if (item.type == CustomGCode::Type::ToolChange && item.extruder <= int(num_filaments))
                    extruders.push_back((unsigned int)(item.extruder - 1));
            }
        }
    }

    // The wipe tower extruder can also be set. When the wipe tower is enabled and it will be generated,
    // append its extruder into the list too.
    if (has_wipe_tower() && config().wipe_tower_filament != 0 && extruders.size() > 1) {
        assert(config().wipe_tower_filament > 0 && config().wipe_tower_filament < int(config().nozzle_diameter.size()));
        extruders.emplace_back(config().wipe_tower_filament - 1); // the config value is 1-based
    }

    sort_remove_duplicates(extruders);

    return extruders;
}

void Print::filament_rule_mismatch_flags(NozzleFilamentRuleMismatch& out_nozzle_mismatch,
                                         bool& out_gesp,
                                         bool& out_pei_not_pla,
                                         bool& out_pei_tpu,
                                         const PresetBundle* preset_bundle) const
{
    FilamentHotBedNozzleRules::singleton().ensure_loaded();
    const std::vector<unsigned int> used = extruders(true);
    FilamentHotBedNozzleRules&      rules = FilamentHotBedNozzleRules::singleton();
    out_nozzle_mismatch = NozzleFilamentRuleMismatch{};
    rules.evaluate_nozzle_filament_mismatch_detail(m_config, used, preset_bundle, out_nozzle_mismatch);

    out_gesp   = rules.evaluate_graphic_effect_bed_filament_mismatch(m_config, used);

    out_pei_tpu     = rules.evaluate_pei_bed_filament_mismatch_tpu(m_config, used);
    out_pei_not_pla = rules.evaluate_pei_bed_filament_mismatch_not_pla(m_config, used);
}

unsigned int Print::num_object_instances() const
{
	unsigned int instances = 0;
    for (const PrintObject *print_object : m_objects)
        instances += (unsigned int)print_object->instances().size();
    return instances;
}

double Print::max_allowed_layer_height() const
{
    double nozzle_diameter_max = 0.;
    for (unsigned int extruder_id : this->extruders())
        nozzle_diameter_max = std::max(nozzle_diameter_max, m_config.nozzle_diameter.get_at(extruder_id));
    return nozzle_diameter_max;
}

std::vector<ObjectID> Print::print_object_ids() const
{
    std::vector<ObjectID> out;
    // Reserve one more for the caller to append the ID of the Print itself.
    out.reserve(m_objects.size() + 1);
    for (const PrintObject *print_object : m_objects)
        out.emplace_back(print_object->id());
    return out;
}

bool Print::has_infinite_skirt() const
{
    // Orca: unclear why (m_config.ooze_prevention && this->extruders().size() > 1) logic is here, removed.
    // return (m_config.draft_shield == dsEnabled && m_config.skirt_loops > 0) || (m_config.ooze_prevention && this->extruders().size() > 1);

    return (m_config.draft_shield == dsEnabled && m_config.skirt_loops > 0);
}

bool Print::has_skirt() const
{
    return (m_config.skirt_height > 0);
}

bool Print::has_brim() const
{
    return std::any_of(m_objects.begin(), m_objects.end(), [](PrintObject *object) { return object->has_brim(); });
}

//BBS
std::vector<size_t> Print::layers_sorted_for_object(float start, float end, std::vector<LayerPtrs> &layers_of_objects, std::vector<BoundingBox> &boundingBox_for_objects, VecOfPoints &objects_instances_shift)
{
    std::vector<size_t> idx_of_object_sorted;
    size_t              idx = 0;
    for (const auto &object : m_objects) {
        idx_of_object_sorted.push_back(idx++);
        object->get_certain_layers(start, end, layers_of_objects, boundingBox_for_objects);
    }
    std::sort(idx_of_object_sorted.begin(), idx_of_object_sorted.end(),
              [boundingBox_for_objects](auto left, auto right) { return boundingBox_for_objects[left].area() > boundingBox_for_objects[right].area(); });

    objects_instances_shift.clear();
    objects_instances_shift.reserve(m_objects.size());
    for (const auto& object : m_objects)
        objects_instances_shift.emplace_back(object->get_instances_shift_without_plate_offset());

    return idx_of_object_sorted;
};

StringObjectException Print::sequential_print_clearance_valid(const Print &print, Polygons *polygons, std::vector<std::pair<Polygon, float>>* height_polygons)
{
    StringObjectException single_object_exception;
    const auto& print_config = print.config();
    Polygons exclude_polys = get_bed_excluded_area(print_config);
    const Vec3d print_origin = print.get_plate_origin();
    std::for_each(exclude_polys.begin(), exclude_polys.end(),
                  [&print_origin](Polygon& p) { p.translate(scale_(print_origin.x()), scale_(print_origin.y())); });

    std::map<ObjectID, Polygon> map_model_object_to_convex_hull;
    struct print_instance_info
    {
        const PrintInstance *print_instance;
        BoundingBox    bounding_box;
        Polygon        hull_polygon;
        int                  object_index;
        double         arrange_score;
        double               height;
        // Y extent (scaled) of the geometry that reaches above extruder_clearance_height_to_rod.
        // The X rod sits that far above the nozzle and spans the whole X axis, so this is the only
        // part of an already-printed instance it can collide with while a later instance prints.
        bool                 has_above_rod   = false;
        coord_t              above_rod_min_y = 0;
        coord_t              above_rod_max_y = 0;
    };
    // ORCA: Y extent of an instance's geometry above the rod height, in the same coordinates as the
    // bounding boxes below. Conservative: any triangle with a vertex above the rod contributes all
    // three of its vertices. Modifiers, blockers and negative volumes are not printed and are skipped.
    const double rod_height = print.config().extruder_clearance_height_to_rod.value;
    auto above_rod_y_range = [rod_height](const PrintInstance &instance, coord_t &min_y, coord_t &max_y) -> bool {
        double ymin = std::numeric_limits<double>::max();
        double ymax = std::numeric_limits<double>::lowest();
        bool   any  = false;
        const Transform3d inst_trafo = instance.model_instance->get_transformation().get_matrix();
        for (const ModelVolume *volume : instance.print_object->model_object()->volumes) {
            if (!volume->is_model_part())
                continue;
            const Transform3d           trafo = inst_trafo * volume->get_transformation().get_matrix();
            const indexed_triangle_set &its   = volume->mesh().its;
            std::vector<double> ys(its.vertices.size()), zs(its.vertices.size());
            for (size_t i = 0; i < its.vertices.size(); ++i) {
                const Vec3d p = trafo * its.vertices[i].cast<double>();
                ys[i] = p.y();
                zs[i] = p.z();
            }
            for (const auto &f : its.indices) {
                if (zs[f(0)] <= rod_height && zs[f(1)] <= rod_height && zs[f(2)] <= rod_height)
                    continue;
                for (int j = 0; j < 3; ++j) {
                    ymin = std::min(ymin, ys[f(j)]);
                    ymax = std::max(ymax, ys[f(j)]);
                }
                any = true;
            }
        }
        if (any) {
            min_y = scaled<coord_t>(ymin);
            max_y = scaled<coord_t>(ymax);
        }
        return any;
    };
    auto find_object_index = [](const Model& model, const ModelObject* obj) {
        for (int index = 0; index < model.objects.size(); index++)
        {
            if (model.objects[index] == obj)
                return index;
        }
        return -1;
    };

    auto [object_skirt_offset, _] = print.object_skirt_offset();
    std::vector<struct print_instance_info> print_instance_with_bounding_box;
    {
        // sequential_print_horizontal_clearance_valid
        Polygons convex_hulls_other;
        if (polygons != nullptr)
            polygons->clear();
        std::vector<size_t> intersecting_idxs;

        // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
        // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
        float obj_distance = print.is_all_objects_are_short() ? scale_(std::max(0.5f * MAX_OUTER_NOZZLE_DIAMETER, object_skirt_offset) - 0.1) : scale_(0.5 * print.config().extruder_clearance_radius.value + object_skirt_offset - 0.1);

        for (const PrintObject *print_object : print.objects()) {
            assert(! print_object->model_object()->instances.empty());
            assert(! print_object->instances().empty());
            ObjectID model_object_id = print_object->model_object()->id();
            auto it_convex_hull = map_model_object_to_convex_hull.find(model_object_id);
            // Get convex hull of all printable volumes assigned to this print object.
            ModelInstance *model_instance0 = print_object->model_object()->instances.front();
            if (it_convex_hull == map_model_object_to_convex_hull.end()) {
                // Calculate the convex hull of a printable object.
                // Grow convex hull with the clearance margin.
                // FIXME: Arrangement has different parameters for offsetting (jtMiter, limit 2)
                // which causes that the warning will be showed after arrangement with the
                // appropriate object distance. Even if I set this to jtMiter the warning still shows up.
                it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, model_object_id,
                            print_object->model_object()->convex_hull_2d(Geometry::assemble_transform(
                            { 0.0, 0.0, model_instance0->get_offset().z() }, model_instance0->get_rotation(), model_instance0->get_scaling_factor(), model_instance0->get_mirror())));
            }
            // Make a copy, so it may be rotated for instances.
            Polygon convex_hull0 = it_convex_hull->second;
            const double z_diff = Geometry::rotation_diff_z(model_instance0->get_rotation(), print_object->instances().front().model_instance->get_rotation());
            if (std::abs(z_diff) > EPSILON)
                convex_hull0.rotate(z_diff);
            // Now we check that no instance of convex_hull intersects any of the previously checked object instances.
            for (const PrintInstance &instance : print_object->instances()) {
                Polygon convex_hull_no_offset = convex_hull0, convex_hull;
                auto tmp = offset(convex_hull_no_offset, obj_distance, jtRound, scale_(0.1));
                if (!tmp.empty()) { // tmp may be empty due to clipper's bug, see STUDIO-2452
                    convex_hull = tmp.front();
                    // instance.shift is a position of a centered object, while model object may not be centered.
                    // Convert the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
                    convex_hull.translate(instance.shift - print_object->center_offset());
                }
                convex_hull_no_offset.translate(instance.shift - print_object->center_offset());
                //juedge the exclude area
                if (!intersection(exclude_polys, convex_hull_no_offset).empty()) {
                    if (single_object_exception.string.empty()) {
                        single_object_exception.string = (boost::format(L("%1% is too close to exclusion area, there may be collisions when printing.")) %instance.model_instance->get_object()->name).str();
                        single_object_exception.object = instance.model_instance->get_object();
                    }
                    else {
                        single_object_exception.string += "\n"+(boost::format(L("%1% is too close to exclusion area, there may be collisions when printing.")) %instance.model_instance->get_object()->name).str();
                        single_object_exception.object = nullptr;
                    }
                    //if (polygons) {
                    //    intersecting_idxs.emplace_back(convex_hulls_other.size());
                    //}
                }

                // if output needed, collect indices (inside convex_hulls_other) of intersecting hulls
                for (size_t i = 0; i < convex_hulls_other.size(); ++i) {
                    if (! intersection(convex_hulls_other[i], convex_hull).empty()) {
                        bool has_exception = false;
                        if (single_object_exception.string.empty()) {
                            single_object_exception.string = (boost::format(L("%1% is too close to others, and collisions may be caused.")) %instance.model_instance->get_object()->name).str();
                            single_object_exception.object = instance.model_instance->get_object();
                            has_exception                  = true;
                        }
                        else {
                            single_object_exception.string += "\n"+(boost::format(L("%1% is too close to others, and collisions may be caused.")) %instance.model_instance->get_object()->name).str();
                            single_object_exception.object = nullptr;
                            has_exception                  = true;
                        }

                        if (polygons) {
                            intersecting_idxs.emplace_back(i);
                            intersecting_idxs.emplace_back(convex_hulls_other.size());
                        }

                        if (has_exception) break;
                    }
                }
                struct print_instance_info print_info {&instance, convex_hull.bounding_box(), convex_hull};
                print_info.height = instance.print_object->height();
                print_info.object_index = find_object_index(print.model(), print_object->model_object());
                print_info.has_above_rod = above_rod_y_range(instance, print_info.above_rod_min_y, print_info.above_rod_max_y);
                print_instance_with_bounding_box.push_back(std::move(print_info));
                convex_hulls_other.emplace_back(std::move(convex_hull));
            }
        }
        if (!intersecting_idxs.empty()) {
            // use collected indices (inside convex_hulls_other) to update output
            std::sort(intersecting_idxs.begin(), intersecting_idxs.end());
            intersecting_idxs.erase(std::unique(intersecting_idxs.begin(), intersecting_idxs.end()), intersecting_idxs.end());
            for (size_t i : intersecting_idxs) {
                polygons->emplace_back(std::move(convex_hulls_other[i]));
            }
        }
    }

    // calc sort order
    double hc1              = scale_(print.config().extruder_clearance_height_to_lid); // height to lid
    double hc2              = scale_(print.config().extruder_clearance_height_to_rod); // height to rod
    double printable_height = scale_(print.config().printable_height);

    // ORCA: The previous approach here either trusted the raw object-list order verbatim
    // (which made "X is too tall, and collisions will be caused" a matter of luck depending on
    // which order objects happen to be declared in the file) or used a score-propagation
    // heuristic that wasn't guaranteed to converge and has been disabled since it was written.
    //
    // Only the *last*-printed instance gets the full printable_height allowance below; every
    // earlier instance is capped at extruder_clearance_height_to_lid (hc1), or the stricter
    // extruder_clearance_height_to_rod (hc2) if some instance printed after it overlaps it in Y.
    // That means: at most one instance may need the "last slot" (height > hc1), and among the
    // rest we need an order where an instance that reaches above hc2 is scheduled after every
    // instance whose footprint overlaps that above-the-rod part in Y. That's a topological sort over
    // a "must print before" constraint graph, not a fixed rule about list position.
    //
    // If a valid order exists we use it; if it doesn't (a genuine, unavoidable collision), we
    // fall back to the previous object-list order so the vertical-clearance check right below
    // this block still fires the same descriptive error it always has.
    {
        const size_t n = print_instance_with_bounding_box.size();

        std::vector<size_t> fallback_order(n);
        for (size_t i = 0; i < n; ++i) fallback_order[i] = i;
        std::stable_sort(fallback_order.begin(), fallback_order.end(),
            [&](size_t l, size_t r) {
                return print_instance_with_bounding_box[l].object_index < print_instance_with_bounding_box[r].object_index;
            });

        auto by_object_index = [&](size_t l, size_t r) {
            return print_instance_with_bounding_box[l].object_index < print_instance_with_bounding_box[r].object_index;
        };

        // Mirrors the vertical-clearance check below: only the part of the potentially-too-tall
        // instance that reaches above the rod can be hit, so that Y extent (shrunk by the same
        // 0.1 mm tolerance the footprint test uses) is tested against the other instance's
        // inflated footprint, i.e. everywhere the nozzle can be while that instance prints.
        auto overlaps_in_y = [&](size_t tall_idx, size_t other_idx) {
            const auto &A = print_instance_with_bounding_box[tall_idx];
            if (!A.has_above_rod)
                return false;
            auto &bbB = print_instance_with_bounding_box[other_idx].bounding_box;
            const coord_t tol = scaled<coord_t>(0.1);
            auto  inter_min = std::max(A.above_rod_min_y + tol, bbB.min.y());
            auto  inter_max = std::min(A.above_rod_max_y - tol, bbB.max.y());
            return inter_max - inter_min > 0;
        };

        std::vector<size_t> must_be_last;
        for (size_t i = 0; i < n; ++i)
            if (print_instance_with_bounding_box[i].height > hc1)
                must_be_last.push_back(i);

        bool solved = false;
        std::vector<size_t> solved_order;

        if (must_be_last.size() <= 1) {
            size_t last_idx = must_be_last.empty() ? size_t(-1) : must_be_last.front();

            // Edge b -> a means "b must be scheduled before a".
            std::vector<std::vector<size_t>> adj(n);
            std::vector<int> indegree(n, 0);
            bool infeasible = false;
            for (size_t a = 0; a < n && !infeasible; ++a) {
                if (a == last_idx) continue;
                if (!print_instance_with_bounding_box[a].has_above_rod) continue; // nothing reaches the rod: no constraint
                for (size_t b = 0; b < n; ++b) {
                    if (b == a) continue;
                    if (!overlaps_in_y(a, b)) continue;
                    if (b == last_idx) {
                        // 'a' can't tolerate the last-placed instance printing after it while
                        // overlapping it, and the last instance can't be moved earlier by
                        // definition (nothing else may be scheduled after it) -> unsolvable.
                        infeasible = true;
                        break;
                    }
                    adj[b].push_back(a);
                    indegree[a]++;
                }
            }

            if (!infeasible) {
                // Kahn's algorithm; ties are broken by original object_index so the result is
                // the minimal perturbation of the incoming order, not an arbitrary one.
                std::vector<size_t> ready;
                for (size_t i = 0; i < n; ++i)
                    if (i != last_idx && indegree[i] == 0)
                        ready.push_back(i);
                std::sort(ready.begin(), ready.end(), by_object_index);

                std::vector<size_t> order;
                order.reserve(n);
                while (!ready.empty()) {
                    size_t cur = ready.front();
                    ready.erase(ready.begin());
                    order.push_back(cur);
                    for (size_t nxt : adj[cur]) {
                        if (--indegree[nxt] == 0) {
                            auto pos = std::upper_bound(ready.begin(), ready.end(), nxt, by_object_index);
                            ready.insert(pos, nxt);
                        }
                    }
                }

                if (last_idx != size_t(-1))
                    order.push_back(last_idx);

                if (order.size() == n) {
                    solved = true;
                    solved_order = std::move(order);
                }
                // else: a cycle among mutually Y-overlapping, over-hc2 instances -> genuinely
                // unsolvable; fall through to the fallback order below.
            }
        }
        // else: more than one instance requires the single "last" slot -> genuinely unsolvable.

        {
            std::vector<print_instance_info> reordered;
            reordered.reserve(n);
            for (size_t idx : (solved ? solved_order : fallback_order))
                reordered.emplace_back(print_instance_with_bounding_box[idx]);
            print_instance_with_bounding_box = std::move(reordered);
        }

        for (auto &inst : print_instance_with_bounding_box)
            BOOST_LOG_TRIVIAL(debug) << "after sequential-print ordering, print_instance "
                                     << inst.print_instance->model_instance->get_object()->name
                                     << ", object_index: " << inst.object_index
                                     << ", height: " << inst.height
                                     << (solved ? " [reordered for clearance]" : " [object-list order]");
    }
    // sequential_print_vertical_clearance_valid
    {
        // Ignore the last instance printed.
        //print_instance_with_bounding_box.pop_back();
        /*bool has_interlaced_objects = false;
        for (int k = 0; k < print_instance_count; k++)
        {
            auto inst = print_instance_with_bounding_box[k].print_instance;
            auto bbox = print_instance_with_bounding_box[k].bounding_box;
            auto iy1 = bbox.min.y();
            auto iy2 = bbox.max.y();

            for (int i = 0; i < k; i++)
            {
                auto& p = print_instance_with_bounding_box[i].print_instance;
                auto bbox2 = print_instance_with_bounding_box[i].bounding_box;
                auto py1 = bbox2.min.y();
                auto py2 = bbox2.max.y();
                auto inter_min = std::max(iy1, py1); // min y of intersection
                auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
                if (inter_max - inter_min > 0) {
                    has_interlaced_objects = true;
                    break;
                }
            }
            if (has_interlaced_objects)
                break;
        }*/

        // if objects are not overlapped on y-axis, they will not collide even if they are taller than extruder_clearance_height_to_rod
        int print_instance_count = print_instance_with_bounding_box.size();
        std::map<const PrintInstance*, std::pair<Polygon, float>> too_tall_instances;
        for (int k = 0; k < print_instance_count; k++)
        {
            auto inst = print_instance_with_bounding_box[k].print_instance;
            // 只需要考虑喷嘴到滑杆的偏移量，这个比整个工具头的碰撞半径要小得多
            // Only the offset from the nozzle to the slide bar needs to be considered, which is much smaller than the collision radius of the entire tool head.
            // ORCA: and only the part of this instance that reaches above the rod can be hit by it while
            // a later instance prints, so that is the Y extent to test (see above_rod_y_range).
            const auto &info_k = print_instance_with_bounding_box[k];
            const coord_t iy1 = info_k.above_rod_min_y + scaled<coord_t>(0.1);
            const coord_t iy2 = info_k.above_rod_max_y - scaled<coord_t>(0.1);
            (const_cast<ModelInstance*>(inst->model_instance))->arrange_order = k+1;
            double height = (k == (print_instance_count - 1))?printable_height:hc1;
            /*if (has_interlaced_objects) {
                if ((k < (print_instance_count - 1)) && (inst->print_object->height() > hc2)) {
                    too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(hc2));
                }
            }
            else {
                if ((k < (print_instance_count - 1)) && (inst->print_object->height() > hc1)) {
                    too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(hc1));
                }
            }*/

            for (int i = k+1; info_k.has_above_rod && i < print_instance_count; i++)
            {
                auto& p = print_instance_with_bounding_box[i].print_instance;
                auto bbox2 = print_instance_with_bounding_box[i].bounding_box;
                auto py1 = bbox2.min.y();
                auto py2 = bbox2.max.y();
                auto inter_min = std::max(iy1, py1); // min y of intersection
                auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
                if (inter_max - inter_min > 0) {
                    height = hc2;
                    break;
                }
            }
            if (height < inst->print_object->max_z())
                too_tall_instances[inst] = std::make_pair(print_instance_with_bounding_box[k].hull_polygon, unscaled<double>(height));
        }

        if (too_tall_instances.size() > 0) {
            //return {, inst->model_instance->get_object()};
            for (auto& iter: too_tall_instances) {
                if (single_object_exception.string.empty()) {
                    single_object_exception.string = (boost::format(L("%1% is too tall, and collisions will be caused.")) %iter.first->model_instance->get_object()->name).str();
                    single_object_exception.object = iter.first->model_instance->get_object();
                }
                else {
                    single_object_exception.string += "\n" + (boost::format(L("%1% is too tall, and collisions will be caused.")) %iter.first->model_instance->get_object()->name).str();
                    single_object_exception.object = nullptr;
                }
                if (height_polygons)
                    height_polygons->emplace_back(std::move(iter.second));
            }
        }
    }

    return single_object_exception;
}

// ---------------------------------------------------------------------------------------------
// Clearance rule for a prime tower compacted by wipe_tower_no_sparse_layers.
// Ported from BambuStudio / Orca #15485 and adapted to Edge: no wrapping-detection gate,
// no prime_tower_lift_height (z_hop alone bounds the spiral), and no WipeTowerData::bbx /
// rib_offset — the generated mesh bottom or width/depth estimate is used instead.
// ---------------------------------------------------------------------------------------------

double compacted_tower_footprint_padding(const PrintConfig &config, double brim_width)
{
    // The brim is deposited material like any other and reaches past the wall on the first layer, so
    // the sweeping rod has to clear it too.
    //
    // On top of it, two effects make a nominal outline fall short of the printed tower on its low
    // corner even though it overshoots by millimetres on the high one: the tower can be re-centred
    // once its first-layer wall is known, and the precise check hulls extrusion centre lines, so the
    // deposited material reaches half a line width further still. Allowing a line width per side
    // covers both, which is what keeps an estimated footprint enclosing the real one and the
    // pre-slice check stricter than the precise one.
    return std::max(0., brim_width) + 2. * config.nozzle_diameter.get_at(0);
}

Polygons compacted_wipe_tower_rings(const CompactedTowerZone &zone, bool any_body_tier)
{
    Polygons rings = zone.grown_nozzle;
    if (any_body_tier)
        append(rings, zone.grown_body);
    return rings;
}

CompactedTowerZone compacted_wipe_tower_zone(const PrintConfig &config, const Polygon &tower_footprint)
{
    CompactedTowerZone zone;
    if (tower_footprint.points.empty())
        return zone;

    // Spiral Z-hop at wipe-tower entry (the G3 Z I J that GCodeWriter emits for a SpiralLift) starts on
    // the tower outline at a low Z. The spiral centre sits one radius away from the start point, so the
    // circle reaches 2 * radius beyond the outline. radius = lift / (2*pi*atan(travel_slope)) is the
    // same formula GCodeWriter uses; both are per filament, so take the widest any filament can make.
    double spiral_reach = 0.;
    for (size_t i = 0; i < config.z_hop.size(); ++i) {
        const double lift = std::min(double(config.z_hop.get_at(i)), 5.);
        if (lift < EPSILON)
            continue;
        const double slope = i < config.travel_slope.size() ? double(config.travel_slope.get_at(i)) : 0.;
        if (slope < EPSILON)
            continue;
        spiral_reach = std::max(spiral_reach, 2. * lift / (2. * PI * std::atan(slope)));
    }

    // Working footprint = outline grown by the spiral envelope. All later clearance tests use this, so
    // a travel that leaves the deposited wall at low Z is still treated as part of the tower.
    zone.hull = tower_footprint;
    if (spiral_reach > EPSILON) {
        const Polygons grown = offset(tower_footprint, float(scale_(spiral_reach)), jtRound, scale_(0.1));
        if (! grown.empty())
            zone.hull = Geometry::convex_hull(grown);
    }

    // The rod sweeps the whole X axis, so its keep-out band is the tower's Y span widened by half
    // the nozzle-to-rod offset per side (the instance carries the other half).
    zone.bbox_rod = zone.hull.bounding_box();
    zone.bbox_rod.offset(scale_(config.extruder_clearance_dist_to_rod.value * 0.5));

    // Horizontal clearance, mirroring the sequential print check down to how the distance is split:
    // there each of the two object hulls grows by half of extruder_clearance_radius, so the two
    // outlines touch exactly when the objects are the full radius apart. Splitting it the same way
    // here (half on the tower, half on the instance in compacted_wipe_tower_clearance) states the
    // same criterion, and it is what lets the plater draw both outlines: they meet at the instant the
    // check trips, instead of one of them being already buried inside the other. The smaller
    // MAX_OUTER_NOZZLE_DIAMETER tier is the bare nozzle cone, the only part narrow enough to sit
    // beside an object rising less than nozzle_height. The 0.2 mm shaved off is the same rounding
    // slack the sequential check applies, 0.1 mm per side. Both rings are built here; which one a
    // given object is measured against depends on its own height and is decided in
    // compacted_wipe_tower_clearance().
    zone.body_radius  = config.extruder_clearance_radius.value;
    zone.grown_body   = offset(zone.hull, float(scale_(compacted_tower_half_clearance(zone.body_radius))), jtRound, scale_(0.1));
    zone.grown_nozzle = offset(zone.hull, float(scale_(compacted_tower_half_clearance(MAX_OUTER_NOZZLE_DIAMETER))), jtRound, scale_(0.1));
    return zone;
}

CompactedTowerClearance compacted_wipe_tower_clearance(const PrintConfig &config, const CompactedTowerZone &zone,
                                                      const Polygon &inst_hull, double object_rise)
{
    BoundingBox inst_bbox = inst_hull.bounding_box();
    inst_bbox.offset(scale_(config.extruder_clearance_dist_to_rod.value * 0.5));

    // Only the Y span matters for the rod: it spans the whole X axis, so an object sharing the tower's
    // Y band passes under it however far apart the two are in X.
    const bool overlaps_in_y = std::min(inst_bbox.max.y(), zone.bbox_rod.max.y()) - std::max(inst_bbox.min.y(), zone.bbox_rod.min.y()) > 0;

    CompactedTowerClearance result;
    result.far_clearance = overlaps_in_y ? config.extruder_clearance_height_to_rod.value : config.extruder_clearance_height_to_lid.value;

    // The rod and the lid are the only obstacles once the object stands far enough away. Closer than
    // the toolhead radius it is the head body itself that hits the object, and it does so as soon as
    // the object rises past the nozzle cone, which is far below the rod.
    // The instance carries the other half of each clearance, the tower rings already hold the first
    // half; see compacted_wipe_tower_zone(). Both halves are needed for the verdict to mean
    // "a full radius apart", and drawing what is tested is what keeps the plater honest.
    //
    // Which tier applies is a property of this object alone: the head body sits above the nozzle cone,
    // so it cannot reach an object that stays below nozzle_height however close it stands, and however
    // tall the rest of the plate is.
    const bool object_is_short = object_rise <= double(config.nozzle_height.value) + EPSILON;
    result.body_clearance      = object_is_short ? double(MAX_OUTER_NOZZLE_DIAMETER) : zone.body_radius;

    const Polygons inst_near_nozzle = offset(inst_hull, float(scale_(compacted_tower_half_clearance(MAX_OUTER_NOZZLE_DIAMETER))), jtRound, scale_(0.1));
    const bool     near_nozzle      = ! intersection(zone.grown_nozzle, inst_near_nozzle).empty();
    result.near_body                = false;
    if (! object_is_short) {
        const Polygons inst_near_body = offset(inst_hull, float(scale_(compacted_tower_half_clearance(zone.body_radius))), jtRound, scale_(0.1));
        result.near_body              = ! intersection(zone.grown_body, inst_near_body).empty();
    }

    result.allowed_rise = result.far_clearance;
    if (near_nozzle)
        result.allowed_rise = 0.;
    else if (result.near_body)
        result.allowed_rise = std::min(result.far_clearance, double(config.nozzle_height.value));
    return result;
}

Polygon compacted_wipe_tower_offender_outline(const Polygon &inst_hull, double body_clearance)
{
    // Exactly the half-clearance the check grew this instance by, so the halo drawn around an object is
    // the very outline that was tested against the tower ring of the same tier. Passing the clearance
    // the object was actually judged on keeps a short object from being drawn with the wide ring it is
    // not subject to.
    const Polygons grown = offset(inst_hull, float(scale_(compacted_tower_half_clearance(body_clearance))), jtRound, scale_(0.1));
    return grown.empty() ? inst_hull : grown.front();
}

// Shared user-facing message for every compacted-tower clearance failure. Height-limit and too-close
// are the same class of layout violation under "No sparse layers", so they share one wording.
static std::string compacted_wipe_tower_clearance_error()
{
    return L("The relative position of the model and the prime tower does not meet the requirements of the \"No sparse layers\" feature. Please adjust their relative positions, lower the model height, or turn off \"No sparse layers\".");
}

// Convex hull of one print instance in bed coordinates, the same outline both compacted tower checks
// compare against the tower.
static Polygon compacted_tower_print_instance_hull(const PrintObject &object, const PrintInstance &instance)
{
    Points pts;
    for (const ModelVolume *v : object.model_object()->volumes) {
        if (! v->is_model_part())
            continue;
        Polygon hull = v->get_convex_hull_2d(Geometry::assemble_transform(Vec3d::Zero(), instance.model_instance->get_rotation(),
                                                                          instance.model_instance->get_scaling_factor(), instance.model_instance->get_mirror()));
        hull.translate(instance.shift - object.center_offset());
        append(pts, hull.points);
    }
    return pts.empty() ? Polygon() : Geometry::convex_hull(pts);
}

// Footprint the compacted prime tower is expected to occupy on the plate, in bed coordinates.
// Before psWipeTower has run there is no tower geometry at all, so this falls back to the same
// estimate the plater builds its preview box from. Answering while the user is still arranging the
// plate is the whole point of the pre-slice check, and an estimate is all that can be had then.
static Polygon estimated_compacted_wipe_tower_footprint(const Print &print)
{
    const PrintConfig &config        = print.config();
    const size_t       filaments_cnt = print.extruders().size();
    if (filaments_cnt == 0)
        return Polygon();

    const WipeTowerData &wtd = print.wipe_tower_data(filaments_cnt);

    // Once the tower has been generated the mesh bottom already carries the brim, in tower-local
    // scaled coordinates. Pad by the extrusion half-width only and map it to the bed.
    if (wtd.wipe_tower_mesh_data.has_value() && ! wtd.wipe_tower_mesh_data->bottom.points.empty()) {
        Polygon footprint = wtd.wipe_tower_mesh_data->bottom;
        const double padding = compacted_tower_footprint_padding(config, 0.);
        if (padding > EPSILON) {
            const Polygons grown = offset(footprint, float(scale_(padding)), jtRound, scale_(0.1));
            if (! grown.empty())
                footprint = Geometry::convex_hull(grown);
        }
        footprint.rotate(Geometry::deg2rad(config.wipe_tower_rotation_angle.value));
        footprint.translate(Point(scale_(config.wipe_tower_x.get_at(print.get_plate_index()) + print.get_plate_origin()(0)),
                                  scale_(config.wipe_tower_y.get_at(print.get_plate_index()) + print.get_plate_origin()(1))));
        return footprint;
    }

    double depth = wtd.depth;
    if (depth < EPSILON)
        return Polygon();
    // wipe_tower_data() already squares a rib tower into width; fall back to the configured width
    // (or depth for a rib wall) when the estimate has not run yet.
    double width = wtd.width > EPSILON ? double(wtd.width)
                                       : (config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib ? depth :
                                                                                                           double(config.prime_tower_width.value));
    const double brim    = double(wtd.brim_width);
    const double padding = compacted_tower_footprint_padding(config, brim);
    Vec2d        local_min(-padding, -padding);
    width += 2. * padding;
    depth += 2. * padding;

    const Eigen::Rotation2Dd rot(Geometry::deg2rad(config.wipe_tower_rotation_angle.value));
    const Vec2d              translate(config.wipe_tower_x.get_at(print.get_plate_index()) + print.get_plate_origin()(0),
                                       config.wipe_tower_y.get_at(print.get_plate_index()) + print.get_plate_origin()(1));

    Polygon footprint;
    for (const Vec2d &corner : { local_min,
                                 Vec2d(local_min.x() + width, local_min.y()),
                                 Vec2d(local_min.x() + width, local_min.y() + depth),
                                 Vec2d(local_min.x(),         local_min.y() + depth) }) {
        const Vec2d p = rot * corner + translate;
        footprint.points.emplace_back(scale_(p.x()), scale_(p.y()));
    }
    return footprint;
}

// Pre-slice counterpart of validate_compacted_wipe_tower_clearance(). It applies the very same
// clearance rule, but to an estimated tower footprint instead of the real tool-change extrusions,
// which is what lets it run from Print::validate() before anything has been sliced. Reporting through
// polygons / height_polygons rather than by throwing is what puts the collision area and the height
// limit plane on the plater, exactly the way sequential printing does it.
StringObjectException Print::compacted_wipe_tower_clearance_valid(const Print &print, Polygons *polygons, std::vector<std::pair<Polygon, float>> *height_polygons)
{
    const PrintConfig &config = print.config();
    if (! wipe_tower_sparse_layers_skipped(config) || config.print_sequence != PrintSequence::ByLayer || ! print.has_wipe_tower())
        return {};

    const CompactedTowerZone zone = compacted_wipe_tower_zone(config, estimated_compacted_wipe_tower_footprint(print));
    if (zone.empty())
        return {};

    StringObjectException exception;
    Polygons              offenders;
    bool                  body_tier_used = false;
    for (const PrintObject *object : print.objects()) {
        const double object_top = unscaled<double>(object->max_z());
        for (const PrintInstance &instance : object->instances()) {
            const Polygon inst_hull = compacted_tower_print_instance_hull(*object, instance);
            if (inst_hull.points.empty())
                continue;
            const CompactedTowerClearance clearance = compacted_wipe_tower_clearance(config, zone, inst_hull, object_top);
            body_tier_used                          = body_tier_used || compacted_tower_body_tier(clearance);
            // Every tier the precise check applies is applied here too, otherwise an object standing
            // within the toolhead radius would pass here and then be rejected mid-slice, which is the
            // one outcome this check exists to prevent. The compacted tower base is unknown before
            // slicing, so the rise is measured from the plate rather than from the tower top; that
            // overstates it by the tower's own height and makes this check err strict, never lax.
            if (object_top <= clearance.allowed_rise + EPSILON)
                continue;

            // Height-limit and too-close cases share one user-facing message: both mean the layout
            // violates the "No sparse layers" clearance rule, and the remedies are the same.
            const std::string msg = compacted_wipe_tower_clearance_error();
            if (exception.string.empty()) {
                exception.string = msg;
                exception.object = instance.model_instance;
            } else {
                // Same wording for every offender; keep a single copy and drop the object pointer.
                exception.object = nullptr;
            }
            const Polygon outline = compacted_wipe_tower_offender_outline(inst_hull, clearance.body_clearance);
            offenders.emplace_back(outline);
            if (height_polygons)
                height_polygons->emplace_back(outline, float(clearance.allowed_rise));
        }
    }

    // Draw the tower's keep-out ring alongside the offending objects, so the collision area reads as
    // "this object reaches into the space the toolhead needs around the tower" rather than as a lone
    // highlighted object. Emitted only on a real collision; the plater discards polygons otherwise.
    // Only the rings some object on this plate is actually measured against are drawn, so that a ring
    // and an object outline touching always means that object is over its limit.
    if (polygons && ! offenders.empty()) {
        append(*polygons, compacted_wipe_tower_rings(zone, body_tier_used));
        append(*polygons, offenders);
    }
    return exception;
}

// With wipe_tower_no_sparse_layers the tower only grows on layers that carry a real toolchange,
// so it ends up far below the object and the nozzle has to descend to it. While the nozzle sits
// down on the compacted tower the rod is at tower_z + extruder_clearance_height_to_rod, and it
// sweeps the tower's Y band across the whole X axis. Anything already printed above that line and
// sharing the band gets hit. Nearer than the toolhead radius the head body hits the object well before
// the rod does, which is the horizontal half of the same problem. The spiral Z-hop that opens a wipe-
// tower travel also leaves the extrusion outline at a low Z, so the footprint used here is the
// deposited hull grown by the spiral circle's maximum reach. This mirrors both clearance checks of
// sequential printing, except that the tower is revisited over and over, so every object is compared
// against it.
void Print::validate_compacted_wipe_tower_clearance() const
{
    // Nothing to check when the tower is not compacted: it then follows the object as usual and the
    // regular by-layer clearance check already covers it. Asking wipe_tower_sparse_layers_skipped()
    // rather than the raw option keeps this from rejecting plates whose tower is in fact full height.
    if (! wipe_tower_sparse_layers_skipped(m_config) || m_config.print_sequence != PrintSequence::ByLayer)
        return;

    const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes = m_wipe_tower_data.tool_changes;
    if (tool_changes.empty() || m_objects.empty())
        return;

    // Same accumulation the G-code emitter runs, so validation and output cannot disagree.
    const std::vector<float> tower_z = compute_compacted_wipe_tower_z(tool_changes, float(m_config.z_offset.value));

    // Wipe tower footprint: build it from the ACTUAL tool-change extrusions rather than the nominal
    // width x depth rectangle. With a rib/cone wall the printed wall bulges past the nominal box and
    // the first-layer brim reaches even further. The extrusion end-points are stored in the wipe-tower
    // local frame, so we map them to the bed frame with the same transform both Edge emitters apply:
    // rotate, then add the tower position (no rib_offset — Edge's append_tcr / append_tcr2 do not
    // inject one).
    const Eigen::Rotation2Dd wt_rot(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value));
    const Vec2d              wt_translate(m_config.wipe_tower_x.get_at(m_plate_index) + m_origin(0),
                                          m_config.wipe_tower_y.get_at(m_plate_index) + m_origin(1));
    auto to_bed = [&wt_rot, &wt_translate](const Vec2d &pt) {
        return Vec2d(wt_rot * pt + wt_translate);
    };

    Points tower_pts;
    for (const std::vector<WipeTower::ToolChangeResult> &layer : tool_changes) {
        if (layer.empty() || wipe_tower_layer_is_sparse(layer))
            continue;
        for (const WipeTower::ToolChangeResult &tcr : layer)
            for (size_t i = 0; i < tcr.extrusions.size(); ++i) {
                // A zero width marks a travel end-point. Keep it only when it opens a real extrusion, so
                // the hull covers the deposited material and nothing else; travels reach a bit further out
                // than the walls do.
                const WipeTower::Extrusion &e = tcr.extrusions[i];
                if (e.width == 0.f && (i + 1 == tcr.extrusions.size() || tcr.extrusions[i + 1].width == 0.f))
                    continue;
                const Vec2d p = to_bed(Vec2d(e.pos.x(), e.pos.y()));
                tower_pts.emplace_back(scale_(p.x()), scale_(p.y()));
            }
    }
    if (tower_pts.empty())
        return;

    const CompactedTowerZone zone = compacted_wipe_tower_zone(m_config, Geometry::convex_hull(tower_pts));
    if (zone.empty())
        return;

    for (const PrintObject *object : m_objects) {
        const double object_top = unscaled<double>(object->max_z());
        for (const PrintInstance &instance : object->instances()) {
            const Polygon inst_hull = compacted_tower_print_instance_hull(*object, instance);
            if (inst_hull.points.empty())
                continue;

            // Report the worst layer rather than the first offending one, it is the one that explains the
            // collision best. The rise has to be known before the clearance: it is what selects the
            // horizontal tier, the nozzle cone being out of the head body's reach.
            double max_rise = 0.;
            for (size_t i = 0; i < tool_changes.size(); ++i) {
                if (tool_changes[i].empty() || wipe_tower_layer_is_sparse(tool_changes[i]))
                    continue;
                // Nothing above the current layer exists yet, so a tall object only counts up to it.
                const double rise = std::min(object_top, double(tool_changes[i].front().print_z)) - tower_z[i];
                if (rise > max_rise)
                    max_rise = rise;
            }

            const CompactedTowerClearance clearance = compacted_wipe_tower_clearance(m_config, zone, inst_hull, max_rise);
            if (max_rise <= clearance.allowed_rise + EPSILON)
                continue;
            // Same wording as compacted_wipe_tower_clearance_valid(): height-limit and too-close
            // share one message, since both are layout violations of "No sparse layers".
            throw Slic3r::SlicingError(compacted_wipe_tower_clearance_error());
        }
    }
}

//BBS
static StringObjectException layered_print_cleareance_valid(const Print &print, StringObjectException *warning)
{
    std::vector<const PrintInstance*> print_instances_ordered = sort_object_instances_by_model_order(print, true);
    if (print_instances_ordered.size() < 1)
        return {};

    const auto& print_config = print.config();
    Polygons exclude_polys = get_bed_excluded_area(print_config);
    const Vec3d print_origin = print.get_plate_origin();
    std::for_each(exclude_polys.begin(), exclude_polys.end(),
                  [&print_origin](Polygon& p) { p.translate(scale_(print_origin.x()), scale_(print_origin.y())); });

    std::map<const PrintInstance*, Polygon> map_model_object_to_convex_hull;
    // sequential_print_horizontal_clearance_valid
    Polygons convex_hulls_other;
    for (int k = 0; k < print_instances_ordered.size(); k++)
    {
        auto& inst = print_instances_ordered[k];
        auto it_convex_hull = map_model_object_to_convex_hull.find(inst);
        // Get convex hull of all printable volumes assigned to this print object.
        const ModelInstance* model_instance0 = inst->model_instance;
        if (it_convex_hull == map_model_object_to_convex_hull.end()) {
            // Calculate the convex hull of a printable object.
            auto convex_hull0 = inst->print_object->model_object()->convex_hull_2d(
                Geometry::assemble_transform(Vec3d::Zero(), model_instance0->get_rotation(), model_instance0->get_scaling_factor(), model_instance0->get_mirror()));

            double z_diff = Geometry::rotation_diff_z(model_instance0->get_rotation(), inst->model_instance->get_rotation());
            if (std::abs(z_diff) > EPSILON)
                convex_hull0.rotate(z_diff);

            // instance.shift is a position of a centered object, while model object may not be centered.
            // Conver the shift from the PrintObject's coordinates into ModelObject's coordinates by removing the centering offset.
            convex_hull0.translate(inst->shift - inst->print_object->center_offset());

            it_convex_hull = map_model_object_to_convex_hull.emplace_hint(it_convex_hull, inst, convex_hull0);
        }
        Polygon& convex_hull = it_convex_hull->second;
        Polygons convex_hulls_temp;
        convex_hulls_temp.push_back(convex_hull);
        if (!intersection(convex_hulls_other, convex_hulls_temp).empty()) {
            if (warning) {
                warning->string = inst->model_instance->get_object()->name + L(" is too close to others, there may be collisions when printing.") + "\n";
                warning->object = inst->model_instance->get_object();
            }
        }
        if (!intersection(exclude_polys, convex_hull).empty()) {
            return {inst->model_instance->get_object()->name + L(" is too close to exclusion area, there may be collisions when printing.") + "\n", inst->model_instance->get_object()};
            /*if (warning) {
                warning->string = inst->model_instance->get_object()->name + L(" is too close to exclusion area, there may be collisions when printing.") + "\n";
                warning->object = inst->model_instance->get_object();
            }*/
        }
        convex_hulls_other.emplace_back(convex_hull);
    }

    //BBS: add the wipe tower check logic
    const PrintConfig &       config   = print.config();
    // Custom G-code tool changes (MultiAsSingle) build a real tower on a plate whose objects
    // all use one filament, so they have to be counted or the hull below collapses to a point.
    int                 filaments_count = print.extruders(true).size();
    int                 plate_index = print.get_plate_index();
    const Vec3d         plate_origin = print.get_plate_origin();
    float               x            = config.wipe_tower_x.get_at(plate_index) + plate_origin(0);
    float               y            = config.wipe_tower_y.get_at(plate_index) + plate_origin(1);
    float               a            = config.wipe_tower_rotation_angle.value;
    //float               v            = config.wiping_volume.value;

    // The estimate resolves the effective width (a rib wall squares the tower).
    const WipeTowerData &wipe_tower_estimate = print.wipe_tower_data(filaments_count);
    float                width               = wipe_tower_estimate.width;
    float                depth               = wipe_tower_estimate.depth;
    float                brim_width          = wipe_tower_estimate.brim_width;

    Polygons convex_hulls_temp;
    // No gate on "is there a tower": one that is not printed estimates to zero, so the hull
    // is degenerate and every check passes. Re-deriving it here missed towers printed with
    // no tool change to purge for (smooth timelapse / wrapping detection).
    if (width > EPSILON && depth > EPSILON) {
        Polygon wipe_tower_convex_hull = estimate_wipe_tower_first_layer_outline(config, resolve_wipe_tower_type(config), width, depth, wipe_tower_estimate.height);
        if (brim_width > EPSILON) {
            Polygons brimmed = offset(wipe_tower_convex_hull, float(scale_(brim_width)));
            if (!brimmed.empty())
                wipe_tower_convex_hull = brimmed.front();
        }
        wipe_tower_convex_hull.rotate(Geometry::deg2rad(a));
        wipe_tower_convex_hull.translate(Point(scale_(x), scale_(y)));
        convex_hulls_temp.push_back(wipe_tower_convex_hull);
    }
    if (!intersection(convex_hulls_other, convex_hulls_temp).empty()) {
        if (warning) {
            warning->string += L("Prime Tower") + L(" is too close to others, and collisions may be caused.\n");
        }
    }
    if (!intersection(exclude_polys, convex_hulls_temp).empty()) {
        /*if (warning) {
            warning->string += L("Prime Tower is too close to exclusion area, there may be collisions when printing.\n");
        }*/
        return {L("Prime Tower") + L(" is too close to exclusion area, and collisions will be caused.\n")};
    }

    return {};
}

bool Print::check_multi_filaments_compatibility(const std::vector<std::string>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (get_filament_temp_type(type) ==FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (get_filament_temp_type(type) == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return false;

    return true;
}

bool Print::is_filaments_compatible(const std::vector<int>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (type == FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (type == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return false;

    return true;
}
int Print::get_compatible_filament_type(const std::set<int>& filament_types)
{
    bool has_high_temperature_filament = false;
    bool has_low_temperature_filament = false;

    for (const auto& type : filament_types) {
        if (type == FilamentTempType::HighTemp)
            has_high_temperature_filament = true;
        else if (type == FilamentTempType::LowTemp)
            has_low_temperature_filament = true;
    }

    if (has_high_temperature_filament && has_low_temperature_filament)
        return HighLowCompatible;
    else if (has_high_temperature_filament)
        return HighTemp;
    else if (has_low_temperature_filament)
        return LowTemp;
    return HighLowCompatible;
}

//BBS: this function is used to check whether multi filament can be printed
StringObjectException Print::check_multi_filament_valid(const Print& print)
{
    auto print_config = print.config();
    std::vector<unsigned int> extruders = print.extruders();
    std::vector<std::string> filament_types;
    filament_types.reserve(extruders.size());

    for (const auto& extruder_idx : extruders)
        filament_types.push_back(print_config.filament_type.get_at(extruder_idx));

    if (!check_multi_filaments_compatibility(filament_types))
        return {L("Cannot print multiple filaments which have large difference of temperature together. Otherwise, the extruder and nozzle may be blocked or damaged during printing.")};

    return {std::string()};
}

// Orca: this g92e0 regex is used copied from PrusaSlicer
// Matches "G92 E0" with various forms of writing the zero and with an optional comment.
boost::regex regex_g92e0 { "^[ \\t]*[gG]92[ \\t]*[eE](0(\\.0*)?|\\.0+)[ \\t]*(;.*)?$" };

// Precondition: Print::validate() requires the Print::apply() to be called its invocation.
//BBS: refine seq-print validation logic
StringObjectException Print::validate(StringObjectException *warning, Polygons* collison_polygons, std::vector<std::pair<Polygon, float>>* height_polygons) const
{
    std::vector<unsigned int> extruders = this->extruders();
    unsigned int nozzles = m_config.nozzle_diameter.size();

    if (m_objects.empty())
        return {std::string()};

    if (extruders.empty())
        return { L("No extrusions under current settings.") };

    if (m_config.print_sequence == PrintSequence::ByObject) {
        if (m_config.timelapse_type == TimelapseType::tlSmooth)
            return {L("Smooth mode of timelapse is not supported when \"by object\" sequence is enabled.")};

        //BBS: refine seq-print validation logic
        auto ret = sequential_print_clearance_valid(*this, collison_polygons, height_polygons);
        if (!ret.string.empty()) {
            ret.type = STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT;
            return ret;
        }
    }
    else {
        //BBS
        auto ret = layered_print_cleareance_valid(*this, warning);
        if (!ret.string.empty()) {
            ret.type = STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT;
            return ret;
        }

        // Orca: a compacted prime tower drags the nozzle back down to the plate on every toolchange, so
        // tall objects collide with it much like they do in sequential printing. Checking it here rather
        // than only during slicing is what lets the plater show the collision area and the height limit
        // while the plate is still being arranged.
        ret = compacted_wipe_tower_clearance_valid(*this, collison_polygons, height_polygons);
        if (!ret.string.empty()) {
            ret.type = STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT;
            return ret;
        }
    }

    if (m_config.spiral_mode) {
        size_t total_copies_count = 0;
        for (const PrintObject* object : m_objects)
            total_copies_count += object->instances().size();
        // #4043
        if (total_copies_count > 1 && m_config.print_sequence != PrintSequence::ByObject)
            return {L("Please select \"By object\" print sequence to print multiple objects in spiral vase mode."), nullptr, "spiral_mode"};
        assert(m_objects.size() == 1);
        const auto all_regions = m_objects.front()->all_regions();
        if (all_regions.size() > 1) {
            // Orca: make sure regions are not compatible
            if (std::any_of(all_regions.begin() + 1, all_regions.end(), [ra = all_regions.front()](const auto rb) {
                return !Layer::is_perimeter_compatible(ra, rb);
            })) {
                return {L("The spiral vase mode does not work when an object contains more than one materials."), nullptr, "spiral_mode"};
            }
        }
    }

    // Cache of layer height profiles for checking:
    // 1) Whether all layers are synchronized if printing with wipe tower and / or unsynchronized supports.
    // 2) Whether layer height is constant for Organic supports.
    // 3) Whether build volume Z is not violated.
    std::vector<std::vector<coordf_t>> layer_height_profiles;
    auto layer_height_profile = [this, &layer_height_profiles](const size_t print_object_idx) -> const std::vector<coordf_t>& {
        const PrintObject       &print_object = *m_objects[print_object_idx];
        if (layer_height_profiles.empty())
            layer_height_profiles.assign(m_objects.size(), std::vector<coordf_t>());
        std::vector<coordf_t>   &profile      = layer_height_profiles[print_object_idx];
        if (profile.empty())
            PrintObject::update_layer_height_profile(*print_object.model_object(), print_object.slicing_parameters(), profile, &print_object);
        return profile;
    };

    // Checks that the print does not exceed the max print height
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx) {
        const PrintObject &print_object = *m_objects[print_object_idx];
        //FIXME It is quite expensive to generate object layers just to get the print height!
        if (auto layers = generate_object_layers(print_object.slicing_parameters(), layer_height_profile(print_object_idx), print_object.config().precise_z_height.value);
            !layers.empty()) {

            Vec3d test =this->shrinkage_compensation();
            const double shrinkage_compensation_z = this->shrinkage_compensation().z();
            
            if (shrinkage_compensation_z != 1. && layers.back() > (this->config().printable_height / shrinkage_compensation_z + EPSILON)) {
                // The object exceeds the maximum build volume height because of shrinkage compensation.
                return StringObjectException{
                    Slic3r::format(_u8L("While the object %1% itself fits the build volume, it exceeds the maximum build volume height because of material shrinkage compensation."), print_object.model_object()->name),
                    print_object.model_object(),
                    ""
                };
            } else if (layers.back() > this->config().printable_height + EPSILON) {
                // Test whether the last slicing plane is below or above the print volume.
                return StringObjectException{
                    0.5 * (layers[layers.size() - 2] + layers.back()) > this->config().printable_height + EPSILON ?
                    Slic3r::format(_u8L("The object %1% exceeds the maximum build volume height."), print_object.model_object()->name) :
                    Slic3r::format(_u8L("While the object %1% itself fits the build volume, its last layer exceeds the maximum build volume height."), print_object.model_object()->name) +
                    " " + _u8L("You might want to reduce the size of your model or change current print settings and retry."),
                    print_object.model_object(),
                    ""
                };
            }
        }
    }

    // Some of the objects has variable layer height applied by painting or by a table.
    bool has_custom_layering = std::find_if(m_objects.begin(), m_objects.end(), 
        [](const PrintObject *object) { return object->model_object()->has_custom_layering(); }) 
        != m_objects.end();

    // Custom layering is not allowed for tree supports as of now.
    for (size_t print_object_idx = 0; print_object_idx < m_objects.size(); ++ print_object_idx)
        if (const PrintObject &print_object = *m_objects[print_object_idx];
            print_object.has_support_material() && is_tree(print_object.config().support_type.value) && (print_object.config().support_style.value == smsTreeOrganic || 
                // Orca: use organic as default
                print_object.config().support_style.value == smsDefault) &&
            print_object.model_object()->has_custom_layering()) {
            if (const std::vector<coordf_t> &layers = layer_height_profile(print_object_idx); ! layers.empty())
                if (! check_object_layers_fixed(print_object.slicing_parameters(), layers))
                    return {_u8L("Variable layer height is not supported with Organic supports.") };
        }

    // H2 preload: the pre-changes purge into the prime tower, so refuse the combination up front
    // rather than silently doing nothing. Points the user at enable_prime_tower.
    if (m_config.preload_all_filaments.value && !m_config.enable_prime_tower.value)
        return {L("\"Preload all filaments\" requires the prime tower: every filament is loaded and purged into it "
                  "on the first layer. Enable the prime tower, or turn \"Preload all filaments\" off."),
                nullptr, "enable_prime_tower"};

    if (this->has_wipe_tower() && ! m_objects.empty()) {
        // Make sure all extruders use same diameter filament and have the same nozzle diameter
        // EPSILON comparison is used for nozzles and 10 % tolerance is used for filaments
        // Mixed nozzle sizes (Phase 2 gate): the prime tower is still generated at ONE line width -
        // WipeTower::set_extruder() overwrites the single m_perimeter_width scalar for every
        // registered filament, so a mixed-diameter job would print the tower at whichever nozzle
        // happened to register last. Refuse the combination with a message that names the way out
        // instead of warning and printing something wrong. Phase 3 makes the tower per-tool.
        auto nozzle_dmr_of = [this](unsigned int filament_id_0based) -> double {
            return m_config.nozzle_diameter.get_at(physical_extruder_for_filament(m_config, filament_id_0based + 1));
        };
        double first_nozzle_diam = nozzle_dmr_of(extruders.front());
        double first_filament_diam = m_config.filament_diameter.get_at(extruders.front());
        for (const auto& extruder_idx : extruders)
            if (std::abs(nozzle_dmr_of(extruder_idx) - first_nozzle_diam) > EPSILON)
                return {L("The prime tower does not support mixed nozzle diameters: it is generated at a single line "
                          "width for every filament. Turn the prime tower off, or use filaments that all print through "
                          "nozzles of the same diameter."), nullptr, "enable_prime_tower"};
        for (const auto& extruder_idx : extruders) {
            double filament_diam = m_config.filament_diameter.get_at(extruder_idx);
            if (warning && std::abs((filament_diam - first_filament_diam) / first_filament_diam) > 0.1) {
                warning->string = L("Different filament diameters may not work well when the prime tower is enabled. "
                                    "It's very experimental, so please proceed with caution.");
                warning->opt_key = "filament_diameter";
                break;
            }
        }

        if (! m_config.use_relative_e_distances)
            return { L("The Wipe Tower is currently only supported with the relative extruder addressing (use_relative_e_distances=1).") };

        if (m_config.ooze_prevention && m_config.single_extruder_multi_material)
            return {L("Ooze prevention is only supported with the wipe tower when 'single_extruder_multi_material' is off.")};
            
#if 0
        if (m_config.gcode_flavor != gcfRepRapSprinter && m_config.gcode_flavor != gcfRepRapFirmware &&
            m_config.gcode_flavor != gcfRepetier && m_config.gcode_flavor != gcfMarlinLegacy && m_config.gcode_flavor != gcfMarlinFirmware)
            return { L("The prime tower is currently only supported for the Marlin, RepRap/Sprinter, RepRapFirmware and Repetier G-code flavors.")};

        if ((m_config.print_sequence == PrintSequence::ByObject) && extruders.size() > 1)
            return { L("The prime tower is not supported in \"By object\" print."), nullptr, "enable_prime_tower" };

        // BBS: When prime tower is on, object layer and support layer must be aligned. So support gap should be multiple of object layer height.
        for (size_t i = 0; i < m_objects.size(); i++) {
            const PrintObject* object = m_objects[i];
            const SlicingParameters& slicing_params = object->slicing_parameters();
            if (object->config().adaptive_layer_height) {
                return  { L("The prime tower is not supported when adaptive layer height is on. It requires that all objects have the same layer height."), object, "adaptive_layer_height" };
            }

            if (!object->config().enable_support)
                continue;

            double gap_layers = slicing_params.gap_object_support / slicing_params.layer_height;
            if (gap_layers - (int)gap_layers > EPSILON) {
                return {L("The prime tower requires \"support gap\" to be multiple of layer height."), object};
            }
        }
#endif

        if (m_objects.size() > 1) {
            const SlicingParameters &slicing_params0 = m_objects.front()->slicing_parameters();
            size_t                  tallest_object_idx = 0;
            for (size_t i = 1; i < m_objects.size(); ++ i) {
                const PrintObject       *object         = m_objects[i];
                const SlicingParameters &slicing_params = object->slicing_parameters();
                if (std::abs(slicing_params.first_print_layer_height - slicing_params0.first_print_layer_height) > EPSILON ||
                    std::abs(slicing_params.layer_height             - slicing_params0.layer_height            ) > EPSILON)
                    return {L("The prime tower requires that all objects have the same layer heights."), object, "initial_layer_print_height"};
                if (slicing_params.raft_layers() != slicing_params0.raft_layers())
                    return {L("The prime tower requires that all objects are printed over the same number of raft layers."), object, "raft_layers"};
                // BBS: support gap can be multiple of object layer height, remove _L()
#if 0
                if (slicing_params0.gap_object_support != slicing_params.gap_object_support ||
                    slicing_params0.gap_support_object != slicing_params.gap_support_object)
                    return {L("The prime tower is only supported for multiple objects if they are printed with the same support_top_z_distance."), object};
#endif
                if (!equal_layering(slicing_params, slicing_params0))
                    return  { L("The prime tower requires that all objects are sliced with the same layer heights."), object };
                if (has_custom_layering) {
                    auto &lh         = layer_height_profile(i);
                    auto &lh_tallest = layer_height_profile(tallest_object_idx);
                    if (*(lh.end() - 2) > *(lh_tallest.end() - 2))
                        tallest_object_idx = i;
                }
            }

            // BBS: remove obsolete logics and _L()
            if (has_custom_layering) {
                std::vector<std::vector<coordf_t>> layer_z_series;
                layer_z_series.assign(m_objects.size(), std::vector<coordf_t>());
               
                for (size_t idx_object = 0; idx_object < m_objects.size(); ++idx_object) {
                    layer_z_series[idx_object] = generate_object_layers(m_objects[idx_object]->slicing_parameters(), layer_height_profiles[idx_object], m_objects[idx_object]->config().precise_z_height.value);
                }

                for (size_t idx_object = 0; idx_object < m_objects.size(); ++idx_object) {
                    if (idx_object == tallest_object_idx) continue;
                    // Check that the layer height profiles are equal. This will happen when one object is
                    // a copy of another, or when a layer height modifier is used the same way on both objects.
                    // The latter case might create a floating point inaccuracy mismatch, so compare
                    // element-wise using an epsilon check.
                    size_t         i   = 0;
                    const coordf_t eps = 0.5 * EPSILON; // layers closer than EPSILON will be merged later. Let's make
                    // this check a bit more sensitive to make sure we never consider two different layers as one.
                    while (i < layer_height_profiles[idx_object].size() && i < layer_height_profiles[tallest_object_idx].size()) {
                        // BBS: remove the break condition, because a variable layer height object and a new object will not be checked when slicing
                        //if (i % 2 == 0 && layer_height_profiles[tallest_object_idx][i] > layer_height_profiles[idx_object][layer_height_profiles[idx_object].size() - 2])
                        //    break;
                        if (std::abs(layer_height_profiles[idx_object][i] - layer_height_profiles[tallest_object_idx][i]) > eps)
                            return {L("The prime tower is only supported if all objects have the same variable layer height.")};
                        ++i;
                    }
                }
            }
        }
    }

	{
        // Mixed nozzle sizes (Phase 1): `extruders` holds 0-based FILAMENT indices. The diameter a
        // filament prints at is the diameter of the nozzle it is loaded into, which on a machine with
        // more filaments than nozzles is only known through filament_map - indexing nozzle_diameter
        // by the filament number silently returned the first nozzle for every filament past the last.
        auto nozzle_dmr_of_filament0 = [this](unsigned int filament_id_0based) -> double {
            return m_config.nozzle_diameter.get_at(physical_extruder_for_filament(m_config, filament_id_0based + 1));
        };
		// Find the smallest used nozzle diameter and the number of unique nozzle diameters.
		double min_nozzle_diameter = std::numeric_limits<double>::max();
		double max_nozzle_diameter = 0;
		for (unsigned int extruder_id : extruders) {
			double dmr = nozzle_dmr_of_filament0(extruder_id);
			min_nozzle_diameter = std::min(min_nozzle_diameter, dmr);
			max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
		}
        (void)max_nozzle_diameter;

        // BBS: remove L()
#if 0
        // We currently allow one to assign extruders with a higher index than the number
        // of physical extruders the machine is equipped with, as the Printer::apply() clamps them.
        unsigned int total_extruders_count = m_config.nozzle_diameter.size();
        for (const auto& extruder_idx : extruders)
            if ( extruder_idx >= total_extruders_count )
                return {L("One or more object were assigned an extruder that the printer does not have.")};
#endif

        // Mixed nozzle sizes (Phase 1b): the bounds are measured against the diameter of the nozzle
        // that prints THIS feature, not against the whole print's smallest/largest nozzle. On a
        // single-diameter machine every caller passes the same number the old code used, so this is
        // a no-op there.
        auto validate_extrusion_width = [](double extrusion_width, double nozzle_diameter, double layer_height, std::string &err_msg) -> bool {
            if (extrusion_width == 0) {
                // Default "auto-generated" extrusion width is always valid.
            } else if (extrusion_width <= layer_height) {
                err_msg = L("Too small line width");
                return false;
            } else if (extrusion_width > nozzle_diameter * MAX_LINE_WIDTH_MULTIPLIER) {
                err_msg = L("Too large line width");
                return false;
            }
            return true;
        };

        // Mixed nozzle sizes (Phase 1c): a feature may not ask its own nozzle for a line wider than
        // MAX_FEATURE_WIDTH_TO_NOZZLE_RATIO x that nozzle. Nothing checked this before: the only
        // upper bound was 5 x the COARSEST nozzle of the print, so a 0.62 mm outer wall routed to a
        // 0.2 mm head sliced silently. The message names the feature and the filament so the user
        // knows which of the two to change.
        auto validate_feature_width_vs_nozzle = [](const char *opt_key, unsigned int filament_id, double extrusion_width, double nozzle_diameter, std::string &err_msg) -> bool {
            if (extrusion_width <= 0. || nozzle_diameter <= 0.)
                return true;
            if (extrusion_width <= nozzle_diameter * MAX_FEATURE_WIDTH_TO_NOZZLE_RATIO + EPSILON)
                return true;
            const ConfigOptionDef *def = print_config_def.get(opt_key);
            char buf_w[32], buf_n[32], buf_k[32];
            snprintf(buf_w, sizeof(buf_w), "%.3f", extrusion_width);
            snprintf(buf_n, sizeof(buf_n), "%.2f", nozzle_diameter);
            snprintf(buf_k, sizeof(buf_k), "%.1f", double(MAX_FEATURE_WIDTH_TO_NOZZLE_RATIO));
            err_msg = (boost::format(L("Line width of %1% (%2% mm) is more than %3% x the diameter of the nozzle that prints it "
                                       "(filament %4%, nozzle %5% mm). Reduce the line width, or print this feature with a coarser nozzle."))
                       % (def ? def->label : std::string(opt_key)) % buf_w % buf_k % filament_id % buf_n).str();
            return false;
        };
        for (PrintObject *object : m_objects) {
            if (object->has_support_material()) {
                // BBS: remove useless logics and L()
#if 0
				if ((object->config().support_filament == 0 || object->config().support_interface_filament == 0) && max_nozzle_diameter - min_nozzle_diameter > EPSILON) {
                    // The object has some form of support and either support_filament or support_interface_filament
                    // will be printed with the current tool without a forced tool change. Play safe, assert that all object nozzles
                    // are of the same diameter.
                    return {L("Printing with multiple extruders of differing nozzle diameters. "
                           "If support is to be printed with the current filament (support_filament == 0 or support_interface_filament == 0), "
                           "all nozzles have to be of the same diameter."), object, "support_filament"};
                }
#endif

                // BBS
#if 0
                if (this->has_wipe_tower() && object->config().independent_support_layer_height) {
                    return {L("The prime tower requires that support has the same layer height with object."), object, "support_filament"};
                }
#endif

                // Prusa: Fixing crashes with invalid tip diameter or branch diameter
                // https://github.com/prusa3d/PrusaSlicer/commit/96b3ae85013ac363cd1c3e98ec6b7938aeacf46d
                if (is_tree(object->config().support_type.value) && (object->config().support_style == smsTreeOrganic ||
                    // Orca: use organic as default
                    object->config().support_style == smsDefault)) {
                    float extrusion_width = std::min(
                        support_material_flow(object).width(),
                        support_material_interface_flow(object).width());
                    if (object->config().tree_support_tip_diameter < extrusion_width - EPSILON)
                        return { L("Organic support tree tip diameter must not be smaller than support material extrusion width."), object, "tree_support_tip_diameter" };
                    if (object->config().tree_support_branch_diameter_organic < 2. * extrusion_width - EPSILON)
                        return { L("Organic support branch diameter must not be smaller than 2x support material extrusion width."), object, "tree_support_branch_diameter_organic" };
                    if (object->config().tree_support_branch_diameter_organic < object->config().tree_support_tip_diameter)
                        return { L("Organic support branch diameter must not be smaller than support tree tip diameter."), object, "tree_support_branch_diameter_organic" };
                }
            }

            // Do we have custom support data that would not be used?
            // Notify the user in that case.
            if (! object->has_support() && warning) {
                for (const ModelVolume* mv : object->model_object()->volumes) {
                    bool has_enforcers = mv->is_support_enforcer() ||
                        (mv->is_model_part() && mv->supported_facets.has_facets(*mv, EnforcerBlockerType::ENFORCER));
                    if (has_enforcers) {
                        warning->string = L("Support enforcers are used but support is not enabled. Please enable support.");
                        warning->object = object;
                        break;
                    }
                }
            }

            // Mixed nozzle sizes (Phase 1a): the layer-height ceiling comes from the nozzles THIS
            // object actually uses, not from the smallest nozzle anywhere on the printer. An object
            // that never touches the 0.2 mm head must not be clamped to 0.2 mm because some other
            // object (or an idle extruder) has one.
            std::vector<unsigned int> object_filaments = object->object_extruders();   // 0-based filament ids
            if (object->has_support_material()) {
                const int sup  = object->config().support_filament.value;
                const int supi = object->config().support_interface_filament.value;
                if (sup > 0)
                    object_filaments.emplace_back(unsigned(sup - 1));
                if (supi > 0)
                    object_filaments.emplace_back(unsigned(supi - 1));
                if (sup == 0 || supi == 0)
                    // "use whatever filament is current": any filament of the print may print it.
                    append(object_filaments, extruders);
            }
            if (this->has_wipe_tower())
                // The prime tower shares this object's layer grid and every filament purges into it.
                append(object_filaments, extruders);
            sort_remove_duplicates(object_filaments);
            if (object_filaments.empty())
                object_filaments = extruders;

            double object_min_nozzle_diameter = std::numeric_limits<double>::max();
            for (unsigned int filament_id : object_filaments)
                object_min_nozzle_diameter = std::min(object_min_nozzle_diameter, nozzle_dmr_of_filament0(filament_id));

            double initial_layer_print_height = m_config.initial_layer_print_height.value;
            double first_layer_min_nozzle_diameter;
            if (object->has_raft()) {
                // if we have raft layers, only support material extruder is used on first layer
                int first_layer_filament = object->config().raft_layers == 1
                    ? object->config().support_interface_filament.value
                    : object->config().support_filament.value;
                first_layer_min_nozzle_diameter = (first_layer_filament <= 0) ?
                    object_min_nozzle_diameter :
                    nozzle_dmr_of_filament0(unsigned(first_layer_filament - 1));
            } else {
                // if we don't have raft layers, any nozzle diameter of this object is potentially used in first layer
                first_layer_min_nozzle_diameter = object_min_nozzle_diameter;
            }
            if (initial_layer_print_height > first_layer_min_nozzle_diameter)
                return {L("Layer height cannot exceed nozzle diameter."), object, "initial_layer_print_height"};

            // validate layer_height
            double layer_height = object->config().layer_height.value;
            if (layer_height > object_min_nozzle_diameter)
                return {L("Layer height cannot exceed nozzle diameter."), object, "layer_height"};

            // Validate extrusion widths, each against the nozzle that prints it.
            std::string err_msg;
            if (!validate_extrusion_width(object->config().get_abs_value("line_width", object_min_nozzle_diameter),
                                          object_min_nozzle_diameter, layer_height, err_msg))
                return {err_msg, object, "line_width"};
            if (object->has_support() || object->has_raft()) {
                const int sup  = object->config().support_filament.value;
                const int supi = object->config().support_interface_filament.value;
                // The support line width has to fit the finer of the two support nozzles; filament 0
                // ("current filament") could be any of the object's, so fall back to its minimum.
                double support_nozzle_diameter = std::min(nozzle_dmr_of_filament0(unsigned(std::max(sup, 1) - 1)),
                                                          nozzle_dmr_of_filament0(unsigned(std::max(supi, 1) - 1)));
                if (sup == 0 || supi == 0)
                    support_nozzle_diameter = std::min(support_nozzle_diameter, object_min_nozzle_diameter);
                double support_width = object->config().get_abs_value("support_line_width", support_nozzle_diameter);
                if (support_width == 0.)
                    support_width = object->config().get_abs_value("line_width", support_nozzle_diameter);
                if (!validate_extrusion_width(support_width, support_nozzle_diameter, layer_height, err_msg))
                    return {err_msg, object, "support_line_width"};
                if (!validate_feature_width_vs_nozzle("support_line_width", unsigned(std::max(sup, supi)), support_width,
                                                      support_nozzle_diameter, err_msg))
                    return {err_msg, object, "support_line_width"};
            }
            // Each width option is bounded against the nozzle of the filament its own FlowRole is
            // routed to (PrintRegion::extruder), the same mapping PrintRegion::flow() uses to size
            // the extrusion. skin/skeleton infill ride along with sparse infill.
            static const std::pair<const char *, FlowRole> s_width_roles[] = {
                { "inner_wall_line_width",            frPerimeter },
                { "outer_wall_line_width",            frExternalPerimeter },
                { "sparse_infill_line_width",         frInfill },
                { "internal_solid_infill_line_width", frSolidInfill },
                { "top_surface_line_width",           frTopSolidInfill },
                { "skin_infill_line_width",           frInfill },
                { "skeleton_infill_line_width",       frInfill },
            };
            for (const auto &width_role : s_width_roles)
                for (const PrintRegion &region : object->all_regions()) {
                    const unsigned int filament_id     = region.extruder(width_role.second);
                    const double       nozzle_diameter = nozzle_dmr_of_filament0(unsigned(std::max<int>(int(filament_id), 1) - 1));
                    // A role width left at 0 falls back to the object's line_width, exactly as
                    // PrintRegion::flow() does, so validate the width that will really be used.
                    double width = region.config().get_abs_value(width_role.first, nozzle_diameter);
                    if (width == 0.)
                        width = object->config().get_abs_value("line_width", nozzle_diameter);
                    if (!validate_extrusion_width(width, nozzle_diameter, layer_height, err_msg))
                        return {err_msg, object, width_role.first};
                    if (!validate_feature_width_vs_nozzle(width_role.first, filament_id, width, nozzle_diameter, err_msg))
                        return {err_msg, object, width_role.first};
                }
        }
    }

    // Orca: G92 E0 is not supported when using absolute extruder addressing
    // This check is copied from PrusaSlicer, the original author is Vojtech Bubnik
    if(!is_BBL_printer()) {
        bool before_layer_gcode_resets_extruder =
            boost::regex_search(m_config.before_layer_change_gcode.value, regex_g92e0);
        bool layer_gcode_resets_extruder = boost::regex_search(m_config.layer_change_gcode.value, regex_g92e0);
        if (m_config.use_relative_e_distances) {
            // See GH issues #6336 #5073
            if ((m_config.gcode_flavor == gcfMarlinLegacy || m_config.gcode_flavor == gcfMarlinFirmware) &&
                !before_layer_gcode_resets_extruder && !layer_gcode_resets_extruder)
                return {L("Relative extruder addressing requires resetting the extruder position at each layer to "
                          "prevent loss of floating point accuracy. Add \"G92 E0\" to layer_gcode."),
                        nullptr, "before_layer_change_gcode"};
        } else if (before_layer_gcode_resets_extruder)
            return {L("\"G92 E0\" was found in before_layer_gcode, which is incompatible with absolute extruder "
                      "addressing."),
                    nullptr, "before_layer_change_gcode"};
        else if (layer_gcode_resets_extruder)
            return {L("\"G92 E0\" was found in layer_gcode, which is incompatible with absolute extruder addressing."),
                    nullptr, "layer_change_gcode"};
    }

    const ConfigOptionDef* bed_type_def = print_config_def.get("curr_bed_type");
    assert(bed_type_def != nullptr);

	    if (is_BBL_printer()) {
	    const t_config_enum_values* bed_type_keys_map = bed_type_def->enum_keys_map;
	    for (unsigned int extruder_id : extruders) {
	        const ConfigOptionInts* bed_temp_opt = m_config.option<ConfigOptionInts>(get_bed_temp_key(m_config.curr_bed_type));
	        for (unsigned int extruder_id : extruders) {
	            int curr_bed_temp = bed_temp_opt->get_at(extruder_id);
	            if (curr_bed_temp == 0 && bed_type_keys_map != nullptr) {
	                std::string bed_type_name;
	                for (auto item : *bed_type_keys_map) {
	                    if (item.second == m_config.curr_bed_type) {
	                        bed_type_name = item.first;
	                        break;
	                    }
	                }

	                StringObjectException except;
	                except.string = Slic3r::format(L("Plate %d: %s does not support filament %s"), this->get_plate_index() + 1, L(bed_type_name), extruder_id + 1);
	                except.string += "\n";
	                except.type   = STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE;
	                except.params.push_back(std::to_string(this->get_plate_index() + 1));
	                except.params.push_back(L(bed_type_name));
	                except.params.push_back(std::to_string(extruder_id+1));
	                except.object = nullptr;
	                return except;
	           }
            }
        }
    }

    // check if print speed/accel/jerk is higher than the maximum speed of the printer
    if (warning) {
        try {
            auto check_motion_ability_object_setting = [&](const std::vector<std::string>& keys_to_check, double limit) -> std::string {
                std::string warning_key;
                for (const auto& key : keys_to_check) {
                    if (m_default_object_config.get_abs_value(key) > limit) {
                        warning_key = key;
                        break;
                    }
                }
                return warning_key;
            };
            auto check_motion_ability_region_setting = [&](const std::vector<std::string>& keys_to_check, double limit) -> std::string {
                std::string warning_key;
                for (const auto& key : keys_to_check) {
                    if (m_default_region_config.get_abs_value(key) > limit) {
                        warning_key = key;
                        break;
                    }
                }
                return warning_key;
            };
            std::string warning_key;

            // check jerk
            if (m_default_object_config.default_jerk.values.front() == 1 || m_default_object_config.outer_wall_jerk.values.front() == 1 ||
                m_default_object_config.inner_wall_jerk.values.front() == 1) {
               warning->string = L("Setting the jerk speed too low could lead to artifacts on curved surfaces");
               if (m_default_object_config.outer_wall_jerk.values.front() == 1)
                    warning_key = "outer_wall_jerk";
               else if (m_default_object_config.inner_wall_jerk.values.front() == 1)
                    warning_key = "inner_wall_jerk";
               else
                    warning_key = "default_jerk";

               warning->opt_key = warning_key;
            }

            if (warning_key.empty() && m_default_object_config.default_jerk.values.front() > 0) {
               std::vector<std::string> jerk_to_check = {"default_jerk",     "outer_wall_jerk",    "inner_wall_jerk", "infill_jerk",
                                                         "top_surface_jerk", "initial_layer_jerk", "travel_jerk"};
               const auto               max_jerk = std::min(m_config.machine_max_jerk_x.values[0], m_config.machine_max_jerk_y.values[0]);
               warning_key.clear();
               if (m_default_object_config.default_jerk.values.front() > 0)
                    warning_key = check_motion_ability_object_setting(jerk_to_check, max_jerk);
               if (!warning_key.empty()) {
                    warning->string = L(
                        "The jerk setting exceeds the printer's maximum jerk (machine_max_jerk_x/machine_max_jerk_y).\nOrca will "
                        "automatically cap the jerk speed to ensure it doesn't surpass the printer's capabilities.\nYou can adjust the "
                        "maximum jerk setting in your printer's configuration to get higher speeds.");
                    warning->opt_key = warning_key;
               }
            }

            // check  junction deviation
            const auto max_junction_deviation = m_config.machine_max_junction_deviation.values[0];
            if (warning_key.empty() && m_default_object_config.default_junction_deviation.values.front() > max_junction_deviation) {
                warning->string  = L( "Junction deviation setting exceeds the printer's maximum value "
                                      "(machine_max_junction_deviation).\nOrca will "
                                      "automatically cap the junction deviation to ensure it doesn't surpass the printer's "
                                      "capabilities.\nYou can adjust the "
                                      "machine_max_junction_deviation value in your printer's configuration to get higher limits.");
                warning->opt_key = warning_key;
            }
            
            // check acceleration
            const auto max_accel = m_config.machine_max_acceleration_extruding.values[0];
            if (warning_key.empty() && m_default_object_config.default_acceleration.values.front() > 0 && max_accel > 0) {
               const bool support_travel_acc = (m_config.gcode_flavor == gcfRepetier || m_config.gcode_flavor == gcfMarlinFirmware ||
                                                m_config.gcode_flavor == gcfRepRapFirmware);

               std::vector<std::string> accel_to_check;
               if (!support_travel_acc)
                    accel_to_check = {
                        "default_acceleration",
                        "inner_wall_acceleration",
                        "outer_wall_acceleration",
                        "bridge_acceleration",
                        "initial_layer_acceleration",
                        "sparse_infill_acceleration",
                        "internal_solid_infill_acceleration",
                        "top_surface_acceleration",
                        "travel_acceleration",
                    };
               else
                    accel_to_check = {
                        "default_acceleration",
                        "inner_wall_acceleration",
                        "outer_wall_acceleration",
                        "bridge_acceleration",
                        "initial_layer_acceleration",
                        "sparse_infill_acceleration",
                        "internal_solid_infill_acceleration",
                        "top_surface_acceleration",
                    };
               warning_key = check_motion_ability_object_setting(accel_to_check, max_accel);
               if (!warning_key.empty()) {
                    warning->string  = L("The acceleration setting exceeds the printer's maximum acceleration "
                                          "(machine_max_acceleration_extruding).\nOrca will "
                                          "automatically cap the acceleration speed to ensure it doesn't surpass the printer's "
                                          "capabilities.\nYou can adjust the "
                                          "machine_max_acceleration_extruding value in your printer's configuration to get higher speeds.");
                    warning->opt_key = warning_key;
               }
               if (support_travel_acc) {
                    const auto max_travel = m_config.machine_max_acceleration_travel.values[0];
                    if (max_travel > 0) {
                        accel_to_check = {
                            "travel_acceleration",
                        };
                        warning_key = check_motion_ability_object_setting(accel_to_check, max_travel);
                        if (!warning_key.empty()) {
                            warning->string = L(
                                "The travel acceleration setting exceeds the printer's maximum travel acceleration "
                                "(machine_max_acceleration_travel).\nOrca will "
                                "automatically cap the travel acceleration speed to ensure it doesn't surpass the printer's "
                                "capabilities.\nYou can adjust the "
                                "machine_max_acceleration_travel value in your printer's configuration to get higher speeds.");
                            warning->opt_key = warning_key;
                        }
                    }
               }
            }

            // check speed
            // Orca: disable the speed check for now as we don't cap the speed
            // if (warning_key.empty()) {
            //    auto       speed_to_check = {"inner_wall_speed",  "outer_wall_speed", "sparse_infill_speed",   "internal_solid_infill_speed",
            //                                 "top_surface_speed", "bridge_speed",     "internal_bridge_speed", "gap_infill_speed"};
            //    const auto max_speed      = std::min(m_config.machine_max_speed_x.values[0], m_config.machine_max_speed_y.values[0]);
            //    warning_key.clear();
            //    warning_key = check_motion_ability_region_setting(speed_to_check, max_speed);
            //    if (warning_key.empty() && m_config.travel_speed.values.front() > max_speed)
            //         warning_key = "travel_speed";
            //    if (!warning_key.empty()) {
            //         warning->string = L(
            //             "The speed setting exceeds the printer's maximum speed (machine_max_speed_x/machine_max_speed_y).\nOrca will "
            //             "automatically cap the print speed to ensure it doesn't surpass the printer's capabilities.\nYou can adjust the "
            //             "maximum speed setting in your printer's configuration to get higher speeds.");
            //         warning->opt_key = warning_key;
            //    }
            // }

            // check wall sequence and precise outer wall
            if (m_default_region_config.precise_outer_wall && m_default_region_config.wall_sequence != WallSequence::InnerOuter) {
                warning->string  = L("The precise wall option will be ignored for outer-inner or inner-outer-inner wall sequences.");
                warning->opt_key = "precise_outer_wall";
            }

        } catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Orca: validate motion ability failed: " << e.what() << std::endl;
        }
    }
    if (!this->has_same_shrinkage_compensations()){
        warning->string = L("Filament shrinkage will not be used because filament shrinkage for the used filaments differs significantly.");
        warning->opt_key = "";
    }
    return {};
}

#if 0
// the bounding box of objects placed in copies position
// (without taking skirt/brim/support material into account)
BoundingBox Print::bounding_box() const
{
    BoundingBox bb;
    for (const PrintObject *object : m_objects)
        for (const PrintInstance &instance : object->instances()) {
        	BoundingBox bb2(object->bounding_box());
        	bb.merge(bb2.min + instance.shift);
        	bb.merge(bb2.max + instance.shift);
        }
    return bb;
}

// the total bounding box of extrusions, including skirt/brim/support material
// this methods needs to be called even when no steps were processed, so it should
// only use configuration values
BoundingBox Print::total_bounding_box() const
{
    // get objects bounding box
    BoundingBox bb = this->bounding_box();

    // we need to offset the objects bounding box by at least half the perimeters extrusion width
    Flow perimeter_flow = m_objects.front()->get_layer(0)->get_region(0)->flow(frPerimeter);
    double extra = perimeter_flow.width/2;

    // consider support material
    if (this->has_support_material()) {
        extra = std::max(extra, SUPPORT_MATERIAL_MARGIN);
    }

    // consider brim and skirt
    if (m_config.brim_width.value > 0) {
        Flow brim_flow = this->brim_flow();
        extra = std::max(extra, m_config.brim_width.value + brim_flow.width/2);
    }
    if (this->has_skirt()) {
        int skirts = m_config.skirt_loops.value;
        if (skirts == 0 && this->has_infinite_skirt()) skirts = 1;
        Flow skirt_flow = this->skirt_flow();
        extra = std::max(
            extra,
            m_config.brim_width.value
                + m_config.skirt_distance.value
                + skirts * skirt_flow.spacing()
                + skirt_flow.width/2
        );
    }

    if (extra > 0)
        bb.offset(scale_(extra));

    return bb;
}
#endif

double Print::skirt_first_layer_height() const
{
    return m_config.initial_layer_print_height.value;
}

Flow Print::brim_flow() const
{
    ConfigOptionFloatOrPercent width = m_config.initial_layer_line_width;
    if (width.value <= 0)
        width = m_print_regions.front()->config().inner_wall_line_width;
    if (width.value <= 0)
        width = m_objects.front()->config().line_width;

    /* We currently use a random region's perimeter extruder.
       While this works for most cases, we should probably consider all of the perimeter
       extruders and take the one with, say, the smallest index.
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    return Flow::new_from_config_width(
        frPerimeter,
        // Flow::new_from_config_width takes care of the percent to value substitution
		width,
        (float)m_config.nozzle_diameter.get_at(m_print_regions.front()->config().wall_filament-1),
		(float)this->skirt_first_layer_height());
}

Flow Print::skirt_flow() const
{
    ConfigOptionFloatOrPercent width = m_config.initial_layer_line_width;
    if (width.value <= 0)
        width = m_objects.front()->config().line_width;

    /* We currently use a random object's support material extruder.
       While this works for most cases, we should probably consider all of the support material
       extruders and take the one with, say, the smallest index;
       The same logic should be applied to the code that selects the extruder during G-code
       generation as well. */
    return Flow::new_from_config_width(
        frPerimeter,
        // Flow::new_from_config_width takes care of the percent to value substitution
		width,
		(float)m_config.nozzle_diameter.get_at(m_objects.front()->config().support_filament-1),
		(float)this->skirt_first_layer_height());
}

bool Print::has_support_material() const
{
    for (const PrintObject *object : m_objects)
        if (object->has_support_material())
            return true;
    return false;
}

// Ultra (H2C 3MF schema): ported verbatim from BambuStudio Print::support_material_on_wipe_tower
// (src/libslic3r/Print.cpp). Reported in slice_info.config so the printer knows the prime tower
// carries a support-only filament.
bool Print::support_material_on_wipe_tower() const
{
    if (!this->has_wipe_tower() || !this->has_support_material())
        return false;

    for (const PrintObject *object : m_objects) {
        if (!object->has_support_material())
            continue;

        const std::vector<unsigned int> obj_filaments = object->object_extruders();
        const int                       support_fil   = object->config().support_filament;
        const int                       support_interface_fil = object->config().support_interface_filament;

        auto support_differs_from_body = [&](int filament_1based) -> bool {
            if (filament_1based <= 0)
                return false;
            const unsigned int filament_0based = static_cast<unsigned int>(filament_1based - 1);
            return std::find(obj_filaments.begin(), obj_filaments.end(), filament_0based) == obj_filaments.end();
        };

        if (support_differs_from_body(support_fil) || support_differs_from_body(support_interface_fil))
            return true;
    }
    return false;
}

/*  This method assigns extruders to the volumes having a material
    but not having extruders set in the volume config. */
void Print::auto_assign_extruders(ModelObject* model_object) const
{
    // only assign extruders if object has more than one volume
    if (model_object->volumes.size() < 2)
        return;

//    size_t extruders = m_config.nozzle_diameter.values.size();
    for (size_t volume_id = 0; volume_id < model_object->volumes.size(); ++ volume_id) {
        ModelVolume *volume = model_object->volumes[volume_id];
        //FIXME Vojtech: This assigns an extruder ID even to a modifier volume, if it has a material assigned.
        if ((volume->is_model_part() || volume->is_modifier()) && ! volume->material_id().empty() && ! volume->config.has("extruder"))
            volume->config.set("extruder", int(volume_id + 1));
    }
}

void  PrintObject::set_shared_object(PrintObject *object)
{
    m_shared_object = object;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, found shared object from %2%")%this%m_shared_object;
}

void  PrintObject::clear_shared_object()
{
    if (m_shared_object) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, clear previous shared object data %2%")%this %m_shared_object;
        m_layers.clear();
        m_support_layers.clear();

        m_shared_object = nullptr;

        invalidate_all_steps_without_cancel();
    }
}

void  PrintObject::copy_layers_from_shared_object()
{
    if (m_shared_object) {
        m_layers.clear();
        m_support_layers.clear();

        firstLayerObjSliceByVolume.clear();
        firstLayerObjSliceByGroups.clear();

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, copied layers from object %2%")%this%m_shared_object;
        m_layers = m_shared_object->layers();
        m_support_layers = m_shared_object->support_layers();

        firstLayerObjSliceByVolume = m_shared_object->firstLayerObjSlice();
        firstLayerObjSliceByGroups = m_shared_object->firstLayerObjGroups();
    }
}

void  PrintObject::copy_layers_overhang_from_shared_object()
{
    if (m_shared_object) {
        for (size_t index = 0; index <  m_layers.size() && index <  m_shared_object->m_layers.size(); index++)
        {
            Layer* layer_src = m_layers[index];
            layer_src->loverhangs = m_shared_object->m_layers[index]->loverhangs;
            layer_src->loverhangs_bbox = m_shared_object->m_layers[index]->loverhangs_bbox;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, copied layer overhang from object %2%")%this%m_shared_object;
    }
}


// BBS
BoundingBox PrintObject::get_first_layer_bbox(float& a, float& layer_height, std::string& name)
{
    BoundingBox bbox;
    a = 0;
    name = this->model_object()->name;
    if (layer_count() > 0) {
        auto layer = get_layer(0);
        layer_height = layer->height;
        // only work for object with single instance
        auto shift = instances()[0].shift_without_plate_offset();
        for (auto bb : layer->lslices_bboxes)
        {
            bb.translate(shift.x(), shift.y());
            bbox.merge(bb);
        }
        for (auto slice : layer->lslices) {
            a += area(slice);
        }
    }
    if (has_brim())
        bbox = firstLayerObjectBrimBoundingBox;
    return bbox;
}

// ============================================================================
// CHAMELEON_DEBUG diagnostic logging (v2.5c, support-match pass instrumentation)
// ============================================================================
// Purpose: discriminate why a color's bucket never forms at a given support layer -
// see docs/superpowers/sdd/2026-08-29-support-interface-match/progress.md's v2.5a
// section and the round-4 GUI refutation this instruments for (white claw supports
// painting teal even though the max-2 trim never fires there - so either the k=1
// nearest-wall vote never samples a white wall at that layer, or the min-benefit gate
// kills the (tiny) claw bucket anyway).
//
// GUI slicing runs on a background thread whose BOOST_LOG_TRIVIAL output does not
// reliably land in the GUI's own log sink (Part 1's own hard-won lesson - the file sink
// wired up by set_log_path_and_level, utils.cpp, is not guaranteed to see every
// background-thread message on a GUI slice the way a CLI run does), so this writes
// directly to its own file via fopen-append instead of routing through
// BOOST_LOG_TRIVIAL - boost::nowide::fopen, the same idiom GCode.cpp/GCodeReader.cpp/
// GCodeProcessor.cpp already use elsewhere in this codebase for exactly this "open,
// write, close" pattern (never held open across calls, so a crash mid-slice never loses
// buffered-but-unflushed lines).
//
// Gate: chameleon_debug_enabled() reads getenv("CHAMELEON_DEBUG") exactly ONCE
// (function-local static). chameleon_assign_support_interfaces reads this ONCE per
// pass invocation into a local bool and threads that bool through every call site
// below it - nothing past that one getenv call runs when the env var is unset (every
// logging/formatting call below is behind `if (chameleon_debug_on)`; WallSampleIndex::
// add_polyline's own sample_count_out and apply_bucket_caps' own debug_out stay
// nullptr, so their own instrumentation is a no-op pointer compare too - see each
// function's own header comment).
//
// Log destination: <data_dir()>/log/chamdbg_support.log (data_dir() resolves to
// %APPDATA%\Snapmaker_Orca on Windows - see utils.cpp's set_log_path_and_level for the
// same log/ subfolder convention this mirrors). Appended to, never truncated - a fresh
// run's lines simply accumulate after any earlier run's; plain text, one record per
// line, never rotated or size-capped (diagnostic tool, not a production log stream).
//
// Line format - every value is a single token with no embedded whitespace, so a line
// splits cleanly into `key=value` fields on whitespace:
//   obj=<ordinal> print_z=<mm> height=<mm> fallback=<extruder> base_fallback=<extruder>
//   free_windowed=<id,id,...> free_strict=<id,id,...> base_cast_mm=<mm>
//   contact_layers=<li,li,...> coplanar_layers=<li,li,...>
//   band_samples=<li:e<id>=<count>,e<id>=<count>;li:...>
//   coplanar_samples=<li:e<id>=<count>,e<id>=<count>;li:...>
//   proj_hits=<n> proj_misses=<n> proj_hits_base=<n> proj_misses_base=<n>
//   buckets=<e<id>:before=<mm>,after=<mm>,outcome=<kept|kept_exempt|gated|trimmed>,redirect=<id|none>;...>
// v2.5d: band_samples/coplanar_samples replace the old single wall_samples= field -
// band_samples is the union (contact-band + coplanar) index interface/ironing vote
// against, unchanged in content/meaning from the old field; coplanar_samples is the
// NEW coplanar-only index base votes against (root cause fix below) - see
// chameleon_assign_support_interfaces' band_idx/coplanar_wall_idx declaration comment
// for why base needed its own, narrower electorate.
// v2.5e: proj_hits/proj_misses count every projection lookup this layer
// (chameleon_assign_support_interfaces' projection_lookup local, threaded through
// BrimFilament.hpp's chameleon_build_support_resolvers) - hit = the model surface
// directly above the sample resolved it (projection-first won); miss = the sample fell
// through to its role's own nearest-wall vote instead. Originally an interface/ironing-
// only signal (base had no projection branch yet). v2.5e review M3 flagged that even
// then, interface and ironing lookups landed in this ONE shared pair with no way to
// tell which role contributed which count (both roles call the identical resolver, see
// ChameleonSupportResolvers::ironing_resolver's own comment) - noted as cosmetic/
// documentation-level, not fixed at the time.
// v2.5f: base now ALSO tries projection first (chameleon_build_support_resolvers,
// BrimFilament.hpp/.cpp - see that function's decision-rule comment), through the SAME
// shared projection_lookup, so proj_hits/proj_misses now count ALL THREE roles combined
// - widening, not just carrying forward, the v2.5e review M3 note above. Fixed the
// base-vs-(interface+ironing) half of that conflation while extending the field (the
// discriminator this task actually needs - "is base's new projection branch engaging"):
// proj_hits_base/proj_misses_base (new fields, this line) isolate base's own share via
// a before/after snapshot of the same shared counters taken around base's own
// partition_support_entities call (chameleon_assign_support_interfaces' own per-layer
// loop is a plain sequential `for`, not a tbb::parallel_for, so the snapshot cannot
// race - see that loop's own comment at the snapshot site). proj_hits/proj_misses minus
// proj_hits_base/proj_misses_base is therefore interface+ironing's own combined share -
// the M3 conflation between THOSE two specifically is left as-is (still cosmetic: they
// share one resolver by design, "ironing follows its interface", so a reader who wants
// them apart would need chameleon_build_support_resolvers' projection_lookup seam to
// learn about roles, which its own header comment explicitly says it has no reason to
// do). Always 0/0/0/0 on a zero-sample layer (the engine calls, and therefore every
// resolver, never run).
// v2.5g: base_cast_mm is this support layer's own base projection depth limit
// (chameleon_base_cast_mm, BrimFilament.hpp/.cpp - min(kBaseCastMaxMm, kBaseCastMaxLayers
// * representative object layer height); see that function's own header comment for the
// "which layer height" choice). Computed and logged unconditionally, even on a zero-
// sample layer (it depends only on contact_idx's own leading layer heights, not on
// band_idx/coplanar_wall_idx having any samples) - cheap (at most 4 double reads), and
// aids exactly the next GUI round's diagnosis the same way every other per-layer field
// here does: a VLH plate's base_cast_mm sequence directly shows whether the layer-count
// or the 1.0mm ceiling is binding at each height.
// (any list-valued field is empty-after-`=` when that set/vector/map is empty - e.g.
// "free_strict=" means no extruder is strictly free at this layer). One line per
// support layer of a mode-active object that reaches this pass' own per-layer body
// (i.e. every layer NOT skipped by the plate guard or the chameleon_interface_visited
// re-run guard - see chameleon_assign_support_interfaces' own per-layer loop for both -
// including a zero-sample layer, whose line has an empty wall_samples/buckets tail:
// "a contact/coplanar band was selected here but nothing in it sampled at all" IS the
// signal that discriminates mechanism (a) from a normal gate/vote outcome). A per-
// object "SUMMARY " line (field-for-field mirror of the existing BOOST_LOG_TRIVIAL info
// line further down this file) is appended once per object, after its last per-layer
// line - see that BOOST_LOG_TRIVIAL call site's own comment for why this duplicate
// exists.
static bool chameleon_debug_enabled()
{
    static const bool enabled = (std::getenv("CHAMELEON_DEBUG") != nullptr);
    return enabled;
}

static void chameleon_debug_log(const std::string &line)
{
    boost::filesystem::path log_dir = boost::filesystem::path(data_dir()) / "log";
    boost::system::error_code ec;
    if (!boost::filesystem::exists(log_dir, ec))
        boost::filesystem::create_directories(log_dir, ec); // best-effort - the fopen below just fails (and is skipped) if this didn't work
    boost::filesystem::path log_path = log_dir / "chamdbg_support.log";
    FILE *f = boost::nowide::fopen(log_path.string().c_str(), "a");
    if (f == nullptr)
        return;
    fputs(line.c_str(), f);
    fputc('\n', f);
    fclose(f);
}

static std::string chameleon_debug_format_mm(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}

static std::string chameleon_debug_format_ids(const std::set<unsigned> &ids)
{
    std::string out;
    for (unsigned id : ids) {
        if (!out.empty())
            out += ',';
        out += std::to_string(id);
    }
    return out;
}

static std::string chameleon_debug_format_indices(const std::vector<size_t> &indices)
{
    std::string out;
    for (size_t i : indices) {
        if (!out.empty())
            out += ',';
        out += std::to_string(i);
    }
    return out;
}

// per_layer: (band layer index -> per-extruder inserted-sample count), one entry per
// li visited by chameleon_assign_support_interfaces' union_layer_indices loop - see
// that call site's own comment for how this is built.
static std::string chameleon_debug_format_wall_samples(
    const std::vector<std::pair<size_t, std::map<unsigned, size_t>>> &per_layer)
{
    std::string out;
    for (const auto &entry : per_layer) {
        if (!out.empty())
            out += ';';
        out += "li" + std::to_string(entry.first) + ":";
        bool first = true;
        for (const auto &kv : entry.second) {
            if (!first)
                out += ',';
            out += "e" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
            first = false;
        }
    }
    return out;
}

static std::string chameleon_debug_format_buckets(const std::vector<ChameleonBucketDebugEntry> &entries)
{
    std::string out;
    for (const ChameleonBucketDebugEntry &e : entries) {
        if (!out.empty())
            out += ';';
        out += "e" + std::to_string(e.extruder)
             + ":before=" + chameleon_debug_format_mm(e.length_before_mm)
             + ",after=" + chameleon_debug_format_mm(e.length_after_mm)
             + ",outcome=" + e.outcome
             + ",redirect=" + (e.redirected_into >= 0 ? std::to_string(e.redirected_into) : std::string("none"));
    }
    return out;
}

// Chameleon brim: walk one LayerRegion's perimeters (recursing into nested
// collections and unwrapping loops/multipaths to their leaf paths), adding
// every wall sample - shifted into plate coordinates - to wall_idx. The
// per-path extruder follows the same override rules PrintRegion::extruder()
// uses (1-based config values), converted to the 0-based scheme used
// throughout the chameleon-brim code (see task-3 report for the analysis).

// Chameleon P2/P2.1: per-region "color" extruder derivation (0-based) for a WALL path -
// wall_filament, then outer_wall_filament for the external perimeter. `is_external` is
// the erExternalPerimeter check for a wall PATH. Used by chameleon_collect_wall_samples'
// add_path lambda below (both the Part 1 brim wall index and the v2.1 coplanar lateral
// wall index feed off it). v2.1 final-review I3 fix: the projection (vertical-surface)
// path does NOT reuse this chain any more - a projected surface sample has no
// erExternalPerimeter concept and prints with the region's solid_infill_filament, not
// wall_filament - see chameleon_projection_region_extruder's own comment below for that
// derivation.
//
// v2.3 Task 2 (spec C8, root cause 6): calls PrintRegion::extruder() (PrintRegion.cpp:
// 18-29) directly instead of re-reading the raw wall_filament/outer_wall_filament config
// values by hand - a PrintRegion is reachable at every call site below (LayerRegion::
// region()), so this can never drift from the engine's own outer_wall_filament>0?outer:
// wall selection. extruder() never returns 0 for frExternalPerimeter/frPerimeter:
// wall_filament has config min=1 (PrintConfig.cpp) and painted regions always set it
// explicitly too (PrintApply.cpp:826-833) - outer_wall_filament's own min=0 is a REAL
// sentinel ("0 = follow walls"), already handled by extruder()'s internal ternary, not a
// gap. So the old own_extruder_0based fallback (reached only if wall_filament.value was
// <= 0, which config guarantees never happens) was unreachable dead code - removed here
// and threaded out of chameleon_collect_wall_samples/its three call sites below, since it
// no longer has anything to feed.
static unsigned chameleon_region_extruder(const PrintRegion &region, bool is_external)
{
    // v2.3 final-review M5 fix: defensive clamp against a hand-edited/legacy profile that
    // violates wall_filament's documented config min=1 and lets extruder() return 0 here -
    // well-formed configs can never reach this (min=1 confirmed at PrintConfig.cpp:3498,
    // painted regions always set it too, PrintApply.cpp:826-833), so this is pure defense-
    // in-depth, not a live bug. Without it the raw `- 1` (unsigned arithmetic) underflows
    // to 0xFFFFFFFF and that garbage value is used DIRECTLY as a bucket key/extruder id -
    // unlike the engine's own `- 1` sites (e.g. LayerRegion.cpp:151's this->extruder(role)
    // - 1), which feed ConfigOptionVector::get_at(), whose own underflow clamp silently
    // falls back to the 0th element (get_at's documented behavior, cited in that same
    // comment). std::max(1u, ...) buys back that same old own-extruder-fallback failure
    // mode here, for free, since chameleon has no get_at() call in this path to clamp it.
    return std::max(1u, region.extruder(is_external ? frExternalPerimeter : frPerimeter)) - 1;
}

// CHAMELEON_DEBUG (v2.5c diagnostic instrumentation): `sample_count_out`, when non-
// null, is threaded straight through to every WallSampleIndex::add_polyline call below
// (including the two recursive self-calls, so a nested collection's paths are counted
// too) - see that function's own header comment (WallSampleIndex.hpp) for the per-
// extruder accumulation this ultimately feeds. Default nullptr: every pre-v2.5c call
// site is unaffected.
static void chameleon_collect_wall_samples(const ExtrusionEntity* entity, const PrintRegion& region,
                                            const Point& shift, size_t object_key, WallSampleIndex& wall_idx,
                                            std::map<unsigned, size_t>* sample_count_out = nullptr)
{
    if (entity == nullptr)
        return;
    if (entity->is_collection()) {
        const auto* coll = static_cast<const ExtrusionEntityCollection*>(entity);
        for (const ExtrusionEntity* child : coll->entities)
            chameleon_collect_wall_samples(child, region, shift, object_key, wall_idx, sample_count_out);
        return;
    }

    auto add_path = [&](const ExtrusionPath& path) {
        if (path.polyline.points.empty())
            return;
        const unsigned extruder = chameleon_region_extruder(region, path.role() == erExternalPerimeter);

        Points shifted;
        shifted.reserve(path.polyline.points.size());
        for (const Point& pt : path.polyline.points)
            shifted.push_back(pt + shift);
        wall_idx.add_polyline(shifted, extruder, object_key, /*spacing_mm=*/0.8, sample_count_out);
    };

    if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity)) {
        add_path(*path);
    } else if (const auto* multipath = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
        for (const ExtrusionPath& path : multipath->paths)
            add_path(path);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
        for (const ExtrusionPath& path : loop->paths)
            add_path(path);
    }
}

// Chameleon P2.1 v2.1 final-review I3 fix: the projection branch resolves the PRINTED
// filament of a projected surface sample, which is NOT the same question
// chameleon_region_extruder answers for a wall path. The surface the support interface
// touches is the model's bottom shell, and the pipeline prints that shell with the
// region's solid_infill_filament (ToolOrdering.cpp's LayerTools::solid_infill_filament /
// PrintRegion::extruder(frSolidInfill|frTopSolidInfill), PrintRegion.cpp:18-35), not
// wall_filament - painted regions set wall/solid/sparse together so the two only diverge
// under a manual filament mapping (wall_filament != solid_infill_filament), exactly the
// mismatch this feature exists to remove. wall_filament/outer_wall_filament are NEVER
// consulted here - those stay chameleon_region_extruder's job, unchanged, for the LATERAL
// (wall-sample) path only.
//
// v2.3 Task 2 (spec C8, root cause 6): calls PrintRegion::extruder(frSolidInfill)
// directly (PrintRegion.cpp:18-29) - a PrintRegion is reachable at the one call site
// below (LayerRegion::region()) - instead of the old hand-rolled "solid_infill_filament
// if set, else sparse_infill_filament" read. That raw read MISSED a real engine
// behavior: PrintRegion::extruder(frSolidInfill) routes through sparse_infill_filament
// whenever this region's sparse_infill_density == 100% (internal_solid_infill_uses_
// sparse_filament, PrintRegion.cpp:10-13) - REGARDLESS of whether solid_infill_filament
// is also set (it always is, see below), because the engine actually prints that
// region's solid infill using sparse_infill_filament in that case. The old code's
// "solid, else sparse" fallback chain only ever took the sparse branch when
// solid_infill_filament read as 0, which - now that extruder() is called directly -
// never happens anyway: both options have config min=1 (PrintConfig.cpp) and painted
// regions always set both explicitly too (PrintApply.cpp:826-833), so the old
// own_extruder_0based last-resort fallback (reached only if BOTH read <= 0) was also
// unreachable dead code. Both issues are fixed by routing through the one canonical
// PrintRegion::extruder() call instead of re-deriving its logic by hand.
// v2.4 (spec A): this function and the two below it (chameleon_build_projection_views,
// chameleon_projection_extruder_from_view) were nearest_surface's own vertical-
// projection resolver glue; v2.4 deleted nearest_surface's only caller
// (chameleon_assign_support_interfaces' else-arm) but deliberately did NOT delete
// these - kept as v2.5 upward-cast scaffolding for IRONING + TOP INTERFACES (see
// docs/superpowers/specs/2026-08-30-support-match-v24-design.md's "E. Non-changes"
// section). v2.5e (this task): LIVE again - chameleon_assign_support_interfaces' own
// projection_lookup lambda (see that function's per-layer loop) calls
// chameleon_projection_extruder_from_view below, which calls this. ProjectionLayerView
// (BrimFilament.hpp), chameleon_pick_projection_region (BrimFilament.hpp/.cpp), and
// their existing unit tests needed no changes for this - only their CALLER was ever
// missing.
static unsigned chameleon_projection_region_extruder(const PrintRegion &region)
{
    // v2.3 final-review M5 fix: same defensive underflow clamp as
    // chameleon_region_extruder above, and for the same reason - solid_infill_filament/
    // sparse_infill_filament both have config min=1 (well-formed configs never reach 0
    // here), but chameleon reads the raw `- 1` directly as a bucket key/extruder id with
    // no get_at()-style clamp downstream to catch a hand-edited/legacy profile's 0.
    return std::max(1u, region.extruder(frSolidInfill)) - 1;
}

// v2.2 Task 2 (spec C5, root cause 4): mirrors Support/SupportMaterial.cpp:57's
// `#define SUPPORT_MATERIAL_MARGIN 1.2` - the margin the support generator itself grows
// interface CONTACT polygons by beyond the overhang (root cause 4: "interface contact
// polygons are grown up to SUPPORT_MATERIAL_MARGIN (1.2mm) + support_expansion beyond
// the overhang, so samples over small features [...] land outside lslices"). That
// #define is file-local (never exported via a header), so this is a deliberate VALUE
// mirror, not a shared symbol - and deliberately NOT named `SUPPORT_MATERIAL_MARGIN`
// itself, because two other unrelated same-named macros already exist in this codebase
// and neither is the one this margin story is about: PrintObject.cpp:4421 #defines the
// same name to the same value (1.2) for a completely different purpose (bridge-edge
// removal, remove_bridges_from_contacts); Support/SupportCommon.cpp:39's own same-named
// macro is 1.5, a different margin for a different purpose (its own bridge-edge/loop-
// line offsetting). Giving this one a distinct name avoids ever confusing it with either.
static constexpr double CHAMELEON_PROJECTION_MARGIN_MM = 1.2;

// Chameleon P2.1 v2.1 final-review M1 fix: build the ProjectionLayerView list for a
// projection band ONCE per support layer instead of once per 0.8mm sample point. Layer/
// LayerRegion can't be built standalone (private/protected ctors, PrintObject-owned
// storage - see the T2 report), so the geometric selection itself stays delegated to
// chameleon_pick_projection_region (BrimFilament.hpp/.cpp, unit-tested with hand-built
// ExPolygons); this function is just the Layer/LayerRegion -> ProjectionLayerView glue,
// now factored out of the per-sample resolver so the caller (chameleon_assign_support_
// interfaces) can call it once per support layer and capture the result by reference in
// its interface_resolver lambda. `band_layer_indices` must already be ordered lowest
// layer first (the order select_layers_in_band/select_contact_layers return). Every
// container this builds (`out_view` and its per-region pointer vectors) holds ExPolygon
// POINTERS only, never copies polygon geometry - see BrimFilament.hpp's
// ProjectionLayerView comment. `out_view_layers` is parallel to `out_view` (same index),
// kept for extruder resolution after chameleon_pick_projection_region reports a hit.
//
// v2.2 Task 2 (spec C5): also builds each layer's expanded_lslices/expanded_lslices_
// bboxes here - ONCE per band layer, same hoisting rationale as the rest of this view.
// Offset amount is CHAMELEON_PROJECTION_MARGIN_MM + this object's own support_expansion
// (mirrors the support generator's own contact-polygon growth - see
// Support/SupportMaterial.cpp:1395's xy_expansion, added to the same margin). Unlike
// lslices/region_slice_polys above, expanded_lslices IS a copy (offset_ex produces new
// geometry, not aliased storage), so this is the one part of the view that isn't free.
// v2.5e: LIVE - called once per support layer from chameleon_assign_support_interfaces'
// per-layer loop, over `contact_idx` (the contact-band layers only, mirroring
// nearest_surface's pre-v2.4 own `band_layer_indices` argument here); see
// chameleon_projection_region_extruder's own banner comment above.
static void chameleon_build_projection_views(const PrintObject &object,
                                              const std::vector<size_t> &band_layer_indices,
                                              std::vector<ProjectionLayerView> &out_view,
                                              std::vector<const Layer *> &out_view_layers)
{
    const auto &layers = object.layers();
    const float margin_scaled = float(scale_(CHAMELEON_PROJECTION_MARGIN_MM + object.config().support_expansion.value));

    out_view.clear();
    out_view_layers.clear();
    out_view.reserve(band_layer_indices.size());
    out_view_layers.reserve(band_layer_indices.size());

    for (size_t li : band_layer_indices) {
        if (li >= layers.size())
            continue; // defensive: bail on out-of-range input rather than crash
        const Layer *layer = layers[li];
        if (layer == nullptr)
            continue;

        ProjectionLayerView lv;
        lv.lslices        = &layer->lslices;
        lv.lslices_bboxes = &layer->lslices_bboxes;

        // v2.2 Task 2 (spec C5): margin-ring geometry for this band layer, computed
        // once here rather than once per sample point (see the function comment).
        lv.expanded_lslices = offset_ex(layer->lslices, margin_scaled);
        lv.expanded_lslices_bboxes.reserve(lv.expanded_lslices.size());
        for (const ExPolygon &expoly : lv.expanded_lslices)
            lv.expanded_lslices_bboxes.push_back(get_extents(expoly));

        lv.region_slice_polys.reserve(layer->regions().size());
        lv.region_bottom_polys.reserve(layer->regions().size());
        for (const LayerRegion *lr : layer->regions()) {
            std::vector<const ExPolygon *> slice_polys;
            std::vector<const ExPolygon *> bottom_polys;
            if (lr != nullptr) {
                for (const Surface &s : lr->slices.surfaces)
                    slice_polys.push_back(&s.expolygon);
                for (const Surface &s : lr->fill_surfaces.surfaces)
                    if (s.is_bottom())
                        bottom_polys.push_back(&s.expolygon);
            }
            lv.region_slice_polys.push_back(std::move(slice_polys));
            lv.region_bottom_polys.push_back(std::move(bottom_polys));
        }

        out_view.push_back(std::move(lv));
        out_view_layers.push_back(layer);
    }
}

// Chameleon P2.1 (v2.1 spec: "vertical projection is primary" for interface entities;
// see docs/superpowers/specs/2026-08-30-support-match-v2-design.md). For one 0.8mm
// sample point p (object coordinates, no instance shift - same convention as
// chameleon_collect_wall_samples above), resolve the extruder of the model surface
// directly above it, against an ALREADY-BUILT `view`/`view_layers` pair (v2.1
// final-review M1 fix: chameleon_build_projection_views above builds these once per
// support layer; this function no longer builds anything, so it's cheap to call once per
// sample point as before). Returns false, leaving out_extruder unwritten, when no band
// layer's lslices cover p - the caller (Task 3) then falls through to the lateral rule.
//
// Extruder resolution: v2.1 final-review I3 fix - chameleon_projection_region_extruder
// above (routes through PrintRegion::extruder(frSolidInfill), v2.3 Task 2 spec C8), NOT
// chameleon_region_extruder (wall_filament-based; still used unchanged by the lateral/
// wall-sample path). A projected surface sample has no erExternalPerimeter concept
// either way - "is this the region's external perimeter path" doesn't apply to a bare
// fill surface - so outer_wall_filament was never, and is still never, consulted here.
//
// v2.3 Task 2 (spec C8): the object_default_extruder parameter this function used to take
// is gone - it only ever fed chameleon_projection_region_extruder's own last-resort
// fallback, which spec C8 established is unreachable dead code (PrintRegion::extruder()
// never returns 0 for frSolidInfill; see that function's own comment above), so there is
// nothing left here for a caller-supplied default to fall back to.
//
// Perf: O(band layers x regions) per call now that the view is prebuilt (was O(band
// layers x regions x islands) per call before M1). Layer::lslices_bboxes is precomputed
// once at slicing time (PrintObjectSlice.cpp), long before this pass runs, so the AABB
// gate inside chameleon_pick_projection_region is free.
// v2.5e: LIVE - wrapped in a small `bool(const Point&, unsigned&)` lambda
// (chameleon_assign_support_interfaces' own projection_lookup local) and passed into
// BrimFilament.hpp's chameleon_build_support_resolvers, whose interface_resolver tries
// this before falling through to the nearest-wall vote; see
// chameleon_projection_region_extruder's own banner comment above.
static bool chameleon_projection_extruder_from_view(const std::vector<ProjectionLayerView> &view,
                                                      const std::vector<const Layer *> &view_layers,
                                                      const Point &p,
                                                      unsigned &out_extruder,
                                                      // v2.5g: forwarded verbatim to chameleon_pick_projection_region's
                                                      // own `layer_limit` (BrimFilament.hpp/.cpp) - default unbounded, so
                                                      // every pre-v2.5g call site (interface/ironing's projection_lookup
                                                      // below) is byte-identical to before. base_projection_lookup
                                                      // (chameleon_assign_support_interfaces, below) is the only caller
                                                      // that ever passes a real limit - see that lambda's own comment.
                                                      size_t layer_limit = std::numeric_limits<size_t>::max(),
                                                      // Optional: receives the object layer the hit is on.
                                                      const Layer **out_hit_layer = nullptr)
{
    size_t hit_layer = 0, hit_region = 0;
    if (!chameleon_pick_projection_region(view, p, hit_layer, hit_region, layer_limit))
        return false; // no band layer's lslices cover p -> caller falls through to lateral

    if (hit_layer >= view_layers.size())
        return false; // defensive: cannot happen (hit_layer indexes `view`, built 1:1 with view_layers)
    const LayerRegionPtrs &regions = view_layers[hit_layer]->regions();
    if (hit_region >= regions.size() || regions[hit_region] == nullptr)
        return false; // defensive: cannot happen (hit_region indexes the same regions() this view was built from)

    const LayerRegion &lr = *regions[hit_region];
    out_extruder = chameleon_projection_region_extruder(lr.region());
    if (out_hit_layer != nullptr)
        *out_hit_layer = view_layers[hit_layer];
    return true;
}

// BBS: map print object with its first layer's first extruder
std::map<ObjectID, unsigned int> getObjectExtruderMap(const Print& print) {
    std::map<ObjectID, unsigned int> objectExtruderMap;
    for (const PrintObject* object : print.objects()) {
        // BBS
        if (object->object_first_layer_wall_extruders.empty()){
            unsigned int objectFirstLayerFirstExtruder = print.config().filament_diameter.size();
            auto firstLayerRegions = object->layers().front()->regions();
            if (!firstLayerRegions.empty()) {
                for (const LayerRegion* regionPtr : firstLayerRegions) {
                    if (regionPtr->has_extrusions())
                        objectFirstLayerFirstExtruder = std::min(objectFirstLayerFirstExtruder,
                          regionPtr->region().extruder(frExternalPerimeter));
                }
            }
            objectExtruderMap.insert(std::make_pair(object->id(), objectFirstLayerFirstExtruder));
        }
        else {
            objectExtruderMap.insert(std::make_pair(object->id(), object->object_first_layer_wall_extruders.front()));
        }
    }
    return objectExtruderMap;
}

// Chameleon P2: this object's default extruder (0-based) - the fallback interface
// extruder when support_interface_filament is unset (0 = "current filament"). v2.3
// Task 2 (spec C8) removed this value's other former use, chameleon_collect_wall_
// samples' own_extruder_0based last-resort parameter - PrintRegion::extruder() never
// actually needs a caller-supplied default (see chameleon_region_extruder's own
// comment above), so that use was unreachable dead code. Deliberately NOT read from
// PrintObject::object_first_layer_wall_extruders
// (getObjectExtruderMap's memoized path above): that field is only populated once
// ToolOrdering::collect_extruders runs (ToolOrdering.cpp ~851), which for this pass is
// always still in the future - see chameleon_assign_support_interfaces' call site in
// Print::process, sited before psWipeTower constructs ToolOrdering. Recomputing directly
// here (mirroring getObjectExtruderMap's own fresh-compute branch) keeps the value
// deterministic and independent of whatever a prior process() run may have left behind.
static unsigned chameleon_object_default_extruder(const Print &print, const PrintObject &object)
{
    if (object.layers().empty())
        return 0;
    unsigned int best = (unsigned int) print.config().filament_diameter.size(); // sentinel: none found
    for (const LayerRegion *lr : object.layers().front()->regions())
        if (lr->has_extrusions())
            best = std::min(best, lr->region().extruder(frExternalPerimeter));
    return (best > 0 && best < (unsigned int) print.config().filament_diameter.size()) ? best - 1 : 0;
}

// v2.3 Task 1 (spec C1): true when ANY enabled, non-deleted mixed-filament row on the
// plate has its own gradient interpolation on (MixedFilament::gradient_enabled) - used
// below to skip chameleon_collect_layer_filaments' object/layer walk entirely rather
// than attempt it. A gradient (or, more generally, a comma-grouped manual-pattern
// mixed row - see MixedFilamentManager::resolve_perimeter's per-wall-loop-index
// resolution, LayerRegion.cpp:73-76) row means a single region's TRUE per-layer, and
// even per-WALL-LOOP, filament can vary in ways LayerRegion::extruder(role) alone (a
// single scalar per role) cannot represent - ToolOrdering's own collect_extruders only
// gets this right by running resolve_mixed with full per-layer-index/mixed-manager
// context, which doesn't exist yet at this point in the pipeline (this pass runs
// before ToolOrdering is even constructed - see chameleon_object_default_extruder's
// own comment above for the same "still in the future" constraint). Skipping here is
// deliberately conservative: every support layer's free-extruder lookup then returns
// empty, so every bucket falls back to the gate's NORMAL tier only - inert, not wrong.
static bool chameleon_mixed_gradient_active(const Print &print)
{
    for (const MixedFilament &mf : print.mixed_filament_manager().mixed_filaments())
        if (mf.enabled && !mf.deleted && mf.gradient_enabled)
            return true;
    return false;
}

// v2.3 Task 1 (spec C1): once-per-pass "free extruder" table build - the object/layer
// walk feeding BrimFilament.hpp's pure LayerFilamentTable/build_layer_filament_table
// (see that function's own doc comment for the merge semantics this only supplies raw
// samples to). Skipped entirely (empty table) when chameleon_mixed_gradient_active
// above is true. Otherwise: for every object NOT a shared-object copy (mirrors the
// skip in chameleon_assign_support_interfaces' own per-object loop below - a copy's
// layers() alias the same Layer* vector as the object it was copied from, so walking
// it again would only re-record the same z/extruder samples, harmlessly but
// pointlessly), every object layer, every region on that layer: LayerRegion::extruder
// (role) is read directly (NOT PrintRegion::extruder - LayerRegion's own override
// already folds in painted-region/effective-filament resolution, see LayerRegion.cpp:
// 86-100) for frExternalPerimeter AND frPerimeter (a region's outer_wall_filament
// override can differ from its own wall_filament, so both must be checked - missing
// the inner-wall-only filament would under-report which extruders are genuinely
// "already here") when the layer has any perimeters, and for frSolidInfill/frInfill
// when it has any fills. A role read of 0 (PrintRegion::extruder's own "follow the
// object default" convention, PrintRegion.cpp:18-35) contributes nothing - this table
// only ever records an EXPLICIT non-zero per-region filament assignment, not a scalar
// default that could still mean any extruder depending on context.
//
// `only` (by-object sequence): restrict the walk to that one object. A sequential print prints
// each object on its own, so an extruder that some OTHER object uses at this z is not already
// active here and must not count as free.
static LayerFilamentTable chameleon_collect_layer_filaments(const Print &print, const PrintObject *only = nullptr)
{
    if (chameleon_mixed_gradient_active(print))
        return {};

    std::vector<std::pair<double, unsigned>> raw;
    for (const PrintObject *object : print.objects()) {
        if (object == nullptr || object->get_shared_object() != nullptr)
            continue;
        if (only != nullptr && object != only)
            continue;
        for (const Layer *layer : object->layers()) {
            if (layer == nullptr)
                continue;
            for (const LayerRegion *lr : layer->regions()) {
                if (lr == nullptr)
                    continue;
                if (!lr->perimeters.entities.empty()) {
                    for (FlowRole role : { frExternalPerimeter, frPerimeter }) {
                        const unsigned int ext = lr->extruder(role);
                        if (ext > 0)
                            raw.emplace_back(layer->print_z, ext - 1);
                    }
                }
                if (!lr->fills.entities.empty()) {
                    for (FlowRole role : { frSolidInfill, frInfill }) {
                        const unsigned int ext = lr->extruder(role);
                        if (ext > 0)
                            raw.emplace_back(layer->print_z, ext - 1);
                    }
                }
            }
        }
    }
    return build_layer_filament_table(std::move(raw));
}

// Support filament matching, "visible surface" rule (fix/support-filament-matching-paint-brim).
//
// One wall piece of an object layer that a support can actually stand against: a wall path
// clipped to the ring within one external-perimeter width of the layer's outline, tagged with
// the filament that prints it (0-based, chameleon_region_extruder).
struct ChameleonWallPiece
{
    Polyline polyline;
    unsigned extruder;
};

static void chameleon_collect_wall_polylines(const ExtrusionEntity *entity, const PrintRegion &region,
                                             std::map<unsigned, Polylines> &out)
{
    if (entity == nullptr)
        return;
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            chameleon_collect_wall_polylines(child, region, out);
        return;
    }
    auto add_path = [&](const ExtrusionPath &path) {
        if (path.polyline.points.size() < 2)
            return;
        out[chameleon_region_extruder(region, path.role() == erExternalPerimeter)].emplace_back(path.polyline.points);
    };
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        add_path(*path);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            add_path(path);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        for (const ExtrusionPath &path : loop->paths)
            add_path(path);
    }
}

// The visible walls of one object layer. A part that is multi-material painted all over while its
// volume is assigned a different filament (the typical CAD import: the paint is right, the
// assignment is not) slices into a painted claim along the surface and the unpainted base region
// inside it. With a bounded paint depth the base region gets its own loop of EXTERNAL perimeter
// along that colour boundary, deep inside the part, printed in the assigned filament; every inner
// wall is inside the part too. None of that can be seen, and none of it can touch a support - but
// before this rule all of it was a wall sample, and on the reported part the assigned filament
// carried nearly half of the samples in every contact band (CHAMELEON_DEBUG), so nearest-wall votes
// under a painted overhang picked the invisible colour. Only wall paths inside the ring within one
// external-perimeter width of Layer::lslices (the merged outline of every region) are kept. Should
// clipping leave nothing (degenerate geometry), the unclipped walls are returned, so this rule
// never turns a layer that had samples into a zero-sample one.
static std::vector<ChameleonWallPiece> chameleon_visible_wall_pieces(const Layer &layer)
{
    std::map<unsigned, Polylines> by_extruder;
    coord_t                       ext_width = 0;
    for (const LayerRegion *lr : layer.regions()) {
        if (lr == nullptr || lr->perimeters.entities.empty())
            continue;
        ext_width = std::max(ext_width, coord_t(lr->flow(frExternalPerimeter).scaled_width()));
        chameleon_collect_wall_polylines(&lr->perimeters, lr->region(), by_extruder);
    }

    std::vector<ChameleonWallPiece> out;
    if (by_extruder.empty())
        return out;
    if (ext_width > 0 && !layer.lslices.empty()) {
        // Everything deeper than one external-perimeter width inside the outline.
        const ExPolygons interior = offset_ex(layer.lslices, -float(ext_width));
        for (const auto &[extruder, polylines] : by_extruder)
            for (Polyline &pl : interior.empty() ? Polylines(polylines) : diff_pl(polylines, interior))
                if (pl.points.size() >= 2)
                    out.push_back({ std::move(pl), extruder });
    }
    if (out.empty())
        for (auto &[extruder, polylines] : by_extruder)
            for (Polyline &pl : polylines)
                out.push_back({ std::move(pl), extruder });
    return out;
}

// The filament with the most visible wall length over `pieces` (ties -> lowest id). False when
// there is none.
static bool chameleon_dominant_wall_extruder(const std::vector<const std::vector<ChameleonWallPiece> *> &pieces,
                                             unsigned                                                    &out)
{
    std::map<unsigned, double> length;
    for (const std::vector<ChameleonWallPiece> *layer_pieces : pieces)
        for (const ChameleonWallPiece &piece : *layer_pieces)
            length[piece.extruder] += piece.polyline.length();
    bool   found = false;
    double best  = 0.;
    for (const auto &[extruder, len] : length)
        if (!found || len > best) { // std::map is ordered, so a tie keeps the lower id
            found = true;
            best  = len;
            out   = extruder;
        }
    return found;
}

// Chameleon P2.1: support match pass (renamed from "interface partition pass" - v2.1
// resolves BOTH support roles now, see docs/superpowers/specs/2026-08-30-support-match-
// v2-design.md's "Decision rules" section for the original binding contract; v2.4 (spec
// A, docs/superpowers/specs/2026-08-30-support-match-v24-design.md) simplified the
// per-layer resolution rule itself to a single mode - see below; v2.6 then replaced the
// resulting two-valued enum with a plain checkbox, since a single mode is what it always
// was). For every object opted into support_filament_matching (v2.4: nearest_surface's
// OWN vertical-projection-primary/lateral-fallback MODE was REMOVED outright, GUI A/B
// round 2 verdict: it still mixed layers even in large single-color areas even after
// v2.2/v2.3's fixes, leaving nearest_wall as the sole non-manual mode this checkbox now
// simply turns on or off), per
// support layer: all three roles - interface (erSupportMaterialInterface), base
// (erSupportMaterial), and ironing (erIroning, "ironing follows its interface": erIroning
// entities are the ironed top surface of a matched interface run, not a role with its own
// independent rule) - are resolved by nearest-wall voting: brim_vote, k=1, UNCAPPED
// (max_dist_mm=0) - "the nearest wall segment wins outright, no distance limit, no
// weighted vote" (spec C8).
//
// v2.5e (upward cast, GUI round 6/7 - user directive from the v2.4 nearest_surface-drop
// decision: "the projection machinery is retained as scaffolding for exactly this")
// covered interface/ironing; v2.5f (GUI round 7 continued - user's standing fidelity
// ruling: "a support sample with model surface within the contact band directly above
// takes that surface's color") extends the SAME rule to base:
// DECISION RULE - a support sample with a model surface directly above it (within the
// contact band) takes THAT surface's color - it will abut it, or supports toward it;
// walls are the tiebreaker ONLY for samples with nothing overhead. Concretely: ALL
// THREE ROLES now resolve PROJECTION FIRST - the model surface directly above a sample,
// via chameleon_build_projection_views/chameleon_projection_extruder_from_view below
// (the nearest_surface-era machinery this comment used to describe as "kept, currently
// unused" before v2.5e revived it) - falling through to a nearest-wall vote only when
// the projection misses (no band layer's lslices cover the sample), and further to that
// vote's own empty-index fallback_extruder return when its index has no samples at all.
// Interface and ironing (via interface_resolver/ironing_resolver, wired identically -
// "ironing follows its interface") fall through to the v2.5d nearest-wall vote against
// `band_idx` (the coplanar+contact-band union) - unchanged since v2.5e. Base (v2.5f,
// this task) falls through to `coplanar_wall_idx` instead - same shape, narrower
// electorate - because a base/wrap run with NOTHING above it still abuts a wall AT ITS
// OWN Z (the wrap case), never the contact band above; that is "base votes same-layer
// walls" in the ruling's own wording, and it is the reason base was left UNCHANGED
// through v2.5e even though this task makes it no longer walls-ONLY. Root cause for
// extending this to base at all: TREE support is ~99% base role (v2.5f forensics: 592mm
// interface vs 191,556mm base on the reference plate) - v2.5e's fix alone covered only
// the thin interface skin, leaving the visible bulk of a support column voting coplanar
// under a painted overhang start. Taper note: at a tapering wall the margin-ring rescue
// pass (chameleon_pick_projection_region PASS 2, BrimFilament.hpp/.cpp) can now resolve
// a base wrap sample to the upper layer's color up to CHAMELEON_PROJECTION_MARGIN_MM
// (1.2mm) early, the same way it already could for interface/ironing since v2.5e -
// accepted per the same fidelity ruling (colors switch TOWARD the upcoming paint); the
// v2.5d coplanar-only fix for base's WALL VOTE is unaffected (it is the fallthrough
// tier, reached only on a genuine projection miss), so the old multi-layer bleed-upward
// defect that fix targeted cannot return through this path. The actual resolver-
// construction wiring for all three roles - the part this decision rule governs - lives
// in BrimFilament.hpp/.cpp's chameleon_build_support_resolvers (v2.5d final-review I1
// fix: pulled out specifically so a unit test can pin this wiring, not just the
// underlying brim_vote/partition_support_entities primitives it composes) - see that
// function's own header comment for the full three-tier fallback chain, the taper note
// in full, and the reasoning behind its `projection_lookup` seam. This per-layer loop's
// job is now just to build band_idx/coplanar_wall_idx/the projection view (unchanged
// from before) and hand them to that function.
//
// All three roles are still partitioned via three calls to the SAME
// partition_support_entities engine (T1/v2.2 Task 3) - interface first (role_filter =
// erSupportMaterialInterface), then base (role_filter = erSupportMaterial), then ironing
// (role_filter = erIroning, reusing ironing_resolver/fallback_extruder/vote_params -
// ironing_resolver being the SAME callable as interface_resolver, per the rule above) -
// sharing ONE SupportLayer::interface_by_extruder
// map (T1 storage), which v2.2 Task 1 then gates/trims as a whole via ONE
// apply_bucket_caps call per layer (spec C1-C3: length-ranked distinct-extruder cap with
// hysteresis, NOT a switch-count budget - see apply_bucket_caps' own doc comment in
// BrimFilament.hpp for the full rationale; order among the three partition_support_entities
// calls only matters for log/counter bookkeeping, not correctness: each call only ever
// touches entities whose role() matches its own role_filter). v2.2 Task 3 (spec C7) also
// makes each of these three calls vote any role-eligible NESTED collection (collapsed
// role() == that call's role_filter) as one whole unit instead of leaving it invisible to
// the matcher entirely (spec root cause 6) - see partition_support_entities' own doc
// comment in BrimFilament.hpp.
//
// Must run after generate_support_material has completed for every object - fresh or
// copied from a shared/cached object - and before ToolOrdering/psWipeTower construction,
// so the per-support-layer registration block in ToolOrdering.cpp (~698-721) - which reads
// interface_by_extruder at ctor time, keyed by extruder regardless of which role(s) are
// mixed within it - sees the finished result (see the call site below for exactly where
// this sits in the pipeline; unlike Part 1's brim pass, no post-hoc union hack is needed
// because this pass always runs first).
//
// Off (checkbox unchecked / single extruder): this function returns
// immediately without touching support_fills or interface_by_extruder on ANY object -
// byte-identical gcode is a hard requirement (spec's off-mode purity). By-object sequence
// used to be "off" too, which is why the checkbox did nothing on sequential plates and their
// support kept printing in whatever filament happened to be active - layer by layer the
// part's assigned filament (still used for its unpainted core) and its painted one, mixed.
// A sequential print prints each object with its own tool ordering (ToolOrdering(const
// PrintObject&) registers interface_by_extruder exactly like the plate-wide one), so the pass
// now runs for it as well; the only by-object difference is that the free-extruder table is
// built per object (chameleon_collect_layer_filaments' `only`). Print::apply already
// regenerates the support of opted-in objects when print_sequence changes, so no partition
// made under one sequence survives into the other. The
// `support_filament_matching.value` gate (below) is the WHOLE opt-in - v2.4 deleted the
// second per-object mode branch that gate used to feed (the old `nearest_wall_mode`
// local and its else-arm), and v2.6 replaced the manual/nearest_wall enum itself with
// this plain bool, so every object that opts in takes the identical path below, no
// per-object branching left.
static void chameleon_assign_support_interfaces(Print &print)
{
    if (print.extruders().size() <= 1)
        return;
    const bool by_object = print.config().print_sequence == PrintSequence::ByObject;

    PrintObjectPtrs &objects  = print.objects_mutable();

    // CHAMELEON_DEBUG (v2.5c diagnostic instrumentation): checked ONCE per pass
    // invocation, here - not per object, not per layer, not per sample - so every
    // downstream site below gates its own extra work behind this single bool instead
    // of re-checking getenv itself. See the CHAMELEON_DEBUG header comment above
    // chameleon_collect_wall_samples for the log destination/line format this feeds.
    const bool chameleon_debug_on = chameleon_debug_enabled();

    // v2.3 Task 1 (spec C1): once per PASS (not once per object) - every object's walls/
    // solid/sparse infill contribute to the SAME free-extruder table, since the whole
    // point is "is some extruder already printing at this z ANYWHERE on the plate".
    // By-object sequence: built per object inside the loop instead (see the function comment).
    const LayerFilamentTable plate_filament_table = by_object ? LayerFilamentTable{} : chameleon_collect_layer_filaments(print);

    for (size_t obj_idx = 0; obj_idx < objects.size(); ++obj_idx) {
        PrintObject *object = objects[obj_idx];
        if (object == nullptr)
            continue;
        // v2.6: support_filament_matching.value is the WHOLE opt-in - a plain checkbox
        // now that (v2.4) nearest_surface was gone and nearest_wall was the only other
        // enum value left, so there was no longer a second mode to pick between below.
        // Every guard/step below this point (shared-object skip, plate guard, visited
        // flag, apply_bucket_caps, storage, emission, logging) applies uniformly to
        // every opted-in object.
        if (!object->config().support_filament_matching.value)
            continue;
        if (object->layers().empty() || object->support_layers().empty())
            continue;
        // C1 fix, guard (a): a shared-object copy's support_layers() aliases the SAME
        // SupportLayer* vector as the object it was copied from (copy_layers_from_shared_object,
        // Print.cpp: m_support_layers = m_shared_object->support_layers()). The shared
        // source object is processed earlier in this same loop (need_slicing_objects are
        // sliced/support-generated before any copy resolves its shared_object, and
        // Print::process's copy_layers_from_shared_object loop runs before this pass), so
        // its layers are already partitioned/marked by the time we would reach this copy.
        // Skip it outright: cheap, and avoids re-walking every layer only to hit the
        // per-layer chameleon_interface_visited guard below on each one.
        if (object->get_shared_object() != nullptr)
            continue;
        // Ultra (support groups, plan 2026-09-02 Stage 3 3.7): a support group that pins its own
        // support-interface filament writes the very same SupportLayer::interface_by_extruder map
        // this pass owns. Letting both run would make the result depend on which ran last, so the
        // explicit per-part assignment wins and Chameleon stands down for this object.
        // The user-visible warning that says so is raised from PrintObject::generate_support_
        // material(), not here: this pass runs after every object step has finished (see the call
        // site at the end of Print::process), so no PrintObjectStep is active and
        // active_step_add_warning() would have nowhere to put it - m_step_active is -1 there,
        // which its own assert says must not happen and which Release builds do not catch.
        if (object->has_support_group_interface_filament())
            continue;

        // By-object sequence: this object's own free-extruder table (see the function comment).
        const LayerFilamentTable layer_filament_table = by_object ? chameleon_collect_layer_filaments(print, object) : plate_filament_table;

        // Visible walls per object layer (chameleon_visible_wall_pieces), built on first use: a
        // support layer reads its coplanar layer and a contact band of ~16 layers above it, so
        // every object layer is read by many support layers.
        std::vector<std::vector<ChameleonWallPiece>> visible_walls(object->layers().size());
        std::vector<char>                            visible_walls_done(object->layers().size(), 0);
        auto visible_walls_of = [&](size_t li) -> const std::vector<ChameleonWallPiece> & {
            if (!visible_walls_done[li]) {
                visible_walls[li]       = chameleon_visible_wall_pieces(*object->layers()[li]);
                visible_walls_done[li]  = 1;
            }
            return visible_walls[li];
        };
        auto dominant_visible_wall = [&](const std::vector<size_t> &layer_indices, unsigned &out) {
            std::vector<const std::vector<ChameleonWallPiece> *> pieces;
            pieces.reserve(layer_indices.size());
            for (size_t li : layer_indices)
                if (li < object->layers().size())
                    pieces.push_back(&visible_walls_of(li));
            return chameleon_dominant_wall_extruder(pieces, out);
        };

        // The object's surface filament: the filament with the most VISIBLE wall length, sampled
        // over up to 64 evenly spread layers. It replaces chameleon_object_default_extruder (the
        // lowest filament id among first-layer regions with ANY extrusion, infill included) as the
        // object-wide last resort: on a part painted all over in B but assigned A, the unpainted
        // core region still prints infill in A on the first layer, so "lowest id" handed support
        // that nothing else resolved to A whenever A < B. The old value is kept only for an object
        // with no walls at all.
        unsigned object_surface_extruder = chameleon_object_default_extruder(print, *object);
        {
            std::vector<size_t> sample_layers;
            const size_t        n    = object->layers().size();
            const size_t        step = std::max<size_t>(1, n / 64);
            for (size_t li = 0; li < n; li += step)
                sample_layers.push_back(li);
            dominant_visible_wall(sample_layers, object_surface_extruder);
        }

        // Per support layer, a "don't care" support role (support_filament / support_interface_
        // filament = 0) falls back to the SURFACE filament around that layer instead of one
        // object-wide id: the dominant visible wall filament of the layers the support layer is
        // coplanar with, else of its contact band, else the object's surface filament. An
        // explicitly configured filament stays the fallback for its role, as before.
        const bool interface_dontcare = object->config().support_interface_filament.value <= 0;
        const bool base_dontcare      = object->config().support_filament.value <= 0;

        // The filaments this object's parts are ASSIGNED (0-based; 0 in the config means the
        // first filament). See the projection lookups below for what they are needed for.
        std::set<unsigned> assigned_extruders;
        for (const ModelVolume *volume : object->model_object()->volumes)
            if (volume != nullptr && volume->is_model_part())
                assigned_extruders.insert(unsigned(std::max(1, volume->extruder_id()) - 1));

        // Per object layer, built on first use: a sample index of its visible walls.
        std::map<const Layer *, size_t> layer_index_of;
        for (size_t li = 0; li < object->layers().size(); ++li)
            layer_index_of.emplace(object->layers()[li], li);
        std::vector<std::unique_ptr<WallSampleIndex>> visible_wall_index(object->layers().size());
        auto visible_wall_index_of = [&](size_t li) -> const WallSampleIndex & {
            if (!visible_wall_index[li]) {
                visible_wall_index[li] = std::make_unique<WallSampleIndex>();
                for (const ChameleonWallPiece &piece : visible_walls_of(li))
                    visible_wall_index[li]->add_polyline(piece.polyline.points, piece.extruder, obj_idx);
            }
            return *visible_wall_index[li];
        };

        // Painted-part rule for the upward projection (see the projection lookups below): the
        // projection resolves the region directly above a support sample, on `hit_layer`. When
        // that region prints in one of the object's ASSIGNED filaments and no visible wall of that
        // same layer does, it is the unpainted core of a painted part showing through - the paint
        // claim is only so deep, and where it does not cover an underside the core prints its
        // own filament there. On a part imported with the wrong assigned filament and painted
        // over, that filament is exactly the one the support must not take: the sample takes
        // the filament of the nearest visible wall of the hit layer instead - the paint around
        // that underside. An assigned filament that IS on a visible wall of the hit layer (an
        // unpainted part, the unpainted half of a partly painted one) and any painted region's
        // filament are kept. Returns false (a miss) only if the layer has no visible wall at all.
        auto resolve_core_hit = [&](const Layer *hit_layer, const Point &p, unsigned &extruder) -> bool {
            if (hit_layer == nullptr || assigned_extruders.count(extruder) == 0)
                return true;
            const auto it = layer_index_of.find(hit_layer);
            if (it == layer_index_of.end())
                return true;
            for (const ChameleonWallPiece &piece : visible_walls_of(it->second))
                if (piece.extruder == extruder)
                    return true;
            const auto nearest = visible_wall_index_of(it->second).knn(p, 1);
            if (nearest.empty())
                return false;
            extruder = nearest.front().first->extruder;
            return true;
        };
        auto layer_surface_extruder = [&](const std::vector<size_t> &coplanar, const std::vector<size_t> &contact) {
            unsigned e = object_surface_extruder;
            if (!dominant_visible_wall(coplanar, e))
                dominant_visible_wall(contact, e);
            return e;
        };

        // Spec-mandated fallback: the object's resolved interface extruder, mirroring
        // ToolOrdering's own scalar computation (ToolOrdering.cpp ~709) for the case
        // where no per-layer match beats it. (Object-level value: logging and the per-layer
        // default below; each support layer resolves its own, see layer_surface_extruder.)
        const unsigned fallback_extruder = !interface_dontcare
            ? unsigned(object->config().support_interface_filament.value - 1)
            : object_surface_extruder;
        // v2.1 base fallback: mirrors ToolOrdering's own BASE scalar computation
        // (ToolOrdering.cpp ~704: extruder_support = resolve_mixed(object.config().
        // support_filament.value, ...)) the same APPROXIMATE way the interface fallback
        // above mirrors ToolOrdering's interface scalar (~709) - a plain 1-based-to-
        // 0-based scalar read of the config value, not resolve_mixed's per-layer gradient-
        // mix resolution (that's the "mixed filament" gradient-printing feature, orthogonal
        // to this pass's role-mixing). Good enough for a last-resort default when no
        // per-point match beats it, same as the interface case.
        const unsigned base_fallback_extruder = !base_dontcare
            ? unsigned(object->config().support_filament.value - 1)
            : object_surface_extruder;

        // Ascending object-layer TOP z values, for select_contact_layers (VLH-safe: keyed
        // by z overlap, not index arithmetic).
        std::vector<double> layer_print_zs;
        layer_print_zs.reserve(object->layers().size());
        for (const Layer *layer : object->layers())
            layer_print_zs.push_back(layer->print_z);

        const double first_layer_top_z = object->slicing_parameters().first_print_layer_height;

        BrimVoteParams vote_params;
        vote_params.fallback_extruder = fallback_extruder;
        // object_area left empty: every wall sample below carries this object's own
        // ordinal as object_key, so BrimVoteParams::object_area's cross-object tie-break
        // (Part 1's multi-object plate scenario) never applies here.

        // v2.3 Task 1 (spec C6): support-pass-only overrides of two run-building
        // tunables - Part 1's own BrimVoteParams (the brim pass' own object, built
        // separately in Print::process's brim call site further down this file) never
        // touches this `vote_params` instance, so its defaults (max_runs=4,
        // min_run_mm=2.0) stay exactly what they were for brim. Support fills sample
        // far more numerous, far shorter runs per layer than a single long brim loop
        // ever does - many short ring/lattice segments crossing sector boundaries every
        // layer, not one continuous perimeter - so brim's max_runs=4/min_run_mm=2.0
        // guard, tuned for a handful of long brim loops, throttled support's own
        // legitimate per-sector matched runs down to noise. max_runs=8/min_run_mm=1.6
        // give support twice the run budget and a tighter absorb floor, sized to the
        // smaller branch-scale buckets C1's gate rework above now admits (5-30mm ring
        // arcs, 20-60mm small interfaces) instead of brim's long-loop scale.
        // sample_mm stays 0.8 and max_extruders (an apply_bucket_caps call-site
        // argument, not a BrimVoteParams field) stays 2 - spec C6 only touches these
        // two fields.
        vote_params.max_runs   = 8;
        vote_params.min_run_mm = 1.6;
        // v2.3 final-review M4 fix: support-pass-only opt-in to split_polyline_core's
        // ring-seam merge (spec C7, BrimVoteParams::merge_ring_seam, default false) - set
        // HERE, on the shared vote_params instance, before every per-role BrimVoteParams
        // copy (v2.5e: interface_wall_params/base_wall_params, built inside BrimFilament.
        // hpp/.cpp's chameleon_build_support_resolvers now, not inline here) is made from
        // it, so every support engine call keeps the seam merge exactly as before this
        // fix. Part 1's own brim vote_params (built
        // independently, further down this file) never touches this field, so it stays
        // false there and the merge is unreachable for brim loops - restoring the "Part 1
        // brim behavior stays byte-identical" contract the merge previously violated.
        vote_params.merge_ring_seam = true;

        // v2.4 (spec A): the gap-aware lateral cap (outer_wall_width_mm/
        // support_nozzle_diameter_mm/support_line_width_mm/lateral_cap_mm, formerly
        // computed here) was v2.2 Task 2's fix for nearest_surface's OWN lateral/base
        // resolver, which capped how far a support skin could reach for a coplanar wall
        // (BrimVoteParams.max_dist_mm). nearest_wall's resolver has never used a
        // distance cap (max_dist_mm stays 0 = uncapped, spec C8) - "nearest wall wins
        // outright, no distance limit" - so this glue has no caller left now that
        // nearest_surface's else-arm is gone. gap_aware_lateral_cap_mm itself
        // (BrimFilament.hpp/.cpp) is NOT deleted - pure function, unit-tested there,
        // kept for the same v2.5 upward-cast reason as the projection machinery (see
        // this function's own header comment above).
        //
        // v2.2 Task 1 (spec C1-C3): this object's hysteresis state - the extruder set
        // apply_bucket_caps committed on the PREVIOUS support layer that reached the
        // engine calls. Empty at object start. Threaded through every iteration of the
        // loop below; only the guards that `continue` BEFORE the engine calls (plate,
        // already-visited, zero-sample) leave it unchanged.
        // v2.3 Task 1 (spec C2): a layer that DOES reach the engine calls no longer
        // always overwrites `prev_kept` unconditionally - chameleon_update_prev_kept
        // (BrimFilament.hpp/.cpp) decides per-layer whether to commit, retain-for-one-
        // more-layer, or clear to empty; `prev_kept_retained_last_layer` is that pure
        // function's own decay-counter state, carried alongside prev_kept itself.
        std::set<unsigned> prev_kept;
        bool                prev_kept_retained_last_layer = false;
        // v2.3 Task 3 (spec C5): this object's descend-hysteresis state - see
        // BrimVoteParams::descended_last_layer's own comment (BrimFilament.hpp) and
        // partition_support_entities' collection branch (BrimFilament.cpp) for the full
        // mechanism. Threaded through this loop the SAME way prev_kept is (synced into
        // vote_params before the engine calls each iteration that reaches them), but
        // REPLACED wholesale - not merged - after those calls run each layer: a column
        // is only ever written into descended_this_layer (below) the layer it actually
        // descends, so a column that doesn't re-qualify simply has no entry going into
        // the NEXT layer - immediate full-threshold reversion, no multi-layer grace like
        // C2's prev_kept retention (the spec only asked for "halved if descended LAST
        // layer", not a decaying grace).
        DescendColumnMap descended_last_layer;
        size_t layers_partitioned  = 0;
        size_t layers_zero_sample  = 0;
        // v2.2 Task 1: replaces the old cumulative_switches/layers_reverted/escalated
        // counters (deleted with the >3 whole-layer revert and >20 per-object
        // escalation, C1-C2 - a switch-boundary tally never measured the real per-layer
        // cost). These count actual apply_bucket_caps outcomes instead.
        size_t buckets_dropped_min_benefit      = 0; // C3 gate: bucket total length < its tier floor (v2.3 Task 1: 12mm normal / 3mm free)
        // v2.4 Task C (spec C): free-tier SUBSET of buckets_dropped_min_benefit above -
        // cheap to split out (apply_bucket_caps' gate loop already computes `is_free`
        // per bucket, BrimFilament.cpp) and answers a triage question the combined total
        // can't: "did the claw fix's free-tier window still fail to rescue enough
        // length, or is 12mm-normal-tier churn the dominant drop cause?" Normal-tier
        // drops are buckets_dropped_min_benefit - buckets_dropped_min_benefit_free, not
        // tracked as its own field since it's one subtraction away.
        size_t buckets_dropped_min_benefit_free = 0;
        size_t buckets_trimmed_cap              = 0; // C1 trim: bucket ranked below the top 2
        // v2.5a Task 2 (spec item 2, residual-paint fix): SUBSET of the two counters
        // above - how many of those gated/trimmed buckets landed inside a surviving
        // MATCHED bucket (apply_bucket_caps' nearest-centroid redirect) rather than
        // falling all the way back to residual support_fills. Always <=
        // buckets_dropped_min_benefit + buckets_trimmed_cap; the gap between them is
        // the legacy no-survivor fallback count (still residual, same as pre-v2.5a).
        size_t buckets_redirected               = 0;
        // v2.5b (spec: "free-extruder trim exemption"): how many buckets skipped the
        // C1 trim entirely because their extruder was already paying for a toolchange
        // at THIS layer's own z from model geometry (apply_bucket_caps'
        // free_extruders_exempt argument, strict-coincidence set below) - triage
        // signal for "is the trim exemption actually firing on this plate".
        size_t buckets_exempt_kept              = 0;
        // Raw matched-run counts (NOT switch-boundary/cap accounting - purely
        // informational, mirrors partition_support_entities' own return value) per
        // role, summed across every layer that reached the engine calls.
        size_t interface_runs_matched = 0;
        size_t base_runs_matched      = 0;
        // v2.2 Task 3 (spec C6): third role's own raw matched-run count, same
        // informational-only accounting as the two above. A whole nested collection
        // moved by C7 doesn't contribute to this (partition_support_entities only
        // counts split-run boundaries), so this undercounts relative to "how much
        // ironing actually matched" whenever C7 moves a collection - documented, not a
        // bug: nothing downstream keys caps off this number (v2.2 Task 1 already made
        // that true for interface/base too).
        size_t ironing_runs_matched   = 0;
        // v2.5e: per-object totals of the per-layer proj_hits/proj_misses debug counts
        // (see the CHAMELEON_DEBUG header comment above for the field's own meaning) -
        // summed here the same way the raw matched-run counts above are, so the SUMMARY/
        // BOOST_LOG_TRIVIAL lines carry an object-wide "how often did the upward cast
        // actually resolve a sample" signal without a triage reader needing to sum every
        // per-layer line themselves. Accumulated unconditionally (cheap integer adds),
        // same as every other counter in this block - only the LOGGING of them is gated
        // behind chameleon_debug_on/BOOST_LOG_TRIVIAL's own severity filter. v2.5f: this
        // pair's own totals now include base's share too (see the CHAMELEON_DEBUG header
        // comment's v2.5f paragraph) - `interface_proj_hits`/`interface_proj_misses`
        // keeps its v2.5e name for grep-stability across log history, but as of this task
        // it is an ALL-ROLES total, not an interface-only one; base_proj_hits/
        // base_proj_misses just below is the isolated per-role breakdown this task adds.
        size_t interface_proj_hits    = 0;
        size_t interface_proj_misses  = 0;
        // v2.5f (v2.5e review M3 fix, base half): base's own isolated share of the
        // totals above, accumulated the same way - see the per-layer snapshot site
        // (proj_hits_before_base, further down in this function) for how this is
        // isolated out of the shared counters without teaching chameleon_build_support_
        // resolvers' projection_lookup seam about roles.
        size_t base_proj_hits         = 0;
        size_t base_proj_misses       = 0;

        // Plate layers (see the plate guard below), resolved after this loop once the layer above
        // them has its final colours.
        std::vector<SupportLayer *> plate_layers;

        for (SupportLayer *support_layer : object->support_layers()) {
            if (support_layer == nullptr || support_layer->support_fills.entities.empty())
                continue;
            // Plate guard: the first support layer (touching the build plate) is never
            // partitioned - never split its plate adhesion, and its geometry stays in
            // support_fills where the brim, skirt and first-layer island code read it. It is
            // not left to "don't care" any more either (that printed the support brim pads in
            // whatever filament the first layer happened to start with - neither the part's nor
            // the support's): see the plate-layer block after this loop.
            if (support_layer->print_z <= first_layer_top_z + EPSILON) {
                plate_layers.push_back(support_layer);
                continue;
            }
            // C1 fix, guard (b): this layer was already visited by an earlier pass over
            // the SAME SupportLayer object - either the shared source this object's copy
            // aliases (belt-and-suspenders backstop for guard (a) above, in case that
            // ordering assumption is ever violated by a future refactor), or an earlier
            // Print::process() run whose posSupportMaterial fast-pathed to done (so
            // support_fills still carries that run's mutations: matched originals deleted,
            // fallback-voted runs re-inserted/re-split at vote boundaries). Re-voting
            // those already-mutated runs is not idempotent: the re-split resamples from a
            // different phase and can report shorter/fewer per-bucket runs than pass 1
            // really found even though pass 1's apply_bucket_caps already merged the
            // excess back (defeating its length ranking - see the "re-run undercounts"
            // unit test's own comment for the mechanism), or come back non-empty and
            // overwrite interface_by_extruder outright, silently dropping pass 1's
            // matched geometry (it lives nowhere else - removed from support_fills by
            // design). Skip unconditionally instead of re-running.
            if (support_layer->chameleon_interface_visited)
                continue;

            // v2.3 Task 1 (spec C3): sync vote_params.prev_kept to THIS layer's current
            // hysteresis state BEFORE the resolvers below copy vote_params by value
            // (interface_wall_params/base_wall_params, both `= vote_params`) - those
            // copies are taken fresh every iteration of this loop, but `vote_params`
            // itself is constructed ONCE per OBJECT, above this loop, so without this
            // line every layer's resolvers would see whatever prev_kept happened to be
            // set at object-start (empty) forever, never the value this loop updates at
            // the bottom of each iteration. Also feeds the three
            // partition_support_entities calls' own `p` argument further down (still
            // `vote_params` itself, unchanged), which is what vote_collection_as_unit's
            // own tie path reads for C7 whole-collection votes.
            vote_params.prev_kept = prev_kept;

            // v2.3 Task 3 (spec C5): synced the same way, same reason - vote_params is
            // constructed once per OBJECT, this loop updates descended_last_layer once
            // per LAYER, so without this every layer's engine calls would see whatever
            // descended_last_layer happened to be at object-start (empty) forever.
            vote_params.descended_last_layer = descended_last_layer;

            // v2.3 Task 1 (spec C1): this layer's free-extruder set - the once-per-pass
            // z-table's WINDOWED lookup at this support layer's own print_z. Resolved
            // once per layer (not per bucket) since it depends only on z, feeds the
            // apply_bucket_caps call at the tail of this iteration.
            //
            // v2.4 Task B (spec B, the claw fix): down_mm/up_mm replace the old exact-z
            // query - a white claw wall that exists only in the contact band ABOVE its
            // support column (z, z + kContactBandMm] was never "free" at the support
            // layer's OWN z under strict coincidence, even though the vote above (a)/(b)
            // samples that same band; every short white bucket then faced the 12mm
            // normal-tier floor and merged back to fallback. down_mm = support_layer->
            // height (interval-overlap correction, mirroring coplanar_idx's own
            // rationale above, for unsynced support/object layer grids); up_mm =
            // kContactBandMm - the SAME constant contact_idx's own select_contact_layers
            // call uses (hoisted, BrimFilament.hpp) - so free-eligibility covers exactly
            // the band the vote can actually match against, no more and no less.
            const std::set<unsigned> free_extruders = chameleon_layer_free_extruders(
                layer_filament_table, support_layer->print_z,
                /*down_mm=*/support_layer->height, /*up_mm=*/kContactBandMm);

            // v2.5b (spec: "free-extruder trim exemption"): a SECOND, STRICTER free-
            // extruder query for apply_bucket_caps' new free_extruders_exempt argument
            // below - deliberately NOT the same `free_extruders` set above. See
            // apply_bucket_caps' own doc comment (BrimFilament.hpp) for the full
            // rationale; short version: up_mm = 0.0 here (vs. kContactBandMm above)
            // because the trim exemption's whole premise is "this bucket's toolchange
            // is already free AT THIS LAYER", which is only true when the extruder is
            // already going to land in THIS support layer's own LayerTools from model
            // geometry (ToolOrdering.cpp's collect_extruders, ~734-736, deduped against
            // model geometry by sort_remove_duplicates/remove_duplicates_preserve_order,
            // ~870-874) - true only for an extruder that prints AT this exact z
            // somewhere on the plate, never for one that only prints on a HIGHER object
            // layer inside the contact band above (the up-window's own reach). down_mm
            // stays support_layer->height, unchanged from free_extruders above - that
            // term only corrects for THIS layer's own z-thickness / unsynced object-
            // layer grids, it doesn't reach into the future the way up_mm does, so it
            // stays part of the "strict, this-layer-only" query too.
            const std::set<unsigned> free_extruders_exempt = chameleon_layer_free_extruders(
                layer_filament_table, support_layer->print_z,
                /*down_mm=*/support_layer->height, /*up_mm=*/0.0);

            // (a) Contact-band layers: v2.0-style band (support_top_z, support_top_z +
            // kContactBandMm]) - unioned below with the coplanar band (b) into ONE
            // wall-sample index (v2.2 Task 4, spec C8; v2.4 spec A made this the only
            // path, deleting nearest_surface's separate use of this list for a
            // projection view - see this function's own header comment).
            std::vector<size_t> contact_idx = select_contact_layers(layer_print_zs, support_layer->print_z, kContactBandMm);

            // v2.5g (spec: "base projection depth limit" - GUI round 8): this support
            // layer's own base_cast_mm, honestly derived from the ACTUAL heights of its
            // leading contact_idx layers (contact_idx is already ascending/lowest-first -
            // see chameleon_base_cast_mm's own header comment in BrimFilament.hpp for why
            // MINIMUM-of-the-first-few rather than the first layer alone or an average),
            // not a config scalar. Only the first kBaseCastMaxLayers entries are ever
            // read - chameleon_base_cast_mm itself only ever looks at that many - so this
            // reserves/collects at most 4 doubles per support layer regardless of how
            // deep contact_idx runs. Empty contact_idx (support above the model's own top
            // walls - the same case coplanar_wall_idx already tolerates) yields an empty
            // `contact_layer_heights` and base_cast_mm = 0.0, which correctly zeroes out
            // base_view_count below.
            std::vector<double> contact_layer_heights;
            contact_layer_heights.reserve(std::min(contact_idx.size(), kBaseCastMaxLayers));
            for (size_t i = 0; i < contact_idx.size() && i < kBaseCastMaxLayers; ++i)
                contact_layer_heights.push_back(object->layers()[contact_idx[i]]->height);
            const double base_cast_mm = chameleon_base_cast_mm(contact_layer_heights);

            // (b) Coplanar lateral band: object layers whose z-INTERVAL OVERLAPS this
            // support layer's OWN span (print_z - height, print_z] - v2.1's "any support
            // within 1mm of a wall at its own layer matches that wall" rule (spec's
            // Decision rules, "Lateral rule - all roles").
            //
            // v2.1 final-review I2 fix: select_layers_overlapping_span, not
            // select_layers_in_band - this band is only ONE support-layer tall, so a
            // pure top-z-in-band test can miss an object layer whose walls flank the
            // band even though its own top overshoots hi_z (unsynced support/object
            // layer grids, variable layer height). The contact band above (contact_idx,
            // select_contact_layers -> select_layers_in_band) is deliberately left
            // as-is: its kContactBandMm width is wider than any single layer, so the
            // top-z deviation the v2.0 review accepted for it still holds.
            std::vector<size_t> coplanar_idx = select_layers_overlapping_span(
                layer_print_zs, support_layer->print_z - support_layer->height, support_layer->print_z,
                // Raft-aware first-layer bottom (re-review N2): with a raft the object's
                // first layer starts well above the plate, and raft-level support layers
                // must not coplanar-match layer 0's walls.
                object->layers().front()->print_z - object->layers().front()->height);

            // v2.4 (spec A): un-nested from the old per-object mode branch - nearest_wall
            // is the only path left, so band_idx/coplanar_wall_idx/interface_resolver/
            // base_resolver are populated unconditionally below instead of by one of two
            // branches. interface_resolver/base_resolver/ironing_resolver (v2.5e) stay
            // std::function (not `auto`) since the three-engine-call tail further down
            // just calls them, agnostic to how they were built (chameleon_build_support_
            // resolvers, BrimFilament.hpp/.cpp, as of v2.5e - see that function's own
            // header comment for the wiring these three now come from). BrimVoteParams is
            // captured BY VALUE (cheap, immutable after construction) so it never
            // dangles; band_idx/coplanar_wall_idx/projection_view/projection_view_layers
            // are captured BY REFERENCE and so are declared here, at the per-layer scope
            // that outlives both the construction below and the engine calls that use
            // the resolvers.
            //
            // v2.5d (root cause fix, debug-log-driven - GUI round 5, transition zone z
            // 34.62-42.18): pre-v2.5d, interface AND base both voted against the SAME
            // single WallSampleIndex - the union of the coplanar layer and the 16-layer
            // contact band above it. That union is the RIGHT electorate for an interface
            // (an interface touches the surface ABOVE it, so the contact band's walls
            // are genuinely relevant), but WRONG for a base/wrap run, which abuts the
            // wall AT ITS OWN Z: the claw tapers, so a higher (whiter) wall's samples
            // land XY-inward - and, in the flattened 2D index, farther - while a teal
            // wall genuinely coplanar with a LOWER support layer can still sit XY-nearer
            // to a query point on a higher, white-zone layer. k=1 always picks the
            // nearest sample regardless of which band contributed it, so the teal wrap
            // bled upward past the diagonal paint transition even though every bucket
            // otherwise formed correctly (engine healthy, nothing gated). Fix: split the
            // single index into two - `band_idx` (unchanged: union of contact_idx and
            // coplanar_idx, still feeds interface/ironing) and `coplanar_wall_idx`
            // (coplanar_idx layers ONLY, feeds base) - so a base run only ever matches a
            // wall that is genuinely at its own z.
            WallSampleIndex                        band_idx;
            WallSampleIndex                        coplanar_wall_idx;
            bool                                    zero_sample = false;
            std::function<unsigned(const Point &)> interface_resolver;
            std::function<unsigned(const Point &)> base_resolver;
            std::function<unsigned(const Point &)> ironing_resolver;

            // v2.5e: the projection band's ProjectionLayerView list, built once per
            // support layer (below, inside the `!zero_sample` block) by
            // chameleon_build_projection_views over `contact_idx` ONLY (the contact-band
            // layers - mirrors nearest_surface's pre-v2.4 own usage of this same
            // function; NOT the coplanar+contact union band_idx votes against - a
            // projection answers "what's directly above", which is a contact-band
            // question, not a lateral one). Declared here (not inside the block below)
            // for the same "outlives the resolvers that reference it" reason band_idx/
            // coplanar_wall_idx are.
            std::vector<ProjectionLayerView> projection_view;
            std::vector<const Layer *>       projection_view_layers;

            // CHAMELEON_DEBUG: per-band-layer, per-extruder wall sample counts feeding
            // band_idx/coplanar_wall_idx below - the (a)-vs-(b) discriminator from the
            // round-4 refutation (progress.md v2.5a section): if a claw's own extruder
            // is ZERO here at its claw layers, the bucket dies at sampling/selection
            // (mechanism a), not at the vote or the gate. v2.5d: tracked SEPARATELY per
            // index now (debug_band_samples/debug_coplanar_samples) - see the loop below
            // for how a coplanar_idx layer contributes to both. Populated only when
            // chameleon_debug_on (see chameleon_collect_wall_samples' own trailing
            // parameter and WallSampleIndex::add_polyline's own comment) - an empty
            // vector plus one pointer check per li when off.
            std::vector<std::pair<size_t, std::map<unsigned, size_t>>> debug_band_samples;
            std::vector<std::pair<size_t, std::map<unsigned, size_t>>> debug_coplanar_samples;

            // v2.5e: this layer's projection hit/miss counts - see the CHAMELEON_DEBUG
            // header comment (proj_hits=/proj_misses=) for the full field meaning.
            // Incremented unconditionally by projection_lookup below (cheap integer add
            // per sample, same "always compute, only conditionally log" convention every
            // other counter in this loop already follows - see interface_proj_hits'/
            // interface_proj_misses' own per-object declaration comment above this
            // loop). v2.5f: base_resolver now shares this SAME pair too (it consults the
            // same projection_lookup, built once below and reused by all three role
            // resolvers) - the per-role snapshot that isolates base's own share back out
            // (proj_hits_before_base) sits at the base partition_support_entities call
            // site further down, not here, since only the caller of that specific engine
            // call knows "this is base's own turn".
            size_t debug_projection_hits   = 0;
            size_t debug_projection_misses = 0;

            // v2.2 Task 4 (spec C8): band_idx over the UNION of the contact-band layers
            // (a) and the coplanar layers (b) - dedupe via union_layer_indices
            // (BrimFilament.hpp/.cpp) so a layer index selected by both bands doesn't
            // double-weight its walls in brim_vote's 1/d^2 scoring. The projection view
            // (projection_view/projection_view_layers, declared above) is built
            // separately, below, over `contact_idx` alone - see this function's own
            // header comment (v2.5e section) for why interface/ironing consult both.
            //
            // v2.5d: single pass over the union - a layer that is ALSO in coplanar_idx
            // (membership tested via `coplanar_idx_set`, built once here since
            // coplanar_idx is already ascending/duplicate-free per
            // select_layers_overlapping_span's own contract) additionally samples into
            // coplanar_wall_idx, so a contact-band-only layer (contributes to band_idx
            // alone) is never charged the cost of a second, pointless lookup, and a
            // coplanar layer is sampled into both indices exactly once each.
            const std::set<size_t> coplanar_idx_set(coplanar_idx.begin(), coplanar_idx.end());
            for (size_t li : union_layer_indices(contact_idx, coplanar_idx)) {
                std::map<unsigned, size_t>  debug_band_li_counts;
                std::map<unsigned, size_t> *debug_band_li_counts_ptr = chameleon_debug_on ? &debug_band_li_counts : nullptr;
                // Visible walls only (chameleon_visible_wall_pieces): no inner walls, no colour-
                // boundary loops inside a painted part. Supports and walls share object
                // coordinates, so no instance shift.
                const std::vector<ChameleonWallPiece> &pieces = visible_walls_of(li);
                for (const ChameleonWallPiece &piece : pieces)
                    band_idx.add_polyline(piece.polyline.points, piece.extruder, obj_idx, /*spacing_mm=*/0.8, debug_band_li_counts_ptr);
                if (chameleon_debug_on)
                    debug_band_samples.emplace_back(li, std::move(debug_band_li_counts));

                if (coplanar_idx_set.count(li)) {
                    std::map<unsigned, size_t>  debug_coplanar_li_counts;
                    std::map<unsigned, size_t> *debug_coplanar_li_counts_ptr = chameleon_debug_on ? &debug_coplanar_li_counts : nullptr;
                    for (const ChameleonWallPiece &piece : pieces)
                        coplanar_wall_idx.add_polyline(piece.polyline.points, piece.extruder, obj_idx, /*spacing_mm=*/0.8, debug_coplanar_li_counts_ptr);
                    if (chameleon_debug_on)
                        debug_coplanar_samples.emplace_back(li, std::move(debug_coplanar_li_counts));
                }
            }

            // v2.5d: skip the whole layer (keep fallback, same as pre-v2.5d) only when
            // BOTH indices are empty - band_idx is a strict superset of coplanar_wall_idx
            // (every coplanar_idx layer is also a member of union_layer_indices(contact_
            // idx, coplanar_idx), so it is sampled into band_idx too; coplanar_wall_idx
            // non-empty therefore implies band_idx non-empty), so this condition can only
            // ever reduce to band_idx.empty() in practice - checked as an explicit `&&`
            // anyway (not relied on as a silent invariant) so a future change to either
            // selector's relationship fails safe rather than silently skipping a layer
            // that still had real interface/ironing geometry to match. A layer where
            // band_idx is non-empty but coplanar_wall_idx is empty - support above the
            // model's own top walls, nothing coplanar under it at all - is NOT skipped
            // here: interface/ironing still resolve normally against band_idx, and
            // base_resolver (built below) uniformly returns base_fallback_extruder for
            // that empty index (brim_vote's own empty-knn_result fallback, verified in
            // the "v2.5d: empty coplanar_idx..." unit test) - a real, deterministic
            // bucket like any other (v2.5c: fallback buckets too), not a special case.
            zero_sample = band_idx.empty() && coplanar_wall_idx.empty();

            // This layer's own "nothing matched" answer for a don't-care role (see
            // layer_surface_extruder above); an explicitly configured filament keeps its role.
            const unsigned layer_surface_extruder_id = layer_surface_extruder(coplanar_idx, contact_idx);
            const unsigned layer_fallback_extruder      = interface_dontcare ? layer_surface_extruder_id : fallback_extruder;
            const unsigned layer_base_fallback_extruder = base_dontcare ? layer_surface_extruder_id : base_fallback_extruder;
            vote_params.fallback_extruder = layer_fallback_extruder;

            // CHAMELEON_DEBUG: common per-layer line prefix, built once here so both the
            // zero-sample early-exit just below and the full engine-call path further
            // down emit the same fields - only the trailing `buckets=` segment differs
            // (a zero-sample layer never reaches apply_bucket_caps, so it logs an empty
            // buckets field instead of no line at all - "a contact/coplanar band was
            // selected here but nothing sampled anywhere in it" is itself part of the
            // (a)-mechanism signal this instrumentation exists to surface).
            std::string chameleon_debug_line;
            if (chameleon_debug_on) {
                chameleon_debug_line =
                    "obj="            + std::to_string(obj_idx) +
                    " print_z="       + chameleon_debug_format_mm(support_layer->print_z) +
                    " height="        + chameleon_debug_format_mm(support_layer->height) +
                    " fallback="      + std::to_string(layer_fallback_extruder) +
                    " base_fallback=" + std::to_string(layer_base_fallback_extruder) +
                    " free_windowed=" + chameleon_debug_format_ids(free_extruders) +
                    " free_strict="   + chameleon_debug_format_ids(free_extruders_exempt) +
                    // v2.5g: base's own projection depth limit for THIS layer - see this
                    // function's own header comment (base_cast_mm= field) for why this is
                    // logged unconditionally, ahead of the zero-sample early-exit below.
                    " base_cast_mm="  + chameleon_debug_format_mm(base_cast_mm) +
                    " contact_layers="  + chameleon_debug_format_indices(contact_idx) +
                    " coplanar_layers=" + chameleon_debug_format_indices(coplanar_idx) +
                    // v2.5d: split from the old single `wall_samples=` field -
                    // band_samples is band_idx's per-layer counts (interface/ironing's
                    // electorate, same shape/values the old field always had); coplanar_
                    // samples is coplanar_wall_idx's own (base's electorate, v2.5d new) -
                    // a claw layer whose coplanar_samples is empty for a color that IS
                    // present in band_samples is exactly the "base correctly excludes
                    // the contact-band-only wall" signal this split exists to surface.
                    " band_samples="     + chameleon_debug_format_wall_samples(debug_band_samples) +
                    " coplanar_samples=" + chameleon_debug_format_wall_samples(debug_coplanar_samples);
            }

            if (!zero_sample) {
                // v2.5e: build the projection view for THIS support layer's contact
                // band (contact_idx alone - see projection_view's own declaration
                // comment above) ONCE here, the same M1 hoisting rationale as band_idx/
                // coplanar_wall_idx's own per-layer sampling loop above - interface and
                // ironing's resolver consults chameleon_projection_extruder_from_view
                // once per 0.8mm sample point below (via projection_lookup), and
                // rebuilding this view per sample (offset_ex + per-region polygon-
                // pointer collection) would be wasteful.
                chameleon_build_projection_views(*object, contact_idx, projection_view, projection_view_layers);

                // v2.5g (spec: base projection depth limit): base_view_count is how many
                // of contact_idx's LEADING (lowest-first) entries fall within the
                // shallower base_cast_mm gap - i.e. top_z in (support_layer->print_z,
                // support_layer->print_z + base_cast_mm]. contact_idx is walked directly
                // (not re-selected via a second select_contact_layers call) since
                // projection_view/projection_view_layers were just built 1:1 over
                // contact_idx in that SAME order (chameleon_build_projection_views' own
                // contract, modulo defensive skips - see the clamp below) - "slicing the
                // vector prefix is the cheap honest form" (spec) only holds if contact_idx
                // really is ascending by top_z here, so that assumption is asserted, not
                // assumed: base_cast_mm <= kContactBandMm always (both are gaps measured
                // from the same support_layer->print_z origin), and select_layers_in_band
                // (which select_contact_layers calls) scans print_zs ascending and
                // appends in that same order, so the set of entries satisfying the
                // TIGHTER bound is necessarily contact_idx's own leading prefix - once one
                // entry's top_z exceeds base_cast_mm's threshold, every later (higher-z,
                // by the assert below) entry does too, so the loop can safely `break`
                // rather than scan the rest.
                size_t base_view_count = 0;
                while (base_view_count < contact_idx.size()) {
                    const double top_z = layer_print_zs[contact_idx[base_view_count]];
                    assert(base_view_count == 0 ||
                           top_z + EPSILON >= layer_print_zs[contact_idx[base_view_count - 1]]);
                    if (top_z > support_layer->print_z + base_cast_mm + EPSILON)
                        break;
                    ++base_view_count;
                }
                // Clamp to projection_view's own size: chameleon_build_projection_views
                // skips an out-of-range/null layer index defensively (never expected in
                // practice - see that function's own comment), which would otherwise let
                // a contact_idx-derived count outrun the parallel projection_view/
                // projection_view_layers vectors chameleon_projection_extruder_from_view's
                // `layer_limit` actually indexes into.
                base_view_count = std::min(base_view_count, projection_view.size());

                // v2.5e: projection_lookup wraps chameleon_projection_extruder_from_view
                // as the narrow `bool(const Point&, unsigned&)` seam chameleon_build_
                // support_resolvers (BrimFilament.hpp/.cpp) actually takes - see that
                // function's own header comment for why this boundary (rather than the
                // raw projection_view/projection_view_layers pair) is what got
                // extracted. Captures projection_view/projection_view_layers BY
                // REFERENCE (both outlive this lambda - same per-layer-scope lifetime
                // band_idx/coplanar_wall_idx already rely on). CHAMELEON_DEBUG: also
                // tallies debug_projection_hits/misses (see that pair's own declaration
                // comment above) - counted HERE, not inside chameleon_build_support_
                // resolvers, since a shared std::function/counter pair like this one
                // needs no role knowledge at all to tally correctly (v2.5f: now shared
                // by interface, ironing, AND base alike - see chameleon_build_support_
                // resolvers' own generic projection_lookup parameter's header comment
                // for why it deliberately has no reason to know which role is calling).
                // Per-role attribution (base_proj_hits/base_proj_misses), when it's
                // needed, is recovered further down by snapshotting these same shared
                // counters around the specific engine call that's currently running -
                // see proj_hits_before_base's own comment at that call site.
                //
                // Painted-part rule (fix/support-filament-matching-paint-brim): every hit goes
                // through resolve_core_hit (above the per-layer loop), so the unpainted core of a
                // painted part never hands its assigned filament to the support under it.
                std::function<bool(const Point &, unsigned &)> projection_lookup =
                    [&projection_view, &projection_view_layers, &debug_projection_hits,
                     &debug_projection_misses, &resolve_core_hit](const Point &p, unsigned &out_extruder) -> bool {
                        const Layer *hit_layer = nullptr;
                        const bool   hit       = chameleon_projection_extruder_from_view(
                            projection_view, projection_view_layers, p, out_extruder,
                            std::numeric_limits<size_t>::max(), &hit_layer) &&
                            resolve_core_hit(hit_layer, p, out_extruder);
                        if (hit)
                            ++debug_projection_hits;
                        else
                            ++debug_projection_misses;
                        return hit;
                    };

                // v2.5g (spec: base projection depth limit): base's OWN projection lookup
                // - same seam shape as projection_lookup above, same shared projection_
                // view/projection_view_layers (no second geometry build), but with
                // `layer_limit=base_view_count` forwarded to chameleon_projection_extruder_
                // from_view/chameleon_pick_projection_region (BrimFilament.hpp/.cpp) so
                // base can only ever hit a band layer within its own shallower reach - a
                // surface further away is invisible to this lookup even though the SAME
                // point would still hit via the unlimited `projection_lookup` above (that
                // is precisely interface/ironing's unchanged full-depth behavior). Tallies
                // into the SAME shared debug_projection_hits/misses pair projection_lookup
                // does (v2.5f precedent: these two counters have counted all three roles
                // combined since base's own projection tier was introduced; base's
                // isolated share is still recovered downstream via the existing before/
                // after snapshot around base's own partition_support_entities call,
                // unaffected by which of the two lookups actually produced the hit/miss).
                std::function<bool(const Point &, unsigned &)> base_projection_lookup =
                    [&projection_view, &projection_view_layers, base_view_count, &debug_projection_hits,
                     &debug_projection_misses, &resolve_core_hit](const Point &p, unsigned &out_extruder) -> bool {
                        const Layer *hit_layer = nullptr;
                        const bool   hit       = chameleon_projection_extruder_from_view(
                            projection_view, projection_view_layers, p, out_extruder, base_view_count, &hit_layer) &&
                            resolve_core_hit(hit_layer, p, out_extruder);
                        if (hit)
                            ++debug_projection_hits;
                        else
                            ++debug_projection_misses;
                        return hit;
                    };

                // v2.5e/v2.5f/v2.5g: the actual resolver-construction wiring - interface/
                // ironing try PROJECTION FIRST via projection_lookup (full contact-band
                // depth, unchanged), base via base_projection_lookup (v2.5g's shallower,
                // depth-limited view of the SAME underlying geometry) - each falling
                // through on a miss to its own nearest-wall vote (interface/ironing:
                // band_idx; base: coplanar_wall_idx, never band_idx) - lives in
                // BrimFilament.hpp/.cpp's chameleon_build_support_resolvers (v2.5d
                // final-review I1 fix: pulled out specifically so a unit test can pin
                // this wiring, not just the brim_vote/partition_support_entities
                // primitives it composes). See that function's own header comment for
                // the full decision rule, the v2.5f taper note, the v2.5g depth-limit
                // paragraph, and the k=1/max_dist_mm=0 role-param construction this call
                // replaces (formerly built inline, right here, pre-v2.5e).
                const ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
                    band_idx, coplanar_wall_idx, projection_lookup, base_projection_lookup, vote_params,
                    layer_fallback_extruder, layer_base_fallback_extruder);
                interface_resolver = resolvers.interface_resolver;
                base_resolver      = resolvers.base_resolver;
                ironing_resolver   = resolvers.ironing_resolver;
            }

            if (zero_sample) {
                // Nothing to project onto / vote on for EITHER index (see the
                // zero_sample assignment above for why band_idx.empty() is the only way
                // both end up empty in practice) -> both roles' resolvers would
                // trivially return their own fallback for every sample; skip the
                // (pointless) engine calls and keep the whole layer on fallback, same as
                // v2.0's zero-sample skip. v2.2: this `continue` is BEFORE the engine
                // calls, so it's one of the "layers that skip partitioning" the
                // hysteresis contract (above) leaves prev_kept unchanged for.
                if (chameleon_debug_on)
                    chameleon_debug_log(chameleon_debug_line +
                        " proj_hits=0 proj_misses=0 proj_hits_base=0 proj_misses_base=0 buckets=");
                ++layers_zero_sample;
                // The support stays in support_fills; its don't-care roles print in this
                // layer's surface filament instead of whatever extruder is active
                // (SupportLayer::chameleon_residual_extruder).
                support_layer->chameleon_residual_extruder = int(layer_surface_extruder_id);
                support_layer->chameleon_interface_visited = true;
                continue;
            }

            // Three engine calls sharing ONE out map (spec: interface, then base, then
            // v2.2 Task 3's ironing). Each call only ever touches entities whose role()
            // matches its own role_filter (Task 1's partition_support_entities; v2.2
            // Task 3's C7 extends this to a nested collection's COLLAPSED role() too), so
            // running these sequentially over the same support_fills is safe: each call's
            // entities.swap() rebuild leaves every entity of a DIFFERENT role exactly
            // where it was for the next call to then walk.
            std::map<unsigned, ExtrusionEntityCollection> partitioned;
            // v2.3 Task 3 (spec C5): fresh accumulator for THIS layer's descends, shared
            // across all three calls below the same way `partitioned` itself is -
            // replaces `descended_last_layer` wholesale once this layer's processing
            // finishes (see the assignment further down, mirroring prev_kept's own
            // update site).
            DescendColumnMap descended_this_layer;

            const size_t interface_switches = partition_support_entities(support_layer->support_fills,
                erSupportMaterialInterface, layer_fallback_extruder, interface_resolver, vote_params, partitioned,
                &descended_this_layer);

            // v2.5f (v2.5e review M3 fix, base half): debug_projection_hits/misses are
            // shared by ALL THREE role resolvers now (base joined interface/ironing this
            // task - see that pair's own per-layer declaration comment above), so a
            // before/after snapshot taken around ONE specific engine call is how this
            // loop recovers that one role's own isolated share, without teaching
            // chameleon_build_support_resolvers' projection_lookup seam about roles
            // (its own header comment explains why that seam stays deliberately
            // generic). Safe with no extra synchronization: this per-layer loop, and the
            // per-object loop it lives inside (chameleon_assign_support_interfaces),
            // are both plain sequential `for` loops, not tbb::parallel_for - the three
            // partition_support_entities calls below run strictly one after another on
            // one thread, so nothing else can touch debug_projection_hits/misses between
            // this snapshot and the one right after base's own call.
            const size_t proj_hits_before_base   = debug_projection_hits;
            const size_t proj_misses_before_base = debug_projection_misses;

            const size_t base_switches = partition_support_entities(support_layer->support_fills,
                erSupportMaterial, layer_base_fallback_extruder, base_resolver, vote_params, partitioned,
                &descended_this_layer);

            // v2.5f: base's own isolated projection hit/miss count for THIS layer - see
            // proj_hits_before_base's own comment just above for the snapshot mechanism.
            // (debug_projection_hits/misses themselves are NOT reset here - they keep
            // accumulating through the ironing call below too, exactly as before v2.5f,
            // since the aggregate proj_hits=/proj_misses= log field is still meant to
            // cover every role combined.)
            const size_t base_proj_hits_this_layer   = debug_projection_hits   - proj_hits_before_base;
            const size_t base_proj_misses_this_layer = debug_projection_misses - proj_misses_before_base;

            // v2.2 Task 3 (spec C6, root cause 5): "ironing follows its interface" -
            // erIroning entities are the ironed top surface of a matched interface run,
            // not a role with its own independent vertical/lateral rule, so this call
            // reuses ironing_resolver/fallback_extruder/vote_params VERBATIM (same three
            // arguments the interface call above passes, modulo the resolver - "vote_
            // params_interface" in the plan/spec's own wording is this same shared
            // `vote_params`, not a separate object). v2.5e: ironing_resolver, not
            // interface_resolver directly - the two are the SAME callable by construction
            // (chameleon_build_support_resolvers, BrimFilament.hpp/.cpp - see
            // ChameleonSupportResolvers::ironing_resolver's own comment), so this is a
            // literal no-op change in behavior, but it is what actually exercises the
            // struct field a unit test pins, rather than silently relying on interface_
            // resolver and ironing_resolver happening to hold equal values. Runs BEFORE
            // apply_bucket_caps below, into the SAME `partitioned` map, so an ironing
            // bucket is gated/trimmed as part of the same per-layer distinct-extruder cap
            // as the interface/base buckets it shares an extruder with - not a separate
            // budget (v22-task-1-report.md's own concern for Task 3, confirmed still
            // correct: apply_bucket_caps' total_path_length_mm already recurses into any
            // nested collection a bucket might hold, per C7).
            const size_t ironing_switches = partition_support_entities(support_layer->support_fills,
                erIroning, layer_fallback_extruder, ironing_resolver, vote_params, partitioned,
                &descended_this_layer);

            interface_runs_matched += interface_switches;
            base_runs_matched      += base_switches;
            ironing_runs_matched   += ironing_switches;
            // v2.5e/v2.5f: accumulate this layer's projection hit/miss counts (already
            // fully tallied by now - interface, base, AND ironing's
            // partition_support_entities calls above have all finished sampling) into
            // the object-wide totals; see interface_proj_hits'/interface_proj_misses'
            // own declaration comment above this loop for why this pair is an ALL-ROLES
            // total as of v2.5f, not an interface-only one.
            interface_proj_hits   += debug_projection_hits;
            interface_proj_misses += debug_projection_misses;
            // v2.5f: base's own isolated share, accumulated the same way - see
            // base_proj_hits'/base_proj_misses' own per-object declaration comment.
            base_proj_hits   += base_proj_hits_this_layer;
            base_proj_misses += base_proj_misses_this_layer;

            // v2.2 Task 1 (spec C1-C3): replaces the old ">3 switch-boundaries -> whole-
            // layer revert" and ">20 cumulative -> per-object escalation" machinery
            // (deleted entirely, C2). Support fills are one ExtrusionPath per LINE, so a
            // switch-boundary count (interface_switches + base_switches above) trips on
            // ordinary boundary-crossing runs and has nothing to do with the toolchange
            // this layer actually pays for - the TRUE per-layer cost is the number of
            // DISTINCT matched extruders. apply_bucket_caps measures that directly and
            // degrades PARTIALLY instead of reverting the whole layer: it first gates
            // any bucket whose total matched path length is under the tier floor (v2.3
            // Task 1, spec C1: 12mm normal / 3mm for a FREE extruder - see
            // free_extruders above - replacing the old flat 40mm; a matched sliver isn't
            // worth a toolchange/purge, but a FREE one is already paying for that
            // toolchange anyway), then, only if more than 2 extruders' worth of geometry
            // survived the gate, trims to the 2 with the longest total length -
            // preferring whichever extruder(s) this object's PREVIOUS support layer
            // committed (`prev_kept`) outright over length (C1 hysteresis: stability up
            // the column, the fix for the alternating-stripe artifact a whole-layer
            // revert caused; v2.3 Task 1 spec C2 additionally halves the GATE's own
            // floor for a prev_kept bucket - see apply_bucket_caps' own doc comment).
            // Every dropped/trimmed bucket's geometry is redirected to the nearest
            // SURVIVING matched bucket (v2.5a: prints a real nearby matched color
            // instead of the residual don't-care path); only when NO bucket survives
            // does it merge back into support_fills (fallback) via the same ownership-
            // transferring append(ExtrusionEntitiesPtr&&) the old revert used - each
            // path still carries its true source role, so nothing is lost.
            //
            // v2.3 Task 1 (spec C2): captured BEFORE the call, since apply_bucket_caps
            // erases gated/trimmed buckets from `partitioned` in place - this is what
            // distinguishes "buckets existed pre-gate but were all gated away" (the
            // one-layer retention case) from "nothing was ever a candidate this layer"
            // (the uniform-fallback fast path, which must NOT retain) in
            // chameleon_update_prev_kept below.
            const bool had_buckets_pre_gate = !partitioned.empty();

            // CHAMELEON_DEBUG: per-bucket before/after/outcome accounting - see
            // ChameleonBucketDebugEntry's own doc comment (BrimFilament.hpp) and
            // apply_bucket_caps' own trailing parameter for what populates this.
            // nullptr when off, matching every other debug hook in this loop.
            std::vector<ChameleonBucketDebugEntry> chameleon_debug_bucket_entries;

            BucketCapResult cap_result = apply_bucket_caps(partitioned, prev_kept,
                /*max_extruders=*/2, /*min_len_mm=*/12.0, support_layer->support_fills,
                free_extruders, /*min_len_free_mm=*/3.0, free_extruders_exempt,
                chameleon_debug_on ? &chameleon_debug_bucket_entries : nullptr);
            buckets_dropped_min_benefit      += cap_result.buckets_dropped_min_benefit;
            buckets_dropped_min_benefit_free += cap_result.buckets_dropped_min_benefit_free;
            buckets_trimmed_cap              += cap_result.buckets_trimmed_cap;
            buckets_redirected               += cap_result.buckets_redirected;
            buckets_exempt_kept              += cap_result.buckets_exempt_kept;

            if (chameleon_debug_on)
                chameleon_debug_log(chameleon_debug_line +
                    " proj_hits=" + std::to_string(debug_projection_hits) +
                    " proj_misses=" + std::to_string(debug_projection_misses) +
                    " proj_hits_base=" + std::to_string(base_proj_hits_this_layer) +
                    " proj_misses_base=" + std::to_string(base_proj_misses_this_layer) +
                    " buckets=" + chameleon_debug_format_buckets(chameleon_debug_bucket_entries));

            if (!partitioned.empty()) {
                support_layer->interface_by_extruder = std::move(partitioned);
                ++layers_partitioned;
            }
            // else: partitioned can be empty only via (a) an all-degenerate layer (every
            // entity Unchanged under the empty-chain guard) or (b) apply_bucket_caps gating/
            // trimming everything with no-survivor merge-back (support_fills already holds
            // that geometry back). interface_by_extruder stays empty either way, so GCode's
            // dominant-bucket pin has nothing to pin to: the don't-care roles of what is left
            // print in this layer's surface filament rather than the active extruder.
            else if (!support_layer->support_fills.entities.empty())
                support_layer->chameleon_residual_extruder = int(layer_surface_extruder_id);

            // Hysteresis (v2.3 Task 1, spec C2): this layer reached the engine calls, so
            // chameleon_update_prev_kept (BrimFilament.hpp/.cpp - see its own doc
            // comment for the three-way decision) now decides prev_kept/the retention
            // grace instead of the old unconditional overwrite. Only the guards ABOVE
            // this point (plate/visited/zero-sample, all of which `continue` before
            // ever reaching here) leave prev_kept/prev_kept_retained_last_layer
            // unchanged.
            PrevKeptState next_prev_kept_state = chameleon_update_prev_kept(
                { prev_kept, prev_kept_retained_last_layer }, cap_result.kept, had_buckets_pre_gate);
            prev_kept                     = std::move(next_prev_kept_state.prev_kept);
            prev_kept_retained_last_layer = next_prev_kept_state.retained_last_layer;
            // v2.3 Task 3 (spec C5): replace wholesale (not merge) - see
            // descended_last_layer's own declaration comment above for why this layer's
            // fresh accumulation is the whole next-layer state, not an addition to it.
            descended_last_layer = std::move(descended_this_layer);
            support_layer->chameleon_interface_visited = true;
        }

        // Plate layers (the support brim pads under every column of a tree support, the first
        // layer of a normal support). They are never partitioned (plate guard above), and they
        // used to be left to "don't care" - i.e. whatever filament the first layer's tool order
        // happened to start with, often neither the part's nor the support's. Rule: a plate
        // layer prints in the filament the support directly above it prints in - the one with
        // the most extrusion length on the next support layer that prints anything (its matched
        // buckets, plus its leftover support at the filament GCode pins that to). Without one,
        // the surface filament of the plate layer's own object layers. Without a raft this
        // replaces an explicitly configured support/interface filament too (the pads belong to
        // their columns, and matching already decides the columns' colour); with a raft only a
        // don't-care role follows, the raft's base stays in the configured support filament.
        // Re-running is idempotent: the layer above is visited once and keeps its result.
        for (SupportLayer *plate : plate_layers) {
            const SupportLayer *above = nullptr;
            for (const SupportLayer *sl : object->support_layers())
                if (sl != nullptr && sl->print_z > plate->print_z + EPSILON &&
                    (!sl->support_fills.entities.empty() || !sl->interface_by_extruder.empty())) {
                    above = sl;
                    break;
                }
            std::map<unsigned, double> length;
            if (above != nullptr) {
                for (const auto &[extruder, bucket] : above->interface_by_extruder)
                    length[extruder] += total_path_length_mm(bucket);
                if (!above->support_fills.entities.empty()) {
                    int residual = above->chameleon_residual_extruder;
                    if (residual < 0)
                        residual = chameleon_dominant_matched_extruder(above->interface_by_extruder);
                    if (residual >= 0)
                        length[unsigned(residual)] += total_path_length_mm(above->support_fills);
                }
            }
            unsigned winner = 0;
            bool     found  = false;
            double   best   = 0.;
            for (const auto &[extruder, len] : length)
                if (!found || len > best) { // ordered map: a tie keeps the lower id
                    found  = true;
                    best   = len;
                    winner = extruder;
                }
            if (!found)
                winner = layer_surface_extruder(select_layers_overlapping_span(layer_print_zs, plate->print_z - plate->height, plate->print_z,
                                                                               object->layers().front()->print_z - object->layers().front()->height),
                                                {});
            plate->chameleon_residual_extruder  = int(winner);
            plate->chameleon_residual_all_roles = object->config().raft_layers.value == 0;
        }

        // v2.4 (spec A): "mode=nearest_wall" is now a constant, not a per-object
        // ternary - nearest_wall is the only resolution mode that ever reaches this line
        // (objects with support_filament_matching off `continue`d out above the loop
        // entirely, and nearest_surface no longer exists) - kept as a literal string, not removed,
        // so the line stays a stable grep target and a triage/verify script can still
        // assert "the pass ran for this object" (mode= present) vs. "the object was
        // gated out as manual" (no line at all for that ordinal) - see the legacy-alias
        // verify check (spike/verify_chameleon.sh) for exactly that assertion.
        BOOST_LOG_TRIVIAL(info) << "Chameleon support match: object ordinal " << obj_idx
            << " mode=nearest_wall"
            // v2.3 Task 4 (spec C9): free_set_size is the once-per-PASS
            // layer_filament_table's entry count (z-coincidence rows, not extruders per
            // row) - same value on every object's line since the table is built once
            // before this loop (see chameleon_collect_layer_filaments above). 0 means
            // either mixed-filament gradient was active (table skipped entirely, C1) or
            // no object in the plate has any wall/solid/sparse layer - i.e. the free tier
            // (3mm floor) can never fire this pass; a triage reader can rule the free
            // tier in/out of a gate-count anomaly from this one field before digging into
            // per-layer free_extruders.
            << " free_set_size=" << layer_filament_table.size()
            // v2.4 Task C (spec C): resolved fallback ids, so a triage reader can tell
            // "this object's whole layer degraded to fallback" apart from "the gate
            // dropped a genuinely-matched bucket back to fallback" (same symptom, gcode-
            // side - a run of the fallback color - but different root cause) without
            // re-deriving these from object config by hand.
            << " fallback=" << fallback_extruder
            << " base_fallback=" << base_fallback_extruder
            << " layers_partitioned=" << layers_partitioned
            << " layers_zero_sample=" << layers_zero_sample
            << " buckets_dropped_min_benefit=" << buckets_dropped_min_benefit
            << " buckets_dropped_min_benefit_free=" << buckets_dropped_min_benefit_free
            << " buckets_trimmed_cap=" << buckets_trimmed_cap
            // v2.5a Task 2 (spec item 2): SUBSET of the two counters just above -
            // see buckets_redirected's own declaration comment for the exact
            // relationship.
            << " buckets_redirected=" << buckets_redirected
            << " buckets_exempt_kept=" << buckets_exempt_kept
            << " interface_runs_matched=" << interface_runs_matched
            << " base_runs_matched=" << base_runs_matched
            << " ironing_runs_matched=" << ironing_runs_matched
            // v2.5e/v2.5f: object-wide sums of the per-layer proj_hits/proj_misses debug
            // counts (see the CHAMELEON_DEBUG header comment's proj_hits=/proj_misses=
            // field for the full meaning, incl. the v2.5f note on why this pair is now
            // an ALL-ROLES total) - unlike the other CHAMELEON_DEBUG-only per-layer
            // fields, these are cheap to accumulate unconditionally (interface_proj_
            // hits'/interface_proj_misses'/base_proj_hits'/base_proj_misses' own
            // declaration comments above this loop), so they're meaningful on this
            // BOOST_LOG_TRIVIAL line even when CHAMELEON_DEBUG itself is unset.
            << " interface_proj_hits=" << interface_proj_hits
            << " interface_proj_misses=" << interface_proj_misses
            // v2.5f: base's own isolated share of the totals above (v2.5e review M3
            // fix, base half) - the discriminator this task adds for "is base's new
            // projection branch actually engaging".
            << " base_proj_hits=" << base_proj_hits
            << " base_proj_misses=" << base_proj_misses;

        // CHAMELEON_DEBUG: field-for-field mirror of the BOOST_LOG_TRIVIAL summary line
        // just above, also written to the dedicated debug file - BOOST_LOG_TRIVIAL(info)
        // may be filtered by whatever log severity level is currently configured, and
        // (Part 1's own hard-won lesson, see the CHAMELEON_DEBUG header comment above
        // chameleon_collect_wall_samples) a GUI slicing background thread's
        // BOOST_LOG_TRIVIAL output does not reliably reach the GUI's own log sink at
        // all - so a triage session relying solely on the line above can come up empty
        // even though the pass genuinely ran. "SUMMARY " prefix distinguishes this line
        // from the per-layer lines above it in the same file.
        if (chameleon_debug_on) {
            chameleon_debug_log(
                "SUMMARY obj=" + std::to_string(obj_idx) +
                " mode=nearest_wall" +
                " free_set_size=" + std::to_string(layer_filament_table.size()) +
                " fallback=" + std::to_string(fallback_extruder) +
                " base_fallback=" + std::to_string(base_fallback_extruder) +
                " layers_partitioned=" + std::to_string(layers_partitioned) +
                " layers_zero_sample=" + std::to_string(layers_zero_sample) +
                " buckets_dropped_min_benefit=" + std::to_string(buckets_dropped_min_benefit) +
                " buckets_dropped_min_benefit_free=" + std::to_string(buckets_dropped_min_benefit_free) +
                " buckets_trimmed_cap=" + std::to_string(buckets_trimmed_cap) +
                " buckets_redirected=" + std::to_string(buckets_redirected) +
                " buckets_exempt_kept=" + std::to_string(buckets_exempt_kept) +
                " interface_runs_matched=" + std::to_string(interface_runs_matched) +
                " base_runs_matched=" + std::to_string(base_runs_matched) +
                " ironing_runs_matched=" + std::to_string(ironing_runs_matched) +
                " interface_proj_hits=" + std::to_string(interface_proj_hits) +
                " interface_proj_misses=" + std::to_string(interface_proj_misses) +
                " base_proj_hits=" + std::to_string(base_proj_hits) +
                " base_proj_misses=" + std::to_string(base_proj_misses));
        }
    }
}

// Slicing process, running at a background thread.
void Print::process_perimeters_only(PrintObject &object)
{
    assert(object.print() == this);
    name_tbb_thread_pool_threads_set_locale();
    object.clear_shared_object();
    object.make_perimeters();
}

void Print::process(long long *time_cost_with_cache, bool use_cache)
{
    long long start_time = 0, end_time = 0;
    if (time_cost_with_cache)
        *time_cost_with_cache = 0;

    name_tbb_thread_pool_threads_set_locale();

    //compute the PrintObject with the same geometries
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": this=%1%, enter, use_cache=%2%, object size=%3%")%this%use_cache%m_objects.size();
    if (m_objects.empty())
        return;

    for (PrintObject *obj : m_objects)
        obj->clear_shared_object();

    //add the print_object share check logic
    auto is_print_object_the_same = [this](const PrintObject* object1, const PrintObject* object2) -> bool{
        if (object1->trafo().matrix() != object2->trafo().matrix())
            return false;
        const ModelObject* model_obj1 = object1->model_object();
        const ModelObject* model_obj2 = object2->model_object();
        if (model_obj1->volumes.size() != model_obj2->volumes.size())
            return false;
        bool has_extruder1 = model_obj1->config.has("extruder");
        bool has_extruder2 = model_obj2->config.has("extruder");
        if ((has_extruder1 != has_extruder2)
            || (has_extruder1 && model_obj1->config.extruder() != model_obj2->config.extruder()))
            return false;
        for (int index = 0; index < model_obj1->volumes.size(); index++) {
            const ModelVolume &model_volume1 = *model_obj1->volumes[index];
            const ModelVolume &model_volume2 = *model_obj2->volumes[index];
            if (model_volume1.type() != model_volume2.type())
                return false;
            if (model_volume1.mesh_ptr() != model_volume2.mesh_ptr())
                return false;
            if (!(model_volume1.get_transformation() == model_volume2.get_transformation()))
                return false;
            has_extruder1 = model_volume1.config.has("extruder");
            has_extruder2 = model_volume2.config.has("extruder");
            if ((has_extruder1 != has_extruder2)
                || (has_extruder1 && model_volume1.config.extruder() != model_volume2.config.extruder()))
                return false;
            if (!model_volume1.supported_facets.equals(model_volume2.supported_facets))
                return false;
            if (!model_volume1.seam_facets.equals(model_volume2.seam_facets))
                return false;
            if (!model_volume1.mmu_segmentation_facets.equals(model_volume2.mmu_segmentation_facets))
                return false;
            if (!model_volume1.fuzzy_skin_facets.equals(model_volume2.fuzzy_skin_facets))
                return false;
            if (model_volume1.config.get() != model_volume2.config.get())
                return false;
        }
        //if (!object1->config().equals(object2->config()))
        //    return false;
        if (model_obj1->config.get() != model_obj2->config.get())
            return false;
        return true;
    };
    int object_count = m_objects.size();
    std::set<PrintObject*> need_slicing_objects;
    std::set<PrintObject*> re_slicing_objects;
    if (!use_cache) {
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            for (PrintObject *slicing_obj : need_slicing_objects)
            {
                if (is_print_object_the_same(obj, slicing_obj)) {
                    obj->set_shared_object(slicing_obj);
                    break;
                }
            }
            if (!obj->get_shared_object())
                need_slicing_objects.insert(obj);
        }
    }
    else {
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            if (obj->layer_count() > 0)
                need_slicing_objects.insert(obj);
        }
        for (int index = 0; index < object_count; index++)
        {
            PrintObject *obj =  m_objects[index];
            bool found_shared = false;
            if (need_slicing_objects.find(obj) == need_slicing_objects.end()) {
                for (PrintObject *slicing_obj : need_slicing_objects)
                {
                    if (is_print_object_the_same(obj, slicing_obj)) {
                        obj->set_shared_object(slicing_obj);
                        found_shared = true;
                        break;
                    }
                }
                if (!found_shared) {
                    BOOST_LOG_TRIVIAL(warning) << boost::format("Also can not find the shared object, identify_id %1%, maybe shared object is skipped")%obj->model_object()->instances[0]->loaded_id;
                    //throw Slic3r::SlicingError("Cannot find the cached data.");
                    //don't report errot, set use_cache to false, and reslice these objects
                    need_slicing_objects.insert(obj);
                    re_slicing_objects.insert(obj);
                    //use_cache = false;
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": total object counts %1% in current print, need to slice %2%")%m_objects.size()%need_slicing_objects.size();
    BOOST_LOG_TRIVIAL(info) << "Starting the slicing process." << log_memory_info();
    if (!use_cache) {
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->make_perimeters();
            }
            else {
                if (obj->set_started(posSlice))
                    obj->set_done(posSlice);
                if (obj->set_started(posPerimeters))
                    obj->set_done(posPerimeters);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->estimate_curled_extrusions();
            }
            else {
                if (obj->set_started(posEstimateCurledExtrusions))
                    obj->set_done(posEstimateCurledExtrusions);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->infill();
            }
            else {
                if (obj->set_started(posPrepareInfill))
                    obj->set_done(posPrepareInfill);
                if (obj->set_started(posInfill))
                    obj->set_done(posInfill);
            }
        }
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->ironing();
            }
            else {
                if (obj->set_started(posIroning))
                    obj->set_done(posIroning);
            }
        }

        // ZAA (Z contouring): after infill and ironing, before path simplification.
        for (PrintObject *obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->contour_z();
            }
            else {
                if (obj->set_started(posContouring))
                    obj->set_done(posContouring);
            }
        }

        tbb::parallel_for(tbb::blocked_range<int>(0, int(m_objects.size())),
            [this, need_slicing_objects](const tbb::blocked_range<int>& range) {
                for (int i = range.begin(); i < range.end(); i++) {
                    PrintObject* obj = m_objects[i];
                    if (need_slicing_objects.count(obj) != 0) {
                        obj->generate_support_material();
                    }
                    else {
                        if (obj->set_started(posSupportMaterial))
                            obj->set_done(posSupportMaterial);
                    }
                }
            }
        );

        for (PrintObject* obj : m_objects) {
            if (need_slicing_objects.count(obj) != 0) {
                obj->detect_overhangs_for_lift();
            }
            else {
                if (obj->set_started(posDetectOverhangsForLift))
                    obj->set_done(posDetectOverhangsForLift);
            }
        }
    }
    else {
        for (PrintObject *obj : m_objects) {
            if (re_slicing_objects.count(obj) == 0) {
                if (obj->set_started(posSlice))
                    obj->set_done(posSlice);
                if (obj->set_started(posPerimeters))
                    obj->set_done(posPerimeters);
                if (obj->set_started(posPrepareInfill))
                    obj->set_done(posPrepareInfill);
                if (obj->set_started(posInfill))
                    obj->set_done(posInfill);
                if (obj->set_started(posIroning))
                    obj->set_done(posIroning);
                if (obj->set_started(posContouring))
                    obj->set_done(posContouring);
                if (obj->set_started(posSupportMaterial))
                    obj->set_done(posSupportMaterial);
                if (obj->set_started(posDetectOverhangsForLift))
                    obj->set_done(posDetectOverhangsForLift);
            }
            else {
                obj->make_perimeters();
                obj->infill();
                obj->ironing();
                obj->contour_z();
                obj->generate_support_material();
                obj->detect_overhangs_for_lift();
                obj->estimate_curled_extrusions();
            }
        }
    }

    for (PrintObject *obj : m_objects)
    {
        if (need_slicing_objects.count(obj) == 0) {
            obj->copy_layers_from_shared_object();
            obj->copy_layers_overhang_from_shared_object();
        }
    }

    // Chameleon P2: assign support-interface extruders by nearest wall BEFORE ToolOrdering
    // is built below - every object's support material (generate_support_material, above,
    // fresh or copied from a shared object) has finished, and the per-support-layer block
    // in ToolOrdering.cpp reads SupportLayer::interface_by_extruder at ctor time, so it
    // must already be populated by the time psWipeTower constructs the tool ordering.
    chameleon_assign_support_interfaces(*this);

    if (this->set_started(psWipeTower)) {
        m_wipe_tower_data.clear();
        m_tool_ordering.clear();
        if (this->has_wipe_tower()) {
            this->_make_wipe_tower();
        } else if (this->config().print_sequence != PrintSequence::ByObject) {
        	// Initialize the tool ordering, so it could be used by the G-code preview slider for planning tool changes and filament switches.
        	m_tool_ordering = ToolOrdering(*this, -1, false);
            if (m_tool_ordering.empty() || m_tool_ordering.last_extruder() == unsigned(-1))
                throw Slic3r::SlicingError("The print is empty. The model is not printable with current print settings.");
        }
        this->set_done(psWipeTower);
    }
    if (this->has_wipe_tower()) {
        // Validated on every process() run rather than only when the wipe tower step is (re)generated.
        // Moving the tower changes only wipe_tower_x/y, which invalidates psSkirtBrim but not psWipeTower,
        // so a validate call living inside _make_wipe_tower would be skipped and keep using the stale
        // position, missing a fresh collision. The tower geometry (tool_changes) is stored in the local
        // frame and is position independent, so re-checking here with the current position is correct.
        this->validate_compacted_wipe_tower_clearance();
    }
    if (this->set_started(psSkirtBrim)) {
        this->set_status(70, L("Generating skirt & brim"));

        if (time_cost_with_cache)
            start_time = (long long)Slic3r::Utils::get_current_time_utc();

        m_skirt.clear();
        m_skirt_convex_hull.clear();
        m_first_layer_convex_hull.points.clear();
        const bool draft_shield = config().draft_shield != dsDisabled;

        if (this->has_skirt() && draft_shield) {
            // In case that draft shield is active, generate skirt first so brim
            // can be trimmed to make room for it.
            _make_skirt();
        }

        //BBS: get the objects' indices when GCodes are generated
        ToolOrdering tool_ordering;
        unsigned int initial_extruder_id = (unsigned int)-1;
        bool         has_wipe_tower = false;
        std::vector<const PrintInstance*> 					print_object_instances_ordering;
        std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
        std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> layers_to_print = GCode::collect_layers_to_print(*this);
        std::vector<unsigned int> printExtruders;
        if (this->config().print_sequence == PrintSequence::ByObject) {
            // Order object instances for sequential print.
            print_object_instances_ordering = sort_object_instances_by_model_order(*this);
            //        print_object_instances_ordering = sort_object_instances_by_max_z(print);
            print_object_instance_sequential_active = print_object_instances_ordering.begin();
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
                tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
                if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<unsigned int>(-1)) {
                    append(printExtruders, tool_ordering.tools_for_layer(layers_to_print.front().first).extruders);
                }
            }
        }
        else {
            tool_ordering = this->tool_ordering();
            tool_ordering.assign_custom_gcodes(*this);
            has_wipe_tower = this->has_wipe_tower() && tool_ordering.has_wipe_tower();
            initial_extruder_id = tool_ordering.first_extruder();
            print_object_instances_ordering = chain_print_object_instances(*this);
            append(printExtruders, tool_ordering.tools_for_layer(layers_to_print.front().first).extruders);
        }

        auto objectExtruderMap = getObjectExtruderMap(*this);
        std::vector<std::pair<ObjectID, unsigned int>> objPrintVec;
        for (const PrintInstance* instance : print_object_instances_ordering) {
            const ObjectID& print_object_ID = instance->print_object->id();
            bool existObject = false;
            for (auto& objIDPair : objPrintVec) {
                if (print_object_ID == objIDPair.first) existObject = true;
            }
            if (!existObject && objectExtruderMap.find(print_object_ID) != objectExtruderMap.end())
                objPrintVec.push_back(std::make_pair(print_object_ID, objectExtruderMap.at(print_object_ID)));
        }
        // BBS: m_brimMap and m_supportBrimMap are used instead of m_brim to generate brim of objs and supports seperately
        m_brimMap.clear();
        m_supportBrimMap.clear();
        m_brimMapByExtruder.clear();
        m_first_layer_convex_hull.points.clear();
        if (this->has_brim()) {
            Polygons islands_area;
            make_brim(*this, this->make_try_cancel(), islands_area, m_brimMap,
                m_supportBrimMap, objPrintVec, printExtruders);

            // Chameleon brim: reassign brim extrusions to whichever extruder
            // printed the nearest first-layer wall. Off by default (bfsObject),
            // and skipped outright on single-extruder prints so the default
            // path stays byte-identical to pre-chameleon behavior.
            // Also restricted to by-layer printing: in PrintSequence::ByObject mode,
            // GCode::process_layers() builds a separate per-object ToolOrdering for
            // each object in turn (which never runs the whole-print union hook in
            // ToolOrdering.cpp), so GCode::process_layer's foreign-brim emission block
            // would see a populated m_brimMapByExtruder without the matching extruder
            // having been registered for that object's layer - risking a double-emit
            // (once via the normal per-object m_brimMap path, once foreign) or a
            // missed/mis-sequenced foreign run. Keep m_brimMapByExtruder empty in
            // sequential (by-object) mode so both the union hook and the emission
            // block stay no-ops there and by-object prints keep the plain per-object
            // brim behavior (each object's brim prints in its own filament).
            if (m_config.brim_filament_source == bfsNearestWall && this->extruders().size() > 1
                && m_config.print_sequence != PrintSequence::ByObject) {
                // Run-stable object key: objIDPair.first.id (the raw ObjectID) is NOT stable
                // across process runs - confirmed empirically (Task 5 verification): two
                // back-to-back runs of the identical command produced wildly different id
                // values (e.g. 0 vs 2317700280585562421), because ObjectID is an
                // allocation-order/address-derived counter, not a content hash. WallSampleIndex's
                // k-NN tie-break and BrimVoteParams::object_area are both keyed by this value
                // specifically for determinism (see WallSampleIndex.hpp's own comment), so an
                // unstable key silently flips tie votes between otherwise-identical runs,
                // shifting brim-run partition boundaries. Build a small dense ordinal
                // (0, 1, 2, ...) from objPrintVec's own iteration order instead:
                // print_object_instances_ordering (which objPrintVec is derived from) is itself
                // deterministic - model order or chained/arranged layout order, never an
                // address/allocation id - so this ordinal is stable run-to-run even though the
                // underlying ObjectID is not.
                std::map<ObjectID, size_t> object_ordinal;
                for (size_t i = 0; i < objPrintVec.size(); ++i)
                    object_ordinal[objPrintVec[i].first] = i;

                WallSampleIndex wall_idx;
                std::map<size_t, double> object_area;
                // Chameleon brim fallback (spec-mandated): an object that contributes zero
                // layer-0 wall samples (e.g. its first layer is entirely support, no walls of
                // its own) has no basis for a nearest-wall vote. Rather than let it fall through
                // to whatever an empty-candidate vote produces, keep its brim on the object's own
                // filament for its whole run - tracked here via a wall_idx size delta per object,
                // acted on below by skipping the partition pass for these objects entirely.
                std::map<ObjectID, bool> zero_sample_objects;

                for (const auto& objIDPair : objPrintVec) {
                    const PrintObject* object = this->get_object(objIDPair.first);
                    if (object == nullptr || object->layers().empty())
                        continue;
                    const size_t object_key = object_ordinal.at(objIDPair.first);
                    const Layer* layer0     = object->layers().front();

                    double area_sum = 0.0;
                    for (const ExPolygon& ex : layer0->lslices)
                        area_sum += ex.area() * SCALING_FACTOR * SCALING_FACTOR;
                    object_area[object_key] = area_sum;

                    const size_t samples_before = wall_idx.size();
                    for (const PrintInstance& instance : object->instances())
                        for (const LayerRegion* lr : layer0->regions()) {
                            chameleon_collect_wall_samples(&lr->perimeters, lr->region(),
                                instance.shift, object_key, wall_idx);
                        }
                    if (wall_idx.size() == samples_before) {
                        zero_sample_objects[objIDPair.first] = true;
                        BOOST_LOG_TRIVIAL(warning) << "Chameleon brim: object id " << objIDPair.first.id
                            << " contributed zero layer-0 wall samples; keeping its brim on the "
                               "object's own filament instead of nearest-wall assignment.";
                    }
                }

                BrimVoteParams vote_params;
                vote_params.object_area = object_area;

                for (auto& brimEntry : m_brimMap) {
                    if (zero_sample_objects.count(brimEntry.first))
                        continue; // fallback: leave this object's brim on its own filament, untouched
                    const auto obj_it = std::find_if(objPrintVec.begin(), objPrintVec.end(),
                        [&brimEntry](const std::pair<ObjectID, unsigned int>& pr) { return pr.first == brimEntry.first; });
                    if (obj_it == objPrintVec.end())
                        continue;
                    const unsigned own_extruder_0based = obj_it->second > 0 ? obj_it->second - 1 : 0;
                    vote_params.fallback_extruder = own_extruder_0based;

                    ExtrusionEntityCollection kept;
                    std::map<unsigned, ExtrusionEntityCollection> foreign;
                    partition_brim_by_wall(brimEntry.second, own_extruder_0based, wall_idx, vote_params, kept, foreign);
                    // I2: release the original entities before the move-assign below replaces
                    // them - ExtrusionEntityCollection::operator=(&&) just overwrites the
                    // raw-pointer `entities` vector without deleting what it pointed to (unlike
                    // the copy-assign operator, which does call clear() first), so skipping this
                    // leaks every original brim entity on every nearest_wall slice. `kept`/`foreign`
                    // above already hold independent entities (new paths, or the original pointers
                    // themselves reassigned into `kept` - never both), so this clear() cannot
                    // double-free anything partition_brim_by_wall handed back to us.
                    brimEntry.second.clear();
                    // Root-cause fix (GUI validation): the per-object brim print site in
                    // GCode::process_layer runs under whichever extruder prints that object
                    // FIRST on layer 0 - not necessarily own_extruder - so leaving the kept
                    // (own-voted) runs in m_brimMap printed them in an arbitrary filament.
                    // Route the own partition through m_brimMapByExtruder as well: every run
                    // then prints under exactly the extruder it was voted for, via the
                    // toolchange-ordered emission block. m_brimMap stays EMPTY for partitioned
                    // objects (the legacy per-object site prints nothing for them).
                    if (!kept.entities.empty())
                        m_brimMapByExtruder[brimEntry.first][own_extruder_0based] = std::move(kept);
                    for (auto& foreignEntry : foreign) {
                        if (foreignEntry.second.entities.empty())
                            continue;
                        m_brimMapByExtruder[brimEntry.first][foreignEntry.first] = std::move(foreignEntry.second);
                    }
                }
            }

            // Chameleon brim: union foreign-extruder brim partitions into the first layer's
            // LayerTools.extruders of the ToolOrdering object GCode actually consumes
            // (m_tool_ordering; m_wipe_tower_data.tool_ordering is a reference to this same
            // object - see WipeTowerData's private ctor above). This MUST run here, after the
            // partition pass above has populated m_brimMapByExtruder, rather than inside
            // ToolOrdering's own constructor: psWipeTower - which constructs/assigns
            // m_tool_ordering, both via _make_wipe_tower() and the plain-ToolOrdering(*this, ...)
            // branch - runs BEFORE psSkirtBrim, so a ctor-time hook always observed an empty map
            // on a fresh slice.
            if (!m_brimMapByExtruder.empty()) {
                if (m_tool_ordering.empty()) {
                    // Belt-and-braces: the ToolOrdering GCode consumes isn't available on this
                    // path (should not happen given the ByObject gate above, but brim geometry
                    // must never be silently dropped) - fold every foreign partition back into
                    // its object's own-extruder brim collection instead of leaving it stranded
                    // and unregistered (which GCode::process_layer would then never visit).
                    for (auto& brimEntry : m_brimMapByExtruder) {
                        ExtrusionEntityCollection& own = m_brimMap[brimEntry.first];
                        for (auto& foreignEntry : brimEntry.second)
                            own.append(std::move(foreignEntry.second.entities));
                    }
                    BOOST_LOG_TRIVIAL(warning) << "Chameleon brim: ToolOrdering unavailable after brim "
                        "partitioning; merged all foreign-extruder brim runs back into their "
                        "object's own extruder instead of dropping them.";
                    m_brimMapByExtruder.clear();
                } else {
                    // Do NOT sort/dedup this vector with sort_remove_duplicates(): by this point
                    // its order is semantically load-bearing, not incidental - reorder_extruders()
                    // has already picked which extruder leads (the requested first_extruder, or a
                    // soluble-first swap ahead of the prime tower) and, for layer 0 specifically,
                    // applied apply_first_layer_order()'s user-configured sequence. A numeric sort
                    // here would silently discard all of that ordering. Instead, append any brim
                    // extruder not already present to the end (dedup via linear find,
                    // order-preserving) so it simply prints last on layer 0.
                    LayerTools& first_layer_tools = m_tool_ordering.tools_for_layer(m_tool_ordering.front().print_z);
                    for (const auto& obj_entry : m_brimMapByExtruder)
                        for (const auto& per_extruder : obj_entry.second) {
                            unsigned int extruder_id = per_extruder.first;
                            if (std::find(first_layer_tools.extruders.begin(), first_layer_tools.extruders.end(), extruder_id)
                                == first_layer_tools.extruders.end())
                                first_layer_tools.extruders.push_back(extruder_id);
                        }
                }
            }

            for (Polygon& poly_ex : islands_area)
                poly_ex.douglas_peucker(SCALED_RESOLUTION);
            for (Polygon &poly : union_(this->first_layer_islands(), islands_area))
                append(m_first_layer_convex_hull.points, std::move(poly.points));
        }


        if (has_skirt() && ! draft_shield) {
            // In case that draft shield is NOT active, generate skirt now.
            // It will be placed around the brim, so brim has to be ready.
            assert(m_skirt.empty());
            _make_skirt();
        }

        this->finalize_first_layer_convex_hull();
        this->set_done(psSkirtBrim);

        if (time_cost_with_cache) {
            end_time = (long long)Slic3r::Utils::get_current_time_utc();
            *time_cost_with_cache = *time_cost_with_cache + end_time - start_time;
        }
    }
    //BBS
    for (PrintObject *obj : m_objects) {
        if (((!use_cache)&&(need_slicing_objects.count(obj) != 0))
            || (use_cache &&(re_slicing_objects.count(obj) != 0))){
            obj->simplify_extrusion_path();
        }
        else {
            if (obj->set_started(posSimplifyPath))
                obj->set_done(posSimplifyPath);
            if (obj->set_started(posSimplifyInfill))
                obj->set_done(posSimplifyInfill);
            if (obj->set_started(posSimplifySupportPath))
                obj->set_done(posSimplifySupportPath);
        }
    }

    // BBS
    bool has_adaptive_layer_height = false;
    for (PrintObject* obj : m_objects) {
        if (obj->model_object()->layer_height_profile.empty() == false) {
            has_adaptive_layer_height = true;
            break;
        }
    }
    // TODO adaptive layer height won't work with conflict checker because m_fake_wipe_tower's path is generated using fixed layer height
    if(!m_no_check && !has_adaptive_layer_height)
    {
        using Clock                 = std::chrono::high_resolution_clock;
        auto            startTime   = Clock::now();
        std::optional<const FakeWipeTower *> wipe_tower_opt = {};
        if (this->has_wipe_tower()) {
            m_fake_wipe_tower.set_pos({m_config.wipe_tower_x.get_at(m_plate_index), m_config.wipe_tower_y.get_at(m_plate_index)});
            wipe_tower_opt = std::make_optional<const FakeWipeTower *>(&m_fake_wipe_tower);
        }
        auto            conflictRes = ConflictChecker::find_inter_of_lines_in_diff_objs(m_objects, wipe_tower_opt);
        auto            endTime     = Clock::now();
        volatile double seconds     = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count() / (double) 1000;
        BOOST_LOG_TRIVIAL(info) << "gcode path conflicts check takes " << seconds << " secs.";

        m_conflict_result = conflictRes;
        if (conflictRes.has_value()) {
            BOOST_LOG_TRIVIAL(error) << boost::format("gcode path conflicts found between %1% and %2%")%conflictRes.value()._objName1 %conflictRes.value()._objName2;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Slicing process finished." << log_memory_info();
}

// G-code export process, running at a background thread.
// The export_gcode may die for various reasons (fails to process filename_format,
// write error into the G-code, cannot execute post-processing scripts).
// It is up to the caller to show an error message.
std::string Print::export_gcode(const std::string& path_template, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    // output everything to a G-code file
    // The following call may die if the filename_format template substitution fails.
    std::string path = this->output_filepath(path_template);
    std::string message;
    if (!path.empty() && result == nullptr) {
        // Only show the path if preview_data is not set -> running from command line.
        message = L("Exporting G-code");
        message += " to ";
        message += path;
    } else
        message = L("Generating G-code");
    this->set_status(80, message);

    // The following line may die for multiple reasons.
    GCode gcode;
    //BBS: compute plate offset for gcode-generator
    const Vec3d origin = this->get_plate_origin();
    gcode.set_gcode_offset(origin(0), origin(1));
    gcode.do_export(this, path.c_str(), result, thumbnail_cb);

    //BBS
    // `result` is optional and documented as such by the `result == nullptr` branch
    // above ("running from command line"), so it must be null-checked here too.
    // Dereferencing it unconditionally segfaulted every caller that passes nullptr -
    // which is every Slic3r::Test::gcode() call in tests/fff_print, taking the whole
    // fff_print_tests binary down mid-run.
    if (result != nullptr)
        result->conflict_result = m_conflict_result;
    return path.c_str();
}

void Print::_make_skirt()
{
    // First off we need to decide how tall the skirt must be.
    // The skirt_height option from config is expressed in layers, but our
    // object might have different layer heights, so we need to find the print_z
    // of the highest layer involved.
    // Note that unless has_infinite_skirt() == true
    // the actual skirt might not reach this $skirt_height_z value since the print
    // order of objects on each layer is not guaranteed and will not generally
    // include the thickest object first. It is just guaranteed that a skirt is
    // prepended to the first 'n' layers (with 'n' = skirt_height).
    // $skirt_height_z in this case is the highest possible skirt height for safety.
    coordf_t skirt_height_z = 0.;
    for (const PrintObject *object : m_objects) {
        size_t skirt_layers = this->has_infinite_skirt() ?
            object->layer_count() :
            std::min(size_t(m_config.skirt_height.value), object->layer_count());
        // `skirt_layers` is 0 when the object sliced to no layers at all, or when
        // skirt_height is 0. `skirt_layers - 1` is size_t arithmetic, so it wrapped to
        // SIZE_MAX and indexed m_layers far out of bounds - a segfault, not a bad value.
        if (skirt_layers == 0)
            continue;
        skirt_height_z = std::max(skirt_height_z, object->m_layers[skirt_layers-1]->print_z);
    }

    // Collect points from all layers contained in skirt height.
    Points points;

    // BBS
    std::map<PrintObject*, Polygon> object_convex_hulls;
    for (PrintObject *object : m_objects) {
        Points object_points;
        // Get object layers up to skirt_height_z.
        for (const Layer *layer : object->m_layers) {
            if (layer->print_z > skirt_height_z)
                break;
            for (const ExPolygon &expoly : layer->lslices)
                // Collect the outer contour points only, ignore holes for the calculation of the convex hull.
                append(object_points, expoly.contour.points);
        }
        // Get support layers up to skirt_height_z.
        for (const SupportLayer *layer : object->support_layers()) {
            if (layer->print_z > skirt_height_z)
                break;
            layer->support_fills.collect_points(object_points);
        }

        object_convex_hulls.insert({ object, Slic3r::Geometry::convex_hull(object_points) });

        // Repeat points for each object copy.
        for (const PrintInstance &instance : object->instances()) {
            Points copy_points = object_points;
            for (Point &pt : copy_points)
                pt += instance.shift;
            append(points, copy_points);
        }
    }

    // Include the wipe tower.
    append(points, this->first_layer_wipe_tower_corners());

    // Unless draft shield is enabled, include all brims as well.
    if (config().draft_shield == dsDisabled)
        append(points, m_first_layer_convex_hull.points);

    if (points.size() < 3)
        // At least three points required for a convex hull.
        return;

    this->throw_if_canceled();
    Polygon convex_hull = Slic3r::Geometry::convex_hull(points);

    // Skirt may be printed on several layers, having distinct layer heights,
    // but loops must be aligned so can't vary width/spacing
    // TODO: use each extruder's own flow
    double initial_layer_print_height = this->skirt_first_layer_height();
    Flow   flow = this->skirt_flow();
    float  spacing = flow.spacing();
    double mm3_per_mm = flow.mm3_per_mm();

    std::vector<size_t> extruders;
    std::vector<double> extruders_e_per_mm;
    {
        auto set_extruders = this->extruders();
        extruders.reserve(set_extruders.size());
        extruders_e_per_mm.reserve(set_extruders.size());
        for (auto &extruder_id : set_extruders) {
            extruders.push_back(extruder_id);
            extruders_e_per_mm.push_back(Extruder((unsigned int)extruder_id, &m_config, m_config.single_extruder_multi_material).e_per_mm(mm3_per_mm));
        }
    }

    // Initial offset of the brim inner edge from the object (possible with a support & raft).
    // The skirt will touch the brim if the brim is extruded.
    auto   distance = float(scale_(m_config.skirt_distance.value - spacing/2.));
    // Draw outlines from outside to inside.
    // Loop while we have less skirts than required or any extruder hasn't reached the min length if any.
    std::vector<coordf_t> extruded_length(extruders.size(), 0.);
    if (m_config.skirt_type == stCombined) {
        for (size_t i = m_config.skirt_loops, extruder_idx = 0; i > 0; -- i) {
            this->throw_if_canceled();
            // Offset the skirt outside.
            distance += float(scale_(spacing));
            // Generate the skirt centerline.
            Polygon loop;
            {
                // BBS. skirt_distance is defined as the gap between skirt and outer most brim, so no need to add max_brim_width
                Polygons loops = offset(convex_hull, distance, ClipperLib::jtRound, float(scale_(0.1)));
                Geometry::simplify_polygons(loops, scale_(0.05), &loops);
			    if (loops.empty())
				    break;
			    loop = loops.front();
            }
            // Extrude the skirt loop.
            ExtrusionLoop eloop(elrSkirt);
            eloop.paths.emplace_back(ExtrusionPath(
                ExtrusionPath(
                    erSkirt,
                    (float)mm3_per_mm,         // this will be overridden at G-code export time
                    flow.width(),
				    (float)initial_layer_print_height  // this will be overridden at G-code export time
                )));
            eloop.paths.back().polyline = loop.split_at_first_point();
            m_skirt.append(eloop);
            if (m_config.min_skirt_length.value > 0) {
                // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
                extruded_length[extruder_idx] += unscale<double>(loop.length()) * extruders_e_per_mm[extruder_idx];
                if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                    // Not extruded enough yet with the current extruder. Add another loop.
                    if (i == 1)
                        ++ i;
                } else {
                    assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                    // Enough extruded with the current extruder. Extrude with the next one,
                    // until the prescribed number of skirt loops is extruded.
                    if (extruder_idx + 1 < extruders.size())
                        ++ extruder_idx;
                }
            } else {
                // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
            }
        }
    } else {
        m_skirt.clear();
    }
    // Brims were generated inside out, reverse to print the outmost contour first.
    m_skirt.reverse();

    // Remember the outer edge of the last skirt line extruded as m_skirt_convex_hull.
    for (Polygon &poly : offset(convex_hull, distance + 0.5f * float(scale_(spacing)), ClipperLib::jtRound, float(scale_(0.1))))
        append(m_skirt_convex_hull, std::move(poly.points));

    if (m_config.skirt_type == stPerObject) {
        // BBS
        for (auto obj_cvx_hull : object_convex_hulls) {
            double object_skirt_distance = float(scale_(m_config.skirt_distance.value - spacing/2.));
            PrintObject* object = obj_cvx_hull.first;
            object->m_skirt.clear();
            extruded_length.assign(extruded_length.size(), 0.);
            for (size_t i = m_config.skirt_loops.value, extruder_idx = 0; i > 0; -- i) {
                object_skirt_distance += float(scale_(spacing));
                Polygon loop;
                {
                    // BBS. skirt_distance is defined as the gap between skirt and outer most brim, so no need to add max_brim_width
                    Polygons loops = offset(obj_cvx_hull.second, object_skirt_distance, ClipperLib::jtRound, float(scale_(0.1)));
                    Geometry::simplify_polygons(loops, scale_(0.05), &loops);
                    if (loops.empty())
                        break;
                    loop = loops.front();
                }

                // Extrude the skirt loop.
                ExtrusionLoop eloop(elrSkirt);
                eloop.paths.emplace_back(ExtrusionPath(
                    ExtrusionPath(
                        erSkirt,
                        (float)mm3_per_mm,         // this will be overridden at G-code export time
                        flow.width(),
                        (float)initial_layer_print_height  // this will be overridden at G-code export time
                    )));
                eloop.paths.back().polyline = loop.split_at_first_point();
                object->m_skirt.append(std::move(eloop));
                if (m_config.min_skirt_length.value > 0) {
                    // The skirt length is limited. Sum the total amount of filament length extruded, in mm.
                    extruded_length[extruder_idx] += unscale<double>(loop.length()) * extruders_e_per_mm[extruder_idx];
                    if (extruded_length[extruder_idx] < m_config.min_skirt_length.value) {
                        // Not extruded enough yet with the current extruder. Add another loop.
                        if (i == 1)
                            ++ i;
                    } else {
                        assert(extruded_length[extruder_idx] >= m_config.min_skirt_length.value);
                        // Enough extruded with the current extruder. Extrude with the next one,
                        // until the prescribed number of skirt loops is extruded.
                        if (extruder_idx + 1 < extruders.size())
                            ++ extruder_idx;
                    }
                } else {
                    // The skirt lenght is not limited, extrude the skirt with the 1st extruder only.
                }

            }
            object->m_skirt.reverse();
        }
    }
}

Polygons Print::first_layer_islands() const
{
    Polygons islands;
    for (PrintObject *object : m_objects) {
        Polygons object_islands;
        for (ExPolygon &expoly : object->m_layers.front()->lslices)
            object_islands.push_back(expoly.contour);
        if (!object->support_layers().empty()) {
            if (object->support_layers().front()->support_type==stInnerNormal)
                object->support_layers().front()->support_fills.polygons_covered_by_spacing(object_islands, float(SCALED_EPSILON));
            else if(object->support_layers().front()->support_type==stInnerTree) {
                ExPolygons &expolys_first_layer = object->m_support_layers.front()->lslices;
                for (ExPolygon &expoly : expolys_first_layer) { object_islands.push_back(expoly.contour); }
            }
        }
        islands.reserve(islands.size() + object_islands.size() * object->instances().size());
        for (const PrintInstance &instance : object->instances())
            for (Polygon &poly : object_islands) {
                islands.push_back(poly);
                islands.back().translate(instance.shift);
            }
    }
    return islands;
}

Points Print::first_layer_wipe_tower_corners(bool check_wipe_tower_existance) const
{
    Points corners;
    if (check_wipe_tower_existance && (!has_wipe_tower() || m_wipe_tower_data.tool_changes.empty()))
        return corners;
    if (is_BBL_printer() && m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib && m_wipe_tower_data.wipe_tower_mesh_data.has_value() &&
        m_wipe_tower_data.wipe_tower_mesh_data->bottom.size() > 4) {
        // A Bambu Lab rib tower: its generated first-layer outline, brim included.
        Polygon outline = m_wipe_tower_data.wipe_tower_mesh_data->bottom;
        outline.rotate(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value));
        outline.translate(Point(scale_(m_config.wipe_tower_x.get_at(m_plate_index) + m_origin(0)), scale_(m_config.wipe_tower_y.get_at(m_plate_index) + m_origin(1))));
        return outline.points;
    }
    {
        double width = m_config.prime_tower_width + 2*m_wipe_tower_data.brim_width;
        double depth = m_wipe_tower_data.depth + 2*m_wipe_tower_data.brim_width;
        Vec2d pt0(-m_wipe_tower_data.brim_width, -m_wipe_tower_data.brim_width);
        
        // First the corners.
        std::vector<Vec2d> pts = { pt0,
                                   Vec2d(pt0.x()+width, pt0.y()),
                                   Vec2d(pt0.x()+width, pt0.y()+depth),
                                   Vec2d(pt0.x(),pt0.y()+depth)
                                 };

        // Now the stabilization cone.
        Vec2d center = (pts[0] + pts[2])/2.;
        const auto [cone_R, cone_x_scale] = WipeTower2::get_wipe_tower_cone_base(m_config.prime_tower_width, m_wipe_tower_data.height, m_wipe_tower_data.depth, m_config.wipe_tower_cone_angle);
        double r = cone_R + m_wipe_tower_data.brim_width;
        for (double alpha = 0.; alpha<2*M_PI; alpha += M_PI/20.)
            pts.emplace_back(center + r*Vec2d(std::cos(alpha)/cone_x_scale, std::sin(alpha)));

        for (Vec2d& pt : pts) {
            pt = Eigen::Rotation2Dd(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value)) * pt;
            //Orca: offset the wipe tower to the plate origin
            pt += Vec2d(m_config.wipe_tower_x.get_at(m_plate_index) + m_origin(0), m_config.wipe_tower_y.get_at(m_plate_index) + m_origin(1));
            corners.emplace_back(Point(scale_(pt.x()), scale_(pt.y())));
        }
    }
    return corners;
}

//SoftFever
Vec2d Print::translate_to_print_space(const Vec2d &point) const {
    //const BoundingBoxf bed_bbox(config().printable_area.values);
    return Vec2d(point(0) - m_origin(0), point(1) - m_origin(1));
}

Vec2d Print::translate_to_print_space(const Point &point) const {
    return Vec2d(unscaled(point.x()) - m_origin(0), unscaled(point.y()) - m_origin(1));
}

FilamentTempType Print::get_filament_temp_type(const std::string& filament_type)
{
    const static std::string HighTempFilamentStr = "high_temp_filament";
    const static std::string LowTempFilamentStr = "low_temp_filament";
    const static std::string HighLowCompatibleFilamentStr = "high_low_compatible_filament";
    static std::unordered_map<std::string, std::unordered_set<std::string>>filament_temp_type_map;

    if (filament_temp_type_map.empty()) {
        fs::path file_path = fs::path(resources_dir()) / "info" / "filament_info.json";
        std::ifstream in(file_path.string());
        json j;
        try{
            j = json::parse(in);
            in.close();
            auto&&high_temp_filament_arr =j[HighTempFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[HighTempFilamentStr] = std::unordered_set<std::string>(high_temp_filament_arr.begin(), high_temp_filament_arr.end());
            auto&& low_temp_filament_arr = j[LowTempFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[LowTempFilamentStr] = std::unordered_set<std::string>(low_temp_filament_arr.begin(), low_temp_filament_arr.end());
            auto&& high_low_compatible_filament_arr = j[HighLowCompatibleFilamentStr].get < std::vector<std::string>>();
            filament_temp_type_map[HighLowCompatibleFilamentStr] = std::unordered_set<std::string>(high_low_compatible_filament_arr.begin(), high_low_compatible_filament_arr.end());
        }
        catch (const json::parse_error& err){
            in.close();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file_path.string() << " got a nlohmann::detail::parse_error, reason = " << err.what();
            filament_temp_type_map[HighTempFilamentStr] = {"ABS","ASA","PC","PA","PA-CF","PA-GF","PA6-CF","PET-CF", "PETG-GF","PPS","PPS-CF","PPA-GF","PPA-CF","ABS-Aero","ABS-GF"};
            filament_temp_type_map[LowTempFilamentStr] = {"PLA","TPU","PLA-CF","PLA-AERO","PVA","BVOH","SBS"};
            filament_temp_type_map[HighLowCompatibleFilamentStr] = { "HIPS","PETG","PCTG","PE","PP","EVA","PE-CF","PP-CF","PP-GF","PHA"};
        }
    }

    if (filament_temp_type_map[HighLowCompatibleFilamentStr].find(filament_type) != filament_temp_type_map[HighLowCompatibleFilamentStr].end())
        return HighLowCompatible;
    if (filament_temp_type_map[HighTempFilamentStr].find(filament_type) != filament_temp_type_map[HighTempFilamentStr].end())
        return HighTemp;
    if (filament_temp_type_map[LowTempFilamentStr].find(filament_type) != filament_temp_type_map[LowTempFilamentStr].end())
        return LowTemp;
    return Undefine;
}

int Print::get_hrc_by_nozzle_type(const NozzleType&type)
{
    static std::map<std::string, int>nozzle_type_to_hrc;
    if (nozzle_type_to_hrc.empty()) {
        fs::path file_path = fs::path(resources_dir()) / "info" / "nozzle_info.json";
        boost::nowide::ifstream in(file_path.string());
        //std::ifstream in(file_path.string());
        json j;
        try {
            j = json::parse(in);
            in.close();
            for (const auto& elem : j["nozzle_hrc"].items())
                nozzle_type_to_hrc[elem.key()] = elem.value();
        }
        catch (const json::parse_error& err) {
            in.close();
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << file_path.string() << " got a nlohmann::detail::parse_error, reason = " << err.what();
            nozzle_type_to_hrc = {
                {"hardened_steel",55},
                {"stainless_steel",20},
                {"brass",2},
                {"undefine",0}
            };
        }
    }
    auto iter = nozzle_type_to_hrc.find(NozzleTypeEumnToStr[type]);
    if (iter != nozzle_type_to_hrc.end())
        return iter->second;
    //0 represents undefine
    return 0;
}

void Print::finalize_first_layer_convex_hull()
{
    append(m_first_layer_convex_hull.points, m_skirt_convex_hull);
    if (m_first_layer_convex_hull.empty()) {
        // Neither skirt nor brim was extruded. Collect points of printed objects from 1st layer.
        for (Polygon &poly : this->first_layer_islands())
            append(m_first_layer_convex_hull.points, std::move(poly.points));
    }
    append(m_first_layer_convex_hull.points, this->first_layer_wipe_tower_corners());
    m_first_layer_convex_hull = Geometry::convex_hull(m_first_layer_convex_hull.points);
}

// Wipe tower support.
bool Print::has_wipe_tower() const
{
    if (m_config.enable_prime_tower.value == true) {
        if (enable_timelapse_print())
            return true;

        return !m_config.spiral_mode.value && m_config.filament_diameter.values.size() > 1;
    }
    return false;
}

const WipeTowerData &Print::wipe_tower_data(size_t filaments_cnt) const
{
    // Until the tower is generated, size it with the estimate the GUI/CLI placement uses, so
    // validation cannot reject a position the clamp just accepted.
    if (is_step_done(psWipeTower) || filaments_cnt == 0)
        return m_wipe_tower_data;

    double max_height   = 0.;
    double layer_height = std::numeric_limits<double>::max();
    for (const PrintObject *object : m_objects) {
        max_height   = std::max(max_height, unscale_(double(object->size().z())));
        layer_height = std::min(layer_height, object->config().layer_height.value);
    }
    if (max_height < EPSILON)
        return m_wipe_tower_data;

    const WipeTowerFootprint footprint = estimate_wipe_tower_footprint(m_config, resolve_wipe_tower_type(m_config), this->extruders(true), layer_height, max_height);
    WipeTowerData &data = const_cast<Print *>(this)->m_wipe_tower_data;
    data.depth      = float(footprint.depth);
    data.width      = float(footprint.width);
    data.brim_width = float(footprint.brim_width);
    return m_wipe_tower_data;
}

bool Print::enable_timelapse_print() const
{
    return m_config.timelapse_type.value == TimelapseType::tlSmooth;
}

void Print::_make_wipe_tower()
{
    m_wipe_tower_data.clear();

    // Get wiping matrix to get number of extruders and convert vector<double> to vector<float>:
    std::vector<float> flush_matrix(cast<float>(m_config.flush_volumes_matrix.values));

    // BBS
    const unsigned int number_of_extruders = (unsigned int)(sqrt(flush_matrix.size()) + EPSILON);
    // Extract purging volumes for each extruder pair:
    std::vector<std::vector<float>> wipe_volumes;
    for (unsigned int i = 0; i<number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(flush_matrix.begin()+i*number_of_extruders, flush_matrix.begin()+(i+1)*number_of_extruders));

    const auto bUseWipeTower2 = is_BBL_printer() ? false : true;
    // Orca: itertate over wipe_volumes and change the non-zero values to the prime_volume
    if ((!m_config.purge_in_prime_tower || !m_config.single_extruder_multi_material) && !is_BBL_printer()) {
        for (unsigned int i = 0; i < number_of_extruders; ++i) {
            for (unsigned int j = 0; j < number_of_extruders; ++j) {
                if (wipe_volumes[i][j] > 0) {
                    wipe_volumes[i][j] = m_config.prime_volume;
                }
            }
        }
    }

    // Let the ToolOrdering class know there will be initial priming extrusions at the start of the print.
    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (unsigned int) -1, bUseWipeTower2 ? true : false);

    if (!m_wipe_tower_data.tool_ordering.has_wipe_tower())
        // Don't generate any wipe tower.
        return;

    // Check whether there are any layers in m_tool_ordering, which are marked with has_wipe_tower,
    // they print neither object, nor support. These layers are above the raft and below the object, and they
    // shall be added to the support layers to be printed.
    // see https://github.com/prusa3d/PrusaSlicer/issues/607
    {
        size_t idx_begin = size_t(-1);
        size_t idx_end   = m_wipe_tower_data.tool_ordering.layer_tools().size();
        // Find the first wipe tower layer, which does not have a counterpart in an object or a support layer.
        for (size_t i = 0; i < idx_end; ++ i) {
            const LayerTools &lt = m_wipe_tower_data.tool_ordering.layer_tools()[i];
            if (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support) {
                idx_begin = i;
                break;
            }
        }
        if (idx_begin != size_t(-1)) {
            // Find the position in m_objects.first()->support_layers to insert these new support layers.
            double wipe_tower_new_layer_print_z_first = m_wipe_tower_data.tool_ordering.layer_tools()[idx_begin].print_z;
            auto it_layer = m_objects.front()->support_layers().begin();
            auto it_end   = m_objects.front()->support_layers().end();
            for (; it_layer != it_end && (*it_layer)->print_z - EPSILON < wipe_tower_new_layer_print_z_first; ++ it_layer);
            // Find the stopper of the sequence of wipe tower layers, which do not have a counterpart in an object or a support layer.
            for (size_t i = idx_begin; i < idx_end; ++ i) {
                LayerTools &lt = const_cast<LayerTools&>(m_wipe_tower_data.tool_ordering.layer_tools()[i]);
                if (! (lt.has_wipe_tower && ! lt.has_object && ! lt.has_support))
                    break;
                lt.has_support = true;
                // Insert the new support layer.
                double height    = lt.print_z - (i == 0 ? 0. : m_wipe_tower_data.tool_ordering.layer_tools()[i-1].print_z);
                //FIXME the support layer ID is set to -1, as Vojtech hopes it is not being used anyway.
                it_layer = m_objects.front()->insert_support_layer(it_layer, -1, 0, height, lt.print_z, lt.print_z - 0.5 * height);
                ++ it_layer;
            }
        }
    }
    this->throw_if_canceled();

    if (!bUseWipeTower2) {
        // in BBL machine, wipe tower is only use to prime extruder. So just use a global wipe volume.
        WipeTower wipe_tower(m_config, m_plate_index, m_origin, m_config.prime_volume, m_wipe_tower_data.tool_ordering.first_extruder(),
                             m_wipe_tower_data.tool_ordering.empty() ? 0.f : m_wipe_tower_data.tool_ordering.back().print_z);

        // wipe_tower.set_retract();
        // wipe_tower.set_zhop();

        // Set the extruder & material properties at the wipe tower object.
        for (size_t i = 0; i < number_of_extruders; ++i)
            wipe_tower.set_extruder(i, m_config);

        // BBS: remove priming logic
        // m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
        //    wipe_tower.prime((float)this->skirt_first_layer_height(), m_wipe_tower_data.tool_ordering.all_extruders(), false));

        // Lets go through the wipe tower layers and determine pairs of extruder changes for each
        // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
        {
            // BBS: priming logic is removed, so get the initial extruder by first_extruder()
            unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.first_extruder();
            for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) { // for all layers
                if (!layer_tools.has_wipe_tower)
                    continue;
                bool first_layer = &layer_tools == &m_wipe_tower_data.tool_ordering.front();
                wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height, current_extruder_id,
                                           current_extruder_id);

                for (const auto extruder_id : layer_tools.extruders) {
                    // BBS: priming logic is removed, so no need to do toolchange for first extruder
                    if (/*(first_layer && extruder_id == m_wipe_tower_data.tool_ordering.all_extruders().back()) || */ extruder_id !=
                        current_extruder_id) {
                        float volume_to_purge = wipe_volumes[current_extruder_id][extruder_id];
                        volume_to_purge *= m_config.flush_multiplier;

                        // Not all of that can be used for infill purging:
                        // volume_to_purge -= (float)get_value_at(m_config, m_config.filament_minimal_purge_on_wipe_tower, ConfigFlowDomain::Filament, extruder_id);

                        // try to assign some infills/objects for the wiping:
                        volume_to_purge = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id,
                                                                                                 volume_to_purge);

                        // add back the minimal amount toforce on the wipe tower:
                        // volume_to_purge += (float)get_value_at(m_config, m_config.filament_minimal_purge_on_wipe_tower, ConfigFlowDomain::Filament, extruder_id);

                        // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
                        wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height,
                                                   current_extruder_id, extruder_id, m_config.prime_volume, volume_to_purge);
                        current_extruder_id = extruder_id;
                    }
                }
                layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);

                // if enable timelapse, slice all layer
                if (enable_timelapse_print()) {
                    if (layer_tools.wipe_tower_partitions == 0)
                        wipe_tower.set_last_layer_extruder_fill(false);
                    continue;
                }

                if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                    break;
            }
        }

        // Generate the wipe tower layers.
        m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
        wipe_tower.generate(m_wipe_tower_data.tool_changes);
        // A rib tower is placed by the box around its first-layer wall (the generator shifts its
        // output by the rib offset so that box starts at the tower position), which is also the
        // square the pre-slice estimate reserves for it: report that box, not the body.
        const Vec2f wt_footprint     = wipe_tower.get_footprint_size();
        m_wipe_tower_data.depth      = wt_footprint.y();
        m_wipe_tower_data.width      = wt_footprint.x();
        m_wipe_tower_data.brim_width = wipe_tower.get_brim_width();
        // The Type1 (Bambu-style) tower ignores the cone wall option, exactly as the pre-slice
        // estimate (estimate_wipe_tower_first_layer_outline) does, so the two footprints agree.
        m_wipe_tower_data.construct_mesh(wipe_tower.width(), wipe_tower.get_depth(), wipe_tower.get_height(), wipe_tower.get_brim_width(),
                                         wipe_tower.is_rib_wall(), wipe_tower.get_rib_width(), wipe_tower.get_rib_length(),
                                         m_config.wipe_tower_fillet_wall,
                                         0.f);
        if (wipe_tower.is_rib_wall() && ! wipe_tower.get_first_layer_wall().empty()) {
            // The printed first layer: the rib wall and the brim loops laid around it.
            Polygon bottom = wipe_tower.get_first_layer_wall();
            if (wipe_tower.get_brim_width() > EPSILON) {
                Polygons brimmed = offset(bottom, float(scale_(wipe_tower.get_brim_width())));
                if (! brimmed.empty())
                    bottom = brimmed.front();
            }
            m_wipe_tower_data.wipe_tower_mesh_data->bottom = std::move(bottom);
        }

        // Unload the current filament over the purge tower.
        coordf_t layer_height = m_objects.front()->config().layer_height.value;
        if (m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions > 0) {
            // The wipe tower goes up to the last layer of the print.
            if (wipe_tower.layer_finished()) {
                // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
                // Lift Z to the next layer.
                wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z + layer_height), float(layer_height), 0, false,
                                     true);
            } else {
                // There is yet enough space at this layer of the wipe tower for the final purge.
            }
        } else {
            // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
            assert(m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions == 0);
            wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z), float(layer_height), 0, false, true);
        }
        m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(wipe_tower.tool_change((unsigned int) (-1)));

        m_wipe_tower_data.used_filament         = wipe_tower.get_used_filament();
        m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
        const Vec3d origin                      = this->get_plate_origin();
        m_fake_wipe_tower.set_fake_extrusion_data(wipe_tower.position(), m_wipe_tower_data.width, wipe_tower.get_height(),
                                                  wipe_tower.get_layer_height(), m_wipe_tower_data.depth, m_wipe_tower_data.brim_width,
                                                  {scale_(origin.x()), scale_(origin.y())});
        // The rib wall's real outline for the conflict checker and the object brim, as Bambu Studio
        // hands over its outer walls; a rectangle tower keeps the box model.
        m_fake_wipe_tower.outer_wall = wipe_tower.get_outer_wall();
    } else {
        // Initialize the wipe tower.
        WipeTower2 wipe_tower(m_config, m_default_region_config, m_plate_index, m_origin, wipe_volumes,
                              m_wipe_tower_data.tool_ordering.first_extruder());
        const std::vector<std::pair<coordf_t, std::vector<GCode::LayerToPrint>>> layers_to_print = GCode::collect_layers_to_print(*this);
        size_t layers_to_print_idx = 0;

        // wipe_tower.set_retract();
        // wipe_tower.set_zhop();

        // Set the extruder & material properties at the wipe tower object.
        for (size_t i = 0; i < number_of_extruders; ++i)
            wipe_tower.set_extruder(i, m_config);

        m_wipe_tower_data.priming = Slic3r::make_unique<std::vector<WipeTower::ToolChangeResult>>(
            wipe_tower.prime((float)this->skirt_first_layer_height(), m_wipe_tower_data.tool_ordering.all_extruders(), false));

        // Lets go through the wipe tower layers and determine pairs of extruder changes for each
        // to pass to wipe_tower (so that it can use it for planning the layout of the tower)
        {
            unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.all_extruders().back();
            for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) { // for all layers
                if (!layer_tools.has_wipe_tower)
                    continue;
                while (layers_to_print_idx + 1 < layers_to_print.size() &&
                       layers_to_print[layers_to_print_idx].first + EPSILON < layer_tools.print_z) {
                    ++layers_to_print_idx;
                }

                const std::vector<GCode::LayerToPrint> *layers_with_same_print_z = nullptr;
                if (layers_to_print_idx < layers_to_print.size() &&
                    std::abs(layers_to_print[layers_to_print_idx].first - layer_tools.print_z) <= EPSILON) {
                    layers_with_same_print_z = &layers_to_print[layers_to_print_idx].second;
                }

                bool first_layer = &layer_tools == &m_wipe_tower_data.tool_ordering.front();

                if (layers_with_same_print_z != nullptr) {
                    const std::vector<LocalZWipeTowerToolchange> local_z_toolchanges =
                        collect_local_z_wipe_tower_toolchanges(*this, *layers_with_same_print_z, int(current_extruder_id));
                    if (!local_z_toolchanges.empty()) {
                        std::ostringstream local_z_sequence;
                        for (size_t toolchange_idx = 0; toolchange_idx < local_z_toolchanges.size(); ++toolchange_idx) {
                            if (toolchange_idx != 0)
                                local_z_sequence << ",";
                            local_z_sequence << local_z_toolchanges[toolchange_idx].old_tool << "->"
                                             << local_z_toolchanges[toolchange_idx].new_tool;
                        }

                        BOOST_LOG_TRIVIAL(debug) << "Local-Z wipe tower preplan"
                                                 << " print_z=" << layer_tools.print_z
                                                 << " start_tool=" << current_extruder_id
                                                 << " nominal_toolchanges=" << layer_tools.extruders.size()
                                                 << " local_z_toolchanges=" << local_z_toolchanges.size()
                                                 << " sequence=" << local_z_sequence.str();
                    }
                    for (const LocalZWipeTowerToolchange &toolchange : local_z_toolchanges) {
                        wipe_tower.plan_local_z_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height,
                                                           toolchange.old_tool, toolchange.new_tool, (float) m_config.prime_volume);
                    }
                    if (!local_z_toolchanges.empty())
                        current_extruder_id = local_z_toolchanges.back().new_tool;
                }

                const std::vector<unsigned int> nominal_layer_extruders =
                    rotate_extruders_to_start_with(layer_tools.extruders, current_extruder_id);

                wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height, current_extruder_id,
                                           current_extruder_id, false);
                for (const auto extruder_id : nominal_layer_extruders) {
                    if ((first_layer && extruder_id == m_wipe_tower_data.tool_ordering.all_extruders().back()) || extruder_id !=
                        current_extruder_id) {
                        float volume_to_wipe = m_config.prime_volume;
                        if (m_config.purge_in_prime_tower && m_config.single_extruder_multi_material) {
                            volume_to_wipe = wipe_volumes[current_extruder_id][extruder_id]; // total volume to wipe after this toolchange
                            volume_to_wipe *= m_config.flush_multiplier;
                            // Not all of that can be used for infill purging:
                            volume_to_wipe -= (float) get_value_at(m_config, m_config.filament_minimal_purge_on_wipe_tower, ConfigFlowDomain::Filament, extruder_id);

                            // try to assign some infills/objects for the wiping:
                            volume_to_wipe = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id,
                                                                                                    volume_to_wipe);

                            // add back the minimal amount toforce on the wipe tower:
                            volume_to_wipe += (float) get_value_at(m_config, m_config.filament_minimal_purge_on_wipe_tower, ConfigFlowDomain::Filament, extruder_id);
                        }

                        // request a toolchange at the wipe tower with at least volume_to_wipe purging amount
                        wipe_tower.plan_toolchange((float) layer_tools.print_z, (float) layer_tools.wipe_tower_layer_height,
                                                   current_extruder_id, extruder_id, volume_to_wipe);
                        current_extruder_id = extruder_id;
                    }
                }

                layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);
                if (&layer_tools == &m_wipe_tower_data.tool_ordering.back() || (&layer_tools + 1)->wipe_tower_partitions == 0)
                    break;
            }
        }

        // Generate the wipe tower layers.
        m_wipe_tower_data.tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
        m_wipe_tower_data.local_z_tool_changes.reserve(m_wipe_tower_data.tool_ordering.layer_tools().size());
        wipe_tower.generate(m_wipe_tower_data.tool_changes, m_wipe_tower_data.local_z_tool_changes);
        BOOST_LOG_TRIVIAL(debug) << "Wipe tower generation completed"
                                 << " nominal_layers=" << m_wipe_tower_data.tool_changes.size()
                                 << " local_z_layers=" << m_wipe_tower_data.local_z_tool_changes.size();
        m_wipe_tower_data.depth             = wipe_tower.get_depth();
        m_wipe_tower_data.width             = wipe_tower.width();
        m_wipe_tower_data.z_and_depth_pairs = wipe_tower.get_z_and_depth_pairs();
        m_wipe_tower_data.local_z_reserve_boxes = wipe_tower.get_local_z_reserve_boxes();
        m_wipe_tower_data.brim_width        = wipe_tower.get_brim_width();
        m_wipe_tower_data.height            = wipe_tower.get_wipe_tower_height();
        m_wipe_tower_data.construct_mesh(wipe_tower.width(), wipe_tower.get_depth(), wipe_tower.get_wipe_tower_height(),
                                         wipe_tower.get_brim_width(), m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwRib,
                                         float(m_config.wipe_tower_rib_width), float(m_config.wipe_tower_extra_rib_length),
                                         m_config.wipe_tower_fillet_wall,
                                         m_config.wipe_tower_wall_type.value == WipeTowerWallType::wtwCone ? float(m_config.wipe_tower_cone_angle) : 0.f);

        // Unload the current filament over the purge tower.
        coordf_t layer_height = m_objects.front()->config().layer_height.value;
        if (m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions > 0) {
            // The wipe tower goes up to the last layer of the print.
            if (wipe_tower.layer_finished()) {
                // The wipe tower is printed to the top of the print and it has no space left for the final extruder purge.
                // Lift Z to the next layer.
                wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z + layer_height), float(layer_height), 0, false,
                                     true);
            } else {
                // There is yet enough space at this layer of the wipe tower for the final purge.
            }
        } else {
            // The wipe tower does not reach the last print layer, perform the pruge at the last print layer.
            assert(m_wipe_tower_data.tool_ordering.back().wipe_tower_partitions == 0);
            wipe_tower.set_layer(float(m_wipe_tower_data.tool_ordering.back().print_z), float(layer_height), 0, false, true);
        }
        m_wipe_tower_data.final_purge = Slic3r::make_unique<WipeTower::ToolChangeResult>(wipe_tower.tool_change((unsigned int) (-1)));

        m_wipe_tower_data.used_filament         = wipe_tower.get_used_filament();
        m_wipe_tower_data.number_of_toolchanges = wipe_tower.get_number_of_toolchanges();
        const Vec3d origin                      = Vec3d::Zero();
        m_fake_wipe_tower.set_fake_extrusion_data(wipe_tower.position(), wipe_tower.width(), wipe_tower.get_wipe_tower_height(),
                                                  config().initial_layer_print_height, m_wipe_tower_data.depth,
                                                  m_wipe_tower_data.z_and_depth_pairs, m_wipe_tower_data.brim_width,
                                                  config().wipe_tower_rotation_angle, config().wipe_tower_cone_angle,
                                                  {scale_(origin.x()), scale_(origin.y())});
        m_fake_wipe_tower.outer_wall = wipe_tower.get_outer_wall();
    }

    // The clamps and checks above work from estimates; re-test the exact generated footprint
    // so an off-plate tower fails with a clear error instead of exporting unprintable G-code.
    if (m_wipe_tower_data.wipe_tower_mesh_data) {
        Polygon footprint = m_wipe_tower_data.wipe_tower_mesh_data->bottom; // includes brim
        footprint.rotate(Geometry::deg2rad(m_config.wipe_tower_rotation_angle.value));
        footprint.translate(Point(scale_(m_config.wipe_tower_x.get_at(m_plate_index)),
                                  scale_(m_config.wipe_tower_y.get_at(m_plate_index))));
        Polygon printable_poly;
        printable_poly.points = get_bed_shape(m_config);
        const Polygons printable_polys{printable_poly};
        if (!printable_poly.empty() && !diff(Polygons{footprint}, printable_polys).empty()) {
            const BoundingBox fp = get_extents(footprint);
            const BoundingBox pr = get_extents(printable_polys);
            BOOST_LOG_TRIVIAL(error) << boost::format("wipe tower footprint [%1%,%2%]-[%3%,%4%] leaves printable [%5%,%6%]-[%7%,%8%]") %
                unscaled(fp.min.x()) % unscaled(fp.min.y()) % unscaled(fp.max.x()) % unscaled(fp.max.y()) %
                unscaled(pr.min.x()) % unscaled(pr.min.y()) % unscaled(pr.max.x()) % unscaled(pr.max.y());
            throw Slic3r::SlicingError(L("Prime Tower") + L(" is partially outside the printable area, and it cannot be printed.\n"));
        }
        // Keep the existing msgid (do not add "an").
        if (!intersection(get_bed_excluded_area(m_config), Polygons{footprint}).empty())
            throw Slic3r::SlicingError(L("Prime Tower") + L(" is too close to exclusion area, and collisions will be caused.\n"));
    }
}

void WipeTowerData::construct_mesh(float width, float depth, float height, float brim_width, bool /*is_rib_wipe_tower*/, float /*rib_width*/, float /*rib_length*/, bool /*fillet_wall*/, float cone_angle)
{
    wipe_tower_mesh_data = WipeTowerMeshData{};
    if (width <= EPSILON || depth <= EPSILON)
        return;
    wipe_tower_mesh_data->bottom = WipeTower2::cone_base_polygon(width, depth, height, cone_angle);
    if (brim_width > EPSILON) {
        Polygons brimmed = offset(wipe_tower_mesh_data->bottom, float(scale_(brim_width)));
        if (!brimmed.empty())
            wipe_tower_mesh_data->bottom = brimmed.front();
    }
}

// Generate a recommended G-code output file name based on the format template, default extension, and template parameters
// (timestamps, object placeholders derived from the model, current placeholder prameters and print statistics.
// Use the final print statistics if available, or just keep the print statistics placeholders if not available yet (before G-code is finalized).
std::string Print::output_filename(const std::string &filename_base) const
{
    // Set the placeholders for the data know first after the G-code export is finished.
    // These values will be just propagated into the output file name.
    DynamicConfig config = this->finished() ? this->print_statistics().config() : this->print_statistics().placeholders();
    config.set_key_value("num_filaments", new ConfigOptionInt((int)m_config.nozzle_diameter.size()));
    config.set_key_value("num_extruders", new ConfigOptionInt((int) m_config.nozzle_diameter.size()));
    config.set_key_value("plate_name", new ConfigOptionString(get_plate_name()));
    config.set_key_value("plate_number", new ConfigOptionString(get_plate_number_formatted()));
    config.set_key_value("model_name", new ConfigOptionString(get_model_name()));

    return this->PrintBase::output_filename(m_config.filename_format.value, ".gcode", filename_base, &config);
}

std::string Print::get_model_name() const
{
    if (model().model_info != nullptr)
    {
        return model().model_info->model_name;
    } else {
        return "";
    }
}

std::string Print::get_plate_number_formatted() const
{
    std::string plate_number = std::to_string(get_plate_index() + 1);
    static const size_t n_zero = 2;

    return std::string(n_zero - std::min(n_zero, plate_number.length()), '0') + plate_number;
}

//BBS: add gcode file preload logic
void Print::set_gcode_file_ready()
{
    this->set_started(psGCodeExport);
	this->set_done(psGCodeExport);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(": done");
}
//BBS: add gcode file preload logic
void Print::set_gcode_file_invalidated()
{
    this->invalidate_step(psGCodeExport);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(": done");
}

//BBS: add gcode file preload logic
void Print::export_gcode_from_previous_file(const std::string& file, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{
    try {
        GCodeProcessor processor;
        GCodeProcessor::s_IsBBLPrinter = is_BBL_printer();
        const Vec3d origin = this->get_plate_origin();
        processor.set_xy_offset(origin(0), origin(1));
        //processor.enable_producers(true);
        processor.process_file(file);

        *result = std::move(processor.extract_result());
    } catch (std::exception & /* ex */) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ <<  boost::format(": found errors when process gcode file %1%") %file.c_str();
        throw Slic3r::RuntimeError(
            std::string("Failed to process the G-code file ") + file + " from previous 3mf\n");
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ <<  boost::format(":  process the G-code file %1% successfully")%file.c_str();
}

std::tuple<float, float> Print::object_skirt_offset(double margin_height) const
{
    if (config().skirt_loops == 0 || config().skirt_type != stPerObject)
        return std::make_tuple(0, 0);
    
    float max_nozzle_diameter = *std::max_element(m_config.nozzle_diameter.values.begin(), m_config.nozzle_diameter.values.end());
    float max_layer_height    = *std::max_element(config().max_layer_height.values.begin(), config().max_layer_height.values.end());
    float line_width = m_config.initial_layer_line_width.get_abs_value(max_nozzle_diameter);
    float object_skirt_witdh  = skirt_flow().width() + (config().skirt_loops - 1) * skirt_flow().spacing();
    float object_skirt_offset = 0;

    if (is_all_objects_are_short())
        object_skirt_offset = config().skirt_distance + object_skirt_witdh;
    else if (config().draft_shield == dsEnabled || config().skirt_height * max_layer_height > config().nozzle_height - margin_height)
        object_skirt_offset = config().skirt_distance + line_width;
    else if (config().skirt_distance + object_skirt_witdh > config().extruder_clearance_radius/2)
        object_skirt_offset = (config().skirt_distance + object_skirt_witdh - config().extruder_clearance_radius/2);
    else
        return std::make_tuple(0, 0);

    return std::make_tuple(object_skirt_offset, object_skirt_witdh);
}

DynamicConfig PrintStatistics::config() const
{
    DynamicConfig config;
    std::string normal_print_time = short_time(this->estimated_normal_print_time);
    std::string silent_print_time = short_time(this->estimated_silent_print_time);
    config.set_key_value("print_time", new ConfigOptionString(normal_print_time));
    config.set_key_value("normal_print_time", new ConfigOptionString(normal_print_time));
    config.set_key_value("silent_print_time", new ConfigOptionString(silent_print_time));
    config.set_key_value("used_filament",             new ConfigOptionFloat(this->total_used_filament / 1000.));
    config.set_key_value("extruded_volume",           new ConfigOptionFloat(this->total_extruded_volume));
    config.set_key_value("total_cost",                new ConfigOptionFloat(this->total_cost));
    config.set_key_value("total_toolchanges",         new ConfigOptionInt(this->total_toolchanges));
    config.set_key_value("total_weight",              new ConfigOptionFloat(this->total_weight));
    config.set_key_value("total_wipe_tower_cost",     new ConfigOptionFloat(this->total_wipe_tower_cost));
    config.set_key_value("total_wipe_tower_filament", new ConfigOptionFloat(this->total_wipe_tower_filament));
    config.set_key_value("initial_tool",              new ConfigOptionInt(static_cast<int>(this->initial_tool)));
    return config;
}

DynamicConfig PrintStatistics::placeholders()
{
    DynamicConfig config;
    for (const std::string key : {
        "print_time", "normal_print_time", "silent_print_time",
        "used_filament", "extruded_volume", "total_cost", "total_weight",
        "initial_tool", "total_toolchanges", "total_wipe_tower_cost", "total_wipe_tower_filament"})
        config.set_key_value(key, new ConfigOptionString(std::string("{") + key + "}"));
    return config;
}

std::string PrintStatistics::finalize_output_path(const std::string &path_in) const
{
    std::string final_path;
    try {
        boost::filesystem::path path(path_in);
        DynamicConfig cfg = this->config();
        PlaceholderParser pp;
        std::string new_stem = pp.process(path.stem().string(), 0, &cfg);
        final_path = (path.parent_path() / (new_stem + path.extension().string())).string();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Failed to apply the print statistics to the export file name: " << ex.what();
        final_path = path_in;
    }
    return final_path;
}

// Orca: Implement prusa's filament shrink compensation approach
// Returns if all used filaments have same shrinkage compensations.
 bool Print::has_same_shrinkage_compensations() const {
     const std::vector<unsigned int> extruders = this->extruders();
     if (extruders.empty())
         return false;

     const double filament_shrinkage_compensation_xy = m_config.filament_shrink.get_at(extruders.front());
     const double filament_shrinkage_compensation_z  = m_config.filament_shrinkage_compensation_z.get_at(extruders.front());

     for (unsigned int extruder : extruders) {
         if (filament_shrinkage_compensation_xy != m_config.filament_shrink.get_at(extruder) ||
             filament_shrinkage_compensation_z  != m_config.filament_shrinkage_compensation_z.get_at(extruder)) {
             return false;
         }
     }

     return true;
 }

// Orca: Implement prusa's filament shrink compensation approach, but amended so 100% from the user is the equivalent to 0 in orca.
 // Returns scaling for each axis representing shrinkage compensations in each axis.
Vec3d Print::shrinkage_compensation() const
{
    if (!this->has_same_shrinkage_compensations())
        return Vec3d::Ones();

    const unsigned int first_extruder = this->extruders().front();

    const double xy_shrinkage_percent = m_config.filament_shrink.get_at(first_extruder);
    const double z_shrinkage_percent  = m_config.filament_shrinkage_compensation_z.get_at(first_extruder);

    const double xy_compensation = 100.0 / xy_shrinkage_percent;
    const double z_compensation  = 100.0 / z_shrinkage_percent;

    return { xy_compensation, xy_compensation, z_compensation };
}

const std::string PrintStatistics::FilamentUsedG     = "filament used [g]";
const std::string PrintStatistics::FilamentUsedGMask = "; filament used [g] =";

const std::string PrintStatistics::TotalFilamentUsedG          = "total filament used [g]";
const std::string PrintStatistics::TotalFilamentUsedGMask      = "; total filament used [g] =";
const std::string PrintStatistics::TotalFilamentUsedGValueMask = "; total filament used [g] = %.2lf\n";

const std::string PrintStatistics::FilamentUsedCm3     = "filament used [cm3]";
const std::string PrintStatistics::FilamentUsedCm3Mask = "; filament used [cm3] =";

const std::string PrintStatistics::FilamentUsedMm     = "filament used [mm]";
const std::string PrintStatistics::FilamentUsedMmMask = "; filament used [mm] =";

const std::string PrintStatistics::FilamentCost     = "filament cost";
const std::string PrintStatistics::FilamentCostMask = "; filament cost =";

const std::string PrintStatistics::TotalFilamentCost          = "total filament cost";
const std::string PrintStatistics::TotalFilamentCostMask      = "; total filament cost =";
const std::string PrintStatistics::TotalFilamentCostValueMask = "; total filament cost = %.2lf\n";

const std::string PrintStatistics::TotalFilamentUsedWipeTower     = "total filament used for wipe tower [g]";
const std::string PrintStatistics::TotalFilamentUsedWipeTowerValueMask = "; total filament used for wipe tower [g] = %.2lf\n";


/*add json export/import related functions */
#define JSON_POLYGON_CONTOUR                "contour"
#define JSON_POLYGON_HOLES                  "holes"
#define JSON_POINTS                 "points"
#define JSON_EXPOLYGON              "expolygon"
#define JSON_ARC_FITTING            "arc_fitting"
#define JSON_OBJECT_NAME            "name"
#define JSON_IDENTIFY_ID          "identify_id"


#define JSON_LAYERS                  "layers"
#define JSON_SUPPORT_LAYERS                  "support_layers"
#define JSON_TREE_SUPPORT_LAYERS                  "tree_support_layers"
#define JSON_LAYER_REGIONS                  "layer_regions"
#define JSON_FIRSTLAYER_GROUPS                  "first_layer_groups"

#define JSON_FIRSTLAYER_GROUP_ID                  "group_id"
#define JSON_FIRSTLAYER_GROUP_VOLUME_IDS          "volume_ids"
#define JSON_FIRSTLAYER_GROUP_SLICES               "slices"

#define JSON_LAYER_PRINT_Z            "print_z"
#define JSON_LAYER_SLICE_Z            "slice_z"
#define JSON_LAYER_HEIGHT             "height"
#define JSON_LAYER_ID                  "layer_id"
#define JSON_LAYER_SLICED_POLYGONS    "sliced_polygons"
#define JSON_LAYER_SLLICED_BBOXES      "sliced_bboxes"
#define JSON_LAYER_OVERHANG_POLYGONS    "overhang_polygons"
#define JSON_LAYER_OVERHANG_BBOX       "overhang_bbox"

#define JSON_SUPPORT_LAYER_ISLANDS                  "support_islands"
#define JSON_SUPPORT_LAYER_FILLS                    "support_fills"
#define JSON_SUPPORT_LAYER_INTERFACE_ID             "interface_id"
#define JSON_SUPPORT_LAYER_TYPE                     "support_type"

#define JSON_LAYER_REGION_CONFIG_HASH             "config_hash"
#define JSON_LAYER_REGION_SLICES                  "slices"
#define JSON_LAYER_REGION_RAW_SLICES              "raw_slices"
//#define JSON_LAYER_REGION_ENTITIES                "entities"
#define JSON_LAYER_REGION_THIN_FILLS                  "thin_fills"
#define JSON_LAYER_REGION_FILL_EXPOLYGONS             "fill_expolygons"
#define JSON_LAYER_REGION_FILL_SURFACES               "fill_surfaces"
#define JSON_LAYER_REGION_FILL_NO_OVERLAP             "fill_no_overlap_expolygons"
#define JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES    "unsupported_bridge_edges"
#define JSON_LAYER_REGION_PERIMETERS                  "perimeters"
#define JSON_LAYER_REGION_FILLS                  "fills"



#define JSON_SURF_TYPE              "surface_type"
#define JSON_SURF_THICKNESS         "thickness"
#define JSON_SURF_THICKNESS_LAYER   "thickness_layers"
#define JSON_SURF_BRIDGE_ANGLE       "bridge_angle"
#define JSON_SURF_EXTRA_PERIMETERS   "extra_perimeters"

#define JSON_ARC_DATA                "arc_data"
#define JSON_ARC_START_INDEX         "start_index"
#define JSON_ARC_END_INDEX           "end_index"
#define JSON_ARC_PATH_TYPE           "path_type"

#define JSON_IS_ARC                  "is_arc"
#define JSON_ARC_LENGTH              "length"
#define JSON_ARC_ANGLE_RADIUS        "angle_radians"
#define JSON_ARC_POLAY_START_THETA   "polar_start_theta"
#define JSON_ARC_POLAY_END_THETA     "polar_end_theta"
#define JSON_ARC_START_POINT          "start_point"
#define JSON_ARC_END_POINT            "end_point"
#define JSON_ARC_DIRECTION            "direction"
#define JSON_ARC_RADIUS               "radius"
#define JSON_ARC_CENTER               "center"

//extrusions
#define JSON_EXTRUSION_ENTITY_TYPE             "entity_type"
#define JSON_EXTRUSION_NO_SORT                 "no_sort"
#define JSON_EXTRUSION_PATHS                   "paths"
#define JSON_EXTRUSION_ENTITIES                "entities"
#define JSON_EXTRUSION_TYPE_PATH               "path"
#define JSON_EXTRUSION_TYPE_MULTIPATH          "multipath"
#define JSON_EXTRUSION_TYPE_LOOP               "loop"
#define JSON_EXTRUSION_TYPE_COLLECTION         "collection"
#define JSON_EXTRUSION_POLYLINE                "polyline"
#define JSON_EXTRUSION_MM3_PER_MM              "mm3_per_mm"
#define JSON_EXTRUSION_WIDTH                   "width"
#define JSON_EXTRUSION_HEIGHT                  "height"
#define JSON_EXTRUSION_ROLE                    "role"
#define JSON_EXTRUSION_NO_EXTRUSION            "no_extrusion"
#define JSON_EXTRUSION_LOOP_ROLE               "loop_role"


static void to_json(json& j, const Points& p_s) {
    for (const Point& p : p_s)
    {
        j.push_back(p.x());
        j.push_back(p.y());
    }
}

static void to_json(json& j, const BoundingBox& bb) {
    j.push_back(bb.min.x());
    j.push_back(bb.min.y());
    j.push_back(bb.max.x());
    j.push_back(bb.max.y());
}

static void to_json(json& j, const ExPolygon& polygon) {
    json contour_json = json::array(), holes_json = json::array();

    //contour
    const Polygon& slice_contour =   polygon.contour;
    contour_json = slice_contour.points;
    j[JSON_POLYGON_CONTOUR] = std::move(contour_json);

    //holes
    const Polygons& slice_holes =   polygon.holes;
    for (const Polygon& hole_polyon : slice_holes)
    {
        json hole_json = json::array();
        hole_json =  hole_polyon.points;
        holes_json.push_back(std::move(hole_json));
    }
    j[JSON_POLYGON_HOLES] = std::move(holes_json);
}

static void to_json(json& j, const Surface& surf) {
    j[JSON_EXPOLYGON] = surf.expolygon;
    j[JSON_SURF_TYPE] = surf.surface_type;
    j[JSON_SURF_THICKNESS] = surf.thickness;
    j[JSON_SURF_THICKNESS_LAYER] = surf.thickness_layers;
    j[JSON_SURF_BRIDGE_ANGLE] = surf.bridge_angle;
    j[JSON_SURF_EXTRA_PERIMETERS] = surf.extra_perimeters;
}

static void to_json(json& j, const ArcSegment& arc_seg) {
    json start_point_json = json::array(), end_point_json = json::array(), center_point_json = json::array();
    j[JSON_IS_ARC] = arc_seg.is_arc;
    j[JSON_ARC_LENGTH] = arc_seg.length;
    j[JSON_ARC_ANGLE_RADIUS] = arc_seg.angle_radians;
    j[JSON_ARC_POLAY_START_THETA] = arc_seg.polar_start_theta;
    j[JSON_ARC_POLAY_END_THETA] = arc_seg.polar_end_theta;
    start_point_json.push_back(arc_seg.start_point.x());
    start_point_json.push_back(arc_seg.start_point.y());
    j[JSON_ARC_START_POINT] = std::move(start_point_json);
    end_point_json.push_back(arc_seg.end_point.x());
    end_point_json.push_back(arc_seg.end_point.y());
    j[JSON_ARC_END_POINT] = std::move(end_point_json);
    j[JSON_ARC_DIRECTION] = arc_seg.direction;
    j[JSON_ARC_RADIUS] = arc_seg.radius;
    center_point_json.push_back(arc_seg.center.x());
    center_point_json.push_back(arc_seg.center.y());
    j[JSON_ARC_CENTER] = std::move(center_point_json);
}


static void to_json(json& j, const Polyline& poly_line) {
    json points_json = json::array(), fittings_json = json::array();
    points_json = poly_line.points;

    j[JSON_POINTS] = std::move(points_json);
    for (const PathFittingData& path_fitting : poly_line.fitting_result)
    {
        json fitting_json;
        fitting_json[JSON_ARC_START_INDEX] = path_fitting.start_point_index;
        fitting_json[JSON_ARC_END_INDEX] = path_fitting.end_point_index;
        fitting_json[JSON_ARC_PATH_TYPE] = path_fitting.path_type;
        if (path_fitting.arc_data.is_arc)
            fitting_json[JSON_ARC_DATA] = path_fitting.arc_data;

        fittings_json.push_back(std::move(fitting_json));
    }
    j[JSON_ARC_FITTING] = fittings_json;
}

static void to_json(json& j, const ExtrusionPath& extrusion_path) {
    j[JSON_EXTRUSION_POLYLINE] = extrusion_path.polyline;
    j[JSON_EXTRUSION_MM3_PER_MM] = extrusion_path.mm3_per_mm;
    j[JSON_EXTRUSION_WIDTH] = extrusion_path.width;
    j[JSON_EXTRUSION_HEIGHT] = extrusion_path.height;
    j[JSON_EXTRUSION_ROLE] = extrusion_path.role();
    j[JSON_EXTRUSION_NO_EXTRUSION] = extrusion_path.is_force_no_extrusion();
}

static bool convert_extrusion_to_json(json& entity_json, json& entity_paths_json, const ExtrusionEntity* extrusion_entity) {
    std::string path_type;
    const ExtrusionPath* path = NULL;
    const ExtrusionMultiPath* multipath = NULL;
    const ExtrusionLoop* loop = NULL;
    const ExtrusionEntityCollection* collection = dynamic_cast<const ExtrusionEntityCollection*>(extrusion_entity);

    if (!collection)
        path = dynamic_cast<const ExtrusionPath*>(extrusion_entity);

    if (!collection && !path)
        multipath = dynamic_cast<const ExtrusionMultiPath*>(extrusion_entity);

    if (!collection && !path && !multipath)
        loop = dynamic_cast<const ExtrusionLoop*>(extrusion_entity);

    path_type = path?JSON_EXTRUSION_TYPE_PATH:(multipath?JSON_EXTRUSION_TYPE_MULTIPATH:(loop?JSON_EXTRUSION_TYPE_LOOP:JSON_EXTRUSION_TYPE_COLLECTION));
    if (path_type.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":invalid extrusion path type Found");
        return false;
    }

    entity_json[JSON_EXTRUSION_ENTITY_TYPE] = path_type;

    if (path) {
        json entity_path_json = *path;
        entity_paths_json.push_back(std::move(entity_path_json));
    }
    else if (multipath) {
        for (const ExtrusionPath& extrusion_path : multipath->paths)
        {
            json entity_path_json = extrusion_path;
            entity_paths_json.push_back(std::move(entity_path_json));
        }
    }
    else if (loop) {
        entity_json[JSON_EXTRUSION_LOOP_ROLE] = loop->loop_role();
        for (const ExtrusionPath& extrusion_path : loop->paths)
        {
            json entity_path_json = extrusion_path;
            entity_paths_json.push_back(std::move(entity_path_json));
        }
    }
    else {
        //recursive collections
        entity_json[JSON_EXTRUSION_NO_SORT] = collection->no_sort;
        for (const ExtrusionEntity* recursive_extrusion_entity : collection->entities) {
            json recursive_entity_json, recursive_entity_paths_json = json::array();
            bool ret = convert_extrusion_to_json(recursive_entity_json, recursive_entity_paths_json, recursive_extrusion_entity);
            if (!ret) {
                continue;
            }
            entity_paths_json.push_back(std::move(recursive_entity_json));
        }
    }

    if (collection)
        entity_json[JSON_EXTRUSION_ENTITIES] = std::move(entity_paths_json);
    else
        entity_json[JSON_EXTRUSION_PATHS] = std::move(entity_paths_json);
    return true;
}

static void to_json(json& j, const LayerRegion& layer_region) {
    json unsupported_bridge_edges_json = json::array(), slices_surfaces_json = json::array(), raw_slices_json = json::array(), thin_fills_json, thin_fill_entities_json = json::array();
    json fill_expolygons_json = json::array(), fill_no_overlap_expolygons_json = json::array(), fill_surfaces_json = json::array(), perimeters_json, perimeter_entities_json = json::array(), fills_json, fill_entities_json = json::array();

    j[JSON_LAYER_REGION_CONFIG_HASH] = layer_region.region().config_hash();
    //slices
    for (const Surface& slice_surface : layer_region.slices.surfaces) {
        json surface_json = slice_surface;
        slices_surfaces_json.push_back(std::move(surface_json));
    }
    j.push_back({JSON_LAYER_REGION_SLICES, std::move(slices_surfaces_json)});

    //raw_slices
    for (const ExPolygon& raw_slice_explogyon : layer_region.raw_slices) {
        json raw_polygon_json = raw_slice_explogyon;

        raw_slices_json.push_back(std::move(raw_polygon_json));
    }
    j.push_back({JSON_LAYER_REGION_RAW_SLICES, std::move(raw_slices_json)});

    //thin fills
    thin_fills_json[JSON_EXTRUSION_NO_SORT] = layer_region.thin_fills.no_sort;
    thin_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.thin_fills.entities) {
        json thinfills_entity_json, thinfill_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(thinfills_entity_json, thinfill_entity_paths_json, extrusion_entity);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":error found at print_z %1%") % layer_region.layer()->print_z;
            continue;
        }

        thin_fill_entities_json.push_back(std::move(thinfills_entity_json));
    }
    thin_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(thin_fill_entities_json);
    j.push_back({JSON_LAYER_REGION_THIN_FILLS, std::move(thin_fills_json)});

    //fill_expolygons
    for (const ExPolygon& fill_expolygon : layer_region.fill_expolygons) {
        json fill_expolygon_json = fill_expolygon;

        fill_expolygons_json.push_back(std::move(fill_expolygon_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_EXPOLYGONS, std::move(fill_expolygons_json)});

    //fill_surfaces
    for (const Surface& fill_surface : layer_region.fill_surfaces.surfaces) {
        json surface_json = fill_surface;
        fill_surfaces_json.push_back(std::move(surface_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_SURFACES, std::move(fill_surfaces_json)});

    //fill_no_overlap_expolygons
    for (const ExPolygon& fill_no_overlap_expolygon : layer_region.fill_no_overlap_expolygons) {
        json fill_no_overlap_expolygon_json = fill_no_overlap_expolygon;

        fill_no_overlap_expolygons_json.push_back(std::move(fill_no_overlap_expolygon_json));
    }
    j.push_back({JSON_LAYER_REGION_FILL_NO_OVERLAP, std::move(fill_no_overlap_expolygons_json)});

    //unsupported_bridge_edges
    for (const Polyline& poly_line : layer_region.unsupported_bridge_edges)
    {
        json polyline_json = poly_line;

        unsupported_bridge_edges_json.push_back(std::move(polyline_json));
    }
    j.push_back({JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES, std::move(unsupported_bridge_edges_json)});

    //perimeters
    perimeters_json[JSON_EXTRUSION_NO_SORT] = layer_region.perimeters.no_sort;
    perimeters_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.perimeters.entities) {
        json perimeters_entity_json, perimeters_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(perimeters_entity_json, perimeters_entity_paths_json, extrusion_entity);
        if (!ret)
            continue;

        perimeter_entities_json.push_back(std::move(perimeters_entity_json));
    }
    perimeters_json[JSON_EXTRUSION_ENTITIES] = std::move(perimeter_entities_json);
    j.push_back({JSON_LAYER_REGION_PERIMETERS, std::move(perimeters_json)});

    //fills
    fills_json[JSON_EXTRUSION_NO_SORT] = layer_region.fills.no_sort;
    fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
    for (const ExtrusionEntity* extrusion_entity : layer_region.fills.entities) {
        json fill_entity_json, fill_entity_paths_json = json::array();
        bool ret = convert_extrusion_to_json(fill_entity_json, fill_entity_paths_json, extrusion_entity);
        if (!ret)
            continue;

        fill_entities_json.push_back(std::move(fill_entity_json));
    }
    fills_json[JSON_EXTRUSION_ENTITIES] = std::move(fill_entities_json);
    j.push_back({JSON_LAYER_REGION_FILLS, std::move(fills_json)});

    return;
}

static void to_json(json& j, const groupedVolumeSlices& first_layer_group) {
    json volumes_json = json::array(), slices_json = json::array();
    j[JSON_FIRSTLAYER_GROUP_ID] = first_layer_group.groupId;

    for (const ObjectID& obj_id : first_layer_group.volume_ids)
    {
        volumes_json.push_back(obj_id.id);
    }
    j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS] = std::move(volumes_json);

    for (const ExPolygon& slice_expolygon : first_layer_group.slices) {
        json slice_expolygon_json = slice_expolygon;

        slices_json.push_back(std::move(slice_expolygon_json));
    }
    j[JSON_FIRSTLAYER_GROUP_SLICES] = std::move(slices_json);
}

//load apis from json
static void from_json(const json& j, Points& p_s) {
    int array_size = j.size();
    for (int index = 0; index < array_size/2; index++)
    {
        coord_t x = j[2*index], y = j[2*index+1];
        Point p(x, y);
        p_s.push_back(std::move(p));
    }
    return;
}

static void from_json(const json& j, BoundingBox& bbox) {
    bbox.min[0] = j[0];
    bbox.min[1] = j[1];
    bbox.max[0] = j[2];
    bbox.max[1] = j[3];
    bbox.defined = true;

    return;
}

static void from_json(const json& j, ExPolygon& polygon) {
    polygon.contour.points = j[JSON_POLYGON_CONTOUR];

    int holes_count = j[JSON_POLYGON_HOLES].size();
    for (int holes_index = 0; holes_index < holes_count; holes_index++)
    {
        Polygon poly;

        poly.points = j[JSON_POLYGON_HOLES][holes_index];
        polygon.holes.push_back(std::move(poly));
    }
    return;
}

static void from_json(const json& j, Surface& surf) {
    surf.expolygon = j[JSON_EXPOLYGON];
    surf.surface_type = j[JSON_SURF_TYPE];
    surf.thickness = j[JSON_SURF_THICKNESS];
    surf.thickness_layers = j[JSON_SURF_THICKNESS_LAYER];
    surf.bridge_angle = j[JSON_SURF_BRIDGE_ANGLE];
    surf.extra_perimeters = j[JSON_SURF_EXTRA_PERIMETERS];

    return;
}

static void from_json(const json& j, ArcSegment& arc_seg) {
    arc_seg.is_arc = j[JSON_IS_ARC];
    arc_seg.length = j[JSON_ARC_LENGTH];
    arc_seg.angle_radians = j[JSON_ARC_ANGLE_RADIUS];
    arc_seg.polar_start_theta = j[JSON_ARC_POLAY_START_THETA];
    arc_seg.polar_end_theta = j[JSON_ARC_POLAY_END_THETA];
    arc_seg.start_point.x() = j[JSON_ARC_START_POINT][0];
    arc_seg.start_point.y() = j[JSON_ARC_START_POINT][1];
    arc_seg.end_point.x() = j[JSON_ARC_END_POINT][0];
    arc_seg.end_point.y() = j[JSON_ARC_END_POINT][1];
    arc_seg.direction = j[JSON_ARC_DIRECTION];
    arc_seg.radius    = j[JSON_ARC_RADIUS];
    arc_seg.center.x() = j[JSON_ARC_CENTER][0];
    arc_seg.center.y() = j[JSON_ARC_CENTER][1];

    return;
}


static void from_json(const json& j, Polyline& poly_line) {
    poly_line.points = j[JSON_POINTS];

    int arc_fitting_count = j[JSON_ARC_FITTING].size();
    for (int arc_fitting_index = 0; arc_fitting_index < arc_fitting_count; arc_fitting_index++)
    {
        const json& fitting_json = j[JSON_ARC_FITTING][arc_fitting_index];
        PathFittingData path_fitting;
        path_fitting.start_point_index = fitting_json[JSON_ARC_START_INDEX];
        path_fitting.end_point_index = fitting_json[JSON_ARC_END_INDEX];
        path_fitting.path_type = fitting_json[JSON_ARC_PATH_TYPE];

        if (fitting_json.contains(JSON_ARC_DATA)) {
            path_fitting.arc_data = fitting_json[JSON_ARC_DATA];
        }

        poly_line.fitting_result.push_back(std::move(path_fitting));
    }
    return;
}

static void from_json(const json& j, ExtrusionPath& extrusion_path) {
    extrusion_path.polyline               =    j[JSON_EXTRUSION_POLYLINE];
    extrusion_path.mm3_per_mm             =    j[JSON_EXTRUSION_MM3_PER_MM];
    extrusion_path.width                  =    j[JSON_EXTRUSION_WIDTH];
    extrusion_path.height                 =    j[JSON_EXTRUSION_HEIGHT];
    extrusion_path.set_extrusion_role(j[JSON_EXTRUSION_ROLE]);
    extrusion_path.set_force_no_extrusion(j[JSON_EXTRUSION_NO_EXTRUSION]);
}

static bool convert_extrusion_from_json(const json& entity_json, ExtrusionEntityCollection& entity_collection) {
    std::string path_type = entity_json[JSON_EXTRUSION_ENTITY_TYPE];
    bool ret = false;

    if (path_type == JSON_EXTRUSION_TYPE_PATH) {
        ExtrusionPath* path = new ExtrusionPath();
        if (!path) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionPath");
            return false;
        }
        *path = entity_json[JSON_EXTRUSION_PATHS][0];
        entity_collection.entities.push_back(path);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_MULTIPATH) {
        ExtrusionMultiPath* multipath = new ExtrusionMultiPath();
        if (!multipath) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionMultiPath");
            return false;
        }
        int paths_count = entity_json[JSON_EXTRUSION_PATHS].size();
        for (int path_index = 0; path_index < paths_count; path_index++)
        {
            ExtrusionPath path;
            path = entity_json[JSON_EXTRUSION_PATHS][path_index];
            multipath->paths.push_back(std::move(path));
        }
        entity_collection.entities.push_back(multipath);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_LOOP) {
        ExtrusionLoop* loop = new ExtrusionLoop();
        if (!loop) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionLoop");
            return false;
        }
        loop->set_loop_role(entity_json[JSON_EXTRUSION_LOOP_ROLE]);
        int paths_count = entity_json[JSON_EXTRUSION_PATHS].size();
        for (int path_index = 0; path_index < paths_count; path_index++)
        {
            ExtrusionPath path;
            path = entity_json[JSON_EXTRUSION_PATHS][path_index];
            loop->paths.push_back(std::move(path));
        }
        entity_collection.entities.push_back(loop);
    }
    else if (path_type == JSON_EXTRUSION_TYPE_COLLECTION) {
        ExtrusionEntityCollection* collection = new ExtrusionEntityCollection();
        if (!collection) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": oom when new ExtrusionEntityCollection");
            return false;
        }
        collection->no_sort = entity_json[JSON_EXTRUSION_NO_SORT];
        int entities_count = entity_json[JSON_EXTRUSION_ENTITIES].size();
        for (int entity_index = 0; entity_index < entities_count; entity_index++)
        {
            const json& entity_item_json = entity_json[JSON_EXTRUSION_ENTITIES][entity_index];
            ret = convert_extrusion_from_json(entity_item_json, *collection);
            if (!ret) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": convert_extrusion_from_json failed");
                return false;
            }
        }
        entity_collection.entities.push_back(collection);
    }
    else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": unknown path type %1%")%path_type;
        return false;
    }

    return true;
}

static void convert_layer_region_from_json(const json& j, LayerRegion& layer_region) {
    //slices
    int slices_count = j[JSON_LAYER_REGION_SLICES].size();
    for (int slices_index = 0; slices_index < slices_count; slices_index++)
    {
        Surface surface;

        surface = j[JSON_LAYER_REGION_SLICES][slices_index];
        layer_region.slices.surfaces.push_back(std::move(surface));
    }

    //raw_slices
    int raw_slices_count = j[JSON_LAYER_REGION_RAW_SLICES].size();
    for (int raw_slices_index = 0; raw_slices_index < raw_slices_count; raw_slices_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_RAW_SLICES][raw_slices_index];
        layer_region.raw_slices.push_back(std::move(polygon));
    }

    //thin fills
    layer_region.thin_fills.no_sort = j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_NO_SORT];
    int thinfills_entities_count = j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int thinfills_entities_index = 0; thinfills_entities_index < thinfills_entities_count; thinfills_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_THIN_FILLS][JSON_EXTRUSION_ENTITIES][thinfills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.thin_fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(":error parsing thin_fills found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing thin_fills at layer %zd, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    //fill_expolygons
    int fill_expolygons_count = j[JSON_LAYER_REGION_FILL_EXPOLYGONS].size();
    for (int fill_expolygons_index = 0; fill_expolygons_index < fill_expolygons_count; fill_expolygons_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_FILL_EXPOLYGONS][fill_expolygons_index];
        layer_region.fill_expolygons.push_back(std::move(polygon));
    }

    //fill_surfaces
    int fill_surfaces_count = j[JSON_LAYER_REGION_FILL_SURFACES].size();
    for (int fill_surfaces_index = 0; fill_surfaces_index < fill_surfaces_count; fill_surfaces_index++)
    {
        Surface surface;

        surface = j[JSON_LAYER_REGION_FILL_SURFACES][fill_surfaces_index];
        layer_region.fill_surfaces.surfaces.push_back(std::move(surface));
    }

    //fill_no_overlap_expolygons
    int fill_no_overlap_expolygons_count = j[JSON_LAYER_REGION_FILL_NO_OVERLAP].size();
    for (int fill_no_overlap_expolygons_index = 0; fill_no_overlap_expolygons_index < fill_no_overlap_expolygons_count; fill_no_overlap_expolygons_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_LAYER_REGION_FILL_NO_OVERLAP][fill_no_overlap_expolygons_index];
        layer_region.fill_no_overlap_expolygons.push_back(std::move(polygon));
    }

    //unsupported_bridge_edges
    int unsupported_bridge_edges_count = j[JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES].size();
    for (int unsupported_bridge_edges_index = 0; unsupported_bridge_edges_index < unsupported_bridge_edges_count; unsupported_bridge_edges_index++)
    {
        Polyline polyline;

        polyline = j[JSON_LAYER_REGION_UNSUPPORTED_BRIDGE_EDGES][unsupported_bridge_edges_index];
        layer_region.unsupported_bridge_edges.push_back(std::move(polyline));
    }

    //perimeters
    layer_region.perimeters.no_sort = j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_NO_SORT];
    int perimeters_entities_count = j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_ENTITIES].size();
    for (int perimeters_entities_index = 0; perimeters_entities_index < perimeters_entities_count; perimeters_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_PERIMETERS][JSON_EXTRUSION_ENTITIES][perimeters_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.perimeters);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing perimeters found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing perimeters at layer %zd, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    //fills
    layer_region.fills.no_sort = j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_NO_SORT];
    int fills_entities_count = j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int fills_entities_index = 0; fills_entities_index < fills_entities_count; fills_entities_index++)
    {
        const json& extrusion_entity_json =  j[JSON_LAYER_REGION_FILLS][JSON_EXTRUSION_ENTITIES][fills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, layer_region.fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing fills found at layer %1%, print_z %2%") %layer_region.layer()->id() %layer_region.layer()->print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing fills at layer %zd, print_z %f", layer_region.layer()->id(), layer_region.layer()->print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    return;
}


void extract_layer(const json& layer_json, Layer& layer) {
    //slice_polygons
    int slice_polygons_count = layer_json[JSON_LAYER_SLICED_POLYGONS].size();
    for (int polygon_index = 0; polygon_index < slice_polygons_count; polygon_index++)
    {
        ExPolygon polygon;

        polygon = layer_json[JSON_LAYER_SLICED_POLYGONS][polygon_index];
        layer.lslices.push_back(std::move(polygon));
    }

    //slice_bboxes
    int sliced_bboxes_count = layer_json[JSON_LAYER_SLLICED_BBOXES].size();
    for (int bbox_index = 0; bbox_index < sliced_bboxes_count; bbox_index++)
    {
        BoundingBox bbox;

        bbox = layer_json[JSON_LAYER_SLLICED_BBOXES][bbox_index];
        layer.lslices_bboxes.push_back(std::move(bbox));
    }

    //overhang_polygons
    int overhang_polygons_count = layer_json[JSON_LAYER_OVERHANG_POLYGONS].size();
    for (int polygon_index = 0; polygon_index < overhang_polygons_count; polygon_index++)
    {
        ExPolygon polygon;

        polygon = layer_json[JSON_LAYER_OVERHANG_POLYGONS][polygon_index];
        layer.loverhangs.push_back(std::move(polygon));
    }

    //overhang_box
    layer.loverhangs_bbox = layer_json[JSON_LAYER_OVERHANG_BBOX];

    //layer_regions
    int layer_region_count = layer.region_count();
    for (int layer_region_index = 0; layer_region_index < layer_region_count; layer_region_index++)
    {
        LayerRegion* layer_region = layer.get_region(layer_region_index);
        const json& layer_region_json = layer_json[JSON_LAYER_REGIONS][layer_region_index];
        convert_layer_region_from_json(layer_region_json, *layer_region);

        //LayerRegion layer_region = layer_json[JSON_LAYER_REGIONS][layer_region_index];
    }

    return;
}

void extract_support_layer(const json& support_layer_json, SupportLayer& support_layer) {
    extract_layer(support_layer_json, support_layer);

    support_layer.support_type = support_layer_json[JSON_SUPPORT_LAYER_TYPE];
    //support_islands
    int islands_count = support_layer_json[JSON_SUPPORT_LAYER_ISLANDS].size();
    for (int islands_index = 0; islands_index < islands_count; islands_index++)
    {
        ExPolygon polygon;

        polygon = support_layer_json[JSON_SUPPORT_LAYER_ISLANDS][islands_index];
        support_layer.support_islands.push_back(std::move(polygon));
    }

    //support_fills
    support_layer.support_fills.no_sort = support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_NO_SORT];
    int support_fills_entities_count = support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_ENTITIES].size();
    for (int support_fills_entities_index = 0; support_fills_entities_index < support_fills_entities_count; support_fills_entities_index++)
    {
        const json& extrusion_entity_json =  support_layer_json[JSON_SUPPORT_LAYER_FILLS][JSON_EXTRUSION_ENTITIES][support_fills_entities_index];
        bool ret = convert_extrusion_from_json(extrusion_entity_json, support_layer.support_fills);
        if (!ret) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error parsing fills found at support_layer %1%, print_z %2%")%support_layer.id() %support_layer.print_z;
            char error_buf[1024];
            ::sprintf(error_buf, "Error while parsing fills at support_layer %zd, print_z %f", support_layer.id(), support_layer.print_z);
            throw Slic3r::FileIOError(error_buf);
        }
    }

    return;
}

static void from_json(const json& j, groupedVolumeSlices& firstlayer_group)
{
    firstlayer_group.groupId               =   j[JSON_FIRSTLAYER_GROUP_ID];

    int volume_count = j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS].size();
    for (int volume_index = 0; volume_index < volume_count; volume_index++)
    {
        ObjectID obj_id;

        obj_id.id = j[JSON_FIRSTLAYER_GROUP_VOLUME_IDS][volume_index];
        firstlayer_group.volume_ids.push_back(std::move(obj_id));
    }

    int slices_count = j[JSON_FIRSTLAYER_GROUP_SLICES].size();
    for (int slice_index = 0; slice_index < slices_count; slice_index++)
    {
        ExPolygon polygon;

        polygon = j[JSON_FIRSTLAYER_GROUP_SLICES][slice_index];
        firstlayer_group.slices.push_back(std::move(polygon));
    }
}

int Print::export_cached_data(const std::string& directory, bool with_space)
{
    int ret = 0;
    boost::filesystem::path directory_path(directory);

    auto convert_layer_to_json = [](json& layer_json, const Layer* layer) {
        json slice_polygons_json = json::array(), slice_bboxs_json = json::array(), overhang_polygons_json = json::array(), layer_regions_json = json::array();
        layer_json[JSON_LAYER_PRINT_Z] = layer->print_z;
        layer_json[JSON_LAYER_HEIGHT] = layer->height;
        layer_json[JSON_LAYER_SLICE_Z] = layer->slice_z;
        layer_json[JSON_LAYER_ID] = layer->id();
        //layer_json["slicing_errors"] = layer->slicing_errors;

        //sliced_polygons
        for (const ExPolygon& slice_polygon : layer->lslices) {
            json slice_polygon_json = slice_polygon;
            slice_polygons_json.push_back(std::move(slice_polygon_json));
        }
        layer_json[JSON_LAYER_SLICED_POLYGONS] = std::move(slice_polygons_json);

        //sliced_bbox
        for (const BoundingBox& slice_bbox : layer->lslices_bboxes) {
            json bbox_json = json::array();

            bbox_json = slice_bbox;
            slice_bboxs_json.push_back(std::move(bbox_json));
        }
        layer_json[JSON_LAYER_SLLICED_BBOXES] = std::move(slice_bboxs_json);

        //overhang_polygons
        for (const ExPolygon& overhang_polygon : layer->loverhangs) {
            json overhang_polygon_json = overhang_polygon;
            overhang_polygons_json.push_back(std::move(overhang_polygon_json));
        }
        layer_json[JSON_LAYER_OVERHANG_POLYGONS] = std::move(overhang_polygons_json);

        //overhang_box
        layer_json[JSON_LAYER_OVERHANG_BBOX] = layer->loverhangs_bbox;

        for (const LayerRegion *layer_region : layer->regions()) {
            json region_json = *layer_region;

            layer_regions_json.push_back(std::move(region_json));
        }
        layer_json[JSON_LAYER_REGIONS] = std::move(layer_regions_json);

        return;
    };

    //firstly clear this directory
    if (fs::exists(directory_path)) {
        fs::remove_all(directory_path);
    }
    try {
        if (!fs::create_directory(directory_path)) {
            BOOST_LOG_TRIVIAL(error) << boost::format("create directory %1% failed")%directory;
            return CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED;
        }
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << boost::format("create directory %1% failed")%directory;
        return CLI_EXPORT_CACHE_DIRECTORY_CREATE_FAILED;
    }

    int count = 0;
    std::vector<std::string> filename_vector;
    std::vector<json> json_vector;
    for (PrintObject *obj : m_objects) {
        const ModelObject* model_obj = obj->model_object();
        if (obj->get_shared_object()) {
            BOOST_LOG_TRIVIAL(info) << boost::format("shared object %1%, skip directly")%model_obj->name;
            continue;
        }

        const PrintInstance &print_instance = obj->instances()[0];
        const ModelInstance *model_instance = print_instance.model_instance;
        size_t identify_id = (model_instance->loaded_id > 0)?model_instance->loaded_id: model_instance->id().id;
        std::string file_name = directory +"/obj_"+std::to_string(identify_id)+".json";

        BOOST_LOG_TRIVIAL(info) << boost::format("begin to dump object %1%, identify_id %2% to %3%")%model_obj->name %identify_id %file_name;

        try {
            json root_json, layers_json = json::array(), support_layers_json = json::array(), first_layer_groups = json::array();

            root_json[JSON_OBJECT_NAME] = model_obj->name;
            root_json[JSON_IDENTIFY_ID] = identify_id;

            //export the layers
            std::vector<json> layers_json_vector(obj->layer_count());
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->layer_count()),
                [&layers_json_vector, obj, convert_layer_to_json](const tbb::blocked_range<size_t>& layer_range) {
                    for (size_t layer_index = layer_range.begin(); layer_index < layer_range.end(); ++ layer_index) {
                        const Layer *layer = obj->get_layer(layer_index);
                        json layer_json;
                        convert_layer_to_json(layer_json, layer);
                        layers_json_vector[layer_index] = std::move(layer_json);
                    }
                }
            );
            for (int l_index = 0; l_index < layers_json_vector.size(); l_index++) {
                layers_json.push_back(std::move(layers_json_vector[l_index]));
            }
            layers_json_vector.clear();
            /*for (const Layer *layer : obj->layers()) {
                // for each layer
                json layer_json;

                convert_layer_to_json(layer_json, layer);

                layers_json.push_back(std::move(layer_json));
            }*/

            root_json[JSON_LAYERS] = std::move(layers_json);

            //export the support layers
            std::vector<json> support_layers_json_vector(obj->support_layer_count());
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->support_layer_count()),
                [&support_layers_json_vector, obj, convert_layer_to_json](const tbb::blocked_range<size_t>& support_layer_range) {
                    for (size_t s_layer_index = support_layer_range.begin(); s_layer_index < support_layer_range.end(); ++ s_layer_index) {
                        const SupportLayer *support_layer = obj->get_support_layer(s_layer_index);
                        json support_layer_json, support_islands_json = json::array(), support_fills_json, supportfills_entities_json = json::array();

                        convert_layer_to_json(support_layer_json, support_layer);

                        support_layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID] = support_layer->interface_id();
                        support_layer_json[JSON_SUPPORT_LAYER_TYPE] = support_layer->support_type;

                        //support_islands
                        for (const ExPolygon& support_island : support_layer->support_islands) {
                            json support_island_json = support_island;
                            support_islands_json.push_back(std::move(support_island_json));
                        }
                        support_layer_json[JSON_SUPPORT_LAYER_ISLANDS] = std::move(support_islands_json);

                        //support_fills
                        support_fills_json[JSON_EXTRUSION_NO_SORT] = support_layer->support_fills.no_sort;
                        support_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
                        for (const ExtrusionEntity* extrusion_entity : support_layer->support_fills.entities) {
                            json supportfill_entity_json, supportfill_entity_paths_json = json::array();
                            bool ret = convert_extrusion_to_json(supportfill_entity_json, supportfill_entity_paths_json, extrusion_entity);
                            if (!ret)
                                continue;

                            supportfills_entities_json.push_back(std::move(supportfill_entity_json));
                        }
                        support_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(supportfills_entities_json);
                        support_layer_json[JSON_SUPPORT_LAYER_FILLS] = std::move(support_fills_json);

                        support_layers_json_vector[s_layer_index] = std::move(support_layer_json);
                    }
                }
            );
            for (int s_index = 0; s_index < support_layers_json_vector.size(); s_index++) {
                support_layers_json.push_back(std::move(support_layers_json_vector[s_index]));
            }
            support_layers_json_vector.clear();

            /*for (const SupportLayer *support_layer : obj->support_layers()) {
                json support_layer_json, support_islands_json = json::array(), support_fills_json, supportfills_entities_json = json::array();

                convert_layer_to_json(support_layer_json, support_layer);

                support_layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID] = support_layer->interface_id();

                //support_islands
                for (const ExPolygon& support_island : support_layer->support_islands.expolygons) {
                    json support_island_json = support_island;
                    support_islands_json.push_back(std::move(support_island_json));
                }
                support_layer_json[JSON_SUPPORT_LAYER_ISLANDS] = std::move(support_islands_json);

                //support_fills
                support_fills_json[JSON_EXTRUSION_NO_SORT] = support_layer->support_fills.no_sort;
                support_fills_json[JSON_EXTRUSION_ENTITY_TYPE] = JSON_EXTRUSION_TYPE_COLLECTION;
                for (const ExtrusionEntity* extrusion_entity : support_layer->support_fills.entities) {
                    json supportfill_entity_json, supportfill_entity_paths_json = json::array();
                    bool ret = convert_extrusion_to_json(supportfill_entity_json, supportfill_entity_paths_json, extrusion_entity);
                    if (!ret)
                        continue;

                    supportfills_entities_json.push_back(std::move(supportfill_entity_json));
                }
                support_fills_json[JSON_EXTRUSION_ENTITIES] = std::move(supportfills_entities_json);
                support_layer_json[JSON_SUPPORT_LAYER_FILLS] = std::move(support_fills_json);

                support_layers_json.push_back(std::move(support_layer_json));
            } // for each layer*/
            root_json[JSON_SUPPORT_LAYERS] = std::move(support_layers_json);

            const std::vector<groupedVolumeSlices> &first_layer_obj_groups =  obj->firstLayerObjGroups();
            for (size_t s_group_index = 0; s_group_index < first_layer_obj_groups.size(); ++ s_group_index) {
                groupedVolumeSlices group = first_layer_obj_groups[s_group_index];

                //convert the id
                for (ObjectID& obj_id : group.volume_ids)
                {
                    const ModelVolume* currentModelVolumePtr = nullptr;
                    //BBS: support shared object logic
                    const PrintObject* shared_object = obj->get_shared_object();
                    if (!shared_object)
                        shared_object = obj;
                    const ModelVolumePtrs& volumes_ptr = shared_object->model_object()->volumes;
                    size_t volume_count = volumes_ptr.size();
                    for (size_t index = 0; index < volume_count; index ++) {
                        currentModelVolumePtr = volumes_ptr[index];
                        if (currentModelVolumePtr->id() == obj_id) {
                            obj_id.id = index;
                            break;
                        }
                    }
                }

                json first_layer_group_json;

                first_layer_group_json = group;
                first_layer_groups.push_back(std::move(first_layer_group_json));
            }
            root_json[JSON_FIRSTLAYER_GROUPS] = std::move(first_layer_groups);

            filename_vector.push_back(file_name);
            json_vector.push_back(std::move(root_json));
            /*boost::nowide::ofstream c;
            c.open(file_name, std::ios::out | std::ios::trunc);
            if (with_space)
                c << std::setw(4) << root_json << std::endl;
            else
                c << root_json.dump(0) << std::endl;
            c.close();*/
            count ++;
            BOOST_LOG_TRIVIAL(info) << boost::format("will dump object %1%'s json to %2%.")%model_obj->name%file_name;
        }
        catch(std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": save to "<<file_name<<" got a generic exception, reason = " << err.what();
            ret = CLI_EXPORT_CACHE_WRITE_FAILED;
        }
    }

    boost::mutex mutex;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, filename_vector.size()),
        [filename_vector, &json_vector, with_space, &ret, &mutex](const tbb::blocked_range<size_t>& output_range) {
            for (size_t object_index = output_range.begin(); object_index < output_range.end(); ++ object_index) {
                try {
                    boost::nowide::ofstream c;
                    c.open(filename_vector[object_index], std::ios::out | std::ios::trunc);
                    if (with_space)
                        c << std::setw(4) << json_vector[object_index] << std::endl;
                    else
                        c << json_vector[object_index].dump(0) << std::endl;
                    c.close();
                }
                catch(std::exception &err) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": save to "<<filename_vector[object_index]<<" got a generic exception, reason = " << err.what();
                    boost::unique_lock l(mutex);
                    ret = CLI_EXPORT_CACHE_WRITE_FAILED;
                }
            }
        }
    );

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": total printobject count %1%, saved %2%, ret=%3%")%m_objects.size() %count %ret;
    return ret;
}


int Print::load_cached_data(const std::string& directory)
{
    int ret = 0;
    boost::filesystem::path directory_path(directory);

    if (!fs::exists(directory_path)) {
        BOOST_LOG_TRIVIAL(info) << boost::format("directory %1% not exist.")%directory;
        return CLI_IMPORT_CACHE_NOT_FOUND;
    }

    auto find_region = [this](PrintObject* object, size_t config_hash) -> const PrintRegion* {
        int regions_count = object->num_printing_regions();
        for (int index = 0; index < regions_count; index++ )
        {
            const PrintRegion&  print_region = object->printing_region(index);
            if (print_region.config_hash() == config_hash ) {
                return &print_region;
            }
        }
        return NULL;
    };

    int count = 0;
    std::vector<std::pair<std::string, PrintObject*>> object_filenames;
    for (PrintObject *obj : m_objects) {
        const ModelObject* model_obj = obj->model_object();
        const PrintInstance &print_instance = obj->instances()[0];
        const ModelInstance *model_instance = print_instance.model_instance;

        obj->clear_layers();
        obj->clear_support_layers();

        int identify_id = model_instance->loaded_id;
        if (identify_id <= 0) {
            //for old 3mf
            identify_id = model_instance->id().id;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": object %1%'s loaded_id is 0, need to use the instance_id %2%")%model_obj->name %identify_id;
            //continue;
        }
        std::string file_name = directory +"/obj_"+std::to_string(identify_id)+".json";

        if (!fs::exists(file_name)) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": file %1% not exist, maybe a shared object, skip it")%file_name;
            continue;
        }
        object_filenames.push_back({file_name, obj});
    }

    boost::mutex mutex;
    std::vector<json> object_jsons(object_filenames.size());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, object_filenames.size()),
        [object_filenames, &ret, &object_jsons, &mutex](const tbb::blocked_range<size_t>& filename_range) {
            for (size_t filename_index = filename_range.begin(); filename_index < filename_range.end(); ++ filename_index) {
                try {
                    json root_json;
                    boost::nowide::ifstream ifs(object_filenames[filename_index].first);
                    ifs >> root_json;
                    object_jsons[filename_index] = std::move(root_json);
                }
                catch(std::exception &err) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": load from "<<object_filenames[filename_index].first<<" got a generic exception, reason = " << err.what();
                    boost::unique_lock l(mutex);
                    ret = CLI_IMPORT_CACHE_LOAD_FAILED;
                }
            }
        }
    );

    if (ret) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": load json failed.");
        return ret;
    }

    for (int obj_index = 0; obj_index < object_jsons.size(); obj_index++) {
        json& root_json = object_jsons[obj_index];
        PrintObject *obj = object_filenames[obj_index].second;

        try {
            //boost::nowide::ifstream ifs(file_name);
            //ifs >> root_json;

            std::string name = root_json.at(JSON_OBJECT_NAME);
            int identify_id = root_json.at(JSON_IDENTIFY_ID);
            int layer_count = 0, support_layer_count = 0, firstlayer_group_count = 0;

            layer_count = root_json[JSON_LAYERS].size();
            support_layer_count = root_json[JSON_SUPPORT_LAYERS].size();
            firstlayer_group_count = root_json[JSON_FIRSTLAYER_GROUPS].size();

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(":will load %1%, identify_id %2%, layer_count %3%, support_layer_count %4%, firstlayer_group_count %5%")
                %name %identify_id %layer_count %support_layer_count %firstlayer_group_count;

            Layer* previous_layer = NULL;
            //create layer and layer regions
            for (int index = 0; index < layer_count; index++)
            {
                json& layer_json = root_json[JSON_LAYERS][index];
                Layer* new_layer = obj->add_layer(layer_json[JSON_LAYER_ID], layer_json[JSON_LAYER_HEIGHT], layer_json[JSON_LAYER_PRINT_Z], layer_json[JSON_LAYER_SLICE_Z]);
                if (!new_layer) {
                    BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":create_layer failed, out of memory");
                    return CLI_OUT_OF_MEMORY;
                }
                if (previous_layer) {
                    previous_layer->upper_layer = new_layer;
                    new_layer->lower_layer = previous_layer;
                }
                previous_layer = new_layer;

                //layer regions
                int layer_regions_count = layer_json[JSON_LAYER_REGIONS].size();
                for (int region_index = 0; region_index < layer_regions_count; region_index++)
                {
                    json& region_json = layer_json[JSON_LAYER_REGIONS][region_index];
                    size_t config_hash = region_json[JSON_LAYER_REGION_CONFIG_HASH];
                    const PrintRegion *print_region = find_region(obj, config_hash);

                    if (!print_region){
                        BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":can not find print region of object %1%, layer %2%, print_z %3%, layer_region %4%")
                            %name % index %new_layer->print_z %region_index;
                        //delete new_layer;
                        return CLI_IMPORT_CACHE_DATA_CAN_NOT_USE;
                    }

                    new_layer->add_region(print_region);
                }

            }

            //load the layer data parallel
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": load the layers in parallel");
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->layer_count()),
                [&root_json, &obj](const tbb::blocked_range<size_t>& layer_range) {
                    for (size_t layer_index = layer_range.begin(); layer_index < layer_range.end(); ++ layer_index) {
                        const json& layer_json = root_json[JSON_LAYERS][layer_index];
                        Layer* layer = obj->get_layer(layer_index);
                        extract_layer(layer_json, *layer);
                    }
                }
            );

            //support layers
            Layer* previous_support_layer = NULL;
            //create support_layers
            for (int index = 0; index < support_layer_count; index++)
            {
                json& layer_json = root_json[JSON_SUPPORT_LAYERS][index];
                SupportLayer* new_support_layer = obj->add_support_layer(layer_json[JSON_LAYER_ID], layer_json[JSON_SUPPORT_LAYER_INTERFACE_ID], layer_json[JSON_LAYER_HEIGHT], layer_json[JSON_LAYER_PRINT_Z]);
                if (!new_support_layer) {
                    BOOST_LOG_TRIVIAL(error) <<__FUNCTION__<< boost::format(":add_support_layer failed, out of memory");
                    return CLI_OUT_OF_MEMORY;
                }
                if (previous_support_layer) {
                    previous_support_layer->upper_layer = new_support_layer;
                    new_support_layer->lower_layer = previous_support_layer;
                }
                previous_support_layer = new_support_layer;
            }

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": finished load layers, start to load support_layers.");
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, obj->support_layer_count()),
                [&root_json, &obj](const tbb::blocked_range<size_t>& support_layer_range) {
                    for (size_t layer_index = support_layer_range.begin(); layer_index < support_layer_range.end(); ++ layer_index) {
                        const json& layer_json = root_json[JSON_SUPPORT_LAYERS][layer_index];
                        SupportLayer* support_layer = obj->get_support_layer(layer_index);
                        extract_support_layer(layer_json, *support_layer);
                    }
                }
            );

            //load first group volumes
            std::vector<groupedVolumeSlices>& firstlayer_objgroups = obj->firstLayerObjGroupsMod();
            for (int index = 0; index < firstlayer_group_count; index++)
            {
                json& firstlayer_group_json = root_json[JSON_FIRSTLAYER_GROUPS][index];
                groupedVolumeSlices firstlayer_group = firstlayer_group_json;
                //convert the id
                for (ObjectID& obj_id : firstlayer_group.volume_ids)
                {
                    ModelVolume* currentModelVolumePtr = nullptr;
                    ModelVolumePtrs& volumes_ptr = obj->model_object()->volumes;
                    size_t volume_count = volumes_ptr.size();
                    if (obj_id.id < volume_count) {
                        currentModelVolumePtr = volumes_ptr[obj_id.id];
                        obj_id = currentModelVolumePtr->id();
                    }
                    else {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": can not find volume_id %1% from object file %2% in firstlayer groups, volume_count %3%!")
                            %obj_id.id %object_filenames[obj_index].first %volume_count;
                        return CLI_IMPORT_CACHE_LOAD_FAILED;
                    }
                }
                firstlayer_objgroups.push_back(std::move(firstlayer_group));
            }

            count ++;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": load object %1% from %2% successfully.")%count%object_filenames[obj_index].first;
        }
        catch(nlohmann::detail::parse_error &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse "<<object_filenames[obj_index].first<<" got a nlohmann::detail::parse_error, reason = " << err.what();
            return CLI_IMPORT_CACHE_LOAD_FAILED;
        }
        catch(std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": load from "<<object_filenames[obj_index].first<<" got a generic exception, reason = " << err.what();
            ret = CLI_IMPORT_CACHE_LOAD_FAILED;
        }
    }

    object_jsons.clear();
    object_filenames.clear();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": total printobject count %1%, loaded %2%, ret=%3%")%m_objects.size() %count %ret;
    return ret;
}

BoundingBoxf3 PrintInstance::get_bounding_box() {
    return print_object->model_object()->instance_bounding_box(*model_instance, false);
}

Polygon PrintInstance::get_convex_hull_2d() {
    Polygon poly = print_object->model_object()->convex_hull_2d(model_instance->get_matrix());
    // Change the distance threshold of the Douglas-Peucker algorithm to 0.5 millimeter and reduce the number of points
    poly.douglas_peucker(scale_(0.5));
    // Round coordinates to 0.1mm grid to limit decimal places
    poly.round_to_grid(scale_(0.1));
    return poly;
}

//BBS: instance_shift is too large because of multi-plate, apply without plate offset.
Point PrintInstance::shift_without_plate_offset() const
{
    const Print* print = print_object->print();
    const Vec3d plate_offset = print->get_plate_origin();
    return shift - Point(scaled(plate_offset.x()), scaled(plate_offset.y()));
}

PrintRegion *PrintObjectRegions::FuzzySkinPaintedRegion::parent_print_object_region(const LayerRangeRegions &layer_range) const
{
    using FuzzySkinParentType = PrintObjectRegions::FuzzySkinPaintedRegion::ParentType;

    if (this->parent_type == FuzzySkinParentType::PaintedRegion) {
        return layer_range.painted_regions[this->parent].region;
    }

    assert(this->parent_type == FuzzySkinParentType::VolumeRegion);
    return layer_range.volume_regions[this->parent].region;
}

int PrintObjectRegions::FuzzySkinPaintedRegion::parent_print_object_region_id(const LayerRangeRegions &layer_range) const
{
    return this->parent_print_object_region(layer_range)->print_object_region_id();
}

ExtrusionLayers FakeWipeTower::getTrueExtrusionLayersFromWipeTower() const 
{ 
    ExtrusionLayers wtels;
    wtels.type = ExtrusionLayersType::WIPE_TOWER;
    std::vector<float> layer_heights;
    layer_heights.reserve(outer_wall.size());
    auto pre = outer_wall.begin();
    for (auto it = outer_wall.begin(); it != outer_wall.end(); ++it) {
        if (it == outer_wall.begin())
            layer_heights.push_back(it->first);
        else {
            layer_heights.push_back(it->first - pre->first);
            ++pre;
        }
    }
    Point trans = {scale_(pos.x()), scale_(pos.y())};
    for (auto it = outer_wall.begin(); it != outer_wall.end(); ++it) {
        int index = std::distance(outer_wall.begin(), it);
        ExtrusionLayer el;
        ExtrusionPaths paths;
        paths.reserve(it->second.size());
        for (auto& polyline : it->second) {
            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, layer_heights[index]);
            path.polyline = polyline;
            for (auto& p : path.polyline.points)
                p += trans;
            paths.push_back(path);
        }
        el.paths = std::move(paths);
        el.bottom_z = it->first - layer_heights[index];
        el.layer = nullptr;
        wtels.push_back(el);
    }
    return wtels;
}

} // namespace Slic3r
