#include "CostEstimate.hpp"

#include "GCode/GCodeProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace Slic3r {

double round_money(double value)
{
    // std::round rounds half away from zero; the small nudge keeps 0.125 (stored as 0.12499999...)
    // from rounding down.
    const double scaled = value * 100.;
    return std::round(scaled + (scaled >= 0. ? 1e-9 : -1e-9)) / 100.;
}

bool CostBreakdown::any_price_set() const
{
    for (const FilamentCostLine &line : lines)
        if (line.price_per_kg > 0. && line.total_g > 0.)
            return true;
    return false;
}

static void finish_display(CostBreakdown &out)
{
    out.display_material = 0.;
    for (const FilamentCostLine &line : out.lines)
        out.display_material += round_money(line.total_cost);
    out.display_machine = round_money(out.machine);
    out.display_total   = out.display_material + out.display_machine;
}

CostBreakdown compute_cost(const GCodeProcessorResult &result)
{
    CostBreakdown out;
    const PrintEstimatedStatistics &ps = result.print_statistics;

    out.prices_known       = result.has_filament_costs;
    out.print_time_s       = ps.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    out.machine_rate_known = result.has_time_cost;
    out.machine_rate_per_h = out.machine_rate_known ? std::max(0., result.time_cost) : 0.;
    out.machine            = out.machine_rate_per_h * (out.print_time_s / 3600.);

    std::set<size_t> slots;
    for (const std::map<size_t, double> *m : { &ps.total_volumes_per_extruder, &ps.model_volumes_per_extruder,
                                                &ps.support_volumes_per_extruder, &ps.flush_per_filament,
                                                &ps.wipe_tower_volumes_per_extruder })
        for (const auto &[slot, volume] : *m)
            slots.insert(slot);

    auto volume_of = [](const std::map<size_t, double> &m, size_t slot) {
        auto it = m.find(slot);
        return it == m.end() ? 0. : it->second;
    };

    for (size_t slot : slots) {
        const double density = slot < result.filament_densities.size() ? double(result.filament_densities[slot]) : 0.;
        const double price   = out.prices_known && slot < result.filament_costs.size() ? double(result.filament_costs[slot]) : 0.;
        // mm3 -> cm3 -> g, in the order GCodeProcessor::run_post_process() writes "; filament used [g]".
        auto grams = [density](double volume_mm3) { return volume_mm3 * 0.001 * density; };
        auto cost  = [price](double g) { return g * price * 0.001; };

        FilamentCostLine line;
        line.slot         = slot;
        line.model_g      = grams(volume_of(ps.model_volumes_per_extruder, slot));
        line.support_g    = grams(volume_of(ps.support_volumes_per_extruder, slot));
        line.flush_g      = grams(volume_of(ps.flush_per_filament, slot));
        line.tower_g      = grams(volume_of(ps.wipe_tower_volumes_per_extruder, slot));
        const auto total  = ps.total_volumes_per_extruder.find(slot);
        line.total_g      = total != ps.total_volumes_per_extruder.end() ?
                                grams(total->second) :
                                line.model_g + line.support_g + line.flush_g + line.tower_g;
        if (line.total_g <= 0.)
            continue;
        line.price_per_kg = price;
        line.model_cost   = cost(line.model_g);
        line.support_cost = cost(line.support_g);
        line.flush_cost   = cost(line.flush_g);
        line.tower_cost   = cost(line.tower_g);
        line.total_cost   = cost(line.total_g);
        out.material += line.total_cost;
        out.lines.emplace_back(line);
    }

    out.total = out.material + out.machine;
    finish_display(out);
    return out;
}

CostBreakdown sum_costs(const std::vector<CostBreakdown> &plates)
{
    CostBreakdown out;
    out.plates = 0;
    if (plates.empty())
        return out;

    std::map<size_t, FilamentCostLine> lines;
    std::map<size_t, std::set<double>> prices;
    bool   first_rate = true;
    for (const CostBreakdown &plate : plates) {
        out.plates += plate.plates;
        out.prices_known = out.prices_known && plate.prices_known;
        out.machine_rate_known = out.machine_rate_known && plate.machine_rate_known;
        for (const FilamentCostLine &line : plate.lines) {
            FilamentCostLine &dst = lines[line.slot];
            dst.slot = line.slot;
            dst.model_g += line.model_g;
            dst.support_g += line.support_g;
            dst.flush_g += line.flush_g;
            dst.tower_g += line.tower_g;
            dst.total_g += line.total_g;
            dst.model_cost += line.model_cost;
            dst.support_cost += line.support_cost;
            dst.flush_cost += line.flush_cost;
            dst.tower_cost += line.tower_cost;
            dst.total_cost += line.total_cost;
            prices[line.slot].insert(line.price_per_kg);
        }
        out.material += plate.material;
        out.print_time_s += plate.print_time_s;
        out.machine += plate.machine;
        out.total += plate.total;
        out.display_material += plate.display_material;
        out.display_machine += plate.display_machine;
        out.display_total += plate.display_total;
        if (plate.print_time_s > 0.) {
            if (first_rate) {
                out.machine_rate_per_h = plate.machine_rate_per_h;
                first_rate             = false;
            } else if (plate.machine_rate_per_h != out.machine_rate_per_h)
                out.machine_rate_per_h = 0., out.machine_rate_varies = true;
        }
        out.machine_rate_varies = out.machine_rate_varies || plate.machine_rate_varies;
    }
    if (out.machine_rate_varies)
        out.machine_rate_per_h = 0.;

    for (auto &[slot, line] : lines) {
        const std::set<double> &p = prices[slot];
        line.price_per_kg = p.size() == 1 ? *p.begin() : (line.total_g > 0. ? line.total_cost / line.total_g * 1000. : 0.);
        out.lines.emplace_back(line);
    }
    return out;
}

} // namespace Slic3r
