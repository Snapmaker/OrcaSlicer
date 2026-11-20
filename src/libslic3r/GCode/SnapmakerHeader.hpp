#pragma once

#include <cstdio>
#include <string_view>

namespace Slic3r::SnapmakerHeader {

inline constexpr char LINE_COUNT_PREFIX[] = ";Header Start\n;file_total_lines: ";
inline constexpr char START[] = ";Header Start\n;file_total_lines: 00000000000000000000\n;header_type: 3dp\n";
inline constexpr int LINE_COUNT_WIDTH = 20;

inline bool is_snapmaker_2(std::string_view printer_model)
{
    constexpr std::string_view models[] = {"Snapmaker A250", "Snapmaker A350"};
    for (const std::string_view model : models) {
        if (printer_model == model ||
            (printer_model.size() > model.size() && printer_model.substr(0, model.size()) == model &&
             printer_model[model.size()] == ' '))
            return true;
    }
    return false;
}

inline bool update_line_count(std::FILE* file, std::size_t line_count)
{
    // Keep the field width unchanged so the viewer's final line offsets remain valid.
    char value[LINE_COUNT_WIDTH + 1];
    const int length = std::snprintf(value, sizeof(value), "%020zu", line_count);
    return length == LINE_COUNT_WIDTH && std::fseek(file, sizeof(LINE_COUNT_PREFIX) - 1, SEEK_SET) == 0 &&
           std::fwrite(value, 1, LINE_COUNT_WIDTH, file) == LINE_COUNT_WIDTH && std::fflush(file) == 0;
}

} // namespace Slic3r::SnapmakerHeader
