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

// The nozzle size of tool head `head` when it differs from the size the preset was made for; 0 for
// the preset's own size and when either size is unknown.
double other_nozzle_size(const DynamicPrintConfig &printer_config, size_t head)
{
    const double preset_size = preset_nozzle_size(printer_config);
    const auto  *diameters   = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (preset_size <= 0. || diameters == nullptr || head >= diameters->values.size())
        return 0.;
    const double head_size = diameters->values[head];
    return head_size > 0. && std::abs(head_size - preset_size) >= EPSILON ? head_size : 0.;
}

bool contains_high_flow(const std::vector<int> &types)
{
    return std::find(types.begin(), types.end(), int(nvtHighFlow)) != types.end();
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
    return contains_high_flow(declared_volume_types(printer_config, head));
}

std::vector<int> offered_volume_types(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    std::vector<int> offered = declared_volume_types(printer_config, head);
    if (contains_high_flow(offered))
        return offered;
    // A head of another size than the preset's: the machine preset of its size decides.
    const double size = other_nozzle_size(printer_config, head);
    if (size > 0. && size_offers && size_offers(size, head)) {
        offered.push_back(int(nvtHighFlow));
        std::sort(offered.begin(), offered.end());
    }
    return offered;
}

bool head_offers_high_flow(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    return contains_high_flow(offered_volume_types(printer_config, head, size_offers));
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
    // The preset's own size, or a preset that does not name its size: its declared columns decide.
    const double size = other_nozzle_size(printer_config, head);
    if (size <= 0.)
        return head_declares_high_flow(printer_config, head);
    // Another size: the machine preset of that size decides, whatever the printer preset declares.
    return size_offers && size_offers(size, head);
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
    return evaluate(nozzle_volume_types, filaments, std::vector<bool>(nozzle_volume_types.size(), process_has_high_flow_column), process_standard_only);
}

Report evaluate(const std::vector<int> &nozzle_volume_types, const std::vector<std::vector<HeadFilament>> &filaments,
                const std::vector<bool> &process_has_high_flow_column, bool process_standard_only)
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
        if (head >= process_has_high_flow_column.size() || !process_has_high_flow_column[head])
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

int flow_selector_index(const std::vector<int> &types, int type)
{
    const auto it = std::find(types.begin(), types.end(), type);
    return it == types.end() ? 0 : int(it - types.begin());
}

int variant_column_for_type(const std::vector<std::string> &variants, int type)
{
    const std::string wanted = get_nozzle_volume_type_string(NozzleVolumeType(type));
    for (size_t column = 0; column < variants.size(); ++column)
        if (split_variant_name(variants[column]).volume_type == wanted)
            return int(column);
    return -1;
}

bool flow_choice_usable(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    return !head_offers_high_flow(printer_config, head, size_offers) || head_can_use_high_flow(printer_config, head, size_offers);
}

FlowRowState flow_row_state(const DynamicPrintConfig &printer_config, size_t head, const SizeOffersHighFlow &size_offers)
{
    if (offered_volume_types(printer_config, head, size_offers).size() <= 1) {
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
    const std::vector<int> offered = offered_volume_types(printer_config, head, size_offers);   // never empty
    const bool             usable  = flow_choice_usable(printer_config, head, size_offers);
    for (int type : offered)
        if (type == stored_type && (usable || stored_type == int(nvtStandard)))
            return type;
    return offered.front();
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

SelectorFit head_selector_fit(const std::vector<int> &long_widths, const std::vector<int> &short_widths, int available)
{
    auto total = [](const std::vector<int> &widths) {
        int sum = 0;
        for (int width : widths)
            sum += width;
        return sum;
    };
    if (available <= 0 || total(long_widths) <= available)
        return SelectorFit::Long;
    if (total(short_widths) <= available)
        return SelectorFit::Short;
    return SelectorFit::ShortRows;
}

void fill_flow_combo(::ComboBox *combo, const DynamicPrintConfig &printer_config, size_t head, int current_type, const SizeOffersHighFlow &size_offers)
{
    if (combo == nullptr)
        return;
    const std::vector<int> offered = offered_volume_types(printer_config, head, size_offers);
    // A single offered type leaves nothing to choose (a Standard-only preset shown for a model
    // that offers High Flow at another size): the combo is disabled with the reason of the size.
    const bool             usable  = offered.size() > 1 && flow_choice_usable(printer_config, head, size_offers);

    bool same_items = combo->GetCount() == offered.size();
    for (size_t item = 0; same_items && item < offered.size(); ++item)
        same_items = intptr_t(combo->GetClientData(int(item))) == intptr_t(offered[item]);
    if (!same_items) {
        const ConfigOptionDef *def = print_config_def.get("nozzle_volume_type");
        // Labels are listed by position, which differs from the enum value from E3D High Flow on.
        auto label_of = [def](int type) -> wxString {
            if (def != nullptr && def->enum_keys_map != nullptr)
                for (size_t pos = 0; pos < def->enum_values.size() && pos < def->enum_labels.size(); ++pos)
                    if (auto it = def->enum_keys_map->find(def->enum_values[pos]); it != def->enum_keys_map->end() && it->second == type)
                        return _L(def->enum_labels[pos]);
            return from_u8(get_nozzle_volume_type_string(NozzleVolumeType(type)));
        };
        combo->Clear();
        for (int type : offered)
            combo->Append(label_of(type), {}, (void*)intptr_t(type));
    }

    // A head that cannot run High Flow shows its first offered type whatever is stored; the
    // sanitizer corrects the stored value.
    const int shown     = shown_volume_type(printer_config, head, current_type, size_offers);
    int       selection = 0;
    for (size_t item = 0; item < offered.size(); ++item)
        if (offered[item] == shown)
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

wxString automatic_reason(PerHeadProcess::Step step, const std::string &plate_class, const std::string &class_used, const std::string &head_size,
                          double height, bool preferred)
{
    const wxString height_text = from_u8(float_to_string_decimal_point(height, 2));
    switch (step) {
    case PerHeadProcess::Step::SameQuality:
        if (plate_class.empty())
            return preferred ?
                // TRN Why a tool head prints with a process preset. %1% is a layer height in mm
                format_wxstr(_L("Automatic: the nearest layer height to the preferred %1% mm."), height_text) :
                // TRN Why a tool head prints with a process preset. %1% is a layer height in mm
                format_wxstr(_L("Automatic: the nearest layer height to the plate's %1% mm."), height_text);
        return preferred ?
            // TRN Why a tool head prints with a process preset. %1% is a quality class ("Standard"), %2% a layer height in mm
            format_wxstr(_L("Automatic: the plate's quality (%1%), nearest to the preferred layer height %2% mm."), from_u8(plate_class), height_text) :
            // TRN Why a tool head prints with a process preset. %1% is a quality class ("Standard"), %2% a layer height in mm
            format_wxstr(_L("Automatic: the plate's quality (%1%), nearest to the plate's layer height %2% mm."), from_u8(plate_class), height_text);
    case PerHeadProcess::Step::ClassLadder:
        if (plate_class.empty())
            return preferred ?
                // TRN Why a tool head prints with a process preset. %1% is a quality class ("Standard"), %2% a layer height in mm
                format_wxstr(_L("Automatic: %1%, nearest to the preferred layer height %2% mm."), from_u8(class_used), height_text) :
                // TRN Why a tool head prints with a process preset. %1% is a quality class ("Standard"), %2% a layer height in mm
                format_wxstr(_L("Automatic: %1%, nearest to the plate's layer height %2% mm."), from_u8(class_used), height_text);
        return preferred ?
            // TRN Why a tool head prints with a process preset. %1% is the plate's quality class ("High Quality"), %2% a nozzle size, %3% the class used instead ("Standard"), %4% a layer height in mm
            format_wxstr(_L("Automatic: no %1% preset exists for a %2% mm nozzle, so %3%, nearest to the preferred layer height %4% mm."),
                         from_u8(plate_class), from_u8(head_size), from_u8(class_used), height_text) :
            // TRN Why a tool head prints with a process preset. %1% is the plate's quality class ("High Quality"), %2% a nozzle size, %3% the class used instead ("Standard"), %4% a layer height in mm
            format_wxstr(_L("Automatic: no %1% preset exists for a %2% mm nozzle, so %3%, nearest to the plate's layer height %4% mm."),
                         from_u8(plate_class), from_u8(head_size), from_u8(class_used), height_text);
    case PerHeadProcess::Step::SizeDefault:
        // TRN Why a tool head prints with a process preset. %1% is a nozzle size
        return format_wxstr(_L("Automatic: the default preset of a %1% mm nozzle."), from_u8(head_size));
    case PerHeadProcess::Step::FirstByName:
        // TRN Why a tool head prints with a process preset. %1% is a nozzle size
        return format_wxstr(_L("Automatic: the first preset installed for a %1% mm nozzle."), from_u8(head_size));
    case PerHeadProcess::Step::SelectedPreset:
    case PerHeadProcess::Step::Chosen:
        break;
    }
    return wxEmptyString;
}

wxString head_entry_tooltip(const std::string &head_size, NozzleVolumeType nozzle, bool standard_chosen)
{
    if (standard_chosen)
        // TRN %1% the nozzle size
        return format_wxstr(_L("%1% mm nozzle, High Flow, Standard speeds (chosen)"), from_u8(head_size));
    // TRN %1% the nozzle size, %2% the flow type ("Standard")
    return format_wxstr(_L("%1% mm nozzle, %2%"), from_u8(head_size), _L(get_nozzle_volume_type_string(nozzle)));
}

wxString speeds_hint_label(const wxString &preset, const wxString &state, SpeedsNote note)
{
    switch (note) {
    case SpeedsNote::HighFlow:
        // TRN %1% a process preset ("0.20mm Standard"), %2% "(automatic)" or "(chosen)"
        return format_wxstr(_L("Preset: %1%, High Flow %2%"), preset, state);
    case SpeedsNote::StandardChosen:
        // TRN %1% a process preset with its state ("0.18mm Standard (automatic)")
        return format_wxstr(_L("Preset: %1%, Standard speeds (chosen)"), state.IsEmpty() ? preset : preset + " " + state);
    default:
        // TRN %1% a process preset ("0.12mm Standard"), %2% "(automatic)" or "(chosen)"
        return format_wxstr(_L("Preset: %1% %2%"), preset, state);
    }
}

wxString standard_chosen_tooltip()
{
    return _L("This extruder carries a High Flow nozzle and prints the Standard speeds, accelerations and jerk, chosen with the toggle of the "
              "Speed page; its filament settings stay those of a High Flow nozzle.");
}

wxString own_preset_description(bool quality_page, const std::string &heads, bool several)
{
    if (quality_page)
        return several ?
            // TRN %1% extruder numbers ("1, 3")
            format_wxstr(_L("Extruders %1% print the line widths of their extruder presets."), from_u8(heads)) :
            // TRN %1% an extruder number ("3")
            format_wxstr(_L("Extruder %1% prints the line widths of its extruder preset."), from_u8(heads));
    return several ?
        // TRN %1% extruder numbers ("2, 3")
        format_wxstr(_L("Extruders %1% print the speeds of their extruder presets."), from_u8(heads)) :
        // TRN %1% an extruder number ("2")
        format_wxstr(_L("Extruder %1% prints the speeds of its extruder preset."), from_u8(heads));
}

wxString preferred_height_sentence(double height)
{
    // TRN %1% a layer height
    return format_wxstr(_L("Prints %1% mm layers."), from_u8(float_to_string_decimal_point(height, 2)));
}

wxString values_set_label(size_t count)
{
    return format_wxstr(_L_PLURAL("%1% value set", "%1% values set", unsigned(count)), count);
}

wxString shared_settings_sentence()
{
    return _L("Greyed settings apply to every extruder.");
}

wxString clear_head_link_label(bool quality_page)
{
    // TRN Link under a selected tool head of the Quality / Speed page of the process settings
    return quality_page ? _L("Clear the line widths set for this extruder") : _L("Clear the speeds set for this extruder");
}

std::string old_reader_width(const std::vector<std::string> &values)
{
    if (values.empty())
        return std::string();
    bool percent = false;
    for (const std::string &value : values)
        percent = percent || value.find('%') != std::string::npos;
    std::string number = values.front();
    boost::trim(number);
    if (!number.empty() && number.back() == '%')
        number.pop_back();
    boost::trim(number);
    return number + (percent ? " %" : " mm");
}

wxString mixed_unit_sentence(const wxString &label, const std::string &old_reader_value)
{
    // TRN Under a selected tool head on the Quality page: a line width set for it has the other unit than the value under All tool heads. %1% the setting ("Default"), %2% the value an older version reads ("0.42 %")
    return format_wxstr(_L("Older versions of Snapmaker Orca read this preset's %1% as %2%; use the same unit for every extruder."), label, from_u8(old_reader_value));
}

std::string width_value_label(const FloatOrPercent &width, double nozzle)
{
    if (width.value <= 0.)
        // TRN A line width of zero in a tooltip: the width follows the nozzle
        return _u8L("auto");
    std::string number = ConfigOptionFloatOrPercent(width.value, width.percent).serialize();
    if (!width.percent)
        return number + " mm";
    if (!number.empty() && number.back() == '%')
        number.pop_back();
    return number + " % (" + float_to_string_decimal_point(width.value / 100. * nozzle, 2) + " mm)";
}

bool widths_differ(const DynamicPrintConfig &a, const DynamicPrintConfig &b)
{
    const int column_a = PerHeadProcess::shared_column(a, nvtStandard);
    const int column_b = PerHeadProcess::shared_column(b, nvtStandard);
    auto text_of = [](const DynamicPrintConfig &config, const std::string &key, int column) {
        const auto *option = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || option->empty())
            return std::string();
        const std::vector<std::string> values = option->vserialize();
        return values[column >= 0 && size_t(column) < values.size() ? size_t(column) : 0];
    };
    for (const std::string &key : PerHeadProcess::flow_independent_keys())
        if (text_of(a, key, column_a) != text_of(b, key, column_b))
            return true;
    return false;
}

wxString object_width_notice(const std::vector<size_t> &heads)
{
    if (heads.empty())
        return wxEmptyString;
    wxString list;
    for (size_t i = 0; i + 1 < heads.size(); ++i)
        list += wxString(i > 0 ? ", " : "") + from_u8(std::to_string(heads[i] + 1));
    const wxString last = from_u8(std::to_string(heads.back() + 1));
    // TRN The last tool head of a list: "1, 3 and 4"
    list = heads.size() == 1 ? last : format_wxstr(_L("%1% and %2%"), list, last);
    // TRN Notice of the object list once "Add settings" added a line width to an object printed by several tool heads. %1% the tool heads that printed it with another line width ("1 and 4")
    return format_wxstr(_L_PLURAL("Line widths added to an object apply on every extruder that prints it; extruder %1% printed it with the line widths of its own nozzle size.",
                                  "Line widths added to an object apply on every extruder that prints it; extruders %1% printed it with the line widths of their own nozzle size.",
                                  unsigned(heads.size())),
                        list);
}

}}} // namespace Slic3r::GUI::HighFlowNotices
