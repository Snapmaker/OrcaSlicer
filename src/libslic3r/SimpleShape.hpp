#ifndef slic3r_SimpleShape_hpp_
#define slic3r_SimpleShape_hpp_

// Simple shapes (circle, square, star, ...) embossed as SVG volumes,
// so user does not need to draw SVG file for a basic shape.
// Parameters are stored inside of the generated SVG data (<metadata>),
// so the shape stays editable after the project is saved into .3mf.

#include <optional>
#include <string>

namespace Slic3r {

class ModelVolume;

enum class SimpleShapeType : int { Circle = 0, Square, Triangle, Pentagon, Hexagon, Octagon, Star, Ring };

struct SimpleShapeParams
{
    SimpleShapeType type = SimpleShapeType::Circle;
    // Width of the shape [in mm], height follows the shape proportions
    double size = 10.;
    // Emboss depth [in mm]
    double depth = 1.;
    // Only star: count of points
    int star_points = 5;
    // Star: inner radius, ring: hole diameter, ratio to the outer one
    double inner_ratio = 0.5;
    // Square and polygons: radius of rounded corners [in mm]
    double corner_radius = 0.;

    bool operator==(const SimpleShapeParams &o) const;
    bool operator!=(const SimpleShapeParams &o) const { return !(*this == o); }
};

// Content of SVG file with the shape (include metadata)
std::string create_simple_shape_svg(const SimpleShapeParams &params);

std::optional<SimpleShapeParams> read_simple_shape_meta(const std::string &svg_data);
// nullopt when volume is not a simple shape
std::optional<SimpleShapeParams> read_simple_shape_meta(const ModelVolume &volume);

// English name of the shape, used as volume name
const char *simple_shape_name(SimpleShapeType type);

// Path of SVG in .3mf archive
std::string simple_shape_path_in_3mf(const SimpleShapeParams &params);

} // namespace Slic3r

#endif // slic3r_SimpleShape_hpp_
