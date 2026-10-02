#include <catch2/catch.hpp>

#include <algorithm>
#include <memory>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"

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
