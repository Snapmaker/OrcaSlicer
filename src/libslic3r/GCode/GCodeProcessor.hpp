#ifndef slic3r_GCodeProcessor_hpp_
#define slic3r_GCodeProcessor_hpp_

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/CustomGCode.hpp"

#include <cstdint>
#include <array>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <utility>

namespace Slic3r {

class Print;
namespace PreCooling { struct Plan; }

// Ultra: Bambu machine start/end/change gcode emits reserved T<n> markers (n > 254) that are NOT tool
// changes and cost no print time: T255, T1000/T1001 (mech-sweep / hotend-type detection), T1100, and the
// AMS pull-back sentinels T65279 (0xFEFF) / T65535 (0xFFFF). The time estimator, the M104 post-processor,
// and the cooling buffer must treat these as no-ops instead of flagging "invalid toolchange".
inline bool is_bbl_special_tool_command(int tool_number)
{
    switch (tool_number) {
    case 255: case 1000: case 1001: case 1100: case 65279: case 65535: return true;
    default: return false;
    }
}

// slice warnings enum strings
#define NOZZLE_HRC_CHECKER                                          "the_actual_nozzle_hrc_smaller_than_the_required_nozzle_hrc"
#define BED_TEMP_TOO_HIGH_THAN_FILAMENT                             "bed_temperature_too_high_than_filament"
#define NOT_SUPPORT_TRADITIONAL_TIMELAPSE                           "not_support_traditional_timelapse"
#define NOT_GENERATE_TIMELAPSE                                      "not_generate_timelapse"
#define LONG_RETRACTION_WHEN_CUT                                    "activate_long_retraction_when_cut"

    enum class EMoveType : unsigned char
    {
        Noop,
        Retract,
        Unretract,
        Seam,
        Tool_change,
        Color_change,
        Pause_Print,
        Custom_GCode,
        Travel,
        Wipe,
        Extrude,
        Count
    };

    struct PrintEstimatedStatistics
    {
        enum class ETimeMode : unsigned char
        {
            Normal,
            Stealth,
            Count
        };

        struct Mode
        {
            float time;
            float prepare_time;
            std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> custom_gcode_times;
            // EDGE (libvgcode stage 1): the time machine no longer accumulates these tables (OrcaSlicer #10735
            // removed them). GCodeProcessor::finalize() recomputes them from the per-move times
            // (MoveVertex::time) with the old semantics, for the legend, the layer slider and slice_info.
            std::vector<std::pair<EMoveType, float>> moves_times;
            std::vector<std::pair<ExtrusionRole, float>> roles_times;
            std::vector<float> layers_times;

            void reset() {
                time = 0.0f;
                prepare_time = 0.0f;
                custom_gcode_times.clear();
                custom_gcode_times.shrink_to_fit();
                moves_times.clear();
                moves_times.shrink_to_fit();
                roles_times.clear();
                roles_times.shrink_to_fit();
                layers_times.clear();
                layers_times.shrink_to_fit();
            }
        };

        std::vector<double>                                 volumes_per_color_change;
        std::map<size_t, double>                            model_volumes_per_extruder;
        std::map<size_t, double>                            wipe_tower_volumes_per_extruder;
        std::map<size_t, double>                            support_volumes_per_extruder;
        std::map<size_t, double>                            total_volumes_per_extruder;
        //BBS: the flush amount of every filament
        std::map<size_t, double>                            flush_per_filament;
        std::map<ExtrusionRole, std::pair<double, double>>  used_filaments_per_role;

        std::array<Mode, static_cast<size_t>(ETimeMode::Count)> modes;
        unsigned int                                        total_filamentchanges;

        PrintEstimatedStatistics() { reset(); }

        void reset() {
            for (auto m : modes) {
                m.reset();
            }
            volumes_per_color_change.clear();
            volumes_per_color_change.shrink_to_fit();
            wipe_tower_volumes_per_extruder.clear();
            model_volumes_per_extruder.clear();
            support_volumes_per_extruder.clear();
            total_volumes_per_extruder.clear();
            flush_per_filament.clear();
            used_filaments_per_role.clear();
            total_filamentchanges = 0;
        }
    };

    struct ConflictResult
    {
        std::string        _objName1;
        std::string        _objName2;
        double             _height;
        const void *_obj1; // nullptr means wipe tower
        const void *_obj2;
        int                layer = -1;
        ConflictResult(const std::string &objName1, const std::string &objName2, double height, const void *obj1, const void *obj2)
            : _objName1(objName1), _objName2(objName2), _height(height), _obj1(obj1), _obj2(obj2)
        {}
        ConflictResult() = default;
    };

    struct BedMatchResult
    {
        bool match;
        std::string bed_type_name;
        int extruder_id;
        BedMatchResult():match(true),bed_type_name(""),extruder_id(-1) {}
        BedMatchResult(bool _match,const std::string& _bed_type_name="",int _extruder_id=-1)
            :match(_match),bed_type_name(_bed_type_name),extruder_id(_extruder_id)
        {}
    };

    using ConflictResultOpt = std::optional<ConflictResult>;

    struct GCodeProcessorResult
    {
        ConflictResultOpt conflict_result;
        BedMatchResult  bed_match_result;

        struct SettingsIds
        {
            std::string print;
            std::vector<std::string> filament;
            std::string printer;

            void reset() {
                print.clear();
                filament.clear();
                printer.clear();
            }
        };

        struct MoveVertex
        {
            unsigned int gcode_id{ 0 };
            EMoveType type{ EMoveType::Noop };
            ExtrusionRole extrusion_role{ erNone };
            unsigned char extruder_id{ 0 };
            unsigned char cp_color_id{ 0 };
            Vec3f position{ Vec3f::Zero() }; // mm
            float delta_extruder{ 0.0f }; // mm
            float feedrate{ 0.0f }; // mm/s
            // Planner (actual) speed at the end of this move, from the Normal time machine. OrcaSlicer #10735.
            float actual_feedrate{ 0.0f }; // mm/s
            float width{ 0.0f }; // mm
            float height{ 0.0f }; // mm
            float mm3_per_mm{ 0.0f };
            float travel_dist{ 0.0f }; // mm
            float fan_speed{ 0.0f }; // percentage
            float temperature{ 0.0f }; // Celsius degrees
            // EDGE (libvgcode stage 3, OrcaSlicer #11673 / #13169): preview-only values for the Pressure
            // advance, Acceleration and Jerk views. The last M900 K / M572 S / SET_PRESSURE_ADVANCE
            // ADVANCE= value, and the Normal-mode acceleration (mm/s^2) and axis jerk (mm/s) the time
            // machine applies to this move. Nothing in the G-code output reads them.
            float pressure_advance{ 0.0f };
            float acceleration{ 0.0f };
            float jerk{ 0.0f };
            // This move's own duration per time estimate mode (s), including any synchronising wait
            // (G4, M400 S, tool change, ...) the time machine booked on it. OrcaSlicer #10735.
            std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> time{ 0.0f, 0.0f };
            // Legacy GCodeViewer only (removed with it in libvgcode stage 3): the raw layer counter while
            // processing (0 = before the first layer change), replaced by the layer duration in finalize().
            float layer_duration{ 0.0f };
            // 0-based layer index from the layer change tag (upstream semantics: max(1, layer counter) - 1).
            unsigned int layer_id{ 0 };
            // A vertex with no G-code line of its own: a segment of a G2/G3 arc, or (when enabled) an
            // actual-speed profile point. OrcaSlicer #10735.
            bool internal_only{ false };
            // Inside the machine start G-code (TimeBlock::Flags::prepare_stage); its travel time is kept
            // out of the per-move-type table, as the time machine did before.
            bool prepare_stage{ false };
            // EDGE: an actual-speed profile point (internal_only, no time, its extruder delta interpolated
            // for the preview). Not a piece of a G-code move: code that sums moves must skip it.
            bool actual_speed_point{ false };

            float volumetric_rate() const { return feedrate * mm3_per_mm; }
            float actual_volumetric_rate() const { return actual_feedrate * mm3_per_mm; }

            // EDGE (libvgcode stage 1): G2/G3 now arrive as internal G1 vertices (PrusaSlicer discretisation,
            // OrcaSlicer #10735), so no move carries BBS arc interpolation data any more. These stubs keep the
            // legacy GCodeViewer's (dead) arc branches compiling until stage 3 replaces that viewer.
            bool is_arc_move_with_interpolation_points() const { return false; }
            bool is_arc_move() const { return false; }
            static inline const std::vector<Vec3f> interpolation_points{};
        };

        struct SliceWarning {
            int         level;                  // 0: normal tips, 1: warning; 2: error
            std::string msg;                    // enum string
            std::string error_code;             // error code for studio
            std::vector<std::string> params;    // extra msg info
        };

        std::string filename;
        unsigned int id;
        std::vector<MoveVertex> moves;
        // Positions of ends of lines of the final G-code this->filename after TimeProcessor::post_process() finalizes the G-code.
        std::vector<size_t> lines_ends;
        Pointfs printable_area;
        //BBS: add bed exclude area
        Pointfs bed_exclude_area;
        //BBS: add toolpath_outside
        bool toolpath_outside;
        //BBS: add object_label_enabled
        bool label_object_enabled;
        //BBS : extra retraction when change filament,experiment func
        bool long_retraction_when_cut {0};
        int timelapse_warning_code {0};
        bool support_traditional_timelapse{true};
        float printable_height;
        // Ultra (H2C 3MF schema): first-layer print time, and the order in which filaments /
        // logical nozzles were entered during the print. Ported from BambuStudio
        // GCodeProcessorResult (initial_layer_time, filament_change_sequence,
        // nozzle_change_sequence); consumed by the sliced-3MF writer.
        float initial_layer_time{0.f};
        std::vector<unsigned int> filament_change_sequence;
        std::vector<unsigned int> nozzle_change_sequence;
        // Ultra (H2C nozzle rack): per-filament physical group assignment for
        // Metadata/filament_sequence.json ("optimal_assignment"); BambuStudio
        // GCodeProcessorResult::optimal_assignment.
        std::vector<int> optimal_assignment;
        SettingsIds settings_ids;
        size_t extruders_count;
        bool backtrace_enabled;
        std::vector<std::string> extruder_colors;
        std::vector<float> filament_diameters;
        std::vector<int>   required_nozzle_HRC;
        std::vector<float> filament_densities;
        std::vector<float> filament_costs;
        // filament_costs are real prices: from the slicing config, or from a G-code whose CONFIG_BLOCK
        // has filament_cost. False for a G-code exported without filament prices or by another slicer;
        // filament_costs then hold no price (0) and the cost views say so instead of guessing.
        bool has_filament_costs{ false };
        // Printer preset time_cost (money per hour of print time), for the machine part of the cost.
        double time_cost{ 0. };
        bool has_time_cost{ false };
        std::vector<int> filament_vitrification_temperature;
        PrintEstimatedStatistics print_statistics;
        std::vector<CustomGCode::Item> custom_gcode_per_print_z;
        // Legacy GCodeViewer only (spiral vase layer ranges as move ids); libvgcode needs only spiral_vase_mode.
        std::vector<std::pair<float, std::pair<size_t, size_t>>> spiral_vase_layers;
        bool spiral_vase_mode{ false };
        float z_offset{ 0.0f };
        //BBS
        std::vector<SliceWarning> warnings;
        int nozzle_hrc;
        NozzleType nozzle_type;
        BedType bed_type = BedType::btCount;
#if ENABLE_GCODE_VIEWER_STATISTICS
        int64_t time{ 0 };
#endif // ENABLE_GCODE_VIEWER_STATISTICS
        void reset();

        //BBS: add mutex for protection of gcode result
        mutable std::mutex result_mutex;
        GCodeProcessorResult& operator=(const GCodeProcessorResult &other) { assign(other); return *this; }
        // Declared because the user-declared copy assignment suppresses the implicit move.
        GCodeProcessorResult& operator=(GCodeProcessorResult &&other) { assign(std::move(other)); return *this; }
        // Add a new member here, or neither assignment transfers it.
        template<class Other> void assign(Other &&other)
        {
            conflict_result = std::forward<Other>(other).conflict_result;
            bed_match_result = std::forward<Other>(other).bed_match_result;
            filename = std::forward<Other>(other).filename;
            id = std::forward<Other>(other).id;
            moves = std::forward<Other>(other).moves;
            lines_ends = std::forward<Other>(other).lines_ends;
            printable_area = std::forward<Other>(other).printable_area;
            bed_exclude_area = std::forward<Other>(other).bed_exclude_area;
            toolpath_outside = std::forward<Other>(other).toolpath_outside;
            label_object_enabled = std::forward<Other>(other).label_object_enabled;
            long_retraction_when_cut = std::forward<Other>(other).long_retraction_when_cut;
            timelapse_warning_code = std::forward<Other>(other).timelapse_warning_code;
            support_traditional_timelapse = std::forward<Other>(other).support_traditional_timelapse;
            printable_height = std::forward<Other>(other).printable_height;
            initial_layer_time = std::forward<Other>(other).initial_layer_time;
            filament_change_sequence = std::forward<Other>(other).filament_change_sequence;
            nozzle_change_sequence = std::forward<Other>(other).nozzle_change_sequence;
            optimal_assignment = std::forward<Other>(other).optimal_assignment;
            settings_ids = std::forward<Other>(other).settings_ids;
            extruders_count = std::forward<Other>(other).extruders_count;
            backtrace_enabled = std::forward<Other>(other).backtrace_enabled;
            extruder_colors = std::forward<Other>(other).extruder_colors;
            filament_diameters = std::forward<Other>(other).filament_diameters;
            required_nozzle_HRC = std::forward<Other>(other).required_nozzle_HRC;
            filament_densities = std::forward<Other>(other).filament_densities;
            filament_costs = std::forward<Other>(other).filament_costs;
            has_filament_costs = other.has_filament_costs;
            time_cost = other.time_cost;
            has_time_cost = other.has_time_cost;
            filament_vitrification_temperature = std::forward<Other>(other).filament_vitrification_temperature;
            print_statistics = std::forward<Other>(other).print_statistics;
            custom_gcode_per_print_z = std::forward<Other>(other).custom_gcode_per_print_z;
            spiral_vase_layers = std::forward<Other>(other).spiral_vase_layers;
            spiral_vase_mode = other.spiral_vase_mode;
            z_offset = other.z_offset;
            warnings = std::forward<Other>(other).warnings;
            nozzle_hrc = std::forward<Other>(other).nozzle_hrc;
            nozzle_type = std::forward<Other>(other).nozzle_type;
            bed_type = std::forward<Other>(other).bed_type;
#if ENABLE_GCODE_VIEWER_STATISTICS
            time = std::forward<Other>(other).time;
#endif
        }
        void  lock() const { result_mutex.lock(); }
        void  unlock() const { result_mutex.unlock(); }
    };


    class GCodeProcessor
    {
        static const std::vector<std::string> Reserved_Tags;
        static const std::vector<std::string> Reserved_Tags_compatible;
        static const std::string Flush_Start_Tag;
        static const std::string Flush_End_Tag;
        static const std::string External_Purge_Tag;
    public:
        enum class ETags : unsigned char
        {
            Role,
            Wipe_Start,
            Wipe_End,
            Height,
            Width,
            Layer_Change,
            Color_Change,
            Pause_Print,
            Custom_Code,
            First_Line_M73_Placeholder,
            Last_Line_M73_Placeholder,
            Estimated_Printing_Time_Placeholder,
            Total_Layer_Number_Placeholder,
            Manual_Tool_Change,
            During_Print_Exhaust_Fan,
            Wipe_Tower_Start,
            Wipe_Tower_End,
            PA_Change,
        };

        static const std::string& reserved_tag(ETags tag) { return s_IsBBLPrinter ? Reserved_Tags[static_cast<unsigned char>(tag)] : Reserved_Tags_compatible[static_cast<unsigned char>(tag)]; }
        // checks the given gcode for reserved tags and returns true when finding the 1st (which is returned into found_tag) 
        static bool contains_reserved_tag(const std::string& gcode, std::string& found_tag);
        // checks the given gcode for reserved tags and returns true when finding any
        // (the first max_count found tags are returned into found_tag)
        static bool contains_reserved_tags(const std::string& gcode, unsigned int max_count, std::vector<std::string>& found_tag, bool is_bbl_printer);

        static int get_gcode_last_filament(const std::string &gcode_str);
        static bool get_last_z_from_gcode(const std::string& gcode_str, double& z);

        static const float Wipe_Width;
        static const float Wipe_Height;

        static bool s_IsBBLPrinter;

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        static const std::string Mm3_Per_Mm_Tag;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    private:
        using AxisCoords = std::array<double, 4>;
        using ExtruderColors = std::vector<unsigned char>;
        using ExtruderTemps = std::vector<float>;

        enum class EUnits : unsigned char
        {
            Millimeters,
            Inches
        };

        enum class EPositioningType : unsigned char
        {
            Absolute,
            Relative
        };

        struct CachedPosition
        {
            AxisCoords position; // mm
            float feedrate; // mm/s

            void reset();
        };

        struct CpColor
        {
            unsigned char counter;
            unsigned char current;

            void reset();
        };

    public:
        struct FeedrateProfile
        {
            float entry{ 0.0f }; // mm/s
            float cruise{ 0.0f }; // mm/s
            float exit{ 0.0f }; // mm/s
        };

        struct Trapezoid
        {
            float accelerate_until{ 0.0f }; // mm
            float decelerate_after{ 0.0f }; // mm
            float cruise_feedrate{ 0.0f }; // mm/sec

            float acceleration_time(float entry_feedrate, float acceleration) const;
            float cruise_time() const { return (cruise_feedrate != 0.0f) ? cruise_distance() / cruise_feedrate : 0.0f; }
            float deceleration_time(float distance, float acceleration) const;
            float acceleration_distance() const { return accelerate_until; }
            float cruise_distance() const { return decelerate_after - accelerate_until; }
            float deceleration_distance(float distance) const { return distance - decelerate_after; }
            bool is_cruise_only(float distance) const { return std::abs(cruise_distance() - distance) < EPSILON; }
        };

        struct TimeBlock
        {
            struct Flags
            {
                bool recalculate{ false };
                bool nominal_length{ false };
                bool prepare_stage{ false };
            };

            EMoveType move_type{ EMoveType::Noop };
            ExtrusionRole role{ erNone };
            // Index into GCodeProcessorResult::moves of the move this block times.
            unsigned int move_id{ 0 };
            unsigned int g1_line_id{ 0 };
            unsigned int remaining_internal_g1_lines{ 0 };
            unsigned int layer_id{ 0 };
            float distance{ 0.0f }; // mm
            float acceleration{ 0.0f }; // mm/s^2
            float max_entry_speed{ 0.0f }; // mm/s
            float safe_feedrate{ 0.0f }; // mm/s
            Flags flags;
            FeedrateProfile feedrate_profile;
            Trapezoid trapezoid;

            // Calculates this block's trapezoid
            void calculate_trapezoid();

            float time() const {
                return trapezoid.acceleration_time(feedrate_profile.entry, acceleration) +
                       trapezoid.cruise_time() + trapezoid.deceleration_time(distance, acceleration);
            }
        };


    private:
        struct TimeMachine
        {
            struct State
            {
                float feedrate; // mm/s
                float safe_feedrate; // mm/s
                //BBS: feedrate of X-Y-Z-E axis. But when the move is G2 and G3, X-Y will be
                //same value which means feedrate in X-Y plane.
                AxisCoords axis_feedrate; // mm/s
                AxisCoords abs_axis_feedrate; // mm/s

                //BBS: unit vector of enter speed and exit speed in x-y-z space. 
                //For line move, there are same. For arc move, there are different.
                Vec3f enter_direction;
                Vec3f exit_direction;

                void reset();
            };

            struct CustomGCodeTime
            {
                bool needed;
                float cache;
                std::vector<std::pair<CustomGCode::Type, float>> times;

                void reset();
            };

            struct G1LinesCacheItem
            {
                unsigned int id;
                unsigned int remaining_internal_g1_lines{ 0 };
                float elapsed_time;
            };

            // A point of the planner's speed profile (end of acceleration / start of deceleration) inside
            // a move, or (position unset) the move's own end speed. OrcaSlicer #10735.
            struct ActualSpeedMove
            {
                unsigned int move_id{ 0 };
                std::optional<Vec3f> position;
                float actual_feedrate{ 0.0f };
                std::optional<float> delta_extruder;
                std::optional<float> feedrate;
                std::optional<float> width;
                std::optional<float> height;
                std::optional<float> mm3_per_mm;
                std::optional<float> fan_speed;
                std::optional<float> temperature;
            };

            bool enabled;
            float acceleration; // mm/s^2
            // hard limit for the acceleration, to which the firmware will clamp.
            float max_acceleration; // mm/s^2
            float retract_acceleration; // mm/s^2
            // hard limit for the acceleration, to which the firmware will clamp.
            float max_retract_acceleration; // mm/s^2
            float travel_acceleration; // mm/s^2
            // hard limit for the travel acceleration, to which the firmware will clamp.
            float max_travel_acceleration; // mm/s^2
            float extrude_factor_override_percentage;
            // We accumulate total print time in doubles to reduce the loss of precision
            // while adding big floating numbers with small float numbers.
            double time; // s
            struct StopTime
            {
                unsigned int g1_line_id;
                float elapsed_time;
            };
            std::vector<StopTime> stop_times;
            std::string line_m73_main_mask;
            std::string line_m73_stop_mask;
            State curr;
            State prev;
            CustomGCodeTime gcode_time;
            std::vector<TimeBlock> blocks;
            std::vector<G1LinesCacheItem> g1_times_cache;
            std::vector<ActualSpeedMove> actual_speed_moves;
            //BBS: prepare stage time before print model, including start gcode time and mostly same with start gcode time
            float prepare_time;

            void reset();

            // Plans the queued blocks, writes each processed block's time onto its move
            // (result.moves[block.move_id].time[mode]) and, for the Normal machine, collects the actual
            // speed profile. keep_last_n_blocks stay queued.
            void calculate_time(GCodeProcessorResult& result, PrintEstimatedStatistics::ETimeMode mode, bool collect_actual_speed_points,
                                size_t keep_last_n_blocks = 0, float additional_time = 0.0f);
        };

        struct TimeProcessor
        {
            struct Planner
            {
                // Size of the firmware planner queue. The old 8-bit Marlins usually just managed 16 trapezoidal blocks.
                // Let's be conservative and plan for newer boards with more memory.
                static constexpr size_t queue_size = 64;
                // The firmware recalculates last planner_queue_size trapezoidal blocks each time a new block is added.
                // We are not simulating the firmware exactly, we calculate a sequence of blocks once a reasonable number of blocks accumulate.
                static constexpr size_t refresh_threshold = queue_size * 4;
            };

            // extruder_id is currently used to correctly calculate filament load / unload times into the total print time.
            // This is currently only really used by the MK3 MMU2:
            // extruder_unloaded = true means no filament is loaded yet, all the filaments are parked in the MK3 MMU2 unit.
            bool extruder_unloaded;
            // allow to skip the lines M201/M203/M204/M205 generated by GCode::print_machine_envelope() for non-Normal time estimate mode
            bool machine_envelope_processing_enabled;
            MachineEnvelopeConfig machine_limits;
            // Additional load / unload times for a filament exchange sequence.
            float filament_load_times;
            float filament_unload_times;
            //Orca:  time for tool change
            float machine_tool_change_time;
            // Ultra: fixed prepare seconds added to the total estimate
            float machine_prepare_time { 0.f };

            std::array<TimeMachine, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)> machines;

            void reset();
        };

        struct UsedFilaments  // filaments per ColorChange
        {
            double color_change_cache;
            std::vector<double> volumes_per_color_change;

            double model_extrude_cache;
            std::map<size_t, double> model_volumes_per_extruder;

            double wipe_tower_cache;
            std::map<size_t, double>wipe_tower_volumes_per_extruder;

            double support_volume_cache;
            std::map<size_t, double>support_volumes_per_extruder;

            //BBS: the flush amount of every filament
            std::map<size_t, double> flush_per_filament;

            double total_volume_cache;
            std::map<size_t, double>total_volumes_per_extruder;

            double role_cache;
            std::map<ExtrusionRole, std::pair<double, double>> filaments_per_role;

            void reset();

            void increase_support_caches(double extruded_volume);
            void increase_model_caches(double extruded_volume);
            void increase_wipe_tower_caches(double extruded_volume);

            void process_color_change_cache();
            void process_model_cache(GCodeProcessor* processor);
            void process_wipe_tower_cache(GCodeProcessor* processor);
            void process_support_cache(GCodeProcessor* processor);
            void process_total_volume_cache(GCodeProcessor* processor);

            void update_flush_per_filament(size_t extrude_id, float flush_length);
            void process_role_cache(GCodeProcessor* processor);
            void process_caches(GCodeProcessor* processor);

            friend class GCodeProcessor;
        };

    public:
        class SeamsDetector
        {
            bool m_active{ false };
            std::optional<Vec3f> m_first_vertex;

        public:
            void activate(bool active) {
                if (m_active != active) {
                    m_active = active;
                    if (m_active)
                        m_first_vertex.reset();
                }
            }

            std::optional<Vec3f> get_first_vertex() const { return m_first_vertex; }
            void set_first_vertex(const Vec3f& vertex) { m_first_vertex = vertex; }

            bool is_active() const { return m_active; }
            bool has_first_vertex() const { return m_first_vertex.has_value(); }
        };

        // Helper class used to fix the z for color change, pause print and
        // custom gcode markes
        class OptionsZCorrector
        {
            GCodeProcessorResult& m_result;
            std::optional<size_t> m_move_id;
            std::optional<size_t> m_custom_gcode_per_print_z_id;

        public:
            explicit OptionsZCorrector(GCodeProcessorResult& result) : m_result(result) {
            }

            void set() {
                m_move_id = m_result.moves.size() - 1;
                m_custom_gcode_per_print_z_id = m_result.custom_gcode_per_print_z.size() - 1;
            }

            // Returns the index the marker move was taken from: every move after it shifted down by one
            // and the marker is now the last move.
            std::optional<size_t> update(float height) {
                if (!m_move_id.has_value() || !m_custom_gcode_per_print_z_id.has_value())
                    return std::nullopt;

                const Vec3f position = m_result.moves.back().position;

                GCodeProcessorResult::MoveVertex& move = m_result.moves.emplace_back(m_result.moves[*m_move_id]);
                move.position = position;
                move.height = height;
                m_result.moves.erase(m_result.moves.begin() + *m_move_id);
                m_result.custom_gcode_per_print_z[*m_custom_gcode_per_print_z_id].print_z = position.z();
                const size_t moved_from = *m_move_id;
                reset();
                return moved_from;
            }

            // Moves were inserted at index pos (actual-speed points): keep the stored id on its move.
            void on_moves_inserted(size_t pos, size_t count) {
                if (m_move_id.has_value() && *m_move_id >= pos)
                    *m_move_id += count;
            }

            void reset() {
                m_move_id.reset();
                m_custom_gcode_per_print_z_id.reset();
            }
        };

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        struct DataChecker
        {
            struct Error
            {
                float value;
                float tag_value;
                ExtrusionRole role;
            };

            std::string type;
            float threshold{ 0.01f };
            float last_tag_value{ 0.0f };
            unsigned int count{ 0 };
            std::vector<Error> errors;

            DataChecker(const std::string& type, float threshold)
                : type(type), threshold(threshold)
            {}

            void update(float value, ExtrusionRole role) {
                if (role != erCustom) {
                    ++count;
                    if (last_tag_value != 0.0f) {
                        if (std::abs(value - last_tag_value) / last_tag_value > threshold)
                            errors.push_back({ value, last_tag_value, role });
                    }
                }
            }

            void reset() { last_tag_value = 0.0f; errors.clear(); count = 0; }

            std::pair<float, float> get_min() const {
                float delta_min = FLT_MAX;
                float perc_min = 0.0f;
                for (const Error& e : errors) {
                    if (delta_min > e.value - e.tag_value) {
                        delta_min = e.value - e.tag_value;
                        perc_min = 100.0f * delta_min / e.tag_value;
                    }
                }
                return { delta_min, perc_min };
            }

            std::pair<float, float> get_max() const {
                float delta_max = -FLT_MAX;
                float perc_max = 0.0f;
                for (const Error& e : errors) {
                    if (delta_max < e.value - e.tag_value) {
                        delta_max = e.value - e.tag_value;
                        perc_max = 100.0f * delta_max / e.tag_value;
                    }
                }
                return { delta_max, perc_max };
            }

            void output() const {
                if (!errors.empty()) {
                    std::cout << type << ":\n";
                    std::cout << "Errors: " << errors.size() << " (" << 100.0f * float(errors.size()) / float(count) << "%)\n";
                    auto [min, perc_min] = get_min();
                    auto [max, perc_max] = get_max();
                    std::cout << "min: " << min << "(" << perc_min << "%) - max: " << max << "(" << perc_max << "%)\n";
                }
            }
        };
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    private:
        GCodeReader m_parser;
        EUnits m_units;
        EPositioningType m_global_positioning_type;
        EPositioningType m_e_local_positioning_type;
        std::vector<Vec3f> m_extruder_offsets;
        GCodeFlavor m_flavor;
        float       m_nozzle_volume;
        AxisCoords m_start_position; // mm
        AxisCoords m_end_position; // mm
        AxisCoords m_origin; // mm
        CachedPosition m_cached_position;
        bool m_wiping;
        bool m_flushing;
        bool m_wipe_tower;
        float m_remaining_volume;
        bool m_manual_filament_change;
        // BBS pre-cooling: where the time estimator booked each synchronising wait (a tool change, G4,
        // ...). calculate_time() adds that time to the OLDEST block still in the planner queue, which
        // can be a few hundred moves before the command; plan_pre_cooling() moves it back onto its line.
        // Recording only: the estimate and the G-code are not changed.
        struct BookedTime
        {
            unsigned int line_id;      // the command's line
            unsigned int first_g1_id;  // g1 line id the time was added to, (unsigned)-1 when it was dropped
            float        time;
            bool         tool_change;
        };
        std::vector<BookedTime> m_booked_times;
        bool                    m_booking_tool_change{ false };

        //BBS: x, y offset for gcode generated
        double          m_x_offset{ 0 };
        double          m_y_offset{ 0 };
        // Insert the planner's acceleration / deceleration points as internal moves (for libvgcode's
        // Actual speed view; OrcaSlicer #10735 always does). On since libvgcode stage 3. The points are
        // flagged MoveVertex::actual_speed_point; MoveVertex::actual_feedrate is filled either way.
        bool m_actual_speed_moves_enabled{ true };

        unsigned int m_line_id;
        unsigned int m_last_line_id;
        float m_feedrate; // mm/s
        float m_width; // mm
        float m_height; // mm
        float m_forced_width; // mm
        float m_forced_height; // mm
        float m_mm3_per_mm;
        float m_travel_dist; // mm
        float m_fan_speed; // percentage
        float m_pressure_advance; // EDGE (OrcaSlicer #11673): preview only
        float m_z_offset; // mm
        ExtrusionRole m_extrusion_role;
        unsigned char m_extruder_id;
        unsigned char m_last_extruder_id;
        ExtruderColors m_extruder_colors;
        ExtruderTemps m_extruder_temps;
        ExtruderTemps m_extruder_temps_config;
        ExtruderTemps m_extruder_temps_first_layer_config;
        bool  m_is_XL_printer = false;
        int m_highest_bed_temp;
        float m_extruded_last_z;
        float m_first_layer_height; // mm
        float m_zero_layer_height; // mm
        bool m_processing_start_custom_gcode;
        unsigned int m_g1_line_id;
        unsigned int m_layer_id;
        CpColor m_cp_color;
        SeamsDetector m_seams_detector;
        OptionsZCorrector m_options_z_corrector;
        size_t m_last_default_color_id;
        bool m_detect_layer_based_on_tag {false};
        int m_seams_count;
        bool m_single_extruder_multi_material;
        float m_preheat_time;
        int m_delta_temperature;
        int m_preheat_steps;
        bool m_disable_m73;
#if ENABLE_GCODE_VIEWER_STATISTICS
        std::chrono::time_point<std::chrono::high_resolution_clock> m_start_time;
#endif // ENABLE_GCODE_VIEWER_STATISTICS

        enum class EProducer
        {
            Unknown,
            Snapmaker_Orca,
            Slic3rPE,
            Slic3r,
            SuperSlicer,
            Cura,
            Simplify3D,
            CraftWare,
            ideaMaker,
            KissSlicer
        };

        static const std::vector<std::pair<GCodeProcessor::EProducer, std::string>> Producers;
        EProducer m_producer;

        DynamicConfig m_current_config;

        TimeProcessor m_time_processor;
        UsedFilaments m_used_filaments;

        Print* m_print{ nullptr };

        GCodeProcessorResult m_result;
        static unsigned int s_result_id;

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        DataChecker m_mm3_per_mm_compare{ "mm3_per_mm", 0.01f };
        DataChecker m_height_compare{ "height", 0.01f };
        DataChecker m_width_compare{ "width", 0.01f };
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    public:
        GCodeProcessor();

        void apply_config(const PrintConfig& config);
        void set_print(Print* print) { m_print = print; }
        void enable_stealth_time_estimator(bool enabled);
        bool is_stealth_time_estimator_enabled() const {
            return m_time_processor.machines[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].enabled;
        }
        void enable_machine_envelope_processing(bool enabled) { m_time_processor.machine_envelope_processing_enabled = enabled; }
        void reset();

        const GCodeProcessorResult& get_result() const { return m_result; }
        GCodeProcessorResult& result() { return m_result; }
        GCodeProcessorResult&& extract_result() { return std::move(m_result); }
        DynamicConfig&              current_dynamic_config() { return m_current_config; }


        GCodeReader& parser() { return m_parser; }
        // Load a G-code into a stand-alone G-code viewer.
        // throws CanceledException through print->throw_if_canceled() (sent by the caller as callback).
        void process_file(const std::string& filename, std::function<void()> cancel_callback = nullptr);

        // Streaming interface, for processing G-codes just generated by PrusaSlicer in a pipelined fashion.
        void initialize(const std::string& filename);
        void process_buffer(const std::string& buffer);
        void finalize(bool post_process);

        float get_time(PrintEstimatedStatistics::ETimeMode mode) const;
        float get_prepare_time(PrintEstimatedStatistics::ETimeMode mode) const;
        std::string get_time_dhm(PrintEstimatedStatistics::ETimeMode mode) const;
        std::vector<std::pair<CustomGCode::Type, std::pair<float, float>>> get_custom_gcode_times(PrintEstimatedStatistics::ETimeMode mode, bool include_remaining) const;

        // Recomputes the per-move-type / per-role / per-layer time tables of one mode from the moves'
        // own times, with the semantics the time machine used to accumulate them (travel in the start
        // G-code kept out of the move-type table; travel time counted as erNone unless erCustom).
        static void fill_time_tables(const std::vector<GCodeProcessorResult::MoveVertex>& moves, PrintEstimatedStatistics::ETimeMode mode,
                                     PrintEstimatedStatistics::Mode& out);

        void enable_actual_speed_moves(bool enable) { m_actual_speed_moves_enabled = enable; }
        bool actual_speed_moves_enabled() const { return m_actual_speed_moves_enabled; }

        //BBS: set offset for gcode writer
        void set_xy_offset(double x, double y) { m_x_offset = x; m_y_offset = y; }

        // Orca: if true, only change new layer if ETags::Layer_Change occurs
        // otherwise when we got a lift of z during extrusion, a new layer will be added
        void detect_layer_based_on_tag(bool enabled) { m_detect_layer_based_on_tag = enabled; }

    private:
        void apply_config(const DynamicPrintConfig& config);
        void apply_config_simplify3d(const std::string& filename);
        void apply_config_superslicer(const std::string& filename);
        void process_gcode_line(const GCodeReader::GCodeLine& line, bool producers_enabled);

        // Process tags embedded into comments
        void process_tags(const std::string_view comment, bool producers_enabled);
        bool process_producers_tags(const std::string_view comment);
        bool process_bambuslicer_tags(const std::string_view comment);
        bool process_cura_tags(const std::string_view comment);
        bool process_simplify3d_tags(const std::string_view comment);
        bool process_craftware_tags(const std::string_view comment);
        bool process_ideamaker_tags(const std::string_view comment);
        bool process_kissslicer_tags(const std::string_view comment);

        bool detect_producer(const std::string_view comment);

        // Move
        void process_G0(const GCodeReader::GCodeLine& line);
        void process_G1(const GCodeReader::GCodeLine& line, const std::optional<unsigned int>& remaining_internal_g1_lines = std::nullopt);
        enum class G1DiscretizationOrigin {
            G1,
            G2G3,
        };
        void process_G1(const std::array<std::optional<double>, 4>& axes, const std::optional<double>& feedrate,
                        G1DiscretizationOrigin origin, const std::optional<unsigned int>& remaining_internal_g1_lines);

        // Arc move, discretised into internal G1 moves (PrusaSlicer, OrcaSlicer #10735)
        void process_G2_G3(const GCodeReader::GCodeLine& line, bool clockwise);

        // BBS: handle delay command
        void process_G4(const GCodeReader::GCodeLine& line);

        // Retract
        void process_G10(const GCodeReader::GCodeLine& line);

        // Unretract
        void process_G11(const GCodeReader::GCodeLine& line);

        // Set Units to Inches
        void process_G20(const GCodeReader::GCodeLine& line);

        // Set Units to Millimeters
        void process_G21(const GCodeReader::GCodeLine& line);

        // Firmware controlled Retract
        void process_G22(const GCodeReader::GCodeLine& line);

        // Firmware controlled Unretract
        void process_G23(const GCodeReader::GCodeLine& line);

        // Move to origin
        void process_G28(const GCodeReader::GCodeLine& line);

        // BBS
        void process_G29(const GCodeReader::GCodeLine& line);

        // Set to Absolute Positioning
        void process_G90(const GCodeReader::GCodeLine& line);

        // Set to Relative Positioning
        void process_G91(const GCodeReader::GCodeLine& line);

        // Set Position
        void process_G92(const GCodeReader::GCodeLine& line);

        // Sleep or Conditional stop
        void process_M1(const GCodeReader::GCodeLine& line);

        // Set extruder to absolute mode
        void process_M82(const GCodeReader::GCodeLine& line);

        // Set extruder to relative mode
        void process_M83(const GCodeReader::GCodeLine& line);

        // Set extruder temperature
        void process_M104(const GCodeReader::GCodeLine& line);

        // Set fan speed
        void process_M106(const GCodeReader::GCodeLine& line);

        // Disable fan
        void process_M107(const GCodeReader::GCodeLine& line);

        // Set tool (Sailfish)
        void process_M108(const GCodeReader::GCodeLine& line);

        // Set extruder temperature and wait
        void process_M109(const GCodeReader::GCodeLine& line);

        // Recall stored home offsets
        void process_M132(const GCodeReader::GCodeLine& line);

        // Set tool (MakerWare)
        void process_M135(const GCodeReader::GCodeLine& line);

        //BBS: Set bed temperature
        void process_M140(const GCodeReader::GCodeLine& line);

        //BBS: wait bed temperature
        void process_M190(const GCodeReader::GCodeLine& line);

        //BBS: wait chamber temperature
        void process_M191(const GCodeReader::GCodeLine& line);

        // Set max printing acceleration
        void process_M201(const GCodeReader::GCodeLine& line);

        // Set maximum feedrate
        void process_M203(const GCodeReader::GCodeLine& line);

        // Set default acceleration
        void process_M204(const GCodeReader::GCodeLine& line);

        // Advanced settings
        void process_M205(const GCodeReader::GCodeLine& line);

        // Klipper SET_VELOCITY_LIMIT
        void process_SET_VELOCITY_LIMIT(const GCodeReader::GCodeLine& line);

        // Set extrude factor override percentage
        void process_M221(const GCodeReader::GCodeLine& line);

        // BBS: handle delay command. M400 is defined by BBL only
        void process_M400(const GCodeReader::GCodeLine& line);

        // Repetier: Store x, y and z position
        void process_M401(const GCodeReader::GCodeLine& line);

        // Repetier: Go to stored position
        void process_M402(const GCodeReader::GCodeLine& line);

        // Set allowable instantaneous speed change
        void process_M566(const GCodeReader::GCodeLine& line);
        // EDGE (OrcaSlicer #11673): pressure advance, for the preview only
        void process_M572(const GCodeReader::GCodeLine& line);
        void process_M900(const GCodeReader::GCodeLine& line);
        void process_SET_PRESSURE_ADVANCE(const GCodeReader::GCodeLine& line);

        // Unload the current filament into the MK3 MMU2 unit at the end of print.
        void process_M702(const GCodeReader::GCodeLine& line);

        // Processes T line (Select Tool)
        void process_T(const GCodeReader::GCodeLine& line);
        void process_T(const std::string_view command);

        // post process the file with the given filename to:
        // 1) add remaining time lines M73 and update moves' gcode ids accordingly
        // 2) update used filament data
        void run_post_process();
        // BBS: plans the idle-nozzle pre-cooling / pre-heating lines of a Bambu two-extruder print from
        // a scan of the finished G-code (GCode/PreCoolingInjector); keyed on run_post_process()'s input
        // line ids, each entry is written right after its line.
        std::map<unsigned int, std::vector<std::string>> plan_pre_cooling(const PreCooling::Plan &plan) const;

        void store_move_vertex(EMoveType type, bool internal_only = false);

        void set_extrusion_role(ExtrusionRole role);

        float minimum_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        float minimum_travel_feedrate(PrintEstimatedStatistics::ETimeMode mode, float feedrate) const;
        float get_axis_max_feedrate(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        float get_axis_max_acceleration(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        float get_axis_max_jerk(PrintEstimatedStatistics::ETimeMode mode, Axis axis) const;
        Vec3f get_xyz_max_jerk(PrintEstimatedStatistics::ETimeMode mode) const;
        float get_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_retract_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
    float get_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
        float get_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode) const;
        void  set_travel_acceleration(PrintEstimatedStatistics::ETimeMode mode, float value);
        float get_filament_load_time(size_t extruder_id);
        float get_filament_unload_time(size_t extruder_id);
        int   get_filament_vitrification_temperature(size_t extrude_id);
        void process_custom_gcode_time(CustomGCode::Type code);
        void process_filaments(CustomGCode::Type code);

        // Runs every enabled time machine over its queued blocks (see TimeMachine::calculate_time()) and
        // inserts the collected actual-speed points into the move list.
        void calculate_time(GCodeProcessorResult& result, size_t keep_last_n_blocks = 0, float additional_time = 0.0f);

        // Simulates firmware st_synchronize() call
        void simulate_st_synchronize(float additional_time = 0.0f);

        void update_estimated_times_stats();
        //BBS:
        void update_slice_warnings();
   };

} /* namespace Slic3r */

#endif /* slic3r_GCodeProcessor_hpp_ */


