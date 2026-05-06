#include "SliceEngine.hpp"
#include "Utils.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <set>

#include <boost/log/trivial.hpp>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;

namespace {

// Bed3D::Axes::DefaultTipRadius, used in plate size calculation
const double BED_AXES_TIP_RADIUS = 1.25;

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP
const double LOGICAL_PART_PLATE_GAP = 0.2;

} // namespace

SliceEngine::SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files)
    : m_cfg(cfg)
    , m_temp_files(temp_files)
{
}

bool SliceEngine::run() {
    bool load_ok = load_3mf();
    bool validate_ok = load_ok && validate_input();

    if (load_ok && validate_ok) {
        m_output_path = generate_output_path(m_cfg.input_file, m_cfg.output_base,
                                             m_cfg.plate_id, m_cfg.format, m_cfg.single_plate);
        BOOST_LOG_TRIVIAL(info) << "Output file: " << m_output_path;

        // Collect plates to process (internal plate_index is 0-based)
        std::vector<int> plates_to_process;
        if (m_cfg.single_plate) {
            plates_to_process.push_back(m_cfg.plate_id - 1);  // CLI 1-based → internal 0-based
        } else {
            for (const auto& pd : m_plate_data)
                plates_to_process.push_back(pd->plate_index);
        }

        // Process each plate
        for (int plate_id : plates_to_process) {
            BOOST_LOG_TRIVIAL(info) << "=== Processing plate " << (plate_id + 1) << " ===";
            process_plate(plate_id);
        }

        // Package output — only if no errors occurred
        bool has_output = !m_plate_results.empty();
        if (has_output && !m_any_error && (m_cfg.format == OutputFormat::GCODE_3MF || !m_cfg.single_plate))
            package_output();

        // For single-plate GCODE mode, the file is written directly to m_output_path
        // during export_gcode. Remove it if errors occurred.
        if (m_any_error && m_cfg.single_plate && m_cfg.format == OutputFormat::GCODE) {
            boost::filesystem::remove(m_output_path);
            BOOST_LOG_TRIVIAL(info) << "Removed output file due to errors: " << m_output_path;
        }

        if (has_output && !m_any_error)
            BOOST_LOG_TRIVIAL(info) << "Done!";
    }

    // Always build JSON statistics (even on early failure) so
    // success=false and error_message are reflected in JSON output.
    build_statistics();

    return !m_plate_results.empty();
}

// ============================================================================
// Stage 1: Load 3MF
// ============================================================================

bool SliceEngine::load_3mf() {
    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

    ConfigSubstitutionContext config_substitutions(ForwardCompatibilitySubstitutionRule::Enable);

    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                           LoadStrategy::AddDefaultInstances | LoadStrategy::LoadAuxiliary;

    try {
        m_model = Model::read_from_file(
            m_cfg.input_file,
            &m_config,
            &config_substitutions,
            strategy,
            &m_plate_data,
            &m_project_presets,
            &m_is_bbl_3mf,
            &m_file_version,
            nullptr,
            nullptr,
            nullptr,
            0
        );
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << e.what();
        m_any_error = true;
        m_stats.error_message = std::string("Failed to load 3MF file: ") + e.what();
        Issue issue;
        issue.level = "error";
        issue.plate_id = -1;
        issue.z_height = -1.0;
        issue.message = m_stats.error_message;
        m_stats.issues.push_back(issue);
        return false;
    }

    if (m_model.objects.empty()) {
        BOOST_LOG_TRIVIAL(error) << "No objects found in 3MF file";
        m_any_error = true;
        m_stats.error_message = "3MF file contains no sliceable model objects";
        Issue issue;
        issue.level = "error";
        issue.plate_id = -1;
        issue.z_height = -1.0;
        issue.message = m_stats.error_message;
        m_stats.issues.push_back(issue);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Loaded " << m_model.objects.size() << " object(s)";
    BOOST_LOG_TRIVIAL(info) << "3MF version: " << m_file_version.to_string();

    return true;
}

// ============================================================================
// Stage 2: Validate input
// ============================================================================

bool SliceEngine::validate_input() {
    // Check plate availability
    if (m_plate_data.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No plate data in 3MF, treating as single plate";
        PlateData* pd = new PlateData();
        pd->plate_index = 0;
        m_plate_data.push_back(pd);
    }

    BOOST_LOG_TRIVIAL(info) << "Found " << m_plate_data.size() << " plate(s) in 3MF";

    // Validate requested plate exists
    if (m_cfg.single_plate) {
        // Internal plate_index is 0-based (from 3MF import), CLI plate_id is 1-based
        bool plate_found = false;
        for (const auto& pd : m_plate_data) {
            if (pd->plate_index + 1 == m_cfg.plate_id) {
                plate_found = true;
                break;
            }
        }
        if (!plate_found) {
            BOOST_LOG_TRIVIAL(error) << "Plate " << m_cfg.plate_id << " not found in 3MF file";
            std::cerr << "Available plates: ";
            for (size_t i = 0; i < m_plate_data.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << m_plate_data[i]->plate_index + 1;
            }
            std::cerr << std::endl;
            m_any_error = true;
            m_stats.error_message = "Requested plate " + std::to_string(m_cfg.plate_id) + " not found in 3MF file";
            Issue issue;
            issue.level = "error";
            issue.plate_id = -1;
            issue.z_height = -1.0;
            issue.message = m_stats.error_message;
            m_stats.issues.push_back(issue);
            return false;
        }
    }

    return true;
}

// ============================================================================
// Stage 3: Process a single plate
// ============================================================================

void SliceEngine::process_plate(int plate_id) {
    // --- Filter instances for this plate ---
    std::set<int> identify_ids;
    int instances_on_plate = filter_instances(plate_id, identify_ids);

    BOOST_LOG_TRIVIAL(info) << "Filtered model: " << instances_on_plate
        << " instances on plate " << (plate_id + 1);

    if (instances_on_plate == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Skipping empty plate " << (plate_id + 1);
        return;
    }

    // --- Build volume check ---
    if (!run_build_volume_check(plate_id, identify_ids))
        return;

    // --- Setup Print object ---
    Print print;
    print.set_status_callback(default_status_callback);

    // Calculate plate dimensions
    double plate_width = 200.0;
    double plate_depth = 200.0;

    if (m_config.has("printable_area")) {
        auto printable_area_opt = m_config.option<ConfigOptionPoints>("printable_area");
        if (printable_area_opt && !printable_area_opt->values.empty()) {
            BoundingBoxf bbox;
            for (const Vec2d& pt : printable_area_opt->values)
                bbox.merge(pt);
            plate_width = bbox.size().x() - BED_AXES_TIP_RADIUS;
            plate_depth = bbox.size().y() - BED_AXES_TIP_RADIUS;
            BOOST_LOG_TRIVIAL(info) << "Plate size from printable_area: "
                << plate_width << " x " << plate_depth
                << " (original bbox: " << bbox.size().x() << " x " << bbox.size().y() << ")";
        }
    }

    setup_print_origin(plate_id, plate_width, plate_depth);

    // --- Apply model ---
    if (!apply_model(plate_id, print))
        return;

    // --- Assign arrange_order ---
    {
        int order = 1;
        for (ModelObject* obj : m_model.objects)
            for (ModelInstance* inst : obj->instances)
                inst->arrange_order = order++;
    }

    // --- Validation ---
    if (!run_validation(plate_id, print))
        return;

    // --- Slicing ---
    if (!run_slicing(plate_id, print))
        return;

    BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << (plate_id + 1);

    // --- Export G-code ---
    PlateSliceResult slice_result;
    if (!export_gcode(plate_id, print, slice_result))
        return;

    // --- Post-processing ---
    run_postprocessing(plate_id, slice_result);

    m_plate_results[plate_id] = slice_result;
}

// ============================================================================
// Per-plate sub-stages
// ============================================================================

int SliceEngine::filter_instances(int plate_id, std::set<int>& identify_ids) {
    for (const auto& pd : m_plate_data) {
        if (pd->plate_index == plate_id) {
            for (const auto& [object_id, inst_info] : pd->obj_inst_map) {
                identify_ids.insert(inst_info.second);
            }
            break;
        }
    }

    int count = 0;
    for (ModelObject* obj : m_model.objects) {
        for (ModelInstance* inst : obj->instances) {
            bool on_plate = (identify_ids.find(static_cast<int>(inst->loaded_id)) != identify_ids.end());
            inst->printable = on_plate;
            inst->print_volume_state = on_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
            if (on_plate) ++count;
        }
    }
    return count;
}

bool SliceEngine::run_build_volume_check(int plate_id, const std::set<int>& identify_ids) {
    if (!(m_config.has("printable_area") && m_config.has("printable_height")))
        return true;

    auto printable_area_opt = m_config.option<ConfigOptionPoints>("printable_area");
    double printable_height = m_config.opt_float("printable_height");
    if (!printable_area_opt || printable_area_opt->values.empty() || printable_height <= 0)
        return true;

    BuildVolume build_volume(printable_area_opt->values, printable_height);
    m_model.update_print_volume_state(build_volume);

    bool has_partly_outside = false;
    for (ModelObject* obj : m_model.objects) {
        for (ModelInstance* inst : obj->instances) {
            if (!inst->printable) continue;
            if (inst->print_volume_state == ModelInstancePVS_Partly_Outside) {
                log_plate_message("[Pre-processing]", "ERROR", plate_id,
                    "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume.");
                has_partly_outside = true;
                Issue issue;
                issue.level = "error";
                issue.plate_id = plate_id;
                issue.object_name = obj->name;
                issue.z_height = -1.0;
                issue.message = "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume";
                m_stats.issues.push_back(issue);
            }
        }
    }

    // Restore printable state — update_print_volume_state may have changed
    // print_volume_state for on-plate instances (e.g. marked them Fully_Outside
    // because grid-layout positions are far from build-volume origin).
    for (ModelObject* obj : m_model.objects) {
        for (ModelInstance* inst : obj->instances) {
            bool on_plate = (identify_ids.find(static_cast<int>(inst->loaded_id)) != identify_ids.end());
            inst->printable = on_plate;
            inst->print_volume_state = on_plate ? ModelInstancePVS_Inside : ModelInstancePVS_Fully_Outside;
        }
    }

    if (has_partly_outside) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " has objects outside build volume, skipping";
        m_any_error = true;
        return false;
    }
    return true;
}

void SliceEngine::setup_print_origin(int plate_id, double plate_width, double plate_depth) {
    BOOST_LOG_TRIVIAL(info) << "Plate " << plate_id << " origin: (0, 0, 0)"
        << " (cloud slicing, independent per-plate)";
    BOOST_LOG_TRIVIAL(info) << "Plate dimensions: " << plate_width << " x " << plate_depth;
}

bool SliceEngine::apply_model(int plate_id, Print& print) {
    // plate_index from m_plate_data is already 0-based (import does -1 conversion)
    print.set_plate_index(plate_id);

    // Cloud slicing: each plate is sliced independently at origin.
    // Plate origin offsets are only meaningful for GUI multi-plate layout display.
    print.set_plate_origin(Vec3d(0, 0, 0));

    // Guard against wipe tower / tool change mismatch.
    // If the model only uses a single extruder but the 3MF config has multiple filaments,
    // Print::has_wipe_tower() still returns true (filament count > 1), which causes
    // "WipeTowerIntegration::append_tcr was asked to do a toolchange it didn't expect"
    // during G-code export. Disable the prime tower when it's not needed.
    {
        std::set<int> used_extruders;
        for (ModelObject* obj : m_model.objects) {
            for (ModelInstance* inst : obj->instances) {
                if (!inst->is_printable())
                    continue;
                for (ModelVolume* vol : obj->volumes) {
                    if (!vol->is_model_part())
                        continue;
                    for (int eid : vol->get_extruders())
                        used_extruders.insert(eid);
                }
            }
        }
        if (used_extruders.size() <= 1) {
            BOOST_LOG_TRIVIAL(info) << "Only " << used_extruders.size()
                << " extruder(s) used by model, disabling prime tower";
            m_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        }
    }

    // Diagnostic: check shared model state before apply
    BOOST_LOG_TRIVIAL(info) << "=== DIAG plate " << plate_id << " shared model state ===";
    for (ModelObject* obj : m_model.objects) {
        BOOST_LOG_TRIVIAL(info) << "  Obj \"" << obj->name << "\" obj.printable=" << obj->printable
            << " instances=" << obj->instances.size();
        for (ModelInstance* inst : obj->instances) {
            BOOST_LOG_TRIVIAL(info) << "    inst lid=" << inst->loaded_id
                << " p=" << inst->printable
                << " pvs=" << static_cast<int>(inst->print_volume_state)
                << " is_p=" << inst->is_printable();
        }
    }
    BOOST_LOG_TRIVIAL(info) << "=== END DIAG ===";

    auto apply_status = print.apply(m_model, m_config);
    BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

    if (print.num_object_instances() == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << " has no printable objects after apply, skipping";
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "No printable objects on this plate after apply";
        m_stats.issues.push_back(issue);
        return false;
    }

    return true;
}

bool SliceEngine::run_validation(int plate_id, Print& print) {
    StringObjectException warning;
    StringObjectException err = print.validate(&warning, nullptr, nullptr);

    if (!warning.string.empty()) {
        auto [obj_name, opt_hint] = format_exception_context(warning);
        std::cerr << "[WARNING] Plate " << plate_id << ": " << warning.string << obj_name << opt_hint << std::endl;
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.object_name = obj_name;
        issue.z_height = -1.0;
        issue.message = warning.string + opt_hint;
        m_stats.issues.push_back(issue);
    }

    if (!err.string.empty()) {
        auto [obj_name, opt_hint] = format_exception_context(err);
        std::cerr << "[ERROR] Plate " << plate_id << ": " << err.string << obj_name << opt_hint << std::endl;
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.object_name = obj_name;
        issue.z_height = -1.0;
        issue.message = err.string + opt_hint;
        m_stats.issues.push_back(issue);
        m_any_error = true;
        return false;
    }

    return true;
}
bool SliceEngine::run_slicing(int plate_id, Print& print) {
    BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << plate_id << "...";

    try {
        print.process();
    }
    catch (SlicingErrors& exs) {
        for (const auto& ex : exs.errors_) {
            std::cerr << "[ERROR] Plate " << plate_id << ": " << ex.what() << std::endl;
            Issue issue;
            issue.level = "error";
            issue.plate_id = plate_id;
            issue.z_height = -1.0;
            issue.message = ex.what();
            m_stats.issues.push_back(issue);
        }
        m_any_error = true;
        return false;
    }
    catch (SlicingError& ex) {
        std::cerr << "[ERROR] Plate " << plate_id << ": " << ex.what() << std::endl;
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = ex.what();
        m_stats.issues.push_back(issue);
        m_any_error = true;
        return false;
    }
    catch (CanceledException&) {
        std::cerr << "[ERROR] Plate " << plate_id << ": Slicing was cancelled." << std::endl;
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "Slicing was cancelled";
        m_stats.issues.push_back(issue);
        m_any_error = true;
        return false;
    }
    catch (std::exception& e) {
        std::cerr << "[ERROR] Plate " << plate_id << ": " << e.what() << std::endl;
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = e.what();
        m_stats.issues.push_back(issue);
        m_any_error = true;
        return false;
    }

    return true;
}

bool SliceEngine::export_gcode(int plate_id, Print& print, PlateSliceResult& result) {
    std::string gcode_output;
    if (m_cfg.format == OutputFormat::GCODE_3MF || !m_cfg.single_plate) {
        gcode_output = m_cfg.temp_dir + "/plate_" + std::to_string(plate_id) + ".gcode";
        m_temp_files.push_back(gcode_output);
    } else {
        gcode_output = m_output_path;
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code for plate " << plate_id << "...";

    try {
        std::string exported = print.export_gcode(gcode_output, &result.gcode_result, nullptr);
        BOOST_LOG_TRIVIAL(info) << "G-code exported to: " << exported;
        result.gcode_path = exported;

        const PrintStatistics& ps = print.print_statistics();
        result.total_weight = ps.total_weight;
        result.support_used = print.is_support_used();
        result.total_used_filament = ps.total_used_filament;
        result.total_cost = ps.total_cost;
        result.filament_volumes = result.gcode_result.print_statistics.total_volumes_per_extruder;

        return true;
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id << ": " << e.what();
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = std::string("G-code export failed: ") + e.what();
        m_stats.issues.push_back(issue);
        m_any_error = true;
        m_plate_results[plate_id] = PlateSliceResult{};
        return false;
    }
}

void SliceEngine::run_postprocessing(int plate_id, PlateSliceResult& result) {
    bool has_postprocess_error = false;
    bool has_postprocess_warning = false;

    // Toolpaths outside print volume
    if (result.gcode_result.toolpath_outside) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Some toolpaths are outside the printable area.");
        has_postprocess_warning = true;
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "Some toolpaths are outside the printable area";
        result.issues.push_back(issue);
    }

    // Toolpath conflict detection
    if (result.gcode_result.conflict_result.has_value()) {
        const auto& cr = result.gcode_result.conflict_result.value();
        std::string obj1 = cr._obj1 ? cr._objName1 : "Wipe Tower";
        std::string obj2 = cr._obj2 ? cr._objName2 : "Wipe Tower";
        log_plate_message("[Post-processing]", "ERROR", plate_id,
            "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2
            + "\" at Z=" + std::to_string(cr._height) + "mm.");
        has_postprocess_error = true;
        Issue issue;
        issue.level = "error";
        issue.plate_id = plate_id;
        issue.object_name = obj1 + " vs " + obj2;
        issue.z_height = cr._height;
        issue.message = "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2 + "\" at Z=" + std::to_string(cr._height) + "mm";
        result.issues.push_back(issue);
    }

    // Bed/filament compatibility
    if (!result.gcode_result.bed_match_result.match) {
        const auto& bm = result.gcode_result.bed_match_result;
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Filament " + std::to_string(bm.extruder_id + 1)
            + " is not compatible with bed type \"" + bm.bed_type_name + "\".");
        has_postprocess_warning = true;
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "Filament " + std::to_string(bm.extruder_id + 1)
            + " is not compatible with bed type \"" + bm.bed_type_name + "\"";
        result.issues.push_back(issue);
    }

    // Timelapse warnings
    if (result.gcode_result.timelapse_warning_code & 1) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Timelapse is not supported in spiral vase mode on this printer.");
        has_postprocess_warning = true;
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "Timelapse is not supported in spiral vase mode on this printer";
        result.issues.push_back(issue);
    }
    if ((result.gcode_result.timelapse_warning_code >> 1) & 1) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Timelapse is not supported with by-object print sequence on this printer.");
        has_postprocess_warning = true;
        Issue issue;
        issue.level = "warning";
        issue.plate_id = plate_id;
        issue.z_height = -1.0;
        issue.message = "Timelapse is not supported with by-object print sequence on this printer";
        result.issues.push_back(issue);
    }

    // Slice warnings
    for (const auto& w : result.gcode_result.warnings) {
        if (w.error_code == "1000C001") continue; // bed temp warning irrelevant for cloud slicing
        if (w.level >= 2) {
            log_plate_message("[Post-processing]", "ERROR", plate_id,
                w.msg + " (code: " + w.error_code + ")");
            has_postprocess_error = true;
            Issue issue;
            issue.level = "error";
            issue.plate_id = plate_id;
            issue.z_height = -1.0;
            issue.code = w.error_code;
            issue.message = w.msg + " (code: " + w.error_code + ")";
            result.issues.push_back(issue);
        } else if (w.level == 1) {
            log_plate_message("[Post-processing]", "WARNING", plate_id,
                w.msg + " (code: " + w.error_code + ")");
            has_postprocess_warning = true;
            Issue issue;
            issue.level = "warning";
            issue.plate_id = plate_id;
            issue.z_height = -1.0;
            issue.code = w.error_code;
            issue.message = w.msg + " (code: " + w.error_code + ")";
            result.issues.push_back(issue);
        } else {
            log_plate_message("[Post-processing]", "TIP", plate_id, w.msg);
            Issue issue;
            issue.level = "tip";
            issue.plate_id = plate_id;
            issue.z_height = -1.0;
            issue.code = w.error_code;
            issue.message = w.msg;
            result.issues.push_back(issue);
        }
    }

    result.has_postprocess_warning = has_postprocess_warning;
    if (has_postprocess_warning)
        m_any_postprocess_warning = true;
    if (has_postprocess_error)
        m_any_error = true;
}

// ============================================================================
// Stage 4: Package output as gcode.3mf
// ============================================================================

void SliceEngine::package_output() {
    BOOST_LOG_TRIVIAL(info) << "Creating gcode.3mf package...";

    std::string printer_model_id;
    std::string nozzle_diameters_str;

    if (m_config.has("printer_model"))
        printer_model_id = m_config.opt_string("printer_model");

    if (m_config.has("nozzle_diameter")) {
        auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
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

    const ConfigOptionStrings* filament_types = nullptr;
    const ConfigOptionStrings* filament_colors = nullptr;
    const ConfigOptionStrings* filament_ids = nullptr;

    if (m_config.has("filament_type"))
        filament_types = m_config.option<ConfigOptionStrings>("filament_type");
    if (m_config.has("filament_colour"))
        filament_colors = m_config.option<ConfigOptionStrings>("filament_colour");
    if (m_config.has("filament_ids"))
        filament_ids = m_config.option<ConfigOptionStrings>("filament_ids");

    for (auto& pd : m_plate_data) {
        auto it = m_plate_results.find(pd->plate_index);
        if (it == m_plate_results.end()) continue;

        PlateSliceResult& result = it->second;
        pd->gcode_file = result.gcode_path;
        pd->is_sliced_valid = true;
        pd->printer_model_id = printer_model_id;
        pd->nozzle_diameters = nozzle_diameters_str;

        auto& modes = result.gcode_result.print_statistics.modes;
        int print_time = (int)modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        pd->gcode_prediction = std::to_string(print_time);

        if (result.total_weight != 0.0) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << result.total_weight;
            pd->gcode_weight = ss.str();
        }

        pd->toolpath_outside = result.gcode_result.toolpath_outside;
        pd->timelapse_warning_code = result.gcode_result.timelapse_warning_code;
        pd->is_support_used = result.support_used;
        pd->is_label_object_enabled = result.gcode_result.label_object_enabled;

        pd->parse_filament_info(&result.gcode_result);

        for (auto& info : pd->slice_filaments_info) {
            size_t idx = static_cast<size_t>(info.id);
            if (filament_types && idx < filament_types->values.size())
                info.type = filament_types->values[idx];
            if (filament_colors && idx < filament_colors->values.size())
                info.color = filament_colors->values[idx];
            if (filament_ids && idx < filament_ids->values.size())
                info.filament_id = filament_ids->values[idx];
        }

        // Rebuild objects_and_instances using model.objects array indices
        std::set<int> plate_identify_ids;
        for (const auto& entry : pd->obj_inst_map)
            plate_identify_ids.insert(entry.second.second);

        pd->objects_and_instances.clear();
        for (size_t obj_idx = 0; obj_idx < m_model.objects.size(); ++obj_idx) {
            const ModelObject* obj = m_model.objects[obj_idx];
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

    StoreParams params;
    params.path = m_output_path.c_str();
    params.plate_data_list = m_plate_data;
    params.model = &m_model;
    params.config = &m_config;
    params.project_presets = m_project_presets;

    if (m_cfg.single_plate)
        params.export_plate_idx = m_cfg.plate_id - 1;
    else
        params.export_plate_idx = -1;

    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel |
                     SaveStrategy::WithGcode | SaveStrategy::SkipModel |
                     SaveStrategy::Zip64;

    std::vector<ThumbnailData*> thumbnail_data;
    std::vector<ThumbnailData*> no_light_thumbnail_data;
    std::vector<ThumbnailData*> top_thumbnail_data;
    std::vector<ThumbnailData*> pick_thumbnail_data;
    std::vector<ThumbnailData*> calibration_thumbnail_data;
    std::vector<PlateBBoxData*> id_bboxes;
    std::vector<std::unique_ptr<PlateBBoxData>> id_bboxes_owned;
    id_bboxes_owned.reserve(m_plate_data.size());
    for (size_t i = 0; i < m_plate_data.size(); ++i) {
        id_bboxes_owned.push_back(std::make_unique<PlateBBoxData>());
        id_bboxes.push_back(id_bboxes_owned.back().get());
    }

    params.thumbnail_data = thumbnail_data;
    params.no_light_thumbnail_data = no_light_thumbnail_data;
    params.top_thumbnail_data = top_thumbnail_data;
    params.pick_thumbnail_data = pick_thumbnail_data;
    params.calibration_thumbnail_data = calibration_thumbnail_data;
    params.id_bboxes = id_bboxes;
    params.project = nullptr;
    params.profile = nullptr;

    try {
        bool success = store_bbs_3mf(params);
        if (!success) {
            BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package";
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << m_output_path;
    }
    catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
    }
}

// ============================================================================
// Stage 5: Build statistics for JSON output
// ============================================================================

void SliceEngine::build_statistics() {
    for (const auto& [plate_id, result] : m_plate_results) {
        SliceOutputStats::PlateStats plate_stats;
        plate_stats.plate_id = plate_id;

        bool plate_has_error = false;
        bool plate_has_warning = false;
        for (const auto& issue : result.issues) {
            if (issue.level == "error") plate_has_error = true;
            if (issue.level == "warning") plate_has_warning = true;
            m_stats.issues.push_back(issue);
        }

        plate_stats.success = !plate_has_error;
        if (plate_has_error)
            m_any_error = true;
        if (plate_has_warning || result.has_postprocess_warning)
            m_any_postprocess_warning = true;

        plate_stats.issues = result.issues;

        if (plate_stats.success) {
            plate_stats.gcode_file = m_output_path;

            auto& modes = result.gcode_result.print_statistics.modes;
            auto& normal_mode = modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
            plate_stats.total_time = normal_mode.time;
            plate_stats.prepare_time = normal_mode.prepare_time;
            plate_stats.print_time = normal_mode.time - normal_mode.prepare_time;

            plate_stats.total_filament_m = result.total_used_filament;
            plate_stats.total_filament_g = result.total_weight;
            plate_stats.total_cost = result.total_cost;
            plate_stats.support_used = result.support_used;
            plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;

            // Calculate model filament
            double total_support_m = 0.0, total_support_g = 0.0;
            double total_flush_m = 0.0, total_flush_g = 0.0;
            double total_wipe_tower_m = 0.0, total_wipe_tower_g = 0.0;

            const auto& fd = result.gcode_result.filament_diameters;
            const auto& fdens = result.gcode_result.filament_densities;

            for (const auto& [extruder_id, volume] : result.filament_volumes) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double diameter = fd[extruder_id];
                    double density = fdens[extruder_id];
                    double cross_section = M_PI * 0.25 * diameter * diameter;
                    double used_m = (volume / cross_section) * 0.001;
                    double used_g = volume * density * 0.001;
                    plate_stats.filament_used_m[extruder_id] = used_m;
                    plate_stats.filament_used_g[extruder_id] = used_g;
                }
            }

            auto& ps = result.gcode_result.print_statistics;
            for (const auto& [extruder_id, volume] : ps.support_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_support_m += (volume / cross_section) * 0.001;
                    total_support_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.wipe_tower_volumes_per_extruder) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_wipe_tower_m += (volume / cross_section) * 0.001;
                    total_wipe_tower_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            for (const auto& [extruder_id, volume] : ps.flush_per_filament) {
                if (extruder_id < fd.size() && extruder_id < fdens.size()) {
                    double cross_section = M_PI * 0.25 * fd[extruder_id] * fd[extruder_id];
                    total_flush_m += (volume / cross_section) * 0.001;
                    total_flush_g += volume * fdens[extruder_id] * 0.001;
                }
            }

            plate_stats.model_filament_m = plate_stats.total_filament_m - total_support_m - total_flush_m - total_wipe_tower_m;
            plate_stats.model_filament_g = plate_stats.total_filament_g - total_support_g - total_flush_g - total_wipe_tower_g;

            if (m_config.has("nozzle_diameter")) {
                auto nozzle_opt = m_config.option<ConfigOptionFloats>("nozzle_diameter");
                if (nozzle_opt)
                    plate_stats.nozzle_diameters = nozzle_opt->values;
            }

            plate_stats.plate_count = static_cast<int>(m_plate_data.size());

            const ConfigOptionStrings* ftypes =
                m_config.has("filament_type") ? m_config.option<ConfigOptionStrings>("filament_type") : nullptr;
            const ConfigOptionStrings* fcolors =
                m_config.has("filament_colour") ? m_config.option<ConfigOptionStrings>("filament_colour") : nullptr;

            for (const auto& [extruder_id, used_g] : plate_stats.filament_used_g) {
                SliceOutputStats::FilamentDetail detail;
                detail.filament_id = extruder_id;
                detail.used_g = used_g;

                if (ftypes && extruder_id < static_cast<int>(ftypes->values.size()))
                    detail.type = ftypes->values[extruder_id];
                else
                    detail.type = "Unknown";

                if (fcolors && extruder_id < static_cast<int>(fcolors->values.size()))
                    detail.color = fcolors->values[extruder_id];
                else
                    detail.color = "#000000";

                plate_stats.filament_details.push_back(detail);
            }

            plate_stats.model_thumbnail = "";
        } else {
            plate_stats.plate_count = static_cast<int>(m_plate_data.size());
        }

        m_stats.plates.push_back(plate_stats);
    }

    // Add placeholder plates for plates with issues but not in plate_results
    std::set<int> plates_with_results;
    for (const auto& p : m_stats.plates)
        plates_with_results.insert(p.plate_id);

    for (const auto& issue : m_stats.issues) {
        if (issue.plate_id >= 0 && plates_with_results.find(issue.plate_id) == plates_with_results.end()) {
            bool found = false;
            for (const auto& p : m_stats.plates) {
                if (p.plate_id == issue.plate_id) { found = true; break; }
            }
            if (!found) {
                SliceOutputStats::PlateStats failed_plate;
                failed_plate.plate_id = issue.plate_id;
                failed_plate.success = false;
                failed_plate.plate_count = static_cast<int>(m_plate_data.size());
                failed_plate.issues.push_back(issue);
                plates_with_results.insert(issue.plate_id);
                m_stats.plates.push_back(failed_plate);
            } else {
                for (auto& p : m_stats.plates) {
                    if (p.plate_id == issue.plate_id) {
                        p.issues.push_back(issue);
                        break;
                    }
                }
            }
        }
    }

    // Sort plates by plate_id
    std::sort(m_stats.plates.begin(), m_stats.plates.end(),
        [](const SliceOutputStats::PlateStats& a, const SliceOutputStats::PlateStats& b) {
            return a.plate_id < b.plate_id;
        });

    // Determine global success
    m_stats.success = !m_stats.plates.empty();
    for (const auto& p : m_stats.plates) {
        if (!p.success) {
            m_stats.success = false;
            break;
        }
    }
    if (!m_stats.success && m_stats.error_message.empty()) {
        if (m_stats.plates.empty())
            m_stats.error_message = "No plates completed successfully";
        else {
            int failed_count = 0;
            for (const auto& p : m_stats.plates)
                if (!p.success) ++failed_count;
            if (failed_count == static_cast<int>(m_stats.plates.size()))
                m_stats.error_message = "All plates failed with errors";
            else
                m_stats.error_message = "Some plates failed with errors";
        }
    }
}
