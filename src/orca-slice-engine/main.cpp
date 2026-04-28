/**
 * OrcaSlicer Cloud Slicing Engine
 * Minimal headless slicer for cloud deployment
 *
 * Usage: orca-slice-engine input.3mf [OPTIONS]
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>
#include <vector>
#include <map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <fstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

// libslic3r headers
#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;

// RAII guard that removes a list of temp files on destruction
struct TempFileGuard {
    std::vector<std::string>& mFiles;
    explicit TempFileGuard(std::vector<std::string>& f) : mFiles(f) {}
    ~TempFileGuard() { cleanup(); }
    void cleanup() {
        for (const auto& file : mFiles) {
            try {
                if (boost::filesystem::exists(file))
                    boost::filesystem::remove(file);
            } catch (...) {}
        }
    }
    // Prevent copy
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

// Emit a structured log line to both boost-log and stderr.
// stage:   "[Pre-processing]" or "[Post-processing]" — preserved so callers can distinguish origin
// level:   "ERROR" | "WARNING" | "TIP"
// plate:   current plate id
// msg:     message body
static void log_plate_message(const char* stage, const char* level, int plate, const std::string& msg)
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

// Extract human-readable object name and config hint from a StringObjectException
static std::pair<std::string, std::string> format_exception_context(const StringObjectException& ex)
{
    std::string obj_name;
    if (ex.object) {
        auto mo = dynamic_cast<ModelObject const*>(ex.object);
        if (!mo) {
            if (auto po = dynamic_cast<PrintObjectBase const*>(ex.object))
                mo = po->model_object();
        }
        if (mo) obj_name = " [object: " + mo->name + "]";
    }
    std::string opt_hint = ex.opt_key.empty() ? "" : " (config: " + ex.opt_key + ")";
    return {obj_name, opt_hint};
}

// Exit codes
static const int EXIT_OK = 0;
static const int EXIT_INVALID_ARGS = 1;
static const int EXIT_FILE_NOT_FOUND = 2;
static const int EXIT_LOAD_ERROR = 3;
static const int EXIT_SLICING_ERROR = 4;
static const int EXIT_EXPORT_ERROR = 5;
static const int EXIT_VALIDATION_ERROR = 6;
static const int EXIT_POSTPROCESS_WARNING = 7;

// Output format enum
enum class OutputFormat {
    GCODE,
    GCODE_3MF
};

// Output statistics structure for JSON export
struct SliceOutputStats {
    // Filament detail for each extruder
    struct FilamentDetail {
        int extruder_id;
        std::string type;        // Filament type (e.g., "PLA", "ABS")
        std::string color;       // Filament color (e.g., "#FF0000")
        double used_g;           // Used weight in grams
    };
    
    // Per-plate statistics
    struct PlateStats {
        int plate_id;
        bool success;
        std::string gcode_file;
        
        // Time statistics (in seconds)
        float total_time;
        float prepare_time;
        float print_time;         // Actual print time (excluding prepare time)
        
        // Filament statistics
        double total_filament_m;      // Total filament in meters
        double total_filament_g;      // Total filament in grams
        double model_filament_m;      // Model filament in meters
        double model_filament_g;      // Model filament in grams
        double total_cost;            // Total cost
        
        // Per-extruder filament usage
        std::map<int, double> filament_used_m;   // Per extruder: meters
        std::map<int, double> filament_used_g;   // Per extruder: grams
        
        // Additional info
        bool support_used;
        bool toolpath_outside;
        
        // New fields for enhanced JSON output
        std::vector<double> nozzle_diameters;    // Nozzle diameter for each extruder
        int plate_count;                         // Total number of plates
        std::vector<FilamentDetail> filament_details;  // Detailed filament info per extruder
        std::string model_thumbnail;             // Model thumbnail (base64 encoded PNG)
    };
    
    bool success;
    std::string error_message;
    std::vector<PlateStats> plates;
};

// Compute column count for plate grid layout (matches GUI's PartPlate.hpp logic)
inline int compute_column_count(int count) {
    float value = sqrt((float)count);
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
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
    std::cout << "  -j, --json <file>      Output slice statistics as JSON to specified file" << std::endl;
    std::cout << "                         If not specified, JSON is printed to stdout" << std::endl;
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

void default_status_callback(const PrintBase::SlicingStatus& status) {
    // Simple progress output for cloud environment
    if (status.percent >= 0) {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    } else {
        std::cout << "[Status] " << status.text << std::endl;
    }
}

// Helper function to format time as HH:MM:SS
std::string format_time_hhmmss(float seconds) {
    if (seconds < 0) return "00:00:00";
    int total_secs = static_cast<int>(seconds);
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
    return std::string(buf);
}

// Helper function to escape JSON strings
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

// Helper function to encode binary data to base64
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

// Output slice statistics as JSON to stdout or file (new simplified format)
void output_slice_statistics(const SliceOutputStats& stats, const std::string& json_output_path) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    
    json << "{\n";
    json << "  \"success\": " << (stats.success ? "true" : "false") << ",\n";
    
    if (!stats.success && !stats.error_message.empty()) {
        json << "  \"error\": \"" << json_escape(stats.error_message) << "\",\n";
    }
    
    json << "  \"plates\": [\n";
    for (size_t i = 0; i < stats.plates.size(); ++i) {
        const SliceOutputStats::PlateStats& plate = stats.plates[i];
        json << "    {\n";
        json << "      \"plate_id\": " << plate.plate_id << ",\n";
        json << "      \"success\": " << (plate.success ? "true" : "false") << ",\n";
        
        if (plate.success) {
            json << "      \"gcode_file\": \"" << json_escape(plate.gcode_file) << "\",\n";
            
            // Print time in seconds and formatted
            json << "      \"print_time_seconds\": " << plate.print_time << ",\n";
            json << "      \"print_time_formatted\": \"" << format_time_hhmmss(plate.print_time) << "\",\n";
            
            // Total consumed weight in grams
            json << "      \"total_weight_g\": " << plate.total_filament_g << ",\n";
            
            // Nozzle diameters
            json << "      \"nozzle_diameters\": [";
            for (size_t j = 0; j < plate.nozzle_diameters.size(); ++j) {
                if (j > 0) json << ", ";
                json << plate.nozzle_diameters[j];
            }
            json << "],\n";
            
            // Plate count (total number of plates in the job)
            json << "      \"plate_count\": " << plate.plate_count << ",\n";
            
            // Filament details (type, color, and consumed weight per extruder)
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
            
            // Model thumbnail (base64 encoded PNG, or empty if not available)
            json << "      \"model_thumbnail\": \"" << json_escape(plate.model_thumbnail) << "\"\n";
        }
        
        json << "    }";
        if (i < stats.plates.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    
    // Output to stdout
    std::cout << "\n=== SLICE STATISTICS (JSON) ===" << std::endl;
    std::cout << json.str() << std::endl;
    std::cout << "=== END STATISTICS ===" << std::endl;
    
    // Optionally write to file
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

// Generate output filename based on parameters
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
        base_name = parent.string() + "/" + input_path.stem().string();
    }

    std::string extension = (format == OutputFormat::GCODE_3MF) ? ".gcode.3mf" : ".gcode";

    if (single_plate) {
        // Add plate suffix for single plate if using default name
        if (output_base.empty()) {
            return base_name + "-p" + std::to_string(plate_id) + extension;
        }
        return base_name + extension;
    } else {
        // All plates always use .gcode.3mf
        return base_name + ".gcode.3mf";
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    boost::nowide::args a(argc, argv);

    // Default values
    std::string input_file;
    std::string output_base;
    std::string resources_dir;
    std::string json_output_path;
    bool verbose = false;
    int plate_id = 0;  // 0 = all plates
    OutputFormat format = OutputFormat::GCODE_3MF;  // Default to gcode.3mf
    bool format_specified = false;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return EXIT_OK;
        }
        else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_base = argv[++i];
        }
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc) {
            resources_dir = argv[++i];
        }
        else if ((arg == "-j" || arg == "--json") && i + 1 < argc) {
            json_output_path = argv[++i];
        }
        else if ((arg == "-p" || arg == "--plate") && i + 1 < argc) {
            std::string plate_arg = argv[++i];
            if (plate_arg == "all" || plate_arg == "0") {
                plate_id = 0;  // All plates
            } else {
                try {
                    plate_id = std::stoi(plate_arg);
                    if (plate_id < 1) {
                        std::cerr << "Error: Plate ID must be 1 or greater (or 'all')." << std::endl;
                        return EXIT_INVALID_ARGS;
                    }
                } catch (...) {
                    std::cerr << "Error: Invalid plate ID: " << plate_arg << std::endl;
                    return EXIT_INVALID_ARGS;
                }
            }
        }
        else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            std::string fmt = argv[++i];
            format_specified = true;
            if (fmt == "gcode") {
                format = OutputFormat::GCODE;
            } else if (fmt == "gcode.3mf") {
                format = OutputFormat::GCODE_3MF;
            } else {
                std::cerr << "Error: Unknown format: " << fmt << " (use 'gcode' or 'gcode.3mf')" << std::endl;
                return EXIT_INVALID_ARGS;
            }
        }
        else if (arg[0] != '-') {
            if (input_file.empty()) {
                input_file = arg;
            } else {
                std::cerr << "Error: Multiple input files specified. Only one 3MF file is supported." << std::endl;
                return EXIT_INVALID_ARGS;
            }
        }
        else {
            std::cerr << "Error: Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return EXIT_INVALID_ARGS;
        }
    }

    // Validate input file
    if (input_file.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        print_usage(argv[0]);
        return EXIT_INVALID_ARGS;
    }

    if (!boost::filesystem::exists(input_file)) {
        std::cerr << "Error: Input file not found: " << input_file << std::endl;
        return EXIT_FILE_NOT_FOUND;
    }

    // Validate plate/format combination
    bool single_plate = (plate_id > 0);
    if (!single_plate && format_specified && format == OutputFormat::GCODE) {
        std::cerr << "Error: All-plate slicing requires gcode.3mf format." << std::endl;
        std::cerr << "Use: --format gcode.3mf or omit --format for automatic selection." << std::endl;
        return EXIT_INVALID_ARGS;
    }

    // Auto-select format for all plates
    if (!single_plate) {
        format = OutputFormat::GCODE_3MF;
    }

    // Setup logging
    boost::log::add_console_log(std::cout, boost::log::keywords::format = "[%Severity%] %Message%");
    boost::log::add_common_attributes();

    if (verbose) {
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);
    } else {
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);
    }

    BOOST_LOG_TRIVIAL(info) << "OrcaSlicer Cloud Engine v" << SLIC3R_VERSION;
    BOOST_LOG_TRIVIAL(info) << "Input file: " << input_file;
    BOOST_LOG_TRIVIAL(info) << "Plate: " << (single_plate ? std::to_string(plate_id) : "all");
    BOOST_LOG_TRIVIAL(info) << "Format: " << (format == OutputFormat::GCODE_3MF ? "gcode.3mf" : "gcode");

    // Setup resources directory
    if (!resources_dir.empty()) {
        set_resources_dir(resources_dir);
        BOOST_LOG_TRIVIAL(info) << "Resources directory: " << resources_dir;
    } else {
        // Try to find resources relative to executable
        boost::filesystem::path exe_path = boost::dll::program_location();
        boost::filesystem::path resource_path = exe_path.parent_path() / "resources";
        if (boost::filesystem::exists(resource_path)) {
            set_resources_dir(resource_path.string());
            BOOST_LOG_TRIVIAL(info) << "Resources directory: " << resource_path;
        } else {
            // Check environment variable
            const char* env_resources = std::getenv("ORCA_RESOURCES");
            if (env_resources) {
                set_resources_dir(env_resources);
                BOOST_LOG_TRIVIAL(info) << "Resources directory (from env): " << env_resources;
            } else {
                BOOST_LOG_TRIVIAL(warning) << "No resources directory specified. Using default preset loading.";
            }
        }
    }

    // Validate resources directory
    std::string validated_resources_dir = resources_dir;
    if (!validated_resources_dir.empty()) {
        if (!boost::filesystem::exists(validated_resources_dir)) {
            BOOST_LOG_TRIVIAL(warning) << "Resources directory does not exist: " << validated_resources_dir;
        } else {
            // Check for key resources subdirectories
            bool has_profiles = boost::filesystem::exists(validated_resources_dir + "/profiles");
            bool has_printers = boost::filesystem::exists(validated_resources_dir + "/printers");
            if (!has_profiles && !has_printers) {
                BOOST_LOG_TRIVIAL(warning) << "Resources directory may be incomplete (missing profiles/printers): " << validated_resources_dir;
            } else {
                BOOST_LOG_TRIVIAL(info) << "Resources directory validated: " << validated_resources_dir;
            }
        }
    }

    // Set temporary directory
    std::string temp_dir = boost::filesystem::temp_directory_path().string();
    set_temporary_dir(temp_dir);
    BOOST_LOG_TRIVIAL(debug) << "Temporary directory: " << temp_dir;

    // Track temporary files for cleanup — TempFileGuard ensures cleanup on any exit path
    std::vector<std::string> temp_files;
    TempFileGuard temp_guard(temp_files);

    // Load 3MF file with embedded configuration
    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

    Model model;
    DynamicPrintConfig config;
    ConfigSubstitutionContext config_substitutions(ForwardCompatibilitySubstitutionRule::Enable);

    PlateDataPtrs plate_data;
    std::vector<Preset*> project_presets;
    bool is_bbl_3mf = false;
    Semver file_version;

    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                           LoadStrategy::AddDefaultInstances | LoadStrategy::LoadAuxiliary;

    try {
        model = Model::read_from_file(
            input_file,
            &config,
            &config_substitutions,
            strategy,
            &plate_data,
            &project_presets,
            &is_bbl_3mf,
            &file_version,
            nullptr,  // Import3mfProgressFn
            nullptr,  // ImportstlProgressFn
            nullptr,  // BBLProject
            0         // plate_id (0 = all plates, filter later)
        );
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << e.what();
        return EXIT_LOAD_ERROR;
    }

    if (model.objects.empty()) {
        BOOST_LOG_TRIVIAL(error) << "No objects found in 3MF file";
        return EXIT_LOAD_ERROR;
    }

    BOOST_LOG_TRIVIAL(info) << "Loaded " << model.objects.size() << " object(s)";
    BOOST_LOG_TRIVIAL(info) << "3MF version: " << file_version.to_string();

    // Check plate availability
    if (plate_data.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No plate data in 3MF, treating as single plate";
        // Create a single plate data entry
        PlateData* pd = new PlateData();
        pd->plate_index = 1;
        plate_data.push_back(pd);
    }

    BOOST_LOG_TRIVIAL(info) << "Found " << plate_data.size() << " plate(s) in 3MF";

    // Validate requested plate exists
    if (single_plate) {
        bool plate_found = false;
        for (const auto& pd : plate_data) {
            if (pd->plate_index == plate_id) {
                plate_found = true;
                break;
            }
        }
        if (!plate_found) {
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " not found in 3MF file";
            std::cerr << "Available plates: ";
            for (size_t i = 0; i < plate_data.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << plate_data[i]->plate_index;
            }
            std::cerr << std::endl;
            return EXIT_INVALID_ARGS;
        }
    }

    // Determine output path
    std::string output_path = generate_output_path(input_file, output_base, plate_id, format, single_plate);
    BOOST_LOG_TRIVIAL(info) << "Output file: " << output_path;

    // Collect plates to process
    std::vector<int> plates_to_process;
    if (single_plate) {
        plates_to_process.push_back(plate_id);
    } else {
        for (const auto& pd : plate_data) {
            plates_to_process.push_back(pd->plate_index);
        }
    }

    // Structure to store slicing results for each plate
    struct PlateSliceResult {
        std::string gcode_path;
        GCodeProcessorResult gcode_result;
        double total_weight;
        bool support_used;
        bool has_postprocess_warning = false;
        // Additional statistics for JSON output
        double total_used_filament = 0.0;
        double total_cost = 0.0;
        std::map<size_t, double> filament_volumes;  // per extruder
    };
    std::map<int, PlateSliceResult> plate_results;
    
    // Global output statistics
    SliceOutputStats output_stats;
    output_stats.success = false;  // Will be set to true on successful completion

    // Process each plate
    for (int current_plate_id : plates_to_process) {
        BOOST_LOG_TRIVIAL(info) << "=== Processing plate " << current_plate_id << " ===";

        // Get instances for current plate from obj_inst_map
        // obj_inst_map format: key=object_id (from 3MF), value=pair<instance_id, identify_id>
        // The identify_id is stored in ModelInstance::loaded_id after 3MF load
        std::set<int> current_plate_identify_ids;
        for (const auto& pd : plate_data) {
            if (pd->plate_index == current_plate_id) {
                for (const auto& [object_id, inst_info] : pd->obj_inst_map) {
                    // inst_info.second is the identify_id
                    current_plate_identify_ids.insert(inst_info.second);
                }
                break;
            }
        }

        // Set printable state for all model instances
        // Only instances on current plate should be printable
        int instances_on_plate = 0;
        for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
            ModelObject* obj = model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
                ModelInstance* inst = obj->instances[inst_idx];
                bool on_current_plate = (current_plate_identify_ids.find(static_cast<int>(inst->loaded_id)) != current_plate_identify_ids.end());
                inst->printable = on_current_plate;
                inst->print_volume_state = on_current_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
                if (on_current_plate)
                    instances_on_plate++;
            }
        }

        BOOST_LOG_TRIVIAL(info) << "Filtered model: " << instances_on_plate
            << " instances on plate " << current_plate_id;

        // Skip empty plates (no instances to slice)
        if (instances_on_plate == 0) {
            BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate " << current_plate_id;
            continue;
        }

        // --- PRE-PROCESSING: Build volume check ---
        // Mirrors GUI's Snapmaker_Orca.cpp: BuildVolume + model.update_print_volume_state()
        // Detects instances that are partly outside the printable area or exceed height limit.
        if (config.has("printable_area") && config.has("printable_height")) {
            auto printable_area_opt = config.option<ConfigOptionPoints>("printable_area");
            double printable_height = config.opt_float("printable_height");
            if (printable_area_opt && !printable_area_opt->values.empty() && printable_height > 0) {
                BuildVolume build_volume(printable_area_opt->values, printable_height);
                model.update_print_volume_state(build_volume);

                bool has_partly_outside = false;
                for (ModelObject* obj : model.objects) {
                    for (ModelInstance* inst : obj->instances) {
                        if (!inst->printable) continue;
                        if (inst->print_volume_state == ModelInstancePVS_Partly_Outside) {
                            log_plate_message("[Pre-processing]", "ERROR", current_plate_id,
                                "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume.");
                            has_partly_outside = true;
                        }
                    }
                }
                if (has_partly_outside)
                    return EXIT_VALIDATION_ERROR;

                // Restore printable state — update_print_volume_state may have changed it
                for (ModelObject* obj : model.objects) {
                    for (ModelInstance* inst : obj->instances) {
                        bool on_plate = (current_plate_identify_ids.find(static_cast<int>(inst->loaded_id)) != current_plate_identify_ids.end());
                        inst->printable = on_plate;
                        if (!on_plate)
                            inst->print_volume_state = ModelInstancePVS_Fully_Outside;
                    }
                }
            }
        }

        // Create Print object for this plate
        Print print;
        print.set_status_callback(default_status_callback);

        // Set plate index and origin for multi-plate support
        // Calculate plate origin based on printer bed size and plate position
        // This matches GUI's PartPlateList::compute_origin() logic
        // CRITICAL: current_plate_id is already 0-based from pd->plate_index (converted during 3MF load)
        // See: bbs_3mf.cpp:1485 where plate_index is converted to 0-based
        // DO NOT subtract 1 again - that would cause negative indices!
        int plate_index = current_plate_id;  // Already 0-based, no conversion needed
        print.set_plate_index(plate_index);

        // Get plate dimensions from printable_area config
        // GUI uses bed extended bounding box size minus DefaultTipRadius (1.25mm) for plate size
        // See: src/slic3r/GUI/Plater.cpp:9167
        // partplate_list.reset_size(max.x() - min.x() - Bed3D::Axes::DefaultTipRadius, ...)
        static const double BED_AXES_TIP_RADIUS = 1.25;  // Bed3D::Axes::DefaultTipRadius
        double plate_width = 200.0;   // Default fallback
        double plate_depth = 200.0;   // Default fallback

        if (config.has("printable_area")) {
            auto printable_area_opt = config.option<ConfigOptionPoints>("printable_area");
            if (printable_area_opt && !printable_area_opt->values.empty()) {
                // Calculate bounding box of printable area (same as GUI)
                BoundingBoxf bbox;
                for (const Vec2d& pt : printable_area_opt->values) {
                    bbox.merge(pt);
                }
                // CRITICAL: Match GUI's plate size calculation exactly
                // GUI: plate_size = bed_size - BED_AXES_TIP_RADIUS (only subtract once, not twice!)
                plate_width = bbox.size().x() - BED_AXES_TIP_RADIUS;
                plate_depth = bbox.size().y() - BED_AXES_TIP_RADIUS;
                BOOST_LOG_TRIVIAL(info) << "Plate size from printable_area: "
                    << plate_width << " x " << plate_depth
                    << " (original bbox: " << bbox.size().x() << " x " << bbox.size().y() << ")";
            }
        }

        // Calculate plate origin using exact same formula as GUI's PartPlateList
        // See: src/slic3r/GUI/PartPlate.cpp compute_shape_position() and plate_stride_x/y()
        // LOGICAL_PART_PLATE_GAP = 1/5 = 0.2
        // plate_stride = plate_size * (1 + gap)
        // origin.x = col * plate_stride_x
        // origin.y = -row * plate_stride_y (negative Y for downward layout)
        const double LOGICAL_PART_PLATE_GAP = 0.2;  // 1/5, same as GUI
        // CRITICAL: Use total plate count from source 3MF, not plates_to_process.size()
        // GUI uses the total number of plates in the project to calculate layout
        const int plate_cols = compute_column_count(static_cast<int>(plate_data.size()));

        BOOST_LOG_TRIVIAL(info) << "=== PLATE ORIGIN DEBUG ===";
        BOOST_LOG_TRIVIAL(info) << "Total plates to process: " << plates_to_process.size();
        BOOST_LOG_TRIVIAL(info) << "Computed plate_cols: " << plate_cols;
        BOOST_LOG_TRIVIAL(info) << "Current plate_id: " << current_plate_id << ", plate_index: " << plate_index;
        BOOST_LOG_TRIVIAL(info) << "Plate dimensions: " << plate_width << " x " << plate_depth;

        int row = plate_index / plate_cols;
        int col = plate_index % plate_cols;

        BOOST_LOG_TRIVIAL(info) << "Calculated row: " << row << ", col: " << col;

        double plate_stride_x = plate_width * (1.0 + LOGICAL_PART_PLATE_GAP);
        double plate_stride_y = plate_depth * (1.0 + LOGICAL_PART_PLATE_GAP);

        BOOST_LOG_TRIVIAL(info) << "Plate stride: X=" << plate_stride_x << ", Y=" << plate_stride_y;

        Vec3d plate_origin(
            col * plate_stride_x,   // x offset for column
            -row * plate_stride_y,  // y offset for row (negative)
            0                       // z always 0
        );

        print.set_plate_origin(plate_origin);
        BOOST_LOG_TRIVIAL(info) << "Plate " << current_plate_id << " origin: ("
            << plate_origin.x() << ", " << plate_origin.y() << ", " << plate_origin.z() << ")";
        BOOST_LOG_TRIVIAL(info) << "=== END PLATE ORIGIN DEBUG ===";

        // Instance offsets from 3MF are already in correct world coordinates.
        // Do NOT modify them here - the slicing pipeline handles plate_origin correctly:
        // 1. PrintApply.cpp extracts shift from model instance offset
        // 2. shift_without_plate_offset() subtracts plate_origin for slicing ops
        // 3. GCode generation adds plate_origin back via set_gcode_offset()
        // Modifying offsets here would cause double-subtraction and incorrect output.

        // Apply model and config to print
        auto apply_status = print.apply(model, config);
        BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

        // Assign unique arrange_order to each model instance so that
        // sort_object_instances_by_model_order() works correctly in validate().
        // GUI sets this during object arrangement; headless mode must set it manually.
        {
            int order = 1;
            for (ModelObject* obj : model.objects)
                for (ModelInstance* inst : obj->instances)
                    inst->arrange_order = order++;
        }

        // --- PRE-PROCESSING VALIDATION ---
        // Mirrors GUI's BackgroundSlicingProcess::validate() -> Print::validate()
        {
            StringObjectException warning;
            StringObjectException err = print.validate(&warning, nullptr, nullptr);

            // Surface any warning (non-fatal, slicing continues)
            if (!warning.string.empty()) {
                auto [obj_name, opt_hint] = format_exception_context(warning);
                std::cerr << "[WARNING] Plate " << current_plate_id << ": " << warning.string << obj_name << opt_hint << std::endl;
            }

            // A non-empty error string means slicing must be aborted
            if (!err.string.empty()) {
                auto [obj_name, opt_hint] = format_exception_context(err);
                std::cerr << "[ERROR] Plate " << current_plate_id << ": " << err.string << obj_name << opt_hint << std::endl;
                return EXIT_VALIDATION_ERROR;
            }
        }

        // Execute slicing
        BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << current_plate_id << "...";

        try {
            print.process();
        }
        catch (SlicingErrors& exs) {
            // Multiple objects failed — mirrors GUI's BackgroundSlicingProcess::format_error_message()
            for (const auto& ex : exs.errors_)
                std::cerr << "[ERROR] Plate " << current_plate_id << ": " << ex.what() << std::endl;
            return EXIT_SLICING_ERROR;
        }
        catch (SlicingError& ex) {
            // Single object failed (e.g. levitating object without supports)
            std::cerr << "[ERROR] Plate " << current_plate_id << ": " << ex.what() << std::endl;
            return EXIT_SLICING_ERROR;
        }
        catch (CanceledException&) {
            std::cerr << "[ERROR] Plate " << current_plate_id << ": Slicing was cancelled." << std::endl;
            return EXIT_SLICING_ERROR;
        }
        catch (std::exception& e) {
            std::cerr << "[ERROR] Plate " << current_plate_id << ": " << e.what() << std::endl;
            return EXIT_SLICING_ERROR;
        }

        BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << current_plate_id;

        // For gcode.3mf format, export to temp file first
        std::string gcode_output;
        if (format == OutputFormat::GCODE_3MF || !single_plate) {
            // Export to temp directory for later packaging
            gcode_output = temp_dir + "/plate_" + std::to_string(current_plate_id) + ".gcode";
            temp_files.push_back(gcode_output);  // Track for cleanup
        } else {
            gcode_output = output_path;
        }

        // Export G-code
        BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << current_plate_id << "...";

        PlateSliceResult slice_result;

        try {
            std::string result = print.export_gcode(gcode_output, &slice_result.gcode_result, nullptr);
            BOOST_LOG_TRIVIAL(info) << "G-code exported to: " << result;
            slice_result.gcode_path = result;

            // Get print statistics before print object is destroyed
            const PrintStatistics& ps = print.print_statistics();
            slice_result.total_weight = ps.total_weight;
            slice_result.support_used = print.is_support_used();
            
            // Collect additional statistics for JSON output
            slice_result.total_used_filament = ps.total_used_filament;
            slice_result.total_cost = ps.total_cost;
            slice_result.filament_volumes = slice_result.gcode_result.print_statistics.total_volumes_per_extruder;

            // --- POST-PROCESSING CHECKS ---
            // Mirror GUI's on_process_completed() checks on GCodeProcessorResult
            bool has_postprocess_warning = false;

            // Check for toolpaths outside the print volume
            if (slice_result.gcode_result.toolpath_outside) {
                log_plate_message("[Post-processing]", "WARNING", current_plate_id,
                    "Some toolpaths are outside the printable area.");
                has_postprocess_warning = true;
            }

            // Check conflict detection result (actual toolpath collision between objects,
            // distinct from validate()'s geometric pre-check — this runs during G-code generation)
            if (slice_result.gcode_result.conflict_result.has_value()) {
                const auto& cr = slice_result.gcode_result.conflict_result.value();
                std::string obj1 = cr._obj1 ? cr._objName1 : "Wipe Tower";
                std::string obj2 = cr._obj2 ? cr._objName2 : "Wipe Tower";
                log_plate_message("[Post-processing]", "ERROR", current_plate_id,
                    "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2
                    + "\" at Z=" + std::to_string(cr._height) + "mm.");
            }

            // Check bed/filament compatibility result (set during G-code export in GCode::do_export)
            if (!slice_result.gcode_result.bed_match_result.match) {
                const auto& bm = slice_result.gcode_result.bed_match_result;
                log_plate_message("[Post-processing]", "WARNING", current_plate_id,
                    "Filament " + std::to_string(bm.extruder_id + 1)
                    + " is not compatible with bed type \"" + bm.bed_type_name + "\".");
                has_postprocess_warning = true;
            }

            // Check timelapse warning codes (mirrors GUI's PartPlate::store_to_3mf_structure)
            // Bit 0: spiral vase + I3 printer -> timelapse not supported
            // Bit 1: by-object sequence + I3 printer -> timelapse not supported
            if (slice_result.gcode_result.timelapse_warning_code & 1) {
                log_plate_message("[Post-processing]", "WARNING", current_plate_id,
                    "Timelapse is not supported in spiral vase mode on this printer.");
                has_postprocess_warning = true;
            }
            if ((slice_result.gcode_result.timelapse_warning_code >> 1) & 1) {
                log_plate_message("[Post-processing]", "WARNING", current_plate_id,
                    "Timelapse is not supported with by-object print sequence on this printer.");
                has_postprocess_warning = true;
            }

            // Check per-step warnings collected during slicing
            // SliceWarning::level: 0=tip, 1=warning, 2=error
            // These mirror GUI's modal dialog for critical warnings on G-code export
            for (const auto& w : slice_result.gcode_result.warnings) {
                if (w.level >= 2) {
                    log_plate_message("[Post-processing]", "ERROR", current_plate_id,
                        w.msg + " (code: " + std::to_string(w.error_code) + ")");
                } else if (w.level == 1) {
                    log_plate_message("[Post-processing]", "WARNING", current_plate_id,
                        w.msg + " (code: " + std::to_string(w.error_code) + ")");
                    has_postprocess_warning = true;
                } else {
                    log_plate_message("[Post-processing]", "TIP", current_plate_id, w.msg);
                }
            }

            slice_result.has_postprocess_warning = has_postprocess_warning;

            plate_results[current_plate_id] = slice_result;
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << current_plate_id << ": " << e.what();
            return EXIT_EXPORT_ERROR;
        }
    }

    // Handle final output
    if (format == OutputFormat::GCODE_3MF || !single_plate) {
        // Package all gcode files into gcode.3mf
        BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

        // Get printer and filament metadata from config
        std::string printer_model_id;
        std::string nozzle_diameters_str;

        if (config.has("printer_model")) {
            printer_model_id = config.opt_string("printer_model");
        }

        if (config.has("nozzle_diameter")) {
            auto nozzle_opt = config.option<ConfigOptionFloats>("nozzle_diameter");
            if (nozzle_opt && !nozzle_opt->values.empty()) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2);
                for (size_t i = 0; i < nozzle_opt->values.size(); ++i) {
                    if (i > 0) ss << ",";
                    ss << nozzle_opt->values[i];
                }
                nozzle_diameters_str = ss.str();
            }
        }

        // Get filament type and color arrays from config
        const ConfigOptionStrings* filament_types = nullptr;
        const ConfigOptionStrings* filament_colors = nullptr;
        const ConfigOptionStrings* filament_ids = nullptr;

        if (config.has("filament_type")) {
            filament_types = config.option<ConfigOptionStrings>("filament_type");
        }
        if (config.has("filament_colour")) {
            filament_colors = config.option<ConfigOptionStrings>("filament_colour");
        }
        if (config.has("filament_ids")) {
            filament_ids = config.option<ConfigOptionStrings>("filament_ids");
        }

        // Prepare plate data with full slicing info, same as GUI's PartPlateList::store_to_3mf_structure
        for (auto& pd : plate_data) {
            auto it = plate_results.find(pd->plate_index);
            if (it != plate_results.end()) {
                PlateSliceResult& result = it->second;

                // Set gcode file path
                pd->gcode_file = result.gcode_path;

                // Mark as sliced valid (required for _add_gcode_file_to_archive)
                pd->is_sliced_valid = true;

                // Set printer metadata
                pd->printer_model_id = printer_model_id;
                pd->nozzle_diameters = nozzle_diameters_str;

                // Set print time prediction from GCodeProcessorResult
                auto& modes = result.gcode_result.print_statistics.modes;
                int print_time = (int)modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
                pd->gcode_prediction = std::to_string(print_time);

                // Set weight from PrintStatistics
                if (result.total_weight != 0.0) {
                    std::ostringstream ss;
                    ss << std::fixed << std::setprecision(2) << result.total_weight;
                    pd->gcode_weight = ss.str();
                }

                // Set toolpath outside flag
                pd->toolpath_outside = result.gcode_result.toolpath_outside;

                // Set timelapse warning code (matches GUI's PartPlate::store_to_3mf_structure)
                pd->timelapse_warning_code = result.gcode_result.timelapse_warning_code;

                // Set support used flag
                pd->is_support_used = result.support_used;

                // Set label object enabled flag
                pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

                // Parse filament info from GCodeProcessorResult (fills slice_filaments_info and warnings)
                pd->parse_filament_info(&result.gcode_result);

                // Fill additional filament metadata (type, color, filament_id) from config
                for (auto& info : pd->slice_filaments_info) {
                    size_t idx = static_cast<size_t>(info.id);
                    if (filament_types && idx < filament_types->values.size()) {
                        info.type = filament_types->values[idx];
                    }
                    if (filament_colors && idx < filament_colors->values.size()) {
                        info.color = filament_colors->values[idx];
                    }
                    if (filament_ids && idx < filament_ids->values.size()) {
                        info.filament_id = filament_ids->values[idx];
                    }
                }

                // Build objects_and_instances using model.objects array indices (not raw 3MF object IDs).
                // obj_inst_map key = 3MF object_id (e.g. 2, 4), but store_bbs_3mf expects
                // 0-based model.objects indices. Match via loaded_id == identify_id (set during load).
                std::set<int> plate_identify_ids;
                for (const auto& entry : pd->obj_inst_map)
                    plate_identify_ids.insert(entry.second.second);

                pd->objects_and_instances.clear();
                for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
                    const ModelObject* obj = model.objects[obj_idx];
                    for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
                        const ModelInstance* inst = obj->instances[inst_idx];
                        if (plate_identify_ids.count(static_cast<int>(inst->loaded_id)))
                            pd->objects_and_instances.emplace_back(
                                static_cast<int>(obj_idx),
                                static_cast<int>(inst_idx));
                    }
                }

                BOOST_LOG_TRIVIAL(info) << "Plate " << pd->plate_index
                    << ": gcode=" << pd->gcode_file
                    << ", prediction=" << pd->gcode_prediction << "s"
                    << ", weight=" << pd->gcode_weight << "g"
                    << ", support=" << (pd->is_support_used ? "yes" : "no")
                    << ", printer=" << pd->printer_model_id
                    << ", nozzle=" << pd->nozzle_diameters;
            }
        }

        // Use store_bbs_3mf to create the output
        StoreParams params;
        params.path = output_path.c_str();
        params.plate_data_list = plate_data;
        params.model = &model;
        params.config = &config;
        params.project_presets = project_presets;
        // Set export_plate_idx for proper thumbnail relationships (0-indexed)
        // For single plate: set to plate_id-1, for all plates: -1 (default)
        // For multi-plate export, set export_plate_idx based on the plate data
        if (single_plate) {
            params.export_plate_idx = plate_id - 1;  // Single plate: 0, 1, 2...
        } else {
            params.export_plate_idx = -1;  // Multi-plate: export all plates
        }
        // Strategy: match GUI's export_gcode_3mf behavior
        // - Silence: suppress verbose output
        // - SplitModel: match GUI export behavior
        // - WithGcode: include gcode files
        // - SkipModel: reduce file size by skipping model data
        // - Zip64: support large files (>4GB)
        params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel |
                         SaveStrategy::WithGcode | SaveStrategy::SkipModel |
                         SaveStrategy::Zip64;

        // Initialize thumbnail data vectors (empty for headless mode)
        // GUI generates these via rendering, but we can't do that in headless mode
        std::vector<ThumbnailData*> thumbnail_data;
        std::vector<ThumbnailData*> no_light_thumbnail_data;
        std::vector<ThumbnailData*> top_thumbnail_data;
        std::vector<ThumbnailData*> pick_thumbnail_data;
        std::vector<ThumbnailData*> calibration_thumbnail_data;
        std::vector<PlateBBoxData*> id_bboxes;
        // Use unique_ptr to own the PlateBBoxData objects; raw pointers are passed to params
        std::vector<std::unique_ptr<PlateBBoxData>> id_bboxes_owned;
        id_bboxes_owned.reserve(plate_data.size());
        for (size_t i = 0; i < plate_data.size(); ++i) {
            id_bboxes_owned.push_back(std::make_unique<PlateBBoxData>());
            id_bboxes.push_back(id_bboxes_owned.back().get());
        }

        params.thumbnail_data = thumbnail_data;
        params.no_light_thumbnail_data = no_light_thumbnail_data;
        params.top_thumbnail_data = top_thumbnail_data;
        params.pick_thumbnail_data = pick_thumbnail_data;
        params.calibration_thumbnail_data = calibration_thumbnail_data;
        params.id_bboxes = id_bboxes;

        // Initialize BBLProject (nullptr for headless mode, GUI uses project context)
        params.project = nullptr;
        params.profile = nullptr;

        try {
            bool success = store_bbs_3mf(params);
            if (!success) {
                BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package";
                return EXIT_EXPORT_ERROR;
            }
            BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << output_path;
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
            return EXIT_EXPORT_ERROR;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "Done!";
    std::cout << "Output: " << output_path << std::endl;

    // Determine final exit code: warn if any plate had post-processing warnings
    bool any_postprocess_warning = false;
    for (const auto& [id, result] : plate_results) {
        if (result.has_postprocess_warning)
            any_postprocess_warning = true;
    }
    
    // Build and output slice statistics as JSON
    output_stats.success = true;
    for (const auto& [plate_id, result] : plate_results) {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;
        plate_stats.success = true;
        plate_stats.gcode_file = result.gcode_path;
        
        // Time statistics from GCodeProcessorResult
        auto& modes = result.gcode_result.print_statistics.modes;
        auto& normal_mode = modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
        plate_stats.total_time = normal_mode.time;
        plate_stats.prepare_time = normal_mode.prepare_time;
        plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;
        
        // Filament statistics
        plate_stats.total_filament_m = result.total_used_filament;
        plate_stats.total_filament_g = result.total_weight;
        plate_stats.total_cost = result.total_cost;
        plate_stats.support_used = result.support_used;
        plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;
        
        // Calculate model filament (total - support - flush - wipe tower)
        // These values are calculated in GCodeViewer.cpp
        double total_support_m = 0.0, total_support_g = 0.0;
        double total_flush_m = 0.0, total_flush_g = 0.0;
        double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;
        
        // Get filament diameter and density for conversions
        const auto& filament_diameters = result.gcode_result.filament_diameters;
        const auto& filament_densities = result.gcode_result.filament_densities;
        
        // Calculate per-extruder filament usage
        for (const auto& [extruder_id, volume] : result.filament_volumes) {
            if (extruder_id < filament_diameters.size() && extruder_id < filament_densities.size()) {
                double diameter = filament_diameters[extruder_id];
                double density = filament_densities[extruder_id];
                double cross_section = M_PI * 0.25 * diameter * diameter;
                double used_m = (volume / cross_section) * 0.001;  // mm³ to m
                double used_g = volume * density * 0.001;  // mm³ to g
                plate_stats.filament_used_m[extruder_id] = used_m;
                plate_stats.filament_used_g[extruder_id] = used_g;
            }
        }
        
        // Get support, flush, and wipe tower volumes from print_statistics
        auto& ps = result.gcode_result.print_statistics;
        
        // Support volumes
        for (const auto& [extruder_id, volume] : ps.support_volumes_per_extruder) {
            if (extruder_id < filament_diameters.size() && extruder_id < filament_densities.size()) {
                double diameter = filament_diameters[extruder_id];
                double density = filament_densities[extruder_id];
                double cross_section = M_PI * 0.25 * diameter * diameter;
                total_support_m += (volume / cross_section) * 0.001;
                total_support_g += volume * density * 0.001;
            }
        }
        
        // Wipe tower volumes
        for (const auto& [extruder_id, volume] : ps.wipe_tower_volumes_per_extruder) {
            if (extruder_id < filament_diameters.size() && extruder_id < filament_densities.size()) {
                double diameter = filament_diameters[extruder_id];
                double density = filament_densities[extruder_id];
                double cross_section = M_PI * 0.25 * diameter * diameter;
                total_wipe_tower_m += (volume / cross_section) * 0.001;
                total_wipe_tower_g += volume * density * 0.001;
            }
        }
        
        // Flush volumes (flush_per_filament)
        for (const auto& [extruder_id, volume] : ps.flush_per_filament) {
            if (extruder_id < filament_diameters.size() && extruder_id < filament_densities.size()) {
                double diameter = filament_diameters[extruder_id];
                double density = filament_densities[extruder_id];
                double cross_section = M_PI * 0.25 * diameter * diameter;
                total_flush_m += (volume / cross_section) * 0.001;
                total_flush_g += volume * density * 0.001;
            }
        }
        
        // Model filament = total - support - flush - wipe tower
        plate_stats.model_filament_m = plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
        plate_stats.model_filament_g = plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;
        
        // NEW: Add nozzle diameters from config
        if (config.has("nozzle_diameter")) {
            auto nozzle_opt = config.option<ConfigOptionFloats>("nozzle_diameter");
            if (nozzle_opt) {
                plate_stats.nozzle_diameters = nozzle_opt->values;
            }
        }
        
        // NEW: Add plate count (total number of plates in the job)
        plate_stats.plate_count = static_cast<int>(plate_data.size());
        
        // NEW: Build filament details with type, color, and consumed weight
        // Get filament configuration arrays from config
        const ConfigOptionStrings* filament_types = nullptr;
        const ConfigOptionStrings* filament_colors = nullptr;
        if (config.has("filament_type")) {
            filament_types = config.option<ConfigOptionStrings>("filament_type");
        }
        if (config.has("filament_colour")) {
            filament_colors = config.option<ConfigOptionStrings>("filament_colour");
        }
        
        // Create filament details for each extruder
        for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g) {
            SliceOutputStats::FilamentDetail detail;
            detail.extruder_id = extruder_id;
            detail.used_g = used_g;
            
            // Get filament type
            if (filament_types && extruder_id < static_cast<int>(filament_types->values.size())) {
                detail.type = filament_types->values[extruder_id];
            } else {
                detail.type = "Unknown";
            }
            
            // Get filament color
            if (filament_colors && extruder_id < static_cast<int>(filament_colors->values.size())) {
                detail.color = filament_colors->values[extruder_id];
            } else {
                detail.color = "#000000";  // Default to black if not specified
            }
            
            plate_stats.filament_details.push_back(detail);
        }
        
        // NEW: Add model thumbnail (base64 encoded PNG)
        // For headless mode, we generate a simple placeholder thumbnail
        // In a real deployment, you might generate actual 3D preview images
        plate_stats.model_thumbnail = "";  // Empty for now (headless mode)
        
        output_stats.plates.push_back(plate_stats);
    }
    
    // Output JSON statistics
    output_slice_statistics(output_stats, json_output_path);

    // Explicitly clean up before quick_exit (which bypasses destructors)
    temp_guard.cleanup();

    // Use quick_exit to avoid crashes in global destructors
    std::quick_exit(any_postprocess_warning ? EXIT_POSTPROCESS_WARNING : EXIT_OK);
}
