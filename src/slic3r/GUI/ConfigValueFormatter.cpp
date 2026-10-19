#include "ConfigValueFormatter.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "I18N.hpp"
#include "GUI.hpp"
#include "Field.hpp"

namespace Slic3r {
namespace GUI {

std::string get_pure_opt_key(const std::string& opt_key)
{
    std::string pure_key = opt_key;
    const int pos = pure_key.find("#");
    if (pos > 0)
        boost::erase_tail(pure_key, pure_key.size() - pos);
    return pure_key;
}

wxString get_string_from_enum(const std::string& opt_key, const DynamicPrintConfig& config, bool is_infill, int idx)
{
    const ConfigOptionDef& def = config.def()->options.at(opt_key);
    const std::vector<std::string>& names = def.enum_labels;//ConfigOptionEnum<T>::get_enum_names();
    int val = 0;

    if (idx >= 0) {
        const auto* values = dynamic_cast<const ConfigOptionInts*>(config.option(opt_key));
        if (values == nullptr || size_t(idx) >= values->size())
            return _L("Undefined");
        val = values->values[idx];
        // A nil entry of a nullable enum array (e.g. an unchecked retraction
        // override) is not a valid index into enum_labels.
        if (values->nullable() && val == ConfigOptionInts::nil_value())
            return _L("Undefined");
    }
    else
        val = config.option(opt_key)->getInt();

    // Each infill doesn't use all list of infill declared in PrintConfig.hpp.
    // So we should "convert" val to the correct one
    if (is_infill) {
        for (auto key_val : *def.enum_keys_map)
            if (int(key_val.second) == val) {
                auto it = std::find(def.enum_values.begin(), def.enum_values.end(), key_val.first);
                if (it == def.enum_values.end())
                    return "";
                return from_u8(_utf8(names[it - def.enum_values.begin()]));
            }
        return _L("Undefined");
    }
    // Unknown int values (older presets, stray entries) must not index out of enum_labels.
    return (val >= 0 && size_t(val) < names.size()) ? from_u8(_utf8(names[val])) : _L("Undefined");
}

wxString get_full_label(const std::string& opt_key, const DynamicPrintConfig& config)
{
    const std::string pure_key = get_pure_opt_key(opt_key);
    auto option = config.option(pure_key);

    if (!option || option->is_nil())
        return _L("N/A");

    const ConfigOptionDef* opt = config.def()->get(pure_key);
    if (opt == nullptr)
        return from_u8(pure_key);
    return opt->full_label.empty() ? opt->label : opt->full_label;
}

// Joins all elements of a multi-value (per-extruder / per-nozzle-variant) option
// whose dirty key has no "#index"; a nil element of a nullable vector shows as N/A.
template<typename Vec, typename Format>
static wxString join_vector_values(const Vec* values, Format format_element)
{
    if (values == nullptr || values->empty())
        return _L("Undefined");

    std::string out;
    for (size_t i = 0; i < values->size(); ++i) {
        if (i > 0)
            out += ", ";
        out += values->is_nil(i) ? into_u8(_L("N/A")) : format_element(values->get_at(i));
    }
    return from_u8(out);
}

static std::string format_int_value(int value)        { return (boost::format("%1%") % value).str(); }
static std::string format_float_value(double value)   { return into_u8(double_to_string(value)); }
static std::string format_percent_value(double value) { return (boost::format("%1%%%") % value).str(); }
static std::string format_bool_value(bool value)      { return value ? "true" : "false"; }
static std::string format_float_or_percent_value(const FloatOrPercent& value)
{
    return into_u8(double_to_string(value.value)) + (value.percent ? "%" : "");
}

// One element ("key#index") or all elements of a vector option. Casts to the common base, since a
// config may hold the nullable or the plain flavour regardless of the definition. An index past
// the end (a preset with fewer variant columns) reads "Undefined".
template<typename T, typename Format>
static wxString format_vector_option(const ConfigOption* option, int index, Format format_element)
{
    const auto* values = dynamic_cast<const ConfigOptionVector<T>*>(option);
    if (values == nullptr)
        return _L("Undefined");
    if (index >= 0)
        return size_t(index) < values->size() ? from_u8(format_element(values->get_at(size_t(index)))) : _L("Undefined");
    return join_vector_values(values, format_element);
}

wxString get_string_value(const std::string& opt_key, const DynamicPrintConfig& config)
{
    int orig_opt_idx = -1;
    int opt_idx = -1;
    int pos = opt_key.find("#");
    std::string temp_str = opt_key;
    if (pos > 0) {
        boost::erase_head(temp_str, pos + 1);
        orig_opt_idx = std::atoi(temp_str.c_str());
    }
    opt_idx = orig_opt_idx >= 0 ? orig_opt_idx : 0;
    const std::string pure_key = get_pure_opt_key(opt_key);
    auto option = config.option(pure_key);
    if (!option) {
        return _L("N/A");
    }
    auto opt_vector = dynamic_cast<const ConfigOptionVectorBase *>(option);

    if ((option->is_scalar() && option->is_nil()) ||
        (option->is_vector() && opt_vector && orig_opt_idx >= 0 && opt_idx < (int) opt_vector->size() && opt_vector->is_nil(opt_idx)))
        return _L("N/A");

    wxString out;

    const ConfigOptionDef* opt = config.def()->get(pure_key);
    if (opt == nullptr)
        return from_u8(option->serialize());
    if (option->type() != opt->type)
        return from_u8(option->serialize());

    switch (opt->type) {
    case coInt:
        return from_u8((boost::format("%1%") % config.opt_int(pure_key)).str());
    case coInts:
        return format_vector_option<int>(option, orig_opt_idx, format_int_value);
    case coBool:
        return config.opt_bool(pure_key) ? "true" : "false";
    case coBools:
        return format_vector_option<unsigned char>(option, orig_opt_idx, format_bool_value);
    case coPercent:
        return from_u8((boost::format("%1%%%") % int(config.optptr(pure_key)->getFloat())).str());
    case coPercents:
        return format_vector_option<double>(option, orig_opt_idx, format_percent_value);
    case coFloat:
        return double_to_string(config.opt_float(pure_key));
    case coFloats:
        return format_vector_option<double>(option, orig_opt_idx, format_float_value);
    case coString:
        return from_u8(config.opt_string(pure_key));
    case coStrings: {
        const ConfigOptionStrings* strings = config.opt<ConfigOptionStrings>(pure_key);
        if (strings) {
            if (pure_key == "compatible_printers" || pure_key == "compatible_prints") {
                if (strings->empty())
                    return _L("All");
                for (size_t id = 0; id < strings->size(); id++)
                    out += from_u8(strings->get_at(id)) + "\n";
                out.RemoveLast(1);
                return out;
            }
            if (!strings->empty()) {
                if (orig_opt_idx >= 0)
                    return opt_idx < strings->values.size() ? from_u8(strings->get_at(opt_idx)) : _L("Undefined");
                return join_vector_values(strings, [](const std::string& value) { return value; });
            }
        }
        break;
        }
    case coFloatOrPercent: {
        const ConfigOptionFloatOrPercent* opt = config.opt<ConfigOptionFloatOrPercent>(pure_key);
        if (opt)
            out = double_to_string(opt->value) + (opt->percent ? "%" : "");
        return out;
    }
    case coFloatsOrPercents: {
        // The nullable and non-nullable variants are distinct template
        // instantiations; address them through their common base.
        const ConfigOptionVector<FloatOrPercent>* values = dynamic_cast<const ConfigOptionFloatsOrPercents*>(option);
        if (values == nullptr)
            values = dynamic_cast<const ConfigOptionFloatsOrPercentsNullable*>(option);
        if (values == nullptr)
            break;
        if (orig_opt_idx >= 0)
            return orig_opt_idx < values->size() ? from_u8(format_float_or_percent_value(values->get_at(orig_opt_idx))) : _L("Undefined");
        return join_vector_values(values, format_float_or_percent_value);
    }
    case coEnum: {
        return get_string_from_enum(pure_key, config,
            pure_key == "top_surface_pattern" ||
            pure_key == "bottom_surface_pattern" ||
            pure_key == "internal_solid_infill_pattern" ||
            pure_key == "sparse_infill_pattern" ||
            pure_key == "ironing_pattern" ||
            pure_key == "support_ironing_pattern" ||
            pure_key == "support_pattern" ||
            pure_key == "support_interface_pattern")
            ;
    }
    case coEnums: {
        const bool is_infill = pure_key == "top_surface_pattern" ||
                               pure_key == "bottom_surface_pattern" ||
                               pure_key == "internal_solid_infill_pattern" ||
                               pure_key == "sparse_infill_pattern" ||
                               pure_key == "ironing_pattern" ||
                               pure_key == "support_ironing_pattern" ||
                               pure_key == "support_pattern" ||
                               pure_key == "support_interface_pattern";
        if (orig_opt_idx < 0) {
            const auto* values = dynamic_cast<const ConfigOptionInts*>(option);
            if (values != nullptr && !values->empty()) {
                std::string joined;
                for (size_t i = 0; i < values->size(); ++i) {
                    if (i > 0)
                        joined += ", ";
                    joined += into_u8(get_string_from_enum(pure_key, config, is_infill, int(i)));
                }
                return from_u8(joined);
            }
        }
        return get_string_from_enum(pure_key, config, is_infill, opt_idx);
    }
    case coPoint: {
        Vec2d val = config.opt<ConfigOptionPoint>(pure_key)->value;
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    case coPoints: {
        //BBS: add bed_exclude_area
        if (pure_key == "printable_area" || pure_key == "thumbnails") {
            ConfigOptionPoints points = *config.option<ConfigOptionPoints>(pure_key);
            //BuildVolume build_volume = {points.values, 0.};
            return get_thumbnails_string(points.values);
        }
        else if (pure_key == "bed_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        else if (pure_key == "head_wrap_detect_zone") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        else if (pure_key == "wrapping_exclude_area") {
            return get_thumbnails_string(config.option<ConfigOptionPoints>(pure_key)->values);
        }
        Vec2d val = config.opt<ConfigOptionPoints>(pure_key)->get_at(opt_idx);
        return from_u8((boost::format("[%1%]") % ConfigOptionPoint(val).serialize()).str());
    }
    default:
        break;
    }
    return out;
}

} // namespace GUI
} // namespace Slic3r
