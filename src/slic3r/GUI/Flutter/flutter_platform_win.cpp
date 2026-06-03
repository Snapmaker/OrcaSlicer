#include "flutter_platform.h"

namespace FlutterPlatform {

std::unique_ptr<FlutterEngineHost> createEngine() {
    return ::createFlutterEngine("", "");
}

static const Capabilities kCaps = {
    true, false, RendererBackend::RENDERER_IMPELLER, PlatformId::PLATFORM_WINDOWS
};

const Capabilities& capabilities() { return kCaps; }

} // namespace FlutterPlatform
