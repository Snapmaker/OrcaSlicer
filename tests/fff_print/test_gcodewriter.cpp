#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <memory>

#include "libslic3r/GCodeWriter.hpp"

using namespace Slic3r;

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

// ---------------------------------------------------------------------------
// GCodeWriter 指令矩阵：逐指令的精确 golden 输出。
// ---------------------------------------------------------------------------

// Configure a writer with a specific G-code flavor (and optional relative-E mode).
static void configure_writer(GCodeWriter &writer, GCodeFlavor flavor, bool relative_e = false)
{
    writer.apply_print_config(static_cast<const PrintConfig &>(FullPrintConfig::defaults()));
    writer.config.gcode_flavor.value = flavor;
    writer.config.use_relative_e_distances.value = relative_e;
}

// Configure retraction parameters and attach a single extruder.
static void configure_retraction(GCodeWriter &writer)
{
    writer.config.retraction_length.values = { 1.5 };
    writer.config.retract_restart_extra.values = { 0.2 };
    writer.config.retraction_speed.values = { 30.0 };
    writer.config.deretraction_speed.values = { 25.0 };
    writer.set_extruders({ 0 });
    writer.set_extruder(0);
}

SCENARIO("set_pressure_advance emits flavor-specific instruction", "[GCodeWriter]") {
    GIVEN("a Klipper-flavor writer") {
        GCodeWriter writer;
        configure_writer(writer, gcfKlipper);
        THEN("it emits SET_PRESSURE_ADVANCE") {
            REQUIRE(writer.set_pressure_advance(0.05) == "SET_PRESSURE_ADVANCE ADVANCE=0.05; Override pressure advance value\n");
        }
    }
    GIVEN("a RepRapFirmware-flavor writer") {
        GCodeWriter writer;
        configure_writer(writer, gcfRepRapFirmware);
        THEN("it emits M572") {
            REQUIRE(writer.set_pressure_advance(0.05) == "M572 D0 S0.05; Override pressure advance value\n");
        }
    }
    GIVEN("a Marlin-flavor writer") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        THEN("it emits M900") {
            REQUIRE(writer.set_pressure_advance(0.05) == "M900 K0.05; Override pressure advance value\n");
        }
    }
    GIVEN("a BBL machine writer") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.set_is_bbl_machine(true);
        THEN("it emits the BBL linear-model override") {
            REQUIRE(writer.set_pressure_advance(0.05) == "M900 K0.05 L1000 M10 ; Override pressure advance value\n");
        }
    }
    GIVEN("a negative pressure advance value") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        THEN("no instruction is emitted") {
            REQUIRE(writer.set_pressure_advance(-0.1).empty());
        }
    }
}

SCENARIO("set_fan emits flavor-specific instruction", "[GCodeWriter]") {
    GIVEN("the full GCodeFlavor matrix") {
        struct FanFlavor { GCodeFlavor flavor; const char *off; const char *on; };
        const FanFlavor flavors[] = {
            { gcfMarlinLegacy,   "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfKlipper,        "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfRepRapFirmware, "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfMarlinFirmware, "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfRepRapSprinter, "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfRepetier,       "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfTeacup,         "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfMakerWare,      "M127 ; disable fan\n",    "M126 ; enable fan\n" },
            { gcfSailfish,       "M127 ; disable fan\n",    "M126 ; enable fan\n" },
            { gcfMach3,          "M106 S0 ; disable fan\n", "M106 P255 ; enable fan\n" },
            { gcfMachinekit,     "M106 S0 ; disable fan\n", "M106 P255 ; enable fan\n" },
            { gcfSmoothie,       "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
            { gcfNoExtrusion,    "M106 S0 ; disable fan\n", "M106 S255 ; enable fan\n" },
        };
        for (const auto &f : flavors) {
            DYNAMIC_SECTION("flavor " << static_cast<int>(f.flavor)) {
                THEN("turning the fan off and on emits the matching instruction") {
                    REQUIRE(GCodeWriter::set_fan(f.flavor, 0) == f.off);
                    REQUIRE(GCodeWriter::set_fan(f.flavor, 100) == f.on);
                }
            }
        }
    }
}

SCENARIO("set_temperature emits M104/M109 depending on wait", "[GCodeWriter]") {
    GIVEN("the temperature-command matrix") {
        struct TempCase { unsigned int temp; GCodeFlavor flavor; bool wait; int tool; const char *expected; };
        const TempCase cases[] = {
            { 200, gcfMarlinFirmware, false, -1, "M104 S200 ; set nozzle temperature\n" },
            { 200, gcfMarlinFirmware, true,  -1, "M109 S200 ; set nozzle temperature and wait for it to be reached\n" },
            { 200, gcfRepRapFirmware, false, -1, "G10 S200 ; set nozzle temperature\n" },
            { 200, gcfRepRapFirmware, true,  -1, "G10 S200 ; set nozzle temperature\nM116 ; wait for temperature to be reached\n" },
            { 200, gcfMakerWare,      true,  -1, "" },
            { 200, gcfMach3,          false, -1, "M104 P200 ; set nozzle temperature\n" },
            { 200, gcfMarlinFirmware, false,  2, "M104 S200 T2 ; set nozzle temperature\n" },
            { 200, gcfRepRapFirmware, false,  2, "G10 S200 P2 ; set nozzle temperature\n" },
            { 200, gcfTeacup,         true,  -1, "M104 S200 ; set nozzle temperature\nM116 ; wait for temperature to be reached\n" },
            { 200, gcfTeacup,         false, -1, "M104 S200 ; set nozzle temperature\n" },
            { 200, gcfMachinekit,     false, -1, "M104 P200 ; set nozzle temperature\n" },
            { 200, gcfSailfish,       true,  -1, "" },
            { 200, gcfRepetier,       false, -1, "M104 S200 ; set nozzle temperature\n" },
            { 200, gcfSmoothie,       true,  -1, "M109 S200 ; set nozzle temperature and wait for it to be reached\n" },
        };
        for (const auto &c : cases) {
            DYNAMIC_SECTION("flavor " << static_cast<int>(c.flavor) << " wait " << c.wait << " tool " << c.tool) {
                THEN("the matching temperature command is emitted") {
                    REQUIRE(GCodeWriter::set_temperature(c.temp, c.flavor, c.wait, c.tool) == c.expected);
                }
            }
        }
    }
}

SCENARIO("toolchange emits T command only for multiple extruders", "[GCodeWriter]") {
    GIVEN("a single-extruder writer") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.set_extruders({ 0 });
        writer.set_extruder(0);
        THEN("toolchange emits nothing") {
            REQUIRE(writer.toolchange(0).empty());
        }
    }
    GIVEN("a multiple-extruder writer with absolute E") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.set_extruders({ 0, 1 });
        writer.set_extruder(0);
        THEN("toolchange emits T1 and an E reset") {
            REQUIRE(writer.toolchange(1) == "T1 ; change extruder\nG92 E0 ; reset extrusion distance\n");
        }
    }
    GIVEN("a multiple-extruder writer with relative E") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware, /*relative_e=*/true);
        writer.set_extruders({ 0, 1 });
        writer.set_extruder(0);
        THEN("toolchange emits T1 without an E reset") {
            REQUIRE(writer.toolchange(1) == "T1 ; change extruder\n");
        }
    }
}

SCENARIO("retract emits scaled length for wipe and toolchange", "[GCodeWriter]") {
    GIVEN("a 50% wipe retraction") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.config.retract_before_wipe.values = { 50 }; // 50%
        configure_retraction(writer);
        THEN("the retraction length is scaled by 0.5") {
            REQUIRE(writer.retract(true) == "G1 E-.75 F1800 ; retract\n"); // 0.5 * 1.5
        }
    }
    GIVEN("a toolchange retraction") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.config.retract_length_toolchange.values = { 2.0 };
        writer.config.retract_restart_extra_toolchange.values = { 0.3 };
        configure_retraction(writer);
        THEN("the toolchange-specific length is used") {
            REQUIRE(writer.retract_for_toolchange() == "G1 E-2 F1800 ; retract for toolchange\n");
        }
    }
}

SCENARIO("retract/unretract emits relative E", "[GCodeWriter]") {
    GIVEN("a writer with relative E distances") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware, /*relative_e=*/true);
        configure_retraction(writer);
        WHEN("the extruder retracts then unretracts") {
            THEN("relative E amounts are emitted") {
                REQUIRE(writer.retract() == "G1 E-1.5 F1800 ; retract\n");
                REQUIRE(writer.unretract() == "G1 E1.7 F1500 ;  ; unretract\n");
            }
        }
    }
}

SCENARIO("retract/unretract emits absolute E", "[GCodeWriter]") {
    GIVEN("a writer with absolute E distances") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware, /*relative_e=*/false);
        configure_retraction(writer);
        WHEN("the extruder retracts then unretracts") {
            THEN("the absolute E position is emitted") {
                REQUIRE(writer.retract() == "G1 E-1.5 F1800 ; retract\n");
                REQUIRE(writer.unretract() == "G1 E.2 F1500 ;  ; unretract\n"); // sub-1 E emits ".2"
            }
        }
    }
}

SCENARIO("retract/unretract with firmware retraction", "[GCodeWriter]") {
    GIVEN("a writer with firmware retraction enabled") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.config.use_firmware_retraction.value = true;
        configure_retraction(writer);
        WHEN("the extruder retracts then unretracts") {
            THEN("G10/G11 firmware commands are emitted") {
                REQUIRE(writer.retract() == "G10 ; retract\n");
                REQUIRE(writer.unretract() == "G11 ; unretract\nG92 E0 ; reset extrusion distance\n");
            }
        }
    }
}

SCENARIO("lift schedules Z-hop for the three lift types", "[GCodeWriter]") {
    GIVEN("a writer with z-hop configured") {
        GCodeWriter writer;
        configure_writer(writer, gcfMarlinFirmware);
        writer.config.z_hop.values = { 1.5 };
        writer.config.retract_lift_above.values = { 0.0 };
        writer.config.retract_lift_below.values = { 0.0 };
        writer.set_extruders({ 0 });
        writer.set_extruder(0);

        // All three lift types only schedule the lift (no immediate G-code);
        // the type is consumed later by travel_to_xyz (spiral -> arc, etc.).
        THEN("each of the three lift types schedules without emitting G-code") {
            REQUIRE(writer.lift(LiftType::NormalLift).empty());
            REQUIRE(writer.lift(LiftType::SpiralLift).empty());
            REQUIRE(writer.lift(LiftType::LazyLift).empty());
            REQUIRE(writer.unlift().empty());
        }
    }
}
