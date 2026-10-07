#pragma once

// Your own filament prices: per-kg prices that apply over every preset of a filament, kept outside
// the presets in <datadir>/cost/filament_overrides.json.
//
// Key. A price is set for a filament FAMILY, identified by
//     vendor | type | family name
// where vendor is filament_vendor (case and spacing folded), type is filament_type (PLA, PLA-CF,
// PETG-HF, ...; upper case) and the family name is the preset name with its printer / nozzle part
// removed: everything from the first '@' ("Bambu PLA Basic @BBL X1C", "Afinia ABS@HS") and a
// trailing "0.2 nozzle" / "(0.6 nozzle)" ("Anker Generic ABS 0.2 nozzle"). A user preset is named
// after its system parent ("inherits"), so "My PLA" derived from "Bambu PLA Basic @BBL A1" is in
// the "Bambu PLA Basic" family. So every printer / nozzle variant of "Bambu PLA Basic" (40 shipped
// presets across BBL and OrcaFilamentLibrary) shares one key, while "Bambu PLA-CF" (type PLA-CF),
// "Bambu PLA Silk" (another family) and "Polymaker PLA" (another vendor) are separate keys.
//
// filament_id is deliberately NOT the key: shipped profiles reuse ids across unrelated filaments
// (OFDSrzZ8, Orca's Generic PLA id, is the id of 79 different vendor/type/family keys, GFB99 of 69),
// so an id-keyed price for one Flashforge PLA would have priced every Flashforge PLA.
//
// A price can also be set for ONE preset ("this preset only", keyed by the preset name).
//
// Precedence for each filament slot, first that applies:
//   1. a "this preset only" price for the slot's preset name;
//   2. the preset's own price, when the preset is a user preset whose price the user set
//      deliberately: a non-system preset with a parent whose filament_cost differs from the parent's;
//   3. the family price;
//   4. the preset's own price (system value, user preset copying its parent's price, project value).
// A price of 0 is a real value ("free"), distinct from "no price of your own".
//
// Applied in exactly one place: the config handed to Print::apply by the GUI's background slicing
// (BackgroundSlicingProcess::apply). Presets, their dirty state, compare, sync and project files
// (3MF / AMF, written from PresetBundle::full_config_secure()) never see these prices.

#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetCollection;

namespace FilamentPrices {

// "Bambu PLA Basic @BBL X1C 0.2 nozzle" -> "Bambu PLA Basic"; spacing collapsed, case kept.
std::string family_name(const std::string &preset_name);
// The family key from its three parts (normalised: vendor lower case, type upper case, family lower case).
std::string family_key(const std::string &vendor, const std::string &type, const std::string &family);

struct Identity
{
    std::string preset;   // preset name (filament_settings_id)
    std::string vendor;
    std::string type;
    std::string family;   // display form, see family_name()
    // A user preset whose price the user set deliberately (differs from its parent's): it keeps it.
    bool        own_price{false};
    double      parent_price{0.};

    std::string key() const { return family_key(vendor, type, family); }
};

// The identity of a filament preset. `price` is the price the slot would use (the edited value);
// `filaments` finds the parent of a user preset (may be null: no own-price rule then).
Identity identify(const Preset &preset, double price, const PresetCollection *filaments);

enum class Source {
    Preset,       // the preset's own price, no price of yours applies
    OwnPrice,     // a user preset's deliberately set price (beats your family price)
    Family,       // your price for the family
    PresetOnly,   // your price for this preset only
};

struct Resolved
{
    double price{0.};          // what slicing uses
    double preset_price{0.};   // what the preset says
    Source source{Source::Preset};
    // When source is OwnPrice: the family price that would otherwise have applied.
    bool   family_shadowed{false};
    double family_price{0.};

    bool yours() const { return source == Source::Family || source == Source::PresetOnly; }
};

struct Entry
{
    enum class Scope { Family, Preset };
    Scope       scope{Scope::Family};
    std::string preset;   // Scope::Preset: the preset name
    // Family parts. For Scope::Family they are the key; for Scope::Preset a display cache.
    std::string vendor;
    std::string type;
    std::string family;
    double      price_per_kg{0.};
    long long   updated{0};       // seconds since the epoch
    std::string extra;            // fields this version does not know, as a JSON object, written back unchanged

    std::string key() const { return family_key(vendor, type, family); }
};

class Store
{
public:
    // Missing file = empty store, true. Unreadable / not our format = empty store, false
    // (load_error() says why); a later save() keeps a copy of the unreadable file first.
    bool load(const std::string &path);
    // Pretty JSON through <path>.tmp + rename: a reader never sees half a file.
    bool save(const std::string &path) const;

    const std::vector<Entry> &entries() const { return m_entries; }
    bool                      empty() const { return m_entries.empty(); }
    const std::string        &load_error() const { return m_load_error; }

    const Entry *find_family(const std::string &key) const;
    const Entry *find_preset(const std::string &preset_name) const;

    // Negative or non-finite prices are refused (false).
    bool set_family(const std::string &vendor, const std::string &type, const std::string &family, double price_per_kg);
    bool set_preset(const std::string &preset_name, const Identity &id, double price_per_kg);
    bool clear_family(const std::string &key);
    bool clear_preset(const std::string &preset_name);
    void clear_all() { m_entries.clear(); }

    Resolved resolve(const Identity &id, double preset_price) const;

private:
    std::vector<Entry> m_entries;
    int                m_version{1};
    std::string        m_extra;   // unknown top-level fields
    std::string        m_load_error;
};

// <datadir>/cost/filament_overrides.json unless a test set another path.
std::string store_path();
void        set_store_path(const std::string &path);   // "" = back to the default

// The process-wide store, loaded from store_path() on first use. Callers get a snapshot.
std::shared_ptr<const Store> global();
// Write `store` to store_path() and make it the global one. False when the file could not be written
// (the global store is still updated, so the session keeps the prices).
bool save_global(const Store &store);
// Forget the cached store (next global() reads the file again).
void reset_global();
// Bumped on every save_global(): lets views notice that prices changed.
unsigned revision();

// Rewrites filament_cost[i] of a full print config (PresetBundle::full_fff_config() shape:
// filament_settings_id, filament_vendor, filament_type, inherits_group) with the resolved prices.
// Only slots that exist in filament_cost are touched. Returns one Resolved per filament slot.
std::vector<Resolved> apply(DynamicPrintConfig &config, const Store &store, const PresetCollection *filaments);

// The same resolution for display, without changing the config.
std::vector<Resolved> resolve_slots(const DynamicPrintConfig &config, const Store &store, const PresetCollection *filaments);

// Removes every filament price from a config about to be written into a sliced-plate 3MF that
// leaves the prices out: filament_cost itself, and its mention in different_settings_to_system (so
// that reopening the file takes the price from the system preset instead of marking the filament
// modified with a default price).
void strip_prices(DynamicPrintConfig &config);

} // namespace FilamentPrices
} // namespace Slic3r
