#pragma once

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

enum class SupportFilamentKind { None, Pva, SupportPla, OtherSupport };

SupportFilamentKind                classify_support_filament(const DynamicPrintConfig& config, unsigned int extruder_id = 0);
int                                support_guide_choice_bit(SupportFilamentKind kind);
bool                               support_guide_choice_hidden(int choice, SupportFilamentKind kind);
int                                support_guide_choice_from_string(const std::string& choice);
bool                               support_filament_types_compatible(const std::string& lhs, const std::string& rhs);
std::string                        support_filament_guide_url(SupportFilamentKind kind, const std::string& language_code);
std::map<std::string, std::string> support_recommendation_diff(const DynamicPrintConfig&                 config,
                                                               const std::map<std::string, std::string>& recommendations);
std::map<std::string, std::string> support_restore_default_diff(const DynamicPrintConfig&       current,
                                                                const DynamicPrintConfig&       selected,
                                                                const std::vector<std::string>& keys);

struct SupportParameterRule
{
    std::string                        id;
    std::vector<std::string>           restore_check_keys;
    std::map<std::string, std::string> recommendations;
};

class SupportFilamentRules
{
public:
    static const SupportFilamentRules& instance();
    static SupportFilamentRules        from_lists(const std::vector<std::string>& default_recommendations,
                                                   const std::vector<std::string>& support_pla_recommendations);

    const SupportParameterRule* rule(SupportFilamentKind kind) const;
    bool                        loaded() const { return m_loaded; }

private:
    std::map<SupportFilamentKind, SupportParameterRule> m_rules;
    bool                                                m_loaded{false};
};

} // namespace Slic3r
