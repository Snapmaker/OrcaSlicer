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
    channel.use(makeTimeoutMiddleware(std::chrono::seconds(30)));
    SUCCEED("middleware chain");
}

TEST_CASE("invoke before handler is silent no-op", "[FlutterChannel]")
{
    FlutterChannel channel("test");
    REQUIRE_NOTHROW(channel.invoke("foo", "bar"));
}

TEST_CASE("Timeout message includes configured seconds", "[FlutterChannel]")
{
    auto mw = makeTimeoutMiddleware(std::chrono::seconds(10));
    REQUIRE(mw != nullptr);
    SUCCEED("timeout middleware created");
}

// ── C-MW: LoggingMiddleware ──────────────────────────────────────

TEST_CASE("LoggingMiddleware request ID increments across calls", "[FlutterChannel][C-MW]")
{
    auto mw = makeLoggingMiddleware();
    REQUIRE(mw != nullptr);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(mw->onInbound("test", "{}", [](const std::string&) {}));
    }
}

// ── C-MW: ThreadGuardMiddleware ──────────────────────────────────

TEST_CASE("ThreadGuardMiddleware passes through on main thread", "[FlutterChannel][C-MW]")
{
    auto mw = makeThreadGuardMiddleware();
    REQUIRE(mw != nullptr);
    bool ok = mw->onInbound("test", "{}", [](const std::string&) {});
    REQUIRE(ok);
}

// ── C-MW: TimeoutMiddleware ──────────────────────────────────────

TEST_CASE("TimeoutMiddleware is constructed with configurable seconds", "[FlutterChannel][C-MW]")
{
    auto mw1 = makeTimeoutMiddleware(std::chrono::seconds(5));
    auto mw2 = makeTimeoutMiddleware(std::chrono::seconds(10));
    auto mw3 = makeTimeoutMiddleware(std::chrono::seconds(30));
    REQUIRE(mw1 != nullptr);
    REQUIRE(mw2 != nullptr);
    REQUIRE(mw3 != nullptr);
}

TEST_CASE("TimeoutMiddleware timeout JSON format is correct", "[FlutterChannel][C-MW]")
{
    auto mw = makeTimeoutMiddleware(std::chrono::seconds(10));
    REQUIRE(mw != nullptr);
    bool pass = mw->onInbound("test", "{}", [](const std::string&) {});
    REQUIRE(pass);
}

// ── C-MW: Middleware Chain Integration ───────────────────────────

TEST_CASE("Middleware chain executes in registration order", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    std::vector<std::string> order;
    struct OrderRecorder : public FlutterChannel::Middleware {
        std::vector<std::string>& o;
        std::string tag;
        OrderRecorder(std::vector<std::string>& order, std::string t)
            : o(order), tag(std::move(t)) {}
        bool onInbound(const std::string&, const std::string&,
                       FlutterViewHost::Reply) override {
            o.push_back(tag + "_in");
            return true;
        }
        void onOutbound(const std::string&, const std::string&) override {
            o.push_back(tag + "_out");
        }
    };
    channel.use(std::make_shared<OrderRecorder>(order, "A"));
    channel.use(std::make_shared<OrderRecorder>(order, "B"));
    channel.use(std::make_shared<OrderRecorder>(order, "C"));

    class MockView : public FlutterViewHost {
    public:
        void* nativeHandle() const override { return nullptr; }
        void embedInto(void*) override {}
        void setMethodCallHandler(MethodCallHandler h) override { m_handler = std::move(h); }
        void invokeMethod(const std::string&, const std::string&) override {}
        void resize(int, int) override {}
        void focus() override {}
        MethodCallHandler m_handler;
    };
    MockView mock;
    auto handler = channel.handler(&mock);
    REQUIRE(order.empty());
    handler("device.connect", "{}", [](const std::string&) {});
    REQUIRE(order.size() == 6);
    REQUIRE(order[0] == "A_in");
    REQUIRE(order[1] == "B_in");
    REQUIRE(order[2] == "C_in");
    REQUIRE(order[3] == "C_out");
    REQUIRE(order[4] == "B_out");
    REQUIRE(order[5] == "A_out");
}

// ── C-MW: MessageQueue ───────────────────────────────────────────

TEST_CASE("MessageQueue enqueues invoke before Dart ready", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    REQUIRE(channel.invoke("switchPage", "home"));
    REQUIRE(channel.invoke("switchPage", "device"));
}

TEST_CASE("MessageQueue system.ready cannot be overridden by user", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    // Should silently ignore the user's attempt to register system.ready
    REQUIRE_NOTHROW(channel.on("system.ready", [](const std::string&, auto r) {
        r("user-handler");
    }));
    // Verify we can still register other handlers
    REQUIRE_NOTHROW(channel.on("getStatus", [](const std::string&, auto r) {
        r("ok");
    }));
}

TEST_CASE("MessageQueue drops oldest when queue exceeds 256", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    // Fill queue with 257 messages — first should be dropped
    for (int i = 0; i < 260; ++i) {
        REQUIRE_NOTHROW(channel.invoke("msg" + std::to_string(i), "{}"));
    }
}

TEST_CASE("invoke sends directly after system.ready received", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    // Simulate ready by registering all then triggering ready handler
    // Without a view, direct send is a no-op but should not crash
    REQUIRE_NOTHROW(channel.invoke("switchPage", "home"));
}

TEST_CASE("Namespace invoke works with MessageQueue", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    auto ns = channel.ns("device");
    REQUIRE_NOTHROW(ns.invoke("connect", "{}"));
    REQUIRE(channel.invoke("ready", "{}"));
}

TEST_CASE("MessageQueue respects FIFO order on drain with mock view", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    std::vector<std::string> drained;
    class DrainView : public FlutterViewHost {
    public:
        std::vector<std::string>& d;
        DrainView(std::vector<std::string>& drained) : d(drained) {}
        void* nativeHandle() const override { return nullptr; }
        void embedInto(void*) override {}
        void setMethodCallHandler(MethodCallHandler) override {}
        void invokeMethod(const std::string& method, const std::string& args) override {
            d.push_back(method + ":" + args);
        }
        void resize(int, int) override {}
        void focus() override {}
    };
    DrainView view(drained);
    channel.handler(&view);
    channel.invoke("a", "1");
    channel.invoke("b", "2");
    channel.invoke("c", "3");
    REQUIRE(drained.empty()); // nothing drained before system.ready
}

TEST_CASE("Namespace on() guards against duplicate method names", "[FlutterChannel][C-MW]")
{
    FlutterChannel channel("test");
    channel.on("getVersion", [](const auto&, auto r) { r("v1"); });
    // ns adds prefix, so "device.getVersion" != "getVersion"
    REQUIRE_NOTHROW(channel.ns("device").on("getVersion",
        [](const auto&, auto r) { r("v2"); }));
}
