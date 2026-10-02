#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// H2D timelapse, two colours (2026-10-01, tests/h2d_timelapse.gcode.3mf plate 6): blue on the left
// nozzle (the most used one, so the photo nozzle), white on the right. With traditional timelapse the
// left-nozzle layers were photographed with a bare "M9711 M0 E1 Z.." (no position: the head stayed
// where the layer ended) and every right-nozzle layer with the template's park branch (Z lift,
// "G1 Y295", "G1 Y265"). The fork had no TimelapsePosPicker, so has_timelapse_safe_pos was always 0
// and the H2D time_lapse_gcode fell into its "no safe position" branches, which differ per nozzle.
// Bambu Studio 2.8 gives every photo a position: inline at the layer's farthest point (M971) when the
// photo nozzle prints it, otherwise M9711 with the safe spot (U/V) the picker chooses near it.

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("h2d_timelapse_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

// Filament 1 on the left nozzle, filament 2 on the right (filament_map is 1-based: 1 = left, 2 = right).
DynamicPrintConfig h2d_config(const std::string &timelapse_type, bool tower, bool farthest_point)
{
    PresetBundle &b = bbl_bundle();
    const std::string filament = "Bambu PLA Basic @BBL H2D";
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#1943E0", "#FFFFFF" });
    b.filament_presets = std::vector<std::string>(2, filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    // The machine profile turns farthest-point timelapse on, as Bambu Studio 2.8's does.
    REQUIRE(cfg.opt_bool("farthest_point_timelapse"));
    // Bambu Studio's clearance (its picker's) is loaded as its own key; by-object collision keeps
    // the profile's extruder_clearance_radius as before.
    REQUIRE(cfg.opt_float("extruder_clearance_max_radius") == Approx(96.));
    REQUIRE(cfg.opt_float("extruder_clearance_radius") == Approx(49.));
    cfg.set_deserialize_strict({
        { "timelapse_type", timelapse_type },
        { "farthest_point_timelapse", farthest_point ? "1" : "0" },
        { "enable_prime_tower", tower ? "1" : "0" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 250. },
        { "wipe_tower_rotation_angle", 0 },
        { "filament_map_mode", "Manual" },
        { "filament_map", "1,2" },
        { "gcode_comments", 0 },
    });
    return cfg;
}

// One object: a 20 x 20 x 6 mm block in filament 1 (left nozzle) with a 20 x 20 x 2 mm block in
// filament 2 (right nozzle) on top, so the left nozzle prints 30 layers alone, then the right one 10.
void add_two_colour_tower(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "two_colour";
    TriangleMesh lower  = Test::mesh(Test::TestMesh::cube_20x20x20);
    lower.scale(Vec3f(1.f, 1.f, 0.3f));
    TriangleMesh upper = Test::mesh(Test::TestMesh::cube_20x20x20);
    upper.scale(Vec3f(1.f, 1.f, 0.1f));
    upper.translate(0.f, 0.f, 6.f);
    ModelVolume *lower_volume = object->add_volume(lower);
    lower_volume->name        = "blue";
    lower_volume->config.set("extruder", 1);
    ModelVolume *upper_volume = object->add_volume(upper);
    upper_volume->name        = "white";
    upper_volume->config.set("extruder", 2);
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(175., 160., 0.));
    object->ensure_on_bed();
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// One timelapse block (time_lapse_gcode between "; SKIPTYPE: timelapse" and "; SKIPPABLE_END").
struct Photo
{
    int         layer    = -1;
    int         filament = -1; // active filament (0-based) when the block was emitted
    bool        inline_photo = false; // M971 only: the shutter where the head is
    bool        positioned   = false; // M9711 with a U/V (or X/Y) safe position
    bool        bare_m9711   = false; // M9711 without a position
    bool        parked       = false; // the template's own park move (G1 Y295)
    bool        lifted       = false; // a Z lift inside the block
    std::string m9711;
};

std::vector<Photo> collect_photos(const std::string &gcode, int &layer_count)
{
    static const std::regex re_t(R"(^T(\d+)( H-?\d+)?$)");
    std::vector<Photo> photos;
    std::istringstream ss(gcode);
    int   layer    = -1;
    int   filament = -1;
    bool  in_block = false;
    Photo current;
    std::vector<std::string> block;
    bool  in_config = false;
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("; CONFIG_BLOCK_START", 0) == 0)
            in_config = true;
        if (in_config) {
            if (line.rfind("; CONFIG_BLOCK_END", 0) == 0)
                in_config = false;
            continue;
        }
        const size_t first = line.find_first_not_of(' ');
        const std::string s = first == std::string::npos ? std::string() : line.substr(first);
        if (s.rfind("; CHANGE_LAYER", 0) == 0) {
            ++layer;
            continue;
        }
        std::smatch m;
        if (std::regex_match(s, m, re_t)) {
            const int t = std::stoi(m[1].str());
            if (t < 255)
                filament = t;
        }
        if (s.rfind("; SKIPTYPE: timelapse", 0) == 0) {
            in_block = true;
            current  = Photo();
            current.layer    = layer;
            current.filament = filament;
            block.clear();
            continue;
        }
        if (!in_block)
            continue;
        if (s.rfind("; SKIPPABLE_END", 0) != 0) {
            block.push_back(s);
            continue;
        }
        in_block = false;
        // The H2D template renders two firmware branches, "M622 J0" (firmware without safe-position
        // support) and "M622 J1" (current firmware); judge what the current firmware runs: the lines
        // after the last "M622 J1" (the only one, after the record flag, for an inline photo).
        size_t from = 0;
        for (size_t i = 0; i < block.size(); ++i)
            if (block[i].rfind("M622 J1", 0) == 0)
                from = i + 1;
        bool shutter = false;
        for (size_t i = from; i < block.size(); ++i) {
            const std::string &l = block[i];
            if (l.rfind("M9711", 0) == 0) {
                current.m9711      = l;
                const bool has_pos = l.find(" U") != std::string::npos || l.find(" X") != std::string::npos;
                current.positioned = current.positioned || has_pos;
                current.bare_m9711 = current.bare_m9711 || !has_pos;
            } else if (l.rfind("M971 ", 0) == 0) {
                shutter = true;
            } else if (l.rfind("G1 Y295", 0) == 0) {
                current.parked = true;
            } else if (l.rfind("G1 Z", 0) == 0) {
                current.lifted = true;
            }
        }
        current.inline_photo = shutter && current.m9711.empty();
        photos.push_back(current);
    }
    layer_count = layer + 1;
    return photos;
}

// U/V of a positioned M9711 ("M9711 M0 E1 U12 V34 Z.."), as text.
// Distance (mm, per axis, the larger) from a safe spot "U..,V.." to the object's footprint,
// 20 x 20 mm centred on (175, 160).
double clearance_from_object(const std::string &spot)
{
    const size_t comma = spot.find(',');
    REQUIRE(comma != std::string::npos);
    const double u  = std::stod(spot.substr(0, comma));
    const double v  = std::stod(spot.substr(comma + 1));
    const double dx = std::max({ 165. - u, u - 185., 0. });
    const double dy = std::max({ 150. - v, v - 170., 0. });
    return std::max(dx, dy);
}

// Bambu Studio's picker grows each object's footprint by half of extruder_clearance_max_radius
// (96 mm on the H2D -> 48 mm; it used extruder_clearance_radius, 49 -> 24.5 mm, before). The spot is
// a whole millimetre, so allow 1 mm of rounding.
constexpr double H2D_MIN_SPOT_CLEARANCE = 96. / 2. - 1.;

std::string safe_spot(const std::string &m9711)
{
    static const std::regex re(R"( U(-?\d+) V(-?\d+))");
    std::smatch m;
    return std::regex_search(m9711, m, re) ? m[1].str() + "," + m[2].str() : std::string();
}

} // namespace

SCENARIO("H2D timelapse photographs right-nozzle layers like left-nozzle layers", "[H2DTimelapse]")
{
    for (const bool tower : { false, true }) {
        GIVEN(std::string("traditional timelapse, farthest point off (Bambu Studio's picker alone), prime tower ") + (tower ? "on" : "off"))
        {
            Print  print;
            Model  model;
            add_two_colour_tower(model);
            const std::string gcode = slice(print, model, h2d_config("0", tower, false));
            int               layers = 0;
            const std::vector<Photo> photos = collect_photos(gcode, layers);
            REQUIRE(layers == 40);

            THEN("one photo per layer, each at a picked safe spot, for both nozzles alike")
            {
                REQUIRE(photos.size() == size_t(layers));
                std::set<int> filaments;
                for (const Photo &p : photos) {
                    CAPTURE(p.layer, p.filament, p.m9711);
                    filaments.insert(p.filament);
                    CHECK(p.positioned);
                    CHECK_FALSE(p.bare_m9711);
                    CHECK_FALSE(p.parked);
                    CHECK_FALSE(p.lifted);
                    if (p.positioned)
                        CHECK(clearance_from_object(safe_spot(p.m9711)) >= H2D_MIN_SPOT_CLEARANCE);
                }
                // Both nozzles were photographed (the left one for 30 layers, the right one for 10).
                CHECK(filaments == std::set<int>{ 0, 1 });
            }
        }

        GIVEN(std::string("traditional timelapse with the profile's farthest-point mode (Bambu Studio 2.8), prime tower ") + (tower ? "on" : "off"))
        {
            Print  print;
            Model  model;
            add_two_colour_tower(model);
            const std::string gcode = slice(print, model, h2d_config("0", tower, true));
            int               layers = 0;
            const std::vector<Photo> photos = collect_photos(gcode, layers);
            REQUIRE(layers == 40);

            THEN("left (photo) nozzle layers shoot inline at the farthest point; right nozzle layers from a safe spot; nothing parks")
            {
                REQUIRE(photos.size() == size_t(layers));
                for (const Photo &p : photos) {
                    CAPTURE(p.layer, p.filament, p.m9711);
                    CHECK_FALSE(p.parked);
                    CHECK_FALSE(p.bare_m9711);
                    CHECK_FALSE(p.lifted);
                    if (p.layer < 30)
                        CHECK(p.inline_photo);      // the left nozzle prints the farthest point
                    else if (p.layer >= 31)
                        CHECK(p.positioned);        // the right nozzle: M9711 at a safe spot
                    CHECK((p.inline_photo || p.positioned));
                    if (p.positioned)
                        CHECK(clearance_from_object(safe_spot(p.m9711)) >= H2D_MIN_SPOT_CLEARANCE);
                }
            }
        }
    }

    GIVEN("smooth timelapse (prime tower on)")
    {
        Print  print;
        Model  model;
        add_two_colour_tower(model);
        const std::string gcode = slice(print, model, h2d_config("1", true, true));
        int               layers = 0;
        const std::vector<Photo> photos = collect_photos(gcode, layers);
        REQUIRE(layers == 40);

        THEN("above the 5 mm nozzle height gap every layer shares one safe spot, whichever nozzle prints")
        {
            REQUIRE(photos.size() == size_t(layers));
            std::set<std::string> spots_above_gap;
            for (const Photo &p : photos) {
                CAPTURE(p.layer, p.filament, p.m9711);
                CHECK(p.m9711.find("M9711 M1 ") == 0);
                const double z = 0.2 * (p.layer + 1);
                if (std::abs(z - 5.0) < 0.05)
                    continue; // at the gap itself, float rounding decides
                if (z > 5.0 + 1e-6) {
                    CHECK(p.positioned);
                    CHECK_FALSE(p.parked);
                    spots_above_gap.insert(safe_spot(p.m9711));
                    CHECK(clearance_from_object(safe_spot(p.m9711)) >= H2D_MIN_SPOT_CLEARANCE);
                } else {
                    // Below the gap Bambu Studio sends both nozzles to the chute (no safe spot).
                    CHECK_FALSE(p.positioned);
                    CHECK(p.parked);
                }
            }
            CHECK(spots_above_gap.size() == 1);
            CHECK_FALSE(spots_above_gap.begin()->empty());
        }
    }
}
