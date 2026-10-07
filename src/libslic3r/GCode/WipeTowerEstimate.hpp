#pragma once

#include <optional>
#include <vector>

#include "../Point.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

class ConfigBase;

// EdgeSlicer has no wipe_tower_type option: Type1 is the Bambu planner (is_BBL_printer),
// Type2 is WipeTower2 for everyone else. Resolved from printer_model so GUI/CLI agree.
enum class WipeTowerType {
    Type1 = 0,
    Type2 = 1,
};

// Pre-slice footprint of the wipe tower, shared by validation (Print), the GUI's placement
// clamp/preview/arrange and the CLI placement. The arithmetic is shared; the inputs below are
// not, so a change to how one caller derives them has to be mirrored in the others.
struct WipeTowerFootprint
{
    double width      = 0.; // effective width: equals depth for a rib wall, which squares the tower
    double depth      = 0.; // 0 when these inputs imply no tower
    double height     = 0.; // tallest object; drives the stability floor and the auto brim
    double brim_width = 0.; // printed width: auto (-1) resolved by height, laid in whole loops; at least the
                            // tower interface run-in reserve (TowerInterface::run_in_reserve())
};

// Which planner builds the tower: Bambu Lab printers always get Type1, the rest follow
// wipe_tower_type. The rule Print::wipe_tower_type() and the CLI apply, read off the config so
// the GUI and CLI placement can resolve it without a Print.
WipeTowerType resolve_wipe_tower_type(const ConfigBase &config);

// First-layer outline of an estimated tower in tower-local scaled coordinates, brim excluded:
// the body box, or for a Type2 cone wall the box unioned with the cone's base. The preview,
// the placement margin and validation all take the outline from here so they cannot disagree
// about whether a cone exists.
Polygon estimate_wipe_tower_first_layer_outline(const ConfigBase &config, WipeTowerType tower_type, double width, double depth, double height);

// filament_ids:  0-based filaments purged on the plate. The config cannot see custom G-code tool
//                changes, so ids derived from the model must include them
//                (Print::extruders(true)) or a real tower is sized as if it were never built.
// layer_height:  thinnest layer the objects are sliced at. The first layer is folded in here.
//
// A raft is deliberately not a reason: normalize_fdm_2 clears enable_prime_tower for a plate
// purging one filament unless smooth timelapse or wrapping detection is on.
WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase                &config,
                                                 WipeTowerType                    tower_type,
                                                 const std::vector<unsigned int> &filament_ids,
                                                 double                           layer_height,
                                                 double                           max_object_height);

// The tower's first-layer footprint, brim included, at plate position `pos` (its front left corner,
// wipe_tower_x / wipe_tower_y), rotated by wipe_tower_rotation_angle: the polygon Print::validate()
// tests against the bed exclusion area.
Polygon placed_wipe_tower_footprint(const ConfigBase &config, const WipeTowerFootprint &footprint, const Vec2d &pos);

// System placement of a tower that would sit on the bed exclusion area. A stored or default position
// whose footprint misses `excluded` is left alone (nullopt), so G-code that slices today is unchanged.
// Otherwise the first clear one of: `preferred` (the GUI's default corner) clamped
// WIPE_TOWER_AUTO_MARGIN + brim inside the bed - the clamp of
// PartPlateList::set_default_wipe_tower_pos_for_plate - then the bed's four corners at that margin.
// nullopt when every candidate is blocked too, leaving validation to report it.
// bed_size: plate width and depth; excluded: bed_exclude_area in plate coordinates.
std::optional<Vec2d> wipe_tower_position_clear_of_exclusion(const ConfigBase         &config,
                                                            const WipeTowerFootprint &footprint,
                                                            const Polygons           &excluded,
                                                            const Vec2d              &bed_size,
                                                            const Vec2d              &preferred,
                                                            const Vec2d              &current);

} // namespace Slic3r
