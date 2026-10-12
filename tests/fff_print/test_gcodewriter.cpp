#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <memory>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"

using namespace Slic3r;

TEST_CASE("Z travel respects the machine speed limit", "[GCodeWriter]") {
    struct SpeedCase {
        double travel_speed;
        double travel_speed_z;
        double max_speed_z;
        std::string feedrate;
    };
    const SpeedCase cases[] = {
        {100., 0., 40., " F2400"},
        {30., 0., 40., " F1800"},
        {100., 70., 40., " F2400"},
        {100., 20., 40., " F1200"},
        {100., 0., 0., " F6000"},
    };

    for (const SpeedCase &speed_case : cases) {
        for (bool emit_limits : {false, true}) {
            CAPTURE(speed_case.travel_speed, speed_case.travel_speed_z, speed_case.max_speed_z, emit_limits);
            PrintConfig config = static_cast<const PrintConfig &>(FullPrintConfig::defaults());
            config.travel_speed.values = {speed_case.travel_speed};
            config.travel_speed_z.value = speed_case.travel_speed_z;
            config.machine_max_speed_z.values = {speed_case.max_speed_z, 5.};
            config.emit_machine_limits_to_gcode.value = emit_limits;
            config.z_hop.values = {1.5};
            config.retract_lift_above.values = {0.};
            config.retract_lift_below.values = {0.};

            GCodeWriter writer;
            writer.apply_print_config(config);
            writer.set_is_first_layer(false);
            writer.set_extruders({0});
            writer.set_extruder(0);

            REQUIRE(writer.travel_to_z(10.) == "G1 Z10" + speed_case.feedrate + "\n");
            REQUIRE(writer.travel_to_z(9.) == "G1 Z9" + speed_case.feedrate + "\n");

            writer.set_current_position_clear(true);
            writer.lift();
            const std::string hop = writer.travel_to_xyz(Vec3d(10., 0., 9.));
            REQUIRE(hop.find("G1 Z10.5" + speed_case.feedrate) != std::string::npos);
            REQUIRE(hop.find("X10 Y0 Z10.5 F" + std::to_string(int(speed_case.travel_speed * 60.))) != std::string::npos);
            REQUIRE(writer.unlift().find("G1 Z9" + speed_case.feedrate) == 0);

            writer.lift();
            REQUIRE(writer.travel_to_xyz(Vec3d(10., 0., 9.2)).find("G1 X10 Y0 Z10.5" + speed_case.feedrate) == 0);
            REQUIRE(writer.unlift().find("G1 Z9.2" + speed_case.feedrate) == 0);
            writer.travel_to_z(9.);

            REQUIRE(writer.lift(LiftType::NormalLift, true).find("G1 Z10.5" + speed_case.feedrate) == 0);
            REQUIRE(writer.unlift().find("G1 Z9" + speed_case.feedrate) == 0);

            writer.lift(LiftType::SpiralLift);
            const std::string spiral = writer.travel_to_xyz(Vec3d(20., 0., 9.));
            REQUIRE(spiral.find("G17\nG3") == 0);
            REQUIRE(spiral.find(speed_case.feedrate) != std::string::npos);
            REQUIRE(writer.unlift().find("G1 Z9" + speed_case.feedrate) == 0);

            REQUIRE(writer.travel_to_xyz(Vec3d(20., 0., 8.)) == "G1 X20 Y0 Z8" + speed_case.feedrate + "\n");
            writer.set_current_position_clear(false);
            REQUIRE(writer.travel_to_xyz(Vec3d(30., 0., 7.)).find("G1 Z7" + speed_case.feedrate) != std::string::npos);
        }
    }
}

TEST_CASE("Z travel preserves first-layer speed and refreshes the machine limit", "[GCodeWriter]") {
    PrintConfig config = static_cast<const PrintConfig &>(FullPrintConfig::defaults());
    config.travel_speed.values = {100.};
    config.travel_speed_z.value = 0.;
    config.machine_max_speed_z.values = {40.};
    config.initial_layer_travel_speed.values.front().value = 25.;
    config.initial_layer_travel_speed.values.front().percent = true;

    GCodeWriter writer;
    writer.apply_print_config(config);
    REQUIRE(writer.travel_to_z(1.) == "G1 Z1 F1500\n");
    writer.set_is_first_layer(false);
    REQUIRE(writer.travel_to_z(2.) == "G1 Z2 F2400\n");

    config.machine_max_speed_z.values = {10.};
    writer.apply_print_config(config);
    REQUIRE(writer.travel_to_z(3.) == "G1 Z3 F600\n");
    REQUIRE(writer.travel_to_xy(Vec2d(10., 0.)) == "G1 X10 Y0 F6000\n");
}

TEST_CASE("Combined XYZ travel limits its Z velocity without slowing shallow moves", "[GCodeWriter]") {
    PrintConfig config = static_cast<const PrintConfig &>(FullPrintConfig::defaults());
    config.travel_speed.values = {100.};
    config.travel_speed_z.value = 0.;
    config.machine_max_speed_z.values = {40.};
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_is_first_layer(false);
    writer.set_current_position_clear(true);

    REQUIRE(writer.travel_to_xyz(Vec3d(10., 0., 10.)) == "G1 X10 Y0 Z10 F3394.112\n");
    REQUIRE(writer.travel_to_xyz(Vec3d(20., 0., 0.)) == "G1 X20 Y0 Z0 F3394.112\n");
    REQUIRE(writer.travel_to_xyz(Vec3d(120., 0., 1.)) == "G1 X120 Y0 Z1 F6000\n");
    REQUIRE(writer.travel_to_xyz(Vec3d(220., 0., 1.)) == "G1 X220 Y0 F6000\n");

    config.machine_max_speed_z.values = {0.};
    writer.apply_print_config(config);
    REQUIRE(writer.travel_to_xyz(Vec3d(230., 0., 11.)) == "G1 X230 Y0 Z11 F6000\n");

    config.machine_max_speed_z.values = {40.};
    writer.apply_print_config(config);
    writer.set_position(Vec3d(0.00049, 0., 0.00049));
    GCodeReader reader;
    reader.parse_buffer(writer.travel_to_xyz(Vec3d(1.00049, 0., 1.00051)),
        [](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            const double dz = std::abs(line.dist_Z(reader));
            const double distance = std::hypot(line.dist_XY(reader), dz);
            REQUIRE(line.f() / 60. * dz / distance <= 40. + 0.0001);
        });
}

TEST_CASE("Each Z-hop segment respects the Z velocity limit", "[GCodeWriter]") {
    for (LiftType type : {LiftType::NormalLift, LiftType::LazyLift, LiftType::SpiralLift}) {
        for (double distance : {0.5, 10., 200.}) {
            CAPTURE(type, distance);
            PrintConfig config = static_cast<const PrintConfig &>(FullPrintConfig::defaults());
            config.travel_speed.values = {100.};
            config.travel_speed_z.value = 0.;
            config.machine_max_speed_z.values = {40.};
            config.z_hop.values = {1.5};
            config.travel_slope.values = {45.};
            config.retract_lift_above.values = {0.};
            config.retract_lift_below.values = {0.};
            GCodeWriter writer;
            writer.apply_print_config(config);
            writer.set_is_first_layer(false);
            writer.set_extruders({0});
            writer.set_extruder(0);
            writer.set_current_position_clear(true);
            std::string gcode = writer.travel_to_xyz(Vec3d(0., 0., 10.));
            writer.lift(type);
            gcode += writer.travel_to_xyz(Vec3d(distance, distance, 10.));
            gcode += writer.unlift();

            int z_moves = 0;
            GCodeReader reader;
            reader.parse_buffer(gcode, [&z_moves](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
                if (line.cmd_is("G1") && line.dist_Z(reader) != 0.) {
                    ++z_moves;
                    const double dz = std::abs(line.dist_Z(reader));
                    const double distance = std::hypot(line.dist_XY(reader), dz);
                    REQUIRE(line.f() / 60. * dz / distance <= 40. + 0.0001);
                }
            });
            REQUIRE(z_moves >= 2);
            if (type == LiftType::NormalLift || distance > 10.)
                REQUIRE(gcode.find(" F6000") != std::string::npos);
        }
    }
}

TEST_CASE("Deferred layer changes preserve the hop height and cap the physical Z move", "[GCodeWriter]") {
    PrintConfig config = static_cast<const PrintConfig &>(FullPrintConfig::defaults());
    config.travel_speed.values = {100.};
    config.machine_max_speed_z.values = {40.};
    config.travel_speed_z.value = 0.;
    config.z_hop.values = {0.4};
    config.retract_lift_above.values = {0.};
    config.retract_lift_below.values = {0.};
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_is_first_layer(false);
    writer.set_extruders({0});
    writer.set_extruder(0);
    writer.set_position(Vec3d(167.804, 167.985, 0.3));
    writer.set_current_position_clear(true);
    REQUIRE(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true) ==
            "G1 X167.921 Y167.804 Z.5 F3528.285\n");

    writer.set_position(Vec3d(167.804, 167.985, 0.3));
    writer.lift();
    REQUIRE(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true).find("G1 Z.9 F2400") == 0);
    REQUIRE(std::abs(writer.get_zhop() - 0.4) < 0.000001);
    REQUIRE(writer.unlift().find("G1 Z.5 F2400") == 0);
}

SCENARIO("lift() is not ignored after unlift() at normal values of Z", "[GCodeWriter]") {
    GIVEN("A config with z-hop and a single extruder.") {
        GCodeWriter writer;
        writer.apply_print_config(static_cast<const PrintConfig &>(FullPrintConfig::defaults()));
        GCodeConfig &config = writer.config;
        config.z_hop.values = { 1.5 };
        config.retract_lift_above.values = { 0.0 };
        config.retract_lift_below.values = { 0.0 };

        std::vector<unsigned int> extruder_ids {0};
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);

        WHEN("Z is set to 203") {
            double trouble_Z = 203;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                writer.lift();
                REQUIRE(writer.travel_to_xyz(Vec3d(1, 0, trouble_Z)).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() schedules a new lift.") {
                            writer.lift();
                            REQUIRE(writer.travel_to_xyz(Vec3d(2, 0, trouble_Z + config.z_hop.values[0])).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 500003") {
            double trouble_Z = 500003;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                writer.lift();
                REQUIRE(writer.travel_to_xyz(Vec3d(1, 0, trouble_Z)).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() schedules a new lift.") {
                            writer.lift();
                            REQUIRE(writer.travel_to_xyz(Vec3d(2, 0, trouble_Z + config.z_hop.values[0])).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 10.3") {
            double trouble_Z = 10.3;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                writer.lift();
                REQUIRE(writer.travel_to_xyz(Vec3d(1, 0, trouble_Z)).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() schedules a new lift.") {
                            writer.lift();
                            REQUIRE(writer.travel_to_xyz(Vec3d(2, 0, trouble_Z + config.z_hop.values[0])).size() > 0);
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
