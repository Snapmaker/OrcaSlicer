#include "AOTypes.hpp"
#include "AOObserver.hpp"

#include <locale>
#include <iomanip>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

std::string make_visual_signature(const AOActualConfig& actual, const AOSettings& settings)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "size" << actual.eval_width << 'x' << actual.eval_height << "/radius" << settings.radius << "/thickness"
        << settings.thickness << "/strength" << settings.strength << '/';
    out << "v1/" << (actual.backend == Backend::Compute ? "cs" : "fs") << '/' << int(actual.quality) << '/' << actual.slices << 'x'
        << actual.steps << '/' << (actual.filter == FilterMode::Connectivity ? "edge" : "geom") << actual.filter_passes << '/';
    const char* composite = actual.composite == CompositeVariant::Pixel      ? "pixel" :
                            actual.composite == CompositeVariant::Sample     ? "sample" :
                            actual.composite == CompositeVariant::SplitMsaa  ? "split" :
                            actual.composite == CompositeVariant::Confidence ? "conf" :
                                                                               "ref";
    out << composite << (actual.fast_reconstruction ? "/xy" : "") << (actual.cs_low_reference ? "/lowlegacy" : "") << '/'
        << (actual.response.output_exponent_applied ? "exp" : "lin") << actual.response.composite_intensity << "/dbg" << settings.debug_view
        << "/pv" << actual.programs.profile_version << 'g' << actual.programs.generation;
    return out.str();
}

bool stage_has_timestamp(AOStage stage)
{
    switch (stage) {
    case AOStage::ColorBeforeComposite:
    case AOStage::ColorCompositeComplete: return false;
    default: return true;
    }
}

const char* frame_target_label(FrameTargetKind kind)
{
    switch (kind) {
    case FrameTargetKind::CanvasMsaa: return "canvas_msaa";
    case FrameTargetKind::WindowFallback: return "window_fallback";
    case FrameTargetKind::Window: return "window";
    }
    return "window";
}

}}} // namespace Slic3r::GUI::AO
