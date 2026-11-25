#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <boost/filesystem.hpp>
#include <cmath>
#include <fstream>
#include <string>

#include "libslic3r/Format/svg.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;

// The shape of an SVG with one open path, stroked 10 mm wide with square caps: its size in X and Y.
static Vec2d size_of_stroke(const std::string &path_d)
{
    const boost::filesystem::path file = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_svg_stroke_%%%%.svg");
    {
        std::ofstream out(file.string(), std::ios::binary);
        out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40mm\" height=\"40mm\" viewBox=\"0 0 40 40\">\n"
               "  <path d=\"" << path_d << "\" fill=\"none\" stroke=\"black\" stroke-width=\"10\" stroke-linecap=\"square\"/>\n</svg>\n";
    }
    Model        model;
    std::string  message;
    const bool   loaded = load_svg(file.string().c_str(), &model, message);
    boost::filesystem::remove(file);
    INFO("message: " << message);
    REQUIRE(loaded);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const BoundingBoxf3 box = model.objects.front()->volumes.front()->mesh().bounding_box();
    return {box.size().x(), box.size().y()};
}

TEST_CASE("A short stroke of an SVG keeps the direction of its square caps", "[Format]") {
    // A stroke 0.015 mm in each direction, 0.0212 mm long. Before its offset by 5 mm, a vertex had to be 0.025 mm
    // from the previous one to stay, so the end of the stroke was dropped and the caps went around its start alone: a
    // square of 10 mm. With both ends, the square caps are turned 45 degrees: 10 mm x sqrt(2) across, and the length
    // of the stroke with it.
    const double expected = 0.015 + 10. * std::sqrt(2.);

    SECTION("The stroke from its first point to its last one")
    {
        const Vec2d size = size_of_stroke("M20 20 L20.015 20.015");
        INFO("size " << size.x() << " x " << size.y() << " mm, expected " << expected);
        CHECK(size.x() == Catch::Approx(expected).epsilon(1e-4));
        CHECK(size.y() == Catch::Approx(expected).epsilon(1e-4));
    }
    SECTION("The stroke from its last point to its first one")
    {
        const Vec2d size = size_of_stroke("M20.015 20.015 L20 20");
        INFO("size " << size.x() << " x " << size.y() << " mm, expected " << expected);
        CHECK(size.x() == Catch::Approx(expected).epsilon(1e-4));
        CHECK(size.y() == Catch::Approx(expected).epsilon(1e-4));
    }
    SECTION("The stroke ending in its last point twice")
    {
        // Only the point at the same place as the previous one goes: the last segment still sets the caps.
        const Vec2d size = size_of_stroke("M20 20 L20.015 20.015 L20.015 20.015");
        INFO("size " << size.x() << " x " << size.y() << " mm, expected " << expected);
        CHECK(size.x() == Catch::Approx(expected).epsilon(1e-4));
        CHECK(size.y() == Catch::Approx(expected).epsilon(1e-4));
    }
}
