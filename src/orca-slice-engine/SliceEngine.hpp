#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Semver.hpp"

#include "Types.hpp"

struct EngineConfig {
    std::string input_file;
    std::string output_base;       // -o value, empty = auto-derive from input name
    int plate_id = 0;              // 0 = all plates
    OutputFormat format = OutputFormat::GCODE_3MF;
    bool single_plate = false;
    std::string temp_dir;          // temp directory for intermediate gcode files
    std::string data_dir;          // --data-dir, custom system presets path (empty = auto)
    int timeout_seconds = 0;       // 0 = no timeout; cloud service sets based on file size
    std::string cancel_file;       // watchdog file path for external cancellation
    bool enforce_official_presets = true;  // P0-2: replace user config with official presets
};

// Intermediate result for a single plate during the pipeline
struct PlateSliceResult {
    std::string gcode_path;
    Slic3r::GCodeProcessorResult gcode_result;
    double total_weight = 0.0;
    bool support_used = false;
    bool has_postprocess_warning = false;
    double total_used_filament = 0.0;
    double total_cost = 0.0;
    std::map<size_t, double> filament_volumes;  // per extruder
    std::vector<Issue> issues;                   // collected issues for this plate
};

class SliceEngine {
public:
    SliceEngine(const EngineConfig& cfg, std::vector<std::string>& temp_files);

    // Run the full pipeline. Returns true if at least one plate produced output.
    bool run();

    // Results after run()
    const SliceOutputStats& stats() const { return m_stats; }
    const std::string& output_path() const { return m_output_path; }

    // Accessors for exit-code determination
    bool any_error() const { return m_any_error; }
    bool any_postprocess_warning() const { return m_any_postprocess_warning; }

    // Returns the most specific exit code based on what failed.
    // Precedence: validation > slicing > export > load > postprocess_warning > ok.
    int exit_code() const;
    void set_error_type(int code);

private:
    // --- Pipeline stages ---
    bool load_3mf();
    void validate_config();
    void load_system_presets();
    void validate_presets();
    void apply_official_presets();
    bool validate_input();
    void process_plate(int plate_id);
    void package_output();
    void build_statistics();

    // --- Per-plate sub-stages ---
    int  filter_instances(int plate_id, std::set<int>& identify_ids);
    bool run_build_volume_check(int plate_id, const std::set<int>& identify_ids);
    Slic3r::Vec3d setup_print_origin(int plate_id, double plate_width, double plate_depth);
    bool apply_model(int plate_id, Slic3r::Print& print, const Slic3r::Vec3d& origin);
    bool run_validation(int plate_id, Slic3r::Print& print);
    bool run_slicing(int plate_id, Slic3r::Print& print);
    bool export_gcode(int plate_id, Slic3r::Print& print, PlateSliceResult& result);
    void run_postprocessing(int plate_id, PlateSliceResult& result);

    // --- State ---
    EngineConfig m_cfg;
    std::string m_output_path;
    SliceOutputStats m_stats;
    std::chrono::steady_clock::time_point m_timeout_deadline;
    bool m_has_timeout = false;
    std::vector<std::string>& m_temp_files;
    std::map<int, PlateSliceResult> m_plate_results;
    bool m_any_error = false;
    bool m_any_postprocess_warning = false;
    int m_error_type = EXIT_OK;  // most severe exit code encountered

    // Loaded data
    Slic3r::Model m_model;
    Slic3r::DynamicPrintConfig m_config;
    Slic3r::ConfigSubstitutionContext m_config_substitutions{
        Slic3r::ForwardCompatibilitySubstitutionRule::Enable};
    Slic3r::PlateDataPtrs m_plate_data;
    std::vector<Slic3r::Preset*> m_project_presets;
    bool m_is_bbl_3mf = false;
    Slic3r::Semver m_file_version;

    // Preset validation (requires system profiles at resources_dir/profiles/)
    std::unique_ptr<Slic3r::PresetBundle> m_preset_bundle;
    bool m_presets_available = false;
};
