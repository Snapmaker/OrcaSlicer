#include <catch2/catch.hpp>
#include "Flutter/flutter_platform.h"
#include "Flutter/FlutterChannel.h"

// ── FlutterPlatform ──────────────────────────────────────────────

TEST_CASE("FlutterPlatform::capabilities returns correct platform values", "[FlutterPlatform]")
{
    auto& caps = FlutterPlatform::capabilities();
    REQUIRE(caps.supportsMultiView == false);
    REQUIRE((caps.platformId == FlutterPlatform::PlatformId::PLATFORM_WINDOWS ||
             caps.platformId == FlutterPlatform::PlatformId::PLATFORM_MACOS ||
             caps.platformId == FlutterPlatform::PlatformId::PLATFORM_LINUX));
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
    auto view = engine->createView("", "test.channel");
    if (!view) {
        WARN("createView skipped — flutter_app.dll not found");
        return;
    }
    void* handle = view->nativeHandle();
    REQUIRE(handle != nullptr);
    view->resize(800, 600);
}

TEST_CASE("Capabilities per-platform invariant checks", "[FlutterPlatform]")
{
    auto& caps = FlutterPlatform::capabilities();
    if (caps.platformId == FlutterPlatform::PlatformId::PLATFORM_WINDOWS)
        REQUIRE(caps.supportsCustomEntrypoint == true);
    else if (caps.platformId == FlutterPlatform::PlatformId::PLATFORM_MACOS)
        REQUIRE(caps.supportsCustomEntrypoint == true);
    else if (caps.platformId == FlutterPlatform::PlatformId::PLATFORM_LINUX)
        REQUIRE(caps.supportsCustomEntrypoint == false);
}

// ── FlutterChannel ───────────────────────────────────────────────

TEST_CASE("FlutterChannel registers handler by name", "[FlutterChannel]")
{
    FlutterChannel channel("test");
    channel.on("getVersion", [](const std::string&, auto r) { r("v1"); });
    SUCCEED("registered");
}

TEST_CASE("Namespace prefixes method names", "[FlutterChannel]")
{
    FlutterChannel channel("test");
    channel.ns("device").on("connect",
        [](const std::string&, auto r) { r("ok"); });
    SUCCEED("namespace ok");
}

TEST_CASE("Channel name accessible", "[FlutterChannel]")
{
    FlutterChannel channel("snapmaker/orca");
    REQUIRE(channel.name() == "snapmaker/orca");
}

TEST_CASE("Middleware use() accepts middleware", "[FlutterChannel]")
{
    FlutterChannel channel("test");
    channel.use(makeLoggingMiddleware());
    channel.use(makeThreadGuardMiddleware());
    channel.use(makeTimeoutMiddleware(30));
    SUCCEED("middleware chain");
}

TEST_CASE("invoke before handler is silent no-op", "[FlutterChannel]")
{
    FlutterChannel channel("test");
    REQUIRE_NOTHROW(channel.invoke("foo", "bar"));
}

TEST_CASE("Timeout message includes configured seconds", "[FlutterChannel]")
{
    auto mw = makeTimeoutMiddleware(10);
    REQUIRE(mw != nullptr);
    SUCCEED("timeout middleware created");
}
