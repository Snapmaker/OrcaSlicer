#include "PerHeadProcess.hpp"

#include "libslic3r.h"
#include "Config.hpp"
#include "LocalesUtils.hpp"
#include "NozzleFilamentPresets.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <memory>

namespace Slic3r { namespace PerHeadProcess {

const char *const source_column_key = "print_extruder_source_column";
const char *const source_flow_key   = "print_extruder_source_flow";
const char *const flow_count_key    = "print_extruder_flow_count";
const char *const record_key        = "extruder_process_preset";
const char *const choice_key        = "extruder_process_choice";
const char *const flow_key          = "extruder_process_flow";

const std::set<std::string>& composed_keys()
{
    static const std::set<std::string> keys = {
        // speeds
        "initial_layer_speed", "initial_layer_infill_speed", "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed",
        "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed", "overhang_1_4_speed", "overhang_2_4_speed",
        "overhang_3_4_speed", "overhang_4_4_speed", "bridge_speed", "internal_bridge_speed", "gap_infill_speed", "support_speed",
        "support_interface_speed",
        // accelerations
        "default_acceleration", "bridge_acceleration", "initial_layer_acceleration", "outer_wall_acceleration",
        "inner_wall_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration", "top_surface_acceleration",
        // jerk / junction deviation
        "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "infill_jerk", "top_surface_jerk", "initial_layer_jerk",
        "default_junction_deviation",
    };
    return keys;
}

std::string quality_class(const std::string &preset_name)
{
    const size_t at = preset_name.find(" @");
    if (at == std::string::npos)
        return std::string();
    const std::string head = preset_name.substr(0, at);
    // The leading layer height token: digits and dots, an optional "mm", one blank. A name that
    // starts with no digit keeps its whole text ("Standard @X").
    size_t pos = 0;
    while (pos < head.size() && (std::isdigit(static_cast<unsigned char>(head[pos])) || head[pos] == '.'))
        ++pos;
    if (pos == 0)
        return head;
    if (head.compare(pos, 2, "mm") == 0)
        pos += 2;
    if (pos >= head.size() || head[pos] != ' ')
        return std::string();
    return head.substr(pos + 1);
}

namespace {

bool same_height(double a, double b) { return std::abs(a - b) < EPSILON; }

double layer_height_of(const Preset &preset)
{
    const auto *option = preset.config.option<ConfigOptionFloat>("layer_height");
    return option == nullptr ? 0. : option->value;
}

std::string default_print_profile_of(const Preset &machine)
{
    const auto *option = machine.config.option<ConfigOptionString>("default_print_profile");
    return option == nullptr ? std::string() : option->value;
}

bool compatible(const PresetBundle &bundle, const Preset &process, const Preset &machine)
{
    return is_compatible_with_printer(bundle.prints.get_preset_with_vendor_profile(process), bundle.printers.get_preset_with_vendor_profile(machine));
}

// The system process presets compatible with `machine`, `selected` excluded, whatever their
// visibility; in the order of the collection, which is the order of the names.
std::vector<const Preset*> candidates_for(const PresetBundle &bundle, const Preset &machine, const Preset &selected)
{
    std::vector<const Preset*> out;
    for (const Preset &preset : bundle.prints)
        if (preset.is_system && preset.name != selected.name && compatible(bundle, preset, machine))
            out.emplace_back(&preset);
    return out;
}

// The system preset the selected print preset is or inherits from; nullptr for a detached or
// imported preset, whose get_selected_preset_parent() is itself, the default preset or nullptr.
const Preset* system_parent(const PresetBundle &bundle)
{
    const Preset *parent = bundle.prints.get_selected_preset_parent();
    return parent == nullptr || !parent->is_system ? nullptr : parent;
}

} // namespace

std::set<std::string> all_edited_keys(const PresetBundle &bundle)
{
    std::set<std::string> out;
    const Preset *parent = system_parent(bundle);
    if (parent == nullptr)
        return out;
    const DynamicPrintConfig &edited = bundle.prints.get_edited_preset().config;
    DynamicPrintConfig        storage;
    const DynamicPrintConfig &reference = reference_in_layout_of(edited, parent->config, storage);
    // The shared columns: every column of a narrow preset (today's whole-key comparison), the
    // id-0 columns of a wide one, so that a value set for one tool head leaves its key unedited.
    std::vector<size_t> shared;
    {
        const Layout layout = layout_of(edited);
        for (size_t column = 0; column < layout.size(); ++column)
            if (!is_wide(edited) || layout.ids[column] == 0)
                shared.emplace_back(column);
    }
    for (const std::string &key : composed_keys()) {
        const auto *mine   = dynamic_cast<const ConfigOptionVectorBase *>(edited.option(key));
        const auto *theirs = dynamic_cast<const ConfigOptionVectorBase *>(reference.option(key));
        if (mine == nullptr || theirs == nullptr) {
            if (mine != theirs)
                out.insert(key);
            continue;
        }
        if (mine->type() != theirs->type()) {
            out.insert(key);
            continue;
        }
        const std::vector<std::string> a = mine->vserialize(), b = theirs->vserialize();
        if (shared.empty() || a.size() != b.size()) {
            if (a != b)
                out.insert(key);
            continue;
        }
        for (size_t column : shared)
            if (column < a.size() && column < b.size() && a[column] != b[column]) {
                out.insert(key);
                break;
            }
    }
    return out;
}

namespace {

// The class of a preset, its system parent's when the name has none (a user preset named "My
// fast PLA" that inherits 0.20mm Standard is Standard); a chain of user presets is followed.
std::string quality_class_of(const PresetBundle &bundle, const Preset &preset)
{
    const Preset *current = &preset;
    for (int depth = 0; current != nullptr && depth < 8; ++depth) {
        const std::string quality = quality_class(current->name);
        if (!quality.empty() || current->is_system)
            return quality;
        current = bundle.prints.get_preset_parent(*current);
    }
    return std::string();
}

// Quality classes tried in order when the head's size has no preset of class `quality`; Color
// Mixing takes High Quality's values (outer wall 60, acceleration 4000). An empty class tries the
// classless candidates first; a size without a Standard preset falls to the machine's default.
std::vector<std::string> class_ladder(const std::string &quality)
{
    if (quality == "Standard")
        return {"Standard"};
    if (quality == "High Quality" || quality == "Color Mixing")
        return {"High Quality", "Standard"};
    return {quality, "Standard"};
}

// The candidate nearest to `target_height`, a tie going to the thinner one; nullptr for none.
const Preset *nearest_height(const std::vector<const Preset *> &candidates, double target_height)
{
    const Preset *nearest = nullptr;
    for (const Preset *candidate : candidates) {
        if (nearest == nullptr) {
            nearest = candidate;
            continue;
        }
        const double distance = std::abs(layer_height_of(*candidate) - target_height);
        const double best     = std::abs(layer_height_of(*nearest) - target_height);
        if (distance < best - EPSILON || (same_height(distance, best) && layer_height_of(*candidate) < layer_height_of(*nearest)))
            nearest = candidate;
    }
    return nearest;
}

} // namespace

std::string plate_quality_class(const PresetBundle &bundle)
{
    if (bundle.prints.get_selected_idx() == size_t(-1))
        return std::string();
    return quality_class_of(bundle, bundle.prints.get_selected_preset());
}

double target_layer_height(const PresetBundle &bundle, size_t head, bool *preferred)
{
    const auto  *heights = bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("extruder_layer_height");
    const double height  = heights != nullptr && head < heights->values.size() ? heights->values[head] : 0.;
    if (preferred != nullptr)
        *preferred = height > 0.;
    if (height > 0.)
        return height;
    return bundle.prints.get_selected_idx() == size_t(-1) ? 0. : layer_height_of(bundle.prints.get_selected_preset());
}

const Preset* source_for_head(const PresetBundle &bundle, const Preset &machine, double preferred_layer_height,
                              const Preset &selected, const std::string &explicit_name, Reason &reason,
                              const std::function<bool(const Preset &)> &accept, RuleTrace *trace)
{
    std::vector<const Preset*> candidates = candidates_for(bundle, machine, selected);
    if (accept)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&accept](const Preset *candidate) { return !accept(*candidate); }),
                         candidates.end());
    auto found = [&reason, trace](const Preset *preset, Reason why, Step step, const std::string &class_used = std::string()) {
        reason = why;
        if (trace != nullptr) {
            trace->step       = step;
            trace->class_used = class_used;
        }
        return preset;
    };

    // `reason` is an output: its incoming value says nothing about this call. An explicit name: any
    // installed preset (a vendor rename resolved), system or user, compatible with the machine,
    // visible or not; a name that is not installed or does not fit falls to the rule.
    Reason by_rule = Reason::Derived;
    if (!explicit_name.empty()) {
        const Preset *named = resolve_chosen(bundle, explicit_name);
        if (named == nullptr)
            by_rule = Reason::NotInstalled;
        else if (compatible(bundle, *named, machine) && (!accept || accept(*named)))
            return found(named, Reason::Explicit, Step::Chosen);
        else
            by_rule = Reason::Unfit;
    }

    // The quality rule: the plate's class, then its ladder; within a class the layer height nearest
    // the head's preferred height, else the saved selected preset's height (the edited copy may hold
    // the layer height planner's grid).
    const double      target_height = preferred_layer_height > 0. ? preferred_layer_height : layer_height_of(selected);
    const std::string quality       = quality_class_of(bundle, selected);
    const std::string default_name  = default_print_profile_of(machine);

    for (const std::string &class_name : class_ladder(quality)) {
        std::vector<const Preset *> of_class;
        for (const Preset *candidate : candidates)
            if (quality_class(candidate->name) == class_name)
                of_class.emplace_back(candidate);
        if (const Preset *nearest = nearest_height(of_class, target_height); nearest != nullptr)
            return found(nearest, by_rule, class_name == quality ? Step::SameQuality : Step::ClassLadder, class_name);
    }

    // With a filter: the nearest layer height of any class among the accepted presets (the filter
    // may have left no preset of any class of the ladder; a preset with the wanted column beats none).
    if (accept)
        if (const Preset *nearest = nearest_height(candidates, target_height); nearest != nullptr)
            return found(nearest, by_rule, Step::ClassLadder, quality_class(nearest->name));

    // No class matched: the machine preset's default process preset, else the first candidate by name.
    if (!default_name.empty())
        for (const Preset &preset : bundle.prints)
            if (preset.is_system && preset.name == default_name && preset.name != selected.name && (!accept || accept(preset)))
                return found(&preset, by_rule, Step::SizeDefault);
    if (!candidates.empty())
        return found(candidates.front(), by_rule, Step::FirstByName);

    return found(nullptr, Reason::NoProcessPreset, Step::SelectedPreset);
}

namespace {

// head_sources with or without the project's choices (the latter says what the rule alone would
// give a chosen head: Source::automatic).
std::vector<Source> compute_sources(const PresetBundle &bundle, bool with_choices)
{
    std::vector<Source> out;
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    if (!state.rule_on)
        return out;
    const Preset &printer  = bundle.printers.get_edited_preset();
    const Preset &selected = bundle.prints.get_selected_preset();
    const Preset *parent   = system_parent(bundle);
    const double  home     = NozzleFilament::home_nozzle_size(printer.config);
    const auto   *heights  = printer.config.option<ConfigOptionFloats>("extruder_layer_height");
    const std::set<std::string> edited = all_edited_keys(bundle);
    auto split_keys = [&edited](Source &source) {
        source.composed_keys.clear();
        source.kept_keys.clear();
        for (const std::string &key : composed_keys())
            (edited.count(key) > 0 ? source.kept_keys : source.composed_keys).emplace_back(key);
    };

    out.resize(state.head_size.size());
    for (size_t head = 0; head < out.size(); ++head) {
        Source &source = out[head];
        source.head        = head;
        source.preset      = &selected;
        source.flow        = effective_flow(bundle, head);
        source.flow_chosen = flow_chosen(bundle, head);
        const Preset *machine   = head < state.head_machine.size() && state.head_machine[head] != nullptr ? state.head_machine[head] : &printer;
        const bool    home_size = machine == &printer && (home <= 0. || same_height(state.head_size[head], home));
        source.chosen           = with_choices ? chosen_of(bundle, head) : std::string();
        // Without a system parent every key counts as edited (all_edited_keys): nothing can be
        // composed, a choice included (it would take all 32 keys with no way to tell what the user
        // edited). The guard stays ahead of the choice.
        if (parent == nullptr) {
            source.reason = Reason::NoParent;
            if (!source.chosen.empty()) {
                source.chosen_state  = ChosenState::Inactive;
                source.chosen_reason = "no parent";
            }
            continue;
        }
        // The choice: an explicit act applies whether the preference is on or off and on a head of
        // the home size. One that is not installed or not made for the head's size stays in the
        // project, inactive, and the rule prints (it applies again when it fits).
        Reason carried          = Reason::Derived;
        bool   same_as_selected = false;
        if (!source.chosen.empty()) {
            const Preset *named = resolve_chosen(bundle, source.chosen);
            std::string   why;
            if (named == nullptr) {
                source.chosen_state = ChosenState::NotInstalled;
                carried             = Reason::NotInstalled;
            } else if (!fits(bundle, *named, head, &why)) {
                source.chosen_state  = ChosenState::Unfit;
                source.chosen_reason = why;
                carried              = Reason::Unfit;
            } else if (named->name == selected.name) {
                source.chosen_state = ChosenState::SameAsSelected;
                source.step         = Step::Chosen;
                same_as_selected    = true;
            } else {
                source.chosen_state = ChosenState::Applied;
                source.step         = Step::Chosen;
                source.reason       = Reason::Explicit;
                source.derived      = true;
                source.preset       = named;
                split_keys(source);
                continue;
            }
        }
        if (!bundle.process_follows_nozzle) {
            source.reason = Reason::Off;
            continue;
        }
        if (machine == &printer) {
            source.reason = home_size ? Reason::HomeSize : Reason::NoMachinePreset;
            continue;
        }
        if (same_as_selected) {
            // An off-size head whose choice names the selected preset (a printer switch after the
            // choice): the choice applies as a composition of nothing.
            source.reason = Reason::Explicit;
            continue;
        }
        const double preferred = heights != nullptr && head < heights->values.size() ? heights->values[head] : 0.;
        Reason        reason   = Reason::Derived;
        RuleTrace     trace;
        const Preset *by_rule  = source_for_head(bundle, *machine, preferred, selected, std::string(), reason, {}, &trace);
        if (by_rule == nullptr) {
            source.reason = Reason::NoProcessPreset;
            continue;
        }
        source.preset     = by_rule;
        source.derived    = true;
        source.reason     = carried;
        source.step       = trace.step;
        source.class_used = trace.class_used;
        split_keys(source);
    }
    // The edited copy of the selected preset: holds unsaved per-head values (compose reads it too);
    // its shared columns tell has_high_flow_values whether a home-size head has High Flow values of
    // its own (once any head has a value the copy is wide and every head owns a High Flow column).
    const DynamicPrintConfig &edited_config = bundle.prints.get_edited_preset().config;
    // The High Flow rule: a head whose effective flow is High Flow and whose source has no High Flow
    // column takes the preset of its size that has one. Heads the size rule left alone stay; a chosen
    // head keeps its choice, its Standard column serving High Flow (compose, own_standard_variants).
    if (bundle.process_follows_nozzle && parent != nullptr)
        for (size_t head = 0; head < out.size(); ++head) {
            Source &source = out[head];
            if (source.flow != nvtHighFlow || source.step == Step::Chosen)
                continue;
            if (source.reason != Reason::HomeSize && source.reason != Reason::Derived && source.reason != Reason::NotInstalled && source.reason != Reason::Unfit)
                continue;
            const bool has = source.derived && source.preset != nullptr ? column_for_head(source.preset->config, head, nvtHighFlow, printer.config) >= 0 :
                                                                          has_high_flow_values(edited_config, head, printer.config);
            if (has)
                continue;
            const Preset *machine   = head < state.head_machine.size() && state.head_machine[head] != nullptr ? state.head_machine[head] : &printer;
            const double  preferred = heights != nullptr && head < heights->values.size() ? heights->values[head] : 0.;
            Reason        reason    = Reason::Derived;
            RuleTrace     trace;
            const Preset *by_rule   = source_for_head(bundle, *machine, preferred, selected, std::string(), reason,
                                                      [&printer, head](const Preset &candidate) { return column_for_head(candidate.config, head, nvtHighFlow, printer.config) >= 0; },
                                                      &trace);
            if (by_rule == nullptr)
                continue;
            source.preset     = by_rule;
            source.derived    = true;
            source.reason     = Reason::HighFlow;
            source.step       = trace.step;
            source.class_used = trace.class_used;
            split_keys(source);
        }
    // The keys set for a tool head on the Speed page, whatever its reason.
    for (Source &source : out)
        source.overridden_keys = head_override_keys(edited_config, source.head);
    return out;
}

} // namespace

std::vector<Source> head_sources(const PresetBundle &bundle)
{
    std::vector<Source> out = compute_sources(bundle, true);
    // What the rule alone would give a chosen head, for the texts of the picker.
    bool any_chosen_head = false;
    for (const Source &source : out)
        any_chosen_head = any_chosen_head || source.step == Step::Chosen;
    if (any_chosen_head) {
        const std::vector<Source> plain = compute_sources(bundle, false);
        for (size_t head = 0; head < out.size() && head < plain.size(); ++head)
            if (out[head].step == Step::Chosen)
                out[head].automatic = plain[head].derived ? plain[head].preset : nullptr;
    }
    return out;
}

// ---- The chosen source --------------------------------------------------------------------------

std::string chosen_of(const PresetBundle &bundle, size_t head)
{
    const auto *option = bundle.project_config.option<ConfigOptionStrings>(choice_key);
    return option != nullptr && head < option->values.size() ? option->values[head] : std::string();
}

void set_chosen(PresetBundle &bundle, size_t head, const std::string &name)
{
    auto *option = bundle.project_config.option<ConfigOptionStrings>(choice_key, true);
    if (option->values.size() <= head)
        option->values.resize(head + 1);
    option->values[head] = name;
    // [] while no head has a choice: a project without choices equals one saved before the key.
    if (std::all_of(option->values.begin(), option->values.end(), [](const std::string &entry) { return entry.empty(); }))
        option->values.clear();
}

bool any_chosen(const PresetBundle &bundle)
{
    const auto *option = bundle.project_config.option<ConfigOptionStrings>(choice_key);
    return option != nullptr && std::any_of(option->values.begin(), option->values.end(), [](const std::string &entry) { return !entry.empty(); });
}

bool active(const PresetBundle &bundle)
{
    return bundle.process_follows_nozzle || any_chosen(bundle) || any_flow_chosen(bundle.project_config);
}

// ---- The chosen flow --------------------------------------------------------------------------

namespace {

// The flow type an entry of flow_key names; false for an empty or unknown entry.
bool parse_flow(const std::string &entry, NozzleVolumeType &flow)
{
    for (NozzleVolumeType candidate : {nvtStandard, nvtHighFlow})
        if (entry == get_nozzle_volume_type_string(candidate)) {
            flow = candidate;
            return true;
        }
    return false;
}

// The nozzle of type `nozzle` (as stored, Hybrid included) can print the speeds column of `flow`.
bool nozzle_offers(NozzleVolumeType nozzle, NozzleVolumeType flow)
{
    if (flow == nvtStandard)
        return true;
    return flow == nvtHighFlow && (nozzle == nvtHighFlow || nozzle == nvtHybrid);
}

NozzleVolumeType stored_nozzle_type(const DynamicPrintConfig &config, size_t head)
{
    const auto *flows = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    return flows != nullptr && head < flows->values.size() ? NozzleVolumeType(flows->values[head]) : nvtStandard;
}

} // namespace

NozzleVolumeType nozzle_flow(const DynamicPrintConfig &config, size_t head)
{
    const NozzleVolumeType stored = stored_nozzle_type(config, head);
    return stored == nvtHybrid ? nvtStandard : stored;
}

NozzleVolumeType effective_flow(const DynamicPrintConfig &config, size_t head)
{
    const NozzleVolumeType own   = nozzle_flow(config, head);
    const auto            *entry = config.option<ConfigOptionStrings>(flow_key);
    if (entry == nullptr || head >= entry->values.size() || entry->values[head].empty())
        return own;
    NozzleVolumeType wanted = own;
    if (!parse_flow(entry->values[head], wanted))
        return own;
    return nozzle_offers(stored_nozzle_type(config, head), wanted) ? wanted : own;
}

NozzleVolumeType effective_flow(const PresetBundle &bundle, size_t head)
{
    return effective_flow(bundle.project_config, head);
}

bool flow_chosen(const DynamicPrintConfig &config, size_t head)
{
    return effective_flow(config, head) != nozzle_flow(config, head);
}

bool flow_chosen(const PresetBundle &bundle, size_t head)
{
    return flow_chosen(bundle.project_config, head);
}

bool any_flow_chosen(const DynamicPrintConfig &config)
{
    const auto *entry = config.option<ConfigOptionStrings>(flow_key);
    if (entry == nullptr)
        return false;
    for (size_t head = 0; head < entry->values.size(); ++head)
        if (!entry->values[head].empty() && flow_chosen(config, head))
            return true;
    return false;
}

std::string chosen_flow_of(const PresetBundle &bundle, size_t head)
{
    const auto *entry = bundle.project_config.option<ConfigOptionStrings>(flow_key);
    return entry != nullptr && head < entry->values.size() ? entry->values[head] : std::string();
}

void set_chosen_flow(PresetBundle &bundle, size_t head, NozzleVolumeType flow)
{
    auto *entry = bundle.project_config.option<ConfigOptionStrings>(flow_key, true);
    if (entry->values.size() <= head)
        entry->values.resize(head + 1);
    // The nozzle's own flow is no entry: a head prints its nozzle's column unless told otherwise.
    entry->values[head] = flow == nozzle_flow(bundle.project_config, head) ? std::string() : get_nozzle_volume_type_string(flow);
    if (std::all_of(entry->values.begin(), entry->values.end(), [](const std::string &value) { return value.empty(); }))
        entry->values.clear();
}

const Preset *resolve_chosen(const PresetBundle &bundle, const std::string &name)
{
    return name.empty() ? nullptr : bundle.prints.find_preset(name, false);
}

bool fits(const PresetBundle &bundle, const Preset &preset, size_t head, std::string *reason)
{
    auto answer = [reason](bool fit, const std::string &why) {
        if (reason != nullptr)
            *reason = why;
        return fit;
    };
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    if (!state.rule_on || head >= state.head_size.size())
        return answer(false, std::string());
    const Preset &printer = bundle.printers.get_edited_preset();
    const Preset *machine = head < state.head_machine.size() && state.head_machine[head] != nullptr ? state.head_machine[head] : &printer;
    const double  home    = NozzleFilament::home_nozzle_size(printer.config);
    if (machine == &printer && !(home <= 0. || same_height(state.head_size[head], home)))
        return answer(false, "no machine preset");
    // A preset without a compatible_printers list and without a condition is compatible with every
    // printer by itself (is_compatible_with_printer): its system root says what it is made for.
    const Preset *judged = &preset;
    if (const auto *list = preset.config.option<ConfigOptionStrings>("compatible_printers");
        (list == nullptr || list->values.empty()) && preset.compatible_printers_condition().empty())
        if (const Preset *root = bundle.prints.get_preset_base(preset); root != nullptr && root->is_system)
            judged = root;
    if (compatible(bundle, *judged, *machine))
        return answer(true, std::string());
    // The size of the preset's machine, when a machine preset of this printer model accepts it.
    const std::string model = printer.config.opt_string("printer_model");
    for (const Preset &candidate : bundle.printers)
        if (candidate.is_system && candidate.config.opt_string("printer_model") == model && compatible(bundle, *judged, candidate))
            if (const auto *sizes = candidate.config.option<ConfigOptionFloats>("nozzle_diameter"); sizes != nullptr && !sizes->values.empty())
                return answer(false, "size " + float_to_string_decimal_point(sizes->values.front()));
    return answer(false, std::string());
}

void resolve_renamed_choices(PresetBundle &bundle)
{
    auto *option = bundle.project_config.option<ConfigOptionStrings>(choice_key);
    if (option == nullptr)
        return;
    for (std::string &entry : option->values)
        if (!entry.empty())
            if (const Preset *preset = resolve_chosen(bundle, entry); preset != nullptr && preset->name != entry) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the chosen process preset %1% is %2% now") % entry % preset->name;
                entry = preset->name;
            }
}

std::vector<Candidate> picker_candidates(const PresetBundle &bundle, size_t head)
{
    std::vector<Candidate> out;
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    if (!state.rule_on || head >= state.head_size.size() || bundle.prints.get_selected_idx() == size_t(-1))
        return out;
    const Preset &printer = bundle.printers.get_edited_preset();
    const Preset *machine = head < state.head_machine.size() && state.head_machine[head] != nullptr ? state.head_machine[head] : &printer;
    const double  home    = NozzleFilament::home_nozzle_size(printer.config);
    if (machine == &printer && !(home <= 0. || same_height(state.head_size[head], home)))
        return out;
    const Preset &selected = bundle.prints.get_selected_preset();
    // The flow whose column the head prints: the nozzle's, or the Standard column chosen for a High Flow nozzle.
    const NozzleVolumeType flow = effective_flow(bundle, head) == nvtHighFlow ? nvtHighFlow : nvtStandard;
    // What the rule gives the head today: its derived preset, else the selected preset.
    const std::vector<Source> plain     = compute_sources(bundle, false);
    const Preset             *automatic = head < plain.size() && plain[head].derived && plain[head].preset != nullptr ? plain[head].preset : &selected;
    auto add = [&](const Preset &preset, Candidate::Group group) {
        Candidate candidate;
        candidate.preset          = &preset;
        candidate.group           = group;
        candidate.has_flow_column = source_column(preset, head, flow, printer.config) >= 0;
        candidate.is_automatic    = preset.name == automatic->name;
        candidate.is_selected     = preset.name == selected.name;
        out.emplace_back(std::move(candidate));
    };
    for (const Preset &preset : bundle.prints)
        if (preset.is_system && !preset.is_default && !preset.is_from_bundle() && compatible(bundle, preset, *machine))
            add(preset, Candidate::System);
    for (const Preset &preset : bundle.prints)
        if (!preset.is_system && !preset.is_default && !preset.is_project_embedded && !preset.is_from_bundle() && fits(bundle, preset, head))
            add(preset, Candidate::User);
    for (const Preset &preset : bundle.prints)
        if (preset.is_project_embedded && !preset.is_from_bundle() && fits(bundle, preset, head))
            add(preset, Candidate::Project);
    return out;
}

int composed_column(const Source &source, NozzleVolumeType flow, const DynamicPrintConfig &printer)
{
    if (!source.derived || source.preset == nullptr)
        return -1;
    const int column = source_column(*source.preset, source.head, flow, printer);
    if (column >= 0 || source.step != Step::Chosen)
        return column;
    return shared_column(source.preset->config, nvtStandard);
}

std::vector<std::string> command_line_record(DynamicPrintConfig &config)
{
    std::vector<std::string> lines;
    if (auto *recorded = config.option<ConfigOptionStrings>(record_key); recorded != nullptr) {
        for (size_t head = 0; head < recorded->values.size(); ++head)
            if (!recorded->values[head].empty())
                lines.emplace_back((boost::format("extruder %1% printed with %2% in the application; the command line slices every extruder with the loaded process preset") % (head + 1) % recorded->values[head]).str());
        // The output's record tells what was sliced here: every tool head with the loaded preset.
        recorded->values.clear();
    }
    if (const auto *chosen = config.option<ConfigOptionStrings>(choice_key); chosen != nullptr)
        for (size_t head = 0; head < chosen->values.size(); ++head)
            if (!chosen->values[head].empty())
                lines.emplace_back((boost::format("extruder %1% is set to take its speeds from %2% in the application; the command line slices every extruder with the loaded process preset") % (head + 1) % chosen->values[head]).str());
    if (const auto *flows = config.option<ConfigOptionStrings>(flow_key); flows != nullptr)
        for (size_t head = 0; head < flows->values.size(); ++head)
            if (!flows->values[head].empty())
                lines.emplace_back((boost::format("extruder %1% is set to print the %2% speeds of its nozzle in the application; the command line slices every extruder with the loaded process preset") % (head + 1) % flows->values[head]).str());
    return lines;
}

std::vector<Source> record_sources(PresetBundle &bundle)
{
    std::vector<Source> sources = head_sources(bundle);
    if (sources.empty())
        return sources;
    // The record stays empty, not one empty entry per head, while no head is derived: a project
    // saved without the key and a plate on which nothing is composed carry the same value, so the
    // apply that follows a load changes nothing (the key is a G-code export step of Print::apply).
    std::vector<std::string> record;
    for (size_t head = 0; head < sources.size(); ++head)
        if (sources[head].derived && sources[head].preset != nullptr) {
            record.resize(sources.size());
            record[head] = sources[head].preset->name;
        }
    // Written only when the value changes; an absent option is created only for a non-empty record.
    auto *option = bundle.project_config.option<ConfigOptionStrings>(record_key, !record.empty());
    if (option != nullptr && option->values != record)
        option->values = std::move(record);
    return sources;
}

std::vector<LoadReportEntry> load_report(const PresetBundle &bundle)
{
    std::vector<LoadReportEntry> out;
    const auto *recorded = bundle.project_config.option<ConfigOptionStrings>(record_key);
    if (recorded == nullptr || recorded->values.empty())
        return out;
    const std::vector<Source> sources = head_sources(bundle);
    if (sources.empty())
        return out;
    const NozzleFilament::State state = NozzleFilament::head_state(bundle);
    for (size_t head = 0; head < sources.size() && head < recorded->values.size(); ++head) {
        const std::string &record = recorded->values[head];
        if (record.empty())
            continue;
        const std::string current = sources[head].derived && sources[head].preset != nullptr ? sources[head].preset->name : std::string();
        if (current == record)
            continue;
        LoadReportEntry entry;
        entry.head               = head;
        entry.nozzle_size        = head < state.head_size.size() ? state.head_size[head] : 0.;
        entry.recorded           = record;
        entry.current            = current;
        entry.reason             = sources[head].reason;
        entry.recorded_installed = bundle.prints.find_preset(record, false) != nullptr;
        out.emplace_back(std::move(entry));
    }
    return out;
}

namespace {

struct Column
{
    size_t           head;
    NozzleVolumeType volume_type;
    std::string      variant;
};

// The extruder type of a tool head. An unexpanded config holds one extruder_type per printer
// variant column, so the head's first column (via printer_extruder_id) is used.
ExtruderType head_extruder_type(const DynamicPrintConfig &full, size_t head, size_t heads)
{
    const auto *types = full.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (types == nullptr || types->values.empty())
        return etDirectDrive;
    if (types->values.size() != heads)
        if (const auto *ids = full.option<ConfigOptionInts>("printer_extruder_id"); ids != nullptr && ids->values.size() == types->values.size())
            for (size_t column = 0; column < ids->values.size(); ++column)
                if (ids->values[column] == int(head) + 1)
                    return ExtruderType(types->values[column]);
    return ExtruderType(types->get_at(head));
}

// The columns Print::apply narrows the process to: one per tool head, or one per (tool head x
// volume type) when the printer declares several types for a head (the slot walk of
// DynamicPrintConfig::update_values_to_printer_extruders).
std::vector<Column> composed_columns(const DynamicPrintConfig &full)
{
    std::vector<Column> columns;
    int extruder_count = 1;
    full.support_different_extruders(extruder_count);
    if (extruder_count <= 1)
        return columns;
    std::vector<std::vector<NozzleVolumeType>> volume_types;
    const int volume_count = full.get_extruder_nozzle_volume_count(extruder_count, volume_types);
    const auto *flows = full.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (flows == nullptr || flows->values.empty())
        return columns;
    for (int head = 0; head < extruder_count; ++head) {
        const ExtruderType extruder_type = head_extruder_type(full, size_t(head), size_t(extruder_count));
        const auto head_flow             = NozzleVolumeType(flows->get_at(size_t(head)));
        const bool per_type      = volume_count > extruder_count || head_flow == nvtHybrid;
        std::vector<NozzleVolumeType> slots = per_type ? volume_types[size_t(head)] : std::vector<NozzleVolumeType>{head_flow};
        for (NozzleVolumeType flow : slots)
            columns.push_back({size_t(head), flow, get_extruder_variant_string(extruder_type, flow == nvtHybrid ? nvtStandard : flow)});
    }
    return columns;
}

} // namespace

bool compose(DynamicPrintConfig &full, const std::set<std::string> &edited, std::vector<Source> &sources)
{
    bool any_derived = false;
    for (const Source &source : sources)
        any_derived = any_derived || (source.derived && source.preset != nullptr && !source.composed_keys.empty()) || source.flow_chosen;
    if (!any_derived)
        return false;
    // The flow a column of tool head `head` is read at: the column's own type, or the flow chosen
    // for the head (a High Flow nozzle printing the Standard speeds) for every column of that head.
    auto lookup_flow = [&sources](const Column &column) {
        return column.head < sources.size() && sources[column.head].flow_chosen ? sources[column.head].flow : column.volume_type;
    };

    const std::vector<Column> columns = composed_columns(full);
    if (columns.empty())
        return false;
    int heads = 1;
    full.support_different_extruders(heads);

    // The selected preset's column that stands in for each composed column, resolved on the
    // layout of the selected preset before it is rewritten.
    std::vector<int> source_columns;
    source_columns.reserve(columns.size());
    for (const Column &column : columns) {
        const int index = full.get_index_for_extruder(int(column.head) + 1, "print_extruder_id", head_extruder_type(full, column.head, size_t(heads)),
                                                      lookup_flow(column), "print_extruder_variant");
        source_columns.emplace_back(index >= 0 ? index : 0);
    }

    // The source preset's column for each composed column of a derived head, -1 for a column the
    // source has no values for (the selected preset's column serves it).
    std::vector<int> preset_columns(columns.size(), -1);
    for (size_t c = 0; c < columns.size(); ++c) {
        const Column &column = columns[c];
        if (column.head >= sources.size() || !sources[column.head].derived || sources[column.head].preset == nullptr)
            continue;
        Source &source = sources[column.head];
        int     index  = source_column(*source.preset, column.head, lookup_flow(column), full);
        if (index < 0) {
            // No source column for the flow: a chosen preset serves its own Standard column; an
            // automatic source leaves the flow to the selected preset's column (head_sources has
            // already applied the High Flow rule).
            switch (source.step) {
            case Step::Chosen: {
                index = shared_column(source.preset->config, nvtStandard);
                std::vector<int> &own = source.own_standard_variants;
                if (std::find(own.begin(), own.end(), int(column.volume_type)) == own.end())
                    own.emplace_back(int(column.volume_type));
                break;
            }
            default: {
                std::vector<int> &fallback = source.fallback_variants;
                if (std::find(fallback.begin(), fallback.end(), int(column.volume_type)) == fallback.end())
                    fallback.emplace_back(int(column.volume_type));
                break;
            }
            }
        }
        preset_columns[c] = index;
    }

    // The flow-only space of the selected preset, resolved before its layout is rewritten: the
    // position of each composed column's source column among the shared columns.
    const std::vector<std::string> flow_space = flow_space_variants(full);
    std::vector<int>               source_flow(columns.size(), -1);
    {
        const Layout selected_layout = layout_of(full);
        for (size_t c = 0; c < columns.size(); ++c) {
            const size_t s = size_t(source_columns[c]);
            if (s >= selected_layout.size())
                continue;
            const auto found = std::find(flow_space.begin(), flow_space.end(), selected_layout.variants[s]);
            if (found != flow_space.end())
                source_flow[c] = int(found - flow_space.begin());
        }
    }

    // The marker of the selected preset, kept aside: the loop below rewrites every variant key of
    // `full`, the marker among them, into the composed layout.
    DynamicPrintConfig marker_snapshot;
    if (const ConfigOption *marker = full.option(override_key); marker != nullptr)
        marker_snapshot.set_key_value(override_key, marker->clone());

    for (const std::string &key : print_options_with_variant) {
        if (key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        const auto *original = dynamic_cast<const ConfigOptionVectorBase*>(full.option(key));
        if (original == nullptr || original->empty())
            continue;
        const bool composed = composed_keys().count(key) > 0 && edited.count(key) == 0;
        std::unique_ptr<ConfigOptionVectorBase> selected(static_cast<ConfigOptionVectorBase*>(original->clone()));
        std::unique_ptr<ConfigOptionVectorBase> out(static_cast<ConfigOptionVectorBase*>(original->clone()));
        out->resize(columns.size());
        for (size_t c = 0; c < columns.size(); ++c) {
            const Column &column = columns[c];
            const ConfigOptionVectorBase *from = selected.get();
            int                           index = source_columns[c];
            // A value set for the tool head on the Speed page beats the source of its size.
            const bool marked = is_marked(marker_snapshot, size_t(index), key);
            if (composed && !marked && preset_columns[c] >= 0) {
                const auto *preset_option = dynamic_cast<const ConfigOptionVectorBase*>(sources[column.head].preset->config.option(key));
                if (preset_option != nullptr && !preset_option->empty() && preset_option->type() == out->type()) {
                    from  = preset_option;
                    index = preset_columns[c];
                }
            }
            out->set_at(from, c, size_t(index));
        }
        full.set_key_value(key, out.release());
    }

    std::vector<int>         ids;
    std::vector<std::string> variants;
    for (const Column &column : columns) {
        ids.emplace_back(int(column.head) + 1);
        variants.emplace_back(column.variant);
    }
    full.set_key_value("print_extruder_id",      new ConfigOptionInts(std::move(ids)));
    full.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::move(variants)));
    full.set_key_value(source_column_key,        new ConfigOptionInts(std::move(source_columns)));
    full.set_key_value(source_flow_key,          new ConfigOptionInts(std::move(source_flow)));
    full.set_key_value(flow_count_key,           new ConfigOptionInt(int(flow_space.size())));
    return true;
}


// ---- Values set per tool head ------------------------------------------------------------------

const char *const override_key = "print_extruder_override";

namespace {

// The value keys of the process layout: print_options_with_variant without the ids, the variants
// and the marker.
const std::vector<std::string> &value_keys()
{
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> out;
        for (const std::string &key : print_options_with_variant)
            if (key != "print_extruder_id" && key != "print_extruder_variant" && key != override_key)
                out.emplace_back(key);
        return out;
    }();
    return keys;
}

std::vector<std::string> split_marker(const std::string &entry)
{
    std::vector<std::string> out;
    std::string              key;
    for (char c : entry) {
        if (c == ',') {
            if (!key.empty())
                out.emplace_back(key);
            key.clear();
        } else if (c != ' ')
            key += c;
    }
    if (!key.empty())
        out.emplace_back(key);
    return out;
}

std::string join_marker(const std::set<std::string> &keys)
{
    std::string out;
    for (const std::string &key : keys)
        out += (out.empty() ? "" : ",") + key;
    return out;
}

// The marker option, created when absent, with one entry per column of the layout.
ConfigOptionStrings *marker_option(DynamicPrintConfig &config, size_t width)
{
    auto *marker = config.option<ConfigOptionStrings>(override_key, true);
    if (marker->values.size() != width)
        marker->values.resize(width, std::string());
    return marker;
}

std::set<std::string> marker_keys(const DynamicPrintConfig &config, size_t column)
{
    std::set<std::string> out;
    const auto *marker = config.option<ConfigOptionStrings>(override_key);
    if (marker == nullptr || column >= marker->values.size())
        return out;
    for (const std::string &key : split_marker(marker->values[column]))
        out.insert(key);
    return out;
}

void write_marker(DynamicPrintConfig &config, size_t column, const std::set<std::string> &keys)
{
    ConfigOptionStrings *marker = marker_option(config, layout_of(config).size());
    if (column < marker->values.size())
        marker->values[column] = join_marker(keys);
}

// A vector option copied from `source` column `from` into column `to` of the same or another
// option of the same type.
void copy_column(ConfigOptionVectorBase &target, const ConfigOptionVectorBase &source, size_t to, size_t from)
{
    if (target.type() != source.type() || source.empty())
        return;
    if (to >= target.size())
        target.resize(to + 1, &target);
    target.set_at(&source, to, from < source.size() ? from : 0);
}

// The column walk of relayout(): the source column of `from` for a target column (id, variant).
int source_column_for(const Layout &from, int id, const std::string &variant)
{
    if (from.size() == 0)
        return 0;
    for (size_t column = 0; column < from.size(); ++column)
        if (from.ids[column] == id && from.variants[column] == variant)
            return int(column);
    bool has_shared = false;
    for (size_t column = 0; column < from.size(); ++column)
        if (from.ids[column] == 0) {
            has_shared = true;
            if (from.variants[column] == variant)
                return int(column);
        }
    if (!has_shared)
        for (size_t column = 0; column < from.size(); ++column)
            if (from.variants[column] == variant)
                return int(column);
    for (size_t column = 0; column < from.size(); ++column)
        if (from.ids[column] == 0)
            return int(column);
    return 0;
}

bool ids_name_heads(const std::vector<int> &ids)
{
    std::set<int> distinct;
    for (int id : ids)
        if (id > 0)
            distinct.insert(id);
    return distinct.size() > 1;
}

// The id-0 column of the variant, else the first id-0 column, else -1.
int shared_column_of_variant(const Layout &layout, const std::string &variant)
{
    int first = -1;
    for (size_t column = 0; column < layout.size(); ++column)
        if (layout.ids[column] == 0) {
            if (layout.variants[column] == variant)
                return int(column);
            if (first < 0)
                first = int(column);
        }
    return first;
}

std::vector<int> distinct_heads(const Layout &layout)
{
    std::vector<int> heads;
    for (int id : layout.ids)
        if (id > 0 && std::find(heads.begin(), heads.end(), id - 1) == heads.end())
            heads.emplace_back(id - 1);
    std::sort(heads.begin(), heads.end());
    return heads;
}

} // namespace

Layout layout_of(const DynamicPrintConfig &config)
{
    Layout      out;
    const auto *ids      = config.option<ConfigOptionInts>("print_extruder_id");
    const auto *variants = config.option<ConfigOptionStrings>("print_extruder_variant");
    if (variants == nullptr)
        return out;
    out.variants = variants->values;
    if (ids != nullptr && ids->values.size() == variants->values.size())
        out.ids = ids->values;
    else
        out.ids.assign(out.variants.size(), 1);
    return out;
}

bool is_wide(const DynamicPrintConfig &config)
{
    const Layout layout = layout_of(config);
    return std::find(layout.ids.begin(), layout.ids.end(), 0) != layout.ids.end();
}

std::vector<std::string> shared_variants(const DynamicPrintConfig &config)
{
    const Layout             layout = layout_of(config);
    std::vector<std::string> out;
    if (is_wide(config)) {
        for (size_t column = 0; column < layout.size(); ++column)
            if (layout.ids[column] == 0)
                out.emplace_back(layout.variants[column]);
        return out;
    }
    if (ids_name_heads(layout.ids)) {
        for (const std::string &variant : layout.variants)
            if (std::find(out.begin(), out.end(), variant) == out.end())
                out.emplace_back(variant);
        return out;
    }
    return layout.variants;
}

size_t shared_width(const DynamicPrintConfig &config)
{
    return shared_variants(config).size();
}

std::vector<std::string> flow_space_variants(const DynamicPrintConfig &config)
{
    const Layout layout = layout_of(config);
    if (layout.size() == 0)
        return {};
    if (is_wide(config))
        return shared_variants(config);
    if (ids_name_heads(layout.ids))
        return {};
    std::vector<std::string> out;
    for (const std::string &variant : layout.variants) {
        if (std::find(out.begin(), out.end(), variant) != out.end())
            return {}; // two columns of one flow: no flow-only space
        out.emplace_back(variant);
    }
    return out;
}

Layout wide_layout(const DynamicPrintConfig &shared_from, const DynamicPrintConfig &printer)
{
    Layout out;
    for (const std::string &variant : shared_variants(shared_from)) {
        out.ids.emplace_back(0);
        out.variants.emplace_back(variant);
    }
    const auto *diameters = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t heads    = diameters == nullptr ? 0 : diameters->values.size();
    const auto *ids       = printer.option<ConfigOptionInts>("printer_extruder_id");
    const auto *variants  = printer.option<ConfigOptionStrings>("printer_extruder_variant");
    if (ids != nullptr && variants != nullptr && !ids->values.empty() && ids->values.size() == variants->values.size() &&
        (heads == 0 || ids->values.size() >= heads) && ids_name_heads(ids->values) == (heads > 1)) {
        for (size_t column = 0; column < ids->values.size(); ++column) {
            out.ids.emplace_back(ids->values[column]);
            out.variants.emplace_back(variants->values[column]);
        }
        return out;
    }
    if (const auto *list = printer.option<ConfigOptionStrings>("extruder_variant_list"); list != nullptr && list->values.size() == heads && heads > 0) {
        for (size_t head = 0; head < heads; ++head) {
            std::vector<std::string> tokens;
            boost::split(tokens, list->values[head], boost::is_any_of(","), boost::token_compress_on);
            for (std::string &token : tokens) {
                boost::trim(token);
                if (token.empty())
                    continue;
                out.ids.emplace_back(int(head) + 1);
                out.variants.emplace_back(token);
            }
        }
        if (out.size() > shared_width(shared_from))
            return out;
        out.ids.resize(shared_width(shared_from));
        out.variants.resize(shared_width(shared_from));
    }
    for (size_t head = 0; head < heads; ++head) {
        out.ids.emplace_back(int(head) + 1);
        out.variants.emplace_back(get_extruder_variant_string(etDirectDrive, nvtStandard));
    }
    return out;
}

void relayout(DynamicPrintConfig &config, const Layout &to)
{
    const Layout from = layout_of(config);
    if (from.size() == 0 || to.size() == 0)
        return;
    std::vector<int> source(to.size());
    for (size_t t = 0; t < to.size(); ++t)
        source[t] = source_column_for(from, to.ids[t], to.variants[t]);

    for (const std::string &key : value_keys()) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || option->empty())
            continue;
        std::unique_ptr<ConfigOptionVectorBase> out(static_cast<ConfigOptionVectorBase *>(option->clone()));
        out->resize(to.size(), option);
        for (size_t t = 0; t < to.size(); ++t)
            out->set_at(option, t, size_t(source[t]) < option->size() ? size_t(source[t]) : 0);
        config.set_key_value(key, out.release());
    }
    // The marker: a shared column carries none, a head column taken from a shared column neither.
    std::vector<std::string> marker(to.size());
    if (const auto *old = config.option<ConfigOptionStrings>(override_key); old != nullptr)
        for (size_t t = 0; t < to.size(); ++t) {
            const size_t s = size_t(source[t]);
            if (to.ids[t] != 0 && s < from.size() && from.ids[s] != 0 && s < old->values.size())
                marker[t] = old->values[s];
        }
    config.set_key_value(override_key, new ConfigOptionStrings(std::move(marker)));
    config.set_key_value("print_extruder_id", new ConfigOptionInts(to.ids));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(to.variants));

    // I4: a head that kept one column by exact match and took another from a shared column (two
    // flows per head after one) copies the kept column's marked keys and values to the new one, so
    // a value set for the head holds in every flow, as set_head_value writes it.
    for (size_t t = 0; t < to.size(); ++t) {
        if (to.ids[t] == 0 || (size_t(source[t]) < from.size() && from.ids[size_t(source[t])] != 0))
            continue;
        for (size_t s = 0; s < to.size(); ++s) {
            if (s == t || to.ids[s] != to.ids[t])
                continue;
            const std::set<std::string> keys = marker_keys(config, s);
            if (keys.empty())
                continue;
            for (const std::string &key : keys)
                if (auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key)); option != nullptr && t < option->size() && s < option->size())
                    copy_column(*option, *option, t, s);
            std::set<std::string> mine = marker_keys(config, t);
            mine.insert(keys.begin(), keys.end());
            write_marker(config, t, mine);
        }
    }
}

void widen(DynamicPrintConfig &config, const DynamicPrintConfig &printer)
{
    if (is_wide(config))
        return;
    relayout(config, wide_layout(config, printer));
}

void narrow(DynamicPrintConfig &config)
{
    if (!is_wide(config))
        return;
    const Layout        from = layout_of(config);
    std::vector<size_t> shared;
    for (size_t column = 0; column < from.size(); ++column)
        if (from.ids[column] == 0)
            shared.emplace_back(column);
    for (const std::string &key : value_keys()) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || option->empty())
            continue;
        std::unique_ptr<ConfigOptionVectorBase> out(static_cast<ConfigOptionVectorBase *>(option->clone()));
        out->resize(shared.size(), option);
        for (size_t t = 0; t < shared.size(); ++t)
            out->set_at(option, t, shared[t] < option->size() ? shared[t] : 0);
        config.set_key_value(key, out.release());
    }
    std::vector<std::string> variants;
    for (size_t column : shared)
        variants.emplace_back(from.variants[column]);
    config.set_key_value(override_key, new ConfigOptionStrings(std::vector<std::string>(shared.size(), std::string())));
    config.set_key_value("print_extruder_id", new ConfigOptionInts(std::vector<int>(shared.size(), 1)));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(std::move(variants)));
}

const DynamicPrintConfig &reference_in_layout_of(const DynamicPrintConfig &child, const DynamicPrintConfig &reference, DynamicPrintConfig &storage)
{
    if (!is_wide(child))
        return reference;
    const Layout child_layout = layout_of(child);
    if (child_layout.size() <= layout_of(reference).size())
        return reference;
    storage = reference;
    relayout(storage, child_layout);
    return storage;
}

bool variant_names_type(const std::string &variant, NozzleVolumeType type)
{
    if (type == nvtHybrid)
        type = nvtStandard;
    for (ExtruderType drive : {etDirectDrive, etBowden})
        if (variant == get_extruder_variant_string(drive, type))
            return true;
    return false;
}

int shared_column(const DynamicPrintConfig &config, NozzleVolumeType type)
{
    const Layout layout = layout_of(config);
    if (layout.size() == 0)
        return 0;
    int first_shared = -1;
    for (size_t column = 0; column < layout.size(); ++column)
        if (layout.ids[column] == 0) {
            if (variant_names_type(layout.variants[column], type))
                return int(column);
            if (first_shared < 0)
                first_shared = int(column);
        }
    if (first_shared >= 0)
        return first_shared;
    // Narrow: a flow-only layout answers the column of the type, anything else its first column.
    if (!ids_name_heads(layout.ids))
        for (size_t column = 0; column < layout.size(); ++column)
            if (variant_names_type(layout.variants[column], type))
                return int(column);
    return 0;
}

std::vector<int> head_columns(const DynamicPrintConfig &config, size_t head)
{
    std::vector<int> out;
    const Layout     layout = layout_of(config);
    for (size_t column = 0; column < layout.size(); ++column)
        if (layout.ids[column] == int(head) + 1)
            out.emplace_back(int(column));
    return out;
}

bool is_marked(const DynamicPrintConfig &config, size_t column, const std::string &key)
{
    return marker_keys(config, column).count(key) > 0;
}

std::vector<std::string> head_override_keys(const DynamicPrintConfig &config, size_t head)
{
    std::set<std::string> keys;
    for (int column : head_columns(config, head))
        for (const std::string &key : marker_keys(config, size_t(column)))
            keys.insert(key);
    return std::vector<std::string>(keys.begin(), keys.end());
}

bool marker_names_any(const DynamicPrintConfig &config)
{
    const auto *marker = config.option<ConfigOptionStrings>(override_key);
    if (marker == nullptr)
        return false;
    for (const std::string &entry : marker->values)
        if (!split_marker(entry).empty())
            return true;
    return false;
}

bool marker_empty(const DynamicPrintConfig &config)
{
    return !marker_names_any(config);
}

const std::set<std::string> &head_editable_keys()
{
    static const std::set<std::string> keys = [] {
        std::set<std::string> out(value_keys().begin(), value_keys().end());
        for (const char *uniform : {"enable_overhang_speed", "slowdown_for_curled_perimeters", "small_perimeter_threshold"})
            out.erase(uniform);
        return out;
    }();
    return keys;
}

void set_head_value(DynamicPrintConfig &config, size_t head, const std::string &key, int written_column)
{
    const std::vector<int> columns = head_columns(config, head);
    if (columns.empty())
        return;
    const int source = std::find(columns.begin(), columns.end(), written_column) != columns.end() ? written_column : columns.front();
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    for (int column : columns) {
        if (option != nullptr && column != source)
            copy_column(*option, *option, size_t(column), size_t(source));
        std::set<std::string> keys = marker_keys(config, size_t(column));
        keys.insert(key);
        write_marker(config, size_t(column), keys);
    }
}

void set_shared_value(DynamicPrintConfig &config, const std::string &key, NozzleVolumeType type)
{
    if (!is_wide(config))
        return;
    const Layout layout = layout_of(config);
    const int    source = shared_column(config, type);
    const bool   single = shared_width(config) == 1;
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    if (option == nullptr)
        return;
    for (size_t column = 0; column < layout.size(); ++column) {
        if (layout.ids[column] == 0 || int(column) == source)
            continue;
        if (!single && layout.variants[column] != layout.variants[size_t(source)])
            continue;
        if (is_marked(config, column, key))
            continue;
        copy_column(*option, *option, column, size_t(source));
    }
}

void clear_head_value(DynamicPrintConfig &config, size_t head, const std::string &key)
{
    const Layout layout = layout_of(config);
    auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
    for (int column : head_columns(config, head)) {
        const int shared = shared_column_of_variant(layout, layout.variants[size_t(column)]);
        if (option != nullptr && shared >= 0)
            copy_column(*option, *option, size_t(column), size_t(shared));
        std::set<std::string> keys = marker_keys(config, size_t(column));
        keys.erase(key);
        write_marker(config, size_t(column), keys);
    }
}

void clear_head(DynamicPrintConfig &config, size_t head)
{
    for (const std::string &key : head_override_keys(config, head))
        clear_head_value(config, head, key);
}

void clear_all_heads(DynamicPrintConfig &config)
{
    for (int head : distinct_heads(layout_of(config)))
        clear_head(config, size_t(head));
}

std::vector<size_t> heads_marked_for(const DynamicPrintConfig &config, const std::string &key)
{
    std::vector<size_t> out;
    for (int head : distinct_heads(layout_of(config)))
        for (int column : head_columns(config, size_t(head)))
            if (is_marked(config, size_t(column), key)) {
                out.emplace_back(size_t(head));
                break;
            }
    return out;
}

void normalise(DynamicPrintConfig &config, const DynamicPrintConfig *parent, const DynamicPrintConfig *printer)
{
    Layout layout = layout_of(config);
    if (layout.size() == 0)
        return;
    const auto element = [](const ConfigOptionVectorBase &option, size_t column) {
        const std::vector<std::string> values = option.vserialize();
        return column < values.size() ? values[column] : std::string();
    };

    // Ids narrower than the widest value key.
    size_t widest = layout.size();
    for (const std::string &key : value_keys())
        if (const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key)); option != nullptr)
            widest = std::max(widest, option->size());
    if (widest > layout.size()) {
        const auto *marker = config.option<ConfigOptionStrings>(override_key);
        Layout      rebuilt;
        if (parent != nullptr && printer != nullptr && marker != nullptr && marker->values.size() == widest)
            rebuilt = wide_layout(*parent, *printer);
        if (rebuilt.size() == widest) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": the process layout has %1% columns while a value key has %2%; ids and variants rebuilt from the parent and the printer") % layout.size() % widest;
            config.set_key_value("print_extruder_id", new ConfigOptionInts(rebuilt.ids));
            config.set_key_value("print_extruder_variant", new ConfigOptionStrings(rebuilt.variants));
            layout = rebuilt;
        } else {
            for (const std::string &key : value_keys())
                if (auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key)); option != nullptr && option->size() > layout.size()) {
                    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": %1% has %2% values on a layout of %3% columns; cut to the layout") % key % option->size() % layout.size();
                    option->resize(layout.size());
                }
        }
    }

    // Value keys narrower than the ids: widened by the column walk from the layout they are in.
    const Layout parent_layout = parent == nullptr ? Layout() : layout_of(*parent);
    for (const std::string &key : value_keys()) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || option->empty() || option->size() >= layout.size())
            continue;
        Layout from;
        if (option->size() == parent_layout.size())
            from = parent_layout;
        else if (option->size() == shared_width(config)) {
            const std::vector<std::string> variants = shared_variants(config);
            from.ids.assign(variants.size(), 1);
            from.variants = variants;
        } else {
            from.ids.assign(option->size(), 1);
            from.variants.assign(option->size(), layout.variants.front());
        }
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": %1% has %2% values on a layout of %3% columns; widened by the column walk") % key % option->size() % layout.size();
        std::unique_ptr<ConfigOptionVectorBase> out(static_cast<ConfigOptionVectorBase *>(option->clone()));
        out->resize(layout.size(), option);
        for (size_t t = 0; t < layout.size(); ++t) {
            const int s = source_column_for(from, layout.ids[t], layout.variants[t]);
            out->set_at(option, t, size_t(s) < option->size() ? size_t(s) : 0);
        }
        config.set_key_value(key, out.release());
    }
    marker_option(config, layout.size());

    // A per-head layout without shared columns whose parent is narrow: mainline data on a printer
    // that uses the selector. The shared columns come from the parent, the marker from the head
    // columns that differ from the parent's column of their flow.
    if (!is_wide(config) && ids_name_heads(layout.ids) && parent != nullptr && parent_layout.size() > 0 && !ids_name_heads(parent_layout.ids)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": a process layout with one column per extruder and no shared columns; the shared columns are taken from the parent";
        Layout target;
        for (const std::string &variant : shared_variants(*parent)) {
            target.ids.emplace_back(0);
            target.variants.emplace_back(variant);
        }
        const size_t shared_count = target.size();
        for (size_t column = 0; column < layout.size(); ++column) {
            target.ids.emplace_back(layout.ids[column]);
            target.variants.emplace_back(layout.variants[column]);
        }
        relayout(config, target);
        for (const std::string &key : value_keys()) {
            auto       *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
            const auto *theirs = dynamic_cast<const ConfigOptionVectorBase *>(parent->option(key));
            if (option == nullptr || theirs == nullptr || theirs->empty() || option->type() != theirs->type())
                continue;
            for (size_t t = 0; t < shared_count; ++t)
                copy_column(*option, *theirs, t, size_t(source_column_for(parent_layout, 1, target.variants[t])));
        }
        layout = layout_of(config);
        for (size_t column = shared_count; column < layout.size(); ++column) {
            std::set<std::string> keys;
            const int shared = shared_column_of_variant(layout, layout.variants[column]);
            for (const std::string &key : head_editable_keys()) {
                const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
                if (option != nullptr && shared >= 0 && element(*option, column) != element(*option, size_t(shared)))
                    keys.insert(key);
            }
            write_marker(config, column, keys);
        }
    }

    // Marker hygiene: known keys only, none on a shared column, a key marked on a head marked on
    // every column of the head with the value of the first marked column (I4).
    for (size_t column = 0; column < layout.size(); ++column) {
        std::set<std::string> keys = marker_keys(config, column);
        std::set<std::string> kept;
        for (const std::string &key : keys) {
            if (layout.ids[column] == 0)
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": %1% marked on shared column %2%; dropped") % key % column;
            else if (head_editable_keys().count(key) == 0)
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": %1% cannot be set per extruder; dropped from column %2%") % key % column;
            else
                kept.insert(key);
        }
        if (kept != keys)
            write_marker(config, column, kept);
    }
    for (int head : distinct_heads(layout)) {
        const std::vector<int> columns = head_columns(config, size_t(head));
        for (const std::string &key : head_override_keys(config, size_t(head))) {
            int first = -1;
            for (int column : columns)
                if (is_marked(config, size_t(column), key)) {
                    first = column;
                    break;
                }
            if (first >= 0)
                set_head_value(config, size_t(head), key, first);
        }
    }

    // I1: wide exactly while the marker names a key.
    if (is_wide(config) && marker_empty(config)) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": a wide process layout without values set per extruder; narrowed";
        narrow(config);
    } else if (!is_wide(config) && marker_names_any(config)) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": a marker on a process layout without shared columns; dropped";
        config.set_key_value(override_key, new ConfigOptionStrings(std::vector<std::string>(layout.size(), std::string())));
    }
}

void transfer_columns(DynamicPrintConfig &target, const DynamicPrintConfig &source, const std::vector<std::string> &indexed_options, const DynamicPrintConfig &printer)
{
    const Layout from = layout_of(source);
    if (from.size() == 0)
        return;
    if (is_wide(source) && !is_wide(target))
        widen(target, printer);
    const Layout to = layout_of(target);
    if (to.size() == 0)
        return;
    for (const std::string &option : indexed_options) {
        const size_t hash = option.find('#');
        if (hash == std::string::npos)
            continue;
        const std::string key    = option.substr(0, hash);
        const size_t      column = size_t(std::atoi(option.c_str() + hash + 1));
        if (column >= from.size() || key == override_key || print_options_with_variant.count(key) == 0 || key == "print_extruder_id" || key == "print_extruder_variant")
            continue;
        auto       *mine   = dynamic_cast<ConfigOptionVectorBase *>(target.option(key, true));
        const auto *theirs = dynamic_cast<const ConfigOptionVectorBase *>(source.option(key));
        if (mine == nullptr || theirs == nullptr || theirs->empty() || mine->type() != theirs->type())
            continue;
        if (mine->size() != to.size())
            mine->resize(to.size(), mine);
        const std::string &variant = from.variants[column];
        if (is_wide(source) && from.ids[column] > 0) {
            if (!is_marked(source, column, key))
                continue;
            for (size_t t = 0; t < to.size(); ++t)
                if (to.ids[t] == from.ids[column] && to.variants[t] == variant) {
                    copy_column(*mine, *theirs, t, column);
                    std::set<std::string> keys = marker_keys(target, t);
                    keys.insert(key);
                    write_marker(target, t, keys);
                }
            continue;
        }
        // A shared or narrow source column.
        int t = -1;
        if (is_wide(target))
            t = shared_column_of_variant(to, variant);
        else
            for (size_t candidate = 0; candidate < to.size() && t < 0; ++candidate)
                if (to.variants[candidate] == variant)
                    t = int(candidate);
        if (t < 0)
            t = int(std::min(column, to.size() - 1));
        copy_column(*mine, *theirs, size_t(t), column);
        if (is_wide(target))
            set_shared_value(target, key, variant_names_type(variant, nvtHighFlow) ? nvtHighFlow : nvtStandard);
    }
    if (is_wide(target) && marker_empty(target))
        narrow(target);
}

int column_for_head(const DynamicPrintConfig &process, size_t head, NozzleVolumeType type, const DynamicPrintConfig &printer)
{
    const auto *diameters = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t heads    = diameters == nullptr ? 1 : std::max<size_t>(1, diameters->values.size());
    int index = process.get_index_for_extruder(int(head) + 1, "print_extruder_id", head_extruder_type(printer, head, heads), type, "print_extruder_variant");
    if (index < 0) {
        const auto *variants = process.option<ConfigOptionStrings>("print_extruder_variant");
        if (variants != nullptr && variants->values.size() == 1 && variant_names_type(variants->values.front(), type))
            index = 0;
    }
    return index;
}

int source_column(const Preset &source, size_t head, NozzleVolumeType type, const DynamicPrintConfig &printer)
{
    if (is_wide(source.config)) {
        // A user preset with values set per tool head: the shared column of the flow stands for
        // every head (a head column holds values set for a head of the printer the preset was saved
        // on, never for a head of this plate).
        const Layout layout = layout_of(source.config);
        const int    shared = shared_column(source.config, type);
        return shared >= 0 && size_t(shared) < layout.size() && variant_names_type(layout.variants[size_t(shared)], type) ? shared : -1;
    }
    return column_for_head(source.config, head, type, printer);
}

bool has_high_flow_values(const DynamicPrintConfig &process, size_t head, const DynamicPrintConfig &printer)
{
    if (is_wide(process)) {
        for (const std::string &variant : shared_variants(process))
            if (variant_names_type(variant, nvtHighFlow))
                return true;
        return false;
    }
    return column_for_head(process, head, nvtHighFlow, printer) >= 0;
}

bool reads_high_flow(const Source &source, NozzleVolumeType flow, const DynamicPrintConfig &printer)
{
    return flow == nvtHighFlow && source.derived && source.preset != nullptr && source_column(*source.preset, source.head, nvtHighFlow, printer) >= 0;
}

} // namespace PerHeadProcess
} // namespace Slic3r
