#pragma once

#include <string>
#include <vector>
#include <map>

#include <boost/filesystem.hpp>

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

// Structured issue for error/warning/tip collection
struct Issue {
    std::string level;       // "error" | "warning" | "tip"
    int plate_id;            // plate index, -1 for global
    std::string object_name; // related object, empty if N/A
    double z_height;         // Z-level in mm, -1 if N/A
    std::string code;        // error code (e.g. from SliceWarning), empty if N/A
    std::string message;     // human-readable description
    std::string suggestion;  // actionable repair suggestion, empty if N/A
};

// Factory functions — set sensible defaults (z_height=-1, correct level)
inline Issue make_error(int plate_id, const std::string& code, const std::string& message,
                        const std::string& object_name = "", const std::string& suggestion = "") {
    return Issue{"error", plate_id, object_name, -1.0, code, message, suggestion};
}
inline Issue make_warning(int plate_id, const std::string& code, const std::string& message,
                          const std::string& object_name = "", const std::string& suggestion = "") {
    return Issue{"warning", plate_id, object_name, -1.0, code, message, suggestion};
}
inline Issue make_tip(int plate_id, const std::string& code, const std::string& message) {
    return Issue{"tip", plate_id, "", -1.0, code, message, ""};
}

// Output statistics structure for JSON export
struct SliceOutputStats {
    // Filament detail for each extruder
    struct FilamentDetail {
        int filament_id;
        std::string type;         // Filament type (e.g., "PLA", "ABS")
        std::string color;        // Filament color (e.g., "#FF0000")
        double used_g;            // Used weight in grams
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
        std::string model_thumbnail;             // Model thumbnail path within gcode.3mf (Metadata/plate_N.png)
        std::vector<Issue> issues;               // Per-plate issues (errors/warnings/tips)
        bool long_retraction_when_cut = false;   // Experimental long retraction when cut feature active
    };

    bool success = false;
    std::string error_message;
    std::vector<PlateStats> plates;
    std::vector<Issue> issues;                   // Global issues (e.g. load errors)
};

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
