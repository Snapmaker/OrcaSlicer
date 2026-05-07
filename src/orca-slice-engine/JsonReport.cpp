#include "JsonReport.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>

#include <boost/log/trivial.hpp>

std::string json_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

static const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* input, size_t input_len) {
    std::string result;
    result.reserve((input_len + 2) / 3 * 4);

    int val = 0;
    int valb = 0;
    for (size_t i = 0; i < input_len; ++i) {
        val = (val << 8) + input[i];
        valb += 8;
        while (valb >= 6) {
            valb -= 6;
            result.push_back(BASE64_CHARS[(val >> valb) & 0x3F]);
        }
    }
    if (valb > 0) {
        result.push_back(BASE64_CHARS[((val << (6 - valb)) & 0x3F)]);
    }
    while (result.size() % 4) {
        result.push_back('=');
    }
    return result;
}

void write_issue_json(std::ostringstream& json, const Issue& issue, const std::string& indent) {
    json << indent << "{\n";
    json << indent << "  \"level\": \"" << json_escape(issue.level) << "\",\n";
    json << indent << "  \"plate_id\": " << issue.plate_id << ",\n";
    json << indent << "  \"object_name\": \"" << json_escape(issue.object_name) << "\",\n";
    json << indent << "  \"z_height\": " << issue.z_height << ",\n";
    json << indent << "  \"code\": \"" << json_escape(issue.code) << "\",\n";
    json << indent << "  \"message\": \"" << json_escape(issue.message) << "\"\n";
    json << indent << "}";
}

void output_slice_statistics(const SliceOutputStats& stats, const std::string& json_output_path, const std::string& output_file_path) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);

    // ---- Aggregate totals for print_info_total ----
    double total_print_time = 0;
    double total_weight = 0;
    int plate_count = static_cast<int>(stats.plates.size());
    // Aggregate filament totals by (type, color), summing used_g across all plates.
    // Use a simple vector and linear search since there are only a handful of filaments.
    struct AggregatedFilament { std::string type; std::string color; double used_g = 0; };
    std::vector<AggregatedFilament> total_filaments;

    for (const auto& plate : stats.plates) {
        if (plate.success) {
            total_print_time += plate.print_time;
            total_weight += plate.total_filament_g;
            for (const auto& detail : plate.filament_details) {
                auto it = std::find_if(total_filaments.begin(), total_filaments.end(),
                    [&](const AggregatedFilament& a) { return a.type == detail.type && a.color == detail.color; });
                if (it != total_filaments.end())
                    it->used_g += detail.used_g;
                else
                    total_filaments.push_back({detail.type, detail.color, detail.used_g});
            }
        }
    }

    json << "{\n";
    json << "  \"success\": " << (stats.success ? "true" : "false") << ",\n";

    // -- print_info_total --
    json << "  \"print_info_total\": {\n";
    json << "    \"output_file\": \"" << (stats.success ? json_escape(output_file_path) : "") << "\",\n";
    json << "    \"print_time_seconds\": " << total_print_time << ",\n";
    json << "    \"print_time_formatted\": \"" << format_time_hhmmss(static_cast<float>(total_print_time)) << "\",\n";
    json << "    \"total_weight_g\": " << total_weight << ",\n";
    json << "    \"plate_count\": " << plate_count << ",\n";
    json << "    \"filaments\": [\n";
    for (size_t fi = 0; fi < total_filaments.size(); ++fi) {
        json << "      {\n";
        json << "        \"type\": \"" << json_escape(total_filaments[fi].type) << "\",\n";
        json << "        \"color\": \"" << json_escape(total_filaments[fi].color) << "\",\n";
        json << "        \"used_g\": " << total_filaments[fi].used_g << "\n";
        json << "      }";
        if (fi < total_filaments.size() - 1) json << ",";
        json << "\n";
    }
    json << "    ]\n";
    json << "  },\n";

    // -- Global issues --
    json << "  \"issues\": [\n";
    for (size_t i = 0; i < stats.issues.size(); ++i) {
        write_issue_json(json, stats.issues[i], "    ");
        if (i < stats.issues.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ],\n";

    // -- Plates --
    json << "  \"plates\": [\n";
    for (size_t i = 0; i < stats.plates.size(); ++i) {
        const auto& plate = stats.plates[i];
        json << "    {\n";
        json << "      \"plate_id\": " << plate.plate_id << ",\n";
        json << "      \"success\": " << (plate.success ? "true" : "false");

        // Per-plate issues (only when non-empty)
        if (!plate.issues.empty()) {
            json << ",\n";
            json << "      \"issues\": [\n";
            for (size_t j = 0; j < plate.issues.size(); ++j) {
                write_issue_json(json, plate.issues[j], "        ");
                if (j < plate.issues.size() - 1) json << ",";
                json << "\n";
            }
            json << "      ]";
        }

        if (plate.success) {
            json << ",\n";
            json << "      \"print_time_seconds\": " << plate.print_time << ",\n";
            json << "      \"print_time_formatted\": \"" << format_time_hhmmss(plate.print_time) << "\",\n";
            json << "      \"total_weight_g\": " << plate.total_filament_g << ",\n";
            json << "      \"filaments\": [\n";
            for (size_t j = 0; j < plate.filament_details.size(); ++j) {
                const auto& detail = plate.filament_details[j];
                json << "        {\n";
                json << "          \"filament_id\": " << detail.filament_id << ",\n";
                json << "          \"type\": \"" << json_escape(detail.type) << "\",\n";
                json << "          \"color\": \"" << json_escape(detail.color) << "\",\n";
                json << "          \"used_g\": " << detail.used_g << "\n";
                json << "        }";
                if (j < plate.filament_details.size() - 1) json << ",";
                json << "\n";
            }
            json << "      ],\n";
            json << "      \"model_thumbnail\": \"" << json_escape(plate.model_thumbnail) << "\"\n";
        }

        json << "    }";
        if (i < stats.plates.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    std::cout << "\n=== SLICE STATISTICS (JSON) ===" << std::endl;
    std::cout << json.str() << std::endl;
    std::cout << "=== END STATISTICS ===" << std::endl;

    if (!json_output_path.empty()) {
        std::ofstream ofs(json_output_path);
        if (ofs.is_open()) {
            ofs << json.str();
            ofs.close();
            BOOST_LOG_TRIVIAL(info) << "Statistics written to: " << json_output_path;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "Failed to write statistics to: " << json_output_path;
        }
    }
}
