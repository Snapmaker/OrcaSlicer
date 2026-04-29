#include "JsonReport.hpp"
#include "Utils.hpp"

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

void output_slice_statistics(const SliceOutputStats& stats, const std::string& json_output_path) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);

    json << "{\n";
    json << "  \"success\": " << (stats.success ? "true" : "false") << ",\n";

    if (!stats.success && !stats.error_message.empty()) {
        json << "  \"error\": \"" << json_escape(stats.error_message) << "\",\n";
    }

    // Global issues array
    json << "  \"issues\": [\n";
    for (size_t i = 0; i < stats.issues.size(); ++i) {
        write_issue_json(json, stats.issues[i], "    ");
        if (i < stats.issues.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ],\n";

    json << "  \"plates\": [\n";
    for (size_t i = 0; i < stats.plates.size(); ++i) {
        const SliceOutputStats::PlateStats& plate = stats.plates[i];
        json << "    {\n";
        json << "      \"plate_id\": " << plate.plate_id << ",\n";
        json << "      \"success\": " << (plate.success ? "true" : "false") << ",\n";

        // Per-plate issues array
        json << "      \"issues\": [\n";
        for (size_t j = 0; j < plate.issues.size(); ++j) {
            write_issue_json(json, plate.issues[j], "        ");
            if (j < plate.issues.size() - 1) json << ",";
            json << "\n";
        }
        json << "      ]";

        if (plate.success) {
            json << ",\n";
            json << "      \"gcode_file\": \"" << json_escape(plate.gcode_file) << "\",\n";
            json << "      \"print_time_seconds\": " << plate.print_time << ",\n";
            json << "      \"print_time_formatted\": \"" << format_time_hhmmss(plate.print_time) << "\",\n";
            json << "      \"total_weight_g\": " << plate.total_filament_g << ",\n";
            json << "      \"nozzle_diameters\": [";
            for (size_t j = 0; j < plate.nozzle_diameters.size(); ++j) {
                if (j > 0) json << ", ";
                json << plate.nozzle_diameters[j];
            }
            json << "],\n";
            json << "      \"plate_count\": " << plate.plate_count << ",\n";
            json << "      \"filaments\": [\n";
            for (size_t j = 0; j < plate.filament_details.size(); ++j) {
                const auto& detail = plate.filament_details[j];
                json << "        {\n";
                json << "          \"extruder_id\": " << detail.extruder_id << ",\n";
                json << "          \"type\": \"" << json_escape(detail.type) << "\",\n";
                json << "          \"color\": \"" << json_escape(detail.color) << "\",\n";
                json << "          \"used_g\": " << detail.used_g << "\n";
                json << "        }";
                if (j < plate.filament_details.size() - 1) json << ",";
                json << "\n";
            }
            json << "      ],\n";
            json << "      \"model_thumbnail\": \"" << json_escape(plate.model_thumbnail) << "\"\n";
        } else {
            json << ",\n";
            json << "      \"plate_count\": " << plate.plate_count << "\n";
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
