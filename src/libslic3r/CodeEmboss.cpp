#include "CodeEmboss.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>

#include "nlohmann/json.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "EmbossShape.hpp"
#include "Model.hpp"
#include "NSVGUtils.hpp"

namespace Slic3r {

namespace {

constexpr const char *META_BEGIN = "<metadata id=\"edgeslicer-code\">";
constexpr const char *META_END   = "</metadata>";
// Version of stored metadata
constexpr int META_FORMAT = 1;

// QR finder patterns with separators, format and version information
// lay in 9 modules from the corners, logo must stay out of them.
constexpr int QR_PROTECTED_BORDER = 9;

coord_t mm_to_coord(double mm) { return static_cast<coord_t>(std::llround(scale_(mm))); }

// Integer [in micrometers] of scaled coordinate, SVG is written in micrometers
long long coord_to_um(coord_t c) { return std::llround(unscale<double>(c) * 1000.); }

std::string um_to_mm_string(long long um)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s%lld.%03lld", um < 0 ? "-" : "", std::llabs(um) / 1000, std::llabs(um) % 1000);
    return buffer;
}

Slic3r::Polygon rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    // counter clockwise in Y up, orientation is fixed by union later
    return Slic3r::Polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
}

std::string meta_xml_escape(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        default: result += c;
        }
    }
    return result;
}

std::string meta_xml_unescape(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '&') {
            static const std::pair<const char *, char> entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool found = false;
            for (const auto &[name, c] : entities) {
                size_t len = std::char_traits<char>::length(name);
                if (text.compare(i, len, name) == 0) {
                    result += c;
                    i += len - 1;
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
        }
        result += text[i];
    }
    return result;
}

std::string to_svg(const ExPolygons &shape, coord_t width, coord_t height, const char *fill, const std::string &meta)
{
    long long w = coord_to_um(width);
    long long h = coord_to_um(height);
    std::stringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << um_to_mm_string(w) << "mm\" height=\"" << um_to_mm_string(h)
       << "mm\" viewBox=\"0 0 " << w << " " << h << "\">\n";
    ss << META_BEGIN << meta_xml_escape(meta) << META_END << "\n";
    ss << "<path fill=\"" << fill << "\" fill-rule=\"evenodd\" d=\"";
    auto write_polygon = [&ss](const Slic3r::Polygon &polygon) {
        if (polygon.points.size() < 3)
            return;
        bool first = true;
        for (const Point &p : polygon.points) {
            ss << (first ? "M" : "L") << coord_to_um(p.x()) << " " << coord_to_um(p.y());
            first = false;
        }
        ss << "Z";
    };
    for (const ExPolygon &expoly : shape) {
        write_polygon(expoly.contour);
        for (const Slic3r::Polygon &hole : expoly.holes)
            write_polygon(hole);
    }
    ss << "\"/>\n</svg>\n";
    return ss.str();
}

const char *to_string(CodeSurround surround)
{
    switch (surround) {
    case CodeSurround::Square: return "square";
    case CodeSurround::Rounded: return "rounded";
    case CodeSurround::Circle: return "circle";
    }
    return "square";
}

// Rectangle with rounded corners, symmetric around its center
Slic3r::Polygon rounded_rectangle(coord_t x0, coord_t y0, coord_t x1, coord_t y1, coord_t radius, size_t segments_per_corner = 16)
{
    radius = std::min({radius, (x1 - x0) / 2, (y1 - y0) / 2});
    if (radius <= 0)
        return rectangle(x0, y0, x1, y1);
    Slic3r::Polygon result;
    const std::array<std::pair<Point, double>, 4> corners = {{{{x1 - radius, y0 + radius}, -PI / 2.},
                                                              {{x1 - radius, y1 - radius}, 0.},
                                                              {{x0 + radius, y1 - radius}, PI / 2.},
                                                              {{x0 + radius, y0 + radius}, PI}}};
    for (const auto &[center, start] : corners)
        for (size_t i = 0; i <= segments_per_corner; ++i) {
            double a = start + (PI / 2.) * double(i) / double(segments_per_corner);
            result.points.emplace_back(center.x() + coord_t(std::llround(radius * std::cos(a))),
                                       center.y() + coord_t(std::llround(radius * std::sin(a))));
        }
    return result;
}

// FNV-1a, stable between platforms, so edit of the code on other computer keeps the pattern
uint64_t stable_hash(const std::string &text)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool random_bit(uint64_t seed, int64_t x, int64_t y)
{
    uint64_t h = seed ^ (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return (h & 1) != 0;
}

const char *to_string(CodeLogoClear clear)
{
    switch (clear) {
    case CodeLogoClear::Outline: return "outline";
    case CodeLogoClear::Square: return "square";
    case CodeLogoClear::Circle: return "circle";
    }
    return "outline";
}

template<typename Enum, size_t N> Enum from_string(const std::string &name, const std::array<Enum, N> &values, Enum def)
{
    for (Enum e : values)
        if (name == to_string(e))
            return e;
    return def;
}

// Place logo into square with center and half size, logo is Y up, result is Y down
ExPolygons place_logo(const ExPolygons &logo, const Point &center, coord_t half_size)
{
    BoundingBox bb = get_extents(logo);
    if (!bb.defined || bb.size().x() <= 0 || bb.size().y() <= 0)
        return {};
    double scale  = (2. * half_size) / std::max(bb.size().x(), bb.size().y());
    Vec2d  bb_center = bb.center().cast<double>();
    ExPolygons result = logo;
    auto transform = [&](Slic3r::Polygon &polygon) {
        for (Point &p : polygon.points) {
            Vec2d v = (p.cast<double>() - bb_center) * scale;
            p.x() = center.x() + coord_t(std::llround(v.x()));
            p.y() = center.y() - coord_t(std::llround(v.y()));
        }
        // mirror by Y change orientation
        polygon.reverse();
    };
    for (ExPolygon &expoly : result) {
        transform(expoly.contour);
        for (Slic3r::Polygon &hole : expoly.holes)
            transform(hole);
    }
    return union_ex(result);
}

Slic3r::Polygon circle(const Point &center, double radius, size_t count = 64)
{
    Slic3r::Polygon result;
    result.points.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        double a = 2. * PI * double(i) / double(count);
        result.points.emplace_back(center.x() + coord_t(std::llround(radius * std::cos(a))),
                                   center.y() + coord_t(std::llround(radius * std::sin(a))));
    }
    return result;
}

bool is_inside(const ExPolygons &zone, const BoundingBox &zone_bb, const Point &p)
{
    if (!zone_bb.contains(p))
        return false;
    for (const ExPolygon &expoly : zone)
        if (expoly.contains(p))
            return true;
    return false;
}

} // namespace

bool CodeEmbossParams::operator==(const CodeEmbossParams &o) const
{
    return symbology == o.symbology && text == o.text && ecc == o.ecc && module_size == o.module_size && bar_height == o.bar_height &&
           quiet_zone == o.quiet_zone && surround == o.surround && decorate == o.decorate && light_part == o.light_part && dark_depth == o.dark_depth && light_depth == o.light_depth &&
           logo_depth == o.logo_depth && has_logo == o.has_logo && logo_size == o.logo_size && logo_margin == o.logo_margin &&
           logo_clear == o.logo_clear && group_id == o.group_id;
}

const CodeEmbossPart *CodeEmbossResult::part(CodePartRole role) const
{
    for (const CodeEmbossPart &p : parts)
        if (p.role == role)
            return &p;
    return nullptr;
}

CodeEmbossResult create_code_emboss(const CodeEmbossParams &params, const ExPolygons *logo)
{
    CodeEmbossResult result;
    Barcode::QrOptions qr_options;
    qr_options.ecc       = params.ecc;
    qr_options.boost_ecc = true;
    result.code          = Barcode::encode(params.symbology, params.text, qr_options);
    if (!result.code.is_valid()) {
        result.error = result.code.error;
        return result;
    }

    const Barcode::Matrix &matrix = result.code.matrix;
    const bool             is_2d  = matrix.is_2d();

    // module size is rounded to even micrometers, so SVG coordinates (in micrometers)
    // are exact integers also for the center of the code
    double module_mm = std::max(0.01, std::round(params.module_size * 500.) / 500.);
    coord_t ms       = mm_to_coord(module_mm);

    int quiet = std::max(0, params.quiet_zone);
    if (params.light_part && quiet < 1) {
        // Light part must surround the dark one to keep the same center of all parts
        quiet = 1;
        result.warnings.push_back("Quiet zone was enlarged to 1 module, it is necessary for the light part.");
    }
    // Linear codes do not need tall vertical quiet zone
    int     quiet_y  = is_2d ? quiet : std::min(quiet, 3);
    coord_t code_w   = coord_t(matrix.width) * ms;
    coord_t code_h   = is_2d ? coord_t(matrix.height) * ms : mm_to_coord(std::round(std::max(module_mm, params.bar_height) * 500.) / 500.);
    coord_t offset_x = coord_t(quiet) * ms;
    coord_t offset_y = coord_t(quiet_y) * ms;
    coord_t width    = code_w + 2 * offset_x;
    coord_t height   = code_h + 2 * offset_y;
    // Radius of circle surround, circumscribed to the code with its quiet zone
    coord_t radius = 0;
    if (params.surround == CodeSurround::Circle) {
        coord_t um   = mm_to_coord(0.001);
        double  r    = 0.5 * std::hypot(double(width), double(height));
        radius       = coord_t(std::ceil(r / double(um))) * um;
        offset_x     = radius - code_w / 2;
        offset_y     = radius - code_h / 2;
        width        = 2 * radius;
        height       = 2 * radius;
    }
    const Point center(width / 2, height / 2);
    result.width  = unscale<double>(width);
    result.height = unscale<double>(height);

    std::vector<uint8_t> dark = matrix.dark;

    // Logo
    ExPolygons logo_shape;
    if (is_2d && params.has_logo && logo != nullptr && !logo->empty()) {
        int    n         = matrix.width;
        int    margin    = std::max(0, params.logo_margin);
        double max_half  = n / 2. - QR_PROTECTED_BORDER - margin; // [in modules]
        double want_half = std::clamp(params.logo_size, 0., 1.) * n / 2.;
        double half      = std::min(want_half, max_half);
        if (half < 1.) {
            result.warnings.push_back("QR code is too small to place the logo. Use longer text or higher error correction.");
        } else {
            if (half + 1e-6 < want_half)
                result.warnings.push_back("Logo was made smaller to not cover the position patterns of QR code.");
            result.logo_size = 2. * half / n;

            coord_t half_size = coord_t(std::llround(half * ms));
            logo_shape        = place_logo(*logo, center, half_size);

            // Area cleared of the modules
            coord_t    margin_c = coord_t(margin) * ms;
            ExPolygons zone;
            if (!logo_shape.empty()) {
                BoundingBox bb = get_extents(logo_shape);
                switch (params.logo_clear) {
                case CodeLogoClear::Outline:
                    zone = margin_c > 0 ? offset_ex(logo_shape, float(margin_c)) : logo_shape;
                    break;
                case CodeLogoClear::Square: {
                    BoundingBox sq = bb;
                    sq.offset(margin_c);
                    zone.emplace_back(rectangle(sq.min.x(), sq.min.y(), sq.max.x(), sq.max.y()));
                    break;
                }
                case CodeLogoClear::Circle: {
                    double radius = bb.size().cast<double>().norm() / 2. + double(margin_c);
                    zone.emplace_back(circle(bb.center(), radius));
                    break;
                }
                }
            }

            // Clear all modules touched by zone
            BoundingBox zone_bb = get_extents(zone);
            const std::vector<uint8_t> function = Barcode::qr_function_modules(result.code.qr_version);
            int damaged_data_modules = 0;
            int data_modules         = 0;
            coord_t inset = std::max<coord_t>(1, ms / 8);
            for (int y = 0; y < matrix.height; ++y)
                for (int x = 0; x < matrix.width; ++x) {
                    size_t index = size_t(y) * size_t(matrix.width) + size_t(x);
                    bool is_data = function[index] == 0;
                    if (is_data)
                        ++data_modules;
                    coord_t x0 = offset_x + coord_t(x) * ms, y0 = offset_y + coord_t(y) * ms;
                    coord_t x1 = x0 + ms, y1 = y0 + ms;
                    BoundingBox module_bb(Point(x0, y0), Point(x1, y1));
                    if (!zone_bb.overlap(module_bb))
                        continue;
                    const Point samples[] = {{(x0 + x1) / 2, (y0 + y1) / 2}, {x0 + inset, y0 + inset}, {x1 - inset, y0 + inset},
                                             {x0 + inset, y1 - inset}, {x1 - inset, y1 - inset}};
                    bool touched = std::any_of(std::begin(samples), std::end(samples),
                                               [&](const Point &p) { return is_inside(zone, zone_bb, p); });
                    if (!touched)
                        continue;
                    ++result.cleared_modules;
                    if (is_data)
                        ++damaged_data_modules;
                    dark[index] = 0;
                }

            // Modules are grouped by 8 into codewords, damaged area hit whole codewords
            double damaged  = data_modules > 0 ? double(damaged_data_modules) / data_modules : 0.;
            double capacity = Barcode::qr_recovery_capacity(result.code.qr_ecc);
            if (damaged > 0.6 * capacity)
                result.warnings.push_back("Logo covers too many modules for the error correction level, the QR code may not be readable. "
                                          "Make the logo smaller or use higher error correction.");
        }
    }

    // Dark modules as rectangles, horizontal runs are merged
    Polygons rectangles;
    for (int y = 0; y < matrix.height; ++y) {
        coord_t y0 = offset_y + coord_t(y) * ms;
        coord_t y1 = is_2d ? y0 + ms : offset_y + code_h;
        for (int x = 0; x < matrix.width;) {
            if (dark[size_t(y) * size_t(matrix.width) + size_t(x)] == 0) {
                ++x;
                continue;
            }
            int end = x;
            while (end < matrix.width && dark[size_t(y) * size_t(matrix.width) + size_t(end)] != 0)
                ++end;
            rectangles.push_back(rectangle(offset_x + coord_t(x) * ms, y0, offset_x + coord_t(end) * ms, y1));
            x = end;
        }
    }

    // Circular QR code: random modules between the quiet zone and the circle
    bool decorate = is_2d && params.decorate && params.surround == CodeSurround::Circle;
    if (decorate) {
        // quiet zone rectangle must stay light
        coord_t qx0 = offset_x - coord_t(quiet) * ms, qx1 = offset_x + code_w + coord_t(quiet) * ms;
        coord_t qy0 = offset_y - coord_t(quiet) * ms, qy1 = offset_y + code_h + coord_t(quiet) * ms;
        // one module of light rim along the circle
        double  limit    = double(radius - ms);
        double  limit_sq = limit * limit;
        int64_t cells    = int64_t(offset_x / ms) + 1;
        struct Cell { int64_t x, y; };
        std::vector<Cell> eligible;
        for (int64_t y = -cells; y < matrix.height + cells; ++y)
            for (int64_t x = -cells; x < matrix.width + cells; ++x) {
                coord_t x0 = offset_x + coord_t(x) * ms, y0 = offset_y + coord_t(y) * ms;
                coord_t x1 = x0 + ms, y1 = y0 + ms;
                if (x1 > qx0 && x0 < qx1 && y1 > qy0 && y0 < qy1)
                    continue; // touch the code or its quiet zone
                bool inside = true;
                for (const Point &p : {Point(x0, y0), Point(x1, y0), Point(x0, y1), Point(x1, y1)}) {
                    double dx = double(p.x() - center.x()), dy = double(p.y() - center.y());
                    if (dx * dx + dy * dy > limit_sq)
                        inside = false;
                }
                if (inside)
                    eligible.push_back({x, y});
            }
        if (!eligible.empty()) {
            // Cells on the extremes are always dark, so the pattern keeps the center of the code
            // (parts are aligned by the center of their bounding box)
            int64_t min_x = eligible.front().x, max_x = min_x, min_y = eligible.front().y, max_y = min_y;
            for (const Cell &c : eligible) {
                min_x = std::min(min_x, c.x);
                max_x = std::max(max_x, c.x);
                min_y = std::min(min_y, c.y);
                max_y = std::max(max_y, c.y);
            }
            uint64_t seed = stable_hash(params.text);
            for (const Cell &c : eligible) {
                bool extreme = c.x == min_x || c.x == max_x || c.y == min_y || c.y == max_y;
                if (!extreme && !random_bit(seed, c.x, c.y))
                    continue;
                coord_t x0 = offset_x + coord_t(c.x) * ms, y0 = offset_y + coord_t(c.y) * ms;
                rectangles.push_back(rectangle(x0, y0, x0 + ms, y0 + ms));
            }
        }
        if (quiet < 2)
            result.warnings.push_back("Keep at least 2 modules of quiet zone between the QR code and the circular pattern, "
                                      "otherwise scanners may not find the code.");
    }

    ExPolygons dark_shape = union_ex(rectangles);
    if (dark_shape.empty()) {
        result.error = "Code does not contain any dark module.";
        return result;
    }

    CodeEmbossMeta meta;
    meta.params = params;
    // store really used values
    meta.params.ecc        = result.code.qr_ecc;
    meta.params.quiet_zone = quiet;
    meta.params.module_size = module_mm;
    if (!is_2d)
        meta.params.has_logo = false;
    meta.params.decorate = decorate;

    auto add_part = [&](CodePartRole role, ExPolygons &&shape, double depth, const char *fill) {
        meta.role = role;
        CodeEmbossPart part;
        part.role  = role;
        part.depth = depth;
        part.svg   = to_svg(shape, width, height, fill, write_code_emboss_meta(meta));
        part.shape = std::move(shape);
        result.parts.push_back(std::move(part));
    };

    ExPolygons light_shape;
    if (params.light_part) {
        Polygons clip = to_polygons(dark_shape);
        polygons_append(clip, to_polygons(logo_shape));
        Slic3r::Polygon outline;
        switch (params.surround) {
        case CodeSurround::Square: outline = rectangle(0, 0, width, height); break;
        case CodeSurround::Rounded: outline = rounded_rectangle(0, 0, width, height, std::min(offset_x, offset_y)); break;
        case CodeSurround::Circle: outline = circle(center, double(radius), 128); break;
        }
        light_shape = diff_ex(Polygons{outline}, clip);
    }

    add_part(CodePartRole::Dark, std::move(dark_shape), params.dark_depth, "#000000");
    if (!light_shape.empty())
        add_part(CodePartRole::Light, std::move(light_shape), params.light_depth, "#FFFFFF");
    if (!logo_shape.empty())
        add_part(CodePartRole::Logo, std::move(logo_shape), params.logo_depth, "#1E6FD9");
    return result;
}

ExPolygons load_code_logo(const std::string &svg_data)
{
    NSVGimage_ptr image = nsvgParse(svg_data);
    if (image == nullptr)
        return {};
    Vec2f min(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec2f max(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    bounds(*image, min, max);
    double size = (max.x() > min.x() && max.y() > min.y()) ? std::max(max.x() - min.x(), max.y() - min.y()) : 10.;
    // tolerance is squared distance in scaled coordinates
    double tolerance_mm = std::max(size / 1000., 1e-4);
    NSVGLineParams    params{std::pow(tolerance_mm / SCALING_FACTOR, 2)};
    ExPolygonsWithIds shapes = create_shape_with_ids(*image, params);
    ExPolygons        result;
    for (ExPolygonsWithId &s : shapes)
        expolygons_append(result, std::move(s.expoly));
    return union_ex(result);
}

std::string write_code_emboss_meta(const CodeEmbossMeta &meta)
{
    const CodeEmbossParams &p = meta.params;
    nlohmann::json          j;
    j["format"]      = META_FORMAT;
    j["group"]       = p.group_id;
    j["role"]        = to_string(meta.role);
    j["symbology"]   = Barcode::to_string(p.symbology);
    j["text"]        = p.text;
    j["ecc"]         = Barcode::to_string(p.ecc);
    j["module"]      = p.module_size;
    j["bar_height"]  = p.bar_height;
    j["quiet"]       = p.quiet_zone;
    j["surround"]    = to_string(p.surround);
    j["decorate"]    = p.decorate;
    j["light"]       = p.light_part;
    j["dark_depth"]  = p.dark_depth;
    j["light_depth"] = p.light_depth;
    j["logo_depth"]  = p.logo_depth;
    j["logo"]        = p.has_logo;
    j["logo_size"]   = p.logo_size;
    j["logo_margin"] = p.logo_margin;
    j["logo_clear"]  = to_string(p.logo_clear);
    // ASCII only output, so it is safe inside of any SVG encoding
    return j.dump(-1, ' ', true);
}

std::optional<CodeEmbossMeta> read_code_emboss_meta(const std::string &svg_data)
{
    size_t begin = svg_data.find(META_BEGIN);
    if (begin == std::string::npos)
        return {};
    begin += std::char_traits<char>::length(META_BEGIN);
    size_t end = svg_data.find(META_END, begin);
    if (end == std::string::npos)
        return {};

    nlohmann::json j = nlohmann::json::parse(meta_xml_unescape(svg_data.substr(begin, end - begin)), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return {};

    CodeEmbossMeta   meta;
    CodeEmbossParams &p = meta.params;
    try {
        p.group_id = j.value("group", std::string());
        if (p.group_id.empty())
            return {};
        using namespace Barcode;
        meta.role   = from_string(j.value("role", std::string()), std::array{CodePartRole::Dark, CodePartRole::Light, CodePartRole::Logo},
                                  CodePartRole::Dark);
        p.symbology = from_string(j.value("symbology", std::string()),
                                  std::array{Symbology::QR, Symbology::Code128, Symbology::EAN13, Symbology::UPCA, Symbology::Code39},
                                  Symbology::QR);
        p.text        = j.value("text", std::string());
        p.ecc         = from_string(j.value("ecc", std::string()), std::array{QrEcc::Low, QrEcc::Medium, QrEcc::Quartile, QrEcc::High},
                                    QrEcc::Medium);
        p.module_size = j.value("module", p.module_size);
        p.bar_height  = j.value("bar_height", p.bar_height);
        p.quiet_zone  = j.value("quiet", p.quiet_zone);
        p.surround    = from_string(j.value("surround", std::string()),
                                    std::array{CodeSurround::Square, CodeSurround::Rounded, CodeSurround::Circle}, CodeSurround::Square);
        p.decorate    = j.value("decorate", p.decorate);
        p.light_part  = j.value("light", p.light_part);
        p.dark_depth  = j.value("dark_depth", p.dark_depth);
        p.light_depth = j.value("light_depth", p.light_depth);
        p.logo_depth  = j.value("logo_depth", p.logo_depth);
        p.has_logo    = j.value("logo", p.has_logo);
        p.logo_size   = j.value("logo_size", p.logo_size);
        p.logo_margin = j.value("logo_margin", p.logo_margin);
        p.logo_clear  = from_string(j.value("logo_clear", std::string()),
                                    std::array{CodeLogoClear::Outline, CodeLogoClear::Square, CodeLogoClear::Circle}, CodeLogoClear::Outline);
    } catch (const nlohmann::json::exception &) {
        return {};
    }
    return meta;
}

std::optional<CodeEmbossMeta> read_code_emboss_meta(const ModelVolume &volume)
{
    if (!volume.emboss_shape.has_value())
        return {};
    const std::optional<EmbossShape::SvgFile> &svg = volume.emboss_shape->svg_file;
    if (!svg.has_value() || svg->file_data == nullptr)
        return {};
    return read_code_emboss_meta(*svg->file_data);
}

std::string create_code_group_id()
{
    static std::mt19937_64 generator(std::random_device{}() ^
                                     static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(generator()));
    return buffer;
}

std::vector<ModelVolume *> get_code_volumes(const ModelObject &object, const std::string &group_id)
{
    std::vector<ModelVolume *> result;
    if (group_id.empty())
        return result;
    for (ModelVolume *volume : object.volumes) {
        std::optional<CodeEmbossMeta> meta = read_code_emboss_meta(*volume);
        if (meta.has_value() && meta->params.group_id == group_id)
            result.push_back(volume);
    }
    return result;
}

std::string code_part_path_in_3mf(const std::string &group_id, CodePartRole role)
{
    return "3D/code_" + group_id + "_" + to_string(role) + ".svg";
}

const char *to_string(CodePartRole role)
{
    switch (role) {
    case CodePartRole::Dark: return "dark";
    case CodePartRole::Light: return "light";
    case CodePartRole::Logo: return "logo";
    }
    return "dark";
}

std::string code_part_name(const CodeEmbossParams &params, CodePartRole role)
{
    std::string name = params.symbology == Barcode::Symbology::QR ? "QR code" : "Barcode";
    switch (role) {
    case CodePartRole::Dark: return name + " - dark";
    case CodePartRole::Light: return name + " - light";
    case CodePartRole::Logo: return name + " - logo";
    }
    return name;
}

} // namespace Slic3r
