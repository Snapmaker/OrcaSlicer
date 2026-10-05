#ifndef slic3r_NSVGUtils_hpp_
#define slic3r_NSVGUtils_hpp_

#include <memory>
#include <string>
#include <sstream>
#include "Polygon.hpp"
#include "ExPolygon.hpp"
#include "EmbossShape.hpp" // ExPolygonsWithIds
#include "nanosvg/nanosvg.h"    // load SVG file

// Helper function to work with nano svg
namespace Slic3r {

/// <summary>
/// Paramreters for conversion curve from SVG to lines in Polygon
/// </summary>
struct NSVGLineParams
{
    // Smaller will divide curve to more lines
    // NOTE: Value is in image scale
    double tesselation_tolerance = 10.f;

    // Maximal depth of recursion for conversion curve to lines
    int max_level = 10;

    // Multiplicator of point coors
    // NOTE: Every point coor from image(float) is multiplied by scale and rounded to integer --> Slic3r::Point
    double scale = 1. / SCALING_FACTOR;

    // Flag wether y is negative, when true than y coor is multiplied by -1
    bool is_y_negative = true;

    // Is used only with rounded Stroke
    double arc_tolerance = 1.;

    // Maximal count of heal iteration
    unsigned max_heal_iteration = 10;

    // Budget of polygon points (all shapes together, after curves are flattened) for input that
    // is not trusted; 0 = unlimited. When it is exceeded create_shape_with_ids() stops flattening
    // and reports it, see its `too_complex` argument.
    size_t max_flat_points = 0;

    explicit NSVGLineParams(double tesselation_tolerance): 
        tesselation_tolerance(tesselation_tolerance), 
        arc_tolerance(std::pow(tesselation_tolerance, 1/3.))
    {}
};

/// <summary>
/// Convert .svg opened by nanoSvg to shapes stored in expolygons with ids
/// </summary>
/// <param name="image">Parsed svg file by NanoSvg</param>
/// <param name="tesselation_tolerance">Smaller will divide curve to more lines
/// NOTE: Value is in image scale</param>
/// <param name="max_level">Maximal depth for conversion curve to lines</param>
/// <param name="scale">Multiplicator of point coors
/// NOTE: Every point coor from image(float) is multiplied by scale and rounded to integer</param>
/// <returns>Shapes from svg image - fill + stroke</returns>
/// <param name="too_complex">When not null it is set to true if param.max_flat_points was exceeded;
/// the result is then empty</param>
ExPolygonsWithIds create_shape_with_ids(const NSVGimage &image, const NSVGLineParams &param, bool *too_complex = nullptr);

// help functions - prepare to be tested
/// <param name="is_y_negative">Flag is y negative, when true than y coor is multiplied by -1</param>
Polygons to_polygons(const NSVGimage &image, const NSVGLineParams &param);

void bounds(const NSVGimage &image, Vec2f &min, Vec2f &max);

// read text data from file; nullptr when it cannot be opened or is larger than max_size
// (default: untrusted::SVG_SIZE_LIMIT), the file is never read past that size
std::unique_ptr<std::string> read_from_disk(const std::string &path);
std::unique_ptr<std::string> read_from_disk(const std::string &path, std::uint64_t max_size, bool *too_large);

using NSVGimage_ptr = std::unique_ptr<NSVGimage, void (*)(NSVGimage*)>;
NSVGimage_ptr nsvgParseFromFile(const std::string &svg_file_path, const char *units = "mm", float dpi = 96.0f);
NSVGimage_ptr nsvgParse(const std::string& file_data, const char *units = "mm", float dpi = 96.0f);

/// Why an SVG was not accepted (init_image, svg_within_limits)
enum class SvgRefusal { None, Unreadable, TooLarge, TooComplex };

/// <summary>
/// Is the parsed SVG small enough to be turned into polygons? Counts shapes, sub-paths and Bezier
/// control points against untrusted::SVG_MAX_SHAPES / SVG_MAX_PATHS / SVG_MAX_POINTS.
/// </summary>
/// <param name="why">When not null, receives which limit was hit (empty when within limits)</param>
bool svg_within_limits(const NSVGimage &image, std::string *why = nullptr);

/// Parse SVG text taken from outside: refused above untrusted::SVG_SIZE_LIMIT (before the parser
/// copies it) and when the result is over the shape/path/point limits. nullptr on refusal.
NSVGimage_ptr nsvgParse_checked(const std::string &file_data, SvgRefusal &refusal, std::string *why = nullptr);

/// Loads svg_file.file_data (from svg_file.path when empty) and parses it with the same size and
/// complexity limits. Returns nullptr when refused; `refusal` says why.
NSVGimage *init_image(EmbossShape::SvgFile &svg_file, SvgRefusal *refusal = nullptr, std::string *why = nullptr);

/// <summary>
/// Iterate over shapes and calculate count
/// </summary>
/// <param name="image">Contain pointer to first shape</param>
/// <returns>Count of shapes</returns>
size_t get_shapes_count(const NSVGimage &image);

//void save(const NSVGimage &image, std::ostream &data);
//bool save(const NSVGimage &image, const std::string &svg_file_path);
} // namespace Slic3r
#endif // slic3r_NSVGUtils_hpp_
