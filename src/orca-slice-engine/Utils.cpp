#include "Utils.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <boost/log/trivial.hpp>

#include "libslic3r/libslic3r.h"

void log_plate_message(const char* stage, const char* level, int plate, const std::string& msg)
{
    std::string full = std::string(stage) + " " + level + ": Plate " + std::to_string(plate) + ": " + msg;
    if (std::strcmp(level, "ERROR") == 0)
        BOOST_LOG_TRIVIAL(error) << full;
    else if (std::strcmp(level, "WARNING") == 0)
        BOOST_LOG_TRIVIAL(warning) << full;
    else
        BOOST_LOG_TRIVIAL(info) << full;

    if (std::strcmp(level, "TIP") != 0)
        std::cerr << "[" << level << "] Plate " << plate << ": " << msg << std::endl;
}

std::pair<std::string, std::string> format_exception_context(const Slic3r::StringObjectException& ex)
{
    std::string obj_name;
    if (ex.object) {
        auto mo = dynamic_cast<Slic3r::ModelObject const*>(ex.object);
        if (!mo) {
            if (auto po = dynamic_cast<Slic3r::PrintObjectBase const*>(ex.object))
                mo = po->model_object();
        }
        if (mo) obj_name = " [object: " + mo->name + "]";
    }
    std::string opt_hint = ex.opt_key.empty() ? "" : " (config: " + ex.opt_key + ")";
    return {obj_name, opt_hint};
}

void print_usage(const char* program_name) {
    std::cout << "OrcaSlicer Cloud Slicing Engine v" << SLIC3R_VERSION << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << program_name << " input.3mf [OPTIONS]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -o, --output <file>    Output file path (without extension)" << std::endl;
    std::cout << "                         Single plate: outputs {file}.gcode or {file}.gcode.3mf" << std::endl;
    std::cout << "                         All plates: outputs {file}.gcode.3mf" << std::endl;
    std::cout << "  -p, --plate <id>       Plate number to slice (1, 2, 3...)" << std::endl;
    std::cout << "                         Omit or \"all\" for all plates (default: all)" << std::endl;
    std::cout << "  -f, --format <fmt>     Output format: gcode | gcode.3mf (default: gcode.3mf)" << std::endl;
    std::cout << "                         Note: All plates always use gcode.3mf" << std::endl;
    std::cout << "  -r, --resources <dir>  Resources directory containing printer profiles" << std::endl;
    std::cout << "  -d, --data-dir <dir>   System presets directory (default: <resources>/profiles)" << std::endl;
    std::cout << "  -j, --json [file]      Output slice statistics as JSON to specified file" << std::endl;
    std::cout << "                         If not specified, JSON is auto-saved next to the output" << std::endl;
    std::cout << "  --log                  Enable log file output (auto-saved next to the output)" << std::endl;
    std::cout << "  --log-file <file>      Specify log file path (implies --log)" << std::endl;
    std::cout << "  -v, --verbose          Enable verbose logging" << std::endl;
    std::cout << "  -h, --help             Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Exit codes:" << std::endl;
    std::cout << "  0  Success" << std::endl;
    std::cout << "  1  Invalid arguments" << std::endl;
    std::cout << "  2  Input file not found" << std::endl;
    std::cout << "  3  3MF load error" << std::endl;
    std::cout << "  4  Slicing error" << std::endl;
    std::cout << "  5  G-code export error" << std::endl;
    std::cout << "  6  Pre-processing validation error (e.g. collision, invalid config)" << std::endl;
    std::cout << "  7  Post-processing warning (toolpath outside print volume)" << std::endl;
    std::cout << std::endl;
    std::cout << "Output:" << std::endl;
    std::cout << "  On success, outputs JSON with slicing statistics including:" << std::endl;
    std::cout << "    - success: true/false" << std::endl;
    std::cout << "    - plates[].time: total, prepare, print time (seconds and formatted)" << std::endl;
    std::cout << "    - plates[].filament: total/model filament (m, g), cost, per-extruder usage" << std::endl;
    std::cout << "    - plates[].gcode_file: path to output file" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " model.3mf                        # All plates -> model.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1                   # Plate 1 -> model-p1.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -f gcode          # Plate 1 -> model-p1.gcode (plain text)" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -o output         # Plate 1 -> output.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -o result              # All plates -> result.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -j stats.json          # Output statistics to stats.json" << std::endl;
}

void default_status_callback(const Slic3r::PrintBase::SlicingStatus& status) {
    if (status.percent >= 0) {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    } else {
        std::cout << "[Status] " << status.text << std::endl;
    }
}

std::string format_time_hhmmss(float seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return "00:00:00";
    int total_secs = static_cast<int>(seconds);
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
    return std::string(buf);
}

std::string generate_output_path(
    const std::string& input_file,
    const std::string& output_base,
    int plate_id,
    OutputFormat format,
    bool single_plate)
{
    boost::filesystem::path input_path(input_file);
    std::string base_name;

    if (!output_base.empty()) {
        base_name = output_base;
    } else {
        boost::filesystem::path parent = input_path.parent_path();
        if (parent.empty())
            parent = boost::filesystem::current_path();
        base_name = (parent / input_path.stem()).string();
    }

    std::string extension = (format == OutputFormat::GCODE_3MF) ? ".gcode.3mf" : ".gcode";

    std::string path;
    if (single_plate) {
        if (output_base.empty()) {
            path = base_name + "-p" + std::to_string(plate_id) + extension;
        } else {
            path = base_name + extension;
        }
    } else {
        path = base_name + ".gcode.3mf";
    }

    // Prevent multi-process collision: append unique suffix if output file already exists
    if (boost::filesystem::exists(path)) {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        boost::filesystem::path p(path);
        path = (p.parent_path() / (p.stem().string() + "_" + std::to_string(ts))).string() + p.extension().string();
    }

    return path;
}
