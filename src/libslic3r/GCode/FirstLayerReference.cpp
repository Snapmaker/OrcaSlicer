#include "FirstLayerReference.hpp"
#include "GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <agg/agg_path_storage.h>
#include <agg/agg_pixfmt_gray.h>
#include <agg/agg_rasterizer_scanline_aa.h>
#include <agg/agg_renderer_scanline.h>
#include <agg/agg_scanline_p.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace Slic3r {
namespace {
bool printable_extrusion(const GCodeProcessorResult::MoveVertex& move)
{
    return move.type == EMoveType::Extrude && move.extrusion_role > erNone && move.extrusion_role < erMixed &&
           move.extrusion_role != erCustom && move.delta_extruder > 0.f && std::isfinite(move.width) && move.width > 0.f &&
           move.position.allFinite();
}

// Add a round-ended extrusion segment. Rasterizing all capsules with the same
// winding produces their union, including joins and overlapping infill.
void add_segment(agg::rasterizer_scanline_aa<>& rasterizer, const Vec2d& a, const Vec2d& b, double radius)
{
    constexpr double  pi    = 3.14159265358979323846;
    const double      angle = std::atan2(b.y() - a.y(), b.x() - a.x());
    agg::path_storage path;
    for (int end = 0; end < 2; ++end) {
        const Vec2d& center = end == 0 ? a : b;
        for (int i = 0; i <= 12; ++i) {
            const double theta = angle + pi * (0.5 + end + double(i) / 12.0);
            const double x     = center.x() + radius * std::cos(theta);
            const double y     = center.y() + radius * std::sin(theta);
            if (end == 0 && i == 0)
                path.move_to(x, y);
            else
                path.line_to(x, y);
        }
    }
    path.close_polygon();
    rasterizer.add_path(path);
}
} // namespace

FirstLayerReference render_first_layer_reference(const GCodeProcessorResult& result,
                                                 const DynamicPrintConfig&   config,
                                                 int                         plate_number,
                                                 const std::string&          gcode_md5,
                                                 unsigned int                long_edge)
{
    FirstLayerReference out;
    out.metadata = {{"version", 1},
                    {"plate_id", plate_number},
                    {"gcode_md5", gcode_md5},
                    {"representation", "extrusion_footprint"},
                    {"scope", "first_printed_layer"},
                    {"foreground", "white"},
                    {"background", "black"},
                    {"status", "no_printable_extrusion"}};
    // Sequential printing has several distinct first-layer completion times.
    // Do not label their union as a simultaneous camera reference.
    if (config.has("print_sequence") && config.opt_serialize("print_sequence") == "by object") {
        out.metadata["status"] = "unsupported_sequential_print";
        return out;
    }
    if (long_edge < 16 || long_edge > 4096) {
        out.metadata["status"] = "invalid_resolution";
        return out;
    }

    Pointfs bed = result.printable_area;
    if (bed.size() < 3) {
        const auto* area = config.option<ConfigOptionPoints>("printable_area");
        if (area != nullptr)
            bed = area->values;
    }
    if (bed.size() < 3 || std::any_of(bed.begin(), bed.end(), [](const Vec2d& p) { return !p.allFinite(); })) {
        out.metadata["status"] = "invalid_printable_area";
        return out;
    }
    Vec2d min = bed.front(), max = min;
    for (const Vec2d& p : bed) {
        min = min.cwiseMin(p);
        max = max.cwiseMax(p);
    }
    const Vec2d extent = max - min;
    if (extent.minCoeff() <= 0.0) {
        out.metadata["status"] = "invalid_printable_area";
        return out;
    }

    unsigned int layer = std::numeric_limits<unsigned int>::max();
    for (const auto& move : result.moves)
        if (printable_extrusion(move) && move.layer_id > 0)
            layer = std::min(layer, move.layer_id);
    if (layer == std::numeric_limits<unsigned int>::max())
        return out;

    const unsigned int width    = std::max(1u, unsigned(std::lround(long_edge * extent.x() / extent.maxCoeff())));
    const unsigned int height   = std::max(1u, unsigned(std::lround(long_edge * extent.y() / extent.maxCoeff())));
    const double       scale    = std::min(width / extent.x(), height / extent.y());
    const Vec2d        padding  = (Vec2d(width, height) - extent * scale) * 0.5;
    auto               to_pixel = [&](const Vec2d& p) -> Vec2d { return (p - min) * scale + padding; };

    std::vector<unsigned char>                                             coverage(size_t(width) * height, 0);
    agg::rendering_buffer                                                  buffer(coverage.data(), width, height, width);
    agg::pixfmt_gray8                                                      pixels(buffer);
    agg::renderer_base<agg::pixfmt_gray8>                                  base(pixels);
    agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> renderer(base);
    renderer.color(agg::gray8(255));
    agg::scanline_p8              scanline;
    agg::rasterizer_scanline_aa<> rasterizer;
    rasterizer.clip_box(0, 0, width, height);

    std::set<std::string> roles;
    double                z_min = std::numeric_limits<double>::max(), z_max = -z_min;
    size_t                segment_count = 0;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& move = result.moves[i];
        if (move.layer_id != layer || !printable_extrusion(move))
            continue;
        Vec3f previous = result.moves[i - 1].position;
        if (!previous.allFinite())
            continue;
        auto draw = [&](const Vec3f& next) {
            if (!next.allFinite())
                return;
            if ((next.head<2>() - previous.head<2>()).squaredNorm() > 1e-12f) {
                add_segment(rasterizer, to_pixel(previous.head<2>().cast<double>()), to_pixel(next.head<2>().cast<double>()),
                            0.5 * move.width * scale);
                ++segment_count;
            }
            previous = next;
        };
        if (move.is_arc_move_with_interpolation_points())
            for (const Vec3f& p : move.interpolation_points)
                draw(p);
        draw(move.position);
        z_min = std::min(z_min, double(move.position.z()));
        z_max = std::max(z_max, double(move.position.z()));
        roles.insert(ExtrusionEntity::role_to_string(move.extrusion_role));
    }
    if (segment_count == 0)
        return out;
    agg::render_scanlines(rasterizer, scanline, renderer);

    // Clip to the actual bed polygon, including circular/non-rectangular beds.
    std::vector<unsigned char>                                             bed_mask(coverage.size(), 0);
    agg::rendering_buffer                                                  mask_buffer(bed_mask.data(), width, height, width);
    agg::pixfmt_gray8                                                      mask_pixels(mask_buffer);
    agg::renderer_base<agg::pixfmt_gray8>                                  mask_base(mask_pixels);
    agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> mask_renderer(mask_base);
    mask_renderer.color(agg::gray8(255));
    rasterizer.reset();
    agg::path_storage boundary;
    for (size_t i = 0; i < bed.size(); ++i) {
        const Vec2d p = to_pixel(bed[i]);
        if (i == 0)
            boundary.move_to(p.x(), p.y());
        else
            boundary.line_to(p.x(), p.y());
    }
    boundary.close_polygon();
    rasterizer.add_path(boundary);
    agg::render_scanlines(rasterizer, scanline, mask_renderer);

    out.image.set(width, height);
    for (size_t i = 0; i < coverage.size(); ++i) {
        const unsigned char value = std::min(coverage[i], bed_mask[i]);
        out.image.pixels[4 * i] = out.image.pixels[4 * i + 1] = out.image.pixels[4 * i + 2] = value;
        out.image.pixels[4 * i + 3]                                                         = 255;
    }
    out.metadata["status"]            = "ready";
    out.metadata["width"]             = width;
    out.metadata["height"]            = height;
    out.metadata["layer_id"]          = layer;
    out.metadata["z_range_mm"]        = {z_min, z_max};
    out.metadata["included_roles"]    = roles;
    out.metadata["excluded_roles"]    = {"Custom", "Travel", "Retract", "Wipe"};
    out.metadata["printable_area_mm"] = nlohmann::json::array();
    for (const Vec2d& p : bed)
        out.metadata["printable_area_mm"].push_back({p.x(), p.y()});
    out.metadata["pixel_origin"]      = "top_left";
    out.metadata["pixel_coordinates"] = "integer_pixel_centers";
    out.metadata["pixel_to_bed_mm"]   = {{1.0 / scale, 0, min.x() + (0.5 - padding.x()) / scale},
                                         {0, -1.0 / scale, min.y() + (height - 0.5 - padding.y()) / scale},
                                         {0, 0, 1}};
    return out;
}
} // namespace Slic3r
