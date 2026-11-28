#include "catch_main.hpp"
#include "libslic3r/GCodeWriter.hpp"

#include <array>

using namespace Slic3r;

namespace {
struct SpiralLiftFixture
{
    GCodeWriter writer;
    Vec2d       nozzle_offset;
    bool        previous_comments = GCodeWriter::full_gcode_comment;

    SpiralLiftFixture(
        const Pointfs& bed, unsigned int tool = 0, Vec2d offset = Vec2d::Zero(), double height = 0.4, Vec2d tool_offset = Vec2d::Zero())
        : nozzle_offset(tool_offset)
    {
        PrintConfig config;
        config.printable_area.values = bed;
        config.extruder_offset.values.assign(4, Vec2d::Zero());
        config.extruder_offset.values[tool] = tool_offset;
        writer.apply_print_config(config);
        writer.config.gcode_flavor.value = gcfKlipper;
        writer.config.z_hop.values.assign(4, height);
        writer.config.travel_slope.values.assign(4, 3.0);
        writer.config.retract_lift_above.values.assign(4, 0.0);
        writer.config.retract_lift_below.values.assign(4, 0.0);
        writer.set_extruders({0, 1, 2, 3});
        writer.set_extruder(tool);
        writer.set_xy_offset(offset.x(), offset.y());
        GCodeWriter::full_gcode_comment = true;
    }

    ~SpiralLiftFixture() { GCodeWriter::full_gcode_comment = previous_comments; }

    std::string travel(const Vec2d& from, const Vec2d& to)
    {
        const Vec2f offset = writer.get_xy_offset();
        writer.set_position(Vec3d(from.x() + offset.x() - nozzle_offset.x(), from.y() + offset.y() - nozzle_offset.y(), 0.2));
        writer.set_current_position_clear(true);
        writer.lift(LiftType::SpiralLift);
        return writer.travel_to_xyz(Vec3d(to.x() + offset.x() - nozzle_offset.x(), to.y() + offset.y() - nozzle_offset.y(), 0.2));
    }
};

const Pointfs u1_bed{{0.5, 1.0}, {270.5, 1.0}, {270.5, 271.0}, {0.5, 271.0}};
} // namespace

TEST_CASE("Spiral lifts at every bed edge fall back to a full vertical lift", "[GCodeWriter][SpiralLift]")
{
    const std::array<Vec2d, 4> starts{{{0.6, 135}, {270.4, 135}, {135, 1.1}, {135, 270.9}}};
    const std::array<Vec2d, 4> targets{{{0.6, 160}, {270.4, 110}, {110, 1.1}, {160, 270.9}}};
    const std::array<Vec2d, 2> offsets{{{0, 0}, {1000, 2000}}};
    for (unsigned int tool = 0; tool < 4; ++tool)
        for (size_t edge = 0; edge < starts.size(); ++edge)
            for (size_t plate = 0; plate < offsets.size(); ++plate) {
                DYNAMIC_SECTION("tool " << tool << ", edge " << edge << ", plate " << plate)
                {
                    SpiralLiftFixture fixture(u1_bed, tool, offsets[plate]);
                    const std::string gcode = fixture.travel(starts[edge], targets[edge]);
                    REQUIRE(gcode.find("G2 ") == std::string::npos);
                    REQUIRE(gcode.find("G3 ") == std::string::npos);
                    REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("G1 Z.6"));
                    REQUIRE_THAT(fixture.writer.get_zhop(), Catch::Matchers::WithinAbs(0.4, 1e-9));
                    REQUIRE_THAT(fixture.writer.get_position().z(), Catch::Matchers::WithinAbs(0.6, 1e-9));
                    REQUIRE_THAT(fixture.writer.unlift(), Catch::Matchers::StartsWith("G1 Z.2"));
                }
            }
}

TEST_CASE("Interior travel retains spiral lifting", "[GCodeWriter][SpiralLift]")
{
    SpiralLiftFixture fixture(u1_bed);
    const std::string gcode = fixture.travel({135, 135}, {135, 160});
    REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("G3 Z.6"));
    REQUIRE(gcode.find("normal lift Z") == std::string::npos);
}

TEST_CASE("Spiral containment uses the bed polygon rather than its bounding box", "[GCodeWriter][SpiralLift]")
{
    const Pointfs     bed{{0, 0}, {50, 0}, {50, 50}, {30, 50}, {30, 20}, {0, 20}};
    SpiralLiftFixture fixture(bed);
    const std::string gcode = fixture.travel({20, 19.9}, {45, 19.9});
    REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("G1 Z.6"));
    REQUIRE(gcode.find("G3 ") == std::string::npos);
}

TEST_CASE("Large spiral lifts are checked beyond the old fixed 3.5 mm margin", "[GCodeWriter][SpiralLift]")
{
    SpiralLiftFixture fixture(u1_bed, 0, Vec2d::Zero(), 4.0);
    const std::string gcode = fixture.travel({10, 135}, {10, 160});
    REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("G1 Z4.2"));
    REQUIRE(gcode.find("G3 ") == std::string::npos);
}

TEST_CASE("Invalid or extremely shallow spiral angles use a vertical lift", "[GCodeWriter][SpiralLift]")
{
    for (const double angle : {0.0, 1e-20}) {
        DYNAMIC_SECTION("angle " << angle)
        {
            SpiralLiftFixture fixture(u1_bed);
            fixture.writer.config.travel_slope.values.assign(4, angle);
            const std::string gcode = fixture.travel({135, 135}, {135, 160});
            REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("G1 Z.6"));
            REQUIRE(gcode.find("G3 ") == std::string::npos);
        }
    }
}

TEST_CASE("Spiral containment accounts for each active nozzle offset", "[GCodeWriter][SpiralLift]")
{
    const std::array<Vec2d, 4> starts{{{0.6, 135}, {270.4, 135}, {135, 1.1}, {135, 270.9}}};
    const std::array<Vec2d, 4> targets{{{0.6, 160}, {270.4, 110}, {110, 1.1}, {160, 270.9}}};
    const std::array<Vec2d, 4> nozzle_offsets{{{-10, 0}, {10, 0}, {0, -10}, {0, 10}}};
    for (unsigned int tool = 0; tool < 4; ++tool) {
        DYNAMIC_SECTION("tool " << tool)
        {
            SpiralLiftFixture fixture(u1_bed, tool, {1000, 2000}, 0.4, nozzle_offsets[tool]);
            const std::string gcode = fixture.travel(starts[tool], targets[tool]);
            REQUIRE_THAT(gcode, Catch::Matchers::StartsWith("G1 Z.6"));
            REQUIRE(gcode.find("G3 ") == std::string::npos);
        }
    }
}

TEST_CASE("Missing nozzle offsets use the same first-entry fallback as point_to_gcode", "[GCodeWriter][SpiralLift]")
{
    SpiralLiftFixture fixture(u1_bed, 2);
    PrintConfig config;
    config.printable_area.values = u1_bed;
    config.extruder_offset.values = {{0, 0}, {10, 0}};
    fixture.writer.apply_print_config(config);
    REQUIRE_THAT(fixture.travel({0.6, 135}, {0.6, 160}), Catch::Matchers::StartsWith("G1 Z.6"));
}

TEST_CASE("A near-edge spiral directed inward is retained", "[GCodeWriter][SpiralLift]")
{
    for (const double nozzle_x : {0.0, 10.0}) {
        DYNAMIC_SECTION("nozzle offset X " << nozzle_x)
        {
            SpiralLiftFixture fixture(u1_bed, 1, {1000, 2000}, 0.4, {nozzle_x, 0});
            const std::string gcode = fixture.travel({0.6, 135}, {0.6, 110});
            REQUIRE_THAT(gcode, Catch::Matchers::ContainsSubstring("G3 Z.6"));
        }
    }
}

TEST_CASE("Spiral containment reserves export rounding clearance", "[GCodeWriter][SpiralLift]")
{
    SECTION("A circle within three micrometers of the edge uses a vertical lift")
    {
        SpiralLiftFixture fixture(u1_bed);
        REQUIRE_THAT(fixture.travel({2.936, 135}, {2.936, 160}), Catch::Matchers::StartsWith("G1 Z.6"));
    }
    SECTION("A circle with sufficient clearance remains a spiral")
    {
        SpiralLiftFixture fixture(u1_bed);
        REQUIRE_THAT(fixture.travel({2.938, 135}, {2.938, 160}), Catch::Matchers::ContainsSubstring("G3 Z.6"));
    }
}

TEST_CASE("Spiral containment handles missing and clockwise bed definitions", "[GCodeWriter][SpiralLift]")
{
    SECTION("An unknown boundary preserves previous lifting behavior")
    {
        SpiralLiftFixture fixture(Pointfs{});
        REQUIRE_THAT(fixture.travel({135, 135}, {135, 160}), Catch::Matchers::ContainsSubstring("G3 Z.6"));
    }
    SECTION("Clockwise corners still enforce the physical boundary")
    {
        Pointfs           bed(u1_bed.rbegin(), u1_bed.rend());
        SpiralLiftFixture fixture(bed);
        REQUIRE_THAT(fixture.travel({0.6, 135}, {0.6, 160}), Catch::Matchers::StartsWith("G1 Z.6"));
    }
}
