#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/FanMover.hpp"
#include "libslic3r/GCodeWriter.hpp"

using namespace Slic3r;

// FanMover is a stateful stream filter: lines are accumulated in a time buffer of
// nb_seconds_delay seconds before being emitted, so the mover must be flushed at the end.
static std::string run(FanMover &mover, const std::string &gcode)
{
    std::string out = mover.process_gcode(gcode, false);
    out += mover.process_gcode("", true);
    return out;
}

// All scenarios below use Marlin flavor + relative E distances, and G1 lines at F600
// (=10 mm/s), so a 10mm move takes exactly 1 second of buffer time.

SCENARIO("M106 is moved back by the delay and the target G1 is split at the time offset", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // 20mm at 10mm/s -> 2s buffered; delay=1s -> M106 lands at 50% of the G1.
    // X is interpolated (3 decimal digits), relative E is split proportionally (5 decimal digits).
    const std::string gcode =
        "G1 X20 Y0 E0.8 F600\n"
        "M106 S255\n";
    const std::string expected =
        "G1 X10.000 Y0 E0.40000 F600\n"
        "M106 S255\n"
        "G1 X20 Y0 E0.40000 F600\n";

    WHEN("a speed-up fan command follows a buffered move") {
        std::string out = run(mover, gcode);
        THEN("the G1 is split in two halves around the moved M106") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("zero delay passes gcode through unchanged", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/0.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // With delay==0 the move-back path is disabled and every buffered line is flushed immediately.
    const std::string gcode =
        "G1 X10 Y0 E0.4 F600\n"
        "M106 S255\n"
        "G1 X20 Y0 E0.4 F600\n"
        "M107\n";

    WHEN("the delay is zero") {
        std::string out = run(mover, gcode);
        THEN("the output is byte-identical to the input") {
            REQUIRE(out == gcode);
        }
    }
}

SCENARIO("fan speed-up with a buffer shorter than the delay is emitted immediately", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/5.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // Only 1s of motion is buffered, less than the 5s delay: the M106 cannot be moved back,
    // so it is printed right away, ahead of the still-buffered G1 (existing behavior).
    // FIXME: existing behavior — when buffered time < delay, M106 is emitted immediately
    // (ahead of the still-buffered G1). Pinned as characterization; revisit if the
    // move-back ordering is ever changed.
    const std::string gcode =
        "G1 X10 Y0 E0.4 F600\n"
        "M106 S255\n";
    const std::string expected =
        "M106 S255\n"
        "G1 X10 Y0 E0.4 F600\n";

    WHEN("the buffered time is shorter than the delay") {
        std::string out = run(mover, gcode);
        THEN("the M106 is printed immediately, before the buffered G1") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("only_overhangs mode restricts the move-back to overhang perimeters", "[FanMover]") {
    // The ;TYPE: comment sets the role for the following extrusions.
    // G1a (1s) flushes the ;TYPE: line out of the buffer, G1b (2s) remains buffered.
    const std::string body =
        "G1 X10 Y0 E0.4 F600\n"
        "G1 X30 Y0 E0.8 F600\n"
        "M106 S255\n";

    WHEN("the current role is an overhang perimeter") {
        GCodeWriter writer;
        writer.config.gcode_flavor.value             = gcfMarlinFirmware;
        writer.config.use_relative_e_distances.value = true;
        FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                       /*only_overhangs=*/true, /*kickstart=*/0.f);

        std::string out = run(mover, ";TYPE:Overhang wall\n" + body);
        THEN("the M106 is moved back 1s into the middle of the 2s G1") {
            const std::string expected =
                ";TYPE:Overhang wall\n"
                "G1 X10 Y0 E0.4 F600\n"
                "G1 X20.000 Y0 E0.40000 F600\n"
                "M106 S255\n"
                "G1 X30 Y0 E0.40000 F600\n";
            REQUIRE(out == expected);
        }
    }
    WHEN("the current role is not an overhang perimeter") {
        GCodeWriter writer;
        writer.config.gcode_flavor.value             = gcfMarlinFirmware;
        writer.config.use_relative_e_distances.value = true;
        FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                       /*only_overhangs=*/true, /*kickstart=*/0.f);

        std::string gcode = ";TYPE:Inner wall\n" + body;
        std::string out   = run(mover, gcode);
        THEN("the M106 stays in place (byte-identical output)") {
            REQUIRE(out == gcode);
        }
    }
}

SCENARIO("fan commands at the head and tail of the stream are flushed correctly", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // The first M106 meets an empty buffer and is printed directly.
    // The trailing G1 (1s) and M107 stay in the buffer until the final flush.
    const std::string gcode =
        "M106 S255\n"
        "G1 X10 Y0 E0.4 F600\n"
        "M107\n";

    WHEN("the stream ends with unflushed lines") {
        std::string out = run(mover, gcode);
        THEN("the tail is emitted in order, without loss or duplication") {
            REQUIRE(out == gcode);
        }
    }
}

SCENARIO("Y-axis G1 is split at the M106 time offset", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // 20mm of Y at 10mm/s -> 2s buffered; delay=1s -> M106 lands at 50%, so Y is interpolated.
    const std::string gcode =
        "G1 Y20 E0.8 F600\n"
        "M106 S255\n";
    const std::string expected =
        "G1 Y10.000 E0.40000 F600\n"
        "M106 S255\n"
        "G1 Y20 E0.40000 F600\n";

    WHEN("a Y-only move is followed by a speed-up fan command") {
        std::string out = run(mover, gcode);
        THEN("Y is split proportionally and relative E is split too") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("absolute-E G1 is split at the M106 time offset", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = false;   // absolute E
    FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/false,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // 20mm X at 10mm/s -> 2s; E0.8 is absolute. delay=1s -> 50% split.
    const std::string gcode =
        "G1 X20 E0.8 F600\n"
        "M106 S255\n";
    const std::string expected =
        "G1 X10.000 E0.40000 F600\n"
        "M106 S255\n"
        "G1 X20 E0.8 F600\n";

    WHEN("an absolute-E move is followed by a speed-up fan command") {
        std::string out = run(mover, gcode);
        THEN("X and absolute E are split at the midpoint") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("a fan command landing past 90% of a G1 is printed before it without splitting", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/0.05f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    // 10mm at 10mm/s -> 1s buffered; delay=0.05s -> offset 0.95s (past 90%), so the
    // M106 is not worth splitting and is emitted before the G1.
    const std::string gcode =
        "G1 X10 Y0 E0.4 F600\n"
        "M106 S255\n";
    const std::string expected =
        "M106 S255\n"
        "G1 X10 Y0 E0.4 F600\n";

    WHEN("the offset lands past 90% of the buffered G1") {
        std::string out = run(mover, gcode);
        THEN("the M106 is emitted before the G1, without splitting") {
            REQUIRE(out == expected);
        }
    }
}

SCENARIO("unflushed tail stays in the buffer until an explicit flush", "[FanMover]") {
    GCodeWriter writer;
    writer.config.gcode_flavor.value             = gcfMarlinFirmware;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, /*nb_seconds_delay=*/1.f, /*with_D_option=*/false, /*relative_e=*/true,
                   /*only_overhangs=*/false, /*kickstart=*/0.f);

    const std::string gcode =
        "G1 X10 Y0 E0.4 F600\n"
        "M107\n";

    // Process without flush: the lines stay buffered (nothing emitted).
    std::string out = mover.process_gcode(gcode, /*flush=*/false);
    REQUIRE(out.empty());

    // Explicit flush: the buffered tail is emitted in order.
    std::string flushed = mover.process_gcode("", /*flush=*/true);
    REQUIRE(flushed == gcode);
}
