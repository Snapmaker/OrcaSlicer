#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <fstream>
#include <string>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/svg.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;

static BoundingBoxf3 load_svg_bbox(const std::string &svg_xml)
{
    const boost::filesystem::path tmp =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_test_svg_%%%%-%%%%.svg");
    {
        std::ofstream ofs(tmp.string(), std::ios::binary);
        ofs << svg_xml;
    }
    Model       model;
    std::string msg;
    const bool  ok = load_svg(tmp.string().c_str(), &model, msg);
    boost::filesystem::remove(tmp);
    REQUIRE(ok);
    REQUIRE(! model.objects.empty());
    BoundingBoxf3 bb;
    for (const ModelObject *obj : model.objects)
        bb.merge(obj->raw_mesh_bounding_box());
    return bb;
}

TEST_CASE("SVG short strokes preserve orientation without shortest-edge decimation", "[Format][SVG]") {
    SECTION("Short open diagonal stroke") {
        const BoundingBoxf3 bb = load_svg_bbox(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" height=\"100mm\" viewBox=\"0 0 100 100\">"
            "<path d=\"M20 20 L20.015 20.015\" fill=\"none\" stroke=\"black\" stroke-width=\"10\"/>"
            "</svg>");
        const Vec3d sz = bb.size();
        // A 45-degree 10mm-wide open square stroke has rotated bounding box ~14.157 x 14.157 mm (not 10 x 10 mm).
        REQUIRE(sz.x() > 14.0);
        REQUIRE(sz.y() > 14.0);
        REQUIRE(sz.x() == Catch::Approx(14.157).margin(0.05));
        REQUIRE(sz.y() == Catch::Approx(14.157).margin(0.05));
    }

    SECTION("Short closed diagonal stroke") {
        const BoundingBoxf3 bb = load_svg_bbox(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" height=\"100mm\" viewBox=\"0 0 100 100\">"
            "<path d=\"M20 20 L20.015 20.015 Z\" fill=\"none\" stroke=\"black\" stroke-width=\"10\"/>"
            "</svg>");
        const Vec3d sz = bb.size();
        // Closed short diagonal stroke must also preserve its 45-degree orientation (~14.157 x 14.157 mm).
        REQUIRE(sz.x() > 14.0);
        REQUIRE(sz.y() > 14.0);
        REQUIRE(sz.x() == Catch::Approx(14.157).margin(0.05));
        REQUIRE(sz.y() == Catch::Approx(14.157).margin(0.05));
    }
}
