#ifndef slic3r_ImageTrace_hpp_
#define slic3r_ImageTrace_hpp_

// Trace a raster image (PNG / JPG) into filled shapes, which are embossed as SVG volumes.
//
// Pipeline (no wxWidgets, no new dependency):
//   decode (OpenCV imgcodecs, refused before decoding when the header announces a huge image)
//   -> downscale to the working resolution (OpenCV INTER_AREA, alpha premultiplied)
//   -> one colour level  : alpha or brightness (Otsu threshold by default) field
//      several colours   : palette by a deterministic k-means over a colour histogram, the
//                          background colour (most common at the border) can be left out
//   -> per level: Gaussian blur -> marching squares with linear interpolation (sub-pixel)
//   -> Clipper (holes by even-odd) -> remove specks -> Douglas-Peucker -> SVG in millimetres.
//
// Every colour level becomes its own SVG volume (one extruder per part). Parameters, the
// position of the level inside of the image and (optionally) a downscaled copy of the source
// image are stored inside of the generated SVG (<metadata>), so the trace stays editable after
// the project is saved into .3mf. The local path of the image is never stored.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ExPolygon.hpp"
#include "Point.hpp"

namespace Slic3r {

class ModelVolume;
class ModelObject;

// Decoded image, 8 bit RGBA, rows from the top
struct TraceImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba; // width * height * 4

    bool empty() const { return width <= 0 || height <= 0 || rgba.size() != size_t(width) * size_t(height) * 4; }
    // Any pixel is noticeably transparent
    bool has_alpha() const;
};

// Which property of the image becomes the shape when only one colour level is traced
enum class TraceShapeFrom : int { Auto = 0, Alpha, Brightness };

// How several colour levels are laid out
enum class TraceLayout : int {
    // Every pixel belongs to exactly one level, all levels have the same depth (multi colour print)
    Flat = 0,
    // Level k covers all levels from k up, every level is deeper by depth_step (stepped relief)
    Stacked
};

struct ImageTraceParams
{
    // Number of colour levels = parts, 1 .. IMAGE_TRACE_MAX_COLORS
    int colors = 4;

    // Only for one colour level
    TraceShapeFrom shape_from = TraceShapeFrom::Auto;
    // Brightness: pixels darker than threshold (0..255) become the shape
    int  threshold      = 128;
    bool auto_threshold = true; // Otsu
    // One colour: the light (or transparent) part becomes the shape.
    // Stacked: the darkest level is on the top instead of the lightest.
    bool invert = false;

    // Several colours of an opaque image: leave out the colour which covers most of the border
    bool remove_background = true;

    // Smoothing, sigma of Gaussian blur in pixels of the working resolution
    double blur = 1.;
    // Islands and holes smaller than this are removed [in mm^2]
    double despeckle = 0.05;
    // Douglas-Peucker tolerance in pixels of the working resolution
    double detail = 0.5;
    // Longest side of the working resolution [in px]
    int max_side = 1024;

    // Size of the whole image [in mm], height follows the aspect ratio
    double width = 40.;
    // Emboss depth of the (first) level [in mm]
    double depth = 1.;
    TraceLayout layout = TraceLayout::Flat;
    // Stacked: additional depth of every next level [in mm]
    double depth_step = 0.4;

    // Store a downscaled copy of the source image in the SVG, to allow re-trace
    bool keep_source = true;

    // Identify parts of the same trace, unique per trace
    std::string group_id;

    bool operator==(const ImageTraceParams &o) const;
    bool operator!=(const ImageTraceParams &o) const { return !(*this == o); }
};

constexpr int IMAGE_TRACE_MAX_COLORS = 8;

struct ImageTraceLayer
{
    // Shape in SVG coordinates (Y down, origin in the top left corner of the image), scaled
    ExPolygons shape;
    // Content of SVG file (include metadata), empty when the trace was made only for preview
    std::string svg;
    // Average colour of the level in the image
    std::array<uint8_t, 3> color{0, 0, 0};
    double                 depth = 1.;
    // Center of the bounding box of the shape relative to the center of the image [in mm], Y up.
    // SVG volumes are centered by their own bounding box, so the volume of this level has to be
    // moved by this offset (in its local XY plane) to stay aligned with the other levels.
    Vec2d  offset = Vec2d::Zero();
    size_t contours = 0;
    size_t points   = 0;
};

struct ImageTraceResult
{
    std::vector<ImageTraceLayer> layers;
    // Size of the whole image [in mm]
    double width  = 0.;
    double height = 0.;
    // Working resolution
    int work_width  = 0;
    int work_height = 0;
    // Threshold used for brightness (Otsu result when auto), -1 when not used
    int threshold = -1;
    // Number of colours found (can be less than wanted), background excluded
    int colors_found = 0;
    // Shape is created from transparency
    bool from_alpha = false;
    // Empty on success
    std::string error;
    // Over limits of SVG (#290) - the trace has to be simplified
    bool too_detailed = false;
    // Non fatal issues
    std::vector<std::string> warnings;
    size_t contours = 0;
    size_t points   = 0;

    bool is_valid() const { return error.empty() && !layers.empty(); }
};

// Limits of input image, checked from the file header before the image is decoded
constexpr size_t   IMAGE_TRACE_MAX_FILE_SIZE = size_t(64) * 1024 * 1024;
constexpr int      IMAGE_TRACE_MAX_SIDE      = 20000;
constexpr uint64_t IMAGE_TRACE_MAX_PIXELS    = uint64_t(100) * 1000 * 1000;

/// <summary>
/// Read width and height from the header of PNG or JPG data.
/// </summary>
/// <returns>False for unknown or broken data</returns>
bool trace_image_dimensions(const std::string &data, int &width, int &height);

/// <summary>
/// Decode PNG or JPG data (any bit depth / channels) into 8 bit RGBA.
/// Huge images are refused before decoding (IMAGE_TRACE_MAX_*).
/// </summary>
/// <param name="error">Reason of refusal, translatable english text</param>
bool decode_trace_image(const std::string &data, TraceImage &out, std::string *error = nullptr);

/// <summary>
/// Downscaled copy of the image encoded as PNG, used to store the source inside of the SVG.
/// The image is downscaled until the PNG fits into max_bytes.
/// </summary>
/// <returns>PNG data, empty on failure</returns>
std::string encode_trace_source(const TraceImage &image, int max_side = 1024, size_t max_bytes = size_t(1536) * 1024);

/// <summary>
/// Downscale the image so its longest side is at most max_side (alpha is premultiplied while resampling)
/// </summary>
TraceImage downscale_trace_image(const TraceImage &image, int max_side);

/// <summary>
/// Trace the image into shapes, one per colour level.
/// </summary>
/// <param name="image">Source image</param>
/// <param name="params">Parameters of the trace</param>
/// <param name="source_png">Encoded copy of the source stored into the first level when params.keep_source</param>
/// <param name="write_svg">False for a fast preview: no SVG text and no SVG limit check by NanoSVG</param>
ImageTraceResult trace_image(const TraceImage &image, const ImageTraceParams &params, const std::string &source_png = {},
                             bool write_svg = true);

// Metadata stored in SVG of a traced level
struct ImageTraceMeta
{
    ImageTraceParams params;
    int    layer  = 0; // index of the level
    int    layers = 1; // count of levels of the trace
    std::array<uint8_t, 3> color{0, 0, 0};
    Vec2d  offset = Vec2d::Zero();
    // Size of the whole image [in mm]
    double width  = 0.;
    double height = 0.;
    // PNG data of the source image, empty when not stored (or not read)
    std::string source_png;
};

std::string write_image_trace_meta(const ImageTraceMeta &meta);
/// <param name="with_source">Decode also the stored source image (can be large)</param>
std::optional<ImageTraceMeta> read_image_trace_meta(const std::string &svg_data, bool with_source = false);
// Read metadata from SVG volume, nullopt when volume is not a traced image
std::optional<ImageTraceMeta> read_image_trace_meta(const ModelVolume &volume, bool with_source = false);

// Volumes of object, which are levels of the trace with group_id
std::vector<ModelVolume *> get_image_trace_volumes(const ModelObject &object, const std::string &group_id);

// Path of SVG in .3mf archive for a traced level, unique for each call
std::string image_trace_path_in_3mf(const std::string &group_id, int layer);

// Name of the volume of a level, base is e.g. name of the image file without extension
std::string image_trace_part_name(const std::string &base, int layer, int layers);

// "#RRGGBB"
std::string image_trace_color_to_hex(const std::array<uint8_t, 3> &color);

} // namespace Slic3r

#endif // slic3r_ImageTrace_hpp_
