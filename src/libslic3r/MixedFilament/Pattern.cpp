#include "Internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace Slic3r { namespace MixedFilamentInternal {

std::vector<std::string> tokenize_pattern_group(const std::string& group)
{
    std::vector<std::string> tokens;
    if (group.empty())
        return tokens;

    for (size_t i = 0; i < group.size(); ++i) {
        char c = group[i];
        if (c >= '1' && c <= '9') {
            tokens.emplace_back(1, c);
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < group.size() && group[j] >= '0' && group[j] <= '9')
                ++j;
            if (j > i + 1 && j < group.size() && group[j] == ']') {
                tokens.emplace_back(group.substr(i + 1, j - i - 1));
                i = j;
            }
        }
    }
    return tokens;
}

bool is_pattern_separator(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_' || c == '|' || c == ':' || c == ';' || c == ',';
}

bool decode_pattern_step(char c, char& out)
{
    if (c >= '1' && c <= '9') {
        out = c;
        return true;
    }
    switch (std::tolower(static_cast<unsigned char>(c))) {
    case 'a': out = '1'; return true;
    case 'b': out = '2'; return true;
    default: return false;
    }
}

std::vector<std::string> split_manual_pattern_groups(const std::string& pattern)
{
    std::vector<std::string> groups;
    if (pattern.empty())
        return groups;

    std::string current;
    for (const char c : pattern) {
        if (c == ',') {
            if (!current.empty()) {
                groups.emplace_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        groups.emplace_back(std::move(current));
    return groups;
}

std::string flatten_manual_pattern_groups(const std::string& pattern)
{
    std::string flattened;
    flattened.reserve(pattern.size());
    for (const char c : pattern)
        if (c != ',')
            flattened.push_back(c);
    return flattened;
}

int mix_percent_from_normalized_pattern(const std::string& pattern)
{
    const std::vector<std::string> groups = split_manual_pattern_groups(pattern);
    if (groups.empty())
        return 50;

    double blend_b = 0.0;
    for (const std::string& group : groups) {
        if (group.empty())
            continue;
        const std::vector<std::string> tokens = tokenize_pattern_group(group);
        if (tokens.empty())
            continue;
        const int count_b = int(std::count(tokens.begin(), tokens.end(), "2"));
        blend_b += double(count_b) / double(tokens.size());
    }
    return clamp_int(int(std::lround(100.0 * blend_b / double(groups.size()))), 0, 100);
}

unsigned int physical_filament_from_legacy_pattern_token(char token, const MixedFilamentLegacyPair& pair)
{
    if (token == '1')
        return pair.component_a.id;
    if (token == '2')
        return pair.component_b.id;
    if (token >= '3' && token <= '9')
        return unsigned(token - '0');
    return 0;
}

std::string legacy_manual_pattern_from_mixed_filament_pattern(const MixedFilamentManualPattern& pattern, const MixedFilamentLegacyPair& pair)
{
    std::string result;
    for (const auto& group : pattern.groups) {
        std::string encoded;
        for (const auto& ref : group) {
            if (ref.id == pair.component_a.id)
                encoded += "1";
            else if (ref.id == pair.component_b.id)
                encoded += "2";
            else if (ref.id >= 3 && ref.id <= 9)
                encoded += char('0' + ref.id);
            else if (ref.id > 9)
                encoded += "[" + std::to_string(ref.id) + "]";
        }
        if (!encoded.empty()) {
            if (!result.empty())
                result += ',';
            result += encoded;
        }
    }
    return result;
}

}} // namespace Slic3r::MixedFilamentInternal

namespace Slic3r {
using namespace MixedFilamentInternal;
std::string MixedFilamentManager::normalize_manual_pattern(const std::string& pattern)
{
    if (pattern.empty())
        return {};

    std::string normalized;
    normalized.reserve(pattern.size());
    bool group_has_content = false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c >= '1' && c <= '9') {
            normalized.push_back(c);
            group_has_content = true;
        } else if (c == ',') {
            if (!group_has_content)
                return {};
            normalized.push_back(',');
            group_has_content = false;
        } else if (c == '[') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9')
                ++j;
            if (j == i + 1 || j >= pattern.size() || pattern[j] != ']')
                return {};

            std::string num_str = pattern.substr(i + 1, j - i - 1);
            if (num_str.size() > 2)
                return {};
            if (num_str.size() > 1 && num_str[0] == '0')
                return {};
            if (num_str == "0")
                return {};

            // Compressing [1]→1 and [2]→2 is safe under the cycle-mode
            // invariant (component_a≡1, component_b≡2) — the symbolic
            // tokens are identity mappings, so no information is lost.
            if (num_str.size() == 1) {
                normalized.push_back(num_str[0]);
            } else {
                normalized.push_back('[');
                normalized.append(num_str);
                normalized.push_back(']');
            }
            group_has_content = true;
            i                 = j;
        } else if (c == ']' || c == '0') {
            return {};
        } else {
            return {};
        }
    }

    if (!group_has_content)
        return {};

    return normalized;
}
int MixedFilamentManager::mix_percent_from_manual_pattern(const std::string& pattern)
{
    return mix_percent_from_normalized_pattern(normalize_manual_pattern(pattern));
}
} // namespace Slic3r
