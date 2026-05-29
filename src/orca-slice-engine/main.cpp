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
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

#include "Types.hpp"
#include "Utils.hpp"
#include "JsonReport.hpp"
#include "SliceEngine.hpp"

namespace {

struct CliArgs {
    EngineConfig engine_cfg;
    std::string  resources_dir;
    std::string  json_output_path;
    bool         json_enabled = false;
    bool         log_enabled  = false;
    std::string  log_file_path;
    bool         verbose      = false;
};

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_OK;
            std::exit(EXIT_OK);
        }
        else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        }
        else if (arg == "--log") {
            args.log_enabled = true;
        }
        else if (arg == "--log-file" && i + 1 < argc) {
            args.log_enabled  = true;
            args.log_file_path = argv[++i];
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            args.engine_cfg.output_base = argv[++i];
        }
        else if ((arg == "-r" || arg == "--resources") && i + 1 < argc) {
            args.resources_dir = argv[++i];
        }
        else if (arg == "-j" || arg == "--json") {
            args.json_enabled = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                args.json_output_path = argv[++i];
        }
        else if ((arg == "-p" || arg == "--plate") && i + 1 < argc) {
            std::string plate_arg = argv[++i];
            if (plate_arg == "all" || plate_arg == "0") {
                args.engine_cfg.plate_id = 0;
            } else {
                try {
                    args.engine_cfg.plate_id = std::stoi(plate_arg);
                    if (args.engine_cfg.plate_id < 1) {
                        std::cerr << "Error: Plate ID must be 1 or greater (or 'all')." << std::endl;
                        BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                        std::exit(EXIT_INVALID_ARGS);
                    }
                } catch (...) {
                    std::cerr << "Error: Invalid plate ID: " << plate_arg << std::endl;
                    BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                    std::exit(EXIT_INVALID_ARGS);
                }
            }
        }
        else if ((arg == "-d" || arg == "--data-dir") && i + 1 < argc) {
            BOOST_LOG_TRIVIAL(warning) << "--data-dir is deprecated and ignored; presets are always loaded from <resources>/profiles";
            ++i; // consume the value
        }
        else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            try {
                args.engine_cfg.timeout_seconds = std::stoi(argv[++i]);
                if (args.engine_cfg.timeout_seconds < 0)
                    args.engine_cfg.timeout_seconds = 0;
            } catch (...) {
                std::cerr << "Error: Invalid timeout value." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else if ((arg == "--max-size") && i + 1 < argc) {
            try {
                int val = std::stoi(argv[++i]);
                args.engine_cfg.max_size_mb = (val < 0) ? 0 : val;
            } catch (...) {
                std::cerr << "Error: Invalid max-size value." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else if (arg == "--cancel-file" && i + 1 < argc) {
            args.engine_cfg.cancel_file = argv[++i];
        }
        else if (arg == "--allow-custom-presets") {
            args.engine_cfg.substitute_printer  = false;
            args.engine_cfg.substitute_filaments = false;
        }
        else if (arg == "--allow-custom-printer-presets") {
            args.engine_cfg.substitute_printer = false;
        }
        else if (arg == "--allow-custom-filament-presets") {
            args.engine_cfg.substitute_filaments = false;
        }
        else if ((arg == "-f" || arg == "--format") && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "gcode") {
                args.engine_cfg.format = OutputFormat::GCODE;
            } else if (fmt == "gcode.3mf") {
                args.engine_cfg.format = OutputFormat::GCODE_3MF;
            } else {
                std::cerr << "Error: Unknown format: " << fmt << " (use 'gcode' or 'gcode.3mf')" << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        // Only parameters that are not recognized as options/option values are considered input files
        else if (arg[0] != '-') {
            if (args.engine_cfg.input_file.empty()) {
                args.engine_cfg.input_file = arg;
            } else {
                std::cerr << "Error: Multiple input files specified." << std::endl;
                BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
                std::exit(EXIT_INVALID_ARGS);
            }
        }
        else {
            std::cerr << "Error: Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
            std::exit(EXIT_INVALID_ARGS);
        }
    }

    // Validate that an input file was provided
    if (args.engine_cfg.input_file.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        print_usage(argv[0]);
        BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_INVALID_ARGS;
        std::exit(EXIT_INVALID_ARGS);
    }

    return args;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace Slic3r;

    boost::nowide::args a(argc, argv);

    // --- Setup console logging early so exit codes are always printed ---
    boost::log::add_console_log(std::cout, boost::log::keywords::format = "[%Severity%] %Message%");
    boost::log::add_common_attributes();
    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);

    // --- Parse CLI (single pass) ---
    CliArgs cli = parse_args(argc, argv);
    EngineConfig& cfg = cli.engine_cfg;
    std::string& resources_dir = cli.resources_dir;
    std::string& json_output_path = cli.json_output_path;
    bool json_enabled = cli.json_enabled;
    bool log_enabled   = cli.log_enabled;
    std::string& log_file_path = cli.log_file_path;
    bool verbose       = cli.verbose;

    if (!boost::filesystem::exists(cfg.input_file)) {
        std::cerr << "Error: Input file not found: " << cfg.input_file << std::endl;
        BOOST_LOG_TRIVIAL(info) << "Exiting with code " << EXIT_FILE_NOT_FOUND;
        return EXIT_FILE_NOT_FOUND;
    }

    // Validate plate/format combination
    cfg.single_plate = (cfg.plate_id > 0);
    if (!cfg.single_plate)
        cfg.format = OutputFormat::GCODE_3MF;

    // --- Pre-compute expected output path (used for auto-derived log/json paths) ---
    std::string expected_output;
    if (log_enabled && log_file_path.empty()) {
        expected_output = generate_output_path(
            cfg.input_file, cfg.output_base, cfg.plate_id, cfg.format, cfg.single_plate);
    }

    // --- Setup file logging if enabled ---
    if (log_enabled) {
        namespace expr = boost::log::expressions;
        if (log_file_path.empty()) {
            boost::filesystem::path out(expected_output);
            log_file_path = (out.parent_path() / out.stem().stem()).string() + ".log";
        }
        {
            boost::filesystem::path log_parent = boost::filesystem::path(log_file_path).parent_path();
            if (!log_parent.empty() && !boost::filesystem::exists(log_parent))
                boost::filesystem::create_directories(log_parent);
        }
        boost::log::add_file_log(
            boost::log::keywords::file_name = log_file_path,
            boost::log::keywords::auto_flush = true,
            boost::log::keywords::format = (
                expr::stream
                    << "[" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S")
                    << "] [" << boost::log::trivial::severity << "] " << expr::smessage
            )
        );
    }

    // Apply verbose filter override after parsing (console may already be set up)
    if (verbose)
        boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);

    BOOST_LOG_TRIVIAL(info) << "OrcaSlicer Cloud Engine v" << SLIC3R_VERSION;
    BOOST_LOG_TRIVIAL(info) << "Input file: " << cfg.input_file;
    BOOST_LOG_TRIVIAL(info) << "Plate: " << (cfg.single_plate ? std::to_string(cfg.plate_id) : "all");
    BOOST_LOG_TRIVIAL(info) << "Format: " << (cfg.format == OutputFormat::GCODE_3MF ? "gcode.3mf" : "gcode");
    if (cfg.timeout_seconds > 0)
        BOOST_LOG_TRIVIAL(info) << "Timeout: " << cfg.timeout_seconds << "s";

    // --- Setup resources directory ---
    if (!resources_dir.empty()) {
        set_resources_dir(resources_dir);
    } else {
        // Auto-detect: prefer ../resources (Ubuntu packaging: bin/orca-slice-engine -> x/resources/)
        // fall back to ./resources (development layout)
        boost::filesystem::path exe_dir = boost::dll::program_location().parent_path();
        boost::filesystem::path parent_resources = exe_dir.parent_path() / "resources";
        boost::filesystem::path local_resources  = exe_dir / "resources";
        if (boost::filesystem::exists(parent_resources)) {
            set_resources_dir(parent_resources.string());
        } else if (boost::filesystem::exists(local_resources)) {
            set_resources_dir(local_resources.string());
        } else {
            const char* env = std::getenv("ORCA_RESOURCES");
            if (env) {
                set_resources_dir(env);
                BOOST_LOG_TRIVIAL(info) << "Resources directory (from env): " << env;
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
    // Remove the per-process temp subdirectory
    if (!cfg.temp_dir.empty()) {
        try { boost::filesystem::remove_all(cfg.temp_dir); } catch (...) {}
    }

    int exit_code = engine.exit_code();
    BOOST_LOG_TRIVIAL(info) << "Exiting with code " << exit_code;
    return exit_code;
}
