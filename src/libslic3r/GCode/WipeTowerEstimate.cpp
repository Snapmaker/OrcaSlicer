#include "WipeTowerEstimate.hpp"

#include "WipeTower.hpp"
#include "WipeTower2.hpp"
#include "../Config.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace Slic3r {

// Every caller today declares all these keys, but the signature accepts any ConfigBase: fall
// back to the key's declared default, never to a hand-copied constant.
static const ConfigOption *option_of(const ConfigBase &config, const char *key)
{
    if (const ConfigOption *opt = config.option(key); opt != nullptr)
        return opt;
    if (const ConfigDef *def = config.def(); def != nullptr)
        if (const ConfigOptionDef *opt_def = def->get(key); opt_def != nullptr)
            return opt_def->default_value.get();
    return nullptr;
}

WipeTowerType resolve_wipe_tower_type(const ConfigBase &config)
{
    // printer_model is what the CLI keys its Bambu Lab detection on; the GUI's vendor flag
    // agrees for every shipped profile.
    if (const auto *model = dynamic_cast<const ConfigOptionString *>(config.option("printer_model"));
        model != nullptr && model->value.compare(0, 9, "Bambu Lab") == 0)
        return WipeTowerType::Type1;
    // By value, not by concrete type: a static PrintConfig holds ConfigOptionEnum<T>, a
    // DynamicConfig built from presets holds ConfigOptionEnumGeneric, and both answer getInt().
    const ConfigOption *type = option_of(config, "wipe_tower_type");
    return type != nullptr ? WipeTowerType(type->getInt()) : WipeTowerType::Type2;
}

Polygon estimate_wipe_tower_first_layer_outline(const ConfigBase &config, WipeTowerType tower_type, double width, double depth, double height)
{
    // Type1 ignores the cone option. The wall type is read by value: a preset-shaped config
    // holds it as ConfigOptionEnumGeneric, which a cast to ConfigOptionEnum<T> cannot see.
    const ConfigOption *wall_type  = option_of(config, "wipe_tower_wall_type");
    const ConfigOption *cone_angle = option_of(config, "wipe_tower_cone_angle");
    const bool          cone       = tower_type == WipeTowerType::Type2 && wall_type != nullptr &&
                         wall_type->getInt() == int(WipeTowerWallType::wtwCone) && cone_angle != nullptr;
    return WipeTower2::cone_base_polygon(width, depth, height, cone ? cone_angle->getFloat() : 0.);
}

WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase &config, WipeTowerType tower_type, const std::vector<unsigned int> &filament_ids, double layer_height, double max_object_height)
{
    WipeTowerFootprint footprint;
    footprint.height = max_object_height;
    const size_t filaments_cnt = filament_ids.size();
    if (filaments_cnt == 0 || layer_height < EPSILON)
        return footprint;

    auto opt_float = [&config](const char *key) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr ? opt->getFloat() : 0.;
    };
    auto opt_bool = [&config](const char *key) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr && opt->getBool();
    };
    auto opt_enum = [&config](const char *key, int fallback) {
        const ConfigOption *opt = option_of(config, key);
        return opt != nullptr ? opt->getInt() : fallback;
    };
    auto floats_of = [&config](const char *key) { return dynamic_cast<const ConfigOptionFloats *>(option_of(config, key)); };
    auto max_of = [&floats_of](const char *key, double fallback) {
        const auto *opt = floats_of(key);
        return (opt != nullptr && !opt->values.empty()) ? *std::max_element(opt->values.begin(), opt->values.end()) : fallback;
    };
    auto float_at = [&floats_of](const char *key, unsigned int id, double fallback) {
        const auto *opt = floats_of(key);
        return (opt != nullptr && !opt->values.empty()) ? opt->get_at(id) : fallback;
    };
    auto int_at = [&config](const char *key, unsigned int id, int fallback) {
        const auto *opt = dynamic_cast<const ConfigOptionInts *>(option_of(config, key));
        return (opt != nullptr && !opt->values.empty()) ? opt->get_at(id) : fallback;
    };

    // Both planners size every layer, so the tower has to fit its thinnest one: the first layer
    // when it is printed thinner than the rest.
    const double first_layer_height = opt_float("initial_layer_print_height");
    if (first_layer_height > EPSILON)
        layer_height = std::min(layer_height, first_layer_height);

    const bool   type1            = tower_type == WipeTowerType::Type1;
    const double width            = opt_float("prime_tower_width");
    const double prime_volume     = opt_float("prime_volume");
    // Type1 spaces its purge lines by prime_tower_infill_gap, Type2 by wipe_tower_extra_spacing.
    // Type2's extra flow cancels out of the depth: the line length is divided by it and the row
    // pitch multiplied by it (WipeTower2::get_wipe_depth).
    const double extra_spacing    = opt_float(type1 ? "prime_tower_infill_gap" : "wipe_tower_extra_spacing") / 100.;
    const double rib_width        = opt_float("wipe_tower_rib_width");
    const double extra_rib_length = opt_float("wipe_tower_extra_rib_length");
    const auto  *nozzle_opt       = floats_of("nozzle_diameter");
    const double nozzle_diameter  = (nozzle_opt != nullptr && !nozzle_opt->values.empty()) ? nozzle_opt->values.front() : 0.4;
    const bool   dual_nozzle      = nozzle_opt != nullptr && nozzle_opt->values.size() == 2;
    const bool   rib_wall         = opt_enum("wipe_tower_wall_type", int(WipeTowerWallType::wtwRectangle)) == int(WipeTowerWallType::wtwRib);
    const bool   smooth_timelapse = opt_enum("timelapse_type", int(TimelapseType::tlTraditional)) == int(TimelapseType::tlSmooth);
    const bool   wrapping         = opt_bool("enable_wrapping_detection");
    // Reasons a tower is printed with no tool change to purge for: the ones that stop
    // normalize_fdm_2 clearing enable_prime_tower. Its mixed-filament case is not modelled.
    const bool   need_wipe_tower  = smooth_timelapse || wrapping;

    // Fewer than two filaments cannot make a tool change, so only wrapping detection or smooth
    // timelapse print a tower then. The flush volume is no proof of one: it is read from the
    // matrix of every configured slot, nonzero even when a single one of them is used.
    if (filaments_cnt < 2 && !need_wipe_tower)
        return footprint;

    // A tower printed for one of the reasons above has no tool change to purge for; both
    // planners give it the idle depth below and nothing more.
    const size_t purge_count = filaments_cnt > 1 ? (dual_nozzle ? filaments_cnt : filaments_cnt - 1) : 0;

    // Type2 purges one volume per tool change. Type1 plans per filament below; here the volume
    // only decides whether a tower exists.
    double volume = prime_volume * double(purge_count);
    if (dual_nozzle) {
        // Dual-nozzle printers also purge the filament change length on the tower.
        const double length   = max_of("filament_change_length", 0.);
        const double diameter = max_of("filament_diameter", 1.75);
        volume += length * PI * diameter * diameter / 4. * double(filaments_cnt / 2);
    }
    // Single-extruder multi-material purges the flush matrix instead of the prime volume.
    const bool semm_flush = opt_bool("purge_in_prime_tower") && opt_bool("single_extruder_multi_material");
    if (semm_flush)
        volume = WipeTower2::estimate_semm_flush_volume(config, filaments_cnt);

    // The Type1 planner wipes each filament's own prime volume after changing to it, in a block
    // per adhesiveness category. On a two-nozzle printer the leaving filament is also rammed at
    // every nozzle change; the tool order groups filaments by nozzle, so a layer crosses
    // (nozzles used - 1) times, charged here to the longest ramming.
    std::vector<WipeTower::PurgeEstimate> purges;
    if (type1 && filaments_cnt > 1) {
        const bool    saving_mode = opt_enum("prime_volume_mode", int(PrimeVolumeMode::pvmDefault)) == int(PrimeVolumeMode::pvmSaving);
        std::set<int> nozzles;
        size_t        longest_ramming = 0;
        for (size_t i = 0; i < filaments_cnt; ++i) {
            const unsigned int         id = filament_ids[i];
            WipeTower::PurgeEstimate purge;
            purge.prime_volume      = saving_mode ? 15.f : float(float_at("filament_prime_volume", id, prime_volume));
            purge.category          = int_at("filament_adhesiveness_category", id, 0);
            purge.filament_diameter = float(float_at("filament_diameter", id, 1.75));
            purges.push_back(purge);
            if (dual_nozzle) {
                nozzles.insert(int_at("filament_map", id, 1));
                if (float_at("filament_change_length", id, 0.) > float_at("filament_change_length", filament_ids[longest_ramming], 0.))
                    longest_ramming = i;
            }
        }
        if (nozzles.size() > 1)
            purges[longest_ramming].filament_change_length = float(float_at("filament_change_length", filament_ids[longest_ramming], 0.) * double(nozzles.size() - 1));
    }

    const double min_depth      = WipeTower::get_limit_depth_by_height(float(max_object_height));
    const float  perimeter_width = float(nozzle_diameter) * 1.25f; // Width_To_Nozzle_Ratio

    // Type2 (flush matrix: rib wall only): a layer's depth is the sum of what set_toolchange() reserves per
    // change - the old tool's ram band and the new tool's whole purge rows, each at its own line width -
    // plus the wall. One change per further filament and layer, in id order; nozzle changes add none.
    const size_t planned_changes = filaments_cnt > 1 ? filaments_cnt - 1 : 0;
    const bool   type2_planned   = !type1 && planned_changes > 0 && (!semm_flush || rib_wall);
    const std::vector<std::vector<float>> flush_volumes = type2_planned && semm_flush ? WipeTower2::extract_wipe_volumes(config)
                                                                                      : std::vector<std::vector<float>>();
    const float widest_line   = float((nozzle_opt != nullptr && !nozzle_opt->values.empty()
                                       ? *std::max_element(nozzle_opt->values.begin(), nozzle_opt->values.end()) : nozzle_diameter) * 1.25);
    auto type2_depth = [&](double tower_width) {
        const bool   semm      = opt_bool("single_extruder_multi_material");
        const bool   gap_wall  = opt_bool("prime_tower_skip_points") && opt_bool("wipe_tower_wall_gap") &&
                                 opt_enum("wipe_tower_wall_type", int(WipeTowerWallType::wtwRectangle)) != int(WipeTowerWallType::wtwCone);
        const double extra_flow  = opt_float("wipe_tower_extra_flow") / 100.;
        const auto  *self_index  = dynamic_cast<const ConfigOptionInts *>(option_of(config, "filament_self_index"));
        const auto  *ramming_on  = dynamic_cast<const ConfigOptionBools *>(option_of(config, "filament_multitool_ramming"));
        auto line_width_of = [&](unsigned int id) {
            const int extruder = int_at("filament_map", id, int(id) + 1) - 1;
            return float(float_at("nozzle_diameter", unsigned(std::max(extruder, 0)), nozzle_diameter) * 1.25);
        };
        double depth = 0.;
        for (size_t k = 1; k <= planned_changes; ++k) {
            const unsigned int old_id = filament_ids[k - 1];
            const unsigned int new_id = filament_ids[k];
            const size_t       column = self_index != nullptr ? first_filament_variant_column(self_index->values, old_id) : old_id;
            const double       ramming_volume = float_at("filament_multitool_ramming_volume", unsigned(column), 0.);
            const double       ramming_flow   = float_at("filament_multitool_ramming_flow", unsigned(column), 0.);
            WipeTower2::ToolChangeGeometry g;
            g.tower_width                      = float(tower_width);
            g.widest_line_width                = widest_line;
            g.layer_height                     = float(layer_height);
            g.old_line_width                   = line_width_of(old_id);
            g.ramming_line_width_multiplicator = float(opt_float("ramming_line_width_ratio"));
            g.ramming                          = !semm && ramming_on != nullptr && !ramming_on->values.empty() &&
                                                 ramming_on->get_at(column) && ramming_volume > 0. && ramming_flow > 0.;
            g.ramming_volume                   = g.ramming ? float(ramming_volume) : 0.f;
            if (semm) {
                // filament_ramming_parameters: line width %, step %, speeds per 0.25 s.
                const auto *parameters = dynamic_cast<const ConfigOptionStrings *>(option_of(config, "filament_ramming_parameters"));
                std::istringstream stream(parameters != nullptr && !parameters->values.empty() ? parameters->get_at(old_id) : std::string());
                float line_width = 100.f, step = 100.f, speed = 0.f, speeds = 0.f;
                stream >> line_width >> step;
                while (stream >> speed)
                    speeds += speed;
                g.ramming                          = opt_bool("enable_filament_ramming");
                g.ramming_line_width_multiplicator = line_width / 100.f;
                g.ramming_step_multiplicator       = step / 100.f;
                g.ramming_volume                   = 0.25f * speeds;
            }
            g.boundary_wipe_start              = g.ramming && gap_wall && !semm;
            g.new_line_width                   = line_width_of(new_id);
            g.wipe_volume = old_id < flush_volumes.size() && new_id < flush_volumes[old_id].size() ? flush_volumes[old_id][new_id]
                                                                                                    : float(prime_volume);
            g.extra_flow                       = float(extra_flow);
            g.extra_spacing_wipe               = float(extra_spacing * extra_flow);
            g.extra_spacing_ramming            = float(extra_spacing);
            depth += WipeTower2::toolchange_depth(g).total();
        }
        return depth + widest_line;
    };
    // With nothing to purge, plan_tower_new sizes the tower for wrapping detection or the
    // stability minimum; WipeTower2 only knows the latter.
    const double idle_depth = (type1 && wrapping && !smooth_timelapse) ? WipeTower::get_wrapping_detection_depth() : min_depth;
    if (rib_wall) {
        // Both planners square the tower to the purge area and extend the ribs, not the body,
        // below the stability minimum.
        double side;
        if (!purges.empty())
            side = WipeTower::estimate_rib_tower_bbox_side(purges, float(width), float(layer_height), float(nozzle_diameter), float(extra_spacing), float(rib_width), float(extra_rib_length), float(max_object_height));
        else if (type2_planned) {
            // WipeTower2::generate() squares the rib tower: the width becomes sqrt(depth x width)
            // (rounded up to the wall line), every change is planned again for it, and the depth
            // follows from that plan.
            const double square = std::ceil(std::sqrt(type2_depth(width) * width) / widest_line) * widest_line;
            side = WipeTower::rib_footprint_side(float(square), float(type2_depth(square)), float(rib_width), float(extra_rib_length), float(max_object_height));
        } else {
            // Type2 squares the tower from its purge volume; Type1 with no purge list (a lone
            // filament kept for timelapse or wrapping) sizes for the idle depth.
            const bool   has_purge = !type1 && volume > EPSILON;
            const double square    = has_purge ? std::sqrt(volume / layer_height * extra_spacing) : idle_depth;
            side = WipeTower::rib_footprint_side(float(square), float(square), float(rib_width), float(extra_rib_length), float(max_object_height));
        }
        footprint.width = footprint.depth = side;
    } else {
        double depth;
        if (type1) {
            // plan_tower_new stretches a short purge stack to the stability minimum behind its
            // leading perimeter width.
            depth = purges.empty() ? idle_depth : std::max(min_depth + perimeter_width, double(WipeTower::estimate_tower_blocks_depth(purges, float(width), float(layer_height), float(nozzle_diameter), float(extra_spacing))));
        } else if (type2_planned) {
            depth = std::max(min_depth, type2_depth(width));
        } else {
            depth = volume / (layer_height * width);
            // The flush volumes already hold the spacing between wipes.
            if (!semm_flush)
                depth *= extra_spacing;
            depth = std::max(min_depth, depth);
        }
        footprint.width = width;
        footprint.depth = depth;
    }

    footprint.brim_width = opt_float("prime_tower_brim_width");
    if (footprint.brim_width < 0)
        footprint.brim_width = WipeTower::get_auto_brim_by_height(float(max_object_height));
    footprint.brim_width = WipeTower::estimate_brim_real_width(float(footprint.brim_width), float(nozzle_diameter), float(first_layer_height > EPSILON ? first_layer_height : layer_height), !type1);
    return footprint;
}

} // namespace Slic3r
