#ifndef slic3r_ExtruderAreas_hpp_
#define slic3r_ExtruderAreas_hpp_

// Per-nozzle reach of a multi-nozzle printer (Bambu H2D / H2C / X2D).
//
// A dual-nozzle machine has one shared bed, but each nozzle only reaches part of it: on the H2D the left nozzle
// reaches X 0..325 and the right one X 25..350, so the strip 0..25 is "left nozzle only" and 325..350 is
// "right nozzle only". The printer profile carries this as extruder_printable_area (one polygon per extruder,
// extruder 0 = left) and extruder_printable_height. Bambu Studio draws the two exclusive strips on the plate
// and refuses a plate where a filament assigned to one nozzle prints something that nozzle cannot reach.
//
// Everything here is pure geometry on plain values (no GUI, no Print), so the overlay, the pre-slice check
// and the unit tests share one definition of "what a nozzle reaches".

#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "Point.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

class ConfigBase;

struct ExtruderAreas
{
    // Scaled polygons, in the same (plate-local) coordinates as printable_area.
    // What each extruder reaches. Empty polygon lists for an unconstrained extruder are never produced:
    // a missing or degenerate area falls back to the whole bed.
    std::vector<Polygons> printable;
    // The bed minus what the extruder reaches (what the extruder must never print into).
    std::vector<Polygons> unprintable;
    // What this extruder reaches and no other extruder does (the "left only" / "right only" strips).
    std::vector<Polygons> only;
    // What every extruder reaches (where a wipe tower or a multi-material object may sit).
    Polygons shared;
    // Per extruder, mm. <= 0 means "no limit of its own" (the bed height applies).
    std::vector<double> heights;

    size_t count() const { return printable.size(); }
    // True when there is more than one nozzle whose reach is declared.
    bool   multi() const { return printable.size() >= 2; }
    // True when at least one nozzle has a strip of the bed the others cannot reach.
    bool   has_exclusive_regions() const;
    // True when the nozzles differ in how high they can print.
    bool   has_height_limits() const;
    // Largest height extruder `e` can print, or 0 for "no limit of its own".
    double height_limit(size_t e) const { return e < heights.size() && heights[e] > 0. ? heights[e] : 0.; }

    // Same reach (the derived strips follow from it).
    bool operator==(const ExtruderAreas &other) const { return printable == other.printable && heights == other.heights; }
    bool operator!=(const ExtruderAreas &other) const { return !(*this == other); }
};

// Build the areas from unscaled values (mm). `bed` is printable_area, `extruder_areas` is
// extruder_printable_area (one polygon per extruder, empty or fewer than 3 points = the whole bed) and
// `extruder_heights` is extruder_printable_height (nil / non-positive = no limit).
// Fewer than two extruder areas gives an empty (single-nozzle) result: single-nozzle printers and the
// four-toolhead Snapmaker U1 declare no extruder areas and so never get an overlay or a check.
ExtruderAreas compute_extruder_areas(const Pointfs &bed, const std::vector<Pointfs> &extruder_areas, const std::vector<double> &extruder_heights);

// Builds before the profile loader joined extruder_printable_area with '#' read the two nozzle polygons of a
// printer profile as ONE group of eight points, and saved projects carry that. Eight points that are exactly two
// axis-aligned rectangles (four points each) cannot be one polygon, so they are split back into the two nozzles'
// areas; anything else is returned unchanged.
std::vector<Pointfs> split_merged_extruder_areas(std::vector<Pointfs> areas);

// The same from a printer / full config (printable_area, extruder_printable_area, extruder_printable_height).
ExtruderAreas extruder_areas_from_config(const ConfigBase &config);

// The same, with every polygon moved by `offset` (mm), e.g. to a plate's place in the plate grid.
ExtruderAreas translate_extruder_areas(const ExtruderAreas &areas, const Vec2d &offset);

// Diagonal stripes (45 degrees) of `stripe_mm` width every `period_mm`, clipped to `region`: the hatching
// drawn over a nozzle-only strip of the plate. Empty for an empty region or a non-positive size.
ExPolygons hatch_region(const Polygons &region, double period_mm, double stripe_mm);

// True when `point` is inside the polygons (boundary counts as inside).
bool point_in_area(const Point &point, const Polygons &area);

// True when `footprint` lies inside `area`, allowing `tolerance_mm` of slack so an object snapped exactly to
// the edge of a nozzle's reach is not reported (arrangement places objects with float round-off).
bool footprint_within(const Polygons &footprint, const Polygons &area, double tolerance_mm = 0.1);

// Which extruders can print an object with this plate-local footprint that rises to `max_z` mm: it must lie
// inside the extruder's area and not be taller than the extruder's own height limit. Size = areas.count(),
// all true for a single-nozzle result.
std::vector<bool> extruders_reaching(const ExtruderAreas &areas, const Polygons &footprint, double max_z, double tolerance_mm = 0.1);

// ---- the decision: which filaments of which objects cannot be printed ----------------------------------

struct ObjectReach
{
    int                id = -1;            // caller's key (object index, name lookup, ...)
    std::vector<int>   filaments;          // 0-based filaments the object is printed with
    std::vector<bool>  reach;              // per extruder: can it print the whole object
};

struct ReachViolation
{
    int              filament = -1;        // 0-based
    // The extruder (0-based) the filament is assigned to and cannot reach the object with; -1 when the
    // filament is not tied to one nozzle (automatic grouping) and no nozzle can print everything it must.
    int              extruder = -1;
    std::vector<int> object_ids;           // the objects that cannot be reached, in input order, no duplicates
};

// Decide which filaments cannot be printed.
//  manual:  filament_map (1-based extruder per filament) is binding. A filament is a violation when an object
//           that uses it is out of reach of the nozzle the filament is mapped to.
//  !manual: the grouping picks the nozzle, so a filament is only a violation when every nozzle is blocked
//           for it by some object (it cannot be printed by any nozzle).
// Objects whose reach vector is shorter than extruder_count are treated as reachable by the missing extruders.
std::vector<ReachViolation> find_reach_violations(const std::vector<ObjectReach> &objects, size_t extruder_count,
                                                  const std::vector<int> &filament_map, bool manual);

} // namespace Slic3r

#endif // slic3r_ExtruderAreas_hpp_
