#pragma once
#include "flutter_host.h"
#include <string>
#include <memory>
#include <cstdint>

namespace FlutterPlatform {

std::unique_ptr<FlutterEngineHost> createEngine();

enum class RendererBackend : uint8_t { RENDERER_IMPELLER, RENDERER_METAL, RENDERER_SOFTWARE };
enum class PlatformId : uint8_t { PLATFORM_WINDOWS, PLATFORM_LINUX, PLATFORM_MACOS };

struct Capabilities
{
    bool            supportsCustomEntrypoint;
    bool            supportsMultiView;
    RendererBackend rendererBackend;
    PlatformId      platformId;
};
const Capabilities& capabilities();

} // namespace FlutterPlatform
