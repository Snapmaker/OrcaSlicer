#pragma once

#include <cmath>
#include <string>
#include <utility>

#include <boost/filesystem.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Print.hpp"

#include "Types.hpp"

// Emit a structured log line to both boost-log and stderr.
void log_plate_message(const char* stage, const char* level, int plate, const std::string& msg);

// Extract human-readable object name and config hint from a StringObjectException
std::pair<std::string, std::string> format_exception_context(const Slic3r::StringObjectException& ex);

// Print CLI usage information
void print_usage(const char* program_name);

// Simple progress callback for cloud environment
void default_status_callback(const Slic3r::PrintBase::SlicingStatus& status);

// Format time in seconds as HH:MM:SS
std::string format_time_hhmmss(float seconds);

// Generate output filename based on parameters
std::string generate_output_path(
    const std::string& input_file,
    const std::string& output_base,
    int plate_id,
    OutputFormat format,
    bool single_plate);

// Compute column count for plate grid layout (matches GUI's PartPlate.hpp logic)
inline int compute_column_count(int count) {
    float value = sqrt((float)count);
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
}
