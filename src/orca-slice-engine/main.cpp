/**
 * OrcaSlicer Cloud Slicing Engine
 * Minimal headless slicer for cloud deployment
 *
 * Usage: orca-slice-engine input.3mf [OPTIONS]
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

#include "Types.hpp"
#include "Utils.hpp"
#include "JsonReport.hpp"
#include "SliceEngine.hpp"

namespace {

EngineConfig parse_args(int argc, char* argv[]) {
    EngineConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_OK);
        }
        else if (arg == "-v" || arg == "--verbose") {
            continue;
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            cfg.output_base = argv[++i];
        }
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc) {
            // Skip the parameter values to avoid being regarded as input files
            ++i;
        }
        else if ((arg == "-j" || arg == "--json") && i + 1 < argc) {
            // Skip the parameter values to avoid being regarded as input files
            ++i;
        }
        else if ((arg == "-p" || arg == "--plate") && i + 1 < argc) {
            std::string plate_arg = argv[++i];
            if (plate_arg == "all" || plate_arg == "0") {
                cfg.plate_id = 0;
            } else {
                try {
                    cfg.plate_id = std::stoi(plate_arg);
                    if (cfg.plate_id < 1) {
                        std::cerr << "Error: Plate ID must be 1 or greater (or 'all')." << std::endl;
                        std::exit(EXIT_INVALID_ARGS);
                    }
                } catch (...) {
                    std::cerr << "Error: Invalid plate ID: " << plate_arg << std::endl;
                    std::exit(EXIT_INVALID_ARGS);
                }
            }
        }
        else if ((arg == "-d" || arg == "--data-dir") && i + 1 < argc) {
            cfg.data_dir = argv[++i];
        }
        else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "gcode") {
                cfg.format = OutputFormat::GCODE;
            } else if (fmt == "gcode.3mf") {
                cfg.format = OutputFormat::GCODE_3MF;
            } else {
                std::cerr << "Error: Unknown format: " << fmt << " (use 'gcode' or 'gcode.3mf')" << std::endl;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        // Only parameters that are not recognized as options/option values are considered input files
        else if (arg[0] != '-') {
            if (cfg.input_file.empty()) {
                cfg.input_file = arg;
            } else {
                std::cerr << "Error: Multiple input files specified." << std::endl;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else {
            std::cerr << "Error: Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            std::exit(EXIT_INVALID_ARGS);
        }
    }

    // Validate that an input file was provided
    if (cfg.input_file.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        print_usage(argv[0]);
        std::exit(EXIT_INVALID_ARGS);
    }

    return cfg;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace Slic3r;

    boost::nowide::args a(argc, argv);

    // --- Parse CLI ---
    EngineConfig cfg = parse_args(argc, argv);

    // Extract post-engine flags (these are consumed separately)
    std::string resources_dir;
    std::string json_output_path;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose")
            verbose = true;
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc)
            resources_dir = argv[++i];
        else if ((arg == "-j" || arg == "--json") && i + 1 < argc)
            json_output_path = argv[++i];
    }

    // Validate input file
    if (cfg.input_file.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        print_usage(argv[0]);
        return EXIT_INVALID_ARGS;
    }

    if (!boost::filesystem::exists(cfg.input_file)) {
        std::cerr << "Error: Input file not found: " << cfg.input_file << std::endl;
        return EXIT_FILE_NOT_FOUND;
    }

    // Validate plate/format combination
    cfg.single_plate = (cfg.plate_id > 0);
    if (!cfg.single_plate)
        cfg.format = OutputFormat::GCODE_3MF;

    // --- Setup logging ---
    boost::log::add_console_log(std::cout, boost::log::keywords::format = "[%Severity%] %Message%");
    boost::log::add_common_attributes();

    if (verbose)
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);
    else
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);

    BOOST_LOG_TRIVIAL(info) << "OrcaSlicer Cloud Engine v" << SLIC3R_VERSION;
    BOOST_LOG_TRIVIAL(info) << "Input file: " << cfg.input_file;
    BOOST_LOG_TRIVIAL(info) << "Plate: " << (cfg.single_plate ? std::to_string(cfg.plate_id) : "all");
    BOOST_LOG_TRIVIAL(info) << "Format: " << (cfg.format == OutputFormat::GCODE_3MF ? "gcode.3mf" : "gcode");

    // --- Setup resources directory ---
    if (!resources_dir.empty()) {
        set_resources_dir(resources_dir);
        BOOST_LOG_TRIVIAL(info) << "Resources directory: " << resources_dir;
    } else {
        boost::filesystem::path exe_path = boost::dll::program_location();
        boost::filesystem::path resource_path = exe_path.parent_path() / "resources";
        if (boost::filesystem::exists(resource_path)) {
            set_resources_dir(resource_path.string());
            BOOST_LOG_TRIVIAL(info) << "Resources directory: " << resource_path;
        } else {
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
    if (!resources_dir.empty() && boost::filesystem::exists(resources_dir)) {
        bool has_profiles = boost::filesystem::exists(resources_dir + "/profiles");
        bool has_printers = boost::filesystem::exists(resources_dir + "/printers");
        if (!has_profiles && !has_printers)
            BOOST_LOG_TRIVIAL(warning) << "Resources directory may be incomplete: " << resources_dir;
        else
            BOOST_LOG_TRIVIAL(info) << "Resources directory validated: " << resources_dir;
    }

    // --- Setup temporary directory (process-isolated to avoid multi-process collisions) ---
    {
        auto pid = get_current_pid();
        auto ts  = std::chrono::system_clock::now().time_since_epoch().count();
        auto unique_dir = boost::filesystem::temp_directory_path()
            / (std::string("orca_slice_") + std::to_string(pid) + "_" + std::to_string(ts));
        boost::filesystem::create_directories(unique_dir);
        cfg.temp_dir = unique_dir.string();
    }
    set_temporary_dir(cfg.temp_dir);
    BOOST_LOG_TRIVIAL(info) << "Temporary directory: " << cfg.temp_dir;

    // --- Run the slicing pipeline ---
    std::vector<std::string> temp_files;
    TempFileGuard temp_guard(temp_files);

    SliceEngine engine(cfg, temp_files);
    engine.run();

    // --- Output JSON ---
    if (json_output_path.empty()) {
        boost::filesystem::path out_path(engine.output_path());
        json_output_path = (out_path.parent_path() / out_path.stem().stem()).string() + ".json";
    }
    output_slice_statistics(engine.stats(), json_output_path, engine.output_path());

    // --- Cleanup & exit ---
    temp_guard.cleanup();
    // Remove the per-process temp subdirectory (std::quick_exit skips destructors)
    if (!cfg.temp_dir.empty()) {
        try { boost::filesystem::remove_all(cfg.temp_dir); } catch (...) {}
    }

    int exit_code;
    if (engine.any_error())
        exit_code = EXIT_VALIDATION_ERROR;
    else if (engine.any_postprocess_warning())
        exit_code = EXIT_POSTPROCESS_WARNING;
    else
        exit_code = EXIT_OK;

    std::quick_exit(exit_code);
}
