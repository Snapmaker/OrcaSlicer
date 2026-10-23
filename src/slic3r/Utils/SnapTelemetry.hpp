#pragma once

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
namespace SnapTelemetry {

inline std::string bool_value(bool value) { return value ? "true" : "false"; }

// Telemetry must never carry a file name or full path. Report the lower-case
// extension only; files without an extension are grouped as "unknown".
inline std::string file_type(const boost::filesystem::path& path)
{
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());

    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return extension.empty() ? "unknown" : extension;
}

inline std::string file_types(const std::vector<boost::filesystem::path>& paths)
{
    std::set<std::string> extensions;
    for (const auto& path : paths)
        extensions.insert(file_type(path));

    std::string result;
    for (const auto& extension : extensions) {
        if (!result.empty())
            result += ",";
        result += extension;
    }
    return result;
}

inline std::string unique_csv(std::vector<std::string> values)
{
    values.erase(std::remove_if(values.begin(), values.end(), [](const std::string& value) { return value.empty(); }),
                 values.end());
    std::set<std::string> unique_values(values.begin(), values.end());

    std::string result;
    for (const auto& value : unique_values) {
        if (!result.empty())
            result += ",";
        result += value;
    }
    return result;
}

inline std::string filament_type(const std::string& value)
{
    static const std::set<std::string> allowed = {
        "ABS", "ASA", "HIPS", "PA", "PA-CF", "PC", "PC-CF", "PEI", "PET", "PETG", "PETG-CF", "PLA", "PLA+", "PLA-CF", "PP", "PVA", "TPE", "TPU"
    };

    std::string normalized = value;
    normalized.erase(0, normalized.find_first_not_of(" \t\r\n"));
    normalized.erase(normalized.find_last_not_of(" \t\r\n") + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    return allowed.find(normalized) != allowed.end() ? normalized : "custom";
}

inline std::string filament_types(const std::vector<std::string>& values)
{
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    for (const auto& value : values) {
        if (!value.empty())
            normalized.push_back(filament_type(value));
    }
    return unique_csv(std::move(normalized));
}

inline std::string preset_type(bool is_system) { return is_system ? "system" : "custom"; }

inline std::string operation_result(bool success, bool cancelled = false)
{
    if (cancelled)
        return "cancelled";
    return success ? "success" : "failed";
}



inline std::string number(double value, int precision)
{
    if (!std::isfinite(value))
        return "0";

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    std::string result(buffer);
    const auto decimal_point = result.find_last_of('.');
    if (decimal_point != std::string::npos)
        result.erase(result.find_last_not_of('0') + 1, std::string::npos);
    if (!result.empty() && result.back() == '.')
        result.pop_back();
    return result;
}

inline std::string file_size_bytes(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    const auto size = boost::filesystem::file_size(path, ec);
    return ec ? "0" : std::to_string(size);
}

template<typename Clock>
std::string elapsed_ms(const std::chrono::time_point<Clock>& start, const std::chrono::time_point<Clock>& end)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

} // namespace SnapTelemetry
} // namespace Slic3r
