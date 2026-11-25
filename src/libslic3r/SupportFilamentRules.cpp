#include "SupportFilamentRules.hpp"

#include "libslic3r/AllowlistManager.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <memory>

namespace Slic3r {

namespace {

static std::string normalized_support_filament_type(const std::string& value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    std::string normalized = value.substr(start, end - start + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return normalized;
}

static std::string trimmed_string(const std::string& value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool is_print_process_option(const std::string& key)
{
    const auto& print_options = Preset::print_options();
    return std::find(print_options.begin(), print_options.end(), key) != print_options.end();
}

static bool apply_recommendation_entry(std::map<std::string, std::string>& config, const std::string& entry)
{
    const size_t separator = entry.find('=');
    if (separator == std::string::npos) {
        BOOST_LOG_TRIVIAL(error) << "Support filament recommendation entry must be key=value: " << entry;
        return false;
    }

    const std::string      key        = trimmed_string(entry.substr(0, separator));
    const std::string      raw_value  = trimmed_string(entry.substr(separator + 1));
    if (!is_print_process_option(key)) {
        BOOST_LOG_TRIVIAL(error) << "Support filament recommendation is not a print process option: " << key;
        return false;
    }

    const ConfigOptionDef* definition = print_config_def.get(key);
    if (definition == nullptr || definition->default_value.get() == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Support filament recommendation uses unknown key: " << key;
        return false;
    }

    std::unique_ptr<ConfigOption> option(definition->default_value->clone());
    std::string                   value = raw_value;
    if (option->type() == coBool && value == "false")
        value = "0";
    else if (option->type() == coBool && value == "true")
        value = "1";
    if (!option->deserialize(value)) {
        BOOST_LOG_TRIVIAL(error) << "Support filament recommendation has invalid value for " << key << ": " << value;
        return false;
    }

    config.emplace(key, option->serialize());
    return true;
}

static std::map<std::string, std::string> parse_recommendations(const std::vector<std::string>& entries)
{
    std::map<std::string, std::string> result;
    for (const std::string& entry : entries)
        apply_recommendation_entry(result, entry);
    return result;
}

static std::vector<std::string> recommendation_keys(const std::map<std::string, std::string>& recommendations)
{
    std::vector<std::string> result;
    result.reserve(recommendations.size());
    for (const auto& recommendation : recommendations)
        result.push_back(recommendation.first);
    return result;
}

} // namespace

SupportFilamentKind classify_support_filament(const DynamicPrintConfig& config, unsigned int extruder_id)
{
    const auto* is_support = config.option<ConfigOptionBools>("filament_is_support");
    if (is_support == nullptr || extruder_id >= is_support->size() || !is_support->get_at(extruder_id))
        return SupportFilamentKind::None;

    const auto* type_opt = config.option<ConfigOptionStrings>("filament_type");
    if (type_opt == nullptr || extruder_id >= type_opt->values.size())
        return SupportFilamentKind::OtherSupport;

    const std::string filament_type = normalized_support_filament_type(type_opt->values[extruder_id]);
    if (filament_type == "PLA")
        return SupportFilamentKind::SupportPla;
    if (filament_type == "PVA")
        return SupportFilamentKind::Pva;
    return SupportFilamentKind::OtherSupport;
}

int support_guide_choice_bit(SupportFilamentKind kind)
{
    switch (kind) {
    case SupportFilamentKind::SupportPla: return 1;
    case SupportFilamentKind::Pva: return 2;
    case SupportFilamentKind::OtherSupport: return 4;
    default: return 0;
    }
}

bool support_guide_choice_hidden(int choice, SupportFilamentKind kind)
{
    const int bit = support_guide_choice_bit(kind);
    return bit != 0 && (choice & bit) != 0;
}

int support_guide_choice_from_string(const std::string& choice)
{
    try {
        return std::stoi(choice);
    } catch (const std::exception&) {
        return 0;
    }
}

bool support_filament_types_compatible(const std::string& lhs, const std::string& rhs)
{
    const std::string left  = normalized_support_filament_type(lhs);
    const std::string right = normalized_support_filament_type(rhs);
    return !left.empty() && left == right;
}

std::string support_filament_guide_url(SupportFilamentKind kind, const std::string& language_code)
{
    const bool chinese = language_code.rfind("zh", 0) == 0;
    switch (kind) {
    case SupportFilamentKind::SupportPla:
        return "https://wiki.snapmaker.com/en/FAQ/Modules_Accessories_Materials/Support_For_PLA_Filament";
    case SupportFilamentKind::Pva:
        return chinese ? "https://wiki.snapmaker.com/zh/general/manual/dissolving_PVA" :
                         "https://wiki.snapmaker.com/en/snapmaker_artisan/troubleshooting/pva_tpu_breakaway_support";
    default:
        return {};
    }
}

std::map<std::string, std::string> support_recommendation_diff(const DynamicPrintConfig&                 config,
                                                               const std::map<std::string, std::string>& recommendations)
{
    std::map<std::string, std::string> result;
    for (const auto& recommendation : recommendations) {
        const ConfigOptionDef* definition = print_config_def.get(recommendation.first);
        if (definition == nullptr || definition->default_value.get() == nullptr)
            continue;

        std::unique_ptr<ConfigOption> option(definition->default_value->clone());
        std::string                   value = recommendation.second;
        if (option->type() == coBool && value == "false")
            value = "0";
        else if (option->type() == coBool && value == "true")
            value = "1";
        if (!option->deserialize(value))
            continue;

        const ConfigOption* current = config.option(recommendation.first);
        if (current == nullptr || current->serialize() != option->serialize())
            result.emplace(recommendation.first, option->serialize());
    }
    return result;
}

std::map<std::string, std::string> support_restore_default_diff(const DynamicPrintConfig&       current,
                                                                const DynamicPrintConfig&       selected,
                                                                const std::vector<std::string>& keys)
{
    std::map<std::string, std::string> result;
    for (const std::string& key : keys) {
        const ConfigOption* current_option  = current.option(key);
        const ConfigOption* selected_option = selected.option(key);
        if (current_option == nullptr || selected_option == nullptr)
            continue;
        if (current_option->serialize() != selected_option->serialize())
            result.emplace(key, selected_option->serialize());
    }
    return result;
}

SupportFilamentRules SupportFilamentRules::from_lists(const std::vector<std::string>& default_recommendations,
                                                      const std::vector<std::string>& support_pla_recommendations)
{
    SupportFilamentRules result;
    if (default_recommendations.empty() && support_pla_recommendations.empty())
        return result;

    const std::map<std::string, std::string> defaults = parse_recommendations(default_recommendations);

    SupportParameterRule fallback;
    fallback.id              = "default";
    fallback.recommendations = defaults;
    fallback.restore_check_keys = recommendation_keys(defaults);
    result.m_rules.emplace(SupportFilamentKind::OtherSupport, fallback);

    SupportParameterRule pva;
    pva.id              = "pva";
    pva.recommendations = defaults;
    pva.restore_check_keys = recommendation_keys(defaults);
    result.m_rules.emplace(SupportFilamentKind::Pva, std::move(pva));

    SupportParameterRule support_pla;
    support_pla.id              = "support_pla";
    support_pla.recommendations = parse_recommendations(support_pla_recommendations);
    for (const auto& recommendation : defaults)
        support_pla.recommendations.emplace(recommendation.first, recommendation.second);
    support_pla.restore_check_keys = recommendation_keys(support_pla.recommendations);
    result.m_rules.emplace(SupportFilamentKind::SupportPla, std::move(support_pla));

    result.m_loaded = true;
    return result;
}

const SupportFilamentRules& SupportFilamentRules::instance()
{
    static const SupportFilamentRules rules = [] {
        AllowlistManager& manager = AllowlistManager::instance();
        const auto         to_vector = [](const AllowlistManager::StringList& list) {
            return std::vector<std::string>(list.begin(), list.end());
        };

        return SupportFilamentRules::from_lists(
            to_vector(manager.get_list("support_filament", "default_recommendations")),
            to_vector(manager.get_list("support_filament", "support_pla_recommendations")));
    }();
    return rules;
}

const SupportParameterRule* SupportFilamentRules::rule(SupportFilamentKind kind) const
{
    const auto iterator = m_rules.find(kind);
    if (iterator != m_rules.end())
        return &iterator->second;
    return nullptr;
}

} // namespace Slic3r
