#ifndef slic3r_FirstLayerReference_hpp_
#define slic3r_FirstLayerReference_hpp_

#include "ThumbnailData.hpp"
#include <string>

namespace Slic3r {
struct GCodeProcessorResult;
class DynamicPrintConfig;

struct FirstLayerReference
{
    // Bottom-up RGBA, matching ThumbnailData and the 3MF PNG encoder.
    ThumbnailData  image;
    nlohmann::json metadata;
};

// A deterministic, orthographic extrusion footprint, independent of GUI/OpenGL state.
// Non-ready results have metadata describing the reason and no image.
FirstLayerReference render_first_layer_reference(const GCodeProcessorResult& result,
                                                 const DynamicPrintConfig&   config,
                                                 int                         plate_number,
                                                 const std::string&          gcode_md5,
                                                 unsigned int                long_edge = 2048);
} // namespace Slic3r

#endif
