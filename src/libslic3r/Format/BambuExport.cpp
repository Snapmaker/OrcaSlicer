#include "BambuExport.hpp"
#include "BambuKeyAliases.hpp"
#include "../BambuDualNozzleSync.hpp"

#include "../Config.hpp"
#include "../PrintConfig.hpp"
#include "../Preset.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace Slic3r {
namespace BambuExport {

namespace {

// ---------------------------------------------------------------------------------------------
// Static tables that are a judgement call rather than something the generator can derive.
// ---------------------------------------------------------------------------------------------

// Keys Bambu spells differently (infill_anchor -> sparse_infill_anchor, ...) and enum values whose
// enumerator names differ between the two code bases, so the generator cannot pair them, live in
// Format/BambuKeyAliases.cpp: the same table drives the import direction (handle_legacy), so a
// Bambu project that round-trips through us keeps these settings.

// Options whose "%" is relative to something our ConfigOptionDef does not name in ratio_over.
const std::pair<const char *, const char *> EXTRA_RATIO_OVER[] = {
    { "seam_gap", "nozzle_diameter" },
};

// Lists Bambu reads differently when present-but-empty than when absent. Bambu Studio's CLI
// derives extruder_nozzle_stats from extruder_max_nozzle_count + nozzle_volume_type only when the
// key is missing (BambuStudio.cpp, "has_extruder_nozzle_stats"); an empty list means "no nozzles"
// and every multi-extruder slice then fails ("Failed slicing the model"). We never fill it.
const std::set<std::string> OMIT_WHEN_EMPTY = {
    "extruder_nozzle_stats", "extruder_nozzle_stats_new",
};

// Keys that describe the per-extruder-variant layout; convert_project() writes them from the
// computed layout instead of copying ours.
const std::set<std::string> LAYOUT_KEYS = {
    "print_extruder_variant", "print_extruder_id",
    "printer_extruder_variant", "printer_extruder_id",
    "filament_extruder_variant", "filament_self_index",
};

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

const std::unordered_map<std::string, const BambuKeyDef *> &key_index()
{
    static const std::unordered_map<std::string, const BambuKeyDef *> index = [] {
        std::unordered_map<std::string, const BambuKeyDef *> m;
        for (const BambuKeyDef &d : bambu_key_defs())
            m.emplace(d.key, &d);
        return m;
    }();
    return index;
}

bool is_renamed(const std::string &our_key) { return BambuKeyAliases::by_ours(our_key) != nullptr; }

bool parse_number(const std::string &s_in, double &out, bool allow_percent = false, bool *is_percent = nullptr)
{
    std::string s = boost::algorithm::trim_copy(s_in);
    bool percent = false;
    if (allow_percent && ! s.empty() && s.back() == '%') {
        percent = true;
        s.pop_back();
    }
    if (s.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || ! std::isfinite(v))
        return false;
    out = v;
    if (is_percent)
        *is_percent = percent;
    return true;
}

bool is_integer(const std::string &s_in)
{
    const std::string s = boost::algorithm::trim_copy(s_in);
    if (s.empty())
        return false;
    char *end = nullptr;
    errno = 0;
    std::strtoll(s.c_str(), &end, 10);
    return end != s.c_str() && *end == '\0' && errno != ERANGE;
}

std::string format_number(double v)
{
    // %.10g keeps a hand-entered 0.42 as "0.42" while avoiding binary noise like 0.41999999999.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string join(const std::vector<std::string> &v, const char *sep = ",")
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

// Bambu type name -> (base kind, is vector).
struct TypeInfo { std::string base; bool vector = false; };
TypeInfo type_info(const std::string &type)
{
    static const std::unordered_map<std::string, TypeInfo> m = {
        { "coFloat", { "Float", false } },           { "coFloats", { "Float", true } },
        { "coInt", { "Int", false } },               { "coInts", { "Int", true } },
        { "coString", { "String", false } },         { "coStrings", { "String", true } },
        { "coPercent", { "Percent", false } },       { "coPercents", { "Percent", true } },
        { "coFloatOrPercent", { "FloatOrPercent", false } }, { "coFloatsOrPercents", { "FloatOrPercent", true } },
        { "coPoint", { "Point", false } },           { "coPoints", { "Point", true } },
        { "coPoint3", { "Point3", false } },
        { "coBool", { "Bool", false } },             { "coBools", { "Bool", true } },
        { "coEnum", { "Enum", false } },             { "coEnums", { "Enum", true } },
    };
    auto it = m.find(type);
    if (it != m.end())
        return it->second;
    // Unknown / group types (coPointsGroups, coIntsGroups ...): vectors of opaque elements.
    return { "Opaque", boost::algorithm::ends_with(type, "s") };
}

std::set<std::string> split_enum_values(const char *joined)
{
    std::set<std::string> out;
    if (! joined)
        return out;
    std::vector<std::string> parts;
    std::string s(joined);
    boost::algorithm::split(parts, s, [](char c) { return c == '|'; });
    out.insert(parts.begin(), parts.end());
    return out;
}

const std::set<std::string> &accepted_enum_values(const BambuKeyDef &def)
{
    static std::unordered_map<std::string, std::set<std::string>> cache;
    static std::mutex                                             mutex;
    std::lock_guard<std::mutex>                                   lock(mutex);
    auto it = cache.find(def.key);
    if (it == cache.end())
        it = cache.emplace(def.key, split_enum_values(def.enum_values)).first;
    return it->second;
}

// Read one of our options as a Value, the way save_to_json / opt_serialize would write it.
Value read_value(const ConfigOption &opt)
{
    Value v;
    if (opt.is_scalar()) {
        v.vector  = false;
        v.strings = opt.type() == coString;
        v.values  = { v.strings ? static_cast<const ConfigOptionString &>(opt).value : opt.serialize() };
    } else {
        v.vector  = true;
        v.strings = opt.type() == coStrings;
        v.values  = static_cast<const ConfigOptionVectorBase &>(opt).vserialize();
    }
    return v;
}

// The numeric base a relative ("%") value of `our_key` is measured against, slot `slot`.
bool ratio_base(const std::string &our_key, const ConfigBase &cfg, const Context &ctx, size_t slot, double &base)
{
    std::string over;
    if (const ConfigOptionDef *def = print_config_def.get(our_key))
        over = def->ratio_over;
    for (const auto &extra : EXTRA_RATIO_OVER)
        if (our_key == extra.first)
            over = extra.second;
    if (over.empty())
        return false;
    // An override in the config being converted (a per-object outer_wall_speed) wins over the project.
    std::vector<double> values;
    if (const ConfigOption *opt = cfg.option(over)) {
        for (const std::string &s : read_value(*opt).values) {
            double d;
            if (parse_number(s, d))
                values.push_back(d);
        }
    }
    if (values.empty()) {
        auto it = ctx.numbers.find(over);
        if (it != ctx.numbers.end())
            values = it->second;
    }
    if (values.empty())
        return false;
    base = values[std::min(slot, values.size() - 1)];
    return base > 0.;
}

// ---------------------------------------------------------------------------------------------
// The conversion of one key
// ---------------------------------------------------------------------------------------------

struct KeyResult
{
    bool        keep = false;
    std::string reason; // why it was dropped, or what was changed
    bool        changed = false;
};

std::string translate_enum(const std::string &bambu_key, const std::string &value, const std::set<std::string> &accepted)
{
    if (accepted.count(value))
        return value;
    for (const BambuKeyAliases::EnumAlias &t : BambuKeyAliases::enum_aliases())
        if (bambu_key == t.key && value == t.ours)
            return t.bambu;
    for (const EnumTranslation &t : generated_enum_translations())
        if (bambu_key == t.key && value == t.ours)
            return t.bambu;
    return {};
}

// Value rewrites tied to a rename, where the two settings are the same idea written differently
// (BambuKeyAliases::Alias::to_bambu; handle_legacy applies the inverse on load).
bool rename_value(const std::string &our_key, Value &v, KeyResult &r)
{
    const BambuKeyAliases::Alias *alias = BambuKeyAliases::by_ours(our_key);
    if (alias == nullptr || alias->to_bambu == nullptr)
        return true;
    if (v.values.empty()) {
        r.reason = "empty value";
        return false;
    }
    for (std::string &s : v.values) {
        const std::string before = s;
        if (! alias->to_bambu(s)) {
            r.reason = alias->drop_reason ? alias->drop_reason : "value \"" + before + "\" has no Bambu Studio equivalent";
            return false;
        }
        r.changed |= s != before;
    }
    return true;
}

// Reshape `v` (ours) into Bambu's type for `def`. Returns false (with r.reason) when it cannot.
bool fit_type(const std::string &our_key, const BambuKeyDef &def, const ConfigBase &cfg, const Context &ctx, Value &v, KeyResult &r)
{
    const TypeInfo ti = type_info(def.type);

    // --- vector <-> scalar --------------------------------------------------------------------
    if (v.vector && ! ti.vector) {
        if (v.values.empty()) {
            r.reason = "empty list";
            return false;
        }
        const bool uniform = std::all_of(v.values.begin(), v.values.end(), [&](const std::string &s) { return s == v.values.front(); });
        v.values.resize(1);
        v.vector = false;
        r.changed = true;
        if (! uniform)
            r.reason = "per-variant list collapsed to its first slot (Bambu has a single value)";
    } else if (! v.vector && ti.vector) {
        v.vector  = true;
        r.changed = true;
    }
    v.strings = ti.base == "String";

    // --- element conversions -----------------------------------------------------------------
    const std::set<std::string> *accepted = ti.base == "Enum" ? &accepted_enum_values(def) : nullptr;
    for (size_t i = 0; i < v.values.size(); ++i) {
        std::string &s = v.values[i];
        if (s == "nil") {
            if (def.nullable && ti.vector)
                continue;
            r.reason = "\"nil\" in an option Bambu does not allow to be empty";
            return false;
        }
        double d = 0.;
        bool   pct = false;
        if (ti.base == "Float") {
            if (! parse_number(s, d, true, &pct)) {
                r.reason = "value \"" + s + "\" is not a number";
                return false;
            }
            if (pct) {
                double base;
                if (! ratio_base(our_key, cfg, ctx, i, base)) {
                    r.reason = "relative value \"" + s + "\" has nothing to resolve against";
                    return false;
                }
                s = format_number(d * base / 100.);
                r.changed = true;
            }
        } else if (ti.base == "Int") {
            if (! is_integer(s)) {
                if (! parse_number(s, d)) {
                    r.reason = "value \"" + s + "\" is not a number";
                    return false;
                }
                s = format_number(std::round(d));
                r.changed = true;
            }
        } else if (ti.base == "Percent") {
            if (! parse_number(s, d, true, &pct)) {
                r.reason = "value \"" + s + "\" is not a number";
                return false;
            }
            if (! pct) {
                // Ours is absolute ("mm or %"); Bambu only takes a percentage of the same base.
                double base;
                if (d == 0.)
                    s = "0%";
                else if (ratio_base(our_key, cfg, ctx, i, base))
                    s = format_number(d * 100. / base) + "%";
                else {
                    r.reason = "absolute value \"" + s + "\" has nothing to express it relative to";
                    return false;
                }
                r.changed = true;
            }
        } else if (ti.base == "FloatOrPercent") {
            if (! parse_number(s, d, true, &pct)) {
                r.reason = "value \"" + s + "\" is not a number";
                return false;
            }
        } else if (ti.base == "Bool") {
            if (s == "true")  { s = "1"; r.changed = true; }
            if (s == "false") { s = "0"; r.changed = true; }
            if (s != "0" && s != "1") {
                r.reason = "value \"" + s + "\" is not a boolean";
                return false;
            }
        } else if (ti.base == "Enum") {
            if (accepted->empty())
                continue; // no list to check against (should not happen for a real enum)
            const std::string t = translate_enum(def.key, s, *accepted);
            if (t.empty()) {
                r.reason = "value \"" + s + "\" does not exist in Bambu Studio";
                return false;
            }
            if (t != s) {
                s = t;
                r.changed = true;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Variant layout
// ---------------------------------------------------------------------------------------------

std::vector<std::string> list_of(const ConfigBase &cfg, const Context *ctx, const char *key)
{
    if (const ConfigOption *opt = cfg.option(key))
        return read_value(*opt).values;
    if (ctx) {
        auto it = ctx->lists.find(key);
        if (it != ctx->lists.end())
            return it->second;
    }
    return {};
}

Layout make_layout(const std::vector<std::string> &names, const std::vector<std::string> &ids)
{
    Layout l;
    l.names = names;
    for (size_t i = 0; i < names.size(); ++i) {
        double d = 0.;
        l.ids.push_back(i < ids.size() && parse_number(ids[i], d) ? int(d) : 1);
    }
    return l;
}

bool covers_extruders(const Layout &l, size_t extruders)
{
    for (size_t e = 1; e <= extruders; ++e)
        if (std::find(l.ids.begin(), l.ids.end(), int(e)) == l.ids.end())
            return false;
    return true;
}

// Which slot of our vector (length n, laid out as `ours`) feeds slot j of the Bambu layout.
size_t source_slot(size_t j, size_t n, const Layout &ours, const Layout &target, size_t per_group, const Context &ctx, bool print_flow)
{
    if (n <= 1)
        return 0;
    const std::string &name = target.names[j];
    const int          id   = target.ids[j];
    if (n == ours.size()) {
        for (size_t k = 0; k < ours.size(); ++k)
            if (ours.names[k] == name && ours.ids[k] == id)
                return k;
        for (size_t k = 0; k < ours.size(); ++k)
            if (ours.ids[k] == id)
                return k;
        for (size_t k = 0; k < ours.size(); ++k)
            if (ours.names[k] == name)
                return k;
    }
    if (per_group > 1 && n == per_group && id >= 1)
        return std::min(size_t(id - 1), n - 1);
    if (print_flow && n == ctx.process_flow_support.size()) {
        // Ours: one slot per flow type (process_flow_support); Bambu: one per extruder variant.
        const bool high_flow = name.find("High Flow") != std::string::npos;
        for (size_t k = 0; k < n; ++k)
            if (high_flow == (ctx.process_flow_support[k] == "high_flow"))
                return k;
        return 0;
    }
    return std::min(j, n - 1);
}

// Bambu variant names that slice our High Flow column: "... High Flow" and, owner decision D6,
// "... E3D High Flow". "... TPU High Flow" slices Standard.
bool slices_high_flow_column(const std::string &variant_name)
{
    return variant_name.find("High Flow") != std::string::npos && variant_name.find("TPU High Flow") == std::string::npos;
}

// Our full config packs a flow-variant filament option per filament: filament f owns
// filament_flow_step_size[f] consecutive slots, named by the packed filament_flow_support
// ("standard", "high_flow"). False when the project carries no such table or `n` is not its total.
bool packed_filament_segments(const Context &ctx, size_t n, size_t filament_count, std::vector<size_t> &starts,
                              std::vector<size_t> &sizes)
{
    auto it = ctx.numbers.find("filament_flow_step_size");
    if (it == ctx.numbers.end() || it->second.size() != filament_count)
        return false;
    size_t total = 0;
    starts.clear();
    sizes.clear();
    for (double d : it->second) {
        const size_t sz = size_t(std::max(1., d));
        starts.push_back(total);
        sizes.push_back(sz);
        total += sz;
    }
    return total == n;
}

void fit_layout(Value &v, VariantClass cls, const Context &ctx, const Layout &ours_print, const Layout &ours_printer,
                const Layout &ours_filament, const Layout &target_print, const Layout &target_printer, const Layout &target_filament, size_t filament_count, bool &changed,
                bool packed_flow_variant = false)
{
    if (! v.vector || v.values.empty())
        return;
    const std::vector<std::string> in = v.values;
    const size_t                   n  = in.size();
    std::vector<std::string>       out;
    switch (cls) {
    case VariantClass::Print:
        for (size_t j = 0; j < target_print.size(); ++j)
            out.push_back(in[source_slot(j, n, ours_print, target_print, 0, ctx, true)]);
        break;
    case VariantClass::Printer:
        for (size_t j = 0; j < target_printer.size(); ++j)
            out.push_back(in[source_slot(j, n, ours_printer, target_printer, ctx.extruder_count, ctx, false)]);
        break;
    case VariantClass::PrinterPair: {
        // (normal, silent) pairs, variant-major: Bambu's get_index_for_extruder(..., stride 2).
        const size_t pairs = std::max<size_t>(1, n / 2);
        for (size_t j = 0; j < target_printer.size(); ++j) {
            const size_t p = source_slot(j, pairs, ours_printer, target_printer, ctx.extruder_count, ctx, false);
            for (size_t sub = 0; sub < 2; ++sub)
                out.push_back(in[std::min(p * 2 + sub, n - 1)]);
        }
        break;
    }
    case VariantClass::Filament: {
        std::vector<size_t> starts, sizes;
        if (packed_flow_variant && packed_filament_segments(ctx, n, filament_count, starts, sizes)) {
            // A flow-variant option of our full config: per filament a Standard slot and, when the
            // preset declares one, a High Flow slot (filament_flow_support). Bambu's High Flow (and
            // E3D High Flow) variants take the High Flow slot, every other variant the Standard one.
            // With mixed step sizes the plain "n / filament_count" grouping below would read another
            // filament's slot.
            const auto support_it = ctx.lists.find("filament_flow_support");
            for (size_t j = 0; j < target_filament.size(); ++j) {
                const size_t f   = std::min(size_t(std::max(1, target_filament.ids[j]) - 1), starts.size() - 1);
                size_t       src = starts[f];
                if (slices_high_flow_column(target_filament.names[j]) && support_it != ctx.lists.end())
                    for (size_t k = starts[f]; k < starts[f] + sizes[f] && k < support_it->second.size(); ++k)
                        if (support_it->second[k] == FLOW_MODE_HIGH_FLOW) { src = k; break; }
                out.push_back(in[std::min(src, n - 1)]);
            }
            break;
        }
        // Bambu: one slot per (filament, variant), filament_self_index names the filament.
        // Ours: our full config keeps one slot per filament (or, for a preset with several
        // variants, g consecutive slots per filament); within a filament's group match the
        // variant by name. Our own filament_self_index is not trusted: the fork concatenates
        // each preset's [1] into [1,1,1,...].
        const size_t g = (filament_count > 0 && n % filament_count == 0) ? n / filament_count : 0;
        for (size_t j = 0; j < target_filament.size(); ++j) {
            const size_t f   = size_t(std::max(1, target_filament.ids[j]) - 1);
            size_t       src = std::min(j, n - 1);
            if (g >= 1) {
                src = std::min(f * g, n - 1);
                if (ours_filament.size() == n)
                    for (size_t k = f * g; k < std::min(n, f * g + g); ++k)
                        if (ours_filament.names[k] == target_filament.names[j]) { src = k; break; }
            }
            out.push_back(in[src]);
        }
        break;
    }
    case VariantClass::Extruder:
        for (size_t e = 0; e < ctx.extruder_count; ++e)
            out.push_back(in[std::min(e, n - 1)]);
        break;
    case VariantClass::None:
        return;
    }
    if (out.empty())
        return;
    if (out != in)
        changed = true;
    v.values = std::move(out);
}

Value strings_value(const std::vector<std::string> &s) { Value v; v.vector = true; v.strings = true; v.values = s; return v; }
Value ints_value(const std::vector<int> &ids)
{
    Value v;
    v.vector = true;
    for (int i : ids)
        v.values.push_back(std::to_string(i));
    return v;
}

Layout filament_layout(const Layout &printer, size_t filament_count)
{
    // One slot per (filament, distinct printer variant), like Bambu's own full config, so that
    // every extruder a filament can be mapped to finds its variant.
    std::vector<std::string> distinct;
    for (const std::string &n : printer.names)
        if (std::find(distinct.begin(), distinct.end(), n) == distinct.end())
            distinct.push_back(n);
    if (distinct.empty())
        distinct.push_back("Direct Drive Standard");
    Layout l;
    for (size_t f = 0; f < std::max<size_t>(1, filament_count); ++f)
        for (const std::string &n : distinct) {
            l.names.push_back(n);
            l.ids.push_back(int(f + 1));
        }
    return l;
}

// Paint penetration (top_color_penetration_layers / bottom_color_penetration_layers). Bambu Studio
// always sets the depth (min 1, default 4 / 3); ours adds 0 = "follow the shell", which Bambu
// cannot represent: written as "0" it would claim no painted depth at all there. So a 0 goes out as
// the depth our own shell settings give (MultiMaterialSegmentation.cpp, compute_layer_color_stat):
// the shell layer count, raised to however many layers of layer_height the shell thickness needs,
// and at least 1 (Bambu's minimum; a 0-layer shell claims nothing here, the closest Bambu can do is
// the surface layer). A value >= 1 is Bambu's own and was already written as it is.
//
// An object that overrides its shells (or layer height) but not the penetration would otherwise
// take the project's converted depth in Bambu Studio, computed from the PROJECT's shells, so it
// gets its own when the project follows the shell too.
double number_of(const ConfigBase &cfg, const Context &ctx, const char *key, double fallback)
{
    if (const ConfigOption *opt = cfg.option(key)) {
        const Value v = read_value(*opt);
        double      d;
        if (! v.values.empty() && parse_number(v.values.front(), d))
            return d;
    }
    if (auto it = ctx.numbers.find(key); it != ctx.numbers.end() && ! it->second.empty())
        return it->second.front();
    return fallback;
}

void write_paint_penetration(const ConfigBase &cfg, const Context &ctx, Scope scope, Config &out, Report &local, const std::string &where)
{
    if (scope != Scope::Project && scope != Scope::Print && scope != Scope::Object)
        return;
    for (const bool top : { true, false }) {
        const char *key        = top ? "top_color_penetration_layers" : "bottom_color_penetration_layers";
        const char *layers_key = top ? "top_shell_layers" : "bottom_shell_layers";
        const char *thick_key  = top ? "top_shell_thickness" : "bottom_shell_thickness";
        if (cfg.has(key)) {
            if (number_of(cfg, ctx, key, 0.) >= 1.)
                continue; // an explicit depth, already written as it is
        } else {
            if (scope != Scope::Object || ! (cfg.has(layers_key) || cfg.has(thick_key) || cfg.has("layer_height")))
                continue;
            if (number_of(cfg, ctx, key, 0.) >= 1.)
                continue; // the object inherits the project's explicit depth, in Bambu Studio too
        }
        const int    layers    = int(std::lround(number_of(cfg, ctx, layers_key, 0.)));
        const double thickness = number_of(cfg, ctx, thick_key, 0.);
        const double height    = number_of(cfg, ctx, "layer_height", 0.);
        int          depth     = layers;
        if (layers > 0 && thickness > 0. && height > 0.)
            depth = std::max(depth, int(std::ceil((thickness - 1e-4) / height)));
        depth = std::max(depth, 1);
        Value v;
        v.values = { std::to_string(depth) };
        out[key] = v;
        local.converted.insert(key);
        local.notes.push_back(where + ": " + key + " 0 (follow the shell) written as " + std::to_string(depth) +
                              ", the depth the " + (top ? "top" : "bottom") + " shell settings give");
    }
}

// The core: convert every key of `cfg`.
Config convert_impl(const ConfigBase &cfg, const Context &ctx, Scope scope, Report &report, const std::string &where)
{
    Config out;
    Report local;

    // Our layout, from this config or the project.
    const Layout ours_print   = make_layout(list_of(cfg, &ctx, "print_extruder_variant"), list_of(cfg, &ctx, "print_extruder_id"));
    const Layout ours_printer = make_layout(list_of(cfg, &ctx, "printer_extruder_variant"), list_of(cfg, &ctx, "printer_extruder_id"));
    const Layout ours_filament = make_layout(list_of(cfg, &ctx, "filament_extruder_variant"), {});
    const size_t filament_count = scope == Scope::Filament ? 1 : ctx.filament_count;
    const Layout target_filament = scope == Scope::Filament ? filament_layout(ctx.printer, 1) : ctx.filament;

    std::vector<std::pair<std::string, std::string>> dropped; // our key, reason

    for (const std::string &our_key : cfg.keys()) {
        if (LAYOUT_KEYS.count(our_key) && scope != Scope::Object)
            continue; // written from the computed layout below
        const ConfigOption *opt = cfg.option(our_key);
        if (! opt)
            continue;
        std::string bkey = bambu_key_name(our_key);
        if (bkey != our_key && cfg.has(bkey))
            bkey = our_key; // both spellings present: the direct key wins, this one is dropped below
        const BambuKeyDef *def = (bkey != our_key || ! is_renamed(our_key)) ? find_key(bkey) : nullptr;
        if (! def) {
            dropped.emplace_back(our_key, "not a Bambu Studio setting");
            continue;
        }
        Value     v = read_value(*opt);
        if (v.vector && v.values.empty() && OMIT_WHEN_EMPTY.count(bkey)) {
            local.notes.push_back(where + ": " + bkey + " is empty; left out so Bambu Studio derives it from the printer");
            continue;
        }
        KeyResult r;
        if (bkey != our_key && ! rename_value(our_key, v, r)) {
            dropped.emplace_back(our_key, r.reason);
            continue;
        }
        if (! fit_type(our_key, *def, cfg, ctx, v, r)) {
            dropped.emplace_back(our_key, r.reason);
            continue;
        }
        bool layout_changed = false;
        if (scope != Scope::Object || def->variant != VariantClass::Filament) {
            fit_layout(v, def->variant, ctx, ours_print, ours_printer, ours_filament, ctx.print, ctx.printer, target_filament, filament_count, layout_changed,
                       scope == Scope::Project && is_filament_flow_variant_option(our_key));
        } else {
            // A per-object override of a filament-variant option is a single value.
            fit_layout(v, def->variant, ctx, ours_print, ours_printer, ours_filament, ctx.print, ctx.printer, filament_layout(ctx.printer, 1), 1, layout_changed);
        }
        if (bkey != our_key) {
            local.renamed[our_key] = bkey;
            local.notes.push_back(where + ": " + our_key + " -> " + bkey);
        }
        if (r.changed || layout_changed) {
            local.converted.insert(bkey);
            if (! r.reason.empty())
                local.notes.push_back(where + ": " + bkey + ": " + r.reason);
        }
        out[bkey] = std::move(v);
    }

    write_paint_penetration(cfg, ctx, scope, out, local, where);

    // Layout keys.
    if (scope == Scope::Project || scope == Scope::Print) {
        out["print_extruder_variant"] = strings_value(ctx.print.names);
        out["print_extruder_id"]      = ints_value(ctx.print.ids);
    }
    if (scope == Scope::Project || scope == Scope::Printer) {
        out["printer_extruder_variant"] = strings_value(ctx.printer.names);
        out["printer_extruder_id"]      = ints_value(ctx.printer.ids);
    }
    if (scope == Scope::Project || scope == Scope::Filament) {
        out["filament_extruder_variant"] = strings_value(target_filament.names);
        out["filament_self_index"]       = ints_value(target_filament.ids);
    }

    // Bambu Studio has one switch, enable_tower_interface_features, for the bundle our three tower
    // interface options split up (GCode/WipeTowerInterface.hpp). It is written from them, never from
    // the Bambu value a loaded file kept: on when any of them is, since Bambu Studio can only turn
    // on the whole bundle. The per-filament values keep Bambu's key names and go out as they are.
    if (scope == Scope::Project || scope == Scope::Print) {
        std::vector<std::string> on;
        bool                     present = false;
        for (const char *key : { "wipe_tower_interface_temp", "wipe_tower_interface_run_in", "wipe_tower_interface_extra_prime" })
            if (const ConfigOption *opt = cfg.option(key); opt != nullptr) {
                present = true;
                if (opt->getBool())
                    on.emplace_back(key);
            }
        if (present) {
            Value v;
            v.values = { on.empty() ? "0" : "1" };
            out["enable_tower_interface_features"] = v;
            if (! on.empty() && on.size() < 3)
                local.notes.push_back(where + ": enable_tower_interface_features written as on although only " + join(on, ", ") +
                                      " is on: Bambu Studio turns on its whole tower interface bundle (interface temperature, run-in, "
                                      "extra prime, interface wall gaps and, on the H2 series, a firmware purge)");
        } else {
            out.erase("enable_tower_interface_features");
        }
    }

    // different_settings_to_system names keys per preset type; keep it in Bambu's vocabulary.
    if (auto it = out.find("different_settings_to_system"); it != out.end()) {
        for (std::string &list : it->second.values) {
            std::vector<std::string> keys, kept;
            boost::algorithm::split(keys, list, [](char c) { return c == ';'; });
            for (const std::string &k : keys) {
                if (k.empty()) continue;
                const std::string bk = bambu_key_name(k);
                if (find_key(bk) && std::find(kept.begin(), kept.end(), bk) == kept.end())
                    kept.push_back(bk);
            }
            list = join(kept, ";");
        }
    }

    for (const auto &d : dropped) {
        local.dropped.insert(d.first);
        if (d.second != "not a Bambu Studio setting")
            local.notes.push_back(where + ": left out " + d.first + ": " + d.second);
    }
    if (! dropped.empty()) {
        std::vector<std::string> names;
        for (const auto &d : dropped)
            names.push_back(d.first);
        BOOST_LOG_TRIVIAL(info) << "Bambu export, " << where << ": " << dropped.size() << " setting(s) left out: " << join(names, ", ");
    }
    for (const std::string &n : local.notes)
        BOOST_LOG_TRIVIAL(info) << "Bambu export, " << n;
    report.merge(local);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

const BambuKeyDef *find_key(const std::string &bambu_key)
{
    auto it = key_index().find(bambu_key);
    return it == key_index().end() ? nullptr : it->second;
}

std::string bambu_key_name(const std::string &our_key)
{
    const BambuKeyAliases::Alias *alias = BambuKeyAliases::by_ours(our_key);
    return alias == nullptr ? our_key : std::string(alias->bambu);
}

std::string export_version()
{
    // "02.08.02.61" -> "02.08.00.00": the schema line the key list targets, without claiming a
    // particular Bambu Studio build. Bambu compares this against its own version: a file from the
    // same major.minor line loads silently, an older Bambu Studio warns that the file is newer.
    std::vector<std::string> parts;
    std::string              ref = bambu_reference_version();
    boost::algorithm::split(parts, ref, [](char c) { return c == '.'; });
    if (parts.size() < 2)
        return ref;
    return parts[0] + "." + parts[1] + ".00.00";
}

std::string application_tag() { return "BambuStudio-" + export_version(); }

void Report::merge(const Report &other)
{
    dropped.insert(other.dropped.begin(), other.dropped.end());
    for (const auto &kv : other.renamed)
        renamed.insert(kv);
    converted.insert(other.converted.begin(), other.converted.end());
    notes.insert(notes.end(), other.notes.begin(), other.notes.end());
}

std::string Report::summary() const
{
    return std::to_string(dropped.size()) + " setting(s) not supported by Bambu Studio left out, " +
           std::to_string(renamed.size()) + " renamed, " + std::to_string(converted.size()) + " converted";
}

Context Context::from_project(const ConfigBase &project)
{
    Context ctx;
    for (const std::string &key : project.keys()) {
        const ConfigOption *opt = project.option(key);
        if (! opt)
            continue;
        const Value v = read_value(*opt);
        if (! v.strings) {
            std::vector<double> nums;
            for (const std::string &s : v.values) {
                double d;
                if (parse_number(s, d, true))
                    nums.push_back(d);
            }
            if (! nums.empty() && nums.size() == v.values.size())
                ctx.numbers[key] = std::move(nums);
        }
    }
    for (const char *key : { "print_extruder_variant", "print_extruder_id", "printer_extruder_variant", "printer_extruder_id",
                             "filament_extruder_variant", "filament_self_index", "extruder_type", "nozzle_volume_type",
                             "filament_flow_support" })
        if (const ConfigOption *opt = project.option(key))
            ctx.lists[key] = read_value(*opt).values;
    if (auto it = ctx.numbers.find("nozzle_diameter"); it != ctx.numbers.end() && ! it->second.empty()) {
        ctx.nozzle_diameter = it->second.front();
        ctx.extruder_count  = it->second.size();
    }
    // One entry per filament in the application's full config; filament_settings_id is the list
    // the per-filament options are merged from, filament_colour the fallback.
    for (const char *key : { "filament_colour", "filament_settings_id" })
        if (const ConfigOption *opt = project.option(key))
            ctx.filament_count = std::max<size_t>(1, read_value(*opt).values.size());
    if (const ConfigOption *opt = project.option("process_flow_support"))
        ctx.process_flow_support = read_value(*opt).values;
    return ctx;
}

Config convert_project(const ConfigBase &project, Context &ctx, Report &report)
{
    // Printer layout: ours when it names every extruder, else one variant per extruder built from
    // extruder_type + nozzle_volume_type, which is how Bambu names variants
    // (get_extruder_variant_string). A single-variant layout on a multi-extruder machine would
    // leave extruders 2+ without a variant: Bambu then logs "could not found extruder_type" and
    // reads the wrong slot.
    Layout ours_printer = make_layout(list_of(project, &ctx, "printer_extruder_variant"), list_of(project, &ctx, "printer_extruder_id"));
    if (ours_printer.size() > 0 && covers_extruders(ours_printer, ctx.extruder_count)) {
        ctx.printer = ours_printer;
    } else {
        const std::vector<std::string> types = list_of(project, &ctx, "extruder_type");
        const std::vector<std::string> nvts  = list_of(project, &ctx, "nozzle_volume_type");
        ctx.printer = Layout();
        for (size_t e = 0; e < ctx.extruder_count; ++e) {
            std::string type = types.empty() ? "Direct Drive" : types[std::min(e, types.size() - 1)];
            std::string nvt  = nvts.empty() ? "Standard" : nvts[std::min(e, nvts.size() - 1)];
            if (nvt == "Hybrid")
                nvt = "Standard"; // Bambu: "nvtHybrid not supported in presets, switch to nvtStandard"
            ctx.printer.names.push_back(type + " " + nvt);
            ctx.printer.ids.push_back(int(e + 1));
        }
    }
    // Print layout: ours when it has a slot for every printer variant, else the printer's layout.
    Layout ours_print = make_layout(list_of(project, &ctx, "print_extruder_variant"), list_of(project, &ctx, "print_extruder_id"));
    bool   print_ok   = ours_print.size() > 0;
    for (size_t j = 0; print_ok && j < ctx.printer.size(); ++j) {
        bool found = false;
        for (size_t k = 0; k < ours_print.size() && ! found; ++k)
            found = ours_print.names[k] == ctx.printer.names[j] && ours_print.ids[k] == ctx.printer.ids[j];
        print_ok = found;
    }
    ctx.print    = print_ok ? ours_print : ctx.printer;
    ctx.filament = filament_layout(ctx.printer, ctx.filament_count);

    Config out = convert_impl(project, ctx, Scope::Project, report, "project");

    // Bambu Studio writes the nozzle stats twice (split_nozzle_stats_for_export, PrintConfig.cpp:912-938):
    // extruder_nozzle_stats_new with the real volume types, and the legacy key with types newer than
    // TPU High Flow folded into Standard for older readers. Its own H2C projects carry both
    // (h2c_wrongextruder_bambu_manual.gcode.3mf: ["Standard#1", "Standard#6"] twice).
    if (auto it = out.find("extruder_nozzle_stats"); it != out.end() && !it->second.values.empty()) {
        out["extruder_nozzle_stats_new"] = it->second;
        auto stats      = get_extruder_nozzle_stats(it->second.values);
        bool downgraded = false;
        for (auto &extruder_stat : stats) {
            std::map<NozzleVolumeType, int> legacy;
            for (const auto &entry : extruder_stat) {
                const NozzleVolumeType legacy_type = entry.first > nvtTPUHighFlow ? nvtStandard : entry.first;
                downgraded |= legacy_type != entry.first;
                legacy[legacy_type] += entry.second;
            }
            extruder_stat = std::move(legacy);
        }
        if (downgraded)
            it->second.values = DualNozzleSync::save_extruder_nozzle_stats_to_string(stats);
    }

    // Bambu Studio refuses to slice ooze prevention together with a prime tower (Print::validate:
    // "Ooze prevention is currently not supported with the prime tower enabled."); this fork
    // supports the combination (tool changers). The prime tower is what a multi-material plate
    // needs, so ooze prevention is the one left out.
    if (auto ooze = out.find("ooze_prevention"), tower = out.find("enable_prime_tower");
        ooze != out.end() && tower != out.end() && !ooze->second.values.empty() && !tower->second.values.empty() &&
        ooze->second.values.front() == "1" && tower->second.values.front() == "1") {
        out.erase(ooze);
        report.dropped.insert("ooze_prevention");
        report.notes.push_back("project: left out ooze_prevention: Bambu Studio cannot combine it with a prime tower");
        BOOST_LOG_TRIVIAL(info) << "Bambu export, project: left out ooze_prevention: Bambu Studio cannot combine it with a prime tower";
    }

    // Flushing is per extruder in Bambu: flush_multiplier has one value per extruder and
    // flush_volumes_matrix one filament x filament matrix per extruder, extruder-major. Ours is a
    // single matrix / multiplier used for every extruder, so repeat it.
    if (ctx.extruder_count > 1) {
        const size_t f2 = ctx.filament_count * ctx.filament_count;
        if (auto it = out.find("flush_volumes_matrix"); it != out.end() && it->second.values.size() == f2) {
            std::vector<std::string> per_extruder;
            for (size_t e = 0; e < ctx.extruder_count; ++e)
                per_extruder.insert(per_extruder.end(), it->second.values.begin(), it->second.values.end());
            it->second.values = std::move(per_extruder);
            report.converted.insert("flush_volumes_matrix");
        }
    }

    // Per-extruder option lists Bambu checks against nozzle_diameter on load (check_project_config
    // in Bambu's Plater: a multi-extruder project whose extruder_type is shorter than
    // nozzle_diameter is loaded geometry-only).
    for (const char *key : { "extruder_type", "nozzle_volume_type", "flush_multiplier" }) {
        auto it = out.find(key);
        if (it == out.end() || ! it->second.vector || it->second.values.empty())
            continue;
        std::vector<std::string> &vals = it->second.values;
        if (vals.size() != ctx.extruder_count) {
            std::vector<std::string> fitted;
            for (size_t e = 0; e < ctx.extruder_count; ++e)
                fitted.push_back(vals[std::min(e, vals.size() - 1)]);
            vals = std::move(fitted);
            report.converted.insert(key);
        }
    }
    return out;
}

Config convert(const ConfigBase &cfg, const Context &ctx, Scope scope, Report &report, const std::string &where)
{
    return convert_impl(cfg, ctx, scope, report, where);
}

std::string to_json(const Config &cfg, const std::string &name, const std::string &from, const std::string &version)
{
    nlohmann::json j;
    j[BBL_JSON_KEY_VERSION] = version;
    j[BBL_JSON_KEY_NAME]    = name;
    j[BBL_JSON_KEY_FROM]    = from;
    for (const auto &kv : cfg) {
        if (kv.second.vector)
            j[kv.first] = kv.second.values;
        else
            j[kv.first] = kv.second.values.empty() ? std::string() : kv.second.values.front();
    }
    return j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::string serialize(const Value &value)
{
    if (! value.vector) {
        const std::string s = value.values.empty() ? std::string() : value.values.front();
        return value.strings ? escape_string_cstyle(s) : s;
    }
    return value.strings ? escape_strings_cstyle(value.values) : join(value.values);
}

std::string translate_enum_value(const std::string &bambu_key, const std::string &our_value)
{
    const BambuKeyDef *def = find_key(bambu_key);
    if (! def || ! def->enum_values)
        return our_value;
    return translate_enum(bambu_key, our_value, accepted_enum_values(*def));
}

} // namespace BambuExport
} // namespace Slic3r
