#pragma once

// Cost estimate of a sliced plate: filament (per filament slot, split into model / support /
// flush / wipe tower) plus machine time. wx-free and computed from GCodeProcessorResult alone, so
// it gives the same answer for a plate sliced now, a plate restored from a sliced 3MF and a G-code
// file dragged into the viewer.

#include <cstddef>
#include <vector>

namespace Slic3r {

struct GCodeProcessorResult;

struct FilamentCostLine
{
    size_t slot{0};             // filament (extruder) index, 0-based

    // Grams, from the processor's volume maps * density.
    double model_g{0.};
    double support_g{0.};
    double flush_g{0.};
    double tower_g{0.};
    double total_g{0.};         // every extrusion of this filament (the four above, as the processor sums them)

    double price_per_kg{0.};

    double model_cost{0.};
    double support_cost{0.};
    double flush_cost{0.};
    double tower_cost{0.};
    // total_g * price: the figure the G-code's "; filament cost" line and the statistics use.
    double total_cost{0.};
};

struct CostBreakdown
{
    std::vector<FilamentCostLine> lines;   // one per filament slot that extruded anything, by slot

    // False when the G-code carried no filament prices (exported with the "Include filament prices
    // in exported G-code" preference off, or written by another slicer). Material cost is then
    // unknown and reported as 0; machine time is still priced.
    bool   prices_known{true};

    double material{0.};                   // sum of lines' total_cost
    double print_time_s{0.};               // Normal-mode total time, prepare / heat-up included
    double machine_rate_per_h{0.};         // printer preset time_cost
    // False when the G-code carried no time_cost (exported with costs left out, or by another
    // slicer): the machine part is unknown and reported as 0.
    bool   machine_rate_known{true};
    bool   machine_rate_varies{false};     // a sum over plates whose printers charge different rates
    double machine{0.};                    // print_time_s / 3600 * machine_rate_per_h
    double total{0.};                      // material + machine: PrintStatistics::total_cost

    // What the cost panel shows. Each leaf is rounded to cents and the totals are sums of the rounded
    // leaves, so the numbers on screen always add up; a sum over plates adds the plates' rounded
    // totals. PR 1c (markup / selling price) derives from these.
    double display_material{0.};
    double display_machine{0.};
    double display_total{0.};

    size_t plates{1};                      // how many plates this breakdown sums

    bool any_price_set() const;            // a used filament has a non-zero price
    bool empty() const { return lines.empty() && print_time_s <= 0.; }
};

// Round half away from zero to 2 decimal places.
double round_money(double value);

// Breakdown of one plate from its processor result.
CostBreakdown compute_cost(const GCodeProcessorResult &result);

// Lines are merged per slot (grams and costs added; price kept when every plate used the same one,
// else the average price per kg of the merged material). Totals are the plates' rounded totals added.
CostBreakdown sum_costs(const std::vector<CostBreakdown> &plates);

} // namespace Slic3r
