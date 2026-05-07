#pragma once

#include <sstream>
#include <string>

#include "Types.hpp"

// Escape special characters for JSON string values
std::string json_escape(const std::string& s);

// Encode binary data as base64
std::string base64_encode(const unsigned char* input, size_t input_len);

// Serialize a single Issue to JSON
void write_issue_json(std::ostringstream& json, const Issue& issue, const std::string& indent);

// Output slice statistics as JSON to stdout and optionally to a file
void output_slice_statistics(const SliceOutputStats& stats, const std::string& json_output_path, const std::string& output_file_path);
