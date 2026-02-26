/**
 * OrcaSlicer Cloud Slicing Engine
 * Minimal headless slicer for cloud deployment
 *
 * Usage: orca-slice-engine input.3mf [--output output.gcode] [--resources /path/to/profiles]
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <iostream>
#include <cstring>

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

void print_usage(const char* program_name) {
    std::cout << "OrcaSlicer Cloud Slicing Engine v" << SLIC3R_VERSION << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << program_name << " input.3mf [OPTIONS]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -o, --output <file>    Output G-code file path (default: same dir as input)" << std::endl;
    std::cout << "  -r, --resources <dir>  Resources directory containing printer profiles" << std::endl;
    std::cout << "  -v, --verbose          Enable verbose logging" << std::endl;
    std::cout << "  -h, --help             Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << program_name << " model.3mf" << std::endl;
    std::cout << "  " << program_name << " model.3mf -o output.gcode" << std::endl;
    std::cout << "  " << program_name << " model.3mf -r /usr/share/orca-slicer/profiles" << std::endl;
}

void default_status_callback(const PrintBase::SlicingStatus& status) {
    // Simple progress output for cloud environment
    if (status.percent >= 0) {
        std::cout << "[Progress] " << status.percent << "% - " << status.text << std::endl;
    } else {
        std::cout << "[Status] " << status.text << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    boost::nowide::args a(argc, argv);

    // Default values
    std::string input_file;
    std::string output_file;
    std::string resources_dir;
    bool verbose = false;

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
            output_file = argv[++i];
        }
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc) {
            resources_dir = argv[++i];
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
            0         // plate_id (0 = all plates)
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

    // Determine output file path
    if (output_file.empty()) {
        boost::filesystem::path input_path(input_file);
        output_file = input_path.parent_path().string() + "/" +
                     input_path.stem().string() + ".gcode";
    }
    BOOST_LOG_TRIVIAL(info) << "Output file: " << output_file;

    // Create Print object and apply configuration
    BOOST_LOG_TRIVIAL(info) << "Setting up print configuration...";

    Print print;
    print.set_status_callback(default_status_callback);

    // Apply model and config to print
    // This links the model objects to the print and applies the configuration
    auto apply_status = print.apply(model, config);
    BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

    // For BBL 3MF, use plate data if available
    if (!plate_data.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Found " << plate_data.size() << " plate(s) in 3MF";
        // Use first plate for MVP
        // TODO: Support multi-plate slicing
    }

    // Execute slicing
    BOOST_LOG_TRIVIAL(info) << "Starting slicing process...";

    try {
        print.process();
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Slicing failed: " << e.what();
        return EXIT_SLICING_ERROR;
    }

    BOOST_LOG_TRIVIAL(info) << "Slicing completed successfully";

    // Export G-code
    BOOST_LOG_TRIVIAL(info) << "Exporting G-code...";

    // Create GCodeProcessorResult for proper export (like GUI does)
    GCodeProcessorResult gcode_result;

    try {
        std::string result = print.export_gcode(output_file, &gcode_result, nullptr);
        BOOST_LOG_TRIVIAL(info) << "G-code exported to: " << result;
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code: " << e.what();
        return EXIT_EXPORT_ERROR;
    }

    BOOST_LOG_TRIVIAL(info) << "Done!";
    std::cout << "Output: " << output_file << std::endl;

    // Use quick_exit to avoid crashes in global destructors
    // The G-code is already written, so it's safe to exit immediately
    std::quick_exit(EXIT_OK);
}
