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
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

using namespace Slic3r;

// Exit codes
static const int EXIT_OK = 0;
static const int EXIT_INVALID_ARGS = 1;
static const int EXIT_FILE_NOT_FOUND = 2;
static const int EXIT_LOAD_ERROR = 3;
static const int EXIT_SLICING_ERROR = 4;
static const int EXIT_EXPORT_ERROR = 5;

// Output format enum
enum class OutputFormat {
    GCODE,
    GCODE_3MF
};

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
    std::cout << "  -v, --verbose          Enable verbose logging" << std::endl;
    std::cout << "  -h, --help             Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " model.3mf                        # All plates -> model.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1                   # Plate 1 -> model-p1.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -f gcode          # Plate 1 -> model-p1.gcode (plain text)" << std::endl;
    std::cout << "  " << program_name << " model.3mf -p 1 -o output         # Plate 1 -> output.gcode.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -o result              # All plates -> result.gcode.3mf" << std::endl;
}

void default_status_callback(const PrintBase::SlicingStatus& status) {
    // Simple progress output for cloud environment
    if (status.percent >= 0) {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    } else {
        std::cout << "[Status] " << status.text << std::endl;
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
        base_name = input_path.parent_path().string() + "/" + input_path.stem().string();
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

    // Set temporary directory
    std::string temp_dir = boost::filesystem::temp_directory_path().string();
    set_temporary_dir(temp_dir);
    BOOST_LOG_TRIVIAL(debug) << "Temporary directory: " << temp_dir;

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

    // Map to store gcode paths for each plate (for gcode.3mf export)
    std::map<int, std::string> plate_gcode_paths;

    // Process each plate
    for (int current_plate_id : plates_to_process) {
        BOOST_LOG_TRIVIAL(info) << "=== Processing plate " << current_plate_id << " ===";

        // Create Print object for this plate
        Print print;
        print.set_status_callback(default_status_callback);

        // Apply model and config to print
        // Note: In headless mode, we apply the entire model
        // The plate filtering is handled by the Print system
        auto apply_status = print.apply(model, config);
        BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

        // Execute slicing
        BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << current_plate_id << "...";

        try {
            print.process();
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Slicing failed for plate " << current_plate_id << ": " << e.what();
            return EXIT_SLICING_ERROR;
        }

        BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << current_plate_id;

        // For gcode.3mf format, export to temp file first
        std::string gcode_output;
        if (format == OutputFormat::GCODE_3MF || !single_plate) {
            // Export to temp directory for later packaging
            gcode_output = temp_dir + "/plate_" + std::to_string(current_plate_id) + ".gcode";
        } else {
            gcode_output = output_path;
        }

        // Export G-code
        BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << current_plate_id << "...";

        GCodeProcessorResult gcode_result;

        try {
            std::string result = print.export_gcode(gcode_output, &gcode_result, nullptr);
            BOOST_LOG_TRIVIAL(info) << "G-code exported to: " << result;
            plate_gcode_paths[current_plate_id] = result;
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

        // Prepare plate data with gcode paths
        for (auto& pd : plate_data) {
            auto it = plate_gcode_paths.find(pd->plate_index);
            if (it != plate_gcode_paths.end()) {
                pd->gcode_file = it->second;
            }
        }

        // Use store_bbs_3mf to create the output
        StoreParams params;
        params.path = output_path.c_str();
        params.plate_data_list = plate_data;
        params.model = &model;
        params.config = &config;
        params.project_presets = project_presets;
        // Strategy: include gcode but skip model data to reduce size
        params.strategy = SaveStrategy::WithGcode | SaveStrategy::SkipModel;

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

    // Use quick_exit to avoid crashes in global destructors
    std::quick_exit(EXIT_OK);
}
