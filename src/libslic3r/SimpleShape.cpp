#include "SimpleShape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "nlohmann/json.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "CodeEmboss.hpp" // create_code_group_id
#include "EmbossShape.hpp"
#include "ExPolygon.hpp"
#include "Model.hpp"

namespace Slic3r {

namespace {

constexpr const char *SHAPE_META_BEGIN = "<metadata id=\"edgeslicer-shape\">";
constexpr const char *SHAPE_META_END   = "</metadata>";

constexpr std::array<SimpleShapeType, 8> SHAPE_TYPES = {SimpleShapeType::Circle,  SimpleShapeType::Square,  SimpleShapeType::Triangle,
                                                        SimpleShapeType::Pentagon, SimpleShapeType::Hexagon, SimpleShapeType::Octagon,
                                                        SimpleShapeType::Star,     SimpleShapeType::Ring};

const char *shape_key(SimpleShapeType type)
{
    switch (type) {
    case SimpleShapeType::Circle: return "circle";
    case SimpleShapeType::Square: return "square";
    case SimpleShapeType::Triangle: return "triangle";
    case SimpleShapeType::Pentagon: return "pentagon";
    case SimpleShapeType::Hexagon: return "hexagon";
    case SimpleShapeType::Octagon: return "octagon";
    case SimpleShapeType::Star: return "star";
    case SimpleShapeType::Ring: return "ring";
    }
    return "circle";
}

// Points on circle of radius 1 around zero, the first one is on the top (Y down),
// flat_bottom rotates polygon to lay on its edge
Pointfs regular_polygon(int count, bool flat_bottom)
{
    Pointfs result;
    double  start = -PI / 2.;
    if (flat_bottom && count % 2 == 0)
        start += PI / count;
    for (int i = 0; i < count; ++i) {
        double a = start + 2. * PI * i / count;
        result.emplace_back(std::cos(a), std::sin(a));
    }
    return result;
}

Pointfs star(int points, double inner_ratio)
{
    Pointfs result;
    for (int i = 0; i < 2 * points; ++i) {
        double a = -PI / 2. + PI * i / points;
        double r = (i % 2 == 0) ? 1. : inner_ratio;
        result.emplace_back(r * std::cos(a), r * std::sin(a));
    }
    return result;
}

// Outline in unit size (circumscribed circle radius 1), Y down, holes for ring
ExPolygon unit_shape(const SimpleShapeParams &p)
{
    auto to_polygon = [](const Pointfs &points) {
        Slic3r::Polygon polygon;
        for (const Vec2d &v : points)
            polygon.points.emplace_back(coord_t(std::llround(scale_(v.x()))), coord_t(std::llround(scale_(v.y()))));
        polygon.make_counter_clockwise();
        return polygon;
    };
    double inner = std::clamp(p.inner_ratio, 0.05, 0.95);
    switch (p.type) {
    case SimpleShapeType::Circle: return ExPolygon(to_polygon(regular_polygon(256, false)));
    case SimpleShapeType::Square: return ExPolygon(to_polygon(regular_polygon(4, true)));
    case SimpleShapeType::Triangle: return ExPolygon(to_polygon(regular_polygon(3, true)));
    case SimpleShapeType::Pentagon: return ExPolygon(to_polygon(regular_polygon(5, true)));
    case SimpleShapeType::Hexagon: return ExPolygon(to_polygon(regular_polygon(6, true)));
    case SimpleShapeType::Octagon: return ExPolygon(to_polygon(regular_polygon(8, true)));
    case SimpleShapeType::Star: return ExPolygon(to_polygon(star(std::clamp(p.star_points, 3, 24), inner)));
    case SimpleShapeType::Ring: {
        ExPolygon ring(to_polygon(regular_polygon(256, false)));
        Pointfs   hole = regular_polygon(256, false);
        for (Vec2d &v : hole)
            v *= inner;
        Slic3r::Polygon hole_polygon = to_polygon(hole);
        hole_polygon.make_clockwise();
        ring.holes.push_back(std::move(hole_polygon));
        return ring;
    }
    }
    return {};
}

long long to_um(coord_t c) { return std::llround(unscale<double>(c) * 1000.); }

std::string um_to_mm(long long um)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%lld.%03lld", um / 1000, um % 1000);
    return buffer;
}

} // namespace

bool SimpleShapeParams::operator==(const SimpleShapeParams &o) const
{
    return type == o.type && size == o.size && depth == o.depth && star_points == o.star_points && inner_ratio == o.inner_ratio &&
           corner_radius == o.corner_radius;
}

std::string create_simple_shape_svg(const SimpleShapeParams &params)
{
    ExPolygon unit = unit_shape(params);
    // scale to the wanted width
    BoundingBox unit_bb = get_extents(unit);
    double      scale   = std::max(params.size, 0.1) / unscale<double>(unit_bb.size().x());
    unit.scale(scale);

    ExPolygons shape{unit};
    // rounded corners: shrink and grow back by the radius
    bool can_round = params.type != SimpleShapeType::Circle && params.type != SimpleShapeType::Ring;
    if (can_round && params.corner_radius > 0.) {
        float      r      = float(scale_(params.corner_radius));
        ExPolygons shrunk = offset_ex(shape, -r);
        if (!shrunk.empty())
            shape = offset_ex(shrunk, r, ClipperLib::jtRound, scale_(0.01));
    }

    // rounded corners make the shape smaller, keep the wanted width
    BoundingBox bb = get_extents(shape);
    if (!bb.defined || bb.size().x() <= 0)
        return {};
    double fix = scale_(std::max(params.size, 0.1)) / double(bb.size().x());
    if (std::abs(fix - 1.) > 1e-9) {
        for (ExPolygon &e : shape)
            e.scale(fix);
        bb = get_extents(shape);
    }
    for (ExPolygon &e : shape)
        e.translate(-bb.min.x(), -bb.min.y());
    long long w = to_um(bb.size().x());
    long long h = to_um(bb.size().y());

    nlohmann::json j;
    j["format"]        = 1;
    j["type"]          = shape_key(params.type);
    j["size"]          = params.size;
    j["depth"]         = params.depth;
    j["star_points"]   = params.star_points;
    j["inner_ratio"]   = params.inner_ratio;
    j["corner_radius"] = params.corner_radius;

    std::stringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << um_to_mm(w) << "mm\" height=\"" << um_to_mm(h) << "mm\" viewBox=\"0 0 "
       << w << " " << h << "\">\n";
    // only numbers and plain names, nothing to escape for XML
    ss << SHAPE_META_BEGIN << j.dump() << SHAPE_META_END << "\n";
    ss << "<path fill=\"#808080\" fill-rule=\"evenodd\" d=\"";
    auto write_polygon = [&ss](const Slic3r::Polygon &polygon) {
        bool first = true;
        for (const Point &p : polygon.points) {
            ss << (first ? "M" : "L") << to_um(p.x()) << " " << to_um(p.y());
            first = false;
        }
        ss << "Z";
    };
    for (const ExPolygon &e : shape) {
        write_polygon(e.contour);
        for (const Slic3r::Polygon &hole : e.holes)
            write_polygon(hole);
    }
    ss << "\"/>\n</svg>\n";
    return ss.str();
}

std::optional<SimpleShapeParams> read_simple_shape_meta(const std::string &svg_data)
{
    size_t begin = svg_data.find(SHAPE_META_BEGIN);
    if (begin == std::string::npos)
        return {};
    begin += std::char_traits<char>::length(SHAPE_META_BEGIN);
    size_t end = svg_data.find(SHAPE_META_END, begin);
    if (end == std::string::npos)
        return {};
    nlohmann::json j = nlohmann::json::parse(svg_data.substr(begin, end - begin), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {};

    SimpleShapeParams p;
    try {
        std::string type = j.value("type", std::string());
        auto        it   = std::find_if(SHAPE_TYPES.begin(), SHAPE_TYPES.end(), [&type](SimpleShapeType t) { return type == shape_key(t); });
        if (it == SHAPE_TYPES.end())
            return {};
        p.type          = *it;
        p.size          = j.value("size", p.size);
        p.depth         = j.value("depth", p.depth);
        p.star_points   = j.value("star_points", p.star_points);
        p.inner_ratio   = j.value("inner_ratio", p.inner_ratio);
        p.corner_radius = j.value("corner_radius", p.corner_radius);
    } catch (const nlohmann::json::exception &) {
        return {};
    }
    return p;
}

std::optional<SimpleShapeParams> read_simple_shape_meta(const ModelVolume &volume)
{
    if (!volume.emboss_shape.has_value())
        return {};
    const std::optional<EmbossShape::SvgFile> &svg = volume.emboss_shape->svg_file;
    if (!svg.has_value() || svg->file_data == nullptr)
        return {};
    return read_simple_shape_meta(*svg->file_data);
}

const char *simple_shape_name(SimpleShapeType type)
{
    switch (type) {
    case SimpleShapeType::Circle: return "Circle";
    case SimpleShapeType::Square: return "Square";
    case SimpleShapeType::Triangle: return "Triangle";
    case SimpleShapeType::Pentagon: return "Pentagon";
    case SimpleShapeType::Hexagon: return "Hexagon";
    case SimpleShapeType::Octagon: return "Octagon";
    case SimpleShapeType::Star: return "Star";
    case SimpleShapeType::Ring: return "Ring";
    }
    return "Shape";
}

std::string simple_shape_path_in_3mf(const SimpleShapeParams &params)
{
    // unique per generated data: copies of a volume share the path until one of them is changed
    return std::string("3D/shape_") + shape_key(params.type) + "_" + create_code_group_id() + ".svg";
}

} // namespace Slic3r
