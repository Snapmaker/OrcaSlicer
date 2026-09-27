#pragma once

// Snapmaker Orca: which tool heads may run a High Flow nozzle, and the filament and process notices
// that follow. The rules need no window, so they are testable; fill_flow_combo() and flow_tooltip() show them.

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/string.h>

#include "libslic3r/PerHeadProcess.hpp"

class ComboBox;

namespace Slic3r {

class DynamicPrintConfig;
class PresetBundle;

namespace GUI { namespace HighFlowNotices {

// The nozzle volume types (as integers of NozzleVolumeType) the printer preset declares for
// tool head `head` in its "extruder_variant_list", in enum order. A printer without the list
// declares Standard only.
std::vector<int> declared_volume_types(const DynamicPrintConfig &printer_config, size_t head);

// The printer preset ships a High Flow column for this tool head.
bool head_declares_high_flow(const DynamicPrintConfig &printer_config, size_t head);

// Which nozzle sizes offer High Flow is a property of the vendor data: size d offers it for tool
// head `head` when the machine preset of the printer model for d declares it; a `nozzle_size`
// <= 0 asks whether any size of the model does. A function keeps the rules testable on bare
// configs; empty means "the preset's own size only".
using SizeOffersHighFlow = std::function<bool(double nozzle_size, size_t head)>;

// The GUI's SizeOffersHighFlow: asks head_declares_high_flow() of the model's machine preset for the size
// (NozzleFilament::head_machine_preset), every one for a size <= 0; false without one. Keeps a pointer
// to `bundle`.
SizeOffersHighFlow size_offers_high_flow(const PresetBundle &bundle);

// The tool head declares High Flow and carries a nozzle size the vendor data has High Flow values
// for: the size the preset was made for ("printer_variant"), or one `size_offers` answers for.
bool head_can_use_high_flow(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers = {});

// Resets every High Flow entry of `nozzle_volume_types` whose tool head cannot use High Flow to
// Standard and returns the tool heads (0-based) that were reset. Other volume types are left alone.
std::vector<size_t> sanitize(const DynamicPrintConfig &printer_config, std::vector<int> &nozzle_volume_types, const SizeOffersHighFlow &size_offers = {});

// A preset holds a High Flow column in its variant list ("filament_extruder_variant" /
// "print_extruder_variant").
bool has_high_flow_column(const DynamicPrintConfig &preset_config, const std::string &variant_key);

// The tool head (0-based) of each filament as the engine assigns it: a binding map (`filament_map`,
// 1-based) is followed; otherwise, and for missing or invalid entries, filament i goes to head i and
// the rest to `master_head`. Forwards to NozzleFilament::filament_heads.
std::vector<size_t> filament_heads(size_t filament_count, size_t head_count, const std::vector<int> &filament_map, bool map_is_binding, size_t master_head = 0);

// A filament a tool head prints.
struct HeadFilament
{
    std::string filament_type;          // "filament_type" of the preset
    std::string preset_name;
    bool        has_high_flow_column { false };
    // N7: the variant keys whose Standard column the preset changes against its system parent
    // while its High Flow column keeps the parent's value (standard_only_edits()), and that
    // parent's name. Empty for a system preset and for one whose columns were edited together.
    std::vector<std::string> standard_only_keys;
    std::string              parent_name;
};

// The filaments of every tool head: entry `filament_head[f]` of filament_heads() files
// `filaments[f]` under its tool head. Filaments of a tool head beyond `head_count` are dropped.
std::vector<std::vector<HeadFilament>> group_by_head(const std::vector<HeadFilament> &filaments, const std::vector<size_t> &filament_head, size_t head_count);

struct Report
{
    struct Entry
    {
        size_t      head { 0 };         // 0-based
        std::string material;           // label for the message
        std::string parent;             // N7: the system parent whose High Flow values print
    };
    std::vector<Entry>  not_recommended;        // N1: allow list says "not recommended"
    std::vector<Entry>  unsupported;            // N2: allow list says "unavailable"; slicing is blocked
    std::vector<Entry>  standard_values_used;   // N3: the filament preset has no High Flow column
    std::vector<size_t> standard_speeds_used;   // N4: the process preset has no High Flow column
    std::vector<Entry>  standard_only_edited;   // N7: the filament preset changes Standard values its High Flow column does not follow
    std::vector<size_t> process_standard_only;  // N7: the same for the process preset, per High Flow tool head

    bool blocks_slicing() const { return !unsupported.empty(); }
    bool empty() const
    {
        return not_recommended.empty() && unsupported.empty() && standard_values_used.empty() && standard_speeds_used.empty() &&
               standard_only_edited.empty() && process_standard_only.empty();
    }
};

// Checks every tool head set to High Flow; `filaments[i]` are the filaments of head i (group_by_head()).
// Heads without filaments are skipped for N1-N3 and N7; each material or preset appears once per head.
// `process_standard_only`: see Report::process_standard_only (N7).
Report evaluate(const std::vector<int> &nozzle_volume_types, const std::vector<std::vector<HeadFilament>> &filaments, bool process_has_high_flow_column,
                bool process_standard_only = false);
// Per-head variant: `process_has_high_flow_column[i]` = head i prints High Flow speeds (PerHeadProcess::head_sources);
// heads beyond the vector count as without. N4 names the heads without.
Report evaluate(const std::vector<int> &nozzle_volume_types, const std::vector<std::vector<HeadFilament>> &filaments,
                const std::vector<bool> &process_has_high_flow_column, bool process_standard_only = false);

// N7: keys of `variant_keys` whose Standard column `preset` changes against `parent` while its High Flow
// column keeps the parent's value; columns are named by `variant_key`. Empty for a single-column preset
// or a parent without High Flow column; missing keys and keys of another width are skipped.
std::vector<std::string> standard_only_edits(const DynamicPrintConfig &preset, const DynamicPrintConfig &parent,
                                             const std::set<std::string> &variant_keys, const std::string &variant_key);

// N1 / N2 grouping: the entries of a report grouped by the nozzle size label of their tool head
// (head_nozzle_size_label(); the preset's own size for a head without a diameter).
std::map<std::string, std::vector<Report::Entry>> group_by_nozzle_size(const std::vector<Report::Entry> &entries, const DynamicPrintConfig &printer_config);

// Flow selector of the Process, plate and object tabs: for a process preset with one column per flow
// type ("print_extruder_variant" [Standard, High Flow]) instead of per tool head, returns the columns'
// NozzleVolumeType values in order; selection s edits column s. Empty when no selector applies.
std::vector<int> flow_selector_types(const DynamicPrintConfig &printer_config, const DynamicPrintConfig &process_config);

// The entry of the flow selector for a flow type: the index of `type` (NozzleVolumeType as int)
// in `types` (flow_selector_types()), 0 when the type has no column or the list is empty. The
// Process tab opens on the column of the tool head whose nozzle tab the sidebar shows.
int flow_selector_index(const std::vector<int> &types, int type);

// The column of a preset's variant list ("filament_extruder_variant") for a flow type: the first
// whose name ends in the name of `type` (split_variant_name), e.g. "Direct Drive High Flow"; -1 if none.
int variant_column_for_type(const std::vector<std::string> &variants, int type);

// A variant name as the presets spell it, "Direct Drive High Flow", in its two parts. A name
// without a known volume type splits at its last blank; one without a blank is all drive.
struct VariantName
{
    std::string drive;          // "Direct Drive"
    std::string volume_type;    // "High Flow"
};
VariantName split_variant_name(const std::string &variant);

// The label parts of column `column` of a preset whose columns are `variants`: the drive is
// left out (empty) when every column names the same one, so the columns of a Snapmaker preset
// read "Standard" / "High Flow". A single column keeps its full name.
VariantName variant_column_label(const std::vector<std::string> &variants, size_t column);

// The ids of a variant table ("print_extruder_id", "printer_extruder_id") tell tool heads apart:
// they hold more than one distinct id. A single column and the flow-only layout above apply to
// every tool head, so naming "Extruder 1" next to such a column would mislead.
bool ids_name_tool_heads(const std::vector<int> &ids);

// The choice of a tool head can be changed: it declares no High Flow, or it may use it.
bool flow_choice_usable(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers = {});

// What the Flow row of a tool head looks like (sidebar nozzle tabs, "Nozzle flow" line). A
// head with one declared volume type shows it ruled out when another size of the model offers
// High Flow (U1 0.2 / 0.6 / 0.8 mm: "Standard", disabled); without High Flow at any size, no row.
enum class FlowRowState {
    Hidden,     // one declared volume type and no size of the model offers High Flow: nothing to choose
    Choice,     // several types, the stored one is shown and can be changed
    RuledOut    // no High Flow values for the head's nozzle size: the first declared type is shown, disabled
};
FlowRowState flow_row_state(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers = {});

// The type the row shows for `stored_type`: the stored one where it is declared and the choice is
// usable (Standard always is), the first declared one otherwise. The sanitizer corrects the stored value.
int shown_volume_type(const DynamicPrintConfig &printer_config, size_t head, int stored_type, const SizeOffersHighFlow &size_offers = {});

// The nozzle size of tool head `head` as the sidebar spells it ("0.6"); empty when the preset has
// no diameter for it. A ruled out row names the head's own size.
std::string head_nozzle_size_label(const DynamicPrintConfig &printer_config, size_t head);

// Label set for the Process-tab speed selector (Tab::fit_head_selector): long labels on one row, else short
// on one row, else short on several rows. Widths include padding; `available` <= 0 keeps the long labels.
enum class SelectorFit {
    Long,       // the long labels on one row
    Short,      // the short labels on one row
    ShortRows   // the short labels on several rows
};
SelectorFit head_selector_fit(const std::vector<int> &long_widths, const std::vector<int> &short_widths, int available);

// Fills a read-only Flow combo with the declared types of `head` (type as client data), shown_volume_type()
// selected; disabled with the reason as tooltip when High Flow is ruled out. Items are rebuilt only
// when they differ, so the combo may be refreshed from its own selection event.
void fill_flow_combo(::ComboBox *combo, const DynamicPrintConfig &printer_config, size_t head, int current_type, const SizeOffersHighFlow &size_offers = {});

// The tooltip of a Flow row and of its label: what the row is, then what follows for this nozzle.
// `head_size`: head_nozzle_size_label(). A ruled out row without a size reads like a choice.
wxString flow_tooltip(FlowRowState state, const std::string &head_size);

// Why the quality rule gave a tool head its preset (PerHeadProcess::source_for_head), from the matched
// `height` and whether it is the head's preferred one. Empty for SelectedPreset and Chosen sources.
wxString automatic_reason(PerHeadProcess::Step step, const std::string &plate_class, const std::string &class_used, const std::string &head_size,
                          double height, bool preferred);

// Speed-page intro and speed-selector tooltip for a tool head: head, nozzle size and flow type;
// `standard_chosen`: a High Flow nozzle set to print the Standard speeds (PerHeadProcess::flow_key).
wxString head_flow_description(size_t head, const std::string &head_size, NozzleVolumeType nozzle, bool standard_chosen);
wxString head_entry_tooltip(size_t head, const std::string &head_size, NozzleVolumeType nozzle, bool standard_chosen);

// The "Preset:" line under a nozzle tab's rows in the sidebar: `preset` the alias of the process preset the
// head prints with, `state` "(automatic)" / "(chosen)" or empty, `note` its column (Plain: the nozzle's flow,
// HighFlow, StandardChosen: Standard on a High Flow nozzle); the three values as the hint shows them.
enum class SpeedsNote { Plain, HighFlow, StandardChosen };
wxString speeds_hint_label(const wxString &preset, const wxString &state, SpeedsNote note, const std::string &outer_wall, const std::string &sparse,
                           const std::string &accel);
// The sentence the hint's tooltip adds for a head that prints the Standard speeds by choice.
wxString standard_chosen_tooltip();

}}} // namespace Slic3r::GUI::HighFlowNotices
