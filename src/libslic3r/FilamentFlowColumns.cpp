#include "FilamentFlowColumns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <type_traits>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include "Config.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

namespace {

const char *const VARIANT_KEY = "filament_extruder_variant";

// The present variant keys of `config` other than the variant list itself.
template<class Config, class Fn> void for_each_variant_key(Config &config, Fn &&fn)
{
    for (const std::string &key : filament_options_with_variant) {
        if (key == VARIANT_KEY)
            continue;
        auto *option = config.option(key);
        if (option == nullptr || !option->is_vector())
            continue;
        fn(key, *static_cast<std::conditional_t<std::is_const_v<Config>, const ConfigOptionVectorBase, ConfigOptionVectorBase> *>(option));
    }
}

bool variant_names_flow(const std::string &variant, NozzleVolumeType type)
{
    for (ExtruderType drive : {etDirectDrive, etBowden})
        if (variant == get_extruder_variant_string(drive, type))
            return true;
    return false;
}

// Puts the columns of `config` in the order of `order` (a permutation of its variant list).
void reorder_columns(DynamicPrintConfig &config, const std::vector<std::string> &order)
{
    auto *list = config.option<ConfigOptionStrings>(VARIANT_KEY);
    if (list == nullptr || list->values.size() != order.size() || list->values == order)
        return;
    std::vector<size_t> source;
    for (const std::string &variant : order) {
        const auto found = std::find(list->values.begin(), list->values.end(), variant);
        if (found == list->values.end())
            return;
        source.emplace_back(size_t(found - list->values.begin()));
    }
    for_each_variant_key(config, [&source](const std::string &, ConfigOptionVectorBase &option) {
        if (option.size() != source.size())
            return;
        const std::unique_ptr<ConfigOption> copy(option.clone());
        for (size_t column = 0; column < source.size(); ++column)
            option.set_at(copy.get(), column, source[column]);
    });
    list->values = order;
}

} // namespace

std::vector<std::string> filament_variants(const DynamicPrintConfig &config)
{
    const auto *variants = config.option<ConfigOptionStrings>(VARIANT_KEY);
    return variants == nullptr ? std::vector<std::string>() : variants->values;
}

int filament_flow_column(const DynamicPrintConfig &config, NozzleVolumeType type)
{
    if (type == nvtHybrid)
        type = nvtStandard;
    const std::vector<std::string> variants = filament_variants(config);
    for (size_t column = 0; column < variants.size(); ++column)
        if (variant_names_flow(variants[column], type))
            return int(column);
    return -1;
}

bool is_known_filament_variant(const std::string &variant)
{
    for (NozzleVolumeType type : get_valid_nozzle_volume_type())
        if (variant_names_flow(variant, type))
            return true;
    return false;
}

bool filament_add_variant_column(DynamicPrintConfig &config, const std::string &variant)
{
    auto *variants = config.option<ConfigOptionStrings>(VARIANT_KEY);
    if (variant.empty() || variants == nullptr || variants->values.empty() ||
        std::find(variants->values.begin(), variants->values.end(), variant) != variants->values.end())
        return false;
    // Each key is first brought to the width of the list, so the new value lands in the new column.
    filament_repair_columns(config);
    const size_t width = variants->values.size();
    for_each_variant_key(config, [width](const std::string &, ConfigOptionVectorBase &option) {
        if (option.empty())
            return;
        option.resize(width + 1);
        option.set_at(&option, width, 0);
    });
    variants->values.emplace_back(variant);
    return true;
}

bool filament_add_flow_column(DynamicPrintConfig &config, NozzleVolumeType type)
{
    if (type == nvtStandard || type == nvtHybrid)
        return false;
    const std::vector<std::string> variants = filament_variants(config);
    if (variants.empty())
        return false;
    const ExtruderType drive = boost::starts_with(variants.front(), "Bowden") ? etBowden : etDirectDrive;
    return filament_add_variant_column(config, get_extruder_variant_string(drive, type));
}

bool filament_columns_consistent(const DynamicPrintConfig &config)
{
    const size_t width = filament_variants(config).size();
    if (width == 0)
        return true;
    bool consistent = true;
    for_each_variant_key(config, [width, &consistent](const std::string &, const ConfigOptionVectorBase &option) {
        if (option.size() != width)
            consistent = false;
    });
    return consistent;
}

bool filament_repair_columns(DynamicPrintConfig &config)
{
    const size_t width = filament_variants(config).size();
    if (width == 0)
        return false;
    bool changed = false;
    for_each_variant_key(config, [width, &changed](const std::string &key, ConfigOptionVectorBase &option) {
        if (option.empty() || option.size() == width)
            return;
        BOOST_LOG_TRIVIAL(warning) << "filament_repair_columns: " << key << " has " << option.size() << " values for " << width << " columns";
        option.resize(width);
        changed = true;
    });
    return changed;
}

std::vector<size_t> filament_columns_below(const DynamicPrintConfig &config, const std::string &key, double min)
{
    std::vector<size_t> columns;
    auto collect = [&columns, min](const std::vector<double> &values) {
        for (size_t column = 0; column < values.size(); ++column)
            if (!std::isnan(values[column]) && values[column] < min)
                columns.emplace_back(column);
    };
    if (const auto *floats = dynamic_cast<const ConfigOptionFloats *>(config.option(key)); floats != nullptr)
        collect(floats->values);
    else if (const auto *nullable = dynamic_cast<const ConfigOptionFloatsNullable *>(config.option(key)); nullable != nullptr)
        collect(nullable->values);
    return columns;
}

void filament_transfer_columns(DynamicPrintConfig &target, const DynamicPrintConfig &source, const std::vector<std::string> &indexed_keys)
{
    const std::vector<std::string> source_variants = filament_variants(source);
    for (const std::string &indexed : indexed_keys) {
        const size_t hash = indexed.find('#');
        if (hash == std::string::npos)
            continue;
        const std::string key    = indexed.substr(0, hash);
        const size_t      column = size_t(std::atoi(indexed.c_str() + hash + 1));
        if (key == VARIANT_KEY || filament_options_with_variant.count(key) == 0 || column >= source_variants.size())
            continue;
        const ConfigOption *from = source.option(key);
        auto               *to   = dynamic_cast<ConfigOptionVectorBase *>(target.option(key));
        if (from == nullptr || to == nullptr || from->type() != to->type())
            continue;
        const std::string &variant = source_variants[column];
        int target_column = -1;
        {
            const std::vector<std::string> target_variants = filament_variants(target);
            const auto found = std::find(target_variants.begin(), target_variants.end(), variant);
            if (found != target_variants.end())
                target_column = int(found - target_variants.begin());
            else if (is_known_filament_variant(variant) && filament_add_variant_column(target, variant))
                target_column = int(target_variants.size());
        }
        if (target_column < 0) {
            BOOST_LOG_TRIVIAL(info) << "filament_transfer_columns: " << indexed << " skipped, the target has no column " << variant;
            continue;
        }
        to->set_at(from, size_t(target_column), column);
    }
    filament_repair_columns(target);
}

const DynamicPrintConfig &filament_reference_in_layout_of(const DynamicPrintConfig &child, const DynamicPrintConfig &parent, DynamicPrintConfig &storage)
{
    const std::vector<std::string> child_variants  = filament_variants(child);
    const std::vector<std::string> parent_variants = filament_variants(parent);
    if (parent_variants.empty())
        return parent;
    std::vector<std::string> missing;
    for (const std::string &variant : child_variants)
        if (std::find(parent_variants.begin(), parent_variants.end(), variant) == parent_variants.end())
            missing.emplace_back(variant);
    if (missing.empty())
        return parent;
    storage = parent;
    for (const std::string &variant : missing)
        filament_add_variant_column(storage, variant);
    // A child that names the same columns in another order compares column by column in its own order.
    reorder_columns(storage, child_variants);
    return storage;
}

std::vector<std::string> filament_standard_edits_not_followed(const DynamicPrintConfig &edited, const DynamicPrintConfig &saved)
{
    std::vector<std::string> keys;
    const int high_flow = filament_flow_column(edited, nvtHighFlow);
    const int standard  = filament_flow_column(edited, nvtStandard);
    if (high_flow < 0 || standard < 0 || filament_flow_column(saved, nvtHighFlow) != high_flow || filament_flow_column(saved, nvtStandard) != standard)
        return keys;
    const size_t hf = size_t(high_flow), st = size_t(standard);
    for_each_variant_key(edited, [&](const std::string &key, const ConfigOptionVectorBase &mine) {
        const auto *theirs = dynamic_cast<const ConfigOptionVectorBase *>(saved.option(key));
        if (theirs == nullptr || mine.type() != theirs->type() || mine.size() < 2 || theirs->size() < 2 || mine.size() <= hf || theirs->size() <= hf ||
            mine.size() <= st || theirs->size() <= st)
            return;
        const std::vector<std::string> e = mine.vserialize(), s = theirs->vserialize();
        if (e[st] != s[st] && e[hf] == s[hf] && s[hf] == s[st])
            keys.emplace_back(key);
    });
    return keys;
}

bool filament_copy_standard_to_high_flow(DynamicPrintConfig &config, const std::vector<std::string> &keys)
{
    const int high_flow = filament_flow_column(config, nvtHighFlow);
    const int standard  = filament_flow_column(config, nvtStandard);
    if (high_flow < 0 || standard < 0)
        return false;
    bool changed = false;
    for (const std::string &key : keys) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option == nullptr || filament_options_with_variant.count(key) == 0 || option->size() <= size_t(std::max(high_flow, standard)))
            continue;
        const std::vector<std::string> before = option->vserialize();
        if (before[size_t(high_flow)] == before[size_t(standard)])
            continue;
        const std::unique_ptr<ConfigOption> copy(option->clone());
        option->set_at(copy.get(), size_t(high_flow), size_t(standard));
        changed = true;
    }
    return changed;
}

bool filament_drop_columns_not_in(DynamicPrintConfig &config, const std::vector<std::string> &variants)
{
    auto *list = config.option<ConfigOptionStrings>(VARIANT_KEY);
    if (list == nullptr || list->values.empty())
        return false;
    std::vector<size_t> kept;
    for (size_t column = 0; column < list->values.size(); ++column)
        if (std::find(variants.begin(), variants.end(), list->values[column]) != variants.end())
            kept.emplace_back(column);
    if (kept.empty())
        kept.emplace_back(0);
    if (kept.size() == list->values.size())
        return false;
    filament_repair_columns(config);
    for_each_variant_key(config, [&kept](const std::string &, ConfigOptionVectorBase &option) {
        if (option.empty())
            return;
        const std::unique_ptr<ConfigOption> copy(option.clone());
        for (size_t column = 0; column < kept.size(); ++column)
            option.set_at(copy.get(), column, kept[column]);
        option.resize(kept.size());
    });
    std::vector<std::string> names;
    for (size_t column : kept)
        names.emplace_back(list->values[column]);
    list->values = std::move(names);
    return true;
}

bool filament_field_shared_under_high_flow(const std::string &key)
{
    return filament_options_with_variant.count(key) == 0;
}

bool filament_self_index_from_counts(const std::vector<int> &counts, size_t list_size, std::vector<int> &out)
{
    size_t total = 0;
    for (int count : counts) {
        if (count < 0)
            return false;
        total += size_t(count);
    }
    if (total != list_size)
        return false;
    out.clear();
    out.reserve(total);
    for (size_t filament = 0; filament < counts.size(); ++filament)
        out.insert(out.end(), size_t(counts[filament]), int(filament) + 1);
    return true;
}

} // namespace Slic3r
