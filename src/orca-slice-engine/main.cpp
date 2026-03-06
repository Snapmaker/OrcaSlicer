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
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/Geometry/BoundingBox.hpp"

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

    // Track temporary files for cleanup
    std::vector<std::string> temp_files;

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
    };
    std::map<int, PlateSliceResult> plate_results;

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
        // Also set print_volume_state to ensure is_printable() returns correct value
        // is_printable() checks: object->printable && printable && (print_volume_state == ModelInstancePVS_Inside)
        int instances_on_plate = 0;
        for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx) {
            ModelObject* obj = model.objects[obj_idx];
            for (size_t inst_idx = 0; inst_idx < obj->instances.size(); ++inst_idx) {
                ModelInstance* inst = obj->instances[inst_idx];
                // Match instances by loaded_id (which was set from identify_id during 3MF load)
                bool on_current_plate = (current_plate_identify_ids.find(static_cast<int>(inst->loaded_id)) != current_plate_identify_ids.end());
                inst->printable = on_current_plate;
                // Explicitly set print_volume_state to handle edge cases where 3MF has instances outside build volume
                inst->print_volume_state = on_current_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
                if (on_current_plate) {
                    instances_on_plate++;
                }
            }
        }

        BOOST_LOG_TRIVIAL(info) << "Filtered model: " << instances_on_plate
            << " instances on plate " << current_plate_id;

        // Skip empty plates (no instances to slice)
        if (instances_on_plate == 0) {
            BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate " << current_plate_id;
            continue;
        }

        // Create Print object for this plate
        Print print;
        print.set_status_callback(default_status_callback);

        // Set plate index and origin for multi-plate support
        // Calculate plate origin based on printer bed size and plate position
        // This matches GUI's PartPlateList::compute_origin() logic
        print.set_plate_index(current_plate_id - 1);  // 0-indexed

        // Get printable area dimensions from config
        double bed_width = 200.0;   // Default fallback
        double bed_depth = 200.0;   // Default fallback

        if (config.has("printable_area")) {
            auto printable_area_opt = config.option<ConfigOptionPoints>("printable_area");
            if (printable_area_opt && !printable_area_opt->values.empty()) {
                // Calculate bounding box of printable area
                BoundingBoxf3 bbox;
                for (const Vec2d& pt : printable_area_opt->values) {
                    bbox.merge(Vec3d(pt.x(), pt.y(), 0));
                }
                bed_width = bbox.size().x();
                bed_depth = bbox.size().y();
            }
        }

        // Calculate plate origin using same formula as GUI
        // LOGICAL_PART_PLATE_GAP = 0.2 (1/5)
        // For now, assume 1 column layout (simple case for most printers)
        const double plate_gap = 0.2;  // 1/5
        Vec3d plate_origin(
            0,  // x: always 0 for single column
            -(current_plate_id - 1) * bed_depth * (1.0 + plate_gap),  // y: negative offset for each plate
            0   // z: always 0
        );

        print.set_plate_origin(plate_origin);
        BOOST_LOG_TRIVIAL(info) << "Plate " << current_plate_id << " origin: ("
            << plate_origin.x() << ", " << plate_origin.y() << ", " << plate_origin.z() << ")";

        // Apply model and config to print
        auto apply_status = print.apply(model, config);
        BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

        // Execute slicing
        BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << current_plate_id << "...";

        try {
            print.process();
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Slicing failed for plate " << current_plate_id << ": " << e.what();
            // Cleanup temp files before exit
            for (const auto& file : temp_files) {
                try {
                    if (boost::filesystem::exists(file)) {
                        boost::filesystem::remove(file);
                    }
                } catch (...) {}
            }
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

            plate_results[current_plate_id] = slice_result;
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << current_plate_id << ": " << e.what();
            // Cleanup temp files before exit
            for (const auto& file : temp_files) {
                try {
                    if (boost::filesystem::exists(file)) {
                        boost::filesystem::remove(file);
                    }
                } catch (...) {}
            }
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

                BOOST_LOG_TRIVIAL(info) << "Plate " << pd->plate_index
                    << ": gcode=" << pd->gcode_file
                    << ", prediction=" << pd->gcode_prediction << "s"
                    << ", weight=" << pd->gcode_weight << "g"
                    << ", support=" << (pd->is_support_used ? "yes" : "no")
                    << ", printer=" << pd->printer_model_id
                    << ", nozzle=" << pd->nozzle_diameters;
            }
        }

        // Clear model objects so GUI recognizes this as gcode.3mf
        // GUI checks: model.objects.empty() && !has_print_instances
        model.clear_objects();
        BOOST_LOG_TRIVIAL(debug) << "Cleared model objects for gcode.3mf export";

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

        // Initialize empty bounding boxes for each plate
        for (size_t i = 0; i < plate_data.size(); ++i) {
            PlateBBoxData* bbox = new PlateBBoxData();
            id_bboxes.push_back(bbox);
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
                // Cleanup PlateBBoxData and temp files before exit
                for (auto* bbox : id_bboxes) { delete bbox; }
                for (const auto& file : temp_files) {
                    try { if (boost::filesystem::exists(file)) { boost::filesystem::remove(file); } } catch (...) {}
                }
                return EXIT_EXPORT_ERROR;
            }
            BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << output_path;
        }
        catch (std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
            // Cleanup PlateBBoxData and temp files before exit
            for (auto* bbox : id_bboxes) { delete bbox; }
            for (const auto& file : temp_files) {
                try { if (boost::filesystem::exists(file)) { boost::filesystem::remove(file); } } catch (...) {}
            }
            return EXIT_EXPORT_ERROR;
        }

        // Cleanup PlateBBoxData after successful export
        for (auto* bbox : id_bboxes) { delete bbox; }
    }

    BOOST_LOG_TRIVIAL(info) << "Done!";
    std::cout << "Output: " << output_path << std::endl;

    // Cleanup temporary files
    for (const auto& file : temp_files) {
        try {
            if (boost::filesystem::exists(file)) {
                boost::filesystem::remove(file);
                BOOST_LOG_TRIVIAL(debug) << "Cleaned up temp file: " << file;
            }
        } catch (...) {
            // Ignore cleanup errors
        }
    }

    // Use quick_exit to avoid crashes in global destructors
    std::quick_exit(EXIT_OK);
}
