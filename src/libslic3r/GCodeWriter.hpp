#ifndef slic3r_GCodeWriter_hpp_
#define slic3r_GCodeWriter_hpp_

#include "libslic3r.h"
#include <string>
#include <charconv>
#include <cmath>
#include <functional>
#include "Extruder.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"
#include "GCode/CoolingBuffer.hpp"

namespace Slic3r {

// Snapmaker (feedrate guard): describes where a feedrate about to be emitted came from, so that
// when the guard in GCodeWriter::set_speed / GCodeFormatter::emit_axis refuses a non-positive or
// non-finite value it can name the ACTUAL CONFIG KEY that resolved badly, instead of leaving the
// user with a silent substitution or a message that only names an extrusion role.
//
// Cost on the valid path: assigning this is a handful of stores of already-computed values (two
// pointers to string literals and an int). Nothing is formatted, allocated or copied unless the
// guard actually fires. `setting`, `role_name` and `object_name` only BORROW their storage, which
// must outlive the writer - string literals, config key names, or a ModelObject's name whose
// lifetime spans the export.
struct FeedrateOrigin
{
    // Config key the speed was read from, e.g. "internal_bridge_speed". nullptr when unknown
    // (travel moves, custom G-code, wipe tower - paths that do not resolve a role speed).
    const char *setting     = nullptr;
    // Human-readable extrusion role, e.g. "Internal Bridge".
    const char *role_name   = nullptr;
    // Object being extruded, or nullptr when not applicable.
    const char *object_name = nullptr;
    // Layer index of the extrusion, or -1 when not applicable.
    int         layer_id    = -1;

    void clear() { *this = FeedrateOrigin{}; }
    bool known() const { return setting != nullptr; }
};

// Snapmaker (feedrate guard): a sink the guard reports a refused feedrate to. GCodeWriter and
// GCodeFormatter live below Print in the dependency graph and cannot call
// Print::active_step_add_warning directly, so GCode::do_export installs a callback that forwards
// to it. When no sink is installed (unit tests, standalone writer use) the guard still clamps and
// logs - it just has nowhere to raise a user-visible warning.
using FeedrateGuardReporter = std::function<void(const FeedrateOrigin &origin, double bad_value, double substituted)>;

class GCodeWriter {
public:
    GCodeConfig config;
    bool multiple_extruders;
    
    GCodeWriter() : 
        multiple_extruders(false), m_extruder(nullptr),
        m_single_extruder_multi_material(false),
        m_last_acceleration(0), m_max_acceleration(0),m_last_travel_acceleration(0), m_max_travel_acceleration(0),
        m_last_jerk(0), m_max_jerk_x(0), m_max_jerk_y(0),
        m_last_bed_temperature(0), m_last_bed_temperature_reached(true),
        m_lifted(0),
        m_to_lift(0),
        m_to_lift_type(LiftType::NormalLift),
        m_current_speed(3600), m_is_first_layer(true)
        {}
    Extruder*            extruder()             { return m_extruder; }
    const Extruder*      extruder()     const   { return m_extruder; }

    void                 apply_print_config(const PrintConfig &print_config);
    // Extruders are expected to be sorted in an increasing order.
    void                 set_extruders(std::vector<unsigned int> extruder_ids);
    const std::vector<Extruder>& extruders() const { return m_extruders; }
    std::vector<unsigned int> extruder_ids() const { 
        std::vector<unsigned int> out; 
        out.reserve(m_extruders.size()); 
        for (const Extruder &e : m_extruders) 
            out.push_back(e.id()); 
        return out;
    }
    std::string preamble();
    std::string postamble() const;
    static std::string set_temperature(unsigned int temperature, GCodeFlavor flavor, bool wait = false, int tool = -1, std::string comment = std::string());

    std::string set_temperature(unsigned int temperature, bool wait = false, int tool = -1) const;
    std::string set_bed_temperature(int temperature, bool wait = false);
    std::string set_chamber_temperature(int temperature, bool wait = false);
    std::string set_print_acceleration(unsigned int acceleration)   { return set_acceleration_internal(Acceleration::Print, acceleration); }
    std::string set_travel_acceleration(unsigned int acceleration)  { return set_acceleration_internal(Acceleration::Travel, acceleration); }
    std::string set_jerk_xy(double jerk);
    // Orca: set acceleration and jerk in one command for Klipper
    std::string set_accel_and_jerk(unsigned int acceleration, double jerk);
    std::string set_junction_deviation(double junction_deviation); 
    std::string set_pressure_advance(double pa) const;
    std::string set_input_shaping(char axis, float damp, float freq) const;
    std::string reset_e(bool force = false);
    std::string update_progress(unsigned int num, unsigned int tot, bool allow_100 = false) const;
    // return false if this extruder was already selected
    bool        need_toolchange(unsigned int extruder_id) const 
        { return m_extruder == nullptr || m_extruder->id() != extruder_id; }
    std::string set_extruder(unsigned int extruder_id)
        { return this->need_toolchange(extruder_id) ? this->toolchange(extruder_id) : ""; }
    // Prefix of the toolchange G-code line, to be used by the CoolingBuffer to separate sections of the G-code
    // printed with the same extruder.
    std::string toolchange_prefix() const;
    std::string toolchange(unsigned int extruder_id);
    std::string set_speed(double F, const std::string &comment = std::string(), const std::string &cooling_marker = std::string());
    void        set_speed(std::string &out, double F, const std::string &comment = std::string(), const std::string &cooling_marker = std::string());
    // SoftFever NOTE: the returned speed is mm/minute
    double      get_current_speed() const { return m_current_speed;}

    // Snapmaker (feedrate guard) ------------------------------------------------------------
    // Where the feedrate currently being emitted came from. GCode::_extrude sets this right
    // before it hands a speed to the writer and clears it afterwards, so a guard trip can name
    // the offending config key. Plain assignment; no cost unless the guard fires.
    void                  set_feedrate_origin(const FeedrateOrigin &origin) { m_feedrate_origin = origin; }
    void                  clear_feedrate_origin() { m_feedrate_origin.clear(); }
    const FeedrateOrigin& feedrate_origin() const { return m_feedrate_origin; }

    // Preferred substitute when a bad feedrate is refused: the resolved outer wall speed in
    // mm/min. GCode::do_export sets this per object/region. 0 means "not available", in which
    // case the guard falls further down the ladder. Outer wall speed is deliberately chosen over
    // any faster default: a substitution that is too slow only costs print time, while one that
    // is too fast ruins the part.
    void   set_guard_fallback_speed(double F) { m_guard_fallback_speed = (std::isfinite(F) && F > 0.) ? F : 0.; }
    double guard_fallback_speed() const { return m_guard_fallback_speed; }

    // Sink for user-visible reporting of a refused feedrate. See FeedrateGuardReporter.
    void set_feedrate_guard_reporter(FeedrateGuardReporter reporter) { m_feedrate_guard_reporter = std::move(reporter); }
    // Report a refused feedrate through the installed sink (no-op when none is installed).
    void report_bad_feedrate(double bad_value, double substituted) const
        { if (m_feedrate_guard_reporter) m_feedrate_guard_reporter(m_feedrate_origin, bad_value, substituted); }
    // Resolve the substitute for a refused feedrate, in mm/min. See GCodeWriter.cpp for the ladder.
    double resolve_guard_fallback() const;
    // Seed a formatter this writer is about to emit an F word through, so the choke-point clamp
    // in GCodeFormatter::emit_axis substitutes down this writer's ladder instead of the bare
    // last-resort constant. Defined out of line (GCodeFormatter is declared below this class).
    void seed_formatter_guard(class GCodeFormatter &w) const;

    // Snapmaker: flow variant
    double      active_travel_speed(bool first_layer_aware = false) const;

    std::string travel_to_xy(const Vec2d &point, const std::string &comment = std::string());
    std::string travel_to_xyz(const Vec3d &point, const std::string &comment = std::string(), bool force_z = false);
    std::string travel_to_z(double z, const std::string &comment = std::string(), bool force = false);
    bool        will_move_z(double z) const;
    std::string extrude_to_xy(const Vec2d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    //BBS: generate G2 or G3 extrude which moves by arc
    std::string extrude_arc_to_xy(const Vec2d &point, const Vec2d &center_offset, double dE, const bool is_ccw, const std::string &comment = std::string(), bool force_no_extrusion = false);
    std::string extrude_to_xyz(const Vec3d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    // Each appends its line to `out` (Orca #16108). Returning overloads above wrap these.
    void        extrude_to_xy(std::string &out, const Vec2d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    void        extrude_arc_to_xy(std::string &out, const Vec2d &point, const Vec2d &center_offset, double dE, const bool is_ccw, const std::string &comment = std::string(), bool force_no_extrusion = false);
    void        extrude_to_xyz(std::string &out, const Vec3d &point, double dE, const std::string &comment = std::string(), bool force_no_extrusion = false);
    std::string retract(bool before_wipe = false, double retract_length = 0);
    std::string retract_for_toolchange(bool before_wipe = false, double retract_length = 0);
    std::string unretract();
    std::string lift(LiftType lift_type = LiftType::NormalLift, bool spiral_vase = false);
    std::string unlift();
    const Vec3d& get_position() const { return m_pos; }
    Vec3d&       get_position() { return m_pos; }
    void        set_position(const Vec3d& in) { m_pos = in; }
    double      get_zhop() const { return m_lifted; }

    //BBS: set offset for gcode writer
    void set_xy_offset(double x, double y) { m_x_offset = x; m_y_offset = y; }
    Vec2f get_xy_offset() { return Vec2f{m_x_offset, m_y_offset}; };
    // To be called by the CoolingBuffer from another thread.
    static std::string set_fan(const GCodeFlavor gcode_flavor, unsigned int speed);
    // To be called by the main thread. It always emits the G-code, it does not remember the previous state.
    // Keeping the state is left to the CoolingBuffer, which runs asynchronously on another thread.
    std::string set_fan(unsigned int speed) const;
    //BBS: set additional fan speed for BBS machine only
    static std::string set_additional_fan(unsigned int speed);
    static std::string set_exhaust_fan(int speed,bool add_eol);
    //BBS
    void set_object_start_str(std::string start_string) { m_gcode_label_objects_start = start_string; }
    bool is_object_start_str_empty() { return m_gcode_label_objects_start.empty(); }
    void set_object_end_str(std::string end_string) { m_gcode_label_objects_end = end_string; }
    bool is_object_end_str_empty() { return m_gcode_label_objects_end.empty(); }
    void add_object_start_labels(std::string &gcode);
    void add_object_end_labels(std::string &gcode);
    void add_object_change_labels(std::string& gcode);

    //BBS:
    void set_current_position_clear(bool clear) { m_is_current_pos_clear = clear; };
    bool is_current_position_clear() const { return m_is_current_pos_clear; };
    //BBS:
    static bool full_gcode_comment;
    //SoftFever
    void set_is_bbl_machine(bool bval) {m_is_bbl_printers = bval;}
    const bool is_bbl_printers() const {return m_is_bbl_printers;}
    void set_is_first_layer(bool bval) { m_is_first_layer = bval; }
    GCodeFlavor get_gcode_flavor() const { return config.gcode_flavor; }

    // Returns whether this flavor supports separate print and travel acceleration.
    static bool supports_separate_travel_acceleration(GCodeFlavor flavor);
  private:
	// Extruders are sorted by their ID, so that binary search is possible.
    std::vector<Extruder> m_extruders;
    bool            m_single_extruder_multi_material;
    Extruder*       m_extruder;
    unsigned int    m_last_acceleration;
    unsigned int    m_last_travel_acceleration;
    unsigned int    m_max_travel_acceleration;

    // Limit for setting the acceleration, to respect the machine limits set for the Marlin firmware.
    // If set to zero, the limit is not in action.
    unsigned int    m_max_acceleration;
    double          m_max_jerk_x;
    double          m_max_jerk_y;
    double          m_last_jerk;
    double          m_max_jerk_z;
    double          m_max_jerk_e;
    double          m_max_junction_deviation;

    unsigned int  m_travel_acceleration;
    unsigned int  m_travel_jerk;


    //BBS
    unsigned int    m_last_additional_fan_speed;
    int             m_last_bed_temperature;
    bool            m_last_bed_temperature_reached;
    double          m_lifted;

    // BBS
    double          m_to_lift;
    LiftType        m_to_lift_type;
    Vec3d           m_pos = Vec3d::Zero();
    //BBS: this flag is used to indicate whether the m_pos is real.
    //A example that of the first move, the m_pos is zero, but the real position of extruder doesn't
    //Pos must be clear after the first xyz travel move
    bool            m_is_current_pos_clear = false;
    //BBS: x, y offset for gcode generated
    double          m_x_offset{ 0 };
    double          m_y_offset{ 0 };
    
    std::string m_gcode_label_objects_start;
    std::string m_gcode_label_objects_end;

    //SoftFever
    bool            m_is_bbl_printers = false;
    double          m_current_speed;
    bool            m_is_first_layer = true;

    // Snapmaker (feedrate guard): see set_feedrate_origin / set_guard_fallback_speed above.
    FeedrateOrigin        m_feedrate_origin;
    double                m_guard_fallback_speed = 0.;
    FeedrateGuardReporter m_feedrate_guard_reporter;

    enum class Acceleration {
        Travel,
        Print
    };

    std::string _travel_to_z(double z, const std::string &comment);
    std::string _spiral_travel_to_z(double z, const Vec2d &ij_offset, const std::string &comment);
    std::string _retract(double length, double restart_extra, const std::string &comment);
    std::string set_acceleration_internal(Acceleration type, unsigned int acceleration);

};

class GCodeFormatter {
public:
    GCodeFormatter() {
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf;
    }

    GCodeFormatter(const GCodeFormatter&) = delete;
    GCodeFormatter& operator=(const GCodeFormatter&) = delete;

    // At layer height 0.15mm, extrusion width 0.2mm and filament diameter 1.75mm,
    // the crossection of extrusion is 0.4 * 0.15 = 0.06mm2
    // and the filament crossection is 1.75^2 = 3.063mm2
    // thus the filament moves 3.063 / 0.6 = 51x slower than the XY axes
    // and we need roughly two decimal digits more on extruder than on XY.
#if 1
    static constexpr const int XYZF_EXPORT_DIGITS = 3;
    static constexpr const int E_EXPORT_DIGITS    = 5;
#else
    // order of magnitude smaller extrusion rate erros
    static constexpr const int XYZF_EXPORT_DIGITS = 4;
    static constexpr const int E_EXPORT_DIGITS    = 6;
    // excessive accuracy
//    static constexpr const int XYZF_EXPORT_DIGITS = 6;
//    static constexpr const int E_EXPORT_DIGITS    = 9;
#endif
    static constexpr const std::array<double, 10> pow_10 { 1., 10., 100., 1000., 10000., 100000., 1000000., 10000000., 100000000., 1000000000. };
    static constexpr const std::array<double, 10> pow_10_inv { 1. / 1., 1. / 10., 1. / 100., 1. / 1000., 1. / 10000., 1. / 100000., 1. / 1000000., 1. / 10000000., 1. / 100000000., 1. / 1000000000. };

    // Quantize doubles to a resolution of the G-code.
    static double quantize(double v, size_t ndigits) { return std::round(v * pow_10[ndigits]) * pow_10_inv[ndigits]; }
    static double quantize_xyzf(double v) { return quantize(v, XYZF_EXPORT_DIGITS); }
    static double quantize_e(double v) { return quantize(v, E_EXPORT_DIGITS); }

    void emit_axis(const char axis, const double v, size_t digits);

    void emit_xy(const Vec2d &point) {
        this->emit_axis('X', point.x(), XYZF_EXPORT_DIGITS);
        this->emit_axis('Y', point.y(), XYZF_EXPORT_DIGITS);
    }

    void emit_xyz(const Vec3d &point) {
        this->emit_axis('X', point.x(), XYZF_EXPORT_DIGITS);
        this->emit_axis('Y', point.y(), XYZF_EXPORT_DIGITS);
        this->emit_z(point.z());
    }

    void emit_z(const double z) {
        this->emit_axis('Z', z, XYZF_EXPORT_DIGITS);
    }

    void emit_e(double v) {
        this->emit_axis('E', v, E_EXPORT_DIGITS);
    }

    void emit_f(double speed) {
        this->emit_axis('F', speed, XYZF_EXPORT_DIGITS);
    }

    // Snapmaker (feedrate guard): substitute used by emit_axis() when a caller tries to emit a
    // non-finite or non-positive F word. Formerly a mutable static shared by every formatter
    // instance, which was a latent data race: G-code emission happens to be single-threaded today
    // (the pipeline's generator stage is slic3r_tbb_filtermode::serial_in_order, GCode.cpp), but
    // nothing enforced that, and parallelising it would have corrupted the fallback silently.
    // It is now per-instance and explicitly seeded by whoever constructs the formatter (see
    // GCodeWriter::set_speed and GCode::_extrude), so there is no shared mutable state at all.
    // 0 means "no substitute supplied"; emit_axis then falls back to GUARD_LAST_RESORT_FEEDRATE.
    void   set_fallback_feedrate(double F) { m_fallback_feedrate = (std::isfinite(F) && F > 0.) ? F : 0.; }
    double fallback_feedrate() const { return m_fallback_feedrate; }

    // Absolute last-resort feedrate (mm/min) when no better substitute is known anywhere in the
    // ladder. 600 mm/min = 10 mm/s: slow enough to be harmless on any machine, and slow enough
    // that it is conspicuous if it ever reaches a real print.
    static constexpr const double GUARD_LAST_RESORT_FEEDRATE = 600.;
    //BBS
    void emit_ij(const Vec2d &point) {
        this->emit_axis('I', point.x(), XYZF_EXPORT_DIGITS);
        this->emit_axis('J', point.y(), XYZF_EXPORT_DIGITS);
    }

    void emit_string(const std::string &s) {
        strncpy(ptr_err.ptr, s.c_str(), s.size());
        ptr_err.ptr += s.size();
    }

    void emit_comment(bool allow_comments, const std::string &comment) {
        if (allow_comments && ! comment.empty()) {
            *ptr_err.ptr ++ = ' '; *ptr_err.ptr ++ = ';'; *ptr_err.ptr ++ = ' ';
            this->emit_string(comment);
        }
    }

    std::string string() {
        std::string out;
        this->append_to(out);
        return out;
    }

    void append_to(std::string &out) {
        *ptr_err.ptr ++ = '\n';
        out.append(this->buf, ptr_err.ptr - buf);
    }

protected:
    static constexpr const size_t   buflen = 256;
    char                            buf[buflen];
    char* buf_end;
    std::to_chars_result            ptr_err;
    // Snapmaker (feedrate guard): per-instance substitute, see set_fallback_feedrate().
    double                          m_fallback_feedrate = 0.;
};

class GCodeG1Formatter : public GCodeFormatter {
public:
    GCodeG1Formatter() {
        this->buf[0] = 'G';
        this->buf[1] = '1';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG1Formatter(const GCodeG1Formatter&) = delete;
    GCodeG1Formatter& operator=(const GCodeG1Formatter&) = delete;
};

class GCodeG2G3Formatter : public GCodeFormatter {
public:
    GCodeG2G3Formatter(bool is_ccw) {
        this->buf[0] = 'G';
        this->buf[1] = is_ccw ? '3' : '2';
        this->buf_end = buf + buflen;
        this->ptr_err.ptr = this->buf + 2;
    }

    GCodeG2G3Formatter(const GCodeG2G3Formatter&) = delete;
    GCodeG2G3Formatter& operator=(const GCodeG2G3Formatter&) = delete;
};

} /* namespace Slic3r */

#endif /* slic3r_GCodeWriter_hpp_ */
