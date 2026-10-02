#include "BambuKeyAliases.hpp"
#include "BambuExport.hpp"

#include "../Config.hpp"
#include "../PrintConfig.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace Slic3r {
namespace BambuKeyAliases {

namespace {

// ---------------------------------------------------------------------------------------------
// Value conversions (one element each). Where the two settings are the same idea written
// differently; a pure rename has none.
// ---------------------------------------------------------------------------------------------

// only_one_wall_top (bool) <-> top_one_wall_type ("not apply" | "all top" | "topmost").
bool one_wall_top_to_bambu(std::string &v) { v = v == "1" ? "all top" : "not apply"; return true; }
bool one_wall_top_from_bambu(std::string &v)
{
    // "none" is not a Bambu value; older files that carry it keep our own value, as before.
    if (v == "none")
        return false;
    // Lossy: Bambu's "topmost" (only the topmost surface gets one wall) is the closest to ours = on.
    v = v == "not apply" ? "0" : "1";
    return true;
}

// reduce_infill_retraction (bool) <-> reduce_infill_retraction_mode ("Disabled" | "Auto" | "Enabled").
bool reduce_retraction_to_bambu(std::string &v) { v = v == "1" ? "Enabled" : "Disabled"; return true; }
bool reduce_retraction_from_bambu(std::string &v)
{
    // "Auto" decides per filament (on for low metal stickiness such as PLA, off for PETG); there
    // is no single bool that means that, so ours keeps its own value.
    if (v == "Enabled")  { v = "1"; return true; }
    if (v == "Disabled") { v = "0"; return true; }
    return false;
}

// wipe_tower_wall_type ("rectangle" | "cone" | "rib") <-> prime_tower_rib_wall (bool).
// Lossy on export: Bambu has no cone tower, a cone goes out as a plain (non-rib) wall.
bool tower_wall_to_bambu(std::string &v) { v = v == "rib" ? "1" : "0"; return true; }
bool tower_wall_from_bambu(std::string &v)
{
    if (v == "1") { v = "rib"; return true; }
    if (v == "0") { v = "rectangle"; return true; }
    return false;
}

// wipe_tower_wall_gap <-> prime_tower_skip_points: the same wall gaps at the start of a purge. Read
// back from anything but the bundled vendor presets: 9 Flashforge Creator 5 process presets carry
// Bambu's "0", and their shipped towers print with our default (gaps on); see
// SystemPresetTowerKeysScope.
bool wall_gap_from_bambu(std::string &) { return ! SystemPresetTowerKeysScope::active(); }

// ironing_angle: ours uses a negative angle for "use the default method". Bambu has no such value;
// leaving the key out gives Bambu its own default, which is the same intent.
bool ironing_angle_to_bambu(std::string &v)
{
    const std::string s   = boost::algorithm::trim_copy(v);
    char             *end = nullptr;
    errno                 = 0;
    const double d        = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && errno != ERANGE && std::isfinite(d) && d >= 0.;
}

// filament_colour_mode (0 = split colours, 1 = gradient) <-> filament_colour_type
// (0 = gradient, 1 = default: single or split colours).
bool colour_mode_to_bambu(std::string &v) { v = v == "1" ? "0" : "1"; return true; }
bool colour_mode_from_bambu(std::string &v)
{
    if (v == "0") { v = "1"; return true; }
    if (v == "1") { v = "0"; return true; }
    return false;
}

// filament_multi_colors ("#RRGGBB|#RRGGBB") <-> filament_multi_colour ("#RRGGBB #RRGGBB").
bool split_and_join(std::string &v, bool (*is_sep)(char), const char *sep)
{
    std::vector<std::string> parts, kept;
    boost::algorithm::split(parts, v, is_sep);
    for (std::string &p : parts) {
        boost::algorithm::trim(p);
        if (! p.empty())
            kept.push_back(p);
    }
    v = boost::algorithm::join(kept, sep);
    return true;
}
bool multi_colours_to_bambu(std::string &v) { return split_and_join(v, [](char c) { return c == '|'; }, " "); }
bool multi_colours_from_bambu(std::string &v) { return split_and_join(v, [](char c) { return c == ' ' || c == '\t'; }, "|"); }

// ---------------------------------------------------------------------------------------------
// The table. Order does not matter; every name appears once on each side (tested).
// ---------------------------------------------------------------------------------------------
std::vector<Alias> make_aliases()
{
    return {
    // Bambu Studio renamed these after Orca forked (Orca's handle_legacy already read them back).
    { "infill_anchor",                  "sparse_infill_anchor",                 nullptr, nullptr, nullptr },
    { "infill_anchor_max",              "sparse_infill_anchor_max",             nullptr, nullptr, nullptr },
    { "chamber_temperature",            "chamber_temperatures",                 nullptr, nullptr, nullptr },
    { "bottom_solid_infill_flow_ratio", "initial_layer_flow_ratio",             nullptr, nullptr, nullptr },
    { "ironing_angle",                  "ironing_direction",                    ironing_angle_to_bambu, nullptr,
      "negative ironing angle means \"default\" here; Bambu keeps its own default" },
    { "only_one_wall_top",              "top_one_wall_type",                    one_wall_top_to_bambu, one_wall_top_from_bambu, nullptr },
    // Same setting, ported under another name by Orca or this fork.
    { "support_ironing",                "enable_support_ironing",               nullptr, nullptr, nullptr },
    { "extruder_clearance_radius",      "extruder_clearance_max_radius",        nullptr, nullptr, nullptr },
    { "dont_slow_down_outer_wall",      "no_slow_down_for_cooling_on_outwalls", nullptr, nullptr, nullptr },
    { "role_based_wipe_speed",          "role_base_wipe_speed",                 nullptr, nullptr, nullptr },
    { "reduce_infill_retraction",       "reduce_infill_retraction_mode",        reduce_retraction_to_bambu, reduce_retraction_from_bambu, nullptr },
    { "wipe_tower_rib_width",           "prime_tower_rib_width",                nullptr, nullptr, nullptr },
    { "wipe_tower_extra_rib_length",    "prime_tower_extra_rib_length",         nullptr, nullptr, nullptr },
    { "wipe_tower_fillet_wall",         "prime_tower_fillet_wall",              nullptr, nullptr, nullptr },
    { "wipe_tower_wall_type",           "prime_tower_rib_wall",                 tower_wall_to_bambu, tower_wall_from_bambu, nullptr },
    { "wipe_tower_max_purge_speed",     "prime_tower_max_speed",                nullptr, nullptr, nullptr },
    { "wipe_tower_wall_gap",            "prime_tower_skip_points",              nullptr, wall_gap_from_bambu, nullptr },
    // Found auditing Bambu Studio's PrintConfig.cpp (2026-09-22): same meaning, same unit.
    { "lateral_lattice_angle_1",        "sparse_infill_lattice_angle_1",        nullptr, nullptr, nullptr },
    { "lateral_lattice_angle_2",        "sparse_infill_lattice_angle_2",        nullptr, nullptr, nullptr },
    { "notes",                          "process_notes",                        nullptr, nullptr, nullptr },
    { "filament_colour_mode",           "filament_colour_type",                 colour_mode_to_bambu, colour_mode_from_bambu, nullptr },
    { "filament_multi_colors",          "filament_multi_colour",                multi_colours_to_bambu, multi_colours_from_bambu, nullptr },
    };
}

// Our "Lateral Lattice" is Bambu's "2D Lattice" (same FillRectilinear-derived pattern, same
// lattice angle options): every pattern key Bambu accepts it in.
std::vector<EnumAlias> make_enum_aliases()
{
    std::vector<EnumAlias> v = {
        // Orca's four-level ensure_vertical_shell_thickness vs Bambu's three levels.
        { "ensure_vertical_shell_thickness", "none",                 "disabled", true },
        { "ensure_vertical_shell_thickness", "ensure_critical_only", "partial",  false },
        { "ensure_vertical_shell_thickness", "ensure_moderate",      "partial",  true },
        { "ensure_vertical_shell_thickness", "ensure_all",           "enabled",  true },
        // Front/Left/Right are Aligned biased toward one side; Bambu Studio has no such concept, so on
        // export they fall back to its closest equivalent, plain Aligned (Aligned back has no
        // Bambu equivalent either, but is handled by the generated table, which maps it to "").
        // Not reversible (import=false): loading a Bambu project that says "aligned" must stay
        // "aligned" for us, not turn into "left".
        { "seam_position", "left",          "aligned", false },
        { "seam_position", "right",         "aligned", false },
        { "seam_position", "aligned_front", "aligned", false },
    };
    for (const char *key : { "sparse_infill_pattern", "top_surface_pattern", "bottom_surface_pattern", "internal_solid_infill_pattern",
                             "ironing_pattern", "support_ironing_pattern", "locked_skin_infill_pattern", "locked_skeleton_infill_pattern" })
        v.push_back({ key, "lateral-lattice", "2dlattice", true });
    return v;
}

template<class Map> const Alias *find_in(const Map &m, const std::string &key)
{
    auto it = m.find(key);
    return it == m.end() ? nullptr : it->second;
}

const std::unordered_map<std::string, const Alias *> &index_ours()
{
    static const std::unordered_map<std::string, const Alias *> m = [] {
        std::unordered_map<std::string, const Alias *> out;
        for (const Alias &a : aliases())
            out.emplace(a.ours, &a);
        return out;
    }();
    return m;
}

const std::unordered_map<std::string, const Alias *> &index_bambu()
{
    static const std::unordered_map<std::string, const Alias *> m = [] {
        std::unordered_map<std::string, const Alias *> out;
        for (const Alias &a : aliases())
            out.emplace(a.bambu, &a);
        return out;
    }();
    return m;
}

// Apply fn to every element of `value`, parsed and re-serialized the way our option `our_key`
// serializes (scalar: whole value; coStrings: quoted, ';'-separated; other vectors: ',').
bool convert_elements(const std::string &our_key, std::string &value, ValueFn fn)
{
    const ConfigOptionDef *def = print_config_def.get(our_key);
    if (def == nullptr || def->is_scalar())
        return fn(value);
    const bool               strings = def->type == coStrings;
    std::vector<std::string> elems;
    if (value.find('"') != std::string::npos || strings) {
        if (! unescape_strings_cstyle(value, elems))
            return false;
    } else {
        boost::algorithm::split(elems, value, boost::is_any_of(","));
    }
    for (std::string &e : elems)
        // "nil" (not applicable to this extruder variant) is left for the nil translation that
        // runs once the value is deserialized against our option (BambuConfigCompat).
        if (e != "nil" && ! fn(e))
            return false;
    value = strings ? escape_strings_cstyle(elems) : boost::algorithm::join(elems, ",");
    return true;
}

} // namespace

const std::vector<Alias> &aliases()
{
    static const std::vector<Alias> v = make_aliases();
    return v;
}

const std::vector<EnumAlias> &enum_aliases()
{
    static const std::vector<EnumAlias> v = make_enum_aliases();
    return v;
}

const Alias *by_ours(const std::string &our_key) { return find_in(index_ours(), our_key); }
const Alias *by_bambu(const std::string &bambu_key) { return find_in(index_bambu(), bambu_key); }

bool import_key(std::string &opt_key, std::string &value)
{
    const Alias *a = by_bambu(opt_key);
    if (a == nullptr)
        return false;
    if (! value.empty() && a->from_bambu != nullptr) {
        std::string converted = value;
        if (! convert_elements(a->ours, converted, a->from_bambu)) {
            BOOST_LOG_TRIVIAL(info) << "Bambu Studio's " << opt_key << " = \"" << value << "\" has no equivalent for " << a->ours
                                    << "; left at our own value";
            opt_key.clear();
            return true;
        }
        value = std::move(converted);
    }
    opt_key = a->ours;
    return true;
}

std::string import_enum_value(const std::string &our_key, const std::string &bambu_value)
{
    const ConfigOptionDef *def = print_config_def.get(our_key);
    if (def == nullptr || def->type != coEnum || def->enum_keys_map == nullptr || def->enum_keys_map->count(bambu_value))
        return {};
    for (const EnumAlias &e : enum_aliases())
        if (e.import && our_key == e.key && bambu_value == e.bambu)
            return e.ours;
    for (const BambuExport::EnumTranslation &t : BambuExport::generated_enum_translations())
        if (*t.bambu != '\0' && our_key == t.key && bambu_value == t.bambu && def->enum_keys_map->count(t.ours))
            return t.ours;
    return {};
}

bool shadowed_by_our_key(const std::string &key, const std::function<bool(const std::string &)> &file_has)
{
    const Alias *a = by_bambu(key);
    return a != nullptr && file_has(a->ours);
}

} // namespace BambuKeyAliases
} // namespace Slic3r
