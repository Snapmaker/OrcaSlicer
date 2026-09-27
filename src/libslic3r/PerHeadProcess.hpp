#pragma once

// Snapmaker Orca: process speeds follow the nozzle size of the tool head that prints.
// Composes a process table with one column per (tool head x volume type); a head of another size takes
// speeds, accelerations and jerk from a system preset of its size. Keys edited in the selected preset win.

#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetBundle;

namespace PerHeadProcess {

enum class Reason {
    Off,             // the preference "process_follows_nozzle" is off
    HomeSize,        // the head carries the nozzle size of the printer preset
    Derived,         // the head prints with the preset the automatic rule chose
    Explicit,        // the head prints with the preset the project names for it
    NoMachinePreset, // the head's size has no machine preset: nothing to follow
    NoProcessPreset, // no system process preset for the head's size is installed
    NoParent,        // the selected preset has no system parent (detached or imported): every key counts as edited
    NotInstalled     // the preset the project names is not installed; the automatic rule applied instead
};

struct Source
{
    size_t                   head { 0 };
    const Preset            *preset { nullptr };  // the process preset the head prints with; never nullptr in a returned entry
    bool                     derived { false };   // false: the selected preset
    Reason                   reason { Reason::HomeSize };
    std::vector<std::string> composed_keys;       // keys taken from `preset` (empty unless derived)
    std::vector<std::string> kept_keys;           // composed keys kept at the selected preset's value (user edits)
    std::vector<int>         fallback_variants;   // volume types (NozzleVolumeType) whose column came from the selected preset
};

// The 41 keys a head of another size takes from its size's preset: 17 role speeds, 8 accelerations, 7 jerk /
// junction deviation keys and the nine line widths. Travel keys, enable_overhang_speed, slowdown_for_curled_perimeters
// and small_perimeter_threshold stay uniform (read through other head mappings or gated per layer).
const std::set<std::string>& composed_keys();

// The name of the transient key that records, per composed column, the column of the selected
// preset that stands in for it. Written by compose(), read by Print::apply for the overrides of
// objects, parts and layer ranges, never stored in a preset, a project or the G-code header.
extern const char *const source_column_key;

// Quality class of a process preset name: the text between "mm " and " @" ("Standard",
// "High Quality", "Strength", "Color Mixing"); empty when the name has no such part.
std::string quality_class(const std::string &preset_name);

// The composed keys the selected process preset changes against its system parent. Empty when
// the selected preset has no system parent (the caller treats that as NoParent).
std::set<std::string> edited_keys(const PresetBundle &bundle);

// System process preset for a tool head of `machine`: `explicit_name` if installed and compatible,
// else the compatible preset at `preferred_layer_height` (0: selected's) preferring the selected quality
// class, else the nearest height of that class, else default_print_profile; nullptr if none.
const Preset* source_for_head(const PresetBundle &bundle, const Preset &machine, double preferred_layer_height,
                              const Preset &selected, const std::string &explicit_name, Reason &reason);

// One entry per tool head. Empty unless the printer is a Snapmaker FFF printer with more than one
// tool head (NozzleFilament::head_state); every entry Off unless the bundle's process_follows_nozzle
// is set. composed_keys / kept_keys are filled from edited_keys(bundle).
std::vector<Source> head_sources(const PresetBundle &bundle);

// Project key: per tool head, the process preset it printed with at the last apply ("" = selected
// preset); [] while no head is derived, matching a project saved without the key.
extern const char *const record_key;

// head_sources(bundle) written into the project config as the record (record_key): one entry per
// tool head when a head is derived (a chosen head names its preset), [] otherwise; written only on
// change; returns the sources. Called before every apply that may compose. head_sources() never reads
// the record as a choice (that would pin the rule); choices live in choice_key, never written here.
std::vector<Source> record_sources(PresetBundle &bundle);

// One tool head whose record differs from what it prints with now.
struct LoadReportEntry
{
    size_t      head { 0 };
    double      nozzle_size { 0. };
    std::string recorded;            // the record, never empty
    std::string current;             // the preset the head prints with now; empty: the selected preset
    Reason      reason { Reason::HomeSize }; // why the head prints with `current`
    bool        recorded_installed { true }; // the recorded preset is still in the bundle
};

// Compares the record with head_sources(bundle): every head whose record is non-empty and differs
// from the source it prints with now. Nothing is switched. Empty when the record is empty or
// matches, or when the printer is no Snapmaker multi-head printer.
std::vector<LoadReportEntry> load_report(const PresetBundle &bundle);

// Rewrites the variant keys of `full` into one column per (tool head x volume type): composed keys of a
// derived head come from sources[head].preset (fallbacks noted in fallback_variants), all other keys and
// `edited_keys` from the selected preset. Returns false and leaves `full` untouched if nothing changes.
bool compose(DynamicPrintConfig &full, const std::set<std::string> &edited_keys, std::vector<Source> &sources);

} // namespace PerHeadProcess
} // namespace Slic3r
