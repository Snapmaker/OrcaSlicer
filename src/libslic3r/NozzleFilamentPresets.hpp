#pragma once

// Snapmaker Orca: filament presets follow the nozzle size of the tool head that prints them.
// The U1 ships one filament preset per nozzle size, pinned by "compatible_printers"; these helpers
// map slots to tool heads and machine presets and pick the version a slot should hold.

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Preset;
class PresetBundle;
class PresetCollection;
class PrinterPresetCollection;

namespace NozzleFilament {

// A filament slot no tool head of its own prints: a slot beyond the tool heads, a mixed
// (virtual) slot.
constexpr size_t no_head = size_t(-1);

// The 0-based tool head of each filament, as the engine decides it: a binding (1-based)
// `filament_map` is followed; otherwise, and for invalid entries, filament i goes to head i and
// filaments beyond the heads to `master_head` (ToolOrdering::get_recommended_filament_maps).
std::vector<size_t> filament_heads(size_t filament_count, size_t head_count, const std::vector<int> &filament_map, bool map_is_binding, size_t master_head = 0);

// "0.4" as a number, whatever the decimal separator of the user's locale is; 0 when the text
// is no size.
double parse_nozzle_size(const std::string &text);

// The nozzle size a printer preset was made for ("printer_variant" as a number), 0 when the
// preset does not say. With 0 every tool head counts as carrying the home size.
double home_nozzle_size(const DynamicPrintConfig &printer_config);

// The machine preset of printer model `model` for the nozzle size spelled `variant_label`
// ("0.2", the spelling of "printer_variant"): the system preset, else a custom one. Visibility
// plays no role. nullptr when the model has no profile of that size.
const Preset* head_machine_preset(const PrinterPresetCollection &printers, const std::string &model, const std::string &variant_label);
// The same by value, for a caller that holds the diameter and not the spelling of the profile.
const Preset* head_machine_preset(const PrinterPresetCollection &printers, const std::string &model, double nozzle_size);

struct State
{
    // Level 1: the rule applies to this printer at all (PresetBundle::nozzle_filament_enabled, FFF,
    // more than one nozzle, no single extruder multi material, a Snapmaker printer preset).
    bool rule_on { false };
    // Level 2: rule_on, and at least one tool head carries another size than the printer preset
    // while a machine preset of that size exists.
    bool mixed { false };
    // Per tool head: the machine preset that stands for it. The edited printer preset for a head
    // of the home size and for a head whose size has no profile. Empty unless rule_on.
    std::vector<const Preset*> head_machine;
    // Per tool head: "nozzle_diameter". Empty unless rule_on.
    std::vector<double>        head_size;
    // Per filament slot: its tool head, or no_head. Empty unless rule_on.
    std::vector<size_t>        slot_head;

    size_t        head_of(size_t slot) const { return slot < slot_head.size() ? slot_head[slot] : no_head; }
    // nullptr for a slot without a tool head.
    const Preset* machine_of(size_t slot) const
    {
        const size_t head = this->head_of(slot);
        return head < head_machine.size() ? head_machine[head] : nullptr;
    }
};
State state(const PresetBundle &bundle);
// The same without the gate on PresetBundle::nozzle_filament_enabled: which tool head carries
// which size and machine preset, whatever the filament rule is set to. The process rule
// (PerHeadProcess) reads it under its own preference.
State head_state(const PresetBundle &bundle);

// Printer sync: keep `edited` when it matches the model and nozzle size of `picked` (from
// PresetBundle::get_similar_printer_preset()), so the sync only sets tool head sizes and flow
// types without re-selecting a modified preset. False: the sync selects `picked`.
bool sync_keeps_printer_preset(const Preset &edited, const Preset &picked);

// The first system preset along "inherits", the preset itself when it is one. nullptr for a user
// preset without a system ancestor.
const Preset* system_ancestor(const PresetCollection &filaments, const Preset &preset);

// The preset restricts the printers it serves, by "compatible_printers" or by
// "compatible_printers_condition". One that does neither fits every printer and is the version of
// no nozzle size.
bool restricts_printers(const Preset &preset);

// The system preset of the family of `system_preset` (Preset::system_inherits + alias) that serves
// `machine`. nullptr when no member or two members (logged) serve it, or without a family.
const Preset* version_for(const PresetCollection &filaments, const Preset &system_preset, const Preset &machine);

// The nozzle size a filament preset is pinned to: "printer_variant" of the machine presets its
// system ancestor lists, when they agree. 0 when the preset pins no single size.
double preset_nozzle_size(const PresetBundle &bundle, const Preset &filament);

// Combo label with the size first ("0.6 mm · Generic PLA") so a narrow combo cuts the name, not
// the size. `label` unchanged when `size_text` is empty. Display only.
std::string size_marked_label(const std::string &label, const std::string &size_text);

// `filament` may print on a tool head that `machine` stands for: compatible with that machine
// preset, not excluded from the edited printer preset (the arm of the filament library) and
// compatible with the edited process preset.
bool fits(const PresetBundle &bundle, const Preset &filament, const Preset &machine);

// The family of `preset` as one string (parent and alias of its system ancestor); empty for a
// preset without a family. The first half of a key of PresetBundle::nozzle_filament_memory.
std::string family_key(const PresetCollection &filaments, const Preset &preset);
// Notes for the session that slot `slot` leaves user preset `name` because it does not fit the
// slot's tool head, so target_for_slot() restores it when a tool head returns to that size.
// A user preset that fits is not noted. No-op unless the rule is on.
void remember_user_preset(PresetBundle &bundle, size_t slot, const std::string &name);

enum class Reason {
    Unchanged,          // the slot holds a preset that fits its tool head
    Switched,           // `to` is the version for the size of the tool head
    HomeVersionUsed,    // no version for that size; `to` is the version of the printer preset
    NoVersion,          // no version for that size, and the slot holds the one of the printer preset already
    NoFamily,           // the preset belongs to no family (no parent, no alias)
    UserPresetKept,     // a user preset without exactly one counterpart for that size
    DirtyKept,          // the preset carries unsaved changes that no other slot would keep
    Skipped             // no tool head, no name, a placeholder, an unknown preset, rule off
};

struct SlotTarget
{
    size_t      slot { 0 };
    size_t      head { no_head };
    std::string from;                       // the name the slot holds, as it holds it
    std::string to;                         // the name it should hold: another preset when Switched / HomeVersionUsed,
                                            // else the preset it holds (under its present name, had it been renamed)
    Reason      reason { Reason::Skipped };
    bool        size_agnostic { false };    // fits, restricts no printer, and the tool head is off the home size
    std::string suggestion;                 // UserPresetKept: the system version for the size of the tool head
    double      head_size { 0. };
    double      preset_size { 0. };         // preset_nozzle_size() of `from`

    bool switches() const { return (reason == Reason::Switched || reason == Reason::HomeVersionUsed) && to != from; }
};

// What slot `slot` should hold. `heads` names the tool heads a pass moves (empty: all of them); it
// only matters for DirtyKept, where a slot of another tool head counts as one that keeps its
// preset. Never yields another material: `to` is `from` or a preset of its family.
SlotTarget target_for_slot(const PresetBundle &bundle, const State &state, size_t slot, const std::vector<size_t> &heads = {});

// The name of a reason ("Switched"), for the log.
const char* reason_name(Reason reason);
// One log line per slot: "NozzleFilament: trigger= filament= head= size= flow= from= to= reason="
// plus preset_size, size_agnostic and suggestion when set. `head_flow`: NozzleVolumeType per tool
// head (Standard beyond it). A slot without a tool head prints "-" for head, size and flow.
std::string describe_target(const SlotTarget &target, const std::string &trigger, const std::vector<int> &head_flow);
// describe_target() of every target on the info level of the log; the flow types are
// "nozzle_volume_type" of the project config. `trigger` names the event that made the pass, or
// the check that only reports.
void log_targets(const PresetBundle &bundle, const std::vector<SlotTarget> &targets, const std::string &trigger);

// The name resources/info/nozzle_incompatibles.json uses for `preset_name`: the alias of its
// system ancestor, else the part of the name in front of the '@'.
std::string blacklist_name(const PresetCollection &filaments, const std::string &preset_name);

// The blacklisted names for a nozzle of the given size and flow type (NozzleVolumeType as int).
using NozzleBlacklist = std::function<std::vector<std::string>(double nozzle_size, int nozzle_volume_type)>;
// (nozzle size, flow type) -> the `names` blacklisted for that nozzle, each filament rated against
// the nozzle of its tool head `filament_head[f]`; a filament without a tool head is not rated.
// A head beyond `head_volume_type` takes the first type.
std::map<std::pair<double, int>, std::set<std::string>> blacklisted_by_head(const std::vector<std::string> &names, const std::vector<size_t> &filament_head,
                                                                           const std::vector<double> &head_size, const std::vector<int> &head_volume_type,
                                                                           const NozzleBlacklist &blacklist);

} // namespace NozzleFilament
} // namespace Slic3r
