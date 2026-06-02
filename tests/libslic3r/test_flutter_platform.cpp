#include <catch2/catch.hpp>
#include "Flutter/flutter_platform.h"

TEST_CASE("FlutterPlatform::capabilities returns correct platform values", "[FlutterPlatform]")
{
    auto& caps = FlutterPlatform::capabilities();
    REQUIRE(caps.supportsMultiView == false);
}

TEST_CASE("FlutterPlatform::capabilities returns identical reference", "[FlutterPlatform]")
{
    auto& c1 = FlutterPlatform::capabilities();
    auto& c2 = FlutterPlatform::capabilities();
    REQUIRE(&c1 == &c2);
}

TEST_CASE("FlutterPlatform::createEngine returns non-null engine", "[FlutterPlatform]")
{
    auto engine = FlutterPlatform::createEngine();
    REQUIRE(engine != nullptr);
}

TEST_CASE("FlutterViewHost::nativeHandle requires AOT data", "[FlutterPlatform][.aot]")
{
    auto engine = FlutterPlatform::createEngine();
    REQUIRE(engine != nullptr);
    engine->start();
    // On Windows, createView loads flutter_app.dll — returns nullptr without it
    auto view = engine->createView("", "test.channel");
    if (!view) {
        WARN("createView skipped — flutter_app.dll not found (expected in CI)");
        return;
    }
    void* handle = view->nativeHandle();
    REQUIRE(handle != nullptr);
    view->resize(800, 600);
}

TEST_CASE("Capabilities per-platform invariant checks", "[FlutterPlatform]")
{
    using namespace FlutterPlatform;
    auto& caps = FlutterPlatform::capabilities();
    if (caps.platformId == PlatformId::PLATFORM_WINDOWS)
        REQUIRE(caps.supportsCustomEntrypoint == true);
    else if (caps.platformId == PlatformId::PLATFORM_MACOS)
        REQUIRE(caps.supportsCustomEntrypoint == true);
    else if (caps.platformId == PlatformId::PLATFORM_LINUX)
        REQUIRE(caps.supportsCustomEntrypoint == false);
}
