#include <catch2/catch.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

SCENARIO("lift() is not ignored after unlift() at normal values of Z", "[GCodeWriter]") {
    GIVEN("A config from a file and a single extruder.") {
        GCodeWriter writer;
        GCodeConfig &config = writer.config;
        // ConfigBase::load() dispatches on the file extension and, since Bambu
        // commented load_from_ini() out of it (Config.cpp:783), rejects .ini with
        // "unsupported format for config file" and returns an EMPTY config - which
        // left z_hop at its 0 default and made every lift() below emit nothing.
        // load_from_ini() itself is still present and works, so call it directly.
        config.load_from_ini(std::string(TEST_DATA_DIR) + "/fff_print_tests/test_gcodewriter/config_lift_unlift.ini", ForwardCompatibilitySubstitutionRule::Disable);

        // FORK BEHAVIOUR (Bambu "lazy lift"): GCodeWriter::lift() no longer emits the Z
        // move itself. Unless spiral_vase is set it only records the pending lift in
        // m_to_lift (GCodeWriter.cpp:866-875) and returns "", leaving travel_to_xy() to
        // fold the hop into the next travel move. The upstream contract this scenario was
        // written against - "lift() returns g-code" - therefore only holds on the
        // spiral_vase path, which is what these calls now exercise. The property under
        // test is unchanged: a lift after an unlift must not be ignored.
        std::vector<unsigned int> extruder_ids {0};
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);

        WHEN("Z is set to 203") {
            double trouble_Z = 203;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 500003") {
            double trouble_Z = 500003;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 10.3") {
            double trouble_Z = 10.3;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
		// The test above will fail for trouble_Z == 9007199254740992, where trouble_Z + 1.5 will be rounded to trouble_Z + 2.0 due to double mantisa overflow.
    }
}

SCENARIO("set_speed emits values with fixed-point output.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 99999.123") {
            THEN("Output string is G1 F99999.123") {
                REQUIRE_THAT(writer.set_speed(99999.123), Catch::Equals("G1 F99999.123\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200522") {
            THEN("Output string is G1 F203.201") {
                REQUIRE_THAT(writer.set_speed(203.200522), Catch::Equals("G1 F203.201\n"));
            }
        }
    }
}

// Orca #15848: custom G-code e_retracted R/W must use the same retract storage as
// retract()/unretract(). Edge keeps a single static m_share_retracted (not Orca's
// per-physical vector / filament_map / extruder_id()), so these cases cover shared
// vs per-filament and relative vs absolute E against that scalar model.
//
// Deferred from upstream:
// - "follows the physical extruder mapping" — needs filament_map + vector share.
// - "Start G-code retraction is repaid..." — needs multifilament_config + a full
//   slice with placeholder start G-code. Unit-level retract/unretract below is
//   the Edge stand-in.

namespace {

void parse_retract_unretract(const std::string &gcode, const GCodeConfig &config, double &retracted, double &unretracted)
{
    retracted   = 0.;
    unretracted = 0.;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.retracting(self))
            retracted -= line.dist_E(self);
        else if (line.extruding(self))
            unretracted += line.dist_E(self);
    });
}

} // namespace

TEST_CASE("Custom retraction state controls generated retract and unretract moves", "[GCodeWriter][Retraction]")
{
    const bool   shared            = GENERATE(false, true);
    const bool   relative_e        = GENERATE(false, true);
    const double custom_retraction = GENERATE(0., 0.5, 0.8);
    CAPTURE(shared, relative_e, custom_retraction);

    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = shared;
    writer.config.use_relative_e_distances.value       = relative_e;
    writer.config.retraction_length.values             = {0.6};
    writer.config.retract_restart_extra.values         = {0.};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(custom_retraction, 0.);
    std::string    gcode            = writer.retract();
    const double   total_retraction = std::max(custom_retraction, 0.6);
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(total_retraction, 1e-9));
    gcode += writer.unretract();
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(0., 1e-9));

    double retracted = 0., unretracted = 0.;
    parse_retract_unretract(gcode, writer.config, retracted, unretracted);
    CHECK_THAT(retracted, WithinAbs(total_retraction - custom_retraction, 1e-6));
    CHECK_THAT(unretracted, WithinAbs(total_retraction, 1e-6));
}

TEST_CASE("Custom retraction state uses the shared SEMM scalar", "[GCodeWriter][Retraction]")
{
    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = true;
    writer.config.use_relative_e_distances.value       = true;
    writer.config.retraction_length.values             = {0.6};
    writer.config.retract_restart_extra.values         = {0.};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(0.5, 0.2);
    // One static m_share_retracted: every SEMM filament sees the same length.
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruder()->unretract(), WithinAbs(0.7, 1e-9));
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0., 1e-9));

    writer.extruder()->set_retracted(0.5, 0.2);
    writer.extruder()->set_retracted(0., 0.2);
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruder()->restart_extra(), WithinAbs(0., 1e-9));
    CHECK(writer.unretract().empty());
}

TEST_CASE("Custom retraction state stays per-filament when SEMM is off", "[GCodeWriter][Retraction]")
{
    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = false;
    writer.config.use_relative_e_distances.value       = true;
    writer.config.retraction_length.values             = {0.6};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(0.5, 0.2);
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruder()->restart_extra(), WithinAbs(0.2, 1e-9));
}

TEST_CASE("GCodeWriter::toolchange throws when the extruder is not registered", "[GCodeWriter][GCode]")
{
    GCodeWriter writer;
    writer.set_extruders({0});
    REQUIRE_THROWS_AS(writer.toolchange(2), SlicingError);
    REQUIRE(writer.extruder() == nullptr);
}

TEST_CASE("Acceleration and velocity limit commands print their values in general notation", "[GCodeWriter]")
{
    enum class Command { Print, Travel, KlipperLimits };
    struct Case
    {
        GCodeFlavor              flavor;
        Command                  command;
        unsigned int             acceleration;
        double                   jerk;
        bool                     comments;
        std::vector<std::string> present;
        std::vector<std::string> absent;
    };
    // accel_to_decel_factor is 50%, so ACCEL_TO_DECEL is half the acceleration.
    const Case cases[] = {
        {gcfKlipper, Command::KlipperLimits, 2000000, 25. / 3., false,
         {"SET_VELOCITY_LIMIT ACCEL=2000000 ", "ACCEL_TO_DECEL=1e+06 ", "SQUARE_CORNER_VELOCITY=8.33333\n"}, {}},
        {gcfKlipper, Command::KlipperLimits, 12345, 0., false, {"ACCEL=12345 ", "ACCEL_TO_DECEL=6172.5\n"}, {"SQUARE_CORNER_VELOCITY"}},
        {gcfKlipper, Command::KlipperLimits, 0, 0.25, true, {"SQUARE_CORNER_VELOCITY=0.25 ", "; adjust VELOCITY_LIMIT"}, {"ACCEL"}},
        {gcfKlipper, Command::Print, 3001, 0., true, {"ACCEL=3001 ", "ACCEL_TO_DECEL=1500.5 ", "; adjust ACCEL_TO_DECEL", "; adjust acceleration"}, {}},
        {gcfMarlinFirmware, Command::Print, 2500, 0., false, {"M204 P2500\n"}, {}},
        {gcfMarlinFirmware, Command::Travel, 7000, 0., false, {"M204 T7000\n"}, {}},
        {gcfRepRapFirmware, Command::Travel, 7000, 0., true, {"M204 T7000 ", "; adjust acceleration"}, {}},
        {gcfMarlinLegacy, Command::Print, 2500, 0., false, {"M204 S2500\n"}, {}},
        {gcfRepetier, Command::Print, 2500, 0., false, {"M201 X2500 Y2500\n"}, {}},
        {gcfRepetier, Command::Travel, 7000, 0., false, {"M202 X7000 Y7000\n"}, {}},
    };

    for (const Case &c : cases) {
        DYNAMIC_SECTION("flavor " << int(c.flavor) << " command " << int(c.command) << " accel " << c.acceleration) {
            struct CommentGuard
            {
                bool saved = GCodeWriter::full_gcode_comment;
                ~CommentGuard() { GCodeWriter::full_gcode_comment = saved; }
            } comment_guard;
            GCodeWriter::full_gcode_comment = c.comments;

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(c.flavor));
            config.option<ConfigOptionBools>("accel_to_decel_enable")->values.assign(1, true);
            config.option<ConfigOptionPercents>("accel_to_decel_factor")->values.assign(1, 50.);
            for (const char *limit : {"machine_max_acceleration_extruding", "machine_max_acceleration_travel", "machine_max_acceleration_x",
                                      "machine_max_acceleration_y", "machine_max_jerk_x", "machine_max_jerk_y"}) {
                std::vector<double> &values = config.option<ConfigOptionFloats>(limit)->values;
                std::fill(values.begin(), values.end(), 0.);
            }
            PrintConfig print_config;
            print_config.apply(config, true);
            GCodeWriter writer;
            writer.apply_print_config(print_config);

            const std::string line = c.command == Command::Print         ? writer.set_print_acceleration(c.acceleration) :
                                     c.command == Command::Travel        ? writer.set_travel_acceleration(c.acceleration) :
                                                                           writer.set_accel_and_jerk(c.acceleration, c.jerk);
            INFO(line);
            for (const std::string &token : c.present)
                CHECK(line.find(token) != std::string::npos);
            for (const std::string &token : c.absent)
                CHECK(line.find(token) == std::string::npos);
        }
    }
}

TEST_CASE("GCodeWriter append overloads emit the same line as the returning overloads", "[GCodeWriter]")
{
    GCodeWriter writer;
    std::string appended;
    writer.set_speed(appended, 1800.);
    CHECK(appended == writer.set_speed(1800.));
}
