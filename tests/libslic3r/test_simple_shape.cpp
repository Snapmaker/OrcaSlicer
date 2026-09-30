#include <catch2/catch.hpp>

#include <libslic3r/SimpleShape.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/NSVGUtils.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
// shape loaded the same way as the SVG gizmo does
ExPolygons load(const std::string &svg)
{
    NSVGimage_ptr image = nsvgParse(svg);
    REQUIRE(image != nullptr);
    ExPolygons result;
    for (ExPolygonsWithId &s : create_shape_with_ids(*image, NSVGLineParams{1e6}))
        expolygons_append(result, std::move(s.expoly));
    return result;
}

double area_mm2(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}
} // namespace

TEST_CASE("Simple shapes have the wanted width and survive SVG round trip", "[SimpleShape]")
{
    for (int t = int(SimpleShapeType::Circle); t <= int(SimpleShapeType::Ring); ++t) {
        for (double corner : {0., 2.}) {
            DYNAMIC_SECTION("type " << t << " corner " << corner)
            {
                SimpleShapeParams params;
                params.type          = SimpleShapeType(t);
                params.size          = 20.;
                params.corner_radius = corner;
                std::string svg      = create_simple_shape_svg(params);

                ExPolygons shape = load(svg);
                REQUIRE(shape.size() == 1);
                BoundingBox bb = get_extents(shape);
                CHECK_THAT(unscale<double>(bb.size().x()), WithinAbs(20., 0.01));
                CHECK(unscale<double>(bb.size().y()) <= 20.01);
                CHECK(shape.front().holes.size() == (params.type == SimpleShapeType::Ring ? 1u : 0u));

                std::optional<SimpleShapeParams> meta = read_simple_shape_meta(svg);
                REQUIRE(meta.has_value());
                CHECK(*meta == params);
            }
        }
    }
}

TEST_CASE("Simple shape areas", "[SimpleShape]")
{
    SimpleShapeParams params;
    params.size = 20.;

    params.type = SimpleShapeType::Square;
    CHECK_THAT(area_mm2(load(create_simple_shape_svg(params))), WithinRel(400., 1e-4));

    params.type = SimpleShapeType::Circle;
    CHECK_THAT(area_mm2(load(create_simple_shape_svg(params))), WithinRel(PI * 100., 1e-3));

    params.type        = SimpleShapeType::Ring;
    params.inner_ratio = 0.5;
    CHECK_THAT(area_mm2(load(create_simple_shape_svg(params))), WithinRel(PI * (100. - 25.), 1e-3));

    SECTION("rounded corners remove area but keep the width")
    {
        params.type          = SimpleShapeType::Square;
        params.corner_radius = 2.;
        double rounded       = area_mm2(load(create_simple_shape_svg(params)));
        double expected      = 400. - (4. - PI) * 4.; // four corners of 2 mm radius
        CHECK_THAT(rounded, WithinRel(expected, 1e-3));
    }
}

TEST_CASE("Simple shape paths in 3mf are unique", "[SimpleShape]")
{
    SimpleShapeParams params;
    CHECK(simple_shape_path_in_3mf(params) != simple_shape_path_in_3mf(params));
    CHECK(!read_simple_shape_meta("<svg/>").has_value());
}
