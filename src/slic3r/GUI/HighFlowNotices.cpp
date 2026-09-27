#include "HighFlowNotices.hpp"

#include "HighFlowCompat.hpp"

#include "GUI.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "Widgets/ComboBox.hpp"

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/NozzleFilamentPresets.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace HighFlowNotices {

namespace {

ExtruderType extruder_type_of(const DynamicPrintConfig &printer_config, size_t head)
{
    const auto *types = printer_config.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (types == nullptr || types->values.empty())
        return etDirectDrive;
    return ExtruderType(types->get_at(head));
}

std::vector<std::string> split_variants(const std::string &list)
{
    std::vector<std::string> variants;
    boost::algorithm::split(variants, list, boost::algorithm::is_any_of(","));
    for (std::string &variant : variants)
        boost::algorithm::trim(variant);
    return variants;
}

// The nozzle size a printer preset was made for, 0 when the preset does not say.
double preset_nozzle_size(const DynamicPrintConfig &printer_config)
{
    return NozzleFilament::home_nozzle_size(printer_config);
}

} // namespace

std::vector<int> declared_volume_types(const DynamicPrintConfig &printer_config, size_t head)
{
    std::vector<int> declared;
    const auto      *lists = printer_config.option<ConfigOptionStrings>("extruder_variant_list");
    if (lists != nullptr && head < lists->values.size()) {
        const std::vector<std::string> variants = split_variants(lists->values[head]);
        const ExtruderType             type     = extruder_type_of(printer_config, head);
        for (NozzleVolumeType volume_type : get_valid_nozzle_volume_type())
            if (std::find(variants.begin(), variants.end(), get_extruder_variant_string(type, volume_type)) != variants.end())
                declared.push_back(int(volume_type));
    }
    if (declared.empty())
        declared.push_back(int(nvtStandard));
    return declared;
}

bool head_declares_high_flow(const DynamicPrintConfig &printer_config, size_t head)
{
    const std::vector<int> declared = declared_volume_types(printer_config, head);
    return std::find(declared.begin(), declared.end(), int(nvtHighFlow)) != declared.end();
}

SizeOffersHighFlow size_offers_high_flow(const PresetBundle &bundle)
{
    const PresetBundle *presets = &bundle;
    return [presets](double nozzle_size, size_t head) {
        const std::string model = presets->printers.get_edited_preset().config.opt_string("printer_model");
        if (nozzle_size <= 0.) {
            // Any size of the model: the Flow row of a Standard-only preset.
            if (model.empty())
                return false;
            for (const Preset &preset : presets->printers)
                if (preset.config.opt_string("printer_model") == model && head_declares_high_flow(preset.config, head))
                    return true;
            return false;
        }
        const Preset *machine = NozzleFilament::head_machine_preset(presets->printers, model, nozzle_size);
        return machine != nullptr && head_declares_high_flow(machine->config, head);
    };
}

bool head_can_use_high_flow(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    if (!head_declares_high_flow(printer_config, head))
        return false;
    const double preset_size = preset_nozzle_size(printer_config);
    const auto  *diameters   = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    // A preset that does not name its nozzle size gives no reason to refuse.
    if (preset_size <= 0. || diameters == nullptr || head >= diameters->values.size())
        return true;
    const double head_size = diameters->values[head];
    if (std::abs(head_size - preset_size) < EPSILON)
        return true;
    // Another size: the vendor data decides whether it has High Flow values.
    return size_offers && size_offers(head_size, head);
}

std::vector<size_t> sanitize(const DynamicPrintConfig &printer_config, std::vector<int> &nozzle_volume_types, const SizeOffersHighFlow &size_offers)
{
    std::vector<size_t> reset_heads;
    for (size_t head = 0; head < nozzle_volume_types.size(); ++head)
        if (nozzle_volume_types[head] == int(nvtHighFlow) && !head_can_use_high_flow(printer_config, head, size_offers)) {
            nozzle_volume_types[head] = int(nvtStandard);
            reset_heads.push_back(head);
        }
    return reset_heads;
}

bool has_high_flow_column(const DynamicPrintConfig &preset_config, const std::string &variant_key)
{
    const auto *variants = preset_config.option<ConfigOptionStrings>(variant_key);
    if (variants == nullptr)
        return false;
    const std::string suffix = get_nozzle_volume_type_string(nvtHighFlow);
    return std::any_of(variants->values.begin(), variants->values.end(), [&suffix](const std::string &variant) {
        // "Direct Drive High Flow", "Bowden High Flow" - but not "... TPU High Flow".
        if (variant.size() < suffix.size() || variant.compare(variant.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;
        const std::string tpu = get_nozzle_volume_type_string(nvtTPUHighFlow);
        return variant.size() < tpu.size() || variant.compare(variant.size() - tpu.size(), tpu.size(), tpu) != 0;
    });
}

std::vector<size_t> filament_heads(size_t filament_count, size_t head_count, const std::vector<int> &filament_map, bool map_is_binding, size_t master_head)
{
    // One implementation: the filament presets that follow their tool head ask the same question.
    return NozzleFilament::filament_heads(filament_count, head_count, filament_map, map_is_binding, master_head);
}

std::vector<std::vector<HeadFilament>> group_by_head(const std::vector<HeadFilament> &filaments, const std::vector<size_t> &filament_head, size_t head_count)
{
    std::vector<std::vector<HeadFilament>> grouped(head_count);
    for (size_t filament = 0; filament < filaments.size() && filament < filament_head.size(); ++filament)
        if (filament_head[filament] < head_count)
            grouped[filament_head[filament]].push_back(filaments[filament]);
    return grouped;
}

Report evaluate(const std::vector<int> &nozzle_volume_types, const std::vector<std::vector<HeadFilament>> &filaments, bool process_has_high_flow_column,
                bool process_standard_only)
{
    auto add_once = [](std::vector<Report::Entry> &entries, size_t head, const std::string &material, const std::string &parent = {}) {
        for (const Report::Entry &entry : entries)
            if (entry.head == head && entry.material == material)
                return;
        entries.push_back({ head, material, parent });
    };

    Report report;
    for (size_t head = 0; head < nozzle_volume_types.size(); ++head) {
        if (nozzle_volume_types[head] != int(nvtHighFlow))
            continue;
        if (!process_has_high_flow_column)
            report.standard_speeds_used.push_back(head);
        else if (process_standard_only)
            report.process_standard_only.push_back(head);
        if (head >= filaments.size())
            continue;
        for (const HeadFilament &filament : filaments[head]) {
            const HighFlowCompat::CompatibilityResult result = HighFlowCompat::check(filament.filament_type, filament.preset_name);
            if (result.level == HighFlowCompat::CompatibilityLevel::Unsupported) {
                // Slicing is refused; the missing column does not matter any more.
                add_once(report.unsupported, head, result.material);
                continue;
            }
            if (result.level == HighFlowCompat::CompatibilityLevel::NotRecommended)
                add_once(report.not_recommended, head, result.material);
            if (!filament.has_high_flow_column)
                add_once(report.standard_values_used, head, filament.preset_name);
            else if (!filament.standard_only_keys.empty())
                add_once(report.standard_only_edited, head, filament.preset_name, filament.parent_name);
        }
    }
    return report;
}

std::vector<std::string> standard_only_edits(const DynamicPrintConfig &preset, const DynamicPrintConfig &parent,
                                             const std::set<std::string> &variant_keys, const std::string &variant_key)
{
    // The Standard and the High Flow column of a config, found by the volume type each variant
    // name ends in; -1 for a column the config has none of.
    struct Columns
    {
        int    standard { -1 };
        int    high_flow { -1 };
        size_t count { 0 };
    };
    auto columns_of = [&variant_key](const DynamicPrintConfig &config) {
        Columns     columns;
        const auto *variants = config.option<ConfigOptionStrings>(variant_key);
        if (variants == nullptr)
            return columns;
        columns.count = variants->values.size();
        for (size_t column = 0; column < variants->values.size(); ++column) {
            const std::string volume_type = split_variant_name(variants->values[column]).volume_type;
            if (columns.standard < 0 && volume_type == get_nozzle_volume_type_string(nvtStandard))
                columns.standard = int(column);
            if (columns.high_flow < 0 && volume_type == get_nozzle_volume_type_string(nvtHighFlow))
                columns.high_flow = int(column);
        }
        return columns;
    };
    const Columns edited_columns = columns_of(preset);
    const Columns base_columns   = columns_of(parent);
    std::vector<std::string> keys;
    if (edited_columns.count < 2 || edited_columns.standard < 0 || edited_columns.high_flow < 0 || base_columns.standard < 0 || base_columns.high_flow < 0)
        return keys;

    for (const std::string &key : variant_keys) {
        if (key == variant_key)
            continue;
        const auto *edited = dynamic_cast<const ConfigOptionVectorBase *>(preset.option(key));
        const auto *base   = dynamic_cast<const ConfigOptionVectorBase *>(parent.option(key));
        if (edited == nullptr || base == nullptr || edited->size() != edited_columns.count || base->size() != base_columns.count)
            continue;
        const std::vector<std::string> edited_values = edited->vserialize();
        const std::vector<std::string> base_values   = base->vserialize();
        if (edited_values[size_t(edited_columns.standard)] != base_values[size_t(base_columns.standard)] &&
            edited_values[size_t(edited_columns.high_flow)] == base_values[size_t(base_columns.high_flow)])
            keys.push_back(key);
    }
    return keys;
}

std::map<std::string, std::vector<Report::Entry>> group_by_nozzle_size(const std::vector<Report::Entry> &entries, const DynamicPrintConfig &printer_config)
{
    std::map<std::string, std::vector<Report::Entry>> groups;
    for (const Report::Entry &entry : entries) {
        std::string size = head_nozzle_size_label(printer_config, entry.head);
        if (size.empty())
            if (const auto *variant = printer_config.option<ConfigOptionString>("printer_variant"); variant != nullptr)
                size = boost::algorithm::trim_copy(variant->value);
        groups[size].push_back(entry);
    }
    return groups;
}

std::vector<int> flow_selector_types(const DynamicPrintConfig &printer_config, const DynamicPrintConfig &process_config)
{
    const auto *diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *lists     = printer_config.option<ConfigOptionStrings>("extruder_variant_list");
    const auto *ids       = process_config.option<ConfigOptionInts>("print_extruder_id");
    const auto *variants  = process_config.option<ConfigOptionStrings>("print_extruder_variant");
    if (diameters == nullptr || lists == nullptr || ids == nullptr || variants == nullptr)
        return {};

    const size_t head_count = diameters->values.size();
    if (head_count <= 2)
        return {};
    size_t       declared_columns = 0;
    bool         several_types    = false;
    const ExtruderType drive      = extruder_type_of(printer_config, 0);
    for (size_t head = 0; head < head_count; ++head) {
        if (extruder_type_of(printer_config, head) != drive)
            return {};
        const size_t declared = declared_volume_types(printer_config, head).size();
        declared_columns += declared;
        several_types |= declared > 1;
    }
    if (!several_types)
        return {};

    // Flow-only layout: a complete id list, all ids equal, more than one column and fewer than
    // the printer declares.
    const size_t columns = variants->values.size();
    if (columns < 2 || columns >= declared_columns || ids->values.size() != columns)
        return {};
    if (std::any_of(ids->values.begin(), ids->values.end(), [ids](int id) { return id != ids->values.front(); }))
        return {};

    std::vector<int> types;
    for (const std::string &variant : variants->values) {
        int type = -1;
        for (NozzleVolumeType volume_type : get_valid_nozzle_volume_type())
            if (variant == get_extruder_variant_string(drive, volume_type))
                type = int(volume_type);
        // An unknown name, or a second column of the same type.
        if (type < 0 || std::find(types.begin(), types.end(), type) != types.end())
            return {};
        types.push_back(type);
    }
    return types;
}

VariantName split_variant_name(const std::string &variant)
{
    // The longest volume type first: "TPU High Flow" ends in "High Flow" as well.
    std::vector<std::string> volume_types;
    for (NozzleVolumeType volume_type : get_valid_nozzle_volume_type())
        volume_types.push_back(get_nozzle_volume_type_string(volume_type));
    std::sort(volume_types.begin(), volume_types.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (const std::string &volume_type : volume_types)
        if (variant.size() > volume_type.size() + 1 && variant[variant.size() - volume_type.size() - 1] == ' ' &&
            variant.compare(variant.size() - volume_type.size(), volume_type.size(), volume_type) == 0)
            return { variant.substr(0, variant.size() - volume_type.size() - 1), volume_type };

    const size_t blank = variant.rfind(' ');
    if (blank == std::string::npos)
        return { variant, {} };
    return { variant.substr(0, blank), variant.substr(blank + 1) };
}

VariantName variant_column_label(const std::vector<std::string> &variants, size_t column)
{
    if (column >= variants.size())
        return {};
    VariantName name = split_variant_name(variants[column]);
    if (variants.size() < 2 || name.volume_type.empty())
        return name;
    for (const std::string &variant : variants) {
        const VariantName other = split_variant_name(variant);
        if (other.drive != name.drive || other.volume_type.empty())
            return name;
    }
    name.drive.clear();
    return name;
}

bool ids_name_tool_heads(const std::vector<int> &ids)
{
    return std::any_of(ids.begin(), ids.end(), [&ids](int id) { return id != ids.front(); });
}

bool flow_choice_usable(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    return !head_declares_high_flow(printer_config, head) || head_can_use_high_flow(printer_config, head, size_offers);
}

FlowRowState flow_row_state(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    if (declared_volume_types(printer_config, head).size() <= 1) {
        // A head beyond the preset's diameters has no row; a Standard-only head of a model that
        // offers High Flow at another size shows the row, ruled out.
        const auto *diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
        const bool  has_head  = diameters != nullptr && head < diameters->values.size();
        return has_head && size_offers && size_offers(0., head) ? FlowRowState::RuledOut : FlowRowState::Hidden;
    }
    return flow_choice_usable(printer_config, head, size_offers) ? FlowRowState::Choice : FlowRowState::RuledOut;
}

int shown_volume_type(const DynamicPrintConfig &printer_config, size_t head, int stored_type, const SizeOffersHighFlow &size_offers)
{
    const std::vector<int> declared = declared_volume_types(printer_config, head);   // never empty
    const bool             usable   = flow_choice_usable(printer_config, head, size_offers);
    for (int type : declared)
        if (type == stored_type && (usable || stored_type == int(nvtStandard)))
            return type;
    return declared.front();
}

std::string head_nozzle_size_label(const DynamicPrintConfig &printer_config, size_t head)
{
    const auto *diameters = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (diameters == nullptr || head >= diameters->values.size() || diameters->values[head] <= 0.)
        return {};
    // "0.6", as the sidebar and "printer_variant" spell it: two decimals, trailing zeros dropped.
    std::string label = float_to_string_decimal_point(diameters->values[head], 2);
    while (label.find('.') != std::string::npos && (label.back() == '0' || label.back() == '.'))
        label.pop_back();
    return label;
}

void fill_flow_combo(::ComboBox *combo, const DynamicPrintConfig &printer_config, size_t head, int current_type, const SizeOffersHighFlow &size_offers)
{
    if (combo == nullptr)
        return;
    const std::vector<int> declared = declared_volume_types(printer_config, head);
    // A single declared type leaves nothing to choose (a Standard-only preset shown for a model
    // that offers High Flow at another size): the combo is disabled with the reason of the size.
    const bool             usable   = declared.size() > 1 && flow_choice_usable(printer_config, head, size_offers);

    bool same_items = combo->GetCount() == declared.size();
    for (size_t item = 0; same_items && item < declared.size(); ++item)
        same_items = intptr_t(combo->GetClientData(int(item))) == intptr_t(declared[item]);
    if (!same_items) {
        const ConfigOptionDef *def = print_config_def.get("nozzle_volume_type");
        combo->Clear();
        for (int type : declared)
            combo->Append(def != nullptr && size_t(type) < def->enum_labels.size() ? _L(def->enum_labels[size_t(type)]) :
                                                                                      from_u8(get_nozzle_volume_type_string(NozzleVolumeType(type))),
                          {}, (void*)intptr_t(type));
    }

    // A head that cannot run High Flow shows its first declared type whatever is stored; the
    // sanitizer corrects the stored value.
    const int shown     = shown_volume_type(printer_config, head, current_type, size_offers);
    int       selection = 0;
    for (size_t item = 0; item < declared.size(); ++item)
        if (declared[item] == shown)
            selection = int(item);
    if (combo->GetSelection() != selection)
        combo->SetSelection(selection);
    combo->Enable(usable);
    combo->SetToolTip(flow_tooltip(usable ? FlowRowState::Choice : FlowRowState::RuledOut, head_nozzle_size_label(printer_config, head)));
}

wxString flow_tooltip(FlowRowState state, const std::string &head_size)
{
    if (state == FlowRowState::RuledOut && !head_size.empty())
        // TRN Tooltip of a disabled Flow row. %1% is the nozzle size of this tool head, e.g. 0.2
        return format_wxstr(_L("Flow type of this nozzle. Snapmaker Orca has no High Flow values for %1% mm nozzles, so this nozzle prints with the Standard values."), head_size);
    return _L("Flow type of this nozzle. A High Flow nozzle prints with the High Flow values of the filament and process presets.");
}

}}} // namespace Slic3r::GUI::HighFlowNotices
