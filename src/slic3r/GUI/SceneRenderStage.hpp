#ifndef slic3r_SceneRenderStage_hpp_
#define slic3r_SceneRenderStage_hpp_
namespace Slic3r { namespace GUI {
// Legacy preserves the original draw ordering for AO Off and unsupported views.
enum class SceneRenderStage { Legacy, Surface, Decoration, AOReceiverDepth };
}} // namespace Slic3r::GUI
#endif
