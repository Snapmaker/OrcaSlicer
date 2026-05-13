#include "SliceEngine.hpp"
#include "GeometryCheck.hpp"
#include "Utils.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

#include <boost/log/trivial.hpp>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/CustomGCode.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "libslic3r/BoundingBox.hpp"

constexpr int MAX_RETRIES = 3;

using namespace Slic3r;

namespace {

// Bed3D::Axes::DefaultTipRadius, used in plate size calculation
const double BED_AXES_TIP_RADIUS = 1.25;

// 1/5, same as GUI's LOGICAL_PART_PLATE_GAP
const double LOGICAL_PART_PLATE_GAP = 0.2;

// Check if a plate result indicates a wipe tower tool change mismatch.
// CGAL/float differences on some platforms cause non-consecutive extruder
// ID handling to fail during G-code export.
bool is_wipe_tower_error(const PlateSliceResult& result) {
    for (const auto& iss : result.issues) {
        if (iss.message.find("append_tcr") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

SliceEngine::SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files)
    : m_cfg(cfg)
    , m_temp_files(temp_files)
{
}

bool SliceEngine::run() {
    bool load_ok = load_3mf();
    if (!load_ok) {
        build_statistics();
        return false;
    }

    // Config & preset validation (desktop parity — non-blocking)
    validate_config();
    load_system_presets();
    validate_presets();

    // Replace user-supplied parameters with official system presets for cloud safety
    if (m_cfg.enforce_official_presets)
        apply_official_presets();

    bool validate_ok = validate_input();

    if (load_ok && validate_ok) {
        // --- Setup timeout deadline ---
        m_has_timeout = (m_cfg.timeout_seconds > 0);
        if (m_has_timeout) {
            m_timeout_deadline = std::chrono::steady_clock::now()
                               + std::chrono::seconds(m_cfg.timeout_seconds);
            BOOST_LOG_TRIVIAL(info) << "Slicing timeout set to "
                                    << m_cfg.timeout_seconds << "s";
        }

        try {
        // --- Geometry defect detection (once for entire model, before per-plate loop) ---
        {
            auto geom_issues = run_geometry_checks(m_model);
            bool has_geom_error = false;
            for (auto& issue : geom_issues) {
                if (issue.level == "error")
                    has_geom_error = true;
                m_stats.issues.push_back(std::move(issue));
            }
            if (has_geom_error) {
                m_any_error = true;
                set_error_type(EXIT_VALIDATION_ERROR);
                m_stats.error_message = "模型文件中检测到几何缺陷（非流形/自相交/零体积），请修复后重新上传";
                BOOST_LOG_TRIVIAL(error) << m_stats.error_message;
                build_statistics();
                return false;
            }
        }

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

        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Unhandled exception in slicing pipeline: " << e.what();
            std::cerr << "[ERROR] An unexpected internal error occurred during slicing." << std::endl;
            m_any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            m_stats.issues.push_back(make_error(-1, "INTERNAL_ERROR",
                "An unexpected internal error occurred during slicing. Please try again or contact support."));
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "Unhandled non-standard exception in slicing pipeline";
            std::cerr << "[ERROR] An unexpected internal error occurred." << std::endl;
            m_any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            m_stats.issues.push_back(make_error(-1, "INTERNAL_ERROR",
                "Unhandled unknown exception in slicing pipeline"));
        }
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
    // --- Pre-load validation: format & size ---

    // Extension check
    std::string ext = boost::filesystem::extension(m_cfg.input_file);
    if (ext != ".3mf") {
        std::string msg = "仅支持上传 .3mf 格式的打印配置文件，请使用 Snapmaker Orca Slicer 导出";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "FORMAT_REJECTED", msg));
        return false;
    }

    // File size check (200MB default)
    constexpr boost::uintmax_t MAX_FILE_SIZE = 200ULL * 1024 * 1024;
    boost::system::error_code ec;
    boost::uintmax_t file_size = boost::filesystem::file_size(m_cfg.input_file, ec);
    if (!ec && file_size > MAX_FILE_SIZE) {
        std::string msg = "文件大小超过限制 (" + std::to_string(MAX_FILE_SIZE / 1024 / 1024)
                        + "MB)，请简化模型或减少面数后重试";
        BOOST_LOG_TRIVIAL(error) << msg;
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = msg;
        m_stats.issues.push_back(make_error(-1, "FILE_SIZE_EXCEEDED", msg));
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Loading 3MF file...";

    // Reset substitution state from any prior run
    m_config_substitutions = ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule::Enable);

    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                           LoadStrategy::AddDefaultInstances | LoadStrategy::LoadAuxiliary;

    try {
        m_model = Model::read_from_file(
            m_cfg.input_file,
            &m_config,
            &m_config_substitutions,
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
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load 3MF file: " << e.what();
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = "Failed to load 3MF file. The file may be corrupted or in an unsupported format.";
        m_stats.issues.push_back(make_error(-1, "LOAD_3MF_ERROR", m_stats.error_message));
        return false;
    }

    if (m_model.objects.empty()) {
        BOOST_LOG_TRIVIAL(error) << "No objects found in 3MF file";
        m_any_error = true;
        set_error_type(EXIT_LOAD_ERROR);
        m_stats.error_message = "3MF file contains no sliceable model objects";
        m_stats.issues.push_back(make_error(-1, "MODEL_EMPTY", m_stats.error_message));
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Loaded " << m_model.objects.size() << " object(s)";
    BOOST_LOG_TRIVIAL(info) << "3MF version: " << m_file_version.to_string();

    // Detect and reject post-processing scripts in cloud mode (RCE prevention)
    if (m_config.has("post_process")) {
        auto* pp = m_config.option<ConfigOptionStrings>("post_process", true);
        if (pp && !pp->values.empty()) {
            m_stats.issues.push_back(make_error(-1, "POST_PROCESS_REJECTED",
                "自定义后处理脚本在云切片中不被支持"));
            m_config.set_key_value("post_process", new ConfigOptionStrings({}));
        }
    }

    return true;
}

// ============================================================================
// Stage 2: Config & preset validation (desktop parity)
// ============================================================================

void SliceEngine::validate_config()
{
    // A1: Validate config values (layer_height, nozzle_diameter, etc.)
    std::map<std::string, std::string> invalid = m_config.validate(true);
    for (const auto& [key, msg] : invalid) {
        BOOST_LOG_TRIVIAL(warning) << "Config validation: " << key << " - " << msg;
        m_stats.issues.push_back(make_error(-1, "CONFIG_INVALID_" + key, msg));
    }

    // A2: Check config substitutions (unknown keys, forward-compat changes)
    if (!m_config_substitutions.empty()) {
        for (const auto& sub : m_config_substitutions.substitutions) {
            const char* key = sub.opt_def ? sub.opt_def->opt_key.c_str() : "?";
            BOOST_LOG_TRIVIAL(warning)
                << "Config substitution: key=" << key
                << " old_value=" << sub.old_value;
            m_stats.issues.push_back(make_warning(-1, "CONFIG_SUBSTITUTION",
                std::string("Config key '") + key
                + "' value was substituted (old: " + sub.old_value + ")"));
        }

        for (const auto& key : m_config_substitutions.unrecogized_keys) {
            BOOST_LOG_TRIVIAL(warning) << "Unrecognized config key in 3MF: " << key;
            m_stats.issues.push_back(make_warning(-1, "CONFIG_UNRECOGNIZED",
                std::string("Unrecognized config key '") + key
                + "' — may be from a newer slicer version"));
        }
    }
}

void SliceEngine::load_system_presets()
{
    // Determine profiles directory: --data-dir takes precedence,
    // otherwise derive from resources_dir/profiles/
    std::string profiles_path;
    if (!m_cfg.data_dir.empty()) {
        profiles_path = m_cfg.data_dir;
    } else {
        const std::string res_dir = Slic3r::resources_dir();
        if (res_dir.empty()) {
            BOOST_LOG_TRIVIAL(info) << "No resources directory set; skipping preset validation";
            return;
        }
        profiles_path = res_dir + "/profiles";
    }

    boost::filesystem::path profiles_dir(profiles_path);
    if (!boost::filesystem::exists(profiles_dir) ||
        !boost::filesystem::is_directory(profiles_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Profiles directory not found: " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Collect vendor JSON files
    std::vector<std::string> vendor_names;
    for (auto& entry : boost::filesystem::directory_iterator(profiles_dir)) {
        std::string file = entry.path().string();
        if (!Slic3r::is_json_file(file))
            continue;
        std::string name = entry.path().filename().string();
        name.erase(name.size() - 5); // strip .json
        vendor_names.push_back(name);
    }

    if (vendor_names.empty()) {
        BOOST_LOG_TRIVIAL(warning)
            << "No vendor JSON files in " << profiles_dir.string()
            << "; skipping preset validation";
        return;
    }

    // Move OrcaFilamentLibrary to the front (cross-vendor filament inheritance)
    for (size_t i = 0; i < vendor_names.size(); ++i) {
        if (vendor_names[i] == PresetBundle::ORCA_FILAMENT_LIBRARY) {
            std::swap(vendor_names[0], vendor_names[i]);
            break;
        }
    }

    try {
        m_preset_bundle = std::make_unique<Slic3r::PresetBundle>();

        // In validation mode, load_vendor_configs_from_json reads presets
        // into this PresetBundle. We load all vendors directly (no merge_presets
        // needed — duplicate detection is not critical for cloud validation).
        const auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;

        for (size_t i = 0; i < vendor_names.size(); ++i) {
            const std::string& vendor = vendor_names[i];
            // First vendor: no base_bundle. Subsequent: pass this bundle for
            // cross-vendor preset inheritance resolution.
            const PresetBundle* base = (i == 0) ? nullptr : m_preset_bundle.get();
            m_preset_bundle->load_vendor_configs_from_json(
                profiles_dir.string(), vendor,
                PresetBundle::LoadSystem, rule, base);
        }

        m_presets_available = true;
        BOOST_LOG_TRIVIAL(info)
            << "System presets loaded from " << vendor_names.size() << " vendor(s)";

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Failed to load system presets: " << e.what()
            << "; preset validation skipped";
        m_preset_bundle.reset();
        m_presets_available = false;
    }
}

void SliceEngine::validate_presets()
{
    if (!m_presets_available || !m_preset_bundle) {
        BOOST_LOG_TRIVIAL(info) << "Preset validation skipped (system presets not available)";
        return;
    }

    PresetBundle& bundle = *m_preset_bundle;

    // B2: Load project embedded presets
    if (!m_project_presets.empty()) {
        try {
            PresetsConfigSubstitutions preset_subs =
                bundle.load_project_embedded_presets(
                    m_project_presets,
                    ForwardCompatibilitySubstitutionRule::Enable);

            for (const auto& ps : preset_subs) {
                for (const auto& sub : ps.substitutions) {
                    const char* key = sub.opt_def ? sub.opt_def->opt_key.c_str() : "?";
                    m_stats.issues.push_back(make_warning(-1, "PRESET_SUBSTITUTION",
                        std::string("Embedded preset '") + ps.preset_name
                        + "' key '" + key + "' was substituted"));
                }
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning)
                << "Failed to load project embedded presets: " << e.what();
        }
    }

    // B3: Validate presets against system profiles
    try {
        std::set<std::string> modified_gcodes;
        int validated = bundle.validate_presets(
            m_cfg.input_file, m_config, modified_gcodes);

        switch (validated) {
        case VALIDATE_PRESETS_SUCCESS:
            BOOST_LOG_TRIVIAL(info) << "Preset validation passed";
            break;

        case VALIDATE_PRESETS_PRINTER_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            BOOST_LOG_TRIVIAL(warning)
                << "Preset validation: printer not found in system";
            std::string msg = "Custom printer preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_PRINTER_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_FILAMENTS_NOT_FOUND: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            BOOST_LOG_TRIVIAL(warning)
                << "Preset validation: filament not found in system";
            std::string msg = "Custom filament preset not found in system presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_FILAMENT_NOT_FOUND", msg));
            break;
        }

        case VALIDATE_PRESETS_MODIFIED_GCODES: {
            std::string details;
            for (const auto& name : modified_gcodes)
                details += (details.empty() ? "" : ", ") + name;
            BOOST_LOG_TRIVIAL(warning)
                << "Preset validation: modified G-codes detected";
            std::string msg = "Modified G-code keys found in presets";
            if (!details.empty())
                msg += ": " + details;
            m_stats.issues.push_back(make_warning(-1, "PRESET_MODIFIED_GCODES", msg));
            break;
        }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning)
            << "Preset validation error: " << e.what();
    }
}

void SliceEngine::apply_official_presets()
{
    // Strip all custom G-code blocks — cloud slicing must not execute
    // or embed user-supplied G-code for safety and consistency.
    static const std::vector<const char*> gcode_keys = {
        "start_gcode", "end_gcode", "layer_gcode",
        "machine_start_gcode", "machine_end_gcode",
        "before_layer_change_gcode", "between_objects_gcode",
        "toolchange_gcode", "print_host",
    };
    for (const char* key : gcode_keys) {
        if (m_config.has(key)) {
            m_config.set_key_value(key, new ConfigOptionString(""));
            m_stats.issues.push_back(make_tip(-1, "GCODE_CLEARED",
                std::string("Custom G-code '") + key + "' cleared for cloud safety"));
        }
    }

    // Diagnostic: log all parameters that differ from desktop defaults
    {
        std::ostringstream diag;
        diag << "=== Post-apply_official_presets diagnostic ===";

        // Key parameters affecting print time / filament usage
        auto log_float = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionFloat*>(m_config.option(key));
                if (o) diag << " " << key << "=" << o->value;
            }
        };
        auto log_ints = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionInts*>(m_config.option(key));
                if (o) {
                    diag << " " << key << "=[";
                    for (size_t i = 0; i < o->values.size(); ++i) {
                        if (i) diag << ",";
                        diag << o->values[i];
                    }
                    diag << "]";
                }
            }
        };
        auto log_percent = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionPercent*>(m_config.option(key));
                if (o) diag << " " << key << "=" << o->value;
            }
        };
        auto log_int = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionInt*>(m_config.option(key));
                if (o) diag << " " << key << "=" << o->value;
            }
        };
        auto log_bool = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionBool*>(m_config.option(key));
                if (o) diag << " " << key << "=" << (o->value ? "true" : "false");
            }
        };
        auto log_floats = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionFloats*>(m_config.option(key));
                if (o) {
                    diag << " " << key << "=[";
                    for (size_t i = 0; i < o->values.size(); ++i) {
                        if (i) diag << ",";
                        diag << o->values[i];
                    }
                    diag << "]";
                }
            }
        };
        auto log_string = [&](const char* key) {
            if (m_config.has(key)) {
                auto* o = dynamic_cast<ConfigOptionString*>(m_config.option(key));
                if (o && !o->value.empty())
                    diag << " " << key << "=\"" << o->value << "\"";
            }
        };

        // G-code blocks (empty = cleared)
        for (const char* k : gcode_keys)
            log_string(k);

        // Temperature
        log_ints("nozzle_temperature");
        log_ints("bed_temperature");

        // Fill / wall
        log_percent("sparse_infill_density");
        log_int("wall_loops");

        // Layer
        log_float("layer_height");

        // Speed / acceleration / jerk (affect print time)
        log_float("outer_wall_speed");
        log_float("inner_wall_speed");
        log_float("sparse_infill_speed");
        log_float("internal_solid_infill_speed");
        log_float("top_surface_speed");
        log_float("travel_speed");
        log_float("default_acceleration");
        log_float("travel_acceleration");
        log_float("default_jerk");

        // Wipe tower / flush volumes (affect filament usage)
        log_bool("enable_prime_tower");
        log_floats("flush_volumes_matrix");
        log_floats("wiping_volumes_extruders");
        log_float("prime_tower_width");
        log_float("prime_volume");

        // Z hop (spiral lift)
        if (m_config.has("z_hop_types")) {
            auto* o = m_config.option<ConfigOptionEnumsGeneric>("z_hop_types");
            if (o) {
                diag << " z_hop_types=[";
                for (size_t i = 0; i < o->values.size(); ++i) {
                    if (i) diag << ",";
                    diag << o->values[i];
                }
                diag << "]";
            }
        }

        // Print sequence
        if (m_config.has("print_sequence")) {
            auto* o = m_config.option<ConfigOptionEnum<PrintSequence> >("print_sequence");
            if (o) diag << " print_sequence=" << static_cast<int>(o->value);
        }

        // Extruder clearance
        log_float("extruder_clearance_radius");
        log_float("extruder_clearance_height_to_lid");
        log_float("extruder_clearance_height_to_rod");

        diag << " ===";
        BOOST_LOG_TRIVIAL(info) << diag.str();
    }
}

// ============================================================================
// Stage 3: Validate input
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
            set_error_type(EXIT_VALIDATION_ERROR);
            m_stats.error_message = "Requested plate " + std::to_string(m_cfg.plate_id) + " not found in 3MF file";
            m_stats.issues.push_back(make_error(-1, "PLATE_NOT_FOUND", m_stats.error_message));
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

    // Calculate plate dimensions (computed once, reused per attempt)
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

    Vec3d origin = setup_print_origin(plate_id, plate_width, plate_depth);

    // Get BBL vendor flag once
    bool is_bbl = (m_presets_available && m_preset_bundle)
        ? m_preset_bundle->is_bbl_vendor() : false;

    // --- Slicing + Export (with retry for non-fatal exceptions) ---
    // Each attempt creates a fresh Print to avoid explicit dtor + placement-new
    // on the wipe-tower-disabled retry path.
    bool wipe_tower_disabled = false;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        // Check timeout before each attempt
        if (m_has_timeout && std::chrono::steady_clock::now() > m_timeout_deadline) {
            BOOST_LOG_TRIVIAL(error) << "Slicing timed out for plate " << (plate_id + 1);
            m_stats.issues.push_back(make_error(plate_id, "SLICING_TIMEOUT",
                "切片超时，模型可能过于复杂。如果您有异议，可点击申诉提交复审。"));
            m_any_error = true;
            set_error_type(EXIT_SLICING_ERROR);
            return;
        }

        if (attempt > 1) {
            BOOST_LOG_TRIVIAL(warning) << "Retry attempt " << attempt << "/" << MAX_RETRIES
                << " for plate " << (plate_id + 1);
            // Clear error issues from the previous failed attempt
            m_stats.issues.erase(
                std::remove_if(m_stats.issues.begin(), m_stats.issues.end(),
                    [&](const Issue& i) { return i.plate_id == plate_id && i.level == "error"; }),
                m_stats.issues.end());
            m_any_error = false;
            m_error_type = EXIT_OK;
        }

        // --- Create fresh Print for this attempt ---
        Print print;
        print.set_status_callback([&print, this](const PrintBase::SlicingStatus& s) {
            default_status_callback(s, &print, &m_cfg.cancel_file);
        });
        print.is_BBL_printer() = is_bbl;

        // --- Apply model ---
        if (!apply_model(plate_id, print, origin))
            return;

        // --- Assign arrange_order ---
        {
            int order = 1;
            for (ModelObject* obj : m_model.objects)
                for (ModelInstance* inst : obj->instances)
                    inst->arrange_order = order++;
        }

        // --- Set global extruder params & speed table ---
        {
            int num_extruders = 0;
            if (m_config.has("filament_diameter")) {
                auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
                if (fd) num_extruders = static_cast<int>(fd->values.size());
            }
            Model::setExtruderParams(m_config, num_extruders);
            Model::setPrintSpeedTable(m_config, print.config());
        }

        // --- Validation ---
        if (!run_validation(plate_id, print))
            return;

        // Slicing
        if (!run_slicing(plate_id, print)) {
            BOOST_LOG_TRIVIAL(warning) << "Slicing failed for plate " << (plate_id + 1)
                << " on attempt " << attempt;
            continue;
        }

        BOOST_LOG_TRIVIAL(info) << "Slicing completed for plate " << (plate_id + 1);

        // Export G-code
        PlateSliceResult slice_result;
        if (!export_gcode(plate_id, print, slice_result)) {
            // Check if this is a wipe tower tool change mismatch.
            // On the next iteration a fresh Print is created with the updated config.
            bool is_wt_error = is_wipe_tower_error(slice_result);

            if (is_wt_error && !wipe_tower_disabled) {
                BOOST_LOG_TRIVIAL(warning) << "Wipe tower tool change mismatch on plate "
                    << (plate_id + 1) << " — disabling wipe tower and re-slicing";
                wipe_tower_disabled = true;
                m_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
                continue;
            }

            BOOST_LOG_TRIVIAL(warning) << "G-code export failed for plate " << (plate_id + 1)
                << " on attempt " << attempt;
            slice_result.issues.clear();
            continue;
        }

        // Success
        run_postprocessing(plate_id, slice_result);
        m_plate_results[plate_id] = slice_result;
        return;
    }

    // All retries exhausted
    BOOST_LOG_TRIVIAL(error) << "Slicing/export failed for plate " << (plate_id + 1)
        << " after " << MAX_RETRIES << " attempts";
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
                m_stats.issues.push_back(make_error(plate_id, "BUILD_VOLUME_PARTLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is placed on the boundary of or exceeds the build volume",
                    obj->name));
            } else if (inst->print_volume_state == ModelInstancePVS_Fully_Outside) {
                log_plate_message("[Pre-processing]", "WARNING", plate_id,
                    "Object \"" + obj->name + "\" is completely outside the build volume and will not be printed.");
                m_stats.issues.push_back(make_warning(plate_id, "BUILD_VOLUME_FULLY_OUTSIDE",
                    "Object \"" + obj->name + "\" is completely outside the build volume and will not be printed",
                    obj->name));
            }
        }
    }

    // Snapmaker: SpiralLiftNearBoundary warning (matches desktop 3DScene.cpp:1105-1122)
    {
        bool spiral_lift_active = false;
        if (m_config.has("z_hop_types")) {
            auto* zht_opt = m_config.option<ConfigOptionEnumsGeneric>("z_hop_types");
            if (zht_opt) {
                for (int v : zht_opt->values) {
                    // zhtSpiral or zhtAuto (which forces SpiralLift on layer change)
                    if (v == static_cast<int>(ZHopType::zhtSpiral) ||
                        v == static_cast<int>(ZHopType::zhtAuto)) {
                        spiral_lift_active = true;
                        break;
                    }
                }
            }
        }
        if (spiral_lift_active && build_volume.type() == BuildVolume_Type::Rectangle) {
            constexpr double SPIRAL_LIFT_SAFETY_MARGIN = 3.5; // mm
            const BoundingBoxf3& bed_bb = build_volume.bounding_volume();
            std::set<std::string> warned_objects;
            for (ModelObject* obj : m_model.objects) {
                if (!obj->printable) continue;
                for (size_t idx = 0; idx < obj->instances.size(); ++idx) {
                    ModelInstance* inst = obj->instances[idx];
                    if (!inst->printable || inst->print_volume_state != ModelInstancePVS_Inside) continue;
                    BoundingBoxf3 bb = obj->instance_bounding_box(idx);
                    double dist_left   = std::abs(bb.min.x() - bed_bb.min.x());
                    double dist_right  = std::abs(bed_bb.max.x() - bb.max.x());
                    double dist_bottom = std::abs(bb.min.y() - bed_bb.min.y());
                    double dist_top    = std::abs(bed_bb.max.y() - bb.max.y());
                    double min_dist    = std::min({dist_left, dist_right, dist_bottom, dist_top});
                    if (min_dist < SPIRAL_LIFT_SAFETY_MARGIN) {
                        if (warned_objects.insert(obj->name).second) {
                            log_plate_message("[Pre-processing]", "WARNING", plate_id,
                                "Object \"" + obj->name + "\" is too close to bed boundary. "
                                "Disable spiral lifting or keep at least 3.5mm gap to avoid collision.");
                            m_stats.issues.push_back(make_warning(plate_id,
                                "SPIRAL_LIFT_NEAR_BOUNDARY",
                                "Model too close to bed boundary. "
                                "Disable spiral lifting or keep at least 3.5mm gap to avoid collision.",
                                obj->name));
                        }
                    }
                }
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
        set_error_type(EXIT_VALIDATION_ERROR);
        return false;
    }
    return true;
}

Vec3d SliceEngine::setup_print_origin(int plate_id, double plate_width, double plate_depth) {
    // Compute plate origin using the same grid layout formula as the desktop GUI
    // (PartPlate::update_plate_layout_arrange). Each plate occupies a cell in a
    // row-major grid with LOGICAL_PART_PLATE_GAP spacing between plates.
    int total_plates = static_cast<int>(m_plate_data.size());
    int cols = compute_column_count(total_plates);
    int row = plate_id / cols;
    int col = plate_id % cols;

    double origin_x = col * (plate_width  * (1.0 + LOGICAL_PART_PLATE_GAP));
    double origin_y = -row * (plate_depth * (1.0 + LOGICAL_PART_PLATE_GAP));

    Vec3d origin(origin_x, origin_y, 0.0);
    BOOST_LOG_TRIVIAL(info) << "Plate " << plate_id << " origin: (" << origin_x << ", " << origin_y
        << ") grid[" << row << "," << col << "] / " << total_plates << " plate(s)"
        << ", dimensions: " << plate_width << " x " << plate_depth;
    return origin;
}

bool SliceEngine::apply_model(int plate_id, Print& print, const Vec3d& origin) {
    // plate_index from m_plate_data is already 0-based (import does -1 conversion)
    print.set_plate_index(plate_id);

    // Use the grid-layout-computed origin so object positions in gcode match
    // the desktop output (PartPlate::update_plate_layout_arrange).
    print.set_plate_origin(origin);

    // Guard against wipe tower / tool change mismatch.
    // If the model uses fewer extruders than filaments configured in the 3MF,
    // the wipe tower generates a tool change sequence that doesn't match actual
    // extrusion, causing "append_tcr was asked to do a toolchange it didn't expect".
    //
    // We must check multiple sources to avoid false positives on multi-color models:
    //   1. vol->get_extruders()  — per-volume assignment + MMU painting
    //   2. plates_custom_gcodes  — AMS per-layer ToolChange entries
    //   3. single_extruder_multi_material — non-Bambu single-extruder multi-material
    // Only trim when ALL sources agree the model is single-extruder.
    //
    // Work on a per-plate copy so extruder-count trimming does not leak
    // into subsequent plates (m_config is shared across the pipeline).
    DynamicPrintConfig merged_config = m_config;
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

        int num_filaments = 0;
        if (m_config.has("filament_diameter")) {
            auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
            if (fd) num_filaments = static_cast<int>(fd->values.size());
        }

        // If volumes suggest single extruder, also check plate-level ToolChange
        // custom G-code (AMS per-layer filament switching).
        if (used_extruders.size() <= 1 && num_filaments > 1) {
            auto it = m_model.plates_custom_gcodes.find(plate_id);
            if (it != m_model.plates_custom_gcodes.end()) {
                for (const auto& item : it->second.gcodes) {
                    if (item.type == CustomGCode::Type::ToolChange && item.extruder > 0)
                        used_extruders.insert(item.extruder);
                }
            }
        }

        // If still single, also check the single_extruder_multi_material config flag
        // (used by non-Bambu printers for single-nozzle multi-filament).
        if (used_extruders.size() <= 1 && num_filaments > 1) {
            auto* semm = m_config.option<ConfigOptionBool>("single_extruder_multi_material");
            if (semm && semm->value) {
                // Model genuinely uses multiple filaments through one extruder.
                // Insert sentinel values to prevent trimming.
                used_extruders.insert(1);
                used_extruders.insert(2);
            }
        }

        // Diagnostic: print each detected extruder and filament info
        {
            std::ostringstream oss;
            for (int eid : used_extruders) oss << eid << " ";
            BOOST_LOG_TRIVIAL(info) << "Extruder usage: model uses " << used_extruders.size()
                << " extruder(s) [" << oss.str() << "], config has " << num_filaments
                << " filament(s), has single_extruder_multi_material: "
                << (m_config.has("single_extruder_multi_material")
                    && m_config.option<ConfigOptionBool>("single_extruder_multi_material")->value);
            auto pcg_it = m_model.plates_custom_gcodes.find(plate_id);
            if (pcg_it != m_model.plates_custom_gcodes.end()) {
                int tc_count = 0;
                for (const auto& item : pcg_it->second.gcodes)
                    if (item.type == CustomGCode::Type::ToolChange) ++tc_count;
                BOOST_LOG_TRIVIAL(info) << "Plate " << plate_id
                    << " has " << pcg_it->second.gcodes.size() << " custom gcode entries ("
                    << tc_count << " ToolChange)";
            } else {
                BOOST_LOG_TRIVIAL(info) << "Plate " << plate_id << " has no custom gcode entries";
            }
        }

        if (used_extruders.size() <= 1 && num_filaments > 1) {
            BOOST_LOG_TRIVIAL(info) << "Trimming filament config from " << num_filaments
                << " to 1 to match single-extruder model";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));

            // Truncate all filament-related array options to 1 entry.
            // This prevents Print::has_wipe_tower() from returning true due to
            // filament_diameter.size() > 1, which is the root cause of the
            // "append_tcr was asked to do a toolchange it didn't expect" error.
            // flush_volumes_matrix (N*N flat vector) and wiping_volumes_extruders
            // must also be trimmed to keep the config internally consistent:
            // a 5*5 matrix with only 1 extruder would mismatch sqrt(size) later.
            static const std::vector<const char*> trim_keys = {
                "filament_diameter",
                "filament_density",
                "filament_cost",
                "filament_colour",
                "filament_type",
                "filament_is_support",
                "filament_settings_id",
                "nozzle_diameter",
                "flush_volumes_matrix",
                "wiping_volumes_extruders",
            };
            for (const char* key : trim_keys) {
                auto* opt = merged_config.option(key, true);
                if (!opt) continue;
                if (auto* fs = dynamic_cast<ConfigOptionFloats*>(opt)) {
                    if (!fs->values.empty()) { fs->values.resize(1); merged_config.set_key_value(key, new ConfigOptionFloats(fs->values)); }
                } else if (auto* ss = dynamic_cast<ConfigOptionStrings*>(opt)) {
                    if (!ss->values.empty()) { ss->values.resize(1); merged_config.set_key_value(key, new ConfigOptionStrings(ss->values)); }
                } else if (auto* bs = dynamic_cast<ConfigOptionBools*>(opt)) {
                    if (!bs->values.empty()) { bs->values.resize(1); merged_config.set_key_value(key, new ConfigOptionBools(bs->values)); }
                }
            }
        } else if (used_extruders.size() <= 1) {
            BOOST_LOG_TRIVIAL(info) << "Disabling prime tower (single extruder model)";
            merged_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
        }
    }

    // Apply per-plate config overrides (curr_bed_type, print_sequence, spiral_mode, etc.)
    for (const auto& pd : m_plate_data) {
        if (pd->plate_index == plate_id && !pd->config.empty()) {
            merged_config.apply(pd->config);
            BOOST_LOG_TRIVIAL(info) << "Applied per-plate config overrides for plate " << plate_id;
            break;
        }
    }
    auto apply_status = print.apply(m_model, merged_config);
    BOOST_LOG_TRIVIAL(info) << "Print apply status: " << static_cast<int>(apply_status);

    // Diagnostic: show wipe tower state and extruder count after apply
    {
        bool has_wt = print.has_wipe_tower();
        auto extrs = print.extruders();
        std::ostringstream oss;
        for (auto e : extrs) oss << e << " ";
        auto fd = m_config.option<ConfigOptionFloats>("filament_diameter");
        bool enable_pt = m_config.option<ConfigOptionBool>("enable_prime_tower")->value;
        BOOST_LOG_TRIVIAL(info) << "After apply: has_wipe_tower=" << has_wt
            << ", enable_prime_tower=" << enable_pt
            << ", filament_diameter.size=" << (fd ? (int)fd->values.size() : -1)
            << ", print.extruders=[" << oss.str() << "]";
    }

    // Verify prime tower state after apply
    if (print.has_wipe_tower()) {
        BOOST_LOG_TRIVIAL(warning) << "Prime tower still enabled after apply";
    }

    if (print.num_object_instances() == 0) {
        BOOST_LOG_TRIVIAL(warning) << "Plate " << plate_id << " has no printable objects after apply, skipping";
        m_stats.issues.push_back(make_warning(plate_id, "NO_PRINTABLE_OBJECTS",
            "No printable objects on this plate after apply"));
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
        std::string wcode;
        switch (warning.type) {
            case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:  wcode = "PRINT_VALIDATE_WARNING_FILAMENT_BED_MISMATCH"; break;
            case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:     wcode = "PRINT_VALIDATE_WARNING_FILAMENT_TEMP_MISMATCH"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:  wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_SEQ"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:wcode = "PRINT_VALIDATE_WARNING_OBJECT_COLLISION_LAYER"; break;
            case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:    wcode = "PRINT_VALIDATE_WARNING_LAYER_HEIGHT_LIMIT"; break;
            default:                                          wcode = "PRINT_VALIDATE_WARNING"; break;
        }
        m_stats.issues.push_back(make_warning(plate_id, wcode,
            warning.string + opt_hint, obj_name));
    }

    if (!err.string.empty()) {
        auto [obj_name, opt_hint] = format_exception_context(err);
        std::cerr << "[ERROR] Plate " << plate_id << ": " << err.string << obj_name << opt_hint << std::endl;
        std::string ecode;
        switch (err.type) {
            case STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE:  ecode = "PRINT_VALIDATE_FILAMENT_BED_MISMATCH"; break;
            case STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP:     ecode = "PRINT_VALIDATE_FILAMENT_TEMP_MISMATCH"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT:  ecode = "PRINT_VALIDATE_OBJECT_COLLISION_SEQ"; break;
            case STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT:ecode = "PRINT_VALIDATE_OBJECT_COLLISION_LAYER"; break;
            case STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT:    ecode = "PRINT_VALIDATE_LAYER_HEIGHT_LIMIT"; break;
            default:                                          ecode = "PRINT_VALIDATE_ERROR"; break;
        }
        m_stats.issues.push_back(make_error(plate_id, ecode,
            err.string + opt_hint, obj_name));
        m_any_error = true;
        set_error_type(EXIT_VALIDATION_ERROR);
        return false;
    }

    return true;
}
bool SliceEngine::run_slicing(int plate_id, Print& print) {
    BOOST_LOG_TRIVIAL(info) << "Starting slicing process for plate " << plate_id << "...";

    try {
        print.process();
        // Diagnostic: show wipe tower state after successful slicing
        if (print.has_wipe_tower()) {
            const auto& tool_ordering = print.get_tool_ordering();
            auto n_layers = std::distance(tool_ordering.begin(), tool_ordering.end());
            BOOST_LOG_TRIVIAL(info) << "After process: wipe tower active, "
                << n_layers << " tool layers";
            size_t count = 0;
            for (auto it = tool_ordering.begin(); it != tool_ordering.end() && count < 3; ++it, ++count) {
                std::ostringstream oss;
                for (auto e : it->extruders) oss << e << " ";
                BOOST_LOG_TRIVIAL(info) << "  layer_tools[" << count << "]: extruders=[" << oss.str()
                    << "], has_wipe_tower=" << it->has_wipe_tower
                    << ", has_object=" << it->has_object
                    << ", wipe_tower_partitions=" << it->wipe_tower_partitions;
            }
        } else {
            BOOST_LOG_TRIVIAL(info) << "After process: no wipe tower (expected for single-extruder)";
        }
    }
    catch (const SlicingErrors& exs) {
        for (const auto& ex : exs.errors_) {
            BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << ex.what();
            std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed" << std::endl;
            m_stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
                "Slicing failed for this plate. The model may contain geometry that cannot be sliced."));
        }
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const SlicingError& ex) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing error: " << ex.what();
        std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed" << std::endl;
        m_stats.issues.push_back(make_error(plate_id, "SLICING_ERROR",
            "Slicing failed for this plate. The model may contain geometry that cannot be sliced."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const CanceledException&) {
        std::cerr << "[ERROR] Plate " << plate_id << ": Slicing was cancelled." << std::endl;
        m_stats.issues.push_back(make_error(plate_id, "SLICING_CANCELLED",
            "Slicing was cancelled"));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
        return false;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Plate " << plate_id << " slicing exception: " << e.what();
        std::cerr << "[ERROR] Plate " << plate_id << ": slicing failed due to an internal error" << std::endl;
        m_stats.issues.push_back(make_error(plate_id, "SLICING_EXCEPTION",
            "Slicing failed due to an internal error. Please try again."));
        m_any_error = true;
        set_error_type(EXIT_SLICING_ERROR);
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
        GCodeProcessor::s_IsBBLPrinter = print.is_BBL_printer();
        std::string exported = print.export_gcode(gcode_output, &result.gcode_result, nullptr);
        BOOST_LOG_TRIVIAL(info) << "G-code exported to: " << exported;
        result.gcode_path = exported;

        // Post-processing scripts are disabled in cloud mode to prevent
        // remote code execution via user-uploaded 3MF files.
        // run_post_process_scripts(result.gcode_path, print.full_print_config());

        // Collect PrintBase warnings (EmptyGcodeLayers, G-code overlap, etc.)
        for (int step = 0; step < static_cast<int>(PrintStep::psCount); ++step) {
            auto wstate = print.step_state_with_warnings(static_cast<PrintStep>(step));
            for (const auto& w : wstate.warnings) {
                if (!w.current) continue;
                bool is_critical = (w.level == PrintStateBase::WarningLevel::CRITICAL);
                BOOST_LOG_TRIVIAL(warning) << "Print warning ["
                    << (is_critical ? "error" : "warning") << "]: " << w.message;
                if (is_critical)
                    result.issues.push_back(make_error(plate_id, "PRINT_WARNING", w.message));
                else
                    result.issues.push_back(make_warning(plate_id, "PRINT_WARNING", w.message));
            }
        }

        const PrintStatistics& ps = print.print_statistics();
        result.total_weight = ps.total_weight;
        result.support_used = print.is_support_used();
        result.total_used_filament = ps.total_used_filament;
        result.total_cost = ps.total_cost;
        result.filament_volumes = result.gcode_result.print_statistics.total_volumes_per_extruder;

        return true;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to export G-code for plate " << plate_id << ": " << e.what();
        result.issues.push_back(make_error(plate_id, "GCODE_EXPORT_ERROR",
            "G-code export failed due to an internal error."));
        m_any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        m_plate_results[plate_id] = result;
        return false;
    }
}

void SliceEngine::run_postprocessing(int plate_id, PlateSliceResult& result) {
    bool has_postprocess_error = false;
    bool has_postprocess_warning = false;

    // Toolpaths outside print volume (matches desktop SLICING_ERROR severity)
    if (result.gcode_result.toolpath_outside) {
        log_plate_message("[Post-processing]", "ERROR", plate_id,
            "Some toolpaths are outside the printable area.");
        has_postprocess_error = true;
        result.issues.push_back(make_error(plate_id, "TOOLPATH_OUTSIDE",
            "Some toolpaths are outside the printable area"));
    }

    // Tool height outside check (matches desktop GLCanvas3D.cpp:9658)
    if (!result.gcode_result.moves.empty() && result.gcode_result.printable_height > 0.0f) {
        float max_z = result.gcode_result.moves[0].position.z();
        for (const auto& move : result.gcode_result.moves)
            if (move.position.z() > max_z) max_z = move.position.z();
        if (max_z - result.gcode_result.printable_height >= 1e-6) {
            log_plate_message("[Post-processing]", "ERROR", plate_id,
                "A G-code path goes beyond the max print height.");
            has_postprocess_error = true;
            Issue h = make_error(plate_id, "TOOL_HEIGHT_OUTSIDE",
                "A G-code path goes beyond the max print height");
            h.z_height = static_cast<double>(max_z);
            result.issues.push_back(h);
        }
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
        Issue conflict = make_error(plate_id, "TOOLPATH_CONFLICT",
            "Toolpath conflict detected between \"" + obj1 + "\" and \"" + obj2
            + "\" at Z=" + std::to_string(cr._height) + "mm",
            obj1 + " vs " + obj2);
        conflict.z_height = cr._height;
        result.issues.push_back(conflict);
    }

    // Bed/filament compatibility
    if (!result.gcode_result.bed_match_result.match) {
        const auto& bm = result.gcode_result.bed_match_result;
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Filament " + std::to_string(bm.extruder_id + 1)
            + " is not compatible with bed type \"" + bm.bed_type_name + "\".");
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "BED_FILAMENT_MISMATCH",
            "Filament " + std::to_string(bm.extruder_id + 1)
            + " is not compatible with bed type \"" + bm.bed_type_name + "\""));
    }

    // Timelapse warnings
    if (result.gcode_result.timelapse_warning_code & 1) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Timelapse is not supported in spiral vase mode on this printer.");
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TIMELAPSE_SPIRAL_VASE",
            "Timelapse is not supported in spiral vase mode on this printer"));
    }
    if ((result.gcode_result.timelapse_warning_code >> 1) & 1) {
        log_plate_message("[Post-processing]", "WARNING", plate_id,
            "Timelapse is not supported with by-object print sequence on this printer.");
        has_postprocess_warning = true;
        result.issues.push_back(make_warning(plate_id, "TIMELAPSE_BY_OBJECT",
            "Timelapse is not supported with by-object print sequence on this printer"));
    }

    // Slice warnings
    for (const auto& w : result.gcode_result.warnings) {
        if (w.error_code == "1000C001") continue; // bed temp warning irrelevant for cloud slicing
        if (w.level >= 2) {
            log_plate_message("[Post-processing]", "ERROR", plate_id,
                w.msg + " (code: " + w.error_code + ")");
            has_postprocess_error = true;
            result.issues.push_back(make_error(plate_id, w.error_code,
                w.msg + " (code: " + w.error_code + ")"));
        } else if (w.level == 1) {
            log_plate_message("[Post-processing]", "WARNING", plate_id,
                w.msg + " (code: " + w.error_code + ")");
            has_postprocess_warning = true;
            result.issues.push_back(make_warning(plate_id, w.error_code,
                w.msg + " (code: " + w.error_code + ")"));
        } else {
            log_plate_message("[Post-processing]", "TIP", plate_id, w.msg);
            result.issues.push_back(make_tip(plate_id, w.error_code, w.msg));
        }
    }

    result.has_postprocess_warning = has_postprocess_warning;
    if (has_postprocess_warning)
        m_any_postprocess_warning = true;
    if (has_postprocess_error) {
        m_any_error = true;
        set_error_type(EXIT_VALIDATION_ERROR);
    }
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
            m_any_error = true;
            set_error_type(EXIT_EXPORT_ERROR);
            std::string msg = "Failed to create output package. The slicing result could not be saved.";
            m_stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
            if (m_stats.error_message.empty())
                m_stats.error_message = msg;
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "gcode.3mf package created: " << m_output_path;
    }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to create gcode.3mf package: " << e.what();
        m_any_error = true;
        set_error_type(EXIT_EXPORT_ERROR);
        std::string msg = "Failed to create output package due to an internal error.";
        m_stats.issues.push_back(make_error(-1, "PACKAGE_EXPORT_ERROR", msg));
        if (m_stats.error_message.empty())
            m_stats.error_message = msg;
    }
}

// ============================================================================
// Exit code derivation
// ============================================================================

void SliceEngine::set_error_type(int code) {
    if (code > m_error_type)
        m_error_type = code;
}

int SliceEngine::exit_code() const {
    if (m_error_type > EXIT_OK)
        return m_error_type;
    if (m_any_error)
        return EXIT_VALIDATION_ERROR;
    if (m_any_postprocess_warning)
        return EXIT_POSTPROCESS_WARNING;
    return EXIT_OK;
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
        if (plate_has_error) {
            m_any_error = true;
            set_error_type(EXIT_VALIDATION_ERROR);
        }
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
            if (!std::isfinite(plate_stats.print_time)) {
                BOOST_LOG_TRIVIAL(warning)
                    << "Plate " << plate_id << " print time is non-finite ("
                    << normal_mode.time << " - " << normal_mode.prepare_time
                    << "), falling back to 0";
                plate_stats.print_time = 0.0f;
            }

            plate_stats.total_filament_m = result.total_used_filament;
            plate_stats.total_filament_g = result.total_weight;
            plate_stats.total_cost = result.total_cost;
            plate_stats.support_used = result.support_used;
            plate_stats.toolpath_outside = result.gcode_result.toolpath_outside;
            plate_stats.long_retraction_when_cut = result.gcode_result.long_retraction_when_cut;

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

            // Look up the thumbnail path from plate data
            for (const auto* pd : m_plate_data) {
                if (pd->plate_index == plate_id && !pd->thumbnail_file.empty()) {
                    plate_stats.model_thumbnail = "Metadata/plate_" + std::to_string(plate_id + 1) + ".png";
                    break;
                }
            }
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
