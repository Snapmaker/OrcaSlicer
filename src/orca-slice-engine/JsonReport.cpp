#include "JsonReport.hpp"
#include "Utils.hpp"

#include <cmath>
#include <fstream>
#include <iostream>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "Types.hpp"

using ordered_json = nlohmann::ordered_json;

// Keep base64_encode — nlohmann doesn't provide base64
static const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char* input, size_t input_len) {
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
    if (valb > 0)
        result.push_back(BASE64_CHARS[((val << (6 - valb)) & 0x3F)]);
    while (result.size() % 4)
        result.push_back('=');
    return result;
}

// Round to 2 decimal places — matches the original std::setprecision(2) output
static double round2(double v) {
    if (!std::isfinite(v)) return 0.0;
    return std::round(v * 100.0) / 100.0;
}

static ordered_json issue_to_json(const Issue& issue) {
    ordered_json j;
    j["level"]       = issue.level;
    j["plate_id"]    = issue.plate_id;
    j["object_name"] = issue.object_name;
    j["z_height"]    = issue.z_height;
    j["code"]        = issue.code;
    j["message"]     = issue.message;
    if (!issue.suggestion.empty())
        j["suggestion"] = issue.suggestion;
    return j;
}

void output_slice_statistics(const SliceOutputStats& stats,
                             const std::string& json_output_path,
                             const std::string& output_file_path)
{
    ordered_json root;

    // ---- Aggregate totals for print_info_total ----
    double total_print_time = 0;
    double total_weight = 0;
    std::vector<ordered_json> total_filaments;
    {
        // Deduplicate by (type, color), summing used_g across plates
        struct AggFilament { std::string type; std::string color; double used_g = 0; };
        std::vector<AggFilament> agg;
        for (const auto& plate : stats.plates) {
            if (plate.success) {
                total_print_time += plate.print_time;
                total_weight += plate.total_filament_g;
                for (const auto& detail : plate.filament_details) {
                    auto it = std::find_if(agg.begin(), agg.end(),
                        [&](const AggFilament& a) { return a.type == detail.type && a.color == detail.color; });
                    if (it != agg.end())
                        it->used_g += detail.used_g;
                    else
                        agg.push_back({detail.type, detail.color, detail.used_g});
                }
            }
        }
        for (const auto& a : agg) {
            ordered_json fj;
            fj["type"]   = a.type;
            fj["color"]  = a.color;
            fj["used_g"] = round2(a.used_g);
            total_filaments.push_back(std::move(fj));
        }
    }

    root["success"] = stats.success;

    ordered_json print_info;
    print_info["output_file"]          = stats.success ? output_file_path : "";
    print_info["print_time_seconds"]   = round2(total_print_time);
    print_info["print_time_formatted"] = format_time_hhmmss(static_cast<float>(total_print_time));
    print_info["total_weight_g"]       = round2(total_weight);
    print_info["plate_count"]          = static_cast<int>(stats.plates.size());
    print_info["filaments"]            = total_filaments;
    root["print_info_total"] = std::move(print_info);

    // Global issues
    ordered_json global_issues = ordered_json::array();
    for (const auto& issue : stats.issues)
        global_issues.push_back(issue_to_json(issue));
    root["issues"] = std::move(global_issues);

    // Plates
    ordered_json plates_json = ordered_json::array();
    for (const auto& plate : stats.plates) {
        ordered_json pj;
        pj["plate_id"] = plate.plate_id;
        pj["success"]  = plate.success;

        if (!plate.issues.empty()) {
            ordered_json pi = ordered_json::array();
            for (const auto& iss : plate.issues)
                pi.push_back(issue_to_json(iss));
            pj["issues"] = std::move(pi);
        }

        if (plate.success) {
            pj["print_time_seconds"]   = round2(plate.print_time);
            pj["print_time_formatted"] = format_time_hhmmss(plate.print_time);
            pj["total_weight_g"]       = round2(plate.total_filament_g);

            ordered_json fils = ordered_json::array();
            for (const auto& detail : plate.filament_details) {
                ordered_json fdj;
                fdj["filament_id"] = detail.filament_id;
                fdj["type"]        = detail.type;
                fdj["color"]       = detail.color;
                fdj["used_g"]      = round2(detail.used_g);
                fils.push_back(std::move(fdj));
            }
            pj["filaments"]              = std::move(fils);
            pj["model_thumbnail"]        = plate.model_thumbnail;
            pj["long_retraction_when_cut"] = plate.long_retraction_when_cut;
        }

        plates_json.push_back(std::move(pj));
    }
    root["plates"] = std::move(plates_json);

    std::string json_str = root.dump(2);

    std::cout << "\n=== SLICE STATISTICS (JSON) ===" << std::endl;
    std::cout << json_str << std::endl;
    std::cout << "=== END STATISTICS ===" << std::endl;

    if (!json_output_path.empty()) {
        std::ofstream ofs(json_output_path);
        if (ofs.is_open()) {
            ofs << json_str;
            BOOST_LOG_TRIVIAL(info) << "Statistics written to: " << json_output_path;
        } else {
            BOOST_LOG_TRIVIAL(warning) << "Failed to write statistics to: " << json_output_path;
        }
    }
}
