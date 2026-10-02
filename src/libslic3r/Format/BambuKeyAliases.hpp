#ifndef slic3r_Format_BambuKeyAliases_hpp_
#define slic3r_Format_BambuKeyAliases_hpp_

// Settings this fork and Bambu Studio both have, under different names or with differently
// spelled enum values. ONE table serves both directions:
//   * "Export Bambu 3MF" (Format/BambuExport.cpp) writes our key under Bambu's name and converts
//     the value with to_bambu;
//   * our loader (PrintConfigDef::handle_legacy) reads Bambu's name back as our key and converts
//     the value with from_bambu - for 3MF project settings, embedded presets, per-object /
//     per-part / height-range settings and loose preset .json files alike, since all of them go
//     through handle_legacy.
// A file that carries BOTH spellings keeps ours: ConfigBase::load_from_json_document and
// ModelConfig::set_deserialize skip the Bambu alias (shadowed_by_our_key).
//
// See docs/bambu-config-compat.md ("Renamed keys") and docs/bambu-3mf-export.md.

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace BambuKeyAliases {

// Converts ONE element of a value in place (a scalar, or one slot of a vector). Returns false when
// the value has no equivalent on the other side: the key is then left out (export) or not loaded
// (import), so the other side keeps its own default or inherited value. Nothing is ever invented.
using ValueFn = bool (*)(std::string &value);

struct Alias
{
    const char *ours;
    const char *bambu;
    ValueFn     to_bambu;    // nullptr: same value
    ValueFn     from_bambu;  // nullptr: same value
    const char *drop_reason; // why to_bambu can refuse a value (export log), nullptr if it never does
};

// Enum values spelled differently on the two sides for the same key.
struct EnumAlias
{
    const char *key;
    const char *ours;
    const char *bambu;
    bool        import; // false: several of ours map to one Bambu value, this one is not the way back
};

const std::vector<Alias>     &aliases();
const std::vector<EnumAlias> &enum_aliases();

const Alias *by_ours(const std::string &our_key);
const Alias *by_bambu(const std::string &bambu_key);

// Import. If opt_key is a Bambu alias, rename it to our key and convert value in place (every
// slot of a vector, parsed the way our option serializes). An empty value (a key-only rename, as
// for different_settings_to_system, or the first pass over a JSON array) only renames. When the
// value has no equivalent here opt_key is cleared, like any other key the loader drops.
// Returns false (and changes nothing) when opt_key is not a Bambu alias.
bool import_key(std::string &opt_key, std::string &value);

// Import. Our spelling of a Bambu enum value for one of our scalar enum options, from the manual
// table above and the generated Bambu translation table; empty when our option already accepts the
// value or there is no translation.
std::string import_enum_value(const std::string &our_key, const std::string &bambu_value);

// Import. True when `key` is a Bambu alias whose our-spelled key the same file also sets: the
// alias must then be skipped, so our own value wins whatever order the two are read in.
bool shadowed_by_our_key(const std::string &key, const std::function<bool(const std::string &)> &file_has);

// Import, after a whole source (file, map) is loaded. A Bambu key this fork loads as itself that
// also feeds one of our differently named settings when the same source does not set ours
// (extruder_clearance_max_radius -> extruder_clearance_radius, formerly a rename alias).
} // namespace BambuKeyAliases
class ConfigBase;
namespace BambuKeyAliases {
void load_fallbacks(ConfigBase &config, const std::function<bool(const std::string &)> &source_has);

} // namespace BambuKeyAliases
} // namespace Slic3r

#endif // slic3r_Format_BambuKeyAliases_hpp_
