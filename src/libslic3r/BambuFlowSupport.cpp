#include "BambuFlowSupport.hpp"

#include "PresetFlowVariant.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r { namespace BambuFlowSupport {

namespace {

constexpr const char *STANDARD_SUFFIX  = " Standard";
constexpr const char *HIGH_FLOW_SUFFIX = " High Flow";

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "<type>" of "<type> Standard" / "<type> High Flow", empty when `name` is neither.
std::string variant_type(const std::string &name, const char *suffix)
{
    const std::string sfx(suffix);
    if (!ends_with(name, sfx) || name.size() == sfx.size())
        return {};
    return name.substr(0, name.size() - sfx.size());
}

// "TPU High Flow" / "E3D High Flow" end in " High Flow" too; their <type> keeps the qualifier.
bool is_qualified_high_flow_type(const std::string &type)
{
    return ends_with(type, " TPU") || ends_with(type, " E3D") || type == "TPU" || type == "E3D";
}

const std::vector<std::string> &standard_high_flow()
{
    static const std::vector<std::string> v{ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    return v;
}

// Not authored: missing, empty or still the default ["standard"].
bool flow_support_unauthored(const ConfigOptionStrings *opt)
{
    return opt == nullptr || opt->values.empty() || (opt->values.size() == 1 && opt->values.front() == FLOW_MODE_STANDARD);
}

const std::vector<std::string> *strings_of(const ConfigBase &config, const char *key)
{
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr || opt->type() != coStrings)
        return nullptr;
    return &static_cast<const ConfigOptionStrings *>(opt)->values;
}

bool contains_high_flow(const ConfigOptionStrings *opt)
{
    return opt != nullptr && std::find(opt->values.begin(), opt->values.end(), FLOW_MODE_HIGH_FLOW) != opt->values.end();
}

} // namespace

bool variant_names_standard_high_flow(const std::vector<std::string> &names)
{
    if (names.size() < 2)
        return false;
    const std::string std_type = variant_type(names[0], STANDARD_SUFFIX);
    const std::string hf_type  = variant_type(names[1], HIGH_FLOW_SUFFIX);
    return !std_type.empty() && std_type == hf_type && !is_qualified_high_flow_type(hf_type);
}

bool printer_variants_offer_high_flow(const std::vector<std::string> &extruder_variant_list)
{
    bool high_flow = false;
    for (const std::string &extruder : extruder_variant_list) {
        std::vector<std::string> variants;
        boost::split(variants, extruder, boost::is_any_of(","), boost::token_compress_on);
        bool has_standard       = false;
        bool has_plain_high_flow = false;
        for (std::string &v : variants) {
            boost::algorithm::trim(v);
            if (v.rfind("Bowden", 0) == 0)
                return false; // D9: the X2D (Bowden second extruder) stays Standard-only in phase 1
            if (!variant_type(v, STANDARD_SUFFIX).empty())
                has_standard = true;
            const std::string type = variant_type(v, HIGH_FLOW_SUFFIX);
            if (!type.empty() && !is_qualified_high_flow_type(type))
                has_plain_high_flow = true;
        }
        // High Flow is a choice only where the extruder also offers Standard. A lone
        // "Direct Drive High Flow" entry (Prusa CORE One INDX: every nozzle is a High Flow
        // nozzle) has nothing to switch to, so it must not turn on the Standard/High Flow selectors.
        if (has_standard && has_plain_high_flow)
            high_flow = true;
    }
    return high_flow;
}

DeriveResult derive(DynamicPrintConfig &config)
{
    DeriveResult result;
    const bool has_filament = config.has("filament_diameter");
    const bool has_process  = config.has("layer_height");
    const bool has_printer  = config.has("nozzle_diameter");
    // One preset type at a time; a composed full config is never rewritten here.
    if (int(has_filament) + int(has_process) + int(has_printer) != 1)
        return result;

    if (has_filament) {
        auto *support = config.option<ConfigOptionStrings>("filament_flow_support");
        const auto *names = strings_of(config, "filament_extruder_variant");
        const auto *mvs   = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        // A preset that names a High Flow variant but stores one value only (a few third-party
        // imports) has no High Flow data of its own: it stays Standard-only (D4 covers it).
        if (flow_support_unauthored(support) && names != nullptr && variant_names_standard_high_flow(*names) &&
            mvs != nullptr && mvs->values.size() >= 2) {
            config.option<ConfigOptionStrings>("filament_flow_support", true)->values = standard_high_flow();
            result.filament = true;
        }
    } else if (has_process) {
        auto       *support = config.option<ConfigOptionStrings>("process_flow_support");
        const auto *names   = strings_of(config, "print_extruder_variant");
        if (flow_support_unauthored(support) && names != nullptr && variant_names_standard_high_flow(*names)) {
            config.option<ConfigOptionStrings>("process_flow_support", true)->values = standard_high_flow();
            result.process = true;
        }
    } else {
        auto       *support = config.option<ConfigOptionStrings>("printer_flow_support");
        const auto *list    = strings_of(config, "extruder_variant_list");
        if (flow_support_unauthored(support) && list != nullptr && printer_variants_offer_high_flow(*list)) {
            config.option<ConfigOptionStrings>("printer_flow_support", true)->values = standard_high_flow();
            result.printer = true;
        }
    }
    return result;
}

NozzleVolumeType nozzle_flow_from_device_code(char code)
{
    switch (char(std::toupper((unsigned char) code))) {
    case 'H':
    case 'E': return NozzleVolumeType::nvtHighFlow;
    default: return NozzleVolumeType::nvtStandard;
    }
}

FilamentVolumeType slicing_flow_of_nozzle(int nozzle_volume_type)
{
    return nozzle_volume_type == int(NozzleVolumeType::nvtHighFlow) || nozzle_volume_type == int(NozzleVolumeType::nvtE3DHighFlow) ?
               fvtHighFlow :
               fvtStandard;
}

std::vector<FilamentVolumeType> filament_volume_types_from_map(const std::vector<int> &filament_map,
                                                               const std::vector<int> &nozzle_volume_types,
                                                               size_t                  filament_count)
{
    std::vector<FilamentVolumeType> out(filament_count, fvtStandard);
    for (size_t i = 0; i < filament_count && i < filament_map.size(); ++i) {
        const int extruder = filament_map[i] - 1; // 1-based logical extruder
        if (extruder >= 0 && size_t(extruder) < nozzle_volume_types.size())
            out[i] = slicing_flow_of_nozzle(nozzle_volume_types[size_t(extruder)]);
    }
    return out;
}

bool printer_supports_high_flow(const ConfigBase &config)
{
    const ConfigOption *opt = config.option("printer_flow_support");
    return opt != nullptr && opt->type() == coStrings && contains_high_flow(static_cast<const ConfigOptionStrings *>(opt));
}

bool apply_filament_volume_types_from_map(ConfigBase &config)
{
    if (!printer_supports_high_flow(config))
        return false;
    const auto *nozzles = dynamic_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"));
    if (nozzles == nullptr || nozzles->size() < 2)
        return false;
    const ConfigOption *map_opt = config.option("filament_map");
    const ConfigOption *nvt_opt = config.option("nozzle_volume_type");
    if (map_opt == nullptr || map_opt->type() != coInts || nvt_opt == nullptr || nvt_opt->type() != coEnums)
        return false;
    auto *fvt = config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true);
    if (fvt == nullptr)
        return false;
    const size_t count = flow_variant_filament_count(config);
    const std::vector<FilamentVolumeType> types =
        filament_volume_types_from_map(static_cast<const ConfigOptionInts *>(map_opt)->values,
                                       static_cast<const ConfigOptionEnumsGeneric *>(nvt_opt)->values, count);
    std::vector<int> values(types.begin(), types.end());
    if (fvt->values == values)
        return false;
    fvt->values = std::move(values);
    return true;
}

std::vector<unsigned int> filaments_without_high_flow_column(const ConfigBase &config, const std::vector<unsigned int> &filament_ids)
{
    std::vector<unsigned int> out;
    const ConfigOption *support_opt = config.option("filament_flow_support");
    const auto         *support     = support_opt != nullptr && support_opt->type() == coStrings ?
                                          static_cast<const ConfigOptionStrings *>(support_opt) :
                                          nullptr;
    for (unsigned int id : filament_ids) {
        if (filament_volume_type_at(config, id) != fvtHighFlow)
            continue;
        // get_config_idx lands on the filament's High Flow slot only when its segment declares one.
        const size_t idx = get_config_idx(config, ConfigFlowDomain::Filament, id);
        if (support == nullptr || idx >= support->values.size() || support->values[idx] != FLOW_MODE_HIGH_FLOW)
            out.push_back(id);
    }
    return out;
}

}} // namespace Slic3r::BambuFlowSupport
