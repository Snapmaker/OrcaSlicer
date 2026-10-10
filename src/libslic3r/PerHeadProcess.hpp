#pragma once

// Snapmaker Orca: process speeds follow the nozzle size of the tool head that prints.
// Composes a process table with one column per (tool head x volume type); a head of another size takes
// speeds, accelerations and jerk from a system preset of its size. Keys edited in the selected preset win.

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "PrintConfig.hpp"

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
    NotInstalled,    // the preset the project names is not installed; the automatic rule applied instead
    HighFlow,        // the head carries a High Flow nozzle and prints with the High Flow column of a preset of its size (head_sources)
    Unfit            // the preset the project names is installed but not made for the head's nozzle size; the automatic rule applied instead
};

// Which step of the rule named the preset a tool head prints with (source_for_head).
enum class Step {
    SelectedPreset, // the head prints with the selected preset (not derived)
    Chosen,         // the preset the project names for the head (the picker of the Speed page)
    SameQuality,    // the nearest layer height among the presets of the plate's quality class
    ClassLadder,    // the plate's class has no preset of the head's size: the next class of the ladder (class_used)
    SizeDefault,    // no class matched: the default_print_profile of the head's machine preset
    FirstByName     // no class matched and no default: the first compatible preset by name
};

// The state of the process preset the project chose for a tool head (choice_key).
enum class ChosenState {
    None,           // no choice: the head follows the rule
    Applied,        // the chosen preset prints (derived, Reason::Explicit)
    SameAsSelected, // the choice names the selected preset: nothing to compose, the choice is kept
    NotInstalled,   // the chosen preset is not installed; the rule applied
    Unfit,          // the chosen preset is not made for the head's nozzle size; the rule applied
    Inactive        // the choice cannot apply here (the selected preset has no system parent); the head prints the selected preset
};

struct Source
{
    size_t                   head { 0 };
    const Preset            *preset { nullptr };  // the process preset the head prints with; never nullptr in a returned entry
    bool                     derived { false };   // false: the selected preset
    Reason                   reason { Reason::HomeSize };
    Step                     step { Step::SelectedPreset };
    std::string              class_used;          // ClassLadder: the class the ladder landed on ("Standard")
    std::vector<std::string> composed_keys;       // keys taken from `preset` (empty unless derived)
    std::vector<std::string> kept_keys;           // composed keys kept at the selected preset's value (user edits)
    std::vector<std::string> overridden_keys;     // keys set for this tool head that print (head_keys_in_use)
    std::vector<int>         fallback_variants;   // volume types (NozzleVolumeType) whose column came from the selected preset
    std::string              chosen;              // the project's choice for this head, "" when none (kept whatever its state)
    ChosenState              chosen_state { ChosenState::None };
    std::string              chosen_reason;       // Unfit / Inactive: "size 0.6" (the size of the preset's machine when known), "no machine preset", "no parent"
    const Preset            *automatic { nullptr }; // a chosen head: the preset the rule would give it (nullptr: the selected preset)
    std::vector<int>         own_standard_variants; // a chosen head: volume types served by the chosen preset's Standard column (it has none of their own)
    NozzleVolumeType         flow { nvtStandard };  // the flow type whose speeds column the head prints (effective_flow)
    bool                     flow_chosen { false };  // `flow` was chosen for the head (flow_key) and is not its nozzle's own
    // The preset of the head's size before the High Flow rule re-picked a speeds source; nullptr unless derived
    // at another size. Line widths come from it (width_source): a High Flow nozzle has the bore of its size.
    const Preset            *size_preset { nullptr };
};

// The 41 keys a head of another size takes from its size's preset: 17 role speeds, 8 accelerations, 7 jerk /
// junction deviation keys and the nine line widths. Travel keys, enable_overhang_speed, slowdown_for_curled_perimeters
// and small_perimeter_threshold stay uniform (read through other head mappings or gated per layer).
const std::set<std::string>& composed_keys();

// The preset whose line widths head `source.head` prints: the chosen preset or Source::size_preset; nullptr for
// the selected preset. Neither the High Flow re-pick nor the chosen flow (flow_key) affects widths.
const Preset *width_source(const Source &source);

// The name of the transient key that records, per composed column, the column of the selected
// preset that stands in for it. Written by compose(), read by Print::apply for the overrides of
// objects, parts and layer ranges, never stored in a preset, a project or the G-code header.
extern const char *const source_column_key;

// Quality class of a process preset name: the text before " @" without the leading layer height
// token ("0.20mm " of the U1 names, "0.28 " of the J1 names): "Standard", "High Quality",
// "Strength", "Color Mixing", "Extra Draft". Empty when the name has no " @" or nothing remains.
std::string quality_class(const std::string &preset_name);

// The quality class of the plate: the selected process preset's class, its system parent's when
// the name has none (a user preset "My fast PLA" inheriting 0.20mm Standard is Standard). May be
// empty (a detached preset, a vendor without classes).
std::string plate_quality_class(const PresetBundle &bundle);

// The layer height the rule matches for tool head `head`: extruder_layer_height of the edited printer
// when set, else the saved selected preset's layer_height, so an unsaved edit does not move the
// source. `preferred` tells which.
double target_layer_height(const PresetBundle &bundle, size_t head, bool *preferred = nullptr);

// The composed keys the selected process preset changes against its system parent, compared in the
// shared columns against the parent in the preset's layout (reference_in_layout_of). Empty without a
// system parent (NoParent). Values set for one tool head do not count: they apply to that head alone.
std::set<std::string> all_edited_keys(const PresetBundle &bundle);

// ---- Values set per tool head (the speed selector of the Process tab) --------------------------
// Shared id-0 columns (one per vendor flow; column 0 holds Standard for Snapmaker Orca 2.4) precede the head
// columns (ids 1..N), e.g. U1 0,0,1,1,..,4,4; head lookups never match id 0. override_key: keys set per column.
// I1: wide (has id-0 columns) iff the marker names a key. I2: every value key has the width of the ids.
// I3: an unmarked head column equals the shared column of its flow (or the only one). I4: a key marked
// on a head is marked, with equal values, on all its columns. I5: shared columns carry no marker.
extern const char *const override_key;

struct Layout
{
    std::vector<int>         ids;
    std::vector<std::string> variants;
    size_t                   size() const { return ids.size(); }
    bool                     operator==(const Layout &other) const { return ids == other.ids && variants == other.variants; }
    bool                     operator!=(const Layout &other) const { return !(*this == other); }
};
Layout layout_of(const DynamicPrintConfig &config);
// The shared columns of `shared_from` (its id-0 columns when wide, else its distinct variants)
// followed by the printer's columns (printer_extruder_id / printer_extruder_variant, else the tokens
// of extruder_variant_list, else one Direct Drive Standard column per nozzle).
Layout wide_layout(const DynamicPrintConfig &shared_from, const DynamicPrintConfig &printer);
// The layout has an id-0 column.
bool is_wide(const DynamicPrintConfig &config);
// The number of shared columns: the id-0 columns of a wide layout, every column of a narrow one.
size_t shared_width(const DynamicPrintConfig &config);
// The variants of the shared columns, in column order.
std::vector<std::string> shared_variants(const DynamicPrintConfig &config);

// The column walk: per target column (id, variant), every value key of print_options_with_variant and the
// marker take the exact source column, else the variant's id-0 column, else its first column, else the first
// shared column. Shared columns and columns filled from one get no marker. All keys or none; idempotent.
void relayout(DynamicPrintConfig &config, const Layout &to);
// relayout to wide_layout(config, printer); nothing when the config is wide already.
void widen(DynamicPrintConfig &config, const DynamicPrintConfig &printer);
// The shared columns alone, ids 1 (the vendor spelling), marker emptied; nothing when narrow.
void narrow(DynamicPrintConfig &config);
// `reference` laid out like `child` when the child is wide and wider than the reference (the
// column walk into `storage`), else `reference` itself. Every comparison of a child with its
// parent (dirty markers, revert, dialogs, save, load) reads the parent through this.
const DynamicPrintConfig &reference_in_layout_of(const DynamicPrintConfig &child, const DynamicPrintConfig &reference, DynamicPrintConfig &storage);

// The shared column of a flow type: the id-0 column whose variant names it on a wide layout, the
// column of the variant on a narrow flow-only layout, column 0 otherwise. Never negative for a
// process config with a variant list.
int shared_column(const DynamicPrintConfig &config, NozzleVolumeType type);
// The variant string of a column names the flow type (the drive is not compared).
bool variant_names_type(const std::string &variant, NozzleVolumeType type);
// The columns of tool head `head` (0-based): the columns with id head+1; empty on a narrow layout.
std::vector<int> head_columns(const DynamicPrintConfig &config, size_t head);
// The marker of a column names the key.
bool is_marked(const DynamicPrintConfig &config, size_t column, const std::string &key);
// The keys the marker names on any column of the head, in the order of print_options_with_variant.
std::vector<std::string> head_override_keys(const DynamicPrintConfig &config, size_t head);
// The marker names a key on some column; false on a narrow preset.
bool marker_names_any(const DynamicPrintConfig &config);
// The marker names nothing.
bool marker_empty(const DynamicPrintConfig &config);
// The 48 keys settable per tool head: the value keys of print_options_with_variant except enable_overhang_speed,
// slowdown_for_curled_perimeters and small_perimeter_threshold, which stay uniform.
const std::set<std::string> &head_editable_keys();
// The nine line width keys of the Quality page. Flow-independent (a High Flow nozzle has the bore of its size): a head
// value fills both flow columns, an All tool heads edit every shared column, the composer reads the Standard shared
// column, normalise() equalises flow columns, and neither the flow toggle nor flow_key selects a width column.
const std::set<std::string> &flow_independent_keys();

// After a write of `key` into `written_column` (one of the head's columns; the first one when
// negative): the value is copied into the other columns of the head and the key marked on all of
// them (I4).
void set_head_value(DynamicPrintConfig &config, size_t head, const std::string &key, int written_column = -1);

// Writes one value into every column of a line width, marker untouched (pressure advance pattern calibration,
// SuggestedConfigCalibPAPattern). An absent key gets one column, widened when the preset is applied.
void set_every_column(DynamicPrintConfig &config, const std::string &key, const FloatOrPercent &value);
// After a write of `key` into the shared column of `type`: the value is copied into every head
// column of that flow that is not marked for the key (every head column when the layout has one
// shared column) (I3).
void set_shared_value(DynamicPrintConfig &config, const std::string &key, NozzleVolumeType type);
// The head's columns take the shared column of their flow for `key` and the key leaves the head's marker.
void clear_head_value(DynamicPrintConfig &config, size_t head, const std::string &key);
// clear_head_value for every key the head's marker names.
void clear_head(DynamicPrintConfig &config, size_t head);
// clear_head_value for the keys the head's marker names among `keys` (the indexed keys of one page
// of the Process tab: the Quality page clears the line widths of the head, the Speed page its speeds).
void clear_head(DynamicPrintConfig &config, size_t head, const std::set<std::string> &keys);
// clear_head for every head; the caller narrows.
void clear_all_heads(DynamicPrintConfig &config);
// The tool heads (0-based) whose marker names `key`.
std::vector<size_t> heads_marked_for(const DynamicPrintConfig &config, const std::string &key);

// Process key, per tool head: nozzle size in microns its values were set for, "" unknown.
extern const char *const nozzle_key;
// Unknown: no recorded size, printed. Inactive: set for another nozzle size, not printed.
enum class HeadValues { None, InUse, Unknown, Inactive };
HeadValues head_values(const DynamicPrintConfig &process, const DynamicPrintConfig &printer, size_t head);
// Recorded size in mm, 0 when none.
double made_for(const DynamicPrintConfig &process, size_t head);
// head_override_keys unless Inactive.
std::vector<std::string> head_keys_in_use(const DynamicPrintConfig &process, const DynamicPrintConfig &printer, size_t head);
// Records the head's current nozzle size for its values.
void stamp_head(DynamicPrintConfig &process, const DynamicPrintConfig &printer, size_t head);
// For a slicing copy, never a preset.
bool drop_inactive(DynamicPrintConfig &config, const DynamicPrintConfig &printer, std::vector<size_t> *dropped = nullptr);

// Load normaliser for projects and user presets, restoring I1-I5: widens short value keys, rebuilds short ids
// from wide_layout(parent, printer) or cuts the key, adds missing shared columns from the parent, keeps only
// head_editable_keys() in the marker, narrows or widens by the marker. `parent` and `printer` may be null.
void normalise(DynamicPrintConfig &config, const DynamicPrintConfig *parent, const DynamicPrintConfig *printer);

// The column of `source` (the preset a tool head prints with) for tool head `head` at flow `type`:
// on a wide source its shared column of the type, never a head column; on a narrow one the exact
// column, else its single column if that names the type; else -1. column_for_head: exact lookup on a config.
int source_column(const Preset &source, size_t head, NozzleVolumeType type, const DynamicPrintConfig &printer);
int column_for_head(const DynamicPrintConfig &process, size_t head, NozzleVolumeType type, const DynamicPrintConfig &printer);
// Whether `process` has High Flow values of its own for tool head `head`: in a wide layout its shared
// columns decide (widen() gives every head a High Flow column, filled from Standard if needed); in a
// narrow one the column the head reads (column_for_head). Used by the High Flow rule and notice N4.
bool has_high_flow_values(const DynamicPrintConfig &process, size_t head, const DynamicPrintConfig &printer);
// The head reads the High Flow column of its source: `flow` (the head's flow type) is High Flow,
// the source is derived and has a column for it. The hint and the Speed page then name the source
// with ", High Flow".
bool reads_high_flow(const Source &source, NozzleVolumeType flow, const DynamicPrintConfig &printer);

// Copies indexed process rows ("key#N" = column N of `source`) into `target` when either is wide, widening a
// narrow target first: marked head columns go to the same (id, variant) with their marker, unmarked ones are
// skipped, shared or narrow ones go to the target's shared column of their flow. Narrows an unmarked result.
// `replaced`: (head, mm) of target values set for another nozzle size.
void transfer_columns(DynamicPrintConfig &target, const DynamicPrintConfig &source, const std::vector<std::string> &indexed_options, const DynamicPrintConfig &printer,
                      std::vector<std::pair<size_t, double>> *replaced = nullptr);

// Which step of the rule chose, and the class it landed on (ClassLadder).
struct RuleTrace
{
    Step        step { Step::SelectedPreset };
    std::string class_used;
};

// The system preset a head of `machine` (its size's machine preset) prints with: an installed compatible
// `explicit_name` (Explicit); else, `selected` excluded, the preset nearest the target height (preferred_layer_height
// when > 0, else `selected`'s) in `selected`'s class, then down the ladder (Color Mixing: High Quality, then Standard),
// then default_print_profile, then the first by name. `accept` filters; `trace` gets the step.
const Preset* source_for_head(const PresetBundle &bundle, const Preset &machine, double preferred_layer_height,
                              const Preset &selected, const std::string &explicit_name, Reason &reason,
                              const std::function<bool(const Preset &)> &accept = {}, RuleTrace *trace = nullptr);

// ---- The chosen source: a process preset chosen for a tool head in the project --------------------

// Project key, one entry per tool head: the process preset chosen on the Speed page for the head's
// speeds, accelerations and jerk, "" to follow the rule, [] when no head has a choice (equal to a
// project saved without the key). Not stored in presets or the record.
extern const char *const choice_key;
// The choice of tool head `head`, "" when none.
std::string chosen_of(const PresetBundle &bundle, size_t head);
// Writes the choice of tool head `head` ("" = the rule); the vector grows to head+1 and never
// shrinks, and a vector of empty entries becomes [].
void set_chosen(PresetBundle &bundle, size_t head, const std::string &name);
// Some tool head has a choice.
bool any_chosen(const PresetBundle &bundle);
// The per-head process table is in use: the preference process_follows_nozzle is on, some head has
// a choice or some head has a chosen flow in effect (both apply whether the preference is on or
// off). The one gate every reader of head_sources uses.
bool active(const PresetBundle &bundle);
// The chosen preset by name, a vendor rename resolved through find_preset(name, false); nullptr
// when not installed.
const Preset *resolve_chosen(const PresetBundle &bundle, const std::string &name);
// Whether `preset` fits tool head `head`: compatible with the printer preset on a home-size head, with
// the head's machine preset otherwise, never without a machine preset. A preset without compatibility
// conditions is judged by its system root. `reason`: "size <x>", "no machine preset" or "".
bool fits(const PresetBundle &bundle, const Preset &preset, size_t head, std::string *reason = nullptr);
// The choices rewritten to the current names of renamed vendor presets; called by the project
// load, once, before the dirty state takes its initial copy.
void resolve_renamed_choices(PresetBundle &bundle);

// ---- The chosen flow: the speeds column a High Flow tool head prints with ------------------------

// Project key, one entry per tool head: the flow whose speeds column the head prints when not its
// nozzle's own ("Standard" on a High Flow nozzle), "" otherwise, [] when no head has one. Not stored in
// presets or the record; an entry the nozzle does not offer is kept but dormant.
extern const char *const flow_key;
// The flow type of the nozzle of tool head `head` (project nozzle_volume_type), Hybrid read as
// Standard, Standard beyond the vector. `config` holds the project keys (project_config, a full config).
NozzleVolumeType nozzle_flow(const DynamicPrintConfig &config, size_t head);
// The flow whose speeds column tool head `head` prints: its flow_key entry when the nozzle offers it
// (High Flow and Hybrid offer both, Standard only Standard), else the nozzle's flow. Speeds only;
// the filament side keeps the nozzle's flow.
NozzleVolumeType effective_flow(const DynamicPrintConfig &config, size_t head);
NozzleVolumeType effective_flow(const PresetBundle &bundle, size_t head);
// effective_flow differs from nozzle_flow: an entry is in effect on the head.
bool flow_chosen(const DynamicPrintConfig &config, size_t head);
bool flow_chosen(const PresetBundle &bundle, size_t head);
// Some tool head has an entry in effect.
bool any_flow_chosen(const DynamicPrintConfig &config);
// The entry of tool head `head` as stored ("", "Standard", "High Flow"), dormant or not.
std::string chosen_flow_of(const PresetBundle &bundle, size_t head);
// Writes the flow of tool head `head`: the nozzle's own flow is stored as "" (the toggle back to
// High Flow clears the entry); the vector grows to head+1 and never shrinks, and a vector of empty
// entries becomes [].
void set_chosen_flow(PresetBundle &bundle, size_t head, NozzleVolumeType flow);

// One entry of the picker of the Speed page.
struct Candidate
{
    const Preset *preset { nullptr };
    enum Group { System, User, Project } group { System };
    bool has_flow_column { false }; // the preset has a column for the head's flow type (source_column >= 0)
    bool is_automatic { false };    // the preset the rule gives the head today
    bool is_selected { false };     // the selected process preset
};
// The presets the Speed page picker lists for tool head `head`: system presets compatible with the
// head's machine preset (any visibility), then fitting user and project presets (no bundle presets),
// each group in collection order. Empty without a machine preset or on a printer the rule is off for.
std::vector<Candidate> picker_candidates(const PresetBundle &bundle, size_t head);

// A tool head's values as a user preset; empty `parent`: not possible.
struct ExtruderPreset
{
    std::string              parent;
    DynamicPrintConfig       config;
    std::vector<std::string> moved;
    double                   size { 0. };      // mm
    bool                     choose { false }; // `size` is the head's current nozzle size
};
ExtruderPreset extruder_preset(const PresetBundle &bundle, size_t head);

// The column of a derived head's source the composer reads for `flow`: source_column, else a chosen
// preset's Standard shared column; -1 when the selected preset's column serves or the head is not derived.
int composed_column(const Source &source, NozzleVolumeType flow, const DynamicPrintConfig &printer);
// Per-key variant: a line width (flow_independent_keys) reads width_source()'s Standard shared column (-1: the
// selected preset), any other key composed_column(); `from` receives the preset read. Used to display head values.
int composed_column_for_key(const Source &source, const std::string &key, NozzleVolumeType flow, const DynamicPrintConfig &printer, const Preset *&from);

// Initial value of a line width override of an object, part or layer range: what the item prints now, serialised
// ("102.5%"). One head (filament - 1, as Print::width_slot): its override_key value, else its width source's Standard
// column; several heads or no composed source: the shared value, differing heads in `differing_heads`. Empty if no key.
std::string override_seed(const PresetBundle &bundle, const std::string &key, const std::vector<int> &filaments, std::vector<size_t> *differing_heads = nullptr);

// One entry per tool head of a Snapmaker FFF multi-head printer, else empty. First match: no system parent ->
// NoParent; a choice -> Applied (also with the preference off) or SameAsSelected; preference off -> Off; HomeSize;
// NoMachinePreset; else Derived. A High Flow head whose source lacks that column moves to a size preset with one.
std::vector<Source> head_sources(const PresetBundle &bundle);

// Project key: per tool head, the process preset it printed with at the last apply ("" = selected
// preset); [] while no head is derived, matching a project saved without the key.
extern const char *const record_key;

// head_sources(bundle) written into the project config as the record (record_key): one entry per
// tool head when a head is derived (a chosen head names its preset), [] otherwise; written only on
// change; returns the sources. Called before every apply that may compose. head_sources() never reads
// the record as a choice (that would pin the rule); choices live in choice_key, never written here.
std::vector<Source> record_sources(PresetBundle &bundle);

// The command line composes no per-head table. Empties the record of `config` (a loaded project),
// keeps the choice and returns one warning line per recorded or chosen tool head.
std::vector<std::string> command_line_record(DynamicPrintConfig &config);

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

// Transient keys (written by compose(), read by Print::apply, never stored, erased from the G-code
// header): per composed column the index of the selected preset's column among its shared columns
// (-1: none), and their count - the flow-only space of object, part and layer range overrides.
extern const char *const source_flow_key;
extern const char *const flow_count_key;

// The variants of the flow-only space: the shared columns of a wide layout, every column of a
// flow-only or single-column layout; empty for a per-head layout without shared columns (read by column).
std::vector<std::string> flow_space_variants(const DynamicPrintConfig &config);

// Rewrites the variant keys of `full` into one column per (tool head x volume type): uniform, edited and marked
// keys from the selected preset's head column; a derived or flow-chosen head's composed keys from source_column,
// else own_standard_variants, else fallback_variants. Writes the transient keys; false, `full` untouched, if unchanged.
bool compose(DynamicPrintConfig &full, const std::set<std::string> &edited_keys, std::vector<Source> &sources);

} // namespace PerHeadProcess
} // namespace Slic3r
