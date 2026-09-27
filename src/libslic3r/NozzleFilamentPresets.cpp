#include "NozzleFilamentPresets.hpp"

#include "LocalesUtils.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace NozzleFilament {

double parse_nozzle_size(const std::string &text)
{
    if (text.empty())
        return 0.;
    size_t       parsed = 0;
    const double value  = string_to_double_decimal_point(text, &parsed);
    return parsed == 0 || value <= 0. ? 0. : value;
}

namespace {

bool same_size(double a, double b) { return std::abs(a - b) < EPSILON; }

template<typename Matches>
const Preset* find_machine_preset(const PrinterPresetCollection &printers, const std::string &model, Matches matches)
{
    if (model.empty())
        return nullptr;
    const Preset *custom = nullptr;
    for (const Preset &preset : printers) {
        if (preset.config.opt_string("printer_model") != model || !matches(preset.config.opt_string("printer_variant")))
            continue;
        if (preset.is_system)
            return &preset;
        if (custom == nullptr)
            custom = &preset;
    }
    return custom;
}

bool is_family_member(const Preset &candidate, const Preset &of)
{
    return candidate.is_system && candidate.system_inherits == of.system_inherits && candidate.alias == of.alias;
}

} // namespace

std::vector<size_t> filament_heads(size_t filament_count, size_t head_count, const std::vector<int> &filament_map, bool map_is_binding, size_t master_head)
{
    std::vector<size_t> heads(filament_count, 0);
    if (head_count == 0)
        return heads;
    if (master_head >= head_count)
        master_head = 0;
    for (size_t filament = 0; filament < filament_count; ++filament) {
        heads[filament] = filament < head_count ? filament : master_head;
        if (map_is_binding && filament < filament_map.size() && filament_map[filament] >= 1 && size_t(filament_map[filament]) <= head_count)
            heads[filament] = size_t(filament_map[filament] - 1);
    }
    return heads;
}

std::string size_marked_label(const std::string &label, const std::string &size_text)
{
    if (size_text.empty())
        return label;
    return size_text + " \xC2\xB7 " + label;
}

double home_nozzle_size(const DynamicPrintConfig &printer_config)
{
    const auto *variant = printer_config.option<ConfigOptionString>("printer_variant");
    return variant == nullptr ? 0. : parse_nozzle_size(variant->value);
}

const Preset* head_machine_preset(const PrinterPresetCollection &printers, const std::string &model, const std::string &variant_label)
{
    if (variant_label.empty())
        return nullptr;
    // The lookup Sidebar::apply_nozzle_diameter makes for the layer height limits of a tool head.
    const Preset *preset = printers.find_system_preset_by_model_and_variant(model, variant_label);
    return preset != nullptr ? preset : printers.find_custom_preset_by_model_and_variant(model, variant_label);
}

const Preset* head_machine_preset(const PrinterPresetCollection &printers, const std::string &model, double nozzle_size)
{
    if (nozzle_size <= 0.)
        return nullptr;
    return find_machine_preset(printers, model, [nozzle_size](const std::string &variant) { return same_size(parse_nozzle_size(variant), nozzle_size); });
}

State state(const PresetBundle &bundle)
{
    State out;
    if (!bundle.nozzle_filament_enabled)
        return out;
    const Preset &printer = bundle.printers.get_edited_preset();
    if (printer.printer_technology() != ptFFF)
        return out;
    const auto *diameters = printer.config.option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters == nullptr || diameters->values.size() <= 1)
        return out;
    if (const auto *semm = printer.config.option<ConfigOptionBool>("single_extruder_multi_material"); semm != nullptr && semm->value)
        return out;
    // The vendor of the system parent. A printer preset without one (imported, detached) is no
    // Snapmaker machine as far as this rule can tell.
    const VendorProfile *vendor = bundle.printers.get_preset_with_vendor_profile(printer).vendor;
    if (vendor == nullptr || (vendor->id != PresetBundle::SM_BUNDLE && vendor->name != PresetBundle::SM_BUNDLE))
        return out;

    out.rule_on   = true;
    out.head_size = diameters->values;
    out.head_machine.assign(out.head_size.size(), &printer);
    const double      home  = home_nozzle_size(printer.config);
    const std::string model = printer.config.opt_string("printer_model");
    for (size_t head = 0; head < out.head_size.size(); ++head) {
        if (home <= 0. || same_size(out.head_size[head], home))
            continue;
        // A size without a profile leaves the head with the printer preset: nothing follows.
        if (const Preset *machine = head_machine_preset(bundle.printers, model, out.head_size[head]); machine != nullptr && machine != &printer) {
            out.head_machine[head] = machine;
            out.mixed              = true;
        }
    }

    // Presets follow the project level map. It binds when the project maps its filaments by hand;
    // otherwise filament i is printed by tool head i. (A printer that groups its filaments onto
    // its extruders binds the map as well, but is no Snapmaker machine.)
    const size_t     slots = bundle.filament_presets.size();
    std::vector<int> filament_map;
    if (const auto *map = bundle.project_config.option<ConfigOptionInts>("filament_map"); map != nullptr)
        filament_map = map->values;
    bool map_is_binding = false;
    if (const auto *mode = bundle.project_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode"); mode != nullptr)
        map_is_binding = mode->value == fmmManual || mode->value == fmmNozzleManual;
    out.slot_head = filament_heads(slots, out.head_size.size(), filament_map, map_is_binding);
    for (size_t slot = 0; slot < slots; ++slot)
        if (slot >= out.head_size.size() || bundle.is_mixed_filament(slot))
            out.slot_head[slot] = no_head;
    return out;
}

bool sync_keeps_printer_preset(const Preset &edited, const Preset &picked)
{
    return edited.config.opt_string("printer_model") == picked.config.opt_string("printer_model") &&
           edited.config.opt_string("printer_variant") == picked.config.opt_string("printer_variant");
}

const Preset* system_ancestor(const PresetCollection &filaments, const Preset &preset)
{
    const Preset *p = &preset;
    // The depth bound guards against a cycle in damaged user presets.
    for (int depth = 0; p != nullptr && depth < 32; ++depth) {
        if (p->is_system)
            return p;
        if (p->inherits().empty())
            return nullptr;
        p = filaments.get_preset_parent(*p);
    }
    return nullptr;
}

bool restricts_printers(const Preset &preset)
{
    const auto *list = preset.config.option<ConfigOptionStrings>("compatible_printers");
    return (list != nullptr && !list->values.empty()) || !preset.compatible_printers_condition().empty();
}

const Preset* version_for(const PresetCollection &filaments, const Preset &system_preset, const Preset &machine)
{
    if (!system_preset.is_system || system_preset.system_inherits.empty() || system_preset.alias.empty())
        return nullptr;
    const PresetWithVendorProfile machine_with_vendor(machine, machine.vendor);
    const Preset                 *version = nullptr;
    for (const Preset &candidate : filaments) {
        if (!is_family_member(candidate, system_preset) || !restricts_printers(candidate))
            continue;
        if (!is_compatible_with_printer(filaments.get_preset_with_vendor_profile(candidate), machine_with_vendor))
            continue;
        if (version != nullptr) {
            BOOST_LOG_TRIVIAL(warning) << "NozzleFilament: \"" << version->name << "\" and \"" << candidate.name << "\" both serve \""
                                       << machine.name << "\"; the family has no single version for it";
            return nullptr;
        }
        version = &candidate;
    }
    return version;
}

double preset_nozzle_size(const PresetBundle &bundle, const Preset &filament)
{
    const Preset *system = system_ancestor(bundle.filaments, filament);
    if (system == nullptr)
        return 0.;
    const auto *list = system->config.option<ConfigOptionStrings>("compatible_printers");
    if (list == nullptr || list->values.empty())
        return 0.;
    double size = 0.;
    for (const std::string &name : list->values) {
        const Preset *machine = bundle.printers.find_preset(name, false);
        if (machine == nullptr)
            continue;
        const double machine_size = home_nozzle_size(machine->config);
        if (machine_size <= 0. || (size > 0. && !same_size(size, machine_size)))
            return 0.;
        size = machine_size;
    }
    return size;
}

bool fits(const PresetBundle &bundle, const Preset &filament, const Preset &machine)
{
    const PresetWithVendorProfile filament_with_vendor = bundle.filaments.get_preset_with_vendor_profile(filament);
    const Preset                 &printer              = bundle.printers.get_edited_preset();
    const bool                    is_edited_printer    = &machine == &printer;
    const PresetWithVendorProfile machine_with_vendor  = bundle.printers.get_preset_with_vendor_profile(machine);
    if (!is_compatible_with_printer(filament_with_vendor, machine_with_vendor))
        return false;
    const PresetWithVendorProfile printer_with_vendor = is_edited_printer ? machine_with_vendor : bundle.printers.get_preset_with_vendor_profile(printer);
    // The library arm of is_compatible_with_printer rates the ACTIVE printer: a library preset
    // hidden from it stays hidden, whatever machine preset stands for the tool head.
    if (!is_edited_printer && filament_with_vendor.vendor != nullptr && filament_with_vendor.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY) {
        const std::set<std::string> &excluded = filament.m_excluded_from;
        if (excluded.find(printer.name) != excluded.end() || excluded.find(printer.inherits()) != excluded.end())
            return false;
    }
    return is_compatible_with_print(filament_with_vendor, bundle.prints.get_edited_preset_with_vendor_profile(), printer_with_vendor);
}

namespace {

// target_for_slot() without the rule about unsaved changes.
SlotTarget resolve_slot(const PresetBundle &bundle, const State &state, size_t slot)
{
    SlotTarget out;
    out.slot = slot;
    if (!state.rule_on || slot >= bundle.filament_presets.size())
        return out;
    out.head = state.head_of(slot);
    out.from = out.to = bundle.filament_presets[slot];
    const Preset *machine = state.machine_of(slot);
    if (machine == nullptr || out.from.empty() || out.from == PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER)
        return out;
    out.head_size = state.head_size[out.head];

    // find_preset() resolves "renamed_from" and hands out the edited copy of the selected preset,
    // so presets are told apart by name below, never by address.
    const PresetCollection &filaments = bundle.filaments;
    const Preset           *preset    = filaments.find_preset(out.from, false);
    if (preset == nullptr || preset->is_default)
        return out;
    out.to = preset->name;
    out.preset_size = preset_nozzle_size(bundle, *preset);

    const Preset &printer = bundle.printers.get_edited_preset();
    if (fits(bundle, *preset, *machine)) {
        out.reason        = Reason::Unchanged;
        out.size_agnostic = machine != &printer && !restricts_printers(*preset);
        return out;
    }

    const Preset *system = system_ancestor(filaments, *preset);
    if (system == nullptr || system->system_inherits.empty() || system->alias.empty()) {
        out.reason = Reason::NoFamily;
        return out;
    }
    // A user preset of this family that was left for its size earlier in the session comes back
    // before any other answer: the way back of "My PLA" after the tool head returned to its size.
    if (const auto remembered = bundle.nozzle_filament_memory.find({ family_key(filaments, *system), machine->name });
        remembered != bundle.nozzle_filament_memory.end() && remembered->second != preset->name) {
        const Preset *user = filaments.find_preset(remembered->second, false);
        if (user != nullptr && user->name == remembered->second && !user->is_system && !user->is_default && user->is_visible &&
            fits(bundle, *user, *machine)) {
            out.to     = user->name;
            out.reason = Reason::Switched;
            return out;
        }
    }

    const Preset *version = version_for(filaments, *system, *machine);
    if (version != nullptr && !fits(bundle, *version, *machine))
        // Served by its list, refused by the process preset or the library exclusion.
        version = nullptr;

    if (preset->is_system) {
        if (version != nullptr) {
            out.to     = version->name;
            out.reason = Reason::Switched;
            return out;
        }
        const Preset *home = machine == &printer ? nullptr : version_for(filaments, *system, printer);
        if (home != nullptr && home->name != preset->name && fits(bundle, *home, printer)) {
            out.to     = home->name;
            out.reason = Reason::HomeVersionUsed;
        } else
            out.reason = Reason::NoVersion;
        return out;
    }

    // A user preset moves only to the one visible user preset made from the version for this size.
    const Preset *counterpart  = nullptr;
    size_t        counterparts = 0;
    if (version != nullptr)
        for (const Preset &candidate : filaments)
            if (const Preset *ancestor = candidate.is_system || candidate.is_default || !candidate.is_visible || candidate.name == preset->name ?
                                             nullptr : system_ancestor(filaments, candidate);
                ancestor != nullptr && ancestor->name == version->name && fits(bundle, candidate, *machine)) {
                counterpart = &candidate;
                ++counterparts;
            }
    if (counterparts == 1) {
        out.to     = counterpart->name;
        out.reason = Reason::Switched;
    } else {
        out.reason = Reason::UserPresetKept;
        if (version != nullptr)
            out.suggestion = version->name;
    }
    return out;
}

} // namespace

std::string family_key(const PresetCollection &filaments, const Preset &preset)
{
    const Preset *system = system_ancestor(filaments, preset);
    return system == nullptr || system->system_inherits.empty() || system->alias.empty() ? std::string() : system->system_inherits + "\n" + system->alias;
}

void remember_user_preset(PresetBundle &bundle, size_t slot, const std::string &name)
{
    const State state = NozzleFilament::state(bundle);
    if (!state.rule_on)
        return;
    const Preset *preset = bundle.filaments.find_preset(name, false);
    if (preset == nullptr || preset->is_system || preset->is_default)
        return;
    const std::string family = family_key(bundle.filaments, *preset);
    const Preset     *slot_machine = state.machine_of(slot);
    if (family.empty() || slot_machine == nullptr || fits(bundle, *preset, *slot_machine))
        return;
    // The machine presets of this printer model: the edited one and the system presets of the other sizes.
    const Preset &printer = bundle.printers.get_edited_preset();
    const auto   *model   = printer.config.option<ConfigOptionString>("printer_model");
    auto note = [&bundle, &family, preset](const Preset &machine) {
        if (fits(bundle, *preset, machine))
            bundle.nozzle_filament_memory[{ family, machine.name }] = preset->name;
    };
    note(printer);
    if (model != nullptr && !model->value.empty())
        for (const Preset &machine : bundle.printers) {
            const auto *other = machine.config.option<ConfigOptionString>("printer_model");
            if (machine.is_system && machine.name != printer.name && other != nullptr && other->value == model->value)
                note(machine);
        }
}

SlotTarget target_for_slot(const PresetBundle &bundle, const State &state, size_t slot, const std::vector<size_t> &heads)
{
    SlotTarget out = resolve_slot(bundle, state, slot);
    if (!out.switches())
        return out;
    // Only the edited preset can carry unsaved changes. They stay with the preset as long as a
    // slot keeps naming it; when every such slot leaves, the changes would be lost from sight.
    const PresetCollection &filaments = bundle.filaments;
    if (filaments.get_selected_idx() == size_t(-1) || filaments.get_selected_preset_name() != out.from || !filaments.current_is_dirty())
        return out;
    auto moves = [&heads](const SlotTarget &target) {
        return target.switches() && (heads.empty() || std::find(heads.begin(), heads.end(), target.head) != heads.end());
    };
    for (size_t other = 0; other < bundle.filament_presets.size(); ++other) {
        if (other == slot)
            continue;
        const SlotTarget target = resolve_slot(bundle, state, other);
        if (target.from == out.from && !moves(target))
            return out;
    }
    out.to     = out.from;
    out.reason = Reason::DirtyKept;
    return out;
}

const char* reason_name(Reason reason)
{
    switch (reason) {
    case Reason::Unchanged:       return "Unchanged";
    case Reason::Switched:        return "Switched";
    case Reason::HomeVersionUsed: return "HomeVersionUsed";
    case Reason::NoVersion:       return "NoVersion";
    case Reason::NoFamily:        return "NoFamily";
    case Reason::UserPresetKept:  return "UserPresetKept";
    case Reason::DirtyKept:       return "DirtyKept";
    case Reason::Skipped:         break;
    }
    return "Skipped";
}

std::string describe_target(const SlotTarget &target, const std::string &trigger, const std::vector<int> &head_flow)
{
    std::string line = "NozzleFilament: trigger=" + trigger + " filament=" + std::to_string(target.slot + 1);
    if (target.head == no_head) {
        line += " head=- size=- flow=-";
    } else {
        line += " head=" + std::to_string(target.head + 1);
        line += " size=" + (target.head_size > 0. ? float_to_string_decimal_point(target.head_size) : std::string("-"));
        const int flow = target.head < head_flow.size() ? head_flow[target.head] : int(nvtStandard);
        line += " flow=";
        switch (flow) {
        case int(nvtStandard): line += "Standard"; break;
        case int(nvtHighFlow): line += "High Flow"; break;
        case int(nvtHybrid):   line += "Hybrid"; break;
        default:               line += std::to_string(flow); break;
        }
    }
    line += " from=\"" + target.from + "\" to=\"" + target.to + "\" reason=" + reason_name(target.reason);
    if (target.preset_size > 0.)
        line += " preset_size=" + float_to_string_decimal_point(target.preset_size);
    if (target.size_agnostic)
        line += " size_agnostic=1";
    if (!target.suggestion.empty())
        line += " suggestion=\"" + target.suggestion + "\"";
    return line;
}

void log_targets(const PresetBundle &bundle, const std::vector<SlotTarget> &targets, const std::string &trigger)
{
    std::vector<int> head_flow;
    if (const auto *types = bundle.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"); types != nullptr)
        head_flow = types->values;
    for (const SlotTarget &target : targets)
        BOOST_LOG_TRIVIAL(info) << describe_target(target, trigger, head_flow);
}

std::string blacklist_name(const PresetCollection &filaments, const std::string &preset_name)
{
    if (const Preset *preset = filaments.find_preset(preset_name, false); preset != nullptr)
        if (const Preset *system = system_ancestor(filaments, *preset); system != nullptr && !system->alias.empty())
            return system->alias;
    const std::string front = preset_name.substr(0, preset_name.find('@'));
    const size_t      first = front.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : front.substr(first, front.find_last_not_of(' ') - first + 1);
}

std::map<std::pair<double, int>, std::set<std::string>> blacklisted_by_head(const std::vector<std::string> &names, const std::vector<size_t> &filament_head,
                                                                           const std::vector<double> &head_size, const std::vector<int> &head_volume_type,
                                                                           const NozzleBlacklist &blacklist)
{
    std::map<std::pair<double, int>, std::set<std::string>> out;
    if (!blacklist)
        return out;
    // One lookup per kind of nozzle.
    std::map<std::pair<double, int>, std::vector<std::string>> listed;
    for (size_t filament = 0; filament < names.size() && filament < filament_head.size(); ++filament) {
        const size_t head = filament_head[filament];
        if (head >= head_size.size() || names[filament].empty())
            continue;
        const int  volume_type = head < head_volume_type.size() ? head_volume_type[head] : head_volume_type.empty() ? 0 : head_volume_type.front();
        const auto nozzle      = std::make_pair(head_size[head], volume_type);
        auto       it          = listed.find(nozzle);
        if (it == listed.end())
            it = listed.emplace(nozzle, blacklist(nozzle.first, nozzle.second)).first;
        if (std::find(it->second.begin(), it->second.end(), names[filament]) != it->second.end())
            out[nozzle].insert(names[filament]);
    }
    return out;
}

}} // namespace Slic3r::NozzleFilament
