#include "flutter_platform.h"

namespace FlutterPlatform {

std::unique_ptr<FlutterEngineHost> createEngine() {
    return ::createFlutterEngine("", "");
}

static const Capabilities kCaps = {
    false, false, RendererBackend::RENDERER_SOFTWARE, PlatformId::PLATFORM_LINUX
};

const Capabilities& capabilities() { return kCaps; }

} // namespace FlutterPlatform
