#include "HighFlowCompat.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>

namespace Slic3r { namespace GUI { namespace HighFlowCompat {

namespace {

constexpr const char *g_mock_not_recommended[] = {
    "Snapmaker PLA Wood"
};
constexpr const char *g_mock_unsupported[] = {
    "Snapmaker TPU 85A"
};
constexpr std::string_view g_vendor_prefix = "snapmaker ";

std::string normalize(const std::string &value)
{
    std::string normalized = boost::algorithm::to_lower_copy(value);
    boost::algorithm::trim(normalized);

    std::string result;
    result.reserve(normalized.size());
    bool previous_space = false;
    for (const char ch : normalized)
    {
        const bool is_space = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (is_space && previous_space)
            continue;

        result.push_back(is_space ? ' ' : ch);
        previous_space = is_space;
    }

    return result;
}

bool has_material_token(const std::string &value, const std::string_view token)
{
    size_t position = value.find(token);
    while (position != std::string::npos)
    {
        const size_t end = position + token.size();
        const bool left_boundary = position == 0 || !std::isalnum(static_cast<unsigned char>(value[position - 1]));
        const bool right_boundary = end == value.size() || !std::isalnum(static_cast<unsigned char>(value[end]));
        if (left_boundary && right_boundary)
            return true;

        position = value.find(token, position + 1);
    }

    return false;
}

bool matches_blacklist_entry(const std::string &value, const std::string &entry)
{
    const std::string normalized_entry = normalize(entry);
    if (value == normalized_entry)
        return true;

    return value.size() > normalized_entry.size() + 2 &&
           value.compare(0, normalized_entry.size(), normalized_entry) == 0 &&
           value.compare(normalized_entry.size(), 2, " @") == 0;
}

template<size_t Size>
bool is_blacklisted(const std::string &name, const char *const (&blacklist)[Size])
{
    return std::any_of(std::begin(blacklist), std::end(blacklist), [&name](const char *entry) {
        return matches_blacklist_entry(name, entry);
    });
}

std::string display_name_without_vendor(const std::string &preset_name)
{
    std::string display_name = preset_name;
    const std::string normalized_name = normalize(preset_name);
    if (normalized_name.compare(0, g_vendor_prefix.size(), g_vendor_prefix) == 0)
    {
        const size_t display_start = preset_name.find_first_not_of(' ', g_vendor_prefix.size());
        if (display_start != std::string::npos)
            display_name = preset_name.substr(display_start);
    }

    const size_t variant_suffix = display_name.find(" @");
    if (variant_suffix != std::string::npos)
        display_name.erase(variant_suffix);

    return display_name;
}

} // anonymous namespace

CompatibilityResult check(const std::string &filament_type, const std::string &preset_name)
{
    const std::string type = normalize(filament_type);
    const std::string name = normalize(preset_name);

    if (is_blacklisted(name, g_mock_unsupported))
        return { CompatibilityLevel::Unsupported, display_name_without_vendor(preset_name) };

    if (has_material_token(type, "cf") || has_material_token(name, "cf") ||
        has_material_token(type, "gf") || has_material_token(name, "gf"))
        return { CompatibilityLevel::NotRecommended, "CF or GF based filaments" };

    if (is_blacklisted(name, g_mock_not_recommended))
        return { CompatibilityLevel::NotRecommended, display_name_without_vendor(preset_name) };

    return {};
}

}}} // namespace Slic3r::GUI::HighFlowCompat
