#pragma once

// Fees and markup on top of a plate's cost (the Costs window, tab Project): what a print is SOLD
// for, as opposed to what it costs. Display only: none of this is a print setting, so changing it
// never invalidates a slice, and it is never written into G-code.
//
// ---- What is charged (per plate; each plate is one print job)
//
//   material       the plate's filament cost (CostBreakdown)
//   machine time   print time x machine rate (CostBreakdown)
//   assembly       assembly_hours x assembly_rate_per_h          (hours of work per plate)
//   object fee     fee_per_object x objects on the plate          (every printable instance on the
//                                                                  plate; copies count separately)
//   part fee       fee_per_part x parts on the plate              (the MODEL parts of each counted
//                                                                  object: modifiers, negative
//                                                                  volumes, support blockers /
//                                                                  enforcers and seam modifiers are
//                                                                  not parts; per instance)
//   setup fee      fee_per_plate, once per plate
//   packaging      packaging_per_plate, once per plate
//   total cost     material + machine time + the fees
//
// The all-plates figure is the sum of the plates' figures, so a 3-plate project pays the setup and
// packaging fees three times.
//
// ---- Markup (on cost: 100% doubles it, it is not a margin on the price)
//
//   percent, basis material : markup = material x p / 100        (machine time and fees unmarked)
//   percent, basis overall  : markup = total cost x p / 100      (material + machine time + fees)
//   flat                    : markup = the amount, once per plate (the basis does not apply)
//   selling price           = total cost + markup
//
// ---- Rounding
//
// Every line is rounded to cents (round_money) and the totals are sums of the rounded lines; a
// percent markup is taken of the rounded basis. A sum over plates adds the plates' rounded lines,
// so the numbers on screen always add up.
//
// ---- Where the values come from
//
// Your defaults are kept with your filament prices and machine rates (CostOverrides::Store,
// "pricing", file version 3). A project can override any field (ProjectPricing, held on Model and
// saved in the project 3MF as model metadata "edgeslicer_pricing"; left out of "Export Bambu 3MF"
// and of sliced-plate files). A field the project does not set uses your default.

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class Model;
struct CostBreakdown;

enum class MarkupType { Percent, Flat };
enum class MarkupBasis { Material, Overall };

struct PricingSettings
{
    MarkupType  markup_type{MarkupType::Percent};
    MarkupBasis markup_basis{MarkupBasis::Overall};
    double      markup_value{0.};           // percent, or an amount per plate; 0 = no markup

    double      assembly_hours{0.};         // per plate
    double      assembly_rate_per_h{0.};
    double      fee_per_object{0.};
    double      fee_per_part{0.};
    double      fee_per_plate{0.};          // setup
    double      packaging_per_plate{0.};

    bool has_markup() const { return markup_value > 0.; }
    bool operator==(const PricingSettings &rhs) const;
    bool operator!=(const PricingSettings &rhs) const { return !(*this == rhs); }
};

// The fields a project can take over one by one ("use my default" per field). The markup's type,
// basis and value are one field: a value only means something with its type.
enum class PricingField : size_t {
    Markup,
    AssemblyHours,
    AssemblyRate,
    ObjectFee,
    PartFee,
    PlateFee,
    Packaging,
    Count
};
constexpr size_t PRICING_FIELDS = size_t(PricingField::Count);

// Copies one field from `from` into `to`.
void copy_pricing_field(PricingSettings &to, const PricingSettings &from, PricingField field);

struct ProjectPricing
{
    PricingSettings                    values;      // only the fields marked in `set` mean anything
    std::array<bool, PRICING_FIELDS>   set{};       // the project's own value, else your default
    std::string                        extra;       // fields of a later version, as JSON, written back

    bool empty() const;                              // nothing of the project's own
    bool is_set(PricingField f) const { return set[size_t(f)]; }
    void set_field(PricingField f, const PricingSettings &from);
    void clear_field(PricingField f) { set[size_t(f)] = false; }
    void clear() { *this = ProjectPricing(); }

    // The project's own fields over `defaults`.
    PricingSettings resolve(const PricingSettings &defaults) const;

    bool operator==(const ProjectPricing &rhs) const;
    bool operator!=(const ProjectPricing &rhs) const { return !(*this == rhs); }
};

// JSON of the 3MF metadata "edgeslicer_pricing": {"v":1, "markup":{...}, "assembly_hours":..., ...},
// only the project's own fields. "" when the project has none.
std::string pricing_to_json(const ProjectPricing &pricing);
// False (and `out` cleared) when the text is not a pricing object. Invalid values are skipped.
bool        pricing_from_json(const std::string &text, ProjectPricing &out);
// The same JSON shape for every field (your defaults in the cost store).
std::string pricing_settings_to_json(const PricingSettings &settings);
bool        pricing_settings_from_json(const std::string &text, PricingSettings &out);

// The 3MF model metadata name.
extern const char *const PRICING_METADATA_KEY;

// What a plate holds, for the per-object and per-part fees.
struct PlateCounts
{
    size_t objects{0};   // printable instances on the plate
    size_t parts{0};     // model parts of those instances
};

// Counts the printable instances `on_plate(object index, instance index)` says are on the plate,
// and their model parts (see the top of this file).
PlateCounts count_plate_items(const Model &model, const std::function<bool(int, int)> &on_plate);

struct PricedCost
{
    PricingSettings settings;

    size_t plates{1};
    size_t objects{0};
    size_t parts{0};
    double assembly_hours{0.};   // hours charged (summed over plates)

    // Cents, each line rounded; the sums are sums of the rounded lines.
    double material{0.};
    double machine{0.};
    double assembly{0.};
    double object_fees{0.};
    double part_fees{0.};
    double setup_fees{0.};
    double packaging{0.};
    double fees{0.};             // assembly + object + part + setup + packaging
    double total_cost{0.};       // material + machine + fees
    double markup{0.};
    double selling_price{0.};    // total_cost + markup

    bool has_markup() const { return settings.has_markup(); }
};

// One plate: its breakdown (display figures), what is on it, and the pricing that applies.
PricedCost price_plate(const CostBreakdown &cost, const PlateCounts &counts, const PricingSettings &settings);
// Plates added line by line. The settings are the first plate's (one project, one pricing).
PricedCost sum_priced(const std::vector<PricedCost> &plates);

} // namespace Slic3r
