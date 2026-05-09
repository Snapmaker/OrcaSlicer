#pragma once

#include <string>

struct SliceOutputStats;

// Output slice statistics as JSON to stdout and optionally to a file.
void output_slice_statistics(const SliceOutputStats& stats,
                             const std::string& json_output_path,
                             const std::string& output_file_path);
