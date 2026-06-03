#include "flutter_platform.h"

namespace FlutterPlatform {

std::unique_ptr<FlutterEngineHost> createEngine() {
    return ::createFlutterEngine("", "");
}

static const Capabilities kCaps = {
    true, false, RendererBackend::RENDERER_METAL, PlatformId::PLATFORM_MACOS
};

const Capabilities& capabilities() { return kCaps; }

} // namespace FlutterPlatform
