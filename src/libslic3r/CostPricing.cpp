#include "CostPricing.hpp"

#include "CostEstimate.hpp"
#include "Model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {

using nlohmann::json;

const char *const PRICING_METADATA_KEY = "edgeslicer_pricing";

bool PricingSettings::operator==(const PricingSettings &rhs) const
{
    return markup_type == rhs.markup_type && markup_basis == rhs.markup_basis && markup_value == rhs.markup_value &&
           assembly_hours == rhs.assembly_hours && assembly_rate_per_h == rhs.assembly_rate_per_h &&
           fee_per_object == rhs.fee_per_object && fee_per_part == rhs.fee_per_part && fee_per_plate == rhs.fee_per_plate &&
           packaging_per_plate == rhs.packaging_per_plate;
}

void copy_pricing_field(PricingSettings &to, const PricingSettings &from, PricingField field)
{
    switch (field) {
    case PricingField::Markup:
        to.markup_type  = from.markup_type;
        to.markup_basis = from.markup_basis;
        to.markup_value = from.markup_value;
        break;
    case PricingField::AssemblyHours: to.assembly_hours = from.assembly_hours; break;
    case PricingField::AssemblyRate: to.assembly_rate_per_h = from.assembly_rate_per_h; break;
    case PricingField::ObjectFee: to.fee_per_object = from.fee_per_object; break;
    case PricingField::PartFee: to.fee_per_part = from.fee_per_part; break;
    case PricingField::PlateFee: to.fee_per_plate = from.fee_per_plate; break;
    case PricingField::Packaging: to.packaging_per_plate = from.packaging_per_plate; break;
    case PricingField::Count: break;
    }
}

bool ProjectPricing::empty() const
{
    return std::none_of(set.begin(), set.end(), [](bool b) { return b; });
}

void ProjectPricing::set_field(PricingField f, const PricingSettings &from)
{
    copy_pricing_field(values, from, f);
    set[size_t(f)] = true;
}

PricingSettings ProjectPricing::resolve(const PricingSettings &defaults) const
{
    PricingSettings out = defaults;
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        if (set[i])
            copy_pricing_field(out, values, PricingField(i));
    return out;
}

bool ProjectPricing::operator==(const ProjectPricing &rhs) const
{
    if (set != rhs.set)
        return false;
    // Only the fields that are set count.
    for (size_t i = 0; i < PRICING_FIELDS; ++i) {
        if (!set[i])
            continue;
        PricingSettings a, b;
        copy_pricing_field(a, values, PricingField(i));
        copy_pricing_field(b, rhs.values, PricingField(i));
        if (a != b)
            return false;
    }
    return true;
}

// ------------------------------------------------------------------ JSON ----

static const char *const FIELD_KEYS[PRICING_FIELDS] = {"markup",       "assembly_hours", "assembly_rate_per_h", "fee_per_object",
                                                       "fee_per_part", "fee_per_plate",  "packaging_per_plate"};

static double *number_field(PricingSettings &s, PricingField f)
{
    switch (f) {
    case PricingField::AssemblyHours: return &s.assembly_hours;
    case PricingField::AssemblyRate: return &s.assembly_rate_per_h;
    case PricingField::ObjectFee: return &s.fee_per_object;
    case PricingField::PartFee: return &s.fee_per_part;
    case PricingField::PlateFee: return &s.fee_per_plate;
    case PricingField::Packaging: return &s.packaging_per_plate;
    default: return nullptr;
    }
}

static double number_value(const PricingSettings &s, PricingField f)
{
    PricingSettings copy = s;
    return *number_field(copy, f);
}

static json field_to_json(const PricingSettings &s, PricingField f)
{
    if (f == PricingField::Markup) {
        json m      = json::object();
        m["type"]   = s.markup_type == MarkupType::Flat ? "flat" : "percent";
        m["basis"]  = s.markup_basis == MarkupBasis::Material ? "material" : "overall";
        m["value"]  = s.markup_value;
        return m;
    }
    return number_value(s, f);
}

static bool valid_amount(const json &v, double &out)
{
    if (!v.is_number())
        return false;
    const double d = v.get<double>();
    if (!std::isfinite(d) || d < 0.)
        return false;
    out = d;
    return true;
}

// Reads one field into `s`; false when absent or invalid (then `s` is untouched).
static bool field_from_json(const json &j, PricingField f, PricingSettings &s)
{
    const char *key = FIELD_KEYS[size_t(f)];
    if (!j.contains(key))
        return false;
    const json &v = j[key];
    if (f == PricingField::Markup) {
        if (!v.is_object() || !v.contains("value"))
            return false;
        double value = 0.;
        if (!valid_amount(v["value"], value))
            return false;
        const std::string type  = v.contains("type") && v["type"].is_string() ? v["type"].get<std::string>() : "percent";
        const std::string basis = v.contains("basis") && v["basis"].is_string() ? v["basis"].get<std::string>() : "overall";
        if ((type != "percent" && type != "flat") || (basis != "material" && basis != "overall"))
            return false;
        s.markup_type  = type == "flat" ? MarkupType::Flat : MarkupType::Percent;
        s.markup_basis = basis == "material" ? MarkupBasis::Material : MarkupBasis::Overall;
        s.markup_value = value;
        return true;
    }
    double value = 0.;
    if (!valid_amount(v, value))
        return false;
    *number_field(s, f) = value;
    return true;
}

std::string pricing_to_json(const ProjectPricing &pricing)
{
    if (pricing.empty())
        return {};
    json j = json::object();
    if (!pricing.extra.empty()) {
        try {
            json extra = json::parse(pricing.extra);
            if (extra.is_object())
                for (auto it = extra.begin(); it != extra.end(); ++it)
                    j[it.key()] = it.value();
        } catch (...) {}
    }
    j["v"] = 1;
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        if (pricing.set[i])
            j[FIELD_KEYS[i]] = field_to_json(pricing.values, PricingField(i));
    return j.dump();
}

bool pricing_from_json(const std::string &text, ProjectPricing &out)
{
    out.clear();
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return false;
    }
    if (!j.is_object())
        return false;
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        out.set[i] = field_from_json(j, PricingField(i), out.values);
    json extra = json::object();
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "v" && std::find(std::begin(FIELD_KEYS), std::end(FIELD_KEYS), it.key()) == std::end(FIELD_KEYS))
            extra[it.key()] = it.value();
    if (!extra.empty())
        out.extra = extra.dump();
    return true;
}

std::string pricing_settings_to_json(const PricingSettings &settings)
{
    json j = json::object();
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        j[FIELD_KEYS[i]] = field_to_json(settings, PricingField(i));
    return j.dump();
}

bool pricing_settings_from_json(const std::string &text, PricingSettings &out)
{
    out = PricingSettings();
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return false;
    }
    if (!j.is_object())
        return false;
    for (size_t i = 0; i < PRICING_FIELDS; ++i)
        field_from_json(j, PricingField(i), out);
    return true;
}

// ---------------------------------------------------------------- counts ----

PlateCounts count_plate_items(const Model &model, const std::function<bool(int, int)> &on_plate)
{
    PlateCounts out;
    for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
        const ModelObject *object = model.objects[obj_idx];
        if (object == nullptr || !object->printable)
            continue;
        size_t parts = 0;
        for (const ModelVolume *volume : object->volumes)
            if (volume != nullptr && volume->is_model_part())
                ++parts;
        if (parts == 0)
            continue;
        for (size_t inst_idx = 0; inst_idx < object->instances.size(); ++inst_idx) {
            const ModelInstance *instance = object->instances[inst_idx];
            if (instance == nullptr || !instance->printable || !on_plate(int(obj_idx), int(inst_idx)))
                continue;
            ++out.objects;
            out.parts += parts;
        }
    }
    return out;
}

// ----------------------------------------------------------------- money ----

PricedCost price_plate(const CostBreakdown &cost, const PlateCounts &counts, const PricingSettings &s)
{
    PricedCost out;
    out.settings       = s;
    out.plates         = 1;
    out.objects        = counts.objects;
    out.parts          = counts.parts;
    out.assembly_hours = s.assembly_hours;

    out.material    = cost.prices_known ? cost.display_material : 0.;
    out.machine     = cost.display_machine;
    out.assembly    = round_money(s.assembly_hours * s.assembly_rate_per_h);
    out.object_fees = round_money(double(counts.objects) * s.fee_per_object);
    out.part_fees   = round_money(double(counts.parts) * s.fee_per_part);
    out.setup_fees  = round_money(s.fee_per_plate);
    out.packaging   = round_money(s.packaging_per_plate);
    out.fees        = out.assembly + out.object_fees + out.part_fees + out.setup_fees + out.packaging;
    out.total_cost  = out.material + out.machine + out.fees;

    if (s.has_markup()) {
        if (s.markup_type == MarkupType::Flat)
            out.markup = round_money(s.markup_value);
        else
            out.markup = round_money((s.markup_basis == MarkupBasis::Material ? out.material : out.total_cost) * s.markup_value / 100.);
    }
    out.selling_price = out.total_cost + out.markup;
    return out;
}

PricedCost sum_priced(const std::vector<PricedCost> &plates)
{
    PricedCost out;
    out.plates = 0;
    if (plates.empty())
        return out;
    out.settings = plates.front().settings;
    for (const PricedCost &p : plates) {
        out.plates += p.plates;
        out.objects += p.objects;
        out.parts += p.parts;
        out.assembly_hours += p.assembly_hours;
        out.material += p.material;
        out.machine += p.machine;
        out.assembly += p.assembly;
        out.object_fees += p.object_fees;
        out.part_fees += p.part_fees;
        out.setup_fees += p.setup_fees;
        out.packaging += p.packaging;
        out.fees += p.fees;
        out.total_cost += p.total_cost;
        out.markup += p.markup;
        out.selling_price += p.selling_price;
    }
    return out;
}

} // namespace Slic3r
